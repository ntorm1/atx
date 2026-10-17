#pragma once
// two-speed-v1 (platform v8 Y-5, lane YCOMB; Ruling PM8-5, accepted as distinct from R-3 by
// PM8-10): the registration, the half-life table and the fast / slow split were relocated to
// atx-engine-research-composition by P9 lane D2 (atx/engine/research/composition/two_speed.hpp,
// header-only). This header is the thin include that keeps the names in atx::impl::strategy for
// the IC runner's theme_sleeves parse (strategy_ic_two_speed.cpp), IcComposition and the gtests.
// The half-lives are the rule's registered parameters and the theme set is the theme table's (P9
// lane D1, strategy_ic_rules.hpp).
#include "atx/engine/research/composition/two_speed.hpp"

namespace atx::impl::strategy {
using atx::engine::research::composition::two_speed_rule;
using atx::engine::research::composition::two_speed_half_lives;
using atx::engine::research::composition::two_speed_half_life;
using atx::engine::research::composition::two_speed_fast;
} // namespace atx::impl::strategy
