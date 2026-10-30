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
                return;
            }
            QString safety = engine.safety(utcNow());
            if (!safety.isEmpty())
            {
                busy = false;
                if (safety == engine.dataProblem(utcNow()))
                    waitForData(safety);
                else
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
                            if (!acceptRead(r, "Order margin estimate unavailable"))
                                return;
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
    if (!cancel)
    {
        const double amount = number(params, "amount");
        if (amount < engine.spec.step * .5 || amount != roundDown(amount, engine.spec.step))
            throw std::runtime_error("Hedge order amount must be positive and match the lot step");
    }
    if (!cancel && engine.running)
        status = engine.flattening ? "Closing hedge" : "Running";
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
    // The intent is already durable in orders. Do not duplicate every edit in events.
    QString label = order.label;
    const quint64 intentRevision = positionRevision;
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
                        if (r.error == "Request cancelled before send")
                            ++skippedRequests;
                        else
                            log(QString("%1 failed: code %2, %3 (order %4); amount %5, step %6, "
                                        "position %7, target %8")
                                    .arg(method)
                                    .arg(r.code)
                                    .arg(r.error, label)
                                    .arg(o->request.contains("amount")
                                             ? QString::number(o->request["amount"].toDouble(), 'g',
                                                               12)
                                             : "n/a")
                                    .arg(engine.spec.step, 0, 'g', 12)
                                    .arg(engine.inventory, 0, 'g', 12)
                                    .arg(o->target, 0, 'g', 12),
                                {{"method", method},
                                 {"code", r.code},
                                 {"reason", r.error},
                                 {"label", label},
                                 {"request", o->request},
                                 {"step", engine.spec.step},
                                 {"inventory", engine.inventory},
                                 {"target", o->target},
                                 {"uncertain", r.uncertain}},
                                true, method + "/" + QString::number(r.code));
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
                            o->state = r.error == "Request cancelled before send" ? "cancelled"
                                                                                  : "rejected";
                        }
                        save();
                        if (r.code == 11044 || r.code == 11008 || r.code == 10010 ||
                            r.code == 10004 ||
                            (r.code == 11030 && r.error.contains("order_not_found")))
                        {
                            o->uncertain = true;
                            save();
                            requestRecovery("Order is no longer open; checking execution");
                            return;
                        }
                        if (r.code == 11054)
                        {
                            retryAt = utcNow() + Exchange::MinimumTradingIntervalMs;
                            // A rejected post-only edit can cancel the resting order.
                            if (method == "private/edit")
                            {
                                o->uncertain = true;
                                save();
                                requestRecovery(
                                    "Post-only edit rejected; checking remaining order");
                            }
                            return;
                        }
                        if (r.error == "Request cancelled before send")
                        {
                            return;
                        }
                        if (r.retryable())
                        {
                            waitForData(r.error);
                            retryAt = utcNow() + Time::SecondMs;
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
                });
        },
        true, true,
        [this, label, cancel, params, intentRevision]
        {
            if (cancel)
                return true;
            auto* o = findOrder(label);
            if (!o || !engine.running || !synced || storageFailed || !engine.quote.fresh(utcNow()))
                return false;
            const bool reducing = params["reduce_only"].toBool();
            if (!reducing && !engine.safety(utcNow()).isEmpty())
                return false;
            double final =
                engine.inventory +
                (o->buy ? 1 : -1) * std::max(0.0, params["amount"].toDouble() - o->filled);
            if (reducing && (engine.inventory * (o->buy ? 1 : -1) >= 0 ||
                             std::abs(final) > std::abs(engine.inventory) + Epsilon ||
                             final * engine.inventory < -Epsilon))
                return false;
            const HedgeQuote intent{o->role,
                                    {},
                                    o->buy,
                                    true,
                                    params["price"].toDouble(),
                                    std::max(0.0, params["amount"].toDouble() - o->filled),
                                    o->target};
            if (!quoteCurrent(intent, intentRevision))
                return false;
            return true;
        });
}
bool TradingWorker::quoteCurrent(const HedgeQuote& desired, quint64 fillRevision)
{
    if (!engine.running || !synced || storageFailed || fillRevision != positionRevision)
        return false;
    if (!(engine.flattening ? engine.quote.fresh(utcNow()) : engine.safety(utcNow()).isEmpty()))
        return false;
    engine.plan(utcNow());
    const auto& next = desired.buy ? engine.quotes.buy : engine.quotes.sell;
    return next.enabled && next.role == desired.role &&
           std::abs(next.amount - desired.amount) < engine.spec.step * .1 &&
           std::abs(next.price - desired.price) <
               engine.spec.tickAt(desired.price) * Limits::QuoteDeadbandTicks &&
           (desired.buy ? desired.price < engine.quote.ask : desired.price > engine.quote.bid);
}
void TradingWorker::submitNew(const HedgeQuote& desired)
{
    const double signedAmount = (desired.buy ? 1 : -1) * desired.amount;
    const bool reducing = engine.inventory * signedAmount < 0 &&
                          desired.amount <= std::abs(engine.inventory) + Epsilon;
    const quint64 fillRevision = positionRevision;
    preflight(
        [this, desired, reducing, fillRevision]
        {
            if (!quoteCurrent(desired, fillRevision))
                return;
            for (const auto& existing : orders)
                if ((!existing.terminal || existing.uncertain || !existing.mutation.isEmpty()) &&
                    existing.buy == desired.buy)
                    return;
            Order o;
            o.label = "gs_" + store->profileId() + "_" +
                      QUuid::createUuid().toString(QUuid::Id128).left(16);
            o.instrument = engine.config.instrument;
            o.role = desired.role;
            o.target = desired.target;
            o.buy = desired.buy;
            o.total = desired.amount;
            o.price = desired.price;
            o.postOnly = true;
            o.ioc = false;
            o.state = "pending";
            o.reduceOnly = reducing;
            o.attemptAt = o.createdAt = utcNow();
            orders.append(o);
            execute(o.buy ? "private/buy" : "private/sell",
                    {{"instrument_name", o.instrument},
                     {"amount", o.total},
                     {"price", o.price},
                     {"type", "limit"},
                     {"time_in_force", "good_til_cancelled"},
                     {"post_only", true},
                     {"reject_post_only", true},
                     {"reduce_only", reducing},
                     {"label", o.label}},
                    orders.last());
        },
        desired.amount, desired.price, desired.buy, reducing);
}
void TradingWorker::editOrder(Order& order, const HedgeQuote& desired)
{
    const double total = roundDown(order.filled + desired.amount, engine.spec.step);
    const bool reducing = engine.inventory * (desired.buy ? 1 : -1) < 0 &&
                          desired.amount <= std::abs(engine.inventory) + Epsilon;
    const QString label = order.label;
    const quint64 fillRevision = positionRevision;
    preflight(
        [this, label, desired, reducing, fillRevision, total]
        {
            auto* o = findOrder(label);
            if (!o || o->terminal || !o->mutation.isEmpty() || !quoteCurrent(desired, fillRevision))
                return;
            o->target = desired.target;
            o->attemptAt = utcNow();
            execute("private/edit",
                    {{"order_id", o->id},
                     {"amount", total},
                     {"price", desired.price},
                     {"post_only", true},
                     {"reject_post_only", true},
                     {"reduce_only", reducing}},
                    *o);
        },
        desired.amount, desired.price, desired.buy, reducing);
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
