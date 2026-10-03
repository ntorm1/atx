#pragma once

// atx::engine::research::fields -- the trailing-window mean rule (research_fields_price.py
// volume_mean_rows; field vol_126 = TrailingMean(n, 126, 63) over the role's volume.f64 and
// present.u8).
//
// Row t is the mean, per column, of the accepted observations of sessions t-window..t-1 (accepted:
// present != 0, finite and >= 0); NaN while t < window or when fewer than min_count observations
// were accepted. The caller asks for row t (value) BEFORE it pushes session t, so no row can read
// its own session: the clock is role-close-lag1.
//
// Bit identity with the Python: the window is a ring of `window` slots indexed by t % window
// (non-accepted slots hold 0.0), and a column's sum is numpy's ring.sum(axis=0) of that (window x
// columns) C-order array: with two or more columns numpy adds the slots in slot order 0..window-1
// from 0.0; with one column the slots are contiguous and numpy uses its pairwise sum
// (field_stats.hpp). The mean is sum / max(accepted, 1).
//
// State: window x columns f64 + bytes, allocated once in create(); value() and push() allocate
// nothing.

#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

class TrailingMean {
public:
  // columns >= 1, window >= 1, 1 <= min_count <= window; Err(InvalidArgument) otherwise.
  [[nodiscard]] static core::Result<TrailingMean> create(usize columns, usize window,
                                                         usize min_count);

  // Row t = sessions() into `out` (columns() values; precondition, asserted).
  void value(std::span<f64> out) const noexcept;
  // Session t's observations (columns() values each; precondition, asserted); advances t.
  void push(std::span<const f64> values, std::span<const u8> present) noexcept;

  [[nodiscard]] usize columns() const noexcept { return columns_; }
  [[nodiscard]] usize sessions() const noexcept { return t_; }

private:
  TrailingMean() = default;

  usize columns_{};
  usize window_{};
  usize min_count_{};
  usize t_{};
  std::vector<f64> ring_; // slot-major: slot s, column j at s * columns_ + j
  std::vector<u8> seen_;
};

} // namespace atx::engine::research::fields
