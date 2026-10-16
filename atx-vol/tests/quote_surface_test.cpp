#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// American quotes on a known smile sigma(k) = atm + skew * ln(K/S), priced with
// the same Andersen-Lake engine the fitter inverts. `half_spread` is relative
// to the price with an absolute floor; `bidless_otm` removes OTM bids beyond
// `bidless_from` log-moneyness (the sparse-chain pattern seen on dev200).
Chain chain(double years, double atm, double skew, double half_spread, int lo = 75,
            int hi = 125, int step = 5, double bidless_from = 10.0) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, years, years, 0.03, 0.01, 0.0};
  for (int strike = lo; strike <= hi; strike += step) {
    const double k = std::log(static_cast<double>(strike) / 100.0);
    const double sigma = atm + skew * k;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(100.0, static_cast<double>(strike), years, sigma, 0.03,
                                       0.01, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * half_spread);
      const bool otm = (side == Side::Call) == (k > 0.0);
      const double bid = otm && std::abs(k) >= bidless_from ? 0.0 : std::max(*price - spread, 0.0);
      out.quotes.push_back({static_cast<double>(strike), side, bid, *price + spread});
    }
  }
  return out;
}

QuoteSliceConfig channel_config() {
  QuoteSliceConfig config;
  config.method = QuoteSliceFitMethod::ChannelSpline;
  return config;
}

std::vector<QuoteSurfaceExpiry> expiries(const std::vector<Chain> &chains) {
  std::vector<QuoteSurfaceExpiry> out;
  for (const auto &c : chains) out.push_back({c.market, c.quotes, {}});
  return out;
}

void expect_calendar_consistent(const std::vector<QuoteSurfaceSlice> &slices) {
  for (std::size_t i = 0; i < slices.size(); ++i) {
    for (std::size_t j = 0; j < slices.size(); ++j) {
      if (slices[i].grade == QuoteSliceGrade::Refused ||
          slices[j].grade == QuoteSliceGrade::Refused ||
          !(slices[i].fit.market.calendar_years < slices[j].fit.market.calendar_years)) {
        continue;
      }
      const auto status = check_american_calendar(*slices[i].fit.prices, *slices[j].fit.prices);
      EXPECT_TRUE(status) << i << "," << j << ": " << status.error().to_string();
    }
  }
}

TEST(QuoteSliceChannel, RecoversFlatVolatilityInsideEveryQuote) {
  const auto c = chain(0.5, 0.25, 0.0, 0.02);
  const auto fit = fit_quote_slice_channel(c.market, c.quotes, QuoteSliceConfig{}, nullptr);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  ASSERT_TRUE(fit->curve);
  EXPECT_EQ(fit->curve->kind(), VolCurveKind::SplineVol);
  const auto &channel = fit->diagnostics.channel;
  EXPECT_TRUE(channel.used);
  EXPECT_EQ(channel.channel_quotes, c.quotes.size());
  EXPECT_EQ(channel.violations, 0U);
  EXPECT_FALSE(channel.term_prior);
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const auto &row = fit->rows[i];
    ASSERT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
    EXPECT_NEAR(row.fitted_iv, 0.25, 2.0e-3) << i;
    EXPECT_GE(row.fitted_price, c.quotes[i].bid - 1.0e-9) << i;
    EXPECT_LE(row.fitted_price, c.quotes[i].ask + 1.0e-9) << i;
  }
}

TEST(QuoteSliceChannel, ServesASparseChainThatPointAdmissionRefuses) {
  // Every OTM leg away from the money is bidless; ITM legs stay two-sided.
  const auto c = chain(0.25, 0.30, -0.10, 0.05, 80, 120, 5, 0.04);
  const auto point = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(point);
  EXPECT_EQ(point->fit_status, QuoteSliceFitStatus::InsufficientQuotes);
  const auto fit = fit_quote_slice(c.market, c.quotes, channel_config());
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  EXPECT_TRUE(fit->diagnostics.channel.used);
  const double atm = fit->curve->iv(std::log(100.0 / fit->diagnostics.forward));
  EXPECT_NEAR(atm, 0.30, 0.02);
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    EXPECT_LE(fit->rows[i].fitted_price, c.quotes[i].ask + 1.0e-6) << i;
  }
  // Point admission provenance is still reported for every row.
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    EXPECT_EQ(fit->rows[i].quote_status, point->rows[i].quote_status) << i;
  }
}

TEST(QuoteSliceChannel, RefusesWhenFewerThanTheMinimumChannelsAreUsable) {
  Chain c = chain(0.5, 0.25, 0.0, 0.02, 100, 100, 5);
  c.quotes.resize(1);
  auto config = channel_config();
  config.channel.min_channel_quotes = 2;
  const auto fit = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(fit);
  EXPECT_EQ(fit->fit_status, QuoteSliceFitStatus::InsufficientQuotes);
  EXPECT_FALSE(fit->prices);
  config.channel.prior_scale = -1.0;
  EXPECT_FALSE(fit_quote_slice(c.market, c.quotes, config));
}

TEST(QuoteSurface, WellQuotedExpiriesAreGradeAAndBitIdenticalToThePointFit) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  const auto surface = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(surface) << surface.error().to_string();
  ASSERT_EQ(surface->size(), chains.size());
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const auto &slice = (*surface)[e];
    EXPECT_EQ(slice.grade, QuoteSliceGrade::A);
    EXPECT_TRUE(slice.refusal.empty());
    EXPECT_FALSE(slice.channel_refit);
    const auto alone = fit_quote_slice(chains[e].market, chains[e].quotes);
    ASSERT_TRUE(alone);
    ASSERT_EQ(alone->fit_status, QuoteSliceFitStatus::Success);
    for (std::size_t i = 0; i < chains[e].quotes.size(); ++i) {
      EXPECT_EQ(slice.fit.rows[i].fitted_price, alone->rows[i].fitted_price);
      EXPECT_EQ(slice.fit.rows[i].fitted_iv, alone->rows[i].fitted_iv);
    }
  }
  expect_calendar_consistent(*surface);
}

TEST(QuoteSurface, SparseExpiryBorrowsLevelAndShapeFromItsNeighbours) {
  // The middle expiry has three wide two-sided strikes near the money only.
  std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.15, 95, 105),
                            chain(1.0, 0.26, -0.06, 0.01)};
  const auto surface = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(surface) << surface.error().to_string();
  const auto &sparse = (*surface)[1];
  EXPECT_EQ(sparse.point_status, QuoteSliceFitStatus::InsufficientQuotes);
  ASSERT_NE(sparse.grade, QuoteSliceGrade::Refused) << sparse.refusal << " " << sparse.error;
  EXPECT_NE(sparse.grade, QuoteSliceGrade::A);
  EXPECT_TRUE(sparse.channel_refit);
  EXPECT_TRUE(sparse.fit.diagnostics.channel.term_prior);
  EXPECT_GT(sparse.fit.diagnostics.channel.prior_strength, 0.0);
  const double atm = sparse.fit.curve->iv(0.0);
  EXPECT_GT(atm, 0.22);
  EXPECT_LT(atm, 0.26);
  for (std::size_t i = 0; i < chains[1].quotes.size(); ++i) {
    EXPECT_GE(sparse.fit.rows[i].fitted_price, chains[1].quotes[i].bid - 1.0e-6) << i;
    EXPECT_LE(sparse.fit.rows[i].fitted_price, chains[1].quotes[i].ask + 1.0e-6) << i;
  }
  EXPECT_EQ((*surface)[0].grade, QuoteSliceGrade::A);
  EXPECT_EQ((*surface)[2].grade, QuoteSliceGrade::A);
  expect_calendar_consistent(*surface);
}

TEST(QuoteSurface, SnapshotWithoutAnyPointFitStillServesFromChannels) {
  std::vector<Chain> chains;
  for (const double years : {0.1, 0.3, 0.7}) {
    chains.push_back(chain(years, 0.35, -0.05, 0.08, 80, 120, 5, 0.04));
  }
  const auto surface = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(surface) << surface.error().to_string();
  std::size_t served = 0;
  for (const auto &slice : *surface) {
    EXPECT_NE(slice.grade, QuoteSliceGrade::A);
    EXPECT_EQ(slice.point_status, QuoteSliceFitStatus::InsufficientQuotes);
    if (slice.grade != QuoteSliceGrade::Refused) {
      ++served;
      EXPECT_TRUE(slice.fit.prices);
      EXPECT_NEAR(slice.fit.curve->iv(0.0), 0.35, 0.03);
    }
  }
  EXPECT_EQ(served, chains.size());
  expect_calendar_consistent(*surface);
}

TEST(QuoteSurface, CalendarCrossingRefusesOnlyTheLowerConfidenceSlice) {
  // The later expiry is quoted far cheaper (tight quotes), so its American
  // prices fall below the earlier expiry's: a genuine calendar crossing.
  const std::vector<Chain> chains{chain(0.25, 0.40, 0.0, 0.005, 70, 130, 5),
                                  chain(0.5, 0.15, 0.0, 0.005, 85, 115, 5)};
  const auto surface = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(surface) << surface.error().to_string();
  const auto &kept = (*surface)[0];
  const auto &dropped = (*surface)[1];
  EXPECT_EQ(kept.grade, QuoteSliceGrade::A);
  EXPECT_EQ(dropped.point_status, QuoteSliceFitStatus::Success);
  EXPECT_EQ(dropped.grade, QuoteSliceGrade::Refused);
  EXPECT_EQ(dropped.refusal, "calendar_arbitrage");
  EXPECT_TRUE(dropped.calendar_retry);
  EXPECT_GT(dropped.calendar_gap, 0.0);
  EXPECT_FALSE(dropped.fit.prices);
  for (const auto &row : dropped.fit.rows) {
    EXPECT_TRUE(std::isnan(row.fitted_price));
    EXPECT_NE(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
  }
}

TEST(QuoteSurface, CalendarRepairInsideTheQuotesAdmitsASmallCrossing) {
  // The later expiry is quoted slightly too cheap: a crossing far smaller than
  // its half-spreads. The repair clamps it above the earlier expiry, re-certifies
  // and demotes the point fit to B; with the repair disabled it is refused.
  const std::vector<Chain> chains{chain(0.25, 0.30, 0.0, 0.01, 70, 130, 5),
                                  chain(0.5, 0.218, 0.0, 0.05, 80, 120, 5)};
  const auto refused = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(refused);
  ASSERT_EQ((*refused)[1].refusal, "calendar_arbitrage");
  QuoteSurfaceConfig surface;
  surface.calendar_repair_half_spreads = 1.0;
  const auto repaired = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, surface);
  ASSERT_TRUE(repaired) << repaired.error().to_string();
  const auto &kept = (*repaired)[0];
  const auto &fixed = (*repaired)[1];
  EXPECT_EQ(kept.grade, QuoteSliceGrade::A);
  EXPECT_EQ(fixed.grade, QuoteSliceGrade::B)
      << fixed.refusal << " " << fixed.error << " gap " << fixed.calendar_gap << " at "
      << fixed.calendar_gap_strike << " required " << fixed.calendar_repair_required;
  EXPECT_TRUE(fixed.calendar_repaired);
  ASSERT_TRUE(fixed.fit.prices);
  expect_calendar_consistent(*repaired);
  const auto point = fit_quote_slice(chains[1].market, chains[1].quotes);
  ASSERT_TRUE(point);
  ASSERT_EQ(point->fit_status, QuoteSliceFitStatus::Success);
  for (std::size_t i = 0; i < chains[1].quotes.size(); ++i) {
    const auto &quote = chains[1].quotes[i];
    const double half_spread = 0.5 * (quote.ask - quote.bid);
    const double moved = fixed.fit.rows[i].fitted_price - point->rows[i].fitted_price;
    EXPECT_GE(moved, -half_spread - 1.0e-9) << i;
    EXPECT_LE(moved, half_spread + 1.0e-9) << i;
  }
}

TEST(QuoteSurface, InvalidExpiryIsRefusedWithoutFailingTheSurface) {
  std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01)};
  chains[1].market.dividend_pv = 1.0; // continuous-yield model cannot take cash PV
  const auto surface = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(surface) << surface.error().to_string();
  EXPECT_EQ((*surface)[0].grade, QuoteSliceGrade::A);
  EXPECT_EQ((*surface)[1].grade, QuoteSliceGrade::Refused);
  EXPECT_EQ((*surface)[1].refusal, "invalid_input");
  EXPECT_FALSE((*surface)[1].error.empty());
}

TEST(QuoteSurface, RejectsDuplicateMaturitiesAndInvalidConfiguration) {
  const std::vector<Chain> same{chain(0.25, 0.22, 0.0, 0.01), chain(0.25, 0.24, 0.0, 0.01)};
  EXPECT_EQ(fit_quote_surface(expiries(same), QuoteSliceConfig{}).error().code(),
            ErrorCode::InvalidArgument);
  const std::vector<Chain> one{chain(0.25, 0.22, 0.0, 0.01)};
  QuoteSurfaceConfig bad;
  bad.calendar_retry_factor = 0.5;
  EXPECT_EQ(fit_quote_surface(expiries(one), QuoteSliceConfig{}, bad).error().code(),
            ErrorCode::InvalidArgument);
  const auto empty = fit_quote_surface({}, QuoteSliceConfig{});
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->empty());
}

TEST(QuoteSurface, GradesFollowOwnMidEvidence) {
  // Same sparse middle expiry; a high B threshold demotes it to C.
  std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.15, 95, 105),
                            chain(1.0, 0.26, -0.06, 0.01)};
  QuoteSurfaceConfig strict;
  strict.grade_b_min_mid_strikes = 50;
  const auto surface = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, strict);
  ASSERT_TRUE(surface);
  EXPECT_EQ((*surface)[1].grade, QuoteSliceGrade::C);
  QuoteSurfaceConfig lenient;
  lenient.grade_b_min_mid_strikes = 1;
  const auto relaxed = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, lenient);
  ASSERT_TRUE(relaxed);
  EXPECT_EQ((*relaxed)[1].grade, QuoteSliceGrade::B);
}

} // namespace
} // namespace atx::vol
