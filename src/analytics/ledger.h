#pragma once
#include "storage/store.h"
#include <QSet>
namespace scalper
{
struct HedgeBook
{
    double quantity = 0, average = 0, realized = 0, fees = 0;
    void trade(double signedQuantity, double price, double fee);
};
struct Valuation
{
    Pnl pnl;
    bool complete = true;
    QStringList missing;
    qint64 first = 0;
};
class Ledger
{
  public:
    Ledger(const QString& corePath, const QString& analyticsPath);
    ~Ledger();
    void syncCore();
    void ensureLegs(const TradingSnapshot& snapshot);
    void logEntry(const QString& currency, const QJsonObject& entry);
    void logEntries(const QString& currency, const QJsonArray& entries);
    void finishImport(const QString& construction, const Option& option);
    Valuation value(const TradingSnapshot& snapshot) const;
    Valuation valueUncached(const TradingSnapshot& snapshot) const;
    void sample(const TradingSnapshot& snapshot, const Valuation& valuation,
                const AnalyticsSnapshot* report = nullptr);
    AnalyticsSnapshot report(const TradingSnapshot& snapshot);
    QString exportCsv(const QString& directory);
    void clear();
    QJsonObject metadata(const QString& key) const;
    void metadata(const QString& key, const QJsonObject& value);
    QSqlDatabase database() const
    {
        return db;
    }
    static QJsonObject valuationSnapshot(const TradingSnapshot& snapshot);
    static TradingSnapshot restoreSnapshot(const QJsonObject& json);

  private:
    QSqlDatabase db, core;
    QString connection, coreConnection;
    qint64 coreFillSeq = 0, coreEventSeq = 0, lastSample = 0;
    quint64 historyRevision = 0;
    qint64 historyHorizon = 0;
    struct CachedValue
    {
        Valuation valuation;
        TradingSnapshot snapshot;
        QHash<QString, double> markExposure;
        double hedge = 0, funding = 0;
    };
    mutable QHash<QByteArray, CachedValue> valuations;
    Valuation evaluate(const TradingSnapshot&, QHash<QString, double>* exposure = nullptr,
                       double* hedge = nullptr, double* funding = nullptr) const;
    void invalidateHistory();
    QVector<QJsonObject> logs(const QString& instrument, qint64 cutoff) const;
    QVector<QJsonObject> trades(const QString& construction, qint64 cutoff) const;
};
} // namespace scalper
