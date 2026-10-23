#include "engine/domain.h"
#include <QTimeZone>
#include <algorithm>
#include <stdexcept>
namespace scalper
{
qint64 utcNow()
{
    return QDateTime::currentMSecsSinceEpoch();
}
QString isoUtc(qint64 v)
{
    return QDateTime::fromMSecsSinceEpoch(v, QTimeZone::utc()).toString(Qt::ISODateWithMs);
}
double number(const QJsonObject& j, const char* key)
{
    auto v = j[key];
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        throw std::runtime_error(
            QString("Missing or invalid API number: %1").arg(key).toStdString());
    return v.toDouble();
}
double roundDown(double value, double step)
{
    if (!std::isfinite(value) || !std::isfinite(step) || step <= 0 || value < 0 ||
        value / step > 9e14)
        throw std::runtime_error("Invalid quantity or quantity step");
    const double count = std::floor(value / step + 1e-8);
    const qint64 units = std::llround(step * 100000000.0);
    if (units <= 0 || std::abs(double(units) / 100000000.0 - step) > 1e-12 ||
        count > double(std::numeric_limits<qint64>::max() / units))
        throw std::runtime_error("Unsupported decimal quantity precision or overflow");
    return double(qint64(count) * units) / 100000000.0;
}
double tickPrice(double value, double tick, bool up)
{
    if (!std::isfinite(value) || tick <= 0 || value <= 0 || value / tick > 9e14)
        throw std::runtime_error("Invalid limit price or tick");
    return (up ? std::ceil(value / tick - 1e-8) : std::floor(value / tick + 1e-8)) * tick;
}
QString executionName(Execution m)
{
    return m == Execution::PostOnly   ? "POST-ONLY"
           : m == Execution::Balanced ? "Balanced"
                                      : "Aggressive";
}
QString Config::validate() const
{
    if (!Exchange::supportedHedge(instrument))
        return "Choose a BTC or ETH USDC perpetual";
    if (!std::isfinite(deltaChange) || deltaChange <= 0 || deltaChange > Limits::MaxDeltaChange)
        return QString("Delta Change must be greater than 0 and no greater than %1")
            .arg(Limits::MaxDeltaChange);
    if (!std::isfinite(hedgePercent) || hedgePercent <= 0)
        return "Set a positive delta coverage percentage";
    if (marginLimitEnabled &&
        (!std::isfinite(maxMarginUsage) || maxMarginUsage <= 0 || maxMarginUsage >= 1))
        return "Set Max Margin Usage between 0 and 100% (exclusive)";
    if (!std::isfinite(slippageBps) || slippageBps <= 0 || slippageBps > Limits::MaxSlippageBps)
        return QString("Slippage limit must be greater than 0 and no greater than %1 bps")
            .arg(Limits::MaxSlippageBps);
    if (cooldownMs < Limits::MinOperationIntervalMs || cooldownMs > Limits::MaxCooldownMs ||
        makerWaitMs < Limits::MinOperationIntervalMs || makerWaitMs > Limits::MaxMakerWaitMs)
        return "Invalid cooldown or maker attempt duration";
    if (selected.isEmpty())
        return "Select at least one open option position";
    return {};
}
QJsonObject Config::json() const
{
    return {{"instrument", instrument},
            {"production", production},
            {"selected", QJsonArray::fromStringList(selected)},
            {"delta_change", deltaChange},
            {"hedge_percent", hedgePercent},
            {"max_margin_usage", maxMarginUsage},
            {"margin_limit_enabled", marginLimitEnabled},
            {"slippage_bps", slippageBps},
            {"cooldown_ms", cooldownMs},
            {"maker_wait_ms", makerWaitMs},
            {"execution", int(execution)}};
}
Config Config::fromJson(const QJsonObject& j)
{
    Config c;
    c.instrument = j["instrument"].toString(c.instrument);
    c.production = j["production"].toBool();
    for (auto v : j["selected"].toArray())
        c.selected.append(v.toString());
    c.deltaChange = j["delta_change"].toDouble(c.deltaChange);
    c.hedgePercent = j["hedge_percent"].toDouble(c.hedgePercent);
    c.maxMarginUsage = j["max_margin_usage"].toDouble(c.maxMarginUsage);
    c.marginLimitEnabled = j["margin_limit_enabled"].toBool(c.maxMarginUsage > 0);
    c.slippageBps = j["slippage_bps"].toDouble(c.slippageBps);
    c.cooldownMs = j["cooldown_ms"].toInt(c.cooldownMs);
    c.makerWaitMs = j["maker_wait_ms"].toInt(c.makerWaitMs);
    // Old profiles migrate once to ordinary limit execution.
    int m = j.contains("hedge_percent") ? j["execution"].toInt(int(c.execution))
                                         : int(Execution::Balanced);
    if (m < int(Execution::PostOnly) || m > int(Execution::Aggressive))
        throw std::runtime_error("Invalid saved execution mode");
    c.execution = Execution(m);
    return c;
}
double Instrument::tickAt(double p) const
{
    double result = tick;
    for (auto s : tickSteps)
        if (p >= s.first)
            result = s.second;
    return result;
}
Instrument Instrument::parse(const QJsonObject& j)
{
    Instrument s;
    s.name = j["instrument_name"].toString();
    s.base = j["base_currency"].toString();
    s.settlement = j["settlement_currency"].toString();
    s.kind = j["kind"].toString();
    s.tick = number(j, "tick_size");
    s.step = number(j, "min_trade_amount");
    s.contractSize = number(j, "contract_size");
    s.strike = j["strike"].toDouble();
    s.expiry = j["expiration_timestamp"].toInteger();
    s.creation = j["creation_timestamp"].toInteger();
    s.inverse = s.kind == "option" && (s.settlement == "BTC" || s.settlement == "ETH");
    if (s.name.isEmpty() || s.tick <= 0 || s.step <= 0 || s.contractSize <= 0 ||
        (s.base != "BTC" && s.base != "ETH"))
        throw std::runtime_error("Unsupported instrument specification");
    for (auto v : j["tick_size_steps"].toArray())
    {
        auto p = v.toObject();
        double a = number(p, "above_price"), b = number(p, "tick_size");
        if (b <= 0)
            throw std::runtime_error("Invalid tick size step");
        s.tickSteps.append({a, b});
    }
    std::sort(s.tickSteps.begin(), s.tickSteps.end());
    return s;
}
QJsonObject Instrument::json() const
{
    QJsonArray a;
    for (auto p : tickSteps)
        a.append(QJsonObject{{"above_price", p.first}, {"tick_size", p.second}});
    return {{"instrument_name", name},
            {"base_currency", base},
            {"settlement_currency", settlement},
            {"kind", kind},
            {"tick_size", tick},
            {"min_trade_amount", step},
            {"contract_size", contractSize},
            {"strike", strike},
            {"expiration_timestamp", expiry},
            {"creation_timestamp", creation},
            {"tick_size_steps", a}};
}
QJsonObject Option::json() const
{
    return {{"spec", spec.json()},
            {"quantity", quantity},
            {"average_price", averagePrice},
            {"average_usd", averageUsd},
            {"delta", delta},
            {"gamma", gamma},
            {"theta", theta},
            {"vega", vega},
            {"mark", mark},
            {"index", index},
            {"underlying", underlying},
            {"iv", iv},
            {"rate", rate},
            {"ticker_at", tickerAt},
            {"call", isCall},
            {"greeks", hasGreeks}};
}
Option Option::fromJson(const QJsonObject& j)
{
    Option o;
    o.spec = Instrument::parse(j["spec"].toObject());
    o.quantity = number(j, "quantity");
    o.averagePrice = j["average_price"].toDouble();
    o.averageUsd = j["average_usd"].toDouble();
    o.delta = j["delta"].toDouble();
    o.gamma = j["gamma"].toDouble();
    o.theta = j["theta"].toDouble();
    o.vega = j["vega"].toDouble();
    o.mark = j["mark"].toDouble();
    o.index = j["index"].toDouble();
    o.underlying = j["underlying"].toDouble();
    o.iv = j["iv"].toDouble();
    o.rate = j["rate"].toDouble();
    o.tickerAt = j["ticker_at"].toInteger();
    o.isCall = j["call"].toBool();
    o.hasGreeks = j["greeks"].toBool();
    return o;
}
Greeks aggregate(const QVector<Option>& options)
{
    Greeks g;
    for (const auto& o : options)
    {
        g.delta += o.quantity * o.delta;
        g.gamma += o.quantity * o.gamma;
        g.theta += o.quantity * o.theta;
        g.vega += o.quantity * o.vega;
        g.gross += std::abs(o.quantity);
    }
    return g;
}
QVector<Option> TradingSnapshot::openOptions(qint64 now) const
{
    QVector<Option> result;
    for (const auto& option : positions)
        if (option.isOpen(now))
            result.append(option);
    return result;
}
Margin Margin::parse(const QJsonObject& j, qint64 now)
{
    Margin m;
    m.at = now;
    QString model = j["margin_model"].toString();
    bool cross =
        model.startsWith("cross") || (model.isEmpty() && (j["cross_collateral_enabled"].toBool() ||
                                                          j.contains("total_margin_balance_usd")));
    m.model = model.isEmpty() ? QString(cross ? "cross_" : "segregated_") +
                                    (j["portfolio_margining_enabled"].toBool() ? "pm" : "sm")
                              : model;
    m.scope = cross ? "Whole cross-collateral account (USD)" : "Settlement account (USDC)";
    m.balance = number(j, cross ? "total_margin_balance_usd" : "margin_balance");
    m.maintenance = number(j, cross ? "total_maintenance_margin_usd" : "maintenance_margin");
    m.available = number(j, "available_funds");
    m.valid = m.balance > 0 && m.maintenance >= 0 && m.available >= 0;
    return m;
}
QJsonObject Order::json() const
{
    return {{"label", label},           {"id", id},
            {"instrument", instrument}, {"buy", buy},
            {"post_only", postOnly},    {"ioc", ioc},
            {"state", state},           {"reduce_only", reduceOnly},
            {"terminal", terminal},     {"uncertain", uncertain},
            {"price", price},           {"total", total},
            {"filled", filled},         {"updated", updated},
            {"attempt_at", attemptAt},  {"created_at", createdAt},
            {"mutation", mutation},     {"request", request}};
}
Order Order::fromJson(const QJsonObject& j)
{
    Order o;
    o.label = j["label"].toString();
    o.id = j["id"].toString();
    o.instrument = j["instrument"].toString();
    o.buy = j["buy"].toBool();
    o.postOnly = j["post_only"].toBool(true);
    o.ioc = j["ioc"].toBool(!o.postOnly);
    o.state = j["state"].toString(j["terminal"].toBool() ? "closed" : "open");
    o.reduceOnly = j["reduce_only"].toBool();
    o.terminal = j["terminal"].toBool();
    o.uncertain = j["uncertain"].toBool();
    o.price = number(j, "price");
    o.total = number(j, "total");
    o.filled = number(j, "filled");
    o.updated = j["updated"].toInteger();
    o.attemptAt = j["attempt_at"].toInteger();
    o.createdAt = j["created_at"].toInteger(o.attemptAt);
    o.mutation = j["mutation"].toString();
    o.request = j["request"].toObject();
    if (o.label.isEmpty() || o.total < 0 || o.filled < 0 || o.filled > o.total + Epsilon)
        throw std::runtime_error("Invalid saved order");
    return o;
}
Pnl Pnl::operator-(const Pnl& p) const
{
    return {options - p.options,       realized - p.realized,   unrealized - p.unrealized,
            optionFees - p.optionFees, hedgeFees - p.hedgeFees, funding - p.funding};
}
QJsonObject Pnl::json() const
{
    return {{"options", options},
            {"realized", realized},
            {"unrealized", unrealized},
            {"option_fees", optionFees},
            {"hedge_fees", hedgeFees},
            {"funding", funding},
            {"hedge_net", realized + unrealized - hedgeFees + funding},
            {"options_net", options - optionFees},
            {"net", net()}};
}
Pnl Pnl::fromJson(const QJsonObject& j)
{
    return {j["options"].toDouble(),     j["realized"].toDouble(),   j["unrealized"].toDouble(),
            j["option_fees"].toDouble(), j["hedge_fees"].toDouble(), j["funding"].toDouble()};
}
} // namespace scalper
