#pragma once

// atx::engine::validation — a reusable bias-audit gate battery: three
// test-support assertions that catch the canonical backtest-evaluation biases,
// plus a self-contained non-vacuous proof that the overfit gates actually fire.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  Three free assertions, each codifying one validity property a backtest must
//  satisfy. They are deliberately small, dependency-light, and PURE so they can
//  be reused VERBATIM as fixtures in later sprints (S3/S4/S5 reuse these as the
//  shared validity spine — do not fork a second copy):
//
//   * check_no_lookahead  — causality / no-look-ahead: an output is truncation-
//     invariant (its first `cut` entries do not change when the function is
//     re-run on only the first `cut` inputs instead of the full `full_n`).
//   * check_survivorship_frozen — survivorship bias: a delisted symbol's PnL is
//     PRESENT (not silently dropped) and FROZEN (exactly zero) after delisting.
//   * catches_overfit_synthetic — a non-vacuous demonstration that BOTH overfit
//     gates (PBO via CSCV, and the Deflated Sharpe multiple-testing correction)
//     fire on a planted-overfit synthetic, so a green suite proves the gates can
//     actually reject, not merely that they were called.
//
// ===========================================================================
//  Numeric conventions (load-bearing)
// ===========================================================================
//  * check_no_lookahead uses an EXACT (bit-for-bit) `!=` comparison: a causal
//    function must reproduce its earlier outputs identically, not merely closely
//    — any drift is a leak. No tolerance is appropriate here.
//  * check_survivorship_frozen tests against the exact literal 0.0; a delisted
//    book is frozen to zeros, so an exact compare is the correct contract.
//  * catches_overfit_synthetic is RNG-free and deterministic (the synthetic
//    matrix is constructed from index arithmetic), so its verdict is stable.

#include <algorithm> // std::min
#include <span>      // std::span

#include "atx/core/types.hpp" // atx::f64, atx::usize

namespace atx::engine::validation {

// ===========================================================================
//  check_no_lookahead — causality (no-look-ahead) check via truncation
//  invariance.
//
//  `recompute(n_visible)` re-derives the output seeing ONLY the first
//  `n_visible` input rows; it returns a sized, index-accessible range of f64
//  (e.g. std::vector<atx::f64>) — the generic `Recompute` is any callable with
//  that shape, so this assertion is reusable across feature/signal pipelines.
//
//  A causal function is truncation-invariant: its first `cut` outputs must be
//  identical whether it saw `full_n` rows or only `cut`. We compute both, take
//  m = min(cut, |full|, |trunc|) to stay in range of both outputs, and compare
//  element-wise with EXACT `!=` (a single differing entry => the function peeked
//  ahead => leak). Returns true iff the invariant holds over [0, m).
//
//  Requires: `recompute` returns a sized, index-accessible range of `atx::f64`
//  (supports `.size()` and `operator[]`, e.g. std::vector<atx::f64>); the first
//  min(cut, |full|, |trunc|) outputs of the two runs are compared bit-for-bit.
// ===========================================================================
template <class Recompute>
[[nodiscard]] inline bool check_no_lookahead(atx::usize full_n, atx::usize cut,
                                             Recompute recompute) {
  const auto full = recompute(full_n);
  const auto trunc = recompute(cut);
  // Stay within both produced ranges and within the `cut`-prefix being audited.
  const atx::usize full_sz = static_cast<atx::usize>(full.size());
  const atx::usize trunc_sz = static_cast<atx::usize>(trunc.size());
  const atx::usize m = std::min(cut, std::min(full_sz, trunc_sz));
  for (atx::usize i = 0U; i < m; ++i) {
    // SAFETY: EXACT bit-for-bit compare is intentional — a causal output must be
    //         reproduced identically, so any difference (even sub-epsilon) is a
    //         genuine look-ahead leak, never floating-point rounding noise.
    if (full[i] != trunc[i]) {
      return false;
    }
  }
  return true;
}

// ===========================================================================
//  check_survivorship_frozen — survivorship-bias check on a delisted symbol's
//  PnL stream.
//
//  Correct handling keeps a delisted symbol in the book (its rows are PRESENT,
//  never silently dropped) and FREEZES its PnL to exactly zero from the period
//  after delisting onward. A non-zero post-delist entry means the symbol kept
//  "trading" past its delisting — the classic survivorship leak.
//
//  Returns true iff pnl[i] == 0.0 for ALL i > delist_period. Indices at or
//  before delist_period are live trading and are not constrained here.
//
//  Contract: `delist_period` is the last LIVE period index; if
//  `delist_period >= pnl.size()` there are no post-delist periods to check and
//  the function returns true (vacuously frozen).
// ===========================================================================
[[nodiscard]] inline bool check_survivorship_frozen(std::span<const atx::f64> pnl,
                                                    atx::usize delist_period) noexcept {
  for (atx::usize i = delist_period + 1U; i < pnl.size(); ++i) {
    if (pnl[i] != 0.0) {
      return false; // post-delist PnL must be frozen to zero
    }
  }
  return true;
}

// ===========================================================================
//  catches_overfit_synthetic — non-vacuous proof that BOTH overfit gates fire.
//
//  Two independent planted-overfit synthetics, each of which a working gate must
//  reject:
//
//   (1) PBO gate (anti-persistent batch). An N×T matrix in which each candidate
//       spikes (+1) on exactly one sub-period window (windows are temporally
//       disjoint; with kN>kS, candidate c and c+kS share a window) and is
//       flat-negative (−0.02) elsewhere — so whichever candidate is in-sample-
//       best is specialized to an IS window and therefore dead out-of-sample, so
//       it lands at/below the OOS median on every split => PBO is HIGH. Assert
//       pbo > 0.5.
//
//   (2) DSR gate (multiple-testing kill). A per-period Sharpe sr=0.12 over T=250
//       looks "significant" under a SINGLE test (PSR(0.12, 0) > 0.9), but as the
//       best of N=1000 trials the selection-deflated DSR < 0.5. Asserting both
//       the single-test pass AND the deflated fail proves the deflation is what
//       does the killing (non-vacuous on the very same point estimate).
//
//  Returns true iff all three sub-conditions hold. RNG-free / deterministic.
//  (Empirically: pbo ≈ 1.0, single_test ≈ 0.97, dsr ≈ 0.086.)
// ===========================================================================
[[nodiscard]] bool catches_overfit_synthetic();

} // namespace atx::engine::validation
