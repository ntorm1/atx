#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include "atx/engine/cost/optimizer_cost_terms.hpp"
#include "atx/engine/risk/cost_terms.hpp"

namespace atx_test_w1_b1_cost_surface {
namespace cost = atx::engine::cost;
namespace book = atx::engine::book;
using atx::f64;
using atx::usize;
constexpr atx::i64 clock = 1000;
constexpr f64 nav = 50'000'000.0;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();

cost::CostSurfaceIdentity identity(atx::i64 time = clock) {
  return {time, std::string(64U, 'a'), "synthetic/raw-dollar-ADV20/TR-vol60/full-spread-v1",
          "modeled-square-root-prior/y=0.6/not-observed-fills"};
}
std::array<cost::CostSurfaceRow, 2> rows() {
  return {{{101U, cost::CostInputState::Available, clock - 1, 2'000'000.0, 0.02, 0.002},
           {202U, cost::CostInputState::Available, clock - 2, 8'000'000.0, 0.03, 0.0008}}};
}
void same_bits(f64 a, f64 b) { EXPECT_EQ(std::bit_cast<atx::u64>(a), std::bit_cast<atx::u64>(b)); }

TEST(CostSurface, SameActualTradesMatchIndependentFormulaFitnessOptimizerAndReplay) {
  const auto inputs = rows();
  auto surface = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(surface);
  auto replay = book::SurfaceReplayCost::create(*surface, 7U, clock + 10); ASSERT_TRUE(replay);
  const std::array<f64, 2> dollars{-500'000.0, 250'000.0};
  const std::array<f64, 2> previous{0.003, -0.02};
  std::array<f64, 2> next{};
  std::array<cost::CostPathTrade, 2> path{};
  f64 independent_return = 0.0;
  for (usize i = 0U; i < dollars.size(); ++i) {
    next[i] = previous[i] + dollars[i] / nav;
    path[i] = {0U, i, clock, nav, dollars[i]};
    const auto q = surface->quote_dollars(i, clock, dollars[i]); ASSERT_TRUE(q.priced());
    const auto quantity = std::abs(dollars[i]);
    const auto expected_spread = quantity * inputs[i].full_spread / 2.0;
    const auto expected_commission = quantity * 1e-4;
    const auto expected_impact = quantity * 0.6 * inputs[i].daily_vol *
                                 std::sqrt(quantity / inputs[i].adv_dollars);
    EXPECT_NEAR(q.spread_dollars / nav, expected_spread / nav, 1e-12);
    EXPECT_NEAR(q.commission_dollars / nav, expected_commission / nav, 1e-12);
    EXPECT_NEAR(q.impact_dollars / nav, expected_impact / nav, 1e-12);
    independent_return += (expected_spread + expected_commission + expected_impact) / nav;
    const auto execution = replay->cost(i, 7U, dollars[i], {nan, nan, nan});
    EXPECT_DOUBLE_EQ(execution.filled_dollars, dollars[i]);
    same_bits(execution.cost_dollars, q.total_dollars);
    same_bits(replay->unrationed_cost(i, 7U, dollars[i], {}), q.total_dollars);
    const auto by_weight = surface->quote_weight_change(i, clock, dollars[i] / nav, nav);
    ASSERT_TRUE(by_weight.dollars.priced());
    EXPECT_NEAR(by_weight.cost_return, q.total_dollars / nav, 1e-12);
  }
  const std::array<cost::CostSurface, 1> snapshots{*surface};
  auto fitness = cost::price_trade_path(snapshots, path); ASSERT_TRUE(fitness);
  EXPECT_NEAR(fitness->cost_return, independent_return, 1e-12);
  EXPECT_DOUBLE_EQ(fitness->one_way_turnover, 750'000.0 / nav);
  auto terms = cost::cost_terms_from_surface(*surface, clock, nav, previous); ASSERT_TRUE(terms);
  auto optimized = atx::engine::risk::evaluate_trade_costs(terms->view(), next); ASSERT_TRUE(optimized);
  EXPECT_NEAR(optimized->total(), independent_return, 1e-12);
  EXPECT_TRUE(terms->borrow_fee.empty()); EXPECT_TRUE(terms->locate_cap.empty());
}

TEST(CostSurface, CappedFillPricesItsOwnQuantityAndFullRequestRemainsExplicit) {
  auto recipe = cost::CostSurfaceRecipe{}; recipe.max_participation = 0.02;
  auto surface = cost::CostSurface::create(recipe, identity(), rows()); ASSERT_TRUE(surface);
  auto replay = book::SurfaceReplayCost::create(*surface, 4U, clock); ASSERT_TRUE(replay);
  constexpr f64 requested = -1'000'000.0, filled = -40'000.0;
  const auto capped = surface->quote_dollars(0U, clock, requested, cost::CostFillRule::ParticipationCapped);
  const auto whole = surface->quote_dollars(0U, clock, requested);
  const auto filled_direct = surface->quote_dollars(0U, clock, filled);
  ASSERT_TRUE(capped.priced()); ASSERT_TRUE(whole.priced()); ASSERT_TRUE(filled_direct.priced());
  EXPECT_DOUBLE_EQ(capped.filled_dollars, filled);
  same_bits(capped.total_dollars, filled_direct.total_dollars);
  const auto executed = replay->cost(0U, 4U, requested, {});
  same_bits(executed.filled_dollars, filled); same_bits(executed.cost_dollars, capped.total_dollars);
  same_bits(replay->unrationed_cost(0U, 4U, requested, {}), whole.total_dollars);
  EXPECT_GT(whole.total_dollars / std::abs(requested), capped.total_dollars / std::abs(filled));
  auto terms = cost::cost_terms_from_surface(*surface, clock, nav); ASSERT_TRUE(terms);
  EXPECT_DOUBLE_EQ(terms->max_trade[0], std::abs(filled) / nav);
  const std::array<f64, 2> weights{filled / nav, 0.0};
  auto priced = atx::engine::risk::evaluate_trade_costs(terms->view(), weights); ASSERT_TRUE(priced);
  EXPECT_NEAR(priced->total(), capped.total_dollars / nav, 1e-12);
}

TEST(CostSurface, MissingRowsNeverBecomeFreeExecutableTrades) {
  auto inputs = rows(); inputs[1].state = cost::CostInputState::Unavailable;
  inputs[1].adv_dollars = nan;
  auto surface = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(surface);
  EXPECT_EQ(surface->quote_dollars(1U, clock, 100.0).status, cost::CostQuoteStatus::Unavailable);
  const auto zero = surface->quote_dollars(1U, clock, 0.0);
  ASSERT_TRUE(zero.priced()); EXPECT_EQ(zero.total_dollars, 0.0);
  auto terms = cost::cost_terms_from_surface(*surface, clock, nav); ASSERT_TRUE(terms);
  EXPECT_EQ(terms->untradeable[1], 1U); EXPECT_EQ(terms->max_trade[1], 0.0);
  auto replay = book::SurfaceReplayCost::create(*surface, 0U, clock); ASSERT_TRUE(replay);
  EXPECT_EQ(replay->cost(1U, 0U, 100.0, {}).filled_dollars, 0.0);
  EXPECT_TRUE(std::isnan(replay->unrationed_cost(1U, 0U, 100.0, {})));
  const std::array<cost::CostSurface, 1> snapshots{*surface};
  const std::array<cost::CostPathTrade, 1> path{{{0U, 1U, clock, nav, 100.0}}};
  EXPECT_FALSE(cost::price_trade_path(snapshots, path));
}

TEST(CostSurface, RejectsInvalidGeometryUnitsAndUnavailableClocksBeforeUse) {
  const auto source = rows();
  auto inputs = source; inputs[1].instrument_id = inputs[0].instrument_id;
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), inputs));
  inputs = source; inputs[0].available_at_ns = clock;
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), inputs));
  inputs[0].available_at_ns = clock + 1;
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), inputs));
  for (const f64 invalid : {0.0, -1.0, nan, std::numeric_limits<f64>::infinity()}) {
    inputs = source; inputs[0].adv_dollars = invalid;
    EXPECT_FALSE(cost::CostSurface::create({}, identity(), inputs));
  }
  inputs = source; inputs[0].full_spread = nan;
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), inputs));
  inputs = source; inputs[0].state = static_cast<cost::CostInputState>(255);
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), inputs));
  auto id = identity(); id.source_sha256[0] = 'z';
  EXPECT_FALSE(cost::CostSurface::create({}, id, source));
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), {}));
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), source, 1U));
  auto recipe = cost::CostSurfaceRecipe{}; recipe.impact_y = nan;
  EXPECT_FALSE(cost::CostSurface::create(recipe, identity(), source));
  auto surface = cost::CostSurface::create({}, identity(), source); ASSERT_TRUE(surface);
  EXPECT_EQ(surface->quote_dollars(2U, clock, 0.0).status, cost::CostQuoteStatus::OutOfRange);
  EXPECT_EQ(surface->quote_dollars(0U, clock + 1, 10.0).status, cost::CostQuoteStatus::WrongDecision);
  EXPECT_EQ(surface->quote_dollars(0U, clock, nan).status, cost::CostQuoteStatus::InvalidRequest);
  EXPECT_FALSE(surface->quote_weight_change(0U, clock, 0.1, 0.0).dollars.priced());
  EXPECT_FALSE(cost::cost_terms_from_surface(*surface, clock + 1, nav));
  const std::array<f64, 1> short_book{0.0};
  EXPECT_FALSE(cost::cost_terms_from_surface(*surface, clock, nav, short_book));
  EXPECT_FALSE(book::SurfaceReplayCost::create(*surface, 0U, clock - 1));
  auto replay = book::SurfaceReplayCost::create(*surface, 0U, clock); ASSERT_TRUE(replay);
  EXPECT_EQ(replay->cost(0U, 1U, 100.0, {}).filled_dollars, 0.0);
  EXPECT_TRUE(std::isnan(replay->unrationed_cost(0U, 1U, 100.0, {})));
}

TEST(CostSurface, CanonicalHashesBindRecipesOrderSupportAndSourceWithoutBorrowing) {
  auto inputs = rows(); auto id = identity();
  auto surface = cost::CostSurface::create({}, id, inputs); ASSERT_TRUE(surface);
  const std::string hash{surface->snapshot_sha256()};
  const auto before = surface->quote_dollars(0U, clock, 1000.0);
  inputs[0].adv_dollars *= 10.0; id.source_sha256[0] = 'b';
  same_bits(surface->quote_dollars(0U, clock, 1000.0).total_dollars, before.total_dollars);
  EXPECT_EQ(surface->snapshot_sha256(), hash); // owned copy
  auto changed = cost::CostSurface::create({}, id, inputs); ASSERT_TRUE(changed);
  EXPECT_NE(changed->snapshot_sha256(), hash);
  auto recipe = cost::CostSurfaceRecipe{}; recipe.spread_scale = 1.5;
  auto scaled = cost::CostSurface::create(recipe, identity(), rows()); ASSERT_TRUE(scaled);
  EXPECT_NE(scaled->recipe_sha256(), surface->recipe_sha256());
  inputs = rows(); std::swap(inputs[0], inputs[1]);
  auto reordered = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(reordered);
  EXPECT_NE(reordered->snapshot_sha256(), hash);
  inputs = rows(); --inputs[0].available_at_ns;
  auto earlier = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(earlier);
  EXPECT_NE(earlier->snapshot_sha256(), hash);
  inputs = rows(); inputs[1].state = cost::CostInputState::Unavailable;
  auto missing = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(missing);
  inputs[1].adv_dollars = nan; inputs[1].daily_vol = nan; inputs[1].available_at_ns = clock + 100;
  auto canonical_missing = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(canonical_missing);
  EXPECT_EQ(missing->snapshot_sha256(), canonical_missing->snapshot_sha256());
  inputs = rows(); inputs[0].daily_vol = 0.0;
  auto plus_zero = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(plus_zero);
  inputs[0].daily_vol = -0.0;
  id = identity(); id.source_sha256.assign(64U, 'A');
  auto minus_zero = cost::CostSurface::create({}, id, inputs); ASSERT_TRUE(minus_zero);
  EXPECT_EQ(plus_zero->snapshot_sha256(), minus_zero->snapshot_sha256());
}

TEST(CostSurface, FutureSnapshotCannotChangePrefixAndPathRequiresChronology) {
  const auto inputs = rows();
  auto first = cost::CostSurface::create({}, identity(), inputs); ASSERT_TRUE(first);
  const std::string prefix_hash{first->snapshot_sha256()};
  const auto prefix_quote = first->quote_dollars(0U, clock, -1000.0);
  auto future = inputs; future[0].available_at_ns = clock + 5; future[0].adv_dollars *= 7.0;
  EXPECT_FALSE(cost::CostSurface::create({}, identity(), future));
  auto later = cost::CostSurface::create({}, identity(clock + 10), future); ASSERT_TRUE(later);
  same_bits(first->quote_dollars(0U, clock, -1000.0).total_dollars, prefix_quote.total_dollars);
  EXPECT_EQ(first->snapshot_sha256(), prefix_hash);
  const std::array<cost::CostSurface, 2> snapshots{*first, *later};
  std::array<cost::CostPathTrade, 2> trades{{{0U, 0U, clock, nav, 1000.0},
                                         {1U, 0U, clock + 10, nav, -2000.0}}};
  ASSERT_TRUE(cost::price_trade_path(snapshots, trades));
  std::swap(trades[0], trades[1]); EXPECT_FALSE(cost::price_trade_path(snapshots, trades));
}

TEST(CostSurface, SignedReversalAndNavScalingUseExecutedDeltas) {
  auto surface = cost::CostSurface::create({}, identity(), rows()); ASSERT_TRUE(surface);
  const auto small = surface->quote_weight_change(0U, clock, -0.02, 1'000'000.0);
  const auto large = surface->quote_weight_change(0U, clock, -0.02, 4'000'000.0);
  ASSERT_TRUE(small.dollars.priced()); ASSERT_TRUE(large.dollars.priced());
  EXPECT_DOUBLE_EQ(small.dollars.filled_dollars, -20'000.0); // +1% to -1% is a 2% trade
  EXPECT_NEAR(large.dollars.impact_dollars / 4'000'000.0,
              2.0 * small.dollars.impact_dollars / 1'000'000.0, 1e-12);
  EXPECT_NEAR(large.dollars.spread_dollars / 4'000'000.0,
              small.dollars.spread_dollars / 1'000'000.0, 1e-12);
  const auto buy = surface->quote_dollars(0U, clock, 20'000.0);
  same_bits(buy.total_dollars, small.dollars.total_dollars);
}

TEST(CostSurface, OverflowFailsClosedAndLargeFiniteCapStaysRepresentableInWeights) {
  auto surface = cost::CostSurface::create({}, identity(), rows()); ASSERT_TRUE(surface);
  const auto maximum = std::numeric_limits<f64>::max();
  EXPECT_EQ(surface->quote_dollars(0U, clock, maximum).status, cost::CostQuoteStatus::NumericalOverflow);
  EXPECT_EQ(surface->quote_weight_change(0U, clock, maximum, 2.0).dollars.status,
            cost::CostQuoteStatus::NumericalOverflow);
  auto inputs = rows(); inputs[0].adv_dollars = 1e200;
  auto recipe = cost::CostSurfaceRecipe{}; recipe.max_participation = 1e200;
  auto enormous = cost::CostSurface::create(recipe, identity(), inputs); ASSERT_TRUE(enormous);
  const auto terms = enormous->coefficients(0U, clock, 1e200);
  ASSERT_TRUE(terms.priced()); EXPECT_DOUBLE_EQ(terms.max_trade_weight, 1e200);
  inputs[0].daily_vol = maximum; recipe.impact_y = maximum;
  EXPECT_FALSE(cost::CostSurface::create(recipe, identity(), inputs));
}
} // namespace atx_test_w1_b1_cost_surface
