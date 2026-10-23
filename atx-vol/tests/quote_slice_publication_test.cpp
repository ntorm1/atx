// Lane C2 (2026-10-22): the published representation of a quote slice is what API
// callers evaluate, so every repair or refusal must leave it certified as a whole.
// These tests pin the review-L3q minors at the representation level: m5 (a refused
// row's off-market value must not stay reachable through the query API), m3 (every
// repaired result of a non-dividend slice must pass the non-dividend certificate it was
// first served under) and m2 (the in-box market repair must never push a row that sat
// inside its own quote out of it).
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// American quotes on sigma(k) = atm + skew * ln(K/S) at spot 100, priced with the
// Andersen-Lake engine the fitter inverts; +/- `half_spread` relative spreads (at least
// 0.01). quote_surface_test.cpp's `chain` fixture with an explicit carry.
Chain smile_chain(double years, double rate, double yield, double atm, double skew,
                  double half_spread) {
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
      const double spread = std::max(0.01, *price * half_spread);
      out.quotes.push_back({k, side, std::max(*price - spread, 0.0), *price + spread});
    }
  }
  return out;
}

std::vector<QuoteSurfaceExpiry> expiries(const std::vector<Chain> &chains) {
  std::vector<QuoteSurfaceExpiry> out;
  for (const Chain &c : chains) out.push_back({c.market, c.quotes, {}});
  return out;
}

std::size_t row_index(const Chain &c, double strike, Side side) {
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (c.quotes[i].strike == strike && c.quotes[i].side == side) return i;
  }
  ADD_FAILURE() << "no quote at " << strike;
  return 0;
}

// The in-the-money call at 90 re-quoted `width` wide and `spreads` widths BELOW its
// model price (quote_surface_test.cpp off_market_call): the point fit serves it about
// `spreads` spreads above its ask while every other row sits inside its own quote.
Chain off_market_call(double spreads, double width) {
  Chain c = smile_chain(0.25, 0.03, 0.01, 0.20, -0.10, 0.02);
  const double sigma = 0.20 - 0.10 * std::log(0.9);
  const auto model = andersen_lake(100.0, 90.0, 0.25, sigma, 0.03, 0.01, Side::Call);
  EXPECT_TRUE(model);
  for (QuoteSliceQuote &q : c.quotes) {
    if (q.side == Side::Call && q.strike == 90.0) {
      q.ask = *model - spreads * width;
      q.bid = q.ask - width;
    }
  }
  return c;
}

QuoteSliceConfig market_distance_config(QuoteSliceMarketDistance mode, double bound = 1.0) {
  QuoteSliceConfig config;
  config.repair.market_distance = mode;
  config.repair.market_distance_spreads = bound;
  return config;
}

// ── m5: a refused row is refused by the API too ─────────────────────────────────

TEST(QuoteSlicePublication, AMarketDistanceRefusalIsATypedRefusalAtTheApiNotANumber) {
  const std::vector<Chain> one{off_market_call(3.0, 0.04)};
  const std::size_t r = row_index(one[0], 90.0, Side::Call);
  const auto gate = fit_quote_surface(expiries(one),
                                      market_distance_config(QuoteSliceMarketDistance::Gate));
  ASSERT_TRUE(gate) << gate.error().to_string();
  const QuoteSurfaceSlice &s = (*gate)[0];
  ASSERT_NE(s.grade, QuoteSliceGrade::Refused) << s.refusal << ": " << s.error;
  ASSERT_EQ(s.fit.rows[r].evaluation_status, QuoteSliceEvaluationStatus::MarketDistanceFailure);
  // The row is refused, so the API must refuse the same contract: no off-market number.
  const auto value = evaluate_quote_slice(s.fit, 90.0, Side::Call);
  EXPECT_FALSE(value) << "evaluate_quote_slice served the refused row's price " << value->price;
  const auto query = query_quote_slice(s.fit, 90.0, Side::Call);
  ASSERT_TRUE(query) << query.error().to_string();
  EXPECT_EQ(query->status, QuoteSliceEvaluationStatus::MarketDistanceFailure);
  EXPECT_TRUE(std::isnan(query->price));
  EXPECT_TRUE(std::isnan(query->volatility));
  // Interpolation next to the refused node reads its off-market value: withheld too.
  for (const double k : {87.5, 92.5}) {
    const auto near = query_quote_slice(s.fit, k, Side::Call);
    ASSERT_TRUE(near) << near.error().to_string();
    EXPECT_EQ(near->status, QuoteSliceEvaluationStatus::MarketDistanceFailure) << k;
  }
  // The neighbouring nodes and the other side at 90 are their own certified values.
  for (const double k : {85.0, 95.0}) {
    const auto served = query_quote_slice(s.fit, k, Side::Call);
    ASSERT_TRUE(served) << served.error().to_string();
    EXPECT_EQ(served->status, QuoteSliceEvaluationStatus::Success) << k;
  }
  const auto put = query_quote_slice(s.fit, 90.0, Side::Put);
  ASSERT_TRUE(put) << put.error().to_string();
  EXPECT_EQ(put->status, QuoteSliceEvaluationStatus::Success);
}

// ── m3: every repaired non-dividend result passes the non-dividend certificate ──────

// A non-dividend chain (zero yield, no cash, positive rate): its served slice passes
// check_nondividend_american at fit time (certify_and_serve).
Chain nondividend_chain() { return smile_chain(1.0, 0.05, 0.0, 0.20, -0.05, 0.02); }

// The fit's own certified nodes with every put raised by `shift`, as an accepted
// earlier slice: a calendar floor no put of the fit may sit below. A constant shift
// keeps the puts convex with unchanged slopes, and for shift below K (1 - df) - the
// early-exercise premium the certificate's call-minus-put bound still holds; the
// non-dividend bound A(K) <= C(K / df) - S + K does not.
AmericanPriceSlice raised_puts(const QuoteSliceFit &fit, double shift) {
  std::vector<AmericanPriceNode> nodes(fit.prices->nodes().begin(), fit.prices->nodes().end());
  for (AmericanPriceNode &n : nodes) n.put += shift;
  const AmericanPriceBounds wide{fit.market.spot, 0.5, 1.0, 0.0, fit.market.spot};
  auto slice = AmericanPriceSlice::create(nodes, wide);
  EXPECT_TRUE(slice) << slice.error().to_string();
  return std::move(*slice);
}

TEST(QuoteSlicePublication, ACalendarRepairNeverServesANondividendViolation) {
  const Chain c = nondividend_chain();
  auto fit = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  const double df = fit->diagnostics.discount;
  ASSERT_TRUE(check_nondividend_american(*fit->prices, df));
  const AmericanPriceSlice earlier = raised_puts(*fit, 1.0);
  const std::vector<const AmericanPriceSlice *> floors{&earlier};
  const std::vector<AmericanPriceNode> before(fit->prices->nodes().begin(),
                                              fit->prices->nodes().end());
  const bool repaired = repair_quote_slice_calendar(*fit, c.quotes, floors, {}, 1.0e6);
  if (repaired) {
    // A served repair must still be a non-dividend certificate.
    const auto status = check_nondividend_american(*fit->prices, df);
    EXPECT_TRUE(status) << "the repair served " << status.error().to_string();
  } else {
    const auto after = fit->prices->nodes();
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_EQ(after[i].put, before[i].put) << i;
  }
}

TEST(QuoteSlicePublication, AnInBoxMarketRepairNeverServesANondividendViolation) {
  const Chain c = nondividend_chain();
  auto fit = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  const double df = fit->diagnostics.discount;
  // Every put quoted one unit above its served price: the in-box repair lifts the whole
  // put curve into those quotes (a constant shift, convex and inside every box).
  std::vector<QuoteSliceQuote> quotes = c.quotes;
  for (QuoteSliceQuote &q : quotes) {
    if (q.side != Side::Put) continue;
    const auto served = fit->prices->price(q.strike, Side::Put);
    ASSERT_TRUE(served);
    q.bid = *served + 1.0 - 0.01;
    q.ask = *served + 1.0 + 0.01;
  }
  QuoteSliceRepairConfig config;
  config.market_distance = QuoteSliceMarketDistance::Repair;
  QuoteSliceRepairRecord record;
  const bool repaired = repair_quote_slice_market(*fit, quotes, {}, {}, config, &record);
  if (repaired) {
    const auto status = check_nondividend_american(*fit->prices, df);
    EXPECT_TRUE(status) << "the repair served " << status.error().to_string();
  } else {
    EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::NotCertified) << to_string(record.outcome);
  }
}

// ── m2: the band projection keeps in-quote rows in their quotes ────────────────────

TEST(QuoteSlicePublication, TheMarketRepairBandNeverPushesAnInQuoteRowOutOfItsQuote) {
  const Chain c = smile_chain(0.25, 0.03, 0.01, 0.20, -0.10, 0.02);
  auto fit = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  const auto price = [&](double k) { return *fit->prices->price(k, Side::Put); };
  // Puts at 85 and 95 quoted 0.02 wide around their served prices; the put at 90 quoted
  // above their chord. No convex curve fits inside the quotes (90 would need its
  // neighbours above their asks), but one fits inside the one-spread band - by lifting
  // the in-quote neighbours above their asks.
  const double chord = 0.5 * (price(85.0) + price(95.0));
  std::vector<QuoteSliceQuote> quotes = c.quotes;
  for (QuoteSliceQuote &q : quotes) {
    if (q.side != Side::Put) continue;
    if (q.strike == 85.0 || q.strike == 95.0) {
      q.bid = price(q.strike) - 0.01;
      q.ask = price(q.strike) + 0.01;
    } else if (q.strike == 90.0) {
      q.bid = chord + 0.03;
      q.ask = chord + 0.04;
    }
  }
  std::vector<double> before(quotes.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) before[i] = fit->rows[i].fitted_price;
  QuoteSliceRepairConfig config;
  config.market_distance = QuoteSliceMarketDistance::Repair;
  QuoteSliceRepairRecord record;
  const bool repaired = repair_quote_slice_market(*fit, quotes, {}, {}, config, &record);
  std::size_t left = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const QuoteSliceQuote &q = quotes[i];
    const bool was_inside = before[i] >= q.bid && before[i] <= q.ask;
    const double after = fit->rows[i].fitted_price;
    if (was_inside && (after < q.bid - 1.0e-12 || after > q.ask + 1.0e-12)) {
      ++left;
      ADD_FAILURE() << "row " << i << " (K " << q.strike << ") left its quote: " << before[i]
                    << " -> " << after << " outside [" << q.bid << ", " << q.ask << "]";
    }
  }
  EXPECT_EQ(left, 0u);
  if (!repaired) {
    EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::LeftChannel) << to_string(record.outcome);
  }
}

} // namespace
} // namespace atx::vol
