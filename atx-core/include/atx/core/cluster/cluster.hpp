#pragma once

// atx::core::cluster — unsupervised partitioning of a similarity / correlation
// matrix into groups (S11 scaffold).
//
// The input is an N×N symmetric matrix S whose (i,j) entry measures how alike
// variables i and j are — a (cleaned) return correlation matrix is the S11 use
// case. cluster() turns S into a dense partition: one integer label per variable.
// Two algorithms are planned:
//
//   Algo::Hierarchical  Agglomerative clustering on the distance d = √(2(1−ρ))
//                       derived from the correlation, cut to `k` clusters. The
//                       merge rule is selected by Linkage (Ward or Average).
//   Algo::SpongeSym     Signed-graph clustering (SPONGE, symmetric variant):
//                       partitions a signed similarity graph by a generalized
//                       eigenproblem on the signed Laplacians — appropriate when
//                       negative correlations carry repulsion information that a
//                       distance metric discards.
//
// Determinism contract (S11; inherited by S11-2): no RNG on the result path
// (no random restarts / random seeding — any eigen-based step uses the fixed
// ascending-eigenvalue order and first-nonzero-component-positive sign
// convention); all reductions are order-fixed; and cluster LABELS are
// CANONICALIZED — relabeled so that clusters are numbered by ASCENDING
// smallest-member index (the cluster containing variable 0 is always label 0,
// the next cluster by its lowest member is label 1, and so on). Canonical labels
// make the partition byte-stable and directly comparable across runs.
//
// Edge placement (Pattern B): general numerics, lives in atx-core; the engine's
// cluster panel consumes it. The matrix type is the atx-core column-major MatX
// shared by the linalg routines; cluster() treats its input as symmetric.
//
// TODO(S11-2): implement Hierarchical and SpongeSym; the declarations below are
// the frozen seam.

#include <vector>

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // i64

#include "atx/core/linalg/linalg.hpp" // MatX

namespace atx::core::cluster {

using atx::core::Result;
using atx::core::linalg::MatX; // the column-major symmetric-matrix type (see linalg.hpp)

// Merge rule for agglomerative (hierarchical) clustering.
enum class Linkage {
  Ward,    // minimum within-cluster variance increase (default; compact clusters)
  Average, // mean pairwise distance between cluster members (UPGMA)
};

// Clustering algorithm family.
enum class Algo {
  Hierarchical, // agglomerative on a correlation-derived distance
  SpongeSym,    // signed-graph spectral clustering (SPONGE, symmetric)
};

// Clustering policy. Aggregate so callers brace-initialize the fields they set;
// defaults pick hierarchical Ward, the interpretable baseline for S11.
struct ClusterConfig {
  // Algorithm family to run.
  Algo algo = Algo::Hierarchical;

  // Merge rule; consulted only by Algo::Hierarchical.
  Linkage linkage = Linkage::Ward;

  // Target number of clusters. Must satisfy 1 ≤ k ≤ N. TODO(S11-2): a future
  // unit may add an automatic-k selection mode; for now k is caller-supplied.
  int k = 1;

  // Numerical floor for distance / eigenproblem denominators so a degenerate
  // (rank-deficient) similarity matrix cannot divide by ~0. TODO(S11-2): wire in.
  double eps = 1e-12;
};

// Result of clustering: a dense label vector plus the realized cluster count.
struct Clustering {
  // cluster_id[i] is the canonical label of variable i, in [0, n_labels). Labels
  // are canonicalized by ascending smallest-member index (see the contract note).
  std::vector<int> cluster_id;

  // Number of distinct labels actually produced (== max(cluster_id)+1 for a
  // non-empty partition). Normally equals ClusterConfig::k, but a degenerate
  // input may collapse clusters; this records what was realized.
  i64 n_labels = 0;
};

// Partition an N×N symmetric similarity matrix into clusters.
//
// Inputs:
//   * `sim` — the N×N symmetric similarity / correlation matrix (treated as
//     symmetric). N ≥ 1 is required.
//   * `cfg` — algorithm + parameters (k must satisfy 1 ≤ k ≤ N).
//
// Returns the canonical labeling, or Err on a non-square / empty `sim`, a k out
// of range, or a numerical failure (InvalidArgument / Internal, matching the
// linalg convention).
//
// TODO(S11-2): define this function. Declared now so S11-3's engine cluster
// panel binds to a stable signature before the algorithm lands.
[[nodiscard]] Result<Clustering> cluster(const MatX &sim, ClusterConfig cfg = {});

} // namespace atx::core::cluster
