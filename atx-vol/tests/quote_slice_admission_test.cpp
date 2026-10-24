// Lane C3 (2026-10-23): admission contracts of the quote slice that the shape-class matrix
// relies on. (1) A one-sided quote is an inequality, never a midpoint: a zero-bid (ask-only)
// quote is not a point observation and carries no channel midpoint, only its ask as an
// upper bound. (A bid-only quote - ask <= 0 - is refused by the cohort adapter as
// invalid_option_quote and cannot be expressed in QuoteSliceQuote, whose ask must be finite
// and positive; see docs/curve-admission-matrix.md.) (2) Fits are snapshot-only: a surface's
// result does not depend on any surface fitted before it in the same process (the per-thread
// de-Am memo and the rung memo carry no cross-snapshot state).
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// Andersen-Lake quotes on sigma(k) = atm + skew * ln(K/S) at spot 100, +/- 2 % (at least a
// tick), strikes 75..125 (quote_surface_test.cpp's chain fixture).
Chain smile_chain(double years, double rate, double yield, double atm, double skew) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, years, years, rate, yield, 0.0};
  for (int strike = 75; strike <= 125; strike += 5) {
    const double k = static_cast<double>(strike);
    const double sigma = atm + skew * std::log(k / 100.0);
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(100.0, k, years, sigma, rate, yield, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * 0.02);
      out.quotes.push_back({k, side, std::max(*price - spread, 0.0), *price + spread});
    }
  }
  return out;
}

// The chain plus one far out-of-the-money put quoted [0, ask]: its fabricated midpoint
// ask / 2 would sit far above the smile's price there.
Chain with_bidless_wing(Chain c, double ask) {
  c.quotes.push_back({60.0, Side::Put, 0.0, ask});
  return c;
}

std::size_t two_sided(const Chain &c) {
  const auto bid = [](const QuoteSliceQuote &q) { return q.bid > 0.0; };
  return static_cast<std::size_t>(std::count_if(c.quotes.begin(), c.quotes.end(), bid));
}

TEST(QuoteSliceAdmission, AZeroBidQuoteIsNeverAPointObservation) {
  const Chain base = smile_chain(0.5, 0.03, 0.01, 0.25, -0.10);
  const Chain wing = with_bidless_wing(base, 1.0);
  const auto without = fit_quote_slice(base.market, base.quotes, QuoteSliceConfig{});
  const auto with = fit_quote_slice(wing.market, wing.quotes, QuoteSliceConfig{});
  ASSERT_TRUE(without && with);
  ASSERT_EQ(with->fit_status, QuoteSliceFitStatus::Success) << with->fit_error;
  const QuoteSliceRow &row = with->rows.back();
  EXPECT_EQ(row.quote_status, QuoteSliceQuoteStatus::BidlessQuote);
  EXPECT_FALSE(row.admitted);
  EXPECT_TRUE(std::isnan(row.market_iv));
  EXPECT_EQ(with->diagnostics.quotes_admitted, without->diagnostics.quotes_admitted);
  // The admitted observations are the same, so the curve - and every two-sided row's
  // served price and volatility - is bit for bit the fit without the wing quote.
  for (std::size_t i = 0; i < base.quotes.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(with->rows[i].fitted_price),
              std::bit_cast<std::uint64_t>(without->rows[i].fitted_price)) << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(with->rows[i].fitted_iv),
              std::bit_cast<std::uint64_t>(without->rows[i].fitted_iv)) << i;
  }
  // The wing is served from the curve, inside its one-sided quote [0, ask] and nowhere near
  // the fabricated midpoint 0.5.
  ASSERT_TRUE(std::isfinite(row.fitted_price));
  EXPECT_LT(row.fitted_price, 0.05);
}

TEST(QuoteSliceAdmission, AZeroBidQuoteIsOnlyAnUpperBoundInTheChannel) {
  const Chain wing = with_bidless_wing(smile_chain(0.5, 0.03, 0.01, 0.25, -0.10), 1.0);
  const auto fit = fit_quote_slice_channel(wing.market, wing.quotes, QuoteSliceConfig{}, nullptr);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  const auto &channel = fit->diagnostics.channel;
  // Every contract is a channel; only the two-sided ones carry a midpoint.
  EXPECT_EQ(channel.channel_quotes, wing.quotes.size());
  EXPECT_EQ(channel.mid_quotes, two_sided(wing));
  const QuoteSliceRow &row = fit->rows.back();
  ASSERT_TRUE(std::isfinite(row.fitted_price));
  EXPECT_LT(row.fitted_price, 0.05); // not pulled toward ask / 2 = 0.5
}

TEST(QuoteSliceAdmission, ABindingZeroBidAskCapsTheChannelFit) {
  // The same wing quoted below the smile's own price: the ask is an upper bound the channel
  // fit must respect (to within one tick of channel slack), not a midpoint.
  const Chain base = smile_chain(0.5, 0.03, 0.01, 0.25, -0.10);
  const double sigma = 0.25 - 0.10 * std::log(0.6);
  const auto model = andersen_lake(100.0, 60.0, 0.5, sigma, 0.03, 0.01, Side::Put);
  ASSERT_TRUE(model);
  const double ask = 0.5 * *model;
  const Chain wing = with_bidless_wing(base, ask);
  const auto fit = fit_quote_slice_channel(wing.market, wing.quotes, QuoteSliceConfig{}, nullptr);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  EXPECT_EQ(fit->diagnostics.channel.mid_quotes, two_sided(wing));
  const QuoteSliceRow &row = fit->rows.back();
  ASSERT_TRUE(std::isfinite(row.fitted_price));
  EXPECT_LT(row.fitted_price, *model); // pulled under the smile toward its cap
}

TEST(QuoteSliceAdmission, ASurfaceFitDependsOnlyOnItsOwnSnapshot) {
  const std::vector<Chain> b{smile_chain(0.25, 0.03, 0.01, 0.22, -0.08),
                             smile_chain(0.5, 0.03, 0.01, 0.24, -0.10)};
  const std::vector<Chain> a{smile_chain(0.25, 0.05, 0.0, 0.40, -0.20),
                             smile_chain(0.5, 0.05, 0.0, 0.45, -0.25)};
  const auto expiries = [](const std::vector<Chain> &chains) {
    std::vector<QuoteSurfaceExpiry> out;
    for (const Chain &c : chains) out.push_back({c.market, c.quotes, {}});
    return out;
  };
  QuoteSliceConfig config;
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel; // the per-thread de-Am memo
  config.channel.deam_cache = true;
  QuoteSurfaceConfig surface;
  surface.calendar_repair_half_spreads = 0.5;
  surface.rung_memo = true;
  const auto eb = expiries(b);
  const auto ea = expiries(a);
  const auto first = fit_quote_surface(eb, config, surface);
  const auto other = fit_quote_surface(ea, config, surface);
  const auto again = fit_quote_surface(eb, config, surface);
  ASSERT_TRUE(first && other && again);
  for (std::size_t s = 0; s < b.size(); ++s) {
    const auto &x = (*first)[s];
    const auto &y = (*again)[s];
    ASSERT_EQ(x.grade, y.grade) << s;
    ASSERT_EQ(x.fit.rows.size(), y.fit.rows.size());
    for (std::size_t r = 0; r < x.fit.rows.size(); ++r) {
      EXPECT_EQ(x.fit.rows[r].evaluation_status, y.fit.rows[r].evaluation_status) << s << "," << r;
      EXPECT_EQ(std::bit_cast<std::uint64_t>(x.fit.rows[r].fitted_price),
                std::bit_cast<std::uint64_t>(y.fit.rows[r].fitted_price)) << s << "," << r;
      EXPECT_EQ(std::bit_cast<std::uint64_t>(x.fit.rows[r].fitted_iv),
                std::bit_cast<std::uint64_t>(y.fit.rows[r].fitted_iv)) << s << "," << r;
    }
  }
}

} // namespace
} // namespace atx::vol
