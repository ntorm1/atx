#include "support/synthetic_opra_hive.hpp"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "atx/core/io/parquet.hpp" // read_parquet, ParquetTable
#include "atx/vol/opra_panel.hpp"  // OpraLoadSpec, load_opra_cbbo_from_table

// Self-tests for the synthetic multi-symbol OPRA hive fixture (Task 2). These
// prove the fixture is loader-consumable BEFORE Tasks 3/5 build the C++ loader
// and build driver on top of it:
//
//   1. V2LayoutFilesExist                      — the hive-v2 date-partition layout
//                                                <root>/date=<d>/data.parquet.
//   2. V2FileLoadsPerSymbolThroughTableSeam    — one date file, read via
//                                                read_parquet, then split per
//                                                `underlying` through the
//                                                load_opra_cbbo_from_table seam
//                                                (the 8-column v2 schema it
//                                                requires), yields a 36-quote
//                                                panel with a PCP-implied spot.

namespace {

namespace fs = std::filesystem;
namespace tsupport = atx::vol::testsupport;

TEST(SyntheticHive, V2LayoutFilesExist) {
  const fs::path tmp = fs::temp_directory_path() / "atx_synth_hive_v2_exist";
  fs::remove_all(tmp);
  tsupport::write_synthetic_hive_v2(tmp, {});
  EXPECT_TRUE(fs::exists(tmp / "date=2026-07-01" / "data.parquet"));
  EXPECT_TRUE(fs::exists(tmp / "date=2026-07-02" / "data.parquet"));
  EXPECT_TRUE(fs::exists(tmp / "date=2026-07-06" / "data.parquet"));
  fs::remove_all(tmp);
}

TEST(SyntheticHive, V2FileLoadsPerSymbolThroughTableSeam) {
  const fs::path tmp = fs::temp_directory_path() / "atx_synth_hive_v2_load";
  fs::remove_all(tmp);
  tsupport::write_synthetic_hive_v2(tmp, {});

  const fs::path date_file = tmp / "date=2026-07-01" / "data.parquet";
  ASSERT_TRUE(fs::exists(date_file));

  // The v2 date file carries all three symbols; read it once, then feed it back
  // per-underlying through the in-memory-table seam (Task 1) that requires the
  // full 8-column OPRA schema.
  const auto table = atx::core::io::read_parquet(date_file.string());
  ASSERT_TRUE(table.has_value()) << table.error().to_string();

  atx::vol::OpraLoadSpec spec;
  spec.path = date_file.string();
  spec.underlying = "AAA";
  spec.snapshot_iso = "2026-07-01";
  spec.r = 0.03;

  const auto panel = atx::vol::load_opra_cbbo_from_table(*table, spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  // 9 strikes x 2 expiries x {C,P} = 36 quotes for underlying "AAA".
  EXPECT_EQ(panel->n_contracts, 36U);
  EXPECT_EQ(panel->frame.rows.size(), 36U);
  // PCP-consistent Black mids imply the planted spot (within 1% of 100.0).
  EXPECT_NEAR(panel->implied_spot, 100.0, 1.0);

  fs::remove_all(tmp);
}

} // namespace
