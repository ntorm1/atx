#pragma once

// Explicit Stage A research construction; no production dispatch/default change.
#include "cubic_weights.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace atx::vol::cash_gaussian {

enum class LocalWeightRoute { NotSelected, OriginalMoments, LocalSeries };
enum class LocalWeightReason { None, ZeroLower, WideCell, DensityVariation, OrderCap };

struct LocalWeightBudget {
  WeightBudget moments;
  std::size_t max_selection_checks = 256;
  std::size_t max_coefficient_products = 262144;
  std::size_t max_local_functions = 64;
  std::size_t max_workspace_bytes = 1048576;
};

struct LocalWeightWork {
  WeightWork moments;
  LocalWeightRoute route = LocalWeightRoute::NotSelected;
  LocalWeightReason reason = LocalWeightReason::None;
  std::size_t selection_checks = 0, coefficient_products = 0, local_functions = 0;
  std::size_t workspace_bytes = 0;
  unsigned log_order = 0, exponential_order = 0;
  bool used_log1p = false;
};

struct LocalCubicWeights {
  CubicWeights weights;
  // Real-arithmetic truncation formulas evaluated at fixed precision; these
  // exclude transcendental/rounding and full-price errors. Empty on the labeled
  // original route because no series remainder was constructed.
  std::array<std::string, 4> value_remainders;
  std::array<std::string, 3> derivative_remainders;
  std::string density_remainder, relative_value_remainder, relative_derivative_remainder;
};

struct BernsteinMonomialIntegral {
  std::int64_t numerator = 0;
  std::uint64_t denominator = 1;
};

// Exact rational integral of B_j^degree(u)*(2*u-1)^power on [0,1].
// Only degree2/3 and powers0..513 belong to this bounded private component.
[[nodiscard]] Result<BernsteinMonomialIntegral> local_bernstein_integral(
    unsigned degree, unsigned j, unsigned power);

// Explicit local construction with labeled original-moment nonadmission.
// All caller work resets, failures retain actual work and return no partial weights.
[[nodiscard]] Result<LocalCubicWeights> cubic_local_weight_control(
    const WeightRequest &request, WeightPrecision precision,
    const LocalWeightBudget &budget, LocalWeightWork &work);

} // namespace atx::vol::cash_gaussian
