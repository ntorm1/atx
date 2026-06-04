#pragma once

// atx::engine::loop — the bar->panel bridge value types (P2-2).
//
// Three types live here, split out of rolling_panel.hpp so the panel's public
// data contract (what goes IN, what comes OUT) is a single small header that the
// downstream Phase-2/Phase-3 units (ISignalSource::evaluate, the VM adapter, the
// weight policy) can include without pulling in the ring storage machinery:
//
//   SliceRow    — one instrument's sealed bar within a cross-section.
//   MarketSlice — one sealed cross-section at a timestamp (a non-owning span of
//                 SliceRow). This is the input to RollingPanel::append_sealed_row.
//   PanelView   — a non-owning, ZERO-COPY read view over the panel's ring
//                 storage. This is the input to ISignalSource::evaluate (P2-3).
//
// ===========================================================================
//  Ownership / lifetime
// ===========================================================================
//  Both MarketSlice and PanelView are NON-OWNING.
//
//  * MarketSlice.rows is a span into a SCRATCH BUFFER the backtest loop owns and
//    refills each frontier (it drains the event bus into a vector<SliceRow>, then
//    hands the span to append_sealed_row). RollingPanel copies the f64 fields it
//    needs out of the span during append; the span need not outlive the call.
//
//  * PanelView holds raw pointers back INTO the panel's ring buffer plus the ring
//    geometry needed to address it. It is valid only until the next
//    append_sealed_row (which overwrites a ring row) or the panel's destruction.
//    Treat it like an iterator: read it within the decision step, never store it.
//
// ===========================================================================
//  Why PanelView is an accessor, not flat spans
// ===========================================================================
//  The panel is a ring: the newest row may sit anywhere in [0, Cap), and the
//  trailing window can straddle the wrap point. A flat contiguous std::span per
//  field therefore cannot express "the last max_lookback rows, newest-first"
//  without a copy. PanelView instead stores (base pointer, head index, valid row
//  count, geometry) and computes the physical ring offset per access — O(1), no
//  copy, no allocation. row 0 is always the newest; increasing row indices walk
//  backward in time. Absent cells read as NaN and present()==false.

#include <cstdint> // std::uint8_t (PanelField underlying type)
#include <span>    // std::span — non-owning row / universe views

#include "atx/core/datetime.hpp"      // atx::core::time::Timestamp
#include "atx/core/domain/domain.hpp" // atx::core::domain::Bar
#include "atx/core/macro.hpp"         // ATX_ASSERT (bounds preconditions)
#include "atx/core/types.hpp"         // atx::usize, atx::u64, atx::f64

#include "atx/engine/loop/types.hpp" // atx::engine::InstrumentId

namespace atx::engine {

// ===========================================================================
//  SliceRow — one instrument's sealed bar inside a cross-section.
//
//  `delisted_final` mirrors data::BarRow / MarketPayload: it marks this as the
//  instrument's LAST bar (survivorship). The bar is written normally for the row
//  it arrives in (the instrument traded that session); the instrument simply
//  stops appearing in later slices, which IS its exclusion from the membership
//  mask going forward — no retroactive erasure of its historical rows.
// ===========================================================================
struct SliceRow {
  InstrumentId id{};
  atx::core::domain::Bar bar{};
  bool delisted_final{false};
};

// ===========================================================================
//  MarketSlice — one sealed cross-section at timestamp `t`.
//
//  `rows` is non-owning (see header ownership note). A row whose `id` is not in
//  the panel's construction-time universe is ignored on append. Universe members
//  absent from `rows` become NaN / present==false cells for this row.
// ===========================================================================
struct MarketSlice {
  // No `{}`: Timestamp's defaulted ctor value-inits to epoch and std::span's
  // defaulted ctor yields an empty span, so an explicit `{}` here is flagged
  // redundant (mirrors data::BarRow's member-init convention).
  atx::core::time::Timestamp t;
  std::span<const SliceRow> rows;
};

// ===========================================================================
//  Field index — the five OHLCV fields stored per cell, in storage order.
//
//  vwap / returns are deferred to Phase 3 (computed in the VM over this panel,
//  not stored here). Keeping the enum the single source of field ordering means
//  the storage layout and the PanelView accessors cannot drift apart.
// ===========================================================================
enum class PanelField : std::uint8_t { Open = 0, High = 1, Low = 2, Close = 3, Volume = 4 };

/// Number of f64 fields stored per (row, instrument) cell.
inline constexpr atx::usize kPanelFieldCount = 5;

// ===========================================================================
//  PanelView — zero-copy read view over a RollingPanel's ring storage.
//
//  Construction is private to RollingPanel (via the friend-accessible aggregate
//  ctor); strategies receive a PanelView from RollingPanel::view() and only read.
//
//  Addressing: a field block is `Cap` rows x `n_inst` columns of f64, row-major
//  within the block (one cross-section row is `n_inst` contiguous f64 — the
//  column-major-per-field layout the plan calls for). `row_from_newest == 0` is
//  the newest sealed row; the physical ring row is computed from `head_`.
// ===========================================================================
class PanelView {
public:
  /// Build a view over a panel's ring storage. Called only by RollingPanel.
  /// `fields` points at the [kPanelFieldCount][Cap][n_inst] f64 block; `mask`
  /// at the [Cap][mask_words] membership bitmap; `universe` is the fixed column
  /// order. `head` is the physical ring index of the NEWEST sealed row (valid
  /// only when `valid_rows > 0`). `cap` is the ring capacity (power of two).
  PanelView(const atx::f64 *fields, const atx::u64 *mask, std::span<const InstrumentId> universe,
            atx::usize cap, atx::usize head, atx::usize valid_rows, atx::usize mask_words) noexcept
      : fields_{fields}, mask_{mask}, universe_{universe}, cap_{cap}, head_{head},
        valid_rows_{valid_rows}, mask_words_{mask_words} {}

  /// Number of valid trailing rows available (<= max_lookback). row 0 = newest.
  [[nodiscard]] atx::usize rows() const noexcept { return valid_rows_; }

  /// Number of instrument columns (the fixed universe size).
  [[nodiscard]] atx::usize instruments() const noexcept { return universe_.size(); }

  /// The fixed universe in column order (column index == universe index).
  [[nodiscard]] std::span<const InstrumentId> universe() const noexcept { return universe_; }

  // ---- per-field, newest-first access (row 0 == newest) --------------------
  // Each PRECONDITION (row < rows(), inst < instruments()) ABORTS in debug.
  // Absent cells read as NaN (a quiet NaN written at append time).

  [[nodiscard]] atx::f64 open(atx::usize row_from_newest, atx::usize inst) const noexcept {
    return field(PanelField::Open, row_from_newest, inst);
  }
  [[nodiscard]] atx::f64 high(atx::usize row_from_newest, atx::usize inst) const noexcept {
    return field(PanelField::High, row_from_newest, inst);
  }
  [[nodiscard]] atx::f64 low(atx::usize row_from_newest, atx::usize inst) const noexcept {
    return field(PanelField::Low, row_from_newest, inst);
  }
  [[nodiscard]] atx::f64 close(atx::usize row_from_newest, atx::usize inst) const noexcept {
    return field(PanelField::Close, row_from_newest, inst);
  }
  [[nodiscard]] atx::f64 volume(atx::usize row_from_newest, atx::usize inst) const noexcept {
    return field(PanelField::Volume, row_from_newest, inst);
  }

  /// True iff instrument `inst` had a bar at row `row_from_newest`.
  /// PRECONDITION: row_from_newest < rows() and inst < instruments() (ABORTS).
  [[nodiscard]] bool present(atx::usize row_from_newest, atx::usize inst) const noexcept {
    const atx::usize phys = physical_row(row_from_newest, inst);
    // SAFETY: phys < cap_ and inst < n_inst (physical_row asserts both); the
    //         linear offset into the mask bitmap is bounded by the panel's
    //         [cap_][mask_words_] allocation. Index into a flat ring array — no
    //         per-cell span is possible because the trailing window wraps.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const atx::u64 word = mask_[phys * mask_words_ + (inst >> 6U)];
    return ((word >> (inst & 63U)) & 1ULL) != 0ULL;
  }

private:
  /// Map a newest-first row index to its physical ring row, with bounds checks.
  [[nodiscard]] atx::usize physical_row(atx::usize row_from_newest,
                                        atx::usize inst) const noexcept {
    ATX_ASSERT(row_from_newest < valid_rows_);
    ATX_ASSERT(inst < universe_.size());
    // SAFETY: head_ is the newest physical row; stepping back `row_from_newest`
    //         rows wraps within [0, cap_). cap_ is a power of two so the mask is
    //         (cap_ - 1). valid_rows_ <= cap_ guarantees no aliasing of a row
    //         outside the live window.
    return (head_ + cap_ - row_from_newest) & (cap_ - 1U);
  }

  /// Read one f64 field cell (newest-first addressing). Absent => NaN by storage.
  [[nodiscard]] atx::f64 field(PanelField f, atx::usize row_from_newest,
                               atx::usize inst) const noexcept {
    const atx::usize phys = physical_row(row_from_newest, inst);
    const atx::usize field_block = static_cast<atx::usize>(f) * cap_ * universe_.size();
    // SAFETY: bounded by the [kPanelFieldCount][cap_][n_inst] field block; phys
    //         and inst are asserted in range by physical_row. Flat ring index.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return fields_[field_block + phys * universe_.size() + inst];
  }

  const atx::f64 *fields_;                 // non-owning: panel's [field][Cap][n_inst] block
  const atx::u64 *mask_;                   // non-owning: panel's [Cap][mask_words] bitmap
  std::span<const InstrumentId> universe_; // fixed column order
  atx::usize cap_;                         // ring capacity (power of two)
  atx::usize head_;                        // physical ring index of the newest sealed row
  atx::usize valid_rows_;                  // count of live trailing rows (<= max_lookback)
  atx::usize mask_words_;                  // u64 words per row in the membership bitmap
};

} // namespace atx::engine
