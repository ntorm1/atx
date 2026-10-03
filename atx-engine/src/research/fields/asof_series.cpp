#include "atx/engine/research/fields/asof_series.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <utility>

namespace atx::engine::research::fields {

core::Result<AsofSeries> AsofSeries::create(const std::vector<AsofObservation> &observations,
                                            usize columns, i64 max_stale_days) {
  if (max_stale_days < 0) {
    return core::Err(core::ErrorCode::InvalidArgument, "as-of series: max_stale_days must be >= 0");
  }
  AsofSeries out;
  out.max_stale_days_ = max_stale_days;
  out.begin_.assign(columns + 1, 0);
  out.days_.reserve(observations.size());
  out.values_.reserve(observations.size());
  for (usize i = 0; i < observations.size(); ++i) {
    const AsofObservation &o = observations[i];
    if (o.column >= columns) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       "as-of series: observation column off the axis");
    }
    if (i > 0) {
      const AsofObservation &p = observations[i - 1];
      if (o.column < p.column || (o.column == p.column && o.available_day <= p.available_day)) {
        return core::Err(
            core::ErrorCode::InvalidArgument,
            "as-of series: observations not ascending and unique by (column, available_day)");
      }
    }
    ++out.begin_[o.column + 1];
    out.days_.push_back(o.available_day);
    out.values_.push_back(o.value);
  }
  for (usize j = 0; j < columns; ++j) {
    out.begin_[j + 1] += out.begin_[j];
  }
  return core::Ok(std::move(out));
}

void AsofSeries::row(i64 day, std::span<f64> values, std::span<i64> source_days) const noexcept {
  assert(values.size() == columns() && source_days.size() == columns());
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  for (usize j = 0; j < values.size(); ++j) {
    const auto first = days_.begin() + static_cast<std::ptrdiff_t>(begin_[j]);
    const auto last = days_.begin() + static_cast<std::ptrdiff_t>(begin_[j + 1]);
    // The first observation available on or after `day`; the one before it is the latest strictly
    // before.
    const auto it = std::lower_bound(first, last, day);
    values[j] = nan;
    source_days[j] = kNoSource;
    if (it == first) {
      continue; // nothing visible yet
    }
    const auto k = static_cast<usize>((it - days_.begin()) - 1);
    if (day - days_[k] > max_stale_days_) {
      continue; // stale
    }
    values[j] = values_[k];
    source_days[j] = days_[k];
  }
}

} // namespace atx::engine::research::fields
