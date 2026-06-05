// atx::engine::loop — WeightPolicy signal->weight + reconcile tests (P2-4).
//
// Covers the plan's Tests list verbatim:
//   * dollar-neutral (Sigma w ~= 0);
//   * gross-normalized (Sigma |w| ~= gross_leverage);
//   * rank monotonic in signal (higher signal -> more-positive weight);
//   * NaN entry -> zero weight (and excluded from the cross-section);
//   * ZScore transform path;
//   * reconcile = target - current (no-op when already on target; flat->target
//     generates the buy; flip long->short generates the full delta crossing 0);
//   * reconcile skips NaN / <=0 price; w==0 -> closes the position.
//   * Boundaries: all-equal signal -> all-zero weights (net 0, no div-by-zero);
//     single instrument (degenerate dollar-neutral -> flat); zero equity.
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath>  // std::isnan, std::fabs
#include <limits> // quiet_NaN
#include <span>
#include <vector>

#include "atx/core/decimal.hpp" // Decimal
#include "atx/core/types.hpp"   // f64, i64, usize

#include "atx/engine/exec/payloads.hpp"       // OrderPayload, OrderType, FillPayload
#include "atx/engine/loop/market.hpp"         // Market, InstrumentStats
#include "atx/engine/loop/signal_source.hpp"  // SignalView
#include "atx/engine/loop/types.hpp"          // InstrumentId, Universe
#include "atx/engine/loop/weight_policy.hpp"  // WeightPolicy, Transform
#include "atx/engine/portfolio/portfolio.hpp" // Portfolio

#include "atx/core/datetime.hpp"           // Timestamp
#include "atx/core/domain/domain.hpp"      // Bar, Price, Quantity
#include "atx/engine/loop/panel_types.hpp" // MarketSlice, SliceRow

namespace {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::engine::InstrumentId;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::Portfolio;
using atx::engine::SignalView;
using atx::engine::SliceRow;
using atx::engine::Transform;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::exec::FillPayload;
using atx::engine::exec::OrderPayload;
using atx::engine::exec::OrderType;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kTol = 1e-9; // documented floating-point tolerance for Sigma checks

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }
[[nodiscard]] Timestamp ts(i64 nanos) noexcept { return Timestamp::from_unix_nanos(nanos); }

[[nodiscard]] f64 sum(std::span<const f64> w) noexcept {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += x;
  }
  return s;
}

[[nodiscard]] f64 gross(std::span<const f64> w) noexcept {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += std::fabs(x);
  }
  return s;
}

// Price a Market over `universe` by feeding it a one-row-per-instrument slice
// whose close == the supplied price (whole units). A NaN price leaves that
// instrument unpriced (its row is omitted).
void price_market(Market &mkt, const Universe &universe, std::span<const f64> prices) {
  std::vector<SliceRow> rows;
  rows.reserve(universe.size());
  for (usize i = 0; i < universe.size(); ++i) {
    if (std::isnan(prices[i])) {
      continue; // leave unpriced (Market keeps its NaN sentinel)
    }
    Bar b{ts(1),
          Price::from_int(static_cast<i64>(prices[i])),
          Price::from_int(static_cast<i64>(prices[i])),
          Price::from_int(static_cast<i64>(prices[i])),
          Price::from_int(static_cast<i64>(prices[i])),
          Quantity::from_int(1000)};
    rows.push_back(SliceRow{universe[i], b, false});
  }
  const MarketSlice slice{ts(1), std::span<const SliceRow>{rows}};
  mkt.update_prices(slice);
}

// Seed a holding by applying one buy/sell fill at `price`.
void seed_holding(Portfolio &p, InstrumentId id, i64 qty, f64 price) {
  const FillPayload f{id, qty, Decimal::from_int(static_cast<i64>(price)), Decimal{}, 0.0, ts(0)};
  p.apply_fill(f);
}

// ===========================================================================
//  to_target_weights — transform / neutralize / normalize
// ===========================================================================

TEST(WeightPolicy, RankDollarNeutral_SumIsZero) {
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{1.0, 2.0, 3.0, 4.0, 5.0};
  const WeightPolicy policy{}; // defaults: Rank, dollar_neutral, gross=1.0
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_NEAR(sum(w), 0.0, kTol);
}

TEST(WeightPolicy, RankGrossNormalized_GrossEqualsLeverage) {
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{1.0, 2.0, 3.0, 4.0, 5.0};
  WeightPolicy policy{};
  policy.gross_leverage = 2.0;
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_NEAR(gross(w), 2.0, kTol);
}

TEST(WeightPolicy, RankDefaultLeverage_GrossEqualsOne) {
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{10.0, 20.0, 30.0, 40.0};
  const WeightPolicy policy{};
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_NEAR(gross(w), 1.0, kTol);
}

TEST(WeightPolicy, RankMonotonicInSignal_HigherSignalHigherWeight) {
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{1.0, 2.0, 3.0, 4.0, 5.0};
  const WeightPolicy policy{};
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  // Weights must be strictly increasing where signal is strictly increasing.
  for (usize i = 1; i < w.size(); ++i) {
    EXPECT_LT(w[i - 1], w[i]) << "weight not monotone at i=" << i;
  }
  // Lowest signal -> most negative; highest -> most positive (dollar-neutral).
  EXPECT_LT(w.front(), 0.0);
  EXPECT_GT(w.back(), 0.0);
}

TEST(WeightPolicy, UnshuffledMatchesShuffled_RankIsCrossSectional) {
  const std::array<InstrumentId, 3> u{inst(1), inst(2), inst(3)};
  const std::array<f64, 3> a{1.0, 2.0, 3.0};
  const std::array<f64, 3> b{3.0, 1.0, 2.0}; // same set, permuted
  const WeightPolicy policy{};
  const auto wa = policy.to_target_weights(SignalView{a}, Universe{u});
  const auto wb = policy.to_target_weights(SignalView{b}, Universe{u});

  // The instrument with the largest signal gets the largest weight in both.
  EXPECT_NEAR(wa[2], wb[0], kTol); // signal 3.0 highest in both placements
  EXPECT_NEAR(wa[0], wb[1], kTol); // signal 1.0 lowest in both placements
}

TEST(WeightPolicy, NaNEntry_GetsZeroWeightAndExcluded) {
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{1.0, kNaN, 3.0, 5.0};
  const WeightPolicy policy{};
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_EQ(w[1], 0.0);           // NaN -> exactly zero weight
  EXPECT_NEAR(sum(w), 0.0, kTol); // still dollar-neutral over the live names
  EXPECT_NEAR(gross(w), 1.0, kTol);
  // The NaN name must not perturb the ranking of the live names.
  EXPECT_LT(w[0], w[2]);
  EXPECT_LT(w[2], w[3]);
}

TEST(WeightPolicy, ZScoreTransform_DollarNeutralAndGrossNormalized) {
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{-2.0, -1.0, 0.0, 1.0, 2.0};
  WeightPolicy policy{};
  policy.transform = Transform::ZScore;
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_NEAR(sum(w), 0.0, kTol);
  EXPECT_NEAR(gross(w), 1.0, kTol);
  // Monotone: zscore preserves order.
  for (usize i = 1; i < w.size(); ++i) {
    EXPECT_LT(w[i - 1], w[i]);
  }
}

TEST(WeightPolicy, AllEqualSignal_AllZeroWeights) {
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{7.0, 7.0, 7.0, 7.0};
  const WeightPolicy policy{};
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  for (const f64 x : w) {
    EXPECT_EQ(x, 0.0); // demean of equal ranks -> 0; no division by zero gross
  }
}

TEST(WeightPolicy, SingleInstrument_DegenerateToFlat) {
  const std::array<InstrumentId, 1> u{inst(1)};
  const std::array<f64, 1> sig{3.0};
  const WeightPolicy policy{};
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  ASSERT_EQ(w.size(), 1U);
  EXPECT_EQ(w[0], 0.0); // one name cannot be dollar-neutral with nonzero weight
}

TEST(WeightPolicy, AllNaN_AllZeroWeights) {
  const std::array<InstrumentId, 3> u{inst(1), inst(2), inst(3)};
  const std::array<f64, 3> sig{kNaN, kNaN, kNaN};
  const WeightPolicy policy{};
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  for (const f64 x : w) {
    EXPECT_EQ(x, 0.0);
  }
}

TEST(WeightPolicy, NotDollarNeutral_RankStaysPositiveButGrossNormalized) {
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{1.0, 2.0, 3.0, 4.0};
  WeightPolicy policy{};
  policy.dollar_neutral = false;
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  // No centering: ranks are non-negative, gross-normalized to 1.
  EXPECT_NEAR(gross(w), 1.0, kTol);
  for (const f64 x : w) {
    EXPECT_GE(x, 0.0);
  }
}

TEST(WeightPolicy, OutputLength_EqualsUniverseSize) {
  const std::array<InstrumentId, 6> u{inst(1), inst(2), inst(3), inst(4), inst(5), inst(6)};
  const std::array<f64, 6> sig{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  const WeightPolicy policy{};
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});
  EXPECT_EQ(w.size(), u.size());
}

// ===========================================================================
//  reconcile — order_target_percent (target - current)
// ===========================================================================

TEST(WeightPolicy, ReconcileFromFlat_GeneratesBuyToTarget) {
  const std::array<InstrumentId, 2> u{inst(1), inst(2)};
  const std::array<f64, 2> prices{10.0, 10.0};
  Market mkt{Universe{u}, {}};
  price_market(mkt, Universe{u}, prices);
  Portfolio pf{Decimal::from_int(1000), Universe{u}};
  pf.mark_to_market(mkt);

  // target weight 0.5 on inst(1): target_shares = 0.5 * 1000 / 10 = 50.
  const std::array<f64, 2> w{0.5, -0.5};
  const WeightPolicy policy{};
  const auto orders = policy.reconcile(std::span<const f64>{w}, Universe{u}, pf, mkt, ts(42));

  ASSERT_EQ(orders.size(), 2U);
  EXPECT_EQ(orders[0].id, inst(1));
  EXPECT_EQ(orders[0].qty, 50);  // buy 50
  EXPECT_EQ(orders[1].qty, -50); // short 50
  EXPECT_EQ(orders[0].type, OrderType::Market);
  EXPECT_EQ(orders[0].queued_at.unix_nanos(), 42);
}

TEST(WeightPolicy, ReconcileAlreadyOnTarget_EmitsNoOrder) {
  const std::array<InstrumentId, 2> u{inst(1), inst(2)};
  const std::array<f64, 2> prices{10.0, 10.0};
  Market mkt{Universe{u}, {}};
  price_market(mkt, Universe{u}, prices);
  Portfolio pf{Decimal::from_int(1000), Universe{u}};
  seed_holding(pf, inst(1), 50, 10.0); // already long 50 @ target
  pf.mark_to_market(mkt);

  // Equity after the buy: cash 500 + 50*10 = 1000. target 0.5 -> 50 shares.
  const std::array<f64, 2> w{0.5, 0.0};
  const WeightPolicy policy{};
  const auto orders = policy.reconcile(std::span<const f64>{w}, Universe{u}, pf, mkt, ts(1));

  // inst(1) already on target -> no order; inst(2) flat & target 0 -> no order.
  EXPECT_TRUE(orders.empty());
}

TEST(WeightPolicy, ReconcileFlipLongToShort_GeneratesFullDelta) {
  const std::array<InstrumentId, 1> u{inst(1)};
  const std::array<f64, 1> prices{10.0};
  Market mkt{Universe{u}, {}};
  price_market(mkt, Universe{u}, prices);
  Portfolio pf{Decimal::from_int(1000), Universe{u}};
  seed_holding(pf, inst(1), 30, 10.0); // currently long 30
  pf.mark_to_market(mkt);

  // Equity = cash 700 + 30*10 = 1000. target weight -0.4 -> -40 shares.
  const std::array<f64, 1> w{-0.4};
  const WeightPolicy policy{};
  const auto orders = policy.reconcile(std::span<const f64>{w}, Universe{u}, pf, mkt, ts(1));

  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].qty, -70); // -40 target - (+30) current = -70 (crosses zero)
}

TEST(WeightPolicy, ReconcileZeroWeight_ClosesPosition) {
  const std::array<InstrumentId, 1> u{inst(1)};
  const std::array<f64, 1> prices{10.0};
  Market mkt{Universe{u}, {}};
  price_market(mkt, Universe{u}, prices);
  Portfolio pf{Decimal::from_int(1000), Universe{u}};
  seed_holding(pf, inst(1), 25, 10.0); // long 25
  pf.mark_to_market(mkt);

  const std::array<f64, 1> w{0.0};
  const WeightPolicy policy{};
  const auto orders = policy.reconcile(std::span<const f64>{w}, Universe{u}, pf, mkt, ts(1));

  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].qty, -25); // sell to flat
}

TEST(WeightPolicy, ReconcileNaNPrice_SkipsInstrument) {
  const std::array<InstrumentId, 2> u{inst(1), inst(2)};
  const std::array<f64, 2> prices{kNaN, 10.0}; // inst(1) unpriced
  Market mkt{Universe{u}, {}};
  price_market(mkt, Universe{u}, prices);
  Portfolio pf{Decimal::from_int(1000), Universe{u}};
  pf.mark_to_market(mkt);

  const std::array<f64, 2> w{0.5, 0.5};
  const WeightPolicy policy{};
  const auto orders = policy.reconcile(std::span<const f64>{w}, Universe{u}, pf, mkt, ts(1));

  // inst(1) is skipped (NaN price); only inst(2) produces an order.
  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].id, inst(2));
}

TEST(WeightPolicy, ReconcileZeroEquity_TargetsFlat) {
  const std::array<InstrumentId, 1> u{inst(1)};
  const std::array<f64, 1> prices{10.0};
  Market mkt{Universe{u}, {}};
  price_market(mkt, Universe{u}, prices);
  Portfolio pf{Decimal{}, Universe{u}}; // zero starting cash, flat
  pf.mark_to_market(mkt);

  const std::array<f64, 1> w{0.5};
  const WeightPolicy policy{};
  const auto orders = policy.reconcile(std::span<const f64>{w}, Universe{u}, pf, mkt, ts(1));

  // target = 0.5 * 0 / 10 = 0 shares; current flat -> no order.
  EXPECT_TRUE(orders.empty());
}

TEST(WeightPolicy, ReconcileTruncatesTowardZero_FractionalSharesDropped) {
  const std::array<InstrumentId, 1> u{inst(1)};
  const std::array<f64, 1> prices{30.0};
  Market mkt{Universe{u}, {}};
  price_market(mkt, Universe{u}, prices);
  Portfolio pf{Decimal::from_int(1000), Universe{u}};
  pf.mark_to_market(mkt);

  // target = 0.5 * 1000 / 30 = 16.66.. -> truncate to 16 shares.
  const std::array<f64, 1> w{0.5};
  const WeightPolicy policy{};
  const auto orders = policy.reconcile(std::span<const f64>{w}, Universe{u}, pf, mkt, ts(1));

  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].qty, 16);
}

} // namespace
