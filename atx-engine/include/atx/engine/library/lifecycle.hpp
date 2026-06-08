#pragma once

// atx::engine::library — LifecycleJournal: PIT append-only lifecycle state machine (S4-4).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The point-in-time (PIT), append-only journal that drives each alpha's library
//  lifecycle as the S7 spine advances. An alpha walks a strict LINEAR spine:
//
//      Candidate -> Admitted -> Live -> Decaying -> {Live (recover) | Dead}
//                                                    Dead -> Recycled
//
//  Every transition is an INSERT into a sqlite `journal` table — NEVER an UPDATE
//  or DELETE. The current state is the latest row for an AlphaId; a point-in-time
//  query (state_as_of) reads the latest row whose as_of_period is <= the query
//  period. Because rows are only ever appended, a LATER transition can never
//  retroactively relabel an EARLIER point-in-time state query — that is the
//  dominant S4-4 risk (a retroactive relabel) made structurally impossible.
//
// ===========================================================================
//  Legal-transition table (the spine)
// ===========================================================================
//  legal(from,to) is a constexpr table with EXACTLY six legal adjacent edges:
//    (Candidate,Admitted) (Admitted,Live) (Live,Decaying)
//    (Decaying,Live)      (Decaying,Dead) (Dead,Recycled)
//  Everything else is illegal: forward-skips (e.g. Admitted->Decaying), all
//  backward edges (e.g. Live->Candidate), and every self-transition. An illegal
//  transition returns Err(InvalidArgument) and appends NOTHING.
//
// ===========================================================================
//  Determinism / threading
// ===========================================================================
//  state_as_of / current_state ORDER BY (as_of_period DESC, seq DESC): the
//  autoincrement seq breaks ties deterministically when two transitions share an
//  as_of_period, so the "latest" row is well-defined under replay (L7). Like the
//  rest of the library metadata, the sqlite Database is one-per-thread
//  (SQLITE_THREADSAFE=2 serial-owner rule) and never shared across threads.
//
// ===========================================================================
//  i64 storage of as_of_period
// ===========================================================================
//  as_of_period is a u64 PERIOD INDEX (a bounded, non-negative bar index, not a
//  wall-clock time). It is stored in a sqlite INTEGER (signed i64) column via
//  static_cast<i64>: periods are small non-negative indices, so the cast is
//  value-preserving for every value the spine ever produces. (Full-range storage
//  would need std::bit_cast, but periods are bounded so static_cast is correct
//  and the comparison `as_of_period <= ?` stays monotonic.)

#include <string>  // db path
#include <utility> // std::move

#include "atx/core/db/sqlite.hpp" // db::Database, Statement, OpenMode
#include "atx/core/error.hpp"     // Result, Status, Ok, Err, ErrorCode, ATX_TRY*
#include "atx/core/macro.hpp"     // ATX_ASSERT
#include "atx/core/types.hpp"     // i64, u8, u64

#include "atx/engine/combine/store.hpp" // combine::AlphaId
#include "atx/engine/library/fwd.hpp"   // LifecycleState (enum decl), LifecycleJournal (class decl)

namespace atx::engine::library {

// ===========================================================================
//  LifecycleState — the alpha's position on the linear lifecycle spine.
//
//  Underlying type MUST match fwd.hpp's `enum class LifecycleState : atx::u8;`.
//  Enumerator values are 0..5 and are stored verbatim in the journal's INTEGER
//  to_state column, then cast back on read — so the order here is part of the
//  on-disk contract and MUST NOT be reordered.
// ===========================================================================
enum class LifecycleState : atx::u8 {
  Candidate = 0, // unborn / freshly proposed (the implicit pre-journal state)
  Admitted = 1,  // passed the gate, admitted to the library
  Live = 2,      // actively traded
  Decaying = 3,  // performance decaying — under watch
  Dead = 4,      // retired
  Recycled = 5,  // GC'd / slot reclaimed (the S7 baton terminal)
};

// True iff `to` is a legal successor of `from` on the strict linear spine.
// The ONLY legal pairs are the six adjacent edges; forward-skips, backward
// edges, and self-transitions are all illegal. constexpr so it folds to a
// jump-free comparison chain at the call site.
[[nodiscard]] constexpr bool legal(LifecycleState from, LifecycleState to) noexcept {
  switch (from) {
  case LifecycleState::Candidate:
    return to == LifecycleState::Admitted;
  case LifecycleState::Admitted:
    return to == LifecycleState::Live;
  case LifecycleState::Live:
    return to == LifecycleState::Decaying;
  case LifecycleState::Decaying:
    return to == LifecycleState::Live || to == LifecycleState::Dead;
  case LifecycleState::Dead:
    return to == LifecycleState::Recycled;
  case LifecycleState::Recycled:
    return false; // terminal — no successor
  }
  return false; // unreachable for valid enumerators
}

// ===========================================================================
//  LifecycleJournal — append-only PIT journal of lifecycle transitions.
//
//  Backed by a single sqlite table on the owning thread's Database connection.
//  transition() is INSERT-ONLY; there is no UPDATE/DELETE anywhere in this unit
//  (the PIT guarantee). Reads (state_as_of / current_state) are pure SELECTs.
// ===========================================================================
class LifecycleJournal {
public:
  /// Open (or create) the lifecycle journal rooted at `dir`: open the sqlite db
  /// file (`<dir>/lifecycle.db`) and create the `journal` table if absent. A
  /// failed open (unwritable dir, corrupt db) is an environment fault — ABORTED
  /// via ATX_ASSERT, mirroring DedupIndex: a half-open journal has no valid use,
  /// and S4 always opens a writable tmpdir. The Database is opened in the init
  /// list (no default ctor); the schema is created in the body.
  explicit LifecycleJournal(const std::string &dir) : db_{open_or_abort(db_path_for(dir))} {
    const auto st = init_schema();
    ATX_ASSERT(st.has_value());
    (void)st;
  }

  /// Record a transition of `id` to `to` as of period `as_of_period`. APPEND
  /// ONLY: validates legality against current_state(id) (the latest known state),
  /// then INSERTs a new journal row — no UPDATE/DELETE ever. Returns
  /// Err(InvalidArgument) if the (current -> to) edge is illegal (forward-skip,
  /// backward, or self), leaving the journal UNCHANGED. Err on a sqlite fault.
  [[nodiscard]] atx::core::Status transition(combine::AlphaId id, LifecycleState to,
                                             atx::u64 as_of_period) {
    ATX_TRY(const LifecycleState cur, current_state(id));
    if (!legal(cur, to)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "LifecycleJournal::transition: illegal lifecycle edge");
    }
    ATX_TRY(auto *stmt,
            db_.prepare_cached("INSERT INTO journal (alpha_id, from_state, to_state, as_of_period) "
                               "VALUES (?1, ?2, ?3, ?4)"));
    ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(id.value)));
    ATX_TRY_VOID(stmt->bind(2, state_to_i64(cur)));
    ATX_TRY_VOID(stmt->bind(3, state_to_i64(to)));
    ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(as_of_period)));
    ATX_TRY(const auto step, stmt->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "LifecycleJournal::transition: insert did not complete");
    }
    return atx::core::Ok();
  }

  /// Point-in-time state of `id` as of period `t`: the to_state of the latest
  /// journal row whose as_of_period <= t (ties on as_of_period broken by the
  /// autoincrement seq DESC — L7 determinism). If no row is at-or-before t, the
  /// alpha is UNBORN as of t and the state is Candidate (the implicit pre-journal
  /// state). Err on a sqlite fault.
  [[nodiscard]] atx::core::Result<LifecycleState> state_as_of(combine::AlphaId id,
                                                              atx::u64 t) const {
    ATX_TRY(auto *stmt,
            db_.prepare_cached("SELECT to_state FROM journal "
                               "WHERE alpha_id = ?1 AND as_of_period <= ?2 "
                               "ORDER BY as_of_period DESC, seq DESC LIMIT 1"));
    ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(id.value)));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(t)));
    return read_state_or_candidate(*stmt);
  }

  /// Latest known state of `id` (NO period filter): the to_state of the most
  /// recent journal row by (as_of_period DESC, seq DESC), or Candidate if the
  /// alpha has no rows yet. This is the state transition() validates against.
  /// Err on a sqlite fault.
  [[nodiscard]] atx::core::Result<LifecycleState> current_state(combine::AlphaId id) const {
    ATX_TRY(auto *stmt, db_.prepare_cached("SELECT to_state FROM journal "
                                           "WHERE alpha_id = ?1 "
                                           "ORDER BY as_of_period DESC, seq DESC LIMIT 1"));
    ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(id.value)));
    return read_state_or_candidate(*stmt);
  }

private:
  [[nodiscard]] static std::string db_path_for(const std::string &dir) {
    return dir + "/lifecycle.db";
  }

  /// Open the lifecycle Database or ABORT (environment fault — see the ctor doc).
  /// Used in the member init list, where a Result cannot be propagated.
  [[nodiscard]] static atx::core::db::Database open_or_abort(const std::string &path) {
    auto db = atx::core::db::Database::open(path, atx::core::db::OpenMode::ReadWriteCreate);
    ATX_ASSERT(db.has_value());
    return std::move(*db);
  }

  /// Bind a LifecycleState as its underlying u8 value widened to i64 (0..5).
  [[nodiscard]] static atx::i64 state_to_i64(LifecycleState s) noexcept {
    return static_cast<atx::i64>(static_cast<atx::u8>(s));
  }

  /// Step `stmt` once: on a Row, read column 0 as a LifecycleState; on Done (no
  /// matching row), return Candidate (the unborn / pre-journal state).
  [[nodiscard]] static atx::core::Result<LifecycleState>
  read_state_or_candidate(atx::core::db::Statement &stmt) {
    ATX_TRY(const auto step, stmt.step());
    if (step == atx::core::db::Statement::Step::Done) {
      return atx::core::Ok(LifecycleState::Candidate);
    }
    const auto v = static_cast<atx::u8>(stmt.column_int(0));
    return atx::core::Ok(static_cast<LifecycleState>(v));
  }

  /// Create the append-only journal schema if absent. seq is an AUTOINCREMENT
  /// PRIMARY KEY so it is a strictly-increasing tie-breaker for same-period
  /// transitions (the ORDER BY ..., seq DESC determinism). Run from the ctor body.
  [[nodiscard]] atx::core::Status init_schema() {
    return db_.exec("CREATE TABLE IF NOT EXISTS journal ("
                    " seq          INTEGER PRIMARY KEY AUTOINCREMENT,"
                    " alpha_id     INTEGER NOT NULL,"
                    " from_state   INTEGER NOT NULL,"
                    " to_state     INTEGER NOT NULL,"
                    " as_of_period INTEGER NOT NULL)");
  }

  // `mutable`: the const read methods (state_as_of / current_state) go through
  // prepare_cached, which mutates the Database's internal statement cache. That
  // is a logical-constness implementation detail — the journal's observable state
  // (the rows) is unchanged by a read — so the cache mutation is hidden behind
  // `mutable`, keeping the read API const-correct.
  mutable atx::core::db::Database db_; // sqlite journal table (one per thread)
};

} // namespace atx::engine::library
