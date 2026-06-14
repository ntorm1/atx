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
//     value AS OF that date (greatest plug row ≤ canonical_date). Appending a
//     later-dated "restatement"/future plug row never changes an earlier
//     aligned cell — see as_of_index in dataset.hpp.
//   * NO SURVIVORSHIP. A delisted plug instrument (no rows after some date)
//     carries its FINAL value forward — the natural consequence of as-of
//     resolution returning the last row ≤ the canonical date.
//
// Extra plug data that cannot land on the canonical axis (instruments outside
// the canonical universe, or plug rows dated after the last canonical date) is
// DROPPED and COUNTED in a DropReport — a diagnostic, not data.
//
// Cold-path (once per backtest window); vector/map allocation is intentional.

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
  // Plug positions whose InstKey IS canonical but whose DateKey is later than
  // the last canonical date — future rows that are never visible on the axis.
  atx::usize extra_date_cells = 0;

  [[nodiscard]] atx::usize total() const noexcept {
    return extra_instrument_cells + extra_date_cells;
  }
};

// The plug's columns re-expressed over the canonical (date × instrument) grid.
struct AlignedView {
  atx::usize num_dates = 0;         // == canonical num_dates
  atx::usize num_instruments = 0;   // == canonical num_instruments
  std::vector<std::string> columns; // == plug schema column names, same order
  // One vector per plug column; each is num_dates*num_instruments, date-major
  // over the CANONICAL axis (cell (d, i) at d*num_instruments + i).
  std::vector<std::vector<atx::f64>> aligned_columns;
  DropReport drops;
};

// Align every `plug` column onto the canonical (date × instrument) axis fixed
// by `canonical_price`.
//
// Err(InvalidArgument) if either dataset's dates() are not strictly ascending
// (strict ascent is required for as-of binary search).
[[nodiscard]] atx::core::Result<AlignedView> align_onto(const Dataset &canonical_price,
                                                        const Dataset &plug);

} // namespace atx::engine::data
