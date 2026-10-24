#include "analytics/model.h"
#include "fixtures.h"
#include <QtTest>
using namespace scalper;
class CoreTests : public QObject
{
    Q_OBJECT
  private slots:
    void normalizationAndSigns()
    {
        auto o = fixtures::option(1000000000);
        auto g = aggregate({o});
        QVERIFY(std::abs(g.delta - .05) < 1e-12);
        QCOMPARE(g.normalized(), .5);
        o.delta = .54;
        QVERIFY(std::abs(aggregate({o}).delta - .054) < 1e-12);
        auto p = fixtures::option(1000000000, false, true, -.1);
        g = aggregate({o, p});
        QVERIFY(std::abs(g.gross - .2) < 1e-12);
        QVERIFY(std::isfinite(g.normalized()));
    }
    void straddleUsesGross()
    {
        auto c = fixtures::option(1000000000), p = fixtures::option(1000000000, false);
        auto g = aggregate({c, p});
        QCOMPARE(g.gross, .2);
        QCOMPARE(g.delta, 0.0);
        QCOMPARE(g.normalized(), 0.0);
    }
    void initialAndEscape()
    {
        qint64 t = 1000000000;
        auto e = fixtures::engine(t);
        QVERIFY(e.start(t));
        auto a = e.evaluate(t);
        QVERIFY(a);
        QCOMPARE(a->target, -.05);
        e.inventory = a->target;
        e.complete(t);
        QVERIFY(!e.evaluate(t));
        t += 11000;
        fixtures::refresh(e, t);
        e.options[0].delta = .54;
        a = e.evaluate(t);
        QVERIFY(a);
        QVERIFY(std::abs(e.remaining() + .004) < 1e-10);
        e.inventory = a->target;
        e.complete(t);
        QVERIFY(!e.evaluate(t + 1));
    }
    void coverageAndMigration()
    {
        auto e = fixtures::engine(1000000000);
        for (double percent : {50., 100., 200., 350.})
        {
            e.config.hedgePercent = percent;
            QVERIFY(std::abs(e.target() + .05 * percent / 100) < Epsilon);
        }
        auto old = e.config.json();
        old.remove("hedge_percent");
        old["max_notional"] = 1;
        old["execution"] = int(Execution::PostOnly);
        const auto migrated = Config::fromJson(old);
        QCOMPARE(migrated.hedgePercent, 100.);
        QCOMPARE(migrated.execution, Execution::Balanced);
        old = migrated.json();
        old["execution"] = int(Execution::PostOnly);
        QCOMPARE(Config::fromJson(old).execution, Execution::PostOnly);
        QVERIFY(!old.contains("max_notional"));
        auto legacy = e.json();
        auto legacyConfig = e.config.json();
        legacyConfig.remove("hedge_percent");
        legacyConfig["max_notional"] = 1000;
        legacy["config"] = legacyConfig;
        legacy["anchored"] = true;
        legacy["inventory"] = -.01;
        Hedger restored;
        restored.restore(legacy);
        QVERIFY(!restored.anchored);
        QCOMPARE(restored.inventory, -.01);
        e.config.hedgePercent = 50;
        e.options[0].delta = .501;
        QCOMPARE(e.target(), -.025);
    }
    void passiveExecutionSettingsPersistAndMigrate()
    {
        auto c = fixtures::engine(utcNow()).config;
        QCOMPARE(c.makerWaitMs, 5000);
        QCOMPARE(c.passiveAttempts(), 3);
        c.execution = Execution::Aggressive;
        QCOMPARE(c.passiveAttempts(), 1);
        c.execution = Execution::PostOnly;
        QCOMPARE(c.passiveAttempts(), 0);
        c.balancedPassiveAttempts = 7;
        c.aggressivePassiveAttempts = 2;
        c.makerWaitMs = 6500;
        auto saved = c.json();
        const auto restored = Config::fromJson(saved);
        QCOMPARE(restored.balancedPassiveAttempts, 7);
        QCOMPARE(restored.aggressivePassiveAttempts, 2);
        QCOMPARE(restored.makerWaitMs, 6500);
        QVERIFY(restored.validate().isEmpty());
        saved.remove("balanced_passive_attempts");
        saved.remove("aggressive_passive_attempts");
        saved["maker_wait_ms"] = 2000;
        const auto migrated = Config::fromJson(saved);
        QCOMPARE(migrated.balancedPassiveAttempts, 3);
        QCOMPARE(migrated.aggressivePassiveAttempts, 1);
        QCOMPARE(migrated.makerWaitMs, 2000);
        saved.remove("maker_wait_ms");
        QCOMPARE(Config::fromJson(saved).makerWaitMs, 5000);
        for (int invalid : {0, -1, Limits::MaxPassiveAttempts + 1})
        {
            c.balancedPassiveAttempts = invalid;
            QVERIFY(!c.validate().isEmpty());
            c.balancedPassiveAttempts = 3;
            c.aggressivePassiveAttempts = invalid;
            QVERIFY(!c.validate().isEmpty());
            c.aggressivePassiveAttempts = 1;
        }
    }
    void minimumDeltaChangeIncludesCoverage()
    {
        auto e = fixtures::engine(1000000000);
        e.config.deltaChange = .001;
        e.config.hedgePercent = 50;
        QVERIFY(!e.start(1000000000));
        e.config.hedgePercent = 200;
        QVERIFY(e.start(1000000000));
        e.config.hedgePercent = 0;
        QVERIFY(!e.config.validate().isEmpty());
    }
    void completionKeepsUnhedgedDrift()
    {
        qint64 t = 1000000000;
        auto e = fixtures::engine(t);
        QVERIFY(e.start(t));
        auto operation = e.evaluate(t);
        QVERIFY(operation);
        e.inventory = operation->target;
        e.options[0].delta = .52;
        e.complete(t + 1000);
        QCOMPARE(e.referenceDelta, .5);
        fixtures::refresh(e, t + 12000);
        e.options[0].delta = .55;
        QVERIFY(e.evaluate(t + 12000));
    }
    void cooldownCoalescesSignals()
    {
        qint64 t = 1000000000;
        auto e = fixtures::engine(t);
        e.start(t);
        auto a = e.evaluate(t);
        e.inventory = a->target;
        e.complete(t);
        t += 1000;
        fixtures::refresh(e, t);
        e.options[0].delta = .7;
        QVERIFY(!e.evaluate(t));
        t += 10000;
        fixtures::refresh(e, t);
        e.options[0].delta = .55;
        a = e.evaluate(t);
        QVERIFY(a);
        QVERIFY(std::abs(a->target + .055) < 1e-10);
    }
    void stalenessAndExpiryStop()
    {
        qint64 t = 1000000000;
        auto e = fixtures::engine(t);
        QVERIFY(e.start(t));
        QVERIFY(!e.evaluate(t + 6000));
        QVERIFY(!e.running);
        fixtures::refresh(e, t);
        e.options[0].spec.expiry = t + 29 * 60000;
        QVERIFY(!e.start(t));
        QVERIFY(e.problem.contains("Expiry"));
    }
    void clockLeadToleranceDoesNotAdmitOldData()
    {
        const qint64 t = 1000000000;
        auto e = fixtures::engine(t);
        for (qint64 lead : {qint64(165), qint64(383), qint64(462), qint64(1000)})
        {
            e.quote.at = t + lead;
            e.options[0].tickerAt = t + lead;
            QVERIFY(e.quote.fresh(t));
            QVERIFY(e.options[0].freshGreeks(t));
            QVERIFY(e.start(t));
            e.stop();
        }
        e.quote.at = t + 1001;
        e.options[0].tickerAt = t + 1001;
        QVERIFY(!e.quote.fresh(t));
        QVERIFY(!e.options[0].freshGreeks(t));
        e.quote.at = t - 5001;
        e.options[0].tickerAt = t - 5001;
        QVERIFY(!e.quote.fresh(t));
        QVERIFY(!e.options[0].freshGreeks(t));
    }
    void marginDataIsMandatoryAndThresholdIsOptional()
    {
        auto e = fixtures::engine(1000000000);
        e.config.maxMarginUsage = 0;
        QVERIFY(!e.start(1000000000));
        e.config.marginLimitEnabled = false;
        QVERIFY(e.start(1000000000));
        e.config.marginLimitEnabled = true;
        e.config.maxMarginUsage = .5;
        e.margin.valid = false;
        QVERIFY(!e.start(1000000000));
    }
    void crossMarginRequiresTotals()
    {
        QJsonObject j{{"cross_collateral_enabled", true},
                      {"available_funds", 100},
                      {"margin_balance", 1000},
                      {"maintenance_margin", 10}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Margin::parse(j, 1000));
        j["total_margin_balance_usd"] = 2000;
        j["total_maintenance_margin_usd"] = 100;
        QCOMPARE(Margin::parse(j, 1000).usage(), .05);
    }
    void minimumAndRoundoff()
    {
        QCOMPARE(roundDown(.3, .1), .3);
        QVERIFY(std::abs(roundDown(.004, .0001) - .004) < 1e-12);
        QCOMPARE(tickPrice(100.1, .5, true), 100.5);
        QCOMPARE(tickPrice(100.1, .5, false), 100.0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, roundDown(1, 0));
    }
    void restoreDoesNotTrade()
    {
        auto e = fixtures::engine(1000000000);
        e.start(1000000000);
        e.evaluate(1000000000);
        auto j = e.json();
        Hedger copy;
        copy.restore(j);
        QVERIFY(!copy.running);
        QVERIFY(copy.active);
        QCOMPARE(copy.operation.target, e.operation.target);
    }
    void modelParityAndGreeks()
    {
        qint64 t = 1000000000;
        auto c = fixtures::option(t), p = fixtures::option(t, false);
        auto a = optionModel(c, 100000, t), b = optionModel(p, 100000, t);
        QVERIFY(std::abs(a.usd - b.usd) < 1e-8);
        QVERIFY(std::abs(a.delta - b.delta - 1) < 1e-10);
        double h = .01;
        auto up = optionModel(c, 100000 + h, t), down = optionModel(c, 100000 - h, t);
        QVERIFY(std::abs((up.usd - down.usd) / (2 * h) - a.delta) < 1e-6);
    }
    void curveAnchorsActualPnl()
    {
        qint64 t = 1000000000;
        auto c = fixtures::option(t);
        auto points = portfolioCurve({c}, -.05, 100000, 123.45, t);
        auto mid = points[points.size() / 2];
        QVERIFY(std::abs(mid.pnl - 123.45) < 1e-8);
        QVERIFY(std::abs(mid.delta) < 1e-8);
        QVERIFY(std::abs(mid.gamma - c.quantity * c.gamma) < 1e-12);
        QVERIFY(std::abs(mid.theta - c.quantity * c.theta) < 1e-8);
        QVERIFY(std::abs(mid.vega - c.quantity * c.vega) < 1e-8);
    }
    void marginThresholdMigrationAndModels()
    {
        Config fresh;
        QVERIFY(!fresh.marginLimitEnabled);
        auto old = fixtures::engine(1000).config.json();
        old.remove("margin_limit_enabled");
        QVERIFY(Config::fromJson(old).marginLimitEnabled);
        old["margin_limit_enabled"] = false;
        QVERIFY(!Config::fromJson(old).marginLimitEnabled);
        for (const auto& model : {"segregated_sm", "segregated_pm", "cross_sm", "cross_pm"})
        {
            QJsonObject j{{"margin_model", model},
                          {"available_funds", 100},
                          {"margin_balance", 1000},
                          {"maintenance_margin", 100},
                          {"total_margin_balance_usd", 2000},
                          {"total_maintenance_margin_usd", 50}};
            const auto parsed = Margin::parse(j, 1000);
            QCOMPARE(parsed.model, QString(model));
            QCOMPARE(parsed.usage(), QString(model).startsWith("cross") ? .025 : .1);
            QVERIFY(!parsed.scope.isEmpty());
        }
    }
    void scenarioGreeksMatchFiniteDifferences()
    {
        const qint64 t = 1000000000;
        for (bool inverse : {false, true})
        {
            auto o = fixtures::option(t, true, inverse);
            const auto greeks = modelGreeks(o, 100000, t);
            const auto up = optionModel(o, 100001, t), down = optionModel(o, 99999, t);
            QVERIFY(std::abs(greeks.gamma - (up.delta - down.delta) / 2) < 1e-10);
            QVERIFY(greeks.vega > 0);
            QVERIFY(greeks.theta < 0);
            o.spec.expiry = t + Time::HourMs;
            QVERIFY(std::isfinite(modelGreeks(o, 100000, t).theta));
        }
    }
    void escapeIsAnIndicator()
    {
        qint64 t = 1000000000;
        auto c = fixtures::option(t);
        auto a = escapePrice({c}, .5, .04, 100000, t, true),
             b = escapePrice({c}, .5, .04, 100000, t, false);
        QVERIFY(a && b);
        QVERIFY(*a > 100000);
        QVERIFY(*b < 100000);
        c.quantity = 0;
        QVERIFY(!escapePrice({c}, 0, .04, 100000, t, true));
    }
    void localMoveEstimates()
    {
        const qint64 now = 1000000000;
        const auto call = fixtures::option(now, true, false, .2);
        auto put = fixtures::option(now, false, false, -.1);
        const auto g = aggregate({call, put});
        const auto moves = estimateMoves(g, 100000, .01);
        QVERIFY(moves.rebalancePercent && moves.breakEven);
        QVERIFY(std::abs(*moves.rebalancePercent - 1.2) < 1e-10);
        // Gamma profit at this move exactly offsets the supplied signed theta.
        QVERIFY(std::abs(.5 * g.gamma * std::pow(*moves.breakEven, 2) + g.theta) < 1e-10);
        const auto halfBand = estimateMoves(g, 100000, .005);
        QVERIFY(std::abs(*halfBand.rebalancePercent - .6) < 1e-10);
        QCOMPARE(*halfBand.breakEven, *moves.breakEven);
        put.quantity = -.2;
        const auto flat = estimateMoves(aggregate({call, put}), 100000, .01);
        QVERIFY(!flat.rebalancePercent && !flat.breakEven);
        put.quantity = -.3;
        const auto shortGamma = estimateMoves(aggregate({call, put}), 100000, .01);
        QVERIFY(shortGamma.rebalancePercent);
        QVERIFY(!shortGamma.breakEven);
        auto invalid = g;
        invalid.theta = 10;
        QVERIFY(!estimateMoves(invalid, 100000, .01).breakEven);
        invalid.gamma = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!estimateMoves(invalid, 100000, .01).rebalancePercent);
        QVERIFY(!estimateMoves(invalid, 100000, .01).breakEven);
        QVERIFY(!estimateMoves(g, 0, .01).rebalancePercent);
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
