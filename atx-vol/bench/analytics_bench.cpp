// analytics_bench.cpp — structured vol-surface analytics throughput.
//
// The headline number is the full `compute_surface_analytics` bundle over a
// genuinely skewed/curved eSSVI surface (real skew + curvature + American
// premium, so the term-structure / wing / RND / var-swap work is all exercised
// at realistic cost). A second full-bundle case turns the two heavy density
// legs off (compute_rnd / compute_varswap) so the density cost can be read off
// as the delta between the two. The remaining three cases isolate the two most
// expensive primitives — the Breeden-Litzenberger risk-neutral density and the
// model-free var-swap strip — plus the two-surface change bundle.
//
// Every case builds its input surface(s) ONCE via a static-local accessor
// (built on first use, kept alive for the whole process), so the timed loop
// measures only the analytics call — the surface is never rebuilt inside the
// loop. The skewed surface fixture is testkit::make_skewed_surface (the same
// known-good eSSVI board the analytics_*_test suite fits against), reached via
// the shared tests/support/ header (../tests is on this target's PRIVATE
// include path, exactly as atx-vol-fitting-bench reaches its spy fixture).
//
//   analytics/surface/full            — compute_surface_analytics, default cfg
//                                       (term structure + skew/wing + RND +
//                                       var-swap): the headline throughput row.
//   analytics/surface/no_density      — same, cfg.compute_rnd = false and
//                                       cfg.compute_varswap = false: isolates the
//                                       term-structure/skew/wing cost from the
//                                       density cost.
//   analytics/rnd/one_tenor           — risk_neutral_density at one 90d tenor
//                                       (Breeden-Litzenberger + BKM strip).
//   analytics/var_swap/one_tenor      — var_swap_vol at the same 90d tenor
//                                       (model-free OTM log-strip).
//   analytics/diff/two_surface        — compute_surface_diff between the base
//                                       skewed surface and a +2vol-bumped copy
//                                       of the same underlying (same uid).

#include <benchmark/benchmark.h>

#include "atx/vol/analytics.hpp" // AnalyticsConfig, compute_surface_analytics, risk_neutral_density, var_swap_vol, compute_surface_diff

#include "bench_util.hpp"
#include "support/analytics_fixture.hpp" // testkit::make_skewed_surface, kFixtureNow

namespace atx::vol::bench {
namespace {

// 90-day tenor (ACT/365.25) — inside the fixture's fitted pillar range
// [0.05, 1.00], so the RND / var-swap strips read fitted (non-extrapolated) vol.
constexpr double kRndTenorYears = 90.0 / 365.25;

// The skewed/curved eSSVI board (uid 1, S = F = 100). Built ONCE on first use
// and kept alive for the whole process — never rebuilt inside a timed loop.
[[nodiscard]] const PricedSurface &skewed_surface() {
  static const PricedSurface value = testkit::make_skewed_surface(1u, 100.0, 100.0);
  return value;
}

// A +2vol-bumped copy of the SAME underlying (uid 1) for the two-surface diff.
[[nodiscard]] const PricedSurface &skewed_surface_bumped() {
  static const PricedSurface value =
      testkit::make_skewed_surface(1u, 100.0, 100.0, testkit::kFixtureNow, /*vol_bump=*/0.02);
  return value;
}

// ── analytics/surface/full + analytics/surface/no_density ─────────────────

void BM_SurfaceAnalytics(benchmark::State &state, AnalyticsConfig cfg) {
  const PricedSurface &ps = skewed_surface();
  for (auto _ : state) {
    Result<SurfaceAnalytics> bundle = compute_surface_analytics(ps, cfg);
    if (!bundle.has_value()) {
      state.SkipWithError(bundle.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(bundle);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = surface bundles
}

// ── analytics/rnd/one_tenor ───────────────────────────────────────────────

void BM_RiskNeutralDensity(benchmark::State &state) {
  const PricedSurface &ps = skewed_surface();
  const AnalyticsConfig cfg{};
  for (auto _ : state) {
    Result<RiskNeutralDensity> rnd = risk_neutral_density(ps, kRndTenorYears, cfg.rnd);
    if (!rnd.has_value()) {
      state.SkipWithError(rnd.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(rnd);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = densities
}

// ── analytics/var_swap/one_tenor ──────────────────────────────────────────

void BM_VarSwapVol(benchmark::State &state) {
  const PricedSurface &ps = skewed_surface();
  for (auto _ : state) {
    Result<double> vs = var_swap_vol(ps, kRndTenorYears);
    if (!vs.has_value()) {
      state.SkipWithError(vs.error().to_string().c_str());
      break;
    }
    double sink = *vs;
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = var-swap strips
}

// ── analytics/diff/two_surface ────────────────────────────────────────────

void BM_SurfaceDiff(benchmark::State &state) {
  const PricedSurface &a = skewed_surface();
  const PricedSurface &b = skewed_surface_bumped();
  const AnalyticsConfig cfg{};
  for (auto _ : state) {
    Result<SurfaceDiff> diff = compute_surface_diff(a, b, cfg);
    if (!diff.has_value()) {
      state.SkipWithError(diff.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(diff);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = diff bundles
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("analytics/surface/full", [](benchmark::State &state) {
    BM_SurfaceAnalytics(state, AnalyticsConfig{});
  }))->Unit(benchmark::kMicrosecond);

  apply_common(benchmark::RegisterBenchmark("analytics/surface/no_density",
                                            [](benchmark::State &state) {
                                              AnalyticsConfig cfg{};
                                              cfg.compute_rnd = false;
                                              cfg.compute_varswap = false;
                                              BM_SurfaceAnalytics(state, cfg);
                                            }))
      ->Unit(benchmark::kMicrosecond);

  apply_common(benchmark::RegisterBenchmark("analytics/rnd/one_tenor", BM_RiskNeutralDensity))
      ->Unit(benchmark::kMicrosecond);

  apply_common(benchmark::RegisterBenchmark("analytics/var_swap/one_tenor", BM_VarSwapVol))
      ->Unit(benchmark::kMicrosecond);

  apply_common(benchmark::RegisterBenchmark("analytics/diff/two_surface", BM_SurfaceDiff))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
