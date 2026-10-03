#pragma once

// atx::engine::research::eval -- the house's CSCV PBO report (platform P9 B2, K-P9-5 `pbo`),
// the block backtest_integrity.cscv_pbo printed, over the ONE engine CSCV
// (eval::pbo_cscv_traced, rule CachedMomentsV2; Bailey, Borwein, Lopez de Prado & Zhu 2017).
// The engine visits every split in lexicographic order, picks the IS winner as the first
// maximum of mean / population sd and ranks it OOS with ties to the lower index, exactly the
// Python's rule; this file adds the summaries the house prints:
//
//   pbo                      share of splits whose logit is <= 0 (the engine's)
//   logit_mean, quantiles    numpy mean (pairwise) and linear quantiles p5..p95 of the logits
//   logit_distribution       per OOS rank k (0 = worst): log((k + 1) / (N - k)) and its splits
//   winner IS / OOS SR       the per-split winner's Sharpes, mean x sqrt(252)
//   prob_winner_oos_loss     share of splits whose winner's OOS Sharpe is below 0
//   degradation_slope        cov(IS, OOS, ddof 1) / var(IS, ddof 1) of the winners; empty with
//                            one split or a constant IS series
//   winner_counts            splits each candidate won
//
// Exhaustive only (the engine bounds S <= 16 blocks, C(16, 8) = 12,870 splits); the Python's
// seeded subsample of splits has no engine twin and is refused by the verb.

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::eval {

inline constexpr usize kPboDefaultBlocks = 16U;
inline constexpr usize kPboMinCells = 4U; // backtest_integrity.PBO_MIN_CELLS
inline constexpr std::array<f64, 5> kLogitQuantiles = {0.05, 0.25, 0.5, 0.75, 0.95};

struct CscvReport {
  f64 pbo{};
  usize n_candidates{};
  usize sessions{};
  usize blocks{};
  usize block_width{};
  usize dropped_tail_sessions{};
  usize splits{};
  usize splits_total{};
  f64 logit_mean{};
  std::array<f64, 5> logit_quantiles{}; // p5 p25 p50 p75 p95
  std::vector<f64> rank_logits;          // per OOS rank k
  std::vector<usize> rank_splits;        // splits whose winner had OOS rank k
  f64 winner_is_sr_annual_mean{};
  f64 winner_oos_sr_annual_mean{};
  f64 prob_winner_oos_loss{};
  std::optional<f64> degradation_slope;
  std::vector<usize> winner_counts;
  std::vector<f64> logits; // per split, split order
  // The engine's diagnostics (a V2 estimate resolved by the reference gather is counted there).
  u64 cached_evaluations{};
  u64 reference_evaluations{};
  u64 ambiguous_comparisons{};
};

// CSCV of a T x N date-major matrix (row t, candidate k at t * N + k: align_many's layout).
// InvalidArgument when N < 2, blocks is odd, < 2, > 16 or > T, or a used return is not finite.
[[nodiscard]] core::Result<CscvReport> cscv_report(std::span<const f64> matrix, usize sessions,
                                                   usize n_candidates, usize blocks);

} // namespace atx::engine::research::eval
