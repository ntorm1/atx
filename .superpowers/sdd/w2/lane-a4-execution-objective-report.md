# W2-A4 bounded delayed execution objective

Status: source and postimplementation fixtures frozen; compilation/runtime pending at root. This is an execution-objective slice, **not full W2-A4 completion**. No local configure/build/test/benchmark or data execution occurred.

## Implemented contract

`ExecutionObjectiveRule::LegacyStreamsV1` stays the default and follows the unchanged legacy arithmetic. `DelayedSurfaceV2` requires a prepared immutable context. The actual fitness/SearchDriver path reuses its evaluated SignalSet; programmatic mine consumers are owned separately by the audit lane. No application default switch or snapshot-file loader is provided.

At decision d, observed decision marks, signal and membership form the declared WeightPolicy target. Dollars are fixed using NAV known at d and enter a bounded delay ring. At e=d+delay, the marked current holdings are traded toward those stored dollars. Snapshot-d liquidity/participation caps price the actual signed delta; unknown nonzero-trade cost refuses scoring. Partial unfilled targets are canceled for that decision. Cash incorporates long purchases, short proceeds and entry costs immediately. The e-to-e+1 return and calendar-day modeled short borrow close at endpoint e+1. Entry costs are attributed with that endpoint while already reducing cash before the next decision. Negative sign is independently executed.

The context requires mark[d] < decision[d] < mark[d+1], exact snapshot decision clocks and instrument ordering, explicit source/role/price identities, and strict-prior CostSurface input availability. The identity hashes policy/config, prices, panel presence, decision membership, ReturnGuard, clocks, groups and every snapshot recipe/content hash. Missing or guarded held marks and missing nonzero-fill entry marks refuse at the actual clock; they never filter an earlier decision using future availability. Review correction `2621ad07` also rejects finite placeholders under false panel presence at both boundaries.

Output retains the full calendar with NaN/invalid structural rows. Only `[first_realization, realization_end)` is a mature contiguous interval; the corresponding decision is endpoint-delay-1. Actual held entry weights, net/gross returns, one-way costs, borrow, filled turnover, eligible names, capped names and NAV diagnostics are retained. There is **no terminal liquidation**: the last NAV includes marked remaining positions and excludes hypothetical closing cost. This is a fractional total-return dollar book, not a claims-aware share/corporate-action replay; cash funding has no input, so negative cash below -32*machine-epsilon*current-positive-NAV returns Unavailable after completed fills/costs and after borrow/marks. The tolerance is not scaled by gross leverage or initial NAV; no cash is clamped. Transient per-name cash during a completed rebalance is permitted so same-batch sale proceeds can fund purchases. The refusal policy is part of the context recipe hash. Modeled borrow fees do not assert locate availability.

V2 factory fitness uses annualized net Sharpe of mature returns in the historical `wq` slot, plus actual filled-dollar/NAV turnover and explicitly labeled execution metadata. It makes no CPCV/OOS claim. It refuses legacy cost/turnover/capacity overlays and weak-panel evaluation. Search refuses strided fidelity, checkpoints/resume, and unbound nonempty AlphaStore pools; PoolView has no calendar/execution identity and explicitly refuses V2. Supported empty-pool diversification is 1. No unbound pool is silently scored, compressed or zero padded. Runtime execution errors abort with `SearchResult.execution_invalid/error`, not a successful zero fitness.

## Memory and build boundaries

New production TUs are `src/alpha/streams.cpp` and `src/factory/execution_objective.cpp`; root must register them. Headers hold the lightweight execution declarations. Legacy streams extraction is a separate mechanical commit, with unchanged normalized 139-line body hash `0e634beb39fcbe299ce8e6641d5f39f4206ed38128d3f079bf32952360cf3ffd` recorded in ignored `build-equity/a4-streams-body-move.json`.

Admission uses checked arithmetic for context copies, retained snapshots, full outputs, delay*N queued targets and worker scratch. CostSurface::bytes counts retained vector/string capacities with saturation and explicit 1 KiB allocator/control-block slack per snapshot, conservatively charging shared snapshots once per retained entry. Context/scratch add separate fixed slack. These are payload admission charges, **not measured RSS**. Concurrent context+worker charges must fit max_working_bytes. Existing VM, search caches and outer consumer storage remain separate and are not claimed inside this execution budget.

## Frozen source and acceptance

- API `343e4a2b`; legacy move `45957e45`; diagnostics `3251e057`; search declarations `6cd17013`.
- Core `1448c7e0`; presence refusal `2621ad07`; actual fitness/search consumers `0270b79c`.
- Own postimplementation fixture `80b231a5`: `tests/factory/execution_fitness_test.cpp`, filter `ExecutionFitnessV2.*` (five checks). It checks mature consumer moments/calendar mask, signal reuse, worker determinism, unpriced refusal, mismatches/unsupported paths/budget, and explicit legacy parity.
- Funding follow-up: nonneutral leveraged longs and asymmetric caps cannot receive free cash financing; independent source regression is owned by G0.
- G0 owns independent hand-ledger/prefix/clock/core fixtures; audit owns mine consumer fixtures and independent review. Their frozen SHAs/results must be added by the integrator; no runtime approval is asserted here.
- `git diff --check` passed. No CMake or shared ledger edited.

Remaining A4 gates: residual exposure/WLS contract, HAC objective, half-life/turnover floor policy, calibrated preferences, recipe-bound nonempty pools and durable resume, source-clock end-to-end X1 qualification, target performance/real market validation, and programmatic consumer/runtime qualification. Full W2 gate remains open.
