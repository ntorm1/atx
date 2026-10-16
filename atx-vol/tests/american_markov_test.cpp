#include "atx/vol/api/pricing/american_markov.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"

namespace atx::vol {
namespace {

double price(const AmericanMarkovModel &model, AmericanMarkovWorkspace &workspace, double strike,
             Side side, std::size_t expiry = 0, ExerciseStyle exercise = ExerciseStyle::American) {
  auto value = price_american_markov(model, strike, side, expiry, workspace, exercise);
  EXPECT_TRUE(value) << value.error().to_string();
  return value ? *value : std::numeric_limits<double>::quiet_NaN();
}

TEST(AmericanMarkov, OneStepMatchesIndependentFiniteDistribution) {
  const std::array grid{0.0, 80.0, 100.0, 120.0, 200.0};
  const std::array intervals{AmericanMarkovInterval{0.01, 0.01, 0.0, 0.0, 0.2, 0.0}};
  AmericanMarkovSpec spec{100.0, intervals, grid, {}};
  auto model = compile_american_markov(spec);
  ASSERT_TRUE(model) << model.error().to_string();
  ASSERT_EQ(model->transitions().size(), 1U);
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  // Variance is 4: on states80/100/120 the two jump probabilities are .005.
  EXPECT_NEAR(price(*model, workspace, 100.0, Side::Call), 0.1, 1.0e-14);
  EXPECT_NEAR(price(*model, workspace, 100.0, Side::Put), 0.1, 1.0e-14);
  EXPECT_NEAR(price(*model, workspace, 110.0, Side::Call), 0.05, 1.0e-14);
}

TEST(AmericanMarkov, PositiveTransitionsPreserveMomentsAndCashWealth) {
  const std::array grid{0.0, 35.0, 80.0, 100.0, 130.0, 240.0, 400.0};
  const std::array intervals{AmericanMarkovInterval{0.13, 0.07, -0.03, -0.01, 0.3, 5.0},
                             AmericanMarkovInterval{0.29, 0.15, 0.02, 0.01, 0.25, 0.0}};
  auto model = compile_american_markov({100.0, intervals, grid, {}});
  ASSERT_TRUE(model) << model.error().to_string();
  for (std::size_t n = 0; n < model->transitions().size(); ++n) {
    const auto &step = model->transitions()[n];
    const auto &before = model->layers()[n];
    const auto &after = model->layers()[n + 1];
    EXPECT_GT(step.discount, 0.0);
    ASSERT_EQ(step.rows.size(), before.states.size());
    for (std::size_t i = 0; i < step.rows.size(); ++i) {
      const auto &row = step.rows[i];
      double sum = 0.0;
      double mean = 0.0;
      double variance = 0.0;
      for (std::size_t j = 0; j < 3; ++j) {
        ASSERT_LT(row.destination[j], after.states.size());
        EXPECT_GE(row.probability[j], 0.0);
        sum += row.probability[j];
        const double next = after.states[row.destination[j]];
        mean += row.probability[j] * next;
        variance += row.probability[j] * std::pow(next - before.states[i], 2.0);
      }
      EXPECT_NEAR(sum, 1.0, 3.0e-16);
      if (step.kind == AmericanMarkovStepKind::Diffusion) {
        EXPECT_NEAR(mean, before.states[i], 1.0e-12);
        const double expected = i == 0 || i + 1 == before.states.size()
                                    ? 0.0
                                    : step.volatility * step.volatility * before.states[i] *
                                          before.states[i] * step.variance_step;
        EXPECT_NEAR(variance, expected, 1.0e-11);
      } else {
        EXPECT_DOUBLE_EQ(step.calendar_step, 0.0);
        EXPECT_DOUBLE_EQ(step.variance_step, 0.0);
        const double stock = before.growth * before.states[i];
        EXPECT_NEAR(after.growth * mean + std::min(stock, step.cash_dividend), stock, 1.0e-12);
      }
    }
  }
  const auto first_expiry = model->expiry_layers()[0];
  EXPECT_DOUBLE_EQ(model->layers()[first_expiry].calendar_time, 0.13);
  EXPECT_DOUBLE_EQ(model->layers()[first_expiry].variance_time, 0.07);
  EXPECT_EQ(model->transitions()[first_expiry - 1].kind, AmericanMarkovStepKind::CashDividend);
  EXPECT_LT(model->diagnostics().max_relative_mean_error, 1.0e-14);
}

TEST(AmericanMarkov, NegativeRatesAndZeroVarianceHaveExactDeterministicLimits) {
  // Volatility has no effect while the variance clock is stopped, even when
  // its square is outside the representable range.
  const std::array intervals{AmericanMarkovInterval{1.0, 0.0, -0.1, -0.1, 1.0e200, 0.0}};
  auto model = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  EXPECT_NEAR(price(*model, workspace, 80.0, Side::Call), 20.0 * std::exp(0.1), 2.0e-12);
  EXPECT_NEAR(price(*model, workspace, 120.0, Side::Put), 20.0 * std::exp(0.1), 2.0e-12);

  const std::array shrinking{AmericanMarkovInterval{1.0, 0.0, -0.05, 0.0, 0.0, 0.0}};
  auto declining = compile_american_markov({100.0, shrinking, {}, {}});
  ASSERT_TRUE(declining);
  EXPECT_NEAR(price(*declining, workspace, 80.0, Side::Call), 20.0, 2.0e-12);
  EXPECT_NEAR(price(*declining, workspace, 80.0, Side::Call, 0, ExerciseStyle::European),
              100.0 - 80.0 * std::exp(0.05), 2.0e-12);
}

TEST(AmericanMarkov, ExpiryCashRetainsCumExerciseAndLimitedLiability) {
  const std::array intervals{AmericanMarkovInterval{0.25, 0.0, 0.0, 0.0, 0.0, 10.0},
                             AmericanMarkovInterval{0.5, 0.0, 0.0, 0.0, 0.0, 1000.0},
                             AmericanMarkovInterval{0.75, 0.0, -0.04, 0.0, 0.0, 0.0}};
  auto model = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  EXPECT_DOUBLE_EQ(price(*model, workspace, 80.0, Side::Call), 20.0);
  EXPECT_DOUBLE_EQ(price(*model, workspace, 80.0, Side::Call, 0, ExerciseStyle::European), 10.0);
  EXPECT_DOUBLE_EQ(price(*model, workspace, 100.0, Side::Put), 10.0);
  EXPECT_DOUBLE_EQ(price(*model, workspace, 100.0, Side::Put, 1), 100.0);
  EXPECT_DOUBLE_EQ(price(*model, workspace, 80.0, Side::Call, 1, ExerciseStyle::European), 0.0);
  EXPECT_NEAR(price(*model, workspace, 100.0, Side::Put, 2), 100.0 * std::exp(0.01), 3.0e-12);
  EXPECT_EQ(model->layers().back().states.size(), 1U);
  EXPECT_DOUBLE_EQ(model->diagnostics().terminal_zero_probability, 1.0);
}

TEST(AmericanMarkov, SharedExpiryPrefixesPreserveAmericanShapeAndCalendarOrdering) {
  const std::array intervals{AmericanMarkovInterval{0.1, 0.07, -0.04, 0.02, 0.25, 1.0},
                             AmericanMarkovInterval{0.25, 0.2, -0.04, 0.03, 0.2, 0.0},
                             AmericanMarkovInterval{0.5, 0.35, 0.01, 0.0, 0.3, 3.0}};
  auto model = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  const auto workspace_size = workspace.size();
  for (const auto side : {Side::Call, Side::Put}) {
    std::array<double, 21> previous{};
    for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
      std::array<double, 21> values{};
      double discount = 1.0;
      double max_discount = 1.0;
      for (std::size_t n = 0; n < model->expiry_layers()[expiry]; ++n) {
        discount *= model->transitions()[n].discount;
        max_discount = std::max(max_discount, discount);
      }
      for (std::size_t i = 0; i < values.size(); ++i) {
        const double strike = 50.0 + 5.0 * static_cast<double>(i);
        values[i] = price(*model, workspace, strike, side, expiry);
        EXPECT_GE(values[i] + 1.0e-11,
                  std::max(side == Side::Call ? 100.0 - strike : strike - 100.0, 0.0));
        EXPECT_GE(values[i] + 1.0e-11, previous[i]);
        EXPECT_LE(values[i], (side == Side::Call ? 100.0 : strike * max_discount) + 1.0e-10);
        if (i > 0) {
          const double slope = (values[i] - values[i - 1]) / 5.0;
          EXPECT_GE(slope, (side == Side::Call ? -max_discount : 0.0) - 1.0e-11);
          EXPECT_LE(slope, (side == Side::Call ? 0.0 : max_discount) + 1.0e-11);
        }
        if (i > 1) {
          EXPECT_GE(values[i] - 2.0 * values[i - 1] + values[i - 2], -1.0e-10);
        }
      }
      previous = values;
    }
  }
  EXPECT_EQ(workspace.size(), workspace_size);
}

TEST(AmericanMarkov, SeparateClocksConvergeToIndependentEuropeanFormula) {
  const std::array intervals{AmericanMarkovInterval{0.5, 0.25, 0.03, 0.01, 0.2, 0.0}};
  const double forward = 100.0 * std::exp(0.02 * 0.5);
  const double expected =
      black76_price(forward, 100.0, 0.25, 0.2, std::exp(-0.03 * 0.5), Side::Call);
  std::array<double, 2> errors{};
  for (std::size_t pass = 0; pass < 2; ++pass) {
    AmericanMarkovSpec spec{100.0, intervals, {}, {}};
    spec.config.grid_nodes = pass == 0 ? 65U : 257U;
    auto model = compile_american_markov(spec);
    ASSERT_TRUE(model) << model.error().to_string();
    AmericanMarkovWorkspace workspace;
    ASSERT_TRUE(workspace.prepare(*model));
    const double call = price(*model, workspace, 100.0, Side::Call, 0, ExerciseStyle::European);
    const double put = price(*model, workspace, 100.0, Side::Put, 0, ExerciseStyle::European);
    EXPECT_NEAR(call - put, std::exp(-0.03 * 0.5) * (forward - 100.0), 1.0e-11);
    errors[pass] = std::abs(call - expected);
  }
  EXPECT_LT(errors[1], errors[0]);
  EXPECT_LT(errors[1], 0.025);
}

TEST(AmericanMarkov, ZeroStrikeShareIdentityAndStockScaleInvariance) {
  const std::array intervals{AmericanMarkovInterval{0.2, 0.1, -0.04, -0.08, 0.2, 0.0}};
  std::array<double, 3> normalized{};
  const std::array scales{1.0e-150, 1.0, 1.0e150};
  for (std::size_t i = 0; i < scales.size(); ++i) {
    const double spot = 100.0 * scales[i];
    auto model = compile_american_markov({spot, intervals, {}, {}});
    ASSERT_TRUE(model) << model.error().to_string();
    AmericanMarkovWorkspace workspace;
    ASSERT_TRUE(workspace.prepare(*model));
    normalized[i] = price(*model, workspace, spot, Side::Call) / spot;
    EXPECT_NEAR(price(*model, workspace, 0.0, Side::Call, 0, ExerciseStyle::European) / spot,
                std::exp(0.08 * 0.2), 2.0e-13);
  }
  EXPECT_NEAR(normalized[0], normalized[1], 2.0e-14);
  EXPECT_NEAR(normalized[2], normalized[1], 2.0e-14);
}

TEST(AmericanMarkov, OwnsInputsAndRejectsMalformedModelsAndUnpreparedWorkspaces) {
  std::vector intervals{AmericanMarkovInterval{0.2, 0.1, 0.02, 0.0, 0.2, 0.0}};
  std::vector grid{0.0, 80.0, 100.0, 120.0, 200.0};
  AmericanMarkovSpec spec{100.0, intervals, grid, {}};
  auto model = compile_american_markov(spec);
  ASSERT_TRUE(model);
  intervals[0].volatility = 9.0;
  grid[2] = 99.0;
  AmericanMarkovWorkspace workspace;
  EXPECT_FALSE(price_american_markov(*model, 100.0, Side::Call, 0, workspace));
  ASSERT_TRUE(workspace.prepare(*model));
  EXPECT_GT(price(*model, workspace, 100.0, Side::Call), 0.0);
  EXPECT_FALSE(price_american_markov(*model, -1.0, Side::Call, 0, workspace));
  EXPECT_FALSE(price_american_markov(*model, 100.0, static_cast<Side>(3), 0, workspace));
  EXPECT_FALSE(price_american_markov(*model, 100.0, Side::Call, 1, workspace));
  EXPECT_FALSE(compile_american_markov(spec)); // exact spot was removed
  grid[2] = 100.0;
  grid[0] = -1.0;
  EXPECT_FALSE(compile_american_markov(spec));
  grid[0] = 0.0;
  grid[1] = grid[2];
  EXPECT_FALSE(compile_american_markov(spec));
  spec.state_grid = {};
  intervals[0].variance_end = -1.0;
  EXPECT_FALSE(compile_american_markov(spec));
  intervals[0].variance_end = 0.1;
  intervals[0].cash_dividend_at_end = -1.0;
  EXPECT_FALSE(compile_american_markov(spec));
}

TEST(AmericanMarkov, ExpandedGridAndStoredRowsHaveHardLimits) {
  const std::array intervals{AmericanMarkovInterval{1.0, 1.0, 0.02, 0.0, 0.4, 1.0}};
  AmericanMarkovSpec spec{100.0, intervals, {}, {}};
  spec.config.max_transitions = 2;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_transitions = 8192;
  spec.config.max_stored_rows = 10;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_stored_rows = kAmericanMarkovMaxStoredRows + 1;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_stored_rows = 1000000;
  spec.config.max_states = kAmericanMarkovMaxStates + 1;
  EXPECT_FALSE(compile_american_markov(spec));
}

void expect_same_operators(const AmericanMarkovModel &a, const AmericanMarkovModel &b) {
  ASSERT_EQ(a.layers().size(), b.layers().size());
  ASSERT_EQ(a.transitions().size(), b.transitions().size());
  for (std::size_t n = 0; n < a.layers().size(); ++n) {
    EXPECT_EQ(a.layers()[n].states, b.layers()[n].states);
    EXPECT_EQ(a.layers()[n].growth, b.layers()[n].growth);
    EXPECT_EQ(a.layers()[n].calendar_time, b.layers()[n].calendar_time);
    EXPECT_EQ(a.layers()[n].variance_time, b.layers()[n].variance_time);
  }
  for (std::size_t n = 0; n < a.transitions().size(); ++n) {
    const auto &left = a.transitions()[n];
    const auto &right = b.transitions()[n];
    EXPECT_EQ(left.kind, right.kind);
    EXPECT_EQ(left.discount, right.discount);
    EXPECT_EQ(left.calendar_step, right.calendar_step);
    EXPECT_EQ(left.variance_step, right.variance_step);
    EXPECT_EQ(left.interval_index, right.interval_index);
    ASSERT_EQ(left.rows.size(), right.rows.size());
    for (std::size_t i = 0; i < left.rows.size(); ++i) {
      EXPECT_EQ(left.rows[i].destination, right.rows[i].destination);
      EXPECT_EQ(left.rows[i].probability, right.rows[i].probability);
    }
  }
}

TEST(AmericanMarkov, ConstantLocalMultipliersExactlyReproduceFlatOperators) {
  const std::array nodes{AmericanMarkovLocalVolNode{-1.0, 1.0},
                         AmericanMarkovLocalVolNode{0.0, 1.0},
                         AmericanMarkovLocalVolNode{1.0, 1.0}};
  std::array intervals{AmericanMarkovInterval{0.13, 0.07, -0.03, -0.01, 0.3, 5.0},
                       AmericanMarkovInterval{0.29, 0.15, 0.02, 0.01, 0.25, 0.0},
                       AmericanMarkovInterval{0.31, 0.15, 0.02, 0.01, 0.5, 0.0}};
  auto flat = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(flat);
  for (auto &interval : intervals) {
    interval.local_volatility = nodes;
  }
  auto one = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(one);
  expect_same_operators(*flat, *one);
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*flat));
  for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      for (const double strike : {80.0, 100.0, 120.0}) {
        EXPECT_EQ(price(*flat, workspace, strike, side, expiry),
                  price(*one, workspace, strike, side, expiry));
      }
    }
  }
  const std::array twice{AmericanMarkovLocalVolNode{0.0, 2.0}};
  for (auto &interval : intervals) {
    interval.volatility *= 0.5;
    interval.local_volatility = twice;
  }
  auto scaled = compile_american_markov({100.0, intervals, {}, {}});
  ASSERT_TRUE(scaled);
  expect_same_operators(*flat, *scaled);
}

TEST(AmericanMarkov, LocalVolatilityPreservesPositiveRowsAndConditionalMoments) {
  const std::array nodes{AmericanMarkovLocalVolNode{-0.7, 1.7},
                         AmericanMarkovLocalVolNode{0.0, 0.0},
                         AmericanMarkovLocalVolNode{0.4, 0.8}};
  const std::array grid{0.0, 35.0, 80.0, 100.0, 130.0, 240.0, 400.0};
  const std::array intervals{AmericanMarkovInterval{0.13, 0.07, -0.03, -0.01, 0.3, 5.0, nodes},
                             AmericanMarkovInterval{0.29, 0.15, 0.02, 0.01, 0.25, 0.0, nodes}};
  AmericanMarkovSpec spec{100.0, intervals, grid, {}};
  spec.config.cfl_fraction = 1.0;
  auto model = compile_american_markov(spec);
  ASSERT_TRUE(model) << model.error().to_string();
  ASSERT_EQ(model->interval_data().size(), 2U);
  EXPECT_EQ(model->interval_data()[0].state_volatilities[3], 0.0);
  for (std::size_t n = 0; n < model->transitions().size(); ++n) {
    const auto &step = model->transitions()[n];
    ASSERT_LT(step.interval_index, model->interval_data().size());
    const auto &metadata = model->interval_data()[step.interval_index];
    ASSERT_LE(metadata.first_layer, n);
    if (step.kind != AmericanMarkovStepKind::Diffusion) {
      continue;
    }
    const auto &before = model->layers()[n];
    const auto &after = model->layers()[n + 1];
    ASSERT_EQ(metadata.state_volatilities.size(), before.states.size());
    for (std::size_t i = 0; i < step.rows.size(); ++i) {
      double sum = 0.0;
      double mean = 0.0;
      double variance = 0.0;
      const auto &row = step.rows[i];
      for (std::size_t j = 0; j < 3; ++j) {
        EXPECT_GE(row.probability[j], 0.0);
        EXPECT_LE(row.probability[j], 1.0);
        ASSERT_LT(row.destination[j], after.states.size());
        sum += row.probability[j];
        const double next = after.states[row.destination[j]];
        mean += row.probability[j] * next;
        variance += row.probability[j] * std::pow(next - before.states[i], 2.0);
      }
      EXPECT_NEAR(sum, 1.0, 3.0e-16);
      EXPECT_NEAR(mean, before.states[i], 1.0e-12);
      const double sigma = metadata.state_volatilities[i];
      const double expected = i == 0 || i + 1 == before.states.size()
                                  ? 0.0
                                  : sigma * sigma * before.states[i] * before.states[i] *
                                        step.variance_step;
      EXPECT_NEAR(variance, expected, 2.0e-11);
    }
  }
  EXPECT_LT(model->diagnostics().max_relative_mean_error, 1.0e-14);
}

TEST(AmericanMarkov, LocalVolatilitySamplesActualPostCashStatesAndOwnsMetadata) {
  std::vector nodes{AmericanMarkovLocalVolNode{-1.0, 2.0},
                    AmericanMarkovLocalVolNode{1.0, 0.5}};
  const std::array grid{0.0, 20.0, 80.0, 100.0, 120.0, 200.0};
  std::array intervals{AmericanMarkovInterval{0.1, 0.1, 0.03, -0.01, 0.2, 30.0, nodes},
                       AmericanMarkovInterval{0.2, 0.2, 0.03, -0.01, 0.2, 0.0, nodes}};
  auto model = compile_american_markov({100.0, intervals, grid, {}});
  ASSERT_TRUE(model);
  ASSERT_EQ(model->interval_data().size(), 2U);
  const auto &first = model->interval_data()[0];
  const auto &second = model->interval_data()[1];
  const auto &shifted = model->layers()[second.first_layer];
  ASSERT_EQ(shifted.states.size(), 5U); // 0 and 20 coalesce after cash.
  EXPECT_NEAR(shifted.states[1], 80.0 - 30.0 / std::exp(0.004), 1.0e-13);
  EXPECT_GT(second.state_volatilities[1], first.state_volatilities[2]);
  for (std::size_t i = 0; i < shifted.states.size(); ++i) {
    const double coordinate = shifted.states[i] == 0.0
                                  ? -1.0
                                  : std::clamp(std::log(shifted.states[i] / 100.0), -1.0, 1.0);
    EXPECT_NEAR(second.state_volatilities[i], 0.2 * (1.25 - 0.75 * coordinate), 2.0e-16);
  }
  const auto expected_values = second.state_volatilities;
  nodes.clear();
  nodes.shrink_to_fit();
  intervals[1].volatility = 9.0;
  EXPECT_EQ(second.state_volatilities, expected_values);
  ASSERT_EQ(second.local_volatility.size(), 2U);
  EXPECT_EQ(second.local_volatility[0].volatility_multiplier, 2.0);
  EXPECT_EQ(second.volatility, 0.2);
  EXPECT_EQ(model->diagnostics().stored_volatility_values, 15U); // 6+2, then 5+2.
}

TEST(AmericanMarkov, LocalVolatilitySkewRetainsStrikeAndCalendarNoArbitrage) {
  const std::array nodes{AmericanMarkovLocalVolNode{-0.4, 1.8},
                         AmericanMarkovLocalVolNode{0.0, 1.0},
                         AmericanMarkovLocalVolNode{0.4, 0.4}};
  const std::array intervals{AmericanMarkovInterval{0.25, 0.25, 0.0, 0.0, 0.25, 0.0, nodes},
                             AmericanMarkovInterval{0.5, 0.5, 0.0, 0.0, 0.25, 0.0, nodes}};
  AmericanMarkovSpec spec{100.0, intervals, {}, {}};
  spec.config.grid_nodes = 129;
  auto model = compile_american_markov(spec);
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  for (const auto side : {Side::Call, Side::Put}) {
    std::array<double, 25> previous{};
    for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
      std::array<double, 25> values{};
      for (std::size_t i = 0; i < values.size(); ++i) {
        const double strike = 70.0 + 2.5 * static_cast<double>(i);
        values[i] = price(*model, workspace, strike, side, expiry);
        EXPECT_GE(values[i] + 1.0e-11, previous[i]);
        EXPECT_GE(values[i] + 1.0e-11,
                  std::max(side == Side::Call ? 100.0 - strike : strike - 100.0, 0.0));
        EXPECT_LE(values[i], (side == Side::Call ? 100.0 : strike) + 1.0e-11);
        if (i > 0) {
          const double slope = (values[i] - values[i - 1]) / 2.5;
          EXPECT_GE(slope, (side == Side::Call ? -1.0 : 0.0) - 1.0e-11);
          EXPECT_LE(slope, (side == Side::Call ? 0.0 : 1.0) + 1.0e-11);
        }
        if (i > 1) {
          EXPECT_GE(values[i] - 2.0 * values[i - 1] + values[i - 2], -1.0e-10);
        }
      }
      previous = values;
    }
  }
  const double low_put = price(*model, workspace, 85.0, Side::Put, 1, ExerciseStyle::European);
  const double high_call = price(*model, workspace, 115.0, Side::Call, 1, ExerciseStyle::European);
  const auto low_iv = implied_vol(low_put, 100.0, 85.0, 0.5, 1.0, Side::Put);
  const auto high_iv = implied_vol(high_call, 100.0, 115.0, 0.5, 1.0, Side::Call);
  ASSERT_TRUE(low_iv);
  ASSERT_TRUE(high_iv);
  EXPECT_GT(*low_iv, *high_iv + 0.03);
}

TEST(AmericanMarkov, LocalVolatilityHandlesExtremeStockScalesAndFlatTails) {
  const std::array nodes{AmericanMarkovLocalVolNode{-0.4, 1.4},
                         AmericanMarkovLocalVolNode{0.0, 1.0},
                         AmericanMarkovLocalVolNode{0.4, 0.7}};
  const std::array intervals{AmericanMarkovInterval{0.2, 0.1, 0.02, 0.01, 0.2, 0.0, nodes}};
  std::array<double, 3> normalized{};
  const std::array scales{1.0e-250, 1.0, 1.0e250};
  for (std::size_t i = 0; i < scales.size(); ++i) {
    const double spot = 100.0 * scales[i];
    auto model = compile_american_markov({spot, intervals, {}, {}});
    ASSERT_TRUE(model) << model.error().to_string();
    AmericanMarkovWorkspace workspace;
    ASSERT_TRUE(workspace.prepare(*model));
    normalized[i] = price(*model, workspace, spot, Side::Call) / spot;
    EXPECT_EQ(model->interval_data()[0].state_volatilities.front(), 0.2 * 1.4);
    EXPECT_EQ(model->interval_data()[0].state_volatilities.back(), 0.2 * 0.7);
  }
  EXPECT_NEAR(normalized[0], normalized[1], 2.0e-13);
  EXPECT_NEAR(normalized[2], normalized[1], 2.0e-13);

  const std::array extreme_grid{0.0, 1.0e-300, 1.0, 1.0e300};
  const std::array stopped{AmericanMarkovInterval{0.1, 0.0, 0.0, 0.0, 0.2, 0.0, nodes}};
  auto wide = compile_american_markov({1.0e-300, stopped, extreme_grid, {}});
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide->interval_data()[0].state_volatilities[2], 0.2 * 0.7);
  EXPECT_EQ(wide->interval_data()[0].state_volatilities[3], 0.2 * 0.7);
}

TEST(AmericanMarkov, LocalVolatilityRejectsMalformedNodesAndOverflow) {
  std::array nodes{AmericanMarkovLocalVolNode{-1.0, 1.0},
                   AmericanMarkovLocalVolNode{1.0, 1.0}};
  std::array intervals{AmericanMarkovInterval{0.1, 0.1, 0.0, 0.0, 0.2, 0.0, nodes}};
  AmericanMarkovSpec spec{100.0, intervals, {}, {}};
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const double coordinate : {nan, inf, -65.0, 65.0, 1.0, 2.0}) {
    nodes[0].log_state_over_spot = coordinate;
    EXPECT_FALSE(compile_american_markov(spec));
  }
  nodes[0].log_state_over_spot = -1.0;
  for (const double multiplier : {-0.01, nan, inf}) {
    nodes[0].volatility_multiplier = multiplier;
    EXPECT_FALSE(compile_american_markov(spec));
  }
  nodes[0].volatility_multiplier = 1.0e200;
  intervals[0].volatility = 1.0e200;
  EXPECT_FALSE(compile_american_markov(spec)); // sigma product overflows.
  intervals[0].volatility = 1.0;
  EXPECT_FALSE(compile_american_markov(spec)); // finite sigma, unresolvable variance.
  intervals[0].variance_end = 0.0;
  EXPECT_TRUE(compile_american_markov(spec)); // stopped variance avoids sigma squared.
  intervals[0].volatility = 0.0;
  intervals[0].variance_end = 0.1;
  EXPECT_TRUE(compile_american_markov(spec)); // zero times a finite multiplier is valid.
}

TEST(AmericanMarkov, LocalVolatilityMetadataHasExplicitHardResourceLimits) {
  std::vector<AmericanMarkovLocalVolNode> nodes(kAmericanMarkovMaxLocalVolNodes);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    nodes[i] = {-1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(nodes.size() - 1), 1.0};
  }
  std::array intervals{AmericanMarkovInterval{0.1, 0.1, 0.0, 0.0, 0.2, 0.0, nodes}};
  AmericanMarkovSpec spec{100.0, intervals, {}, {}};
  const std::size_t exact_values = spec.config.grid_nodes + nodes.size();
  spec.config.max_stored_volatility_values = exact_values;
  auto exact = compile_american_markov(spec);
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->diagnostics().stored_volatility_values, exact_values);
  spec.config.max_stored_volatility_values = exact_values - 1;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_stored_volatility_values = 0;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_stored_volatility_values = kAmericanMarkovMaxStoredVolatilityValues + 1;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_stored_volatility_values = 1000000;
  nodes.push_back({2.0, 1.0});
  intervals[0].local_volatility = nodes;
  EXPECT_FALSE(compile_american_markov(spec));
  nodes.resize(2);
  nodes[0] = {-kAmericanMarkovMaxLocalVolLogState, 1.0};
  nodes[1] = {kAmericanMarkovMaxLocalVolLogState, 1.0};
  intervals[0].local_volatility = nodes;
  EXPECT_TRUE(compile_american_markov(spec));
}

} // namespace
} // namespace atx::vol
