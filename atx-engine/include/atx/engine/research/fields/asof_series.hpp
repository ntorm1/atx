#pragma once

// atx::engine::research::fields -- the as-of clock rule of publication-lagged sources (FINRA short
// interest today; any dated release tomorrow). One rule, previously written three times
// (prepare_research_fields.py finra_field, atx-impl asof_field.cpp build_asof_column, engine
// data/finra_short.cpp), now in one place:
//
//   at a session dated `day`, column j shows the value of its LATEST observation with
//   available_day < day (strict: a value published on day d is first visible at the first session
//   after d, so an after-hours publication never reaches that day's session); NaN before the first
//   such observation; NaN when day - available_day > max_stale_days (calendar days); a visible NaN
//   stays NaN (no skip-back to an older value).
//
// Immutable after create(); row() is const and safe to call from several threads.

#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

// One dated observation on the role's id axis.
struct AsofObservation {
  usize column{};
  i64 available_day{};
  f64 value{};
};

class AsofSeries {
public:
  // The source day reported for a cell with nothing visible (none yet, or stale).
  static constexpr i64 kNoSource = std::numeric_limits<i64>::min();

  // `observations` ascending by (column, available_day) and unique on that pair, every column <
  // `columns`; max_stale_days >= 0. Err(InvalidArgument) otherwise.
  [[nodiscard]] static core::Result<AsofSeries>
  create(const std::vector<AsofObservation> &observations, usize columns, i64 max_stale_days);

  // The row of a session dated `day`: `values` and `source_days` (the visible observation's
  // available_day, or kNoSource) each hold columns() elements (precondition, asserted).
  void row(i64 day, std::span<f64> values, std::span<i64> source_days) const noexcept;

  [[nodiscard]] usize columns() const noexcept { return begin_.size() - 1; }

private:
  AsofSeries() = default;

  std::vector<usize> begin_; // CSR: column j's observations are [begin_[j], begin_[j + 1])
  std::vector<i64> days_;
  std::vector<f64> values_;
  i64 max_stale_days_{};
};

} // namespace atx::engine::research::fields
