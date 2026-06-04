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

#include "atx/core/domain/symbol.hpp" // atx::core::domain::Symbol

namespace atx::engine {

/// Opaque 32-bit interned instrument id. Equality and ordering are integer
/// comparisons; use atx::core::domain::SymbolTable to intern names.
using InstrumentId = atx::core::domain::Symbol;

} // namespace atx::engine
