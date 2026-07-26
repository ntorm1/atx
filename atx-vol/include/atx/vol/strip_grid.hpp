#pragma once

// ── The ONE log-forward-moneyness strip/grid convention (E2 / AN-P1-2) ──────
//
// atx-vol integrates model-free quantities — the variance strip / MFIV, the
// Breeden–Litzenberger density, the BKM moments — on a grid that is UNIFORM IN
// LOG-FORWARD-MONEYNESS k = ln(K/F), quadratured with composite Simpson.
//
// Before E2 there were TWO independent implementations of that grid:
// `derivatives.cpp` (fixed ±1.5 span by quality tier, linear-in-F forward
// interpolation) and `analytics_density.cpp` (adaptive width_sigmas·σ_atm·√T
// span). They disagreed on span policy AND on forward interpolation, so the
// same tenor on the same surface could produce two different K_var. This header
// is the single source of both conventions; both TUs call it, and it is also the
// seam E6 re-types `derivatives` onto.
//
// SPAN POLICY. Half-width in k is
//
//     kh = max(floor_half_width, width_sigmas · σ_atm · √T)
//
// The vol-scaled term is what keeps the wings on a high-vol or long-dated
// tenor: a σ = 60%, T = 1y name needs ±3.6 to reach 6σ√T, and integrating it on
// a fixed ±1.5 truncates the strip and biases K_var LOW. `floor_half_width` is a
// floor, never a cap. `width_sigmas = 0` pins the fixed span (the escape hatch
// a caller uses when it wants an exactly-specified strip).
//
// TRUNCATION IS A COVERAGE PROPERTY, NOT A NaN PROPERTY. The pre-E2 code raised
// `StripTruncated*` only when the surface returned a non-finite IV at an
// integration boundary. A parametric eSSVI/SVI surface returns a finite IV at
// EVERY k, so a truncated parametric strip reported full coverage — silently
// wrong, which is the AN-P1-2 defect. `wing_coverage` below decides truncation
// by comparing the actual span against the vol-scaled requirement.
//
// FORWARD INTERPOLATION IS LOG-LINEAR IN F. `forward_log_blend` matches
// `projection.cpp`'s `curve_forward_T` exactly (linear in log F, clamped
// outside the pillar range), so a forward read for a var strip and a forward
// read for a projection agree by construction.

#include <algorithm> // std::max (adaptive_half_width) — was only transitive
#include <cmath>
#include <cstddef>

namespace atx::vol::strip {

// Default adaptive width in σ√T units. Matches `RndConfig::width_sigmas`, which
// is where this policy was already correct — E2 propagates it, it does not
// invent it. 6σ covers ~1 - 2e-9 of a lognormal's mass per side.
inline constexpr double kDefaultWidthSigmas = 6.0;

// Half-width in log-forward-moneyness: the tier/config floor, widened to the
// tenor's own vol scale. Returns `floor_half_width` unchanged when σ_atm is
// unusable (non-finite / non-positive), when T is unusable, or when
// `width_sigmas <= 0` (span pinned by the caller).
[[nodiscard]] inline double adaptive_half_width(double floor_half_width, double sigma_atm, double T,
                                                double width_sigmas) noexcept {
  double kh = floor_half_width;
  if (std::isfinite(sigma_atm) && sigma_atm > 0.0 && std::isfinite(T) && T > 0.0 &&
      width_sigmas > 0.0) {
    kh = std::max(kh, width_sigmas * sigma_atm * std::sqrt(T));
  }
  return kh;
}

// How far out in k the wings must reach for the strip to be considered complete
// at this tenor. Zero (== "no requirement expressible") when σ_atm or T is
// unusable, which callers treat as "cannot judge coverage".
[[nodiscard]] inline double required_half_width(double sigma_atm, double T,
                                                double width_sigmas) noexcept {
  if (!std::isfinite(sigma_atm) || sigma_atm <= 0.0 || !std::isfinite(T) || T <= 0.0 ||
      !(width_sigmas > 0.0)) {
    return 0.0;
  }
  return width_sigmas * sigma_atm * std::sqrt(T);
}

// Per-side truncation verdict for an actual integration span [k_lo, k_hi].
struct WingCoverage {
  bool left_short{false};  // k_lo > -required  => left wing cut
  bool right_short{false}; // k_hi <  required  => right wing cut
};

// Decide truncation from SPAN COVERAGE. `required` comes from
// `required_half_width`; a non-positive `required` means coverage cannot be
// judged and neither side is reported short (the caller's own NaN-boundary
// check still applies on top).
[[nodiscard]] inline WingCoverage wing_coverage(double k_lo, double k_hi,
                                                double required) noexcept {
  WingCoverage out;
  if (!(required > 0.0)) {
    return out;
  }
  out.left_short = k_lo > -required;
  out.right_short = k_hi < required;
  return out;
}

// Force an odd node count (composite Simpson needs an even interval count),
// bumping a too-small request up to `minimum` rather than erroring.
[[nodiscard]] inline std::size_t odd_nodes(std::size_t requested, std::size_t minimum) noexcept {
  std::size_t n = requested < minimum ? minimum : requested;
  if ((n % 2u) == 0u) {
    ++n;
  }
  return n;
}

// Composite-Simpson weight for node i of n (n odd): end nodes 1, interior
// alternating 4 / 2. The caller supplies the trailing Δk/3.
[[nodiscard]] inline double simpson_weight(std::size_t i, std::size_t n) noexcept {
  if (i == 0 || i + 1 == n) {
    return 1.0;
  }
  return (i % 2u != 0u) ? 4.0 : 2.0;
}

// The ONE bracketing-pillar forward blend: LINEAR IN log(F).
//
// `projection.cpp`'s `curve_forward_T` already used this convention; the
// derivatives var strip used linear-in-F. Same forward curve, two answers at any
// T strictly between two pillars with F0 != F1. Both now call this.
//
// Linear-in-log-F keeps F strictly positive and is the convention that composes
// with the log-forward-moneyness grid the strip integrates on. Degenerate
// inputs — a non-increasing bracket, or a non-positive pillar forward where the
// log is undefined — fall back to the linear reading rather than returning NaN.
[[nodiscard]] inline double forward_log_blend(double t0, double f0, double t1, double f1,
                                              double T) noexcept {
  if (!(t1 > t0)) {
    return f0;
  }
  const double alpha = (T - t0) / (t1 - t0);
  if (!(f0 > 0.0) || !(f1 > 0.0)) {
    return f0 + alpha * (f1 - f0);
  }
  const double log_f = std::log(f0) + alpha * (std::log(f1) - std::log(f0));
  return std::exp(log_f);
}

} // namespace atx::vol::strip
