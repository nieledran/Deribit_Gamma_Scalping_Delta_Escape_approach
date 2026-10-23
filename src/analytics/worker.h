#pragma once
#include "analytics/ledger.h"
#include "exchange/client.h"
#include <QObject>
#include <QQueue>
#include <QTimer>
namespace scalper
{
class AnalyticsWorker : public QObject
{
    Q_OBJECT
  public:
    AnalyticsWorker(QString corePath, std::shared_ptr<SharedState> shared);
  public slots:
    void initialize();
    void connectHistory(QString id, QString secret, bool production, QString account,
                        QUrl endpoint);
    void disconnectHistory();
    void exportData(QString directory);
    void deleteData();
    void requestRisk(RiskRequest request);
    void shutdown();
  signals:
    void stopped();

  private:
    enum class ImportKind
    {
        Option,
        Perpetual,
        Delivery
    };
    struct Import
    {
        QString name, currency, construction, deliveryKey;
        Option option;
        qint64 start = 0, end = 0;
        QJsonValue cursor;
        ImportKind kind = ImportKind::Option;
    };
    QString corePath, account, error, historyStatus;
    std::shared_ptr<SharedState> shared;
    std::unique_ptr<Ledger> ledger;
    Client* history = nullptr;
    QTimer *timer = nullptr, *historyTimer = nullptr;
    QQueue<Import> imports;
    QSet<QString> scheduled;
    bool busy = false, ready = false;
    int epoch = 0;
    quint64 revision = 0;
    qint64 lastFundingRefresh = 0;
    void guard(const std::function<void()>& operation);
    TradingSnapshot snapshot() const;
    bool coherentSnapshot(const TradingSnapshot& snapshot) const;
    void update();
    void authenticateHistory();
    void verifyHistoryAccount(int generation, const RpcResult& result);
    void scheduleImports(const TradingSnapshot& snapshot);
    void queueOptionImport(const TradingSnapshot& snapshot, const Option& option);
    void queueDeliveryImport(const TradingSnapshot& snapshot, const Option& option);
    void queueFundingImport(const TradingSnapshot& snapshot);
    void nextImport();
    void receiveImport(int generation, const Import& job, const RpcResult& response);
    void finishImport(const Import& job);
};
} // namespace scalper
