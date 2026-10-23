#include "trading/worker.h"
#include <stdexcept>
namespace scalper
{
void TradingWorker::waitForData(const QString& reason)
{
    const bool changed = !waitingData || engine.problem != reason;
    waitingData = true;
    engine.stop(reason);
    desiredClose = false;
    retryAt = utcNow() + Exchange::MinimumTradingIntervalMs;
    status = requested == RunRequest::None ? "Waiting for data" : "Waiting; auto-resume pending";
    if (requestId)
        client->cancelQueued(requestId);
    if (changed)
    {
        const QString quoteState =
            engine.quote.fresh(utcNow()) ? QString()
            : engine.quote.at <= 0
                ? QString(" No valid perpetual quote available.")
                : QString(" Quote age %1 ms after clock adjustment; bid %2, ask %3.")
                      .arg(utcNow() - engine.quote.at)
                      .arg(engine.quote.bid, 0, 'g', 12)
                      .arg(engine.quote.ask, 0, 'g', 12);
        log("Temporary pause: " + reason +
                (requested != RunRequest::None || startPending ? " Automatic resumption pending."
                                                               : QString()) +
                quoteState,
            {{"quote_at", engine.quote.at},
             {"bid", engine.quote.bid},
             {"ask", engine.quote.ask},
             {"exchange_clock_offset_ms", exchangeClockOffsetMs.value_or(0)}});
    }
    save();
    publish();
}
bool TradingWorker::acceptRead(const RpcResult& result, const QString& context)
{
    if (result.ok)
        return true;
    if (!result.retryable())
    {
        fail(context + ": " + result.error);
        return false;
    }
    waitForData(context + ": " + result.error);
    retryAt = utcNow() + Time::SecondMs;
    if (!synced)
        client->reconnect(); // Restart an interrupted connection/reconciliation sequence.
    return false;
}
bool TradingWorker::resumeRequested()
{
    if (!waitingData || utcNow() < retryAt || !synced || syncing || busy || liveOrder() ||
        storageFailed || foreignOrders || selectionReview || startPending)
        return false;
    const bool closing = requested == RunRequest::Close;
    const qint64 now = utcNow();
    if ((!closing && !engine.dataProblem(now).isEmpty()) || (closing && !engine.quote.fresh(now)))
        return false;
    if (now - positionAt > Time::MarketDataMaxAgeMs ||
        std::abs(actualPosition - expectedPosition()) > engine.spec.step * .1)
    {
        poll(); // Only recovery needs an extra account refresh, never the normal price loop.
        return false;
    }
    if (requested == RunRequest::Hedge && !engine.start(now))
    {
        fail(engine.problem);
        return false;
    }
    if (closing)
    {
        engine.running = engine.flattening = true;
        engine.active = false;
    }
    waitingData = false;
    engine.problem.clear();
    status = closing ? "Closing hedge" : engine.running ? "Running" : "Paused";
    attempts = 0;
    save();
    log(requested == RunRequest::None ? "Data restored; trading remains paused"
                                      : "Data and account verified; trading resumed automatically");
    publish();
    return engine.running;
}
QStringList TradingWorker::marketInstruments(bool onlyStale) const
{
    const qint64 now = utcNow();
    QStringList names;
    if (!onlyStale || !engine.quote.fresh(now) || now - engine.quote.at >= Time::MarketRefreshAgeMs)
        names.append(engine.config.instrument);
    if (engine.flattening)
        return names;
    for (const auto& name : engine.config.selected)
    {
        const auto it = positions.constFind(name);
        if (it == positions.cend() || !it->isOpen(now))
            continue;
        if (!onlyStale || !it->freshGreeks(now) || now - it->tickerAt >= Time::MarketRefreshAgeMs)
            if (!names.contains(name))
                names.append(name);
    }
    return names;
}
void TradingWorker::refreshMarketData(QStringList names, std::function<void(bool)> done)
{
    if (names.isEmpty())
    {
        done(true);
        return;
    }
    const QString name = names.takeFirst();
    const int generation = epoch;
    client->call(
        "public/ticker", {{"instrument_name", name}},
        [this, generation, name, names = std::move(names),
         done = std::move(done)](const RpcResult& result) mutable
        {
            if (generation != epoch)
                return;
            if (!acceptRead(result, "Market snapshot unavailable for " + name))
            {
                done(false);
                return;
            }
            const bool accepted = guard(
                [&]
                {
                    const auto value = result.value.toObject();
                    if (value["instrument_name"].toString() != name)
                        throw std::runtime_error("Market snapshot instrument mismatch");
                    ticker(value);
                    const qint64 now = utcNow();
                    const bool fresh =
                        name == engine.config.instrument
                            ? engine.quote.fresh(now)
                            : positions.contains(name) && positions[name].freshGreeks(now);
                    if (!fresh)
                    {
                        waitForData("Waiting for fresh, valid market data for " + name);
                    }
                });
            const bool fresh =
                name == engine.config.instrument
                    ? engine.quote.fresh(utcNow())
                    : positions.contains(name) && positions[name].freshGreeks(utcNow());
            if (!accepted || !fresh)
            {
                const qint64 sent = utcNow();
                client->call(
                    "public/get_time", {},
                    [this, generation, sent](const RpcResult& time)
                    {
                        if (generation != epoch)
                            return;
                        const qint64 received = utcNow();
                        if (time.ok && time.value.isDouble())
                        {
                            const qint64 offset = time.value.toInteger() - (sent + received) / 2;
                            if (std::abs(offset - exchangeClockOffsetMs.value_or(0)) >
                                Time::MarketClockLeadToleranceMs / 2)
                            {
                                engine.quote.at = 0;
                                for (auto& option : positions)
                                    option.tickerAt = 0;
                            }
                            exchangeClockOffsetMs = offset;
                            log(QString("Clock check: exchange %1 ms %2 computer (request round "
                                        "trip %3 ms)")
                                    .arg(std::abs(*exchangeClockOffsetMs))
                                    .arg(*exchangeClockOffsetMs >= 0 ? "ahead of" : "behind")
                                    .arg(received - sent));
                        }
                        else
                            log("Exchange clock check unavailable");
                    });
                done(false);
                if (accepted && !synced)
                    client->reconnect();
                return;
            }
            refreshMarketData(std::move(names), std::move(done));
        });
}
void TradingWorker::refreshQuietMarkets()
{
    if (!client || !client->authenticated() || !synced || syncing || refreshingMarket)
        return;
    if (startPending)
    {
        startMarketRefresh();
        return;
    }
    auto names = marketInstruments(true);
    if (names.isEmpty())
        return;
    refreshingMarket = true;
    refreshMarketData(std::move(names),
                      [this](bool)
                      {
                          refreshingMarket = false;
                          startMarketRefresh();
                      });
}
} // namespace scalper
