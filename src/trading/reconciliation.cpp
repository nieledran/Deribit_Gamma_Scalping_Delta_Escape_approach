#include "trading/worker.h"
#include <QSet>
#include <algorithm>
#include <stdexcept>
namespace scalper
{
void TradingWorker::requestRecovery(const QString& reason)
{
    if (recovering)
        return;
    recoveryRunning = engine.running;
    recoveryClose = engine.flattening || desiredClose;
    recoveryChecks = 0;
    recoveryReadyAt = 0;
    recovering = true;
    synced = false;
    engine.stop();
    desiredClose = false;
    status = "Reconciling execution";
    log(reason, {{"exchange_position", actualPosition}, {"expected_position", expectedPosition()}});
    save();
    publish();
}
void TradingWorker::recoverPosition()
{
    if (utcNow() < recoveryReadyAt)
        return;
    syncing = true;
    reconcileOrder(0, [this]
    {
        if (!recovering)
            return;
        if (auto* live = liveOrder())
        {
            syncing = false;
            cancelOrder(*live);
            return;
        }
        recoverTrades(0, [this]
        {
            const auto before = positionRevision;
            loadPositions([this, before]
            {
                if (!recovering)
                    return;
                const bool matching = before == positionRevision &&
                    std::abs(actualPosition - expectedPosition()) <= engine.spec.step * .1;
                syncing = false;
                if (!matching && ++recoveryChecks < 3)
                {
                    recoveryReadyAt = utcNow() + Time::FillRecoveryDelayMs;
                    return;
                }
                if (!matching || foreignOrders || selectionReview)
                {
                    fail(QString("Position reconciliation stopped: exchange %1, expected %2 %3; "
                                 "difference %4. Profile and history retained; review and reconnect.")
                             .arg(actualPosition, 0, 'g', 12).arg(expectedPosition(), 0, 'g', 12)
                             .arg(engine.config.base()).arg(actualPosition - expectedPosition(), 0, 'g', 12));
                    return;
                }
                recovering = false;
                synced = true;
                engine.active = false;
                attempts = 0;
                engine.problem.clear();
                engine.running = recoveryRunning || recoveryClose;
                engine.flattening = recoveryClose;
                if (engine.running)
                {
                    const auto reason = engine.flattening
                        ? (engine.quote.fresh(utcNow()) ? QString() : QString("Fresh quote required to close hedge"))
                        : engine.safety(utcNow());
                    if (!reason.isEmpty())
                    {
                        fail(reason);
                        return;
                    }
                }
                status = engine.running ? (engine.flattening ? "Closing hedge" : "Running") : "Paused";
                save();
                log("Own orders and fills reconciled; previous action restored",
                    {{"inventory", engine.inventory}, {"running", engine.running},
                     {"closing", engine.flattening}});
                publish();
            }, true);
        });
    });
}
void TradingWorker::reconcile()
{
    int generation = epoch;
    client->call(
        "private/get_open_orders_by_instrument", {{"instrument_name", engine.config.instrument}},
        [this, generation](const RpcResult& r)
        {
            guard(
                [&]
                {
                    if (generation != epoch)
                        return;
                    if (!r.ok)
                        throw std::runtime_error(r.error.toStdString());
                    if (!r.value.isArray())
                        throw std::runtime_error("Invalid open orders response");
                    foreignOrders = false;
                    for (auto o : r.value.toArray())
                        receiveOrder(o.toObject(), true);
                    reconcileOrder(
                        0,
                        [this]
                        {
                            recoverTrades(
                                0,
                                [this]
                                {
                                    loadPositions(
                                        [this]
                                        {
                                            if (std::abs(actualPosition - expectedPosition()) >
                                                engine.spec.step * .1)
                                                throw std::runtime_error(
                                                    QString(
                                                        "Position mismatch: exchange %1, bot "
                                                        "%2 %3. "
                                                        "Do not trade this construction manually. "
                                                        "Export CSV, then use Actions > Delete "
                                                        "local profile.")
                                                        .arg(actualPosition, 0, 'g', 10)
                                                        .arg(expectedPosition(), 0, 'g', 10)
                                                        .arg(engine.config.base())
                                                        .toStdString());
                                            syncing = false;
                                            synced = true;
                                            engine.active = false;
                                            engine.running = false;
                                            engine.flattening = false;
                                            engine.problem =
                                                std::abs(externalPosition) > Epsilon
                                                    ? "Unmanaged perpetual remains from the "
                                                      "deleted profile. Close it on the exchange, "
                                                      "then delete this profile again before Start."
                                                    : QString();
                                            status = "Paused";
                                            save();
                                            log("Reconciliation complete; review settings and "
                                                "press Start / Resume");
                                            emit historyConnect(clientId, clientSecret,
                                                                engine.config.production, account,
                                                                endpoint);
                                            publish();
                                        },
                                        true);
                                });
                        });
                });
        });
}
void TradingWorker::reconcileOrder(int index, std::function<void()> done)
{
    if (index >= orders.size())
    {
        done();
        return;
    }
    auto& o = orders[index];
    if (o.terminal && !o.uncertain && o.mutation.isEmpty())
    {
        reconcileOrder(index + 1, std::move(done));
        return;
    }
    QString label = o.label, id = o.id;
    int generation = epoch;
    QString method = id.isEmpty() ? "private/get_order_state_by_label" : "private/get_order_state";
    QJsonObject params = id.isEmpty() ? QJsonObject{{"currency", "USDC"}, {"label", label}}
                                      : QJsonObject{{"order_id", id}};
    client->call(
        method, params,
        [this, generation, index, label, done = std::move(done)](const RpcResult& r) mutable
        {
            guard(
                [&]
                {
                    if (generation != epoch)
                        return;
                    QJsonObject j = r.value.isArray() ? (r.value.toArray().isEmpty()
                                                             ? QJsonObject()
                                                             : r.value.toArray().first().toObject())
                                                      : r.value.toObject();
                    if (!r.ok || j.isEmpty())
                    {
                        // A missing create reply is never permission to resend. Keep its durable
                        // label unresolved until the exchange can prove the order's final state.
                        orders[index].uncertain = true;
                        save();
                        if ((r.ok && j.isEmpty()) || r.code == 10004)
                        {
                            resolveAbsentOrder(index, std::move(done));
                            return;
                        }
                        throw std::runtime_error(
                            "Unresolved order " + label.toStdString() +
                            ": exchange state unavailable; retry Connect after review");
                    }
                    receiveOrder(j, true);
                    reconcileOrder(index + 1, std::move(done));
                });
        },
        false, true);
}
void TradingWorker::resolveAbsentOrder(int index, std::function<void()> done)
{
    auto& order = orders[index];
    if (!owns(order) || order.createdAt <= 0)
        throw std::runtime_error(
            "Missing order identity or original intent time; automatic recovery refused");
    int generation = epoch;
    QString label = order.label;
    order.mutation = "private/cancel_by_label";
    order.request = {{"currency", "USDC"}, {"label", label}};
    save();
    client->call(
        "private/cancel_by_label", order.request,
        [this, index, generation, label,
         done = std::move(done)](const RpcResult& cancellation) mutable
        {
            guard(
                [&]
                {
                    if (generation != epoch)
                        return;
                    if (!cancellation.ok)
                        throw std::runtime_error(
                            "Cannot confirm cancellation of unresolved label: " +
                            label.toStdString());
                    scanOrderWindow(index, 0, {}, std::move(done));
                });
        },
        true, true);
}
void TradingWorker::scanOrderWindow(int index, int phase, QJsonArray accumulated,
                                    std::function<void()> done)
{
    int generation = epoch;
    QString label = orders[index].label;
    QJsonObject params{
        {"instrument_name", engine.config.instrument},
        {"start_timestamp",
         std::max<qint64>(1, orders[index].createdAt - History::RecoveryWindowPaddingMs)},
        {"end_timestamp", utcNow() + History::RecoveryWindowPaddingMs},
        {"count", History::PageSize},
        {"sorting", "asc"},
        {"historical", phase == 1}};
    client->call(
        "private/get_user_trades_by_instrument_and_time", params,
        [this, index, generation, label, phase, accumulated,
         done = std::move(done)](const RpcResult& history) mutable
        {
            guard(
                [&]
                {
                    if (generation != epoch)
                        return;
                    if (!history.ok || !history.value.toObject()["trades"].isArray() ||
                        !history.value.toObject()["has_more"].isBool() ||
                        history.value.toObject()["has_more"].toBool())
                        throw std::runtime_error("Incomplete trade window for unresolved label; "
                                                 "recovery remains blocked");
                    for (auto v : history.value.toObject()["trades"].toArray())
                        accumulated.append(v);
                    if (phase == 0)
                    {
                        scanOrderWindow(index, 1, accumulated, std::move(done));
                        return;
                    }
                    QJsonArray owned;
                    double filled = 0;
                    QString exchangeId = orders[index].id;
                    QSet<QString> seen;
                    for (auto v : accumulated)
                    {
                        auto trade = v.toObject();
                        QString id = trade["order_id"].toString(),
                                tradeLabel = trade["label"].toString();
                        if (tradeLabel == label || (!exchangeId.isEmpty() && id == exchangeId))
                        {
                            if (!exchangeId.isEmpty() && exchangeId != id)
                                throw std::runtime_error(
                                    "Unresolved label matches multiple exchange orders");
                            exchangeId = id;
                            trade["label"] = label;
                            QString key = trade["trade_id"].toString();
                            if (key.isEmpty())
                                throw std::runtime_error("Recovery fill has no trade ID");
                            if (!seen.contains(key))
                            {
                                seen.insert(key);
                                filled += number(trade, "amount");
                                owned.append(trade);
                            }
                        }
                        else if (!findOrder(tradeLabel, id))
                            throw std::runtime_error("Foreign trade overlaps an unresolved "
                                                     "request; manual reconciliation required");
                    }
                    auto& o = orders[index];
                    if (filled + Epsilon < o.filled || filled > o.total + Epsilon)
                        throw std::runtime_error(
                            "Recovery fill total contradicts the durable order");
                    if (filled > 0)
                    {
                        o.id = exchangeId;
                        receiveOrder(
                            {{"label", label},
                             {"order_id", exchangeId},
                             {"instrument_name", o.instrument},
                             {"order_type", "limit"},
                             {"direction", o.buy ? "buy" : "sell"},
                             {"order_state", filled >= o.total - Epsilon ? "filled" : "cancelled"},
                             {"amount", o.total},
                             {"filled_amount", filled},
                             {"price", o.price},
                             {"last_update_timestamp", utcNow()}},
                            true);
                        receiveTrades(owned);
                    }
                    else
                    {
                        o.terminal = true;
                        o.uncertain = false;
                        o.mutation.clear();
                        o.request = {};
                        save();
                    }
                    log("Unresolved label cancelled and trade window reconciled",
                        {{"label", label}, {"filled", filled}});
                    reconcileOrder(index + 1, std::move(done));
                });
        },
        false, true);
}
void TradingWorker::recoverTrades(int index, std::function<void()> done, bool historical)
{
    if (index >= orders.size())
    {
        done();
        return;
    }
    auto& o = orders[index];
    if (o.id.isEmpty() || o.filled <= 0 ||
        std::abs(store->coveredQuantity(o.id) - o.filled) < Epsilon)
    {
        recoverTrades(index + 1, std::move(done));
        return;
    }
    int generation = epoch;
    client->call(
        "private/get_user_trades_by_order",
        {{"order_id", o.id}, {"sorting", "asc"}, {"historical", historical}},
        [this, generation, index, historical, done = std::move(done)](const RpcResult& r) mutable
        {
            guard(
                [&]
                {
                    if (generation != epoch)
                        return;
                    if (!r.ok)
                        throw std::runtime_error("Cannot recover order fills: " +
                                                 r.error.toStdString());
                    if (!r.value.isArray() && !r.value.toObject()["trades"].isArray())
                        throw std::runtime_error("Invalid order fills response");
                    auto a = r.value.isArray() ? r.value.toArray()
                                               : r.value.toObject()["trades"].toArray();
                    receiveTrades(a);
                    if (std::abs(store->coveredQuantity(orders[index].id) - orders[index].filled) >
                        Epsilon)
                    {
                        if (!historical)
                        {
                            recoverTrades(index, std::move(done), true);
                            return;
                        }
                        throw std::runtime_error("Historical fill coverage does not match "
                                                 "cumulative execution; trading remains blocked");
                    }
                    recoverTrades(index + 1, std::move(done));
                });
        });
}
} // namespace scalper
