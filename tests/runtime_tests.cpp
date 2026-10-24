#include "fixtures.h"
#include "ui/chart_scale.h"
#include "ui/format.h"
#include "ui/theme.h"
#include "ui/window.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QSemaphore>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QWebSocketServer>
#include <QtTest>
#include <atomic>
#include <utility>
using namespace scalper;
class FakeDeribit : public QObject
{
    Q_OBJECT
  public:
    QWebSocketServer* server = nullptr;
    QWebSocket* peer = nullptr;
    QVector<QWebSocket*> peers;
    QTimer* feed = nullptr;
    QHash<QString, QJsonObject> byLabel;
    QHash<QString, QJsonArray> fills;
    QString corePath;
    std::atomic<int> mutations{0};
    std::atomic<int> historyPages{0};
    std::atomic<int> optionTickerRequests{0}, positionRequests{0};
    std::atomic<int> invalidAmounts{0};
    QJsonObject lastSubmitted;
    std::atomic<bool> durable{true};
    bool holdMaker = false, dropReply = false;
    bool dropBeforeCreate = false;
    int rejectRecoveryCancel = 0, rejectRecoveryHistory = 0;
    bool fillOnCancel = false, fillBeforePositionReply = false, rejectNextEdit = false,
         partialIoc = false;
    double quoteShift = 0, spreadExtra = 0;
    int fillSequence = 0;
    QVector<QJsonObject> submitted;
    bool rejectProfileCancel = false, lateProfileOrder = false;
    int resetOrderSnapshots = 0;
    QStringList cancelledLabels;
    bool paginateHistory = false;
    bool optionNotifications = true, overrideOptionPositions = false, staleOptionSnapshots = false;
    bool perpetualNotifications = true, stalePerpetualSnapshots = false;
    bool invalidBid = false, invalidGreeks = false;
    int rejectNextCreateCode = 0;
    int optionSnapshotDelayMs = 0, failPositionReads = 0, clockLeadMs = 0;
    QJsonArray optionPositions;
    QHash<QString, Instrument> optionSpecs;
    double position = 0, delta = .5, lastEditAmount = 0;
    QList<double> positionReplies;
    QList<qint64> positionReadTimes;
    int next = 0;
    quint16 port = 0;
    void initialize()
    {
        server = new QWebSocketServer("Fake Deribit", QWebSocketServer::NonSecureMode, this);
        server->listen(QHostAddress::LocalHost, 0);
        port = server->serverPort();
        connect(server, &QWebSocketServer::newConnection, this,
                [this]
                {
                    auto* socket = server->nextPendingConnection();
                    peer = socket;
                    peers.append(socket);
                    socket->setParent(this);
                    connect(socket, &QWebSocket::textMessageReceived, this,
                            [this, socket](const QString& frame)
                            { request(socket, QJsonDocument::fromJson(frame.toUtf8()).object()); });
                });
        feed = new QTimer(this);
        feed->setInterval(250);
        connect(feed, &QTimer::timeout, this, [this] { tickers(); });
        feed->start();
    }
    QJsonObject ticker(QString name)
    {
        auto o = fixtures::option(utcNow());
        if (name == "BTC_USDC-PERPETUAL")
            return {{"instrument_name", name},
                    {"timestamp", utcNow() + clockLeadMs},
                    {"best_bid_price", invalidBid ? QJsonValue(QJsonValue::Null)
                                                  : QJsonValue(99999 + quoteShift - spreadExtra)},
                    {"best_ask_price", 100001 + quoteShift + spreadExtra},
                    {"mark_price", 100000},
                    {"index_price", 100000}};
        return {{"instrument_name", name},
                {"timestamp", utcNow() + clockLeadMs},
                {"mark_price", name.startsWith("BTC_USDC-") ? 6000 : .06},
                {"index_price", 100000},
                {"underlying_price", 100000},
                {"mark_iv", 60},
                {"greeks", QJsonObject{{"delta", invalidGreeks ? QJsonValue(QJsonValue::Null)
                                                               : QJsonValue(delta)},
                                       {"gamma", .000025},
                                       {"theta", -100},
                                       {"vega", 110}}}};
    }
    void send(QWebSocket* socket, qint64 id, QJsonValue value)
    {
        socket->sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", value}})
                .toJson(QJsonDocument::Compact)));
    }
    void notification(QString channel, QJsonObject payload)
    {
        for (auto* socket : peers)
            if (socket->state() == QAbstractSocket::ConnectedState)
                socket->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{"method", "subscription"},
                                              {"params", QJsonObject{{"channel", channel},
                                                                     {"data", payload}}}})
                        .toJson(QJsonDocument::Compact)));
    }
    void tickers()
    {
        if (!peer)
            return;
        if (perpetualNotifications)
            notification("ticker.BTC_USDC-PERPETUAL.100ms", ticker("BTC_USDC-PERPETUAL"));
        auto o = fixtures::option(utcNow());
        if (optionNotifications)
            notification("ticker." + o.spec.name + ".100ms", ticker(o.spec.name));
    }
    void verifyIntent(const QString& label)
    {
        QString connection = "verify_" + QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(corePath);
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (!db.open())
                durable = false;
            else
            {
                QSqlQuery q(db);
                q.prepare("SELECT data FROM orders WHERE label=?");
                q.addBindValue(label);
                if (!q.exec() || !q.next() ||
                    object(q.value(0).toString())["mutation"].toString().isEmpty())
                    durable = false;
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void fillOrder(const QString& label, double amount, int delayMs = 0)
    {
        auto& order = byLabel[label];
        const double cumulative = order["filled_amount"].toDouble() + amount;
        position += (order["direction"] == "buy" ? 1 : -1) * amount;
        order["filled_amount"] = cumulative;
        order["order_state"] =
            cumulative + Epsilon >= order["amount"].toDouble() ? "filled" : "open";
        order["last_update_timestamp"] = utcNow();
        QJsonObject trade{{"trade_id", "partial-" + QString::number(++fillSequence)},
                          {"order_id", order["order_id"]},
                          {"label", label},
                          {"instrument_name", "BTC_USDC-PERPETUAL"},
                          {"direction", order["direction"]},
                          {"amount", amount},
                          {"price", order["price"]},
                          {"fee", .001},
                          {"fee_currency", "USDC"},
                          {"timestamp", utcNow()}};
        fills[order["order_id"].toString()].append(trade);
        const QJsonObject payload{{"orders", QJsonArray{order}}, {"trades", QJsonArray{trade}}};
        QTimer::singleShot(delayMs, this, [this, payload]
                           { notification("user.changes.BTC_USDC-PERPETUAL.100ms", payload); });
    }
    void rpcError(QWebSocket* socket, qint64 id, int code, const QString& message)
    {
        socket->sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"id", id},
                                      {"error", QJsonObject{{"code", code}, {"message", message}}}})
                .toJson()));
    }
    void request(QWebSocket* socket, QJsonObject j)
    {
        QString method = j["method"].toString();
        auto p = j["params"].toObject();
        qint64 id = j["id"].toInteger();
        auto o = fixtures::option(utcNow());
        if (method == "public/get_time")
        {
            send(socket, id, utcNow() + clockLeadMs);
            return;
        }
        if (method == "public/auth")
        {
            send(socket, id, QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}});
            return;
        }
        if (method == "public/set_heartbeat" || method == "public/test" ||
            method == "private/enable_cancel_on_disconnect" || method == "private/subscribe")
        {
            send(socket, id, "ok");
            return;
        }
        if (method == "private/get_account_summary")
        {
            send(socket, id,
                 QJsonObject{{"id", 123},
                             {"margin_balance", 10000},
                             {"maintenance_margin", 100},
                             {"available_funds", 9900}});
            return;
        }
        if (method == "public/get_instrument")
        {
            auto spec = p["instrument_name"] == "BTC_USDC-PERPETUAL"
                            ? fixtures::perpetual()
                            : optionSpecs.value(p["instrument_name"].toString(), o.spec);
            auto result = spec.json();
            result["state"] = "open";
            result["settlement_period"] = spec.kind == "future" ? "perpetual" : "month";
            result["option_type"] = spec.name.endsWith("-P") ? "put" : "call";
            send(socket, id, result);
            return;
        }
        if (method == "private/get_positions")
        {
            ++positionRequests;
            if (failPositionReads > 0)
            {
                --failPositionReads;
                socket->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                              {"id", id},
                                              {"error", QJsonObject{{"code", 10028},
                                                                    {"message",
                                                                     "simulated read rejection"}}}})
                        .toJson(QJsonDocument::Compact)));
                return;
            }
            if (overrideOptionPositions)
            {
                send(socket, id, optionPositions);
                return;
            }
            send(socket, id,
                 QJsonArray{QJsonObject{{"instrument_name", o.spec.name},
                                        {"size", .1},
                                        {"direction", "buy"},
                                        {"average_price", .05},
                                        {"average_price_usd", 5000}}});
            return;
        }
        if (method == "private/get_position")
        {
            positionReadTimes.append(utcNow());
            if (fillBeforePositionReply)
                for (auto it = byLabel.begin(); it != byLabel.end(); ++it)
                    if (it.value()["order_state"] == "open")
                    {
                        fillBeforePositionReply = false;
                        fillOrder(it.key(),
                                  it.value()["amount"].toDouble() -
                                      it.value()["filled_amount"].toDouble(),
                                  350);
                        break;
                    }
            const double reported =
                positionReplies.isEmpty() ? position : positionReplies.takeFirst();
            send(socket, id,
                 QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"},
                             {"size_currency", reported},
                             {"realized_funding", 0},
                             {"direction", reported < 0 ? "sell" : "buy"}});
            return;
        }
        if (method == "public/ticker")
        {
            const QString name = p["instrument_name"].toString();
            auto value = ticker(name);
            if (name == "BTC_USDC-PERPETUAL" && stalePerpetualSnapshots)
                value["timestamp"] = utcNow() - Time::MarketDataMaxAgeMs - 1000;
            if (name != "BTC_USDC-PERPETUAL")
            {
                ++optionTickerRequests;
                if (staleOptionSnapshots)
                    value["timestamp"] = utcNow() - Time::MarketDataMaxAgeMs - 1000;
                if (optionSnapshotDelayMs > 0)
                {
                    QTimer::singleShot(optionSnapshotDelayMs, socket,
                                       [this, socket, id, value] { send(socket, id, value); });
                    return;
                }
            }
            send(socket, id, value);
            return;
        }
        if (method == "private/get_open_orders_by_instrument")
        {
            ++resetOrderSnapshots;
            if (lateProfileOrder && !cancelledLabels.isEmpty() && resetOrderSnapshots >= 3)
                byLabel[cancelledLabels.first()]["order_state"] = "open";
            QJsonArray a;
            for (auto order : byLabel)
                if (order["order_state"] == "open")
                    a.append(order);
            send(socket, id, a);
            return;
        }
        if (method == "private/get_order_state_by_label" || method == "private/get_order_state")
        {
            for (auto order : byLabel)
                if (order["label"] == p["label"] || order["order_id"] == p["order_id"])
                {
                    send(socket, id, order);
                    return;
                }
            send(socket, id, QJsonArray{});
            return;
        }
        if (method == "private/get_user_trades_by_order")
        {
            send(socket, id, fills.value(p["order_id"].toString()));
            return;
        }
        if (method == "private/get_user_trades_by_instrument_and_time")
        {
            if (rejectRecoveryHistory)
            {
                rpcError(socket, id, std::exchange(rejectRecoveryHistory, 0),
                         "temporary history rejection");
                return;
            }
            send(socket, id, QJsonObject{{"trades", QJsonArray{}}, {"has_more", false}});
            return;
        }
        if (method == "private/cancel_by_label")
        {
            ++mutations;
            if (rejectRecoveryCancel)
            {
                rpcError(socket, id, std::exchange(rejectRecoveryCancel, 0),
                         "temporary cancellation rejection");
                return;
            }
            const auto label = p["label"].toString();
            cancelledLabels.append(label);
            if (rejectProfileCancel)
            {
                socket->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(
                        QJsonObject{
                            {"id", id},
                            {"error", QJsonObject{{"code", 10028},
                                                  {"message", "simulated cancellation failure"}}}})
                        .toJson(QJsonDocument::Compact)));
                return;
            }
            int cancelled = 0;
            for (auto it = byLabel.begin(); it != byLabel.end(); ++it)
                if (it->value("label") == label && it->value("order_state") == "open")
                {
                    it.value()["order_state"] = "cancelled";
                    ++cancelled;
                }
            send(socket, id, cancelled);
            return;
        }
        if (method == "private/get_margins")
        {
            send(socket, id, QJsonObject{{"buy", 10}, {"sell", 10}});
            return;
        }
        if (method == "private/get_transaction_log")
        {
            if (p["query"] == o.spec.name)
            {
                ++historyPages;
                if (paginateHistory && !p.contains("continuation"))
                {
                    send(socket, id,
                         QJsonObject{{"logs", QJsonArray{}}, {"continuation", "next-option-page"}});
                    return;
                }
            }
            QJsonArray a;
            if (p["query"] == o.spec.name)
                a.append(QJsonObject{{"id", 1},
                                     {"timestamp", utcNow() - 10000},
                                     {"instrument_name", o.spec.name},
                                     {"type", "trade"},
                                     {"side", "open buy"},
                                     {"amount", .1},
                                     {"position", .1},
                                     {"cashflow", -.005},
                                     {"price", .05},
                                     {"index_price", 100000},
                                     {"commission", 0}});
            send(socket, id, QJsonObject{{"logs", a}, {"continuation", QJsonValue::Null}});
            return;
        }
        if (method == "private/buy" || method == "private/sell")
        {
            lastSubmitted = p;
            submitted.append(p);
            const double amount = p["amount"].toDouble();
            if (amount <= 0 || amount != roundDown(amount, fixtures::perpetual().step))
            {
                ++invalidAmounts;
                socket->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(
                        QJsonObject{
                            {"id", id},
                            {"error", QJsonObject{{"code", -32602},
                                                  {"message", "Invalid params"},
                                                  {"data", QJsonObject{{"param", "amount"}}}}}})
                        .toJson(QJsonDocument::Compact)));
                return;
            }
            ++mutations;
            if (rejectNextCreateCode)
            {
                const int code = std::exchange(rejectNextCreateCode, 0);
                rpcError(socket, id, code, "temporary exchange rejection");
                return;
            }
            QString label = p["label"].toString();
            verifyIntent(label);
            if (dropBeforeCreate)
            {
                dropBeforeCreate = false;
                socket->close();
                return;
            }
            QJsonObject order{{"label", label},
                              {"order_id", QString::number(++next)},
                              {"instrument_name", "BTC_USDC-PERPETUAL"},
                              {"direction", method == "private/buy" ? "buy" : "sell"},
                              {"order_type", "limit"},
                              {"order_state", "open"},
                              {"amount", p["amount"]},
                              {"filled_amount", 0},
                              {"price", p["price"]},
                              {"post_only", p["post_only"]},
                              {"reduce_only", p["reduce_only"]},
                              {"time_in_force", p["time_in_force"]},
                              {"last_update_timestamp", utcNow()}};
            QJsonArray trades;
            if (!holdMaker || p["time_in_force"] == "immediate_or_cancel")
            {
                const double executed = partialIoc && p["time_in_force"] == "immediate_or_cancel" &&
                                                amount > .0001 + Epsilon
                                            ? std::max(.0001, roundDown(amount / 2, .0001))
                                            : amount;
                position += (method == "private/buy" ? 1 : -1) * executed;
                order["filled_amount"] = executed;
                order["order_state"] = executed + Epsilon >= amount ? "filled" : "cancelled";
                trades.append(QJsonObject{{"trade_id", QString::number(next)},
                                          {"order_id", order["order_id"]},
                                          {"label", label},
                                          {"instrument_name", "BTC_USDC-PERPETUAL"},
                                          {"direction", order["direction"]},
                                          {"amount", executed},
                                          {"price", p["price"]},
                                          {"fee", .01},
                                          {"fee_currency", "USDC"},
                                          {"timestamp", utcNow()}});
                fills[order["order_id"].toString()] = trades;
            }
            byLabel[label] = order;
            if (!dropReply)
                send(socket, id, QJsonObject{{"order", order}, {"trades", trades}});
            else
            {
                socket->close();
                dropReply = false;
            }
            return;
        }
        if (method == "private/edit" || method == "private/cancel")
        {
            ++mutations;
            for (auto it = byLabel.begin(); it != byLabel.end(); ++it)
                if (it->value("order_id") == p["order_id"])
                {
                    verifyIntent(it.key());
                    auto order = it.value();
                    if (method == "private/cancel" && fillOnCancel &&
                        order["order_state"] == "open")
                    {
                        fillOnCancel = false;
                        fillOrder(it.key(),
                                  order["amount"].toDouble() - order["filled_amount"].toDouble(),
                                  200);
                        rpcError(socket, id, 11044, "not_open_order");
                        return;
                    }
                    if (method == "private/edit" && rejectNextEdit)
                    {
                        rejectNextEdit = false;
                        it.value()["order_state"] = "cancelled";
                        it.value()["last_update_timestamp"] = utcNow();
                        const auto cancelled = it.value();
                        QTimer::singleShot(200, this,
                                           [this, cancelled]
                                           {
                                               notification("user.changes.BTC_USDC-PERPETUAL.100ms",
                                                            {{"orders", QJsonArray{cancelled}}});
                                           });
                        rpcError(socket, id, 11054, "post_only_reject");
                        return;
                    }
                    if (method == "private/cancel")
                        order["order_state"] = "cancelled";
                    else
                    {
                        order["price"] = p["price"];
                        order["amount"] = p["amount"];
                        lastEditAmount = p["amount"].toDouble();
                    }
                    order["last_update_timestamp"] = utcNow();
                    it.value() = order;
                    send(socket, id,
                         method == "private/cancel"
                             ? QJsonValue(order)
                             : QJsonValue(QJsonObject{{"order", order}, {"trades", QJsonArray{}}}));
                    return;
                }
        }
        send(socket, id, QJsonObject{});
    }
};
struct RuntimeHarness
{
    QThread serverThread, thread;
    FakeDeribit* server = nullptr;
    TradingWorker* worker = nullptr;
    QUrl endpoint;
    std::shared_ptr<SharedState> state = std::make_shared<SharedState>();
    RuntimeHarness(QString path, Execution mode = Execution::PostOnly, bool hold = false,
                   bool drop = false, bool startWorker = true, int clockLead = 0,
                   double initialPosition = 0, QList<double> positionReplies = {})
    {
        server = new FakeDeribit;
        server->clockLeadMs = clockLead;
        server->position = initialPosition;
        server->positionReplies = std::move(positionReplies);
        server->corePath = path;
        server->holdMaker = hold;
        server->dropReply = drop;
        server->moveToThread(&serverThread);
        QObject::connect(&serverThread, &QThread::started, server, &FakeDeribit::initialize);
        QObject::connect(&serverThread, &QThread::finished, server, &QObject::deleteLater);
        serverThread.start();
        quint16 port = 0;
        QMetaObject::invokeMethod(
            server, [&] { port = server->port; }, Qt::BlockingQueuedConnection);
        endpoint = QUrl(QString("ws://127.0.0.1:%1").arg(port));
        if (!startWorker)
            return;
        Config c;
        c.hedgePercent = 20;
        c.selected = {fixtures::option(utcNow()).spec.name};
        c.maxMarginUsage = .5;
        c.execution = mode;
        c.makerWaitMs = 500;
        worker = new TradingWorker(path, c, state, QUrl(QString("ws://127.0.0.1:%1").arg(port)));
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::started, worker, &TradingWorker::initialize);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(worker, [this] { worker->connectExchange("fake", "fake"); });
    }
    ~RuntimeHarness()
    {
        if (thread.isRunning())
        {
            QMetaObject::invokeMethod(worker, &TradingWorker::shutdown,
                                      Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
        }
        if (serverThread.isRunning())
        {
            QMetaObject::invokeMethod(
                server,
                [this]
                {
                    server->feed->stop();
                    server->server->close();
                },
                Qt::BlockingQueuedConnection);
            serverThread.quit();
            serverThread.wait();
        }
    }
    TradingSnapshot snapshot()
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->trading;
    }
    void start()
    {
        QMetaObject::invokeMethod(worker, &TradingWorker::startTrading);
    }
};
static QString seedMismatchedProfile(const QString& path)
{
    Store store(path);
    auto engine = fixtures::engine(utcNow());
    engine.inventory = -.01;
    engine.anchored = true;
    store.bind("123/testnet/BTC_USDC-PERPETUAL");
    store.metadata("construction", {{"id", "old-construction"}});
    store.fill({{"instrument_name", engine.config.instrument},
                {"trade_id", "old-fill"},
                {"order_id", "old-order"},
                {"amount", .01},
                {"price", 100000},
                {"direction", "sell"},
                {"fee", .1},
                {"fee_currency", "USDC"},
                {"timestamp", utcNow()},
                {"construction_id", "old-construction"}});
    store.save(engine, {});
    return store.profileId();
}
class RuntimeTests : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        theme::apply(*qobject_cast<QApplication*>(QCoreApplication::instance()));
    }
    void selectionHidesClosedAndExpiredWithoutDeletingAccountingLegs()
    {
        Window window("", Config{}, true);
        window.show();
        const qint64 now = utcNow();
        auto active = fixtures::option(now);
        auto closed = fixtures::option(now, true, false, 0);
        auto expired = fixtures::option(now, false);
        expired.spec.expiry = now - 1000;
        const auto state = window.sharedState();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->trading.inventory = 0;
            state->trading.positions = {closed, expired, active};
            state->trading.config.selected = {closed.spec.name, expired.spec.name,
                                              active.spec.name};
        }
        QTest::qWait(250);
        bool inspected = false;
        QTimer::singleShot(
            0, &window,
            [&]
            {
                auto* dialog = window.findChild<QDialog*>("optionSelection");
                auto* table = dialog->findChild<QTableWidget*>("optionSelectionTable");
                inspected = table->rowCount() == 1 && table->item(0, 1)->text() == active.spec.name;
                dialog->reject();
            });
        window.findChild<QPushButton*>("selectOptions")->click();
        QVERIFY(inspected);
        std::lock_guard<std::mutex> lock(state->mutex);
        QCOMPARE(state->trading.positions.size(), 3);
        QCOMPARE(state->trading.config.selected.size(), 3);
    }
    void positionPollingRecoversAndDiscoversReplacementOptions()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        const QString path = d.filePath("core.db");
        const QString oldName = fixtures::option(utcNow()).spec.name;
        auto next = fixtures::option(utcNow(), false, false, -.02);
        const QJsonObject row{{"instrument_name", next.spec.name},
                              {"size", .02},
                              {"direction", "sell"},
                              {"average_price", 5000},
                              {"average_price_usd", 5000}};
        {
            RuntimeHarness h(path);
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
            QMetaObject::invokeMethod(
                h.server,
                [&]
                {
                    h.server->overrideOptionPositions = true;
                    h.server->optionPositions = {row};
                    h.server->optionSpecs[next.spec.name] = next.spec;
                    h.server->failPositionReads = 1;
                },
                Qt::BlockingQueuedConnection);
            auto available = [&] { return h.snapshot().openOptions(utcNow()); };
            QTRY_VERIFY_WITH_TIMEOUT(
                available().size() == 1 && available().first().spec.name == next.spec.name, 15000);
            QCOMPARE(available().first().quantity, -.02);
            QVERIFY(!available().first().isCall);
            const auto state = h.snapshot();
            bool retained = false;
            for (const auto& option : state.positions)
                if (option.spec.name == oldName)
                {
                    retained = true;
                    QCOMPARE(option.quantity, 0.0);
                }
            QVERIFY(retained);
            QVERIFY(state.config.selected.contains(oldName));
            QMetaObject::invokeMethod(
                h.server, [&] { h.server->optionPositions = {}; }, Qt::BlockingQueuedConnection);
            QTRY_VERIFY_WITH_TIMEOUT(available().isEmpty() && h.snapshot().positions.size() == 1,
                                     10000);
            QMetaObject::invokeMethod(
                h.server, [&] { h.server->optionPositions = {row}; }, Qt::BlockingQueuedConnection);
            QTRY_VERIFY_WITH_TIMEOUT(
                available().size() == 1 && available().first().spec.name == next.spec.name, 10000);
            QVERIFY(!available().first().isCall);
            QCOMPARE(h.server->mutations.load(), 0);
        }
        Store store(path);
        Hedger restored;
        QVector<Order> orders;
        QVERIFY(store.load(restored, orders));
        QCOMPARE(restored.options.size(), 1);
        QCOMPARE(restored.options.first().spec.name, oldName);
        QCOMPARE(restored.options.first().quantity, 0.0);
    }
    void quietOptionMarketsRefreshBeforeStartAndWhileRunning()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        RuntimeHarness h(d.filePath("core.db"));
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->optionNotifications = false; }, Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QVERIFY(h.server->optionTickerRequests.load() > 0);
        h.start();
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(h.snapshot().inventory + .01) < 1e-8, 10000);
        const int snapshots = h.server->optionTickerRequests.load();
        QTest::qWait(Time::MarketDataMaxAgeMs + 1500);
        QVERIFY2(h.snapshot().running, qPrintable(h.snapshot().problem));
        QVERIFY(h.snapshot().positions.first().freshGreeks(utcNow()));
        QVERIFY(h.server->optionTickerRequests.load() > snapshots);
        QCOMPARE(h.server->mutations.load(), 1);
    }
    void staleOptionSnapshotsDoNotPermitOrders()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        RuntimeHarness h(d.filePath("core.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->optionNotifications = false;
                h.server->staleOptionSnapshots = true;
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("Waiting for fresh"), 10000);
        const int requests = h.server->optionTickerRequests.load();
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->optionTickerRequests.load() > requests, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().busy, 5000);
        QVERIFY(!h.snapshot().running);
        QCOMPARE(h.server->mutations.load(), 0);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->staleOptionSnapshots = false; },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(h.snapshot().inventory + .01) < 1e-8, 10000);
        QVERIFY(h.snapshot().running);
    }
    void stopCancelsStartWhileMarketSnapshotsArePending()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        RuntimeHarness h(d.filePath("core.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->optionSnapshotDelayMs = 1000; },
            Qt::BlockingQueuedConnection);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().busy, 5000);
        QMetaObject::invokeMethod(h.worker, &TradingWorker::stopTrading);
        QTRY_VERIFY_WITH_TIMEOUT(!h.snapshot().busy, 5000);
        QVERIFY(!h.snapshot().running);
        QCOMPARE(h.server->mutations.load(), 0);
        QCOMPARE(h.snapshot().inventory, 0.0);
    }
    void latencyDistinguishesOfflinePendingAndSlowReplies()
    {
        Window window("", Config{}, true);
        window.show();
        auto state = window.sharedState();
        auto* latency = window.findChild<QLabel*>("latencyStatus");
        auto* connection = window.findChild<QLabel*>("connectionStatus");
        QVERIFY(latency);
        QVERIFY(connection);
        QCOMPARE(latency->text(), QString::fromUtf8("Latency: — ms"));
        QCOMPARE(connection->text(), QString("Disconnected"));
        QVERIFY(!latency->styleSheet().contains(theme::Alert));
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->trading.connected = true;
            state->trading.connectionStatus = "Connected";
            state->trading.latencyMs = 12;
            state->trading.connectionSlow = false;
        }
        QTRY_COMPARE(latency->text(), QString("Latency: 12 ms"));
        QCOMPARE(connection->text(), QString("Connected"));
        QVERIFY(!latency->styleSheet().contains(theme::Alert));
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->trading.latencyMs = 2400;
            state->trading.connectionSlow = true;
        }
        QTRY_COMPARE(latency->text(), QString("Latency: 2400 ms"));
        QVERIFY(latency->styleSheet().contains(theme::Alert));
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->trading.latencyMs = -1;
        }
        QTRY_COMPARE(latency->text(), QString::fromUtf8("Latency: — ms"));
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->trading.connected = false;
            state->trading.connectionStatus = "Disconnected";
        }
        QTRY_COMPARE(latency->text(), QString::fromUtf8("Latency: — ms"));
        QVERIFY(!latency->text().contains("-1"));
    }
    void externalOptionTradeRequiresBasketConfirmation()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        RuntimeHarness h(d.filePath("core.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                auto name = fixtures::option(utcNow()).spec.name;
                h.server->notification(
                    "user.changes." + name + ".100ms",
                    {{"trades", QJsonArray{QJsonObject{{"trade_id", "external"}}}}});
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("externally"), 5000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("Confirm the changed"), 5000);
        QCOMPARE(h.server->mutations.load(), 0);
        Config c = h.snapshot().config;
        QMetaObject::invokeMethod(h.worker, [&] { h.worker->applyConfig(c); });
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.isEmpty(), 5000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(h.snapshot().inventory + .01) < 1e-8, 10000);
    }
    void aheadOfPcQuotesConnectAndStayLive()
    {
        QTemporaryDir d;
        const QString path = d.filePath("core.db");
        RuntimeHarness h(path, Execution::PostOnly, false, false, false, 462);
        Config c;
        c.hedgePercent = 20;
        c.selected = {fixtures::option(utcNow()).spec.name};
        Window window(path, c, false, h.endpoint);
        window.show();
        auto shared = window.sharedState();
        auto snapshot = [&]
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->trading;
        };
        QTRY_VERIFY(!snapshot().profileId.isEmpty());
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_VERIFY_WITH_TIMEOUT(snapshot().synced, 15000);
        QTRY_COMPARE(window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"));
        auto* table = window.findChild<QTableWidget*>("optionPositions");
        QTRY_COMPARE(table->rowCount(), 1);
        for (int i = 0; i < 10; ++i)
        {
            QTest::qWait(100);
            QCOMPARE(table->item(0, 8)->text(), QString("Live"));
            QVERIFY(snapshot().problem.isEmpty());
        }
        QVERIFY(snapshot().exchangeClockOffsetMs.has_value());
        QVERIFY(window.findChild<QLineEdit*>("clientSecret")->text().isEmpty());
        auto* cooldown = window.findChild<QDoubleSpinBox*>("cooldown");
        QVERIFY(!window.findChild<QDialog*>("advancedDialog")->isAncestorOf(cooldown));
        cooldown->setValue(2.3);
        auto* advanced = window.findChild<QDialog*>("advancedDialog");
        auto* balanced = window.findChild<QSpinBox*>("balancedPassiveAttempts");
        auto* aggressive = window.findChild<QSpinBox*>("aggressivePassiveAttempts");
        QVERIFY(advanced->isAncestorOf(balanced));
        QVERIFY(advanced->isAncestorOf(aggressive));
        balanced->setValue(4);
        aggressive->setValue(2);
        window.findChild<QDoubleSpinBox*>("makerWait")->setValue(5);
        auto* apply = window.findChild<QPushButton*>("applySettings");
        QTRY_VERIFY(apply->property("pendingChanges").toBool());
        QCOMPARE(apply->text(), QString("Apply"));
        apply->click();
        QTRY_COMPARE(snapshot().config.cooldownMs, 2300);
        QTRY_COMPARE(snapshot().config.balancedPassiveAttempts, 4);
        QCOMPARE(snapshot().config.aggressivePassiveAttempts, 2);
        QCOMPARE(snapshot().config.makerWaitMs, 5000);
        QTRY_VERIFY(!apply->property("pendingChanges").toBool());
        window.findChild<QPushButton*>("start")->click();
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(snapshot().inventory + .01) < 1e-8, 10000);
        window.findChild<QPushButton*>("stop")->click();
        QTRY_VERIFY(!snapshot().running && !snapshot().busy);
    }
    void exchangeClockDifferenceDoesNotMakeFreshQuotesStale_data()
    {
        QTest::addColumn<int>("offset");
        QTest::newRow("reported-1063ms") << 1063;
        QTest::newRow("2500ms") << 2500;
        QTest::newRow("computer-ahead") << -2500;
    }
    void exchangeClockDifferenceDoesNotMakeFreshQuotesStale()
    {
        QFETCH(int, offset);
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("core.db"), Execution::PostOnly, false, false, true, offset);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QVERIFY(h.snapshot().problem.isEmpty());
        QVERIFY(h.snapshot().quote.fresh(utcNow()));
        QVERIFY(std::abs(utcNow() - h.snapshot().quote.at) < 500);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored && h.snapshot().running, 10000);
        QVERIFY(h.snapshot().problem.isEmpty());
    }
    void quoteFlashFadesAndReflectsChangeSize()
    {
        QuoteLabel quote("Bid", Qt::green);
        quote.show();
        quote.setQuote(10000);
        QCOMPARE(quote.flashStrength(), 0.0);
        quote.setQuote(10000.1);
        const double small = quote.flashStrength();
        QVERIFY(small > 0);
        quote.setQuote(10010);
        QVERIFY(quote.flashStrength() > small);
        QTest::qWait(750);
        QCOMPARE(quote.flashStrength(), 0.0);
    }
    void accumulatedPositionClosesWithCanonicalAmount()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("close.db");
        constexpr double inventory = -0.008599999999999997;
        {
            Store store(path);
            auto engine = fixtures::engine(utcNow());
            engine.inventory = inventory;
            store.bind("123/testnet/BTC_USDC-PERPETUAL");
            store.save(engine, {});
        }
        RuntimeHarness h(path, Execution::PostOnly, false, false, true, 0, inventory);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(h.worker, &TradingWorker::closeHedge);
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() >= 1, 10000);
        QTRY_VERIFY(std::abs(h.snapshot().inventory) < 1e-9 && !h.snapshot().running &&
                    !h.snapshot().busy);
        QCOMPARE(h.server->invalidAmounts.load(), 0);
        QJsonObject sent;
        QMetaObject::invokeMethod(
            h.server, [&] { sent = h.server->lastSubmitted; }, Qt::BlockingQueuedConnection);
        QCOMPARE(sent["amount"].toDouble(), .0086);
        QVERIFY(sent["reduce_only"].toBool());
        QCOMPARE(h.snapshot().inventory, 0.);
    }
    void reversalDoesNotSubmitZeroAmount_data()
    {
        QTest::addColumn<double>("inventory");
        QTest::addColumn<double>("delta");
        QTest::newRow("long-to-short") << .0017000000000000036 << .5;
        QTest::newRow("short-to-long") << -.0017000000000000036 << -.5;
        QTest::newRow("restored-positive-dust") << 3.686287386450715e-18 << .5;
        QTest::newRow("restored-negative-dust") << -3.686287386450715e-18 << -.5;
    }
    void reversalDoesNotSubmitZeroAmount()
    {
        QFETCH(double, inventory);
        QFETCH(double, delta);
        QTemporaryDir directory;
        const auto path = directory.filePath("reversal.db");
        {
            Store store(path);
            auto engine = fixtures::engine(utcNow());
            engine.inventory = inventory;
            engine.config.hedgePercent = 3.8;
            engine.config.deltaChange = .04; // Keep this small basket above the minimum hedge lot.
            engine.config.execution = Execution::Balanced;
            store.bind("123/testnet/BTC_USDC-PERPETUAL");
            store.save(engine, {});
        }
        RuntimeHarness h(path, Execution::Balanced, false, false, true, 0, inventory);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->delta = delta; }, Qt::BlockingQueuedConnection);
        QTRY_COMPARE(h.snapshot().positions[0].delta, delta);
        if (std::abs(inventory) < Epsilon)
            QCOMPARE(h.snapshot().inventory, 0.);
        h.start();
        const double target = delta > 0 ? -.0019 : .0019;
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored && h.snapshot().running &&
                                     !h.snapshot().busy &&
                                     std::abs(h.snapshot().inventory - target) < Epsilon,
                                 10000);
        QCOMPARE(h.server->invalidAmounts.load(), 0);
        QVERIFY(h.snapshot().problem.isEmpty());
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                const bool hadPosition = std::abs(inventory) > Epsilon;
                QCOMPARE(h.server->submitted.size(), hadPosition ? 2 : 1);
                if (hadPosition)
                {
                    const auto first = h.server->submitted.first();
                    QCOMPARE(first["amount"].toDouble(), .0017);
                    QVERIFY(first["reduce_only"].toBool());
                }
                const auto last = h.server->submitted.last();
                QCOMPARE(last["amount"].toDouble(), .0019);
                QVERIFY(!last["reduce_only"].toBool());
            },
            Qt::BlockingQueuedConnection);
    }
    void connectedUiStartsOnePersistentPnlPeriod()
    {
        QTemporaryDir d;
        const auto path = d.filePath("core.db");
        RuntimeHarness h(path, Execution::PostOnly, false, false, false);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->paginateHistory = true; }, Qt::BlockingQueuedConnection);
        Config c;
        c.hedgePercent = 20;
        c.selected = {fixtures::option(utcNow()).spec.name};
        Window window(path, c, false, h.endpoint);
        window.show();
        const auto shared = window.sharedState();
        auto report = [&]
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->analytics;
        };
        QTRY_VERIFY(!(
            [&]
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                return shared->trading.profileId.isEmpty();
            })());
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->historyPages.load() >= 2, 15000);
        QVERIFY(!report().ready);
        QVERIFY(!window.findChild<QAction*>("newSession"));
        window.findChild<QPushButton*>("start")->click();
        QTRY_VERIFY_WITH_TIMEOUT(report().ready && report().complete, 10000);
        const auto started = report().startedAt;
        QVERIFY(started > 0);
        window.findChild<QPushButton*>("stop")->click();
        QTRY_VERIFY(!(
            [&]
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                return shared->trading.running || shared->trading.busy;
            })());
        window.findChild<QPushButton*>("start")->click();
        QTest::qWait(500);
        QCOMPARE(report().startedAt, started);
        window.findChild<QPushButton*>("stop")->click();
    }
    void demoHasEnglishControls()
    {
        Window window("", Config{}, true);
        window.show();
        QTest::qWait(50);
        QVERIFY(window.findChild<QPushButton*>("start"));
        QVERIFY(!window.findChild<ProfileChart*>("portfolioChart"));
        QCOMPARE(window.findChild<QComboBox*>("executionMode")->currentText(), QString("Balanced"));
        QVERIFY(window.findChild<QLabel*>("normalizedDelta")->text().contains("Normalized delta"));
        QVERIFY(!window.findChild<QLabel*>("netPnl"));
        auto* deltaPanel = window.findChild<QWidget*>("deltaPanel");
        auto* executionPanel = window.findChild<QWidget*>("executionPanel");
        QCOMPARE(executionPanel->geometry().top(), deltaPanel->geometry().top());
        QCOMPARE(executionPanel->height(), deltaPanel->height());
        auto* rebalance = window.findChild<QLabel*>("rebalanceBand");
        QCOMPARE(rebalance->parentWidget(), executionPanel);
        auto* breakEven = window.findChild<QLabel*>("breakEvenMove");
        QCOMPARE(breakEven->parentWidget(), executionPanel);
        QVERIFY(!window.findChild<QLabel*>("escapeDelta"));
        QCOMPARE(window.findChild<QLabel*>("tradingActivity")->y(),
                 window.findChild<QLabel*>("normalizedDelta")->y());
        QCOMPARE(rebalance->y(), window.findChild<QLabel*>("referenceDelta")->y());
        QCOMPARE(breakEven->y(), window.findChild<QLabel*>("targetNetDelta")->y());
        auto* bid = window.findChild<QWidget*>("bidQuote");
        auto* ask = window.findChild<QWidget*>("askQuote");
        auto* middle = window.findChild<QLabel*>("riskSummary");
        QVERIFY(bid->x() < ask->x() && ask->x() < middle->x());
        QVERIFY(middle->text().startsWith("Middle Price:"));
    }
    void advancedAttemptCountsAndCompactDialog()
    {
        Window window("", Config{}, true);
        window.show();
        auto* advanced = window.findChild<QDialog*>("advancedDialog");
        auto* mode = window.findChild<QComboBox*>("executionMode");
        auto* balanced = window.findChild<QSpinBox*>("balancedPassiveAttempts");
        auto* aggressive = window.findChild<QSpinBox*>("aggressivePassiveAttempts");
        auto* duration = window.findChild<QDoubleSpinBox*>("makerWait");
        QCOMPARE(duration->value(), 5.);
        QTimer::singleShot(1000, advanced, &QDialog::reject);
        QTimer::singleShot(50, &window,
                           [&]
                           {
                               QVERIFY(balanced->isVisible());
                               QCOMPARE(balanced->value(), 3);
                               mode->setCurrentIndex(int(Execution::Aggressive));
                               QVERIFY(aggressive->isVisible());
                               QCOMPARE(aggressive->value(), 1);
                               aggressive->setValue(2);
                               mode->setCurrentIndex(int(Execution::Balanced));
                               QCOMPARE(balanced->value(), 3);
                               balanced->setValue(4);
                               duration->setValue(7);
                               auto* buttons = advanced->findChild<QDialogButtonBox*>();
                               QVERIFY(advanced->width() >= 450);
                               QVERIFY(advanced->height() - buttons->geometry().bottom() <= 16);
                               advanced->reject();
                           });
        window.findChild<QPushButton*>("advanced")->click();
        QCOMPARE(balanced->value(), 3);
        QCOMPARE(aggressive->value(), 1);
        QCOMPARE(duration->value(), 5.);
        QCOMPARE(mode->currentIndex(), int(Execution::Balanced));
    }
    void portfolioIndicatorsAndDollarGamma()
    {
        Window window("", Config{}, true);
        window.show();
        const auto shared = window.sharedState();
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            auto& s = shared->trading;
            s.positions = {fixtures::option(utcNow(), true, false, .2),
                           fixtures::option(utcNow(), false, false, -.1)};
            s.config.selected = {s.positions[0].spec.name, s.positions[1].spec.name};
            s.config.deltaChange = .01;
        }
        auto* band = window.findChild<QLabel*>("rebalanceBand");
        auto* be = window.findChild<QLabel*>("breakEvenMove");
        QTRY_VERIFY(band->text().contains("1.2%"));
        QVERIFY(be->text().contains("daily"));
        QVERIFY(be->text().contains("2.828%"));
        QVERIFY(be->toolTip().contains("2828.43"));
        QVERIFY(band->toolTip().contains("commission"));
        auto* table = window.findChild<QTableWidget*>("optionPositions");
        QCOMPARE(table->columnCount(), 9);
        QCOMPARE(table->horizontalHeaderItem(4)->text(), QString("Gamma P&L (1%)"));
        QCOMPARE(table->item(0, 4)->text().toDouble(), 2.5);
        QCOMPARE(table->item(1, 4)->text().toDouble(), -1.25);
        QVERIFY(table->item(0, 4)->toolTip().contains("25000"));
        QCOMPARE(table->item(2, 4)->text(), QString("0"));
        QCOMPARE(table->item(0, 5)->text().toDouble(), -20.);
        QCOMPARE(table->item(0, 7)->text().toDouble(), 6000.);
        QCOMPARE(table->item(0, 8)->text(), QString("Live"));
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->trading.config.hedgePercent = 200;
            shared->trading.positions[0].spec.expiry = utcNow() + Time::HourMs;
        }
        QTRY_VERIFY(be->text().contains("<1d"));
        QVERIFY(band->text().contains("1.2%"));
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->trading.positions[0].hasGreeks = false;
        }
        QTRY_VERIFY(band->text().contains(QString::fromUtf8("—")));
        QVERIFY(be->text().contains(QString::fromUtf8("—")));
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            auto& s = shared->trading;
            s.positions[0].hasGreeks = true;
            s.quote.bid = 0;
        }
        QTRY_VERIFY(
            window.findChild<QLabel*>("riskSummary")->text().contains(QString::fromUtf8("—")));
        QVERIFY(band->text().contains(QString::fromUtf8("—")));
        QVERIFY(be->text().contains(QString::fromUtf8("—")));
    }
    void connectionPositionMismatchRecoversOrPausesWithoutReset_data()
    {
        QTest::addColumn<bool>("temporary");
        QTest::newRow("stale-zero-snapshot") << true;
        QTest::newRow("persistent-unexplained-zero") << false;
    }
    void connectionPositionMismatchRecoversOrPausesWithoutReset()
    {
        QFETCH(bool, temporary);
        QTemporaryDir directory;
        const auto path = directory.filePath("connection-mismatch.db");
        const auto profile = seedMismatchedProfile(path);
        RuntimeHarness h(path, Execution::PostOnly, false, false, true, 0, temporary ? -.01 : 0,
                         {0, 0, 0});
        if (temporary)
        {
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
            QVERIFY(h.snapshot().problem.isEmpty());
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("after 3 checks"), 15000);
            QVERIFY(!h.snapshot().synced);
            QVERIFY(h.snapshot().problem.contains("exchange 0, expected -0.01 BTC"));
            QVERIFY(!h.snapshot().problem.contains("Delete"));
        }
        QVERIFY(!h.snapshot().running);
        QCOMPARE(h.snapshot().profileId, profile);
        QCOMPARE(h.snapshot().inventory, -.01);
        QCOMPARE(h.server->mutations.load(), 0);
        QList<qint64> reads;
        QMetaObject::invokeMethod(
            h.server, [&] { reads = h.server->positionReadTimes; }, Qt::BlockingQueuedConnection);
        QCOMPARE(reads.size(), temporary ? 4 : 5);
        for (int i = 3; i < reads.size(); ++i)
            QVERIFY(reads[i] - reads[i - 1] >= Time::FillRecoveryDelayMs);
        Store core(path);
        QCOMPARE(core.coveredQuantity("old-order"), .01);
        QSqlQuery query(core.database());
        QVERIFY(query.exec("SELECT data FROM events WHERE kind='position_mismatch_snapshot'"));
        QVERIFY(query.next());
        const auto details = object(query.value(0).toString());
        QCOMPARE(details["expected_position"].toDouble(), -.01);
        QCOMPARE(details["position"].toObject()["size_currency"].toDouble(), 0.);
        QCOMPARE(details["position"].toObject()["instrument_name"].toString(),
                 QString("BTC_USDC-PERPETUAL"));
    }
    void profileDeletionUnlockedAfterManualPositionChange()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("reset.db");
        const auto originalProfile = seedMismatchedProfile(path);
        RuntimeHarness exchange(path, Execution::PostOnly, false, false, false);
        {
            Window window(path, Config{}, false, exchange.endpoint);
            window.show();
            const auto shared = window.sharedState();
            auto snapshot = [&]
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                return shared->trading;
            };
            QTRY_VERIFY(!snapshot().profileId.isEmpty());
            window.findChild<QLineEdit*>("clientId")->setText("fake");
            window.findChild<QLineEdit*>("clientSecret")->setText("fake");
            window.findChild<QPushButton*>("connect")->click();
            QTRY_VERIFY_WITH_TIMEOUT(snapshot().positionMismatch && snapshot().canDelete, 15000);
            auto* action = window.findChild<QAction*>("deleteData");
            QTRY_VERIFY(action->isEnabled());
            QVERIFY(!window.findChild<QAction*>("closeHedge")->isEnabled());
            QVERIFY(!window.findChild<QAction*>("closeHedge")->toolTip().contains("Delete"));
            QVERIFY(!window.findChild<QAction*>("reconcilePosition"));
            const auto capture = qEnvironmentVariable("GAMMA_UI_QA_DIR");
            if (!capture.isEmpty())
            {
                QDir().mkpath(capture);
                window.findChild<QMenu*>("actionsMenu")
                    ->popup(window.mapToGlobal(QPoint(540, 115)));
                QTest::qWait(100);
                QVERIFY(window.grab().save(capture + "/profile-reset-unlocked.png"));
                window.findChild<QMenu*>("actionsMenu")->hide();
            }
            bool mentionsPnl = false, defaultNo = false;
            QTimer::singleShot(0, &window,
                               [&]
                               {
                                   auto* confirmation = qobject_cast<QMessageBox*>(
                                       QApplication::activeModalWidget());
                                   if (!confirmation)
                                       return;
                                   mentionsPnl = confirmation->text().contains("accumulated P&L");
                                   defaultNo = confirmation->defaultButton() ==
                                               confirmation->button(QMessageBox::No);
                                   confirmation->done(QMessageBox::Yes);
                               });
            action->trigger();
            QVERIFY(mentionsPnl && defaultNo);
            QTRY_VERIFY_WITH_TIMEOUT(snapshot().profileId != originalProfile && snapshot().synced,
                                     15000);
            QCOMPARE(snapshot().inventory, 0.0);
            QVERIFY(snapshot().orders.isEmpty());
            QVERIFY(snapshot().config.selected.isEmpty());
            QVERIFY(!snapshot().running);
            QVERIFY(snapshot().constructionId != "old-construction");
            QCOMPARE(exchange.server->mutations.load(), 0); // No order creation or position close.
        }
        // Reopening the program uses the new persisted profile and no longer mismatches.
        RuntimeHarness restart(path);
        QTRY_VERIFY_WITH_TIMEOUT(restart.snapshot().synced, 15000);
        QCOMPARE(restart.snapshot().inventory, 0.0);
        QVERIFY(restart.snapshot().profileId != originalProfile);
        Store core(path);
        QSqlQuery fills(core.database());
        QVERIFY(fills.exec("SELECT COUNT(*) FROM fills"));
        QVERIFY(fills.next());
        QCOMPARE(fills.value(0).toInt(), 0);
    }
    void profileResetCancelsOnlyOwnLabels_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::newRow("success-keeps-manual-order") << QString();
        QTest::newRow("rejected-cancellation-keeps-pnl") << QString("cancel");
        QTest::newRow("late-order-keeps-pnl") << QString("late");
    }
    void profileResetCancelsOnlyOwnLabels()
    {
        QFETCH(QString, failure);
        QTemporaryDir directory;
        const auto path = directory.filePath("reset.db");
        const auto originalProfile = seedMismatchedProfile(path);
        RuntimeHarness h(path);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().positionMismatch && h.snapshot().canDelete, 15000);
        const auto label = "gs_" + originalProfile + "_orphan";
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->resetOrderSnapshots = 0;
                h.server->rejectProfileCancel = failure == "cancel";
                h.server->lateProfileOrder = failure == "late";
                h.server->byLabel[label] = {{"instrument_name", "BTC_USDC-PERPETUAL"},
                                            {"order_id", "owned-orphan"},
                                            {"label", label},
                                            {"order_state", "open"}};
                h.server->byLabel["manual"] = {{"instrument_name", "BTC_USDC-PERPETUAL"},
                                               {"order_id", "manual-id"},
                                               {"label", "manual"},
                                               {"order_state", "open"}};
            },
            Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(h.worker, &TradingWorker::deleteData);
        if (failure.isEmpty())
        {
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().profileId != originalProfile, 15000);
            QVERIFY(h.snapshot().synced);
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("profile and P&L retained"),
                                     15000);
            QCOMPARE(h.snapshot().profileId, originalProfile);
            QCOMPARE(h.snapshot().inventory, -.01);
            QVERIFY(h.snapshot().canDelete); // Can retry instead of getting locked out again.
        }
        QStringList cancelled;
        QString manualState;
        int checks;
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                cancelled = h.server->cancelledLabels;
                manualState = h.server->byLabel["manual"]["order_state"].toString();
                checks = h.server->resetOrderSnapshots;
            },
            Qt::BlockingQueuedConnection);
        QCOMPARE(cancelled, QStringList{label});
        QCOMPARE(manualState, QString("open"));
        QCOMPARE(h.server->mutations.load(), 1); // Only cancel_by_label; no creates or closes.
        if (failure != "cancel")
            QVERIFY(checks >= 3); // Two separate final verification reads.
        Store core(path);
        QSqlQuery fills(core.database());
        QVERIFY(fills.exec("SELECT COUNT(*) FROM fills"));
        QVERIFY(fills.next());
        QCOMPARE(fills.value(0).toInt(), failure.isEmpty() ? 0 : 1);
    }
    void profileResetLeavesRemainingPositionUnmanaged()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("reset.db");
        const auto originalProfile = seedMismatchedProfile(path);
        {
            RuntimeHarness h(path, Execution::PostOnly, false, false, true, 0, .02);
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().canDelete, 15000);
            QMetaObject::invokeMethod(h.worker, &TradingWorker::deleteData);
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().profileId != originalProfile, 15000);
            QCOMPARE(h.snapshot().inventory, 0.0);
            QCOMPARE(h.snapshot().externalPosition, .02);
            h.start();
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("still unmanaged"), 5000);
            QCOMPARE(h.server->mutations.load(), 0);
        }
        // Its baseline is durable, not a phantom bot hedge after restart.
        RuntimeHarness restart(path, Execution::PostOnly, false, false, true, 0, .02);
        QTRY_VERIFY_WITH_TIMEOUT(restart.snapshot().synced, 15000);
        QCOMPARE(restart.snapshot().externalPosition, .02);
        QCOMPARE(restart.snapshot().inventory, 0.0);
        QVERIFY(!restart.snapshot().positionMismatch);
        QCOMPARE(restart.server->mutations.load(), 0);
    }
    void riskGraphIsFrozenAndComputedOnlyOnRequest()
    {
        Window window("", Config{}, true);
        window.show();
        auto shared = window.sharedState();
        auto count = [&]
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->riskCalculations;
        };
        QTest::qWait(600);
        QCOMPARE(count(), quint64(0));
        window.findChild<QPushButton*>("riskGraph")->click();
        QTRY_COMPARE(count(), quint64(1));
        auto* refresh = window.findChild<QPushButton*>("riskRefresh");
        QTRY_VERIFY(refresh->isEnabled());
        qint64 at;
        double pnl;
        const QString ivCaption = window.findChild<QLabel*>("riskIv")->text();
        QVERIFY(ivCaption.contains("60%"));
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            at = shared->risk.timestamp;
            pnl = shared->risk.curve.at(120).pnl;
            shared->trading.quote.mark += 100;
            shared->trading.timestamp += 1000;
            shared->trading.positions[0].iv = .65;
        }
        window.findChild<QComboBox*>("scenarioGreek")->setCurrentIndex(3);
        auto* dialog = window.findChild<QDialog*>("riskDialog");
        dialog->resize(1000, 600);
        QTest::qWait(600);
        QCOMPARE(count(), quint64(1));
        QCOMPARE(window.findChild<QLabel*>("riskIv")->text(), ivCaption);
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            QCOMPARE(shared->risk.timestamp, at);
            QCOMPARE(shared->risk.curve.at(120).pnl, pnl);
        }
        refresh->click();
        QTRY_COMPARE(count(), quint64(2));
        QTRY_VERIFY(refresh->isEnabled());
        QTRY_VERIFY(window.findChild<QLabel*>("riskIv")->text().contains("65%"));
        dialog->close();
        QTest::qWait(600);
        QCOMPARE(count(), quint64(2));
    }
    void compactLayoutAndFormatting()
    {
        Window window("", Config{}, true);
        window.resize(960, 540);
        window.show();
        QTest::qWait(100);
        QCOMPARE(window.size(), QSize(960, 540));
        QVERIFY(window.findChild<QLineEdit*>("clientId")->isVisible());
        QVERIFY(window.findChild<QLineEdit*>("clientSecret")->isVisible());
        QVERIFY(window.findChild<QPushButton*>("stop")->isVisible());
        QVERIFY(window.findChild<QTableWidget*>("optionPositions")->width() > 800);
        auto* summary = window.findChild<QTableWidget*>("pnlSummary");
        QVERIFY(summary && summary->isVisible());
        QCOMPARE(summary->rowCount(), 1);
        QCOMPARE(summary->columnCount(), 6);
        QVERIFY(!window.findChild<QPushButton*>("pnlDetails"));
        QVERIFY(window.findChild<QWidget*>("pnlPanel")->y() >
                window.findChild<QWidget*>("mainTabs")->y());
        QVERIFY(window.findChild<QLabel*>("pnlPeriod")->text().contains("Since"));
        QVERIFY(!window.findChild<QLabel*>("pnlPeriod")->text().contains("first Start"));
        QVERIFY(window.findChild<QLabel*>("targetNetDelta")->text().contains("100%"));
        QVERIFY(window.findChild<QLabel*>("hedgeRemaining")->text().startsWith("To rebalance:"));
        QCOMPARE(displayNumber(0.0), QString("0"));
        QCOMPARE(displayNumber(-0.0), QString("0"));
        QCOMPARE(displayNumber(.01), QString("0.01"));
        QVERIFY(displayNumber(1e-9) != "0");
        QVERIFY(window.findChild<QLabel*>("normalizedDelta")->text().contains("Normalized delta"));
    }
    void streamingValuesKeepTheirGeometry()
    {
        Window window("", Config{}, true);
        window.show();
        QTest::qWait(100);
        const QStringList names = {
            "riskSummary",     "bidQuote",        "askQuote",        "rebalanceBand",
            "breakEvenMove",   "pnlCoverage",     "hedgeEstimate",   "netDeltaValue",
            "hedgeValue",      "hedgeUsage",      "normalizedDelta", "referenceDelta",
            "legsValue",       "tradingActivity", "latencyStatus",
            "quoteAge",        "clockDifference", "dataQuality",     "connectionStatus",
            "optionPositions", "connect",         "advanced",        "actions",
            "pnlPanel",        "executionState",  "marketPanel",     "deltaPanel",
            "hedgePanel",      "executionPanel",  "connectionPanel", "messagePanel"};
        QVector<QRect> bounds;
        for (const auto& name : names)
        {
            auto* widget = window.findChild<QWidget*>(name);
            QVERIFY(widget);
            const QRect rect(widget->mapTo(&window, QPoint()), widget->size());
            QVERIFY2(window.rect().contains(rect), qPrintable(name));
            bounds.append(rect);
        }
        auto shared = window.sharedState();
        for (int i = 0; i < 12; ++i)
        {
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                auto& s = shared->trading;
                const bool alternate = i % 2;
                s.quote = alternate ? Quote{9, 10, 9.5, 9.5, utcNow() - 4}
                                    : Quote{99999, 100001, 100000, 100000, utcNow() - 4000};
                s.running = alternate;
                s.busy = i % 3 == 0;
                s.cooldownUntil = utcNow() + (i % 3 ? 0 : 4500);
                s.inventory = 0;
                s.connected = alternate;
                s.connectionStatus = alternate ? "Connected" : "Connection error";
                s.latencyMs = alternate ? 1 : 12000;
                s.exchangeClockOffsetMs = alternate ? std::optional<qint64>(500) : std::nullopt;
                shared->analytics.pnl.options = alternate ? 0 : -1234567.89;
                shared->analytics.complete = alternate;
            }
            QTest::qWait(220);
            QCOMPARE(window.size(), QSize(960, 540));
            for (int n = 0; n < names.size(); ++n)
            {
                auto* widget = window.findChild<QWidget*>(names[n]);
                const QRect actual(widget->mapTo(&window, QPoint()), widget->size());
                QVERIFY2(actual == bounds[n], qPrintable(names[n]));
            }
            QVERIFY(window.findChild<QLabel*>("hedgeUsage")->text().startsWith("Futures target:"));
        }
        auto* band = window.findChild<QDoubleSpinBox*>("deltaChange");
        const double original = band->value();
        band->setValue(original + .01);
        auto* apply = window.findChild<QPushButton*>("applySettings");
        QTRY_VERIFY(apply->property("pendingChanges").toBool());
        QCOMPARE(apply->text(), QString("Apply"));
        QVERIFY(!window.findChild<QLabel*>("hedgeEstimate")->text().contains("Unapplied"));
        band->setValue(original);
        QTRY_VERIFY(!apply->property("pendingChanges").toBool());
    }
    void chartTicksHaveStableCentralZero()
    {
        const auto pnl = pnlAxis(100, 767.38);
        const double delta = zeroCenteredLimit(.07723);
        QCOMPARE(pnl.lower, 0.0);
        QVERIFY(pnl.upper >= 767.38);
        QCOMPARE(delta, .1);
        QCOMPARE(axisLabel(-delta / 2, delta / 2), QString("-0.05"));
        QCOMPARE(axisLabel(0, delta / 2), QString("0"));
        QCOMPARE(axisLabel(pnl.step, pnl.step), QString("250"));
        QCOMPARE(pnlAxis(50, 200, pnl).upper, pnl.upper);
        QVERIFY(pnlAxis(50, 1200, pnl).upper > pnl.upper);
        const auto losses = pnlAxis(-200, -10);
        QCOMPARE(losses.upper, 0.0);
        QVERIFY(losses.lower <= -200);
        const auto mixed = pnlAxis(-100, 767.38, pnl);
        QVERIFY(mixed.lower <= -100 && mixed.upper >= 767.38);
        QCOMPARE(roundAxisStep(7500), 10000.0);
    }
    void captureCompactDialogs()
    {
        const QString directory = qEnvironmentVariable("GAMMA_UI_QA_DIR");
        if (directory.isEmpty())
            QSKIP("Set GAMMA_UI_QA_DIR to capture normal Windows rendering");
        QDir().mkpath(directory);
        Window window("", Config{}, true);
        window.show();
        QTest::qWait(200);
        auto save = [&](QWidget* widget, const QString& name)
        { QVERIFY(widget->grab().save(directory + "/" + name + ".png")); };
        save(&window, "main");
        auto* deltaChange = window.findChild<QDoubleSpinBox*>("deltaChange");
        const double savedDelta = deltaChange->value();
        deltaChange->setValue(savedDelta + .01);
        QTest::qWait(250);
        save(&window, "pending-settings");
        deltaChange->setValue(savedDelta);
        QTest::qWait(250);
        auto* menu = window.findChild<QMenu*>("actionsMenu");
        menu->popup(window.mapToGlobal(QPoint(700, 90)));
        QTest::qWait(100);
        save(menu, "actions");
        menu->hide();
        QVERIFY(window.findChild<QAction*>("closeHedge")->text().endsWith("(Locked)"));
        QVERIFY(window.findChild<QLineEdit*>("clientId")->isVisible());
        QVERIFY(window.findChild<QLineEdit*>("clientSecret")->isVisible());
        QCOMPARE(window.findChild<QLineEdit*>("clientSecret")->echoMode(), QLineEdit::Password);
        QTimer::singleShot(150, &window,
                           [&]
                           {
                               auto* dialog = window.findChild<QDialog*>("advancedDialog");
                               save(dialog, "advanced");
                               dialog->reject();
                           });
        window.findChild<QPushButton*>("advanced")->click();
        auto shared = window.sharedState();
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->trading.inventory = 0;
        }
        QTest::qWait(250);
        QTimer::singleShot(150, &window,
                           [&]
                           {
                               auto* dialog = window.findChild<QDialog*>("optionSelection");
                               auto* type = dialog->findChild<QComboBox*>("optionTypeFilter");
                               type->setCurrentIndex(1);
                               auto* table =
                                   dialog->findChild<QTableWidget*>("optionSelectionTable");
                               QCOMPARE(table->isRowHidden(0), false);
                               QCOMPARE(table->isRowHidden(1), true);
                               save(dialog, "selection");
                               dialog->reject();
                           });
        window.findChild<QPushButton*>("selectOptions")->click();
        save(window.findChild<QWidget*>("pnlPanel"), "pnl");
        window.findChild<QPushButton*>("riskGraph")->click();
        auto* refresh = window.findChild<QPushButton*>("riskRefresh");
        QTRY_VERIFY(refresh->isEnabled());
        save(window.findChild<QDialog*>("riskDialog"), "risk-delta");
        window.findChild<QComboBox*>("scenarioGreek")->setCurrentIndex(2);
        QTest::qWait(100);
        save(window.findChild<QDialog*>("riskDialog"), "risk-theta");
        window.findChild<QDialog*>("riskDialog")->close();
        window.resize(1450, 900);
        QTest::qWait(100);
        save(&window, "expanded");
    }
    void tradingContinuesWithoutGuiAndPersistsBeforeSending()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        QString path = d.filePath("gamma.db");
        {
            RuntimeHarness h(path);
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
            QTRY_VERIFY_WITH_TIMEOUT(
                !h.snapshot().positions.isEmpty() && h.snapshot().positions[0].hasGreeks, 5000);
            h.start();
            QThread::msleep(1800); // Deliberately stop the GUI event loop.
            auto s = h.snapshot();
            QVERIFY2(h.server->mutations.load() > 0, qPrintable(s.problem));
            QVERIFY(h.server->durable.load());
            QVERIFY2(std::abs(s.inventory + .01) < 1e-8, qPrintable(s.problem));
            int count = h.server->mutations.load();
            QTest::qWait(1200);
            QCOMPARE(h.server->mutations.load(), count);
        }
        Store store(path);
        Hedger restored;
        QVector<Order> orders;
        QVERIFY(store.load(restored, orders));
        QVERIFY(std::abs(restored.inventory + .01) < 1e-8);
        QVERIFY(!restored.running);
        QVERIFY(restored.anchored);
    }
    void makerPolicies_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("requests");
        QTest::addColumn<int>("passiveAttempts");
        QTest::newRow("balanced") << int(Execution::Balanced) << 3 << 3;
        QTest::newRow("aggressive") << int(Execution::Aggressive) << 3 << 1;
        QTest::newRow("custom-balanced") << int(Execution::Balanced) << 3 << 5;
        QTest::newRow("custom-aggressive") << int(Execution::Aggressive) << 3 << 4;
    }
    void makerPolicies()
    {
        QFETCH(int, mode);
        QFETCH(int, requests);
        QFETCH(int, passiveAttempts);
        QTemporaryDir d;
        QVERIFY(d.isValid());
        RuntimeHarness h(d.filePath("core.db"), Execution(mode), true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !h.snapshot().positions.isEmpty() && h.snapshot().positions[0].hasGreeks, 5000);
        auto c = h.snapshot().config;
        if (Execution(mode) == Execution::Balanced)
            c.balancedPassiveAttempts = passiveAttempts;
        else
            c.aggressivePassiveAttempts = passiveAttempts;
        QMetaObject::invokeMethod(h.worker, [&, c] { h.worker->applyConfig(c); },
                                  Qt::BlockingQueuedConnection);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(h.snapshot().inventory + .01) < 1e-8, 15000);
        QCOMPARE(h.server->mutations.load(), requests);
        QVERIFY(h.server->durable.load());
        const auto orders = h.snapshot().orders;
        QVERIFY(orders.size() >= 2);
        QVERIFY(orders.last().ioc);
        QVERIFY(orders.last().createdAt - orders.first().createdAt >=
                passiveAttempts * c.makerWaitMs);
    }
    void lostCreateReplyIsRecoveredWithoutDuplicate()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        RuntimeHarness h(d.filePath("core.db"), Execution::PostOnly, false, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !h.snapshot().positions.isEmpty() && h.snapshot().positions[0].hasGreeks, 5000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() == 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced && h.snapshot().running &&
                                     h.snapshot().anchored &&
                                     std::abs(h.snapshot().inventory + .01) < 1e-8,
                                 20000);
        QCOMPARE(h.server->mutations.load(), 1);
    }
    void lostUnacceptedCreateIsProvedAbsentBeforeRetry_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::newRow("lost-before-acceptance") << QString();
        QTest::newRow("rate-limit-during-cancel-verification") << QString("cancel");
        QTest::newRow("rate-limit-during-history-verification") << QString("history");
    }
    void lostUnacceptedCreateIsProvedAbsentBeforeRetry()
    {
        QFETCH(QString, failure);
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("absent-create.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->dropBeforeCreate = true;
                h.server->rejectRecoveryCancel = failure == "cancel" ? 10028 : 0;
                h.server->rejectRecoveryHistory = failure == "history" ? 10028 : 0;
            },
            Qt::BlockingQueuedConnection);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().running && h.snapshot().synced &&
                                     h.snapshot().anchored &&
                                     std::abs(h.snapshot().inventory + .01) < Epsilon,
                                 30000);
        QVERIFY(h.snapshot().problem.isEmpty());
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                QCOMPARE(h.server->submitted.size(), 2);
                QCOMPARE(h.server->byLabel.size(),
                         1); // Only the verified replacement reached the book.
                QVERIFY(h.server->cancelledLabels.contains(
                    h.server->submitted.first()["label"].toString()));
                QVERIFY(h.server->submitted.last()["label"] !=
                        h.server->submitted.first()["label"]);
            },
            Qt::BlockingQueuedConnection);
    }
    void temporaryPriceOutageResumesUnlessStopped_data()
    {
        QTest::addColumn<bool>("manualStop");
        QTest::newRow("automatic-resume") << false;
        QTest::newRow("stop-cancels-resume") << true;
    }
    void temporaryPriceOutageResumesUnlessStopped()
    {
        QFETCH(bool, manualStop);
        QTemporaryDir d;
        const auto path = d.filePath("prices.db");
        RuntimeHarness h(path);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored && h.snapshot().running, 10000);
        Store core(path);
        const auto key = "pnl_start/" + h.snapshot().constructionId;
        const auto baseline = core.metadata(key);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->perpetualNotifications = false;
                h.server->stalePerpetualSnapshots = true;
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(!h.snapshot().running && h.snapshot().busy, 10000);
        QCOMPARE(h.server->mutations.load(), 1);
        if (manualStop)
        {
            QMetaObject::invokeMethod(h.worker, &TradingWorker::stopTrading);
            QTRY_VERIFY(!h.snapshot().busy);
        }
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->perpetualNotifications = true;
                h.server->stalePerpetualSnapshots = false;
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().quote.fresh(utcNow()) && !h.snapshot().busy, 10000);
        QCOMPARE(h.snapshot().running, !manualStop);
        QCOMPARE(h.snapshot().inventory, -.01);
        QCOMPARE(core.metadata(key), baseline);
        QCOMPARE(h.server->mutations.load(), 1);
    }
    void reconnectPreservesRunRequest_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("resume-hedging") << QString("hedge");
        QTest::newRow("resume-partial-close") << QString("close");
        QTest::newRow("stop-while-disconnected") << QString("stop");
        QTest::newRow("external-position-stays-stopped") << QString("external");
    }
    void reconnectPreservesRunRequest()
    {
        QFETCH(QString, action);
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("reconnect.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored && h.snapshot().running, 10000);
        if (action == "close")
        {
            QMetaObject::invokeMethod(
                h.server, [&] { h.server->holdMaker = true; }, Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(h.worker, &TradingWorker::closeHedge);
            QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() == 2, 5000);
        }
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                if (action == "close")
                    for (auto it = h.server->byLabel.begin(); it != h.server->byLabel.end(); ++it)
                        if (it.value()["order_state"] == "open")
                        {
                            h.server->fillOrder(it.key(), .004);
                            it.value()["order_state"] = "cancelled"; // Connection-scoped COD.
                            it.value()["last_update_timestamp"] = utcNow();
                        }
                h.server->holdMaker = false;
                if (action == "external")
                    h.server->position -= .002;
                for (auto* peer : h.server->peers)
                    peer->close();
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(!h.snapshot().connected, 5000);
        if (action == "stop")
            QMetaObject::invokeMethod(h.worker, &TradingWorker::stopTrading);
        if (action == "external")
        {
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("after 3 checks"), 20000);
            QVERIFY(!h.snapshot().running && !h.snapshot().synced);
            QCOMPARE(h.snapshot().inventory, -.01);
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced && !h.snapshot().busy, 20000);
            if (action == "close")
            {
                QTRY_VERIFY_WITH_TIMEOUT(
                    std::abs(h.snapshot().inventory) < Epsilon && !h.snapshot().running, 10000);
                QMetaObject::invokeMethod(
                    h.server,
                    [&]
                    {
                        QCOMPARE(h.server->submitted.size(), 3);
                        QCOMPARE(h.server->submitted.last()["amount"].toDouble(), .006);
                        QVERIFY(h.server->submitted.last()["reduce_only"].toBool());
                    },
                    Qt::BlockingQueuedConnection);
            }
            else
            {
                QCOMPARE(h.snapshot().running, action == "hedge");
                QCOMPARE(h.server->mutations.load(), 1);
            }
        }
    }
    void invalidMarketFieldsResumeAutomatically_data()
    {
        QTest::addColumn<bool>("greeks");
        QTest::newRow("null-bid") << false;
        QTest::newRow("null-greeks") << true;
    }
    void invalidMarketFieldsResumeAutomatically()
    {
        QFETCH(bool, greeks);
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("invalid-data.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored && h.snapshot().running, 10000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->invalidBid = !greeks;
                h.server->invalidGreeks = greeks;
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(!h.snapshot().running && h.snapshot().busy, 5000);
        QCOMPARE(h.server->mutations.load(), 1);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->invalidBid = h.server->invalidGreeks = false; },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().running && !h.snapshot().busy, 10000);
        QVERIFY(h.snapshot().problem.isEmpty());
        QCOMPARE(h.server->mutations.load(), 1);
    }
    void clockDriftIsRecalibratedWhileRunning()
    {
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("clock-drift.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored && h.snapshot().running, 10000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->clockLeadMs = 3500; }, Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().exchangeClockOffsetMs.value_or(0) > 2500 &&
                                     h.snapshot().running && !h.snapshot().busy,
                                 12000);
        QVERIFY(h.snapshot().quote.fresh(utcNow()));
        QVERIFY(h.snapshot().problem.isEmpty());
        QCOMPARE(h.server->mutations.load(), 1);
    }
    void wideSpreadWaitsWithoutStopping()
    {
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("spread.db"), Execution::Aggressive, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->spreadExtra = 1000; }, Qt::BlockingQueuedConnection);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().status == "Waiting for narrower spread", 10000);
        QVERIFY(h.snapshot().running);
        QCOMPARE(h.server->mutations.load(), 2); // Passive create, then cancel; no wide IOC.
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->spreadExtra = 0; }, Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored && h.snapshot().running, 10000);
        QCOMPARE(h.server->mutations.load(), 3);
    }
    void transientExchangeRejectionsRetryWithoutDuplicate_data()
    {
        QTest::addColumn<int>("code");
        QTest::newRow("rate-limit") << 10028;
        QTest::newRow("retry") << 10040;
        QTest::newRow("settlement") << 10041;
        QTest::newRow("engine-busy") << 10047;
    }
    void transientExchangeRejectionsRetryWithoutDuplicate()
    {
        QFETCH(int, code);
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("retry.db"));
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->rejectNextCreateCode = code; }, Qt::BlockingQueuedConnection);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(
            h.snapshot().running && h.snapshot().anchored && !h.snapshot().busy, 10000);
        QCOMPARE(h.snapshot().inventory, -.01);
        QCOMPARE(h.server->mutations.load(), 2);
        QMetaObject::invokeMethod(
            h.server, [&] { QCOMPARE(h.server->byLabel.size(), 1); }, Qt::BlockingQueuedConnection);
    }
    void legacyProfileRebalancesWithoutAnotherDeltaEscape()
    {
        QTemporaryDir d;
        const auto path = d.filePath("legacy.db");
        {
            Store core(path);
            auto e = fixtures::engine(utcNow());
            e.inventory = -.01;
            e.anchored = true;
            e.referenceDelta = .5;
            core.save(e, {});
            auto legacy = e.json();
            auto config = legacy["config"].toObject();
            config.remove("hedge_percent");
            config["max_notional"] = 1000;
            config["execution"] = int(Execution::PostOnly);
            legacy["config"] = config;
            core.metadata("engine", legacy);
        }
        RuntimeHarness h(path, Execution::PostOnly, false, false, true, 0, -.01);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QCOMPARE(h.snapshot().config.hedgePercent, 100.);
        QCOMPARE(h.snapshot().config.execution, Execution::Balanced);
        QVERIFY(!h.snapshot().anchored);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(h.snapshot().inventory + .05) < Epsilon, 10000);
        QCOMPARE(h.server->mutations.load(), 1);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                QCOMPARE(h.server->lastSubmitted["amount"].toDouble(), .04);
                QVERIFY(!h.server->lastSubmitted["post_only"].toBool());
            },
            Qt::BlockingQueuedConnection);
    }
    void positionSnapshotBeforeFillRecoversAutomatically()
    {
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("race.db"), Execution::PostOnly, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() == 1, 5000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->fillBeforePositionReply = true; },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(h.snapshot().inventory + .01) < Epsilon &&
                                     h.snapshot().synced && h.snapshot().running,
                                 12000);
        QCOMPARE(h.server->mutations.load(), 1);
        QVERIFY(h.snapshot().problem.isEmpty());
    }
    void fillDuringCancelRestoresCloseHedge()
    {
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("cancel-fill.db"), Execution::PostOnly, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() == 1, 5000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->fillOnCancel = true; }, Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(h.worker, &TradingWorker::closeHedge);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.snapshot().synced && h.snapshot().running && h.server->mutations.load() >= 3, 10000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                for (auto it = h.server->byLabel.begin(); it != h.server->byLabel.end(); ++it)
                    if (it.value()["order_state"] == "open")
                    {
                        QVERIFY(it.value()["reduce_only"].toBool());
                        h.server->fillOrder(it.key(), it.value()["amount"].toDouble());
                        break;
                    }
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced && !h.snapshot().running &&
                                     std::abs(h.snapshot().inventory) < Epsilon,
                                 10000);
    }
    void unexplainedLivePositionMismatchPausesWithoutReset()
    {
        QTemporaryDir d;
        const auto path = d.filePath("external-live.db");
        RuntimeHarness h(path);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().anchored, 5000);
        const auto id = h.snapshot().profileId;
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->position -= .002; }, Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().problem.contains("Position reconciliation stopped"),
                                 15000);
        QVERIFY(!h.snapshot().running && !h.snapshot().synced);
        QCOMPARE(h.snapshot().profileId, id);
        QVERIFY(std::abs(h.snapshot().inventory + .01) < Epsilon);
        QCOMPARE(h.server->mutations.load(), 1);
        Store core(path);
        QVERIFY(!core.metadata("pnl_start/" + h.snapshot().constructionId).isEmpty());
        QCOMPARE(core.coveredQuantity(h.snapshot().orders.first().id), .01);
    }
    void partialFillsDuplicatesAndStaleUpdates()
    {
        QTemporaryDir d;
        const auto path = d.filePath("partials.db");
        RuntimeHarness h(path, Execution::PostOnly, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() == 1, 5000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                const auto label = h.server->byLabel.begin().key();
                const auto stale = h.server->byLabel[label];
                h.server->fillOrder(label, .003);
                h.server->fillOrder(label, .002, 50);
                const auto duplicate = h.server->byLabel[label];
                QTimer::singleShot(100, h.server,
                                   [&, stale, duplicate]
                                   {
                                       h.server->notification(
                                           "user.changes.BTC_USDC-PERPETUAL.100ms",
                                           {{"orders", QJsonArray{duplicate, stale}}});
                                   });
                h.server->fillOrder(label, .005, 150);
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.snapshot().anchored && std::abs(h.snapshot().inventory + .01) < Epsilon, 5000);
        QTest::qWait(800);
        QCOMPARE(h.server->mutations.load(), 1);
        Store core(path);
        QCOMPARE(core.coveredQuantity(h.snapshot().orders.first().id), .01);
        QVERIFY(h.snapshot().running);
    }
    void postOnlyEditRejectionIsReconciled()
    {
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("edit-reject.db"), Execution::PostOnly, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() == 1, 5000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->rejectNextEdit = true;
                h.server->quoteShift = 1;
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.server->mutations.load() >= 3 && h.snapshot().running && h.snapshot().synced, 10000);
        QVERIFY(h.snapshot().problem.isEmpty());
    }
    void balancedUsesOrdinaryLimitsAndRetriesPartialIoc()
    {
        QTemporaryDir d;
        RuntimeHarness h(d.filePath("ioc.db"), Execution::Balanced, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->partialIoc = true; }, Qt::BlockingQueuedConnection);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(
            h.snapshot().anchored && std::abs(h.snapshot().inventory + .01) < Epsilon, 15000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                QVERIFY(!h.server->submitted.first()["post_only"].toBool());
                QCOMPARE(h.server->submitted.first()["time_in_force"].toString(),
                         QString("good_til_cancelled"));
                QVERIFY(h.server->submitted.size() > 2);
                for (int i = 1; i < h.server->submitted.size(); ++i)
                    QCOMPARE(h.server->submitted[i]["time_in_force"].toString(),
                             QString("immediate_or_cancel"));
            },
            Qt::BlockingQueuedConnection);
        QVERIFY(h.snapshot().running && h.snapshot().synced);
    }
    void partialCloseBeforeReversalKeepsReduceOnlyAmount()
    {
        QTemporaryDir d;
        const auto path = d.filePath("reverse.db");
        {
            Store core(path);
            auto e = fixtures::engine(utcNow());
            e.config.hedgePercent = 20;
            e.inventory = .01;
            core.save(e, {});
        }
        RuntimeHarness h(path, Execution::PostOnly, true, false, true, 0, .01);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() == 1, 5000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                h.server->fillOrder(h.server->byLabel.begin().key(), .004);
                h.server->quoteShift = 1;
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() >= 2, 5000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                QCOMPARE(h.server->lastEditAmount, .01);
                h.server->fillOrder(h.server->byLabel.begin().key(), .006);
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() >= 3, 5000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                QVERIFY(!h.server->lastSubmitted["reduce_only"].toBool());
                QCOMPARE(h.server->lastSubmitted["amount"].toDouble(), .01);
                for (auto it = h.server->byLabel.begin(); it != h.server->byLabel.end(); ++it)
                    if (it.value()["order_state"] == "open")
                        h.server->fillOrder(it.key(), .01);
            },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.snapshot().anchored && std::abs(h.snapshot().inventory + .01) < Epsilon, 5000);
    }
    void postOnlyNeverCrossesAndEditIncludesPartialFill()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        RuntimeHarness h(d.filePath("core.db"), Execution::PostOnly, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.snapshot().synced, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !h.snapshot().positions.isEmpty() && h.snapshot().positions[0].hasGreeks, 5000);
        h.start();
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() >= 1, 10000);
        QMetaObject::invokeMethod(
            h.server,
            [&]
            {
                auto it = h.server->byLabel.begin();
                auto o = it.value();
                o["filled_amount"] = .004;
                o["last_update_timestamp"] = utcNow();
                it.value() = o;
                h.server->position = -.004;
                h.server->notification("user.changes.BTC_USDC-PERPETUAL.100ms",
                                       {{"orders", QJsonArray{o}}});
            },
            Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(
            h.server, [&] { h.server->quoteShift = 1; }, Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(h.server->mutations.load() >= 2, 10000);
        double edited = 0;
        QMetaObject::invokeMethod(
            h.server, [&] { edited = h.server->lastEditAmount; }, Qt::BlockingQueuedConnection);
        QVERIFY(std::abs(edited - .01) < 1e-8);
        QVERIFY(std::abs(h.snapshot().inventory + .004) < 1e-8);
        QMetaObject::invokeMethod(h.worker, &TradingWorker::stopTrading);
        QTRY_VERIFY_WITH_TIMEOUT(!h.snapshot().busy && !h.snapshot().running, 5000);
        QVERIFY(h.server->durable.load());
    }
};
QTEST_MAIN(RuntimeTests)
#include "runtime_tests.moc"
