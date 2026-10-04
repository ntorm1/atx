// Ingest of NAV cycle bindings (`atx.cycle-nav-binding/v1`, <run dir>/cycle_binding.json;
// writer scripts/cycle_resume.py write_binding, text mode: CRLF on Windows; typed render) and
// cycle verdicts (`atx.cycle-verdict/v1`, cycle_verdict.json and its verdicts/<mode>-<k>.json
// copies; writer scripts/cycle_verdict.py, LF; doc render). A verdict's statistics stay inside
// `doc`; its typed columns are identity and ledger fields only.

#include <optional>
#include <string>
#include <utility>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog::detail {

void ingest_cycle_binding(const FamilyInput &in, IngestResult &out) {
  const std::string run_dir = parent_of(in.file.path);
  if (!is_relpath(run_dir)) {
    out.unparsed = true;
    return;
  }
  DocFields f{in.doc};
  CycleBindingRow row;
  row.run_dir = run_dir;
  row.schema = f.req_text("schema");
  row.output = f.req_relpath("output");
  row.spec_sha256 = f.sha("spec_sha256");
  row.spec_rule = f.text("spec_rule");
  row.argv_sha256 = f.sha("argv_sha256");
  if (!f.shape_ok()) {
    out.unparsed = true;
    return;
  }
  row.key_order = f.key_order();
  row.extra = f.extra();
  row.file_sha256 = in.file.sha256;
  out.writes.emplace_back([row = std::move(row)](core::db::Database &db) -> core::Status {
    return upsert(db, row);
  });
  std::vector<PinRow> pins;
  binding_pins(PinScope{in.ctx, in.file.path}, in.doc, pins);
  emit_pins(in.file.path, std::move(pins), out);
}

void ingest_cycle_verdict(const FamilyInput &in, IngestResult &out) {
  DocFields f{in.doc};
  CycleVerdictRow row;
  row.path = in.file.path;
  row.schema = f.req_text("schema");
  row.cycle = f.req_text("cycle");
  row.mode = f.req_text("mode");
  if (!f.shape_ok()) {
    out.unparsed = true;
    return;
  }
  row.spec_sha256 = f.sha("spec_sha256");
  if (const OJson *path = member_at(in.doc, {"ledger", "path"}); path != nullptr) {
    row.ledger_path = fit_relpath(*path);
    // The ledger of record a verdict names is visited (an index of it; ruling SQL-9).
    if (const std::optional<std::string> text = fit_text(*path); text) {
      if (std::optional<std::string> rel = root_relative(in.ctx.root, *text); rel) {
        out.named_files.push_back(std::move(*rel));
      }
    }
  }
  if (const OJson *head = member_at(in.doc, {"ledger", "head"}); head != nullptr) {
    row.ledger_head = fit_sha(*head);
  }
  if (const OJson *lines = member_at(in.doc, {"ledger", "lines"}); lines != nullptr) {
    row.ledger_lines = fit_int(*lines);
  }
  row.doc = compact(in.doc);
  row.file_sha256 = in.file.sha256;
  out.writes.emplace_back([row = std::move(row)](core::db::Database &db) -> core::Status {
    return upsert(db, row);
  });
  std::vector<PinRow> pins;
  verdict_pins(PinScope{in.ctx, in.file.path}, in.doc, pins);
  emit_pins(in.file.path, std::move(pins), out);
}

} // namespace atx::engine::research::store::catalog::detail
