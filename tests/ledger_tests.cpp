#include "analytics/ledger.h"
#include "fixtures.h"
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>
using namespace scalper;
static QJsonObject optionTrade(const Option& o, qint64 id, qint64 at, double amount,
                               double position, double price, double index = 100000)
{
    return {{"id", id},
            {"timestamp", at},
            {"instrument_name", o.spec.name},
            {"type", "trade"},
            {"side", amount > 0 ? "open buy" : "close sell"},
            {"amount", std::abs(amount)},
            {"position", position},
            {"price", price},
            {"index_price", index},
            {"cashflow", -amount * price},
            {"commission", 0.0},
            {"trade_id", "shared-trade"}};
}
static TradingSnapshot snap(qint64 t)
{
    TradingSnapshot s;
    s.timestamp = t;
    s.config = fixtures::engine(t).config;
    s.constructionId = "construction";
    s.positions = {fixtures::option(t)};
    s.quote = {99999, 100001, 100000, 100000, t};
    return s;
}
class LedgerTests : public QObject
{
    Q_OBJECT
  private slots:
    void continuousAverageCost()
    {
        HedgeBook b;
        b.trade(-.1, 100000, .5);
        b.trade(.04, 99000, .2);
        QVERIFY(std::abs(b.quantity + .06) < 1e-12);
        QVERIFY(std::abs(b.realized - 40) < 1e-8);
        b.trade(.1, 101000, .3);
        QVERIFY(std::abs(b.realized + 20) < 1e-8);
        QVERIFY(std::abs(b.quantity - .04) < 1e-12);
        QCOMPARE(b.average, 101000.0);
    }
    void incompleteOptionEpisodeIsRecheckedAfterBackfill()
    {
        QTemporaryDir dir;
        const auto corePath = dir.filePath("core.db");
        Store core(corePath);
        Ledger ledger(corePath, dir.filePath("analytics.db"));
        auto s = snap(1000000000);
        const auto& option = s.positions.first();
        ledger.ensureLegs(s);
        ledger.logEntry("BTC", optionTrade(option, 1, s.timestamp - 1000, .05, .05, .05));
        ledger.finishImport(s.constructionId, option);
        QVERIFY(!ledger.value(s).complete);
        ledger.logEntry("BTC", optionTrade(option, 2, s.timestamp - 500, .05, .1, .05));
        ledger.finishImport(s.constructionId, option);
        QVERIFY(ledger.value(s).complete);
    }
    void durableIntentAndOwnership()
    {
        try
        {
            QTemporaryDir d;
            QVERIFY(d.isValid());
            Store store(d.filePath("core.db"));
            auto e = fixtures::engine(1000000000);
            Order o;
            o.label = "gs_" + store.profileId() + "_1";
            o.instrument = e.config.instrument;
            o.total = .01;
            o.price = 100000;
            o.mutation = "private/sell";
            o.request = {{"label", o.label}};
            store.save(e, {o});
            Hedger copy;
            QVector<Order> orders;
            QVERIFY(store.load(copy, orders));
            QCOMPARE(orders.size(), 1);
            QCOMPARE(orders[0].mutation, QString("private/sell"));
            store.bind("account/testnet/BTC_USDC-PERPETUAL");
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.bind("another-account"));
        }
        catch (const std::exception& e)
        {
            QFAIL(e.what());
        }
    }
    void profileResetIsAtomic()
    {
        QTemporaryDir dir;
        Store store(dir.filePath("core.db"));
        auto engine = fixtures::engine(utcNow());
        engine.inventory = -.01;
        store.bind("bound-account");
        store.metadata("construction", {{"id", "old"}});
        store.fill({{"instrument_name", engine.config.instrument},
                    {"trade_id", "old"},
                    {"order_id", "order"},
                    {"amount", .01},
                    {"timestamp", utcNow()}});
        store.save(engine, {});
        const auto oldProfile = store.profileId();
        Hedger fresh;
        sqlExec(store.database(),
                "CREATE TRIGGER reject_reset BEFORE INSERT ON meta WHEN NEW.key='engine' BEGIN "
                "SELECT RAISE(ABORT,'simulated storage failure'); END");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 store.resetProfile(fresh, "new", "bound-account", 0));
        QCOMPARE(store.profileId(), oldProfile);
        QCOMPARE(store.metadata("construction")["id"].toString(), QString("old"));
        QCOMPARE(store.metadata("engine")["inventory"].toDouble(), -.01);
        QCOMPARE(store.coveredQuantity("order"), .01);
        sqlExec(store.database(), "DROP TRIGGER reject_reset");
        store.resetProfile(fresh, "new", "bound-account", .02);
        QVERIFY(store.profileId() != oldProfile);
        QCOMPARE(store.coveredQuantity("order"), 0.0);
        QCOMPARE(store.metadata("engine")["inventory"].toDouble(), 0.0);
        QCOMPARE(store.metadata("external_position")["quantity"].toDouble(), .02);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.bind("another-account"));
    }
    void analyticsDetectsProfileResetAfterInterruptedNotification()
    {
        QTemporaryDir dir;
        const auto corePath = dir.filePath("core.db");
        const auto analyticsPath = dir.filePath("analytics.db");
        Store store(corePath);
        auto engine = fixtures::engine(utcNow());
        store.save(engine, {});
        store.fill({{"instrument_name", engine.config.instrument},
                    {"trade_id", "old"},
                    {"order_id", "old"},
                    {"amount", .01},
                    {"timestamp", utcNow()}});
        {
            Ledger old(corePath, analyticsPath);
            old.syncCore();
            QVERIFY(old.metadata("core_cursor")["fills"].toInteger() > 0);
            // Simulate exiting after the core reset, without notifying analytics.
            store.resetProfile(Hedger{}, "new", "bound-account", 0);
        }
        Ledger reopened(corePath, analyticsPath);
        reopened.syncCore();
        QSqlQuery query(reopened.database());
        QVERIFY(query.exec("SELECT COUNT(*) FROM trades"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        QCOMPARE(reopened.metadata("core_profile"), store.metadata("profile"));
    }
    void importCurrentEpisodeNotOldTrades()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        l.ensureLegs(s);
        auto o = s.positions[0];
        l.logEntry("BTC", optionTrade(o, 1, s.timestamp - 10000, .1, .1, .01));
        l.logEntry("BTC", optionTrade(o, 2, s.timestamp - 9000, -.1, 0, .02));
        l.logEntry("BTC", optionTrade(o, 3, s.timestamp - 8000, .1, .1, .05));
        l.finishImport(s.constructionId, o);
        auto v = l.value(s);
        QVERIFY(v.complete);
        QVERIFY(std::abs(v.pnl.options - 100) < 1e-8);
        QCOMPARE(v.first, s.timestamp - 8000);
    }
    void logRowsWithSameTradeIdArePreserved()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        l.ensureLegs(s);
        auto o = s.positions[0];
        auto j = optionTrade(o, 11, s.timestamp - 1000, .1, .1, .05);
        l.logEntry("BTC", j);
        l.logEntry("BTC", j);
        j["id"] = 12;
        j["amount"] = .01;
        j["position"] = .11;
        l.logEntry("BTC", j);
        QSqlQuery q(l.database());
        QVERIFY(q.exec("SELECT COUNT(*) FROM raw_logs"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
    }
    void missingHistoryIsMarked()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        l.ensureLegs(s);
        auto v = l.value(s);
        QVERIFY(!v.complete);
        QVERIFY(!v.missing.isEmpty());
    }
    void settlementAndFundingAreNotCountedTwice()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        s.config.selected.clear();
        s.positions.clear();
        QJsonObject fill{{"trade_id", "one"},
                         {"instrument_name", s.config.instrument},
                         {"order_id", "own"},
                         {"construction_id", s.constructionId},
                         {"timestamp", s.timestamp - 100000},
                         {"direction", "sell"},
                         {"amount", .01},
                         {"price", 100000},
                         {"fee", .5}};
        core.fill(fill);
        core.fill(fill);
        l.syncCore();
        s.inventory = -.01;
        QJsonObject settle{{"id", 1},
                           {"timestamp", s.timestamp - 50000},
                           {"instrument_name", s.config.instrument},
                           {"type", "settlement"},
                           {"cashflow", 5000},
                           {"session_rpl", 4000},
                           {"total_interest_pl", 2},
                           {"interest_pl", 2}};
        l.logEntry("USDC", settle);
        l.logEntry("USDC", settle);
        settle["id"] = 2;
        settle["timestamp"] = s.timestamp - 1000;
        settle["interest_pl"] = -.5;
        l.logEntry("USDC", settle);
        auto v = l.value(s);
        QCOMPARE(v.pnl.realized, 0.0);
        QCOMPARE(v.pnl.funding, 1.5);
        QCOMPARE(v.pnl.hedgeFees, .5);
        QCOMPARE(v.pnl.net(), 1.0);
    }
    void constructionPnlRebasesWhenHistoryArrives()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        l.ensureLegs(s);
        core.metadata("pnl_start/" + s.constructionId, Ledger::valuationSnapshot(s));
        auto later = s;
        later.timestamp += 1000;
        later.positions[0].mark = .061;
        auto before = l.report(later).pnl.net();
        l.logEntry("BTC", optionTrade(s.positions[0], 1, s.timestamp - 1000, .1, .1, .04));
        l.finishImport(s.constructionId, s.positions[0]);
        auto after = l.report(later).pnl.net();
        QVERIFY(std::abs(before - after) < 1e-8);
        QVERIFY(std::abs(after - 10) < 1e-8);
    }
    void constructionBaselineSurvivesReopen()
    {
        QTemporaryDir d;
        const auto corePath = d.filePath("core.db"), analyticsPath = d.filePath("analytics.db");
        Store core(corePath);
        auto s = snap(1000000000);
        core.metadata("pnl_start/" + s.constructionId, Ledger::valuationSnapshot(s));
        {
            Ledger l(corePath, analyticsPath);
            l.ensureLegs(s);
            QVERIFY(l.report(s).ready);
            QCOMPARE(l.report(s).pnl.net(), 0.0);
        }
        Ledger l(corePath, analyticsPath);
        s.timestamp += 1000;
        s.positions[0].mark += .001;
        const auto a = l.report(s);
        QCOMPARE(a.startedAt, qint64(1000000000));
        QVERIFY(std::abs(a.pnl.net() - 10) < 1e-8);
    }
    void twoStepDeliveryHasOneEconomicPayout()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        auto& o = s.positions[0];
        o.spec.expiry = s.timestamp + 1000;
        l.ensureLegs(s);
        l.logEntry("BTC", optionTrade(o, 1, s.timestamp - 1000, .1, .1, .05));
        l.finishImport(s.constructionId, o);
        auto ended = s;
        ended.timestamp += 2000;
        ended.positions[0].quantity = 0;
        QVERIFY(ended.openOptions(ended.timestamp).isEmpty());
        QCOMPARE(ended.positions.size(), 1);
        QJsonObject delivery{{"id", 2},
                             {"timestamp", o.spec.expiry},
                             {"instrument_name", o.spec.name.section('-', 0, 1)},
                             {"type", "delivery"},
                             {"price", 110000},
                             {"cashflow", 99999}};
        l.logEntry("BTC", delivery);
        auto v = l.value(ended);
        QVERIFY(v.complete);
        QVERIFY(std::abs(v.pnl.options - 500) < 1e-8);
    }
    void liveFundingRollsIntoSettlementOnce()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        qint64 boundary =
            QDateTime(QDate(2026, 10, 25), QTime(8, 0), QTimeZone::utc()).toMSecsSinceEpoch();
        auto s = snap(boundary - 1800000);
        s.positions.clear();
        s.config.selected.clear();
        s.inventory = -.01;
        QJsonObject fill{{"trade_id", "1"},
                         {"instrument_name", s.config.instrument},
                         {"order_id", "own"},
                         {"construction_id", s.constructionId},
                         {"timestamp", boundary - 3300000},
                         {"direction", "sell"},
                         {"amount", .01},
                         {"price", 100000},
                         {"fee", 0}};
        core.fill(fill);
        l.syncCore();
        s.fundingValid = true;
        s.fundingAt = s.timestamp;
        s.sessionFunding = 7;
        s.fundingBaseline = 5;
        s.fundingBaselineSession = boundary - 86400000;
        l.logEntry("USDC", {{"id", 1},
                            {"timestamp", boundary - 2400000},
                            {"instrument_name", s.config.instrument},
                            {"type", "trade"},
                            {"order_id", "own"},
                            {"interest_pl", 1}});
        QCOMPARE(l.value(s).pnl.funding, 2.0);
        core.metadata("pnl_start/" + s.constructionId, Ledger::valuationSnapshot(s));
        s.timestamp = boundary - 1000;
        s.fundingAt = s.timestamp;
        l.sample(s, l.value(s));
        auto pending = s;
        pending.timestamp = boundary + 600000;
        pending.fundingAt = pending.timestamp;
        pending.sessionFunding = .3;
        auto carried = l.value(pending);
        QVERIFY(std::abs(carried.pnl.funding - 2.3) < 1e-12);
        QVERIFY(!carried.complete);
        l.logEntry("USDC", {{"id", 2},
                            {"timestamp", boundary + 100},
                            {"instrument_name", s.config.instrument},
                            {"type", "settlement"},
                            {"cashflow", 999},
                            {"interest_pl", 1},
                            {"total_interest_pl", 2}});
        s.timestamp = boundary + 600000;
        s.fundingAt = s.timestamp;
        s.sessionFunding = .3;
        QVERIFY(std::abs(l.value(s).pnl.funding - 2.3) < 1e-12);
        QVERIFY(std::abs(l.report(s).pnl.funding - .3) < 1e-12);
    }
    void cachedValuationsMatchFullReplayAcrossHistoryChanges()
    {
        QTemporaryDir d;
        Store core(d.filePath("core.db"));
        auto ledger = std::make_unique<Ledger>(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        auto compare = [&](const TradingSnapshot& snapshot)
        {
            const auto cached = ledger->value(snapshot), full = ledger->valueUncached(snapshot);
            const auto a = cached.pnl.json(), b = full.pnl.json();
            for (auto it = a.begin(); it != a.end(); ++it)
                QVERIFY2(std::abs(it.value().toDouble() - b[it.key()].toDouble()) < 1e-7,
                         qPrintable(it.key()));
            QCOMPARE(cached.complete, full.complete);
            QCOMPARE(cached.missing, full.missing);
        };
        ledger->ensureLegs(s);
        compare(s); // Missing history must not get stuck in a cache.
        auto opening = optionTrade(s.positions[0], 1, s.timestamp - 10000, .1, .1, .05);
        ledger->logEntry("BTC", opening);
        ledger->finishImport(s.constructionId, s.positions[0]);
        QVERIFY(ledger->value(s).complete);
        compare(s);
        auto missing = s;
        missing.config.selected << "unavailable-option";
        compare(missing);
        core.metadata("pnl_start/" + s.constructionId, Ledger::valuationSnapshot(s));
        const auto baseline = s;
        s.timestamp += 1000;
        s.positions[0].mark = .065;
        s.positions[0].index = 101000;
        compare(s); // Reprice inverse marks without replaying historical FX.
        QCOMPARE(ledger->report(s).pnl.net(),
                 ledger->valueUncached(s).pnl.net() - ledger->valueUncached(baseline).pnl.net());
        opening["commission"] = .00001;
        ledger->logEntry("BTC", opening); // Corrected row, same stable log ID.
        compare(s);
        ledger->logEntry("BTC", optionTrade(s.positions[0], 2, s.timestamp - 500, -.04, .06, .07));
        s.positions[0].quantity = .06;
        compare(s);
        QJsonObject fill{{"trade_id", "hedge-1"},
                         {"instrument_name", s.config.instrument},
                         {"order_id", "own"},
                         {"construction_id", s.constructionId},
                         {"timestamp", s.timestamp - 100},
                         {"direction", "sell"},
                         {"amount", .01},
                         {"price", 100000},
                         {"fee", .1}};
        core.fill(fill);
        ledger->syncCore();
        s.inventory = -.01;
        s.fundingValid = true;
        s.fundingAt = s.timestamp;
        s.sessionFunding = 2;
        compare(s);
        s.quote.mark = 102000;
        s.sessionFunding = 2.5;
        s.timestamp += 100;
        compare(s); // Mark and live funding changes on the same historical book.
        fill["trade_id"] = "hedge-2";
        fill["direction"] = "buy";
        fill["amount"] = .004;
        fill["price"] = 101000;
        fill["timestamp"] = s.timestamp;
        core.fill(fill);
        ledger->syncCore();
        s.inventory = -.006;
        compare(s);
        ledger->sample(s, ledger->value(s));
        ledger.reset();
        ledger = std::make_unique<Ledger>(d.filePath("core.db"), d.filePath("analytics.db"));
        compare(s); // Persisted history and cache reconstruction.
        s.timestamp = Time::fundingSessionStart(s.timestamp) + Time::DayMs + 1000;
        s.fundingAt = s.timestamp;
        s.sessionFunding = .3;
        compare(s); // Settlement boundary without complete funding history.
        const auto boundary = Time::fundingSessionStart(s.timestamp);
        ledger->metadata("perp_log_cursor", {{"at", boundary}});
        ledger->logEntry("USDC", {{"id", 900},
                                  {"instrument_name", s.config.instrument},
                                  {"timestamp", boundary + 100},
                                  {"type", "settlement"},
                                  {"interest_pl", 2.5}});
        compare(s);
        s.timestamp = s.positions[0].spec.expiry + 1000;
        s.positions[0].quantity = 0;
        ledger->logEntry("BTC", {{"id", 3},
                                 {"instrument_name", s.positions[0].spec.name},
                                 {"timestamp", s.positions[0].spec.expiry},
                                 {"type", "expiry"},
                                 {"cashflow", 0.0}});
        compare(s);
    }
    void dailyCsvUsesRecordedConstructionPnlAndMarksGaps()
    {
        QTemporaryDir d;
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        const qint64 t =
            QDateTime(QDate(2026, 10, 23), QTime(9, 0), QTimeZone::utc()).toMSecsSinceEpoch();
        auto s = snap(t);
        l.ensureLegs(s);
        l.logEntry("BTC", optionTrade(s.positions[0], 1, t - 1000, .1, .1, .05));
        l.finishImport(s.constructionId, s.positions[0]);
        core.metadata("pnl_start/" + s.constructionId, Ledger::valuationSnapshot(s));
        QCOMPARE(l.report(s).pnl.net(), 0.0);
        s.timestamp += 1000;
        s.positions[0].mark += .001;
        auto a = l.report(s);
        l.sample(s, l.value(s), &a);
        s.timestamp += 60000; // Missing observations must remain visibly partial.
        s.positions[0].mark += .001;
        a = l.report(s);
        l.sample(s, l.value(s), &a);
        s.timestamp =
            QDateTime(QDate(2026, 10, 24), QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch();
        a = l.report(s);
        l.sample(s, l.value(s), &a);
        // History arriving after samples were saved must also correct the daily export.
        auto sell = optionTrade(s.positions[0], 2, t + 1000, -.001, .099, .06);
        sell["commission"] = .00001;
        auto buy = optionTrade(s.positions[0], 3, t + 1000, .001, .1, .06);
        buy["commission"] = .00001;
        l.logEntry("BTC", sell);
        l.logEntry("BTC", buy);
        const auto folder = l.exportCsv(d.filePath("exports"));
        QFile daily(folder + "/daily.csv");
        QVERIFY(daily.open(QIODevice::ReadOnly));
        const auto text = daily.readAll();
        QVERIFY(text.startsWith("\xEF\xBB\xBF"));
        QVERIFY(text.contains("2026-10-23"));
        QVERIFY(text.contains("2026-10-24"));
        const auto firstDay = QString::fromUtf8(text).split('\n').at(1).split(',');
        QVERIFY(std::abs(QString(firstDay.at(9)).remove('"').toDouble() - 18) < 1e-7);
        QVERIFY(text.contains("Partial recorded period"));
        QVERIFY(!text.contains("Complete recorded period"));
        QVERIFY(QFile::exists(folder + "/trades.csv"));
        QVERIFY(QFile::exists(folder + "/sessions.csv")); // Legacy raw history remains exportable.
    }
    void resetAndExport()
    {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        Store core(d.filePath("core.db"));
        Ledger l(d.filePath("core.db"), d.filePath("analytics.db"));
        auto s = snap(1000000000);
        l.ensureLegs(s);
        core.metadata("pnl_start/" + s.constructionId, Ledger::valuationSnapshot(s));
        l.sample(s, l.value(s));
        auto folder = l.exportCsv(d.filePath("exports"));
        QFile f(folder + "/summaries.csv");
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto data = f.readAll();
        QVERIFY(data.startsWith("\xEF\xBB\xBF"));
        QVERIFY(data.contains("Timestamp UTC"));
        QVERIFY(data.contains("net"));
        l.clear();
        QSqlQuery q(l.database());
        QVERIFY(q.exec("SELECT COUNT(*) FROM samples"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }
};
QTEST_GUILESS_MAIN(LedgerTests)
#include "ledger_tests.moc"
