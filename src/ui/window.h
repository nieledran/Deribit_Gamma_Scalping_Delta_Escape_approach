#pragma once
#include "analytics/worker.h"
#include "trading/worker.h"
#include "ui/apply_button.h"
#include "ui/profile_chart.h"
#include "ui/quote_label.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
namespace scalper
{
class Window : public QMainWindow
{
    Q_OBJECT
  public:
    Window(QString profile, Config config, bool demo = false, QUrl endpoint = {});
    ~Window() override;
    std::shared_ptr<SharedState> sharedState() const
    {
        return shared;
    }
    qint64 lastRenderUs = 0;
    quint64 renderCount = 0;

  protected:
    void closeEvent(QCloseEvent*) override;

  private:
    std::shared_ptr<SharedState> shared = std::make_shared<SharedState>();
    QThread tradingThread, analyticsThread;
    TradingWorker* trading = nullptr;
    AnalyticsWorker* analytics = nullptr;
    QTimer* refresh = nullptr;
    Config initial;
    QString renderedProfile;
    QStringList draftSelected;
    bool demo = false, loaded = false, dirty = false, rendering = false;
    QLineEdit *id = nullptr, *secret = nullptr;
    ApplyButton* applyButton = nullptr;
    QPushButton *connectButton = nullptr, *startButton = nullptr, *stopButton = nullptr,
                *selectButton = nullptr;
    QAction *closeAction = nullptr, *deleteAction = nullptr;
    QDoubleSpinBox *band = nullptr, *coverage = nullptr, *margin = nullptr, *cooldown = nullptr,
                   *makerWait = nullptr, *slippage = nullptr;
    QCheckBox* marginEnabled = nullptr;
    QComboBox* execution = nullptr;
    QTableWidget *positions = nullptr, *pnl = nullptr, *orders = nullptr;
    QTabWidget* tabs = nullptr;
    QLabel *status = nullptr, *latency = nullptr, *risk = nullptr, *estimate = nullptr,
           *quality = nullptr, *netPnl = nullptr, *marginInfo = nullptr;
    QLabel *dataAge = nullptr, *clockDifference = nullptr;
    QLabel *netDeltaValue = nullptr, *hedgeValue = nullptr, *hedgeUsage = nullptr,
           *normalizedValue = nullptr, *referenceValue = nullptr, *escapeValue = nullptr,
           *legsValue = nullptr, *activity = nullptr, *executionState = nullptr,
           *pnlCoverage = nullptr;
    QuoteLabel *bidQuote = nullptr, *askQuote = nullptr;
    QPlainTextEdit* journal = nullptr;
    QDialog* advancedDialog = nullptr;
    QPointer<QDialog> riskDialog, pnlDialog;
    ProfileChart* chart = nullptr;
    QLabel *riskTimestamp = nullptr, *riskIv = nullptr;
    QPushButton* riskRefresh = nullptr;
    quint64 riskRequestId = 0, displayedRiskId = 0;
    void setupUi();
    void setupDialogs();
    void render();
    Config configFromUi() const;
    void populateConfig(const Config& config);
    void markDirty();
    void seedDemo();
    void selectOptions();
    void showRisk();
    void requestRisk();
    void showPnl();
    void showLeg(int row);
};
} // namespace scalper
