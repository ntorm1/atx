#pragma once
// atx::engine::store::universe_registry — point-in-time universe definitions, their
// members, and external data snapshots. universe_id / snapshot_id are content hashes
// (TEXT) supplied by the caller; both inserts are INSERT OR REPLACE (idempotent define).

#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store::universe_registry {

[[nodiscard]] inline atx::core::Status
define(atx::core::db::Database& db, std::string_view universe_id, std::string_view name,
       atx::i64 as_of, std::string_view rule, atx::u64 content_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO universe(universe_id, name, as_of, rule, content_hash)"
      " VALUES (?1, ?2, ?3, ?4, ?5)"));
  ATX_TRY_VOID(stmt->bind(1, universe_id));
  ATX_TRY_VOID(stmt->bind(2, name));
  ATX_TRY_VOID(stmt->bind(3, as_of));
  ATX_TRY_VOID(stmt->bind(4, rule));
  ATX_TRY_VOID(stmt->bind(5, static_cast<atx::i64>(content_hash)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "universe_registry::define: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Status
add_member(atx::core::db::Database& db, std::string_view universe_id, atx::u64 instrument_id) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR IGNORE INTO universe_member(universe_id, instrument_id) VALUES (?1, ?2)"));
  ATX_TRY_VOID(stmt->bind(1, universe_id));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(instrument_id)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "universe_registry::add_member: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<std::vector<atx::u64>>
members(atx::core::db::Database& db, std::string_view universe_id) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT instrument_id FROM universe_member WHERE universe_id = ?1 ORDER BY instrument_id"));
  ATX_TRY_VOID(stmt->bind(1, universe_id));
  std::vector<atx::u64> out;
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == atx::core::db::Statement::Step::Done) break;
    out.push_back(static_cast<atx::u64>(stmt->column_int(0)));
  }
  return atx::core::Ok(std::move(out));
}

[[nodiscard]] inline atx::core::Status
record_snapshot(atx::core::db::Database& db, std::string_view snapshot_id, std::string_view source,
                atx::i64 as_of, atx::u64 content_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO data_snapshot(snapshot_id, source, as_of, content_hash)"
      " VALUES (?1, ?2, ?3, ?4)"));
  ATX_TRY_VOID(stmt->bind(1, snapshot_id));
  ATX_TRY_VOID(stmt->bind(2, source));
  ATX_TRY_VOID(stmt->bind(3, as_of));
  ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(content_hash)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "universe_registry::record_snapshot: incomplete");
  }
  return atx::core::Ok();
}

}  // namespace atx::engine::store::universe_registry
