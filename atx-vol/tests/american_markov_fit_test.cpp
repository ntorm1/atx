#include "atx/vol/api/fitting/american_markov_fit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

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
  ASSERT_EQ(fit->interval_groups().size(), 3U);
  EXPECT_EQ(fit->interval_groups()[0], 0U);
  EXPECT_EQ(fit->interval_groups()[1], 0U);
  EXPECT_EQ(fit->interval_groups()[2], 1U);
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

} // namespace
} // namespace atx::vol
