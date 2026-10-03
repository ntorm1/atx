// Implementation of the atx::core::db SQLite wrapper.
//
// This is the ONLY translation unit that includes <sqlite3.h>; the C API and
// its macros are confined here (the public headers forward-declare the opaque
// handles). See include/atx/core/db/sqlite.hpp for the contract.

#include "atx/core/db/sqlite.hpp"

#include "atx/core/db/blob.hpp"

#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core::db {

namespace {

// Map a SQLite result code to an atx ErrorCode (W3). Constraint failures are
// told apart by their extended code (connections run with extended result
// codes on, see Database::open): a duplicate key is AlreadyExists, a value the
// schema refuses is InvalidArgument. Every other code maps by its primary part
// (the low 8 bits), so e.g. SQLITE_BUSY_SNAPSHOT maps as SQLITE_BUSY did.
[[nodiscard]] ErrorCode map_code(int rc) noexcept {
  switch (rc) {
  case SQLITE_CONSTRAINT_UNIQUE:
  case SQLITE_CONSTRAINT_PRIMARYKEY:
    return ErrorCode::AlreadyExists;
  case SQLITE_CONSTRAINT_CHECK:
  case SQLITE_CONSTRAINT_NOTNULL:
  case SQLITE_CONSTRAINT_FOREIGNKEY:
  case SQLITE_CONSTRAINT_TRIGGER:
  case SQLITE_CONSTRAINT_DATATYPE:
    return ErrorCode::InvalidArgument;
  default:
    break;
  }
  switch (rc & 0xFF) {
  case SQLITE_OK:
  case SQLITE_ROW:
  case SQLITE_DONE:
    return ErrorCode::Internal; // not an error; callers should not map these
  case SQLITE_CONSTRAINT:
    return ErrorCode::AlreadyExists;
  case SQLITE_NOTFOUND:
    return ErrorCode::NotFound;
  case SQLITE_PERM:
  case SQLITE_AUTH:
  case SQLITE_READONLY:
    return ErrorCode::PermissionDenied;
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    return ErrorCode::Unavailable;
  case SQLITE_IOERR:
  case SQLITE_CANTOPEN:
  case SQLITE_FULL:
  case SQLITE_CORRUPT:
  case SQLITE_NOTADB:
    return ErrorCode::IoError;
  case SQLITE_RANGE:
    return ErrorCode::OutOfRange;
  case SQLITE_MISUSE:
  case SQLITE_INTERNAL:
  case SQLITE_NOMEM:
    return ErrorCode::Internal;
  case SQLITE_ERROR:
    return ErrorCode::ParseError; // generic SQL error incl. syntax/no-such-table
  default:
    return ErrorCode::Unknown;
  }
}

// Build an Error from a SQLite result code, preferring the connection's
// human-readable message (sqlite3_errmsg) when a handle is available.
[[nodiscard]] Error make_error(int rc, sqlite3 *db) {
  const char *const text = (db != nullptr) ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
  std::string msg = (text != nullptr) ? text : "sqlite error";
  msg += " (sqlite rc=";
  msg += std::to_string(rc);
  msg += ')';
  return Error{map_code(rc), std::move(msg)};
}

[[nodiscard]] int open_flags(OpenMode mode) noexcept {
  switch (mode) {
  case OpenMode::ReadOnly:
    return SQLITE_OPEN_READONLY;
  case OpenMode::ReadWrite:
    return SQLITE_OPEN_READWRITE;
  case OpenMode::ReadWriteCreate:
    return SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  }
  return SQLITE_OPEN_READONLY; // unreachable (exhaustive switch)
}

// True when `tail` holds only whitespace and SQL comments (`-- ...` to the end
// of a line, `/* ... */`; an unterminated block comment runs to the end, as in
// SQLite's tokenizer). Bounded: every iteration consumes at least one byte.
[[nodiscard]] bool tail_is_blank(std::string_view tail) noexcept {
  usize i = 0;
  while (i < tail.size()) {
    const char c = tail[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      ++i;
    } else if (tail.substr(i, 2) == "--") {
      const usize end = tail.find('\n', i + 2);
      i = (end == std::string_view::npos) ? tail.size() : end + 1;
    } else if (tail.substr(i, 2) == "/*") {
      const usize end = tail.find("*/", i + 2);
      i = (end == std::string_view::npos) ? tail.size() : end + 2;
    } else {
      return false;
    }
  }
  return true;
}

// Checked-reader guard: a current row holds column `col`.
[[nodiscard]] Status check_column(sqlite3_stmt *stmt, i32 col) {
  if (col < 0 || col >= sqlite3_data_count(stmt)) {
    return Err(ErrorCode::OutOfRange, "no current row or column " + std::to_string(col) +
                                          " outside it");
  }
  return Ok();
}

[[nodiscard]] Status check_storage(sqlite3_stmt *stmt, i32 col, int want, std::string_view what) {
  const int got = sqlite3_column_type(stmt, col);
  if (got == SQLITE_NULL) {
    return Err(ErrorCode::InvalidArgument,
               "column " + std::to_string(col) + " is NULL, expected " + std::string{what});
  }
  if (got != want) {
    return Err(ErrorCode::InvalidArgument, "column " + std::to_string(col) +
                                               " has storage class " + std::to_string(got) +
                                               ", expected " + std::string{what});
  }
  return Ok();
}

} // namespace

// ===========================================================================
//  Statement
// ===========================================================================

Statement::~Statement() noexcept {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
  }
}

Statement::Statement(Statement &&other) noexcept : stmt_{other.stmt_}, db_{other.db_} {
  other.stmt_ = nullptr;
  other.db_ = nullptr;
}

Statement &Statement::operator=(Statement &&other) noexcept {
  if (this != &other) {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
    stmt_ = other.stmt_;
    db_ = other.db_;
    other.stmt_ = nullptr;
    other.db_ = nullptr;
  }
  return *this;
}

Status Statement::bind(i32 index, i64 value) {
  const int rc = sqlite3_bind_int64(stmt_, index, value);
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
}

Status Statement::bind(i32 index, f64 value) {
  const int rc = sqlite3_bind_double(stmt_, index, value);
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
}

Status Statement::bind(i32 index, std::string_view text) {
  // SQLITE_TRANSIENT: SQLite makes its own copy, so `text` need not outlive us.
  // A default-constructed empty string_view may have data()==nullptr; SQLite
  // interprets a null pointer as SQL NULL even when the byte count is zero.
  if (text.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
    return Err(ErrorCode::OutOfRange, "SQLite text binding exceeds INT_MAX bytes");
  }
  const char *const data = text.empty() ? "" : text.data();
  const int rc =
      sqlite3_bind_text(stmt_, index, data, static_cast<int>(text.size()), SQLITE_TRANSIENT);
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
}

Status Statement::bind(i32 index, std::span<const std::byte> blob) {
  if (blob.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
    return Err(ErrorCode::OutOfRange, "SQLite blob binding exceeds INT_MAX bytes");
  }
  if (blob.empty()) {
    const int rc = sqlite3_bind_zeroblob(stmt_, index, 0);
    return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
  }
  const int rc =
      sqlite3_bind_blob(stmt_, index, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
}

Status Statement::bind_null(i32 index) {
  const int rc = sqlite3_bind_null(stmt_, index);
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
}

Status Statement::named_index(std::string_view name, i32 &out) const {
  const std::string z{name};
  const int idx = sqlite3_bind_parameter_index(stmt_, z.c_str());
  if (idx == 0) {
    return Err(ErrorCode::NotFound, "no such bind parameter: " + z);
  }
  out = static_cast<i32>(idx);
  return Ok();
}

Status Statement::bind(std::string_view name, i64 value) {
  i32 idx{};
  ATX_TRY_VOID(named_index(name, idx));
  return bind(idx, value);
}

Status Statement::bind(std::string_view name, f64 value) {
  i32 idx{};
  ATX_TRY_VOID(named_index(name, idx));
  return bind(idx, value);
}

Status Statement::bind(std::string_view name, std::string_view text) {
  i32 idx{};
  ATX_TRY_VOID(named_index(name, idx));
  return bind(idx, text);
}

Status Statement::bind(std::string_view name, std::span<const std::byte> blob) {
  i32 idx{};
  ATX_TRY_VOID(named_index(name, idx));
  return bind(idx, blob);
}

Status Statement::bind_null(std::string_view name) {
  i32 idx{};
  ATX_TRY_VOID(named_index(name, idx));
  return bind_null(idx);
}

Result<Statement::Step> Statement::step() {
  const int rc = sqlite3_step(stmt_);
  switch (rc) {
  case SQLITE_ROW:
    return Ok(Step::Row);
  case SQLITE_DONE:
    return Ok(Step::Done);
  default:
    return Err(make_error(rc, db_));
  }
}

i64 Statement::column_int(i32 col) const noexcept { return sqlite3_column_int64(stmt_, col); }

f64 Statement::column_double(i32 col) const noexcept { return sqlite3_column_double(stmt_, col); }

std::string_view Statement::column_text(i32 col) const noexcept {
  // SAFETY: sqlite3_column_text returns a NUL-terminated UTF-8 buffer owned by
  //   SQLite (valid until the next step/reset). The cast from `const unsigned
  //   char*` to `const char*` is a permitted character-type alias. Length comes
  //   from sqlite3_column_bytes (excludes the terminator). nullptr => empty.
  const auto *const ptr = reinterpret_cast<const char *>(sqlite3_column_text(stmt_, col));
  if (ptr == nullptr) {
    return {};
  }
  const int len = sqlite3_column_bytes(stmt_, col);
  return std::string_view{ptr, static_cast<usize>(len)};
}

std::span<const std::byte> Statement::column_blob(i32 col) const noexcept {
  const void *const ptr = sqlite3_column_blob(stmt_, col);
  const int len = sqlite3_column_bytes(stmt_, col);
  if (ptr == nullptr || len <= 0) {
    return {};
  }
  // SAFETY: SQLite-owned buffer of `len` bytes (valid until next step/reset).
  //   Reinterpreting raw bytes as std::byte is the standard byte-view cast.
  return std::span<const std::byte>{static_cast<const std::byte *>(ptr), static_cast<usize>(len)};
}

bool Statement::column_is_null(i32 col) const noexcept {
  return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

i32 Statement::column_count() const noexcept { return sqlite3_column_count(stmt_); }

ColumnType Statement::column_type(i32 col) const noexcept {
  switch (sqlite3_column_type(stmt_, col)) {
  case SQLITE_INTEGER:
    return ColumnType::Integer;
  case SQLITE_FLOAT:
    return ColumnType::Float;
  case SQLITE_TEXT:
    return ColumnType::Text;
  case SQLITE_BLOB:
    return ColumnType::Blob;
  case SQLITE_NULL:
  default:
    return ColumnType::Null;
  }
}

Result<i64> Statement::checked_int(i32 col) const {
  ATX_TRY_VOID(check_column(stmt_, col));
  ATX_TRY_VOID(check_storage(stmt_, col, SQLITE_INTEGER, "INTEGER"));
  return Ok(static_cast<i64>(sqlite3_column_int64(stmt_, col)));
}

Result<f64> Statement::checked_double(i32 col) const {
  ATX_TRY_VOID(check_column(stmt_, col));
  if (sqlite3_column_type(stmt_, col) == SQLITE_INTEGER) {
    return Ok(sqlite3_column_double(stmt_, col));
  }
  ATX_TRY_VOID(check_storage(stmt_, col, SQLITE_FLOAT, "REAL"));
  return Ok(sqlite3_column_double(stmt_, col));
}

Result<std::string> Statement::checked_text(i32 col) const {
  ATX_TRY_VOID(check_column(stmt_, col));
  ATX_TRY_VOID(check_storage(stmt_, col, SQLITE_TEXT, "TEXT"));
  return Ok(std::string{column_text(col)});
}

Result<std::vector<std::byte>> Statement::checked_blob(i32 col) const {
  ATX_TRY_VOID(check_column(stmt_, col));
  ATX_TRY_VOID(check_storage(stmt_, col, SQLITE_BLOB, "BLOB"));
  const std::span<const std::byte> view = column_blob(col);
  return Ok(std::vector<std::byte>(view.begin(), view.end()));
}

Status Statement::reset() noexcept {
  // sqlite3_reset returns the result code of the PRIOR step (SQLITE_OK if it
  // succeeded). A prior-step error here is informational, not a reset failure;
  // we surface it so callers can notice, but the statement is still rewound.
  const int rc = sqlite3_reset(stmt_);
  return (rc == SQLITE_OK) ? Ok() : Err(Error{map_code(rc), "sqlite3_reset"});
}

Status Statement::clear_bindings() noexcept {
  const int rc = sqlite3_clear_bindings(stmt_);
  return (rc == SQLITE_OK) ? Ok() : Err(Error{map_code(rc), "sqlite3_clear_bindings"});
}

// ===========================================================================
//  Database
// ===========================================================================

Result<Database> Database::open(std::string_view path, OpenMode mode) {
  const std::string z{path}; // sqlite3_open_v2 needs a NUL-terminated path
  sqlite3 *db = nullptr;
  const int rc = sqlite3_open_v2(z.c_str(), &db, open_flags(mode), nullptr);
  if (rc != SQLITE_OK) {
    Error e = make_error(rc, db);
    sqlite3_close_v2(db); // db may be non-null even on failure; close it
    return Err(std::move(e));
  }
  // W3: extended codes let map_code tell a duplicate key from a refused value.
  (void)sqlite3_extended_result_codes(db, 1); // returns SQLITE_OK on an open handle
  return Ok(Database{db});
}

Result<Database> Database::open_memory() {
  return open(std::string_view{":memory:"}, OpenMode::ReadWriteCreate);
}

Database::~Database() noexcept {
  cache_.clear(); // finalize cached statements BEFORE closing the connection
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
  }
}

Database::Database(Database &&other) noexcept : db_{other.db_}, cache_{std::move(other.cache_)} {
  other.db_ = nullptr;
}

Database &Database::operator=(Database &&other) noexcept {
  if (this != &other) {
    cache_.clear();
    if (db_ != nullptr) {
      sqlite3_close_v2(db_);
    }
    db_ = other.db_;
    cache_ = std::move(other.cache_);
    other.db_ = nullptr;
  }
  return *this;
}

Status Database::exec(std::string_view sql) {
  const std::string z{sql}; // sqlite3_exec needs a NUL-terminated string
  const int rc = sqlite3_exec(db_, z.c_str(), nullptr, nullptr, nullptr);
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
}

Result<Statement> Database::prepare(std::string_view sql) {
  // W2: the length crosses into a C int.
  if (sql.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
    return Err(ErrorCode::OutOfRange, "SQL text exceeds INT_MAX bytes");
  }
  sqlite3_stmt *stmt = nullptr;
  const char *tail = nullptr;
  // prepare_v2 takes an explicit length, so `sql` need not be NUL-terminated.
  const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, &tail);
  if (rc != SQLITE_OK) {
    return Err(make_error(rc, db_));
  }
  Statement compiled{stmt, db_}; // owns stmt from here (finalized on every return path)
  // W1: SQLite compiles the first statement only; refuse a second one instead of dropping it.
  // SAFETY: on SQLITE_OK `tail` points into [sql.data(), sql.data() + sql.size()].
  const usize consumed =
      (tail == nullptr) ? sql.size() : static_cast<usize>(tail - sql.data());
  if (!tail_is_blank(sql.substr(consumed))) {
    return Err(ErrorCode::ParseError,
               "prepare: more than one SQL statement (use exec for multi-statement SQL)");
  }
  return Ok(std::move(compiled));
}

Result<Statement *> Database::prepare_cached(std::string_view sql) {
  std::string key{sql};
  const auto it = cache_.find(key);
  if (it != cache_.end()) {
    // Reuse: rewind + clear bindings. W8: both statuses are discarded on purpose.
    // sqlite3_reset returns the result of the statement's PREVIOUS step (already
    // reported to whoever stepped it), not a failure to rewind, and
    // sqlite3_clear_bindings cannot fail on a valid statement; surfacing either
    // here would fail an unrelated new use of the cached statement.
    (void)it->second->reset();
    (void)it->second->clear_bindings();
    return Ok(it->second.get());
  }
  ATX_TRY(Statement stmt, prepare(sql));
  const auto [pos, _] =
      cache_.emplace(std::move(key), std::make_unique<Statement>(std::move(stmt)));
  return Ok(pos->second.get());
}

i64 Database::last_insert_rowid() const noexcept { return sqlite3_last_insert_rowid(db_); }

i64 Database::changes() const noexcept { return sqlite3_changes64(db_); }

Status Database::set_busy_timeout(i32 ms) {
  const int rc = sqlite3_busy_timeout(db_, ms);
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, db_));
}

Status Database::pragma(std::string_view name, std::string_view value) {
  std::string sql = "PRAGMA ";
  sql.append(name);
  sql += " = ";
  sql.append(value);
  sql += ';';
  return exec(sql);
}

Status Database::backup_to(Database &dest) {
  ATX_TRY(auto ignored, backup_to(dest, BackupOptions{}));
  (void)ignored;
  return Ok();
}

Result<BackupReport> Database::backup_to(Database &dest, const BackupOptions &options) {
  if (options.pages_per_step <= 0 || options.maximum_busy_retries < 0 ||
      options.retry_delay_ms < 0 || options.step_delay_ms < 0) {
    return Err(ErrorCode::InvalidArgument, "invalid SQLite backup options");
  }
  sqlite3_backup *const backup = sqlite3_backup_init(dest.db_, "main", db_, "main");
  if (backup == nullptr) {
    return Err(make_error(sqlite3_errcode(dest.db_), dest.db_));
  }
  BackupReport report;
  while (true) {
    const int step_rc = sqlite3_backup_step(backup, options.pages_per_step);
    ++report.steps;
    report.remaining_pages = sqlite3_backup_remaining(backup);
    report.page_count = sqlite3_backup_pagecount(backup);
    if (step_rc == SQLITE_DONE) {
      const int finish_rc = sqlite3_backup_finish(backup);
      if (finish_rc != SQLITE_OK) {
        return Err(make_error(finish_rc, dest.db_));
      }
      return Ok(report);
    }
    if (step_rc == SQLITE_OK) {
      if (options.step_delay_ms != 0) {
        sqlite3_sleep(options.step_delay_ms);
      }
      continue;
    }
    // Primary part: the connections run with extended result codes (W3).
    if ((step_rc & 0xFF) == SQLITE_BUSY || (step_rc & 0xFF) == SQLITE_LOCKED) {
      if (report.busy_retries >= options.maximum_busy_retries) {
        Error error = make_error(step_rc, dest.db_);
        (void)sqlite3_backup_finish(backup);
        return Err(std::move(error));
      }
      ++report.busy_retries;
      if (options.retry_delay_ms != 0) {
        sqlite3_sleep(options.retry_delay_ms);
      }
      continue;
    }
    Error error = make_error(step_rc, dest.db_);
    (void)sqlite3_backup_finish(backup);
    return Err(std::move(error));
  }
}

// ===========================================================================
//  Transaction
// ===========================================================================

Result<Transaction> Transaction::begin(Database &db) {
  ATX_TRY_VOID(db.exec("BEGIN"));
  return Ok(Transaction{&db});
}

Result<Transaction> Transaction::begin_immediate(Database &db) {
  ATX_TRY_VOID(db.exec("BEGIN IMMEDIATE"));
  return Ok(Transaction{&db});
}

namespace {

// W6: the destructor / move-assign rollback. Database::exec builds a std::string
// (it may throw) and is not callable from a noexcept path; this runs the literal
// directly and allocates nothing on the C++ side. A failure to roll back is
// unrecoverable here; the result code is discarded (fail-safe: SQLite rolls the
// transaction back when the connection closes).
void rollback_noexcept(Database *db) noexcept {
  if (db != nullptr && db->handle() != nullptr) {
    (void)sqlite3_exec(db->handle(), "ROLLBACK", nullptr, nullptr, nullptr);
  }
}

} // namespace

Transaction::~Transaction() noexcept {
  if (!finished_) {
    rollback_noexcept(db_);
  }
}

Transaction::Transaction(Transaction &&other) noexcept
    : db_{other.db_}, finished_{other.finished_} {
  other.db_ = nullptr;
  other.finished_ = true; // moved-from guard is inert
}

Transaction &Transaction::operator=(Transaction &&other) noexcept {
  if (this != &other) {
    if (!finished_) {
      rollback_noexcept(db_);
    }
    db_ = other.db_;
    finished_ = other.finished_;
    other.db_ = nullptr;
    other.finished_ = true;
  }
  return *this;
}

Status Transaction::commit() {
  ATX_TRY_VOID(db_->exec("COMMIT"));
  finished_ = true;
  return Ok();
}

// ===========================================================================
//  BlobStream
// ===========================================================================

Result<BlobStream> BlobStream::open(Database &db, std::string_view table, std::string_view column,
                                    i64 rowid, bool writable, std::string_view schema) {
  const std::string z_schema{schema};
  const std::string z_table{table};
  const std::string z_column{column};
  sqlite3_blob *blob = nullptr;
  const int rc = sqlite3_blob_open(db.handle(), z_schema.c_str(), z_table.c_str(), z_column.c_str(),
                                   rowid, writable ? 1 : 0, &blob);
  if (rc != SQLITE_OK) {
    return Err(make_error(rc, db.handle()));
  }
  return Ok(BlobStream{blob});
}

BlobStream::~BlobStream() noexcept {
  if (blob_ != nullptr) {
    sqlite3_blob_close(blob_);
  }
}

BlobStream::BlobStream(BlobStream &&other) noexcept : blob_{other.blob_} { other.blob_ = nullptr; }

BlobStream &BlobStream::operator=(BlobStream &&other) noexcept {
  if (this != &other) {
    if (blob_ != nullptr) {
      sqlite3_blob_close(blob_);
    }
    blob_ = other.blob_;
    other.blob_ = nullptr;
  }
  return *this;
}

i64 BlobStream::size() const noexcept { return sqlite3_blob_bytes(blob_); }

namespace {

// W5: [offset, offset + length) must lie inside a blob of `size` bytes, and both
// values must fit the C API's int, BEFORE either is narrowed.
[[nodiscard]] Status check_blob_range(i64 offset, usize length, i64 size) {
  constexpr i64 kIntMax = std::numeric_limits<int>::max();
  if (offset < 0 || offset > kIntMax || length > static_cast<usize>(kIntMax)) {
    return Err(ErrorCode::OutOfRange, "blob offset or length outside [0, INT_MAX]");
  }
  if (offset + static_cast<i64>(length) > size) {
    return Err(ErrorCode::OutOfRange, "blob range [" + std::to_string(offset) + ", " +
                                          std::to_string(offset + static_cast<i64>(length)) +
                                          ") exceeds the blob size " + std::to_string(size));
  }
  return Ok();
}

} // namespace

Result<usize> BlobStream::read(std::span<std::byte> out, i64 offset) {
  ATX_TRY_VOID(check_blob_range(offset, out.size(), size()));
  const int rc =
      sqlite3_blob_read(blob_, out.data(), static_cast<int>(out.size()), static_cast<int>(offset));
  if (rc != SQLITE_OK) {
    return Err(make_error(rc, nullptr));
  }
  return Ok(out.size());
}

Status BlobStream::write(std::span<const std::byte> in, i64 offset) {
  ATX_TRY_VOID(check_blob_range(offset, in.size(), size()));
  const int rc =
      sqlite3_blob_write(blob_, in.data(), static_cast<int>(in.size()), static_cast<int>(offset));
  return (rc == SQLITE_OK) ? Ok() : Err(make_error(rc, nullptr));
}

} // namespace atx::core::db
