// Lane C0 (2026-10-22): the identifiability contract of served volatilities. A price
// with no time value - a deep in-the-money put at its exercise value - is served by the
// historical graded-v1 path with the inverter's floor kIvMin as if it were an identified
// volatility; QuoteSliceFloorPolicy::PriceOnly publishes it PriceOnly (FloorVol) with
// the same price, and the library query never returns the floor as an identified IV.
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_fit_policy.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

// The L3a-b probe's 400-step CRR American pricer (probe.py), operation for operation, so
// the chain below is the CLI fixture's chain to the last bit.
double crr_american(double spot, double strike, double years, double sigma, double rate,
                    double q, bool call) {
  constexpr int kSteps = 400;
  const double dt = years / kSteps;
  const double u = std::exp(sigma * std::sqrt(dt));
  const double d = 1.0 / u;
  const double p = (std::exp((rate - q) * dt) - d) / (u - d);
  const double disc = std::exp(-rate * dt);
  std::vector<double> values;
  values.reserve(kSteps + 1);
  for (int j = 0; j <= kSteps; ++j) {
    const double s = spot * std::pow(u, static_cast<double>(j)) *
                     std::pow(d, static_cast<double>(kSteps - j));
    values.push_back(call ? std::max(s - strike, 0.0) : std::max(strike - s, 0.0));
  }
  for (int i = kSteps - 1; i >= 0; --i) {
    for (int j = 0; j <= i; ++j) {
      const double s = spot * std::pow(u, static_cast<double>(j)) *
                       std::pow(d, static_cast<double>(i - j));
      const double cont = disc * (p * values[static_cast<std::size_t>(j) + 1] +
                                  (1.0 - p) * values[static_cast<std::size_t>(j)]);
      const double ex = call ? s - strike : strike - s;
      values[static_cast<std::size_t>(j)] = std::max(cont, ex);
    }
  }
  return values[0];
}

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// The L3a-b wing chain exactly as the CLI fixture writes it (probe.py write_chain): spot
// 99.71, T 0.1, sigma 0.15, r 0.03, strikes 60-140 every 4 (call then put), a 0.06 yield
// on alternate strikes below 90, +/- 5 % spreads (at least 0.01). The market yield is the
// chain's median contract yield (0), as quote_bench chain_market takes it.
Chain wing_chain() {
  Chain out;
  out.market = QuoteSliceMarket{99.71, 0.1, 0.1, 0.03, 0.0, 0.0};
  for (int strike = 60; strike <= 140; strike += 4) {
    const double q = strike < 90 && ((strike - 60) / 4) % 2 == 0 ? 0.06 : 0.0;
    for (const bool call : {true, false}) {
      const double k = static_cast<double>(strike);
      const double price = crr_american(99.71, k, 0.1, 0.15, 0.03, q, call);
      const double spread = std::max(0.01, price * 0.05);
      out.quotes.push_back({k, call ? Side::Call : Side::Put, std::max(price - spread, 0.0),
                            price + spread, q});
    }
  }
  return out;
}

constexpr std::size_t kDeepItmPut = 39; // K 136 put: row_id 39 of the CLI fixture

// The CLI's graded-v1 historical arm on this chain (probe_c0.py graded-historical): the
// "ordinary" profile it resolves, channel.repair_half_spreads 0.5, the funding-PV dividend
// model, the price-space channel with the library's inverter / grid controls (the R3 kill
// switch); surface calendar_repair_half_spreads 0.5, no rung memo.
QuoteSliceConfig graded_v1(QuoteSliceFloorPolicy policy) {
  QuoteSliceConfig config;
  for (const QuoteFitProfile &profile : default_quote_fit_profiles()) {
    if (profile.name == "ordinary") config = profile.config;
  }
  config.channel.repair_half_spreads = 0.5;
  config.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
  config.channel.fit_space = QuoteSliceFitSpace::PriceChannel;
  config.grid_kernel = QuoteSliceGridKernel::Accurate;
  config.floor_policy = policy;
  return config;
}

QuoteSurfaceConfig graded_v1_surface() {
  QuoteSurfaceConfig surface;
  surface.calendar_repair_half_spreads = 0.5;
  surface.rung_memo = false;
  return surface;
}

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

TEST(QuoteSliceIdentification, DeepItmPutAtIntrinsicIsServedFloorVolPriceOnlyWithItsPrice) {
  const Chain c = wing_chain();
  const std::array<QuoteSurfaceExpiry, 1> expiry{{{c.market, c.quotes, {}}}};
  const auto legacy = fit_quote_surface(expiry, graded_v1(QuoteSliceFloorPolicy::Legacy),
                                        graded_v1_surface());
  const auto typed = fit_quote_surface(expiry, graded_v1(QuoteSliceFloorPolicy::PriceOnly),
                                       graded_v1_surface());
  ASSERT_TRUE(legacy) << legacy.error().to_string();
  ASSERT_TRUE(typed) << typed.error().to_string();
  const QuoteSurfaceSlice &a = (*legacy)[0];
  const QuoteSurfaceSlice &b = (*typed)[0];
  ASSERT_NE(a.grade, QuoteSliceGrade::Refused) << a.refusal << " " << a.error;
  ASSERT_EQ(a.fit.rows.size(), c.quotes.size());
  ASSERT_EQ(b.fit.rows.size(), c.quotes.size());

  // Legacy is the historical serve record: the floor bound as a Success volatility, at
  // the put's exercise value 136 - 99.71.
  const QuoteSliceRow &floor_row = a.fit.rows[kDeepItmPut];
  ASSERT_EQ(floor_row.evaluation_status, QuoteSliceEvaluationStatus::Success)
      << "fixture broken: the deep ITM put is no longer served";
  ASSERT_EQ(floor_row.fitted_iv, kIvMin) << "fixture broken: no floor volatility";
  EXPECT_NEAR(floor_row.fitted_price, 136.0 - 99.71, 1.0e-9);
  EXPECT_EQ(vol_convention(floor_row), QuoteSliceVolConvention::ReimpliedRepaired);

  // PriceOnly: the same certified price, no volatility, the typed reason and interval.
  const QuoteSliceRow &row = b.fit.rows[kDeepItmPut];
  EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::PriceOnly);
  EXPECT_EQ(row.price_only_reason, QuoteSlicePriceOnlyReason::FloorVol);
  EXPECT_EQ(bits(row.fitted_price), bits(floor_row.fitted_price));
  EXPECT_TRUE(std::isnan(row.fitted_iv));
  EXPECT_EQ(vol_convention(row), QuoteSliceVolConvention::None);
  // On the exercise plateau every volatility up to its edge prices the exercise value,
  // so the interval reaches past the chain's 0.15 instead of claiming [0, kIvMin].
  EXPECT_EQ(row.iv_lower, 0.0);
  EXPECT_GT(row.iv_upper, 0.15);
  EXPECT_LE(row.iv_upper, kIvMax);

  // Nothing else changes: every other row's status, volatility and price, the grade,
  // and the moved nodes.
  EXPECT_EQ(b.grade, a.grade);
  EXPECT_EQ(b.fit.moved_nodes, a.fit.moved_nodes);
  std::size_t floor_rows = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const QuoteSliceRow &x = a.fit.rows[i];
    const QuoteSliceRow &y = b.fit.rows[i];
    EXPECT_EQ(bits(y.fitted_price), bits(x.fitted_price)) << i;
    if (x.evaluation_status == QuoteSliceEvaluationStatus::Success && x.fitted_iv <= kIvMin) {
      ++floor_rows;
      EXPECT_EQ(y.evaluation_status, QuoteSliceEvaluationStatus::PriceOnly) << i;
      continue;
    }
    EXPECT_EQ(y.evaluation_status, x.evaluation_status) << i;
    EXPECT_EQ(bits(y.fitted_iv), bits(x.fitted_iv)) << i;
  }
  EXPECT_EQ(floor_rows, 1u);

  // The library query never returns the floor as an identified IV, under either policy;
  // its typed form carries the price, the reason and the same interval.
  for (const QuoteSliceFit *fit : {&a.fit, &b.fit}) {
    const auto value = evaluate_quote_slice(*fit, 136.0, Side::Put);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().code(), ErrorCode::OutOfRange);
    const auto answer = query_quote_slice(*fit, 136.0, Side::Put);
    ASSERT_TRUE(answer) << answer.error().to_string();
    EXPECT_EQ(answer->status, QuoteSliceEvaluationStatus::PriceOnly);
    EXPECT_EQ(answer->reason, QuoteSlicePriceOnlyReason::FloorVol);
    EXPECT_EQ(answer->vol_convention, QuoteSliceVolConvention::None);
    EXPECT_EQ(bits(answer->price), bits(floor_row.fitted_price));
    EXPECT_TRUE(std::isnan(answer->volatility));
    EXPECT_EQ(answer->iv_lower, 0.0);
    EXPECT_EQ(bits(answer->iv_upper), bits(row.iv_upper));
  }
}

TEST(QuoteSliceIdentification, IdentifiedRowsAndQueriesReportTheirVolatilityConvention) {
  const Chain c = wing_chain();
  const std::array<QuoteSurfaceExpiry, 1> expiry{{{c.market, c.quotes, {}}}};
  const auto fitted = fit_quote_surface(expiry, graded_v1(QuoteSliceFloorPolicy::PriceOnly),
                                        graded_v1_surface());
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  const QuoteSliceFit &fit = (*fitted)[0].fit;
  ASSERT_TRUE(fit.prices);
  const auto nodes = fit.prices->nodes();
  std::size_t curve = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const QuoteSliceRow &row = fit.rows[i];
    if (row.evaluation_status != QuoteSliceEvaluationStatus::Success) continue;
    const auto at = std::find_if(nodes.begin(), nodes.end(), [&](const AmericanPriceNode &n) {
      return n.strike == c.quotes[i].strike;
    });
    ASSERT_NE(at, nodes.end());
    const auto node = static_cast<std::size_t>(at - nodes.begin());
    const unsigned bit = c.quotes[i].side == Side::Call ? 1u : 2u;
    const bool moved = node < fit.moved_nodes.size() && (fit.moved_nodes[node] & bit) != 0u;
    const QuoteSliceVolConvention expected = moved ? QuoteSliceVolConvention::ReimpliedRepaired
                                                   : QuoteSliceVolConvention::FittedCurve;
    EXPECT_EQ(vol_convention(row), expected) << i;
    if (!moved) ++curve;
    const auto answer = query_quote_slice(fit, c.quotes[i].strike, c.quotes[i].side);
    ASSERT_TRUE(answer) << i << ": " << answer.error().to_string();
    EXPECT_EQ(answer->status, QuoteSliceEvaluationStatus::Success) << i;
    EXPECT_EQ(answer->vol_convention, expected) << i;
    EXPECT_EQ(answer->reason, QuoteSlicePriceOnlyReason::None) << i;
    EXPECT_EQ(bits(answer->price), bits(row.fitted_price)) << i;
    if (!moved) {
      EXPECT_EQ(bits(answer->volatility), bits(row.fitted_iv)) << i;
    }
  }
  EXPECT_GT(curve, 0u);
  // Between two nodes the certified price is re-implied: never the curve's own value.
  const auto between = query_quote_slice(fit, 102.0, Side::Call);
  ASSERT_TRUE(between) << between.error().to_string();
  EXPECT_EQ(between->status, QuoteSliceEvaluationStatus::Success);
  EXPECT_EQ(between->vol_convention, QuoteSliceVolConvention::ReimpliedRepaired);
  const auto value = evaluate_quote_slice(fit, 102.0, Side::Call);
  ASSERT_TRUE(value) << value.error().to_string();
  EXPECT_EQ(bits(value->volatility), bits(between->volatility));
  EXPECT_EQ(bits(value->price), bits(between->price));
}

TEST(QuoteSliceIdentification, AnInverterRefusalIsTypedUnidentifiedIntervalUnderBothPolicies) {
  // quote_surface_test's lifted-calendar-floor fixture: a calendar floor at the
  // certificate's upper edge lifts every node above any finite-volatility American
  // price, so the moved rows re-imply nothing. The historical PriceOnly row, now typed.
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.03, 0.01, 0.0};
  std::vector<QuoteSliceQuote> quotes;
  for (int strike = 80; strike <= 120; strike += 5) {
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price =
          andersen_lake(100.0, static_cast<double>(strike), 0.5, 0.25, 0.03, 0.01, side);
      ASSERT_TRUE(price) << price.error().to_string();
      const double spread = std::max(0.01, *price * 0.01);
      quotes.push_back({static_cast<double>(strike), side, std::max(*price - spread, 0.0),
                        *price + spread});
    }
  }
  for (const QuoteSliceFloorPolicy policy :
       {QuoteSliceFloorPolicy::Legacy, QuoteSliceFloorPolicy::PriceOnly}) {
    QuoteSliceConfig config;
    config.floor_policy = policy;
    auto fit = fit_quote_slice(market, quotes, config);
    ASSERT_TRUE(fit) << fit.error().to_string();
    ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
    EXPECT_EQ(fit->floor_policy, policy);
    std::vector<AmericanPriceNode> lifted;
    for (const auto &node : fit->prices->nodes()) {
      lifted.push_back(
          {node.strike, 100.0 - 1.0e-4 * node.strike, node.strike * (1.0 - 2.0e-4)});
    }
    const auto floor = AmericanPriceSlice::create(lifted, fit->prices->bounds());
    ASSERT_TRUE(floor) << floor.error().to_string();
    const std::array<const AmericanPriceSlice *, 1> earlier{&*floor};
    ASSERT_TRUE(repair_quote_slice_calendar(*fit, quotes, earlier, {}, 1.0e9));
    std::size_t typed = 0;
    for (std::size_t i = 0; i < quotes.size(); ++i) {
      const QuoteSliceRow &row = fit->rows[i];
      if (row.evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) continue;
      ++typed;
      EXPECT_EQ(row.price_only_reason, QuoteSlicePriceOnlyReason::UnidentifiedInterval) << i;
      EXPECT_TRUE(std::isnan(row.iv_lower) && std::isnan(row.iv_upper)) << i;
      const auto answer = query_quote_slice(*fit, quotes[i].strike, quotes[i].side);
      ASSERT_TRUE(answer) << answer.error().to_string();
      EXPECT_EQ(answer->status, QuoteSliceEvaluationStatus::PriceOnly) << i;
      EXPECT_EQ(answer->reason, QuoteSlicePriceOnlyReason::UnidentifiedInterval) << i;
      EXPECT_EQ(bits(answer->price), bits(row.fitted_price)) << i;
    }
    EXPECT_GT(typed, 0u) << to_string(policy);
  }
}

TEST(QuoteSliceIdentification, SnapshotProvenanceRidesOnEverySurfaceSlice) {
  const Chain c = wing_chain();
  QuoteSliceSnapshotInfo info;
  info.quote_ns = 1791988206009793000;
  info.underlying_ns = 1791988205869274000;
  info.spot_version = 1;
  info.carry_version = 1;
  info.carry_convention = QuoteSliceCarryConvention::ForwardPreserving;
  info.raw_spot = 99.72;
  info.spot_stale = true;
  const std::array<QuoteSurfaceExpiry, 1> expiry{{{c.market, c.quotes, {}, info}}};
  const auto fitted = fit_quote_surface(expiry, graded_v1(QuoteSliceFloorPolicy::Legacy),
                                        graded_v1_surface());
  ASSERT_TRUE(fitted) << fitted.error().to_string();
  const QuoteSliceSnapshotInfo &got = (*fitted)[0].fit.snapshot;
  EXPECT_EQ(got.quote_ns, info.quote_ns);
  EXPECT_EQ(got.underlying_ns, info.underlying_ns);
  EXPECT_EQ(got.spot_version, 1u);
  EXPECT_EQ(got.carry_version, 1u);
  EXPECT_EQ(got.carry_convention, QuoteSliceCarryConvention::ForwardPreserving);
  EXPECT_EQ(got.raw_spot, 99.72);
  EXPECT_TRUE(got.spot_stale);
  EXPECT_EQ(model_kind((*fitted)[0].fit), QuoteSliceModelKind::AndersenLake);
  // A standalone fit leaves the caller's provenance unset.
  const auto alone =
      fit_quote_slice(c.market, c.quotes, graded_v1(QuoteSliceFloorPolicy::Legacy));
  ASSERT_TRUE(alone) << alone.error().to_string();
  EXPECT_EQ(alone->snapshot.spot_version, 0u);
  EXPECT_EQ(alone->snapshot.carry_convention, QuoteSliceCarryConvention::Supplied);
  EXPECT_TRUE(std::isnan(alone->snapshot.raw_spot));
}

// Puts from `lo` to 148 every 4 at a 5 % rate, a constant yield and the skew
// sigma(K) = atm + skew x clamp((116 - K) / 40, 0, 1) (flat at `atm` from K 116 up),
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

TEST(QuoteSliceIdentification, ASurfaceCalendarRepairOntoTheExerciseValueIsFloorVolPriceOnly) {
  // N (0.45 y, 50 %, 4 % yield): puts 116-148 with time value. F (0.5 y, skewed down to
  // 5 % from K 116): puts 60-148, at their exercise value K - 100 from K 116 up. F serves
  // grade A, N grade C (a channel refit), so F outranks N. N crosses F and the bounded
  // calendar repair - run on a
  // repair trial of N's fit - caps N's nodes onto F's prices: N's own exercise value, whose
  // only volatility root is the inverter floor. Legacy serves that floor as an identified
  // volatility; under PriceOnly every repair path must type it FloorVol with the same price
  // (lane C0: the surface's repair trial dropped the fit's floor policy and re-implied
  // under Legacy - 190 dev200 rows served at 0.005 with serve.floor_price_only on).
  const std::array<Chain, 2> chains{put_chain(0.45, 0.5, 0.0, 116, 0.04),
                                    put_chain(0.5, 0.05, 0.30, 60, 0.0)};
  const std::array<QuoteSurfaceExpiry, 2> expiries{{{chains[0].market, chains[0].quotes, {}, {}},
                                                    {chains[1].market, chains[1].quotes, {}, {}}}};
  QuoteSurfaceConfig surface;
  surface.grade_b_min_mid_strikes = 10;
  surface.calendar_repair_half_spreads = 100.0;
  std::vector<double> legacy_prices;
  for (const QuoteSliceFloorPolicy policy :
       {QuoteSliceFloorPolicy::Legacy, QuoteSliceFloorPolicy::PriceOnly}) {
    QuoteSliceConfig config;
    config.side_policy = QuoteSliceSidePolicy::All; // N's own in-the-money evidence
    config.channel.repair_half_spreads = 100.0;
    config.floor_policy = policy;
    const auto fitted = fit_quote_surface(expiries, config, surface);
    ASSERT_TRUE(fitted) << fitted.error().to_string();
    const QuoteSurfaceSlice &near = (*fitted)[0];
    ASSERT_NE(near.grade, QuoteSliceGrade::Refused) << near.refusal << " " << near.error;
    ASSERT_TRUE(near.calendar_repaired) << to_string(policy);
    std::size_t floor_success = 0;
    std::size_t floor_price_only = 0;
    for (std::size_t i = 0; i < chains[0].quotes.size(); ++i) {
      const QuoteSliceRow &row = near.fit.rows[i];
      if (policy == QuoteSliceFloorPolicy::Legacy) legacy_prices.push_back(row.fitted_price);
      if (row.evaluation_status == QuoteSliceEvaluationStatus::Success &&
          row.fitted_iv <= kIvMin) {
        ++floor_success;
      }
      if (row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly &&
          row.price_only_reason == QuoteSlicePriceOnlyReason::FloorVol) {
        ++floor_price_only;
        EXPECT_EQ(bits(row.fitted_price), bits(legacy_prices[i])) << i;
      }
    }
    if (policy == QuoteSliceFloorPolicy::Legacy) {
      EXPECT_GT(floor_success, 0u); // the historical floor, served as identified
      EXPECT_EQ(floor_price_only, 0u);
    } else {
      EXPECT_EQ(floor_success, 0u);
      EXPECT_GT(floor_price_only, 0u);
    }
  }
}

TEST(QuoteSliceIdentification, AnUnknownFloorPolicyIsRefusedAndEveryNameIsStable) {
  const Chain c = wing_chain();
  QuoteSliceConfig config;
  config.floor_policy = static_cast<QuoteSliceFloorPolicy>(7);
  const auto refused = fit_quote_slice(c.market, c.quotes, config);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  EXPECT_STREQ(to_string(QuoteSlicePriceOnlyReason::None), "none");
  EXPECT_STREQ(to_string(QuoteSlicePriceOnlyReason::FloorVol), "floor_vol");
  EXPECT_STREQ(to_string(QuoteSlicePriceOnlyReason::FlatExerciseRegion), "flat_exercise_region");
  EXPECT_STREQ(to_string(QuoteSlicePriceOnlyReason::LowVega), "low_vega");
  EXPECT_STREQ(to_string(QuoteSlicePriceOnlyReason::UnidentifiedInterval),
               "unidentified_interval");
  EXPECT_STREQ(to_string(QuoteSliceVolConvention::None), "none");
  EXPECT_STREQ(to_string(QuoteSliceVolConvention::AmericanQuoting), "american_quoting");
  EXPECT_STREQ(to_string(QuoteSliceVolConvention::EuropeanEquivalent), "european_equivalent");
  EXPECT_STREQ(to_string(QuoteSliceVolConvention::FittedCurve), "fitted_curve");
  EXPECT_STREQ(to_string(QuoteSliceVolConvention::ReimpliedRepaired), "reimplied_repaired");
  EXPECT_STREQ(to_string(QuoteSliceFloorPolicy::Legacy), "legacy");
  EXPECT_STREQ(to_string(QuoteSliceFloorPolicy::PriceOnly), "price_only");
  EXPECT_STREQ(to_string(QuoteSliceCarryConvention::Supplied), "supplied");
  EXPECT_STREQ(to_string(QuoteSliceCarryConvention::ForwardPreserving), "forward_preserving");
  EXPECT_STREQ(to_string(QuoteSliceModelKind::AndersenLake), "andersen_lake");
  EXPECT_STREQ(to_string(QuoteSliceModelKind::DiscreteDividendLattice),
               "discrete_dividend_lattice");
  EXPECT_STREQ(to_string(static_cast<QuoteSlicePriceOnlyReason>(9)), "unknown");
}

} // namespace
} // namespace atx::vol
