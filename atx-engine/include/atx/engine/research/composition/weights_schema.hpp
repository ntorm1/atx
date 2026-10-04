#pragma once
// atx::engine::research::composition -- the composition weights document's schema ids (P9 ruling
// P19). v1 and v2 are today's files and stay untouched: v1 a plain weighted blend, v2 a file that
// carries one theme grouping block (theme_redistribution or theme_standardise; the IC runner
// accepts v2 iff one is present, atx-impl strategy_ic_admission.cpp). v3 is reserved for contract
// K-P9-8, the time-indexed weights of lane D3 (rows keyed by decision date, each fitted on
// decisions <= d - 2); until D3 lands the IC runner refuses it with every other unknown schema
// (StrategyIcRunner tests: "schema/library identity").
#include <string_view>

namespace atx::engine::research::composition {
inline constexpr std::string_view weights_schema_v1="atx.dsl-composition-weights/v1";
inline constexpr std::string_view weights_schema_v2="atx.dsl-composition-weights/v2";
inline constexpr std::string_view weights_schema_v3="atx.dsl-composition-weights/v3"; // K-P9-8, D3
} // namespace atx::engine::research::composition
