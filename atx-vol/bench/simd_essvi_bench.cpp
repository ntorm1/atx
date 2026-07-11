// Throughput proof for the vectorized eSSVI backbone batch evaluator: scalar
// per-strike loop vs the AVX2 SoA batch, on one representative calibration slice
// swept across a dense log-moneyness grid (the fit inner loop).
//
// Both cases evaluate the SAME strike array; the scalar case calls the
// per-strike essvi_backbone_w in a loop (the pre-vectorization fit hot path),
// the SIMD case calls simd::essvi_backbone_w_batch (4-lane AVX2 on this host).
// The items/s (strikes evaluated per second) and ns/strike that Google Benchmark
// derives are the speedup evidence. A one-shot max-abs cross-check is surfaced as
// a label so the run also documents that the two agree.

#include "atx/vol/simd/essvi_batch.hpp"

#include "atx/vol/simd/cpu.hpp"
#include "atx/vol/vol_surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

constexpr std::size_t kN = 1u << 14; // 16,384 strikes

// A dense, symmetric-backbone slice (the common calibration regime, where the
// vector kernel does its full work — the asymmetric blend only slows the SIMD
// case via scalar fallback, so excluding it is the conservative comparison).
EssviParams make_slice() {
  EssviParams s;
  s.theta = 0.045;
  s.phi = 1.6;
  s.rho = -0.32;
  s.T = 0.35;
  s.F = 100.0;
  return s;
}

// A deterministic log-moneyness grid across [-1, 1].
std::vector<double> make_k(std::size_t n) {
  std::vector<double> k(n);
  for (std::size_t i = 0; i < n; ++i) {
    k[i] = -1.0 + 2.0 * (static_cast<double>(i) / static_cast<double>(n - 1));
  }
  return k;
}

void BM_EssviBackbone_Scalar(benchmark::State& state) {
  const EssviParams s = make_slice();
  const std::vector<double> k = make_k(kN);
  std::vector<double> w(kN);
  for (auto _ : state) {
    for (std::size_t i = 0; i < kN; ++i) {
      w[i] = essvi_backbone_w(s, k[i]);
    }
    benchmark::DoNotOptimize(w.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_EssviBackbone_Avx2(benchmark::State& state) {
  const EssviParams s = make_slice();
  const std::vector<double> k = make_k(kN);
  std::vector<double> w(kN);
  for (auto _ : state) {
    simd::essvi_backbone_w_batch(s, k.data(), w.data(), kN);
    benchmark::DoNotOptimize(w.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));

  // One-shot parity cross-check, surfaced as a benchmark label.
  double max_abs = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    max_abs = std::max(max_abs, std::abs(w[i] - essvi_backbone_w(s, k[i])));
  }
  state.SetLabel("avx2=" + std::to_string(simd::have_avx2() ? 1 : 0) +
                 " max_abs_vs_scalar=" + std::to_string(max_abs));
}

// ── Fused w + natural gradient (the LM residual/Jacobian build) ────────────
//
// The residual+Jacobian pass needs w AND {∂w/∂θ, ∂w/∂φ, ∂w/∂ρ} at every quote
// strike. The scalar case is the pre-vectorization hot path: essvi_backbone_w
// THEN essvi_w_grad3 per strike (each rebuilding the shared backbone tree). The
// AVX2 case is the fused kernel that shares that tree across w and all three
// partials in one pass.

void BM_EssviWGrad_Scalar(benchmark::State& state) {
  const EssviParams s = make_slice();
  const std::vector<double> k = make_k(kN);
  std::vector<double> w(kN), dth(kN), dphi(kN), drho(kN);
  for (auto _ : state) {
    for (std::size_t i = 0; i < kN; ++i) {
      w[i] = essvi_backbone_w(s, k[i]);
      const std::array<double, 3> g = essvi_w_grad3(s, k[i]);
      dth[i] = g[0];
      dphi[i] = g[1];
      drho[i] = g[2];
    }
    benchmark::DoNotOptimize(w.data());
    benchmark::DoNotOptimize(dth.data());
    benchmark::DoNotOptimize(dphi.data());
    benchmark::DoNotOptimize(drho.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_EssviWGrad_Avx2(benchmark::State& state) {
  const EssviParams s = make_slice();
  const std::vector<double> k = make_k(kN);
  std::vector<double> w(kN), dth(kN), dphi(kN), drho(kN);
  for (auto _ : state) {
    simd::essvi_backbone_w_grad_batch(s, k.data(), w.data(), dth.data(),
                                      dphi.data(), drho.data(), kN);
    benchmark::DoNotOptimize(w.data());
    benchmark::DoNotOptimize(dth.data());
    benchmark::DoNotOptimize(dphi.data());
    benchmark::DoNotOptimize(drho.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));

  // One-shot parity cross-check (w + all three partials), surfaced as a label.
  double max_abs = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    max_abs = std::max(max_abs, std::abs(w[i] - essvi_backbone_w(s, k[i])));
    const std::array<double, 3> g = essvi_w_grad3(s, k[i]);
    max_abs = std::max(max_abs, std::abs(dth[i] - g[0]));
    max_abs = std::max(max_abs, std::abs(dphi[i] - g[1]));
    max_abs = std::max(max_abs, std::abs(drho[i] - g[2]));
  }
  state.SetLabel("avx2=" + std::to_string(simd::have_avx2() ? 1 : 0) +
                 " max_abs_vs_scalar=" + std::to_string(max_abs));
}

// Register with the mandated common knobs (warm-up, 5 reps, p95 + CV).
const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("simd/essvi_backbone/scalar",
                                            BM_EssviBackbone_Scalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("simd/essvi_backbone/avx2",
                                            BM_EssviBackbone_Avx2))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("simd/essvi_w_grad/scalar",
                                            BM_EssviWGrad_Scalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("simd/essvi_w_grad/avx2",
                                            BM_EssviWGrad_Avx2))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
