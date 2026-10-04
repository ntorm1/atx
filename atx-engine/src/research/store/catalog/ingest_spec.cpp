// Ingest of repository inputs into spec_doc (specs, templates, wave manifests, libraries,
// recipes, registries, the research window, the field registry) with the pins each states,
// and of wave candidates (`atx.wave-candidate/v1`; writer scripts/wave_queue.py write:
// json.dumps(indent=2, ensure_ascii=False) + "\n", LF; doc render) with their history.
//
// What a walk follows from a holder (sql-design 3.9: "files inside the run, cycle and wave dirs
// those holders name"): a spec's output dirs (every "output" / "*_output" string of a
// top-level section, as a directory prefix: <output>, <output>-<k>, <output>-run<k>, ...), its
// cycle dir (<out_root>/cycle-<name>[-<suffix>]), its summ.ledger; a template's parent spec; a
// wave manifest's out_dir, ledger and parent spec. Specs are human inputs and are never
// re-rendered; the catalog does not re-implement research_spec.spec_digest (E2's).

#include <optional>
#include <string>
#include <string_view>
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

constexpr std::string_view kDefaultOutRoot = "build-equity"; // research_cycle.DEFAULT_OUT_ROOT

[[nodiscard]] std::optional<std::string> nested_text(const OJson &doc,
                                                     std::initializer_list<std::string_view> at) {
  const OJson *v = member_at(doc, at);
  return v != nullptr ? fit_text(*v) : std::nullopt;
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

void name_file(const IngestContext &ctx, const std::optional<std::string> &text,
               std::vector<std::string> &out) {
  if (text) {
    if (std::optional<std::string> rel = root_relative(ctx.root, *text); rel) {
      out.push_back(std::move(*rel));
    }
  }
}

void spec_follow(const FamilyInput &in, IngestResult &out) {
  const OJson &doc = in.doc;
  for (auto it = doc.begin(); it != doc.end(); ++it) {
    if (!it.value().is_object()) {
      continue;
    }
    for (auto jt = it.value().begin(); jt != it.value().end(); ++jt) {
      if ((jt.key() == "output" || ends_with(jt.key(), "_output")) && jt.value().is_string()) {
        name_file(in.ctx, jt.value().get<std::string>(), out.named_prefixes);
      }
    }
  }
  const std::optional<std::string> name = nested_text(doc, {"name"});
  if (name) {
    const std::string base = nested_text(doc, {"out_root"}).value_or(std::string{kDefaultOutRoot});
    name_file(in.ctx, base + "/cycle-" + *name, out.named_prefixes);
  }
  name_file(in.ctx, nested_text(doc, {"summ", "ledger"}), out.named_files);
}

void template_follow(const FamilyInput &in, IngestResult &out) {
  const std::optional<std::string> parent = nested_text(in.doc, {"parent"});
  const std::optional<std::string> nominal = nested_text(in.doc, {"nominal_parent"});
  for (const std::optional<std::string> &p : {parent, nominal}) {
    if (!p) {
      continue;
    }
    // research_spec.resolve looks a parent up next to the template, then from the root.
    name_file(in.ctx, join_rel(parent_of(in.file.path), *p), out.named_files);
    name_file(in.ctx, p, out.named_files);
  }
}

void wave_follow(const FamilyInput &in, IngestResult &out) {
  name_file(in.ctx, nested_text(in.doc, {"out_dir"}), out.named_dirs);
  name_file(in.ctx, nested_text(in.doc, {"ledger"}), out.named_files);
  name_file(in.ctx, nested_text(in.doc, {"parent", "spec"}), out.named_files);
}

[[nodiscard]] std::optional<std::string> parent_text(const SpecDocRow &row, const OJson &doc) {
  if (row.kind == "template") {
    return nested_text(doc, {"parent"});
  }
  if (row.kind == "wave-manifest") {
    return nested_text(doc, {"parent", "spec"});
  }
  return std::nullopt;
}

} // namespace

void ingest_spec_doc(const FamilyInput &in, IngestResult &out) {
  SpecDocRow row;
  row.path = in.file.path;
  row.kind = in.file.cls->spec_kind;
  row.schema = schema_of(in.doc);
  row.file_sha256 = in.file.sha256;
  // build*/ is git-ignored (.gitignore "build-*/"); the catalog does not run git.
  row.git_tracked = in.file.path.rfind("build", 0) != 0;
  row.parent = parent_text(row, in.doc);
  row.doc = compact(in.doc);
  std::vector<PinRow> pins;
  const PinScope scope{in.ctx, in.file.path};
  if (row.kind == "spec") {
    spec_pins(scope, in.doc, pins);
    spec_follow(in, out);
  } else if (row.kind == "template") {
    template_pins(scope, in.doc, pins);
    template_follow(in, out);
  } else if (row.kind == "wave-manifest") {
    wave_manifest_pins(scope, in.doc, pins);
    wave_follow(in, out);
  }
  out.writes.emplace_back([row = std::move(row)](core::db::Database &db) -> core::Status {
    return upsert(db, row);
  });
  emit_pins(in.file.path, std::move(pins), out);
}

void ingest_candidate(const FamilyInput &in, IngestResult &out) {
  DocFields f{in.doc};
  CandidateRow row;
  row.id = f.req_text("id");
  row.status = f.req_text("status");
  if (!f.shape_ok()) {
    out.unparsed = true;
    return;
  }
  row.path = in.file.path;
  row.wave = f.text("wave");
  row.dsl_sha256 = f.sha("dsl_sha256");
  row.file_sha256 = in.file.sha256;
  row.doc = compact(in.doc);
  std::vector<CandidateEventRow> events;
  const OJson *history = member(in.doc, "history");
  if (history != nullptr && history->is_array()) {
    for (usize i = 0; i < history->size(); ++i) {
      const OJson &h = (*history)[i];
      CandidateEventRow e;
      e.id = row.id;
      e.ord = static_cast<i64>(i);
      e.status = nested_text(h, {"status"}).value_or(std::string{});
      e.at = nested_text(h, {"at"}).value_or(std::string{});
      e.by = nested_text(h, {"by"}).value_or(std::string{});
      e.wave = nested_text(h, {"wave"});
      e.note = nested_text(h, {"note"});
      events.push_back(std::move(e));
    }
  }
  out.writes.emplace_back([row = std::move(row), events = std::move(events)](
                              core::db::Database &db) -> core::Status {
    ATX_TRY_VOID(claim_key(db, "candidate", "id", row.id, row.path));
    ATX_TRY_VOID(delete_where(db, "candidate_event", "id", row.id));
    ATX_TRY_VOID(upsert(db, row));
    for (const CandidateEventRow &e : events) {
      ATX_TRY_VOID(insert(db, e));
    }
    return core::Ok();
  });
}

} // namespace atx::engine::research::store::catalog::detail
