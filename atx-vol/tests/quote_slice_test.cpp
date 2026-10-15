#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/black76.hpp"

namespace atx::vol {
namespace {

std::vector<QuoteSliceQuote> flat_quotes(const QuoteSliceMarket &market, double sigma = 0.25) {
  std::vector<QuoteSliceQuote> quotes;
  const double ratio = market.calendar_years / market.variance_years;
  for (int strike = 75; strike <= 125; strike += 5) {
    for (Side side : {Side::Call, Side::Put}) {
      const auto price =
          andersen_lake(market.spot, static_cast<double>(strike), market.variance_years, sigma,
                        market.rate * ratio, market.continuous_yield * ratio, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return {};
      }
      const double half_spread = std::min(0.01, *price * 0.1);
      quotes.push_back(
          {static_cast<double>(strike), side, *price - half_spread, *price + half_spread});
    }
  }
  return quotes;
}

TEST(QuoteSlice, SeparateClocksPreserveCarryAndRecoverVolatility) {
  const QuoteSliceMarket market{100.0, 0.4, 0.5, 0.04, 0.015, 0.0};
  const auto quotes = flat_quotes(market);
  auto fitted = fit_quote_slice(market, quotes);
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  EXPECT_NEAR(fitted->diagnostics.effective_rate, 0.05, 1.0e-15);
  EXPECT_NEAR(fitted->diagnostics.effective_yield, 0.01875, 1.0e-15);
  EXPECT_NEAR(fitted->diagnostics.forward, 100.0 * std::exp(0.025 * 0.5), 1.0e-12);
  EXPECT_EQ(fitted->diagnostics.quotes_admitted, 11U);
  EXPECT_EQ(fitted->diagnostics.prices_produced, quotes.size());
  for (const auto &row : fitted->rows) {
    EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
    EXPECT_NEAR(row.fitted_iv, 0.25, 5.0e-4);
    if (row.admitted) {
      EXPECT_NEAR(row.market_iv, 0.25, 5.0e-6);
    }
  }
  const auto value = evaluate_quote_slice(*fitted, 102.5, Side::Call);
  ASSERT_TRUE(value) << value.error().to_string();
  EXPECT_NEAR(value->volatility, 0.25, 5.0e-4);
  const auto expected =
      andersen_lake(100.0, 102.5, 0.4, value->volatility, 0.05, 0.01875, Side::Call);
  ASSERT_TRUE(expected);
  EXPECT_DOUBLE_EQ(value->price, *expected);
}

TEST(QuoteSlice, RepricesRejectedQuotesAndPreservesTheirProvenance) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  auto quotes = flat_quotes(market);
  quotes.push_back({103.0, Side::Call, 3.0, 2.0});
  quotes.push_back({-1.0, Side::Call, 1.0, 2.0});
  auto fitted = fit_quote_slice(market, quotes);
  ASSERT_TRUE(fitted);
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  const auto &bad_quote = fitted->rows[quotes.size() - 2];
  EXPECT_EQ(bad_quote.quote_status, QuoteSliceQuoteStatus::InvalidQuote);
  EXPECT_FALSE(bad_quote.admitted);
  EXPECT_EQ(bad_quote.evaluation_status, QuoteSliceEvaluationStatus::Success);
  EXPECT_TRUE(std::isfinite(bad_quote.fitted_price));
  const auto &bad_contract = fitted->rows.back();
  EXPECT_EQ(bad_contract.quote_status, QuoteSliceQuoteStatus::InvalidContract);
  EXPECT_EQ(bad_contract.evaluation_status, QuoteSliceEvaluationStatus::InvalidContract);
  EXPECT_TRUE(std::isnan(bad_contract.fitted_price));
  EXPECT_EQ(fitted->rows.front().quote_status, QuoteSliceQuoteStatus::ExcludedSide);
  EXPECT_EQ(fitted->rows.front().evaluation_status, QuoteSliceEvaluationStatus::Success);
}

TEST(QuoteSlice, SmoothingProducesAFittedSmileRatherThanMidpointIdentity) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  auto quotes = flat_quotes(market);
  // A raw quote perturbation is the only changed input. An eSSVI fit has fewer
  // degrees of freedom than these strikes and should smooth this isolated bump.
  std::size_t changed = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (quotes[i].strike == 105.0 && quotes[i].side == Side::Call) {
      quotes[i].bid += 0.10;
      quotes[i].ask += 0.10;
      changed = i;
    }
  }
  auto fitted = fit_quote_slice(market, quotes);
  ASSERT_TRUE(fitted);
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  ASSERT_TRUE(fitted->rows[changed].admitted);
  const double mid = (quotes[changed].bid + quotes[changed].ask) * 0.5;
  EXPECT_GT(std::abs(fitted->rows[changed].fitted_price - mid), 0.005);
}

TEST(QuoteSlice, AggregateCashIsAnExplicitApproximationAndCannotBeDoubleCounted) {
  const QuoteSliceMarket market{100.0, 0.4, 0.5, 0.04, 0.01, 2.0};
  EXPECT_FALSE(fit_quote_slice(market, {})); // refusing silently ignored cash
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::AggregatePresentValue;
  auto fitted = fit_quote_slice(market, {}, config);
  ASSERT_TRUE(fitted);
  EXPECT_EQ(fitted->fit_status, QuoteSliceFitStatus::InsufficientQuotes);
  EXPECT_NEAR(fitted->diagnostics.forward, 98.0 * std::exp(0.03 * 0.5), 1.0e-12);
  EXPECT_NEAR(fitted->diagnostics.effective_yield, 0.0125 - std::log(0.98) / 0.4, 1.0e-14);
  EXPECT_STREQ(to_string(fitted->dividend_model), "aggregate_pv_approximation");
  const std::vector<CashDividend> events{{0.2, 2.0}};
  EXPECT_FALSE(fit_quote_slice(market, {}, config, events));
  config.dividend_model = QuoteSliceDividendModel::DatedCash;
  EXPECT_FALSE(fit_quote_slice(market, {}, config, events));
}

TEST(QuoteSlice, DatedCashUsesTheSameLatticeForInversionAndNewStrikeQueries) {
  const QuoteSliceMarket market{100.0, 0.4, 0.5, 0.04, 0.0, 0.0};
  const std::vector<CashDividend> calendar_events{{0.2, 1.8}, {0.8, 2.0}};
  const std::vector<CashDividend> model_events{{0.16, 1.8}};
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::DatedCash;
  config.curve.kind = VolCurveKind::LinearVariance;
  config.min_quotes = 5;
  config.discrete_div_steps = 101;
  std::vector<QuoteSliceQuote> quotes;
  for (int strike = 85; strike <= 115; strike += 5) {
    const auto side = strike <= 100 ? Side::Put : Side::Call;
    const auto value = american_discrete_div_price(100.0, static_cast<double>(strike), 0.4, 0.25,
                                                   0.05, 0.0, side, model_events, 101);
    ASSERT_TRUE(value) << value.error().to_string();
    quotes.push_back({static_cast<double>(strike), side, *value - 0.001, *value + 0.001});
  }
  auto fitted = fit_quote_slice(market, quotes, config, calendar_events);
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  ASSERT_EQ(fitted->variance_clock_dividends.size(), 1U);
  EXPECT_NEAR(fitted->variance_clock_dividends[0].tau, 0.16, 1.0e-15);
  for (const auto &row : fitted->rows) {
    ASSERT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
    EXPECT_NEAR(row.market_iv, 0.25, 2.0e-6);
    EXPECT_NEAR(row.fitted_iv, 0.25, 2.0e-6);
  }
  const auto value = evaluate_quote_slice(*fitted, 102.5, Side::Call);
  ASSERT_TRUE(value);
  const auto expected = american_discrete_div_price(100.0, 102.5, 0.4, value->volatility, 0.05, 0.0,
                                                    Side::Call, model_events, 101);
  ASSERT_TRUE(expected);
  EXPECT_NEAR(value->price, *expected, 1.0e-11);
}

TEST(QuoteSlice, InsufficientDataRetainsEveryRowAndDoesNotFabricateAFit) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  auto quotes = flat_quotes(market);
  QuoteSliceConfig config;
  config.min_quotes = 50;
  auto fitted = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(fitted);
  EXPECT_EQ(fitted->fit_status, QuoteSliceFitStatus::InsufficientQuotes);
  EXPECT_EQ(fitted->rows.size(), quotes.size());
  EXPECT_FALSE(fitted->curve);
  EXPECT_FALSE(evaluate_quote_slice(*fitted, 100.0, Side::Call));
  for (const auto &row : fitted->rows) {
    EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::NoFit);
    EXPECT_TRUE(std::isnan(row.fitted_price));
  }
}

TEST(QuoteSlice, TerminalCashCallAgreesWithIndependentEuropeanExerciseLimit) {
  // With q=0 and r>=0, a call's only useful exercise opportunity is immediately
  // before the terminal cash payment. Its exact value is the dividend-free
  // Black-Scholes call. These quotes therefore do not use the lattice under
  // test, and the recovered volatility exposes its finite-grid error.
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.0, 0.0};
  const std::vector<CashDividend> dividends{{0.5, 4.0}};
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::DatedCash;
  config.side_policy = QuoteSliceSidePolicy::All;
  config.curve.kind = VolCurveKind::LinearVariance;
  config.min_quotes = 5;
  config.discrete_div_steps = 1001;
  std::vector<QuoteSliceQuote> quotes;
  for (const double strike : {90.0, 95.0, 100.0, 105.0, 110.0}) {
    const double exact =
        black76_price(100.0 * std::exp(0.02), strike, 0.5, 0.25, std::exp(-0.02), Side::Call);
    quotes.push_back({strike, Side::Call, exact - 0.001, exact + 0.001});
  }
  auto fitted = fit_quote_slice(market, quotes, config, dividends);
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  for (const auto &row : fitted->rows) {
    ASSERT_TRUE(row.admitted);
    EXPECT_NEAR(row.market_iv, 0.25, 2.0e-4);
  }
}

TEST(QuoteSlice, InvalidSharedInputsFailWithoutPartiallyUsableOutput) {
  QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  QuoteSliceConfig config;
  config.iv_tolerance = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(fit_quote_slice(market, {}, config));
  market.variance_years = 0.0;
  EXPECT_FALSE(fit_quote_slice(market, {}));
  market.variance_years = 0.5;
  market.rate = std::numeric_limits<double>::max();
  EXPECT_FALSE(fit_quote_slice(market, {}));
}

TEST(QuoteSlice, StrikeSpecificCarryRecoversFlatVolAndRepricesWithLocalYield) {
  const QuoteSliceMarket market{100.0, 0.4, 0.5, 0.04, 0.015, 0.0};
  std::vector<QuoteSliceQuote> quotes;
  for (int strike = 75; strike <= 125; strike += 5) {
    const double K = static_cast<double>(strike);
    const double yield = 0.015 + 0.08 * std::log(K / 100.0);
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(100.0, K, 0.4, 0.25, 0.05, yield * 1.25, side);
      ASSERT_TRUE(price) << price.error().to_string();
      const double half_spread = std::min(0.01, *price * 0.1);
      quotes.push_back({K, side, *price - half_spread, *price + half_spread, yield});
    }
  }
  auto fitted = fit_quote_slice(market, quotes);
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  ASSERT_EQ(fitted->carry_curve.size(), 11U);
  EXPECT_DOUBLE_EQ(fitted->curve->F(), fitted->diagnostics.forward);
  EXPECT_NEAR(fitted->curve->F(), 100.0 * std::exp((0.04 - 0.015) * 0.5), 1.0e-12);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &row = fitted->rows[i];
    ASSERT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
    EXPECT_NEAR(row.fitted_iv, 0.25, 5.0e-4);
    if (row.admitted) { EXPECT_NEAR(row.market_iv, 0.25, 5.0e-6); }
    const auto expected = andersen_lake(100.0, quotes[i].strike, 0.4, row.fitted_iv,
                                       0.05, *quotes[i].continuous_yield * 1.25, quotes[i].side);
    ASSERT_TRUE(expected);
    EXPECT_DOUBLE_EQ(row.fitted_price, *expected);
    const auto queried = evaluate_quote_slice(*fitted, quotes[i].strike, quotes[i].side);
    ASSERT_TRUE(queried);
    EXPECT_DOUBLE_EQ(queried->price, row.fitted_price);
  }
  // Midway in log strike interpolates the two supplied carries; outside the
  // knot range serving keeps the nearest edge carry, including at new strikes.
  for (double K : {60.0, std::sqrt(95.0 * 100.0), 150.0}) {
    const double yield = 0.015 + 0.08 * std::log(std::clamp(K, 75.0, 125.0) / 100.0);
    const auto queried = evaluate_quote_slice(*fitted, K, Side::Call);
    ASSERT_TRUE(queried) << queried.error().to_string();
    const auto expected = andersen_lake(100.0, K, 0.4, queried->volatility, 0.05,
                                       yield * 1.25, Side::Call);
    ASSERT_TRUE(expected);
    EXPECT_NEAR(queried->price, *expected, 1.0e-10);
  }
}

TEST(QuoteSlice, MissingYieldOverridesPreserveHistoricalArithmeticBitForBit) {
  const QuoteSliceMarket market{100.0, 0.4, 0.5, 0.04, 0.015, 0.0};
  auto quotes = flat_quotes(market);
  auto baseline = fit_quote_slice(market, quotes);
  ASSERT_TRUE(baseline);
  ASSERT_EQ(baseline->fit_status, QuoteSliceFitStatus::Success);
  EXPECT_TRUE(baseline->carry_curve.empty());
  // Explicitly providing the reference yield, and a mixed supplied/missing
  // schedule, must both take the exact legacy arithmetic path.
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if ((i / 2U) % 2U == 0U) { quotes[i].continuous_yield = market.continuous_yield; }
  }
  auto explicit_carry = fit_quote_slice(market, quotes);
  ASSERT_TRUE(explicit_carry);
  ASSERT_EQ(explicit_carry->fit_status, QuoteSliceFitStatus::Success);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &a = baseline->rows[i];
    const auto &b = explicit_carry->rows[i];
    EXPECT_EQ(a.quote_status, b.quote_status);
    EXPECT_EQ(a.evaluation_status, b.evaluation_status);
    EXPECT_DOUBLE_EQ(a.fitted_price, b.fitted_price);
    EXPECT_DOUBLE_EQ(a.fitted_iv, b.fitted_iv);
    if (a.admitted) { EXPECT_DOUBLE_EQ(a.market_iv, b.market_iv); }
  }
}

TEST(QuoteSlice, ConflictingAndNonFiniteStrikeCarryIsRefusedWithoutAveraging) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  auto quotes = flat_quotes(market);
  // An absent override resolves to the reference yield and participates in
  // the conflict check just like an explicit carry at that strike.
  quotes.front().continuous_yield = 0.03;
  const auto conflict = fit_quote_slice(market, quotes);
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(conflict.error().message().find("conflicting"), std::string::npos);
  quotes.front().continuous_yield = std::numeric_limits<double>::quiet_NaN();
  const auto invalid = fit_quote_slice(market, quotes);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST(QuoteSlice, DatedCashInversionAndServingPreserveStrikeSpecificCarry) {
  const QuoteSliceMarket market{100.0, 0.4, 0.5, 0.04, 0.01, 0.0};
  const std::vector<CashDividend> calendar_events{{0.2, 1.8}};
  const std::vector<CashDividend> model_events{{0.16, 1.8}};
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::DatedCash;
  config.curve.kind = VolCurveKind::LinearVariance;
  config.side_policy = QuoteSliceSidePolicy::All;
  config.min_quotes = 5;
  config.discrete_div_steps = 101;
  std::vector<QuoteSliceQuote> quotes;
  for (int strike = 85; strike <= 115; strike += 5) {
    const double K = static_cast<double>(strike);
    const double yield = 0.01 + 0.04 * std::log(K / 100.0);
    const Side side = strike <= 100 ? Side::Put : Side::Call;
    const auto price = american_discrete_div_price(100.0, K, 0.4, 0.25, 0.05,
                                                  yield * 1.25, side, model_events, 101);
    ASSERT_TRUE(price);
    quotes.push_back({K, side, *price - 0.001, *price + 0.001, yield});
  }
  auto fitted = fit_quote_slice(market, quotes, config, calendar_events);
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &row = fitted->rows[i];
    ASSERT_TRUE(row.admitted);
    EXPECT_NEAR(row.market_iv, 0.25, 2.0e-6);
    const auto expected = american_discrete_div_price(100.0, quotes[i].strike, 0.4,
        row.fitted_iv, 0.05, *quotes[i].continuous_yield * 1.25, quotes[i].side, model_events, 101);
    ASSERT_TRUE(expected);
    EXPECT_DOUBLE_EQ(row.fitted_price, *expected);
  }
  const double K = std::sqrt(100.0 * 105.0);
  const double yield = 0.01 + 0.04 * std::log(K / 100.0);
  const auto queried = evaluate_quote_slice(*fitted, K, Side::Call);
  ASSERT_TRUE(queried);
  const auto expected = american_discrete_div_price(100.0, K, 0.4, queried->volatility,
      0.05, yield * 1.25, Side::Call, model_events, 101);
  ASSERT_TRUE(expected);
  EXPECT_NEAR(queried->price, *expected, 1.0e-10);
}

TEST(QuoteSlice, BidlessAndLockedMarketsCannotBecomePointTargetsButRemainPriced) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  auto quotes = flat_quotes(market);
  auto baseline = fit_quote_slice(market, quotes);
  ASSERT_TRUE(baseline);
  ASSERT_EQ(baseline->fit_status, QuoteSliceFitStatus::Success);
  const auto original_count = quotes.size();
  quotes.push_back({140.0, Side::Call, 0.0, 75.0});
  quotes.push_back({60.0, Side::Put, 0.0, 75.0});
  quotes.push_back({103.0, Side::Call, 20.0, 20.0});
  auto fitted = fit_quote_slice(market, quotes);
  ASSERT_TRUE(fitted);
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success) << fitted->fit_error;
  EXPECT_EQ(fitted->diagnostics.iv_attempted, baseline->diagnostics.iv_attempted);
  EXPECT_EQ(fitted->diagnostics.quotes_admitted, baseline->diagnostics.quotes_admitted);
  for (std::size_t i = 0; i < original_count; ++i) {
    EXPECT_DOUBLE_EQ(fitted->rows[i].fitted_iv, baseline->rows[i].fitted_iv);
    EXPECT_DOUBLE_EQ(fitted->rows[i].fitted_price, baseline->rows[i].fitted_price);
  }
  for (std::size_t i = original_count; i < quotes.size(); ++i) {
    const auto &row = fitted->rows[i];
    EXPECT_FALSE(row.admitted);
    EXPECT_EQ(row.weight_w, 0.0);
    EXPECT_FALSE(row.weight_capped);
    EXPECT_TRUE(std::isnan(row.market_iv));
    EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
    EXPECT_NEAR(row.fitted_iv, 0.25, 5.0e-4);
  }
  EXPECT_EQ(fitted->rows[original_count].quote_status, QuoteSliceQuoteStatus::BidlessQuote);
  EXPECT_EQ(fitted->rows[original_count + 1].quote_status, QuoteSliceQuoteStatus::BidlessQuote);
  EXPECT_EQ(fitted->rows.back().quote_status, QuoteSliceQuoteStatus::InvalidQuote);

  for (auto &quote : quotes) { quote.bid = 0.0; }
  auto no_points = fit_quote_slice(market, quotes);
  ASSERT_TRUE(no_points);
  EXPECT_EQ(no_points->fit_status, QuoteSliceFitStatus::InsufficientQuotes);
  EXPECT_EQ(no_points->diagnostics.iv_attempted, 0U);
  EXPECT_EQ(no_points->diagnostics.prices_produced, 0U);
}

TEST(QuoteSlice, ConfigurableQualityGatesReportTheRejectedMeasurement) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  auto quotes = flat_quotes(market);
  const auto value = andersen_lake(100.0, 104.0, 0.5, 0.25, 0.04, 0.01, Side::Call);
  ASSERT_TRUE(value);
  quotes.push_back({104.0, Side::Call, 0.6 * *value, 1.4 * *value});
  QuoteSliceConfig config;
  auto relative_rejection = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(relative_rejection);
  ASSERT_EQ(relative_rejection->fit_status, QuoteSliceFitStatus::Success);
  EXPECT_EQ(relative_rejection->rows.back().quote_status, QuoteSliceQuoteStatus::WideSpreadToMid);
  EXPECT_TRUE(std::isnan(relative_rejection->rows.back().market_iv));
  EXPECT_EQ(relative_rejection->rows.back().evaluation_status, QuoteSliceEvaluationStatus::Success);

  config.quality.max_spread_to_mid = 0.0;
  auto vol_rejection = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(vol_rejection);
  EXPECT_EQ(vol_rejection->rows.back().quote_status, QuoteSliceQuoteStatus::WideSpreadVol);
  EXPECT_GT(vol_rejection->rows.back().spread_vol, config.quality.max_spread_vol);
  EXPECT_NEAR(vol_rejection->rows.back().market_iv, 0.25, 5.0e-6);

  config.quality.max_spread_vol = 0.0;
  auto admitted = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(admitted);
  EXPECT_TRUE(admitted->rows.back().admitted);
  EXPECT_EQ(admitted->rows.back().evaluation_status, QuoteSliceEvaluationStatus::Success);

  const double noise = admitted->rows.back().spread_vol;
  config.quality.min_vega_weight = 2.0 / (noise * noise);
  auto sensitivity_rejection = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(sensitivity_rejection);
  EXPECT_EQ(sensitivity_rejection->rows.back().quote_status, QuoteSliceQuoteStatus::LowVegaWeight);
  EXPECT_EQ(sensitivity_rejection->rows.back().weight_w, 0.0);

  config.quality.min_vega_weight = 0.0;
  config.quality.min_bid = quotes.back().bid + 0.001;
  auto bid_rejection = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(bid_rejection);
  EXPECT_EQ(bid_rejection->rows.back().quote_status, QuoteSliceQuoteStatus::BelowMinimumBid);
}

TEST(QuoteSlice, ObservationWeightsUseLocalUncertaintyAndTheConfiguredFiniteCap) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  auto quotes = flat_quotes(market);
  // This extra point is accurate but substantially less precise than the other
  // quotes, so its uncapped inverse uncertainty must remain smaller.
  const auto value = andersen_lake(100.0, 104.0, 0.5, 0.25, 0.04, 0.03, Side::Call);
  ASSERT_TRUE(value);
  quotes.push_back({104.0, Side::Call, *value - 0.2, *value + 0.2, 0.03});
  QuoteSliceConfig config;
  config.quality.max_weight = 1.0e6;
  auto fitted = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(fitted);
  ASSERT_EQ(fitted->fit_status, QuoteSliceFitStatus::Success);
  EXPECT_GT(fitted->diagnostics.weights_capped, 0U);
  EXPECT_LT(fitted->diagnostics.weights_capped, fitted->diagnostics.quotes_admitted);
  std::size_t capped_count = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &row = fitted->rows[i];
    if (!row.admitted) { continue; }
    const double yield = quotes[i].continuous_yield.value_or(market.continuous_yield);
    const double forward = 100.0 * std::exp((0.04 - yield) * 0.5);
    const double vega = black76_value_and_vega(forward, quotes[i].strike, 0.5,
        row.market_iv, std::exp(-0.02), quotes[i].side).vega;
    const double spread = std::max(quotes[i].ask - quotes[i].bid, config.spread_floor);
    const double root = vega / spread / row.market_iv; // 2*sigma*T = sigma
    EXPECT_NEAR(row.spread_vol, spread / vega, 1.0e-14);
    EXPECT_NEAR(row.weight_w, std::min(root * root, config.quality.max_weight), 1.0e-7);
    EXPECT_LE(row.weight_w, config.quality.max_weight);
    if (row.weight_capped) { ++capped_count; }
  }
  EXPECT_EQ(capped_count, fitted->diagnostics.weights_capped);
  EXPECT_FALSE(fitted->rows.back().weight_capped);
  EXPECT_GT(fitted->rows.back().weight_w, 0.0);
}

TEST(QuoteSlice, InvalidQualityPoliciesFailAtTheSharedBoundary) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  QuoteSliceConfig config;
  for (double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity()}) {
    for (double QuoteSliceQualityConfig::*field : {&QuoteSliceQualityConfig::min_bid,
         &QuoteSliceQualityConfig::max_spread_to_mid, &QuoteSliceQualityConfig::max_spread_vol,
         &QuoteSliceQualityConfig::min_vega_weight, &QuoteSliceQualityConfig::max_weight}) {
      config.quality = {};
      config.quality.*field = invalid;
      const auto rejected = fit_quote_slice(market, {}, config);
      ASSERT_FALSE(rejected);
      EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    }
  }
  config.quality = {};
  config.quality.max_weight = 0.0;
  EXPECT_FALSE(fit_quote_slice(market, {}, config));
}

} // namespace
} // namespace atx::vol
