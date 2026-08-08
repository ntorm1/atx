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
//   deriv/greeks/standard_priced_surface — deriv_greeks on the PricedSurface-
//                                       native entry point (Task P-1): a
//                                       DerivQuality::Standard (257-node) var
//                                       swap with the default greek bundle
//                                       (second_order + carry_theta on), the
//                                       center quote plus up to 16 bumped/
//                                       rolled strip repricings per call
//                                       (Task P-2 dropped the r+ FD rho bump:
//                                       17 -> 16) — the per-strip-constant
//                                       resolve hoist's target.
//   swap_leg/solve_cycle_swap_entry   — solve_cycle_swap's entry-vega greeks
//                                       call (swap_leg.cpp, Task P-2 / GK-P3):
//                                       a VarSwap cycle-open sizing solve that
//                                       reads ONLY `greeks->vega`, so its
//                                       DerivGreekBumps carries
//                                       second_order = false, carry_theta =
//                                       false (this task's swap_leg change) —
//                                       the per-cycle-open backtest-populate
//                                       path the brief's "one backtest
//                                       populate leg" bench targets.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <vector>

#include "atx/vol/american.hpp"    // al_fast_opts, AmericanMethod
#include "atx/vol/analytics.hpp" // AnalyticsConfig, compute_surface_analytics, risk_neutral_density, var_swap_vol, compute_surface_diff
#include "atx/vol/backtest.hpp"    // MarketSnapshot, SurfaceRef
#include "atx/vol/derivatives.hpp" // DerivContract/Config/GreekBumps, deriv_greeks (Task P-1)
#include "atx/vol/surface_archive.hpp" // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/swap_leg.hpp"        // CycleSwapRequest, solve_cycle_swap (Task P-2)
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

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

// ── deriv/greeks/standard_priced_surface ──────────────────────────────────

// An unaged 1e6-notional var swap at the same 90d tenor as the RND/var-swap
// cases above (inside the fixture's fitted pillar range), priced against the
// skewed PricedSurface fixture.
[[nodiscard]] DerivContract skewed_var_swap_contract() {
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = kRndTenorYears;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  return c;
}

void BM_DerivGreeks_Standard_PricedSurface(benchmark::State &state) {
  const PricedSurface &ps = skewed_surface();
  const DerivContract contract = skewed_var_swap_contract();
  const DerivConfig cfg{};       // DerivQuality::Standard (257-node strip)
  const DerivGreekBumps bumps{}; // second_order + carry_theta both default true
  for (auto _ : state) {
    Result<DerivGreeks> g = deriv_greeks(ps, contract, cfg, bumps);
    if (!g.has_value()) {
      state.SkipWithError(g.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(g);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = deriv_greeks calls
}

// ── swap_leg/solve_cycle_swap_entry ────────────────────────────────────────

// A one-symbol on-disk archive -> loadable MarketSnapshot, built ONCE (the
// only way to obtain a SurfaceRef -- solve_cycle_swap's own surface type).
// Mirrors tests/swap_leg_test.cpp's fixture recipe, trimmed to what the entry
// solve alone needs.
[[nodiscard]] const MarketSnapshot &solve_cycle_swap_snapshot() {
  static const MarketSnapshot value = [] {
    constexpr double kR = 0.043;
    constexpr std::uint32_t kUid = 11;
    constexpr std::int64_t kBaseNow = 1700000000000000000LL;

    CurveSurface cs;
    std::vector<SliceContext> ctx;
    const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
    int i = 0;
    for (const double T : Ts) {
      const double term_forward = 100.0 * std::exp((kR - 0.02) * T);
      EssviParams e{};
      e.theta = 0.04 + 0.005 * static_cast<double>(i);
      e.phi = 1.5 - 0.05 * static_cast<double>(i);
      e.rho = -0.4 + 0.02 * static_cast<double>(i);
      e.psi = 0.5;
      e.p = 0.5;
      e.lambda = 0.5;
      e.T = T;
      e.F = term_forward;
      e.expiry_id = static_cast<std::uint16_t>(i);
      cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
      ctx.push_back(SliceContext{T, term_forward, 0.0, 0.02, 250, 7});
      ++i;
    }
    PricingContext pc;
    pc.S = 100.0;
    pc.r = kR;
    pc.now_ts_ns = kBaseNow;
    pc.method = AmericanMethod::AndersenLake;
    pc.al_opts = al_fast_opts();
    pc.uid = kUid;
    Result<PricedSurface> ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
    if (!ps.has_value()) {
      std::abort(); // fixture construction must not fail silently
    }

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "atx-vol-bench-swap-leg";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "2026-08-01.atxvsa").string();
    const SurfaceArchiveItem item{"XOM", &*ps, std::nullopt};
    const std::span<const SurfaceArchiveItem> items(&item, 1);
    if (!write_surface_archive_v2_file(path, items).has_value()) {
      std::abort();
    }
    Result<MarketSnapshot> snap = MarketSnapshot::load(path);
    if (!snap.has_value()) {
      std::abort();
    }
    return std::move(*snap);
  }();
  return value;
}

// A 5-session grid (kUid, kBaseNow, kStepNs matching the snapshot fixture
// above): open at sessions[0], expiry at sessions[4], 3 observed returns
// (mirrors swap_leg_test.cpp's make_request/SolveCycleSwapStrikesFair...).
[[nodiscard]] CycleSwapRequest solve_cycle_swap_request() {
  static const std::vector<std::int64_t> sessions = [] {
    constexpr std::int64_t kBaseNow = 1700000000000000000LL;
    constexpr std::int64_t kStepNs = 30LL * 86400LL * 1000000000LL;
    std::vector<std::int64_t> out;
    for (int i = 0; i < 5; ++i) {
      out.push_back(kBaseNow + i * kStepNs);
    }
    return out;
  }();
  CycleSwapRequest req;
  req.uid = 11u;
  req.kind = DerivKind::VarSwap;
  req.cap_dec = 0.0;
  req.notional = 1.0;
  req.annualization = 252.0;
  req.open_ts_ns = sessions.front();
  req.expiry_ts_ns = sessions.back();
  req.session_ts = sessions;
  req.deriv_cfg = DerivConfig{};
  return req;
}

void BM_SolveCycleSwapEntry(benchmark::State &state) {
  const MarketSnapshot &snap = solve_cycle_swap_snapshot();
  const SurfaceRef surface = snap.find(11u);
  const CycleSwapRequest req = solve_cycle_swap_request();
  constexpr double kTargetVega = 2500.0;
  for (auto _ : state) {
    Result<SwapLot> lot = solve_cycle_swap(surface, req, kTargetVega);
    if (!lot.has_value()) {
      state.SkipWithError(lot.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(lot);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = solve_cycle_swap calls
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

  apply_common(benchmark::RegisterBenchmark("deriv/greeks/standard_priced_surface",
                                            BM_DerivGreeks_Standard_PricedSurface))
      ->Unit(benchmark::kMicrosecond);

  apply_common(benchmark::RegisterBenchmark("swap_leg/solve_cycle_swap_entry",
                                            BM_SolveCycleSwapEntry))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
