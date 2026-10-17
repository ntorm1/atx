#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
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
void expect_moved_nodes_reimplied(const QuoteSliceFit &fit,
                                  std::span<const QuoteSliceQuote> quotes) {
  ASSERT_TRUE(fit.prices);
  const auto nodes = fit.prices->nodes();
  ASSERT_EQ(fit.moved_nodes.size(), nodes.size());
  std::size_t checked = 0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &quote = quotes[i];
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

TEST(QuoteSurface, MovedNodeWithoutAnIdentifiableVolatilityIsNeverServedWithTheCurveVol) {
  // A calendar floor at the certificate's upper edge (call S - 1e-4 K, put
  // K (1 - 2e-4): linear, so convex, and inside every price bound) lifts every
  // node above any finite-volatility American price: the served prices have no
  // identifiable volatility, so the rows must not keep the curve's volatility.
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
  std::size_t invalid = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const auto &row = fit->rows[i];
    if (row.evaluation_status != QuoteSliceEvaluationStatus::InvalidVolatility) continue;
    ++invalid;
    EXPECT_TRUE(std::isnan(row.fitted_iv)) << i;
    EXPECT_TRUE(std::isnan(row.fitted_price)) << i;
    EXPECT_FALSE(evaluate_quote_slice(*fit, c.quotes[i].strike, c.quotes[i].side)) << i;
  }
  EXPECT_GT(invalid, 0u);
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
  // No moved price ends farther outside its own quote than the point fit put it.
  const auto point = fit_quote_slice(chains[1].market, chains[1].quotes);
  ASSERT_TRUE(point);
  for (std::size_t i = 0; i < chains[1].quotes.size(); ++i) {
    const auto &quote = chains[1].quotes[i];
    if (fixed.fit.rows[i].evaluation_status != QuoteSliceEvaluationStatus::Success) continue;
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

} // namespace
} // namespace atx::vol
