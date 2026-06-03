#pragma once

// atx::engine::SimClock — monotonic simulation clock + point-in-time gate (P1-4).
//
// The look-ahead defence: a consumer of market data may ONLY see records whose
// knowledge_time is at or before the current clock value. Advancing the clock is
// the ONLY mechanism by which new data becomes visible. This prevents any
// accidental look-ahead bias — a common source of overfitted backtests.
//
// Ownership / threading:
//   SimClock is a plain value type (Rule of Zero; trivially default-constructible).
//   In Phase 1 (single-threaded backtest) the DataHandler is the SOLE advancer;
//   no synchronisation is required. Do not share a SimClock across threads without
//   external locking.
//
// ===========================================================================
//  Contract
// ===========================================================================
//  Invariant:  now() is non-decreasing across the lifetime of the object.
//  advance_to: precondition t >= now() (ATX_ASSERT; debug-aborts on violation).
//              Advancing to the current timestamp (t == now) is valid (idempotent).
//  is_visible: returns true iff knowledge_ts <= now(). The gate is inclusive on
//              both sides of the boundary: a record known exactly at now() is
//              admissible; one known one nanosecond in the future is not.
//
//  Restatement semantics: when two records share an event_ts but carry different
//  knowledge_ts values (an original filing and a later restatement), the gate
//  admits only the record(s) whose knowledge_ts <= now(). Before the restatement's
//  knowledge time the caller sees only the original value; after the clock
//  advances past the restatement's knowledge time both are visible and the caller
//  should pick the latest admissible record (highest knowledge_ts <= now). The
//  "pick latest" selection is caller logic; SimClock only provides the gate.

#include "atx/core/datetime.hpp"
#include "atx/core/macro.hpp"

namespace atx::engine {

// ===========================================================================
//  SimClock
// ===========================================================================
class SimClock {
public:
  // Default-construct at the Unix epoch (ns = 0). Any record with
  // knowledge_ts == epoch() is immediately visible — useful for synthetic or
  // zero-indexed test datasets.
  SimClock() noexcept = default;

  // -------------------------------------------------------------------------
  //  now() — the current simulation instant.
  // -------------------------------------------------------------------------
  /// @return  The timestamp the engine currently believes is "now".
  ///          Monotonically non-decreasing; only changes via advance_to().
  [[nodiscard]] atx::core::time::Timestamp now() const noexcept { return now_; }

  // -------------------------------------------------------------------------
  //  advance_to() — move the simulation forward to t.
  // -------------------------------------------------------------------------
  /// Move the clock to `t`.
  ///
  /// Precondition: t >= now()  (monotonic; ATX_ASSERT fires in debug builds).
  /// Advancing to the current timestamp (t == now()) is valid and idempotent.
  /// The DataHandler is the sole caller of this function in Phase 1 backtest
  /// mode; no other component should advance the clock directly.
  ///
  /// @param t  The new simulation instant. Must be >= now().
  // advance_to is intentionally NOT constexpr: ATX_ASSERT expands to logging +
  // std::abort(), which are not constexpr-compatible (matching datetime.hpp's
  // approach of keeping runtime-checking functions non-constexpr).
  void advance_to(atx::core::time::Timestamp t) noexcept {
    // Monotonic invariant: the clock may only move forward. A backward advance
    // indicates a bug in the event-ordering layer (e.g. out-of-order events
    // delivered to the DataHandler). Fail loud in debug; compiled out in release.
    ATX_ASSERT(t.unix_nanos() >= now_.unix_nanos());
    now_ = t;
  }

  // -------------------------------------------------------------------------
  //  is_visible() — the point-in-time look-ahead gate.
  // -------------------------------------------------------------------------
  /// Return true iff a record with the given knowledge_ts is admissible as of
  /// now(). A record is visible when its knowledge_ts <= now(), i.e. the
  /// information it carries was already knowable at the current simulation
  /// instant. A knowledge_ts strictly greater than now() denotes future
  /// information — invisible until the clock advances to or past that point.
  ///
  /// @param knowledge_ts  The timestamp at which the record's information
  ///                      became available to market participants.
  /// @return              true  → the record is knowable as of now();
  ///                      false → the record is a future revision, invisible.
  [[nodiscard]] bool is_visible(atx::core::time::Timestamp knowledge_ts) const noexcept {
    return knowledge_ts <= now_;
  }

private:
  // Starts at epoch (unix nanos = 0). Default-constructed Timestamp == epoch
  // by the Timestamp invariant (ns_ member zero-initialised); no explicit {}
  // needed — the defaulted constructor handles it.
  atx::core::time::Timestamp now_;
};

} // namespace atx::engine
