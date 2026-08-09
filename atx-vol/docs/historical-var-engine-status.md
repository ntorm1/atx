# Historical VaR engine: goal, implementation, and current state

Status date: 2026-08-08
Scope: `atx-vol` only

## Executive summary

The goal is to replay a fixed terminal option and stock portfolio through
historical `atx-vol` surface snapshots, then calculate historical-simulation
VaR and expected shortfall from the resulting one-period P&L distribution.

An option's requested absolute delta is the projection coordinate. Each
historical base date resolves a new strike at that delta and relative time to
expiry, but the terminal portfolio's signed option quantity is unchanged.
Stock hedge shares are unchanged too. Earlier versions re-sized every option
and stock position on every scenario base date to preserve reference dollar
delta. That replayed a characteristic-target strategy rather than the final
held portfolio and has been removed.

The old cumulative-P&L chart was also methodologically invalid. Historical VaR
scenarios are alternative one-day observations for the same reference book;
their sum is not an equity curve. The report now plots one-day P&L by scenario
and its distribution, with 95% and 99% loss cutoffs.

The production route is now the cross-sectional inverse-delta engine
(`OptionDeltaSolvePolicy::CrossSectionalColdConfirm`, the library default). It
replaces the direct and screened scalar per-option root solves with a small
fixed number of cross-sectional batch passes over each base surface's option
group; see "Contract projection extension" and "Shipped design" below.

The honest performance number is a mixed result, not a clean pass. On the
realistic SP100 fixture, five-repetition/CV-gated measurements give one
citable core-second figure that meets the target and one that misses it: at
four requested workers (`rel`, sse2 Release) the route runs at 0.995
core-seconds per scenario (median 26.357 s, CV 3.68%) — under the 1.0
core-second-per-scenario target at that operating point. The best citable
eight-worker figure is 1.172 core-seconds per scenario (`rel-avx2`,
`/arch:AVX2` Release, median 15.532 s, CV 4.96%), which misses the target.
The gap is structural, not an unexplored algorithmic opportunity: the two
cold mark passes (base and shifted) floor the route's per-scenario work at
roughly 44%, essentially untouched by any solver-side accelerant, and the
wall-time * workers / scenarios metric charges each of the eight requested
threads as a full core on this benchmark host's 4-performance/8-efficiency-core
part — SMT and E-core threads are not full cores, which inflates the
thread-summed accounting past what the single-thread numbers (0.510-0.567
core-s/scenario at one worker) would predict. See "Measured performance" for
the full table and the citable-versus-directional distinction, and "Shipped
design" for the remaining, explicitly speculative levers.

The module is verified, not merely implemented, though verification is not
total. The focused `Var.*` (21/21) and `ContractProjection.*` (17/17) suites
are green on the merged tree, clang-format is clean over every file this
sprint touched, and the SP100 aggregate-vs-retained correctness gate passes
bit-exact at production scale (every parity error counter identically zero).
What remains open: the full `atx_vol_fast`/`atx_vol_slow` label sweeps were
not run this sprint (the user directed focused test groups instead), one
pre-existing failure outside this module's scope remains unresolved and
tracked, and a final whole-branch review has not yet been performed. See
"Validation state" for the complete, ungenerous accounting.

## Economic objective

The input portfolio contains two position types:

- Options identified by underlier, relative time to expiry, absolute delta,
  call/put side, quantity, and multiplier.
- Stock hedges identified by underlier and share quantity.

For a reference option position, the engine records its signed reference
dollar delta:

```text
target_dollar_delta = reference_quantity
                    * contract_multiplier
                    * reference_spot
                    * reference_unit_delta
```

For every independent historical base-to-shifted scenario, the option replay
does the following:

1. Resolve the same relative time to expiry on the base date.
2. Find the concrete strike whose cold-reference American spot delta has the
   requested absolute delta, preserving call/put side.
3. Carry the terminal portfolio's signed quantity into the scenario unchanged:

   ```text
   scenario_units = reference_quantity
   ```

4. Hold that concrete strike, expiry, and unit count over the base-to-shifted
   interval.
5. Price the held contract on both surfaces and calculate:

   ```text
   scenario_pnl = reference_quantity * contract_multiplier
                * (shifted_mark - base_mark)
   ```

Stock hedge shares are also copied unchanged. Their historical dollar delta is
`reference_shares * base_spot`, and their one-period P&L is
`reference_shares * (shifted_spot - base_spot)`.

This answers the intended question: “What would today's portfolio have earned
or lost on every historical adjacent-session market move if its option profile
were projected to that base market and its actual contract/share quantities
were held fixed for the next session?” The historical concrete option strike
changes as part of projection; the exposure does not.

## Implemented components

### Public module

[`include/atx/vol/var.hpp`](../include/atx/vol/var.hpp) defines the reusable API:

- `VarOptionPosition`, `VarStockPosition`, and `VarPosition` describe inputs.
- `resolve_var_sizing` remains a standalone, sign-safe helper for callers that
  intentionally construct dollar-delta-targeted strategy books. The historical
  VaR replay does not use it.
- `PreparedVarPortfolio` normalizes and anchors a portfolio once, then replays
  any number of independent scenarios without retaining borrowed reference
  surfaces.
- `VarLegFrame` and `VarScenarioFrame` expose auditable leg- and scenario-level
  marks, sizing, P&L, status, and definition fingerprints.
- `historical_var_statistics` calculates deterministic nearest-rank VaR and
  inclusive-tail expected shortfall over successful scenario frames.
- `run_historical_var` is the end-to-end `SurfaceDb` entry point. It resolves
  dates, loads snapshots, prepares the reference portfolio, performs the
  replay, applies the configured failure policy, and returns the distribution.

The default public route uses cold-reference strike resolution and cold marks.
Prepared pricing may be used to propose a delta root, but every successful
screened root is cold-confirmed to the configured tolerance and falls back to
the robust cold solver when needed. The library default is now the
cross-sectional route, `OptionDeltaSolvePolicy::CrossSectionalColdConfirm`
(`VarEvaluationConfig::projection_solve_policy`); see "Contract projection
extension" below for its semantics.

### Engine implementation

[`src/var.cpp`](../src/var.cpp) contains:

- Portfolio validation, symbol normalization, reference anchoring, and stable
  fingerprints.
- Hash-based deduplication of identical option definitions. Duplicate lots
  share pricing work while retaining their distinct quantities and exposures.
- Independent option and stock evaluation paths.
- Explicit market and numerical statuses, including
  `ExpiredBeforeShift`, `ProjectionUnavailable`, provenance rejection, missing
  surfaces, and timestamp mismatch.
- An aggregate path for production risk runs and an optional retained-leg path
  for audit and correctness comparison.
- Balanced contiguous date subranges executed by the persistent `atx-vol`
  pricing executor. Dates remain in ascending order and each worker processes a
  contiguous range, in the same broad style as the existing backtest engine.

Scenarios are mathematically independent. Parallel execution does not carry
state or a restruck contract from one date into the next.

### Contract projection extension

[`include/atx/vol/contract_projection.hpp`](../include/atx/vol/contract_projection.hpp)
and [`src/contract_projection.cpp`](../src/contract_projection.cpp) add
`OptionDeltaSolvePolicy`, with three members:

- `Direct` uses the robust requested pricing route directly, one option at a
  time.
- `FastScreenColdConfirm` permits a prepared tier to propose a strike, checks it
  using cold-reference American delta, refines it if necessary, and uses the
  all-cold solver as the fallback.
- `CrossSectionalColdConfirm` is the library default. In the scalar
  `project_option_contract` entry point it behaves exactly like
  `FastScreenColdConfirm` (a batch of one gains nothing); batch consumers such
  as `PreparedVarPortfolio` route it to the cross-sectional group solver
  instead, `solve_american_delta_batch`.

`solve_american_delta_batch` finds, for every row in a same-surface option
group, the strike whose cold American delta matches the requested absolute
delta to `tolerance`. Each row is seeded from a Black-style inverse-delta
candidate refreshed against the base surface's current smile — the same two
smile-refresh iterations the scalar solver uses, so the seed is a function of
today's surface, not a frozen reference-date value. The seeded candidates are
then evaluated together in laned, cold (`QueryExecution::ColdReference`)
`FirstOrder` passes over the AVX2 greek kernels: one safeguarded Newton step
off the closed-form Black delta slope follows the seed pass, and every pass
after that is a secant correction from the previous two points, up to
`kMaxBatchDeltaPasses = 8` total laned passes. A row is accepted once its
residual is within tolerance/2 of the target — half the caller's tolerance, so
the documented laned-vs-scalar kernel gap cannot push an accepted row outside
the scalar solver's cold oracle tolerance — and any row still unconverged after the
laned passes is handed to the robust scalar fallback solver, in ascending
row-id order. The whole path is deterministic: fixed row order,
batch-composition-invariant kernels, and no cross-call state.

This keeps delta-defined contract projection reusable outside the VaR engine.

### Tests and benchmark tooling

- [`tests/var_test.cpp`](../tests/var_test.cpp) covers reference anchoring,
  reusable sizing, delta/TTE preservation, held-contract repricing, stock
  hedges, long/short call/put signs, thread invariance, aggregate-versus-leg
  equivalence, validation and failure statuses, VaR/ES statistics, and an
  end-to-end three-date `SurfaceDb` run.
- [`tests/contract_projection_test.cpp`](../tests/contract_projection_test.cpp)
  verifies that screened delta roots are always cold-confirmed.
- [`bench/var_bench.cpp`](../bench/var_bench.cpp) builds a realistic terminal
  portfolio from the SP100 overlapping dispersion-strangle backtest and times
  prepared replay across 1, 4, 8, and 16 requested workers.
- [`bench/plot_var_scenario_pnl.py`](../bench/plot_var_scenario_pnl.py)
  converts an exported scenario TSV into a one-day P&L timeline and histogram;
  it deliberately does not sum mutually exclusive VaR scenarios.

The new implementation and tests are wired into the existing `atx-vol` CMake
targets; the benchmark is part of `atx-vol-projection-bench` when
`ATX_BUILD_BENCH=ON`.

## Correctness issue found and fixed

An earlier version converted today's delta to today's forward log-moneyness and
then held that log-moneyness through history. That is not the requested replay
semantics. Delta, rather than reference log-moneyness, is the defining
moneyness coordinate.

The error was economically catastrophic for boundary-delta options. In one CAT
example, a reference delta near 0.0186 became a historical base delta near
`3.4e-10`. Preserving dollar delta then required roughly 26.7 million option
units, and the next mark generated about $2.23 billion of artificial P&L. The
aggregate trace ended around +$2.48 billion, which is why the first cumulative
P&L chart was obviously wrong.

The engine restrikes to the requested absolute delta on every base surface and
then values the fixed terminal quantity. The earlier billion-dollar result was
caused by combining boundary deltas with dollar-delta re-sizing; the re-sizing
path no longer exists in historical replay.

Two proposed accelerators were also rejected on evidence:

- Using prepared/fast marks instead of cold marks produced up to 4.8% aggregate
  value error and about $45,759 absolute scenario-P&L error. It is not an
  admitted VaR valuation route.
- The experimental grouped batch-root implementation was economically close to
  the scalar cold oracle, but it was slower than the accepted screened scalar
  route. It has therefore not been retained as the production algorithm.

## SP100 benchmark fixture

The opt-in benchmark uses the SP100 surface database and the terminal checkpoint
of the overlapping 3M/25-delta dispersion-strangle strategy. It converts the
terminal book to VaR positions, then requires a consistent portfolio that can
be replayed over every selected scenario.

| Item | Count |
|---|---:|
| Source terminal option lots | 9,966 |
| Excluded for incomplete underlier coverage | 6,666 |
| Excluded at extreme delta boundaries | 45 |
| Excluded after replayability screening | 360 |
| Retained option positions | 2,895 |
| Retained stock hedges | 33 |
| Total replayed positions | 2,928 |
| Underliers | 33 |
| Adjacent-session scenarios | 106 |
| Dates missing SPY | 18 |
| Skipped multi-session gaps | 16 |
| History breaks | 12 |

The benchmark is therefore a realistic thousands-of-options portfolio, but it
is explicitly a replayable subset of the terminal strategy book. It is not
valid to describe it as replaying all 9,966 source option lots.

The filtered book still records 922 `ProjectionUnavailable` and 162
`ExpiredBeforeShift` failures encountered while determining which source lots
can form the consistent replayable subset. The accepted timed book has complete
scenario frames.

## Measured performance

The canonical numbers are five-repetition, CV-gated measurements of the
cross-sectional route on the i7-1260P (4P+8E) benchmark host, using the
repository's quiet-window protocol: `rel` is the sse2 Release preset,
`rel-avx2` is the `/arch:AVX2` Release preset. A row is citable only when
its coefficient of variation is at most 5%; higher-CV rows reflect ambient
host-load contamination during this sprint and are reported as directional
context only, not as the sprint's performance result. Core-seconds/scenario is
`median_wall_s * workers / 106`.

| Preset | Workers | Median (5-rep) | CV | Core-seconds/scenario | Citable |
|---|---:|---:|---:|---:|---|
| rel (sse2) | 1 | 54.015 s | 7.95% | 0.510 | Directional (CV > 5%) |
| rel (sse2) | 4 | 26.357 s | 3.68% | 0.995 | Citable |
| rel (sse2) | 8 | 14.937 s | 22.97% | 1.127 | Directional (CV > 5%) |
| rel (sse2) | 16 | 13.007 s | 6.98% | — | Directional (CV > 5%) |
| rel-avx2 (/arch:AVX2) | 1 | 55.460 s | 34.77% | — | Directional (CV > 5%) |
| rel-avx2 (/arch:AVX2) | 4 | 23.430 s | 41.32% | 0.884 | Directional (CV > 5%) |
| rel-avx2 (/arch:AVX2) | 8 | 15.532 s | 4.96% | 1.172 | Citable |
| rel-avx2 (/arch:AVX2) | 16 | 16.520 s | 7.94% | — | Directional (CV > 5%) |

Against the target of at most 1.0 core-second/scenario: rel t4 (0.995,
citable) meets it; the best citable eight-worker figure, rel-avx2 t8 (1.172),
misses it. See the executive summary for the structural reason (the cold-mark
floor plus the SMT/E-core thread-accounting convention on this host). Cells
marked "—" are directional rows for which no core-seconds/scenario figure is
reported here; deriving one from a CV > 5% median would manufacture false
precision.

Additional directional data points, kept for context:

- Single-shot, quiet-host samples of the cross-sectional route (not
  five-repetition, therefore not CV-gated): the Task 11 P&L-producing run at
  t8, 13.887 s = 1.048 core-s/scenario; the Task 9 t1 sample, 60.133 s = 0.567
  core-s/scenario.
- A five-repetition attempt at the prior scalar screened/cold-confirm route,
  run for a same-protocol comparison, also failed the CV gate on this host:
  median 25.997 s, CV 17.11% — not citable. No CV-clean scalar-vs-cross-sectional
  comparison exists under the Task 10 protocol; the citable cross-sectional
  numbers above are judged against the 1.0 target, not against the scalar
  route.
- This host's screened/cold-confirm single-shot at sprint start (Task 1) was
  25.464 s at t8 = 1.922 core-s/scenario. The original version of this
  document reported 2.85 core-s/scenario for the same scalar route, captured
  on a slower benchmark host than the one used for this sprint's
  measurements — the two figures are not directly comparable to each other or
  to the table above.

### Historical one-shot measurements (pre-cross-sectional, kept for context)

These are one-shot Release measurements taken earlier in the sprint, on the
benchmark host used for the original version of this document, before the
cross-sectional route existed. They are retained as rejected-route guardrails
and are not directly comparable to the Task 10 table above (different host,
single-shot rather than five-repetition/CV-gated, and the fast-screen row
describes a route that has since been superseded, not rejected).

| Route | 8-worker wall time for 106 scenarios | Scenarios/s | Approx. core-seconds/scenario | Decision |
|---|---:|---:|---:|---|
| Direct cold root + cold marks | 109.127 s | 0.971 | 8.24 | Correct baseline, too slow |
| Fast screen, cold-confirmed root + cold marks | 37.695 s | 2.812 | 2.85 | Superseded; see the Task 10 table above |
| Experimental grouped batch root + cold marks | 57.717 s | 1.837 | 4.36 | Rejected; slower |

At the time, the fast-screen scalar route was about 2.90 times faster than the
direct cold baseline, but still about 2.85 times slower than the one
core-second-per-scenario target. The grouped batch-root experiment wrapped
per-option scalar root solves in a batch loop without reducing the underlying
scalar root cost: every option still ran its own seed-Newton-bracket sequence
with scalar cold delta evaluations, so the batch added orchestration overhead
while retaining nearly all of the original root cost. The cross-sectional
design that replaced it (see "Shipped design" below) is different in kind, not
degree — a small, fixed number of cross-sectional kernel passes over a
shrinking active set replaces per-option scalar iteration entirely.

## Current P&L result and artifacts

The corrected fixed-unit cross-sectional trace is:

```text
C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_fixed_units.tsv
```

It contains 106 alternative adjacent-session observations over 2,928 accepted
positions. The production-scale run proved every successful scenario leg kept
exactly the terminal reference quantity. Aggregate-vs-retained parity counters
were all zero. The loss distribution is:

| Metric | Result |
|---|---:|
| 95% VaR | $415,634.66 |
| 99% VaR | $941,857.72 |
| 99% ES | $950,796.42 |
| Worst one-day P&L | -$959,735.12 |
| Best one-day P&L | +$1,630,680.01 |

The arithmetic sum of all scenario P&Ls is +$6,314,335.32, but that number is
not a return and must not be used as a baseline. The scenarios are mutually
exclusive historical observations for one reference book. Summing them creates
a synthetic repeated-entry strategy and re-arms theta on every observation.

The corrected chart therefore shows one-day P&L and its histogram rather than
a cumulative line:

```text
C:\atx\artifacts\var\sp100_dispersion_ytd_one_day_pnl_fixed_units.png
```

The fixture still reports material pre-replay exclusions: 6,666 source options
lack full-history underlier coverage, 45 are at unsupported delta boundaries,
and 360 fail at least one replay scenario. The result is the fixed held exposure
of the 2,928-position replayable subset, not the full 9,966-lot terminal book.

## Validation state

Focused suites are green on the merged tree: `^Var\.` 21/21, `^ContractProjection\.`
17/17. clang-format is clean over every file this plan touched. The
hygiene-preset build (`configure -Preset hygiene` then `build atx-vol-tests
-Preset hygiene`) completed clean on this revision.

The full `atx_vol_fast` and `atx_vol_slow` label sweeps were not run in
this sprint; the user directed focused test groups (`^Var\.`,
`^ContractProjection\.`) instead of full-suite runs. The last partial
`atx_vol_fast` evidence, taken before that direction, stood at 2639/2676 with
2 failures. Both were triaged:

- `VolUmbrella.TierAIsClosedUnderInclusion` was a pre-existing fork-point
  failure (the `strategy.hpp` -> `swap_leg.hpp` Tier-A closure gap) already
  fixed upstream on `main` by commit `be98049`. It passes on the merged `main`
  today.
- `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`
  fails on both the fork-derived tree and current merged `main`. It is a
  pre-existing convex-dense risk-surface admission rejection in the
  fitting/QP domain, with no overlap with any file this sprint touched. It is
  tracked as an upstream defect outside this module's scope, not resolved by
  this sprint.

Correctness at production scale is bit-exact: the SP100 aggregate-vs-retained
1e-9 gate passes with every error counter identically zero (sse2 build). The
Task 8 fixture defect that once caused a gate failure was in the bench
fixture's oracle, not the engine: a superset-book retained-leg oracle is
invalid under the cross-sectional route's whole-scenario fallback
composition-dependence. The fix was a fresh same-book retained replay (commit
`5630362`), regression-pinned by
`Var.CrossSectionalAggregateMatchesRetainedAfterExclusionRebuild`. The
engine's own gates — cold-confirm at 1e-7 per strike (the solver's internal
batch acceptance tolerance is half that, tolerance/2, for headroom), cold
marks only, bit-exact thread invariance, and scenario independence — are
unchanged.

A final whole-branch review has not yet been performed.

## Shipped design

The design once proposed in this section as future work is now implemented,
tested, and the library default:

1. Options sharing a base surface are grouped and stable-sorted by
   `(uid, expiry_offset_ns)`, so bit-identical time-to-expiry rows land in
   contiguous runs that the laned kernels' per-run T-bracket caching already
   exploits.
2. Every row is seeded from a Black-style inverse-delta candidate refreshed
   against the base surface's current smile (two smile-refresh iterations,
   mirroring the scalar solver) rather than a frozen reference-date value.
3. The seeded candidates are evaluated together in laned, cold `FirstOrder`
   passes; one safeguarded Newton step follows the seed pass, then secant
   correction passes refine the shrinking active set, up to
   `kMaxBatchDeltaPasses = 8` total passes, with internal batch acceptance at
   tolerance/2 so the scalar cold oracle holds at the full tolerance.
4. Any row still unconverged after the laned passes is handed to the robust
   scalar fallback solver — 0.43% of rows on the SP100 fixture, after the
   pass budget was raised from 6 to 8 to bring the fallback fraction under the
   2% guardrail.
5. Base and shifted cold price marks are batched once strikes are final.

This changed the asymptotic constant that mattered: thousands of independent
scalar root searches became a small fixed number of cross-sectional kernel
passes plus a rare scalar fallback tail. The correctness gate did not change:
every accepted strike still satisfies cold American delta tolerance, and
aggregate P&L still matches the retained-leg cold oracle within the admitted
economic error (in production it matches bit-exactly; see "Validation state").

Measured on the SP100 fixture (deterministic counters, t8): delta-solve is
55.8% of thread-summed core-time and the two cold mark passes are 44.1%, with
an average of 3.77 laned passes per row. A laned `FirstOrder` cold
evaluation costs about the same as a `Price` cold evaluation (~36 microseconds
per row) — the batch solver reorganizes cold evaluations rather than
eliminating them, which is why the mark passes are now close to half of total
work and are the dominant remaining floor, not the solve.

Two accelerant ideas were measured and rejected during development, not
merely proposed and skipped, and are kept here as guardrails: a
reference-anchored seed (4.66 laned passes/row, 9.6% scalar fallback —
reference-date moneyness does not transfer across vol regimes, since the
k(delta) map scales with the current smile level) and trimming the seed from
two smile refreshes to one (4.32 passes/row, 5.37% fallback). Both regressed
the pass count against the shipped Black-seed/two-refresh design and are not
retained.

The remaining ideas below are speculative — none has been measured on this
codebase, and none is scheduled:

- Further ISA work (for example AVX-512 laned kernels) to reduce the ~36
  microsecond/row cold-evaluation cost directly, rather than reducing the
  number of evaluations.
- Reducing the fixed mark-pass cost itself (currently ~44% of work and
  untouched by any solver-side accelerant), for example by batching the base
  and shifted marks across scenarios rather than per scenario.
- E-core-aware executor partitioning on hybrid parts. The wall * workers /
  scenario metric currently charges SMT and E-core threads as full cores on
  the 4P+8E benchmark host; the single-thread numbers (0.510-0.567
  core-s/scenario) versus the eight-worker numbers (1.127-1.172) show this
  metric convention accounts for part of the t8 gap independent of any
  algorithmic change.

## Reproduction commands

Build the focused targets:

```powershell
cmake --preset rel -DATX_BUILD_BENCH=ON
cmake --build build-rel --target atx-vol-tests atx-vol-projection-bench -- -j 12
```

Build the `rel-avx2` preset the same way (`cmake --preset rel-avx2 ...` /
`cmake --build build-rel-avx2 ...`) to reproduce the `rel-avx2` rows of the
"Measured performance" table.

Run the accepted cross-sectional route once while iterating:

```powershell
$env:ATX_SP100_SURFACE_DB='C:\atx-scratch\surface-db\sp100-2026'
$env:ATX_VAR_BENCH_SINGLE_SHOT='1'
$env:ATX_VAR_PNL_TSV='C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_fixed_units.tsv'
$env:ATX_VAR_FAILURE_TSV='C:\atx\artifacts\var\sp100_dispersion_ytd_failures_fixed_units.tsv'

.\build-rel\bin\atx-vol-projection-bench.exe `
  --benchmark_filter='^var/prepared/sp100_dispersion_terminal/ytd/thousands/cross_cold/t8/' `
  --benchmark_out='C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_fixed_units.json' `
  --benchmark_out_format=json
```

Generate the chart:

```powershell
python atx-vol\bench\plot_var_scenario_pnl.py `
  C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_fixed_units.tsv `
  C:\atx\artifacts\var\sp100_dispersion_ytd_one_day_pnl_fixed_units.png
```

For a citable result, unset `ATX_VAR_BENCH_SINGLE_SHOT`, use a leased quiet
host, and use the benchmark harness's normal warm-up, five repetitions, and
coefficient-of-variation reporting (CV <= 5% to cite). Run the same filter
against both the `rel` (sse2) and `rel-avx2` (`/arch:AVX2`) presets to
reproduce the "Measured performance" table; do not compare numbers across
presets, across hosts, or against single-shot (`ATX_VAR_BENCH_SINGLE_SHOT=1`)
runs.
