#pragma once

// atx::engine::data — align_onto: the point-in-time (PIT) alignment rail.
//
// align_onto joins a `plug` Dataset (a feature / signal / reference block) onto
// the FIXED canonical (date × instrument) axis defined by a `canonical_price`
// Dataset. The output axis is exactly the canonical axis, so the join is
// deterministic regardless of the plug's internal ordering.
//
// Three PIT invariants are enforced here, once, so no downstream consumer can
// violate them:
//   * MISSING coverage → NaN, never imputed. A canonical (date, inst) the plug
//     does not cover (instrument absent, or no plug row on/before that date)
//     resolves to quiet NaN. NaN is never silently replaced by zero.
//   * NO LOOK-AHEAD (truncation-invariant). Each canonical date reads the plug
//     value AS OF that date (greatest availability date <= canonical_date). Appending a
//     later-dated "restatement"/future plug row never changes an earlier
//     aligned cell. Availability includes the declared calendar-day pit_delay.
//   * NO SURVIVORSHIP. A delisted plug instrument (no rows after some date)
//     carries its FINAL value forward — the natural consequence of as-of
//     resolution returning the last row ≤ the canonical date.
//
// Extra plug data that cannot land on the canonical axis (instruments outside
// the canonical universe, or plug rows dated after the last canonical date) is
// DROPPED and COUNTED in a DropReport — a diagnostic, not data.
//
// Cold-path (once per backtest window); vector/map allocation is intentional.

#include <limits>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

// Diagnostic counts of plug (date, inst) positions that could NOT land on the
// canonical axis. Positions are classified ONCE (no per-column double-count).
struct DropReport {
  // Plug positions whose InstKey is NOT in the canonical universe.
  atx::usize extra_instrument_cells = 0;
  // Plug positions whose InstKey IS canonical but whose availability is later than
  // the last canonical date — future rows that are never visible on the axis.
  atx::usize extra_date_cells = 0;

  [[nodiscard]] atx::usize total() const noexcept {
    return extra_instrument_cells + extra_date_cells;
  }
};

// The plug's columns re-expressed over the canonical (date × instrument) grid.
// Values already include the plug's reporting delay. Rewrapping these values
// into a Dataset requires pit_delay=0; retaining the original lag applies it twice.
struct AlignedView {
  atx::usize num_dates = 0;         // == canonical num_dates
  atx::usize num_instruments = 0;   // == canonical num_instruments
  std::vector<std::string> columns; // == plug schema column names, same order
  // One vector per plug column; each is num_dates*num_instruments, date-major
  // over the CANONICAL axis (cell (d, i) at d*num_instruments + i).
  std::vector<std::vector<atx::f64>> aligned_columns;
  DropReport drops;
};

// =========================================================================
//  Event columns and staleness caps (W0-D0, D-05)
// =========================================================================
//
// The plain as-of join forward-fills every column without limit. That is right
// for a slowly-changing reference value, but wrong for EVENT columns: a cash
// dividend forward-filled onto the following sessions is counted again on each of
// them, and a split factor forward-filled past the plug's last row freezes. A
// column rule caps how stale the joined row may be, counted in CANONICAL sessions:
//
//   staleness(d) = number of canonical dates in (row availability, canonical_date[d]]
//
// so staleness 0 means the row is available exactly on the canonical session (or,
// when its date is not on the canonical axis, on the first canonical session after
// it — once, never again). A row available before the first canonical date has
// unknown staleness and never passes a finite cap (fail closed). A cell whose row
// is staler than the cap is NaN.
inline constexpr atx::usize kAlignUnboundedStaleness = std::numeric_limits<atx::usize>::max();
inline constexpr atx::usize kAlignEventSession = 0; // event column: join once, never fill

struct AlignColumnRule {
  // Largest allowed staleness in canonical sessions; kAlignUnboundedStaleness is the
  // legacy unlimited forward fill.
  atx::usize max_stale_sessions = kAlignUnboundedStaleness;
};

struct AlignOptions {
  // Empty: every column uses the legacy unlimited as-of join. Otherwise exactly one
  // rule per plug column, in plug column order.
  std::vector<AlignColumnRule> column_rules{};
  // When true, a canonical date later than the plug's last availability date fails
  // with Err(OutOfRange) instead of silently reusing the plug's final row.
  bool require_coverage = false;
};

// Align every `plug` column onto the canonical (date × instrument) axis fixed
// by `canonical_price`, with the legacy unlimited as-of join on every column
// (== align_onto(canonical_price, plug, AlignOptions{})). Correct for reference /
// feature plugs; event plugs (corporate actions) use the options overload.
//
// Err(InvalidArgument) if either dataset's dates() are not strictly ascending
// or date encodings differ (including typed versus Opaque).
[[nodiscard]] atx::core::Result<AlignedView> align_onto(const Dataset &canonical_price,
                                                        const Dataset &plug);

// Same join with per-column staleness caps and an optional coverage guard (see
// above). Additional errors: Err(InvalidArgument) if column_rules is neither empty
// nor one per plug column; Err(OutOfRange) if require_coverage and the canonical
// dates extend past the plug's last availability date (or the plug has no dates).
[[nodiscard]] atx::core::Result<AlignedView>
align_onto(const Dataset &canonical_price, const Dataset &plug, const AlignOptions &options);

} // namespace atx::engine::data
