#pragma once

// atx::engine::risk — short/long-horizon covariance blend (S8.8).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  Vendors ship TWO risk models at two horizons (USE4S/L, AXUS4 SH/MH): a fast
//  short-horizon model that tracks the recent regime and a slow long-horizon model
//  that anchors structure. There is no single "best" half-life set — the short model
//  responds to a vol spike, the long model is steadier. The blend is the standard
//  reconciliation: a convex combination of the two horizons' covariances.
//
//    blend_factor_cov(F_short, F_long, w) = w·F_short + (1−w)·F_long   (K×K)
//    blend_specific  (d_short, d_long, w) = w·d_short + (1−w)·d_long   (length M)
//
//  with `w` clamped to [0,1]. Each input is built by the SAME estimator at a
//  different half-life set (ewma_factor_covariance / specific_risk_blend), so the
//  blend takes two SPD/positive inputs of the SAME dimension.
//
// ===========================================================================
//  PSD preservation (the whole point)
// ===========================================================================
//  A convex combination of two SPD matrices is SPD: for any x ≠ 0,
//  xᵀ(w·F_s + (1−w)·F_l)x = w·(xᵀF_s x) + (1−w)·(xᵀF_l x) > 0 when 0 ≤ w ≤ 1 and at
//  least one weight is positive (both terms are > 0 for SPD F_s, F_l). So NO extra
//  PSD repair is needed after the blend — the inputs (from ewma_factor_covariance,
//  which eigenvalue-floors) are already SPD and the blend keeps it. `blend_specific`
//  likewise keeps each entry positive (a convex combo of two positive variances).
//
// ===========================================================================
//  Contract / determinism
// ===========================================================================
//  PURE, RNG-free, order-fixed (a single Eigen elementwise expression — ascending
//  storage order). Same inputs ⇒ byte-identical output. The dimension match is an
//  INTERNAL caller contract (the builder always blends two same-horizon outputs of
//  the same factor count / instrument count), so it is an ATX_ASSERT, not a runtime
//  Result — a mismatch is a programming error, not a data condition.

#include <algorithm> // std::clamp

#include <Eigen/Dense> // MatX / VecX elementwise ops

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64

#include "atx/core/linalg/linalg.hpp" // MatX, VecX (column-major Eigen)

namespace atx::engine::risk {

// Convex blend of two factor covariances at a short and a long horizon:
// F = w·F_short + (1−w)·F_long, with w clamped to [0,1]. F_short / F_long are the
// SAME-dimension SPD factor covariances built by ewma_factor_covariance at two
// half-life sets. PSD-preserving (convex combo of SPD is SPD — see header). The
// dimension match is an internal caller contract (ATX_ASSERT). w = 1 ⇒ F_short
// exactly, w = 0 ⇒ F_long exactly. Order-fixed, RNG-free.
[[nodiscard]] inline atx::core::linalg::MatX
blend_factor_cov(const atx::core::linalg::MatX &f_short, const atx::core::linalg::MatX &f_long,
                 atx::f64 w) {
  ATX_ASSERT(f_short.rows() == f_long.rows() && f_short.cols() == f_long.cols());
  const atx::f64 ww = std::clamp(w, 0.0, 1.0);
  return ww * f_short + (1.0 - ww) * f_long;
}

// Convex blend of two specific-variance vectors at a short and a long horizon:
// d = w·d_short + (1−w)·d_long, with w clamped to [0,1]. Elementwise; each entry is
// a convex combo of two positive variances ⇒ stays positive. Same-length internal
// caller contract (ATX_ASSERT). Order-fixed, RNG-free.
[[nodiscard]] inline atx::core::linalg::VecX blend_specific(const atx::core::linalg::VecX &d_short,
                                                            const atx::core::linalg::VecX &d_long,
                                                            atx::f64 w) {
  ATX_ASSERT(d_short.size() == d_long.size());
  const atx::f64 ww = std::clamp(w, 0.0, 1.0);
  return ww * d_short + (1.0 - ww) * d_long;
}

} // namespace atx::engine::risk
