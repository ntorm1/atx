#pragma once

// atx::engine::research::store -- plain row structs of the group `catalog_core`
// (contract K-P9-13, sql-design section 3.7). No templates, no SQLite: consumers include
// this, ops_core.hpp and store.hpp only. std::optional members are the nullable columns;
// every other member is NOT NULL. Comments name the column kind and flags
// (key = primary-key member, ix = indexed, volatile = stored but never digested).
//
// store_info is the per-store self description written by open_store (db_kind,
// schema_json; SQL2 adds created_by). Every store has it, the cache index included, so its
// descriptor is shared by both groups and its operations are declared once (ops_core.hpp).

#include <optional>
#include <string>

#include "atx/core/types.hpp"

namespace atx::engine::research::store {

// store_info (volatile table): key text [key], value text.
struct StoreInfoRow {
  std::string key;
  std::string value;
};

// catalog_run (volatile table): one row per catalog run.
struct CatalogRunRow {
  std::string catalog_run_id;                  // text [key]
  std::optional<std::string> git_sha;          // text
  std::optional<std::string> store_exe_sha256; // sha256
  std::string roots;                           // json
  std::string started_utc;                     // text
  f64 seconds{};                               // real
  i64 files_seen{};                            // int
  i64 files_verified{};                        // int
  i64 files_declared{};                        // int
  i64 files_skipped{};                         // int
  std::optional<std::string> catalog_digest;   // sha256
};

// artifact: the current tree state, one row per catalogued file.
struct ArtifactRow {
  std::string path_key;                    // text [key]: case-folded, '/'-separated
  std::string path;                        // relpath
  std::string sha256;                      // sha256 [ix]
  std::optional<i64> bytes;                // int
  std::string artifact_class;              // text [ix], column "class"
  std::optional<std::string> json_schema;  // text
  std::string sha_source;                  // text, allowed {verified, declared}
  std::optional<std::string> declared_by;  // relpath
  std::optional<std::string> eol;          // text, allowed {lf, crlf, mixed, none}
  std::optional<std::string> producer_key; // sha256
};

// artifact_seen (volatile, append-only): history across catalog runs.
struct ArtifactSeenRow {
  std::string path_key;       // text [key]
  std::string sha256;         // sha256 [key]
  std::string catalog_run_id; // text [key]
};

// skipped_path (volatile): paths a catalog run listed but did not open.
struct SkippedPathRow {
  std::string catalog_run_id; // text [key]
  std::string path;           // text [key]
  std::string reason;         // text, allowed {seal-name, outside-roots, declared-only,
                              //                unreadable, unparsed}
};

// producer: K-P9-3 / ruling P6 producer identities.
struct ProducerRow {
  std::string producer_key;                  // sha256 [key]: record digest of the other columns
  std::string kind;                          // text, allowed {engine, python, powershell, human,
                                             //                unknown}
  std::optional<std::string> exe_sha256;     // sha256
  std::optional<std::string> git_sha;        // text
  std::optional<std::string> build_type;     // text
  std::optional<std::string> module_name;    // text, column "module"
  std::optional<std::string> code_sha256;    // sha256
  std::optional<std::string> receipt_sha256; // sha256
};

} // namespace atx::engine::research::store
