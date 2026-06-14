// Tests for align_onto (S6.3) — the PIT alignment rail. TDD suite.
//
// Suite: DataAlign
// All test names match the spec verbatim so `ctest -R DataAlign` picks them up.

#include "atx/engine/data/align.hpp"

#include <cmath>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace {

using namespace atx::engine::data;
using atx::f64;
using atx::usize;

// ---------------------------------------------------------------------------
//  Test-fixture helpers
// ---------------------------------------------------------------------------

// Single-column schema named "val".
DatasetSchema make_schema(Role role = Role::Feature) {
  DatasetSchema s;
  s.columns = {"val"};
  s.dtypes = {ColumnDType::F64};
  s.role = role;
  s.region = "US";
  s.universe_tag = "test";
  return s;
}

// Build a single-column Dataset over the given dates × instruments. `col` is
// date-major (length dates*instruments). No mask (all in-universe).
Dataset make_ds(std::vector<DateKey> dates, std::vector<InstKey> instruments, std::vector<f64> col,
                Role role = Role::Feature) {
  auto res = Dataset::create(make_schema(role), std::move(dates), std::move(instruments),
                             {std::move(col)}, {}, {"test", ""});
  return std::move(res).value();
}

// Index of the single column's aligned cell at (date_idx, inst_idx).
f64 cell(const AlignedView &v, usize d, usize i) {
  return v.aligned_columns[0][(d * v.num_instruments) + i];
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

// A canonical (date, inst) the plug does not cover → aligned cell is NaN,
// never imputed/zero. Two flavours: instrument absent, and no plug row ≤ date.
TEST(DataAlign, MissingCellsBecomeNaN) {
  // Canonical axis: dates {1,2}, instruments {10,20}.
  Dataset canonical = make_ds({1, 2}, {10, 20}, {0, 0, 0, 0}, Role::Price);

  // Plug covers only instrument 10, and only starting at date 2.
  Dataset plug = make_ds({2}, {10}, {7.5});

  auto res = align_onto(canonical, plug);
  ASSERT_TRUE(res.has_value());
  const AlignedView &v = res.value();

  // inst 20 absent from plug → NaN at both dates.
  EXPECT_TRUE(std::isnan(cell(v, 0, 1)));
  EXPECT_TRUE(std::isnan(cell(v, 1, 1)));
  // inst 10 present but no plug row ≤ date 1 → NaN; date 2 covered → real.
  EXPECT_TRUE(std::isnan(cell(v, 0, 0)));
  EXPECT_FALSE(std::isnan(cell(v, 1, 0)));
  EXPECT_DOUBLE_EQ(cell(v, 1, 0), 7.5);
}

// Plug carries an instrument outside the canonical universe → those positions
// land in no aligned cell AND extra_instrument_cells == plug.num_dates() per
// extra instrument.
TEST(DataAlign, ExtraInstrumentsDroppedAndCounted) {
  Dataset canonical = make_ds({1, 2}, {10}, {0, 0}, Role::Price);

  // Plug instruments {10, 99}; 99 is NOT in the canonical universe.
  // date-major over 2 dates × 2 insts: [d1i10,d1i99, d2i10,d2i99].
  Dataset plug = make_ds({1, 2}, {10, 99}, {1.0, 100.0, 2.0, 200.0});

  auto res = align_onto(canonical, plug);
  ASSERT_TRUE(res.has_value());
  const AlignedView &v = res.value();

  // Canonical inst 10 carries the real plug values; 99 lands nowhere.
  EXPECT_DOUBLE_EQ(cell(v, 0, 0), 1.0);
  EXPECT_DOUBLE_EQ(cell(v, 1, 0), 2.0);

  // 1 extra instrument × 2 plug dates = 2 dropped positions.
  EXPECT_EQ(v.drops.extra_instrument_cells, usize{2});
  EXPECT_EQ(v.drops.extra_date_cells, usize{0});
  EXPECT_EQ(v.drops.total(), usize{2});
}

// Plug instrument has rows up to date X then none later; a canonical date > X
// carries the FINAL value forward (no survivorship; not NaN).
TEST(DataAlign, DelistedFinalValueSurvives) {
  // Canonical dates {1,2,3}; plug last row at date 2 (delists after).
  Dataset canonical = make_ds({1, 2, 3}, {10}, {0, 0, 0}, Role::Price);
  Dataset plug = make_ds({1, 2}, {10}, {11.0, 22.0});

  auto res = align_onto(canonical, plug);
  ASSERT_TRUE(res.has_value());
  const AlignedView &v = res.value();

  EXPECT_DOUBLE_EQ(cell(v, 0, 0), 11.0); // date 1
  EXPECT_DOUBLE_EQ(cell(v, 1, 0), 22.0); // date 2
  // date 3 > last plug date 2 → final value 22.0 survives, not NaN.
  EXPECT_FALSE(std::isnan(cell(v, 2, 0)));
  EXPECT_DOUBLE_EQ(cell(v, 2, 0), 22.0);
}

// Appending a later-dated (restatement/future) plug row never changes an
// earlier aligned cell — the no-look-ahead pin. Assert byte-identical.
TEST(DataAlign, AsOfAlignmentIsTruncationInvariant) {
  // Canonical date 5 reads "as of 5" from the plug.
  Dataset canonical = make_ds({5}, {10}, {0.0}, Role::Price);

  // Plug A: rows at dates {3,4}. As-of 5 → row at 4 → value 44.0.
  Dataset plug_a = make_ds({3, 4}, {10}, {33.0, 44.0});
  auto res_a = align_onto(canonical, plug_a);
  ASSERT_TRUE(res_a.has_value());
  const f64 early = cell(res_a.value(), 0, 0);
  EXPECT_DOUBLE_EQ(early, 44.0);

  // Plug B: same data PLUS a later row at date 9 (future restatement).
  Dataset plug_b = make_ds({3, 4, 9}, {10}, {33.0, 44.0, 999.0});
  auto res_b = align_onto(canonical, plug_b);
  ASSERT_TRUE(res_b.has_value());
  const f64 early_b = cell(res_b.value(), 0, 0);

  // Byte-identical: the future row is invisible at canonical date 5.
  EXPECT_EQ(std::memcmp(&early, &early_b, sizeof(f64)), 0);

  // extra_date_cells differs (a report, not data): plug_b's date-9 row is
  // dropped as a future row (9 > max canonical date 5).
  EXPECT_EQ(res_a.value().drops.extra_date_cells, usize{0});
  EXPECT_EQ(res_b.value().drops.extra_date_cells, usize{1});
}

// Match is by InstKey, not by position. Canonical {10,20,30}; plug in a
// different order / subset {30,10} → values land under the correct InstKey.
TEST(DataAlign, KeyMatchIsByIdNotPosition) {
  Dataset canonical = make_ds({1}, {10, 20, 30}, {0, 0, 0}, Role::Price);

  // Plug instruments {30, 10}, single date. date-major: [i30, i10].
  Dataset plug = make_ds({1}, {30, 10}, {300.0, 100.0});

  auto res = align_onto(canonical, plug);
  ASSERT_TRUE(res.has_value());
  const AlignedView &v = res.value();

  EXPECT_DOUBLE_EQ(cell(v, 0, 0), 100.0); // canonical inst 10
  EXPECT_TRUE(std::isnan(cell(v, 0, 1))); // canonical inst 20 not in plug
  EXPECT_DOUBLE_EQ(cell(v, 0, 2), 300.0); // canonical inst 30

  // Nothing dropped: both plug instruments are in the canonical universe.
  EXPECT_EQ(v.drops.total(), usize{0});
}

// Non-ascending dates (plug or canonical) → align_onto returns is_err().
TEST(DataAlign, RejectsNonAscendingDates) {
  Dataset good = make_ds({1, 2}, {10}, {0, 0}, Role::Price);

  // Plug with non-ascending dates {2,1}.
  Dataset bad_plug = make_ds({2, 1}, {10}, {1.0, 2.0});
  EXPECT_FALSE(align_onto(good, bad_plug).has_value());

  // Canonical with non-ascending dates {3,1}.
  Dataset bad_canonical = make_ds({3, 1}, {10}, {0, 0}, Role::Price);
  Dataset ok_plug = make_ds({1}, {10}, {1.0});
  EXPECT_FALSE(align_onto(bad_canonical, ok_plug).has_value());
}

// Boundary: a single-column plug covering only some instruments → covered
// cells real, uncovered NaN, drop counts correct.
TEST(DataAlign, EmptyPlugColumnAllNaNWhereUncovered) {
  // Canonical {10,20,30} over dates {1,2}.
  Dataset canonical = make_ds({1, 2}, {10, 20, 30}, {0, 0, 0, 0, 0, 0}, Role::Price);

  // Plug covers only {20}; values 2.0 (date1), 4.0 (date2).
  Dataset plug = make_ds({1, 2}, {20}, {2.0, 4.0});

  auto res = align_onto(canonical, plug);
  ASSERT_TRUE(res.has_value());
  const AlignedView &v = res.value();

  // inst 20 covered at both dates.
  EXPECT_DOUBLE_EQ(cell(v, 0, 1), 2.0);
  EXPECT_DOUBLE_EQ(cell(v, 1, 1), 4.0);
  // inst 10 and 30 uncovered → NaN everywhere.
  EXPECT_TRUE(std::isnan(cell(v, 0, 0)));
  EXPECT_TRUE(std::isnan(cell(v, 1, 0)));
  EXPECT_TRUE(std::isnan(cell(v, 0, 2)));
  EXPECT_TRUE(std::isnan(cell(v, 1, 2)));

  // Plug's only instrument (20) IS canonical → no drops.
  EXPECT_EQ(v.drops.total(), usize{0});
}

} // namespace
