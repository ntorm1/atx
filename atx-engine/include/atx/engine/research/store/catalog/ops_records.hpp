#pragma once

// atx::engine::research::store -- non-template operations of the group `catalog_records`
// (P9 SQL2). Defined in src/research/store/catalog/records_ops.cpp, the group's only
// instantiating TU. Same contract per table X as ops_core.hpp:
//   insert(db, row)    INSERT; Err(AlreadyExists) for a duplicate key, Err(InvalidArgument)
//                      for a value a CHECK / trigger refuses (or a NaN / -0.0 REAL)
//   upsert(db, row)    INSERT ... ON CONFLICT(key) DO UPDATE SET every non-key column
//                      (trial_line is append-only: its upsert of an existing key raises)
//   read_X(stmt)       the current row of a statement whose result columns are the table's
//                      columns in declared order
//   select_all_X(db)   every row, ORDER BY the key
//   digest(row)        atx.record-digest/v1 of the row (64 lower-case hex)
// Statements go through the connection's prepare_cached; the caller owns transactions.
// Thread-safety: as the Database (one thread per connection).

#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/catalog/rows_records.hpp"
#include "atx/engine/research/store/store.hpp"

namespace atx::engine::research::store {

// The catalog DB's records group (version 1, DbKind::Catalog): run, run_command, run_binding,
// run_start, stage_receipt, cycle_binding, cycle_verdict, wave_result, wave_timing, spec_doc,
// pin, trial_line, ledger_state, field_manifest, field_entry, candidate, candidate_event,
// build_receipt, build_exe; view pin_status. The catalog DB is {core_group(), records_group()}.
[[nodiscard]] const GroupOps &records_group() noexcept;

// The view's name (its SQL lives with the descriptors).
inline constexpr std::string_view kPinStatusView = "pin_status";

#define ATX_RESEARCH_RECORDS_DECLARE(Row, suffix)                                                  \
  [[nodiscard]] core::Status insert(core::db::Database &db, const Row &row);                       \
  [[nodiscard]] core::Status upsert(core::db::Database &db, const Row &row);                       \
  [[nodiscard]] core::Result<Row> read_##suffix(const core::db::Statement &stmt);                  \
  [[nodiscard]] core::Result<std::vector<Row>> select_all_##suffix(core::db::Database &db);        \
  [[nodiscard]] std::string digest(const Row &row);

ATX_RESEARCH_RECORDS_DECLARE(RunRow, run)
ATX_RESEARCH_RECORDS_DECLARE(RunCommandRow, run_command)
ATX_RESEARCH_RECORDS_DECLARE(RunBindingRow, run_binding)
ATX_RESEARCH_RECORDS_DECLARE(RunStartRow, run_start)
ATX_RESEARCH_RECORDS_DECLARE(StageReceiptRow, stage_receipt)
ATX_RESEARCH_RECORDS_DECLARE(CycleBindingRow, cycle_binding)
ATX_RESEARCH_RECORDS_DECLARE(CycleVerdictRow, cycle_verdict)
ATX_RESEARCH_RECORDS_DECLARE(WaveResultRow, wave_result)
ATX_RESEARCH_RECORDS_DECLARE(WaveTimingRow, wave_timing)
ATX_RESEARCH_RECORDS_DECLARE(SpecDocRow, spec_doc)
ATX_RESEARCH_RECORDS_DECLARE(PinRow, pin)
ATX_RESEARCH_RECORDS_DECLARE(TrialLineRow, trial_line)
ATX_RESEARCH_RECORDS_DECLARE(LedgerStateRow, ledger_state)
ATX_RESEARCH_RECORDS_DECLARE(FieldManifestRow, field_manifest)
ATX_RESEARCH_RECORDS_DECLARE(FieldEntryRow, field_entry)
ATX_RESEARCH_RECORDS_DECLARE(CandidateRow, candidate)
ATX_RESEARCH_RECORDS_DECLARE(CandidateEventRow, candidate_event)
ATX_RESEARCH_RECORDS_DECLARE(BuildReceiptRow, build_receipt)
ATX_RESEARCH_RECORDS_DECLARE(BuildExeRow, build_exe)

#undef ATX_RESEARCH_RECORDS_DECLARE

} // namespace atx::engine::research::store
