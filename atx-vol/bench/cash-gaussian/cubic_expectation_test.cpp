#include "cubic_expectation.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include <boost/math/special_functions/erf.hpp>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include <gtest/gtest.h>

namespace atx::vol::cash_gaussian {
namespace {
using R = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<200>>;
constexpr std::array precisions{WeightPrecision::Decimal100, WeightPrecision::Decimal200};

void close(const std::string &actual, const R &expected) {
  if (expected == 0) EXPECT_EQ(R(actual), 0);
  else EXPECT_LT(abs(R(actual)-expected)/abs(expected), R("1e-40"));
}

R phi(const R &z) { return boost::math::erfc(-z/sqrt(R(2)))/2; }

TEST(CashCubicExpectation, EmptyAffineFullMomentsAndDelta) {
  CubicOwnerWork ow;
  for (const auto tail : {CubicAffineTail{0., 2., 3.}, {0., -2., -3.}, {0., 5., 0.},
                          {0., 0., 0.}}) {
    auto curve = StockCubic::make({}, tail, {}, ow);
    ASSERT_TRUE(curve);
    for (const auto precision : precisions) {
      CubicExpectationWork work;
      const CubicExpectationRequest r{100., .75, .2, -.03, .01};
      auto result = cubic_expectation(*curve, r, precision, {}, work);
      ASSERT_TRUE(result) << result.error().message();
      close(result->value, R(tail.value)*exp(-R(r.rate)*R(r.dt)) +
          R(tail.slope)*R(r.stock)*exp(-R(r.yield)*R(r.dt)));
      close(result->right_delta, R(tail.slope)*exp(-R(r.yield)*R(r.dt)));
      EXPECT_EQ(result->left_delta, result->right_delta);
      EXPECT_EQ(work.finite_attempts, 0U);
      EXPECT_EQ(work.tail_completions, 1U);
      EXPECT_EQ(work.tail_moment_attempts, 2U);
      EXPECT_EQ(work.normal_functions, 0U);
      EXPECT_EQ(work.endpoint_projection_count, 0U);
    }
  }
}

TEST(CashCubicExpectation, PartitionedAffineVisitsEveryCellAndTail) {
  const std::array cells{HermiteCell{0., 90., 2., 92., 1., 1.},
      HermiteCell{90., 110., 92., 112., 1., 1.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {110., 112., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  for (const auto precision : precisions) {
    const CubicExpectationRequest r{100., .5, .2, .03, -.01};
    CubicExpectationWork work;
    auto result = cubic_expectation(*curve, r, precision, {}, work);
    ASSERT_TRUE(result) << result.error().message();
    close(result->value, 2*exp(-R(r.rate)*R(r.dt))+R(r.stock)*exp(-R(r.yield)*R(r.dt)));
    close(result->right_delta, exp(-R(r.yield)*R(r.dt)));
    EXPECT_EQ(work.finite_attempts, 2U);
    EXPECT_EQ(work.finite_completions, 2U);
    EXPECT_EQ(work.tail_completions, 1U);
    EXPECT_EQ(work.exact_proof_attempts, 2U);
    EXPECT_GT(R(work.tail.excess), 0);
    EXPECT_LE(work.last_cell.conversions+5U, 128U);
    EXPECT_LE(work.last_cell.coefficient_terms, 16U);
    EXPECT_LE(work.last_cell.accumulation_terms+4U, 16U);
  }
}

TEST(CashCubicExpectation, ContinuousKinkMatchesIndependentBlackValueAndDelta) {
  const std::array cells{HermiteCell{0., 100., 0., 0., 0., 0.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {100., 0., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  const CubicExpectationRequest r{103., .7, .25, .03, .01};
  const R t(r.dt), sigma(r.sigma), s(r.stock), rate(r.rate), q(r.yield);
  const R d1=(log(s/100)+(rate-q+sigma*sigma/2)*t)/(sigma*sqrt(t));
  const R d2=d1-sigma*sqrt(t);
  for (const auto precision : precisions) {
    CubicExpectationWork work;
    auto result = cubic_expectation(*curve, r, precision, {}, work);
    ASSERT_TRUE(result) << result.error().message();
    close(result->value, s*exp(-q*t)*phi(d1)-100*exp(-rate*t)*phi(d2));
    close(result->right_delta, exp(-q*t)*phi(d1));
    EXPECT_EQ(result->slope_kind, CubicSlopeKind::Smooth);
  }
}

TEST(CashCubicExpectation, ActualLimitsRetainKinkAndZeroRightDerivative) {
  const std::array cells{HermiteCell{0., 1., 0., 0., 0., 0.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {1., 0., 2.}, {}, ow);
  ASSERT_TRUE(curve);
  for (const auto precision : precisions) {
    CubicExpectationWork work;
    auto zero_time = cubic_expectation(*curve, {1., 0., .2, .03, .01}, precision, {}, work);
    ASSERT_TRUE(zero_time);
    EXPECT_EQ(zero_time->slope_kind, CubicSlopeKind::Kink);
    close(zero_time->left_delta, R(0)); close(zero_time->right_delta, R(2));
    EXPECT_EQ(work.route, CubicExpectationRoute::ZeroTime);
    auto deterministic = cubic_expectation(*curve, {1., .5, 0., .03, .03}, precision, {}, work);
    ASSERT_TRUE(deterministic);
    EXPECT_EQ(deterministic->slope_kind, CubicSlopeKind::Kink);
    close(deterministic->right_delta, 2*exp(-R(.03)*R(.5)));
    auto zero_stock = cubic_expectation(*curve, {0., .5, .2, .03, .01}, precision, {}, work);
    ASSERT_TRUE(zero_stock);
    EXPECT_EQ(zero_stock->slope_kind, CubicSlopeKind::RightLimit);
    EXPECT_TRUE(zero_stock->left_delta.empty());
    close(zero_stock->value, R(0));
  }
}

TEST(CashCubicExpectation, TinyPositiveVarianceIsNotDeterministic) {
  CubicOwnerWork ow;
  auto curve = StockCubic::make({}, {0., 0., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  CubicExpectationWork work;
  const CubicExpectationRequest r{1., 1e-300, 1e-150, .03, .01};
  auto result = cubic_expectation(*curve, r, WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(result) << result.error().message();
  EXPECT_EQ(work.route, CubicExpectationRoute::Stochastic);
  close(result->value, exp(-R(r.yield)*R(r.dt)));
}

TEST(CashCubicExpectation, NonzeroDriftUncertainKnotRefuses) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 1., 1.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {1., 1., 2.}, {}, ow);
  ASSERT_TRUE(curve);
  CubicExpectationWork work;
  const auto result = cubic_expectation(*curve, {1., 1e-300, 0., .03, .01},
      WeightPrecision::Decimal100, {}, work);
  EXPECT_FALSE(result);
  EXPECT_EQ(work.route, CubicExpectationRoute::ZeroVolatility);
  EXPECT_GT(work.elementary_functions, 0U);
}

TEST(CashCubicExpectation, WorkspaceAndWholeCallBudgetRefuseAndReset) {
  const std::array cells{HermiteCell{0., 90., 0., 90., 1., 1.},
      HermiteCell{90., 110., 90., 110., 1., 1.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {110., 110., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  CubicExpectationWork work;
  CubicExpectationBudget budget;
  budget.owner.max_workspace_bytes = 0;
  EXPECT_FALSE(cubic_expectation(*curve, {100., .5, .2, .03, .01},
      WeightPrecision::Decimal100, budget, work));
  EXPECT_EQ(work.conversions, 0U);
  EXPECT_EQ(work.finite_attempts, 0U);
  budget = {}; budget.owner.max_finite_cells = 1;
  EXPECT_FALSE(cubic_expectation(*curve, {100., .5, .2, .03, .01},
      WeightPrecision::Decimal100, budget, work));
  EXPECT_EQ(work.finite_completions, 1U);
  EXPECT_EQ(work.tail_attempts, 0U);
  auto result = cubic_expectation(*curve, {100., .5, .2, .03, .01},
      WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(result);
  EXPECT_EQ(work.finite_completions, 2U);
}

TEST(CashCubicExpectation, CancellingNonzeroGlobalOutputRefuses) {
  CubicOwnerWork ow;
  auto curve = StockCubic::make({}, {0., -1., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  CubicExpectationWork work;
  EXPECT_FALSE(cubic_expectation(*curve, {1., .5, .2, 0., 0.},
      WeightPrecision::Decimal100, {}, work));
  EXPECT_EQ(work.tail_attempts, 1U);
}

TEST(CashCubicExpectation, FiniteCubicWithAffineTailUsesIndependentTruncatedMoments) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 0., 3.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {1., 1., 3.}, {}, ow);
  ASSERT_TRUE(curve);
  const CubicExpectationRequest r{.8, .6, .25, -.02, .01};
  const R s(r.stock), t(r.dt), sigma(r.sigma), rate(r.rate), q(r.yield);
  const R sd = sigma*sqrt(t);
  auto below = [&](unsigned k) -> R {
    const R n(k), total = pow(s, k)*exp((n*(rate-q)+n*(n-1)*sigma*sigma/2-rate)*t);
    return total*phi((-log(s)-(rate-q+(n-R(".5"))*sigma*sigma)*t)/sd);
  };
  const R m0_above=exp(-rate*t)-below(0), m1_above=s*exp(-q*t)-below(1);
  const R expected=below(3)+3*m1_above-2*m0_above;
  const R derivative=3*(below(3)+m1_above)/s;
  for (const auto precision : precisions) {
    CubicExpectationWork work;
    auto result = cubic_expectation(*curve, r, precision, {}, work);
    ASSERT_TRUE(result) << result.error().message();
    close(result->value, expected); close(result->right_delta, derivative);
    EXPECT_EQ(work.exact_zeros, 1U);
    EXPECT_EQ(work.finite_completions, 1U);
    EXPECT_EQ(work.tail_completions, 1U);
  }
}

TEST(CashCubicExpectation, AdjacentHighLowUnitCellsPreserveAffineFullExpectation) {
  for (const double a : {1e12, 1e-12, 1.}) {
    const double b=std::nextafter(a, std::numeric_limits<double>::infinity());
    const std::array cells{HermiteCell{0., a, 0., a, 1., 1.},
        HermiteCell{a, b, a, b, 1., 1.}};
    CubicOwnerWork ow;
    auto curve = StockCubic::make(cells, {b, b, 1.}, {}, ow);
    ASSERT_TRUE(curve);
    for (const auto precision : precisions) {
      CubicExpectationWork work;
      const CubicExpectationRequest r{a, .5, .2, .03, .01};
      auto result = cubic_expectation(*curve, r, precision, {}, work);
      ASSERT_TRUE(result) << result.error().message();
      close(result->value, R(a)*exp(-R(.01)*R(.5)));
      close(result->right_delta, exp(-R(.01)*R(.5)));
      EXPECT_EQ(work.finite_completions, 2U);
      EXPECT_GT(work.coefficient_products, 0U);
    }
  }
}

TEST(CashCubicExpectation, DeterministicInteriorCubicAndZeroStockUseAnalyticDelta) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 0., 3.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {1., 1., 3.}, {}, ow);
  auto affine = StockCubic::make({}, {0., 2., -3.}, {}, ow);
  ASSERT_TRUE(curve); ASSERT_TRUE(affine);
  for (const auto precision : precisions) {
    CubicExpectationWork work;
    const CubicExpectationRequest r{.5, .5, 0., .03, -.01};
    const R factor=exp((3*(R(r.rate)-R(r.yield))-R(r.rate))*R(r.dt));
    auto result = cubic_expectation(*curve, r, precision, {}, work);
    ASSERT_TRUE(result) << result.error().message();
    close(result->value, R(".125")*factor);
    close(result->right_delta, R(".75")*factor);
    EXPECT_EQ(work.finite_completions, 1U);
    auto zero = cubic_expectation(*affine, {0., .5, .2, .03, -.01}, precision, {}, work);
    ASSERT_TRUE(zero);
    close(zero->value, 2*exp(-R(.03)*R(.5)));
    close(zero->right_delta, -3*exp(R(.01)*R(.5)));
    EXPECT_TRUE(zero->left_delta.empty());
  }
}

TEST(CashCubicExpectation, SupportedExtremeTailIsKeptAndThresholdDomainRefuses) {
  const std::array cells{HermiteCell{0., 1., 0., 0., 0., 0.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {1., 0., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  for (const auto precision : precisions) {
    CubicExpectationWork work;
    const CubicExpectationRequest r{std::exp(-5.), 1., .1, 0., 0.};
    const R s(r.stock), sigma(r.sigma), d1=(log(s)+sigma*sigma/2)/sigma;
    auto result = cubic_expectation(*curve, r, precision, {}, work);
    ASSERT_TRUE(result) << result.error().message();
    const R expected=s*phi(d1)-phi(d1-sigma);
    close(result->value, expected);
    close(result->right_delta, phi(d1));
    EXPECT_GT(R(result->value), 0);
    EXPECT_LT(R(result->value), R("1e-500"));
    EXPECT_GT(R(work.tail.value_absolute_sum), R(result->value));
    const auto refused=cubic_expectation(*curve, {1., 1e-300, 1e-150, 0., 0.},
        precision, {}, work);
    EXPECT_FALSE(refused); // No small-variance deterministic substitution.
    EXPECT_EQ(work.route, CubicExpectationRoute::Stochastic);
  }
}

TEST(CashCubicExpectation, TailExhaustionKeepsCompletedFiniteWork) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 1., 1.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {1., 1., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  CubicExpectationWork work;
  CubicExpectationBudget budget;
  budget.owner.max_moments = 4;
  auto result = cubic_expectation(*curve, {1., .5, .2, .03, .01},
      WeightPrecision::Decimal100, budget, work);
  EXPECT_FALSE(result);
  EXPECT_EQ(work.finite_completions, 1U);
  EXPECT_EQ(work.moment_attempts, 4U);
  EXPECT_EQ(work.tail_attempts, 1U);
  EXPECT_EQ(work.tail_moment_attempts, 0U);
  EXPECT_EQ(work.phase, CubicExpectationPhase::Tail);
  budget = {}; budget.max_tail_moments = 1;
  result = cubic_expectation(*curve, {1., .5, .2, .03, .01},
      WeightPrecision::Decimal100, budget, work);
  EXPECT_FALSE(result);
  EXPECT_EQ(work.tail_moment_attempts, 1U);
  EXPECT_FALSE(work.tail.moments[0].empty());
  EXPECT_TRUE(work.tail.moments[1].empty());
  budget = {}; budget.max_conversions = 0;
  EXPECT_FALSE(cubic_expectation(*curve, {1., .5, .2, .03, .01},
      WeightPrecision::Decimal100, budget, work));
  EXPECT_EQ(work.conversions, 0U);
  EXPECT_EQ(work.finite_attempts, 0U);
}

TEST(CashCubicExpectation, InvalidRequestsResetAndLargeBudgetsCannotOverflowCaps) {
  CubicOwnerWork ow;
  auto curve = StockCubic::make({}, {0., 1., 1.}, {}, ow);
  ASSERT_TRUE(curve);
  CubicExpectationWork work;
  CubicExpectationBudget budget;
  budget.owner.max_workspace_bytes = std::numeric_limits<std::size_t>::max();
  budget.max_conversions = std::numeric_limits<std::size_t>::max();
  auto result = cubic_expectation(*curve, {1., 1., .2, .03, .01},
      WeightPrecision::Decimal100, budget, work);
  ASSERT_TRUE(result);
  EXPECT_LE(work.workspace_bytes, 64U*1024U*1024U);
  for (const auto r : {CubicExpectationRequest{-1., 1., .2, .03, .01},
                      {1., -1., .2, .03, .01}, {1., 1., -.2, .03, .01},
                      {1., 1., .2, std::numeric_limits<double>::infinity(), .01}}) {
    EXPECT_FALSE(cubic_expectation(*curve, r, WeightPrecision::Decimal100, {}, work));
    EXPECT_EQ(work.conversions, 0U);
    EXPECT_EQ(work.tail_attempts, 0U);
  }
  EXPECT_FALSE(cubic_expectation(*curve, {1., 1., .2, .03, .01},
      static_cast<WeightPrecision>(99), {}, work));
  EXPECT_EQ(work.workspace_bytes, 0U);
  EXPECT_FALSE(cubic_expectation(*curve, {1., 1e6, .2, 1., .01},
      WeightPrecision::Decimal100, {}, work));
}

TEST(CashCubicExpectation, SignedNonconvexCubicMatchesIndependentMoments) {
  const std::array cells{HermiteCell{0., 2., -3., 4., -2., 5.}};
  CubicOwnerWork ow;
  auto curve = StockCubic::make(cells, {2., 4., 5.}, {}, ow);
  ASSERT_TRUE(curve);
  const CubicExpectationRequest r{1.5, .7, .3, .02, -.01};
  const R s(r.stock), t(r.dt), sigma(r.sigma), rate(r.rate), q(r.yield);
  auto below = [&](unsigned k) -> R {
    const R n(k);
    return pow(s,k)*exp((n*(rate-q)+n*(n-1)*sigma*sigma/2-rate)*t)*
        phi((log(2/s)-(rate-q+(n-R(".5"))*sigma*sigma)*t)/(sigma*sqrt(t)));
  };
  const R a0=exp(-rate*t)-below(0), a1=s*exp(-q*t)-below(1);
  const R expected=-3*below(0)-2*below(1)+R("4.75")*below(2)-below(3)+5*a1-6*a0;
  const R derivative=(-2*below(1)+R("9.5")*below(2)-3*below(3)+5*a1)/s;
  for (const auto precision : precisions) {
    CubicExpectationWork work;
    auto result=cubic_expectation(*curve,r,precision,{},work);
    ASSERT_TRUE(result) << result.error().message();
    close(result->value, expected); close(result->right_delta, derivative);
  }
}

TEST(CashCubicExpectation, CancellationBetweenCompleteCellsAndTailIsVisible) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 1., 1.}};
  CubicOwnerWork ow;
  auto curve=StockCubic::make(cells,{1.,1.,-1.},{},ow);
  ASSERT_TRUE(curve);
  for (const auto precision : precisions) {
    CubicExpectationWork work;
    // Size-biased probability is exactly one half: d1=0. The positive finite
    // derivative and negative tail derivative cancel; neither is itself zero.
    auto result=cubic_expectation(*curve,{1.,1.,.5,0.,.125},precision,{},work);
    EXPECT_FALSE(result);
    EXPECT_EQ(work.finite_completions,1U);
    EXPECT_EQ(work.tail_completions,1U);
    EXPECT_GT(R(work.tail.delta_absolute_sum),0);
  }
}

TEST(CashCubicExpectation, LocalSeriesExhaustionRetainsActualCompletedPrefix) {
  const double a=1.,b=std::nextafter(a,std::numeric_limits<double>::infinity());
  const std::array cells{HermiteCell{0.,a,0.,a,1.,1.},HermiteCell{a,b,a,b,1.,1.}};
  CubicOwnerWork ow;
  auto curve=StockCubic::make(cells,{b,b,1.},{},ow);
  ASSERT_TRUE(curve);
  CubicExpectationBudget budget;
  budget.max_coefficient_products=1;
  CubicExpectationWork work;
  auto result=cubic_expectation(*curve,{1.,.5,.2,.03,.01},
      WeightPrecision::Decimal100,budget,work);
  EXPECT_FALSE(result);
  EXPECT_EQ(work.finite_completions,1U);
  EXPECT_EQ(work.finite_attempts,2U);
  EXPECT_EQ(work.coefficient_products,1U);
  EXPECT_EQ(work.tail_attempts,0U);
  EXPECT_EQ(work.phase,CubicExpectationPhase::FiniteCell);
  EXPECT_EQ(work.component_index,1U);
}

TEST(CashCubicExpectation, ResultOwnsAllStringsAfterInputLifetimeEnds) {
  std::optional<CubicExpectation> retained;
  CubicExpectationWork work;
  {
    CubicOwnerWork ow;
    auto curve=StockCubic::make({}, {0.,2.,3.}, {}, ow);
    ASSERT_TRUE(curve);
    auto result=cubic_expectation(*curve,{4.,.5,.2,0.,0.},
        WeightPrecision::Decimal100,{},work);
    ASSERT_TRUE(result);
    retained=*result;
  }
  close(retained->value,R(14)); close(retained->right_delta,R(3));
  EXPECT_EQ(work.endpoint_projection_count,0U);
}

TEST(CashCubicExpectation, PartitionedSignedConstantsAndAllZeroRemainValid) {
  for (const double value : {0., -3., 3.}) {
    const std::array cells{HermiteCell{0., 1., value, value, 0., 0.},
        HermiteCell{1., 2., value, value, 0., 0.}};
    CubicOwnerWork ow;
    auto curve=StockCubic::make(cells,{2.,value,0.},{},ow);
    ASSERT_TRUE(curve);
    for (const auto precision : precisions) {
      CubicExpectationWork work;
      auto result=cubic_expectation(*curve,{1.5,.5,.2,-.03,.01},precision,{},work);
      ASSERT_TRUE(result) << result.error().message();
      close(result->value,R(value)*exp(R(.03)*R(.5)));
      close(result->right_delta,R(0));
      EXPECT_EQ(R(result->delta_absolute_sum),0);
      if (value==0.) EXPECT_EQ(R(result->value_absolute_sum),0);
      EXPECT_EQ(work.finite_completions,2U);
      EXPECT_EQ(work.tail_completions,1U);
      EXPECT_EQ(work.exact_zeros,2U);
    }
  }
}
} // namespace
} // namespace atx::vol::cash_gaussian
