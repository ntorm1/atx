#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::detail {

[[nodiscard]] inline bool is_mark_coverage_name(std::string_view name) noexcept {
  return name == "n_priced_marks" || name == "n_greeks_unavailable";
}

// Both empty means unknown legacy coverage. Known observations are a paired,
// row-parallel set of exact nonnegative position counts, with partial <= priced.
// Span inputs also permit validation of mapped optional storage columns.
[[nodiscard]] inline Status validate_mark_coverage(std::span<const double> priced,
                                                  std::span<const double> partial,
                                                  std::size_t rows) {
  if (priced.empty() && partial.empty()) {
    return core::Ok();
  }
  if (priced.size() != rows || partial.size() != rows) {
    return core::Err(ErrorCode::InvalidArgument,
                     "BacktestResult: mark coverage must be paired and row-parallel");
  }
  constexpr double max_count = static_cast<double>((std::numeric_limits<std::uint32_t>::max)());
  static_assert(max_count <= 9007199254740992.); // Every accepted integer is exact in F64.
  for (std::size_t i = 0; i < rows; ++i) {
    const double count = priced[i], refused = partial[i];
    if (!std::isfinite(count) || count < 0. || count > max_count || count != std::floor(count) ||
        !std::isfinite(refused) || refused < 0. || refused > count ||
        refused != std::floor(refused)) {
      return core::Err(ErrorCode::InvalidArgument,
                       "BacktestResult: mark coverage requires integer 0 <= partial <= priced");
    }
  }
  return core::Ok();
}

} // namespace atx::vol::detail
