#include "semigroup.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>
#include <vector>

namespace atx::vol::cash_gaussian {
Result<double> repeat_expectation(StockPwl value, std::span<const double> grid,
    double stock, double dt, std::size_t steps, double sigma, double rate, double yield,
    Budget &budget, SemigroupWork &work) {
  using atx::core::Err;
  work = SemigroupWork{};
  if (value.knots().empty() || grid.size() < 2 || grid.front() != 0. ||
      !std::isfinite(stock) || stock < 0. || !std::isfinite(dt) || dt < 0. ||
      !std::isfinite(sigma) || sigma < 0. || !std::isfinite(rate) || !std::isfinite(yield) ||
      steps == 0 || steps > 4096)
    return Err(ErrorCode::InvalidArgument, "semigroup input");
  if (grid.size() > 8192 || grid.size() > budget.limits.max_knots)
    return Err(ErrorCode::Unavailable, "semigroup grid budget");
  for (std::size_t i = 0; i < grid.size(); ++i)
    if (!std::isfinite(grid[i]) || (i > 0 && !(grid[i] > grid[i-1])))
      return Err(ErrorCode::InvalidArgument, "semigroup grid");
  if (!std::binary_search(grid.begin(), grid.end(), stock))
    return Err(ErrorCode::InvalidArgument, "semigroup stock anchor");
  work.grid_nodes = grid.size();
  try {
    // grid may borrow value.knots(): the first value replacement destroys that
    // storage. Own a bounded copy before any mutation, including for this alias.
    const std::vector<double> stable_grid(grid.begin(), grid.end());
    std::vector<double> values;
    values.reserve(stable_grid.size());
    for (std::size_t step = 0; step < steps; ++step) {
      values.clear();
      for (const double x : stable_grid) {
        const auto result = expectation(value, x, dt, sigma, rate, yield, budget);
        if (!result) return Err(result.error());
        values.push_back(*result);
      }
      const double slope = value.right_slope()*std::exp(-yield*dt);
      if (!std::isfinite(slope)) return Err(ErrorCode::OutOfRange, "semigroup tail");
      auto result = StockPwl::create(stable_grid, values, slope, budget);
      if (!result) return Err(result.error());
      value = std::move(*result);
      ++work.completed_steps;
    }
    return value.value(stock);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "semigroup allocation");
  }
}
} // namespace atx::vol::cash_gaussian
