// sponge_test.cpp — TDD tests for atx::core::cluster signed-graph clustering
// (Algo::SpongeSym).
//
//   cluster(sim, {algo=SpongeSym, k, tau_plus, tau_minus})
//       Treat sim as a signed adjacency A (diagonal zeroed), split into A+/A-,
//       form regularized signed Laplacians, solve the SPONGEsym generalized
//       eigenproblem (L+ + tau_minus*D-) v = lambda (L- + tau_plus*D+) v, take the
//       bottom-k eigenvectors as an embedding, and run deterministic k-means++ to
//       assign k clusters. Labels canonicalized by ascending smallest-member index.
//
// Anchors: a planted 3-block correlation matrix (recovers the 3 blocks at k=3), a
// strongly anti-correlated pair that SPONGE must split (where the distance metric
// would merge it), the k=1 / k=N degenerate cases, the validation contract, the
// permutation-invariance of canonical labels, run-to-run identity, and a pinned
// FNV-1a-64 digest of the label vector.
//
// Determinism note: every fixture is built without RNG. SPONGE's eigenvectors use
// the ascending-eigenvalue order with a fixed sign convention, and k-means++ uses
// deterministic D^2 furthest-first seeding, so the partition is an exact function
// of the fixture.

#include <atx/core/cluster/cluster.hpp>

#include <gtest/gtest.h>

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

// FNV-1a-64 over the cluster_id vector (see hierarchical_test.cpp for the scheme).
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
        u64 bits = static_cast<u64>(static_cast<i64>(label));
        fold_u64(bits);
    }
    return h;
}

// Block-diagonal correlation matrix (see hierarchical_test.cpp). within is the
// intra-block correlation, between the inter-block correlation; unit diagonal.
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

[[nodiscard]] bool same_partition(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() != b.size()) {
        return false;
    }
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

TEST(Sponge, RejectsNonSquare) {
    MatX C(2, 3);
    C << 1, 0.2, 0.1, 0.2, 1, 0.3;
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 2});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Sponge, RejectsEmpty) {
    MatX C(0, 0);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Sponge, RejectsKBelowOne) {
    MatX C = block_corr({0, 0, 1, 1}, 0.9, 0.1);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 0});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Sponge, RejectsKAboveN) {
    MatX C = block_corr({0, 0, 1, 1}, 0.9, 0.1);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 5});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ============================================================
// Block recovery
// ============================================================

// Three clean positive-correlation blocks: SPONGE's attractive part (A+) ties
// each block together and the embedding separates them, so k=3 recovers exactly
// {0,1,2},{3,4,5},{6,7,8}.
TEST(Sponge, RecoversThreeBlocks) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1, 2, 2, 2}, 0.9, 0.0);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 3);
    const std::vector<int> expected{0, 0, 0, 1, 1, 1, 2, 2, 2};
    EXPECT_EQ(r->cluster_id, expected);
}

// ============================================================
// SPONGE's raison d'être: anti-correlation is repulsion
// ============================================================

// Two groups that are positively correlated WITHIN and strongly negatively
// correlated ACROSS. The sqrt(2(1-rho)) distance turns the strong anti-correlation
// (rho = -0.8) into the LARGEST distance, so a naive distance clusterer would also
// separate them — but the discriminating case for SPONGE is the signed-cut
// contract: the signed Laplacian's repulsion keeps the anti-correlated groups apart.
//
// Two blocks {0,1,2} and {3,4,5}. Within each block rho=+0.8 (attraction); across
// the blocks rho=-0.8 (repulsion). SPONGE must place {0,1,2} and {3,4,5} in
// different clusters at k=2.
//
// Fixture power note (review fix): an earlier two-members-per-block version was
// under-powered for the canonical (un-weighted, row-normalized) SPONGEsym
// embedding — with only two noise-free members per block the bottom generalized
// eigenvectors cannot resolve the blocks and k-means peels off a single node. Three
// members per block put the discriminative signed-cut mode clearly above the
// within-block bulk, so the canonical embedding recovers the planted split. (The
// prior version only "passed" because the removed 1/sqrt(lambda) weighting happened
// to suppress the bulk modes — exercising the bug, not the contract; cf. S11-3's
// under-powered-fixture fix.)
TEST(Sponge, AntiCorrelatedPairSplits) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1}, 0.8, -0.8);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 2});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 2);
    // {0,1,2} together, {3,4,5} together, and the two groups distinct.
    EXPECT_EQ(r->cluster_id[0], r->cluster_id[1]);
    EXPECT_EQ(r->cluster_id[1], r->cluster_id[2]);
    EXPECT_EQ(r->cluster_id[3], r->cluster_id[4]);
    EXPECT_EQ(r->cluster_id[4], r->cluster_id[5]);
    EXPECT_NE(r->cluster_id[0], r->cluster_id[3]);
    const std::vector<int> expected{0, 0, 0, 1, 1, 1};
    EXPECT_EQ(r->cluster_id, expected);
}

// A single strongly anti-correlated pair embedded in an otherwise-positive block.
// d = sqrt(2(1-rho)) would still merge the anti-correlated members into the block
// because the block's positive correlations dominate the average linkage; SPONGE's
// signed repulsion must pull the anti-correlated member out. Variables 0..3 are a
// positive block (rho=+0.7), but variable 4 is strongly anti-correlated (rho=-0.9)
// with the block. At k=2 the anti member 4 must land in a different cluster from
// the positive block {0,1,2,3}.
TEST(Sponge, AntiCorrelatedMemberSeparated) {
    const Eigen::Index n = 5;
    MatX C = MatX::Identity(n, n);
    for (Eigen::Index i = 0; i < 4; ++i) {
        for (Eigen::Index j = 0; j < 4; ++j) {
            if (i != j) {
                C(i, j) = 0.7;
            }
        }
    }
    for (Eigen::Index i = 0; i < 4; ++i) {
        C(i, 4) = -0.9;
        C(4, i) = -0.9;
    }
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 2});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 2);
    // The anti-correlated variable 4 is in its own cluster, separate from the block.
    EXPECT_NE(r->cluster_id[4], r->cluster_id[0]);
    EXPECT_EQ(r->cluster_id[0], r->cluster_id[1]);
    EXPECT_EQ(r->cluster_id[0], r->cluster_id[2]);
    EXPECT_EQ(r->cluster_id[0], r->cluster_id[3]);
}

// ============================================================
// Degenerate cuts
// ============================================================

TEST(Sponge, KOneAllZeroOneLabel) {
    MatX C = block_corr({0, 0, 1, 1, 2, 2}, 0.8, -0.2);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 1});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 1);
    const std::vector<int> expected(6, 0);
    EXPECT_EQ(r->cluster_id, expected);
}

TEST(Sponge, KEqualsNEachItsOwnCluster) {
    MatX C = block_corr({0, 0, 1, 1}, 0.9, -0.1);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 4});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->n_labels, 4);
    const std::vector<int> expected{0, 1, 2, 3};
    EXPECT_EQ(r->cluster_id, expected);
}

// ============================================================
// Canonical labels invariant under input permutation
// ============================================================

TEST(Sponge, CanonicalLabelsPermutationInvariant) {
    const std::vector<int> blocks{0, 0, 0, 1, 1, 1, 2, 2, 2};
    MatX C = block_corr(blocks, 0.9, -0.1);
    auto base = cluster(C, {Algo::SpongeSym, Linkage::Ward, 3});
    ASSERT_TRUE(base.has_value());

    const std::vector<int> perm{4, 0, 7, 2, 8, 1, 5, 3, 6};
    const auto n = static_cast<Eigen::Index>(perm.size());
    MatX P(n, n);
    for (Eigen::Index a = 0; a < n; ++a) {
        for (Eigen::Index b = 0; b < n; ++b) {
            P(a, b) = C(perm[static_cast<std::size_t>(a)], perm[static_cast<std::size_t>(b)]);
        }
    }
    auto permuted = cluster(P, {Algo::SpongeSym, Linkage::Ward, 3});
    ASSERT_TRUE(permuted.has_value());

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

TEST(Sponge, TwoRunsEqual) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1, 2, 2, 2}, 0.85, -0.15);
    auto a = cluster(C, {Algo::SpongeSym, Linkage::Ward, 3});
    auto b = cluster(C, {Algo::SpongeSym, Linkage::Ward, 3});
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->cluster_id, b->cluster_id);
    EXPECT_EQ(a->n_labels, b->n_labels);
    EXPECT_EQ(fnv1a64_labels(a->cluster_id), fnv1a64_labels(b->cluster_id));
}

// ============================================================
// Digest stability: pinned FNV-1a-64 golden of the label vector
// ============================================================

TEST(Sponge, DigestStability) {
    MatX C = block_corr({0, 0, 0, 1, 1, 1, 2, 2, 2}, 0.9, 0.0);
    auto r = cluster(C, {Algo::SpongeSym, Linkage::Ward, 3});
    ASSERT_TRUE(r.has_value());
    // Partition is {0,1,2},{3,4,5},{6,7,8} -> labels 0,0,0,1,1,1,2,2,2.
    constexpr u64 kGoldenDigest = 15156715062202025513ull; // pinned from green run
    EXPECT_EQ(fnv1a64_labels(r->cluster_id), kGoldenDigest);
}
