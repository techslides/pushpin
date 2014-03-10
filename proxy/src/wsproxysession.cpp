/*
 * Copyright (C) 2014 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "wsproxysession.h"

#include <assert.h>
#include <QUrl>
#include <QHostAddress>
#include <qjson/serializer.h>
#include "packet/httprequestdata.h"
#include "log.h"
#include "zhttpmanager.h"
#include "zwebsocket.h"
#include "domainmap.h"
#include "wscontrolmanager.h"
#include "wscontrolsession.h"
#include "xffrule.h"
#include "proxyutil.h"

#define PENDING_FRAMES_MAX 100

class HttpExtension
{
public:
	bool isNull() const { return name.isEmpty(); }

	QByteArray name;
	QHash<QByteArray, QByteArray> params;
};

static int findNext(const QByteArray &in, const char *charList, int start = 0)
{
	int len = qstrlen(charList);
	for(int n = start; n < in.size(); ++n)
	{
		char c = in[n];
		for(int i = 0; i < len; ++i)
		{
			if(c == charList[i])
				return n;
		}
	}

	return -1;
}

static QHash<QByteArray, QByteArray> parseParams(const QByteArray &in, bool *ok = 0)
{
	QHash<QByteArray, QByteArray> out;

	int start = 0;
	while(start < in.size())
	{
		QByteArray var;
		QByteArray val;

		int at = findNext(in, "=;", start);
		if(at != -1)
		{
			var = in.mid(start, at - start).trimmed();
			if(in[at] == '=')
			{
				if(at + 1 >= in.size())
				{
					if(ok)
						*ok = false;
					return QHash<QByteArray, QByteArray>();
				}

				++at;

				if(in[at] == '\"')
				{
					++at;

					bool complete = false;
					for(int n = at; n < in.size(); ++n)
					{
						if(in[n] == '\\')
						{
							if(n + 1 >= in.size())
							{
								if(ok)
									*ok = false;
								return QHash<QByteArray, QByteArray>();
							}

							++n;
							val += in[n];
						}
						else if(in[n] == '\"')
						{
							complete = true;
							at = n + 1;
							break;
						}
						else
							val += in[n];
					}

					if(!complete)
					{
						if(ok)
							*ok = false;
						return QHash<QByteArray, QByteArray>();
					}

					at = in.indexOf(';', at);
					if(at != -1)
						start = at + 1;
					else
						start = in.size();
				}
				else
				{
					int vstart = at;
					at = in.indexOf(';', vstart);
					if(at != -1)
					{
						val = in.mid(vstart, at - vstart).trimmed();
						start = at + 1;
					}
					else
					{
						val = in.mid(vstart).trimmed();
						start = in.size();
					}
				}
			}
			else
				start = at + 1;
		}
		else
		{
			var = in.mid(start).trimmed();
			start = in.size();
		}

		out[var] = val;
	}

	if(ok)
		*ok = true;

	return out;
}

static HttpExtension getExtension(const QList<QByteArray> &extStrings, const QByteArray &name)
{
	foreach(const QByteArray &ext, extStrings)
	{
		bool found = false;
		int at = ext.indexOf(';');
		if(at != -1)
		{
			if(ext.mid(0, at).trimmed() == name)
				found = true;
		}
		else
		{
			if(ext == name)
				found = true;
		}

		if(found)
		{
			HttpExtension e;
			e.name = name;

			if(at != -1)
			{
				bool ok;
				e.params = parseParams(ext.mid(at + 1), &ok);
				if(!ok)
					return HttpExtension();
			}

			return e;
		}
	}

	return HttpExtension();
}

class WsProxySession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Idle,
		Connecting,
		Connected,
		Closing
	};

	WsProxySession *q;
	State state;
	ZhttpManager *zhttpManager;
	DomainMap *domainMap;
	WsControlManager *wsControlManager;
	WsControlSession *wsControl;
	QByteArray defaultSigIss;
	QByteArray defaultSigKey;
	QByteArray defaultUpstreamKey;
	bool passToUpstream;
	bool useXForwardedProtocol;
	XffRule xffRule;
	XffRule xffTrustedRule;
	QList<QByteArray> origHeadersNeedMark;
	HttpRequestData requestData;
	ZWebSocket *inSock;
	ZWebSocket *outSock;
	int inPending;
	int outPending;
	int outReadInProgress; // frame type or -1
	QByteArray channelPrefix;
	QList<DomainMap::Target> targets;
	QByteArray messagePrefix;
	bool detached;
	QString subChannel;

	Private(WsProxySession *_q, ZhttpManager *_zhttpManager, DomainMap *_domainMap, WsControlManager *_wsControlManager) :
		QObject(_q),
		q(_q),
		state(Idle),
		zhttpManager(_zhttpManager),
		domainMap(_domainMap),
		wsControlManager(_wsControlManager),
		wsControl(0),
		passToUpstream(false),
		useXForwardedProtocol(false),
		inSock(0),
		outSock(0),
		inPending(0),
		outPending(0),
		outReadInProgress(-1),
		detached(false)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		delete inSock;
		inSock = 0;

		delete outSock;
		outSock = 0;

		delete wsControl;
		wsControl = 0;
	}

	void start(ZWebSocket *sock)
	{
		assert(!inSock);

		state = Connecting;

		inSock = sock;
		inSock->setParent(this);
		connect(inSock, SIGNAL(readyRead()), SLOT(in_readyRead()));
		connect(inSock, SIGNAL(framesWritten(int)), SLOT(in_framesWritten(int)));
		connect(inSock, SIGNAL(peerClosed()), SLOT(in_peerClosed()));
		connect(inSock, SIGNAL(closed()), SLOT(in_closed()));
		connect(inSock, SIGNAL(error()), SLOT(in_error()));

		requestData.uri = inSock->requestUri();
		requestData.headers = inSock->requestHeaders();

		QString host = requestData.uri.host();
		bool isSecure = (requestData.uri.scheme() == "wss");

		DomainMap::Entry entry = domainMap->entry(DomainMap::WebSocket, isSecure, host, requestData.uri.encodedPath());
		if(entry.isNull())
		{
			log_warning("wsproxysession: %p %s has 0 routes", q, qPrintable(host));
			reject(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		QByteArray sigIss;
		QByteArray sigKey;
		if(!entry.sigIss.isEmpty() && !entry.sigKey.isEmpty())
		{
			sigIss = entry.sigIss;
			sigKey = entry.sigKey;
		}
		else
		{
			sigIss = defaultSigIss;
			sigKey = defaultSigKey;
		}

		channelPrefix = entry.prefix;
		targets = entry.targets;

		log_debug("wsproxysession: %p %s has %d routes", q, qPrintable(host), targets.count());

		bool trustedClient = ProxyUtil::manipulateRequestHeaders("wsproxysession", q, &requestData, defaultUpstreamKey, entry, sigIss, sigKey, useXForwardedProtocol, xffTrustedRule, xffRule, origHeadersNeedMark, inSock->peerAddress());

		// don't proxy extensions, as we may not know how to handle them
		requestData.headers.removeAll("Sec-WebSocket-Extensions");

		// send grip extension
		requestData.headers += HttpHeader("Sec-WebSocket-Extensions", "grip");

		if(trustedClient)
			passToUpstream = true;

		tryNextTarget();
	}

	void tryNextTarget()
	{
		if(targets.isEmpty())
		{
			reject(502, "Bad Gateway", "Error while proxying to origin.");
			return;
		}

		DomainMap::Target target = targets.takeFirst();

		QUrl uri = requestData.uri;
		if(target.ssl)
			uri.setScheme("wss");
		else
			uri.setScheme("ws");

		if(!target.host.isEmpty())
			uri.setHost(target.host);

		subChannel = target.subChannel;

		log_debug("wsproxysession: %p forwarding to %s:%d", q, qPrintable(target.connectHost), target.connectPort);

		outSock = zhttpManager->createSocket();
		outSock->setParent(this);
		connect(outSock, SIGNAL(connected()), SLOT(out_connected()));
		connect(outSock, SIGNAL(readyRead()), SLOT(out_readyRead()));
		connect(outSock, SIGNAL(framesWritten(int)), SLOT(out_framesWritten(int)));
		connect(outSock, SIGNAL(peerClosed()), SLOT(out_peerClosed()));
		connect(outSock, SIGNAL(closed()), SLOT(out_closed()));
		connect(outSock, SIGNAL(error()), SLOT(out_error()));

		if(target.trusted)
			outSock->setIgnorePolicies(true);

		if(target.insecure)
			outSock->setIgnoreTlsErrors(true);

		outSock->setConnectHost(target.connectHost);
		outSock->setConnectPort(target.connectPort);

		outSock->start(uri, requestData.headers);
	}

	void reject(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		assert(state == Connecting);

		state = Closing;
		inSock->respondError(code, reason, headers, body);
	}

	void reject(int code, const QString &reason, const QString &errorMessage)
	{
		reject(code, reason.toUtf8(), HttpHeaders(), (errorMessage + '\n').toUtf8());
	}

	void tryReadIn()
	{
		while(inSock->framesAvailable() > 0 && outPending < PENDING_FRAMES_MAX)
		{
			ZWebSocket::Frame f = inSock->readFrame();

			if(detached)
				continue;

			outSock->writeFrame(f);
			++outPending;
		}
	}

	void tryReadOut()
	{
		while(outSock->framesAvailable() > 0 && inPending < PENDING_FRAMES_MAX)
		{
			ZWebSocket::Frame f = outSock->readFrame();

			if(detached)
				continue;

			if(f.type == ZWebSocket::Frame::Text || f.type == ZWebSocket::Frame::Binary || f.type == ZWebSocket::Frame::Continuation)
			{
				// we are skipping the rest of this message
				if(f.type == ZWebSocket::Frame::Continuation && outReadInProgress == -1)
					continue;

				if(f.type != ZWebSocket::Frame::Continuation)
					outReadInProgress = (int)f.type;

				if(wsControl)
				{
					if(f.type == ZWebSocket::Frame::Text && f.data.startsWith("c:"))
					{
						// grip messages must only be one frame
						if(!f.more)
							wsControl->sendGripMessage(f.data.mid(2)); // process
						else
							outReadInProgress = -1; // ignore rest of message
					}
					else if(f.type != ZWebSocket::Frame::Continuation && f.data.startsWith(messagePrefix))
					{
						inSock->writeFrame(f);
						++inPending;
					}
					else if(f.type == ZWebSocket::Frame::Continuation)
					{
						assert(outReadInProgress != -1);

						inSock->writeFrame(f);
						++inPending;
					}
				}
				else
				{
					inSock->writeFrame(f);
					++inPending;
				}

				if(!f.more)
					outReadInProgress = -1;
			}
			else
			{
				// always relay non-content frames
				inSock->writeFrame(f);
				++inPending;
			}
		}
	}

	void tryFinish()
	{
		if(!inSock && !outSock)
		{
			cleanup();
			emit q->finishedByPassthrough();
		}
	}

private slots:
	void in_readyRead()
	{
		if(!detached && outSock && outSock->state() == ZWebSocket::Connected)
			tryReadIn();
	}

	void in_framesWritten(int count)
	{
		inPending -= count;

		if(!detached)
			tryReadOut();
	}

	void in_peerClosed()
	{
		if(!detached && outSock && outSock->state() != ZWebSocket::Closing)
			outSock->close();
	}

	void in_closed()
	{
		delete inSock;
		inSock = 0;

		if(!detached && outSock && outSock->state() != ZWebSocket::Closing)
			outSock->close();

		tryFinish();
	}

	void in_error()
	{
		delete inSock;
		inSock = 0;

		if(!detached)
		{
			delete outSock;
			outSock = 0;
		}

		tryFinish();
	}

	void out_connected()
	{
		log_debug("wsproxysession: %p connected", q);

		state = Connected;

		HttpHeaders headers = outSock->responseHeaders();

		// don't proxy extensions, as we may not know how to handle them
		QList<QByteArray> wsExtensions = headers.takeAll("Sec-WebSocket-Extensions");

		HttpExtension grip = getExtension(wsExtensions, "grip");
		if(!grip.isNull() || !subChannel.isEmpty())
		{
			if(!grip.isNull())
			{
				if(grip.params.contains("message-prefix"))
					messagePrefix = grip.params.value("message-prefix");
				else
					messagePrefix = "m:";
			}

			log_debug("grip enabled, message-prefix=[%s]", messagePrefix.data());

			if(wsControlManager)
			{
				wsControl = wsControlManager->createSession();
				connect(wsControl, SIGNAL(sendEventReceived(const QByteArray &, const QByteArray &)), SLOT(wsControl_sendEventReceived(const QByteArray &, const QByteArray &)));
				connect(wsControl, SIGNAL(detachEventReceived()), SLOT(wsControl_detachEventReceived()));
				wsControl->start();

				if(!subChannel.isEmpty())
				{
					log_debug("forcing subscription to [%s]", qPrintable(subChannel));

					QJson::Serializer serializer;
					QVariantMap msg;
					msg["type"] = "subscribe";
					msg["channel"] = subChannel;
					wsControl->sendGripMessage(serializer.serialize(msg));
				}
			}
		}

		inSock->respondSuccess(outSock->responseReason(), headers);

		// send any pending frames
		tryReadIn();
	}

	void out_readyRead()
	{
		tryReadOut();
	}

	void out_framesWritten(int count)
	{
		outPending -= count;

		if(!detached)
			tryReadIn();
	}

	void out_peerClosed()
	{
		if(!detached && inSock && inSock->state() != ZWebSocket::Closing)
			inSock->close();
	}

	void out_closed()
	{
		delete outSock;
		outSock = 0;

		if(!detached && inSock && inSock->state() != ZWebSocket::Closing)
			inSock->close();

		tryFinish();
	}

	void out_error()
	{
		ZWebSocket::ErrorCondition e = outSock->errorCondition();
		log_debug("wsproxysession: %p target error state=%d, condition=%d", q, (int)state, (int)e);

		if(detached)
		{
			delete outSock;
			outSock = 0;

			tryFinish();
			return;
		}

		if(state == Connecting)
		{
			bool tryAgain = false;

			switch(e)
			{
				case ZWebSocket::ErrorConnect:
				case ZWebSocket::ErrorConnectTimeout:
				case ZWebSocket::ErrorTls:
					tryAgain = true;
					break;
				case ZWebSocket::ErrorRejected:
					reject(outSock->responseCode(), outSock->responseReason(), outSock->responseHeaders(), outSock->responseBody());
					break;
				default:
					reject(502, "Bad Gateway", "Error while proxying to origin.");
					break;
			}

			delete outSock;
			outSock = 0;

			if(tryAgain)
				tryNextTarget();
		}
		else
		{
			delete inSock;
			inSock = 0;
			delete outSock;
			outSock = 0;

			tryFinish();
		}
	}

	void wsControl_sendEventReceived(const QByteArray &contentType, const QByteArray &message)
	{
		if(inSock && inSock->state() != ZWebSocket::Closing)
		{
			if(contentType == "binary")
				inSock->writeFrame(ZWebSocket::Frame(ZWebSocket::Frame::Binary, message, false));
			else
				inSock->writeFrame(ZWebSocket::Frame(ZWebSocket::Frame::Text, message, false));

			++inPending;
		}
	}

	void wsControl_detachEventReceived()
	{
		// if already detached, do nothing
		if(detached)
			return;

		detached = true;

		if(outSock && outSock->state() != ZWebSocket::Closing)
			outSock->close();
	}
};

WsProxySession::WsProxySession(ZhttpManager *zhttpManager, DomainMap *domainMap, WsControlManager *wsControlManager, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, zhttpManager, domainMap, wsControlManager);
}

WsProxySession::~WsProxySession()
{
	delete d;
}

void WsProxySession::setDefaultSigKey(const QByteArray &iss, const QByteArray &key)
{
	d->defaultSigIss = iss;
	d->defaultSigKey = key;
}

void WsProxySession::setDefaultUpstreamKey(const QByteArray &key)
{
	d->defaultUpstreamKey = key;
}

void WsProxySession::setUseXForwardedProtocol(bool enabled)
{
	d->useXForwardedProtocol = enabled;
}

void WsProxySession::setXffRules(const XffRule &untrusted, const XffRule &trusted)
{
	d->xffRule = untrusted;
	d->xffTrustedRule = trusted;
}

void WsProxySession::setOrigHeadersNeedMark(const QList<QByteArray> &names)
{
	d->origHeadersNeedMark = names;
}

void WsProxySession::start(ZWebSocket *sock)
{
	d->start(sock);
}

#include "wsproxysession.moc"