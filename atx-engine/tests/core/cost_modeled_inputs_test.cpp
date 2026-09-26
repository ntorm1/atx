#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "atx/engine/cost/borrow_tiers.hpp"
#include "atx/engine/cost/fim_adjustment.hpp"
#include "atx/engine/cost/modeled_inputs.hpp"
#include "atx/engine/cost/optimizer_cost_terms.hpp"
#include "atx/engine/cost/spread_estimators.hpp"
#include "atx/engine/risk/cost_terms.hpp"

namespace atx_test_w1_b1_modeled_inputs {
namespace cost = atx::engine::cost;
constexpr atx::i64 clock = 1000;
constexpr auto available = cost::SpreadState::Available;
constexpr atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();

std::array<cost::SpreadOhlc, 8> history() {
  return {{{100, 500, available, 100, 101, 99, 100.7},
           {101, 501, available, 100.5, 101.2, 99.8, 100.1},
           {102, 502, available, 99, 100, 98.4, 99.5},
           {103, 503, available, 101.3, 102, 100.7, 101.8},
           {104, 504, available, 100.8, 102.2, 100.2, 101.1},
           {105, 505, available, 102.1, 103.5, 101.2, 103},
           {106, 506, available, 101.7, 102.4, 100.9, 101.3},
           {107, 507, available, 103, 104, 102.6, 103.8}}};
}
cost::CostSurfaceIdentity identity() {
  return {clock, std::string(64U, 'a'), "synthetic/raw20/vol60", "modeled-prior"};
}
cost::BorrowPredictors ordinary_borrow() { return {true, 800, 2e9, 20.0, 0.02, 1000.0}; }
cost::ModeledCostInput input(std::span<const cost::SpreadOhlc> h) {
  cost::ModeledCostInput p;
  p.instrument_id = 7U; p.liquidity_state = cost::CostInputState::Available;
  p.liquidity_available_at_ns = 900; p.adv_dollars = 1e7; p.daily_vol = 0.02;
  p.history = h; p.borrow = ordinary_borrow();
  return p;
}

TEST(CostSpread, FixedSyntheticReferenceValuesAndEqualWeightScale) {
  // Receipt .superpowers/sdd/w1/b1-spread-reference.json: upstream bidask 1caba55d,
  // exact Python EDGE execution; independently transcribed R CS/AR equations.
  // These constants do not imply R package runtime parity or empirical calibration.
  auto h = history();
  const std::array<std::array<atx::f64, 3>, 3> expected{{
      {0.0008834482771881905, 0.007979074775115292, 0.006190521947239885},
      {0.0008834482771885214, 0.03100781196167705, 0.0038692712601222133},
      {0.020000000000000344, 0.00010000500033413573, 0.0}}};
  for (atx::usize case_id = 0U; case_id < expected.size(); ++case_id) {
    if (case_id == 1U) {
      for (atx::usize i = 3U; i < h.size(); ++i) {
        h[i].open *= 1.8; h[i].high *= 1.8; h[i].low *= 1.8; h[i].close *= 1.8;
      }
    } else if (case_id == 2U) {
      for (auto& row : h) { row.open = 100; row.high = 101; row.low = 99; row.close = 100; }
    }
    auto estimate = cost::estimate_spread(h, clock); ASSERT_TRUE(estimate);
    ASSERT_EQ(estimate->state, available);
    EXPECT_NEAR(estimate->corwin_schultz, expected[case_id][0], 1e-10);
    EXPECT_NEAR(estimate->abdi_ranaldo, expected[case_id][1], 1e-10);
    EXPECT_NEAR(estimate->edge, expected[case_id][2], 1e-10);
    EXPECT_NEAR(estimate->full_spread,
        (expected[case_id][0] + expected[case_id][1] + expected[case_id][2]) / 3.0, 1e-10);
    auto recipe = cost::SpreadRecipe{}; recipe.scale = 0.6;
    auto scaled = cost::estimate_spread(h, clock, recipe); ASSERT_TRUE(scaled);
    EXPECT_DOUBLE_EQ(scaled->edge, estimate->edge);
    EXPECT_DOUBLE_EQ(scaled->full_spread, 0.6 * estimate->full_spread);
  }
}

TEST(CostSpread, MissingGapAndFutureCloseNeverBecomePartialOrZeroComposite) {
  for (const auto kind : {0, 1, 2, 3}) {
    auto h = history();
    if (kind == 0) h[3].state = cost::SpreadState::Unavailable;
    if (kind == 1) for (atx::usize i = 3U; i < h.size(); ++i) ++h[i].session;
    if (kind == 2) h[3].available_at_ns = clock;
    if (kind == 3) for (auto& row : h) row.open = row.high = row.low = row.close = 100.0;
    auto estimate = cost::estimate_spread(h, clock); ASSERT_TRUE(estimate);
    EXPECT_EQ(estimate->state, cost::SpreadState::Unavailable);
    EXPECT_TRUE(std::isnan(estimate->full_spread));
  }
  const auto h = history();
  auto short_window = cost::estimate_spread(std::span{h}.first(2U), clock);
  ASSERT_TRUE(short_window); EXPECT_EQ(short_window->state, cost::SpreadState::Unavailable);
}

TEST(CostSpread, RejectsMalformedRawGeometryAndRespectsBoundedWindow) {
  auto h = history(); h[2].open = h[2].high + 1.0;
  EXPECT_FALSE(cost::estimate_spread(h, clock));
  h = history(); h[2].low = 0.0; EXPECT_FALSE(cost::estimate_spread(h, clock));
  h = history(); h[2].session = h[1].session; EXPECT_FALSE(cost::estimate_spread(h, clock));
  auto recipe = cost::SpreadRecipe{}; recipe.max_observations = 4U;
  EXPECT_FALSE(cost::estimate_spread(history(), clock, recipe));
  recipe = {}; recipe.scale = nan; EXPECT_FALSE(cost::estimate_spread(history(), clock, recipe));
  const auto original = cost::estimate_spread(history(), clock); ASSERT_TRUE(original);
  h = history();
  for (auto& row : h) {
    row.open *= 100.0; row.high *= 100.0; row.low *= 100.0; row.close *= 100.0;
  }
  const auto scaled = cost::estimate_spread(h, clock); ASSERT_TRUE(scaled);
  EXPECT_NEAR(scaled->full_spread, original->full_spread, 1e-12);
}

TEST(CostBorrowTiers, PublicPredictorPriorsAndCheckedAnnualUnits) {
  auto p = ordinary_borrow();
  auto gc = cost::estimate_borrow_tier(p, clock); ASSERT_TRUE(gc);
  EXPECT_EQ(gc->tier, cost::BorrowTier::GeneralCollateral);
  EXPECT_DOUBLE_EQ(gc->annual_fraction, 0.00275);
  p.raw_price_usd = 4.99;
  auto warm = cost::estimate_borrow_tier(p, clock); ASSERT_TRUE(warm);
  EXPECT_EQ(warm->tier, cost::BorrowTier::Warm); EXPECT_EQ(warm->risk_flags, 1U);
  p.short_interest_to_float = 0.10;
  auto special = cost::estimate_borrow_tier(p, clock); ASSERT_TRUE(special);
  EXPECT_EQ(special->tier, cost::BorrowTier::Special); EXPECT_EQ(special->risk_flags, 2U);
  auto bps = cost::annual_fraction_to_bps(special->annual_fraction); ASSERT_TRUE(bps);
  EXPECT_DOUBLE_EQ(*bps, 2750.0);
  auto fraction = cost::annual_bps_to_fraction(*bps); ASSERT_TRUE(fraction);
  EXPECT_DOUBLE_EQ(*fraction, special->annual_fraction);
  auto model = cost::as_borrow_model(*special, cost::DayCount::D365); ASSERT_TRUE(model);
  EXPECT_DOUBLE_EQ(model->annual_rate, special->annual_fraction);
  EXPECT_EQ(model->day_count, cost::DayCount::D365);
  EXPECT_FALSE(cost::annual_fraction_to_bps(std::numeric_limits<atx::f64>::max()));
  EXPECT_FALSE(cost::annual_bps_to_fraction(-1.0));
  EXPECT_FALSE(cost::annual_bps_to_fraction(std::numeric_limits<atx::f64>::denorm_min()));
}

TEST(CostBorrowTiers, MissingAndNotYetPublishedPredictorsRemainUnavailable) {
  auto p = ordinary_borrow(); p.available_at_ns = clock;
  auto equal = cost::estimate_borrow_tier(p, clock); ASSERT_TRUE(equal);
  EXPECT_EQ(equal->tier, cost::BorrowTier::Unavailable);
  EXPECT_FALSE(cost::as_borrow_model(*equal));
  p = {}; auto missing = cost::estimate_borrow_tier(p, clock); ASSERT_TRUE(missing);
  EXPECT_EQ(missing->tier, cost::BorrowTier::Unavailable);
  p = ordinary_borrow(); p.short_interest_to_float = nan;
  EXPECT_FALSE(cost::estimate_borrow_tier(p, clock));
  auto r = cost::BorrowTierRecipe{}; r.warm_annual_fraction = r.gc_annual_fraction / 2.0;
  EXPECT_FALSE(cost::estimate_borrow_tier({}, clock, r));
}

TEST(CostFim, ExplicitTableUnitsDifferentialAndAnchorPreserveReferencePrior) {
  const cost::FimRecipe r{cost::FimRule::Table7Global5AnchoredV2, 1e9, 0.20, 0.02};
  cost::FimPredictors p{true, 700, 1e9, 0.20};
  auto same = cost::fim_adjustment(r, p, clock, 0.6, 0.02); ASSERT_TRUE(same);
  EXPECT_DOUBLE_EQ(same->impact_multiplier, 1.0); EXPECT_DOUBLE_EQ(same->differential_bps, 0.0);
  p.market_cap_usd = 0.5e9; p.idio_annual_fraction = 0.30;
  auto adjusted = cost::fim_adjustment(r, p, clock, 0.6, 0.02); ASSERT_TRUE(adjusted);
  const auto independent = -0.62 * (std::log(1.5) - std::log(2.0)) + 0.28 * 10.0;
  EXPECT_NEAR(adjusted->differential_bps, independent, 1e-13);
  const auto prior_bps = 0.6 * 0.02 * std::sqrt(0.02) * 1e4;
  EXPECT_NEAR(prior_bps * adjusted->impact_multiplier, prior_bps + independent, 1e-12);
  auto table = cost::fim_reference_impact_bps(r.rule, 0.02, 1e9, 0.20, 0.0);
  ASSERT_TRUE(table);
  EXPECT_NEAR(*table, -0.62 * std::log(2.0) + 0.28 * 20.0 - 0.13 * 2.0 +
      8.89 * std::sqrt(2.0), 1e-12);
  auto us = cost::fim_reference_impact_bps(cost::FimRule::Table7Us10AnchoredV2,
      0.10, 1e9, 0.20, 2.5); ASSERT_TRUE(us);
  EXPECT_NEAR(*us, 2.5 - 0.14 * std::log(2.0) + 0.31 * 20.0 - 0.53 * 10.0 +
      11.21 * std::sqrt(10.0), 1e-12);
}

TEST(CostFim, ExplicitMissingBoundsAndZeroFloorAreNotEmpiricalCalibration) {
  cost::FimRecipe r{cost::FimRule::Table7Global5AnchoredV2, 1e9, 0.20, 0.02};
  cost::FimPredictors p{true, clock, 1e9, 0.20};
  auto future = cost::fim_adjustment(r, p, clock, 0.6, 0.02); ASSERT_TRUE(future);
  EXPECT_FALSE(future->available);
  p = {true, 800, 1e15, 0.0};
  auto floor = cost::fim_adjustment(r, p, clock, 0.6, 0.0001); ASSERT_TRUE(floor);
  EXPECT_TRUE(floor->floored_at_zero); EXPECT_DOUBLE_EQ(floor->impact_multiplier, 0.0);
  auto zero = cost::fim_adjustment(r, p, clock, 0.0, 0.02); ASSERT_TRUE(zero);
  EXPECT_FALSE(zero->available);
  r.participation_anchor = nan; EXPECT_FALSE(cost::fim_adjustment(r, p, clock, 0.6, 0.02));
  auto disabled = cost::fim_adjustment({}, {}, clock, 0.6, 0.02); ASSERT_TRUE(disabled);
  EXPECT_TRUE(disabled->available); EXPECT_DOUBLE_EQ(disabled->impact_multiplier, 1.0);
}

TEST(CostModeledInputs, OneWayV2MatchesFitnessOptimizerReplayAndKeepsBorrowSeparate) {
  const auto h = history(); auto p = input(h);
  p.fim = {true, 750, 0.5e9, 0.30};
  cost::ModeledCostRecipe recipe;
  recipe.fim = {cost::FimRule::Table7Global5AnchoredV2, 1e9, 0.20, 0.02};
  const std::array rows{p};
  auto surface = cost::make_modeled_cost_surface(recipe, identity(), rows); ASSERT_TRUE(surface);
  constexpr atx::f64 nav = 5e7, dollars = -2e5;
  auto quote = surface->quote_dollars(0U, clock, dollars); ASSERT_TRUE(quote.priced());
  const auto adjusted = cost::fim_adjustment(recipe.fim, p.fim, clock, 0.6, 0.02);
  ASSERT_TRUE(adjusted);
  const auto expected = std::abs(dollars) *
      (0.005017681666514456 / 2.0 + 1e-4 +
       0.6 * 0.02 * adjusted->impact_multiplier * std::sqrt(std::abs(dollars) / p.adv_dollars));
  EXPECT_NEAR(quote.total_dollars / nav, expected / nav, 1e-12);
  const std::array snapshots{*surface};
  const std::array<cost::CostPathTrade, 1> path{{{0U, 0U, clock, nav, dollars}}};
  auto fitness = cost::price_trade_path(snapshots, path); ASSERT_TRUE(fitness);
  EXPECT_NEAR(fitness->cost_return, expected / nav, 1e-12);
  auto terms = cost::cost_terms_from_surface(*surface, clock, nav); ASSERT_TRUE(terms);
  const std::array weights{dollars / nav};
  auto optimized = atx::engine::risk::evaluate_trade_costs(terms->view(), weights);
  ASSERT_TRUE(optimized); EXPECT_NEAR(optimized->total(), expected / nav, 1e-12);
  auto replay = atx::engine::book::SurfaceReplayCost::create(*surface, 4U, clock);
  ASSERT_TRUE(replay); EXPECT_NEAR(replay->cost(0U, 4U, dollars, {}).cost_dollars / nav,
                                  expected / nav, 1e-12);
  auto rate = surface->borrow_annual_rate(0U, clock);
  EXPECT_EQ(rate.status, cost::CostQuoteStatus::Priced);
  EXPECT_DOUBLE_EQ(rate.annual_fraction, .00275);
  EXPECT_TRUE(terms->borrow_fee.empty()); // borrowing still needs explicit elapsed-time policy
}

TEST(CostModeledInputs, IndependentMissingInputsAndHashBoundKnobs) {
  auto h = history(); auto p = input(h); std::array rows{p};
  cost::ModeledCostRecipe r;
  auto original = cost::make_modeled_cost_surface(r, identity(), rows); ASSERT_TRUE(original);
  rows[0].borrow.available = false;
  auto missing_borrow = cost::make_modeled_cost_surface(r, identity(), rows);
  ASSERT_TRUE(missing_borrow);
  EXPECT_TRUE(missing_borrow->quote_dollars(0U, clock, 100.0).priced());
  EXPECT_EQ(missing_borrow->borrow_annual_rate(0U, clock).status,
            cost::CostQuoteStatus::Unavailable);
  EXPECT_NE(missing_borrow->snapshot_sha256(), original->snapshot_sha256());
  rows[0] = p; h[3].state = cost::SpreadState::Unavailable;
  auto missing_trade = cost::make_modeled_cost_surface(r, identity(), rows);
  ASSERT_TRUE(missing_trade);
  EXPECT_EQ(missing_trade->quote_dollars(0U, clock, 100.0).status,
            cost::CostQuoteStatus::Unavailable);
  EXPECT_TRUE(missing_trade->quote_dollars(0U, clock, 0.0).priced());
  EXPECT_EQ(missing_trade->borrow_annual_rate(0U, clock).status, cost::CostQuoteStatus::Priced);
  h = history();
  for (const auto knob : {0, 1, 2, 3}) {
    r = {};
    if (knob == 0) r.spread.scale = 0.5;
    if (knob == 1) r.borrow.small_cap_usd = 5e8;
    if (knob == 2) r.borrow.gc_annual_fraction = 0.003;
    if (knob == 3) r.fim = {cost::FimRule::Table7Global5AnchoredV2, 1e9, 0.20, 0.02};
    auto changed = cost::make_modeled_cost_surface(r, identity(), rows); ASSERT_TRUE(changed);
    EXPECT_NE(changed->recipe_sha256(), original->recipe_sha256());
  }
  r = {}; r.fim.reference_market_cap_usd = 3e9; // unused disabled knob
  auto ignored = cost::make_modeled_cost_surface(r, identity(), rows); ASSERT_TRUE(ignored);
  EXPECT_EQ(ignored->recipe_sha256(), original->recipe_sha256());
  EXPECT_FALSE(cost::make_modeled_cost_surface({}, identity(), rows, 64U));
  r = {}; r.surface.spread_scale = 0.5;
  EXPECT_FALSE(cost::make_modeled_cost_surface(r, identity(), rows));
}

TEST(CostModeledInputs, LegacyV1IgnoresNewFieldsAndRetainsFrozenHashCodec) {
  std::array<cost::CostSurfaceRow, 1> rows{{
      {7U, cost::CostInputState::Available, 900, 1e7, 0.02, 0.002}}};
  auto legacy = cost::CostSurface::create({}, identity(), rows); ASSERT_TRUE(legacy);
  rows[0].impact_multiplier = nan; rows[0].borrow_state = cost::CostInputState::Available;
  rows[0].borrow_available_at_ns = clock + 100; rows[0].borrow_annual_fraction = nan;
  auto ignored = cost::CostSurface::create({}, identity(), rows); ASSERT_TRUE(ignored);
  EXPECT_EQ(legacy->recipe_sha256(), ignored->recipe_sha256());
  EXPECT_EQ(legacy->snapshot_sha256(), ignored->snapshot_sha256());
  EXPECT_EQ(std::bit_cast<atx::u64>(legacy->quote_dollars(0U, clock, -1000).total_dollars),
            std::bit_cast<atx::u64>(ignored->quote_dollars(0U, clock, -1000).total_dollars));
  EXPECT_EQ(ignored->borrow_annual_rate(0U, clock).status, cost::CostQuoteStatus::Unavailable);
  // Frozen V1 little-endian codec reproduced independently in the source report.
  EXPECT_EQ(legacy->recipe_sha256(),
            "3879022a980db24a1fcc54c9b2ea198fe68eb9b1c7559a9fecacea62a579ab49");
  EXPECT_EQ(legacy->snapshot_sha256(),
            "15208617c87033f4884283d5ec7b91a40629c0a4a590dc44b670743a1143124d");
}

TEST(CostModeledInputs, V2DirectContractValidatesIndependentBorrowClockAndImpact) {
  cost::CostSurfaceRecipe recipe; recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
  std::array<cost::CostSurfaceRow, 1> rows{{
      {7U, cost::CostInputState::Available, 900, 1e7, .02, .002}}};
  rows[0].borrow_state = cost::CostInputState::Available;
  rows[0].borrow_annual_fraction = .03; rows[0].borrow_available_at_ns = clock;
  EXPECT_FALSE(cost::CostSurface::create(recipe, identity(), rows));
  rows[0].borrow_available_at_ns = clock - 1;
  auto valid = cost::CostSurface::create(recipe, identity(), rows); ASSERT_TRUE(valid);
  EXPECT_EQ(valid->borrow_annual_rate(1U, clock).status, cost::CostQuoteStatus::OutOfRange);
  EXPECT_EQ(valid->borrow_annual_rate(0U, clock + 1).status, cost::CostQuoteStatus::WrongDecision);
  rows[0].impact_multiplier = -1.0;
  EXPECT_FALSE(cost::CostSurface::create(recipe, identity(), rows));
  rows[0].impact_multiplier = nan;
  EXPECT_FALSE(cost::CostSurface::create(recipe, identity(), rows));
}
} // namespace atx_test_w1_b1_modeled_inputs
