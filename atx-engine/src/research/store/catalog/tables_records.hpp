#pragma once

// Descriptors of the group `catalog_records` (P9 SQL2, K-P9-13, sql-design section 3.7), in
// table order, and the `pin_status` view. PRIVATE header: included only by records_ops.cpp
// (the group's one instantiating TU). Editing a descriptor or the view changes the printed
// schema: the fixture atx-engine/tests/fixtures/research_store/schema/catalog_records.json
// must change with it (gtest ResearchCatalog.RecordsSchemaJsonEqualsFixture).
//
// Per-class render metadata (mode typed / doc, rule) is data in
// atx-engine/schemas/research_store/classes.json, not part of these descriptors.

#include <array>
#include <string_view>

#include "atx/engine/research/store/catalog/rows_records.hpp"
#include "research/store/detail/table.hpp"

namespace atx::engine::research::store {

inline constexpr std::array<std::string_view, 10> kSpecDocKinds{
    "spec",   "template", "wave-manifest", "candidate", "library",
    "recipe", "registry", "window",        "field-registry", "other"};

inline constexpr std::array<std::string_view, 15> kPinKinds{
    "input",        "locked",        "fields-manifest", "rule-template", "exe",
    "binding",      "receipt-binding", "receipt-log",   "spec-digest",   "ledger-head",
    "wave-manifest", "stage-receipt", "field-payload",  "field-registry", "field-source"};

inline constexpr auto kRunTable = table<RunRow>(
    "run",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("run_dir", &RunRow::run_dir, kKey),
    col<Sql::Text>("receipt_schema", &RunRow::receipt_schema),
    col<Sql::Text>("source_sha", &RunRow::source_sha),
    col<Sql::Sha256>("executable_sha256", &RunRow::executable_sha256, kIndexed),
    col<Sql::Sha256>("argv_sha256", &RunRow::argv_sha256, kIndexed),
    col<Sql::Int>("attempt", &RunRow::attempt),
    col<Sql::Text>("build_type", &RunRow::build_type),
    col<Sql::Text>("role_id", &RunRow::role_id),
    col<Sql::Text>("outcome", &RunRow::outcome, kIndexed),
    col<Sql::Int>("exit_code", &RunRow::exit_code),
    col<Sql::Text>("git_state", &RunRow::git_state),
    col<Sql::Text>("error", &RunRow::error),
    col<Sql::Json>("limits", &RunRow::limits),
    col<Sql::Json>("admission", &RunRow::admission, kVolatile),
    col<Sql::Json>("dirty_outside", &RunRow::dirty_outside),
    col<Sql::Json>("store", &RunRow::store),
    col<Sql::Text>("started_utc", &RunRow::started_utc, kVolatile),
    col<Sql::Real>("wall_seconds", &RunRow::wall_seconds, kVolatile),
    col<Sql::Int>("sampled_peak_tree_rss_bytes", &RunRow::sampled_peak_tree_rss_bytes, kVolatile),
    col<Sql::Int>("minimum_system_free_bytes", &RunRow::minimum_system_free_bytes, kVolatile),
    col<Sql::Json>("owned_processes", &RunRow::owned_processes, kVolatile),
    col<Sql::Text>("ownership_scope", &RunRow::ownership_scope),
    col<Sql::Json>("logs", &RunRow::logs),
    col<Sql::Json>("key_order", &RunRow::key_order),
    col<Sql::Json>("extra", &RunRow::extra),
    col<Sql::Sha256>("file_sha256", &RunRow::file_sha256));

inline constexpr auto kRunCommandTable = table<RunCommandRow>(
    "run_command",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("run_dir", &RunCommandRow::run_dir, kKey),
    col<Sql::Int>("ord", &RunCommandRow::ord, kKey),
    col<Sql::Text>("arg", &RunCommandRow::arg));

inline constexpr auto kRunBindingTable = table<RunBindingRow>(
    "run_binding",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("run_dir", &RunBindingRow::run_dir, kKey),
    col<Sql::Int>("ord", &RunBindingRow::ord, kKey),
    col<Sql::Text>("path", &RunBindingRow::path),
    col<Sql::Sha256>("sha256", &RunBindingRow::sha256));

inline constexpr auto kRunStartTable = table<RunStartRow>(
    "run_start",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("run_dir", &RunStartRow::run_dir, kKey),
    col<Sql::Sha256>("file_sha256", &RunStartRow::file_sha256),
    col<Sql::Json>("doc", &RunStartRow::doc));

inline constexpr auto kStageReceiptTable = table<StageReceiptRow>(
    "stage_receipt",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("state_dir", &StageReceiptRow::state_dir, kKey),
    col<Sql::Text>("file_name", &StageReceiptRow::file_name, kKey),
    col<Sql::Int>("stage_index", &StageReceiptRow::stage_index),
    col<Sql::Text>("stage", &StageReceiptRow::stage, kIndexed),
    col<Sql::Text>("status", &StageReceiptRow::status),
    col<Sql::Text>("schema", &StageReceiptRow::schema),
    col<Sql::Text>("chain", &StageReceiptRow::chain),
    col<Sql::Json>("inputs", &StageReceiptRow::inputs),
    col<Sql::Json>("outputs", &StageReceiptRow::outputs),
    col<Sql::Text>("started_utc", &StageReceiptRow::started_utc, kVolatile),
    col<Sql::Real>("seconds", &StageReceiptRow::seconds, kVolatile),
    col<Sql::Json>("extra", &StageReceiptRow::extra),
    col<Sql::Sha256>("file_sha256", &StageReceiptRow::file_sha256));

inline constexpr auto kCycleBindingTable = table<CycleBindingRow>(
    "cycle_binding",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("run_dir", &CycleBindingRow::run_dir, kKey),
    col<Sql::Text>("schema", &CycleBindingRow::schema),
    col<Sql::RelPath>("output", &CycleBindingRow::output),
    col<Sql::Sha256>("spec_sha256", &CycleBindingRow::spec_sha256),
    col<Sql::Text>("spec_rule", &CycleBindingRow::spec_rule),
    col<Sql::Sha256>("argv_sha256", &CycleBindingRow::argv_sha256),
    col<Sql::Json>("key_order", &CycleBindingRow::key_order),
    col<Sql::Json>("extra", &CycleBindingRow::extra),
    col<Sql::Sha256>("file_sha256", &CycleBindingRow::file_sha256));

inline constexpr auto kCycleVerdictTable = table<CycleVerdictRow>(
    "cycle_verdict",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("path", &CycleVerdictRow::path, kKey),
    col<Sql::Text>("schema", &CycleVerdictRow::schema),
    col<Sql::Text>("cycle", &CycleVerdictRow::cycle),
    col<Sql::Text>("mode", &CycleVerdictRow::mode),
    col<Sql::Sha256>("spec_sha256", &CycleVerdictRow::spec_sha256),
    col<Sql::RelPath>("ledger_path", &CycleVerdictRow::ledger_path),
    col<Sql::Sha256>("ledger_head", &CycleVerdictRow::ledger_head),
    col<Sql::Int>("ledger_lines", &CycleVerdictRow::ledger_lines),
    col<Sql::Json>("doc", &CycleVerdictRow::doc),
    col<Sql::Sha256>("file_sha256", &CycleVerdictRow::file_sha256));

inline constexpr auto kWaveResultTable = table<WaveResultRow>(
    "wave_result",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("path", &WaveResultRow::path, kKey),
    col<Sql::Text>("schema", &WaveResultRow::schema),
    col<Sql::Text>("wave", &WaveResultRow::wave),
    col<Sql::Text>("kind", &WaveResultRow::kind),
    col<Sql::RelPath>("manifest_path", &WaveResultRow::manifest_path),
    col<Sql::Sha256>("manifest_sha256", &WaveResultRow::manifest_sha256),
    col<Sql::Bool>("accepted", &WaveResultRow::accepted),
    col<Sql::Sha256>("ledger_head", &WaveResultRow::ledger_head),
    col<Sql::Int>("n_before", &WaveResultRow::n_before),
    col<Sql::Int>("n_after", &WaveResultRow::n_after),
    col<Sql::Text>("trial_id", &WaveResultRow::trial_id),
    col<Sql::RelPath>("next_parent_spec", &WaveResultRow::next_parent_spec),
    col<Sql::Json>("doc", &WaveResultRow::doc),
    col<Sql::Sha256>("file_sha256", &WaveResultRow::file_sha256));

inline constexpr auto kWaveTimingTable = table<WaveTimingRow>(
    "wave_timing",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("path", &WaveTimingRow::path, kKey),
    col<Sql::Int>("ord", &WaveTimingRow::ord, kKey),
    col<Sql::Text>("phase", &WaveTimingRow::phase),
    col<Sql::RelPath>("run_dir", &WaveTimingRow::run_dir),
    col<Sql::Text>("outcome", &WaveTimingRow::outcome),
    col<Sql::Int>("exit_code", &WaveTimingRow::exit_code),
    col<Sql::Real>("seconds", &WaveTimingRow::seconds, kVolatile),
    col<Sql::Int>("peak_mib", &WaveTimingRow::peak_mib, kVolatile),
    col<Sql::Sha256>("executable_sha256", &WaveTimingRow::executable_sha256));

inline constexpr auto kSpecDocTable = table<SpecDocRow>(
    "spec_doc",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("path", &SpecDocRow::path, kKey),
    col<Sql::Text>("kind", &SpecDocRow::kind, 0, kSpecDocKinds),
    col<Sql::Text>("schema", &SpecDocRow::schema),
    col<Sql::Sha256>("file_sha256", &SpecDocRow::file_sha256),
    col<Sql::Bool>("git_tracked", &SpecDocRow::git_tracked),
    col<Sql::Text>("parent", &SpecDocRow::parent),
    col<Sql::Json>("doc", &SpecDocRow::doc));

inline constexpr auto kPinTable = table<PinRow>(
    "pin",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("holder_path", &PinRow::holder_path, kKey),
    col<Sql::Text>("pointer", &PinRow::pointer, kKey),
    col<Sql::Text>("pin_kind", &PinRow::pin_kind, 0, kPinKinds),
    col<Sql::Text>("target_path", &PinRow::target_path, kIndexed),
    col<Sql::Sha256>("target_sha256", &PinRow::target_sha256, kIndexed));

inline constexpr auto kTrialLineTable = table<TrialLineRow>(
    "trial_line",
    TableOpts{.version = 1, .since = 1, .append_only = true, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("ledger_path", &TrialLineRow::ledger_path, kKey),
    col<Sql::Int>("seq", &TrialLineRow::seq, kKey),
    col<Sql::Text>("line", &TrialLineRow::line),
    col<Sql::Sha256>("line_sha256", &TrialLineRow::line_sha256),
    col<Sql::Text>("schema", &TrialLineRow::schema),
    col<Sql::Text>("kind", &TrialLineRow::kind),
    col<Sql::Text>("cell", &TrialLineRow::cell),
    col<Sql::Text>("trial_id", &TrialLineRow::trial_id),
    col<Sql::Int>("count", &TrialLineRow::count),
    col<Sql::Sha256>("prev_sha256", &TrialLineRow::prev_sha256));

inline constexpr auto kLedgerStateTable = table<LedgerStateRow>(
    "ledger_state",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("ledger_path", &LedgerStateRow::ledger_path, kKey),
    col<Sql::Int>("lines", &LedgerStateRow::lines),
    col<Sql::Sha256>("file_sha256", &LedgerStateRow::file_sha256),
    col<Sql::Sha256>("head_sha256", &LedgerStateRow::head_sha256),
    col<Sql::Text>("head_rule", &LedgerStateRow::head_rule));

inline constexpr auto kFieldManifestTable = table<FieldManifestRow>(
    "field_manifest",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("path", &FieldManifestRow::path, kKey),
    col<Sql::Text>("schema", &FieldManifestRow::schema),
    col<Sql::Text>("status", &FieldManifestRow::status),
    col<Sql::Sha256>("role_manifest_sha256", &FieldManifestRow::role_manifest_sha256),
    col<Sql::Text>("seal_exclusive_end", &FieldManifestRow::seal_exclusive_end),
    col<Sql::Sha256>("registry_sha256", &FieldManifestRow::registry_sha256),
    col<Sql::Sha256>("engine_exe_sha256", &FieldManifestRow::engine_exe_sha256),
    col<Sql::Sha256>("file_sha256", &FieldManifestRow::file_sha256),
    col<Sql::Json>("doc", &FieldManifestRow::doc));

inline constexpr auto kFieldEntryTable = table<FieldEntryRow>(
    "field_entry",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("path", &FieldEntryRow::path, kKey),
    col<Sql::Text>("name", &FieldEntryRow::name, kKey),
    col<Sql::Int>("ord", &FieldEntryRow::ord),
    col<Sql::Text>("file", &FieldEntryRow::file),
    col<Sql::Sha256>("sha256", &FieldEntryRow::sha256),
    col<Sql::Text>("dtype", &FieldEntryRow::dtype),
    col<Sql::Sha256>("producer_key", &FieldEntryRow::producer_key),
    col<Sql::Sha256>("formula_sha256", &FieldEntryRow::formula_sha256),
    col<Sql::Text>("reused_from", &FieldEntryRow::reused_from));

inline constexpr auto kCandidateTable = table<CandidateRow>(
    "candidate",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("id", &CandidateRow::id, kKey),
    col<Sql::RelPath>("path", &CandidateRow::path),
    col<Sql::Text>("status", &CandidateRow::status),
    col<Sql::Text>("wave", &CandidateRow::wave),
    col<Sql::Sha256>("dsl_sha256", &CandidateRow::dsl_sha256),
    col<Sql::Sha256>("file_sha256", &CandidateRow::file_sha256),
    col<Sql::Json>("doc", &CandidateRow::doc));

inline constexpr auto kCandidateEventTable = table<CandidateEventRow>(
    "candidate_event",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("id", &CandidateEventRow::id, kKey),
    col<Sql::Int>("ord", &CandidateEventRow::ord, kKey),
    col<Sql::Text>("status", &CandidateEventRow::status),
    col<Sql::Text>("at", &CandidateEventRow::at),
    col<Sql::Text>("by", &CandidateEventRow::by),
    col<Sql::Text>("wave", &CandidateEventRow::wave),
    col<Sql::Text>("note", &CandidateEventRow::note));

inline constexpr auto kBuildReceiptTable = table<BuildReceiptRow>(
    "build_receipt",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("tag", &BuildReceiptRow::tag, kKey),
    col<Sql::RelPath>("path", &BuildReceiptRow::path),
    col<Sql::Text>("preset", &BuildReceiptRow::preset),
    col<Sql::Text>("source", &BuildReceiptRow::source),
    col<Sql::Int>("exit_code", &BuildReceiptRow::exit_code),
    col<Sql::Real>("wall_seconds", &BuildReceiptRow::wall_seconds, kVolatile),
    col<Sql::Sha256>("file_sha256", &BuildReceiptRow::file_sha256),
    col<Sql::Json>("doc", &BuildReceiptRow::doc));

inline constexpr auto kBuildExeTable = table<BuildExeRow>(
    "build_exe",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("tag", &BuildExeRow::tag, kKey),
    col<Sql::Text>("target", &BuildExeRow::target, kKey),
    col<Sql::Sha256>("sha256", &BuildExeRow::sha256, kIndexed));

// pin x artifact (sql-design 3.7, ruling SQL-7). One line: the printed schema carries it as a
// JSON string. States: unresolved (no target path: a digest pin or an outside-root target; or
// a field-source pin, never followed, whose target was not catalogued), missing (no artifact
// row), stale (the artifact's SHA-256 differs), declared (equal, but only declared by a
// manifest: the catalog never opened the file), ok (equal and verified by the catalog).
inline constexpr std::string_view kPinStatusViewSql =
    "CREATE VIEW pin_status AS SELECT p.holder_path AS holder_path, p.pointer AS pointer, "
    "p.pin_kind AS pin_kind, p.target_path AS target_path, p.target_sha256 AS target_sha256, "
    "a.sha256 AS artifact_sha256, a.sha_source AS sha_source, CASE WHEN p.target_path IS NULL "
    "OR (p.pin_kind = 'field-source' AND a.path_key IS NULL) THEN 'unresolved' WHEN a.path_key "
    "IS NULL THEN 'missing' WHEN a.sha256 <> p.target_sha256 THEN 'stale' WHEN a.sha_source = "
    "'declared' THEN 'declared' ELSE 'ok' END AS state FROM pin AS p LEFT JOIN artifact AS a ON "
    "a.path_key = p.target_path;";

} // namespace atx::engine::research::store
