#pragma once

// CSCV Probability of Backtest Overfitting, candidate-major M[c*T+t].
// Equal contiguous blocks use T_used=(T/S)*S; trailing periods are ignored.
// All C(S,S/2) splits are visited lexicographically. IS winner is first maximum;
// its OOS ascending rank breaks ties by candidate index. Logit is
// log(w/(1-w)), w=(rank0+1)/(N+1); PBO is the fraction of logits <=0.
//
// CachedMomentsV2 is the deliberate production default. It caches centered
// block moments and counts only the winner's rank. Ambiguous comparisons and
// unstable moments fall back to the original ordered two-pass gather. This is
// deterministic within a rule, not a universal floating-point bit-equivalence
// claim. LegacyGatherV1 preserves the old arithmetic/sort on valid inputs.
// Persist the selected rule in research/config identity when reusing artifacts.
//
// Both rules reject nonfinite USED returns and nonfinite reference Sharpes;
// those have no valid ordering under the former std::stable_sort comparator.
// Trimmed returns are neither validated nor used. Sharpe is mean/population std,
// with zero std ->0; no annualization, interpolation or missing-value imputation.

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/engine/eval/pbo_config.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/combinatorics.hpp"

namespace atx::engine::eval {

struct PboResult {
  atx::f64 pbo;
  std::vector<atx::f64> split_logits;
  atx::f64 mean_logit;
  PboRule rule{PboRule::CachedMomentsV2};
  // Diagnostics do not change inference or split ordering. One evaluation is
  // one candidate/side/split. Reference includes all legacy evaluations or V2
  // fallbacks; cached counts attempted V2 moment estimates, including fallback.
  atx::u64 cached_evaluations{0};
  atx::u64 reference_evaluations{0};
  atx::u64 ambiguous_comparisons{0};
};


// Validated entry: N>=2; rectangular N*T matrix; even 2<=S<=16 and S<=T;
// finite used inputs and finite reference scores. Returns InvalidArgument on
// violation. Cache O(N*S), scratch O(N+T); no per-split allocation in V2.
[[nodiscard]] atx::core::Result<PboResult>
pbo_cscv_checked(std::span<const atx::f64> perf, atx::usize n_candidates,
                 atx::usize n_splits, PboRule rule = PboRule::CachedMomentsV2);

// One split of a traced run (P9 B2: the research eval verb's CSCV report reads the
// winner's identity and Sharpes from here, so no second CSCV exists). winner = the IS
// first maximum, rank = its OOS ascending rank (0 = worst; the logit's argument),
// is_sharpe / oos_sharpe = the winner's Sharpes as the rule compared them (a V2 cached
// estimate unless the split resolved it exactly).
struct PboSplitTrace {
  atx::usize winner{};
  atx::usize rank{};
  atx::f64 is_sharpe{};
  atx::f64 oos_sharpe{};
};

// pbo_cscv_checked with one PboSplitTrace per split in split order (`trace` is
// replaced). The result, its logits and its diagnostics equal pbo_cscv_checked's: the
// trace records values the rule computes anyway and adds no comparison.
[[nodiscard]] atx::core::Result<PboResult>
pbo_cscv_traced(std::span<const atx::f64> perf, atx::usize n_candidates, atx::usize n_splits,
                PboRule rule, std::vector<PboSplitTrace> &trace);

// Convenience wrapper for the SAME checked preconditions, including rectangular
// shape, S<=16 and finite used values/scores. Fails fast in every build on error;
// call pbo_cscv_checked when validity is not statically guaranteed.
[[nodiscard]] inline PboResult
pbo_cscv(std::span<const atx::f64> perf, atx::usize n_candidates, atx::usize n_splits,
         PboRule rule = PboRule::CachedMomentsV2) {
  auto result = pbo_cscv_checked(perf, n_candidates, n_splits, rule);
  ATX_CHECK(result.has_value());
  return std::move(*result);
}
} // namespace atx::engine::eval
