#include "analytics/ledger.h"
#include "analytics/model.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace scalper
{
void HedgeBook::trade(double q, double p, double fee)
{
    if (!std::isfinite(q) || !std::isfinite(p) || p <= 0 || !std::isfinite(fee))
        throw std::runtime_error("Invalid fill in P&L ledger");
    fees += fee;
    if (std::abs(quantity) < Epsilon || quantity * q > 0)
    {
        average =
            (std::abs(quantity) * average + std::abs(q) * p) / (std::abs(quantity) + std::abs(q));
        quantity += q;
        return;
    }
    double closing = std::min(std::abs(quantity), std::abs(q));
    realized += closing * (p - average) * (quantity > 0 ? 1 : -1);
    double prior = quantity;
    quantity += q;
    if (std::abs(quantity) < Epsilon)
    {
        quantity = 0;
        average = 0;
    }
    else if (prior * quantity < 0)
        average = p;
}
Ledger::Ledger(const QString& corePath, const QString& path)
{
    connection = "analytics_" + QUuid::createUuid().toString(QUuid::Id128);
    coreConnection = "reader_" + connection;
    db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    if (!db.open())
        throw std::runtime_error(db.lastError().text().toStdString());
    sqlExec(db, "PRAGMA journal_mode=WAL");
    sqlExec(db, "PRAGMA synchronous=FULL");
    sqlExec(db, "PRAGMA busy_timeout=1000");
    sqlExec(db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,data TEXT NOT NULL)");
    sqlExec(db, "CREATE TABLE IF NOT EXISTS raw_logs(currency TEXT,log_id TEXT,at "
                "INTEGER,instrument TEXT,data TEXT,PRIMARY KEY(currency,log_id))");
    sqlExec(db, "CREATE INDEX IF NOT EXISTS logs_instrument ON raw_logs(instrument,at)");
    sqlExec(db, "CREATE TABLE IF NOT EXISTS trades(fill_key TEXT PRIMARY KEY,construction TEXT,at "
                "INTEGER,data TEXT)");
    sqlExec(db, "CREATE TABLE IF NOT EXISTS events(event_key TEXT PRIMARY KEY,at INTEGER,kind "
                "TEXT,data TEXT)");
    sqlExec(db, "CREATE TABLE IF NOT EXISTS legs(construction TEXT,instrument TEXT,selected_at "
                "INTEGER,quantity REAL,start_id TEXT,start_at INTEGER,complete INTEGER,reason "
                "TEXT,PRIMARY KEY(construction,instrument))");
    sqlExec(db, "CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,at INTEGER,construction "
                "TEXT,baseline TEXT)");
    sqlExec(db, "CREATE TABLE IF NOT EXISTS samples(at INTEGER,construction TEXT,snapshot TEXT,pnl "
                "TEXT,complete INTEGER,PRIMARY KEY(at,construction))");
    core = QSqlDatabase::addDatabase("QSQLITE", coreConnection);
    core.setDatabaseName(corePath);
    core.setConnectOptions("QSQLITE_OPEN_READONLY");
    if (!core.open())
        throw std::runtime_error(core.lastError().text().toStdString());
    auto cursor = metadata("core_cursor");
    coreFillSeq = cursor["fills"].toInteger();
    coreEventSeq = cursor["events"].toInteger();
    QSqlQuery horizon(db);
    if (horizon.exec(
            "SELECT MAX(at) FROM (SELECT at FROM raw_logs UNION ALL SELECT at FROM trades)") &&
        horizon.next())
        historyHorizon = horizon.value(0).toLongLong();
}
Ledger::~Ledger()
{
    core.close();
    core = QSqlDatabase();
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(coreConnection);
    QSqlDatabase::removeDatabase(connection);
}
QJsonObject Ledger::metadata(const QString& key) const
{
    QSqlQuery q(db);
    q.prepare("SELECT data FROM meta WHERE key=?");
    q.addBindValue(key);
    if (!q.exec())
        throw std::runtime_error(q.lastError().text().toStdString());
    return q.next() ? object(q.value(0).toString()) : QJsonObject{};
}
void Ledger::metadata(const QString& key, const QJsonObject& j)
{
    if (metadata(key) == j)
        return;
    if (key != "core_cursor")
        invalidateHistory();
    sqlExec(db, "INSERT OR REPLACE INTO meta VALUES(?,?)", {key, compact(j)});
}
void Ledger::syncCore()
{
    // A reset can commit just before the process exits, before the UI clear signal arrives.
    QSqlQuery identity(core);
    if (!identity.exec("SELECT data FROM meta WHERE key='profile'") || !identity.next())
        throw std::runtime_error("Cannot read core profile identity");
    const auto currentProfile = object(identity.value(0).toString());
    identity.finish();
    const auto priorProfile = metadata("core_profile");
    if (!priorProfile.isEmpty() && priorProfile != currentProfile)
        clear();
    metadata("core_profile", currentProfile);
    if (!db.transaction())
        throw std::runtime_error("Cannot begin analytics synchronization");
    qint64 fills = coreFillSeq, events = coreEventSeq;
    try
    {
        QSqlQuery q(core);
        q.prepare("SELECT seq,fill_key,at,data FROM fills WHERE seq>? ORDER BY seq");
        q.addBindValue(fills);
        if (!q.exec())
            throw std::runtime_error(q.lastError().text().toStdString());
        while (q.next())
        {
            auto j = object(q.value(3).toString());
            sqlExec(db, "INSERT OR IGNORE INTO trades VALUES(?,?,?,?)",
                    {q.value(1), j["construction_id"].toString(), q.value(2), q.value(3)});
            fills = q.value(0).toLongLong();
            historyHorizon = std::max(historyHorizon, q.value(2).toLongLong());
        }
        q.finish();
        q.prepare("SELECT seq,at,kind,data FROM events WHERE seq>? ORDER BY seq");
        q.addBindValue(events);
        if (!q.exec())
            throw std::runtime_error(q.lastError().text().toStdString());
        while (q.next())
        {
            sqlExec(db, "INSERT OR IGNORE INTO events VALUES(?,?,?,?)",
                    {QString::number(q.value(0).toLongLong()), q.value(1), q.value(2), q.value(3)});
            events = q.value(0).toLongLong();
        }
        metadata("core_cursor", {{"fills", fills}, {"events", events}});
        if (!db.commit())
            throw std::runtime_error("Cannot commit analytics synchronization");
    }
    catch (...)
    {
        db.rollback();
        throw;
    }
    if (fills != coreFillSeq || events != coreEventSeq)
        invalidateHistory();
    coreFillSeq = fills;
    coreEventSeq = events;
}
void Ledger::invalidateHistory()
{
    ++historyRevision;
    valuations.clear();
}
void Ledger::ensureLegs(const TradingSnapshot& s)
{
    if (s.constructionId.isEmpty())
        return;
    for (const auto& o : s.positions)
        if (s.config.selected.contains(o.spec.name))
        {
            sqlExec(db, "INSERT OR IGNORE INTO legs VALUES(?,?,?,?,?,?,?,?)",
                    {s.constructionId, o.spec.name, s.timestamp, o.quantity, QString(), qint64(0),
                     0, "History not imported"});
            QSqlQuery changed(db);
            if (changed.exec("SELECT changes()") && changed.next() && changed.value(0).toInt())
                invalidateHistory();
        }
}
void Ledger::logEntry(const QString& currency, const QJsonObject& j)
{
    QString id = j["id"].isString() ? j["id"].toString() : QString::number(j["id"].toInteger(-1));
    if (id == "-1" || id.isEmpty() || j["timestamp"].toInteger() <= 0)
        throw std::runtime_error("Transaction log entry has no stable identity");
    // Do not deduplicate by trade_id: one trade can close a position and open another.
    sqlExec(
        db,
        "INSERT INTO raw_logs VALUES(?,?,?,?,?) ON CONFLICT(currency,log_id) DO UPDATE SET "
        "at=excluded.at,instrument=excluded.instrument,data=excluded.data WHERE "
        "data<>excluded.data",
        {currency, id, j["timestamp"].toInteger(), j["instrument_name"].toString(), compact(j)});
    QSqlQuery changed(db);
    if (changed.exec("SELECT changes()") && changed.next() && changed.value(0).toInt())
    {
        historyHorizon = std::max(historyHorizon, j["timestamp"].toInteger());
        invalidateHistory();
    }
}
void Ledger::logEntries(const QString& currency, const QJsonArray& entries)
{
    if (!db.transaction())
        throw std::runtime_error("Cannot begin history page");
    try
    {
        for (auto v : entries)
            logEntry(currency, v.toObject());
        if (!db.commit())
            throw std::runtime_error("Cannot commit history page");
    }
    catch (...)
    {
        db.rollback();
        throw;
    }
}
QVector<QJsonObject> Ledger::logs(const QString& name, qint64 cutoff) const
{
    QVector<QJsonObject> a;
    QSqlQuery q(db);
    q.prepare("SELECT data FROM raw_logs WHERE instrument=? AND at<=? ORDER BY at,CAST(log_id AS "
              "INTEGER)");
    q.addBindValue(name);
    q.addBindValue(cutoff);
    if (!q.exec())
        throw std::runtime_error(q.lastError().text().toStdString());
    while (q.next())
        a.append(object(q.value(0).toString()));
    return a;
}
static double signedAmount(const QJsonObject& j)
{
    QString side = j["side"].toString(j["direction"].toString());
    if (!side.contains("buy") && !side.contains("sell"))
        throw std::runtime_error("Unknown trade direction");
    return std::abs(number(j, "amount")) * (side.contains("buy") ? 1 : -1);
}
void Ledger::finishImport(const QString& construction, const Option& o)
{
    QSqlQuery q(db);
    q.prepare("SELECT selected_at,quantity,start_id,complete FROM legs WHERE construction=? AND "
              "instrument=?");
    q.addBindValue(construction);
    q.addBindValue(o.spec.name);
    if (!q.exec() || !q.next())
        return;
    if (!q.value(2).toString().isEmpty() && q.value(3).toBool())
        return; // Freeze the chosen position episode, including after its expiry.
    qint64 selected = q.value(0).toLongLong();
    double wanted = q.value(1).toDouble();
    auto entries = logs(o.spec.name, selected);
    QString start;
    qint64 startAt = 0;
    double position = 0;
    bool origin = false;
    for (const auto& j : entries)
        if (j["type"] == "trade" || j["type"] == "trade allocation")
        {
            double amount = signedAmount(j);
            double after = j.contains("position") ? number(j, "position") : position + amount;
            double before = after - amount;
            if (std::abs(before) < Epsilon || before * after < 0)
            {
                start =
                    j["id"].isString() ? j["id"].toString() : QString::number(j["id"].toInteger());
                startAt = j["timestamp"].toInteger();
                origin = std::abs(before) < Epsilon;
            }
            position = after;
            if (std::abs(position) < Epsilon)
            {
                start.clear();
                startAt = 0;
                origin = false;
            }
        }
    bool complete = origin && !start.isEmpty() &&
                    std::abs(position - wanted) < std::max(Epsilon, o.spec.step * .1);
    QString reason =
        complete ? QString() : "Cannot prove the current position episode from available history";
    sqlExec(db,
            "UPDATE legs SET start_id=?,start_at=?,complete=?,reason=? WHERE construction=? AND "
            "instrument=?",
            {start, startAt, int(complete), reason, construction, o.spec.name});
    invalidateHistory();
}
QVector<QJsonObject> Ledger::trades(const QString& construction, qint64 cutoff) const
{
    QVector<QJsonObject> a;
    QSqlQuery q(db);
    q.prepare("SELECT data FROM trades WHERE construction=? AND at<=? ORDER BY "
              "at,CAST(substr(fill_key,instr(fill_key,'/')+1) AS INTEGER),fill_key");
    q.addBindValue(construction);
    q.addBindValue(cutoff);
    if (!q.exec())
        throw std::runtime_error(q.lastError().text().toStdString());
    while (q.next())
        a.append(object(q.value(0).toString()));
    std::stable_sort(
        a.begin(), a.end(),
        [](const QJsonObject& left, const QJsonObject& right)
        {
            qint64 la = left["timestamp"].toInteger(), ra = right["timestamp"].toInteger();
            if (la != ra)
                return la < ra;
            qint64 ls = left["trade_seq"].toInteger(), rs = right["trade_seq"].toInteger();
            if (ls > 0 && rs > 0 && ls != rs)
                return ls < rs;
            QString li = left["trade_id"].toString(), ri = right["trade_id"].toString();
            bool ln = false, rn = false;
            qint64 lv = li.section('-', -1).toLongLong(&ln),
                   rv = ri.section('-', -1).toLongLong(&rn);
            return ln && rn && lv != rv ? lv < rv : li < ri;
        });
    return a;
}
Valuation Ledger::value(const TradingSnapshot& s) const
{
    QJsonArray legs;
    for (const auto& o : s.positions)
        if (s.config.selected.contains(o.spec.name))
            legs.append(QJsonObject{{"name", o.spec.name},
                                    {"quantity", o.quantity},
                                    {"average", o.averagePrice},
                                    {"average_usd", o.averageUsd},
                                    {"expired", o.spec.expiry <= s.timestamp}});
    const QJsonObject state{
        {"revision", qint64(historyRevision)},
        {"selected", QJsonArray::fromStringList(s.config.selected)},
        {"construction", s.constructionId},
        {"instrument", s.config.instrument},
        {"legs", legs},
        {"inventory", s.inventory},
        {"funding_at", s.fundingAt},
        {"funding_valid", s.fundingValid},
        {"baseline", s.fundingBaseline},
        {"baseline_session", s.fundingBaselineSession},
        {"funding_fresh", s.timestamp - s.fundingAt <= Time::FundingDataMaxAgeMs},
        {"cutoff", s.timestamp < historyHorizon ? s.timestamp : 0}};
    const auto key = QCryptographicHash::hash(QJsonDocument(state).toJson(QJsonDocument::Compact),
                                              QCryptographicHash::Sha256);
    const auto found = valuations.constFind(key);
    if (found != valuations.cend())
    {
        auto result = found->valuation;
        const auto& previous = found->snapshot;
        result.pnl.unrealized +=
            found->hedge * ((s.quote.mark > 0 ? s.quote.mark : s.quote.mid()) -
                            (previous.quote.mark > 0 ? previous.quote.mark : previous.quote.mid()));
        for (const auto& o : s.positions)
            for (const auto& old : previous.positions)
                if (old.spec.name == o.spec.name)
                    result.pnl.options +=
                        found->markExposure.value(o.spec.name) * (o.markUsd() - old.markUsd());
        result.pnl.funding += found->funding * (s.sessionFunding - previous.sessionFunding);
        return result;
    }
    CachedValue cached;
    cached.snapshot = s;
    cached.valuation = evaluate(s, &cached.markExposure, &cached.hedge, &cached.funding);
    // Incomplete history can use live FX fallbacks and settlement carry snapshots.
    // Re-evaluate it until reconciliation rather than caching an approximation.
    if (cached.valuation.complete)
    {
        if (valuations.size() >= 16)
            valuations.clear();
        valuations.insert(key, cached);
    }
    return cached.valuation;
}
Valuation Ledger::valueUncached(const TradingSnapshot& s) const
{
    return evaluate(s);
}
Valuation Ledger::evaluate(const TradingSnapshot& s, QHash<QString, double>* exposure,
                           double* hedge, double* funding) const
{
    Valuation v;
    for (const auto& name : s.config.selected)
        if (std::none_of(s.positions.begin(), s.positions.end(),
                         [&](const Option& o) { return o.spec.name == name; }))
        {
            v.complete = false;
            v.missing << "Selected option position unavailable: " + name;
        }
    QSet<QString> ownedOrders;
    HedgeBook book;
    qint64 firstFill = 0;
    for (const auto& j : trades(s.constructionId, s.timestamp))
    {
        book.trade(signedAmount(j), number(j, "price"), j["fee"].toDouble());
        ownedOrders.insert(j["order_id"].toString());
        if (!firstFill)
            firstFill = j["timestamp"].toInteger();
    }
    v.pnl.realized = book.realized;
    if (hedge)
        *hedge = book.quantity;
    v.pnl.hedgeFees = book.fees;
    v.pnl.unrealized =
        book.quantity * ((s.quote.mark > 0 ? s.quote.mark : s.quote.mid()) - book.average);
    if (std::abs(book.quantity - s.inventory) > 1e-8)
    {
        v.complete = false;
        v.missing << "Waiting for confirmed hedge fills";
    }
    for (const auto& o : s.positions)
        if (s.config.selected.contains(o.spec.name))
        {
            QSqlQuery q(db);
            q.prepare("SELECT start_id,start_at,complete,reason FROM legs WHERE construction=? AND "
                      "instrument=?");
            q.addBindValue(s.constructionId);
            q.addBindValue(o.spec.name);
            bool exists = q.exec() && q.next();
            QString start = exists ? q.value(0).toString() : QString();
            qint64 startAt = exists ? q.value(1).toLongLong() : 0;
            bool complete = exists && q.value(2).toBool();
            if (!startAt || !complete)
            {
                v.complete = false;
                v.missing << "Option history incomplete: " + o.spec.name;
                v.pnl.options +=
                    o.quantity *
                    (o.markUsd() - (o.averageUsd > 0 ? o.averageUsd
                                                     : (o.spec.inverse ? o.averagePrice * o.index
                                                                       : o.averagePrice)));
                continue;
            }
            if (!v.first || startAt < v.first)
                v.first = startAt;
            double cash = 0, fees = 0, lastPosition = 0;
            bool ended = false, settled = false;
            for (const auto& j : logs(o.spec.name, s.timestamp))
            {
                qint64 at = j["timestamp"].toInteger();
                QString id =
                    j["id"].isString() ? j["id"].toString() : QString::number(j["id"].toInteger());
                if (at < startAt || (at == startAt && id.toLongLong() < start.toLongLong()) ||
                    ended)
                    continue;
                auto type = j["type"].toString();
                double fx = 1;
                if (o.spec.inverse)
                {
                    fx = j["index_price"].toDouble();
                    if (fx <= 0)
                    {
                        v.complete = false;
                        v.missing << "Historical conversion rate missing: " + o.spec.name;
                        fx = o.index;
                    }
                }
                if (type == "trade" || type == "trade allocation")
                {
                    double amount = signedAmount(j);
                    cash += (j.contains("cashflow") ? j["cashflow"].toDouble()
                                                    : -amount * j["price"].toDouble()) *
                            fx;
                    fees += j["commission"].toDouble() * fx;
                    lastPosition =
                        j.contains("position") ? j["position"].toDouble() : lastPosition + amount;
                    if (std::abs(lastPosition) < Epsilon)
                        ended = true;
                }
                else if (type == "expiry" || type == "delivery" || type == "exercise" ||
                         type == "assignment")
                {
                    double settlement = j["settlement_price"].toDouble(j["price"].toDouble());
                    if (j["cashflow"].toDouble() != 0)
                    {
                        cash += j["cashflow"].toDouble() * fx;
                        settled = true;
                    }
                    else if (type == "expiry")
                    {
                        settled = true;
                    }
                    else if (settlement > 0)
                    {
                        cash += lastPosition * std::max(0.0, o.isCall ? settlement - o.spec.strike
                                                                      : o.spec.strike - settlement);
                        settled = true;
                    }
                    fees += j["commission"].toDouble() * fx;
                    ended = true;
                }
            }
            // New-style settlement can create then immediately deliver a dated future.
            // Attribute its intrinsic payout once; never also add that future's cashflow.
            if (!ended && o.spec.expiry <= s.timestamp && std::abs(o.quantity) < Epsilon)
            {
                QString future = o.spec.name.section('-', 0, 1);
                for (const auto& j : logs(future, s.timestamp))
                    if (j["type"] == "delivery" &&
                        std::abs(j["timestamp"].toInteger() - o.spec.expiry) <
                            History::DeliveryWindowMs)
                    {
                        double settlement = j["settlement_price"].toDouble(j["price"].toDouble());
                        if (settlement > 0)
                        {
                            cash +=
                                lastPosition * std::max(0.0, o.isCall ? settlement - o.spec.strike
                                                                      : o.spec.strike - settlement);
                            settled = true;
                            ended = true;
                            break;
                        }
                    }
                if (!settled)
                {
                    v.complete = false;
                    v.missing << "Waiting for final option settlement: " + o.spec.name;
                }
            }
            v.pnl.options += cash + (ended ? 0 : o.quantity * o.markUsd());
            if (exposure)
                exposure->insert(o.spec.name, ended ? 0 : o.quantity);
            v.pnl.optionFees += fees;
        }
    qint64 fundingBoundary = Time::fundingSessionStart(s.fundingAt);
    bool liveFunding = firstFill && s.fundingValid && s.fundingAt > 0 &&
                       s.timestamp - s.fundingAt <= Time::FundingDataMaxAgeMs;
    if (firstFill && !liveFunding)
    {
        v.complete = false;
        v.missing << "Current funding accrual unavailable; only confirmed intervals are included";
    }
    if (firstFill && firstFill < fundingBoundary &&
        metadata("perp_log_cursor")["at"].toInteger() < fundingBoundary)
    {
        v.complete = false;
        v.missing << "Perpetual funding history has not reached the latest settlement";
    }
    if (firstFill)
        for (const auto& j : logs(s.config.instrument, s.timestamp))
            if (j["timestamp"].toInteger() >= firstFill)
            {
                if (j["type"] == "trade" && !ownedOrders.contains(j["order_id"].toString()))
                {
                    v.complete = false;
                    v.missing << "Foreign perpetual trade in funding interval";
                    continue;
                }
                // interest_pl is an interval amount; total_interest_pl/session_rpl reset daily.
                // Neither settlement cashflow nor session_rpl is added to this independent book.
                qint64 at = j["timestamp"].toInteger();
                if (!liveFunding || at < fundingBoundary ||
                    (j["type"] == "settlement" && at < fundingBoundary + History::DeliveryWindowMs))
                    v.pnl.funding += j["interest_pl"].toDouble();
            }
    if (liveFunding)
    {
        if (funding)
            *funding = 1;
        bool settled = false;
        double older = 0;
        qint64 previousBoundary = fundingBoundary - Time::DayMs;
        for (const auto& j : logs(s.config.instrument, s.timestamp))
            if (j["timestamp"].toInteger() >= firstFill)
            {
                qint64 at = j["timestamp"].toInteger();
                bool settlement = j["type"] == "settlement";
                if (settlement && at >= fundingBoundary &&
                    at < fundingBoundary + History::DeliveryWindowMs)
                    settled = true;
                if (at < previousBoundary ||
                    (settlement && at < previousBoundary + History::DeliveryWindowMs))
                    older += j["interest_pl"].toDouble();
            }
        if (firstFill < fundingBoundary && !settled)
        {
            v.complete = false;
            v.missing << "Settlement funding reconciliation pending";
            QSqlQuery q(db);
            q.prepare("SELECT snapshot FROM samples WHERE construction=? AND at<? AND at>=? ORDER "
                      "BY at DESC LIMIT 1");
            q.addBindValue(s.constructionId);
            q.addBindValue(fundingBoundary);
            q.addBindValue(fundingBoundary - Time::FundingCarryWindowMs);
            if (q.exec() && q.next())
            {
                auto prior = restoreSnapshot(object(q.value(0).toString()));
                if (prior.fundingValid && std::abs(prior.inventory) > Epsilon)
                {
                    v.pnl.funding =
                        older + prior.sessionFunding -
                        (prior.fundingBaselineSession == previousBoundary ? prior.fundingBaseline
                                                                          : 0);
                    v.complete = false;
                    v.missing << "Settlement funding reconciliation pending; carrying the last "
                                 "pre-settlement accrual";
                }
            }
        }
        v.pnl.funding += s.sessionFunding -
                         (s.fundingBaselineSession == fundingBoundary ? s.fundingBaseline : 0);
    }
    v.missing.removeDuplicates();
    return v;
}
QJsonObject Ledger::valuationSnapshot(const TradingSnapshot& s)
{
    QJsonArray a;
    for (const auto& o : s.positions)
        if (s.config.selected.contains(o.spec.name))
            a.append(o.json());
    return {{"at", s.timestamp},
            {"construction", s.constructionId},
            {"config", s.config.json()},
            {"inventory", s.inventory},
            {"mark", s.quote.mark},
            {"bid", s.quote.bid},
            {"ask", s.quote.ask},
            {"positions", a},
            {"session_funding", s.sessionFunding},
            {"funding_baseline", s.fundingBaseline},
            {"funding_at", s.fundingAt},
            {"funding_baseline_session", s.fundingBaselineSession},
            {"funding_valid", s.fundingValid}};
}
TradingSnapshot Ledger::restoreSnapshot(const QJsonObject& j)
{
    TradingSnapshot s;
    s.timestamp = j["at"].toInteger();
    s.constructionId = j["construction"].toString();
    s.config = Config::fromJson(j["config"].toObject());
    s.inventory = j["inventory"].toDouble();
    s.quote.mark = j["mark"].toDouble();
    s.quote.bid = j["bid"].toDouble();
    s.quote.ask = j["ask"].toDouble();
    for (auto o : j["positions"].toArray())
        s.positions.append(Option::fromJson(o.toObject()));
    s.sessionFunding = j["session_funding"].toDouble();
    s.fundingBaseline = j["funding_baseline"].toDouble();
    s.fundingAt = j["funding_at"].toInteger();
    s.fundingBaselineSession = j["funding_baseline_session"].toInteger();
    s.fundingValid = j["funding_valid"].toBool();
    return s;
}
void Ledger::sample(const TradingSnapshot& s, const Valuation& v, const AnalyticsSnapshot* report)
{
    if (s.timestamp - lastSample < 1000)
        return;
    QJsonObject data = v.pnl.json();
    if (report && report->ready)
    {
        data["construction_pnl"] = report->pnl.json();
        data["started_at"] = report->startedAt;
    }
    sqlExec(db, "INSERT OR REPLACE INTO samples VALUES(?,?,?,?,?)",
            {s.timestamp, s.constructionId, compact(valuationSnapshot(s)), compact(data),
             int(v.complete)});
    lastSample = s.timestamp;
}
AnalyticsSnapshot Ledger::report(const TradingSnapshot& s)
{
    AnalyticsSnapshot a;
    a.constructionId = s.constructionId;
    a.timestamp = s.timestamp;
    const QString key = "pnl_start/" + s.constructionId;
    auto baseline = metadata(key);
    if (baseline.isEmpty())
    {
        QSqlQuery q(core);
        q.prepare("SELECT data FROM meta WHERE key=?");
        q.addBindValue(key);
        if (!q.exec())
            throw std::runtime_error("Cannot read construction P&L start");
        if (q.next())
        {
            baseline = object(q.value(0).toString());
            metadata(key, baseline);
        }
    }
    if (baseline.isEmpty())
    {
        a.completeness = "P&L begins at the first Start of this construction";
        return a;
    }
    const auto current = value(s);
    const auto start = value(restoreSnapshot(baseline));
    a.startedAt = baseline["at"].toInteger();
    a.pnl = current.pnl - start.pnl;
    a.ready = true;
    a.complete = current.complete && start.complete;
    if (metadata("pnl_sample/" + s.constructionId).isEmpty())
    {
        auto zero = start.pnl.json();
        zero["construction_pnl"] = Pnl{}.json();
        zero["started_at"] = a.startedAt;
        sqlExec(
            db, "INSERT OR IGNORE INTO samples VALUES(?,?,?,?,?)",
            {a.startedAt, s.constructionId, compact(baseline), compact(zero), int(start.complete)});
        metadata("pnl_sample/" + s.constructionId, {{"at", a.startedAt}});
    }
    QStringList missing = current.missing + start.missing;
    missing.removeDuplicates();
    a.completeness =
        a.complete ? "Reconciled P&L since " + isoUtc(a.startedAt) : missing.join("; ");
    return a;
}
void Ledger::clear()
{
    invalidateHistory();
    historyHorizon = 0;
    if (!db.transaction())
        throw std::runtime_error("Cannot begin analytics deletion");
    try
    {
        for (auto t : {"meta", "raw_logs", "trades", "events", "legs", "sessions", "samples"})
            sqlExec(db, QString("DELETE FROM %1").arg(t));
        if (!db.commit())
            throw std::runtime_error("Cannot delete analytics");
    }
    catch (...)
    {
        db.rollback();
        throw;
    }
    coreFillSeq = coreEventSeq = lastSample = 0;
    sqlExec(db, "PRAGMA wal_checkpoint(TRUNCATE)");
    sqlExec(db, "VACUUM");
}
} // namespace scalper
