#pragma once
// Composition rule ic-shrink-v1 (platform v8 R-10; cell slot 49 of Ruling E-38). Registration
// (lane COMB2, task-R-10-report.md; every constant fixed blind before any cell read):
//   1. ic_k, the fitter's IC estimate of member k: its admission row's train_mean, the mean of
//      s_k * f_k over the live decisions of the fit window the parent fitter already uses
//      (fit_composition_weights.py; no new window, no new read).
//   2. Per theme, James-Stein shrinkage toward the theme's equal weight with the fixed intensity
//      0.5: shrunk_k = 0.5 * mean_{theme(k)} ic + 0.5 * ic_k.
//   3. Negative shrunk values floored at 0; the within-theme share is floored / theme sum, or the
//      equal share 1 / n_theme when the theme has no positive shrunk value.
//   4. w_k = share_k / T (theme share 1 / T, T = themes with a member), then the member cap
//      1 / (2T) of ew-theme-std-v1 (excess pro rata to the other themes' uncapped members).
//   5. Standardisation unchanged: the per-date rule is ew-theme-std-v1's (IcThemeRule::standardise).
// The arithmetic is atx/engine/combine/group_shrink.hpp's; the fitter's port is
// atx-impl/tools/composition_ic_shrink.py. The IC runner verifies a weights file of this rule
// against the inputs its theme_standardise block records (strategy_ic_admission.cpp, the
// theme_standardise rule table).
#include <span>
#include <string_view>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
inline constexpr std::string_view ic_shrink_rule="ic-shrink-v1";
inline constexpr atx::f64 ic_shrink_intensity=0.5;          // weight of the theme mean (James-Stein target)
inline constexpr atx::f64 ic_shrink_floor=0.0;              // shrunk ICs below it are floored to it
inline constexpr atx::f64 ic_shrink_cap_tolerance=1e-12;    // relative (composition_rules.CAP_TOLERANCE)
inline constexpr atx::f64 ic_shrink_weight_tolerance=1e-12; // runner check: |pinned - rule| per weight
struct IcShrinkFit {
  std::vector<atx::f64> shrunk,share,weights; // per member, input order
  std::vector<atx::u8> equal_theme;           // per theme: 1 = no positive shrunk IC, equal shares
  atx::f64 cap{};                             // 1 / (2T)
  atx::usize cap_passes{};                    // capping passes run (0: no member above the cap)
};
// Members in input order: ic[k] finite, theme[k] < themes and every theme index with at least
// one member; 1..256 members, 1..32 themes. Err (InvalidArgument) on other inputs or on an
// infeasible cap (a theme's excess with no member of another theme to take it).
[[nodiscard]] atx::core::Result<IcShrinkFit> ic_shrink_weights(std::span<const atx::f64> ic,
                                                               std::span<const atx::usize> theme,
                                                               atx::usize themes);
} // namespace atx::impl::strategy
