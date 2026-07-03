// market_absent_volume_test.cpp — S4-3b [CORRECTNESS, B4]: absent instruments
// must not provide phantom liquidity. Market::update_prices historically
// touched only slice-present rows; an instrument absent from a frontier (e.g.
// delisted) kept its PRIOR volumes_[idx], which bar_volume() then fed into
// ExecutionSimulator::volume_capped_qty as if the name still traded -- a
// delisted/halted name could still be "filled" on stale volume.
//
// By-construction fixture: a 2-name universe; slice 1 prices BOTH names with a
// nonzero volume; slice 2 carries ONLY name A. Name B's executable volume must
// be exactly 0 after slice 2 (it did not trade), while its MARK is
// deliberately UNCHANGED (a persistent last-value MTM reference is a separate,
// legitimate concern this fix does not touch).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
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

namespace atxtest_market_absent_volume_test {

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
//  market_absent_zeroes_volume: bar_volume(B) must be exactly 0 after a slice
//  that carries only A. RED (pre-fix): stale volume persists (fails at 2000).
// ===========================================================================
TEST(MarketAbsentVolume, AbsentInstrument_BarVolumeZeroedNextSlice) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 2> r0{row(10, 100, 50, 1000), row(20, 100, 75, 2000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});
  ASSERT_DOUBLE_EQ(mkt.bar_volume(inst(20)), 2000.0); // sanity: slice 1 set it

  // Slice 2 carries ONLY instrument 10 (20 is absent -- e.g. delisted/halted).
  std::array<SliceRow, 1> r1{row(10, 200, 55, 1100)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});

  EXPECT_DOUBLE_EQ(mkt.bar_volume(inst(20)), 0.0)
      << "an instrument absent from the slice did not trade -> 0 executable volume";
  // A's own volume refreshes normally.
  EXPECT_DOUBLE_EQ(mkt.bar_volume(inst(10)), 1100.0);
  // MARK is a SEPARATE, persistent last-value concern -- unchanged for B (this
  // fix is volume-scoped only; see header note + Market.UpdatePrices_InstrumentAbsentFromSlice_RetainsPriorMark).
  EXPECT_DOUBLE_EQ(mkt.mark(inst(20)), 75.0);
}

// ===========================================================================
//  Follow-on: an order on the absent name settles to ZERO fillable via the
//  real ExecutionSimulator (volume_capped_qty reads bar_volume == 0 -> no
//  phantom fill on a name that did not trade this bar).
// ===========================================================================
TEST(MarketAbsentVolume, AbsentInstrument_OrderNeverFillsOnStaleVolume) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 2> r0{row(10, 100, 50, 1000), row(20, 100, 75, 2000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});

  // Slice 2: instrument 20 goes absent (delisted). Its mark stays priced
  // (75.0) so an order against it is still "eligible" on the ref-price gate --
  // only the executable-volume budget must gate the fill to zero.
  std::array<SliceRow, 1> r1{row(10, 200, 55, 1100)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});

  const SlippageCfg slip{SlippageMode::VolumeShare, 0.0, 0.0, 0.025, 0.10};
  const ImpactCfg impact{0.0, 0.5, 0.0};
  const CommissionCfg comm{CommissionMode::PerShare, 0.0, 0.0, 0.005, 0.0};
  ExecutionSimulator sim{FillCfg{}, slip, impact, comm, LatencyCfg{}, VolumeCapCfg{1.0}};

  std::array<OrderPayload, 1> orders{
      OrderPayload{inst(20), /*qty=*/100, OrderType::Market, Decimal{}, ts(200)}};
  sim.queue(std::span<const OrderPayload>{orders}, ts(200));
  const auto fills = sim.settle_pending(ts(300), mkt);
  EXPECT_TRUE(fills.empty())
      << "a delisted/halted name with zero executable volume must not phantom-fill";
}

} // namespace atxtest_market_absent_volume_test
