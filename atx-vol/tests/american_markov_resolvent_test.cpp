#include "atx/vol/api/pricing/american_markov.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american_markov_batch.hpp"
#include "atx/vol/api/pricing/american_markov_grid.hpp"
#include "atx/vol/api/pricing/american_markov_pwl.hpp"

namespace atx::vol {
namespace {

AmericanMarkovConfig resolvent_config() {
  AmericanMarkovConfig config;
  config.diffusion_method = AmericanMarkovDiffusionMethod::VarianceResolvent;
  config.max_calendar_step = 1.0;
  config.max_variance_step = 1.0;
  return config;
}

// Independent dense Gauss-Jordan inversion with partial pivoting. Construct
// I-hQ from the second-derivative rates directly, without production factors.
std::vector<double> dense_resolvent(std::span<const double> grid, double sigma, double h) {
  const auto n = grid.size();
  std::vector<double> matrix(n * n, 0.0);
  std::vector<double> inverse(n * n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    matrix[i * n + i] = 1.0;
    inverse[i * n + i] = 1.0;
    if (i > 0 && i + 1 < n) {
      const double left = grid[i] - grid[i - 1];
      const double right = grid[i + 1] - grid[i];
      const double variance = sigma * sigma * grid[i] * grid[i];
      const double down = h * variance / (left * (left + right));
      const double up = h * variance / (right * (left + right));
      matrix[i * n + i - 1] = -down;
      matrix[i * n + i] += down + up;
      matrix[i * n + i + 1] = -up;
    }
  }
  for (std::size_t column = 0; column < n; ++column) {
    std::size_t pivot = column;
    for (std::size_t row = column + 1; row < n; ++row) {
      if (std::abs(matrix[row * n + column]) > std::abs(matrix[pivot * n + column])) pivot = row;
    }
    EXPECT_GT(std::abs(matrix[pivot * n + column]), 0.0);
    if (matrix[pivot * n + column] == 0.0) return {};
    for (std::size_t j = 0; j < n; ++j) {
      std::swap(matrix[column * n + j], matrix[pivot * n + j]);
      std::swap(inverse[column * n + j], inverse[pivot * n + j]);
    }
    const double scale = matrix[column * n + column];
    for (std::size_t j = 0; j < n; ++j) {
      matrix[column * n + j] /= scale;
      inverse[column * n + j] /= scale;
    }
    for (std::size_t row = 0; row < n; ++row) {
      if (row == column) continue;
      const double multiplier = matrix[row * n + column];
      for (std::size_t j = 0; j < n; ++j) {
        matrix[row * n + j] -= multiplier * matrix[column * n + j];
        inverse[row * n + j] -= multiplier * inverse[column * n + j];
      }
    }
  }
  return inverse;
}

double independent_dense_snell(const AmericanMarkovModel &model,
                                std::span<const AmericanMarkovInterval> intervals,
                                double strike, Side side, ExerciseStyle exercise,
                                std::size_t &active_interior_exercises) {
  const auto payoff = [&](double stock) {
    return std::max(side == Side::Call ? stock - strike : strike - stock, 0.0);
  };
  const auto &terminal = model.layers().back();
  std::vector<double> values;
  for (const auto x : terminal.states()) values.push_back(payoff(terminal.growth * x));
  for (std::size_t step = model.transitions().size(); step-- > 0;) {
    const auto &transition = model.transitions()[step];
    const auto &before = model.layers()[step];
    const auto &after = model.layers()[step + 1];
    const auto &interval = intervals[transition.interval_index];
    const auto source = before.states();
    const auto target = after.states();
    std::vector<double> previous(source.size());
    if (transition.kind == AmericanMarkovStepKind::CashDividend) {
      for (std::size_t i = 0; i < source.size(); ++i) {
        const double post =
            std::max(source[i] - interval.cash_dividend_at_end / before.growth, 0.0);
        const auto destination = std::lower_bound(target.begin(), target.end(), post);
        EXPECT_NE(destination, target.end());
        if (destination == target.end()) return std::numeric_limits<double>::quiet_NaN();
        EXPECT_DOUBLE_EQ(*destination, post);
        previous[i] = values[static_cast<std::size_t>(destination - target.begin())];
      }
    } else {
      const auto matrix = dense_resolvent(source, interval.volatility,
                                          after.variance_time - before.variance_time);
      if (matrix.size() != source.size() * target.size()) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      for (std::size_t i = 0; i < source.size(); ++i) {
        for (std::size_t j = 0; j < target.size(); ++j) {
          previous[i] += matrix[i * target.size() + j] * values[j];
        }
      }
    }
    const double discount = std::exp(-interval.rate * (after.calendar_time - before.calendar_time));
    for (std::size_t i = 0; i < source.size(); ++i) {
      previous[i] *= discount;
      if (exercise == ExerciseStyle::American) {
        const double immediate = payoff(before.growth * source[i]);
        if (i > 0 && i + 1 < source.size() && immediate > previous[i] + 1.0e-10) {
          ++active_interior_exercises;
        }
        previous[i] = std::max(immediate, previous[i]);
      }
    }
    values = std::move(previous);
  }
  return values[model.initial_state_index()];
}

TEST(AmericanMarkovResolvent, DenseSnellKeepsLinearContinuationSeparateFromDiscountAndExercise) {
  const std::array grid{0.0, 50.0, 80.0, 100.0, 120.0, 160.0, 300.0};
  const std::array intervals{AmericanMarkovInterval{0.2, 0.1, 0.10, 0.0, 0.2, 8.0},
                             AmericanMarkovInterval{0.4, 0.25, 0.07, 0.03, 0.25, 0.0}};
  auto config = resolvent_config();
  config.max_calendar_step = 0.1;
  config.max_variance_step = 0.08;
  auto model = compile_american_markov({100.0, intervals, grid, config});
  ASSERT_TRUE(model);
  AmericanMarkovWorkspace scalar;
  AmericanMarkovBatchWorkspace batch;
  const std::array strikes{80.0, 100.0, 120.0};
  std::array<double, strikes.size()> output{};
  ASSERT_TRUE(scalar.prepare(*model));
  ASSERT_TRUE(batch.prepare(*model, strikes.size()));
  for (const auto side : {Side::Call, Side::Put}) {
    std::size_t active = 0;
    for (const auto exercise : {ExerciseStyle::European, ExerciseStyle::American}) {
      ASSERT_TRUE(price_american_markov_batch(*model, strikes, side, 1, output, batch, exercise));
      for (std::size_t i = 0; i < strikes.size(); ++i) {
        const double reference = independent_dense_snell(*model, intervals, strikes[i], side,
                                                         exercise, active);
        const auto value = price_american_markov(*model, strikes[i], side, 1, scalar, exercise);
        ASSERT_TRUE(value);
        EXPECT_NEAR(*value, reference, 1.0e-11);
        EXPECT_NEAR(output[i], reference, 1.0e-11);
      }
    }
    EXPECT_GT(active, 0U); // Both put and call scenarios exercise at interior states.
  }
}

TEST(AmericanMarkovResolvent, OneInteriorStateMatchesClosedFormWithoutCflSubdivision) {
  const std::array grid{0.0, 100.0, 200.0};
  const std::array intervals{AmericanMarkovInterval{1.0, 1.0, 0.0, 0.0, 4.0, 0.0}};
  AmericanMarkovSpec spec{100.0, intervals, grid, resolvent_config()};
  spec.config.max_transitions = 1;
  auto model = compile_american_markov(spec);
  ASSERT_TRUE(model) << model.error().to_string();
  ASSERT_EQ(model->transitions().size(), 1U);
  EXPECT_EQ(model->diffusion_method(), AmericanMarkovDiffusionMethod::VarianceResolvent);
  EXPECT_EQ(model->diagnostics().stored_rows, 3U);
  EXPECT_EQ(model->diagnostics().transition_work, 6U);
  EXPECT_EQ(model->diagnostics().physical_transition_rows, 0U);
  EXPECT_EQ(model->diagnostics().physical_resolvent_factors, 3U);
  const std::array payoff{0.0, 0.0, 100.0};
  std::array<double, 3> output{};
  ASSERT_TRUE(apply_american_markov_transition(model->transitions().front(), payoff, output));
  EXPECT_DOUBLE_EQ(output[0], 0.0);
  EXPECT_NEAR(output[1], 800.0 / 17.0, 1.0e-13);
  EXPECT_DOUBLE_EQ(output[2], 100.0);
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  for (const auto side : {Side::Call, Side::Put}) {
    for (const auto exercise : {ExerciseStyle::American, ExerciseStyle::European}) {
      auto value = price_american_markov(*model, 100.0, side, 0, workspace, exercise);
      ASSERT_TRUE(value);
      EXPECT_NEAR(*value, 800.0 / 17.0, 1.0e-13);
    }
  }
  spec.config.diffusion_method = AmericanMarkovDiffusionMethod::ExplicitEuler;
  EXPECT_FALSE(compile_american_markov(spec));
}

TEST(AmericanMarkovResolvent, BasisActionsAgreeWithIndependentDenseInverseAndTranspose) {
  const std::array grid{0.0, 25.0, 80.0, 100.0, 130.0, 240.0, 400.0};
  constexpr double sigma = 0.3;
  for (const double h : {0.0, 0.001, 0.2, 3.0, 1.0e5}) {
    const std::array intervals{AmericanMarkovInterval{0.1, h, 0.0, 0.0, sigma, 0.0}};
    auto config = resolvent_config();
    config.max_variance_step = std::max(1.0, h);
    auto model = compile_american_markov({100.0, intervals, grid, config});
    ASSERT_TRUE(model) << model.error().to_string();
    ASSERT_EQ(model->transitions().size(), 1U);
    const auto &transition = model->transitions().front();
    const auto inverse = dense_resolvent(grid, sigma, h);
    ASSERT_EQ(inverse.size(), grid.size() * grid.size());
    std::array<double, grid.size()> input{};
    std::array<double, grid.size()> backward{};
    std::array<double, grid.size()> forward{};
    for (std::size_t basis = 0; basis < grid.size(); ++basis) {
      input.fill(0.0);
      input[basis] = 1.0;
      ASSERT_TRUE(apply_american_markov_transition(transition, input, backward));
      ASSERT_TRUE(apply_american_markov_transition(transition, input, forward,
                                                   AmericanMarkovTransitionAction::Forward));
      for (std::size_t i = 0; i < grid.size(); ++i) {
        EXPECT_GE(backward[i], 0.0);
        EXPECT_GE(forward[i], 0.0);
        EXPECT_LE(backward[i], 1.0);
        EXPECT_NEAR(backward[i], inverse[i * grid.size() + basis], 2.0e-11) << h;
        EXPECT_NEAR(forward[i], inverse[basis * grid.size() + i], 2.0e-11) << h;
      }
      EXPECT_NEAR(std::accumulate(forward.begin(), forward.end(), 0.0), 1.0, 3.0e-15);
    }
    input.fill(1.0);
    ASSERT_TRUE(apply_american_markov_transition(transition, input, backward));
    for (const double value : backward) EXPECT_DOUBLE_EQ(value, 1.0);
    ASSERT_TRUE(apply_american_markov_transition(transition, grid, backward));
    for (std::size_t i = 0; i < grid.size(); ++i) {
      EXPECT_NEAR(backward[i], grid[i], 2.0e-12);
    }
    const std::array values{-2.0, 1.0, 4.0, 0.3, -1.0, 8.0, 2.0};
    const std::array mass{0.1, 0.0, 0.2, 0.3, 0.1, 0.0, 0.3};
    ASSERT_TRUE(apply_american_markov_transition(transition, values, backward));
    ASSERT_TRUE(apply_american_markov_transition(transition, mass, forward,
                                                 AmericanMarkovTransitionAction::Forward));
    EXPECT_NEAR(std::inner_product(mass.begin(), mass.end(), backward.begin(), 0.0),
                std::inner_product(values.begin(), values.end(), forward.begin(), 0.0), 2.0e-14);
  }
}

TEST(AmericanMarkovResolvent, ActionsValidateShapesOverlapAndFiniteInputBeforeWriting) {
  const std::array grid{0.0, 100.0, 200.0};
  const std::array intervals{AmericanMarkovInterval{0.1, 0.1, 0.0, 0.0, 0.3, 150.0}};
  auto model = compile_american_markov({100.0, intervals, grid, resolvent_config()});
  ASSERT_TRUE(model);
  const auto &transition = model->transitions().front();
  std::array input{1.0, 2.0, 3.0, 4.0};
  std::array output{-7.0, -7.0, -7.0};
  EXPECT_FALSE(apply_american_markov_transition(transition, input, output));
  EXPECT_FALSE(apply_american_markov_transition(transition, std::span(input).first(3),
                                                std::span(input).subspan(1)));
  EXPECT_FALSE(apply_american_markov_transition(transition, std::span(input).first(3), output,
                                                static_cast<AmericanMarkovTransitionAction>(99)));
  input[1] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(apply_american_markov_transition(transition, std::span(input).first(3), output));
  EXPECT_EQ(output, (std::array{-7.0, -7.0, -7.0}));
  const AmericanMarkovTransition empty;
  EXPECT_FALSE(apply_american_markov_transition(empty, {}, {}));

  const auto &cash = model->transitions().back();
  EXPECT_EQ(cash.source_state_count(), 3U);
  EXPECT_EQ(cash.target_state_count(), 2U);
  const std::array probabilities{0.2, 0.3, 0.5};
  std::array<double, 2> post{};
  ASSERT_TRUE(apply_american_markov_transition(cash, probabilities, post,
                                               AmericanMarkovTransitionAction::Forward));
  EXPECT_EQ(post, (std::array{0.5, 0.5}));
}

TEST(AmericanMarkovResolvent, CashLocalVolatilityAndPausedClocksKeepScalarBatchShapeRelations) {
  const std::array local{AmericanMarkovLocalVolNode{-0.3, 1.2},
                         AmericanMarkovLocalVolNode{0.3, 0.8}};
  const std::array intervals{AmericanMarkovInterval{0.1, 0.06, -0.03, 0.01, 0.3, 2.0, local},
                             AmericanMarkovInterval{0.15, 0.06, 0.02, 0.0, 0.3, 0.0, local},
                             AmericanMarkovInterval{0.3, 0.20, 0.03, 0.01, 0.25, 0.0, local}};
  auto config = resolvent_config();
  config.max_calendar_step = 0.025;
  config.max_variance_step = 0.025;
  auto model = compile_american_markov({100.0, intervals, {}, config});
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace scalar;
  AmericanMarkovBatchWorkspace batch;
  const std::array strikes{70.0, 80.0, 90.0, 100.0, 110.0, 120.0, 130.0};
  std::array<double, strikes.size()> output{};
  ASSERT_TRUE(scalar.prepare(*model));
  ASSERT_TRUE(batch.prepare(*model, strikes.size()));
  for (const auto side : {Side::Call, Side::Put}) {
    std::array<double, strikes.size()> earlier{};
    for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
      for (const auto exercise : {ExerciseStyle::European, ExerciseStyle::American}) {
        ASSERT_TRUE(price_american_markov_batch(*model, strikes, side, expiry,
                                               output, batch, exercise));
        for (std::size_t i = 0; i < strikes.size(); ++i) {
          const auto value =
              price_american_markov(*model, strikes[i], side, expiry, scalar, exercise);
          ASSERT_TRUE(value);
          EXPECT_NEAR(output[i], *value, 3.0e-13);
          if (i > 0) {
            if (side == Side::Call) EXPECT_LE(output[i], output[i - 1] + 1.0e-12);
            else EXPECT_GE(output[i], output[i - 1] - 1.0e-12);
          }
          if (i > 1) EXPECT_GE(output[i] - 2.0 * output[i - 1] + output[i - 2], -1.0e-12);
          if (exercise == ExerciseStyle::American) {
            EXPECT_GE(output[i], earlier[i] - 1.0e-12);
            earlier[i] = output[i];
          }
        }
      }
    }
  }
  EXPECT_LE(model->diagnostics().max_relative_mean_error, kAmericanMarkovResolventMaxActionError);
  EXPECT_LE(model->diagnostics().max_forward_mass_error, kAmericanMarkovResolventMaxActionError);
  EXPECT_FALSE(compile_american_markov_pwl(*model, 50.0, 150.0));
}

TEST(AmericanMarkovResolvent, LogicalRowsAndTwoPassWorkHaveIndependentExactBudgets) {
  const std::array grid{0.0, 50.0, 100.0, 200.0, 400.0};
  const std::array intervals{AmericanMarkovInterval{0.25, 0.25, 0.0, 0.0, 0.5, 1.0}};
  AmericanMarkovSpec spec{100.0, intervals, grid, resolvent_config()};
  spec.config.max_stored_rows = 10;
  spec.config.max_transition_work = 15;
  auto exact = compile_american_markov(spec);
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->diagnostics().stored_rows, 10U);
  EXPECT_EQ(exact->diagnostics().transition_work, 15U);
  spec.config.max_transition_work = 14;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_transition_work = 15;
  spec.config.max_stored_rows = 9;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_stored_rows = 10;
  spec.config.max_transition_work = 0;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_transition_work = kAmericanMarkovMaxTransitionWork + 1;
  EXPECT_FALSE(compile_american_markov(spec));
  spec.config.max_transition_work = 15;
  spec.config.diffusion_method = static_cast<AmericanMarkovDiffusionMethod>(99);
  EXPECT_FALSE(compile_american_markov(spec));
}

TEST(AmericanMarkovResolvent, DefaultWorkBudgetDoesNotRestrictRaisedExplicitRowBudget) {
  const std::array intervals{AmericanMarkovInterval{1.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
  AmericanMarkovSpec spec{100.0, intervals, {}, {}};
  spec.config.max_calendar_step = 1.0 / 32768.0;
  spec.config.max_transitions = 32768;
  spec.config.max_stored_rows = 3000000;
  EXPECT_GE(spec.config.max_transition_work, kAmericanMarkovMaxStoredRows);
  auto model = compile_american_markov(spec);
  ASSERT_TRUE(model);
  EXPECT_GT(model->diagnostics().transition_work, 2000000U);
  EXPECT_EQ(model->diagnostics().transition_work, model->diagnostics().stored_rows);
}

TEST(AmericanMarkovResolvent, ExactStepFactorsOwnTheirInputsAcrossCopiesAndMoves) {
  std::optional<AmericanMarkovModel> retained;
  AmericanMarkovTransition copied_transition;
  const AmericanMarkovResolventFactor *address = nullptr;
  {
    const std::array intervals{AmericanMarkovInterval{0.25, 0.25, 0.0, 0.0, 0.2, 0.0},
                               AmericanMarkovInterval{0.50, 0.50, 0.0, 0.0, 0.3, 0.0}};
    auto config = resolvent_config();
    config.max_calendar_step = 1.0 / 128.0;
    auto model = compile_american_markov({100.0, intervals, {}, config});
    ASSERT_TRUE(model);
    std::map<std::pair<std::size_t, std::uint64_t>, const AmericanMarkovResolventFactor *> blocks;
    for (const auto &transition : model->transitions()) {
      EXPECT_TRUE(transition.rows().empty());
      const auto [prior, inserted] = blocks.emplace(
          std::pair{transition.interval_index,
                    std::bit_cast<std::uint64_t>(transition.variance_step)},
          transition.resolvent_factors().data());
      EXPECT_EQ(prior->second, transition.resolvent_factors().data());
      if (inserted && blocks.size() == 2) EXPECT_NE(blocks.begin()->second, prior->second);
    }
    EXPECT_EQ(blocks.size(), 2U);
    EXPECT_EQ(model->diagnostics().resolvent_factor_blocks, 2U);
    EXPECT_EQ(model->diagnostics().physical_resolvent_factors, 2 * model->diagnostics().max_states);
    retained = *model;
    copied_transition = model->transitions().front();
    address = copied_transition.resolvent_factors().data();
  }
  auto moved_transition = std::move(copied_transition);
  EXPECT_TRUE(copied_transition.resolvent_factors().empty());
  auto moved_model = std::move(*retained);
  retained.reset();
  EXPECT_EQ(moved_model.transitions().front().resolvent_factors().data(), address);
  EXPECT_EQ(moved_transition.resolvent_factors().data(), address);
  std::vector<double> input(moved_transition.target_state_count(), 1.0);
  std::vector<double> output(moved_transition.source_state_count());
  ASSERT_TRUE(apply_american_markov_transition(moved_transition, input, output));
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](double x) { return x == 1.0; }));
}

TEST(AmericanMarkovResolvent, OriginalStateProbeHandlesExtremeIdentityAndRefusesLostTailDrift) {
  const std::array grid{0.0, 1.0e-300, 1.0e300};
  const std::array intervals{AmericanMarkovInterval{0.1, 0.0, 0.0, 0.0, 0.0, 0.0}};
  auto identity = compile_american_markov({1.0e-300, intervals, grid, resolvent_config()});
  ASSERT_TRUE(identity);
  EXPECT_EQ(identity->diagnostics().max_relative_mean_error, 0.0);
  std::array<double, grid.size()> output{};
  ASSERT_TRUE(apply_american_markov_transition(identity->transitions().front(), grid, output));
  EXPECT_EQ(output, grid);
  EXPECT_TRUE(compile_american_markov({1.0e-300, intervals, grid, {}}));

  // Normalizing this spot by its upper boundary leaves a positive subnormal.
  // Its rounded action falsely reports zero drift; the represented unscaled
  // transition loses about 9% of the spot because its upward rate underflows.
  const std::array extreme_grid{0.0, 1.0e-100, 1.0e223};
  const std::array extreme_interval{AmericanMarkovInterval{0.1, 1.0e308, 0.0, 0.0, 1.0e7, 0.0}};
  auto config = resolvent_config();
  config.max_calendar_step = 1.0;
  config.max_variance_step = 1.0e308;
  const auto lost_tail =
      compile_american_markov({1.0e-100, extreme_interval, extreme_grid, config});
  ASSERT_FALSE(lost_tail);
  EXPECT_NE(lost_tail.error().to_string().find("martingale residual exceeded"), std::string::npos);
}

TEST(AmericanMarkovResolvent, RefusesNonfiniteFactors) {
  const std::array ordinary_grid{0.0, 100.0, 200.0};
  const std::array overflow{AmericanMarkovInterval{0.1, 1.0e308, 0.0, 0.0, 4.0, 0.0}};
  auto config = resolvent_config();
  config.max_variance_step = 1.0e308;
  EXPECT_FALSE(compile_american_markov({100.0, overflow, ordinary_grid, config}));
}

TEST(AmericanMarkovResolvent, TimeRefinementReducesIndependentEuropeanAtmError) {
  auto grid = make_american_markov_sinh_grid(100.0, {257, 8.0, 0.1, 1025});
  ASSERT_TRUE(grid);
  const std::array intervals{AmericanMarkovInterval{0.25, 0.25, 0.0, 0.0, 0.2, 0.0}};
  const double normal = 0.5 * std::erfc(-0.05 / std::sqrt(2.0));
  const double reference = 100.0 * (2.0 * normal - 1.0);
  std::vector<double> errors;
  for (const double steps : {8.0, 32.0, 128.0, 512.0}) {
    auto config = resolvent_config();
    config.max_calendar_step = 0.25 / steps;
    config.max_variance_step = 0.25 / steps;
    auto model = compile_american_markov({100.0, intervals, grid->states, config});
    ASSERT_TRUE(model) << model.error().to_string();
    AmericanMarkovWorkspace workspace;
    ASSERT_TRUE(workspace.prepare(*model));
    auto value =
        price_american_markov(*model, 100.0, Side::Call, 0, workspace, ExerciseStyle::European);
    ASSERT_TRUE(value);
    errors.push_back(std::abs(*value - reference));
  }
  EXPECT_LT(errors.back(), 0.01);
  EXPECT_LT(errors.back(), errors.front() / 4.0);
  for (std::size_t i = 1; i < errors.size(); ++i) EXPECT_LT(errors[i], errors[i - 1]);
}

} // namespace
} // namespace atx::vol
