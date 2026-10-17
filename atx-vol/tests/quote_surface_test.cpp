#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"

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

} // namespace
} // namespace atx::vol
