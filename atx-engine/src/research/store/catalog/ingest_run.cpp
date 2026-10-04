// Ingest of bounded-run receipts (receipt.json: typed, with run_command / run_binding child rows)
// and launch receipts (start.json: doc). Writer: scripts/run_bounded_research.py (the receipt
// dict built at launch, completed in its `finally`; both written in text mode: CRLF on Windows).

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog::detail {
namespace {

// `command`: an array of strings, one child row each (else the key stays in extra).
[[nodiscard]] std::optional<std::vector<RunCommandRow>> command_rows(const OJson &doc,
                                                                     const std::string &run_dir) {
  const OJson *command = member(doc, "command");
  if (command == nullptr || !command->is_array()) {
    return std::nullopt;
  }
  std::vector<RunCommandRow> rows;
  for (usize i = 0; i < command->size(); ++i) {
    const std::optional<std::string> arg = fit_text((*command)[i]);
    if (!arg) {
      return std::nullopt;
    }
    rows.push_back(RunCommandRow{run_dir, static_cast<i64>(i), *arg});
  }
  return rows;
}

// `bindings`: an array of {"path": str, "sha256": sha} objects, exactly those keys in that
// order (what the renderer rebuilds), one child row each (else the key stays in extra).
[[nodiscard]] std::optional<std::vector<RunBindingRow>> binding_rows(const OJson &doc,
                                                                     const std::string &run_dir) {
  const OJson *bindings = member(doc, "bindings");
  if (bindings == nullptr || !bindings->is_array()) {
    return std::nullopt;
  }
  std::vector<RunBindingRow> rows;
  for (usize i = 0; i < bindings->size(); ++i) {
    const OJson &b = (*bindings)[i];
    if (!b.is_object() || b.size() != 2 || b.begin().key() != "path") {
      return std::nullopt;
    }
    const OJson *path = member(b, "path");
    const OJson *sha = member(b, "sha256");
    const std::optional<std::string> p = path != nullptr ? fit_text(*path) : std::nullopt;
    const std::optional<std::string> s = sha != nullptr ? fit_sha(*sha) : std::nullopt;
    if (!p || !s) {
      return std::nullopt;
    }
    rows.push_back(RunBindingRow{run_dir, static_cast<i64>(i), *p, *s});
  }
  return rows;
}

} // namespace

void ingest_run(const FamilyInput &in, IngestResult &out) {
  const std::string run_dir = parent_of(in.file.path);
  if (!is_relpath(run_dir)) {
    out.unparsed = true;
    return;
  }
  DocFields f{in.doc};
  RunRow row;
  row.run_dir = run_dir;
  row.receipt_schema = f.req_text("schema");
  row.source_sha = f.text("source_sha");
  row.started_utc = f.text("started_utc");
  std::optional<std::vector<RunCommandRow>> commands = command_rows(in.doc, run_dir);
  if (commands) {
    f.claim("command");
  }
  row.executable_sha256 = f.sha("executable_sha256");
  row.argv_sha256 = f.sha("argv_sha256");
  row.attempt = f.integer("attempt");
  row.build_type = f.text("build_type");
  std::optional<std::vector<RunBindingRow>> bindings = binding_rows(in.doc, run_dir);
  if (bindings) {
    f.claim("bindings");
  }
  row.limits = f.json("limits");
  row.sampled_peak_tree_rss_bytes = f.integer("sampled_peak_tree_rss_bytes");
  row.minimum_system_free_bytes = f.integer("minimum_system_free_bytes");
  row.outcome = f.req_text("outcome");
  row.exit_code = f.integer("exit_code");
  row.git_state = f.text("git");
  row.dirty_outside = f.json("dirty_outside_pathspec");
  row.role_id = f.text("role_id");
  row.admission = f.json("admission");
  row.error = f.text("error");
  row.wall_seconds = f.real("wall_seconds");
  row.owned_processes = f.json("owned_processes");
  row.ownership_scope = f.text("ownership_scope");
  row.logs = f.json("logs");
  row.store = f.json("store");
  if (!f.shape_ok()) {
    out.unparsed = true;
    return;
  }
  row.key_order = f.key_order();
  row.extra = f.extra();
  row.file_sha256 = in.file.sha256;
  out.writes.emplace_back([row = std::move(row),
                           commands = commands.value_or(std::vector<RunCommandRow>{}),
                           bindings = bindings.value_or(std::vector<RunBindingRow>{})](
                              core::db::Database &db) -> core::Status {
    ATX_TRY_VOID(delete_where(db, "run_command", "run_dir", row.run_dir));
    ATX_TRY_VOID(delete_where(db, "run_binding", "run_dir", row.run_dir));
    ATX_TRY_VOID(upsert(db, row));
    for (const RunCommandRow &c : commands) {
      ATX_TRY_VOID(insert(db, c));
    }
    for (const RunBindingRow &b : bindings) {
      ATX_TRY_VOID(insert(db, b));
    }
    return core::Ok();
  });
  std::vector<PinRow> pins;
  receipt_pins(PinScope{in.ctx, in.file.path}, in.doc, pins);
  emit_pins(in.file.path, std::move(pins), out);
}

void ingest_run_start(const FamilyInput &in, IngestResult &out) {
  const std::string run_dir = parent_of(in.file.path);
  if (!is_relpath(run_dir)) {
    out.unparsed = true;
    return;
  }
  out.writes.emplace_back([row = RunStartRow{run_dir, in.file.sha256, compact(in.doc)}](
                              core::db::Database &db) -> core::Status { return upsert(db, row); });
}

} // namespace atx::engine::research::store::catalog::detail
