#pragma once
// atx::engine::store::PipelineRecorder — write path for one pipeline run.
// Header-only, inline, mirrors run_recorder.hpp style exactly.
//
// begin()          — INSERT pipeline_run + 'started' event (one Transaction).
// find_resumable() — SELECT the open run + last checkpoint generation.
// resume()         — UPDATE status='resumed' + 'resumed' event; return recorder.
// save_checkpoint()— ONE Transaction: checkpoint + iteration + run UPDATE + 2 events.
// latest_population_blob() — last checkpoint blob.
// heartbeat()      — UPDATE last_heartbeat_at.
// log()            — INSERT pipeline_log.
// event()          — plain INSERT pipeline_event (NO internal Transaction — composes
//                    cleanly inside save_checkpoint's Transaction).
// complete()       — UPDATE status='completed' + 'completed' event.
// mark_failed()    — UPDATE status='failed' + 'failed' event + log("error", ...).
//
// Free helpers: join_population / split_population / population_hash.

#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/store/fingerprint.hpp"

namespace atx::engine::store {

// ---------------------------------------------------------------------------
// Row types
// ---------------------------------------------------------------------------

struct PipelineRunRow {
  std::string pipeline_run_id;
  atx::u64    fingerprint{0};
  std::string stage;
  atx::u64    master_seed{0};
  atx::i64    population{0};
  atx::i64    total_generations{0};
  std::string panel_path;
  std::string config_json;
  std::string engine_git_sha;
  atx::i64    created_at{0};
};

struct ResumableRun {
  std::string pipeline_run_id;
  atx::i64    last_generation{-1};
};

// The full cross-generation accumulated search state ENTERING a generation
// (resumable-discover, Task F1). The blobs are produced by the engine's bit-exact,
// deterministically-ordered serializers (search_progress.hpp); the store treats them
// as opaque TEXT and round-trips them losslessly. DEFAULT-constructed (all empty / 0)
// is a legacy population-only checkpoint that restores nothing.
struct CheckpointState {
  std::string canon_blob;
  std::string cache_blob;
  std::string archive_blob;
  std::string best_per_gen_blob;
  atx::u64    digest{0};
  atx::i64    candidates_generated{0};
};

// A full checkpoint read back from the store (population + accumulated state).
struct CheckpointRow {
  std::string population_blob;
  CheckpointState state;
};

// ---------------------------------------------------------------------------
// Free blob helpers (same namespace)
// ---------------------------------------------------------------------------

/// Join a list of population expressions into a single '\n'-delimited blob.
[[nodiscard]] inline std::string join_population(const std::vector<std::string>& v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) out += '\n';
    out += v[i];
  }
  return out;
}

/// Split a '\n'-delimited blob back into individual expressions.
/// Trailing empty element (from a trailing '\n') is dropped.
[[nodiscard]] inline std::vector<std::string> split_population(std::string_view blob) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= blob.size()) {
    auto end = blob.find('\n', start);
    if (end == std::string_view::npos) {
      out.emplace_back(blob.substr(start));
      break;
    }
    out.emplace_back(blob.substr(start, end - start));
    start = end + 1;
  }
  // Drop trailing empty string
  if (!out.empty() && out.back().empty()) {
    out.pop_back();
  }
  return out;
}

/// FNV-1a 64-bit hash of the blob bytes.
[[nodiscard]] inline atx::u64 population_hash(std::string_view blob) {
  return fingerprint::fold_bytes(fingerprint::kFnvOffset, blob);
}

/// state_hash over the FULL checkpoint payload (resumable-discover, Task F1): the
/// population blob AND every accumulated-state blob + the digest + the
/// candidates_generated counter. Folding ALL of it means latest_checkpoint can verify
/// the whole resume payload (not just the population) against tampering/corruption,
/// extending the prior population-only guard. The fold order is fixed (deterministic).
/// A '\x1f' (unit separator) is folded between fields so distinct field boundaries
/// cannot alias (e.g. ("ab","") vs ("a","b")).
[[nodiscard]] inline atx::u64
checkpoint_state_hash(std::string_view population_blob, std::string_view canon_blob,
                      std::string_view cache_blob, std::string_view archive_blob,
                      std::string_view best_per_gen_blob, atx::u64 digest,
                      atx::i64 candidates_generated) {
  atx::u64 h = fingerprint::kFnvOffset;
  const std::string_view sep{"\x1f", 1};
  h = fingerprint::fold_bytes(h, population_blob);
  h = fingerprint::fold_bytes(h, sep);
  h = fingerprint::fold_bytes(h, canon_blob);
  h = fingerprint::fold_bytes(h, sep);
  h = fingerprint::fold_bytes(h, cache_blob);
  h = fingerprint::fold_bytes(h, sep);
  h = fingerprint::fold_bytes(h, archive_blob);
  h = fingerprint::fold_bytes(h, sep);
  h = fingerprint::fold_bytes(h, best_per_gen_blob);
  h = fingerprint::fold_u64(h, digest);
  h = fingerprint::fold_u64(h, static_cast<atx::u64>(candidates_generated));
  return h;
}

// ---------------------------------------------------------------------------
// PipelineRecorder
// ---------------------------------------------------------------------------

class PipelineRecorder {
public:
  // -------------------------------------------------------------------------
  // Static factories
  // -------------------------------------------------------------------------

  /// INSERT pipeline_run + 'started' event in a single Transaction.
  /// Returns Err(AlreadyExists) if the fingerprint UNIQUE constraint fires.
  [[nodiscard]] static atx::core::Result<PipelineRecorder>
  begin(atx::core::db::Database& db, const PipelineRunRow& r) {
    ATX_TRY(auto txn, atx::core::db::Transaction::begin(db));
    {
      ATX_TRY(auto* stmt, db.prepare_cached(
          "INSERT INTO pipeline_run("
          " pipeline_run_id, fingerprint, stage, status, master_seed, population,"
          " total_generations, last_generation, panel_path, config_json, engine_git_sha,"
          " created_at, updated_at, last_heartbeat_at, finished_at)"
          " VALUES (?1,?2,?3,'running',?4,?5,?6,-1,?7,?8,?9,?10,?10,?10,NULL)"));
      ATX_TRY_VOID(stmt->bind(1, r.pipeline_run_id));
      ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(r.fingerprint)));
      ATX_TRY_VOID(stmt->bind(3, r.stage));
      ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(r.master_seed)));
      ATX_TRY_VOID(stmt->bind(5, r.population));
      ATX_TRY_VOID(stmt->bind(6, r.total_generations));
      ATX_TRY_VOID(stmt->bind(7, r.panel_path));
      ATX_TRY_VOID(stmt->bind(8, r.config_json));
      ATX_TRY_VOID(stmt->bind(9, r.engine_git_sha));
      ATX_TRY_VOID(stmt->bind(10, r.created_at));
      ATX_TRY(const auto step, stmt->step());
      if (step != atx::core::db::Statement::Step::Done) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "PipelineRecorder::begin: insert incomplete");
      }
    }
    // 'started' event — plain INSERT (no nested txn — composes inside this txn)
    ATX_TRY_VOID(insert_event(db, r.pipeline_run_id, "started", -1, "", r.created_at));
    ATX_TRY_VOID(txn.commit());
    return atx::core::Ok(PipelineRecorder{db, r.pipeline_run_id});
  }

  /// Find an open (not finished, not completed) run for the given fingerprint.
  /// Returns Err(NotFound) if none exists.
  [[nodiscard]] static atx::core::Result<ResumableRun>
  find_resumable(atx::core::db::Database& db, atx::u64 fp) {
    ATX_TRY(auto* stmt, db.prepare_cached(
        "SELECT pipeline_run_id FROM pipeline_run"
        " WHERE fingerprint=?1 AND finished_at IS NULL AND status<>'completed' LIMIT 1"));
    ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(fp)));
    ATX_TRY(const auto step, stmt->step());
    if (step != atx::core::db::Statement::Step::Row) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "PipelineRecorder::find_resumable: no open run");
    }
    std::string run_id{stmt->column_text(0)};
    // Find latest checkpoint generation
    ATX_TRY(auto* cstmt, db.prepare_cached(
        "SELECT COALESCE(MAX(generation),-1) FROM pipeline_checkpoint"
        " WHERE pipeline_run_id=?1"));
    ATX_TRY_VOID(cstmt->bind(1, run_id));
    ATX_TRY(const auto cstep, cstmt->step());
    if (cstep != atx::core::db::Statement::Step::Row) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "PipelineRecorder::find_resumable: COALESCE returned no row");
    }
    atx::i64 last_gen = cstmt->column_int(0);
    return atx::core::Ok(ResumableRun{std::move(run_id), last_gen});
  }

  /// UPDATE status='resumed', append 'resumed' event, return recorder bound to that id.
  [[nodiscard]] static atx::core::Result<PipelineRecorder>
  resume(atx::core::db::Database& db, std::string_view pipeline_run_id, atx::i64 ts) {
    {
      ATX_TRY(auto* stmt, db.prepare_cached(
          "UPDATE pipeline_run SET status='resumed', last_heartbeat_at=?2, updated_at=?2"
          " WHERE pipeline_run_id=?1"));
      ATX_TRY_VOID(stmt->bind(1, pipeline_run_id));
      ATX_TRY_VOID(stmt->bind(2, ts));
      ATX_TRY(const auto step, stmt->step());
      if (step != atx::core::db::Statement::Step::Done) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "PipelineRecorder::resume: UPDATE incomplete");
      }
    }
    if (db.changes() != 1) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "PipelineRecorder::resume: pipeline_run_id not found");
    }
    ATX_TRY_VOID(insert_event(db, pipeline_run_id, "resumed", -1, "", ts));
    return atx::core::Ok(PipelineRecorder{db, std::string(pipeline_run_id)});
  }

  // -------------------------------------------------------------------------
  // Instance methods
  // -------------------------------------------------------------------------

  /// ONE atomic Transaction: checkpoint + iteration + run UPDATE + 2 events.
  /// `state` carries the FULL cross-generation accumulated search state ENTERING
  /// `generation` (resumable-discover, Task F1). Defaulted so a legacy caller that
  /// only persists the population gets an empty accumulated state (restores nothing).
  /// The state_hash covers the WHOLE payload (population + state) so latest_checkpoint
  /// verifies all of it on resume.
  [[nodiscard]] atx::core::Status
  save_checkpoint(atx::i64 generation, std::string_view population_blob,
                  atx::i64 population_count, atx::f64 best_fitness, atx::f64 mean_fitness,
                  atx::i64 n_evaluated, atx::i64 n_unique, atx::i64 wall_ms, atx::i64 ts,
                  const CheckpointState& state = {}) {
    ATX_TRY(auto txn, atx::core::db::Transaction::begin(db_));
    const auto state_hash = static_cast<atx::i64>(checkpoint_state_hash(
        population_blob, state.canon_blob, state.cache_blob, state.archive_blob,
        state.best_per_gen_blob, state.digest, state.candidates_generated));
    // 1. Checkpoint
    {
      ATX_TRY(auto* stmt, db_.prepare_cached(
          "INSERT OR REPLACE INTO pipeline_checkpoint("
          " pipeline_run_id, generation, population_blob, population_count, state_hash,"
          " canon_blob, cache_blob, archive_blob, best_per_gen_blob, digest,"
          " candidates_generated, created_at)"
          " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)"));
      ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
      ATX_TRY_VOID(stmt->bind(2, generation));
      ATX_TRY_VOID(stmt->bind(3, population_blob));
      ATX_TRY_VOID(stmt->bind(4, population_count));
      ATX_TRY_VOID(stmt->bind(5, state_hash));
      ATX_TRY_VOID(stmt->bind(6, state.canon_blob));
      ATX_TRY_VOID(stmt->bind(7, state.cache_blob));
      ATX_TRY_VOID(stmt->bind(8, state.archive_blob));
      ATX_TRY_VOID(stmt->bind(9, state.best_per_gen_blob));
      ATX_TRY_VOID(stmt->bind(10, static_cast<atx::i64>(state.digest)));
      ATX_TRY_VOID(stmt->bind(11, state.candidates_generated));
      ATX_TRY_VOID(stmt->bind(12, ts));
      ATX_TRY(const auto step, stmt->step());
      if (step != atx::core::db::Statement::Step::Done) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "save_checkpoint: checkpoint insert incomplete");
      }
    }
    // 2. Iteration
    {
      ATX_TRY(auto* stmt, db_.prepare_cached(
          "INSERT OR REPLACE INTO pipeline_iteration("
          " pipeline_run_id, generation, best_fitness, mean_fitness,"
          " n_evaluated, n_unique, wall_ms, ts)"
          " VALUES (?1,?2,?3,?4,?5,?6,?7,?8)"));
      ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
      ATX_TRY_VOID(stmt->bind(2, generation));
      ATX_TRY_VOID(stmt->bind(3, best_fitness));
      ATX_TRY_VOID(stmt->bind(4, mean_fitness));
      ATX_TRY_VOID(stmt->bind(5, n_evaluated));
      ATX_TRY_VOID(stmt->bind(6, n_unique));
      ATX_TRY_VOID(stmt->bind(7, wall_ms));
      ATX_TRY_VOID(stmt->bind(8, ts));
      ATX_TRY(const auto step, stmt->step());
      if (step != atx::core::db::Statement::Step::Done) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "save_checkpoint: iteration insert incomplete");
      }
    }
    // 3. Update pipeline_run
    {
      ATX_TRY(auto* stmt, db_.prepare_cached(
          "UPDATE pipeline_run SET last_generation=?2, updated_at=?3, last_heartbeat_at=?3"
          " WHERE pipeline_run_id=?1"));
      ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
      ATX_TRY_VOID(stmt->bind(2, generation));
      ATX_TRY_VOID(stmt->bind(3, ts));
      ATX_TRY(const auto step, stmt->step());
      if (step != atx::core::db::Statement::Step::Done) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "save_checkpoint: run UPDATE incomplete");
      }
    }
    if (db_.changes() != 1) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "PipelineRecorder::save_checkpoint: pipeline_run_id not found");
    }
    // 4. Two events — plain INSERTs (no nested txn; compose inside this txn)
    ATX_TRY_VOID(insert_event(db_, pipeline_run_id_, "generation_complete", generation, "", ts));
    ATX_TRY_VOID(insert_event(db_, pipeline_run_id_, "checkpoint_saved", generation, "", ts));
    ATX_TRY_VOID(txn.commit());
    return atx::core::Ok();
  }

  /// SELECT the latest FULL checkpoint (population + accumulated state), Err(NotFound)
  /// if none. Verifies the stored state_hash against checkpoint_state_hash over the
  /// WHOLE payload (population + every accumulated-state blob + digest + counter);
  /// returns Err(Internal) on a mismatch (corrupt/tampered checkpoint). This is the
  /// resume read-back path (Task F1).
  [[nodiscard]] atx::core::Result<CheckpointRow> latest_checkpoint() const {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "SELECT population_blob, state_hash, canon_blob, cache_blob, archive_blob,"
        " best_per_gen_blob, digest, candidates_generated FROM pipeline_checkpoint"
        " WHERE pipeline_run_id=?1 ORDER BY generation DESC LIMIT 1"));
    ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
    ATX_TRY(const auto step, stmt->step());
    if (step != atx::core::db::Statement::Step::Row) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "PipelineRecorder::latest_checkpoint: no checkpoint");
    }
    CheckpointRow row;
    row.population_blob = std::string{stmt->column_text(0)};
    const auto stored_hash = static_cast<atx::u64>(stmt->column_int(1));
    row.state.canon_blob = std::string{stmt->column_text(2)};
    row.state.cache_blob = std::string{stmt->column_text(3)};
    row.state.archive_blob = std::string{stmt->column_text(4)};
    row.state.best_per_gen_blob = std::string{stmt->column_text(5)};
    row.state.digest = static_cast<atx::u64>(stmt->column_int(6));
    row.state.candidates_generated = stmt->column_int(7);
    const auto recomputed = checkpoint_state_hash(
        row.population_blob, row.state.canon_blob, row.state.cache_blob,
        row.state.archive_blob, row.state.best_per_gen_blob, row.state.digest,
        row.state.candidates_generated);
    if (recomputed != stored_hash) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "PipelineRecorder::latest_checkpoint: state_hash mismatch"
                            " (corrupt checkpoint payload)");
    }
    return atx::core::Ok(std::move(row));
  }

  /// SELECT the latest population blob, Err(NotFound) if none. Verifies the FULL
  /// payload state_hash (delegates to latest_checkpoint) and returns just the
  /// population blob — the legacy convenience accessor.
  [[nodiscard]] atx::core::Result<std::string> latest_population_blob() const {
    ATX_TRY(auto row, latest_checkpoint());
    return atx::core::Ok(std::move(row.population_blob));
  }

  /// UPDATE last_heartbeat_at + updated_at.
  [[nodiscard]] atx::core::Status heartbeat(atx::i64 ts) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "UPDATE pipeline_run SET last_heartbeat_at=?2, updated_at=?2 WHERE pipeline_run_id=?1"));
    ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
    ATX_TRY_VOID(stmt->bind(2, ts));
    return step_done(*stmt, "heartbeat");
  }

  /// INSERT pipeline_log row.
  [[nodiscard]] atx::core::Status
  log(std::string_view level, atx::i64 generation, std::string_view message, atx::i64 ts) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "INSERT INTO pipeline_log(pipeline_run_id, ts, level, generation, message)"
        " VALUES (?1,?2,?3,?4,?5)"));
    ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
    ATX_TRY_VOID(stmt->bind(2, ts));
    ATX_TRY_VOID(stmt->bind(3, level));
    ATX_TRY_VOID(stmt->bind(4, generation));
    ATX_TRY_VOID(stmt->bind(5, message));
    return step_done(*stmt, "log");
  }

  /// Plain INSERT into pipeline_event — NO internal Transaction so it composes
  /// cleanly inside save_checkpoint's Transaction (and any other caller txn).
  [[nodiscard]] atx::core::Status
  event(std::string_view event_type, atx::i64 generation,
        std::string_view payload, atx::i64 ts) {
    return insert_event(db_, pipeline_run_id_, event_type, generation, payload, ts);
  }

  /// UPDATE status='completed', finished_at, + 'completed' event.
  [[nodiscard]] atx::core::Status complete(atx::i64 ts) {
    {
      ATX_TRY(auto* stmt, db_.prepare_cached(
          "UPDATE pipeline_run SET status='completed', finished_at=?2, updated_at=?2"
          " WHERE pipeline_run_id=?1"));
      ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
      ATX_TRY_VOID(stmt->bind(2, ts));
      ATX_TRY_VOID(step_done(*stmt, "complete:step"));
    }
    if (db_.changes() != 1) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "PipelineRecorder::complete: pipeline_run_id not found");
    }
    return insert_event(db_, pipeline_run_id_, "completed", -1, "", ts);
  }

  /// UPDATE status='failed', finished_at, + 'failed' event + log("error", -1, message).
  [[nodiscard]] atx::core::Status mark_failed(atx::i64 ts, std::string_view message) {
    {
      ATX_TRY(auto* stmt, db_.prepare_cached(
          "UPDATE pipeline_run SET status='failed', finished_at=?2, updated_at=?2"
          " WHERE pipeline_run_id=?1"));
      ATX_TRY_VOID(stmt->bind(1, pipeline_run_id_));
      ATX_TRY_VOID(stmt->bind(2, ts));
      ATX_TRY_VOID(step_done(*stmt, "mark_failed:step"));
    }
    if (db_.changes() != 1) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "PipelineRecorder::mark_failed: pipeline_run_id not found");
    }
    ATX_TRY_VOID(insert_event(db_, pipeline_run_id_, "failed", -1, message, ts));
    return log("error", -1, message, ts);
  }

private:
  PipelineRecorder(atx::core::db::Database& db, std::string pipeline_run_id)
      : db_{db}, pipeline_run_id_{std::move(pipeline_run_id)} {}

  /// Shared plain-INSERT helper for pipeline_event — no Transaction, composes anywhere.
  [[nodiscard]] static atx::core::Status
  insert_event(atx::core::db::Database& db, std::string_view run_id,
               std::string_view event_type, atx::i64 generation,
               std::string_view payload, atx::i64 ts) {
    ATX_TRY(auto* stmt, db.prepare_cached(
        "INSERT INTO pipeline_event(pipeline_run_id, ts, event_type, generation, payload)"
        " VALUES (?1,?2,?3,?4,?5)"));
    ATX_TRY_VOID(stmt->bind(1, run_id));
    ATX_TRY_VOID(stmt->bind(2, ts));
    ATX_TRY_VOID(stmt->bind(3, event_type));
    ATX_TRY_VOID(stmt->bind(4, generation));
    ATX_TRY_VOID(stmt->bind(5, payload));
    return step_done(*stmt, "insert_event");
  }

  [[nodiscard]] static atx::core::Status
  step_done(atx::core::db::Statement& stmt, const char* who) {
    ATX_TRY(const auto step, stmt.step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, who);
    }
    return atx::core::Ok();
  }

  atx::core::db::Database& db_;
  std::string               pipeline_run_id_;
};

}  // namespace atx::engine::store
