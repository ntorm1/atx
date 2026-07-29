# atx-vol benchmark harness

Google Benchmark suites that gate the atx-vol American-pricing and portfolio
throughput work. Nothing in the performance sprint ships without a measured
before/after through these targets; every claimed speedup is a **ratio** against
a checked-in baseline, verified by `compare_baseline.py`.

Benchmark targets are built only when `ATX_BUILD_BENCH=ON` (the shared, atx-wide option —
Google Benchmark is **not** a hard atx-vol dependency):

| target                    | source                          | what it measures |
|---------------------------|---------------------------------|------------------|
| `atx-vol-american-bench`  | `american_pricing_bench.cpp`    | route x side x API pricer matrix over a moneyness/maturity/vol grid, plus the call-slice batch path |
| `atx-vol-portfolio-bench` | `portfolio_throughput_bench.cpp`| PricedSurface queries, `PortfolioPricer::price`/`pnl_explain`, position-scatter-only, kernel floor |
| `atx-vol-projection-bench` | `contract_projection_bench.cpp` | scalar and prepared relative-contract projection, definition/mark/full-Greeks output, batch and thread-count scaling |
| `atx-vol-simd-bench` | `simd_*bench.cpp` | scalar-loop vs AVX2 SoA-batch throughput for the b76 value/vega, greeks, IV-invert, eSSVI backbone, and pnl-explain kernels |
| `atx-vol-reloc-bench` | `backtest_throughput_bench.cpp`, `american_greeks_reuse_bench.cpp`, `strangle_solver_bench.cpp` | multi-underlier backtest steps/s, FD-vs-analytic American Greeks, and the SPY strangle solver's per-eval cost breakdown (relocated out of the test suite — they measure timing, not correctness) |
| `atx-vol-fitting-bench` | `fitting_throughput_bench.cpp`, `corpus_build_bench.cpp` | whole-surface and single-slice eSSVI calibration cost (synthetic AND one real-OPRA SPY board), the 16.5k-anchor-comparable American-IV inversion rate, and 20-board corpus build throughput |
| `atx-vol-e2e-hotpath-bench` | `e2e_hotpath_bench.cpp` | canonical real-OPRA fit/model-mark/snapshot path plus cold, representative, and screen-with-cold-confirm SPY backtests |

The old `examples/*.cpp` hand-timed demos are left untouched — they are human
demos, not gates.

## Configure & build

```
cmake --preset rel -DATX_BUILD_BENCH=ON
cmake --build build-rel --target atx-vol-american-bench atx-vol-portfolio-bench atx-vol-projection-bench -- -j 12
```

The `rel` preset is the canonical **Release, x64-default SSE2** build (clang-cl).
Do **not** add `/arch:AVX2`, `/fp:fast`, or LTO — the baseline is deliberately the
current SSE2 build; that is the whole point of pinning it.

## Run

```
./build-rel/bin/atx-vol-american-bench.exe  --benchmark_out=amer.json --benchmark_out_format=json
./build-rel/bin/atx-vol-portfolio-bench.exe --benchmark_out=port.json --benchmark_out_format=json
./build-rel/bin/atx-vol-projection-bench.exe --benchmark_out=projection.json --benchmark_out_format=json
./build-rel/bin/atx-vol-e2e-hotpath-bench.exe --benchmark_out=e2e.json --benchmark_out_format=json
```

JSON is the deliverable format (`compare_baseline.py` reads it). Full-matrix wall
time is ~9.5 min (american) and ~8.5 min (portfolio) on the baseline host.
Filter while iterating, e.g. `--benchmark_filter='amer/american_price_cached/.*'`.

Every case carries `->MinWarmUpTime(0.5)` (>= 0.5 s warm-up), `->Repetitions(5)`,
`->ReportAggregatesOnly(false)`, and two custom statistics on top of Google
Benchmark's median/mean/stddev: **`p95`** and **`cv`** (coefficient of variation).
The two corpus-scale `fit/e2e/*` rows are the deliberate exception: each process
runs exactly one corpus operation, and three separate processes supply the
best-of-3 baseline. Their canonical value stage requests `OutputField::Prices`
(model IV and model price) with production automatic valuation workers. Full
`Prices | Bands` performs three market-IV inversions per quote and belongs only in
an explicit noncanonical diagnostic, not the default accuracy panel or backtest
hot path. The backtest rows retain the
cold-oracle and screen/cold-confirm economic gates and use the common warmup and
repetition policy above.

### Counters (`contracts/s`, `ns/option`, `bytes/s`, `positions/s`, ...)

Each row reports throughput via `benchmark::Counter`. **A `positions/s` figure is
meaningless — and is rejected in review — without its dedup ratio.** The portfolio
rows therefore always emit `unique_contracts_per_s`, `positions_per_s`,
`bytes_per_s`, `n_unique`, `n_positions`, and `dedup_ratio` *together*.

## Algorithm counters (`ATX_VOL_COUNTERS`, P0.2)

An opt-in, compile-time facility (`atx/vol/counters.hpp`) that counts boundary
solves, Newton/fixed-point sweeps, premium-quadrature evals, norm_cdf/log/exp
calls, correction-cache hits/clamps/fallbacks, frame allocations/bytes, and worker
launches. **Default OFF: `ATX_VOL_COUNT(...)` expands to `((void)0)` — zero runtime
cost, zero ABI change** (verified by diffing the preprocessed pricing TU). Turn on
with:

```
cmake --preset rel -DATX_BUILD_BENCH=ON -DATX_VOL_COUNTERS=ON
```

When ON, both bench targets dump the per-operation counts as `cnt_*` columns in
the JSON (measured over a single representative op *outside* the timed loop, so
warm-up does not inflate them).

## Quiet-window bench protocol (M3)

This box is an i7-1260P — a P+E hybrid with no CPU-frequency pinning — so raw
numbers are only citable under a disciplined protocol. Every bench exe that links
`bench_main.cpp` (all of them except `atx-vol-surface-v2-bench` / `atx-vol-cstar-panel`,
which own their `main()`) now enforces it automatically:

1. **P-core pinning.** Before the first pool use `main()` calls
   `configure_pricing_executor(Topology::PerformanceCores)`, landing the pricing
   executor's workers on the performance cores (best-effort on Windows; falls back
   to `Auto` sizing if P-core discovery / the affinity API fails — affinity is a
   latency prior, never a correctness gate). *Scope caveat:* this pins the
   **pricing** executor pool; the fitter's own `parallel_for` pool shares the same
   core budget/env cap but is **not** pinned by this call, so a fit-heavy row
   (`fit/e2e/*`) is only partially covered. Cap fit fan-out with
   `ATX_VOL_FIT_WORKERS` and lease the P-cores to one bench at a time when several
   agents share the box.
2. **Turbo/thermal preamble + warmup.** `main()` prints a stderr preamble (quiesce
   the box; take best-of-N; reject CV>5%) and runs a brief active warmup so the
   first measured case does not eat the cold-clock / pre-turbo transient. Each case
   additionally carries the `apply_common()` `>=0.5 s` `MinWarmUpTime`. Skip the
   warmup with `ATX_BENCH_NO_WARMUP=1`.
3. **Best-of-N + CV≤5% gate.** A run is trustworthy only when its coefficient of
   variation (stddev/mean over the 5 repetitions) is `<= 5%`. Take best-of-N and
   discard/flag any kept run whose row is `NOISY`; `compare_baseline.py` reads the
   same `cv` statistic and never gates a `CV>5%` row. The `Iterations(1)` corpus
   rows (`fit/e2e/*`) carry no in-process CV — supply best-of-3 from three separate
   processes, as before.
4. **Per-ISA baseline naming (enforced).** The exe knows its own build ISA
   (`__AVX2__` ⇒ `avx2`, else `sse2`) and **refuses** a `--benchmark_out` that
   lands under a `baselines/` directory unless the filename carries this build's
   ISA tag (and not the other's). An `avx2` run therefore cannot overwrite an
   `sse2` baseline (or vice-versa). Convention:
   `i7-1260p-clang18-{sse2,avx2}-<bench>.json`.

**Self-check (M3 verification, class `tooling`).** Run any bench exe with
`--atx-self-check` to prove the CV gate rejects a synthetic high-variance sample
and accepts a low-variance one, and that per-ISA naming is enforced (accepts the
this-ISA name, rejects the other-ISA and ISA-less names). It exits `0` on success
without touching Google Benchmark:

```
./build-rel-avx2/bin/atx-vol-e2e-hotpath-bench.exe --atx-self-check
```

## Regenerate a baseline

Baselines live in `bench/baselines/<host>-<compiler>-<isa>.json`. On the pinned
host, rebuild Release and overwrite:

```
cmake --preset rel -DATX_BUILD_BENCH=ON
cmake --build build-rel --target atx-vol-american-bench atx-vol-portfolio-bench -- -j 12
./build-rel/bin/atx-vol-american-bench.exe  --benchmark_out=bench/baselines/i7-1260p-clang18-sse2-american.json  --benchmark_out_format=json
./build-rel/bin/atx-vol-portfolio-bench.exe --benchmark_out=bench/baselines/i7-1260p-clang18-sse2-portfolio.json --benchmark_out_format=json
./build-rel/bin/atx-vol-projection-bench.exe --benchmark_out=bench/baselines/i7-1260p-clang18-sse2-projection.json --benchmark_out_format=json
```

Quiesce the box first (close other load; the reference host has no CPU-frequency
pinning, so a few fast rows are legitimately noisy — see below).

## Compare / gate

```
python bench/compare_baseline.py bench/baselines/i7-1260p-clang18-sse2-american.json amer.json
```

- Matches benchmarks by name, prints `name | baseline median | new median | ratio |
  CV | verdict`.
- **Fails (exit 1) only on a ratio > 1.10 where the new run's CV <= 5%.** A move
  under noise is not signal.
- **`CV` (coefficient of variation) = stddev / mean of the 5 per-repetition times**
  (each repetition's value is itself the mean over that repetition's iterations —
  not the repetitions' medians; Google Benchmark hands the `cv` statistic lambda
  the raw per-repetition means). It is the run's own noise floor. A benchmark
  with `CV > 5%` is printed `NOISY`
  and *never* fails the gate — you cannot trust a 10% regression call when the
  measurement itself swings 15%. Sub-microsecond rows (e.g. the cached price ~4 µs,
  the position scatter) are inherently noisy on an unpinned laptop; treat their
  ratios as advisory and re-run isolated (`--benchmark_filter=...`) if they matter.
- Absolute nanoseconds are pinned to one host, so **only ratios are gated.** The
  script prints both files' host metadata (`host_name`, `num_cpus`, `mhz_per_cpu`,
  `library_build_type`, cache sizes) and raises a loud warning on any mismatch —
  comparing across different silicon or a debug-vs-release build is invalid.

## Baseline host

`i7-1260p-clang18-sse2`: 16 logical CPUs @ 2496 MHz, clang-cl 18 (VS 2022 LLVM),
`library_build_type=release`, x64-default SSE2 (no AVX2 / fp:fast / LTO). The exact
`context` block is embedded in each baseline JSON.

An ISA-fair `rel-avx2` preset also exists (global `/arch:AVX2`, same clang-cl/
sccache/shared-deps toolchain, `binaryDir=build-rel-avx2`) — see CMakePresets.json.
Baselines produced under it are named `i7-1260p-clang18-avx2-<target>.json`
(note `-avx2-` replacing `-sse2-`). The `-avx2-`/`-sse2-` split is no longer a
naming *convention* alone: it is **enforced** by `bench_main.cpp` (M3, above) — an
`avx2` exe refuses to write an `-sse2-` baselines/ file and vice-versa, so the two
ISAs can never clobber each other's baseline.

## Baselines (C0.2: reloc / simd / fitting gaps + real-OPRA fit case)

`bench/baselines/i7-1260p-clang18-sse2-{reloc,simd,fitting}.json` — the three
targets that had no baseline before C0.2 — plus a **real-OPRA** fitting case,
`fit/surface_cold/spy_real` (pinned to `kSpyFitFixtures[0]`,
`SPY_2026-02-12T1435Z.parquet`, registered only when the fixture parquet is
found on disk), inside `i7-1260p-clang18-sse2-fitting.json`. Produced on the
`i7-1260P` host above, Release, x64-default SSE2 (`rel` preset), same machine
as the pre-existing american/portfolio/projection baselines.

**CV policy**: every row checked in has CV <= 5% (single-core cases) or <= 10%
(the one all-core case, `backtest/multiunderlier_straddle/steps`, which uses
`->UseRealTime()` and fans out over all cores by design). `fit/surface_cold/
spy_real` was run with `--benchmark_min_time=2s` (each cold surface fit is
tens of ms, so the default budget under-samples it) — pass the same flag when
regenerating.

**Machine was NOT quiescent for most of this measurement session** — a
concurrent Debug `atx-vol-tests.exe` run (and, briefly, another worktree's
Release compile) from a different Claude Code agent shared the same physical
box for long stretches, and even confirmed process-level quiescence
(`ninja`/`cl`/`clang-cl`/test-exe all absent) did not guarantee a clean run:
the i7-1260P's hybrid P/E-core scheduling and lack of frequency pinning meant
consecutive "quiet" runs still swung by 2-4x in places. Each of the three exes
was run repeatedly (4-6 full runs, plus a few `--benchmark_filter`-narrowed
retries for the slow `spy_real` case) and **only the rows whose CV held <=5%
on their best run, AND which did not produce a false REGRESS on a subsequent
independent self-gate rerun, were kept.** Everything else was excluded
(ungated) rather than checked in noisy — see the row lists below. Re-running
`compare_baseline.py` for any of the three files against a fresh rerun on this
machine should therefore be expected to print `NOISY` (never `REGRESS`) for
every gated row; that is the honest state of this host during this session,
not a defect in the rows themselves (each was independently confirmed
REGRESS-free against two separate fresh reruns before being checked in).

Rows checked in (gated, CV shown from the run that was kept):

| baseline | run_name | CV |
|---|---|---|
| reloc | `backtest/multiunderlier_straddle/steps` (all-core) | 3.38% |
| reloc | `strangle/eval/greeks` | 4.35% |
| reloc | `american_greeks/fd_warm` | 2.92% |
| simd | `simd/pnl_explain/avx2` | 2.40% |
| fitting | `corpus/build_20boards` | 2.86% |
| fitting | `fit/surface_cold/spy_synth` | 1.93% |
| fitting | `fit/surface_cold/spy_real` | 3.17% |

Rows excluded (ungated/noisy — CV never held <=5% [<=10% all-core] across
repeated retries on this machine, or (simd `scalar_autovec`) produced a
false REGRESS on a self-gate rerun despite a clean capture CV; re-measure on
a quieter box before relying on these):

- reloc: `american_greeks/fd_ref`, `american_greeks/fd_fast`,
  `american_greeks/andersen_lake`, `strangle/eval/delta`,
  `strangle/eval/resolve`, `strangle/eval/vega`
- simd: `simd/pnl_explain/scalar_novec`, `simd/pnl_explain/scalar_autovec`,
  `simd/b76_value_vega/{scalar,avx2}`, `simd/b76_greeks/{scalar,avx2}`,
  `simd/essvi_backbone/{scalar,avx2}`, `simd/iv_invert/{scalar,avx2}`
- fitting: `fit/slice_cold/spy_synth`, `fit/slice_warm_refit/spy_synth`,
  `fit/american_iv/{cold,warm}`

### QUARANTINE: `port/price/greeks/u2688/*` (portfolio baseline)

`i7-1260p-clang18-sse2-portfolio.json` has had its `port/price/greeks/u2688/*`
scaling-row family (16 run_names: `r{1,10,100,1000}/t{1,2,4,8}`) **removed**.
Those rows measured median 23.1 s @ CV 40% for `r1/t1` alone — 11x the
row's own kernel floor (`port/floor/greeks/u2688` = 2.03 s) — physically
impossible, and `compare_baseline.py` refuses to gate CV>5% rows anyway, so
the family was ungated in practice already. The `port/floor/*` rows and every
other `u2688` family (`prices_only`, `analytic`, `pnl_explain`,
`scatter_only`) are untouched. The re-take is owned by the 07-11 sprint's
C0.1 task, after a backtest rewiring; until then `port/price/greeks/u2688/*`
has no baseline claim.

## Backtest hot-path throughput sprint (WS-M: M1/M2/M3)

Three measurement targets landed for the backtest hot-path sprint (the loop
**fit -> serialize -> deserialize -> price/greeks**). All three are captured under
the quiet-window protocol on `rel-avx2` and named `-avx2-` (enforced).

- **M1 — `atx-vol-surface-archive-bench`** (`surface_archive_bench.cpp`): the
  previously-missing serialize/deserialize measurement. ATXVSA2-only since the v1
  format was deleted (release-v1 plan 3.5/3.6); the v1 rows (`serialize`,
  `open_reconstruct_all`, `reconstruct_all`, `reconstruct_one`) are gone with it, so
  the v1 numbers in the checked-in baselines below are HISTORY, not a comparand.
  - `surface_archive/serialize_v2/{essvi,convexdense}/count:{1,4,16,50,100}` —
    `write_surface_archive_v2` in memory; `items_per_second` == surfaces/s (µs/surface
    = 1e6 / surfaces_per_s), `bytes_per_second` == partition write MB/s, and
    `bytes_per_surface` the per-surface payload weight.
  - `surface_archive/deserialize/<payload>/<mode>/count:N` — five modes. Three rebuild
    OWNED surfaces: `open_reconstruct_all_v2` (open + `reconstruct_all_with_provenance`,
    the whole-board "bytes -> ready-to-price" the backtest pays every step),
    `reconstruct_all_v2` (the same on a pre-opened archive, isolating reconstruct from
    open), and `reconstruct_one_v2` (`reconstruct_symbol` on one symbol). Two build
    zero-copy views over the same bytes: `mmap_open` (open + `map_all`) and
    `subset_map_zero_copy` (`map_symbol`), so owned-vs-view is a same-run ratio.
  - Baseline: `i7-1260p-clang18-avx2-surface-archive.json`.
- **M2 — universe case in `atx-vol-reloc-bench`** (`backtest_throughput_bench.cpp`):
  `backtest/universe_strangle_hedged/steps` — kUnivN=10 names × daily 40Δ Strangle
  entry × held-to-expiry (overlapping cohorts) × daily DeltaToZero hedge (the WS-D
  strategy shape), synthetic surfaces. Headline `steps/s` (items/s) + `final_open_lots`
  book size. The pre-existing `backtest/multiunderlier_straddle/steps` stays as the
  ATM straddle reference. Baseline: `i7-1260p-clang18-avx2-backtest-throughput.json`.
- **M3 — attribution row in `atx-vol-e2e-hotpath-bench`** (`e2e_hotpath_bench.cpp`):
  `attribution/pipeline/synth_fit_ser_deser_price` — a self-contained synthetic
  fit -> serialize -> deserialize -> price pass whose four stage boundaries are timed
  harness-side via `counters::timing::ScopedStageTimer` (no foreign TU instrumented).
  Publishes `{fit,serialize,deserialize,price}_ms` + `_frac` so effort lands on the
  real critical path. Baseline: `i7-1260p-clang18-avx2-e2e-attribution.json`.

Regenerate (pinned host, quiet box, `rel-avx2`, fit fan-out capped):

```
cmake --preset rel-avx2 -DATX_BUILD_BENCH=ON -DFETCHCONTENT_BASE_DIR=C:/atx-wt/<wt>/deps/rel-avx2
cmake --build build-rel-avx2 --target atx-vol-surface-archive-bench atx-vol-reloc-bench atx-vol-e2e-hotpath-bench
$env:ATX_VOL_FIT_WORKERS=1
./build-rel-avx2/bin/atx-vol-surface-archive-bench.exe --benchmark_out=bench/baselines/i7-1260p-clang18-avx2-surface-archive.json --benchmark_out_format=json
./build-rel-avx2/bin/atx-vol-reloc-bench.exe --benchmark_filter="backtest/" --benchmark_out=bench/baselines/i7-1260p-clang18-avx2-backtest-throughput.json --benchmark_out_format=json
./build-rel-avx2/bin/atx-vol-e2e-hotpath-bench.exe --benchmark_filter="attribution" --benchmark_out=bench/baselines/i7-1260p-clang18-avx2-e2e-attribution.json --benchmark_out_format=json
```
