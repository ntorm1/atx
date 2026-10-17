#pragma once
// atx::engine::research::composition -- the composition weights document's schema ids (P9 ruling
// P19). v1 and v2 are today's files and stay untouched: v1 a plain weighted blend, v2 a file that
// carries one theme grouping block (theme_redistribution or theme_standardise; the IC runner
// accepts v2 iff one is present, atx-impl strategy_ic_admission.cpp). v3 is contract K-P9-8's,
// the time-indexed weights of lane D3W: v2's blocks plus a walk_forward block whose rows, keyed by
// decision session, are each fitted on decisions <= d - 2 (fit.hpp fit_walk_forward; the IC
// runner's parse is strategy_ic_rules.hpp composition_walk_forward, its tests WeightsV3.*). A
// binary that predates D3W refuses v3 with every other unknown schema ("schema/library identity").
#include <string_view>

namespace atx::engine::research::composition {
inline constexpr std::string_view weights_schema_v1="atx.dsl-composition-weights/v1";
inline constexpr std::string_view weights_schema_v2="atx.dsl-composition-weights/v2";
inline constexpr std::string_view weights_schema_v3="atx.dsl-composition-weights/v3"; // K-P9-8, D3
} // namespace atx::engine::research::composition
