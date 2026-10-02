#pragma once

// atx::engine::book -- inverse-volatility scaling of a desired-target row (platform v8 X, lane
// XCOMB; the construction rule inv-vol-v1 in atx-impl reads it).
//
// scale_inverse_vol: the members' values (atx-impl: their centred tied ranks, before the demean)
// are multiplied by m_i = median / max(s_i, floor_fraction x median), so a book built from them
// holds dollars in proportion to rank / volatility (the position of a mean-variance holder whose
// alpha is IC x volatility x score on a diagonal risk model, Grinold and Kahn, "Active Portfolio
// Management", 2nd ed., ch. 14), normalised so that the median-volatility name keeps m = 1.
//   usable    s_i finite and > 0 (an eligible name without one takes s_i = the median: m = 1);
//   median    of the eligible names' usable s_i, sorted ascending: the middle one (odd count) or
//             (lower + upper) / 2 (even count);
//   floor     floor_fraction x median, so m_i <= 1 / floor_fraction (a stale or near-constant
//             price cannot take the book);
//   identity  equal usable volatilities give m = median / median = 1 exactly, i.e. the row bit for
//             bit; no eligible name with a usable s_i leaves the row untouched (median NaN).
// Names with eligible[i] == 0 are not read or written. One pass in index order; no RNG, clock or
// map.
//
// Header-only (as combine/group_erc.hpp): one sort and one loop, no engine library object and no
// engine CMake change; the target replay's TU is its only caller.

#include <algorithm> // std::sort, std::max
#include <cmath>     // std::isfinite
#include <limits>    // std::numeric_limits
#include <span>      // std::span
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Result, Err, Ok
#include "atx/core/types.hpp" // f64, u8, usize

namespace atx::engine::book {

// What one scale_inverse_vol pass did over the eligible names.
struct InverseVolStats {
  atx::usize scaled = 0U;         // eligible names multiplied (filled and floored included)
  atx::usize filled = 0U;         // of them: no usable volatility (took the median, m = 1)
  atx::usize floored = 0U;        // of them: volatility raised to the floor (m = 1 / fraction)
  atx::f64 median = 0.0;          // the median usable volatility; NaN when there is none
  atx::f64 max_multiplier = 0.0;  // the largest m applied (0 when nothing was scaled)
};

// See the header comment. Err InvalidArgument, nothing modified: rows of different widths,
// floor_fraction not in (0, 1], or an eligible name with a non-finite value. `scratch` is
// caller-owned storage for the sorted volatilities (reused across decisions).
[[nodiscard]] inline atx::core::Result<InverseVolStats>
scale_inverse_vol(std::span<atx::f64> values, std::span<const atx::f64> sigma,
                  std::span<const atx::u8> eligible, atx::f64 floor_fraction,
                  std::vector<atx::f64> &scratch) {
  namespace co = atx::core;
  const atx::usize n = values.size();
  if (sigma.size() != n || eligible.size() != n)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "scale_inverse_vol: values, sigma and eligible differ in width");
  if (!(floor_fraction > 0.0 && floor_fraction <= 1.0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "scale_inverse_vol: floor_fraction must be in (0, 1]");
  const auto usable = [](atx::f64 s) { return std::isfinite(s) && s > 0.0; };
  scratch.clear();
  for (atx::usize i = 0U; i < n; ++i) {
    if (eligible[i] == 0U) continue;
    if (!std::isfinite(values[i]))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "scale_inverse_vol: an eligible name has a non-finite value");
    if (usable(sigma[i])) scratch.push_back(sigma[i]);
  }
  InverseVolStats stats;
  const atx::usize count = scratch.size();
  if (count == 0U) {
    stats.median = std::numeric_limits<atx::f64>::quiet_NaN();
    return co::Ok(stats);
  }
  std::sort(scratch.begin(), scratch.end());
  const atx::usize mid = count / 2U;
  const atx::f64 median =
      count % 2U == 1U ? scratch[mid] : (scratch[mid - 1U] + scratch[mid]) / 2.0;
  const atx::f64 lowest = floor_fraction * median;
  stats.median = median;
  for (atx::usize i = 0U; i < n; ++i) {
    if (eligible[i] == 0U) continue;
    atx::f64 s = sigma[i];
    if (!usable(s)) {
      s = median;
      ++stats.filled;
    } else if (s < lowest) {
      s = lowest;
      ++stats.floored;
    }
    const atx::f64 m = median / s;
    values[i] *= m;
    ++stats.scaled;
    stats.max_multiplier = std::max(stats.max_multiplier, m);
  }
  return co::Ok(stats);
}

} // namespace atx::engine::book
