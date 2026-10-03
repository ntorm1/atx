// The descriptor templates (research/store/detail/table_ops.hpp) over toy tables: exact DDL
// and SQL text, bind / read of every Sql type, refusals, append-only, upsert, key order.

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "research/research_store_test_support.hpp"

namespace store = atx::engine::research::store;
namespace db = atx::core::db;
using atx::core::ErrorCode;
using store::test::kPlainTable;
using store::test::kToyTable;
using store::test::PlainRow;
using store::test::ToyRow;

// Review finding 4: the member-type rule binds Column itself, not only col().
template <class M>
concept IntColumnFormable = requires { typename store::Column<ToyRow, M, store::Sql::Int>; };
static_assert(IntColumnFormable<atx::i64> && IntColumnFormable<std::optional<atx::i64>>);
static_assert(!IntColumnFormable<atx::i32> && !IntColumnFormable<bool>);
static_assert(!store::StorableAs<atx::i32, store::Sql::Int>);
static_assert(!store::StorableAs<float, store::Sql::Real>);
static_assert(!store::StorableAs<std::string, store::Sql::Blob>);
static_assert(!store::StorableAs<std::optional<atx::i64>, store::Sql::Bool>);
static_assert(store::StorableAs<std::optional<atx::u64>, store::Sql::U64>);

namespace {

atx::core::Status insert_row(db::Database &d, const ToyRow &row) {
  return store::execute_row(d, store::insert_sql(kToyTable), kToyTable, row);
}

atx::core::Status upsert_plain(db::Database &d, const PlainRow &row) {
  return store::execute_row(d, store::upsert_sql(kPlainTable), kPlainTable, row);
}

} // namespace

TEST(ResearchStoreTable, ToyDdlExactText) {
  const std::vector<std::string> expected{
      "CREATE TABLE toy(k TEXT NOT NULL, i INTEGER NOT NULL, b INTEGER NOT NULL CHECK(b IN (0, "
      "1)), r REAL NOT NULL, u INTEGER NOT NULL, s TEXT NOT NULL CHECK(length(s) = 64 AND s NOT "
      "GLOB '*[^0-9a-f]*'), p TEXT NOT NULL CHECK(instr(p, '\\') = 0 AND substr(p, 1, 1) <> '/' "
      "AND ('/' || p || '/') NOT GLOB '*/../*'), j TEXT NOT NULL CHECK(json_valid(j)), x BLOB NOT "
      "NULL, o TEXT CHECK(o IN ('red', 'blue')), v INTEGER, PRIMARY KEY(k)) STRICT, WITHOUT "
      "ROWID;",
      "CREATE INDEX ix_toy_s ON toy(s);",
      "CREATE TRIGGER toy_no_update BEFORE UPDATE ON toy BEGIN SELECT RAISE(ABORT, 'toy is "
      "append-only'); END;",
      "CREATE TRIGGER toy_no_delete BEFORE DELETE ON toy BEGIN SELECT RAISE(ABORT, 'toy is "
      "append-only'); END;"};
  EXPECT_EQ(store::ddl(kToyTable), expected);
  EXPECT_EQ(store::insert_sql(kToyTable),
            "INSERT INTO toy(k, i, b, r, u, s, p, j, x, o, v) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, "
            "?8, ?9, ?10, ?11);");
  EXPECT_EQ(store::select_all_sql(kPlainTable), "SELECT a, b, c, d FROM plain ORDER BY a, b;");
  EXPECT_EQ(store::upsert_sql(kPlainTable),
            "INSERT INTO plain(a, b, c, d) VALUES (?1, ?2, ?3, ?4) ON CONFLICT(a, b) DO UPDATE "
            "SET c = excluded.c, d = excluded.d;");
  // The migration step of a later column, and nothing for a version without changes.
  EXPECT_EQ(store::ddl_since(store::test::kMigV2Table, 2),
            (std::vector<std::string>{"ALTER TABLE mig ADD COLUMN b REAL;",
                                      "CREATE INDEX ix_mig_b ON mig(b);"}));
  EXPECT_EQ(store::ddl_since(store::test::kMigV2Table, 1),
            (std::vector<std::string>{
                "CREATE TABLE mig(k TEXT NOT NULL, a INTEGER NOT NULL, PRIMARY KEY(k)) STRICT, "
                "WITHOUT ROWID;"}));
  EXPECT_TRUE(store::ddl_since(store::test::kMigV2Table, 3).empty());
  // Every statement applies on the vendored SQLite.
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kToyTable));
  store::test::exec_all(d, store::ddl(kPlainTable));
}

TEST(ResearchStoreTable, BindReadRoundTripEveryType) {
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kToyTable));
  ToyRow row = store::test::sample_toy("k1");
  row.o = "blue";
  row.r = -2.75e-300;
  row.i = std::numeric_limits<atx::i64>::min();
  row.b = false;
  ASSERT_TRUE(insert_row(d, row));
  auto rows = store::select_all(d, kToyTable);
  ASSERT_TRUE(rows) << rows.error().to_string();
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(rows->front(), row);
  // The storage is what the DDL promises: Bool 0 / 1 INTEGER, Blob BLOB.
  EXPECT_EQ(store::test::scalar_text(d, "SELECT typeof(b) || typeof(x) || typeof(r) FROM toy"),
            "integerblobreal");
}

TEST(ResearchStoreTable, NullOptionalRoundTrip) {
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kToyTable));
  ToyRow empty = store::test::sample_toy("a");
  empty.o = std::nullopt;
  empty.v = std::nullopt;
  empty.x.clear();
  ToyRow red = store::test::sample_toy("b");
  red.o = "red";
  red.v = 0;
  ASSERT_TRUE(insert_row(d, empty));
  ASSERT_TRUE(insert_row(d, red));
  EXPECT_EQ(store::test::scalar_int(d, "SELECT count(*) FROM toy WHERE o IS NULL AND v IS NULL"),
            1);
  auto rows = store::select_all(d, kToyTable);
  ASSERT_TRUE(rows) << rows.error().to_string();
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ((*rows)[0], empty);
  EXPECT_EQ((*rows)[1], red);
  EXPECT_FALSE((*rows)[0].o.has_value());
  EXPECT_EQ((*rows)[1].v, std::optional<atx::i64>{0});
}

TEST(ResearchStoreTable, U64BitPatternRoundTrip) {
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kToyTable));
  const std::vector<atx::u64> values{0ULL, 1ULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
                                     0xFFFFFFFFFFFFFFFFULL};
  for (std::size_t n = 0; n < values.size(); ++n) {
    ToyRow row = store::test::sample_toy("u" + std::to_string(n));
    row.u = values[n];
    ASSERT_TRUE(insert_row(d, row));
  }
  // 2^64 - 1 is stored as its signed bit pattern, -1.
  EXPECT_EQ(store::test::scalar_int(d, "SELECT u FROM toy WHERE k = 'u4'"), -1);
  auto rows = store::select_all(d, kToyTable);
  ASSERT_TRUE(rows) << rows.error().to_string();
  ASSERT_EQ(rows->size(), values.size());
  for (std::size_t n = 0; n < values.size(); ++n) {
    EXPECT_EQ((*rows)[n].u, values[n]) << n;
  }
}

TEST(ResearchStoreTable, ReadRejectsNullInRequiredColumn) {
  db::Database d = store::test::memory_db();
  // A lax copy of the toy table (no NOT NULL, no STRICT) holds what the real DDL refuses.
  ASSERT_TRUE(d.exec("CREATE TABLE toy(k, i, b, r, u, s, p, j, x, o, v)"));
  ASSERT_TRUE(d.exec("INSERT INTO toy VALUES ('n', NULL, 0, 1.0, 0, 's', 'p', '{}', x'', NULL, "
                     "NULL), ('t', 'text', 0, 1.0, 0, 's', 'p', '{}', x'', NULL, NULL), ('b', 1, "
                     "2, 1.0, 0, 's', 'p', '{}', x'', NULL, NULL)"));
  auto stmt = d.prepare(store::select_all_sql(kToyTable));
  ASSERT_TRUE(stmt) << stmt.error().to_string();
  // The contract is the code and the "<table>.<column>: " prefix; the rest is wrapper text.
  for (const std::string_view expected : {"toy.b: ", "toy.i: ", "toy.i: "}) {
    ASSERT_EQ(*stmt->step(), db::Statement::Step::Row);
    auto row = store::read(*stmt, kToyTable);
    ASSERT_FALSE(row) << expected;
    EXPECT_EQ(row.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(row.error().message().rfind(expected, 0), 0U) << row.error().message();
  }
  // A statement of another shape is refused before any column is read.
  auto narrow = d.prepare("SELECT k FROM toy");
  ASSERT_TRUE(narrow);
  ASSERT_EQ(*narrow->step(), db::Statement::Step::Row);
  auto refused = store::read(*narrow, kToyTable);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
}

TEST(ResearchStoreTable, AppendOnlyRefusesUpdateAndDelete) {
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kToyTable));
  ASSERT_TRUE(insert_row(d, store::test::sample_toy("k1")));
  const auto code_of = [&](std::string_view sql) {
    auto done = d.exec(sql);
    return done ? ErrorCode::Unknown : done.error().code();
  };
  EXPECT_EQ(code_of("UPDATE toy SET i = 2"), ErrorCode::InvalidArgument);
  EXPECT_EQ(code_of("DELETE FROM toy"), ErrorCode::InvalidArgument);
  // The upsert of an existing key takes the UPDATE path and is refused the same way.
  auto upserted = store::execute_row(d, store::upsert_sql(kToyTable), kToyTable,
                                     store::test::sample_toy("k1"));
  ASSERT_FALSE(upserted);
  EXPECT_EQ(upserted.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(upserted.error().message().find("toy is append-only"), std::string::npos);
  auto duplicate = insert_row(d, store::test::sample_toy("k1"));
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code(), ErrorCode::AlreadyExists);
  EXPECT_EQ(store::test::scalar_int(d, "SELECT i FROM toy"), -42);
}

TEST(ResearchStoreTable, UpsertReplacesNonKeyColumns) {
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kPlainTable));
  ASSERT_TRUE(upsert_plain(d, PlainRow{"a", 1, 0.5, "first"}));
  ASSERT_TRUE(upsert_plain(d, PlainRow{"a", 2, std::nullopt, "other key"}));
  ASSERT_TRUE(upsert_plain(d, PlainRow{"a", 1, std::nullopt, "second"}));
  auto rows = store::select_all(d, kPlainTable);
  ASSERT_TRUE(rows) << rows.error().to_string();
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ((*rows)[0], (PlainRow{"a", 1, std::nullopt, "second"}));
  EXPECT_EQ((*rows)[1], (PlainRow{"a", 2, std::nullopt, "other key"}));
}

TEST(ResearchStoreTable, SelectAllOrderedByKeyUnderReverseUnorderedSelects) {
  db::Database d = store::test::memory_db();
  ASSERT_TRUE(d.exec("PRAGMA reverse_unordered_selects = ON"));
  store::test::exec_all(d, store::ddl(kPlainTable));
  const std::vector<PlainRow> scrambled{{"b", 2, 1.0, "x"}, {"a", 9, 2.0, "y"}, {"b", -1, 3.0, "z"},
                                        {"a", 0, std::nullopt, "w"}, {"c", 5, 4.0, "v"}};
  for (const PlainRow &row : scrambled) {
    ASSERT_TRUE(upsert_plain(d, row));
  }
  auto rows = store::select_all(d, kPlainTable);
  ASSERT_TRUE(rows) << rows.error().to_string();
  std::vector<std::pair<std::string, atx::i64>> keys;
  for (const PlainRow &row : *rows) {
    keys.emplace_back(row.a, row.b);
  }
  EXPECT_EQ(keys, (std::vector<std::pair<std::string, atx::i64>>{
                      {"a", 0}, {"a", 9}, {"b", -1}, {"b", 2}, {"c", 5}}));
}

TEST(ResearchStoreTable, BindRefusesNanAndNegativeZeroReal) {
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kToyTable));
  for (const atx::f64 bad : {std::nan(""), -0.0}) {
    ToyRow row = store::test::sample_toy("bad");
    row.r = bad;
    auto refused = insert_row(d, row);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(refused.error().message().find("toy.r"), std::string::npos);
  }
  EXPECT_EQ(store::test::scalar_int(d, "SELECT count(*) FROM toy"), 0);
  ToyRow zero = store::test::sample_toy("zero");
  zero.r = 0.0;
  EXPECT_TRUE(insert_row(d, zero));
}

TEST(ResearchStoreTable, TypeChecksRefuseBadValues) {
  db::Database d = store::test::memory_db();
  store::test::exec_all(d, store::ddl(kToyTable));
  const auto refused = [&](ToyRow row) {
    auto done = insert_row(d, row);
    return !done && done.error().code() == ErrorCode::InvalidArgument;
  };
  ToyRow row = store::test::sample_toy("bad");
  row.s = std::string(64, 'A'); // upper-case hex
  EXPECT_TRUE(refused(row));
  row = store::test::sample_toy("bad");
  row.s = std::string(63, 'a');
  EXPECT_TRUE(refused(row));
  for (const char *path : {"/abs/x", "a\\b", "..", "a/../b", "a/.."}) {
    row = store::test::sample_toy("bad");
    row.p = path;
    EXPECT_TRUE(refused(row)) << path;
  }
  row = store::test::sample_toy("bad");
  row.j = "{not json";
  EXPECT_TRUE(refused(row));
  row = store::test::sample_toy("bad");
  row.o = "green";
  EXPECT_TRUE(refused(row));
  // Allowed look-alikes pass.
  row = store::test::sample_toy("good");
  row.p = "a/..b/c..";
  row.o = "red";
  EXPECT_TRUE(insert_row(d, row));
}
