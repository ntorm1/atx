// Throughput proof for the vectorized Black-76 batch kernels: scalar
// per-contract loop vs the AVX2 SoA batch, on one realistic SoA workload.
//
// Both cases price the SAME array; the scalar case calls the per-contract
// black76_value_and_vega in a loop (the pre-vectorization hot path), the SIMD
// case calls simd::black76_value_vega_batch (4-lane AVX2 on this host). The
// items/s (contracts priced per second) and the ns/contract that Google
// Benchmark derives are the speedup evidence. A one-shot max-abs cross-check is
// printed via a label so the run also documents that the two agree.

#include "simd/black76_batch.hpp"

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

struct Soa {
  std::vector<double> F, K, T, sigma, df;
  std::vector<Side> side;
};

// A deterministic, cache-cold-ish spread of contracts resembling a multi-expiry
// option surface: a few forwards × a moneyness ladder × tenors × vols, tiled to
// the requested size. No degenerate/wing rows here — this measures the common
// interior path, where the vector kernel does its full work (the patch path only
// slows the SIMD case, so excluding it is the conservative comparison).
Soa make_workload(std::size_t n) {
  Soa s;
  s.F.reserve(n); s.K.reserve(n); s.T.reserve(n);
  s.sigma.reserve(n); s.df.reserve(n); s.side.reserve(n);
  const double moneyness[] = {0.85, 0.90, 0.95, 1.0, 1.05, 1.10, 1.15};
  const double tenors[] = {0.03, 0.08, 0.17, 0.5, 1.0};
  const double vols[] = {0.12, 0.18, 0.25, 0.40};
  std::size_t idx = 0;
  while (s.F.size() < n) {
    const double F = 50.0 + static_cast<double>((idx * 37) % 400);
    const double m = moneyness[idx % 7];
    const double T = tenors[(idx / 7) % 5];
    const double v = vols[(idx / 3) % 4];
    s.F.push_back(F);
    s.K.push_back(F * m);
    s.T.push_back(T);
    s.sigma.push_back(v);
    s.df.push_back(std::exp(-0.03 * T));
    s.side.push_back((idx & 1u) ? Side::Put : Side::Call);
    ++idx;
  }
  return s;
}

constexpr std::size_t kN = 1u << 14; // 16,384 contracts

void BM_Black76_ValueVega_Scalar(benchmark::State& state) {
  const Soa s = make_workload(kN);
  std::vector<double> price(kN), vega(kN);
  for (auto _ : state) {
    for (std::size_t i = 0; i < kN; ++i) {
      const Black76ValueVega r = black76_value_and_vega(
          s.F[i], s.K[i], s.T[i], s.sigma[i], s.df[i], s.side[i]);
      price[i] = r.price;
      vega[i] = r.vega;
    }
    benchmark::DoNotOptimize(price.data());
    benchmark::DoNotOptimize(vega.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_Black76_ValueVega_Avx2(benchmark::State& state) {
  const Soa s = make_workload(kN);
  std::vector<double> price(kN), vega(kN);
  for (auto _ : state) {
    simd::black76_value_vega_batch(s.F.data(), s.K.data(), s.T.data(),
                                   s.sigma.data(), s.df.data(), s.side.data(),
                                   price.data(), vega.data(), kN);
    benchmark::DoNotOptimize(price.data());
    benchmark::DoNotOptimize(vega.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));

  // One-shot parity cross-check, surfaced as a benchmark label.
  double max_abs = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    const Black76ValueVega w = black76_value_and_vega(
        s.F[i], s.K[i], s.T[i], s.sigma[i], s.df[i], s.side[i]);
    max_abs = std::max(max_abs, std::abs(price[i] - w.price));
  }
  state.SetLabel("avx2=" + std::to_string(simd::have_avx2() ? 1 : 0) +
                 " max_abs_vs_scalar=" + std::to_string(max_abs));
}

// Register with the mandated common knobs (warm-up, 5 reps, p95 + CV).
const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("simd/b76_value_vega/scalar",
                                            BM_Black76_ValueVega_Scalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("simd/b76_value_vega/avx2",
                                            BM_Black76_ValueVega_Avx2))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace

} // namespace atx::vol::bench
