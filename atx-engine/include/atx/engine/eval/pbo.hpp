#pragma once

// atx::engine::eval — Probability of Backtest Overfitting (PBO) via the
// Combinatorially-Symmetric Cross-Validation (CSCV) framework.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  Given a performance matrix of N candidate strategies over T periods, PBO is
//  the probability that the strategy that looks best IN-SAMPLE (IS) ranks at or
//  below the OUT-OF-SAMPLE (OOS) median — i.e. the in-sample winner is, more
//  often than chance, an OOS dud. A genuine persistent edge drives PBO toward 0;
//  pure noise drives it toward 0.5 (the IS-best lands at a uniformly-random OOS
//  rank, half of which are at/below the median).
//
//  Reference: D. H. Bailey, J. M. Borwein, M. López de Prado, Q. J. Zhu,
//  "The Probability of Backtest Overfitting", Journal of Computational Finance
//  20(4), 2017 (§3.4 — CSCV). This file implements that procedure verbatim, with
//  one documented input convention (returns are candidate-major, M[c*T+t]).
//
// ===========================================================================
//  The CSCV procedure (as implemented)
// ===========================================================================
//  * Infer T = perf.size() / n_candidates. Trim trailing periods so S | T:
//    T_used = (T/S)*S; the trailing (T % S) periods are discarded (documented —
//    CSCV requires S contiguous EQUAL-width sub-periods). Width w = T_used / S.
//  * Enumerate ALL C(S, S/2) IS sub-period selections in LEXICOGRAPHIC order via
//    a deterministic next_combination index walk (NO RNG). The complement is OOS.
//  * Per split:
//      1. IS Sharpe of each candidate over the concatenation of its returns in
//         the IS sub-periods (non-annualized = mean/std, std==0 ⇒ 0). The IS-best
//         candidate n* = argmax IS Sharpe, ascending-index tie-break (first max).
//      2. OOS Sharpe of each candidate over the OOS sub-periods. Rank all N
//         candidates ascending by OOS Sharpe (best OOS ⇒ highest rank).
//      3. Relative rank of the IS-best: w_bar = (rank0[n*] + 1) / (N + 1), which
//         lies strictly in (0, 1). Logit lambda = ln( w_bar / (1 - w_bar) ),
//         always finite. lambda <= 0 ⇔ IS-best at/below the OOS median.
//  * PBO = fraction of splits with lambda <= 0. Also returns the per-split lambda
//    vector and the mean lambda.
//
// ===========================================================================
//  Numeric / determinism conventions (load-bearing)
// ===========================================================================
//  * Reductions are ORDER-FIXED (ascending index) so the result is run-to-run
//    byte-identical. No RNG anywhere — the combination walk is deterministic.
//  * Sharpe here is NON-ANNUALIZED (mean/std); std == 0 (constant/empty subset)
//    yields 0, consistent with the eval spine's degenerate-variance handling.
//  * Per-candidate tie-break for the IS argmax is ascending index (first max
//    wins); OOS ranking ties resolve by ascending index via core::partial_rank.

#include <cmath>   // std::log
#include <span>    // std::span
#include <string>  // std::string (Err message; included by error.hpp but explicit)
#include <vector>  // std::vector

#include "atx/core/error.hpp"            // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp"            // ATX_ASSERT
#include "atx/core/stats/algo.hpp"       // stats::partial_rank
#include "atx/core/types.hpp"            // atx::f64, atx::usize
#include "atx/engine/eval/stats_ext.hpp" // eval::mean_std_pop, eval::MeanStd

namespace atx::engine::eval {

// ===========================================================================
//  PboResult — the PBO scalar plus per-split diagnostics.
//
//  Rule of Zero: a small aggregate owning only its split_logits vector.
//    pbo          — fraction of splits whose IS-best lands at/below OOS median.
//    split_logits — one logit lambda per split, in lexicographic split order.
//    mean_logit   — arithmetic mean of split_logits (0 if there are no splits).
// ===========================================================================
struct PboResult {
  atx::f64 pbo;
  std::vector<atx::f64> split_logits;
  atx::f64 mean_logit;
};

namespace detail {

// ---------------------------------------------------------------------------
//  binomial — C(n, k), computed multiplicatively to size the split vector
//  without overflowing a factorial. Returns 0 when k > n. Uses the symmetry
//  C(n, k) == C(n, n-k) to keep the running product small.
//
//  SAFETY (no intermediate overflow): after iteration i the running product is
//  EXACTLY C(n - kk + 1 + i, i + 1), itself a binomial coefficient and therefore
//  an integer that never exceeds the final C(n, kk) = C(n, k). So no intermediate
//  is larger than the result; the only overflow risk is the result itself, and
//  the checked guard keeps S small and even (practical CSCV uses S <= 16, whose
//  largest split count C(16, 8) = 12870 is far inside usize). The division by
//  (i + 1) is exact (the product of i+1 consecutive integers is divisible by
//  (i+1)!), so the running value stays a true integer at every step.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::usize binomial(atx::usize n, atx::usize k) noexcept {
  if (k > n) {
    return 0U;
  }
  const atx::usize kk = (k > n - k) ? (n - k) : k; // smaller arm, by symmetry
  atx::usize result = 1U;
  for (atx::usize i = 0U; i < kk; ++i) {
    [[maybe_unused]] const atx::usize prev = result;
    // result *= (n - kk + 1 + i); result /= (i + 1) — exact at each step because
    // the running product is always a binomial coefficient (an integer).
    result = result * (n - kk + 1U + i) / (i + 1U);
    // C(m, j) is non-decreasing as the walk grows the coefficient toward C(n, kk);
    // a decrease would signal a wraparound (overflow), which the S<=16 guard rules out.
    ATX_ASSERT(result >= prev);
  }
  return result;
}

// ---------------------------------------------------------------------------
//  next_combination — advance a strictly-ascending k-subset of [0, n) to the
//  lexicographically next one. Returns false when `comb` is the final (largest)
//  combination, leaving it unchanged. Deterministic; no allocation; no RNG.
//
//  Standard index walk: find the rightmost element that can be incremented
//  (comb[i] < n - k + i), bump it, then reset every element to its right to the
//  minimal increasing run. `comb` must hold k strictly-ascending indices < n.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool next_combination(std::span<atx::usize> comb, atx::usize n) noexcept {
  const atx::usize k = comb.size();
  if (k == 0U) {
    return false; // the empty subset is its own only combination
  }
  // Walk from the right to the first index that has headroom to increment.
  atx::usize i = k; // one past the last; loop decrements before use
  while (i > 0U) {
    --i;
    const atx::usize ceiling = n - k + i; // max legal value at position i
    if (comb[i] < ceiling) {
      ++comb[i];
      // Reset the suffix to the minimal ascending run after comb[i].
      for (atx::usize j = i + 1U; j < k; ++j) {
        comb[j] = comb[j - 1U] + 1U;
      }
      return true;
    }
  }
  return false; // already the last combination
}

// ---------------------------------------------------------------------------
//  subset_sharpe — non-annualized Sharpe (mean/std) of one candidate's returns
//  over a chosen set of contiguous sub-periods.
//
//  The returns are gathered ascending: outer loop over the chosen sub-period
//  indices (already ascending), inner loop over the period offset within a
//  sub-period — so the concatenated stream is in ascending global-period order
//  and the reduction inside mean_std_pop is order-fixed. std == 0 ⇒ 0.
//
//    row     — candidate c's full T-period return row, M[c*T .. c*T+T).
//    subset  — ascending sub-period indices to include (each in [0, S)).
//    width   — periods per sub-period (w).
//    scratch — reused buffer (cleared then refilled) to avoid per-call alloc.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 subset_sharpe(std::span<const atx::f64> row,
                                            std::span<const atx::usize> subset,
                                            atx::usize width,
                                            std::vector<atx::f64> &scratch) {
  scratch.clear();
  for (const atx::usize sub : subset) {
    const atx::usize base = sub * width;
    // SAFETY: the gather reads row[base .. base+width). Every sub-period index is
    // < S and width = T_used/S, so base+width <= T_used <= row.size(); asserted to
    // pin the precondition (a malformed subset/width would otherwise read OOB).
    ATX_ASSERT(base + width <= row.size());
    for (atx::usize off = 0U; off < width; ++off) {
      scratch.push_back(row[base + off]);
    }
  }
  const MeanStd ms = mean_std_pop(std::span<const atx::f64>{scratch});
  if (ms.std == 0.0) {
    return 0.0;
  }
  return ms.mean / ms.std;
}

// ---------------------------------------------------------------------------
//  SplitScratch — per-call reusable buffers for one CSCV run, allocated once and
//  reused across every split to keep the inner loop allocation-free.
//    oos_set — the OOS complement sub-period indices for the current split.
//    in_is   — membership table over [0, S) for the current IS-set.
//    concat  — Sharpe gather buffer (cleared/refilled per candidate per side).
//    is_sh   — per-candidate IS Sharpe.
//    oos_sh  — per-candidate OOS Sharpe.
//    oos_rank— per-candidate ascending OOS rank (partial_rank output).
// ---------------------------------------------------------------------------
struct SplitScratch {
  std::vector<atx::usize> oos_set;
  std::vector<bool> in_is;
  std::vector<atx::f64> concat;
  std::vector<atx::f64> is_sh;
  std::vector<atx::f64> oos_sh;
  std::vector<atx::usize> oos_rank;
};

// ---------------------------------------------------------------------------
//  split_logit — the per-split body of CSCV: given the IS sub-period set, derive
//  the OOS complement, find the IS-best candidate (max IS Sharpe, first-max
//  tie-break), rank all candidates by OOS Sharpe, and return the IS-best's logit
//  lambda = ln( w_bar / (1 - w_bar) ), w_bar = (rank0[n*]+1)/(N+1) in (0,1).
//
//  Pure w.r.t. `perf`; mutates only the caller-owned `sc` scratch (reused across
//  splits). Preconditions: n >= 2; is_set holds S/2 ascending indices in [0, S);
//  width = T_used/S; row stride is `periods`.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 split_logit(std::span<const atx::f64> perf, atx::usize n,
                                          atx::usize periods, atx::usize n_splits,
                                          atx::usize width, std::span<const atx::usize> is_set,
                                          SplitScratch &sc) {
  // Build the OOS complement and a membership table for this IS-set.
  for (atx::usize i = 0U; i < n_splits; ++i) {
    sc.in_is[i] = false;
  }
  for (const atx::usize sub : is_set) {
    sc.in_is[sub] = true;
  }
  atx::usize w = 0U;
  for (atx::usize i = 0U; i < n_splits; ++i) {
    if (!sc.in_is[i]) {
      sc.oos_set[w] = i;
      ++w;
    }
  }

  // (1)/(2) IS & OOS Sharpe per candidate; pick the IS-best (first-max tie-break).
  atx::usize best_is = 0U;
  atx::f64 best_is_sharpe = 0.0;
  for (atx::usize c = 0U; c < n; ++c) {
    const std::span<const atx::f64> row{perf.data() + c * periods, periods};
    sc.is_sh[c] = subset_sharpe(row, is_set, width, sc.concat);
    sc.oos_sh[c] = subset_sharpe(row, std::span<const atx::usize>{sc.oos_set}, width, sc.concat);
    // Strict '>' keeps the FIRST maximum (ascending-index tie-break).
    if (c == 0U || sc.is_sh[c] > best_is_sharpe) {
      best_is_sharpe = sc.is_sh[c];
      best_is = c;
    }
  }

  // (3) Rank candidates ascending by OOS Sharpe (best OOS ⇒ highest rank0).
  atx::core::stats::partial_rank<atx::f64>(std::span<const atx::f64>{sc.oos_sh},
                                           std::span<atx::usize>{sc.oos_rank});
  const atx::f64 denom = static_cast<atx::f64>(n + 1U);                            // (N+1)
  const atx::f64 w_bar = (static_cast<atx::f64>(sc.oos_rank[best_is]) + 1.0) / denom; // (0,1)
  return std::log(w_bar / (1.0 - w_bar));                                          // finite
}

// ---------------------------------------------------------------------------
//  pbo_cscv_core — the CSCV procedure. `periods` is the already-computed
//  T = perf.size() / n_candidates (passed in so the checked wrapper reuses its
//  own division). Preconditions: n_candidates >= 2, n_splits positive & even,
//  n_splits <= periods. Decomposed out of the public entry points so both share
//  one definition of the procedure.
// ---------------------------------------------------------------------------
[[nodiscard]] PboResult pbo_cscv_core(std::span<const atx::f64> perf,
                                      atx::usize n_candidates,
                                      atx::usize n_splits,
                                      atx::usize periods);

} // namespace detail

// ===========================================================================
//  pbo_cscv_checked — validated CSCV PBO. Returns InvalidArgument when:
//    * n_candidates < 2                    (no cross-sectional ranking possible),
//    * n_splits is odd (or zero)           (CSCV needs an even S to split S/2),
//    * n_splits > T (= perf.size()/N)      (cannot form S non-empty sub-periods).
//  On success, see PboResult.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<PboResult>
pbo_cscv_checked(std::span<const atx::f64> perf, atx::usize n_candidates, atx::usize n_splits) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  using atx::core::Ok;

  if (n_candidates < 2U) {
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: n_candidates must be >= 2");
  }
  if (n_splits == 0U || (n_splits % 2U) != 0U) {
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: n_splits must be a positive even number");
  }
  // n_candidates >= 2 guarantees a non-zero divisor; integer division floors T.
  const atx::usize periods = perf.size() / n_candidates;
  if (n_splits > periods) {
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: n_splits must not exceed T (periods)");
  }
  return Ok(detail::pbo_cscv_core(perf, n_candidates, n_splits, periods));
}

// ===========================================================================
//  pbo_cscv — unchecked convenience for inputs known to be valid (n_candidates
//  >= 2, n_splits positive & even, n_splits <= T). It delegates to the checked
//  variant and FAILS FAST on a precondition violation (ATX_ASSERT, compiled out
//  under NDEBUG) rather than running an undefined-behavior path — a safety-
//  critical library never silently consumes malformed input. Call
//  pbo_cscv_checked directly when validity is not statically guaranteed.
// ===========================================================================
[[nodiscard]] inline PboResult
pbo_cscv(std::span<const atx::f64> perf, atx::usize n_candidates, atx::usize n_splits) {
  auto r = pbo_cscv_checked(perf, n_candidates, n_splits);
  // SAFETY: precondition contract — a release build with NDEBUG trusts the caller;
  // a debug build traps the invalid argument instead of proceeding into UB.
  ATX_ASSERT(r.has_value());
  return *r;
}

} // namespace atx::engine::eval
