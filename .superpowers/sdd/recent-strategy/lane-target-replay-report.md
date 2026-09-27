# Saved combined target replay — source qualification

Production `64c8f58c`, scored-window correction `8005bc08`; owning fixtures
`f7a376ea`, `8005bc08`, `4e72a542`. Implementation preceded fixtures. No local
compilation, real payload read, performance run, or turnover qualification occurred.

New private files are `atx-impl/src/strategy_target_replay.hpp/.cpp` and the thin
`atx-impl/tools/equity_strategy_targets.cpp`. Root must register the private CPP,
standalone target, and `atx-impl/tests/strategy_target_replay_test.cpp`. Existing
composition, VM, IC, book, DSL library and prior receipts are unchanged.

The reader consumes the exact `atx.dsl-combined-signal/v1` schema frozen by
`5eb11cf4`: externally pinned manifest, same-handle SHA-checked payloads, exact
geometry, ordered axes, finite mask and decision support. The optional externally
pinned price role must match its manifest/source/axes/support; it reads only
close/raw close/presence plus transient support/axis verification. Aggregate
admission charges 64 MiB metadata/output reserve, 10 bytes/cell without prices or
28 bytes/cell with them, axes, O(N) rank/current/desired scratch, and O(D) results.
There is no DSL evaluation, label cache, or retained D×N target history.

`BaselineTargetV1` defaults to cadence 5 and interpolation .25. Desired tied-rank,
demean/gross normalization and current-weight update order reproduce the existing
`IcComposition::finish` arithmetic. `MonthlyTargetBudgetV2` additionally defaults
to .30 sum-absolute target changes per **decision-session calendar month**. It
charges deployment and forced membership exits first, then caps discretionary
interpolation by remaining budget. Mandatory exits are never postponed; their
unavoidable budget excess, partial gross and net exposures are reported. No
survivor renormalization or price-drift turnover is introduced. Partial calendar
months retain their actual observed session counts rather than being annualized.

Optional rough returns use target[d] with entry d+1 and endpoint d+2 strictly
inside the declared score window. Source absence and the existing fast-IC guard
(`abs(log adjusted return)>1.5` or adjusted magnitude exceeding raw by .10) make
held returns unavailable. Daily CSV distinguishes the observed component from
missing long/short/gross exposure; full-day gross/net returns are NaN on incomplete
support. Assumed one-way bps times target changes and annual borrow bps times
short weight/252 are explicit recipe knobs (default zero). These are constant
target-weight diagnostics with no price drift, fills, funding, corporate-action
accounting, self-financing NAV, capacity or realistic net-Sharpe claim. Summary
sets net_sharpe null and does not compound the incomplete period.

Seven source-only postimplementation cases cover actual composition bit parity,
ties/exits, deployment/calendar budget reset/forced breach and future invariance,
delayed missing and guarded short exposure with known modeled costs, actual
pinned artifact publication/tampering, flat/invalid/budget admission, shorter
score-window future-tail refusal, and actual optional price-role binding/missing
exposure. G0 independently approved production and the first six fixture definitions;
the last test-only price-role case was sent separately for review. Runtime remains
the parent-owned focused gate; original strategy profitability is unqualified.

CLI: `--combined PATH --combined-sha256 SHA --output NEW_DIR`, optionally
`--rule monthly-budget-v2 --monthly-budget .30`, `--cadence 5 --trade-fraction .25`,
`--role PATH --role-sha256 SHA`, `--one-way-bps B --annual-borrow-bps B`, and
`--max-bytes BYTES`. Parent plans one predeclared TRAIN baseline/budget comparison
on the same saved signal and frozen signs, not an optimizer grid or validation fit.
