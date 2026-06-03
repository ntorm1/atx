// pca_test.cpp — TDD tests for atx::core::linalg principal component analysis.
//
// Convention: X is n_samples × n_features (rows = observations). pca() mean-
// centers columns, eigendecomposes the covariance, and returns components in
// descending-variance order with explained variance and ratios. transform()
// projects new rows onto the fitted components.
//
// Anchors: a planted dataset whose variance lies along a known direction; the
// ratio summing to one over all components; and a full-rank reconstruction
// round-trip via transform.

#include <atx/core/linalg/pca.hpp>

#include <cmath> // std::abs

#include <gtest/gtest.h>

#include <atx/core/error.hpp>
#include <atx/core/linalg/linalg.hpp>

using namespace atx::core::linalg;

// ============================================================
// pca — directionality and variance
// ============================================================

// Data spread along the [1,1] diagonal: the top component is ±[1,1]/√2 and
// carries all the variance (the orthogonal direction has none).
TEST(Pca, RecoversDominantDirection) {
    MatX X(5, 2);
    X << -2, -2, -1, -1, 0, 0, 1, 1, 2, 2;
    auto r = pca(X);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->components.cols(), 2);
    // Top eigenvector aligned with [1,1]/√2 (sign is arbitrary).
    EXPECT_NEAR(std::abs(r->components(0, 0)), 0.70710678, 1e-6);
    EXPECT_NEAR(std::abs(r->components(1, 0)), 0.70710678, 1e-6);
    // Variance along the diagonal: cov has eigenvalue 5 there, 0 orthogonal.
    EXPECT_NEAR(r->explained_variance[0], 5.0, 1e-9);
    EXPECT_NEAR(r->explained_variance[1], 0.0, 1e-9);
    EXPECT_NEAR(r->explained_ratio[0], 1.0, 1e-9);
}

// Per-feature mean is reported and components are unit-norm, descending order.
TEST(Pca, MeanAndOrderedVariance) {
    MatX X(4, 2);
    X << 1, 2, 3, 1, 2, 4, 5, 3;
    auto r = pca(X);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->mean[0], 2.75, 1e-12); // (1+3+2+5)/4
    EXPECT_NEAR(r->mean[1], 2.5, 1e-12);  // (2+1+4+3)/4
    // Eigenvalues descending.
    EXPECT_GE(r->explained_variance[0], r->explained_variance[1]);
    // Ratios sum to 1 across all components.
    EXPECT_NEAR(r->explained_ratio.sum(), 1.0, 1e-12);
    // Components are unit vectors.
    EXPECT_NEAR(r->components.col(0).norm(), 1.0, 1e-12);
    EXPECT_NEAR(r->components.col(1).norm(), 1.0, 1e-12);
}

// k selects the top-k components only.
TEST(Pca, TopKSelectsLeadingComponents) {
    MatX X(4, 2);
    X << 1, 2, 3, 1, 2, 4, 5, 3;
    auto r = pca(X, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->components.cols(), 1);
    EXPECT_EQ(r->explained_variance.size(), 1);
}

// ============================================================
// pca — error paths
// ============================================================

// Fewer than two observations: variance is undefined → InvalidArgument.
TEST(Pca, RejectsTooFewSamples) {
    MatX X(1, 2);
    X << 1, 2;
    auto r = pca(X);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// k greater than the feature count is invalid.
TEST(Pca, RejectsKAboveFeatureCount) {
    MatX X(4, 2);
    X << 1, 2, 3, 1, 2, 4, 5, 3;
    EXPECT_FALSE(pca(X, 3).has_value());
}

// ============================================================
// transform
// ============================================================

// Projecting a row equal to the feature mean lands at the origin.
TEST(Pca, TransformOfMeanIsZero) {
    MatX X(4, 2);
    X << 1, 2, 3, 1, 2, 4, 5, 3;
    auto r = pca(X);
    ASSERT_TRUE(r.has_value());
    MatX mean_row(1, 2);
    mean_row << r->mean[0], r->mean[1];
    auto p = transform(*r, mean_row);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->rows(), 1);
    EXPECT_EQ(p->cols(), 2);
    EXPECT_LT(p->cwiseAbs().maxCoeff(), 1e-12);
}

// With all components retained, scores reconstruct the original data:
// X ≈ mean + scores · componentsᵀ.
TEST(Pca, TransformReconstructsWithAllComponents) {
    MatX X(4, 2);
    X << 1, 2, 3, 1, 2, 4, 5, 3;
    auto r = pca(X);
    ASSERT_TRUE(r.has_value());
    auto scores = transform(*r, X);
    ASSERT_TRUE(scores.has_value());
    MatX recon = (*scores) * r->components.transpose();
    recon.rowwise() += r->mean.transpose();
    EXPECT_LT((recon - X).cwiseAbs().maxCoeff(), 1e-10);
}

// transform rejects a feature count that does not match the fitted model.
TEST(Pca, TransformRejectsWrongFeatureCount) {
    MatX X(4, 2);
    X << 1, 2, 3, 1, 2, 4, 5, 3;
    auto r = pca(X);
    ASSERT_TRUE(r.has_value());
    MatX bad(2, 3);
    bad << 1, 2, 3, 4, 5, 6;
    EXPECT_FALSE(transform(*r, bad).has_value());
}
