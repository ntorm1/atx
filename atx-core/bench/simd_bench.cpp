// simd_bench.cpp — micro-benchmarks for atx::core::simd vectorized reductions.
//
// Benchmarks (vectorized vs naive scalar baseline, swept over span length):
//   BM_SimdSum      — xsimd horizontal sum.
//   BM_ScalarSum    — naive left-fold sum baseline.
//   BM_SimdDot      — xsimd inner product.
//   BM_ScalarDot    — naive scalar inner product baseline.

#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include <atx/core/simd.hpp>
#include <atx/core/types.hpp>

namespace simd = atx::core::simd;
using atx::f64;
using atx::usize;

namespace {

// Deterministic, allocation-free-at-steady-state input buffer.
[[nodiscard]] std::vector<f64> make_ramp(usize n) {
    std::vector<f64> v(n);
    for (usize i = 0U; i < n; ++i) { v[i] = static_cast<f64>(i % 17U) * 0.5; }
    return v;
}

[[nodiscard]] f64 scalar_sum(std::span<const f64> x) {
    f64 acc = 0.0;
    for (const f64 v : x) { acc += v; }
    return acc;
}

[[nodiscard]] f64 scalar_dot(std::span<const f64> a, std::span<const f64> b) {
    f64 acc = 0.0;
    for (usize i = 0U; i < a.size(); ++i) { acc += a[i] * b[i]; }
    return acc;
}

constexpr usize kMinN = 1U << 6;  // 64
constexpr usize kMaxN = 1U << 16; // 65536

} // namespace

// ---------------------------------------------------------------------------
// BM_SimdSum — vectorized horizontal sum over a length-N span.
// ---------------------------------------------------------------------------
static void BM_SimdSum(benchmark::State& state) {
    const auto             v = make_ramp(static_cast<usize>(state.range(0)));
    const std::span<const f64> s(v);
    for (auto _ : state) {
        benchmark::DoNotOptimize(simd::sum<f64>(s));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SimdSum)->Range(kMinN, kMaxN);

// ---------------------------------------------------------------------------
// BM_ScalarSum — naive left-fold baseline for comparison.
// ---------------------------------------------------------------------------
static void BM_ScalarSum(benchmark::State& state) {
    const auto             v = make_ramp(static_cast<usize>(state.range(0)));
    const std::span<const f64> s(v);
    for (auto _ : state) {
        benchmark::DoNotOptimize(scalar_sum(s));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ScalarSum)->Range(kMinN, kMaxN);

// ---------------------------------------------------------------------------
// BM_SimdDot — vectorized inner product over two length-N spans.
// ---------------------------------------------------------------------------
static void BM_SimdDot(benchmark::State& state) {
    const auto             a = make_ramp(static_cast<usize>(state.range(0)));
    const auto             b = make_ramp(static_cast<usize>(state.range(0)));
    const std::span<const f64> sa(a);
    const std::span<const f64> sb(b);
    for (auto _ : state) {
        benchmark::DoNotOptimize(simd::dot<f64>(sa, sb));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SimdDot)->Range(kMinN, kMaxN);

// ---------------------------------------------------------------------------
// BM_ScalarDot — naive scalar inner product baseline.
// ---------------------------------------------------------------------------
static void BM_ScalarDot(benchmark::State& state) {
    const auto             a = make_ramp(static_cast<usize>(state.range(0)));
    const auto             b = make_ramp(static_cast<usize>(state.range(0)));
    const std::span<const f64> sa(a);
    const std::span<const f64> sb(b);
    for (auto _ : state) {
        benchmark::DoNotOptimize(scalar_dot(sa, sb));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ScalarDot)->Range(kMinN, kMaxN);
