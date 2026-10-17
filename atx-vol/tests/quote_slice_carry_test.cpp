#include "atx/vol/api/fitting/quote_slice.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

// A single-clock American chain priced by Andersen-Lake at the TRUE carry
// `true_yield`, with a mild skew and proportional spreads.
std::vector<QuoteSliceQuote> american_quotes(double spot, double years, double rate,
                                             double true_yield, double forward) {
  std::vector<QuoteSliceQuote> quotes;
  for (double k = 60.0; k <= 160.0 + 1.0e-9; k += 5.0) {
    const double sigma = 0.30 - 0.08 * std::log(k / forward);
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(spot, k, years, sigma, rate, true_yield, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return {};
      }
      const double half = std::max(0.005, 0.01 * *price);
      quotes.push_back({k, side, std::max(*price - half, 1.0e-3), *price + half});
    }
  }
  return quotes;
}

QuoteSliceConfig implied_config() {
  QuoteSliceConfig cfg;
  cfg.carry.mode = QuoteSliceCarryMode::ImpliedForward;
  cfg.carry.implied.al_opts = std::nullopt; // match the generator exactly
  return cfg;
}

void expect_same_fit(const QuoteSliceFit &a, const QuoteSliceFit &b) {
  ASSERT_EQ(a.fit_status, b.fit_status);
  ASSERT_EQ(a.rows.size(), b.rows.size());
  EXPECT_EQ(std::memcmp(&a.diagnostics.forward, &b.diagnostics.forward, sizeof(double)), 0);
  EXPECT_EQ(a.diagnostics.quotes_admitted, b.diagnostics.quotes_admitted);
  EXPECT_EQ(a.carry_curve.size(), b.carry_curve.size());
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    EXPECT_EQ(a.rows[i].quote_status, b.rows[i].quote_status) << i;
    EXPECT_EQ(a.rows[i].evaluation_status, b.rows[i].evaluation_status) << i;
    const double pa = a.rows[i].fitted_price;
    const double pb = b.rows[i].fitted_price;
    EXPECT_TRUE((std::isnan(pa) && std::isnan(pb)) || std::memcmp(&pa, &pb, sizeof(double)) == 0)
        << i;
  }
}

TEST(QuoteSliceCarry, DefaultModeIsSuppliedAndRecordsNoResolution) {
  const QuoteSliceConfig cfg;
  EXPECT_EQ(cfg.carry.mode, QuoteSliceCarryMode::Supplied);
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  const auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  const auto fit = fit_quote_slice(market, quotes, cfg);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->carry_resolution.mode, QuoteSliceCarryMode::Supplied);
  EXPECT_FALSE(fit->carry_resolution.applied);
  EXPECT_STREQ(to_string(QuoteSliceCarryMode::ImpliedForward), "implied_forward");
}

TEST(QuoteSliceCarry, ImpliedForwardReplacesAWrongSuppliedCarry) {
  const double spot = 100.0;
  const double years = 1.5;
  const double rate = 0.045;
  const double true_yield = 0.012;
  const double forward = spot * std::exp((rate - true_yield) * years);
  // Supplied carry is wrong by 150 bp; per-contract overrides repeat it.
  const QuoteSliceMarket market{spot, years, years, rate, 0.027, 0.0};
  auto quotes = american_quotes(spot, years, rate, true_yield, forward);
  for (auto &quote : quotes) {
    quote.continuous_yield = 0.027;
  }
  const auto fit = fit_quote_slice(market, quotes, implied_config());
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &res = fit->carry_resolution;
  ASSERT_TRUE(res.applied) << res.reason;
  EXPECT_EQ(res.fallback, QuoteSliceCarryFallback::None);
  EXPECT_EQ(res.status, ImpliedForwardStatus::Ok);
  EXPECT_NEAR(res.implied_forward / forward - 1.0, 0.0, 5.0e-6);
  EXPECT_NEAR(res.implied_yield, true_yield, 5.0e-6);
  EXPECT_NEAR(res.supplied_forward, spot * std::exp((rate - 0.027) * years), 1.0e-9);
  EXPECT_NEAR(fit->market.continuous_yield, true_yield, 5.0e-6);
  EXPECT_NEAR(fit->diagnostics.forward / forward - 1.0, 0.0, 5.0e-6);
  // One carry per expiry: supplied per-contract yields are dropped.
  EXPECT_TRUE(fit->carry_curve.empty());
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;

  // The implied carry reprices the market better than the wrong supplied one.
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_TRUE(supplied);
  ASSERT_EQ(supplied->fit_status, QuoteSliceFitStatus::Success) << supplied->fit_error;
  double err_implied = 0.0;
  double err_supplied = 0.0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const double mid = 0.5 * (quotes[i].bid + quotes[i].ask);
    if (fit->rows[i].evaluation_status == QuoteSliceEvaluationStatus::Success &&
        supplied->rows[i].evaluation_status == QuoteSliceEvaluationStatus::Success) {
      err_implied += std::fabs(fit->rows[i].fitted_price - mid);
      err_supplied += std::fabs(supplied->rows[i].fitted_price - mid);
    }
  }
  EXPECT_LT(err_implied, 0.5 * err_supplied);
}

TEST(QuoteSliceCarry, FundingPresentValueKeepsCashAndMatchesTheImpliedForward) {
  const double spot = 100.0;
  const double years = 1.0;
  const double rate = 0.04;
  const double pv = 2.0;
  // Market truth: funding-PV forward with zero residual yield.
  const double forward = spot * std::exp(rate * years) - pv * std::exp(rate * years);
  const double q_eff = rate - std::log(forward / spot) / years;
  std::vector<QuoteSliceQuote> quotes;
  for (double k = 60.0; k <= 150.0 + 1.0e-9; k += 5.0) {
    const double sigma = 0.28 - 0.06 * std::log(k / forward);
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(spot, k, years, sigma, rate, q_eff, side);
      ASSERT_TRUE(price);
      const double half = std::max(0.005, 0.01 * *price);
      quotes.push_back({k, side, std::max(*price - half, 1.0e-3), *price + half});
    }
  }
  const QuoteSliceMarket market{spot, years, years, rate, 0.02, pv};
  auto cfg = implied_config();
  cfg.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
  const auto fit = fit_quote_slice(market, quotes, cfg);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_TRUE(fit->carry_resolution.applied) << fit->carry_resolution.reason;
  EXPECT_DOUBLE_EQ(fit->market.dividend_pv, pv);
  EXPECT_NEAR(fit->market.continuous_yield, 0.0, 5.0e-6);
  EXPECT_NEAR(fit->diagnostics.forward / forward - 1.0, 0.0, 5.0e-6);
  EXPECT_NEAR(fit->diagnostics.effective_yield, q_eff, 5.0e-6);
}

TEST(QuoteSliceCarry, RefusalFallsBackBitIdenticallyToSuppliedCarry) {
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  for (auto &quote : quotes) {
    quote.continuous_yield = 0.01;
  }
  auto cfg = implied_config();
  cfg.carry.implied.min_pairs = 32; // more than the chain can supply near the money
  cfg.carry.implied.max_pairs = 32;
  const auto refused = fit_quote_slice(market, quotes, cfg);
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_TRUE(refused && supplied);
  EXPECT_FALSE(refused->carry_resolution.applied);
  EXPECT_EQ(refused->carry_resolution.fallback, QuoteSliceCarryFallback::EstimateRefused);
  EXPECT_EQ(refused->carry_resolution.status, ImpliedForwardStatus::InsufficientPairs);
  EXPECT_EQ(refused->carry_resolution.mode, QuoteSliceCarryMode::ImpliedForward);
  EXPECT_DOUBLE_EQ(refused->market.continuous_yield, 0.01);
  expect_same_fit(*refused, *supplied);
}

TEST(QuoteSliceCarry, DatedCashModelIsNotImpliedAndFallsBack) {
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  const auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  auto cfg = implied_config();
  cfg.dividend_model = QuoteSliceDividendModel::DatedCash;
  cfg.discrete_div_steps = 51;
  const auto fit = fit_quote_slice(market, quotes, cfg);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_FALSE(fit->carry_resolution.applied);
  EXPECT_EQ(fit->carry_resolution.fallback, QuoteSliceCarryFallback::DatedCash);
  EXPECT_STREQ(to_string(fit->carry_resolution.fallback), "dated_cash");
  EXPECT_DOUBLE_EQ(fit->market.continuous_yield, 0.01);
  auto supplied_cfg = cfg;
  supplied_cfg.carry.mode = QuoteSliceCarryMode::Supplied;
  const auto supplied = fit_quote_slice(market, quotes, supplied_cfg);
  ASSERT_TRUE(supplied);
  expect_same_fit(*fit, *supplied);
}

TEST(QuoteSliceCarry, MalformedMarketStillReportsTheSuppliedError) {
  const QuoteSliceMarket market{-1.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  const std::vector<QuoteSliceQuote> quotes{{100.0, Side::Call, 1.0, 1.1}};
  const auto implied = fit_quote_slice(market, quotes, implied_config());
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_FALSE(implied);
  ASSERT_FALSE(supplied);
  EXPECT_EQ(implied.error().code(), supplied.error().code());
}

TEST(QuoteSliceCarry, MalformedQuoteIsRejectedBeforeCarryResolution) {
  // The implied-carry path replaces every contract yield, so a non-finite
  // supplied yield must be refused before carry resolution, exactly as the
  // supplied path refuses it.
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  quotes[3].continuous_yield = std::numeric_limits<double>::quiet_NaN();
  const auto implied = fit_quote_slice(market, quotes, implied_config());
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_FALSE(supplied);
  ASSERT_FALSE(implied);
  EXPECT_EQ(implied.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(implied.error().code(), supplied.error().code());
  // A malformed shared configuration is refused the same way.
  auto config = implied_config();
  config.min_quotes = 1;
  quotes[3].continuous_yield = 0.01;
  const auto bad_config = fit_quote_slice(market, quotes, config);
  ASSERT_FALSE(bad_config);
  EXPECT_EQ(bad_config.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace atx::vol
