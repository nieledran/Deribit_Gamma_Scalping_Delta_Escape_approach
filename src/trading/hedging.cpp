#include "trading/worker.h"
#include <algorithm>
namespace scalper
{
void TradingWorker::finishOperation()
{
    if (hedgeElapsed.isValid())
    {
        lastHedgeMs = hedgeElapsed.elapsed();
        hedgeElapsed.invalidate();
    }
    engine.complete(utcNow());
    status = engine.running ? "Running" : "Paused";
    attempts = 0;
    while (orders.size() > Limits::RecentOrders && orders.first().terminal &&
           !orders.first().uncertain && orders.first().mutation.isEmpty() &&
           std::abs(store->coveredQuantity(orders.first().id) - orders.first().filled) < Epsilon)
        orders.removeFirst();
    save();
    log(QString("Hedge target reached: position %1, target %2, remaining %3 %4")
            .arg(engine.inventory, 0, 'g', 10).arg(engine.operation.target, 0, 'g', 10)
            .arg(engine.remaining(), 0, 'g', 10).arg(engine.config.base()), {{"inventory", engine.inventory},
                                     {"execution_ms", lastHedgeMs},
                                     {"reference_delta", engine.referenceDelta},
                                     {"target", engine.operation.target},
                                     {"remaining", engine.remaining()}});
}
void TradingWorker::pump()
{
    if (!client || !client->authenticated() || syncing || busy || resettingProfile)
        return;
    if (recovering)
    {
        recoverPosition();
        return;
    }
    auto* live = liveOrder();
    if (!synced)
    {
        // A mismatch blocks new trades, but must not block cancelling verified bot orders.
        if (!engine.running && live && !live->uncertain && !live->id.isEmpty() &&
            live->mutation.isEmpty() && owns(*live))
            cancelOrder(*live);
        return;
    }
    if (live && live->uncertain)
        return;
    if (!engine.running)
    {
        if (live)
        {
            cancelOrder(*live);
            return;
        }
        if (desiredClose)
        {
            desiredClose = false;
            if (std::abs(engine.inventory) > engine.spec.step * .5)
            {
                engine.flattening = true;
                engine.running = true;
                engine.active = false;
                attempts = 0;
            }
            else
                status = "Paused";
        }
        else
            return;
    }
    if (storageFailed)
    {
        if (live)
            cancelOrder(*live);
        return;
    }
    if (!engine.flattening)
    {
        auto reason = engine.safety(utcNow());
        if (!reason.isEmpty())
        {
            fail(reason);
            if (live)
                cancelOrder(*live);
            return;
        }
    }
    if (engine.active && engine.obsolete())
    {
        if (live)
        {
            cancelOrder(*live);
            return;
        }
        engine.active = false;
        save();
    }
    if (!engine.active)
    {
        if (live)
        {
            cancelOrder(*live);
            return;
        }
        if (utcNow() - lastFill > Time::FillRecoveryDelayMs)
        {
            for (const auto& o : orders)
                if (o.filled > 0 && std::abs(store->coveredQuantity(o.id) - o.filled) > Epsilon)
                {
                    busy = true;
                    recoverTrades(0, [this] { busy = false; });
                    return;
                }
        }
        qint64 previous = engine.completedAt;
        auto next = engine.evaluate(utcNow());
        if (next || previous != engine.completedAt)
            save();
        if (!next)
            return;
        hedgeElapsed.start();
        log("Delta escape", {{"option_delta", next->delta},
                             {"normalized_delta", next->normalized},
                             {"target", next->target},
                             {"market_at", engine.quote.at},
                             {"bid", engine.quote.bid},
                             {"ask", engine.quote.ask},
                             {"engine_snapshot", engine.json()}});
        attempts = 0;
    }
    if (live)
    {
        if (live->mutation.isEmpty() && utcNow() - live->attemptAt >= engine.config.makerWaitMs)
        {
            ++attempts;
            bool aggressive = engine.config.execution == Execution::Aggressive
                                  ? attempts >= 1
                                  : engine.config.execution == Execution::Balanced && attempts >= 2;
            editOrder(*live, aggressive);
        }
        return;
    }
    if (std::abs(engine.remaining()) < engine.spec.step * .5)
    {
        finishOperation();
        return;
    }
    bool aggressive = engine.config.execution == Execution::Aggressive
                          ? attempts >= 1
                          : engine.config.execution == Execution::Balanced && attempts >= 2;
    submitNew(aggressive);
}
} // namespace scalper
