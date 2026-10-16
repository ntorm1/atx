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

} // namespace
} // namespace atx::vol
