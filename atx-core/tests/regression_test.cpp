// regression_test.cpp — TDD tests for atx::core::linalg regression (OLS/ridge/WLS).
//
// Order: seed tests (from spec) first, then extras covering perfect-fit
// residuals, noisy r2 in (0,1), WLS equivalence/weighting, ridge↔OLS limits,
// and the error paths (rank deficiency, negative lambda, dimension mismatch,
// degenerate mean-only target).
//
// Known-value assertions are anchored to closed-form facts: the seed OLS case is
// the line y = 1 + 2x sampled exactly, so beta = (1, 2) and r2 = 1 to machine
// precision.

#include <atx/core/linalg/regression.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>
#include <atx/core/linalg/linalg.hpp>

using namespace atx::core::linalg;

// ============================================================
// Seed tests (from spec)
// ============================================================

TEST(Regression, OlsRecoversKnownCoeffs) {
    MatX X(4, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 4;
    VecX y(4);
    y << 3, 5, 7, 9;
    auto r = ols(X, y);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->beta[0], 1.0, 1e-9);
    EXPECT_NEAR(r->beta[1], 2.0, 1e-9);
    EXPECT_NEAR(r->r2, 1.0, 1e-9);
}

TEST(Regression, RankDeficientErrs) {
    MatX X(2, 2);
    X << 1, 1, 1, 1;
    VecX y(2);
    y << 1, 2;
    EXPECT_FALSE(ols(X, y).has_value());
}

TEST(Regression, RidgeShrinks) {
    MatX X(3, 1);
    X << 1, 2, 3;
    VecX y(3);
    y << 2, 4, 6;
    auto r = ridge(X, y, 10.0);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(r->beta[0], 2.0);
}

// ============================================================
// Extras — OLS
// ============================================================

// The perfect-fit case has residuals that vanish to machine precision.
TEST(Regression, OlsPerfectFitResidualsNearZero) {
    MatX X(4, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 4;
    VecX y(4);
    y << 3, 5, 7, 9;
    auto r = ols(X, y);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->residuals.size(), 4);
    EXPECT_LT(r->residuals.cwiseAbs().maxCoeff(), 1e-9);
}

// A noisy target produces a fit that explains some but not all variance, so r2
// lands strictly inside (0, 1).
TEST(Regression, OlsNoisyFitR2InUnitInterval) {
    MatX X(5, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 4, 1, 5;
    VecX y(5);
    y << 2.1, 3.9, 6.2, 7.8, 10.3; // ~2x + noise
    auto r = ols(X, y);
    ASSERT_TRUE(r.has_value());
    EXPECT_GT(r->r2, 0.0);
    EXPECT_LT(r->r2, 1.0);
}

// rows < cols is underdetermined → error, independent of the values.
TEST(Regression, OlsUnderdeterminedErrs) {
    MatX X(2, 3);
    X << 1, 2, 3, 4, 5, 6;
    VecX y(2);
    y << 1, 2;
    EXPECT_FALSE(ols(X, y).has_value());
}

// X.rows() must match y.size().
TEST(Regression, OlsDimensionMismatchErrs) {
    MatX X(4, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 4;
    VecX y(3);
    y << 1, 2, 3;
    EXPECT_FALSE(ols(X, y).has_value());
}

// A constant target (SS_tot == 0) that is fit exactly reports r2 = 1.
TEST(Regression, OlsConstantTargetExactFitR2IsOne) {
    MatX X(3, 1);
    X << 1, 1, 1; // intercept-only design
    VecX y(3);
    y << 5, 5, 5;
    auto r = ols(X, y);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->beta[0], 5.0, 1e-12);
    EXPECT_NEAR(r->r2, 1.0, 1e-12);
}

// ============================================================
// Extras — ridge
// ============================================================

// As lambda → 0 ridge converges to the OLS solution.
TEST(Regression, RidgeLambdaZeroMatchesOls) {
    MatX X(4, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 5;
    VecX y(4);
    y << 2, 3, 5, 8;
    auto o = ols(X, y);
    auto r = ridge(X, y, 0.0);
    ASSERT_TRUE(o.has_value());
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->beta[0], o->beta[0], 1e-9);
    EXPECT_NEAR(r->beta[1], o->beta[1], 1e-9);
}

// Negative penalty is a precondition violation → InvalidArgument.
TEST(Regression, RidgeNegativeLambdaErrs) {
    MatX X(3, 1);
    X << 1, 2, 3;
    VecX y(3);
    y << 2, 4, 6;
    EXPECT_FALSE(ridge(X, y, -1.0).has_value());
}

// With lambda > 0 a rank-deficient design is still solvable (SPD system).
TEST(Regression, RidgeSolvesRankDeficientDesign) {
    MatX X(2, 2);
    X << 1, 1, 1, 1; // rank 1 — OLS would reject this
    VecX y(2);
    y << 1, 2;
    auto r = ridge(X, y, 1.0);
    EXPECT_TRUE(r.has_value());
}

// ============================================================
// Extras — WLS
// ============================================================

// Uniform weights make WLS identical to OLS.
TEST(Regression, WlsUniformWeightsEqualsOls) {
    MatX X(4, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 4;
    VecX y(4);
    y << 3, 5, 7, 9;
    VecX w = VecX::Constant(4, 1.0);
    auto o = ols(X, y);
    auto r = wls(X, y, w);
    ASSERT_TRUE(o.has_value());
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->beta[0], o->beta[0], 1e-9);
    EXPECT_NEAR(r->beta[1], o->beta[1], 1e-9);
}

// WLS recovers exact coefficients on a perfect-fit case regardless of weights.
TEST(Regression, WlsRecoversKnownCoeffs) {
    MatX X(4, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 4;
    VecX y(4);
    y << 3, 5, 7, 9;
    VecX w(4);
    w << 0.5, 2.0, 1.0, 3.0;
    auto r = wls(X, y, w);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->beta[0], 1.0, 1e-9);
    EXPECT_NEAR(r->beta[1], 2.0, 1e-9);
}

// A near-zero weight on an outlier de-emphasizes it: the WLS slope tracks the
// well-weighted points far more closely than OLS does.
TEST(Regression, WlsNearZeroWeightDeemphasizesOutlier) {
    MatX X(4, 2);
    X << 1, 1, 1, 2, 1, 3, 1, 4;
    VecX y(4);
    y << 2, 4, 6, 100; // last point is an outlier off the y = 2x line
    VecX w(4);
    w << 1.0, 1.0, 1.0, 1e-9; // all-but-ignore the outlier
    auto wfit = wls(X, y, w);
    auto ofit = ols(X, y);
    ASSERT_TRUE(wfit.has_value());
    ASSERT_TRUE(ofit.has_value());
    // True slope on the kept points is 2; WLS should sit far nearer to it.
    EXPECT_NEAR(wfit->beta[1], 2.0, 1e-3);
    EXPECT_GT(std::abs(ofit->beta[1] - 2.0), std::abs(wfit->beta[1] - 2.0));
}

// Negative weights are invalid.
TEST(Regression, WlsNegativeWeightErrs) {
    MatX X(3, 2);
    X << 1, 1, 1, 2, 1, 3;
    VecX y(3);
    y << 1, 2, 3;
    VecX w(3);
    w << 1.0, -1.0, 1.0;
    EXPECT_FALSE(wls(X, y, w).has_value());
}

// Weight vector length must match the number of observations.
TEST(Regression, WlsWeightSizeMismatchErrs) {
    MatX X(3, 2);
    X << 1, 1, 1, 2, 1, 3;
    VecX y(3);
    y << 1, 2, 3;
    VecX w(2);
    w << 1.0, 1.0;
    EXPECT_FALSE(wls(X, y, w).has_value());
}

// A rank-deficient weighted system is still rejected.
TEST(Regression, WlsRankDeficientErrs) {
    MatX X(3, 2);
    X << 1, 1, 1, 1, 1, 1; // both columns identical → rank 1
    VecX y(3);
    y << 1, 2, 3;
    VecX w = VecX::Constant(3, 1.0);
    EXPECT_FALSE(wls(X, y, w).has_value());
}
