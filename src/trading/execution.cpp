#include "trading/worker.h"
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace scalper
{
void TradingWorker::preflight(std::function<void()> action, double amount, double price, bool buy,
                              bool reducing)
{
    if (reducing)
    {
        action();
        return;
    }
    busy = true;
    int generation = epoch;
    refreshMargin(
        [this, generation, action = std::move(action), amount, price, buy](bool ok) mutable
        {
            if (generation != epoch)
                return;
            if (!ok)
            {
                busy = false;
                fail("Margin data unavailable before hedge increase");
                return;
            }
            QString safety = engine.safety(utcNow());
            if (!safety.isEmpty())
            {
                busy = false;
                fail(safety);
                return;
            }
            client->call(
                "private/get_margins",
                {{"instrument_name", engine.config.instrument},
                 {"amount", amount},
                 {"price", price}},
                [this, generation, action = std::move(action), buy](const RpcResult& r) mutable
                {
                    guard(
                        [&]
                        {
                            if (generation != epoch)
                                return;
                            busy = false;
                            if (!r.ok)
                                throw std::runtime_error("Order margin estimate unavailable: " +
                                                         r.error.toStdString());
                            double needed = number(r.value.toObject(), buy ? "buy" : "sell");
                            if (needed < 0 || needed > engine.margin.available)
                                throw std::runtime_error(
                                    "Insufficient available funds for the hedge order");
                            if (engine.running)
                                action();
                        });
                },
                false, true);
        });
}
void TradingWorker::execute(QString method, QJsonObject params, Order& order)
{
    if (!owns(order))
        throw std::runtime_error("Order ownership is unverified");
    bool cancel = method == "private/cancel";
    order.mutation = method;
    order.request = params;
    if (cancel)
    {
        try
        {
            save();
        }
        catch (...)
        {
            storageFailed = true;
        }
    }
    else
        save();
    busy = true;
    if (!storageFailed)
        store->event("order_intent", {{"method", method},
                                      {"params", params},
                                      {"label", order.label},
                                      {"target", engine.operation.target},
                                      {"option_delta", aggregate(engine.options).delta}});
    QString label = order.label;
    int generation = epoch;
    requestId = client->call(
        method, params,
        [this, generation, label, method](const RpcResult& r)
        {
            guard(
                [&]
                {
                    if (generation != epoch)
                        return;
                    busy = false;
                    requestId = 0;
                    auto* o = findOrder(label);
                    if (!o)
                        return;
                    if (!r.ok)
                    {
                        log(QString("%1 failed: code %2, %3 (order %4)")
                                .arg(method).arg(r.code).arg(r.error, label),
                            {{"method", method}, {"code", r.code}, {"reason", r.error},
                             {"label", label}, {"uncertain", r.uncertain}});
                        if (r.uncertain)
                        {
                            o->uncertain = true;
                            save();
                            requestRecovery("Unknown request result; checking the original order");
                            return;
                        }
                        o->mutation.clear();
                        o->request = {};
                        if (method == "private/buy" || method == "private/sell")
                        {
                            o->terminal = true;
                            o->state = "rejected";
                        }
                        save();
                        if (r.code == 11044 || r.code == 11008 || r.code == 10010 || r.code == 10004)
                        {
                            o->uncertain = true;
                            save();
                            requestRecovery("Order is no longer open; checking execution");
                            return;
                        }
                        if (r.code == 11054)
                        {
                            ++attempts;
                            // A rejected post-only edit can cancel the resting order.
                            if (method == "private/edit")
                            {
                                o->uncertain = true;
                                save();
                                requestRecovery("Post-only edit rejected; checking remaining order");
                            }
                            return;
                        }
                        if (r.error == "Request cancelled before send")
                        {
                            log("Queued request cancelled before send");
                            return;
                        }
                        fail(r.error);
                        return;
                    }
                    auto root = r.value.toObject();
                    auto j = root.contains("order") ? root["order"].toObject() : root;
                    if (j["label"].toString() != label)
                    {
                        o->uncertain = true;
                        save();
                        fail("Trading response label mismatch; reconciliation required");
                        synced = false;
                        return;
                    }
                    receiveOrder(j, true);
                    receiveTrades(root["trades"].toArray());
                    if (!recovering && o->terminal && engine.active &&
                        std::abs(engine.remaining()) < engine.spec.step * .5)
                        finishOperation();
                });
        },
        true, true,
        [this, label, cancel, params]
        {
            if (cancel)
                return true;
            auto* o = findOrder(label);
            if (!o || !engine.running || !synced || storageFailed || !engine.quote.fresh(utcNow()))
                return false;
            if (!o->reduceOnly && !engine.safety(utcNow()).isEmpty())
                return false;
            double final =
                engine.inventory +
                (o->buy ? 1 : -1) * std::max(0.0, params["amount"].toDouble() - o->filled);
            if (o->reduceOnly && (engine.inventory * (o->buy ? 1 : -1) >= 0 ||
                                  std::abs(final) > std::abs(engine.inventory) + Epsilon ||
                                  final * engine.inventory < -Epsilon))
                return false;
            const double mid = engine.quote.mid();
            const double adverse = (o->buy ? 1 : -1) * (params["price"].toDouble() - mid);
            if (adverse / mid * Limits::BasisPoints > engine.config.slippageBps)
                return false;
            return true;
        });
}
void TradingWorker::submitNew(bool aggressive)
{
    double remaining = engine.remaining();
    bool buy = remaining > 0;
    double qty = roundDown(std::abs(remaining), engine.spec.step);
    if (qty < engine.spec.step * .5)
    {
        finishOperation();
        return;
    }
    bool reducing = engine.inventory * remaining < 0;
    if (reducing)
        qty = roundDown(std::min(qty, std::abs(engine.inventory)), engine.spec.step);
    // Re-quantize the accumulated position before serializing a reduce-only amount.
    double mid = engine.quote.mid();
    double p = aggressive ? (buy ? engine.quote.ask : engine.quote.bid)
                          : (buy ? engine.quote.bid : engine.quote.ask);
    double tick = engine.spec.tickAt(p);
    p = tickPrice(p, tick, !buy);
    if (aggressive && std::abs(p - mid) / mid * Limits::BasisPoints > engine.config.slippageBps)
    {
        fail("Spread exceeds the configured slippage limit");
        return;
    }
    const bool postOnly = engine.config.execution == Execution::PostOnly;
    preflight(
        [this, buy, qty, p, aggressive, reducing, postOnly]
        {
            Order o;
            o.label = "gs_" + store->profileId() + "_" +
                      QUuid::createUuid().toString(QUuid::Id128).left(16);
            o.instrument = engine.config.instrument;
            o.buy = buy;
            o.total = qty;
            o.price = p;
            o.postOnly = postOnly;
            o.ioc = aggressive;
            o.state = "pending";
            o.reduceOnly = reducing;
            o.attemptAt = utcNow();
            o.createdAt = o.attemptAt;
            orders.append(o);
            QJsonObject params{
                {"instrument_name", o.instrument},
                {"amount", qty},
                {"price", p},
                {"type", "limit"},
                {"time_in_force", aggressive ? "immediate_or_cancel" : "good_til_cancelled"},
                {"post_only", postOnly},
                {"reject_post_only", postOnly},
                {"reduce_only", reducing},
                {"label", o.label}};
            execute(buy ? "private/buy" : "private/sell", params, orders.last());
        },
        qty, p, buy, reducing);
}
void TradingWorker::editOrder(Order& o, bool aggressive)
{
    if (aggressive)
    {
        cancelOrder(o);
        return;
    } // edit has no supported IOC parameter; create IOC only after cancel acknowledgement.
    double p = o.buy ? engine.quote.bid : engine.quote.ask;
    p = tickPrice(p, engine.spec.tickAt(p), !o.buy);
    double remaining = engine.remaining();
    bool buy = remaining > 0;
    if (buy != o.buy)
    {
        cancelOrder(o);
        return;
    }
    double qty = roundDown(std::abs(remaining), engine.spec.step);
    if (qty < engine.spec.step * .5)
    {
        cancelOrder(o);
        return;
    }
    if (o.reduceOnly)
        qty = roundDown(std::min(qty, std::abs(engine.inventory)), engine.spec.step);
    double total = roundDown(o.filled + qty, engine.spec.step);
    o.attemptAt = utcNow();
    if (std::abs(o.price - p) < engine.spec.tickAt(p) * .1 &&
        std::abs(o.total - total) < engine.spec.step * .1)
        return;
    execute("private/edit",
            {{"order_id", o.id},
             {"amount", total},
             {"price", p},
             {"post_only", o.postOnly},
             {"reject_post_only", o.postOnly},
             {"reduce_only", o.reduceOnly}},
            o);
}
void TradingWorker::cancelOrder(Order& o)
{
    if (o.id.isEmpty())
    {
        o.uncertain = true;
        save();
        fail("Order ID missing; reconnect to resolve its durable label");
        synced = false;
        return;
    }
    execute("private/cancel", {{"order_id", o.id}}, o);
}
} // namespace scalper
