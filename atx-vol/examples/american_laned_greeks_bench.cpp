// H1 (WS-H) microbench: the per-contract American analytic-Greeks hot path,
// SCALAR per-contract american_greeks_al vs the 4-wide laned
// simd::american_put_greeks_batch bundle the FullGreeks risk loop now dispatches
// (evaluate_batch, ForceAvx2). Puts are placed in the genuine early-exercise
// regime (r>0) so the laned AVX2 boundary pack actually engages rather than
// scalar-patching every lane. Threads al_fast_opts() to mirror the production
// (HFT-preset) surfaces. Prints ns per contract for each route + the speedup.
//
// This is a reporting harness, not a gate; the correctness gate is the parity test
// PricedSurface.EvaluateBatchGreeksForceAvx2MatchesScalarWithinGate and
// american_batch_test's AmericanPutGreeksBatchAvx2.MatchesScalarAl.

#include <chrono>
#include <cstdio>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/simd/american_boundary_batch.hpp"
#include "atx/vol/types.hpp"

using namespace atx::vol;

int main() {
  constexpr std::size_t kN = 2048; // contracts per pass
  constexpr int kIters = 15;       // timed repetitions

  const double S = 100.0;
  const double r = 0.05; // r > 0 => puts are genuine American (early-exercise regime)
  const double q = 0.0;
  const std::optional<AlOpts> opts{al_fast_opts()}; // {7,16,4,1e-8} — the production scheme

  std::vector<double> Sv(kN, S), Kv(kN), Tv(kN), sig(kN), rv(kN, r), qv(kN, q);
  for (std::size_t i = 0; i < kN; ++i) {
    // Moneyness sweep 0.80..1.20 (deep ITM puts are the early-exercise-heavy lanes).
    Kv[i] = 80.0 + (40.0 * static_cast<double>(i % 400)) / 399.0;
    Tv[i] = 0.10 + 0.90 * (static_cast<double>(i % 37) / 36.0);
    sig[i] = 0.15 + 0.35 * (static_cast<double>(i % 23) / 22.0);
  }
  std::vector<AmericanGreeks> out(kN);

  const auto run_scalar = [&]() -> double {
    double acc = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
      const Result<AmericanGreeks> g =
          american_greeks_al(Sv[i], Kv[i], Tv[i], sig[i], rv[i], qv[i], Side::Put, opts);
      if (g.has_value()) {
        acc += g->delta + g->vega + g->gamma;
      }
    }
    return acc;
  };
  const auto run_laned = [&](simd::SimdIsa isa) -> double {
    (void)simd::american_put_greeks_batch(Sv.data(), Kv.data(), Tv.data(), sig.data(), rv.data(),
                                          qv.data(), kN, opts, out.data(), isa,
                                          /*need_vega=*/true, /*need_rho=*/true,
                                          /*need_charm=*/true);
    double acc = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
      acc += out[i].delta + out[i].vega + out[i].gamma;
    }
    return acc;
  };

  // Warmup.
  volatile double sink = 0.0;
  sink += run_scalar();
  sink += run_laned(simd::SimdIsa::ForceScalar);
  sink += run_laned(simd::SimdIsa::ForceAvx2);

  const auto bench = [&](const char *name, auto &&fn) {
    const auto t0 = std::chrono::steady_clock::now();
    double acc = 0.0;
    for (int it = 0; it < kIters; ++it) {
      acc += fn();
    }
    const auto t1 = std::chrono::steady_clock::now();
    sink += acc;
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double per = ns / (static_cast<double>(kN) * static_cast<double>(kIters));
    std::printf("  %-28s %8.1f ns/contract\n", name, per);
    return per;
  };

  std::printf("American analytic-Greeks bundle — %zu puts x %d iters (r=%.2f, al_fast_opts)\n", kN,
              kIters, r);
  const double ns_scalar = bench("scalar american_greeks_al", run_scalar);
  const double ns_batch_scalar =
      bench("laned batch (ForceScalar)", [&]() { return run_laned(simd::SimdIsa::ForceScalar); });
  const double ns_batch_avx2 =
      bench("laned batch (ForceAvx2)", [&]() { return run_laned(simd::SimdIsa::ForceAvx2); });
  std::printf("  avx2_greeks_selected(ForceAvx2) = %s\n",
              simd::avx2_greeks_selected(simd::SimdIsa::ForceAvx2) ? "true (AVX2 host)"
                                                                   : "false (scalar fallback)");
  std::printf("  speedup laned-AVX2 vs scalar-loop = %.2fx\n", ns_scalar / ns_batch_avx2);
  std::printf("  speedup laned-AVX2 vs laned-scalar = %.2fx\n", ns_batch_scalar / ns_batch_avx2);
  return static_cast<int>(sink) & 0; // keep sink live, always return 0
}
