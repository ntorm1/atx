#include "atx/engine/research/fields/sources/vendor_panel.hpp"

#include <arrow/array.h>
#include <arrow/chunked_array.h>
#include <arrow/io/file.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();
constexpr f32 kNanF = std::numeric_limits<f32>::quiet_NaN();

const std::string kDate = "tradingDate";
const std::string kId = "securityID";
const std::string kClose = "close";
const std::string kVolume = "volume";
const std::string kFactor = "cumulReturnFactor";
const std::string kShares = "shares";
const std::string kOpen = "open";
const std::string kHigh = "high";
const std::string kLow = "low";

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "vendor panel: " + std::move(message));
}

[[nodiscard]] core::Error from_arrow(const arrow::Status &status, std::string_view what) {
  return core::Error(status.IsIOError() ? core::ErrorCode::IoError : core::ErrorCode::ParseError,
                     "vendor panel: " + std::string(what) + ": " + status.ToString());
}

// The UTF-8 path text Arrow opens on every platform.
[[nodiscard]] std::string utf8_path(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  std::string out;
  out.reserve(text.size());
  for (const char8_t c : text) {
    out.push_back(static_cast<char>(c));
  }
  return out;
}

// Parquet leaf indexes of the columns one scan reads (-1: not requested).
struct Leaves {
  int date{-1};
  int id{-1};
  int close{-1};
  int volume{-1};
  int factor{-1};
  int shares{-1};
  int open{-1};
  int high{-1};
  int low{-1};
};

// The leaf of `name` when the file has it as one of `types` (research_fields_price.py TH_TYPES /
// OPEN_TYPES, research_fields_ohlc.py BAR_TYPES).
[[nodiscard]] core::Result<int> leaf_of(const arrow::Schema &schema,
                                        const parquet::SchemaDescriptor &descr,
                                        const std::string &name,
                                        std::initializer_list<arrow::Type::type> types) {
  const auto field = schema.GetFieldByName(name);
  const bool typed = field != nullptr && std::find(types.begin(), types.end(),
                                                   field->type()->id()) != types.end();
  const int leaf = descr.ColumnIndex(name);
  if (!typed || leaf < 0) {
    return core::Err(refused("price_source column " + name + " is missing or not of its "
                             "TickerHistory3 type"));
  }
  return core::Ok(leaf);
}

[[nodiscard]] core::Result<Leaves> leaves_of(const arrow::Schema &schema,
                                             const parquet::SchemaDescriptor &descr,
                                             const VendorPanelRequest &request) {
  using T = arrow::Type;
  Leaves out;
  ATX_TRY(out.date, leaf_of(schema, descr, kDate, {T::DATE32}));
  ATX_TRY(out.id, leaf_of(schema, descr, kId, {T::INT64}));
  if (request.price) {
    ATX_TRY(out.close, leaf_of(schema, descr, kClose, {T::FLOAT}));
    ATX_TRY(out.volume, leaf_of(schema, descr, kVolume, {T::DOUBLE}));
    ATX_TRY(out.factor, leaf_of(schema, descr, kFactor, {T::DOUBLE}));
  }
  if (request.shares) {
    ATX_TRY(out.shares, leaf_of(schema, descr, kShares, {T::INT64}));
  }
  if (request.price_open || request.bars) {
    ATX_TRY(out.open, leaf_of(schema, descr, kOpen, {T::FLOAT, T::DOUBLE}));
  }
  if (request.bars) {
    ATX_TRY(out.high, leaf_of(schema, descr, kHigh, {T::FLOAT, T::DOUBLE}));
    ATX_TRY(out.low, leaf_of(schema, descr, kLow, {T::FLOAT, T::DOUBLE}));
  }
  return core::Ok(out);
}

// [min, max] of a row group's tradingDate statistics (date32 = physical INT32, signed order);
// nullopt when the writer recorded none.
[[nodiscard]] std::optional<std::pair<i64, i64>> date_bounds(const parquet::RowGroupMetaData &rg,
                                                             int leaf) {
  const auto chunk = rg.ColumnChunk(leaf);
  if (!chunk->is_stats_set()) {
    return std::nullopt;
  }
  const std::shared_ptr<parquet::Statistics> stats = chunk->statistics();
  if (stats == nullptr || !stats->HasMinMax() || stats->physical_type() != parquet::Type::INT32) {
    return std::nullopt;
  }
  // SAFETY: physical_type() == INT32, so the dynamic type is TypedStatistics<Int32Type>.
  const auto typed = std::static_pointer_cast<parquet::Int32Statistics>(stats);
  return std::pair<i64, i64>{static_cast<i64>(typed->min()), static_cast<i64>(typed->max())};
}

// One row group's columns `leaves`, each as one contiguous chunk.
[[nodiscard]] core::Result<std::shared_ptr<arrow::Table>>
read_row_group(parquet::arrow::FileReader &reader, int group, const std::vector<int> &leaves) {
  auto read = reader.ReadRowGroup(group, leaves);
  if (!read.ok()) {
    return core::Err(from_arrow(read.status(), "read row group"));
  }
  auto combined = (*read)->CombineChunks(arrow::default_memory_pool());
  if (!combined.ok()) {
    return core::Err(from_arrow(combined.status(), "combine row group chunks"));
  }
  return core::Ok(*std::move(combined));
}

[[nodiscard]] core::Result<std::shared_ptr<arrow::Array>>
chunk_of(const arrow::Table &table, const std::string &name, i64 rows, arrow::Type::type type) {
  const auto column = table.GetColumnByName(name);
  if (column == nullptr || column->num_chunks() != 1 || column->length() != rows ||
      column->chunk(0)->type_id() != type) {
    return core::Err(refused("row group column " + name + " is not one chunk of the file type"));
  }
  return core::Ok(column->chunk(0));
}

// A float32 / float64 cell widened to f64 (null: NaN).
[[nodiscard]] f64 float_at(const arrow::Array &a, i64 i) {
  if (a.IsNull(i)) {
    return kNan;
  }
  if (a.type_id() == arrow::Type::FLOAT) {
    return static_cast<f64>(static_cast<const arrow::FloatArray &>(a).Value(i));
  }
  return static_cast<const arrow::DoubleArray &>(a).Value(i);
}

struct Selected {
  i64 index{}; // row inside the row group
  usize row{}; // panel axis row
  usize line{};
};

struct ScanWindow {
  const RoleAxes &role;
  const ExtendedAxis &axis;
  i64 first{};
  i64 last{}; // min(role's last session, seal - 1)
  i64 seal{};
};

// The rows of one row group that reach the panel, by key only (no value column is decoded here).
// A sealed row is dropped first, by its date alone: it is counted (`sealed`, and the run's
// rows_sealed_dropped) and nothing else of it is looked at, so it reaches no other statistic.
[[nodiscard]] std::vector<Selected> select_rows(const arrow::Date32Array &dates,
                                                const arrow::Int64Array &ids,
                                                const ScanWindow &w, VendorScanStats &stats,
                                                u64 &sealed) {
  std::vector<Selected> out;
  const i64 n = dates.length();
  for (i64 i = 0; i < n; ++i) {
    const i64 day = dates.IsNull(i) ? -1 : static_cast<i64>(dates.Value(i));
    if (day >= w.seal) {
      ++stats.rows_sealed_dropped;
      ++sealed;
      continue;
    }
    if (day < w.first || day > w.last) {
      continue;
    }
    const i64 id = ids.IsNull(i) ? 0 : ids.Value(i);
    const auto line = w.role.column_of(id);
    if (!line) {
      continue;
    }
    const auto &days = w.axis.days;
    const auto it = std::lower_bound(days.begin(), days.end(), day);
    if (it == days.end() || *it != day) {
      ++stats.rows_off_calendar;
      continue;
    }
    out.push_back(Selected{i, static_cast<usize>(it - days.begin()), *line});
  }
  return out;
}

struct ValueColumns {
  std::shared_ptr<arrow::Array> factor;
  std::shared_ptr<arrow::Array> close;
  std::shared_ptr<arrow::Array> volume;
  std::shared_ptr<arrow::Array> shares;
  std::shared_ptr<arrow::Array> open;
  std::shared_ptr<arrow::Array> high;
  std::shared_ptr<arrow::Array> low;
};

[[nodiscard]] core::Result<ValueColumns> value_columns(const arrow::Table &table,
                                                       const arrow::Schema &schema,
                                                       const VendorPanelRequest &request,
                                                       i64 rows) {
  const auto type_of = [&schema](const std::string &name) {
    return schema.GetFieldByName(name)->type()->id();
  };
  ValueColumns out;
  if (request.price) {
    ATX_TRY(out.factor, chunk_of(table, kFactor, rows, arrow::Type::DOUBLE));
    ATX_TRY(out.close, chunk_of(table, kClose, rows, arrow::Type::FLOAT));
    ATX_TRY(out.volume, chunk_of(table, kVolume, rows, arrow::Type::DOUBLE));
  }
  if (request.shares) {
    ATX_TRY(out.shares, chunk_of(table, kShares, rows, arrow::Type::INT64));
  }
  if (request.price_open || request.bars) {
    ATX_TRY(out.open, chunk_of(table, kOpen, rows, type_of(kOpen)));
  }
  if (request.bars) {
    ATX_TRY(out.high, chunk_of(table, kHigh, rows, type_of(kHigh)));
    ATX_TRY(out.low, chunk_of(table, kLow, rows, type_of(kLow)));
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] VendorObservation observation(const ValueColumns &v, const Selected &s) {
  VendorObservation o;
  o.row = s.row;
  o.line = s.line;
  const i64 i = s.index;
  if (v.factor) {
    o.factor = float_at(*v.factor, i);
    const auto &close = static_cast<const arrow::FloatArray &>(*v.close);
    o.close = close.IsNull(i) ? kNanF : close.Value(i);
    o.volume = float_at(*v.volume, i);
  }
  if (v.shares) {
    const auto &shares = static_cast<const arrow::Int64Array &>(*v.shares);
    o.shares = shares.IsNull(i) ? 0 : shares.Value(i);
  }
  if (v.open) {
    o.open = float_at(*v.open, i);
  }
  if (v.high) {
    o.high = float_at(*v.high, i);
    o.low = float_at(*v.low, i);
  }
  return o;
}

struct Scan {
  parquet::arrow::FileReader &reader;
  const arrow::Schema &schema;
  const Leaves &leaves;
  const VendorPanelRequest &request;
  const ScanWindow &window;
  std::vector<int> value_leaves;
};

[[nodiscard]] core::Status scan_row_group(const Scan &scan, int group, i64 rows,
                                          VendorPanel::Assembler &assembler,
                                          VendorScanStats &stats) {
  ATX_TRY(const auto keys, read_row_group(scan.reader, group, {scan.leaves.date, scan.leaves.id}));
  ATX_TRY(const auto dates, chunk_of(*keys, kDate, rows, arrow::Type::DATE32));
  ATX_TRY(const auto ids, chunk_of(*keys, kId, rows, arrow::Type::INT64));
  ++stats.row_groups_keys_decoded;
  stats.rows_keys_decoded += static_cast<u64>(rows);
  u64 sealed = 0;
  // SAFETY: chunk_of checked each chunk's type id (DATE32, INT64).
  const auto selected = select_rows(static_cast<const arrow::Date32Array &>(*dates),
                                    static_cast<const arrow::Int64Array &>(*ids), scan.window,
                                    stats, sealed);
  if (selected.empty()) {
    return core::Ok(); // no surviving row: the value columns are never decoded
  }
  // A column chunk decodes whole: in a group straddling the seal the sealed rows' values are
  // decoded with it. Only the surviving rows' indexes are read below and the chunks are released
  // when this function returns, so a sealed value reaches nothing; the count records that it was
  // decoded.
  stats.rows_sealed_value_decoded += sealed;
  ATX_TRY(const auto values, read_row_group(scan.reader, group, scan.value_leaves));
  ATX_TRY(const auto columns, value_columns(*values, scan.schema, scan.request, rows));
  ++stats.row_groups_values_decoded;
  stats.rows_selected += static_cast<u64>(selected.size());
  for (const Selected &s : selected) {
    assembler.add(observation(columns, s));
  }
  return core::Ok();
}

[[nodiscard]] std::vector<int> value_leaves_of(const Leaves &l) {
  std::vector<int> out;
  for (const int leaf : {l.factor, l.close, l.volume, l.shares, l.open, l.high, l.low}) {
    if (leaf >= 0) {
      out.push_back(leaf);
    }
  }
  return out;
}

// Every row group once: pruned on its tradingDate statistics (on or after the seal, or outside
// the window), else keys first and values only for surviving rows.
[[nodiscard]] core::Status scan_file(const std::filesystem::path &source,
                                     const ScanWindow &window, const VendorPanelRequest &request,
                                     VendorPanel::Assembler &assembler, VendorScanStats &stats) {
  auto opened = arrow::io::ReadableFile::Open(utf8_path(source));
  if (!opened.ok()) {
    return core::Err(from_arrow(opened.status(), "open price_source"));
  }
  auto made = parquet::arrow::OpenFile(*opened, arrow::default_memory_pool());
  if (!made.ok()) {
    return core::Err(from_arrow(made.status(), "open price_source as parquet"));
  }
  const std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(made);
  reader->set_use_threads(false);
  std::shared_ptr<arrow::Schema> schema;
  const arrow::Status got = reader->GetSchema(&schema);
  if (!got.ok()) {
    return core::Err(from_arrow(got, "price_source schema"));
  }
  const std::shared_ptr<parquet::FileMetaData> meta = reader->parquet_reader()->metadata();
  ATX_TRY(const auto leaves, leaves_of(*schema, *meta->schema(), request));
  const Scan scan{*reader, *schema, leaves, request, window, value_leaves_of(leaves)};
  const int groups = meta->num_row_groups();
  stats.row_groups = static_cast<u64>(groups);
  stats.rows_in_file = static_cast<u64>(meta->num_rows());
  for (int g = 0; g < groups; ++g) {
    const auto rg = meta->RowGroup(g);
    const i64 rows = rg->num_rows();
    const auto bounds = date_bounds(*rg, leaves.date);
    if (bounds && bounds->first >= window.seal) {
      ++stats.row_groups_pruned_sealed; // the seal pushed down: never read, never decoded
      stats.rows_in_row_groups_pruned_sealed += static_cast<u64>(rows);
      stats.rows_sealed_dropped += static_cast<u64>(rows);
      continue;
    }
    // A group straddling the seal is never pruned on the window: its keys are decoded so that its
    // sealed rows are counted exactly (its values are decoded only if a row survives).
    if (bounds && bounds->second < window.seal &&
        (bounds->second < window.first || bounds->first > window.last)) {
      ++stats.row_groups_pruned_outside_window;
      stats.rows_in_row_groups_pruned_outside_window += static_cast<u64>(rows);
      continue;
    }
    if (rows <= 0) {
      continue;
    }
    ATX_TRY_VOID(scan_row_group(scan, g, rows, assembler, stats));
  }
  return core::Ok();
}

} // namespace

f32 to_f32(f64 value) noexcept {
  constexpr auto kMaxF32 = static_cast<f64>(std::numeric_limits<f32>::max());
  if (!(std::fabs(value) > kMaxF32)) { // in range, or NaN
    return static_cast<f32>(value);
  }
  // Beyond the largest finite f32: below the midpoint 2^128 - 2^103 IEEE rounds to it, at or above
  // (ties to even: the largest f32 has an odd significand) to infinity.
  constexpr f64 kMidpoint = 0x1.ffffffp+127;
  const f32 magnitude = std::fabs(value) < kMidpoint ? std::numeric_limits<f32>::max()
                                                     : std::numeric_limits<f32>::infinity();
  return std::signbit(value) ? -magnitude : magnitude;
}

void VendorPanelRequest::merge(const VendorPanelRequest &other) noexcept {
  pre_sessions = std::max(pre_sessions, other.pre_sessions);
  lookback_days = std::max(lookback_days, other.lookback_days);
  price = price || other.price;
  shares = shares || other.shares;
  price_open = price_open || other.price_open;
  bars = bars || other.bars;
}

core::Result<VendorPanel::Assembler>
VendorPanel::Assembler::create(ExtendedAxis axis, usize lines, const VendorPanelRequest &request) {
  if (axis.days.empty() || lines == 0 || axis.prefix > axis.days.size() ||
      axis.days.size() > std::numeric_limits<usize>::max() / lines) {
    return core::Err(refused("needs a non-empty axis and at least one line"));
  }
  if ((request.shares || request.price_open) && !request.price) {
    return core::Err(refused("shares and the observation-contract open need the price matrices"));
  }
  Assembler out;
  const usize cells = axis.days.size() * lines;
  out.counts_.assign(cells, u16{0});
  if (request.price) {
    out.factor_.assign(cells, kNan);
    out.close_.assign(cells, kNanF);
  }
  if (request.shares) {
    out.shares_.assign(cells, kNanF);
    out.first_above_.assign(lines, kNeverDay);
  }
  if (request.price_open) {
    out.price_open_.assign(cells, kNanF);
  }
  if (request.bars) {
    out.bar_open_.assign(cells, kNanF);
    out.bar_high_.assign(cells, kNanF);
    out.bar_low_.assign(cells, kNanF);
  }
  out.axis_ = std::move(axis);
  out.lines_ = lines;
  out.request_ = request;
  return core::Ok(std::move(out));
}

void VendorPanel::Assembler::add(const VendorObservation &o) {
  assert(o.row < axis_.days.size() && o.line < lines_);
  const usize c = cell(o.row, o.line);
  if (counts_[c] < std::numeric_limits<u16>::max()) {
    ++counts_[c]; // saturating: a key seen twice is quarantined however often it repeats
  }
  if (request_.price) {
    // The role's present contract (prepare_recent_research.py projection), applied once.
    const bool observed = std::isfinite(o.factor) && o.factor > 0.0 && std::isfinite(o.close) &&
                          o.close > 0.0F && std::isfinite(o.volume) && o.volume >= 0.0;
    factor_[c] = observed ? o.factor : kNan;
    close_[c] = observed ? o.close : kNanF;
    if (request_.shares) {
      const auto s = static_cast<f64>(o.shares);
      const bool above = s > kSharesThousandsRowCeiling;
      if (above) {
        ++shares_rows_above_;
        first_above_[o.line] = std::min(first_above_[o.line], axis_.days[o.row]);
      }
      shares_[c] = (observed && s > 0.0 && !above) ? to_f32(s) : kNanF;
    }
    if (request_.price_open) {
      const bool open_ok = observed && std::isfinite(o.open) && o.open > 0.0;
      price_open_[c] = open_ok ? to_f32(o.open) : kNanF;
    }
  }
  if (request_.bars) {
    bar_open_[c] = to_f32(o.open);
    bar_high_[c] = to_f32(o.high);
    bar_low_[c] = to_f32(o.low);
  }
}

VendorPanel VendorPanel::Assembler::finish(SourceRecord source, VendorScanStats stats) && {
  u64 duplicates = 0;
  for (usize c = 0; c < counts_.size(); ++c) {
    if (counts_[c] <= 1) {
      continue;
    }
    ++duplicates; // every positive duplicate key is quarantined, never picked
    if (!factor_.empty()) {
      factor_[c] = kNan;
      close_[c] = kNanF;
    }
    for (std::vector<f32> *m : {&shares_, &price_open_, &bar_open_, &bar_high_, &bar_low_}) {
      if (!m->empty()) {
        (*m)[c] = kNanF;
      }
    }
  }
  stats.duplicate_keys_quarantined = duplicates;
  stats.shares_rows_above_a9_ceiling = shares_rows_above_;
  stats.shares_lines_withheld_c81 = static_cast<u64>(std::count_if(
      first_above_.begin(), first_above_.end(), [](i64 day) { return day != kNeverDay; }));
  VendorPanel out;
  if (request_.price) {
    out.breaks_ = factor_breaks_v1(factor_, close_, axis_.days, lines_);
    const usize rows = axis_.days.size();
    for (const FactorBreakStep &s : out.breaks_.steps) {
      if (s.action != FactorBreakAction::Repaired) {
        continue;
      }
      for (usize r = s.row; r < rows; ++r) { // F chained: the repaired step divided out from t on
        factor_[r * lines_ + s.line] /= s.k;
      }
    }
    out.gaps_ = KeptGaps(out.breaks_, lines_);
  }
  out.days_ = std::move(axis_.days);
  out.prefix_ = axis_.prefix;
  out.lines_ = lines_;
  out.request_ = request_;
  out.factor_ = std::move(factor_);
  out.close_ = std::move(close_);
  out.shares_ = std::move(shares_);
  out.first_above_ = std::move(first_above_);
  out.price_open_ = std::move(price_open_);
  out.bar_open_ = std::move(bar_open_);
  out.bar_high_ = std::move(bar_high_);
  out.bar_low_ = std::move(bar_low_);
  out.source_ = std::move(source);
  out.stats_ = stats;
  return out;
}

usize VendorPanel::repaired_steps() const noexcept {
  return static_cast<usize>(
      std::count_if(breaks_.steps.begin(), breaks_.steps.end(), [](const FactorBreakStep &s) {
        return s.action == FactorBreakAction::Repaired;
      }));
}

core::Result<VendorPanel> VendorPanel::load(const std::filesystem::path &source,
                                            const RoleAxes &role,
                                            const VendorPanelRequest &request) {
  try {
    if (!request.any()) {
      return core::Err(refused("nothing requested"));
    }
    ATX_TRY(const auto stamp, file_stamp(source));
    ATX_TRY(const auto digest, digest_file(source)); // the build's one SHA-256 pass over the file
    const auto &pinned = role.source_sha256();
    if (!pinned || *pinned != digest.sha256) {
      return core::Err(refused("price_source SHA-256 differs from the role's source_sha256 (not "
                               "the file the role was projected from)"));
    }
    ATX_TRY(const auto hashed, file_stamp(source));
    if (!(hashed == stamp)) {
      return core::Err(core::ErrorCode::IoError, "vendor panel: price_source changed while hashed");
    }
    ATX_TRY(auto axis, extended_axis(role.days(), request.pre_sessions, request.lookback_days));
    ATX_TRY(auto assembler, Assembler::create(axis, role.instruments(), request));
    const ScanWindow window{role, axis, axis.days.front(),
                            std::min(role.days().back(), seal_day() - 1), seal_day()};
    VendorScanStats stats;
    ATX_TRY_VOID(scan_file(source, window, request, assembler, stats));
    ATX_TRY(const auto after, file_stamp(source));
    if (!(after == stamp)) {
      return core::Err(core::ErrorCode::IoError, "vendor panel: price_source changed while read");
    }
    SourceRecord record{record_path(source), digest.bytes, digest.sha256};
    return core::Ok(std::move(assembler).finish(std::move(record), stats));
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal, std::string("vendor panel: ") + e.what());
  }
}

} // namespace atx::engine::research::fields
