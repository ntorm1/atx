#pragma once

// cold_route — the surface's COLD American evaluation at (K, T) with the surface vol
// read under an explicit TenorInterp (risk-engine sprint 2026-10-24, lane B).
//
// TotalVariance answers through the surface's own cold queries
// (`fair_value` / `greeks_analytic` / `delta` with QueryExecution::ColdReference), so
// it is bit-identical to them by construction. Any other mode makes the IDENTICAL
// american_* calls PricedSurface's cold route makes (priced_surface.cpp
// price_resolved / greeks_resolved / delta_resolved: spot, rate_at(T), q_eff_at(T),
// the stored method and AL preset) at the sigma that mode reads — so a ShapeBlend
// mark, Greek or American delta is the honest answer on the ShapeBlend smile rather
// than a TotalVariance number reported under the ShapeBlend name.
//
// Thread-safety: pure functions over a const surface.

#include <cmath>
#include <optional>

#include "atx/core/error.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp" // TenorInterp
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::risk_detail {

// The cold pricer inputs at (K, T) under `interp`; `valid` is false for a
// non-finite / non-positive K, T or surface vol.
struct ColdPoint {
  double S{0.0};
  double K{0.0};
  double T{0.0};
  double sigma{0.0};
  double r{0.0};
  double q{0.0};
  bool valid{false};
};

[[nodiscard]] inline ColdPoint cold_point(const SurfaceRef &s, double K, double T,
                                          TenorInterp interp) noexcept {
  ColdPoint p;
  p.K = K;
  p.T = T;
  if (!(std::isfinite(K) && K > 0.0 && std::isfinite(T) && T > 0.0)) {
    return p;
  }
  p.S = s.pricing().S;
  p.sigma = s.iv(K, T, interp);
  p.r = s.rate_at(T);
  p.q = s.q_eff_at(T);
  p.valid = std::isfinite(p.sigma) && p.sigma > 0.0;
  return p;
}

[[nodiscard]] inline Result<double> cold_delta(const SurfaceRef &s, double K, double T,
                                               Side side, TenorInterp interp) {
  if (interp == TenorInterp::TotalVariance) {
    return s.delta(K, T, side, QueryExecution::ColdReference);
  }
  const ColdPoint p = cold_point(s, K, T, interp);
  if (!p.valid) {
    return atx::core::Err(ErrorCode::Unavailable, "cold_delta: no vol at (K, T)");
  }
  return american_delta(p.S, p.K, p.T, p.sigma, p.r, p.q, side, s.pricing().method,
                        std::optional<AlOpts>{s.pricing().al_opts});
}

[[nodiscard]] inline Result<double> cold_price(const SurfaceRef &s, double K, double T,
                                               Side side, TenorInterp interp) {
  if (interp == TenorInterp::TotalVariance) {
    return s.fair_value(K, T, side, QueryExecution::ColdReference);
  }
  const ColdPoint p = cold_point(s, K, T, interp);
  if (!p.valid) {
    return atx::core::Err(ErrorCode::Unavailable, "cold_price: no vol at (K, T)");
  }
  return american_price(p.S, p.K, p.T, p.sigma, p.r, p.q, side, s.pricing().method,
                        std::optional<AlOpts>{s.pricing().al_opts});
}

[[nodiscard]] inline Result<AmericanGreeks> cold_greeks_analytic(const SurfaceRef &s, double K,
                                                                 double T, Side side,
                                                                 TenorInterp interp) {
  if (interp == TenorInterp::TotalVariance) {
    return s.greeks_analytic(K, T, side, QueryExecution::ColdReference);
  }
  const ColdPoint p = cold_point(s, K, T, interp);
  if (!p.valid) {
    return atx::core::Err(ErrorCode::Unavailable, "cold_greeks_analytic: no vol at (K, T)");
  }
  const PricingContext &pc = s.pricing();
  if (pc.method == AmericanMethod::AndersenLake) {
    return american_greeks_al(p.S, p.K, p.T, p.sigma, p.r, p.q, side,
                              std::optional<AlOpts>{pc.al_opts});
  }
  return american_greeks_fd(p.S, p.K, p.T, p.sigma, p.r, p.q, side, pc.method,
                            std::optional<AlOpts>{pc.al_opts});
}

} // namespace atx::vol::risk_detail
