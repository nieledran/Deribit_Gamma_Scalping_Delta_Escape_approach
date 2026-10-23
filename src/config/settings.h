#pragma once
#include <QStringList>
#include <QtTypes>

namespace scalper
{
// Shared policy values, protocol limits and units. User choices belong to Config.
namespace Time
{
inline constexpr int SecondMs = 1000;
inline constexpr int MinuteMs = 60 * SecondMs;
inline constexpr int HourMs = 60 * MinuteMs;
inline constexpr qint64 DayMs = 24LL * HourMs;
inline constexpr qint64 FundingSettlementOffsetMs = 8LL * HourMs;
inline constexpr qint64 ExpiryPauseMs = 30LL * MinuteMs;
inline constexpr int MarketDataMaxAgeMs = 5 * SecondMs;
// A small forward timestamp is clock skew, not stale data. Old data still has a 5 s limit.
inline constexpr int MarketClockLeadToleranceMs = SecondMs;
inline constexpr int MarginDataMaxAgeMs = 10 * SecondMs;
inline constexpr int FundingDataMaxAgeMs = 10 * SecondMs;
inline constexpr int TickerTimestampToleranceMs = 10 * SecondMs;
inline constexpr int TradingPumpIntervalMs = 25;
inline constexpr int UiRefreshIntervalMs = 200;
inline constexpr int AccountPollIntervalMs = 3 * SecondMs;
inline constexpr int MarketRefreshIntervalMs = SecondMs;
inline constexpr int MarketRefreshAgeMs = 2 * SecondMs;
inline constexpr int AnalyticsIntervalMs = 500;
inline constexpr int FillRecoveryDelayMs = 500;
inline constexpr int FundingCarryWindowMs = 10 * SecondMs;

inline constexpr qint64 fundingSessionStart(qint64 timestamp)
{
    return ((timestamp - FundingSettlementOffsetMs) / DayMs) * DayMs + FundingSettlementOffsetMs;
}
} // namespace Time

namespace Limits
{
inline constexpr double MaxDeltaChange = 2;
inline constexpr double MaxSlippageBps = 1000;
inline constexpr double MaxMarginUsagePercent = 99.99;
inline constexpr int MinOperationIntervalMs = 500;
inline constexpr int MaxCooldownMs = Time::HourMs;
inline constexpr int MaxMakerWaitMs = Time::MinuteMs;
inline constexpr int RecentOrders = 100;
inline constexpr int JournalMessages = 200;
inline constexpr double BasisPoints = 10000;
} // namespace Limits

namespace History
{
inline constexpr int RequestIntervalMs = 500;
inline constexpr int RefreshIntervalMs = 15 * Time::SecondMs;
inline constexpr int OverlapMs = Time::SecondMs;
inline constexpr int PageSize = 1000;
inline constexpr int DeliveryWindowMs = Time::MinuteMs;
inline constexpr int RecoveryWindowPaddingMs = 10 * Time::SecondMs;
} // namespace History

namespace Exchange
{
inline constexpr char BtcPerpetual[] = "BTC_USDC-PERPETUAL";
inline constexpr char EthPerpetual[] = "ETH_USDC-PERPETUAL";
inline constexpr char ProductionUrl[] = "wss://www.deribit.com/ws/api/v2";
inline constexpr char TestnetUrl[] = "wss://test.deribit.com/ws/api/v2";
// Fallback fee estimates; account-specific rates replace them when available.
inline constexpr double DefaultMakerFee = .00015;
inline constexpr double DefaultTakerFee = .00035;
inline constexpr int MinimumTradingIntervalMs = 500;
inline constexpr int ReadIntervalMs = 100;
inline constexpr int InitialRetryMs = 1500;
inline constexpr int MaxRetryMs = 30 * Time::SecondMs;
inline constexpr int RequestTimeoutMs = 15 * Time::SecondMs;
inline constexpr int HeartbeatTimeoutMs = 35 * Time::SecondMs;
inline constexpr int HeartbeatIntervalSeconds = 10;
inline constexpr int SlowResponseMs = 2 * Time::SecondMs;
inline constexpr int SlowFeedMs = 15 * Time::SecondMs;
inline constexpr double AccountRateHeadroomMs = 1250;

inline QStringList hedgeInstruments()
{
    return {BtcPerpetual, EthPerpetual};
}
inline bool supportedHedge(const QString& instrument)
{
    return instrument == BtcPerpetual || instrument == EthPerpetual;
}
} // namespace Exchange
} // namespace scalper
