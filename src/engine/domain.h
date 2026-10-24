#pragma once
#include "config/settings.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QStringList>
#include <QVector>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

namespace scalper
{
constexpr double Epsilon = 1e-10;
enum class Execution
{
    PostOnly,
    Balanced,
    Aggressive
};
QString executionName(Execution mode);
qint64 utcNow();
QString isoUtc(qint64 milliseconds);
double number(const QJsonObject& json, const char* key);
double roundDown(double value, double step);
double tickPrice(double value, double tick, bool up);
struct Config
{
    QString instrument = Exchange::BtcPerpetual;
    bool production = false;
    bool marginLimitEnabled = false;
    QStringList selected;
    double deltaChange = .01, hedgePercent = 100, maxMarginUsage = 0, slippageBps = 5;
    int cooldownMs = 10000, makerWaitMs = 5000;
    int balancedPassiveAttempts = 3, aggressivePassiveAttempts = 1;
    Execution execution = Execution::Balanced;
    QString base() const
    {
        return instrument.startsWith("ETH") ? "ETH" : "BTC";
    }
    QString validate() const;
    int passiveAttempts() const
    {
        return execution == Execution::PostOnly ? 0
               : execution == Execution::Balanced ? balancedPassiveAttempts
                                                  : aggressivePassiveAttempts;
    }
    QJsonObject json() const;
    static Config fromJson(const QJsonObject& json);
};
struct Instrument
{
    QString name, base, settlement, kind;
    double tick = 0, step = 0, contractSize = 1, strike = 0;
    qint64 expiry = 0, creation = 0;
    bool inverse = false;
    QVector<QPair<double, double>> tickSteps;
    double tickAt(double price) const;
    QJsonObject json() const;
    static Instrument parse(const QJsonObject& json);
};
struct Option
{
    Instrument spec;
    double quantity = 0, averagePrice = 0, averageUsd = 0;
    double delta = 0, gamma = 0, theta = 0, vega = 0;
    double mark = 0, index = 0, underlying = 0, iv = 0, rate = 0;
    qint64 tickerAt = 0;
    bool isCall = true, hasGreeks = false;
    bool isOpen(qint64 now) const
    {
        return spec.kind == "option" && std::abs(quantity) > Epsilon && spec.expiry > now;
    }
    bool freshGreeks(qint64 now) const
    {
        return hasGreeks && tickerAt > 0 && now - tickerAt >= -Time::MarketClockLeadToleranceMs &&
               now - tickerAt <= Time::MarketDataMaxAgeMs;
    }
    double markUsd() const
    {
        return spec.inverse ? mark * index : mark;
    }
    QJsonObject json() const;
    static Option fromJson(const QJsonObject& json);
};
struct Quote
{
    double bid = 0, ask = 0, mark = 0, index = 0;
    qint64 at = 0;
    double mid() const
    {
        return (bid + ask) / 2;
    }
    bool fresh(qint64 now) const
    {
        return bid > 0 && ask >= bid && at > 0 && now - at <= Time::MarketDataMaxAgeMs &&
               now - at >= -Time::MarketClockLeadToleranceMs;
    }
};
struct Greeks
{
    double delta = 0, gamma = 0, theta = 0, vega = 0, gross = 0;
    double normalized() const
    {
        return gross > Epsilon ? delta / gross : 0;
    }
};
Greeks aggregate(const QVector<Option>& options);
struct Margin
{
    double balance = 0, maintenance = 0, available = 0;
    qint64 at = 0;
    bool valid = false;
    QString model, scope;
    double usage() const
    {
        return balance > 0 ? maintenance / balance : std::numeric_limits<double>::infinity();
    }
    static Margin parse(const QJsonObject& json, qint64 now);
};
struct Order
{
    QString label, id, instrument, state = "open";
    bool buy = false, postOnly = true, ioc = false, reduceOnly = false, terminal = false,
         uncertain = false;
    double price = 0, total = 0, filled = 0;
    qint64 updated = 0, attemptAt = 0, createdAt = 0;
    QString mutation;
    QJsonObject request;
    QJsonObject json() const;
    static Order fromJson(const QJsonObject& json);
};
struct TradingSnapshot
{
    quint64 revision = 0;
    Config config;
    QString status = "Disconnected", account, profileId, constructionId, problem;
    QString connectionStatus = "Disconnected";
    bool connectionSlow = false;
    bool connected = false, synced = false, running = false, busy = false, canDelete = false;
    bool anchored = false, executing = false;
    bool positionMismatch = false;
    double externalPosition = 0;
    qint64 timestamp = 0, cooldownUntil = 0, latencyMs = -1;
    qint64 rpcQueueMs = -1, lastHedgeMs = -1;
    std::optional<qint64> exchangeClockOffsetMs;
    double inventory = 0, target = 0, referenceDelta = 0, liquidation = 0;
    double makerFee = Exchange::DefaultMakerFee, takerFee = Exchange::DefaultTakerFee;
    double sessionFunding = 0, fundingBaseline = 0;
    qint64 fundingAt = 0, fundingBaselineSession = 0;
    bool fundingValid = false;
    Quote quote;
    Margin margin;
    QVector<Option> positions;
    QVector<Order> orders;
    QStringList messages;
    QVector<Option> openOptions(qint64 now) const;
};
struct Pnl
{
    double options = 0, realized = 0, unrealized = 0, optionFees = 0, hedgeFees = 0, funding = 0;
    double net() const
    {
        return options + realized + unrealized - optionFees - hedgeFees + funding;
    }
    Pnl operator-(const Pnl& other) const;
    QJsonObject json() const;
    static Pnl fromJson(const QJsonObject& json);
};
struct ProfilePoint
{
    double price = 0, pnl = 0, delta = 0;
    double gamma = 0, theta = 0, vega = 0;
};
enum class ScenarioGreek
{
    Delta,
    Gamma,
    Theta,
    Vega
};
struct RiskRequest
{
    quint64 id = 0;
    TradingSnapshot trading;
    bool demo = false;
    Pnl demoPnl;
};
struct RiskResult
{
    quint64 id = 0;
    qint64 timestamp = 0;
    double spot = 0;
    QVector<ProfilePoint> curve;
    std::optional<double> lower, upper;
    QVector<QPair<QString, double>> impliedVols;
    double referenceDelta = 0, deltaChange = 0;
    QString problem;
    bool complete = false;
};
struct AnalyticsSnapshot
{
    QString constructionId;
    quint64 revision = 0;
    qint64 timestamp = 0, startedAt = 0;
    QString completeness = "Waiting for fresh prices and selected option data", historyStatus;
    Pnl pnl;
    bool complete = false, ready = false;
    QString message;
    qint64 calculationUs = 0;
};
struct SharedState
{
    std::mutex mutex;
    TradingSnapshot trading;
    AnalyticsSnapshot analytics;
    RiskResult risk;
    quint64 riskCalculations = 0;
    quint64 activeRiskRequest = 0;
};
} // namespace scalper
