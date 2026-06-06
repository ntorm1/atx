#pragma once

// atx::core::db — a safe RAII / Result wrapper over the SQLite C API.
//
// ===========================================================================
//  What this is
// ===========================================================================
//  A thin, hard-to-misuse C++ layer over vendored SQLite (third-party/sqlite,
//  compiled as the `atx_sqlite3` static lib). It turns the C API's manual
//  resource management and integer return codes into:
//    * RAII handles (Database / Statement / Transaction) that close/finalize/
//      rollback in their destructors — no leak on an early return or throw.
//    * `Result<T>` / `Status` returns (never C error codes, never exceptions
//      for control flow) — expected failures travel in the return type.
//
//  Opaque-handle discipline: this header forward-declares the SQLite C structs
//  (`sqlite3`, `sqlite3_stmt`) and NEVER includes <sqlite3.h>. The C API and its
//  macros are confined to src/db/sqlite.cpp, so including this header does not
//  drag the entire SQLite C surface into every translation unit.
//
// ===========================================================================
//  Threading (build is SQLITE_THREADSAFE=2 — "multi-thread")
// ===========================================================================
//  A single `Database` (connection) must NOT be shared across threads. Each
//  thread that touches SQLite owns its own `Database`. Multiple `Database`
//  objects on multiple threads are safe concurrently. See
//  third-party/sqlite/PROVENANCE.md.
//
// ===========================================================================
//  Conventions inherited from the C API (kept, documented — least surprise)
// ===========================================================================
//  * Bind parameter indices are 1-BASED (`?1`, `?2`, …).
//  * Result column indices are 0-BASED.
//  * `column_text` / `column_blob` return a view into SQLite-owned memory that
//    is valid only until the next `step()` / `reset()` / statement destruction.
//    Copy out if you need to keep it.

#include <cstddef>     // std::byte
#include <memory>      // std::unique_ptr (statement cache values)
#include <span>        // std::span (blob bind / column)
#include <string>      // std::string (statement-cache keys)
#include <string_view> // std::string_view (SQL text, names)
#include <unordered_map>

#include "atx/core/error.hpp" // Result, Status, Error, ErrorCode
#include "atx/core/types.hpp" // i32, i64, f64, u8

// Opaque SQLite C handles. Full definitions live in <sqlite3.h>, included ONLY
// in src/db/sqlite.cpp. Forward-declaring the C struct tags lets us hold typed
// pointers as members without exposing the C API to includers of this header.
struct sqlite3;
struct sqlite3_stmt;

namespace atx::core::db {

// ===========================================================================
//  Enums
// ===========================================================================

// How to open a database file. Maps to SQLITE_OPEN_* flags.
enum class OpenMode : u8 {
  ReadOnly,        // SQLITE_OPEN_READONLY — fail if the file does not exist
  ReadWrite,       // SQLITE_OPEN_READWRITE — fail if the file does not exist
  ReadWriteCreate, // SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE — create if absent
};

// The fundamental SQLite storage class of a result column value.
enum class ColumnType : u8 {
  Integer, // SQLITE_INTEGER
  Float,   // SQLITE_FLOAT
  Text,    // SQLITE_TEXT
  Blob,    // SQLITE_BLOB
  Null,    // SQLITE_NULL
};

// ===========================================================================
//  Statement — a prepared SQL statement (RAII over sqlite3_stmt).
//
//  Move-only: owns the compiled statement and finalizes it on destruction.
//  Construct via Database::prepare / prepare_cached (the only friends that can
//  build one from a raw handle).
// ===========================================================================
class Statement {
public:
  // Outcome of stepping a statement once.
  enum class Step : u8 {
    Row,  // a result row is available; read columns then step again
    Done, // execution complete; no more rows
  };

  ~Statement() noexcept;
  Statement(Statement &&other) noexcept;
  Statement &operator=(Statement &&other) noexcept;
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  // --- bind by 1-based index (SQLite convention) ---------------------------
  // Text/blob are bound with SQLITE_TRANSIENT (SQLite copies), so the source
  // span/view need not outlive the call.
  [[nodiscard]] Status bind(i32 index, i64 value);
  [[nodiscard]] Status bind(i32 index, f64 value);
  [[nodiscard]] Status bind(i32 index, std::string_view text);
  [[nodiscard]] Status bind(i32 index, std::span<const std::byte> blob);
  [[nodiscard]] Status bind_null(i32 index);

  // --- bind by name (":name", "@name", "$name") ----------------------------
  [[nodiscard]] Status bind(std::string_view name, i64 value);
  [[nodiscard]] Status bind(std::string_view name, f64 value);
  [[nodiscard]] Status bind(std::string_view name, std::string_view text);
  [[nodiscard]] Status bind(std::string_view name, std::span<const std::byte> blob);
  [[nodiscard]] Status bind_null(std::string_view name);

  // Advance execution by one step. Row => a row is ready; Done => finished.
  // An error (constraint, I/O, …) returns Err.
  [[nodiscard]] Result<Step> step();

  // --- typed column readers (0-based; call after step() == Row) -------------
  [[nodiscard]] i64 column_int(i32 col) const noexcept;
  [[nodiscard]] f64 column_double(i32 col) const noexcept;
  // View into SQLite-owned memory; valid until the next step()/reset(). Empty
  // view if the column is NULL.
  [[nodiscard]] std::string_view column_text(i32 col) const noexcept;
  [[nodiscard]] std::span<const std::byte> column_blob(i32 col) const noexcept;
  [[nodiscard]] bool column_is_null(i32 col) const noexcept;
  [[nodiscard]] i32 column_count() const noexcept;
  [[nodiscard]] ColumnType column_type(i32 col) const noexcept;

  // Rewind for re-execution (keeps bindings) / clear all bindings to NULL.
  [[nodiscard]] Status reset() noexcept;
  [[nodiscard]] Status clear_bindings() noexcept;

  // Escape hatch for advanced use (e.g. BlobStream). Non-owning.
  [[nodiscard]] sqlite3_stmt *handle() const noexcept { return stmt_; }

private:
  friend class Database;
  Statement(sqlite3_stmt *stmt, sqlite3 *db) noexcept : stmt_{stmt}, db_{db} {}

  // Resolve a named parameter to its 1-based index, or Err(NotFound).
  [[nodiscard]] Status named_index(std::string_view name, i32 &out) const;

  sqlite3_stmt *stmt_{nullptr}; // owned; finalized in the destructor
  sqlite3 *db_{nullptr};        // non-owning (owned by Database); for error text
};

// ===========================================================================
//  Database — an open connection (RAII over sqlite3).
//
//  Move-only: owns the connection and closes it (after finalizing any cached
//  statements) on destruction.
// ===========================================================================
class Database {
public:
  // Open a database file (or any SQLite URI/path). Use open_memory() for an
  // in-memory database. Returns Err on failure (e.g. ReadOnly on a missing
  // file, permission, I/O).
  [[nodiscard]] static Result<Database> open(std::string_view path,
                                             OpenMode mode = OpenMode::ReadWriteCreate);
  // Open a private, in-memory database (":memory:").
  [[nodiscard]] static Result<Database> open_memory();

  ~Database() noexcept;
  Database(Database &&other) noexcept;
  Database &operator=(Database &&other) noexcept;
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  // Execute one or more SQL statements that return no rows (DDL, INSERT/UPDATE/
  // DELETE without readback). For parameterized or row-returning SQL use
  // prepare().
  [[nodiscard]] Status exec(std::string_view sql);

  // Compile a statement. The returned Statement owns the compiled program.
  [[nodiscard]] Result<Statement> prepare(std::string_view sql);

  // Compile-or-reuse: returns a borrowed pointer to a Statement owned by this
  // Database's internal cache, keyed by the SQL text. The statement is reset
  // (and its bindings cleared) before being returned, ready for fresh binds.
  // The pointer is stable for the lifetime of this Database (the cache stores
  // statements via unique_ptr, so growth never relocates them). Do NOT
  // finalize/destroy it; the cache owns it.
  [[nodiscard]] Result<Statement *> prepare_cached(std::string_view sql);

  // ROWID of the most recent successful INSERT on this connection.
  [[nodiscard]] i64 last_insert_rowid() const noexcept;
  // Rows changed by the most recent INSERT/UPDATE/DELETE on this connection.
  [[nodiscard]] i64 changes() const noexcept;

  // Set the busy-handler timeout (ms) for locked-database retries.
  [[nodiscard]] Status set_busy_timeout(i32 ms);
  // Convenience for `PRAGMA <name> = <value>;`.
  [[nodiscard]] Status pragma(std::string_view name, std::string_view value);
  // Online backup of this database into `dest` (overwrites dest's main db).
  [[nodiscard]] Status backup_to(Database &dest);

  // Escape hatch: the raw connection handle (non-owning, advanced use).
  [[nodiscard]] sqlite3 *handle() const noexcept { return db_; }

private:
  explicit Database(sqlite3 *db) noexcept : db_{db} {}

  sqlite3 *db_{nullptr}; // owned; closed in the destructor
  // Prepared-statement cache. unique_ptr values keep Statement addresses stable
  // across rehash so prepare_cached can hand out durable pointers. Declared
  // after db_ so it is destroyed first — statements finalize before the
  // connection closes (the destructor also clears it explicitly to be sure).
  std::unordered_map<std::string, std::unique_ptr<Statement>> cache_;
};

// ===========================================================================
//  Transaction — a scoped BEGIN…COMMIT/ROLLBACK guard.
//
//  begin() issues BEGIN. commit() issues COMMIT. If the guard is destroyed
//  without a successful commit(), the destructor issues ROLLBACK — so an early
//  return or a thrown exception cannot leave a half-applied transaction.
//  Move-only; the moved-from guard is inert.
// ===========================================================================
class Transaction {
public:
  [[nodiscard]] static Result<Transaction> begin(Database &db);

  ~Transaction() noexcept;
  Transaction(Transaction &&other) noexcept;
  Transaction &operator=(Transaction &&other) noexcept;
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  // Commit the transaction. After a successful commit the destructor is inert.
  [[nodiscard]] Status commit();

private:
  explicit Transaction(Database *db) noexcept : db_{db} {}

  Database *db_{nullptr}; // non-owning; null/finished guard is inert
  bool finished_{false};  // committed or rolled back — destructor does nothing
};

} // namespace atx::core::db
