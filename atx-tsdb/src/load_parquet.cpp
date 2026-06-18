#include "atx/tsdb/load_parquet.hpp"

#include <algorithm>     // std::sort, std::unique
#include <filesystem>    // std::filesystem
#include <string>        // std::string
#include <string_view>   // std::string_view
#include <unordered_map> // std::unordered_map
#include <utility>       // std::pair
#include <vector>        // std::vector

#include "atx/core/datetime.hpp"   // time::Timestamp
#include "atx/core/error.hpp"      // Result, Err, Ok, ATX_TRY
#include "atx/core/io/parquet.hpp" // io::read_parquet, ParquetTable
#include "atx/core/types.hpp"      // i64, u32, u64, f64, usize

#include "atx/tsdb/builder.hpp" // SegmentBuilder

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

Status build_from_long(const LongColumns &cols, const std::string &path,
                       atx::i64 created_at_nanos) {
  const atx::usize rows = cols.times.size();
  if (cols.symbols.size() != rows) {
    return Err(ErrorCode::InvalidArgument, "times/symbols length mismatch");
  }
  for (const auto &v : cols.values) {
    if (v.size() != rows) {
      return Err(ErrorCode::InvalidArgument, "field-values length mismatch");
    }
  }
  if (cols.values.size() != cols.field_names.size()) {
    return Err(ErrorCode::InvalidArgument, "field_names/values count mismatch");
  }

  // --- time axis: unique sorted timestamps -> row index --------------------
  // Fast path: a single-date segment (the ORATS loader's per-date flush) has
  // every timestamp identical — skip the O(R log R) sort + the R hash inserts.
  std::vector<atx::i64> axis;
  std::unordered_map<atx::i64, atx::u64> time_to_row;
  const bool all_equal =
      rows > 0 && std::all_of(cols.times.begin(), cols.times.end(),
                              [&](atx::i64 t) { return t == cols.times.front(); });
  if (all_equal) {
    axis.push_back(cols.times.front());
    time_to_row.emplace(cols.times.front(), 0);
  } else {
    axis = cols.times;
    std::sort(axis.begin(), axis.end());
    axis.erase(std::unique(axis.begin(), axis.end()), axis.end());
    time_to_row.reserve(axis.size());
    for (atx::u64 i = 0; i < axis.size(); ++i) time_to_row.emplace(axis[i], i);
  }

  // --- symbols: first-seen interning order -> instrument index -------------
  // Reserve the upper bound (unique count <= row count) so push_back never
  // reallocates; the string_view keys below alias into `symbols`, so a realloc
  // would dangle them.
  std::vector<std::string> symbols;
  symbols.reserve(cols.symbols.size());
  std::unordered_map<std::string_view, atx::u32> sym_to_inst;
  for (const std::string &s : cols.symbols) {
    if (sym_to_inst.find(s) == sym_to_inst.end()) {
      const auto idx = static_cast<atx::u32>(symbols.size());
      symbols.push_back(s);
      sym_to_inst.emplace(symbols.back(), idx);
    }
  }

  SegmentBuilder builder(cols.field_names, symbols, axis);
  for (atx::usize r = 0; r < rows; ++r) {
    const atx::u64 t = time_to_row.at(cols.times[r]);
    const atx::u32 inst = sym_to_inst.at(cols.symbols[r]);
    for (atx::u32 fld = 0; fld < cols.field_names.size(); ++fld) {
      builder.set(fld, t, inst, cols.values[fld][r]);
    }
  }
  return builder.write(path, created_at_nanos);
}

Status load_parquet(const std::string &parquet_path, const std::string &out_path,
                    const std::string &time_col, const std::string &symbol_col,
                    const std::vector<std::string> &field_cols, atx::i64 created_at_nanos) {
  ATX_TRY(auto table, atx::core::io::read_parquet(parquet_path));

  LongColumns cols;
  cols.field_names = field_cols;

  // Timestamps: materialize via to_column (column_view rejects any unit
  // conversion; to_column normalizes the Arrow timestamp to time::Timestamp).
  ATX_TRY(auto ts_col, table.to_column<atx::core::time::Timestamp>(time_col));
  const auto ts = ts_col.view();
  cols.times.reserve(ts.size());
  for (const auto &t : ts) {
    cols.times.push_back(t.unix_nanos());
  }

  // Symbols: string column.
  ATX_TRY(auto syms, table.strings(symbol_col));
  cols.symbols.reserve(syms.size());
  for (const std::string_view s : syms) {
    cols.symbols.emplace_back(s);
  }

  // Field values: each numeric column as f64.
  cols.values.resize(field_cols.size());
  for (atx::usize f = 0; f < field_cols.size(); ++f) {
    ATX_TRY(auto col, table.column_view<atx::f64>(field_cols[f]));
    cols.values[f].assign(col.begin(), col.end());
  }

  return build_from_long(cols, out_path, created_at_nanos);
}

Status load_parquet_scaled(const std::string &parquet_path, const std::string &out_path,
                           const std::string &time_col, const std::string &symbol_col,
                           const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                           atx::i64 created_at_nanos) {
  ATX_TRY(auto table, atx::core::io::read_parquet(parquet_path));

  LongColumns cols;
  cols.field_names.reserve(field_scales.size());

  ATX_TRY(auto ts_col, table.to_column<atx::core::time::Timestamp>(time_col));
  const auto ts = ts_col.view();
  cols.times.reserve(ts.size());
  for (const auto &t : ts) {
    cols.times.push_back(t.unix_nanos());
  }

  ATX_TRY(auto syms, table.strings(symbol_col));
  cols.symbols.reserve(syms.size());
  for (const std::string_view s : syms) {
    cols.symbols.emplace_back(s);
  }

  cols.values.resize(field_scales.size());
  for (atx::usize f = 0; f < field_scales.size(); ++f) {
    cols.field_names.push_back(field_scales[f].first);
    ATX_TRY(auto col, table.column_view<atx::i64>(field_scales[f].first));
    const atx::f64 scale = field_scales[f].second;
    auto &dst = cols.values[f];
    dst.reserve(col.size());
    for (const atx::i64 v : col) {
      dst.push_back(static_cast<atx::f64>(v) * scale);
    }
  }

  return build_from_long(cols, out_path, created_at_nanos);
}

Status build_dated_segments(const std::string &hive_root, const std::string &seg_dir,
                            const std::string &time_col, const std::string &symbol_col,
                            const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                            atx::i64 created_at_nanos) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(fs::path{hive_root}, ec)) {
    return Err(ErrorCode::IoError, "hive_root does not exist");
  }
  // Collect date partitions: subdirs named "date=YYYY-MM-DD".
  fs::directory_iterator it{fs::path{hive_root}, ec};
  if (ec) {
    return Err(ErrorCode::IoError, "cannot iterate hive_root");
  }
  std::vector<std::string> dates;
  static constexpr std::string_view prefix = "date=";
  for (const auto &entry : it) {
    // A stat failure on an entry is treated as "skip non-dir".
    if (!entry.is_directory(ec)) {
      continue;
    }
    const std::string dir = entry.path().filename().string();
    if (dir.rfind(prefix, 0) == 0) {
      dates.push_back(dir.substr(prefix.size()));
    }
  }
  if (dates.empty()) {
    return Err(ErrorCode::InvalidArgument, "no date= partitions under hive_root");
  }
  std::sort(dates.begin(), dates.end()); // ISO dates sort chronologically

  fs::create_directories(fs::path{seg_dir}, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "cannot create seg_dir");
  }
  for (const std::string &date : dates) {
    const std::string parquet = (fs::path{hive_root} / ("date=" + date) / "data.parquet").string();
    const std::string out = (fs::path{seg_dir} / (date + ".seg")).string();
    ATX_TRY_VOID(
        load_parquet_scaled(parquet, out, time_col, symbol_col, field_scales, created_at_nanos));
  }
  return Ok();
}

} // namespace atx::tsdb
