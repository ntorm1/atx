#pragma once

// atx::engine::data — Dataset: a typed, point-in-time, date x instrument block.
//
// Dataset is the foundational data-plane value for the S6 data layer. It holds
// one or more named f64 columns in DATE-MAJOR flat layout: cell (d, i) for
// column c lives at flat index `d * num_instruments + i` in columns_[c].
//
// Construction is COLD-PATH (once per backtest window). std::vector allocation
// is intentional and explicit. The Rule of Zero applies: all members are value
// types, so copy/move/destroy are compiler-generated.
//
// `column()` returns a std::span borrowing an owned inner vector — spans are
// stable across moves of the Dataset (moving a vector moves its buffer; the
// inner vectors' data() pointers do NOT change, so spans into them remain
// valid). Do NOT hold a span past the Dataset's lifetime.
//
// Defined in two files:
//   include/…/dataset.hpp  — class definition + inline accessors
//   src/data/dataset.cpp   — Dataset::create validation body

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atx::engine::data {

// =========================================================================
//  Dataset — columnar, date-major, PIT-versioned data block.
// =========================================================================
class Dataset {
public:
  // Build a Dataset from raw column data.
  //
  // Contract (each violation -> Err(InvalidArgument, "<clear message>")):
  //   1. schema_is_coherent(schema) must be true.
  //   2. columns.size() must equal schema.columns.size().
  //   3. Every column must have exactly dates.size()*instruments.size() cells
  //      (ragged rejected).
  //   4. mask, if non-empty, must be dates.size()*instruments.size() bytes of
  //      {0,1}.
  //
  // Empty dates or instruments (zero-cell Dataset) is permitted as long as
  // all column vectors are also empty.
  [[nodiscard]] static atx::core::Result<Dataset>
  create(DatasetSchema schema, std::vector<DateKey> dates, std::vector<InstKey> instruments,
         std::vector<std::vector<atx::f64>> columns, std::vector<std::uint8_t> mask,
         DatasetProvenance provenance);

  // ---- Accessors (inline, [[nodiscard]], noexcept where pure) ----

  [[nodiscard]] const DatasetSchema &schema() const noexcept { return schema_; }

  [[nodiscard]] atx::usize num_dates() const noexcept { return dates_.size(); }

  [[nodiscard]] atx::usize num_instruments() const noexcept { return instruments_.size(); }

  [[nodiscard]] atx::usize cells() const noexcept { return dates_.size() * instruments_.size(); }

  [[nodiscard]] std::span<const DateKey> dates() const noexcept { return dates_; }

  [[nodiscard]] std::span<const InstKey> instruments() const noexcept { return instruments_; }

  // Convenience: == schema().role.
  [[nodiscard]] Role role() const noexcept { return schema_.role; }

  // Whole column `col_idx` (date-major, length == cells()).
  // Precondition: col_idx < schema().columns.size() (ATX_ASSERT-checked).
  [[nodiscard]] std::span<const atx::f64> column(atx::usize col_idx) const noexcept {
    ATX_ASSERT(col_idx < columns_.size());
    return columns_[col_idx];
  }

  // Resolve by column name; Err(NotFound) if the name is absent. O(n) in column
  // count — a linear name scan, acceptable for the small field counts expected
  // (do not "optimize" into a map: schemas carry a handful of columns).
  [[nodiscard]] atx::core::Result<std::span<const atx::f64>>
  column_by_name(std::string_view name) const;

  // Point-in-time universe membership of cell (d, i).
  // Empty mask -> all cells in-universe.
  [[nodiscard]] bool in_universe(atx::usize d, atx::usize i) const noexcept {
    if (mask_.empty()) {
      return true;
    }
    const atx::usize ni = instruments_.size();
    ATX_ASSERT(d < dates_.size());
    ATX_ASSERT(i < ni);
    return mask_[d * ni + i] != 0;
  }

  [[nodiscard]] const DatasetProvenance &provenance() const noexcept { return provenance_; }

  // Raw universe mask as stored (empty span iff the Dataset was built with an
  // empty mask, which means "all cells in-universe"). Expose the stored bytes
  // verbatim so callers (e.g. adapt_panel) can forward the exact representation
  // to Panel::create / with_datafields without re-encoding.
  [[nodiscard]] std::span<const std::uint8_t> mask() const noexcept { return mask_; }

private:
  Dataset() = default; // built only via create()

  DatasetSchema schema_;
  std::vector<DateKey> dates_;
  std::vector<InstKey> instruments_;
  // Owned columns; each inner vector has dates_.size()*instruments_.size() cells.
  // column() returns spans over the inner vectors — stable across Dataset moves
  // (moving a vector preserves its elements' heap buffers).
  std::vector<std::vector<atx::f64>> columns_;
  std::vector<std::uint8_t> mask_; // empty -> all in-universe
  DatasetProvenance provenance_;
};

// ---------------------------------------------------------------------------
//  Free axis utilities
// ---------------------------------------------------------------------------

// True iff `dates` is strictly ascending (each element > previous). An empty or
// single-element span is vacuously ascending. Strict ascent is the precondition
// for as-of binary search (as_of_index) — consumers validate it once at their
// ingestion boundary (catalog register, align_onto).
[[nodiscard]] inline bool is_strictly_ascending(std::span<const DateKey> dates) noexcept {
  for (atx::usize i = 1; i < dates.size(); ++i) {
    if (dates[i] <= dates[i - 1]) {
      return false;
    }
  }
  return true;
}

// Greatest index i with ascending_dates[i] <= canonical_date; nullopt if
// ascending_dates is empty or canonical_date precedes the first date.
//
// Truncation-invariant: appending later dates never changes the result for an
// earlier canonical_date (upper_bound-then-step-back; binary search on a
// prefix has the same answer as on the full span).
[[nodiscard]] inline std::optional<atx::usize> as_of_index(std::span<const DateKey> ascending_dates,
                                                           DateKey canonical_date) noexcept {
  if (ascending_dates.empty()) {
    return std::nullopt;
  }
  // upper_bound returns iterator to first element > canonical_date.
  auto it = std::upper_bound(ascending_dates.begin(), ascending_dates.end(), canonical_date);
  if (it == ascending_dates.begin()) {
    return std::nullopt; // canonical_date precedes the first date
  }
  --it;
  return static_cast<atx::usize>(it - ascending_dates.begin());
}

} // namespace atx::engine::data
