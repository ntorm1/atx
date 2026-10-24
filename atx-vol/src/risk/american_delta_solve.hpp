#pragma once

// american_delta_solve — the cold-confirmed strike-from-AMERICAN-delta solve used by
// the risk projection (strike_coord.cpp, DeltaConvention::American).
//
// A COPY of `solve_american_delta` / `solve_american_delta_screened` from
// src/analytics/contract_projection.cpp (risk-engine sprint 2026-10-24 decision:
// contract_projection is untouched this sprint; the two copies are reconciled when
// it is re-pointed at api/risk). Adapted in three ways:
//   * the target and the reported delta are SIGNED (call in (0,1), put in (-1,0));
//   * the tolerance is the caller's ProjectionPolicy::delta_tolerance;
//   * `interp` selects the smile: TotalVariance runs the original code on
//     PricedSurface::delta; another mode runs the same solve on the cold American
//     delta at that mode's vol (cold_route.hpp), always cold (no fast screen).
// The numerical path (Black seed with two smile refinements, one safeguarded
// Newton step, bracket widening, Illinois-secant refinement, and on a prepared
// fast tier a screened proposal that is always cold-confirmed) is unchanged.
//
// Thread-safety: a pure function of its arguments over a const surface.

#include <cstdint>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/core/types.hpp"                // Result, Side
#include "atx/vol/api/fitting/vol_curve.hpp"         // TenorInterp

namespace atx::vol::risk_detail {

struct AmericanDeltaSolution {
  double strike{0.0};
  double delta{0.0};             // signed American delta achieved at `strike` (cold route)
  std::uint16_t evaluations{0};  // American delta evaluations spent (saturating)
};

// The strike whose American delta at (T, side) equals `signed_target` within
// `tolerance` (|achieved - target| <= tolerance, cold-confirmed).
//
// @return InvalidArgument for a target outside (0,1) / (-1,0) or with the wrong sign
//         for `side`, or a tolerance outside (0, 1e-3]; Unavailable when the surface
//         has no forward / vol at T or the delta cannot be evaluated or converged;
//         NotFound when the target is not bracketed within |log-moneyness| <= 5.
[[nodiscard]] Result<AmericanDeltaSolution> solve_american_delta(const SurfaceRef &surface,
                                                                 double T, Side side,
                                                                 double signed_target,
                                                                 double tolerance,
                                                                 TenorInterp interp);

} // namespace atx::vol::risk_detail
