// The group `catalog_records` (P9 SQL2): the only TU of this lane that includes SQL1's
// descriptor templates (research/store/detail/*). ops_records.hpp declares what it defines;
// nothing else lives here, so editing a records descriptor recompiles this file alone.

#include "atx/engine/research/store/catalog/ops_records.hpp"

#include <string>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/catalog/rows_records.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/store/catalog/tables_records.hpp"
#include "research/store/detail/table_ops.hpp"

namespace atx::engine::research::store {

namespace {

std::vector<ViewSchema> records_views() {
  return {ViewSchema{std::string{kPinStatusView}, std::string{kPinStatusViewSql}}};
}

constexpr GroupOps kRecordsGroup =
    group_ops<kRunTable, kRunCommandTable, kRunBindingTable, kRunStartTable, kStageReceiptTable,
              kCycleBindingTable, kCycleVerdictTable, kWaveResultTable, kWaveTimingTable,
              kSpecDocTable, kPinTable, kTrialLineTable, kLedgerStateTable, kFieldManifestTable,
              kFieldEntryTable, kCandidateTable, kCandidateEventTable, kBuildReceiptTable,
              kBuildExeTable>("catalog_records", DbKind::Catalog, 1, &records_views);

} // namespace

const GroupOps &records_group() noexcept { return kRecordsGroup; }

// One block of the five per-table operations (ops_records.hpp), as core_ops.cpp. The SQL text
// of insert / upsert is built once per process (thread-safe static) and keys the statement
// cache.
#define ATX_RESEARCH_RECORDS_OPS(Row, kTable, suffix)                                              \
  core::Status insert(core::db::Database &db, const Row &row) {                                    \
    static const std::string sql = insert_sql(kTable);                                             \
    return execute_row(db, sql, kTable, row);                                                      \
  }                                                                                                \
  core::Status upsert(core::db::Database &db, const Row &row) {                                    \
    static const std::string sql = upsert_sql(kTable);                                             \
    return execute_row(db, sql, kTable, row);                                                      \
  }                                                                                                \
  core::Result<Row> read_##suffix(const core::db::Statement &stmt) { return read(stmt, kTable); } \
  core::Result<std::vector<Row>> select_all_##suffix(core::db::Database &db) {                     \
    return select_all(db, kTable);                                                                 \
  }                                                                                                \
  std::string digest(const Row &row) { return row_digest(kTable, row); }

ATX_RESEARCH_RECORDS_OPS(RunRow, kRunTable, run)
ATX_RESEARCH_RECORDS_OPS(RunCommandRow, kRunCommandTable, run_command)
ATX_RESEARCH_RECORDS_OPS(RunBindingRow, kRunBindingTable, run_binding)
ATX_RESEARCH_RECORDS_OPS(RunStartRow, kRunStartTable, run_start)
ATX_RESEARCH_RECORDS_OPS(StageReceiptRow, kStageReceiptTable, stage_receipt)
ATX_RESEARCH_RECORDS_OPS(CycleBindingRow, kCycleBindingTable, cycle_binding)
ATX_RESEARCH_RECORDS_OPS(CycleVerdictRow, kCycleVerdictTable, cycle_verdict)
ATX_RESEARCH_RECORDS_OPS(WaveResultRow, kWaveResultTable, wave_result)
ATX_RESEARCH_RECORDS_OPS(WaveTimingRow, kWaveTimingTable, wave_timing)
ATX_RESEARCH_RECORDS_OPS(SpecDocRow, kSpecDocTable, spec_doc)
ATX_RESEARCH_RECORDS_OPS(PinRow, kPinTable, pin)
ATX_RESEARCH_RECORDS_OPS(TrialLineRow, kTrialLineTable, trial_line)
ATX_RESEARCH_RECORDS_OPS(LedgerStateRow, kLedgerStateTable, ledger_state)
ATX_RESEARCH_RECORDS_OPS(FieldManifestRow, kFieldManifestTable, field_manifest)
ATX_RESEARCH_RECORDS_OPS(FieldEntryRow, kFieldEntryTable, field_entry)
ATX_RESEARCH_RECORDS_OPS(CandidateRow, kCandidateTable, candidate)
ATX_RESEARCH_RECORDS_OPS(CandidateEventRow, kCandidateEventTable, candidate_event)
ATX_RESEARCH_RECORDS_OPS(BuildReceiptRow, kBuildReceiptTable, build_receipt)
ATX_RESEARCH_RECORDS_OPS(BuildExeRow, kBuildExeTable, build_exe)

#undef ATX_RESEARCH_RECORDS_OPS

} // namespace atx::engine::research::store
