#include "engine/hedger.h"
#include <algorithm>
#include <stdexcept>
namespace scalper
{
QString Hedger::dataProblem(qint64 now, bool requireOptions) const
{
    if (!quote.fresh(now))
        return "Perpetual prices are older than 5 s or bid/ask is invalid. Trading paused.";
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
    if (!reason.isEmpty())
        problem = reason;
}
double Hedger::target() const
{
    if (flattening)
        return 0;
    if (quote.mid() <= 0 || spec.step <= 0)
        return inventory;
    const double desired = -aggregate(options).delta * config.hedgePercent / 100;
    return std::copysign(roundDown(std::abs(desired), spec.step), desired);
}
std::optional<Hedge> Hedger::evaluate(qint64 now)
{
    if (!running || active)
        return {};
    if (!flattening)
    {
        auto error = safety(now);
        if (!error.isEmpty())
        {
            stop(error);
            return {};
        }
    }
    else if (!quote.fresh(now))
    {
        stop("Cannot close hedge without a fresh quote");
        return {};
    }
    auto g = aggregate(options);
    bool initial = !anchored;
    if (!flattening && !initial)
    {
        if (now < completedAt || now - completedAt < config.cooldownMs)
            return {};
        if (std::abs(g.normalized() - referenceDelta) + Epsilon < config.deltaChange)
            return {};
    }
    operation = {target(), g.delta, g.normalized(), initial};
    active = true;
    if (std::abs(remaining()) < spec.step * .5)
    {
        complete(now);
        return {};
    }
    return operation;
}
double Hedger::remaining() const
{
    return operation.target - inventory;
}
void Hedger::complete(qint64 now)
{
    if (!flattening)
    {
        referenceDelta = operation.normalized;
        anchored = true;
    }
    completedAt = now;
    active = false;
    if (flattening)
    {
        flattening = false;
        running = false;
    }
}
bool Hedger::obsolete() const
{
    if (!active || flattening || operation.initial)
        return false;
    auto g = aggregate(options);
    return std::abs(g.normalized() - referenceDelta) + Epsilon < config.deltaChange ||
           (target() - inventory) * remaining() < -Epsilon;
}
QJsonObject Hedger::json() const
{
    QJsonArray a;
    for (const auto& o : options)
        a.append(o.json());
    return {{"version", 1},
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
    if (j["version"].toInt() != 1)
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
    for (auto v : j["options"].toArray())
        options.append(Option::fromJson(v.toObject()));
    running = false; // A restart always requires an explicit Start.
}
} // namespace scalper
