// Lane C2 (2026-10-22): the published representation of a quote slice is what API
// callers evaluate, so every repair or refusal must leave it certified as a whole.
// These tests pin the review-L3q minors at the representation level: m5 (a refused
// row's off-market value must not stay reachable through the query API), m3 (every
// repaired result of a non-dividend slice must pass the non-dividend certificate it was
// first served under) and m2 (the in-box market repair must never push a row that sat
// inside its own quote out of it).
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "fitting/publication_seam.hpp" // ScopedPublicationInjection (review m1 test seam)
#include "fitting/counters.hpp"

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
  // The historical rule (Skip, fit_quote_surface's default path) serves exactly that
  // violation: the fixture exercises the recheck, and Recheck is what refused it.
  auto historical = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(historical) << historical.error().to_string();
  ASSERT_TRUE(repair_quote_slice_calendar(*historical, c.quotes, floors, {}, 1.0e6, nullptr,
                                          QuoteSliceNondividendRecheck::Skip));
  EXPECT_FALSE(check_nondividend_american(*historical->prices, df));
  EXPECT_FALSE(repaired);
  EXPECT_FALSE(repair_quote_slice_calendar(*historical, c.quotes, floors, {}, 1.0e6, nullptr,
                                           static_cast<QuoteSliceNondividendRecheck>(7)));
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
  EXPECT_FALSE(repaired);
  // Skip (fit_quote_surface without Validate; lane C2 fix round 1, review m4) is the
  // historical rule: the in-box repair is applied and serves exactly the violation Recheck
  // refused. Parity evidence for the gating: dev200b's box h0 + market-distance repair arm
  // hashes to 2e31e087... on the C0 base binary and on lane C2 with it.
  auto historical = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(historical) << historical.error().to_string();
  QuoteSliceRepairRecord skipped;
  ASSERT_TRUE(repair_quote_slice_market(*historical, quotes, {}, {}, config, &skipped,
                                        QuoteSliceNondividendRecheck::Skip))
      << to_string(skipped.outcome);
  EXPECT_FALSE(check_nondividend_american(*historical->prices, df));
  EXPECT_FALSE(repair_quote_slice_market(*historical, quotes, {}, {}, config, nullptr,
                                         static_cast<QuoteSliceNondividendRecheck>(7)));
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

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

void expect_same_bits(const std::vector<double> &a, const std::vector<double> &b,
                      const char *what) {
  ASSERT_EQ(a.size(), b.size()) << what;
  for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(bits(a[i]), bits(b[i])) << what << " " << i;
}

// ── Bounded joint projection: typed outcomes, rollback (deliverable 3) ────────────

// american_price_slice_test.cpp's joint fixture: two expiries of one call side on 90 /
// 100 / 110; the earlier sits 0.10 above the later at the first two strikes and each box
// lets its slice move only 0.06 toward the other, so only a joint move removes the
// crossing.
struct JointPair {
  std::vector<double> strikes{90.0, 100.0, 110.0};
  std::vector<double> near{2.0, 1.2, 0.6};
  std::vector<double> far{1.9, 1.1, 0.9};
  std::vector<double> near_lo{1.94, 1.14, 0.54};
  std::vector<double> near_hi{2.0, 1.2, 0.6};
  std::vector<double> far_lo{1.9, 1.1, 0.9};
  std::vector<double> far_hi{1.96, 1.16, 0.96};
  [[nodiscard]] std::vector<JointPriceSide> sides() {
    return {{strikes, near, near_lo, near_hi, -1.0, 0.0, 100.0},
            {strikes, far, far_lo, far_hi, -1.0, 0.0, 100.0}};
  }
};

TEST(QuoteSlicePublication, AFeasibleJointProjectionSettlesExactlyAsTheHistoricalOne) {
  JointPair bounded;
  JointPair historical;
  const JointProjectionResult result = project_calendar_joint_bounded(bounded.sides());
  ASSERT_TRUE(project_calendar_joint(historical.sides()));
  EXPECT_EQ(result.outcome, JointProjectionOutcome::Feasible) << to_string(result.outcome);
  EXPECT_GT(result.sweeps, 1u);
  EXPECT_LE(result.sweeps, JointProjectionBudget{}.max_sweeps);
  EXPECT_EQ(result.conflict.kind, JointConflictKind::None);
  expect_same_bits(bounded.near, historical.near, "near");
  expect_same_bits(bounded.far, historical.far, "far");
  for (std::size_t i = 0; i < 3; ++i) EXPECT_GE(bounded.far[i] - bounded.near[i], -1.0e-9) << i;
}

TEST(QuoteSlicePublication, ASpentSweepBudgetIsNonConvergedAndLeavesThePricesUnchanged) {
  JointPair pair;
  const JointPair before;
  const JointProjectionResult result =
      project_calendar_joint_bounded(pair.sides(), JointProjectionBudget{1, 1.0e-10});
  EXPECT_EQ(result.outcome, JointProjectionOutcome::NonConverged) << to_string(result.outcome);
  EXPECT_EQ(result.sweeps, 1u);
  EXPECT_EQ(result.conflict.kind, JointConflictKind::None);
  expect_same_bits(pair.near, before.near, "near");
  expect_same_bits(pair.far, before.far, "far");
}

TEST(QuoteSlicePublication, AnEmptyBoxIsInfeasibleDataNamingItsNode) {
  JointPair pair;
  pair.far_lo[2] = 0.97; // above far_hi[2] = 0.96
  const JointPair before;
  const JointProjectionResult result = project_calendar_joint_bounded(pair.sides());
  EXPECT_EQ(result.outcome, JointProjectionOutcome::InfeasibleData) << to_string(result.outcome);
  EXPECT_EQ(result.sweeps, 0u); // the certificate refused before any sweep
  EXPECT_EQ(result.conflict.kind, JointConflictKind::EmptyBox) << to_string(result.conflict.kind);
  EXPECT_EQ(result.conflict.slice, 1u);
  EXPECT_EQ(result.conflict.node, 2u);
  EXPECT_EQ(result.conflict.strike, 110.0);
  EXPECT_NEAR(result.conflict.excess, 0.01, 1.0e-12);
  expect_same_bits(pair.near, before.near, "near");
  expect_same_bits(pair.far, before.far, "far");
}

TEST(QuoteSlicePublication, BoxesThatAdmitNoConvexCurveAreInfeasibleData) {
  // One call side whose middle box lies wholly above the chord of its neighbours' caps
  // (1.3 at 100): every convex curve under the caps passes 100 at or below 1.3.
  const std::vector<double> strikes{90.0, 100.0, 110.0};
  std::vector<double> prices{2.0, 1.5, 0.6};
  const std::vector<double> start = prices;
  const std::vector<double> lo{1.9, 1.45, 0.5};
  const std::vector<double> hi{2.0, 1.5, 0.6};
  const std::vector<JointPriceSide> one{{strikes, prices, lo, hi, -1.0, 0.0, 100.0}};
  const JointProjectionResult result = project_calendar_joint_bounded(one);
  EXPECT_EQ(result.outcome, JointProjectionOutcome::InfeasibleData) << to_string(result.outcome);
  EXPECT_EQ(result.conflict.kind, JointConflictKind::NoConvexCurve)
      << to_string(result.conflict.kind);
  EXPECT_EQ(result.conflict.slice, 0u);
  EXPECT_EQ(result.conflict.node, 1u);
  EXPECT_EQ(result.conflict.strike, 100.0);
  EXPECT_NEAR(result.conflict.excess, 0.15, 1.0e-6);
  expect_same_bits(prices, start, "prices");
}

TEST(QuoteSlicePublication, CalendarBoxesThatCrossAreInfeasibleDataNamingTheKnot) {
  // The later slice held at its prices: at 90 the earlier one cannot go below 1.94 and
  // the later one cannot go above 1.90 (the historical projection fails here too).
  JointPair pair;
  pair.far_hi = pair.far_lo;
  const JointPair before;
  const JointProjectionResult result = project_calendar_joint_bounded(pair.sides());
  EXPECT_EQ(result.outcome, JointProjectionOutcome::InfeasibleData) << to_string(result.outcome);
  EXPECT_EQ(result.conflict.kind, JointConflictKind::CalendarBox)
      << to_string(result.conflict.kind);
  EXPECT_EQ(result.conflict.slice, 0u);
  EXPECT_EQ(result.conflict.other, 1u);
  EXPECT_EQ(result.conflict.node, 0u);
  EXPECT_EQ(result.conflict.strike, 90.0);
  EXPECT_NEAR(result.conflict.excess, 0.04, 1.0e-6);
  expect_same_bits(pair.near, before.near, "near");
  expect_same_bits(pair.far, before.far, "far");
}

TEST(QuoteSlicePublication, AMalformedSliceOrBudgetIsInvalidInputAndChangesNothing) {
  JointPair pair;
  const JointPair before;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const JointProjectionBudget budget :
       {JointProjectionBudget{0, 1.0e-10}, JointProjectionBudget{10, 0.0},
        JointProjectionBudget{10, nan}}) {
    EXPECT_EQ(project_calendar_joint_bounded(pair.sides(), budget).outcome,
              JointProjectionOutcome::InvalidInput);
  }
  std::vector<double> short_prices{1.0, 0.5};
  const std::vector<JointPriceSide> bad{
      {pair.strikes, short_prices, pair.near_lo, pair.near_hi, -1.0, 0.0, 100.0}};
  EXPECT_EQ(project_calendar_joint_bounded(bad).outcome, JointProjectionOutcome::InvalidInput);
  expect_same_bits(pair.near, before.near, "near");
  expect_same_bits(pair.far, before.far, "far");
  EXPECT_STREQ(to_string(JointProjectionOutcome::Feasible), "feasible");
  EXPECT_STREQ(to_string(JointProjectionOutcome::InfeasibleData), "infeasible_data");
  EXPECT_STREQ(to_string(JointProjectionOutcome::NonConverged), "nonconverged");
  EXPECT_STREQ(to_string(JointProjectionOutcome::InvalidInput), "invalid_input");
  EXPECT_STREQ(to_string(JointConflictKind::EmptyBox), "empty_box");
  EXPECT_STREQ(to_string(JointConflictKind::NoConvexCurve), "no_convex_curve");
  EXPECT_STREQ(to_string(JointConflictKind::CalendarBox), "calendar_box");
  EXPECT_STREQ(to_string(QuoteSliceRepairOutcome::NonConverged), "nonconverged");
}

QuoteSliceFit fitted(const Chain &c) {
  auto fit = fit_quote_slice(c.market, c.quotes);
  if (!fit || fit->fit_status != QuoteSliceFitStatus::Success) {
    ADD_FAILURE() << "fixture chain (T " << c.market.calendar_years << ") did not fit: "
                  << (fit ? fit->fit_error : fit.error().to_string());
    return QuoteSliceFit{};
  }
  return std::move(*fit);
}

// `fit`'s certified nodes with both sides raised by `lift` (a constant shift keeps
// convexity, slopes and the call-minus-put spread), on its own bounds.
AmericanPriceSlice lifted(const QuoteSliceFit &fit, double lift) {
  std::vector<AmericanPriceNode> nodes;
  for (const AmericanPriceNode &n : fit.prices->nodes()) {
    nodes.push_back({n.strike, n.call + lift, n.put + lift});
  }
  auto slice = AmericanPriceSlice::create(nodes, fit.prices->bounds());
  if (!slice) {
    ADD_FAILURE() << slice.error().to_string();
    return *fit.prices;
  }
  return std::move(*slice);
}

// `quotes` re-quoted `half_width` around `fit`'s certified price of each contract.
std::vector<QuoteSliceQuote> quotes_around(const QuoteSliceFit &fit,
                                           std::span<const QuoteSliceQuote> quotes,
                                           double half_width) {
  std::vector<QuoteSliceQuote> out(quotes.begin(), quotes.end());
  for (QuoteSliceQuote &q : out) {
    const double p = fit.prices->price(q.strike, q.side).value_or(0.0);
    q.bid = std::max(p - half_width, 0.0);
    q.ask = p + half_width;
  }
  return out;
}

// Two fitted expiries of one smile (0.25 y and 0.5 y), calendar-consistent, with the
// earlier one's certified prices then lifted by `lift`: it crosses the later one wherever
// their difference is below the lift (the deep in-the-money puts, at their exercise
// value in both).
struct JointCase {
  Chain near_chain = smile_chain(0.25, 0.03, 0.01, 0.20, -0.10, 0.02);
  Chain far_chain = smile_chain(0.5, 0.03, 0.01, 0.20, -0.10, 0.02);
  QuoteSliceFit near;
  QuoteSliceFit far;
};

void make_joint_case(JointCase &c, double lift) {
  c.near = fitted(c.near_chain);
  c.far = fitted(c.far_chain);
  ASSERT_TRUE(c.near.prices && c.far.prices);
  ASSERT_TRUE(check_american_calendar(*c.near.prices, *c.far.prices));
  c.near.prices = lifted(c.near, lift);
  ASSERT_FALSE(check_american_calendar(*c.near.prices, *c.far.prices)); // the lift crosses
}

std::vector<AmericanPriceNode> nodes_of(const QuoteSliceFit &fit) {
  return {fit.prices->nodes().begin(), fit.prices->nodes().end()};
}

void expect_same_nodes(const QuoteSliceFit &fit, const std::vector<AmericanPriceNode> &nodes,
                       const char *what) {
  const auto now = fit.prices->nodes();
  ASSERT_EQ(now.size(), nodes.size()) << what;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(bits(now[i].call), bits(nodes[i].call)) << what << " call " << i;
    EXPECT_EQ(bits(now[i].put), bits(nodes[i].put)) << what << " put " << i;
  }
}

TEST(QuoteSlicePublication, AJointCalendarRepairThatSettlesCommitsTheMovedMembers) {
  JointCase c;
  make_joint_case(c, 0.3);
  const std::array<QuoteSliceJointMember, 2> members{
      {{&c.near, c.near_chain.quotes}, {&c.far, c.far_chain.quotes}}};
  QuoteSliceRepairRecord record;
  std::vector<std::size_t> moved;
  const bool repaired =
      repair_quote_slice_calendar_joint(members, {}, QuoteSliceRepairConfig{}, &moved, &record);
  ASSERT_TRUE(repaired) << to_string(record.outcome);
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(record.sweeps, 0u);
  EXPECT_EQ(record.conflict.kind, JointConflictKind::None);
  ASSERT_EQ(moved.size(), 2u);
  EXPECT_GT(moved[0], 0u);
  EXPECT_TRUE(check_american_calendar(*c.near.prices, *c.far.prices));
}

TEST(QuoteSlicePublication, JointTerminalFloorPolicySurvivesEveryMemberCommit) {
  JointCase c;
  make_joint_case(c, 0.3);
  const auto near_quotes = quotes_around(c.near, c.near_chain.quotes, 1.0);
  const auto far_quotes = quotes_around(c.far, c.far_chain.quotes, 1.0);
  const std::array<QuoteSliceJointMember, 2> members{
      {{&c.near, near_quotes}, {&c.far, far_quotes}}};
  ASSERT_FALSE(c.near.terminal_floor_recovery || c.far.terminal_floor_recovery);
  QuoteSliceRepairConfig config;
  config.certificate = QuoteSliceCertificate::Box;
  config.terminal_floor_recovery = true;
  QuoteSliceRepairRecord record;
  ASSERT_TRUE(repair_quote_slice_calendar_joint(members, {}, config, nullptr, &record))
      << to_string(record.outcome);
  EXPECT_TRUE(c.near.terminal_floor_recovery);
  EXPECT_TRUE(c.far.terminal_floor_recovery);
  EXPECT_TRUE(check_american_calendar(*c.near.prices, *c.far.prices));
  for (const auto &member : members) {
    const auto &fit = *member.fit;
    const double prepaid = fit.market.spot *
                           std::exp(-fit.market.continuous_yield * fit.market.calendar_years);
    for (const auto &node : fit.prices->nodes()) {
      EXPECT_GE(node.call, prepaid - fit.diagnostics.discount * node.strike);
      EXPECT_GE(node.put, fit.diagnostics.discount * node.strike - prepaid);
    }
  }
}

TEST(QuoteSlicePublication, AJointCalendarRepairOutOfBudgetIsNonConvergedAndRollsBack) {
  JointCase c;
  make_joint_case(c, 0.3);
  const auto near_nodes = nodes_of(c.near);
  const auto far_nodes = nodes_of(c.far);
  const std::vector<QuoteSliceRow> near_rows = c.near.rows;
  const std::array<QuoteSliceJointMember, 2> members{
      {{&c.near, c.near_chain.quotes}, {&c.far, c.far_chain.quotes}}};
  QuoteSliceRepairConfig config;
  config.joint_max_sweeps = 1;
  QuoteSliceRepairRecord record;
  EXPECT_FALSE(repair_quote_slice_calendar_joint(members, {}, config, nullptr, &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::NonConverged) << to_string(record.outcome);
  EXPECT_EQ(record.sweeps, 1u);
  EXPECT_EQ(record.conflict.kind, JointConflictKind::None);
  expect_same_nodes(c.near, near_nodes, "near");
  expect_same_nodes(c.far, far_nodes, "far");
  ASSERT_EQ(c.near.rows.size(), near_rows.size());
  for (std::size_t i = 0; i < near_rows.size(); ++i) {
    EXPECT_EQ(bits(c.near.rows[i].fitted_price), bits(near_rows[i].fitted_price)) << i;
    EXPECT_EQ(c.near.rows[i].evaluation_status, near_rows[i].evaluation_status) << i;
  }
  EXPECT_TRUE(c.near.moved_nodes.empty());
  config.joint_max_sweeps = 0; // validated at the shared boundary
  EXPECT_FALSE(validate_quote_slice_repair(config));
}

TEST(QuoteSlicePublication, AJointCalendarRepairOnCrossingQuotesIsInfeasibleAndRollsBack) {
  // Both expiries quoted 0.001 around their (lifted / fitted) prices: wherever the lift
  // exceeds their difference by more than the two half-widths no prices inside the quotes
  // are calendar-consistent - an infeasibility certificate, not a solver budget.
  JointCase c;
  make_joint_case(c, 0.3);
  const auto near_quotes = quotes_around(c.near, c.near_chain.quotes, 0.001);
  const auto far_quotes = quotes_around(c.far, c.far_chain.quotes, 0.001);
  const auto near_nodes = nodes_of(c.near);
  const auto far_nodes = nodes_of(c.far);
  const std::array<QuoteSliceJointMember, 2> members{
      {{&c.near, near_quotes}, {&c.far, far_quotes}}};
  QuoteSliceRepairRecord record;
  EXPECT_FALSE(
      repair_quote_slice_calendar_joint(members, {}, QuoteSliceRepairConfig{}, nullptr, &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Infeasible) << to_string(record.outcome);
  EXPECT_EQ(record.conflict.kind, JointConflictKind::CalendarBox)
      << to_string(record.conflict.kind);
  EXPECT_GT(record.conflict.excess, 0.0);
  expect_same_nodes(c.near, near_nodes, "near");
  expect_same_nodes(c.far, far_nodes, "far");
  EXPECT_TRUE(c.near.moved_nodes.empty());
}

// Review m2 (lane C2 fix round 1): the joint rung's own empty-box exits name their conflict
// like the projection's certificates. The conflict is EmptyBox, with the member (in
// maturity order), node, strike and excess, and no member changes. The fixed earlier
// neighbour carries the near member's prices lifted by one unit.
// - Exit 1 (prepare): a fixed later neighbour caps the near member at its own prices,
//   below that floor.
// - Exit 2 (box): with no cap, the floor lies above the near member's quote box.
TEST(QuoteSlicePublication, AJointRepairsEmptyClampedBoxIsAnEmptyBoxConflict) {
  for (const bool capped : {true, false}) {
    JointCase c;
    make_joint_case(c, 0.3);
    const AmericanPriceSlice floor = lifted(c.near, 1.0);
    const AmericanPriceSlice cap = *c.near.prices;
    std::vector<QuoteSliceCalendarNeighbour> fixed{{&floor, 0.1}};
    if (capped) fixed.push_back({&cap, 0.3});
    const auto near_quotes = quotes_around(c.near, c.near_chain.quotes, 0.001);
    const auto near_nodes = nodes_of(c.near);
    const auto far_nodes = nodes_of(c.far);
    const std::array<QuoteSliceJointMember, 2> members{
        {{&c.near, near_quotes}, {&c.far, c.far_chain.quotes}}};
    QuoteSliceRepairRecord record;
    const char *what = capped ? "prepare" : "box";
    EXPECT_FALSE(repair_quote_slice_calendar_joint(members, fixed, QuoteSliceRepairConfig{},
                                                   nullptr, &record))
        << what;
    EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Infeasible) << what;
    EXPECT_EQ(record.conflict.kind, JointConflictKind::EmptyBox)
        << what << ": " << to_string(record.conflict.kind);
    EXPECT_EQ(record.conflict.slice, 0u) << what;
    EXPECT_EQ(record.conflict.other, 0u) << what;
    EXPECT_EQ(record.conflict.node, 0u) << what;
    EXPECT_EQ(record.conflict.strike, near_nodes.front().strike) << what;
    // floor - cap = 1 (prepare); floor - (price + 0.001) = 0.999 (box).
    EXPECT_NEAR(record.conflict.excess, capped ? 1.0 : 0.999, 1.0e-9) << what;
    EXPECT_EQ(record.sweeps, 0u) << what;
    expect_same_nodes(c.near, near_nodes, what);
    expect_same_nodes(c.far, far_nodes, what);
  }
}

// The channel / box calendar repair (lane C2 fix round 1, review m4). The fit is quoted
// 2 wide around its own prices, so the box certificate's quote boxes can absorb an
// earlier slice whose puts sit one unit above the fit's: the floor under which the
// bounded-move repair serves a non-dividend violation.
// - Recheck (the default; fit_quote_surface under Validate) refuses the result.
// - Skip (the historical rule; fit_quote_surface without Validate) serves it.
// Parity evidence for the gating: dev200b's box h0 + market-distance repair arm hashes to
// 2e31e087... on the C0 base binary and on lane C2 with it.
TEST(QuoteSlicePublication, AnInChannelRepairRechecksTheNondividendCertificateOnlyWhenAsked) {
  const Chain c = nondividend_chain();
  QuoteSliceRepairConfig config;
  config.certificate = QuoteSliceCertificate::Box;
  QuoteSliceFit fit = fitted(c);
  ASSERT_TRUE(fit.prices);
  const double df = fit.diagnostics.discount;
  ASSERT_TRUE(check_nondividend_american(*fit.prices, df));
  const auto quotes = quotes_around(fit, c.quotes, 2.0);
  const AmericanPriceSlice earlier = raised_puts(fit, 1.0);
  const std::vector<const AmericanPriceSlice *> floors{&earlier};
  const auto before = nodes_of(fit);
  QuoteSliceRepairRecord record;
  EXPECT_FALSE(repair_quote_slice_calendar_in_channel(fit, quotes, floors, {}, config, &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::NotCertified) << to_string(record.outcome);
  expect_same_nodes(fit, before, "Recheck");
  QuoteSliceFit historical = fitted(c);
  QuoteSliceRepairRecord skipped;
  ASSERT_TRUE(repair_quote_slice_calendar_in_channel(historical, quotes, floors, {}, config,
                                                     &skipped,
                                                     QuoteSliceNondividendRecheck::Skip))
      << to_string(skipped.outcome);
  EXPECT_EQ(skipped.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_TRUE(check_american_calendar(earlier, *historical.prices));
  EXPECT_FALSE(check_nondividend_american(*historical.prices, df));
  EXPECT_FALSE(repair_quote_slice_calendar_in_channel(historical, quotes, floors, {}, config,
                                                      nullptr,
                                                      static_cast<QuoteSliceNondividendRecheck>(7)));
}

// A non-dividend joint case (review-L3q m3 on the joint rung; the dev200b SJB / LEMB
// pattern): the earlier member (the non-dividend chain's fit at maturity 0.9) carries puts
// one unit above the later member's (the same fit at 1.0), quoted 0.001 around them so it
// cannot move; the later member is quoted 2 wide, so the joint projection lifts its puts
// onto the earlier ones - exactly the floor under which the bounded-move repair serves a
// non-dividend violation (ACalendarRepairNeverServesANondividendViolation).
struct NondividendJointCase {
  Chain chain = nondividend_chain();
  QuoteSliceFit near;
  QuoteSliceFit far;
  std::vector<QuoteSliceQuote> near_quotes;
  std::vector<QuoteSliceQuote> far_quotes;
};

void make_nondividend_joint_case(NondividendJointCase &c) {
  c.near = fitted(c.chain);
  c.far = fitted(c.chain);
  ASSERT_TRUE(c.near.prices && c.far.prices);
  c.near.market.calendar_years = 0.9;
  c.near.prices = raised_puts(c.near, 1.0);
  c.near_quotes = quotes_around(c.near, c.chain.quotes, 0.001);
  c.far_quotes = quotes_around(c.far, c.chain.quotes, 2.0);
  ASSERT_FALSE(check_american_calendar(*c.near.prices, *c.far.prices)); // the puts cross
  ASSERT_TRUE(check_nondividend_american(*c.far.prices, c.far.diagnostics.discount));
}

TEST(QuoteSlicePublication, AJointRepairRechecksTheNondividendCertificateOnlyWhenAsked) {
  // Recheck (the default for a direct caller; fit_quote_surface under Validate): refused,
  // nothing changes.
  NondividendJointCase c;
  make_nondividend_joint_case(c);
  const auto far_nodes = nodes_of(c.far);
  const std::array<QuoteSliceJointMember, 2> members{
      {{&c.near, c.near_quotes}, {&c.far, c.far_quotes}}};
  QuoteSliceRepairRecord record;
  EXPECT_FALSE(
      repair_quote_slice_calendar_joint(members, {}, QuoteSliceRepairConfig{}, nullptr, &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::NotCertified) << to_string(record.outcome);
  expect_same_nodes(c.far, far_nodes, "far (Recheck)");
  EXPECT_TRUE(c.far.moved_nodes.empty());
  // Skip (fit_quote_surface without Validate, PM ruling 2026-10-23): the historical rule -
  // the projection is applied and the later member serves exactly the violation Recheck
  // refused, as the keys-ON repair arms' base served it.
  NondividendJointCase h;
  make_nondividend_joint_case(h);
  const std::array<QuoteSliceJointMember, 2> historical{
      {{&h.near, h.near_quotes}, {&h.far, h.far_quotes}}};
  QuoteSliceRepairRecord skipped;
  std::vector<std::size_t> moved;
  ASSERT_TRUE(repair_quote_slice_calendar_joint(historical, {}, QuoteSliceRepairConfig{}, &moved,
                                                &skipped, QuoteSliceNondividendRecheck::Skip))
      << to_string(skipped.outcome);
  EXPECT_EQ(skipped.outcome, QuoteSliceRepairOutcome::Applied);
  ASSERT_EQ(moved.size(), 2u);
  EXPECT_GT(moved[1], 0u); // the later member moved
  EXPECT_TRUE(check_american_calendar(*h.near.prices, *h.far.prices));
  EXPECT_FALSE(check_nondividend_american(*h.far.prices, h.far.diagnostics.discount));
  // An invalid value is refused and changes nothing.
  NondividendJointCase v;
  make_nondividend_joint_case(v);
  const auto v_far = nodes_of(v.far);
  const std::array<QuoteSliceJointMember, 2> invalid{
      {{&v.near, v.near_quotes}, {&v.far, v.far_quotes}}};
  EXPECT_FALSE(repair_quote_slice_calendar_joint(invalid, {}, QuoteSliceRepairConfig{}, nullptr,
                                                 nullptr,
                                                 static_cast<QuoteSliceNondividendRecheck>(7)));
  expect_same_nodes(v.far, v_far, "far (invalid)");
}

// ── The publication validator (deliverable 1) ───────────────────────────────────────

// smile_chain over strikes lo..hi every `step` (rate 3 %, yield 1 %, +/- 2 % spreads).
Chain smile_range(double years, double atm, double skew, int lo, int hi, int step) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, years, years, 0.03, 0.01, 0.0};
  for (int strike = lo; strike <= hi; strike += step) {
    const double k = static_cast<double>(strike);
    const double sigma = atm + skew * std::log(k / 100.0);
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(100.0, k, years, sigma, 0.03, 0.01, side);
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

TEST(QuoteSlicePublication, ACleanPublishedSliceHasNoViolationAndCountsItsPoints) {
  const Chain c = smile_chain(0.25, 0.03, 0.01, 0.20, -0.10, 0.02);
  const QuoteSliceFit fit = fitted(c);
  ASSERT_TRUE(fit.prices);
  const std::array<QuoteSlicePublicationSlice, 1> one{{{&fit, c.quotes}}};
  const auto report = validate_published_representation(one);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_TRUE(report->clean());
  EXPECT_EQ(report->slices, 1u);
  EXPECT_EQ(report->calendar_pairs, 0u);
  EXPECT_EQ(report->withheld_rows, 0u);
  EXPECT_EQ(report->gap_segments, 0u);
  // Every node and three interior points per node segment, per side.
  const std::size_t nodes = fit.prices->nodes().size();
  EXPECT_EQ(report->points, 2u * (nodes + 3u * (nodes - 1u)));
  EXPECT_TRUE(report->findings.empty());
}

TEST(QuoteSlicePublication, ANestedDomainCalendarCrossingIsFoundAcrossANonNeighbour) {
  // E (0.25 y, steep skew) and L (0.6 y, flat 20 %) quote 75-125; M (0.32 y, E's skew)
  // only 90-110. E's wing variance puts its deep out-of-the-money puts above L's at 75-80,
  // outside M's domain, while E-M and M-L are consistent: an adjacent-maturity check
  // never compares E with L.
  const Chain e = smile_range(0.25, 0.20, -0.60, 75, 125, 5);
  const Chain m = smile_range(0.32, 0.20, -0.60, 90, 110, 2);
  const Chain l = smile_range(0.60, 0.20, 0.0, 75, 125, 5);
  const QuoteSliceFit fe = fitted(e);
  const QuoteSliceFit fm = fitted(m);
  const QuoteSliceFit fl = fitted(l);
  ASSERT_TRUE(fe.prices && fm.prices && fl.prices);
  ASSERT_TRUE(check_american_calendar(*fe.prices, *fm.prices));
  ASSERT_TRUE(check_american_calendar(*fm.prices, *fl.prices));
  ASSERT_FALSE(check_american_calendar(*fe.prices, *fl.prices));
  const std::array<QuoteSlicePublicationSlice, 3> three{
      {{&fe, e.quotes}, {&fm, m.quotes}, {&fl, l.quotes}}};
  const auto report = validate_published_representation(three);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_EQ(report->calendar_pairs, 3u);
  EXPECT_GT(report->count(QuoteSlicePublicationViolation::Calendar), 0u);
  EXPECT_EQ(report->total(), report->count(QuoteSlicePublicationViolation::Calendar));
  for (const QuoteSlicePublicationFinding &f : report->findings) {
    EXPECT_EQ(f.expiry, 0u);
    EXPECT_EQ(f.other, 2u);
    EXPECT_LT(f.strike, 90.0); // outside M's domain
    EXPECT_GT(f.excess, 0.0);
  }
  // Focused on M (the after-mutation form) only M's pairs are checked: clean.
  QuoteSlicePublicationConfig focus;
  focus.focus = 1;
  const auto focused = validate_published_representation(three, focus);
  ASSERT_TRUE(focused) << focused.error().to_string();
  EXPECT_TRUE(focused->clean());
  EXPECT_EQ(focused->calendar_pairs, 2u);
}

TEST(QuoteSlicePublication, AServedRowBeyondTheMarketDistanceBoundIsAQuoteBoxViolation) {
  const Chain c = off_market_call(3.0, 0.04);
  const std::size_t r = row_index(c, 90.0, Side::Call);
  const QuoteSliceFit fit = fitted(c);
  ASSERT_EQ(fit.rows[r].evaluation_status, QuoteSliceEvaluationStatus::Success);
  const std::array<QuoteSlicePublicationSlice, 1> one{{{&fit, c.quotes}}};
  const auto unbounded = validate_published_representation(one);
  ASSERT_TRUE(unbounded) << unbounded.error().to_string();
  EXPECT_TRUE(unbounded->clean()); // no bound, no quote-box class
  QuoteSlicePublicationConfig config;
  config.market_distance_spreads = 1.0;
  const auto bounded = validate_published_representation(one, config);
  ASSERT_TRUE(bounded) << bounded.error().to_string();
  ASSERT_EQ(bounded->total(), 1u);
  ASSERT_EQ(bounded->count(QuoteSlicePublicationViolation::QuoteBox), 1u);
  const QuoteSlicePublicationFinding &f = bounded->findings.front();
  EXPECT_EQ(f.row, r);
  EXPECT_EQ(f.side, Side::Call);
  EXPECT_EQ(f.strike, 90.0);
  const double d =
      market_distance_spreads(fit.rows[r].fitted_price, c.quotes[r].bid, c.quotes[r].ask, 0.01);
  EXPECT_NEAR(f.excess, d - 1.0, 1.0e-9);
}

TEST(QuoteSlicePublication, ANondividendViolationOfThePublishedCurveIsFound) {
  const Chain c = nondividend_chain();
  QuoteSliceFit fit = fitted(c);
  ASSERT_TRUE(fit.prices);
  const std::array<QuoteSlicePublicationSlice, 1> one{{{&fit, c.quotes}}};
  const auto before = validate_published_representation(one);
  ASSERT_TRUE(before) << before.error().to_string();
  EXPECT_TRUE(before->clean());
  fit.prices = raised_puts(fit, 1.0); // certified, but beyond the non-dividend bound
  const auto after = validate_published_representation(one);
  ASSERT_TRUE(after) << after.error().to_string();
  EXPECT_EQ(after->count(QuoteSlicePublicationViolation::Nondividend), 1u);
}

TEST(QuoteSlicePublication, InvalidInputIsRefusedAtTheBoundary) {
  const Chain c = smile_chain(0.25, 0.03, 0.01, 0.20, -0.10, 0.02);
  const QuoteSliceFit fit = fitted(c);
  const std::array<QuoteSlicePublicationSlice, 1> one{{{&fit, c.quotes}}};
  const std::array<QuoteSlicePublicationSlice, 1> null_fit{{{nullptr, c.quotes}}};
  const std::array<QuoteSlicePublicationSlice, 1> short_rows{
      {{&fit, std::span<const QuoteSliceQuote>(c.quotes).first(2)}}};
  const std::array<QuoteSlicePublicationSlice, 2> twice{{{&fit, c.quotes}, {&fit, c.quotes}}};
  EXPECT_FALSE(validate_published_representation(null_fit));
  EXPECT_FALSE(validate_published_representation(short_rows));
  EXPECT_FALSE(validate_published_representation(twice)); // equal maturities
  QuoteSlicePublicationConfig config;
  config.focus = 1;
  EXPECT_FALSE(validate_published_representation(one, config));
  config = {};
  config.dense_points = 65;
  EXPECT_FALSE(validate_published_representation(one, config));
  config = {};
  config.box_tick = 0.0;
  EXPECT_FALSE(validate_published_representation(one, config));
  config = {};
  config.market_distance_spreads = -1.0;
  EXPECT_FALSE(validate_published_representation(one, config));
  EXPECT_STREQ(to_string(QuoteSlicePublicationViolation::Identification), "identification");
  EXPECT_STREQ(to_string(QuoteSlicePublicationViolation::Calendar), "calendar");
}

// ── Combined path: repair -> revalidate -> publish -> evaluate (deliverables 1, 5) ──

// lane C0's calendar-repair fixture (quote_slice_identification_test.cpp): puts from `lo`
// to 148 every 4 at a 5 % rate, skew sigma(K) = atm + skew x clamp((116 - K) / 40, 0, 1),
// +/- 5 % spreads (at least 0.01), each quote carrying the yield.
Chain put_chain(double years, double atm, double skew, int lo, double yield) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, years, years, 0.05, yield, 0.0};
  for (int strike = lo; strike <= 148; strike += 4) {
    const double k = static_cast<double>(strike);
    const double sigma = atm + skew * std::clamp((116.0 - k) / 40.0, 0.0, 1.0);
    const auto price = andersen_lake(100.0, k, years, sigma, 0.05, yield, Side::Put);
    if (!price) {
      ADD_FAILURE() << price.error().to_string();
      return out;
    }
    const double spread = std::max(0.01, *price * 0.05);
    out.quotes.push_back({k, Side::Put, std::max(*price - spread, 0.0), *price + spread, yield});
  }
  return out;
}

// N (0.45 y, 50 %, 4 % yield): puts 116-148 with time value; F (0.5 y): puts 60-148, at
// their exercise value from K 116 up. F outranks N; the bounded calendar repair caps N
// onto F's exercise values (re-implied: FloorVol under PriceOnly), far below N's quotes.
std::array<Chain, 2> calendar_repair_chains() {
  return {put_chain(0.45, 0.5, 0.0, 116, 0.04), put_chain(0.5, 0.05, 0.30, 60, 0.0)};
}

Result<std::vector<QuoteSurfaceSlice>>
fit_calendar_repair(const std::array<Chain, 2> &chains, QuoteSliceFloorPolicy policy,
                    QuoteSliceMarketDistance gate, QuoteSurfacePublication publication) {
  const std::array<QuoteSurfaceExpiry, 2> both{{{chains[0].market, chains[0].quotes, {}, {}},
                                                {chains[1].market, chains[1].quotes, {}, {}}}};
  QuoteSurfaceConfig surface;
  surface.grade_b_min_mid_strikes = 10;
  surface.calendar_repair_half_spreads = 100.0;
  surface.publication = publication;
  QuoteSliceConfig config;
  config.side_policy = QuoteSliceSidePolicy::All;
  config.channel.repair_half_spreads = 100.0;
  config.floor_policy = policy;
  config.repair.market_distance = gate;
  return fit_quote_surface(both, config, surface);
}

// One answer per contract (lane C0's contract, checked independently of the validator):
// every served row says what query_quote_slice says; a MarketDistanceFailure row's
// contract is refused by every serving call. Returns the refused rows.
std::size_t expect_one_answer_per_contract(const QuoteSliceFit &fit,
                                           std::span<const QuoteSliceQuote> quotes,
                                           const std::string &label) {
  std::size_t refused = 0;
  for (std::size_t i = 0; i < quotes.size() && i < fit.rows.size(); ++i) {
    const QuoteSliceRow &row = fit.rows[i];
    const std::string at = label + " row " + std::to_string(i);
    const auto answer = query_quote_slice(fit, quotes[i].strike, quotes[i].side);
    if (!answer) {
      ADD_FAILURE() << at << ": " << answer.error().to_string();
      continue;
    }
    if (row.evaluation_status == QuoteSliceEvaluationStatus::MarketDistanceFailure) {
      ++refused;
      EXPECT_EQ(answer->status, QuoteSliceEvaluationStatus::MarketDistanceFailure) << at;
      EXPECT_TRUE(std::isnan(answer->price)) << at;
      const auto value = evaluate_quote_slice(fit, quotes[i].strike, quotes[i].side);
      EXPECT_FALSE(value) << at;
      if (!value) EXPECT_EQ(value.error().code(), ErrorCode::Unavailable) << at;
      EXPECT_FALSE(served_quote_slice_price(fit, quotes[i].strike, quotes[i].side)) << at;
      continue;
    }
    if (row.evaluation_status != QuoteSliceEvaluationStatus::Success &&
        row.evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) {
      continue;
    }
    EXPECT_EQ(bits(answer->price), bits(row.fitted_price)) << at;
    const auto served = served_quote_slice_price(fit, quotes[i].strike, quotes[i].side);
    EXPECT_TRUE(served && bits(*served) == bits(row.fitted_price)) << at;
    if (answer->status == QuoteSliceEvaluationStatus::Success) {
      EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success) << at;
      EXPECT_EQ(vol_convention(row), answer->vol_convention) << at;
      EXPECT_EQ(bits(row.fitted_iv), bits(answer->volatility)) << at;
      continue;
    }
    EXPECT_EQ(answer->status, QuoteSliceEvaluationStatus::PriceOnly) << at;
    EXPECT_EQ(vol_convention(row), QuoteSliceVolConvention::None) << at;
    EXPECT_EQ(row.price_only_reason, answer->reason) << at;
    EXPECT_EQ(bits(row.iv_lower), bits(answer->iv_lower)) << at;
    EXPECT_EQ(bits(row.iv_upper), bits(answer->iv_upper)) << at;
  }
  return refused;
}

TEST(QuoteSlicePublication, ARefusedRowOpensATypedGapInTheServedSupport) {
  const std::vector<Chain> one{off_market_call(3.0, 0.04)};
  auto gate = fit_quote_surface(expiries(one),
                                market_distance_config(QuoteSliceMarketDistance::Gate));
  ASSERT_TRUE(gate) << gate.error().to_string();
  QuoteSurfaceSlice &s = (*gate)[0];
  ASSERT_EQ(s.market_gated_rows, 1u);
  EXPECT_EQ(s.withheld_rows, 1u);
  EXPECT_FALSE(served_quote_slice_price(s.fit, 90.0, Side::Call));
  EXPECT_FALSE(served_quote_slice_price(s.fit, 91.0, Side::Call));
  EXPECT_TRUE(served_quote_slice_price(s.fit, 85.0, Side::Call));
  EXPECT_TRUE(served_quote_slice_price(s.fit, 90.0, Side::Put));
  const auto value = evaluate_quote_slice(s.fit, 90.0, Side::Call);
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().code(), ErrorCode::Unavailable);
  const std::vector<QuoteSliceGap> gaps = quote_slice_gaps(s.fit);
  ASSERT_EQ(gaps.size(), 1u);
  EXPECT_EQ(gaps[0].side, Side::Call);
  EXPECT_EQ(gaps[0].lo, 85.0);
  EXPECT_EQ(gaps[0].hi, 95.0);
  EXPECT_TRUE(gaps[0].lo_open);
  EXPECT_TRUE(gaps[0].hi_open);
  expect_one_answer_per_contract(s.fit, one[0].quotes, "gate");
  // The validator agrees (Support): the refused row's contract is not served.
  const std::array<QuoteSlicePublicationSlice, 1> published{{{&s.fit, one[0].quotes}}};
  QuoteSlicePublicationConfig config;
  config.market_distance_spreads = 1.0;
  const auto report = validate_published_representation(published, config);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_TRUE(report->clean());
  EXPECT_EQ(report->withheld_rows, 1u);
  EXPECT_EQ(report->gap_segments, 2u); // 85-90 and 90-95 on the call side
  // Without the mask (the pre-C2 fit) the refused row's value is served: a Support
  // violation at that row.
  s.fit.withheld_nodes.clear();
  const auto leak = validate_published_representation(published, config);
  ASSERT_TRUE(leak) << leak.error().to_string();
  ASSERT_EQ(leak->count(QuoteSlicePublicationViolation::Support), 1u);
  EXPECT_EQ(leak->findings.front().row, row_index(one[0], 90.0, Side::Call));
}

TEST(QuoteSlicePublication, PublicationValidationIsOptInAndNamed) {
  EXPECT_EQ(QuoteSurfaceConfig{}.publication, QuoteSurfacePublication::Off);
  EXPECT_STREQ(to_string(QuoteSurfacePublication::Off), "off");
  EXPECT_STREQ(to_string(QuoteSurfacePublication::Validate), "validate");
  const std::vector<Chain> one{smile_chain(0.25, 0.03, 0.01, 0.20, -0.10, 0.02)};
  QuoteSurfaceConfig surface;
  surface.publication = static_cast<QuoteSurfacePublication>(7);
  EXPECT_FALSE(fit_quote_surface(expiries(one), QuoteSliceConfig{}, surface));
}

TEST(QuoteSlicePublication, RepairRevalidatePublishEvaluateGivesOneTypedAnswerPerContract) {
  // The combined path on lane C0's calendar-repair fixture under PriceOnly with the
  // market-distance gate: N is repaired onto F's exercise values (moved nodes re-implied
  // FloorVol), the repair is validated before it is committed, the gate then refuses
  // N's repaired rows (far below its quotes) and withholds them, and the final check
  // passes the whole published surface. Every contract gets one answer.
  const std::array<Chain, 2> chains = calendar_repair_chains();
  const auto off = fit_calendar_repair(chains, QuoteSliceFloorPolicy::PriceOnly,
                                       QuoteSliceMarketDistance::Gate,
                                       QuoteSurfacePublication::Off);
  const auto on = fit_calendar_repair(chains, QuoteSliceFloorPolicy::PriceOnly,
                                      QuoteSliceMarketDistance::Gate,
                                      QuoteSurfacePublication::Validate);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_TRUE(on) << on.error().to_string();
  const QuoteSurfaceSlice &near = (*on)[0];
  const QuoteSurfaceSlice &far = (*on)[1];
  ASSERT_NE(near.grade, QuoteSliceGrade::Refused) << near.refusal << ": " << near.error;
  ASSERT_NE(far.grade, QuoteSliceGrade::Refused) << far.refusal << ": " << far.error;
  ASSERT_TRUE(near.calendar_repaired);
  ASSERT_GT(near.market_gated_rows, 0u);
  // The repair was validated after its mutation (and after C0's align_with_query, which
  // runs inside the library commit of the trial), and nothing was rolled back or found.
  EXPECT_GT(near.publication_checks, 0u);
  EXPECT_EQ(near.publication_rollbacks, 0u);
  EXPECT_EQ(near.publication_violations, 0u);
  EXPECT_EQ(far.publication_violations, 0u);
  EXPECT_GE(near.withheld_rows, near.market_gated_rows);
  // Validation changes nothing on a clean surface: same rows bit for bit as Off.
  for (std::size_t s = 0; s < 2; ++s) {
    const QuoteSliceFit &a = (*off)[s].fit;
    const QuoteSliceFit &b = (*on)[s].fit;
    ASSERT_EQ(a.rows.size(), b.rows.size());
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
      EXPECT_EQ(a.rows[i].evaluation_status, b.rows[i].evaluation_status) << s << " " << i;
      EXPECT_EQ(bits(a.rows[i].fitted_price), bits(b.rows[i].fitted_price)) << s << " " << i;
      EXPECT_EQ(a.rows[i].price_only_reason, b.rows[i].price_only_reason) << s << " " << i;
    }
    EXPECT_EQ(a.withheld_nodes, b.withheld_nodes) << s;
  }
  EXPECT_EQ(expect_one_answer_per_contract(near.fit, chains[0].quotes, "near"),
            near.withheld_rows);
  expect_one_answer_per_contract(far.fit, chains[1].quotes, "far");
  // The published surface passes the final check, labels included.
  const std::array<QuoteSlicePublicationSlice, 2> published{
      {{&near.fit, chains[0].quotes}, {&far.fit, chains[1].quotes}}};
  QuoteSlicePublicationConfig config;
  config.market_distance_spreads = 1.0;
  const auto report = validate_published_representation(published, config);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_TRUE(report->clean());
  EXPECT_EQ(report->withheld_rows, near.withheld_rows + far.withheld_rows);
  EXPECT_FALSE(quote_slice_gaps(near.fit).empty());
}

// ── Validate's enforcement, a backstop no fit reaches today (fix round 1, review m1) ──
//
// Every mutation path re-certifies what the validator checks, so neither the
// after-mutation rollback nor the final pass's refusals fire on any fixture or cohort.
// The publication_seam.hpp injection makes the check report a finding. The tests fail if
// the enforcement is reverted: a trial committed despite its check, a structural finding
// that does not refuse its slice, or a QuoteBox finding that does not refuse and withhold
// its row.

// The calendar-repair surface (PriceOnly, no market distance, Validate) fitted with
// `injection` armed on this thread.
Result<std::vector<QuoteSurfaceSlice>> fit_injected(const std::array<Chain, 2> &chains,
                                                    detail::PublicationInjection &injection) {
  const detail::ScopedPublicationInjection armed(injection);
  return fit_calendar_repair(chains, QuoteSliceFloorPolicy::PriceOnly,
                             QuoteSliceMarketDistance::Off, QuoteSurfacePublication::Validate);
}

TEST(QuoteSlicePublication, AFailedAfterMutationCheckRollsTheRepairBack) {
  const std::array<Chain, 2> chains = calendar_repair_chains();
  detail::PublicationInjection injection;
  injection.rollback_expiry = 0; // N
  const auto surface = fit_injected(chains, injection);
  ASSERT_TRUE(surface) << surface.error().to_string();
  const QuoteSurfaceSlice &near = (*surface)[0];
  // Without the injection N is repaired onto F (RepairRevalidatePublishEvaluate...); with
  // every check of N's trials failing, no repair is committed and N stays refused.
  EXPECT_GE(injection.injected, 1u);
  EXPECT_EQ(near.publication_rollbacks, near.publication_checks);
  EXPECT_GE(near.publication_rollbacks, 1u);
  EXPECT_FALSE(near.calendar_repaired);
  EXPECT_FALSE(near.calendar_channel_repaired);
  EXPECT_EQ(near.grade, QuoteSliceGrade::Refused) << near.refusal;
  EXPECT_NE((*surface)[1].grade, QuoteSliceGrade::Refused) << (*surface)[1].refusal;
}

TEST(QuoteSlicePublication, AStructuralFindingOfTheFinalPassRefusesItsSlice) {
  const std::array<Chain, 2> chains = calendar_repair_chains();
  detail::PublicationInjection injection;
  injection.refuse_expiry = 1; // F
  const auto surface = fit_injected(chains, injection);
  ASSERT_TRUE(surface) << surface.error().to_string();
  const QuoteSurfaceSlice &far = (*surface)[1];
  EXPECT_EQ(injection.injected, 1u);
  EXPECT_EQ(far.grade, QuoteSliceGrade::Refused);
  EXPECT_EQ(far.refusal, "publication_invalid");
  EXPECT_EQ(far.publication_violations, 1u);
  EXPECT_FALSE(far.fit.prices); // stripped: nothing of F is served
  for (const QuoteSliceRow &row : far.fit.rows) {
    EXPECT_NE(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
    EXPECT_NE(row.evaluation_status, QuoteSliceEvaluationStatus::PriceOnly);
  }
  EXPECT_NE((*surface)[0].grade, QuoteSliceGrade::Refused) << (*surface)[0].refusal;
}

TEST(QuoteSlicePublication, ARowFindingOfTheFinalPassRefusesAndWithholdsItsRow) {
  const std::array<Chain, 2> chains = calendar_repair_chains();
  const auto clean = fit_calendar_repair(chains, QuoteSliceFloorPolicy::PriceOnly,
                                         QuoteSliceMarketDistance::Off,
                                         QuoteSurfacePublication::Validate);
  ASSERT_TRUE(clean) << clean.error().to_string();
  const QuoteSliceFit &before = (*clean)[1].fit; // F
  const std::size_t k = row_index(chains[1], 100.0, Side::Put);
  ASSERT_EQ(before.rows[k].evaluation_status, QuoteSliceEvaluationStatus::Success);
  ASSERT_EQ((*clean)[1].withheld_rows, 0u);
  detail::PublicationInjection injection;
  injection.withhold_expiry = 1;
  injection.row = k;
  const auto surface = fit_injected(chains, injection);
  ASSERT_TRUE(surface) << surface.error().to_string();
  const QuoteSurfaceSlice &far = (*surface)[1];
  EXPECT_EQ(injection.injected, 1u);
  EXPECT_NE(far.grade, QuoteSliceGrade::Refused) << far.refusal;
  EXPECT_EQ(far.publication_violations, 1u);
  EXPECT_EQ(far.fit.rows[k].evaluation_status, QuoteSliceEvaluationStatus::MarketDistanceFailure);
  EXPECT_GE(far.withheld_rows, 1u);
  const auto answer = query_quote_slice(far.fit, 100.0, Side::Put);
  ASSERT_TRUE(answer) << answer.error().to_string();
  EXPECT_EQ(answer->status, QuoteSliceEvaluationStatus::MarketDistanceFailure);
  EXPECT_FALSE(served_quote_slice_price(far.fit, 100.0, Side::Put));
  expect_one_answer_per_contract(far.fit, chains[1].quotes, "far");
}

TEST(QuoteSlicePublication, ALabelThatDisagreesWithTheQueryIsAnIdentificationViolation) {
  // Validation must see the labels C0's align_with_query writes: a repaired row whose
  // labels are stale (as they would be if the check ran before the alignment) is a
  // finding, so the after-mutation check runs on the aligned trial.
  const std::array<Chain, 2> chains = calendar_repair_chains();
  for (const QuoteSliceFloorPolicy policy :
       {QuoteSliceFloorPolicy::Legacy, QuoteSliceFloorPolicy::PriceOnly}) {
    auto fitted_surface = fit_calendar_repair(chains, policy, QuoteSliceMarketDistance::Off,
                                              QuoteSurfacePublication::Off);
    ASSERT_TRUE(fitted_surface) << fitted_surface.error().to_string();
    QuoteSliceFit &near = (*fitted_surface)[0].fit;
    ASSERT_TRUE((*fitted_surface)[0].calendar_repaired);
    const std::array<QuoteSlicePublicationSlice, 1> one{{{&near, chains[0].quotes}}};
    const auto aligned = validate_published_representation(one);
    ASSERT_TRUE(aligned) << aligned.error().to_string();
    EXPECT_EQ(aligned->count(QuoteSlicePublicationViolation::Identification), 0u)
        << to_string(policy);
    const auto floor_row = std::find_if(near.rows.begin(), near.rows.end(), [](const auto &row) {
      return row.price_only_reason == QuoteSlicePriceOnlyReason::FloorVol;
    });
    ASSERT_NE(floor_row, near.rows.end()) << to_string(policy);
    floor_row->price_only_reason = QuoteSlicePriceOnlyReason::None; // a stale label
    const auto stale = validate_published_representation(one);
    ASSERT_TRUE(stale) << stale.error().to_string();
    ASSERT_EQ(stale->count(QuoteSlicePublicationViolation::Identification), 1u)
        << to_string(policy);
    EXPECT_EQ(stale->findings.front().row,
              static_cast<std::size_t>(floor_row - near.rows.begin()));
  }
}

// ── K4b declared hunk (C3 review D2): the parity-coupled calendar clamp ─────────────

// TIGR 20770 (dev200b; C3 evidence probe\quotes-TIGR-b.csv, its 36 quotes) between the
// accepted 20763 (floors) and 20784 (caps) of the call-floor run: the clamp's floors and caps
// at 20770's nodes as C3's trace printed them (probe\dbg-tigr-on-repair-trace.txt, the first
// T=0.086125 attempt). At K 1.0-3.0 the call's floor equals its cap; at K 3 the call is raised
// to 1.18496 while the put stays near the fit's, so call - put leaves the certificate's affine
// bound prepaid_max - K discount_min and AmericanPriceSlice::create refuses the slice ("node 5:
// affine call-minus-put bound violated") - 36 rows lost to calendar_arbitrage. The parity-
// coupled clamp raises that node's put, inside its own calendar cap, to the bound.
struct TigrClampNode {
  double strike, floor_call, floor_put, cap_call, cap_put;
};
constexpr TigrClampNode kTigr20770Clamp[] = {
    {0.5, 3.65517349395857, 7.03807736204925e-173, 3.65561264802797, 2.56538608471777e-15},
    {1.0, 3.15622532466616, 8.78684961095426e-13, 3.15622532466616, 3.0858944396449e-08},
    {1.5, 2.65683800130435, 9.90631593886388e-08, 2.65683800130435, 2.94284995465659e-05},
    {2.0, 2.15745067794255, 5.32720730931763e-05, 2.15745067794255, 0.00148997579284861},
    {2.5, 1.66286122073876, 0.00206370122159342, 1.66286122073876, 0.00695041378516763},
    {3.0, 1.18496274352134, 0.0096413575919144, 1.18496274352134, 0.0268025982401},
    {3.5, 0.716442617772497, 0.0385343987755786, 0.73320289761091, 0.0724769380701921},
    {4.0, 0.318950299713772, 0.140331071793943, 0.400239231738017, 0.237165279016782},
    {4.5, 0.0993543350121134, 0.422635666029215, 0.185001093015903, 0.520214327116729},
    {5.0, 0.0411877761830755, 0.86804698564258, 0.0811497848097429, 0.915623644811922},
    {5.5, 0.0222910052205404, 1.35245323547547, 0.0370139833947931, 1.3717176844344},
    {6.0, 0.0128519087503053, 1.84647559900552, 0.0222948172593044, 1.85742234694302},
    {6.5, 0.00802724198133015, 2.34501108805046, 0.0178027144405422, 2.35295897017364},
    {7.0, 0.00513011302536369, 2.84500002861023, 0.0144862635744217, 2.85014805536949},
    {7.5, 0.00256979267110344, 3.34500002861023, 0.0111698127083012, 3.34733714056535},
    {8.0, 0.0010514429327423, 3.84500002861023, 0.0078533618421807, 3.84541611206876},
    {8.5, 0.000392450060277517, 4.34500002861023, 0.00505438511792927, 4.34500002861023},
    {9.0, 0.000143697182296793, 4.84500002861023, 0.0208574760694072, 4.84500002861023}};

std::vector<QuoteSliceQuote> tigr_20770_quotes() {
  const std::vector<std::array<double, 4>> rows = {
      {0.5, 1, 3.25, 4.199999809265137},   {0.5, 0, 0, 2.130000114440918},
      {1, 1, 2.5, 4.449999809265137},      {1, 0, 0, 1},
      {1.5, 1, 1.8700000047683716, 4.800000190734863}, {1.5, 0, 0, 1},
      {2, 1, 1.9600000381469727, 2.9100000858306885},  {2, 0, 0, 1},
      {2.5, 1, 1.4700000286102295, 2.4200000762939453}, {2.5, 0, 0, 1},
      {3, 1, 0.9800000190734863, 1.7300000190734863},  {3, 0, 0, 0.949999988079071},
      {3.5, 1, 0.5, 1.25}, {3.5, 0, 0.029999999329447746, 0.07000000029802322},
      {4, 1, 0.3100000023841858, 0.4099999964237213},
      {4, 0, 0.12999999523162842, 0.20000000298023224},
      {4.5, 1, 0.10000000149011612, 0.1899999976158142},
      {4.5, 0, 0.41999998688697815, 0.4699999988079071},
      {5, 1, 0.03999999910593033, 0.07999999821186066},
      {5, 0, 0.3499999940395355, 1.100000023841858},
      {5.5, 1, 0, 0.05000000074505806}, {5.5, 0, 0.8199999928474426, 1.5700000524520874},
      {6, 1, 0, 0.019999999552965164},  {6, 0, 0.9100000262260437, 3.799999952316284},
      {6.5, 1, 0, 0.019999999552965164}, {6.5, 0, 1.409999966621399, 4.300000190734863},
      {7, 1, 0, 0.8600000143051147},    {7, 0, 2.0999999046325684, 4.800000190734863},
      {7.5, 1, 0, 0.8500000238418579},  {7.5, 0, 2.5999999046325684, 5.300000190734863},
      {8, 1, 0, 0.05000000074505806},   {8, 0, 3.0999999046325684, 5.800000190734863},
      {8.5, 1, 0, 0.949999988079071},   {8.5, 0, 3.5999999046325684, 6.300000190734863},
      {9, 1, 0, 0.8500000238418579},    {9, 0, 4.099999904632568, 6.800000190734863}};
  std::vector<QuoteSliceQuote> out;
  for (const auto &r : rows) {
    out.push_back({r[0], r[1] != 0.0 ? Side::Call : Side::Put, r[2], r[3]});
  }
  return out;
}

TEST(QuoteSliceCalendarParity, ARaisedCallRaisesItsPutToTheAffineBound) {
  const QuoteSliceMarket market{4.1549999713897705, 0.08612515777349472, 0.08612515777349472,
                                0.045037999749183655, 0.0018157954327762127, 0.0};
  const std::vector<QuoteSliceQuote> quotes = tigr_20770_quotes();
  // The channel fit (the graded sparse / ordinary profiles' method): every valid contract's
  // bid / ask channel, so the wide, bidless TIGR quotes still give a certified slice.
  QuoteSliceConfig config;
  config.side_policy = QuoteSliceSidePolicy::All;
  config.channel.repair_half_spreads = 0.5;
  config.repair.calendar_parity_clamp = true; // the opt-in rule under test
  auto fit = fit_quote_slice_channel(market, quotes, config, nullptr);
  // The historical rule (the default, keys off) refuses the same repair: the fail-on-revert.
  QuoteSliceConfig historical = config;
  historical.repair.calendar_parity_clamp = false;
  auto refused = fit_quote_slice_channel(market, quotes, historical, nullptr);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  const AmericanPriceBounds bounds = fit->prices->bounds();
  // The accepted neighbours as slices on 20770's strikes (wide carry bounds: they are other
  // expiries'). The floors and caps are maxima / minima over several accepted slices with
  // different domains: the floor slice is 20763's domain [1, 9] (the 0.5 floor came from an
  // earlier expiry and does not bind), the cap slice stops at 8.5 (its 9.0 cap came from a
  // different later slice).
  std::vector<AmericanPriceNode> floors;
  std::vector<AmericanPriceNode> caps;
  for (const TigrClampNode &n : kTigr20770Clamp) {
    if (n.strike >= 1.0) floors.push_back({n.strike, n.floor_call, n.floor_put});
    if (n.strike <= 8.5) caps.push_back({n.strike, n.cap_call, n.cap_put});
  }
  const AmericanPriceBounds wide{market.spot, 0.5, 1.0, 0.0, market.spot};
  const auto earlier = AmericanPriceSlice::create(floors, wide);
  const auto later = AmericanPriceSlice::create(caps, wide);
  ASSERT_TRUE(earlier) << earlier.error().to_string();
  ASSERT_TRUE(later) << later.error().to_string();
  const std::array<const AmericanPriceSlice *, 1> before{&*earlier};
  const std::array<const AmericanPriceSlice *, 1> after{&*later};
  ASSERT_TRUE(refused);
  ASSERT_EQ(refused->fit_status, QuoteSliceFitStatus::Success);
  double refused_required = 0.0;
  EXPECT_FALSE(repair_quote_slice_calendar(*refused, quotes, before, after, 2.0,
                                           &refused_required));
  const std::vector<AmericanPriceNode> original(fit->prices->nodes().begin(),
                                                fit->prices->nodes().end());
  // 2 half-spreads: the graded policy's rejected-slice retry budget (calendar_repair 0.5 x
  // calendar_retry_factor 4); this fit is not the run's (its own profile), so its moves differ.
  double required = 0.0;
  const bool repaired =
      repair_quote_slice_calendar(*fit, quotes, before, after, 2.0, &required);
  std::printf("[K4b TIGR] repaired %d required %.6g half-spreads; nodes %zu\n",
              repaired ? 1 : 0, required, original.size());
  ASSERT_TRUE(repaired) << "the calendar repair refused TIGR 20770 (required " << required
                        << " half-spreads)";
  // The repaired slice: inside every floor and cap, the certificate's affine bound held, and
  // the put moved only where the call's clamp pushed call - put past that bound.
  std::size_t coupled = 0;
  for (const AmericanPriceNode &node : fit->prices->nodes()) {
    const double tol = 1.0e-8 + 1.0e-9 * std::max(market.spot, node.strike);
    const double upper = bounds.prepaid_max - node.strike * bounds.discount_min;
    EXPECT_LE(node.call - node.put, upper + tol) << node.strike;
    for (const TigrClampNode &n : kTigr20770Clamp) {
      if (n.strike != node.strike) continue;
      if (n.strike >= 1.0) {
        EXPECT_GE(node.call, n.floor_call - tol) << node.strike;
        EXPECT_GE(node.put, n.floor_put - tol) << node.strike;
      }
      if (n.strike <= 8.5) {
        EXPECT_LE(node.call, n.cap_call + tol) << node.strike;
        EXPECT_LE(node.put, n.cap_put + tol) << node.strike;
      }
    }
    for (const AmericanPriceNode &o : original) {
      if (o.strike == node.strike && node.put > o.put &&
          node.call - node.put >= upper - tol) {
        ++coupled;
        std::printf("[K4b TIGR] K %.1f: call %.8f put %.8f -> %.8f (bound %.8f)\n", node.strike,
                    node.call, o.put, node.put, upper);
      }
    }
  }
  EXPECT_GE(coupled, 1u);
  // C2's publication validation still runs on what the repair served: the slice is clean
  // (certificate, shape, cross-side affine bounds, support, identification).
  const std::array<QuoteSlicePublicationSlice, 1> one{QuoteSlicePublicationSlice{&*fit, quotes}};
  const auto report = validate_published_representation(one);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_TRUE(report->clean()) << report->total() << " findings, first "
                               << (report->findings.empty()
                                       ? "-"
                                       : to_string(report->findings.front().kind));
}

} // namespace
} // namespace atx::vol

namespace atx::vol {
namespace {

QuoteSliceFit walk_fixture(double years, double shift) {
  QuoteSliceFit fit;
  fit.market = {100., years, years, .03, .01, 0.};
  fit.fit_status = QuoteSliceFitStatus::Success;
  fit.diagnostics.effective_rate = .03;
  fit.diagnostics.effective_yield = .01;
  fit.diagnostics.discount = std::exp(-.03 * years);
  std::array<AmericanPriceNode, 5> nodes{{{80., 22., 2.}, {90., 14., 4.},
      {100., 8., 8.}, {110., 4., 14.}, {120., 2., 22.}}};
  for (auto &node : nodes) { node.call += shift; node.put += shift; }
  auto prices = AmericanPriceSlice::create(nodes, {100., 1., 1., 100., 100.});
  EXPECT_TRUE(prices);
  if (prices) fit.prices = std::move(*prices);
  return fit;
}

TEST(QuoteSlicePublication, LinearWalkRetainsAll528PairsAndFocusedWork) {
  namespace timing = counters::fit_timing;
  std::vector<QuoteSliceFit> fits;
  fits.reserve(33);
  for (std::size_t i = 0; i < 33; ++i) {
    fits.push_back(walk_fixture(.1+.01*static_cast<double>(i), .01*static_cast<double>(i)));
  }
  std::vector<QuoteSlicePublicationSlice> views;
  for (const auto &fit : fits) views.push_back({&fit, {}});
  QuoteSlicePublicationConfig config;
  config.dense_points = 0;
  config.identification = false;
  for (const std::size_t focus : {kQuoteSlicePublicationAll, std::size_t{16}}) {
    config.focus = focus;
    timing::ChainRecorder recorder;
    const std::size_t pairs = focus == kQuoteSlicePublicationAll ? 528 : 32;
    {
      const timing::ChainScope scope(&recorder);
      const auto report = validate_published_representation(views, config);
      ASSERT_TRUE(report);
      EXPECT_TRUE(report->clean());
      EXPECT_EQ(report->calendar_pairs, pairs);
    }
    const auto &work = recorder.counts.calendar_publication;
    EXPECT_EQ(work.attempted_pairs, pairs);
    EXPECT_EQ(work.overlapping_pairs, pairs);
    EXPECT_EQ(work.knots, 5*pairs);
    EXPECT_EQ(work.midpoints, 4*pairs);
    EXPECT_EQ(work.side_comparisons, 18*pairs);
    EXPECT_EQ(work.node_advances, 8*pairs);
    EXPECT_GT(work.wall_ns, 0u);
    EXPECT_EQ(recorder.counts.calendar_certificate.attempted_pairs, 0u);
  }
}

TEST(QuoteSlicePublication, LinearWalkFindingsMatchOriginalServedApiWithMasksAndRefusals) {
  namespace timing = counters::fit_timing;
  auto early = walk_fixture(.1, .02), late = walk_fixture(.2, 0.);
  const std::array<QuoteSlicePublicationSlice, 2> slices{{{&early, {}}, {&late, {}}}};
  QuoteSlicePublicationConfig config;
  config.dense_points = 0;
  config.identification = false;
  for (unsigned variant = 0; variant < 4; ++variant) {
    if (variant == 1) early.withheld_nodes = {0, 1, 2, 0, 0};
    if (variant == 2) early.withheld_nodes = {0}; // malformed: serves nothing
    if (variant == 3) {
      early.withheld_nodes.clear();
      early.fit_status = QuoteSliceFitStatus::FitFailure;
    }
    std::vector<QuoteSlicePublicationFinding> expected;
    std::size_t sides = 0;
    // Independently reproduce the old API queries, including midpoint allowance.
    for (std::size_t i = 0; i < 9; ++i) {
      const double k = 80. + 5.*static_cast<double>(i);
      const double upper = (i % 2u) ? k+5. : k;
      const double tolerance = 1.0e-8+1.0e-9*std::max(100., upper);
      for (Side side : {Side::Call, Side::Put}) {
        const auto a = served_quote_slice_price(early, k, side);
        const auto b = served_quote_slice_price(late, k, side);
        if (!a || !b) continue;
        ++sides;
        const double excess = *a-*b-tolerance;
        if (excess > 0.) expected.push_back({QuoteSlicePublicationViolation::Calendar,
            0, 1, kQuoteSlicePublicationAll, side, k, excess});
      }
    }
    timing::ChainRecorder recorder;
    {
      const timing::ChainScope scope(&recorder);
      const auto report = validate_published_representation(slices, config);
      ASSERT_TRUE(report);
      ASSERT_EQ(report->findings.size(), expected.size());
      for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto &a = report->findings[i], &b = expected[i];
        EXPECT_EQ(a.kind, b.kind);
        EXPECT_EQ(a.expiry, b.expiry);
        EXPECT_EQ(a.other, b.other);
        EXPECT_EQ(a.side, b.side);
        EXPECT_EQ(a.strike, b.strike);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(a.excess), std::bit_cast<std::uint64_t>(b.excess));
      }
    }
    const auto &work = recorder.counts.calendar_publication;
    EXPECT_EQ(work.knots, 5u);
    EXPECT_EQ(work.midpoints, 4u);
    EXPECT_EQ(work.side_comparisons, sides);
  }
}

} // namespace
} // namespace atx::vol
