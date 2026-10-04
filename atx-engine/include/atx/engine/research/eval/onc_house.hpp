#pragma once

// atx::engine::research::eval -- the house ONC (house_v1) and the effective number of trials
// (platform P9 B2, K-P9-5 `onc`). The registered procedure the verdicts printed
// (backtest_integrity.onc / effective_trials: Lopez de Prado & Lewis 2019, code as Lopez de
// Prado 2020 MLAM snippets 4.1-4.2), beside the engine's calibrated eval::onc_cluster
// (trial_clusters.hpp), which is a different algorithm (kernel k-means, SSE restarts,
// singleton / tight floors) and does not reproduce the house's clusters:
//
//   distance        x_ij = sqrt(max((1 - rho_ij) / 2, 0)); k-means runs on the ROWS of x
//   k-means         Lloyd, k-means++ seeding from the draws (first center integers(n), each
//                   next choice(n, p = d^2 / sum d^2), integers(n) when that sum is 0), argmin
//                   to the first nearest center, an empty cluster takes the point farthest from
//                   its own center, stop when labels repeat, at most 300 iterations
//   silhouettes     Euclidean (Gram form g_i + g_j - 2 x_i.x_j, floored at 0), 0 for a
//                   singleton or a one-cluster partition
//   base            best quality mean(s) / sd(s) over n_init x k = 2..max_k (strictly greater)
//   recursion       clusters whose t = mean / sd of their silhouettes is below the mean finite
//                   t are re-clustered together (>= 2 such clusters, >= 3 members), the result
//                   kept only when its mean finite t beats theirs; fewer than 3 trials are
//                   singletons
//   effective N     ONC on the trials' correlation (numpy corrcoef, NaN read as 0, unit
//                   diagonal); per cluster the inverse-variance mix of its members (equal
//                   weights when every variance is 0), its per-session SR (house_moments);
//                   V = ddof-1 variance of the defined cluster SRs
//
// The draws come from numpy's default_rng(seed) (numpy_rng.hpp), so the clusters equal the
// Python's; a test replays a recorded trace through the same interface.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/eval/house_moments.hpp"
#include "atx/engine/research/eval/numpy_rng.hpp"

namespace atx::engine::research::eval {

inline constexpr usize kKmeansMaxIter = 300U;

// The draws of the k-means++ seeding, in call order.
class KmeansDraws {
public:
  KmeansDraws() = default;
  KmeansDraws(const KmeansDraws &) = delete;
  KmeansDraws &operator=(const KmeansDraws &) = delete;
  KmeansDraws(KmeansDraws &&) = delete;
  KmeansDraws &operator=(KmeansDraws &&) = delete;
  virtual ~KmeansDraws() = default;
  // integers(n): an index in [0, n).
  [[nodiscard]] virtual core::Result<usize> integers(usize n) = 0;
  // choice(n, p): an index drawn with probabilities p (n = p.size()).
  [[nodiscard]] virtual core::Result<usize> choice(std::span<const f64> p) = 0;
};

// numpy.random.default_rng(seed) as KmeansDraws.
class NumpyKmeansDraws final : public KmeansDraws {
public:
  explicit NumpyKmeansDraws(u64 seed) noexcept : generator_(seed) {}
  [[nodiscard]] core::Result<usize> integers(usize n) override;
  [[nodiscard]] core::Result<usize> choice(std::span<const f64> p) override;

private:
  NumpyGenerator generator_;
};

struct OncPartition {
  std::vector<std::vector<usize>> clusters; // member indices, the Python cluster order
  std::vector<f64> silhouettes;             // per member, input order
};

// The house ONC of the n x n row-major correlation `corr` (members 0..n-1). n_init >= 1.
[[nodiscard]] core::Result<OncPartition> onc_house_v1(std::span<const f64> corr, usize n,
                                                      usize n_init, KmeansDraws &draws);

struct EffectiveTrials {
  usize n_trials{};
  usize n_eff{};
  usize sessions{};
  std::vector<std::vector<usize>> clusters;     // trial indices
  std::vector<std::optional<f64>> cluster_sr_daily;
  std::optional<f64> variance_sr;
  usize n_init{};
  u64 seed{};
  f64 mean_offdiag_corr{};
};

// effective_trials of a T x N date-major matrix (row t, column k at t * N + k). T >= 3.
[[nodiscard]] core::Result<EffectiveTrials> effective_trials(std::span<const f64> matrix,
                                                             usize sessions, usize n_trials,
                                                             usize n_init, u64 seed);
// The same with explicit draws (`seed` is reported only).
[[nodiscard]] core::Result<EffectiveTrials>
effective_trials_with(std::span<const f64> matrix, usize sessions, usize n_trials, usize n_init,
                      u64 seed, KmeansDraws &draws);

// The correlation effective_trials hands to ONC (numpy corrcoef of the columns, NaN read as 0,
// unit diagonal), N x N row-major.
[[nodiscard]] std::vector<f64> trial_correlation(std::span<const f64> matrix, usize sessions,
                                                 usize n_trials);

// backtest_integrity.effective_n_dsr: one cell's DSR with N = N_eff and V = the clusters' SR
// variance (eval::house_dsr, method onc).
struct EffectiveNDsr {
  usize n_eff{};
  usize n_trials{};
  std::optional<f64> variance_sr;
  std::optional<f64> dsr;
  std::optional<f64> sr0_daily;
  std::optional<f64> sr0_annual;
  std::string variance_source;
};
[[nodiscard]] EffectiveNDsr effective_n_dsr(const HouseMoments &cell, const EffectiveTrials &eff);

} // namespace atx::engine::research::eval
