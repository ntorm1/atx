#include "atx/vol/api/fitting/american_markov_seed.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {
using QuoteStatus = AmericanMarkovSeedQuoteStatus;
using ParameterStatus = AmericanMarkovSeedParameterStatus;

double call(double spot, double strike, double variance, double rate = 0, double yield = 0) {
  const auto cdf = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
  const double width = std::sqrt(variance);
  const double d1 = (std::log(spot / strike) + rate - yield) / width + width * 0.5;
  return spot * std::exp(-yield) * cdf(d1) - strike * std::exp(-rate) * cdf(d1 - width);
}

AmericanMarkovFitQuote quote(std::size_t expiry, double years, double sigma,
                             double strike = 100, double spread = 0.001,
                             double rate = 0, double yield = 0) {
  const double price = call(100, strike, sigma * sigma * years, rate, yield);
  return {expiry, strike, Side::Call, price - spread, price + spread, 1};
}

TEST(AmericanMarkovSeeds, IndependentFlatControlOwnsSeedsAndDoesNotApplyThem) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  std::vector<double> knots{-0.2, 0, 0.2};
  const AmericanMarkovSeedLayout layout{AmericanMarkovFitTimeShape::Shared, knots};
  const std::array quotes{quote(0, 0.5, 0.3, 95), quote(0, 0.5, 0.3),
                           quote(0, 0.5, 0.3, 105)};
  AmericanMarkovFitConfig untouched;
  untouched.prior_weight = 2;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->initial_sigmas.size(), 3U);
  for (const auto sigma : result->initial_sigmas) EXPECT_NEAR(sigma, 0.3, 2e-6);
  EXPECT_EQ(result->used_quotes, 3U);
  EXPECT_EQ(result->inversion_calls, 9U);
  EXPECT_EQ(result->reserved_root_iteration_slots, 9U * 32U);
  EXPECT_GT(result->expiries[0].total_variance->upper,
            result->expiries[0].total_variance->lower);
  knots[0] = -1;
  EXPECT_EQ(result->log_state_knots[0], -0.2);
  EXPECT_EQ(untouched.initial_sigma, 0.25);
  EXPECT_TRUE(untouched.initial_sigmas.empty());
  EXPECT_TRUE(untouched.prior_sigmas.empty());
  EXPECT_EQ(untouched.prior_weight, 2);
  EXPECT_EQ(quotes[0].bid, quote(0, 0.5, 0.3, 95).bid);
}

TEST(AmericanMarkovSeeds, IndependentTermVarianceRecoversPositiveForwardSeeds) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0, 0, 0},
                              AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  const double terminal = std::sqrt(0.04 * 0.25 + 0.16 * 0.75);
  const std::array quotes{quote(0, 0.25, 0.2), quote(1, 1, terminal)};
  AmericanMarkovSeedLayout layout;
  layout.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->initial_sigmas.size(), 2U);
  EXPECT_NEAR(result->initial_sigmas[0], 0.2, 2e-6);
  EXPECT_NEAR(result->initial_sigmas[1], 0.4, 2e-6);
  EXPECT_GT(result->parameters[1].variance_rate->lower, 0);
}

TEST(AmericanMarkovSeeds, DualClocksAndFundingPvContinuousYieldUseCumulativeCarry) {
  const double calendar = 0.5;
  const double variance = 0.25;
  const double cumulative_yield = -std::log(std::exp(0.05) - 2.0 / 100.0);
  const std::array intervals{AmericanMarkovFitInterval{
      calendar, variance, 0.02, cumulative_yield / calendar, 0}};
  const std::array quotes{quote(0, variance, 0.35, 100, 0.001, 0.01, cumulative_yield)};
  const auto result = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->initial_sigmas.size(), 1U);
  EXPECT_NEAR(result->initial_sigmas[0], 0.35, 2e-6);
  EXPECT_DOUBLE_EQ(result->expiries[0].integrated_rate, 0.01);
  EXPECT_DOUBLE_EQ(result->expiries[0].integrated_yield, cumulative_yield);
}

TEST(AmericanMarkovSeeds, SharedWeightsDistinctVarianceEndpointsEqually) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0, 0, 0},
      AmericanMarkovFitInterval{0.5, 0.25, 0, 0, 0},
      AmericanMarkovFitInterval{0.75, 0.25, 0, 0, 0},
      AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  const std::array quotes{quote(0, 0.25, 0.2), quote(1, 0.25, 0.2),
                           quote(2, 0.25, 0.2), quote(3, 1, 0.4)};
  const auto result = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->variance_endpoints.size(), 2U);
  ASSERT_EQ(result->initial_sigmas.size(), 1U);
  EXPECT_NEAR(result->initial_sigmas[0], std::sqrt((0.04 + 0.16) * 0.5), 2e-6);
  EXPECT_EQ(result->variance_endpoints[0].supported_expiries, 3U);
  EXPECT_EQ(result->variance_endpoints[0].expiry_indices.size(), 3U);
}

TEST(AmericanMarkovSeeds, MissingEndpointsNeverInterpolateOrFillACompleteVector) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0, 0, 0},
                              AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  const std::array quotes{quote(1, 1, 0.3)};
  AmericanMarkovSeedLayout layout;
  layout.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->initial_sigmas.empty());
  ASSERT_EQ(result->parameters.size(), 2U);
  for (const auto &p : result->parameters) EXPECT_EQ(p.status, ParameterStatus::MissingEndpoint);
  const auto empty = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, {});
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->initial_sigmas.empty());
}

TEST(AmericanMarkovSeeds, NegativeAndUncertainForwardVarianceRemainUnseeded) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0},
                              AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  AmericanMarkovSeedLayout layout;
  layout.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  const std::array decreasing{quote(0, 0.5, 0.6), quote(1, 1, 0.2)};
  const auto negative = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, decreasing, layout);
  ASSERT_TRUE(negative);
  EXPECT_TRUE(negative->initial_sigmas.empty());
  EXPECT_EQ(negative->parameters[1].status, ParameterStatus::NegativeForwardVariance);
  EXPECT_LT(negative->parameters[1].variance_rate->midpoint, 0);
  const std::array overlapping{quote(0, 0.5, 0.3, 100, 0.1),
                               quote(1, 1, std::sqrt(0.0451), 100, 0.1)};
  const auto uncertain = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, overlapping, layout);
  ASSERT_TRUE(uncertain);
  EXPECT_TRUE(uncertain->initial_sigmas.empty());
  EXPECT_EQ(uncertain->parameters[1].status, ParameterStatus::UncertainForwardVariance);
  EXPECT_LT(uncertain->parameters[1].variance_rate->lower, 0);
  EXPECT_GT(uncertain->parameters[1].variance_rate->midpoint, 0);
}

TEST(AmericanMarkovSeeds, DatedCashRefusedWithoutSpendingInversionWork) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 1}};
  const std::array quotes{quote(0, 0.5, 0.3)};
  const auto result = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->initial_sigmas.empty());
  EXPECT_EQ(result->inversion_calls, 0U);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::UnsupportedCashSchedule);
  EXPECT_EQ(result->parameters[0].status, ParameterStatus::UnsupportedCashSchedule);
  EXPECT_EQ(result->refused_quotes, quotes.size());
}

TEST(AmericanMarkovSeeds, SelectionReasonsDoNotSilentlyEraseOriginalQuotes) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  std::array quotes{quote(0, 0.5, 0.3), quote(0, 0.5, 0.3, 101),
      quote(0, 0.5, 0.3, 102), quote(0, 0.5, 0.3, 103), quote(0, 0.5, 0.3, 150),
      quote(0, 0.5, 0.3, 95), quote(1, 0.5, 0.3, 100)};
  quotes[1].bid = 0;
  quotes[2].ask = 3 * quotes[2].bid;
  quotes[3].bid = -1;
  quotes[5].bid = 5;
  quotes[5].ask = 5.001;
  const auto result = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->quotes.size(), quotes.size());
  const std::array expected{QuoteStatus::Used, QuoteStatus::Bidless, QuoteStatus::WideSpread,
      QuoteStatus::InvalidQuote, QuoteStatus::FarMoneyness, QuoteStatus::IntrinsicPlateau,
      QuoteStatus::InvalidQuote};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(result->quotes[i].status, expected[i]);
  }
  EXPECT_EQ(result->used_quotes, 1U);
  EXPECT_EQ(result->refused_quotes, 6U);
  EXPECT_EQ(quotes[3].bid, -1);
}

TEST(AmericanMarkovSeeds, IvFloorAndRequestedSigmaBoundsNeverClampSeeds) {
  const std::array intervals{AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  AmericanMarkovSeedLayout layout;
  layout.min_sigma = 1e-6;
  const std::array low{quote(0, 1, 0.001, 100, 1e-6)};
  const auto floor = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, low, layout);
  ASSERT_TRUE(floor);
  EXPECT_TRUE(floor->initial_sigmas.empty());
  EXPECT_EQ(floor->quotes[0].status, QuoteStatus::IvFloor);
  EXPECT_EQ(floor->quotes[0].equivalent_vols[0], kIvMin);
  const std::array high{quote(0, 1, 0.8)};
  layout.max_sigma = 0.5;
  const auto bound = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, high, layout);
  ASSERT_TRUE(bound);
  EXPECT_TRUE(bound->initial_sigmas.empty());
  EXPECT_EQ(bound->quotes[0].status, QuoteStatus::SigmaBounds);
  EXPECT_GT(*bound->quotes[0].equivalent_vols[0], layout.max_sigma);
}

TEST(AmericanMarkovSeeds, RoundRobinBudgetsAndPermutationAreDeterministic) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0},
                              AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  std::vector quotes{quote(0, 0.5, 0.3, 95), quote(0, 0.5, 0.3), quote(0, 0.5, 0.3, 105),
                     quote(1, 1, 0.4, 95), quote(1, 1, 0.4), quote(1, 1, 0.4, 105)};
  AmericanMarkovSeedConfig config;
  config.max_inversions = 6;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, config);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->inversion_calls, 6U);
  EXPECT_EQ(result->expiries[0].used_quotes, 1U);
  EXPECT_EQ(result->expiries[1].used_quotes, 1U);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::InversionBudget);
  std::reverse(quotes.begin(), quotes.end());
  const auto reversed = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, config);
  ASSERT_TRUE(reversed);
  EXPECT_EQ(result->initial_sigmas, reversed->initial_sigmas);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    EXPECT_EQ(result->quotes[i].status, reversed->quotes[quotes.size() - 1 - i].status);
  }
  config.max_inversions = 384;
  config.max_root_iteration_slots = 3 * config.max_iv_iterations;
  const auto work = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, config);
  ASSERT_TRUE(work);
  EXPECT_EQ(work->inversion_calls, 3U);
  EXPECT_EQ(work->reserved_root_iteration_slots, config.max_root_iteration_slots);
}

TEST(AmericanMarkovSeeds, CandidateCapsAndDuplicateContractsAreExplicit) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  std::vector quotes{quote(0, 0.5, 0.3, 95), quote(0, 0.5, 0.3), quote(0, 0.5, 0.3, 105)};
  AmericanMarkovSeedConfig config;
  config.candidates_per_expiry = 1;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, config);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->eligible_quotes, 3U);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::CandidateLimit);
  quotes.push_back(quotes[1]);
  const auto duplicate = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, config);
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(duplicate->quotes[1].status, QuoteStatus::DuplicateContract);
  EXPECT_EQ(duplicate->quotes[3].status, QuoteStatus::DuplicateContract);
}

TEST(AmericanMarkovSeeds, GlobalBoundsAndAloControlsFailBeforeWork) {
  std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  const std::array quotes{quote(0, 0.5, 0.3)};
  AmericanMarkovSeedConfig config;
  config.max_inversions = kAmericanMarkovSeedMaxInversions + 1;
  EXPECT_FALSE(estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes, {}, config));
  config = {};
  config.inversion_options.n_collocation = 33;
  EXPECT_FALSE(estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes, {}, config));
  config = {};
  config.inversion_options.n_quad_price = 0; // No silently selected accuracy preset.
  EXPECT_FALSE(estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes, {}, config));
  config = {};
  intervals[0].rate = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes, {}, config));
  intervals[0].rate = 0;
  const std::array knots{0.0, -0.1};
  AmericanMarkovSeedLayout layout;
  layout.log_state_knots = knots;
  EXPECT_FALSE(estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes, layout));
}

TEST(AmericanMarkovSeeds, InverterFailureRetainsOriginalErrorAndChargesAttempt) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  const std::array quotes{AmericanMarkovFitQuote{0, 100, Side::Call, 110, 111, 1}};
  const auto result = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->initial_sigmas.empty());
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::InversionFailure);
  EXPECT_NE(result->quotes[0].inversion_error.find("price above upper bound"), std::string::npos);
  EXPECT_EQ(result->inversion_calls, 1U);
  EXPECT_EQ(result->reserved_root_iteration_slots, 32U);
  EXPECT_EQ(result->eligible_quotes, 1U);
  EXPECT_EQ(result->refused_quotes, 1U);
}

TEST(AmericanMarkovSeeds, ForwardSeedBoundsRefuseRatherThanClipAndZeroVarianceIsUninformative) {
  const std::array intervals{AmericanMarkovFitInterval{0.1, 0, 0, 0, 0},
      AmericanMarkovFitInterval{1, 1, 0, 0, 0}, AmericanMarkovFitInterval{1.01, 1.01, 0, 0, 0}};
  const std::array quotes{AmericanMarkovFitQuote{0, 100, Side::Call, 1, 1.01, 1},
                           quote(1, 1, 0.1), quote(2, 1.01, 0.49)};
  AmericanMarkovSeedLayout layout;
  layout.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  layout.max_sigma = 0.5;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->parameters.size(), 2U);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::ZeroVariance);
  EXPECT_EQ(result->parameters[1].status, ParameterStatus::SigmaBounds);
  EXPECT_GT(result->parameters[1].variance_rate->midpoint, layout.max_sigma * layout.max_sigma);
  EXPECT_FALSE(result->parameters[1].sigma);
  EXPECT_TRUE(result->initial_sigmas.empty());
}

TEST(AmericanMarkovSeeds, VarianceTimeKnotLayoutIsRefusedBeforeAnyInversion) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0, 0, 0},
                             AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  const std::array quotes{quote(0, 0.25, 0.2), quote(1, 1, 0.3)};
  AmericanMarkovSeedLayout layout;
  layout.time_shape = AmericanMarkovFitTimeShape::VarianceTimeKnots;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::Unavailable);
  // The mode refusal precedes every other check: an otherwise invalid budget
  // still reports the unsupported layout, so no projection or inversion runs.
  AmericanMarkovSeedConfig invalid;
  invalid.max_inversions = 0;
  const auto ordered = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout, invalid);
  ASSERT_FALSE(ordered);
  EXPECT_EQ(ordered.error().code(), ErrorCode::Unavailable);
  // The legacy interval layout over the same quotes remains available.
  layout.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  EXPECT_TRUE(estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes, layout));
}

// ---------------------------------------------------------------------------
// SigmaInterval (opt-in sparse-market estimator). Quotes are built from
// independent Black-Scholes prices at chosen bid/ask volatilities; with zero
// carry the American call equals the European one, so equivalent IVs are exact.
// ---------------------------------------------------------------------------

AmericanMarkovFitQuote bounded(std::size_t expiry, double years, double bid_sigma,
                               double ask_sigma, double strike = 100) {
  const double bid = bid_sigma > 0 ? call(100, strike, bid_sigma * bid_sigma * years) : 0.0;
  return {expiry, strike, Side::Call, bid, call(100, strike, ask_sigma * ask_sigma * years), 1};
}

AmericanMarkovSeedConfig interval_config() {
  AmericanMarkovSeedConfig config;
  config.estimator = AmericanMarkovSeedEstimator::SigmaInterval;
  return config;
}

TEST(AmericanMarkovSeedInterval, WideAndBidlessQuotesBoundSigmaThatBidMidAskRefuses) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  const std::array quotes{bounded(0, 0.5, 0.4, 0.9), bounded(0, 0.5, 0.0, 0.8, 105)};
  const auto legacy = estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes);
  ASSERT_TRUE(legacy);
  EXPECT_TRUE(legacy->initial_sigmas.empty());
  EXPECT_EQ(legacy->quotes[0].status, QuoteStatus::WideSpread);
  EXPECT_EQ(legacy->quotes[1].status, QuoteStatus::Bidless);
  EXPECT_FALSE(legacy->sigma_interval);

  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, interval_config());
  ASSERT_TRUE(result);
  ASSERT_EQ(result->initial_sigmas.size(), 1U);
  // Feasible interval: lower from the wide bid, upper = min(0.9, 0.8) from the bidless ask.
  EXPECT_NEAR(result->initial_sigmas[0], std::sqrt(0.4 * 0.8), 1e-5);
  ASSERT_TRUE(result->sigma_interval);
  const auto &s = *result->sigma_interval;
  EXPECT_NEAR(*s.lower, 0.4, 1e-5);
  EXPECT_NEAR(*s.upper, 0.8, 1e-5);
  EXPECT_FALSE(s.crossed);
  EXPECT_NEAR(s.log_width, std::log(2.0), 1e-4);
  EXPECT_EQ(s.lower_bound_quotes, 1U);
  EXPECT_EQ(s.upper_bound_quotes, 2U);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::Used);
  EXPECT_TRUE(result->quotes[0].lower_bound && result->quotes[0].upper_bound);
  EXPECT_FALSE(result->quotes[0].equivalent_vols[1]); // Never inverts a midpoint.
  EXPECT_EQ(result->quotes[1].status, QuoteStatus::Used);
  EXPECT_FALSE(result->quotes[1].lower_bound);
  EXPECT_TRUE(result->quotes[1].upper_bound);
  // Work: two calls for the two-sided quote, one for the bidless ask.
  EXPECT_EQ(result->inversion_calls, 3U);
  EXPECT_EQ(result->reserved_root_iteration_slots, 3U * 32U);
  EXPECT_EQ(result->parameters[0].status, ParameterStatus::Ready);
  // Both sides are market bounds: no one-sided provenance is attached.
  EXPECT_FALSE(result->parameters[0].interval_lower);
  EXPECT_FALSE(result->parameters[0].interval_upper);
  ASSERT_TRUE(result->expiries[0].sigma_interval);
  EXPECT_NEAR(result->expiries[0].total_variance->midpoint, 0.4 * 0.8 * 0.5, 1e-5);
}

// A one-sided interval's midpoint would be sqrt(bound * layout constant), so a
// layout sigma bound would become the seed (and, under prior "seed", the prior
// centre). It is refused with the market bound reported instead.
TEST(AmericanMarkovSeedInterval, AskOnlyIntervalIsRefusedNotClosedWithMinSigma) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  const std::array quotes{bounded(0, 0.5, 0.0, 0.8), bounded(0, 0.5, 0.0, 1.1, 95)};
  AmericanMarkovSeedLayout layout;
  layout.min_sigma = 0.05;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout, interval_config());
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->sigma_interval);
  EXPECT_FALSE(result->sigma_interval->lower);
  EXPECT_NEAR(*result->sigma_interval->upper, 0.8, 1e-5);
  // The descriptive fields still close the open side; they are never a seed.
  EXPECT_EQ(result->sigma_interval->effective_lower, layout.min_sigma);
  EXPECT_TRUE(result->initial_sigmas.empty());
  const auto &p = result->parameters[0];
  EXPECT_EQ(p.status, ParameterStatus::OneSidedInterval);
  EXPECT_STREQ(to_string(p.status), "one_sided_interval");
  EXPECT_FALSE(p.sigma);
  EXPECT_FALSE(p.variance_rate); // No band built from the layout constant.
  EXPECT_FALSE(p.interval_lower);
  ASSERT_TRUE(p.interval_upper);
  EXPECT_NEAR(*p.interval_upper, 0.8, 1e-5);
  // The quotes still bound sigma; only the point seed is refused.
  EXPECT_EQ(result->used_quotes, 2U);
  EXPECT_EQ(result->inversion_calls, 2U);
}

TEST(AmericanMarkovSeedInterval, BidOnlyIntervalIsRefusedNotClosedWithMaxSigma) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  // Asks above max_sigma bound nothing inside the layout; the bids bound below.
  const std::array quotes{bounded(0, 0.5, 0.3, 0.9), bounded(0, 0.5, 0.35, 0.95, 105)};
  AmericanMarkovSeedLayout layout;
  layout.max_sigma = 0.6;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout, interval_config());
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->sigma_interval);
  EXPECT_NEAR(*result->sigma_interval->lower, 0.35, 1e-5);
  EXPECT_FALSE(result->sigma_interval->upper);
  EXPECT_EQ(result->sigma_interval->effective_upper, layout.max_sigma);
  EXPECT_TRUE(result->initial_sigmas.empty());
  const auto &p = result->parameters[0];
  EXPECT_EQ(p.status, ParameterStatus::OneSidedInterval);
  ASSERT_TRUE(p.interval_lower);
  EXPECT_NEAR(*p.interval_lower, 0.35, 1e-5);
  EXPECT_FALSE(p.interval_upper);
  EXPECT_FALSE(p.sigma);
  EXPECT_EQ(result->used_quotes, 2U);
  EXPECT_EQ(result->inversion_calls, 4U);
}

// Two one-sided quotes on opposite sides make a two-sided market interval.
TEST(AmericanMarkovSeedInterval, OppositeOneSidedQuotesCloseTheIntervalFromTheMarket) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  const std::array quotes{bounded(0, 0.5, 0.0, 0.8), bounded(0, 0.5, 0.35, 0.95, 105)};
  AmericanMarkovSeedLayout layout;
  layout.max_sigma = 0.9;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout, interval_config());
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->sigma_interval);
  EXPECT_NEAR(*result->sigma_interval->lower, 0.35, 1e-5);
  EXPECT_NEAR(*result->sigma_interval->upper, 0.8, 1e-5);
  EXPECT_EQ(result->parameters[0].status, ParameterStatus::Ready);
  ASSERT_EQ(result->initial_sigmas.size(), 1U);
  EXPECT_NEAR(result->initial_sigmas[0], std::sqrt(0.35 * 0.8), 1e-5);
}

TEST(AmericanMarkovSeedInterval, PerIntervalOneSidedEndpointRefusesBothDifferencedGroups) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0, 0, 0},
                             AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  AmericanMarkovSeedLayout layout;
  layout.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  // Later endpoint ask-only: group 0 seeds, group 1 refuses on endpoint 1.
  const std::array later{bounded(0, 0.25, 0.15, 0.25), bounded(1, 1, 0.0, 0.4)};
  const auto a = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, later, layout, interval_config());
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->initial_sigmas.empty());
  EXPECT_EQ(a->parameters[0].status, ParameterStatus::Ready);
  EXPECT_EQ(a->parameters[1].status, ParameterStatus::OneSidedInterval);
  ASSERT_TRUE(a->parameters[1].interval_upper);
  EXPECT_NEAR(*a->parameters[1].interval_upper, 0.4, 1e-5);
  // Earlier endpoint ask-only: both groups difference its midpoint, both refuse
  // and report endpoint 0's bound.
  const std::array earlier{bounded(0, 0.25, 0.0, 0.25), bounded(1, 1, 0.3, 0.4)};
  const auto b = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, earlier, layout, interval_config());
  ASSERT_TRUE(b);
  EXPECT_TRUE(b->initial_sigmas.empty());
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(b->parameters[i].status, ParameterStatus::OneSidedInterval) << i;
    ASSERT_TRUE(b->parameters[i].interval_upper) << i;
    EXPECT_NEAR(*b->parameters[i].interval_upper, 0.25, 1e-5) << i;
    EXPECT_FALSE(b->parameters[i].interval_lower) << i;
  }
}

TEST(AmericanMarkovSeedInterval, UnboundedBothWaysIsRefusedNotDefaulted) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  // A bidless ask above the max_sigma price bounds nothing inside the layout.
  const std::array quotes{bounded(0, 0.5, 0.0, 0.9)};
  AmericanMarkovSeedLayout layout;
  layout.max_sigma = 0.5;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout, interval_config());
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->initial_sigmas.empty());
  EXPECT_FALSE(result->sigma_interval);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::Bidless);
  EXPECT_GT(*result->quotes[0].equivalent_vols[2], layout.max_sigma); // Preserved, not clipped.
  EXPECT_EQ(result->parameters[0].status, ParameterStatus::MissingEndpoint);
  EXPECT_EQ(result->inversion_calls, 1U);
}

TEST(AmericanMarkovSeedInterval, CrossedBoundsAreReportedAndNeverRepaired) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  const std::array quotes{bounded(0, 0.5, 0.5, 0.6), bounded(0, 0.5, 0.2, 0.3, 105)};
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, interval_config());
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->sigma_interval);
  const auto &s = *result->sigma_interval;
  EXPECT_TRUE(s.crossed);
  EXPECT_NEAR(*s.lower, 0.5, 1e-5);
  EXPECT_NEAR(*s.upper, 0.3, 1e-5);
  EXPECT_LT(s.log_width, 0.0);
  ASSERT_EQ(result->initial_sigmas.size(), 1U);
  EXPECT_NEAR(result->initial_sigmas[0], std::sqrt(0.5 * 0.3), 1e-5);
  // The descriptive band is ordered even when the bounds cross.
  EXPECT_LT(result->parameters[0].variance_rate->lower, result->parameters[0].variance_rate->upper);
}

TEST(AmericanMarkovSeedInterval, PerVarianceIntervalDifferencesGeometricMidpointVariance) {
  const std::array intervals{AmericanMarkovFitInterval{0.25, 0.25, 0, 0, 0},
                             AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  const std::array quotes{bounded(0, 0.25, 0.15, 0.25), bounded(1, 1, 0.3, 0.4)};
  AmericanMarkovSeedLayout layout;
  layout.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout, interval_config());
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->sigma_interval); // Shared-only summary.
  ASSERT_EQ(result->initial_sigmas.size(), 2U);
  const double first = 0.15 * 0.25;
  const double second = (0.3 * 0.4 * 1.0 - first * 0.25) / 0.75;
  EXPECT_NEAR(result->initial_sigmas[0], std::sqrt(first), 1e-5);
  EXPECT_NEAR(result->initial_sigmas[1], std::sqrt(second), 1e-5);
  // No adjacent-support requirement: the conservative band may go negative.
  EXPECT_EQ(result->parameters[1].status, ParameterStatus::Ready);
  ASSERT_TRUE(result->variance_endpoints[1].sigma_interval);
  // A decreasing geometric-midpoint total variance is refused, never clipped.
  const std::array decreasing{bounded(0, 0.25, 0.8, 0.9), bounded(1, 1, 0.1, 0.2)};
  const auto negative = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, decreasing, layout, interval_config());
  ASSERT_TRUE(negative);
  EXPECT_TRUE(negative->initial_sigmas.empty());
  EXPECT_EQ(negative->parameters[1].status, ParameterStatus::NegativeForwardVariance);
}

TEST(AmericanMarkovSeedInterval, BudgetChargesOnlyInformativeSidesAndRefusesWholeQuotes) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0},
                             AmericanMarkovFitInterval{1, 1, 0, 0, 0}};
  const std::array quotes{bounded(0, 0.5, 0.3, 0.6), bounded(0, 0.5, 0.3, 0.6, 105),
                          bounded(1, 1, 0.0, 0.7)};
  auto config = interval_config();
  config.max_inversions = 3;
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, {}, config);
  ASSERT_TRUE(result);
  // Rank 0: expiry 0 two-sided (2 calls), expiry 1 bidless (1 call); rank 1 cannot fit.
  EXPECT_EQ(result->inversion_calls, 3U);
  EXPECT_EQ(result->reserved_root_iteration_slots, 3U * config.max_iv_iterations);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::Used);
  EXPECT_EQ(result->quotes[1].status, QuoteStatus::InversionBudget);
  EXPECT_EQ(result->quotes[2].status, QuoteStatus::Used);
  EXPECT_EQ(result->used_quotes, 2U);
  EXPECT_EQ(result->refused_quotes, 1U);
}

TEST(AmericanMarkovSeedInterval, IntrinsicAskFarMoneynessAndConflictsAreExplicit) {
  const std::array intervals{AmericanMarkovFitInterval{0.5, 0.5, 0, 0, 0}};
  std::array quotes{bounded(0, 0.5, 0.0, 0.5, 150), bounded(0, 0.5, 0.0, 0.3),
                    bounded(0, 0.5, 0.0, 0.3, 105)};
  quotes[1].strike = 85; // ITM call (inside the band) asked at intrinsic: no information.
  quotes[1].ask = 15;
  AmericanMarkovSeedLayout layout;
  layout.min_sigma = 0.4; // The third quote's ask says sigma <= 0.3: conflicts.
  const auto result = estimate_american_markov_quote_seeds(
      {100, intervals, {}, {}}, quotes, layout, interval_config());
  ASSERT_TRUE(result);
  EXPECT_EQ(result->quotes[0].status, QuoteStatus::FarMoneyness);
  EXPECT_EQ(result->quotes[1].status, QuoteStatus::IntrinsicPlateau);
  EXPECT_EQ(result->quotes[2].status, QuoteStatus::SigmaBounds);
  EXPECT_LT(*result->quotes[2].equivalent_vols[2], layout.min_sigma);
  EXPECT_TRUE(result->initial_sigmas.empty());
  EXPECT_EQ(result->inversion_calls, 1U);
  AmericanMarkovSeedConfig invalid;
  invalid.estimator = static_cast<AmericanMarkovSeedEstimator>(9);
  EXPECT_FALSE(estimate_american_markov_quote_seeds({100, intervals, {}, {}}, quotes, {}, invalid));
}

} // namespace
} // namespace atx::vol
