# A4 explicit mine execution consumer: source qualification

Status: source implemented and postimplementation fixtures frozen; no configure, C++ build, runtime, market-data read or performance measurement performed by this lane. This is a bounded execution-objective slice, not completion of W2-A4 or evidence of tradeable alpha.

## Reviewable changes

- Mine production: `184ee024` (`stage_equity_mine.hpp/.cpp`).
- Owning fixtures: `b93263ea` (two `EquityMineExecution` checks in the existing `stage_equity_mine_core_test.cpp`).
- Core-owner declaration dependencies: `343e4a2b`, `3251e057`, `6cd17013`. Core implementation `1448c7e0`; source-presence repair `2621ad07`. Fitness/search implementation is a separate owner release; the mine caller uses the agreed final execution configuration, context pointer and explicit failure fields.
- Root owns source/test registration and compiled qualification. G0 independently reviews this consumer; this report does not self-approve it.

## Actual caller behavior

`ScoreCfg.execution_rule` remains `LegacyStreamsV1` by default. Explicit `DelayedSurfaceV2` requires a prepared immutable `MineData.execution` for every role. The direct scorer, `mine_train`, train SearchDriver, `mine_validate`, individual holdout, family/admitted blends all use that route. Context-free V2, wrong panel/policy/window/price/support, or a conflicting search rule is an explicit error. There is no new CLI flag or invented snapshot loader; command-line adoption is not claimed.

The mine book explicitly requires rank weights, no winsorization, dollar neutrality, gross one, no industry/truncation adjustment. Context configuration owns AUM, borrow and cost assumptions. Existing delay/min-name/return-guard settings and each role's maturity cutoff must match it. Context identity binds the core's exact clocks, panel marks/support, source declaration, cost snapshot identities and execution recipe. The consumer additionally records its IC/HAC inference settings without decimal-rounding collisions. Caller-supplied source provenance remains a declaration, not acquired proof of market inputs or locate availability.

V2 delegates marked-dollar holdings, participation-capped actual fills, cash/NAV, costs and borrow to the shared kernel. An unpriceable nonzero request or unavailable held return never becomes a successful zero-cost observation. A negative train-selected orientation reruns the kernel; legacy `flip_score` is prohibited for V2. Legacy PnL arithmetic, candidate identity and emitted score JSON retain their old path.

The kernel's full calendar is projected onto its exact contiguous mature realized interval. Statistics, family-correlation sketches and Romano-Wolf use that same finite interval. TrialRegistry retains its actual realized offset relative to the role calendar instead of padding a tail with zero returns. V2 IC endpoints must also mature inside the role cutoff. Active candidate and screened-trial identities include the execution context; V1 hash bytes receive no suffix. Score/outcome records and conditional report fields expose the context, recipe, realized range, actual turnover and separate execution/borrow cost sums.

Retained context plus concurrent execution output/scratch is admitted using checked subtraction/division against the declared execution budget. Existing VM allocations remain a separate caller budget; no whole-process RSS claim is made.

## Bounded independent core finding

Inspection of `1448c7e0` found that nonzero entry fills and held marks accepted a finite backing price despite absent Panel presence. The core owner fixed both actual-time branches in `2621ad07`; source review confirms this repair. It refuses the missing observation at entry/realization without using future missingness to change the earlier decision. G0 owns the separate finite-backing-value absence fixtures.

## Postimplementation fixtures, not run

1. Exact mine/shared-kernel mature PnL, gross and turnover; asymmetric negative borrow; missing-context, support, window and unpriced-trade refusal; mutation strictly after the role cutoff leaves mature PnL/IC unchanged.
2. Actual mine train sign selection with independent rescoring; correct registry realized offsets; validation, individual holdout and both blend seams; changing the supplied cost/source identity changes trial identity; empty holdout still refuses context-free requested V2.

`git diff --check` passed after source and fixture edits. No test result is asserted before root's combined qualification.

## Explicit remaining scope

Residual WLS against exposures, the full multihorizon HAC/half-life objective, the target turnover-band objective, contiguous fidelity rungs, full registry coverage for every fidelity attempt, an authenticated cost artifact loader/CLI selection, real calibration/locate evidence, and empirical/RSS/performance acceptance remain open. The core owner deliberately refuses unsupported V2 strided fidelity, unbound pools, weak panels and resume in this slice; those are not silently converted to legacy fitness. The original plan's broad residual-alpha/turnover acceptance and real-data tradeability are not claimed.
