#pragma once

// scenario_grid internals split out of the public scenario_grid.hpp API surface
// (Task 6, atx-vol API restructure): scenario_grid_product_is_representable is
// used only by scenario_grid.cpp's own TU and by scenario_grid_test.cpp's
// direct overflow-boundary unit test, never by any other production TU.

#include <cstddef>
#include <limits>

namespace atx::vol::detail {

// Allocation-free sizing seam used by scenario_grid for every multiplied shape
// (result cells, compact exact-price lanes, and executor tasks). Public only so
// overflow boundaries can be pinned without attempting pathological allocations.
[[nodiscard]] constexpr bool scenario_grid_product_is_representable(std::size_t lhs,
                                                                    std::size_t rhs) noexcept {
  return lhs == 0u || rhs <= (std::numeric_limits<std::size_t>::max)() / lhs;
}

} // namespace atx::vol::detail
