#include "atx/vol/api/marketdata/spiderrock_store.hpp"

// Third-party Arrow/Parquet headers: the tools/oracle_cohort_reader.cpp
// arrangement — the vcpkg IMPORTED targets' include dirs are SYSTEM, so this TU
// stays /W4 /WX-clean unwrapped. The scan is DIRECT Arrow rather than atx-core's
// LazyParquet for the reason that reader records: the real store is written by
// polars, whose strings round-trip as LARGE_STRING, which LazyParquet's string
// bridge does not read. Both encodings are handled here.
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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/marketdata/data.hpp" // expiry_instant_ns

namespace atx::vol {

using atx::core::Err;
using atx::core::Error;
using atx::core::Ok;

// ── UnderlierTable ──────────────────────────────────────────────────────────

std::uint32_t UnderlierTable::intern(std::string_view ticker) {
  if (const std::uint32_t uid = find(ticker); uid != 0) {
    return uid;
  }
  tickers_.emplace_back(ticker);
  return static_cast<std::uint32_t>(tickers_.size());
}

std::uint32_t UnderlierTable::find(std::string_view ticker) const noexcept {
  for (std::size_t i = 0; i < tickers_.size(); ++i) {
    if (tickers_[i] == ticker) {
      return static_cast<std::uint32_t>(i + 1);
    }
  }
  return 0;
}

std::string_view UnderlierTable::ticker(std::uint32_t uid) const noexcept {
  if (uid == 0 || uid > tickers_.size()) {
    return {};
  }
  return tickers_[uid - 1];
}

namespace {

namespace fs = std::filesystem;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinuteNs = 60'000'000'000;
constexpr std::int64_t kSettleMinuteEt = 16 * 60; // PM settlement, 16:00 ET

[[nodiscard]] bool is_digits(std::string_view s) noexcept {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// YYYY-MM-DD shape (dates become path components, so the shape is checked before
// any path is formed; calendar validity is expiry_instant_ns's concern).
[[nodiscard]] bool is_date_shape(std::string_view s) noexcept {
  return s.size() == 10 && is_digits(s.substr(0, 4)) && s[4] == '-' &&
         is_digits(s.substr(5, 2)) && s[7] == '-' && is_digits(s.substr(8, 2));
}

// "HHMM" -> minutes after midnight ET; nullopt unless 00:00 <= HH:MM <= 23:59.
[[nodiscard]] std::optional<std::int64_t> bucket_minutes(std::string_view b) noexcept {
  if (b.size() != 4 || !is_digits(b)) {
    return std::nullopt;
  }
  const std::int64_t hh = (b[0] - '0') * 10 + (b[1] - '0');
  const std::int64_t mm = (b[2] - '0') * 10 + (b[3] - '0');
  if (hh > 23 || mm > 59) {
    return std::nullopt;
  }
  return hh * 60 + mm;
}

[[nodiscard]] Error from_arrow(const arrow::Status &s, const std::string &ctx) {
  return Error{ErrorCode::IoError, ctx + ": " + s.ToString()};
}

// ── Column layout ───────────────────────────────────────────────────────────

// Float64 columns (the ingest pins every numeric column to Float64; a different
// physical type is a loud InvalidArgument, never a silent cast).
enum NumCol : std::size_t {
  kStrike = 0,
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
  kBid,
  kAsk,
  kNumCols,
};
constexpr std::array<std::string_view, kNumCols> kNumNames{
    "okey_xx", "uPrc", "rate", "sdiv", "ddiv", "years",  "srVol",
    "srPrc",   "de",   "ga",   "th",   "ve",   "bidPrc", "askPrc"};
// A NULL in a required column drops the row (rows_dropped_null); an optional
// column's NULL is carried as NaN (vendor / carry diagnostics only).
constexpr std::array<bool, kNumCols> kRequired{true,  true,  true,  false, false,
                                               true,  true,  false, false, false,
                                               false, false, false, false};

// Vendor sentinels the ingest may leave in place: -99 on any numeric column (the
// oracle store nulls it on bidIV / askIV / error only) and -9.99 on sdiv (seen on
// the 2026-10-14 drop). A sentinel reads as NULL: it drops the row on a required
// column and becomes NaN on an optional one.
constexpr double kSentinel = -99.0;
constexpr double kSdivSentinelCeiling = -9.98;

[[nodiscard]] bool is_sentinel(std::size_t col, double v) noexcept {
  return v == kSentinel || (col == kSdiv && v <= kSdivSentinelCeiling);
}

enum IntCol : std::size_t { kYr = 0, kMn, kDy, kNumIntCols };
constexpr std::array<std::string_view, kNumIntCols> kIntNames{"okey_yr", "okey_mn", "okey_dy"};

// OPTIONAL Float64 columns (the licensed drop has them; the synthetic and the older
// oracle store do not): read when present, a NULL / -99 cell carried as NaN, never a
// reason to drop a row. The bucket's vector stays EMPTY when no file has the column.
enum OptCol : std::size_t { kOptDdivPv = 0, kOptSynSpot, kOptYearsC, kNumOptCols };
constexpr std::array<std::string_view, kNumOptCols> kOptNames{"ddivPv", "synSpot", "yearsC"};
constexpr std::array<std::vector<double> SpiderRockBucket::*, kNumOptCols> kOptFields{
    &SpiderRockBucket::ddiv_pv, &SpiderRockBucket::syn_spot, &SpiderRockBucket::vendor_years_c};

// Row filters read from OPTIONAL columns: the licensed intraday drop's exercise
// style (absent from the synthetic and the older oracle store, where every row is
// American), its price framework (a Forward-framework row carries a per-expiry forward
// in uPrc) and the option root (a root other than the underlier's ticker is an
// adjusted / non-standard deliverable whose (expiry, side, strike) key collides with
// the standard contract's). An absent column filters nothing.
constexpr std::string_view kExTypeName = "exType";
constexpr std::string_view kOptionRootName = "okey_tk";
constexpr std::string_view kPrcFrameworkName = "prcFramework";

struct RowFilters {
  bool american_only{true};
  bool standard_roots_only{true};
  bool spot_framework_only{true};
};

struct ColumnIndex {
  int tk{-1};
  int cp{-1};
  int ex_type{-1};       // >= 0 only when the column exists AND american_only is set
  int okey_tk{-1};       // >= 0 only when the column exists AND standard_roots_only is set
  int prc_framework{-1}; // >= 0 only when the column exists AND spot_framework_only is set
  std::array<int, kNumOptCols> opt{-1, -1, -1}; // >= 0 when the optional column exists
  std::vector<int> all;  // projection, flat schema: arrow field index == parquet column
};

[[nodiscard]] Result<ColumnIndex> resolve_columns(const arrow::Schema &schema,
                                                  const std::string &file, RowFilters filters) {
  ColumnIndex ix;
  auto find = [&](std::string_view name) -> Result<int> {
    const int idx = schema.GetFieldIndex(std::string{name});
    if (idx < 0) {
      return Err(ErrorCode::InvalidArgument,
                 "spiderrock store: column '" + std::string{name} + "' missing in " + file);
    }
    ix.all.push_back(idx);
    return Ok(idx);
  };
  ATX_TRY(ix.tk, find("undSecKey_tk"));
  ATX_TRY(ix.cp, find("okey_cp"));
  for (const std::string_view name : kIntNames) {
    ATX_TRY(const int idx, find(name));
    (void)idx;
  }
  for (const std::string_view name : kNumNames) {
    ATX_TRY(const int idx, find(name));
    (void)idx;
  }
  auto optional = [&](bool wanted, std::string_view name) {
    const int idx = wanted ? schema.GetFieldIndex(std::string{name}) : -1;
    if (idx >= 0) {
      ix.all.push_back(idx);
    }
    return idx;
  };
  ix.ex_type = optional(filters.american_only, kExTypeName);
  ix.okey_tk = optional(filters.standard_roots_only, kOptionRootName);
  ix.prc_framework = optional(filters.spot_framework_only, kPrcFrameworkName);
  for (std::size_t c = 0; c < kNumOptCols; ++c) {
    ix.opt[c] = optional(true, kOptNames[c]);
  }
  return Ok(std::move(ix));
}

struct OpenedFile {
  std::string path;
  std::unique_ptr<parquet::arrow::FileReader> reader;
  std::shared_ptr<parquet::FileMetaData> meta;
  ColumnIndex cols;
};

[[nodiscard]] Result<OpenedFile> open_file(const fs::path &path, RowFilters filters) {
  OpenedFile f;
  f.path = path.string();
  auto in = arrow::io::ReadableFile::Open(f.path);
  if (!in.ok()) {
    return Err(from_arrow(in.status(), "spiderrock store: open " + f.path));
  }
  auto reader = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
  if (!reader.ok()) {
    return Err(from_arrow(reader.status(), "spiderrock store: open reader " + f.path));
  }
  f.reader = *std::move(reader);
  std::shared_ptr<arrow::Schema> schema;
  const arrow::Status st = f.reader->GetSchema(&schema);
  if (!st.ok()) {
    return Err(from_arrow(st, "spiderrock store: schema of " + f.path));
  }
  f.meta = f.reader->parquet_reader()->metadata();
  // The projection (ReadRowGroup's leaf indices) and the row-group statistics address
  // parquet LEAF columns by the arrow field index; that holds iff every field is one
  // leaf. A nested column would misalign both (statistics of another column => groups
  // pruned silently), so it is refused here.
  if (schema->num_fields() != f.meta->num_columns()) {
    return Err(ErrorCode::InvalidArgument,
               "spiderrock store: nested (non-flat) schema in " + f.path + ": " +
                   std::to_string(schema->num_fields()) + " fields over " +
                   std::to_string(f.meta->num_columns()) + " leaf columns");
  }
  ATX_TRY(f.cols, resolve_columns(*schema, f.path, filters));
  return Ok(std::move(f));
}

// Every `*.parquet` regular file of one partition dir, sorted (deterministic row
// order). NotFound when the dir is missing or holds none.
[[nodiscard]] Result<std::vector<fs::path>> partition_files(const fs::path &part) {
  std::error_code ec;
  if (!fs::is_directory(part, ec) || ec) {
    return Err(ErrorCode::NotFound, "spiderrock partition missing: " + part.string());
  }
  std::vector<fs::path> files;
  for (auto it = fs::directory_iterator(part, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".parquet") {
      files.push_back(it->path());
    }
  }
  if (ec) {
    return Err(ErrorCode::IoError, "cannot list " + part.string() + ": " + ec.message());
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    return Err(ErrorCode::NotFound, "no parquet files in partition: " + part.string());
  }
  return Ok(std::move(files));
}

// ── String / int accessors (tools/oracle_cohort_reader.cpp rules) ─────────────

[[nodiscard]] bool is_string_type(const arrow::Array &arr) noexcept {
  const arrow::Type::type id = arr.type_id();
  return id == arrow::Type::STRING || id == arrow::Type::LARGE_STRING;
}

[[nodiscard]] std::string_view string_at(const arrow::Array &arr, std::int64_t i) noexcept {
  if (arr.type_id() == arrow::Type::STRING) {
    // SAFETY: type id checked on this line.
    return static_cast<const arrow::StringArray &>(arr).GetView(i);
  }
  // SAFETY: callers admit only STRING / LARGE_STRING columns (is_string_type).
  return static_cast<const arrow::LargeStringArray &>(arr).GetView(i);
}

[[nodiscard]] bool is_int_type(const arrow::Array &arr) noexcept {
  return arr.type_id() == arrow::Type::INT64 || arr.type_id() == arrow::Type::INT32;
}

[[nodiscard]] std::int64_t int_at(const arrow::Array &arr, std::int64_t i) noexcept {
  if (arr.type_id() == arrow::Type::INT64) {
    // SAFETY: type id checked on this line.
    return static_cast<const arrow::Int64Array &>(arr).Value(i);
  }
  // SAFETY: callers admit only INT64 / INT32 columns (is_int_type).
  return static_cast<const arrow::Int32Array &>(arr).Value(i);
}

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

// Cell `i` of a string column equals `lower` (a lower-case ASCII word) ignoring ASCII
// case. A NULL equals nothing.
[[nodiscard]] bool equals_ci(const arrow::Array &col, std::int64_t i,
                             std::string_view lower) noexcept {
  if (col.IsNull(i)) {
    return false;
  }
  const std::string_view v = string_at(col, i);
  if (v.size() != lower.size()) {
    return false;
  }
  for (std::size_t k = 0; k < v.size(); ++k) {
    const char c = (v[k] >= 'A' && v[k] <= 'Z') ? static_cast<char>(v[k] - 'A' + 'a') : v[k];
    if (c != lower[k]) {
      return false;
    }
  }
  return true;
}

// The optional filter columns of one decoded row group (null = column absent or
// filter off). All must be string columns (checked by the caller).
struct FilterArrays {
  const arrow::Array *ex_type{nullptr};
  const arrow::Array *okey_tk{nullptr};
  const arrow::Array *prc_framework{nullptr};
};

enum class FilterVerdict : std::uint8_t { Pass, NonAmerican, NonSpotFramework, NonStandardRoot };

// Row `i` (non-null ticker `tk`) against the optional filters: exType first (a NULL is
// not American), then the price framework (a NULL is not Spot), then the option root (a
// NULL root cannot be judged and passes).
[[nodiscard]] FilterVerdict row_filter(const FilterArrays &f, std::string_view tk,
                                       std::int64_t i) noexcept {
  if (f.ex_type != nullptr && !equals_ci(*f.ex_type, i, "american")) {
    return FilterVerdict::NonAmerican;
  }
  if (f.prc_framework != nullptr && !equals_ci(*f.prc_framework, i, "spot")) {
    return FilterVerdict::NonSpotFramework;
  }
  if (f.okey_tk != nullptr && !f.okey_tk->IsNull(i) && string_at(*f.okey_tk, i) != tk) {
    return FilterVerdict::NonStandardRoot;
  }
  return FilterVerdict::Pass;
}

// ── Underlier admission ─────────────────────────────────────────────────────

// nullopt = admit every ticker; otherwise a SORTED, unique ticker set.
using Admitted = std::optional<std::vector<std::string>>;

[[nodiscard]] bool admits(const Admitted &a, std::string_view tk) noexcept {
  return !a.has_value() || std::binary_search(a->begin(), a->end(), tk, std::less<>{});
}

// True iff row group `rg`'s ticker statistics admit some admitted ticker. Absent /
// foreign statistics fail open (correctness never depends on stats; the exact row
// filter still runs).
[[nodiscard]] bool group_may_hold(const parquet::RowGroupMetaData &rg, int col_idx,
                                  const Admitted &a) {
  if (!a.has_value()) {
    return true;
  }
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
  // TypedStatistics<ByteArrayType> (oracle_cohort_reader.cpp's cast).
  const auto typed = std::static_pointer_cast<parquet::ByteArrayStatistics>(stats);
  const parquet::ByteArray mn = typed->min();
  const parquet::ByteArray mx = typed->max();
  // SAFETY: ByteArray::ptr is the raw UTF-8 byte run; string_view comparison is
  // memcmp-like, matching parquet's unsigned byte-string order for these stats.
  const std::string_view lo{reinterpret_cast<const char *>(mn.ptr), mn.len};
  const std::string_view hi{reinterpret_cast<const char *>(mx.ptr), mx.len};
  const auto it = std::lower_bound(a->begin(), a->end(), lo, std::less<>{});
  return it != a->end() && std::string_view{*it} <= hi;
}

// The one chunk of column `name` (CombineChunks can leave a binary column in several
// chunks past 2 GiB of offsets; that is refused loudly, never read partially).
[[nodiscard]] Result<const arrow::Array *> single_chunk(const arrow::Table &table,
                                                       std::string_view name,
                                                       const std::string &file) {
  const std::shared_ptr<arrow::ChunkedArray> col = table.GetColumnByName(std::string{name});
  if (col == nullptr || col->num_chunks() != 1) {
    return Err(ErrorCode::Internal,
               "spiderrock store: column '" + std::string{name} + "' unreadable in " + file);
  }
  return Ok(col->chunk(0).get());
}

// Counts the non-null tickers of one row group into `counts`, over the rows that
// pass the optional filters: the filters precede the top-N count.
[[nodiscard]] Status count_group_tickers(const OpenedFile &f, int g,
                                         std::map<std::string, std::int64_t, std::less<>> &counts) {
  std::vector<int> cols{f.cols.tk};
  if (f.cols.ex_type >= 0) {
    cols.push_back(f.cols.ex_type);
  }
  if (f.cols.okey_tk >= 0) {
    cols.push_back(f.cols.okey_tk);
  }
  if (f.cols.prc_framework >= 0) {
    cols.push_back(f.cols.prc_framework);
  }
  auto t = f.reader->ReadRowGroup(g, cols);
  if (!t.ok()) {
    return Err(from_arrow(t.status(), "spiderrock store: read tickers of " + f.path));
  }
  auto combined = (*t)->CombineChunks(arrow::default_memory_pool());
  if (!combined.ok()) {
    return Err(from_arrow(combined.status(), "spiderrock store: combine " + f.path));
  }
  const std::shared_ptr<arrow::Table> table = *std::move(combined);
  if (table->num_rows() == 0) {
    return Ok();
  }
  ATX_TRY(const arrow::Array *tk_arr, single_chunk(*table, "undSecKey_tk", f.path));
  FilterArrays filters;
  if (f.cols.ex_type >= 0) {
    ATX_TRY(filters.ex_type, single_chunk(*table, kExTypeName, f.path));
  }
  if (f.cols.okey_tk >= 0) {
    ATX_TRY(filters.okey_tk, single_chunk(*table, kOptionRootName, f.path));
  }
  if (f.cols.prc_framework >= 0) {
    ATX_TRY(filters.prc_framework, single_chunk(*table, kPrcFrameworkName, f.path));
  }
  const bool strings =
      is_string_type(*tk_arr) &&
      (filters.ex_type == nullptr || is_string_type(*filters.ex_type)) &&
      (filters.okey_tk == nullptr || is_string_type(*filters.okey_tk)) &&
      (filters.prc_framework == nullptr || is_string_type(*filters.prc_framework));
  if (!strings) {
    return Err(ErrorCode::InvalidArgument,
               "spiderrock store: undSecKey_tk/exType/okey_tk/prcFramework is not a string "
               "column in " + f.path);
  }
  for (std::int64_t i = 0; i < tk_arr->length(); ++i) {
    if (tk_arr->IsNull(i)) {
      continue;
    }
    const std::string_view tk = string_at(*tk_arr, i);
    if (row_filter(filters, tk, i) != FilterVerdict::Pass) {
      continue;
    }
    auto it = counts.find(tk);
    if (it == counts.end()) {
      it = counts.emplace(std::string{tk}, 0).first;
    }
    ++it->second;
  }
  return Ok();
}

// Top-N tickers by row count over every file of the partition (ties by ticker),
// returned sorted ascending. Reads ONLY the ticker column.
[[nodiscard]] Result<std::vector<std::string>> top_n_tickers(const std::vector<OpenedFile> &files,
                                                             std::size_t n) {
  std::map<std::string, std::int64_t, std::less<>> counts;
  for (const OpenedFile &f : files) {
    const int n_groups = f.meta->num_row_groups();
    for (int g = 0; g < n_groups; ++g) {
      ATX_TRY_VOID(count_group_tickers(f, g, counts));
    }
  }
  std::vector<std::pair<std::string, std::int64_t>> ranked(counts.begin(), counts.end());
  // The map iterates in ascending ticker order and the sort is stable, so equal
  // counts stay ordered by ticker.
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const auto &a, const auto &b) { return a.second > b.second; });
  ranked.resize(std::min(n, ranked.size()));
  std::vector<std::string> out;
  out.reserve(ranked.size());
  for (auto &entry : ranked) {
    out.push_back(std::move(entry.first));
  }
  std::sort(out.begin(), out.end());
  return Ok(std::move(out));
}

[[nodiscard]] Result<Admitted> admitted_set(const SpiderRockStoreSpec &spec,
                                            const std::vector<OpenedFile> &files) {
  if (!spec.underliers.empty()) {
    std::vector<std::string> list = spec.underliers;
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    return Ok(Admitted{std::move(list)});
  }
  if (spec.max_underliers > 0) {
    ATX_TRY(std::vector<std::string> top, top_n_tickers(files, spec.max_underliers));
    return Ok(Admitted{std::move(top)});
  }
  return Ok(Admitted{});
}

// ── Row decoding ────────────────────────────────────────────────────────────

struct GroupArrays {
  std::shared_ptr<arrow::Table> table; // keeps every array below alive
  const arrow::Array *tk{nullptr};
  const arrow::Array *cp{nullptr};
  FilterArrays filters{}; // optional exType / okey_tk / prcFramework
  std::array<const arrow::Array *, kNumIntCols> ints{};
  std::array<const arrow::DoubleArray *, kNumCols> nums{};
  std::array<const arrow::DoubleArray *, kNumOptCols> opt{}; // null = column absent
};

[[nodiscard]] Error bad_type(std::string_view name, const char *want, const std::string &file) {
  return Error{ErrorCode::InvalidArgument, "spiderrock store: column '" + std::string{name} +
                                               "' is not " + want + " in " + file};
}

[[nodiscard]] Result<GroupArrays> read_group(const OpenedFile &f, int g) {
  auto t = f.reader->ReadRowGroup(g, f.cols.all);
  if (!t.ok()) {
    return Err(from_arrow(t.status(), "spiderrock store: read " + f.path));
  }
  auto combined = (*t)->CombineChunks(arrow::default_memory_pool());
  if (!combined.ok()) {
    return Err(from_arrow(combined.status(), "spiderrock store: combine " + f.path));
  }
  GroupArrays a;
  a.table = *std::move(combined);
  ATX_TRY(a.tk, single_chunk(*a.table, "undSecKey_tk", f.path));
  ATX_TRY(a.cp, single_chunk(*a.table, "okey_cp", f.path));
  if (!is_string_type(*a.tk) || !is_string_type(*a.cp)) {
    return Err(bad_type("undSecKey_tk/okey_cp", "a string", f.path));
  }
  if (f.cols.ex_type >= 0) {
    ATX_TRY(a.filters.ex_type, single_chunk(*a.table, kExTypeName, f.path));
    if (!is_string_type(*a.filters.ex_type)) {
      return Err(bad_type(kExTypeName, "a string", f.path));
    }
  }
  if (f.cols.okey_tk >= 0) {
    ATX_TRY(a.filters.okey_tk, single_chunk(*a.table, kOptionRootName, f.path));
    if (!is_string_type(*a.filters.okey_tk)) {
      return Err(bad_type(kOptionRootName, "a string", f.path));
    }
  }
  if (f.cols.prc_framework >= 0) {
    ATX_TRY(a.filters.prc_framework, single_chunk(*a.table, kPrcFrameworkName, f.path));
    if (!is_string_type(*a.filters.prc_framework)) {
      return Err(bad_type(kPrcFrameworkName, "a string", f.path));
    }
  }
  for (std::size_t c = 0; c < kNumIntCols; ++c) {
    ATX_TRY(a.ints[c], single_chunk(*a.table, kIntNames[c], f.path));
    if (!is_int_type(*a.ints[c])) {
      return Err(bad_type(kIntNames[c], "Int64/Int32", f.path));
    }
  }
  for (std::size_t c = 0; c < kNumCols; ++c) {
    ATX_TRY(const arrow::Array *arr, single_chunk(*a.table, kNumNames[c], f.path));
    if (arr->type_id() != arrow::Type::DOUBLE) {
      return Err(bad_type(kNumNames[c], "Float64", f.path));
    }
    // SAFETY: type id checked to DOUBLE immediately above.
    a.nums[c] = static_cast<const arrow::DoubleArray *>(arr);
  }
  for (std::size_t c = 0; c < kNumOptCols; ++c) {
    if (f.cols.opt[c] < 0) {
      continue;
    }
    ATX_TRY(const arrow::Array *arr, single_chunk(*a.table, kOptNames[c], f.path));
    if (arr->type_id() != arrow::Type::DOUBLE) {
      return Err(bad_type(kOptNames[c], "Float64", f.path));
    }
    // SAFETY: type id checked to DOUBLE immediately above.
    a.opt[c] = static_cast<const arrow::DoubleArray *>(arr);
  }
  return Ok(std::move(a));
}

enum class RowVerdict : std::uint8_t {
  Admitted,
  NotSelected,
  NonAmerican,
  NonSpotFramework,
  NonStandardRoot,
  Null,
  Bad,
};

// Per-load staging: rows are appended with a LOCAL ticker index in `uid`, remapped
// to table uids (interned in ascending ticker order) once the partition is read.
struct Decoder {
  const Admitted &admitted;
  SpiderRockBucket &out;
  std::map<std::string, std::uint32_t, std::less<>> local{};
  std::map<std::int64_t, std::int64_t> expiry_cache{};
  std::string last_tk{};
  std::uint32_t last_local{0};
  bool have_last{false};
  std::array<bool, kNumOptCols> opt_seen{}; // some file of the partition has the column
};

[[nodiscard]] std::int64_t expiry_of(std::int64_t yr, std::int64_t mn, std::int64_t dy,
                                     Decoder &d) {
  if (yr < 1900 || yr > 2200 || mn < 1 || mn > 12 || dy < 1 || dy > 31) {
    return 0;
  }
  const std::int64_t key = yr * 10000 + mn * 100 + dy;
  if (const auto it = d.expiry_cache.find(key); it != d.expiry_cache.end()) {
    return it->second;
  }
  std::array<char, 16> buf{};
  const int len = std::snprintf(buf.data(), buf.size(), "%04lld-%02lld-%02lld",
                                static_cast<long long>(yr), static_cast<long long>(mn),
                                static_cast<long long>(dy));
  const std::int64_t ns =
      len == 10 ? expiry_instant_ns(std::string_view{buf.data(), 10}, SettlementSession::Pm) : 0;
  d.expiry_cache.emplace(key, ns);
  return ns;
}

[[nodiscard]] std::uint32_t local_index(std::string_view tk, Decoder &d) {
  if (d.have_last && tk == d.last_tk) { // rows cluster by ticker: skip the map walk
    return d.last_local;
  }
  auto it = d.local.find(tk);
  if (it == d.local.end()) {
    const auto next = static_cast<std::uint32_t>(d.local.size());
    it = d.local.emplace(std::string{tk}, next).first;
  }
  d.last_tk.assign(tk);
  d.last_local = it->second;
  d.have_last = true;
  return it->second;
}

[[nodiscard]] bool any_required_null(const GroupArrays &a, std::int64_t i) noexcept {
  if (a.cp->IsNull(i)) {
    return true;
  }
  for (const arrow::Array *col : a.ints) {
    if (col->IsNull(i)) {
      return true;
    }
  }
  for (std::size_t c = 0; c < kNumCols; ++c) {
    if (kRequired[c] && (a.nums[c]->IsNull(i) || is_sentinel(c, a.nums[c]->Value(i)))) {
      return true;
    }
  }
  return false;
}

void append_row(const GroupArrays &a, std::int64_t i, std::uint32_t local, double strike,
                std::int64_t expiry, Side side, SpiderRockBucket &o) {
  bool null_vendor = false;
  auto num = [&](NumCol c) {
    const bool null_cell = a.nums[c]->IsNull(i) || is_sentinel(c, a.nums[c]->Value(i));
    null_vendor = null_vendor || null_cell; // only optional columns can reach here NULL
    return null_cell ? kNaN : a.nums[c]->Value(i);
  };
  o.uid.push_back(local);
  o.strike.push_back(strike);
  o.expiry_ns.push_back(expiry);
  o.side.push_back(side);
  o.sigma.push_back(num(kSrVol));
  o.vendor_price.push_back(num(kSrPrc));
  o.vendor_delta.push_back(num(kDe));
  o.vendor_gamma.push_back(num(kGa));
  o.vendor_vega.push_back(num(kVe));
  o.vendor_theta.push_back(num(kTh));
  o.vendor_T.push_back(num(kYears));
  o.uprc.push_back(num(kUPrc));
  o.rate.push_back(num(kRate));
  o.sdiv.push_back(num(kSdiv));
  o.ddiv.push_back(num(kDdiv));
  o.bid.push_back(num(kBid));
  o.ask.push_back(num(kAsk));
  o.rows_null_vendor += null_vendor ? 1 : 0;
  // Every row gets a cell in every optional vector (NaN when this file lacks the
  // column); finalize_bucket clears a vector no file had.
  for (std::size_t c = 0; c < kNumOptCols; ++c) {
    const arrow::DoubleArray *col = a.opt[c];
    const bool cell = col != nullptr && !col->IsNull(i) && col->Value(i) != kSentinel;
    (o.*kOptFields[c]).push_back(cell ? col->Value(i) : kNaN);
  }
}

[[nodiscard]] RowVerdict decode_row(const GroupArrays &a, std::int64_t i, Decoder &d) {
  if (a.tk->IsNull(i)) {
    return RowVerdict::Null;
  }
  const std::string_view tk = string_at(*a.tk, i);
  switch (row_filter(a.filters, tk, i)) { // before admission: the filters precede top-N
  case FilterVerdict::Pass:
    break;
  case FilterVerdict::NonAmerican:
    return RowVerdict::NonAmerican;
  case FilterVerdict::NonSpotFramework:
    return RowVerdict::NonSpotFramework;
  case FilterVerdict::NonStandardRoot:
    return RowVerdict::NonStandardRoot;
  }
  if (!admits(d.admitted, tk)) {
    return RowVerdict::NotSelected;
  }
  if (any_required_null(a, i)) {
    return RowVerdict::Null;
  }
  const std::optional<Side> side = parse_side(string_at(*a.cp, i));
  const double strike = a.nums[kStrike]->Value(i);
  const double uprc = a.nums[kUPrc]->Value(i);
  const double vol = a.nums[kSrVol]->Value(i);
  const std::int64_t expiry = expiry_of(int_at(*a.ints[kYr], i), int_at(*a.ints[kMn], i),
                                        int_at(*a.ints[kDy], i), d);
  const bool valid = side.has_value() && std::isfinite(strike) && strike > 0.0 &&
                     std::isfinite(uprc) && uprc > 0.0 &&
                     std::isfinite(a.nums[kRate]->Value(i)) &&
                     std::isfinite(a.nums[kYears]->Value(i)) && std::isfinite(vol) &&
                     vol > 0.0 && expiry != 0;
  if (!valid) {
    return RowVerdict::Bad;
  }
  append_row(a, i, local_index(tk, d), strike, expiry, *side, d.out);
  return RowVerdict::Admitted;
}

void count_verdict(RowVerdict v, SpiderRockBucket &o) noexcept {
  switch (v) {
  case RowVerdict::Admitted:
    break;
  case RowVerdict::NotSelected:
    ++o.rows_not_selected;
    break;
  case RowVerdict::NonAmerican:
    ++o.dropped_non_american;
    break;
  case RowVerdict::NonSpotFramework:
    ++o.dropped_forward_framework;
    break;
  case RowVerdict::NonStandardRoot:
    ++o.dropped_nonstandard_root;
    break;
  case RowVerdict::Null:
    ++o.rows_dropped_null;
    break;
  case RowVerdict::Bad:
    ++o.rows_dropped_bad_input;
    break;
  }
}

[[nodiscard]] Status decode_file(const OpenedFile &f, Decoder &d) {
  SpiderRockBucket &o = d.out;
  const int n_groups = f.meta->num_row_groups();
  for (int g = 0; g < n_groups; ++g) {
    const std::unique_ptr<parquet::RowGroupMetaData> rg = f.meta->RowGroup(g);
    const std::int64_t n_rows = rg->num_rows();
    o.rows_read += n_rows;
    for (std::size_t c = 0; c < kNumOptCols; ++c) {
      d.opt_seen[c] = d.opt_seen[c] || f.cols.opt[c] >= 0;
    }
    if (!group_may_hold(*rg, f.cols.tk, d.admitted)) {
      o.rows_not_selected += n_rows; // provably no admitted ticker in this group
      continue;
    }
    ATX_TRY(const GroupArrays a, read_group(f, g));
    const std::int64_t n = a.table->num_rows();
    for (std::int64_t i = 0; i < n; ++i) {
      count_verdict(decode_row(a, i, d), o);
    }
  }
  return Ok();
}

template <class T> void gather(std::vector<T> &v, const std::vector<std::size_t> &perm) {
  std::vector<T> tmp;
  tmp.reserve(v.size());
  for (const std::size_t p : perm) {
    tmp.push_back(v[p]);
  }
  v.swap(tmp);
}

// Stable permutation by (uid, expiry_ns, side, strike); the index breaks ties so
// equal keys keep file order (the FIRST of a duplicate key wins downstream).
[[nodiscard]] std::vector<std::size_t> key_order(std::span<const std::uint32_t> uid,
                                                 std::span<const std::int64_t> expiry,
                                                 std::span<const Side> side,
                                                 std::span<const double> strike) {
  std::vector<std::size_t> perm(uid.size());
  for (std::size_t i = 0; i < perm.size(); ++i) {
    perm[i] = i;
  }
  std::sort(perm.begin(), perm.end(), [&](std::size_t a, std::size_t b) {
    if (uid[a] != uid[b]) {
      return uid[a] < uid[b];
    }
    if (expiry[a] != expiry[b]) {
      return expiry[a] < expiry[b];
    }
    if (side[a] != side[b]) {
      return side[a] < side[b];
    }
    if (strike[a] != strike[b]) {
      return strike[a] < strike[b];
    }
    return a < b;
  });
  return perm;
}

void finalize_bucket(Decoder &d, UnderlierTable &table) {
  SpiderRockBucket &o = d.out;
  std::vector<std::uint32_t> remap(d.local.size());
  for (const auto &[name, local] : d.local) { // ascending ticker order
    remap[local] = table.intern(name);
  }
  for (std::uint32_t &u : o.uid) {
    u = remap[u];
  }
  const std::vector<std::size_t> perm = key_order(o.uid, o.expiry_ns, o.side, o.strike);
  gather(o.uid, perm);
  gather(o.strike, perm);
  gather(o.expiry_ns, perm);
  gather(o.side, perm);
  gather(o.sigma, perm);
  gather(o.vendor_price, perm);
  gather(o.vendor_delta, perm);
  gather(o.vendor_gamma, perm);
  gather(o.vendor_vega, perm);
  gather(o.vendor_theta, perm);
  gather(o.vendor_T, perm);
  gather(o.uprc, perm);
  gather(o.rate, perm);
  gather(o.sdiv, perm);
  gather(o.ddiv, perm);
  gather(o.bid, perm);
  gather(o.ask, perm);
  for (std::size_t c = 0; c < kNumOptCols; ++c) {
    std::vector<double> &v = o.*kOptFields[c];
    if (d.opt_seen[c]) {
      gather(v, perm);
    } else {
      std::vector<double>{}.swap(v); // no file has the column: EMPTY, not all-NaN
    }
  }
}

} // namespace

// ── Public: listing, instants, loading ──────────────────────────────────────

Result<std::int64_t> spiderrock_bucket_instant_ns(std::string_view date,
                                                  std::string_view bucket_et) {
  const std::optional<std::int64_t> minutes = bucket_minutes(bucket_et);
  if (!is_date_shape(date) || !minutes.has_value()) {
    return Err(ErrorCode::InvalidArgument, "spiderrock bucket: bad date/bucket '" +
                                               std::string{date} + "' '" +
                                               std::string{bucket_et} + "'");
  }
  const std::int64_t close_ns = expiry_instant_ns(date, SettlementSession::Pm);
  if (close_ns == 0) {
    return Err(ErrorCode::InvalidArgument,
               "spiderrock bucket: unparseable date " + std::string{date});
  }
  return Ok(close_ns - (kSettleMinuteEt - *minutes) * kMinuteNs);
}

double spiderrock_q_eff(double spot, double pv_divs, double rate, double q, double T) noexcept {
  const bool valid = std::isfinite(spot) && spot > 0.0 && std::isfinite(pv_divs) &&
                     pv_divs < spot && std::isfinite(rate) && std::isfinite(q) &&
                     std::isfinite(T) && T > 0.0;
  if (!valid) {
    return kNaN;
  }
  const double forward = (spot - pv_divs) * std::exp((rate - q) * T);
  return rate - std::log(forward / spot) / T;
}

Result<std::vector<std::string>> list_spiderrock_buckets(std::string_view store_root,
                                                         std::string_view date) {
  if (!is_date_shape(date)) {
    return Err(ErrorCode::InvalidArgument,
               "spiderrock store: date not YYYY-MM-DD: " + std::string{date});
  }
  const fs::path dir = fs::path{std::string{store_root}} / ("date=" + std::string{date});
  std::error_code ec;
  if (!fs::is_directory(dir, ec) || ec) {
    return Err(ErrorCode::NotFound, "spiderrock store: no date partition " + dir.string());
  }
  constexpr std::string_view kPrefix = "bucket_et=";
  std::vector<std::string> out;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    const bool bucket_dir =
        name.size() == kPrefix.size() + 4 && name.starts_with(kPrefix) &&
        bucket_minutes(std::string_view{name}.substr(kPrefix.size())).has_value();
    if (bucket_dir && it->is_directory(ec)) {
      out.push_back(name.substr(kPrefix.size()));
    }
  }
  if (ec) {
    return Err(ErrorCode::IoError, "cannot list " + dir.string() + ": " + ec.message());
  }
  std::sort(out.begin(), out.end());
  return Ok(std::move(out));
}

Result<SpiderRockBucket> load_spiderrock_bucket(const SpiderRockStoreSpec &spec,
                                                std::string_view bucket_et,
                                                UnderlierTable &table) {
  ATX_TRY(const std::int64_t ts_ns, spiderrock_bucket_instant_ns(spec.date, bucket_et));
  const fs::path part = fs::path{spec.store_root} / ("date=" + spec.date) /
                        ("bucket_et=" + std::string{bucket_et});
  ATX_TRY(const std::vector<fs::path> paths, partition_files(part));
  std::vector<OpenedFile> files;
  files.reserve(paths.size());
  for (const fs::path &p : paths) {
    const RowFilters filters{.american_only = spec.american_only,
                             .standard_roots_only = spec.standard_roots_only,
                             .spot_framework_only = spec.spot_framework_only};
    ATX_TRY(OpenedFile f, open_file(p, filters));
    files.push_back(std::move(f));
  }
  ATX_TRY(const Admitted admitted, admitted_set(spec, files));

  SpiderRockBucket out;
  out.date = spec.date;
  out.bucket_et = std::string{bucket_et};
  out.ts_ns = ts_ns;
  out.carry_mode = spec.carry_mode;
  out.clock = spec.clock;
  Decoder d{admitted, out};
  for (const OpenedFile &f : files) {
    ATX_TRY_VOID(decode_file(f, d));
    out.files_opened.push_back(f.path);
  }
  finalize_bucket(d, table);
  return Ok(std::move(out));
}

// ── Bucket -> MarketStateStorage ────────────────────────────────────────────

namespace {

// Median of the finite values among `values[idx]` for idx in `rows`; NaN when none.
// An even count averages the two middle values.
[[nodiscard]] double median_of(std::span<const double> values, std::span<const std::size_t> rows,
                               std::vector<double> &scratch) {
  scratch.clear();
  for (const std::size_t r : rows) {
    if (std::isfinite(values[r])) {
      scratch.push_back(values[r]);
    }
  }
  if (scratch.empty()) {
    return kNaN;
  }
  std::sort(scratch.begin(), scratch.end());
  const std::size_t m = scratch.size() / 2;
  return (scratch.size() % 2 == 1) ? scratch[m] : (scratch[m - 1] + scratch[m]) / 2.0;
}

[[nodiscard]] double or_else(double value, double fallback) noexcept {
  return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] double finite_spread(std::span<const double> values,
                                   std::span<const std::size_t> rows) noexcept {
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  for (const std::size_t r : rows) {
    if (std::isfinite(values[r])) {
      lo = std::min(lo, values[r]);
      hi = std::max(hi, values[r]);
    }
  }
  return hi >= lo ? hi - lo : 0.0;
}


[[nodiscard]] Status check_bucket_shape(const SpiderRockBucket &b) {
  const std::size_t n = b.sigma.size();
  const bool core = b.uid.size() == n && b.strike.size() == n && b.expiry_ns.size() == n &&
                    b.side.size() == n && b.uprc.size() == n && b.rate.size() == n &&
                    b.sdiv.size() == n && b.ddiv.size() == n;
  auto empty_or_n = [n](const std::vector<double> &v) { return v.empty() || v.size() == n; };
  const bool vendor = empty_or_n(b.vendor_price) && empty_or_n(b.vendor_delta) &&
                      empty_or_n(b.vendor_T) && empty_or_n(b.bid) && empty_or_n(b.ask) &&
                      empty_or_n(b.ddiv_pv);
  if (!core || !vendor) {
    return Err(ErrorCode::InvalidArgument,
               "market_state_from_bucket: bucket columns have inconsistent lengths");
  }
  return Ok();
}

// ── Implied carry (SpiderRockCarryMode::ImpliedFromCalls) ──────────────────

using Clock = std::chrono::steady_clock;

constexpr double kImpliedLoSpot = 0.2; // F bracket [0.2, 5] x spot
constexpr double kImpliedHiSpot = 5.0;
constexpr double kImpliedTolRel = 1e-10;
constexpr int kImpliedMaxSteps = 100;
constexpr double kImpliedMinSigma = 0.01; // srVol admitted in (0.01, 3]
constexpr double kImpliedMaxSigma = 3.0;

[[nodiscard]] double norm_cdf(double x) noexcept {
  return 0.5 * std::erfc(-x / std::numbers::sqrt2);
}

struct PriceInF {
  double f;  // price - target
  double df; // d price / dF = e^{-rT} N(d1) > 0
  double d1;
};

// European Black call on the forward, e^{-rT}[F N(d1) - K N(d2)], minus `target`, with
// its F-derivative; the price is increasing and convex in F, with
// d2 price / dF2 = e^{-rT} phi(d1) / (F sd) (curvature()).
struct BlackCallInF {
  double target;
  double K;
  double sd;   // sigma sqrt(T)
  double disc; // e^{-rT}
  [[nodiscard]] PriceInF eval(double F) const noexcept {
    const double d1 = (std::log(F / K) + 0.5 * sd * sd) / sd;
    const double n1 = norm_cdf(d1);
    return {disc * (F * n1 - K * norm_cdf(d1 - sd)) - target, disc * n1, d1};
  }
  [[nodiscard]] double curvature(double F, double d1) const noexcept {
    return disc * std::exp(-0.5 * d1 * d1) / (std::sqrt(2.0 * std::numbers::pi) * F * sd);
  }
};

// The forward F in the bracket [lo, hi] at which the European Black call prices
// `fn.target`, to kImpliedTolRel relative, in <= kImpliedMaxSteps price evaluations:
// Newton steps from `guess`, each safeguarded by the bracket, which shrinks to every
// evaluated point (a step that would leave it bisects instead). Converged when the Newton
// correction dx is within the tolerance, or when the step lands inside the bracket and the
// error it leaves, (f'' / 2 f') dx^2 by Newton's quadratic convergence, is under a
// thousandth of the tolerance (saving the confirming evaluation). That estimate is trusted
// only for a step over which d1 moves by <= 0.01, so the curvature at x bounds the
// curvature along the step: far from the root (deep in the money, f'' ~ 0) it would
// otherwise accept a linear extrapolation. The bracket ends are NOT priced up front: a
// target outside [price(lo), price(hi)] never converges, the bracket collapses onto one
// end without a sign change, and that is nullopt (as are NaN prices and running out of
// steps). Pure bisection would need ~36 evaluations per call; a warm-started Newton needs
// ~2, which keeps a bucket's ~75k inversions near the brief's 20 ms.
[[nodiscard]] std::optional<double> forward_from_call(const BlackCallInF &fn, double lo,
                                                      double hi, double guess) noexcept {
  bool lo_bracketed = false; // an evaluated point below the root (price < target)
  bool hi_bracketed = false; // an evaluated point above it
  double x = (std::isfinite(guess) && guess > lo && guess < hi) ? guess : 0.5 * (lo + hi);
  for (int step = 0; step < kImpliedMaxSteps; ++step) {
    const auto [fx, dfx, d1x] = fn.eval(x);
    if (!std::isfinite(fx)) {
      return std::nullopt;
    }
    if (fx == 0.0) {
      return x;
    }
    if (fx < 0.0) {
      lo = x;
      lo_bracketed = true;
    } else {
      hi = x;
      hi_bracketed = true;
    }
    const double dx = dfx > 0.0 ? fx / dfx : std::numeric_limits<double>::infinity();
    const double newton = x - dx;
    if (std::abs(dx) <= kImpliedTolRel * x) { // x is in the bracket; so is newton, to tol
      return newton;
    }
    const bool inside = newton > lo && newton < hi;
    const bool local = std::abs(dx) <= 0.01 * fn.sd * x; // d1 moves <= 0.01 over the step
    if (inside && local &&
        0.5 * fn.curvature(x, d1x) / dfx * dx * dx <= 1e-3 * kImpliedTolRel * newton) {
      return newton;
    }
    const double next = inside ? newton : 0.5 * (lo + hi);
    if (hi - lo <= kImpliedTolRel * next) {
      return lo_bracketed && hi_bracketed ? std::optional<double>{next} : std::nullopt;
    }
    x = next;
  }
  return std::nullopt;
}

// Linear interpolation between the order statistics of sorted `v` at (n - 1) p.
[[nodiscard]] double sorted_quantile(std::span<const double> v, double p) noexcept {
  const double pos = p * static_cast<double>(v.size() - 1);
  const auto i = static_cast<std::size_t>(pos);
  const double frac = pos - static_cast<double>(i);
  return i + 1 < v.size() ? v[i] + frac * (v[i + 1] - v[i]) : v[i];
}

struct ImpliedForward {
  double forward{kNaN};
  double iqr_rel{kNaN};
  std::size_t n_calls{0};
  bool accepted{false};
};

// The forward the expiry's OTM calls imply (SpiderRockCarryMode in the header): `run` is
// the (uid, expiry) rows, `c` its Published row (rate, and the Newton start), `T` the
// expiry's median vendor `years` (the clock srPrc was computed on).
[[nodiscard]] ImpliedForward implied_forward(const SpiderRockBucket &b,
                                             std::span<const std::size_t> run, double spot,
                                             const ExpiryCarry &c, double T,
                                             std::vector<double> &fs) {
  ImpliedForward out;
  if (!(std::isfinite(T) && T > 0.0 && std::isfinite(c.rate))) {
    return out;
  }
  const double disc = std::exp(-c.rate * T);
  // Newton starts at the Published forward, then at the previous call's F: the calls of
  // one expiry imply nearly one forward, so a warm start converges in ~3 evaluations.
  double guess = (spot - c.pv_divs) * std::exp((c.rate - c.q) * T);
  fs.clear();
  for (const std::size_t r : run) {
    const double K = b.strike[r];
    const double price = b.vendor_price[r];
    const double sigma = b.sigma[r];
    const bool usable = b.side[r] == Side::Call && K >= spot && b.bid[r] > 0.0 &&
                        b.ask[r] >= b.bid[r] && std::isfinite(price) && price > 0.0 &&
                        sigma > kImpliedMinSigma && sigma <= kImpliedMaxSigma;
    if (!usable) {
      continue;
    }
    const BlackCallInF fn{.target = price, .K = K, .sd = sigma * std::sqrt(T), .disc = disc};
    if (const std::optional<double> F =
            forward_from_call(fn, kImpliedLoSpot * spot, kImpliedHiSpot * spot, guess)) {
      fs.push_back(*F);
      guess = *F;
    }
  }
  out.n_calls = fs.size();
  if (fs.empty()) {
    return out;
  }
  std::sort(fs.begin(), fs.end());
  out.forward = sorted_quantile(fs, 0.5);
  out.iqr_rel = (sorted_quantile(fs, 0.75) - sorted_quantile(fs, 0.25)) / out.forward;
  out.accepted = out.n_calls >= kImpliedCarryMinCalls && out.iqr_rel <= kImpliedCarryMaxIqrRel;
  return out;
}

// ── Per-uid state rows ───────────────────────────────────────────────────────

// Everything one state build threads through its per-uid steps.
struct StateBuild {
  const SpiderRockBucket &b;
  const TimeSpec &time;
  bool implied; // ImpliedFromCalls AND the vendor price / years / quote columns exist
  bool vendor_clock; // SpiderRockClock::VendorYears AND the years column exists
  BucketStateReport &rep;
  std::vector<SpiderRockExpiryForward> *forwards;
  std::vector<double> scratch{};
  std::vector<double> fs{};
};

// T on the state's clock; NaN when the clock refuses (VolTime outside its calendar).
[[nodiscard]] double state_T(const StateBuild &sb, std::int64_t expiry_ns) {
  const Result<double> T = time_to_expiry_years(sb.b.ts_ns, expiry_ns, sb.time);
  return T.has_value() ? *T : kNaN;
}

// The design-D4 row from the medians over the expiry's rows.
[[nodiscard]] ExpiryCarry published_carry(StateBuild &sb, std::span<const std::size_t> run,
                                          const UnderlierState &u) {
  const SpiderRockBucket &b = sb.b;
  ExpiryCarry c;
  c.expiry_ns = b.expiry_ns[run.front()];
  // D4 wants the PRESENT value: ddivPv when the column exists and the expiry has a finite
  // cell, else ddiv. The spread is measured on the column the expiry actually used.
  const double pv_median = b.ddiv_pv.empty() ? kNaN : median_of(b.ddiv_pv, run, sb.scratch);
  const bool use_pv = std::isfinite(pv_median);
  c.pv_divs = use_pv ? pv_median : or_else(median_of(b.ddiv, run, sb.scratch), 0.0);
  c.rate = or_else(median_of(b.rate, run, sb.scratch), u.flat_rate);
  c.q = or_else(median_of(b.sdiv, run, sb.scratch), u.flat_q);
  sb.rep.max_ddiv_spread =
      std::max(sb.rep.max_ddiv_spread, finite_spread(use_pv ? b.ddiv_pv : b.ddiv, run));
  return c;
}

// The (uid, expiry) carry rows of one uid; `rows` is the uid's run in key order.
[[nodiscard]] std::vector<ExpiryCarry> expiry_carries(StateBuild &sb,
                                                      std::span<const std::size_t> rows,
                                                      const UnderlierState &u) {
  const SpiderRockBucket &b = sb.b;
  std::vector<ExpiryCarry> out;
  for (std::size_t lo = 0; lo < rows.size();) {
    std::size_t hi = lo + 1;
    while (hi < rows.size() && b.expiry_ns[rows[hi]] == b.expiry_ns[rows[lo]]) {
      ++hi;
    }
    const std::span<const std::size_t> run = rows.subspan(lo, hi - lo);
    ExpiryCarry c = published_carry(sb, run, u);
    SpiderRockExpiryForward diag;
    diag.uid = u.uid;
    diag.expiry_ns = c.expiry_ns;
    diag.T_vendor = b.vendor_T.empty() ? kNaN : median_of(b.vendor_T, run, sb.scratch);
    // Design D22: under the vendor clock the expiry's T IS the vendor's (median) years, for
    // the kernel, the explain and the implied q alike; one clock, so spot e^{(rate - q) T}
    // reproduces the implied F on exactly the T the kernel prices at.
    if (sb.vendor_clock && std::isfinite(diag.T_vendor) && diag.T_vendor > 0.0) {
      c.T_override = diag.T_vendor;
      ++sb.rep.n_expiries_vendor_T;
    }
    const double T = std::isfinite(c.T_override) ? c.T_override : state_T(sb, c.expiry_ns);
    if (T > 0.0 && c.pv_divs < u.spot) {
      diag.published_forward = (u.spot - c.pv_divs) * std::exp((c.rate - c.q) * T);
    }
    if (sb.implied) {
      const Clock::time_point t0 = Clock::now();
      const ImpliedForward imp = implied_forward(b, run, u.spot, c, diag.T_vendor, sb.fs);
      sb.rep.implied_carry_seconds += std::chrono::duration<double>(Clock::now() - t0).count();
      diag.implied_forward = imp.forward;
      diag.n_calls = imp.n_calls;
      diag.iqr_rel = imp.iqr_rel;
      if (imp.accepted && T > 0.0) {
        c.pv_divs = 0.0;
        c.q = c.rate - std::log(imp.forward / u.spot) / T; // spot e^{(rate - q) T} == F
        diag.mode = SpiderRockCarryMode::ImpliedFromCalls;
        if (std::isfinite(diag.published_forward)) {
          sb.rep.max_implied_vs_published_forward_rel =
              std::max(sb.rep.max_implied_vs_published_forward_rel,
                       std::abs(imp.forward / diag.published_forward - 1.0));
        }
      }
    }
    const bool implied_row = diag.mode == SpiderRockCarryMode::ImpliedFromCalls;
    sb.rep.n_expiries_implied += implied_row ? 1u : 0u;
    sb.rep.n_expiries_published += implied_row ? 0u : 1u;
    if (sb.forwards != nullptr) {
      sb.forwards->push_back(diag);
    }
    out.push_back(c);
    lo = hi;
  }
  return out;
}

// One MarksColumns per uid in (expiry, side, strike) order with unique keys (the
// first row of a duplicated key wins: `rows` is a stable key order).
[[nodiscard]] MarketStateStorage::MarksColumns uid_marks(const SpiderRockBucket &b,
                                                         std::span<const std::size_t> rows,
                                                         std::uint32_t uid,
                                                         BucketStateReport &rep) {
  MarketStateStorage::MarksColumns m;
  m.uid = uid;
  const bool vp = !b.vendor_price.empty();
  const bool vd = !b.vendor_delta.empty();
  for (const std::size_t r : rows) {
    if (!(std::isfinite(b.sigma[r]) && b.sigma[r] > 0.0)) {
      ++rep.n_rows_bad_sigma;
      continue;
    }
    const bool dup = !m.sigma.empty() && m.expiry_ns.back() == b.expiry_ns[r] &&
                     m.side.back() == b.side[r] && m.strike.back() == b.strike[r];
    if (dup) {
      ++rep.n_duplicate_marks;
      continue;
    }
    m.strike.push_back(b.strike[r]);
    m.expiry_ns.push_back(b.expiry_ns[r]);
    m.side.push_back(b.side[r]);
    m.sigma.push_back(b.sigma[r]);
    if (vp) {
      m.vendor_price.push_back(b.vendor_price[r]);
    }
    if (vd) {
      m.vendor_delta.push_back(b.vendor_delta[r]);
    }
  }
  return m;
}

void add_uid(StateBuild &sb, std::span<const std::size_t> rows, MarketStateStorage &s) {
  const SpiderRockBucket &b = sb.b;
  const std::uint32_t uid = b.uid[rows.front()];
  const double spot = median_of(b.uprc, rows, sb.scratch);
  if (!(std::isfinite(spot) && spot > 0.0)) {
    ++sb.rep.n_uids_dropped;
    return;
  }
  UnderlierState u;
  u.uid = uid;
  u.spot = spot;
  u.flat_rate = or_else(median_of(b.rate, rows, sb.scratch), 0.0);
  u.flat_q = or_else(median_of(b.sdiv, rows, sb.scratch), 0.0);
  u.carry = CarryFrom::State;
  std::vector<ExpiryCarry> carries = expiry_carries(sb, rows, u);
  const bool any_divs = std::any_of(carries.begin(), carries.end(),
                                    [](const ExpiryCarry &c) { return c.pv_divs > 0.0; });
  sb.rep.n_uids_with_divs += any_divs ? 1u : 0u;
  sb.rep.n_expiry_rows += carries.size();
  MarketStateStorage::MarksColumns marks = uid_marks(b, rows, uid, sb.rep);
  sb.rep.n_marks += marks.sigma.size();
  ++sb.rep.n_uids;
  s.underliers.push_back(u);
  s.term_carries.push_back(std::move(carries));
  s.marks.push_back(std::move(marks));
}

} // namespace

Result<MarketStateStorage> market_state_from_bucket(const SpiderRockBucket &bucket,
                                                    const TimeSpec &time,
                                                    BucketStateReport *report) {
  return market_state_from_bucket(bucket, time, report, nullptr);
}

Result<MarketStateStorage>
market_state_from_bucket(const SpiderRockBucket &bucket, const TimeSpec &time,
                         BucketStateReport *report,
                         std::vector<SpiderRockExpiryForward> *forwards) {
  ATX_TRY_VOID(check_bucket_shape(bucket));
  const std::vector<std::size_t> order =
      key_order(bucket.uid, bucket.expiry_ns, bucket.side, bucket.strike);
  MarketStateStorage s;
  s.ts_ns = bucket.ts_ns;
  s.time = time;
  BucketStateReport rep;
  rep.pv_divs_from_ddiv_pv = !bucket.ddiv_pv.empty();
  if (forwards != nullptr) {
    forwards->clear();
  }
  // The inversion reads srPrc, years and the quote; a bucket without them (hand-built)
  // cannot imply anything and keeps every row Published.
  const std::size_t n = bucket.size();
  const bool implied = bucket.carry_mode == SpiderRockCarryMode::ImpliedFromCalls &&
                       bucket.vendor_price.size() == n && bucket.vendor_T.size() == n &&
                       bucket.bid.size() == n && bucket.ask.size() == n;
  const bool vendor_clock =
      bucket.clock == SpiderRockClock::VendorYears && bucket.vendor_T.size() == n;
  StateBuild sb{.b = bucket,
                .time = time,
                .implied = implied,
                .vendor_clock = vendor_clock,
                .rep = rep,
                .forwards = forwards};
  const std::span<const std::size_t> all{order};
  for (std::size_t lo = 0; lo < all.size();) {
    std::size_t hi = lo + 1;
    while (hi < all.size() && bucket.uid[all[hi]] == bucket.uid[all[lo]]) {
      ++hi;
    }
    add_uid(sb, all.subspan(lo, hi - lo), s);
    lo = hi;
  }
  // Point every underlier at its carry rows only now: `term_carries` no longer
  // grows, and moving the storage moves the inner vectors' buffers, so the spans
  // stay valid for the life of the returned storage (which must not be COPIED; see
  // market_state.hpp).
  for (std::size_t i = 0; i < s.underliers.size(); ++i) {
    s.underliers[i].term_carry = s.term_carries[i];
  }
  if (report != nullptr) {
    *report = rep;
  }
  return Ok(std::move(s));
}

} // namespace atx::vol
