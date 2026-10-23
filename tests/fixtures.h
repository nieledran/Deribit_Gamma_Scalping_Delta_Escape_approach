#pragma once
#include "engine/hedger.h"
namespace scalper::fixtures
{
inline Instrument perpetual()
{
    Instrument s;
    s.name = "BTC_USDC-PERPETUAL";
    s.base = "BTC";
    s.settlement = "USDC";
    s.kind = "future";
    s.tick = 1;
    s.step = .0001;
    s.contractSize = .0001;
    return s;
}
inline Option option(qint64 now, bool call = true, bool inverse = true, double qty = .1)
{
    Option o;
    o.spec = perpetual();
    o.spec.name = QString("BTC%1-20NOV26-100000-%2").arg(inverse ? "" : "_USDC", call ? "C" : "P");
    o.spec.kind = "option";
    o.spec.strike = 100000;
    o.spec.expiry = now + 30LL * 86400000;
    o.spec.creation = now - 86400000;
    o.spec.settlement = inverse ? "BTC" : "USDC";
    o.spec.inverse = inverse;
    o.quantity = qty;
    o.isCall = call;
    o.delta = call ? .5 : -.5;
    o.gamma = .000025;
    o.theta = -100;
    o.vega = 110;
    o.index = o.underlying = 100000;
    o.iv = .6;
    o.mark = inverse ? .06 : 6000;
    o.averagePrice = inverse ? .05 : 5000;
    o.averageUsd = 5000;
    o.tickerAt = now;
    o.hasGreeks = true;
    return o;
}
inline Hedger engine(qint64 now)
{
    Hedger e;
    e.spec = perpetual();
    e.options = {option(now)};
    e.config.selected = {e.options.first().spec.name};
    e.config.maxMarginUsage = .5;
    e.config.marginLimitEnabled = true;
    e.quote = {99999, 100001, 100000, 100000, now};
    e.margin = {10000, 100, 9900, now, true};
    return e;
}
inline void refresh(Hedger& e, qint64 now)
{
    e.quote.at = now;
    e.margin.at = now;
    for (auto& o : e.options)
        o.tickerAt = now;
}
} // namespace scalper::fixtures
