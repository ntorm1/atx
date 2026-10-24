#include "cubic_local_weights.hpp"

#include <cmath>
#include <array>
#include <limits>
#include <string>

#include <boost/multiprecision/cpp_bin_float.hpp>
#include <gtest/gtest.h>

namespace {
using namespace atx::vol;
namespace cg = cash_gaussian;
using CheckReal = boost::multiprecision::number<
    boost::multiprecision::cpp_bin_float<250>>;

TEST(CashGaussianLocalWeights, ExactRationalBasisMassAndParity) {
  for (const unsigned degree : {2u, 3u}) {
    for (unsigned j = 0; j <= degree; ++j) {
      const auto mass = cg::local_bernstein_integral(degree, j, 0);
      ASSERT_TRUE(mass);
      EXPECT_EQ(mass->numerator, 1);
      EXPECT_EQ(mass->denominator, degree + 1u);
      for (const unsigned power : {1u, 2u, 17u, 512u, 513u}) {
        const auto first = cg::local_bernstein_integral(degree, j, power);
        const auto mirror = cg::local_bernstein_integral(degree, degree-j, power);
        ASSERT_TRUE(first);
        ASSERT_TRUE(mirror);
        EXPECT_EQ(first->numerator, (power % 2 ? -1 : 1) * mirror->numerator);
        EXPECT_EQ(first->denominator, mirror->denominator);
      }
    }
  }
  const auto first_moment = cg::local_bernstein_integral(3, 1, 1);
  ASSERT_TRUE(first_moment);
  EXPECT_EQ(first_moment->numerator, -1);
  EXPECT_EQ(first_moment->denominator, 20u);
  EXPECT_FALSE(cg::local_bernstein_integral(4, 0, 0));
  EXPECT_FALSE(cg::local_bernstein_integral(3, 4, 0));
  EXPECT_FALSE(cg::local_bernstein_integral(3, 0, 514));
}

TEST(CashGaussianLocalWeights, AdjacentCellUsesDirectWeightsAndExplicitRemainders) {
  const double a = 1e12, b = std::nextafter(a, std::numeric_limits<double>::infinity());
  for (const auto precision : {cg::WeightPrecision::Decimal100,
                              cg::WeightPrecision::Decimal200}) {
    cg::LocalWeightWork work;
    const auto result = cg::cubic_local_weight_control({a, b, a, 1., .2, 0., 0.},
                                                       precision, {}, work);
    ASSERT_TRUE(result) << result.error().to_string();
    EXPECT_EQ(work.route, cg::LocalWeightRoute::LocalSeries);
    EXPECT_EQ(work.reason, cg::LocalWeightReason::None);
    EXPECT_GT(work.log_order, 0u);
    EXPECT_LE(work.log_order, 16u);
    EXPECT_GT(work.exponential_order, 0u);
    EXPECT_LE(work.exponential_order, 16u);
    EXPECT_GT(work.coefficient_products, 0u);
    EXPECT_LE(work.coefficient_products, 262144u);
    EXPECT_GT(work.workspace_bytes, 0u);
    EXPECT_LE(work.workspace_bytes, 1048576u);
    EXPECT_EQ(work.moments.moment_attempts, 4u);
    EXPECT_EQ(work.moments.normal_functions, 8u);
    EXPECT_EQ(work.moments.polynomial_terms, 0u);
    EXPECT_LT(std::stod(result->weights.maximum_condition), 4.);
    const double mass = std::stod(result->weights.mass);
    for (const auto &w : result->weights.value) EXPECT_NEAR(std::stod(w)/mass, .25, 1e-14);
    for (const auto &w : result->weights.derivative)
      EXPECT_NEAR(std::stod(w)/mass, 1./3., 1e-14);
    EXPECT_GT(std::stod(result->density_remainder), 0.);
    EXPECT_LT(std::stod(result->relative_value_remainder), 1e-70);
    for (const auto &bound : result->value_remainders) EXPECT_GT(std::stod(bound), 0.);
    for (const auto &bound : result->derivative_remainders) EXPECT_GT(std::stod(bound), 0.);
  }
}

TEST(CashGaussianLocalWeights, LowProductBudgetRefusesWithoutMomentFallback) {
  cg::LocalWeightBudget budget;
  budget.max_coefficient_products = 1;
  cg::LocalWeightWork work;
  const double b = std::nextafter(1., 2.);
  const auto result = cg::cubic_local_weight_control({1., b, 1., 1., .2, 0., 0.},
      cg::WeightPrecision::Decimal100, budget, work);
  EXPECT_FALSE(result);
  EXPECT_EQ(work.route, cg::LocalWeightRoute::LocalSeries);
  EXPECT_EQ(work.coefficient_products, 1u);
  EXPECT_EQ(work.moments.polynomial_terms, 0u);
}

TEST(CashGaussianLocalWeights, RationalIntegralsMatchIndependentBetaExpansion) {
  const auto choose = [](unsigned n, unsigned k) {
    CheckReal value = 1;
    for (unsigned i = 1; i <= k; ++i) value *= CheckReal(n+1-i)/i;
    return value;
  };
  for (unsigned degree : {2u, 3u}) {
    for (unsigned j = 0; j <= degree; ++j) {
      for (unsigned k = 0; k <= 12; ++k) {
        CheckReal beta_sum = 0;
        for (unsigned l = 0; l <= k; ++l) {
          CheckReal beta = CheckReal(1)/(degree+1);
          for (unsigned i = 1; i <= l; ++i)
            beta *= CheckReal(j+i)/(degree+1+i);
          beta_sum += ((k-l) % 2 ? -1 : 1)*choose(k, l)*pow(CheckReal(2), l)*beta;
        }
        const auto result = cg::local_bernstein_integral(degree, j, k);
        ASSERT_TRUE(result);
        const CheckReal actual = CheckReal(result->numerator)/result->denominator;
        EXPECT_LT(abs(actual-beta_sum), CheckReal("1e-230"));
      }
    }
    for (unsigned k : {511u, 512u, 513u}) {
      CheckReal sum = 0;
      for (unsigned j = 0; j <= degree; ++j) {
        const auto result = cg::local_bernstein_integral(degree, j, k);
        ASSERT_TRUE(result);
        sum += CheckReal(result->numerator)/result->denominator;
      }
      const CheckReal expected = k % 2 ? CheckReal(0) : CheckReal(1)/(k+1);
      EXPECT_LT(abs(sum-expected), CheckReal("1e-240"));
    }
  }
}

TEST(CashGaussianLocalWeights, TwoPrecisionsRetainValueAndDerivativeAcrossStockScales) {
  const std::array<cg::WeightRequest, 5> requests{{
      {1e12, std::nextafter(1e12, 2e12), 1e12, 1., .2, 0., 0.},
      {1e-12, std::nextafter(1e-12, 2e-12), 1e-12, 1., .2, 0., 0.},
      {1., std::nextafter(1., 2.), 1., 1., .2, 0., 0.},
      {100., std::nextafter(100., 200.), 100., 1e-16, .2, 0., 0.},
      {99.9999999999, 100.0000000001, 100., .5, .35, -.03, -.01}}};
  for (const auto &request : requests) {
    cg::LocalWeightWork first_work, second_work;
    const auto first = cg::cubic_local_weight_control(request,
        cg::WeightPrecision::Decimal100, {}, first_work);
    const auto second = cg::cubic_local_weight_control(request,
        cg::WeightPrecision::Decimal200, {}, second_work);
    ASSERT_TRUE(first) << first.error().to_string();
    ASSERT_TRUE(second) << second.error().to_string();
    ASSERT_EQ(first_work.route, cg::LocalWeightRoute::LocalSeries);
    ASSERT_EQ(second_work.route, cg::LocalWeightRoute::LocalSeries);
    for (unsigned j = 0; j < 4; ++j) {
      const CheckReal a(first->weights.value[j]), b(second->weights.value[j]);
      EXPECT_LT(abs(a-b)/abs(b), CheckReal("1e-40"));
    }
    for (unsigned j = 0; j < 3; ++j) {
      const CheckReal a(first->weights.derivative[j]), b(second->weights.derivative[j]);
      EXPECT_LT(abs(a-b)/abs(b), CheckReal("1e-40"));
    }
    EXPECT_LE(first_work.local_functions, 64u);
    EXPECT_LE(second_work.selection_checks, 256u);
  }
}

TEST(CashGaussianLocalWeights, PositiveIntegrandQuadratureChecksEveryWeight) {
  const cg::WeightRequest request{99.9999999999, 100.0000000001, 100., .5, .35, -.03, -.01};
  cg::LocalWeightWork work;
  const auto result = cg::cubic_local_weight_control(request,
      cg::WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(result);
  ASSERT_EQ(work.route, cg::LocalWeightRoute::LocalSeries);
  const double h = request.upper-request.lower, ratio = h/request.lower;
  const double variance = request.sigma*request.sigma*request.dt;
  const double sd = std::sqrt(variance);
  const double drift = (request.rate-request.yield)*request.dt-variance/2;
  const double log_a = std::log1p((request.lower-request.stock)/request.stock);
  constexpr unsigned intervals = 4096;
  std::array<double, 7> integrals{};
  for (unsigned i = 0; i <= intervals; ++i) {
    const double u = double(i)/intervals, v = 1-u;
    const double log_x = log_a+std::log1p(ratio*u);
    const double z = (log_x-drift)/sd;
    const double density = std::exp(-request.rate*request.dt-z*z/2)*ratio/
        ((1+ratio*u)*sd*std::sqrt(2*std::acos(-1.)));
    const double multiplier = i == 0 || i == intervals ? 1 : i % 2 ? 4 : 2;
    const double x_over_s = (request.lower/request.stock)*(1+ratio*u);
    const std::array<double, 7> basis{{v*v*v, 3*u*v*v, 3*u*u*v, u*u*u,
        x_over_s*v*v, x_over_s*2*u*v, x_over_s*u*u}};
    for (unsigned j = 0; j < 7; ++j) integrals[j] += multiplier*density*basis[j];
  }
  for (unsigned j = 0; j < 7; ++j) {
    const double expected = integrals[j]/(3*intervals);
    const double actual = std::stod(j < 4 ? result->weights.value[j]
                                        : result->weights.derivative[j-4]);
    EXPECT_NEAR(actual, expected, 5e-11*expected);
  }
}

TEST(CashGaussianLocalWeights, CubicPropagationAndAnalyticDerivativeMatchRawMoments) {
  const cg::WeightRequest request{99.9999999999, 100.0000000001, 100., .5, .35, -.03, -.01};
  cg::LocalWeightWork work;
  const auto result = cg::cubic_local_weight_control(request,
      cg::WeightPrecision::Decimal200, {}, work);
  ASSERT_TRUE(result);
  const CheckReal a(request.lower), b(request.upper), h = b-a, stock(request.stock);
  const auto choose = [](unsigned n, unsigned k) {
    CheckReal value = 1;
    for (unsigned i = 1; i <= k; ++i) value *= CheckReal(n+1-i)/i;
    return value;
  };
  for (unsigned degree = 0; degree <= 3; ++degree) {
    std::array<CheckReal, 4> coefficients{};
    for (unsigned j = 0; j <= 3; ++j)
      for (unsigned k = 0; k <= degree && k <= j; ++k)
        coefficients[j] += choose(degree, k)*pow(a, degree-k)*pow(h, k)*
            choose(j, k)/choose(3, k);
    CheckReal price = 0, delta = 0;
    for (unsigned j = 0; j < 4; ++j)
      price += coefficients[j]*CheckReal(result->weights.value[j]);
    for (unsigned j = 0; j < 3; ++j)
      delta += 3*(coefficients[j+1]-coefficients[j])/h*
          CheckReal(result->weights.derivative[j]);
    const CheckReal expected = pow(b, degree)*CheckReal(result->weights.scaled_moments[degree]);
    EXPECT_LT(abs(price-expected)/expected, CheckReal("1e-40"));
    EXPECT_LT(abs(delta-CheckReal(degree)/stock*expected)/expected, CheckReal("1e-40"));
  }
}

TEST(CashGaussianLocalWeights, NonadmissionKeepsOriginalResultAndWorkExactly) {
  const std::array<cg::WeightRequest, 4> requests{{
      {0., 100., 80., .5, .2, -.03, .02},
      {80., 120., 100., .7, .35, .03, .01},
      {99.99, 100.01, 100., .01, .001, 0., 0.},
      {99.9, 100.1, 100., 1., .2, 0., 0.}}};
  const std::array<cg::LocalWeightReason, 4> reasons{{cg::LocalWeightReason::ZeroLower,
      cg::LocalWeightReason::WideCell, cg::LocalWeightReason::DensityVariation,
      cg::LocalWeightReason::OrderCap}};
  for (unsigned i = 0; i < requests.size(); ++i) {
    cg::WeightWork original_work;
    cg::LocalWeightWork local_work;
    const auto original = cg::cubic_weight_control(requests[i],
        cg::WeightPrecision::Decimal100, {}, original_work);
    const auto local = cg::cubic_local_weight_control(requests[i],
        cg::WeightPrecision::Decimal100, {}, local_work);
    ASSERT_TRUE(original);
    ASSERT_TRUE(local) << local.error().to_string();
    EXPECT_EQ(local_work.route, cg::LocalWeightRoute::OriginalMoments);
    EXPECT_EQ(local_work.reason, reasons[i]);
    EXPECT_EQ(local->weights.value, original->value);
    EXPECT_EQ(local->weights.derivative, original->derivative);
    EXPECT_EQ(local->weights.scaled_moments, original->scaled_moments);
    EXPECT_EQ(local->weights.mass, original->mass);
    EXPECT_EQ(local->weights.discounted_stock_ratio, original->discounted_stock_ratio);
    EXPECT_EQ(local->weights.maximum_condition, original->maximum_condition);
    EXPECT_EQ(local_work.moments.polynomial_terms, original_work.polynomial_terms);
    EXPECT_EQ(local_work.coefficient_products, 0u);
    EXPECT_TRUE(local->density_remainder.empty());
    for (const auto &bound : local->value_remainders) EXPECT_TRUE(bound.empty());
  }
}

TEST(CashGaussianLocalWeights, EveryBudgetAndInvalidInputRetainsActualWorkOnly) {
  const cg::WeightRequest request{1., std::nextafter(1., 2.), 1., 1., .2, 0., 0.};
  for (unsigned kind = 0; kind < 5; ++kind) {
    cg::LocalWeightBudget budget;
    if (kind == 0) budget.max_local_functions = 0;
    if (kind == 1) budget.max_selection_checks = 1;
    if (kind == 2) budget.max_workspace_bytes = 0;
    if (kind == 3) budget.moments.max_moments = 2;
    if (kind == 4) budget.moments.max_normal_functions = 3;
    cg::LocalWeightWork work;
    const auto result = cg::cubic_local_weight_control(request,
        cg::WeightPrecision::Decimal100, budget, work);
    EXPECT_FALSE(result);
    EXPECT_NE(work.route, cg::LocalWeightRoute::OriginalMoments);
    EXPECT_EQ(work.coefficient_products, 0u);
    if (kind == 0) EXPECT_EQ(work.local_functions, 0u);
    if (kind == 1) EXPECT_EQ(work.selection_checks, 1u);
    if (kind == 2) EXPECT_EQ(work.workspace_bytes, 0u);
    if (kind == 3) EXPECT_EQ(work.moments.moment_attempts, 2u);
    if (kind == 4) EXPECT_EQ(work.moments.normal_functions, 2u);
  }
  cg::LocalWeightWork work;
  work.coefficient_products = 999;
  const auto bad_precision = cg::cubic_local_weight_control(request,
      static_cast<cg::WeightPrecision>(7), {}, work);
  EXPECT_FALSE(bad_precision);
  EXPECT_EQ(work.coefficient_products, 0u);
  auto invalid = request;
  invalid.dt = 0.;
  EXPECT_FALSE(cg::cubic_local_weight_control(invalid,
      cg::WeightPrecision::Decimal100, {}, work));
  EXPECT_EQ(work.local_functions, 0u);
  invalid.dt = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(cg::cubic_local_weight_control(invalid,
      cg::WeightPrecision::Decimal100, {}, work));
  EXPECT_EQ(work.moments.moment_attempts, 0u);
}

TEST(CashGaussianLocalWeights, SelectionAndRemaindersMatchIndependentPrecisionFormula) {
  const cg::WeightRequest request{1., std::nextafter(1., 2.), 1., 1., .2, 0., 0.};
  cg::LocalWeightWork work;
  const auto result = cg::cubic_local_weight_control(request,
      cg::WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(result);
  const CheckReal lower(request.lower), upper(request.upper), stock(request.stock);
  const CheckReal width = upper-lower, center = (upper+lower)/2;
  const CheckReal epsilon = width/(upper+lower), sigma(request.sigma), dt(request.dt);
  const CheckReal sd = sigma*sqrt(dt);
  const CheckReal mu = (CheckReal(request.rate)-request.yield-sigma*sigma/2)*dt;
  const CheckReal z = (log(center/stock)-mu)/sd;
  const CheckReal a = 1+z/sd, b = 1/(2*sd*sd), ell = -log(1-epsilon);
  const CheckReal c = abs(a)*ell+b*ell*ell;
  unsigned first_j = 0, first_n = 0;
  CheckReal rf = 0;
  for (unsigned j = 1; j <= 16 && first_j == 0; ++j) {
    const CheckReal rlog = pow(epsilon, j+1)/(CheckReal(j+1)*(1-epsilon));
    const CheckReal e = rlog*(abs(a)+b*(2*ell+rlog));
    for (unsigned n = 1; n <= 16; ++n) {
      CheckReal factorial = 1;
      for (unsigned i = 2; i <= n+1; ++i) factorial *= i;
      const CheckReal candidate = exp(c+e)*(e+pow(c+e, n+1)/factorial);
      if (candidate*exp(c)*(1+epsilon)/(1-epsilon) <= CheckReal("1e-80")) {
        first_j = j;
        first_n = n;
        rf = candidate;
        break;
      }
    }
  }
  EXPECT_EQ(work.log_order, first_j);
  EXPECT_EQ(work.exponential_order, first_n);
  EXPECT_EQ(work.selection_checks, (first_j-1)*16+first_n);
  EXPECT_LT(abs(CheckReal(result->density_remainder)-rf)/rf, CheckReal("1e-70"));
  const CheckReal pi = acos(CheckReal(-1));
  const CheckReal g0 = exp(-CheckReal(request.rate)*dt)*exp(-z*z/2)*
      width/(sd*center*sqrt(2*pi));
  const CheckReal value_bound = g0*rf/4;
  const CheckReal derivative_bound = g0*(center/stock)*(1+epsilon)*rf/3;
  for (const auto &encoded : result->value_remainders)
    EXPECT_LT(abs(CheckReal(encoded)-value_bound)/value_bound, CheckReal("1e-70"));
  for (const auto &encoded : result->derivative_remainders)
    EXPECT_LT(abs(CheckReal(encoded)-derivative_bound)/derivative_bound, CheckReal("1e-70"));
}
} // namespace
