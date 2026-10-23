#include "exchange/client.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSslError>
#include <algorithm>
#include <cmath>
using namespace scalper;

Client::Client(QObject* parent) : QObject(parent)
{
    clock.start();
    retry.setSingleShot(true);
    refresh.setSingleShot(true);
    watchdog.setInterval(Time::SecondMs);
    sendTimer.setSingleShot(true);
    connect(&sendTimer, &QTimer::timeout, this, &Client::sendNext);
    connect(&retry, &QTimer::timeout, this,
            [this]
            {
                if (wanted)
                {
                    connectionStarted = clock.elapsed();
                    socket.open(endpoint);
                }
            });
    connect(&refresh, &QTimer::timeout, this, [this] { authenticate(true); });
    connect(&socket, &QWebSocket::connected, this,
            [this]
            {
                lastMessage = clock.elapsed();
                connectionFailed = false;
                authenticate(false);
            });
    connect(&socket, &QWebSocket::disconnected, this, &Client::disconnected);
    connect(&socket, &QWebSocket::textMessageReceived, this, &Client::receive);
    connect(&socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError)
            {
                connectionFailed = true;
                lastLatency = -1;
                emit notice("WebSocket: " + socket.errorString());
                if (wanted && socket.state() == QAbstractSocket::UnconnectedState &&
                    !retry.isActive())
                    retry.start(retryMs);
            });
    connect(&socket, &QWebSocket::sslErrors, this,
            [this](const QList<QSslError>&)
            {
                emit notice("TLS certificate verification failed; connection refused");
                socket.abort();
            });
    connect(&watchdog, &QTimer::timeout, this,
            [this]
            {
                const auto now = clock.elapsed();
                bool expired = false;
                for (const auto& [id, p] : pending)
                {
                    Q_UNUSED(id);
                    if (now > p.deadline)
                        expired = true;
                }
                if (expired || (authorized && now - lastMessage > Exchange::HeartbeatTimeoutMs))
                {
                    emit notice("Connection timed out; reconciling before any retry");
                    socket.abort();
                }
            });
    watchdog.start();
}

Client::~Client()
{
    // The socket is destroyed after the callback containers. Shut it down while they still exist.
    QObject::disconnect(&socket, nullptr, this, nullptr);
    stop();
}

void Client::connectTo(const QString& id, const QString& secret, bool production,
                       const QUrl& testEndpoint)
{
    stop();
    clientId = id;
    clientSecret = secret;
    wanted = true;
    retryMs = InitialRetryMs;
    connectionStarted = clock.elapsed();
    mutationInterval = MinimumTradingIntervalMs;
    endpoint = testEndpoint.isEmpty()
                   ? QUrl(production ? Exchange::ProductionUrl : Exchange::TestnetUrl)
                   : testEndpoint;
    socket.open(endpoint);
}

void Client::stop()
{
    wanted = false;
    authorized = false;
    retry.stop();
    refresh.stop();
    sendTimer.stop();
    outbound.clear();
    pending.clear();
    socket.abort();
    clientSecret.clear();
    refreshToken.clear();
    connectionFailed = false;
    lastLatency = -1;
    lastQueueDelay = -1;
}

Client::ConnectionStatus Client::connectionStatus() const
{
    ConnectionStatus status;
    status.queueMilliseconds = lastQueueDelay;
    const qint64 now = clock.elapsed();
    if (socket.state() != QAbstractSocket::ConnectedState)
    {
        if (!wanted)
        {
            status.state =
                connectionFailed ? ConnectionStatus::State::Offline : ConnectionStatus::State::Idle;
            return status;
        }
        if (socket.state() == QAbstractSocket::UnconnectedState)
        {
            status.state = ConnectionStatus::State::Offline;
            status.slow = true;
            return status;
        }
        status.state = ConnectionStatus::State::Connecting;
        status.milliseconds = now - connectionStarted;
        status.waiting = true;
        status.slow = connectionFailed || status.milliseconds >= Exchange::SlowResponseMs;
        return status;
    }
    status.state = ConnectionStatus::State::Online;
    status.milliseconds = lastLatency;
    qint64 waiting = -1;
    for (const auto& [id, p] : pending)
    {
        Q_UNUSED(id);
        if (p.sentAt >= 0)
            waiting = std::max(waiting, now - p.sentAt);
    }
    if (waiting >= Exchange::SlowResponseMs)
    {
        status.milliseconds = waiting;
        status.waiting = true;
    }
    status.slow = connectionFailed || status.milliseconds >= Exchange::SlowResponseMs ||
                  now - lastMessage >= Exchange::SlowFeedMs;
    return status;
}

void Client::reconnect()
{
    if (wanted && socket.state() != QAbstractSocket::UnconnectedState)
        socket.abort();
}

qint64 Client::call(const QString& method, const QJsonObject& params, Callback callback,
                    bool mutation, bool priority, std::function<bool()> beforeSend)
{
    const auto id = ++sequence;
    if (socket.state() != QAbstractSocket::ConnectedState)
    {
        QTimer::singleShot(0, this, [callback] { callback({false, {}, "Not connected", false}); });
        return id;
    }
    pending.emplace(id, Pending{std::move(callback),
                                clock.elapsed() + Exchange::RequestTimeoutMs +
                                    static_cast<qint64>(outbound.size() + 1) * mutationInterval,
                                mutation, -1, std::move(beforeSend), clock.elapsed(), method});
    const QJsonObject message{
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    if (mutation)
    {
        auto position =
            std::find_if(outbound.begin(), outbound.end(),
                         [&](const auto& item)
                         {
                             auto request = pending.find(item.first);
                             return request == pending.end() || !request->second.mutation;
                         });
        outbound.insert(position, {id, message});
    }
    else if (priority)
        outbound.emplace_front(id, message);
    else
        outbound.emplace_back(id, message);
    if (!sendTimer.isActive())
        sendTimer.start(static_cast<int>(
            std::max<qint64>(0, Exchange::ReadIntervalMs - (clock.elapsed() - lastSent))));
    return id;
}

bool Client::cancelQueued(qint64 id)
{
    auto item = std::find_if(outbound.begin(), outbound.end(),
                             [id](const auto& p) { return p.first == id; });
    auto request = pending.find(id);
    if (item == outbound.end() || request == pending.end() || request->second.sentAt >= 0)
        return false;
    outbound.erase(item);
    auto callback = std::move(request->second.callback);
    pending.erase(request);
    callback({false, {}, "Request cancelled before send", false});
    return true;
}

void Client::sendNext()
{
    if (outbound.empty() || socket.state() != QAbstractSocket::ConnectedState)
        return;
    const auto queued = pending.find(outbound.front().first);
    const bool mutation = queued != pending.end() && queued->second.mutation;
    if (mutation && std::any_of(pending.begin(), pending.end(),
                                [&](const auto& p)
                                {
                                    return p.first != outbound.front().first && p.second.mutation &&
                                           p.second.sentAt >= 0;
                                }))
    {
        sendTimer.start(Time::TradingPumpIntervalMs);
        return;
    }
    const qint64 now = clock.elapsed();
    const qint64 delay = std::max<qint64>(Exchange::ReadIntervalMs - (now - lastSent),
                                          mutation ? mutationInterval - (now - lastMutation) : 0);
    if (delay > 0)
    {
        sendTimer.start(static_cast<int>(delay));
        return;
    }
    const auto [id, message] = outbound.front();
    outbound.pop_front();
    if (auto it = pending.find(id);
        it != pending.end() && it->second.beforeSend && !it->second.beforeSend())
    {
        auto callback = std::move(it->second.callback);
        pending.erase(it);
        callback({false, {}, "Request cancelled before send", false});
        if (!outbound.empty())
            sendTimer.start(0);
        return;
    }
    lastSent = clock.elapsed();
    if (mutation)
        lastMutation = lastSent;
    if (auto it = pending.find(id); it != pending.end())
    {
        it->second.deadline = lastSent + Exchange::RequestTimeoutMs;
        it->second.sentAt = lastSent;
        lastQueueDelay = lastSent - it->second.queuedAt;
    }
    // Read RPCs stay at ten per second; mutations additionally respect the account's
    // sustained limit with headroom. Immediate continuation still avoids a pump-timer gap.
    socket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    if (!outbound.empty())
        sendTimer.start(Exchange::ReadIntervalMs);
}

void Client::setTradingLimits(const QJsonObject& limits)
{
    auto matching = limits["matching_engine"].toObject();
    if (limits["limits_per_currency"].toBool())
        matching = matching["usdc"].toObject();
    const double rate = matching["trading"].toObject()["total"].toObject()["rate"].toDouble();
    mutationInterval =
        std::isfinite(rate) && rate >= 1
            ? static_cast<int>(std::max<double>(MinimumTradingIntervalMs,
                                                std::ceil(Exchange::AccountRateHeadroomMs / rate)))
            : MinimumTradingIntervalMs;
    emit notice(QString("Trading RPC pacing: at most %1 requests/s in this window; account limits "
                        "are shared with other clients")
                    .arg(double(Time::SecondMs) / mutationInterval, 0, 'f', 1));
}

void Client::authenticate(bool renew)
{
    QJsonObject params =
        renew && !refreshToken.isEmpty()
            ? QJsonObject{{"grant_type", "refresh_token"}, {"refresh_token", refreshToken}}
            : QJsonObject{{"grant_type", "client_credentials"},
                          {"client_id", clientId},
                          {"client_secret", clientSecret}};
    call("public/auth", params,
         [this, renew](const RpcResult& r) { receiveAuthentication(r, renew); });
}

void Client::receiveAuthentication(const RpcResult& r, bool renew)
{
    if (!r.ok)
    {
        if (r.retryable())
        {
            emit notice("Authentication temporarily unavailable; reconnecting automatically");
            socket.abort();
            return;
        }
        if (renew)
        {
            emit notice("Token refresh failed; reconnecting with the API credentials");
            socket.abort();
            return;
        }
        // Authentication failures never log request parameters or secrets.
        emit notice("Authentication failed; check API key permissions and environment");
        wanted = false;
        socket.abort();
        return;
    }
    auto auth = r.value.toObject();
    authorized = true;
    refreshToken = auth["refresh_token"].toString();
    retryMs = InitialRetryMs;
    const int seconds = std::clamp(auth["expires_in"].toInt(3600) - 60, 10, 21600);
    refresh.start(seconds * 1000);
    if (renew)
        return;
    enableHeartbeat(auth);
}

void Client::enableHeartbeat(const QJsonObject& auth)
{
    call("public/set_heartbeat", {{"interval", Exchange::HeartbeatIntervalSeconds}},
         [this, auth](const RpcResult& h)
         {
             if (!h.ok)
             {
                 emit notice("Cannot enable heartbeat");
                 socket.abort();
                 return;
             }
             emit authenticatedSession(auth);
         });
}

void Client::receive(const QString& message)
{
    lastMessage = clock.elapsed();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(message.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        emit notice("Invalid JSON-RPC frame; reconnecting");
        socket.abort();
        return;
    }
    const auto o = document.object();
    if (o.contains("id"))
    {
        const qint64 id = o["id"].toInteger();
        auto it = pending.find(id);
        if (it == pending.end())
            return;
        auto p = std::move(it->second);
        pending.erase(it);
        if (p.sentAt >= 0 && (o.contains("result") || o.contains("error")))
            lastLatency = clock.elapsed() - p.sentAt;
        if (o.contains("error"))
        {
            const auto e = o["error"].toObject();
            QString text = QString("%1: API error %2: %3")
                               .arg(p.method)
                               .arg(e["code"].toInt())
                               .arg(e["message"].toString());
            // Include the parameter name, never request values or an echoed auth payload.
            const QString parameter = e["data"].toObject()["param"].toString();
            if (p.method != "public/auth" && !parameter.isEmpty() && parameter.size() <= 64 &&
                std::all_of(parameter.begin(), parameter.end(),
                            [](QChar c) { return c.isLetterOrNumber() || c == '_'; }))
                text += " (parameter: " + parameter + ")";
            const int code = e["code"].toInt();
            // 10028 explicitly rejects this request; only a missing response or
            // matching-engine timeout leaves its outcome uncertain.
            const bool ambiguous = p.mutation && code == 10050;
            if (code == 10028)
                emit notice(text + "; exchange rate limit reached");
            p.callback({false, {}, text, ambiguous, code});
        }
        else if (o.contains("result"))
            p.callback({true, o["result"], {}, false});
        else
        {
            p.callback({false, {}, "Invalid JSON-RPC response", p.mutation});
            socket.abort();
        }
    }
    else if (o["method"].toString() == "heartbeat")
    {
        if (o["params"].toObject()["type"].toString() == "test_request")
            call("public/test", {}, [](const RpcResult&) {});
    }
    else if (o["method"].toString() == "subscription")
    {
        const auto p = o["params"].toObject();
        emit subscription(p["channel"].toString(), p["data"]);
    }
}

void Client::disconnected()
{
    connectionFailed = wanted;
    lastLatency = -1;
    authorized = false;
    refresh.stop();
    sendTimer.stop();
    outbound.clear();
    auto old = std::move(pending);
    pending.clear();
    emit lost();
    for (auto& [id, p] : old)
    {
        Q_UNUSED(id);
        p.callback({false, {}, "Connection lost", p.mutation});
    }
    if (wanted && !retry.isActive())
    {
        retry.start(retryMs);
        retryMs = std::min(Exchange::MaxRetryMs, retryMs * 2);
    }
}
