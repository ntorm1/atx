#pragma once

#include "stock_cubic.hpp"

#include <string>

namespace atx::vol::cash_gaussian::detail {
// Private deterministic transport helper. The decimal point is never rounded
// through binary64. Nonidentity transport refuses uncertain knot locations.
[[nodiscard]] Result<CubicPoint> cubic_decimal_point(const StockCubic &curve,
    const std::string &stock, WeightPrecision precision,
    const CubicOwnerBudget &budget, CubicOwnerWork &work);
} // namespace atx::vol::cash_gaussian::detail
