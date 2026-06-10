// atx::engine::learn — elastic-net coordinate-descent kernel tests (S5-3).
//
// The Pattern-B edge-1 kernel atx-core lacks (regression::{ols,ridge,wls} are
// L2-only). elastic_net is RNG-free cyclic coordinate descent over a
// column-standardized design, so its two closed-form contracts and its
// determinism can be pinned exactly:
//
//   Suite ElasticNet
//     1. AlphaZero_MatchesAtxCoreRidge (M4) — with alpha == 0 (pure L2) on a
//        STANDARDIZED design, elastic_net must agree with atx-core
//        core::linalg::ridge on the SAME standardized X. The two objectives use
//        different penalty conventions. ridge minimizes ||Xb-y||^2 + L*||b||^2,
//        i.e. its normal system is (X^T X + L*I) b = X^T y. elastic_net (alpha=0)
//        minimizes (1/2n)||Xb-y||^2 + lambda*(1/2)||b||^2, whose stationarity is
//        (1/n)(X^T X) b - (1/n)X^T y + lambda*b = 0, i.e. (X^T X + n*lambda*I) b =
//        X^T y. The two minimizers therefore coincide iff L == n*lambda (the (1/2n)
//        normalization rescales the penalty by n, NOT 2n). The test tunes L that
//        way and asserts agreement ~1e-4.
//     2. OrthonormalX_MatchesSoftThreshold (M4) — with orthonormal columns
//        (X^T X == I) and alpha == 1 (lasso), the coordinate solution is the
//        closed form b_j == soft_threshold(ols_j, n*lambda) (~1e-6). The same
//        (1/2n) normalization makes the effective L1 radius n*lambda: the update
//        is soft((1/n)ols_j, lambda)/(1/n) == soft(ols_j, n*lambda).
//     3. SameInput_Deterministic_ByteIdentical (M1) — two calls on identical
//        input produce bit-identical coefficients (RNG-free + fixed-order cyclic
//        sweep). Pinned by hashing the coefficient f64 bytes.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath> // std::sqrt, std::fabs

#include <Eigen/Dense> // Eigen::Index, MatX/VecX construction in fixtures

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"           // atx::core::hash_bytes
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/regression.hpp" // core::linalg::ridge
#include "atx/core/types.hpp"          // f64, u64, usize

#include "atx/engine/learn/elastic_net.hpp" // elastic_net, ElasticNetCfg

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::learn::ElasticNetCfg;
using atx::engine::learn::elastic_net;

// Column-standardize a design IN PLACE to mean 0 / population-std 1, matching the
// elastic_net precondition (it assumes a standardized Xs). Constant columns are
// left at 0. Returns the standardized copy.
[[nodiscard]] MatX standardize_columns(const MatX &x) {
  MatX xs = x;
  const auto n = xs.rows();
  for (Eigen::Index j = 0; j < xs.cols(); ++j) {
    const f64 mean = xs.col(j).mean();
    xs.col(j).array() -= mean;
    const f64 nf = static_cast<f64>(n);
    f64 sq = 0.0;
    for (Eigen::Index i = 0; i < n; ++i) {
      sq += xs(i, j) * xs(i, j);
    }
    const f64 sd = std::sqrt(sq / nf);
    if (sd > 0.0) {
      xs.col(j).array() /= sd;
    }
  }
  return xs;
}

// Hash a coefficient vector's f64 bit patterns (deterministic within a process).
[[nodiscard]] u64 hash_vec(const VecX &v) {
  // SAFETY: Eigen VecX stores doubles contiguously; .data() points at
  // size()*sizeof(double) live bytes for the vector's lifetime.
  return atx::core::hash_bytes(v.data(), static_cast<usize>(v.size()) * sizeof(f64));
}

// 1. alpha == 0 (pure ridge) on a standardized design matches atx-core ridge with
//    the matched penalty L == n*lambda.
TEST(ElasticNet, AlphaZero_MatchesAtxCoreRidge) {
  const Eigen::Index n = 40;
  const Eigen::Index p = 3;
  MatX x(n, p);
  VecX y(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    const f64 t = static_cast<f64>(i);
    x(i, 0) = t;
    x(i, 1) = std::sqrt(t + 1.0);
    x(i, 2) = (i % 3 == 0) ? 1.0 : -0.5; // a non-collinear third column
    y(i) = 0.7 * t - 0.4 * std::sqrt(t + 1.0) + 0.3;
  }
  const MatX xs = standardize_columns(x);

  const f64 lambda = 0.05;
  ElasticNetCfg cfg{lambda, /*alpha=*/0.0, /*max_iter=*/5000, /*tol=*/1e-12};
  const VecX b_en = elastic_net(xs, y, cfg);

  // Penalty-scaling bridge: elastic_net's stationarity (X^T X + n*lambda*I)b = X^T y
  // matches atx-core ridge's (X^T X + L*I)b = X^T y iff L == n*lambda.
  const f64 ridge_lambda = static_cast<f64>(n) * lambda;
  const auto r = atx::core::linalg::ridge(xs, y, ridge_lambda);
  ASSERT_TRUE(r.has_value());
  const VecX b_ridge = r->beta;

  ASSERT_EQ(b_en.size(), b_ridge.size());
  for (Eigen::Index j = 0; j < b_en.size(); ++j) {
    EXPECT_NEAR(b_en(j), b_ridge(j), 1e-4) << "coefficient " << j;
  }
}

// 2. Orthonormal X (X^T X == I) with alpha == 1 (lasso): b_j == soft_threshold(
//    ols_j, n*lambda). With orthonormal columns ols_j == X[:,j] . y, and the
//    coordinate update reduces to one soft-threshold of that inner product (the
//    (1/2n) objective normalization scales the L1 radius to n*lambda).
TEST(ElasticNet, OrthonormalX_MatchesSoftThreshold) {
  // Build an orthonormal 4x3 design via a thin QR of an arbitrary matrix.
  const Eigen::Index n = 6;
  const Eigen::Index p = 3;
  MatX a(n, p);
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < p; ++j) {
      a(i, j) = static_cast<f64>((i + 1) * (j + 2) % 7) - 3.0;
    }
  }
  const Eigen::HouseholderQR<MatX> qr(a);
  const MatX q = qr.householderQ() * MatX::Identity(n, p); // n x p, orthonormal cols

  VecX y(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    y(i) = static_cast<f64>(i) - 2.5;
  }

  // lambda chosen so the effective L1 radius n*lambda sits among the |ols_j|
  // magnitudes — at least one coefficient survives and at least one may be
  // thresholded, so the soft-threshold is exercised, not trivially all-zero.
  const f64 lambda = 0.05;
  ElasticNetCfg cfg{lambda, /*alpha=*/1.0, /*max_iter=*/5000, /*tol=*/1e-14};
  const VecX b = elastic_net(q, y, cfg);

  // Closed form: with X^T X == I, the OLS coefficient is X[:,j] . y, and the
  // (1/2n)-normalized coordinate update soft-thresholds it at n*lambda.
  auto soft = [](f64 z, f64 g) -> f64 {
    const f64 mag = std::fabs(z) - g;
    if (mag <= 0.0) {
      return 0.0;
    }
    return (z > 0.0 ? 1.0 : -1.0) * mag;
  };
  const f64 radius = static_cast<f64>(n) * lambda;
  bool any_nonzero = false;
  for (Eigen::Index j = 0; j < p; ++j) {
    const f64 ols_j = q.col(j).dot(y);
    EXPECT_NEAR(b(j), soft(ols_j, radius), 1e-6) << "coefficient " << j;
    any_nonzero = any_nonzero || (b(j) != 0.0);
  }
  EXPECT_TRUE(any_nonzero); // non-vacuous: the lasso did not zero everything
}

// 3. Determinism (M1): two identical calls produce bit-identical coefficients.
TEST(ElasticNet, SameInput_Deterministic_ByteIdentical) {
  const Eigen::Index n = 25;
  const Eigen::Index p = 4;
  MatX x(n, p);
  VecX y(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    const f64 t = static_cast<f64>(i);
    x(i, 0) = t;
    x(i, 1) = t * t * 0.01;
    x(i, 2) = (i % 2 == 0) ? 1.0 : 0.0;
    x(i, 3) = std::sqrt(t + 1.0);
    y(i) = 0.2 * t - 0.1 * t * t * 0.01 + 0.5;
  }
  const MatX xs = standardize_columns(x);

  ElasticNetCfg cfg{0.1, 0.5, 2000, 1e-10};
  const VecX b1 = elastic_net(xs, y, cfg);
  const VecX b2 = elastic_net(xs, y, cfg);

  ASSERT_EQ(b1.size(), b2.size());
  EXPECT_EQ(hash_vec(b1), hash_vec(b2));
}

} // namespace
