#include "atx/vol/api/marketdata/american_markov_market.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {
using MarketStatus = AmericanMarkovMarketStatus;
using Dividends = AmericanMarkovMarketDividends;

AmericanMarkovRawQuote raw(std::uint64_t expiry, double strike = 100.0) {
  AmericanMarkovRawQuote q;
  q.expiry_key = expiry;
  q.strike = strike;
  q.bid = 1.0;
  q.ask = 2.0;
  q.underlying_spot = 100.0;
  q.calendar_years = 0.5;
  q.variance_years = 0.4;
  q.rate = 0.04;
  q.continuous_yield = 0.02;
  return q;
}

void expect_refusal(const AmericanMarkovMarketPreparation &out, MarketStatus status) {
  EXPECT_EQ(out.status, status) << to_string(out.status);
  EXPECT_FALSE(out.market);
}

TEST(AmericanMarkovMarket, ResolvesCommonSpotAndRobustCarryWithoutQuoteWeighting) {
  std::array rows{raw(7, 80.0), raw(7, 100.0), raw(7, 120.0)};
  rows[0].underlying_spot = 100.02;
  rows[1].underlying_spot = 99.98;
  rows[0].continuous_yield = 0.01;
  rows[1].continuous_yield = 0.03;
  rows[0].weight = 1000000.0;
  rows[1].weight = 1.0e-100;
  const auto out = prepare_american_markov_market(rows);
  ASSERT_EQ(out.status, MarketStatus::Success);
  ASSERT_TRUE(out.market);
  EXPECT_DOUBLE_EQ(out.market->spot(), 100.0);
  EXPECT_DOUBLE_EQ(out.market->intervals()[0].continuous_yield, 0.02);
  const auto &d = out.diagnostics.expiries[0];
  EXPECT_DOUBLE_EQ(d.supplied_yield.median, 0.02);
  EXPECT_DOUBLE_EQ(d.supplied_yield.minimum, 0.01);
  EXPECT_DOUBLE_EQ(d.supplied_yield.maximum, 0.03);
  EXPECT_NEAR(d.supplied_yield.median_absolute_deviation, 0.01, 1.0e-16);
  EXPECT_NEAR(d.prepaid_over_spot.minimum, std::exp(-0.015), 1.0e-15);
  EXPECT_NEAR(d.prepaid_over_spot.maximum, std::exp(-0.005), 1.0e-15);
  EXPECT_DOUBLE_EQ(out.market->quotes()[0].weight, rows[0].weight);
  EXPECT_FALSE(out.diagnostics.refused_row);
  EXPECT_FALSE(out.diagnostics.refused_expiry);
}

TEST(AmericanMarkovMarket, TermStructureUsesCumulativePrefixesAndKeepsOriginalOrder) {
  std::array rows{raw(1, 110.0), raw(9, 80.0), raw(1, 100.0), raw(9, 90.0)};
  for (auto &q : rows) {
    if (q.expiry_key == 1) {
      q.calendar_years = 1.0;
      q.variance_years = 0.7;
      q.rate = 0.05;
      q.continuous_yield = 0.03;
    }
  }
  const auto out = prepare_american_markov_market(rows);
  ASSERT_EQ(out.status, MarketStatus::Success);
  const auto &market = *out.market;
  ASSERT_EQ(market.intervals().size(), 2U);
  EXPECT_EQ(market.expiry_keys()[0], 9U);
  EXPECT_EQ(market.expiry_keys()[1], 1U);
  EXPECT_NEAR(market.intervals()[1].rate, 0.06, 1.0e-15);
  EXPECT_NEAR(market.intervals()[1].continuous_yield, 0.04, 1.0e-15);
  EXPECT_DOUBLE_EQ(market.intervals()[1].variance_end, 0.7);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_DOUBLE_EQ(market.quotes()[i].strike, rows[i].strike);
    EXPECT_EQ(market.quotes()[i].expiry_index, rows[i].expiry_key == 9 ? 0U : 1U);
    EXPECT_DOUBLE_EQ(market.quotes()[i].bid, rows[i].bid);
  }
}

TEST(AmericanMarkovMarket, PreparedMarketOwnsInputAndBorrowsOnlyExplicitGrid) {
  auto row = raw(8);
  auto out = prepare_american_markov_market(std::span(&row, 1));
  ASSERT_TRUE(out.market);
  row.strike = 500.0;
  row.underlying_spot = 123.0;
  auto moved = std::move(*out.market);
  const std::array grid{0.0, 100.0, 300.0};
  AmericanMarkovConfig model_config;
  model_config.max_states = 32;
  const auto view = moved.fit_market(model_config, grid);
  EXPECT_DOUBLE_EQ(view.spot, 100.0);
  EXPECT_DOUBLE_EQ(moved.quotes()[0].strike, 100.0);
  EXPECT_EQ(view.intervals.data(), moved.intervals().data());
  EXPECT_EQ(view.state_grid.data(), grid.data());
  EXPECT_EQ(view.model_config.max_states, 32U);
}

TEST(AmericanMarkovMarket, FundingAndEscrowedPvConventionsPreserveDistinctForwards) {
  auto row = raw(5);
  row.continuous_yield = 0.08;
  row.dividend_pv = 3.0;
  AmericanMarkovMarketConfig config;
  config.dividends = Dividends::FundingPvContinuousYield;
  const auto funding = prepare_american_markov_market(std::span(&row, 1), config);
  ASSERT_TRUE(funding.market);
  config.dividends = Dividends::EscrowedPvContinuousYield;
  const auto escrowed = prepare_american_markov_market(std::span(&row, 1), config);
  ASSERT_TRUE(escrowed.market);
  const auto &f = funding.diagnostics.expiries[0];
  const auto &e = escrowed.diagnostics.expiries[0];
  const double expected_funding = 100.0 * std::exp(-0.04) - 3.0;
  const double expected_escrowed = 97.0 * std::exp(-0.04);
  EXPECT_NEAR(100.0 * f.resolved_prepaid_over_spot, expected_funding, 2.0e-14);
  EXPECT_NEAR(100.0 * e.resolved_prepaid_over_spot, expected_escrowed, 2.0e-14);
  EXPECT_NEAR(100.0 * f.resolved_forward_over_spot,
              expected_funding * std::exp(0.02), 2.0e-14);
  EXPECT_NE(f.interval_yield, e.interval_yield);
  EXPECT_DOUBLE_EQ(funding.market->intervals()[0].cash_dividend_at_end, 0.0);
}

TEST(AmericanMarkovMarket, NegativeYieldAllowsFundingPvAboveSpot) {
  auto row = raw(5);
  row.continuous_yield = -2.0;
  row.dividend_pv = 150.0;
  AmericanMarkovMarketConfig config;
  config.dividends = Dividends::FundingPvContinuousYield;
  const auto funding = prepare_american_markov_market(std::span(&row, 1), config);
  ASSERT_TRUE(funding.market) << to_string(funding.status);
  EXPECT_NEAR(100.0 * funding.diagnostics.expiries[0].resolved_prepaid_over_spot,
              100.0 * std::exp(1.0) - 150.0, 6.0e-14);
  config.dividends = Dividends::EscrowedPvContinuousYield;
  expect_refusal(prepare_american_markov_market(std::span(&row, 1), config),
                 MarketStatus::NonpositivePrepaid);
  config.dividends = Dividends::None;
  expect_refusal(prepare_american_markov_market(std::span(&row, 1), config),
                 MarketStatus::DividendConvention);
}

TEST(AmericanMarkovMarket, InfeasibleIndividualCarryCannotBeHiddenByMedian) {
  std::array rows{raw(5, 90.0), raw(5, 100.0), raw(5, 110.0)};
  rows[2].dividend_pv = 100.0;
  AmericanMarkovMarketConfig config;
  config.dividends = Dividends::FundingPvContinuousYield;
  const auto out = prepare_american_markov_market(rows, config);
  expect_refusal(out, MarketStatus::NonpositivePrepaid);
  ASSERT_TRUE(out.diagnostics.refused_row);
  EXPECT_EQ(*out.diagnostics.refused_row, 2U);
}

TEST(AmericanMarkovMarket, FundingPvRemainsScaleInvariantAcrossFiniteStockScales) {
  AmericanMarkovMarketConfig config;
  config.dividends = Dividends::FundingPvContinuousYield;
  for (const double scale : {1.0e-200, 1.0, 1.0e200}) {
    auto row = raw(5);
    row.underlying_spot = 100.0 * scale;
    row.dividend_pv = 150.0 * scale;
    row.continuous_yield = -2.0;
    const auto out = prepare_american_markov_market(std::span(&row, 1), config);
    ASSERT_TRUE(out.market) << to_string(out.status);
    EXPECT_NEAR(out.diagnostics.expiries[0].resolved_prepaid_over_spot,
                std::exp(1.0) - 1.5, 1.0e-15);
  }
  auto impossible = raw(5);
  impossible.continuous_yield = 0.0;
  impossible.dividend_pv = 100.0;
  expect_refusal(prepare_american_markov_market(std::span(&impossible, 1), config),
                 MarketStatus::NonpositivePrepaid);
}

TEST(AmericanMarkovMarket, FundingPvTermPrefixesRecoverEveryQuotedTerminalPrepaid) {
  std::array rows{raw(5), raw(6), raw(7)};
  rows[1].calendar_years = 1.0;
  rows[1].variance_years = 0.8;
  rows[1].rate = -0.01;
  rows[1].continuous_yield = -0.03;
  rows[1].dividend_pv = 3.0;
  rows[2].calendar_years = 2.0;
  rows[2].variance_years = 1.7;
  rows[2].rate = 0.03;
  rows[2].continuous_yield = 0.01;
  rows[2].dividend_pv = 5.0;
  AmericanMarkovMarketConfig config;
  config.dividends = Dividends::FundingPvContinuousYield;
  const auto out = prepare_american_markov_market(rows, config);
  ASSERT_TRUE(out.market);
  double previous = 0.0;
  double integral_r = 0.0;
  double integral_q = 0.0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &interval = out.market->intervals()[i];
    const double dt = interval.calendar_end - previous;
    integral_r += dt * interval.rate;
    integral_q += dt * interval.continuous_yield;
    EXPECT_NEAR(integral_r, rows[i].rate * rows[i].calendar_years, 1.0e-16);
    const double prepaid =
        100.0 * std::exp(-rows[i].continuous_yield * rows[i].calendar_years) - rows[i].dividend_pv;
    EXPECT_NEAR(100.0 * std::exp(-integral_q), prepaid, 2.0e-14);
    EXPECT_DOUBLE_EQ(interval.cash_dividend_at_end, 0.0);
    previous = interval.calendar_end;
  }
}

TEST(AmericanMarkovMarket, RefusesOutliersAndRetainsFullDispersionSummary) {
  std::array rows{raw(5, 90.0), raw(5, 100.0), raw(5, 110.0)};
  rows[2].continuous_yield = 0.1;
  const auto out = prepare_american_markov_market(rows);
  expect_refusal(out, MarketStatus::CarryDispersion);
  ASSERT_EQ(out.diagnostics.expiries.size(), 1U);
  const auto &d = out.diagnostics.expiries[0];
  EXPECT_DOUBLE_EQ(d.supplied_yield.median, 0.02);
  EXPECT_DOUBLE_EQ(d.supplied_yield.median_absolute_deviation, 0.0);
  EXPECT_DOUBLE_EQ(d.supplied_yield.maximum, 0.1);
  EXPECT_NEAR(d.supplied_yield.maximum_absolute_deviation, 0.08, 1.0e-16);
  EXPECT_GT(d.prepaid_over_spot.maximum_absolute_deviation, 0.0);
  ASSERT_TRUE(out.diagnostics.refused_expiry);
  EXPECT_EQ(*out.diagnostics.refused_expiry, 5U);
}

TEST(AmericanMarkovMarket, NormalizedForwardDispersionProtectsLongMaturities) {
  std::array rows{raw(5, 90.0), raw(5, 100.0), raw(5, 110.0)};
  for (auto &q : rows) {
    q.calendar_years = 10.0;
    q.rate = 0.04;
    q.continuous_yield = 0.0;
  }
  rows[2].continuous_yield = 0.04; // Annual q discrepancy passes .05 gate.
  const auto out = prepare_american_markov_market(rows);
  expect_refusal(out, MarketStatus::CarryDispersion);
  EXPECT_GT(out.diagnostics.expiries[0].forward_over_spot.maximum_absolute_deviation, 0.05);
}

TEST(AmericanMarkovMarket, ClockAndSpotDiscrepanciesHaveDistinctRefusals) {
  std::array rows{raw(5, 90.0), raw(5, 100.0), raw(5, 110.0)};
  rows[2].calendar_years += 1.0e-7;
  const auto calendar = prepare_american_markov_market(rows);
  expect_refusal(calendar, MarketStatus::ClockDispersion);
  EXPECT_GT(calendar.diagnostics.expiries[0].calendar_years.maximum_absolute_deviation, 0.0);
  rows[2] = raw(5, 110.0);
  rows[2].variance_years += 1.0e-7;
  expect_refusal(prepare_american_markov_market(rows), MarketStatus::ClockDispersion);
  rows[2] = raw(5, 110.0);
  rows[2].underlying_spot = 102.0;
  const auto spot = prepare_american_markov_market(rows);
  expect_refusal(spot, MarketStatus::SpotDispersion);
  EXPECT_DOUBLE_EQ(spot.diagnostics.spot.median, 100.0);
  EXPECT_DOUBLE_EQ(spot.diagnostics.spot.maximum, 102.0);
}

TEST(AmericanMarkovMarket, TolerancesAreInclusiveAndZeroRequiresAgreement) {
  std::array rows{raw(5, 90.0), raw(5, 100.0), raw(5, 110.0)};
  for (auto &q : rows) q.continuous_yield = 0.0;
  rows[2].continuous_yield = 0.03125;
  AmericanMarkovMarketConfig config;
  config.max_yield_absolute_deviation = 0.03125;
  EXPECT_TRUE(prepare_american_markov_market(rows, config).market);
  config.max_yield_absolute_deviation = std::nextafter(0.03125, 0.0);
  expect_refusal(prepare_american_markov_market(rows, config), MarketStatus::CarryDispersion);
  rows[2].continuous_yield = 0.0;
  config.max_yield_absolute_deviation = 0.0;
  EXPECT_TRUE(prepare_american_markov_market(rows, config).market);
}

TEST(AmericanMarkovMarket, DuplicateContractsRefusedButOppositeSidesRemainDistinct) {
  std::array rows{raw(5), raw(5)};
  expect_refusal(prepare_american_markov_market(rows), MarketStatus::DuplicateContract);
  rows[1].side = Side::Put;
  EXPECT_TRUE(prepare_american_markov_market(rows).market);
}

TEST(AmericanMarkovMarket, AmbiguousExpiryAndDecreasingVarianceAreRefusedWithoutRepair) {
  std::array rows{raw(5), raw(6)};
  expect_refusal(prepare_american_markov_market(rows), MarketStatus::AmbiguousExpiry);
  rows[1].calendar_years = 1.0;
  rows[1].variance_years = 0.3;
  expect_refusal(prepare_american_markov_market(rows), MarketStatus::NonmonotoneVarianceClock);
  rows[1].variance_years = 0.4;
  EXPECT_TRUE(prepare_american_markov_market(rows).market); // Paused variance is valid.
}

TEST(AmericanMarkovMarket, CapsDifferencedAndCumulativeCarryHazards) {
  std::array rows{raw(5), raw(6)};
  rows[1].calendar_years = 0.50000001;
  rows[1].rate = 0.05;
  const auto differenced = prepare_american_markov_market(rows);
  expect_refusal(differenced, MarketStatus::NumericLimit);
  EXPECT_GT(differenced.diagnostics.expiries[1].interval_rate, 10.0);
  rows[1].calendar_years = 100.0;
  rows[1].rate = 1.0;
  expect_refusal(prepare_american_markov_market(rows), MarketStatus::NumericLimit);
}

TEST(AmericanMarkovMarket, InvalidRowsAndInvalidConfigurationsNeverPublishPartialMarkets) {
  const auto row = raw(3);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (auto member : {&AmericanMarkovRawQuote::underlying_spot,
       &AmericanMarkovRawQuote::calendar_years, &AmericanMarkovRawQuote::variance_years,
       &AmericanMarkovRawQuote::rate, &AmericanMarkovRawQuote::continuous_yield,
       &AmericanMarkovRawQuote::dividend_pv, &AmericanMarkovRawQuote::strike,
       &AmericanMarkovRawQuote::bid, &AmericanMarkovRawQuote::ask,
       &AmericanMarkovRawQuote::weight}) {
    auto invalid = row;
    invalid.*member = nan;
    const auto out = prepare_american_markov_market(std::span(&invalid, 1));
    expect_refusal(out, MarketStatus::InvalidRow);
    EXPECT_EQ(out.diagnostics.refused_row, 0U);
  }
  AmericanMarkovMarketConfig config;
  config.max_yield_absolute_deviation = nan;
  expect_refusal(prepare_american_markov_market(std::span(&row, 1), config),
                 MarketStatus::InvalidConfig);
  config = {};
  config.dividends = static_cast<Dividends>(255);
  expect_refusal(prepare_american_markov_market(std::span(&row, 1), config),
                 MarketStatus::InvalidConfig);
}

TEST(AmericanMarkovMarket, ResourceCapsRefuseBeforePartialPublication) {
  std::array rows{raw(5), raw(6)};
  rows[1].calendar_years = 1.0;
  AmericanMarkovMarketConfig config;
  config.max_quotes = 1;
  expect_refusal(prepare_american_markov_market(rows, config), MarketStatus::ResourceLimit);
  config.max_quotes = 2;
  config.max_expiries = 1;
  expect_refusal(prepare_american_markov_market(rows, config), MarketStatus::ResourceLimit);
  expect_refusal(prepare_american_markov_market({}), MarketStatus::ResourceLimit);
  config.max_expiries = kAmericanMarkovMarketMaxExpiries + 1;
  expect_refusal(prepare_american_markov_market(rows, config), MarketStatus::InvalidConfig);
}

TEST(AmericanMarkovMarket, BidlessAndLockedQuotesPassThroughWithoutMidpointTargets) {
  std::array rows{raw(5, 90.0), raw(5, 100.0)};
  rows[0].bid = 0.0;
  rows[1].bid = rows[1].ask;
  const auto out = prepare_american_markov_market(rows);
  ASSERT_TRUE(out.market);
  EXPECT_DOUBLE_EQ(out.market->quotes()[0].bid, 0.0);
  EXPECT_DOUBLE_EQ(out.market->quotes()[1].bid, out.market->quotes()[1].ask);
  rows[1].bid = rows[1].ask + 0.01;
  expect_refusal(prepare_american_markov_market(rows), MarketStatus::InvalidRow);
}

TEST(AmericanMarkovMarket, InputPermutationPreservesMarketAndRemapsContracts) {
  std::vector rows{raw(4, 90.0), raw(4, 100.0), raw(4, 110.0), raw(8, 100.0)};
  rows[0].continuous_yield = 0.03;
  rows[1].continuous_yield = 0.01;
  rows[3].calendar_years = 0.8;
  rows[3].variance_years = 0.7;
  const auto first = prepare_american_markov_market(rows);
  std::reverse(rows.begin(), rows.end());
  const auto second = prepare_american_markov_market(rows);
  ASSERT_TRUE(first.market);
  ASSERT_TRUE(second.market);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_DOUBLE_EQ(first.market->intervals()[i].rate, second.market->intervals()[i].rate);
    EXPECT_DOUBLE_EQ(first.market->intervals()[i].continuous_yield,
                     second.market->intervals()[i].continuous_yield);
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(first.market->quotes()[i].expiry_index,
              second.market->quotes()[rows.size() - 1 - i].expiry_index);
  }
}

TEST(AmericanMarkovMarket, FitterIntegrationPreservesEuropeanForwardIdentityAtEveryPrefix) {
  std::array rows{raw(4), raw(8)};
  rows[1].calendar_years = 1.0;
  rows[1].variance_years = 0.8;
  rows[1].rate = 0.05;
  rows[1].continuous_yield = 0.03;
  for (auto &q : rows) { q.bid = 0.0; q.ask = 100.0; }
  const auto prepared = prepare_american_markov_market(rows);
  ASSERT_TRUE(prepared.market);
  AmericanMarkovFitConfig fit_config;
  fit_config.max_evaluations = 1;
  AmericanMarkovConfig model_config;
  model_config.grid_nodes = 17;
  const auto fitted = fit_american_markov_intervals(
      prepared.market->fit_market(model_config), prepared.market->quotes(), fit_config);
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  AmericanMarkovWorkspace workspace;
  ASSERT_TRUE(workspace.prepare(fitted->model()));
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto call = price_american_markov(fitted->model(), 100.0, Side::Call, i, workspace,
                                           ExerciseStyle::European);
    const auto put = price_american_markov(fitted->model(), 100.0, Side::Put, i, workspace,
                                          ExerciseStyle::European);
    ASSERT_TRUE(call);
    ASSERT_TRUE(put);
    const double years = rows[i].calendar_years;
    const double parity = 100.0 * std::exp(-rows[i].continuous_yield * years) -
                          100.0 * std::exp(-rows[i].rate * years);
    EXPECT_NEAR(*call - *put, parity, 1.0e-12);
  }
}

} // namespace
} // namespace atx::vol
