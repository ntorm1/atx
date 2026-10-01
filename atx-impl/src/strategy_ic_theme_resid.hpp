#pragma once
// Composition rule theme-resid-v1 (platform v8 R-11, Ruling E-38 slot 50; registration in
// task-R-11-report.md and atx-impl/tools/composition_resid.py): the per-session step the IC
// composition (IcThemeRule::residualise) applies to the ew-theme-std-v1 theme planes. The weights
// file's theme_residualise block is read by ic_detail::composition_residualise (declared in
// strategy_ic_detail.hpp, defined in strategy_ic_theme_resid.cpp).
#include <span>
#include <utility>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
// `planes`: one date-major dates x `names` plane per theme, index = position in the registered
// theme order, holding the sum of the theme's present members' w_k s_k rank_k (NaN = no member
// present); `mass`: W_theme per plane. Per date, each row is first replaced by the theme's
// standardised composite z (centred tied rank over its present names; 0 for a lone present name),
// then theme 0 adds W_0 z_0 and theme t > 0 adds W_t times the centred tied re-rank of the
// least-squares residual of z_t on an intercept and z_0..z_{t-1} (absent = 0) over the names where
// t is present (combine/group_residualise.hpp; a residual within its span tolerance adds nothing),
// the residual first replaced by its mean inside each tie block of z_t (exact equality; Ruling
// PM4-12: names t does not distinguish stay tied; without ties the residual is unchanged).
// A theme with fewer than two present names on a date adds nothing, as under ew-theme-std-v1.
// Theme 0 adds exactly what ew-theme-std-v1 adds for it. `out` (dates x names) is accumulated;
// `row` is ranking scratch (its capacity is kept). The planes are consumed. Refuses
// (InvalidArgument) on mismatched shapes; OutOfRange if the scratch cannot be allocated.
[[nodiscard]] atx::core::Status add_theme_residualised(std::span<std::vector<atx::f64>> planes,
                                                       std::span<const atx::f64> mass, atx::usize names,
                                                       std::span<atx::f64> out,
                                                       std::vector<std::pair<atx::f64, atx::usize>>& row);
} // namespace atx::impl::strategy
