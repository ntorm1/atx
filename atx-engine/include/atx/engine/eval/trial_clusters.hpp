#pragma once

// atx::engine::eval — trial clustering (ONC-style) and the Monte-Carlo null of
// the maximum Sharpe ratio under an estimated trial correlation (W0-E0b, E-01).
//
// ===========================================================================
//  Why
// ===========================================================================
//  The Deflated Sharpe Ratio needs a benchmark SR* = E[max_i SR_i] over the
//  trials the winner was selected from. The Bailey-LdP closed form assumes N
//  INDEPENDENT trials with a common variance V. Research trials are not
//  independent: a miner produces families of near-duplicates. The plan's rule
//  (findings §3.4, López de Prado & Lewis 2019, "Detection of false investment
//  strategies using unsupervised learning methods") is:
//    * cluster the trials on their PnL correlation (ONC),
//    * N = the number of clusters, V = the variance of the cluster
//      representative Sharpes,
//  and, as a cross-check, a Monte-Carlo E[max] under the estimated correlation
//  (no independence assumption at all).
//
// ===========================================================================
//  onc_cluster — the clustering (deterministic, seeded, no clock)
// ===========================================================================
//  Input: an n x n correlation matrix R (row-major, symmetric, unit diagonal).
//  Distance: ONC's correlation distance D_ij = sqrt((1 - R_ij) / 2) in [0, 1].
//  Base stage: for every k in [2, k_max] run kernel k-means on the trials'
//  unit vectors (Euclidean distance between unit vectors is 2·D_ij, so the
//  k-means objective and ONC's distance agree), best of `n_init` seeded
//  restarts by within-cluster SSE (restart 0 is seeded farthest-first, which
//  puts one seed in every block of a clean block model; the others use greedy
//  k-means++ with 2 + ln k local trials); score each partition by ONC's
//  quality q = mean(silhouette) / sd(silhouette) and keep the best k.
//  Second stage (ONC "top"): clusters whose own quality is below the mean are
//  re-clustered together, recursively (depth-bounded); the refinement is kept
//  only when it raises the mean cluster quality (LdP's makeNewOutputs rule).
//  Degenerate shapes, decided before / after the search:
//    * every pairwise distance <= tight_distance -> ONE cluster (duplicates);
//    * best mean silhouette < min_silhouette     -> no block structure: every
//      trial is its own cluster (SINGLETONS). With singletons the cluster
//      benchmark reduces to N = n_raw and V = the cross-trial variance, which
//      is the correct E[max] for equicorrelated and independent trials alike.
//  Silhouette of a trial in a singleton cluster is 0 (Rousseeuw's convention).
//  Cost: O(n²) memory-free per k-means iteration (the kernel form never builds
//  centroids), O(n²) per silhouette pass; k_max defaults to min(n-1, 64).
//
// ===========================================================================
//  mc_max_sharpe_null — the Monte-Carlo cross-check
// ===========================================================================
//  Trials are given as n unit rows u_i in R^d whose Gram u_i·u_j is the trial
//  correlation (the TrialRegistry's normalized PnL sketches). A draw is
//  g ~ N(0, I_d), Z_i = u_i·g (so Cov(Z) = the estimated correlation, exactly,
//  PSD by construction), and the null maximum Sharpe is max_i null_sd[i]·Z_i,
//  with null_sd[i] = 1/sqrt(T_i) the sampling sd of trial i's per-period
//  Sharpe when its true Sharpe is 0. The sorted draws give E[max], sd, CDF and
//  quantiles. Deterministic for a given seed (splitmix64 + Box-Muller).
//  Cost: O(draws · n · d).

#include <algorithm> // std::upper_bound
#include <cmath>     // std::ceil
#include <limits>    // quiet_NaN
#include <span>
#include <vector>

#include "atx/core/error.hpp" // atx::core::Result
#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::u32, atx::u64, atx::usize

namespace atx::engine::eval {

// Default upper bound on k when OncConfig::max_k == 0.
inline constexpr atx::usize kOncDefaultMaxK = 64U;

struct OncConfig {
  atx::usize max_k{0};           // largest k tried; 0 -> min(n - 1, kOncDefaultMaxK)
  atx::usize n_init{4};          // seeded restarts per k (best SSE kept), >= 1
  atx::usize max_iter{50};       // Lloyd iterations per restart (bounded), >= 1
  atx::usize max_depth{2};       // ONC second-stage recursion depth (0 = base stage only)
  atx::u64 seed{0x0c1a55eedULL}; // k-means++ seed (the result is a pure function of it)
  atx::f64 tight_distance{0.1};  // max pairwise D <= this -> one cluster (rho >= 0.98)
  atx::f64 min_silhouette{0.2};  // best mean silhouette below this -> singletons
};

// How the partition was reached (reporting / tests).
enum class ClusterShape : atx::u8 {
  Empty = 0,      // n == 0
  OneCluster = 1, // n == 1, or every pairwise distance within tight_distance
  Singletons = 2, // no block structure found: every trial is its own cluster
  Blocks = 3,     // an ONC partition with >= 2 clusters
};

struct TrialClusters {
  atx::usize n_clusters{};
  std::vector<atx::u32> labels; // per trial (input order), in [0, n_clusters)
  atx::f64 quality{};           // ONC q = mean(s) / sd(s) of the partition (0 unless Blocks)
  atx::f64 mean_silhouette{};   // mean silhouette of the partition (0 unless Blocks)
  ClusterShape shape{ClusterShape::Empty};
};

// Cluster the n trials whose row-major n x n correlation matrix is `corr`.
// Err(InvalidArgument) when corr.size() != n*n, an entry is non-finite, or the
// config is invalid (n_init == 0, max_iter == 0, thresholds not finite).
[[nodiscard]] atx::core::Result<TrialClusters> onc_cluster(std::span<const atx::f64> corr,
                                                           atx::usize n, const OncConfig &cfg);

// Representative per-period Sharpe of every cluster: the Sharpe of the
// cluster's equal-risk portfolio of its members' standardized returns,
//   SR_c = mean_{i in c}(SR_i) / sqrt(max(mean_{i,j in c} R_ij, 1/m_c)),
// (m_c members; the 1/m_c floor is the independent-members value, so an
// anti-correlated cluster never inflates its representative). Output index =
// cluster label. Err(InvalidArgument) on shape mismatches.
[[nodiscard]] atx::core::Result<std::vector<atx::f64>>
cluster_representative_sharpes(std::span<const atx::f64> corr, atx::usize n,
                               std::span<const atx::f64> sharpes, const TrialClusters &clusters);

// Monte-Carlo null distribution of max_i SR_i (see header note).
struct McMaxNull {
  std::vector<atx::f64> sorted_max; // ascending draws of the null maximum Sharpe
  atx::f64 mean{};                  // E[max] — the Monte-Carlo SR* benchmark
  atx::f64 sd{};                    // sd of the null maximum

  // Fraction of draws <= x (the null CDF of the maximum); NaN when empty.
  [[nodiscard]] atx::f64 cdf(atx::f64 x) const noexcept {
    if (sorted_max.empty()) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    const auto it = std::upper_bound(sorted_max.begin(), sorted_max.end(), x);
    return static_cast<atx::f64>(it - sorted_max.begin()) /
           static_cast<atx::f64>(sorted_max.size());
  }
  // Empirical p-quantile (the ceil(p·B)-th smallest draw); NaN when empty or
  // p outside [0, 1].
  [[nodiscard]] atx::f64 quantile(atx::f64 p) const noexcept {
    if (sorted_max.empty() || !(p >= 0.0) || !(p <= 1.0)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    const atx::f64 pos = std::ceil(p * static_cast<atx::f64>(sorted_max.size()));
    const atx::usize idx = pos < 1.0 ? 0U : static_cast<atx::usize>(pos) - 1U;
    return sorted_max[std::min(idx, sorted_max.size() - 1U)];
  }
};

// unit_rows: n x d row-major unit vectors (Gram = trial correlation);
// null_sd: n per-trial null Sharpe sds (finite, > 0); draws in [1, 10^7].
// Err(InvalidArgument) on any shape / value violation.
[[nodiscard]] atx::core::Result<McMaxNull>
mc_max_sharpe_null(std::span<const atx::f64> unit_rows, atx::usize n, atx::usize d,
                   std::span<const atx::f64> null_sd, atx::usize draws, atx::u64 seed);

} // namespace atx::engine::eval
