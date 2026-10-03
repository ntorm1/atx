#include "atx/engine/research/fields/trailing_mean.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <utility>

#include "atx/engine/research/fields/field_stats.hpp"

namespace atx::engine::research::fields {

core::Result<TrailingMean> TrailingMean::create(usize columns, usize window, usize min_count) {
  if (columns == 0 || window == 0 || min_count == 0 || min_count > window) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "trailing mean: needs columns >= 1, window >= 1 and 1 <= min_count <= window");
  }
  TrailingMean out;
  out.columns_ = columns;
  out.window_ = window;
  out.min_count_ = min_count;
  out.ring_.assign(window * columns, 0.0);
  out.seen_.assign(window * columns, u8{0});
  return core::Ok(std::move(out));
}

void TrailingMean::value(std::span<f64> out) const noexcept {
  assert(out.size() == columns_);
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  if (t_ < window_) {
    std::fill(out.begin(), out.end(), nan);
    return;
  }
  for (usize j = 0; j < columns_; ++j) {
    usize accepted = 0;
    for (usize s = 0; s < window_; ++s) {
      accepted += seen_[s * columns_ + j] != 0 ? 1U : 0U;
    }
    f64 sum = 0.0;
    if (columns_ == 1) {
      sum = numpy_pairwise_sum(ring_); // one column: the slots are contiguous, numpy sums pairwise
    } else {
      for (usize s = 0; s < window_; ++s) {
        sum += ring_[s * columns_ +
                     j]; // two or more columns: numpy adds the slots in slot order from 0.0
      }
    }
    out[j] = accepted >= min_count_ ? sum / static_cast<f64>(std::max<usize>(accepted, 1)) : nan;
  }
}

void TrailingMean::push(std::span<const f64> values, std::span<const u8> present) noexcept {
  assert(values.size() == columns_ && present.size() == columns_);
  const usize slot = t_ % window_;
  for (usize j = 0; j < columns_; ++j) {
    const f64 v = values[j];
    const bool accepted = present[j] != 0 && std::isfinite(v) && v >= 0.0;
    ring_[slot * columns_ + j] = accepted ? v : 0.0;
    seen_[slot * columns_ + j] = accepted ? u8{1} : u8{0};
  }
  ++t_;
}

} // namespace atx::engine::research::fields
