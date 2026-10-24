#include "atx/vol/api/pricing/american_markov_grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "atx/vol/api/pricing/american_markov.hpp"

namespace atx::vol {
namespace {
using atx::core::Err;
using atx::core::Ok;

bool positive(double value) noexcept { return value > 0.0 && std::isfinite(value); }

Status diagnose(AmericanMarkovStateGrid &grid, double spot) {
  grid.min_gap_over_spot = std::numeric_limits<double>::infinity();
  for (std::size_t i = 1; i < grid.states.size(); ++i) {
    const double state = grid.states[i];
    const double gap = state - grid.states[i - 1];
    const double relative_gap = gap / spot;
    if (!positive(state) || !positive(gap) || !positive(relative_gap)) {
      return Err(ErrorCode::OutOfRange,
                 "American Markov grid: state spacing loses floating-point resolution");
    }
    grid.min_gap_over_spot = std::min(grid.min_gap_over_spot, relative_gap);
    if (i + 1 < grid.states.size()) {
      const double next_gap = grid.states[i + 1] - state;
      if (!positive(next_gap)) {
        return Err(ErrorCode::OutOfRange,
                   "American Markov grid: state spacing loses floating-point resolution");
      }
      const double rate = (state / gap) * (state / next_gap);
      if (!positive(rate)) {
        return Err(ErrorCode::OutOfRange,
                   "American Markov grid: unit volatility exit rate loses numerical range");
      }
      grid.max_unit_volatility_exit_rate = std::max(grid.max_unit_volatility_exit_rate, rate);
    }
  }
  const std::size_t index = grid.initial_state_index;
  grid.lower_spot_gap_over_spot = (spot - grid.states[index - 1]) / spot;
  grid.upper_spot_gap_over_spot = (grid.states[index + 1] - spot) / spot;
  return Ok();
}
} // namespace

Result<AmericanMarkovStateGrid>
make_american_markov_sinh_grid(double spot, const AmericanMarkovSinhGridConfig &config) {
  if (!positive(spot) || !positive(config.concentration_scale) ||
      !positive(config.upper_spot_multiple) || config.upper_spot_multiple <= 1.0 ||
      config.grid_nodes < 3 || config.max_states < 3 ||
      config.max_states > kAmericanMarkovMaxStates) {
    return Err(ErrorCode::InvalidArgument, "American Markov grid: invalid grid configuration");
  }
  if (config.grid_nodes > config.max_states) {
    return Err(ErrorCode::OutOfRange, "American Markov grid: requested state limit exceeded");
  }
  const double upper = spot * config.upper_spot_multiple;
  const double left_span = std::asinh(1.0 / config.concentration_scale);
  const double right_span =
      std::asinh((config.upper_spot_multiple - 1.0) / config.concentration_scale);
  if (!positive(upper) || !(upper > spot) || !positive(left_span) ||
      !positive(right_span)) {
    return Err(ErrorCode::OutOfRange, "American Markov grid: transformation loses numerical range");
  }
  const std::size_t intervals = config.grid_nodes - 1;
  const double allocation = static_cast<double>(intervals) *
                            (left_span / (left_span + right_span));
  // Finite positive spans bound allocation by intervals <= the hard state cap;
  // the clamp reserves one interval on each side without changing the node count.
  const auto left = std::clamp(static_cast<std::size_t>(std::floor(allocation + 0.5)),
                               std::size_t{1}, intervals - 1);
  const std::size_t right = intervals - left;
  AmericanMarkovStateGrid grid;
  grid.states.resize(config.grid_nodes);
  grid.initial_state_index = left;
  grid.states.front() = 0.0;
  grid.states[left] = spot;
  grid.states.back() = upper;
  for (std::size_t i = 1; i < left; ++i) {
    const double coordinate = left_span *
                              (static_cast<double>(left - i) / static_cast<double>(left));
    grid.states[i] = spot * (1.0 - config.concentration_scale * std::sinh(coordinate));
  }
  for (std::size_t i = 1; i < right; ++i) {
    const double coordinate = right_span *
                              (static_cast<double>(i) / static_cast<double>(right));
    grid.states[left + i] = spot * (1.0 + config.concentration_scale * std::sinh(coordinate));
  }
  const auto valid = diagnose(grid, spot);
  if (!valid) {
    return Err(valid.error());
  }
  return grid;
}

} // namespace atx::vol
