#pragma once

// atx::engine::research::store::catalog -- ingest of one opened file into rows (P9 SQL2;
// sql-design section 3.9).
//
// A file the catalog opened (never a sealed one, never one over kMaxOpenBytes) is classified
// (classify.hpp) and handed to its class's ingest family. A json family parses the bytes with
// nlohmann ordered_json (key order kept) and fills its typed columns; a NaN token, an
// out-of-range number, invalid UTF-8, or a document not of the family's shape leaves the file
// as an artifact only (skipped_path reason `unparsed`). The result is the file's database
// writes (run in one BEGIN IMMEDIATE segment by the caller), the pins it states and what a
// catalog walk follows from it (pin targets, named files, named run / cycle / wave dirs).
//
// Ledger chain head -- the ONE seam to lane B2 (wave2-carry "B2", rulings SQL-7 / SQL-9).
// The catalog never re-implements the trial ledger's chain rule. ledger_state.head_sha256 /
// head_rule are filled only by a LedgerHeadFn: B2's public research/ledger chain-head function
// takes the ledger's line texts in file order (each without its line end) and returns the
// chain head under the existing rule plus the rule's name. default_ledger_head() is the single
// place that function is bound: P9 lane SQL4 binds it to research/ledger chain_head (chain.hpp),
// so the CLI's catalog and ingest fill the head; a caller passing no function keeps it NULL.
//
// verify --ledger (SQL4, rulings SQL-7 / SQL-9): verify_ledger_heads below checks the catalogued
// heads against the catalogued lines of named ledgers, through B2's prefix_heads only. The JSONL
// ledger stays the authority and the index never decides N: a line edited in the file after it
// was catalogued is refused by the next catalog run (trial_line is append-only), so a caller
// re-catalogues, then verifies.

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/classify.hpp"
#include "atx/engine/research/store/catalog/rows_records.hpp"

namespace atx::engine::research::store::catalog {

struct LedgerHead {
  std::string head_sha256; // 64 lower-case hex
  std::string rule;        // the chain rule's name (ledger_state.head_rule)
};

// Lines: the ledger's non-blank line texts in file order, without line ends.
using LedgerHeadFn = std::function<core::Result<LedgerHead>(std::span<const std::string> lines)>;

// The seam (see the header comment): an empty function until B2's chain-head function is
// bound here (ingest_ledger.cpp).
[[nodiscard]] LedgerHeadFn default_ledger_head();

// A database write of one ingested file (rows, child rows, pins). Runs inside the caller's
// BEGIN IMMEDIATE; it may run again when that transaction is retried, so it touches nothing
// outside the database.
using DbWrite = std::function<core::Status(core::db::Database &)>;

struct IngestContext {
  std::filesystem::path root; // absolute, lexically normal (normal_root)
  LedgerHeadFn ledger_head;   // may be empty: ledger heads stay NULL
};

// One opened file.
struct FileView {
  std::string path;       // root-relative '/'-path
  std::string sha256;     // verified: hashed by the catalog
  std::string_view bytes; // the whole file
  const ClassInfo *cls{}; // non-null
};

struct IngestResult {
  std::vector<DbWrite> writes;           // typed rows, child rows, pins
  std::vector<PinRow> pins;              // the pins the file states (also in `writes`)
  std::vector<std::string> named_files;  // root-relative files a walk visits (pin targets ...)
  std::vector<std::string> named_dirs;   // root-relative dirs a walk lists (run / wave dirs)
  std::vector<std::string> named_prefixes; // "<dir>/<name prefix>": sibling dirs a walk lists
  std::optional<std::string> json_schema;  // artifact.json_schema
  std::optional<std::string> producer_key; // artifact.producer_key
  bool unparsed{false};
};

// "lf", "crlf", "mixed" or "none" (no line feed at all).
[[nodiscard]] std::string_view detect_eol(std::string_view bytes) noexcept;

// Classify-free ingest of `file` by its class's family. Err only for a refusal the caller
// must stop on (Err(PermissionDenied): a trial ledger whose stored lines changed or shrank;
// trial_line is append-only); every malformed file is IngestResult::unparsed instead.
[[nodiscard]] core::Result<IngestResult> ingest_file(const IngestContext &ctx,
                                                     const FileView &file);

// One named ledger of verify_ledger_heads: its catalogued lines and their chain head.
struct LedgerIndexCheck {
  std::string ledger_path;            // as catalogued (trial_line.ledger_path), '/'-separated
  i64 lines{};                        // trial_line rows
  std::string head_sha256;            // chain_head of those lines (genesis when none)
  std::string rule;                   // the chain rule's name
  std::optional<std::string> problem; // the index is inconsistent (nullopt: consistent)
};
// One head a catalogued verdict or wave result records (its non-null ledger_head).
struct RecordedLedgerHead {
  std::string table;                      // "cycle_verdict" or "wave_result"
  std::string path;                       // the record's root-relative path
  std::string head_sha256;
  std::optional<std::string> ledger_path; // the ledger the record names (nullopt: none)
  std::optional<i64> prefix_lines;        // k: the head of the first k lines; nullopt: no prefix
};
struct LedgerVerifyReport {
  std::vector<LedgerIndexCheck> ledgers;    // in the order named
  std::vector<RecordedLedgerHead> recorded; // the heads checked, table then path order
  i64 other{};                              // heads naming a ledger not named: not checked
  [[nodiscard]] bool ok() const noexcept;   // every ledger consistent, every head a prefix head
};
// `verify --ledger L...`: for each named ledger (its catalogued path; '\' read as '/') the
// catalogued lines, their prefix heads (genesis, then one per line) and the consistency of its
// ledger_state row (present; `lines` equal to the rows; a stored head equal to the lines' chain
// head under kChainRule); then every recorded head: one naming a named ledger must be a prefix
// head of that ledger, one naming no ledger a prefix head of some named ledger. A tail edit (a
// line a recorded head covers, changed before the catalog read the ledger) leaves that head
// outside every prefix. Reads only; Err for a database or chain-rule error (a line that is not a
// JSON object).
[[nodiscard]] core::Result<LedgerVerifyReport>
verify_ledger_heads(core::db::Database &db, std::span<const std::string> ledger_paths);

} // namespace atx::engine::research::store::catalog
