// Tests for the atx::core::db SQLite wrapper (RAII + Result over the C API).
//
// Tests prefer private in-memory databases. The online-backup concurrency cases
// use per-test temporary files because independent connections are part of the
// contract under test.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <latch>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

static std::filesystem::path database_path(std::string_view suffix = {}) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto directory = std::filesystem::temp_directory_path() / "atx_core_db_tests";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  auto name = std::string{info->test_suite_name()} + "_" + info->name();
  if (!suffix.empty()) {
    name += "_";
    name += suffix;
  }
  const auto path = directory / (name + ".sqlite");
  std::filesystem::remove(path, error);
  std::filesystem::remove(path.string() + "-wal", error);
  std::filesystem::remove(path.string() + "-shm", error);
  return path;
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

TEST(DbStatement, DefaultConstructedEmptyStringViewBindsAsEmptyText) {
  auto opened = db::Database::open_memory();
  ASSERT_TRUE(opened) << opened.error().to_string();
  ASSERT_TRUE(opened->exec("CREATE TABLE values_(value TEXT NOT NULL) STRICT"));
  auto insert = opened->prepare("INSERT INTO values_(value) VALUES(?1)");
  ASSERT_TRUE(insert) << insert.error().to_string();
  const std::string_view empty;
  ASSERT_TRUE(insert->bind(1, empty));
  auto inserted = insert->step();
  ASSERT_TRUE(inserted) << inserted.error().to_string();
  EXPECT_EQ(*inserted, db::Statement::Step::Done);
  auto query = opened->prepare("SELECT value,typeof(value),length(value) FROM values_");
  ASSERT_TRUE(query) << query.error().to_string();
  auto row = query->step();
  ASSERT_TRUE(row) << row.error().to_string();
  ASSERT_EQ(*row, db::Statement::Step::Row);
  EXPECT_EQ(query->column_text(0), "");
  EXPECT_EQ(query->column_text(1), "text");
  EXPECT_EQ(query->column_int(2), 0);
}

TEST(DbStatement, DefaultConstructedEmptySpanBindsAsEmptyBlob) {
  auto opened = db::Database::open_memory();
  ASSERT_TRUE(opened) << opened.error().to_string();
  ASSERT_TRUE(opened->exec("CREATE TABLE values_(value BLOB NOT NULL) STRICT"));
  auto insert = opened->prepare("INSERT INTO values_(value) VALUES(?1)");
  ASSERT_TRUE(insert) << insert.error().to_string();
  const std::span<const std::byte> empty;
  ASSERT_TRUE(insert->bind(1, empty));
  auto inserted = insert->step();
  ASSERT_TRUE(inserted) << inserted.error().to_string();
  EXPECT_EQ(*inserted, db::Statement::Step::Done);
  auto query = opened->prepare("SELECT typeof(value),length(value) FROM values_");
  ASSERT_TRUE(query) << query.error().to_string();
  auto row = query->step();
  ASSERT_TRUE(row) << row.error().to_string();
  ASSERT_EQ(*row, db::Statement::Step::Row);
  EXPECT_EQ(query->column_text(0), "blob");
  EXPECT_EQ(query->column_int(1), 0);
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

TEST(DbDatabase, OnlineBackupReportsCompletionAndCopiesCommittedContent) {
  db::Database source = open_mem();
  db::Database destination = open_mem();
  ASSERT_TRUE(source.exec("CREATE TABLE records(id INTEGER PRIMARY KEY,value TEXT NOT NULL)"));
  ASSERT_TRUE(source.exec(
      "WITH RECURSIVE n(value) AS (VALUES(1) UNION ALL SELECT value+1 FROM n WHERE value<100) "
      "INSERT INTO records(id,value) SELECT value,printf('record-%d',value) FROM n"));
  db::BackupOptions options;
  options.pages_per_step = 1;
  auto backed_up = source.backup_to(destination, options);
  ASSERT_TRUE(backed_up) << backed_up.error().to_string();
  EXPECT_GT(backed_up->page_count, 0);
  EXPECT_EQ(backed_up->remaining_pages, 0);
  EXPECT_GT(backed_up->steps, 0);
  auto count = destination.prepare("SELECT count(*) FROM records");
  ASSERT_TRUE(count);
  ASSERT_EQ(*count->step(), db::Statement::Step::Row);
  EXPECT_EQ(count->column_int(0), 100);
}

TEST(DbDatabase, OnlineBackupDoesNotReportBusyDestinationAsSuccess) {
  db::Database source = open_mem();
  ASSERT_TRUE(source.exec("CREATE TABLE source_record(value TEXT NOT NULL)"));
  ASSERT_TRUE(source.exec("INSERT INTO source_record VALUES('must not partially replace')"));
  const auto destination_path = database_path("destination");
  auto destination = db::Database::open(destination_path.string());
  auto blocker = db::Database::open(destination_path.string());
  ASSERT_TRUE(destination);
  ASSERT_TRUE(blocker);
  ASSERT_TRUE(destination->exec("CREATE TABLE marker(value TEXT NOT NULL)"));
  ASSERT_TRUE(destination->exec("INSERT INTO marker VALUES('preserved')"));
  ASSERT_TRUE(blocker->exec("BEGIN IMMEDIATE"));
  ASSERT_TRUE(blocker->exec("UPDATE marker SET value='uncommitted'"));
  db::BackupOptions options;
  options.pages_per_step = 1;
  options.maximum_busy_retries = 0;
  options.retry_delay_ms = 0;
  auto backed_up = source.backup_to(*destination, options);
  ASSERT_FALSE(backed_up);
  EXPECT_EQ(backed_up.error().code(), atx::core::ErrorCode::Unavailable);
  ASSERT_TRUE(blocker->exec("ROLLBACK"));
  auto marker = destination->prepare("SELECT value FROM marker");
  ASSERT_TRUE(marker);
  ASSERT_EQ(*marker->step(), db::Statement::Step::Row);
  EXPECT_EQ(marker->column_text(0), "preserved");
}

TEST(DbDatabase, OnlineBackupConvergesToAConsistentSnapshotDuringWalWrite) {
  const auto source_path = database_path("source");
  const auto destination_path = database_path("destination");
  auto source = db::Database::open(source_path.string());
  auto destination = db::Database::open(destination_path.string());
  ASSERT_TRUE(source);
  ASSERT_TRUE(destination);
  ASSERT_TRUE(source->pragma("journal_mode", "WAL"));
  ASSERT_TRUE(source->pragma("synchronous", "FULL"));
  ASSERT_TRUE(source->exec(
      "CREATE TABLE records(id INTEGER PRIMARY KEY,payload BLOB NOT NULL);"
      "WITH RECURSIVE n(value) AS (VALUES(1) UNION ALL SELECT value+1 FROM n WHERE value<512) "
      "INSERT INTO records(id,payload) SELECT value,zeroblob(4096) FROM n"));

  std::latch ready{1};
  std::latch start{1};
  std::atomic<bool> writer_succeeded{false};
  std::jthread writer{[&] {
    auto connection = db::Database::open(source_path.string());
    ready.count_down();
    start.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    if (connection && connection->set_busy_timeout(5'000) &&
        connection->exec("INSERT INTO records(id,payload) VALUES(513,zeroblob(4096))")) {
      writer_succeeded.store(true);
    }
  }};
  ready.wait();
  start.count_down();
  db::BackupOptions options;
  options.pages_per_step = 1;
  options.step_delay_ms = 1;
  auto backed_up = source->backup_to(*destination, options);
  writer.join();
  ASSERT_TRUE(writer_succeeded.load());
  ASSERT_TRUE(backed_up) << backed_up.error().to_string();
  EXPECT_GT(backed_up->steps, 100);
  auto integrity = destination->prepare("PRAGMA integrity_check");
  ASSERT_TRUE(integrity);
  ASSERT_EQ(*integrity->step(), db::Statement::Step::Row);
  EXPECT_EQ(integrity->column_text(0), "ok");
  auto count = destination->prepare("SELECT count(*) FROM records");
  ASSERT_TRUE(count);
  ASSERT_EQ(*count->step(), db::Statement::Step::Row);
  EXPECT_EQ(count->column_int(0), 513);
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
