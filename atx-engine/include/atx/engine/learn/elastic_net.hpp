#pragma once

// atx::engine::learn — elastic-net (L1+L2) coordinate-descent solver (S5-3).
//
// =====================================================================
//  What this header is — the Pattern-B edge-1 kernel atx-core lacks
// =====================================================================
//  atx-core ships core::linalg::{ols, ridge, wls} — all L2-only. There is no
//  L1 / elastic-net solver. S5-3's linear learned alpha wants an L1-sparsifying
//  penalty (drop dead features) blended with L2 shrinkage (stabilize collinear
//  ones), so this header provides the missing kernel ENGINE-LOCAL (Pattern-B,
//  M5): a deterministic, RNG-free cyclic coordinate-descent (CD) minimizer of
//
//      f(b) = (1/2n)||y - Xs*b||^2 + lambda*( alpha*||b||_1 + (1-alpha)/2*||b||_2^2 )
//
//  over a COLUMN-STANDARDIZED design Xs (each column mean 0, population-std 1 —
//  the caller standardizes; the penalty is only fair across columns of equal
//  scale). alpha == 0 collapses to pure ridge; alpha == 1 to pure lasso.
//
// =====================================================================
//  The algorithm (Friedman-Hastie-Tibshirani 2010, "Regularization Paths for
//  Generalized Linear Models via Coordinate Descent", J. Stat. Software 33(1))
// =====================================================================
//  Cyclic CD: sweep coordinates j = 0..p-1 in FIXED order, each time minimizing
//  f over b[j] with the others held fixed. With Xs standardized the per-feature
//  second moment col_norm2[j]/n is 1 by construction, but we compute it exactly
//  so the kernel is correct for any (even non-standardized) input. The univariate
//  minimizer is the soft-thresholded, ridge-shrunk partial correlation:
//
//      rho_j = (1/n) * Xs[:,j] . r_partial          (r_partial = r + Xs[:,j]*b[j])
//      b[j]  = soft_threshold(rho_j, lambda*alpha)
//              / ( col_norm2[j]/n + lambda*(1-alpha) )
//      soft_threshold(z, g) = sign(z) * max(|z| - g, 0)
//
//  where r = y - Xs*b is the working residual, maintained incrementally: before
//  updating b[j] we ADD Xs[:,j]*b[j] back into r (forming r_partial), and after
//  computing the new b[j] we SUBTRACT Xs[:,j]*b_new[j] out again. Iterate sweeps
//  until max|Δb| < tol or max_iter sweeps elapse.
//
// =====================================================================
//  Determinism (M1) — RNG-free, fixed-order
// =====================================================================
//  No RNG. The sweep order is the natural coordinate order; every inner product
//  walks rows in ascending index, so the floating-point result is run-to-run
//  byte-identical. Two calls on identical input return bit-identical coeffs.
//
// Header-only; every function is defined inline. Fitting is a COLD path (once per
// training window / fold), so the working-residual VecX allocation is fine.

#include <cmath> // std::fabs

#include <Eigen/Dense> // Eigen::Index, column dot products

#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

namespace atx::engine::learn {

namespace lin = atx::core::linalg;

// ===========================================================================
//  ElasticNetCfg — the penalty + stopping knobs.
//
//  lambda  : overall penalty strength (>= 0). 0 recovers unpenalized OLS.
//  alpha   : L1/L2 mix in [0, 1]. 0 == pure ridge, 1 == pure lasso.
//  max_iter: maximum number of full coordinate sweeps.
//  tol     : convergence threshold on the max coefficient change in a sweep.
// ===========================================================================
struct ElasticNetCfg {
  atx::f64 lambda;
  atx::f64 alpha;
  atx::usize max_iter = 1000;
  atx::f64 tol = 1e-8;
};

namespace detail {

// soft_threshold(z, g) = sign(z) * max(|z| - g, 0). The proximal operator of the
// L1 penalty: shrinks z toward 0 by g, clamping at 0 (the lasso's sparsifier).
[[nodiscard]] inline atx::f64 soft_threshold(atx::f64 z, atx::f64 g) noexcept {
  const atx::f64 mag = std::fabs(z) - g;
  if (mag <= 0.0) {
    return 0.0;
  }
  return (z > 0.0 ? 1.0 : -1.0) * mag;
}

} // namespace detail

// ===========================================================================
//  elastic_net — minimize the elastic-net objective by cyclic coordinate descent.
//
//  Xs is the (n x p) COLUMN-STANDARDIZED design; y the (n) target. Returns the
//  (p) coefficient vector. Degenerate inputs (no rows / no columns) return a
//  zero / empty coefficient vector — there is nothing to fit.
//
//  Determinism (M1): RNG-free, fixed sweep order, ascending-index reductions.
// ===========================================================================
[[nodiscard]] inline lin::VecX elastic_net(const lin::MatX &Xs, const lin::VecX &y,
                                           const ElasticNetCfg &c) {
  const Eigen::Index n = Xs.rows();
  const Eigen::Index p = Xs.cols();
  lin::VecX b = lin::VecX::Zero(p);
  if (n == 0 || p == 0) {
    return b;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);

  // Per-column second moment (Xs[:,j] . Xs[:,j]) / n. For a standardized column
  // this is 1; computed exactly so the kernel is correct for any input.
  lin::VecX col_m2(p);
  for (Eigen::Index j = 0; j < p; ++j) {
    col_m2(j) = Xs.col(j).squaredNorm() / nf;
  }

  // Working residual r = y - Xs*b. b starts at 0, so r starts at y.
  lin::VecX r = y;

  const atx::f64 l1 = c.lambda * c.alpha;          // L1 soft-threshold radius
  const atx::f64 l2 = c.lambda * (1.0 - c.alpha);  // L2 (ridge) shrinkage term

  for (atx::usize sweep = 0; sweep < c.max_iter; ++sweep) {
    atx::f64 max_delta = 0.0;
    for (Eigen::Index j = 0; j < p; ++j) {
      const atx::f64 bj_old = b(j);
      // Partial residual r_partial = r + Xs[:,j]*b[j] (coordinate j removed).
      // rho_j = (1/n) Xs[:,j] . r_partial = (1/n) Xs[:,j].r + col_m2[j]*b[j].
      const atx::f64 rho = Xs.col(j).dot(r) / nf + col_m2(j) * bj_old;
      const atx::f64 denom = col_m2(j) + l2;
      const atx::f64 bj_new = (denom > 0.0) ? detail::soft_threshold(rho, l1) / denom : 0.0;
      const atx::f64 delta = bj_new - bj_old;
      if (delta != 0.0) {
        // Maintain r incrementally: r -= Xs[:,j]*delta keeps r == y - Xs*b.
        r.noalias() -= Xs.col(j) * delta;
        b(j) = bj_new;
        const atx::f64 ad = std::fabs(delta);
        if (ad > max_delta) {
          max_delta = ad;
        }
      }
    }
    if (max_delta < c.tol) {
      break; // converged: no coordinate moved more than tol this sweep
    }
  }
  return b;
}

} // namespace atx::engine::learn
