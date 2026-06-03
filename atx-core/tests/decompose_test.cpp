// decompose_test.cpp — TDD tests for atx::core::linalg matrix decompositions.
//
// Owned-result wrappers over Eigen with Result<> failure paths:
//   cholesky      A = L Lᵀ        (SPD only; Internal if not PD)
//   qr            A = Q R         (thin)
//   svd           A = U Σ Vᵀ      (thin, σ descending)
//   symmetric_eig A = V Λ Vᵀ      (symmetric input; eigenvalues ascending)
//
// Anchors are reconstruction identities plus a couple of closed-form spectra.

#include <atx/core/linalg/decompose.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>
#include <atx/core/linalg/linalg.hpp>

using namespace atx::core::linalg;

namespace {

// Largest absolute entry of (lhs - rhs); a scale-free reconstruction error.
[[nodiscard]] double max_abs_diff(const MatX& lhs, const MatX& rhs) {
    return (lhs - rhs).cwiseAbs().maxCoeff();
}

} // namespace

// ============================================================
// cholesky
// ============================================================

// SPD matrix factorizes and L·Lᵀ reconstructs the input; L is lower-triangular.
TEST(Decompose, CholeskyReconstructsSpd) {
    MatX A(2, 2);
    A << 4, 2, 2, 3;
    auto r = cholesky(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->L(0, 1), 0.0, 1e-12); // upper entry is zero
    EXPECT_LT(max_abs_diff(r->L * r->L.transpose(), A), 1e-12);
}

// An indefinite matrix (eigenvalues 3, -1) is not PD → Internal.
TEST(Decompose, CholeskyRejectsIndefinite) {
    MatX A(2, 2);
    A << 1, 2, 2, 1;
    auto r = cholesky(A);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::Internal);
}

// Non-square input is a shape precondition violation.
TEST(Decompose, CholeskyRejectsNonSquare) {
    MatX A(3, 2);
    A << 1, 2, 3, 4, 5, 6;
    EXPECT_FALSE(cholesky(A).has_value());
}

// ============================================================
// qr
// ============================================================

// Tall matrix: Q·R reconstructs A and Q has orthonormal columns (QᵀQ = I).
TEST(Decompose, QrReconstructsAndOrthonormal) {
    MatX A(3, 2);
    A << 1, 2, 3, 4, 5, 7;
    auto r = qr(A);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(max_abs_diff(r->Q * r->R, A), 1e-10);
    const MatX gram = r->Q.transpose() * r->Q;
    EXPECT_LT(max_abs_diff(gram, MatX::Identity(2, 2)), 1e-10);
}

// ============================================================
// svd
// ============================================================

// U·Σ·Vᵀ reconstructs A and singular values are sorted descending and >= 0.
TEST(Decompose, SvdReconstructsAndSingularDescending) {
    MatX A(3, 2);
    A << 1, 0, 0, 2, 0, 0;
    auto r = svd(A);
    ASSERT_TRUE(r.has_value());
    const MatX recon = r->U * r->singular.asDiagonal() * r->V.transpose();
    EXPECT_LT(max_abs_diff(recon, A), 1e-10);
    ASSERT_EQ(r->singular.size(), 2);
    EXPECT_GE(r->singular[0], r->singular[1]);
    EXPECT_GE(r->singular[1], 0.0);
    // This A has singular values {2, 1}.
    EXPECT_NEAR(r->singular[0], 2.0, 1e-10);
    EXPECT_NEAR(r->singular[1], 1.0, 1e-10);
}

// ============================================================
// symmetric_eig
// ============================================================

// Diagonal matrix: eigenvalues are the diagonal, ascending.
TEST(Decompose, SymmetricEigDiagonalSpectrum) {
    MatX A(2, 2);
    A << 3, 0, 0, 2;
    auto r = symmetric_eig(A);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->values.size(), 2);
    EXPECT_NEAR(r->values[0], 2.0, 1e-12); // ascending
    EXPECT_NEAR(r->values[1], 3.0, 1e-12);
}

// V·Λ·Vᵀ reconstructs a general symmetric matrix; eigenvectors orthonormal.
TEST(Decompose, SymmetricEigReconstructs) {
    MatX A(2, 2);
    A << 2, 1, 1, 2;
    auto r = symmetric_eig(A);
    ASSERT_TRUE(r.has_value());
    const MatX recon = r->vectors * r->values.asDiagonal() * r->vectors.transpose();
    EXPECT_LT(max_abs_diff(recon, A), 1e-12);
    const MatX gram = r->vectors.transpose() * r->vectors;
    EXPECT_LT(max_abs_diff(gram, MatX::Identity(2, 2)), 1e-12);
    // Spectrum of [[2,1],[1,2]] is {1, 3}.
    EXPECT_NEAR(r->values[0], 1.0, 1e-12);
    EXPECT_NEAR(r->values[1], 3.0, 1e-12);
}

// A non-symmetric matrix is rejected before any eigen work.
TEST(Decompose, SymmetricEigRejectsAsymmetric) {
    MatX A(2, 2);
    A << 1, 2, 3, 4;
    auto r = symmetric_eig(A);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}
