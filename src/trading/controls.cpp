#include "analytics/ledger.h"
#include "trading/worker.h"
#include <QUuid>
#include <stdexcept>
namespace scalper
{
void TradingWorker::applyConfig(Config c)
{
    guard(
        [&]
        {
            if (!synced || requested != RunRequest::None || engine.running || busy ||
                startPending || liveOrder())
                throw std::runtime_error(
                    "Stop, cancel and reconcile orders before applying settings");
            if (c.instrument != engine.config.instrument ||
                c.production != engine.config.production)
                throw std::runtime_error("Instrument/environment belongs to the startup profile");
            auto error = c.validate();
            if (!error.isEmpty())
                throw std::runtime_error(error.toStdString());
            for (auto name : c.selected)
                if (!positions.contains(name) || !positions[name].isOpen(utcNow()))
                    throw std::runtime_error("Selected option is no longer open");
            QStringList old = engine.config.selected, next = c.selected;
            old.sort();
            next.sort();
            if (old != next)
            {
                if (std::abs(engine.inventory) > Epsilon)
                    throw std::runtime_error(
                        "Close the bot hedge before changing the option basket");
                construction = QUuid::createUuid().toString(QUuid::Id128);
                store->metadata("construction", {{"id", construction}});
                engine.anchored = false;
                engine.active = false;
                if (fundingValid)
                {
                    fundingBaseline = sessionFunding;
                    fundingBaselineSession = Time::fundingSessionStart(fundingAt);
                    store->metadata("funding/" + construction, {{"session", fundingBaselineSession},
                                                                {"value", fundingBaseline}});
                }
            }
            if (selectionReview)
            {
                engine.anchored = false;
                engine.active = false;
                selectionReview = false;
                store->metadata("selection_review", {{"required", false}});
            }
            if (engine.config.hedgePercent != c.hedgePercent)
            {
                engine.anchored = false;
                engine.active = false;
            }
            engine.config = c;
            client->setTradingIntervalMs(c.tradingIntervalMs);
            updateSelected();
            engine.problem.clear();
            save();
            log("Settings applied", c.json());
            publish();
        });
}
void TradingWorker::validateStart()
{
    if (std::abs(externalPosition) > Epsilon)
        throw std::runtime_error(
            "A position left from the deleted profile is still unmanaged. "
            "Close it on the exchange, then Delete local profile again before Start.");
    if (selectionReview)
        throw std::runtime_error(
            "Confirm the changed option basket with Apply Settings before Start");
    if (!synced || syncing || busy || liveOrder() || foreignOrders || storageFailed)
        throw std::runtime_error("Reconcile and cancel foreign/unfinished orders before Start");
    if (utcNow() - positionAt > Time::MarketDataMaxAgeMs ||
        std::abs(actualPosition - expectedPosition()) > engine.spec.step * .1)
        throw std::runtime_error("A fresh matching account position is required");
    const auto error = engine.config.validate();
    if (!error.isEmpty())
        throw std::runtime_error(error.toStdString());
    for (const auto& name : engine.config.selected)
        if (!positions.contains(name) || !positions[name].isOpen(utcNow()))
            throw std::runtime_error("Selected option closed or expired; confirm a current basket");
}
void TradingWorker::startTrading()
{
    if (startPending)
        return;
    guard(
        [&]
        {
            validateStart();
            startPending = true;
            status = "Refreshing market data";
            publish();
            startMarketRefresh();
        });
}
void TradingWorker::startMarketRefresh()
{
    if (!startPending || refreshingMarket)
        return;
    busy = true;
    refreshMarketData(marketInstruments(false),
                      [this](bool ok)
                      {
                          busy = false;
                          if (!ok && startPending && client->authenticated() && synced)
                              return; // Keep the Start request until the next market refresh.
                          const bool requested = startPending;
                          startPending = false;
                          if (ok && requested)
                              completeStart();
                      });
}
void TradingWorker::completeStart()
{
    guard(
        [&]
        {
            validateStart();
            if (!engine.start(utcNow()))
                throw std::runtime_error(engine.problem.toStdString());
            if (!pendingPnlStarts.contains(construction))
            {
                publish();
                TradingSnapshot snapshot;
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    snapshot = shared->trading;
                }
                // Preserve the first Start snapshot even if report storage is temporarily busy.
                pendingPnlStarts.insert(construction, Ledger::valuationSnapshot(snapshot));
                flushPnlStart();
            }
            engine.problem.clear();
            requested = RunRequest::Hedge;
            waitingData = false;
            retryAt = 0;
            attempts = 0;
            status = "Running";
            passiveSince = 0;
            passiveAlerted = false;
            hedgeAlert.clear();
            save();
            log("Trading started");
            pump();
        });
}
void TradingWorker::stopTrading()
{
    startPending = false;
    guard(
        [&]
        {
            requested = RunRequest::None;
            waitingData = false;
            retryAt = 0;
            engine.stop();
            desiredClose = false;
            passiveSince = 0;
            passiveAlerted = false;
            hedgeAlert.clear();
            status = "Paused";
            if (requestId)
                client->cancelQueued(requestId);
            save();
            log("Stop & Cancel requested; hedge inventory is retained");
            pump();
        });
}
void TradingWorker::closeHedge()
{
    startPending = false;
    guard(
        [&]
        {
            if (!synced || foreignOrders || storageFailed)
                throw std::runtime_error(
                    "Reconcile a fresh matching account position before closing the hedge");
            engine.stop();
            engine.problem.clear();
            requested = RunRequest::Close;
            waitingData = false;
            retryAt = 0;
            desiredClose = true;
            status = "Closing hedge";
            if (requestId)
                client->cancelQueued(requestId);
            if (utcNow() - positionAt > Time::MarketDataMaxAgeMs ||
                std::abs(actualPosition - expectedPosition()) > engine.spec.step * .1)
            {
                waitForData("Verifying account position before Close hedge");
                poll();
            }
            save();
            pump();
        });
}
} // namespace scalper
