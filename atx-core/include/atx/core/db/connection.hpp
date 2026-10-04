#pragma once

// atx::core::db -- the store connection policy (P9 SQL1, sql-design section 3.3).
//
// Database::open (sqlite.hpp) opens a file and nothing more. A research store
// (catalog, cache index) opens through open_with_policy instead, which applies one
// fixed set of settings and refuses a file that is not this policy's store:
//
//   path        refused when it starts with `\\` or `//` (UNC) or names a drive
//               Windows reports as DRIVE_REMOTE (WAL needs shared memory on one host)
//   hardening   SQLITE_DBCONFIG_DEFENSIVE = 1, trusted_schema = OFF (a crafted schema
//               cannot run unsafe functions or write the schema table)
//   busy        busy_timeout = policy.busy_timeout_ms
//   new file    (application_id 0, user_version 0, no schema object): page_size,
//               then journal_mode = WAL (must report "wal"), then synchronous and
//               foreign_keys = ON. application_id / user_version are NOT stamped here:
//               the caller stamps them in the same transaction that creates its schema
//               (stamp_identity), so a crash can never leave a stamped, empty store.
//   existing    application_id != policy's -> Err(InvalidArgument);
//               user_version > policy's -> Err(NotImplemented) ("this build cannot read
//               schema N"); otherwise synchronous and foreign_keys = ON per connection
//
// Threading: as Database (one connection per thread; build is SQLITE_THREADSAFE=2).

#include <string>
#include <string_view>
#include <type_traits>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core::db {

struct StorePolicy {
  enum class Sync : u8 { Normal, Full };

  u32 application_id{};      // PRAGMA application_id (stored as its signed 32-bit pattern)
  i32 user_version{};        // the newest schema version this build reads and writes
  Sync sync{Sync::Full};     // PRAGMA synchronous
  i32 busy_timeout_ms{30000};
  i32 page_size{8192};       // applied to a new, empty file only
};

struct OpenedStore {
  Database db;
  bool created{};    // the file was empty: the caller creates the schema and stamps it
  i32 user_version{}; // the file's user_version as opened (0 when created)
};

// Open `path` under `policy` (see the header comment for every step).
// @param path  UTF-8 file path; ":memory:" is refused by the WAL step (Err(IoError)).
// @param mode  ReadOnly opens an existing store without changing its journal mode; an
//              empty file opened ReadOnly is Err(NotFound).
// @return the open connection and what was found; Err(InvalidArgument) for a UNC /
//         network path, a foreign application_id or a non-empty file without one;
//         Err(NotImplemented) for a newer user_version; Err(IoError) when WAL cannot be
//         set; any open / pragma error as Database reports it.
// Thread-safety: the returned Database belongs to the calling thread.
[[nodiscard]] Result<OpenedStore> open_with_policy(std::string_view path, OpenMode mode,
                                                   const StorePolicy &policy);

// Write PRAGMA application_id and user_version from `policy`. Both live in the database
// header and are transactional: call inside the transaction that creates or migrates the
// schema. Err as Database::exec.
[[nodiscard]] Status stamp_identity(Database &db, const StorePolicy &policy);

// Read PRAGMA application_id (as its unsigned 32-bit pattern) / user_version.
[[nodiscard]] Result<u32> read_application_id(Database &db);
[[nodiscard]] Result<i32> read_user_version(Database &db);

// PRAGMA wal_checkpoint(TRUNCATE): copy the WAL into the database and truncate it.
// Err(Unavailable) when a reader or writer kept the checkpoint from completing.
[[nodiscard]] Status checkpoint_truncate(Database &db);

// PRAGMA quick_check: "ok", or the first problem row SQLite reports.
[[nodiscard]] Result<std::string> quick_check(Database &db);

// with_immediate retry bound: BEGIN IMMEDIATE that still meets SQLITE_BUSY after the
// busy handler (busy_timeout) is retried this many times, kImmediateRetryDelayMs apart.
inline constexpr i32 kImmediateRetries = 3;
inline constexpr i32 kImmediateRetryDelayMs = 50;

namespace detail {
// Sleep kImmediateRetryDelayMs (non-template, so the header needs no <thread>).
void immediate_retry_pause() noexcept;
} // namespace detail

// Run `fn(db)` inside BEGIN IMMEDIATE and commit it. When the attempt fails with
// Err(Unavailable) (SQLITE_BUSY / SQLITE_LOCKED after the busy handler gave up) the
// transaction is rolled back and the whole attempt is repeated, at most kImmediateRetries
// times, kImmediateRetryDelayMs apart; any other error is returned at once (rolled back).
// Precondition: no transaction is open on `db`. `fn` returns Status and may run up to
// kImmediateRetries + 1 times, so it must not have effects outside the database.
template <class Fn>
  requires std::is_invocable_r_v<Status, Fn &, Database &>
[[nodiscard]] Status with_immediate(Database &db, Fn &&fn) {
  Status last = Ok();
  for (i32 attempt = 0; attempt <= kImmediateRetries; ++attempt) {
    if (attempt > 0) {
      detail::immediate_retry_pause();
    }
    last = [&]() -> Status {
      ATX_TRY(Transaction txn, Transaction::begin_immediate(db));
      ATX_TRY_VOID(fn(db));
      return txn.commit();
    }();
    if (last.has_value() || last.error().code() != ErrorCode::Unavailable) {
      return last;
    }
  }
  return last;
}

} // namespace atx::core::db
