// atx::engine::exec — ExecutionSimulator cost-honesty + determinism tests (P2-6).
//
// The ExecutionSimulator is Phase-2's cost-honesty core: queued orders settle on
// a STRICTLY LATER slice (the no-look-ahead firewall), priced through a fixed
// pipeline — slippage -> temporary √-impact (into the fill PRICE) -> commission —
// with permanent impact shifting the instrument MARK (and only the mark). The
// volume cap bounds participation per bar; an order that exceeds the cap fills
// partially and the remainder spills to the next slice.
//
// Covers the plan's Tests list verbatim:
//   * firewall: order queued at t does NOT fill at t; fills at t+1;
//   * same-bar cheat OFF by default; ON only when FillCfg.allow_same_bar_fill;
//   * fill price worse than mid by the modeled cost (buy pays MORE, sell LESS);
//   * MONOTONIC in participation (2x order qty -> strictly worse fill price);
//   * zero-size order -> no fill / no cost (rejected at queue);
//   * volume cap -> partial fill; remainder fills next slice; sum == order qty;
//   * exact-cap fill (fillable lands exactly on the cap);
//   * zero bar volume -> no fill (cap is zero);
//   * permanent impact shifts the mark; temporary impact does NOT persist in it;
//   * commission min floor + max cap (PerShare); per-dollar commission;
//   * limit order not penetrated -> no fill; penetrated -> fills;
//   * deterministic across two identical sims (no RNG by construction).
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>

#include "atx/core/datetime.hpp"      // Timestamp
#include "atx/core/decimal.hpp"       // Decimal
#include "atx/core/domain/domain.hpp" // Bar, Price, Quantity
#include "atx/core/types.hpp"         // u32, i64, f64

#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator + cfg structs
#include "atx/engine/exec/payloads.hpp"      // OrderPayload, FillPayload, OrderType
#include "atx/engine/loop/market.hpp"        // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp"   // MarketSlice, SliceRow
#include "atx/engine/loop/types.hpp"         // InstrumentId

namespace {

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

// ---- helpers --------------------------------------------------------------

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

[[nodiscard]] SliceRow row(atx::u32 id, atx::i64 t, atx::i64 close, atx::i64 vol) noexcept {
  return SliceRow{inst(id), bar(t, close, vol), false};
}

// A market order with the canonical signed-qty direction (+buy / -sell).
[[nodiscard]] OrderPayload market_order(atx::u32 id, atx::i64 qty, atx::i64 queued_nanos) noexcept {
  return OrderPayload{inst(id), qty, OrderType::Market, Decimal{}, ts(queued_nanos)};
}

[[nodiscard]] OrderPayload limit_order(atx::u32 id, atx::i64 qty, atx::i64 limit_whole,
                                       atx::i64 queued_nanos) noexcept {
  return OrderPayload{inst(id), qty, OrderType::Limit, Decimal::from_int(limit_whole),
                      ts(queued_nanos)};
}

// Build a single-instrument book priced at `close`/`vol`, with the given stats.
// The instrument id is always 10; storage is heap-owned by the fixture-free
// helper via the returned struct so the universe span stays valid.
struct Book {
  std::array<InstrumentId, 1> universe;
  std::array<InstrumentStats, 1> stats;
  Market market;

  Book(atx::i64 close, atx::i64 vol, InstrumentStats s)
      : universe{inst(10)}, stats{s},
        market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}} {
    std::array<SliceRow, 1> rows{row(10, 100, close, vol)};
    market.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{rows}});
  }
};

// A simulator configured with NO statistics-driven cost (k=0, bps=0, spread=0,
// Y=0, gamma=0, no commission) so a fill reproduces the raw mark exactly — used
// to isolate the firewall / volume-cap / determinism behaviour from the pricing.
[[nodiscard]] ExecutionSimulator zero_cost_sim() {
  const SlippageCfg slip{SlippageMode::VolumeShare, /*k=*/0.0, /*bps=*/0.0,
                         /*cap_volshare=*/0.025, /*cap_bps=*/0.10};
  const ImpactCfg impact{/*Y=*/0.0, /*delta=*/0.5, /*gamma=*/0.0};
  const CommissionCfg comm{CommissionMode::PerShare, /*per_share=*/0.0, /*min_fee=*/0.0,
                           /*max_pct=*/0.005, /*per_dollar_bps=*/0.0};
  return ExecutionSimulator{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};
}

// =====================================================================
//  Firewall — an order queued at t cannot fill at t; it fills strictly later.
// =====================================================================

TEST(ExecSim, Firewall_QueuedAtT_DoesNotFillAtSameSlice) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  ExecutionSimulator sim = zero_cost_sim();

  std::array<OrderPayload, 1> orders{market_order(10, 100, /*queued=*/1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));

  // Settling at the SAME timestamp the order was queued at must NOT fill (no
  // look-ahead: the bar that queued the order has not yet sealed downstream).
  const auto fills = sim.settle_pending(ts(1000), b.market);
  EXPECT_TRUE(fills.empty());
}

TEST(ExecSim, Firewall_QueuedAtT_FillsAtNextSlice) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  ExecutionSimulator sim = zero_cost_sim();

  std::array<OrderPayload, 1> orders{market_order(10, 100, /*queued=*/1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));

  EXPECT_TRUE(sim.settle_pending(ts(1000), b.market).empty()); // same slice: no fill
  const auto fills = sim.settle_pending(ts(2000), b.market);   // strictly later: fills
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].qty, 100);
  EXPECT_EQ(fills[0].t, ts(2000));
}

// =====================================================================
//  Same-bar cheat — OFF by default, ON only when explicitly enabled.
// =====================================================================

TEST(ExecSim, SameBarFill_DefaultOff_NoFillAtQueueSlice) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  ExecutionSimulator sim = zero_cost_sim(); // FillCfg{} default => allow_same_bar_fill == false

  std::array<OrderPayload, 1> orders{market_order(10, 100, /*queued=*/1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  EXPECT_TRUE(sim.settle_pending(ts(1000), b.market).empty());
}

TEST(ExecSim, SameBarFill_Enabled_FillsAtQueueSlice) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  FillCfg fill{};
  fill.allow_same_bar_fill = true; // the documented "cheat" — opt-in only
  ExecutionSimulator sim{fill, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};

  std::array<OrderPayload, 1> orders{market_order(10, 100, /*queued=*/1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto fills = sim.settle_pending(ts(1000), b.market); // same slice now fills
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].qty, 100);
}

// =====================================================================
//  Latency — an order is ineligible until queued_at + latency elapses.
// =====================================================================

TEST(ExecSim, Latency_BeforeElapsed_NoFill_AfterElapsed_Fills) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  ExecutionSimulator sim{FillCfg{},     slip, impact, comm, LatencyCfg{/*latency_nanos=*/5000},
                         VolumeCapCfg{}};

  std::array<OrderPayload, 1> orders{market_order(10, 100, /*queued=*/1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));

  // t=2000: strictly later than queue but < queued_at + latency (6000) => no fill.
  EXPECT_TRUE(sim.settle_pending(ts(2000), b.market).empty());
  // t=6000 == queued_at + latency => eligible.
  const auto fills = sim.settle_pending(ts(6000), b.market);
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].qty, 100);
}

// =====================================================================
//  Direction — a buy pays MORE than mid, a sell receives LESS than mid.
// =====================================================================

TEST(ExecSim, Slippage_Buy_FillAboveMid_Sell_FillBelowMid) {
  // VolumeShare slippage with a non-trivial share so the cost is visible.
  const InstrumentStats stats{/*adv=*/1'000'000.0, /*sigma=*/0.0, /*spread=*/0.0};
  Book b{/*close=*/100, /*vol=*/10'000, stats};
  const SlippageCfg slip{SlippageMode::VolumeShare, /*k=*/0.1, 0.0, /*cap=*/0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};

  ExecutionSimulator buy_sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};
  std::array<OrderPayload, 1> buy{market_order(10, /*qty=*/100, 1000)};
  buy_sim.queue(std::span<const OrderPayload>{buy}, ts(1000));
  const auto bf = buy_sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(bf.size(), 1U);
  EXPECT_GT(bf[0].price.to_double(), 100.0); // buy pays MORE than the 100 mid

  Book b2{/*close=*/100, /*vol=*/10'000, stats};
  ExecutionSimulator sell_sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};
  std::array<OrderPayload, 1> sell{market_order(10, /*qty=*/-100, 1000)};
  sell_sim.queue(std::span<const OrderPayload>{sell}, ts(1000));
  const auto sf = sell_sim.settle_pending(ts(2000), b2.market);
  ASSERT_EQ(sf.size(), 1U);
  EXPECT_LT(sf[0].price.to_double(), 100.0); // sell receives LESS than the 100 mid
}

// =====================================================================
//  Monotonicity — a larger order fills at a strictly worse price.
//
//  Both orders are within the per-bar volume cap so they fill fully in one
//  slice; the bigger order has the higher participation -> bigger slippage and
//  bigger temporary impact -> worse (higher, for a buy) fill price.
// =====================================================================

TEST(ExecSim, Participation_LargerBuy_FillsStrictlyWorse) {
  const InstrumentStats stats{/*adv=*/1'000'000.0, /*sigma=*/0.02, /*spread=*/0.0};
  const SlippageCfg slip{SlippageMode::VolumeShare, /*k=*/0.1, 0.0, /*cap=*/0.025, 0.10};
  const ImpactCfg impact{/*Y=*/1.0, /*delta=*/0.5, /*gamma=*/0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  // Generous cap so both orders fill fully in one slice.
  const VolumeCapCfg cap{/*volume_limit=*/0.5};

  Book small{/*close=*/100, /*vol=*/100'000, stats};
  ExecutionSimulator s1{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};
  std::array<OrderPayload, 1> o1{market_order(10, /*qty=*/1'000, 1000)};
  s1.queue(std::span<const OrderPayload>{o1}, ts(1000));
  const auto f1 = s1.settle_pending(ts(2000), small.market);
  ASSERT_EQ(f1.size(), 1U);

  Book big{/*close=*/100, /*vol=*/100'000, stats};
  ExecutionSimulator s2{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};
  std::array<OrderPayload, 1> o2{market_order(10, /*qty=*/2'000, 1000)};
  s2.queue(std::span<const OrderPayload>{o2}, ts(1000));
  const auto f2 = s2.settle_pending(ts(2000), big.market);
  ASSERT_EQ(f2.size(), 1U);
  EXPECT_EQ(f2[0].qty, 2'000); // confirm both filled fully in one slice

  EXPECT_GT(f2[0].price.to_double(), f1[0].price.to_double()); // bigger buy => worse fill
}

// =====================================================================
//  Zero-size order — rejected at queue; never fills, never costs.
// =====================================================================

TEST(ExecSim, ZeroSize_Order_NeverFills) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  ExecutionSimulator sim = zero_cost_sim();

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/0, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  EXPECT_TRUE(sim.settle_pending(ts(2000), b.market).empty());
}

// =====================================================================
//  Volume cap — partial fill, remainder spills, sum of partials == order qty.
// =====================================================================

TEST(ExecSim, VolumeCap_OrderExceedsCap_FillsPartially) {
  // cap = 0.1 * 10000 = 1000 shares/bar; order is 2500 shares.
  const VolumeCapCfg cap{/*volume_limit=*/0.1};
  Book b{/*close=*/100, /*vol=*/10'000, InstrumentStats{}};
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/2'500, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));

  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_EQ(f[0].qty, 1'000); // capped at 0.1 * 10000
}

TEST(ExecSim, VolumeCap_Remainder_FillsAcrossSlices_SumEqualsOrderQty) {
  const VolumeCapCfg cap{/*volume_limit=*/0.1}; // 1000 shares/bar
  Book b{/*close=*/100, /*vol=*/10'000, InstrumentStats{}};
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/2'500, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));

  atx::i64 total = 0;
  // Each later slice is a NEW bar (distinct timestamp), so the per-bar cap resets
  // and another 1000 (then the 500 remainder) fills.
  for (atx::i64 slice = 2000; slice <= 4000; slice += 1000) {
    const auto f = sim.settle_pending(ts(slice), b.market);
    for (const auto &fill : f) {
      total += fill.qty;
    }
  }
  EXPECT_EQ(total, 2'500); // sum of partials == original order qty
}

TEST(ExecSim, VolumeCap_ExactCap_FillsFullyInOneSlice) {
  const VolumeCapCfg cap{/*volume_limit=*/0.1}; // 1000 shares/bar
  Book b{/*close=*/100, /*vol=*/10'000, InstrumentStats{}};
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/1'000, 1000)}; // exactly the cap
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_EQ(f[0].qty, 1'000); // exact-cap fill, fully in one slice
}

// =====================================================================
//  Zero bar volume — cap is zero, nothing fills, order stays open.
// =====================================================================

TEST(ExecSim, ZeroBarVolume_NoFill_OrderStaysOpen) {
  Book b{/*close=*/100, /*vol=*/0, InstrumentStats{}}; // zero volume bar
  ExecutionSimulator sim = zero_cost_sim();

  std::array<OrderPayload, 1> orders{market_order(10, 100, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  EXPECT_TRUE(sim.settle_pending(ts(2000), b.market).empty()); // cap == 0 => no fill
}

// =====================================================================
//  Permanent vs temporary impact — perm shifts the mark, temp does not.
// =====================================================================

TEST(ExecSim, PermanentImpact_Buy_ShiftsMarkUp) {
  const InstrumentStats stats{/*adv=*/1'000'000.0, /*sigma=*/0.02, /*spread=*/0.0};
  Book b{/*close=*/100, /*vol=*/100'000, stats};
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10}; // no slippage
  const ImpactCfg impact{/*Y=*/0.0, /*delta=*/0.5, /*gamma=*/0.314};        // perm only
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  const VolumeCapCfg cap{/*volume_limit=*/0.5};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};

  EXPECT_DOUBLE_EQ(b.market.mark(inst(10)), 100.0);
  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/10'000, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);

  // Permanent impact (gamma>0) moves the mark UP for a buy.
  EXPECT_GT(b.market.mark(inst(10)), 100.0);
}

TEST(ExecSim, TemporaryImpact_DoesNotPersistInMark) {
  const InstrumentStats stats{/*adv=*/1'000'000.0, /*sigma=*/0.02, /*spread=*/0.0};
  Book b{/*close=*/100, /*vol=*/100'000, stats};
  // Temporary impact ON (Y>0), permanent OFF (gamma==0): the fill price moves but
  // the MARK must be unchanged (temporary impact never persists).
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{/*Y=*/1.0, /*delta=*/0.5, /*gamma=*/0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  const VolumeCapCfg cap{/*volume_limit=*/0.5};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/10'000, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_GT(f[0].price.to_double(), 100.0); // temporary impact IS in the fill price
  EXPECT_GT(f[0].impact, 0.0);              // and recorded for attribution

  EXPECT_DOUBLE_EQ(b.market.mark(inst(10)), 100.0); // but the mark did NOT move
}

// =====================================================================
//  Commission — PerShare min floor + max cap, and PerDollar mode.
// =====================================================================

TEST(ExecSim, Commission_PerShare_MinFeeFloorApplied) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  // 10 shares * $0.005 = $0.05, below the $1 min floor -> fee == $1.
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, /*per_share=*/0.005, /*min_fee=*/1.0,
                           /*max_pct=*/0.5, /*per_dollar_bps=*/0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/10, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_DOUBLE_EQ(f[0].fee.to_double(), 1.0); // min-fee floor
}

TEST(ExecSim, Commission_PerShare_MaxPctCapApplied) {
  Book b{/*close=*/1, /*vol=*/1'000'000, InstrumentStats{}};
  // 100 shares * $0.005 = $0.50 raw; notional = 100 * $1 = $100; cap = 0.1% * 100
  // = $0.10. The cap is below the raw per-share fee and below min_fee, so the cap
  // wins -> fee == $0.10. (max cap dominates min floor when the cap is smaller.)
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, /*per_share=*/0.005, /*min_fee=*/1.0,
                           /*max_pct=*/0.001, /*per_dollar_bps=*/0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/100, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_NEAR(f[0].fee.to_double(), 0.10, 1e-6); // max-pct cap
}

TEST(ExecSim, Commission_PerDollar_NotionalBpsApplied) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  // notional = 100 * $100 = $10,000; 15 bps => $15.
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerDollar, /*per_share=*/0.005, /*min_fee=*/1.0,
                           /*max_pct=*/0.005, /*per_dollar_bps=*/15.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/100, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_NEAR(f[0].fee.to_double(), 15.0, 1e-6);
}

// =====================================================================
//  Limit orders — penetration gate.
// =====================================================================

TEST(ExecSim, LimitBuy_RefAboveLimit_NoFill) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  ExecutionSimulator sim = zero_cost_sim();
  // buy limit at 90 with ref 100: ref > limit => not marketable => no fill.
  std::array<OrderPayload, 1> orders{limit_order(10, /*qty=*/100, /*limit=*/90, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  EXPECT_TRUE(sim.settle_pending(ts(2000), b.market).empty());
}

TEST(ExecSim, LimitBuy_RefAtOrBelowLimit_Fills) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  ExecutionSimulator sim = zero_cost_sim();
  // buy limit at 110 with ref 100: ref <= limit => marketable => fills.
  std::array<OrderPayload, 1> orders{limit_order(10, /*qty=*/100, /*limit=*/110, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_EQ(f[0].qty, 100);
}

TEST(ExecSim, LimitSell_RefBelowLimit_NoFill) {
  Book b{/*close=*/100, /*vol=*/1'000'000, InstrumentStats{}};
  ExecutionSimulator sim = zero_cost_sim();
  // sell limit at 110 with ref 100: ref < limit => not marketable => no fill.
  std::array<OrderPayload, 1> orders{limit_order(10, /*qty=*/-100, /*limit=*/110, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  EXPECT_TRUE(sim.settle_pending(ts(2000), b.market).empty());
}

// =====================================================================
//  Determinism — two identical sims fed identical input produce identical fills.
// =====================================================================

TEST(ExecSim, Determinism_TwoIdenticalRuns_ProduceIdenticalFills) {
  const InstrumentStats stats{/*adv=*/1'000'000.0, /*sigma=*/0.02, /*spread=*/0.01};
  const SlippageCfg slip{SlippageMode::VolumeShare, 0.1, 0.0, 0.025, 0.10};
  const ImpactCfg impact{1.0, 0.5, 0.314};
  const CommissionCfg comm{CommissionMode::PerShare, 0.005, 1.0, 0.005, 0.0};
  const VolumeCapCfg cap{0.05};

  auto run = [&](FillPayload &out_fill) {
    Book b{/*close=*/100, /*vol=*/200'000, stats};
    ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};
    std::array<OrderPayload, 2> orders{market_order(10, 5'000, 1000),
                                       market_order(10, 3'000, 1000)};
    sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
    const auto f = sim.settle_pending(ts(2000), b.market);
    ASSERT_FALSE(f.empty());
    out_fill = f[0];
  };

  FillPayload a{};
  FillPayload c{};
  run(a);
  run(c);

  EXPECT_EQ(a.qty, c.qty);
  EXPECT_EQ(a.price, c.price); // exact Decimal equality — bit-for-bit reproducible
  EXPECT_EQ(a.fee, c.fee);
  EXPECT_DOUBLE_EQ(a.impact, c.impact);
}

// =====================================================================
//  Spread floor — a fill always crosses at least half the spread.
// =====================================================================

TEST(ExecSim, SpreadFloor_ZeroSlippage_StillCrossesHalfSpread) {
  // spread = 2.0 (price units); half-spread = 1.0. With zero slippage/impact a
  // buy must still pay at least mid + half-spread = 101.
  const InstrumentStats stats{/*adv=*/1'000'000.0, /*sigma=*/0.0, /*spread=*/2.0};
  Book b{/*close=*/100, /*vol=*/1'000'000, stats};
  const SlippageCfg slip{SlippageMode::VolumeShare, /*k=*/0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{}};

  std::array<OrderPayload, 1> orders{market_order(10, /*qty=*/100, 1000)};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto f = sim.settle_pending(ts(2000), b.market);
  ASSERT_EQ(f.size(), 1U);
  EXPECT_NEAR(f[0].price.to_double(), 101.0, 1e-6); // mid + half-spread
}

} // namespace
