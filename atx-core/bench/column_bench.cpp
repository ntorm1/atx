// column_bench.cpp — micro-benchmarks for atx::core::series::Column<T>.
//
// Benchmarks:
//   BM_ColumnAppend   — append N elements into a fresh column (growth path).
//   BM_ColumnBulkSum  — sum a large pre-filled column via atx::core::simd::sum.

#include <span>

#include <benchmark/benchmark.h>

#include <atx/core/series/column.hpp>
#include <atx/core/simd.hpp>
#include <atx/core/types.hpp>

namespace simd = atx::core::simd;
using atx::core::series::Column;
using atx::f64;
using atx::usize;

// ---------------------------------------------------------------------------
// BM_ColumnAppend
//
// Measures the cost of appending `range` elements into an initially-empty
// column. Exercises the doubling-growth path (aligned realloc + memcpy) plus
// the per-element store and validity push.
// ---------------------------------------------------------------------------
static void BM_ColumnAppend(benchmark::State& state) {
    const usize n = static_cast<usize>(state.range(0));
    for (auto _ : state) {
        Column<f64> c;
        for (usize i = 0U; i < n; ++i) {
            c.append(static_cast<f64>(i));
        }
        benchmark::DoNotOptimize(c.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
}
BENCHMARK(BM_ColumnAppend)->Range(1 << 8, 1 << 16);

// ---------------------------------------------------------------------------
// BM_ColumnBulkSum
//
// Measures a SIMD horizontal sum over a large, pre-filled column. data() is
// cache-line aligned, so this models the hot reduction path in column stats.
// ---------------------------------------------------------------------------
static void BM_ColumnBulkSum(benchmark::State& state) {
    const usize n = static_cast<usize>(state.range(0));
    Column<f64> c;
    c.reserve(n);
    for (usize i = 0U; i < n; ++i) {
        c.append(static_cast<f64>(i % 17U) * 0.5);
    }
    for (auto _ : state) {
        // Pass the rvalue directly: the const-lvalue DoNotOptimize overload is
        // deprecated (can permit undesired optimizations); the rvalue/by-value
        // overload is the supported form.
        benchmark::DoNotOptimize(simd::sum(c.view()));
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
}
BENCHMARK(BM_ColumnBulkSum)->Range(1 << 10, 1 << 18);
