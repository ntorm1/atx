#pragma once

// atx::engine::book -- van der Waerden normal scores of a ranked row (platform v8 Y, lane YCOMB;
// the construction rule norm-score-v1 in atx-impl reads it).
//
// normal_scores: the names of `sorted` (value, index), ascending by value as the target replay's
// tied-rank pass leaves them, receive z = Phi^{-1}(u) with
//
//   u = (b + e + 1) / (2 (n + 1))     for the tie block [b, e) of the n sorted names,
//
// i.e. the mean 1-based rank of the block over n + 1 (van der Waerden 1952; the score map
// Grinold and Kahn, "Active Portfolio Management", 2nd ed., ch. 14, prescribe for turning a rank
// into a standard-normal score, so that a forecast alpha = volatility x IC x score is linear in
// it). Against the centred tied rank, which spreads the names uniformly on [-.5, .5], the normal
// score puts more of the row's gross on the tails: at n = 1,850 the largest |z| is about 3.3
// against a mean |z| of about .8 (the uniform rank: .5 against .25). There is no free constant.
// Tied names keep one score; mirrored blocks get u and 1 - u. u lies in [1 / (n + 1),
// n / (n + 1)], inside (0, 1), so Phi^{-1} is finite. Phi^{-1} is engine::eval::norm_ppf
// (Acklam's approximation with one Halley step: about machine precision).
//
// Names not in `sorted` are not written. Fewer than two names: nothing is written (a row of one
// ranks nothing, as the tied rank). One pass in sorted order; no RNG, clock or map. Header-only
// (as inverse_vol.hpp): the target replay's TU is its only caller.

#include <algorithm> // std::max
#include <cmath>     // std::abs, std::isnan
#include <span>      // std::span
#include <utility>   // std::pair

#include "atx/core/error.hpp"             // Result, Err, Ok
#include "atx/core/types.hpp"             // f64, usize
#include "atx/engine/eval/stats_ext.hpp"  // norm_ppf

namespace atx::engine::book {

// What one normal_scores pass did.
struct NormalScoreStats {
  atx::usize scored = 0U;   // names written
  atx::f64 max_abs = 0.0;   // the largest |z| written (0 when nothing was)
};

// See the header comment. Err InvalidArgument, nothing written: an index outside `out`, a NaN
// value, or values out of ascending order.
[[nodiscard]] inline atx::core::Result<NormalScoreStats>
normal_scores(std::span<const std::pair<atx::f64, atx::usize>> sorted, std::span<atx::f64> out) {
  namespace co = atx::core;
  const atx::usize n = sorted.size();
  for (atx::usize k = 0U; k < n; ++k) {
    if (sorted[k].second >= out.size() || std::isnan(sorted[k].first) ||
        (k > 0U && sorted[k].first < sorted[k - 1U].first))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "normal_scores: an index outside the row, a NaN value or a row out of order");
  }
  NormalScoreStats stats;
  if (n < 2U) return co::Ok(stats);
  const atx::f64 denominator = 2.0 * (static_cast<atx::f64>(n) + 1.0);
  for (atx::usize b = 0U; b < n;) {
    atx::usize e = b + 1U;
    while (e < n && sorted[e].first == sorted[b].first) ++e;
    const atx::f64 u = (static_cast<atx::f64>(b) + static_cast<atx::f64>(e) + 1.0) / denominator;
    const atx::f64 z = atx::engine::eval::norm_ppf(u);
    for (atx::usize k = b; k < e; ++k) out[sorted[k].second] = z;
    stats.scored += e - b;
    stats.max_abs = std::max(stats.max_abs, std::abs(z));
    b = e;
  }
  return co::Ok(stats);
}

} // namespace atx::engine::book
