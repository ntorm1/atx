#pragma once

// surface_db_seed — the ONE per-symbol manifest-seed recipe shared by the
// universe populate driver (`populate_universe_streaming`,
// surface_db_populate.cpp) and the auto-config generator
// (`generate_symbol_configs`, surface_db_build.cpp).
//
// Both stages must seed a symbol's `SymbolFitConfig` identically: the preset's
// captured config, upgraded to the DENSE INDEX RECIPE (a default `CurveConfig` —
// ConvexDense, node_cap 40) when the symbol is the designated index leg.
// Extracted here so the two call sites cannot drift bit-for-bit (the SPY/index
// precedent, spy_ytd_corpus.cpp). Whether that recipe is a hard PIN is the
// caller's explicit choice (`pin_curve_family` below) — the two stages differ
// there, and only there.
//
// Private, src/-only header: NOT installed, NOT part of the public atx/vol/ API
// surface. Both translation units that need it live inside the atx-vol library
// target, so ordinary internal linkage across TUs suffices.

#include <string_view>

#include "atx/vol/session.hpp"    // FitPreset
#include "atx/vol/surface_db.hpp" // SymbolFitConfig

namespace atx::vol {

// Build the seed `SymbolFitConfig` for `symbol`: `symbol_config_from_preset(preset)`,
// upgraded to the dense index recipe (a default `CurveConfig{}` — the ConvexDense
// node_cap-40 index fit) when `index_symbol` is non-empty and equals `symbol` (a
// plain, un-canonicalized string compare, exactly as `populate_universe_streaming`
// did inline). Every other symbol is left on the preset's auto-selector
// (`pin_curve = false`).
//
// `pin_curve_family` decides whether the index recipe is stored as a hard PIN
// (`pin_curve = true`, which makes `PricerFitter` skip both of its fallback
// ladders for that symbol) or only as the preferred family. Because the index
// recipe pinned here is always ConvexDense, that hard pin still leaves the
// symbol eligible for `PricerFitter`'s strict same-family admission-recovery
// refit (never a family substitution); a hard pin naming any OTHER family
// (not produced by this function, but by `PricerFitter`'s general pin
// contract) gets no recovery at all. It has NO effect on a
// non-index symbol, which is never pinned here. Explicit rather than defaulted:
// the two callers deliberately disagree — `populate_universe_streaming` keeps the
// historical pin (it has no operator knob and its seeding is a no-op inside
// `build_surface_db`, which configures every symbol first), while
// `generate_symbol_configs` forwards `AutoConfigSpec::pin_curve_family`, which
// defaults to false so the ladders stay alive in production.
//
// Pure: a function of its arguments only; no shared state. Thread-safe.
[[nodiscard]] SymbolFitConfig seed_symbol_config(std::string_view symbol, FitPreset preset,
                                                 std::string_view index_symbol,
                                                 bool pin_curve_family);

} // namespace atx::vol
