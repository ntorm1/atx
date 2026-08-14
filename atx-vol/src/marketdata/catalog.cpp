#include "marketdata/catalog.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "storage/writer_lock.hpp" // Task D6: detail::current_process_id/process_alive reuse

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

namespace {

namespace fs = std::filesystem;
namespace db = atx::core::db;

[[nodiscard]] std::int64_t wall_clock_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Schema v1, landed verbatim from the brief's DDL with one addition: every
// CREATE TABLE/INDEX is `IF NOT EXISTS` so `Catalog::open()` is idempotent --
// re-opening an already-initialized catalog.sqlite (the normal case for
// every call after the very first) is not an error, matching the class's
// documented "create-if-absent, open otherwise" contract.
constexpr std::string_view kSchemaDdl = R"sql(
CREATE TABLE IF NOT EXISTS tracks(
  track_key TEXT PRIMARY KEY,          -- hex SHA-256
  underlier TEXT NOT NULL, family TEXT NOT NULL,
  config_json TEXT NOT NULL,           -- canonical, human-queryable copy
  engine_id TEXT NOT NULL, economics_rev INTEGER NOT NULL,
  data_snapshot_id TEXT NOT NULL,
  date_min TEXT NOT NULL, date_max TEXT NOT NULL,
  status TEXT NOT NULL CHECK(status IN ('staging','compacted','retired')),
  file TEXT, row_group INTEGER,        -- NULL while staging
  created_ts INTEGER NOT NULL, last_access_ts INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS trials(     -- B4's N: EVERY variant ever attempted
  trial_id INTEGER PRIMARY KEY,
  track_key TEXT NOT NULL REFERENCES tracks(track_key),
  sweep_id TEXT NOT NULL, sharpe REAL, created_ts INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_tracks_dims ON tracks(underlier, family, date_min, date_max);
CREATE TABLE IF NOT EXISTS reader_marks( -- Task D6: shared advisory marks
  mark_id INTEGER PRIMARY KEY,
  file TEXT NOT NULL, pid INTEGER NOT NULL, created_ts INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_reader_marks_file ON reader_marks(file);
)sql";

constexpr std::string_view kProbeSql =
    "SELECT track_key, underlier, family, config_json, engine_id, economics_rev, "
    "data_snapshot_id, date_min, date_max, status, file, row_group, created_ts, "
    "last_access_ts FROM tracks WHERE track_key = ?1;";

// Touch-on-probe (fix-round, Important 1): the ONE write `probe()` performs,
// and only on a hit. Single statement, no explicit transaction -- same
// "one autocommit write" discipline as `kRetireStaleSql`/`kMarkCompactedSql`.
constexpr std::string_view kTouchLastAccessSql = "UPDATE tracks SET last_access_ts = ?1 WHERE track_key = ?2;";

constexpr std::string_view kInsertTrackSql =
    "INSERT INTO tracks(track_key, underlier, family, config_json, engine_id, economics_rev, "
    "data_snapshot_id, date_min, date_max, status, file, row_group, created_ts, "
    "last_access_ts) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14);";

constexpr std::string_view kMarkCompactedSql =
    "UPDATE tracks SET status = ?1, file = ?2, row_group = ?3 WHERE track_key = ?4 AND status = "
    "?5;";

// Economics-rev supersession (Task D5). Retires every OLDER-generation row
// for the SAME logical variant as the row `register_staging` just inserted:
// same `underlier`/`family`/`config_json`/`data_snapshot_id` (none of the
// four encodes `engine_id`, so all four stay byte-identical across an
// economics_rev bump -- see track_key.hpp), but a STRICTLY LOWER
// `economics_rev`. The comparison is a plain integer `<` -- never
// `created_ts`/wall-clock -- so the outcome is the same regardless of which
// generation's row happened to be registered "first" in real time (I1-I8).
// `status != ?1` is not required for correctness (re-setting 'retired' to
// 'retired' is a no-op) but keeps `Database::changes()` meaningful for a
// caller that inspects it. Never touches `file`/`row_group`: retiring is a
// status LABEL only, so an already-compacted old row keeps pointing at its
// real Parquet data (never deleted -- both generations stay `probe()`-able,
// and `atxpy.tracks.catalog()`'s unfiltered SELECT still returns it, per the
// brief's "both generations queryable").
constexpr std::string_view kRetireSupersededSql =
    "UPDATE tracks SET status = ?1 WHERE underlier = ?2 AND family = ?3 AND config_json = ?4 AND "
    "data_snapshot_id = ?5 AND economics_rev < ?6 AND status != ?1;";

constexpr std::string_view kCountTrialsSql = "SELECT COUNT(*) FROM trials WHERE sweep_id = ?1;";

// ORDER BY trial_id: determinism is a binding sprint constraint. Without an
// explicit order, the float summation order behind sr_variance's mean/
// sum-of-squares (Catalog::trial_stats) would ride on SQLite's own implicit
// scan order -- unspecified by the SQL standard and not itself guaranteed
// stable across SQLite versions/query plans. trial_id (INTEGER PRIMARY KEY,
// insertion order) gives a stable, reproducible aggregation order.
constexpr std::string_view kSelectTrialSharpesSql =
    "SELECT sharpe FROM trials WHERE sweep_id = ?1 AND sharpe IS NOT NULL ORDER BY trial_id;";

constexpr std::string_view kInsertTrialSql =
    "INSERT INTO trials(track_key, sweep_id, sharpe, created_ts) VALUES (?1, ?2, ?3, ?4);";

constexpr std::string_view kListByStatusSql =
    "SELECT track_key, underlier, family, config_json, engine_id, economics_rev, "
    "data_snapshot_id, date_min, date_max, status, file, row_group, created_ts, "
    "last_access_ts FROM tracks WHERE status = ?1 ORDER BY track_key;";

// Task D6 -- retire_stale: pure status-label transition, same shape as
// kRetireSupersededSql above, but selecting on age (last_access_ts) rather
// than on economics_rev/variant identity. Only ever targets 'compacted' rows
// -- see retire_stale's own doc comment for why 'staging'/'retired' rows are
// never selected.
constexpr std::string_view kRetireStaleSql =
    "UPDATE tracks SET status = ?1 WHERE status = ?2 AND last_access_ts < ?3;";

constexpr std::string_view kRowsByFileSql =
    "SELECT track_key, underlier, family, config_json, engine_id, economics_rev, "
    "data_snapshot_id, date_min, date_max, status, file, row_group, created_ts, "
    "last_access_ts FROM tracks WHERE file = ?1 ORDER BY track_key;";

constexpr std::string_view kInsertReaderMarkSql =
    "INSERT INTO reader_marks(file, pid, created_ts) VALUES (?1, ?2, ?3);";
constexpr std::string_view kDeleteReaderMarkByIdSql = "DELETE FROM reader_marks WHERE mark_id = ?1;";
constexpr std::string_view kSelectReaderMarksForFileSql =
    "SELECT mark_id, pid FROM reader_marks WHERE file = ?1;";

// apply_gc_rewrite's two UPDATEs. Both repeat `status`/`old_file`(=`file`) in
// the WHERE clause as a defensive guard -- a row that changed shape between
// the caller's own read (rows_by_file) and this call (e.g. a concurrent
// writer) is simply not touched, rather than blindly overwritten.
constexpr std::string_view kClearReclaimedFileSql =
    "UPDATE tracks SET file = NULL, row_group = NULL WHERE track_key = ?1 AND status = ?2 AND file = "
    "?3;";
constexpr std::string_view kRepointRewrittenFileSql =
    "UPDATE tracks SET file = ?1, row_group = 0 WHERE track_key = ?2 AND status = ?3 AND file = ?4;";

// Shared by probe() and list_by_status() -- both read the same 14-column
// SELECT shape (kProbeSql/kListByStatusSql), so this is the ONE place that
// column order has to agree with the schema.
[[nodiscard]] Result<TrackRow> row_from_statement(db::Statement &stmt) {
  TrackRow row;
  row.track_key = std::string(stmt.column_text(0));
  row.underlier = std::string(stmt.column_text(1));
  row.family = std::string(stmt.column_text(2));
  row.config_json = std::string(stmt.column_text(3));
  row.engine_id = std::string(stmt.column_text(4));
  row.economics_rev = stmt.column_int(5);
  row.data_snapshot_id = std::string(stmt.column_text(6));
  row.date_min = std::string(stmt.column_text(7));
  row.date_max = std::string(stmt.column_text(8));
  auto status = track_status_from_string(stmt.column_text(9));
  if (!status) {
    return Err(status.error());
  }
  row.status = *status;
  if (!stmt.column_is_null(10)) {
    row.file = std::string(stmt.column_text(10));
  }
  if (!stmt.column_is_null(11)) {
    row.row_group = stmt.column_int(11);
  }
  row.created_ts = stmt.column_int(12);
  row.last_access_ts = stmt.column_int(13);
  return Ok(std::move(row));
}

} // namespace

std::string_view to_string(TrackStatus status) noexcept {
  switch (status) {
  case TrackStatus::Staging:
    return "staging";
  case TrackStatus::Compacted:
    return "compacted";
  case TrackStatus::Retired:
    return "retired";
  }
  return "unrecognized"; // unreachable for valid enumerators
}

Result<TrackStatus> track_status_from_string(std::string_view text) {
  if (text == "staging") {
    return Ok(TrackStatus::Staging);
  }
  if (text == "compacted") {
    return Ok(TrackStatus::Compacted);
  }
  if (text == "retired") {
    return Ok(TrackStatus::Retired);
  }
  return Err(ErrorCode::ParseError,
             "Catalog: unrecognized tracks.status value: " + std::string(text));
}

Result<Catalog> Catalog::open(std::string lake_root, std::chrono::milliseconds busy_timeout) {
  if (lake_root.empty()) {
    return Err(ErrorCode::InvalidArgument, "Catalog::open: empty lake_root");
  }
  std::error_code mkdir_ec;
  fs::create_directories(fs::path(lake_root), mkdir_ec);
  if (mkdir_ec) {
    return Err(ErrorCode::IoError, "Catalog::open: cannot create lake_root: " + mkdir_ec.message());
  }
  const std::string db_path = (fs::path(lake_root) / std::string(kCatalogDbName)).string();

  ATX_TRY(db::Database database, db::Database::open(db_path, db::OpenMode::ReadWriteCreate));

  // WAL first (brief's stated pragma order): this is the mechanism that
  // makes the catalog single-writer/many-reader across processes without any
  // of this task's own file-lock machinery (contrast detail/writer_lock.hpp,
  // built for BacktestDb/SurfaceDb's flat-file manifest, which has no
  // built-in concurrency control of its own). `PRAGMA journal_mode=WAL;` is
  // read back as a ROW (not just checked as a Status) because SQLite reports
  // SQLITE_OK and silently falls back to a different mode on a filesystem
  // that cannot support WAL (e.g. some network mounts) -- trusting the
  // Status alone would let a non-WAL catalog through undetected.
  {
    ATX_TRY(auto wal_stmt, database.prepare("PRAGMA journal_mode=WAL;"));
    ATX_TRY(const auto step, wal_stmt.step());
    if (step != db::Statement::Step::Row) {
      return Err(ErrorCode::Internal, "Catalog::open: journal_mode pragma returned no row");
    }
    const std::string_view mode = wal_stmt.column_text(0);
    if (mode != "wal") {
      return Err(ErrorCode::Unavailable,
                 "Catalog::open: journal_mode=WAL did not take effect (got '" +
                     std::string(mode) + "') -- filesystem may not support WAL");
    }
  }

  if (Status s = database.set_busy_timeout(static_cast<int>(busy_timeout.count())); !s) {
    return Err(s.error());
  }
  if (Status s = database.pragma("synchronous", "NORMAL"); !s) {
    return Err(s.error());
  }
  if (Status s = database.exec(kSchemaDdl); !s) {
    return Err(s.error());
  }

  return Ok(Catalog(std::move(database), std::move(lake_root)));
}

Result<std::optional<TrackRow>> Catalog::probe(const TrackKey &key) {
  ATX_TRY(auto *stmt, db_.prepare_cached(kProbeSql));
  ATX_TRY_VOID(stmt->bind(1, key.hex()));
  ATX_TRY(const auto step, stmt->step());
  if (step == db::Statement::Step::Done) {
    ATX_TRY_VOID(stmt->reset());
    return Ok(std::optional<TrackRow>{});
  }

  Result<TrackRow> row = row_from_statement(*stmt);
  ATX_TRY_VOID(stmt->reset());
  if (!row.has_value()) {
    return Err(row.error());
  }

  // Touch-on-probe (fix-round, Important 1) -- see catalog.hpp's own doc
  // comment on `probe()` for why this write exists. One additional
  // single-statement UPDATE; failure here is reported to the caller (this
  // function is not pure-read any more, so it fails closed like every other
  // catalog write) rather than swallowed.
  const std::int64_t touched_ts = wall_clock_ns();
  ATX_TRY(auto *touch_stmt, db_.prepare_cached(kTouchLastAccessSql));
  ATX_TRY_VOID(touch_stmt->bind(1, touched_ts));
  ATX_TRY_VOID(touch_stmt->bind(2, key.hex()));
  auto touch_step = touch_stmt->step();
  if (!touch_step) {
    ATX_TRY_VOID(touch_stmt->reset());
    return Err(touch_step.error());
  }
  ATX_TRY_VOID(touch_stmt->reset());
  row->last_access_ts = touched_ts;

  return Ok(std::optional<TrackRow>{std::move(*row)});
}

Result<std::vector<TrackRow>> Catalog::list_by_status(TrackStatus status) {
  std::vector<TrackRow> rows;
  ATX_TRY(auto *stmt, db_.prepare_cached(kListByStatusSql));
  ATX_TRY_VOID(stmt->bind(1, to_string(status)));
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == db::Statement::Step::Done) {
      break;
    }
    Result<TrackRow> row = row_from_statement(*stmt);
    if (!row.has_value()) {
      ATX_TRY_VOID(stmt->reset());
      return Err(row.error());
    }
    rows.push_back(std::move(*row));
  }
  ATX_TRY_VOID(stmt->reset());
  return Ok(std::move(rows));
}

Status Catalog::register_staging(const TrackKey &key, const TrackMeta &meta,
                                 const TrackRegistration &registration) {
  if (meta.underlier.empty() || meta.family.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "Catalog::register_staging: empty underlier/family");
  }
  if (registration.config_json.empty() || registration.engine_id.empty() ||
      registration.data_snapshot_id.empty() || registration.date_min.empty() ||
      registration.date_max.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "Catalog::register_staging: empty registration field");
  }
  if (registration.date_max < registration.date_min) {
    return Err(ErrorCode::InvalidArgument,
               "Catalog::register_staging: date_max precedes date_min");
  }

  // Fix-round (D5 review finding 1): the fresh-row INSERT and the
  // supersession retire UPDATE below must land as ONE atomic unit. Without
  // this transaction they are two independent autocommit statements, and a
  // crash between them is UNRECOVERABLE, not merely benign bookkeeping
  // drift: the fresh row's key is now registered, so every future probe()
  // against it is a HIT -- nothing ever calls register_staging for that
  // exact key again, so nothing ever re-drives the retire step for the
  // predecessor it was supposed to supersede. That predecessor would stay
  // un-retired forever unless some LATER, unrelated generation happens to
  // register against the same variant. begin_immediate (not begin):
  // acquires the write reservation up front -- both statements below are
  // writes, so there is no read phase that would benefit from a deferred
  // transaction, and acquiring immediately avoids a late upgrade failing
  // with SQLITE_BUSY_SNAPSHOT against a concurrent writer.
  ATX_TRY(db::Transaction txn, db::Transaction::begin_immediate(db_));

  const std::int64_t now = wall_clock_ns();
  ATX_TRY(auto *stmt, db_.prepare_cached(kInsertTrackSql));
  ATX_TRY_VOID(stmt->bind(1, key.hex()));
  ATX_TRY_VOID(stmt->bind(2, meta.underlier));
  ATX_TRY_VOID(stmt->bind(3, meta.family));
  ATX_TRY_VOID(stmt->bind(4, registration.config_json));
  ATX_TRY_VOID(stmt->bind(5, registration.engine_id));
  ATX_TRY_VOID(stmt->bind(6, registration.economics_rev));
  ATX_TRY_VOID(stmt->bind(7, registration.data_snapshot_id));
  ATX_TRY_VOID(stmt->bind(8, registration.date_min));
  ATX_TRY_VOID(stmt->bind(9, registration.date_max));
  ATX_TRY_VOID(stmt->bind(10, to_string(TrackStatus::Staging)));
  ATX_TRY_VOID(stmt->bind_null(11)); // file: NULL while staging
  ATX_TRY_VOID(stmt->bind_null(12)); // row_group: NULL while staging
  ATX_TRY_VOID(stmt->bind(13, now));
  ATX_TRY_VOID(stmt->bind(14, now));

  auto step = stmt->step();
  if (!step) {
    // PRIMARY KEY conflict on a re-registration of the same key maps to
    // AlreadyExists (atx::core::db's SQLITE_CONSTRAINT mapping) -- do not
    // reset-and-swallow, propagate it (still resetting first, so the cached
    // statement is not left mid-error for the next caller).
    ATX_TRY_VOID(stmt->reset());
    return Err(step.error());
  }
  ATX_TRY_VOID(stmt->reset());

  // Economics-rev supersession (Task D5) -- see kRetireSupersededSql's own
  // comment for the exact match/compare rule. Runs AFTER the insert above so
  // the fresh row already exists (irrelevant to the UPDATE's own WHERE
  // clause, which only ever touches OTHER rows: their economics_rev is
  // strictly less than this row's, so this row itself can never match).
  ATX_TRY(auto *retire_stmt, db_.prepare_cached(kRetireSupersededSql));
  ATX_TRY_VOID(retire_stmt->bind(1, to_string(TrackStatus::Retired)));
  ATX_TRY_VOID(retire_stmt->bind(2, meta.underlier));
  ATX_TRY_VOID(retire_stmt->bind(3, meta.family));
  ATX_TRY_VOID(retire_stmt->bind(4, registration.config_json));
  ATX_TRY_VOID(retire_stmt->bind(5, registration.data_snapshot_id));
  ATX_TRY_VOID(retire_stmt->bind(6, registration.economics_rev));
  auto retire_step = retire_stmt->step();
  if (!retire_step) {
    ATX_TRY_VOID(retire_stmt->reset());
    return Err(retire_step.error());
  }
  ATX_TRY_VOID(retire_stmt->reset());

  // Commits both statements atomically. Any early `return Err(...)` above
  // leaves `txn` un-committed -- its destructor issues ROLLBACK, so a
  // failure at either statement leaves NEITHER applied (no fresh row
  // registered with its predecessor left un-retired, and no predecessor
  // retired against a fresh row that never actually landed).
  return txn.commit();
}

Status Catalog::mark_compacted(const TrackKey &key, std::string_view file,
                               std::int64_t row_group) {
  if (file.empty()) {
    return Err(ErrorCode::InvalidArgument, "Catalog::mark_compacted: empty file");
  }

  ATX_TRY(auto existing, probe(key));
  if (!existing.has_value()) {
    return Err(ErrorCode::NotFound, "Catalog::mark_compacted: no such track");
  }
  if (existing->status != TrackStatus::Staging) {
    return Err(ErrorCode::InvalidArgument,
               "Catalog::mark_compacted: track is not staging (status=" +
                   std::string(to_string(existing->status)) + ")");
  }

  // The WHERE clause repeats the `status = 'staging'` guard atomically at
  // write time -- the probe() above is only what makes the ERROR MESSAGE
  // specific (NotFound vs "not staging"); this UPDATE is what actually
  // enforces it against a writer that changed the row between the two calls
  // (a different Catalog handle -- this process or another; WAL makes THIS
  // connection's own two calls trivially safe, since one Database is never
  // shared across threads/concurrent callers).
  ATX_TRY(auto *stmt, db_.prepare_cached(kMarkCompactedSql));
  ATX_TRY_VOID(stmt->bind(1, to_string(TrackStatus::Compacted)));
  ATX_TRY_VOID(stmt->bind(2, file));
  ATX_TRY_VOID(stmt->bind(3, row_group));
  ATX_TRY_VOID(stmt->bind(4, key.hex()));
  ATX_TRY_VOID(stmt->bind(5, to_string(TrackStatus::Staging)));

  auto step = stmt->step();
  if (!step) {
    ATX_TRY_VOID(stmt->reset());
    return Err(step.error());
  }
  const std::int64_t changed = db_.changes();
  ATX_TRY_VOID(stmt->reset());
  if (changed == 0) {
    // The pre-check above passed, but a DIFFERENT writer (another Catalog
    // handle -- this process or another) flipped the row's status between
    // that read and this write. Report it as a live conflict, not a
    // deterministic "already compacted" -- the caller's retry is the right
    // next step, exactly like BacktestDb/SurfaceDb's OCC conflict (Task
    // D3's other half, detail/writer_lock.hpp's callers).
    return Err(ErrorCode::Unavailable,
               "Catalog::mark_compacted: track status changed concurrently between probe and "
               "update; retry");
  }
  return Ok();
}

Result<std::int64_t> Catalog::record_trial(const TrackKey &track_key, std::string_view sweep_id,
                                           std::optional<double> sharpe) {
  if (sweep_id.empty()) {
    return Err(ErrorCode::InvalidArgument, "Catalog::record_trial: empty sweep_id");
  }
  ATX_TRY(auto *stmt, db_.prepare_cached(kInsertTrialSql));
  ATX_TRY_VOID(stmt->bind(1, track_key.hex()));
  ATX_TRY_VOID(stmt->bind(2, sweep_id));
  if (sharpe.has_value()) {
    ATX_TRY_VOID(stmt->bind(3, *sharpe));
  } else {
    ATX_TRY_VOID(stmt->bind_null(3));
  }
  ATX_TRY_VOID(stmt->bind(4, wall_clock_ns()));

  auto step = stmt->step();
  if (!step) {
    ATX_TRY_VOID(stmt->reset());
    return Err(step.error());
  }
  const std::int64_t trial_id = db_.last_insert_rowid();
  ATX_TRY_VOID(stmt->reset());
  return Ok(trial_id);
}

Result<TrialStats> Catalog::trial_stats(std::string_view sweep_id) {
  TrialStats stats;

  ATX_TRY(auto *count_stmt, db_.prepare_cached(kCountTrialsSql));
  ATX_TRY_VOID(count_stmt->bind(1, sweep_id));
  ATX_TRY(const auto count_step, count_stmt->step());
  if (count_step != db::Statement::Step::Row) {
    ATX_TRY_VOID(count_stmt->reset());
    return Err(ErrorCode::Internal, "Catalog::trial_stats: COUNT query returned no row");
  }
  const std::int64_t n_trials = count_stmt->column_int(0);
  ATX_TRY_VOID(count_stmt->reset());
  stats.n_trials = static_cast<std::uint64_t>(n_trials);
  if (n_trials == 0) {
    return Ok(stats);
  }

  ATX_TRY(auto *sel_stmt, db_.prepare_cached(kSelectTrialSharpesSql));
  ATX_TRY_VOID(sel_stmt->bind(1, sweep_id));
  std::vector<double> sharpes;
  for (;;) {
    ATX_TRY(const auto step, sel_stmt->step());
    if (step == db::Statement::Step::Done) {
      break;
    }
    sharpes.push_back(sel_stmt->column_double(0));
  }
  ATX_TRY_VOID(sel_stmt->reset());

  // Sample (Bessel-corrected) variance -- needs at least 2 observations;
  // fewer reports 0.0, not NaN (see the header's doc comment on why).
  if (sharpes.size() < 2) {
    stats.sr_variance = 0.0;
    return Ok(stats);
  }
  double mean = 0.0;
  for (const double v : sharpes) {
    mean += v;
  }
  mean /= static_cast<double>(sharpes.size());
  double sum_sq = 0.0;
  for (const double v : sharpes) {
    const double d = v - mean;
    sum_sq += d * d;
  }
  stats.sr_variance = sum_sq / static_cast<double>(sharpes.size() - 1);
  return Ok(stats);
}

// ── Task D6: retention/GC support ───────────────────────────────────────

Result<std::int64_t> Catalog::retire_stale(std::int64_t older_than_ts_ns) {
  ATX_TRY(auto *stmt, db_.prepare_cached(kRetireStaleSql));
  ATX_TRY_VOID(stmt->bind(1, to_string(TrackStatus::Retired)));
  ATX_TRY_VOID(stmt->bind(2, to_string(TrackStatus::Compacted)));
  ATX_TRY_VOID(stmt->bind(3, older_than_ts_ns));
  auto step = stmt->step();
  if (!step) {
    ATX_TRY_VOID(stmt->reset());
    return Err(step.error());
  }
  const std::int64_t changed = db_.changes();
  ATX_TRY_VOID(stmt->reset());
  return Ok(changed);
}

Result<std::vector<TrackRow>> Catalog::rows_by_file(std::string_view file) {
  std::vector<TrackRow> rows;
  ATX_TRY(auto *stmt, db_.prepare_cached(kRowsByFileSql));
  ATX_TRY_VOID(stmt->bind(1, file));
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == db::Statement::Step::Done) {
      break;
    }
    Result<TrackRow> row = row_from_statement(*stmt);
    if (!row.has_value()) {
      ATX_TRY_VOID(stmt->reset());
      return Err(row.error());
    }
    rows.push_back(std::move(*row));
  }
  ATX_TRY_VOID(stmt->reset());
  return Ok(std::move(rows));
}

Result<std::int64_t> Catalog::mark_reader(std::string_view file) {
  if (file.empty()) {
    return Err(ErrorCode::InvalidArgument, "Catalog::mark_reader: empty file");
  }
  ATX_TRY(auto *stmt, db_.prepare_cached(kInsertReaderMarkSql));
  ATX_TRY_VOID(stmt->bind(1, file));
  ATX_TRY_VOID(stmt->bind(2, static_cast<std::int64_t>(detail::current_process_id())));
  ATX_TRY_VOID(stmt->bind(3, wall_clock_ns()));
  auto step = stmt->step();
  if (!step) {
    ATX_TRY_VOID(stmt->reset());
    return Err(step.error());
  }
  const std::int64_t mark_id = db_.last_insert_rowid();
  ATX_TRY_VOID(stmt->reset());
  return Ok(mark_id);
}

Status Catalog::release_reader_mark(std::int64_t mark_id) {
  ATX_TRY(auto *stmt, db_.prepare_cached(kDeleteReaderMarkByIdSql));
  ATX_TRY_VOID(stmt->bind(1, mark_id));
  auto step = stmt->step();
  if (!step) {
    ATX_TRY_VOID(stmt->reset());
    return Err(step.error());
  }
  ATX_TRY_VOID(stmt->reset());
  return Ok();
}

Result<bool> Catalog::has_live_reader_mark(std::string_view file) {
  ATX_TRY(auto *select_stmt, db_.prepare_cached(kSelectReaderMarksForFileSql));
  ATX_TRY_VOID(select_stmt->bind(1, file));
  std::vector<std::pair<std::int64_t, std::int64_t>> marks; // (mark_id, pid)
  for (;;) {
    ATX_TRY(const auto step, select_stmt->step());
    if (step == db::Statement::Step::Done) {
      break;
    }
    marks.emplace_back(select_stmt->column_int(0), select_stmt->column_int(1));
  }
  ATX_TRY_VOID(select_stmt->reset());

  bool any_live = false;
  for (const auto &[mark_id, pid] : marks) {
    if (detail::process_alive(static_cast<std::uint64_t>(pid))) {
      any_live = true;
      continue;
    }
    // Confirmed-dead owner -- opportunistic cleanup, mirroring
    // detail::WriterLock's own stale-owner takeover. Best-effort: a failure
    // here does not change the liveness ANSWER (this mark was already
    // established dead), only whether the row lingers to be swept next time.
    ATX_TRY(auto *delete_stmt, db_.prepare_cached(kDeleteReaderMarkByIdSql));
    ATX_TRY_VOID(delete_stmt->bind(1, mark_id));
    static_cast<void>(delete_stmt->step());
    ATX_TRY_VOID(delete_stmt->reset());
  }
  return Ok(any_live);
}

Status Catalog::apply_gc_rewrite(std::string_view old_file, std::string_view new_file,
                                 std::span<const std::string> retired_keys,
                                 std::span<const std::string> kept_keys) {
  if (old_file.empty()) {
    return Err(ErrorCode::InvalidArgument, "Catalog::apply_gc_rewrite: empty old_file");
  }
  if (new_file.empty() && !kept_keys.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "Catalog::apply_gc_rewrite: kept_keys non-empty but new_file empty -- a batch "
               "delete must have no survivors");
  }
  if (retired_keys.empty() && kept_keys.empty()) {
    return Err(ErrorCode::InvalidArgument, "Catalog::apply_gc_rewrite: nothing to apply");
  }

  ATX_TRY(db::Transaction txn, db::Transaction::begin_immediate(db_));

  // Review finding (Important): a per-key UPDATE that matches ZERO rows is
  // not a SQLite error -- WHERE track_key=? AND status=? AND file=? simply
  // no-ops if a CONCURRENT writer changed that row's status/file between the
  // caller's own rows_by_file() snapshot and this call (e.g. a D5
  // economics-rev supersession retiring a `kept_keys` row mid-rewrite: its
  // status flips to Retired while `file` still points at `old_file`). A
  // silent no-op here would let the caller (track_gc.cpp) go on to
  // physically delete `old_file` anyway, leaving that row Retired with a
  // non-null `file` pointing at bytes that no longer exist -- exactly the
  // dangling-pointer state TrackStatus::Retired's own doc comment claims
  // this function prevents. Every per-key UPDATE must therefore affect
  // EXACTLY one row or the whole call fails closed (txn's destructor rolls
  // back on this early return, so nothing partial is ever committed) --
  // the caller sees the conflict and must re-snapshot and retry rather than
  // proceed to delete anything.
  if (!retired_keys.empty()) {
    ATX_TRY(auto *clear_stmt, db_.prepare_cached(kClearReclaimedFileSql));
    for (const std::string &key : retired_keys) {
      ATX_TRY_VOID(clear_stmt->bind(1, key));
      ATX_TRY_VOID(clear_stmt->bind(2, to_string(TrackStatus::Retired)));
      ATX_TRY_VOID(clear_stmt->bind(3, old_file));
      auto step = clear_stmt->step();
      if (!step) {
        ATX_TRY_VOID(clear_stmt->reset());
        return Err(step.error());
      }
      const std::int64_t changed = db_.changes();
      ATX_TRY_VOID(clear_stmt->reset());
      if (changed != 1) {
        return Err(ErrorCode::Internal,
                   "Catalog::apply_gc_rewrite: retired row changed concurrently -- expected to "
                   "clear track_key=" +
                       key + " status=retired file=" + std::string(old_file) +
                       ", matched " + std::to_string(changed) + " row(s); retry against a fresh scan");
      }
    }
  }

  if (!kept_keys.empty()) {
    ATX_TRY(auto *repoint_stmt, db_.prepare_cached(kRepointRewrittenFileSql));
    for (const std::string &key : kept_keys) {
      ATX_TRY_VOID(repoint_stmt->bind(1, new_file));
      ATX_TRY_VOID(repoint_stmt->bind(2, key));
      ATX_TRY_VOID(repoint_stmt->bind(3, to_string(TrackStatus::Compacted)));
      ATX_TRY_VOID(repoint_stmt->bind(4, old_file));
      auto step = repoint_stmt->step();
      if (!step) {
        ATX_TRY_VOID(repoint_stmt->reset());
        return Err(step.error());
      }
      const std::int64_t changed = db_.changes();
      ATX_TRY_VOID(repoint_stmt->reset());
      if (changed != 1) {
        return Err(ErrorCode::Internal,
                   "Catalog::apply_gc_rewrite: surviving row changed concurrently -- expected to "
                   "repoint track_key=" +
                       key + " status=compacted file=" + std::string(old_file) +
                       ", matched " + std::to_string(changed) + " row(s); retry against a fresh scan");
      }
    }
  }

  return txn.commit();
}

} // namespace atx::vol
