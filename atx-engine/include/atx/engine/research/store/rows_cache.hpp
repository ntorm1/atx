#pragma once

// atx::engine::research::store -- plain row structs of the group `cache` (contract
// K-P9-13, sql-design section 3.7). Every cache index is <cache dir>/index.sqlite holding
// this group (plus the shared store_info table, rows_core.hpp). SQL1's record store
// (atx-engine/tools/record_store.py) writes `record`; SQL4 writes the IC and pair tables.

#include <optional>
#include <string>

#include "atx/core/types.hpp"

namespace atx::engine::research::store {

// record: one content-keyed record of tools/record_store.py. Exactly one of body /
// body_file is set (table CHECK); bodies over 1 MiB live in objects/<sha[0:2]>/<sha>.json
// beside the index, named by body_file.
struct RecordRow {
  std::string root;                     // text [key]: "" or the store root's directory name
  std::string kind;                     // text [key]
  std::string key_sha256;               // sha256 [key]
  std::string key;                      // json: the compact canonical key
  std::optional<std::string> body;      // json: compact, insertion order
  std::optional<std::string> body_file; // relpath (relative to the index directory)
  i64 body_bytes{};                     // int: UTF-8 bytes of the body text
  std::string content_sha256;           // sha256 (record_store.content_sha256)
};

// ic_signal: one IC signal sidecar (SQL4).
struct IcSignalRow {
  std::string key_sha256;     // sha256 [key]
  std::string schema;         // text
  std::string role_sha256;    // sha256
  std::string id;             // text
  std::string dsl_sha256;     // sha256
  std::string vm_identity;    // text
  std::string eval_mode;      // text
  std::string payload;        // relpath
  std::string payload_sha256; // sha256
  i64 payload_bytes{};        // int
  std::string sidecar;        // json
};

// ic_result: one IC-result record (SQL4).
struct IcResultRow {
  std::string entry_dir;     // relpath [key]
  std::string id;            // text [key]
  std::string key_sha256;    // sha256
  std::string record;        // json
  std::string record_sha256; // sha256
};

// pair_key: one marginal pair-cache key (SQL4).
struct PairKeyRow {
  std::string key_sha256; // sha256 [key]
  i64 version{};          // int
  std::string key_text;   // text
};

// pair_stat: one pair statistic (SQL4; the writer checks payload_lo <= payload_hi).
struct PairStatRow {
  std::string key_sha256; // sha256 [key]
  std::string payload_lo; // sha256 [key]
  std::string payload_hi; // sha256 [key]
  u64 sum_bits{};         // u64: the IEEE-754 bits of the sum
  i64 dates{};            // int
};

} // namespace atx::engine::research::store
