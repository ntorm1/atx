#pragma once
// atx::engine::store::promotion — move an alpha's identity (single canon_hash) from one
// environment DB file to the next (Dev→UAT→PROD) by ATTACH-ing the destination file,
// copying alpha + alpha_lineage rows, and recording a promotion_ledger entry plus an
// alpha_event in the destination. canon_hash keeps the identity stable across files.
//
// ATTACH semantics: ATTACH must run outside any transaction. We then use BEGIN IMMEDIATE
// so SQLite acquires a write lock on both main and dest before the first write, avoiding
// the "database dest is locked" SQLITE_ERROR that arises when a deferred transaction
// lazily tries to promote to a writer on an attached WAL-mode database.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store::promotion {

struct PromotionRequest {
  atx::u64 canon_hash{0};
  std::string from_env;
  std::string to_env;
  std::string justifying_run_id;
  std::string approved_by;
  atx::i64 ts{0};
  std::string dest_path; // file path of the destination env DB
};

// ATTACH the destination, copy identity + lineage for canon_hash into it, write the
// ledger + event, then DETACH. Uses BEGIN IMMEDIATE for the cross-database transaction
// so the write lock on dest is acquired up-front (not lazily), preventing lock errors
// on WAL-mode attached databases.
[[nodiscard]] inline atx::core::Status
promote(atx::core::db::Database& src, const PromotionRequest& req) {
  // ATTACH cannot run inside a transaction; do it first, DETACH at the end.
  {
    ATX_TRY(auto* att, src.prepare_cached("ATTACH DATABASE ?1 AS dest"));
    ATX_TRY_VOID(att->bind(1, req.dest_path));
    ATX_TRY(const auto step, att->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "promotion::promote: ATTACH failed");
    }
  }
  const auto h = static_cast<atx::i64>(req.canon_hash);
  // BEGIN IMMEDIATE acquires a write lock on all attached databases up-front,
  // preventing "database dest is locked" when writing to dest.* in the same txn.
  auto run_copy = [&]() -> atx::core::Status {
    ATX_TRY_VOID(src.exec("BEGIN IMMEDIATE"));
    auto committed = false;
    // RAII guard: ROLLBACK on early return (ATX_TRY* failure) unless we committed.
    struct Guard {
      atx::core::db::Database* db; bool* ok;
      ~Guard() noexcept { if (!*ok) (void)db->exec("ROLLBACK"); }
    } guard{&src, &committed};

    {
      ATX_TRY(auto* c1, src.prepare_cached(
          "INSERT OR IGNORE INTO dest.alpha SELECT * FROM main.alpha WHERE canon_hash = ?1"));
      ATX_TRY_VOID(c1->bind(1, h));
      ATX_TRY(const auto s1, c1->step());
      if (s1 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: copy alpha failed");
    }
    {
      ATX_TRY(auto* c2, src.prepare_cached(
          "INSERT OR IGNORE INTO dest.alpha_lineage SELECT * FROM main.alpha_lineage"
          " WHERE child_hash = ?1"));
      ATX_TRY_VOID(c2->bind(1, h));
      ATX_TRY(const auto s2, c2->step());
      if (s2 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: copy lineage failed");
    }
    {
      ATX_TRY(auto* c3, src.prepare_cached(
          "INSERT INTO dest.promotion_ledger(canon_hash, from_env, to_env, justifying_run_id,"
          " approved_by, ts) VALUES (?1,?2,?3,?4,?5,?6)"));
      ATX_TRY_VOID(c3->bind(1, h));
      ATX_TRY_VOID(c3->bind(2, req.from_env));
      ATX_TRY_VOID(c3->bind(3, req.to_env));
      ATX_TRY_VOID(c3->bind(4, req.justifying_run_id));
      ATX_TRY_VOID(c3->bind(5, req.approved_by));
      ATX_TRY_VOID(c3->bind(6, req.ts));
      ATX_TRY(const auto s3, c3->step());
      if (s3 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: ledger insert failed");
    }
    {
      ATX_TRY(auto* c4, src.prepare_cached(
          "INSERT INTO dest.alpha_event(ts, canon_hash, event_type, run_id, actor, payload)"
          " VALUES (?1,?2,'promoted',?3,'user',?4)"));
      ATX_TRY_VOID(c4->bind(1, req.ts));
      ATX_TRY_VOID(c4->bind(2, h));
      ATX_TRY_VOID(c4->bind(3, req.justifying_run_id));
      ATX_TRY_VOID(c4->bind(4, std::string("{\"to_env\":\"") + req.to_env + "\"}"));
      ATX_TRY(const auto s4, c4->step());
      if (s4 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: event insert failed");
    }
    // S11-4: copy the env's cluster_panel artifact registry into dest. These rows
    // are universe-scoped build artifacts (not keyed by canon_hash), so the
    // promotion carries the whole registry forward; INSERT OR IGNORE keeps it
    // idempotent against a dest that already holds some of the rows.
    {
      ATX_TRY(auto* c5, src.prepare_cached(
          "INSERT OR IGNORE INTO dest.cluster_panel SELECT * FROM main.cluster_panel"));
      ATX_TRY(const auto s5, c5->step());
      if (s5 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: copy cluster_panel failed");
    }
    // Dual-write a cluster_panel_built event onto the destination timeline so the
    // promoted panels are visible alongside the other artifact-type events.
    {
      ATX_TRY(auto* c6, src.prepare_cached(
          "INSERT INTO dest.alpha_event(ts, canon_hash, event_type, run_id, actor, payload)"
          " VALUES (?1,?2,'cluster_panel_built',?3,'user',?4)"));
      ATX_TRY_VOID(c6->bind(1, req.ts));
      ATX_TRY_VOID(c6->bind(2, h));
      ATX_TRY_VOID(c6->bind(3, req.justifying_run_id));
      ATX_TRY_VOID(c6->bind(4, std::string("{\"to_env\":\"") + req.to_env + "\"}"));
      ATX_TRY(const auto s6, c6->step());
      if (s6 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: cluster_panel event insert failed");
    }
    ATX_TRY_VOID(src.exec("COMMIT"));
    committed = true;
    return atx::core::Ok();
  };
  const auto copy_status = run_copy();
  // Always DETACH, even on failure (best-effort; ignore detach error if copy already failed).
  (void)src.exec("DETACH DATABASE dest");
  return copy_status;
}

}  // namespace atx::engine::store::promotion
