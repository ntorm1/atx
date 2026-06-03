#pragma once

// atx::core::linalg — positive-definite matrix hygiene.
//
// Estimated covariance / risk matrices are often only approximately symmetric
// and may pick up small negative eigenvalues from sampling noise, which breaks
// the Cholesky factorizations and inversions that depend on positive
// definiteness. These helpers detect and repair that:
//
//   is_symmetric(A, tol)       square and equal to Aᵀ within a relative tolerance
//   is_positive_definite(A)    symmetric and Cholesky-factorable (cheapest PD test)
//   nearest_pd(A, eps)         closest PD matrix via Higham eigenvalue clamping
//   regularize(A, jitter)      A + jitter·I (diagonal loading)
//
// Predicates return plain bool; the repairing operations return Result<T> with
// InvalidArgument on shape violations. Scalar f64, column-major MatX throughout.

#include <utility> // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64

#include "atx/core/linalg/decompose.hpp" // detail::is_symmetric_within, symmetric_eig
#include "atx/core/linalg/linalg.hpp"    // MatX

namespace atx::core::linalg {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// True when A is square and equals its transpose within a relative tolerance
// (scaled by the largest magnitude entry, so the test is scale-invariant).
[[nodiscard]] inline bool is_symmetric(const MatX &A, f64 tol = detail::kSymmetryTol) {
  return detail::is_symmetric_within(A, tol);
}

// True when A is symmetric and positive-definite. The Cholesky factorization
// succeeds exactly for SPD matrices, so a successful LLT is the test.
[[nodiscard]] inline bool is_positive_definite(const MatX &A) {
  if (!is_symmetric(A)) {
    return false;
  }
  return Eigen::LLT<MatX>(A).info() == Eigen::Success;
}

// Nearest positive-definite matrix to A in Frobenius norm, via Higham's
// eigenvalue-clamp projection: symmetrize, take the symmetric eigendecomposition,
// raise every eigenvalue to at least `eps`, and reassemble. The result is
// symmetric with all eigenvalues >= eps, hence positive-definite. An already-PD
// input is returned essentially unchanged. Rejects non-square input.
[[nodiscard]] inline Result<MatX> nearest_pd(const MatX &A, f64 eps = 1e-8) {
  if (A.rows() != A.cols() || A.rows() == 0) {
    return Err(ErrorCode::InvalidArgument, "nearest_pd: A must be square and non-empty");
  }
  const MatX sym = 0.5 * (A + A.transpose());
  auto eig = symmetric_eig(sym);
  if (!eig.has_value()) {
    return Err(eig.error().code(), "nearest_pd: eigendecomposition failed");
  }
  VecX values = eig->values;
  for (Eigen::Index i = 0; i < values.size(); ++i) {
    values[i] = values[i] < eps ? eps : values[i];
  }
  MatX pd = eig->vectors * values.asDiagonal() * eig->vectors.transpose();
  pd = 0.5 * (pd + pd.transpose()); // scrub round-off asymmetry
  return Ok(std::move(pd));
}

// Diagonal loading: A + jitter·I. Lifts every eigenvalue by `jitter`, the
// cheapest way to push a borderline-singular symmetric matrix away from
// singularity. Requires a square A and jitter >= 0.
[[nodiscard]] inline Result<MatX> regularize(const MatX &A, f64 jitter) {
  if (A.rows() != A.cols() || A.rows() == 0) {
    return Err(ErrorCode::InvalidArgument, "regularize: A must be square and non-empty");
  }
  if (jitter < 0.0) {
    return Err(ErrorCode::InvalidArgument, "regularize: jitter must be non-negative");
  }
  MatX out = A;
  out.diagonal().array() += jitter;
  return Ok(std::move(out));
}

} // namespace atx::core::linalg
