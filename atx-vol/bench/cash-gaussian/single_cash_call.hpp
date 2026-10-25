#pragma once

// Private one-event research builder. No general rollback or production dispatch.
#include "cubic_expectation.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace atx::vol::cash_gaussian {

struct SingleCashCallModel {
  double stock = 100., strike = 100., maturity = 1., sigma = .2;
  double rate = .03, yield = 0., dividend_time = .4, dividend = 5.;
};

struct SingleCashCallBudget {
  CubicOwnerBudget owner;
  std::size_t max_bracket_expansions = 32, max_bisections = 256;
  std::size_t max_knots = 8192, max_endpoint_projections = 65536;
  std::size_t max_workspace_bytes = 64 * 1024 * 1024;
};

struct CashEndpointProjection {
  std::string decimal, residual;
  double projected = 0.;
};

struct CashCallEndpoint {
  CashEndpointProjection stock, value, left_slope, right_slope;
  // At the boundary this differs from the shared intrinsic value explicitly.
  std::string continuation_value, continuation_delta;
};

struct CashRootStep { std::string lower, upper, f_lower, f_upper; };
enum class CashCallRoute { NotSelected, FiniteBoundary, ExerciseAtEvent };
enum class CashCallPhase { Preflight, Boundary, Endpoints, Owner, Allowance, Complete };

struct SingleCashCallWork {
  CashCallRoute route = CashCallRoute::NotSelected;
  CashCallPhase phase = CashCallPhase::Preflight;
  std::size_t bracket_expansions = 0, bisections = 0, black_evaluations = 0;
  std::size_t normal_functions = 0, elementary_functions = 0;
  std::size_t endpoint_projections = 0, constructed_knots = 0, workspace_bytes = 0;
  // Kept on numerical/resource refusal as well as success.
  std::vector<CashRootStep> root_steps;
  CubicOwnerWork owner;
};

struct CashCallAllowances {
  std::string projection_value, projection_delta, root_value, root_delta;
  std::string combined_value, combined_delta;
};

struct SingleCashCallCurve {
  SingleCashCallModel model;
  WeightPrecision precision = WeightPrecision::Decimal100;
  std::size_t continuation_cells = 0;
  StockCubic curve;
  std::vector<CashCallEndpoint> endpoints;
  std::string root_lower, root_upper, boundary_residual;
  double boundary = 0.;
  CashCallAllowances allowances;
};

// Owns all tuples/strings. Reset work; typed failure publishes no partial curve.
// Positive sigma, r>=0, q=0, one interior spot-liquidator event only. D<K needs
// a finite boundary; D>=K is the exact exercise-at-event analytical control.
// Allowances are conditional real-arithmetic/empirical diagnostics, not interval
// certification of special functions or full option interpolation error.
[[nodiscard]] Result<SingleCashCallCurve> build_single_cash_call(
    const SingleCashCallModel &model, std::size_t continuation_cells,
    WeightPrecision precision, const SingleCashCallBudget &budget,
    SingleCashCallWork &work);

} // namespace atx::vol::cash_gaussian
