// Ingest of research-build receipts (build-equity/mega-<Tag>-receipt.json; writer
// scripts/research-build.ps1: ConvertTo-Json | Set-Content -Encoding ASCII, CRLF): the receipt
// row and one build_exe row per Executables entry (exe SHA-256 -> tag -> source git SHA).
// Stage 1 only: no render rule (PowerShell's layout is not re-rendered).

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog::detail {

void ingest_build_receipt(const FamilyInput &in, IngestResult &out) {
  const OJson *tag = member(in.doc, "Tag");
  const std::optional<std::string> tag_text = tag != nullptr ? fit_text(*tag) : std::nullopt;
  if (!tag_text || tag_text->empty()) {
    out.unparsed = true;
    return;
  }
  BuildReceiptRow row;
  row.tag = *tag_text;
  row.path = in.file.path;
  if (const OJson *v = member(in.doc, "Preset"); v != nullptr) {
    row.preset = fit_text(*v);
  }
  if (const OJson *v = member(in.doc, "Source"); v != nullptr) {
    row.source = fit_text(*v);
  }
  if (const OJson *v = member(in.doc, "ExitCode"); v != nullptr) {
    row.exit_code = fit_int(*v);
  }
  if (const OJson *v = member(in.doc, "WallSeconds"); v != nullptr) {
    row.wall_seconds = fit_real(*v);
  }
  row.file_sha256 = in.file.sha256;
  row.doc = compact(in.doc);
  std::vector<BuildExeRow> exes;
  const OJson *executables = member(in.doc, "Executables");
  if (executables != nullptr && executables->is_object()) {
    for (auto it = executables->begin(); it != executables->end(); ++it) {
      const OJson *sha = member(it.value(), "Sha256");
      std::optional<std::string> text = sha != nullptr ? fit_text(*sha) : std::nullopt;
      if (!text) {
        continue;
      }
      std::string lower = path_key(*text); // Get-FileHash prints upper-case hex
      if (is_sha256(lower)) {
        exes.push_back(BuildExeRow{row.tag, it.key(), std::move(lower)});
      }
    }
  }
  out.writes.emplace_back([row = std::move(row), exes = std::move(exes)](
                              core::db::Database &db) -> core::Status {
    ATX_TRY_VOID(claim_key(db, "build_receipt", "tag", row.tag, row.path));
    ATX_TRY_VOID(delete_where(db, "build_exe", "tag", row.tag));
    ATX_TRY_VOID(upsert(db, row));
    for (const BuildExeRow &e : exes) {
      ATX_TRY_VOID(insert(db, e));
    }
    return core::Ok();
  });
}

} // namespace atx::engine::research::store::catalog::detail
