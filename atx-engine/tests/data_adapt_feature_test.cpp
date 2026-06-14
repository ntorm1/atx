// atx::engine::data — adapt_feature unit tests (P2-S6.4b).
//
// Suite: DataAdaptFeature
//
// Covers:
//   * FeatureColumnsAppendedByName        — aligned feature appended as a named
//       field; existing price fields unchanged (same id + values).
//   * AlignedFeatureReferenceableInFeatureSpec — merged feature feeds a
//       FeatureMatrix built via learn::build_features.
//   * MissingFeatureCellMakesRowInvalid   — uncovered (NaN) feature cell ->
//       row_valid == 0 (M8).
//   * FeatureNameCollisionErrs            — feature column named "close" ->
//       merge_features_into_panel returns Err.
//   * PriceOnlyUnaffected                 — merge is opt-in: original fields and
//       values are byte-identical to panel_in.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/data/adapt_feature.hpp"
#include "atx/engine/data/adapt_panel.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/learn/feature_matrix.hpp"

namespace atxtest_data_adapt_feature_test {

using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaStore;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::merge_features_into_panel;
using atx::engine::data::price_to_panel;
using atx::engine::data::Role;
using atx::engine::learn::build_features;
using atx::engine::learn::FeatureSpec;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

namespace {

// 3 dates × 2 instruments price axis.
//   dates {1,2,3}; instruments {10,20}.
// field index: 0=open 1=high 2=low 3=close 4=volume.
// cell (d,i) for field f = f*100 + d*10 + i + 1  (all positive, no NaN).
constexpr atx::usize kDates = 3;
constexpr atx::usize kInsts = 2;
constexpr atx::usize kCells = kDates * kInsts; // 6

std::vector<DateKey> price_dates() { return {1, 2, 3}; }
std::vector<InstKey> price_insts() { return {10u, 20u}; }

DatasetSchema make_ohlcv_schema() {
  DatasetSchema s;
  s.columns = {"open", "high", "low", "close", "volume"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
              ColumnDType::F64};
  s.role = Role::Price;
  return s;
}

Dataset make_price_dataset() {
  std::vector<std::vector<atx::f64>> data(5, std::vector<atx::f64>(kCells));
  for (atx::usize f = 0; f < 5; ++f) {
    for (atx::usize d = 0; d < kDates; ++d) {
      for (atx::usize i = 0; i < kInsts; ++i) {
        data[f][d * kInsts + i] = static_cast<atx::f64>(f * 100 + d * 10 + i + 1);
      }
    }
  }
  auto res = Dataset::create(make_ohlcv_schema(), price_dates(), price_insts(), std::move(data),
                             /*mask=*/{}, DatasetProvenance{"test", "price"});
  EXPECT_TRUE(res.has_value()) << (res.has_value() ? "" : res.error().message());
  return std::move(res).value();
}

// A "sentiment" feature Dataset covering ONLY instrument 10 (instrument 20 is
// absent -> uncovered -> NaN after alignment). Single date row at date 1 so
// every canonical date >= 1 carries it forward (delisted-survives semantics).
// Values: sentiment(inst10) = 0.5.
Dataset make_sentiment_dataset(const std::string &col_name = "sentiment") {
  DatasetSchema s;
  s.columns = {col_name};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Feature;
  // 1 date × 1 instrument (inst 10), value 0.5.
  std::vector<std::vector<atx::f64>> data = {{0.5}};
  auto res = Dataset::create(std::move(s), /*dates=*/{1}, /*instruments=*/{10u}, std::move(data),
                             /*mask=*/{}, DatasetProvenance{"test", "sentiment"});
  EXPECT_TRUE(res.has_value()) << (res.has_value() ? "" : res.error().message());
  return std::move(res).value();
}

bool cells_equal(atx::f64 a, atx::f64 b) noexcept {
  atx::u64 ua{};
  atx::u64 ub{};
  std::memcpy(&ua, &a, sizeof(atx::u64));
  std::memcpy(&ub, &b, sizeof(atx::u64));
  return ua == ub;
}

// Build a price-only Panel via the S6.4a adapter (no adv windows needed here).
Panel make_price_panel(const Dataset &price) {
  std::vector<atx::u16> adv{}; // no adv columns
  auto res = price_to_panel(price, std::span<const atx::u16>{adv});
  EXPECT_TRUE(res.has_value()) << (res.has_value() ? "" : res.error().message());
  return std::move(res).value();
}

} // namespace

// ---------------------------------------------------------------------------
// DataAdaptFeature suite
// ---------------------------------------------------------------------------

// The aligned feature is appended as a named field; covered cells carry the
// feature value, uncovered cells are NaN; existing price fields are unchanged.
TEST(DataAdaptFeature, FeatureColumnsAppendedByName) {
  const Dataset price = make_price_dataset();
  const Dataset feature = make_sentiment_dataset();
  const Panel panel_in = make_price_panel(price);

  auto res = merge_features_into_panel(panel_in, price, feature);
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Panel merged = std::move(res).value();

  // "sentiment" resolves.
  auto sent_res = merged.field_id("sentiment");
  ASSERT_TRUE(sent_res.has_value()) << "merged Panel missing sentiment field";
  const auto sent = merged.field_all(sent_res.value());
  ASSERT_EQ(sent.size(), kCells);

  // inst 10 covered (value 0.5 carried forward at every date); inst 20 NaN.
  for (atx::usize d = 0; d < kDates; ++d) {
    EXPECT_DOUBLE_EQ(sent[d * kInsts + 0], 0.5) << "d=" << d << " inst10";
    EXPECT_TRUE(std::isnan(sent[d * kInsts + 1])) << "d=" << d << " inst20";
  }

  // Existing price fields unchanged: same field_id and byte-identical values.
  for (const std::string &name : {std::string{"open"}, std::string{"high"}, std::string{"low"},
                                  std::string{"close"}, std::string{"volume"}}) {
    auto fid_in = panel_in.field_id(name);
    auto fid_merged = merged.field_id(name);
    ASSERT_TRUE(fid_in.has_value());
    ASSERT_TRUE(fid_merged.has_value());
    EXPECT_EQ(fid_in.value(), fid_merged.value()) << "field id changed for " << name;
    const auto col_in = panel_in.field_all(fid_in.value());
    const auto col_merged = merged.field_all(fid_merged.value());
    ASSERT_EQ(col_in.size(), col_merged.size());
    for (atx::usize k = 0; k < col_in.size(); ++k) {
      EXPECT_TRUE(cells_equal(col_in[k], col_merged[k])) << name << " cell=" << k;
    }
  }
}

// The merged feature column actually feeds a FeatureMatrix: build_features with
// raw_fields {"close","sentiment"} produces a matrix whose sentiment feature
// column equals the aligned dataset value for valid (covered) rows.
TEST(DataAdaptFeature, AlignedFeatureReferenceableInFeatureSpec) {
  const Dataset price = make_price_dataset();
  const Dataset feature = make_sentiment_dataset();
  const Panel panel_in = make_price_panel(price);

  auto merge_res = merge_features_into_panel(panel_in, price, feature);
  ASSERT_TRUE(merge_res.has_value()) << merge_res.error().message();
  const Panel merged = std::move(merge_res).value();

  const AlphaStore store; // empty pool: no pool alphas referenced
  FeatureSpec spec;
  spec.raw_fields = {"close", "sentiment"};
  spec.pool_alphas = {};
  spec.horizons = {1};   // minimal single horizon
  spec.max_lookback = 0; // raw fields read only date d

  auto fm_res = build_features(merged, store, spec);
  ASSERT_TRUE(fm_res.has_value()) << fm_res.error().message();
  const auto fm = std::move(fm_res).value();

  EXPECT_EQ(fm.n_features, atx::usize{2}); // close, sentiment

  // sentiment is the 2nd feature column (index 1). For inst-10 rows it must equal
  // the aligned value 0.5; that row is also valid (both close and sentiment finite).
  ASSERT_GT(fm.n_rows(), atx::usize{0});
  bool saw_inst10 = false;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    const atx::usize inst = fm.row_inst[r];
    const atx::f64 sentiment_val = fm.X[r * fm.n_features + 1];
    if (inst == 0) { // canonical instrument index 0 == InstKey 10
      saw_inst10 = true;
      EXPECT_DOUBLE_EQ(sentiment_val, 0.5) << "row " << r;
      EXPECT_EQ(fm.row_valid[r], atx::u8{1}) << "inst10 row " << r << " should be valid";
    }
  }
  EXPECT_TRUE(saw_inst10) << "no inst-10 rows emitted";
}

// An uncovered feature cell (NaN after alignment) makes its row invalid (M8):
// build_features emits the row (the cell is in-universe) but row_valid == 0.
TEST(DataAdaptFeature, MissingFeatureCellMakesRowInvalid) {
  const Dataset price = make_price_dataset();
  const Dataset feature = make_sentiment_dataset(); // covers only inst 10
  const Panel panel_in = make_price_panel(price);

  auto merge_res = merge_features_into_panel(panel_in, price, feature);
  ASSERT_TRUE(merge_res.has_value()) << merge_res.error().message();
  const Panel merged = std::move(merge_res).value();

  const AlphaStore store;
  FeatureSpec spec;
  spec.raw_fields = {"close", "sentiment"};
  spec.horizons = {1};
  spec.max_lookback = 0;

  auto fm_res = build_features(merged, store, spec);
  ASSERT_TRUE(fm_res.has_value()) << fm_res.error().message();
  const auto fm = std::move(fm_res).value();

  // inst 20 (canonical index 1) has NaN sentiment everywhere -> its rows invalid.
  bool saw_inst20 = false;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    if (fm.row_inst[r] == 1) { // canonical instrument index 1 == InstKey 20
      saw_inst20 = true;
      const atx::f64 sentiment_val = fm.X[r * fm.n_features + 1];
      EXPECT_TRUE(std::isnan(sentiment_val)) << "row " << r << " sentiment should be NaN";
      EXPECT_EQ(fm.row_valid[r], atx::u8{0}) << "inst20 row " << r << " should be invalid";
    }
  }
  EXPECT_TRUE(saw_inst20) << "no inst-20 rows emitted";
}

// A feature column whose name collides with an existing panel field ("close")
// makes merge_features_into_panel return Err.
TEST(DataAdaptFeature, FeatureNameCollisionErrs) {
  const Dataset price = make_price_dataset();
  const Dataset feature = make_sentiment_dataset(/*col_name=*/"close"); // collides
  const Panel panel_in = make_price_panel(price);

  auto res = merge_features_into_panel(panel_in, price, feature);
  EXPECT_FALSE(res.has_value());
}

// Merge is OPT-IN: the original price Panel is untouched. We assert that for every
// original field, the merged Panel's values are byte-identical to panel_in — i.e.
// merging only APPENDS and never mutates the price-only path.
TEST(DataAdaptFeature, PriceOnlyUnaffected) {
  const Dataset price = make_price_dataset();
  const Dataset feature = make_sentiment_dataset();
  const Panel panel_in = make_price_panel(price);

  auto res = merge_features_into_panel(panel_in, price, feature);
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Panel merged = std::move(res).value();

  // The merged Panel carries strictly MORE fields (the appended feature).
  EXPECT_EQ(merged.num_fields(), panel_in.num_fields() + 1);

  // Every original field id < panel_in.num_fields() maps to the same name and
  // byte-identical values in the merged Panel (append-only, order preserved).
  for (atx::usize f = 0; f < panel_in.num_fields(); ++f) {
    const auto fid = static_cast<atx::engine::alpha::FieldId>(f);
    EXPECT_EQ(panel_in.field_name(fid), merged.field_name(fid)) << "field index " << f;
    const auto col_in = panel_in.field_all(fid);
    const auto col_merged = merged.field_all(fid);
    ASSERT_EQ(col_in.size(), col_merged.size());
    for (atx::usize k = 0; k < col_in.size(); ++k) {
      EXPECT_TRUE(cells_equal(col_in[k], col_merged[k])) << "field=" << f << " cell=" << k;
    }
  }

  // in_universe is carried over verbatim.
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize i = 0; i < kInsts; ++i) {
      EXPECT_EQ(panel_in.in_universe(d, i), merged.in_universe(d, i)) << "d=" << d << " i=" << i;
    }
  }
}

} // namespace atxtest_data_adapt_feature_test
