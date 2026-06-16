#pragma once

// atx::engine::data — Split + dividend total-return adjustment (p3 S1-3).
//
// WHAT THIS IS
//   Folds the split factor (`cum_adj_factor`, from S1-2) and reinvested cash
//   dividends (`cash_dividend`, per-share on the ex-date) into a single
//   total-return index — the canonical price field every downstream alpha
//   consumes (resolved convention §0.7 #1). Operates on ONE symbol's parallel,
//   ascending-by-date arrays (a slice of `CorpActionColumns` + the symbol's raw
//   close). Pure value-in / value-out: no I/O, no allocation beyond the result.
//
// THE MATH (implemented exactly; see adjust.cpp for the per-cell flow)
//   Let S_t = split-adjusted close, D_t = per-share cash dividend on date t,
//   r_t = daily total return, TRI_t = total-return index.
//
//   1. Split-adjusted close   S_t      = raw_close_t * cum_adj_factor_t
//      `cum_adj_factor` is the cumulative FUTURE-split factor on the latest share
//      basis (S1-2 / us_split_adjustment_factors --mode split): the factor steps
//      by the split ratio on each ex-date and the raw close drops by exactly that
//      ratio, so S_t is CONTINUOUS across a split (no fabricated jump). This is
//      the invariant tested at the AAPL 4:1 (2020-08-31) and 7:1 (2014-06-09)
//      ex-dates.
//
//   2. Daily total return     r_t      = (S_t + D_t_adj) / S_{t-1} - 1,  r_0 = 0
//      where D_t_adj = D_t * cum_adj_factor_t expresses the cash dividend on the
//      SAME split basis as S_t. Modelled as additive reinvestment in the
//      numerator — robust to large dividends, with none of the `1 - D/C`
//      negative-factor pathology of a multiplicative back-adjustment.
//
//   3. Total-return index     TRI_0    = S_0  (first valid close — the level basis)
//                             TRI_t    = TRI_{t-1} * (1 + r_t)
//      A geometric chain of (1 + r_t). The absolute level is arbitrary (it is
//      anchored to the first valid split-adjusted close); only the RETURNS r_t
//      are economically meaningful, which is what §0.7 #2 (return-invariance ⇒
//      non-leak) relies on.
//
// THE RETURN-INVARIANCE NON-LEAK CONTRACT (§0.7 #2 — load-bearing)
//   Scaling the ENTIRE raw_close history by any positive constant k leaves
//   `total_return` BYTE-IDENTICAL. Proof: S_t scales to k*S_t and D_t_adj is part
//   of the same basis, so r_t = (k*S_t + k*D_t_adj)/(k*S_{t-1}) - 1 is independent
//   of k. (The dividend inputs are the symbol's own per-share cash amounts, also
//   on the price scale, so a k-rescale of the price series carries them too — see
//   the test, which rescales close AND the price-denominated dividend together.)
//   Back-adjustment therefore rescales price LEVELS but not RETURNS; alphas
//   consume returns/ranks, so full-history back-adjustment does not leak the
//   future. The PIT leak guard lives on the fundamental as-of join (S1-2), NOT
//   here.
//
// NaN POLICY (no silent zero-fill — propagate, never fabricate)
//   * NaN raw_close_t      -> S_t = NaN, r_t = NaN, TRI_t carried forward as NaN
//     for that cell; the series RESUMES at the next valid close (its r is again 0,
//     re-anchoring TRI, because there is no defined return across a gap).
//   * NaN cum_adj_factor_t -> treated IDENTICALLY to a NaN raw_close: S_t = NaN
//     and the cell is a gap. S1-2 emits NaN (NOT 1.0) where the split factor is
//     absent — which only happens where the symbol is genuinely absent on a union
//     date (and the price is absent too). Defaulting a missing factor to 1.0 would
//     silently un-adjust a price across an UNKNOWN split and fabricate a return,
//     so we refuse it: an unknown factor is a gap, exactly like an unknown price.
//   * cash_dividend is 0.0 off the ex-date (S1-2 convention); a NaN dividend is
//     treated as 0.0 (no dividend that day) rather than poisoning an otherwise
//     valid price cell — dividends are additive, absence means none.
//
// OWNERSHIP / LIFECYCLE
//   `adjust_total_return` returns an owned `AdjustedSeries` by value (cold path,
//   once per symbol per backtest window; allocation is intentional). It borrows
//   its inputs as spans for the duration of the call only and stores no
//   reference to them. The three input spans must be the SAME length and aligned
//   index-for-index (index k describes the same trading date in all three); a
//   length mismatch yields an empty result (see the contract below). Inputs are
//   ascending by date, one symbol.

#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::data {

// =========================================================================
//  Result type
// =========================================================================

// One symbol's adjusted price series. The three vectors are PARALLEL and have
// the same length as the inputs: index k describes the same trading date as
// input index k. NaN marks a gap (absent/NaN raw close or split factor) — never
// a fabricated value.
struct AdjustedSeries {
  std::vector<atx::f64> split_adj_close;    // S_t = raw_close * cum_adj_factor (split-only)
  std::vector<atx::f64> total_return_index; // TRI_t; level basis = first valid S
  std::vector<atx::f64> total_return;       // r_t (for backtest / fitness); r at a gap = NaN
};

// =========================================================================
//  Adjustment
// =========================================================================

// Fold split + reinvested dividends into the total-return series for ONE symbol.
//
// Inputs (parallel, ascending by date, one symbol):
//   raw_close       UNADJUSTED close; NaN where absent (a gap).
//   cum_adj_factor  cumulative split-only factor (S1-2); NaN where absent (a gap,
//                   NOT 1.0 — see the NaN policy above).
//   cash_dividend   per-share cash dividend on the ex-date; 0.0 otherwise; a NaN
//                   is treated as 0.0 (no dividend).
//
// Contract:
//   * The three spans MUST be equal length. On a length mismatch the function
//     returns a default (all-empty) AdjustedSeries — the caller built the slice
//     wrong; this is a programming error surfaced as an empty, harmless result
//     rather than UB. (S1-3 callers always pass the symbol's own parallel
//     arrays, so this is a guard, not a normal path.)
//   * total_return[0] == 0.0 for the first VALID cell (no prior price ⇒ no
//     return). A leading run of gaps carries NaN until the first finite S.
//   * Return-invariant: rescaling raw_close (and the price-denominated dividend)
//     by a positive constant leaves total_return byte-identical (§0.7 #2).
//
// noexcept: the only allocation is the result's three vectors; on a bad_alloc the
// process is already lost, so we do not promise noexcept — but no other failure
// mode exists (no I/O, no parsing, total arithmetic).
[[nodiscard]] AdjustedSeries adjust_total_return(std::span<const atx::f64> raw_close,
                                                 std::span<const atx::f64> cum_adj_factor,
                                                 std::span<const atx::f64> cash_dividend);

} // namespace atx::engine::data
