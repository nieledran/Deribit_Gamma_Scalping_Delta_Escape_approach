# Deribit Gamma Scalper

Windows x64 C++20 / Qt 6 application that hedges selected existing BTC or ETH options with the corresponding USDC perpetual. Options are read-only. This is a long-gamma options strategy; nonpositive net gamma pauses hedging.

## Run

Build and test with `python scripts/build.py`, then run `start.cmd`. Profiles live in `gamma-dist/gamma-profiles/`, separately for each instrument and environment. The application starts paused.

1. Connect with the matching environment's API key. Credentials stay in memory.
2. Select existing option positions and click Apply.
3. Set **Delta coverage (%)** and **Delta Change**. Click Start / Resume.
4. Request spacing, fee estimates and the optional margin stop are under **Advanced**.

The first successful Start durably records the construction's initial valuation before submitting any hedge order. Stop/Resume, reconnect, restart, changing coverage and Close hedge preserve this P&L start. Changing the option basket creates a new construction and requires closing the previous hedge first.

## Hedge size and trigger

`Option delta = sum(signed option quantity x per-unit option delta)`.

`Target perpetual = -option delta x coverage / 100`, rounded toward zero to the exchange amount step.

Coverage defaults to 100%; 50% hedges half the current delta and 200% hedges twice the delta. There is no independent dollar hedge cap. Instrument quantity steps, fresh market data, margin and order ownership checks remain mandatory. Option API quantities are already in the underlying coin and are not multiplied by contract size again.

`Normalized option delta = option delta / sum(abs(option quantity))`.

Delta Change is a normalized delta tolerance around confirmed hedge inventory. With gross option quantity G, coverage fraction c and confirmed perpetual inventory I, the reference is -I/(G*c); the lower and upper thresholds are reference minus/plus Delta Change. Saved values and the .01 default are retained. This is not a price percentage: .1 can represent a much larger price move than 0.1%.

The bot pre-places a buy below and a sell above the market at the prices projected to reach those thresholds. The nonlinear option model is anchored to each leg's observed index, IV and exchange delta; perpetual/index basis maps index levels into limit prices. A common move of index and perpetual approaches those levels rather than moving them away. Order quantity reaches the coverage-adjusted futures target projected at its limit price, rounded to the exchange step. Large settings can make one side unreachable: that side is omitted and the UI explains why.

The main window separates the actual futures position, required futures quantity, BUY/SELL quantity to rebalance and the construction's delta goal. At 100% coverage the delta goal is zero; the futures target offsets the options and is generally nonzero. The execution panel shows both resting prices, remaining quantities, order states and actual hedge error. The tooltip gives current notional. Construction includes the perpetual as a separate leg with delta equal to its signed quantity and zero gamma/theta/vega. The risk graph includes the existing hedge, holds IV/time fixed and calculates only on request.

The quote strip shows Bid, Ask, Middle Price `(bid + ask) / 2` and break-even. Rebalance band is a diagnostic estimate in the execution panel. It estimates the price move in percent from a hedge reference to the next Delta Change trigger: `100 * Delta Change * gross option quantity / abs(net option gamma) / Middle Price`. It is a local constant-gamma estimate, not the distance remaining from the current price. Its tooltip estimates turnover and one-way maker/taker commissions at the applied coverage; lot rounding, spread and slippage are excluded. Coverage scales turnover, not the price trigger.

Break-even is the approximate price move in percent: `100 * sqrt(2 * abs(net option theta) / net option gamma) / Middle Price` (the tooltip retains its USD equivalent) for positive net gamma and nonpositive theta, before trading costs, funding and IV changes. It uses signed quantities and the existing API Greeks. Deribit caps theta to remaining lifetime below one day: the caption then changes from `daily` to `<1d`, including mixed-expiry baskets. This is a capped-horizon estimate, not a full-day forecast. Missing, expired or stale inputs suppress both move estimates. Construction shows `Gamma P&L (1%)`: `0.5 * position gamma * (0.01 * Middle Price)^2` in USD, including the position sign. This estimates the gamma-only contribution of a 1% move in either direction at constant gamma, excluding delta, theta, IV changes and costs. Each cell's tooltip retains the original Dollar Gamma coefficient `0.5 * position gamma * Middle Price^2`; for example, a coefficient of 293129 means about $29.31 for a 1% move. P&L is shown once, below Construction; its coverage warnings remain beside the period. These indicators use existing snapshots and add no exchange requests or realized-volatility history.

The theta horizon follows [Deribit's ticker Greeks documentation](https://docs.deribit.com/api-reference/market-data/public-ticker).

## Execution and recovery

All new orders are strict **post-only GTC limits**. There are no Aggressive/Balanced modes, cooldown or timed IOC fallback. Initial hedging and IV/time-driven correction use one passive order at best bid/ask; after it finishes, the bot prepares the pair. Close hedge first cancels both strategy orders, then works a single reduce-only passive order to zero.

Advanced also exposes trading-request spacing (default 250 ms) and fallback maker/taker fee estimates (default 0.015% / 0.035%). A stricter reported account rate limit overrides the selected spacing. Reported account fees override fallback estimates; relative account fee multipliers use the standard base fee schedule. These choices are saved per profile and take effect with Apply.

Quote updates use the existing asynchronous dispatcher, at most four trading requests per second (250 ms spacing), with one trading mutation in flight. A three-tick price deadband suppresses small changes; quantity changes and risk reductions are still applied. Levels inside the current spread are shown as too tight and omitted until usable. Post-only rejections are retried from fresh state, without crossing. Repricing can lose queue priority; accepted orders and a market touching a level do not guarantee fills.

Only cumulative fill increases change inventory. Both sides are recalculated after partial or full fills, and duplicates/stale notifications are ignored. Edits include already filled quantity in their total. A strategy order may close and reverse inventory in one order; only an entirely closing order is reduce-only. Reversal/opening quantities require margin checks. Fills can arrive on both sides before cancellation acknowledgements; they are accounted before replacements.

Passive initial hedging, correction and closing raise a journal/UI alert after 30 seconds unresolved. They remain passive; no elapsed-time or large-error condition silently switches to taker execution. Review the displayed delta error when alerted. Long gamma does not make every hedge error harmless or guarantee profitable trading, especially during news, changing IV or missed fills.

The journal includes accepted/partly filled/filled/cancelled orders and request method, numerical error code and reason. `not_open_order` is reconciled rather than reported as a maker rejection. Each profile has at most two resting hedge orders and one unresolved trading mutation. Trading RPCs retain at least 250 ms spacing (up to four requests/s; slower reported account limits take precedence). Reads have at least 100 ms spacing. Additional reconciliation/account reads run on anomalies, not on every market update. Account limits remain shared with other clients; see [Deribit rate limits, Appendix K](https://support.deribit.com/hc/en-us/articles/25944555524125-Deribit-Exchange-Rulebook-Deribit-FZE).

**Stop & Cancel** retains positions and cancels automatic resumption, including while disconnected. **Close hedge** targets zero using passive reduce-only limits; options remain open. Stale/invalid market data and temporary API congestion/rate-limit/settlement responses suspend requests and resume automatically once data and account state are verified. Foreign orders/trades, unexplained position differences, margin risk stops, insufficient funds, expiry and storage/invariant failures require explicit review. Risk stops cancel orders without automatically flattening. Selected options pause trading 30 minutes before expiry.

Do not manually trade this construction while the bot owns it. A local process lock prevents duplicate ownership of the same account/environment/perpetual. Connection-scoped cancel-on-disconnect is enabled. After a connection loss the client reconnects automatically; the trading worker resolves original requests, verifies position and data, then restores the prior Hedge or Close action. Paused bots stay paused. An application restart always starts paused. Quote timestamps are adjusted to local time using the measured exchange clock offset; genuine old quotes are still rejected. Clock calibration is refreshed on anomalous market snapshots.

## One construction P&L

The main window shows one P&L row below Construction/Orders/Journal, with the actual first-Start date and time in the computer's local timezone. It reports a single period, **since the first Start of this construction**, retained across application restarts:

`Net = options + hedge realized + hedge unrealized - option fees - hedge fees + funding`.

The report subtracts the initial valuation from the current valuation. Historical imports revalue both sides consistently. Earlier option profit/loss and earlier fees are excluded from the new period. The breakdown shows the components of that same period; there is no New Session action or automatic daily/application-session reset.

Perpetual average-cost accounting is independent of Deribit's daily settlement. Funding combines unique settled intervals with current accrual, avoiding double counting. Incomplete history/FX/valuation is labelled; stale data shows the last available valuation. USDC is valued at $1; amounts are USD equivalents for the selected construction, not all account collateral. Perpetual P&L is not a separate measurement of pure gamma profit.

**Export CSV** preserves raw trades, fees/funding, events, snapshots and legacy session rows, and adds `daily.csv` grouped by UTC day. Daily rows include actual observation boundaries and P&L changes over the recorded interval. Missing observations, unavailable history and open days remain explicitly partial; offline valuations are never invented.

## Existing profiles

Migration preserves profile identity, positions, orders and history. Old dollar limits are ignored; coverage starts at 100%. Version 1 profiles migrate to version 2 with inventory, history and Delta Change preserved; obsolete execution settings are ignored. Legacy orders are reconciled and cancelled before the new strategy is enabled. Since old profiles lack an exact first-Start valuation, their new P&L period begins on the next successful Start after updating. Old data remains exportable.

Delete local profile remains an explicit destructive action: it cancels owned labels, verifies cancellation and clears local history/settings. It is not the normal recovery path for partial fills. Exchange positions remain and are not silently adopted.

## Development

See [docs/BUILDING.md](docs/BUILDING.md). Tests use loopback fake WebSocket servers and temporary profiles; no real keys or exchange orders are used. `DeribitGamma --demo` opens an offline demonstration. Analytics, export and graph calculation remain outside the trading and GUI threads.

MIT license; see `LICENSE` and `THIRD_PARTY.md`.
