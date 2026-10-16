#pragma once

#include <cstddef>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

struct AmericanMarkovSinhGridConfig {
  // Exact output count, including zero, spot and the upper boundary.
  std::size_t grid_nodes = 129;
  double upper_spot_multiple = 4.0;
  // Positive dimensionless fraction of spot: x/spot = 1 + scale*sinh(u).
  // A characteristic sigma*sqrt(variance_horizon) is one possible caller choice;
  // this helper does not choose a horizon or infer a volatility from prices.
  double concentration_scale = 0.05;
  // Also limited by kAmericanMarkovMaxStates from american_markov.hpp.
  std::size_t max_states = 1025;
};

struct AmericanMarkovStateGrid {
  std::vector<double> states;
  std::size_t initial_state_index = 0;
  // Spacings are in units of initial spot and describe the represented grid.
  double lower_spot_gap_over_spot = 0.0;
  double upper_spot_gap_over_spot = 0.0;
  double min_gap_over_spot = 0.0;
  // max_i (x_i/(x_i-x_{i-1})) * (x_i/(x_{i+1}-x_i)), interior states only.
  // For constant sigma, sigma^2 times this is the initial-grid diffusion exit
  // rate per variance year. This is a diagnostic, not an outward-rounded bound.
  // Cash events and local volatility change rates; compilation checks them anew.
  double max_unit_volatility_exit_rate = 0.0;
};

// Owns exactly config.grid_nodes finite, strictly increasing nonnegative states,
// including exact 0, spot and spot*upper_spot_multiple. The sinh coordinate is
// uniform separately on either side of spot, with at least one interval per side.
// There are no strike inputs: reuse one grid for the entire common stock process.
//
// InvalidArgument: invalid finite/positive inputs or resource configuration.
// OutOfRange: requested count exceeds max_states, or floating-point range or
// resolution prevents the exact requested grid and finite diagnostics. No nodes
// are dropped, no width is floored, and no compiler budget is modified.
// Allocates O(grid_nodes) owned storage; allocation failure may throw bad_alloc.
// Reentrant; no shared state. Callers must keep states alive until compilation.
//
// Concentration can greatly increase CFL work. This helper does not certify
// continuous-American, tail, or exercise-time accuracy, nor sufficient model
// budgets. The existing compiler retains every positivity and resource check.
[[nodiscard]] Result<AmericanMarkovStateGrid>
make_american_markov_sinh_grid(double spot, const AmericanMarkovSinhGridConfig &config = {});

} // namespace atx::vol
