#include "atx/vol/research/track_store.hpp"

// Third-party Arrow/Parquet headers. These arrive as IMPORTED CONFIG targets
// (find_package(Arrow/Parquet CONFIG REQUIRED), atx-vol/CMakeLists.txt) whose
// INTERFACE_INCLUDE_DIRECTORIES CMake already treats as SYSTEM includes by
// default -- the same arrangement atx-core/src/io/parquet.cpp relies on to
// stay /W4 /WX-clean while including these headers directly, unwrapped. No
// pragma suppression needed here for the same reason; verified locally (see
// the D2 report) rather than assumed.
#include <arrow/array/data.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/array/util.h>
#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/ipc/feather.h>
#include <arrow/memory_pool.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/type_fwd.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <parquet/types.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"                    // time::days_from_civil, is_valid_date
#include "atx/vol/detail/archive_util.hpp"           // reserve_unique_publish_temp_file, flush_and_publish_file
#include "atx/vol/detail/backtest_series_columns.hpp" // kBacktestSeriesColumns

namespace atx::vol {
using atx::core::Ok; // Err resolves via ADL (its ErrorCode/Error argument is atx::core::*);
                     // Ok(...)'s argument types (shared_ptr<arrow::Table>, StagedTrack, ...)
                     // are not, so this needs the explicit using -- same convention as
                     // backtest_db.cpp.

namespace {

namespace fs = std::filesystem;

// ── Arrow status -> atx Error ───────────────────────────────────────────────

[[nodiscard]] Error from_arrow(const arrow::Status &s, std::string_view ctx) {
  std::string msg{ctx};
  msg += ": ";
  msg += s.ToString();
  if (s.IsIOError()) {
    return Error{ErrorCode::IoError, std::move(msg)};
  }
  if (s.IsInvalid()) {
    return Error{ErrorCode::ParseError, std::move(msg)};
  }
  if (s.IsNotImplemented()) {
    return Error{ErrorCode::NotImplemented, std::move(msg)};
  }
  return Error{ErrorCode::Internal, std::move(msg)};
}

// ── TrackMeta / BacktestResult validation ───────────────────────────────────

[[nodiscard]] bool is_hive_safe(std::string_view s) noexcept {
  if (s.empty()) {
    return false;
  }
  for (const char c : s) {
    switch (c) {
    case '/': case '\\': case ':': case '*': case '?': case '"':
    case '<': case '>': case '|': case '=': case '\0':
      return false;
    default:
      break;
    }
  }
  return true;
}

[[nodiscard]] Status validate_track_meta(const TrackMeta &meta) {
  if (!is_hive_safe(meta.underlier)) {
    return Err(ErrorCode::InvalidArgument,
               "write_staging: TrackMeta.underlier is empty or contains a path/hive-unsafe character");
  }
  if (!is_hive_safe(meta.family)) {
    return Err(ErrorCode::InvalidArgument,
               "write_staging: TrackMeta.family is empty or contains a path/hive-unsafe character");
  }
  return Ok();
}

// Mirrors backtest_db.cpp's `validate_result_shape` + `validate_series_data`'s
// stricter step_pnl_total rule ("stored history must include one inception
// row") -- TrackStore is, like BacktestDb, a persistence layer, so it inherits
// the same storable-shape contract. Not a call into that function (it is a
// private helper of a different translation unit); re-derived here against the
// same `BacktestResult` invariants documented in backtest.hpp.
[[nodiscard]] Status validate_backtest_result_shape(const BacktestResult &result) {
  const std::size_t rows = result.size();
  if (rows == 0) {
    return Err(ErrorCode::InvalidArgument, "write_staging: empty backtest result (0 rows)");
  }
  if (result.ts_ns.size() != rows) {
    return Err(ErrorCode::InvalidArgument, "write_staging: ts_ns row count mismatch");
  }
  for (const BacktestSeriesColumn &column : backtest_series_columns()) {
    if ((result.*(column.member)).size() != rows) {
      return Err(ErrorCode::InvalidArgument,
                 "write_staging: mandatory series column row count mismatch");
    }
  }
  if ((!result.swap_pv.empty() && result.swap_pv.size() != rows) ||
      (!result.swap_pnl.empty() && result.swap_pnl.size() != rows) ||
      (!result.gross_vega_abs.empty() && result.gross_vega_abs.size() != rows) ||
      (!result.nav_liquidation.empty() && result.nav_liquidation.size() != rows)) {
    return Err(ErrorCode::InvalidArgument, "write_staging: swap-lane column row count mismatch");
  }
  if (!result.step_pnl_total.empty() && result.step_pnl_total.size() != rows - 1) {
    return Err(ErrorCode::InvalidArgument,
               "write_staging: step_pnl_total must be empty or exactly rows-1 (one inception row)");
  }
  for (std::size_t i = 0; i < rows; ++i) {
    if (result.date[i].size() != 10) {
      return Err(ErrorCode::InvalidArgument, "write_staging: date must be a 10-char YYYY-MM-DD string");
    }
    if (i != 0 && (result.date[i - 1] >= result.date[i] || result.ts_ns[i - 1] >= result.ts_ns[i])) {
      return Err(ErrorCode::InvalidArgument,
                 "write_staging: rows are not strictly ordered by (date, ts_ns) -- compact()'s "
                 "(track_key, date) sort relies on each staged track already being chronological");
    }
  }
  return Ok();
}

// ── ISO "YYYY-MM-DD" -> date32 (days since 1970-01-01) ──────────────────────

[[nodiscard]] std::optional<std::int32_t> parse_iso_date_days(std::string_view s) noexcept {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return std::nullopt;
  }
  const auto digit = [](char c) -> int { return (c >= '0' && c <= '9') ? (c - '0') : -1; };
  int y = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const int d = digit(s[i]);
    if (d < 0) {
      return std::nullopt;
    }
    y = y * 10 + d;
  }
  int m = 0;
  for (std::size_t i = 5; i < 7; ++i) {
    const int d = digit(s[i]);
    if (d < 0) {
      return std::nullopt;
    }
    m = m * 10 + d;
  }
  int day = 0;
  for (std::size_t i = 8; i < 10; ++i) {
    const int d = digit(s[i]);
    if (d < 0) {
      return std::nullopt;
    }
    day = day * 10 + d;
  }
  if (!atx::core::time::is_valid_date(y, static_cast<std::uint32_t>(m), static_cast<std::uint32_t>(day))) {
    return std::nullopt;
  }
  const std::int64_t days =
      atx::core::time::days_from_civil(y, static_cast<std::uint32_t>(m), static_cast<std::uint32_t>(day));
  if (days < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) ||
      days > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>(days);
}

// ── Arrow table construction ─────────────────────────────────────────────────

// Wraps `values` as a non-nullable, zero-copy Arrow array. `values` must
// outlive every use of the returned array -- callers only ever pass a member
// of the `BacktestResult` the caller holds for the whole synchronous
// write_staging() call, so this is safe.
template <class Native>
[[nodiscard]] std::shared_ptr<arrow::Array>
wrap_zero_copy(const std::shared_ptr<arrow::DataType> &type, const std::vector<Native> &values) {
  auto data = arrow::ArrayData::Make(type, static_cast<std::int64_t>(values.size()),
                                     {nullptr, arrow::Buffer::Wrap(values)},
                                     /*null_count=*/0);
  return arrow::MakeArray(data);
}

// Builds the 5 nullable swap-lane columns. `src` empty -> every row NULL.
// `src` size == rows -> stored verbatim. Any other length is a caller bug
// that validate_backtest_result_shape() must already have rejected.
[[nodiscard]] Result<std::shared_ptr<arrow::Array>>
build_row_parallel_nullable_column(const std::vector<double> &src, std::int64_t rows) {
  arrow::DoubleBuilder builder;
  arrow::Status st = builder.Reserve(rows);
  if (!st.ok()) {
    return Err(from_arrow(st, "build_track_table: reserve"));
  }
  if (src.empty()) {
    for (std::int64_t i = 0; i < rows; ++i) {
      st = builder.AppendNull();
      if (!st.ok()) {
        return Err(from_arrow(st, "build_track_table: append null"));
      }
    }
  } else {
    for (const double v : src) {
      st = builder.Append(v);
      if (!st.ok()) {
        return Err(from_arrow(st, "build_track_table: append"));
      }
    }
  }
  std::shared_ptr<arrow::DoubleArray> array;
  st = builder.Finish(&array);
  if (!st.ok()) {
    return Err(from_arrow(st, "build_track_table: finish"));
  }
  return Ok(std::static_pointer_cast<arrow::Array>(array));
}

// step_pnl_total: row 0 is always NULL (no step precedes the inception row).
// `src` empty -> every row NULL. `src.size() == rows - 1` (already validated)
// -> row i>=1 gets src[i-1].
[[nodiscard]] Result<std::shared_ptr<arrow::Array>>
build_step_pnl_total_column(const std::vector<double> &src, std::int64_t rows) {
  arrow::DoubleBuilder builder;
  arrow::Status st = builder.Reserve(rows);
  if (!st.ok()) {
    return Err(from_arrow(st, "build_track_table: reserve step_pnl_total"));
  }
  st = builder.AppendNull(); // row 0: inception, no preceding step
  if (!st.ok()) {
    return Err(from_arrow(st, "build_track_table: append null step_pnl_total[0]"));
  }
  if (src.empty()) {
    for (std::int64_t i = 1; i < rows; ++i) {
      st = builder.AppendNull();
      if (!st.ok()) {
        return Err(from_arrow(st, "build_track_table: append null step_pnl_total"));
      }
    }
  } else {
    for (const double v : src) {
      st = builder.Append(v);
      if (!st.ok()) {
        return Err(from_arrow(st, "build_track_table: append step_pnl_total"));
      }
    }
  }
  std::shared_ptr<arrow::DoubleArray> array;
  st = builder.Finish(&array);
  if (!st.ok()) {
    return Err(from_arrow(st, "build_track_table: finish step_pnl_total"));
  }
  return Ok(std::static_pointer_cast<arrow::Array>(array));
}

// Schema v1 field list, in the exact order documented in track_store.hpp.
[[nodiscard]] arrow::FieldVector schema_v1_fields() {
  arrow::FieldVector fields;
  fields.reserve(3 + std::size(kBacktestSeriesColumns) + 5);
  fields.push_back(arrow::field("track_key", arrow::utf8(), /*nullable=*/false));
  fields.push_back(arrow::field("date", arrow::date32(), /*nullable=*/false));
  fields.push_back(arrow::field("ts_ns", arrow::int64(), /*nullable=*/false));
  for (const BacktestSeriesColumn &column : backtest_series_columns()) {
    fields.push_back(arrow::field(std::string(column.name), arrow::float64(), /*nullable=*/false));
  }
  fields.push_back(arrow::field("swap_pv", arrow::float64(), /*nullable=*/true));
  fields.push_back(arrow::field("swap_pnl", arrow::float64(), /*nullable=*/true));
  fields.push_back(arrow::field("gross_vega_abs", arrow::float64(), /*nullable=*/true));
  fields.push_back(arrow::field("nav_liquidation", arrow::float64(), /*nullable=*/true));
  fields.push_back(arrow::field("step_pnl_total", arrow::float64(), /*nullable=*/true));
  return fields;
}

// Builds the schema-v1 arrow::Table for one track. `key`/`result` must already
// have passed validate_backtest_result_shape(). The 25 frozen series columns +
// ts_ns are zero-copy views into `result`'s own SoA vectors (safe: the table
// is written out and destroyed entirely within the caller's synchronous
// write_staging() call, so `result` outlives every use of it).
[[nodiscard]] Result<std::shared_ptr<arrow::Table>>
build_track_table(const TrackKey &key, const BacktestResult &result) {
  const auto rows = static_cast<std::int64_t>(result.size());

  arrow::StringBuilder key_builder;
  arrow::Status st = key_builder.Reserve(rows);
  if (!st.ok()) {
    return Err(from_arrow(st, "build_track_table: reserve track_key"));
  }
  const std::string hex = key.hex();
  for (std::int64_t i = 0; i < rows; ++i) {
    st = key_builder.Append(hex);
    if (!st.ok()) {
      return Err(from_arrow(st, "build_track_table: append track_key"));
    }
  }
  std::shared_ptr<arrow::StringArray> key_array;
  st = key_builder.Finish(&key_array);
  if (!st.ok()) {
    return Err(from_arrow(st, "build_track_table: finish track_key"));
  }

  std::vector<std::int32_t> days;
  days.reserve(static_cast<std::size_t>(rows));
  for (std::int64_t i = 0; i < rows; ++i) {
    const std::optional<std::int32_t> parsed = parse_iso_date_days(result.date[static_cast<std::size_t>(i)]);
    if (!parsed.has_value()) {
      return Err(ErrorCode::InvalidArgument,
                 "build_track_table: malformed date '" + result.date[static_cast<std::size_t>(i)] + "'");
    }
    days.push_back(*parsed);
  }
  // FromVector (not Wrap): `days` is a local about to go out of scope, so the
  // Array must OWN the buffer -- FromVector's shared_ptr deleter keeps the
  // moved-in vector alive for the buffer's lifetime.
  auto date_array = arrow::MakeArray(arrow::ArrayData::Make(
      arrow::date32(), rows, {nullptr, arrow::Buffer::FromVector(std::move(days))}, /*null_count=*/0));

  auto ts_ns_array = wrap_zero_copy(arrow::int64(), result.ts_ns);

  std::vector<std::shared_ptr<arrow::Array>> arrays;
  arrays.reserve(3 + std::size(kBacktestSeriesColumns) + 5);
  arrays.push_back(std::move(key_array));
  arrays.push_back(std::move(date_array));
  arrays.push_back(std::move(ts_ns_array));
  for (const BacktestSeriesColumn &column : backtest_series_columns()) {
    arrays.push_back(wrap_zero_copy(arrow::float64(), result.*(column.member)));
  }

  Result<std::shared_ptr<arrow::Array>> swap_pv = build_row_parallel_nullable_column(result.swap_pv, rows);
  if (!swap_pv.has_value()) {
    return Err(swap_pv.error());
  }
  Result<std::shared_ptr<arrow::Array>> swap_pnl = build_row_parallel_nullable_column(result.swap_pnl, rows);
  if (!swap_pnl.has_value()) {
    return Err(swap_pnl.error());
  }
  Result<std::shared_ptr<arrow::Array>> gross_vega_abs =
      build_row_parallel_nullable_column(result.gross_vega_abs, rows);
  if (!gross_vega_abs.has_value()) {
    return Err(gross_vega_abs.error());
  }
  Result<std::shared_ptr<arrow::Array>> nav_liquidation =
      build_row_parallel_nullable_column(result.nav_liquidation, rows);
  if (!nav_liquidation.has_value()) {
    return Err(nav_liquidation.error());
  }
  Result<std::shared_ptr<arrow::Array>> step_pnl_total =
      build_step_pnl_total_column(result.step_pnl_total, rows);
  if (!step_pnl_total.has_value()) {
    return Err(step_pnl_total.error());
  }
  arrays.push_back(*std::move(swap_pv));
  arrays.push_back(*std::move(swap_pnl));
  arrays.push_back(*std::move(gross_vega_abs));
  arrays.push_back(*std::move(nav_liquidation));
  arrays.push_back(*std::move(step_pnl_total));

  auto schema = arrow::schema(schema_v1_fields());
  return Ok(arrow::Table::Make(std::move(schema), arrays, rows));
}

// ── compact() internals ──────────────────────────────────────────────────────

struct StagedTrack {
  std::string path;
  std::string underlier;
  std::string family;
  std::string track_key_hex;
  std::shared_ptr<arrow::Table> table;
};

[[nodiscard]] Result<StagedTrack> open_staged_track(std::string path) {
  auto in_res = arrow::io::ReadableFile::Open(path);
  if (!in_res.ok()) {
    return Err(from_arrow(in_res.status(), "compact: open staged file " + path));
  }
  auto reader_res = arrow::ipc::feather::Reader::Open(*in_res);
  if (!reader_res.ok()) {
    return Err(from_arrow(reader_res.status(), "compact: open feather reader " + path));
  }
  std::shared_ptr<arrow::ipc::feather::Reader> reader = *reader_res;
  std::shared_ptr<arrow::Table> table;
  const arrow::Status read_st = reader->Read(&table);
  if (!read_st.ok()) {
    return Err(from_arrow(read_st, "compact: read feather table " + path));
  }
  const std::shared_ptr<const arrow::KeyValueMetadata> meta = table->schema()->metadata();
  if (!meta) {
    return Err(ErrorCode::Internal, "compact: staged file missing schema metadata: " + path);
  }
  const arrow::Result<std::string> underlier = meta->Get("underlier");
  const arrow::Result<std::string> family = meta->Get("family");
  const arrow::Result<std::string> track_key_hex = meta->Get("track_key");
  if (!underlier.ok() || !family.ok() || !track_key_hex.ok()) {
    return Err(ErrorCode::Internal,
               "compact: staged file missing underlier/family/track_key metadata: " + path);
  }
  return Ok(StagedTrack{std::move(path), *underlier, *family, *track_key_hex, std::move(table)});
}

[[nodiscard]] std::int64_t table_nbytes(const arrow::Table &table) noexcept {
  std::int64_t total = 0;
  for (int c = 0; c < table.num_columns(); ++c) {
    for (const std::shared_ptr<arrow::Array> &chunk : table.column(c)->chunks()) {
      for (const std::shared_ptr<arrow::Buffer> &buf : chunk->data()->buffers) {
        if (buf) {
          total += buf->size();
        }
      }
    }
  }
  return total;
}

[[nodiscard]] std::string format_batch_name(std::uint64_t idx) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof buf, "batch-%06llu.parquet", static_cast<unsigned long long>(idx));
  return std::string(buf, buf + (n > 0 ? n : 0));
}

// Highest existing "batch-NNNNNN.parquet" index in `dst_dir`, plus one (0 if
// the directory is absent or empty) -- so repeated compact() calls over the
// same hive partition are additive, never overwriting a prior batch.
[[nodiscard]] std::uint64_t next_batch_index(const fs::path &dst_dir) {
  std::error_code ec;
  if (!fs::exists(dst_dir, ec)) {
    return 0;
  }
  constexpr std::string_view kPrefix = "batch-";
  constexpr std::string_view kSuffix = ".parquet";
  bool any = false;
  std::uint64_t max_seen = 0;
  fs::directory_iterator it(dst_dir, ec);
  if (ec) {
    return 0;
  }
  const fs::directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    const std::string name = it->path().filename().string();
    if (name.size() != kPrefix.size() + 6 + kSuffix.size() || name.rfind(kPrefix, 0) != 0 ||
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
      continue;
    }
    const std::string digits = name.substr(kPrefix.size(), 6);
    const bool all_digit =
        std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!all_digit) {
      continue;
    }
    const std::uint64_t idx = std::stoull(digits);
    if (!any || idx > max_seen) {
      max_seen = idx;
      any = true;
    }
  }
  return any ? max_seen + 1 : 0;
}

[[nodiscard]] Result<std::shared_ptr<arrow::Table>>
concat_tables(const std::vector<std::shared_ptr<arrow::Table>> &tables) {
  auto res = arrow::ConcatenateTables(tables);
  if (!res.ok()) {
    return Err(from_arrow(res.status(), "compact: concatenate tables"));
  }
  return Ok(*res);
}

// zstd-compresses `table` into ONE row group at `dst_path`, atomically
// published. `table`'s rows must already be sorted by (track_key, date).
[[nodiscard]] Status write_parquet_batch(const arrow::Table &table, const std::string &dst_path) {
  const int track_key_idx = table.schema()->GetFieldIndex("track_key");
  const int date_idx = table.schema()->GetFieldIndex("date");
  if (track_key_idx < 0 || date_idx < 0) {
    return Err(ErrorCode::Internal, "compact: batch schema missing track_key/date");
  }

  const Result<std::string> tmp_path = detail::reserve_unique_publish_temp_file(dst_path);
  if (!tmp_path.has_value()) {
    return Err(tmp_path.error());
  }

  // ONE row group per batch file, regardless of row count: WriterProperties'
  // OWN max_row_group_length defaults to 1Mi rows (DEFAULT_MAX_ROW_GROUP_LENGTH,
  // parquet/properties.h) independently of the `chunk_size` passed to
  // WriteTable() below, so both must agree on "the whole table is one chunk"
  // or a batch approaching the 256-512 MB / many-rows target could silently
  // split into more than one row group past that default.
  const std::int64_t whole_table_rows = table.num_rows() > 0 ? table.num_rows() : 1;
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::ZSTD);
  builder.max_row_group_length(whole_table_rows);
  builder.set_sorting_columns({
      parquet::SortingColumn{track_key_idx, /*descending=*/false, /*nulls_first=*/false},
      parquet::SortingColumn{date_idx, /*descending=*/false, /*nulls_first=*/false},
  });
  const std::shared_ptr<parquet::WriterProperties> props = builder.build();

  auto out_res = arrow::io::FileOutputStream::Open(*tmp_path);
  if (!out_res.ok()) {
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(out_res.status(), "compact: open temp batch file"));
  }
  const std::shared_ptr<arrow::io::FileOutputStream> out = *out_res;

  // chunk_size == the whole table too (belt-and-suspenders with
  // max_row_group_length above).
  const arrow::Status write_st =
      parquet::arrow::WriteTable(table, arrow::default_memory_pool(), out, whole_table_rows, props);
  if (!write_st.ok()) {
    [[maybe_unused]] const arrow::Status ignored_close = out->Close();
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(write_st, "compact: parquet write failed"));
  }
  const arrow::Status close_st = out->Close();
  if (!close_st.ok()) {
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(close_st, "compact: cannot close temp batch file"));
  }

  return detail::flush_and_publish_file(*tmp_path, dst_path);
}

// Batches ~256-512 MB per file: accumulate tracks (already sorted by
// track_key) until the running estimated byte size crosses the lower target,
// then flush. The final, possibly-under-target remainder for a group is
// always flushed too (a group is never silently dropped for being small).
constexpr std::int64_t kTargetBatchBytes = 256LL * 1024 * 1024;

} // namespace

Status TrackStore::write_staging(const TrackKey &key, const BacktestResult &result, const TrackMeta &meta) {
  Status meta_status = validate_track_meta(meta);
  if (!meta_status.has_value()) {
    return meta_status;
  }
  Status shape_status = validate_backtest_result_shape(result);
  if (!shape_status.has_value()) {
    return shape_status;
  }

  Result<std::shared_ptr<arrow::Table>> built = build_track_table(key, result);
  if (!built.has_value()) {
    return Err(built.error());
  }
  std::shared_ptr<arrow::Table> table = *std::move(built);

  // Attach underlier/family/track_key as schema-level metadata so compact()
  // can read the hive placement back out of the file without parsing (or
  // trusting) the staging filename.
  const std::string hex = key.hex();
  auto kv = arrow::KeyValueMetadata::Make({"underlier", "family", "track_key"},
                                          {meta.underlier, meta.family, hex});
  table = table->ReplaceSchemaMetadata(kv);

  const fs::path staging_dir = fs::path(lake_root_) / "staging";
  std::error_code mkdir_ec;
  fs::create_directories(staging_dir, mkdir_ec);
  if (mkdir_ec) {
    return Err(ErrorCode::IoError, "write_staging: cannot create staging directory: " + mkdir_ec.message());
  }
  const std::string dst_path = (staging_dir / (hex + ".feather")).string();

  const Result<std::string> tmp_path = detail::reserve_unique_publish_temp_file(dst_path);
  if (!tmp_path.has_value()) {
    return Err(tmp_path.error());
  }

  auto out_res = arrow::io::FileOutputStream::Open(*tmp_path);
  if (!out_res.ok()) {
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(out_res.status(), "write_staging: cannot open temp for write"));
  }
  const std::shared_ptr<arrow::io::FileOutputStream> out = *out_res;

  const arrow::Status write_st =
      arrow::ipc::feather::WriteTable(*table, out.get(), arrow::ipc::feather::WriteProperties::Defaults());
  if (!write_st.ok()) {
    [[maybe_unused]] const arrow::Status ignored_close = out->Close();
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(write_st, "write_staging: feather write failed"));
  }
  const arrow::Status close_st = out->Close();
  if (!close_st.ok()) {
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(close_st, "write_staging: cannot close temp"));
  }

  return detail::flush_and_publish_file(*tmp_path, dst_path);
}

Result<CompactStats> compact(std::string_view lake_root) {
  CompactStats stats{};
  const fs::path root{std::string(lake_root)};
  const fs::path staging_dir = root / "staging";

  std::error_code exists_ec;
  if (!fs::exists(staging_dir, exists_ec)) {
    return Ok(stats);
  }

  std::vector<StagedTrack> staged;
  std::error_code list_ec;
  fs::directory_iterator it(staging_dir, list_ec);
  if (list_ec) {
    return Err(ErrorCode::IoError, "compact: cannot list staging directory: " + list_ec.message());
  }
  const fs::directory_iterator dir_end;
  for (; it != dir_end; it.increment(list_ec)) {
    if (list_ec) {
      return Err(ErrorCode::IoError, "compact: staging directory iteration failed: " + list_ec.message());
    }
    const fs::directory_entry entry = *it;
    std::error_code type_ec;
    if (!entry.is_regular_file(type_ec) || entry.path().extension() != ".feather") {
      continue;
    }
    Result<StagedTrack> opened = open_staged_track(entry.path().string());
    if (!opened.has_value()) {
      return Err(opened.error());
    }
    staged.push_back(*std::move(opened));
  }

  if (staged.empty()) {
    return Ok(stats);
  }

  std::map<std::pair<std::string, std::string>, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < staged.size(); ++i) {
    groups[{staged[i].underlier, staged[i].family}].push_back(i);
  }

  for (auto &group : groups) {
    const std::string &underlier = group.first.first;
    const std::string &family = group.first.second;
    std::vector<std::size_t> &idxs = group.second;

    std::sort(idxs.begin(), idxs.end(), [&staged](std::size_t a, std::size_t b) {
      return staged[a].track_key_hex < staged[b].track_key_hex;
    });

    const fs::path dst_dir = root / "tracks" / ("underlier=" + underlier) / ("family=" + family);
    std::error_code mkdir_ec;
    fs::create_directories(dst_dir, mkdir_ec);
    if (mkdir_ec) {
      return Err(ErrorCode::IoError, "compact: cannot create hive partition dir: " + mkdir_ec.message());
    }

    std::uint64_t next_batch = next_batch_index(dst_dir);
    std::vector<std::shared_ptr<arrow::Table>> batch_tables;
    std::vector<std::size_t> batch_members;
    std::int64_t batch_bytes = 0;

    const auto flush_batch = [&]() -> Status {
      if (batch_tables.empty()) {
        return Ok();
      }
      Result<std::shared_ptr<arrow::Table>> combined = concat_tables(batch_tables);
      if (!combined.has_value()) {
        return Err(combined.error());
      }
      const std::string dst_path = (dst_dir / format_batch_name(next_batch)).string();
      ++next_batch;
      Status write_status = write_parquet_batch(**combined, dst_path);
      if (!write_status.has_value()) {
        return write_status;
      }
      // The batch file landed durably (atomic rename). Only now delete the
      // staged inputs it folded in.
      for (const std::size_t idx : batch_members) {
        std::error_code rm_ec;
        fs::remove(staged[idx].path, rm_ec);
        if (rm_ec) {
          return Err(ErrorCode::IoError,
                     "compact: batch published but staged input delete failed: " + staged[idx].path +
                         ": " + rm_ec.message());
        }
        ++stats.staged_files_deleted;
      }
      stats.tracks_compacted += batch_members.size();
      ++stats.batch_files_written;
      batch_tables.clear();
      batch_members.clear();
      batch_bytes = 0;
      return Ok();
    };

    for (const std::size_t idx : idxs) {
      batch_bytes += table_nbytes(*staged[idx].table);
      batch_tables.push_back(staged[idx].table);
      batch_members.push_back(idx);
      if (batch_bytes >= kTargetBatchBytes) {
        Status flushed = flush_batch();
        if (!flushed.has_value()) {
          return Err(flushed.error());
        }
      }
    }
    Status flushed = flush_batch(); // remainder, even if under target
    if (!flushed.has_value()) {
      return Err(flushed.error());
    }
  }

  return Ok(stats);
}

} // namespace atx::vol
