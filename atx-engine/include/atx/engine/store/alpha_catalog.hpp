#pragma once
// atx::engine::store::alpha_catalog — alpha identity (canon_hash PK = dedup) + lineage.

#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store::alpha_catalog {

// Insert the alpha identity if its canon_hash is new; a duplicate canon_hash is a
// no-op (INSERT OR IGNORE) — canon_hash PK IS the dedup index.
[[nodiscard]] inline atx::core::Status
upsert(atx::core::db::Database& db, atx::u64 canon_hash, atx::u64 alpha_id,
       std::string_view expr, atx::i64 created_at, std::string_view first_run_id) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR IGNORE INTO alpha(canon_hash, alpha_id, expr_source, created_at, first_run_id)"
      " VALUES (?1, ?2, ?3, ?4, ?5)"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(alpha_id)));
  ATX_TRY_VOID(stmt->bind(3, expr));
  ATX_TRY_VOID(stmt->bind(4, created_at));
  ATX_TRY_VOID(stmt->bind(5, first_run_id));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "alpha_catalog::upsert: insert incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<bool>
exists(atx::core::db::Database& db, atx::u64 canon_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached("SELECT 1 FROM alpha WHERE canon_hash = ?1 LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY(const auto step, stmt->step());
  return atx::core::Ok(step == atx::core::db::Statement::Step::Row);
}

[[nodiscard]] inline atx::core::Status
add_lineage(atx::core::db::Database& db, atx::u64 child, atx::u64 parent,
            atx::u64 mutation_op, atx::u64 seed) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR IGNORE INTO alpha_lineage(child_hash, parent_hash, mutation_op, seed)"
      " VALUES (?1, ?2, ?3, ?4)"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(child)));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(parent)));
  ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(mutation_op)));
  ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(seed)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "alpha_catalog::add_lineage: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<std::vector<atx::u64>>
parents(atx::core::db::Database& db, atx::u64 child) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT parent_hash FROM alpha_lineage WHERE child_hash = ?1 ORDER BY parent_hash"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(child)));
  std::vector<atx::u64> out;
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == atx::core::db::Statement::Step::Done) break;
    out.push_back(static_cast<atx::u64>(stmt->column_int(0)));
  }
  return atx::core::Ok(std::move(out));
}

}  // namespace atx::engine::store::alpha_catalog
