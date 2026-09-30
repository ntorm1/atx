#pragma once

// atx::engine::combine — per-date marginal rank IC (platform v8 F-2, contract K6).
//
//   The orthogonality read-out asks one question of a candidate: does it predict the label
//   after the book it would join has been projected out? For one decision date:
//
//     1. the candidate row (callers pass centred ranks) is residualised on a few regressor rows
//        (the book composite and its theme composites; an intercept is always included) with
//        residualize_signal, over the names where the candidate and every regressor are finite;
//     2. on exactly the names where that residual and the label are finite (the paired names),
//        the label is ranked; the raw IC is the Spearman rank IC of the candidate (re-ranked on
//        the paired names, as the IC runner does) and the marginal IC is the correlation of the
//        residual itself with the label's ranks. The two series share one support per date.
//
//   The marginal IC is linear in the residual on purpose. The residual is already in centred-
//   rank units; re-ranking it would let a tiny ordered remainder of the projection decide the
//   order of every name the regressors explain, so a near-copy of the book would read as
//   strongly additive. Linear residualisation of rank-transformed signals still leaves any
//   nonlinear dependence on the book in the residual (the rank transform of book + noise bends
//   at the cross-section's edges): a small bias of either sign, not a zero-by-construction.
//
//   A date whose residual is spanned by the regressors (norm below 1e-10 of the candidate's
//   own dispersion on the residualised names) contributes a marginal IC of exactly 0: the
//   candidate adds nothing there, and the round-off residual's correlation would be noise.
//
//   Row-at-a-time by design: callers stream dates from cached payloads, so working memory is
//   O(names x rows), never O(dates x names). Mining and screening reuse the same kernel.
//
//   Series summaries are the mean of the defined daily values with a Bartlett HAC t at a
//   fixed lag (small-sample corrected, eval::hac::mean_inference), computed over the defined
//   dates compacted in order (undefined dates are dropped, not zero-filled).

#include <limits>  // std::numeric_limits
#include <span>    // std::span
#include <utility> // std::pair
#include <vector>  // std::vector

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u8, usize

namespace atx::engine::combine {

// At most the book composite plus ten theme composites.
inline constexpr atx::usize kMaxMarginalRegressors = 11U;
inline constexpr atx::f64 kMarginalNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Reusable buffers of the row kernels (grown on first use to the row width; never shrunk).
struct MarginalRankIcScratch {
  std::vector<atx::f64> ones;                            // intercept exposure
  std::vector<atx::f64> x_rank, y_rank;                  // average ranks on the paired names
  std::vector<atx::usize> names;                         // paired names, ascending
  std::vector<std::pair<atx::f64, atx::usize>> sorted;   // (value, name) sort buffer
};

// Centred tied rank of `values` over the names where `eligible` is nonzero (empty span: every
// name) and the value is finite: a tie group at sorted positions [b, e) of n such names gets
// (b + e - 1) / (2 (n - 1)) - 0.5, the composition's rank arithmetic. Other names get NaN; with
// fewer than 2 names the whole row is NaN. Err on a size mismatch.
[[nodiscard]] atx::core::Status
centred_tied_ranks(std::span<const atx::f64> values, std::span<const atx::u8> eligible,
                   std::span<atx::f64> out, std::vector<std::pair<atx::f64, atx::usize>> &sorted);

// One date of the marginal read-out. Both ICs are NaN when fewer than `min_names` names pair
// (or the correlation is undefined); `spanned` marks a date whose marginal IC was set to 0.
struct MarginalRankIcDay {
  atx::f64 raw_ic = kMarginalNaN;      // Spearman(candidate, label) on the paired names
  atx::f64 marginal_ic = kMarginalNaN; // Pearson(residual, rank of label) on the same names
  atx::usize names = 0U;
  atx::u8 spanned = 0U;
};

// `candidate`, `label` and each of `regressors` (at most kMaxMarginalRegressors; zero is a plain
// rank IC with an intercept-only residual) are rows of the same width. min_names >= 3. Err on a
// shape mismatch, too many regressors or min_names < 3.
[[nodiscard]] atx::core::Result<MarginalRankIcDay>
marginal_rank_ic_day(std::span<const atx::f64> candidate,
                     std::span<const std::span<const atx::f64>> regressors,
                     std::span<const atx::f64> label, atx::usize min_names,
                     MarginalRankIcScratch &scratch);

// Mean of the finite values of a daily series and its Bartlett HAC t at `hac_lag` (clamped to
// n - 1). `mean` is NaN with no defined date; `hac_t` is NaN when inference is undefined
// (fewer than 2 dates or zero long-run variance).
struct RankIcSummary {
  atx::f64 mean = kMarginalNaN;
  atx::f64 hac_t = kMarginalNaN;
  atx::usize dates = 0U;
};
[[nodiscard]] RankIcSummary summarize_rank_ic(std::span<const atx::f64> daily, atx::usize hac_lag,
                                              std::vector<atx::f64> &compact);

// Mean over dates of the per-date Pearson correlation of every pair of K rows over their jointly
// finite names, a date counting for a pair only when at least `min_names` names are joint and
// the correlation is defined. Rows are expected bounded (centred ranks): the one-pass moments
// cannot overflow and their cancellation is harmless at that scale.
class PairwiseRowCorrelation {
public:
  PairwiseRowCorrelation(atx::usize rows, atx::usize min_names);
  // `rows.size()` must equal the constructed row count and every row the same width.
  [[nodiscard]] atx::core::Status add_date(std::span<const std::span<const atx::f64>> rows);
  // NaN when the pair was never defined; a == b is NaN (not a pair).
  [[nodiscard]] atx::f64 mean(atx::usize a, atx::usize b) const noexcept;
  [[nodiscard]] atx::usize dates(atx::usize a, atx::usize b) const noexcept;

private:
  atx::usize rows_ = 0U;
  atx::usize min_names_ = 0U;
  std::vector<atx::f64> sum_;     // upper triangle, row-major a * rows_ + b (a < b)
  std::vector<atx::usize> count_; // same layout
};

} // namespace atx::engine::combine
