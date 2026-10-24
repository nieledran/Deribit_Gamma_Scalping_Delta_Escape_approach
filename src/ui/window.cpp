#include "ui/window.h"
#include "ui/dialogs.h"
#include <QCloseEvent>
#include <QTimer>
namespace scalper
{
Window::Window(QString profile, Config c, bool isDemo, QUrl endpoint)
    : initial(std::move(c)), demo(isDemo)
{
    setupUi();
    analytics = new AnalyticsWorker(profile, shared);
    analytics->moveToThread(&analyticsThread);
    connect(&analyticsThread, &QThread::finished, analytics, &QObject::deleteLater);
    if (demo)
        seedDemo();
    else
    {
        trading = new TradingWorker(profile, initial, shared, endpoint);
        trading->moveToThread(&tradingThread);
        connect(&tradingThread, &QThread::started, trading, &TradingWorker::initialize);
        connect(&analyticsThread, &QThread::started, analytics, &AnalyticsWorker::initialize);
        connect(&tradingThread, &QThread::finished, trading, &QObject::deleteLater);
        connect(trading, &TradingWorker::historyConnect, analytics,
                &AnalyticsWorker::connectHistory);
        connect(trading, &TradingWorker::historyStop, analytics,
                &AnalyticsWorker::disconnectHistory);
        connect(trading, &TradingWorker::deleted, analytics, &AnalyticsWorker::deleteData);
        connect(trading, &TradingWorker::deleted, this,
                [this]
                {
                    id->clear();
                    secret->clear();
                });
        tradingThread.start(QThread::HighPriority);
    }
    analyticsThread.start(QThread::LowPriority);
    refresh = new QTimer(this);
    refresh->setInterval(Time::UiRefreshIntervalMs);
    connect(refresh, &QTimer::timeout, this, &Window::render);
    refresh->start();
    render();
}
Window::~Window()
{
    refresh->stop();
    if (trading && tradingThread.isRunning())
        QMetaObject::invokeMethod(trading, &TradingWorker::shutdown, Qt::BlockingQueuedConnection);
    if (analytics && analyticsThread.isRunning())
        QMetaObject::invokeMethod(analytics, &AnalyticsWorker::shutdown,
                                  Qt::BlockingQueuedConnection);
    tradingThread.quit();
    analyticsThread.quit();
    tradingThread.wait();
    analyticsThread.wait();
}
void Window::markDirty()
{
    if (!rendering)
        dirty = true;
}
void Window::populateConfig(const Config& c)
{
    const bool wasRendering = rendering;
    rendering = true;
    band->setValue(c.deltaChange);
    coverage->setValue(c.hedgePercent);
    marginEnabled->setChecked(c.marginLimitEnabled);
    margin->setValue(c.maxMarginUsage > 0 ? c.maxMarginUsage * 100 : 50);
    margin->setEnabled(c.marginLimitEnabled);
    cooldown->setValue(c.cooldownMs / 1000.0);
    makerWait->setValue(c.makerWaitMs / 1000.0);
    balancedAttempts->setValue(c.balancedPassiveAttempts);
    aggressiveAttempts->setValue(c.aggressivePassiveAttempts);
    slippage->setValue(c.slippageBps);
    execution->setCurrentIndex(int(c.execution));
    rendering = wasRendering;
}
Config Window::configFromUi() const
{
    Config c = initial;
    c.deltaChange = band->value();
    c.hedgePercent = coverage->value();
    c.marginLimitEnabled = marginEnabled->isChecked();
    c.maxMarginUsage = c.marginLimitEnabled ? margin->value() / 100 : 0;
    c.cooldownMs = int(cooldown->value() * 1000);
    c.makerWaitMs = int(makerWait->value() * 1000);
    c.balancedPassiveAttempts = balancedAttempts->value();
    c.aggressivePassiveAttempts = aggressiveAttempts->value();
    c.slippageBps = slippage->value();
    c.execution = Execution(execution->currentIndex());
    c.selected = draftSelected;
    return c;
}
void Window::closeEvent(QCloseEvent* event)
{
    TradingSnapshot s;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        s = shared->trading;
    }
    if (!demo && (s.running || s.busy) &&
        !confirmAction(this, "Stop and exit",
                       "Stop the algorithm and disconnect? Accepted bot orders are cancelled by "
                       "connection cancel-on-disconnect. Hedge positions remain open."))
    {
        event->ignore();
        return;
    }
    event->accept();
}
} // namespace scalper
