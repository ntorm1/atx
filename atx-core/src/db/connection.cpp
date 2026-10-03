// The store connection policy (P9 SQL1). See include/atx/core/db/connection.hpp.
//
// One of the two atx-core TUs that include <sqlite3.h> (with sqlite.cpp): the hardening
// switches are connection configuration options with no PRAGMA-only equivalent for
// DEFENSIVE.

#include "atx/core/db/connection.hpp"

#include <bit>
#include <exception>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sqlite3.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace atx::core::db {
namespace {

#if defined(_WIN32)
// The root name (e.g. L"C:", or L"\\\\server" for a share) of a UTF-8 path resolved through
// links: weakly_canonical follows the existing prefix through junctions and symlinks, so a
// link to a share is seen as the share. A final-path "\\?\" prefix is dropped ("\\?\UNC\" is
// a share). Invalid UTF-8 or an unresolvable path is an error, never a throw
// (std::filesystem::path throws on invalid UTF-8 on MSVC).
[[nodiscard]] Result<std::wstring> resolved_root(std::string_view text) {
  try {
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(
        std::filesystem::path{std::u8string{text.begin(), text.end()}}, ec);
    if (ec) {
      return Err(ErrorCode::IoError,
                 "store path cannot be resolved: " + std::string{text} + ": " + ec.message());
    }
    std::wstring native = resolved.native();
    if (native.starts_with(L"\\\\?\\UNC\\")) {
      return Ok(std::wstring{L"\\\\"});
    }
    if (native.starts_with(L"\\\\?\\")) {
      native.erase(0, 4);
    }
    return Ok(std::filesystem::path{native}.root_name().wstring());
  } catch (const std::exception &) {
    return Err(ErrorCode::InvalidArgument, "store path is not valid UTF-8: " + std::string{text});
  }
}
#endif

// WAL keeps its index in shared memory, which works on one host only.
[[nodiscard]] Status refuse_remote_path(std::string_view path) {
  if (path.starts_with("\\\\") || path.starts_with("//")) {
    return Err(ErrorCode::InvalidArgument,
               "store path refused (UNC path; WAL needs a local disk): " + std::string{path});
  }
#if defined(_WIN32)
  ATX_TRY(const std::wstring root, resolved_root(path));
  if (root.starts_with(L"\\\\") || root.starts_with(L"//")) {
    return Err(ErrorCode::InvalidArgument,
               "store path refused (resolves to a UNC path): " + std::string{path});
  }
  if (root.size() == 2 && root[1] == L':') {
    const std::wstring drive = root + L"\\";
    if (GetDriveTypeW(drive.c_str()) == DRIVE_REMOTE) {
      return Err(ErrorCode::InvalidArgument,
                 "store path refused (network drive; WAL needs a local disk): " +
                     std::string{path});
    }
  }
#endif
  return Ok();
}

[[nodiscard]] Status validate_policy(const StorePolicy &policy) {
  const bool page_ok = policy.page_size >= 512 && policy.page_size <= 65536 &&
                       std::has_single_bit(static_cast<u32>(policy.page_size));
  if (!page_ok || policy.user_version < 0 || policy.busy_timeout_ms < 0) {
    return Err(ErrorCode::InvalidArgument,
               "store policy: page_size must be a power of two in [512, 65536], user_version and "
               "busy_timeout_ms non-negative");
  }
  return Ok();
}

// First column of the first row of a one-row statement (PRAGMA reads, counts).
[[nodiscard]] Result<i64> query_int(Database &db, std::string_view sql) {
  ATX_TRY(Statement stmt, db.prepare(sql));
  ATX_TRY(const Statement::Step step, stmt.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "no row from: " + std::string{sql});
  }
  return stmt.checked_int(0);
}

[[nodiscard]] Result<std::string> query_text(Database &db, std::string_view sql) {
  ATX_TRY(Statement stmt, db.prepare(sql));
  ATX_TRY(const Statement::Step step, stmt.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "no row from: " + std::string{sql});
  }
  return stmt.checked_text(0);
}

[[nodiscard]] Status harden(Database &db) {
  sqlite3 *const handle = db.handle();
  int applied = 0;
  if (sqlite3_db_config(handle, SQLITE_DBCONFIG_DEFENSIVE, 1, &applied) != SQLITE_OK ||
      applied != 1) {
    return Err(ErrorCode::Internal, "cannot enable SQLITE_DBCONFIG_DEFENSIVE");
  }
  if (sqlite3_db_config(handle, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &applied) != SQLITE_OK ||
      applied != 0) {
    return Err(ErrorCode::Internal, "cannot set trusted_schema = OFF");
  }
  return Ok();
}

[[nodiscard]] Status set_wal(Database &db) {
  ATX_TRY(const std::string mode, query_text(db, "PRAGMA journal_mode = WAL"));
  if (mode != "wal") {
    return Err(ErrorCode::IoError, "journal_mode = WAL refused; the store reports '" + mode + "'");
  }
  return Ok();
}

[[nodiscard]] Status set_connection_pragmas(Database &db, const StorePolicy &policy) {
  ATX_TRY_VOID(
      db.pragma("synchronous", policy.sync == StorePolicy::Sync::Full ? "FULL" : "NORMAL"));
  return db.pragma("foreign_keys", "ON");
}

[[nodiscard]] std::string hex32(u32 value) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out = "0x00000000";
  for (usize i = 0; i < 8; ++i) {
    out[9 - i] = kDigits[(value >> (4 * i)) & 0xFU];
  }
  return out;
}

} // namespace

Result<u32> read_application_id(Database &db) {
  ATX_TRY(const i64 value, query_int(db, "PRAGMA application_id"));
  if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) {
    return Err(ErrorCode::Internal, "application_id outside 32 bits");
  }
  return Ok(std::bit_cast<u32>(static_cast<i32>(value)));
}

Result<i32> read_user_version(Database &db) {
  ATX_TRY(const i64 value, query_int(db, "PRAGMA user_version"));
  if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) {
    return Err(ErrorCode::Internal, "user_version outside 32 bits");
  }
  return Ok(static_cast<i32>(value));
}

Status stamp_identity(Database &db, const StorePolicy &policy) {
  ATX_TRY_VOID(db.pragma("application_id",
                         std::to_string(std::bit_cast<i32>(policy.application_id))));
  return db.pragma("user_version", std::to_string(policy.user_version));
}

Result<OpenedStore> open_with_policy(std::string_view path, OpenMode mode,
                                     const StorePolicy &policy) {
  ATX_TRY_VOID(validate_policy(policy));
  ATX_TRY_VOID(refuse_remote_path(path));
  ATX_TRY(Database db, Database::open(path, mode));
  ATX_TRY_VOID(harden(db));
  ATX_TRY_VOID(db.set_busy_timeout(policy.busy_timeout_ms));
  ATX_TRY(const u32 application_id, read_application_id(db));
  ATX_TRY(const i32 user_version, read_user_version(db));
  ATX_TRY(const i64 objects, query_int(db, "SELECT count(*) FROM sqlite_schema"));

  if (application_id == 0 && user_version == 0 && objects == 0) {
    if (mode == OpenMode::ReadOnly) {
      return Err(ErrorCode::NotFound, "empty store opened read-only: " + std::string{path});
    }
    // page_size first: a WAL database cannot change its page size afterwards.
    ATX_TRY_VOID(db.pragma("page_size", std::to_string(policy.page_size)));
    ATX_TRY_VOID(set_wal(db));
    ATX_TRY_VOID(set_connection_pragmas(db, policy));
    return Ok(OpenedStore{std::move(db), true, 0});
  }
  if (application_id != policy.application_id) {
    return Err(ErrorCode::InvalidArgument, "store " + std::string{path} + " has application_id " +
                                               hex32(application_id) + ", expected " +
                                               hex32(policy.application_id));
  }
  if (user_version > policy.user_version) {
    return Err(ErrorCode::NotImplemented,
               "this build cannot read schema " + std::to_string(user_version) + " of " +
                   std::string{path} + " (newest known: " +
                   std::to_string(policy.user_version) + ")");
  }
  if (mode != OpenMode::ReadOnly) {
    ATX_TRY(const std::string journal, query_text(db, "PRAGMA journal_mode"));
    if (journal != "wal") {
      ATX_TRY_VOID(set_wal(db));
    }
  }
  ATX_TRY_VOID(set_connection_pragmas(db, policy));
  return Ok(OpenedStore{std::move(db), false, user_version});
}

Status checkpoint_truncate(Database &db) {
  ATX_TRY(Statement stmt, db.prepare("PRAGMA wal_checkpoint(TRUNCATE)"));
  ATX_TRY(const Statement::Step step, stmt.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "wal_checkpoint returned no row");
  }
  ATX_TRY(const i64 busy, stmt.checked_int(0));
  if (busy != 0) {
    return Err(ErrorCode::Unavailable, "wal_checkpoint(TRUNCATE) blocked by another connection");
  }
  return Ok();
}

Result<std::string> quick_check(Database &db) { return query_text(db, "PRAGMA quick_check"); }

namespace detail {
void immediate_retry_pause() noexcept { (void)sqlite3_sleep(kImmediateRetryDelayMs); }
} // namespace detail

} // namespace atx::core::db
