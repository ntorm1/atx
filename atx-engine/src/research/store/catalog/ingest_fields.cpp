// Ingest of fields manifests (`atx.research-role-fields/v1`): the manifest row, one field_entry
// per `fields` entry, producer rows (K-P9-3 engine blocks, ruling P6 legacy Python identities)
// and the payload / registry / source pins. Payloads are never opened here: a payload over
// 16 MiB becomes a `declared` artifact through these pins (catalog.cpp); field-source pins are
// recorded and never followed.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/digest.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/rows_core.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog::detail {
namespace {

[[nodiscard]] std::optional<std::string> text_at(const OJson &obj,
                                                 std::initializer_list<std::string_view> at) {
  const OJson *v = member_at(obj, at);
  return v != nullptr ? fit_text(*v) : std::nullopt;
}

[[nodiscard]] std::optional<std::string> sha_at(const OJson &obj,
                                                std::initializer_list<std::string_view> at) {
  const OJson *v = member_at(obj, at);
  return v != nullptr ? fit_sha(*v) : std::nullopt;
}

void add_text(DigestStream &s, std::string_view column, const std::optional<std::string> &v) {
  if (v) {
    s.add_text(column, *v);
  } else {
    s.add_null(column);
  }
}

// producer_key = the record digest of the producer row's other columns (sql-design 3.7):
// "row producer@1" then kind, exe_sha256, git_sha, build_type, module, code_sha256,
// receipt_sha256 in declared order.
void keyed(ProducerRow &p) {
  DigestStream s;
  s.begin_row("producer", 1);
  s.add_text("kind", p.kind);
  add_text(s, "exe_sha256", p.exe_sha256);
  add_text(s, "git_sha", p.git_sha);
  add_text(s, "build_type", p.build_type);
  add_text(s, "module", p.module_name);
  add_text(s, "code_sha256", p.code_sha256);
  add_text(s, "receipt_sha256", p.receipt_sha256);
  p.producer_key = s.finish();
}

// An entry's producer block: {"kind": "engine", exe_sha256, git_sha, build_type,
// receipt_sha256} (K-P9-3) or the legacy Python shape {module, code_sha256, ...} (P6).
[[nodiscard]] std::optional<ProducerRow> producer_of(const OJson *block) {
  if (block == nullptr || !block->is_object()) {
    return std::nullopt;
  }
  ProducerRow p;
  const std::optional<std::string> kind = text_at(*block, {"kind"});
  if (kind && *kind == "engine") {
    p.kind = "engine";
    p.exe_sha256 = sha_at(*block, {"exe_sha256"});
    p.git_sha = text_at(*block, {"git_sha"});
    p.build_type = text_at(*block, {"build_type"});
    p.receipt_sha256 = sha_at(*block, {"receipt_sha256"});
  } else if (!kind) {
    p.kind = "python";
    p.module_name = text_at(*block, {"module"});
    p.code_sha256 = sha_at(*block, {"code_sha256"});
    if (!p.module_name && !p.code_sha256) {
      return std::nullopt;
    }
  } else {
    p.kind = "unknown";
  }
  keyed(p);
  return p;
}

// The manifest's own producer: its engine block, else the builder's top-level code identity.
[[nodiscard]] std::optional<ProducerRow> manifest_producer(const OJson &doc) {
  if (const OJson *engine = member(doc, "engine"); engine != nullptr && engine->is_object()) {
    ProducerRow p;
    p.kind = "engine";
    p.exe_sha256 = sha_at(*engine, {"exe_sha256"});
    p.git_sha = text_at(*engine, {"git_sha"});
    p.build_type = text_at(*engine, {"build_type"});
    p.receipt_sha256 = sha_at(doc, {"receipt_sha256"});
    keyed(p);
    return p;
  }
  if (sha_at(doc, {"code_sha256"})) {
    ProducerRow p;
    p.kind = "python";
    p.module_name = text_at(doc, {"module"});
    p.code_sha256 = sha_at(doc, {"code_sha256"});
    keyed(p);
    return p;
  }
  return std::nullopt;
}

} // namespace

void ingest_field_manifest(const FamilyInput &in, IngestResult &out) {
  const std::optional<std::string> schema = text_at(in.doc, {"schema"});
  if (!schema) {
    out.unparsed = true;
    return;
  }
  FieldManifestRow row;
  row.path = in.file.path;
  row.schema = *schema;
  row.status = text_at(in.doc, {"status"});
  row.role_manifest_sha256 = sha_at(in.doc, {"role", "manifest_sha256"});
  row.seal_exclusive_end = text_at(in.doc, {"seal", "exclusive_end"});
  row.registry_sha256 = sha_at(in.doc, {"registry", "sha256"});
  row.engine_exe_sha256 = sha_at(in.doc, {"engine", "exe_sha256"});
  row.file_sha256 = in.file.sha256;
  row.doc = compact(in.doc);
  std::vector<FieldEntryRow> entries;
  std::vector<ProducerRow> producers;
  const OJson *fields = member(in.doc, "fields");
  if (fields != nullptr && fields->is_array()) {
    for (usize i = 0; i < fields->size(); ++i) {
      const OJson &entry = (*fields)[i];
      const std::optional<std::string> name = text_at(entry, {"name"});
      const std::optional<std::string> file = text_at(entry, {"file"});
      if (!name || !file) {
        continue;
      }
      FieldEntryRow e;
      e.path = in.file.path;
      e.name = *name;
      e.ord = static_cast<i64>(i);
      e.file = *file;
      e.sha256 = sha_at(entry, {"sha256"});
      e.dtype = text_at(entry, {"dtype"});
      e.formula_sha256 = sha_at(entry, {"formula_sha256"});
      const OJson *reused = member(entry, "reused_from");
      if (reused != nullptr && !reused->is_null()) {
        e.reused_from = compact(*reused);
      }
      if (std::optional<ProducerRow> p = producer_of(member(entry, "producer")); p) {
        e.producer_key = p->producer_key;
        producers.push_back(std::move(*p));
      }
      entries.push_back(std::move(e));
    }
  }
  if (std::optional<ProducerRow> p = manifest_producer(in.doc); p) {
    out.producer_key = p->producer_key;
    producers.push_back(std::move(*p));
  }
  out.writes.emplace_back([row = std::move(row), entries = std::move(entries),
                           producers = std::move(producers)](
                              core::db::Database &db) -> core::Status {
    ATX_TRY_VOID(delete_where(db, "field_entry", "path", row.path));
    for (const ProducerRow &p : producers) {
      ATX_TRY_VOID(upsert(db, p));
    }
    ATX_TRY_VOID(upsert(db, row));
    for (const FieldEntryRow &e : entries) {
      ATX_TRY_VOID(upsert(db, e));
    }
    return core::Ok();
  });
  std::vector<PinRow> pins;
  fields_manifest_pins(PinScope{in.ctx, in.file.path}, in.doc, pins);
  emit_pins(in.file.path, std::move(pins), out);
}

} // namespace atx::engine::research::store::catalog::detail
