#pragma once

// surface_db_seed — the ONE per-symbol manifest-seed recipe shared by the
// universe populate driver (`populate_universe_streaming`,
// surface_db_populate.cpp) and the auto-config generator
// (`generate_symbol_configs`, surface_db_build.cpp).
//
// Both stages must seed a symbol's `SymbolFitConfig` identically: the preset's
// captured config, upgraded to the DENSE INDEX RECIPE (a pinned default
// `CurveConfig` — ConvexDense, node_cap 40) when the symbol is the designated
// index leg. Extracted here so the two call sites cannot drift bit-for-bit
// (the SPY/index precedent, spy_ytd_corpus.cpp).
//
// Private, src/-only header: NOT installed, NOT part of the public atx/vol/ API
// surface. Both translation units that need it live inside the atx-vol library
// target, so ordinary internal linkage across TUs suffices.

#include <string_view>

#include "atx/vol/session.hpp"    // FitPreset
#include "atx/vol/surface_db.hpp" // SymbolFitConfig

namespace atx::vol {

// Build the seed `SymbolFitConfig` for `symbol`: `symbol_config_from_preset(preset)`,
// upgraded to the dense index recipe (`pin_curve = true` with a default
// `CurveConfig{}` — the ConvexDense node_cap-40 index fit) when `index_symbol`
// is non-empty and equals `symbol` (a plain, un-canonicalized string compare,
// exactly as `populate_universe_streaming` did inline). Every other symbol is
// left on the preset's auto-selector (`pin_curve = false`).
//
// Pure: a function of its arguments only; no shared state. Thread-safe.
[[nodiscard]] SymbolFitConfig seed_symbol_config(std::string_view symbol, FitPreset preset,
                                                 std::string_view index_symbol);

} // namespace atx::vol
