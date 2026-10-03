#pragma once

// Shared support of the research-store gtests: toy descriptors (instantiated in the test TUs,
// the "one gtest TU that instantiates a toy table" of sql-design section 3.6, here shared by
// the table, digest and open tests), temp paths and fixture reads.

#include <array>
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
#include "atx/engine/research/store/store.hpp"
#include "research/store/detail/table.hpp"
#include "research/store/detail/table_ops.hpp"
#include "research/store/tables_common.hpp"

#ifndef ATX_RESEARCH_STORE_FIXTURE
#error "ATX_RESEARCH_STORE_FIXTURE must name atx-engine/tests/fixtures/research_store"
#endif

namespace atx::engine::research::store::test {

using atx::f64;
using atx::i32;
using atx::i64;
using atx::u64;

// Every Sql type, an optional with `allowed`, an index, a volatile column; append-only.
struct ToyRow {
  std::string k;
  i64 i{};
  bool b{};
  f64 r{};
  u64 u{};
  std::string s;
  std::string p;
  std::string j;
  std::vector<std::byte> x;
  std::optional<std::string> o;
  std::optional<i64> v;

  friend bool operator==(const ToyRow &, const ToyRow &) = default;
};

inline constexpr std::array<std::string_view, 2> kToyColours{"red", "blue"};

inline constexpr auto kToyTable = table<ToyRow>(
    "toy",
    TableOpts{.version = 3, .since = 1, .append_only = true, .volatile_table = false, .check = {}},
    col<Sql::Text>("k", &ToyRow::k, kKey), col<Sql::Int>("i", &ToyRow::i),
    col<Sql::Bool>("b", &ToyRow::b), col<Sql::Real>("r", &ToyRow::r),
    col<Sql::U64>("u", &ToyRow::u), col<Sql::Sha256>("s", &ToyRow::s, kIndexed),
    col<Sql::RelPath>("p", &ToyRow::p), col<Sql::Json>("j", &ToyRow::j),
    col<Sql::Blob>("x", &ToyRow::x), col<Sql::Text>("o", &ToyRow::o, 0, kToyColours),
    col<Sql::Int>("v", &ToyRow::v, kVolatile));

// A plain (updatable) table with a two-column key.
struct PlainRow {
  std::string a;
  i64 b{};
  std::optional<f64> c;
  std::string d;

  friend bool operator==(const PlainRow &, const PlainRow &) = default;
};

inline constexpr auto kPlainTable = table<PlainRow>(
    "plain",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("a", &PlainRow::a, kKey), col<Sql::Int>("b", &PlainRow::b, kKey),
    col<Sql::Real>("c", &PlainRow::c), col<Sql::Text>("d", &PlainRow::d));

// A migrating table: v1 (k, a), v2 adds the optional, indexed column b at schema version 2.
struct MigV1Row {
  std::string k;
  i64 a{};
  friend bool operator==(const MigV1Row &, const MigV1Row &) = default;
};
struct MigV2Row {
  std::string k;
  i64 a{};
  std::optional<f64> b;
  friend bool operator==(const MigV2Row &, const MigV2Row &) = default;
};

inline constexpr auto kMigV1Table = table<MigV1Row>(
    "mig",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("k", &MigV1Row::k, kKey), col<Sql::Int>("a", &MigV1Row::a));
inline constexpr auto kMigV2Table = table<MigV2Row>(
    "mig",
    TableOpts{.version = 2, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("k", &MigV2Row::k, kKey), col<Sql::Int>("a", &MigV2Row::a),
    col<Sql::Real>("b", &MigV2Row::b, kIndexed, {}, 2));

inline constexpr GroupOps kMigGroupV1 =
    group_ops<kStoreInfoTable, kMigV1Table>("mig", DbKind::Cache, 1);
inline constexpr GroupOps kMigGroupV2 =
    group_ops<kStoreInfoTable, kMigV2Table>("mig", DbKind::Cache, 2);

// A second catalog group (for multi-group documents and digests).
inline constexpr GroupOps kPlainCatalogGroup =
    group_ops<kPlainTable>("plain_group", DbKind::Catalog, 1);

// The toy cache group whose printed form is fixtures/research_store/schema/toy.json: the
// Python accessor tests use it for every Sql type (the real groups have no bool or blob).
inline constexpr GroupOps kToyGroup =
    group_ops<kStoreInfoTable, kToyTable, kPlainTable>("toy", DbKind::Cache, 1);

inline std::string sha(char c) { return std::string(64, c); }

inline ToyRow sample_toy(std::string key) {
  ToyRow row;
  row.k = std::move(key);
  row.i = -42;
  row.b = true;
  row.r = 1.5;
  row.u = 0xFFFFFFFFFFFFFFFFULL;
  row.s = sha('a');
  row.p = "dir/file.json";
  row.j = R"({"x":[1,2]})";
  row.x = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}};
  row.o = std::nullopt;
  row.v = 7;
  return row;
}

// A fresh path under the per-process temp root (atx-test-scratch), removed with its WAL files.
inline std::filesystem::path temp_path(std::string_view leaf) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto dir = std::filesystem::temp_directory_path() / "atx_research_store_tests" /
                   (std::string{info->test_suite_name()} + "_" + info->name());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto path = dir / std::string{leaf};
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path.string() + "-wal", ec);
  std::filesystem::remove(path.string() + "-shm", ec);
  return path;
}

inline std::filesystem::path fixture(std::string_view relative) {
  return std::filesystem::path{ATX_RESEARCH_STORE_FIXTURE} / std::string{relative};
}

inline std::string read_bytes(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

inline core::db::Database memory_db() {
  auto opened = core::db::Database::open_memory();
  EXPECT_TRUE(opened) << opened.error().to_string();
  return std::move(*opened);
}

// Execute every statement or fail the test.
inline void exec_all(core::db::Database &db, const std::vector<std::string> &statements) {
  for (const std::string &statement : statements) {
    auto done = db.exec(statement);
    ASSERT_TRUE(done) << statement << "\n" << done.error().to_string();
  }
}

inline i64 scalar_int(core::db::Database &db, std::string_view sql) {
  auto stmt = db.prepare(sql);
  EXPECT_TRUE(stmt) << stmt.error().to_string();
  if (!stmt) {
    return -1;
  }
  auto step = stmt->step();
  EXPECT_TRUE(step && *step == core::db::Statement::Step::Row) << sql;
  return stmt->column_int(0);
}

inline std::string scalar_text(core::db::Database &db, std::string_view sql) {
  auto stmt = db.prepare(sql);
  EXPECT_TRUE(stmt) << stmt.error().to_string();
  if (!stmt) {
    return {};
  }
  auto step = stmt->step();
  EXPECT_TRUE(step && *step == core::db::Statement::Step::Row) << sql;
  return std::string{stmt->column_text(0)};
}

} // namespace atx::engine::research::store::test
