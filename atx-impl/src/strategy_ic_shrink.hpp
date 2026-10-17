#pragma once
// Composition rule ic-shrink-v1 / ic-shrink-aim-v1 (platform v8 R-10, Ruling E-44): relocated to
// atx-engine-research-composition by P9 lane D2 (atx/engine/research/composition/ic_shrink.hpp
// holds the registration text, the constants and ic_shrink_weights). This header is the thin
// include that keeps the IC runner's names (atx::impl::strategy) for its verify
// (strategy_ic_rules.cpp) and the gtests.
#include "atx/engine/research/composition/ic_shrink.hpp"

namespace atx::impl::strategy {
using atx::engine::research::composition::ic_shrink_rule;
using atx::engine::research::composition::ic_shrink_aim_rule;
using atx::engine::research::composition::ic_shrink_intensity;
using atx::engine::research::composition::ic_shrink_floor;
using atx::engine::research::composition::ic_shrink_cap_tolerance;
using atx::engine::research::composition::ic_shrink_weight_tolerance;
using atx::engine::research::composition::IcShrinkFit;
using atx::engine::research::composition::ic_shrink_weights;
} // namespace atx::impl::strategy
