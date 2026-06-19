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
// Method (S11-2):
//   Hierarchical — agglomerative clustering on the Winton "covariance sectors"
//     distance d_ij = √(2(1−ρ_ij)) (ρ clamped to [−1,1]). Lance-Williams updates
//     drive Ward or Average linkage; the dendrogram is cut to exactly k clusters
//     by stopping after N−k merges. Equal merge distances are broken by the
//     lowest surviving (i,j) representative-index pair.
//   SpongeSym — Oxford-Man signed-graph stat-arb clustering. The off-diagonal
//     correlations form a signed adjacency A = A⁺ − A⁻; with degree diagonals
//     D⁺, D⁻ and signed Laplacians L± = D± − A±, SPONGEsym solves the generalized
//     eigenproblem (L⁺ + τ⁻·D⁻) v = λ (L⁻ + τ⁺·D⁺) v and clusters the bottom-k
//     generalized eigenvectors with deterministic k-means++ (D²-furthest-first
//     seeding, no RNG). Keeping the negative correlations as repulsion is the
//     whole point: a strongly anti-correlated pair is pushed apart even where the
//     √(2(1−ρ)) distance would merge it.

#include <algorithm> // std::max, std::min, std::clamp
#include <cmath>     // std::sqrt, std::abs
#include <limits>    // std::numeric_limits
#include <utility>   // std::move
#include <vector>

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, i64

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

namespace atx::core::cluster {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::linalg::MatX; // the column-major symmetric-matrix type (see linalg.hpp)
using atx::core::linalg::VecX;

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
  // (rank-deficient) similarity matrix cannot divide by ~0, and the SPD guard on
  // the SPONGEsym right-hand pencil.
  double eps = 1e-12;

  // SPONGEsym signed-Laplacian regularizers (consulted only by Algo::SpongeSym).
  // tau_plus weights the positive-degree diagonal added to the repulsion pencil
  // L⁻; tau_minus weights the negative-degree diagonal added to the attraction
  // pencil L⁺. Both lift the generalized eigenproblem's right-hand side to SPD so
  // GeneralizedSelfAdjointEigenSolver is well-posed even when a node has no
  // negative (or no positive) edges. Defaults of 1.0 match the Oxford-Man
  // SPONGEsym reference; larger values regularize harder toward the degree prior.
  double tau_plus = 1.0;
  double tau_minus = 1.0;
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

namespace detail {

// Neumaier (improved Kahan) compensated sum. Centroid coordinates and squared
// distances fold many terms of mixed magnitude; an order-fixed compensated sum
// keeps the low bits stable so the partition (and its pinned digest) is byte-
// identical across runs. atx-core exposes no shared compensated-sum helper, so
// this small local copy is the reused primitive — matching rmt_clean.hpp's.
class NeumaierSum {
public:
  void add(f64 x) noexcept {
    const f64 t = sum_ + x;
    if (std::abs(sum_) >= std::abs(x)) {
      compensation_ += (sum_ - t) + x;
    } else {
      compensation_ += (x - t) + sum_;
    }
    sum_ = t;
  }
  [[nodiscard]] f64 value() const noexcept { return sum_ + compensation_; }

private:
  f64 sum_ = 0.0;
  f64 compensation_ = 0.0;
};

// Relabel an arbitrary partition so clusters are numbered by ASCENDING
// smallest-member index: the cluster containing variable 0 becomes label 0, the
// next cluster (by its lowest member) becomes 1, and so on. This makes the label
// vector a function of the partition alone, independent of the internal scan or
// allocation order that produced `raw`. Returns the canonical labels and the
// realized cluster count. `raw` may use any integer ids; only co-membership
// matters.
[[nodiscard]] inline Clustering canonicalize(const std::vector<int> &raw) {
  const std::size_t n = raw.size();
  std::vector<int> out(n, -1);
  int next = 0;
  // Scanning members in ascending index order and assigning the next free label
  // the first time each raw id is seen yields exactly the smallest-member order.
  std::vector<int> first_label(n, -1); // raw id -> canonical label (raw ids are < n
                                       // because they index members in our callers)
  for (std::size_t i = 0; i < n; ++i) {
    const int rid = raw[i];
    // raw ids from our partitioners are in [0, n); guard anyway.
    if (rid >= 0 && static_cast<std::size_t>(rid) < n) {
      if (first_label[static_cast<std::size_t>(rid)] < 0) {
        first_label[static_cast<std::size_t>(rid)] = next++;
      }
      out[i] = first_label[static_cast<std::size_t>(rid)];
    }
  }
  Clustering c;
  c.cluster_id = std::move(out);
  c.n_labels = next;
  return c;
}

// Enforce the fixed eigenvector sign convention: the first component with
// magnitude above a tiny threshold is made positive. A generalized eigenvector
// is only defined up to sign; pinning it makes the k-means embedding sign-stable
// so the same input yields the same seeds and the same partition.
inline void canonicalize_signs(MatX &vectors) noexcept {
  for (Eigen::Index c = 0; c < vectors.cols(); ++c) {
    for (Eigen::Index r = 0; r < vectors.rows(); ++r) {
      const f64 v = vectors(r, c);
      if (std::abs(v) > 1e-300) {
        if (v < 0.0) {
          vectors.col(c) = -vectors.col(c);
        }
        break;
      }
    }
  }
}

// ---- Hierarchical (agglomerative) partitioner -----------------------------
//
// Lance-Williams agglomerative clustering on the distance matrix derived from
// the correlation. Active clusters are tracked by a union of members; the pair
// with the smallest linkage distance is merged each step until N−k clusters
// remain (i.e. N−k merges). Ties in the merge distance are broken by the lowest
// (representative-index) pair, where a cluster's representative is its smallest
// member — this is the determinism hazard the contract pins.
[[nodiscard]] inline Clustering hierarchical_partition(const MatX &sim, int k, Linkage linkage,
                                                       f64 eps) {
  const auto n = static_cast<int>(sim.rows());
  const bool ward = (linkage == Linkage::Ward);

  // Working dissimilarity matrix `cd` whose units depend on the linkage:
  //   * Ward (Ward.D2, matching scipy `ward`) operates on SQUARED distances
  //     d_ij² = 2(1−ρ_ij). The Lance-Williams recurrence and the closest-pair
  //     selection are both defined on d² for Ward; keeping the matrix in squared
  //     units is what makes this canonical Ward rather than Ward applied to raw d.
  //   * Average (UPGMA) operates on the raw distance d_ij = √(2(1−ρ_ij)); its
  //     recurrence is the size-weighted mean of the un-squared distances.
  // ρ is clamped to [−1,1] so d² ∈ [0,4]; the clamp to ≥0 guards round-off only.
  // The merge selector compares cd entries by magnitude, which is order-preserving
  // for both units, so no display distance (no √) is ever needed on this path.
  MatX cd(n, n);
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      const f64 rho = std::clamp(sim(i, j), -1.0, 1.0);
      const f64 d2 = 2.0 * (1.0 - rho);
      const f64 d2c = d2 > 0.0 ? d2 : 0.0;
      cd(i, j) = ward ? d2c : std::sqrt(d2c);
    }
  }
  (void)eps; // distance is well-defined without a floor; kept for signature parity

  // Active clusters: id -> {members, size, representative=min member}. We keep a
  // dense list of live cluster ids and the working matrix `cd` indexed by cluster
  // id (Lance-Williams updates rewrite rows/cols in place).
  std::vector<std::vector<int>> members(static_cast<std::size_t>(n));
  std::vector<int> rep(static_cast<std::size_t>(n));
  std::vector<int> live;
  live.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    members[static_cast<std::size_t>(i)] = {i};
    rep[static_cast<std::size_t>(i)] = i;
    live.push_back(i);
  }

  const int target_merges = n - k; // cut after exactly N−k merges -> k clusters
  for (int step = 0; step < target_merges; ++step) {
    // Find the closest live pair, tie-broken by the lowest (rep[a], rep[b]) pair
    // with a < b in representative order. Scanning live ids in their (sorted)
    // order and comparing (distance, rep_lo, rep_hi) lexicographically realizes
    // the ascending-index rule deterministically.
    f64 best_d = std::numeric_limits<f64>::infinity();
    int best_lo_rep = std::numeric_limits<int>::max();
    int best_hi_rep = std::numeric_limits<int>::max();
    int best_a = -1, best_b = -1;
    for (std::size_t ia = 0; ia < live.size(); ++ia) {
      for (std::size_t ib = ia + 1; ib < live.size(); ++ib) {
        const int a = live[ia];
        const int b = live[ib];
        const int ra = rep[static_cast<std::size_t>(a)];
        const int rb = rep[static_cast<std::size_t>(b)];
        const int lo_rep = std::min(ra, rb);
        const int hi_rep = std::max(ra, rb);
        const f64 d = cd(a, b);
        const bool better = (d < best_d) ||
                            (d == best_d && (lo_rep < best_lo_rep ||
                                             (lo_rep == best_lo_rep && hi_rep < best_hi_rep)));
        if (better) {
          best_d = d;
          best_lo_rep = lo_rep;
          best_hi_rep = hi_rep;
          best_a = a;
          best_b = b;
        }
      }
    }

    // Merge best_b into best_a. Keep the smaller representative on the surviving
    // cluster so the tie-break stays a function of original instrument indices.
    int keep = best_a, drop = best_b;
    if (rep[static_cast<std::size_t>(drop)] < rep[static_cast<std::size_t>(keep)]) {
      std::swap(keep, drop);
    }
    const f64 size_keep = static_cast<f64>(members[static_cast<std::size_t>(keep)].size());
    const f64 size_drop = static_cast<f64>(members[static_cast<std::size_t>(drop)].size());

    // Lance-Williams update of the merged cluster `keep` to every other live
    // cluster c, applied in the working matrix's own units.
    //   * Average (UPGMA): the size-weighted mean of the two old (un-squared)
    //     distances.
    //   * Ward (Ward.D2): the recurrence on SQUARED distances,
    //       d'² = α_k·d_kc² + α_d·d_dc² + β·d_kd²,
    //     with α_k=(n_k+n_c)/T, α_d=(n_d+n_c)/T, β=−n_c/T, T=n_k+n_d+n_c. Because
    //     `cd` already holds d² for Ward, the recurrence runs directly on the
    //     stored values — no squaring of an already-updated distance and no √.
    const f64 d_kd = cd(keep, drop); // d² (Ward) or d (Average), in cd's units
    for (int c : live) {
      if (c == keep || c == drop) {
        continue;
      }
      const f64 d_keep_c = cd(keep, c);
      const f64 d_drop_c = cd(drop, c);
      f64 nd;
      if (ward) {
        const f64 size_c = static_cast<f64>(members[static_cast<std::size_t>(c)].size());
        const f64 total = size_keep + size_drop + size_c;
        const f64 ak = (size_keep + size_c) / total;
        const f64 ad = (size_drop + size_c) / total;
        const f64 beta = -size_c / total;
        nd = ak * d_keep_c + ad * d_drop_c + beta * d_kd;
        if (nd < 0.0) {
          nd = 0.0; // squared distance stays non-negative under round-off
        }
      } else {
        nd = (size_keep * d_keep_c + size_drop * d_drop_c) / (size_keep + size_drop);
      }
      cd(keep, c) = nd;
      cd(c, keep) = nd;
    }

    // Absorb drop's members into keep and retire drop from the live set.
    auto &mk = members[static_cast<std::size_t>(keep)];
    auto &md = members[static_cast<std::size_t>(drop)];
    mk.insert(mk.end(), md.begin(), md.end());
    rep[static_cast<std::size_t>(keep)] =
        std::min(rep[static_cast<std::size_t>(keep)], rep[static_cast<std::size_t>(drop)]);
    md.clear();
    live.erase(std::remove(live.begin(), live.end(), drop), live.end());
  }

  // Each surviving live cluster id is one group; emit a raw label per variable.
  std::vector<int> raw(static_cast<std::size_t>(n), 0);
  int gid = 0;
  for (int c : live) {
    for (int m : members[static_cast<std::size_t>(c)]) {
      raw[static_cast<std::size_t>(m)] = gid;
    }
    ++gid;
  }
  return canonicalize(raw);
}

// One Lloyd refinement from a fixed set of seed centers. Assignments are
// tie-broken to the lowest center index; centroids are order-fixed compensated
// means and an empty cluster keeps its previous center. Writes the converged
// assignment into `assign` and returns the total within-cluster inertia (the
// objective k-means minimizes), which the restart loop uses to pick the best run.
[[nodiscard]] inline f64 lloyd_refine(const MatX &embed, int k, f64 eps,
                                      std::vector<VecX> centers, std::vector<int> &assign) {
  const auto n = static_cast<int>(embed.rows());
  const auto dim = static_cast<int>(embed.cols());
  assign.assign(static_cast<std::size_t>(n), 0);

  auto sq_dist = [&](int p, const VecX &center) -> f64 {
    NeumaierSum s;
    for (int d = 0; d < dim; ++d) {
      const f64 diff = embed(p, d) - center[d];
      s.add(diff * diff);
    }
    return s.value();
  };

  constexpr int kMaxIters = 100;
  for (int it = 0; it < kMaxIters; ++it) {
    bool changed = false;
    for (int i = 0; i < n; ++i) {
      int best = 0;
      f64 best_d = std::numeric_limits<f64>::infinity();
      for (int c = 0; c < k; ++c) {
        const f64 d = sq_dist(i, centers[static_cast<std::size_t>(c)]);
        if (d < best_d) { // strict: first (lowest-index) center wins a tie
          best_d = d;
          best = c;
        }
      }
      if (assign[static_cast<std::size_t>(i)] != best) {
        assign[static_cast<std::size_t>(i)] = best;
        changed = true;
      }
    }

    std::vector<int> counts(static_cast<std::size_t>(k), 0);
    std::vector<std::vector<NeumaierSum>> sums(
        static_cast<std::size_t>(k), std::vector<NeumaierSum>(static_cast<std::size_t>(dim)));
    for (int i = 0; i < n; ++i) {
      const int c = assign[static_cast<std::size_t>(i)];
      ++counts[static_cast<std::size_t>(c)];
      for (int d = 0; d < dim; ++d) {
        sums[static_cast<std::size_t>(c)][static_cast<std::size_t>(d)].add(embed(i, d));
      }
    }
    f64 shift = 0.0;
    for (int c = 0; c < k; ++c) {
      if (counts[static_cast<std::size_t>(c)] == 0) {
        continue; // empty cluster keeps its previous center (no collapse)
      }
      VecX nc(dim);
      for (int d = 0; d < dim; ++d) {
        nc[d] = sums[static_cast<std::size_t>(c)][static_cast<std::size_t>(d)].value() /
                static_cast<f64>(counts[static_cast<std::size_t>(c)]);
      }
      shift = std::max(shift, (nc - centers[static_cast<std::size_t>(c)]).cwiseAbs().maxCoeff());
      centers[static_cast<std::size_t>(c)] = std::move(nc);
    }
    if (!changed && shift <= eps) {
      break;
    }
  }

  NeumaierSum inertia;
  for (int i = 0; i < n; ++i) {
    inertia.add(sq_dist(i, centers[static_cast<std::size_t>(assign[static_cast<std::size_t>(i)])]));
  }
  return inertia.value();
}

// Deterministic k-means on a fixed embedding (one row per variable).
//
// k-means++ replaces its random seeding with deterministic D²-furthest-first
// seeding: from a fixed first center, each next center is the point whose squared
// distance to its nearest chosen center is largest (ties to the lowest index).
// Plain furthest-first from a single start is greedy and can latch onto an
// outlier axis (e.g. a degenerate within-cluster eigen-mode), so we run it once
// per possible first center — n fully deterministic restarts — refine each with
// Lloyd, and keep the lowest-inertia partition (ties broken by the
// lexicographically smallest assignment). This recovers global structure without
// any RNG, satisfying the no-random-restart determinism contract.
//
// Returns a raw label per row.
[[nodiscard]] inline std::vector<int> kmeans_deterministic(const MatX &embed, int k, f64 eps) {
  const auto n = static_cast<int>(embed.rows());
  const auto dim = static_cast<int>(embed.cols());
  std::vector<int> assign(static_cast<std::size_t>(n), 0);
  if (k <= 1) {
    return assign; // single cluster: everything is label 0
  }
  if (k >= n) {
    // Each point its own cluster; raw id == row index (canonicalized by caller).
    for (int i = 0; i < n; ++i) {
      assign[static_cast<std::size_t>(i)] = i;
    }
    return assign;
  }

  auto sq_dist = [&](int p, const VecX &center) -> f64 {
    NeumaierSum s;
    for (int d = 0; d < dim; ++d) {
      const f64 diff = embed(p, d) - center[d];
      s.add(diff * diff);
    }
    return s.value();
  };

  // Furthest-first seeding of k centers given a fixed first-center index.
  auto seed_from = [&](int first) -> std::vector<VecX> {
    std::vector<VecX> centers;
    centers.reserve(static_cast<std::size_t>(k));
    centers.push_back(embed.row(first).transpose());
    std::vector<f64> nearest(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      nearest[static_cast<std::size_t>(i)] = sq_dist(i, centers[0]);
    }
    while (static_cast<int>(centers.size()) < k) {
      int pick = 0;
      f64 pick_d = -1.0;
      for (int i = 0; i < n; ++i) {
        if (nearest[static_cast<std::size_t>(i)] > pick_d) {
          pick_d = nearest[static_cast<std::size_t>(i)];
          pick = i;
        }
      }
      centers.push_back(embed.row(pick).transpose());
      for (int i = 0; i < n; ++i) {
        nearest[static_cast<std::size_t>(i)] =
            std::min(nearest[static_cast<std::size_t>(i)], sq_dist(i, centers.back()));
      }
    }
    return centers;
  };

  std::vector<int> best_assign;
  f64 best_inertia = std::numeric_limits<f64>::infinity();
  for (int first = 0; first < n; ++first) {
    std::vector<int> trial;
    const f64 inertia = lloyd_refine(embed, k, eps, seed_from(first), trial);
    const bool better = (inertia < best_inertia - eps) ||
                        (std::abs(inertia - best_inertia) <= eps &&
                         (best_assign.empty() || trial < best_assign));
    if (better) {
      best_inertia = inertia;
      best_assign = std::move(trial);
    }
  }
  return best_assign;
}

// ---- SPONGEsym (signed-graph) partitioner ---------------------------------
//
// Builds the signed adjacency from the off-diagonal correlations, forms the
// regularized signed Laplacian pencils, solves the generalized eigenproblem, and
// k-means-clusters the bottom-k generalized eigenvectors. Returns Err only on a
// numerical failure (the RHS pencil is SPD by construction once τ > 0).
[[nodiscard]] inline Result<Clustering> sponge_partition(const MatX &sim, int k, f64 tau_plus,
                                                         f64 tau_minus, f64 eps) {
  const Eigen::Index n = sim.rows();

  // Signed adjacency A with a zeroed diagonal (no self-loops in the graph); split
  // into the attractive part A⁺ = max(A,0) and the repulsive part A⁻ = max(−A,0).
  MatX ap = MatX::Zero(n, n);
  MatX am = MatX::Zero(n, n);
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      if (i == j) {
        continue;
      }
      const f64 a = sim(i, j);
      if (a > 0.0) {
        ap(i, j) = a;
      } else if (a < 0.0) {
        am(i, j) = -a;
      }
    }
  }

  // Degree diagonals and signed Laplacians L± = D± − A±.
  VecX dp = ap.rowwise().sum();
  VecX dm = am.rowwise().sum();
  MatX lp = -ap;
  MatX lm = -am;
  for (Eigen::Index i = 0; i < n; ++i) {
    lp(i, i) += dp[i];
    lm(i, i) += dm[i];
  }

  // SPONGEsym pencils: solve (L⁺ + τ⁻·D⁻) v = λ (L⁻ + τ⁺·D⁺) v. The τ·D terms
  // regularize each Laplacian toward its degree prior and, crucially, make the
  // right-hand matrix SPD so the generalized self-adjoint solver is well-posed
  // even for a node lacking positive (or negative) edges. A tiny eps·I backstops
  // an all-zero degree row (an isolated node) so the RHS stays strictly PD.
  MatX lhs = lp;
  MatX rhs = lm;
  for (Eigen::Index i = 0; i < n; ++i) {
    lhs(i, i) += tau_minus * dm[i];
    rhs(i, i) += tau_plus * dp[i] + eps;
  }
  // Symmetrize away round-off so the self-adjoint solver sees an exactly symmetric pencil.
  lhs = 0.5 * (lhs + lhs.transpose());
  rhs = 0.5 * (rhs + rhs.transpose());

  Eigen::GeneralizedSelfAdjointEigenSolver<MatX> ges(lhs, rhs);
  if (ges.info() != Eigen::Success) {
    return Err(ErrorCode::Internal, "cluster: SPONGEsym generalized eigensolver failed");
  }
  // Eigenvalues ascending; the bottom-k eigenvectors are the discriminative
  // embedding (smallest generalized eigenvalues ~ best signed-cut directions).
  MatX vecs = ges.eigenvectors();
  canonicalize_signs(vecs);

  // Build the bottom-k embedding from the RAW generalized eigenvectors (the
  // canonical SPONGEsym embedding) and then unit-normalize each row before
  // k-means — the standard Ng–Jordan–Weiss spectral-clustering step.
  //
  // We deliberately do NOT apply the 1/√(λ+eps) diffusion-map weighting an earlier
  // draft used: that put the LARGEST weight on the SMALLEST eigenvalue's axis and,
  // at λ≈0, scaled one coordinate by ~1/√eps, letting a single near-degenerate mode
  // dominate Euclidean k-means (and risking a blow-up). The fix removes it.
  //
  // Row normalization is the load-bearing step for a signed cut. The discriminative
  // information for a k-way signed partition concentrates in the few lowest modes,
  // but the higher modes inside a (near-)degenerate eigenvalue bulk are arbitrary
  // within-cluster axes whose raw spatial spread otherwise dominates the k-means
  // geometry and splits a cluster instead of separating the signed groups. Scaling
  // every row to the unit sphere removes that magnitude artifact and makes k-means
  // key on the ANGLE between embedding rows — co-grouped instruments point the same
  // way, anti-correlated groups point apart — which is exactly the SPONGE contract.
  // A zero row (an isolated node with no embedding energy) is left at the origin
  // rather than divided by ~0, so there is no blow-up.
  const Eigen::Index kk = std::min<Eigen::Index>(k, n);
  MatX embed(n, kk);
  for (Eigen::Index c = 0; c < kk; ++c) {
    embed.col(c) = vecs.col(c); // ascending order -> bottom-k already leftmost
  }
  for (Eigen::Index r = 0; r < n; ++r) {
    const f64 norm = embed.row(r).norm();
    if (norm > eps) {
      embed.row(r) /= norm;
    }
  }

  const std::vector<int> raw = kmeans_deterministic(embed, k, eps);
  return Ok(canonicalize(raw));
}

} // namespace detail

// Partition an N×N symmetric similarity matrix into clusters.
//
// Inputs:
//   * `sim` — the N×N symmetric similarity / correlation matrix (treated as
//     symmetric). N ≥ 1 is required.
//   * `cfg` — algorithm + parameters (k must satisfy 1 ≤ k ≤ N).
//
// Returns the canonical labeling, or Err on a non-square / empty `sim`, a k out
// of range, or a numerical failure (InvalidArgument / Internal, matching the
// linalg convention). The label vector is always canonicalized by ascending
// smallest-member index, so it is a pure function of the recovered partition.
[[nodiscard]] inline Result<Clustering> cluster(const MatX &sim, ClusterConfig cfg = {}) {
  if (sim.rows() != sim.cols() || sim.rows() == 0) {
    return Err(ErrorCode::InvalidArgument, "cluster: sim must be square and non-empty");
  }
  const int n = static_cast<int>(sim.rows());
  if (cfg.k < 1 || cfg.k > n) {
    return Err(ErrorCode::InvalidArgument, "cluster: k must satisfy 1 <= k <= N");
  }
  const f64 eps = cfg.eps > 0.0 ? cfg.eps : 1e-12;

  switch (cfg.algo) {
  case Algo::Hierarchical:
    return Ok(detail::hierarchical_partition(sim, cfg.k, cfg.linkage, eps));
  case Algo::SpongeSym:
    return detail::sponge_partition(sim, cfg.k, cfg.tau_plus, cfg.tau_minus, eps);
  }
  return Err(ErrorCode::InvalidArgument, "cluster: unknown algorithm");
}

} // namespace atx::core::cluster
