// Tests for the store connection policy (atx/core/db/connection.hpp, P9 SQL1).
//
// File databases under the per-process temp root (atx-test-scratch): the policy is about
// files (page size, WAL, identity pragmas, concurrent connections).

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/db/connection.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace db = atx::core::db;
using atx::i32;
using atx::i64;
using atx::core::ErrorCode;

namespace {

constexpr atx::u32 kAppId = 0x4154584BU; // "ATXK"

db::StorePolicy policy(i32 user_version = 1,
                       db::StorePolicy::Sync sync = db::StorePolicy::Sync::Normal) {
  db::StorePolicy p;
  p.application_id = kAppId;
  p.user_version = user_version;
  p.sync = sync;
  p.busy_timeout_ms = 30000;
  p.page_size = 8192;
  return p;
}

std::filesystem::path store_path(std::string_view suffix = {}) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto directory = std::filesystem::temp_directory_path() / "atx_core_db_connection_tests";
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

i64 scalar_int(db::Database &d, std::string_view sql) {
  auto stmt = d.prepare(sql);
  EXPECT_TRUE(stmt) << stmt.error().to_string();
  if (!stmt) {
    return -1;
  }
  auto step = stmt->step();
  EXPECT_TRUE(step && *step == db::Statement::Step::Row);
  return stmt->column_int(0);
}

std::string scalar_text(db::Database &d, std::string_view sql) {
  auto stmt = d.prepare(sql);
  EXPECT_TRUE(stmt) << stmt.error().to_string();
  if (!stmt) {
    return {};
  }
  auto step = stmt->step();
  EXPECT_TRUE(step && *step == db::Statement::Step::Row);
  return std::string{stmt->column_text(0)};
}

// Create a store the way a schema owner does: open, create its table and stamp it in one
// IMMEDIATE transaction.
void create_store(const std::filesystem::path &path, const db::StorePolicy &p) {
  auto opened = db::open_with_policy(path.string(), db::OpenMode::ReadWriteCreate, p);
  ASSERT_TRUE(opened) << opened.error().to_string();
  ASSERT_TRUE(opened->created);
  auto made = db::with_immediate(opened->db, [&](db::Database &d) -> atx::core::Status {
    ATX_TRY_VOID(d.exec("CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT NOT NULL) STRICT"));
    return db::stamp_identity(d, p);
  });
  ASSERT_TRUE(made) << made.error().to_string();
}

} // namespace

TEST(DbConnection, OpenWithPolicyStampsAppIdAndVersion) {
  const auto path = store_path();
  create_store(path, policy(3));
  auto reopened = db::open_with_policy(path.string(), db::OpenMode::ReadWrite, policy(3));
  ASSERT_TRUE(reopened) << reopened.error().to_string();
  EXPECT_FALSE(reopened->created);
  EXPECT_EQ(reopened->user_version, 3);
  auto app = db::read_application_id(reopened->db);
  ASSERT_TRUE(app);
  EXPECT_EQ(*app, kAppId);
  auto version = db::read_user_version(reopened->db);
  ASSERT_TRUE(version);
  EXPECT_EQ(*version, 3);
  EXPECT_EQ(scalar_int(reopened->db, "PRAGMA foreign_keys"), 1);
  EXPECT_EQ(scalar_int(reopened->db, "PRAGMA trusted_schema"), 0);
  EXPECT_EQ(scalar_int(reopened->db, "PRAGMA synchronous"), 1); // NORMAL
  // An older schema is opened as-is: the owner migrates it.
  auto newer_build = db::open_with_policy(path.string(), db::OpenMode::ReadWrite, policy(5));
  ASSERT_TRUE(newer_build) << newer_build.error().to_string();
  EXPECT_FALSE(newer_build->created);
  EXPECT_EQ(newer_build->user_version, 3);
}

TEST(DbConnection, RefusesForeignApplicationId) {
  const auto path = store_path();
  db::StorePolicy other = policy();
  other.application_id = 0x41545843U; // "ATXC": a catalog, not a cache index
  create_store(path, other);
  auto opened = db::open_with_policy(path.string(), db::OpenMode::ReadWrite, policy());
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(opened.error().message().find("0x41545843"), std::string::npos)
      << opened.error().message();

  // A non-empty file nobody stamped is foreign too.
  const auto plain = store_path("plain");
  {
    auto raw = db::Database::open(plain.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("CREATE TABLE x(a INTEGER)"));
  }
  auto plain_opened = db::open_with_policy(plain.string(), db::OpenMode::ReadWrite, policy());
  ASSERT_FALSE(plain_opened);
  EXPECT_EQ(plain_opened.error().code(), ErrorCode::InvalidArgument);
}

TEST(DbConnection, RefusesNewerUserVersion) {
  const auto path = store_path();
  create_store(path, policy(2));
  auto opened = db::open_with_policy(path.string(), db::OpenMode::ReadWrite, policy(1));
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code(), ErrorCode::NotImplemented);
  EXPECT_NE(opened.error().message().find("cannot read schema 2"), std::string::npos)
      << opened.error().message();
}

TEST(DbConnection, RefusesUncPath) {
  for (const std::string_view path : {std::string_view{"\\\\server\\share\\store.sqlite"},
                                      std::string_view{"//server/share/s.db"},
                                      std::string_view{"\\\\?\\C:\\store.sqlite"}}) {
    auto opened = db::open_with_policy(path, db::OpenMode::ReadWriteCreate, policy());
    ASSERT_FALSE(opened) << path;
    EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument) << path;
  }
}

TEST(DbConnection, NewFilePageSizeThenWal) {
  const auto path = store_path();
  create_store(path, policy());
  auto raw = db::Database::open(path.string(), db::OpenMode::ReadOnly);
  ASSERT_TRUE(raw) << raw.error().to_string();
  EXPECT_EQ(scalar_int(*raw, "PRAGMA page_size"), 8192);
  EXPECT_EQ(scalar_text(*raw, "PRAGMA journal_mode"), "wal");

  // An empty file opened read-only is not a store yet.
  const auto empty = store_path("empty");
  { ASSERT_TRUE(db::Database::open(empty.string())); }
  auto read_only = db::open_with_policy(empty.string(), db::OpenMode::ReadOnly, policy());
  ASSERT_FALSE(read_only);
  EXPECT_EQ(read_only.error().code(), ErrorCode::NotFound);

  // A policy page size that is not a power of two is refused before any file is touched.
  db::StorePolicy bad = policy();
  bad.page_size = 5000;
  auto refused =
      db::open_with_policy(store_path("bad").string(), db::OpenMode::ReadWriteCreate, bad);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
}

TEST(DbConnection, ImmediateWritersSerialiseAcrossThreads) {
  const auto path = store_path();
  create_store(path, policy());
  constexpr i64 kPerThread = 500;
  std::atomic<i32> failures{0};
  auto writer = [&](i64 base) {
    auto opened = db::open_with_policy(path.string(), db::OpenMode::ReadWrite, policy());
    if (!opened) {
      failures.fetch_add(1);
      return;
    }
    for (i64 i = 0; i < kPerThread; ++i) {
      auto written = db::with_immediate(opened->db, [&](db::Database &d) -> atx::core::Status {
        // Read-then-write inside the transaction: safe only because IMMEDIATE holds the
        // write lock from BEGIN (no deferred upgrade that could fail with BUSY_SNAPSHOT).
        ATX_TRY(db::Statement count, d.prepare("SELECT count(*) FROM t"));
        ATX_TRY(const db::Statement::Step step, count.step());
        (void)step;
        ATX_TRY(db::Statement insert, d.prepare("INSERT INTO t(k, v) VALUES (?1, ?2)"));
        ATX_TRY_VOID(insert.bind(1, base + i));
        ATX_TRY_VOID(insert.bind(2, std::string_view{"row"}));
        ATX_TRY(const db::Statement::Step done, insert.step());
        (void)done;
        return atx::core::Ok();
      });
      if (!written) {
        failures.fetch_add(1);
      }
    }
  };
  {
    std::jthread a{writer, i64{0}};
    std::jthread b{writer, i64{1'000'000}};
  }
  EXPECT_EQ(failures.load(), 0);
  auto check = db::open_with_policy(path.string(), db::OpenMode::ReadWrite, policy());
  ASSERT_TRUE(check) << check.error().to_string();
  EXPECT_EQ(scalar_int(check->db, "SELECT count(*) FROM t"), 2 * kPerThread);
  EXPECT_EQ(scalar_int(check->db, "SELECT count(*) FROM t WHERE k < 1000000"), kPerThread);
}

TEST(DbConnection, QuickCheckOk) {
  const auto path = store_path();
  create_store(path, policy());
  auto opened = db::open_with_policy(path.string(), db::OpenMode::ReadWrite, policy());
  ASSERT_TRUE(opened) << opened.error().to_string();
  ASSERT_TRUE(opened->db.exec("INSERT INTO t(k, v) VALUES (1, 'a'), (2, 'b')"));
  auto checked = db::quick_check(opened->db);
  ASSERT_TRUE(checked) << checked.error().to_string();
  EXPECT_EQ(*checked, "ok");
  auto truncated = db::checkpoint_truncate(opened->db);
  ASSERT_TRUE(truncated) << truncated.error().to_string();
  std::error_code ec;
  EXPECT_EQ(std::filesystem::file_size(path.string() + "-wal", ec), 0U);
}
