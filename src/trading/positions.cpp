#include "trading/worker.h"
#include <QSet>
#include <algorithm>
#include <stdexcept>
namespace scalper
{
void TradingWorker::loadPositions(std::function<void()> done, bool reconcileRead)
{
    int generation = epoch;
    quint64 before = positionRevision;
    client->call(
        "private/get_positions", {{"currency", "any"}, {"kind", "option"}},
        [this, generation, before, done = std::move(done),
         reconcileRead](const RpcResult& r) mutable
        {
            if (!guard(
                    [&]
                    {
                        if (generation != epoch)
                            return;
                        if (!r.ok)
                            throw std::runtime_error(r.error.toStdString());
                        QSet<QString> seen;
                        QStringList metadata;
                        if (!r.value.isArray())
                            throw std::runtime_error("Invalid option positions response");
                        for (const auto& value : r.value.toArray())
                        {
                            auto j = value.toObject();
                            QString name = j["instrument_name"].toString();
                            if (!(name.startsWith(engine.config.base() + "-") ||
                                  name.startsWith(engine.config.base() + "_USDC-")))
                                continue;
                            double qty = number(j, "size");
                            if (j["direction"] == "sell")
                                qty = -std::abs(qty);
                            else if (j["direction"] == "buy")
                                qty = std::abs(qty);
                            if (std::abs(qty) < Epsilon)
                                continue;
                            seen.insert(name);
                            bool known =
                                positions.contains(name) && positions[name].spec.kind == "option";
                            auto& o = positions[name];
                            if (known && engine.config.selected.contains(name) &&
                                (std::abs(o.quantity - qty) > Epsilon ||
                                 std::abs(o.averagePrice - j["average_price"].toDouble()) > 1e-9))
                            {
                                selectionReview = true;
                                store->metadata("selection_review", {{"required", true}});
                                fail("Selected option changed externally; confirm the basket with "
                                     "Apply Settings");
                            }
                            if (!reconcileRead && engine.config.selected.contains(name) &&
                                std::abs(o.quantity - qty) > Epsilon)
                                fail(
                                    "Selected option quantity changed externally; review selection "
                                    "and Apply Settings");
                            o.spec.name = name;
                            o.quantity = qty;
                            o.averagePrice = j["average_price"].toDouble();
                            o.averageUsd = j["average_price_usd"].toDouble();
                            metadata.append(name);
                        }
                        for (auto it = positions.begin(); it != positions.end(); ++it)
                            if (!seen.contains(it.key()) && std::abs(it->quantity) > Epsilon)
                            {
                                if (engine.config.selected.contains(it.key()))
                                {
                                    selectionReview = true;
                                    store->metadata("selection_review", {{"required", true}});
                                }
                                if (!reconcileRead && engine.config.selected.contains(it.key()))
                                    fail("Selected option closed or expired; hedge remains open");
                                it->quantity = 0;
                            }
                        loadMetadata(
                            metadata,
                            [this, generation, before, done = std::move(done)]() mutable
                            {
                                if (generation != epoch)
                                    return;
                                updateSelected();
                                client->call(
                                    "private/get_position",
                                    {{"instrument_name", engine.config.instrument}},
                                    [this, generation, before,
                                     done = std::move(done)](const RpcResult& p) mutable
                                    {
                                        if (!guard(
                                                [&]
                                                {
                                                    if (generation != epoch)
                                                        return;
                                                    if (!p.ok)
                                                        throw std::runtime_error(
                                                            p.error.toStdString());
                                                    auto j = p.value.toObject();
                                                    actualPosition = number(j, "size_currency");
                                                    if (j["direction"] == "sell")
                                                        actualPosition = -std::abs(actualPosition);
                                                    else if (j["direction"] == "buy")
                                                        actualPosition = std::abs(actualPosition);
                                                    positionAt = utcNow();
                                                    liquidation =
                                                        j["estimated_liquidation_price"].toDouble();
                                                    fundingValid = j["realized_funding"].isDouble();
                                                    if (fundingValid)
                                                    {
                                                        sessionFunding =
                                                            number(j, "realized_funding");
                                                        fundingAt = positionAt;
                                                        auto baseline = store->metadata(
                                                            "funding/" + construction);
                                                        if (baseline.isEmpty() &&
                                                            std::abs(engine.inventory) < Epsilon &&
                                                            !engine.anchored)
                                                        {
                                                            qint64 boundary =
                                                                Time::fundingSessionStart(
                                                                    positionAt);
                                                            baseline = {{"session", boundary},
                                                                        {"value", sessionFunding}};
                                                            store->metadata("funding/" +
                                                                                construction,
                                                                            baseline);
                                                        }
                                                        fundingBaseline =
                                                            baseline["value"].toDouble();
                                                        fundingBaselineSession =
                                                            baseline["session"].toInteger();
                                                    }
                                                    if (synced && before == positionRevision &&
                                                        std::abs(actualPosition - expectedPosition()) >
                                                            engine.spec.step * .1)
                                                        requestRecovery("Position snapshot differs from known fills");
                                                    save();
                                                    done();
                                                }))
                                            polling = false;
                                    },
                                    false, true);
                            });
                    }))
                polling = false;
        });
}
void TradingWorker::ticker(const QJsonObject& j)
{
    QString name = j["instrument_name"].toString();
    qint64 at = j["timestamp"].toInteger();
    qint64 now = utcNow();
    if (at <= 0 || std::abs(now - at) > Time::TickerTimestampToleranceMs)
        return;
    if (name == engine.config.instrument)
    {
        if (at < engine.quote.at)
            return;
        double bid = number(j, "best_bid_price"), ask = number(j, "best_ask_price");
        if (bid <= 0 || ask < bid)
            return;
        engine.quote = {bid, ask, j["mark_price"].toDouble((bid + ask) / 2),
                        j["index_price"].toDouble(), at};
        return;
    }
    auto it = positions.find(name);
    if (it == positions.end())
        return;
    auto& o = it.value();
    if (at < o.tickerAt)
        return;
    auto g = j["greeks"].toObject();
    o.delta = number(g, "delta");
    o.gamma = number(g, "gamma");
    o.theta = number(g, "theta");
    o.vega = number(g, "vega");
    o.mark = number(j, "mark_price");
    o.index = number(j, "index_price");
    o.underlying = number(j, "underlying_price");
    o.iv = number(j, "mark_iv") / 100;
    o.rate = j["interest_rate"].toDouble();
    if (o.mark < 0 || o.index <= 0 || o.underlying <= 0 || o.iv < 0 ||
        std::abs(o.delta) > 1.000001 || o.gamma < 0 || o.vega < 0)
        throw std::runtime_error("Invalid option mark or per-unit Greeks");
    o.tickerAt = at;
    o.hasGreeks = true;
    updateSelected();
}
void TradingWorker::updateSelected()
{
    pruneClosedOptions();
    engine.options.clear();
    for (const auto& name : engine.config.selected)
        if (positions.contains(name))
            engine.options.append(positions[name]);
}
void TradingWorker::pruneClosedOptions()
{
    const qint64 now = utcNow();
    for (auto it = positions.begin(); it != positions.end();)
    {
        if (!engine.config.selected.contains(it.key()) && it->spec.kind == "option" &&
            !it->isOpen(now))
        {
            instruments.remove(it.key());
            it = positions.erase(it);
        }
        else
            ++it;
    }
}
void TradingWorker::handleSubscription(const QString& channel, const QJsonValue& payload)
{
    if (channel.startsWith("ticker."))
    {
        ticker(payload.toObject());
        return;
    }
    auto j = payload.toObject();
    QString instrument = channel.mid(QString("user.changes.").size()).section('.', 0, 0);
    if (instrument == engine.config.instrument)
    {
        for (auto o : j["orders"].toArray())
            receiveOrder(o.toObject());
        receiveTrades(j["trades"].toArray());
    }
    else
    {
        if (engine.config.selected.contains(instrument) && !j["trades"].toArray().isEmpty())
        {
            selectionReview = true;
            store->metadata("selection_review", {{"required", true}});
            fail("Selected option traded externally; review quantity and cost before Apply "
                 "Settings");
        }
        for (auto p : j["positions"].toArray())
        {
            auto v = p.toObject();
            QString name = v["instrument_name"].toString();
            if (!engine.config.selected.contains(name))
                continue;
            double qty = number(v, "size");
            if (v["direction"] == "sell")
                qty = -std::abs(qty);
            if (positions.contains(name) && std::abs(positions[name].quantity - qty) > Epsilon)
            {
                selectionReview = true;
                store->metadata("selection_review", {{"required", true}});
                positions[name].quantity = qty;
                updateSelected();
                fail("Selected option quantity changed externally; confirm with Apply Settings");
            }
        }
    }
}
void TradingWorker::poll()
{
    if (!synced || syncing || polling || !client->authenticated())
        return;
    polling = true;
    refreshMargin(
        [this](bool ok)
        {
            if (!ok)
            {
                polling = false;
                return;
            }
            loadPositions(
                [this]
                {
                    polling = false;
                    subscribe([] {});
                });
        });
}
} // namespace scalper
