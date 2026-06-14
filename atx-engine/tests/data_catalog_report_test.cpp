// data_catalog_report_test.cpp — S6.9: the catalog/lineage report writer
// reproducibility gate (suite CatalogReport).
//
// write_catalog_report must emit a BYTE-REPRODUCIBLE headless artifact: writing the
// same catalog twice (to two paths) produces byte-identical files. The content must
// list the registered dataset names, their roles/schema/provenance, AND the lineage
// edge recorded via DatasetCatalog::derive. This pins the determinism contract (no
// timestamps / RNG / absolute paths / hash-order iteration) with concrete substring
// checks plus a full byte-equality assertion.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/catalog_report.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atxtest_data_catalog_report_test {

using atx::f64;
using atx::usize;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetCatalog;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::Role;
using atx::engine::data::write_catalog_report;

namespace {

constexpr usize kDates = 4;
constexpr usize kInsts = 2;

[[nodiscard]] std::vector<DateKey> ascending_dates() {
  std::vector<DateKey> d(kDates);
  for (usize t = 0; t < kDates; ++t) {
    d[t] = static_cast<DateKey>(t);
  }
  return d;
}

[[nodiscard]] std::vector<InstKey> insts() { return {0u, 1u}; }

// A one-column f64 Dataset over the canonical small axis.
[[nodiscard]] Dataset make_dataset(std::vector<std::string> columns,
                                   std::vector<ColumnDType> dtypes, Role role,
                                   const std::string &source) {
  DatasetSchema s;
  s.columns = std::move(columns);
  s.dtypes = std::move(dtypes);
  s.role = role;
  s.region = "US";
  s.universe_tag = "top2";
  std::vector<std::vector<f64>> data(s.columns.size(), std::vector<f64>(kDates * kInsts, 1.0));
  auto r = Dataset::create(std::move(s), ascending_dates(), insts(), std::move(data),
                           /*mask=*/{}, DatasetProvenance{source, "note"});
  EXPECT_TRUE(r.has_value()) << "dataset must build";
  return std::move(r).value();
}

[[nodiscard]] std::string read_file(const std::filesystem::path &p) {
  std::ifstream is{p, std::ios::binary};
  EXPECT_TRUE(is.good()) << "report file must exist: " << p.string();
  return std::string{std::istreambuf_iterator<char>{is}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::string base = std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s6_9_report" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

} // namespace

// =============================================================================
//  CatalogReportIsReproducible — two writes are byte-identical + content checks.
// =============================================================================
TEST(CatalogReport, CatalogReportIsReproducible) {
  DatasetCatalog catalog;
  ASSERT_TRUE(catalog
                  .register_dataset("prices", make_dataset({"close", "rev"},
                                                           {ColumnDType::F64, ColumnDType::F64},
                                                           Role::Price, "byo:prices"))
                  .has_value());
  ASSERT_TRUE(catalog
                  .register_dataset("sentiment", make_dataset({"score"}, {ColumnDType::F64},
                                                              Role::Feature, "byo:news"))
                  .has_value());
  ASSERT_TRUE(catalog
                  .register_dataset("signal", make_dataset({"ext_sig"}, {ColumnDType::F64},
                                                           Role::Signal, "external:sentiment_v1"))
                  .has_value());
  // A lineage edge: signal derives from sentiment.
  ASSERT_TRUE(catalog.derive("signal", {"sentiment"}).has_value());

  const std::string dir_a = tmpdir("a");
  const std::string dir_b = tmpdir("b");
  ASSERT_TRUE(write_catalog_report(catalog, dir_a).has_value());
  ASSERT_TRUE(write_catalog_report(catalog, dir_b).has_value());

  const std::string a = read_file(std::filesystem::path{dir_a} / "catalog_report.txt");
  const std::string b = read_file(std::filesystem::path{dir_b} / "catalog_report.txt");

  // Byte-reproducible: the two reports are identical.
  EXPECT_EQ(a, b) << "the report must be byte-reproducible";
  EXPECT_FALSE(a.empty());

  // Concrete content: every dataset name appears, with roles + provenance + lineage.
  EXPECT_NE(a.find("dataset: prices"), std::string::npos);
  EXPECT_NE(a.find("dataset: sentiment"), std::string::npos);
  EXPECT_NE(a.find("dataset: signal"), std::string::npos);
  EXPECT_NE(a.find("role: Signal"), std::string::npos);
  EXPECT_NE(a.find("provenance_source: external:sentiment_v1"), std::string::npos);
  // The recorded lineage edge: signal's parents list includes sentiment.
  EXPECT_NE(a.find("lineage_parents: sentiment"), std::string::npos);

  // Determinism guard: no absolute path of either out_dir leaks into the bytes.
  EXPECT_EQ(a.find(dir_a), std::string::npos) << "no absolute path may leak into the report";
  EXPECT_EQ(a.find(dir_b), std::string::npos) << "no absolute path may leak into the report";
}

// =============================================================================
//  CatalogReportEmptyCatalog — the zero-dataset path: succeeds + deterministic.
// =============================================================================
TEST(CatalogReport, CatalogReportEmptyCatalog) {
  const DatasetCatalog empty; // no datasets registered

  const std::string dir_a = tmpdir("empty_a");
  const std::string dir_b = tmpdir("empty_b");
  ASSERT_TRUE(write_catalog_report(empty, dir_a).has_value())
      << "an empty catalog must still write a (header-only) report";
  ASSERT_TRUE(write_catalog_report(empty, dir_b).has_value());

  const std::string a = read_file(std::filesystem::path{dir_a} / "catalog_report.txt");
  const std::string b = read_file(std::filesystem::path{dir_b} / "catalog_report.txt");

  // Deterministic: two writes of the empty catalog are byte-identical.
  EXPECT_EQ(a, b) << "the empty-catalog report must be byte-reproducible";
  // Header-only: the report version header is present, but no dataset block.
  EXPECT_NE(a.find("catalog_report"), std::string::npos) << "the report header must be present";
  EXPECT_EQ(a.find("dataset:"), std::string::npos) << "an empty catalog lists no datasets";
}

} // namespace atxtest_data_catalog_report_test
