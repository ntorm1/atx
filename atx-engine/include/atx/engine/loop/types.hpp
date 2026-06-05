#pragma once

// atx::engine::loop — shared type aliases for the Phase-2 backtest loop.
//
// Centralises the InstrumentId alias so every Phase-2 unit uses the same
// name without each header independently pulling in atx-core's domain layer.
//
// Design choice: InstrumentId aliases atx::core::domain::Symbol — a 32-bit
// opaque interned id — rather than introducing a separate engine-level type.
// This avoids an id-space split (one set of ids for atx-core, another for the
// loop) and keeps comparisons and hashing as single-word integer ops. The
// SymbolTable that owns the id<->name mapping lives in atx-core; the engine
// holds references to symbols, not strings.

#include <span> // std::span (Universe view)

#include "atx/core/domain/symbol.hpp" // atx::core::domain::Symbol

namespace atx::engine {

/// Opaque 32-bit interned instrument id. Equality and ordering are integer
/// comparisons; use atx::core::domain::SymbolTable to intern names.
using InstrumentId = atx::core::domain::Symbol;

/// The fixed, ordered live instrument set a Phase-2 decision operates over. A
/// NON-OWNING view: the caller's InstrumentId storage must outlive any Universe
/// handed to a unit. universe[i] is the instrument at cross-section index i — the
/// SAME index order SignalView/PanelView/WeightPolicy weights are aligned to, so
/// values[i], weight[i] and universe[i] all refer to one instrument. Shared
/// alias (rather than a bare std::span at each call site) so the index-alignment
/// contract has one named type across the loop, signal source and weight policy.
using Universe = std::span<const InstrumentId>;

} // namespace atx::engine
