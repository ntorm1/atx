// hierarchical_test.cpp — TDD tests for atx::core::cluster agglomerative
// clustering (Algo::Hierarchical).
//
//   cluster(sim, {algo=Hierarchical, linkage, k})
//       Convert correlation -> distance d = sqrt(2(1-rho)), agglomerate with
//       Ward or Average linkage, cut to exactly k clusters, then canonicalize
//       labels by ascending smallest-member index.
//
// Anchors: a planted 3-block correlation matrix (both linkages recover the 3
// blocks at k=3), the ascending-index tie-break on equal merge distances, the
// k=1 / k=N degenerate cuts, the validation contract (non-square / empty / k out
// of range), permutation-invariance of the canonical labels, run-to-run identity,
// and a pinned FNV-1a-64 digest of the label vector.
//
// Determinism note: every fixture is built without RNG. Merge distances and the
// resulting partition are exact functions of the fixture, so the expected labels
// are deterministic, not statistical.

#include <atx/core/cluster/cluster.hpp>

#include <gtest/gtest.h>

#include <cstring> // std::memcpy
#include <vector>

#include <atx/core/error.hpp>
#include <atx/core/linalg/linalg.hpp>
#include <atx/core/types.hpp>

using namespace atx::core::cluster;
using atx::core::linalg::MatX;
using atx::f64;
using atx::i64;
using atx::u64;

namespace {

// FNV-1a-64 over the cluster_id vector, length-prefixed by the element count and
// folding each label as a little-endian 64-bit value. Matches the engine store's
// canonical FNV scheme (offset/prime) so the digest is byte-stable across
// platforms and runs. Implemented locally because atx-core's hash.hpp uses
// wyhash, which is explicitly not cross-platform stable.
[[nodiscard]] u64 fnv1a64_labels(const std::vector<int>& labels) {
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
    fold_u64(static_cast<u64>(labels.size()));
    for (int label : labels) {
        // Sign-extend then reinterpret so negative sentinels (should never occur
        // in a valid partition) still fold deterministically.
        u64 bits = static_cast<u64>(static_cast<i64>(label));
        fold_u64(bits);
    }
    return h;
}

// Build a block-diagonal correlation matrix: variables in the same block share
// off-diagonal correlation `within`, variables in different blocks share `between`.
// Unit diagonal. With within >> between the blocks are the unambiguous clusters.
[[nodiscard]] MatX block_corr(const std::vector<int>& block_of, f64 within, f64 between) {
    const auto n = static_cast<Eigen::Index>(block_of.size());
    MatX C(n, n);
    for (Eigen::Index i = 0; i < n; ++i) {
        for (Eigen::Index j = 0; j < n; ++j) {
            if (i == j) {
                C(i, j) = 1.0;
            } else {
                C(i, j) = (block_of[static_cast<std::size_t>(i)] ==
                           block_of[static_cast<std::size_t>(j)])
                              ? within
                              : between;
            }
        }
    }
    return C;
}

// True when two label vectors describe the SAME partition (cluster contents),
// independent of the integer labels assigned. Both are canonicalized here so a
// direct == is enough, but this helper also tolerates non-canonical inputs.
[[nodiscard]] bool same_partition(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    // Two points are co-clustered in `a` iff they are co-clustered in `b`.
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = i + 1; j < a.size(); ++j) {
            if ((a[i] == a[j]) != (b[i] == b[j])) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

// ============================================================
// Validation / error contract
// ============================================================

TEST(Hierarchical, RejectsNonSquare) {
    MatX C(2, 3);
    C << 1, 0.2, 0.1, 0.2, 1, 0.3;
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 2});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Hierarchical, RejectsEmpty) {
    MatX C(0, 0);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Hierarchical, RejectsKBelowOne) {
    MatX C = block_corr({0, 0, 1, 1}, 0.9, 0.1);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 0});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Hierarchical, RejectsKAboveN) {
    MatX C = block_corr({0, 0, 1, 1}, 0.9, 0.1);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 5});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ============================================================
// Block recovery: both linkages recover 3 planted blocks at k=3
// ============================================================

// Nine variables in three clean blocks {0,1,2},{3,4,5},{6,7,8}; within-block
// correlation 0.9, between-block 0.1. The sqrt(2(1-rho)) distance is far smaller
// inside a block than across, so both Ward and Average must recover the blocks
// exactly. Canonical labels number the block containing variable 0 as 0, etc.
TEST(Hierarchical, WardRecoversThreeBlocks) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1, 2, 2, 2}, 0.9, 0.1);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 3);
    const std::vector<int> expected{0, 0, 0, 1, 1, 1, 2, 2, 2};
    EXPECT_EQ(r->cluster_id, expected);
}

TEST(Hierarchical, AverageRecoversThreeBlocks) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1, 2, 2, 2}, 0.9, 0.1);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Average, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 3);
    const std::vector<int> expected{0, 0, 0, 1, 1, 1, 2, 2, 2};
    EXPECT_EQ(r->cluster_id, expected);
}

// ============================================================
// Degenerate cuts
// ============================================================

TEST(Hierarchical, KOneAllZeroOneLabel) {
    MatX C = block_corr({0, 0, 1, 1, 2, 2}, 0.8, 0.2);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 1});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 1);
    const std::vector<int> expected(6, 0);
    EXPECT_EQ(r->cluster_id, expected);
}

TEST(Hierarchical, KEqualsNEachItsOwnCluster) {
    MatX C = block_corr({0, 0, 1, 1}, 0.9, 0.1);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 4});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 4);
    // Each variable is a singleton; canonicalization numbers them by their own
    // (single) member index, i.e. the identity labeling.
    const std::vector<int> expected{0, 1, 2, 3};
    EXPECT_EQ(r->cluster_id, expected);
}

// ============================================================
// Tie-break: equal merge distances broken by ascending index
// ============================================================

// Four variables where the smallest pairwise distances are exactly equal: the
// correlation matrix is constructed so d(0,1) == d(2,3) == d(0,2) (a symmetric
// near-tie). With everything equidistant, a Ward agglomeration faces tied merge
// candidates at the first step; the contract resolves the tie by the lowest
// (i,j) instrument pair, so the very first merge must join {0,1}. We assert that
// at k=3 the resulting partition contains the pair {0,1} as one cluster.
TEST(Hierarchical, TieBreakAscendingIndex) {
    // Equicorrelation: every off-diagonal equals rho, so all pairwise distances
    // are identical and every first-merge candidate is tied. The ascending-index
    // rule must pick the (0,1) pair first.
    const Eigen::Index n = 4;
    MatX C = MatX::Constant(n, n, 0.5);
    C.diagonal().setOnes();
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 3);
    // The first (tied) merge joins variables 0 and 1; they must share a label,
    // while the remaining two stay singletons.
    EXPECT_EQ(r->cluster_id[0], r->cluster_id[1]);
    EXPECT_NE(r->cluster_id[0], r->cluster_id[2]);
    EXPECT_NE(r->cluster_id[0], r->cluster_id[3]);
    EXPECT_NE(r->cluster_id[2], r->cluster_id[3]);
    // Canonical labels: cluster with members {0,1} is label 0, {2} is 1, {3} is 2.
    const std::vector<int> expected{0, 0, 1, 2};
    EXPECT_EQ(r->cluster_id, expected);
}

// ============================================================
// Canonical labels invariant under input permutation
// ============================================================

// Permuting the rows/columns of the similarity matrix must not change the
// recovered partition once we invert the permutation on the labels and
// canonicalize. We permute a 3-block matrix, cluster the permuted matrix, map
// labels back to original variable order, and require the same partition as the
// unpermuted run.
TEST(Hierarchical, CanonicalLabelsPermutationInvariant) {
    const std::vector<int> blocks{0, 0, 0, 1, 1, 1, 2, 2, 2};
    MatX C = block_corr(blocks, 0.9, 0.1);
    auto base = cluster(C, {Algo::Hierarchical, Linkage::Ward, 3});
    ASSERT_TRUE(base.has_value());

    // A fixed non-identity permutation perm: position p holds original index perm[p].
    const std::vector<int> perm{4, 0, 7, 2, 8, 1, 5, 3, 6};
    const auto n = static_cast<Eigen::Index>(perm.size());
    MatX P(n, n);
    for (Eigen::Index a = 0; a < n; ++a) {
        for (Eigen::Index b = 0; b < n; ++b) {
            P(a, b) = C(perm[static_cast<std::size_t>(a)], perm[static_cast<std::size_t>(b)]);
        }
    }
    auto permuted = cluster(P, {Algo::Hierarchical, Linkage::Ward, 3});
    ASSERT_TRUE(permuted.has_value());

    // Invert the permutation: label of original variable perm[p] is permuted[p].
    std::vector<int> recovered(static_cast<std::size_t>(n), -1);
    for (Eigen::Index p = 0; p < n; ++p) {
        recovered[static_cast<std::size_t>(perm[static_cast<std::size_t>(p)])] =
            permuted->cluster_id[static_cast<std::size_t>(p)];
    }
    EXPECT_TRUE(same_partition(recovered, base->cluster_id));
}

// ============================================================
// Determinism: two runs identical
// ============================================================

TEST(Hierarchical, TwoRunsEqual) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1, 2, 2, 2}, 0.85, 0.15);
    auto a = cluster(C, {Algo::Hierarchical, Linkage::Ward, 3});
    auto b = cluster(C, {Algo::Hierarchical, Linkage::Ward, 3});
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->cluster_id, b->cluster_id);
    EXPECT_EQ(a->n_labels, b->n_labels);
    EXPECT_EQ(fnv1a64_labels(a->cluster_id), fnv1a64_labels(b->cluster_id));
}

// ============================================================
// Digest stability: pinned FNV-1a-64 golden of the label vector
// ============================================================

// Pins the label-vector digest for a fixed 3-block fixture so any future change
// to the merge order, linkage formula, or canonicalization is caught. The golden
// value is computed once from the green implementation and frozen here.
TEST(Hierarchical, DigestStability) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1, 2, 2, 2}, 0.9, 0.1);
    auto r = cluster(C, {Algo::Hierarchical, Linkage::Ward, 3});
    ASSERT_TRUE(r.has_value());
    // Partition is {0,1,2},{3,4,5},{6,7,8} -> labels 0,0,0,1,1,1,2,2,2.
    constexpr u64 kGoldenDigest = 15156715062202025513ull; // pinned from green run
    EXPECT_EQ(fnv1a64_labels(r->cluster_id), kGoldenDigest);
}
