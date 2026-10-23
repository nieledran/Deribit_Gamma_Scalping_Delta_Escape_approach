#include "trading/worker.h"
#include <stdexcept>
namespace scalper
{
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
            const bool accepted = guard(
                [&]
                {
                    if (!result.ok)
                        throw std::runtime_error(QString("Market snapshot unavailable for %1: %2")
                                                     .arg(name, result.error)
                                                     .toStdString());
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
                        const qint64 age = now - value["timestamp"].toInteger();
                        QString reason;
                        if (value["timestamp"].toInteger() <= 0)
                            reason = "Exchange sent a quote without a valid timestamp for " + name;
                        else if (age < -Time::MarketClockLeadToleranceMs)
                            reason = QString("Quote for %1 is %2 ms ahead of PC time. Synchronize "
                                             "Windows time and reconnect.")
                                         .arg(name)
                                         .arg(-age);
                        else if (age > Time::MarketDataMaxAgeMs)
                            reason = QString("No fresh prices for %1: last quote is %2 s old. "
                                             "Trading paused; reconnect if this persists.")
                                         .arg(name)
                                         .arg(age / 1000.0, 0, 'f', 1);
                        else if (name == engine.config.instrument)
                            reason = "No valid bid/ask for " + name +
                                     ". Waiting for tradable prices; trading paused.";
                        else
                            reason = "Selected option data unavailable for " + name +
                                     ". Waiting for valid Greeks; trading paused.";
                        throw std::runtime_error(reason.toStdString());
                    }
                });
            if (!accepted)
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
                            exchangeClockOffsetMs = time.value.toInteger() - (sent + received) / 2;
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
                return;
            }
            refreshMarketData(std::move(names), std::move(done));
        });
}
void TradingWorker::refreshQuietMarkets()
{
    if (!client || !client->authenticated() || !synced || syncing || refreshingMarket ||
        startPending)
        return;
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
