#pragma once
// Composition schedule theme-tsmom-v1 (platform v8 Y, lane YCOMB; task-YCOMB-report.md rule Y-2):
// its registration text, constants and mass kernel were relocated to the library
// atx-engine-research-composition by P9 lane D2 (atx/engine/research/composition/theme_tsmom.hpp).
// This header is the thin include that keeps the IC runner's names (atx::impl::strategy); the weights-file parse
// (ic_detail::composition_schedule, declared in strategy_ic_rules.hpp) stays in
// strategy_ic_theme_tsmom.cpp.
#include "atx/engine/research/composition/theme_tsmom.hpp"

namespace atx::impl::strategy {
using atx::engine::research::composition::theme_tsmom_rule;
using atx::engine::research::composition::theme_tsmom_lookback;
using atx::engine::research::composition::theme_tsmom_lag;
using atx::engine::research::composition::theme_tsmom_step;
using atx::engine::research::composition::theme_tsmom_max_blocks;
using atx::engine::research::composition::theme_tsmom_masses;
} // namespace atx::impl::strategy
