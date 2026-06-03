#pragma once

// atx::core::linalg — principal component analysis.
//
// PCA over the covariance of a data matrix X laid out as n_samples × n_features
// (rows are observations, matching the regression design-matrix convention).
// pca() mean-centers the columns, forms the sample covariance, and takes its
// symmetric eigendecomposition; components are returned in descending-variance
// order with the corresponding eigenvalues (explained variance) and their share
// of the total (explained ratio). transform() projects new observations onto the
// fitted components.
//
// The covariance route uses SelfAdjointEigenSolver (via symmetric_eig), which is
// the fast, numerically sound path when the feature count is modest — the common
// case for statistical factor models. Eigen returns eigenpairs ascending; we
// reverse once so the leading component carries the most variance.
//
// Failures (too few samples, k beyond the feature count, shape mismatch in
// transform) travel in Result<T>; scalar f64, column-major MatX/VecX throughout.

#include <utility> // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, i64

#include "atx/core/linalg/decompose.hpp" // symmetric_eig
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX

namespace atx::core::linalg {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// Fitted PCA model. components has one unit eigenvector per column (n_features
// rows, k columns) ordered by descending explained_variance; explained_ratio is
// explained_variance normalized by the total variance.
struct PcaResult {
  VecX mean;
  MatX components;
  VecX explained_variance;
  VecX explained_ratio;
};

// Fit PCA on X (n_samples × n_features). With k <= 0 all components are kept;
// otherwise the leading k. Rejects fewer than two samples (variance undefined),
// an empty feature set, or k exceeding the feature count (InvalidArgument).
[[nodiscard]] inline Result<PcaResult> pca(const MatX &X, i64 k = -1) {
  const Eigen::Index n = X.rows();
  const Eigen::Index p = X.cols();
  if (n < 2) {
    return Err(ErrorCode::InvalidArgument, "pca: need at least 2 samples");
  }
  if (p == 0) {
    return Err(ErrorCode::InvalidArgument, "pca: matrix must have at least one feature");
  }
  if (k > static_cast<i64>(p)) {
    return Err(ErrorCode::InvalidArgument, "pca: k must not exceed the feature count");
  }
  const Eigen::Index keep = (k <= 0) ? p : static_cast<Eigen::Index>(k);

  const VecX mean = X.colwise().mean();
  const MatX centered = X.rowwise() - mean.transpose();
  const MatX cov = (centered.transpose() * centered) / static_cast<f64>(n - 1);

  auto eig = symmetric_eig(cov); // eigenpairs ascending
  if (!eig.has_value()) {
    return Err(eig.error().code(), "pca: covariance eigendecomposition failed");
  }

  // Reverse ascending eigenpairs into descending-variance order.
  const Eigen::Index full = eig->values.size();
  VecX var_desc(full);
  MatX comp_desc(p, full);
  for (Eigen::Index i = 0; i < full; ++i) {
    var_desc[i] = eig->values[full - 1 - i];
    comp_desc.col(i) = eig->vectors.col(full - 1 - i);
  }
  const f64 total = var_desc.sum();

  PcaResult out;
  out.mean = mean;
  out.components = comp_desc.leftCols(keep);
  out.explained_variance = var_desc.head(keep);
  VecX ratio = var_desc.head(keep);
  if (total > 0.0) {
    ratio /= total;
  } else {
    ratio.setZero(); // degenerate (constant) data: no variance to attribute
  }
  out.explained_ratio = std::move(ratio);
  return Ok(std::move(out));
}

// Project observations X (n_samples × n_features) onto the fitted components:
// (X − mean)·components, yielding n_samples × k scores. The feature count must
// match the fitted model.
[[nodiscard]] inline Result<MatX> transform(const PcaResult &model, const MatX &X) {
  if (X.cols() != model.mean.size()) {
    return Err(ErrorCode::InvalidArgument, "transform: feature count must match the fitted model");
  }
  const MatX centered = X.rowwise() - model.mean.transpose();
  return Ok(MatX(centered * model.components));
}

} // namespace atx::core::linalg
