// rmt_clean_test.cpp — TDD tests for atx::core::linalg random-matrix cleaning.
//
//   rmt_clean(C, q, cfg)   Marchenko-Pastur-fit upper edge + eigenvalue clip
//                          (default) or rotationally-invariant shrinkage (RIE).
//
// Anchors: a planted signal-plus-noise correlation matrix (one strong factor on
// top of an MP noise bulk), the trace-preservation invariant of the clip, PSD-ness
// of the repaired matrix, the q->0 near-no-op limit, the RIE q>=1 -> Clip fallback,
// run-to-run byte identity, and a pinned FNV-1a-64 digest of the cleaned matrix.
//
// Determinism note: the fixtures here are built without any RNG. The "noise bulk"
// is a fixed Toeplitz-like correlation whose eigenvalues sit inside the MP support,
// so the expected clipped-count is a deterministic function of the fixture, not a
// statistical expectation.

#include <atx/core/linalg/rmt_clean.hpp>

#include <gtest/gtest.h>

#include <cstring> // std::memcpy
#include <vector>

#include <atx/core/error.hpp>
#include <atx/core/linalg/decompose.hpp> // symmetric_eig (for PSD checks)
#include <atx/core/linalg/linalg.hpp>
#include <atx/core/types.hpp>

using namespace atx::core::linalg;
using atx::f64;
using atx::i64;
using atx::u64;

namespace {

// Smallest eigenvalue of a symmetric matrix; used to assert PSD-ness directly
// rather than trusting the cleaner's own internal repair.
[[nodiscard]] f64 min_eigenvalue(const MatX& A) {
    auto eig = symmetric_eig(A);
    EXPECT_TRUE(eig.has_value());
    return eig->values.minCoeff();
}

// FNV-1a-64 over the raw little-endian double bytes of a matrix, in column-major
// storage order, length-prefixed by (rows, cols). Matches the engine store's
// canonical FNV scheme (kFnvOffset/kFnvPrime) so the digest is byte-stable across
// platforms and runs — the property the digest test pins. Implemented locally
// because atx-core's hash.hpp uses wyhash, which is explicitly NOT cross-platform
// stable.
[[nodiscard]] u64 fnv1a64_matrix(const MatX& A) {
    constexpr u64 kFnvOffset = 1469598103934665603ull;
    constexpr u64 kFnvPrime = 1099511628211ull;
    u64 h = kFnvOffset;
    auto fold_u64 = [&](u64 v) {
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<unsigned char>(v & 0xFFu);
            h *= kFnvPrime;
            v >>= 8;
        }
    };
    fold_u64(static_cast<u64>(A.rows()));
    fold_u64(static_cast<u64>(A.cols()));
    for (Eigen::Index c = 0; c < A.cols(); ++c) {
        for (Eigen::Index r = 0; r < A.rows(); ++r) {
            u64 bits = 0;
            const f64 v = A(r, c);
            std::memcpy(&bits, &v, sizeof(bits));
            fold_u64(bits);
        }
    }
    return h;
}

// Build a deterministic N×N "signal + noise" correlation matrix: an equicorrelation
// background (every off-diagonal == rho) carries one dominant market-factor
// eigenvalue (1 + (N-1)*rho), while the remaining N-1 eigenvalues all equal
// (1 - rho) and form the degenerate noise bulk. Choosing rho so that 1 - rho sits
// inside the MP support makes the bulk clippable and the single factor a clear
// outlier. Unit diagonal by construction.
[[nodiscard]] MatX equicorrelation(Eigen::Index n, f64 rho) {
    MatX C = MatX::Constant(n, n, rho);
    C.diagonal().setOnes();
    return C;
}

} // namespace

// ============================================================
// Validation / error contract
// ============================================================

TEST(RmtClean, RejectsNonSquare) {
    MatX C(2, 3);
    C << 1, 0.2, 0.1, 0.2, 1, 0.3;
    auto r = rmt_clean(C, 0.5);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(RmtClean, RejectsEmpty) {
    MatX C(0, 0);
    auto r = rmt_clean(C, 0.5);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(RmtClean, RejectsNonPositiveQ) {
    MatX C = equicorrelation(4, 0.3);
    auto r = rmt_clean(C, 0.0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ============================================================
// MP-fit clip: removes the bulk, keeps the outlier
// ============================================================

// A 20×20 equicorrelation matrix at rho=0.3 has spectrum {1+19*0.3=6.7} ∪ {0.7}×19.
// With q = 20/100 = 0.2 the MP upper edge for unit-variance noise is (1+√0.2)² ≈ 2.08,
// so the 19 degenerate bulk eigenvalues (0.7) fall below the edge and are clipped,
// while the market factor (6.7) is kept. clipped == N-1.
TEST(RmtClean, ClipRemovesBulkKeepsOutlier) {
    const Eigen::Index n = 20;
    MatX C = equicorrelation(n, 0.3);
    auto r = rmt_clean(C, 0.2);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->clipped, static_cast<i64>(n - 1));
}

// ============================================================
// Trace preservation under clip
// ============================================================

TEST(RmtClean, ClipPreservesTrace) {
    MatX C = equicorrelation(20, 0.3);
    const f64 trace_before = C.trace();
    auto r = rmt_clean(C, 0.2);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->corr.trace(), trace_before, 1e-9);
}

// ============================================================
// PSD repair
// ============================================================

TEST(RmtClean, CleanedMatrixIsPsd) {
    MatX C = equicorrelation(20, 0.3);
    auto r = rmt_clean(C, 0.2);
    ASSERT_TRUE(r.has_value());
    EXPECT_GE(min_eigenvalue(r->corr), -1e-12);
}

TEST(RmtClean, CleanedMatrixHasUnitDiagonal) {
    MatX C = equicorrelation(20, 0.3);
    auto r = rmt_clean(C, 0.2);
    ASSERT_TRUE(r.has_value());
    for (Eigen::Index i = 0; i < r->corr.rows(); ++i) {
        EXPECT_NEAR(r->corr(i, i), 1.0, 1e-12);
    }
}

// ============================================================
// q -> 0 (T >> N): near no-op on an already-clean matrix
// ============================================================

// A matrix that is already "clean" — the identity, whose entire spectrum is the
// single degenerate value 1 — must come back essentially unchanged: clipping a
// flat spectrum to its own mean is the identity map, and the unit-diagonal /
// PSD-repair steps are fixed points of it. With q tiny (T >> N) the cleaner has
// the most data and the least reason to move anything. We assert the cleaned
// matrix stays within tight Frobenius distance of the input — the real "near
// no-op" invariant (the clipped count is not meaningful on a degenerate spectrum,
// since "all equal to the mean" trivially leaves the matrix fixed).
TEST(RmtClean, SmallQNearNoOpOnCleanMatrix) {
    const Eigen::Index n = 10;
    MatX C = MatX::Identity(n, n);
    auto r = rmt_clean(C, 0.01);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT((r->corr - C).cwiseAbs().maxCoeff(), 1e-9);
}

// ============================================================
// RIE: q>=1 falls back to Clip (identical result)
// ============================================================

TEST(RmtClean, RieFallsBackToClipWhenQGEOne) {
    MatX C = equicorrelation(20, 0.3);

    RmtConfig clip_cfg; // default Clip
    auto clip = rmt_clean(C, 1.5, clip_cfg);
    ASSERT_TRUE(clip.has_value());

    RmtConfig rie_cfg;
    rie_cfg.mode = RmtConfig::Mode::RIE;
    auto rie = rmt_clean(C, 1.5, rie_cfg);
    ASSERT_TRUE(rie.has_value());

    // q >= 1 makes RIE ill-posed (it needs T > N); the cleaner must fall back to
    // Clip and produce a byte-identical matrix and clipped count.
    EXPECT_EQ(rie->clipped, clip->clipped);
    EXPECT_EQ(fnv1a64_matrix(rie->corr), fnv1a64_matrix(clip->corr));
}

// ============================================================
// RIE: q<1 (T>N) runs, is PSD, trace reasonable
// ============================================================

TEST(RmtClean, RieRunsAndIsPsdWhenQLTOne) {
    MatX C = equicorrelation(20, 0.3);
    RmtConfig rie_cfg;
    rie_cfg.mode = RmtConfig::Mode::RIE;
    auto r = rmt_clean(C, 0.2, rie_cfg);
    ASSERT_TRUE(r.has_value());
    EXPECT_GE(min_eigenvalue(r->corr), -1e-12);
    // RIE reshapes eigenvalues but the trace of a correlation matrix stays N.
    EXPECT_NEAR(r->corr.trace(), static_cast<f64>(r->corr.rows()), 1e-6);
    for (Eigen::Index i = 0; i < r->corr.rows(); ++i) {
        EXPECT_NEAR(r->corr(i, i), 1.0, 1e-9);
    }
}

// ============================================================
// Determinism: two runs are byte-identical
// ============================================================

TEST(RmtClean, TwoRunsEqual) {
    MatX C = equicorrelation(20, 0.3);
    auto a = rmt_clean(C, 0.2);
    auto b = rmt_clean(C, 0.2);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(a->corr.rows(), b->corr.rows());
    ASSERT_EQ(a->corr.cols(), b->corr.cols());
    // Byte-identical, not just near: same input must yield the same bits.
    EXPECT_EQ(fnv1a64_matrix(a->corr), fnv1a64_matrix(b->corr));
    EXPECT_EQ(std::memcmp(a->corr.data(), b->corr.data(),
                          static_cast<std::size_t>(a->corr.size()) * sizeof(f64)),
              0);
}

// ============================================================
// Digest stability: pinned FNV-1a-64 golden value
// ============================================================

// Pins the cleaned-matrix digest for a fixed fixture so any future change to the
// numerics (edge fit, clip average, reconstruction order) is caught. The golden
// value was computed once from the green implementation and frozen here.
TEST(RmtClean, DigestStability) {
    MatX C = equicorrelation(20, 0.3);
    auto r = rmt_clean(C, 0.2);
    ASSERT_TRUE(r.has_value());
    constexpr u64 kGoldenDigest = 17595468881287217971ull; // pinned from green run
    EXPECT_EQ(fnv1a64_matrix(r->corr), kGoldenDigest);
}
