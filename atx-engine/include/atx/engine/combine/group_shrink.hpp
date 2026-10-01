#pragma once

// atx::engine::combine -- shrunk within-group shares and a cross-group member cap.
//
// group_shrink_shares: every member carries an estimate (for example an alpha's IC) and a
// group (for example its theme). Per group g with members M_g (n_g of them) and mean
// estimate m_g, each estimate is shrunk toward the group mean with a fixed intensity,
//   shrunk_k = intensity * m_g + (1 - intensity) * estimate_k,
// floored (p_k = shrunk_k when above `minimum`, else `minimum`) and normalised in its group,
//   share_k = p_k / sum_{M_g} p,
// so each group's shares sum to 1. A group whose floored values sum to 0 (only possible with
// minimum 0: no positive shrunk estimate) takes the shrinkage target instead, the equal share
// 1 / n_g, and is flagged. With no member floored and m_g > 0 the share is the
// estimate-proportional share shrunk toward the equal share:
//   share_k = intensity / n_g + (1 - intensity) * estimate_k / sum_{M_g} estimate.
//
// cap_across_groups: no weight above `cap`. Each pass sets every unfrozen member above
// cap * (1 + tolerance) to the cap and freezes it; each group's total excess is then added
// to the unfrozen members of the OTHER groups pro rata to their weights at the start of that
// redistribution (groups in index order). Passes repeat until no unfrozen member is above the
// bound; every capping pass freezes a member, so at most n + 1 passes run. An excess with no
// receiver of positive weight is refused: the cap is infeasible.
//
// Arithmetic: sums run in member order, one expression per step as written above, so a port
// that keeps the member order reproduces the values (atx-impl/tools/composition_ic_shrink.py
// and composition_rules.member_cap are such ports; their theme order may differ in the cap
// when two groups spill in one pass, which moves only the last bits).
//
// Header-only as group_rerank.hpp: small per-member loops, no engine library object.

#include <algorithm> // std::all_of, std::any_of, std::copy, std::fill
#include <cmath>     // std::isfinite
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Result, Err, Ok
#include "atx/core/types.hpp" // f64, u8, usize

namespace atx::engine::combine {

// group_shrink_shares output, per member in input order (`equal`: per group).
struct GroupShrinkShares {
  std::vector<atx::f64> shrunk; // intensity * group mean + (1 - intensity) * estimate
  std::vector<atx::f64> share;  // floored shrunk / group sum, or 1 / n_g for an equal group
  std::vector<atx::u8> equal;   // 1: the group had no positive floored value (equal shares)
};

// Shares of every member inside its group (see the header comment). Refuses
// (InvalidArgument): no member, `group` not one index per estimate, groups == 0, an index
// >= groups, a group index without a member, an intensity outside [0, 1], a minimum that is
// negative or not finite, a non-finite estimate, or sums that overflow.
[[nodiscard]] inline atx::core::Result<GroupShrinkShares>
group_shrink_shares(std::span<const atx::f64> estimate, std::span<const atx::usize> group,
                    atx::usize groups, atx::f64 intensity, atx::f64 minimum) {
  namespace co = atx::core;
  if (estimate.empty() || group.size() != estimate.size() || groups == 0U ||
      std::any_of(group.begin(), group.end(), [&](atx::usize g) { return g >= groups; }))
    return co::Err(co::ErrorCode::InvalidArgument, "group shrink: shapes or a group index out of range");
  std::vector<atx::usize> count(groups, 0U);
  for (const atx::usize g : group) ++count[g];
  if (!std::all_of(count.begin(), count.end(), [](atx::usize n) { return n > 0U; }))
    return co::Err(co::ErrorCode::InvalidArgument, "group shrink: a group index without a member");
  if (!(intensity >= 0.0 && intensity <= 1.0) || !(minimum >= 0.0) || !std::isfinite(minimum) ||
      std::any_of(estimate.begin(), estimate.end(), [](atx::f64 x) { return !std::isfinite(x); }))
    return co::Err(co::ErrorCode::InvalidArgument, "group shrink: intensity outside [0, 1], a negative or "
                                                   "non-finite minimum, or a non-finite estimate");
  std::vector<atx::f64> total(groups, 0.0), mass(groups, 0.0), kept(estimate.size());
  for (atx::usize k = 0; k < estimate.size(); ++k) total[group[k]] += estimate[k];
  GroupShrinkShares out;
  out.shrunk.resize(estimate.size());
  out.share.resize(estimate.size());
  out.equal.assign(groups, 0U);
  for (atx::usize k = 0; k < estimate.size(); ++k) {
    const atx::usize g = group[k];
    const atx::f64 mean = total[g] / static_cast<atx::f64>(count[g]);
    out.shrunk[k] = intensity * mean + (1.0 - intensity) * estimate[k];
    kept[k] = out.shrunk[k] > minimum ? out.shrunk[k] : minimum;
    mass[g] += kept[k];
  }
  for (atx::usize g = 0; g < groups; ++g) out.equal[g] = mass[g] > 0.0 ? 0U : 1U;
  for (atx::usize k = 0; k < estimate.size(); ++k) {
    const atx::usize g = group[k];
    out.share[k] = out.equal[g] != 0U ? 1.0 / static_cast<atx::f64>(count[g]) : kept[k] / mass[g];
  }
  if (!std::all_of(out.share.begin(), out.share.end(), [](atx::f64 s) { return std::isfinite(s); }))
    return co::Err(co::ErrorCode::InvalidArgument, "group shrink: the estimates overflow");
  return co::Ok(std::move(out));
}

// Caps `weights` in place (see the header comment) and returns the number of capping passes
// (0: no member was above the bound). Refuses (InvalidArgument): `group` not one index per
// weight, groups == 0, an index >= groups, a negative or non-finite weight, a cap that is not
// positive and finite, a negative or non-finite tolerance, or an infeasible cap. On a refusal
// `weights` holds a partial result (basic guarantee): callers pass a working copy.
[[nodiscard]] inline atx::core::Result<atx::usize> cap_across_groups(std::span<atx::f64> weights,
                                                                     std::span<const atx::usize> group,
                                                                     atx::usize groups, atx::f64 cap,
                                                                     atx::f64 tolerance) {
  namespace co = atx::core;
  const atx::usize n = weights.size();
  const auto bad_weight = [](atx::f64 w) { return !(w >= 0.0) || !std::isfinite(w); };
  if (group.size() != n || groups == 0U ||
      std::any_of(group.begin(), group.end(), [&](atx::usize g) { return g >= groups; }) ||
      std::any_of(weights.begin(), weights.end(), bad_weight) || !(cap > 0.0) || !std::isfinite(cap) ||
      !(tolerance >= 0.0) || !std::isfinite(tolerance))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "group cap: shapes, a group index, a weight, the cap or the tolerance");
  const atx::f64 bound = cap * (1.0 + tolerance);
  std::vector<atx::u8> frozen(n, 0U);
  std::vector<atx::f64> excess(groups, 0.0), base(n, 0.0);
  for (atx::usize pass = 0; pass <= n; ++pass) {
    std::fill(excess.begin(), excess.end(), 0.0);
    bool over = false;
    for (atx::usize k = 0; k < n; ++k) {
      if (frozen[k] != 0U || !(weights[k] > bound)) continue;
      excess[group[k]] += weights[k] - cap;
      weights[k] = cap;
      frozen[k] = 1U;
      over = true;
    }
    if (!over) return co::Ok(pass);
    std::copy(weights.begin(), weights.end(), base.begin());
    for (atx::usize g = 0; g < groups; ++g) {
      if (!(excess[g] > 0.0)) continue;
      atx::f64 receivers = 0.0;
      for (atx::usize j = 0; j < n; ++j)
        if (frozen[j] == 0U && group[j] != g) receivers += base[j];
      if (!(receivers > 0.0))
        return co::Err(co::ErrorCode::InvalidArgument,
                       "group cap: no member of another group can take a group's excess (infeasible cap)");
      for (atx::usize j = 0; j < n; ++j)
        if (frozen[j] == 0U && group[j] != g) weights[j] += excess[g] * base[j] / receivers;
    }
  }
  // Unreachable: every capping pass freezes at least one of the n members.
  return co::Err(co::ErrorCode::Internal, "group cap: no fixed point");
}

} // namespace atx::engine::combine
