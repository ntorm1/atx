#pragma once

// atx::engine::alpha — columnar evaluation context (Panel + SlotPool +
// SignalSet) (P3-5).
//
// These three value types are the data plane the tree-walking reference oracle
// (oracle.hpp) and, later, the fast vectorized VM (P3-6) execute against:
//
//   * Panel     — the read-only INPUT: a date x instrument block of field
//                 values plus a point-in-time universe mask. Self-contained
//                 (owns its data); see the design note below on why this is NOT
//                 a thin view over atx-core's row-oriented `series::Frame`.
//   * SlotPool  — the SCRATCH plane: a pre-sized arena of `dates*instruments`
//                 f64 buffers the linearized Program writes through, one buffer
//                 per live SlotId. acquire()/release() recycle slots exactly as
//                 the bytecode linearizer's Free instructions schedule.
//   * SignalSet — the OUTPUT: one date-major f64 column per Program root, named
//                 by its StoreAlpha.
//
// Ownership / lifetime:
//   * Panel and SignalSet own all their storage by value (Rule of Zero).
//   * SlotPool owns its backing buffer; `column()` hands out non-owning spans
//     that alias it and are valid until the SlotPool is destroyed or the slot
//     is released and re-acquired (the caller must not retain a released span).
//   * `field_cross_section` / `field_all` / `alpha_cross_section` return spans
//     borrowing the owner's storage — never outlive the Panel/SignalSet.
//
// DESIGN NOTE — Panel is SELF-CONTAINED, not a `series::Frame` view.
//   atx-core's `series::Frame` is a row-table (a timestamp index + named typed
//   columns), NOT a 3-D date x instrument panel. A "thin view" would have to
//   reshape a row-major frame into a date-major cross-section on every access,
//   which is both awkward and a hidden cost. The oracle is the obviously-correct
//   REFERENCE, so Panel owns a flat date-major f64 array (+ a parallel u8
//   universe mask) and offers O(1) cross-section / whole-field spans. Ingest
//   adapters can fill a Panel from a Frame at the boundary; that is out of scope
//   here.
//
// Header-only; every free function / member is defined inline. Construction is a
// COLD path (once per backtest window), so std::vector allocation is fine.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/fwd.hpp" // struct SignalSet; (forward-declared)

namespace atx::engine::alpha {

// Stable id of a field within a Panel (== position in its field dictionary).
using FieldId = atx::u32;

// Index of a single date row within a Panel (0 == earliest date).
using DateIdx = atx::usize;

// Index of a value slot in the VM's runtime slot pool. Identical to the alias
// bytecode.hpp declares (a `using` redeclaration with the same target type is
// legal, so a TU that includes both headers sees one consistent SlotId).
using SlotId = atx::u32;

// =========================================================================
//  Panel — the read-only date x instrument input block.
//
//  Field values are stored DATE-MAJOR: the value of (field, date, inst) lives
//  at flat index `date * instruments + inst` within that field's column. A cell
//  is MISSING — and reads back as NaN through the oracle's LoadField — when it
//  is out-of-universe at that date OR NaN in the source (point-in-time: a not-
//  yet-listed / delisted instrument reads NaN, never a stale or look-ahead
//  value). Group/classifier fields store integer labels widened to f64.
// =========================================================================

class Panel {
public:
  // Build a Panel from raw column-major field data + an optional universe mask.
  //
  // Contract:
  //   * `field_names.size()` MUST equal `field_data.size()` (one column per
  //     field) and each column MUST have exactly `dates*instruments` cells.
  //   * `universe` is `dates*instruments` of {0,1} (1 == in-universe). An EMPTY
  //     universe is a convenience for "all cells in-universe".
  //   * Any ragged input (mismatched counts) -> Err(InvalidArgument). A
  //     zero-field Panel is permitted (dates/instruments still define its shape).
  [[nodiscard]] static atx::core::Result<Panel>
  create(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
         std::vector<std::vector<atx::f64>> field_data, std::vector<std::uint8_t> universe) {
    if (field_names.size() != field_data.size()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Panel::create: field_names and field_data size mismatch");
    }
    const atx::usize cells = dates * instruments;
    for (const std::vector<atx::f64> &col : field_data) {
      if (col.size() != cells) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "Panel::create: a field column is not dates*instruments cells");
      }
    }
    if (!universe.empty() && universe.size() != cells) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Panel::create: universe is neither empty nor dates*instruments cells");
    }

    Panel p;
    p.dates_ = dates;
    p.instruments_ = instruments;
    p.field_names_ = std::move(field_names);
    p.backing_ = std::move(field_data);
    p.columns_.reserve(p.backing_.size());
    for (const std::vector<atx::f64> &col : p.backing_) {
      p.columns_.emplace_back(col.data(), col.size());
    }
    // Empty universe == all-valid: materialize an all-ones mask so in_universe()
    // is a single O(1) read with no special-casing on the hot path.
    if (universe.empty()) {
      p.universe_.assign(cells, std::uint8_t{1});
    } else {
      p.universe_ = std::move(universe);
    }
    return atx::core::Ok(std::move(p));
  }

  // Build a Panel whose field columns are BORROWED (no copy). Each span in
  // `columns` must have exactly dates*instruments cells and must outlive the
  // Panel (caller's contract — e.g. a mmap held by a MappedPanel). The universe
  // mask is still OWNED (materialized small mask); empty == all-in-universe.
  [[nodiscard]] static atx::core::Result<Panel>
  create_borrowed(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
                  std::vector<std::span<const atx::f64>> columns,
                  std::vector<std::uint8_t> universe) {
    if (field_names.size() != columns.size()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Panel::create_borrowed: field_names and columns size mismatch");
    }
    const atx::usize cells = dates * instruments;
    for (const std::span<const atx::f64> &col : columns) {
      if (col.size() != cells) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "Panel::create_borrowed: a column is not dates*instruments cells");
      }
    }
    if (!universe.empty() && universe.size() != cells) {
      return atx::core::Err(
          atx::core::ErrorCode::InvalidArgument,
          "Panel::create_borrowed: universe is neither empty nor dates*instruments cells");
    }
    Panel p;
    p.dates_ = dates;
    p.instruments_ = instruments;
    p.field_names_ = std::move(field_names);
    p.columns_ = std::move(columns); // borrowed; backing_ stays empty
    if (universe.empty()) {
      p.universe_.assign(cells, std::uint8_t{1});
    } else {
      p.universe_ = std::move(universe);
    }
    return atx::core::Ok(std::move(p));
  }

  // Rule of five: columns_ caches spans into backing_ for owned panels. A naive
  // copy would deep-copy backing_ but leave the copy's spans pointing at the
  // SOURCE's backing_ (dangling once the source dies). Re-point on copy. Move is
  // safe by default: moving a vector preserves its elements' addresses, so the
  // moved spans still alias the moved backing_.
  Panel(const Panel &other) { copy_from(other); }
  Panel &operator=(const Panel &other) {
    if (this != &other) {
      copy_from(other);
    }
    return *this;
  }
  Panel(Panel &&) noexcept = default;
  Panel &operator=(Panel &&) noexcept = default;
  ~Panel() = default;

  [[nodiscard]] atx::usize dates() const noexcept { return dates_; }
  [[nodiscard]] atx::usize instruments() const noexcept { return instruments_; }
  [[nodiscard]] atx::usize cells() const noexcept { return dates_ * instruments_; }
  [[nodiscard]] atx::usize num_fields() const noexcept { return field_names_.size(); }

  // Resolve a field name to its id, or Err(NotFound) if absent.
  [[nodiscard]] atx::core::Result<FieldId> field_id(std::string_view name) const {
    for (atx::usize i = 0; i < field_names_.size(); ++i) {
      if (field_names_[i] == name) {
        return atx::core::Ok(static_cast<FieldId>(i));
      }
    }
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          std::string{"Panel::field_id: unknown field '"} + std::string{name} +
                              "'");
  }

  // One date's cross-section for a field (length == instruments()). Precondition:
  // `field` < num_fields() and `date` < dates() (caller-validated; the oracle
  // only ever passes ids it resolved from this Panel).
  [[nodiscard]] std::span<const atx::f64> field_cross_section(FieldId field,
                                                              DateIdx date) const noexcept {
    ATX_ASSERT(field < columns_.size());
    ATX_ASSERT(date < dates_);
    return columns_[field].subspan(date * instruments_, instruments_);
  }

  // The whole field, date-major (length == dates*instruments).
  [[nodiscard]] std::span<const atx::f64> field_all(FieldId field) const noexcept {
    ATX_ASSERT(field < columns_.size());
    return columns_[field];
  }

  // Point-in-time universe membership of (date, inst). Precondition: indices in
  // range (caller-validated).
  [[nodiscard]] bool in_universe(DateIdx date, atx::usize inst) const noexcept {
    ATX_ASSERT(date < dates_);
    ATX_ASSERT(inst < instruments_);
    return universe_[date * instruments_ + inst] != 0;
  }

private:
  Panel() = default; // built only via create() / create_borrowed()

  // Deep-copy state; rebuild columns_ to alias THIS panel's backing_ when owned.
  // A borrowed panel (backing_ empty) keeps its external column spans verbatim.
  void copy_from(const Panel &o) {
    dates_ = o.dates_;
    instruments_ = o.instruments_;
    field_names_ = o.field_names_;
    universe_ = o.universe_;
    backing_ = o.backing_;
    if (backing_.empty()) {
      columns_ = o.columns_; // borrowed (or zero-field): external spans copy fine
    } else {
      columns_.clear();
      columns_.reserve(backing_.size());
      for (const std::vector<atx::f64> &col : backing_) {
        columns_.emplace_back(col.data(), col.size());
      }
    }
  }

  atx::usize dates_{};
  atx::usize instruments_{};
  std::vector<std::string> field_names_;
  // Access plane: one span per field (date-major, dates*instruments). Spans point
  // either into `backing_` (owned panels) or into caller/mmap memory (borrowed
  // panels). Every read goes through here, so owned and borrowed share one code
  // path with no per-access branch.
  std::vector<std::span<const atx::f64>> columns_;
  // Owned column storage; EMPTY for a borrowed panel. SAFETY: moving a Panel
  // moves this outer vector by buffer transfer (no reallocation), so the inner
  // vectors' data() pointers — and therefore the spans in columns_ that alias
  // them — stay valid across the create()-return-by-value move.
  std::vector<std::vector<atx::f64>> backing_;
  std::vector<std::uint8_t> universe_; // dates*instruments, 1 == in-universe
};

// =========================================================================
//  SlotPool — pre-sized arena of `dates*instruments` f64 buffers.
//
//  The oracle executes the linearized Program once top-to-bottom; each live
//  SlotId owns a WHOLE date x instrument buffer (a Scalar is broadcast to fill
//  it, a CrossSection result is written per date-row, a Panel fills naturally).
//  acquire() hands out the next free slot; release() returns it. Over-acquiring
//  past `capacity` (== Program::num_slots) is a precondition failure, not a
//  recoverable error — the linearizer's peak-live-slot count is authoritative.
// =========================================================================

class SlotPool {
public:
  // Pre-size for `num_slots` simultaneously-live buffers of `cells_per_slot`
  // f64s each. All buffers are allocated up front (cold path); acquire() never
  // allocates.
  explicit SlotPool(atx::usize num_slots, atx::usize cells_per_slot)
      : cells_{cells_per_slot}, capacity_{num_slots},
        storage_(num_slots * cells_per_slot, atx::f64{}) {}

  // Hand out the next slot. SAFETY: over-acquire is a contract violation — the
  // Program's num_slots is the proven peak live count, so a well-formed Program
  // never exceeds it. ATX_ASSERT trips (aborts) in debug; release builds rely on
  // the linearizer's invariant.
  [[nodiscard]] SlotId acquire() noexcept {
    ATX_ASSERT(live_ < capacity_);
    return static_cast<SlotId>(live_++);
  }

  // Return a slot to the pool. SAFETY: the oracle releases in LIFO-agnostic
  // order via Free instructions; because every slot buffer is independent and
  // distinctly addressed, the pool only needs to track the live HIGH-WATER count
  // for the acquire precondition — a freed slot's buffer keeps its address, so
  // outstanding writes never alias a re-acquired slot within one Program (the
  // linearizer guarantees a slot is re-acquired only after its Free).
  void release(SlotId /*slot*/) noexcept {
    ATX_ASSERT(live_ > 0);
    --live_;
  }

  // Mutable view of a slot's buffer (length == cells_per_slot).
  [[nodiscard]] std::span<atx::f64> column(SlotId slot) noexcept {
    ATX_ASSERT(static_cast<atx::usize>(slot) < capacity_);
    return std::span<atx::f64>{storage_.data() + static_cast<atx::usize>(slot) * cells_, cells_};
  }

  // Const view of a slot's buffer.
  [[nodiscard]] std::span<const atx::f64> column(SlotId slot) const noexcept {
    ATX_ASSERT(static_cast<atx::usize>(slot) < capacity_);
    return std::span<const atx::f64>{storage_.data() + static_cast<atx::usize>(slot) * cells_,
                                     cells_};
  }

  [[nodiscard]] atx::usize cells_per_slot() const noexcept { return cells_; }
  [[nodiscard]] atx::usize capacity() const noexcept { return capacity_; }
  [[nodiscard]] atx::usize live() const noexcept { return live_; }

private:
  atx::usize cells_{};
  atx::usize capacity_{};
  atx::usize live_{0};            // current high-water of acquired slots
  std::vector<atx::f64> storage_; // capacity_ * cells_ contiguous f64s
};

// =========================================================================
//  SignalSet — the oracle / VM output (one alpha per Program root).
//
//  This is the FULL definition of the type forward-declared in fwd.hpp as
//  `struct SignalSet;`. Each alpha's `values` is date-major (dates*instruments),
//  NaN where the cell is masked / undefined.
// =========================================================================

struct SignalSet {
  struct Alpha {
    std::string name;             // owned; the StoreAlpha root name (may be empty)
    std::vector<atx::f64> values; // date-major, dates*instruments
  };

  std::vector<Alpha> alphas; // one per Program root, indexed by StoreAlpha output index
  atx::usize dates{};
  atx::usize instruments{};

  // One date's cross-section of alpha `a` (length == instruments). Precondition:
  // `a` < alphas.size() and `date` < dates (caller-validated).
  [[nodiscard]] std::span<const atx::f64> alpha_cross_section(atx::usize a,
                                                              DateIdx date) const noexcept {
    ATX_ASSERT(a < alphas.size());
    ATX_ASSERT(date < dates);
    const std::vector<atx::f64> &v = alphas[a].values;
    return std::span<const atx::f64>{v.data() + date * instruments, instruments};
  }
};

} // namespace atx::engine::alpha
