#pragma once

// atx::engine::research::store::catalog -- read-only queries over a catalog (P9 SQL2; sql-design
// section 3.9: `verify --pins`, `query`, `dump`). Every query orders its rows by the table's key
// (or by holder / pointer for pins); `query` prints typed columns only (no JSON document
// column), so its output is blind-safe by construction (sql-design 3.5).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::store::catalog {

enum class QueryFormat : u8 { Text, Json };

// The names `query` accepts: artifacts, pins, stale-pins, runs, timings, producers.
[[nodiscard]] std::vector<std::string> query_names();

// The printed lines of one named query: Text = a tab-separated header line, then one line per
// row (NULL printed as "NULL"); Json = one compact JSON object per row (no header).
// Err(InvalidArgument) for an unknown name.
[[nodiscard]] core::Result<std::vector<std::string>> run_query(core::db::Database &db,
                                                               std::string_view name,
                                                               QueryFormat format);

// Every row of `table` (a table of the catalog or cache groups) as one compact JSON object per
// line, in key order (`dump --table T`, for review diffs). Err(InvalidArgument) for a table no
// group declares.
[[nodiscard]] core::Result<std::vector<std::string>> dump_table(core::db::Database &db,
                                                                std::string_view table);

struct PinCount {
  std::string pin_kind;
  std::string state; // ok | declared | stale | missing | unresolved
  i64 count{};
};

struct PinProblem {
  std::string holder_path;
  std::string pointer;
  std::string pin_kind;
  std::string state; // stale | missing
  std::optional<std::string> target_path;
  std::string target_sha256;
  std::optional<std::string> artifact_sha256;
};

struct ShaSourceCount {
  std::string sha_source; // verified | declared
  i64 count{};
};

// pin_status counts by kind and state, ordered (kind, state); holder = one holder path only.
[[nodiscard]] core::Result<std::vector<PinCount>>
pin_counts(core::db::Database &db, const std::optional<std::string> &holder);

// The stale and missing pins, ordered by holder and pointer.
[[nodiscard]] core::Result<std::vector<PinProblem>>
pin_problems(core::db::Database &db, const std::optional<std::string> &holder);

// artifact rows by sha_source (verified and declared counted apart).
[[nodiscard]] core::Result<std::vector<ShaSourceCount>> artifact_counts(core::db::Database &db);

} // namespace atx::engine::research::store::catalog
