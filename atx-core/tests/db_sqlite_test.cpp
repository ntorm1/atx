// Tests for the atx::core::db SQLite wrapper (RAII + Result over the C API).
//
// Every test runs against an in-memory database (":memory:") so the suite is
// deterministic, isolated, and touches no filesystem. One behavior per TEST.

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "atx/core/db/blob.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace db = atx::core::db;
using atx::f64;
using atx::i64;
using atx::usize;

// Helper: open an in-memory DB or fail the test hard (later lines would UB).
static db::Database open_mem() {
  auto opened = db::Database::open_memory();
  EXPECT_TRUE(opened.has_value()) << (opened ? std::string{} : opened.error().to_string());
  return std::move(*opened);
}

// ---------------------------------------------------------------------------
//  Database open / lifecycle
// ---------------------------------------------------------------------------

TEST(DbDatabase, OpenMemory_Succeeds) {
  auto opened = db::Database::open_memory();
  ASSERT_TRUE(opened.has_value()) << (opened ? "" : opened.error().to_string());
}

TEST(DbDatabase, OpenReadOnlyMissingFile_ReturnsError) {
  auto opened = db::Database::open("c:/atx-nonexistent-xyz.sqlite", db::OpenMode::ReadOnly);
  EXPECT_FALSE(opened.has_value());
}

TEST(DbDatabase, Move_TransfersOwnership) {
  db::Database a = open_mem();
  ASSERT_TRUE(a.exec("CREATE TABLE t(x INTEGER)").has_value());
  db::Database b = std::move(a);
  // The moved-to handle owns the open connection and the created table.
  EXPECT_TRUE(b.exec("INSERT INTO t(x) VALUES (1)").has_value());
}

// ---------------------------------------------------------------------------
//  exec — statements with no result rows
// ---------------------------------------------------------------------------

TEST(DbExec, CreateTable_Succeeds) {
  db::Database d = open_mem();
  EXPECT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v REAL)").has_value());
}

TEST(DbExec, InvalidSql_ReturnsParseError) {
  db::Database d = open_mem();
  auto r = d.exec("CREATE TABEL oops(");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::ParseError);
}

// ---------------------------------------------------------------------------
//  prepare / bind / step / column
// ---------------------------------------------------------------------------

TEST(DbStatement, BindStepInsert_AndReadBack) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL)").has_value());

  {
    auto ins = d.prepare("INSERT INTO t(id, name, score) VALUES (?1, ?2, ?3)");
    ASSERT_TRUE(ins.has_value()) << (ins ? "" : ins.error().to_string());
    ASSERT_TRUE(ins->bind(1, static_cast<i64>(7)).has_value());
    ASSERT_TRUE(ins->bind(2, std::string_view{"alpha"}).has_value());
    ASSERT_TRUE(ins->bind(3, 1.5).has_value());
    auto step = ins->step();
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(*step, db::Statement::Step::Done);
  }

  auto q = d.prepare("SELECT id, name, score FROM t WHERE id = ?1");
  ASSERT_TRUE(q.has_value());
  ASSERT_TRUE(q->bind(1, static_cast<i64>(7)).has_value());
  auto step = q->step();
  ASSERT_TRUE(step.has_value());
  ASSERT_EQ(*step, db::Statement::Step::Row);
  EXPECT_EQ(q->column_int(0), 7);
  EXPECT_EQ(q->column_text(1), std::string_view{"alpha"});
  EXPECT_DOUBLE_EQ(q->column_double(2), 1.5);
  // Only one row.
  auto step2 = q->step();
  ASSERT_TRUE(step2.has_value());
  EXPECT_EQ(*step2, db::Statement::Step::Done);
}

TEST(DbStatement, BindNamedParameter_Binds) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER)").has_value());
  auto ins = d.prepare("INSERT INTO t(id) VALUES (:id)");
  ASSERT_TRUE(ins.has_value());
  ASSERT_TRUE(ins->bind(":id", static_cast<i64>(42)).has_value());
  ASSERT_TRUE(ins->step().has_value());

  auto q = d.prepare("SELECT id FROM t");
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  EXPECT_EQ(q->column_int(0), 42);
}

TEST(DbStatement, ColumnType_ReportsTypes) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(i INTEGER, f REAL, s TEXT, b BLOB, n INTEGER)").has_value());
  ASSERT_TRUE(d.exec("INSERT INTO t VALUES (1, 2.0, 'x', x'00ff', NULL)").has_value());
  auto q = d.prepare("SELECT i, f, s, b, n FROM t");
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  EXPECT_EQ(q->column_type(0), db::ColumnType::Integer);
  EXPECT_EQ(q->column_type(1), db::ColumnType::Float);
  EXPECT_EQ(q->column_type(2), db::ColumnType::Text);
  EXPECT_EQ(q->column_type(3), db::ColumnType::Blob);
  EXPECT_EQ(q->column_type(4), db::ColumnType::Null);
  EXPECT_TRUE(q->column_is_null(4));
  EXPECT_EQ(q->column_count(), 5);
}

TEST(DbStatement, ResetAndReuse_RebindsAndRuns) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER)").has_value());
  auto ins = d.prepare("INSERT INTO t(id) VALUES (?1)");
  ASSERT_TRUE(ins.has_value());
  for (i64 i = 0; i < 3; ++i) {
    ASSERT_TRUE(ins->reset().has_value());
    ASSERT_TRUE(ins->clear_bindings().has_value());
    ASSERT_TRUE(ins->bind(1, i).has_value());
    ASSERT_TRUE(ins->step().has_value());
  }
  auto q = d.prepare("SELECT COUNT(*) FROM t");
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  EXPECT_EQ(q->column_int(0), 3);
}

// ---------------------------------------------------------------------------
//  Errors / constraints
// ---------------------------------------------------------------------------

TEST(DbStatement, PrepareInvalidSql_ReturnsError) {
  db::Database d = open_mem();
  auto q = d.prepare("SELECT FROM");
  EXPECT_FALSE(q.has_value());
}

TEST(DbStatement, ConstraintViolation_ReturnsError) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY)").has_value());
  ASSERT_TRUE(d.exec("INSERT INTO t(id) VALUES (1)").has_value());
  auto ins = d.prepare("INSERT INTO t(id) VALUES (1)"); // duplicate PK
  ASSERT_TRUE(ins.has_value());
  auto step = ins->step();
  EXPECT_FALSE(step.has_value());
}

// ---------------------------------------------------------------------------
//  rowid / changes
// ---------------------------------------------------------------------------

TEST(DbDatabase, LastInsertRowidAndChanges) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)").has_value());
  ASSERT_TRUE(d.exec("INSERT INTO t(v) VALUES (10)").has_value());
  EXPECT_EQ(d.last_insert_rowid(), 1);
  ASSERT_TRUE(d.exec("INSERT INTO t(v) VALUES (20)").has_value());
  EXPECT_EQ(d.last_insert_rowid(), 2);
  ASSERT_TRUE(d.exec("UPDATE t SET v = v + 1").has_value());
  EXPECT_EQ(d.changes(), 2);
}

// ---------------------------------------------------------------------------
//  Transactions (RAII guard)
// ---------------------------------------------------------------------------

TEST(DbTransaction, Commit_Persists) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER)").has_value());
  {
    auto tx = db::Transaction::begin(d);
    ASSERT_TRUE(tx.has_value());
    ASSERT_TRUE(d.exec("INSERT INTO t(id) VALUES (1)").has_value());
    ASSERT_TRUE(tx->commit().has_value());
  }
  auto q = d.prepare("SELECT COUNT(*) FROM t");
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  EXPECT_EQ(q->column_int(0), 1);
}

TEST(DbTransaction, RollbackOnScopeExit_Discards) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER)").has_value());
  {
    auto tx = db::Transaction::begin(d);
    ASSERT_TRUE(tx.has_value());
    ASSERT_TRUE(d.exec("INSERT INTO t(id) VALUES (1)").has_value());
    // No commit() — the dtor must ROLLBACK.
  }
  auto q = d.prepare("SELECT COUNT(*) FROM t");
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  EXPECT_EQ(q->column_int(0), 0);
}

// ---------------------------------------------------------------------------
//  Prepared-statement cache
// ---------------------------------------------------------------------------

TEST(DbDatabase, PrepareCached_ReusesSameStatement) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER)").has_value());
  auto a = d.prepare_cached("INSERT INTO t(id) VALUES (?1)");
  ASSERT_TRUE(a.has_value());
  auto b = d.prepare_cached("INSERT INTO t(id) VALUES (?1)");
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, *b); // same borrowed Statement* from the cache
}

// ---------------------------------------------------------------------------
//  BLOB — bind/read round-trip + incremental BlobStream
// ---------------------------------------------------------------------------

TEST(DbBlob, BindAndReadBack_RoundTrips) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB)").has_value());
  const std::array<std::byte, 4> payload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                         std::byte{0xEF}};
  {
    auto ins = d.prepare("INSERT INTO t(id, data) VALUES (1, ?1)");
    ASSERT_TRUE(ins.has_value());
    ASSERT_TRUE(ins->bind(1, std::span<const std::byte>{payload}).has_value());
    ASSERT_TRUE(ins->step().has_value());
  }
  auto q = d.prepare("SELECT data FROM t WHERE id = 1");
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  const std::span<const std::byte> got = q->column_blob(0);
  ASSERT_EQ(got.size(), payload.size());
  EXPECT_EQ(std::memcmp(got.data(), payload.data(), payload.size()), 0);
}

TEST(DbBlob, BlobStream_ReadsExistingBlob) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB)").has_value());
  ASSERT_TRUE(d.exec("INSERT INTO t(id, data) VALUES (1, x'01020304')").has_value());

  auto bs = db::BlobStream::open(d, "t", "data", /*rowid=*/1, /*writable=*/false);
  ASSERT_TRUE(bs.has_value()) << (bs ? "" : bs.error().to_string());
  EXPECT_EQ(bs->size(), 4);
  std::array<std::byte, 4> buf{};
  auto n = bs->read(std::span<std::byte>{buf}, /*offset=*/0);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 4U);
  EXPECT_EQ(static_cast<unsigned>(buf[0]), 0x01U);
  EXPECT_EQ(static_cast<unsigned>(buf[3]), 0x04U);
}

TEST(DbBlob, BlobStream_WritesIntoZeroblob) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB)").has_value());
  ASSERT_TRUE(d.exec("INSERT INTO t(id, data) VALUES (1, zeroblob(4))").has_value());
  {
    auto bs = db::BlobStream::open(d, "t", "data", 1, /*writable=*/true);
    ASSERT_TRUE(bs.has_value());
    const std::array<std::byte, 4> payload{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},
                                           std::byte{0xDD}};
    ASSERT_TRUE(bs->write(std::span<const std::byte>{payload}, 0).has_value());
  }
  auto q = d.prepare("SELECT data FROM t WHERE id = 1");
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  EXPECT_EQ(static_cast<unsigned>(q->column_blob(0)[0]), 0xAAU);
}
