#pragma once
// atx::engine::store::RunRecorder — write path for one run. begin() rejects a replay
// (committed fingerprint already present) and inserts the run with status='running';
// commit() flips it to 'committed'. A crash before commit leaves status='running'
// (GC-able). One writer; the caller owns the Database (one per thread).

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/store/fingerprint.hpp"

namespace atx::engine::store {

struct RunRow {
  std::string run_id;
  atx::u64 fingerprint{0};
  std::string kind;
  std::string engine_git_sha;
  atx::u64 master_seed{0};
  std::string universe_id;
  std::string snapshot_id;
  atx::i64 fit_start{0}, fit_end{0}, bt_start{0}, bt_end{0};
  std::string position_mode;
  bool sector_neutral{false};
  atx::i64 rebalance_every{0};
  std::string cost_model;
  atx::i64 started_at{0};
};

struct AlphaMetricsRow {
  atx::u64 canon_hash{0};
  double sharpe{0}, returns{0}, drawdown{0}, turnover{0}, margin{0}, fitness{0};
};

class RunRecorder {
public:
  [[nodiscard]] static atx::core::Result<RunRecorder>
  begin(atx::core::db::Database& db, const RunRow& r) {
    ATX_TRY(const bool replay, fingerprint::is_replay(db, r.fingerprint));
    if (replay) {
      return atx::core::Err(atx::core::ErrorCode::AlreadyExists,
                            "RunRecorder::begin: fingerprint already committed (replay)");
    }
    ATX_TRY(auto* stmt, db.prepare_cached(
        "INSERT INTO run(run_id, run_fingerprint, kind, status, engine_git_sha, master_seed,"
        " universe_id, snapshot_id, fit_start, fit_end, bt_start, bt_end, position_mode,"
        " sector_neutral, rebalance_every, cost_model, started_at)"
        " VALUES (?1,?2,?3,'running',?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)"));
    ATX_TRY_VOID(stmt->bind(1, r.run_id));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(r.fingerprint)));
    ATX_TRY_VOID(stmt->bind(3, r.kind));
    ATX_TRY_VOID(stmt->bind(4, r.engine_git_sha));
    ATX_TRY_VOID(stmt->bind(5, static_cast<atx::i64>(r.master_seed)));
    ATX_TRY_VOID(stmt->bind(6, r.universe_id));
    ATX_TRY_VOID(stmt->bind(7, r.snapshot_id));
    ATX_TRY_VOID(stmt->bind(8, r.fit_start));
    ATX_TRY_VOID(stmt->bind(9, r.fit_end));
    ATX_TRY_VOID(stmt->bind(10, r.bt_start));
    ATX_TRY_VOID(stmt->bind(11, r.bt_end));
    ATX_TRY_VOID(stmt->bind(12, r.position_mode));
    ATX_TRY_VOID(stmt->bind(13, static_cast<atx::i64>(r.sector_neutral ? 1 : 0)));
    ATX_TRY_VOID(stmt->bind(14, r.rebalance_every));
    ATX_TRY_VOID(stmt->bind(15, r.cost_model));
    ATX_TRY_VOID(stmt->bind(16, r.started_at));
    ATX_TRY(const auto step, stmt->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "RunRecorder::begin: insert incomplete");
    }
    return atx::core::Ok(RunRecorder{db, r.run_id});
  }

  [[nodiscard]] atx::core::Status set_param(std::string_view key, std::string_view value) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "INSERT OR REPLACE INTO run_param(run_id, key, value) VALUES (?1, ?2, ?3)"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, key));
    ATX_TRY_VOID(stmt->bind(3, value));
    return step_done(*stmt, "set_param");
  }

  [[nodiscard]] atx::core::Status link_alpha(atx::u64 canon_hash, std::string_view role) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "INSERT OR REPLACE INTO run_alpha(run_id, canon_hash, role) VALUES (?1, ?2, ?3)"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(canon_hash)));
    ATX_TRY_VOID(stmt->bind(3, role));
    return step_done(*stmt, "link_alpha");
  }

  [[nodiscard]] atx::core::Status record_metrics(const AlphaMetricsRow& m) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "INSERT OR REPLACE INTO alpha_metrics(run_id, canon_hash, sharpe, returns, drawdown,"
        " turnover, margin, fitness) VALUES (?1,?2,?3,?4,?5,?6,?7,?8)"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(m.canon_hash)));
    ATX_TRY_VOID(stmt->bind(3, m.sharpe));
    ATX_TRY_VOID(stmt->bind(4, m.returns));
    ATX_TRY_VOID(stmt->bind(5, m.drawdown));
    ATX_TRY_VOID(stmt->bind(6, m.turnover));
    ATX_TRY_VOID(stmt->bind(7, m.margin));
    ATX_TRY_VOID(stmt->bind(8, m.fitness));
    return step_done(*stmt, "record_metrics");
  }

  [[nodiscard]] atx::core::Status commit(atx::i64 finished_at, atx::u64 result_digest) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "UPDATE run SET status='committed', finished_at=?2, result_digest=?3 WHERE run_id=?1"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, finished_at));
    ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(result_digest)));
    ATX_TRY_VOID(step_done(*stmt, "commit"));
    if (db_.changes() != 1) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "RunRecorder::commit: run_id not found (no row updated)");
    }
    return atx::core::Ok();
  }

private:
  RunRecorder(atx::core::db::Database& db, std::string run_id)
      : db_{db}, run_id_{std::move(run_id)} {}

  [[nodiscard]] static atx::core::Status step_done(atx::core::db::Statement& stmt, const char* who) {
    ATX_TRY(const auto step, stmt.step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, who);
    }
    return atx::core::Ok();
  }

  atx::core::db::Database& db_;
  std::string run_id_;
};

}  // namespace atx::engine::store
