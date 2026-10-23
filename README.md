# Deribit Gamma Scalper v1

A Windows x64 C++20 / Qt 6 desktop application that delta-hedges **selected existing BTC or ETH option positions** with their **USDC perpetual**. Options are read-only. There is no grid, option order entry, multiplier, price trigger, preplaced hedge, or automatic option roll.

## Run

Build with `python scripts/build.py`, then double-click `start.cmd`. The application is `gamma-dist/DeribitGamma.exe`. Select `BTC_USDC-PERPETUAL` or `ETH_USDC-PERPETUAL` and Testnet/Production in the opening dialog. Each pair has its own profile in `gamma-profiles/` beside the executable. The old grid databases and executable are incompatible and are not loaded.

1. Enter the matching environment's API Client ID and secret in the top row, then click **Connect**. Credentials are memory-only; the secret field clears after connecting. Use a key with account/trade read permissions and trading permission for the perpetual. A second read-only history connection uses the same credentials; secrets are never saved or exported.
2. Open **Select options**, filter existing positions by expiry or Call/Put, and choose the legs. Save the draft with **Apply** in the main window. Both coin-settled and USDC-settled options for the chosen asset are supported. The first use requires a flat perpetual and no foreign orders. Close the bot hedge before changing the basket. Other positions are excluded from construction P&L but still affect account margin.
3. Set a positive **Max hedge ($)**, **Delta Change**, **Cooldown (s)**, and execution policy; click **Apply**, then **Start / Resume**. Maker wait, slippage and an optional account MM stop are under **Advanced**. New profiles have the user MM stop disabled; old positive thresholds remain enabled when migrated.
4. Watch **Net P&L** in the price row (since inception, including fees and funding) and remaining net delta. **P&L** opens the three reporting periods and breakdown. Incomplete history is labelled; unavailable valuation is a dash. **Hedge P&L is the contribution of the perpetual, not a measurement of pure gamma profit.**

The program always starts paused. A reconnect reconciles durable orders/fills/positions and returns to paused. Review and explicitly resume. Unresolved exchange outcomes block further orders; reconnect to look up the original label instead of sending a duplicate. A separate process lock prevents two gamma profiles owning the same account/environment/perpetual on this computer.

Open options lists only nonzero positions whose expiry is still in the future. Closed or expired selected legs remain in the accounting basket for history, settlement and P&L; they are not offered as tradable selections. Account polling discovers replacement positions without reconnecting. A read error stops trading safely but does not permanently disable later position polls.

Start requests fresh perpetual and selected-option snapshots before enabling hedging. Quiet selected markets also receive periodic `public/ticker` refreshes, so freshness does not depend solely on unsolicited ticker events. Exchange timestamps are checked; stale or invalid snapshots still block trading. Stop cancels a pending Start, including while market-data requests are outstanding.

The main window starts at 960 x 540 with Qt Fusion controls and a light palette matching Grid Bot. Construction, Orders and Journal share the central area. The construction table includes position Greeks; its narrow Data column shows quote freshness. Double-click a leg for contract details. Bid and ask have short, fading green/red backgrounds on change; larger moves have stronger shading. Production is marked in red. **Actions** contains Close hedge, New Session, Export CSV and Delete local profile. Destructive items are red and show `(Locked)` while disabled; confirmations default to No.

Live fields have separate, stable columns: changing values or status text cannot move adjacent fields. Overlong fields are elided with full text in tooltips. Main buttons have equal dimensions. Unsaved settings are indicated only by subtle moving blue/white stripes on **Apply**; restoring the applied settings removes the highlight. **Hedge limit** always shows actual bot perpetual notional at the current mid price / applied maximum, with remaining capacity in its tooltip. It excludes pending orders and theoretical target positions. The trading limit calculation is unchanged; see [the hedge-limit review](docs/HEDGE_LIMIT_REVIEW.md) for why the old indicator could appear at zero inventory.

The information area uses three compact frames without headings: delta (net, normalized and reference), hedge (inventory, notional limit and legs), and execution/escape (bot state, threshold and remaining cooldown). Market/P&L and connection/status fields use matching native Qt frames. **Actions** follows **Risk graph**; **P&L** is directly below **Advanced**. Apply uses the Grid Bot's original blue palette, stripe geometry and animation speed.

The connection indicator follows Connect. Footer **Latency** is the exchange request round-trip time; **Data age** is the age of the latest perpetual quote, not another latency. **Clock** shows whether the exchange clock is ahead of or behind the PC. Per-option quote ages are in Data tooltips. A forward ticker timestamp of up to 1 second is tolerated to prevent harmless subsecond clock differences from breaking synchronization or flashing Live/Stale. Quotes older than 5 seconds, larger clock leads and invalid bid/ask still block trading. No system clock or stored timestamp is modified.

**Risk graph** opens a separate, nonmodal snapshot of the construction at fixed current IV/time. It calculates once on opening and on **Refresh**. Choose Delta, Gamma, Theta or Vega without recalculation; resizing and minimizing also preserve the snapshot. The timestamp stays visible. P&L includes the current hedge, never hypothetical future hedge trades. The initial price range is +/-15%. Graphs are never computed while the window is closed.

The legend below the chart shows actual solid, dashed, dotted and vertical line samples beside their names. The dotted lower/upper delta triggers are scenario prices where normalized option delta reaches the escape threshold relative to the reference; hover for the values. The IV line shows each leg's implied volatility from the same snapshot, fixed until Refresh. There is no single averaged construction IV.

The two vertical axes scale independently with rounded ticks. Positive P&L starts at
zero at the bottom; losses remain visible if the curve becomes negative. The selected
Greek has a central zero and symmetric limits. Refresh preserves each scale unless
values outgrow it or the P&L curve changes sign.

## Delta Change and limits

`Normalized Delta = sum(signed option base quantity × per-unit option delta) / sum(abs(option base quantity))`.

API option position amounts are already in the underlying coin: **do not multiply them by contract size again**. Individual ticker delta/gamma/theta/vega are multiplied by signed position quantity once. Account `DeltaTotal` uses a different price-adjusted convention and is not substituted for selected option Greeks.

For 0.1 BTC gross options, a move from delta 0.50 to 0.54 is **0.04 delta points**, **0.004 BTC** of changed exposure. A straddle includes both legs in gross quantity. Escape price markers in the risk graph are scenario indicators, not trading triggers. Time and IV can change delta without spot moving.

The desired hedge is `-aggregate option delta`, rounded to the instrument's minimum amount and capped by the **whole-position** maximum notional, including open order exposure. Initial hedging may be partial at this cap. `HEDGE LIMIT REACHED` means directional exposure remains. Own hedge fills do not create option delta changes, and a capped/rounded target does not cause endless retries. A price-only increase in the value of existing inventory does not force a sale; further increases are blocked while reductions remain possible.

The escape reference is the option basket's normalized delta at completion, including any market drift during execution. Execution drift and size rounding remain visible in net delta. Cooldown defaults to **10 seconds** after a completed decision. After cooldown the latest snapshot is evaluated; old signals are not queued. This is a rate-control choice, not an optimal economic threshold or a guarantee of bounded unhedged risk.

## Execution and controls

All trading uses limit orders. Accepted maker orders are repriced with `private/edit`; its amount is the total including fills, not an additional quantity. Cancellation is confirmed before changing direction or replacing a GTC order with IOC. Each profile has at most one unresolved trading mutation and one live hedge order.

| Mode | Maker attempts before aggressive limit execution |
|---|---|
| POST-ONLY (default) | Never becomes taker |
| Balanced | Two |
| Aggressive | One |

A maker attempt waits **2 seconds** by default. A post-only rejection also consumes an attempt. Aggressive orders cross the best opposite quote within the configured slippage limit (**5 bps** initially); new aggressive orders use IOC. A limit order can still pay taker fees. An accepted maker order is not a promise of timely execution. Partial fills reduce the outstanding requirement.

- **Stop and Cancel:** stop decisions and cancel only owned orders; positions remain.
- **Close Hedge:** close only the bot's perpetual inventory using the selected limit policy. Options remain open. This can increase directional exposure of the remaining portfolio.
- **New Session:** begin a new reporting period without changing inventory, orders, the strategy's delta reference or Lifetime history.
- **Delete local profile:** the Grid Bot recovery path, also available after a position mismatch. After confirmation it cancels only this profile's exact order labels, verifies cancellation twice, reads the remaining exchange position, then creates a fresh empty profile in one database transaction. Local settings, trade history and accumulated P&L are cleared; export CSV first if needed. Exchange positions, manual/other-profile orders, exchange history and exported files remain. Failed cancellation or verification retains the profile and its P&L. A leftover perpetual is recorded as external, never adopted as a new bot hedge; close it on the exchange and reset again before starting Gamma.

Do not manually trade the selected options or hedge perpetual while this construction is in use, including while the bot is paused or closed. Stop & Cancel retains its position records. If you manually change those positions, export the old P&L and use Delete local profile before starting a new construction; there is no automatic import of manual trades.

Margin usage is `maintenance margin / margin balance`, calculated from exchange values. Cross collateral uses the total USD fields; segregated margin uses settlement account fields. **Advanced** shows the margin model and scope. The optional user threshold stops decisions and cancels orders, leaving inventory open. Disabling it retains margin freshness and execution checks. Reducing a perpetual does not guarantee lower portfolio margin. Margin freshness, available funds and the exchange order margin estimate are checked before increases. Missing/unsafe margin, stale selected data, external option quantity changes, position mismatches, storage failure and foreign hedge orders stop trading. Risk stops cancel orders and **do not automatically flatten**. Selected options enter an expiry pause 30 minutes before their own expiry. The hedge remains until a manual decision.

Trading RPCs are spaced by at least **500 ms**; slower account limits take precedence. Creates/edits/cancels are prioritized over ordinary reads, mutations wait for the previous mutation outcome, and queued increases are checked again before sending. All RPCs have at least 100 ms spacing. Limits are shared with other API clients. Connection-scoped cancel-on-disconnect is enabled; the application does not change account-wide CoD configuration.

## Accounting and export

The durable core database stores orders, original request intents and confirmed fills. An independent analytics database stores imported exchange transaction rows, reporting periods, valuation snapshots and events. Analytics/export/graph computation run outside the trading and GUI threads. The GUI polls a latest-snapshot mailbox at most 5 times per second and populates only visible tables. Reconciled historical valuations are cached and repriced using current marks; new/corrected imports, fills, funding state and construction changes invalidate the relevant calculations. Incomplete valuations use full replay. No UI acknowledgement is needed to hedge.

| Period | Start |
|---|---|
| Lifetime | Last opening from flat of each selected current option episode; adds this bot's hedge sessions |
| Session | New application run or New Session; baseline is the first coherent connected snapshot |
| Today UTC | 00:00 UTC, if a valuation baseline is available; otherwise visibly partial |

Option history imports premiums, partial closes, fees and settlement information. Old closed option episodes and preexisting manual perpetual hedges are not silently attributed. Missing history/FX/valuation baselines are marked incomplete. Backfilling history rebases session comparisons consistently rather than creating a spurious session profit. Quotes/Greeks that were never recorded are not fabricated.

Perpetual average cost and realized P&L are maintained independently from daily exchange sessions. **Settlement at 08:00 UTC does not reset local Lifetime or Session P&L.** Funding combines unique settled transaction-log intervals with the current exchange session's `realized_funding`, excluding the pre-bot baseline. Current-session log intervals are not added twice. During settlement-log delay a recent saved boundary snapshot can carry the prior accrued amount provisionally; coverage remains incomplete until reconciliation. Settlement cashflows and `session_rpl` are audit inputs, not extra profit. There is no daily theta-coverage ratio: API theta is capped to the remaining option lifetime below 24 hours.

All storage/export timestamps are UTC, unaffected by local DST. Values are shown as **USD equivalents, with USDC valued at $1**. Coin option premiums and commissions use their recorded index conversion when available; missing historical conversions are explicitly incomplete. This is a selected-positions report, not the USD return of all collateral or the account's coin cash balance. The graph holds current IV/time/carry fixed, anchors at actual marks and includes the existing hedge only; it does not predict future hedge P&L.

**Export CSV** writes a consistent set of UTF-8 BOM files that Excel can open: summaries, trades, fees/funding audit rows, Greeks/snapshots, events and sessions. Raw JSON accompanies flattened fields for auditability. Numerical data retains full precision. Export does not block the trading thread.

## Development and validation

See [docs/BUILDING.md](docs/BUILDING.md). Tests use local fake WebSocket servers and temporary profiles, never live credentials. `DeribitGamma --demo` shows synthetic portfolio data without network/profile writes. `--demo --smoke-test --screenshot PATH` provides an offline UI check.

Source layout: `main.cpp` only creates the application and delegates startup to `ui/startup.cpp`. Palette, fonts and shared styles live in `ui/theme.h` and `ui/theme.cpp`. User setting defaults are defined once in `Config` (`engine/domain.h`); shared timing, exchange and safety constants are in `config/settings.h`. Application metadata is generated from CMake's project name/version.

The window is split into lifecycle, layout/actions, snapshot rendering, auxiliary windows (`ui/window_tools.cpp`) and offline fixtures. Shared confirmations are in `ui/dialogs.cpp`; number formatting is in `ui/format.h`. Scenario requests carry an ID and immutable trading snapshot; results for a closed window are discarded. Advanced diagnostics separate GUI/analytics calculation time, RPC queue time and hedge execution duration.

`trading/worker.h` owns the trading state. Its implementation is divided by responsibility: `worker.cpp` (lifecycle/persistence/snapshots), `connection.cpp`, `positions.cpp`, `market_data.cpp`, `orders.cpp`, `reconciliation.cpp`, `controls.cpp`, `execution.cpp` and `hedging.cpp`. These translation units share that one state owner; they do not create independent copies of inventory or configuration. `analytics/worker.cpp` publishes valuations, `analytics/history.cpp` handles typed history jobs and pagination, and `analytics/ledger_export.cpp` handles CSV serialization. Synthetic demo values and test scenarios are fixtures, not live trading defaults.

The original copied grid source is preserved in the initial Git commit. `legacy-archive/` contains the copied runtime/build artifacts and consistent old Production database snapshots; it is ignored by Git and packaging. Dependencies are cached in `.deps/`. No old production profile is migrated automatically.

MIT license; see `LICENSE`. Qt licensing and portable packaging are described in `THIRD_PARTY.md`.
