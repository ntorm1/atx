#pragma once
// atx::engine::store::schema — greenfield v2 DDL for the alpha-lifecycle persistence
// layer. All tables are CREATE TABLE IF NOT EXISTS so create_all is idempotent across
// reopen. Append-only tables (alpha_event, lifecycle_journal) use AUTOINCREMENT PK.

#include "atx/core/db/sqlite.hpp" // db::Database
#include "atx/core/error.hpp"     // Status, Ok, ATX_TRY_VOID

namespace atx::engine::store::schema {

inline constexpr int kSchemaVersion = 2;

// Create every v2 table if absent and stamp schema_meta. Idempotent.
[[nodiscard]] inline atx::core::Status create_all(atx::core::db::Database& db) {
  ATX_TRY_VOID(db.exec(
    "CREATE TABLE IF NOT EXISTS alpha ("
    " canon_hash INTEGER PRIMARY KEY, alpha_id INTEGER UNIQUE, expr_source TEXT NOT NULL,"
    " created_at INTEGER NOT NULL, first_run_id TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS alpha_lineage ("
    " child_hash INTEGER NOT NULL, parent_hash INTEGER NOT NULL,"
    " mutation_op INTEGER NOT NULL, seed INTEGER NOT NULL,"
    " PRIMARY KEY(child_hash, parent_hash));"
    "CREATE TABLE IF NOT EXISTS universe ("
    " universe_id TEXT PRIMARY KEY, name TEXT, as_of INTEGER, rule TEXT, content_hash INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS universe_member ("
    " universe_id TEXT NOT NULL, instrument_id INTEGER NOT NULL,"
    " PRIMARY KEY(universe_id, instrument_id));"
    "CREATE TABLE IF NOT EXISTS data_snapshot ("
    " snapshot_id TEXT PRIMARY KEY, source TEXT, as_of INTEGER, content_hash INTEGER);"
    "CREATE TABLE IF NOT EXISTS run ("
    " run_id TEXT PRIMARY KEY, run_fingerprint INTEGER UNIQUE, kind TEXT NOT NULL,"
    " status TEXT NOT NULL, engine_git_sha TEXT, master_seed INTEGER,"
    " universe_id TEXT, snapshot_id TEXT, fit_start INTEGER, fit_end INTEGER,"
    " bt_start INTEGER, bt_end INTEGER, position_mode TEXT, sector_neutral INTEGER,"
    " rebalance_every INTEGER, cost_model TEXT, manifest_version_id INTEGER,"
    " result_digest INTEGER, started_at INTEGER, finished_at INTEGER);"
    "CREATE TABLE IF NOT EXISTS run_param ("
    " run_id TEXT NOT NULL, key TEXT NOT NULL, value TEXT, PRIMARY KEY(run_id, key));"
    "CREATE TABLE IF NOT EXISTS run_alpha ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, role TEXT NOT NULL,"
    " PRIMARY KEY(run_id, canon_hash));"
    "CREATE TABLE IF NOT EXISTS alpha_metrics ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, sharpe REAL, returns REAL,"
    " drawdown REAL, turnover REAL, margin REAL, fitness REAL,"
    " PRIMARY KEY(run_id, canon_hash));"
    "CREATE TABLE IF NOT EXISTS eval_fold ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, fold_id INTEGER NOT NULL,"
    " sharpe REAL, returns REAL, n_test INTEGER, PRIMARY KEY(run_id, canon_hash, fold_id));"
    "CREATE TABLE IF NOT EXISTS conviction ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, dsr REAL, pbo REAL,"
    " stability REAL, explain_flag INTEGER, score REAL, PRIMARY KEY(run_id, canon_hash));"
    "CREATE TABLE IF NOT EXISTS alpha_event ("
    " event_id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,"
    " canon_hash INTEGER NOT NULL, event_type TEXT NOT NULL, run_id TEXT,"
    " actor TEXT NOT NULL, payload TEXT);"
    "CREATE TABLE IF NOT EXISTS lifecycle_journal ("
    " seq INTEGER PRIMARY KEY AUTOINCREMENT, canon_hash INTEGER NOT NULL,"
    " from_state INTEGER NOT NULL, to_state INTEGER NOT NULL,"
    " as_of_period INTEGER NOT NULL, run_id TEXT);"
    "CREATE TABLE IF NOT EXISTS env_config (key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS promotion_ledger ("
    " promo_id INTEGER PRIMARY KEY AUTOINCREMENT, canon_hash INTEGER NOT NULL,"
    " from_env TEXT, to_env TEXT, justifying_run_id TEXT, approved_by TEXT, ts INTEGER);"
    "CREATE TABLE IF NOT EXISTS segment ("
    " segment_id TEXT PRIMARY KEY, path TEXT, content_hash INTEGER, base_alpha_id INTEGER,"
    " n_alphas INTEGER, crc INTEGER, format_version INTEGER, created_by_run_id TEXT);"
    "CREATE TABLE IF NOT EXISTS segment_alpha ("
    " canon_hash INTEGER NOT NULL, segment_id TEXT NOT NULL, dir_index INTEGER NOT NULL,"
    " PRIMARY KEY(canon_hash, segment_id));"
    "CREATE INDEX IF NOT EXISTS ix_alpha_event_hash ON alpha_event(canon_hash, ts, event_id);"
    "CREATE INDEX IF NOT EXISTS ix_run_alpha_hash ON run_alpha(canon_hash);"
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    " schema_version INTEGER NOT NULL, engine_version TEXT, applied_at INTEGER);"));
  // v2 pipeline progress tables.
  ATX_TRY_VOID(db.exec(
    "CREATE TABLE IF NOT EXISTS pipeline_run ("
    " pipeline_run_id   TEXT PRIMARY KEY,"
    " fingerprint       INTEGER UNIQUE NOT NULL,"
    " stage             TEXT NOT NULL,"
    " status            TEXT NOT NULL,"
    " master_seed       INTEGER NOT NULL,"
    " population        INTEGER NOT NULL,"
    " total_generations INTEGER NOT NULL,"
    " last_generation   INTEGER NOT NULL DEFAULT -1,"
    " panel_path        TEXT,"
    " config_json       TEXT,"
    " engine_git_sha    TEXT,"
    " created_at        INTEGER NOT NULL,"
    " updated_at        INTEGER NOT NULL,"
    " last_heartbeat_at INTEGER NOT NULL,"
    " finished_at       INTEGER);"
    "CREATE TABLE IF NOT EXISTS pipeline_checkpoint ("
    " pipeline_run_id  TEXT NOT NULL,"
    " generation       INTEGER NOT NULL,"
    " population_blob  TEXT NOT NULL,"
    " population_count INTEGER NOT NULL,"
    " state_hash       INTEGER NOT NULL,"
    " created_at       INTEGER NOT NULL,"
    " PRIMARY KEY (pipeline_run_id, generation));"
    "CREATE TABLE IF NOT EXISTS pipeline_iteration ("
    " pipeline_run_id  TEXT NOT NULL,"
    " generation       INTEGER NOT NULL,"
    " best_fitness     REAL,"
    " mean_fitness     REAL,"
    " n_evaluated      INTEGER,"
    " n_unique         INTEGER,"
    " wall_ms          INTEGER,"
    " ts               INTEGER NOT NULL,"
    " PRIMARY KEY (pipeline_run_id, generation));"
    "CREATE TABLE IF NOT EXISTS pipeline_event ("
    " event_id        INTEGER PRIMARY KEY AUTOINCREMENT,"
    " pipeline_run_id TEXT NOT NULL,"
    " ts              INTEGER NOT NULL,"
    " event_type      TEXT NOT NULL,"
    " generation      INTEGER,"
    " payload         TEXT);"
    "CREATE TABLE IF NOT EXISTS pipeline_log ("
    " log_id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    " pipeline_run_id TEXT NOT NULL,"
    " ts              INTEGER NOT NULL,"
    " level           TEXT NOT NULL,"
    " generation      INTEGER,"
    " message         TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS ix_pipeline_event_run ON pipeline_event(pipeline_run_id, ts, event_id);"
    "CREATE INDEX IF NOT EXISTS ix_pipeline_log_run   ON pipeline_log(pipeline_run_id, ts, log_id);"));
  // Upward upsert: insert on first create, update version on reopen (v1 → v2).
  ATX_TRY(auto* cnt_stmt, db.prepare_cached("SELECT COUNT(*) FROM schema_meta"));
  ATX_TRY(const auto step, cnt_stmt->step());
  if (step != atx::core::db::Statement::Step::Row) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "schema::create_all: COUNT(*) returned no row");
  }
  if (cnt_stmt->column_int(0) == 0) {
    ATX_TRY(auto* ins, db.prepare_cached(
        "INSERT INTO schema_meta(schema_version, engine_version, applied_at) VALUES (?1, 'v2', 0)"));
    ATX_TRY_VOID(ins->bind(1, static_cast<atx::i64>(kSchemaVersion)));
    ATX_TRY(const auto ins_step, ins->step());
    if (ins_step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "schema::create_all: schema_meta stamp insert incomplete");
    }
  } else {
    ATX_TRY(auto* upd, db.prepare_cached(
        "UPDATE schema_meta SET schema_version = ?1, engine_version = 'v2'"));
    ATX_TRY_VOID(upd->bind(1, static_cast<atx::i64>(kSchemaVersion)));
    ATX_TRY(const auto upd_step, upd->step());
    if (upd_step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "schema::create_all: schema_meta restamp incomplete");
    }
  }
  return atx::core::Ok();
}

}  // namespace atx::engine::store::schema
