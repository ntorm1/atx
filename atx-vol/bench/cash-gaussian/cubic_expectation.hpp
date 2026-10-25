#pragma once

// Private Stage A full represented-curve expectation. No option rollback.
#include "stock_cubic.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace atx::vol::cash_gaussian {

struct CubicExpectationRequest {
  double stock = 0., dt = 0., sigma = 0., rate = 0., yield = 0.;
};

struct CubicExpectationBudget {
  CubicOwnerBudget owner;
  std::size_t max_selection_checks = 262144;
  std::size_t max_coefficient_products = 262144;
  std::size_t max_local_functions = 262144;
  std::size_t max_normal_functions = 524288;
  std::size_t max_conversions = 1048576;
  std::size_t max_coefficient_terms = 524288;
  std::size_t max_accumulation_terms = 524288;
  std::size_t max_elementary_functions = 64;
  std::size_t max_tail_moments = 2;
};

enum class CubicExpectationRoute { NotSelected, ZeroTime, ZeroStock, ZeroVolatility, Stochastic };
enum class CubicExpectationPhase { None, Preflight, Point, FiniteCell, Tail, Output };

struct CubicTailLedger {
  std::array<std::string, 2> moments, thresholds, prefactor_exponents;
  std::string excess, excess_absolute_sum, excess_condition;
  std::string value_absolute_sum, delta_absolute_sum;
};

struct CubicExpectationWork {
  CubicExpectationRoute route = CubicExpectationRoute::NotSelected;
  CubicExpectationPhase phase = CubicExpectationPhase::None;
  std::size_t component_index = 0;
  std::size_t finite_attempts = 0, finite_completions = 0;
  std::size_t tail_attempts = 0, tail_completions = 0, tail_moment_attempts = 0;
  std::size_t moment_attempts = 0, normal_functions = 0, polynomial_terms = 0;
  std::size_t selection_checks = 0, coefficient_products = 0, local_functions = 0;
  std::size_t conversions = 0, coefficient_terms = 0, accumulation_terms = 0;
  std::size_t elementary_functions = 0;
  std::size_t exact_proof_attempts = 0, exact_zeros = 0, exact_proof_refusals = 0;
  // Aggregate operation counters only; input bits/status live in last_cell.
  DyadicZeroWork dyadic;
  std::size_t workspace_bytes = 0, endpoint_projection_count = 0;
  unsigned decimal_digits = 0;
  CubicOwnerWork last_cell;
  CubicTailLedger tail;
};

struct CubicExpectation {
  std::string value, left_delta, right_delta;
  CubicSlopeKind slope_kind = CubicSlopeKind::Smooth;
  std::string value_absolute_sum, delta_absolute_sum, maximum_condition;
};

// Resets work; no partial result on refusal. Full infinite tail, no cutoffs.
// Exact zero with zero absolute-term sum is valid. Unresolved cancellation of
// nonzero terms refuses. No binary64 endpoint projection or general error bound.
[[nodiscard]] Result<CubicExpectation> cubic_expectation(const StockCubic &curve,
    const CubicExpectationRequest &request, WeightPrecision precision,
    const CubicExpectationBudget &budget, CubicExpectationWork &work);

} // namespace atx::vol::cash_gaussian
