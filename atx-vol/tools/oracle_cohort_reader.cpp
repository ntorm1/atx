#include "oracle_cohort_reader.hpp"

// Third-party Arrow/Parquet headers: same arrangement as src/storage/
// track_store.cpp — the vcpkg IMPORTED targets' include dirs are SYSTEM, so
// this stays /W4 /WX-clean unwrapped. The file scan is DIRECT Arrow rather
// than atx-core's LazyParquet DELIBERATELY: the real oracle store is written
// by polars, whose parquet files round-trip strings as arrow LARGE_STRING,
// and LazyParquet's string predicate/bridge supports STRING only (measured:
// its filter returns InvalidArgument "unsupported column type" on a
// polars-written undSecKey_tk). Handling both encodings here keeps the
// atx-core surface untouched and the tool compatible with the store it
// exists to read.
#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/memory_pool.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace atx::vol::oracle {

using atx::core::Ok; // Err resolves via ADL; Ok's arguments are not atx::core
                     // types — same convention as track_store.cpp.

// ── Cohort JSON (strict minimal parser) ─────────────────────────────────
//
// The cohort schema needs exactly: an object of string -> (string | array of
// string). No in-repo JSON parser exists outside third-party trees, and the
// schema is pinned by cohorts/README.md, so a bounded recursive-descent
// subset parser here beats importing a dependency for five keys. Unknown keys
// with scalar / flat-array values are skipped; nested objects are rejected.

namespace {

struct Cursor {
  std::string_view s;
  std::size_t i = 0;
  [[nodiscard]] bool eof() const noexcept { return i >= s.size(); }
  [[nodiscard]] char peek() const noexcept { return s[i]; }
};

void skip_ws(Cursor &c) noexcept {
  while (!c.eof() &&
         (c.peek() == ' ' || c.peek() == '\t' || c.peek() == '\n' || c.peek() == '\r')) {
    ++c.i;
  }
}

[[nodiscard]] Result<std::string> parse_string(Cursor &c) {
  skip_ws(c);
  if (c.eof() || c.peek() != '"') {
    return Err(ErrorCode::ParseError, "cohort JSON: expected string");
  }
  ++c.i;
  std::string out;
  while (!c.eof()) { // bounded: every iteration consumes >= 1 char
    const char ch = c.s[c.i++];
    if (ch == '"') {
      return Ok(std::move(out));
    }
    if (ch == '\\') {
      if (c.eof()) {
        break;
      }
      const char esc = c.s[c.i++];
      switch (esc) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default: // \uXXXX deliberately unsupported: cohort files are ASCII
        return Err(ErrorCode::ParseError, "cohort JSON: unsupported string escape");
      }
    } else if (static_cast<unsigned char>(ch) < 0x20) {
      return Err(ErrorCode::ParseError, "cohort JSON: control character in string");
    } else {
      out.push_back(ch);
    }
  }
  return Err(ErrorCode::ParseError, "cohort JSON: unterminated string");
}

[[nodiscard]] Result<std::vector<std::string>> parse_string_array(Cursor &c) {
  skip_ws(c);
  if (c.eof() || c.peek() != '[') {
    return Err(ErrorCode::ParseError, "cohort JSON: expected array of strings");
  }
  ++c.i;
  std::vector<std::string> out;
  skip_ws(c);
  if (!c.eof() && c.peek() == ']') {
    ++c.i;
    return Ok(std::move(out));
  }
  for (std::size_t guard = 0; guard <= c.s.size(); ++guard) { // bounded (JPL)
    ATX_TRY(std::string item, parse_string(c));
    out.push_back(std::move(item));
    skip_ws(c);
    if (c.eof()) {
      break;
    }
    const char ch = c.s[c.i++];
    if (ch == ']') {
      return Ok(std::move(out));
    }
    if (ch != ',') {
      return Err(ErrorCode::ParseError, "cohort JSON: expected ',' or ']' in array");
    }
  }
  return Err(ErrorCode::ParseError, "cohort JSON: unterminated array");
}

// One unknown-key scalar: string / number / true / false / null.
[[nodiscard]] Status skip_scalar(Cursor &c) {
  skip_ws(c);
  if (c.eof()) {
    return Err(ErrorCode::ParseError, "cohort JSON: truncated value");
  }
  if (c.peek() == '"') {
    ATX_TRY(const std::string ignored, parse_string(c));
    (void)ignored;
    return Ok();
  }
  const std::size_t start = c.i;
  while (!c.eof()) {
    const char ch = c.peek();
    if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' || ch == '\n' ||
        ch == '\r') {
      break;
    }
    ++c.i;
  }
  if (c.i == start) {
    return Err(ErrorCode::ParseError, "cohort JSON: empty value");
  }
  return Ok();
}

// Unknown-key value: a scalar or a FLAT array of scalars.
[[nodiscard]] Status skip_value(Cursor &c) {
  skip_ws(c);
  if (c.eof()) {
    return Err(ErrorCode::ParseError, "cohort JSON: truncated value");
  }
  if (c.peek() == '{') {
    return Err(ErrorCode::ParseError, "cohort JSON: nested objects are not part of the schema");
  }
  if (c.peek() != '[') {
    return skip_scalar(c);
  }
  ++c.i;
  skip_ws(c);
  if (!c.eof() && c.peek() == ']') {
    ++c.i;
    return Ok();
  }
  for (std::size_t guard = 0; guard <= c.s.size(); ++guard) { // bounded (JPL)
    ATX_TRY_VOID(skip_scalar(c));
    skip_ws(c);
    if (c.eof()) {
      break;
    }
    const char ch = c.s[c.i++];
    if (ch == ']') {
      return Ok();
    }
    if (ch != ',') {
      return Err(ErrorCode::ParseError, "cohort JSON: expected ',' or ']'");
    }
  }
  return Err(ErrorCode::ParseError, "cohort JSON: unterminated array");
}

[[nodiscard]] bool is_digits(std::string_view s) noexcept {
  if (s.empty()) {
    return false;
  }
  for (const char ch : s) {
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}

// YYYY-MM-DD shape (calendar validity is the store's concern; the shape check
// exists because dates become path components).
[[nodiscard]] bool is_date_shape(std::string_view s) noexcept {
  return s.size() == 10 && is_digits(s.substr(0, 4)) && s[4] == '-' &&
         is_digits(s.substr(5, 2)) && s[7] == '-' && is_digits(s.substr(8, 2));
}

} // namespace

Result<Cohort> parse_cohort_json(std::string_view text) {
  Cursor c{text};
  skip_ws(c);
  if (c.eof() || c.peek() != '{') {
    return Err(ErrorCode::ParseError, "cohort JSON: expected top-level object");
  }
  ++c.i;
  Cohort out;
  bool have_name = false;
  bool have_dates = false;
  bool have_underliers = false;
  bool have_buckets = false;

  skip_ws(c);
  bool closed = false;
  if (!c.eof() && c.peek() == '}') {
    ++c.i;
    closed = true;
  }
  for (std::size_t guard = 0; guard <= text.size() && !closed; ++guard) { // bounded (JPL)
    ATX_TRY(const std::string key, parse_string(c));
    skip_ws(c);
    if (c.eof() || c.s[c.i++] != ':') {
      return Err(ErrorCode::ParseError, "cohort JSON: expected ':' after key");
    }
    if (key == "name") {
      ATX_TRY(out.name, parse_string(c));
      have_name = true;
    } else if (key == "notes") {
      ATX_TRY(out.notes, parse_string(c));
    } else if (key == "dates") {
      ATX_TRY(out.dates, parse_string_array(c));
      have_dates = true;
    } else if (key == "underliers") {
      ATX_TRY(out.underliers, parse_string_array(c));
      have_underliers = true;
    } else if (key == "buckets_et") {
      ATX_TRY(out.buckets_et, parse_string_array(c));
      have_buckets = true;
    } else {
      ATX_TRY_VOID(skip_value(c));
    }
    skip_ws(c);
    if (c.eof()) {
      return Err(ErrorCode::ParseError, "cohort JSON: unterminated object");
    }
    const char ch = c.s[c.i++];
    if (ch == '}') {
      closed = true;
    } else if (ch != ',') {
      return Err(ErrorCode::ParseError, "cohort JSON: expected ',' or '}'");
    } else {
      skip_ws(c);
    }
  }
  if (!closed) {
    return Err(ErrorCode::ParseError, "cohort JSON: unterminated object");
  }
  skip_ws(c);
  if (!c.eof()) {
    return Err(ErrorCode::ParseError, "cohort JSON: trailing content after object");
  }

  // Schema validation (README rules + path safety: dates/buckets become
  // partition dir names).
  if (!have_name || out.name.empty()) {
    return Err(ErrorCode::InvalidArgument, "cohort: missing or empty 'name'");
  }
  if (!have_dates || out.dates.empty()) {
    return Err(ErrorCode::InvalidArgument, "cohort: missing or empty 'dates'");
  }
  if (!have_underliers || out.underliers.empty()) {
    return Err(ErrorCode::InvalidArgument, "cohort: missing or empty 'underliers'");
  }
  if (!have_buckets || out.buckets_et.empty()) {
    return Err(ErrorCode::InvalidArgument, "cohort: missing or empty 'buckets_et'");
  }
  for (const std::string &date : out.dates) {
    if (!is_date_shape(date)) {
      return Err(ErrorCode::InvalidArgument, "cohort: date not YYYY-MM-DD: " + date);
    }
  }
  for (const std::string &bucket : out.buckets_et) {
    if (bucket.size() != 4 || !is_digits(bucket)) {
      return Err(ErrorCode::InvalidArgument, "cohort: bucket_et not HHMM: " + bucket);
    }
  }
  for (const std::string &tk : out.underliers) {
    if (tk.empty() || tk.find_first_of("/\\") != std::string::npos) {
      return Err(ErrorCode::InvalidArgument, "cohort: bad underlier token: " + tk);
    }
  }
  return Ok(std::move(out));
}

Result<Cohort> load_cohort_json(const std::string &path) {
  std::ifstream f{path, std::ios::binary};
  if (!f) {
    return Err(ErrorCode::IoError, "cannot open cohort JSON: " + path);
  }
  std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  if (text.size() > (1u << 20)) {
    return Err(ErrorCode::InvalidArgument, "cohort JSON implausibly large (> 1 MiB): " + path);
  }
  return parse_cohort_json(text);
}

// ── Partition-pruned parquet reading ────────────────────────────────────

namespace {

namespace fs = std::filesystem;

[[nodiscard]] Error from_arrow(const arrow::Status &s, const std::string &ctx) {
  return Error{ErrorCode::IoError, ctx + ": " + s.ToString()};
}

// Required Float64 columns (stage-1 ingest pins every numeric column to
// Float64; a different physical type surfaces as a loud InvalidArgument,
// never a silent cast).
enum NumericCol : std::size_t {
  kOkeyXx = 0,
  kUPrc,
  kRate,
  kSdiv,
  kDdiv,
  kYears,
  kSrVol,
  kSrPrc,
  kDe,
  kGa,
  kTh,
  kVe,
  kRh,
  kPh,
  kVo,
  kVa,
  kDeDecay,
  kBidPrc,
  kAskPrc,
  kNumNumericCols,
};

constexpr std::array<std::string_view, kNumNumericCols> kNumericCols{
    "okey_xx", "uPrc", "rate", "sdiv",    "ddiv",   "years",  "srVol",
    "srPrc",   "de",   "ga",   "th",      "ve",     "rh",     "ph",
    "vo",      "va",   "deDecay", "bidPrc", "askPrc"};

// -99 sentinels became nulls on exactly these at ingest.
constexpr std::array<std::string_view, 3> kSentinelCols{"bidIV", "askIV", "error"};

[[nodiscard]] std::optional<Side> parse_side(std::string_view cp) noexcept {
  if (cp.empty()) {
    return std::nullopt;
  }
  const char c0 = cp.front();
  if (c0 == 'C' || c0 == 'c') {
    return Side::Call;
  }
  if (c0 == 'P' || c0 == 'p') {
    return Side::Put;
  }
  return std::nullopt;
}

// True iff row group `rg`'s undSecKey_tk statistics ADMIT `tk` — the
// predicate-pushdown leg: a group whose [min, max] cannot contain tk is never
// read. Absent/foreign statistics fail open (correctness never depends on
// stats; the exact row filter below still runs).
[[nodiscard]] bool group_may_contain(const parquet::RowGroupMetaData &rg, int col_idx,
                                     std::string_view tk) {
  const std::unique_ptr<parquet::ColumnChunkMetaData> cc = rg.ColumnChunk(col_idx);
  if (cc == nullptr || !cc->is_stats_set()) {
    return true;
  }
  const std::shared_ptr<parquet::Statistics> stats = cc->statistics();
  if (stats == nullptr || !stats->HasMinMax() ||
      stats->physical_type() != parquet::Type::BYTE_ARRAY) {
    return true;
  }
  // SAFETY: physical_type() == BYTE_ARRAY -> the dynamic type is
  // TypedStatistics<ByteArrayType> (same pattern as atx-core parquet.cpp's
  // typed-statistics casts).
  const auto typed = std::static_pointer_cast<parquet::ByteArrayStatistics>(stats);
  const parquet::ByteArray mn = typed->min();
  const parquet::ByteArray mx = typed->max();
  // SAFETY: ByteArray::ptr is the raw UTF-8 byte run; char_traits<char>::
  // compare is memcmp-like (unsigned), matching parquet's unsigned
  // byte-string sort order for these stats.
  const std::string_view mn_v{reinterpret_cast<const char *>(mn.ptr), mn.len};
  const std::string_view mx_v{reinterpret_cast<const char *>(mx.ptr), mx.len};
  return mn_v <= tk && tk <= mx_v;
}

// The value at row `i` of a STRING or LARGE_STRING array. Callers validate
// the type id up front (check_string_column).
[[nodiscard]] std::string_view string_at(const arrow::Array &arr, std::int64_t i) noexcept {
  if (arr.type_id() == arrow::Type::STRING) {
    // SAFETY: type id checked on this line.
    return static_cast<const arrow::StringArray &>(arr).GetView(i);
  }
  // SAFETY: callers admit only STRING / LARGE_STRING columns.
  return static_cast<const arrow::LargeStringArray &>(arr).GetView(i);
}

[[nodiscard]] bool is_string_type(const arrow::ChunkedArray &col) noexcept {
  const arrow::Type::type id = col.type()->id();
  return id == arrow::Type::STRING || id == arrow::Type::LARGE_STRING;
}

// The partition dir a scan is reading, as the COHORT declared it. Passed down
// rather than parsed back out of the file path: these two strings are what
// formed the path, so stamping them onto each row cannot disagree with the
// directory that was opened.
struct PartitionKey {
  std::string_view date;      // YYYY-MM-DD
  std::string_view bucket_et; // HHMM
};

// Scans ONE parquet file for ONE underlier: row groups pruned via the
// undSecKey_tk column statistics, only the compared columns decoded, then an
// exact per-row tk filter and the sentinel / bad-input admission screens.
// Handles BOTH utf8 and large_utf8 string columns (polars writes the latter).
[[nodiscard]] Status scan_file_for_underlier(const std::string &file, const std::string &tk,
                                             const PartitionKey &partition, CohortScan &out) {
  auto in_res = arrow::io::ReadableFile::Open(file);
  if (!in_res.ok()) {
    return Err(from_arrow(in_res.status(), "oracle store: open " + file));
  }
  auto reader_res = parquet::arrow::OpenFile(*in_res, arrow::default_memory_pool());
  if (!reader_res.ok()) {
    return Err(from_arrow(reader_res.status(), "oracle store: open reader " + file));
  }
  const std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(reader_res);

  std::shared_ptr<arrow::Schema> schema;
  const arrow::Status schema_st = reader->GetSchema(&schema);
  if (!schema_st.ok()) {
    return Err(from_arrow(schema_st, "oracle store: schema of " + file));
  }
  // Column projection: resolve every compared column (flat schema: arrow
  // field index == parquet column index).
  std::vector<int> indices;
  indices.reserve(kNumNumericCols + kSentinelCols.size() + 2);
  auto push_index = [&](std::string_view name) -> Status {
    const int idx = schema->GetFieldIndex(std::string{name});
    if (idx < 0) {
      return Err(ErrorCode::InvalidArgument,
                 "oracle store: column '" + std::string{name} + "' missing in " + file);
    }
    indices.push_back(idx);
    return Ok();
  };
  for (const std::string_view col : kNumericCols) {
    ATX_TRY_VOID(push_index(col));
  }
  for (const std::string_view col : kSentinelCols) {
    ATX_TRY_VOID(push_index(col));
  }
  ATX_TRY_VOID(push_index("okey_cp"));
  ATX_TRY_VOID(push_index("undSecKey_tk"));
  const int tk_field_idx = indices.back();

  // Predicate pushdown on the underlier: prune row groups by statistics.
  const std::shared_ptr<parquet::FileMetaData> meta = reader->parquet_reader()->metadata();
  std::vector<int> keep;
  const int n_groups = meta->num_row_groups();
  keep.reserve(static_cast<std::size_t>(n_groups));
  for (int g = 0; g < n_groups; ++g) {
    if (group_may_contain(*meta->RowGroup(g), tk_field_idx, tk)) {
      keep.push_back(g);
    }
  }
  if (keep.empty()) {
    return Ok(); // underlier provably absent from this file
  }

  auto table_res = reader->ReadRowGroups(keep, indices);
  if (!table_res.ok()) {
    return Err(from_arrow(table_res.status(), "oracle store: read " + file));
  }
  auto combined_res = (*table_res)->CombineChunks(arrow::default_memory_pool());
  if (!combined_res.ok()) {
    return Err(from_arrow(combined_res.status(), "oracle store: combine " + file));
  }
  const std::shared_ptr<arrow::Table> table = *std::move(combined_res);
  const std::int64_t n_rows = table->num_rows();
  if (n_rows == 0) {
    return Ok();
  }

  // Post-CombineChunks every column is single-chunk; the table shared_ptr
  // keeps every array alive for the extraction below.
  auto column = [&](std::string_view name) -> Result<std::shared_ptr<arrow::ChunkedArray>> {
    std::shared_ptr<arrow::ChunkedArray> col = table->GetColumnByName(std::string{name});
    if (col == nullptr || col->num_chunks() != 1) {
      return Err(ErrorCode::Internal,
                 "oracle store: column '" + std::string{name} + "' unreadable in " + file);
    }
    return Ok(std::move(col));
  };

  std::array<std::shared_ptr<arrow::ChunkedArray>, kNumNumericCols> num_cols;
  std::array<const arrow::DoubleArray *, kNumNumericCols> num_arr{};
  for (std::size_t c = 0; c < kNumNumericCols; ++c) {
    ATX_TRY(num_cols[c], column(kNumericCols[c]));
    if (num_cols[c]->type()->id() != arrow::Type::DOUBLE) {
      return Err(ErrorCode::InvalidArgument, "oracle store: column '" +
                                                 std::string{kNumericCols[c]} +
                                                 "' is not Float64 in " + file);
    }
    // SAFETY: type id checked to DOUBLE immediately above.
    num_arr[c] = static_cast<const arrow::DoubleArray *>(num_cols[c]->chunk(0).get());
  }
  std::array<std::shared_ptr<arrow::ChunkedArray>, kSentinelCols.size()> sentinel_cols;
  for (std::size_t c = 0; c < kSentinelCols.size(); ++c) {
    ATX_TRY(sentinel_cols[c], column(kSentinelCols[c]));
  }
  ATX_TRY(const std::shared_ptr<arrow::ChunkedArray> cp_col, column("okey_cp"));
  ATX_TRY(const std::shared_ptr<arrow::ChunkedArray> tk_col, column("undSecKey_tk"));
  if (!is_string_type(*cp_col) || !is_string_type(*tk_col)) {
    return Err(ErrorCode::InvalidArgument,
               "oracle store: okey_cp/undSecKey_tk is not a string column in " + file);
  }
  const arrow::Array &cp_arr = *cp_col->chunk(0);
  const arrow::Array &tk_arr = *tk_col->chunk(0);

  for (std::int64_t i = 0; i < n_rows; ++i) {
    // Exact row-level underlier filter (statistics pruning above is only an
    // admission bound).
    if (tk_arr.IsNull(i) || string_at(tk_arr, i) != tk) {
      continue;
    }
    bool sentinel_null = false;
    for (const std::shared_ptr<arrow::ChunkedArray> &col : sentinel_cols) {
      sentinel_null = sentinel_null || col->chunk(0)->IsNull(i);
    }
    if (sentinel_null) {
      ++out.rows_null_sentinel; // -99-at-source row: counted, never priced
      continue;
    }
    bool bad = false;
    for (std::size_t c = 0; c < kNumNumericCols && !bad; ++c) {
      bad = num_arr[c]->IsNull(i) || !std::isfinite(num_arr[c]->Value(i));
    }
    const std::optional<Side> side =
        cp_arr.IsNull(i) ? std::nullopt : parse_side(string_at(cp_arr, i));
    if (bad || !side.has_value()) {
      ++out.rows_bad_input;
      continue;
    }
    OracleRow row;
    row.underlier = tk;
    row.date = partition.date;
    row.bucket_et = partition.bucket_et;
    row.side = *side;
    row.strike = num_arr[kOkeyXx]->Value(i);
    row.uprc = num_arr[kUPrc]->Value(i);
    row.rate = num_arr[kRate]->Value(i);
    row.sdiv = num_arr[kSdiv]->Value(i);
    row.ddiv = num_arr[kDdiv]->Value(i);
    row.years = num_arr[kYears]->Value(i);
    row.sr_vol = num_arr[kSrVol]->Value(i);
    row.sr_prc = num_arr[kSrPrc]->Value(i);
    row.de = num_arr[kDe]->Value(i);
    row.ga = num_arr[kGa]->Value(i);
    row.th = num_arr[kTh]->Value(i);
    row.ve = num_arr[kVe]->Value(i);
    row.rh = num_arr[kRh]->Value(i);
    row.ph = num_arr[kPh]->Value(i);
    row.vo = num_arr[kVo]->Value(i);
    row.va = num_arr[kVa]->Value(i);
    row.de_decay = num_arr[kDeDecay]->Value(i);
    row.bid_prc = num_arr[kBidPrc]->Value(i);
    row.ask_prc = num_arr[kAskPrc]->Value(i);
    out.rows.push_back(std::move(row));
  }
  return Ok();
}

} // namespace

Result<CohortScan> read_cohort_rows(const Cohort &cohort, std::string_view store_root) {
  CohortScan out;
  const fs::path root{std::string{store_root}};
  for (const std::string &date : cohort.dates) {
    for (const std::string &bucket : cohort.buckets_et) {
      // Pruning by construction: the ONLY paths ever formed are the
      // cohort-named partition dirs. The store root is never enumerated.
      const fs::path part = root / ("date=" + date) / ("bucket_et=" + bucket);
      std::error_code ec;
      if (!fs::is_directory(part, ec) || ec) {
        return Err(ErrorCode::NotFound, "cohort partition missing: " + part.string());
      }
      out.partitions_opened.push_back(part.string());

      std::vector<fs::path> files;
      try {
        for (const fs::directory_entry &entry : fs::directory_iterator{part}) {
          if (entry.is_regular_file() && entry.path().extension() == ".parquet") {
            files.push_back(entry.path());
          }
        }
      } catch (const fs::filesystem_error &e) {
        return Err(ErrorCode::IoError,
                   "cannot list cohort partition " + part.string() + ": " + e.what());
      }
      std::sort(files.begin(), files.end()); // deterministic row order
      if (files.empty()) {
        return Err(ErrorCode::NotFound, "no parquet files in cohort partition: " + part.string());
      }
      const PartitionKey key{date, bucket};
      for (const fs::path &file : files) {
        for (const std::string &tk : cohort.underliers) {
          ATX_TRY_VOID(scan_file_for_underlier(file.string(), tk, key, out));
        }
      }
    }
  }
  return Ok(std::move(out));
}

} // namespace atx::vol::oracle
