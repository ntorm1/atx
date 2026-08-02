#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/detail/counters.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"

using namespace atx::vol;

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

constexpr std::int64_t kValuation = 1'700'000'000'000'000'000LL;
constexpr std::int64_t kExpiry = kValuation + 30 * static_cast<std::int64_t>(kListedNsPerDay);

[[nodiscard]] ListedOptionQuote option(const std::string &symbol, std::uint32_t id, Side side) {
  ListedOptionQuote q;
  q.trade_date = "2026-07-10";
  q.symbol = symbol;
  q.instrument_id = id;
  q.raw_symbol = symbol + std::to_string(id);
  q.expiry_ts_ns = kExpiry;
  q.strike = symbol == "SPY" ? 500.0 : (symbol == "N0" ? 100.0 : 200.0);
  q.side = side;
  q.bid = 2.0 + static_cast<double>(id) * 0.01;
  q.ask = q.bid + 0.2;
  q.quote_ts_ns = kValuation;
  q.multiplier = 100.0;
  q.standard_monthly = true;
  q.standard_deliverable = true;
  q.source_fingerprint = 1000u + id;
  return q;
}

[[nodiscard]] ListedStraddle straddle(std::string symbol, std::uint32_t uid, std::uint32_t id,
                                      double raw_weight, double normalized_weight) {
  ListedStraddle s;
  s.symbol = std::move(symbol);
  s.uid = uid;
  s.expiry_ts_ns = kExpiry;
  s.strike = s.symbol == "SPY" ? 500.0 : (s.symbol == "N0" ? 100.0 : 200.0);
  s.call = option(s.symbol, id, Side::Call);
  s.put = option(s.symbol, id + 1, Side::Put);
  s.raw_weight = raw_weight;
  s.normalized_weight = normalized_weight;
  return s;
}

[[nodiscard]] ListedDispersionSelection selection() {
  ListedDispersionSelection s;
  s.trade_date = "2026-07-10";
  s.valuation_ts_ns = kValuation;
  s.expiry_ts_ns = kExpiry;
  s.dte_days = 30.0;
  s.index = straddle("SPY", 1u, 1u, 0.0, 0.0);
  s.names.push_back(straddle("N0", 2u, 3u, 1.0, 1.0 / 3.0));
  s.names.push_back(straddle("N1", 3u, 5u, 2.0, 2.0 / 3.0));
  return s;
}

[[nodiscard]] ListedRiskLookup risks() {
  return [](std::uint32_t uid, const ListedOptionQuote &q) -> Result<ListedOptionRisk> {
    double vega = 0.0;
    if (uid == 1u)
      vega = 10.0;
    else if (uid == 2u)
      vega = 5.0;
    else if (uid == 3u)
      vega = q.side == Side::Call ? 8.0 : 12.0;
    else
      return Err(ErrorCode::NotFound, "missing risk");
    const double delta = q.side == Side::Call ? 0.55 : -0.45;
    return Ok(ListedOptionRisk{0.5 * (q.bid + q.ask), delta, vega});
  };
}

[[nodiscard]] ListedScheduleBuildConfig build_config() {
  ListedScheduleBuildConfig cfg;
  cfg.gross_index_vega_target_per_vol_point = 10000.0;
  cfg.cohort = 7u;
  cfg.surface_fingerprint = 987654321u;
  return cfg;
}

[[nodiscard]] Result<PricedSurface> surface(std::uint32_t uid, double spot, double sigma) {
  const double tenor = static_cast<double>(kExpiry - kValuation) / kNsPerYear;
  EssviParams params{};
  params.theta = sigma * sigma * tenor;
  params.phi = 1.1;
  params.rho = -0.25;
  params.psi = 0.5;
  params.p = 0.5;
  params.lambda = 0.5;
  params.T = tenor;
  params.F = spot;

  constexpr double rate = 0.04;
  CurveSurface curves;
  curves.push(std::make_unique<EssviCurve>(params, std::exp(-rate * tenor)));
  std::vector<SliceContext> context{{tenor, spot, 0.0, rate, 200u, 5u}};

  PricingContext pricing;
  pricing.S = spot;
  pricing.r = rate;
  pricing.now_ts_ns = kValuation;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  return PricedSurface::create(std::move(curves), std::move(context), pricing);
}

} // namespace

TEST(ListedDispersionSchedule, SurfaceAdapterUsesOnlyMarkDeltaVegaRoutes) {
  auto index = surface(1u, 500.0, 0.22);
  auto name0 = surface(2u, 100.0, 0.30);
  auto name1 = surface(3u, 200.0, 0.34);
  ASSERT_TRUE(index.has_value()) << index.error().to_string();
  ASSERT_TRUE(name0.has_value()) << name0.error().to_string();
  ASSERT_TRUE(name1.has_value()) << name1.error().to_string();
  std::vector<PricedSurface> surfaces;
  surfaces.push_back(std::move(*index));
  surfaces.push_back(std::move(*name0));
  surfaces.push_back(std::move(*name1));
  std::vector<const PricedSurface *> pointers;
  pointers.reserve(surfaces.size());
  for (const PricedSurface &item : surfaces) {
    pointers.push_back(&item);
  }
  auto set = SurfaceSet::create(pointers);
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  auto roll = build_listed_dispersion_roll(selection(), *set, build_config());
  ASSERT_TRUE(roll.has_value()) << roll.error().to_string();

  if constexpr (counters::counters_enabled()) {
    const counters::Snapshot measured = counters::snapshot();
    EXPECT_EQ(measured.get(counters::Counter::SurfaceScalarPriceRoutes), 6u);
    EXPECT_EQ(measured.get(counters::Counter::SurfaceDeltaRoutes), 6u);
    EXPECT_EQ(measured.get(counters::Counter::SurfaceVegaRoutes), 6u);
    EXPECT_EQ(measured.get(counters::Counter::SurfaceFullGreekRoutes), 0u);
    // Across three calls and three puts this fixture costs price(6) + dedicated
    // delta(9) + dedicated vega(12) solves. The retired six full FD bundles
    // cost 6 * 7 = 42 cold solves.
    EXPECT_EQ(measured.get(counters::Counter::BoundarySolves), 27u);
  } else {
    EXPECT_FALSE(counters::snapshot().enabled);
  }

  const double tenor = static_cast<double>(kExpiry - kValuation) / kNsPerYear;
  ASSERT_EQ(roll->legs.size(), 6u);
  for (const ListedScheduleLeg &leg : roll->legs) {
    const SurfaceRef priced = set->find(leg.uid);
    ASSERT_NE(priced, nullptr);
    const auto oracle = priced->greeks_analytic(leg.strike, tenor, leg.side);
    ASSERT_TRUE(oracle.has_value()) << oracle.error().to_string();
    EXPECT_NEAR(leg.model_mark, oracle->price, 1.0e-8);
    EXPECT_NEAR(leg.delta_per_share, oracle->delta, 1.0e-7);
    EXPECT_NEAR(leg.vega_per_unit_vol, oracle->vega,
                1.0e-7 * std::max(1.0, std::fabs(oracle->vega)));
  }
}

// ── E1 / AN-P1-1: ONE vega unit across both dispersion routes ───────────────
//
// `DispersionConfig::target_vega` (projected/surface route) and
// `ListedScheduleBuildConfig::gross_index_vega_target_per_vol_point` (listed
// route) are the same conceptual knob: "how much index-leg gross vega does this
// book carry". Handed the SAME number, against the SAME three PricedSurfaces,
// at the SAME tenor, side and contract multiplier, the two routes must size the
// index leg to the same DOLLAR VEGA PER VOL POINT.
//
// Before E1 the projected route divided by `straddle_vega * multiplier` where
// `straddle_vega` is dP/dsigma per UNIT vol, so its book carried target_vega
// dollars per 1.00 of vol == target_vega/100 dollars per vol point — exactly
// 100x smaller than the listed route's book for the same knob value.
//
// The two routes strike differently by construction (listed: the selected
// listed strike, 500; projected: the ATM forward, ~501.65), so the comparison
// is a 5% economic band, not an equality. A 100x unit error clears that band by
// three and a half orders of magnitude.
TEST(ListedDispersionSchedule, ProjectedAndListedRoutesAgreeOnVegaUnit) {
  auto index = surface(1u, 500.0, 0.22);
  auto name0 = surface(2u, 100.0, 0.30);
  auto name1 = surface(3u, 200.0, 0.34);
  ASSERT_TRUE(index.has_value()) << index.error().to_string();
  ASSERT_TRUE(name0.has_value()) << name0.error().to_string();
  ASSERT_TRUE(name1.has_value()) << name1.error().to_string();
  std::vector<PricedSurface> surfaces;
  surfaces.push_back(std::move(*index));
  surfaces.push_back(std::move(*name0));
  surfaces.push_back(std::move(*name1));
  std::vector<const PricedSurface *> pointers;
  pointers.reserve(surfaces.size());
  for (const PricedSurface &item : surfaces) {
    pointers.push_back(&item);
  }
  auto set = SurfaceSet::create(pointers);
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  constexpr double kTargetVega = 10000.0;   // dollars per vol point, both routes
  constexpr double kMultiplier = 100.0;     // same contract multiplier
  constexpr double kTenor = 30.0 / 365.25;  // same tenor: kExpiry - kValuation

  // ── listed route ──
  ListedScheduleBuildConfig listed_cfg = build_config();
  listed_cfg.gross_index_vega_target_per_vol_point = kTargetVega;
  auto roll = build_listed_dispersion_roll(selection(), *set, listed_cfg);
  ASSERT_TRUE(roll.has_value()) << roll.error().to_string();
  ASSERT_EQ(roll->legs.size(), 6u);
  ASSERT_TRUE(roll->legs[0].is_index);
  ASSERT_TRUE(roll->legs[1].is_index);
  const double listed_index_vega_per_vol_point =
      std::fabs(roll->legs[0].achieved_leg_vega_per_vol_point) +
      std::fabs(roll->legs[1].achieved_leg_vega_per_vol_point);

  // ── projected route, same knob value ──
  DispersionUniverse universe;
  universe.index = DispersionMember{"SPY", 1u, 0.0};
  universe.names.push_back(DispersionMember{"N0", 2u, 1.0});
  universe.names.push_back(DispersionMember{"N1", 3u, 2.0});

  DispersionConfig projected_cfg;
  projected_cfg.target_T = kTenor;
  projected_cfg.target_vega = kTargetVega;
  projected_cfg.side = DispersionSide::ShortIndexLongNames;
  projected_cfg.multiplier = kMultiplier;
  auto book = build_dispersion_book(universe, *set, projected_cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();

  // The book's index-leg dollar vega per vol point, expressed in the LISTED
  // route's own arithmetic (per-share vega -> per contract -> per vol point).
  const DispersionLeg &leg = book->index_leg;
  const double projected_index_vega_per_vol_point =
      std::fabs(leg.straddle_qty) * leg.straddle_vega * kMultiplier * 0.01;

  EXPECT_NEAR(projected_index_vega_per_vol_point, listed_index_vega_per_vol_point,
              0.05 * listed_index_vega_per_vol_point)
      << "projected route index vega/vol-pt = " << projected_index_vega_per_vol_point
      << ", listed route = " << listed_index_vega_per_vol_point << " (ratio "
      << (listed_index_vega_per_vol_point / projected_index_vega_per_vol_point) << "x)";

  // And the canonical unit is absolute, not merely mutually consistent: a
  // target_vega of 10000 means $10,000 of index vega per 1 vol point.
  EXPECT_NEAR(projected_index_vega_per_vol_point, kTargetVega, 0.05 * kTargetVega);
}

TEST(ListedDispersionSchedule, SizesShortIndexLongNamesVegaFlat) {
  auto roll = build_listed_dispersion_roll(selection(), risks(), build_config());
  ASSERT_TRUE(roll.has_value()) << roll.error().to_string();
  ASSERT_EQ(roll->legs.size(), 6u);
  EXPECT_EQ(roll->n_names, 2u);
  EXPECT_EQ(roll->cohort, 7u);
  EXPECT_LE(std::fabs(roll->net_vega_per_vol_point) / roll->gross_index_vega_target_per_vol_point,
            1.0e-10);
  EXPECT_NEAR(roll->gross_vega_per_vol_point, 20000.0, 1.0e-12);

  EXPECT_DOUBLE_EQ(roll->legs[0].quantity, -500.0);
  EXPECT_DOUBLE_EQ(roll->legs[1].quantity, -500.0);
  EXPECT_DOUBLE_EQ(roll->legs[0].vega_per_contract_per_vol_point, 10.0);
  EXPECT_GT(roll->legs[2].quantity, 0.0);
  EXPECT_GT(roll->legs[4].quantity, 0.0);

  const double n0_vega =
      roll->legs[2].achieved_leg_vega_per_vol_point + roll->legs[3].achieved_leg_vega_per_vol_point;
  const double n1_vega =
      roll->legs[4].achieved_leg_vega_per_vol_point + roll->legs[5].achieved_leg_vega_per_vol_point;
  EXPECT_NEAR(n0_vega, 10000.0 / 3.0, 1.0e-12);
  EXPECT_NEAR(n1_vega, 20000.0 / 3.0, 1.0e-12);
}

TEST(ListedDispersionSchedule, ReverseSideChangesQuantitySignsOnly) {
  ListedScheduleBuildConfig long_index = build_config();
  long_index.side = DispersionSide::LongIndexShortNames;
  auto a = build_listed_dispersion_roll(selection(), risks(), build_config());
  auto b = build_listed_dispersion_roll(selection(), risks(), long_index);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_EQ(a->legs.size(), b->legs.size());
  for (std::size_t i = 0; i < a->legs.size(); ++i) {
    EXPECT_DOUBLE_EQ(a->legs[i].quantity, -b->legs[i].quantity);
    EXPECT_DOUBLE_EQ(a->legs[i].model_mark, b->legs[i].model_mark);
    EXPECT_DOUBLE_EQ(a->legs[i].vega_per_unit_vol, b->legs[i].vega_per_unit_vol);
  }
}

TEST(ListedDispersionSchedule, RejectsBadRiskAndMultiplier) {
  const ListedRiskLookup bad = [](std::uint32_t,
                                  const ListedOptionQuote &) -> Result<ListedOptionRisk> {
    return Ok(ListedOptionRisk{1.0, 0.5, 0.0});
  };
  auto no_vega = build_listed_dispersion_roll(selection(), bad, build_config());
  ASSERT_FALSE(no_vega.has_value());
  EXPECT_EQ(no_vega.error().code(), ErrorCode::Unavailable);

  ListedDispersionSelection mismatched = selection();
  mismatched.names[0].put.multiplier = 10.0;
  auto bad_multiplier = build_listed_dispersion_roll(mismatched, risks(), build_config());
  ASSERT_FALSE(bad_multiplier.has_value());
  EXPECT_EQ(bad_multiplier.error().code(), ErrorCode::InvalidArgument);
}

TEST(ListedDispersionSchedule, DeterministicTsvRoundTripsExactly) {
  auto roll = build_listed_dispersion_roll(selection(), risks(), build_config());
  ASSERT_TRUE(roll.has_value()) << roll.error().to_string();
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(*roll);

  auto text = serialize_listed_dispersion_schedule(schedule);
  ASSERT_TRUE(text.has_value()) << text.error().to_string();
  auto parsed = parse_listed_dispersion_schedule(*text);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().to_string();
  EXPECT_EQ(*parsed, schedule);
  auto text2 = serialize_listed_dispersion_schedule(*parsed);
  ASSERT_TRUE(text2.has_value()) << text2.error().to_string();
  EXPECT_EQ(*text2, *text);
}

TEST(ListedDispersionSchedule, ParserRejectsCorruptTotalsAndDuplicateKeys) {
  auto roll = build_listed_dispersion_roll(selection(), risks(), build_config());
  ASSERT_TRUE(roll.has_value()) << roll.error().to_string();
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(*roll);
  auto text = serialize_listed_dispersion_schedule(schedule);
  ASSERT_TRUE(text.has_value()) << text.error().to_string();

  std::string corrupt = *text;
  const std::string needle = "\t20000\t2\t";
  const std::size_t pos = corrupt.find(needle);
  ASSERT_NE(pos, std::string::npos);
  corrupt.replace(pos, needle.size(), "\t19999\t2\t");
  auto bad_total = parse_listed_dispersion_schedule(corrupt);
  ASSERT_FALSE(bad_total.has_value());
  EXPECT_EQ(bad_total.error().code(), ErrorCode::ParseError);

  ListedDispersionSchedule duplicate = schedule;
  duplicate.rolls[0].legs[1].instrument_id = duplicate.rolls[0].legs[0].instrument_id;
  duplicate.rolls[0].legs[1].raw_symbol = duplicate.rolls[0].legs[0].raw_symbol;
  auto duplicate_text = serialize_listed_dispersion_schedule(duplicate);
  ASSERT_FALSE(duplicate_text.has_value());
  EXPECT_EQ(duplicate_text.error().code(), ErrorCode::ParseError);
}

TEST(ListedDispersionSchedule, FileRoundTripUsesValidatedTsv) {
  auto roll = build_listed_dispersion_roll(selection(), risks(), build_config());
  ASSERT_TRUE(roll.has_value()) << roll.error().to_string();
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(*roll);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "atx-listed-dispersion-schedule.tsv";
  ASSERT_TRUE(write_listed_dispersion_schedule_file(path.string(), schedule).has_value());
  auto loaded = read_listed_dispersion_schedule_file(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(*loaded, schedule);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}
