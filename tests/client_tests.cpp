#include "exchange/client.h"
#include <QJsonDocument>
#include <QWebSocketServer>
#include <QtTest>

class ClientTests : public QObject
{
    Q_OBJECT
  private slots:

    void invalidParamsIdentifiesMethodAndParameter()
    {
        QWebSocketServer server("Invalid parameters", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Client client;
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        connect(&server, &QWebSocketServer::newConnection, this,
                [&]
                {
                    auto* peer = server.nextPendingConnection();
                    peer->setParent(&server);
                    connect(
                        peer, &QWebSocket::textMessageReceived, this,
                        [peer](const QString& frame)
                        {
                            const auto request = QJsonDocument::fromJson(frame.toUtf8()).object();
                            QJsonObject reply{{"id", request["id"]}};
                            if (request["method"] == "private/buy")
                                reply["error"] = QJsonObject{
                                    {"code", -32602},
                                    {"message", "Invalid params"},
                                    {"data", QJsonObject{{"param", "amount"},
                                                         {"value", "do-not-log-values"}}}};
                            else
                                reply["result"] = QJsonObject{{"expires_in", 3600}};
                            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(reply).toJson()));
                        });
                });
        client.connectTo("fake", "secret-not-for-logs", false,
                         QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        QString error;
        client.call("private/buy", {{"amount", .0086}},
                    [&](const RpcResult& result) { error = result.error; });
        QTRY_VERIFY(!error.isEmpty());
        QVERIFY(error.contains("private/buy"));
        QVERIFY(error.contains("-32602"));
        QVERIFY(error.contains("parameter: amount"));
        QVERIFY(!error.contains("do-not-log-values"));
        QVERIFY(!error.contains("secret-not-for-logs"));
    }

    void latencyTracksRepliesSlowWaitAndDisconnect()
    {
        QWebSocketServer server("Latency", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Client client;
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        QWebSocket* peer = nullptr;
        QJsonValue delayedId;
        const auto reply = [](QWebSocket* socket, const QJsonValue& id)
        {
            socket->sendTextMessage(QString::fromUtf8(
                QJsonDocument(
                    QJsonObject{{"id", id}, {"result", QJsonObject{{"expires_in", 3600}}}})
                    .toJson()));
        };
        connect(&server, &QWebSocketServer::newConnection, this,
                [&]
                {
                    peer = server.nextPendingConnection();
                    peer->setParent(&server);
                    connect(peer, &QWebSocket::textMessageReceived, this,
                            [&](const QString& frame)
                            {
                                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                                if (j["method"] == "read/delayed")
                                    delayedId = j["id"];
                                else if (j["method"] == "read/measured")
                                    QTimer::singleShot(200, peer,
                                                       [&, id = j["id"]] { reply(peer, id); });
                                else
                                    reply(peer, j["id"]);
                            });
                });
        using State = Client::ConnectionStatus::State;
        QCOMPARE(client.connectionStatus().state, State::Idle);
        client.connectTo("fake", "fake", false,
                         QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        bool measured = false;
        client.call("read/measured", {}, [&](const RpcResult&) { measured = true; });
        QTRY_VERIFY(measured);
        const auto normal = client.connectionStatus();
        QVERIFY(normal.milliseconds >= 150);
        QVERIFY(normal.milliseconds < 1000);
        QVERIFY(!normal.slow);
        bool delayed = false;
        client.call("read/delayed", {}, [&](const RpcResult&) { delayed = true; });
        QTRY_VERIFY(!delayedId.isUndefined());
        QTRY_VERIFY_WITH_TIMEOUT(client.connectionStatus().slow, 3000);
        QVERIFY(client.connectionStatus().waiting);
        QVERIFY(client.connectionStatus().milliseconds >= 2000);
        // Incoming notifications do not hide a stalled RPC.
        peer->sendTextMessage(
            "{\"method\":\"subscription\",\"params\":{\"channel\":\"ticker.test\",\"data\":{}}}");
        QTest::qWait(50);
        QVERIFY(client.connectionStatus().slow);
        reply(peer, delayedId);
        QTRY_VERIFY(delayed);
        QVERIFY(client.connectionStatus().slow);
        bool fast = false;
        client.call("read/fast", {}, [&](const RpcResult&) { fast = true; });
        QTRY_VERIFY(fast);
        QVERIFY(!client.connectionStatus().slow);
        QVERIFY(client.connectionStatus().milliseconds < 1000);
        peer->close();
        QTRY_COMPARE(client.connectionStatus().state, State::Offline);
        QVERIFY(client.connectionStatus().slow);
        QCOMPARE(client.connectionStatus().milliseconds, qint64(-1));
        client.stop();
        QCOMPARE(client.connectionStatus().state, State::Idle);
    }

    void latencyExcludesTradingPacingQueue()
    {
        QWebSocketServer server("Queued latency", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Client client;
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        connect(&server, &QWebSocketServer::newConnection, this,
                [&]
                {
                    auto* peer = server.nextPendingConnection();
                    peer->setParent(&server);
                    connect(peer, &QWebSocket::textMessageReceived, this,
                            [peer](const QString& frame)
                            {
                                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                                peer->sendTextMessage(QString::fromUtf8(
                                    QJsonDocument(
                                        QJsonObject{{"id", j["id"]},
                                                    {"result", QJsonObject{{"expires_in", 3600}}}})
                                        .toJson()));
                            });
                });
        client.connectTo("fake", "fake", false,
                         QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        client.setTradingLimits(
            {{"matching_engine",
              QJsonObject{{"trading", QJsonObject{{"total", QJsonObject{{"rate", 1}}}}}}}});
        int completed = 0;
        QElapsedTimer wait;
        wait.start();
        for (int i = 0; i < 3; ++i)
            client.call("private/buy", {}, [&](const RpcResult&) { ++completed; }, true);
        QTRY_COMPARE_WITH_TIMEOUT(completed, 3, 5000);
        QVERIFY(wait.elapsed() >=
                1800); // Third request has waited in the local queue, not on the network.
        QVERIFY(!client.connectionStatus().slow);
        QVERIFY(client.connectionStatus().milliseconds < 1000);
        QTRY_COMPARE_WITH_TIMEOUT(completed, 3, 3000);
        client.stop();
    }

    void rateLimitRejectionAndPacing_data()
    {
        QTest::addColumn<bool>("perCurrency");
        QTest::newRow("account") << false;
        QTest::newRow("USDC") << true;
    }

    void rateLimitRejectionAndPacing()
    {
        QFETCH(bool, perCurrency);
        QWebSocketServer server("Rate limit simulator", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Client client;
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        QSignalSpy pacing(&client, &Client::notice);
        QList<qint64> times;
        QElapsedTimer elapsed;
        elapsed.start();
        int completed = 0;
        bool uncertain = true;
        connect(&server, &QWebSocketServer::newConnection, this,
                [&]
                {
                    auto* peer = server.nextPendingConnection();
                    peer->setParent(&server);
                    connect(peer, &QWebSocket::textMessageReceived, this,
                            [&, peer](const QString& frame)
                            {
                                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                                const auto method = j["method"].toString();
                                QJsonObject response{{"jsonrpc", "2.0"}, {"id", j["id"]}};
                                if (method == "private/buy")
                                {
                                    times.append(elapsed.elapsed());
                                    response["error"] = QJsonObject{
                                        {"code", 10028}, {"message", "too_many_requests"}};
                                }
                                else
                                    response["result"] = QJsonObject{{"expires_in", 3600},
                                                                     {"refresh_token", "fake"}};
                                peer->sendTextMessage(
                                    QString::fromUtf8(QJsonDocument(response).toJson()));
                            });
                });
        client.connectTo("fake", "secret", false,
                         QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        const QJsonObject trading{{"trading", QJsonObject{{"total", QJsonObject{{"rate", 5}}}}}};
        client.setTradingLimits(
            {{"limits_per_currency", perCurrency},
             {"matching_engine", perCurrency ? QJsonObject{{"usdc", trading}} : trading}});
        QVERIFY(!pacing.isEmpty());
        QVERIFY(pacing.last().first().toString().contains("4.0 requests/s"));
        for (int i = 0; i < 10; ++i)
            client.call(
                "private/buy", {},
                [&](const RpcResult& r)
                {
                    uncertain = r.uncertain;
                    ++completed;
                    QVERIFY(!r.ok);
                },
                true);
        QTRY_COMPARE_WITH_TIMEOUT(completed, 10, 6000);
        QVERIFY(!uncertain);
        QCOMPARE(times.size(), 10);
        // Receive times include event-loop/network jitter. Nine 250 ms intervals should
        // span about 2250 ms; test sustained pacing, not individual packet-arrival gaps.
        const auto span = times.last() - times.first();
        QVERIFY2(span >= 2100 && span < 2750,
                 qPrintable(QString("Ten-request arrival span: %1 ms").arg(span)));
        client.stop();
    }

    void configuredTradingSpacingRespectsAccountLimits_data()
    {
        QTest::addColumn<int>("configured");
        QTest::addColumn<int>("accountRate");
        QTest::addColumn<bool>("beforeConnect");
        QTest::addColumn<int>("expected");
        QTest::newRow("saved-slower-spacing") << 600 << 5 << true << 600;
        QTest::newRow("account-limit-wins-after-edit") << 100 << 1 << false << 1250;
        QTest::newRow("saved-faster-spacing") << 100 << 100 << true << 250;
    }
    void configuredTradingSpacingRespectsAccountLimits()
    {
        QFETCH(int, configured);
        QFETCH(int, accountRate);
        QFETCH(bool, beforeConnect);
        QFETCH(int, expected);
        QWebSocketServer server("Configured pacing", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Client client;
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        QSignalSpy notices(&client, &Client::notice);
        QList<qint64> arrivals;
        QElapsedTimer clock;
        clock.start();
        connect(&server, &QWebSocketServer::newConnection, this,
                [&]
                {
                    auto* peer = server.nextPendingConnection();
                    peer->setParent(&server);
                    connect(peer, &QWebSocket::textMessageReceived, this,
                            [&, peer](const QString& frame)
                            {
                                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                                if (j["method"] == "private/buy")
                                    arrivals.append(clock.elapsed());
                                peer->sendTextMessage(QString::fromUtf8(
                                    QJsonDocument(QJsonObject{
                                        {"id", j["id"]},
                                        {"result", QJsonObject{{"expires_in", 3600}}}}).toJson()));
                            });
                });
        if (beforeConnect)
            client.setTradingIntervalMs(configured);
        client.connectTo("fake", "fake", false,
                         QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        client.setTradingLimits({{"matching_engine", QJsonObject{
            {"trading", QJsonObject{{"total", QJsonObject{{"rate", accountRate}}}}}}}});
        if (!beforeConnect)
            client.setTradingIntervalMs(configured);
        QVERIFY(notices.last().first().toString().contains(
            QString::number(1000.0 / expected, 'f', 1) + " requests/s"));
        int completed = 0;
        for (int i = 0; i < 3; ++i)
            client.call("private/buy", {}, [&](const RpcResult&) { ++completed; }, true);
        QTRY_COMPARE_WITH_TIMEOUT(completed, 3, 6000);
        QCOMPARE(arrivals.size(), 3);
        QVERIFY(arrivals.last() - arrivals.first() >= 2 * expected - 30);
    }
    void destructionWithPendingRequests()
    {
        QWebSocketServer server("Destruction simulator", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QWebSocket* peer = nullptr;
        connect(&server, &QWebSocketServer::newConnection, this,
                [&]
                {
                    peer = server.nextPendingConnection();
                    peer->setParent(&server);
                });
        bool callback = false;
        {
            Client client;
            client.connectTo("fake", "fake-secret", false,
                             QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
            QTRY_VERIFY(peer);
            client.call("pending/read", {}, [&](const RpcResult&) { callback = true; });
            // Deliberately destroy without an explicit stop and with a live socket/auth callback.
        }
        QTest::qWait(50);
        QVERIFY(!callback);
    }

    void correlationHeartbeatAndLostMutation()
    {
        QWebSocketServer server("Fake Deribit", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Client client;
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        QSignalSpy events(&client, &Client::subscription);
        QSignalSpy notices(&client, &Client::notice);
        QWebSocket* peer = nullptr;
        QList<QJsonObject> requests;
        bool authParametersOK = false;
        auto reply = [](QWebSocket* socket, qint64 id, const QJsonValue& result)
        {
            socket->sendTextMessage(QString::fromUtf8(
                QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}})
                    .toJson(QJsonDocument::Compact)));
        };
        connect(&server, &QWebSocketServer::newConnection, this,
                [&]
                {
                    peer = server.nextPendingConnection();
                    peer->setParent(&server);
                    connect(peer, &QWebSocket::textMessageReceived, this,
                            [&](const QString& frame)
                            {
                                const auto o = QJsonDocument::fromJson(frame.toUtf8()).object();
                                const auto method = o["method"].toString();
                                const qint64 id = o["id"].toInteger();
                                if (method == "public/auth")
                                {
                                    const auto p = o["params"].toObject();
                                    authParametersOK = p["client_id"] == "test-id" &&
                                                       p["client_secret"] == "test-secret" &&
                                                       p["grant_type"] == "client_credentials";
                                    reply(peer, id,
                                          QJsonObject{{"expires_in", 3600},
                                                      {"refresh_token", "test-refresh"}});
                                }
                                else if (method == "public/set_heartbeat" ||
                                         method == "public/test")
                                    reply(peer, id, QJsonObject{{"ok", true}});
                                else
                                    requests.append(o);
                            });
                });
        client.connectTo("test-id", "test-secret", false,
                         QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        QVERIFY(authParametersOK);
        int first = 0, second = 0;
        const auto a = client.call("read/first", {},
                                   [&](const RpcResult& r)
                                   {
                                       if (r.ok)
                                           first = r.value.toInt();
                                   });
        const auto b = client.call("read/second", {},
                                   [&](const RpcResult& r)
                                   {
                                       if (r.ok)
                                           second = r.value.toInt();
                                   });
        QTRY_COMPARE(requests.size(), 2);
        reply(peer, b, 22);
        reply(peer, a, 11);
        QTRY_COMPARE(first, 11);
        QTRY_COMPARE(second, 22);
        peer->sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"method", "subscription"},
                                      {"params", QJsonObject{{"channel", "ticker.test.100ms"},
                                                             {"data", QJsonObject{{"price", 1}}}}}})
                .toJson()));
        QTRY_COMPARE(events.size(), 1);
        int calls = 0;
        bool uncertain = false;
        client.call(
            "private/buy", {{"type", "limit"}},
            [&](const RpcResult& r)
            {
                ++calls;
                uncertain = r.uncertain;
            },
            true);
        QTRY_COMPARE(requests.size(), 3);
        peer->close();
        QTRY_COMPARE(calls, 1);
        QVERIFY(uncertain);
        client.stop();
        for (const auto& row : notices)
            QVERIFY(!row[0].toString().contains("test-secret"));
    }
};
QTEST_GUILESS_MAIN(ClientTests)
#include "client_tests.moc"
