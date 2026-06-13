#pragma once

// atx::engine::cost — IRLS-Huber robust least squares (Sprint S6-1, Pattern-B).
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  A deterministic Iteratively-Reweighted Least Squares loop with Huber weights,
//  layered over atx::core::linalg::wls. It is the robust regression engine the
//  power-law cost calibration (cost/calibration.hpp) fits on: realized fills are
//  heavy-tailed, so an ordinary least-squares fit of the impact exponent chases
//  outliers. Huber re-weighting bounds each residual's leverage at a tunable knee
//  `huber_k` (1.345 = 95% efficiency under clean Gaussian noise) while keeping
//  full efficiency in the bulk.
//
//  Pattern-B (engine-local primitive first, atx-core L7 lift later): this layer
//  consumes the atx-core regression primitives {ols, wls} and adds ONLY the
//  re-weighting loop. It introduces no new solver and no second cost formula.
//
// ===========================================================================
//  Determinism (the C5 contract)
// ===========================================================================
//  * NO RNG. The loop is a deterministic function of (X, y, cfg).
//  * Fixed iteration bound (cfg.max_iter); the loop ALWAYS terminates.
//  * The robust scale uses an exact order-statistic median (eval::median), so
//    reductions are order-fixed and the result is run-to-run byte-identical.
//  * huber_k = +inf  =>  every weight is 1  =>  the first WLS step is EXACTLY
//    linalg::ols (the C4 differential anchor).
//
// ===========================================================================
//  Failure policy
// ===========================================================================
//  wls/ols may fail only on a rank-deficient or mis-shaped design. The caller
//  (calibration) guards degenerate rows upstream, so a well-formed design always
//  solves; a failure here is a programmer error, surfaced via ATX_CHECK (the
//  deref of the Result lives OUTSIDE the success condition, so CHECK not ASSERT).
//  Eigen may still throw std::bad_alloc — these functions are noexcept(false).

#include <algorithm> // std::max
#include <cmath>     // std::abs, std::isinf
#include <vector>    // std::vector (residual export, MAD scratch)

#include "atx/core/linalg/linalg.hpp"     // MatX, VecX
#include "atx/core/linalg/regression.hpp" // wls, OlsResult, Result
#include "atx/core/macro.hpp"             // ATX_CHECK
#include "atx/core/types.hpp"             // f64, usize

#include "atx/engine/eval/stats_ext.hpp" // eval::median (robust MAD scale)

namespace atx::engine::cost {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

// ---------------------------------------------------------------------------
//  Configuration + result
// ---------------------------------------------------------------------------

/// IRLS-Huber tuning. `huber_k` is the residual knee in robust-scale units:
/// 1.345 gives 95% asymptotic efficiency vs OLS on clean Gaussian noise.
/// huber_k = +inf collapses the fit to ordinary least squares.
struct RobustCfg {
  f64 huber_k = 1.345; // Huber knee (robust-scale units)
  usize max_iter = 50; // hard iteration bound (deterministic termination)
  f64 tol = 1e-10;     // weight-change convergence tolerance
};

/// A fitted robust model: coefficients, R² (of the final UNWEIGHTED residuals),
/// the residual vector, and the number of IRLS iterations actually run.
struct RobustFit {
  VecX beta;
  f64 r2{0.0};
  std::vector<f64> residuals;
  usize iters{0};
};

namespace detail {

// A small positive floor: guards the residual/scale division when a fit is
// (near-)exact and the MAD scale collapses to zero.
inline constexpr f64 kScaleEps = 1e-12;

/// Huber weight for a standardised residual z = r / scale.
/// w(z) = 1 for |z| <= k (full weight in the bulk); k/|z| for |z| > k (the
/// residual's influence is capped at the knee). z == 0 is full weight.
[[nodiscard]] inline f64 huber_weight(f64 z, f64 k) noexcept {
  const f64 az = std::abs(z);
  if (az <= k) {
    return 1.0;
  }
  return k / az;
}

/// Robust scale estimate s = 1.4826 * median(|r|) (the MAD, scaled so it is a
/// consistent estimator of σ under Gaussian noise). Floored at kScaleEps so the
/// subsequent r/s division is always well defined.
[[nodiscard]] inline f64 mad_scale(const VecX &r) {
  std::vector<f64> abs_r(static_cast<usize>(r.size()));
  for (Eigen::Index i = 0; i < r.size(); ++i) {
    abs_r[static_cast<usize>(i)] = std::abs(r[i]);
  }
  const f64 s = 1.4826 * eval::median(abs_r);
  return std::max(s, kScaleEps);
}

} // namespace detail

// ---------------------------------------------------------------------------
//  irls_huber — the kernel
// ---------------------------------------------------------------------------

/// Fit beta minimising the Huber loss via IRLS over linalg::wls.
///
/// @param X       n×p design (full column rank; the caller guarantees this).
/// @param y       length-n target.
/// @param c       tuning (knee, iteration bound, convergence tolerance).
/// @param prior_w OPTIONAL fixed per-observation prior weight w0 (length n).
///                nullptr ⇒ w0 ≡ Ones, which is bit-identical to the original
///                S6-1 behaviour (the RobustLs* suite is unaffected). When
///                supplied, the per-iteration weight is ω_i = w0_i · huber(r_i/s,k)
///                and the convergence test runs on the HUBER factor (not ω_i) so a
///                fixed prior never perturbs the iteration count. S8.1 passes a
///                √-cap / inverse-specific-variance prior here.
/// @return        the converged (or iteration-capped) robust fit.
///
/// @pre  X.rows() == y.size() and X is full column rank. A violated precondition
///       trips ATX_CHECK on the underlying wls Result (fail-loud). If prior_w is
///       non-null it must have length n (debug-checked) and entries >= 0.
/// @note noexcept(false): Eigen may throw std::bad_alloc.
// PATTERN-B: reuse S6-1 cost::irls_huber; atx-core huber_irls is the eventual L7 home.
[[nodiscard]] inline RobustFit irls_huber(const MatX &X, const VecX &y, const RobustCfg &c,
                                          const VecX *prior_w = nullptr) {
  const Eigen::Index n = X.rows();
  ATX_ASSERT(prior_w == nullptr || prior_w->size() == n);
  // Huber factor h_i (starts at 1 ⇒ first WLS step uses w0 directly: OLS when
  // prior_w is Ones — the C4 anchor). The effective WLS weight is ω_i = w0_i·h_i.
  VecX h = VecX::Ones(n);
  VecX w(n);
  VecX r = VecX::Zero(n);

  atx::core::linalg::OlsResult fit;
  usize it = 0;
  for (; it < c.max_iter; ++it) {
    for (Eigen::Index i = 0; i < n; ++i) {
      const f64 w0 = (prior_w == nullptr) ? 1.0 : (*prior_w)[i];
      w[i] = w0 * h[i];
    }
    auto res = atx::core::linalg::wls(X, y, w);
    // SAFETY: the deref is OUTSIDE the success test, so ATX_CHECK (always-on),
    // not ATX_ASSERT — a rank-deficient design is a caller contract violation.
    ATX_CHECK(res.has_value());
    fit = *res;

    r = y - X * fit.beta;
    const f64 s = detail::mad_scale(r);

    f64 max_dh = 0.0;
    for (Eigen::Index i = 0; i < n; ++i) {
      const f64 z = r[i] / s; // s >= kScaleEps, division is safe
      const f64 hi = detail::huber_weight(z, c.huber_k);
      max_dh = std::max(max_dh, std::abs(hi - h[i]));
      h[i] = hi;
    }
    if (max_dh < c.tol) {
      ++it; // count this (converged) iteration
      break;
    }
  }

  std::vector<f64> resid(static_cast<usize>(n));
  for (Eigen::Index i = 0; i < n; ++i) {
    resid[static_cast<usize>(i)] = r[i];
  }
  return RobustFit{fit.beta, fit.r2, std::move(resid), it};
}

} // namespace atx::engine::cost
