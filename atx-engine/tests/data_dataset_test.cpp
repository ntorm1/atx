// atx::engine::data — Dataset + DatasetSchema unit tests (P2-S6.1).
//
// Suite: DataDataset
//
// Covers:
//   * CreateRejectsRaggedColumns          — mismatched column cell-count -> Err
//   * CreateRejectsDtypeRoleMismatch      — dtypes.size() != columns.size() -> Err
//   * RoundTripsColumnsByteIdentical      — every cell (incl. NaN) round-trips exactly
//   * OhlcvDatasetMatchesPanelColumns     — Price Dataset columns == Panel field data
//   * SingleInstrument                    — 1-instrument shape is valid
//   * SingleDate                          — 1-date shape is valid
//   * EmptyMaskMeansAllInUniverse         — empty mask -> every cell in-universe
//   * ColumnByNameUnknownErrs             — column_by_name with bad name -> Err(NotFound)
//   * CreateRejectsDuplicateColumnNames   — duplicate column name -> Err
//   * ZeroCellDatasetAccepted             — empty dates + zero-cell columns -> ok
//   * MaskFiltersUniverse                 — non-empty mask -> exact in_universe bits
//   * CreateRejectsBadMaskSize            — mask.size() != dates*instruments -> Err

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atxtest_data_dataset_test {

using atx::core::ErrorCode;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::Role;
using atx::engine::data::schema_is_coherent;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

DatasetSchema make_ohlcv_schema() {
  DatasetSchema s;
  s.columns = {"open", "high", "low", "close", "volume"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
              ColumnDType::F64};
  s.role = Role::Price;
  s.pit_delay = 0;
  s.region = "US";
  s.universe_tag = "sp500";
  return s;
}

// Build a simple 2-date x 3-instrument OHLCV dataset with known values.
// cell (d,i) for field f = static_cast<f64>(f*100 + d*10 + i + 1)
Dataset make_ohlcv_dataset() {
  const atx::usize nd = 2;
  const atx::usize ni = 3;

  std::vector<DateKey> dates = {20230101, 20230102};
  std::vector<InstKey> instruments = {1u, 2u, 3u};

  // 5 columns, 6 cells each (date-major: d0i0, d0i1, d0i2, d1i0, d1i1, d1i2)
  std::vector<std::vector<atx::f64>> cols(5, std::vector<atx::f64>(nd * ni));
  for (atx::usize c = 0; c < 5; ++c) {
    for (atx::usize d = 0; d < nd; ++d) {
      for (atx::usize i = 0; i < ni; ++i) {
        cols[c][d * ni + i] = static_cast<atx::f64>(c * 100 + d * 10 + i + 1);
      }
    }
  }

  auto res = Dataset::create(make_ohlcv_schema(), std::move(dates), std::move(instruments),
                             std::move(cols),
                             /*mask=*/{}, DatasetProvenance{"test", "ohlcv fixture"});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

} // namespace

// ---------------------------------------------------------------------------
// DataDataset suite
// ---------------------------------------------------------------------------

TEST(DataDataset, CreateRejectsRaggedColumns) {
  // Column 0 has correct size (2*3=6), column 1 has wrong size (5).
  DatasetSchema schema = make_ohlcv_schema();

  std::vector<std::vector<atx::f64>> cols(5, std::vector<atx::f64>(6, 1.0));
  cols[1].resize(5); // ragged: one cell short

  auto res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u, 3u}, std::move(cols), {},
                             DatasetProvenance{});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(DataDataset, CreateRejectsDtypeRoleMismatch) {
  // schema has 5 columns names but only 3 dtypes -> schema_is_coherent fails
  DatasetSchema schema = make_ohlcv_schema();
  schema.dtypes.resize(3); // mismatch: columns.size()=5, dtypes.size()=3

  std::vector<std::vector<atx::f64>> cols(5, std::vector<atx::f64>(6, 1.0));

  auto res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u, 3u}, std::move(cols), {},
                             DatasetProvenance{});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(DataDataset, RoundTripsColumnsByteIdentical) {
  const atx::usize nd = 2;
  const atx::usize ni = 3;
  const atx::usize cells = nd * ni;

  DatasetSchema schema;
  schema.columns = {"a", "b"};
  schema.dtypes = {ColumnDType::F64, ColumnDType::F64};
  schema.role = Role::Feature;

  // Include a NaN to verify bit-identical NaN round-trip.
  const atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
  std::vector<std::vector<atx::f64>> original_cols = {{1.0, 2.0, 3.0, 4.0, 5.0, 6.0},
                                                      {7.0, kNaN, 9.0, 10.0, 11.0, 12.0}};

  // Copy for comparison before move.
  const std::vector<std::vector<atx::f64>> expected_cols = original_cols;

  auto res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u, 3u}, std::move(original_cols),
                             {}, DatasetProvenance{});
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Dataset ds = std::move(res).value();

  EXPECT_EQ(ds.num_dates(), nd);
  EXPECT_EQ(ds.num_instruments(), ni);
  EXPECT_EQ(ds.cells(), cells);

  for (atx::usize c = 0; c < 2; ++c) {
    const auto col = ds.column(c);
    ASSERT_EQ(col.size(), cells);
    for (atx::usize k = 0; k < cells; ++k) {
      // Bit-identical comparison (works for NaN too).
      atx::u64 got{};
      atx::u64 want{};
      std::memcpy(&got, &col[k], sizeof(atx::u64));
      std::memcpy(&want, &expected_cols[c][k], sizeof(atx::u64));
      EXPECT_EQ(got, want) << "col=" << c << " cell=" << k;
    }
  }
}

TEST(DataDataset, OhlcvDatasetMatchesPanelColumns) {
  const atx::usize nd = 2;
  const atx::usize ni = 3;

  // Build the same data for both Dataset and Panel.
  const std::vector<std::string> field_names = {"open", "high", "low", "close", "volume"};
  std::vector<std::vector<atx::f64>> field_data(5, std::vector<atx::f64>(nd * ni));
  for (atx::usize c = 0; c < 5; ++c) {
    for (atx::usize d = 0; d < nd; ++d) {
      for (atx::usize i = 0; i < ni; ++i) {
        field_data[c][d * ni + i] = static_cast<atx::f64>(c * 100 + d * 10 + i + 1);
      }
    }
  }

  // ---- Dataset ----
  DatasetSchema schema = make_ohlcv_schema();
  std::vector<std::vector<atx::f64>> ds_cols = field_data; // copy
  auto ds_res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u, 3u}, std::move(ds_cols), {},
                                DatasetProvenance{"test", "ohlcv"});
  ASSERT_TRUE(ds_res.has_value()) << ds_res.error().message();
  const Dataset ds = std::move(ds_res).value();

  // ---- Panel ----
  std::vector<std::vector<atx::f64>> panel_data = field_data; // copy
  auto panel_res = atx::engine::alpha::Panel::create(nd, ni, field_names, std::move(panel_data),
                                                     /*universe=*/{});
  ASSERT_TRUE(panel_res.has_value()) << panel_res.error().message();
  const atx::engine::alpha::Panel panel = std::move(panel_res).value();

  // Compare each field: Dataset::column_by_name vs Panel::field_all.
  for (atx::usize c = 0; c < field_names.size(); ++c) {
    const std::string &name = field_names[c];

    auto col_res = ds.column_by_name(name);
    ASSERT_TRUE(col_res.has_value()) << "Dataset missing column: " << name;

    auto fid_res = panel.field_id(name);
    ASSERT_TRUE(fid_res.has_value()) << "Panel missing field: " << name;
    const auto panel_col = panel.field_all(fid_res.value());

    const auto ds_col = col_res.value();
    ASSERT_EQ(ds_col.size(), panel_col.size());

    for (atx::usize k = 0; k < ds_col.size(); ++k) {
      atx::u64 got{};
      atx::u64 want{};
      std::memcpy(&got, &ds_col[k], sizeof(atx::u64));
      std::memcpy(&want, &panel_col[k], sizeof(atx::u64));
      EXPECT_EQ(got, want) << "field=" << name << " cell=" << k;
    }
  }
}

TEST(DataDataset, SingleInstrument) {
  DatasetSchema schema;
  schema.columns = {"price"};
  schema.dtypes = {ColumnDType::F64};
  schema.role = Role::Price;

  // 3 dates, 1 instrument
  std::vector<std::vector<atx::f64>> cols = {{10.0, 20.0, 30.0}};
  auto res = Dataset::create(schema, {20230101, 20230102, 20230103}, {42u}, std::move(cols), {},
                             DatasetProvenance{});
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Dataset ds = std::move(res).value();
  EXPECT_EQ(ds.num_instruments(), 1U);
  EXPECT_EQ(ds.num_dates(), 3U);
  EXPECT_EQ(ds.cells(), 3U);

  const auto col = ds.column(0);
  ASSERT_EQ(col.size(), 3U);
  EXPECT_DOUBLE_EQ(col[0], 10.0);
  EXPECT_DOUBLE_EQ(col[1], 20.0);
  EXPECT_DOUBLE_EQ(col[2], 30.0);
}

TEST(DataDataset, SingleDate) {
  DatasetSchema schema;
  schema.columns = {"signal"};
  schema.dtypes = {ColumnDType::F64};
  schema.role = Role::Signal;

  // 1 date, 4 instruments
  std::vector<std::vector<atx::f64>> cols = {{1.1, 2.2, 3.3, 4.4}};
  auto res = Dataset::create(schema, {20230101}, {10u, 20u, 30u, 40u}, std::move(cols), {},
                             DatasetProvenance{});
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Dataset ds = std::move(res).value();
  EXPECT_EQ(ds.num_dates(), 1U);
  EXPECT_EQ(ds.num_instruments(), 4U);

  const auto col = ds.column(0);
  EXPECT_DOUBLE_EQ(col[0], 1.1);
  EXPECT_DOUBLE_EQ(col[3], 4.4);
}

TEST(DataDataset, EmptyMaskMeansAllInUniverse) {
  DatasetSchema schema;
  schema.columns = {"x"};
  schema.dtypes = {ColumnDType::F64};
  schema.role = Role::Reference;

  std::vector<std::vector<atx::f64>> cols = {{1.0, 2.0, 3.0, 4.0}};
  auto res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u}, std::move(cols),
                             /*mask=*/{}, DatasetProvenance{});
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Dataset ds = std::move(res).value();

  for (atx::usize d = 0; d < 2; ++d) {
    for (atx::usize i = 0; i < 2; ++i) {
      EXPECT_TRUE(ds.in_universe(d, i)) << "d=" << d << " i=" << i;
    }
  }
}

TEST(DataDataset, ColumnByNameUnknownErrs) {
  const Dataset ds = make_ohlcv_dataset();
  auto res = ds.column_by_name("nonexistent_column");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
}

TEST(DataDataset, CreateRejectsDuplicateColumnNames) {
  // Duplicate column name "px" -> schema_is_coherent false -> InvalidArgument.
  DatasetSchema schema;
  schema.columns = {"px", "px", "vol"};
  schema.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64};
  schema.role = Role::Price;

  // 3 columns, 6 cells each (well-formed shape); only the names are bad.
  std::vector<std::vector<atx::f64>> cols(3, std::vector<atx::f64>(6, 1.0));

  auto res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u, 3u}, std::move(cols), {},
                             DatasetProvenance{});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(DataDataset, ZeroCellDatasetAccepted) {
  // Coherent schema (non-empty columns/dtypes) but no dates -> zero cells.
  DatasetSchema schema;
  schema.columns = {"x"};
  schema.dtypes = {ColumnDType::F64};
  schema.role = Role::Feature;

  // Zero dates, two instruments -> 0 cells; the single column is empty.
  std::vector<std::vector<atx::f64>> cols = {std::vector<atx::f64>{}};
  auto res =
      Dataset::create(schema, /*dates=*/{}, {1u, 2u}, std::move(cols), {}, DatasetProvenance{});
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Dataset ds = std::move(res).value();

  EXPECT_EQ(ds.num_dates(), 0U);
  EXPECT_EQ(ds.cells(), 0U);
  EXPECT_TRUE(ds.column(0).empty());
}

TEST(DataDataset, MaskFiltersUniverse) {
  DatasetSchema schema;
  schema.columns = {"x"};
  schema.dtypes = {ColumnDType::F64};
  schema.role = Role::Reference;

  // 2 dates x 2 instruments; mask (date-major): (d0,i0)=in, (d0,i1)=out,
  // (d1,i0)=out, (d1,i1)=in.
  std::vector<std::vector<atx::f64>> cols = {{1.0, 2.0, 3.0, 4.0}};
  std::vector<std::uint8_t> mask = {1, 0, 0, 1};
  auto res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u}, std::move(cols), mask,
                             DatasetProvenance{});
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Dataset ds = std::move(res).value();

  EXPECT_TRUE(ds.in_universe(0, 0));
  EXPECT_FALSE(ds.in_universe(0, 1));
  EXPECT_FALSE(ds.in_universe(1, 0));
  EXPECT_TRUE(ds.in_universe(1, 1));
}

TEST(DataDataset, CreateRejectsBadMaskSize) {
  DatasetSchema schema;
  schema.columns = {"x"};
  schema.dtypes = {ColumnDType::F64};
  schema.role = Role::Reference;

  // 2 dates x 2 instruments -> needs 4 mask bytes; supply 3.
  std::vector<std::vector<atx::f64>> cols = {{1.0, 2.0, 3.0, 4.0}};
  std::vector<std::uint8_t> mask = {1, 0, 1};
  auto res = Dataset::create(schema, {20230101, 20230102}, {1u, 2u}, std::move(cols), mask,
                             DatasetProvenance{});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

} // namespace atxtest_data_dataset_test
