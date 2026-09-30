#pragma once

// atx::engine::combine -- group rank composite and its centred tied re-rank.
//
// A group (for example a theme of alpha members) forms, per date and name, the sum of
// its present members' weighted signed cross-sectional ranks; a member missing for a
// name adds nothing (neutral, no redistribution inside the group). Dividing that sum
// by the group's fixed total weight gives the weighted mean of the member ranks. The
// divisor is one positive constant per group, so it cannot change any per-date order:
// the re-rank below ranks the sum itself and no division can round two values into a
// tie. add_group_rerank ranks each date's composite over the names with at least one
// present member and adds weight x centred tied rank to a blend, so every group enters
// the blend with the same cross-sectional dispersion whatever its member count or the
// correlation of its members.
//
// Layout: date-major dates x names planes of f64. A NaN plane cell means "no member
// present"; accumulate_group_cell turns the first contribution into a value.
// Ranks: sort (value, name index) ascending; a tie block [b, e) of n ranked values gets
// (b + e - 1) / (2 (n - 1)) - 0.5, in [-0.5, 0.5] with mean 0; fewer than two values
// rank nothing. This is the IC composition's member rank expression, bit for bit.
//
// Header-only on purpose: these are per-cell loops that atx-impl instantiates inside its
// optimised IC composition translation unit, while the engine library itself builds
// unoptimised in Debug (atx-impl/CMakeLists.txt gives the IC TUs /O2).

#include <algorithm> // std::sort
#include <cmath>     // std::isnan, std::isfinite
#include <span>      // std::span
#include <utility>   // std::pair
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Status
#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::combine {

// (value, name index): the sort key of one ranked name.
using RankedName = std::pair<atx::f64, atx::usize>;

// Sorts `row` by (value, name index) and calls apply(name index, centred tied rank) for
// every entry, in sorted order. Precondition: no value is NaN. Fewer than two entries:
// no call. Allocation free.
template <class Apply>
void for_each_centered_rank(std::vector<RankedName>& row, Apply&& apply) {
  std::sort(row.begin(), row.end());
  if (row.size() < 2U) return;
  for (atx::usize b = 0; b < row.size();) {
    atx::usize e = b + 1U;
    while (e < row.size() && row[e].first == row[b].first) ++e;
    const auto r = (static_cast<atx::f64>(b) + static_cast<atx::f64>(e - 1U)) /
                   (2.0 * static_cast<atx::f64>(row.size() - 1U)) - 0.5;
    for (atx::usize k = b; k < e; ++k) apply(row[k].second, r);
    b = e;
  }
}

// Adds one member's contribution to a group plane cell; the first contribution replaces
// the NaN "no member present" sentinel. Precondition: `contribution` is not NaN.
inline void accumulate_group_cell(atx::f64& cell, atx::f64 contribution) noexcept {
  cell = std::isnan(cell) ? contribution : cell + contribution;
}

// For each date d in [begin, end): the centred tied ranks of the non-NaN cells of plane
// row d over those names, times `weight`, added to `out` at the same cells. Every other
// cell of `out` is untouched, and a row with fewer than two present names adds nothing.
// `scratch` is reused per row; reserve `names` entries to keep the loop allocation free.
// Dates are independent, so disjoint [begin, end) bands may run concurrently on one
// `out` with one scratch each. Refuses (InvalidArgument): names == 0, plane and out of
// different sizes or not whole rows, end beyond the last row, begin > end, or a
// non-finite weight. `out` must not alias `plane`.
[[nodiscard]] inline atx::core::Status add_group_rerank(std::span<const atx::f64> plane, atx::usize names,
                                                        atx::usize begin, atx::usize end, atx::f64 weight,
                                                        std::span<atx::f64> out,
                                                        std::vector<RankedName>& scratch) {
  if (names == 0U || plane.size() != out.size() || plane.size() % names != 0U || begin > end ||
      end > plane.size() / names || !std::isfinite(weight))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "group rerank: shape, date range or weight");
  for (atx::usize d = begin; d < end; ++d) {
    const auto offset = d * names;
    scratch.clear();
    for (atx::usize i = 0; i < names; ++i)
      if (!std::isnan(plane[offset + i])) scratch.emplace_back(plane[offset + i], i);
    for_each_centered_rank(scratch, [&](atx::usize i, atx::f64 r) { out[offset + i] += weight * r; });
  }
  return atx::core::Ok();
}

} // namespace atx::engine::combine
