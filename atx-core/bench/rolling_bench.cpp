// rolling_bench.cpp — micro-benchmarks for atx::core::stats rolling statistics.
//
// Benchmarks:
//   BM_RollingMeanUpdate        — per-update throughput of RollingMean (steady
//                                 state: window full, evict + admit each call).
//   BM_RollingCorrelationUpdate — per-update throughput of RollingCorrelation
//                                 (windowed power-sum maintenance + sqrt).

#include <benchmark/benchmark.h>

#include <atx/core/stats/rolling.hpp>

using atx::f64;
using atx::core::stats::RollingCorrelation;
using atx::core::stats::RollingMean;

// ---------------------------------------------------------------------------
// BM_RollingMeanUpdate
//
// Drives a width-64 rolling mean with a varying stream.  After the first 64
// calls the window stays full, so each iteration exercises the evict-oldest +
// admit-newest fast path (RunningMean remove()/update()).
// ---------------------------------------------------------------------------
static void BM_RollingMeanUpdate(benchmark::State& state) {
    RollingMean<64U> m;
    f64 x = 0.0;
    for (auto _ : state) {
        x += 1.0;
        benchmark::DoNotOptimize(m.update(x));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingMeanUpdate);

// ---------------------------------------------------------------------------
// BM_RollingCorrelationUpdate
//
// Drives a width-64 rolling Pearson correlation.  Each steady-state update
// subtracts the evicted pair, folds in the new pair, and computes a sqrt — the
// dominant cost on this hot path.
// ---------------------------------------------------------------------------
static void BM_RollingCorrelationUpdate(benchmark::State& state) {
    RollingCorrelation<64U> c;
    f64 x = 0.0;
    for (auto _ : state) {
        x += 1.0;
        // A mildly non-linear y keeps the correlation off the degenerate guard.
        const f64 y = 2.0 * x + (x * 0.5);
        benchmark::DoNotOptimize(c.update(x, y));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingCorrelationUpdate);
