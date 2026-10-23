#include "analytics/model.h"
#include <algorithm>
#include <numbers>
namespace scalper
{
static double cdf(double x)
{
    return .5 * std::erfc(-x / std::numbers::sqrt2);
}
ModelValue optionModel(const Option& o, double spot, qint64 now)
{
    if (spot <= 0 || o.index <= 0 || o.spec.strike <= 0)
        return {};
    double t = std::max(0.0, double(o.spec.expiry - now) / (365.0 * Time::DayMs));
    double carry = o.underlying > 0 && t > 0 ? std::log(o.underlying / o.index) / t : o.rate;
    // Inverse Greeks use spot with fixed carry; linear Greeks use the expiry
    // forward, shifted in parallel with the underlying (fixed absolute basis).
    double forward = o.spec.inverse ? spot * std::exp(carry * t)
                                    : std::max(1e-8, o.underlying + spot - o.index),
           k = o.spec.strike;
    if (t <= 1e-12 || o.iv <= 0)
    {
        double d = o.isCall ? (forward > k ? 1.0 : 0.0) : (forward < k ? -1.0 : 0.0);
        double v = std::max(0.0, o.isCall ? forward - k : k - forward);
        return {o.spec.inverse ? v * std::exp(-carry * t) : v, d};
    }
    double vt = o.iv * std::sqrt(t), d1 = (std::log(forward / k) + .5 * o.iv * o.iv * t) / vt,
           d2 = d1 - vt;
    double black = o.isCall ? forward * cdf(d1) - k * cdf(d2) : k * cdf(-d2) - forward * cdf(-d1);
    double d = o.isCall ? cdf(d1) : cdf(d1) - 1;
    return {black * (o.spec.inverse ? std::exp(-carry * t) : 1), d};
}
Greeks modelGreeks(const Option& o, double spot, qint64 now)
{
    Greeks result;
    if (spot <= 0 || o.index <= 0 || o.spec.expiry <= now)
        return result;
    result.delta = optionModel(o, spot, now).delta;
    const double bump = std::max(.001, spot * 1e-5);
    result.gamma = (optionModel(o, spot + bump, now).delta -
                    optionModel(o, std::max(1e-8, spot - bump), now).delta) /
                   (2 * bump);
    auto up = o, down = o;
    up.iv += .0001;
    down.iv = std::max(0.0, o.iv - .0001);
    result.vega = (optionModel(up, spot, now).usd - optionModel(down, spot, now).usd) /
                  (up.iv - down.iv) * .01;
    const qint64 remaining = o.spec.expiry - now;
    const qint64 elapsed = std::min<qint64>(Time::DayMs, remaining);
    auto rolled = o;
    const double t = double(remaining) / (365.0 * Time::DayMs);
    const double carry = o.underlying > 0 ? std::log(o.underlying / o.index) / t : o.rate;
    rolled.underlying =
        o.index * std::exp(carry * double(remaining - elapsed) / (365.0 * Time::DayMs));
    // A one-day change, capped at remaining lifetime, matching the API theta horizon.
    result.theta = optionModel(rolled, spot, now + elapsed).usd - optionModel(o, spot, now).usd;
    return result;
}
QVector<ProfilePoint> portfolioCurve(const QVector<Option>& options, double hedge, double price,
                                     double net, qint64 now, double range, int count)
{
    QVector<ProfilePoint> points;
    if (price <= 0 || options.isEmpty() || count < 3 || range <= 0 || range >= 1)
        return points;
    QVector<ModelValue> current;
    QVector<Greeks> currentGreeks;
    for (const auto& o : options)
    {
        current.append(optionModel(o, o.index, now));
        currentGreeks.append(modelGreeks(o, o.index, now));
    }
    for (int i = 0; i < count; ++i)
    {
        double p = price * (1 - range + 2 * range * i / (count - 1));
        ProfilePoint v{p, net + hedge * (p - price), hedge};
        for (int leg = 0; leg < options.size(); ++leg)
        {
            const auto& o = options[leg];
            if (o.hasGreeks && o.index > 0)
            {
                const auto shift = optionModel(o, o.index * p / price, now);
                const auto greeks = modelGreeks(o, o.index * p / price, now);
                v.pnl += o.quantity * (shift.usd - current[leg].usd);
                // Anchor the live point to exchange Greeks and mark, preserving model curvature.
                v.delta += o.quantity * (o.delta + shift.delta - current[leg].delta);
                v.gamma += o.quantity * (o.gamma + greeks.gamma - currentGreeks[leg].gamma);
                v.theta += o.quantity * (o.theta + greeks.theta - currentGreeks[leg].theta);
                v.vega += o.quantity * (o.vega + greeks.vega - currentGreeks[leg].vega);
            }
        }
        points.append(v);
    }
    return points;
}
std::optional<double> escapePrice(const QVector<Option>& options, double reference, double band,
                                  double price, qint64 now, bool above)
{
    auto g = aggregate(options);
    if (g.gross <= Epsilon || price <= 0)
        return {};
    auto change = [&](double p)
    {
        double d = g.delta;
        for (const auto& o : options)
            if (o.index > 0)
                d += o.quantity * (optionModel(o, o.index * p / price, now).delta -
                                   optionModel(o, o.index, now).delta);
        return std::abs(d / g.gross - reference) - band;
    };
    double last = price;
    if (change(last) >= -Epsilon)
        return price;
    // Scan to find the nearest root: option spreads need not be monotone.
    double move = 0, step = .00001;
    for (int i = 1; i <= 1000 && move < 2; ++i)
    {
        move += step;
        step = std::min(.005, step * 1.04);
        double p = price * std::exp((above ? 1 : -1) * move);
        if (change(p) >= 0)
        {
            double lo = last, hi = p;
            for (int n = 0; n < 50; ++n)
            {
                double mid = (lo + hi) / 2;
                if (change(mid) >= 0)
                    hi = mid;
                else
                    lo = mid;
            }
            return (lo + hi) / 2;
        }
        last = p;
    }
    return {};
}
} // namespace scalper
