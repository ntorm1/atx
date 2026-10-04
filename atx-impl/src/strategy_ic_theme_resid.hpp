#pragma once
// Composition rule theme-resid-v1 (platform v8 R-11, Ruling E-38 slot 50): its per-session kernel
// add_theme_residualised was relocated to atx-engine-research-composition by P9 lane D2
// (atx/engine/research/composition/theme_resid.hpp, with the registration text). This header is
// the thin include that keeps the IC runner's name (atx::impl::strategy) for IcComposition
// (IcThemeRule::residualise) and the gtests; the weights file's theme_residualise parse
// (ic_detail::composition_residualise, declared in strategy_ic_rules.hpp) stays in
// strategy_ic_theme_resid.cpp. The registered theme order is the theme table's (P9 lane D1,
// strategy_ic_rules.hpp).
#include "atx/engine/research/composition/theme_resid.hpp"

namespace atx::impl::strategy {
using atx::engine::research::composition::add_theme_residualised;
} // namespace atx::impl::strategy
