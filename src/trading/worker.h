#pragma once
#include "engine/hedger.h"
#include "exchange/client.h"
#include "storage/store.h"
#include <QHash>
#include <QLockFile>
#include <QObject>
#include <QTimer>
#include <functional>
namespace scalper
{
class TradingWorker : public QObject
{
    Q_OBJECT
  public:
    TradingWorker(QString profile, Config initial, std::shared_ptr<SharedState> state,
                  QUrl endpoint = {});
    ~TradingWorker() override;
  public slots:
    void initialize();
    void connectExchange(QString id, QString secret);
    void applyConfig(Config config);
    void startTrading();
    void stopTrading();
    void closeHedge();
    void deleteData();
    void reportIssue(QString message);
    void shutdown();
  signals:
    void initialized();
    void historyConnect(QString id, QString secret, bool production, QString account,
                        QUrl endpoint);
    void historyStop();
    void deleted();
    void stopped();

  private:
    QString path, account, construction, status = "Disconnected", clientId, clientSecret;
    Config initial;
    std::shared_ptr<SharedState> shared;
    QUrl endpoint;
    std::unique_ptr<Store> store;
    std::unique_ptr<QLockFile> profileLock, accountLock;
    Client* client = nullptr;
    QTimer *pumpTimer = nullptr, *publishTimer = nullptr, *pollTimer = nullptr,
           *marketTimer = nullptr;
    Hedger engine;
    QVector<Order> orders;
    QHash<QString, Option> positions;
    QHash<QString, Instrument> instruments;
    struct JournalMessage
    {
        QString text;
        bool important = false;
    };
    QVector<JournalMessage> messages;
    QHash<QString, qint64> loggedAt;
    QVector<QPair<QString, QJsonObject>> pendingEvents;
    qint64 journalRetryAt = 0, pnlRetryAt = 0, summaryAt = 0;
    quint64 skippedRequests = 0;
    QHash<QString, QJsonObject> pendingPnlStarts;
    quint64 revision = 0, positionRevision = 0;
    int epoch = 0, attempts = 0;
    bool synced = false, syncing = false, busy = false, polling = false, storageFailed = false,
         foreignOrders = false;
    bool desiredClose = false, shuttingDown = false;
    bool selectionReview = false;
    bool refreshingMarket = false, startPending = false;
    bool resettingProfile = false;
    bool recovering = false, waitingData = false;
    enum class RunRequest
    {
        None,
        Hedge,
        Close
    };
    RunRequest requested = RunRequest::None;
    qint64 retryAt = 0;
    int recoveryChecks = 0;
    int recoveryRetryMs = Time::FillRecoveryDelayMs;
    qint64 recoveryReadyAt = 0;
    double externalPosition = 0;
    qint64 requestId = 0, lastFill = 0, positionAt = 0;
    qint64 passiveSince = 0;
    QString hedgeAlert;
    bool passiveAlerted = false;
    qint64 lastHedgeMs = -1;
    std::optional<qint64> exchangeClockOffsetMs;
    double actualPosition = 0, liquidation = 0;
    std::optional<double> accountMakerFee, accountTakerFee;
    double sessionFunding = 0, fundingBaseline = 0;
    qint64 fundingAt = 0, fundingBaselineSession = 0;
    bool fundingValid = false;
    bool guard(const std::function<void()>& operation);
    void log(const QString& message, const QJsonObject& details = {}, bool important = false,
             const QString& category = {});
    void journalMessage(const QString& message, bool important);
    void recordEvent(const QString& kind, const QJsonObject& details = {});
    void flushJournal();
    void flushPnlStart();
    void fail(const QString& reason);
    void save();
    void publish();
    void pump();
    void initializeExchange();
    void loadPositions(std::function<void()> done, bool reconcile = false);
    void loadMetadata(QStringList names, std::function<void()> done);
    void subscribe(std::function<void()> done);
    void reconcile();
    void requestRecovery(const QString& reason);
    void deferRecovery(const QString& reason);
    void recoverPosition();
    void reconcileOrder(int index, std::function<void()> done);
    void resolveAbsentOrder(int index, std::function<void()> done);
    void scanOrderWindow(int index, int phase, QJsonArray accumulated, std::function<void()> done);
    void recoverTrades(int index, std::function<void()> done, bool historical = false);
    void refreshMargin(std::function<void(bool)> done = {});
    void poll();
    void handleSubscription(const QString& channel, const QJsonValue& payload);
    void ticker(const QJsonObject& json);
    void updateSelected();
    void pruneClosedOptions();
    void refreshMarketData(QStringList names, std::function<void(bool)> done);
    void refreshQuietMarkets();
    void waitForData(const QString& reason);
    bool resumeRequested();
    bool acceptRead(const RpcResult& result, const QString& context);
    QStringList marketInstruments(bool onlyStale) const;
    void completeStart();
    void validateStart();
    void startMarketRefresh();
    void receiveOrder(const QJsonObject& json, bool response = false);
    void receiveTrades(const QJsonArray& trades);
    Order* liveOrder();
    Order* findOrder(const QString& label, const QString& id = {});
    bool owns(const Order& order) const;
    void execute(QString method, QJsonObject params, Order& order);
    void submitNew(const HedgeQuote& quote);
    void editOrder(Order& order, const HedgeQuote& quote);
    bool quoteCurrent(const HedgeQuote& quote, quint64 revision);
    void cancelOrder(Order& order);
    void preflight(std::function<void()> action, double amount, double price, bool buy,
                   bool reducing);
    double expectedPosition() const
    {
        return externalPosition + engine.inventory;
    }
    void readProfileResetOrders();
    void cancelProfileLabels(QStringList labels);
    void verifyProfileCancellation(int checks);
    void finishProfileReset();
};
} // namespace scalper
