#pragma once
// Composition rule theme-erc-v1 (platform v8 expansion X, lane XCOMB): relocated to
// atx-engine-research-composition by P9 lane D2 (atx/engine/research/composition/theme_erc.hpp
// holds the registration text, the constants and theme_erc_weights). This header is the thin
// include that keeps the IC runner's names (atx::impl::strategy) for its verify
// (strategy_ic_rules.cpp) and the gtests.
#include "atx/engine/research/composition/theme_erc.hpp"

namespace atx::impl::strategy {
using atx::engine::research::composition::theme_erc_rule;
using atx::engine::research::composition::theme_erc_sweeps;
using atx::engine::research::composition::theme_erc_dispersion;
using atx::engine::research::composition::theme_erc_share_tolerance;
using atx::engine::research::composition::theme_erc_cap_tolerance;
using atx::engine::research::composition::theme_erc_weight_tolerance;
using atx::engine::research::composition::ThemeErcFit;
using atx::engine::research::composition::theme_erc_weights;
} // namespace atx::impl::strategy
