#include "single_cash_call.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

#include <boost/math/special_functions/erf.hpp>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include <gtest/gtest.h>

namespace atx::vol::cash_gaussian {
namespace {
using R = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<200>>;
constexpr std::array precisions{WeightPrecision::Decimal100, WeightPrecision::Decimal200};
R cdf(const R &z) { return boost::math::erfc(-z/sqrt(R(2)))/2; }

TEST(SingleCashCall, BuildsOwnedContinuousBoundaryWithProjectedEndpoints) {
  for (const auto precision : precisions) {
    SingleCashCallWork work;
    auto built = build_single_cash_call({}, 4, precision, {}, work);
    ASSERT_TRUE(built) << built.error().message();
    EXPECT_EQ(work.route, CashCallRoute::FiniteBoundary);
    EXPECT_EQ(work.phase, CashCallPhase::Complete);
    ASSERT_EQ(built->curve.cells().size(), 5U);
    ASSERT_EQ(built->endpoints.size(), 6U);
    EXPECT_GT(built->boundary, 100.);
    EXPECT_LE(R(built->boundary_residual), 0);
    EXPECT_LE(R(built->root_upper)-R(built->root_lower),
              ldexp(R(built->root_upper), -192));
    const auto nodes = built->curve.cells();
    for (std::size_t i = 1; i < nodes.size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(nodes[i-1].value_upper),
                std::bit_cast<std::uint64_t>(nodes[i].value_lower));
    }
    EXPECT_EQ(nodes.back().value_upper, built->curve.tail().value);
    EXPECT_EQ(built->curve.tail().slope, 1.);
    EXPECT_LT(nodes.back().slope_upper, 1.);
    EXPECT_LE(R(built->allowances.combined_value), R("1e-8"));
    EXPECT_LE(R(built->allowances.combined_delta), R("1e-8"));
  }
}

TEST(SingleCashCall, CashAtLeastStrikeReducesToEarlierEuropeanCall) {
  SingleCashCallModel model;
  model.dividend = 110.;
  for (const auto precision : precisions) {
    SingleCashCallWork work;
    auto built = build_single_cash_call(model, 4, precision, {}, work);
    ASSERT_TRUE(built) << built.error().message();
    EXPECT_EQ(work.route, CashCallRoute::ExerciseAtEvent);
    EXPECT_EQ(work.black_evaluations, 0U);
    CubicExpectationWork ew;
    auto answer = cubic_expectation(built->curve,
        {model.stock, model.dividend_time, model.sigma, model.rate, 0.}, precision, {}, ew);
    ASSERT_TRUE(answer) << answer.error().message();
    const R s(model.stock), k(model.strike), t(model.dividend_time), r(model.rate);
    const R a = R(model.sigma)*sqrt(t), d1 = (log(s/k)+r*t)/a+a/2;
    const R expected = s*cdf(d1)-k*exp(-r*t)*cdf(d1-a);
    EXPECT_LT(abs(R(answer->value)-expected), R("1e-40"));
    EXPECT_LT(abs(R(answer->right_delta)-cdf(d1)), R("1e-40"));
  }
}

TEST(SingleCashCall, EndpointValuesSlopesAndResidualsDescribeActualTuples) {
  SingleCashCallWork work;
  auto built = build_single_cash_call({}, 4, WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(built);
  const auto &m = built->model;
  const R tau = R(m.maturity)-R(m.dividend_time), a = R(m.sigma)*sqrt(tau);
  for (std::size_t i = 2; i < built->endpoints.size(); ++i) {
    const auto &node = built->endpoints[i];
    const R y = R(node.stock.projected)-R(m.dividend);
    const R d1 = (log(y/R(m.strike))+R(m.rate)*tau)/a+a/2;
    const R value = y*cdf(d1)-R(m.strike)*exp(-R(m.rate)*tau)*cdf(d1-a);
    EXPECT_LT(abs(value-R(node.continuation_value)), R("1e-80"));
    EXPECT_LT(abs(cdf(d1)-R(node.continuation_delta)), R("1e-80"));
    for (const auto *const p : {&node.stock, &node.value, &node.left_slope,
                               &node.right_slope}) {
      EXPECT_LT(abs((R(p->projected)-R(p->decimal))-R(p->residual)), R("1e-80"));
    }
  }
  EXPECT_EQ(work.endpoint_projections, 3*built->endpoints.size()+1);
  EXPECT_EQ(work.root_steps.size(), 1+work.bracket_expansions+work.bisections);
  for (const auto &step : work.root_steps) {
    EXPECT_GT(R(step.f_lower), 0);
    EXPECT_LT(R(step.lower), R(step.upper));
  }
  EXPECT_GT(R(built->allowances.root_delta), 0);
}

TEST(SingleCashCall, KinkAndZeroStateRetainActualMeaning) {
  SingleCashCallWork work;
  auto built = build_single_cash_call({}, 4, WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(built);
  CubicOwnerWork ow;
  auto point = cubic_point(built->curve, built->boundary, WeightPrecision::Decimal100, {}, ow);
  ASSERT_TRUE(point);
  EXPECT_EQ(point->slope_kind, CubicSlopeKind::Kink);
  EXPECT_LT(R(point->left_derivative), R(point->right_derivative));
  EXPECT_EQ(R(point->right_derivative), 1);
  CubicExpectationWork ew;
  auto zero = cubic_expectation(built->curve, {0., .4, .2, .03, 0.},
                               WeightPrecision::Decimal100, {}, ew);
  ASSERT_TRUE(zero);
  EXPECT_EQ(R(zero->value), 0);
  EXPECT_EQ(R(zero->right_delta), 0);
  EXPECT_EQ(zero->slope_kind, CubicSlopeKind::RightLimit);
}

TEST(SingleCashCall, UnsupportedModelsRefuseAndResetPriorWork) {
  const SingleCashCallModel base;
  std::array<SingleCashCallModel, 8> bad;
  bad.fill(base);
  bad[0].yield = .01; bad[1].rate = -.01; bad[2].sigma = 0;
  bad[3].dividend_time = 0; bad[4].dividend_time = base.maturity;
  bad[5].dividend = 0; bad[6].strike = 0;
  bad[7].stock = std::numeric_limits<double>::infinity();
  SingleCashCallWork work;
  for (const auto &model : bad) {
    work.black_evaluations = 100;
    auto result = build_single_cash_call(model, 4, WeightPrecision::Decimal100, {}, work);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(work.black_evaluations, 0U);
    EXPECT_EQ(work.workspace_bytes, 0U);
  }
  auto no_boundary = base;
  no_boundary.dividend = .01;
  auto result = build_single_cash_call(no_boundary, 4, WeightPrecision::Decimal100, {}, work);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(work.black_evaluations, 0U);
}

TEST(SingleCashCall, SetupBudgetsRefuseBeforeLocalArithmetic) {
  std::array<SingleCashCallBudget, 3> budgets;
  budgets[0].max_knots = 5;
  budgets[1].max_workspace_bytes = 0;
  budgets[2].max_endpoint_projections = 18;
  for (const auto &budget : budgets) {
    SingleCashCallWork work;
    auto result = build_single_cash_call({}, 4, WeightPrecision::Decimal100, budget, work);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Unavailable);
    EXPECT_EQ(work.black_evaluations, 0U);
    EXPECT_EQ(work.elementary_functions, 0U);
    EXPECT_EQ(work.endpoint_projections, 0U);
    EXPECT_TRUE(work.root_steps.empty());
  }
}

TEST(SingleCashCall, BisectionBudgetRetainsActualPartialRootEvidence) {
  SingleCashCallBudget budget;
  budget.max_bisections = 2;
  SingleCashCallWork work;
  auto result = build_single_cash_call({}, 4, WeightPrecision::Decimal100, budget, work);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::Unavailable);
  EXPECT_EQ(work.phase, CashCallPhase::Boundary);
  EXPECT_EQ(work.bisections, 2U);
  EXPECT_EQ(work.root_steps.size(), 1+work.bracket_expansions+work.bisections);
  EXPECT_EQ(work.black_evaluations, 2+work.bracket_expansions+work.bisections);
  EXPECT_EQ(work.constructed_knots, 0U);
  EXPECT_GT(work.workspace_bytes, 0U);
}

TEST(SingleCashCall, ExactExerciseRouteDoesNotEvaluateIrrelevantPostEventBlackInputs) {
  SingleCashCallModel model;
  model.dividend = 110.;
  model.rate = 1e308;
  model.dividend_time = 1e-308;
  SingleCashCallWork work;
  auto result = build_single_cash_call(model, 4, WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(result) << result.error().message();
  EXPECT_EQ(work.black_evaluations, 0U);
  EXPECT_EQ(work.normal_functions, 0U);
  EXPECT_EQ(work.elementary_functions, 1U); // Only actual pre-event discount.
  EXPECT_EQ(R(result->allowances.combined_value), 0);
  EXPECT_EQ(R(result->allowances.combined_delta), 0);
}
} // namespace
} // namespace atx::vol::cash_gaussian
