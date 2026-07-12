#pragma once

// Skew-adjusted delta / VegaSlope — SpiderRock LiveVolSurfaces /
// ClientVolatilitySurfaces "Adjusted Greeks" model, ported verbatim:
//
//   Adjusted Delta = Delta + VegaSlope * Vega
//
// where VegaSlope = dSigma/dS is how much the curve's implied vol AT A FIXED
// STRIKE moves when the underlying moves, blending two idealized smile
// dynamics via a sticky control omega = refUPrcWeight in [0, 1]:
//
//   omega = 0  sticky-delta:  the smile is fixed in log-moneyness
//              k = ln(K/F) and slides bodily with the underlying (F prop. to
//              S), so a fixed strike's effective vol DOES move as S moves.
//   omega = 1  sticky-strike: the smile is fixed in absolute strike K and is
//              pinned regardless of S, so VegaSlope collapses to 0 (the
//              "raw", unadjusted analytic Greeks).
//
//   VegaSlope        = (1 - omega) * dSigma/dS|slide
//   dSigma/dS|slide   = -(dSigma/dk) / S        (k = ln(K/F), F prop. to S)
//
// `curve_skew_slope` gets dSigma/dk from the curve's analytic total-variance
// slope w'(k) (central finite difference, h = 1e-4, on `IVolCurve::w` — per
// vol_curve.hpp the interface is virtual only at the slice-query layer, so
// this is 2 extra vcalls, not a hot-path concern) via
//
//   sigma = sqrt(w(k) / T),   dSigma/dk = w'(k) / (2 * sigma * T).
//
// ## Thread-safety
//
// Every entry here is a pure function of its arguments (a fitted `IVolCurve`
// is an immutable value after construction, so concurrent `w` reads are
// safe) — safe to call concurrently from any number of threads.

#include "atx/vol/greeks.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol {

// Sticky-delta/sticky-strike blend control. `ref_uprc_weight` is
// SpiderRock's refUPrcWeight: 0 = pure sticky-delta (smile slides with the
// underlying), 1 = pure sticky-strike (smile pinned, VegaSlope forced to 0).
// Values outside [0, 1] are accepted uninterpreted (no clamp) since the
// formula is affine in omega.
struct StickyParams {
  double ref_uprc_weight{0.0};
};

// dSigma/dk at `k_log`, from the curve's analytic total-variance slope:
// sigma = sqrt(w(k_log) / T), dSigma/dk = w'(k_log) / (2 * sigma * T), with
// w'(k_log) a central finite difference (h = 1e-4) on `IVolCurve::w`.
//
// NaN if T <= 0, if sigma(k_log) is non-positive/non-finite (e.g. w(k_log)
// <= 0, or the curve declines to serve k_log), or if either FD stencil point
// lands where `w` returns NaN (e.g. off the ConvexDense curve's no-arb price
// band). A curve that flat-extrapolates at its wings (LinearVarianceCurve)
// yields a well-defined slope even AT the boundary node: the central
// stencil straddles the kink, so the result is the average of the flat
// (zero) side and the true interior one-sided slope, i.e. exactly half the
// interior segment's slope.
[[nodiscard]] double curve_skew_slope(const IVolCurve& c, double k_log) noexcept;

// VegaSlope = (1 - omega) * (-skew_slope / S), the sticky-delta/sticky-strike
// blended dSigma/dS at a fixed strike (k = ln(K/F), F proportional to S).
// NaN if S <= 0 / non-finite, or if `curve_skew_slope` is NaN.
[[nodiscard]] double vega_slope_per_spot(const IVolCurve& c, double k_log, double S,
                                         const StickyParams& sp = {}) noexcept;

// Adjusted Delta = Delta + VegaSlope * Vega (SpiderRock's skew-adjusted
// delta); every other field of `g` passes through unchanged. A non-finite
// `vega_slope` propagates to a non-finite adjusted delta (no clamp — the
// caller decides how to handle a degenerate input).
[[nodiscard]] Greeks skew_adjusted(const Greeks& g, double vega_slope) noexcept;

}  // namespace atx::vol
