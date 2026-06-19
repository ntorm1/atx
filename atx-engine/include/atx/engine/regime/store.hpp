#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::regime {

// Read-only view over a sealed regime segment (one synthetic instrument
// "MACRO"; fields are series names; the time axis is the master date axis).
class RegimeStore {
public:
  [[nodiscard]] static atx::core::Result<RegimeStore> open(const std::string &seg_path);

  [[nodiscard]] std::span<const std::string> series_names() const noexcept { return names_; }
  [[nodiscard]] std::span<const atx::i64> date_axis() const noexcept { return axis_; }

  // As-of (<=) lookup: value of `series` at the latest axis date <= t_nanos.
  // NaN if `series` is unknown or t_nanos precedes the first axis date.
  [[nodiscard]] atx::f64 value(std::string_view series, atx::i64 t_nanos) const noexcept;

private:
  std::vector<std::string> names_;     // field/series names (field index order)
  std::vector<atx::i64> axis_;         // ascending master dates
  std::vector<std::vector<atx::f64>> cols_;  // [series][date] dense f64 (NaN-filled)
};

}  // namespace atx::engine::regime
