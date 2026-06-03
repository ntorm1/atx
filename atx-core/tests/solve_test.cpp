// solve_test.cpp — TDD tests for atx::core::linalg solvers and matrix queries.
//
//   solve / solve_spd   linear systems (LU / Cholesky)
//   inverse             matrix inverse with invertibility gate
//   pseudo_inverse      Moore-Penrose via thin SVD (rectangular / rank-deficient)
//   determinant, rank, condition_number   scalar matrix queries
//
// Anchors are closed-form solutions, round-trip identities, and the
// Moore-Penrose conditions; error paths cover bad shape and singularity.

#include <atx/core/linalg/solve.hpp>

#include <cmath> // std::isinf

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
// solve
// ============================================================

// Closed-form: [[2,1],[1,3]]·x = [3,5] has the unique solution x = [0.8, 1.4].
TEST(Solve, SolveRecoversKnownSolution) {
    MatX A(2, 2);
    A << 2, 1, 1, 3;
    VecX b(2);
    b << 3, 5;
    auto r = solve(A, b);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR((*r)[0], 0.8, 1e-12);
    EXPECT_NEAR((*r)[1], 1.4, 1e-12);
    EXPECT_LT((A * *r - b).norm(), 1e-12);
}

TEST(Solve, SolveRejectsNonSquare) {
    MatX A(3, 2);
    A << 1, 2, 3, 4, 5, 6;
    VecX b(3);
    b << 1, 2, 3;
    EXPECT_FALSE(solve(A, b).has_value());
}

TEST(Solve, SolveRejectsSizeMismatch) {
    MatX A(2, 2);
    A << 1, 0, 0, 1;
    VecX b(3);
    b << 1, 2, 3;
    EXPECT_FALSE(solve(A, b).has_value());
}

// A singular, inconsistent system has no solution → Internal.
TEST(Solve, SolveRejectsSingular) {
    MatX A(2, 2);
    A << 1, 2, 2, 4; // rank 1
    VecX b(2);
    b << 1, 1; // not in the range of A
    auto r = solve(A, b);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::Internal);
}

// ============================================================
// solve_spd
// ============================================================

TEST(Solve, SolveSpdRoundTrips) {
    MatX A(2, 2);
    A << 4, 1, 1, 3;
    VecX b(2);
    b << 1, 2;
    auto r = solve_spd(A, b);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT((A * *r - b).norm(), 1e-12);
}

// A symmetric but indefinite matrix is not PD → Internal.
TEST(Solve, SolveSpdRejectsIndefinite) {
    MatX A(2, 2);
    A << 1, 2, 2, 1; // eigenvalues 3, -1
    VecX b(2);
    b << 1, 1;
    auto r = solve_spd(A, b);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::Internal);
}

// ============================================================
// inverse
// ============================================================

TEST(Solve, InverseRoundTrips) {
    MatX A(2, 2);
    A << 2, 0, 0, 4;
    auto r = inverse(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(max_abs_diff(A * *r, MatX::Identity(2, 2)), 1e-12);
    EXPECT_NEAR((*r)(0, 0), 0.5, 1e-12);
    EXPECT_NEAR((*r)(1, 1), 0.25, 1e-12);
}

TEST(Solve, InverseRejectsSingular) {
    MatX A(2, 2);
    A << 1, 2, 2, 4;
    auto r = inverse(A);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::Internal);
}

// ============================================================
// pseudo_inverse
// ============================================================

// A full-column-rank tall matrix has a left inverse: pinv·A = I.
TEST(Solve, PseudoInverseIsLeftInverse) {
    MatX A(3, 2);
    A << 1, 0, 0, 1, 0, 0;
    auto r = pseudo_inverse(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->rows(), 2);
    EXPECT_EQ(r->cols(), 3);
    EXPECT_LT(max_abs_diff(*r * A, MatX::Identity(2, 2)), 1e-10);
    // Moore-Penrose: A·A⁺·A = A.
    EXPECT_LT(max_abs_diff(A * *r * A, A), 1e-10);
}

// Rank-deficient input still yields a valid Moore-Penrose pseudo-inverse.
TEST(Solve, PseudoInverseHandlesRankDeficient) {
    MatX A(2, 2);
    A << 1, 1, 1, 1; // rank 1
    auto r = pseudo_inverse(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(max_abs_diff(A * *r * A, A), 1e-10); // A A⁺ A = A
}

// ============================================================
// determinant / rank / condition_number
// ============================================================

TEST(Solve, DeterminantKnownValue) {
    MatX A(2, 2);
    A << 1, 2, 3, 4;
    auto r = determinant(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, -2.0, 1e-12);
}

TEST(Solve, DeterminantRejectsNonSquare) {
    MatX A(2, 3);
    A << 1, 2, 3, 4, 5, 6;
    EXPECT_FALSE(determinant(A).has_value());
}

TEST(Solve, RankCountsIndependentColumns) {
    MatX full(2, 2);
    full << 1, 0, 0, 1;
    auto rf = rank(full);
    ASSERT_TRUE(rf.has_value());
    EXPECT_EQ(*rf, 2);

    MatX deficient(2, 2);
    deficient << 1, 2, 2, 4; // rank 1
    auto rd = rank(deficient);
    ASSERT_TRUE(rd.has_value());
    EXPECT_EQ(*rd, 1);
}

TEST(Solve, ConditionNumberIdentityIsOne) {
    auto r = condition_number(MatX::Identity(3, 3));
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 1.0, 1e-12);
}

TEST(Solve, ConditionNumberDiagonalRatio) {
    MatX A(2, 2);
    A << 100, 0, 0, 1;
    auto r = condition_number(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 100.0, 1e-9);
}

// A singular matrix has an infinite condition number — a valid, informative
// answer (Ok), not an error.
TEST(Solve, ConditionNumberSingularIsInfinite) {
    MatX A(2, 2);
    A << 1, 1, 1, 1;
    auto r = condition_number(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(std::isinf(*r));
}
