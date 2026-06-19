#pragma once
// atx::engine::store::segment_index — the DB index over the binary mmap segments that
// hold heavy PnL/position arrays. The DB never stores the arrays; it maps a canon_hash
// to (segment_id, dir_index) so a reader can mmap the file and seek the alpha's slot.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store {

struct SegmentRow {
  std::string segment_id;
  std::string path;
  atx::u64 content_hash{0};
  atx::u64 base_alpha_id{0};
  atx::i64 n_alphas{0};
  atx::u64 crc{0};
  atx::i64 format_version{0};
  std::string created_by_run_id;
};

struct SegmentLoc {
  std::string segment_id;
  atx::i64 dir_index{0};
};

namespace segment_index {

[[nodiscard]] inline atx::core::Status
register_segment(atx::core::db::Database& db, const SegmentRow& s) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO segment(segment_id, path, content_hash, base_alpha_id, n_alphas,"
      " crc, format_version, created_by_run_id) VALUES (?1,?2,?3,?4,?5,?6,?7,?8)"));
  ATX_TRY_VOID(stmt->bind(1, s.segment_id));
  ATX_TRY_VOID(stmt->bind(2, s.path));
  ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(s.content_hash)));
  ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(s.base_alpha_id)));
  ATX_TRY_VOID(stmt->bind(5, s.n_alphas));
  ATX_TRY_VOID(stmt->bind(6, static_cast<atx::i64>(s.crc)));
  ATX_TRY_VOID(stmt->bind(7, s.format_version));
  ATX_TRY_VOID(stmt->bind(8, s.created_by_run_id));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "segment_index::register_segment: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Status
map_alpha(atx::core::db::Database& db, atx::u64 canon_hash, std::string_view segment_id,
          atx::i64 dir_index) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO segment_alpha(canon_hash, segment_id, dir_index) VALUES (?1,?2,?3)"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY_VOID(stmt->bind(2, segment_id));
  ATX_TRY_VOID(stmt->bind(3, dir_index));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "segment_index::map_alpha: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<SegmentLoc>
locate(atx::core::db::Database& db, atx::u64 canon_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT segment_id, dir_index FROM segment_alpha WHERE canon_hash = ?1 LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY(const auto step, stmt->step());
  if (step == atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "segment_index::locate: alpha not mapped");
  }
  SegmentLoc loc;
  loc.segment_id = std::string(stmt->column_text(0));
  loc.dir_index = stmt->column_int(1);
  return atx::core::Ok(std::move(loc));
}

}  // namespace segment_index
}  // namespace atx::engine::store
