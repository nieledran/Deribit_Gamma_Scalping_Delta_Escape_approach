#pragma once
#include "engine/hedger.h"
#include <QHash>
#include <QSqlDatabase>
namespace scalper
{
QString compact(const QJsonObject& json);
QJsonObject object(const QString& text);
void sqlExec(QSqlDatabase db, const QString& statement, const QVariantList& values = {});
class Store
{
  public:
    explicit Store(const QString& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    QJsonObject metadata(const QString& key) const;
    void metadata(const QString& key, const QJsonObject& value);
    QString profileId() const
    {
        return id;
    }
    void bind(const QString& account);
    bool load(Hedger& engine, QVector<Order>& orders);
    void save(const Hedger& engine, const QVector<Order>& orders);
    void event(const QString& kind, const QJsonObject& data = {}, qint64 at = 0);
    void fill(const QJsonObject& trade);
    void fillMany(const QJsonArray& trades);
    double coveredQuantity(const QString& orderId) const;
    void resetProfile(const Hedger& fresh, const QString& construction, const QString& account,
                      double externalPosition);
    QSqlDatabase database() const
    {
        return db;
    }

  private:
    QSqlDatabase db;
    QString connection, id;
    QHash<QString, QString> savedOrders;
    mutable QHash<QString, double> coverageCache;
    QString savedState;
};
} // namespace scalper
