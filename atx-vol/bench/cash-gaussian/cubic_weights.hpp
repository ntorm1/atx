#pragma once

// Private Stage A high-precision control. No production pricing dispatch.
#include <array>
#include <cstddef>
#include <string>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::cash_gaussian {

enum class WeightPrecision { Decimal100, Decimal200 };

struct WeightRequest {
  double lower = 0., upper = 0., stock = 0., dt = 0., sigma = 0., rate = 0., yield = 0.;
};

struct WeightBudget {
  std::size_t max_moments = 4, max_normal_functions = 8;
};

struct WeightWork {
  std::size_t moment_attempts = 0, normal_functions = 0, polynomial_terms = 0;
  unsigned decimal_digits = 0;
};

struct CubicWeights {
  std::array<std::string, 4> value, scaled_moments;
  std::array<std::string, 3> derivative;
  std::string mass, discounted_stock_ratio, maximum_condition;
};

// Gaussian-only finite-cell weights. Decimal strings preserve tiny contributions.
// Resets caller work and counts partial failures. No numerical bound is claimed.
[[nodiscard]] Result<CubicWeights> cubic_weight_control(const WeightRequest &request,
    WeightPrecision precision, const WeightBudget &budget, WeightWork &work);

} // namespace atx::vol::cash_gaussian
