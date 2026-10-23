#include "trading/worker.h"
#include <QSet>
#include <QUuid>
#include <stdexcept>

namespace scalper
{
void TradingWorker::deleteData()
{
    guard(
        [&]
        {
            if (!client || !client->authenticated() || account.isEmpty() || !accountLock ||
                !accountLock->isLocked() || syncing || recovering ||
                requested != RunRequest::None || busy || startPending || engine.running ||
                storageFailed)
                throw std::runtime_error(
                    "Connect to the bound account and stop trading before deleting this profile");
            // Like Grid Bot: cancel this profile's exact labels, verify twice, then recreate.
            recovering = false;
            requested = RunRequest::None;
            waitingData = false;
            ++epoch;
            resettingProfile = syncing = busy = true;
            synced = polling = refreshingMarket = desiredClose = false;
            engine.stop();
            for (auto& order : orders)
                if (owns(order) &&
                    (!order.terminal || order.uncertain || !order.mutation.isEmpty()))
                {
                    order.mutation = "private/cancel_by_label";
                    order.request = {{"currency", "USDC"}, {"label", order.label}};
                    order.uncertain = true;
                }
            save();
            log("Deleting current profile: cancelling owned orders; settings and P&L retained "
                "until verification");
            emit historyStop();
            status = "Cancelling profile orders";
            publish();
            readProfileResetOrders();
        });
}
void TradingWorker::readProfileResetOrders()
{
    const int generation = epoch;
    client->call("private/get_open_orders_by_instrument",
                 {{"instrument_name", engine.config.instrument}},
                 [this, generation](const RpcResult& response)
                 {
                     if (generation != epoch)
                         return;
                     guard(
                         [&]
                         {
                             if (!response.ok || !response.value.isArray())
                                 throw std::runtime_error(
                                     "Cannot list orders for deletion; profile and P&L retained: " +
                                     response.error.toStdString());
                             QSet<QString> labels;
                             for (const auto& order : orders)
                                 if (owns(order) && (!order.terminal || order.uncertain ||
                                                     !order.mutation.isEmpty()))
                                     labels.insert(order.label);
                             const auto prefix = "gs_" + store->profileId() + "_";
                             for (const auto& value : response.value.toArray())
                             {
                                 const auto order = value.toObject();
                                 if (order["instrument_name"] != engine.config.instrument ||
                                     order["order_id"].toString().isEmpty())
                                     throw std::runtime_error(
                                         "Invalid cancellation snapshot; profile and P&L retained");
                                 const auto label = order["label"].toString();
                                 if (label.startsWith(prefix))
                                     labels.insert(label);
                             }
                             cancelProfileLabels(labels.values());
                         });
                 });
}
void TradingWorker::cancelProfileLabels(QStringList labels)
{
    if (labels.isEmpty())
    {
        verifyProfileCancellation(0);
        return;
    }
    const auto label = labels.takeFirst();
    const int generation = epoch;
    client->call(
        "private/cancel_by_label", {{"currency", "USDC"}, {"label", label}},
        [this, generation, labels](const RpcResult& response)
        {
            if (generation != epoch)
                return;
            guard(
                [&]
                {
                    if (!response.ok || !response.value.isDouble() ||
                        response.value.toInteger(-1) < 0)
                        throw std::runtime_error("Cancellation not confirmed; profile and P&L "
                                                 "retained. Retry Delete local profile: " +
                                                 response.error.toStdString());
                    cancelProfileLabels(labels);
                });
        },
        true, true);
}
void TradingWorker::verifyProfileCancellation(int checks)
{
    const int generation = epoch;
    QTimer::singleShot(
        checks ? 2000 : 0, this,
        [this, generation, checks]
        {
            if (generation != epoch)
                return;
            client->call(
                "private/get_open_orders_by_instrument",
                {{"instrument_name", engine.config.instrument}},
                [this, generation, checks](const RpcResult& response)
                {
                    if (generation != epoch)
                        return;
                    guard(
                        [&]
                        {
                            if (!response.ok || !response.value.isArray())
                                throw std::runtime_error(
                                    "Cannot verify cancellation; profile and P&L retained");
                            const auto prefix = "gs_" + store->profileId() + "_";
                            for (const auto& value : response.value.toArray())
                            {
                                const auto order = value.toObject();
                                if (order["instrument_name"] != engine.config.instrument ||
                                    order["order_id"].toString().isEmpty())
                                    throw std::runtime_error(
                                        "Invalid final order snapshot; profile and P&L retained");
                                if (order["label"].toString().startsWith(prefix))
                                    throw std::runtime_error(
                                        "A profile order remains open; profile and P&L retained. "
                                        "Retry Delete local profile.");
                            }
                            if (!checks)
                                verifyProfileCancellation(1);
                            else
                            {
                                foreignOrders = !response.value.toArray().isEmpty();
                                finishProfileReset();
                            }
                        });
                });
        });
}
void TradingWorker::finishProfileReset()
{
    const int generation = epoch;
    client->call(
        "private/get_position", {{"instrument_name", engine.config.instrument}},
        [this, generation](const RpcResult& response)
        {
            if (generation != epoch)
                return;
            guard(
                [&]
                {
                    if (!response.ok)
                        throw std::runtime_error(
                            "Cannot read remaining position; profile and P&L retained");
                    const auto position = response.value.toObject();
                    double remaining = number(position, "size_currency");
                    if (position["direction"] == "sell")
                        remaining = -std::abs(remaining);
                    else if (position["direction"] == "buy")
                        remaining = std::abs(remaining);
                    else if (std::abs(remaining) > Epsilon)
                        throw std::runtime_error(
                            "Invalid position direction; profile and P&L retained");
                    Hedger fresh;
                    fresh.config.instrument = engine.config.instrument;
                    fresh.config.production = engine.config.production;
                    fresh.spec = engine.spec;
                    fresh.quote = engine.quote;
                    fresh.margin = engine.margin;
                    const auto nextConstruction = QUuid::createUuid().toString(QUuid::Id128);
                    try
                    {
                        store->resetProfile(fresh, nextConstruction, account, remaining);
                    }
                    catch (...)
                    {
                        storageFailed = true;
                        throw;
                    }
                    engine = std::move(fresh);
                    construction = nextConstruction;
                    externalPosition = actualPosition = remaining;
                    positionAt = utcNow();
                    ++positionRevision;
                    orders.clear();
                    messages.clear();
                    selectionReview = false;
                    fundingValid = false;
                    sessionFunding = fundingBaseline = 0;
                    fundingAt = fundingBaselineSession = 0;
                    resettingProfile = syncing = busy = false;
                    synced = true;
                    status = "Paused";
                    if (std::abs(remaining) > Epsilon)
                        engine.problem =
                            "Unmanaged perpetual remains from the deleted profile. Close it on the "
                            "exchange, then delete this profile again before Start.";
                    emit deleted();
                    log("Profile recreated; accumulated local P&L cleared. Exchange position "
                        "remains " +
                        QString::number(remaining, 'g', 10) + " " + engine.config.base());
                    emit historyConnect(clientId, clientSecret, engine.config.production, account,
                                        endpoint);
                    publish();
                });
        });
}
} // namespace scalper
