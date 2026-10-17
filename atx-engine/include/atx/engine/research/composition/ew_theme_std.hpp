#pragma once
// atx::engine::research::composition -- the fit side of composition rule ew-theme-std-v1 (platform
// v8 R-1: S-1 + S-4 + tier re-grade) and its aim variant ew-theme-std-aim-v1 (R-3 on an R-1 parent,
// Ruling E-27), in C++ (P9 lane D2; composition review table rows 5 and 5b: before D2 these member
// weights and the member cap were computed only by atx-impl/tools/composition_rules.py, with no C++
// check at all). Registration (task-R-1-brief.md, binding; declared before any read):
//   1. Within-theme weights proportional to the tier score in the registry.
//   3. Theme share 1 / T, T = themes with a member: w_k = score_k / (T * sum_{theme(k)} score).
//   4. Member cap: no member's weight above 1 / (2T); the excess goes pro rata to the other themes
//      (combine/group_shrink.hpp cap_across_groups, to a fixed point, relative tolerance 1e-12).
//   5. Tier re-grades from the v8 literature (tier_regrades_v8): res_mom_12_1 B+ to B-; ear B+ to
//      C+; sue C+ stays; ins_opp B- to C+. No other tier changes. No tier comes from a statistic.
// Rules 2 and 3's per-date re-rank are the IC runner's (IcThemeRule::standardise). The aim variant
// multiplies each score by the member's aim gain before rule 1 (each theme still 1 / T before the
// cap). Summation order (composition review row 5, pinned here): every sum runs in member order and
// the cap visits the themes in index order; the fit verb numbers the themes in ascending name
// order, which is composition_rules.member_cap's `sorted(excess)` order, so it reproduces the
// Python bit for bit on the same inputs.
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/group_shrink.hpp"

namespace atx::engine::research::composition {
inline constexpr std::string_view ew_theme_std_rule="ew-theme-std-v1";
// the aim variant; its weights file's block rule stays ew-theme-std-v1
inline constexpr std::string_view ew_theme_std_aim_rule="ew-theme-std-aim-v1";
// R-3 on an ew-theme-v1 parent (Rulings E-27a, E-27b; composition_rules.ew_theme_aim_v2), fitted in
// C++ by P9 lane D3W-PB: theme_gain_weights on scores = the members' aim gains (every parent score
// 1): w_k = (1/T) g_k / sum_{theme(k)} g, then the member cap 1/(2T). Its weights file is a plain
// weighted blend (schema v1, no theme block).
inline constexpr std::string_view ew_theme_aim_v2_rule="ew-theme-aim-v2";
inline constexpr atx::f64 member_cap_tolerance=1e-12; // relative (composition_rules.CAP_TOLERANCE)
// Rule 5, verbatim: (id, from, to); "sue C+ stays" is declared as a no-op row.
struct TierRegrade {
  std::string_view id,from,to;
};
inline constexpr std::array<TierRegrade,4> tier_regrades_v8{{
    {"res_mom_12_1","B+","B-"},{"ear","B+","C+"},{"sue","C+","C+"},{"ins_opp","B-","C+"}}};
// The declared tier scores (task A-1 registry schema; composition_rules.TIER_SCORES_DECLARED), the
// table used when no registry tier_scores table is given.
struct TierScore {
  std::string_view tier;
  atx::f64 score;
};
inline constexpr std::array<TierScore,6> tier_scores_declared{{
    {"A",1.0},{"A-",0.9},{"B+",0.8},{"B",0.7},{"B-",0.55},{"C+",0.4}}};
// One applied row of rule 5 (composition_rules.apply_regrades): status not-a-member (member false,
// source_tier empty), declared-unchanged, applied or already-at-target.
struct RegradeApplied {
  std::string id,from,to,source_tier,status;
  bool member{};
};
// Rule 5 on `tiers` (one source tier per id, in place), table row by table row; a member is found
// by its first occurrence in `ids`. Err (InvalidArgument) when a member's source tier is neither
// the row's from- nor to-grade, or `ids` and `tiers` differ in length.
[[nodiscard]] atx::core::Result<std::vector<RegradeApplied>> apply_tier_regrades(
    std::span<const std::string> ids,std::span<std::string> tiers);
// Rules 1, 3 and 4 on member scores (composition_rules.theme_gain_weights; the aim rules pass score
// x gain): per member in input order the weight before and after the cap, the cap 1 / (2T) and the
// cap's passes. `group` numbers each member's theme (< groups, every index used); 1..256 members,
// 1..32 groups; scores finite and > 0. Err (InvalidArgument) otherwise or on an infeasible cap.
struct ThemeGainFit {
  std::vector<atx::f64> weights,uncapped;
  atx::f64 cap{};
  std::vector<atx::engine::combine::GroupCapPass> passes;
};
[[nodiscard]] atx::core::Result<ThemeGainFit> theme_gain_weights(std::span<const atx::f64> scores,
                                                                 std::span<const atx::usize> group,
                                                                 atx::usize groups);
} // namespace atx::engine::research::composition
