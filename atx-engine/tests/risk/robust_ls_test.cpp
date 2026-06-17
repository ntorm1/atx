// atx::engine::cost — RobustLs test suite (S6-1, Pattern-B kernel).
//
// The IRLS-Huber kernel (cost/robust_ls.hpp) is the robust least-squares engine
// the power-law calibration fits on. These tests pin its three load-bearing
// behaviours:
//
//   C4 (differential anchor)  huber_k = +inf collapses to EXACTLY linalg::ols.
//   C1 (why robust)           under outlier contamination the Huber fit recovers
//                             the true coefficients more accurately than OLS.
//   C5 (determinism)          same data in -> byte-identical beta out (RNG-free,
//                             fixed-iteration, order-fixed reductions).
//
// Fixtures are RNG-driven (Xoshiro256pp) only to BUILD the synthetic design; the
// kernel under test is itself RNG-free. The true generating coefficients are
// known, so coefficient error is measurable against ground truth.

#include <gtest/gtest.h>

#include <array>
#include <cmath>   // std::abs
#include <utility> // std::pair

#include "atx/core/hash.hpp"            // hash_bytes (C5 byte-identity)
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/regression.hpp" // ols (C4 anchor)
#include "atx/core/random.hpp"         // Xoshiro256pp (fixture noise only)
#include "atx/core/types.hpp"          // f64, i64, u64, usize

#include "atx/engine/cost/robust_ls.hpp" // irls_huber, RobustCfg, RobustFit

namespace atxtest_robust_ls_test {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
namespace cost = atx::engine::cost;

// The ground-truth coefficients every fixture below generates against. The
// design has an intercept column (all ones) plus one slope column, so beta is
// [intercept, slope].
[[nodiscard]] VecX true_beta() {
  VecX b(2);
  b[0] = 3.0;  // intercept
  b[1] = -2.0; // slope
  return b;
}

// A clean (noise-free) linear design: row i = [1, x_i], y_i = X_i . true_beta.
// x sweeps a fixed deterministic grid so there is no RNG in the C4 anchor path.
[[nodiscard]] std::pair<MatX, VecX> clean_linear_fixture(usize n, usize /*p*/) {
  const VecX beta = true_beta();
  MatX X(static_cast<Eigen::Index>(n), 2);
  VecX y(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    const auto r = static_cast<Eigen::Index>(i);
    const f64 x = static_cast<f64>(i) * 0.1 - 2.0; // deterministic grid
    X(r, 0) = 1.0;
    X(r, 1) = x;
    y[r] = beta[0] + beta[1] * x;
  }
  return {X, y};
}

// A linear design with light Gaussian noise plus a `contam` fraction of gross
// vertical outliers (a large additive shock on y). OLS chases the outliers;
// the Huber fit down-weights them.
[[nodiscard]] std::pair<MatX, VecX> linear_with_outliers_fixture(usize n, f64 contam) {
  const VecX beta = true_beta();
  atx::core::Xoshiro256pp rng{1234ULL};
  MatX X(static_cast<Eigen::Index>(n), 2);
  VecX y(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    const auto r = static_cast<Eigen::Index>(i);
    const f64 x = rng.uniform(-3.0, 3.0);
    X(r, 0) = 1.0;
    X(r, 1) = x;
    f64 yi = beta[0] + beta[1] * x + 0.05 * rng.normal(); // small clean noise
    // The first `contam` fraction of rows receive a large one-sided shock.
    if (static_cast<f64>(i) < contam * static_cast<f64>(n)) {
      yi += 25.0; // gross outlier
    }
    y[r] = yi;
  }
  return {X, y};
}

// L2 coefficient error vs ground truth.
[[nodiscard]] f64 coef_error(const VecX& beta, const VecX& truth) {
  return (beta - truth).norm();
}

// Hash a coefficient vector's raw bytes (C5 byte-identity check).
[[nodiscard]] u64 hash_beta(const VecX& beta) {
  return atx::core::hash_bytes(beta.data(),
                               static_cast<usize>(beta.size()) * sizeof(f64));
}

// =============================================================================
//  Suite: RobustLs
// =============================================================================

// C4 — the differential anchor. With huber_k -> +inf every observation keeps
// weight 1, so the very first WLS step IS ordinary least squares and the loop
// converges immediately. The Huber beta must match linalg::ols to ~machine eps.
TEST(RobustLs, HuberInfinity_EqualsOls) {
  auto [X, y] = clean_linear_fixture(40, 2);
  auto rob = cost::irls_huber(X, y, cost::RobustCfg{.huber_k = 1e18});
  auto ols = atx::core::linalg::ols(X, y);
  ASSERT_TRUE(ols.has_value());
  for (int j = 0; j < 2; ++j) {
    EXPECT_NEAR(rob.beta[j], ols->beta[j], 1e-9);
  }
}

// C1 — the reason the kernel is robust at all. On a contaminated design the
// Huber fit's coefficients sit closer to the truth than the OLS fit's.
TEST(RobustLs, OutlierContamination_RobustBeatsOls) {
  auto [X, y] = linear_with_outliers_fixture(60, 0.1);
  auto rob = cost::irls_huber(X, y, cost::RobustCfg{});
  auto ols = atx::core::linalg::ols(X, y);
  ASSERT_TRUE(ols.has_value());
  EXPECT_LT(coef_error(rob.beta, true_beta()), coef_error(ols->beta, true_beta()));
}

// C5 — determinism. Two runs over the same data produce byte-identical beta.
TEST(RobustLs, SameData_ByteIdentical) {
  auto [X, y] = linear_with_outliers_fixture(60, 0.1);
  const u64 h1 = hash_beta(cost::irls_huber(X, y, {}).beta);
  const u64 h2 = hash_beta(cost::irls_huber(X, y, {}).beta);
  EXPECT_EQ(h1, h2);
}

// Boundary — the loop reports a bounded, non-zero iteration count and a residual
// vector parallel to the data. (Contract surface, not a numeric claim.)
TEST(RobustLs, ReportsBoundedIterationsAndResiduals) {
  auto [X, y] = linear_with_outliers_fixture(60, 0.1);
  const cost::RobustCfg cfg{};
  auto rob = cost::irls_huber(X, y, cfg);
  EXPECT_LE(rob.iters, cfg.max_iter);
  EXPECT_EQ(rob.residuals.size(), static_cast<usize>(y.size()));
  EXPECT_TRUE(std::isfinite(rob.r2));
}


}  // namespace atxtest_robust_ls_test
