#pragma once

// atx::tsdb parquet loader — pivot a LONG-format table (one row per
// (timestamp, symbol), one numeric column per field) into a sealed dense grid.
//
// The pivot core (build_from_long) takes already-extracted columns so it is unit
// testable without an Arrow fixture. load_parquet() is the thin file entry point
// that pulls those columns out of a parquet via atx-core io::read_parquet.

#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::tsdb {

/// Extracted long-format columns. Row r contributes (times[r], symbols[r],
/// values[field][r]) for each field. All inner vectors share the row count.
struct LongColumns {
  std::vector<std::string> field_names;      // F field names
  std::vector<atx::i64> times;               // R row timestamps (unix nanos)
  std::vector<std::string> symbols;          // R row symbols
  std::vector<std::vector<atx::f64>> values; // [F][R] values
};

/// Pivot `cols` into a dense grid (dedup+sort time axis, intern symbols in
/// first-seen order) and write a sealed segment to `path`. Err(InvalidArgument)
/// on ragged column lengths.
[[nodiscard]] atx::core::Status build_from_long(const LongColumns &cols, const std::string &path,
                                                atx::i64 created_at_nanos);

/// Read `parquet_path`, project (time_col, symbol_col, field_cols) into
/// LongColumns, and build a sealed segment at `out_path`.
[[nodiscard]] atx::core::Status
load_parquet(const std::string &parquet_path, const std::string &out_path,
             const std::string &time_col, const std::string &symbol_col,
             const std::vector<std::string> &field_cols, atx::i64 created_at_nanos);

/// Read `parquet_path` whose field columns are int64 fixed-point; for each
/// (name, scale) in `field_scales`, project that column as f64 (value * scale) and
/// build a sealed segment at `out_path`. `time_col` is a timestamp column,
/// `symbol_col` a string column.
[[nodiscard]] atx::core::Status
load_parquet_scaled(const std::string &parquet_path, const std::string &out_path,
                    const std::string &time_col, const std::string &symbol_col,
                    const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                    atx::i64 created_at_nanos);

/// For each `<hive_root>/date=YYYY-MM-DD/data.parquet`, build a sealed segment at
/// `<seg_dir>/YYYY-MM-DD.seg` via load_parquet_scaled. Creates `seg_dir`. Dates are
/// processed in ascending order. Err if `hive_root` has no `date=` partitions.
[[nodiscard]] atx::core::Status
build_dated_segments(const std::string &hive_root, const std::string &seg_dir,
                     const std::string &time_col, const std::string &symbol_col,
                     const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                     atx::i64 created_at_nanos);

} // namespace atx::tsdb
