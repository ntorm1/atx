#pragma once

// atx::core::linalg — linear regression over the typed Eigen bridge.
//
// Three estimators, all returning the same OlsResult (coefficients, R², and the
// per-observation residuals y - X·beta):
//
//   ols(X, y)         ordinary least squares, beta = argmin ‖X·beta − y‖₂.
//   ridge(X, y, λ)    L2-penalized, beta = (XᵀX + λI)⁻¹ Xᵀy.
//   wls(X, y, w)      weighted least squares, minimizing Σ wᵢ (yᵢ − Xᵢ·beta)².
//
// Design notes:
//   - Scalar type is f64 throughout (the linalg convention). These take concrete
//     MatX/VecX, so the bodies are plain `inline` functions, not templates.
//   - Solver: column-pivoting Householder QR (ColPivHouseholderQR). It is
//     rank-revealing, so we can reject a rank-deficient design explicitly rather
//     than silently returning a least-norm / garbage solution. Ridge with λ > 0
//     forms an SPD normal-equation system instead and uses LLᵀ (Cholesky).
//   - Expected failures (bad shapes, rank deficiency, invalid penalties/weights)
//     travel in Result<T>, never via exceptions (agent profile §4). Eigen may
//     still throw std::bad_alloc on allocation; that is genuinely exceptional and
//     left to propagate — these functions are therefore noexcept(false).
//
// R² convention: r2 = 1 − SS_res / SS_tot, with SS_tot = Σ (yᵢ − mean(y))².
// When SS_tot == 0 (a constant target) the ratio is undefined; we report r2 = 1
// if the fit is also exact (SS_res == 0) and r2 = 0 otherwise.

#include <utility> // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp"  // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp"  // f64

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

namespace atx::core::linalg {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// Coefficients of a fitted linear model, the coefficient of determination, and
// the unweighted residual vector y - X·beta.
struct OlsResult {
  VecX beta;
  f64 r2{0.0};
  VecX residuals;
};

namespace detail {

// Build an OlsResult from a design, target and solved coefficients: compute the
// (unweighted) residuals and R². Factored out so each estimator stays short and
// shares one definition of the R² convention documented above.
[[nodiscard]] inline OlsResult make_result(const MatX &X, const VecX &y, VecX beta) {
  VecX residuals = y - X * beta;
  const f64 ss_res = residuals.squaredNorm();
  const f64 mean = y.mean();
  const f64 ss_tot = (y.array() - mean).matrix().squaredNorm();
  // SS_tot == 0 ⇒ constant target: R² is 1 for an exact fit, else 0 (see header).
  const f64 r2 = (ss_tot == 0.0) ? (ss_res == 0.0 ? 1.0 : 0.0) : (1.0 - ss_res / ss_tot);
  return OlsResult{std::move(beta), r2, std::move(residuals)};
}

} // namespace detail

// Ordinary least squares. Rejects a design that is not full column rank:
// X.rows() != y.size(), fewer rows than columns, or a numerically rank-deficient
// column set all return InvalidArgument. On success beta is the unique minimizer.
[[nodiscard]] inline Result<OlsResult> ols(const MatX &X, const VecX &y) {
  if (X.rows() != y.size()) {
    return Err(ErrorCode::InvalidArgument, "ols: X.rows() must equal y.size()");
  }
  if (X.rows() < X.cols()) {
    return Err(ErrorCode::InvalidArgument, "ols: underdetermined (rows < cols)");
  }
  Eigen::ColPivHouseholderQR<MatX> qr(X);
  if (qr.rank() < X.cols()) {
    return Err(ErrorCode::InvalidArgument, "ols: rank-deficient design");
  }
  VecX beta = qr.solve(y);
  return Ok(detail::make_result(X, y, std::move(beta)));
}

// Ridge regression: beta = (XᵀX + λI)⁻¹ Xᵀy. Requires λ >= 0; a negative penalty
// returns InvalidArgument. With λ > 0 the normal-equation matrix is symmetric
// positive-definite and always solvable, so even a rank-deficient X is accepted.
[[nodiscard]] inline Result<OlsResult> ridge(const MatX &X, const VecX &y, f64 lambda) {
  if (X.rows() != y.size()) {
    return Err(ErrorCode::InvalidArgument, "ridge: X.rows() must equal y.size()");
  }
  if (lambda < 0.0) {
    return Err(ErrorCode::InvalidArgument, "ridge: lambda must be non-negative");
  }
  MatX normal = X.transpose() * X;
  normal.diagonal().array() += lambda;
  Eigen::LLT<MatX> llt(normal);
  if (llt.info() != Eigen::Success) {
    // SPD factorization can only fail when lambda == 0 and XᵀX is singular.
    return Err(ErrorCode::InvalidArgument, "ridge: singular system (rank-deficient, lambda=0)");
  }
  VecX beta = llt.solve(X.transpose() * y);
  return Ok(detail::make_result(X, y, std::move(beta)));
}

// Weighted least squares with per-observation weights w (>= 0, one per row).
// Minimizes Σ wᵢ (yᵢ − Xᵢ·beta)² by scaling each row by sqrt(wᵢ) and solving the
// resulting OLS problem. Rank deficiency, bad shapes, or any negative weight
// return InvalidArgument. Reported residuals and R² use the *unweighted* fit.
[[nodiscard]] inline Result<OlsResult> wls(const MatX &X, const VecX &y, const VecX &w) {
  if (X.rows() != y.size()) {
    return Err(ErrorCode::InvalidArgument, "wls: X.rows() must equal y.size()");
  }
  if (w.size() != X.rows()) {
    return Err(ErrorCode::InvalidArgument, "wls: w.size() must equal X.rows()");
  }
  if ((w.array() < 0.0).any()) {
    return Err(ErrorCode::InvalidArgument, "wls: weights must be non-negative");
  }
  const VecX s = w.array().sqrt(); // per-row sqrt(weight)
  const MatX Xw = s.asDiagonal() * X;
  const VecX yw = s.asDiagonal() * y;
  Eigen::ColPivHouseholderQR<MatX> qr(Xw);
  if (qr.rank() < X.cols()) {
    return Err(ErrorCode::InvalidArgument, "wls: rank-deficient weighted design");
  }
  VecX beta = qr.solve(yw);
  return Ok(detail::make_result(X, y, std::move(beta)));
}

} // namespace atx::core::linalg
