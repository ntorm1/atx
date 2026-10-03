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

// ---------------------------------------------------------------------------
//  P9 SQL1: wrapper defects W1-W6 fixed in place
// ---------------------------------------------------------------------------

namespace {

// Step a one-statement INSERT and return its error code (Unknown when it succeeded).
atx::core::ErrorCode insert_error(db::Database &d, std::string_view sql) {
  auto stmt = d.prepare(sql);
  EXPECT_TRUE(stmt) << stmt.error().to_string();
  if (!stmt) {
    return atx::core::ErrorCode::Unknown;
  }
  auto step = stmt->step();
  return step ? atx::core::ErrorCode::Unknown : step.error().code();
}

} // namespace

TEST(DbSqlite, PrepareRefusesTrailingStatement) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER)"));
  auto two = d.prepare("INSERT INTO t(id) VALUES (1); INSERT INTO t(id) VALUES (2)");
  ASSERT_FALSE(two);
  EXPECT_EQ(two.error().code(), atx::core::ErrorCode::ParseError);
  auto trailing_garbage = d.prepare("SELECT 1; x");
  ASSERT_FALSE(trailing_garbage);
  EXPECT_EQ(trailing_garbage.error().code(), atx::core::ErrorCode::ParseError);
  // Nothing ran: the refused statement was finalized, not stepped.
  auto count = d.prepare("SELECT count(*) FROM t");
  ASSERT_TRUE(count);
  ASSERT_EQ(*count->step(), db::Statement::Step::Row);
  EXPECT_EQ(count->column_int(0), 0);
  // Multi-statement SQL still runs through exec.
  ASSERT_TRUE(d.exec("INSERT INTO t(id) VALUES (1); INSERT INTO t(id) VALUES (2)"));
}

TEST(DbSqlite, PrepareAllowsTrailingComment) {
  db::Database d = open_mem();
  for (const std::string_view sql :
       {std::string_view{"SELECT 7;"}, std::string_view{"SELECT 7;  \n\t "},
        std::string_view{"SELECT 7; -- trailing note"}, std::string_view{"SELECT 7 /* a */ ;"},
        std::string_view{"SELECT 7; /* block */ -- line\n"},
        std::string_view{"SELECT 7; /* unterminated"}}) {
    auto stmt = d.prepare(sql);
    ASSERT_TRUE(stmt) << sql << ": " << stmt.error().to_string();
    ASSERT_EQ(*stmt->step(), db::Statement::Step::Row);
    EXPECT_EQ(stmt->column_int(0), 7);
  }
}

TEST(DbSqlite, UniqueViolationIsAlreadyExists) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(k TEXT PRIMARY KEY, u TEXT UNIQUE) STRICT"));
  ASSERT_TRUE(d.exec("INSERT INTO t(k, u) VALUES ('a', 'x')"));
  EXPECT_EQ(insert_error(d, "INSERT INTO t(k, u) VALUES ('a', 'y')"),
            atx::core::ErrorCode::AlreadyExists); // PRIMARY KEY
  EXPECT_EQ(insert_error(d, "INSERT INTO t(k, u) VALUES ('b', 'x')"),
            atx::core::ErrorCode::AlreadyExists); // UNIQUE
  ASSERT_TRUE(d.exec("CREATE TABLE w(k TEXT NOT NULL, PRIMARY KEY(k)) STRICT, WITHOUT ROWID"));
  ASSERT_TRUE(d.exec("INSERT INTO w(k) VALUES ('a')"));
  EXPECT_EQ(insert_error(d, "INSERT INTO w(k) VALUES ('a')"),
            atx::core::ErrorCode::AlreadyExists); // WITHOUT ROWID primary key
}

TEST(DbSqlite, CheckNotNullFkViolationsAreInvalidArgument) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("PRAGMA foreign_keys = ON"));
  ASSERT_TRUE(d.exec(
      "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
      "CREATE TABLE child(id INTEGER PRIMARY KEY, parent INTEGER REFERENCES parent(id),"
      " flag INTEGER NOT NULL CHECK(flag IN (0, 1)), n INTEGER) STRICT;"
      "CREATE TRIGGER child_no_delete BEFORE DELETE ON child"
      " BEGIN SELECT RAISE(ABORT, 'child is append-only'); END;"
      "INSERT INTO parent(id) VALUES (1);"
      "INSERT INTO child(id, parent, flag) VALUES (1, 1, 0);"));
  EXPECT_EQ(insert_error(d, "INSERT INTO child(id, parent, flag) VALUES (2, 1, 7)"),
            atx::core::ErrorCode::InvalidArgument); // CHECK
  EXPECT_EQ(insert_error(d, "INSERT INTO child(id, parent, flag) VALUES (3, 1, NULL)"),
            atx::core::ErrorCode::InvalidArgument); // NOT NULL
  EXPECT_EQ(insert_error(d, "INSERT INTO child(id, parent, flag) VALUES (4, 99, 0)"),
            atx::core::ErrorCode::InvalidArgument); // FOREIGN KEY
  EXPECT_EQ(insert_error(d, "DELETE FROM child WHERE id = 1"),
            atx::core::ErrorCode::InvalidArgument); // TRIGGER RAISE(ABORT)
  EXPECT_EQ(insert_error(d, "INSERT INTO child(id, parent, flag, n) VALUES (5, 1, 0, 'x')"),
            atx::core::ErrorCode::InvalidArgument); // STRICT datatype
}

TEST(DbSqlite, CheckedReadersRejectNullAndWrongClass) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(i INTEGER, f REAL, s TEXT, b BLOB, n INTEGER)"));
  ASSERT_TRUE(d.exec("INSERT INTO t VALUES (5, 2.5, 'txt', x'0102', NULL)"));
  auto q = d.prepare("SELECT i, f, s, b, n FROM t");
  ASSERT_TRUE(q);
  // Before the first step there is no row: out of range, not a silent 0.
  auto early = q->checked_int(0);
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error().code(), atx::core::ErrorCode::OutOfRange);
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);

  ASSERT_TRUE(q->checked_int(0));
  EXPECT_EQ(*q->checked_int(0), 5);
  EXPECT_EQ(*q->checked_double(1), 2.5);
  EXPECT_EQ(*q->checked_text(2), "txt");
  const auto blob = q->checked_blob(3);
  ASSERT_TRUE(blob);
  ASSERT_EQ(blob->size(), 2U);
  EXPECT_EQ((*blob)[1], std::byte{0x02});

  const auto is_invalid = [](const auto &result) {
    return !result && result.error().code() == atx::core::ErrorCode::InvalidArgument;
  };
  EXPECT_TRUE(is_invalid(q->checked_int(4)));    // NULL
  EXPECT_TRUE(is_invalid(q->checked_double(4))); // NULL
  EXPECT_TRUE(is_invalid(q->checked_text(4)));   // NULL
  EXPECT_TRUE(is_invalid(q->checked_blob(4)));   // NULL
  EXPECT_TRUE(is_invalid(q->checked_int(1)));    // REAL as int
  EXPECT_TRUE(is_invalid(q->checked_int(2)));    // TEXT as int
  EXPECT_TRUE(is_invalid(q->checked_double(2))); // TEXT as double
  EXPECT_TRUE(is_invalid(q->checked_text(3)));   // BLOB as text
  EXPECT_TRUE(is_invalid(q->checked_blob(2)));   // TEXT as blob
  EXPECT_TRUE(is_invalid(q->checked_text(0)));   // INTEGER as text
  for (const atx::i32 bad : {-1, 5, 99}) {
    auto out = q->checked_text(bad);
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code(), atx::core::ErrorCode::OutOfRange) << bad;
  }
  // The unchecked readers keep their documented behaviour.
  EXPECT_EQ(q->column_int(4), 0);
  EXPECT_TRUE(q->column_text(4).empty());
}

TEST(DbSqlite, CheckedDoubleAcceptsInteger) {
  db::Database d = open_mem();
  auto q = d.prepare("SELECT 3, 9007199254740993");
  ASSERT_TRUE(q);
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  auto three = q->checked_double(0);
  ASSERT_TRUE(three) << three.error().to_string();
  EXPECT_EQ(*three, 3.0);
  auto big = q->checked_double(1);
  ASSERT_TRUE(big);
  EXPECT_EQ(*big, static_cast<f64>(9007199254740993LL)); // SQLite's own int -> double
}

TEST(DbSqlite, BlobStreamRangeChecked) {
  db::Database d = open_mem();
  ASSERT_TRUE(d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB)"));
  ASSERT_TRUE(d.exec("INSERT INTO t(id, data) VALUES (1, zeroblob(8))"));
  auto bs = db::BlobStream::open(d, "t", "data", 1, /*writable=*/true);
  ASSERT_TRUE(bs) << bs.error().to_string();
  std::array<std::byte, 4> buf{};
  const auto out_of_range = [](const auto &result) {
    return !result && result.error().code() == atx::core::ErrorCode::OutOfRange;
  };
  EXPECT_TRUE(out_of_range(bs->read(std::span<std::byte>{buf}, -1)));
  EXPECT_TRUE(out_of_range(bs->read(std::span<std::byte>{buf}, 5)));     // 5 + 4 > 8
  EXPECT_TRUE(out_of_range(bs->read(std::span<std::byte>{buf}, i64{1} << 40)));
  EXPECT_TRUE(out_of_range(bs->write(std::span<const std::byte>{buf}, -3)));
  EXPECT_TRUE(out_of_range(bs->write(std::span<const std::byte>{buf}, 6)));
  EXPECT_TRUE(bs->read(std::span<std::byte>{buf}, 4));                  // exactly the tail
  EXPECT_TRUE(bs->write(std::span<const std::byte>{buf}, 0));
  EXPECT_TRUE(bs->read(std::span<std::byte>{}, 8));                     // empty at the end
}

TEST(DbSqlite, TransactionDestructorRollsBack) {
  const auto path = database_path();
  auto d = db::Database::open(path.string());
  ASSERT_TRUE(d);
  ASSERT_TRUE(d->pragma("journal_mode", "WAL"));
  ASSERT_TRUE(d->exec("CREATE TABLE t(id INTEGER)"));
  {
    auto tx = db::Transaction::begin_immediate(*d);
    ASSERT_TRUE(tx);
    ASSERT_TRUE(d->exec("INSERT INTO t(id) VALUES (1)"));
  }
  {
    // Move-assigning over an open guard rolls that transaction back as well.
    auto first = db::Transaction::begin(*d);
    ASSERT_TRUE(first);
    ASSERT_TRUE(d->exec("INSERT INTO t(id) VALUES (2)"));
    db::Transaction live = std::move(*first); // *first is inert from here
    live = std::move(*first);                 // inert over open: the open one rolls back now
    auto again = db::Transaction::begin_immediate(*d);
    ASSERT_TRUE(again) << again.error().to_string(); // so no transaction was left open
  }
  // No transaction is left open: a second connection can take the write lock at once.
  auto other = db::Database::open(path.string());
  ASSERT_TRUE(other);
  ASSERT_TRUE(other->exec("BEGIN IMMEDIATE"));
  ASSERT_TRUE(other->exec("ROLLBACK"));
  auto q = d->prepare("SELECT count(*) FROM t");
  ASSERT_TRUE(q);
  ASSERT_EQ(*q->step(), db::Statement::Step::Row);
  EXPECT_EQ(q->column_int(0), 0);
}
