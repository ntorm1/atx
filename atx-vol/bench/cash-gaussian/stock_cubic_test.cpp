#include "stock_cubic.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <boost/multiprecision/cpp_bin_float.hpp>
#include <gtest/gtest.h>

namespace atx::vol::cash_gaussian {
namespace {
using R = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<200>>;
constexpr std::array precisions{WeightPrecision::Decimal100, WeightPrecision::Decimal200};

TEST(CashStockCubic, AdjacentAffineKeepsEndpointAndInteriorDerivatives) {
  for (const double lower : {1.e12, 1.e-12, 1.}) {
    const double upper = std::nextafter(lower, std::numeric_limits<double>::infinity());
    const std::array cells{HermiteCell{0., lower, 0., lower, 1., 1.},
        HermiteCell{lower, upper, lower, upper, 1., 1.}};
    CubicOwnerWork work;
    auto curve = StockCubic::make(cells, {upper, upper, 1.}, {}, work);
    ASSERT_TRUE(curve) << curve.error().message();
    for (const auto precision : precisions) for (const double u : {0., .25, .5, .75, 1.}) {
      auto result = cubic_cell_coordinate(*curve, 1, u, precision, {}, work);
      ASSERT_TRUE(result) << result.error().message();
      EXPECT_EQ(R(result->left_derivative), 1);
      EXPECT_EQ(R(result->right_derivative), 1);
      EXPECT_LT(abs(R(result->value) - (R(lower) + R(u)*(R(upper)-R(lower)))),
                R("1e-85") * R(lower));
    }
  }
}

TEST(CashStockCubic, AbsoluteControlCounterexampleIsRetained) {
  const double a = 1.e12, b = std::nextafter(a, std::numeric_limits<double>::infinity());
  const double h = b-a, b1 = a+h/3., b2 = b-h/3.;
  EXPECT_EQ(3.*(b1-a)/h, 0.);
  EXPECT_EQ(3.*((b1-a)*.25 + (b2-b1)*.5 + (b-b2)*.25)/h, 1.5);
}

TEST(CashStockCubic, OwnsSnapshotsAndRejectsDiscontinuousValues) {
  std::vector<HermiteCell> cells{{0., 1., -3., -1., 2., 2.},
      {1., 2., -1., 3., 4., 4.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {2., 3., 4.}, {}, work);
  ASSERT_TRUE(curve);
  cells[0].value_lower = 99.;
  const auto knot = cubic_point(*curve, 1., WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(knot);
  EXPECT_EQ(R(knot->value), -1);
  EXPECT_EQ(knot->slope_kind, CubicSlopeKind::Kink);
  EXPECT_EQ(R(knot->left_derivative), 2);
  EXPECT_EQ(R(knot->right_derivative), 4);
  auto endpoint = cubic_cell_coordinate(*curve, 0, 1., WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint->slope_kind, CubicSlopeKind::Kink);
  cells[1].value_lower = std::nextafter(-1., 0.);
  EXPECT_FALSE(StockCubic::make(cells, {2., 3., 4.}, {}, work));
}

TEST(CashStockCubic, ZeroCurveAndRightDerivativeAreExplicit) {
  const std::array cells{HermiteCell{0., 1., 0., 0., 0., 0.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {1., 0., 0.}, {}, work);
  ASSERT_TRUE(curve);
  for (const auto precision : precisions) for (const double x : {0., .5, 1., 2.}) {
    auto point = cubic_point(*curve, x, precision, {}, work);
    ASSERT_TRUE(point);
    EXPECT_EQ(R(point->value), 0);
    EXPECT_EQ(R(point->right_derivative), 0);
    EXPECT_EQ(R(point->value_absolute_sum), 0);
    EXPECT_EQ(point->slope_kind, x == 0. ? CubicSlopeKind::RightLimit : CubicSlopeKind::Smooth);
    if (x == 0.) EXPECT_TRUE(point->left_derivative.empty());
  }
}

TEST(CashStockCubic, SignedHermiteUsesIndependentPolynomialDerivative) {
  const std::array cells{HermiteCell{0., 2., -3., 4., -2., 5.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {2., 4., 5.}, {}, work);
  ASSERT_TRUE(curve);
  // Independent power coefficients: f(x)=-3-2x+4.75x^2-x^3.
  for (const auto precision : precisions) for (const double x : {.25, .75, 1.25, 1.75}) {
    const R z(x), expected = -3 - 2*z + R("4.75")*z*z - z*z*z;
    const R derivative = -2 + R("9.5")*z - R(3)*z*z;
    auto point = cubic_point(*curve, x, precision, {}, work);
    ASSERT_TRUE(point);
    EXPECT_LT(abs(R(point->value)-expected), R("1e-85"));
    EXPECT_LT(abs(R(point->right_derivative)-derivative), R("1e-85"));
  }
}

TEST(CashStockCubic, FiniteContributionKeepsOffsetAndAffineDelta) {
  const std::array cells{HermiteCell{0., 90., 2., 92., 1., 1.},
      HermiteCell{90., 110., 92., 112., 1., 1.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {110., 112., 1.}, {}, work);
  ASSERT_TRUE(curve);
  const WeightRequest request{90., 110., 100., .5, .2, .03, .01};
  for (const auto precision : precisions) {
    auto result = cubic_cell_contribution(*curve, 1, request, precision, {}, work);
    ASSERT_TRUE(result) << result.error().message();
    LocalWeightWork other_work;
    auto weights = cubic_local_weight_control(request, precision, {}, other_work);
    ASSERT_TRUE(weights);
    const auto &w = weights->weights;
    const R expected = 2*R(w.mass) + 110*R(w.scaled_moments[1]);
    EXPECT_LT(abs(R(result->value)-expected), R("1e-85"));
    EXPECT_LT(abs(R(result->delta)-R(w.discounted_stock_ratio)), R("1e-85"));
    EXPECT_EQ(work.finite_cells, 1U);
    EXPECT_EQ(work.weights.moments.moment_attempts, 4U);
  }
}

TEST(CashStockCubic, BudgetsAndMalformedInputsRefuseBeforeArithmetic) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 1., 1.}};
  CubicOwnerWork work;
  auto low = CubicOwnerBudget{};
  low.max_workspace_bytes = 0;
  EXPECT_FALSE(StockCubic::make(cells, {1., 1., 1.}, low, work));
  EXPECT_EQ(work.cells_copied, 0U);
  auto curve = StockCubic::make(cells, {1., 1., 1.}, {}, work);
  ASSERT_TRUE(curve);
  EXPECT_FALSE(cubic_cell_coordinate(*curve, 0, .5, WeightPrecision::Decimal100, low, work));
  EXPECT_EQ(work.conversions, 0U);
  EXPECT_FALSE(cubic_point(*curve, -1., WeightPrecision::Decimal100, {}, work));
  EXPECT_FALSE(cubic_cell_coordinate(*curve, 1, .5, WeightPrecision::Decimal100, {}, work));
  EXPECT_FALSE(cubic_cell_coordinate(*curve, 0, 1.1, WeightPrecision::Decimal100, {}, work));
  EXPECT_FALSE(StockCubic::make(cells, {1., 2., 1.}, {}, work));
}

TEST(CashStockCubic, CancellationRefusalAndExactConstantZeroAreDistinct) {
  const std::array crossing{HermiteCell{0., 1., -1., 1., 2., 2.}};
  const std::array constant{HermiteCell{0., 1., -2., -2., 0., 0.}};
  CubicOwnerWork work;
  auto cross = StockCubic::make(crossing, {1., 1., 2.}, {}, work);
  auto flat = StockCubic::make(constant, {1., -2., 0.}, {}, work);
  ASSERT_TRUE(cross);
  ASSERT_TRUE(flat);
  for (const auto precision : precisions) {
    EXPECT_FALSE(cubic_cell_coordinate(*cross, 0, .5, precision, {}, work));
    EXPECT_GT(work.accumulation_terms, 0U);
    auto value = cubic_cell_coordinate(*flat, 0, .5, precision, {}, work);
    ASSERT_TRUE(value);
    EXPECT_EQ(R(value->value), -2);
    EXPECT_EQ(R(value->right_derivative), 0);
    ASSERT_EQ(work.coefficient_records, 2U);
    EXPECT_TRUE(work.coefficients[1].identical_input_zero);
    EXPECT_EQ(R(work.coefficients[1].absolute_sum), 0);
  }
}

TEST(CashStockCubic, WeightedFailureKeepsActualPartialWorkAndResetsNextCall) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 1., 1.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {1., 1., 1.}, {}, work);
  ASSERT_TRUE(curve);
  const WeightRequest r{0., 1., 1., .5, .2, .03, .01};
  auto budget = CubicOwnerBudget{};
  budget.max_moments = 2;
  EXPECT_FALSE(cubic_cell_contribution(*curve, 0, r, WeightPrecision::Decimal100, budget, work));
  EXPECT_EQ(work.finite_cells, 1U);
  EXPECT_EQ(work.weights.moments.moment_attempts, 2U);
  EXPECT_EQ(work.weights.moments.normal_functions, 4U);
  EXPECT_EQ(work.coefficient_records, 0U);
  auto wrong = r;
  wrong.upper = 2.;
  EXPECT_FALSE(cubic_cell_contribution(*curve, 0, wrong, WeightPrecision::Decimal100, {}, work));
  EXPECT_EQ(work.finite_cells, 0U);
  EXPECT_EQ(work.weights.moments.moment_attempts, 0U);
  EXPECT_TRUE(cubic_point(*curve, .5, WeightPrecision::Decimal100, {}, work));
  EXPECT_EQ(work.coefficient_records, 4U);
}

TEST(CashStockCubic, AdjacentWeightedAffineUsesExactLocalIncrement) {
  for (const double a : {1.e12, 1.e-12, 1.}) {
    const double b = std::nextafter(a, std::numeric_limits<double>::infinity());
    const std::array cells{HermiteCell{0., a, 0., a, 1., 1.},
        HermiteCell{a, b, a, b, 1., 1.}};
    CubicOwnerWork work;
    auto curve = StockCubic::make(cells, {b, b, 1.}, {}, work);
    ASSERT_TRUE(curve);
    const WeightRequest request{a, b, a, .5, .2, .03, .01};
    for (const auto precision : precisions) {
      auto result = cubic_cell_contribution(*curve, 1, request, precision, {}, work);
      ASSERT_TRUE(result) << result.error().message();
      EXPECT_EQ(work.weights.route, LocalWeightRoute::LocalSeries);
      ASSERT_EQ(work.coefficient_records, 3U);
      EXPECT_EQ(R(work.coefficients[1].result), R(b)-R(a));
      EXPECT_EQ(R(work.coefficients[2].result), 1);
      LocalWeightWork other_work;
      auto weights = cubic_local_weight_control(request, precision, {}, other_work);
      ASSERT_TRUE(weights);
      const R expected = R(b)*R(weights->weights.scaled_moments[1]);
      const R derivative(weights->weights.discounted_stock_ratio);
      EXPECT_LT(abs(R(result->value)-expected)/expected, R("1e-70"));
      EXPECT_LT(abs(R(result->delta)-derivative)/derivative, R("1e-70"));
    }
  }
}

TEST(CashStockCubic, PureAffineTailAndInvalidTupleRemainExplicit) {
  CubicOwnerWork work;
  auto curve = StockCubic::make({}, {0., -3., 2.}, {}, work);
  ASSERT_TRUE(curve);
  EXPECT_TRUE(curve->cells().empty());
  auto at_zero = cubic_point(*curve, 0., WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(at_zero);
  EXPECT_EQ(at_zero->slope_kind, CubicSlopeKind::RightLimit);
  auto value = cubic_point(*curve, 2., WeightPrecision::Decimal200, {}, work);
  ASSERT_TRUE(value);
  EXPECT_EQ(R(value->value), 1);
  EXPECT_EQ(R(value->right_derivative), 2);
  EXPECT_EQ(work.finite_cells, 0U);
  EXPECT_FALSE(StockCubic::make({}, {1., 0., 0.}, {}, work));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::array bad{HermiteCell{0., 1., 0., 1., nan, 1.}};
  EXPECT_FALSE(StockCubic::make(bad, {1., 1., 1.}, {}, work));
  EXPECT_FALSE(cubic_point(*curve, nan, WeightPrecision::Decimal100, {}, work));
  EXPECT_FALSE(cubic_point(*curve, 1., static_cast<WeightPrecision>(42), {}, work));
}

TEST(CashStockCubic, WeightedZeroAndProvedCubicZeroAreDistinctFromUnprovedCancellation) {
  const std::array zero{HermiteCell{0., 1., 0., 0., 0., 0.}};
  const std::array cubic{HermiteCell{0., 1., 0., 1., 0., 3.}};
  CubicOwnerWork work;
  auto z = StockCubic::make(zero, {1., 0., 0.}, {}, work);
  auto c = StockCubic::make(cubic, {1., 1., 3.}, {}, work);
  ASSERT_TRUE(z);
  ASSERT_TRUE(c);
  const WeightRequest request{0., 1., 1., .5, .2, .03, .01};
  for (const auto precision : precisions) {
    auto result = cubic_cell_contribution(*z, 0, request, precision, {}, work);
    ASSERT_TRUE(result);
    EXPECT_EQ(R(result->value), 0);
    EXPECT_EQ(R(result->delta), 0);
    EXPECT_EQ(R(result->delta_absolute_sum), 0);
    auto legacy = CubicOwnerBudget{};
    legacy.exact_zero.max_workspace_bytes = 0;
    EXPECT_FALSE(cubic_cell_contribution(*c, 0, request, precision, legacy, work));
    EXPECT_EQ(work.weights.moments.moment_attempts, 4U);
    ASSERT_EQ(work.coefficient_records, 3U);
    EXPECT_EQ(work.coefficients[2].kind, CubicCoefficient::MiddleSlope);
    EXPECT_EQ(R(work.coefficients[2].result), 0);
    EXPECT_EQ(R(work.coefficients[2].absolute_sum), 6);
    EXPECT_EQ(work.coefficients[2].condition, "unbounded");
    result = cubic_cell_contribution(*c, 0, request, precision, {}, work);
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_TRUE(work.middle_proof);
    EXPECT_TRUE(work.middle_proof->is_zero());
    EXPECT_TRUE(work.coefficients[2].exact_dyadic_zero);
    EXPECT_FALSE(work.coefficients[2].identical_input_zero);
    EXPECT_EQ(R(work.coefficients[2].absolute_sum), 0);
    LocalWeightWork weight_work;
    auto weights = cubic_local_weight_control(request, precision, {}, weight_work);
    ASSERT_TRUE(weights);
    const R expected(weights->weights.scaled_moments[3]);
    EXPECT_LT(abs(R(result->value)-expected), R("1e-85"));
    EXPECT_LT(abs(R(result->delta)-3*expected), R("1e-85"));
  }
}

TEST(CashStockCubic, RoundedZeroWithNonzeroExactNumeratorStillRefuses) {
  const std::array cells{HermiteCell{0., 1., 0., 1., 1e-300, 3.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {1., 1., 3.}, {}, work);
  ASSERT_TRUE(curve);
  const WeightRequest request{0., 1., 1., .5, .2, .03, .01};
  for (const auto precision : precisions) {
    EXPECT_FALSE(cubic_cell_contribution(*curve, 0, request, precision, {}, work));
    ASSERT_TRUE(work.middle_proof);
    EXPECT_EQ(work.middle_proof->sign, -1);
    EXPECT_EQ(work.dyadic_zero.status, DyadicZeroStatus::Nonzero);
    ASSERT_EQ(work.coefficient_records, 3U);
    EXPECT_EQ(R(work.coefficients[2].result), 0);
    EXPECT_EQ(work.coefficients[2].condition, "unbounded");
    EXPECT_FALSE(work.coefficients[2].exact_dyadic_zero);
  }
}

TEST(CashStockCubic, ExactNonzeroOrUnavailableProofPreservesOriginalOutputBits) {
  const std::array cells{HermiteCell{0., 1., -3., -1., 2., 2.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {1., -1., 2.}, {}, work);
  ASSERT_TRUE(curve);
  const WeightRequest request{0., 1., 1., .5, .2, .03, .01};
  for (const auto precision : precisions) {
    auto legacy = CubicOwnerBudget{};
    legacy.cell.max_workspace_bytes = 0; // zero-lower original moment route
    legacy.exact_zero.max_workspace_bytes = 0;
    auto original = cubic_cell_contribution(*curve, 0, request, precision, legacy, work);
    ASSERT_TRUE(original);
    const auto old_reservation = work.workspace_bytes;
    auto enabled = legacy;
    enabled.exact_zero = {};
    auto current = cubic_cell_contribution(*curve, 0, request, precision, enabled, work);
    ASSERT_TRUE(current);
    ASSERT_TRUE(work.middle_proof);
    EXPECT_FALSE(work.middle_proof->is_zero());
    EXPECT_EQ(current->value, original->value);
    EXPECT_EQ(current->delta, original->delta);
    EXPECT_EQ(current->maximum_condition, original->maximum_condition);
    EXPECT_EQ(current->value_absolute_sum, original->value_absolute_sum);
    EXPECT_EQ(current->delta_absolute_sum, original->delta_absolute_sum);
    enabled.max_workspace_bytes = old_reservation;
    current = cubic_cell_contribution(*curve, 0, request, precision, enabled, work);
    ASSERT_TRUE(current);
    EXPECT_FALSE(work.middle_proof);
    EXPECT_EQ(work.dyadic_zero.status, DyadicZeroStatus::WorkspaceRefused);
    EXPECT_EQ(work.workspace_bytes, old_reservation);
    EXPECT_EQ(current->value, original->value);
    EXPECT_EQ(current->delta, original->delta);
  }
}

TEST(CashStockCubic, NegativeCubicProofPreservesSignedValueAndDelta) {
  const std::array cells{HermiteCell{0., 1., 0., -1., 0., -3.}};
  CubicOwnerWork work;
  auto curve = StockCubic::make(cells, {1., -1., -3.}, {}, work);
  ASSERT_TRUE(curve);
  const WeightRequest request{0., 1., 1., .5, .2, .03, .01};
  for (const auto precision : precisions) {
    auto result = cubic_cell_contribution(*curve, 0, request, precision, {}, work);
    ASSERT_TRUE(result);
    ASSERT_TRUE(work.middle_proof);
    EXPECT_TRUE(work.middle_proof->is_zero());
    LocalWeightWork weight_work;
    auto weights = cubic_local_weight_control(request, precision, {}, weight_work);
    ASSERT_TRUE(weights);
    const R expected = -R(weights->weights.scaled_moments[3]);
    EXPECT_LT(abs(R(result->value)-expected), R("1e-85"));
    EXPECT_LT(abs(R(result->delta)-3*expected), R("1e-85"));
  }
}

TEST(CashStockCubic, ReconstructedOwnerDoesNotBorrowOriginalStorage) {
  const std::array cells{HermiteCell{0., 1., -3., -1., 2., 2.}};
  CubicOwnerWork work;
  auto original = StockCubic::make(cells, {1., -1., 2.}, {}, work);
  ASSERT_TRUE(original);
  auto snapshot = StockCubic::make(original->cells(), original->tail(), {}, work);
  ASSERT_TRUE(snapshot);
  original = StockCubic::make({}, {0., 99., 0.}, {}, work);
  auto retained = cubic_point(*snapshot, .25, WeightPrecision::Decimal100, {}, work);
  ASSERT_TRUE(retained);
  EXPECT_EQ(R(retained->value), R("-2.5"));
  EXPECT_EQ(R(retained->right_derivative), 2);
}
} // namespace
} // namespace atx::vol::cash_gaussian
