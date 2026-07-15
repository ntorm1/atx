#pragma once

// Shared synthetic-surface fixtures for the analytics tests. In-memory eSSVI
// PricedSurfaces (no external data files), so every analytics_*_test can build
// a known-good surface with analytic properties.
//
//   make_flat_surface(...)   — φ=0 eSSVI ⇒ w(k)=θ is flat in log-moneyness, so
//                              iv(K,T)=σ everywhere: skew=0, curvature=0, the
//                              risk-neutral density is exactly lognormal(σ√T).
//                              θ_i = σ²·T_i (calendar-arb-free, w increasing).
//   make_skewed_surface(...) — genuine downside skew (ρ<0) + curvature (φ>0),
//                              mirrors strategy_test/backtest_test make_surface.
//   make_event_schedule(...) — one earnings instant at now + t_event years.
//
// eSSVI backbone (vol_surface.hpp): w(k) = (θ/2)(1 + ρφk + √((φk+ρ)² + (1−ρ²))).

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "atx/vol/american.hpp"       // al_fast_opts, AmericanMethod
#include "atx/vol/event_vol.hpp"      // EventSchedule
#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/vol_curve.hpp"      // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"    // EssviParams

namespace atx::vol::testkit {

inline constexpr double kFixtureRate = 0.043;
inline constexpr std::int64_t kFixtureNow = 1700000000000000000LL; // 2023-11-14T22:13:20Z
inline constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
inline constexpr double kYearNs = 365.25 * 86400.0 * 1e9;

// The default slice grid (year-fractions), ascending — covers 18d … 1y so the
// 1w/2w tenors interpolate below the front pillar's 50% floor is avoided.
inline const std::vector<double> &fixture_tenors() {
  static const std::vector<double> t = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  return t;
}

[[nodiscard]] inline PricedSurface unwrap_surface(Result<PricedSurface> r) {
  if (!r.has_value()) {
    throw std::runtime_error("analytics fixture: PricedSurface::create failed: " +
                             r.error().to_string());
  }
  return std::move(*r);
}

// Flat lognormal surface: iv(K,T) == sigma for all K,T on the grid.
[[nodiscard]] inline PricedSurface
make_flat_surface(std::uint32_t uid, double S, double fwd, double sigma,
                  std::int64_t now_ts = kFixtureNow,
                  const std::vector<double> &Ts = fixture_tenors()) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  std::uint16_t i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = sigma * sigma * T; // w = θ ⇒ iv = sqrt(θ/T) = sigma
    e.phi = 0.0;                 // flat smile
    e.rho = 0.0;
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = fwd;
    e.expiry_id = i;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kFixtureRate * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.0, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kFixtureRate;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return unwrap_surface(PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

// Skewed/curved surface with genuine American premium (q_eff = 0.02) and a
// downside skew (ρ<0). `vol_bump` shifts the whole level (for two-surface diffs).
[[nodiscard]] inline PricedSurface
make_skewed_surface(std::uint32_t uid, double S, double fwd, std::int64_t now_ts = kFixtureNow,
                    double vol_bump = 0.0, const std::vector<double> &Ts = fixture_tenors()) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  std::uint16_t i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = fwd;
    e.expiry_id = i;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kFixtureRate * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kFixtureRate;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return unwrap_surface(PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

// One earnings instant `t_event` years after `now_ts` (Calendar365).
[[nodiscard]] inline EventSchedule make_event_schedule(double t_event,
                                                       std::int64_t now_ts = kFixtureNow) {
  const auto ev = now_ts + static_cast<std::int64_t>(t_event * kYearNs);
  return EventSchedule(std::vector<std::int64_t>{ev});
}

} // namespace atx::vol::testkit
