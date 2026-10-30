#include "trading/worker.h"
#include <algorithm>
namespace scalper
{
void TradingWorker::pump()
{
    flushJournal();
    flushPnlStart();
    if (skippedRequests && utcNow() - summaryAt >= Time::MinuteMs)
    {
        log(QString("Skipped %1 outdated queued requests before sending").arg(skippedRequests));
        skippedRequests = 0;
        summaryAt = utcNow();
    }
    if (!client || !client->authenticated() || syncing || busy || resettingProfile)
        return;
    if (recovering)
    {
        recoverPosition();
        return;
    }
    if (utcNow() < retryAt)
        return;
    auto* live = liveOrder();
    if (!synced)
    {
        if (!engine.running)
            for (auto& order : orders)
                if (!order.terminal && !order.uncertain && !order.id.isEmpty() &&
                    order.mutation.isEmpty() && owns(order))
                {
                    cancelOrder(order);
                    break;
                }
        return;
    }
    if (storageFailed)
    {
        // An unsent create whose durable save failed must not hide the other live side.
        for (auto& order : orders)
            if (!order.terminal && !order.uncertain && !order.id.isEmpty() &&
                order.mutation.isEmpty() && owns(order))
            {
                cancelOrder(order);
                break;
            }
        return;
    }
    for (const auto& o : orders)
        if (o.uncertain || !o.mutation.isEmpty())
            return;
    if (waitingData || storageFailed || !engine.running)
    {
        if (live)
        {
            cancelOrder(*live);
            return;
        }
        if (waitingData)
        {
            resumeRequested();
            return;
        }
        if (storageFailed)
            return;
        if (!desiredClose)
            return;
        desiredClose = false;
        engine.flattening = engine.running = true;
        engine.active = false;
    }
    const QString reason =
        engine.flattening
            ? (engine.quote.fresh(utcNow()) ? QString() : "Fresh quote required to close hedge")
            : engine.safety(utcNow());
    if (!reason.isEmpty())
    {
        if (reason == engine.dataProblem(utcNow()) || engine.flattening)
            waitForData(reason);
        else
            fail(reason);
        if (live)
            cancelOrder(*live);
        return;
    }
    // Account every cumulative fill before replacing or resizing either side.
    if (utcNow() - lastFill > Time::FillRecoveryDelayMs)
        for (const auto& o : orders)
            if (o.filled > 0 && std::abs(store->coveredQuantity(o.id) - o.filled) > Epsilon)
            {
                busy = true;
                recoverTrades(0, [this] { busy = false; });
                return;
            }
    const auto priorState = engine.state;
    engine.plan(utcNow());
    status = strategyName(engine.state);
    if (priorState != engine.state)
    {
        if (engine.active)
        {
            passiveSince = 0;
            passiveAlerted = false;
            hedgeAlert.clear();
        }
        save();
    }
    const bool passive = engine.state == StrategyState::InitialHedge ||
                         engine.state == StrategyState::Corrective ||
                         engine.state == StrategyState::Closing;
    if (passive)
    {
        if (!passiveSince)
            passiveSince = utcNow();
        if (utcNow() - passiveSince >= Time::PassiveHedgeAlertMs)
        {
            hedgeAlert =
                QString("Passive hedge unfilled for 30 s; delta error %1 %2. Review exposure.")
                    .arg(engine.quotes.error, 0, 'g', 8)
                    .arg(engine.config.base());
            if (!passiveAlerted)
                log(hedgeAlert);
            passiveAlerted = true;
        }
    }
    else
    {
        if (passiveSince)
            lastHedgeMs = utcNow() - passiveSince;
        passiveSince = 0;
        passiveAlerted = false;
        hedgeAlert.clear();
    }
    // First remove stale roles, crossed levels and duplicate sides. All cancellation paths
    // revisit this pump until every affected order is terminal, including Close and Stop.
    bool seenBuy = false, seenSell = false;
    for (auto& o : orders)
    {
        if (o.terminal)
            continue;
        const auto& desired = o.buy ? engine.quotes.buy : engine.quotes.sell;
        bool& seen = o.buy ? seenBuy : seenSell;
        const bool invalid = !engine.running || !desired.enabled || o.role != desired.role ||
                             seen || !o.postOnly || o.ioc ||
                             (o.buy ? o.price >= engine.quote.ask : o.price <= engine.quote.bid);
        seen = true;
        if (invalid)
        {
            cancelOrder(o);
            return;
        }
    }
    if (!engine.running)
    {
        requested = RunRequest::None;
        save();
        return;
    }
    // Reductions precede increases; preserve a price within the three-tick deadband.
    for (bool reductions : {true, false})
        for (auto& o : orders)
        {
            if (o.terminal)
                continue;
            auto desired = o.buy ? engine.quotes.buy : engine.quotes.sell;
            const double remaining = std::max(0.0, o.total - o.filled);
            const bool reducing = engine.inventory * (o.buy ? 1 : -1) < 0 &&
                                  desired.amount <= std::abs(engine.inventory) + Epsilon;
            const bool less = desired.amount < remaining - engine.spec.step * .1;
            if (less != reductions)
                continue;
            if (std::abs(desired.price - o.price) <
                engine.spec.tickAt(o.price) * Limits::QuoteDeadbandTicks)
                desired.price = o.price;
            if (std::abs(desired.price - o.price) > engine.spec.tickAt(o.price) * .1 ||
                std::abs(desired.amount - remaining) > engine.spec.step * .1 ||
                reducing != o.reduceOnly)
            {
                editOrder(o, desired);
                return;
            }
        }
    // Restoring a missing side comes after existing orders have been made safe.
    for (const auto& desired : {engine.quotes.buy, engine.quotes.sell})
    {
        if (!desired.enabled)
            continue;
        bool found = false;
        for (const auto& o : orders)
            found |= !o.terminal && o.buy == desired.buy;
        if (!found)
        {
            submitNew(desired);
            return;
        }
    }
    while (orders.size() > Limits::RecentOrders && orders.first().terminal &&
           !orders.first().uncertain && orders.first().mutation.isEmpty() &&
           std::abs(store->coveredQuantity(orders.first().id) - orders.first().filled) < Epsilon)
        orders.removeFirst();
}
} // namespace scalper
