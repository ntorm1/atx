#pragma once

// atx::engine::risk — PSD-repair toolkit (S8.7).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  Two STANDALONE positive-semidefinite repair primitives (NOT wired into
//  FactorModel::build this sprint):
//
//    * nearest_correlation(A, max_iter, tol) — Higham (2002) alternating
//      projections: the FROBENIUS-CLOSEST unit-diagonal PSD matrix to a symmetric
//      near-correlation input that may be indefinite (e.g. a correlation matrix
//      assembled from pairwise / shrunk estimates that lost positive-definiteness).
//
//    * eigenvalue_clip(A, eps) — the CHEAP repair: eigendecompose, clip every
//      eigenvalue to ≥ eps, reassemble. For COVARIANCES (it does NOT preserve the
//      unit diagonal — do not use it on correlations). The eps floor gives strict
//      positive-definiteness (Cholesky succeeds), unlike Higham's 0-floor which
//      targets the nearest PSD correlation.
//
// ===========================================================================
//  Floors differ by problem (LOAD-BEARING)
// ===========================================================================
//  * nearest_correlation's PSD projection clips eigenvalues at 0 (the
//    nearest-correlation problem is "closest PSD with unit diagonal"; flooring at a
//    positive eps would move AWAY from the true projection).
//  * eigenvalue_clip floors at the caller's eps (strict PD for an invertible cov).
//  Two functions, two floors — deliberate, not an inconsistency.
//
// ===========================================================================
//  Higham bounded loop (JPL rule)
// ===========================================================================
//  The alternating-projection loop runs a FIXED upper bound of `max_iter`
//  iterations. The Frobenius early-exit (‖Y−Yprev‖_F < tol) is an OPTIMIZATION, not
//  the termination guarantee — the loop is bounded regardless of convergence.
//
//    Y = A,  ΔS = 0
//    repeat (at most max_iter):
//      R = Y − ΔS
//      X = proj_PSD(R)              (symmetric_eig; clip eigenvalues <0 to 0; rebuild)
//      ΔS = X − R                   (Dykstra correction)
//      Yprev = Y
//      Y = X with Y_ii = 1          (unit-diagonal projection)
//      if ‖Y − Yprev‖_F < tol: stop
//
//  // PATTERN-B: atx-core nearest_corr is the L7 lift; the engine ships
//  Higham-via-symmetric_eig here.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG, no clock, no map iteration. symmetric_eig is deterministic; every
//  reduction runs in canonical ascending order. Same input ⇒ byte-identical output.

#include <utility> // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp"            // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/linalg/decompose.hpp" // symmetric_eig
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX
#include "atx/core/types.hpp"            // f64, usize

namespace atx::engine::risk {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

namespace detail {

// Projection onto the PSD cone: eigendecompose, clip eigenvalues below `floor` up to
// `floor`, reassemble U·diag(Λ_clipped)·Uᵀ. `floor`==0 ⇒ the nearest PSD matrix
// (Higham); a positive floor ⇒ strict PD (eigenvalue_clip). Order-fixed ascending.
[[nodiscard]] inline Result<MatX> project_psd(const MatX &a, atx::f64 floor) {
  ATX_TRY(const auto eig, atx::core::linalg::symmetric_eig(a));
  VecX clipped = eig.values;
  for (Eigen::Index k = 0; k < clipped.size(); ++k) {
    if (clipped[k] < floor) {
      clipped[k] = floor;
    }
  }
  return Ok(MatX(eig.vectors * clipped.asDiagonal() * eig.vectors.transpose()));
}

} // namespace detail

// Higham (2002) nearest-correlation matrix via alternating projections with the
// Dykstra correction. Returns the Frobenius-closest matrix to `a` that is PSD and
// has unit diagonal.
//
// Contract: `a` is symmetric (the symmetry is enforced by symmetric_eig inside the
// PSD projection; a non-symmetric input yields Err). `max_iter` is the FIXED upper
// iteration bound (JPL bounded-loop rule); `tol` is the Frobenius early-exit
// threshold. Defaults: 100 iterations, 1e-9.
//
// Err: symmetric_eig failure inside any PSD projection (InvalidArgument on a
// non-symmetric / empty input, Internal on eigensolver non-convergence).
[[nodiscard]] inline Result<MatX> nearest_correlation(const MatX &a, atx::usize max_iter = 100,
                                                      atx::f64 tol = 1e-9) {
  const Eigen::Index n = a.rows();
  MatX y = a;
  MatX delta_s = MatX::Zero(n, n);
  // FIXED upper bound — the loop runs at most max_iter times regardless of
  // convergence; the tol check below is an early-exit optimization only.
  for (atx::usize iter = 0; iter < max_iter; ++iter) {
    const MatX r = y - delta_s;
    ATX_TRY(const MatX x, detail::project_psd(r, /*floor=*/0.0)); // proj onto PSD cone
    delta_s = x - r;                                              // Dykstra correction
    const MatX y_prev = y;
    y = x;
    for (Eigen::Index i = 0; i < n; ++i) {
      y(i, i) = 1.0; // projection onto the unit-diagonal set
    }
    if ((y - y_prev).squaredNorm() < tol * tol) {
      break; // converged — Frobenius change below tol
    }
  }
  return Ok(std::move(y));
}

// Cheap PSD repair for a COVARIANCE: eigendecompose, clip every eigenvalue to ≥ eps,
// reassemble. The eps floor yields strict positive-definiteness (Cholesky succeeds).
// Does NOT preserve a unit diagonal — for correlations use nearest_correlation.
//
// GRACEFUL DEGRADE (non-fallible MatX return): if symmetric_eig fails (non-symmetric
// or non-convergent input), fall back to a diagonal RIDGE — the input's diagonal
// floored at eps, off-diagonal zeroed. That is always a valid PD matrix (assuming
// eps>0) and a documented worst-case repair, so no Result allocation is needed.
[[nodiscard]] inline MatX eigenvalue_clip(const MatX &a, atx::f64 eps) {
  const auto repaired = detail::project_psd(a, eps);
  if (repaired.has_value()) {
    return *repaired;
  }
  // Fallback: diagonal ridge (off-diagonal dropped, diagonal floored at eps).
  const Eigen::Index n = a.rows();
  MatX ridge = MatX::Zero(n, n);
  for (Eigen::Index i = 0; i < n; ++i) {
    ridge(i, i) = (a(i, i) > eps) ? a(i, i) : eps;
  }
  return ridge;
}

} // namespace atx::engine::risk
