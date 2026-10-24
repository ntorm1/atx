#include "atx/vol/api/pricing/american_markov_grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american_markov.hpp"
#include "atx/vol/api/pricing/black76.hpp"

namespace atx::vol {
namespace {

TEST(AmericanMarkovGrid, ExactCountBoundariesAndSpotForAsymmetricDomains) {
  for (const std::size_t count : {3U, 4U, 65U, 128U, 257U, 4097U}) {
    for (const double upper : {1.01, 2.0, 4.0, 19.0}) {
      AmericanMarkovSinhGridConfig config{count, upper, 0.02, kAmericanMarkovMaxStates};
      const auto grid = make_american_markov_sinh_grid(87.13, config);
      ASSERT_TRUE(grid) << grid.error().to_string();
      ASSERT_EQ(grid->states.size(), count);
      EXPECT_DOUBLE_EQ(grid->states.front(), 0.0);
      EXPECT_DOUBLE_EQ(grid->states.back(), 87.13 * upper);
      ASSERT_GT(grid->initial_state_index, 0U);
      ASSERT_LT(grid->initial_state_index, count - 1);
      EXPECT_EQ(grid->states[grid->initial_state_index], 87.13);
      EXPECT_EQ(std::count(grid->states.begin(), grid->states.end(), 87.13), 1);
      for (std::size_t i = 1; i < count; ++i) {
        EXPECT_TRUE(std::isfinite(grid->states[i]));
        EXPECT_GT(grid->states[i], grid->states[i - 1]);
      }
    }
  }
}

TEST(AmericanMarkovGrid, MinimumGridAndRepresentedSpacingDiagnostics) {
  const auto grid = make_american_markov_sinh_grid(20.0, {3, 4.0, 0.01, 3});
  ASSERT_TRUE(grid);
  EXPECT_EQ(grid->states, (std::vector<double>{0.0, 20.0, 80.0}));
  EXPECT_DOUBLE_EQ(grid->lower_spot_gap_over_spot, 1.0);
  EXPECT_DOUBLE_EQ(grid->upper_spot_gap_over_spot, 3.0);
  EXPECT_DOUBLE_EQ(grid->min_gap_over_spot, 1.0);
  EXPECT_DOUBLE_EQ(grid->max_unit_volatility_exit_rate, 1.0 / 3.0);
}

TEST(AmericanMarkovGrid, ConcentrationNarrowsSpotSpacingAndRevealsCflCost) {
  const auto wide = make_american_markov_sinh_grid(100.0, {129, 4.0, 0.1, 129});
  const auto narrow = make_american_markov_sinh_grid(100.0, {129, 4.0, 0.001, 129});
  ASSERT_TRUE(wide);
  ASSERT_TRUE(narrow);
  EXPECT_LT(narrow->lower_spot_gap_over_spot, 0.1 * wide->lower_spot_gap_over_spot);
  EXPECT_LT(narrow->upper_spot_gap_over_spot, 0.1 * wide->upper_spot_gap_over_spot);
  EXPECT_GT(narrow->max_unit_volatility_exit_rate, 100.0 * wide->max_unit_volatility_exit_rate);
}

TEST(AmericanMarkovGrid, ShapeAndDiagnosticsAreCovariantAcrossStockScales) {
  const auto reference = make_american_markov_sinh_grid(1.0);
  ASSERT_TRUE(reference);
  for (const double spot : {1.0e-150, 87.13, 1.0e150}) {
    const auto grid = make_american_markov_sinh_grid(spot);
    ASSERT_TRUE(grid) << grid.error().to_string();
    ASSERT_EQ(grid->initial_state_index, reference->initial_state_index);
    ASSERT_EQ(grid->states.size(), reference->states.size());
    for (std::size_t i = 0; i < grid->states.size(); ++i) {
      EXPECT_NEAR(grid->states[i] / spot, reference->states[i], 1.0e-15);
    }
    EXPECT_NEAR(grid->min_gap_over_spot, reference->min_gap_over_spot, 5.0e-16);
    EXPECT_NEAR(grid->max_unit_volatility_exit_rate /
                    reference->max_unit_volatility_exit_rate,
                1.0, 1.0e-12);
  }
}

TEST(AmericanMarkovGrid, RejectsInvalidInputsAndCountsBeforeAllocation) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const double spot : {0.0, -1.0, nan, inf}) {
    EXPECT_FALSE(make_american_markov_sinh_grid(spot));
  }
  for (const double width : {0.0, -1.0, nan, inf}) {
    EXPECT_FALSE(make_american_markov_sinh_grid(100.0, {129, 4.0, width, 129}));
  }
  for (const double upper : {0.0, 1.0, -1.0, nan, inf}) {
    EXPECT_FALSE(make_american_markov_sinh_grid(100.0, {129, upper, 0.05, 129}));
  }
  EXPECT_FALSE(make_american_markov_sinh_grid(100.0, {2, 4.0, 0.05, 3}));
  EXPECT_FALSE(make_american_markov_sinh_grid(100.0, {3, 4.0, 0.05, 2}));
  EXPECT_FALSE(make_american_markov_sinh_grid(100.0, {4, 4.0, 0.05, 3}));
  EXPECT_FALSE(make_american_markov_sinh_grid(
      100.0, {3, 4.0, 0.05, kAmericanMarkovMaxStates + 1}));
  EXPECT_FALSE(make_american_markov_sinh_grid(
      100.0, {std::numeric_limits<std::size_t>::max(), 4.0, 0.05, 1025}));
}

TEST(AmericanMarkovGrid, RefusesUnrepresentableGeometryWithoutChangingWidthOrCount) {
  EXPECT_FALSE(make_american_markov_sinh_grid(std::numeric_limits<double>::max()));
  EXPECT_FALSE(make_american_markov_sinh_grid(std::numeric_limits<double>::denorm_min()));
  EXPECT_FALSE(make_american_markov_sinh_grid(100.0, {129, 4.0, 1.0e-300, 129}));
  EXPECT_FALSE(make_american_markov_sinh_grid(
      100.0, {129, 4.0, std::numeric_limits<double>::denorm_min(), 129}));
  EXPECT_FALSE(make_american_markov_sinh_grid(
      100.0, {129, std::nextafter(1.0, 2.0), 1.0e-16, 129}));
}

TEST(AmericanMarkovGrid, NearExpiryPricesConvergeToIndependentEuropeanFormula) {
  // With zero rate/yield, American calls and puts equal their European limits.
  // This isolating control uses no vendor prices and checks multiple strikes.
  constexpr double variance = 1.0 / 252.0;
  constexpr double sigma = 0.02;
  const std::array intervals{AmericanMarkovInterval{7.0 / 365.0, variance, 0.0, 0.0,
                                                  sigma, 0.0}};
  const auto uniform = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(uniform);
  AmericanMarkovWorkspace uniform_workspace;
  ASSERT_TRUE(uniform_workspace.prepare(*uniform));
  const auto uniform_atm = price_american_markov(*uniform, 100.0, Side::Put, 0,
                                                uniform_workspace);
  ASSERT_TRUE(uniform_atm);
  const double atm_expected = black76_price(100.0, 100.0, variance, sigma, 1.0, Side::Put);
  std::array<double, 2> maximum_errors{};
  for (std::size_t pass = 0; pass < 2; ++pass) {
    const std::size_t count = pass == 0 ? 65U : 257U;
    const auto grid = make_american_markov_sinh_grid(
        100.0, {count, 4.0, sigma * std::sqrt(variance), 1025});
    ASSERT_TRUE(grid);
    const auto model = compile_american_markov({100.0, intervals, grid->states, {}});
    ASSERT_TRUE(model) << model.error().to_string();
    AmericanMarkovWorkspace workspace;
    ASSERT_TRUE(workspace.prepare(*model));
    for (const double strike : {99.85, 99.95, 100.0, 100.05, 100.15}) {
      for (const auto side : {Side::Call, Side::Put}) {
        const auto value = price_american_markov(*model, strike, side, 0, workspace);
        ASSERT_TRUE(value);
        const double expected = black76_price(100.0, strike, variance, sigma, 1.0, side);
        maximum_errors[pass] = std::max(maximum_errors[pass], std::abs(*value - expected));
      }
    }
    EXPECT_LT(model->diagnostics().terminal_upper_probability, 1.0e-12);
    EXPECT_LT(model->diagnostics().max_relative_mean_error, 1.0e-14);
  }
  EXPECT_LT(maximum_errors[1], maximum_errors[0]);
  EXPECT_LT(maximum_errors[1], 1.0e-4);
  EXPECT_LT(maximum_errors[1], 0.01 * std::abs(*uniform_atm - atm_expected));
  RecordProperty("uniform_atm_error", std::abs(*uniform_atm - atm_expected));
  RecordProperty("sinh65_max_error", maximum_errors[0]);
  RecordProperty("sinh257_max_error", maximum_errors[1]);
}

TEST(AmericanMarkovGrid, SeparateClocksNegativeRatesAndBroadExpiryControl) {
  const std::array intervals{AmericanMarkovInterval{0.5, 0.25, -0.03, -0.01, 0.2, 0.0}};
  const auto grid = make_american_markov_sinh_grid(100.0, {129, 4.0, 0.1, 129});
  ASSERT_TRUE(grid);
  const auto model = compile_american_markov({100.0, intervals, grid->states, {}});
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  for (const double strike : {80.0, 95.0, 100.0, 105.0, 120.0}) {
    for (const auto side : {Side::Call, Side::Put}) {
      const auto value = price_american_markov(*model, strike, side, 0, workspace,
                                               ExerciseStyle::European);
      ASSERT_TRUE(value);
      const double expected = black76_price(100.0 * std::exp(-0.01), strike, 0.25, 0.2,
                                             std::exp(0.015), side);
      EXPECT_NEAR(*value, expected, 0.004);
    }
  }
}

TEST(AmericanMarkovGrid, CashAndLocalVolatilityKeepPositiveMartingaleRows) {
  const std::array local{AmericanMarkovLocalVolNode{-0.3, 1.3},
                         AmericanMarkovLocalVolNode{0.0, 1.0},
                         AmericanMarkovLocalVolNode{0.3, 0.8}};
  const std::array intervals{AmericanMarkovInterval{0.02, 0.01, -0.03, 0.01, 0.2, 2.0, local},
                             AmericanMarkovInterval{0.05, 0.03, 0.02, 0.0, 0.3, 0.0, local}};
  const auto grid = make_american_markov_sinh_grid(100.0);
  ASSERT_TRUE(grid);
  const auto model = compile_american_markov({100.0, intervals, grid->states, {}});
  ASSERT_TRUE(model) << model.error().to_string();
  for (std::size_t n = 0; n < model->transitions().size(); ++n) {
    const auto &step = model->transitions()[n];
    const auto &before = model->layers()[n];
    const auto &after = model->layers()[n + 1];
    for (std::size_t i = 0; i < step.rows().size(); ++i) {
      double probability = 0.0;
      double mean = 0.0;
      for (std::size_t j = 0; j < 3; ++j) {
        EXPECT_GE(step.rows()[i].probability[j], 0.0);
        probability += step.rows()[i].probability[j];
        mean += step.rows()[i].probability[j] * after.states()[step.rows()[i].destination[j]];
      }
      EXPECT_NEAR(probability, 1.0, 4.0e-16);
      if (step.kind == AmericanMarkovStepKind::Diffusion) {
        EXPECT_NEAR(mean, before.states()[i], 1.0e-12);
      } else {
        const double stock = before.growth * before.states()[i];
        EXPECT_NEAR(after.growth * mean + std::min(stock, step.cash_dividend), stock, 1.0e-12);
      }
    }
  }
}

TEST(AmericanMarkovGrid, PausedVarianceAndLimitedLiabilityKeepDeterministicExercise) {
  const std::array intervals{AmericanMarkovInterval{0.25, 0.0, 0.0, 0.0, 9.0, 10.0},
                             AmericanMarkovInterval{0.5, 0.0, 0.0, 0.0, 9.0, 1000.0}};
  const auto grid = make_american_markov_sinh_grid(100.0);
  ASSERT_TRUE(grid);
  const auto model = compile_american_markov({100.0, intervals, grid->states, {}});
  ASSERT_TRUE(model);
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  const auto call = price_american_markov(*model, 80.0, Side::Call, 0, workspace);
  const auto put = price_american_markov(*model, 100.0, Side::Put, 1, workspace);
  ASSERT_TRUE(call);
  ASSERT_TRUE(put);
  EXPECT_DOUBLE_EQ(*call, 20.0);
  EXPECT_DOUBLE_EQ(*put, 100.0);
  EXPECT_DOUBLE_EQ(model->diagnostics().terminal_zero_probability, 1.0);
}

TEST(AmericanMarkovGrid, CompilerKeepsTransitionAndStorageLimitsForNarrowGrids) {
  const auto grid = make_american_markov_sinh_grid(100.0, {129, 4.0, 0.001, 129});
  ASSERT_TRUE(grid);
  const std::array intervals{AmericanMarkovInterval{1.0, 1.0, 0.04, 0.0, 0.8, 0.0}};
  const auto model = compile_american_markov({100.0, intervals, grid->states, {}});
  EXPECT_FALSE(model);
  const std::array short_intervals{AmericanMarkovInterval{0.01, 0.01, 0.0, 0.0, 0.02, 0.0}};
  AmericanMarkovConfig config;
  config.max_stored_rows = grid->states.size();
  const auto insufficient = compile_american_markov({100.0, short_intervals, grid->states, config});
  EXPECT_FALSE(insufficient);
}

} // namespace
} // namespace atx::vol
