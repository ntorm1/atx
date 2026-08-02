# Historical VaR engine: goal, implementation, and current state

Status date: 2026-08-02  
Scope: `atx-vol` only

## Executive summary

The goal is to replay today's option and stock portfolio through historical
`atx-vol` surface snapshots while preserving the portfolio's economic
characteristics, then calculate historical-simulation VaR and expected
shortfall from the resulting one-period P&L distribution.

The reusable engine and SP100 dispersion benchmark are implemented in the
working tree. The important economic correction is also implemented: an
option's requested absolute delta is the moneyness coordinate. Each historical
base date therefore resolves a new strike at that same delta and relative time
to expiry. The reference-date forward log-moneyness is retained only for audit;
it is not replayed as the historical strike coordinate.

The current implementation is a correctness baseline plus a first performance
pass, not the final performance result. On the realistic SP100 fixture, the
accepted screened/cold-confirm route processes 106 scenarios in 37.695 seconds
with eight requested workers. That is about 2.85 core-seconds per scenario,
assuming all eight workers are occupied, versus the target of at most 1
core-second per scenario. A grouped batch-root experiment was slower and has
been rejected. The next optimization needs to replace per-option scalar root
solves with a genuinely cross-sectional inverse-delta algorithm.

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
3. Resize the option units so that the historical base position has the same
   signed dollar delta as the reference position:

   ```text
   scenario_units = target_dollar_delta
                  / (base_spot * base_unit_delta * contract_multiplier)
   ```

4. Hold that concrete strike, expiry, and unit count over the base-to-shifted
   interval.
5. Price the held contract on both surfaces and calculate:

   ```text
   scenario_pnl = scenario_units * contract_multiplier
                * (shifted_mark - base_mark)
   ```

Stock hedges use the same dollar-delta sizing boundary. A stock's unit delta is
one, shares are resolved on the historical base date, and those shares are held
through the interval.

This answers the intended question: “What would today's portfolio have earned
or lost on every historical adjacent-session market move if its delta
moneyness, relative expiry, and reference dollar-delta exposures had been held
constant?” It does not claim that the historical concrete strike or original
share count is the same as today's.

## Implemented components

### Public module

[`include/atx/vol/var.hpp`](../include/atx/vol/var.hpp) defines the reusable API:

- `VarOptionPosition`, `VarStockPosition`, and `VarPosition` describe inputs.
- `resolve_var_sizing` is the shared, sign-safe dollar-delta sizing boundary.
  It rejects invalid or overflowing inputs and never silently clamps exposure.
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
the robust cold solver when needed.

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
`OptionDeltaSolvePolicy`:

- `Direct` uses the robust requested pricing route directly.
- `FastScreenColdConfirm` permits a prepared tier to propose a strike, checks it
  using cold-reference American delta, refines it if necessary, and uses the
  all-cold solver as the fallback.

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
- [`bench/plot_var_cumulative_pnl.py`](../bench/plot_var_cumulative_pnl.py)
  converts an exported scenario TSV into a cumulative-P&L PNG, retaining visible
  breaks where the historical series is not adjacent.

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

The corrected engine now restrikes to the requested absolute delta on every
base surface before sizing. The accepted SP100 trace ends around +$6.55 million,
not billions.

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
| Excluded after replayability screening | 351 |
| Retained option positions | 2,904 |
| Retained stock hedges | 33 |
| Total replayed positions | 2,937 |
| Underliers | 33 |
| Adjacent-session scenarios | 106 |
| Dates missing SPY | 18 |
| Skipped multi-session gaps | 16 |
| Visible chart breaks | 12 |

The benchmark is therefore a realistic thousands-of-options portfolio, but it
is explicitly a replayable subset of the terminal strategy book. It is not
valid to describe it as replaying all 9,966 source option lots.

The filtered book still records 922 `ProjectionUnavailable` and 162
`ExpiredBeforeShift` failures encountered while determining which source lots
can form the consistent replayable subset. The accepted timed book has complete
scenario frames.

## Measured performance

These are one-shot Release measurements on the current benchmark host. They are
useful directional measurements, not the final five-repetition/CV-gated
baseline.

| Route | 8-worker wall time for 106 scenarios | Scenarios/s | Approx. core-seconds/scenario | Decision |
|---|---:|---:|---:|---|
| Direct cold root + cold marks | 109.127 s | 0.971 | 8.24 | Correct baseline, too slow |
| Fast screen, cold-confirmed root + cold marks | 37.695 s | 2.812 | 2.85 | Current accepted route |
| Experimental grouped batch root + cold marks | 57.717 s | 1.837 | 4.36 | Rejected; slower |

The accepted route is about 2.90 times faster than the direct cold baseline, but
it is still about 2.85 times slower than the target of one core-second per
scenario. In eight-worker wall-clock terms, that target is approximately 13.25
seconds for 106 scenarios if scaling is ideal.

The fundamental remaining cost is per-option scalar inverse-delta work. Merely
wrapping those scalar roots in a grouped loop does not solve the problem; the
rejected experiment demonstrated that it can add batch passes while retaining
most of the original root cost.

## Current P&L result and artifacts

The accepted screened/cold trace is:

```text
C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_screened.tsv
```

Its final cumulative P&L is approximately $6,546,716. The existing chart is:

```text
C:\atx\artifacts\var\sp100_dispersion_ytd_cumulative_pnl.png
```

That PNG was generated from the corrected cold-reference trace. It has the same
corrected economics, but it should be regenerated from the accepted
`*_screened.tsv` trace after the final code/benchmark cleanup so the delivered
chart and final named trace have identical provenance.

Other useful evidence files are:

```text
C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_screened.json
C:\atx\artifacts\var\sp100_dispersion_ytd_failures_screened.tsv
C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_corrected.tsv
C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_corrected.json
```

Files named `*_final.*` currently contain results from the rejected grouped
batch-root experiment and should not be cited as the accepted result.

## Validation state

The focused VaR and contract-projection tests passed before the latest removal
of the rejected batch-root implementation. Strict source checks also passed at
that point. The cleanup is present in the working tree, but the following final
gates have not yet been rerun after that cleanup:

- Focused `Var.*` and `ContractProjection.*` tests.
- C++ formatting and strict source checks over every touched file.
- PCH-off compile hygiene required by the C++ agent instructions.
- The full `atx-vol` test suite, intentionally deferred until the end.
- A stable repeated benchmark with the repository's CV gate.
- Regeneration and visual inspection of the PNG from the accepted screened TSV.

The module should therefore be treated as implemented but still in active
verification and optimization, not as a completed or committed production
feature.

## Recommended next performance design

The next attempt should eliminate the scalar root solve from the common case:

1. Group unique options by base surface/underlier.
2. Generate a vector of initial strikes directly from the existing Black-style
   inverse-delta seed for all maturities and sides.
3. Evaluate cold American deltas for the whole vector in one batch pass.
4. Apply a vectorized Newton or secant correction and perform one or two further
   batch delta passes.
5. Cold-confirm every candidate against the configured tolerance.
6. Send only the small unconverged tail to the robust scalar solver.
7. Batch the base and shifted cold price marks once strikes are final.

This changes the asymptotic constant that matters: thousands of independent
scalar root searches become a small fixed number of cross-sectional kernel
passes plus rare scalar fallbacks. The correctness gate remains unchanged:
every accepted strike must satisfy cold American delta tolerance, and aggregate
P&L must match the retained-leg cold oracle within the admitted economic error.

Performance should then be measured separately at 1, 4, 8, and 16 workers. The
one-worker result is essential because it directly tests the one-second-per-day
per-core target; thread scaling cannot compensate for a slow single-scenario
algorithm.

## Reproduction commands

Build the focused targets:

```powershell
cmake --preset rel -DATX_BUILD_BENCH=ON
cmake --build build-rel --target atx-vol-tests atx-vol-projection-bench -- -j 12
```

Run the realistic accepted route once while iterating:

```powershell
$env:ATX_SP100_SURFACE_DB='C:\atx-scratch\surface-db\sp100-2026'
$env:ATX_VAR_BENCH_SINGLE_SHOT='1'
$env:ATX_VAR_PNL_TSV='C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_screened.tsv'
$env:ATX_VAR_FAILURE_TSV='C:\atx\artifacts\var\sp100_dispersion_ytd_failures_screened.tsv'

.\build-rel\bin\atx-vol-projection-bench.exe `
  --benchmark_filter='^var/prepared/sp100_dispersion_terminal/ytd/thousands/screened_cold/t8/' `
  --benchmark_out='C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_screened.json' `
  --benchmark_out_format=json
```

Generate the chart:

```powershell
python atx-vol\bench\plot_var_cumulative_pnl.py `
  C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_screened.tsv `
  C:\atx\artifacts\var\sp100_dispersion_ytd_cumulative_pnl.png
```

For a citable result, unset `ATX_VAR_BENCH_SINGLE_SHOT` and use the benchmark
harness's normal warm-up, five repetitions, p95, and coefficient-of-variation
reporting.
