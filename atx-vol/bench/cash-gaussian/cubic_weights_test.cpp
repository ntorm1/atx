#include "cubic_weights.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

#include <gtest/gtest.h>

namespace {
using namespace atx::vol;
namespace cg = cash_gaussian;

TEST(CashGaussianCubicWeights, PositiveCentralWeightsConserveMassAndDerivativeMoment) {
  const cg::WeightRequest request{80., 120., 100., .7, .35, .03, .01};
  for (const auto precision : {cg::WeightPrecision::Decimal100,
                                cg::WeightPrecision::Decimal200}) {
    cg::WeightWork work;
    const auto result = cg::cubic_weight_control(request, precision, {}, work);
    ASSERT_TRUE(result) << result.error().to_string();
    double mass = 0., mean = 0.;
    for (const auto &w : result->value) {
      EXPECT_GT(std::stod(w), 0.);
      mass += std::stod(w);
    }
    for (const auto &w : result->derivative) {
      EXPECT_GT(std::stod(w), 0.);
      mean += std::stod(w);
    }
    EXPECT_NEAR(mass, std::stod(result->mass), 2e-16);
    EXPECT_NEAR(mean, std::stod(result->discounted_stock_ratio), 2e-16);
    EXPECT_EQ(work.moment_attempts, 4u);
    EXPECT_EQ(work.normal_functions, 8u);
    EXPECT_GT(work.polynomial_terms, 0u);
  }
}

TEST(CashGaussianCubicWeights, AdjacentHighStockCellRetainsEveryPositiveWeight) {
  const double a = 1e12, b = std::nextafter(a, std::numeric_limits<double>::infinity());
  cg::WeightWork work;
  const auto result = cg::cubic_weight_control({a, b, a, 1., .2, 0., 0.},
      cg::WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(result) << result.error().to_string();
  const double mass = std::stod(result->mass);
  EXPECT_GT(mass, 0.);
  for (const auto &w : result->value) EXPECT_NEAR(std::stod(w) / mass, .25, 1e-14);
  for (const auto &w : result->derivative) EXPECT_NEAR(std::stod(w) / mass, 1./3., 1e-14);
}

TEST(CashGaussianCubicWeights, InvalidAndUnsupportedIntervalsResetAndRefuse) {
  for (const auto request : {cg::WeightRequest{1., 1., 1., 1., .2, 0., 0.},
                            cg::WeightRequest{1., 2., 1., 0., .2, 0., 0.},
                            cg::WeightRequest{1., 2., 1., 1., 0., 0., 0.},
                            cg::WeightRequest{99., 101., 100., 1e-16, .2, 0., 0.}}) {
    cg::WeightWork work{999, 999, 999, 999};
    const auto result = cg::cubic_weight_control(request, cg::WeightPrecision::Decimal100,
                                                {}, work);
    EXPECT_FALSE(result);
    EXPECT_EQ(work.normal_functions, 0u);
  }
}

TEST(CashGaussianCubicWeights, ResourceRefusalRetainsOnlyActualPartialWork) {
  cg::WeightWork work;
  cg::WeightBudget budget;
  budget.max_moments = 2;
  const auto result = cg::cubic_weight_control({80., 120., 100., .7, .35, .03, .01},
      cg::WeightPrecision::Decimal100, budget, work);
  EXPECT_FALSE(result);
  EXPECT_EQ(work.moment_attempts, 2u);
  EXPECT_EQ(work.normal_functions, 4u);
  EXPECT_EQ(work.polynomial_terms, 0u);
}

TEST(CashGaussianCubicWeights, LocalPolynomialPriceAndDerivativeIntegralsMatchMoments) {
  constexpr double a = 80., b = 120., stock = 100., h = b-a;
  cg::WeightWork work;
  const auto result = cg::cubic_weight_control({a, b, stock, .7, .35, .03, .01},
      cg::WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(result);
  const auto choose = [](unsigned n, unsigned k) {
    double value = 1.;
    for (unsigned i = 1; i <= k; ++i) value *= double(n+1-i)/double(i);
    return value;
  };
  for (unsigned degree = 0; degree < 4; ++degree) {
    std::array<double, 4> coefficients{};
    for (unsigned j = 0; j < 4; ++j)
      for (unsigned k = 0; k <= degree && k <= j; ++k)
        coefficients[j] += choose(degree, k) * std::pow(a, int(degree-k)) *
            std::pow(h, int(k)) * choose(j, k) / choose(3, k);
    double value = 0., derivative = 0.;
    for (unsigned j = 0; j < 4; ++j) value += coefficients[j] * std::stod(result->value[j]);
    for (unsigned j = 0; j < 3; ++j)
      derivative += 3./h * (coefficients[j+1]-coefficients[j]) *
          std::stod(result->derivative[j]);
    const double expected = std::pow(b, int(degree)) *
        std::stod(result->scaled_moments[degree]);
    EXPECT_NEAR(value, expected, 2e-15 * expected);
    EXPECT_NEAR(derivative, degree/stock * expected, 2e-15 * std::max(1., expected));
  }
}

TEST(CashGaussianCubicWeights, NormalCallBudgetAndBadPrecisionFailWithoutFabricatedWork) {
  const cg::WeightRequest request{80., 120., 100., .7, .35, .03, .01};
  cg::WeightBudget budget;
  budget.max_normal_functions = 3;
  cg::WeightWork work;
  const auto result = cg::cubic_weight_control(request, cg::WeightPrecision::Decimal100,
                                              budget, work);
  EXPECT_FALSE(result);
  EXPECT_EQ(work.moment_attempts, 1u);
  EXPECT_EQ(work.normal_functions, 2u);
  EXPECT_EQ(work.polynomial_terms, 0u);
  const auto invalid = cg::cubic_weight_control(request, static_cast<cg::WeightPrecision>(8),
                                               {}, work);
  EXPECT_FALSE(invalid);
  EXPECT_EQ(work.moment_attempts, 0u);
  EXPECT_EQ(work.normal_functions, 0u);
  EXPECT_EQ(work.decimal_digits, 0u);
}
} // namespace
