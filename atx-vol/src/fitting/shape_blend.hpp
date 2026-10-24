#pragma once

// shape_blend — the ONE implementation of the TenorInterp::ShapeBlend interior
// blend (risk-engine sprint 2026-10-24). CurveSurface (vol_curve.cpp) and the
// zero-copy PricedSurfaceView (priced_surface_view.cpp) evaluate their slices
// differently (virtual curves vs mapped kind dispatch), but both feed the SAME two
// out-of-line functions below, so an owned surface and a view over its archived
// copy agree bit-for-bit: the arithmetic is compiled once, never transcribed
// (clang may contract a transcribed copy into a different FMA shape).
//
// The formula is projection.hpp InterpMode::ShapeBlend (projection.cpp
// `w_on_inserted_slice`), restated on a CurveSurface bracket:
//   wwHi = upper_weight, wwLo = 1 - wwHi
//   atm_x = sqrt(w_x(0) / T_x);  atm_q^2 T_q = wwLo w_lo(0) + wwHi w_hi(0)
//   z = k / (atm_q sqrt T_q);    k_x = z atm_x sqrt T_x
//   m_x = sqrt(w_x(k_x) / T_x) / atm_x;  sigma = atm_q (wwLo m_lo + wwHi m_hi)
//   w = sigma^2 T_q
// Pure value functions; thread-safe.

namespace atx::vol::detail {

// Stage 1 of a ShapeBlend query: the ATM anchor and the query's coordinates in
// each parent. `usable == false` means the caller must fall back to the
// TotalVariance blend (a degenerate T or ATM total variance); every other field is
// then unspecified.
struct ShapeBlendAnchor {
  double T_lo{0.0};
  double T_hi{0.0};
  double T_q{0.0};
  double ww_hi{0.0};
  double atm_lo{0.0};
  double atm_hi{0.0};
  double atm_q{0.0};
  double k_lo{0.0}; // where to read the lower parent's w
  double k_hi{0.0}; // where to read the upper parent's w
  bool usable{false};
};

// `w_lo0` / `w_hi0` are the parents' total variance at k = 0; `upper_weight` is
// the bracket's (T_q - T_lo) / (T_hi - T_lo).
[[nodiscard]] ShapeBlendAnchor shape_blend_anchor(double k_log, double T_q, double T_lo,
                                                  double T_hi, double upper_weight,
                                                  double w_lo0, double w_hi0) noexcept;

// Stage 2: blend the parents' total variance read at (k_lo, k_hi). Returns NaN when
// either reading is non-finite or negative, which the caller treats as "fall back
// to TotalVariance" (projection.cpp's rule).
[[nodiscard]] double shape_blend_w(const ShapeBlendAnchor &anchor, double w_lo_k,
                                   double w_hi_k) noexcept;

} // namespace atx::vol::detail
