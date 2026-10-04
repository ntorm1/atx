// Ingest of wave results (`atx.wave-result/v1`, <wave dir>/wave-result.json; writer
// scripts/wave_stage_record.py through wave_context.Wave.write_json: json.dumps(indent=2,
// allow_nan=False) + "\n", LF; doc render) with their `timings` rows extracted to wave_timing.
// The statistics (stats, paired, dsr, pbo, bundle, ...) stay inside `doc`.

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

[[nodiscard]] std::optional<std::string> nested_text(const OJson &doc,
                                                     std::initializer_list<std::string_view> at) {
  const OJson *v = member_at(doc, at);
  return v != nullptr ? fit_text(*v) : std::nullopt;
}

[[nodiscard]] std::vector<WaveTimingRow> timing_rows(const IngestContext &ctx, const OJson &doc,
                                                     const std::string &path,
                                                     std::vector<std::string> &run_dirs) {
  std::vector<WaveTimingRow> rows;
  const OJson *timings = member(doc, "timings");
  if (timings == nullptr || !timings->is_array()) {
    return rows;
  }
  for (usize i = 0; i < timings->size(); ++i) {
    const OJson &t = (*timings)[i];
    if (!t.is_object()) {
      continue;
    }
    WaveTimingRow row;
    row.path = path;
    row.ord = static_cast<i64>(i);
    const OJson *phase = member(t, "phase");
    row.phase = phase != nullptr ? fit_text(*phase).value_or(std::string{}) : std::string{};
    if (const std::optional<std::string> dir = nested_text(t, {"run_dir"}); dir) {
      if (std::optional<std::string> rel = root_relative(ctx.root, *dir); rel) {
        row.run_dir = *rel;
        run_dirs.push_back(std::move(*rel));
      }
    }
    row.outcome = nested_text(t, {"outcome"});
    if (const OJson *v = member(t, "exit_code"); v != nullptr) {
      row.exit_code = fit_int(*v);
    }
    if (const OJson *v = member(t, "seconds"); v != nullptr) {
      row.seconds = fit_real(*v);
    }
    if (const OJson *v = member(t, "peak_mib"); v != nullptr) {
      row.peak_mib = fit_int(*v);
    }
    if (const OJson *v = member(t, "executable_sha256"); v != nullptr) {
      row.executable_sha256 = fit_sha(*v);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

} // namespace

void ingest_wave_result(const FamilyInput &in, IngestResult &out) {
  DocFields f{in.doc};
  WaveResultRow row;
  row.path = in.file.path;
  row.schema = f.req_text("schema");
  row.wave = f.req_text("wave");
  row.kind = f.req_text("kind");
  const std::optional<std::string> manifest = nested_text(in.doc, {"manifest", "path"});
  const OJson *manifest_sha = member_at(in.doc, {"manifest", "sha256"});
  std::optional<std::string> manifest_rel =
      manifest ? root_relative(in.ctx.root, *manifest) : std::nullopt;
  const std::optional<std::string> manifest_digest =
      manifest_sha != nullptr ? fit_sha(*manifest_sha) : std::nullopt;
  if (!f.shape_ok() || !manifest_rel || !manifest_digest) {
    out.unparsed = true;
    return;
  }
  row.manifest_path = *manifest_rel;
  row.manifest_sha256 = *manifest_digest;
  if (const OJson *v = member_at(in.doc, {"verdict", "accepted"}); v != nullptr) {
    row.accepted = fit_bool(*v);
  }
  if (const OJson *v = member_at(in.doc, {"ledger", "head"}); v != nullptr) {
    row.ledger_head = fit_sha(*v);
  }
  if (const OJson *v = member_at(in.doc, {"ledger", "n_before"}); v != nullptr) {
    row.n_before = fit_int(*v);
  }
  if (const OJson *v = member_at(in.doc, {"ledger", "n_after"}); v != nullptr) {
    row.n_after = fit_int(*v);
  }
  row.trial_id = nested_text(in.doc, {"ledger", "trial_id"});
  if (const OJson *v = member_at(in.doc, {"next_parent", "spec"}); v != nullptr) {
    row.next_parent_spec = fit_relpath(*v);
  }
  row.doc = compact(in.doc);
  row.file_sha256 = in.file.sha256;
  std::vector<WaveTimingRow> timings = timing_rows(in.ctx, in.doc, in.file.path, out.named_dirs);
  if (const std::optional<std::string> ledger = nested_text(in.doc, {"ledger", "path"}); ledger) {
    if (std::optional<std::string> rel = root_relative(in.ctx.root, *ledger); rel) {
      out.named_files.push_back(std::move(*rel));
    }
  }
  out.writes.emplace_back([row = std::move(row), timings = std::move(timings)](
                              core::db::Database &db) -> core::Status {
    ATX_TRY_VOID(delete_where(db, "wave_timing", "path", row.path));
    ATX_TRY_VOID(upsert(db, row));
    for (const WaveTimingRow &t : timings) {
      ATX_TRY_VOID(insert(db, t));
    }
    return core::Ok();
  });
  std::vector<PinRow> pins;
  wave_result_pins(PinScope{in.ctx, in.file.path}, in.doc, pins);
  emit_pins(in.file.path, std::move(pins), out);
}

} // namespace atx::engine::research::store::catalog::detail
