#include "trading/worker.h"
#include <QCryptographicHash>
#include <QDir>
#include <stdexcept>
namespace scalper
{
void TradingWorker::connectExchange(QString id, QString secret)
{
    guard(
        [&]
        {
            if (!client || storageFailed)
                throw std::runtime_error("Profile is not ready");
            if (engine.running || (synced && liveOrder()))
                throw std::runtime_error("Stop and reconcile bot orders before reconnecting");
            startPending = false;
            refreshingMarket = false;
            busy = false;
            recovering = false;
            ++epoch;
            synced = false;
            syncing = false;
            resettingProfile = false;
            exchangeClockOffsetMs.reset();
            clientId = std::move(id);
            clientSecret = std::move(secret);
            status = "Connecting";
            engine.problem.clear();
            accountMakerFee.reset();
            accountTakerFee.reset();
            client->connectTo(clientId, clientSecret, engine.config.production, endpoint);
        });
}
void TradingWorker::initializeExchange()
{
    ++epoch;
    int generation = epoch;
    synced = false;
    syncing = true;
    busy = false;
    status = "Synchronizing";
    accountMakerFee.reset();
    accountTakerFee.reset();
    engine.quote.at = 0;
    for (auto& option : positions)
        option.tickerAt = 0;
    const qint64 timeRequested = utcNow();
    client->call(
        "public/get_time", {},
        [this, generation, timeRequested](const RpcResult& r)
        {
            if (generation != epoch || !r.ok || !r.value.isDouble())
                return;
            const qint64 received = utcNow();
            exchangeClockOffsetMs = r.value.toInteger() - (timeRequested + received) / 2;
            log(QString("Clock check: exchange %1 ms %2 computer; request round trip %3 ms")
                    .arg(std::abs(*exchangeClockOffsetMs))
                    .arg(*exchangeClockOffsetMs >= 0 ? "ahead of" : "behind")
                    .arg(received - timeRequested));
        });
    client->call(
        "private/get_account_summary", {{"currency", "USDC"}, {"extended", true}},
        [this, generation](const RpcResult& r)
        {
            guard(
                [&]
                {
                    if (generation != epoch)
                        return;
                    if (!acceptRead(r, "Read account summary"))
                        return;
                    auto j = r.value.toObject();
                    auto v = j["id"];
                    QString id = v.isString() ? v.toString() : QString::number(v.toInteger());
                    if (id.isEmpty() || id == "0")
                        throw std::runtime_error("Account identity is unavailable");
                    QString identity = id + "/" +
                                       (engine.config.production ? "production/" : "testnet/") +
                                       engine.config.instrument;
                    store->bind(identity);
                    account = identity;
                    if (!accountLock)
                    {
                        QString hash = QString::fromLatin1(
                            QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
                                .toHex());
                        accountLock = std::make_unique<QLockFile>(
                            QDir::temp().filePath("DeribitGamma-" + hash + ".lock"));
                        if (!accountLock->tryLock())
                        {
                            accountLock.reset();
                            throw std::runtime_error(
                                "Another gamma instance owns this account and perpetual");
                        }
                    }
                    client->setTradingLimits(j["limits"].toObject());
                    emit historyConnect(clientId, clientSecret, engine.config.production, account,
                                        endpoint);
                    engine.margin = Margin::parse(j, utcNow());
                    auto fee = j["fees"]
                                   .toObject()[engine.config.base().toLower() + "_usdc"]
                                   .toObject()["perpetual"]
                                   .toObject()["default"]
                                   .toObject();
                    if (fee["type"] == "fixed")
                    {
                        accountMakerFee = number(fee, "maker");
                        accountTakerFee = number(fee, "taker");
                    }
                    else if (fee["type"] == "relative")
                    {
                        accountMakerFee = Exchange::DefaultMakerFee * number(fee, "maker");
                        accountTakerFee = Exchange::DefaultTakerFee * number(fee, "taker");
                    }
                    client->call(
                        "public/get_instrument", {{"instrument_name", engine.config.instrument}},
                        [this, generation](const RpcResult& spec)
                        {
                            guard(
                                [&]
                                {
                                    if (generation != epoch)
                                        return;
                                    if (!acceptRead(spec, "Read hedge instrument"))
                                        return;
                                    auto j = spec.value.toObject();
                                    engine.spec = Instrument::parse(j);
                                    if (engine.spec.kind != "future" ||
                                        engine.spec.settlement != "USDC" ||
                                        j["settlement_period"] != "perpetual" ||
                                        j["state"] != "open")
                                        throw std::runtime_error(
                                            "Hedge must be an open USDC linear perpetual");
                                    client->call(
                                        "private/enable_cancel_on_disconnect",
                                        {{"scope", "connection"}},
                                        [this, generation](const RpcResult& cod)
                                        {
                                            guard(
                                                [&]
                                                {
                                                    if (generation != epoch)
                                                        return;
                                                    if (!acceptRead(cod,
                                                                    "Enable cancel-on-disconnect"))
                                                        return;
                                                    loadPositions(
                                                        [this]
                                                        { subscribe([this] { reconcile(); }); },
                                                        true);
                                                });
                                        });
                                });
                        });
                });
        });
}
void TradingWorker::loadMetadata(QStringList names, std::function<void()> done)
{
    if (names.isEmpty())
    {
        done();
        return;
    }
    QString name = names.takeFirst();
    if (instruments.contains(name))
    {
        loadMetadata(names, std::move(done));
        return;
    }
    int generation = epoch;
    client->call(
        "public/get_instrument", {{"instrument_name", name}},
        [this, generation, name, names, done = std::move(done)](const RpcResult& r) mutable
        {
            if (!guard(
                    [&]
                    {
                        if (generation != epoch)
                            return;
                        if (!acceptRead(r, "Read option instrument"))
                            return;
                        auto spec = Instrument::parse(r.value.toObject());
                        if (spec.kind != "option" || spec.base != engine.config.base() ||
                            (spec.settlement != "USDC" && spec.settlement != engine.config.base()))
                            throw std::runtime_error("Unsupported selected option");
                        instruments[name] = spec;
                        positions[name].spec = spec;
                        positions[name].isCall = r.value.toObject()["option_type"] == "call";
                        loadMetadata(names, std::move(done));
                    }))
                polling = false;
        });
}
void TradingWorker::subscribe(std::function<void()> done)
{
    QJsonArray channels;
    channels.append("ticker." + engine.config.instrument + ".100ms");
    channels.append("user.changes." + engine.config.instrument + ".100ms");
    for (auto it = positions.begin(); it != positions.end(); ++it)
        if (it->spec.kind == "option" && std::abs(it->quantity) > Epsilon &&
            it->spec.expiry > utcNow())
        {
            channels.append("ticker." + it.key() + ".100ms");
            channels.append("user.changes." + it.key() + ".100ms");
        }
    int generation = epoch;
    client->call("private/subscribe", {{"channels", channels}},
                 [this, generation, done = std::move(done)](const RpcResult& r) mutable
                 {
                     guard(
                         [&]
                         {
                             if (generation != epoch)
                                 return;
                             if (!acceptRead(r, "Subscribe to market and account updates"))
                                 return;
                             auto names = marketInstruments(true);
                             if (refreshingMarket || startPending)
                             {
                                 done();
                                 return;
                             }
                             const qint64 now = utcNow();
                             for (const auto& option : positions)
                                 if (!engine.flattening && option.isOpen(now) &&
                                     (!option.freshGreeks(now) ||
                                      now - option.tickerAt >= Time::MarketRefreshAgeMs) &&
                                     !names.contains(option.spec.name))
                                     names.append(option.spec.name);
                             refreshingMarket = true;
                             refreshMarketData(std::move(names),
                                               [this, done = std::move(done)](bool ok)
                                               {
                                                   refreshingMarket = false;
                                                   if (ok)
                                                       done();
                                                   startMarketRefresh();
                                               });
                         });
                 });
}
void TradingWorker::refreshMargin(std::function<void(bool)> done)
{
    int generation = epoch;
    client->call(
        "private/get_account_summary", {{"currency", "USDC"}, {"extended", true}},
        [this, generation, done = std::move(done)](const RpcResult& r)
        {
            if (!guard(
                    [&]
                    {
                        if (generation != epoch)
                            return;
                        if (!r.ok)
                        {
                            engine.margin.valid = false;
                            acceptRead(r, "Refresh margin");
                            if (done)
                                done(false);
                            return;
                        }
                        engine.margin = Margin::parse(r.value.toObject(), utcNow());
                        if (done)
                            done(true);
                    }))
            {
                if (done)
                    done(false);
            }
        },
        false, true);
}
} // namespace scalper
