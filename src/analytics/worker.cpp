#include "analytics/worker.h"
#include "analytics/model.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <algorithm>
namespace scalper
{
AnalyticsWorker::AnalyticsWorker(QString p, std::shared_ptr<SharedState> s)
    : corePath(std::move(p)), shared(std::move(s))
{
}
void AnalyticsWorker::guard(const std::function<void()>& f)
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        error = QString::fromUtf8(e.what());
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->analytics.message = "Analytics error: " + error;
    }
}
TradingSnapshot AnalyticsWorker::snapshot() const
{
    std::lock_guard<std::mutex> lock(shared->mutex);
    return shared->trading;
}
void AnalyticsWorker::initialize()
{
    timer = new QTimer(this);
    timer->setInterval(Time::AnalyticsIntervalMs);
    connect(timer, &QTimer::timeout, this, [this] { guard([&] { update(); }); });
    timer->start();
    historyTimer = new QTimer(this);
    historyTimer->setInterval(History::RequestIntervalMs);
    connect(historyTimer, &QTimer::timeout, this, [this] { guard([&] { nextImport(); }); });
    historyTimer->start();
    history = new Client(this);
    connect(history, &Client::authenticatedSession, this,
            [this] { guard([&] { authenticateHistory(); }); });
    connect(history, &Client::lost, this,
            [this]
            {
                ready = false;
                busy = false;
                historyStatus =
                    "History disconnected; queued imports will resume after authentication";
            });
    connect(history, &Client::notice, this, [this](const QString& text) { historyStatus = text; });
}
bool AnalyticsWorker::coherentSnapshot(const TradingSnapshot& s) const
{
    if (!s.quote.fresh(s.timestamp) || s.config.selected.isEmpty())
        return false;
    for (const auto& name : s.config.selected)
    {
        auto it = std::find_if(s.positions.begin(), s.positions.end(),
                               [&](const Option& o) { return o.spec.name == name; });
        if (it == s.positions.end())
            return false;
        if (std::abs(it->quantity) > Epsilon && !it->freshGreeks(s.timestamp))
            return false;
    }
    return true;
}
void AnalyticsWorker::update()
{
    QElapsedTimer elapsed;
    elapsed.start();
    if (!ledger)
    {
        if (!QFileInfo::exists(corePath))
            return;
        ledger = std::make_unique<Ledger>(corePath, corePath + ".analytics.db");
    }
    ledger->syncCore();
    auto s = snapshot();
    if (!s.synced || s.constructionId.isEmpty() || s.quote.mid() <= 0)
        return;
    const bool coherent = coherentSnapshot(s);
    ledger->ensureLegs(s);
    auto a = ledger->report(s);
    a.revision = ++revision;
    a.historyStatus = historyStatus;
    if (!coherent)
        a.completeness = "Market data stale; last available valuation. " + a.completeness;
    if (!error.isEmpty())
        a.message = "Analytics error: " + error;
    if (coherent)
    {
        Valuation valuation;
        valuation.pnl = ledger->value(s).pnl;
        valuation.complete = a.complete;
        ledger->sample(s, valuation, &a);
    }
    a.calculationUs = elapsed.nsecsElapsed() / 1000;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        QString message = shared->analytics.message;
        if (!message.isEmpty() && a.message.isEmpty())
            a.message = message;
        shared->analytics = std::move(a);
    }
    if (ready)
        scheduleImports(s);
}
void AnalyticsWorker::requestRisk(RiskRequest request)
{
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->activeRiskRequest != request.id)
            return;
    }
    RiskResult result;
    result.id = request.id;
    const auto& s = request.trading;
    result.timestamp = s.timestamp;
    result.spot = s.quote.mid();
    try
    {
        if (!s.synced || !coherentSnapshot(s) || (!request.demo && !s.quote.fresh(utcNow())))
            throw std::runtime_error(
                "Fresh, reconciled market data and selected options are required");
        for (const auto& option : s.positions)
            if (!request.demo && s.config.selected.contains(option.spec.name) &&
                std::abs(option.quantity) > Epsilon && !option.freshGreeks(utcNow()))
                throw std::runtime_error(
                    "Selected option data became stale before graph calculation");
        Pnl pnl = request.demoPnl;
        result.complete = request.demo;
        if (!request.demo)
        {
            if (!ledger)
                throw std::runtime_error("P&L history is not ready yet");
            ledger->syncCore();
            const auto report = ledger->report(s);
            if (!report.ready)
                throw std::runtime_error("P&L begins at the first Start of this construction");
            pnl = report.pnl;
            result.complete = report.complete;
        }
        QVector<Option> selected;
        for (const auto& option : s.positions)
            if (s.config.selected.contains(option.spec.name) && option.isOpen(s.timestamp))
                selected.append(option);
        if (selected.isEmpty())
            throw std::runtime_error("No open selected option positions");
        result.curve = portfolioCurve(selected, s.inventory, result.spot, pnl.net(), s.timestamp);
        const double reference = s.anchored ? s.referenceDelta : aggregate(selected).normalized();
        result.referenceDelta = reference;
        result.deltaChange = s.config.deltaChange;
        for (const auto& option : selected)
            result.impliedVols.append({option.spec.name, option.iv});
        result.lower = s.hedgeQuotes.buy.enabled ? std::optional<double>(s.hedgeQuotes.buy.price) : std::nullopt;
        result.upper = s.hedgeQuotes.sell.enabled ? std::optional<double>(s.hedgeQuotes.sell.price) : std::nullopt;
        for (const auto& order : s.orders)
            if (!order.terminal && !order.uncertain)
            {
                if (order.role == s.hedgeQuotes.buy.role && s.hedgeQuotes.buy.enabled)
                    result.lower = order.price;
                if (order.role == s.hedgeQuotes.sell.role && s.hedgeQuotes.sell.enabled)
                    result.upper = order.price;
            }
    }
    catch (const std::exception& e)
    {
        result.problem = QString::fromUtf8(e.what());
    }
    std::lock_guard<std::mutex> lock(shared->mutex);
    ++shared->riskCalculations;
    if (shared->activeRiskRequest == request.id)
        shared->risk = std::move(result);
}
void AnalyticsWorker::exportData(QString directory)
{
    guard(
        [&]
        {
            if (!ledger)
                throw std::runtime_error("No ledger is available to export");
            ledger->syncCore();
            auto path = ledger->exportCsv(directory);
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->analytics.message = "CSV export saved: " + path;
        });
}
void AnalyticsWorker::deleteData()
{
    disconnectHistory();
    guard(
        [&]
        {
            if (ledger)
                ledger->clear();
            scheduled.clear();
            error.clear();
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->analytics = AnalyticsSnapshot{};
        });
}
void AnalyticsWorker::shutdown()
{
    if (timer)
        timer->stop();
    if (historyTimer)
        historyTimer->stop();
    disconnectHistory();
    ledger.reset();
    emit stopped();
}
} // namespace scalper
