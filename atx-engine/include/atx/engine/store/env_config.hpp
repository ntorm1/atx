#pragma once
// atx::engine::store::env_config — key/value config describing THIS environment
// (cost knobs, capacity curve, execution model, rebalance schedule). One row per key.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"

namespace atx::engine::store::env_config {

[[nodiscard]] inline atx::core::Status
set(atx::core::db::Database& db, std::string_view key, std::string_view value) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO env_config(key, value) VALUES (?1, ?2)"));
  ATX_TRY_VOID(stmt->bind(1, key));
  ATX_TRY_VOID(stmt->bind(2, value));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "env_config::set: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<std::string>
get(atx::core::db::Database& db, std::string_view key) {
  ATX_TRY(auto* stmt, db.prepare_cached("SELECT value FROM env_config WHERE key = ?1 LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, key));
  ATX_TRY(const auto step, stmt->step());
  if (step == atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "env_config::get: key unset");
  }
  return atx::core::Ok(std::string(stmt->column_text(0)));
}

}  // namespace atx::engine::store::env_config
