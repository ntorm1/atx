#pragma once

// atx::engine::research::store -- plain row structs of the group `catalog_records` (P9 SQL2,
// contract K-P9-13; sql-design section 3.7). No templates, no SQLite, no JSON type: consumers
// include this, ops_records.hpp and store.hpp only. std::optional members are the nullable
// columns; every other member is NOT NULL. Comments name the column kind and flags (key =
// primary-key member, ix = indexed, volatile = stored but never digested).
//
// Blind by construction (sql-design section 3.5): no typed column holds a return, IC, Sharpe,
// turnover, drawdown, DSR / PSR / PBO or NAV value. Documents that carry statistics (verdicts,
// wave results) keep them inside their `doc` JSON column; the catalog file is root-only.
//
// Typed-render tables (run, cycle_binding) keep the document's top-level key order in
// `key_order` and every key the typed columns do not hold (or a value that does not fit its
// column's type) in `extra`, in document order; `extra` wins over a typed column when the
// renderer rebuilds the document (atx-engine/tools/research_store_identity.py).

#include <optional>
#include <string>

#include "atx/core/types.hpp"

namespace atx::engine::research::store {

// run: one bounded-run receipt (`atx.bounded-research-run/v1`, receipt.json). Render: typed.
struct RunRow {
  std::string run_dir;                                  // relpath [key]
  std::string receipt_schema;                           // text ("schema")
  std::optional<std::string> source_sha;                // text
  std::optional<std::string> executable_sha256;         // sha256 [ix]
  std::optional<std::string> argv_sha256;               // sha256 [ix]
  std::optional<i64> attempt;                           // int
  std::optional<std::string> build_type;                // text
  std::optional<std::string> role_id;                   // text
  std::string outcome;                                  // text [ix]
  std::optional<i64> exit_code;                         // int
  std::optional<std::string> git_state;                 // text ("git")
  std::optional<std::string> error;                     // text
  std::optional<std::string> limits;                    // json
  std::optional<std::string> admission;                 // json, volatile (host admission wait)
  std::optional<std::string> dirty_outside;             // json ("dirty_outside_pathspec")
  std::optional<std::string> store;                     // json (SQL3's receipt block)
  std::optional<std::string> started_utc;               // text, volatile
  std::optional<f64> wall_seconds;                      // real, volatile
  std::optional<i64> sampled_peak_tree_rss_bytes;       // int, volatile
  std::optional<i64> minimum_system_free_bytes;         // int, volatile
  std::optional<std::string> owned_processes;           // json, volatile
  std::optional<std::string> ownership_scope;           // text
  std::optional<std::string> logs;                      // json
  std::string key_order;                                // json: top-level keys in order
  std::optional<std::string> extra;                     // json: keys not held above
  std::string file_sha256;                              // sha256
};

// run_command: the receipt's `command`, one row per argument.
struct RunCommandRow {
  std::string run_dir; // relpath [key]
  i64 ord{};           // int [key]: 0-based position
  std::string arg;     // text
};

// run_binding: the receipt's `bindings`, one row per {path, sha256}.
struct RunBindingRow {
  std::string run_dir; // relpath [key]
  i64 ord{};           // int [key]
  std::string path;    // text (as written: absolute)
  std::string sha256;  // sha256
};

// run_start: a launch receipt (start.json). Render: doc.
struct RunStartRow {
  std::string run_dir;     // relpath [key]
  std::string file_sha256; // sha256
  std::string doc;         // json
};

// stage_receipt: one stage receipt (`atx.stage-receipt/v1`; .failed-k files keep their own
// file name). Render: typed, rule py-indent2-sorted (sorted keys: no key order is stored).
struct StageReceiptRow {
  std::string state_dir;            // relpath [key]: the dir holding receipts/
  std::string file_name;            // text [key]
  i64 stage_index{};                // int ("index")
  std::string stage;                // text [ix]
  std::string status;               // text
  std::string schema;               // text
  std::string chain;                // text
  std::string inputs;               // json
  std::string outputs;              // json
  std::string started_utc;          // text, volatile
  f64 seconds{};                    // real, volatile
  std::optional<std::string> extra; // json (error, code, ...)
  std::string file_sha256;          // sha256
};

// cycle_binding: a NAV cycle binding (`atx.cycle-nav-binding/v1`). Render: typed.
struct CycleBindingRow {
  std::string run_dir;                    // relpath [key]
  std::string schema;                     // text
  std::string output;                     // relpath
  std::optional<std::string> spec_sha256; // sha256 (null for the ref phase's binding)
  std::optional<std::string> spec_rule;   // text
  std::optional<std::string> argv_sha256; // sha256
  std::string key_order;                  // json
  std::optional<std::string> extra;       // json
  std::string file_sha256;                // sha256
};

// cycle_verdict: a cycle verdict or one of its verdicts/ copies. Render: doc.
struct CycleVerdictRow {
  std::string path;                        // relpath [key]
  std::string schema;                      // text
  std::string cycle;                       // text
  std::string mode;                        // text
  std::optional<std::string> spec_sha256;  // sha256
  std::optional<std::string> ledger_path;  // relpath
  std::optional<std::string> ledger_head;  // sha256
  std::optional<i64> ledger_lines;         // int
  std::string doc;                         // json
  std::string file_sha256;                 // sha256
};

// wave_result: a wave result (`atx.wave-result/v1`). Render: doc.
struct WaveResultRow {
  std::string path;                            // relpath [key]
  std::string schema;                          // text
  std::string wave;                            // text
  std::string kind;                            // text
  std::string manifest_path;                   // relpath
  std::string manifest_sha256;                 // sha256
  std::optional<bool> accepted;                // bool
  std::optional<std::string> ledger_head;      // sha256
  std::optional<i64> n_before;                 // int
  std::optional<i64> n_after;                  // int
  std::optional<std::string> trial_id;         // text
  std::optional<std::string> next_parent_spec; // relpath
  std::string doc;                             // json
  std::string file_sha256;                     // sha256
};

// wave_timing: the wave result's `timings` rows (a query extract of `doc`).
struct WaveTimingRow {
  std::string path;                              // relpath [key]
  i64 ord{};                                     // int [key]
  std::string phase;                             // text
  std::optional<std::string> run_dir;            // relpath
  std::optional<std::string> outcome;            // text
  std::optional<i64> exit_code;                  // int
  std::optional<f64> seconds;                    // real, volatile
  std::optional<i64> peak_mib;                   // int, volatile
  std::optional<std::string> executable_sha256;  // sha256
};

// spec_doc: a repository input (spec, template, wave manifest, library, recipe, registry,
// window, field registry).
struct SpecDocRow {
  std::string path;                  // relpath [key]
  std::string kind;                  // text, allowed {spec, template, wave-manifest, candidate,
                                     //   library, recipe, registry, window, field-registry, other}
  std::optional<std::string> schema; // text
  std::string file_sha256;           // sha256
  bool git_tracked{};                // bool: outside the git-ignored build trees (build*/)
  std::optional<std::string> parent; // text (a template's / wave's parent spec)
  std::string doc;                   // json
};

// pin: one SHA-256 a holder states for a target (RFC 6901 pointer into the holder).
struct PinRow {
  std::string holder_path;                // relpath [key]
  std::string pointer;                    // text [key]
  std::string pin_kind;                   // text, allowed (sql-design 3.7)
  std::optional<std::string> target_path; // text [ix]: the target's path_key; null = unresolved
  std::string target_sha256;              // sha256 [ix]
};

// trial_line (append-only): one non-blank line of a trial ledger, its exact bytes.
struct TrialLineRow {
  std::string ledger_path;                // relpath [key]
  i64 seq{};                              // int [key]: 1-based physical line number
  std::string line;                       // text (no line end)
  std::string line_sha256;                // sha256
  std::optional<std::string> schema;      // text
  std::optional<std::string> kind;        // text
  std::optional<std::string> cell;        // text
  std::optional<std::string> trial_id;    // text
  std::optional<i64> count;               // int
  std::optional<std::string> prev_sha256; // sha256
};

// ledger_state: a ledger's size and file digest; the head only from B2's chain-head function.
struct LedgerStateRow {
  std::string ledger_path;                // relpath [key]
  i64 lines{};                            // int: non-blank lines
  std::string file_sha256;                // sha256
  std::optional<std::string> head_sha256; // sha256 (B2's function through the seam; else NULL)
  std::optional<std::string> head_rule;   // text
};

// field_manifest: a fields manifest (`atx.research-role-fields/v1`).
struct FieldManifestRow {
  std::string path;                                 // relpath [key]
  std::string schema;                               // text
  std::optional<std::string> status;                // text
  std::optional<std::string> role_manifest_sha256;  // sha256
  std::optional<std::string> seal_exclusive_end;    // text
  std::optional<std::string> registry_sha256;       // sha256
  std::optional<std::string> engine_exe_sha256;     // sha256
  std::string file_sha256;                          // sha256
  std::string doc;                                  // json
};

// field_entry: one entry of a fields manifest's `fields`.
struct FieldEntryRow {
  std::string path;                           // relpath [key]: the manifest
  std::string name;                           // text [key]
  i64 ord{};                                  // int
  std::string file;                           // text
  std::optional<std::string> sha256;          // sha256
  std::optional<std::string> dtype;           // text
  std::optional<std::string> producer_key;    // sha256
  std::optional<std::string> formula_sha256;  // sha256
  std::optional<std::string> reused_from;     // text (compact JSON of the entry's reused_from)
};

// candidate: a wave candidate registration. Render: doc, rule py-indent2-noascii.
struct CandidateRow {
  std::string id;                         // text [key]
  std::string path;                       // relpath
  std::string status;                     // text
  std::optional<std::string> wave;        // text
  std::optional<std::string> dsl_sha256;  // sha256
  std::string file_sha256;                // sha256
  std::string doc;                        // json
};

// candidate_event: one entry of a candidate's `history`.
struct CandidateEventRow {
  std::string id;                   // text [key]
  i64 ord{};                        // int [key]
  std::string status;               // text
  std::string at;                   // text
  std::string by;                   // text
  std::optional<std::string> wave;  // text
  std::optional<std::string> note;  // text
};

// build_receipt: a research-build receipt (mega-<Tag>-receipt.json).
struct BuildReceiptRow {
  std::string tag;                   // text [key]
  std::string path;                  // relpath
  std::optional<std::string> preset; // text
  std::optional<std::string> source; // text
  std::optional<i64> exit_code;      // int
  std::optional<f64> wall_seconds;   // real, volatile
  std::string file_sha256;           // sha256
  std::string doc;                   // json
};

// build_exe: one executable a build receipt lists (exe SHA -> tag -> git SHA).
struct BuildExeRow {
  std::string tag;    // text [key]
  std::string target; // text [key]
  std::string sha256; // sha256 [ix]
};

} // namespace atx::engine::research::store
