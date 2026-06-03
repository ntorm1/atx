#pragma once

// atx::core::linalg — linear solvers and scalar matrix queries.
//
// Each operation selects the Eigen decomposition suited to it and reports
// structural / numerical failure through Result<T> (agent profile §4):
//
//   solve(A, b)          square system, partial-pivot LU + residual/finite guard
//   solve_spd(A, b)      SPD fast path via Cholesky (LLT)
//   inverse(A)           inverse, gated by a rank-revealing invertibility check
//   pseudo_inverse(A)    Moore-Penrose via thin SVD (rectangular / rank-deficient)
//   determinant(A)       LU determinant (square)
//   rank(A)              numerical rank via column-pivoting QR
//   condition_number(A)  σ_max / σ_min from SVD (Ok(+inf) when singular)
//
// Shape / precondition violations return InvalidArgument; numerical failure
// (singular solve, non-PD Cholesky) returns Internal. condition_number reports a
// singular matrix as Ok(+inf): an informative answer, not an error. Only
// std::bad_alloc from Eigen propagates, so these functions are noexcept(false).

#include <limits> // std::numeric_limits

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, i64

#include "atx/core/linalg/decompose.hpp" // detail::is_symmetric_within, kSymmetryTol
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX

namespace atx::core::linalg {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// Solve the square system A·x = b via partial-pivot LU. Rejects a non-square A or
// a right-hand side of the wrong length (InvalidArgument). A singular system is
// detected by a non-finite solution or a large relative residual and reported as
// Internal — partial-pivot LU does not reveal rank directly, so the solution is
// validated after the fact.
[[nodiscard]] inline Result<VecX> solve(const MatX &A, const VecX &b) {
  if (A.rows() != A.cols()) {
    return Err(ErrorCode::InvalidArgument, "solve: A must be square");
  }
  if (b.size() != A.rows()) {
    return Err(ErrorCode::InvalidArgument, "solve: b.size() must equal A.rows()");
  }
  const VecX x = Eigen::PartialPivLU<MatX>(A).solve(b);
  const f64 scale = b.norm() > 0.0 ? b.norm() : 1.0;
  if (!x.allFinite() || (A * x - b).norm() > 1e-8 * scale) {
    return Err(ErrorCode::Internal, "solve: singular or inconsistent system");
  }
  return Ok(VecX(x));
}

// Solve a symmetric positive-definite system A·x = b via Cholesky (LLT). Rejects
// a non-symmetric A (InvalidArgument) and a non-positive-definite one (Internal).
[[nodiscard]] inline Result<VecX> solve_spd(const MatX &A, const VecX &b) {
  if (!detail::is_symmetric_within(A, detail::kSymmetryTol)) {
    return Err(ErrorCode::InvalidArgument, "solve_spd: A must be square and symmetric");
  }
  if (b.size() != A.rows()) {
    return Err(ErrorCode::InvalidArgument, "solve_spd: b.size() must equal A.rows()");
  }
  Eigen::LLT<MatX> llt(A);
  if (llt.info() != Eigen::Success) {
    return Err(ErrorCode::Internal, "solve_spd: matrix is not positive-definite");
  }
  return Ok(VecX(llt.solve(b)));
}

// Inverse of a square matrix, gated by a full-pivot LU invertibility test so a
// singular matrix is rejected (Internal) rather than yielding garbage.
[[nodiscard]] inline Result<MatX> inverse(const MatX &A) {
  if (A.rows() != A.cols()) {
    return Err(ErrorCode::InvalidArgument, "inverse: A must be square");
  }
  Eigen::FullPivLU<MatX> lu(A);
  if (!lu.isInvertible()) {
    return Err(ErrorCode::Internal, "inverse: matrix is singular");
  }
  return Ok(MatX(lu.inverse()));
}

// Moore-Penrose pseudo-inverse via thin SVD. Singular values at or below
// max(rows,cols)·eps·σ_max are treated as zero, so rectangular and rank-deficient
// inputs are handled. For an m×n input the result is n×m.
[[nodiscard]] inline Result<MatX> pseudo_inverse(const MatX &A) {
  if (A.rows() == 0 || A.cols() == 0) {
    return Err(ErrorCode::InvalidArgument, "pseudo_inverse: matrix must be non-empty");
  }
  Eigen::BDCSVD<MatX> bdc(A, Eigen::ComputeThinU | Eigen::ComputeThinV);
  if (bdc.info() != Eigen::Success) {
    return Err(ErrorCode::Internal, "pseudo_inverse: SVD did not converge");
  }
  const VecX &s = bdc.singularValues();
  const auto dim = static_cast<f64>(std::max(A.rows(), A.cols()));
  const f64 tol = dim * std::numeric_limits<f64>::epsilon() * (s.size() > 0 ? s[0] : 0.0);
  VecX s_inv(s.size());
  for (Eigen::Index i = 0; i < s.size(); ++i) {
    s_inv[i] = s[i] > tol ? 1.0 / s[i] : 0.0;
  }
  return Ok(MatX(bdc.matrixV() * s_inv.asDiagonal() * bdc.matrixU().transpose()));
}

// Determinant of a square matrix via partial-pivot LU.
[[nodiscard]] inline Result<f64> determinant(const MatX &A) {
  if (A.rows() != A.cols()) {
    return Err(ErrorCode::InvalidArgument, "determinant: A must be square");
  }
  return Ok(f64(Eigen::PartialPivLU<MatX>(A).determinant()));
}

// Numerical rank via column-pivoting Householder QR (rank-revealing), valid for
// any non-empty matrix.
[[nodiscard]] inline Result<i64> rank(const MatX &A) {
  if (A.rows() == 0 || A.cols() == 0) {
    return Err(ErrorCode::InvalidArgument, "rank: matrix must be non-empty");
  }
  return Ok(i64(Eigen::ColPivHouseholderQR<MatX>(A).rank()));
}

// 2-norm condition number σ_max / σ_min from the singular values. A singular
// matrix (σ_min == 0) yields Ok(+inf) — a valid, informative answer.
[[nodiscard]] inline Result<f64> condition_number(const MatX &A) {
  if (A.rows() == 0 || A.cols() == 0) {
    return Err(ErrorCode::InvalidArgument, "condition_number: matrix must be non-empty");
  }
  Eigen::BDCSVD<MatX> bdc(A);
  if (bdc.info() != Eigen::Success) {
    return Err(ErrorCode::Internal, "condition_number: SVD did not converge");
  }
  const VecX &s = bdc.singularValues();
  const f64 smin = s[s.size() - 1];
  if (smin <= 0.0) {
    return Ok(std::numeric_limits<f64>::infinity());
  }
  return Ok(f64(s[0] / smin));
}

} // namespace atx::core::linalg
