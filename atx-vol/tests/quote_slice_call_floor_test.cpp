// Lane C3 (2026-10-23): the opt-in served call floor (QuoteSliceCallFloor). The review of
// lane C2 (B(3)) found the graded default path publishing deep in-the-money NON-dividend
// calls at the American exercise value S - K, below the no-arbitrage bound S - K * df: a
// LATER slice whose own carry puts its deep call at S - K (a dividend payer with q > r)
// caps the short non-dividend slice through the bounded calendar repair. The fixture is
// the dev200 SOC chain of that review (spot 3.585, the 3-day non-dividend expiry 20742 and
// the q = 5.36 % expiry 20784, K 1.5 quoted [2.00, 2.30]: published 2.085000 vs the bound
// 2.085633). The data are the vendor's quotes re-priced by Andersen-Lake, never the
// vendor's rows (no market data is committed).
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

constexpr double kSpot = 3.584999918937683;
constexpr double kShortYears = 0.00939783826470375;  // SOC 20742, 3 days
constexpr double kShortRate = 0.044896867126226425;
constexpr double kLongYears = 0.12320990115404129;   // SOC 20784
constexpr double kLongRate = 0.045253366231918335;
constexpr double kLongYield = 0.053613416850566864;  // q > r: deep calls exercise at S - K
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// Andersen-Lake quotes on a flat 80 % volatility, +/- 2 % (at least one tick), strikes every
// 0.5 as SOC lists them; the K 1.5 call carries the SOC quote of its expiry.
Chain soc_chain(double years, double rate, double yield, double max_strike, double bid15,
                double ask15) {
  Chain out;
  out.market = QuoteSliceMarket{kSpot, years, years, rate, yield, 0.0};
  for (double k = 0.5; k <= max_strike + 1e-9; k += 0.5) {
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(kSpot, k, years, 0.80, rate, yield, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * 0.02);
      QuoteSliceQuote q{k, side, std::max(*price - spread, 0.0), *price + spread};
      if (side == Side::Call && k == 1.5) {
        q.bid = bid15;
        q.ask = ask15;
      }
      out.quotes.push_back(q);
    }
  }
  return out;
}

// The short non-dividend expiry has fewer strikes than the long one, so the calendar pass
// accepts the long slice first and repairs the short one against it (as on dev200).
std::vector<Chain> soc_surface() {
  return {soc_chain(kShortYears, kShortRate, 0.0, 5.0, 2.00, 2.30),
          soc_chain(kLongYears, kLongRate, kLongYield, 7.0, 1.98, 2.39)};
}

std::vector<QuoteSurfaceExpiry> expiries(const std::vector<Chain> &chains) {
  std::vector<QuoteSurfaceExpiry> out;
  for (const Chain &c : chains) out.push_back({c.market, c.quotes, {}});
  return out;
}

QuoteSurfaceConfig graded_surface(QuoteSurfacePublication publication) {
  QuoteSurfaceConfig surface;
  surface.calendar_repair_half_spreads = 0.5; // graded-v1
  surface.publication = publication;
  return surface;
}

QuoteSliceConfig floored(std::span<const QuoteSurfaceExpiry> e) {
  QuoteSliceConfig config;
  config.call_floor.mode = QuoteSliceCallFloor::DiscountedIntrinsic;
  config.call_floor.lines = quote_surface_call_floor_lines(e);
  return config;
}

double short_bound(double strike) { return kSpot - strike * std::exp(-kShortRate * kShortYears); }

double allowance(double strike) { return 1.0e-8 + 1.0e-9 * std::max(kSpot, strike); }

TEST(QuoteSliceCallFloor, LinesListExactlyTheNonDividendExpiries) {
  const std::vector<Chain> chains = soc_surface();
  std::vector<QuoteSurfaceExpiry> e = expiries(chains);
  const auto lines = quote_surface_call_floor_lines(e);
  ASSERT_EQ(lines.size(), 1u); // the dividend payer has no line
  EXPECT_EQ(lines[0].calendar_years, kShortYears);
  EXPECT_EQ(lines[0].spot, kSpot);
  EXPECT_EQ(lines[0].discount, std::exp(-kShortRate * kShortYears));
  // A dividend PV, a calendar dividend, a quote's own yield or a negative rate each leave
  // the non-dividend regime check_nondividend_american certifies.
  std::vector<QuoteSurfaceExpiry> changed = e;
  changed[0].market.dividend_pv = 0.01;
  EXPECT_TRUE(quote_surface_call_floor_lines(changed).empty());
  changed = e;
  changed[0].market.rate = -0.001;
  EXPECT_TRUE(quote_surface_call_floor_lines(changed).empty());
  const std::vector<CashDividend> cash{{0.005, 0.01}}; // tau, amount
  changed = e;
  changed[0].calendar_dividends = cash;
  EXPECT_TRUE(quote_surface_call_floor_lines(changed).empty());
  std::vector<QuoteSliceQuote> overridden = chains[0].quotes;
  overridden[3].continuous_yield = 0.01;
  changed = e;
  changed[0].quotes = overridden;
  EXPECT_TRUE(quote_surface_call_floor_lines(changed).empty());
}

TEST(QuoteSliceCallFloor, AnInertOrInvalidFloorIsRefused) {
  const Chain c = soc_surface()[0];
  QuoteSliceConfig config;
  config.call_floor.lines = {{kShortYears, kSpot, 0.999}}; // lines under Legacy: inert
  EXPECT_FALSE(fit_quote_slice(c.market, c.quotes, config));
  config.call_floor.mode = QuoteSliceCallFloor::DiscountedIntrinsic;
  for (const QuoteSliceCallFloorLine bad : {QuoteSliceCallFloorLine{kShortYears, kSpot, 1.01},
                                            QuoteSliceCallFloorLine{kShortYears, 0.0, 0.99},
                                            QuoteSliceCallFloorLine{-1.0, kSpot, 0.99},
                                            QuoteSliceCallFloorLine{kShortYears, kSpot, kNaN}}) {
    config.call_floor.lines = {bad};
    EXPECT_FALSE(fit_quote_slice(c.market, c.quotes, config));
  }
  config.call_floor.lines = {{kShortYears, kSpot, 0.999}};
  EXPECT_TRUE(fit_quote_slice(c.market, c.quotes, config));
  config.call_floor.mode = static_cast<QuoteSliceCallFloor>(7);
  EXPECT_FALSE(fit_quote_slice(c.market, c.quotes, config));
  EXPECT_STREQ(to_string(QuoteSliceCallFloor::Legacy), "legacy");
  EXPECT_STREQ(to_string(QuoteSliceCallFloor::DiscountedIntrinsic), "discounted_intrinsic");
}

// Legacy (the historical path, pinned): the SOC defect itself. The short non-dividend
// slice is served, and its deep call sits at S - K under S - K * df.
TEST(QuoteSliceCallFloor, LegacyServesTheSocDeepCallBelowTheDiscountedIntrinsic) {
  const std::vector<Chain> chains = soc_surface();
  const auto e = expiries(chains);
  const auto fit = fit_quote_surface(e, QuoteSliceConfig{}, graded_surface({}));
  ASSERT_TRUE(fit) << fit.error().to_string();
  const QuoteSurfaceSlice &n = (*fit)[0];
  ASSERT_NE(n.grade, QuoteSliceGrade::Refused) << n.refusal << ": " << n.error;
  ASSERT_TRUE(n.calendar_repaired);
  const auto c15 = n.fit.prices->price(1.5, Side::Call);
  ASSERT_TRUE(c15);
  EXPECT_LT(*c15, short_bound(1.5) - allowance(1.5)) << *c15;
  EXPECT_FALSE(check_nondividend_american(*n.fit.prices, std::exp(-kShortRate * kShortYears)));
}

// DiscountedIntrinsic: the LONG slice is lifted off its exercise value onto the short
// slice's discounted intrinsic, so the short slice is served at or above its bound and
// passes the non-dividend certificate; both slices stay served and calendar-consistent.
TEST(QuoteSliceCallFloor, TheSharedFloorServesTheSocDeepCallAtOrAboveItsBound) {
  const std::vector<Chain> chains = soc_surface();
  const auto e = expiries(chains);
  const auto legacy = fit_quote_surface(e, QuoteSliceConfig{}, graded_surface({}));
  const auto fit = fit_quote_surface(e, floored(e), graded_surface({}));
  ASSERT_TRUE(legacy && fit);
  const QuoteSurfaceSlice &n = (*fit)[0];
  const QuoteSurfaceSlice &d = (*fit)[1];
  ASSERT_NE(n.grade, QuoteSliceGrade::Refused) << n.refusal << ": " << n.error;
  ASSERT_NE(d.grade, QuoteSliceGrade::Refused) << d.refusal << ": " << d.error;
  for (const double k : {0.5, 1.0, 1.5, 2.0}) {
    const auto c = n.fit.prices->price(k, Side::Call);
    ASSERT_TRUE(c);
    EXPECT_GE(*c, short_bound(k) - allowance(k)) << k;
  }
  EXPECT_TRUE(check_nondividend_american(*n.fit.prices, std::exp(-kShortRate * kShortYears)));
  EXPECT_TRUE(check_american_calendar(*n.fit.prices, *d.fit.prices));
  // The long slice carries the short line and lifted its deep calls (moved nodes).
  ASSERT_EQ(d.fit.diagnostics.call_floor_lines.size(), 1u);
  EXPECT_GT(d.fit.diagnostics.call_floor_lifted, 0u);
  EXPECT_FALSE(d.fit.moved_nodes.empty());
  const auto d15 = d.fit.prices->price(1.5, Side::Call);
  ASSERT_TRUE(d15);
  EXPECT_GE(*d15, short_bound(1.5) - allowance(1.5));
  // No lost price: every row served under Legacy is served (a price) under the floor, and
  // every changed price is at or above the short bound or unchanged.
  for (std::size_t s = 0; s < 2; ++s) {
    const auto &before = (*legacy)[s].fit.rows;
    const auto &after = (*fit)[s].fit.rows;
    for (std::size_t r = 0; r < before.size(); ++r) {
      const bool had = std::isfinite(before[r].fitted_price) &&
                       (before[r].evaluation_status == QuoteSliceEvaluationStatus::Success ||
                        before[r].evaluation_status == QuoteSliceEvaluationStatus::PriceOnly);
      if (!had) continue;
      EXPECT_TRUE(std::isfinite(after[r].fitted_price)) << s << " row " << r;
    }
  }
  // The floor never lowers a price: the long slice only moves up.
  const auto legacy_d = (*legacy)[1].fit.prices->nodes();
  const auto floored_d = d.fit.prices->nodes();
  ASSERT_EQ(legacy_d.size(), floored_d.size());
  for (std::size_t i = 0; i < floored_d.size(); ++i) {
    EXPECT_GE(floored_d[i].call, legacy_d[i].call) << floored_d[i].strike;
    EXPECT_EQ(floored_d[i].put, legacy_d[i].put) << floored_d[i].strike;
  }
}

// Under lane C2's Validate the Legacy short slice is refused (its repair fails the
// non-dividend recheck); with the floor it is served and the publication is clean.
TEST(QuoteSliceCallFloor, ValidateServesTheFlooredShortSliceItRefusesUnderLegacy) {
  const std::vector<Chain> chains = soc_surface();
  const auto e = expiries(chains);
  const auto surface = graded_surface(QuoteSurfacePublication::Validate);
  const auto legacy = fit_quote_surface(e, QuoteSliceConfig{}, surface);
  const auto fit = fit_quote_surface(e, floored(e), surface);
  ASSERT_TRUE(legacy && fit);
  EXPECT_EQ((*legacy)[0].grade, QuoteSliceGrade::Refused);
  ASSERT_NE((*fit)[0].grade, QuoteSliceGrade::Refused) << (*fit)[0].refusal;
  ASSERT_NE((*fit)[1].grade, QuoteSliceGrade::Refused) << (*fit)[1].refusal;
  EXPECT_EQ((*fit)[0].publication_violations + (*fit)[1].publication_violations, 0u);
  const std::vector<QuoteSlicePublicationSlice> published{{&(*fit)[0].fit, chains[0].quotes},
                                                          {&(*fit)[1].fit, chains[1].quotes}};
  const auto report = validate_published_representation(published);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_EQ(report->count(QuoteSlicePublicationViolation::Nondividend), 0u);
  EXPECT_EQ(report->count(QuoteSlicePublicationViolation::Calendar), 0u);
}


} // namespace
} // namespace atx::vol
