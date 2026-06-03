#pragma once

// atx::core::linalg — matrix decompositions with owned results and Result<>
// failure paths.
//
// Each wrapper picks the Eigen algorithm suited to the factorization, copies the
// factors out into owned MatX/VecX (so the result outlives any temporary input),
// and reports structural / numerical failure through Result<T> rather than by
// exception (agent profile §4). Only std::bad_alloc from Eigen propagates, so
// these functions are noexcept(false).
//
//   cholesky(A)       A = L·Lᵀ        SPD only; LLT. Internal if not PD.
//   qr(A)             A = Q·R         thin HouseholderQR.
//   svd(A)            A = U·Σ·Vᵀ      thin BDCSVD; singular values descending.
//   symmetric_eig(A)  A = V·Λ·Vᵀ      SelfAdjointEigenSolver; eigenvalues ascending.
//
// Conventions match the rest of linalg: scalar f64, column-major MatX/VecX.
// symmetric_eig is the shared substrate for spd.hpp and pca.hpp.

#include <algorithm> // std::min
#include <utility>   // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, i64

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

namespace atx::core::linalg {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// A = L·Lᵀ, with L lower-triangular.
struct CholeskyResult {
  MatX L;
};

// A = Q·R, thin: Q has orthonormal columns, R is upper-triangular.
struct QrResult {
  MatX Q;
  MatX R;
};

// A = U·diag(singular)·Vᵀ, thin. singular is sorted descending, all >= 0.
struct SvdResult {
  MatX U;
  VecX singular;
  MatX V;
};

// A = vectors·diag(values)·vectorsᵀ for symmetric A. values ascending, real;
// each column of vectors is a unit eigenvector.
struct EigResult {
  VecX values;
  MatX vectors;
};

namespace detail {

// Square and equal to its transpose within a relative tolerance. Shared by
// symmetric_eig's guard and spd::is_symmetric.
[[nodiscard]] inline bool is_symmetric_within(const MatX &A, f64 tol) {
  if (A.rows() != A.cols() || A.rows() == 0) {
    return false;
  }
  const f64 scale = A.cwiseAbs().maxCoeff();
  const f64 threshold = tol * (scale > 0.0 ? scale : 1.0);
  return (A - A.transpose()).cwiseAbs().maxCoeff() <= threshold;
}

// Default relative tolerance for treating a matrix as symmetric.
inline constexpr f64 kSymmetryTol = 1e-9;

} // namespace detail

// Cholesky factor of a symmetric positive-definite matrix. Rejects non-square or
// non-symmetric input (InvalidArgument) and a non-positive-definite matrix
// (Internal, from the LLT numerical check). Returns the lower factor L.
[[nodiscard]] inline Result<CholeskyResult> cholesky(const MatX &A) {
  if (!detail::is_symmetric_within(A, detail::kSymmetryTol)) {
    return Err(ErrorCode::InvalidArgument, "cholesky: A must be square and symmetric");
  }
  Eigen::LLT<MatX> llt(A);
  if (llt.info() != Eigen::Success) {
    return Err(ErrorCode::Internal, "cholesky: matrix is not positive-definite");
  }
  return Ok(CholeskyResult{MatX(llt.matrixL())});
}

// Thin QR factorization A = Q·R via Householder reflections. Q has min(rows,cols)
// orthonormal columns; R is the corresponding upper-triangular factor. Valid for
// any non-empty matrix.
[[nodiscard]] inline Result<QrResult> qr(const MatX &A) {
  if (A.rows() == 0 || A.cols() == 0) {
    return Err(ErrorCode::InvalidArgument, "qr: matrix must be non-empty");
  }
  const Eigen::Index m = A.rows();
  const Eigen::Index n = A.cols();
  const Eigen::Index k = std::min(m, n);
  Eigen::HouseholderQR<MatX> hh(A);
  MatX Q = hh.householderQ() * MatX::Identity(m, k);
  MatX R = hh.matrixQR().topRows(k).triangularView<Eigen::Upper>();
  return Ok(QrResult{std::move(Q), std::move(R)});
}

// Thin singular value decomposition via divide-and-conquer BDCSVD. Singular
// values are returned descending and non-negative; U and V hold the
// corresponding left/right singular vectors. Valid for any non-empty matrix.
[[nodiscard]] inline Result<SvdResult> svd(const MatX &A) {
  if (A.rows() == 0 || A.cols() == 0) {
    return Err(ErrorCode::InvalidArgument, "svd: matrix must be non-empty");
  }
  Eigen::BDCSVD<MatX> bdc(A, Eigen::ComputeThinU | Eigen::ComputeThinV);
  if (bdc.info() != Eigen::Success) {
    return Err(ErrorCode::Internal, "svd: decomposition did not converge");
  }
  return Ok(SvdResult{MatX(bdc.matrixU()), VecX(bdc.singularValues()), MatX(bdc.matrixV())});
}

// Eigendecomposition of a symmetric matrix via SelfAdjointEigenSolver. Returns
// real eigenvalues ascending and orthonormal eigenvectors. Rejects non-symmetric
// input (InvalidArgument) and reports solver non-convergence (Internal).
[[nodiscard]] inline Result<EigResult> symmetric_eig(const MatX &A) {
  if (!detail::is_symmetric_within(A, detail::kSymmetryTol)) {
    return Err(ErrorCode::InvalidArgument, "symmetric_eig: A must be square and symmetric");
  }
  Eigen::SelfAdjointEigenSolver<MatX> es(A);
  if (es.info() != Eigen::Success) {
    return Err(ErrorCode::Internal, "symmetric_eig: eigensolver did not converge");
  }
  return Ok(EigResult{VecX(es.eigenvalues()), MatX(es.eigenvectors())});
}

} // namespace atx::core::linalg
