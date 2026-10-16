#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "atx/vol/api/pricing/american_markov_pwl.hpp"

namespace atx::vol {
namespace {

[[nodiscard]] Result<AmericanMarkovModel> one_step_model() {
  const std::array grid{0.0, 80.0, 100.0, 120.0, 200.0};
  const std::array intervals{AmericanMarkovInterval{0.01, 0.01, 0.0, 0.0, 0.2, 0.0}};
  return compile_american_markov({100.0, intervals, grid, {}});
}

[[nodiscard]] Result<AmericanMarkovModel> multiple_expiry_model() {
  const std::array grid{0.0, 70.0, 100.0, 130.0, 220.0};
  const std::array intervals{AmericanMarkovInterval{0.02, 0.01, -0.03, 0.01, 0.2, 3.0},
                             AmericanMarkovInterval{0.06, 0.04, 0.02, 0.01, 0.25, 0.0},
                             AmericanMarkovInterval{0.10, 0.07, -0.02, 0.0, 0.2, 2.0}};
  AmericanMarkovSpec spec{100.0, intervals, grid, {}};
  spec.config.max_calendar_step = 0.025;
  spec.config.max_variance_step = 0.025;
  return compile_american_markov(spec);
}

TEST(AmericanMarkovPwl, OneStepMatchesIndependentDistributionAtOffGridStrikes) {
  auto model = one_step_model();
  ASSERT_TRUE(model.has_value());
  const auto surface = compile_american_markov_pwl(std::move(*model), 50.0, 150.0);
  ASSERT_TRUE(surface.has_value()) << surface.error().to_string();
  ASSERT_EQ(surface->slices().size(), 1u);
  for (const Side side : {Side::Call, Side::Put}) {
    for (int i = 0; i <= 800; ++i) {
      const double strike = 50.0 + 0.125 * static_cast<double>(i);
      const auto intrinsic = [side, strike](double stock) {
        return std::max(side == Side::Call ? stock - strike : strike - stock, 0.0);
      };
      const double expected = 0.005 * intrinsic(80.0) + 0.99 * intrinsic(100.0) +
                              0.005 * intrinsic(120.0);
      const auto price = surface->price(strike, side, 0u);
      ASSERT_TRUE(price.has_value());
      EXPECT_NEAR(*price, expected, 3.0e-14);
    }
  }
  const auto &diagnostics = surface->diagnostics();
  EXPECT_GT(diagnostics.algebra_operations, 0u);
  EXPECT_GT(diagnostics.input_nodes, 0u);
  EXPECT_GE(diagnostics.generated_nodes, diagnostics.peak_live_nodes);
  EXPECT_GE(diagnostics.peak_live_nodes, diagnostics.output_nodes);
  EXPECT_EQ(diagnostics.output_nodes,
            surface->slices()[0].call.nodes().size() + surface->slices()[0].put.nodes().size());
}

TEST(AmericanMarkovPwl, SharedCashAndRateModelMatchesScalarOffNodesAndAllCalendars) {
  auto model = multiple_expiry_model();
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const auto surface = compile_american_markov_pwl(std::move(*model), 45.0, 170.0);
  ASSERT_TRUE(surface.has_value()) << surface.error().to_string();
  ASSERT_EQ(surface->slices().size(), 3u);
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(surface->source_model()));
  for (std::size_t expiry = 0u; expiry < surface->slices().size(); ++expiry) {
    const auto &slice = surface->slices()[expiry];
    EXPECT_EQ(slice.expiry_index, expiry);
    EXPECT_EQ(slice.terminal_layer, surface->source_model().expiry_layers()[expiry]);
    EXPECT_EQ(slice.calendar_time,
              surface->source_model().layers()[slice.terminal_layer].calendar_time);
    EXPECT_EQ(slice.variance_time,
              surface->source_model().layers()[slice.terminal_layer].variance_time);
    for (const Side side : {Side::Call, Side::Put}) {
      double previous_change = -1.0e10;
      double previous_price = *surface->price(45.0, side, expiry);
      for (int i = 1; i <= 500; ++i) {
        const double strike = 45.0 + 0.25 * static_cast<double>(i);
        const auto scalar = price_american_markov(surface->source_model(), strike, side,
                                                   expiry, workspace);
        ASSERT_TRUE(scalar.has_value());
        const double price = *surface->price(strike, side, expiry);
        EXPECT_NEAR(price, *scalar, 2.0e-12);
        const double change = price - previous_price;
        EXPECT_GE(change, previous_change - 2.0e-12);
        EXPECT_GE(price + 2.0e-12,
                  std::max(side == Side::Call ? 100.0 - strike : strike - 100.0, 0.0));
        if (side == Side::Call) EXPECT_LE(change, 2.0e-12);
        else EXPECT_GE(change, -2.0e-12);
        for (std::size_t later = expiry + 1u; later < surface->slices().size(); ++later) {
          EXPECT_GE(*surface->price(strike, side, later) + 2.0e-12, price);
        }
        previous_change = change;
        previous_price = price;
      }
    }
  }
}

TEST(AmericanMarkovPwl, EndpointCashAndNegativeRatesHaveIndependentDeterministicValues) {
  const std::array grid{0.0, 50.0, 100.0, 150.0, 200.0};
  const std::array intervals{AmericanMarkovInterval{0.25, 0.0, 0.0, 0.0, 0.0, 10.0},
                             AmericanMarkovInterval{0.5, 0.0, 0.0, 0.0, 0.0, 1000.0},
                             AmericanMarkovInterval{0.75, 0.0, -0.04, 0.0, 0.0, 0.0}};
  AmericanMarkovSpec spec{100.0, intervals, grid, {}};
  spec.config.max_calendar_step = 0.25;
  auto model = compile_american_markov(spec);
  ASSERT_TRUE(model.has_value());
  const auto surface = compile_american_markov_pwl(std::move(*model), 50.0, 150.0);
  ASSERT_TRUE(surface.has_value()) << surface.error().to_string();
  for (const double strike : {75.25, 99.75, 100.25, 125.25}) {
    EXPECT_NEAR(*surface->price(strike, Side::Call, 0u), std::max(100.0 - strike, 0.0), 1.0e-13);
    EXPECT_NEAR(*surface->price(strike, Side::Put, 0u), std::max(strike - 90.0, 0.0), 1.0e-13);
    EXPECT_NEAR(*surface->price(strike, Side::Call, 1u), std::max(100.0 - strike, 0.0), 1.0e-13);
    EXPECT_NEAR(*surface->price(strike, Side::Put, 1u), strike, 1.0e-13);
    EXPECT_NEAR(*surface->price(strike, Side::Put, 2u), strike * std::exp(0.01), 1.0e-12);
  }
  EXPECT_EQ(surface->source_model().layers().back().states().size(), 1u);
}

TEST(AmericanMarkovPwl, DefaultMartingaleRetainsBoundedCrossingsAcrossFullRollback) {
  const std::array intervals{AmericanMarkovInterval{0.25, 0.25, 0.0, 0.0, 0.2, 0.0}};
  auto model = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(model);
  ASSERT_EQ(model->diagnostics().max_states, 65u);
  const auto surface = compile_american_markov_pwl(std::move(*model), 40.0, 180.0);
  ASSERT_TRUE(surface) << surface.error().to_string();
  const auto &slice = surface->slices()[0];
  const double call_error = slice.call.crossing_resolution_error_bound();
  const double put_error = slice.put.crossing_resolution_error_bound();
  EXPECT_GT(std::max(call_error, put_error), 0.0);
  EXPECT_EQ(surface->diagnostics().max_output_crossing_resolution_error,
            std::max(call_error, put_error));
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(surface->source_model()));
  for (const Side side : {Side::Call, Side::Put}) {
    const auto &function = side == Side::Call ? slice.call : slice.put;
    double largest_value = 1.0;
    for (const auto &node : function.nodes()) largest_value = std::max(largest_value, node.value);
    const double allowance = 64.0 * std::numeric_limits<double>::epsilon() * largest_value;
    EXPECT_LE(function.crossing_resolution_error_bound(), allowance);
    for (int i = 0; i <= 1120; ++i) {
      const double strike = 40.0 + 0.125 * static_cast<double>(i);
      const auto scalar = price_american_markov(
          surface->source_model(), strike, side, 0, workspace);
      ASSERT_TRUE(scalar);
      // The exposed bound excludes ordinary arithmetic roundoff; do not claim
      // it alone bounds the two differently ordered floating-point recursions.
      EXPECT_NEAR(*function.value(strike), *scalar, 3.0e-12);
    }
    for (const auto &node : function.nodes()) {
      const auto scalar = price_american_markov(
          surface->source_model(), node.strike, side, 0, workspace);
      ASSERT_TRUE(scalar);
      EXPECT_NEAR(node.value, *scalar, 3.0e-12);
    }
  }
}

TEST(AmericanMarkovPwl, AggregateBudgetsFailClosedWithoutPartialSurfaces) {
  const auto model = one_step_model();
  ASSERT_TRUE(model.has_value());
  for (int budget = 0; budget != 5; ++budget) {
    AmericanMarkovPwlLimits limits;
    if (budget == 0) limits.max_live_nodes = 2u;
    if (budget == 1) limits.max_generated_nodes = 2u;
    if (budget == 2) limits.max_input_nodes = 2u;
    if (budget == 3) limits.max_operations = 1u;
    if (budget == 4) limits.algebra.max_nodes = 2u;
    const auto surface = compile_american_markov_pwl(*model, 50.0, 150.0, limits);
    ASSERT_FALSE(surface.has_value());
    EXPECT_EQ(surface.error().code(), ErrorCode::Unavailable);
  }
  const auto multiple = multiple_expiry_model();
  ASSERT_TRUE(multiple.has_value());
  AmericanMarkovPwlLimits limits;
  limits.max_expiries = 1u;
  const auto surface = compile_american_markov_pwl(*multiple, 50.0, 150.0, limits);
  ASSERT_FALSE(surface.has_value());
  EXPECT_EQ(surface.error().code(), ErrorCode::Unavailable);
}

TEST(AmericanMarkovPwl, RejectsInvalidConfigurationMovedModelsAndUnsupportedQueries) {
  auto model = one_step_model();
  ASSERT_TRUE(model.has_value());
  AmericanMarkovPwlLimits limits;
  limits.algebra.max_nodes = kPwlStrikeMaxNodes + 1u;
  const auto bad_algebra = compile_american_markov_pwl(*model, 50.0, 150.0, limits);
  ASSERT_FALSE(bad_algebra.has_value());
  EXPECT_EQ(bad_algebra.error().code(), ErrorCode::InvalidArgument);
  EXPECT_FALSE(compile_american_markov_pwl(*model, 0.0, 150.0));
  EXPECT_FALSE(compile_american_markov_pwl(*model, 150.0, 150.0));
  EXPECT_FALSE(compile_american_markov_pwl(*model, 50.0,
                                         std::numeric_limits<double>::infinity()));
  const auto surface = compile_american_markov_pwl(std::move(*model), 50.0, 150.0);
  ASSERT_TRUE(surface.has_value());
  EXPECT_FALSE(compile_american_markov_pwl(std::move(*model), 50.0, 150.0));
  EXPECT_FALSE(surface->price(49.0, Side::Call, 0u));
  EXPECT_FALSE(surface->price(151.0, Side::Call, 0u));
  EXPECT_FALSE(surface->price(100.0, static_cast<Side>(255), 0u));
  EXPECT_FALSE(surface->price(100.0, Side::Call, 1u));
  auto copy = *surface;
  const auto moved = std::move(copy);
  EXPECT_EQ(moved.price(100.0, Side::Call, 0u), surface->price(100.0, Side::Call, 0u));
  EXPECT_FALSE(copy.price(100.0, Side::Call, 0u));
}

} // namespace
} // namespace atx::vol
