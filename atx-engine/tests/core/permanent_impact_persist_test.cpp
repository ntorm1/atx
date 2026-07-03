// permanent_impact_persist_test.cpp — S4-3d [CORRECTNESS, B6]: permanent
// impact must survive across bars. ExecutionSimulator::apply_permanent_impact
// shifts Market::marks_[idx] via shift_mark after a fill, but the NEXT
// update_prices() call overwrites marks_[idx] = r.bar.close for any name
// present in that slice, discarding the shift -- "permanent" impact was
// effectively temporary (it evaporated at the very next close).
//
// Fix: Market carries a per-instrument, additive, accumulating offset;
// update_prices() sets mark = incoming_close + offset instead of clobbering
// it. A zero offset (no fills ever) is byte-identical to today; a no-fill bar
// leaves the offset unchanged (it does not re-accrue).
//
// By-construction fixture: direct Market::shift_mark calls (isolating the
// persistence contract from the impact FORMULA, which S4-3a/limit_fill_clamp
// and the existing temp_perm suite already cover) plus one end-to-end test
// through a real ExecutionSimulator fill to confirm the wiring.

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

namespace atxtest_permanent_impact_persist_test {

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

[[nodiscard]] SliceRow row(atx::u32 id, atx::i64 t, atx::i64 close, atx::i64 vol) noexcept {
  return SliceRow{inst(id), bar(t, close, vol), false};
}

// ===========================================================================
//  ShiftPersistsAcrossNextBarUpdate: RED asserts mark==close+delta on the bar
//  AFTER a direct shift_mark call, when that bar's fresh close equals the
//  PRE-impact price (fails at 100.0 pre-fix -- the shift is discarded).
// ===========================================================================
TEST(PermanentImpactPersist, ShiftPersistsAcrossNextBarUpdate) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 1> r0{row(10, 100, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});
  ASSERT_DOUBLE_EQ(mkt.mark(inst(10)), 100.0); // sanity

  // A fill applies a KNOWN permanent shift of +5.0 (immediate effect, unchanged
  // pre-fix behavior -- shift_mark always moved the CURRENT mark right away).
  mkt.shift_mark(inst(10), 5.0);
  ASSERT_DOUBLE_EQ(mkt.mark(inst(10)), 105.0);

  // Next bar's close reverts to the PRE-impact reference price (100.0) -- the
  // sim's close feed is the unimpacted historical close, per the spec's
  // physical justification. The persisted offset (+5.0) must still apply.
  std::array<SliceRow, 1> r1{row(10, 200, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});

  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 105.0)
      << "a permanent shift must survive the NEXT update_prices call, not be "
         "discarded when the fresh close overwrites the mark";
}

// ===========================================================================
//  NoFillBarLeavesOffsetUnchanged: the offset must not RE-ACCRUE on a bar with
//  no new shift_mark call -- it persists at its CURRENT value, it does not grow.
// ===========================================================================
TEST(PermanentImpactPersist, NoFillBarLeavesOffsetUnchanged) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 1> r0{row(10, 100, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});
  mkt.shift_mark(inst(10), 5.0); // offset -> 5.0

  std::array<SliceRow, 1> r1{row(10, 200, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}}); // no shift this bar
  ASSERT_DOUBLE_EQ(mkt.mark(inst(10)), 105.0);

  std::array<SliceRow, 1> r2{row(10, 300, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(300), std::span<const SliceRow>{r2}}); // still no shift
  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 105.0)
      << "a no-fill bar must not re-accrue the offset -- it stays at its "
         "current value, not grow by another +5.0";
}

// ===========================================================================
//  OffsetAccumulatesAcrossMultipleFills: two separate shift_mark calls on two
//  different bars compose additively (the offset is a running accumulator).
// ===========================================================================
TEST(PermanentImpactPersist, OffsetAccumulatesAcrossMultipleFills) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 1> r0{row(10, 100, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});
  mkt.shift_mark(inst(10), 5.0); // offset -> 5.0

  std::array<SliceRow, 1> r1{row(10, 200, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});
  mkt.shift_mark(inst(10), 3.0); // offset -> 8.0

  std::array<SliceRow, 1> r2{row(10, 300, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(300), std::span<const SliceRow>{r2}});
  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 108.0)
      << "the offset accumulates across fills on separate bars: 5.0+3.0=8.0 on a 100.0 close";
}

// ===========================================================================
//  ZeroFillRun_ByteIdenticalMarks: with NO shift_mark call ever, the offset is
//  always exactly 0 -> mark == fresh close, byte-identical to the pre-fix
//  behavior on every unconfigured/no-impact path.
// ===========================================================================
TEST(PermanentImpactPersist, ZeroFillRun_ByteIdenticalMarks) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 1> r0{row(10, 100, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});
  EXPECT_EQ(mkt.mark(inst(10)), 100.0);

  std::array<SliceRow, 1> r1{row(10, 200, 137, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});
  EXPECT_EQ(mkt.mark(inst(10)), 137.0)
      << "a run with zero fills must be byte-identical -- the offset stays exactly 0";
}

// ===========================================================================
//  EndToEnd_RealFillPersistsThroughExecutionSimulator: drives one large buy
//  through the real settle_pending path, computes the KNOWN permanent shift by
//  the documented formula (delta = ref*0.5*gamma*sigma*part), then asserts the
//  NEXT update_prices (fresh close == the pre-impact ref) reflects it.
// ===========================================================================
TEST(PermanentImpactPersist, EndToEnd_RealFillPersistsThroughExecutionSimulator) {
  std::array<InstrumentId, 1> uni{inst(10)};
  // gamma=2.0, sigma=1.0; Y=0.0 (no TEMPORARY impact) isolates the permanent
  // term so fill_px == ref exactly and delta is hand-computable.
  std::array<InstrumentStats, 1> stats{InstrumentStats{/*adv=*/1000.0, /*sigma=*/1.0, /*spread=*/0.0}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{stats}};

  std::array<SliceRow, 1> r0{row(10, 100, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});

  const SlippageCfg slip{SlippageMode::FixedBps, 0.0, 0.0, 1.0, 1.0}; // 0bps, uncapped
  const ImpactCfg impact{/*Y=*/0.0, /*delta=*/1.0, /*gamma=*/2.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.0, 0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{1.0}};

  // fillable=100, adv=1000 -> part=0.1; perm = 0.5*gamma(2.0)*sigma(1.0)*part(0.1) = 0.1;
  // delta = ref(100.0)*perm(0.1)*dir(+1) = 10.0.
  std::array<OrderPayload, 1> orders{
      OrderPayload{inst(10), /*qty=*/100, OrderType::Market, Decimal{}, ts(1000)}};
  sim.queue(std::span<const OrderPayload>{orders}, ts(1000));
  const auto fills = sim.settle_pending(ts(2000), mkt);
  ASSERT_EQ(fills.size(), 1U);
  ASSERT_DOUBLE_EQ(mkt.mark(inst(10)), 110.0); // sanity: immediate shift applied

  // Next bar's close reverts to the unimpacted 100.0 reference.
  std::array<SliceRow, 1> r1{row(10, 200, 100, 1'000'000)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});
  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 110.0)
      << "a real fill's permanent impact must persist across the next bar's price update";
}

} // namespace atxtest_permanent_impact_persist_test
