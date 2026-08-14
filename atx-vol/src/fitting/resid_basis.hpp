#pragma once

// Dense vol-residual basis — the C2 compact-support "bump" family that the
// eSSVI dense-residual layer (ResidualBasisKind::C2Bspline) rides on top of the
// backbone. Header-only and SHARED by BOTH the calibrator (essvi_calib.cpp, which
// solves for the coefficients) and the hot-path evaluator (vol_surface.cpp, which
// serves them): keeping the basis in ONE place guarantees the fitted coefficients
// and the surface actually served can never drift apart.
//
// ## Shape
//
// N centers c_j uniform on the normalized log-moneyness y = k / scale in [-1, 1];
// each basis function is the C2 compact bump
//
//     B_j(y) = (1 - u^2)^3   for |u| < 1,  u = (y - c_j) / h,  else 0
//
// (the same kernel the CStar modal curve uses). h = 1.5 * spacing gives smooth
// overlapping support so the basis is well-conditioned and the fitted residual is
// C2. Unlike the wing-only HINGE_QUAD basis, these cover the WHOLE smile —
// including near-the-money — which is what lets a vega-weighted fit tighten the
// high-vega core toward the market (the metric that gates % within bid-ask).
//
// The coefficients live in `EssviParams::resid_coef[0 .. N-1]` (N <= 16, the
// storage width); the fit adds a 2nd-difference roughness penalty so the layer
// behaves as a smoothing spline (interpolates where quotes are dense, stays
// smooth where they are sparse) rather than overfitting noise.

#include <algorithm>
#include <array>
#include <cstddef>

namespace atx::vol::detail {

// Usable dense-residual coefficient count: clamped to the 16-slot `resid_coef`
// storage, with a floor of 4 (a smoothing fit needs a few knots to be meaningful).
[[nodiscard]] inline int resid_bump_count(int requested) noexcept {
  return std::clamp(requested, 4, 16);
}

// Evaluate the N C2 bumps at normalized moneyness `y`, writing B_j(y) into
// out[0 .. N-1] (and zeroing the rest of the 16-wide buffer). `y` is expected
// pre-clamped to [-1, 1] by the caller. Pure; no allocation; noexcept.
inline void resid_bump_basis(double y, int n, std::array<double, 16>& out) noexcept {
  out = {};
  n = resid_bump_count(n);
  const double spacing = 2.0 / static_cast<double>(n - 1);
  const double h = 1.5 * spacing;
  for (int j = 0; j < n; ++j) {
    const double c = -1.0 + spacing * static_cast<double>(j);
    const double u = (y - c) / h;
    if (u > -1.0 && u < 1.0) {
      const double t = 1.0 - u * u;
      out[static_cast<std::size_t>(j)] = t * t * t;
    }
  }
}

}  // namespace atx::vol::detail
