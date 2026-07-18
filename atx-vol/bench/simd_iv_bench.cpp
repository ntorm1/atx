// Throughput proof for the vectorized implied-vol batch kernel: scalar
// per-contract inversion loop vs the AVX2 SoA batch, on one realistic SoA
// workload.
//
// Both cases invert the SAME array of premiums (priced once up front by
// black76_price at a spread of sigmas): the scalar case calls the per-contract
// implied_vol in a loop (the pre-vectorization hot path), the SIMD case calls
// simd::implied_vol_batch (4-lane AVX2 on this host). The items/s (contracts
// inverted per second) and the ns/contract Google Benchmark derives are the
// speedup evidence. A one-shot max-abs σ cross-check is printed via a label so
// the run also documents that the two agree.

#include "atx/vol/black76.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_util.hpp"

namespace atx::vol::simd::detail {
// Defined in src/simd/iv_batch_avx2.cpp; the AVX2 4-lane IV batch kernel. R-24
// routed the PUBLIC simd::implied_vol_batch to scalar, so this bench calls the
// kernel DIRECTLY (same pattern as tests/simd_iv_avx2_direct_test.cpp) to time
// the true vector path — going through the public entry would measure scalar.
void implied_vol_batch_avx2(const double *price, const double *F, const double *K, const double *T,
                            const double *df, const Side *side, double *iv_out,
                            std::uint8_t *ok_out, std::size_t n) noexcept;
} // namespace atx::vol::simd::detail

namespace atx::vol::bench {
namespace {

struct Soa {
  std::vector<double> price, F, K, T, df;
  std::vector<Side> side;
};

// A deterministic spread of contracts resembling a multi-expiry option surface:
// a few forwards × a near-ATM moneyness ladder × tenors × vols, tiled to the
// requested size and priced up front so the benchmark times only the inversion.
// The ladder stays near ATM so vega is healthy and the AVX2 kernel runs its full
// vector path on most lanes (deep wings would patch to scalar, which only slows
// the SIMD case — so keeping to the interior is the conservative comparison).
Soa make_workload(std::size_t n) {
  Soa s;
  s.price.reserve(n);
  s.F.reserve(n);
  s.K.reserve(n);
  s.T.reserve(n);
  s.df.reserve(n);
  s.side.reserve(n);
  const double moneyness[] = {0.90, 0.95, 1.0, 1.05, 1.10};
  const double tenors[] = {0.08, 0.25, 0.5, 1.0};
  const double vols[] = {0.15, 0.25, 0.35};
  std::size_t idx = 0;
  while (s.F.size() < n) {
    const double F = 50.0 + static_cast<double>((idx * 37) % 400);
    const double m = moneyness[idx % 5];
    const double T = tenors[(idx / 5) % 4];
    const double v = vols[(idx / 3) % 3];
    const double df = std::exp(-0.03 * T);
    const Side side = (idx & 1u) ? Side::Put : Side::Call;
    s.F.push_back(F);
    s.K.push_back(F * m);
    s.T.push_back(T);
    s.df.push_back(df);
    s.side.push_back(side);
    s.price.push_back(black76_price(F, F * m, T, v, df, side));
    ++idx;
  }
  return s;
}

constexpr std::size_t kN = 1u << 14; // 16,384 contracts

void BM_ImpliedVol_Scalar(benchmark::State& state) {
  const Soa s = make_workload(kN);
  std::vector<double> iv(kN);
  for (auto _ : state) {
    for (std::size_t i = 0; i < kN; ++i) {
      const Result<double> r = implied_vol(s.price[i], s.F[i], s.K[i], s.T[i],
                                           s.df[i], s.side[i]);
      iv[i] = r ? *r : std::nan("");
    }
    benchmark::DoNotOptimize(iv.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_ImpliedVol_Avx2(benchmark::State& state) {
  const Soa s = make_workload(kN);
  std::vector<double> iv(kN);
  std::vector<std::uint8_t> ok(kN);
  for (auto _ : state) {
    simd::detail::implied_vol_batch_avx2(s.price.data(), s.F.data(), s.K.data(), s.T.data(),
                                         s.df.data(), s.side.data(), iv.data(), ok.data(), kN);
    benchmark::DoNotOptimize(iv.data());
    benchmark::DoNotOptimize(ok.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));

  // One-shot parity cross-check, surfaced as a benchmark label.
  double max_abs = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    const Result<double> r = implied_vol(s.price[i], s.F[i], s.K[i], s.T[i],
                                         s.df[i], s.side[i]);
    if (r && ok[i]) {
      max_abs = std::max(max_abs, std::abs(iv[i] - *r));
    }
  }
  state.SetLabel("avx2=" + std::to_string(simd::have_avx2() ? 1 : 0) +
                 " max_abs_vs_scalar=" + std::to_string(max_abs));
}

// Register with the mandated common knobs (warm-up, 5 reps, p95 + CV).
const int kRegistered = [] {
  apply_common(
      benchmark::RegisterBenchmark("simd/iv_invert/scalar", BM_ImpliedVol_Scalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("simd/iv_invert/avx2", BM_ImpliedVol_Avx2))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace

} // namespace atx::vol::bench
