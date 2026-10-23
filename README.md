# Deribit Gamma Scalper

Windows x64 C++20 / Qt 6 application that hedges selected existing BTC or ETH options with the corresponding USDC perpetual. Options are read-only.

## Run

Build and test with `python scripts/build.py`, then run `start.cmd`. Profiles live in `gamma-dist/gamma-profiles/`, separately for each instrument and environment. The application starts paused.

1. Connect with the matching environment's API key. Credentials stay in memory.
2. Select existing option positions and click Apply.
3. Set **Delta coverage (%)**, **Delta Change**, and **Cooldown (s)**. Click Start / Resume.
4. Execution policy, passive wait, slippage and the optional margin stop are under **Advanced**.

The first successful Start durably records the construction's initial valuation before submitting any hedge order. Stop/Resume, reconnect, restart, changing coverage and Close hedge preserve this P&L start. Changing the option basket creates a new construction and requires closing the previous hedge first.

## Hedge size and trigger

`Option delta = sum(signed option quantity x per-unit option delta)`.

`Target perpetual = -option delta x coverage / 100`, rounded toward zero to the exchange amount step.

Coverage defaults to 100%; 50% hedges half the current delta and 200% hedges twice the delta. There is no independent dollar hedge cap. Instrument quantity steps, fresh market data, margin and order ownership checks remain mandatory. Option API quantities are already in the underlying coin and are not multiplied by contract size again.

`Normalized option delta = option delta / sum(abs(option quantity))`.

Delta Change measures movement in normalized option delta; Cooldown defaults to 10 seconds. The minimum-change validation includes the coverage multiplier. Own hedge fills do not change option delta. A completed operation keeps the option-delta reference used to calculate its target, so market movement during execution remains eligible for the next decision. Changing coverage schedules recalculation at the next Start.

The main window separates the actual futures position, required futures quantity, BUY/SELL quantity to rebalance and the construction's delta goal. At 100% coverage the delta goal is zero; the futures target offsets the options and is generally nonzero. Rebalancing still obeys Delta Change and Cooldown. The tooltip gives current notional. Construction includes the perpetual as a separate leg with delta equal to its signed quantity and zero gamma/theta/vega. The risk graph includes the existing hedge, holds IV/time fixed and calculates only on request.

The quote strip shows Bid, Ask and Middle Price `(bid + ask) / 2`. Rebalance band estimates the price move in percent from a hedge reference to the next Delta Change trigger: `100 * Delta Change * gross option quantity / abs(net option gamma) / Middle Price`. It is a local constant-gamma estimate, not the distance remaining from the current price. Its tooltip estimates turnover and one-way maker/taker commissions at the applied coverage; lot rounding, spread and slippage are excluded. Coverage scales turnover, not the price trigger.

Break-even is the approximate price move in percent: `100 * sqrt(2 * abs(net option theta) / net option gamma) / Middle Price` (the tooltip retains its USD equivalent) for positive net gamma and nonpositive theta, before trading costs, funding and IV changes. It uses signed quantities and the existing API Greeks. Deribit caps theta to remaining lifetime below one day: the caption then changes from `daily` to `<1d`, including mixed-expiry baskets. This is a capped-horizon estimate, not a full-day forecast. Missing, expired or stale inputs suppress both move estimates. Dollar Gamma in Construction is `0.5 * position gamma * Middle Price^2`, including the position sign. Multiply by the squared fractional price move to estimate gamma P&L (for a 1% move, multiply by `0.0001`). P&L is shown once, below Construction; its coverage warnings remain beside the period. These indicators use existing snapshots and add no exchange requests or realized-volatility history.

The theta horizon follows [Deribit's ticker Greeks documentation](https://docs.deribit.com/api-reference/market-data/public-ticker).

## Execution and recovery

All orders are limit orders. **Balanced is the default**.

| Policy | Passive attempts before crossing limit IOC |
|---|---|
| Balanced | Two |
| Aggressive | One |
| POST-ONLY | Never crosses |

Passive attempts wait 2 seconds by default. Balanced/Aggressive use ordinary GTC limit orders, BUY at bid and SELL at ask, without post-only. They can execute as taker if the book moves. POST-ONLY retains strict post-only/reject behavior. Crossing IOC prices respect the configured adverse slippage bound (default 5 bps). No-op edits are skipped without delaying escalation. Accepted maker orders do not guarantee execution.

Partial executions are normal: only cumulative fill increases update inventory; duplicate order notifications and trade IDs are not counted twice. Editing uses total order amount including already filled quantity. Both create and edit limit reduce-only amounts to the remaining position. Reversals close the old side before opening the new side.

A position response can arrive before the matching fill notification. Such a mismatch suspends new orders and starts an owned-order/fill reconciliation. The bot waits for outstanding mutations, resolves original order labels, cancels any remainder and verifies the account position up to three times, at least 500 ms apart. Explained differences automatically restore the previous action, including Close hedge. Persistent unexplained differences pause trading with actual/expected quantities; profile/history are retained. Unknown request outcomes never authorize duplicate creates.

The same bounded verification runs for a mismatch during initial connection. Mismatching raw position responses are retained in the event export for diagnosis. Manual position changes remain paused until explicitly reconciled; reconnecting does not silently adopt an external close or delete accounting history.

The journal includes accepted/partly filled/filled/cancelled orders and request method, numerical error code and reason. `not_open_order` is reconciled rather than reported as a maker rejection. Each profile has at most one live hedge order and unresolved trading mutation. Trading RPCs retain at least 250 ms spacing (up to four requests/s; slower reported account limits take precedence). Reads have at least 100 ms spacing. Additional reconciliation/account reads run on anomalies, not on every market update. Account limits remain shared with other clients; see [Deribit rate limits, Appendix K](https://support.deribit.com/hc/en-us/articles/25944555524125-Deribit-Exchange-Rulebook-Deribit-FZE).

**Stop & Cancel** retains positions and cancels automatic resumption, including while disconnected. **Close hedge** targets zero using the selected execution policy; options remain open. Stale/invalid market data and temporary API congestion/rate-limit/settlement responses suspend requests and resume automatically once data and account state are verified. An excessive IOC spread waits for a price within the slippage bound. Foreign orders/trades, unexplained position differences, margin risk stops, insufficient funds, expiry and storage/invariant failures require explicit review. Risk stops cancel orders without automatically flattening. Selected options pause trading 30 minutes before expiry.

Do not manually trade this construction while the bot owns it. A local process lock prevents duplicate ownership of the same account/environment/perpetual. Connection-scoped cancel-on-disconnect is enabled. After a connection loss the client reconnects automatically; the trading worker resolves original requests, verifies position and data, then restores the prior Hedge or Close action. Paused bots stay paused. An application restart always starts paused. Quote timestamps are adjusted to local time using the measured exchange clock offset; genuine old quotes are still rejected. Clock calibration is refreshed on anomalous market snapshots.

## One construction P&L

The main window shows one P&L row below Construction/Orders/Journal, with the actual first-Start date and time in the computer's local timezone. It reports a single period, **since the first Start of this construction**, retained across application restarts:

`Net = options + hedge realized + hedge unrealized - option fees - hedge fees + funding`.

The report subtracts the initial valuation from the current valuation. Historical imports revalue both sides consistently. Earlier option profit/loss and earlier fees are excluded from the new period. The breakdown shows the components of that same period; there is no New Session action or automatic daily/application-session reset.

Perpetual average-cost accounting is independent of Deribit's daily settlement. Funding combines unique settled intervals with current accrual, avoiding double counting. Incomplete history/FX/valuation is labelled; stale data shows the last available valuation. USDC is valued at $1; amounts are USD equivalents for the selected construction, not all account collateral. Perpetual P&L is not a separate measurement of pure gamma profit.

**Export CSV** preserves raw trades, fees/funding, events, snapshots and legacy session rows, and adds `daily.csv` grouped by UTC day. Daily rows include actual observation boundaries and P&L changes over the recorded interval. Missing observations, unavailable history and open days remain explicitly partial; offline valuations are never invented.

## Existing profiles

Migration preserves profile identity, positions, orders and history. Old dollar limits are ignored; coverage starts at 100% and execution migrates once to Balanced. Later explicit execution choices are retained. Since old profiles lack an exact first-Start valuation, their new P&L period begins on the next successful Start after updating. Old data remains exportable.

Delete local profile remains an explicit destructive action: it cancels owned labels, verifies cancellation and clears local history/settings. It is not the normal recovery path for partial fills. Exchange positions remain and are not silently adopted.

## Development

See [docs/BUILDING.md](docs/BUILDING.md). Tests use loopback fake WebSocket servers and temporary profiles; no real keys or exchange orders are used. `DeribitGamma --demo` opens an offline demonstration. Analytics, export and graph calculation remain outside the trading and GUI threads.

MIT license; see `LICENSE` and `THIRD_PARTY.md`.
