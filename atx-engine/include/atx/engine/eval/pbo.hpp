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
