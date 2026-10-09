// P3.3 vector-math BAKEOFF microbench: the transcendental candidates, timed on the
// same arrays, so the throughput table in at-task-14-report.md is measured, not
// asserted. Two candidates survive the decision gate and are timed here:
//
//   Reference        — scalar libm per element (atx::core::norm_cdf = ½·erfc, std::log,
//                      std::exp). This is candidate 1 (the per-lane SVML-backed libm
//                      the American cold loop already calls; see american.cpp:800-809
//                      for the recorded xsimd/SVML negatives that ruled out those
//                      routes).
//   FastDeterministic— detail/vector_math.hpp 4-lane AVX2 (Cody-Waite log/exp +
//                      Chebyshev-Clenshaw Φ), via the vector_math_probe surface. This
//                      is candidate 4, the in-tree winner.
//
// SLEEF (candidates 2/3) is NOT vendored — the report documents the decline: it
// claims parity with SVML (not a speedup over libm), the 4-lane win is already
// captured below, and the boundary kernel that might use it is seed-bound. So the
// only real numbers to collect are Reference vs FastDeterministic; the items/s ratio
// is the "how much does vectorizing Φ/log/exp actually buy" figure the report needs.
//
// items/s = transcendental evaluations per second. The one-shot max-error label
// documents the accuracy the speed is bought at (the bound test asserts it).

#include "simd/math_mode.hpp"
#include "simd/vector_math_probe.hpp"

#include "atx/core/math.hpp"
#include "atx/vol/api/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

constexpr std::size_t kN = 1u << 14; // 16,384 evaluations per timed pass

// A deterministic interior grid (no wings) for each function's natural domain.
std::vector<double> phi_grid() {
  std::vector<double> x(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    // spread over [-5, 5] — the trusted Chebyshev interior.
    x[i] = -5.0 + 10.0 * (static_cast<double>(i % 997) / 997.0);
  }
  return x;
}
std::vector<double> log_grid() {
  std::vector<double> x(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    x[i] = 0.05 + 40.0 * (static_cast<double>(i % 1009) / 1009.0); // F/K-ish ratios
  }
  return x;
}
std::vector<double> exp_grid() {
  std::vector<double> x(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    x[i] = -3.0 + 6.0 * (static_cast<double>(i % 1013) / 1013.0); // -rT / drift range
  }
  return x;
}

// ── Φ ────────────────────────────────────────────────────────────────────────
void BM_Phi_Reference(benchmark::State& state) {
  const std::vector<double> x = phi_grid();
  std::vector<double> out(kN);
  for (auto _ : state) {
    for (std::size_t i = 0; i < kN; ++i) {
      out[i] = atx::core::norm_cdf(x[i]);
    }
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_Phi_FastDeterministic(benchmark::State& state) {
  if (!simd::have_avx2()) {
    state.SkipWithError("no AVX2 — FastDeterministic unavailable");
    return;
  }
  const std::vector<double> x = phi_grid();
  std::vector<double> out(kN);
  for (auto _ : state) {
    simd::fd_norm_cdf_batch(x.data(), out.data(), kN);
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
  double max_abs = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    max_abs = std::max(max_abs, std::abs(out[i] - atx::core::norm_cdf(x[i])));
  }
  state.SetLabel("max_abs_vs_ref=" + std::to_string(max_abs));
}

// ── log ───────────────────────────────────────────────────────────────────────
void BM_Log_Reference(benchmark::State& state) {
  const std::vector<double> x = log_grid();
  std::vector<double> out(kN);
  for (auto _ : state) {
    for (std::size_t i = 0; i < kN; ++i) {
      out[i] = std::log(x[i]);
    }
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_Log_FastDeterministic(benchmark::State& state) {
  if (!simd::have_avx2()) {
    state.SkipWithError("no AVX2 — FastDeterministic unavailable");
    return;
  }
  const std::vector<double> x = log_grid();
  std::vector<double> out(kN);
  for (auto _ : state) {
    simd::fd_log_batch(x.data(), out.data(), kN);
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
  double max_rel = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    const double want = std::log(x[i]);
    max_rel = std::max(max_rel, std::abs(out[i] - want) / std::max(std::abs(want), 1.0));
  }
  state.SetLabel("max_rel_vs_ref=" + std::to_string(max_rel));
}

// ── exp ───────────────────────────────────────────────────────────────────────
void BM_Exp_Reference(benchmark::State& state) {
  const std::vector<double> x = exp_grid();
  std::vector<double> out(kN);
  for (auto _ : state) {
    for (std::size_t i = 0; i < kN; ++i) {
      out[i] = std::exp(x[i]);
    }
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_Exp_FastDeterministic(benchmark::State& state) {
  if (!simd::have_avx2()) {
    state.SkipWithError("no AVX2 — FastDeterministic unavailable");
    return;
  }
  const std::vector<double> x = exp_grid();
  std::vector<double> out(kN);
  for (auto _ : state) {
    simd::fd_exp_batch(x.data(), out.data(), kN);
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
  double max_rel = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    const double want = std::exp(x[i]);
    max_rel = std::max(max_rel, std::abs(out[i] - want) / want);
  }
  state.SetLabel("max_rel_vs_ref=" + std::to_string(max_rel));
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("vecmath/phi/reference", BM_Phi_Reference))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("vecmath/phi/fastdet",
                                            BM_Phi_FastDeterministic))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("vecmath/log/reference", BM_Log_Reference))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("vecmath/log/fastdet",
                                            BM_Log_FastDeterministic))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("vecmath/exp/reference", BM_Exp_Reference))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("vecmath/exp/fastdet",
                                            BM_Exp_FastDeterministic))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
