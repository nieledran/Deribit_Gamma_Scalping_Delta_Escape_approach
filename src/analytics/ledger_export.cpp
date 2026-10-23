#include "analytics/ledger.h"
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <QMap>
#include <QTimeZone>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <algorithm>
namespace scalper
{
static QString csvValue(const QJsonValue& value)
{
    switch (value.type())
    {
    case QJsonValue::Double:
        return QString::number(value.toDouble(), 'g', 17);
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Bool:
        return value.toBool() ? "true" : "false";
    case QJsonValue::Object:
        return compact(value.toObject());
    case QJsonValue::Array:
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return {};
    }
    return {};
}

static QString csvCell(QString s)
{
    // Prevent Excel from executing formulas embedded in external text fields.
    bool numeric = false;
    s.toDouble(&numeric);
    if (!s.isEmpty() && (QString("=+@\t\r").contains(s.front()) || (s.front() == '-' && !numeric)))
        s.prepend('\'');
    s.replace('"', "\"\"");
    return '"' + s + '"';
}
static QJsonObject flatObject(const QJsonObject& value, const QString& prefix = {})
{
    QJsonObject result;
    for (auto it = value.begin(); it != value.end(); ++it)
    {
        QString key = prefix + it.key();
        if (it->isObject())
        {
            auto nested = flatObject(it->toObject(), key + ".");
            for (auto n = nested.begin(); n != nested.end(); ++n)
                result[n.key()] = n.value();
        }
        else
            result[key] = it.value();
    }
    return result;
}
QString Ledger::exportCsv(const QString& dir)
{
    QDir d(dir);
    if (!d.exists() && !d.mkpath("."))
        throw std::runtime_error("Cannot create export directory");
    QString folder = d.filePath("DeribitGamma-" +
                                QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
    QDir().mkpath(folder);
    struct Export
    {
        QString name, query;
    };
    const QVector<Export> exports = {
        {"summaries", "SELECT at,construction,pnl,complete FROM samples ORDER BY at"},
        {"trades", "SELECT at,construction,data FROM trades ORDER BY at"},
        {"fees_funding",
         "SELECT at,currency,instrument,data FROM raw_logs ORDER BY at,CAST(log_id AS INTEGER)"},
        {"greeks", "SELECT at,construction,snapshot FROM samples ORDER BY at"},
        {"events", "SELECT at,kind,data FROM events ORDER BY at"},
        {"sessions", "SELECT at,id,construction,baseline FROM sessions ORDER BY at"}};
    // A read transaction keeps all exported tables at one consistent ledger revision.
    if (!db.transaction())
        throw std::runtime_error("Cannot begin export snapshot");
    try
    {
        for (const auto& spec : exports)
        {
            QSqlQuery q(db);
            if (!q.exec(spec.query))
                throw std::runtime_error(q.lastError().text().toStdString());
            QSaveFile file(QDir(folder).filePath(spec.name + ".csv"));
            if (!file.open(QIODevice::WriteOnly))
                throw std::runtime_error("Cannot write CSV export");
            file.write("\xEF\xBB\xBF");
            if (spec.name == "greeks")
            {
                QStringList fields = {"Timestamp UTC",
                                      "Construction",
                                      "Scope",
                                      "Instrument",
                                      "Quantity coin",
                                      "Option delta",
                                      "Position delta coin",
                                      "Normalized delta",
                                      "Position gamma per USD",
                                      "Position theta API",
                                      "Position vega API",
                                      "Mark native",
                                      "Mark USD equivalent",
                                      "IV percent",
                                      "Index",
                                      "Underlying",
                                      "Expiry UTC",
                                      "Ticker UTC",
                                      "Theta lifetime capped",
                                      "Raw JSON"};
                auto write = [&](const QStringList& values)
                {
                    QStringList line;
                    for (const auto& value : values)
                        line << csvCell(value);
                    file.write((line.join(',') + "\r\n").toUtf8());
                };
                auto num = [](double x) { return QString::number(x, 'g', 17); };
                write(fields);
                while (q.next())
                {
                    auto snapshot = restoreSnapshot(object(q.value(2).toString()));
                    for (const auto& o : snapshot.positions)
                        write({isoUtc(snapshot.timestamp),
                               snapshot.constructionId,
                               "Option",
                               o.spec.name,
                               num(o.quantity),
                               num(o.delta),
                               num(o.quantity * o.delta),
                               QString(),
                               num(o.quantity * o.gamma),
                               num(o.quantity * o.theta),
                               num(o.quantity * o.vega),
                               num(o.mark),
                               num(o.markUsd()),
                               num(o.iv * 100),
                               num(o.index),
                               num(o.underlying),
                               isoUtc(o.spec.expiry),
                               isoUtc(o.tickerAt),
                               o.spec.expiry - snapshot.timestamp < Time::DayMs ? "true" : "false",
                               compact(o.json())});
                    auto g = aggregate(snapshot.positions);
                    write({isoUtc(snapshot.timestamp),
                           snapshot.constructionId,
                           "Aggregate",
                           snapshot.config.base(),
                           num(g.gross),
                           QString(),
                           num(g.delta),
                           num(g.normalized()),
                           num(g.gamma),
                           num(g.theta),
                           num(g.vega),
                           QString(),
                           QString(),
                           QString(),
                           QString(),
                           QString(),
                           QString(),
                           QString(),
                           QString(),
                           QString()});
                }
                if (!file.commit())
                    throw std::runtime_error("Cannot commit Greeks CSV export");
                continue;
            }
            QStringList headers = {"Timestamp UTC", "Construction / Currency / Event",
                                   "Instrument / Data", "Data / Complete"};
            // Flatten JSON payloads into usable spreadsheet columns, keeping raw JSON as a final
            // audit column.
            QSet<QString> keys;
            while (q.next())
            {
                QVariantList row;
                for (int i = 0; i < q.record().count(); ++i)
                    row.append(q.value(i));
                for (const auto& cell : row)
                    if (cell.toString().startsWith('{'))
                        for (auto k : flatObject(object(cell.toString())).keys())
                            keys.insert(k);
            }
            QStringList fields = keys.values();
            std::sort(fields.begin(), fields.end());
            headers.clear();
            for (int i = 0; i < q.record().count(); ++i)
                headers << q.record().fieldName(i);
            headers << fields;
            QStringList line;
            for (auto h : headers)
                line << csvCell(h == "at" ? "Timestamp UTC" : h);
            file.write((line.join(',') + "\r\n").toUtf8());
            q.finish();
            if (!q.exec(spec.query))
                throw std::runtime_error("Cannot stream export rows");
            while (q.next())
            {
                QVariantList row;
                for (int i = 0; i < q.record().count(); ++i)
                    row.append(q.value(i));
                line.clear();
                QJsonObject flattened;
                for (int i = 0; i < row.size(); ++i)
                {
                    QString text = row[i].toString();
                    if (q.record().fieldName(i) == "at")
                        text = isoUtc(row[i].toLongLong());
                    line << csvCell(text);
                    if (text.startsWith('{'))
                    {
                        auto j = flatObject(object(text));
                        for (auto it = j.begin(); it != j.end(); ++it)
                            flattened[it.key()] = it.value();
                    }
                }
                for (const auto& key : fields)
                {
                    auto x = flattened[key];
                    line << csvCell(csvValue(x));
                }
                file.write((line.join(',') + "\r\n").toUtf8());
            }
            if (!file.commit())
                throw std::runtime_error("Cannot commit CSV export");
        }
        // Daily summaries use recorded valuations only; gaps are never interpolated.
        struct Day
        {
            QString construction, date;
            qint64 first = 0, last = 0, started = 0;
            Pnl opening, closing;
            bool complete = true;
        };
        QMap<QString, Day> days;
        QSqlQuery samples(db);
        if (!samples.exec("SELECT at,construction,pnl,complete FROM samples ORDER BY construction,at"))
            throw std::runtime_error("Cannot read daily valuations");
        QString previousConstruction;
        qint64 previousAt = 0;
        while (samples.next())
        {
            const auto data = object(samples.value(2).toString());
            if (!data.contains("construction_pnl"))
                continue; // Preserve legacy raw exports without inventing a new start.
            const qint64 at = samples.value(0).toLongLong();
            const QString construction = samples.value(1).toString();
            const QString date = QDateTime::fromMSecsSinceEpoch(at, QTimeZone::utc()).date().toString(Qt::ISODate);
            auto& day = days[construction + "/" + date];
            if (!day.first)
            {
                day.construction = construction;
                day.date = date;
                day.first = at;
                day.started = data["started_at"].toInteger();
                day.opening = Pnl::fromJson(data["construction_pnl"].toObject());
                const qint64 midnight = QDateTime(QDate::fromString(date, Qt::ISODate), QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch();
                day.complete = at - std::max(midnight, day.started) <= 2000;
            }
            if (previousConstruction == construction && at - previousAt > Time::MarketDataMaxAgeMs)
                day.complete = false;
            day.last = at;
            day.closing = Pnl::fromJson(data["construction_pnl"].toObject());
            day.complete = day.complete && samples.value(3).toBool();
            previousAt = at;
            previousConstruction = construction;
        }
        QSaveFile daily(QDir(folder).filePath("daily.csv"));
        if (!daily.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot write daily CSV");
        daily.write("\xEF\xBB\xBF");
        daily.write("Date UTC,Construction,From UTC,To UTC,Options,Hedge realized,Hedge unrealized,Fees,Funding,Net P&L,Coverage\r\n");
        for (const auto& day : days)
        {
            const auto pnl = day.closing - day.opening;
            const qint64 next = QDateTime(QDate::fromString(day.date, Qt::ISODate).addDays(1), QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch();
            const bool complete = day.complete && next - day.last <= 2000;
            QStringList row{day.date, day.construction, isoUtc(day.first), isoUtc(day.last),
                            QString::number(pnl.options, 'g', 17), QString::number(pnl.realized, 'g', 17),
                            QString::number(pnl.unrealized, 'g', 17), QString::number(pnl.optionFees + pnl.hedgeFees, 'g', 17),
                            QString::number(pnl.funding, 'g', 17), QString::number(pnl.net(), 'g', 17),
                            complete ? "Complete recorded period" : "Partial recorded period"};
            for (auto& cell : row)
                cell = csvCell(cell);
            daily.write((row.join(',') + "\r\n").toUtf8());
        }
        if (!daily.commit())
            throw std::runtime_error("Cannot commit daily CSV");
        if (!db.commit())
            throw std::runtime_error("Cannot finish export snapshot");
    }
    catch (...)
    {
        db.rollback();
        throw;
    }
    return folder;
}
} // namespace scalper
