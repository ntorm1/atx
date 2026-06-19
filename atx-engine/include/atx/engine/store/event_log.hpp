#pragma once
// atx::engine::store::event_log — the unified append-only timeline of every change to
// any alpha (alpha_event), plus a typed lifecycle projection (lifecycle_journal) for
// fast PIT state queries. transition() writes BOTH in one transaction so they never
// drift. INSERT-only — no UPDATE/DELETE. Ordering uses the AUTOINCREMENT PK as the
// deterministic tie-break, mirroring library/lifecycle.hpp.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store {

// Values 0..5 are the on-disk contract — do NOT reorder (matches library::LifecycleState).
enum class LifecycleState : atx::u8 {
  Candidate = 0, Admitted = 1, Live = 2, Decaying = 3, Dead = 4, Recycled = 5,
};

struct EventRow {
  atx::i64 ts{0};
  atx::u64 canon_hash{0};
  std::string event_type;
  std::string run_id;
  std::string actor;
  std::string payload;
};

namespace event_log {

[[nodiscard]] inline atx::core::Status append(atx::core::db::Database& db, const EventRow& e) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT INTO alpha_event(ts, canon_hash, event_type, run_id, actor, payload)"
      " VALUES (?1,?2,?3,?4,?5,?6)"));
  ATX_TRY_VOID(stmt->bind(1, e.ts));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(e.canon_hash)));
  ATX_TRY_VOID(stmt->bind(3, e.event_type));
  ATX_TRY_VOID(stmt->bind(4, e.run_id));
  ATX_TRY_VOID(stmt->bind(5, e.actor));
  ATX_TRY_VOID(stmt->bind(6, e.payload));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "event_log::append: incomplete");
  }
  return atx::core::Ok();
}

// Dual-write a lifecycle move: a typed row into lifecycle_journal AND a unified
// alpha_event(event_type='lifecycle') row, atomically.
[[nodiscard]] inline atx::core::Status
transition(atx::core::db::Database& db, atx::u64 canon_hash, LifecycleState from,
           LifecycleState to, atx::u64 as_of_period, std::string_view run_id, atx::i64 ts) {
  ATX_TRY(auto txn, atx::core::db::Transaction::begin(db));
  {
    ATX_TRY(auto* j, db.prepare_cached(
        "INSERT INTO lifecycle_journal(canon_hash, from_state, to_state, as_of_period, run_id)"
        " VALUES (?1,?2,?3,?4,?5)"));
    ATX_TRY_VOID(j->bind(1, static_cast<atx::i64>(canon_hash)));
    ATX_TRY_VOID(j->bind(2, static_cast<atx::i64>(static_cast<atx::u8>(from))));
    ATX_TRY_VOID(j->bind(3, static_cast<atx::i64>(static_cast<atx::u8>(to))));
    ATX_TRY_VOID(j->bind(4, static_cast<atx::i64>(as_of_period)));
    ATX_TRY_VOID(j->bind(5, run_id));
    ATX_TRY(const auto step, j->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "event_log::transition: journal incomplete");
    }
  }
  ATX_TRY_VOID(append(db, EventRow{ts, canon_hash, "lifecycle", std::string(run_id), "system",
                                   "{\"to\":" + std::to_string(static_cast<int>(to)) + "}"}));
  ATX_TRY_VOID(txn.commit());
  return atx::core::Ok();
}

// PIT state: to_state of the latest journal row with as_of_period <= t (ties broken by
// seq DESC). No row at-or-before t => Candidate (the implicit pre-journal state).
[[nodiscard]] inline atx::core::Result<LifecycleState>
state_as_of(atx::core::db::Database& db, atx::u64 canon_hash, atx::u64 t) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT to_state FROM lifecycle_journal WHERE canon_hash = ?1 AND as_of_period <= ?2"
      " ORDER BY as_of_period DESC, seq DESC LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(t)));
  ATX_TRY(const auto step, stmt->step());
  if (step == atx::core::db::Statement::Step::Done) {
    return atx::core::Ok(LifecycleState::Candidate);
  }
  return atx::core::Ok(static_cast<LifecycleState>(static_cast<atx::u8>(stmt->column_int(0))));
}

// Full ordered timeline for one alpha (created -> evaluated -> lifecycle -> promoted ...).
[[nodiscard]] inline atx::core::Result<std::vector<EventRow>>
history(atx::core::db::Database& db, atx::u64 canon_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT ts, event_type, run_id, actor, payload FROM alpha_event"
      " WHERE canon_hash = ?1 ORDER BY ts, event_id"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  std::vector<EventRow> out;
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == atx::core::db::Statement::Step::Done) break;
    EventRow e;
    e.ts = stmt->column_int(0);
    e.canon_hash = canon_hash;
    e.event_type = std::string(stmt->column_text(1));
    e.run_id = std::string(stmt->column_text(2));
    e.actor = std::string(stmt->column_text(3));
    e.payload = std::string(stmt->column_text(4));
    out.push_back(std::move(e));
  }
  return atx::core::Ok(std::move(out));
}

}  // namespace event_log
}  // namespace atx::engine::store
