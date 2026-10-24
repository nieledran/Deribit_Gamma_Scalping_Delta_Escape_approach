#include "engine/hedger.h"
#include "analytics/model.h"
#include <algorithm>
#include <stdexcept>
namespace scalper
{
QString Hedger::dataProblem(qint64 now, bool requireOptions) const
{
    if (!quote.fresh(now))
        return "Perpetual prices are older than 5 s or bid/ask is invalid. Trading paused.";
    if (requireOptions && (!std::isfinite(quote.index) || quote.index <= 0))
        return "A valid perpetual index price is required for resting hedge levels";
    if (!margin.valid || margin.at <= 0 || now < margin.at ||
        now - margin.at > Time::MarginDataMaxAgeMs)
        return "Margin data is stale or unavailable";
    if (requireOptions)
        for (const auto& o : options)
            if (!o.freshGreeks(now))
                return "Selected option market data is stale or unavailable";
    return {};
}
QString Hedger::safety(qint64 now, bool requireOptions) const
{
    if (!quote.fresh(now))
        return "Perpetual prices are older than 5 s or bid/ask is invalid. Trading paused.";
    if (spec.step <= 0 || spec.tick <= 0 || spec.name != config.instrument ||
        spec.settlement != "USDC")
        return "Hedge instrument is not ready";
    if (!margin.valid || margin.at <= 0 || now < margin.at ||
        now - margin.at > Time::MarginDataMaxAgeMs)
        return "Margin data is stale or unavailable";
    if (config.marginLimitEnabled && margin.usage() >= config.maxMarginUsage)
        return "Max Margin Usage reached: Stop & Cancel";
    if (!requireOptions)
        return {};
    if (options.isEmpty() || aggregate(options).gross <= Epsilon)
        return "No selected option exposure";
    for (const auto& o : options)
    {
        if (o.spec.base != config.base() || o.spec.expiry <= 0)
            return "Selected option specification is invalid";
        if (now >= o.spec.expiry - Time::ExpiryPauseMs)
            return "Expiry window: trading paused; hedge remains open";
        if (!o.freshGreeks(now))
            return "Selected option market data is stale or unavailable";
    }
    if (aggregate(options).gamma <= 0)
        return "Long-gamma strategy requires positive net option gamma; hedge retained";
    if (!std::isfinite(quote.index) || quote.index <= 0)
        return "A valid perpetual index price is required for resting hedge levels";
    return {};
}
bool Hedger::start(qint64 now)
{
    problem = config.validate();
    if (problem.isEmpty())
        problem = safety(now);
    if (!problem.isEmpty())
        return false;
    if (aggregate(options).gross * config.deltaChange * config.hedgePercent / 100 + Epsilon <
        spec.step)
    {
        problem = "Delta Change implies a hedge below the minimum order amount; increase the band";
        return false;
    }
    running = true;
    flattening = false;
    return true;
}
void Hedger::stop(const QString& reason)
{
    running = false;
    flattening = false;
    active = false;
    state = StrategyState::Paused;
    quotes = {};
    quotes.buy.buy = true;
    if (!reason.isEmpty())
        problem = reason;
}
double Hedger::target() const
{
    if (flattening)
        return 0;
    if (quote.mid() <= 0 || spec.step <= 0)
        return inventory;
    const double desired = -projectedDelta(options, quote.index, quote.at) * config.hedgePercent / 100;
    return std::copysign(roundDown(std::abs(desired), spec.step), desired);
}
double Hedger::reference() const
{
    const double scale = aggregate(options).gross * config.hedgePercent / 100;
    return scale > Epsilon ? -inventory / scale : 0;
}
QString strategyName(StrategyState state)
{
    switch (state)
    {
    case StrategyState::InitialHedge: return "Initial hedge";
    case StrategyState::Quoting: return "Resting hedge orders";
    case StrategyState::Corrective: return "Passive correction";
    case StrategyState::Closing: return "Closing hedge";
    default: return "Paused";
    }
}
QJsonObject HedgeQuote::json() const
{
    return {{"role", role}, {"reason", reason}, {"buy", buy}, {"enabled", enabled},
            {"price", price}, {"amount", amount}, {"target", target}, {"normalized", normalized}};
}
void Hedger::plan(qint64 now)
{
    quotes = {};
    quotes.buy.buy = true;
    quotes.buy.role = "buy_level";
    quotes.sell.role = "sell_level";
    if (!running)
    {
        state = StrategyState::Paused;
        return;
    }
    const QString error = flattening ? (quote.fresh(now) ? QString() : "Cannot close hedge without a fresh quote") : safety(now);
    if (!error.isEmpty())
    {
        stop(error);
        return;
    }
    const auto g = aggregate(options);
    const double scale = g.gross * config.hedgePercent / 100;
    referenceDelta = reference();
    quotes.normalized = g.gross > Epsilon ? projectedDelta(options, quote.index, now) / g.gross : 0;
    quotes.error = inventory + quotes.normalized * scale;
    if (std::abs(quotes.error) < Epsilon)
        quotes.error = 0;
    const double desired = flattening ? 0 : -quotes.normalized * scale;
    const double currentTarget = std::copysign(roundDown(std::abs(desired), spec.step), desired);
    const bool outside = scale > Epsilon && std::abs(quotes.error) > scale * config.deltaChange + Epsilon;
    if (flattening || !anchored || state == StrategyState::Corrective || outside)
    {
        state = flattening ? StrategyState::Closing : !anchored ? StrategyState::InitialHedge
                                                               : StrategyState::Corrective;
        operation = {currentTarget, quotes.normalized * g.gross, quotes.normalized, !anchored};
        active = true;
        const double difference = currentTarget - inventory;
        if (std::abs(difference) < spec.step * .5)
        {
            complete(now);
            if (!running)
                return;
        }
        else
        {
            auto& side = difference > 0 ? quotes.buy : quotes.sell;
            side.buy = difference > 0;
            side.enabled = true;
            side.role = flattening ? "close" : !anchored ? "initial" : "corrective";
            side.target = currentTarget;
            side.amount = roundDown(std::abs(difference), spec.step);
            side.price = tickPrice(side.buy ? quote.bid : quote.ask,
                                   spec.tickAt(side.buy ? quote.bid : quote.ask), !side.buy);
            return;
        }
    }
    active = false;
    state = StrategyState::Quoting;
    for (auto* side : {&quotes.buy, &quotes.sell})
    {
        side->normalized = referenceDelta + (side->buy ? -1 : 1) * config.deltaChange;
        auto level = deltaLevel(options, side->normalized, quote, now, !side->buy);
        if (!level)
        {
            side->reason = side->buy ? "No reachable buy level" : "No reachable sell level";
            continue;
        }
        side->price = tickPrice(*level, spec.tickAt(*level), !side->buy);
        const double index = side->price - (quote.mid() - quote.index);
        const double position = -projectedDelta(options, index, now) * config.hedgePercent / 100;
        side->target = std::copysign(roundDown(std::abs(position), spec.step), position);
        const double difference = side->target - inventory;
        side->amount = roundDown(std::abs(difference), spec.step);
        if (side->amount < spec.step * .5 || (difference > 0) != side->buy)
            side->reason = "Level has no valid hedge quantity";
        else if ((side->buy && side->price >= quote.bid) || (!side->buy && side->price <= quote.ask))
            side->reason = "Delta Change too tight for the current spread";
        else
            side->enabled = true;
    }
}
double Hedger::remaining() const
{
    return operation.target - inventory;
}
void Hedger::complete(qint64 now)
{
    if (!flattening)
    {
        referenceDelta = reference();
        anchored = true;
    }
    completedAt = now;
    active = false;
    state = StrategyState::Quoting;
    if (flattening)
    {
        flattening = false;
        running = false;
        state = StrategyState::Paused;
    }
}
QJsonObject Hedger::json() const
{
    QJsonArray a;
    for (const auto& o : options)
        a.append(o.json());
    return {{"version", 2},
            {"strategy_state", int(state)},
            {"buy_quote", quotes.buy.json()},
            {"sell_quote", quotes.sell.json()},
            {"config", config.json()},
            {"inventory", inventory},
            {"reference_delta", referenceDelta},
            {"anchored", anchored},
            {"completed_at", completedAt},
            {"active", active},
            {"flattening", flattening},
            {"operation_target", operation.target},
            {"operation_delta", operation.delta},
            {"operation_normalized", operation.normalized},
            {"operation_initial", operation.initial},
            {"options", a}};
}
void Hedger::restore(const QJsonObject& j)
{
    if (j["version"].toInt() != 1 && j["version"].toInt() != 2)
        throw std::runtime_error(
            "Unsupported gamma profile version; grid profiles cannot be imported");
    config = Config::fromJson(j["config"].toObject());
    inventory = number(j, "inventory");
    if (std::abs(inventory) < Epsilon)
        inventory = 0;
    referenceDelta = number(j, "reference_delta");
    // Removing the old cap changes the target even if option delta has not moved.
    anchored = j["config"].toObject().contains("hedge_percent") && j["anchored"].toBool();
    completedAt = j["completed_at"].toInteger();
    active = j["active"].toBool();
    flattening = j["flattening"].toBool();
    operation = {number(j, "operation_target"), number(j, "operation_delta"),
                 number(j, "operation_normalized"), j["operation_initial"].toBool()};
    options.clear();
    for (auto v : j["options"].toArray())
        options.append(Option::fromJson(v.toObject()));
    running = false; // A restart always requires an explicit Start.
    active = flattening = false;
    state = StrategyState::Paused;
}
} // namespace scalper
