#include "storage/store.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>
namespace scalper
{
QString compact(const QJsonObject& j)
{
    return QString::fromUtf8(QJsonDocument(j).toJson(QJsonDocument::Compact));
}
QJsonObject object(const QString& s)
{
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(s.toUtf8(), &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        throw std::runtime_error("Invalid persisted JSON");
    return d.object();
}
void sqlExec(QSqlDatabase db, const QString& text, const QVariantList& values)
{
    QSqlQuery q(db);
    if (!q.prepare(text))
        throw std::runtime_error(q.lastError().text().toStdString());
    for (const auto& v : values)
        q.addBindValue(v);
    if (!q.exec())
        throw std::runtime_error(q.lastError().text().toStdString());
}
Store::Store(const QString& path)
{
    try
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        connection = "core_" + QUuid::createUuid().toString(QUuid::Id128);
        db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(path);
        if (!db.open())
            throw std::runtime_error(db.lastError().text().toStdString());
        {
            QSqlQuery q(db);
            if (!q.exec("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                        "'sqlite_%'"))
                throw std::runtime_error("Cannot inspect profile format");
            QSet<QString> tables;
            while (q.next())
                tables.insert(q.value(0).toString());
            if (!tables.isEmpty() && (!tables.contains("meta") || !tables.contains("orders") ||
                                      !tables.contains("fills")))
                throw std::runtime_error("This is not a gamma profile; legacy grid and analytics "
                                         "databases cannot be imported");
        }
        sqlExec(db, "PRAGMA journal_mode=WAL");
        sqlExec(db, "PRAGMA synchronous=FULL");
        sqlExec(db, "PRAGMA busy_timeout=1000");
        sqlExec(db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,data TEXT NOT NULL)");
        sqlExec(db,
                "CREATE TABLE IF NOT EXISTS orders(label TEXT PRIMARY KEY,exchange_id TEXT,data "
                "TEXT NOT NULL)");
        sqlExec(db,
                "CREATE TABLE IF NOT EXISTS fills(seq INTEGER PRIMARY KEY AUTOINCREMENT,fill_key "
                "TEXT UNIQUE NOT NULL,at INTEGER NOT NULL,data TEXT NOT NULL)");
        QSet<QString> columns;
        {
            QSqlQuery q(db);
            q.exec("PRAGMA table_info(fills)");
            while (q.next())
                columns.insert(q.value(1).toString());
        }
        if (!columns.contains("order_id"))
            sqlExec(db, "ALTER TABLE fills ADD COLUMN order_id TEXT NOT NULL DEFAULT ''");
        if (!columns.contains("quantity"))
            sqlExec(db, "ALTER TABLE fills ADD COLUMN quantity REAL NOT NULL DEFAULT 0");
        sqlExec(db, "CREATE INDEX IF NOT EXISTS fills_order ON fills(order_id)");
        {
            QSqlQuery q(db);
            q.exec("SELECT seq,data FROM fills WHERE order_id=''");
            while (q.next())
            {
                auto j = object(q.value(1).toString());
                sqlExec(db, "UPDATE fills SET order_id=?,quantity=? WHERE seq=?",
                        {j["order_id"].toString(), j["amount"].toDouble(), q.value(0)});
            }
        }
        sqlExec(db, "CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY AUTOINCREMENT,at "
                    "INTEGER NOT NULL,kind TEXT NOT NULL,data TEXT NOT NULL)");
        id = metadata("profile")["id"].toString();
        if (id.isEmpty())
        {
            id = QUuid::createUuid().toString(QUuid::Id128).left(16);
            metadata("profile", {{"id", id}});
        }
    }
    catch (...)
    {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
        throw;
    }
}
Store::~Store()
{
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}
QJsonObject Store::metadata(const QString& key) const
{
    QSqlQuery q(db);
    q.prepare("SELECT data FROM meta WHERE key=?");
    q.addBindValue(key);
    if (!q.exec())
        throw std::runtime_error(q.lastError().text().toStdString());
    return q.next() ? object(q.value(0).toString()) : QJsonObject{};
}
void Store::metadata(const QString& key, const QJsonObject& value)
{
    sqlExec(db, "INSERT OR REPLACE INTO meta VALUES(?,?)", {key, compact(value)});
}
void Store::bind(const QString& account)
{
    auto prior = metadata("account")["identity"].toString();
    if (!prior.isEmpty() && prior != account)
        throw std::runtime_error("Profile belongs to another account or environment");
    metadata("account", {{"identity", account}});
}
bool Store::load(Hedger& e, QVector<Order>& orders)
{
    auto state = metadata("engine");
    if (state.isEmpty())
        return false;
    e.restore(state);
    savedState = compact(state);
    QSqlQuery q(db);
    if (!q.exec("SELECT data FROM orders ORDER BY rowid"))
        throw std::runtime_error(q.lastError().text().toStdString());
    QVector<Order> history;
    while (q.next())
        history.append(Order::fromJson(object(q.value(0).toString())));
    q.finish();
    for (int i = 0; i < history.size(); ++i)
    {
        const auto& o = history[i];
        if (i >= history.size() - Limits::RecentOrders || !o.terminal || o.uncertain ||
            !o.mutation.isEmpty() || std::abs(coveredQuantity(o.id) - o.filled) > Epsilon)
        {
            orders.append(o);
            savedOrders[o.label] = compact(o.json());
        }
    }
    return true;
}
void Store::save(const Hedger& e, const QVector<Order>& orders)
{
    QString state = compact(e.json());
    if (!db.transaction())
        throw std::runtime_error(db.lastError().text().toStdString());
    QHash<QString, QString> changed;
    try
    {
        if (state != savedState)
            metadata("engine", e.json());
        for (const auto& o : orders)
        {
            QString s = compact(o.json());
            if (savedOrders.value(o.label) != s)
            {
                sqlExec(db, "INSERT OR REPLACE INTO orders VALUES(?,?,?)", {o.label, o.id, s});
                changed[o.label] = s;
            }
        }
        if (!db.commit())
            throw std::runtime_error(db.lastError().text().toStdString());
    }
    catch (...)
    {
        db.rollback();
        coverageCache.clear();
        throw;
    }
    savedState = state;
    for (auto it = changed.begin(); it != changed.end(); ++it)
        savedOrders[it.key()] = it.value();
}
void Store::event(const QString& kind, const QJsonObject& data, qint64 at)
{
    sqlExec(db, "INSERT INTO events(at,kind,data) VALUES(?,?,?)",
            {at ? at : utcNow(), kind, compact(data)});
}
void Store::fill(const QJsonObject& trade)
{
    QString key = trade["instrument_name"].toString() + "/" + trade["trade_id"].toString();
    if (trade["trade_id"].toString().isEmpty())
        throw std::runtime_error("Fill without trade ID");
    sqlExec(db, "INSERT OR IGNORE INTO fills(fill_key,at,data,order_id,quantity) VALUES(?,?,?,?,?)",
            {key, trade["timestamp"].toInteger(), compact(trade), trade["order_id"].toString(),
             number(trade, "amount")});
    coverageCache.remove(trade["order_id"].toString());
}
void Store::fillMany(const QJsonArray& trades)
{
    if (trades.isEmpty())
        return;
    if (!db.transaction())
        throw std::runtime_error("Cannot begin durable fill batch");
    try
    {
        for (auto v : trades)
            fill(v.toObject());
        if (!db.commit())
            throw std::runtime_error("Cannot commit durable fill batch");
    }
    catch (...)
    {
        db.rollback();
        coverageCache.clear();
        throw;
    }
}
double Store::coveredQuantity(const QString& orderId) const
{
    if (coverageCache.contains(orderId))
        return coverageCache[orderId];
    QSqlQuery q(db);
    q.prepare("SELECT COALESCE(SUM(quantity),0) FROM fills WHERE order_id=?");
    q.addBindValue(orderId);
    if (!q.exec() || !q.next())
        throw std::runtime_error("Cannot verify fill coverage");
    double quantity = q.value(0).toDouble();
    coverageCache[orderId] = quantity;
    return quantity;
}
void Store::resetProfile(const Hedger& fresh, const QString& construction, const QString& account,
                         double externalPosition)
{
    const auto nextId = QUuid::createUuid().toString(QUuid::Id128).left(16);
    const auto state = fresh.json();
    if (!db.transaction())
        throw std::runtime_error("Cannot begin profile reset");
    try
    {
        for (const auto& table : {"meta", "orders", "fills", "events"})
            sqlExec(db, QString("DELETE FROM %1").arg(table));
        metadata("profile", {{"id", nextId}});
        metadata("engine", state);
        metadata("construction", {{"id", construction}});
        metadata("account", {{"identity", account}});
        metadata("external_position", {{"quantity", externalPosition}});
        if (!db.commit())
            throw std::runtime_error("Cannot commit profile reset");
    }
    catch (...)
    {
        db.rollback();
        throw;
    }
    id = nextId;
    savedOrders.clear();
    coverageCache.clear();
    savedState = compact(state);
}
} // namespace scalper
