#include "analytics/model.h"
#include "fixtures.h"
#include "ui/theme.h"
#include "ui/window.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <memory>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
using namespace scalper;
static double cpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    auto value = [](FILETIME time)
    { return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return double(value(kernel) + value(user)) / 1e7;
#else
    return 0;
#endif
}
static double percentile(QVector<qint64> values, double fraction)
{
    std::sort(values.begin(), values.end());
    return values.isEmpty() ? 0 : double(values[int((values.size() - 1) * fraction)]);
}
// Original continuous delta-only chart workload, retained only for comparison.
static void legacyScenario(const TradingSnapshot& s)
{
    double total = 0;
    for (int i = 0; i < 241; ++i)
    {
        const double p = s.quote.mid() * (.85 + .30 * i / 240);
        for (const auto& o : s.positions)
        {
            auto current = optionModel(o, o.index, s.timestamp);
            auto shifted = optionModel(o, o.index * p / s.quote.mid(), s.timestamp);
            total += shifted.usd - current.usd + shifted.delta - current.delta;
        }
    }
    const auto lower = escapePrice(s.positions, s.referenceDelta, s.config.deltaChange,
                                   s.quote.mid(), s.timestamp, false);
    const auto upper = escapePrice(s.positions, s.referenceDelta, s.config.deltaChange,
                                   s.quote.mid(), s.timestamp, true);
    if (!std::isfinite(total) || !lower || !upper)
        qFatal("Invalid synthetic scenario");
}
class PerformanceTests : public QObject
{
    Q_OBJECT
  private slots:
    void changingMarketAndFourWindows()
    {
        theme::apply(*qobject_cast<QApplication*>(QCoreApplication::instance()));
        QTemporaryDir directory;
        Store core(directory.filePath("core.db"));
        Ledger ledger(directory.filePath("core.db"), directory.filePath("analytics.db"));
        TradingSnapshot s;
        s.timestamp = utcNow();
        s.config = fixtures::engine(s.timestamp).config;
        s.constructionId = "benchmark";
        s.positions = {fixtures::option(s.timestamp)};
        s.quote = {99999, 100001, 100000, 100000, s.timestamp};
        s.referenceDelta = .5;
        ledger.ensureLegs(s);
        QJsonArray rows;
        for (int i = 0; i < 2001; ++i)
        {
            const bool buy = i == 0 || i % 2;
            const double amount = i == 0 ? .1 : .001;
            rows.append(QJsonObject{{"id", i + 1},
                                    {"instrument_name", s.positions[0].spec.name},
                                    {"timestamp", s.timestamp - 100000 + i},
                                    {"type", "trade"},
                                    {"side", buy ? "open buy" : "close sell"},
                                    {"amount", amount},
                                    {"position", i == 0 || i % 2 == 0 ? .1 : .101},
                                    {"price", .05},
                                    {"cashflow", buy ? -amount * .05 : amount * .05},
                                    {"index_price", 100000},
                                    {"commission", 0.0}});
        }
        ledger.logEntries("BTC", rows);
        ledger.finishImport(s.constructionId, s.positions[0]);
        QVERIFY(ledger.value(s).complete);
        core.metadata("pnl_start/" + s.constructionId, Ledger::valuationSnapshot(s));
        ledger.sample(s, ledger.value(s));
        QElapsedTimer timer;
        const int repeats = 20;
        timer.start();
        for (int i = 0; i < repeats; ++i)
        {
            ledger.valueUncached(s);
            ledger.valueUncached(s);
            ledger.valueUncached(s);
            legacyScenario(s);
        }
        const double legacyUs = double(timer.nsecsElapsed()) / 1000 / repeats;
        timer.restart();
        for (int i = 0; i < repeats; ++i)
        {
            auto live = s;
            live.timestamp += i * 10;
            live.positions[0].mark += i * .00001;
            ledger.report(live);
        }
        const double cachedUs = double(timer.nsecsElapsed()) / 1000 / repeats;
        std::vector<std::unique_ptr<Window>> windows;
        for (int i = 0; i < 4; ++i)
        {
            auto window = std::make_unique<Window>("", Config{}, true);
            window->resize(960, 540);
            window->show();
            windows.push_back(std::move(window));
        }
        QTest::qWait(250);
        const double cpuBefore = cpuSeconds();
        timer.restart();
        QVector<qint64> renders, events;
        for (int frame = 0; frame < 80; ++frame)
        {
            for (auto& window : windows)
            {
                auto shared = window->sharedState();
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    auto& trading = shared->trading;
                    trading.timestamp = utcNow();
                    trading.quote.bid = 99999 + frame;
                    trading.quote.ask = 100001 + frame;
                    trading.quote.mark = 100000 + frame;
                    trading.quote.at = trading.timestamp;
                    for (auto& option : trading.positions)
                    {
                        option.tickerAt = trading.timestamp;
                        option.delta += .0001;
                        option.mark += .000001;
                    }
                    shared->analytics.timestamp = trading.timestamp;
                    shared->analytics.pnl.options = 120 + frame * .1;
                }
                auto event = std::make_shared<QElapsedTimer>();
                event->start();
                QTimer::singleShot(0, window.get(), [event, &events]
                                   { events.append(event->nsecsElapsed() / 1000); });
            }
            QTest::qWait(50);
            for (auto& window : windows)
                renders.append(window->lastRenderUs);
        }
        const double elapsed = timer.elapsed() / 1000.0;
        const double cpu = cpuSeconds() - cpuBefore;
        for (auto& window : windows)
        {
            auto shared = window->sharedState();
            std::lock_guard<std::mutex> lock(shared->mutex);
            QCOMPARE(shared->riskCalculations, quint64(0));
        }
        QJsonObject report{
            {"history_rows", rows.size()},
            {"legacy_report_mean_us", legacyUs},
            {"cached_report_mean_us", cachedUs},
            {"legacy_workload",
             "3 full valuations, 241 delta-only curve points and 2 escape searches"},
            {"new_workload", "ledger.report with changing marks and cached history; graph closed"},
            {"windows", 4},
            {"window_size", "960x540"},
            {"elapsed_s", elapsed},
            {"cpu_seconds", cpu},
            {"cpu_percent_one_core", cpu / elapsed * 100},
            {"render_p95_us", percentile(renders, .95)},
            {"event_p95_us", percentile(events, .95)},
            {"closed_graph_calculations", 0}};
        QFile out(QCoreApplication::applicationDirPath() + "/../performance-report.json");
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QJsonDocument(report).toJson());
        qInfo().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
        QVERIFY(percentile(renders, .95) < 100000);
        QVERIFY(percentile(events, .95) < 100000);
    }
};
QTEST_MAIN(PerformanceTests)
#include "performance_tests.moc"
