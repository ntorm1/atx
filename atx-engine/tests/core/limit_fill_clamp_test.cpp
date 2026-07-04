// limit_fill_clamp_test.cpp — S4-3a [CORRECTNESS, B3]: limit orders must never
// fill THROUGH their limit after costs. ExecutionSimulator::settle_one gates a
// fill on the RAW ref via limit_marketable (buy iff ref<=limit, sell iff
// ref>=limit), then emit_fill composes slippage+temporary-impact onto ref with
// NO re-clamp to the limit -- a buy-limit that passed at ref<=limit can still
// execute at fill_px>limit once costs are added (a sell mirrors this on the
// other side). This books a materially better price than any real fill could
// achieve.
//
// By-construction fixture: ref==limit==100.0 exactly (limit_marketable passes
// on the boundary), FixedBps slippage (25bps) + linear temporary impact
// (delta=1, Y=25, sigma=1.0, part=fillable/adv=100/1e6=1e-4 -> temp=25bps) so
// the composed cost is HAND-COMPUTABLE and isolated from VolumeShare's share^2
// nonlinearity:
//   fill_px = ref * (1+slip) * (1+temp) = 100 * 1.0025 * 1.0025 = 100.500625
// A buy books that PRICE (worse than its limit); a sell mirrors (dir=-1) to
// ref*(1-slip)*(1-temp) = 100*0.9975*0.9975 = 99.500625 -- WORSE for a seller
// (receives less than its floor). Both cross the limit by construction.

#include <gtest/gtest.h>

#include <array>
#include <span>

#include "atx/core/datetime.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/exec/payloads.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/types.hpp"

namespace atxtest_limit_fill_clamp_test {

using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::SliceRow;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::FillPayload;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::OrderPayload;
using atx::engine::exec::OrderType;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }
[[nodiscard]] Timestamp ts(atx::i64 nanos) noexcept { return Timestamp::from_unix_nanos(nanos); }

[[nodiscard]] Bar bar(atx::i64 t, atx::i64 close, atx::i64 vol) noexcept {
  return Bar{ts(t),
             Price::from_int(close),
             Price::from_int(close),
             Price::from_int(close),
             Price::from_int(close),
             Quantity::from_int(vol)};
}

struct Book {
  std::array<InstrumentId, 1> universe;
  std::array<InstrumentStats, 1> stats;
  Market market;

  Book(atx::i64 close, atx::i64 vol, InstrumentStats s)
      : universe{inst(10)}, stats{s},
        market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}} {
    std::array<SliceRow, 1> rows{SliceRow{inst(10), bar(100, close, vol), false}};
    market.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{rows}});
  }
};

// 25bps FixedBps slippage + Y=25/delta=1/sigma=1.0 linear temporary impact:
// at part=fillable/adv=1e-4 (fillable=100, adv=1e6), both slip and temp are
// EXACTLY 0.0025 (25bps) -- composed multiplicatively, fill_px overshoots the
// ref by ~0.5006% (100 -> 100.500625 buy / 99.750625 sell).
[[nodiscard]] ExecutionSimulator overshoot_sim() {
  const SlippageCfg slip{SlippageMode::FixedBps, /*k=*/0.0, /*bps=*/25.0,
                         /*cap_volshare=*/0.025, /*cap_bps=*/1.0};
  const ImpactCfg impact{/*Y=*/25.0, /*delta=*/1.0, /*gamma=*/0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  return ExecutionSimulator{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};
}

// adv=1e6 (part=100/1e6=1e-4 at fillable=100), sigma=1.0 (isolates temp's Y
// coefficient), spread=0 (no spread floor interference).
[[nodiscard]] InstrumentStats overshoot_stats() noexcept {
  return InstrumentStats{/*adv=*/1.0e6, /*sigma=*/1.0, /*spread=*/0.0};
}

// ===========================================================================
//  limit_fill_never_through — buy: RED asserts fill_px<=limit (fails at
//  100.500625 > 100 pre-fix); sell mirrors (fails at 99.750625 < 100 pre-fix).
// ===========================================================================
TEST(LimitFillClamp, Buy_NeverFillsAboveLimit) {
  Book b{/*close=*/100, /*vol=*/1'000'000, overshoot_stats()};
  ExecutionSimulator sim = overshoot_sim();
  std::array<OrderPayload, 1> orders{
      OrderPayload{inst(10), /*qty=*/100, OrderType::Limit, Decimal::from_int(100), ts(1000)}};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto fills = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_LE(fills[0].price.to_double(), 100.0)
      << "a buy-limit must never fill above its limit; got " << fills[0].price.to_double();
}

TEST(LimitFillClamp, Sell_NeverFillsBelowLimit) {
  Book b{/*close=*/100, /*vol=*/1'000'000, overshoot_stats()};
  ExecutionSimulator sim = overshoot_sim();
  std::array<OrderPayload, 1> orders{
      OrderPayload{inst(10), /*qty=*/-100, OrderType::Limit, Decimal::from_int(100), ts(1000)}};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto fills = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_GE(fills[0].price.to_double(), 100.0)
      << "a sell-limit must never fill below its limit; got " << fills[0].price.to_double();
}

// ===========================================================================
//  Market orders take no clamp path -- byte-identical to the unclamped
//  slippage+impact composition (the pre-fix formula, still correct for Market).
// ===========================================================================
TEST(LimitFillClamp, MarketOrder_UnaffectedByClamp) {
  Book b{/*close=*/100, /*vol=*/1'000'000, overshoot_stats()};
  ExecutionSimulator sim = overshoot_sim();
  std::array<OrderPayload, 1> orders{
      OrderPayload{inst(10), /*qty=*/100, OrderType::Market, Decimal{}, ts(1000)}};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto fills = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(fills.size(), 1U);
  // 100 * 1.0025 * 1.0025 = 100.500625 exactly (FixedBps has no ADV-nonlinearity).
  EXPECT_NEAR(fills[0].price.to_double(), 100.500625, 1e-9)
      << "a Market order must be priced by the raw slip+temp composition, unclamped";
}

// ===========================================================================
//  Clamp is INERT when costs already keep the fill on the favorable side of a
//  non-binding limit (a buy-limit far above ref, a sell-limit far below).
// ===========================================================================
TEST(LimitFillClamp, NonBindingLimit_ClampIsNoOp) {
  Book b{/*close=*/100, /*vol=*/1'000'000, overshoot_stats()};
  ExecutionSimulator sim = overshoot_sim();
  // Buy-limit at 200: ref(100)<=limit(200) passes marketability, and the
  // overshoot fill (100.500625) never approaches 200 -- clamp never binds.
  std::array<OrderPayload, 1> orders{
      OrderPayload{inst(10), /*qty=*/100, OrderType::Limit, Decimal::from_int(200), ts(1000)}};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto fills = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_NEAR(fills[0].price.to_double(), 100.500625, 1e-9)
      << "a non-binding limit must not perturb the priced fill";
}

} // namespace atxtest_limit_fill_clamp_test
