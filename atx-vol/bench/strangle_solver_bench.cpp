// Per-eval solver micro-benchmarks for the SPY short-strangle backtest: where
// does a daily restrike step spend its time? Relocated from the (assertion-free,
// printf-only) DISABLED_SolverBreakdown diagnostic in
// tests/spy_strangle_backtest_test.cpp into a proper Google Benchmark TU.
//
// A restrike step resolves a 40-delta call + put strike (resolve_strike_by_delta,
// which rides the fast delta-only path) and then reprices each resolved leg's full
// FD greeks (for the book vega). These cases isolate the sub-operations the
// diagnostic timed, exercising BOTH legs (call + put) per timed iteration:
//
//   strangle/eval/greeks  — full american_greeks() FD greeks (result read via delta)
//   strangle/eval/delta   — the delta-only fast path (american_delta)
//   strangle/eval/resolve — the strike-from-delta bisection solver
//   strangle/eval/vega    — the same full greeks() call, read via vega (the per-leg
//                           cost expand_leg pays to size on book vega)
//
// greeks and vega both drive one full greeks() eval (a single call produces every
// axis); they are reported as separate cases only to mirror the diagnostic's two
// readouts (delta-reduction vs the full-greek per-leg vega cost).
//
// The synthetic eSSVI SPY surface is assembled ONCE per case, outside the timed
// loop; only the single eval is timed. items/s == option legs evaluated per second.

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod, AmericanGreeks
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"        // resolve_strike_by_delta
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Side, Result
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::uint32_t kSpy = 42;
constexpr double kTenorT = 0.5;
constexpr double kSpot = 600.0;  // date-0 spot of the diagnostic's evolving corpus

// Synthetic eSSVI SPY surface (the make_surface pattern from the strangle backtest
// test): seven strictly-ascending tenors with deterministic eSSVI params. Cache-
// free, no fit and no data dependency. Propagates PricedSurface::create's
// InvalidArgument (never hit for these fixed, well-formed params).
[[nodiscard]] Result<PricedSurface> make_surface(double S, std::int64_t now_ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = S;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, S, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = kSpy;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc);
}

// ── strangle/eval/greeks — full FD greeks on both legs, read via delta ────────
void BM_StrangleEvalGreeks(benchmark::State& state) {
  Result<PricedSurface> ps = make_surface(kSpot, kBaseNow);
  if (!ps.has_value()) {
    const std::string err = ps.error().to_string();
    state.SkipWithError(err.c_str());
    return;
  }
  const PricedSurface& s = *ps;
  const double F = s.forward_at(kTenorT);
  const double Kc = 1.05 * F;
  const double Kp = 0.95 * F;
  if (!s.greeks(Kp, kTenorT, Side::Put).has_value() ||
      !s.greeks(Kc, kTenorT, Side::Call).has_value()) {
    state.SkipWithError("greeks eval failed");
    return;
  }
  for (auto _ : state) {
    double sink = s.greeks(Kp, kTenorT, Side::Put)->delta;
    sink += s.greeks(Kc, kTenorT, Side::Call)->delta;
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * 2);
}

// ── strangle/eval/delta — delta-only fast path on both legs ───────────────────
void BM_StrangleEvalDelta(benchmark::State& state) {
  Result<PricedSurface> ps = make_surface(kSpot, kBaseNow);
  if (!ps.has_value()) {
    const std::string err = ps.error().to_string();
    state.SkipWithError(err.c_str());
    return;
  }
  const PricedSurface& s = *ps;
  const double F = s.forward_at(kTenorT);
  const double Kc = 1.05 * F;
  const double Kp = 0.95 * F;
  if (!s.delta(Kp, kTenorT, Side::Put).has_value() ||
      !s.delta(Kc, kTenorT, Side::Call).has_value()) {
    state.SkipWithError("delta eval failed");
    return;
  }
  for (auto _ : state) {
    double sink = s.delta(Kp, kTenorT, Side::Put).value_or(0.0);
    sink += s.delta(Kc, kTenorT, Side::Call).value_or(0.0);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * 2);
}

// ── strangle/eval/resolve — strike-from-delta solver on both legs ─────────────
void BM_StrangleEvalResolve(benchmark::State& state) {
  Result<PricedSurface> ps = make_surface(kSpot, kBaseNow);
  if (!ps.has_value()) {
    const std::string err = ps.error().to_string();
    state.SkipWithError(err.c_str());
    return;
  }
  const PricedSurface& s = *ps;
  if (!resolve_strike_by_delta(s, kTenorT, Side::Put, 0.40).has_value() ||
      !resolve_strike_by_delta(s, kTenorT, Side::Call, 0.40).has_value()) {
    state.SkipWithError("resolve failed");
    return;
  }
  for (auto _ : state) {
    double sink = resolve_strike_by_delta(s, kTenorT, Side::Put, 0.40).value_or(0.0);
    sink += resolve_strike_by_delta(s, kTenorT, Side::Call, 0.40).value_or(0.0);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * 2);
}

// ── strangle/eval/vega — full FD greeks on both legs, read via vega ───────────
void BM_StrangleEvalVega(benchmark::State& state) {
  Result<PricedSurface> ps = make_surface(kSpot, kBaseNow);
  if (!ps.has_value()) {
    const std::string err = ps.error().to_string();
    state.SkipWithError(err.c_str());
    return;
  }
  const PricedSurface& s = *ps;
  const double F = s.forward_at(kTenorT);
  const double Kc = 1.05 * F;
  const double Kp = 0.95 * F;
  if (!s.greeks(Kp, kTenorT, Side::Put).has_value() ||
      !s.greeks(Kc, kTenorT, Side::Call).has_value()) {
    state.SkipWithError("greeks eval failed");
    return;
  }
  for (auto _ : state) {
    double sink = s.greeks(Kp, kTenorT, Side::Put)->vega;
    sink += s.greeks(Kc, kTenorT, Side::Call)->vega;
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * 2);
}

// Register with the mandated common knobs (warm-up, 5 reps, p95 + CV), matching
// bench/simd_greeks_bench.cpp's style.
const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("strangle/eval/greeks", BM_StrangleEvalGreeks))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("strangle/eval/delta", BM_StrangleEvalDelta))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("strangle/eval/resolve", BM_StrangleEvalResolve))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("strangle/eval/vega", BM_StrangleEvalVega))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

}  // namespace

}  // namespace atx::vol::bench
