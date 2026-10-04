#pragma once

// Shared support of the catalog gtests (P9 SQL2): a private copy of the synthetic tree per test
// (tests edit it and create the two declared payloads), the class registry, catalog runs and
// small SQL readers. Public headers only: the tests never instantiate descriptor templates.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/catalog.hpp"
#include "atx/engine/research/store/catalog/classify.hpp"
#include "atx/engine/research/store/store.hpp"

#ifndef ATX_RESEARCH_STORE_FIXTURE
#error "ATX_RESEARCH_STORE_FIXTURE must name atx-engine/tests/fixtures/research_store"
#endif
#ifndef ATX_RESEARCH_STORE_CLASSES_FILE
#error "ATX_RESEARCH_STORE_CLASSES_FILE must name atx-engine/schemas/research_store/classes.json"
#endif

namespace atx::engine::research::store::catalog::test {

using atx::i64;
using atx::u64;

inline constexpr u64 kMiB = 1024ULL * 1024ULL;
inline constexpr u64 kF1Size = 16 * kMiB + 8;  // make_store_tree.PAYLOAD_SIZES
inline constexpr u64 kF2Size = 16 * kMiB + 16;

inline std::filesystem::path fixture(std::string_view relative) {
  return std::filesystem::path{ATX_RESEARCH_STORE_FIXTURE} / std::string{relative};
}

// A fresh, empty directory under the per-process temp root for this test.
inline std::filesystem::path temp_dir(std::string_view leaf) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto dir = std::filesystem::temp_directory_path() / "atx_research_catalog_tests" /
                   (std::string{info->test_suite_name()} + "_" + info->name()) / std::string{leaf};
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// A private copy of the synthetic tree (tests may edit it).
inline std::filesystem::path tree_copy(std::string_view leaf = "tree") {
  const auto dir = temp_dir(leaf);
  std::filesystem::copy(fixture("tree"), dir, std::filesystem::copy_options::recursive);
  return dir;
}

// A file of `size` bytes, each `fill` (0 = the bytes the fields manifest declares).
inline void write_payload(const std::filesystem::path &path, u64 size, char fill) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  const std::string chunk(1024 * 1024, fill);
  u64 left = size;
  while (left > 0) {
    const u64 n = left < chunk.size() ? left : chunk.size();
    out.write(chunk.data(), static_cast<std::streamsize>(n));
    left -= n;
  }
}

// The two payloads the fields manifest declares (zeros: the declared SHA-256s hold).
inline void make_payloads(const std::filesystem::path &tree, char fill = '\0') {
  write_payload(tree / "build-equity" / "fx-fields" / "f1.f64", kF1Size, fill);
  write_payload(tree / "build-equity" / "fx-fields" / "f2.f64", kF2Size, fill);
}

inline std::string read_bytes(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

inline void write_bytes(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline const ClassRegistry &registry() {
  static const ClassRegistry loaded = [] {
    auto r = load_class_registry(ATX_RESEARCH_STORE_CLASSES_FILE);
    EXPECT_TRUE(r) << r.error().to_string();
    return r ? *r : ClassRegistry{};
  }();
  return loaded;
}

inline core::db::Database open_new(const std::filesystem::path &db_path) {
  auto db = open_catalog(db_path.string(), StoreOpen::CreateIfMissing);
  EXPECT_TRUE(db) << db.error().to_string();
  return std::move(*db);
}

inline CatalogOptions options(const std::filesystem::path &tree) {
  CatalogOptions opt;
  opt.root = tree;
  return opt;
}

// One catalog run; the report (a failed run fails the test).
inline CatalogReport run(core::db::Database &db, const CatalogOptions &opt) {
  auto report = run_catalog(db, registry(), opt);
  EXPECT_TRUE(report) << report.error().to_string();
  return report ? *report : CatalogReport{};
}

inline i64 scalar_int(core::db::Database &db, const std::string &sql) {
  auto stmt = db.prepare(sql);
  EXPECT_TRUE(stmt) << sql << ": " << stmt.error().to_string();
  if (!stmt) {
    return -1;
  }
  auto step = stmt->step();
  EXPECT_TRUE(step && *step == core::db::Statement::Step::Row) << sql;
  return stmt->column_int(0);
}

inline std::optional<std::string> scalar_text(core::db::Database &db, const std::string &sql) {
  auto stmt = db.prepare(sql);
  EXPECT_TRUE(stmt) << sql << ": " << stmt.error().to_string();
  if (!stmt) {
    return std::nullopt;
  }
  auto step = stmt->step();
  if (!step || *step != core::db::Statement::Step::Row || stmt->column_is_null(0)) {
    return std::nullopt;
  }
  return std::string{stmt->column_text(0)};
}

inline i64 rows(core::db::Database &db, std::string_view table) {
  return scalar_int(db, "SELECT count(*) FROM " + std::string{table} + ";");
}

// The pin_status state of one holder pointer ("" when there is no such pin).
inline std::string pin_state(core::db::Database &db, std::string_view holder,
                             std::string_view pointer) {
  return scalar_text(db, "SELECT state FROM pin_status WHERE holder_path = '" +
                             std::string{holder} + "' AND pointer = '" + std::string{pointer} +
                             "';")
      .value_or(std::string{});
}

inline std::string digest_of(core::db::Database &db) {
  auto d = catalog_digest(db, catalog_groups());
  EXPECT_TRUE(d) << d.error().to_string();
  return d ? *d : std::string{};
}

} // namespace atx::engine::research::store::catalog::test
