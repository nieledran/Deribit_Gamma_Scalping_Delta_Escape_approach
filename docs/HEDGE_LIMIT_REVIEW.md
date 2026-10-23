# Hedge limit: historical issue and resolution

The former `limited` flag compared residual delta after a hypothetical rounded target against 0.51 amount steps. It mixed ordinary rounding with an actual dollar-cap restriction and could be true at zero inventory or far below the configured cap.

The dollar cap and `limited` flag have now been removed. Target perpetual quantity is `-aggregate option delta * hedge_percent / 100`, rounded toward zero to the exchange step. The main controls use delta coverage, default 100%, supporting 50%, 200% and higher. Strike times quantity is not a hard limit on the current dollar value of a delta hedge.

The UI shows actual quantity, target and remaining quantity. Current dollar notional is informational. Margin, exchange amount/tick steps, ownership and fresh-data checks remain in effect. Old profile history is retained; obsolete dollar-limit settings are ignored during migration.

See `engine/hedger.cpp`, `trading/execution.cpp` and the README for current behavior.
