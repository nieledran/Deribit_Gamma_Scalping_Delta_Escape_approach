#include "trading/worker.h"
#include <algorithm>
#include <stdexcept>
namespace scalper
{
Order* TradingWorker::findOrder(const QString& label, const QString& id)
{
    for (auto& o : orders)
        if ((!label.isEmpty() && o.label == label) || (!id.isEmpty() && o.id == id))
            return &o;
    return nullptr;
}
bool TradingWorker::owns(const Order& o) const
{
    return store && o.label.startsWith("gs_" + store->profileId() + "_") &&
           o.instrument == engine.config.instrument;
}
Order* TradingWorker::liveOrder()
{
    for (auto& o : orders)
        if (!o.terminal || o.uncertain || !o.mutation.isEmpty())
            return &o;
    return nullptr;
}
void TradingWorker::receiveOrder(const QJsonObject& j, bool response)
{
    QString label = j["label"].toString(), id = j["order_id"].toString();
    auto* o = findOrder(label, id);
    if (!o)
    {
        if (j["order_state"] == "open")
        {
            foreignOrders = true;
            if (engine.running)
                fail("Foreign order detected on the hedge instrument");
        }
        return;
    }
    if (!owns(*o) || j["instrument_name"] != engine.config.instrument ||
        j["order_type"] != "limit" || j["direction"] != (o->buy ? "buy" : "sell") ||
        (!o->id.isEmpty() && o->id != id) || id.isEmpty())
        throw std::runtime_error("Order ownership/instrument/direction verification failed");
    QString state = j["order_state"].toString();
    if (state != "open" && state != "filled" && state != "cancelled" && state != "rejected")
        throw std::runtime_error("Unknown hedge order state");
    double filled = number(j, "filled_amount"), total = number(j, "amount");
    qint64 time = j["last_update_timestamp"].toInteger();
    if (filled < o->filled - Epsilon || time < o->updated || (o->terminal && state == "open"))
    {
        if (response)
        {
            o->uncertain = false;
            o->mutation.clear();
            o->request = {};
            save();
        }
        return;
    }
    if (filled < 0 || total < filled - Epsilon ||
        std::abs(filled / engine.spec.step - std::round(filled / engine.spec.step)) > 1e-5)
        throw std::runtime_error("Invalid cumulative fill quantity");
    double change = filled - o->filled;
    const bool changedState = o->id.isEmpty() || o->state != state;
    engine.inventory += change * (o->buy ? 1 : -1);
    if (change > Epsilon)
    {
        ++positionRevision;
        lastFill = utcNow();
    }
    o->id = id;
    o->filled = filled;
    o->total = total;
    o->price = number(j, "price");
    o->updated = time;
    o->state = state;
    o->terminal = state != "open";
    if (response)
    {
        o->uncertain = false;
        o->mutation.clear();
        o->request = {};
    }
    save();
    if (!storageFailed)
        store->event("order_update", j);
    if (change > Epsilon || changedState)
        log(QString("%1 %2: %3; filled %4 / %5, remaining %6 %7 at limit %8")
                .arg(o->buy ? "BUY" : "SELL", id, state)
                .arg(filled, 0, 'g', 10).arg(total, 0, 'g', 10)
                .arg(std::max(0.0, total - filled), 0, 'g', 10).arg(engine.config.base())
                .arg(o->price, 0, 'g', 12), j);
}
void TradingWorker::receiveTrades(const QJsonArray& trades)
{
    QJsonArray verified;
    for (const auto& value : trades)
    {
        auto j = value.toObject();
        auto* o = findOrder(j["label"].toString(), j["order_id"].toString());
        if (!o || !owns(*o))
            continue;
        if (j["instrument_name"] != engine.config.instrument ||
            j["direction"] != (o->buy ? "buy" : "sell"))
            throw std::runtime_error("Fill ownership verification failed");
        if (number(j, "amount") <= 0 || number(j, "price") <= 0)
            throw std::runtime_error("Invalid hedge trade amount or price");
        number(j, "fee");
        if (j["fee_currency"].toString() != "USDC")
            throw std::runtime_error("Unexpected hedge commission currency");
        j["construction_id"] = construction;
        verified.append(j);
    }
    store->fillMany(verified);
}
} // namespace scalper
