# Independent A4 execution and mine source review

Review date: 2026-09-26. Reviewer: G0 lane, pool-3. Source-only review;
no compilation, runtime test, benchmark, warehouse access or real-data run.

Verdict: **approve the bounded source packet through `c0430524` for the
root's combined compilation and focused runtime qualification**. All three
actionable findings below are closed. This is not full W2-A4 acceptance.

## Reviewed packet

- Context API `343e4a2b`, legacy stream extraction move `45957e45`, diagnostics
  and support binding `3251e057`, fitness/search declarations `6cd17013`.
- Chronological execution core `1448c7e0`, physical-presence correction
  `2621ad07`, fitness/search consumers `0270b79c`, owning consumer fixtures
  `80b231a5`, scope/report `8709f382`, cash-funding refusal `ba47b521`, and
  signed-deflation compatibility guard `c0430524`.
- Programmatic mine consumers `184ee024`, owning mine fixtures `b93263ea`,
  reports `64e7c932` and `d2ef00fc`. The duplicate core imports on the mine
  branch are not additional production changes and need not be imported twice.
- Independent postimplementation core fixtures `c368ed98`, new
  `atx-engine/tests/factory/execution_objective_test.cpp`, nine tests selected
  by `ExecutionObjective.*`. Root owns source/test registration and the next
  combined qualification build.

## Findings and closure

1. **Absent finite marks could be traded or held.** Core `1448c7e0` checked
   finite positive entry/endpoint prices but omitted the underlying Panel
   presence bit. An absent cell containing finite 777 could produce a fill or
   PnL. Fix `2621ad07` checks actual entry presence for every nonzero fill and
   both held endpoints before marking. This refuses the later unavailable
   event without retrospectively filtering earlier decisions. The independent
   fixture tests entry and held-endpoint absence separately, while decision
   membership remains one.
2. **Unmodeled cash financing was reachable.** A supported raw, nonneutral,
   gross-two positive policy buys $2,000 with NAV $1,000. Asymmetric caps can
   also leave a neutral gross-three target with $1,500 purchases but only $100
   short proceeds. Fix `ba47b521` refuses materially negative cash after a
   completed rebalance and after marks/borrow. Its tolerance is precisely
   `32 * double-epsilon * current-positive-NAV`, without clamping, gross-scale
   dilution, or initial-NAV dilution. It is bound into the context hash. The
   fixture also exercises a fully funded buy-before-sale rebalance, which must
   succeed despite transient per-name negative cash.
3. **Signed fitness and the legacy deflation multiplier need a guard.** V2
   produces signed net Sharpe. The old optional `deflate_selection` branch
   multiplies raw by DSR, pulling a negative score toward zero as evidence gets
   worse. Fix `c0430524` explicitly refuses that V2 search combination before
   candidate work. Direct fitness continues to report DSR separately without
   multiplying signed raw. The owning fixture checks refusal/empty admission
   and separately verifies that changing reported trial N leaves direct raw
   unchanged. Default false and the legacy selection path are unaffected.

## Independent reasoning

The bounded core uses fixed decision dollars, strict mark/decision clocks,
decision-time eligibility, delayed execution against actual marked holdings,
and snapshot-d participation caps. Entry costs debit cash immediately and are
attributed to the next realized endpoint alongside marked PnL and calendar-day
short borrow. Short proceeds remain cash. The ledger oracle explicitly prices
the first two cost-bearing rebalances, including the second rebalance's tiny
actual dollar adjustment; it also compares those supplied actual trades with
the existing CostSurface path adapter. Delay-two coverage checks that a later
NAV gain does not resize an already formed target.

The context owns immutable input copies and shared immutable snapshots. The
matching check binds panel storage/content, policy and normalized configuration;
the mine caller additionally binds membership and ReturnGuard. Streaming hash
updates avoid a second population-sized canonical buffer. Checked admissions
cover retained context/snapshots, per-signal outputs and pending-target/weight
scratch, including concurrent execution workers. These are execution payload
bounds, not a claim to bound the whole VM/search/application process RSS.

The output keeps the original calendar with NaN/invalid structural rows and a
contiguous mature realized interval. Role maturity caps prices and labels;
future mutation cannot change the earlier realized prefix. Missing nonzero
trade costs, missing modeled short borrow, guarded held returns and unavailable
held marks all refuse explicitly. The nine core fixtures cover these contracts,
strict clock equality, ID-order/support mismatch and the exact combined budget
boundary. They do not claim observed fills or liquidity calibration.

Fitness uses the same evaluated SignalSet, reads only the mature net interval,
and reports its rule/context/range. Unbound nonempty pools, PoolView, weak-panel
overlays, legacy cost/capacity/turnover overlays, strided fidelity and unsupported
checkpoint/resume paths are refused. Execution errors return an explicit search
failure before generation admission. Calendar NaNs remain in descriptors;
existing behavioral distances use complete-case pairs. Run-local caches retain
one execution context and do not cross recipes. Legacy default dispatch and
arithmetic remain explicit.

The mine consumer validates every train/validation/holdout/blend call, including
empty-family/empty-holdout admission. Inverse orientation reruns the execution
ledger, rather than negating net PnL. Family sketches and Romano-Wolf matrices
use the actual mature count. Full-PnL TrialRegistry records use the realized
offset inside the training calendar, without padding or compressing interior
missing dates. Active candidate and screened-only identities bind the execution
context; the full score additionally binds IC/inference settings. The stage has
an explicit programmatic route only; no CLI loader or cost source is fabricated.

## Qualification limits

Source review and `git diff --check` are the only checks performed by this lane.
Expected focused cohorts are nine independent core cases, five factory/search
consumer cases, and two mine consumer cases; all runtime results remain pending
the root's single combined build and native execution. No scale, performance,
real-data profitability, historical coverage or tradability acceptance is made.

This is the A-04/A-21 execution slice of W2-A4. Residual WLS/HAC/half-life/exposure
objective work remains open. The V2 ledger is a fractional total-return marked
dollar book with modeled costs/borrow, not share/corporate-action replay or a
locate guarantee. Terminal positions remain marked; no forced terminal sale or
closing cost is fabricated. V2 is opt-in, and explicit legacy reproduction stays
available.
