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
#include "atx/core/types.hpp"         // atx::u8 (Delay underlying type)

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

// ===========================================================================
//  Delay — the execution-timing knob (delay-0 vs delay-1).
//
//  This is an EXECUTION-TIMING setting, NOT part of the alpha expression: the
//  SAME compiled program runs under either value and reads the SAME panel data;
//  only the bar an order FILLS on differs. It mirrors the standard backtest
//  convention (and Zipline/LEAN's "trade at this close" vs "next open").
//
//    * Next  (delay-1, DEFAULT): the conservative, structurally-safe firewall.
//      An order decided on bar t fills no earlier than a STRICTLY-LATER slice
//      (the loop settles the prior batch BEFORE queueing this bar's orders, and
//      the ExecutionSimulator additionally refuses any fill whose bar end_time
//      <= queued_at). This is the no-look-ahead default a reviewer must find on.
//    * Same  (delay-0, OPT-IN): the less-conservative relaxation that lets an
//      order fill on the SAME bar's close it was decided on. Wired to the
//      ExecutionSimulator's FillCfg.allow_same_bar_fill relaxation (which itself
//      defaults OFF). Use only with eyes open — it trades on the close the
//      decision read, so it is mildly optimistic, not look-ahead in the panel
//      sense (the signal still reads only sealed <= t rows).
// ===========================================================================
enum class Delay : atx::u8 {
  Same, // delay-0: fill on THIS bar's close (opt-in; relaxes the exec firewall)
  Next, // delay-1: fill on a strictly-later slice (default; firewall intact)
};

} // namespace atx::engine
