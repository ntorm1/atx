# atx-vol benchmark harness

Google Benchmark suites that gate the atx-vol American-pricing and portfolio
throughput work. Nothing in the performance sprint ships without a measured
before/after through these targets; every claimed speedup is a **ratio** against
a checked-in baseline, verified by `compare_baseline.py`.

Two targets, built only when `ATX_BUILD_BENCH=ON` (the shared, atx-wide option —
Google Benchmark is **not** a hard atx-vol dependency):

| target                    | source                          | what it measures |
|---------------------------|---------------------------------|------------------|
| `atx-vol-american-bench`  | `american_pricing_bench.cpp`    | route x side x API pricer matrix over a moneyness/maturity/vol grid, plus the call-slice batch path |
| `atx-vol-portfolio-bench` | `portfolio_throughput_bench.cpp`| PricedSurface queries, `PortfolioPricer::price`/`pnl_explain`, position-scatter-only, kernel floor |

The old `examples/*.cpp` hand-timed demos are left untouched — they are human
demos, not gates.

## Configure & build

```
cmake --preset rel -DATX_BUILD_BENCH=ON
cmake --build build-rel --target atx-vol-american-bench atx-vol-portfolio-bench -- -j 12
```

The `rel` preset is the canonical **Release, x64-default SSE2** build (clang-cl).
Do **not** add `/arch:AVX2`, `/fp:fast`, or LTO — the baseline is deliberately the
current SSE2 build; that is the whole point of pinning it.

## Run

```
./build-rel/bin/atx-vol-american-bench.exe  --benchmark_out=amer.json --benchmark_out_format=json
./build-rel/bin/atx-vol-portfolio-bench.exe --benchmark_out=port.json --benchmark_out_format=json
```

JSON is the deliverable format (`compare_baseline.py` reads it). Full-matrix wall
time is ~9.5 min (american) and ~8.5 min (portfolio) on the baseline host.
Filter while iterating, e.g. `--benchmark_filter='amer/american_price_cached/.*'`.

Every case carries `->MinWarmUpTime(0.5)` (>= 0.5 s warm-up), `->Repetitions(5)`,
`->ReportAggregatesOnly(false)`, and two custom statistics on top of Google
Benchmark's median/mean/stddev: **`p95`** and **`cv`** (coefficient of variation).

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

## Regenerate a baseline

Baselines live in `bench/baselines/<host>-<compiler>-<isa>.json`. On the pinned
host, rebuild Release and overwrite:

```
cmake --preset rel -DATX_BUILD_BENCH=ON
cmake --build build-rel --target atx-vol-american-bench atx-vol-portfolio-bench -- -j 12
./build-rel/bin/atx-vol-american-bench.exe  --benchmark_out=bench/baselines/i7-1260p-clang18-sse2-american.json  --benchmark_out_format=json
./build-rel/bin/atx-vol-portfolio-bench.exe --benchmark_out=bench/baselines/i7-1260p-clang18-sse2-portfolio.json --benchmark_out_format=json
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
