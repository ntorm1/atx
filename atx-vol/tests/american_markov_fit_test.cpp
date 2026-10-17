#include "atx/vol/api/fitting/american_markov_fit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american_markov_batch.hpp"
#include "fitting/american_markov_fit_lm.hpp"
#include "fitting/american_markov_time_basis.hpp"

namespace atx::vol {
namespace {

std::vector<AmericanMarkovFitQuote> synthetic_quotes(
    const AmericanMarkovFitMarket &market, std::span<const double> knots,
    std::span<const double> sigmas, bool per_interval = false, double half_spread = 0.002) {
  std::vector<AmericanMarkovLocalVolNode> nodes;
  for (std::size_t i = 0; i < sigmas.size(); ++i) {
    nodes.push_back({knots[i % knots.size()], sigmas[i]});
  }
  std::vector<AmericanMarkovInterval> intervals;
  std::size_t group = 0;
  double last_variance = 0.0;
  for (const auto &in : market.intervals) {
    const std::size_t offset = per_interval && in.variance_end > last_variance ? group++ : 0;
    intervals.push_back({in.calendar_end, in.variance_end, in.rate, in.continuous_yield,
                         1.0, in.cash_dividend_at_end,
                         std::span<const AmericanMarkovLocalVolNode>(nodes).subspan(
                             offset * knots.size(), knots.size())});
    last_variance = in.variance_end;
  }
  auto model = compile_american_markov(
      {market.spot, intervals, market.state_grid, market.model_config});
  EXPECT_TRUE(model);
  if (!model) return {};
  AmericanMarkovWorkspace workspace;
  EXPECT_TRUE(workspace.prepare(*model));
  std::vector<AmericanMarkovFitQuote> quotes;
  for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      for (const double strike : {80.0, 90.0, 100.0, 110.0, 120.0}) {
        const auto value = price_american_markov(*model, strike, side, expiry, workspace);
        EXPECT_TRUE(value);
        if (!value) return {};
        quotes.push_back({expiry, strike, side, std::max(0.0, *value - half_spread),
                          *value + half_spread, 1.0});
      }
    }
  }
  return quotes;
}

double normal_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

double independent_call(double spot, double strike, double years, double sigma) {
  const double width = sigma * std::sqrt(years);
  const double d1 = std::log(spot / strike) / width + 0.5 * width;
  return spot * normal_cdf(d1) - strike * normal_cdf(d1 - width);
}

TEST(AmericanMarkovFit, RecoversSharedFlatSigmaAndOwnsAllOutputs) {
  std::vector intervals{AmericanMarkovFitInterval{0.15, 0.10, -0.01, 0.02, 2.0},
                        AmericanMarkovFitInterval{0.35, 0.28, 0.02, 0.01, 0.0}};
  AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array knots{0.0};
  const std::array target{0.31};
  auto quotes = synthetic_quotes(market, knots, target);
  std::reverse(quotes.begin(), quotes.end());
  std::vector initial{0.20};
  AmericanMarkovFitConfig config;
  config.initial_sigmas = initial;
  config.max_evaluations = 128;
  config.max_sweeps = 40;
  config.midpoint_weight = 0.01;
  auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->sigmas().size(), 1U);
  EXPECT_NEAR(fit->sigmas()[0], target[0], 0.0002);
  EXPECT_LT(fit->diagnostics().objective, fit->diagnostics().initial_objective * 1.0e-5);
  EXPECT_LT(fit->diagnostics().max_interval_error, 0.004);
  ASSERT_EQ(fit->quote_prices().size(), quotes.size());
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(fit->model()));
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &q = quotes[i];
    const auto value = price_american_markov(fit->model(), q.strike, q.side, q.expiry_index,
                                            workspace);
    ASSERT_TRUE(value);
    EXPECT_NEAR(*value, fit->quote_prices()[i], 1.0e-12);
  }
  const auto first_price = fit->quote_prices().front();
  const auto first_sigma = fit->sigmas().front();
  intervals.clear();
  intervals.shrink_to_fit();
  quotes.clear();
  initial[0] = 9.0;
  auto copy = *fit;
  EXPECT_EQ(copy.sigmas().front(), first_sigma);
  EXPECT_EQ(copy.quote_prices().front(), first_price);
  EXPECT_EQ(copy.model().interval_data()[0].local_volatility[0].volatility_multiplier,
            first_sigma);
}

TEST(AmericanMarkovFit, FlexibleShapeImprovesSkewAndPreservesCommonModelRelations) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.50, 0.50, 0.0, 0.0, 0.0}};
  AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array knots{-0.35, 0.0, 0.35};
  const std::array target{0.45, 0.27, 0.16};
  const auto quotes = synthetic_quotes(market, knots, target, false, 0.02);
  AmericanMarkovFitConfig config;
  config.max_evaluations = 400;
  config.max_sweeps = 80;
  const auto flat = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(flat);
  config.log_state_knots = knots;
  config.strike_roughness_weight = 0.01;
  const auto flexible = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(flexible) << flexible.error().to_string();
  EXPECT_EQ(flexible->sigmas().size(), 3U);
  EXPECT_GT(flexible->sigmas().front(), flexible->sigmas().back() + 0.1);
  EXPECT_LT(flexible->diagnostics().interval_loss, flat->diagnostics().interval_loss * 0.10);
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(flexible->model()));
  for (const auto side : {Side::Call, Side::Put}) {
    std::array<double, 31> earlier{};
    for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
      std::array<double, 31> prices{};
      for (std::size_t i = 0; i < prices.size(); ++i) {
        const double strike = 70.0 + 2.0 * static_cast<double>(i);
        const auto value =
            price_american_markov(flexible->model(), strike, side, expiry, workspace);
        ASSERT_TRUE(value);
        prices[i] = *value;
        EXPECT_GE(prices[i] + 1.0e-10, earlier[i]);
        if (i > 0) {
          const double slope = (prices[i] - prices[i - 1]) / 2.0;
          EXPECT_GE(slope, (side == Side::Call ? -1.0 : 0.0) - 1.0e-10);
          EXPECT_LE(slope, (side == Side::Call ? 0.0 : 1.0) + 1.0e-10);
        }
        if (i > 1) EXPECT_GE(prices[i] - 2.0 * prices[i - 1] + prices[i - 2], -1.0e-10);
      }
      earlier = prices;
    }
  }
}

TEST(AmericanMarkovFit, ActiveVarianceIntervalsHaveIndependentParametersAndCashPrefixes) {
  const std::array intervals{AmericanMarkovFitInterval{0.10, 0.10, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.15, 0.10, 0.0, 0.0, 2.0},
                             AmericanMarkovFitInterval{0.35, 0.30, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array knots{0.0};
  const std::array target{0.18, 0.40};
  const auto quotes = synthetic_quotes(market, knots, target, true, 0.01);
  AmericanMarkovFitConfig config;
  config.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  config.max_evaluations = 300;
  config.max_sweeps = 70;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->sigmas().size(), 2U);
  EXPECT_NEAR(fit->sigmas()[0], 0.18, 0.003);
  EXPECT_NEAR(fit->sigmas()[1], 0.40, 0.003);
  EXPECT_EQ(fit->time_shape(), AmericanMarkovFitTimeShape::PerVarianceInterval);
  EXPECT_TRUE(fit->variance_time_knots().empty());
  EXPECT_FALSE(fit->time_basis_diagnostics().has_value());
  // Legacy one-hot rows: the paused cash interval keeps group zero and is inactive.
  const auto weights = fit->interval_time_weights();
  ASSERT_EQ(weights.size(), 3U);
  for (std::size_t i = 0; i < weights.size(); ++i) {
    EXPECT_EQ(weights[i].left_group, i == 2 ? 1U : 0U);
    EXPECT_EQ(weights[i].right_group, weights[i].left_group);
    EXPECT_EQ(weights[i].right_weight, 0.0);
    EXPECT_EQ(weights[i].variance_active, i != 1);
  }
  EXPECT_DOUBLE_EQ(weights[0].variance_time, 0.05);
  EXPECT_DOUBLE_EQ(weights[1].variance_time, 0.10);
  EXPECT_DOUBLE_EQ(weights[2].variance_time, 0.20);
  EXPECT_EQ(fit->model().diagnostics().cash_steps, 1U);
  EXPECT_LT(fit->diagnostics().max_interval_error, 0.02);
}

TEST(AmericanMarkovFit, IndependentBlackScholesCallsExposeFiniteGridApproximation) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  market.model_config.grid_nodes = 129;
  const double target = 0.25;
  std::vector<AmericanMarkovFitQuote> quotes;
  for (const double strike : {90.0, 95.0, 100.0, 105.0, 110.0}) {
    const double mid = independent_call(100.0, strike, 0.25, target);
    quotes.push_back({0, strike, Side::Call, mid - 0.005, mid + 0.005, 1.0});
  }
  AmericanMarkovFitConfig config;
  config.initial_sigma = 0.20;
  config.midpoint_weight = 0.01;
  config.max_evaluations = 100;
  config.max_sweeps = 40;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  // This is a numerical approximation check, not an exact analytic recovery:
  // the finite grid shifts the fitted sigma and leaves measurable residuals.
  EXPECT_NEAR(fit->sigmas()[0], target, 0.004);
  EXPECT_LT(fit->diagnostics().max_interval_error, 0.04);
  EXPECT_GT(fit->diagnostics().max_interval_error, 1.0e-5);
  EXPECT_LT(fit->diagnostics().objective, fit->diagnostics().initial_objective * 0.01);
}

TEST(AmericanMarkovFit, BidlessQuotesDoNotCreateMidpointTargets) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array quote{AmericanMarkovFitQuote{0, 100.0, Side::Call, 0.0, 20.0, 1.0}};
  AmericanMarkovFitConfig config;
  config.midpoint_weight = 100.0;
  config.prior_weight = 1.0;
  const auto fit = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(fit);
  EXPECT_EQ(fit->sigmas()[0], config.initial_sigma);
  EXPECT_EQ(fit->diagnostics().objective, 0.0);
  EXPECT_EQ(fit->diagnostics().midpoint_loss, 0.0);
  EXPECT_EQ(fit->diagnostics().bidless_quotes, 1U);
  EXPECT_EQ(fit->diagnostics().quotes_inside_interval, 1U);
  EXPECT_EQ(fit->diagnostics().termination, AmericanMarkovFitTermination::StepTolerance);
  EXPECT_LT(fit->quote_prices()[0], 7.0); // Deliberately far below ask/2.
}

TEST(AmericanMarkovFit, PriorAndTimeRoughnessControlSparseParameterSelection) {
  const std::array intervals{AmericanMarkovFitInterval{0.10, 0.10, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.20, 0.20, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array quote{AmericanMarkovFitQuote{1, 100.0, Side::Call, 0.0, 50.0, 1.0}};
  const std::array initial{0.15, 0.45};
  const std::array prior{0.30, 0.30};
  AmericanMarkovFitConfig config;
  config.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  config.initial_sigmas = initial;
  config.prior_sigmas = prior;
  config.prior_weight = 1.0;
  config.time_roughness_weight = 1.0;
  config.max_sweeps = 80;
  const auto fit = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(fit);
  EXPECT_NEAR(fit->sigmas()[0], 0.30, 0.001);
  EXPECT_NEAR(fit->sigmas()[1], 0.30, 0.001);
  EXPECT_LT(fit->diagnostics().regularization_loss, 1.0e-5);
  EXPECT_EQ(fit->diagnostics().interval_loss, 0.0);
}

TEST(AmericanMarkovFit, EvaluationBudgetReturnsExplicitFiniteBestAndIsDeterministic) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array quote{AmericanMarkovFitQuote{0, 100.0, Side::Call, 9.0, 9.1, 1.0}};
  AmericanMarkovFitConfig config;
  config.max_evaluations = 1;
  const auto one = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(one);
  EXPECT_EQ(one->diagnostics().evaluations, 1U);
  EXPECT_EQ(one->diagnostics().termination, AmericanMarkovFitTermination::EvaluationLimit);
  EXPECT_EQ(one->diagnostics().objective, one->diagnostics().initial_objective);
  config.max_evaluations = 12;
  const auto a = fit_american_markov_intervals(market, quote, config);
  const auto b = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(a->diagnostics().evaluations, 12U);
  EXPECT_EQ(a->diagnostics().termination, AmericanMarkovFitTermination::EvaluationLimit);
  EXPECT_EQ(a->sigmas()[0], b->sigmas()[0]);
  EXPECT_EQ(a->quote_prices()[0], b->quote_prices()[0]);
  EXPECT_LE(a->diagnostics().objective, a->diagnostics().initial_objective);
  EXPECT_LE(a->diagnostics().best_evaluation, a->diagnostics().evaluations);
  config.max_evaluations = 128;
  config.max_sweeps = 1;
  const auto sweep = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(sweep);
  EXPECT_EQ(sweep->diagnostics().termination, AmericanMarkovFitTermination::SweepLimit);
  EXPECT_EQ(sweep->diagnostics().completed_sweeps, 1U);
}

TEST(AmericanMarkovFit, RefusedTrialCannotReplaceFiniteBestModel) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  market.model_config.max_transitions = 20;
  const std::array quote{AmericanMarkovFitQuote{0, 100.0, Side::Call, 9.0, 9.1, 1.0}};
  AmericanMarkovFitConfig config;
  config.initial_sigma = 0.10;
  config.initial_log_step = 1.0;
  config.max_evaluations = 3;
  const auto fit = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_GE(fit->diagnostics().failed_evaluations, 1U);
  EXPECT_EQ(fit->sigmas()[0], 0.10);
  EXPECT_TRUE(std::isfinite(fit->quote_prices()[0]));
}

TEST(AmericanMarkovFit, InvalidQuotesAndBudgetsNeverProduceModel) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const AmericanMarkovFitQuote quote{0, 100.0, Side::Call, 5.0, 5.1, 1.0};
  std::array quotes{quote};
  AmericanMarkovFitConfig config;
  for (const double bid : {-1.0, 5.2, std::numeric_limits<double>::quiet_NaN()}) {
    quotes[0].bid = bid;
    EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
  }
  quotes[0] = quote;
  const std::array duplicate{quote, quote};
  EXPECT_FALSE(fit_american_markov_intervals(market, duplicate, config));
  EXPECT_FALSE(fit_american_markov_intervals(market, {}, config));
  quotes[0].expiry_index = 1;
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
  quotes[0] = quote;
  config.max_evaluations = kAmericanMarkovFitMaxEvaluations + 1;
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
  config = {};
  config.max_batch_values = 1;
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
  config = {};
  config.max_rollback_work = 1;
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
  config = {};
  const std::array knots{-1.0, 0.0, 1.0};
  config.log_state_knots = knots;
  config.max_parameters = 2;
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
  config = {};
  const std::array wrong_initial{0.20, 0.30};
  config.initial_sigmas = wrong_initial;
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
}

TEST(AmericanMarkovFit, ExplicitInitialVectorAndExactBoundsAreRespected) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array quote{AmericanMarkovFitQuote{0, 100.0, Side::Call, 50.0, 51.0, 1.0}};
  const std::array initial{0.35};
  AmericanMarkovFitConfig config;
  config.min_sigma = 0.30;
  config.max_sigma = 0.40;
  config.initial_sigmas = initial; // The unused default scalar is below the bound.
  const auto fit = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_DOUBLE_EQ(fit->sigmas()[0], config.max_sigma);
  EXPECT_GT(fit->diagnostics().max_interval_error, 30.0);
  EXPECT_EQ(fit->diagnostics().quotes_inside_interval, 0U);
  EXPECT_LE(fit->diagnostics().evaluations, config.max_evaluations);
}

TEST(AmericanMarkovFit, WeightRescalingAndBatchChunkingPreserveBestModel) {
  const std::array intervals{AmericanMarkovFitInterval{0.10, 0.10, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array knots{0.0};
  const std::array target{0.30};
  auto quotes = synthetic_quotes(market, knots, target);
  AmericanMarkovFitConfig config;
  config.batch_options = 1;
  config.max_evaluations = 40;
  const auto single = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(single);
  for (auto &quote : quotes) quote.weight = 1.0e-300;
  config.batch_options = 7; // Crosses both side and expiry group boundaries.
  const auto chunked = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(chunked);
  EXPECT_EQ(single->sigmas()[0], chunked->sigmas()[0]);
  EXPECT_NEAR(single->diagnostics().objective, chunked->diagnostics().objective, 1.0e-10);
  EXPECT_GT(chunked->diagnostics().initial_objective, 1.0);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    EXPECT_NEAR(single->quote_prices()[i], chunked->quote_prices()[i], 1.0e-12);
  }
  const std::array overflow{AmericanMarkovFitQuote{0, 100.0, Side::Call, 1.0e308, 1.0e308, 1.0}};
  EXPECT_FALSE(fit_american_markov_intervals(market, overflow, config));
}

TEST(AmericanMarkovFit, MalformedSharedScheduleAndNonfiniteInputsAreRefused) {
  const AmericanMarkovFitInterval first{0.10, 0.10, 0.0, 0.0, 0.0};
  const AmericanMarkovFitInterval second{0.25, 0.25, 0.0, 0.0, 0.0};
  std::array intervals{first, second};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array quote{AmericanMarkovFitQuote{1, 100.0, Side::Put, 5.0, 5.1, 1.0}};
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const auto field : {&AmericanMarkovFitInterval::calendar_end,
                           &AmericanMarkovFitInterval::variance_end,
                           &AmericanMarkovFitInterval::rate,
                           &AmericanMarkovFitInterval::continuous_yield,
                           &AmericanMarkovFitInterval::cash_dividend_at_end}) {
    intervals[1] = second;
    intervals[1].*field = nan;
    EXPECT_FALSE(fit_american_markov_intervals(market, quote));
  }
  intervals[1] = second;
  intervals[1].calendar_end = first.calendar_end;
  EXPECT_FALSE(fit_american_markov_intervals(market, quote));
  intervals[1] = second;
  intervals[1].variance_end = 0.05;
  EXPECT_FALSE(fit_american_markov_intervals(market, quote));
  intervals[1] = second;
  intervals[1].cash_dividend_at_end = -1.0;
  EXPECT_FALSE(fit_american_markov_intervals(market, quote));
  intervals[1] = second;
  AmericanMarkovFitConfig config;
  const std::array invalid_knots{0.0, nan};
  config.log_state_knots = invalid_knots;
  EXPECT_FALSE(fit_american_markov_intervals(market, quote, config));
  config = {};
  const std::array invalid_sigma{nan};
  config.initial_sigmas = invalid_sigma;
  EXPECT_FALSE(fit_american_markov_intervals(market, quote, config));
  config = {};
  config.prior_sigmas = invalid_sigma;
  EXPECT_FALSE(fit_american_markov_intervals(market, quote, config));
}

TEST(AmericanMarkovFit, SubnormalWeightRetainsFiniteLargeResidualLoss) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const double tiny = std::numeric_limits<double>::denorm_min();
  const std::array quotes{
      AmericanMarkovFitQuote{0, 100.0, Side::Call, 0.0, 20.0, 1.0e6},
      AmericanMarkovFitQuote{0, 110.0, Side::Call, 1.0e148, 1.0e148, tiny}};
  AmericanMarkovFitConfig config;
  config.price_scale_floor = 1.0e-12;
  config.midpoint_weight = 0.5;
  config.max_evaluations = 1;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  // Factored reference avoids representing 1e320 or tiny/1e6. The model's
  // bounded stock-scale price is negligible relative to this valid quote.
  const double expected = tiny * 1.0e308 * 1.0e6;
  EXPECT_GT(expected, 1.0e-10);
  EXPECT_NEAR(fit->diagnostics().interval_loss, expected, expected * 1.0e-14);
  EXPECT_NEAR(fit->diagnostics().midpoint_loss, 0.5 * expected, expected * 1.0e-14);
  EXPECT_NEAR(fit->diagnostics().objective, 1.5 * expected, expected * 1.0e-14);
}

TEST(AmericanMarkovFit, MidpointCoefficientPrecedesSquaringAtTinyStockScale) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{1.0e-160, intervals, {}, {}};
  const std::array quote{
      AmericanMarkovFitQuote{0, 1.0e-160, Side::Call, 1.0e-160, 2.0e-160, 1.0}};
  AmericanMarkovFitConfig config;
  config.price_scale_floor = 100.0;
  config.midpoint_weight = 1.0e6;
  config.max_evaluations = 1;
  const auto fit = fit_american_markov_intervals(market, quote, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  // With the coefficient applied after squaring, the unweighted squared
  // midpoint residual rounds to zero. Its weighted contribution is representable.
  EXPECT_GT(fit->diagnostics().midpoint_loss, 1.0e-319);
  EXPECT_LT(fit->diagnostics().midpoint_loss, 1.0e-317);
}

TEST(AmericanMarkovFit, ResolventQuotesChargeBothPassesAgainstRollbackBudget) {
  const std::array grid{0.0, 100.0, 200.0};
  const std::array intervals{AmericanMarkovFitInterval{0.1, 0.1, 0.0, 0.0, 0.0}};
  AmericanMarkovFitMarket market{100.0, intervals, grid, {}};
  market.model_config.diffusion_method = AmericanMarkovDiffusionMethod::VarianceResolvent;
  market.model_config.max_calendar_step = 1.0;
  market.model_config.max_variance_step = 1.0;
  const std::array quotes{AmericanMarkovFitQuote{0, 100.0, Side::Call, 0.0, 100.0, 1.0},
                          AmericanMarkovFitQuote{0, 100.0, Side::Put, 0.0, 100.0, 1.0}};
  AmericanMarkovFitConfig config;
  config.max_evaluations = 1;
  config.max_rollback_work = 12;
  const auto exact = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(exact) << exact.error().to_string();
  EXPECT_EQ(exact->model().diagnostics().stored_rows, 3U);
  EXPECT_EQ(exact->model().diagnostics().transition_work, 6U);
  config.max_rollback_work = 11;
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
  config.max_rollback_work = 6; // Enough for one pass per quote, insufficient for the solve.
  EXPECT_FALSE(fit_american_markov_intervals(market, quotes, config));
}

TEST(AmericanMarkovFit, ResolventFitRetainsSelectedCommonProcess) {
  const std::array intervals{AmericanMarkovFitInterval{0.2, 0.15, 0.02, 0.0, 1.0}};
  AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  market.model_config.diffusion_method = AmericanMarkovDiffusionMethod::VarianceResolvent;
  const std::array knots{0.0};
  const std::array sigmas{0.31};
  const auto quotes = synthetic_quotes(market, knots, sigmas, false, 0.001);
  AmericanMarkovFitConfig config;
  config.initial_sigma = 0.20;
  config.min_sigma = 0.05;
  config.max_sigma = 0.8;
  config.max_evaluations = 100;
  config.max_sweeps = 32;
  config.min_log_step = 1.0e-4;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->model().diffusion_method(), AmericanMarkovDiffusionMethod::VarianceResolvent);
  EXPECT_NEAR(fit->sigmas().front(), 0.31, 0.001);
  EXPECT_LT(fit->diagnostics().max_interval_error, 0.002);
}

// ---------------------------------------------------------------------------
// VarianceTimeKnots. The dense references below share no code or recurrence
// with the streaming Givens factorization or with the fitter's interpolation.
// ---------------------------------------------------------------------------

std::uint64_t bits_of(double value) { return std::bit_cast<std::uint64_t>(value); }

std::string decimal(double value) {
  std::array<char, 64> text{};
  const int written = std::snprintf(text.data(), text.size(), "%.17g", value);
  return written > 0 ? std::string(text.data()) : std::string("nan");
}

// Row-major dense design of the represented interval log-sigma map. Only
// positive-variance rows contribute, as documented for the fitter's admission.
struct DenseDesign {
  std::size_t rows = 0;
  std::size_t columns = 0;
  std::vector<double> values;
  double &at(std::size_t row, std::size_t column) { return values[row * columns + column]; }
  double at(std::size_t row, std::size_t column) const {
    return values[row * columns + column];
  }
};

DenseDesign dense_design(std::span<const AmericanMarkovFitTimeWeight> prefix,
                         std::size_t groups) {
  DenseDesign out{0, groups, {}};
  for (const auto &row : prefix) {
    if (!row.variance_active) continue;
    out.values.resize(out.values.size() + groups, 0.0);
    const auto r = out.rows++;
    if (row.left_group == row.right_group) {
      out.at(r, row.left_group) = 1.0;
    } else {
      out.at(r, row.left_group) = 1.0 - row.right_weight;
      out.at(r, row.right_group) = row.right_weight;
    }
  }
  return out;
}

// Householder QR, then an explicit triangular inverse: 1/(|R|_inf |R^-1|_inf).
double dense_reciprocal_condition_inf(DenseDesign d) {
  const auto m = d.rows;
  const auto n = d.columns;
  std::vector<double> v(m, 0.0);
  for (std::size_t k = 0; k < n; ++k) {
    double norm = 0.0;
    for (std::size_t i = k; i < m; ++i) norm = std::hypot(norm, d.at(i, k));
    if (norm == 0.0) return 0.0;
    const double alpha = d.at(k, k) > 0.0 ? -norm : norm;
    double vv = 0.0;
    for (std::size_t i = k; i < m; ++i) {
      v[i] = d.at(i, k) - (i == k ? alpha : 0.0);
      vv += v[i] * v[i];
    }
    for (std::size_t j = k; j < n; ++j) {
      double dot = 0.0;
      for (std::size_t i = k; i < m; ++i) dot += v[i] * d.at(i, j);
      const double scale = 2.0 * dot / vv;
      for (std::size_t i = k; i < m; ++i) d.at(i, j) -= scale * v[i];
    }
  }
  double r_norm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (d.at(i, i) == 0.0) return 0.0; // Exactly singular.
    double row = 0.0;
    for (std::size_t j = i; j < n; ++j) row += std::abs(d.at(i, j));
    r_norm = std::max(r_norm, row);
  }
  std::vector<double> inverse(n * n, 0.0);
  for (std::size_t c = 0; c < n; ++c) {
    for (std::size_t i = n; i-- > 0;) {
      double value = i == c ? 1.0 : 0.0;
      for (std::size_t j = i + 1; j < n; ++j) value -= d.at(i, j) * inverse[j * n + c];
      inverse[i * n + c] = value / d.at(i, i);
    }
  }
  double inverse_norm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    double row = 0.0;
    for (std::size_t c = 0; c < n; ++c) row += std::abs(inverse[i * n + c]);
    inverse_norm = std::max(inverse_norm, row);
  }
  return std::isfinite(inverse_norm) ? 1.0 / (r_norm * inverse_norm) : 0.0;
}

void expect_matches_dense(const AmericanMarkovFitTimeBasisDiagnostics &basis,
                          std::span<const AmericanMarkovFitTimeWeight> prefix,
                          std::size_t groups) {
  const auto dense = dense_design(prefix, groups);
  EXPECT_EQ(basis.quoted_variance_intervals, dense.rows);
  EXPECT_EQ(basis.common_normalization, std::sqrt(static_cast<double>(dense.rows)));
  ASSERT_EQ(basis.column_support_counts.size(), groups);
  ASSERT_EQ(basis.column_l2_norms.size(), groups);
  for (std::size_t j = 0; j < groups; ++j) {
    std::size_t support = 0;
    double squares = 0.0;
    for (std::size_t i = 0; i < dense.rows; ++i) {
      support += dense.at(i, j) > 0.0 ? 1U : 0U;
      squares += dense.at(i, j) * dense.at(i, j);
    }
    EXPECT_EQ(basis.column_support_counts[j], support) << "column " << j;
    EXPECT_NEAR(basis.column_l2_norms[j], std::sqrt(squares), 1.0e-13 * std::sqrt(squares))
        << "column " << j;
  }
  // Both factorizations are backward stable, so the absolute rcond discrepancy
  // scales like n*eps, independent of how ill-conditioned the design is.
  const double reference = dense_reciprocal_condition_inf(dense);
  EXPECT_NEAR(basis.qr_reciprocal_condition_inf, reference,
              64.0 * static_cast<double>(groups) * std::numeric_limits<double>::epsilon())
      << "dense reciprocal condition " << decimal(reference);
}

std::vector<AmericanMarkovFitTimeWeight> knot_rows(std::span<const double> midpoints,
                                                   std::span<const double> knots) {
  std::vector<AmericanMarkovFitTimeWeight> out;
  for (const double time : midpoints) {
    const auto row = detail::markov_time_weight(time, true, knots);
    EXPECT_TRUE(row) << "time " << decimal(time);
    if (row) out.push_back(*row);
  }
  return out;
}

// Zero-carry schedule whose calendar and variance clocks agree at every end.
std::vector<AmericanMarkovFitInterval> clock_schedule(std::span<const double> ends) {
  std::vector<AmericanMarkovFitInterval> out;
  for (const double end : ends) out.push_back({end, end, 0.0, 0.0, 0.0});
  return out;
}

// Wide bidless upper bounds, one per expiry: valid quotes that leave the
// represented operator, not the objective, as the subject of a test.
std::vector<AmericanMarkovFitQuote> wide_quotes(std::size_t expiries) {
  std::vector<AmericanMarkovFitQuote> out;
  for (std::size_t expiry = 0; expiry < expiries; ++expiry) {
    out.push_back({expiry, 100.0, Side::Call, 0.0, 100.0, 1.0});
  }
  return out;
}

AmericanMarkovFitConfig knot_config(std::span<const double> time_knots) {
  AmericanMarkovFitConfig config;
  config.time_shape = AmericanMarkovFitTimeShape::VarianceTimeKnots;
  config.variance_time_knots = time_knots;
  config.max_evaluations = 1;
  return config;
}

// Independent continuous flat-tailed log-linear sigma(variance time).
double knot_curve(double time, std::span<const double> knots, std::span<const double> controls) {
  if (time <= knots.front()) return controls.front();
  if (time >= knots.back()) return controls.back();
  std::size_t right = 1;
  while (knots[right] < time) ++right;
  const double weight = (time - knots[right - 1]) / (knots[right] - knots[right - 1]);
  return controls[right - 1] * std::pow(controls[right] / controls[right - 1], weight);
}

// Exact integral of knot_curve^2 over [a, b]. Between knots sigma^2 is exactly
// exponential in variance time, so each piece has a closed form.
double exact_integrated_variance(double a, double b, std::span<const double> knots,
                                 std::span<const double> controls) {
  double total = 0.0;
  double left = a;
  for (std::size_t cell = 0; cell <= knots.size() && left < b; ++cell) {
    const double edge = cell < knots.size() ? std::clamp(knots[cell], a, b) : b;
    if (!(edge > left)) continue;
    if (cell == 0 || cell == knots.size()) {
      const double sigma = cell == 0 ? controls.front() : controls.back();
      total += sigma * sigma * (edge - left);
    } else {
      const double rate = 2.0 * std::log(controls[cell] / controls[cell - 1]) /
                          (knots[cell] - knots[cell - 1]);
      const double base = controls[cell - 1] * controls[cell - 1] *
                          std::exp(rate * (left - knots[cell - 1]));
      total += rate == 0.0 ? base * (edge - left) : base * std::expm1(rate * (edge - left)) / rate;
    }
    left = edge;
  }
  return total;
}

void expect_same_operator(const AmericanMarkovModel &a, const AmericanMarkovModel &b) {
  ASSERT_EQ(a.layers().size(), b.layers().size());
  for (std::size_t i = 0; i < a.layers().size(); ++i) {
    const auto &x = a.layers()[i];
    const auto &y = b.layers()[i];
    EXPECT_EQ(bits_of(x.calendar_time), bits_of(y.calendar_time));
    EXPECT_EQ(bits_of(x.variance_time), bits_of(y.variance_time));
    EXPECT_EQ(bits_of(x.growth), bits_of(y.growth));
    ASSERT_EQ(x.states().size(), y.states().size());
    for (std::size_t j = 0; j < x.states().size(); ++j) {
      EXPECT_EQ(bits_of(x.states()[j]), bits_of(y.states()[j]));
    }
  }
  ASSERT_EQ(a.transitions().size(), b.transitions().size());
  for (std::size_t i = 0; i < a.transitions().size(); ++i) {
    const auto &x = a.transitions()[i];
    const auto &y = b.transitions()[i];
    EXPECT_EQ(x.kind, y.kind);
    EXPECT_EQ(x.interval_index, y.interval_index);
    for (const auto field : {&AmericanMarkovTransition::discount,
                             &AmericanMarkovTransition::calendar_step,
                             &AmericanMarkovTransition::variance_step,
                             &AmericanMarkovTransition::volatility,
                             &AmericanMarkovTransition::cash_dividend}) {
      EXPECT_EQ(bits_of(x.*field), bits_of(y.*field)) << "transition " << i;
    }
    ASSERT_EQ(x.rows().size(), y.rows().size());
    for (std::size_t r = 0; r < x.rows().size(); ++r) {
      EXPECT_EQ(x.rows()[r].destination, y.rows()[r].destination);
      for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_EQ(bits_of(x.rows()[r].probability[k]), bits_of(y.rows()[r].probability[k]));
      }
    }
    ASSERT_EQ(x.resolvent_factors().size(), y.resolvent_factors().size());
    for (std::size_t r = 0; r < x.resolvent_factors().size(); ++r) {
      const auto &f = x.resolvent_factors()[r];
      const auto &g = y.resolvent_factors()[r];
      EXPECT_EQ(bits_of(f.forward_input_weight), bits_of(g.forward_input_weight));
      EXPECT_EQ(bits_of(f.backward_input_weight), bits_of(g.backward_input_weight));
    }
  }
  ASSERT_EQ(a.interval_data().size(), b.interval_data().size());
  for (std::size_t i = 0; i < a.interval_data().size(); ++i) {
    const auto &x = a.interval_data()[i];
    const auto &y = b.interval_data()[i];
    EXPECT_EQ(x.first_layer, y.first_layer);
    ASSERT_EQ(x.state_volatilities.size(), y.state_volatilities.size());
    for (std::size_t j = 0; j < x.state_volatilities.size(); ++j) {
      EXPECT_EQ(bits_of(x.state_volatilities[j]), bits_of(y.state_volatilities[j]));
    }
  }
  EXPECT_EQ(a.diagnostics().transition_work, b.diagnostics().transition_work);
  EXPECT_EQ(a.diagnostics().stored_rows, b.diagnostics().stored_rows);
  EXPECT_EQ(a.diagnostics().stored_volatility_values, b.diagnostics().stored_volatility_values);
}

TEST(AmericanMarkovTimeBasis, WeightsHonorExactKnotsFlatTailsAndRefuseLostCoefficients) {
  const std::array knots{0.125, 0.5, 1.0};
  const auto check = [&](double time, bool active, std::size_t left, std::size_t right,
                         double weight) {
    const auto row = detail::markov_time_weight(time, active, knots);
    ASSERT_TRUE(row) << row.error().to_string();
    EXPECT_EQ(row->left_group, left) << decimal(time);
    EXPECT_EQ(row->right_group, right) << decimal(time);
    EXPECT_EQ(row->right_weight, weight) << decimal(time);
    EXPECT_EQ(row->variance_time, time);
    EXPECT_EQ(row->variance_active, active);
  };
  check(0.0, true, 0, 0, 0.0);      // Flat left tail.
  check(0.125, true, 0, 0, 0.0);    // Exact first knot.
  check(0.3125, false, 0, 1, 0.5);  // Interior, dyadic weight; paused rows keep the map.
  check(0.5, true, 1, 1, 0.0);      // Exact interior knot is one-hot.
  check(0.875, true, 1, 2, 0.75);
  check(1.0, true, 2, 2, 0.0);      // Exact last knot.
  check(7.0, true, 2, 2, 0.0);      // Flat right tail.
  // A subnormal interior coefficient underflows to zero: refused, never nudged.
  const std::array wide{0.0, 1.0e10};
  const auto lost = detail::markov_time_weight(1.0e-320, true, wide);
  ASSERT_FALSE(lost);
  EXPECT_EQ(lost.error().code(), ErrorCode::OutOfRange);
}

TEST(AmericanMarkovTimeBasis, StreamingQrMatchesIndependentDenseHouseholderReference) {
  // Tails, exact knots and interior rows on five uneven columns.
  const std::array uneven{0.05, 0.1, 0.25, 0.5, 1.0};
  const std::array mixed{0.01, 0.03, 0.05, 0.07, 0.1, 0.12, 0.2, 0.3,
                         0.45, 0.5, 0.6, 0.8, 1.0, 1.3, 2.0};
  // Long bidiagonal chain at the 128-parameter hard cap with 300 rows.
  std::vector<double> chain(kAmericanMarkovFitMaxParameters);
  for (std::size_t k = 0; k < chain.size(); ++k) {
    chain[k] = 2.0 * static_cast<double>(k) / static_cast<double>(chain.size() - 1);
  }
  std::vector<double> dense_midpoints(300);
  for (std::size_t i = 0; i < dense_midpoints.size(); ++i) {
    dense_midpoints[i] = 2.0 * (static_cast<double>(i) + 0.5) / 300.0;
  }
  // Nearly dependent columns and a weak column, both still admissible.
  const std::array pair{0.0, 1.0};
  const std::array nearly{0.5, 0.5 + 1.0e-6};
  const std::array triple{0.0, 1.0, 2.0};
  const std::array weak{0.0, 0.5, 1.0, 1.0 + 1.0e-9};
  struct Case {
    std::span<const double> knots;
    std::span<const double> midpoints;
    double max_rcond;
  };
  const std::array cases{Case{uneven, mixed, 1.0}, Case{chain, dense_midpoints, 1.0},
                         Case{pair, nearly, 1.0e-5}, Case{triple, weak, 1.0e-8}};
  for (const auto &c : cases) {
    const auto rows = knot_rows(c.midpoints, c.knots);
    const auto basis = detail::diagnose_markov_time_basis(rows, c.knots.size());
    ASSERT_TRUE(basis) << basis.error().to_string();
    expect_matches_dense(*basis, rows, c.knots.size());
    EXPECT_LE(basis->qr_reciprocal_condition_inf, c.max_rcond);
    EXPECT_GE(basis->qr_reciprocal_condition_inf,
              kAmericanMarkovFitMinTimeBasisReciprocalCondition);
  }
}

TEST(AmericanMarkovTimeBasis, RefusesIllConditionedDependentAndUnsupportedColumns) {
  const auto refuse = [](std::span<const double> knots, std::span<const double> midpoints) {
    const auto rows = knot_rows(midpoints, knots);
    const auto basis = detail::diagnose_markov_time_basis(rows, knots.size());
    ASSERT_FALSE(basis);
    EXPECT_EQ(basis.error().code(), ErrorCode::OutOfRange);
    // Independent confirmation that the refusal is not a streaming artifact.
    const auto dense = dense_design(rows, knots.size());
    if (dense.rows >= dense.columns) {
      EXPECT_LT(dense_reciprocal_condition_inf(dense),
                kAmericanMarkovFitMinTimeBasisReciprocalCondition);
    }
  };
  const std::array pair{0.0, 1.0};
  const std::array triple{0.0, 1.0, 2.0};
  refuse(pair, std::array{0.5, 0.5});                      // Exactly dependent.
  refuse(pair, std::array{0.5, 0.5 + 4.0e-15});            // Below the 1e-12 threshold.
  refuse(triple, std::array{0.0, 0.5, 1.0, 1.0 + 1.0e-14}); // Weak column.
  refuse(triple, std::array{0.0, 0.0, 2.0});               // Unsupported middle column.
  refuse(triple, std::array{0.25, 1.5});                   // Fewer rows than columns.
}

TEST(AmericanMarkovTimeBasis, RefusesUnorderedOrInvalidRowsAndCapacity) {
  using Row = AmericanMarkovFitTimeWeight;
  const auto code = [](std::vector<Row> rows, std::size_t groups) {
    const auto basis = detail::diagnose_markov_time_basis(rows, groups);
    return basis ? std::optional<ErrorCode>{} : std::optional{basis.error().code()};
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Row a{0, 1, 0.5, 1.0, true};
  const Row b{1, 2, 0.5, 2.0, true};
  const Row c{2, 2, 0.0, 3.0, true};
  EXPECT_EQ(code({a, b, c}, 3), std::nullopt);
  for (const auto &rows : std::vector<std::vector<Row>>{
           {b, a, c},                                  // Left group decreases.
           {a, b, Row{1, 1, 0.0, 2.5, true}, c},        // Right group decreases.
           {Row{0, 2, 0.5, 1.0, true}, b, c},           // Non-adjacent support.
           {Row{1, 0, 0.5, 1.0, true}, b, c},           // Reversed support.
           {Row{0, 1, -0.1, 1.0, true}, b, c},          // Negative weight.
           {Row{0, 1, 1.1, 1.0, true}, b, c},           // Weight above one.
           {Row{0, 1, nan, 1.0, true}, b, c},           // Nonfinite weight.
           {Row{0, 0, 0.3, 1.0, true}, b, c},           // One-hot row with a weight.
           {a, b, Row{3, 3, 0.0, 3.0, true}},           // Group outside the basis.
           {a, Row{1, 2, 0.5, 0.5, true}, c},           // Variance time decreases.
           {a, Row{1, 2, 0.5, nan, true}, c}}) {        // Nonfinite variance time.
    EXPECT_EQ(code(rows, 3), ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(code({a, b, c}, 1), ErrorCode::OutOfRange);
  EXPECT_EQ(code({a, b, c}, kAmericanMarkovFitMaxParameters + 1), ErrorCode::OutOfRange);
  EXPECT_EQ(code({}, 3), ErrorCode::OutOfRange);
  // Paused rows contribute nothing and are never counted as quoted support.
  const Row paused{0, 0, 0.0, 1.5, false};
  const auto with_pause = detail::diagnose_markov_time_basis(std::array{a, paused, b, c}, 3);
  ASSERT_TRUE(with_pause);
  EXPECT_EQ(with_pause->quoted_variance_intervals, 3U);
  EXPECT_EQ(code({a, paused, c}, 3), ErrorCode::OutOfRange);
}

TEST(AmericanMarkovFit, TimeKnotBasisUsesFullQuotedPrefixAndSkipsPausedIntervals) {
  // Dyadic clocks make every represented midpoint exact.
  const std::array intervals{AmericanMarkovFitInterval{0.125, 0.125, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.3125, 0.25, 0.0, 0.0, 1.0},
                             AmericanMarkovFitInterval{0.5, 0.5, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.75, 0.75, 0.0, 0.0, 0.0}};
  AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  market.model_config.grid_nodes = 33;
  // Only interval 3 is quoted; intervals 0 and 1 are unquoted intermediates.
  std::vector<AmericanMarkovFitQuote> quotes{{3, 100.0, Side::Call, 0.0, 50.0, 1.0},
                                             {3, 100.0, Side::Put, 0.0, 50.0, 1.0}};
  const std::array exact{0.0625, 0.1875, 0.375};
  auto config = knot_config(exact);
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto weights = fit->interval_time_weights();
  ASSERT_EQ(weights.size(), intervals.size());
  const std::array<std::array<std::size_t, 2>, 5> groups{{{0, 0}, {1, 1}, {1, 2}, {2, 2},
                                                          {2, 2}}};
  const std::array right_weights{0.0, 0.0, (0.25 - 0.1875) / (0.375 - 0.1875), 0.0, 0.0};
  const std::array midpoints{0.0625, 0.1875, 0.25, 0.375, 0.625};
  for (std::size_t i = 0; i < weights.size(); ++i) {
    EXPECT_EQ(weights[i].left_group, groups[i][0]) << i;
    EXPECT_EQ(weights[i].right_group, groups[i][1]) << i;
    EXPECT_EQ(weights[i].right_weight, right_weights[i]) << i;
    EXPECT_EQ(weights[i].variance_time, midpoints[i]) << i;
    EXPECT_EQ(weights[i].variance_active, i != 2) << i;
  }
  const auto &basis = fit->time_basis_diagnostics();
  ASSERT_TRUE(basis);
  // Intervals 0, 1 and 3: the paused interval and post-quote interval 4 are excluded.
  EXPECT_EQ(basis->quoted_variance_intervals, 3U);
  EXPECT_EQ(basis->column_support_counts, (std::vector<std::size_t>{1, 1, 1}));
  EXPECT_EQ(basis->qr_reciprocal_condition_inf, 1.0);
  expect_matches_dense(*basis, weights.first(4), exact.size());
  // A control supported only after the final quoted expiry is refused.
  const std::array beyond{0.0625, 0.1875, 0.375, 0.625};
  config.variance_time_knots = beyond;
  const auto refused = fit_american_markov_intervals(market, quotes, config);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
  quotes.push_back({4, 100.0, Side::Call, 0.0, 50.0, 1.0});
  const auto extended = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(extended) << extended.error().to_string();
  EXPECT_EQ(extended->time_basis_diagnostics()->quoted_variance_intervals, 4U);
  // Interior interpolation over the same prefix, checked against the dense reference.
  const std::array outer{0.0, 0.875};
  config.variance_time_knots = outer;
  quotes.pop_back();
  const auto interior = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(interior) << interior.error().to_string();
  expect_matches_dense(*interior->time_basis_diagnostics(),
                       interior->interval_time_weights().first(4), outer.size());
}

TEST(AmericanMarkovFit, EqualTimeKnotControlsReproduceSharedOperatorAndPriceBits) {
  const std::array schedule{AmericanMarkovFitInterval{0.05, 0.04, 0.02, 0.01, 1.0},
                            AmericanMarkovFitInterval{0.07, 0.04, -0.01, 0.0, 0.0},
                            AmericanMarkovFitInterval{0.12, 0.10, 0.03, 0.0, 0.0}};
  const std::array state_knots{-0.2, 0.0, 0.2};
  const std::array shared_initial{0.31, 0.24, 0.19};
  std::vector<double> knot_initial;
  for (int group = 0; group < 2; ++group) {
    knot_initial.insert(knot_initial.end(), shared_initial.begin(), shared_initial.end());
  }
  const std::array time_knots{0.0, 0.2};
  std::vector<AmericanMarkovFitQuote> quotes;
  for (std::size_t expiry = 0; expiry < schedule.size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      for (const double strike : {90.0, 100.0, 110.0}) {
        quotes.push_back({expiry, strike, side, 0.5, 4.0, 1.0});
      }
    }
  }
  for (const auto method : {AmericanMarkovDiffusionMethod::ExplicitEuler,
                            AmericanMarkovDiffusionMethod::VarianceResolvent}) {
    AmericanMarkovFitMarket market{100.0, schedule, {}, {}};
    market.model_config.grid_nodes = 33;
    market.model_config.diffusion_method = method;
    market.model_config.max_calendar_step = 0.02;
    market.model_config.max_variance_step = 0.02;
    AmericanMarkovFitConfig shared;
    shared.log_state_knots = state_knots;
    shared.initial_sigmas = shared_initial;
    shared.max_evaluations = 1;
    shared.midpoint_weight = 0.25;
    shared.prior_weight = 0.5;
    shared.time_roughness_weight = 0.7;
    auto knots = shared;
    knots.time_shape = AmericanMarkovFitTimeShape::VarianceTimeKnots;
    knots.variance_time_knots = time_knots;
    knots.initial_sigmas = knot_initial;
    for (const double strike_weight : {0.0, 0.4}) {
      shared.strike_roughness_weight = knots.strike_roughness_weight = strike_weight;
      const auto a = fit_american_markov_intervals(market, quotes, shared);
      const auto b = fit_american_markov_intervals(market, quotes, knots);
      ASSERT_TRUE(a) << a.error().to_string();
      ASSERT_TRUE(b) << b.error().to_string();
      expect_same_operator(a->model(), b->model());
      ASSERT_EQ(a->quote_prices().size(), b->quote_prices().size());
      for (std::size_t i = 0; i < quotes.size(); ++i) {
        EXPECT_EQ(bits_of(a->quote_prices()[i]), bits_of(b->quote_prices()[i])) << i;
      }
      const auto &x = a->diagnostics();
      const auto &y = b->diagnostics();
      EXPECT_EQ(bits_of(x.interval_loss), bits_of(y.interval_loss));
      EXPECT_EQ(bits_of(x.midpoint_loss), bits_of(y.midpoint_loss));
      EXPECT_EQ(bits_of(x.max_interval_error), bits_of(y.max_interval_error));
      if (strike_weight == 0.0) {
        EXPECT_EQ(bits_of(x.objective), bits_of(y.objective));
        EXPECT_EQ(y.regularization_loss, 0.0);
      } else {
        // Same mean penalty; only the sum's rounding order differs (2 vs 6 terms).
        EXPECT_GT(y.regularization_loss, 0.0);
        EXPECT_NEAR(y.regularization_loss, x.regularization_loss,
                    4.0e-15 * x.regularization_loss);
      }
      EXPECT_EQ(y.parameter_count, 6U);
      EXPECT_EQ(y.time_groups, 2U);
      EXPECT_FALSE(b->interval_time_weights()[1].variance_active);
    }
  }
}

TEST(AmericanMarkovFit, TimeKnotProvenanceIsOwnedAcrossCopiesAndMoves) {
  const std::array ends{0.125, 0.25, 0.5};
  const auto schedule = clock_schedule(ends);
  AmericanMarkovFitMarket market{100.0, schedule, {}, {}};
  market.model_config.grid_nodes = 33;
  std::vector<double> knots{0.0, 0.125, 0.5};
  const auto quotes = wide_quotes(ends.size());
  auto fit = fit_american_markov_intervals(market, quotes, knot_config(knots));
  ASSERT_TRUE(fit) << fit.error().to_string();
  const std::vector<double> expected_knots = knots;
  const std::vector<AmericanMarkovFitTimeWeight> expected_weights(
      fit->interval_time_weights().begin(), fit->interval_time_weights().end());
  ASSERT_TRUE(fit->time_basis_diagnostics());
  const auto expected_basis = *fit->time_basis_diagnostics();
  knots.assign(3, 9.0);
  knots.clear();
  knots.shrink_to_fit();
  const auto check = [&](const AmericanMarkovFit &f) {
    EXPECT_EQ(f.time_shape(), AmericanMarkovFitTimeShape::VarianceTimeKnots);
    EXPECT_TRUE(std::equal(f.variance_time_knots().begin(), f.variance_time_knots().end(),
                           expected_knots.begin(), expected_knots.end()));
    ASSERT_EQ(f.interval_time_weights().size(), expected_weights.size());
    for (std::size_t i = 0; i < expected_weights.size(); ++i) {
      const auto &row = f.interval_time_weights()[i];
      EXPECT_EQ(row.left_group, expected_weights[i].left_group);
      EXPECT_EQ(row.right_group, expected_weights[i].right_group);
      EXPECT_EQ(row.right_weight, expected_weights[i].right_weight);
      EXPECT_EQ(row.variance_time, expected_weights[i].variance_time);
      EXPECT_EQ(row.variance_active, expected_weights[i].variance_active);
    }
    ASSERT_TRUE(f.time_basis_diagnostics());
    const auto &basis = *f.time_basis_diagnostics();
    EXPECT_EQ(basis.quoted_variance_intervals, expected_basis.quoted_variance_intervals);
    EXPECT_EQ(basis.column_support_counts, expected_basis.column_support_counts);
    EXPECT_EQ(basis.column_l2_norms, expected_basis.column_l2_norms);
    EXPECT_EQ(basis.qr_reciprocal_condition_inf, expected_basis.qr_reciprocal_condition_inf);
  };
  const AmericanMarkovFit copy = *fit;
  AmericanMarkovFit moved = std::move(*fit);
  AmericanMarkovFit assigned = copy;
  assigned = moved;
  check(copy);
  check(moved);
  check(assigned);
  AmericanMarkovFit target = copy;
  target = std::move(assigned);
  check(target);
  // Legacy shapes report no knot coordinates and no basis diagnostics.
  AmericanMarkovFitConfig legacy_config;
  legacy_config.max_evaluations = 1;
  const auto legacy = fit_american_markov_intervals(market, quotes, legacy_config);
  ASSERT_TRUE(legacy);
  EXPECT_EQ(legacy->time_shape(), AmericanMarkovFitTimeShape::Shared);
  EXPECT_TRUE(legacy->variance_time_knots().empty());
  EXPECT_FALSE(legacy->time_basis_diagnostics());
}

TEST(AmericanMarkovFit, TimeKnotParameterCapsAreExactAndNeverWidened) {
  const std::array ends{0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  const auto schedule = clock_schedule(ends);
  AmericanMarkovFitMarket market{100.0, schedule, {}, {}};
  market.model_config.grid_nodes = 33;
  const auto quotes = wide_quotes(ends.size());
  const std::array state_knots{-0.2, 0.0, 0.2};
  const std::array four{0.0, 0.2, 0.4, 0.6};
  auto config = knot_config(four);
  config.log_state_knots = state_knots;
  config.max_parameters = 12;
  const auto exact = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(exact) << exact.error().to_string();
  EXPECT_EQ(exact->diagnostics().parameter_count, 12U);
  EXPECT_EQ(exact->diagnostics().time_groups, 4U);
  const auto refuse = [&](const AmericanMarkovFitConfig &c, ErrorCode expected) {
    const auto result = fit_american_markov_intervals(market, quotes, c);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), expected) << result.error().to_string();
  };
  config.max_parameters = 11;
  refuse(config, ErrorCode::OutOfRange);
  config.max_parameters = kAmericanMarkovFitMaxParameters + 1;
  refuse(config, ErrorCode::InvalidArgument);
  config.max_parameters = 12;
  const std::array one{0.3};
  config.variance_time_knots = one;
  refuse(config, ErrorCode::OutOfRange);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const auto &bad : {std::array{0.0, 0.0}, std::array{0.2, 0.1}, std::array{-0.1, 0.2},
                          std::array{0.0, nan}, std::array{0.0, inf}}) {
    config.variance_time_knots = bad;
    refuse(config, ErrorCode::InvalidArgument);
  }
  for (const auto shape : {AmericanMarkovFitTimeShape::Shared,
                           AmericanMarkovFitTimeShape::PerVarianceInterval}) {
    auto legacy = config;
    legacy.time_shape = shape;
    legacy.variance_time_knots = four;
    refuse(legacy, ErrorCode::InvalidArgument);
  }
  // Interval-times-state scratch is bounded by the model's own volatility budget.
  config.variance_time_knots = four;
  market.model_config.max_stored_volatility_values = ends.size() * state_knots.size() - 1;
  refuse(config, ErrorCode::OutOfRange);
}

TEST(AmericanMarkovFit, ManyExpiryTimeKnotsKeepParameterCountBounded) {
  std::vector<double> ends(200);
  for (std::size_t i = 0; i < ends.size(); ++i) {
    ends[i] = static_cast<double>(i + 1) / 52.0;
  }
  const auto schedule = clock_schedule(ends);
  AmericanMarkovFitMarket market{100.0, schedule, {}, {}};
  market.model_config.grid_nodes = 33;
  const auto quotes = wide_quotes(ends.size());
  AmericanMarkovFitConfig per_interval;
  per_interval.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  per_interval.max_parameters = kAmericanMarkovFitMaxParameters;
  per_interval.max_evaluations = 1;
  const auto refused = fit_american_markov_intervals(market, quotes, per_interval);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
  for (const std::size_t count : {std::size_t{8}, kAmericanMarkovFitMaxParameters}) {
    std::vector<double> knots(count);
    for (std::size_t k = 0; k < count; ++k) {
      knots[k] = ends.back() * static_cast<double>(k) / static_cast<double>(count - 1);
    }
    auto config = knot_config(knots);
    config.max_parameters = kAmericanMarkovFitMaxParameters;
    const auto fit = fit_american_markov_intervals(market, quotes, config);
    ASSERT_TRUE(fit) << count << ": " << fit.error().to_string();
    EXPECT_EQ(fit->diagnostics().parameter_count, count);
    EXPECT_EQ(fit->sigmas().size(), count);
    EXPECT_EQ(fit->interval_time_weights().size(), ends.size());
    ASSERT_TRUE(fit->time_basis_diagnostics());
    EXPECT_EQ(fit->time_basis_diagnostics()->quoted_variance_intervals, ends.size());
    expect_matches_dense(*fit->time_basis_diagnostics(), fit->interval_time_weights(), count);
  }
  std::vector<double> too_many(kAmericanMarkovFitMaxParameters + 1);
  for (std::size_t k = 0; k < too_many.size(); ++k) too_many[k] = static_cast<double>(k);
  auto config = knot_config(too_many);
  config.max_parameters = kAmericanMarkovFitMaxParameters;
  const auto capped = fit_american_markov_intervals(market, quotes, config);
  ASSERT_FALSE(capped);
  EXPECT_EQ(capped.error().code(), ErrorCode::OutOfRange);
}

TEST(AmericanMarkovFit, TimeKnotPriorAndRoughnessUseGroupMajorControlLayout) {
  const std::array ends{0.125, 0.25, 0.5, 0.75};
  const auto schedule = clock_schedule(ends);
  AmericanMarkovFitMarket market{100.0, schedule, {}, {}};
  market.model_config.grid_nodes = 33;
  const auto quotes = wide_quotes(ends.size());
  const std::array knots{0.0625, 0.375, 0.625};
  const std::array state_knots{-0.1, 0.1};
  const std::array initial{0.20, 0.25, 0.30, 0.28, 0.40, 0.33};
  const std::array prior{0.22, 0.22, 0.30, 0.30, 0.35, 0.35};
  auto config = knot_config(knots);
  config.log_state_knots = state_knots;
  config.initial_sigmas = initial;
  config.prior_sigmas = prior;
  config.prior_weight = 0.7;
  config.strike_roughness_weight = 0.3;
  config.time_roughness_weight = 0.11;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  // Group-major: index = time_group * state_knots + state_knot. Strike pairs are
  // adjacent state knots within a time group; time pairs are adjacent time knots.
  double prior_sum = 0.0;
  double strike_sum = 0.0;
  double time_sum = 0.0;
  for (std::size_t t = 0; t < knots.size(); ++t) {
    for (std::size_t s = 0; s < state_knots.size(); ++s) {
      const auto i = t * state_knots.size() + s;
      const double log_sigma = std::log(initial[i]);
      const double p = log_sigma - std::log(prior[i]);
      prior_sum += p * p;
      if (s > 0) {
        const double d = log_sigma - std::log(initial[i - 1]);
        strike_sum += d * d;
      }
      if (t > 0) {
        const double d = log_sigma - std::log(initial[i - state_knots.size()]);
        time_sum += d * d;
      }
    }
  }
  const double expected = (0.7 * prior_sum + 0.3 * strike_sum + 0.11 * time_sum) / 6.0;
  EXPECT_NEAR(fit->diagnostics().regularization_loss, expected, 1.0e-13 * expected);
  // Empty prior means the initial vector, fixed for the whole search.
  config.prior_sigmas = {};
  config.strike_roughness_weight = config.time_roughness_weight = 0.0;
  const auto self_prior = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(self_prior);
  EXPECT_EQ(self_prior->diagnostics().regularization_loss, 0.0);
  // Legacy-sized vectors are refused for the knot layout.
  const std::array shared_sized{0.2, 0.2};
  config.initial_sigmas = shared_sized;
  auto refused = fit_american_markov_intervals(market, quotes, config);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  config.initial_sigmas = initial;
  config.prior_sigmas = shared_sized;
  refused = fit_american_markov_intervals(market, quotes, config);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  // A refused basis stays refused, however strong the prior.
  const std::array unsupported{0.0625, 0.7, 0.8};
  config.variance_time_knots = unsupported;
  config.prior_sigmas = prior;
  config.prior_weight = 1.0e6;
  config.time_roughness_weight = 1.0e6;
  refused = fit_american_markov_intervals(market, quotes, config);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
}

TEST(AmericanMarkovFit, VarianceTimeKnotsRecoverSyntheticNonFlatTermStructure) {
  const std::array ends{0.1, 0.25, 0.5, 1.0};
  const std::array knots{0.05, 0.75};
  const std::array truth{0.18, 0.36};
  const auto schedule = clock_schedule(ends);
  const AmericanMarkovFitMarket market{100.0, schedule, {}, {}};
  // Independent generator: the continuous curve sampled at each interval midpoint.
  std::vector<AmericanMarkovLocalVolNode> nodes;
  double start = 0.0;
  for (const double end : ends) {
    nodes.push_back({0.0, knot_curve(0.5 * (start + end), knots, truth)});
    start = end;
  }
  std::vector<AmericanMarkovInterval> intervals;
  for (std::size_t i = 0; i < ends.size(); ++i) {
    intervals.push_back({ends[i], ends[i], 0.0, 0.0, 1.0, 0.0,
                         std::span<const AmericanMarkovLocalVolNode>(nodes).subspan(i, 1)});
  }
  const auto model = compile_american_markov({100.0, intervals, {}, market.model_config});
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(*model));
  std::vector<AmericanMarkovFitQuote> quotes;
  for (std::size_t expiry = 0; expiry < ends.size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      for (const double strike : {80.0, 90.0, 100.0, 110.0, 120.0}) {
        const auto value = price_american_markov(*model, strike, side, expiry, workspace);
        ASSERT_TRUE(value);
        quotes.push_back({expiry, strike, side, std::max(0.0, *value - 0.002), *value + 0.002,
                          1.0});
      }
    }
  }
  auto config = knot_config(knots);
  config.max_evaluations = 300;
  config.max_sweeps = 70;
  config.midpoint_weight = 0.01;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->sigmas().size(), 2U);
  EXPECT_NEAR(fit->sigmas()[0], truth[0], 0.003);
  EXPECT_NEAR(fit->sigmas()[1], truth[1], 0.003);
  EXPECT_LT(fit->diagnostics().max_interval_error, 0.01);
  config.time_shape = AmericanMarkovFitTimeShape::Shared;
  config.variance_time_knots = {};
  const auto shared = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(shared) << shared.error().to_string();
  EXPECT_GT(shared->diagnostics().max_interval_error,
            3.0 * fit->diagnostics().max_interval_error);
  EXPECT_GT(shared->diagnostics().interval_loss, 10.0 * fit->diagnostics().interval_loss);
}

// Measures, and deliberately does not hide, the midpoint-freezing approximation:
// each ORIGINAL interval uses sigma at its variance midpoint, whereas a continuous
// knot curve integrates sigma(t)^2. A 32x refined schedule on the same aligned
// dyadic step grid isolates the resulting option-price bias.
TEST(AmericanMarkovFit, MidpointFrozenKnotSigmaBiasIsMeasuredAgainstContinuousCurve) {
  const std::array ends{0.0625, 0.125, 0.375, 0.5, 0.875};
  const std::array knots{0.125, 0.625};
  const std::array controls{0.15, 0.45};
  const auto schedule = clock_schedule(ends);
  AmericanMarkovFitMarket market{100.0, schedule, {}, {}};
  market.model_config.diffusion_method = AmericanMarkovDiffusionMethod::VarianceResolvent;
  market.model_config.max_calendar_step = 1.0 / 512.0;
  market.model_config.max_variance_step = 1.0 / 512.0;
  auto config = knot_config(knots);
  config.initial_sigmas = controls;
  const auto fit = fit_american_markov_intervals(market, wide_quotes(ends.size()), config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const double slope = 2.0 * std::log(controls[1] / controls[0]) / (knots[1] - knots[0]);
  double start = 0.0;
  double frozen_total = 0.0;
  double exact_total = 0.0;
  double max_interval_bias = 0.0;
  std::array<double, 5> cumulative_bias{};
  for (std::size_t i = 0; i < ends.size(); ++i) {
    const double width = ends[i] - start;
    const double midpoint = 0.5 * (start + ends[i]);
    const double sigma =
        fit->model().interval_data()[i].local_volatility[0].volatility_multiplier;
    EXPECT_NEAR(sigma, knot_curve(midpoint, knots, controls), 1.0e-14 * sigma) << i;
    const double frozen = sigma * sigma * width;
    const double exact = exact_integrated_variance(start, ends[i], knots, controls);
    const double bias = frozen / exact - 1.0;
    if (i < 2) {
      EXPECT_LT(std::abs(bias), 1.0e-15) << "flat left tail interval " << i;
    } else if (i < 4) {
      // Wholly inside one log-linear cell: frozen/exact = x/sinh(x), x = slope*width/2.
      const double x = 0.5 * slope * width;
      EXPECT_NEAR(frozen / exact, x / std::sinh(x), 1.0e-13) << i;
      EXPECT_LT(bias, -1.0e-3) << i;
    } else {
      EXPECT_GT(bias, 1.0e-3) << "midpoint in the flat tail overstates a rising segment";
    }
    frozen_total += frozen;
    exact_total += exact;
    cumulative_bias[i] = frozen_total / exact_total - 1.0;
    max_interval_bias = std::max(max_interval_bias, std::abs(bias));
    RecordProperty("interval_" + std::to_string(i) + "_relative_variance_bias", decimal(bias));
    RecordProperty("expiry_" + std::to_string(i) + "_cumulative_variance_bias",
                   decimal(cumulative_bias[i]));
    start = ends[i];
  }
  RecordProperty("max_abs_interval_relative_variance_bias", decimal(max_interval_bias));
  // Refined reference: every original interval split into 32 dyadic pieces, each
  // frozen at its own midpoint. Steps align exactly with the original model.
  constexpr std::size_t kSplit = 32;
  std::vector<double> refined_ends;
  start = 0.0;
  for (const double end : ends) {
    for (std::size_t k = 1; k <= kSplit; ++k) {
      refined_ends.push_back(k == kSplit ? end : std::lerp(start, end,
          static_cast<double>(k) / static_cast<double>(kSplit)));
    }
    start = end;
  }
  const auto refined_schedule = clock_schedule(refined_ends);
  AmericanMarkovFitMarket refined_market = market;
  refined_market.intervals = refined_schedule;
  const auto refined = fit_american_markov_intervals(
      refined_market, wide_quotes(refined_ends.size()), config);
  ASSERT_TRUE(refined) << refined.error().to_string();
  ASSERT_EQ(refined->model().layers().size(), fit->model().layers().size());
  double refined_frozen = 0.0;
  start = 0.0;
  for (std::size_t i = 0; i < refined_ends.size(); ++i) {
    const double sigma =
        refined->model().interval_data()[i].local_volatility[0].volatility_multiplier;
    refined_frozen += sigma * sigma * (refined_ends[i] - start);
    start = refined_ends[i];
  }
  // The reference's own residual freezing error is (1/32)^2 smaller.
  EXPECT_LT(std::abs(refined_frozen / exact_total - 1.0), 1.0e-4);
  AmericanMarkovWorkspace a;
  AmericanMarkovWorkspace b;
  ASSERT_TRUE(a.prepare(fit->model()));
  ASSERT_TRUE(b.prepare(refined->model()));
  double max_price_bias = 0.0;
  for (std::size_t i = 0; i < ends.size(); ++i) {
    const auto frozen = price_american_markov(fit->model(), 100.0, Side::Call, i, a);
    const auto reference =
        price_american_markov(refined->model(), 100.0, Side::Call, (i + 1) * kSplit - 1, b);
    ASSERT_TRUE(frozen);
    ASSERT_TRUE(reference);
    const double difference = *frozen - *reference;
    RecordProperty("expiry_" + std::to_string(i) + "_atm_call_price_bias", decimal(difference));
    if (std::abs(cumulative_bias[i]) > 1.0e-3) {
      // Lower represented variance gives a lower ATM call, and vice versa.
      EXPECT_EQ(difference < 0.0, cumulative_bias[i] < 0.0) << i;
      EXPECT_GT(std::abs(difference), 1.0e-3) << i;
    } else {
      EXPECT_LT(std::abs(difference), 1.0e-9) << i;
    }
    max_price_bias = std::max(max_price_bias, std::abs(difference));
  }
  RecordProperty("max_abs_atm_call_price_bias", decimal(max_price_bias));
  std::printf("[midpoint-freezing] max |interval variance bias| %.6g, final cumulative %.6g, "
              "max |ATM call price bias| %.6g\n",
              max_interval_bias, cumulative_bias.back(), max_price_bias);
}

// ---------------------------------------------------------------------------
// Search budget, Levenberg-Marquardt and work accounting. The generator below
// is an independent compile/scalar-price path; the fitter prices in batches.
// ---------------------------------------------------------------------------

AmericanMarkovFitMarket resolvent_market(std::span<const AmericanMarkovFitInterval> intervals) {
  AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  market.model_config.diffusion_method = AmericanMarkovDiffusionMethod::VarianceResolvent;
  market.model_config.grid_nodes = 129;
  return market;
}

// Group-major sigmas: one group (Shared) or one per positive-variance interval.
std::vector<AmericanMarkovFitQuote> smile_quotes(const AmericanMarkovFitMarket &market,
                                                 std::span<const double> knots,
                                                 std::span<const double> sigmas,
                                                 std::span<const double> strikes,
                                                 double half_spread) {
  std::vector<AmericanMarkovLocalVolNode> nodes;
  for (std::size_t i = 0; i < sigmas.size(); ++i) {
    nodes.push_back({knots[i % knots.size()], sigmas[i]});
  }
  const bool per_interval = sigmas.size() > knots.size();
  std::vector<AmericanMarkovInterval> intervals;
  std::size_t group = 0;
  double last_variance = 0.0;
  for (const auto &in : market.intervals) {
    const std::size_t offset = per_interval && in.variance_end > last_variance ? group++ : 0;
    intervals.push_back({in.calendar_end, in.variance_end, in.rate, in.continuous_yield, 1.0,
                         in.cash_dividend_at_end,
                         std::span<const AmericanMarkovLocalVolNode>(nodes).subspan(
                             offset * knots.size(), knots.size())});
    last_variance = in.variance_end;
  }
  auto model = compile_american_markov(
      {market.spot, intervals, market.state_grid, market.model_config});
  EXPECT_TRUE(model) << model.error().to_string();
  if (!model) return {};
  AmericanMarkovWorkspace workspace;
  EXPECT_TRUE(workspace.prepare(*model));
  std::vector<AmericanMarkovFitQuote> quotes;
  for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      for (const double strike : strikes) {
        const auto value = price_american_markov(*model, strike, side, expiry, workspace);
        EXPECT_TRUE(value);
        if (!value) return {};
        quotes.push_back({expiry, strike, side, std::max(0.0, *value - half_spread),
                          *value + half_spread, 1.0});
      }
    }
  }
  return quotes;
}

// One objective evaluation's rollback work for these quotes on a model.
std::size_t quote_work(const AmericanMarkovModel &model,
                       std::span<const AmericanMarkovFitQuote> quotes) {
  std::vector<std::size_t> prefix(model.layers().size(), 0);
  for (std::size_t i = 0; i < model.transitions().size(); ++i) {
    prefix[i + 1] = prefix[i] + model.transitions()[i].rollback_work();
  }
  std::size_t work = 0;
  for (const auto &q : quotes) work += prefix[model.expiry_layers()[q.expiry_index]];
  return work;
}

// Re-prices exactly as the fitter does: per expiry/side, ascending strikes, one batch.
std::vector<double> batch_reprice(const AmericanMarkovModel &model,
                                  std::span<const AmericanMarkovFitQuote> quotes) {
  std::vector<double> out(quotes.size(), -1.0);
  AmericanMarkovBatchWorkspace workspace;
  EXPECT_TRUE(workspace.prepare(model, 128));
  for (std::size_t expiry = 0; expiry < model.expiry_layers().size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      std::vector<std::size_t> rows;
      for (std::size_t i = 0; i < quotes.size(); ++i) {
        if (quotes[i].expiry_index == expiry && quotes[i].side == side) rows.push_back(i);
      }
      std::sort(rows.begin(), rows.end(),
                [&](std::size_t a, std::size_t b) { return quotes[a].strike < quotes[b].strike; });
      EXPECT_LE(rows.size(), 128U);
      std::vector<double> strikes;
      for (const auto i : rows) strikes.push_back(quotes[i].strike);
      std::vector<double> values(strikes.size());
      EXPECT_TRUE(price_american_markov_batch(model, strikes, side, expiry, values, workspace));
      for (std::size_t k = 0; k < rows.size(); ++k) out[rows[k]] = values[k];
    }
  }
  return out;
}

TEST(AmericanMarkovFitSearch, ScaledBudgetIsExplicitClampedAndOverflowSafe) {
  AmericanMarkovFitConfig config;
  EXPECT_EQ(american_markov_fit_evaluation_budget(config, 5), 256U);
  EXPECT_EQ(american_markov_fit_evaluation_budget(config, 128), 256U);
  config.evaluations_per_parameter = 64;
  config.max_scaled_evaluations = 1024;
  EXPECT_EQ(american_markov_fit_evaluation_budget(config, 1), 256U); // Never below the base.
  EXPECT_EQ(american_markov_fit_evaluation_budget(config, 5), 320U);
  EXPECT_EQ(american_markov_fit_evaluation_budget(config, 16), 1024U);
  EXPECT_EQ(american_markov_fit_evaluation_budget(config, 128), 1024U); // Hard cap.
  config.evaluations_per_parameter = kAmericanMarkovFitMaxEvaluations;
  EXPECT_EQ(american_markov_fit_evaluation_budget(config, static_cast<std::size_t>(-1)), 1024U);
}

TEST(AmericanMarkovFitSearch, InvalidScalingAndSearchControlsAreRefusedBeforeWork) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array quote{AmericanMarkovFitQuote{0, 100.0, Side::Call, 5.0, 5.2, 1.0}};
  const auto refused = [&](auto mutate) {
    AmericanMarkovFitConfig config;
    mutate(config);
    const auto fit = fit_american_markov_intervals(market, quote, config);
    return !fit && fit.error().code() == ErrorCode::InvalidArgument;
  };
  EXPECT_TRUE(refused([](auto &c) { c.evaluations_per_parameter = 8; })); // No hard cap.
  EXPECT_TRUE(refused([](auto &c) { c.max_scaled_evaluations = 512; }));  // Cap without scaling.
  EXPECT_TRUE(refused([](auto &c) {
    c.evaluations_per_parameter = 8;
    c.max_scaled_evaluations = 128; // Below max_evaluations: would silently shrink.
  }));
  EXPECT_TRUE(refused([](auto &c) {
    c.evaluations_per_parameter = 8;
    c.max_scaled_evaluations = kAmericanMarkovFitMaxEvaluations + 1;
  }));
  EXPECT_TRUE(refused([](auto &c) {
    c.evaluations_per_parameter = kAmericanMarkovFitMaxEvaluations + 1;
    c.max_scaled_evaluations = 512;
  }));
  EXPECT_TRUE(refused([](auto &c) { c.search = static_cast<AmericanMarkovFitSearch>(7); }));
  EXPECT_TRUE(refused([](auto &c) { c.objective_tolerance = -1.0; }));
  EXPECT_TRUE(refused([](auto &c) { c.objective_tolerance = 2.0; }));
  EXPECT_TRUE(refused([](auto &c) {
    c.gradient_tolerance = std::numeric_limits<double>::quiet_NaN();
  }));
  EXPECT_TRUE(refused([](auto &c) { c.jacobian_log_step = 0.0; }));
  EXPECT_TRUE(refused([](auto &c) { c.jacobian_log_step = 0.6; }));
  EXPECT_TRUE(refused([](auto &c) { c.initial_damping = 0.0; }));
  EXPECT_TRUE(refused([](auto &c) { c.max_damping = 1.0e-4; })); // Below initial_damping.
  EXPECT_TRUE(refused([](auto &c) { c.max_damping = 1.0e17; }));
  AmericanMarkovFitConfig valid;
  valid.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  valid.evaluations_per_parameter = 8;
  valid.max_scaled_evaluations = 256; // Equal to max_evaluations is allowed.
  EXPECT_TRUE(fit_american_markov_intervals(market, quote, valid));
}

TEST(AmericanMarkovFitSearch, ScaledBudgetLetsFiveKnotSmileConvergeWhereDefaultHitsLimit) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.75, 0.75, 0.0, 0.0, 0.0}};
  const auto market = resolvent_market(intervals);
  const std::array knots{-0.6, -0.3, 0.0, 0.3, 0.6};
  const std::array truth{0.55, 0.40, 0.30, 0.26, 0.25};
  const std::array strikes{55.0, 65.0, 75.0, 85.0, 95.0, 100.0, 105.0, 115.0, 130.0, 150.0,
                           175.0};
  const auto quotes = smile_quotes(market, knots, truth, strikes, 0.01);
  ASSERT_FALSE(quotes.empty());
  AmericanMarkovFitConfig config;
  config.log_state_knots = knots;
  const auto unscaled = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(unscaled) << unscaled.error().to_string();
  // The historical budget binds, exactly as on the development cohort.
  EXPECT_EQ(unscaled->diagnostics().termination, AmericanMarkovFitTermination::EvaluationLimit);
  EXPECT_EQ(unscaled->diagnostics().evaluations, 256U);
  EXPECT_EQ(unscaled->diagnostics().evaluation_budget, 256U);

  auto coordinate = config;
  coordinate.evaluations_per_parameter = 400;
  coordinate.max_scaled_evaluations = 2048;
  coordinate.max_sweeps = 400;
  const auto scaled = fit_american_markov_intervals(market, quotes, coordinate);
  ASSERT_TRUE(scaled) << scaled.error().to_string();
  EXPECT_EQ(scaled->diagnostics().evaluation_budget, 2000U);
  EXPECT_EQ(scaled->diagnostics().termination, AmericanMarkovFitTermination::StepTolerance);
  EXPECT_LT(scaled->diagnostics().evaluations, 2000U);
  EXPECT_LT(scaled->diagnostics().objective, unscaled->diagnostics().objective);

  auto lm = config;
  lm.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  lm.evaluations_per_parameter = 64;
  lm.max_scaled_evaluations = 1024;
  lm.max_sweeps = 64;
  lm.objective_tolerance = 1.0e-10;
  const auto damped = fit_american_markov_intervals(market, quotes, lm);
  ASSERT_TRUE(damped) << damped.error().to_string();
  const auto &d = damped->diagnostics();
  EXPECT_EQ(d.search, AmericanMarkovFitSearch::LevenbergMarquardt);
  EXPECT_EQ(d.evaluation_budget, 320U);
  EXPECT_NE(d.termination, AmericanMarkovFitTermination::EvaluationLimit);
  EXPECT_NE(d.termination, AmericanMarkovFitTermination::SweepLimit);
  EXPECT_LT(d.evaluations, scaled->diagnostics().evaluations);
  EXPECT_EQ(d.jacobian_evaluations % 5U, 0U);
  EXPECT_GT(d.jacobian_evaluations, 0U);
  EXPECT_LE(d.objective, scaled->diagnostics().objective * 1.05);
  EXPECT_LT(d.objective, unscaled->diagnostics().objective);
  EXPECT_LT(d.max_interval_error, 0.02);
  for (std::size_t i = 1; i + 1 < truth.size(); ++i) {
    EXPECT_NEAR(damped->sigmas()[i], truth[i], 0.02 * truth[i]) << i;
  }
  RecordProperty("coordinate_default_objective", decimal(unscaled->diagnostics().objective));
  RecordProperty("coordinate_scaled_evaluations",
                 std::to_string(scaled->diagnostics().evaluations));
  RecordProperty("lm_evaluations", std::to_string(d.evaluations));
  std::printf("[five-knot] default %zu evals obj %.3g | coordinate scaled %zu evals obj %.3g | "
              "LM %zu evals (%zu Jacobian, %zu rejected) obj %.3g\n",
              unscaled->diagnostics().evaluations, unscaled->diagnostics().objective,
              scaled->diagnostics().evaluations, scaled->diagnostics().objective, d.evaluations,
              d.jacobian_evaluations, d.rejected_steps, d.objective);
}

TEST(AmericanMarkovFitSearch, PerIntervalShapeConvergesAndReusesExactPrefixPrices) {
  const std::array intervals{AmericanMarkovFitInterval{0.1, 0.1, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.3, 0.3, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.6, 0.6, 0.0, 0.0, 0.0}};
  const auto market = resolvent_market(intervals);
  const std::array knots{-0.25, 0.0, 0.25};
  const std::array truth{0.36, 0.30, 0.27, 0.33, 0.28, 0.25, 0.31, 0.26, 0.24};
  const std::array strikes{75.0, 85.0, 92.0, 100.0, 108.0, 118.0, 130.0};
  const auto quotes = smile_quotes(market, knots, truth, strikes, 0.005);
  ASSERT_FALSE(quotes.empty());
  AmericanMarkovFitConfig config;
  config.log_state_knots = knots;
  config.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  const auto unscaled = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(unscaled) << unscaled.error().to_string();
  EXPECT_EQ(unscaled->diagnostics().termination, AmericanMarkovFitTermination::EvaluationLimit);
  EXPECT_EQ(unscaled->diagnostics().reused_quote_prices, 0U);

  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  config.evaluations_per_parameter = 48;
  config.max_scaled_evaluations = 1024;
  config.max_sweeps = 64;
  config.objective_tolerance = 1.0e-10;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &d = fit->diagnostics();
  EXPECT_EQ(d.evaluation_budget, 432U);
  EXPECT_NE(d.termination, AmericanMarkovFitTermination::EvaluationLimit);
  EXPECT_NE(d.termination, AmericanMarkovFitTermination::SweepLimit);
  EXPECT_LT(d.objective, unscaled->diagnostics().objective);
  EXPECT_LT(d.max_interval_error, 0.01);
  for (std::size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(fit->sigmas()[i], truth[i], 0.03 * truth[i]) << i;
  }
  // Every evaluation accounts for every quote exactly once: priced or reused.
  EXPECT_EQ(d.failed_evaluations, 0U);
  EXPECT_EQ(d.priced_quotes + d.reused_quote_prices, d.evaluations * quotes.size());
  EXPECT_GT(d.reused_quote_prices, 0U);
  // Reused prefixes are not charged: total work is strictly below full repricing.
  const auto full = quote_work(fit->model(), quotes);
  EXPECT_LT(d.rollback_work, d.evaluations * full);
  EXPECT_GE(d.rollback_work, (d.evaluations - d.jacobian_evaluations) * full);
  // Stored marks equal a fresh fitter-identical batch repricing, bit for bit.
  const auto repriced = batch_reprice(fit->model(), quotes);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    EXPECT_EQ(bits_of(repriced[i]), bits_of(fit->quote_prices()[i])) << i;
  }
  std::printf("[per-interval] default %zu evals obj %.3g | LM %zu evals obj %.3g, "
              "priced %zu reused %zu, work %zu vs full %zu\n",
              unscaled->diagnostics().evaluations, unscaled->diagnostics().objective,
              d.evaluations, d.objective, d.priced_quotes, d.reused_quote_prices,
              d.rollback_work, d.evaluations * full);
}

// The reuse rule's premise, through the public compiler: changing only a later
// interval's local volatility leaves every earlier expiry's batch prices
// bit-identical, including with dated cash between them.
TEST(AmericanMarkovFitSearch, LaterIntervalSigmaNeverChangesEarlierExpiryPriceBits) {
  const std::array schedule{AmericanMarkovFitInterval{0.1, 0.1, 0.01, 0.0, 0.0},
                            AmericanMarkovFitInterval{0.2, 0.2, 0.01, 0.0, 1.0},
                            AmericanMarkovFitInterval{0.5, 0.45, 0.01, 0.0, 0.0}};
  for (const auto method : {AmericanMarkovDiffusionMethod::ExplicitEuler,
                            AmericanMarkovDiffusionMethod::VarianceResolvent}) {
    std::array<std::vector<double>, 2> prices;
    for (std::size_t variant = 0; variant < 2; ++variant) {
      const std::array nodes{AmericanMarkovLocalVolNode{-0.2, 0.30},
                             AmericanMarkovLocalVolNode{0.2, 0.25},
                             AmericanMarkovLocalVolNode{-0.2, variant == 0 ? 0.40 : 0.55},
                             AmericanMarkovLocalVolNode{0.2, 0.35}};
      std::vector<AmericanMarkovInterval> intervals;
      for (std::size_t i = 0; i < schedule.size(); ++i) {
        const auto &s = schedule[i];
        intervals.push_back({s.calendar_end, s.variance_end, s.rate, s.continuous_yield, 1.0,
                             s.cash_dividend_at_end,
                             std::span<const AmericanMarkovLocalVolNode>(nodes).subspan(
                                 i == 2 ? 2 : 0, 2)});
      }
      AmericanMarkovConfig model_config;
      model_config.diffusion_method = method;
      const auto model = compile_american_markov({100.0, intervals, {}, model_config});
      ASSERT_TRUE(model) << model.error().to_string();
      std::vector<AmericanMarkovFitQuote> quotes;
      for (std::size_t expiry = 0; expiry < 2; ++expiry) {
        for (const double strike : {80.0, 95.0, 100.0, 110.0, 130.0}) {
          quotes.push_back({expiry, strike, Side::Put, 0.0, 1.0, 1.0});
          quotes.push_back({expiry, strike, Side::Call, 0.0, 1.0, 1.0});
        }
      }
      prices[variant] = batch_reprice(*model, quotes);
    }
    ASSERT_EQ(prices[0].size(), prices[1].size());
    for (std::size_t i = 0; i < prices[0].size(); ++i) {
      EXPECT_EQ(bits_of(prices[0][i]), bits_of(prices[1][i])) << i;
    }
  }
}

TEST(AmericanMarkovFitSearch, CoordinateWorkLedgerChargesEveryQuoteOfEveryEvaluation) {
  const std::array intervals{AmericanMarkovFitInterval{0.2, 0.2, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.5, 0.5, 0.0, 0.0, 0.0}};
  const auto market = resolvent_market(intervals);
  const std::array knots{0.0};
  const std::array truth{0.3};
  const std::array strikes{90.0, 100.0, 110.0};
  const auto quotes = smile_quotes(market, knots, truth, strikes, 0.01);
  AmericanMarkovFitConfig config;
  config.max_evaluations = 40;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &d = fit->diagnostics();
  EXPECT_EQ(d.search, AmericanMarkovFitSearch::CoordinatePattern);
  EXPECT_EQ(d.failed_evaluations, 0U);
  EXPECT_EQ(d.jacobian_evaluations, 0U);
  EXPECT_EQ(d.reused_quote_prices, 0U);
  EXPECT_EQ(d.priced_quotes, d.evaluations * quotes.size());
  // Resolvent clocks do not depend on sigma, so every evaluation costs the same.
  EXPECT_EQ(d.rollback_work, d.evaluations * quote_work(fit->model(), quotes));
}

TEST(AmericanMarkovFitSearch, LevenbergMarquardtTerminationsAreExplicitAndDeterministic) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const auto market = resolvent_market(intervals);
  AmericanMarkovFitConfig config;
  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  // All quotes inside, no regularization: exactly zero gradient after one Jacobian.
  const std::array inside{AmericanMarkovFitQuote{0, 100.0, Side::Call, 0.0, 30.0, 1.0}};
  const auto flat = fit_american_markov_intervals(market, inside, config);
  ASSERT_TRUE(flat) << flat.error().to_string();
  EXPECT_EQ(flat->diagnostics().termination, AmericanMarkovFitTermination::GradientTolerance);
  EXPECT_EQ(flat->diagnostics().evaluations, 2U);
  EXPECT_EQ(flat->diagnostics().gradient_norm, 0.0);
  EXPECT_EQ(flat->sigmas()[0], config.initial_sigma);

  const std::array target{0.31};
  const std::array knots{0.0};
  const std::array strikes{90.0, 100.0, 110.0};
  const auto quotes = smile_quotes(market, knots, target, strikes, 0.002);
  // A budget below one iteration (n Jacobian columns + one trial) never starts one.
  config.max_evaluations = 1;
  const auto starved = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(starved);
  EXPECT_EQ(starved->diagnostics().termination, AmericanMarkovFitTermination::EvaluationLimit);
  EXPECT_EQ(starved->diagnostics().evaluations, 1U);
  EXPECT_EQ(starved->diagnostics().objective, starved->diagnostics().initial_objective);

  config.max_evaluations = 256;
  config.objective_tolerance = 1.0; // Any accepted step satisfies it.
  const auto once = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(once);
  EXPECT_EQ(once->diagnostics().termination, AmericanMarkovFitTermination::ObjectiveTolerance);
  EXPECT_EQ(once->diagnostics().accepted_steps, 1U);
  EXPECT_EQ(once->diagnostics().completed_sweeps, 1U);
  EXPECT_LT(once->diagnostics().objective, once->diagnostics().initial_objective);

  config.objective_tolerance = 0.0;
  config.max_sweeps = 2;
  const auto swept = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(swept);
  EXPECT_EQ(swept->diagnostics().completed_sweeps, 2U);
  EXPECT_EQ(swept->diagnostics().termination, AmericanMarkovFitTermination::SweepLimit);

  config.max_sweeps = 32;
  const auto a = fit_american_markov_intervals(market, quotes, config);
  const auto b = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(bits_of(a->sigmas()[0]), bits_of(b->sigmas()[0]));
  EXPECT_EQ(a->diagnostics().evaluations, b->diagnostics().evaluations);
  EXPECT_EQ(a->diagnostics().termination, b->diagnostics().termination);
  EXPECT_NEAR(a->sigmas()[0], target[0], 0.001);
  EXPECT_LE(a->diagnostics().best_evaluation, a->diagnostics().evaluations);
  EXPECT_LE(a->diagnostics().evaluations, a->diagnostics().evaluation_budget);
  EXPECT_GT(a->diagnostics().final_damping, 0.0);
}

// ---------------------------------------------------------------------------
// Levenberg-Marquardt refusal paths. ExplicitEuler on its fixed default grid
// subdivides each interval by a CFL count proportional to sigma^2, so a
// max_rollback_work cap equal to one flat-sigma evaluation refuses exactly the
// probes and trials whose sigma is larger. Each test asserts that premise.
// ---------------------------------------------------------------------------

// One objective evaluation's rollback work at a flat sigma (ExplicitEuler).
std::size_t flat_work(const AmericanMarkovFitMarket &market,
                      std::span<const AmericanMarkovFitQuote> quotes, double sigma) {
  AmericanMarkovFitConfig config;
  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  config.initial_sigma = sigma;
  config.max_evaluations = 1; // Below one LM iteration: the initial model only.
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  EXPECT_TRUE(fit) << fit.error().to_string();
  return fit ? quote_work(fit->model(), quotes) : 0U;
}

TEST(AmericanMarkovFitSearch, RefusedJacobianProbeIsRetriedInTheOtherDirection) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array knots{0.0};
  const std::array target{0.2};
  const std::array strikes{90.0, 100.0, 110.0};
  const auto quotes = smile_quotes(market, knots, target, strikes, 0.002);
  ASSERT_FALSE(quotes.empty());
  const auto cap = flat_work(market, quotes, 0.25);
  ASSERT_GT(flat_work(market, quotes, 0.25 * std::exp(0.5)), cap);
  AmericanMarkovFitConfig config;
  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  config.jacobian_log_step = 0.5; // Forward probe 0.41 is refused; backward 0.15 is not.
  config.max_rollback_work = cap;
  config.max_sweeps = 4;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &d = fit->diagnostics();
  EXPECT_NE(d.termination, AmericanMarkovFitTermination::JacobianFailure);
  EXPECT_GE(d.failed_evaluations, 1U);
  // The first column needed the fallback: a refused probe, then an admitted one.
  EXPECT_GE(d.jacobian_evaluations, 2U);
  EXPECT_GT(d.gradient_norm, 0.0);
  EXPECT_GE(d.accepted_steps, 1U);
  EXPECT_LT(d.objective, d.initial_objective);
  EXPECT_LT(fit->sigmas()[0], 0.25);
}

TEST(AmericanMarkovFitSearch, ColumnWithoutAnAdmittedProbeStopsWithJacobianFailure) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array knots{0.0};
  const std::array target{0.31};
  const std::array strikes{90.0, 100.0, 110.0};
  const auto quotes = smile_quotes(market, knots, target, strikes, 0.002);
  ASSERT_FALSE(quotes.empty());
  const auto cap = flat_work(market, quotes, 0.2);
  ASSERT_GT(flat_work(market, quotes, 0.2 * std::exp(0.5)), cap);
  AmericanMarkovFitConfig config;
  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  // At min_sigma the backward probe cannot move, so the refused forward probe
  // has no fallback. Before the fix the zero column passed the gradient test.
  config.min_sigma = 0.2;
  config.initial_sigma = 0.2;
  config.jacobian_log_step = 0.5;
  config.max_rollback_work = cap;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &d = fit->diagnostics();
  EXPECT_EQ(d.termination, AmericanMarkovFitTermination::JacobianFailure);
  EXPECT_EQ(d.evaluations, 2U);
  EXPECT_EQ(d.jacobian_evaluations, 1U);
  EXPECT_EQ(d.failed_evaluations, 1U);
  EXPECT_EQ(d.gradient_norm, 0.0); // No gradient test on a missing column.
  EXPECT_EQ(d.accepted_steps, 0U);
  EXPECT_EQ(d.completed_sweeps, 0U);
  EXPECT_EQ(fit->sigmas()[0], 0.2);
  EXPECT_EQ(d.objective, d.initial_objective);
}

TEST(AmericanMarkovFitSearch, ForwardRoomShorterThanTheStepProbesBackward) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const auto market = resolvent_market(intervals);
  const std::array knots{0.0};
  const std::array target{0.25};
  const std::array strikes{90.0, 100.0, 110.0};
  const auto quotes = smile_quotes(market, knots, target, strikes, 0.002);
  ASSERT_FALSE(quotes.empty());
  AmericanMarkovFitConfig config;
  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  config.max_sigma = 0.3;
  config.initial_sigma = 0.2999; // Forward room log(0.3/0.2999) is far below the step.
  config.jacobian_log_step = 0.01;
  config.gradient_tolerance = 1.0e6; // Stop after the first Jacobian.
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &d = fit->diagnostics();
  EXPECT_EQ(d.termination, AmericanMarkovFitTermination::GradientTolerance);
  EXPECT_EQ(d.jacobian_evaluations, 1U);
  // The full backward probe moved toward the quotes and became the best model;
  // a forward probe would have been truncated to max_sigma (away from them).
  EXPECT_EQ(d.best_evaluation, 2U);
  EXPECT_EQ(bits_of(fit->sigmas()[0]), bits_of(std::exp(std::log(0.2999) - 0.01)));
}

// DampingLimit: with max_damping == initial_damping, the first rejected trial
// (refused here by the work cap) cannot grow the damping and ends the search.
TEST(AmericanMarkovFitSearch, FirstRejectedTrialAtMaxDampingStopsWithDampingLimit) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0.0, 0.0, 0.0}};
  const AmericanMarkovFitMarket market{100.0, intervals, {}, {}};
  const std::array knots{0.0};
  const std::array target{0.31};
  const std::array strikes{90.0, 100.0, 110.0};
  const auto quotes = smile_quotes(market, knots, target, strikes, 0.002);
  ASSERT_FALSE(quotes.empty());
  const auto cap = flat_work(market, quotes, 0.25);
  ASSERT_GT(flat_work(market, quotes, 0.28), cap);
  AmericanMarkovFitConfig config;
  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  config.max_rollback_work = cap;
  config.max_damping = config.initial_damping;
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &d = fit->diagnostics();
  EXPECT_EQ(d.termination, AmericanMarkovFitTermination::DampingLimit);
  EXPECT_EQ(d.rejected_steps, 1U);
  EXPECT_EQ(d.accepted_steps, 0U);
  EXPECT_EQ(d.completed_sweeps, 0U);
  EXPECT_GE(d.failed_evaluations, 1U); // The trial toward 0.31 exceeded the work cap.
  EXPECT_EQ(d.evaluations, 1U + d.jacobian_evaluations + 1U);
  EXPECT_EQ(d.final_damping, 2.0 * config.initial_damping);
}

// The Cholesky refusal is unreachable from finite public inputs (a finite J'J
// plus the Marquardt diagonal is positive definite), so the solver's contract
// is driven directly through the private header.
TEST(AmericanMarkovFitSearch, DampedStepSolvesPositiveSystemsAndRefusesTheRest) {
  std::vector<double> system(4);
  std::vector<double> step(2);
  const std::array normal{4.0, 0.0, 0.0, 9.0};
  const std::array gradient{2.0, -3.0};
  ASSERT_TRUE(detail::markov_lm_damped_step(normal, gradient, 1.0, system, step));
  EXPECT_DOUBLE_EQ(step[0], -2.0 / 8.0); // (A + 1 * diag(A)) x = -g.
  EXPECT_DOUBLE_EQ(step[1], 3.0 / 18.0);
  const std::array indefinite{1.0, 2.0, 2.0, 1.0};
  EXPECT_FALSE(detail::markov_lm_damped_step(indefinite, gradient, 1.0e-3, system, step));
  const std::array nonfinite{std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0};
  EXPECT_FALSE(detail::markov_lm_damped_step(nonfinite, gradient, 1.0e-3, system, step));
  const std::array nan_gradient{std::numeric_limits<double>::quiet_NaN(), 0.0};
  EXPECT_FALSE(detail::markov_lm_damped_step(normal, nan_gradient, 1.0, system, step));
}

// VarianceTimeKnots is the shape whose reuse rule is non-trivial: interpolation
// groups plus paused rows. Interval 1 is paused but maps onto knot 1, so knot
// 1's probe conservatively reprices expiries 1..2 and reuses expiry 0 only.
// The best model is that reused probe; its stored prices must equal a full
// fitter-identical repricing bit for bit.
TEST(AmericanMarkovFitSearch, TimeKnotProbeReusesOnlyTheUnaffectedPrefixBitForBit) {
  const std::array intervals{AmericanMarkovFitInterval{0.1, 0.1, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.2, 0.1, 0.0, 0.0, 0.0},
                             AmericanMarkovFitInterval{0.5, 0.5, 0.0, 0.0, 0.0}};
  const auto market = resolvent_market(intervals);
  const std::array time_knots{0.05, 0.4};
  std::vector<AmericanMarkovFitQuote> quotes;
  for (std::size_t expiry = 0; expiry < intervals.size(); ++expiry) {
    for (const auto side : {Side::Call, Side::Put}) {
      for (const double strike : {85.0, 95.0, 100.0, 105.0, 115.0}) {
        quotes.push_back({expiry, strike, side, 0.0, 1000.0, 1.0});
      }
    }
  }
  const auto price_at = [&](std::span<const double> controls) {
    auto config = knot_config(time_knots);
    config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
    config.initial_sigmas = controls;
    return fit_american_markov_intervals(market, quotes, config);
  };
  const std::array truth{0.25, 0.35};
  const auto generator = price_at(truth);
  ASSERT_TRUE(generator) << generator.error().to_string();
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const double price = generator->quote_prices()[i];
    quotes[i].bid = std::max(0.0, price - 0.0005);
    quotes[i].ask = price + 0.0005;
  }
  const double step = 0.02;
  const std::array initial{truth[0], truth[1] * std::exp(-step)};
  const auto start = price_at(initial);
  ASSERT_TRUE(start) << start.error().to_string();

  auto config = knot_config(time_knots);
  config.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  config.initial_sigmas = initial;
  config.max_evaluations = 64;
  config.jacobian_log_step = step;   // Knot 1's forward probe lands on the truth.
  config.gradient_tolerance = 1.0e6; // Stop after the first Jacobian.
  const auto fit = fit_american_markov_intervals(market, quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &d = fit->diagnostics();
  ASSERT_EQ(d.termination, AmericanMarkovFitTermination::GradientTolerance);
  ASSERT_EQ(d.jacobian_evaluations, 2U);
  ASSERT_EQ(d.best_evaluation, 3U) << "the knot-1 probe must be the stored model";
  const std::size_t prefix = 10; // Expiry 0: five strikes, two sides.
  EXPECT_EQ(d.reused_quote_prices, prefix);
  EXPECT_EQ(d.priced_quotes + d.reused_quote_prices, d.evaluations * quotes.size());
  EXPECT_LT(d.objective, d.initial_objective);
  const auto repriced = batch_reprice(fit->model(), quotes);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    EXPECT_EQ(bits_of(repriced[i]), bits_of(fit->quote_prices()[i])) << i;
    if (quotes[i].expiry_index == 0) {
      // The reused prefix is the starting model's own price, bit for bit.
      EXPECT_EQ(bits_of(fit->quote_prices()[i]), bits_of(start->quote_prices()[i])) << i;
    }
  }
}

} // namespace
} // namespace atx::vol
