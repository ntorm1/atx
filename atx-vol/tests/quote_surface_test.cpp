#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "fitting/counters.hpp"
#include "fitting/deam_iv_cache.hpp"   // deam_iv_memo_clear (rung memo tests)
#include "fitting/expiry_schedule.hpp" // ScopedExpiryDispatchTrace (fan-out order seam)
#include "fitting/rung_memo.hpp"       // ExpiryRungMemo, ScopedRungMemo

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

// Flat-volatility American quotes whose supplied per-strike carry alternates, so
// the repriced American prices zig-zag across strikes (locally non-convex) by
// less than a few half-spreads: the bounded convexity repair has to move them.
Chain zigzag_carry_chain(double years, double sigma, double low_q, double high_q,
                         double half_spread) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, years, years, 0.03, low_q, 0.0};
  for (int strike = 80; strike <= 120; strike += 2) {
    const double q = (strike / 2) % 2 == 0 ? low_q : high_q;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price =
          andersen_lake(100.0, static_cast<double>(strike), years, sigma, 0.03, q, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * half_spread);
      out.quotes.push_back({static_cast<double>(strike), side, std::max(*price - spread, 0.0),
                            *price + spread, q});
    }
  }
  return out;
}

// At every node a repair moved, the served row and evaluate_quote_slice both
// report the volatility of the SERVED price (not the curve's) with that price.
// PriceOnly rows have no volatility to report (expect_price_only_rows).
void expect_moved_nodes_reimplied(const QuoteSliceFit &fit,
                                  std::span<const QuoteSliceQuote> quotes) {
  ASSERT_TRUE(fit.prices);
  const auto nodes = fit.prices->nodes();
  ASSERT_EQ(fit.moved_nodes.size(), nodes.size());
  std::size_t checked = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &quote = quotes[i];
    if (fit.rows[i].evaluation_status == QuoteSliceEvaluationStatus::PriceOnly) continue;
    const auto at = std::find_if(nodes.begin(), nodes.end(), [&](const AmericanPriceNode &n) {
      return n.strike == quote.strike;
    });
    ASSERT_NE(at, nodes.end());
    const unsigned bit = quote.side == Side::Call ? 1u : 2u;
    if ((fit.moved_nodes[static_cast<std::size_t>(at - nodes.begin())] & bit) == 0u) continue;
    const auto value = evaluate_quote_slice(fit, quote.strike, quote.side);
    ASSERT_TRUE(value) << value.error().to_string();
    EXPECT_DOUBLE_EQ(value->price, fit.rows[i].fitted_price) << i;
    EXPECT_NEAR(value->volatility, fit.rows[i].fitted_iv, 1.0e-6) << i;
    const auto repriced = andersen_lake(fit.market.spot, quote.strike, fit.market.variance_years,
                                        value->volatility, fit.diagnostics.effective_rate,
                                        quote.continuous_yield.value_or(fit.market.continuous_yield),
                                        quote.side);
    ASSERT_TRUE(repriced);
    EXPECT_NEAR(*repriced, value->price, 1.0e-4) << i;
    ++checked;
  }
  EXPECT_GT(checked, 0u);
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

TEST(QuoteSliceChannel, EveryPointFamilySolvesTheSameChannelObjective) {
  const auto c = chain(0.5, 0.25, -0.05, 0.02);
  // The point spline keeps its own pinned butterfly gate, which may refuse the
  // channel targets (it does on this chain); a refusal is a fit failure of the
  // expiry, never an error.
  {
    auto config = channel_config();
    config.channel.family = VolCurveKind::SplineVol;
    const auto fit = fit_quote_slice(c.market, c.quotes, config);
    ASSERT_TRUE(fit) << fit.error().to_string();
    EXPECT_TRUE(fit->fit_status == QuoteSliceFitStatus::Success ||
                fit->fit_status == QuoteSliceFitStatus::FitFailure);
  }
  for (const VolCurveKind kind : {VolCurveKind::Essvi, VolCurveKind::Svi, VolCurveKind::C8,
                                  VolCurveKind::ConvexDense}) {
    auto config = channel_config();
    config.channel.family = kind;
    const auto fit = fit_quote_slice(c.market, c.quotes, config);
    ASSERT_TRUE(fit) << to_string(kind) << ": " << fit.error().to_string();
    ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << to_string(kind) << ": "
                                                             << fit->fit_error;
    ASSERT_TRUE(fit->curve);
    EXPECT_EQ(fit->curve->kind(), kind);
    EXPECT_TRUE(fit->prices);
    EXPECT_TRUE(fit->diagnostics.channel.used);
    EXPECT_NEAR(fit->curve->iv(0.0), 0.25, 0.01) << to_string(kind);
    std::size_t inside = 0;
    for (std::size_t i = 0; i < c.quotes.size(); ++i) {
      const auto &row = fit->rows[i];
      ASSERT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success) << i;
      inside += row.fitted_price >= c.quotes[i].bid - 1.0e-9 &&
                row.fitted_price <= c.quotes[i].ask + 1.0e-9;
    }
    EXPECT_GE(inside * 10, c.quotes.size() * 9) << to_string(kind);
  }
}

TEST(QuoteSliceChannel, RejectsAnInterpolatingFamilyAndZeroFamilySteps) {
  const auto c = chain(0.5, 0.25, 0.0, 0.02);
  auto config = channel_config();
  config.channel.family = VolCurveKind::LinearVariance;
  ASSERT_FALSE(fit_quote_slice(c.market, c.quotes, config));
  config.channel.family = VolCurveKind::Essvi;
  config.channel.family_iterations = 0;
  ASSERT_FALSE(fit_quote_slice(c.market, c.quotes, config));
  config.channel.family_iterations = 12;
  EXPECT_TRUE(fit_quote_slice(c.market, c.quotes, config));
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

TEST(QuoteSurface, BoundedCalendarRepairAdmitsASmallCrossing) {
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
  expect_moved_nodes_reimplied(fixed.fit, chains[1].quotes);
}

// Every PriceOnly row carries its certified price and no volatility, its node
// side is flagged in price_only_nodes, and the library query refuses the same
// contract; prices_produced counts Success and PriceOnly rows. Returns the
// number of PriceOnly rows.
std::size_t expect_price_only_rows(const QuoteSliceFit &fit,
                                   std::span<const QuoteSliceQuote> quotes) {
  if (!fit.prices) {
    ADD_FAILURE() << "no certified prices";
    return 0;
  }
  const auto nodes = fit.prices->nodes();
  std::size_t price_only = 0;
  std::size_t priced = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &row = fit.rows[i];
    priced += row.evaluation_status == QuoteSliceEvaluationStatus::Success;
    if (row.evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) continue;
    ++price_only;
    ++priced;
    EXPECT_TRUE(std::isnan(row.fitted_iv)) << i;
    const auto certified = fit.prices->price(quotes[i].strike, quotes[i].side);
    EXPECT_TRUE(certified) << i;
    if (certified) {
      EXPECT_EQ(row.fitted_price, *certified) << i;
    }
    const auto at = std::find_if(nodes.begin(), nodes.end(), [&](const AmericanPriceNode &n) {
      return n.strike == quotes[i].strike;
    });
    const auto node = static_cast<std::size_t>(at - nodes.begin());
    const unsigned bit = quotes[i].side == Side::Call ? 1u : 2u;
    EXPECT_TRUE(at != nodes.end() && node < fit.price_only_nodes.size() &&
                (fit.price_only_nodes[node] & bit) != 0u)
        << i << ": the node side is not flagged price-only";
    EXPECT_FALSE(evaluate_quote_slice(fit, quotes[i].strike, quotes[i].side)) << i;
  }
  EXPECT_EQ(fit.diagnostics.prices_produced, priced);
  return price_only;
}

TEST(QuoteSurface, MovedNodeWithoutAnIdentifiableVolatilityIsNeverServedWithTheCurveVol) {
  // A calendar floor at the certificate's upper edge (call S - 1e-4 K, put
  // K (1 - 2e-4): linear, so convex, and inside every price bound) lifts every
  // node above any finite-volatility American price: the served prices have no
  // identifiable volatility, so the rows must not keep the curve's volatility.
  // They are PriceOnly: the certified price, a NaN volatility.
  const auto c = chain(0.5, 0.25, 0.0, 0.01, 80, 120, 5);
  auto fit = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  std::vector<AmericanPriceNode> lifted;
  for (const auto &node : fit->prices->nodes()) {
    lifted.push_back({node.strike, 100.0 - 1.0e-4 * node.strike, node.strike * (1.0 - 2.0e-4)});
  }
  const auto floor = AmericanPriceSlice::create(lifted, fit->prices->bounds());
  ASSERT_TRUE(floor) << floor.error().to_string();
  const std::array<const AmericanPriceSlice *, 1> earlier{&*floor};
  ASSERT_TRUE(repair_quote_slice_calendar(*fit, c.quotes, earlier, {}, 1.0e9));
  EXPECT_GT(expect_price_only_rows(*fit, c.quotes), 0u);
  EXPECT_STREQ(to_string(QuoteSliceEvaluationStatus::PriceOnly), "price_only");
}

// The fit's node prices shifted by `shift` on both sides (a constant shift keeps
// convexity, slopes and the call-minus-put spread), certified on the fit's bounds.
AmericanPriceSlice shifted_slice(const QuoteSliceFit &fit, double shift) {
  std::vector<AmericanPriceNode> nodes;
  for (const auto &node : fit.prices->nodes()) {
    nodes.push_back({node.strike, node.call + shift, node.put + shift});
  }
  auto slice = AmericanPriceSlice::create(nodes, fit.prices->bounds());
  if (!slice) {
    ADD_FAILURE() << slice.error().to_string();
    return *fit.prices;
  }
  return std::move(*slice);
}

// A carry zig-zag far wider than the quotes' noise on deep in-the-money call
// strikes (64-88 every 4, volatility 0.15; 4% yield at the end strikes and every
// other one, zero at the others). A deep high-yield call is exercised (worth
// about its intrinsic S - K), a zero-yield call is worth at least S - K e^{-rT}:
// above the chord of its two high-yield neighbours, so the greatest convex
// minorant lowers it below its own zero-volatility American value. Both sides
// stay monotone (the strike step outweighs the yield's forward shift), and the
// hull keeps both high-yield end nodes, so the repaired slopes stay in bounds.
Chain wide_zigzag_chain() {
  Chain out;
  out.market = QuoteSliceMarket{100.0, 0.5, 0.5, 0.03, 0.0, 0.0};
  for (int strike = 64; strike <= 88; strike += 4) {
    const double q = ((strike - 64) / 4) % 2 == 0 ? 0.04 : 0.0;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price =
          andersen_lake(100.0, static_cast<double>(strike), 0.5, 0.15, 0.03, q, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * 0.05);
      out.quotes.push_back({static_cast<double>(strike), side, std::max(*price - spread, 0.0),
                            *price + spread, q});
    }
  }
  return out;
}

QuoteSliceConfig unbounded_convexity_repair() {
  auto config = channel_config();
  config.channel.repair_half_spreads = 1.0e6; // accept any move
  return config;
}

TEST(QuoteSliceChannel, ConvexityRepairBelowTheZeroVolatilityValueServesPriceOnlyRows) {
  const auto c = wide_zigzag_chain();
  auto config = unbounded_convexity_repair();
  const auto fit = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  EXPECT_GT(fit->diagnostics.channel.repaired_nodes, 0u);
  ASSERT_FALSE(fit->moved_nodes.empty());
  EXPECT_GT(expect_price_only_rows(*fit, c.quotes), 0u);
  // Every PriceOnly row sits at a node the convexity repair moved.
  const auto nodes = fit->prices->nodes();
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (fit->rows[i].evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) continue;
    const auto at = std::find_if(nodes.begin(), nodes.end(), [&](const AmericanPriceNode &n) {
      return n.strike == c.quotes[i].strike;
    });
    ASSERT_NE(at, nodes.end());
    const unsigned bit = c.quotes[i].side == Side::Call ? 1u : 2u;
    EXPECT_NE(fit->moved_nodes[static_cast<std::size_t>(at - nodes.begin())] & bit, 0u) << i;
  }
  // Without the repair the same slice fails its certificate.
  config.channel.repair_half_spreads = 0.0;
  const auto refused = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->fit_status, QuoteSliceFitStatus::ArbitrageFailure);
}

TEST(QuoteSliceChannel, CalendarRepairNeverTurnsAPriceOnlyRowIntoAVolatilityRow) {
  // The convexity-repaired PriceOnly slice above, then a calendar floor 1.5
  // above every node (a constant shift keeps the floor certified): the
  // calendar repair lifts every node, the PriceOnly ones included, to prices
  // that do have an identifiable volatility. PriceOnly is final: each such row
  // keeps PriceOnly with its refreshed certified price and no volatility, and
  // evaluate_quote_slice keeps refusing it, so a price-only outcome is never
  // served with a volatility (the bench reports it ok_price_only or refuses it).
  const auto c = wide_zigzag_chain();
  auto fit = fit_quote_slice(c.market, c.quotes, unbounded_convexity_repair());
  ASSERT_TRUE(fit && fit->fit_status == QuoteSliceFitStatus::Success);
  std::vector<std::size_t> price_only;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (fit->rows[i].evaluation_status == QuoteSliceEvaluationStatus::PriceOnly) {
      price_only.push_back(i);
    }
  }
  ASSERT_FALSE(price_only.empty());
  const std::vector<std::uint8_t> flagged = fit->price_only_nodes;
  const AmericanPriceSlice floor = shifted_slice(*fit, 1.5);
  const std::array<const AmericanPriceSlice *, 1> earlier{&floor};
  ASSERT_TRUE(repair_quote_slice_calendar(*fit, c.quotes, earlier, {}, 1.0e9));
  std::size_t identifiable = 0;
  for (const std::size_t i : price_only) {
    const auto &quote = c.quotes[i];
    const auto &row = fit->rows[i];
    const auto certified = fit->prices->price(quote.strike, quote.side);
    ASSERT_TRUE(certified);
    EXPECT_EQ(row.fitted_price, *certified) << i; // refreshed
    EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::PriceOnly)
        << i << ": " << to_string(row.evaluation_status) << " iv " << row.fitted_iv;
    EXPECT_TRUE(std::isnan(row.fitted_iv)) << i;
    EXPECT_FALSE(evaluate_quote_slice(*fit, quote.strike, quote.side)) << i;
    // The lifted price itself is invertible: re-implying it would vol-serve the row.
    const auto iv = american_implied_vol(*certified, c.market.spot, quote.strike,
                                         c.market.variance_years, 0.03, *quote.continuous_yield,
                                         quote.side);
    identifiable += iv && *iv > 0.0;
  }
  EXPECT_GT(identifiable, 0u);
  // The flags only accumulate: every node side flagged before still is.
  ASSERT_EQ(fit->price_only_nodes.size(), flagged.size());
  for (std::size_t n = 0; n < flagged.size(); ++n) {
    EXPECT_EQ(fit->price_only_nodes[n] & flagged[n], flagged[n]) << n;
  }
  EXPECT_EQ(expect_price_only_rows(*fit, c.quotes), price_only.size());
}

// Deep in-the-money puts only, zero rate (no early exercise, so these American
// prices are the European ones), flat 15% volatility, strikes 100-148 every 4
// with a per-strike yield of 0 at even positions (both ends included) and 4%
// between. A 4% put is worth about K - S e^{-qT}, above the chord of its zero-
// yield neighbours (about K - S), so the convexity repair lowers it below its
// own zero-volatility value: PriceOnly rows (the bench CLI fixture, in C++).
// `lo` and `step` set the strikes; `yield` (when set) replaces the zig-zag.
Chain put_zigzag_chain(double years, double sigma, int step = 4,
                       std::optional<double> yield = std::nullopt, int lo = 100) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, years, years, 0.0, 0.0, 0.0};
  for (int strike = lo; strike <= 148; strike += step) {
    const double q = yield ? *yield : (((strike - lo) / step) % 2 == 1 ? 0.04 : 0.0);
    const auto price =
        andersen_lake(100.0, static_cast<double>(strike), years, sigma, 0.0, q, Side::Put);
    if (!price) {
      ADD_FAILURE() << price.error().to_string();
      return out;
    }
    const double spread = std::max(0.01, *price * 0.05);
    out.quotes.push_back({static_cast<double>(strike), Side::Put, std::max(*price - spread, 0.0),
                          *price + spread, q});
  }
  return out;
}

std::size_t count_price_only(const QuoteSliceFit &fit) {
  return static_cast<std::size_t>(
      std::count_if(fit.rows.begin(), fit.rows.end(), [](const QuoteSliceRow &row) {
        return row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly;
      }));
}

TEST(QuoteSurface, PriceOnlyRowsSurviveTheSurfaceCalendarRepairAndAreStrippedWhenRefused) {
  // Two put zig-zag expiries (no point evidence: both are served by channel
  // refits whose convexity repair leaves PriceOnly rows). The nearer one is
  // quoted at a higher volatility, so it lies above the later one: a calendar
  // crossing.
  const std::vector<Chain> chains{put_zigzag_chain(0.45, 0.2), put_zigzag_chain(0.5, 0.15)};
  QuoteSliceConfig config;
  config.channel.repair_half_spreads = 100.0;
  // With the bounded calendar repair, the lower-confidence slice is lifted onto
  // the other through a repair trial: its PriceOnly rows stay PriceOnly, with
  // their flags, refreshed prices and refused queries.
  QuoteSurfaceConfig repairing;
  repairing.calendar_repair_half_spreads = 100.0;
  const auto repaired = fit_quote_surface(expiries(chains), config, repairing);
  ASSERT_TRUE(repaired) << repaired.error().to_string();
  std::size_t lifted = 0;
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const auto &slice = (*repaired)[e];
    ASSERT_NE(slice.grade, QuoteSliceGrade::Refused) << e << " " << slice.refusal << " "
                                                     << slice.error;
    EXPECT_TRUE(slice.channel_refit) << e;
    EXPECT_GT(expect_price_only_rows(slice.fit, chains[e].quotes), 0u) << e;
    lifted += slice.calendar_repaired;
  }
  EXPECT_EQ(lifted, 1u);
  expect_calendar_consistent(*repaired);
  // Without it the lower-confidence slice is refused, and a refused slice
  // carries no price: its PriceOnly rows become NoFit like its Success rows.
  const auto refused = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(refused) << refused.error().to_string();
  std::size_t stripped = 0;
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const auto &slice = (*refused)[e];
    if (slice.grade != QuoteSliceGrade::Refused) {
      EXPECT_GT(expect_price_only_rows(slice.fit, chains[e].quotes), 0u) << e;
      continue;
    }
    ++stripped;
    EXPECT_EQ(slice.refusal, "calendar_arbitrage") << e;
    EXPECT_FALSE(slice.fit.prices);
    EXPECT_EQ(count_price_only(slice.fit), 0u) << e;
    for (const auto &row : slice.fit.rows) {
      EXPECT_TRUE(std::isnan(row.fitted_price));
      EXPECT_TRUE(std::isnan(row.fitted_iv));
      EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::NoFit)
          << to_string(row.evaluation_status);
    }
  }
  EXPECT_EQ(stripped, 1u);
}

TEST(QuoteSurface, ASurfaceCalendarRepairThatLeavesNoVolatilityFlagsTheRowPriceOnly) {
  // N: puts at a 4% yield everywhere (0.45 y, 15%; zero-volatility value about
  // K - 98.2), 9 strikes 116-148 every 4. F: later zero-yield puts at 5% from
  // 92 to 148 every 4 (10 mid strikes), worth about K - 100: below N's zero-
  // volatility value. With 10 mid strikes required for B, N is C and F is B, so
  // F ranks first and N crosses it; the bounded calendar repair (inside the
  // surface's repair trial) caps N's nodes onto F's prices (F's knots are N's
  // nodes there), where they have no identifiable volatility: rows that were
  // Success become PriceOnly, flagged, and refused by the query, as after a
  // convexity repair.
  const std::vector<Chain> chains{put_zigzag_chain(0.45, 0.15, 4, 0.04, 116),
                                  put_zigzag_chain(0.5, 0.05, 4, 0.0, 92)};
  QuoteSliceConfig config;
  config.channel.repair_half_spreads = 100.0;
  QuoteSurfaceConfig surface;
  surface.grade_b_min_mid_strikes = 10;
  const auto clean = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(clean) << clean.error().to_string();
  ASSERT_EQ((*clean)[0].refusal, "calendar_arbitrage") << (*clean)[0].error;
  ASSERT_EQ((*clean)[1].grade, QuoteSliceGrade::B) << (*clean)[1].refusal;
  QuoteSurfaceConfig repairing = surface;
  repairing.calendar_repair_half_spreads = 100.0;
  const auto repaired = fit_quote_surface(expiries(chains), config, repairing);
  ASSERT_TRUE(repaired) << repaired.error().to_string();
  const auto &near = (*repaired)[0];
  ASSERT_NE(near.grade, QuoteSliceGrade::Refused)
      << near.refusal << " " << near.error << " required " << near.calendar_repair_required
      << " gap " << near.calendar_gap << " at " << near.calendar_gap_strike;
  EXPECT_TRUE(near.calendar_repaired);
  EXPECT_EQ(count_price_only(near.fit), expect_price_only_rows(near.fit, chains[0].quotes));
  EXPECT_GT(count_price_only(near.fit), 0u);
  expect_calendar_consistent(*repaired);
}

TEST(QuoteSliceChannel, CalendarRepairFlagsAreOredIntoConvexityRepairFlags) {
  // A convexity-repaired knot slice (mild carry zig-zag), then a calendar floor
  // only over strikes >= 100, 0.002 above them: the calendar repair moves those
  // nodes, and the moved-node mask is the union of both repairs.
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05);
  auto config = channel_config();
  config.channel.repair_half_spreads = 20.0;
  auto fit = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(fit && fit->fit_status == QuoteSliceFitStatus::Success);
  ASSERT_FALSE(fit->moved_nodes.empty());
  const std::vector<std::uint8_t> convexity = fit->moved_nodes;
  const auto before = fit->prices->nodes();
  const std::vector<AmericanPriceNode> original(before.begin(), before.end());
  std::vector<AmericanPriceNode> upper;
  for (const auto &node : original) {
    if (node.strike >= 100.0) upper.push_back({node.strike, node.call + 0.002, node.put + 0.002});
  }
  auto floor = AmericanPriceSlice::create(upper, fit->prices->bounds());
  ASSERT_TRUE(floor) << floor.error().to_string();
  const std::array<const AmericanPriceSlice *, 1> earlier{&*floor};
  ASSERT_TRUE(repair_quote_slice_calendar(*fit, c.quotes, earlier, {}, 1.0e9));
  const auto after = fit->prices->nodes();
  ASSERT_EQ(fit->moved_nodes.size(), original.size());
  std::size_t added = 0;
  for (std::size_t n = 0; n < original.size(); ++n) {
    const std::uint8_t calendar = static_cast<std::uint8_t>(
        (after[n].call != original[n].call ? 1u : 0u) | (after[n].put != original[n].put ? 2u : 0u));
    EXPECT_EQ(fit->moved_nodes[n], convexity[n] | calendar) << n;
    added += (calendar & ~convexity[n]) != 0u;
  }
  EXPECT_GT(added, 0u);
  std::size_t convexity_only = 0;
  for (std::size_t n = 0; n < original.size(); ++n) {
    convexity_only += convexity[n] != 0u && original[n].strike < 100.0;
  }
  EXPECT_GT(convexity_only, 0u); // flags the calendar repair did not set survive
  expect_moved_nodes_reimplied(*fit, c.quotes);
}

TEST(QuoteSurface, KnotSplineProfileGradesAOnlyWithOwnEvidenceAndUnmovedPrices) {
  // A knot-spline profile's pass A is its term-prior-free channel fit. Well-quoted
  // expiries meet the point-evidence minimum and keep their fitted prices: A.
  auto config = channel_config();
  config.channel.repair_half_spreads = 20.0;
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  const auto surface = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(surface) << surface.error().to_string();
  for (const auto &slice : *surface) {
    ASSERT_EQ(slice.grade, QuoteSliceGrade::A) << slice.refusal << " " << slice.error;
    EXPECT_FALSE(slice.channel_refit);
    EXPECT_EQ(slice.fit.curve->kind(), VolCurveKind::SplineVol);
    EXPECT_TRUE(slice.fit.diagnostics.channel.used);
    EXPECT_FALSE(slice.fit.diagnostics.channel.term_prior);
    EXPECT_TRUE(slice.fit.moved_nodes.empty());
  }
  expect_calendar_consistent(*surface);
  // Same evidence, but the served prices had to be moved by the bounded
  // convexity repair: the slice is served from pass A and graded B.
  const std::vector<Chain> zig{zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05)};
  const auto moved = fit_quote_surface(expiries(zig), config);
  ASSERT_TRUE(moved) << moved.error().to_string();
  const auto &slice = (*moved)[0];
  EXPECT_EQ(slice.grade, QuoteSliceGrade::B) << slice.refusal << " " << slice.error;
  EXPECT_FALSE(slice.channel_refit);
  EXPECT_GE(slice.fit.diagnostics.distinct_strikes, config.min_quotes);
  EXPECT_GT(slice.fit.diagnostics.channel.repaired_nodes, 0u);
  EXPECT_FALSE(slice.fit.moved_nodes.empty());
  expect_moved_nodes_reimplied(slice.fit, zig[0].quotes);
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
  // A malformed channel configuration is shared: Err, not a per-expiry refusal.
  for (const auto mutate : {+[](QuoteSliceChannelConfig &c) { c.mult_ceil = c.mult_floor; },
                            +[](QuoteSliceChannelConfig &c) { c.channel_weight = 0.0; }}) {
    QuoteSliceConfig channel_bad;
    mutate(channel_bad.channel);
    EXPECT_FALSE(validate_quote_slice_channel(channel_bad.channel));
    EXPECT_EQ(fit_quote_surface(expiries(one), channel_bad).error().code(),
              ErrorCode::InvalidArgument);
  }
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

// ── Opt-in channel repair and channel-dominance gate ─────────────────────────

double channel_distance(double price, const QuoteSliceQuote &quote) {
  return std::max({quote.bid - price, price - quote.ask, 0.0});
}

QuoteSliceConfig channel_repair_config(std::size_t max_nodes = 64) {
  QuoteSliceConfig config;
  config.repair.mode = QuoteSliceRepairMode::Channel;
  config.repair.max_nodes = max_nodes;
  return config;
}

TEST(QuoteSliceChannel, RepairIsOptInAndServesAMicroKinkedPointFitInsideItsChannels) {
  // Per-strike carry zig-zag: the point fit's repriced American prices are
  // locally concave by much less than a half-spread, so its certificate fails.
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05);
  const auto off = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ(off->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << off->fit_error;
  EXPECT_FALSE(off->prices);
  EXPECT_EQ(off->diagnostics.repair.outcome, QuoteSliceRepairOutcome::NotAttempted);
  const auto config = channel_repair_config();
  const auto on = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(on) << on.error().to_string();
  const auto &record = on->diagnostics.repair;
  ASSERT_EQ(on->fit_status, QuoteSliceFitStatus::Success)
      << on->fit_error << " repair " << to_string(record.outcome);
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(record.nodes, 0u);
  EXPECT_LE(record.material_nodes, config.repair.max_nodes);
  EXPECT_GT(record.max_price_move, 0.0);
  EXPECT_LE(record.max_unquoted_vol_move, config.repair.max_vol_move);
  ASSERT_TRUE(on->prices);
  EXPECT_TRUE(AmericanPriceSlice::create(on->prices->nodes(), on->prices->bounds()));
  expect_moved_nodes_reimplied(*on, c.quotes);
  // No moved price ends farther outside its own quote than the curve put it.
  const auto nodes = on->prices->nodes();
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const auto &quote = c.quotes[i];
    const auto at = std::find_if(nodes.begin(), nodes.end(), [&](const AmericanPriceNode &n) {
      return n.strike == quote.strike;
    });
    ASSERT_NE(at, nodes.end());
    const unsigned bit = quote.side == Side::Call ? 1u : 2u;
    if ((on->moved_nodes[static_cast<std::size_t>(at - nodes.begin())] & bit) == 0u) continue;
    const double vol = on->curve->iv(std::log(quote.strike / on->diagnostics.forward));
    const auto curve_price =
        andersen_lake(c.market.spot, quote.strike, c.market.variance_years, vol,
                      on->diagnostics.effective_rate, *quote.continuous_yield, quote.side);
    ASSERT_TRUE(curve_price);
    EXPECT_LE(channel_distance(on->rows[i].fitted_price, quote),
              channel_distance(*curve_price, quote) + 1.0e-4)
        << i;
  }
}

TEST(QuoteSliceChannel, RepairRefusesBeyondItsNodeBudget) {
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05);
  auto config = channel_repair_config(1);
  config.repair.immaterial_price_move = 0.0; // every move counts
  const auto fit = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->fit_status, QuoteSliceFitStatus::ArbitrageFailure);
  EXPECT_FALSE(fit->prices);
  EXPECT_EQ(fit->diagnostics.repair.outcome, QuoteSliceRepairOutcome::NodeBudget);
  EXPECT_GT(fit->diagnostics.repair.material_nodes, 1u);
}

TEST(QuoteSliceChannel, RepairRefusesWhenNoConvexPriceSetFitsInsideTheQuotes) {
  // A carry zig-zag far wider than the (one-cent) quotes: no convex price set
  // lies inside the channels, so the repair refuses rather than leave them.
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.005, 0.015, 0.0);
  const auto fit = fit_quote_slice(c.market, c.quotes, channel_repair_config(1000));
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << fit->fit_error;
  EXPECT_FALSE(fit->prices);
  // The lowering would leave the one-cent channels and the projection has no
  // candidate, so the record keeps the lowering's refusal.
  EXPECT_EQ(fit->diagnostics.repair.outcome, QuoteSliceRepairOutcome::LeftChannel)
      << to_string(fit->diagnostics.repair.outcome);
}

// ── Opt-in box certificate (lane L3r1 dense-certificate, 2026-10-20) ────────

QuoteSliceConfig box_certificate_config(double half_ticks = 0.5) {
  QuoteSliceConfig config;
  config.repair.certificate = QuoteSliceCertificate::Box;
  config.repair.box_half_ticks = half_ticks;
  return config;
}

TEST(QuoteSliceBoxCertificate, ServesAMicroKinkedPointFitInsideTheWidenedQuotes) {
  // The same micro-kinked zig-zag the exact certificate refuses (above). Under
  // the box certificate the fitted prices are projected into every quote's
  // [bid - h, ask + h] and the UNCHANGED certificate passes the projected
  // curve; there is no node budget, the channel repair stays off.
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05);
  const auto exact = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(exact) << exact.error().to_string();
  ASSERT_EQ(exact->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << exact->fit_error;
  EXPECT_EQ(exact->diagnostics.box.outcome, QuoteSliceRepairOutcome::NotAttempted);
  const auto config = box_certificate_config();
  const auto on = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(on) << on.error().to_string();
  const auto &record = on->diagnostics.box;
  ASSERT_EQ(on->fit_status, QuoteSliceFitStatus::Success)
      << on->fit_error << " box " << to_string(record.outcome);
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(record.nodes, 0u);
  EXPECT_GT(record.max_price_move, 0.0);
  EXPECT_EQ(on->diagnostics.repair.outcome, QuoteSliceRepairOutcome::NotAttempted);
  ASSERT_TRUE(on->prices);
  EXPECT_TRUE(AmericanPriceSlice::create(on->prices->nodes(), on->prices->bounds()));
  EXPECT_FALSE(on->moved_nodes.empty());
  expect_moved_nodes_reimplied(*on, c.quotes);
  // Every served price lies inside its own quote widened by h on each side.
  const double h = config.repair.box_half_ticks * config.repair.box_tick;
  std::size_t served = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const auto &row = on->rows[i];
    if (row.evaluation_status != QuoteSliceEvaluationStatus::Success &&
        row.evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) {
      continue;
    }
    ++served;
    EXPECT_GE(row.fitted_price, c.quotes[i].bid - h) << i;
    EXPECT_LE(row.fitted_price, c.quotes[i].ask + h) << i;
  }
  EXPECT_EQ(served, c.quotes.size());
  // No widening: the market's exact [bid, ask] boxes still hold a curve.
  const auto tight = fit_quote_slice(c.market, c.quotes, box_certificate_config(0.0));
  ASSERT_TRUE(tight) << tight.error().to_string();
  EXPECT_EQ(tight->fit_status, QuoteSliceFitStatus::Success) << tight->fit_error;
}

TEST(QuoteSliceBoxCertificate, RefusesWhenNoArbitrageFreeCurveFitsInsideTheQuotes) {
  // The carry zig-zag far wider than the one-cent quotes: no convex curve lies
  // inside the boxes even widened by half a tick, so the slice stays refused
  // and the record says why.
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.005, 0.015, 0.0);
  const auto fit = fit_quote_slice(c.market, c.quotes, box_certificate_config());
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << fit->fit_error;
  EXPECT_FALSE(fit->prices);
  EXPECT_EQ(fit->diagnostics.box.outcome, QuoteSliceRepairOutcome::Infeasible)
      << to_string(fit->diagnostics.box.outcome);
  // The exact certificate's error is what the refusal reports.
  EXPECT_NE(fit->fit_error.find("AmericanPriceSlice"), std::string::npos) << fit->fit_error;
}

TEST(QuoteSurface, BoxCertificateServesThePassAFitAsGradeBWithoutARepairRung) {
  // Under graded admission the box certificate acts at pass A (it is the
  // certificate, not a fallback rung): the micro-kinked expiry serves from its
  // own point fit, relabelled B because its served prices left the fit.
  std::vector<Chain> one{zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05)};
  const auto exact = fit_quote_surface(expiries(one), QuoteSliceConfig{});
  ASSERT_TRUE(exact) << exact.error().to_string();
  ASSERT_EQ(exact->size(), 1u);
  EXPECT_EQ((*exact)[0].grade, QuoteSliceGrade::Refused);
  EXPECT_EQ((*exact)[0].refusal, "arbitrage_failure");
  const auto box = fit_quote_surface(expiries(one), box_certificate_config());
  ASSERT_TRUE(box) << box.error().to_string();
  ASSERT_EQ(box->size(), 1u);
  const QuoteSurfaceSlice &slice = (*box)[0];
  EXPECT_TRUE(slice.refusal.empty()) << slice.refusal << ": " << slice.error;
  EXPECT_EQ(slice.grade, QuoteSliceGrade::B);
  EXPECT_FALSE(slice.channel_refit);
  EXPECT_FALSE(slice.repair_rung);
  EXPECT_EQ(slice.fit.diagnostics.box.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_FALSE(slice.fit.moved_nodes.empty());
}

TEST(QuoteSliceBoxCertificate, ConfigurationIsValidatedAtTheSharedBoundary) {
  QuoteSliceRepairConfig config;
  EXPECT_TRUE(validate_quote_slice_repair(config));
  config.certificate = QuoteSliceCertificate::Box;
  EXPECT_TRUE(validate_quote_slice_repair(config));
  config.box_half_ticks = -0.5;
  EXPECT_FALSE(validate_quote_slice_repair(config));
  config.box_half_ticks = 0.0;
  config.box_tick = 0.0;
  EXPECT_FALSE(validate_quote_slice_repair(config));
  config.box_tick = 0.01;
  config.certificate = static_cast<QuoteSliceCertificate>(9);
  EXPECT_FALSE(validate_quote_slice_repair(config));
}

// ── Opt-in market wing ceiling (lane L3r1 lever 3, 2026-10-21) ──────────────

// American quotes (three-month, 2 % relative half-spread, one-cent floor) on an
// arbitrage-free put wing that saturates far from the money:
// sigma(k) = atm + amplitude * (1 - exp(k / scale)) for k < 0, flat atm above.
// Its far-wing multiple approaches (atm + amplitude) / atm while the slope stays
// mild where the wing is far out of the money.
Chain wing_chain(double atm, double amplitude, double scale) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, 0.25, 0.25, 0.03, 0.01, 0.0};
  for (int strike = 60; strike <= 120; strike += 5) {
    const double k = std::log(static_cast<double>(strike) / 100.0);
    const double sigma = k < 0.0 ? atm + amplitude * (1.0 - std::exp(k / scale)) : atm;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price =
          andersen_lake(100.0, static_cast<double>(strike), 0.25, sigma, 0.03, 0.01, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * 0.02);
      out.quotes.push_back(
          {static_cast<double>(strike), side, std::max(*price - spread, 0.0), *price + spread});
    }
  }
  return out;
}

// The 60 put: the outermost (two-sided) strike of the put wing.
std::size_t outermost_put(const Chain &c) {
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (c.quotes[i].side == Side::Put && c.quotes[i].strike == 60.0) return i;
  }
  ADD_FAILURE() << "no 60 put";
  return 0;
}

TEST(QuoteSliceChannel, MarketWingCeilingServesAWingTheThreeTimesAtmClampCuts) {
  // At K = 60 the market volatility is ~4 x ATM. The policy ceiling (mult_ceil
  // 3) clamps the served knot spline onto 3 x ATM from K = 75 outwards; the
  // clamp's concave kink fails the price certificate (convexity), so the whole
  // slice is refused. The market ceiling lifts it to 1.1 x the market's own
  // outermost two-sided multiple: the slice certifies and every put of the wing
  // is served inside its quote.
  const auto c = wing_chain(0.12, 0.45, 0.3);
  auto config = channel_config();
  const auto policy = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(policy) << policy.error().to_string();
  EXPECT_EQ(policy->diagnostics.channel.wing_mult_cap, 0.0);
  EXPECT_EQ(policy->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << policy->fit_error;
  EXPECT_NE(policy->fit_error.find("convexity"), std::string::npos) << policy->fit_error;
  config.repair.wing_mult_source = QuoteSliceWingMultSource::Market;
  const auto market = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(market) << market.error().to_string();
  ASSERT_EQ(market->fit_status, QuoteSliceFitStatus::Success) << market->fit_error;
  const double cap = market->diagnostics.channel.wing_mult_cap;
  EXPECT_GT(cap, 4.0);
  EXPECT_LT(cap, kQuoteSliceMarketWingMultMax);
  std::size_t wing_puts = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (c.quotes[i].side != Side::Put || c.quotes[i].strike > 80.0) continue;
    const auto &row = market->rows[i];
    ASSERT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success) << i;
    EXPECT_GE(row.fitted_price, c.quotes[i].bid - 1.0e-9) << i;
    EXPECT_LE(row.fitted_price, c.quotes[i].ask + 1.0e-9) << i;
    ++wing_puts;
  }
  EXPECT_EQ(wing_puts, 5u);
  EXPECT_GT(market->rows[outermost_put(c)].fitted_iv, 3.0 * 0.12);
}

TEST(QuoteSliceChannel, MarketWingCeilingIsBoundedAndNeverBelowThePolicyCeiling) {
  // ~5.8 x ATM at K = 60 (x 1.1 > 6): the market ceiling stops at
  // kQuoteSliceMarketWingMultMax.
  const auto c = wing_chain(0.12, 0.8, 0.4);
  auto config = channel_config();
  config.repair.wing_mult_source = QuoteSliceWingMultSource::Market;
  const auto bounded = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(bounded) << bounded.error().to_string();
  EXPECT_EQ(bounded->diagnostics.channel.wing_mult_cap, kQuoteSliceMarketWingMultMax);
  // A policy ceiling above the bound is kept: the market source only raises it.
  config.channel.mult_ceil = 8.0;
  const auto kept = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(kept) << kept.error().to_string();
  EXPECT_EQ(kept->diagnostics.channel.wing_mult_cap, 8.0);
  QuoteSliceRepairConfig invalid;
  invalid.wing_mult_source = static_cast<QuoteSliceWingMultSource>(7);
  EXPECT_FALSE(validate_quote_slice_repair(invalid));
}

TEST(QuoteSliceChannel, ChannelCalendarRepairLiftsACrossingInsideTheQuotes) {
  // An accepted earlier slice 0.2 cent above every node: a crossing inside
  // every channel (half-spreads >= 1 cent) but beyond 0.1 half-spreads.
  const auto c = chain(0.5, 0.25, 0.0, 0.05, 80, 120, 5);
  const auto fitted = [&] {
    auto fit = fit_quote_slice(c.market, c.quotes);
    EXPECT_TRUE(fit && fit->fit_status == QuoteSliceFitStatus::Success);
    return std::move(*fit);
  };
  auto bounded = fitted();
  const AmericanPriceSlice floor = shifted_slice(bounded, 0.002);
  const std::array<const AmericanPriceSlice *, 1> earlier{&floor};
  double required = 0.0;
  EXPECT_FALSE(repair_quote_slice_calendar(bounded, c.quotes, earlier, {}, 0.01, &required));
  EXPECT_GT(required, 0.01);
  // Node budget: every strike moves materially.
  auto tight = fitted();
  QuoteSliceRepairConfig budget;
  budget.max_nodes = 2;
  QuoteSliceRepairRecord record;
  EXPECT_FALSE(repair_quote_slice_calendar_in_channel(tight, c.quotes, earlier, {}, budget,
                                                      &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::NodeBudget);
  EXPECT_TRUE(tight.moved_nodes.empty());
  // Within budget: lifted exactly onto the floor, certified, inside the quotes.
  auto fit = fitted();
  budget.max_nodes = 100;
  ASSERT_TRUE(repair_quote_slice_calendar_in_channel(fit, c.quotes, earlier, {}, budget, &record))
      << to_string(record.outcome);
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_EQ(record.nodes, floor.nodes().size());
  EXPECT_NEAR(record.max_price_move, 0.002, 1.0e-9);
  EXPECT_TRUE(check_american_calendar(floor, *fit.prices));
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    EXPECT_GE(fit.rows[i].fitted_price, c.quotes[i].bid) << i;
    EXPECT_LE(fit.rows[i].fitted_price, c.quotes[i].ask) << i;
  }
  expect_moved_nodes_reimplied(fit, c.quotes);
  // A floor above every ask cannot be met inside the channels.
  auto far = fitted();
  const AmericanPriceSlice high = shifted_slice(far, 5.0);
  const std::array<const AmericanPriceSlice *, 1> above{&high};
  EXPECT_FALSE(repair_quote_slice_calendar_in_channel(far, c.quotes, above, {}, budget, &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Infeasible);
  EXPECT_TRUE(far.moved_nodes.empty());
}

TEST(QuoteSliceBoxCertificate, BoxCalendarRepairLiftsACrossingInsideTheQuotesWithoutABudget) {
  // The same 0.2-cent crossing as above. Under the box certificate the calendar
  // repair projects into the quote boxes (here h = 0: the exact [bid, ask]) and
  // has no node budget: max_nodes 2 refuses the channel repair above, the box
  // repair serves, every price inside its quote. A floor above every ask is
  // outside the quotes: infeasible.
  const auto c = chain(0.5, 0.25, 0.0, 0.05, 80, 120, 5);
  const auto fitted = [&] {
    auto fit = fit_quote_slice(c.market, c.quotes);
    EXPECT_TRUE(fit && fit->fit_status == QuoteSliceFitStatus::Success);
    return std::move(*fit);
  };
  auto fit = fitted();
  const AmericanPriceSlice floor = shifted_slice(fit, 0.002);
  const std::array<const AmericanPriceSlice *, 1> earlier{&floor};
  QuoteSliceRepairConfig budget; // mode Off: the box certificate alone opens the repair
  budget.max_nodes = 2;
  budget.certificate = QuoteSliceCertificate::Box;
  budget.box_half_ticks = 0.0;
  QuoteSliceRepairRecord record;
  ASSERT_TRUE(repair_quote_slice_calendar_in_channel(fit, c.quotes, earlier, {}, budget, &record))
      << to_string(record.outcome);
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_EQ(record.nodes, floor.nodes().size());
  EXPECT_GT(record.material_nodes, budget.max_nodes);
  EXPECT_NEAR(record.max_price_move, 0.002, 1.0e-9);
  EXPECT_TRUE(check_american_calendar(floor, *fit.prices));
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    EXPECT_GE(fit.rows[i].fitted_price, c.quotes[i].bid) << i;
    EXPECT_LE(fit.rows[i].fitted_price, c.quotes[i].ask) << i;
  }
  expect_moved_nodes_reimplied(fit, c.quotes);
  auto far = fitted();
  const AmericanPriceSlice high = shifted_slice(far, 5.0);
  const std::array<const AmericanPriceSlice *, 1> above{&high};
  EXPECT_FALSE(repair_quote_slice_calendar_in_channel(far, c.quotes, above, {}, budget, &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Infeasible);
  EXPECT_TRUE(far.moved_nodes.empty());
}

TEST(QuoteSliceChannel, TypedCalendarRepairIdentifiesEachChangedPriceOnlyOnce) {
  // A parallel lift changes every node without changing strike shape. Publishing the
  // moved rows and aligning their public query labels must share the typed inversion.
  const auto c = chain(0.5, 0.25, 0.0, 0.05, 80, 120, 5);
  QuoteSliceConfig config;
  config.floor_policy = QuoteSliceFloorPolicy::PriceOnly;
  auto fit = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(fit && fit->fit_status == QuoteSliceFitStatus::Success);
  const AmericanPriceSlice floor = shifted_slice(*fit, 0.002);
  const std::array<const AmericanPriceSlice *, 1> earlier{&floor};
  namespace timing = counters::fit_timing;
  timing::ChainRecorder repair;
  {
    const timing::ChainScope scope(&repair);
    ASSERT_TRUE(repair_quote_slice_calendar(*fit, c.quotes, earlier, {}, 1.0));
  }
  timing::ChainRecorder queries;
  {
    const timing::ChainScope scope(&queries);
    for (std::size_t i = 0; i < c.quotes.size(); ++i) {
      const auto answer = query_quote_slice(*fit, c.quotes[i].strike, c.quotes[i].side);
      ASSERT_TRUE(answer);
      ASSERT_EQ(answer->status, QuoteSliceEvaluationStatus::Success) << i;
      EXPECT_EQ(answer->volatility, fit->rows[i].fitted_iv) << i;
      EXPECT_EQ(answer->price, fit->rows[i].fitted_price) << i;
      EXPECT_EQ(answer->vol_convention, vol_convention(fit->rows[i])) << i;
    }
  }
  ASSERT_GT(queries.result().iv_newton_iters, 0u);
  EXPECT_EQ(repair.result().iv_newton_iters, queries.result().iv_newton_iters);
  EXPECT_EQ(repair.result().al_price_calls, queries.result().al_price_calls);
  timing::ChainRecorder unchanged;
  {
    const timing::ChainScope scope(&unchanged);
    ASSERT_TRUE(repair_quote_slice_calendar(*fit, c.quotes, earlier, {}, 1.0));
  }
  EXPECT_EQ(unchanged.result().iv_newton_iters, 0u);
  EXPECT_EQ(unchanged.result().al_price_calls, 0u);
}

TEST(QuoteSliceChannel, RejectsAnInvalidRepairOrDominanceConfiguration) {
  const auto c = chain(0.5, 0.25, 0.0, 0.02);
  const std::vector<Chain> one{c};
  for (const auto mutate : {+[](QuoteSliceRepairConfig &r) { r.max_nodes = 0; },
                            +[](QuoteSliceRepairConfig &r) { r.max_vol_move = 0.0; },
                            +[](QuoteSliceRepairConfig &r) { r.max_vol_move = std::nan(""); },
                            +[](QuoteSliceRepairConfig &r) { r.immaterial_price_move = -1.0; },
                            +[](QuoteSliceRepairConfig &r) {
                              r.mode = static_cast<QuoteSliceRepairMode>(7);
                            }}) {
    QuoteSliceConfig config = channel_repair_config();
    mutate(config.repair);
    EXPECT_FALSE(validate_quote_slice_repair(config.repair));
    EXPECT_EQ(fit_quote_slice(c.market, c.quotes, config).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(fit_quote_surface(expiries(one), config).error().code(),
              ErrorCode::InvalidArgument);
  }
  EXPECT_TRUE(validate_quote_slice_repair(channel_repair_config().repair));
  for (const double bad : {-0.1, 1.5, std::nan("")}) {
    QuoteSliceConfig config;
    config.channel.point_max_outside_fraction = bad;
    EXPECT_FALSE(validate_quote_slice_channel(config.channel)) << bad;
    EXPECT_EQ(fit_quote_surface(expiries(one), config).error().code(),
              ErrorCode::InvalidArgument);
  }
  for (const double good : {0.0, 1.0}) {
    QuoteSliceChannelConfig channel;
    channel.point_max_outside_fraction = good;
    EXPECT_TRUE(validate_quote_slice_channel(channel)) << good;
  }
  EXPECT_STREQ(to_string(QuoteSliceRepairOutcome::LeftChannel), "left_channel");
  EXPECT_STREQ(to_string(QuoteSliceRepairOutcome::VolBudget), "vol_budget");
}

TEST(QuoteSurface, ChannelRepairServesAMicroKinkedExpiryAndLeavesTheRestUnchanged) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01),
                                  zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ((*off)[1].refusal, "arbitrage_failure") << to_string((*off)[1].grade);
  const auto on = fit_quote_surface(expiries(chains), channel_repair_config());
  ASSERT_TRUE(on) << on.error().to_string();
  const auto &fixed = (*on)[1];
  ASSERT_NE(fixed.grade, QuoteSliceGrade::Refused) << fixed.refusal << " " << fixed.error;
  EXPECT_NE(fixed.grade, QuoteSliceGrade::A); // a repaired slice is never A
  ASSERT_TRUE(fixed.fit.prices);
  EXPECT_FALSE(fixed.fit.moved_nodes.empty());
  // Served by its channel refit, which the repair certified (not by the rung).
  EXPECT_TRUE(fixed.channel_refit);
  EXPECT_FALSE(fixed.repair_rung);
  EXPECT_EQ(fixed.fit.diagnostics.repair.outcome, QuoteSliceRepairOutcome::Applied);
  expect_moved_nodes_reimplied(fixed.fit, chains[1].quotes);
  expect_calendar_consistent(*on);
  // The well-quoted neighbours certify without the repair: unchanged, still A.
  for (const std::size_t e : {std::size_t{0}, std::size_t{2}}) {
    EXPECT_EQ((*on)[e].grade, QuoteSliceGrade::A);
    EXPECT_FALSE((*on)[e].repair_rung);
    for (std::size_t i = 0; i < chains[e].quotes.size(); ++i) {
      EXPECT_EQ((*on)[e].fit.rows[i].fitted_price, (*off)[e].fit.rows[i].fitted_price);
    }
  }
}

// A put skew that stops at the money (a convex kink in volatility), which the
// smooth point family cannot follow; half-spread relative, one-cent floor.
Chain kinked_chain(double years, double half_spread) {
  Chain out = chain(years, 0.24, -0.08, 0.0);
  for (auto &quote : out.quotes) {
    const double k = std::log(quote.strike / 100.0);
    const double sigma = 0.24 + 0.40 * std::max(-k, 0.0);
    const auto price = andersen_lake(100.0, quote.strike, years, sigma, 0.03, 0.01, quote.side);
    if (!price) {
      ADD_FAILURE() << price.error().to_string();
      return out;
    }
    const double spread = std::max(0.01, *price * half_spread);
    quote.bid = std::max(*price - spread, 0.0);
    quote.ask = *price + spread;
  }
  return out;
}

// One-cent quotes. The outer expiries have a linear skew the eSSVI point fit
// meets everywhere; the middle one is kinked.
std::vector<Chain> tight_chains() {
  return {chain(0.25, 0.22, -0.10, 0.0), kinked_chain(0.5, 0.0), chain(1.0, 0.26, -0.06, 0.0)};
}

std::size_t outside_quotes(const QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes) {
  std::size_t outside = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    outside += !(channel_distance(fit.rows[i].fitted_price, quotes[i]) <= 0.0);
  }
  return outside;
}

TEST(QuoteSurface, DominanceGateRefitsAPointFitOutsideItsQuotes) {
  const auto chains = tight_chains();
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  for (const auto &slice : *off) {
    ASSERT_EQ(slice.grade, QuoteSliceGrade::A) << slice.refusal;
    EXPECT_FALSE(slice.point_gated);
    EXPECT_EQ(slice.point_outside_fraction, 0.0); // not measured without the gate
  }
  // A limit of 1 gates nothing: bit-identical to the gate being off.
  QuoteSliceConfig open;
  open.channel.point_max_outside_fraction = 1.0;
  const auto same = fit_quote_surface(expiries(chains), open);
  ASSERT_TRUE(same) << same.error().to_string();
  QuoteSliceConfig config;
  config.channel.point_max_outside_fraction = 0.0;
  const auto on = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(on) << on.error().to_string();
  std::size_t gated = 0;
  for (std::size_t e = 0; e < chains.size(); ++e) {
    EXPECT_FALSE((*same)[e].point_gated);
    EXPECT_EQ((*same)[e].grade, QuoteSliceGrade::A);
    for (std::size_t i = 0; i < chains[e].quotes.size(); ++i) {
      EXPECT_EQ((*same)[e].fit.rows[i].fitted_price, (*off)[e].fit.rows[i].fitted_price);
    }
    const auto &slice = (*on)[e];
    const std::size_t point_outside = outside_quotes((*off)[e].fit, chains[e].quotes);
    EXPECT_EQ(slice.point_gated, slice.point_outside_fraction > 0.0) << e;
    EXPECT_EQ(slice.point_gated, point_outside > 0) << e;
    EXPECT_EQ(slice.point_outside_quotes, point_outside) << e;
    // The gate runs last and never refuses: every expiry is still served.
    ASSERT_NE(slice.grade, QuoteSliceGrade::Refused) << slice.refusal << " " << slice.error;
    if (!slice.point_gated) {
      // An expiry that is not gated is untouched: same grade, same prices.
      EXPECT_EQ(slice.grade, QuoteSliceGrade::A) << e;
      for (std::size_t i = 0; i < chains[e].quotes.size(); ++i) {
        EXPECT_EQ(slice.fit.rows[i].fitted_price, (*off)[e].fit.rows[i].fitted_price) << e;
      }
      continue;
    }
    ++gated;
    EXPECT_NE(slice.grade, QuoteSliceGrade::A) << e;
    EXPECT_NE(slice.channel_refit, slice.point_fallback) << e;
    if (slice.channel_refit) {
      EXPECT_LE(outside_quotes(slice.fit, chains[e].quotes), point_outside) << e;
    }
  }
  EXPECT_TRUE((*on)[1].point_gated);
  EXPECT_TRUE((*on)[1].channel_refit); // the kinked expiry is served by its refit
  EXPECT_FALSE((*on)[0].point_gated);
  EXPECT_FALSE((*on)[2].point_gated);
  EXPECT_GT(gated, 0u);
  expect_calendar_consistent(*on);
}

TEST(QuoteSurface, DominanceGateNeedsTheMinimumOutsideCount) {
  const auto chains = tight_chains();
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  const std::size_t kinked_outside = outside_quotes((*off)[1].fit, chains[1].quotes);
  ASSERT_GT(kinked_outside, 1u);
  QuoteSliceConfig config;
  config.channel.point_max_outside_fraction = 0.0;
  // One more than the kinked expiry's outside count: the share passes the gate,
  // the count does not, so nothing is gated and the surface is unchanged.
  config.channel.point_min_outside_quotes = kinked_outside + 1;
  const auto below = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(below) << below.error().to_string();
  EXPECT_EQ((*below)[1].point_outside_quotes, kinked_outside);
  EXPECT_GT((*below)[1].point_outside_fraction, 0.0);
  for (std::size_t e = 0; e < chains.size(); ++e) {
    EXPECT_FALSE((*below)[e].point_gated) << e;
    EXPECT_EQ((*below)[e].grade, (*off)[e].grade) << e;
    for (std::size_t i = 0; i < chains[e].quotes.size(); ++i) {
      EXPECT_EQ((*below)[e].fit.rows[i].fitted_price, (*off)[e].fit.rows[i].fitted_price);
    }
  }
  config.channel.point_min_outside_quotes = kinked_outside; // exactly the count: gated
  const auto at = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(at) << at.error().to_string();
  EXPECT_TRUE((*at)[1].point_gated);
  config.channel.point_min_outside_quotes = 0;
  EXPECT_FALSE(validate_quote_slice_channel(config.channel));
  EXPECT_EQ(fit_quote_surface(expiries(chains), config).error().code(),
            ErrorCode::InvalidArgument);
}

TEST(QuoteSurface, DominanceGateKeepsThePointFitWhenNoRefitIsServed) {
  const auto chains = tight_chains();
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  QuoteSliceConfig config;
  config.channel.point_max_outside_fraction = 0.0;
  config.channel.min_channel_quotes = 100000; // no channel refit can be served
  const auto surface = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(surface) << surface.error().to_string();
  std::size_t fallbacks = 0;
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const auto &slice = (*surface)[e];
    if (!slice.point_gated) continue;
    ++fallbacks;
    EXPECT_TRUE(slice.point_fallback);
    EXPECT_FALSE(slice.channel_refit);
    EXPECT_EQ(slice.grade, QuoteSliceGrade::B); // was A: not A evidence any more
    EXPECT_TRUE(slice.refusal.empty());
    for (std::size_t i = 0; i < chains[e].quotes.size(); ++i) {
      EXPECT_EQ(slice.fit.rows[i].fitted_price, (*off)[e].fit.rows[i].fitted_price) << i;
    }
  }
  EXPECT_TRUE((*surface)[1].point_fallback);
  EXPECT_GT(fallbacks, 0u);
}

TEST(QuoteSurface, ChannelCalendarRepairServesACrossingTheBoundedRepairAndRetryCannot) {
  // The later expiry is quoted slightly too cheap (as in the bounded-repair
  // test), the bounded-move repair is off and no channel refit can be served,
  // so the retry fails too: only repair_quote_slice_calendar_in_channel, on the
  // expiry's own point fit, can serve it.
  const std::vector<Chain> chains{chain(0.25, 0.30, 0.0, 0.01, 70, 130, 5),
                                  chain(0.5, 0.218, 0.0, 0.05, 80, 120, 5)};
  QuoteSliceConfig no_refit;
  no_refit.channel.min_channel_quotes = 100000;
  const auto off = fit_quote_surface(expiries(chains), no_refit);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ((*off)[1].refusal, "calendar_arbitrage");
  EXPECT_TRUE((*off)[1].calendar_retry);
  QuoteSliceConfig config = channel_repair_config();
  config.channel.min_channel_quotes = 100000;
  const auto on = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(on) << on.error().to_string();
  const auto &kept = (*on)[0];
  const auto &fixed = (*on)[1];
  EXPECT_EQ(kept.grade, QuoteSliceGrade::A);
  ASSERT_EQ(fixed.grade, QuoteSliceGrade::B)
      << fixed.refusal << " " << fixed.error << " channel calendar repair "
      << to_string(fixed.calendar_channel_repair.outcome);
  EXPECT_TRUE(fixed.calendar_retry);
  EXPECT_TRUE(fixed.calendar_channel_repaired);
  EXPECT_TRUE(fixed.calendar_repaired);
  EXPECT_FALSE(fixed.channel_refit);
  EXPECT_EQ(fixed.calendar_channel_repair.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(fixed.calendar_channel_repair.nodes, 0u);
  ASSERT_TRUE(fixed.fit.prices);
  expect_calendar_consistent(*on);
  expect_moved_nodes_reimplied(fixed.fit, chains[1].quotes);
  // Every served price is inside its own quote (the point fit already was, and
  // no moved price ends farther outside its channel than the point fit put it).
  const auto point = fit_quote_slice(chains[1].market, chains[1].quotes);
  ASSERT_TRUE(point);
  for (std::size_t i = 0; i < chains[1].quotes.size(); ++i) {
    const auto &quote = chains[1].quotes[i];
    ASSERT_EQ(fixed.fit.rows[i].evaluation_status, QuoteSliceEvaluationStatus::Success) << i;
    EXPECT_EQ(channel_distance(fixed.fit.rows[i].fitted_price, quote), 0.0) << i;
    EXPECT_LE(channel_distance(fixed.fit.rows[i].fitted_price, quote),
              channel_distance(point->rows[i].fitted_price, quote) + 1.0e-12)
        << i;
  }
}

TEST(QuoteSurface, BoxCertificateOpensTheCalendarRepairInsideTheQuotesWithTheChannelRepairOff) {
  // The crossing of the test above, with the channel repair OFF: the box
  // certificate alone opens the calendar repair, now into the exact quote boxes
  // (h = 0), so the later expiry serves inside every quote and the surface is
  // calendar-consistent; under the exact certificate it stays refused.
  const std::vector<Chain> chains{chain(0.25, 0.30, 0.0, 0.01, 70, 130, 5),
                                  chain(0.5, 0.218, 0.0, 0.05, 80, 120, 5)};
  QuoteSliceConfig exact;
  exact.channel.min_channel_quotes = 100000;
  const auto off = fit_quote_surface(expiries(chains), exact);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ((*off)[1].refusal, "calendar_arbitrage");
  QuoteSliceConfig config = box_certificate_config(0.0);
  config.channel.min_channel_quotes = 100000;
  ASSERT_EQ(config.repair.mode, QuoteSliceRepairMode::Off);
  const auto on = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(on) << on.error().to_string();
  const auto &fixed = (*on)[1];
  ASSERT_EQ(fixed.grade, QuoteSliceGrade::B)
      << fixed.refusal << " " << fixed.error << " box calendar repair "
      << to_string(fixed.calendar_channel_repair.outcome);
  EXPECT_TRUE(fixed.calendar_channel_repaired);
  EXPECT_EQ(fixed.calendar_channel_repair.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(fixed.calendar_channel_repair.nodes, 0u);
  EXPECT_FALSE(fixed.repair_rung);
  expect_calendar_consistent(*on);
  expect_moved_nodes_reimplied(fixed.fit, chains[1].quotes);
  for (std::size_t i = 0; i < chains[1].quotes.size(); ++i) {
    ASSERT_EQ(fixed.fit.rows[i].evaluation_status, QuoteSliceEvaluationStatus::Success) << i;
    EXPECT_EQ(channel_distance(fixed.fit.rows[i].fitted_price, chains[1].quotes[i]), 0.0) << i;
  }
}

TEST(QuoteSurface, JointInBoxRepairServesFeasibleCrossingsWithoutRefittingExpiries) {
  const std::vector<Chain> chains{chain(0.25, 0.30, 0.0, 0.01, 70, 130, 5),
                                  chain(0.5, 0.218, 0.0, 0.05, 80, 120, 5)};
  auto config = box_certificate_config(0.0);
  config.floor_policy = QuoteSliceFloorPolicy::PriceOnly;
  config.repair.inbox_projection = true;
  config.repair.market_distance = QuoteSliceMarketDistance::Repair;
  config.channel.min_channel_quotes = 100000; // no refit can help this price-feasible case
  QuoteSurfaceConfig surface;
  surface.publication = QuoteSurfacePublication::Validate;
  const auto result = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->size(), 2u);
  expect_calendar_consistent(*result);
  bool repaired = false;
  for (std::size_t s = 0; s < result->size(); ++s) {
    const auto &slice = (*result)[s];
    EXPECT_NE(slice.grade, QuoteSliceGrade::Refused) << slice.error;
    EXPECT_FALSE(slice.calendar_retry);
    EXPECT_FALSE(slice.channel_refit);
    repaired = repaired || slice.calendar_joint_repaired;
    for (std::size_t i = 0; i < chains[s].quotes.size(); ++i) {
      const auto &row = slice.fit.rows[i];
      EXPECT_TRUE(row.evaluation_status == QuoteSliceEvaluationStatus::Success ||
                  row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly);
      EXPECT_LE(channel_distance(row.fitted_price, chains[s].quotes[i]), 1e-8);
    }
  }
  EXPECT_TRUE(repaired);
}

// Zig-zag carry chain quoted on its out-of-the-money side only: every strike's
// in-the-money node-side has no quote, so no channel protects its price.
Chain otm_only(Chain c) {
  std::erase_if(c.quotes, [](const QuoteSliceQuote &q) {
    return (q.side == Side::Call) != (q.strike >= 100.0);
  });
  return c;
}

TEST(QuoteSliceChannel, RepairVolBudgetBindsOnlyUnquotedNodeSides) {
  const auto c = otm_only(zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05));
  const auto off = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ(off->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << off->fit_error;
  auto config = channel_repair_config(1000);
  config.repair.max_vol_move = 10.0; // effectively unbounded
  const auto loose = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(loose) << loose.error().to_string();
  const auto &moved = loose->diagnostics.repair;
  ASSERT_EQ(loose->fit_status, QuoteSliceFitStatus::Success)
      << loose->fit_error << " repair " << to_string(moved.outcome);
  ASSERT_GT(moved.max_unquoted_vol_move, 0.0); // unquoted in-the-money sides moved
  config.repair.max_vol_move = 0.5 * moved.max_unquoted_vol_move;
  const auto tight = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_TRUE(tight) << tight.error().to_string();
  EXPECT_EQ(tight->fit_status, QuoteSliceFitStatus::ArbitrageFailure);
  EXPECT_FALSE(tight->prices);
  EXPECT_EQ(tight->diagnostics.repair.outcome, QuoteSliceRepairOutcome::VolBudget);
  EXPECT_GT(tight->diagnostics.repair.max_unquoted_vol_move, config.repair.max_vol_move);
}

TEST(QuoteSliceChannel, ChannelCalendarRepairIsNotCertifiedWhenANeighbourKnotStillCrosses) {
  // The repair clamps this slice at its own nodes. A later neighbour with extra
  // knots between those nodes, dipping below this slice's segments there, is
  // met at every node yet still crossed between them: NotCertified, unchanged.
  const auto c = chain(0.5, 0.25, 0.0, 0.05, 80, 120, 5);
  auto fit = fit_quote_slice(c.market, c.quotes);
  ASSERT_TRUE(fit && fit->fit_status == QuoteSliceFitStatus::Success);
  const AmericanPriceSlice floor = shifted_slice(*fit, 0.002);
  std::vector<AmericanPriceNode> cap_nodes;
  const auto nodes = fit->prices->nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    cap_nodes.push_back({nodes[i].strike, nodes[i].call + 0.02, nodes[i].put + 0.02});
    const bool near_money = nodes[i].strike >= 95.0 && nodes[i].strike < 105.0;
    if (i + 1 < nodes.size() && near_money) {
      const auto &next = nodes[i + 1];
      cap_nodes.push_back({0.5 * (nodes[i].strike + next.strike),
                           0.5 * (nodes[i].call + next.call) - 0.003,
                           0.5 * (nodes[i].put + next.put) - 0.003});
    }
  }
  auto cap = AmericanPriceSlice::create(cap_nodes, fit->prices->bounds());
  ASSERT_TRUE(cap) << cap.error().to_string();
  const std::array<const AmericanPriceSlice *, 1> earlier{&floor};
  const std::array<const AmericanPriceSlice *, 1> later{&*cap};
  const std::vector<AmericanPriceNode> before(nodes.begin(), nodes.end());
  QuoteSliceRepairConfig budget;
  budget.max_nodes = 100;
  QuoteSliceRepairRecord record;
  EXPECT_FALSE(
      repair_quote_slice_calendar_in_channel(*fit, c.quotes, earlier, later, budget, &record));
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::NotCertified);
  EXPECT_TRUE(fit->moved_nodes.empty());
  const auto after = fit->prices->nodes();
  for (std::size_t i = 0; i < after.size(); ++i) {
    EXPECT_EQ(after[i].call, before[i].call) << i;
    EXPECT_EQ(after[i].put, before[i].put) << i;
  }
  // Without the dipping knots the same repair is applied.
  QuoteSliceRepairRecord plain;
  EXPECT_TRUE(repair_quote_slice_calendar_in_channel(*fit, c.quotes, earlier, {}, budget, &plain))
      << to_string(plain.outcome);
}

TEST(QuoteSurface, DominanceGateNeverCostsAnExpiryTheCalendarPassServed) {
  // The one-cent kinked expiry is gated, but a later, wide-quoted expiry with
  // the same kink sits between its point fit and its refit: the refit crosses
  // it, the point fit does not. The gate keeps the point fit, so every expiry
  // the calendar pass served is still served, at its gate-off prices. (Run
  // before the calendar pass, the refit replaced the point fit and the calendar
  // pass then refused the expiry: the review's RIVN case.)
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.0), kinked_chain(0.5, 0.0),
                                  kinked_chain(0.51, 0.10)};
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  QuoteSliceConfig config;
  config.channel.point_max_outside_fraction = 0.05;
  const auto on = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(on) << on.error().to_string();
  for (std::size_t e = 0; e < chains.size(); ++e) {
    ASSERT_EQ((*off)[e].grade, QuoteSliceGrade::A) << e << " " << (*off)[e].refusal;
    ASSERT_NE((*on)[e].grade, QuoteSliceGrade::Refused)
        << e << " " << (*on)[e].refusal << " " << (*on)[e].error;
    for (std::size_t i = 0; i < chains[e].quotes.size(); ++i) {
      EXPECT_EQ((*on)[e].fit.rows[i].fitted_price, (*off)[e].fit.rows[i].fitted_price) << e;
    }
  }
  const auto &kinked = (*on)[1];
  EXPECT_TRUE(kinked.point_gated);
  EXPECT_TRUE(kinked.point_fallback);
  EXPECT_FALSE(kinked.channel_refit);
  EXPECT_EQ(kinked.grade, QuoteSliceGrade::B);
  EXPECT_FALSE((*on)[2].point_gated);
  EXPECT_EQ((*on)[2].grade, QuoteSliceGrade::A);
  expect_calendar_consistent(*on);
  // The neighbour is what blocks the refit: without it the refit is served.
  const std::vector<Chain> alone{chains[0], chains[1]};
  const auto refit = fit_quote_surface(expiries(alone), config);
  ASSERT_TRUE(refit) << refit.error().to_string();
  EXPECT_TRUE((*refit)[1].channel_refit);
  EXPECT_FALSE((*refit)[1].point_fallback);
}

// ── Expiry fan-out (QuoteSurfaceConfig::fit_workers) ─────────────────────────

// Every field a caller reads from a slice, bit for bit (doubles by bit pattern,
// so a stripped NaN row compares equal to itself).
void expect_bit_identical(const std::vector<QuoteSurfaceSlice> &a,
                          const std::vector<QuoteSurfaceSlice> &b, unsigned workers) {
  ASSERT_EQ(a.size(), b.size()) << "workers " << workers;
  const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
  for (std::size_t e = 0; e < a.size(); ++e) {
    const QuoteSurfaceSlice &x = a[e];
    const QuoteSurfaceSlice &y = b[e];
    const auto where = [&] { return ::testing::Message() << "workers " << workers << " e " << e; };
    EXPECT_EQ(x.grade, y.grade) << where();
    EXPECT_EQ(x.refusal, y.refusal) << where();
    EXPECT_EQ(x.error, y.error) << where();
    EXPECT_EQ(x.method, y.method) << where();
    EXPECT_EQ(x.point_status, y.point_status) << where();
    EXPECT_EQ(x.knot_status, y.knot_status) << where();
    EXPECT_EQ(x.ladder_fallback, y.ladder_fallback) << where();
    EXPECT_EQ(x.channel_refit, y.channel_refit) << where();
    EXPECT_EQ(x.calendar_retry, y.calendar_retry) << where();
    EXPECT_EQ(x.calendar_repaired, y.calendar_repaired) << where();
    EXPECT_EQ(bits(x.calendar_gap), bits(y.calendar_gap)) << where();
    EXPECT_EQ(bits(x.outside_fraction), bits(y.outside_fraction)) << where();
    EXPECT_EQ(x.fit.fit_status, y.fit.fit_status) << where();
    EXPECT_EQ(x.fit.diagnostics.quotes_admitted, y.fit.diagnostics.quotes_admitted) << where();
    EXPECT_EQ(x.fit.diagnostics.channel.iterations, y.fit.diagnostics.channel.iterations)
        << where();
    ASSERT_EQ(x.fit.rows.size(), y.fit.rows.size()) << where();
    for (std::size_t i = 0; i < x.fit.rows.size(); ++i) {
      const QuoteSliceRow &p = x.fit.rows[i];
      const QuoteSliceRow &q = y.fit.rows[i];
      EXPECT_EQ(p.evaluation_status, q.evaluation_status) << where() << " row " << i;
      EXPECT_EQ(bits(p.fitted_price), bits(q.fitted_price)) << where() << " row " << i;
      EXPECT_EQ(bits(p.fitted_iv), bits(q.fitted_iv)) << where() << " row " << i;
    }
  }
}

std::size_t strictly_two_sided(const Chain &c) {
  return static_cast<std::size_t>(std::count_if(
      c.quotes.begin(), c.quotes.end(),
      [](const QuoteSliceQuote &q) { return q.bid > 0.0 && q.ask > q.bid; }));
}

// Four well-quoted expiries, each listing more strikes than the one before, so
// the densest-first order is the reverse of the index order.
std::vector<Chain> widening_chains() {
  return {chain(0.25, 0.22, -0.10, 0.01, 85, 115), chain(0.5, 0.24, -0.08, 0.01, 80, 120),
          chain(1.0, 0.26, -0.06, 0.01, 75, 125), chain(1.5, 0.27, -0.05, 0.01, 70, 130)};
}

// Review Focus #3: the expiry fan-out is a scheduling change only. A surface
// that exercises every fanned-out stage (pass A, B1/B2 refits of a sparse
// expiry) and the serial stages behind them (a calendar crossing and its
// retry) is bit-identical at 2, 7 and AUTO workers to the serial fit, with and
// without the knot-first ladder (knot_pass).
TEST(QuoteSurface, FanOutIsBitIdenticalForEveryWorkerCount) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01),
                                  chain(0.5, 0.24, -0.08, 0.15, 95, 105),
                                  chain(1.0, 0.26, -0.06, 0.01), chain(1.5, 0.12, 0.0, 0.005, 85, 115)};
  QuoteSurfaceConfig ladder;
  ladder.ladder = QuoteSurfaceLadder::KnotFirst;
  for (const QuoteSurfaceConfig &base : {QuoteSurfaceConfig{}, ladder}) {
    QuoteSurfaceConfig serial = base;
    serial.fit_workers = 1;
    const auto reference = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, serial);
    ASSERT_TRUE(reference) << reference.error().to_string();
    // The fixture really reaches the channel stages and the calendar pass.
    EXPECT_TRUE((*reference)[1].channel_refit);
    EXPECT_TRUE(std::any_of(reference->begin(), reference->end(),
                            [](const QuoteSurfaceSlice &s) { return s.calendar_retry; }));
    for (const unsigned workers : {2u, 7u, 0u}) {
      QuoteSurfaceConfig fanned = base;
      fanned.fit_workers = workers;
      const auto result = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, fanned);
      ASSERT_TRUE(result) << result.error().to_string();
      expect_bit_identical(*reference, *result, workers);
    }
  }
}

// Longest-expected-first: pass A starts the expiry with the most strictly
// two-sided quotes first (ties by index), for the point pass and the ladder's
// knot pass alike. Observed through the calling thread's dispatch trace at
// fit_workers = 1, where every task runs inline in dispatch order.
TEST(QuoteSurface, FanOutDispatchesTheDensestExpiryFirst) {
  const std::vector<Chain> chains = widening_chains();
  for (std::size_t e = 1; e < chains.size(); ++e) {
    ASSERT_LT(strictly_two_sided(chains[e - 1]), strictly_two_sided(chains[e])) << e;
  }
  QuoteSurfaceConfig ladder;
  ladder.ladder = QuoteSurfaceLadder::KnotFirst;
  for (const QuoteSurfaceConfig &surface : {QuoteSurfaceConfig{}, ladder}) {
    ASSERT_EQ(surface.fit_workers, 1u); // the default is the serial width
    std::vector<std::size_t> trace;
    {
      const detail::ScopedExpiryDispatchTrace arm{trace};
      const auto fitted = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, surface);
      ASSERT_TRUE(fitted) << fitted.error().to_string();
      for (const QuoteSurfaceSlice &slice : *fitted) {
        ASSERT_EQ(slice.grade, QuoteSliceGrade::A) << slice.refusal; // pass A only
      }
    }
    EXPECT_EQ(trace, (std::vector<std::size_t>{3, 2, 1, 0}));
  }
}

// The B1 and B2 channel refits keep the same order. Every expiry is bidless
// away from the money (no point fit), so all three take both channel stages,
// and each stage starts the densest expiry first.
TEST(QuoteSurface, FanOutOrdersTheChannelStagesDensestFirst) {
  std::vector<Chain> chains;
  const std::array<double, 3> years{0.1, 0.3, 0.7};
  const std::array<int, 3> half_width{15, 20, 25};
  for (std::size_t e = 0; e < years.size(); ++e) {
    chains.push_back(chain(years[e], 0.35, -0.05, 0.08, 100 - half_width[e],
                           100 + half_width[e], 5, 0.04));
  }
  for (std::size_t e = 1; e < chains.size(); ++e) {
    ASSERT_LT(strictly_two_sided(chains[e - 1]), strictly_two_sided(chains[e])) << e;
  }
  std::vector<std::size_t> trace;
  {
    const detail::ScopedExpiryDispatchTrace arm{trace};
    const auto fitted = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
    ASSERT_TRUE(fitted) << fitted.error().to_string();
    for (const QuoteSurfaceSlice &slice : *fitted) {
      ASSERT_EQ(slice.point_status, QuoteSliceFitStatus::InsufficientQuotes);
      ASSERT_TRUE(slice.channel_refit);
      ASSERT_NE(slice.grade, QuoteSliceGrade::Refused) << slice.refusal << " " << slice.error;
    }
  }
  // pass A, then B1, then B2
  EXPECT_EQ(trace, (std::vector<std::size_t>{2, 1, 0, 2, 1, 0, 2, 1, 0}));
}

std::atomic<std::size_t> g_tasks_run{0};
void count_expiry_task(std::size_t) { g_tasks_run.fetch_add(1u); }

// Every pass-A task runs exactly once at any width. At fit_workers = 1 all of them
// run inline on the calling thread (its armed trace sees each one); above one worker
// they run on the persistent pool, where the caller takes part as context 0 (lane P0,
// expiry_pool.hpp), so its trace sees at most all of them. The result is the same
// either way (FanOutIsBitIdenticalForEveryWorkerCount).
TEST(QuoteSurface, FanOutRunsEveryTaskOnceOnThePoolAboveOneWorker) {
  const std::vector<Chain> chains = widening_chains();
  for (const unsigned workers : {1u, 4u}) {
    QuoteSurfaceConfig surface;
    surface.fit_workers = workers;
    std::vector<std::size_t> trace;
    g_tasks_run.store(0u);
    {
      const detail::ScopedExpiryTaskHook hook(&count_expiry_task);
      const detail::ScopedExpiryDispatchTrace arm{trace};
      const auto fitted = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, surface);
      ASSERT_TRUE(fitted) << fitted.error().to_string();
    }
    EXPECT_EQ(g_tasks_run.load(), chains.size()) << "workers " << workers;
    if (workers == 1u) {
      EXPECT_EQ(trace.size(), chains.size());
    } else {
      EXPECT_LE(trace.size(), chains.size());
    }
  }
}

// ── Opt-in knot-first ladder ─────────────────────────────────────────────────

QuoteSurfaceConfig knot_first() {
  QuoteSurfaceConfig surface;
  surface.ladder = QuoteSurfaceLadder::KnotFirst;
  return surface;
}

void expect_same_prices(const QuoteSliceFit &served, const QuoteSliceFit &alone) {
  ASSERT_EQ(served.rows.size(), alone.rows.size());
  for (std::size_t i = 0; i < served.rows.size(); ++i) {
    EXPECT_EQ(served.rows[i].fitted_price, alone.rows[i].fitted_price) << i;
    EXPECT_EQ(served.rows[i].fitted_iv, alone.rows[i].fitted_iv) << i;
  }
}

TEST(QuoteSurfaceLadder, PassAServesTheCertifiedKnotSliceInsideTheMarket) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  const auto on = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, knot_first());
  ASSERT_TRUE(on) << on.error().to_string();
  for (std::size_t e = 0; e < chains.size(); ++e) {
    // Without the ladder: the point fit, and the ladder fields keep their defaults.
    const auto &point = (*off)[e];
    EXPECT_EQ(point.method, QuoteSurfaceMethod::Point);
    EXPECT_EQ(point.ladder_fallback, QuoteSurfaceLadderFallback::None);
    EXPECT_EQ(point.knot_status, QuoteSliceFitStatus::InsufficientQuotes);
    EXPECT_EQ(point.outside_fraction, 0.0);
    const auto &slice = (*on)[e];
    ASSERT_EQ(slice.grade, QuoteSliceGrade::A) << e << " " << slice.refusal << " " << slice.error;
    EXPECT_EQ(slice.method, QuoteSurfaceMethod::Channel);
    EXPECT_EQ(slice.ladder_fallback, QuoteSurfaceLadderFallback::None);
    EXPECT_EQ(slice.knot_status, QuoteSliceFitStatus::Success);
    EXPECT_EQ(slice.point_status, QuoteSliceFitStatus::Success);
    EXPECT_FALSE(slice.channel_refit);
    EXPECT_TRUE(slice.fit.diagnostics.channel.used);
    EXPECT_FALSE(slice.fit.diagnostics.channel.term_prior);
    EXPECT_LE(slice.outside_fraction, 0.05);
    // Pass A's knot rung is the standalone term-prior-free channel fit.
    const auto knot =
        fit_quote_slice_channel(chains[e].market, chains[e].quotes, QuoteSliceConfig{}, nullptr);
    ASSERT_TRUE(knot) << knot.error().to_string();
    ASSERT_EQ(knot->fit_status, QuoteSliceFitStatus::Success);
    expect_same_prices(slice.fit, *knot);
  }
  expect_calendar_consistent(*on);
}

TEST(QuoteSurfaceLadder, KnotFailureFallsBackToThePointFitAtGradeA) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01)};
  QuoteSliceConfig config;
  config.channel.min_channel_quotes = 100000; // no knot slice can be fitted
  const auto on = fit_quote_surface(expiries(chains), config, knot_first());
  ASSERT_TRUE(on) << on.error().to_string();
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const auto &slice = (*on)[e];
    ASSERT_EQ(slice.grade, QuoteSliceGrade::A) << e << " " << slice.refusal << " " << slice.error;
    EXPECT_EQ(slice.method, QuoteSurfaceMethod::Point);
    EXPECT_EQ(slice.knot_status, QuoteSliceFitStatus::InsufficientQuotes);
    EXPECT_FALSE(slice.knot_error.empty());
    EXPECT_EQ(slice.ladder_fallback, QuoteSurfaceLadderFallback::KnotFailure);
    const auto point = fit_quote_slice(chains[e].market, chains[e].quotes, config);
    ASSERT_TRUE(point) << point.error().to_string();
    expect_same_prices(slice.fit, *point);
  }
}

TEST(QuoteSurfaceLadder, PointRungServesOnlyInsideTheMarketUnlessThePolicyRelaxesIt) {
  // One-cent quotes; the middle expiry is kinked, so its point fit lies outside
  // some of them. No knot slice and no channel refit can be fitted.
  const auto chains = tight_chains();
  QuoteSliceConfig config;
  config.channel.min_channel_quotes = 100000;
  const auto off = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ((*off)[1].grade, QuoteSliceGrade::A) << (*off)[1].refusal;
  const std::size_t kinked_outside = outside_quotes((*off)[1].fit, chains[1].quotes);
  ASSERT_GT(kinked_outside, 0u);
  const auto strict = fit_quote_surface(expiries(chains), config, knot_first());
  ASSERT_TRUE(strict) << strict.error().to_string();
  EXPECT_EQ((*strict)[1].grade, QuoteSliceGrade::Refused);
  EXPECT_FALSE((*strict)[1].fit.prices);
  EXPECT_EQ((*strict)[1].ladder_fallback, QuoteSurfaceLadderFallback::KnotFailure);
  // Explicitly relaxed (a limit of 1, or a count above the kinked expiry's):
  // the point fit serves at grade A with its own prices.
  QuoteSurfaceConfig open = knot_first();
  open.ladder_max_outside_fraction = 1.0;
  QuoteSurfaceConfig count = knot_first();
  count.ladder_min_outside_quotes = kinked_outside + 1;
  for (const QuoteSurfaceConfig &relaxed : {open, count}) {
    const auto served = fit_quote_surface(expiries(chains), config, relaxed);
    ASSERT_TRUE(served) << served.error().to_string();
    for (std::size_t e = 0; e < chains.size(); ++e) {
      const auto &slice = (*served)[e];
      ASSERT_EQ(slice.grade, QuoteSliceGrade::A) << e << " " << slice.refusal;
      EXPECT_EQ(slice.method, QuoteSurfaceMethod::Point);
      expect_same_prices(slice.fit, (*off)[e].fit);
    }
    EXPECT_EQ((*served)[1].outside_quotes, kinked_outside);
    EXPECT_GT((*served)[1].outside_fraction, 0.0);
  }
}

TEST(QuoteSurfaceLadder, AChannelRefitOutsideTheMarketIsRefused) {
  // The middle expiry has three one-cent strikes near the money only (no point
  // evidence), quoted at a higher level than both neighbours, so it takes the
  // channel passes. A wide channel half-width floor (min_half_width_vol, the
  // channel objective's unit) lets the neighbours' prior pull its refit off
  // those one-cent quotes without a channel penalty.
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01),
                                  chain(0.5, 0.30, -0.08, 0.0, 95, 105),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  QuoteSliceConfig config;
  config.channel.min_half_width_vol = 0.05;
  QuoteSurfaceConfig open = knot_first();
  open.ladder_channel_max_outside_fraction = 1.0;
  const auto served = fit_quote_surface(expiries(chains), config, open);
  ASSERT_TRUE(served) << served.error().to_string();
  const auto &sparse = (*served)[1];
  ASSERT_NE(sparse.grade, QuoteSliceGrade::Refused) << sparse.refusal << " " << sparse.error;
  EXPECT_NE(sparse.grade, QuoteSliceGrade::A);
  EXPECT_EQ(sparse.method, QuoteSurfaceMethod::ChannelRefit);
  EXPECT_TRUE(sparse.channel_refit);
  ASSERT_GT(sparse.outside_quotes, 0u);
  EXPECT_EQ(sparse.outside_quotes, outside_quotes(sparse.fit, chains[1].quotes));
  // The default channel limit (5%, at least one quote) refuses that refit.
  const auto strict = fit_quote_surface(expiries(chains), config, knot_first());
  ASSERT_TRUE(strict) << strict.error().to_string();
  const auto &outside = (*strict)[1];
  EXPECT_EQ(outside.grade, QuoteSliceGrade::Refused);
  EXPECT_EQ(outside.refusal, "outside_market") << outside.error;
  EXPECT_FALSE(outside.fit.prices);
  EXPECT_EQ((*strict)[0].grade, QuoteSliceGrade::A);
  EXPECT_EQ((*strict)[2].grade, QuoteSliceGrade::A);
}

TEST(QuoteSurfaceLadder, CalendarPassNeverLetsALowerConfidenceSliceBlockARepair) {
  // X (13 strikes) is quoted richly; H (9 strikes, 5% spreads) slightly too
  // cheap, a crossing its bounded-move repair fixes against X alone; E sits
  // between them in maturity with only 7 strikes and one-cent quotes, priced
  // like X, so it is consistent with X but crosses H once H is lifted. No knot
  // slice or channel refit can be fitted, so every slice is a point fit (A).
  const std::vector<Chain> chains{chain(0.25, 0.30, 0.0, 0.01, 70, 130, 5),
                                  chain(0.375, 0.30, 0.0, 0.0, 85, 115, 5),
                                  chain(0.5, 0.218, 0.0, 0.05, 80, 120, 5)};
  QuoteSliceConfig config;
  config.channel.min_channel_quotes = 100000;
  QuoteSurfaceConfig surface;
  surface.calendar_repair_half_spreads = 1.0;
  // Without the ladder, the two-pass calendar pass accepts every clean slice
  // first: E is accepted beside X, and H's repair must then clear E as well.
  const auto off = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ((*off)[0].grade, QuoteSliceGrade::A);
  ASSERT_EQ((*off)[1].grade, QuoteSliceGrade::A) << (*off)[1].refusal;
  ASSERT_EQ((*off)[2].refusal, "calendar_arbitrage");
  // The ladder visits once in confidence order (X, H, E): H is repaired against
  // X before E is visited, and E, the lower-confidence slice, is the one refused.
  surface.ladder = QuoteSurfaceLadder::KnotFirst;
  surface.ladder_max_outside_fraction = 1.0; // containment is not under test
  const auto on = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(on) << on.error().to_string();
  EXPECT_EQ((*on)[0].grade, QuoteSliceGrade::A);
  const auto &h = (*on)[2];
  ASSERT_EQ(h.grade, QuoteSliceGrade::B) << h.refusal << " " << h.error;
  EXPECT_TRUE(h.calendar_repaired);
  EXPECT_EQ(h.method, QuoteSurfaceMethod::Point);
  EXPECT_EQ((*on)[1].grade, QuoteSliceGrade::Refused);
  EXPECT_EQ((*on)[1].refusal, "calendar_arbitrage");
  expect_calendar_consistent(*on);
}

TEST(QuoteSurfaceLadder, ASliceOutsideTheMarketNeverOutranksOneInsideItForTheCalendarSlot) {
  // K (one-cent quotes, kinked smile, 11 strikes): its point fit lies outside
  // some of its quotes. L, later and quoted far cheaper (7 strikes), crosses it.
  // Neither can be repaired or refitted, so the calendar order alone decides.
  const std::vector<Chain> chains{kinked_chain(0.25, 0.0),
                                  chain(0.5, 0.15, 0.0, 0.005, 85, 115, 5)};
  QuoteSliceConfig config;
  config.channel.min_channel_quotes = 100000; // no knot slice, no channel refit
  const auto off = fit_quote_surface(expiries(chains), config);
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ((*off)[0].grade, QuoteSliceGrade::A) << (*off)[0].refusal;
  ASSERT_EQ((*off)[1].refusal, "calendar_arbitrage"); // K keeps the slot (more evidence)
  // K's point fit serves only because the policy relaxes the point limit; the
  // channel limit stays 5%, so K is not inside the market under the smaller
  // limit and ranks after L, which is: L keeps the slot.
  QuoteSurfaceConfig relaxed = knot_first();
  relaxed.ladder_max_outside_fraction = 1.0;
  const auto on = fit_quote_surface(expiries(chains), config, relaxed);
  ASSERT_TRUE(on) << on.error().to_string();
  ASSERT_GT((*on)[0].outside_fraction, 0.05);
  EXPECT_EQ((*on)[1].outside_quotes, 0u);
  EXPECT_EQ((*on)[1].grade, QuoteSliceGrade::A) << (*on)[1].refusal;
  EXPECT_EQ((*on)[0].grade, QuoteSliceGrade::Refused);
  EXPECT_EQ((*on)[0].refusal, "calendar_arbitrage");
  // Both limits relaxed: containment no longer separates them, so the
  // two-sided quote count does and K keeps the slot again.
  relaxed.ladder_channel_max_outside_fraction = 1.0;
  const auto open = fit_quote_surface(expiries(chains), config, relaxed);
  ASSERT_TRUE(open) << open.error().to_string();
  EXPECT_EQ((*open)[0].grade, QuoteSliceGrade::A) << (*open)[0].refusal;
  EXPECT_EQ((*open)[1].refusal, "calendar_arbitrage");
}

TEST(QuoteSurfaceLadder, AKnotSliceThatCrossesFallsBackToItsHeldPointFit) {
  // One smile sigma(k) = 0.25 - 0.30 k. E (0.48 y, 7 strikes, one-cent quotes)
  // has too few channels for a knot slice, so its point fit serves. L (0.5 y,
  // 13 strikes, 10% half-spreads) gets a knot slice that a wide channel floor
  // and a strong flat default shape (no mid anchor) pull off its skew: outside
  // its quotes, served only under the relaxed channel limit, and below E's
  // prices in the wings. L's own point fit follows the skew, is inside its
  // quotes and is consistent with E.
  const std::vector<Chain> chains{chain(0.48, 0.25, -0.30, 0.0, 85, 115, 5),
                                  chain(0.5, 0.25, -0.30, 0.10, 70, 130, 5)};
  QuoteSliceConfig config;
  config.channel.min_channel_quotes = 20;
  config.channel.min_half_width_vol = 0.05;
  config.channel.mid_weight = 0.0;
  config.channel.default_shape_weight = 100.0;
  QuoteSurfaceConfig surface = knot_first();
  surface.ladder_channel_max_outside_fraction = 1.0;
  const auto on = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(on) << on.error().to_string();
  const auto &e = (*on)[0];
  const auto &l = (*on)[1];
  ASSERT_EQ(e.grade, QuoteSliceGrade::A) << e.refusal << " " << e.error;
  EXPECT_EQ(e.method, QuoteSurfaceMethod::Point);
  EXPECT_EQ(e.knot_status, QuoteSliceFitStatus::InsufficientQuotes);
  // L's knot slice served pass A (it certified, under the relaxed limit), then
  // crossed E; the calendar pass served L's held point fit instead, unrepaired.
  EXPECT_EQ(l.knot_status, QuoteSliceFitStatus::Success);
  ASSERT_EQ(l.grade, QuoteSliceGrade::A)
      << l.refusal << " " << l.error << " method " << to_string(l.method) << " fallback "
      << to_string(l.ladder_fallback);
  EXPECT_EQ(l.method, QuoteSurfaceMethod::Point);
  EXPECT_FALSE(l.channel_refit);
  EXPECT_LE(l.outside_fraction, 0.05);
  EXPECT_EQ(l.ladder_fallback, QuoteSurfaceLadderFallback::Calendar);
  EXPECT_FALSE(l.calendar_repaired);
  EXPECT_FALSE(l.calendar_retry);
  const auto point = fit_quote_slice(chains[1].market, chains[1].quotes, config);
  ASSERT_TRUE(point) << point.error().to_string();
  expect_same_prices(l.fit, *point);
  expect_calendar_consistent(*on);
  // The knot slice alone does cross E (it is what the calendar pass refused).
  const auto knot = fit_quote_slice_channel(chains[1].market, chains[1].quotes, config, nullptr);
  ASSERT_TRUE(knot && knot->fit_status == QuoteSliceFitStatus::Success);
  EXPECT_FALSE(check_american_calendar(*e.fit.prices, *knot->prices));
}

TEST(QuoteSurfaceLadder, RejectsAChannelProfileTheDominanceGateAndBadLimits) {
  const std::vector<Chain> one{chain(0.25, 0.22, 0.0, 0.01)};
  EXPECT_EQ(fit_quote_surface(expiries(one), channel_config(), knot_first()).error().code(),
            ErrorCode::InvalidArgument);
  QuoteSliceConfig gated;
  gated.channel.point_max_outside_fraction = 0.05;
  EXPECT_EQ(fit_quote_surface(expiries(one), gated, knot_first()).error().code(),
            ErrorCode::InvalidArgument);
  for (const auto mutate : {+[](QuoteSurfaceConfig &s) { s.ladder_max_outside_fraction = -0.1; },
                            +[](QuoteSurfaceConfig &s) { s.ladder_max_outside_fraction = 1.5; },
                            +[](QuoteSurfaceConfig &s) {
                              s.ladder_channel_max_outside_fraction = std::nan("");
                            },
                            +[](QuoteSurfaceConfig &s) { s.ladder_min_outside_quotes = 0; },
                            +[](QuoteSurfaceConfig &s) {
                              s.ladder = static_cast<QuoteSurfaceLadder>(7);
                            }}) {
    QuoteSurfaceConfig bad = knot_first();
    mutate(bad);
    EXPECT_EQ(fit_quote_surface(expiries(one), QuoteSliceConfig{}, bad).error().code(),
              ErrorCode::InvalidArgument);
  }
}

TEST(QuoteSurfaceLadder, RepairRungServesTheKnotSliceItsChannelRepairCertifies) {
  // N (0.25 y, 14% volatility, seven strikes, clean) serves its knot slice at
  // pass A. Z (0.5 y, 25%) is the micro-kinked carry zig-zag: its knot slice and
  // its point fit both fail their certificates at pass A (which never runs the
  // channel repair). A strong term prior pulls Z's channel refits (B1, B2)
  // towards N's 14%, where more strikes kink: the channel repair would move four
  // strikes materially, beyond repair.max_nodes = 3, so both refits fail their
  // certificates and Z reaches the ladder's repair rung still refused
  // arbitrage_failure. There the prior-free knot fit WITH the channel repair
  // moves one strike, certifies inside the market and serves first: the knot
  // branch of the rung, ahead of the point fit.
  const std::vector<Chain> chains{chain(0.25, 0.14, 0.0, 0.1, 85, 115, 5),
                                  zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05)};
  const QuoteSliceConfig config = channel_repair_config(3);
  QuoteSurfaceConfig surface = knot_first();
  surface.term_prior_weight = 100.0;
  const auto on = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(on) << on.error().to_string();
  EXPECT_EQ((*on)[0].grade, QuoteSliceGrade::A) << (*on)[0].refusal;
  const auto &z = (*on)[1];
  EXPECT_EQ(z.knot_status, QuoteSliceFitStatus::ArbitrageFailure) << z.knot_error;
  EXPECT_EQ(z.point_status, QuoteSliceFitStatus::ArbitrageFailure) << z.point_error;
  EXPECT_EQ(z.ladder_fallback, QuoteSurfaceLadderFallback::Certificate);
  ASSERT_NE(z.grade, QuoteSliceGrade::Refused)
      << z.refusal << " " << z.error << " rung " << to_string(z.rung_repair.outcome);
  EXPECT_NE(z.grade, QuoteSliceGrade::A);
  EXPECT_TRUE(z.repair_rung);
  EXPECT_FALSE(z.channel_refit);
  EXPECT_EQ(z.method, QuoteSurfaceMethod::Channel) << to_string(z.method);
  EXPECT_EQ(z.rung_repair.outcome, QuoteSliceRepairOutcome::Applied)
      << to_string(z.rung_repair.outcome);
  EXPECT_LE(z.rung_repair.material_nodes, config.repair.max_nodes);
  EXPECT_TRUE(z.fit.diagnostics.channel.used);
  EXPECT_FALSE(z.fit.diagnostics.channel.term_prior); // the knot rung is prior-free
  EXPECT_LE(z.outside_fraction, 0.05);
  // Served exactly as the standalone prior-free knot fit with the channel repair.
  const auto knot = fit_quote_slice_channel(chains[1].market, chains[1].quotes, config, nullptr);
  ASSERT_TRUE(knot) << knot.error().to_string();
  ASSERT_EQ(knot->fit_status, QuoteSliceFitStatus::Success) << knot->fit_error;
  EXPECT_EQ(knot->diagnostics.repair.outcome, QuoteSliceRepairOutcome::Applied);
  expect_same_prices(z.fit, *knot);
  expect_moved_nodes_reimplied(z.fit, chains[1].quotes);
  expect_calendar_consistent(*on);
}

TEST(QuoteSurfaceLadder, AnInMarketKnotSliceWithoutOwnEvidenceKeepsFallbackNone) {
  // The middle expiry quotes three strikes near the money: its knot slice
  // certifies inside the market, but it lacks the A rule's own point evidence,
  // and so does its point fit (the same admission), which is refused. The knot
  // rung did not fail, so ladder_fallback stays None; pass A serves neither,
  // and the channel passes serve a refit (method channel_refit, not point).
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01),
                                  chain(0.5, 0.24, -0.08, 0.01, 95, 105),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  const QuoteSliceConfig config;
  const auto knot = fit_quote_slice_channel(chains[1].market, chains[1].quotes, config, nullptr);
  ASSERT_TRUE(knot) << knot.error().to_string();
  ASSERT_EQ(knot->fit_status, QuoteSliceFitStatus::Success) << knot->fit_error;
  EXPECT_LT(knot->diagnostics.distinct_strikes, config.min_quotes);
  EXPECT_EQ(outside_quotes(*knot, chains[1].quotes), 0u);
  const auto on = fit_quote_surface(expiries(chains), config, knot_first());
  ASSERT_TRUE(on) << on.error().to_string();
  const auto &sparse = (*on)[1];
  EXPECT_EQ(sparse.knot_status, QuoteSliceFitStatus::Success);
  EXPECT_EQ(sparse.point_status, QuoteSliceFitStatus::InsufficientQuotes);
  EXPECT_EQ(sparse.ladder_fallback, QuoteSurfaceLadderFallback::None)
      << to_string(sparse.ladder_fallback);
  ASSERT_NE(sparse.grade, QuoteSliceGrade::Refused) << sparse.refusal << " " << sparse.error;
  EXPECT_NE(sparse.grade, QuoteSliceGrade::A);
  EXPECT_EQ(sparse.method, QuoteSurfaceMethod::ChannelRefit) << to_string(sparse.method);
  EXPECT_TRUE(sparse.channel_refit);
  EXPECT_TRUE(sparse.fit.diagnostics.channel.term_prior);
  for (const std::size_t e : {std::size_t{0}, std::size_t{2}}) {
    EXPECT_EQ((*on)[e].grade, QuoteSliceGrade::A) << e;
    EXPECT_EQ((*on)[e].method, QuoteSurfaceMethod::Channel) << e;
    EXPECT_EQ((*on)[e].ladder_fallback, QuoteSurfaceLadderFallback::None) << e;
  }
}

TEST(QuoteSurfaceLadder, EveryNewStatusHasItsReportString) {
  EXPECT_STREQ(to_string(QuoteSliceEvaluationStatus::PriceOnly), "price_only");
  EXPECT_STREQ(to_string(QuoteSurfaceMethod::Point), "point");
  EXPECT_STREQ(to_string(QuoteSurfaceMethod::Channel), "channel");
  EXPECT_STREQ(to_string(QuoteSurfaceMethod::ChannelRefit), "channel_refit");
  EXPECT_STREQ(to_string(QuoteSurfaceLadderFallback::None), "none");
  EXPECT_STREQ(to_string(QuoteSurfaceLadderFallback::Certificate), "certificate");
  EXPECT_STREQ(to_string(QuoteSurfaceLadderFallback::KnotFailure), "knot_failure");
  EXPECT_STREQ(to_string(QuoteSurfaceLadderFallback::Containment), "containment");
  EXPECT_STREQ(to_string(QuoteSurfaceLadderFallback::Calendar), "calendar");
  // An enumerator outside the declared set (any value of the fixed underlying
  // type is a valid enum value) maps to "unknown", never to a declared string.
  EXPECT_STREQ(to_string(static_cast<QuoteSliceEvaluationStatus>(200)), "unknown");
  EXPECT_STREQ(to_string(static_cast<QuoteSurfaceMethod>(200)), "unknown");
  EXPECT_STREQ(to_string(static_cast<QuoteSurfaceLadderFallback>(200)), "unknown");
}

// ── Ladder containment after pass A, and the channel_repaired calendar key ───

// Share of the strictly two-sided quotes (0 < bid < ask) whose fitted price is
// outside [bid, ask]: the ladder's containment measure on a fit whose rows all
// carry a price.
double outside_share_of(const QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes) {
  std::size_t two_sided = 0;
  std::size_t outside = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (!(quotes[i].bid > 0.0) || !(quotes[i].ask > quotes[i].bid)) continue;
    ++two_sided;
    outside += !(channel_distance(fit.rows[i].fitted_price, quotes[i]) <= 0.0);
  }
  return two_sided == 0 ? 0.0 : static_cast<double>(outside) / static_cast<double>(two_sided);
}

std::size_t two_sided_quotes(std::span<const QuoteSliceQuote> quotes) {
  return static_cast<std::size_t>(
      std::count_if(quotes.begin(), quotes.end(), [](const QuoteSliceQuote &q) {
        return q.bid > 0.0 && q.ask > q.bid;
      }));
}

// The chain with its in-the-money calls at `strikes` re-quoted `width` wide,
// `offset` above their model price (the old mid). The point fit prices the
// out-of-the-money side only, so it reprices those calls at the model, below
// their bids. A channel fit sees them, but its objective floors a channel's
// half-width at channel.min_half_width_vol, which on a deep call (small vega) is
// far wider than such a quote, and the put at the same strike holds the knot
// slice at the model: it can sit below those bids as well.
Chain off_market_itm_calls(Chain c, std::initializer_list<double> strikes, double offset,
                           double width) {
  for (auto &quote : c.quotes) {
    const bool listed = std::find(strikes.begin(), strikes.end(), quote.strike) != strikes.end();
    if (quote.side != Side::Call || !listed) continue;
    const double mid = 0.5 * (quote.bid + quote.ask);
    quote.bid = mid + offset;
    quote.ask = quote.bid + width;
  }
  return c;
}

TEST(QuoteSurfaceLadder, RepairRungSkipsAKnotFitItsRepairCertifiesOutsideTheMarket) {
  // N (0.25 y, 14%, 10% half-spreads) serves its knot slice at pass A. Z is the
  // micro-kinked carry zig-zag of RepairRungServesTheKnotSliceItsChannelRepair-
  // Certifies with its three deepest in-the-money calls (80, 82, 84) re-quoted
  // one cent wide, eight cents above the model. Both pass-A fits fail their
  // certificates and the strong prior's refits exceed the repair's node budget,
  // so Z reaches the repair rung. There the prior-free knot fit WITH the channel
  // repair certifies, but outside the market on more than 5% of Z's quotes: the
  // rung skips it (default channel limit) and serves the point fit with the
  // repair, which this policy lets serve outside (ladder_max_outside_fraction 1).
  const Chain z = off_market_itm_calls(zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05),
                                       {80.0, 82.0, 84.0}, 0.08, 0.01);
  const std::vector<Chain> chains{chain(0.25, 0.14, 0.0, 0.1, 85, 115, 5), z};
  const QuoteSliceConfig config = channel_repair_config(3);
  const auto knot = fit_quote_slice_channel(z.market, z.quotes, config, nullptr);
  ASSERT_TRUE(knot) << knot.error().to_string();
  ASSERT_EQ(knot->fit_status, QuoteSliceFitStatus::Success) << knot->fit_error;
  EXPECT_EQ(knot->diagnostics.repair.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(outside_share_of(*knot, z.quotes), 0.05);
  const auto point = fit_quote_slice(z.market, z.quotes, config);
  ASSERT_TRUE(point) << point.error().to_string();
  ASSERT_EQ(point->fit_status, QuoteSliceFitStatus::Success) << point->fit_error;
  QuoteSurfaceConfig surface = knot_first();
  surface.term_prior_weight = 100.0;
  surface.ladder_max_outside_fraction = 1.0;
  const auto on = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(on) << on.error().to_string();
  EXPECT_EQ((*on)[0].grade, QuoteSliceGrade::A) << (*on)[0].refusal;
  const auto &served = (*on)[1];
  EXPECT_EQ(served.knot_status, QuoteSliceFitStatus::ArbitrageFailure);
  EXPECT_EQ(served.point_status, QuoteSliceFitStatus::ArbitrageFailure);
  ASSERT_NE(served.grade, QuoteSliceGrade::Refused) << served.refusal << " " << served.error;
  EXPECT_TRUE(served.repair_rung);
  ASSERT_EQ(served.method, QuoteSurfaceMethod::Point)
      << "ladder_repair_rung served the knot fit outside the market (method "
      << to_string(served.method) << ", outside share " << served.outside_fraction << ")";
  expect_same_prices(served.fit, *point);
  EXPECT_EQ(served.outside_quotes, outside_quotes(*point, z.quotes));
}

TEST(QuoteSurfaceLadder, RepairRungSkipsAPointFitItsRepairCertifiesOutsideTheMarket) {
  // Z with three in-the-money calls (84, 88, 92) re-quoted two cents wide, five
  // cents above the model. They pull the knot slice against the puts at the
  // same strikes, so the rung's knot fit exceeds the repair's node budget. The
  // rung's point fit (out-of-the-money side) certifies with the repair but
  // reprices those calls below their bids: 3 of 42 quotes, more than 5%. The
  // rung skips it under the default point limit and Z stays refused; only an
  // explicitly relaxed limit lets the rung serve it.
  const Chain z = off_market_itm_calls(zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05),
                                       {84.0, 88.0, 92.0}, 0.05, 0.02);
  const std::vector<Chain> chains{chain(0.25, 0.14, 0.0, 0.1, 85, 115, 5), z};
  const QuoteSliceConfig config = channel_repair_config(3);
  const auto knot = fit_quote_slice_channel(z.market, z.quotes, config, nullptr);
  ASSERT_TRUE(knot) << knot.error().to_string();
  EXPECT_EQ(knot->fit_status, QuoteSliceFitStatus::ArbitrageFailure);
  EXPECT_EQ(knot->diagnostics.repair.outcome, QuoteSliceRepairOutcome::NodeBudget)
      << to_string(knot->diagnostics.repair.outcome);
  const auto point = fit_quote_slice(z.market, z.quotes, config);
  ASSERT_TRUE(point) << point.error().to_string();
  ASSERT_EQ(point->fit_status, QuoteSliceFitStatus::Success) << point->fit_error;
  EXPECT_EQ(point->diagnostics.repair.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(outside_share_of(*point, z.quotes), 0.05);
  QuoteSurfaceConfig surface = knot_first();
  surface.term_prior_weight = 100.0;
  const auto strict = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(strict) << strict.error().to_string();
  EXPECT_EQ((*strict)[0].grade, QuoteSliceGrade::A) << (*strict)[0].refusal;
  const auto &refused = (*strict)[1];
  EXPECT_EQ(refused.grade, QuoteSliceGrade::Refused)
      << "ladder_repair_rung served the point fit outside the market (method "
      << to_string(refused.method) << ", outside share " << refused.outside_fraction << ")";
  EXPECT_EQ(refused.refusal, "arbitrage_failure");
  EXPECT_FALSE(refused.repair_rung);
  EXPECT_FALSE(refused.fit.prices);
  surface.ladder_max_outside_fraction = 1.0;
  const auto relaxed = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(relaxed) << relaxed.error().to_string();
  const auto &served = (*relaxed)[1];
  ASSERT_NE(served.grade, QuoteSliceGrade::Refused) << served.refusal << " " << served.error;
  EXPECT_TRUE(served.repair_rung);
  EXPECT_EQ(served.method, QuoteSurfaceMethod::Point);
  expect_same_prices(served.fit, *point);
  EXPECT_GT(served.outside_fraction, 0.05);
}

TEST(QuoteSurfaceLadder, ABoundedCalendarRepairThatLeavesTheMarketIsRefused) {
  // The bounded-repair pair with L (0.5 y, nine strikes, 5% half-spreads) quoted
  // further below X (0.25 y): L's bounded-move calendar repair needs 2.6 of its
  // half-spreads, within this policy's bound (10), certifies and is consistent
  // with X, but lifts L above its asks on more than 5% of its quotes. No knot
  // slice and no channel refit can be fitted, so no later rung can serve L: it
  // is refused. With the point limit relaxed the same repair serves L.
  const std::vector<Chain> chains{chain(0.25, 0.30, 0.0, 0.01, 70, 130, 5),
                                  chain(0.5, 0.21, 0.0, 0.05, 80, 120, 5)};
  QuoteSliceConfig config;
  config.channel.min_channel_quotes = 100000;
  QuoteSurfaceConfig surface = knot_first();
  surface.calendar_repair_half_spreads = 10.0;
  const auto strict = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(strict) << strict.error().to_string();
  EXPECT_EQ((*strict)[0].grade, QuoteSliceGrade::A) << (*strict)[0].refusal;
  const auto &refused = (*strict)[1];
  EXPECT_EQ(refused.grade, QuoteSliceGrade::Refused)
      << "a bounded calendar repair served L outside the market (outside share "
      << refused.outside_fraction << ")";
  EXPECT_EQ(refused.refusal, "calendar_arbitrage");
  EXPECT_FALSE(refused.calendar_repaired);
  EXPECT_GT(refused.calendar_repair_required, 1.0);
  EXPECT_LE(refused.calendar_repair_required, surface.calendar_repair_half_spreads);
  surface.ladder_max_outside_fraction = 1.0;
  const auto relaxed = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(relaxed) << relaxed.error().to_string();
  const auto &moved = (*relaxed)[1];
  ASSERT_EQ(moved.grade, QuoteSliceGrade::B) << moved.refusal << " " << moved.error;
  EXPECT_TRUE(moved.calendar_repaired);
  EXPECT_EQ(moved.method, QuoteSurfaceMethod::Point);
  EXPECT_GT(moved.outside_fraction, 0.05);
  EXPECT_EQ(moved.outside_quotes, outside_quotes(moved.fit, chains[1].quotes));
  expect_calendar_consistent(*relaxed);
}

TEST(QuoteSurfaceLadder, AChannelCalendarRepairThatEndsOutsideTheMarketIsRefused) {
  // The bounded-repair pair without the bounded repair: L (0.5 y, nine strikes,
  // 5% half-spreads) crosses X (0.25 y) by less than its half-spreads. The
  // channel calendar repair of L's own (knot) prices exceeds the repair's node
  // budget (3). The stronger-prior retry lifts L towards X far above its asks
  // and still crosses X; the channel calendar repair of the retry's prices fits
  // the budget and certifies, but leaves L outside its quotes: refused
  // (left_channel). With the channel limit relaxed it serves the retry.
  const std::vector<Chain> chains{chain(0.25, 0.30, 0.0, 0.01, 70, 130, 5),
                                  chain(0.5, 0.218, 0.0, 0.05, 80, 120, 5)};
  const QuoteSliceConfig config = channel_repair_config(3);
  {
    auto own = fit_quote_slice_channel(chains[1].market, chains[1].quotes, QuoteSliceConfig{},
                                       nullptr);
    const auto x = fit_quote_slice_channel(chains[0].market, chains[0].quotes,
                                           QuoteSliceConfig{}, nullptr);
    ASSERT_TRUE(own && own->fit_status == QuoteSliceFitStatus::Success);
    ASSERT_TRUE(x && x->fit_status == QuoteSliceFitStatus::Success);
    const std::array<const AmericanPriceSlice *, 1> earlier{&*x->prices};
    QuoteSliceRepairRecord record;
    EXPECT_FALSE(repair_quote_slice_calendar_in_channel(*own, chains[1].quotes, earlier, {},
                                                        config.repair, &record));
    EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::NodeBudget) << to_string(record.outcome);
  }
  QuoteSurfaceConfig surface = knot_first();
  surface.term_prior_weight = 3.0;
  const auto strict = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(strict) << strict.error().to_string();
  EXPECT_EQ((*strict)[0].grade, QuoteSliceGrade::A) << (*strict)[0].refusal;
  const auto &refused = (*strict)[1];
  EXPECT_EQ(refused.grade, QuoteSliceGrade::Refused)
      << "a channel calendar repair served L outside the market (outside share "
      << refused.outside_fraction << ", method " << to_string(refused.method) << ")";
  EXPECT_EQ(refused.refusal, "calendar_arbitrage");
  EXPECT_TRUE(refused.calendar_retry);
  EXPECT_FALSE(refused.calendar_channel_repaired);
  EXPECT_EQ(refused.calendar_channel_repair.outcome, QuoteSliceRepairOutcome::LeftChannel)
      << to_string(refused.calendar_channel_repair.outcome);
  surface.ladder_channel_max_outside_fraction = 1.0;
  const auto relaxed = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(relaxed) << relaxed.error().to_string();
  const auto &served = (*relaxed)[1];
  ASSERT_EQ(served.grade, QuoteSliceGrade::B) << served.refusal << " " << served.error;
  EXPECT_EQ(served.method, QuoteSurfaceMethod::ChannelRefit); // the retry, not the own fit
  EXPECT_TRUE(served.calendar_channel_repaired);
  EXPECT_TRUE(served.calendar_repaired);
  EXPECT_EQ(served.calendar_channel_repair.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_LE(served.calendar_channel_repair.material_nodes, config.repair.max_nodes);
  EXPECT_EQ(served.ladder_fallback, QuoteSurfaceLadderFallback::Calendar);
  EXPECT_GT(served.outside_fraction, 0.05);
  expect_calendar_consistent(*relaxed);
}

TEST(QuoteSurfaceLadder, ACleanRetryOutsideTheMarketIsRefused) {
  // X (0.25 y, 40%) and L (0.5 y, 15%, seven strikes, one-cent quotes): L's knot
  // slice and its held point fit cross X, and no calendar repair is enabled. A
  // wide channel half-width floor (min_half_width_vol 0.2) and a strong prior
  // let the stronger-prior retry lift L to X's level: consistent with X (clean)
  // but outside every one of L's quotes, so it is refused. With the channel
  // limit relaxed the same clean retry serves L.
  const std::vector<Chain> chains{chain(0.25, 0.40, 0.0, 0.01, 70, 130, 5),
                                  chain(0.5, 0.15, 0.0, 0.0, 85, 115, 5)};
  QuoteSliceConfig config;
  config.channel.min_half_width_vol = 0.2;
  QuoteSurfaceConfig surface = knot_first();
  surface.term_prior_weight = 100.0;
  const auto strict = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(strict) << strict.error().to_string();
  EXPECT_EQ((*strict)[0].grade, QuoteSliceGrade::A) << (*strict)[0].refusal;
  const auto &refused = (*strict)[1];
  EXPECT_EQ(refused.grade, QuoteSliceGrade::Refused)
      << "the stronger-prior retry served L outside the market (outside share "
      << refused.outside_fraction << ", method " << to_string(refused.method) << ")";
  EXPECT_EQ(refused.refusal, "calendar_arbitrage");
  EXPECT_TRUE(refused.calendar_retry);
  EXPECT_FALSE(refused.channel_refit);
  surface.ladder_channel_max_outside_fraction = 1.0;
  const auto relaxed = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(relaxed) << relaxed.error().to_string();
  const auto &served = (*relaxed)[1];
  ASSERT_EQ(served.grade, QuoteSliceGrade::B) << served.refusal << " " << served.error;
  EXPECT_EQ(served.method, QuoteSurfaceMethod::ChannelRefit);
  EXPECT_EQ(served.ladder_fallback, QuoteSurfaceLadderFallback::Calendar);
  EXPECT_FALSE(served.calendar_repaired); // clean: no repair moved it
  EXPECT_FALSE(served.calendar_channel_repaired);
  EXPECT_GT(served.outside_fraction, 0.05);
  expect_calendar_consistent(*relaxed);
}

TEST(QuoteSurfaceLadder, AnUnrepairedSliceKeepsTheCalendarSlotOverAChannelRepairedOne) {
  // Equal containment and grade tier: U (0.25 y, 45%, five strikes: no own
  // point evidence) is served by a channel refit at its fit's own prices, grade
  // B; R (the micro-kinked carry zig-zag, 0.5 y, 21 strikes) fails both pass-A
  // certificates and its channel refit certifies only with the channel repair,
  // grade B. Both are inside the market. U is quoted far above R, so they cross,
  // and no repair or retry reconciles them: one keeps the calendar slot. R has
  // more strictly two-sided quotes, but U's prices are its fit's own: U keeps it.
  const Chain u = chain(0.25, 0.45, 0.0, 0.01, 90, 110, 5);
  const Chain r = zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05);
  const std::vector<Chain> only_u{u};
  const std::vector<Chain> only_r{r};
  const std::vector<Chain> pair{u, r};
  const QuoteSliceConfig config = channel_repair_config();
  ASSERT_LT(two_sided_quotes(u.quotes), two_sided_quotes(r.quotes));
  // Alone, each is served by a channel refit at grade B inside the market.
  const auto alone_u = fit_quote_surface(expiries(only_u), config, knot_first());
  ASSERT_TRUE(alone_u) << alone_u.error().to_string();
  ASSERT_EQ((*alone_u)[0].grade, QuoteSliceGrade::B) << (*alone_u)[0].refusal;
  EXPECT_EQ((*alone_u)[0].method, QuoteSurfaceMethod::ChannelRefit);
  EXPECT_EQ((*alone_u)[0].fit.diagnostics.repair.outcome, QuoteSliceRepairOutcome::NotAttempted);
  EXPECT_LE((*alone_u)[0].outside_fraction, 0.05);
  const auto alone_r = fit_quote_surface(expiries(only_r), config, knot_first());
  ASSERT_TRUE(alone_r) << alone_r.error().to_string();
  ASSERT_EQ((*alone_r)[0].grade, QuoteSliceGrade::B) << (*alone_r)[0].refusal;
  EXPECT_EQ((*alone_r)[0].method, QuoteSurfaceMethod::ChannelRefit);
  EXPECT_EQ((*alone_r)[0].fit.diagnostics.repair.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_LE((*alone_r)[0].outside_fraction, 0.05);
  const auto both = fit_quote_surface(expiries(pair), config, knot_first());
  ASSERT_TRUE(both) << both.error().to_string();
  const auto &kept = (*both)[0];
  const auto &dropped = (*both)[1];
  ASSERT_EQ(kept.grade, QuoteSliceGrade::B)
      << "the channel-repaired slice kept the calendar slot: U " << kept.refusal << ", R "
      << to_string(dropped.grade) << " " << dropped.refusal;
  EXPECT_EQ(kept.method, QuoteSurfaceMethod::ChannelRefit);
  EXPECT_EQ(kept.fit.diagnostics.repair.outcome, QuoteSliceRepairOutcome::NotAttempted);
  EXPECT_FALSE(kept.calendar_repaired);
  EXPECT_TRUE(kept.fit.moved_nodes.empty());
  EXPECT_EQ(dropped.grade, QuoteSliceGrade::Refused);
  EXPECT_EQ(dropped.refusal, "calendar_arbitrage");
  EXPECT_TRUE(dropped.channel_refit);
  EXPECT_EQ(dropped.fit.diagnostics.repair.outcome, QuoteSliceRepairOutcome::Applied);
}

// ── Opt-in per-chain fit timing (counters::fit_timing, L1 fit profile) ──────────

namespace ft = counters::fit_timing;

[[nodiscard]] std::uint32_t al_in(const ft::FitTimingCounters &t, ft::Bucket b) {
  return t.al_by_bucket[static_cast<unsigned>(b)];
}

// The buckets partition the chain wall and the AL work.
void expect_partitioned(const ft::FitTimingCounters &t) {
  EXPECT_EQ(t.wall_us, t.deam_us + t.solve_us + t.project_us + t.price_us + t.certify_us +
                           t.calendar_us + t.other_us);
  std::uint64_t al = 0;
  for (const std::uint32_t n : t.al_by_bucket) al += n;
  EXPECT_EQ(al, t.al_price_calls);
}

void expect_same_surface(const std::vector<QuoteSurfaceSlice> &a,
                         const std::vector<QuoteSurfaceSlice> &b) {
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t e = 0; e < a.size(); ++e) {
    EXPECT_EQ(a[e].grade, b[e].grade) << e;
    EXPECT_EQ(a[e].refusal, b[e].refusal) << e;
    EXPECT_EQ(a[e].method, b[e].method) << e;
    expect_same_prices(a[e].fit, b[e].fit);
    for (std::size_t i = 0; i < a[e].fit.rows.size() && i < b[e].fit.rows.size(); ++i) {
      EXPECT_EQ(a[e].fit.rows[i].evaluation_status, b[e].fit.rows[i].evaluation_status) << i;
    }
  }
}

void spin_for(std::chrono::microseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  for (std::uint64_t guard = 0; guard < (1ull << 40) && std::chrono::steady_clock::now() < until;
       ++guard) {
  }
}

TEST(QuoteSurfaceTiming, ArmedTraceCountsEachPointServedExpiryWithoutChangingTheFit) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  ASSERT_EQ(ft::armed_surface(), nullptr); // off by default
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  std::vector<ft::FitTimingCounters> counted;
  {
    ft::SurfaceTrace trace;
    ASSERT_EQ(ft::armed_surface(), &trace);
    const auto on = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
    ASSERT_TRUE(on) << on.error().to_string();
    expect_same_surface(*off, *on);
    counted = trace.chains;
  }
  EXPECT_EQ(ft::armed_surface(), nullptr);
  ASSERT_EQ(counted.size(), chains.size());
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const ft::FitTimingCounters &t = counted[e];
    ASSERT_EQ((*off)[e].grade, QuoteSliceGrade::A) << e;
    EXPECT_EQ(t.rung_attempts, 1u) << e;        // the point pass only
    EXPECT_EQ(t.certification_passes, 1u) << e; // one American reprice + certificate
    EXPECT_EQ(t.channel_solves, 0u) << e;
    EXPECT_EQ(t.jacobian_evals, 0u) << e;
    EXPECT_EQ(t.projections, 0u) << e;
    // Calendar order is the nearer maturity first: expiry e is checked against e others.
    EXPECT_EQ(t.calendar_pairs, e) << e;
    EXPECT_GT(t.iv_newton_iters, 0u) << e;                // mid IVs of the admitted quotes
    EXPECT_GT(al_in(t, ft::Bucket::Price), 0u) << e;      // the certification grid reprice
    EXPECT_EQ(al_in(t, ft::Bucket::Solve), 0u) << e;      // the curve solve prices no American
    EXPECT_EQ(al_in(t, ft::Bucket::Calendar), 0u) << e;   // pair checks read certified prices
    EXPECT_EQ(t.al_greeks_calls, 0u) << e;
    expect_partitioned(t);
  }
}

TEST(QuoteSurfaceTiming, KnotFirstLadderChargesBothPassARungsAndTheKnotChannelSolve) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, knot_first());
  ASSERT_TRUE(off) << off.error().to_string();
  ft::SurfaceTrace trace;
  const auto on = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, knot_first());
  ASSERT_TRUE(on) << on.error().to_string();
  expect_same_surface(*off, *on);
  ASSERT_EQ(trace.chains.size(), chains.size());
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const ft::FitTimingCounters &t = trace.chains[e];
    ASSERT_EQ((*on)[e].method, QuoteSurfaceMethod::Channel) << e;
    EXPECT_EQ(t.rung_attempts, 2u) << e;        // knot fit + point fit, eagerly
    EXPECT_EQ(t.channel_solves, 1u) << e;       // the knot fit's channel solve
    EXPECT_GE(t.jacobian_evals, 1u) << e;       // its Newton steps
    EXPECT_EQ(t.certification_passes, 2u) << e; // each rung certifies once
    EXPECT_EQ(t.calendar_pairs, e) << e;
    EXPECT_EQ(al_in(t, ft::Bucket::Solve), 0u) << e; // the channel solve is in vol space
    EXPECT_GT(al_in(t, ft::Bucket::Deam), 0u) << e;  // bid/ask/mid IVs and floor/cap prices
    expect_partitioned(t);
  }
}

TEST(QuoteSurfaceTiming, BucketsAreExclusiveAndAChainScopeIsAnOptInNoOp) {
  ft::ChainRecorder rec;
  {
    const ft::ChainScope off(nullptr, 5);
    const ft::BucketScope nothing(ft::Bucket::Deam); // no chain current: no-op
    ft::count(&ft::FitTimingCounters::projections);
  }
  {
    const ft::ChainScope chain(&rec, 1);
    const ft::BucketScope deam(ft::Bucket::Deam);
    spin_for(std::chrono::microseconds(2000));
    {
      const ft::BucketScope price(ft::Bucket::Price);
      ft::count(&ft::FitTimingCounters::projections, 3);
      const ft::ChainScope again(&rec, 1); // re-entering the current chain only counts
      spin_for(std::chrono::microseconds(3000));
    }
    spin_for(std::chrono::microseconds(1000));
  }
  const ft::FitTimingCounters t = rec.result();
  EXPECT_EQ(t.rung_attempts, 2u);
  EXPECT_EQ(t.projections, 3u);
  EXPECT_GE(t.price_us, 3000u);
  EXPECT_GE(t.deam_us, 3000u);        // 2 ms before and 1 ms after the nested price scope
  EXPECT_LT(t.deam_us, t.wall_us);    // ...and never the nested 3 ms
  EXPECT_GE(t.wall_us, 6000u);
  expect_partitioned(t);
}

TEST(QuoteSurfaceTiming, ARepairRungVisitIsAnInclusiveCrossCutOutsideTheBucketPartition) {
  ft::ChainRecorder rec;
  {
    const ft::RepairRungScope off; // no chain current: no-op
  }
  {
    const ft::ChainScope chain(&rec, 1);
    spin_for(std::chrono::microseconds(1000));
    const ft::RepairRungScope repair;
    const ft::BucketScope price(ft::Bucket::Price);
    spin_for(std::chrono::microseconds(2000));
  }
  const ft::FitTimingCounters t = rec.result();
  EXPECT_EQ(t.repair_rung_visits, 1u);
  EXPECT_GE(t.repair_rung_us, 2000u);         // inclusive of the nested price bucket
  EXPECT_LT(t.repair_rung_us, t.wall_us);     // the 1 ms before the visit is outside it
  EXPECT_GE(t.price_us, 2000u);               // the bucket still charges its own time
  expect_partitioned(t);                      // and the cross-cut is not in wall_us
}

TEST(QuoteSurfaceTiming, BothRepairRungsTimeOnlyTheExpiryTheyRefit) {
  // The fixture of QuoteSurfaceLadder.RepairRungServesTheKnotSliceItsChannelRepairCertifies:
  // N certifies at pass A; Z (micro-kinked carry zig-zag) fails both pass-A certificates
  // and both prior-pulled channel refits, so it reaches the repair rung still refused.
  const std::vector<Chain> chains{chain(0.25, 0.14, 0.0, 0.1, 85, 115, 5),
                                  zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05)};
  const QuoteSliceConfig config = channel_repair_config(3);
  QuoteSurfaceConfig graded;
  graded.term_prior_weight = 100.0;
  QuoteSurfaceConfig ladder = knot_first();
  ladder.term_prior_weight = 100.0;
  for (const QuoteSurfaceConfig &surface : {graded, ladder}) {
    const bool knot = surface.ladder == QuoteSurfaceLadder::KnotFirst;
    const auto off = fit_quote_surface(expiries(chains), config, surface);
    ASSERT_TRUE(off) << off.error().to_string();
    ft::SurfaceTrace trace;
    const auto on = fit_quote_surface(expiries(chains), config, surface);
    ASSERT_TRUE(on) << on.error().to_string();
    expect_same_surface(*off, *on);
    ASSERT_EQ(trace.chains.size(), chains.size()) << knot;
    ASSERT_EQ((*on)[1].point_status, QuoteSliceFitStatus::ArbitrageFailure) << knot;
    ASSERT_NE((*on)[1].rung_repair.outcome, QuoteSliceRepairOutcome::NotAttempted) << knot;
    EXPECT_EQ(trace.chains[0].repair_rung_visits, 0u) << knot;
    EXPECT_EQ(trace.chains[0].repair_rung_us, 0u) << knot;
    const ft::FitTimingCounters &z = trace.chains[1];
    EXPECT_EQ(z.repair_rung_visits, 1u) << knot;
    EXPECT_GT(z.repair_rung_us, 0u) << knot;
    // Inclusive, so within the chain wall (up to the per-bucket microsecond rounding).
    EXPECT_LE(z.repair_rung_us, z.wall_us + ft::kBucketCount) << knot;
    expect_partitioned(z);
  }
}

// ── Opt-in rung memo, pre-screen, fan-out errors (lane L2c) ──────────────────

QuoteSliceConfig vol_keys(QuoteSliceConfig config) {
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  config.channel.deam_cache = true;
  return config;
}

// Every served or reported value of two slice fits, bit for bit.
void expect_same_fit(const QuoteSliceFit &a, const QuoteSliceFit &b) {
  const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
  EXPECT_EQ(a.fit_status, b.fit_status);
  EXPECT_EQ(a.fit_error, b.fit_error);
  EXPECT_EQ(a.moved_nodes, b.moved_nodes);
  EXPECT_EQ(a.price_only_nodes, b.price_only_nodes);
  ASSERT_EQ(a.rows.size(), b.rows.size());
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    EXPECT_EQ(a.rows[i].quote_status, b.rows[i].quote_status) << i;
    EXPECT_EQ(a.rows[i].evaluation_status, b.rows[i].evaluation_status) << i;
    EXPECT_EQ(bits(a.rows[i].market_iv), bits(b.rows[i].market_iv)) << i;
    EXPECT_EQ(bits(a.rows[i].fitted_iv), bits(b.rows[i].fitted_iv)) << i;
    EXPECT_EQ(bits(a.rows[i].fitted_price), bits(b.rows[i].fitted_price)) << i;
  }
  ASSERT_EQ(a.prices.has_value(), b.prices.has_value());
  if (!a.prices) return;
  const auto na = a.prices->nodes();
  const auto nb = b.prices->nodes();
  ASSERT_EQ(na.size(), nb.size());
  for (std::size_t i = 0; i < na.size(); ++i) {
    EXPECT_EQ(bits(na[i].call), bits(nb[i].call)) << i;
    EXPECT_EQ(bits(na[i].put), bits(nb[i].put)) << i;
  }
}

// Runs `fit` on a fresh thread with `memo` installed there (null: none), as the
// surface fan-out runs every stage's expiry tasks on freshly spawned workers.
template <class Fit> auto on_fresh_thread(detail::ExpiryRungMemo *memo, const Fit &fit) {
  std::optional<decltype(fit())> out;
  std::jthread([&] {
    const detail::ScopedRungMemo installed(memo);
    out.emplace(fit());
  }).join();
  return std::move(*out);
}

// Brief acceptance: three rungs of one expiry -> one inversion pass + two hits, on
// whichever thread each rung runs. The thread-local memo (the historical carrier)
// misses on every fresh thread.
TEST(QuoteSurfaceRungMemo, ThreeRungsOfOneExpiryShareOneInversionPassOnAnyThread) {
  const Chain c = chain(0.5, 0.24, -0.08, 0.02);
  const QuoteSliceConfig config = vol_keys(QuoteSliceConfig{});
  const std::vector<double> z{-2.0, -1.0, 0.0, 1.0, 2.0};
  const std::vector<double> multiples{1.2, 1.08, 1.0, 0.96, 0.95};
  const QuoteSlicePrior prior{0.24, z, multiples, 0.5};
  const auto knot_rung = [&] {
    return fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
  };
  const auto point_rung = [&] { return fit_quote_slice(c.market, c.quotes, config); };
  const auto prior_rung = [&] {
    return fit_quote_slice_channel(c.market, c.quotes, config, &prior);
  };
  deam_iv_memo_clear();
  const auto knot_alone = on_fresh_thread(nullptr, knot_rung);
  const auto point_alone = on_fresh_thread(nullptr, point_rung);
  const auto prior_alone = on_fresh_thread(nullptr, prior_rung);
  ASSERT_TRUE(knot_alone && point_alone && prior_alone);
  EXPECT_FALSE(point_alone->diagnostics.deam.reused); // a fresh thread's memo is empty
  EXPECT_GT(point_alone->diagnostics.deam.inversions, 0u);

  detail::ExpiryRungMemo memo;
  const auto knot = on_fresh_thread(&memo, knot_rung);
  const auto point = on_fresh_thread(&memo, point_rung);
  const auto refit = on_fresh_thread(&memo, prior_rung);
  ASSERT_TRUE(knot && point && refit);
  EXPECT_EQ(memo.deam_misses(), 1u); // the one inversion pass
  EXPECT_EQ(memo.deam_hits(), 2u);
  EXPECT_FALSE(knot->diagnostics.deam.reused);
  EXPECT_GT(knot->diagnostics.deam.inversions, 0u);
  for (const QuoteSliceFit *fit : {&*point, &*refit}) {
    EXPECT_TRUE(fit->diagnostics.deam.reused);
    EXPECT_EQ(fit->diagnostics.deam.inversions, 0u); // nothing left to invert
    EXPECT_EQ(fit->diagnostics.deam.model_prices, 0u);
  }
  EXPECT_EQ(knot->fit_status, QuoteSliceFitStatus::Success) << knot->fit_error;
  expect_same_fit(*knot, *knot_alone);
  expect_same_fit(*point, *point_alone);
  expect_same_fit(*refit, *prior_alone);
  deam_iv_memo_clear();
}

// A repair rung that refits the pass-A candidate with the channel repair re-presents
// an unchanged candidate (same chain, same fitted volatility at every certification
// strike): it reuses the memoised American grid and serves exactly what a full
// reprice serves. Another curve (a prior) is a changed candidate and misses.
TEST(QuoteSurfaceRungMemo, ARepairRungReusesTheGridOfAnUnchangedCandidate) {
  const Chain zz = zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05);
  QuoteSliceConfig pass_a = channel_repair_config(3);
  pass_a.repair.mode = QuoteSliceRepairMode::Off; // pass A never runs the channel repair
  const QuoteSliceConfig rung = channel_repair_config(3);
  const std::vector<double> z{-1.0, 0.0, 1.0};
  const std::vector<double> multiples{1.05, 1.0, 1.0};
  const QuoteSlicePrior prior{0.25, z, multiples, 0.5};
  for (const bool knot : {true, false}) {
    SCOPED_TRACE(knot ? "knot rung" : "point rung");
    const auto fit = [&](const QuoteSliceConfig &config) {
      return knot ? fit_quote_slice_channel(zz.market, zz.quotes, config, nullptr)
                  : fit_quote_slice(zz.market, zz.quotes, config);
    };
    const auto pass_a_alone = fit(pass_a);
    const auto rung_alone = fit(rung);
    ASSERT_TRUE(pass_a_alone && rung_alone);
    ASSERT_EQ(pass_a_alone->fit_status, QuoteSliceFitStatus::ArbitrageFailure);
    ASSERT_NE(rung_alone->diagnostics.repair.outcome, QuoteSliceRepairOutcome::NotAttempted);
    detail::ExpiryRungMemo memo;
    const detail::ScopedRungMemo installed(&memo);
    const auto first = fit(pass_a);
    const auto second = fit(rung);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(memo.grid_misses(), 1u);
    EXPECT_EQ(memo.grid_hits(), 1u); // the rung did not reprice
    expect_same_fit(*first, *pass_a_alone);
    expect_same_fit(*second, *rung_alone);
    if (knot) {
      const auto changed = fit_quote_slice_channel(zz.market, zz.quotes, rung, &prior);
      ASSERT_TRUE(changed);
      EXPECT_EQ(memo.grid_misses(), 2u);
      EXPECT_EQ(memo.grid_hits(), 1u);
    }
  }
}

// Review Focus #2: memoised state never crosses a quote-set boundary. One changed
// bid misses both the IV cache and the grid, serves exactly what an unmemoised fit
// of the changed chain serves, and that differs from the unchanged chain's prices;
// the unchanged chain still hits afterwards.
TEST(QuoteSurfaceRungMemo, OneChangedBidMissesTheMemoAndChangesTheServedPrices) {
  const Chain c = chain(0.5, 0.24, -0.08, 0.02);
  Chain moved = c;
  const auto atm = std::find_if(moved.quotes.begin(), moved.quotes.end(), [](const auto &q) {
    return q.strike == 100.0 && q.side == Side::Call;
  });
  ASSERT_NE(atm, moved.quotes.end());
  atm->bid += 0.5 * (atm->ask - atm->bid);
  const QuoteSliceConfig config = vol_keys(QuoteSliceConfig{});
  deam_iv_memo_clear();
  const auto alone = fit_quote_slice_channel(moved.market, moved.quotes, config, nullptr);
  deam_iv_memo_clear();
  ASSERT_TRUE(alone);
  detail::ExpiryRungMemo memo;
  const detail::ScopedRungMemo installed(&memo);
  const auto original = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
  const auto changed = fit_quote_slice_channel(moved.market, moved.quotes, config, nullptr);
  ASSERT_TRUE(original && changed);
  EXPECT_EQ(memo.deam_misses(), 2u);
  EXPECT_EQ(memo.deam_hits(), 0u);
  EXPECT_EQ(memo.grid_misses(), 2u);
  EXPECT_EQ(memo.grid_hits(), 0u);
  EXPECT_FALSE(changed->diagnostics.deam.reused);
  expect_same_fit(*changed, *alone);
  ASSERT_EQ(original->fit_status, QuoteSliceFitStatus::Success) << original->fit_error;
  ASSERT_EQ(changed->fit_status, QuoteSliceFitStatus::Success) << changed->fit_error;
  std::size_t differing = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (original->rows[i].fitted_price != changed->rows[i].fitted_price) ++differing;
  }
  EXPECT_GT(differing, 0u);
  const auto again = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
  ASSERT_TRUE(again);
  EXPECT_EQ(memo.deam_hits(), 1u);
  EXPECT_EQ(memo.grid_hits(), 1u);
  expect_same_fit(*again, *original);
}

struct TracedSurface {
  std::vector<QuoteSurfaceSlice> slices;
  std::uint64_t deam_al = 0;  // AL solves in the deam bucket, summed over the chains
  std::uint64_t price_al = 0; // AL solves of the certification grids
};

TracedSurface traced_fit(const std::vector<Chain> &chains, const QuoteSliceConfig &config,
                         const QuoteSurfaceConfig &surface) {
  deam_iv_memo_clear(); // the calling thread starts with an empty thread-local memo
  TracedSurface out;
  ft::SurfaceTrace trace;
  auto fitted = fit_quote_surface(expiries(chains), config, surface);
  deam_iv_memo_clear();
  if (!fitted) {
    ADD_FAILURE() << fitted.error().to_string();
    return out;
  }
  out.slices = std::move(*fitted);
  for (const ft::FitTimingCounters &t : trace.chains) {
    out.deam_al += al_in(t, ft::Bucket::Deam);
    out.price_al += al_in(t, ft::Bucket::Price);
  }
  return out;
}

// The memo is a speed change only: bit-identical surfaces with it at every width, for
// the graded and the ladder admission, price and vol channels. Under the vol-space
// cache it makes the inversion work independent of the width: the memo at 2 workers
// inverts exactly what the serial thread-local memo inverts, and less than the
// thread-local memo at 2 workers (whose fresh worker threads miss across stages).
TEST(QuoteSurfaceRungMemo, SurfacesAreBitIdenticalAndInvertEachExpiryOnceAtAnyWidth) {
  const std::vector<Chain> chains{
      chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.15, 95, 105),
      chain(1.0, 0.26, -0.06, 0.01), chain(1.5, 0.12, 0.0, 0.005, 85, 115)};
  for (const bool ladder : {false, true}) {
    for (const bool vol : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "ladder " << ladder << " vol " << vol);
      const QuoteSliceConfig config = vol ? vol_keys(QuoteSliceConfig{}) : QuoteSliceConfig{};
      QuoteSurfaceConfig off = ladder ? knot_first() : QuoteSurfaceConfig{};
      off.fit_workers = 1;
      const TracedSurface reference = traced_fit(chains, config, off);
      ASSERT_EQ(reference.slices.size(), chains.size());
      EXPECT_TRUE(reference.slices[1].channel_refit); // the fixture reaches B1/B2
      QuoteSurfaceConfig off_wide = off;
      off_wide.fit_workers = 2;
      const TracedSurface thread_local_wide = traced_fit(chains, config, off_wide);
      expect_bit_identical(reference.slices, thread_local_wide.slices, 2);
      for (const unsigned workers : {1u, 2u, 0u}) {
        QuoteSurfaceConfig on = off;
        on.rung_memo = true;
        on.fit_workers = workers;
        const TracedSurface memo = traced_fit(chains, config, on);
        expect_bit_identical(reference.slices, memo.slices, workers);
        if (!vol) continue;
        EXPECT_EQ(memo.deam_al, reference.deam_al) << "workers " << workers;
        EXPECT_LT(memo.deam_al, thread_local_wide.deam_al) << "workers " << workers;
        EXPECT_LE(memo.price_al, reference.price_al) << "workers " << workers;
      }
    }
  }
}

// Brief item 6: under the vol-space cache the band fills are de-Am work: charged to
// the deam bucket, so a chain's "other" bucket prices no American option.
TEST(QuoteSurfaceRungMemo, VolSpaceBandFillsAreChargedToTheDeamBucket) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  QuoteSurfaceConfig surface = knot_first();
  surface.rung_memo = true;
  deam_iv_memo_clear();
  ft::SurfaceTrace trace;
  const auto fitted = fit_quote_surface(expiries(chains), vol_keys(QuoteSliceConfig{}), surface);
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  ASSERT_EQ(trace.chains.size(), chains.size());
  for (std::size_t e = 0; e < chains.size(); ++e) {
    const ft::FitTimingCounters &t = trace.chains[e];
    EXPECT_EQ(t.rung_attempts, 2u) << e; // knot fit + point fit
    EXPECT_GT(al_in(t, ft::Bucket::Deam), 0u) << e;
    EXPECT_EQ(al_in(t, ft::Bucket::Other), 0u) << e;
    expect_partitioned(t);
  }
  deam_iv_memo_clear();
}

// Deep in-the-money quotes whose asks sit below intrinsic value (and so below the
// minimum-volatility price): no quote has a usable bid/ask band. `atm_calls` adds
// that many well-posed near-the-money calls.
Chain empty_band_chain(int atm_calls) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, 0.5, 0.5, 0.03, 0.01, 0.0};
  for (int strike = 70; strike <= 90; strike += 5) {
    const double intrinsic = 100.0 - strike;
    out.quotes.push_back({static_cast<double>(strike), Side::Call, 0.5 * intrinsic,
                          0.9 * intrinsic});
    const double put_strike = 200.0 - strike;
    out.quotes.push_back({put_strike, Side::Put, 0.5 * (put_strike - 100.0),
                          0.9 * (put_strike - 100.0)});
  }
  for (int i = 0; i < atm_calls; ++i) {
    const double strike = 100.0 + 5.0 * i;
    const auto price = andersen_lake(100.0, strike, 0.5, 0.25, 0.03, 0.01, Side::Call);
    if (!price) {
      ADD_FAILURE() << price.error().to_string();
      return out;
    }
    out.quotes.push_back({strike, Side::Call, *price - 0.05, *price + 0.05});
  }
  return out;
}

// Brief item 3: the pre-solve screen refuses a constructed empty-band slice with its
// typed reason before any solve, in both channel spaces, exactly as today's refusal
// (InsufficientQuotes, same message); a slice with one usable band is TooFewChannels.
TEST(QuoteSurfaceRungMemo, PreScreenRefusesAnEmptyBandSliceBeforeAnySolve) {
  struct Case {
    int atm_calls;
    QuoteSlicePreScreen expected;
  };
  for (const Case &k : {Case{0, QuoteSlicePreScreen::EmptyBand},
                        Case{1, QuoteSlicePreScreen::TooFewChannels}}) {
    const Chain c = empty_band_chain(k.atm_calls);
    for (const bool vol : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "atm " << k.atm_calls << " vol " << vol);
      const QuoteSliceConfig config = vol ? vol_keys(QuoteSliceConfig{}) : QuoteSliceConfig{};
      const auto fit = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
      ASSERT_TRUE(fit) << fit.error().to_string();
      EXPECT_EQ(fit->diagnostics.channel.prescreen, k.expected)
          << to_string(fit->diagnostics.channel.prescreen);
      EXPECT_EQ(fit->fit_status, QuoteSliceFitStatus::InsufficientQuotes);
      EXPECT_EQ(fit->fit_error, "too few usable bid/ask channels");
      EXPECT_EQ(fit->diagnostics.channel.channel_quotes,
                static_cast<std::size_t>(k.atm_calls));
      EXPECT_EQ(fit->diagnostics.channel.iterations, 0u); // no solve ran
      EXPECT_FALSE(fit->prices.has_value());
    }
    deam_iv_memo_clear();
  }
  EXPECT_STREQ(to_string(QuoteSlicePreScreen::EmptyBand), "empty_band");
  EXPECT_STREQ(to_string(QuoteSlicePreScreen::TooFewChannels), "too_few_channels");
  EXPECT_STREQ(to_string(QuoteSlicePreScreen::Pass), "pass");
}

// ...and never refuses a slice the solve serves: over every chain shape of this file
// (table), each served channel fit, price or vol space, passed the screen.
TEST(QuoteSurfaceRungMemo, PreScreenPassesEverySliceTheSolveServes) {
  const std::vector<Chain> table{
      chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.15, 95, 105),
      chain(1.0, 0.26, -0.06, 0.01), chain(1.5, 0.12, 0.0, 0.005, 85, 115),
      chain(0.1, 0.35, -0.05, 0.08, 85, 115, 5, 0.04),
      zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05), empty_band_chain(3)};
  std::size_t served = 0;
  for (const Chain &c : table) {
    for (const bool vol : {false, true}) {
      const QuoteSliceConfig config = vol ? vol_keys(QuoteSliceConfig{}) : QuoteSliceConfig{};
      const auto fit = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
      ASSERT_TRUE(fit) << fit.error().to_string();
      if (fit->fit_status != QuoteSliceFitStatus::Success) continue;
      ++served;
      EXPECT_EQ(fit->diagnostics.channel.prescreen, QuoteSlicePreScreen::Pass);
    }
  }
  EXPECT_GE(served, 8u);
  deam_iv_memo_clear();
}

// review-L2d minor F2: a fan-out task that throws (here through the test-only task
// hook) is returned as the surface's typed error at every width; nothing escapes.
std::atomic<int> g_injected_kind{0};

void throw_on_expiry_one(std::size_t expiry) {
  if (expiry != 1) return;
  switch (g_injected_kind.load()) {
  case 0:
    throw std::bad_alloc();
  case 1:
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
  default:
    throw std::runtime_error("injected expiry failure");
  }
}

TEST(QuoteSurfaceRungMemo, AnExpiryTaskThatThrowsIsReturnedAsTheSurfaceError) {
  const std::vector<Chain> chains = widening_chains();
  const auto reference = fit_quote_surface(expiries(chains), QuoteSliceConfig{});
  ASSERT_TRUE(reference) << reference.error().to_string();
  struct Case {
    int kind;
    ErrorCode code;
  };
  for (const unsigned workers : {1u, 2u}) {
    for (const Case &k : {Case{0, ErrorCode::Unavailable}, Case{1, ErrorCode::Unavailable},
                          Case{2, ErrorCode::Internal}}) {
      SCOPED_TRACE(::testing::Message() << "workers " << workers << " kind " << k.kind);
      QuoteSurfaceConfig surface;
      surface.fit_workers = workers;
      g_injected_kind.store(k.kind);
      std::optional<Result<std::vector<QuoteSurfaceSlice>>> fitted;
      {
        const detail::ScopedExpiryTaskHook hook(&throw_on_expiry_one);
        EXPECT_NO_THROW(
            fitted.emplace(fit_quote_surface(expiries(chains), QuoteSliceConfig{}, surface)));
      }
      ASSERT_TRUE(fitted.has_value());
      ASSERT_FALSE(*fitted);
      EXPECT_EQ(fitted->error().code(), k.code) << fitted->error().to_string();
    }
  }
  QuoteSurfaceConfig wide;
  wide.fit_workers = 2;
  const auto again = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, wide);
  ASSERT_TRUE(again) << again.error().to_string();
  expect_bit_identical(*reference, *again, 2);
}

// ── Opt-in distance-to-market gate / repair (lane L3q market-distance) ──────

// The smile chain with its in-the-money call at 90 re-quoted `width` wide and
// `spreads` widths BELOW its model price. The point fit prices the out-of-the-money
// side only, so it serves that call near the model: about `spreads` spreads above
// its ask, while every other row sits inside its own quote.
Chain off_market_call(double spreads, double width) {
  Chain c = chain(0.25, 0.20, -0.10, 0.02);
  const double sigma = 0.20 - 0.10 * std::log(0.9);
  const auto model = andersen_lake(100.0, 90.0, 0.25, sigma, 0.03, 0.01, Side::Call);
  EXPECT_TRUE(model);
  for (auto &q : c.quotes) {
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

std::size_t row_index(const Chain &c, double strike, Side side) {
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (c.quotes[i].strike == strike && c.quotes[i].side == side) return i;
  }
  ADD_FAILURE() << "no quote at " << strike;
  return 0;
}

TEST(QuoteSurface, MarketDistanceGateRefusesOnlyTheRowOutsideItsQuote) {
  const std::vector<Chain> one{off_market_call(3.0, 0.04)};
  const std::size_t r = row_index(one[0], 90.0, Side::Call);
  const auto off = fit_quote_surface(expiries(one), QuoteSliceConfig{});
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ(off->size(), 1u);
  const QuoteSurfaceSlice &base = (*off)[0];
  ASSERT_NE(base.grade, QuoteSliceGrade::Refused) << base.refusal << ": " << base.error;
  ASSERT_EQ(base.fit.rows[r].evaluation_status, QuoteSliceEvaluationStatus::Success);
  const QuoteSliceQuote &q = one[0].quotes[r];
  const double distance = market_distance_spreads(base.fit.rows[r].fitted_price, q.bid, q.ask, 0.01);
  ASSERT_GT(distance, 2.0); // the constructed row: ~3 spreads above its ask
  EXPECT_EQ(base.market_checked_rows, 0u); // provenance stays at its default when off
  const auto gate = fit_quote_surface(expiries(one),
                                      market_distance_config(QuoteSliceMarketDistance::Gate));
  ASSERT_TRUE(gate) << gate.error().to_string();
  const QuoteSurfaceSlice &g = (*gate)[0];
  EXPECT_EQ(g.grade, base.grade); // the chain still serves
  EXPECT_EQ(g.market_gated_rows, 1u);
  EXPECT_NEAR(g.market_max_spreads, distance, 1.0e-12);
  EXPECT_GT(g.market_checked_rows, 1u);
  for (std::size_t i = 0; i < one[0].quotes.size(); ++i) {
    if (i == r) {
      EXPECT_EQ(g.fit.rows[i].evaluation_status, QuoteSliceEvaluationStatus::MarketDistanceFailure);
      EXPECT_TRUE(std::isnan(g.fit.rows[i].fitted_iv));
      continue;
    }
    EXPECT_EQ(g.fit.rows[i].evaluation_status, base.fit.rows[i].evaluation_status) << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(g.fit.rows[i].fitted_price),
              std::bit_cast<std::uint64_t>(base.fit.rows[i].fitted_price))
        << i;
  }
  EXPECT_STREQ(to_string(QuoteSliceEvaluationStatus::MarketDistanceFailure),
               "market_distance_failure");
  // A bound beyond the row's distance is inert: every row as without the gate.
  const auto loose = fit_quote_surface(
      expiries(one), market_distance_config(QuoteSliceMarketDistance::Gate, 10.0));
  ASSERT_TRUE(loose) << loose.error().to_string();
  EXPECT_EQ((*loose)[0].market_gated_rows, 0u);
  for (std::size_t i = 0; i < one[0].quotes.size(); ++i) {
    EXPECT_EQ((*loose)[0].fit.rows[i].evaluation_status, base.fit.rows[i].evaluation_status) << i;
  }
}

TEST(QuoteSurface, MarketDistanceRepairServesTheRowInsideItsQuoteWithTheCertificateIntact) {
  const std::vector<Chain> one{off_market_call(3.0, 0.04)};
  const std::size_t r = row_index(one[0], 90.0, Side::Call);
  const auto repair = fit_quote_surface(
      expiries(one), market_distance_config(QuoteSliceMarketDistance::Repair));
  ASSERT_TRUE(repair) << repair.error().to_string();
  const QuoteSurfaceSlice &s = (*repair)[0];
  ASSERT_NE(s.grade, QuoteSliceGrade::Refused) << s.refusal << ": " << s.error;
  EXPECT_EQ(s.market_repair.outcome, QuoteSliceRepairOutcome::Applied)
      << to_string(s.market_repair.outcome);
  EXPECT_GT(s.market_max_spreads, 2.0);
  EXPECT_LE(s.market_max_spreads_after, 1.0e-9);
  EXPECT_EQ(s.market_gated_rows, 0u);
  EXPECT_EQ(s.grade, QuoteSliceGrade::B); // its served prices left the fit
  const QuoteSliceRow &row = s.fit.rows[r];
  ASSERT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
  EXPECT_GE(row.fitted_price, one[0].quotes[r].bid - 1.0e-9);
  EXPECT_LE(row.fitted_price, one[0].quotes[r].ask + 1.0e-9);
  ASSERT_TRUE(s.fit.prices);
  EXPECT_TRUE(AmericanPriceSlice::create(s.fit.prices->nodes(), s.fit.prices->bounds()));
  EXPECT_FALSE(s.fit.moved_nodes.empty());
  expect_moved_nodes_reimplied(s.fit, one[0].quotes);
}

TEST(QuoteSurface, MarketDistanceConfigurationIsValidatedAtTheSharedBoundary) {
  QuoteSliceRepairConfig config;
  EXPECT_EQ(config.market_distance, QuoteSliceMarketDistance::Off);
  config.market_distance = QuoteSliceMarketDistance::Repair;
  config.market_distance_spreads = 0.0; // strict containment is allowed
  EXPECT_TRUE(validate_quote_slice_repair(config));
  config.market_distance_spreads = -1.0;
  EXPECT_FALSE(validate_quote_slice_repair(config));
  config.market_distance_spreads = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(validate_quote_slice_repair(config));
  config.market_distance_spreads = 1.0;
  config.market_distance = static_cast<QuoteSliceMarketDistance>(7);
  EXPECT_FALSE(validate_quote_slice_repair(config));
}

// ── Opt-in ladder wing tail (lane L3a) ───────────────────────────────────────

// Rows of a served slice re-implied (moved node side) at exactly the floor kIvMin.
std::size_t floor_reimplied_rows(const QuoteSliceFit &fit,
                                 std::span<const QuoteSliceQuote> quotes) {
  if (!fit.prices || fit.moved_nodes.empty()) return 0;
  const auto nodes = fit.prices->nodes();
  std::size_t count = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &row = fit.rows[i];
    if (row.evaluation_status != QuoteSliceEvaluationStatus::Success || row.fitted_iv != kIvMin) {
      continue;
    }
    const auto at = std::find_if(nodes.begin(), nodes.end(), [&](const AmericanPriceNode &n) {
      return n.strike == quotes[i].strike;
    });
    const unsigned bit = quotes[i].side == Side::Call ? 1u : 2u;
    if (at != nodes.end() &&
        (fit.moved_nodes[static_cast<std::size_t>(at - nodes.begin())] & bit) != 0u) {
      ++count;
    }
  }
  return count;
}

// A dense 36-day chain (strikes 60-140 every 4, volatility `sigma`) around a
// non-integer spot. Below 90 the supplied yield zig-zags (0.06 / 0), so the fitted
// deep in-the-money call prices are locally non-convex and the bounded convexity
// repair lowers sides onto their convex minorant; the high-yield calls sit in the
// exercise region at S - K, where the zero-volatility value is S - K itself.
Chain wing_tail_chain(double spot, double years, double sigma) {
  Chain out;
  out.market = QuoteSliceMarket{spot, years, years, 0.03, 0.0, 0.0};
  for (int strike = 60; strike <= 140; strike += 4) {
    const double q = strike < 90 && ((strike - 60) / 4) % 2 == 0 ? 0.06 : 0.0;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price =
          andersen_lake(spot, static_cast<double>(strike), years, sigma, 0.03, q, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * 0.05);
      out.quotes.push_back({static_cast<double>(strike), side, std::max(*price - spread, 0.0),
                            *price + spread, q});
    }
  }
  return out;
}

// The ladder with the convexity repair accepting any move and both containment
// limits relaxed, so the repaired channel refit of wing_tail_chain serves.
QuoteSliceConfig wing_config() {
  QuoteSliceConfig config;
  config.channel.repair_half_spreads = 1.0e6;
  return config;
}

QuoteSurfaceConfig wing_surface(QuoteSurfaceWingTail tail) {
  QuoteSurfaceConfig surface = knot_first();
  surface.ladder_max_outside_fraction = 1.0;
  surface.ladder_channel_max_outside_fraction = 1.0;
  surface.ladder_wing_tail = tail;
  return surface;
}

TEST(QuoteSurfaceLadder, WingTailFloorPriceOnlyRefusesAVolatilityReimpliedAtTheFloor) {
  // The convexity repair lowers the exercised 68 call (yield 0.06) to S - K, its
  // own zero-volatility value: the re-implied volatility is the inverter's floor
  // kIvMin, a bound 14.5 vol points from the chain's 0.15. With the key off it is
  // served as a volatility; FloorPriceOnly publishes that row PriceOnly and changes
  // nothing else: every price, every other row's volatility and status, the grade.
  constexpr double kSigma = 0.15;
  const std::vector<Chain> chains{wing_tail_chain(99.71, 0.1, kSigma)};
  const auto &quotes = chains[0].quotes;
  const auto off =
      fit_quote_surface(expiries(chains), wing_config(), wing_surface(QuoteSurfaceWingTail::Off));
  ASSERT_TRUE(off) << off.error().to_string();
  const auto on = fit_quote_surface(expiries(chains), wing_config(),
                                    wing_surface(QuoteSurfaceWingTail::FloorPriceOnly));
  ASSERT_TRUE(on) << on.error().to_string();
  const QuoteSurfaceSlice &a = (*off)[0];
  const QuoteSurfaceSlice &b = (*on)[0];
  ASSERT_NE(a.grade, QuoteSliceGrade::Refused) << a.refusal << " " << a.error;
  ASSERT_FALSE(a.fit.moved_nodes.empty());
  const std::size_t floor_rows = floor_reimplied_rows(a.fit, quotes);
  ASSERT_GT(floor_rows, 0u) << "fixture broken: no re-implied floor volatility";
  EXPECT_EQ(a.wing_tail_price_only, 0u);
  const std::size_t price_only_off = expect_price_only_rows(a.fit, quotes);

  EXPECT_EQ(floor_reimplied_rows(b.fit, quotes), 0u);
  EXPECT_EQ(b.wing_tail_price_only, floor_rows);
  EXPECT_EQ(expect_price_only_rows(b.fit, quotes), price_only_off + floor_rows);
  EXPECT_EQ(b.grade, a.grade);
  EXPECT_EQ(b.method, a.method);
  EXPECT_EQ(b.fit.moved_nodes, a.fit.moved_nodes);
  ASSERT_EQ(b.fit.rows.size(), a.fit.rows.size());
  std::size_t refused = 0;
  for (std::size_t i = 0; i < a.fit.rows.size(); ++i) {
    const QuoteSliceRow &x = a.fit.rows[i];
    const QuoteSliceRow &y = b.fit.rows[i];
    EXPECT_EQ(std::bit_cast<std::uint64_t>(x.fitted_price),
              std::bit_cast<std::uint64_t>(y.fitted_price)) << i;
    const bool floor = x.evaluation_status == QuoteSliceEvaluationStatus::Success &&
                       x.fitted_iv == kIvMin;
    if (floor) {
      // The tail row: served 0.005 against 0.15 with the key off; no volatility on.
      EXPECT_GT(std::abs(x.fitted_iv - kSigma), 0.1) << i;
      EXPECT_EQ(y.evaluation_status, QuoteSliceEvaluationStatus::PriceOnly) << i;
      EXPECT_TRUE(std::isnan(y.fitted_iv)) << i;
      ++refused;
      continue;
    }
    EXPECT_EQ(y.evaluation_status, x.evaluation_status) << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(y.fitted_iv), std::bit_cast<std::uint64_t>(x.fitted_iv))
        << i;
  }
  EXPECT_EQ(refused, floor_rows);
  EXPECT_STREQ(to_string(QuoteSurfaceWingTail::Off), "off");
  EXPECT_STREQ(to_string(QuoteSurfaceWingTail::FloorPriceOnly), "floor-price-only");
}

TEST(QuoteSurfaceLadder, WingTailIsANoOpWithoutAReimpliedFloorVolatility) {
  // The pass-A knot chains: no repair moves a node, so the key changes nothing.
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  const auto off = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, knot_first());
  ASSERT_TRUE(off) << off.error().to_string();
  QuoteSurfaceConfig surface = knot_first();
  surface.ladder_wing_tail = QuoteSurfaceWingTail::FloorPriceOnly;
  const auto on = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, surface);
  ASSERT_TRUE(on) << on.error().to_string();
  expect_bit_identical(*off, *on, 1);
  for (const QuoteSurfaceSlice &slice : *on) EXPECT_EQ(slice.wing_tail_price_only, 0u);
}

TEST(QuoteSurfaceLadder, WingFloorVariantServesTheWingFlooredKnotSlice) {
  // Put-skewed chains whose OTM wings beyond |k| 0.15 are bidless. Whenever the knot
  // rung serves, FloorPriceOnlyWingFloor's knot slice is the standalone channel fit
  // with the wing floor and FloorPriceOnly's the plain one; on at least one chain the
  // floor binds (the two differ). A floored fit that fails its certificate is never
  // served by the knot rung (the point fit or the channel passes take the slice).
  std::size_t bound = 0;
  for (const double skew : {-0.10, -0.20, -0.30}) {
    SCOPED_TRACE(::testing::Message() << "skew " << skew);
    const std::vector<Chain> chains{chain(0.25, 0.22, skew, 0.01, 60, 140, 5, 0.15)};
    QuoteSurfaceConfig surface = knot_first();
    surface.ladder_max_outside_fraction = 1.0;
    surface.ladder_channel_max_outside_fraction = 1.0;
    surface.ladder_wing_tail = QuoteSurfaceWingTail::FloorPriceOnlyWingFloor;
    const auto floored = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, surface);
    surface.ladder_wing_tail = QuoteSurfaceWingTail::FloorPriceOnly;
    const auto plain = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, surface);
    ASSERT_TRUE(floored) << floored.error().to_string();
    ASSERT_TRUE(plain) << plain.error().to_string();
    const QuoteSurfaceSlice &a = (*floored)[0];
    const QuoteSurfaceSlice &b = (*plain)[0];
    QuoteSliceConfig config;
    config.channel.wing_floor = true;
    const auto knot = fit_quote_slice_channel(chains[0].market, chains[0].quotes, config, nullptr);
    const auto unfloored =
        fit_quote_slice_channel(chains[0].market, chains[0].quotes, QuoteSliceConfig{}, nullptr);
    ASSERT_TRUE(knot) << knot.error().to_string();
    ASSERT_TRUE(unfloored) << unfloored.error().to_string();
    const bool a_knot = a.method == QuoteSurfaceMethod::Channel && a.grade == QuoteSliceGrade::A;
    if (a_knot) expect_same_prices(a.fit, *knot);
    if (b.method == QuoteSurfaceMethod::Channel && b.grade == QuoteSliceGrade::A) {
      expect_same_prices(b.fit, *unfloored);
    }
    if (a_knot && unfloored->fit_status == QuoteSliceFitStatus::Success) {
      for (std::size_t i = 0; i < knot->rows.size(); ++i) {
        if (knot->rows[i].fitted_iv != unfloored->rows[i].fitted_iv) {
          ++bound;
          break;
        }
      }
    }
  }
  EXPECT_GT(bound, 0u) << "fixture broken: no served knot slice where the wing floor binds";
  EXPECT_STREQ(to_string(QuoteSurfaceWingTail::FloorPriceOnlyWingFloor),
               "floor-price-only+wing-floor");
}

TEST(QuoteSurfaceLadder, WingTailRequiresTheKnotFirstLadderAndAKnownMode) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01)};
  QuoteSurfaceConfig no_ladder;
  no_ladder.ladder_wing_tail = QuoteSurfaceWingTail::FloorPriceOnly;
  const auto refused = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, no_ladder);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  QuoteSurfaceConfig unknown = knot_first();
  unknown.ladder_wing_tail = static_cast<QuoteSurfaceWingTail>(7);
  const auto bad = fit_quote_surface(expiries(chains), QuoteSliceConfig{}, unknown);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
  EXPECT_STREQ(to_string(static_cast<QuoteSurfaceWingTail>(7)), "unknown");
}

// review-SPY1s-p1 minor (confirm-solve naming / reason-split counters untested): under the
// Screen-seeded Premium cache (with the Phase 2 routing gate kappa and sigma-family keys) the
// fit_timing reason split partitions every inversion's AL solves - iterate + confirm + gate +
// search == deam_inversion_al - and deam_confirms counts confirming SOLVES (= the confirm
// reason; a root can take two), at least one per confirmed root; the whole split (with the
// band edges' range_edge / ambiguous_edge reasons - none on these point-served chains) never
// exceeds the chain's AL work; family builds / prices are counted beside it. Arming the trace
// changes no fit.
TEST(QuoteSurfaceTiming, ScreenSeededReasonSplitPartitionsInversionSolvesAndCountsConfirmSolves) {
  const std::vector<Chain> chains{chain(0.25, 0.22, -0.10, 0.01), chain(0.5, 0.24, -0.08, 0.01),
                                  chain(1.0, 0.26, -0.06, 0.01)};
  QuoteSliceConfig config = vol_keys(QuoteSliceConfig{});
  config.channel.deam_profile = QuoteSliceDeamProfile::Screen;
  config.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
  config.channel.deam_seed_gate_kappa = 4.3e-5;
  config.channel.deam_family = QuoteSliceDeamFamily::Always;
  QuoteSurfaceConfig surface;
  surface.fit_workers = 1;
  deam_iv_memo_clear();
  const auto off = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(off) << off.error().to_string();
  const TracedSurface traced = traced_fit(chains, config, surface);
  ASSERT_EQ(traced.slices.size(), chains.size());
  expect_same_surface(*off, traced.slices);
  deam_iv_memo_clear();
  ft::SurfaceTrace trace;
  const auto on = fit_quote_surface(expiries(chains), config, surface);
  ASSERT_TRUE(on) << on.error().to_string();
  deam_iv_memo_clear();
  ASSERT_EQ(trace.chains.size(), chains.size());
  const auto reason = [](const ft::FitTimingCounters &t, ft::DeamReason r) -> std::uint64_t {
    return t.deam_al_by_reason[static_cast<std::size_t>(r)];
  };
  std::uint64_t confirms = 0, premium = 0, family_prices = 0, family_builds = 0;
  for (std::size_t e = 0; e < trace.chains.size(); ++e) {
    const ft::FitTimingCounters &t = trace.chains[e];
    const std::uint64_t in_inversions =
        reason(t, ft::DeamReason::Iterate) + reason(t, ft::DeamReason::Confirm) +
        reason(t, ft::DeamReason::Gate) + reason(t, ft::DeamReason::Search);
    EXPECT_EQ(in_inversions, t.deam_inversion_al) << e;
    EXPECT_EQ(t.deam_confirms, reason(t, ft::DeamReason::Confirm)) << e; // SOLVES, not roots
    EXPECT_EQ(reason(t, ft::DeamReason::Gate), 0u) << e; // confirm always: no gate solve
    const std::uint64_t accepted = t.deam_premium - t.deam_premium_fallbacks;
    const std::uint64_t confirmed = accepted - std::min<std::uint64_t>(t.deam_seed_routed, accepted);
    EXPECT_GE(t.deam_confirms, confirmed) << e; // every non-routed accepted root confirmed
    EXPECT_LE(t.deam_confirms, 2u * t.deam_premium) << e; // kIvPremiumConfirmSolves = 2
    std::uint64_t split = 0;
    for (const std::uint32_t n : t.deam_al_by_reason) split += n;
    EXPECT_LE(split, t.al_price_calls) << e;
    expect_partitioned(t);
    confirms += t.deam_confirms;
    premium += t.deam_premium;
    family_prices += t.deam_family_prices;
    family_builds += t.deam_family_builds;
  }
  EXPECT_GT(premium, 0u);
  EXPECT_GT(confirms, 0u);
  EXPECT_GT(family_builds, 0u);
  EXPECT_GT(family_prices, 0u);
  expect_same_surface(*off, *on);
}

} // namespace
} // namespace atx::vol
