// atx::engine::data — adapt_panel unit tests (P2-S6.4a).
//
// Suite: DataAdaptPanel
//
// Covers:
//   * PriceOnlyPanelEqualsWithDatafields — boundary-pin seed: path A (direct
//       with_datafields) and path B (Dataset + price_to_panel) produce panels
//       byte-identical across all fields, dates, and instruments.
//   * AdvWindowsMatch   — adv5 field present and cells match path A.
//   * MaskPreserved     — in_universe(d,i) matches path A; out-of-universe
//                         cells are NaN in both panels.
//   * EmptyMaskAllInUniverse — empty mask → byte-identical to with_datafields
//                              called with an empty universe.
//   * MissingCloseErrs  — Dataset lacking "close" → price_to_panel is_err().

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

#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/adapt_panel.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atxtest_data_adapt_panel_test {

using atx::core::ErrorCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::datafields::with_datafields;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::price_to_panel;
using atx::engine::data::Role;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

namespace {

// 4 dates × 3 instruments OHLCV fixture.
// field index: 0=open 1=high 2=low 3=close 4=volume
// cell (d,i) for field f  = f*100 + d*10 + i + 1   (all positive, no NaN in base data)
constexpr atx::usize kDates = 4;
constexpr atx::usize kInsts = 3;
constexpr atx::usize kCells = kDates * kInsts; // 12

std::vector<std::string> make_field_names() { return {"open", "high", "low", "close", "volume"}; }

std::vector<std::vector<atx::f64>> make_field_data() {
  std::vector<std::vector<atx::f64>> data(5, std::vector<atx::f64>(kCells));
  for (atx::usize f = 0; f < 5; ++f) {
    for (atx::usize d = 0; d < kDates; ++d) {
      for (atx::usize i = 0; i < kInsts; ++i) {
        data[f][d * kInsts + i] = static_cast<atx::f64>(f * 100 + d * 10 + i + 1);
      }
    }
  }
  return data;
}

// Non-trivial universe: out-of-universe at (d=0,i=1) and (d=2,i=2).
// date-major flat: index = d*kInsts + i
std::vector<std::uint8_t> make_universe() {
  std::vector<std::uint8_t> u(kCells, 1u);
  u[0 * kInsts + 1] = 0u; // (d0, i1) out
  u[2 * kInsts + 2] = 0u; // (d2, i2) out
  return u;
}

DatasetSchema make_ohlcv_schema() {
  DatasetSchema s;
  s.columns = make_field_names();
  s.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
              ColumnDType::F64};
  s.role = Role::Price;
  return s;
}

std::vector<DateKey> make_dates() { return {20230101, 20230102, 20230103, 20230104}; }

std::vector<InstKey> make_instruments() { return {1u, 2u, 3u}; }

// Build a Dataset from the fixture data.
Dataset make_dataset(std::vector<std::uint8_t> mask,
                     const std::vector<std::string> *col_names_override = nullptr) {
  DatasetSchema schema = make_ohlcv_schema();
  if (col_names_override) {
    schema.columns = *col_names_override;
    schema.dtypes.resize(col_names_override->size(), ColumnDType::F64);
  }
  std::vector<std::vector<atx::f64>> data = make_field_data();
  if (col_names_override) {
    data.resize(col_names_override->size());
  }
  auto res = Dataset::create(schema, make_dates(), make_instruments(), std::move(data),
                             std::move(mask), DatasetProvenance{"test", "ohlcv"});
  EXPECT_TRUE(res.has_value()) << (res.has_value() ? "" : res.error().message());
  return std::move(res).value();
}

// Compare two Panel cells bit-for-bit (handles NaN == NaN correctly).
bool cells_equal(atx::f64 a, atx::f64 b) noexcept {
  atx::u64 ua{};
  atx::u64 ub{};
  std::memcpy(&ua, &a, sizeof(atx::u64));
  std::memcpy(&ub, &b, sizeof(atx::u64));
  return ua == ub;
}

// Assert that every cell in every field of panel_a and panel_b is bit-identical.
void assert_panels_byte_identical(const Panel &panel_a, const Panel &panel_b) {
  ASSERT_EQ(panel_a.num_fields(), panel_b.num_fields());
  ASSERT_EQ(panel_a.dates(), panel_b.dates());
  ASSERT_EQ(panel_a.instruments(), panel_b.instruments());

  const atx::usize nf = panel_a.num_fields();
  for (atx::usize f = 0; f < nf; ++f) {
    // Field names must match in order.
    EXPECT_EQ(panel_a.field_name(static_cast<atx::engine::alpha::FieldId>(f)),
              panel_b.field_name(static_cast<atx::engine::alpha::FieldId>(f)))
        << "field index " << f;

    const auto col_a = panel_a.field_all(static_cast<atx::engine::alpha::FieldId>(f));
    const auto col_b = panel_b.field_all(static_cast<atx::engine::alpha::FieldId>(f));
    ASSERT_EQ(col_a.size(), col_b.size()) << "field " << f;

    for (atx::usize k = 0; k < col_a.size(); ++k) {
      EXPECT_TRUE(cells_equal(col_a[k], col_b[k]))
          << "field=" << f << " cell=" << k << " a=" << col_a[k] << " b=" << col_b[k];
    }
  }
}

} // namespace

// ---------------------------------------------------------------------------
// DataAdaptPanel suite
// ---------------------------------------------------------------------------

// THE boundary-pin seed.
// Path A: direct with_datafields call.
// Path B: Dataset + price_to_panel.
// Both must produce byte-identical panels.
TEST(DataAdaptPanel, PriceOnlyPanelEqualsWithDatafields) {
  const std::vector<atx::u16> adv = {2u, 3u};
  const std::span<const atx::u16> adv_sp{adv};

  auto names_a = make_field_names();
  auto data_a = make_field_data();
  auto univ_a = make_universe();

  // Path A — direct call.
  auto res_a = with_datafields(kDates, kInsts, names_a, data_a, univ_a, adv_sp);
  ASSERT_TRUE(res_a.has_value()) << res_a.error().message();
  const Panel panel_a = std::move(res_a).value();

  // Path B — Dataset + adapter.
  const Dataset ds = make_dataset(make_universe());
  auto res_b = price_to_panel(ds, adv_sp);
  ASSERT_TRUE(res_b.has_value()) << res_b.error().message();
  const Panel panel_b = std::move(res_b).value();

  assert_panels_byte_identical(panel_a, panel_b);
}

// adv5 field must be present and match path A cell-for-cell.
TEST(DataAdaptPanel, AdvWindowsMatch) {
  const std::vector<atx::u16> adv = {5u};
  const std::span<const atx::u16> adv_sp{adv};

  auto names_a = make_field_names();
  auto data_a = make_field_data();
  auto univ_a = make_universe();

  auto res_a = with_datafields(kDates, kInsts, names_a, data_a, univ_a, adv_sp);
  ASSERT_TRUE(res_a.has_value()) << res_a.error().message();
  const Panel panel_a = std::move(res_a).value();

  const Dataset ds = make_dataset(make_universe());
  auto res_b = price_to_panel(ds, adv_sp);
  ASSERT_TRUE(res_b.has_value()) << res_b.error().message();
  const Panel panel_b = std::move(res_b).value();

  // adv5 field present in path B.
  auto fid_res = panel_b.field_id("adv5");
  ASSERT_TRUE(fid_res.has_value()) << "adv5 field missing from adapter Panel";

  // Cells of adv5 must match path A.
  const auto fid_a_res = panel_a.field_id("adv5");
  ASSERT_TRUE(fid_a_res.has_value());
  const auto col_a = panel_a.field_all(fid_a_res.value());
  const auto col_b = panel_b.field_all(fid_res.value());
  ASSERT_EQ(col_a.size(), col_b.size());
  for (atx::usize k = 0; k < col_a.size(); ++k) {
    EXPECT_TRUE(cells_equal(col_a[k], col_b[k]))
        << "adv5 cell=" << k << " a=" << col_a[k] << " b=" << col_b[k];
  }
}

// The adapter Panel's in_universe(d,i) must match path A's.  Out-of-universe
// cells in the DERIVED dollar_volume field must be NaN in both panels
// (with_datafields NaN-gates derived columns; raw base fields retain their
// input values even for out-of-universe cells, so we test a derived field here).
TEST(DataAdaptPanel, MaskPreserved) {
  const std::vector<atx::u16> adv = {2u};
  const std::span<const atx::u16> adv_sp{adv};

  auto names_a = make_field_names();
  auto data_a = make_field_data();
  const auto univ_a = make_universe();

  auto res_a = with_datafields(kDates, kInsts, names_a, data_a, univ_a, adv_sp);
  ASSERT_TRUE(res_a.has_value()) << res_a.error().message();
  const Panel panel_a = std::move(res_a).value();

  const Dataset ds = make_dataset(make_universe());
  auto res_b = price_to_panel(ds, adv_sp);
  ASSERT_TRUE(res_b.has_value()) << res_b.error().message();
  const Panel panel_b = std::move(res_b).value();

  // in_universe must agree for all cells.
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize i = 0; i < kInsts; ++i) {
      EXPECT_EQ(panel_a.in_universe(d, i), panel_b.in_universe(d, i)) << "d=" << d << " i=" << i;
    }
  }

  // Out-of-universe cells must be NaN in the derived dollar_volume field
  // (with_datafields NaN-gates derived columns at out-of-universe positions).
  const auto dvol_a_res = panel_a.field_id("dollar_volume");
  const auto dvol_b_res = panel_b.field_id("dollar_volume");
  ASSERT_TRUE(dvol_a_res.has_value()) << "path A: dollar_volume missing";
  ASSERT_TRUE(dvol_b_res.has_value()) << "path B: dollar_volume missing";

  const atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
  // Known out-of-universe cells: (d=0,i=1) and (d=2,i=2).
  struct Cell {
    atx::usize d;
    atx::usize i;
  };
  for (const auto [d, i] : {Cell{0, 1}, Cell{2, 2}}) {
    const auto cs_a = panel_a.field_cross_section(dvol_a_res.value(), d);
    const auto cs_b = panel_b.field_cross_section(dvol_b_res.value(), d);
    EXPECT_TRUE(cells_equal(cs_a[i], kNaN))
        << "path A dollar_volume not NaN at d=" << d << " i=" << i;
    EXPECT_TRUE(cells_equal(cs_b[i], kNaN))
        << "path B dollar_volume not NaN at d=" << d << " i=" << i;
  }
}

// A Dataset built with an empty mask (all-in-universe) → adapter Panel must be
// byte-identical to with_datafields called with an empty universe.
TEST(DataAdaptPanel, EmptyMaskAllInUniverse) {
  const std::vector<atx::u16> adv = {2u};
  const std::span<const atx::u16> adv_sp{adv};

  auto names_a = make_field_names();
  auto data_a = make_field_data();

  // Path A with empty universe.
  auto res_a = with_datafields(kDates, kInsts, names_a, data_a,
                               /*universe=*/{}, adv_sp);
  ASSERT_TRUE(res_a.has_value()) << res_a.error().message();
  const Panel panel_a = std::move(res_a).value();

  // Path B with empty mask Dataset.
  const Dataset ds = make_dataset(/*mask=*/{});
  auto res_b = price_to_panel(ds, adv_sp);
  ASSERT_TRUE(res_b.has_value()) << res_b.error().message();
  const Panel panel_b = std::move(res_b).value();

  assert_panels_byte_identical(panel_a, panel_b);
}

// A Price Dataset whose schema lacks "close" must cause price_to_panel to return
// Err (forwarded from with_datafields Err(NotFound, "… missing required base
// field 'close'")).
TEST(DataAdaptPanel, MissingCloseErrs) {
  // Build a schema without "close": just open, high, low, volume.
  const std::vector<std::string> bad_names = {"open", "high", "low", "volume"};
  const Dataset ds = make_dataset(/*mask=*/{}, &bad_names);

  const std::vector<atx::u16> adv = {2u};
  auto res = price_to_panel(ds, std::span<const atx::u16>{adv});
  EXPECT_FALSE(res.has_value());
}

} // namespace atxtest_data_adapt_panel_test
