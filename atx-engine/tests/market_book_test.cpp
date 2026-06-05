// atx::engine — Market price/stats book tests (P2-5).
//
// Market is the loop's CURRENT market-state book: a dense, fixed-universe table
// of the latest reference mark + last sealed bar volume per instrument, plus a
// per-instrument cost-parameter record (InstrumentStats) calibrated downstream.
//
// NOTE: a separate `market_test.cpp` already covers the Phase-1
// `atx::engine::data::MarketPayload` event type — a DIFFERENT type. This file
// covers the Phase-2 loop book `atx::engine::Market` in `loop/market.hpp`. The
// suite names (Market / MarketDeathTest) intentionally share the `-R "Market"`
// ctest filter with that file.
//
// Covers the plan's Market test list:
//   * update_prices sets mark + bar_volume from a MarketSlice's bars;
//   * the latest slice wins (a second update overwrites the first);
//   * an instrument absent from a slice keeps its prior mark (no stale wipe);
//   * mark() is NaN before the first slice sets it;
//   * shift_mark moves the mark by a delta (the permanent-impact hook);
//   * stats() returns the configured params, and zero-default when none given;
//   * bar_volume() reflects the latest slice;
//   * EXPECT_DEATH on an out-of-universe id (ATX_ASSERT precondition).
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>

#include "atx/core/datetime.hpp"      // Timestamp
#include "atx/core/domain/domain.hpp" // Bar, Price, Quantity
#include "atx/core/types.hpp"         // u32, i64, f64

#include "atx/engine/loop/market.hpp"      // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp" // MarketSlice, SliceRow
#include "atx/engine/loop/types.hpp"       // InstrumentId

namespace {

using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::SliceRow;

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

// =====================================================================
//  update_prices — mark + bar_volume taken from the slice's bars.
// =====================================================================

TEST(Market, UpdatePrices_SingleSlice_SetsMarkAndVolume) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 2> rows{row(10, 100, 50, 1000), row(20, 100, 75, 2000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{rows}});

  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 50.0);
  EXPECT_DOUBLE_EQ(mkt.mark(inst(20)), 75.0);
  EXPECT_DOUBLE_EQ(mkt.bar_volume(inst(10)), 1000.0);
  EXPECT_DOUBLE_EQ(mkt.bar_volume(inst(20)), 2000.0);
}

// =====================================================================
//  Latest slice wins — a later update overwrites the earlier mark.
// =====================================================================

TEST(Market, UpdatePrices_SecondSlice_OverwritesMark) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 1> r0{row(10, 100, 50, 1000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});
  std::array<SliceRow, 1> r1{row(10, 200, 60, 1500)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});

  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 60.0);
  EXPECT_DOUBLE_EQ(mkt.bar_volume(inst(10)), 1500.0);
}

// =====================================================================
//  Absent instrument keeps its prior mark (no stale wipe).
//
//  A universe member that does not appear in a later slice (it did not trade
//  that frontier) retains its last known mark — the book is a persistent
//  last-value table, not a per-slice snapshot.
// =====================================================================

TEST(Market, UpdatePrices_InstrumentAbsentFromSlice_RetainsPriorMark) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 2> r0{row(10, 100, 50, 1000), row(20, 100, 75, 2000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});

  // Second slice only carries instrument 10.
  std::array<SliceRow, 1> r1{row(10, 200, 55, 1100)};
  mkt.update_prices(MarketSlice{ts(200), std::span<const SliceRow>{r1}});

  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 55.0);
  EXPECT_DOUBLE_EQ(mkt.mark(inst(20)), 75.0); // unchanged
}

// =====================================================================
//  mark() is NaN before any slice sets it.
// =====================================================================

TEST(Market, Mark_BeforeAnyUpdate_IsNaN) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  EXPECT_TRUE(std::isnan(mkt.mark(inst(10))));
  EXPECT_DOUBLE_EQ(mkt.bar_volume(inst(10)), 0.0);
}

// =====================================================================
//  shift_mark — the permanent-impact hook moves the mark by a delta.
// =====================================================================

TEST(Market, ShiftMark_AfterUpdate_MovesMarkByDelta) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 1> r0{row(10, 100, 50, 1000)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{r0}});

  mkt.shift_mark(inst(10), 0.25);
  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 50.25);

  mkt.shift_mark(inst(10), -0.75);
  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 49.5);
}

// =====================================================================
//  stats() — configured params parallel to the universe.
// =====================================================================

TEST(Market, Stats_Configured_ReturnsParallelParams) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  std::array<InstrumentStats, 2> cfg{InstrumentStats{1000.0, 0.02, 0.01},
                                     InstrumentStats{2000.0, 0.03, 0.02}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{cfg}};

  EXPECT_DOUBLE_EQ(mkt.stats(inst(10)).adv, 1000.0);
  EXPECT_DOUBLE_EQ(mkt.stats(inst(10)).sigma, 0.02);
  EXPECT_DOUBLE_EQ(mkt.stats(inst(10)).spread, 0.01);
  EXPECT_DOUBLE_EQ(mkt.stats(inst(20)).adv, 2000.0);
  EXPECT_DOUBLE_EQ(mkt.stats(inst(20)).sigma, 0.03);
  EXPECT_DOUBLE_EQ(mkt.stats(inst(20)).spread, 0.02);
}

// =====================================================================
//  stats() — empty stats span ⇒ all-zero defaults.
// =====================================================================

TEST(Market, Stats_NoConfig_ReturnsZeroDefaults) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  EXPECT_DOUBLE_EQ(mkt.stats(inst(10)).adv, 0.0);
  EXPECT_DOUBLE_EQ(mkt.stats(inst(10)).sigma, 0.0);
  EXPECT_DOUBLE_EQ(mkt.stats(inst(10)).spread, 0.0);
}

// =====================================================================
//  A SliceRow whose id is outside the universe is ignored on update.
// =====================================================================

TEST(Market, UpdatePrices_RowOutsideUniverse_Ignored) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  std::array<SliceRow, 2> rows{row(10, 100, 50, 1000), row(99, 100, 999, 9999)};
  mkt.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{rows}});

  EXPECT_DOUBLE_EQ(mkt.mark(inst(10)), 50.0);
}

// =====================================================================
//  Death tests — out-of-universe id is a programmer error (ATX_ASSERT).
// =====================================================================

TEST(MarketDeathTest, Mark_IdNotInUniverse_Aborts) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  EXPECT_DEATH({ (void)mkt.mark(inst(77)); }, "");
}

TEST(MarketDeathTest, ShiftMark_IdNotInUniverse_Aborts) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  EXPECT_DEATH({ mkt.shift_mark(inst(77), 1.0); }, "");
}

} // namespace
