// atx::impl — R1 typed-fields cardinality classifier tests (suite AtxImplTypedFields).
//
// Tests the stage cardinality scan helper (detail::classify_typed_fields) as
// required by the R1 brief §Tests.4:
//   - A binary field (values {-1, 0, 1}) is classified as excluded at K=12.
//   - A small-count field (values {1..5}) is classified as excluded at K=12.
//   - A dense continuous field (many distinct values) is kept (NOT excluded).
//   - An is_group_field field (e.g. "sector") is skipped entirely (not touched).
//   - The "gics" backstop: excluded AND routed to extra_group.

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

#include "stage_discover_detail.hpp" // atx::impl::detail::classify_typed_fields

namespace atxtest_typed_fields {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;

// Build a minimal 1-instrument panel with the given single-field column.
[[nodiscard]] Panel make_single_field_panel(const std::string& fname,
                                             const std::vector<f64>& col) {
  const usize n = col.size();
  auto r = Panel::create(n, 1U, {fname}, {col}, {});
  EXPECT_TRUE(r.has_value()) << "single-field panel must build";
  return std::move(*r);
}

// Build a two-field panel: field0 + field1.
[[nodiscard]] Panel make_two_field_panel(const std::string& f0, const std::vector<f64>& c0,
                                          const std::string& f1, const std::vector<f64>& c1) {
  EXPECT_EQ(c0.size(), c1.size());
  const usize n = c0.size();
  auto r = Panel::create(n, 1U, {f0, f1}, {c0, c1}, {});
  EXPECT_TRUE(r.has_value()) << "two-field panel must build";
  return std::move(*r);
}

// =============================================================================
// Test 1 — Binary field {-1, +1} is excluded at K=12.
// =============================================================================
TEST(AtxImplTypedFields, BinaryFieldIsExcluded) {
  // 20 cells, only 2 distinct values: -1 and +1.
  std::vector<f64> col;
  for (int i = 0; i < 20; ++i) col.push_back((i % 2 == 0) ? 1.0 : -1.0);
  Panel panel = make_single_field_panel("earnFlag", col);
  const std::vector<std::string> fields = {"earnFlag"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  ASSERT_EQ(excl.size(), 1U) << "binary earnFlag must be excluded";
  EXPECT_EQ(excl[0], "earnFlag");
  EXPECT_TRUE(grp.empty()) << "earnFlag is not a group field";
}

// =============================================================================
// Test 2 — Small-count field (values 1..5) is excluded at K=12.
// =============================================================================
TEST(AtxImplTypedFields, SmallCountFieldIsExcluded) {
  std::vector<f64> col;
  for (int i = 0; i < 30; ++i) col.push_back(static_cast<f64>((i % 5) + 1));
  Panel panel = make_single_field_panel("nEarnCnt_5d", col);
  const std::vector<std::string> fields = {"nEarnCnt_5d"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  ASSERT_EQ(excl.size(), 1U) << "5-value nEarnCnt_5d must be excluded (cardinality 5 <= 12)";
  EXPECT_EQ(excl[0], "nEarnCnt_5d");
  EXPECT_TRUE(grp.empty());
}

// =============================================================================
// Test 3 — Dense continuous field (50 distinct values) is KEPT at K=12.
// =============================================================================
TEST(AtxImplTypedFields, DenseContinuousFieldIsKept) {
  std::vector<f64> col;
  for (int i = 0; i < 100; ++i) col.push_back(static_cast<f64>(i) * 0.01 + 1.0);
  Panel panel = make_single_field_panel("close", col);
  const std::vector<std::string> fields = {"close"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  EXPECT_TRUE(excl.empty()) << "dense 'close' must NOT be excluded";
  EXPECT_TRUE(grp.empty());
}

// =============================================================================
// Test 4 — Exactly K distinct values is EXCLUDED (boundary: <= K means exclude).
// =============================================================================
TEST(AtxImplTypedFields, ExactlyKDistinctValuesIsExcluded) {
  // 12 distinct values — exactly at K=12 boundary.
  std::vector<f64> col;
  for (int i = 0; i < 24; ++i) col.push_back(static_cast<f64>(i % 12));
  Panel panel = make_single_field_panel("myfield", col);
  const std::vector<std::string> fields = {"myfield"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  ASSERT_EQ(excl.size(), 1U) << "field with exactly K distinct values is excluded (<=K)";
  EXPECT_EQ(excl[0], "myfield");
}

// =============================================================================
// Test 5 — K+1 distinct values is KEPT (just above threshold).
// =============================================================================
TEST(AtxImplTypedFields, KPlusOneDistinctValuesIsKept) {
  // 13 distinct values at K=12 -> kept.
  std::vector<f64> col;
  for (int i = 0; i < 26; ++i) col.push_back(static_cast<f64>(i % 13));
  Panel panel = make_single_field_panel("myfield2", col);
  const std::vector<std::string> fields = {"myfield2"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  EXPECT_TRUE(excl.empty()) << "field with K+1 distinct values must NOT be excluded";
}

// =============================================================================
// Test 6 — "sector" (is_group_field) is SKIPPED; never appears in either list.
// =============================================================================
TEST(AtxImplTypedFields, GroupFieldIsSkipped) {
  // "sector" only has 3 distinct values — but is_group_field() catches it first.
  std::vector<f64> col;
  for (int i = 0; i < 30; ++i) col.push_back(static_cast<f64>(i % 3));
  Panel panel = make_single_field_panel("sector", col);
  const std::vector<std::string> fields = {"sector"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  EXPECT_TRUE(excl.empty()) << "'sector' is a group field and must be skipped by the scan";
  EXPECT_TRUE(grp.empty())  << "'sector' already handled by is_group_field; not in extra_group";
}

// =============================================================================
// Test 7 — "gics" backstop: excluded AND routed to extra_group.
// =============================================================================
TEST(AtxImplTypedFields, GicsBackstopExcludedAndRoutedToGroup) {
  // "gics" has many distinct values but is in the backstop list.
  std::vector<f64> col;
  for (int i = 0; i < 100; ++i) col.push_back(static_cast<f64>(1010 + i)); // 100 distinct
  Panel panel = make_single_field_panel("gics", col);
  const std::vector<std::string> fields = {"gics"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  ASSERT_EQ(excl.size(), 1U) << "'gics' must be in backstop -> excluded";
  EXPECT_EQ(excl[0], "gics");
  ASSERT_EQ(grp.size(), 1U)  << "'gics' must also be routed to extra_group";
  EXPECT_EQ(grp[0], "gics");
}

// =============================================================================
// Test 8 — Mixed panel: binary excluded, dense kept, sector skipped.
// =============================================================================
TEST(AtxImplTypedFields, MixedPanelCorrectlyClassifies) {
  const usize n = 50;
  // "close": 50 distinct values (dense -> kept)
  std::vector<f64> close_col;
  for (usize i = 0; i < n; ++i) close_col.push_back(100.0 + static_cast<f64>(i));
  // "flag": 2 distinct values (binary -> excluded)
  std::vector<f64> flag_col;
  for (usize i = 0; i < n; ++i) flag_col.push_back((i % 2 == 0) ? 1.0 : -1.0);

  auto r = Panel::create(n, 1U, {"close", "flag"}, {close_col, flag_col}, {});
  ASSERT_TRUE(r.has_value());
  Panel panel = std::move(*r);
  const std::vector<std::string> fields = {"close", "flag"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  ASSERT_EQ(excl.size(), 1U);
  EXPECT_EQ(excl[0], "flag") << "'flag' (binary) must be excluded";
  EXPECT_TRUE(grp.empty())   << "'flag' is not a categorical group field";
}

// =============================================================================
// Test 9 — NaN values are ignored in the cardinality scan.
//   A field with values {NaN, 1.0, 2.0} has cardinality 2 (not 3).
// =============================================================================
TEST(AtxImplTypedFields, NaNValuesIgnoredInCardinalityScan) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  // 3 cells: NaN, 1.0, 2.0 — only 2 finite distinct values.
  auto r = Panel::create(3U, 1U, {"myfield"}, {{kNaN, 1.0, 2.0}}, {});
  ASSERT_TRUE(r.has_value());
  Panel panel = std::move(*r);
  const std::vector<std::string> fields = {"myfield"};

  std::vector<std::string> excl, grp;
  atx::impl::detail::classify_typed_fields(panel, fields, /*K=*/12, excl, grp);

  // cardinality 2 <= 12 -> excluded
  ASSERT_EQ(excl.size(), 1U) << "NaN-skipped field with 2 finite values must be excluded";
}

} // namespace atxtest_typed_fields
