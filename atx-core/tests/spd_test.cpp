// spd_test.cpp — TDD tests for atx::core::linalg positive-definite hygiene.
//
//   is_symmetric / is_positive_definite   predicates
//   nearest_pd                            Higham eigenvalue-clamp projection
//   regularize                            diagonal jitter A + jitter·I
//
// Anchors: closed-form spectra, the PD-ness of a projected indefinite matrix,
// and the fixed-point property on an already-PD matrix.

#include <atx/core/linalg/spd.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>
#include <atx/core/linalg/linalg.hpp>

using namespace atx::core::linalg;

namespace {

[[nodiscard]] double max_abs_diff(const MatX& lhs, const MatX& rhs) {
    return (lhs - rhs).cwiseAbs().maxCoeff();
}

} // namespace

// ============================================================
// is_symmetric
// ============================================================

TEST(Spd, IsSymmetricTrueForSymmetric) {
    MatX A(2, 2);
    A << 2, 1, 1, 2;
    EXPECT_TRUE(is_symmetric(A));
}

TEST(Spd, IsSymmetricFalseForAsymmetric) {
    MatX A(2, 2);
    A << 1, 2, 3, 4;
    EXPECT_FALSE(is_symmetric(A));
}

TEST(Spd, IsSymmetricFalseForNonSquare) {
    MatX A(2, 3);
    A << 1, 2, 3, 4, 5, 6;
    EXPECT_FALSE(is_symmetric(A));
}

// ============================================================
// is_positive_definite
// ============================================================

TEST(Spd, IsPositiveDefiniteTrueForSpd) {
    MatX A(2, 2);
    A << 2, 1, 1, 2; // eigenvalues 1, 3 > 0
    EXPECT_TRUE(is_positive_definite(A));
}

TEST(Spd, IsPositiveDefiniteFalseForIndefinite) {
    MatX A(2, 2);
    A << 1, 2, 2, 1; // eigenvalues 3, -1
    EXPECT_FALSE(is_positive_definite(A));
}

TEST(Spd, IsPositiveDefiniteFalseForAsymmetric) {
    MatX A(2, 2);
    A << 1, 2, 3, 4;
    EXPECT_FALSE(is_positive_definite(A));
}

// ============================================================
// nearest_pd
// ============================================================

// Projecting an indefinite matrix yields a positive-definite one.
TEST(Spd, NearestPdMakesIndefinitePositiveDefinite) {
    MatX A(2, 2);
    A << 1, 2, 2, 1; // indefinite (eigenvalue -1)
    auto r = nearest_pd(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(is_positive_definite(*r));
    EXPECT_TRUE(is_symmetric(*r));
}

// An already-PD matrix is (numerically) a fixed point of the projection.
TEST(Spd, NearestPdFixedPointOnPd) {
    MatX A(2, 2);
    A << 2, 1, 1, 2; // already PD
    auto r = nearest_pd(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(max_abs_diff(*r, A), 1e-8);
    EXPECT_TRUE(is_positive_definite(*r));
}

TEST(Spd, NearestPdRejectsNonSquare) {
    MatX A(2, 3);
    A << 1, 2, 3, 4, 5, 6;
    EXPECT_FALSE(nearest_pd(A).has_value());
}

// ============================================================
// regularize
// ============================================================

// A + jitter·I shifts the diagonal and leaves off-diagonal entries untouched.
TEST(Spd, RegularizeShiftsDiagonal) {
    MatX A(2, 2);
    A << 2, 1, 1, 2;
    auto r = regularize(A, 1.0);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR((*r)(0, 0), 3.0, 1e-12);
    EXPECT_NEAR((*r)(1, 1), 3.0, 1e-12);
    EXPECT_NEAR((*r)(0, 1), 1.0, 1e-12);
    EXPECT_NEAR((*r)(1, 0), 1.0, 1e-12);
}

TEST(Spd, RegularizeRejectsNegativeJitter) {
    MatX A(2, 2);
    A << 1, 0, 0, 1;
    auto r = regularize(A, -0.5);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Spd, RegularizeRejectsNonSquare) {
    MatX A(2, 3);
    A << 1, 2, 3, 4, 5, 6;
    EXPECT_FALSE(regularize(A, 1.0).has_value());
}
