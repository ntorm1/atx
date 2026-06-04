// atx::engine::loop — RollingPanel point-in-time panel tests (P2-2).
//
// Covers the plan's Tests list verbatim:
//   * append + read trailing window (newest-first ordering, value coherence);
//   * PIT proof — a not-yet-appended (future) bar is invisible;
//   * eviction past max_lookback (oldest dropped) + wrap-around at Cap;
//   * column-major / cross-section coherence (one row's instruments contiguous,
//     read back consistently across fields);
//   * NaN for a missing symbol (universe member absent from a slice);
//   * membership excludes a delisted instrument after its final bar (it simply
//     stops appearing in later slices);
//   * boundaries: max_lookback == 1; single instrument; universe larger than
//     Cap columns; all-NaN date;
//   * construction-time pow2 static_assert (documented, compile-time);
//   * EXPECT_DEATH on an out-of-bounds view() access (ATX_ASSERT precondition).
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>
#include <type_traits>

#include "atx/core/datetime.hpp"      // Timestamp
#include "atx/core/domain/domain.hpp" // Bar, Price, Quantity
#include "atx/core/domain/symbol.hpp" // Symbol
#include "atx/core/types.hpp"         // usize, u32

#include "atx/engine/loop/panel_types.hpp"   // MarketSlice, SliceRow, PanelView
#include "atx/engine/loop/rolling_panel.hpp" // RollingPanel<Cap>
#include "atx/engine/loop/types.hpp"         // InstrumentId

namespace {

using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::core::time::Timestamp;
using atx::engine::InstrumentId;
using atx::engine::MarketSlice;
using atx::engine::PanelView;
using atx::engine::RollingPanel;
using atx::engine::SliceRow;

// ---- helpers --------------------------------------------------------------

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }
[[nodiscard]] Timestamp ts(atx::i64 nanos) noexcept { return Timestamp::from_unix_nanos(nanos); }

// Build a Bar with whole-unit OHLCV from doubles-as-ints (exact under Decimal).
[[nodiscard]] Bar bar(atx::i64 t, atx::i64 o, atx::i64 h, atx::i64 l, atx::i64 c,
                      atx::i64 v) noexcept {
  return Bar{ts(t),
             Price::from_int(o),
             Price::from_int(h),
             Price::from_int(l),
             Price::from_int(c),
             Quantity::from_int(v)};
}

[[nodiscard]] SliceRow row(atx::u32 id, atx::i64 t, atx::i64 o, atx::i64 h, atx::i64 l, atx::i64 c,
                           atx::i64 v, bool delisted = false) noexcept {
  return SliceRow{inst(id), bar(t, o, h, l, c, v), delisted};
}

// =====================================================================
//  Compile-time guarantee — Cap must be a power of two.
//
//  The static_assert inside RollingPanel<Cap> rejects a non-pow2 Cap at
//  instantiation. We cannot instantiate RollingPanel<3> in a passing test (it
//  would be a hard compile error), so the contract is asserted positively: the
//  pow2 capacities used throughout this suite instantiate cleanly.
// =====================================================================
static_assert(std::is_class_v<RollingPanel<1>>, "Cap=1 (pow2) instantiates");
static_assert(std::is_class_v<RollingPanel<8>>, "Cap=8 (pow2) instantiates");

// =====================================================================
//  Append + read the trailing window (newest-first).
// =====================================================================

TEST(RollingPanel, AppendThenView_TwoRows_NewestFirstOrdering) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, /*max_lookback=*/4};

  std::array<SliceRow, 2> r0{row(10, 100, 1, 1, 1, 10, 100), row(20, 100, 2, 2, 2, 20, 200)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r0}});

  std::array<SliceRow, 2> r1{row(10, 200, 3, 3, 3, 30, 300), row(20, 200, 4, 4, 4, 40, 400)};
  panel.append_sealed_row(MarketSlice{ts(200), std::span<const SliceRow>{r1}});

  const PanelView view = panel.view();
  EXPECT_EQ(view.rows(), 2U);
  EXPECT_EQ(view.instruments(), 2U);

  // row 0 = newest (t=200): inst 10 close=30, inst 20 close=40.
  EXPECT_DOUBLE_EQ(view.close(0, 0), 30.0);
  EXPECT_DOUBLE_EQ(view.close(0, 1), 40.0);
  // row 1 = older (t=100): inst 10 close=10, inst 20 close=20.
  EXPECT_DOUBLE_EQ(view.close(1, 0), 10.0);
  EXPECT_DOUBLE_EQ(view.close(1, 1), 20.0);
}

TEST(RollingPanel, View_AllFields_RoundTripExactly) {
  std::array<InstrumentId, 1> uni{inst(7)};
  RollingPanel<4> panel{std::span<const InstrumentId>{uni}, 2};

  std::array<SliceRow, 1> r0{row(7, 50, 11, 22, 5, 17, 999)};
  panel.append_sealed_row(MarketSlice{ts(50), std::span<const SliceRow>{r0}});

  const PanelView view = panel.view();
  EXPECT_DOUBLE_EQ(view.open(0, 0), 11.0);
  EXPECT_DOUBLE_EQ(view.high(0, 0), 22.0);
  EXPECT_DOUBLE_EQ(view.low(0, 0), 5.0);
  EXPECT_DOUBLE_EQ(view.close(0, 0), 17.0);
  EXPECT_DOUBLE_EQ(view.volume(0, 0), 999.0);
  EXPECT_TRUE(view.present(0, 0));
}

// =====================================================================
//  Point-in-time proof — an un-appended (future) bar is invisible.
// =====================================================================

TEST(RollingPanel, View_FutureBarNotAppended_Invisible) {
  std::array<InstrumentId, 1> uni{inst(1)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 4};

  std::array<SliceRow, 1> r0{row(1, 100, 1, 1, 1, 10, 100)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r0}});

  // Only the t=100 row was sealed; the t=200 bar is in-progress / future and was
  // never appended. The panel structurally cannot see it.
  const PanelView before = panel.view();
  EXPECT_EQ(before.rows(), 1U);
  EXPECT_DOUBLE_EQ(before.close(0, 0), 10.0);

  // After sealing t=200 the new newest row appears; the prior row shifts to 1.
  std::array<SliceRow, 1> r1{row(1, 200, 2, 2, 2, 20, 200)};
  panel.append_sealed_row(MarketSlice{ts(200), std::span<const SliceRow>{r1}});
  const PanelView after = panel.view();
  EXPECT_EQ(after.rows(), 2U);
  EXPECT_DOUBLE_EQ(after.close(0, 0), 20.0); // newest is now t=200
  EXPECT_DOUBLE_EQ(after.close(1, 0), 10.0); // t=100 demoted to row 1
}

// =====================================================================
//  Eviction past max_lookback — view() exposes only the trailing window.
// =====================================================================

TEST(RollingPanel, View_MoreRowsThanLookback_OldestDropped) {
  std::array<InstrumentId, 1> uni{inst(1)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, /*max_lookback=*/3};

  for (atx::i64 k = 0; k < 5; ++k) {
    std::array<SliceRow, 1> r{row(1, 100 + k, 1, 1, 1, k, 100)};
    panel.append_sealed_row(MarketSlice{ts(100 + k), std::span<const SliceRow>{r}});
  }

  // 5 rows appended, max_lookback=3 → only the 3 newest (close 4,3,2) visible.
  const PanelView view = panel.view();
  EXPECT_EQ(view.rows(), 3U);
  EXPECT_DOUBLE_EQ(view.close(0, 0), 4.0);
  EXPECT_DOUBLE_EQ(view.close(1, 0), 3.0);
  EXPECT_DOUBLE_EQ(view.close(2, 0), 2.0);
}

// =====================================================================
//  Wrap-around at Cap — appending more than Cap rows must stay correct.
// =====================================================================

TEST(RollingPanel, View_AppendBeyondCap_WrapsCorrectly) {
  std::array<InstrumentId, 1> uni{inst(1)};
  // Cap=4, max_lookback=4: append 10 rows → ring wraps twice.
  RollingPanel<4> panel{std::span<const InstrumentId>{uni}, 4};

  for (atx::i64 k = 0; k < 10; ++k) {
    std::array<SliceRow, 1> r{row(1, 100 + k, 1, 1, 1, k, 100)};
    panel.append_sealed_row(MarketSlice{ts(100 + k), std::span<const SliceRow>{r}});
  }

  // Last 4 appended closes are 9,8,7,6 (newest-first).
  const PanelView view = panel.view();
  EXPECT_EQ(view.rows(), 4U);
  EXPECT_DOUBLE_EQ(view.close(0, 0), 9.0);
  EXPECT_DOUBLE_EQ(view.close(1, 0), 8.0);
  EXPECT_DOUBLE_EQ(view.close(2, 0), 7.0);
  EXPECT_DOUBLE_EQ(view.close(3, 0), 6.0);
}

// =====================================================================
//  Column-major / cross-section coherence — one row's instruments read back
//  consistently and are distinct per column.
// =====================================================================

TEST(RollingPanel, View_CrossSection_ColumnsAreInstrumentCoherent) {
  std::array<InstrumentId, 3> uni{inst(1), inst(2), inst(3)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 4};

  // One slice; each instrument's close encodes its id so a column swap is caught.
  std::array<SliceRow, 3> r{row(1, 100, 1, 1, 1, 111, 10), row(2, 100, 2, 2, 2, 222, 20),
                            row(3, 100, 3, 3, 3, 333, 30)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  EXPECT_EQ(view.instruments(), 3U);
  EXPECT_DOUBLE_EQ(view.close(0, 0), 111.0);
  EXPECT_DOUBLE_EQ(view.close(0, 1), 222.0);
  EXPECT_DOUBLE_EQ(view.close(0, 2), 333.0);
  // Universe order is preserved.
  const std::span<const InstrumentId> u = view.universe();
  ASSERT_EQ(u.size(), 3U);
  EXPECT_EQ(u[0], inst(1));
  EXPECT_EQ(u[1], inst(2));
  EXPECT_EQ(u[2], inst(3));
}

TEST(RollingPanel, AppendRowOutOfUniverseOrder_MapsToCorrectColumn) {
  std::array<InstrumentId, 3> uni{inst(10), inst(20), inst(30)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 2};

  // Slice rows in a DIFFERENT order than the universe — must still land in the
  // column the universe dictates (column = universe index, not slice index).
  std::array<SliceRow, 3> r{row(30, 100, 3, 3, 3, 333, 30), row(10, 100, 1, 1, 1, 111, 10),
                            row(20, 100, 2, 2, 2, 222, 20)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  EXPECT_DOUBLE_EQ(view.close(0, 0), 111.0); // col 0 == inst 10
  EXPECT_DOUBLE_EQ(view.close(0, 1), 222.0); // col 1 == inst 20
  EXPECT_DOUBLE_EQ(view.close(0, 2), 333.0); // col 2 == inst 30
}

// =====================================================================
//  NaN for a missing symbol — universe member absent from a slice.
// =====================================================================

TEST(RollingPanel, View_MissingSymbol_ReadsNaNAndNotPresent) {
  std::array<InstrumentId, 2> uni{inst(1), inst(2)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 2};

  // Only inst 1 trades this slice; inst 2 is absent → NaN, present=false.
  std::array<SliceRow, 1> r{row(1, 100, 1, 1, 1, 10, 100)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  EXPECT_TRUE(view.present(0, 0));
  EXPECT_DOUBLE_EQ(view.close(0, 0), 10.0);
  EXPECT_FALSE(view.present(0, 1));
  EXPECT_TRUE(std::isnan(view.close(0, 1)));
  EXPECT_TRUE(std::isnan(view.open(0, 1)));
  EXPECT_TRUE(std::isnan(view.volume(0, 1)));
}

TEST(RollingPanel, View_AllNaNDate_RowPresentButAllAbsent) {
  std::array<InstrumentId, 2> uni{inst(1), inst(2)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 2};

  // A sealed slice with NO rows — the date is complete but empty (e.g. a
  // session with no prints for any universe member). The row still exists.
  std::array<SliceRow, 0> empty{};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{empty}});

  const PanelView view = panel.view();
  EXPECT_EQ(view.rows(), 1U);
  EXPECT_FALSE(view.present(0, 0));
  EXPECT_FALSE(view.present(0, 1));
  EXPECT_TRUE(std::isnan(view.close(0, 0)));
  EXPECT_TRUE(std::isnan(view.close(0, 1)));
}

// =====================================================================
//  Membership — a delisted instrument disappears after its final bar.
// =====================================================================

TEST(RollingPanel, Membership_DelistedAfterFinalBar_AbsentInLaterRows) {
  std::array<InstrumentId, 2> uni{inst(1), inst(2)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 4};

  // Slice 0: both trade; inst 2's bar is its FINAL bar (delisted_final=true).
  std::array<SliceRow, 2> r0{row(1, 100, 1, 1, 1, 10, 100),
                             row(2, 100, 2, 2, 2, 20, 200, /*delisted=*/true)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r0}});

  // Slice 1: only inst 1 (the feed emits nothing further for delisted inst 2).
  std::array<SliceRow, 1> r1{row(1, 200, 3, 3, 3, 30, 300)};
  panel.append_sealed_row(MarketSlice{ts(200), std::span<const SliceRow>{r1}});

  const PanelView view = panel.view();
  // Row 0 (t=200): inst 2 is gone — absent, NaN.
  EXPECT_TRUE(view.present(0, 0));
  EXPECT_FALSE(view.present(0, 1));
  EXPECT_TRUE(std::isnan(view.close(0, 1)));
  // Row 1 (t=100): inst 2 STILL present on its final bar (it traded that day) —
  // history is not retroactively erased.
  EXPECT_TRUE(view.present(1, 1));
  EXPECT_DOUBLE_EQ(view.close(1, 1), 20.0);
}

// =====================================================================
//  Universe rows whose id is not in the universe are ignored (PIT membership).
// =====================================================================

TEST(RollingPanel, Append_RowOutsideUniverse_IgnoredNoWrite) {
  std::array<InstrumentId, 1> uni{inst(1)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 2};

  // inst 99 is not in the universe → silently ignored; inst 1 written normally.
  std::array<SliceRow, 2> r{row(1, 100, 1, 1, 1, 10, 100), row(99, 100, 9, 9, 9, 90, 900)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  EXPECT_EQ(view.instruments(), 1U);
  EXPECT_TRUE(view.present(0, 0));
  EXPECT_DOUBLE_EQ(view.close(0, 0), 10.0);
}

// =====================================================================
//  Boundaries.
// =====================================================================

TEST(RollingPanel, MaxLookbackOne_OnlyNewestVisible) {
  std::array<InstrumentId, 1> uni{inst(1)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, /*max_lookback=*/1};

  for (atx::i64 k = 0; k < 4; ++k) {
    std::array<SliceRow, 1> r{row(1, 100 + k, 1, 1, 1, k, 100)};
    panel.append_sealed_row(MarketSlice{ts(100 + k), std::span<const SliceRow>{r}});
  }

  const PanelView view = panel.view();
  EXPECT_EQ(view.rows(), 1U);
  EXPECT_DOUBLE_EQ(view.close(0, 0), 3.0); // only the very newest
}

TEST(RollingPanel, SingleInstrument_BasicAppendRead) {
  std::array<InstrumentId, 1> uni{inst(42)};
  RollingPanel<2> panel{std::span<const InstrumentId>{uni}, 2};

  std::array<SliceRow, 1> r{row(42, 100, 1, 2, 3, 4, 5)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  EXPECT_EQ(view.instruments(), 1U);
  EXPECT_EQ(view.rows(), 1U);
  EXPECT_DOUBLE_EQ(view.close(0, 0), 4.0);
}

TEST(RollingPanel, UniverseLargerThanCap_ManyColumnsFewRows) {
  // Cap (rows) = 2 but the universe has 5 instruments (columns >> Cap). Columns
  // and rows are independent dimensions; a wide universe over a shallow ring is
  // legal and must read back correctly.
  std::array<InstrumentId, 5> uni{inst(1), inst(2), inst(3), inst(4), inst(5)};
  RollingPanel<2> panel{std::span<const InstrumentId>{uni}, 2};

  std::array<SliceRow, 5> r{row(1, 100, 1, 1, 1, 10, 100), row(2, 100, 2, 2, 2, 20, 200),
                            row(3, 100, 3, 3, 3, 30, 300), row(4, 100, 4, 4, 4, 40, 400),
                            row(5, 100, 5, 5, 5, 50, 500)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  EXPECT_EQ(view.instruments(), 5U);
  EXPECT_EQ(view.rows(), 1U);
  for (atx::usize i = 0; i < 5; ++i) {
    EXPECT_TRUE(view.present(0, i));
    EXPECT_DOUBLE_EQ(view.close(0, i), static_cast<double>((i + 1) * 10));
  }
}

TEST(RollingPanel, View_NoRowsAppended_EmptyView) {
  std::array<InstrumentId, 2> uni{inst(1), inst(2)};
  const RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 4};

  const PanelView view = panel.view();
  EXPECT_EQ(view.rows(), 0U);
  EXPECT_EQ(view.instruments(), 2U);
}

// =====================================================================
//  Bounds — out-of-range view() access aborts (ATX_ASSERT precondition).
// =====================================================================

TEST(RollingPanelDeathTest, View_RowOutOfRange_Aborts) {
  std::array<InstrumentId, 1> uni{inst(1)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 4};
  std::array<SliceRow, 1> r{row(1, 100, 1, 1, 1, 10, 100)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  // Only 1 valid row exists; row index 1 is past the trailing window.
  EXPECT_DEATH({ (void)view.close(1, 0); }, ".*");
}

TEST(RollingPanelDeathTest, View_InstrumentOutOfRange_Aborts) {
  std::array<InstrumentId, 1> uni{inst(1)};
  RollingPanel<8> panel{std::span<const InstrumentId>{uni}, 4};
  std::array<SliceRow, 1> r{row(1, 100, 1, 1, 1, 10, 100)};
  panel.append_sealed_row(MarketSlice{ts(100), std::span<const SliceRow>{r}});

  const PanelView view = panel.view();
  // Universe has 1 instrument; column index 1 is out of range.
  EXPECT_DEATH({ (void)view.close(0, 1); }, ".*");
}

} // namespace
