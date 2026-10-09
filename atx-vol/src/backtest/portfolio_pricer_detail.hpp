#pragma once

// PortfolioPricer internals split out of the public portfolio_pricer.hpp API
// surface (Task 6, atx-vol API restructure): portfolio_index_count_is_representable
// is used only by portfolio_pricer.cpp's own TU and by portfolio_pricer_test.cpp's
// direct overflow-boundary unit test, never by any other production TU.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace atx::vol::detail {

// Checked-narrowing seam shared by Portfolio::create and its boundary test. The
// prepared substrate stores position-to-contract and execution indices as
// uint32_t, so no allocation may begin for an unrepresentable position count.
[[nodiscard]] constexpr bool portfolio_index_count_is_representable(std::size_t count) noexcept {
  return count <= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
}

} // namespace atx::vol::detail
