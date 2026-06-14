#pragma once

// atx::engine::learn — the NN-alpha DEFLATION + PBO admission gate (p2 S5-4, R4/R8).
//
// =====================================================================
//  What this header is
// =====================================================================
//  ONE honest admission gate over a SWEEP of NN-alpha candidates (the architecture
//  x seed search that an NN model selection actually runs). It REUSES the existing
//  eval spine verbatim — eval::deflated_sharpe (the Bailey-LdP DSR) and
//  eval::pbo_cscv (the CSCV PBO) — and adds ONLY the one thing NN scale needs to
//  be honest: the trial-count aggregation across the WHOLE sweep.
//
//  The single biggest snooping risk in an NN search (R4) is deflating the winner
//  by its OWN trial_count while the SELECTION ran over every architecture x seed.
//  The expected-maximum Sharpe SR*_N grows with N, so under-counting N inflates the
//  DSR of whichever candidate happened to look best. This gate fixes the deflation
//  N to the SUM of all candidates' trial_count — the full sweep is in the count —
//  so the winner is deflated by the true number of trials the selection performed.
//
//    * n_trials  = Σ_c candidates[c].trial_count  (any 0 counted as 1 — a fit
//                  happened, so N is never UNDER-counted). This is the honest N.
//    * winner    = argmax_c mean(candidates[c].oos_score_series) (ascending-index
//                  tie-break — the first max), the best-OOS-skill candidate.
//    * dsr       = oos_deflated_sharpe's EXACT formula on the winner's series, but
//                  with N = n_trials (NOT the winner's own trial_count).
//    * pbo       = eval::pbo_cscv over the candidates' equal-length OOS rows (the
//                  flattened [n x T] matrix), the CSCV overfit probability.
//    * admit     = dsr > dsr_min && pbo < pbo_max  (the ~20-30% PBO bar, R4).
//
//  PURE over the candidates: no RNG, no SIMD; every fold (the trial-count sum, the
//  winner argmax, the perf-matrix fill) is ascending-index, so the result is
//  run-to-run byte-identical (R1). The gate adds NO new gate math — it is a thin
//  orchestration over the eval inline functions, so it is header-only (no .cpp).
//
// Header-only; every member / free function is defined inline.

#include <optional> // std::nullopt (single-stream variance path of deflated_sharpe)
#include <span>     // std::span (non-owning candidate view)
#include <vector>   // std::vector (the flattened PBO perf matrix)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/pbo.hpp"             // eval::pbo_cscv_checked, PboResult
#include "atx/engine/eval/stats_ext.hpp"       // eval::mean_std_pop, skewness, excess_kurtosis
#include "atx/engine/learn/learned_source.hpp" // LearnedModel

namespace atx::engine::learn {

// ===========================================================================
//  NnGateCfg — the admission thresholds (tiny by design).
//
//  n_splits : CSCV splits for the PBO (must be EVEN and <= T, the OOS-series
//             length); the deeper the split the finer the overfit estimate.
//  dsr_min  : admit requires the winner's sweep-deflated DSR strictly above this.
//  pbo_max  : admit requires the CSCV PBO strictly below this (the ~20-30% bar).
// ===========================================================================
struct NnGateCfg {
  atx::usize n_splits = 8;   // CSCV splits for PBO (even, <= T)
  atx::f64 dsr_min = 0.0;    // admit requires DSR > dsr_min
  atx::f64 pbo_max = 0.30;   // admit requires PBO < pbo_max (the ~20-30% bar, R4)
};

// ===========================================================================
//  NnGateResult — the gate verdict + its load-bearing diagnostics.
//
//  Trivial aggregate (Rule of Zero); owns nothing.
//    dsr      : deflated Sharpe of the WINNER, deflated by the FULL sweep N.
//    pbo      : CSCV PBO over the candidate sweep.
//    n_trials : the honest deflation N = Σ candidates' trial_count (R4).
//    winner   : index of the best-mean-OOS-skill candidate.
//    admit    : dsr > dsr_min && pbo < pbo_max.
// ===========================================================================
struct NnGateResult {
  atx::f64 dsr{0.0};
  atx::f64 pbo{1.0};
  atx::usize n_trials{0};
  atx::usize winner{0};
  bool admit{false};
};

// ===========================================================================
//  gate_nn_sweep — gate a SWEEP of NN candidates (architecture x seed).
//
//  Each candidate carries its own per-date oos_score_series (ascending OOS skill)
//  + trial_count. See the header preamble for the contract. Returns:
//    - Err(InvalidArgument) when candidates.size() < 2 (PBO needs >= 2),
//    - Err(InvalidArgument) when any oos_score_series is empty or they are not all
//      the SAME length T (PBO needs an aligned [n x T] matrix),
//    - the pbo_cscv_checked error when n_splits is odd/zero or > T,
//    - Ok(NnGateResult) otherwise.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<NnGateResult>
gate_nn_sweep(std::span<const LearnedModel> candidates, const NnGateCfg &cfg) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  using atx::core::Ok;

  // (1) Validate: PBO needs >= 2 candidates, each with a non-empty OOS series, and
  //     all series the SAME length T (the aligned [n x T] matrix the CSCV ranks).
  if (candidates.size() < 2U) {
    return Err(ErrorCode::InvalidArgument, "gate_nn_sweep: need >= 2 candidates");
  }
  const atx::usize T = candidates[0].oos_score_series.size();
  if (T == 0U) {
    return Err(ErrorCode::InvalidArgument, "gate_nn_sweep: oos_score_series must be non-empty");
  }
  for (const LearnedModel &c : candidates) {
    if (c.oos_score_series.size() != T) {
      return Err(ErrorCode::InvalidArgument,
                 "gate_nn_sweep: all oos_score_series must be the same length T");
    }
  }

  NnGateResult res{};

  // (2) Honest deflation N = Σ trial_count, ascending. A 0 trial_count is counted
  //     as 1 (a fit happened) so N is never UNDER-counted (the conservative floor).
  res.n_trials = 0U;
  for (const LearnedModel &c : candidates) {
    res.n_trials += (c.trial_count == 0U) ? 1U : c.trial_count;
  }

  // (3) Winner = argmax mean(oos_score_series), ascending-index tie-break (first max).
  atx::f64 best_mean = 0.0;
  for (atx::usize c = 0U; c < candidates.size(); ++c) {
    const atx::f64 m =
        eval::mean_std_pop(std::span<const atx::f64>{candidates[c].oos_score_series}).mean;
    if (c == 0U || m > best_mean) {
      best_mean = m;
      res.winner = c;
    }
  }

  // (4) Winner DSR: oos_deflated_sharpe's EXACT formula, but with N = n_trials (the
  //     full sweep — NOT the winner's own trial_count). Degenerate (T<2 or std==0)
  //     -> 0 (no edge), matching oos_deflated_sharpe.
  const std::span<const atx::f64> win{candidates[res.winner].oos_score_series};
  const eval::MeanStd ms = eval::mean_std_pop(win);
  if (T >= 2U && ms.std != 0.0) {
    const atx::f64 sr = ms.mean / ms.std;
    const atx::f64 skew = eval::skewness(win);
    const atx::f64 exkurt = eval::excess_kurtosis(win);
    res.dsr = eval::deflated_sharpe(sr, T, skew, exkurt, res.n_trials, std::nullopt).dsr;
  }

  // (5) PBO: flatten the candidates' equal-length OOS rows into the [n x T] perf
  //     matrix (row c = candidate c's series, ascending) and run the CSCV PBO.
  std::vector<atx::f64> perf;
  perf.reserve(candidates.size() * T);
  for (const LearnedModel &c : candidates) {
    for (const atx::f64 v : c.oos_score_series) {
      perf.push_back(v);
    }
  }
  auto pbo = eval::pbo_cscv_checked(std::span<const atx::f64>{perf}, candidates.size(), cfg.n_splits);
  if (!pbo.has_value()) {
    return Err(pbo.error().code(), pbo.error().message()); // surface the CSCV error verbatim
  }
  res.pbo = pbo->pbo;

  // (6) Admit iff the winner clears BOTH bars (strict on each).
  res.admit = (res.dsr > cfg.dsr_min) && (res.pbo < cfg.pbo_max);
  return Ok(res);
}

} // namespace atx::engine::learn
