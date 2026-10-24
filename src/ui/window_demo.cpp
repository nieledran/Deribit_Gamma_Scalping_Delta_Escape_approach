#include "ui/window.h"
#include <QTimeZone>
namespace scalper
{
void Window::seedDemo()
{
    TradingSnapshot s;
    s.config = initial;
    s.makerFee = s.config.fallbackMakerFee;
    s.takerFee = s.config.fallbackTakerFee;
    s.config.maxMarginUsage = .5;
    s.profileId = "offline-demo";
    s.account = "OFFLINE DEMO";
    s.constructionId = "demo";
    s.timestamp = utcNow();
    s.synced = true;
    s.status = "OFFLINE DEMO";
    s.anchored = true;
    s.referenceDelta = .02;
    const double spot = s.config.base() == "ETH" ? 4000 : 100000;
    const auto expiry =
        QDateTime(QDateTime::fromMSecsSinceEpoch(s.timestamp, QTimeZone::utc()).date().addDays(28),
                  QTime(8, 0), QTimeZone::utc());
    const auto prefix = s.config.base() + "-" + expiry.toString("ddMMMyy").toUpper() + "-" +
                        QString::number(spot, 'f', 0);
    s.quote = {spot - 1, spot + 1, spot, spot, s.timestamp};
    s.margin = {10000, 500, 9500, s.timestamp, true};
    s.margin.model = "segregated_sm";
    s.margin.scope = "Settlement account (USDC)";
    Option o;
    o.spec = {prefix + "-C",
              s.config.base(),
              s.config.base(),
              "option",
              .0001,
              .1,
              1,
              spot,
              expiry.toMSecsSinceEpoch(),
              s.timestamp - Time::DayMs,
              true,
              {}};
    o.isCall = true;
    o.quantity = .1;
    o.delta = .52;
    o.gamma = .000025 * 100000 / spot;
    o.theta = -80 * spot / 100000;
    o.vega = 110 * spot / 100000;
    o.iv = .6;
    o.index = o.underlying = spot;
    o.tickerAt = s.timestamp;
    o.hasGreeks = true;
    o.mark = .067;
    s.positions.append(o);
    o.spec.name = prefix + "-P";
    o.isCall = false;
    o.delta = -.48;
    o.mark = .063;
    s.positions.append(o);
    s.config.selected = {s.positions[0].spec.name, o.spec.name};
    s.inventory = -.004;
    s.target = -.004;
    Hedger preview;
    preview.config = s.config;
    preview.spec.name = s.config.instrument;
    preview.spec.base = s.config.base();
    preview.spec.settlement = "USDC";
    preview.spec.tick = 1;
    preview.spec.step = .0001;
    preview.options = s.positions;
    preview.quote = s.quote;
    preview.margin = s.margin;
    preview.inventory = s.inventory;
    preview.running = preview.anchored = true;
    preview.plan(s.timestamp);
    s.strategy = preview.state;
    s.hedgeQuotes = preview.quotes;
    s.messages = {isoUtc(s.timestamp) + "  Offline demo. No exchange connection."};
    AnalyticsSnapshot a;
    a.constructionId = s.constructionId;
    a.timestamp = s.timestamp;
    a.startedAt = s.timestamp - Time::DayMs;
    a.pnl = {120, 18, -4, 3, 1, .2};
    a.ready = true;
    a.complete = true;
    a.completeness = "Synthetic example only";
    a.historyStatus = "No live trading";
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->trading = s;
    shared->analytics = a;
}
} // namespace scalper
