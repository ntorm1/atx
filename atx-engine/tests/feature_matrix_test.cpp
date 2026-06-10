// atx::engine::learn — FeatureMatrix + train scaffold unit tests (S5-1).
//
// Covers the plan's load-bearing semantics:
//   * Suite FeatureMatrix
//       1. ForwardLabel_IsForwardReturn_NaNAtTail  — labels read FORWARD, NaN at
//          the tail (never zero) (M8 / §0.6).
//       2. Feature_ReadsOnlyUpToDate_TruncationInvariant — a feature cell at date
//          d is identical with or without later dates present (PIT, M2).
//       3. OutOfUniverseCell_Excluded_NotZeroed — an out-of-universe (d,i) emits
//          NO row (has_row false), not a zeroed row (M8).
//       4. RowOrder_IsDateThenInstrument_Deterministic — rows are emitted in
//          (date, instrument) order (M1).
//   * Suite TrainScaffold
//       5. DateFold_ExpandsToRows_NoTestDateInTrain — a CPCV date-fold expands to
//          feature rows with NO train row sharing a date with any test row (§0.2).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>   // std::isnan
#include <cstdint> // std::uint8_t
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/eval/cpcv.hpp"

#include "atx/engine/learn/feature_matrix.hpp"
#include "atx/engine/learn/train.hpp"

namespace {

using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaId;
using atx::engine::learn::FeatureMatrix;
using atx::engine::learn::FeatureSpec;

// Build a close-only Panel from a (date-major) per-date list of instrument
// closes. `rows[d][i]` is the close of instrument i at date d. `universe` (if
// non-empty) is the dates*instruments PIT mask (1 == in-universe); empty == all
// in-universe. Mirrors the as-built Panel::create contract (column-major: one
// field column of dates*instruments cells, date-major within the column).
[[nodiscard]] Panel make_panel_close(const std::vector<std::vector<atx::f64>> &rows,
                                     std::vector<std::uint8_t> universe = {}) {
  const atx::usize dates = rows.size();
  const atx::usize insts = rows.empty() ? 0U : rows[0].size();
  std::vector<atx::f64> col;
  col.reserve(dates * insts);
  for (const std::vector<atx::f64> &r : rows) {
    for (const atx::f64 v : r) {
      col.push_back(v);
    }
  }
  std::vector<std::vector<atx::f64>> data{std::move(col)};
  auto res = Panel::create(dates, insts, {"close"}, std::move(data), std::move(universe));
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

// The default S5-1 spec the tests exercise: raw "close", an EMPTY pool, one
// horizon (overridden per test), max_lookback 0 (raw fields read only date d).
[[nodiscard]] FeatureSpec close_spec(std::vector<atx::u16> horizons) {
  FeatureSpec spec;
  spec.raw_fields = {"close"};
  spec.pool_alphas = std::vector<AlphaId>{};
  spec.horizons = std::move(horizons);
  spec.max_lookback = 0U;
  return spec;
}

// ---- Suite FeatureMatrix -----------------------------------------------------

// 1. Forward-return label is close[d+H]/close[d]-1; NaN at the tail (not zero).
TEST(FeatureMatrix, ForwardLabel_IsForwardReturn_NaNAtTail) {
  // 3 dates x 2 insts: d0=[10,20], d1=[11,22], d2=[12,24].
  const Panel panel = make_panel_close({{10.0, 20.0}, {11.0, 22.0}, {12.0, 24.0}});
  const atx::engine::combine::AlphaStore store; // empty pool

  auto fm_res = atx::engine::learn::build_features(panel, store, close_spec({1}));
  ASSERT_TRUE(fm_res.has_value()) << (fm_res ? "" : fm_res.error().message());
  const FeatureMatrix &fm = fm_res.value();

  ASSERT_EQ(fm.Y.size(), 1U);                 // one horizon
  ASSERT_TRUE(fm.has_row(0, 0));              // (d0, i0)
  const atx::usize r00 = fm.row_of(0, 0);
  EXPECT_NEAR(fm.Y[0][r00], 11.0 / 10.0 - 1.0, 1e-12); // forward return

  ASSERT_TRUE(fm.has_row(2, 0));              // last date (d2, i0)
  const atx::usize r20 = fm.row_of(2, 0);
  EXPECT_TRUE(std::isnan(fm.Y[0][r20]));      // NaN at the tail, NOT zero
}

// 2. A feature cell at date d reads only rows with date <= d (PIT firewall):
//    the value at (d1,i0) is identical with or without a later date present.
TEST(FeatureMatrix, Feature_ReadsOnlyUpToDate_TruncationInvariant) {
  const Panel full =
      make_panel_close({{10.0, 20.0}, {11.0, 22.0}, {12.0, 24.0}, {13.0, 26.0}});
  const Panel trunc = make_panel_close({{10.0, 20.0}, {11.0, 22.0}, {12.0, 24.0}});
  const atx::engine::combine::AlphaStore store;

  auto full_res = atx::engine::learn::build_features(full, store, close_spec({1}));
  auto trunc_res = atx::engine::learn::build_features(trunc, store, close_spec({1}));
  ASSERT_TRUE(full_res.has_value());
  ASSERT_TRUE(trunc_res.has_value());
  const FeatureMatrix &a = full_res.value();
  const FeatureMatrix &b = trunc_res.value();

  ASSERT_TRUE(a.has_row(1, 0));
  ASSERT_TRUE(b.has_row(1, 0));
  // close feature is feature index 0; X is row-major-by-row (col == feature).
  const atx::f64 a_cell = a.X[a.row_of(1, 0) * a.n_features + 0U];
  const atx::f64 b_cell = b.X[b.row_of(1, 0) * b.n_features + 0U];
  EXPECT_EQ(a_cell, b_cell); // feature reads <= d: d3's presence cannot change it
  EXPECT_EQ(a_cell, 11.0);
}

// 3. An out-of-universe (d0,i1) emits NO row — not a zeroed row.
TEST(FeatureMatrix, OutOfUniverseCell_Excluded_NotZeroed) {
  // 2 dates x 2 insts; mask out (d0, i1). Universe is dates*instruments,
  // date-major: [ (d0,i0) (d0,i1) (d1,i0) (d1,i1) ].
  const std::vector<std::uint8_t> universe{1, 0, 1, 1};
  const Panel panel = make_panel_close({{10.0, 20.0}, {11.0, 22.0}}, universe);
  const atx::engine::combine::AlphaStore store;

  auto fm_res = atx::engine::learn::build_features(panel, store, close_spec({1}));
  ASSERT_TRUE(fm_res.has_value());
  const FeatureMatrix &fm = fm_res.value();

  EXPECT_FALSE(fm.has_row(0, 1)); // out-of-universe: no row at all
  EXPECT_TRUE(fm.has_row(0, 0));  // in-universe neighbor still present
  EXPECT_TRUE(fm.has_row(1, 1));  // in-universe at a later date still present
}

// 4. Rows are emitted in (date, instrument) order, deterministically.
TEST(FeatureMatrix, RowOrder_IsDateThenInstrument_Deterministic) {
  const Panel panel = make_panel_close({{10.0, 20.0}, {11.0, 22.0}});
  const atx::engine::combine::AlphaStore store;

  auto fm_res = atx::engine::learn::build_features(panel, store, close_spec({1}));
  ASSERT_TRUE(fm_res.has_value());
  const FeatureMatrix &fm = fm_res.value();

  ASSERT_TRUE(fm.has_row(0, 0));
  ASSERT_TRUE(fm.has_row(0, 1));
  ASSERT_TRUE(fm.has_row(1, 0));
  EXPECT_LT(fm.row_of(0, 0), fm.row_of(0, 1)); // same date: instrument-minor
  EXPECT_LT(fm.row_of(0, 1), fm.row_of(1, 0)); // date-major dominates
}

// ---- Suite TrainScaffold -----------------------------------------------------

// 5. A CPCV date-fold expands to feature rows with NO train row sharing a date
//    with any test row (the §0.2 firewall, carried to the (date x instrument)
//    feature rows).
TEST(TrainScaffold, DateFold_ExpandsToRows_NoTestDateInTrain) {
  // 6 dates x 2 insts, strictly increasing closes (all rows valid, no NaN).
  std::vector<std::vector<atx::f64>> rows;
  for (atx::usize d = 0; d < 6; ++d) {
    const atx::f64 base = 10.0 + static_cast<atx::f64>(d);
    rows.push_back({base, base + 10.0});
  }
  const Panel panel = make_panel_close(rows);
  const atx::engine::combine::AlphaStore store;

  auto fm_res = atx::engine::learn::build_features(panel, store, close_spec({1}));
  ASSERT_TRUE(fm_res.has_value());
  const FeatureMatrix &fm = fm_res.value();

  atx::engine::eval::CpcvConfig cfg;
  cfg.n_groups = 3;
  cfg.n_test_groups = 1;
  cfg.embargo = 0.0;

  const std::vector<atx::engine::eval::LabelSpan> spans =
      atx::engine::learn::date_label_spans(fm, 1);
  const auto folds =
      atx::engine::learn::expand_date_folds(atx::engine::eval::cpcv_folds(spans, cfg), fm);

  ASSERT_FALSE(folds.empty());
  for (const auto &f : folds) {
    for (const atx::usize tr : f.train_rows) {
      for (const atx::usize te : f.test_rows) {
        EXPECT_NE(fm.row_date[tr], fm.row_date[te]); // no train date == any test date
      }
    }
  }
}

} // namespace
