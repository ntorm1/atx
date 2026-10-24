#include "stock_pwl.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/black76.hpp"

namespace {
using namespace atx::vol;
namespace cg = cash_gaussian;

Result<cg::StockPwl> payoff(double strike, Side side, cg::Budget &budget) {
  const std::array<double, 2> stock{0., strike};
  const std::array<double, 2> value{side == Side::Call ? 0. : strike, 0.};
  return cg::StockPwl::create(stock, value, side == Side::Call ? 1. : 0., budget);
}

TEST(CashGaussianStockPwl, ConstantsLinearAndFullInfiniteTailMoments) {
  cg::Budget budget;
  const std::array<double, 4> x{0., 2., 80., 400.};
  const std::array<double, 4> ones{1., 1., 1., 1.};
  const auto constant = cg::StockPwl::create(x, ones, 0., budget);
  const auto linear = cg::StockPwl::create(x, x, 1., budget);
  ASSERT_TRUE(constant && linear);
  for (double stock : {1e-6, 1., 100., 1e6}) for (double sigma : {.001, .2, 1.5})
    for (double rate : {-.03, 0., .1}) for (double q : {-.02, .04}) {
      const double T = 1.7;
      const auto mass = cg::expectation(*constant, stock, T, sigma, rate, q, budget);
      const auto mean = cg::expectation(*linear, stock, T, sigma, rate, q, budget);
      ASSERT_TRUE(mass && mean);
      EXPECT_NEAR(*mass, std::exp(-rate * T), 3e-14);
      EXPECT_NEAR(*mean, stock * std::exp(-q * T), 3e-14 * std::max(stock, 1.));
  }
  EXPECT_EQ(budget.interval_evaluations, 4u * 3u * 3u * 2u * 2u * x.size());
}

TEST(CashGaussianStockPwl, NonconvexFunctionMatchesIndependentHingeExpansion) {
  cg::Budget budget;
  const std::array<double, 4> x{0., 50., 100., 150.}, y{4., 6., 3., 12.};
  const auto f = cg::StockPwl::create(x, y, .2, budget);
  ASSERT_TRUE(f);
  constexpr double S = 110., T = .7, sigma = .45, r = -.02, q = .01;
  const double F = S * std::exp((r - q) * T), df = std::exp(-r * T);
  const double expected = df * (4. + .04 * F)
      - .10 * black76_price(F, 50., T, sigma, df, Side::Call)
      + .24 * black76_price(F, 100., T, sigma, df, Side::Call)
      + .02 * black76_price(F, 150., T, sigma, df, Side::Call);
  const auto actual = cg::expectation(*f, S, T, sigma, r, q, budget);
  ASSERT_TRUE(actual);
  EXPECT_NEAR(*actual, expected, 2e-12);
  EXPECT_EQ(budget.interval_evaluations, 4u);
}

TEST(CashGaussianStockPwl, EuropeanNoCashMatchesIndependentBlack76BothSides) {
  cg::Budget budget;
  for (const Side side : {Side::Call, Side::Put}) {
    const auto f = payoff(100., side, budget);
    ASSERT_TRUE(f);
    for (double S : {60., 100., 160.}) for (double T : {1e-6, .5, 3.})
      for (double sigma : {.001, .25, 1.}) for (double r : {-.02, 0., .1})
        for (double q : {0., .03}) {
          const auto actual = cg::expectation(*f, S, T, sigma, r, q, budget);
          ASSERT_TRUE(actual);
          const double expected = black76_price(S * std::exp((r-q)*T), 100., T,
                                                sigma, std::exp(-r*T), side);
          EXPECT_NEAR(*actual, expected, 8e-12);
        }
  }
}

TEST(CashGaussianStockPwl, LiquidatorJumpIsExactAndKeepsZeroBranchAndCashBits) {
  cg::Budget budget;
  const std::array<double, 4> x{0., 1.5, 10., 100.}, y{7., 7., 3., 8.};
  const auto f = cg::StockPwl::create(x, y, -.01, budget);
  ASSERT_TRUE(f);
  for (double cash : {0., .3, 2., 125.}) {
    const auto before = cg::liquidator_jump(*f, cash, budget);
    ASSERT_TRUE(before);
    EXPECT_EQ(before->knots().size(), cash == 0. ? x.size() : x.size() + 1);
    EXPECT_EQ(before->knots()[0], 0.);
    if (cash > 0.) EXPECT_EQ(before->knots()[1], cash);
    for (double S : {0., .1, .3, 1.5, 2., 7., 100., 125., 1000.}) {
      const auto a = before->value(S), b = f->value(std::max(S-cash, 0.));
      ASSERT_TRUE(a && b);
      EXPECT_NEAR(*a, *b, 2e-13);
    }
  }
}

TEST(CashGaussianStockPwl, SingleCashEuropeanMatchesIndependentCallSpreadIdentity) {
  cg::Budget budget;
  constexpr double K = 100., T = .8, sigma = .3;
  for (const Side side : {Side::Call, Side::Put}) {
    const auto after = payoff(K, side, budget);
    ASSERT_TRUE(after);
    for (double D : {0., 5., 150.}) for (double S : {25., 100., 200.})
      for (double r : {-.03, 0., .05}) {
        const auto before = cg::liquidator_jump(*after, D, budget);
        ASSERT_TRUE(before);
        const auto actual = cg::expectation(*before, S, T, sigma, r, .01, budget);
        ASSERT_TRUE(actual);
        const double F = S * std::exp((r-.01)*T), df = std::exp(-r*T);
        const double call = black76_price(F, K+D, T, sigma, df, Side::Call);
        const double expected = side == Side::Call ? call :
            K*df - (D == 0. ? F*df : black76_price(F, D, T, sigma, df, Side::Call)) + call;
        EXPECT_NEAR(*actual, expected, 8e-12);
      }
  }
}

TEST(CashGaussianStockPwl, ZeroStateAndDeterministicTransitionsAreExplicit) {
  cg::Budget budget;
  const auto put = payoff(100., Side::Put, budget);
  ASSERT_TRUE(put);
  for (double r : {-.03, 0., .05}) {
    const auto zero = cg::expectation(*put, 0., 2., .4, r, .02, budget);
    ASSERT_TRUE(zero);
    EXPECT_DOUBLE_EQ(*zero, 100. * std::exp(-r*2.));
    for (double S : {50., 100., 200.}) {
      const auto now = cg::expectation(*put, S, 0., .4, r, .02, budget);
      const auto deterministic = cg::expectation(*put, S, 2., 0., r, .02, budget);
      ASSERT_TRUE(now && deterministic);
      EXPECT_DOUBLE_EQ(*now, std::max(100.-S, 0.));
      EXPECT_NEAR(*deterministic, std::exp(-r*2.) *
          std::max(100.-S*std::exp((r-.02)*2.), 0.), 5e-13);
    }
  }
  EXPECT_EQ(budget.interval_evaluations, 0u);
  EXPECT_EQ(budget.zero_state_evaluations, 3u);
  EXPECT_EQ(budget.deterministic_evaluations, 18u);
}

TEST(CashGaussianStockPwl, InvalidDataAndCumulativeResourceLimitsRefuseBeforePartialPrice) {
  const std::array<double, 3> x{0., 50., 100.}, y{100., 50., 0.};
  cg::Budget budget;
  for (const std::vector<double> &bad : {std::vector<double>{}, {1., 50., 100.},
       {0., 0., 100.}, {0., 100., 50.}, {0., 50., std::numeric_limits<double>::infinity()}}) {
    const auto value = cg::StockPwl::create(bad, y, 0., budget);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().code(), ErrorCode::InvalidArgument);
  }
  const auto f = cg::StockPwl::create(x, y, 0., budget);
  ASSERT_TRUE(f);
  budget.limits.max_interval_evaluations = 3;
  ASSERT_TRUE(cg::expectation(*f, 100., .5, .2, .04, 0., budget));
  const auto exhausted = cg::expectation(*f, 100., .5, .2, .04, 0., budget);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error().code(), ErrorCode::Unavailable);
  EXPECT_EQ(budget.interval_evaluations, 3u);
  budget.limits.max_knots = 3;
  const auto too_many = cg::liquidator_jump(*f, 1., budget);
  ASSERT_FALSE(too_many);
  EXPECT_EQ(too_many.error().code(), ErrorCode::Unavailable);
  const auto invalid = cg::expectation(*f, 100., -.1, .2, .04, 0., budget);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST(CashGaussianStockPwl, ZeroTimeIgnoresUnrepresentableRateDifference) {
  cg::Budget budget;
  const auto put = payoff(100., Side::Put, budget);
  ASSERT_TRUE(put);
  const auto identity = cg::expectation(*put, 50., 0., .2, 1e308, -1e308, budget);
  ASSERT_TRUE(identity);
  EXPECT_DOUBLE_EQ(*identity, 50.);
}

TEST(CashGaussianStockPwl, ArithmeticMovedStateAndAllBudgetKindsFailClosed) {
  cg::Budget budget;
  const auto original = payoff(100., Side::Put, budget);
  ASSERT_TRUE(original);
  auto moving = *original;
  const auto moved = std::move(moving);
  EXPECT_FALSE(moving.value(100.));
  EXPECT_FALSE(cg::expectation(moving, 100., .5, .2, .04, 0., budget));
  EXPECT_FALSE(cg::liquidator_jump(moving, 1., budget));
  const auto overflow = cg::expectation(moved, 100., 1., .2, -1000., 0., budget);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().code(), ErrorCode::OutOfRange);
  const auto bad_sigma = cg::expectation(moved, 100., 1., -1., 0., 0., budget);
  ASSERT_FALSE(bad_sigma);
  EXPECT_EQ(bad_sigma.error().code(), ErrorCode::InvalidArgument);
  const auto collapsed = cg::liquidator_jump(moved, 1e300, budget);
  ASSERT_FALSE(collapsed);
  EXPECT_EQ(collapsed.error().code(), ErrorCode::OutOfRange);
  budget.limits.max_expectations = budget.expectations;
  const auto query_budget = cg::expectation(moved, 0., 1., .2, 0., 0., budget);
  ASSERT_FALSE(query_budget);
  EXPECT_EQ(query_budget.error().code(), ErrorCode::Unavailable);
  budget.limits.max_created_knots = budget.created_knots;
  const auto node_budget = cg::liquidator_jump(moved, 0., budget);
  ASSERT_FALSE(node_budget);
  EXPECT_EQ(node_budget.error().code(), ErrorCode::Unavailable);
}

// Independent test-only pre-event Gaussian integral of the closed-form European
// post-event price. Finite quadrature tails have an explicit moment bound below;
// the component itself still integrates its complete [0,infinity) representation.
double single_cash_integral(Side side, int steps) {
  constexpr double S = 100., K = 100., D = 15., sigma = .25;
  constexpr double before = .4, after = .6, r = .04, q = .01;
  const double sd = sigma * std::sqrt(before);
  const double location = std::log(S) + (r-q-.5*sigma*sigma)*before;
  const auto integrand = [&](double z) {
    const double post = std::max(std::exp(location+sd*z)-D, 0.);
    const double option = post == 0. ? (side == Side::Put ? K*std::exp(-r*after) : 0.) :
        black76_price(post*std::exp((r-q)*after), K, after, sigma, std::exp(-r*after), side);
    return option * std::exp(-.5*z*z) / std::sqrt(2.*std::numbers::pi);
  };
  const auto simpson = [&](double lo, double hi) {
    const double h = (hi-lo)/steps;
    double sum = integrand(lo)+integrand(hi);
    for (int i = 1; i < steps; ++i) sum += (i%2 == 0 ? 2. : 4.)*integrand(lo+h*i);
    return sum*h/3.;
  };
  const double cash_boundary = std::clamp((std::log(D)-location)/sd, -12., 12.);
  return std::exp(-r*before) * (simpson(-12., cash_boundary)+simpson(cash_boundary, 12.));
}

TEST(CashGaussianStockPwl, InteriorCashEuropeanConvergesToIndependentQuadrature) {
  constexpr double S = 100., K = 100., D = 15., sigma = .25;
  constexpr double before = .4, after = .6, r = .04, q = .01;
  const double sd = sigma * std::sqrt(before);
  const double tail_mass = std::erfc(12./std::sqrt(2.));
  const double tail_mean = S*std::exp((r-q)*before)*.5*
      (std::erfc((12.+sd)/std::sqrt(2.))+std::erfc((12.-sd)/std::sqrt(2.)));
  const double tail_bound = std::exp(-r*before) *
      (std::exp(-q*after)*tail_mean+K*std::exp(-r*after)*tail_mass);
  ASSERT_LT(tail_bound, 1e-25);
  RecordProperty("quadrature_tail_bound", tail_bound);
  for (const Side side : {Side::Call, Side::Put}) {
    const double reference = single_cash_integral(side, 16384);
    EXPECT_NEAR(reference, single_cash_integral(side, 8192), 2e-10);
    cg::Budget budget;
    budget.limits.max_knots = 8192;
    double previous_error = std::numeric_limits<double>::infinity();
    for (int count : {1024, 2048, 4096}) {
      std::vector<double> x, y;
      for (int i = 0; i <= count; ++i) {
        const double stock = 600.*i/count;
        x.push_back(stock);
        y.push_back(stock == 0. ? (side == Side::Put ? K*std::exp(-r*after) : 0.) :
            black76_price(stock*std::exp((r-q)*after), K, after, sigma, std::exp(-r*after), side));
      }
      const auto represented = cg::StockPwl::create(x, y,
          side == Side::Call ? std::exp(-q*after) : 0., budget);
      ASSERT_TRUE(represented);
      const auto shifted = cg::liquidator_jump(*represented, D, budget);
      ASSERT_TRUE(shifted);
      const auto actual = cg::expectation(*shifted, S, before, sigma, r, q, budget);
      ASSERT_TRUE(actual);
      const double error = std::abs(*actual-reference);
      EXPECT_LT(error, previous_error);
      previous_error = error;
      if (count == 4096) EXPECT_LT(error, 5e-5);
    }
    RecordProperty(side == Side::Call ? "call_finest_error" : "put_finest_error", previous_error);
    RecordProperty(side == Side::Call ? "call_reference" : "put_reference", reference);
  }
}
} // namespace
