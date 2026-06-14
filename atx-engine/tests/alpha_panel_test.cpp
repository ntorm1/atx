// atx::engine::alpha — Panel / SlotPool / SignalSet unit tests (P3-5).
//
// Covers the plan's list:
//   * Panel: field cross-section access at a date; whole-field over time;
//     out-of-universe cell membership; field_id of unknown -> Err; ragged
//     create -> Err; 1x1 boundary; empty-universe convenience -> all valid.
//   * SlotPool: acquire/release round-trips; column() spans distinct & writable;
//     over-acquire trips ATX_ASSERT (EXPECT_DEATH).
//   * SignalSet: alpha_cross_section slicing.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atxtest_alpha_panel_test {

using atx::core::ErrorCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::SignalSet;
using atx::engine::alpha::SlotPool;

// A 2-date x 3-instrument Panel with one field "close" and an all-valid universe.
// close (date-major): d0 = [1,2,3], d1 = [4,5,6].
[[nodiscard]] Panel make_panel() {
  std::vector<std::vector<atx::f64>> data{{1.0, 2.0, 3.0, 4.0, 5.0, 6.0}};
  auto res = Panel::create(2, 3, {"close"}, data, {});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

// ---- Panel construction ------------------------------------------------------

TEST(AlphaPanel_Create, AllValidUniverseEmpty_EveryCellInUniverse) {
  const Panel p = make_panel();
  EXPECT_EQ(p.dates(), 2U);
  EXPECT_EQ(p.instruments(), 3U);
  for (atx::usize d = 0; d < 2; ++d) {
    for (atx::usize j = 0; j < 3; ++j) {
      EXPECT_TRUE(p.in_universe(d, j));
    }
  }
}

TEST(AlphaPanel_Create, FieldCountMismatch_ReturnsErr) {
  std::vector<std::vector<atx::f64>> data{{1.0, 2.0}};
  auto res = Panel::create(1, 2, {"a", "b"}, data, {}); // 2 names, 1 column
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(AlphaPanel_Create, RaggedColumn_ReturnsErr) {
  std::vector<std::vector<atx::f64>> data{{1.0, 2.0, 3.0}}; // 3 cells, need 1*2=2
  auto res = Panel::create(1, 2, {"close"}, data, {});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(AlphaPanel_Create, UniverseWrongSize_ReturnsErr) {
  std::vector<std::vector<atx::f64>> data{{1.0, 2.0}};
  auto res = Panel::create(1, 2, {"close"}, data, std::vector<std::uint8_t>{1, 0, 1});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ---- Panel access ------------------------------------------------------------

TEST(AlphaPanel_FieldCrossSection, KnownDate_ReturnsRow) {
  const Panel p = make_panel();
  const auto fid = p.field_id("close");
  ASSERT_TRUE(fid.has_value());
  const auto row1 = p.field_cross_section(fid.value(), 1); // d1 = [4,5,6]
  ASSERT_EQ(row1.size(), 3U);
  EXPECT_DOUBLE_EQ(row1[0], 4.0);
  EXPECT_DOUBLE_EQ(row1[1], 5.0);
  EXPECT_DOUBLE_EQ(row1[2], 6.0);
}

TEST(AlphaPanel_FieldAll, WholeField_IsDateMajor) {
  const Panel p = make_panel();
  const auto all = p.field_all(p.field_id("close").value());
  ASSERT_EQ(all.size(), 6U);
  EXPECT_DOUBLE_EQ(all[0], 1.0); // (d0, inst0)
  EXPECT_DOUBLE_EQ(all[3], 4.0); // (d1, inst0)
  EXPECT_DOUBLE_EQ(all[5], 6.0); // (d1, inst2)
}

TEST(AlphaPanel_FieldId, UnknownName_ReturnsNotFound) {
  const Panel p = make_panel();
  auto res = p.field_id("nope");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
}

TEST(AlphaPanel_Universe, OutOfUniverseCell_ReadsFalse) {
  std::vector<std::vector<atx::f64>> data{{1.0, 2.0, 3.0, 4.0}};
  std::vector<std::uint8_t> uni{1, 0, 1, 1}; // (d0,inst1) masked out
  auto res = Panel::create(2, 2, {"close"}, data, uni);
  ASSERT_TRUE(res.has_value());
  const Panel p = std::move(res).value();
  EXPECT_TRUE(p.in_universe(0, 0));
  EXPECT_FALSE(p.in_universe(0, 1));
  EXPECT_TRUE(p.in_universe(1, 0));
}

// ---- Panel boundary: 1x1 ----------------------------------------------------

TEST(AlphaPanel_Boundary, OneByOne_SingleCell) {
  std::vector<std::vector<atx::f64>> data{{42.0}};
  auto res = Panel::create(1, 1, {"x"}, data, {});
  ASSERT_TRUE(res.has_value());
  const Panel p = std::move(res).value();
  EXPECT_EQ(p.cells(), 1U);
  const auto cs = p.field_cross_section(p.field_id("x").value(), 0);
  ASSERT_EQ(cs.size(), 1U);
  EXPECT_DOUBLE_EQ(cs[0], 42.0);
}

TEST(AlphaPanel_Boundary, ZeroFields_AllowedShapeOnly) {
  auto res = Panel::create(3, 2, {}, {}, {});
  ASSERT_TRUE(res.has_value());
  const Panel p = std::move(res).value();
  EXPECT_EQ(p.num_fields(), 0U);
  EXPECT_EQ(p.cells(), 6U);
}

// ---- SlotPool ---------------------------------------------------------------

TEST(AlphaSlotPool_Acquire, RoundTrip_RecyclesCapacity) {
  SlotPool pool{2, 4};
  EXPECT_EQ(pool.live(), 0U);
  const auto a = pool.acquire();
  const auto b = pool.acquire();
  EXPECT_EQ(pool.live(), 2U);
  pool.release(b);
  pool.release(a);
  EXPECT_EQ(pool.live(), 0U);
  // After release the pool can be re-acquired without tripping over-acquire.
  (void)pool.acquire();
  (void)pool.acquire();
  EXPECT_EQ(pool.live(), 2U);
}

TEST(AlphaSlotPool_Column, DistinctSlots_AreIndependentlyWritable) {
  SlotPool pool{2, 3};
  const auto a = pool.acquire();
  const auto b = pool.acquire();
  ASSERT_NE(a, b);
  auto ca = pool.column(a);
  auto cb = pool.column(b);
  ASSERT_EQ(ca.size(), 3U);
  // Writes to one slot do not bleed into the other.
  ca[0] = 7.0;
  ca[1] = 8.0;
  cb[0] = -1.0;
  EXPECT_DOUBLE_EQ(pool.column(a)[0], 7.0);
  EXPECT_DOUBLE_EQ(pool.column(a)[1], 8.0);
  EXPECT_DOUBLE_EQ(pool.column(b)[0], -1.0);
  EXPECT_NE(pool.column(a).data(), pool.column(b).data());
}

TEST(AlphaSlotPool_Cells, ReportsConfiguredGeometry) {
  const SlotPool pool{5, 12};
  EXPECT_EQ(pool.capacity(), 5U);
  EXPECT_EQ(pool.cells_per_slot(), 12U);
}

// Over-acquire is a precondition failure (ATX_ASSERT -> abort) in debug builds.
// NOTE: the death-statement is wrapped in a helper because the preprocessor
// does NOT treat `{...}` as protecting commas — a brace-init like `pool{1, 2}`
// inside EXPECT_DEATH(...) would be split into extra macro arguments.
void over_acquire() {
  SlotPool pool{1, 2};
  (void)pool.acquire();
  (void)pool.acquire(); // second acquire exceeds capacity 1
}

TEST(AlphaSlotPoolDeathTest, OverAcquire_Aborts) { EXPECT_DEATH(over_acquire(), ".*"); }

// ---- SignalSet --------------------------------------------------------------

TEST(AlphaSignalSet_CrossSection, SlicesAlphaRowByDate) {
  SignalSet s;
  s.dates = 2;
  s.instruments = 2;
  s.alphas.push_back(SignalSet::Alpha{"a", {10.0, 11.0, 20.0, 21.0}});
  const auto row1 = s.alpha_cross_section(0, 1); // d1 = [20,21]
  ASSERT_EQ(row1.size(), 2U);
  EXPECT_DOUBLE_EQ(row1[0], 20.0);
  EXPECT_DOUBLE_EQ(row1[1], 21.0);
}

// ---- Borrowed Panel ---------------------------------------------------------

TEST(AlphaPanel_Borrowed_MatchesOwned, BorrowedReadSurface) {
  // Two fields, 2 dates x 3 instruments, date-major columns.
  const std::vector<atx::f64> close{1, 2, 3, 4, 5, 6}; // d0=[1,2,3] d1=[4,5,6]
  const std::vector<atx::f64> volume{10, 20, 30, 40, 50, 60};
  const std::vector<std::uint8_t> uni{1, 0, 1, 1, 1, 0};

  // Borrowed: column spans alias the vectors above (which outlive the panel).
  std::vector<std::span<const atx::f64>> cols{std::span<const atx::f64>{close},
                                              std::span<const atx::f64>{volume}};
  auto bp = Panel::create_borrowed(2, 3, {"close", "volume"}, std::move(cols), uni);
  ASSERT_TRUE(bp.has_value()) << (bp ? "" : bp.error().message());
  const Panel &p = bp.value();

  EXPECT_EQ(p.dates(), 2U);
  EXPECT_EQ(p.instruments(), 3U);
  EXPECT_EQ(p.num_fields(), 2U);
  const auto cid = p.field_id("close");
  ASSERT_TRUE(cid.has_value());
  const auto cs1 = p.field_cross_section(cid.value(), 1); // d1 = [4,5,6]
  ASSERT_EQ(cs1.size(), 3U);
  EXPECT_DOUBLE_EQ(cs1[0], 4.0);
  EXPECT_DOUBLE_EQ(cs1[2], 6.0);
  EXPECT_DOUBLE_EQ(p.field_all(cid.value())[0], 1.0);
  EXPECT_TRUE(p.in_universe(0, 0));
  EXPECT_FALSE(p.in_universe(0, 1));
}

TEST(AlphaPanel_Borrowed_RaggedColumn_Errs, BorrowedValidation) {
  const std::vector<atx::f64> good{1, 2};
  const std::vector<atx::f64> bad{1, 2, 3}; // wrong length for 1x2
  std::vector<std::span<const atx::f64>> cols{std::span<const atx::f64>{good},
                                              std::span<const atx::f64>{bad}};
  auto bp = Panel::create_borrowed(1, 2, {"a", "b"}, std::move(cols), {});
  EXPECT_FALSE(bp.has_value());
}

TEST(AlphaPanel_BorrowedSurvivesPanelMove, BorrowedMoveStable) {
  const std::vector<atx::f64> close{1, 2, 3, 4};
  std::vector<std::span<const atx::f64>> cols{std::span<const atx::f64>{close}};
  auto bp = Panel::create_borrowed(2, 2, {"close"}, std::move(cols), {});
  ASSERT_TRUE(bp.has_value());
  Panel moved = std::move(bp.value()); // borrowed spans point at `close`, unaffected
  EXPECT_DOUBLE_EQ(moved.field_all(moved.field_id("close").value())[3], 4.0);
}

TEST(AlphaPanel_OwnedCopy_RepointsSpans, OwnedCopySafe) {
  std::vector<std::vector<atx::f64>> data{{1, 2, 3, 4}}; // 2x2, one field
  auto src = Panel::create(2, 2, {"close"}, data, {});
  ASSERT_TRUE(src.has_value());
  Panel copy = src.value();                                      // exercises copy ctor
  src.value() = Panel::create(1, 1, {"x"}, {{9.0}}, {}).value(); // clobber source
  EXPECT_EQ(copy.dates(), 2U);
  EXPECT_EQ(copy.instruments(), 2U);
  EXPECT_DOUBLE_EQ(copy.field_all(copy.field_id("close").value())[3], 4.0);
}


}  // namespace atxtest_alpha_panel_test
