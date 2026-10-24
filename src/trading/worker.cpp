#include "trading/worker.h"
#include "analytics/model.h"
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace scalper
{
TradingWorker::TradingWorker(QString p, Config c, std::shared_ptr<SharedState> s, QUrl url)
    : path(std::move(p)), initial(std::move(c)), shared(std::move(s)), endpoint(std::move(url))
{
}
TradingWorker::~TradingWorker() = default;
bool TradingWorker::guard(const std::function<void()>& f)
{
    try
    {
        f();
        return true;
    }
    catch (const std::exception& e)
    {
        fail(QString::fromUtf8(e.what()));
        return false;
    }
}
void TradingWorker::initialize()
{
    guard(
        [&]
        {
            QDir().mkpath(QFileInfo(path).absolutePath());
            profileLock = std::make_unique<QLockFile>(path + ".lock");
            if (!profileLock->tryLock())
                throw std::runtime_error("This gamma profile is already open");
            store = std::make_unique<Store>(path);
            if (!store->load(engine, orders))
                engine.config = initial;
            if (engine.config.instrument != initial.instrument ||
                engine.config.production != initial.production)
                throw std::runtime_error(
                    "Profile instrument/environment does not match the selected startup profile");
            construction = store->metadata("construction")["id"].toString();
            selectionReview = store->metadata("selection_review")["required"].toBool();
            externalPosition = store->metadata("external_position")["quantity"].toDouble();
            if (construction.isEmpty())
            {
                construction = QUuid::createUuid().toString(QUuid::Id128);
                store->metadata("construction", {{"id", construction}});
            }
            for (auto o : engine.options)
                positions[o.spec.name] = o;
            client = new Client(this);
            client->setTradingIntervalMs(engine.config.tradingIntervalMs);
            connect(client, &Client::notice, this,
                    [this](const QString& text) { guard([&] { log(text); }); });
            connect(client, &Client::authenticatedSession, this,
                    [this] { guard([&] { initializeExchange(); }); });
            connect(client, &Client::lost, this,
                    [this]
                    {
                        guard(
                            [&]
                            {
                                ++epoch;
                                waitingData = requested != RunRequest::None;
                                recovering = false;
                                synced = false;
                                syncing = false;
                                busy = false;
                                polling = false;
                                refreshingMarket = false;
                                startPending = false;
                                resettingProfile = false;
                                engine.stop("Connection lost; reconciliation required");
                                status = waitingData ? "Reconnecting; auto-resume pending"
                                                     : "Disconnected";
                                log(waitingData ? "Connection lost; reconnecting automatically, "
                                                  "then verifying orders before resuming"
                                                : "Connection lost; reconnecting automatically");
                                save();
                                publish();
                            });
                    });
            connect(client, &Client::subscription, this,
                    [this](const QString& c, const QJsonValue& p)
                    { guard([&] { handleSubscription(c, p); }); });
            pumpTimer = new QTimer(this);
            pumpTimer->setInterval(Time::TradingPumpIntervalMs);
            connect(pumpTimer, &QTimer::timeout, this, [this] { guard([&] { pump(); }); });
            pumpTimer->start();
            publishTimer = new QTimer(this);
            publishTimer->setInterval(Time::UiRefreshIntervalMs);
            connect(publishTimer, &QTimer::timeout, this, &TradingWorker::publish);
            publishTimer->start();
            pollTimer = new QTimer(this);
            pollTimer->setInterval(Time::AccountPollIntervalMs);
            connect(pollTimer, &QTimer::timeout, this, [this] { guard([&] { poll(); }); });
            pollTimer->start();
            marketTimer = new QTimer(this);
            marketTimer->setInterval(Time::MarketRefreshIntervalMs);
            connect(marketTimer, &QTimer::timeout, this,
                    [this] { guard([&] { refreshQuietMarkets(); }); });
            marketTimer->start();
            log("Gamma profile ready. Trading starts paused; options are read-only.");
            save();
            publish();
            emit initialized();
        });
}
void TradingWorker::log(const QString& text, const QJsonObject& details)
{
    messages.append(isoUtc(utcNow()) + "  " + text);
    while (messages.size() > Limits::JournalMessages)
        messages.removeFirst();
    if (store && !storageFailed)
        store->event(text, details);
}
void TradingWorker::fail(const QString& reason)
{
    requested = RunRequest::None;
    waitingData = false;
    retryAt = 0;
    if (resettingProfile)
    {
        ++epoch;
        resettingProfile = false;
        busy = false;
    }
    if (!synced)
        syncing = false;
    startPending = false;
    recovering = false;
    syncing = false;
    engine.stop(reason);
    status = "Stopped";
    messages.append(isoUtc(utcNow()) + "  ERROR: " + reason);
    while (messages.size() > Limits::JournalMessages)
        messages.removeFirst();
    // Cancellation remains available even when durability has failed. New orders do not.
    if (store && !storageFailed)
        try
        {
            store->event("error", {{"message", reason}});
            store->save(engine, orders);
        }
        catch (...)
        {
            storageFailed = true;
        }
    publish();
}
void TradingWorker::save()
{
    if (!store || storageFailed)
        throw std::runtime_error("Durable storage unavailable");
    try
    {
        store->save(engine, orders);
    }
    catch (...)
    {
        storageFailed = true;
        throw;
    }
}
void TradingWorker::publish()
{
    TradingSnapshot s;
    s.revision = ++revision;
    s.config = engine.config;
    s.account = account;
    s.profileId = store ? store->profileId() : QString();
    s.constructionId = construction;
    s.timestamp = utcNow();
    s.lastHedgeMs = lastHedgeMs;
    s.exchangeClockOffsetMs = exchangeClockOffsetMs;
    s.status = status;
    s.problem = engine.problem;
    s.connected = client && client->authenticated();
    s.synced = synced;
    s.running = engine.running;
    s.busy = busy || startPending || (waitingData && requested != RunRequest::None) || recovering;
    s.anchored = engine.anchored;
    s.executing = engine.active || recovering;
    s.inventory = engine.inventory;
    s.externalPosition = externalPosition;
    s.positionMismatch = positionAt > 0 && engine.spec.step > 0 &&
                         std::abs(actualPosition - expectedPosition()) > engine.spec.step * .1;
    s.referenceDelta = engine.referenceDelta;
    s.strategy = engine.state;
    s.hedgeQuotes = engine.quotes;
    if (engine.quote.index > 0 && !engine.options.isEmpty())
    {
        const auto greeks = aggregate(engine.options);
        const double delta = projectedDelta(engine.options, engine.quote.index, s.timestamp);
        s.hedgeQuotes.normalized = greeks.gross > Epsilon ? delta / greeks.gross : 0;
        s.hedgeQuotes.error = engine.inventory + delta * engine.config.hedgePercent / 100;
        if (std::abs(s.hedgeQuotes.error) < Epsilon)
            s.hedgeQuotes.error = 0;
    }
    s.hedgeAlert = hedgeAlert;
    s.quote = engine.quote;
    s.margin = engine.margin;
    s.orders = orders;
    s.messages = messages;
    s.liquidation = liquidation;
    s.makerFee = accountMakerFee.value_or(engine.config.fallbackMakerFee);
    s.takerFee = accountTakerFee.value_or(engine.config.fallbackTakerFee);
    s.sessionFunding = sessionFunding;
    s.fundingBaseline = fundingBaseline;
    s.fundingAt = fundingAt;
    s.fundingBaselineSession = fundingBaselineSession;
    s.fundingValid = fundingValid;
    for (auto it = positions.begin(); it != positions.end(); ++it)
        s.positions.append(it.value());
    std::sort(s.positions.begin(), s.positions.end(),
              [](const Option& a, const Option& b) { return a.spec.name < b.spec.name; });
    if (engine.spec.step > 0 && engine.quote.mid() > 0)
    {
        s.target = engine.active ? engine.operation.target : engine.target();
    }
    s.canDelete = s.connected && accountLock && accountLock->isLocked() && !account.isEmpty() &&
                  !syncing && !recovering && requested == RunRequest::None && !busy &&
                  !startPending && !engine.running && !storageFailed;
    if (client)
    {
        const auto connection = client->connectionStatus();
        s.latencyMs = connection.milliseconds;
        s.rpcQueueMs = connection.queueMilliseconds;
        s.connectionSlow = connection.slow;
        if (s.connected)
            s.connectionStatus = s.synced               ? "Connected"
                                 : !s.problem.isEmpty() ? "Connection error"
                                                        : "Reconciling";
        else if (connection.state == Client::ConnectionStatus::State::Connecting)
            s.connectionStatus = "Connecting";
    }
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->trading = std::move(s);
}
void TradingWorker::shutdown()
{
    shuttingDown = true;
    if (pumpTimer)
        pumpTimer->stop();
    if (publishTimer)
        publishTimer->stop();
    if (pollTimer)
        pollTimer->stop();
    if (marketTimer)
        marketTimer->stop();
    startPending = false;
    requested = RunRequest::None;
    guard(
        [&]
        {
            engine.stop();
            if (store)
                save();
        });
    if (client)
        client->stop();
    clientSecret.clear();
    emit historyStop();
    store.reset();
    accountLock.reset();
    profileLock.reset();
    emit stopped();
}
} // namespace scalper
