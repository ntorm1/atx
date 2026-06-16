// atx::engine::data — Split + dividend total-return adjustment (p3 S1-3).
//
// See adjust.hpp for the full contract, the math, the NaN policy, and the
// return-invariance non-leak argument. This file is the per-cell realization:
//   S_t  = raw_close_t * cum_adj_factor_t                       (split-adjusted)
//   r_t  = (S_t + D_t * cum_adj_factor_t) / S_{t-1} - 1,  r_0=0  (daily total ret)
//   TRI  = chain of (1 + r_t), anchored to the first valid S_t
// All arithmetic is total (no division by zero is reachable on a valid cell: a
// valid prior S_{t-1} is finite and non-zero because a traded price is > 0; a
// zero/negative prior price is treated as a gap). Cold path: one pass per symbol.

#include "atx/engine/data/adjust.hpp"

#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::data {

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// A cell's split-adjusted close is VALID (a real, chainable price) iff both the
// raw close and the split factor are finite and the resulting price is strictly
// positive. A non-positive or non-finite S cannot anchor a return (it would make
// r_t = (S + D)/S_prev - 1 meaningless or divide-by-zero downstream), so it is a
// gap. This is the single predicate the whole pass keys off.
[[nodiscard]] bool is_valid_close(atx::f64 s) noexcept { return std::isfinite(s) && s > 0.0; }

// Split-adjusted close for one cell; NaN if either input is non-finite (a gap).
// A NaN cum_adj_factor is propagated here exactly like a NaN raw_close — an
// unknown split factor is a gap, never silently 1.0 (see adjust.hpp NaN policy).
[[nodiscard]] atx::f64 split_adjusted(atx::f64 raw_close, atx::f64 factor) noexcept {
  if (!std::isfinite(raw_close) || !std::isfinite(factor)) {
    return kNaN;
  }
  return raw_close * factor;
}

// The dividend on the SAME split basis as S_t: D_t * cum_adj_factor_t. A NaN
// dividend means "no dividend that day" -> 0.0 (additive; absence is none), so it
// never poisons an otherwise valid price cell. Requires a finite factor (the
// caller only invokes this on a valid S_t, where the factor is finite).
[[nodiscard]] atx::f64 dividend_on_basis(atx::f64 dividend, atx::f64 factor) noexcept {
  const atx::f64 d = std::isfinite(dividend) ? dividend : 0.0;
  return d * factor;
}

} // namespace

AdjustedSeries adjust_total_return(std::span<const atx::f64> raw_close,
                                   std::span<const atx::f64> cum_adj_factor,
                                   std::span<const atx::f64> cash_dividend) {
  AdjustedSeries out{};
  const atx::usize n = raw_close.size();
  // Guard: the three parallel arrays must align index-for-index. A mismatch is a
  // caller bug; surface it as an empty (harmless) result rather than reading OOB.
  if (cum_adj_factor.size() != n || cash_dividend.size() != n) {
    return out;
  }

  out.split_adj_close.assign(n, kNaN);
  out.total_return_index.assign(n, kNaN);
  out.total_return.assign(n, kNaN);

  // Running state for the geometric chain. `prev_s` is the split-adjusted close
  // of the immediately-preceding cell (NaN after a gap); `prev_tri` is the last
  // finite TRI level. When `prev_s` is NaN (the first valid cell, or the first
  // valid cell after a gap) we ANCHOR: r=0, TRI=S — there is no defined return to
  // chain across a gap. Otherwise we extend the chain by (1 + r_t).
  atx::f64 prev_s = kNaN;
  atx::f64 prev_tri = kNaN;

  for (atx::usize t = 0; t < n; ++t) {
    const atx::f64 s = split_adjusted(raw_close[t], cum_adj_factor[t]);
    out.split_adj_close[t] = s;

    if (!is_valid_close(s)) {
      // Gap: NaN r and TRI for this cell; the next valid cell re-anchors.
      prev_s = kNaN;
      continue;
    }

    if (!is_valid_close(prev_s)) {
      // First valid close, or resumption after a gap: anchor the index here.
      out.total_return[t] = 0.0;
      out.total_return_index[t] = s;
      prev_tri = s;
    } else {
      const atx::f64 d_adj = dividend_on_basis(cash_dividend[t], cum_adj_factor[t]);
      const atx::f64 r = (s + d_adj) / prev_s - 1.0;
      out.total_return[t] = r;
      prev_tri *= (1.0 + r);
      out.total_return_index[t] = prev_tri;
    }
    prev_s = s;
  }

  return out;
}

} // namespace atx::engine::data
