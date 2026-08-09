#include "atx/vol/backtest_db.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

#include "atx/vol/detail/backtest_series_columns.hpp"
#include "atx/vol/detail/archive_util.hpp"
#include "atx/vol/detail/writer_lock.hpp" // Task D3: cross-process manifest writer lock
#include "atx/vol/track_key.hpp" // Task D1: make_engine_id() (engine_id incl. economics rev)

namespace atx::vol {
using atx::core::Ok;

namespace {

constexpr std::string_view kDbMetaSection = "db_meta";
constexpr std::string_view kTemplatesSection = "templates";
constexpr std::string_view kTemplateLegsSection = "template_legs";
constexpr std::string_view kSeriesIndexSection = "series_index";
constexpr std::string_view kSeriesMetaSection = "series_meta";
constexpr std::string_view kSourcesSection = "sources";
constexpr std::string_view kCheckpointSection = "checkpoint";
constexpr std::string_view kCheckpointLotsSection = "checkpoint_lots";
constexpr std::string_view kCheckpointSharesSection = "checkpoint_shares";
constexpr std::uint32_t kBacktestDbFormatVersion = 1;
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;
constexpr std::size_t kMaxCatalogIdBytes = 256;
constexpr std::size_t kMaxDisplayNameBytes = 1024;
constexpr std::size_t kMaxSymbolBytes = 32;

[[nodiscard]] std::int64_t wall_clock_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Task D3: every public mutator acquires this BEFORE `std::lock_guard
// lock(*mu_)` -- see detail/writer_lock.hpp's "LOCK ORDERING" doc comment
// for why that order is load-bearing (a cross-process wait taken under `mu_`
// would stall every in-process reader for up to WriterLock::kDefaultTimeout,
// not just other writers). `persist_locked` itself no longer acquires this
// lock -- it is a precondition that the caller already holds it (alongside
// `mu_`) for the whole read-modify-publish window.
[[nodiscard]] Result<detail::WriterLock>
acquire_manifest_writer_lock(const std::filesystem::path &manifest) {
  return detail::WriterLock::acquire(manifest.string() + ".lock");
}

// Task D6: BacktestReaderMark's mechanism -- see that class's own doc
// comment. `<root>/readers/` is a sibling of `partitions/`, never a
// generation-versioned BacktestDb filename, so it is never mistaken for a
// partition candidate by vacuum_unindexed_partitions' own directory scan
// (which only ever iterates partitions/).
[[nodiscard]] std::filesystem::path reader_marks_dir(std::string_view root) {
  return std::filesystem::path(std::string(root)) / "readers";
}

// Best-effort: an empty/unreadable/malformed mark file reads as "cannot
// determine an owner" -- nullopt -- which vacuum's scan (below) treats
// exactly like detail::WriterLock treats an indeterminate owner: NOT
// eligible for stale cleanup, the conservative default (never guess a live
// mark is dead).
[[nodiscard]] std::optional<std::uint64_t> read_reader_mark_pid(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::size_t end = text.size();
  while (end > 0 && (text[end - 1] == '\0' || text[end - 1] == '\n' || text[end - 1] == '\r' ||
                     text[end - 1] == ' ')) {
    --end;
  }
  if (end == 0) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < end; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

[[nodiscard]] constexpr std::int64_t u64_bits(std::uint64_t value) noexcept {
  return std::bit_cast<std::int64_t>(value);
}

[[nodiscard]] constexpr std::uint64_t i64_bits(std::int64_t value) noexcept {
  return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t fnv_bytes(std::uint64_t h, const void *data,
                                      std::size_t size) noexcept {
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (std::size_t i = 0; i < size; ++i) {
    h ^= bytes[i];
    h *= kFnvPrime;
  }
  return h;
}

template <class T> [[nodiscard]] std::uint64_t fnv_value(std::uint64_t h, const T &value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  return fnv_bytes(h, &value, sizeof value);
}

[[nodiscard]] std::uint64_t fnv_string(std::uint64_t h, std::string_view value) noexcept {
  const std::uint64_t size = value.size();
  h = fnv_value(h, size);
  return fnv_bytes(h, value.data(), value.size());
}

[[nodiscard]] std::uint64_t force_nonzero(std::uint64_t h) noexcept {
  return h != 0 ? h : 0x9e3779b97f4a7c15ULL;
}

[[nodiscard]] Result<std::string> canonical_db_symbol(std::string_view symbol) {
  if (symbol.empty() || symbol.size() > kMaxSymbolBytes) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: symbol length must be 1..32");
  }
  for (const char ch : symbol) {
    const bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
    if (!valid) {
      return Err(ErrorCode::InvalidArgument, "backtest_db: symbol has invalid character");
    }
  }
  const std::string canonical = detail::canonicalize_symbol(symbol, kMaxSymbolBytes);
  if (canonical.empty()) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: empty canonical symbol");
  }
  return Ok(canonical);
}

[[nodiscard]] std::string partition_filename_prefix(std::uint64_t template_fingerprint,
                                                    std::string_view template_id,
                                                    std::string_view symbol) {
  std::uint64_t id_hash = fnv_string(kFnvOffset, template_id);
  id_hash = force_nonzero(id_hash);
  std::uint64_t symbol_hash = fnv_string(kFnvOffset, symbol);
  symbol_hash = force_nonzero(symbol_hash);
  std::ostringstream out;
  out << 't' << std::hex << std::setfill('0') << std::setw(16) << template_fingerprint << "-i"
      << std::setw(16) << id_hash << "-s" << std::setw(16) << symbol_hash;
  return out.str();
}

[[nodiscard]] std::string partition_filename(std::uint64_t template_fingerprint,
                                             std::string_view template_id, std::string_view symbol,
                                             std::uint64_t generation) {
  std::ostringstream out;
  out << partition_filename_prefix(template_fingerprint, template_id, symbol) << "-g" << std::hex
      << std::setfill('0') << std::setw(16) << generation << kBacktestDbPartitionExt;
  return out.str();
}

[[nodiscard]] bool safe_partition_filename(std::string_view filename) noexcept {
  if (filename.size() != 78 || filename.front() != 't' || filename.substr(17, 2) != "-i" ||
      filename.substr(35, 2) != "-s" || filename.substr(53, 2) != "-g" ||
      filename.substr(filename.size() - kBacktestDbPartitionExt.size()) !=
          kBacktestDbPartitionExt) {
    return false;
  }
  for (std::size_t i = 1; i < 17; ++i) {
    const char c = filename[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  for (std::size_t i = 19; i < 35; ++i) {
    const char c = filename[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  for (std::size_t i = 37; i < 53; ++i) {
    const char c = filename[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  for (std::size_t i = 55; i < 71; ++i) {
    const char c = filename[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_source_order(std::span<const BacktestSourcePartition> sources) noexcept {
  if (sources.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const BacktestSourcePartition &source = sources[i];
    if (source.date.empty() || source.identity.file_size == 0) {
      return false;
    }
    if (i != 0 && sources[i - 1].date >= source.date) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool valid_side(Side side) noexcept {
  return side == Side::Call || side == Side::Put;
}

struct SectionArena {
  std::vector<std::vector<double>> f64;
  std::vector<std::vector<std::int64_t>> i64;
  std::vector<std::vector<std::uint32_t>> u32;
  std::vector<std::vector<std::string>> strings;
};

[[nodiscard]] RaSectionData make_section(std::string name, RaSectionKind kind, std::uint64_t n_rows,
                                         const std::shared_ptr<SectionArena> &arena) {
  RaSectionData section;
  section.name = std::move(name);
  section.kind = kind;
  section.n_rows = n_rows;
  section.storage = arena;
  return section;
}

void add_f64(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
             std::vector<double> values) {
  arena->f64.push_back(std::move(values));
  section.columns.emplace_back(std::move(name),
                               RaColumnData::of_f64(std::span<const double>(arena->f64.back())));
}

void add_i64(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
             std::vector<std::int64_t> values) {
  arena->i64.push_back(std::move(values));
  section.columns.emplace_back(
      std::move(name), RaColumnData::of_i64(std::span<const std::int64_t>(arena->i64.back())));
}

void add_u32(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
             std::vector<std::uint32_t> values) {
  arena->u32.push_back(std::move(values));
  section.columns.emplace_back(
      std::move(name), RaColumnData::of_u32(std::span<const std::uint32_t>(arena->u32.back())));
}

void add_dict(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
              std::vector<std::string> values) {
  std::vector<std::uint32_t> codes(values.size());
  std::iota(codes.begin(), codes.end(), std::uint32_t{0});
  arena->u32.push_back(std::move(codes));
  arena->strings.push_back(std::move(values));
  section.columns.emplace_back(
      std::move(name), RaColumnData::of_dict(std::span<const std::uint32_t>(arena->u32.back()),
                                             std::span<const std::string>(arena->strings.back())));
}

struct ExpectedColumn {
  std::string_view name;
  RaDType dtype;
};

[[nodiscard]] Status validate_columns(const RaSectionView &section,
                                      std::span<const ExpectedColumn> expected,
                                      bool allow_extra = false) {
  const std::span<const RaColumnDescriptor> columns = section.columns();
  if ((!allow_extra && columns.size() != expected.size()) ||
      (allow_extra && columns.size() < expected.size())) {
    return Err(ErrorCode::ParseError, "backtest_db: custom section column count mismatch");
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const RaColumnDescriptor &column = columns[i];
    if (column.name_len > sizeof column.name ||
        std::string_view(column.name, column.name_len) != expected[i].name ||
        column.dtype != expected[i].dtype) {
      return Err(ErrorCode::ParseError, "backtest_db: custom section schema mismatch");
    }
  }
  return Ok();
}

[[nodiscard]] Result<RaSectionView> required_section(const RunArchive &archive,
                                                     std::string_view name, RaSectionKind kind) {
  auto section = archive.section(name);
  if (!section) {
    return Err(ErrorCode::ParseError,
               "backtest_db: required section missing: " + std::string(name));
  }
  if (section->kind() != kind) {
    return Err(ErrorCode::ParseError, "backtest_db: section kind mismatch");
  }
  return section;
}

[[nodiscard]] bool identity_equal(const ArchiveContentIdentity &a,
                                  const ArchiveContentIdentity &b) noexcept {
  return a == b;
}

[[nodiscard]] bool identity_is_zero(const ArchiveContentIdentity &identity) noexcept {
  return identity == ArchiveContentIdentity{};
}

constexpr ExpectedColumn kDbMetaColumns[] = {
    {"format_version", RaDType::U32}, {"generation", RaDType::I64},
    {"schema_salt", RaDType::I64},    {"created_ts_ns", RaDType::I64},
    {"updated_ts_ns", RaDType::I64},
};

constexpr ExpectedColumn kTemplateColumns[] = {
    {"id", RaDType::DictStr},
    {"name", RaDType::DictStr},
    {"fingerprint", RaDType::I64},
    {"entry_every_n", RaDType::U32},
    {"holding", RaDType::U32},
    {"hedge_kind", RaDType::U32},
    {"hedge_cadence", RaDType::U32},
    {"hedge_band", RaDType::F64},
    {"analytic_greeks", RaDType::U32},
    {"delta_tolerance", RaDType::F64},
    {"query_execution", RaDType::U32},
    {"spread_kind", RaDType::U32},
    {"half_spread_bps", RaDType::F64},
    {"vol_tick", RaDType::F64},
    {"impact_fraction", RaDType::F64},
    {"per_contract_cost", RaDType::F64},
    {"hedge_slippage_bps", RaDType::F64},
    {"settlement", RaDType::U32},
};

constexpr ExpectedColumn kTemplateLegColumns[] = {
    {"template_id", RaDType::DictStr},
    {"ordinal", RaDType::U32},
    {"maturity_kind", RaDType::U32},
    {"year_fraction", RaDType::F64},
    {"calendar_count", RaDType::I64},
    {"expiry_ts_ns", RaDType::I64},
    {"strike_kind", RaDType::U32},
    {"strike_value", RaDType::F64},
    {"side", RaDType::U32},
    {"quantity", RaDType::F64},
    {"multiplier", RaDType::F64},
};

constexpr ExpectedColumn kSeriesIndexColumns[] = {
    {"template_id", RaDType::DictStr},
    {"template_fingerprint", RaDType::I64},
    {"symbol", RaDType::DictStr},
    {"uid", RaDType::U32},
    {"row_count", RaDType::I64},
    {"first_ts_ns", RaDType::I64},
    {"last_ts_ns", RaDType::I64},
    {"source_fingerprint", RaDType::I64},
    {"run_identity_hash", RaDType::I64},
    {"partition_filename", RaDType::DictStr},
    {"partition_file_size", RaDType::I64},
    {"partition_created_ts_ns", RaDType::I64},
    {"partition_header_crc32c", RaDType::U32},
    {"partition_metadata_crc32c", RaDType::U32},
};

constexpr ExpectedColumn kSeriesMetaColumns[] = {
    {"format_version", RaDType::U32},    {"schema_salt", RaDType::I64},
    {"template_id", RaDType::DictStr},   {"template_fingerprint", RaDType::I64},
    {"symbol", RaDType::DictStr},        {"uid", RaDType::U32},
    {"row_count", RaDType::I64},         {"first_ts_ns", RaDType::I64},
    {"last_ts_ns", RaDType::I64},        {"source_fingerprint", RaDType::I64},
    {"run_identity_hash", RaDType::I64}, {"next_cohort", RaDType::U32},
    {"record_every_n", RaDType::U32},
};

constexpr ExpectedColumn kSourceColumns[] = {
    {"date", RaDType::DictStr},        {"file_size", RaDType::I64},
    {"created_ts_ns", RaDType::I64},   {"header_crc32c", RaDType::U32},
    {"metadata_crc32c", RaDType::U32},
};

constexpr ExpectedColumn kCheckpointColumns[] = {
    {"base_ts_ns", RaDType::I64},  {"completed_step_index", RaDType::I64},
    {"next_lot_id", RaDType::I64}, {"cash", RaDType::F64},
    {"nav", RaDType::F64},         {"cumulative_noncash_financing", RaDType::F64},
};

constexpr ExpectedColumn kCheckpointLotColumns[] = {
    {"ordinal", RaDType::U32},  {"id", RaDType::I64},          {"uid", RaDType::U32},
    {"strike", RaDType::F64},   {"residual_t", RaDType::F64},  {"side", RaDType::U32},
    {"quantity", RaDType::F64}, {"multiplier", RaDType::F64},  {"expiry_ts_ns", RaDType::I64},
    {"cohort", RaDType::U32},   {"entry_price", RaDType::F64},
};

constexpr ExpectedColumn kCheckpointShareColumns[] = {
    {"ordinal", RaDType::U32},
    {"uid", RaDType::U32},
    {"shares", RaDType::F64},
};

[[nodiscard]] bool valid_template_id(std::string_view id) noexcept {
  return !id.empty() && id.size() <= kMaxCatalogIdBytes;
}

[[nodiscard]] bool template_key_less(const BacktestStrategyTemplate &a,
                                     const BacktestStrategyTemplate &b) noexcept {
  return a.id < b.id;
}

[[nodiscard]] bool series_key_less(const BacktestSeriesInfo &a,
                                   const BacktestSeriesInfo &b) noexcept {
  if (a.template_id != b.template_id) {
    return a.template_id < b.template_id;
  }
  return a.symbol < b.symbol;
}

[[nodiscard]] bool same_series_key(const BacktestSeriesInfo &a,
                                   const BacktestSeriesInfo &b) noexcept {
  return a.template_id == b.template_id && a.symbol == b.symbol;
}

[[nodiscard]] std::vector<double> *mutable_backtest_column(BacktestResult &result,
                                                           std::string_view name) noexcept {
  if (name == "pnl_total")
    return &result.pnl_total;
  if (name == "pnl_delta")
    return &result.pnl_delta;
  if (name == "pnl_gamma")
    return &result.pnl_gamma;
  if (name == "pnl_vega")
    return &result.pnl_vega;
  if (name == "pnl_vanna")
    return &result.pnl_vanna;
  if (name == "pnl_volga")
    return &result.pnl_volga;
  if (name == "pnl_theta")
    return &result.pnl_theta;
  if (name == "pnl_rho")
    return &result.pnl_rho;
  if (name == "pnl_charm")
    return &result.pnl_charm;
  if (name == "pnl_unexplained")
    return &result.pnl_unexplained;
  if (name == "pnl_settlement")
    return &result.pnl_settlement;
  if (name == "pnl_shares")
    return &result.pnl_shares;
  if (name == "financing")
    return &result.financing;
  if (name == "cost")
    return &result.cost;
  if (name == "nav")
    return &result.nav;
  if (name == "cash")
    return &result.cash;
  if (name == "gross_delta")
    return &result.gross_delta;
  if (name == "gross_gamma")
    return &result.gross_gamma;
  if (name == "gross_vega")
    return &result.gross_vega;
  if (name == "gross_theta")
    return &result.gross_theta;
  if (name == "turnover_notional")
    return &result.turnover_notional;
  if (name == "turnover_vega")
    return &result.turnover_vega;
  if (name == "n_open_lots")
    return &result.n_open_lots;
  if (name == "n_unpriced_lots")
    return &result.n_unpriced_lots;
  if (name == "n_unpriced_greeks")
    return &result.n_unpriced_greeks;
  return nullptr;
}

} // namespace

struct BacktestDb::Snapshot {
  std::uint64_t generation{0};
  std::int64_t created_ts_ns{0};
  std::int64_t updated_ts_ns{0};
  std::vector<BacktestStrategyTemplate> templates;
  std::vector<BacktestSeriesInfo> series;
};

namespace {

[[nodiscard]] Result<std::vector<BacktestStrategyTemplate>>
decode_templates(const RaSectionView &templates_section, const RaSectionView &legs_section);
[[nodiscard]] Result<std::vector<BacktestSeriesInfo>>
decode_series_index(const RaSectionView &section,
                    std::span<const BacktestStrategyTemplate> templates);
[[nodiscard]] std::vector<RaSectionData>
build_manifest_sections(std::uint64_t generation, std::int64_t created_ts_ns,
                        std::int64_t updated_ts_ns,
                        std::span<const BacktestStrategyTemplate> templates,
                        std::span<const BacktestSeriesInfo> series);
[[nodiscard]] Status write_manifest_file(const std::filesystem::path &path,
                                         std::uint64_t generation, std::int64_t created_ts_ns,
                                         std::int64_t updated_ts_ns,
                                         std::span<const BacktestStrategyTemplate> templates,
                                         std::span<const BacktestSeriesInfo> series);
[[nodiscard]] Status validate_checkpoint(const BacktestCheckpoint &checkpoint,
                                         std::uint32_t next_cohort, const BacktestResult &result);
[[nodiscard]] Status validate_series_data(std::uint64_t template_fingerprint, std::uint32_t uid,
                                          const BacktestSeriesData &data);
[[nodiscard]] Result<std::vector<RaSectionData>>
build_partition_sections(const BacktestSeriesInfo &info, const BacktestSeriesData &data);

struct ParsedSeriesMeta {
  std::uint32_t next_cohort{0};
};

[[nodiscard]] Result<ParsedSeriesMeta> decode_series_meta(const RaSectionView &section,
                                                          const BacktestSeriesInfo &info) {
  const Status schema = validate_columns(section, kSeriesMetaColumns);
  if (!schema || section.n_rows() != 1) {
    return Err(ErrorCode::ParseError, "backtest_db: invalid series metadata schema");
  }
  const auto format = section.u32_col("format_version");
  const auto salt = section.i64_col("schema_salt");
  const RaDictColumn template_id = section.dict_col("template_id");
  const auto template_fingerprint = section.i64_col("template_fingerprint");
  const RaDictColumn symbol = section.dict_col("symbol");
  const auto uid = section.u32_col("uid");
  const auto row_count = section.i64_col("row_count");
  const auto first_ts = section.i64_col("first_ts_ns");
  const auto last_ts = section.i64_col("last_ts_ns");
  const auto source_fingerprint = section.i64_col("source_fingerprint");
  const auto run_identity = section.i64_col("run_identity_hash");
  const auto next_cohort = section.u32_col("next_cohort");
  const auto record_every_n = section.u32_col("record_every_n");
  if (format[0] != kBacktestDbFormatVersion || i64_bits(salt[0]) != kBacktestDbEngineSchemaSalt ||
      template_id.at(0) != info.template_id ||
      i64_bits(template_fingerprint[0]) != info.template_fingerprint ||
      symbol.at(0) != info.symbol || uid[0] != info.uid ||
      i64_bits(row_count[0]) != info.row_count || first_ts[0] != info.first_ts_ns ||
      last_ts[0] != info.last_ts_ns || i64_bits(source_fingerprint[0]) != info.source_fingerprint ||
      i64_bits(run_identity[0]) != info.run_identity_hash || record_every_n[0] != 1) {
    return Err(ErrorCode::ParseError, "backtest_db: manifest/partition metadata mismatch");
  }
  return Ok(ParsedSeriesMeta{next_cohort[0]});
}

[[nodiscard]] Result<std::vector<BacktestSourcePartition>>
decode_sources(const RaSectionView &section, const BacktestSeriesInfo &info) {
  const Status schema = validate_columns(section, kSourceColumns);
  if (!schema || section.n_rows() != info.row_count ||
      section.n_rows() > (std::numeric_limits<std::size_t>::max)()) {
    return Err(ErrorCode::ParseError, "backtest_db: invalid source section");
  }
  const std::size_t count = static_cast<std::size_t>(section.n_rows());
  const RaDictColumn dates = section.dict_col("date");
  const auto sizes = section.i64_col("file_size");
  const auto created = section.i64_col("created_ts_ns");
  const auto header_crcs = section.u32_col("header_crc32c");
  const auto metadata_crcs = section.u32_col("metadata_crc32c");
  std::vector<BacktestSourcePartition> sources;
  sources.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    BacktestSourcePartition source;
    source.date = dates.at(i);
    source.identity = ArchiveContentIdentity{i64_bits(sizes[i]), i64_bits(created[i]),
                                             header_crcs[i], metadata_crcs[i]};
    sources.push_back(std::move(source));
  }
  if (!valid_source_order(sources) ||
      backtest_source_fingerprint(sources) != info.source_fingerprint ||
      backtest_series_identity(info.template_fingerprint, info.uid, sources) !=
          info.run_identity_hash) {
    return Err(ErrorCode::ParseError, "backtest_db: source identity mismatch");
  }
  return Ok(std::move(sources));
}

[[nodiscard]] Result<BacktestCheckpoint> decode_checkpoint(const RaSectionView &checkpoint_section,
                                                           const RaSectionView &lots_section,
                                                           const RaSectionView &shares_section) {
  const Status checkpoint_schema = validate_columns(checkpoint_section, kCheckpointColumns);
  const Status lots_schema = validate_columns(lots_section, kCheckpointLotColumns);
  const Status shares_schema = validate_columns(shares_section, kCheckpointShareColumns);
  if (!checkpoint_schema || !lots_schema || !shares_schema || checkpoint_section.n_rows() != 1 ||
      lots_section.n_rows() > (std::numeric_limits<std::size_t>::max)() ||
      shares_section.n_rows() > (std::numeric_limits<std::size_t>::max)()) {
    return Err(ErrorCode::ParseError, "backtest_db: invalid checkpoint section schema");
  }
  BacktestCheckpoint checkpoint;
  checkpoint.base_ts_ns = checkpoint_section.i64_col("base_ts_ns")[0];
  const std::uint64_t completed = i64_bits(checkpoint_section.i64_col("completed_step_index")[0]);
  if (completed > (std::numeric_limits<std::size_t>::max)()) {
    return Err(ErrorCode::ParseError, "backtest_db: checkpoint step index is out of range");
  }
  checkpoint.completed_step_index = static_cast<std::size_t>(completed);
  checkpoint.next_lot_id = i64_bits(checkpoint_section.i64_col("next_lot_id")[0]);
  checkpoint.cash = checkpoint_section.f64_col("cash")[0];
  checkpoint.nav = checkpoint_section.f64_col("nav")[0];
  checkpoint.cumulative_noncash_financing =
      checkpoint_section.f64_col("cumulative_noncash_financing")[0];

  const std::size_t lot_count = static_cast<std::size_t>(lots_section.n_rows());
  const auto lot_ordinals = lots_section.u32_col("ordinal");
  const auto lot_ids = lots_section.i64_col("id");
  const auto lot_uids = lots_section.u32_col("uid");
  const auto lot_strikes = lots_section.f64_col("strike");
  const auto lot_t = lots_section.f64_col("residual_t");
  const auto lot_sides = lots_section.u32_col("side");
  const auto lot_quantities = lots_section.f64_col("quantity");
  const auto lot_multipliers = lots_section.f64_col("multiplier");
  const auto lot_expiries = lots_section.i64_col("expiry_ts_ns");
  const auto lot_cohorts = lots_section.u32_col("cohort");
  const auto lot_entry_prices = lots_section.f64_col("entry_price");
  checkpoint.portfolio.lots.reserve(lot_count);
  for (std::size_t i = 0; i < lot_count; ++i) {
    if (lot_ordinals[i] != i || lot_sides[i] > static_cast<std::uint32_t>(Side::Put)) {
      return Err(ErrorCode::ParseError, "backtest_db: invalid checkpoint lot ordinal/side");
    }
    Lot lot;
    lot.id = i64_bits(lot_ids[i]);
    lot.contract.uid = lot_uids[i];
    lot.contract.K = lot_strikes[i];
    lot.contract.T = lot_t[i];
    lot.contract.side = static_cast<Side>(lot_sides[i]);
    lot.qty = lot_quantities[i];
    lot.multiplier = lot_multipliers[i];
    lot.expiry_ts_ns = lot_expiries[i];
    lot.cohort = lot_cohorts[i];
    lot.entry_price = lot_entry_prices[i];
    checkpoint.portfolio.lots.push_back(lot);
  }

  const std::size_t share_count = static_cast<std::size_t>(shares_section.n_rows());
  const auto share_ordinals = shares_section.u32_col("ordinal");
  const auto share_uids = shares_section.u32_col("uid");
  const auto share_values = shares_section.f64_col("shares");
  checkpoint.hedge_shares.reserve(share_count);
  for (std::size_t i = 0; i < share_count; ++i) {
    if (share_ordinals[i] != i) {
      return Err(ErrorCode::ParseError, "backtest_db: invalid checkpoint share ordinal");
    }
    checkpoint.hedge_shares.push_back(HedgeSharePosition{share_uids[i], share_values[i]});
  }
  return Ok(std::move(checkpoint));
}

[[nodiscard]] Status validate_backtest_view(const RaSectionView &section,
                                            const BacktestSeriesInfo &info,
                                            std::span<const BacktestSourcePartition> sources,
                                            const BacktestCheckpoint &checkpoint,
                                            std::uint32_t next_cohort) {
  if (section.kind() != RaSectionKind::TimeSeries || section.n_rows() != info.row_count ||
      section.n_rows() > (std::numeric_limits<std::size_t>::max)()) {
    return Err(ErrorCode::ParseError, "backtest_db: invalid backtest section geometry");
  }
  const std::span<const RaColumnDescriptor> columns = section.columns();
  if (columns.size() < std::size(kBacktestCols)) {
    return Err(ErrorCode::ParseError, "backtest_db: backtest section is missing columns");
  }
  for (std::size_t i = 0; i < std::size(kBacktestCols); ++i) {
    const RaColumnDescriptor &column = columns[i];
    if (column.name_len > sizeof column.name ||
        std::string_view(column.name, column.name_len) != kBacktestCols[i].name ||
        column.dtype != kBacktestCols[i].dtype) {
      return Err(ErrorCode::ParseError, "backtest_db: backtest registry schema mismatch");
    }
  }
  for (std::size_t i = std::size(kBacktestCols); i < columns.size(); ++i) {
    const RaColumnDescriptor &column = columns[i];
    if (column.name_len == 0 || column.name_len > sizeof column.name ||
        column.dtype != RaDType::F64) {
      return Err(ErrorCode::ParseError, "backtest_db: invalid dynamic backtest column");
    }
  }
  const std::size_t rows = static_cast<std::size_t>(section.n_rows());
  const RaDictColumn dates = section.dict_col("date");
  const auto timestamps = section.i64_col("ts_ns");
  if (dates.size() != rows || timestamps.size() != rows || sources.size() != rows) {
    return Err(ErrorCode::ParseError, "backtest_db: backtest row count mismatch");
  }
  for (std::size_t i = 0; i < rows; ++i) {
    if (dates.at(i).empty() || dates.at(i) != sources[i].date || timestamps[i] <= 0 ||
        (i != 0 && (dates.at(i - 1) >= dates.at(i) || timestamps[i - 1] >= timestamps[i]))) {
      return Err(ErrorCode::ParseError, "backtest_db: backtest rows are not ordered");
    }
  }
  if (timestamps.front() != info.first_ts_ns || timestamps.back() != info.last_ts_ns) {
    return Err(ErrorCode::ParseError, "backtest_db: backtest timestamp/index mismatch");
  }
  for (std::size_t i = 2; i < std::size(kBacktestCols); ++i) {
    const auto values = section.f64_col(kBacktestCols[i].name);
    if (values.size() != rows) {
      return Err(ErrorCode::ParseError, "backtest_db: malformed backtest numeric column");
    }
  }
  const auto cash = section.f64_col("cash");
  const auto nav = section.f64_col("nav");
  const auto n_open_lots = section.f64_col("n_open_lots");
  if (checkpoint.completed_step_index != rows - 1 || checkpoint.base_ts_ns != timestamps.back() ||
      checkpoint.cash != cash.back() || checkpoint.nav != nav.back() ||
      n_open_lots.back() != static_cast<double>(checkpoint.portfolio.lots.size())) {
    return Err(ErrorCode::ParseError, "backtest_db: checkpoint/backtest disagreement");
  }
  BacktestResult minimal;
  minimal.date.reserve(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    minimal.date.emplace_back(dates.at(i));
  }
  minimal.ts_ns.assign(timestamps.begin(), timestamps.end());
  minimal.cash.assign(cash.begin(), cash.end());
  minimal.nav.assign(nav.begin(), nav.end());
  minimal.n_open_lots.assign(n_open_lots.begin(), n_open_lots.end());
  const Status checkpoint_status = validate_checkpoint(checkpoint, next_cohort, minimal);
  if (!checkpoint_status) {
    return Err(ErrorCode::ParseError, checkpoint_status.error().message());
  }
  return Ok();
}

[[nodiscard]] Result<BacktestResult> decode_backtest(const RaSectionView &section) {
  BacktestResult result;
  const std::size_t rows = static_cast<std::size_t>(section.n_rows());
  const RaDictColumn dates = section.dict_col("date");
  result.date.reserve(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    result.date.emplace_back(dates.at(i));
  }
  const auto timestamps = section.i64_col("ts_ns");
  result.ts_ns.assign(timestamps.begin(), timestamps.end());
  for (const BacktestSeriesColumn &column : backtest_series_columns()) {
    const auto values = section.f64_col(column.name);
    std::vector<double> *const destination = mutable_backtest_column(result, column.name);
    if (destination == nullptr) {
      return Err(ErrorCode::Internal, "backtest_db: unknown mandatory result column");
    }
    destination->assign(values.begin(), values.end());
  }
  for (std::size_t i = std::size(kBacktestCols); i < section.columns().size(); ++i) {
    const RaColumnDescriptor &descriptor = section.columns()[i];
    const std::string name(descriptor.name, descriptor.name_len);
    const auto values = section.f64_col(name);
    if (name == "gross_vega_abs") {
      result.gross_vega_abs.assign(values.begin(), values.end());
    } else if (name == "nav_liquidation") {
      result.nav_liquidation.assign(values.begin(), values.end());
    } else {
      result.signals.emplace_back(name, std::vector<double>(values.begin(), values.end()));
    }
  }
  if (result.pnl_total.size() > 1) {
    result.step_pnl_total.assign(result.pnl_total.begin() + 1, result.pnl_total.end());
  }
  return Ok(std::move(result));
}

struct OpenedSeries {
  std::shared_ptr<const RunArchive> archive;
  RaSectionView backtest;
  BacktestCheckpoint checkpoint;
  std::uint32_t next_cohort{0};
  std::vector<BacktestSourcePartition> sources;
};

[[nodiscard]] Result<OpenedSeries> open_series_partition(const std::filesystem::path &path,
                                                         const BacktestSeriesInfo &info) {
  auto opened = RunArchive::open_mapped(path.string());
  if (!opened) {
    return tl::unexpected<Error>(std::move(opened).error());
  }
  auto archive = std::make_shared<RunArchive>(std::move(*opened));
  if (!identity_equal(archive->identity(), info.partition_identity) ||
      archive->header().run_identity_hash != info.run_identity_hash || archive->count() != 6) {
    return Err(ErrorCode::ParseError, "backtest_db: partition identity/index mismatch");
  }
  const Status integrity = archive->validate_all();
  if (!integrity) {
    return tl::unexpected<Error>(integrity.error());
  }
  auto backtest = required_section(*archive, "backtest", RaSectionKind::TimeSeries);
  auto meta = required_section(*archive, kSeriesMetaSection, RaSectionKind::ScalarKV);
  auto sources = required_section(*archive, kSourcesSection, RaSectionKind::SubTable);
  auto checkpoint = required_section(*archive, kCheckpointSection, RaSectionKind::ScalarKV);
  auto lots = required_section(*archive, kCheckpointLotsSection, RaSectionKind::SubTable);
  auto shares = required_section(*archive, kCheckpointSharesSection, RaSectionKind::SubTable);
  if (!backtest || !meta || !sources || !checkpoint || !lots || !shares) {
    return Err(ErrorCode::ParseError, "backtest_db: partition section directory mismatch");
  }
  auto parsed_meta = decode_series_meta(*meta, info);
  auto parsed_sources = decode_sources(*sources, info);
  auto parsed_checkpoint = decode_checkpoint(*checkpoint, *lots, *shares);
  if (!parsed_meta || !parsed_sources || !parsed_checkpoint) {
    return Err(ErrorCode::ParseError, "backtest_db: invalid partition metadata");
  }
  const Status valid_view = validate_backtest_view(*backtest, info, *parsed_sources,
                                                   *parsed_checkpoint, parsed_meta->next_cohort);
  if (!valid_view) {
    return tl::unexpected<Error>(valid_view.error());
  }
  OpenedSeries output;
  output.archive = std::move(archive);
  output.backtest = std::move(*backtest);
  output.checkpoint = std::move(*parsed_checkpoint);
  output.next_cohort = parsed_meta->next_cohort;
  output.sources = std::move(*parsed_sources);
  return Ok(std::move(output));
}

} // namespace

Result<BacktestDb> BacktestDb::create(std::string_view root) {
  if (root.empty()) {
    return Err(ErrorCode::InvalidArgument, "BacktestDb::create: empty root");
  }
  const std::filesystem::path root_path{std::string(root)};
  const std::filesystem::path manifest = root_path / kBacktestDbManifestName;
  std::error_code ec;
  const bool manifest_exists = std::filesystem::exists(manifest, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "BacktestDb::create: cannot inspect manifest");
  }
  if (manifest_exists) {
    return Err(ErrorCode::AlreadyExists, "BacktestDb::create: manifest already exists");
  }
  std::filesystem::create_directories(root_path / kBacktestDbPartitionDir, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "BacktestDb::create: cannot create database directories");
  }
  const std::int64_t now = wall_clock_ns();
  const std::vector<BacktestStrategyTemplate> templates;
  const std::vector<BacktestSeriesInfo> series;
  const Status written = write_manifest_file(manifest, 1, now, now, templates, series);
  if (!written) {
    return tl::unexpected<Error>(written.error());
  }
  return open(root);
}

Result<BacktestDb> BacktestDb::open(std::string_view root) {
  if (root.empty()) {
    return Err(ErrorCode::InvalidArgument, "BacktestDb::open: empty root");
  }
  const std::filesystem::path root_path{std::string(root)};
  const std::filesystem::path manifest = root_path / kBacktestDbManifestName;
  std::error_code ec;
  const bool exists = std::filesystem::exists(manifest, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "BacktestDb::open: cannot inspect manifest");
  }
  if (!exists) {
    return Err(ErrorCode::NotFound, "BacktestDb::open: manifest not found");
  }
  auto snapshot = read_manifest(manifest);
  if (!snapshot) {
    return tl::unexpected<Error>(std::move(snapshot).error());
  }
  BacktestDb db;
  db.root_ = root_path.string();
  db.mu_ = std::make_unique<std::mutex>();
  db.snapshot_ = std::move(*snapshot);
  return Ok(std::move(db));
}

std::filesystem::path BacktestDb::manifest_path() const {
  return std::filesystem::path(root_) / kBacktestDbManifestName;
}

std::filesystem::path BacktestDb::partition_path(std::string_view filename) const {
  return std::filesystem::path(root_) / kBacktestDbPartitionDir / std::string(filename);
}

std::uint64_t BacktestDb::generation() const {
  const std::lock_guard lock(*mu_);
  return snapshot_->generation;
}

Status BacktestDb::refresh() {
  const std::lock_guard lock(*mu_);
  auto replacement = read_manifest(manifest_path());
  if (!replacement) {
    return tl::unexpected<Error>(std::move(replacement).error());
  }
  if ((*replacement)->generation < snapshot_->generation) {
    return Err(ErrorCode::ParseError, "BacktestDb::refresh: manifest generation rolled back");
  }
  if ((*replacement)->generation > snapshot_->generation) {
    snapshot_ = std::move(*replacement);
  }
  return Ok();
}

Result<BacktestReaderMark> BacktestReaderMark::acquire(std::string_view root) {
  if (root.empty()) {
    return Err(ErrorCode::InvalidArgument, "BacktestReaderMark::acquire: empty root");
  }
  const std::filesystem::path dir = reader_marks_dir(root);
  std::error_code mkdir_ec;
  std::filesystem::create_directories(dir, mkdir_ec);
  if (mkdir_ec) {
    return Err(ErrorCode::IoError,
               "BacktestReaderMark::acquire: cannot create readers directory: " + mkdir_ec.message());
  }

  // Unique by construction (pid + monotonic in-process counter + a
  // steady_clock tick) -- no CREATE_NEW/O_EXCL exclusivity needed the way
  // detail::WriterLock needs it, since this is a many-holder registration,
  // not mutual exclusion: an extremely improbable name collision would only
  // ever occur between two marks from the SAME process, and losing one to
  // the other's overwrite still leaves at least one live mark, which is all
  // vacuum's guard below checks for.
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t pid = detail::current_process_id();
  const auto tick = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string filename = "reader-" + std::to_string(pid) + "-" + std::to_string(tick) + "-" +
                               std::to_string(counter.fetch_add(1)) + ".mark";
  const std::string mark_path = (dir / filename).string();

  // Review finding (Important, fix-round 1): atomic-publish discipline
  // (detail/archive_util.hpp), same as every other durable lakehouse write
  // -- catalog.sqlite is the ONE documented exception, and this is not it.
  // A plain std::ofstream(mark_path, trunc) makes the mark DISCOVERABLE at
  // its final, scanned-for name the instant the file is created -- empty,
  // before the pid is written. vacuum's scan below treats an unparseable/
  // empty mark file as "cannot determine an owner", i.e. NOT a live
  // blocker (the same conservative default detail::WriterLock's own
  // stale-owner probe uses) -- so a mark caught mid-creation would be
  // invisible to a racing vacuum for the whole window between create and
  // the pid write landing, exactly when a reader that just called
  // mark_reader() per this class's own doc contract needs it to be seen.
  // Reserve a unique temp file, write+flush the pid into THAT, then
  // atomically rename onto the mark's real name: vacuum's scan only ever
  // observes the mark file fully formed or not present at all, never
  // partially written.
  ATX_TRY(const std::string tmp_path, detail::reserve_unique_publish_temp_file(mark_path));

  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::error_code rm_ec;
      std::filesystem::remove(tmp_path, rm_ec);
      return Err(ErrorCode::IoError, "BacktestReaderMark::acquire: cannot open temp mark file");
    }
    out << pid;
    out.flush();
    if (!out) {
      std::error_code rm_ec;
      std::filesystem::remove(tmp_path, rm_ec);
      return Err(ErrorCode::IoError, "BacktestReaderMark::acquire: cannot write temp mark file");
    }
  }

  ATX_TRY_VOID(detail::flush_and_publish_file(tmp_path, mark_path));

  BacktestReaderMark mark;
  mark.mark_path_ = mark_path;
  return Ok(std::move(mark));
}

BacktestReaderMark::~BacktestReaderMark() { release(); }

BacktestReaderMark::BacktestReaderMark(BacktestReaderMark &&other) noexcept
    : mark_path_{std::move(other.mark_path_)} {
  other.mark_path_.clear();
}

BacktestReaderMark &BacktestReaderMark::operator=(BacktestReaderMark &&other) noexcept {
  if (this != &other) {
    release();
    mark_path_ = std::move(other.mark_path_);
    other.mark_path_.clear();
  }
  return *this;
}

void BacktestReaderMark::release() noexcept {
  if (mark_path_.empty()) {
    return;
  }
  std::error_code ec;
  // Bounded best-effort retry, same tolerance detail::WriterLock::release()
  // documents: a concurrent liveness-probe reader (vacuum's own scan) can
  // hold a transient open against this file on Windows.
  for (int attempt = 0; attempt < 5; ++attempt) {
    std::filesystem::remove(mark_path_, ec);
    if (!ec) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  mark_path_.clear();
}

Result<BacktestReaderMark> BacktestDb::mark_reader() const { return BacktestReaderMark::acquire(root_); }

Result<std::size_t> BacktestDb::vacuum_unindexed_partitions() {
  const std::lock_guard lock(*mu_);

  // Task D6: refuse OUTRIGHT -- before even reading the manifest, let alone
  // enumerating or removing a candidate -- while a live reader mark is
  // registered. See BacktestReaderMark's own doc comment for the mechanism
  // and vacuum_unindexed_partitions' header doc comment for what this does
  // and does not protect.
  {
    const std::filesystem::path readers_dir = reader_marks_dir(root_);
    std::error_code exists_ec;
    if (std::filesystem::exists(readers_dir, exists_ec) && !exists_ec) {
      std::error_code list_ec;
      std::filesystem::directory_iterator it(readers_dir, list_ec);
      const std::filesystem::directory_iterator dir_end;
      for (; !list_ec && it != dir_end; it.increment(list_ec)) {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec) || type_ec) {
          continue;
        }
        const std::optional<std::uint64_t> pid = read_reader_mark_pid(it->path());
        if (!pid.has_value()) {
          // Cannot determine an owner -- conservative default, same as
          // detail::WriterLock's own stale-takeover probe: NOT eligible for
          // cleanup, but also not treated as a live blocker (an empty/
          // unreadable file is not evidence of a live reader either).
          continue;
        }
        if (detail::process_alive(*pid)) {
          return Err(ErrorCode::Unavailable,
                     "BacktestDb::vacuum_unindexed_partitions: refusing -- a live reader mark is "
                     "registered at " +
                         it->path().string());
        }
        // Confirmed-dead owner -- opportunistic cleanup, mirroring
        // detail::WriterLock's own stale-owner takeover, so a mark left by a
        // crashed reader does not block vacuum forever.
        std::error_code rm_ec;
        std::filesystem::remove(it->path(), rm_ec);
      }
    }
  }

  auto replacement = read_manifest(manifest_path());
  if (!replacement) {
    return tl::unexpected<Error>(std::move(replacement).error());
  }
  if ((*replacement)->generation < snapshot_->generation) {
    return Err(ErrorCode::ParseError,
               "BacktestDb::vacuum_unindexed_partitions: manifest generation rolled back");
  }
  if ((*replacement)->generation > snapshot_->generation) {
    snapshot_ = std::move(*replacement);
  }

  std::unordered_set<std::string> referenced;
  referenced.reserve(snapshot_->series.size());
  for (const BacktestSeriesInfo &info : snapshot_->series) {
    referenced.insert(info.partition_filename);
  }

  const std::filesystem::path directory = std::filesystem::path(root_) / kBacktestDbPartitionDir;
  std::error_code ec;
  const std::filesystem::file_status directory_status =
      std::filesystem::symlink_status(directory, ec);
  if (ec || !std::filesystem::is_directory(directory_status) ||
      std::filesystem::is_symlink(directory_status)) {
    return Err(ErrorCode::IoError,
               "BacktestDb::vacuum_unindexed_partitions: invalid partitions directory");
  }

  std::vector<std::filesystem::path> candidates;
  std::filesystem::directory_iterator it(directory, ec);
  const std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::file_status status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
      continue;
    }
    const std::string filename = it->path().filename().string();
    if (safe_partition_filename(filename) && !referenced.contains(filename)) {
      candidates.push_back(it->path());
    }
  }
  if (ec) {
    return Err(ErrorCode::IoError,
               "BacktestDb::vacuum_unindexed_partitions: cannot enumerate partitions");
  }
  std::sort(candidates.begin(), candidates.end());

  std::size_t removed = 0;
  for (const std::filesystem::path &candidate : candidates) {
    const std::filesystem::file_status status = std::filesystem::symlink_status(candidate, ec);
    if (ec || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
      return Err(ErrorCode::IoError,
                 "BacktestDb::vacuum_unindexed_partitions: candidate changed during vacuum");
    }
    const bool did_remove = std::filesystem::remove(candidate, ec);
    if (ec || !did_remove) {
      return Err(ErrorCode::IoError,
                 "BacktestDb::vacuum_unindexed_partitions: cannot remove candidate");
    }
    ++removed;
  }
  return Ok(removed);
}

// PRECONDITION (Task D3): the caller already holds a `WriterLock` on this
// manifest's `<path>.lock` (acquired via `acquire_manifest_writer_lock`,
// BEFORE `mu_` -- see detail/writer_lock.hpp's "LOCK ORDERING" doc comment)
// for the duration of this call, in addition to `mu_` itself. This function
// does not acquire or check either lock -- it trusts the caller, exactly as
// it already trusted the caller to hold `mu_` before this task existed.
// `mu_` above only serializes mutations through THIS BacktestDb instance --
// two separate handles on the same root (two handles in one process, or one
// handle each in two processes) do not share a `mu_`, so without the
// WriterLock they could both read `snapshot_->generation == N`, both compute
// N+1, and both publish: the LAST atomic rename wins and the other writer's
// update vanishes with no error.
Status BacktestDb::persist_locked(std::vector<BacktestStrategyTemplate> templates,
                                  std::vector<BacktestSeriesInfo> series) {
  std::sort(templates.begin(), templates.end(), template_key_less);
  std::sort(series.begin(), series.end(), series_key_less);
  const std::filesystem::path manifest = manifest_path();

  // Re-read the on-disk manifest UNDER the lock: another writer may have
  // advanced the generation since `snapshot_` was last synced (this
  // process's own last write, or -- the case the lock exists for -- a
  // DIFFERENT writer's). `templates`/`series` above were computed by the
  // caller (e.g. register_template) against the OLD `snapshot_`, so if the
  // generation moved, publishing them now would silently overwrite whatever
  // the other writer just committed with content that never accounted for
  // it. Fail cleanly instead -- Unavailable, not a corrupted or lost write --
  // and resync `snapshot_` so the caller's very next retry decides against
  // fresh state.
  auto current = read_manifest(manifest);
  if (!current) {
    return Err(current.error());
  }
  if ((*current)->generation != snapshot_->generation) {
    snapshot_ = std::move(*current);
    return Err(ErrorCode::Unavailable,
               "BacktestDb::persist_locked: manifest was concurrently advanced by another "
               "writer; retry against the refreshed snapshot");
  }

  const std::uint64_t next_generation = snapshot_->generation + 1;
  if (next_generation == 0) {
    return Err(ErrorCode::OutOfRange, "BacktestDb: manifest generation overflow");
  }
  const std::int64_t updated = wall_clock_ns();
  const Status written = write_manifest_file(manifest, next_generation, snapshot_->created_ts_ns,
                                             updated, templates, series);
  if (!written) {
    return written;
  }
  auto replacement = read_manifest(manifest);
  if (!replacement || (*replacement)->generation != next_generation) {
    return Err(ErrorCode::ParseError, "BacktestDb: published manifest failed validation");
  }
  snapshot_ = std::move(*replacement);
  return Ok();
}

Status BacktestDb::register_template(const BacktestStrategyTemplate &strategy_template) {
  const Status valid = validate_backtest_template(strategy_template);
  if (!valid) {
    return valid;
  }
  if (!valid_template_id(strategy_template.id) ||
      strategy_template.name.size() > kMaxDisplayNameBytes) {
    return Err(ErrorCode::InvalidArgument, "BacktestDb::register_template: invalid metadata");
  }
  const std::uint64_t fingerprint = fingerprint_backtest_template(strategy_template);
  if (fingerprint == 0) {
    return Err(ErrorCode::InvalidArgument,
               "BacktestDb::register_template: invalid economic fingerprint");
  }
  // Lock order (detail/writer_lock.hpp): WriterLock BEFORE mu_, always --
  // acquired here even though the id-already-registered path below may turn
  // out to need no write, because that can only be known after `mu_` is
  // taken, and the file lock must already be held by then.
  auto file_lock = acquire_manifest_writer_lock(manifest_path());
  if (!file_lock) {
    return Err(file_lock.error());
  }
  const std::lock_guard lock(*mu_);
  const auto it = std::lower_bound(
      snapshot_->templates.begin(), snapshot_->templates.end(), strategy_template.id,
      [](const BacktestStrategyTemplate &value, std::string_view id) { return value.id < id; });
  if (it != snapshot_->templates.end() && it->id == strategy_template.id) {
    if (fingerprint_backtest_template(*it) == fingerprint) {
      return Ok();
    }
    return Err(ErrorCode::AlreadyExists,
               "BacktestDb::register_template: id already has different economics");
  }
  std::vector<BacktestStrategyTemplate> templates = snapshot_->templates;
  templates.push_back(strategy_template);
  return persist_locked(std::move(templates), snapshot_->series);
}

std::vector<BacktestStrategyTemplate> BacktestDb::templates() const {
  const std::lock_guard lock(*mu_);
  return snapshot_->templates;
}

Result<BacktestStrategyTemplate> BacktestDb::find_template(std::string_view template_id) const {
  const std::lock_guard lock(*mu_);
  const auto it = std::lower_bound(
      snapshot_->templates.begin(), snapshot_->templates.end(), template_id,
      [](const BacktestStrategyTemplate &value, std::string_view id) { return value.id < id; });
  if (it == snapshot_->templates.end() || it->id != template_id) {
    return Err(ErrorCode::NotFound, "BacktestDb::find_template: template not found");
  }
  return Ok(*it);
}

std::vector<BacktestSeriesInfo> BacktestDb::series() const {
  const std::lock_guard lock(*mu_);
  return snapshot_->series;
}

Result<BacktestSeriesInfo> BacktestDb::find_series(std::string_view template_id,
                                                   std::string_view symbol) const {
  auto canonical = canonical_db_symbol(symbol);
  if (!canonical) {
    return tl::unexpected<Error>(std::move(canonical).error());
  }
  BacktestSeriesInfo key;
  key.template_id = template_id;
  key.symbol = *canonical;
  const std::lock_guard lock(*mu_);
  const auto it =
      std::lower_bound(snapshot_->series.begin(), snapshot_->series.end(), key, series_key_less);
  if (it == snapshot_->series.end() || !same_series_key(*it, key)) {
    return Err(ErrorCode::NotFound, "BacktestDb::find_series: series not found");
  }
  return Ok(*it);
}

Status BacktestDb::write_series(std::string_view template_id, std::string_view symbol,
                                std::uint32_t uid, const BacktestSeriesData &data) {
  auto strategy_template = find_template(template_id);
  if (!strategy_template) {
    return tl::unexpected<Error>(std::move(strategy_template).error());
  }
  auto canonical = canonical_db_symbol(symbol);
  if (!canonical) {
    return tl::unexpected<Error>(std::move(canonical).error());
  }
  BacktestSeriesInfo info;
  info.template_id = std::string(template_id);
  info.template_fingerprint = fingerprint_backtest_template(*strategy_template);
  info.symbol = std::move(*canonical);
  info.uid = uid;
  info.row_count = data.backtest.size();
  if (!data.backtest.ts_ns.empty()) {
    info.first_ts_ns = data.backtest.ts_ns.front();
    info.last_ts_ns = data.backtest.ts_ns.back();
  }
  info.source_fingerprint = backtest_source_fingerprint(data.sources);
  info.run_identity_hash = backtest_series_identity(info.template_fingerprint, uid, data.sources);
  auto current = find_series(template_id, info.symbol);
  if (current) {
    info.partition_filename = current->partition_filename;
    info.partition_identity = current->partition_identity;
  } else if (current.error().code() != ErrorCode::NotFound) {
    return tl::unexpected<Error>(std::move(current).error());
  }
  return write_series(info, data);
}

Status BacktestDb::write_series(const BacktestSeriesInfo &supplied,
                                const BacktestSeriesData &data) {
  auto canonical = canonical_db_symbol(supplied.symbol);
  if (!canonical || *canonical != supplied.symbol) {
    return Err(ErrorCode::InvalidArgument, "BacktestDb::write_series: symbol is not canonical");
  }
  const Status valid = validate_series_data(supplied.template_fingerprint, supplied.uid, data);
  if (!valid) {
    return valid;
  }
  BacktestSeriesInfo expected = supplied;
  expected.row_count = data.backtest.size();
  expected.first_ts_ns = data.backtest.ts_ns.front();
  expected.last_ts_ns = data.backtest.ts_ns.back();
  expected.source_fingerprint = backtest_source_fingerprint(data.sources);
  expected.run_identity_hash =
      backtest_series_identity(supplied.template_fingerprint, supplied.uid, data.sources);
  expected.partition_filename.clear();
  expected.partition_identity = {};
  if (supplied.row_count != expected.row_count || supplied.first_ts_ns != expected.first_ts_ns ||
      supplied.last_ts_ns != expected.last_ts_ns ||
      supplied.source_fingerprint != expected.source_fingerprint ||
      supplied.run_identity_hash != expected.run_identity_hash) {
    return Err(ErrorCode::InvalidArgument,
               "BacktestDb::write_series: caller metadata disagrees with data");
  }

  // Lock order (detail/writer_lock.hpp): WriterLock BEFORE mu_. This
  // critical section computes `next_generation` from `snapshot_` and writes
  // a partition file NAMED after it (below) before ever reaching
  // persist_locked -- both the generation read and the partition file's
  // name are cross-process-racy exactly like persist_locked's own re-read,
  // so the file lock has to span this whole section, not just the tail.
  auto file_lock = acquire_manifest_writer_lock(manifest_path());
  if (!file_lock) {
    return Err(file_lock.error());
  }
  const std::lock_guard lock(*mu_);
  const auto template_it = std::lower_bound(
      snapshot_->templates.begin(), snapshot_->templates.end(), supplied.template_id,
      [](const BacktestStrategyTemplate &value, std::string_view id) { return value.id < id; });
  if (template_it == snapshot_->templates.end() || template_it->id != supplied.template_id) {
    return Err(ErrorCode::NotFound, "BacktestDb::write_series: template not registered");
  }
  if (fingerprint_backtest_template(*template_it) != supplied.template_fingerprint) {
    return Err(ErrorCode::InvalidArgument,
               "BacktestDb::write_series: template fingerprint mismatch");
  }
  const auto current_it = std::lower_bound(snapshot_->series.begin(), snapshot_->series.end(),
                                           supplied, series_key_less);
  const bool exists =
      current_it != snapshot_->series.end() && same_series_key(*current_it, supplied);
  if ((!exists &&
       (!supplied.partition_filename.empty() || !identity_is_zero(supplied.partition_identity))) ||
      (exists && (supplied.partition_filename != current_it->partition_filename ||
                  !identity_equal(supplied.partition_identity, current_it->partition_identity)))) {
    return Err(ErrorCode::InvalidArgument,
               "BacktestDb::write_series: stale or unexpected partition version");
  }
  const std::uint64_t next_generation = snapshot_->generation + 1;
  if (next_generation == 0) {
    return Err(ErrorCode::OutOfRange, "BacktestDb::write_series: generation overflow");
  }
  expected.partition_filename = partition_filename(
      expected.template_fingerprint, expected.template_id, expected.symbol, next_generation);

  auto sections = build_partition_sections(expected, data);
  if (!sections) {
    return tl::unexpected<Error>(std::move(sections).error());
  }
  const std::filesystem::path path = partition_path(expected.partition_filename);
  const Status written = write_run_archive_file(path.string(), *sections, expected.last_ts_ns,
                                                expected.run_identity_hash);
  if (!written) {
    return written;
  }
  auto partition = RunArchive::open_file(path.string());
  if (!partition || partition->header().run_identity_hash != expected.run_identity_hash) {
    return Err(ErrorCode::ParseError,
               "BacktestDb::write_series: published partition failed validation");
  }
  const Status integrity = partition->validate_all();
  if (!integrity) {
    return integrity;
  }
  expected.partition_identity = partition->identity();
  std::vector<BacktestSeriesInfo> series = snapshot_->series;
  const auto key_it = std::lower_bound(series.begin(), series.end(), expected, series_key_less);
  if (key_it != series.end() && same_series_key(*key_it, expected)) {
    *key_it = expected;
  } else {
    series.insert(key_it, expected);
  }
  return persist_locked(snapshot_->templates, std::move(series));
}

Result<MappedBacktestView> BacktestDb::map_backtest(std::string_view template_id,
                                                    std::string_view symbol) const {
  auto info = find_series(template_id, symbol);
  if (!info) {
    return tl::unexpected<Error>(std::move(info).error());
  }
  auto opened = open_series_partition(partition_path(info->partition_filename), *info);
  if (!opened) {
    return tl::unexpected<Error>(std::move(opened).error());
  }
  MappedBacktestView mapped;
  mapped.archive = std::move(opened->archive);
  mapped.view = std::move(opened->backtest);
  return Ok(std::move(mapped));
}

Result<BacktestSeriesData> BacktestDb::load_series(std::string_view template_id,
                                                   std::string_view symbol) const {
  auto info = find_series(template_id, symbol);
  if (!info) {
    return tl::unexpected<Error>(std::move(info).error());
  }
  auto opened = open_series_partition(partition_path(info->partition_filename), *info);
  if (!opened) {
    return tl::unexpected<Error>(std::move(opened).error());
  }
  auto backtest = decode_backtest(opened->backtest);
  if (!backtest) {
    return tl::unexpected<Error>(std::move(backtest).error());
  }
  BacktestSeriesData data;
  data.backtest = std::move(*backtest);
  data.checkpoint = std::move(opened->checkpoint);
  data.next_cohort = opened->next_cohort;
  data.sources = std::move(opened->sources);
  const Status valid = validate_series_data(info->template_fingerprint, info->uid, data);
  if (!valid) {
    return Err(ErrorCode::ParseError, valid.error().message());
  }
  return Ok(std::move(data));
}

Result<std::shared_ptr<const BacktestDb::Snapshot>>
BacktestDb::read_manifest(const std::filesystem::path &path) {
  auto archive = RunArchive::open_file(path.string());
  if (!archive) {
    return tl::unexpected<Error>(std::move(archive).error());
  }
  const Status integrity = archive->validate_all();
  if (!integrity) {
    return tl::unexpected<Error>(integrity.error());
  }
  if (archive->count() != 4) {
    return Err(ErrorCode::ParseError, "backtest_db: manifest section count mismatch");
  }
  auto meta = required_section(*archive, kDbMetaSection, RaSectionKind::ScalarKV);
  auto templates_section = required_section(*archive, kTemplatesSection, RaSectionKind::SubTable);
  auto legs_section = required_section(*archive, kTemplateLegsSection, RaSectionKind::SubTable);
  auto series_section = required_section(*archive, kSeriesIndexSection, RaSectionKind::SubTable);
  if (!meta || !templates_section || !legs_section || !series_section) {
    return Err(ErrorCode::ParseError, "backtest_db: invalid manifest section directory");
  }
  const Status meta_schema = validate_columns(*meta, kDbMetaColumns);
  if (!meta_schema || meta->n_rows() != 1) {
    return Err(ErrorCode::ParseError, "backtest_db: invalid manifest metadata");
  }
  const auto format_version = meta->u32_col("format_version");
  const auto generation = meta->i64_col("generation");
  const auto schema_salt = meta->i64_col("schema_salt");
  const auto created_ts = meta->i64_col("created_ts_ns");
  const auto updated_ts = meta->i64_col("updated_ts_ns");
  if (format_version[0] != kBacktestDbFormatVersion ||
      i64_bits(schema_salt[0]) != kBacktestDbEngineSchemaSalt || i64_bits(generation[0]) == 0 ||
      created_ts[0] <= 0 || updated_ts[0] < created_ts[0]) {
    return Err(ErrorCode::ParseError, "backtest_db: unsupported manifest metadata");
  }
  auto decoded_templates = decode_templates(*templates_section, *legs_section);
  if (!decoded_templates) {
    return tl::unexpected<Error>(std::move(decoded_templates).error());
  }
  auto decoded_series = decode_series_index(*series_section, *decoded_templates);
  if (!decoded_series) {
    return tl::unexpected<Error>(std::move(decoded_series).error());
  }
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->generation = i64_bits(generation[0]);
  snapshot->created_ts_ns = created_ts[0];
  snapshot->updated_ts_ns = updated_ts[0];
  snapshot->templates = std::move(*decoded_templates);
  snapshot->series = std::move(*decoded_series);
  return Ok(std::shared_ptr<const Snapshot>(std::move(snapshot)));
}

namespace {

[[nodiscard]] Status write_manifest_file(const std::filesystem::path &path,
                                         std::uint64_t generation, std::int64_t created_ts_ns,
                                         std::int64_t updated_ts_ns,
                                         std::span<const BacktestStrategyTemplate> templates,
                                         std::span<const BacktestSeriesInfo> series) {
  std::vector<RaSectionData> sections =
      build_manifest_sections(generation, created_ts_ns, updated_ts_ns, templates, series);
  std::uint64_t identity = fnv_value(kFnvOffset, kBacktestDbEngineSchemaSalt);
  identity = fnv_value(identity, generation);
  identity = fnv_value(identity, static_cast<std::uint64_t>(templates.size()));
  identity = fnv_value(identity, static_cast<std::uint64_t>(series.size()));
  return write_run_archive_file(path.string(), sections, updated_ts_ns, force_nonzero(identity));
}

[[nodiscard]] RaSectionData build_series_meta(const BacktestSeriesInfo &info,
                                              std::uint32_t next_cohort) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kSeriesMetaSection), RaSectionKind::ScalarKV, 1, arena);
  add_u32(section, arena, "format_version", {kBacktestDbFormatVersion});
  add_i64(section, arena, "schema_salt", {u64_bits(kBacktestDbEngineSchemaSalt)});
  add_dict(section, arena, "template_id", {info.template_id});
  add_i64(section, arena, "template_fingerprint", {u64_bits(info.template_fingerprint)});
  add_dict(section, arena, "symbol", {info.symbol});
  add_u32(section, arena, "uid", {info.uid});
  add_i64(section, arena, "row_count", {u64_bits(info.row_count)});
  add_i64(section, arena, "first_ts_ns", {info.first_ts_ns});
  add_i64(section, arena, "last_ts_ns", {info.last_ts_ns});
  add_i64(section, arena, "source_fingerprint", {u64_bits(info.source_fingerprint)});
  add_i64(section, arena, "run_identity_hash", {u64_bits(info.run_identity_hash)});
  add_u32(section, arena, "next_cohort", {next_cohort});
  add_u32(section, arena, "record_every_n", {1});
  return section;
}

[[nodiscard]] RaSectionData
build_sources_section(std::span<const BacktestSourcePartition> sources) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kSourcesSection), RaSectionKind::SubTable, sources.size(), arena);
  std::vector<std::string> dates;
  std::vector<std::int64_t> sizes;
  std::vector<std::int64_t> created;
  std::vector<std::uint32_t> header_crcs;
  std::vector<std::uint32_t> metadata_crcs;
  for (const BacktestSourcePartition &source : sources) {
    dates.push_back(source.date);
    sizes.push_back(u64_bits(source.identity.file_size));
    created.push_back(u64_bits(source.identity.created_ts_ns));
    header_crcs.push_back(source.identity.header_crc32c);
    metadata_crcs.push_back(source.identity.metadata_crc32c);
  }
  add_dict(section, arena, "date", std::move(dates));
  add_i64(section, arena, "file_size", std::move(sizes));
  add_i64(section, arena, "created_ts_ns", std::move(created));
  add_u32(section, arena, "header_crc32c", std::move(header_crcs));
  add_u32(section, arena, "metadata_crc32c", std::move(metadata_crcs));
  return section;
}

[[nodiscard]] RaSectionData build_checkpoint_section(const BacktestCheckpoint &checkpoint) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kCheckpointSection), RaSectionKind::ScalarKV, 1, arena);
  add_i64(section, arena, "base_ts_ns", {checkpoint.base_ts_ns});
  add_i64(section, arena, "completed_step_index", {u64_bits(checkpoint.completed_step_index)});
  add_i64(section, arena, "next_lot_id", {u64_bits(checkpoint.next_lot_id)});
  add_f64(section, arena, "cash", {checkpoint.cash});
  add_f64(section, arena, "nav", {checkpoint.nav});
  add_f64(section, arena, "cumulative_noncash_financing",
          {checkpoint.cumulative_noncash_financing});
  return section;
}

[[nodiscard]] RaSectionData build_checkpoint_lots_section(const BacktestCheckpoint &checkpoint) {
  const std::span<const Lot> lots = checkpoint.portfolio.lots;
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section = make_section(std::string(kCheckpointLotsSection), RaSectionKind::SubTable,
                                       lots.size(), arena);
  std::vector<std::uint32_t> ordinals;
  std::vector<std::int64_t> ids;
  std::vector<std::uint32_t> uids;
  std::vector<double> strikes;
  std::vector<double> residual_t;
  std::vector<std::uint32_t> sides;
  std::vector<double> quantities;
  std::vector<double> multipliers;
  std::vector<std::int64_t> expiries;
  std::vector<std::uint32_t> cohorts;
  std::vector<double> entry_prices;
  for (std::size_t i = 0; i < lots.size(); ++i) {
    const Lot &lot = lots[i];
    ordinals.push_back(static_cast<std::uint32_t>(i));
    ids.push_back(u64_bits(lot.id));
    uids.push_back(lot.contract.uid);
    strikes.push_back(lot.contract.K);
    residual_t.push_back(lot.contract.T);
    sides.push_back(static_cast<std::uint32_t>(lot.contract.side));
    quantities.push_back(lot.qty);
    multipliers.push_back(lot.multiplier);
    expiries.push_back(lot.expiry_ts_ns);
    cohorts.push_back(lot.cohort);
    entry_prices.push_back(lot.entry_price);
  }
  add_u32(section, arena, "ordinal", std::move(ordinals));
  add_i64(section, arena, "id", std::move(ids));
  add_u32(section, arena, "uid", std::move(uids));
  add_f64(section, arena, "strike", std::move(strikes));
  add_f64(section, arena, "residual_t", std::move(residual_t));
  add_u32(section, arena, "side", std::move(sides));
  add_f64(section, arena, "quantity", std::move(quantities));
  add_f64(section, arena, "multiplier", std::move(multipliers));
  add_i64(section, arena, "expiry_ts_ns", std::move(expiries));
  add_u32(section, arena, "cohort", std::move(cohorts));
  add_f64(section, arena, "entry_price", std::move(entry_prices));
  return section;
}

[[nodiscard]] RaSectionData build_checkpoint_shares_section(const BacktestCheckpoint &checkpoint) {
  const std::span<const HedgeSharePosition> shares = checkpoint.hedge_shares;
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section = make_section(std::string(kCheckpointSharesSection),
                                       RaSectionKind::SubTable, shares.size(), arena);
  std::vector<std::uint32_t> ordinals;
  std::vector<std::uint32_t> uids;
  std::vector<double> values;
  for (std::size_t i = 0; i < shares.size(); ++i) {
    ordinals.push_back(static_cast<std::uint32_t>(i));
    uids.push_back(shares[i].uid);
    values.push_back(shares[i].shares);
  }
  add_u32(section, arena, "ordinal", std::move(ordinals));
  add_u32(section, arena, "uid", std::move(uids));
  add_f64(section, arena, "shares", std::move(values));
  return section;
}

[[nodiscard]] Result<std::vector<RaSectionData>>
build_partition_sections(const BacktestSeriesInfo &info, const BacktestSeriesData &data) {
  if (data.checkpoint.completed_step_index >
          static_cast<std::size_t>((std::numeric_limits<std::uint64_t>::max)()) ||
      data.checkpoint.portfolio.lots.size() >
          static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) ||
      data.checkpoint.hedge_shares.size() >
          static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: checkpoint count is out of range");
  }
  RaSectionData backtest = encode_backtest_section("backtest", data.backtest);
  if (!data.backtest.gross_vega_abs.empty()) {
    backtest.columns.emplace_back("gross_vega_abs", RaColumnData::of_f64(std::span<const double>(
                                                        data.backtest.gross_vega_abs)));
  }
  if (!data.backtest.nav_liquidation.empty()) {
    backtest.columns.emplace_back("nav_liquidation", RaColumnData::of_f64(std::span<const double>(
                                                         data.backtest.nav_liquidation)));
  }
  std::vector<RaSectionData> sections;
  sections.reserve(6);
  sections.push_back(std::move(backtest));
  sections.push_back(build_series_meta(info, data.next_cohort));
  sections.push_back(build_sources_section(data.sources));
  sections.push_back(build_checkpoint_section(data.checkpoint));
  sections.push_back(build_checkpoint_lots_section(data.checkpoint));
  sections.push_back(build_checkpoint_shares_section(data.checkpoint));
  return Ok(std::move(sections));
}

} // namespace

std::uint64_t
backtest_source_fingerprint(std::span<const BacktestSourcePartition> sources) noexcept {
  if (!valid_source_order(sources)) {
    return 0;
  }
  std::uint64_t h = fnv_value(kFnvOffset, kBacktestDbEngineSchemaSalt);
  const std::uint64_t count = sources.size();
  h = fnv_value(h, count);
  for (const BacktestSourcePartition &source : sources) {
    h = fnv_string(h, source.date);
    h = fnv_value(h, source.identity.file_size);
    h = fnv_value(h, source.identity.created_ts_ns);
    h = fnv_value(h, source.identity.header_crc32c);
    h = fnv_value(h, source.identity.metadata_crc32c);
  }
  return force_nonzero(h);
}

std::uint64_t backtest_series_identity(std::uint64_t template_fingerprint, std::uint32_t uid,
                                       std::span<const BacktestSourcePartition> sources) noexcept {
  if (template_fingerprint == 0 || uid == 0 || !valid_source_order(sources)) {
    return 0;
  }
  std::uint64_t h = fnv_value(kFnvOffset, kBacktestDbEngineSchemaSalt);
  const std::uint64_t run_archive_schema = ra_schema_hash();
  h = fnv_value(h, run_archive_schema);
  h = fnv_value(h, kBacktestTemplateEngineSchemaSalt);
  // Task D1: fold the FULL engine identity -- ATX_VOL_VERSION_STRING +
  // kBacktestEconomicsRev + ra_schema_hash(), see make_engine_id() in
  // atx/vol/track_key.hpp -- so a change that moves the golden NAV
  // invalidates every persisted series MECHANICALLY, through the same
  // kBacktestEconomicsRev the D1 golden-NAV tripwire gates, instead of
  // resting on someone remembering to hand-bump kBacktestDbEngineSchemaSalt /
  // kBacktestTemplateEngineSchemaSalt above. Those two salts stay: they still
  // gate the MANIFEST/PARTITION BINARY LAYOUT and the template's own
  // encoding, which are structural changes an economics-rev fold cannot see.
  // Folding this string is a NEW schema/generation input, not a format edit
  // (`backtest_series_identity` was already, and remains, an opaque u64
  // stored in the existing `run_identity_hash` column) -- so BacktestDb v1
  // partitions stay byte- and reader-compatible; only the VALUE recomputed
  // for a given (template, uid, sources) changes, which invalidates every
  // series cached under the OLD recipe. That invalidation is expected: this
  // is exactly the "no more resting on hand-bumped salts" this task closes.
  h = fnv_string(h, make_engine_id());
  h = fnv_value(h, template_fingerprint);
  h = fnv_value(h, uid);
  const std::uint64_t count = sources.size();
  h = fnv_value(h, count);
  for (const BacktestSourcePartition &source : sources) {
    h = fnv_string(h, source.date);
    h = fnv_value(h, source.identity.file_size);
    h = fnv_value(h, source.identity.created_ts_ns);
    h = fnv_value(h, source.identity.header_crc32c);
    h = fnv_value(h, source.identity.metadata_crc32c);
  }
  return force_nonzero(h);
}

namespace {

[[nodiscard]] Status validate_result_shape(const BacktestResult &result, bool require_nonempty) {
  const std::size_t rows = result.size();
  if (require_nonempty && rows == 0) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: empty backtest series");
  }
  if (result.ts_ns.size() != rows) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: timestamp row count mismatch");
  }
  for (const BacktestSeriesColumn &column : backtest_series_columns()) {
    if ((result.*(column.member)).size() != rows) {
      return Err(ErrorCode::InvalidArgument,
                 "backtest_db: mandatory backtest column row count mismatch");
    }
  }
  if ((!result.gross_vega_abs.empty() && result.gross_vega_abs.size() != rows) ||
      (!result.nav_liquidation.empty() && result.nav_liquidation.size() != rows) ||
      (!result.swap_pv.empty() && result.swap_pv.size() != rows) ||
      (!result.swap_pnl.empty() && result.swap_pnl.size() != rows)) {
    return Err(ErrorCode::InvalidArgument,
               "backtest_db: optional backtest column row count mismatch");
  }
  if (!result.step_pnl_total.empty() &&
      result.step_pnl_total.size() != (rows == 0 ? 0 : rows - 1) &&
      result.step_pnl_total.size() != rows) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: full-resolution PnL row count mismatch");
  }
  std::unordered_set<std::string> signal_names;
  for (const auto &[name, values] : result.signals) {
    if (name.empty() || name.size() > sizeof(RaColumnDescriptor{}.name) || values.size() != rows ||
        name == "gross_vega_abs" || name == "nav_liquidation" ||
        !signal_names.insert(name).second) {
      return Err(ErrorCode::InvalidArgument, "backtest_db: invalid signal column");
    }
    for (const BacktestSeriesColumn &column : backtest_series_columns()) {
      if (name == column.name) {
        return Err(ErrorCode::InvalidArgument,
                   "backtest_db: signal shadows a mandatory backtest column");
      }
    }
    if (name == "date" || name == "ts_ns") {
      return Err(ErrorCode::InvalidArgument,
                 "backtest_db: signal shadows a mandatory backtest column");
    }
  }
  for (std::size_t i = 0; i < rows; ++i) {
    if (result.date[i].empty() || result.ts_ns[i] <= 0 ||
        (i != 0 &&
         (result.date[i - 1] >= result.date[i] || result.ts_ns[i - 1] >= result.ts_ns[i]))) {
      return Err(ErrorCode::InvalidArgument, "backtest_db: rows are not strictly ordered");
    }
  }
  return Ok();
}

template <class T> void append_vector(std::vector<T> &dst, const std::vector<T> &src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

// True when a result carries a swap lane that actually DID something. A
// zero-swap engine run fills both columns with exactly 0.0, which is
// indistinguishable in meaning from the empty columns a decoded result has â€”
// so "populated" here means "carries a non-zero value", not "non-empty".
[[nodiscard]] bool result_has_swap_data(const BacktestResult &result) noexcept {
  for (const double v : result.swap_pv) {
    if (v != 0.0) {
      return true;
    }
  }
  for (const double v : result.swap_pnl) {
    if (v != 0.0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] Status validate_checkpoint(const BacktestCheckpoint &checkpoint,
                                         std::uint32_t next_cohort, const BacktestResult &result) {
  if (checkpoint.base_ts_ns <= 0 || checkpoint.next_lot_id == 0 || !finite(checkpoint.cash) ||
      !finite(checkpoint.nav) || !finite(checkpoint.cumulative_noncash_financing)) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: invalid checkpoint scalar");
  }
  if (result.size() == 0) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: checkpoint needs result rows");
  }
  if (checkpoint.completed_step_index != result.size() - 1 ||
      checkpoint.base_ts_ns != result.ts_ns.back() || checkpoint.cash != result.cash.back() ||
      checkpoint.nav != result.nav.back()) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: checkpoint/result disagreement");
  }
  if (result.n_open_lots.back() != static_cast<double>(checkpoint.portfolio.lots.size())) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: checkpoint open-lot count mismatch");
  }
  // The on-disk checkpoint format (three frozen sections, folded into
  // `ra_schema_hash()`) carries no swap lane. Refuse a checkpoint that has one
  // rather than dropping it: a resumed run would silently restart every swap
  // lot's fixing series from zero. Extending the format is a schema decision â€”
  // a new section plus a version gate plus fresh goldens â€” not a side effect of
  // the engine gaining the lane. Decoded checkpoints never carry swap state, so
  // this only ever fires on the write path.
  if (!checkpoint.portfolio.swap_lots.empty() || !checkpoint.swap_accruals.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "backtest_db: the stored checkpoint format does not persist the swap lane");
  }
  std::unordered_set<std::uint64_t> lot_ids;
  for (const Lot &lot : checkpoint.portfolio.lots) {
    if (lot.id == 0 || lot.id >= checkpoint.next_lot_id || lot.contract.uid == 0 ||
        !finite(lot.contract.K) || lot.contract.K <= 0 || !finite(lot.contract.T) ||
        lot.contract.T < 0 || !valid_side(lot.contract.side) || !finite(lot.qty) ||
        !finite(lot.multiplier) || lot.multiplier <= 0 ||
        lot.expiry_ts_ns <= checkpoint.base_ts_ns || lot.cohort >= next_cohort ||
        !finite(lot.entry_price) || lot.entry_price < 0 || !lot_ids.insert(lot.id).second) {
      return Err(ErrorCode::InvalidArgument, "backtest_db: invalid checkpoint lot");
    }
  }
  std::unordered_set<std::uint32_t> share_uids;
  for (const HedgeSharePosition &position : checkpoint.hedge_shares) {
    if (position.uid == 0 || !finite(position.shares) || !share_uids.insert(position.uid).second) {
      return Err(ErrorCode::InvalidArgument, "backtest_db: invalid checkpoint hedge share");
    }
  }
  return Ok();
}

[[nodiscard]] Status validate_series_data(std::uint64_t template_fingerprint, std::uint32_t uid,
                                          const BacktestSeriesData &data) {
  const Status result_shape = validate_result_shape(data.backtest, true);
  if (!result_shape) {
    return result_shape;
  }
  if (!data.backtest.step_pnl_total.empty() &&
      data.backtest.step_pnl_total.size() != data.backtest.size() - 1) {
    return Err(ErrorCode::InvalidArgument,
               "backtest_db: stored history must include one inception row");
  }
  // The frozen series registry has no swap columns, so storing a run that HAS a
  // swap lane would drop its `swap_pv`/`swap_pnl` history on the floor. The
  // `validate_checkpoint` guard (reached at the end of this function) catches a
  // run still HOLDING swap lots; this catches the run whose swaps all settled
  // before the last row, leaving a clean checkpoint. Zero-swap runs carry
  // exactly 0.0 in both columns and store unchanged.
  for (std::size_t i = 0; i < data.backtest.size(); ++i) {
    const bool live = (i < data.backtest.swap_pv.size() && data.backtest.swap_pv[i] != 0.0) ||
                      (i < data.backtest.swap_pnl.size() && data.backtest.swap_pnl[i] != 0.0);
    if (live) {
      return Err(ErrorCode::InvalidArgument,
                 "backtest_db: the stored series schema does not carry the swap lane");
    }
  }
  if (!valid_source_order(data.sources) || data.sources.size() != data.backtest.size()) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: source/result row count mismatch");
  }
  for (std::size_t i = 0; i < data.sources.size(); ++i) {
    if (data.sources[i].date != data.backtest.date[i]) {
      return Err(ErrorCode::InvalidArgument, "backtest_db: source/result date mismatch");
    }
  }
  if (backtest_source_fingerprint(data.sources) == 0 ||
      backtest_series_identity(template_fingerprint, uid, data.sources) == 0) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: invalid series identity inputs");
  }
  return validate_checkpoint(data.checkpoint, data.next_cohort, data.backtest);
}

} // namespace

Status append_backtest_results(BacktestResult &dst, const BacktestResult &src) {
  const Status dst_shape = validate_result_shape(dst, false);
  if (!dst_shape) {
    return dst_shape;
  }
  const Status src_shape = validate_result_shape(src, false);
  if (!src_shape) {
    return src_shape;
  }
  if (!dst.date.empty() && !src.date.empty() &&
      (dst.date.back() >= src.date.front() || dst.ts_ns.back() >= src.ts_ns.front())) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: appended rows are not ordered");
  }
  const bool gross_shape_ok = dst.gross_vega_abs.empty() == src.gross_vega_abs.empty() ||
                              dst.size() == 0 || src.size() == 0;
  const bool liquidation_shape_ok = dst.nav_liquidation.empty() == src.nav_liquidation.empty() ||
                                    dst.size() == 0 || src.size() == 0;
  if (!gross_shape_ok || !liquidation_shape_ok || dst.signals.size() != src.signals.size()) {
    return Err(ErrorCode::InvalidArgument, "backtest_db: appended optional columns differ");
  }
  // A swap-lane SHAPE change is tolerated only when neither side has real swap
  // data â€” that is the DB-extension case (a decoded prefix reports the lane
  // absent, a freshly computed swap-free continuation reports it present and
  // all-zero), and collapsing it to "absent" loses nothing. If EITHER side
  // actually carries swap PnL, collapsing would destroy it AND disarm the
  // store-path guard in validate_series_data, which only ever sees the combined
  // result. Refuse instead.
  const bool dst_swap_shape = !dst.swap_pv.empty() && !dst.swap_pnl.empty();
  const bool src_swap_shape = !src.swap_pv.empty() && !src.swap_pnl.empty();
  if (dst_swap_shape != src_swap_shape &&
      (result_has_swap_data(dst) || result_has_swap_data(src))) {
    return Err(ErrorCode::InvalidArgument,
               "backtest_db: cannot append across a swap-lane shape change while either side "
               "carries swap data (the stored schema does not persist the lane)");
  }
  for (std::size_t i = 0; i < dst.signals.size(); ++i) {
    if (dst.signals[i].first != src.signals[i].first) {
      return Err(ErrorCode::InvalidArgument, "backtest_db: appended signal columns differ");
    }
  }

  BacktestResult combined = dst;
  append_vector(combined.date, src.date);
  append_vector(combined.ts_ns, src.ts_ns);
  for (const BacktestSeriesColumn &column : backtest_series_columns()) {
    std::vector<double> *const destination = mutable_backtest_column(combined, column.name);
    if (destination == nullptr) {
      return Err(ErrorCode::Internal, "backtest_db: unknown mandatory result column");
    }
    append_vector(*destination, src.*(column.member));
  }
  append_vector(combined.gross_vega_abs, src.gross_vega_abs);
  append_vector(combined.nav_liquidation, src.nav_liquidation);
  // Both sides carry the lane => concatenate. Shapes differ => the check above
  // has already proven neither side has real swap data, so collapsing to
  // "absent" (what a decoded result reports) is information-preserving.
  if (dst_swap_shape && src_swap_shape) {
    append_vector(combined.swap_pv, src.swap_pv);
    append_vector(combined.swap_pnl, src.swap_pnl);
  } else {
    combined.swap_pv.clear();
    combined.swap_pnl.clear();
  }
  append_vector(combined.step_pnl_total, src.step_pnl_total);
  for (std::size_t i = 0; i < combined.signals.size(); ++i) {
    append_vector(combined.signals[i].second, src.signals[i].second);
  }
  dst = std::move(combined);
  return Ok();
}

namespace {

[[nodiscard]] Result<std::vector<BacktestStrategyTemplate>>
decode_templates(const RaSectionView &templates_section, const RaSectionView &legs_section) {
  const Status template_schema = validate_columns(templates_section, kTemplateColumns);
  if (!template_schema) {
    return tl::unexpected<Error>(template_schema.error());
  }
  const Status leg_schema = validate_columns(legs_section, kTemplateLegColumns);
  if (!leg_schema) {
    return tl::unexpected<Error>(leg_schema.error());
  }
  const std::size_t count = static_cast<std::size_t>(templates_section.n_rows());
  const RaDictColumn ids = templates_section.dict_col("id");
  const RaDictColumn names = templates_section.dict_col("name");
  const auto fingerprints = templates_section.i64_col("fingerprint");
  const auto entry_every_n = templates_section.u32_col("entry_every_n");
  const auto holding = templates_section.u32_col("holding");
  const auto hedge_kind = templates_section.u32_col("hedge_kind");
  const auto hedge_cadence = templates_section.u32_col("hedge_cadence");
  const auto hedge_band = templates_section.f64_col("hedge_band");
  const auto analytic_greeks = templates_section.u32_col("analytic_greeks");
  const auto delta_tolerance = templates_section.f64_col("delta_tolerance");
  const auto query_execution = templates_section.u32_col("query_execution");
  const auto spread_kind = templates_section.u32_col("spread_kind");
  const auto half_spread_bps = templates_section.f64_col("half_spread_bps");
  const auto vol_tick = templates_section.f64_col("vol_tick");
  const auto impact_fraction = templates_section.f64_col("impact_fraction");
  const auto per_contract_cost = templates_section.f64_col("per_contract_cost");
  const auto hedge_slippage_bps = templates_section.f64_col("hedge_slippage_bps");
  const auto settlement = templates_section.u32_col("settlement");

  std::vector<BacktestStrategyTemplate> templates;
  templates.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::string_view id = ids.at(i);
    const std::string_view name = names.at(i);
    if (!valid_template_id(id) || name.size() > kMaxDisplayNameBytes ||
        holding[i] > static_cast<std::uint32_t>(BacktestHoldingRule::HoldToExpiry) ||
        hedge_kind[i] > static_cast<std::uint32_t>(HedgeSpec::Kind::DeltaToZero) ||
        hedge_cadence[i] > static_cast<std::uint32_t>(HedgeSpec::Cadence::Daily) ||
        analytic_greeks[i] > 1 ||
        query_execution[i] > static_cast<std::uint32_t>(QueryExecution::ColdReference) ||
        spread_kind[i] > static_cast<std::uint32_t>(FrictionModel::SpreadKind::VolTicks) ||
        settlement[i] >
            static_cast<std::uint32_t>(TheoreticalSettlementRule::FollowingNyseSessionSnapshot)) {
      return Err(ErrorCode::ParseError, "backtest_db: invalid persisted template enum");
    }
    BacktestStrategyTemplate value;
    value.id = id;
    value.name = name;
    value.entry_every_n = entry_every_n[i];
    value.holding = static_cast<BacktestHoldingRule>(holding[i]);
    value.hedge.kind = static_cast<HedgeSpec::Kind>(hedge_kind[i]);
    value.hedge.cadence = static_cast<HedgeSpec::Cadence>(hedge_cadence[i]);
    value.hedge.band = hedge_band[i];
    value.projection.analytic_greeks = analytic_greeks[i] != 0;
    value.projection.delta_tolerance = delta_tolerance[i];
    value.projection.query_execution = static_cast<QueryExecution>(query_execution[i]);
    value.frictions.spread_kind = static_cast<FrictionModel::SpreadKind>(spread_kind[i]);
    value.frictions.half_spread_bps = half_spread_bps[i];
    value.frictions.vol_tick = vol_tick[i];
    value.frictions.impact_fraction = impact_fraction[i];
    value.frictions.per_contract_cost = per_contract_cost[i];
    value.frictions.hedge_slippage_bps = hedge_slippage_bps[i];
    value.settlement = static_cast<TheoreticalSettlementRule>(settlement[i]);
    templates.push_back(std::move(value));
  }
  if (!std::is_sorted(templates.begin(), templates.end(), template_key_less)) {
    return Err(ErrorCode::ParseError, "backtest_db: templates are not sorted");
  }
  for (std::size_t i = 1; i < templates.size(); ++i) {
    if (templates[i - 1].id == templates[i].id) {
      return Err(ErrorCode::ParseError, "backtest_db: duplicate template id");
    }
  }

  const std::size_t leg_count = static_cast<std::size_t>(legs_section.n_rows());
  const RaDictColumn leg_ids = legs_section.dict_col("template_id");
  const auto ordinal = legs_section.u32_col("ordinal");
  const auto maturity_kind = legs_section.u32_col("maturity_kind");
  const auto year_fraction = legs_section.f64_col("year_fraction");
  const auto calendar_count = legs_section.i64_col("calendar_count");
  const auto expiry_ts_ns = legs_section.i64_col("expiry_ts_ns");
  const auto strike_kind = legs_section.u32_col("strike_kind");
  const auto strike_value = legs_section.f64_col("strike_value");
  const auto side = legs_section.u32_col("side");
  const auto quantity = legs_section.f64_col("quantity");
  const auto multiplier = legs_section.f64_col("multiplier");
  std::string_view previous_id;
  std::uint32_t expected_ordinal = 0;
  for (std::size_t i = 0; i < leg_count; ++i) {
    const std::string_view id = leg_ids.at(i);
    if (i == 0 || id != previous_id) {
      if (i != 0 && id < previous_id) {
        return Err(ErrorCode::ParseError, "backtest_db: template legs are not sorted");
      }
      previous_id = id;
      expected_ordinal = 0;
    }
    if (ordinal[i] != expected_ordinal ||
        maturity_kind[i] > static_cast<std::uint32_t>(ProjectedMaturityKind::AbsoluteExpiry) ||
        strike_kind[i] > static_cast<std::uint32_t>(ProjectedStrikeKind::AbsoluteStrike) ||
        side[i] > static_cast<std::uint32_t>(Side::Put)) {
      return Err(ErrorCode::ParseError, "backtest_db: invalid persisted template leg");
    }
    ++expected_ordinal;
    const auto template_it = std::lower_bound(
        templates.begin(), templates.end(), id,
        [](const BacktestStrategyTemplate &value, std::string_view key) { return value.id < key; });
    if (template_it == templates.end() || template_it->id != id) {
      return Err(ErrorCode::ParseError, "backtest_db: leg references unknown template");
    }
    BacktestTemplateLeg leg;
    leg.maturity.kind = static_cast<ProjectedMaturityKind>(maturity_kind[i]);
    leg.maturity.year_fraction = year_fraction[i];
    if (calendar_count[i] < (std::numeric_limits<std::int32_t>::min)() ||
        calendar_count[i] > (std::numeric_limits<std::int32_t>::max)()) {
      return Err(ErrorCode::ParseError, "backtest_db: calendar count is out of range");
    }
    leg.maturity.calendar_count = static_cast<std::int32_t>(calendar_count[i]);
    leg.maturity.expiry_ts_ns = expiry_ts_ns[i];
    leg.strike.kind = static_cast<ProjectedStrikeKind>(strike_kind[i]);
    leg.strike.value = strike_value[i];
    leg.side = static_cast<Side>(side[i]);
    leg.quantity = quantity[i];
    leg.multiplier = multiplier[i];
    template_it->legs.push_back(leg);
  }
  for (std::size_t i = 0; i < templates.size(); ++i) {
    const Status valid = validate_backtest_template(templates[i]);
    if (!valid || fingerprint_backtest_template(templates[i]) != i64_bits(fingerprints[i])) {
      return Err(ErrorCode::ParseError,
                 "backtest_db: persisted template fingerprint/economics mismatch");
    }
  }
  return Ok(std::move(templates));
}

[[nodiscard]] Result<std::vector<BacktestSeriesInfo>>
decode_series_index(const RaSectionView &section,
                    std::span<const BacktestStrategyTemplate> templates) {
  const Status schema = validate_columns(section, kSeriesIndexColumns);
  if (!schema) {
    return tl::unexpected<Error>(schema.error());
  }
  const std::size_t count = static_cast<std::size_t>(section.n_rows());
  const RaDictColumn template_ids = section.dict_col("template_id");
  const auto template_fingerprints = section.i64_col("template_fingerprint");
  const RaDictColumn symbols = section.dict_col("symbol");
  const auto uids = section.u32_col("uid");
  const auto row_counts = section.i64_col("row_count");
  const auto first_ts = section.i64_col("first_ts_ns");
  const auto last_ts = section.i64_col("last_ts_ns");
  const auto source_fingerprints = section.i64_col("source_fingerprint");
  const auto run_identities = section.i64_col("run_identity_hash");
  const RaDictColumn filenames = section.dict_col("partition_filename");
  const auto partition_sizes = section.i64_col("partition_file_size");
  const auto partition_created = section.i64_col("partition_created_ts_ns");
  const auto partition_header_crcs = section.u32_col("partition_header_crc32c");
  const auto partition_metadata_crcs = section.u32_col("partition_metadata_crc32c");
  std::vector<BacktestSeriesInfo> output;
  output.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    BacktestSeriesInfo info;
    info.template_id = template_ids.at(i);
    info.template_fingerprint = i64_bits(template_fingerprints[i]);
    auto symbol = canonical_db_symbol(symbols.at(i));
    if (!symbol || *symbol != symbols.at(i)) {
      return Err(ErrorCode::ParseError, "backtest_db: non-canonical persisted symbol");
    }
    info.symbol = std::move(*symbol);
    info.uid = uids[i];
    info.row_count = i64_bits(row_counts[i]);
    info.first_ts_ns = first_ts[i];
    info.last_ts_ns = last_ts[i];
    info.source_fingerprint = i64_bits(source_fingerprints[i]);
    info.run_identity_hash = i64_bits(run_identities[i]);
    info.partition_filename = filenames.at(i);
    info.partition_identity =
        ArchiveContentIdentity{i64_bits(partition_sizes[i]), i64_bits(partition_created[i]),
                               partition_header_crcs[i], partition_metadata_crcs[i]};
    const auto template_it = std::lower_bound(
        templates.begin(), templates.end(), info.template_id,
        [](const BacktestStrategyTemplate &value, std::string_view key) { return value.id < key; });
    if (template_it == templates.end() || template_it->id != info.template_id ||
        fingerprint_backtest_template(*template_it) != info.template_fingerprint || info.uid == 0 ||
        info.row_count == 0 || info.first_ts_ns <= 0 || info.last_ts_ns < info.first_ts_ns ||
        info.source_fingerprint == 0 || info.run_identity_hash == 0 ||
        info.partition_identity.file_size == 0 ||
        !safe_partition_filename(info.partition_filename) ||
        !info.partition_filename.starts_with(
            partition_filename_prefix(info.template_fingerprint, info.template_id, info.symbol) +
            "-g")) {
      return Err(ErrorCode::ParseError, "backtest_db: invalid series index record");
    }
    output.push_back(std::move(info));
  }
  if (!std::is_sorted(output.begin(), output.end(), series_key_less)) {
    return Err(ErrorCode::ParseError, "backtest_db: series index is not sorted");
  }
  for (std::size_t i = 1; i < output.size(); ++i) {
    if (same_series_key(output[i - 1], output[i])) {
      return Err(ErrorCode::ParseError, "backtest_db: duplicate series index record");
    }
  }
  return Ok(std::move(output));
}

} // namespace

namespace {

[[nodiscard]] RaSectionData build_db_meta(std::uint64_t generation, std::int64_t created_ts_ns,
                                          std::int64_t updated_ts_ns) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kDbMetaSection), RaSectionKind::ScalarKV, 1, arena);
  add_u32(section, arena, "format_version", {kBacktestDbFormatVersion});
  add_i64(section, arena, "generation", {u64_bits(generation)});
  add_i64(section, arena, "schema_salt", {u64_bits(kBacktestDbEngineSchemaSalt)});
  add_i64(section, arena, "created_ts_ns", {created_ts_ns});
  add_i64(section, arena, "updated_ts_ns", {updated_ts_ns});
  return section;
}

[[nodiscard]] RaSectionData
build_templates_section(std::span<const BacktestStrategyTemplate> templates) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section = make_section(std::string(kTemplatesSection), RaSectionKind::SubTable,
                                       templates.size(), arena);
  std::vector<std::string> ids;
  std::vector<std::string> names;
  std::vector<std::int64_t> fingerprints;
  std::vector<std::uint32_t> entry_every_n;
  std::vector<std::uint32_t> holding;
  std::vector<std::uint32_t> hedge_kind;
  std::vector<std::uint32_t> hedge_cadence;
  std::vector<double> hedge_band;
  std::vector<std::uint32_t> analytic_greeks;
  std::vector<double> delta_tolerance;
  std::vector<std::uint32_t> query_execution;
  std::vector<std::uint32_t> spread_kind;
  std::vector<double> half_spread_bps;
  std::vector<double> vol_tick;
  std::vector<double> impact_fraction;
  std::vector<double> per_contract_cost;
  std::vector<double> hedge_slippage_bps;
  std::vector<std::uint32_t> settlement;
  for (const BacktestStrategyTemplate &value : templates) {
    ids.push_back(value.id);
    names.push_back(value.name);
    fingerprints.push_back(u64_bits(fingerprint_backtest_template(value)));
    entry_every_n.push_back(value.entry_every_n);
    holding.push_back(static_cast<std::uint32_t>(value.holding));
    hedge_kind.push_back(static_cast<std::uint32_t>(value.hedge.kind));
    hedge_cadence.push_back(static_cast<std::uint32_t>(value.hedge.cadence));
    hedge_band.push_back(value.hedge.band);
    analytic_greeks.push_back(value.projection.analytic_greeks ? 1u : 0u);
    delta_tolerance.push_back(value.projection.delta_tolerance);
    query_execution.push_back(static_cast<std::uint32_t>(value.projection.query_execution));
    spread_kind.push_back(static_cast<std::uint32_t>(value.frictions.spread_kind));
    half_spread_bps.push_back(value.frictions.half_spread_bps);
    vol_tick.push_back(value.frictions.vol_tick);
    impact_fraction.push_back(value.frictions.impact_fraction);
    per_contract_cost.push_back(value.frictions.per_contract_cost);
    hedge_slippage_bps.push_back(value.frictions.hedge_slippage_bps);
    settlement.push_back(static_cast<std::uint32_t>(value.settlement));
  }
  add_dict(section, arena, "id", std::move(ids));
  add_dict(section, arena, "name", std::move(names));
  add_i64(section, arena, "fingerprint", std::move(fingerprints));
  add_u32(section, arena, "entry_every_n", std::move(entry_every_n));
  add_u32(section, arena, "holding", std::move(holding));
  add_u32(section, arena, "hedge_kind", std::move(hedge_kind));
  add_u32(section, arena, "hedge_cadence", std::move(hedge_cadence));
  add_f64(section, arena, "hedge_band", std::move(hedge_band));
  add_u32(section, arena, "analytic_greeks", std::move(analytic_greeks));
  add_f64(section, arena, "delta_tolerance", std::move(delta_tolerance));
  add_u32(section, arena, "query_execution", std::move(query_execution));
  add_u32(section, arena, "spread_kind", std::move(spread_kind));
  add_f64(section, arena, "half_spread_bps", std::move(half_spread_bps));
  add_f64(section, arena, "vol_tick", std::move(vol_tick));
  add_f64(section, arena, "impact_fraction", std::move(impact_fraction));
  add_f64(section, arena, "per_contract_cost", std::move(per_contract_cost));
  add_f64(section, arena, "hedge_slippage_bps", std::move(hedge_slippage_bps));
  add_u32(section, arena, "settlement", std::move(settlement));
  return section;
}

[[nodiscard]] RaSectionData
build_template_legs_section(std::span<const BacktestStrategyTemplate> templates) {
  std::size_t count = 0;
  for (const BacktestStrategyTemplate &value : templates) {
    count += value.legs.size();
  }
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kTemplateLegsSection), RaSectionKind::SubTable, count, arena);
  std::vector<std::string> ids;
  std::vector<std::uint32_t> ordinal;
  std::vector<std::uint32_t> maturity_kind;
  std::vector<double> year_fraction;
  std::vector<std::int64_t> calendar_count;
  std::vector<std::int64_t> expiry_ts_ns;
  std::vector<std::uint32_t> strike_kind;
  std::vector<double> strike_value;
  std::vector<std::uint32_t> side;
  std::vector<double> quantity;
  std::vector<double> multiplier;
  for (const BacktestStrategyTemplate &value : templates) {
    for (std::size_t i = 0; i < value.legs.size(); ++i) {
      const BacktestTemplateLeg &leg = value.legs[i];
      ids.push_back(value.id);
      ordinal.push_back(static_cast<std::uint32_t>(i));
      maturity_kind.push_back(static_cast<std::uint32_t>(leg.maturity.kind));
      year_fraction.push_back(leg.maturity.year_fraction);
      calendar_count.push_back(leg.maturity.calendar_count);
      expiry_ts_ns.push_back(leg.maturity.expiry_ts_ns);
      strike_kind.push_back(static_cast<std::uint32_t>(leg.strike.kind));
      strike_value.push_back(leg.strike.value);
      side.push_back(static_cast<std::uint32_t>(leg.side));
      quantity.push_back(leg.quantity);
      multiplier.push_back(leg.multiplier);
    }
  }
  add_dict(section, arena, "template_id", std::move(ids));
  add_u32(section, arena, "ordinal", std::move(ordinal));
  add_u32(section, arena, "maturity_kind", std::move(maturity_kind));
  add_f64(section, arena, "year_fraction", std::move(year_fraction));
  add_i64(section, arena, "calendar_count", std::move(calendar_count));
  add_i64(section, arena, "expiry_ts_ns", std::move(expiry_ts_ns));
  add_u32(section, arena, "strike_kind", std::move(strike_kind));
  add_f64(section, arena, "strike_value", std::move(strike_value));
  add_u32(section, arena, "side", std::move(side));
  add_f64(section, arena, "quantity", std::move(quantity));
  add_f64(section, arena, "multiplier", std::move(multiplier));
  return section;
}

[[nodiscard]] RaSectionData build_series_index_section(std::span<const BacktestSeriesInfo> series) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kSeriesIndexSection), RaSectionKind::SubTable, series.size(), arena);
  std::vector<std::string> template_ids;
  std::vector<std::int64_t> template_fingerprints;
  std::vector<std::string> symbols;
  std::vector<std::uint32_t> uids;
  std::vector<std::int64_t> row_counts;
  std::vector<std::int64_t> first_ts;
  std::vector<std::int64_t> last_ts;
  std::vector<std::int64_t> source_fingerprints;
  std::vector<std::int64_t> run_identities;
  std::vector<std::string> filenames;
  std::vector<std::int64_t> partition_sizes;
  std::vector<std::int64_t> partition_created;
  std::vector<std::uint32_t> partition_header_crcs;
  std::vector<std::uint32_t> partition_metadata_crcs;
  for (const BacktestSeriesInfo &value : series) {
    template_ids.push_back(value.template_id);
    template_fingerprints.push_back(u64_bits(value.template_fingerprint));
    symbols.push_back(value.symbol);
    uids.push_back(value.uid);
    row_counts.push_back(u64_bits(value.row_count));
    first_ts.push_back(value.first_ts_ns);
    last_ts.push_back(value.last_ts_ns);
    source_fingerprints.push_back(u64_bits(value.source_fingerprint));
    run_identities.push_back(u64_bits(value.run_identity_hash));
    filenames.push_back(value.partition_filename);
    partition_sizes.push_back(u64_bits(value.partition_identity.file_size));
    partition_created.push_back(u64_bits(value.partition_identity.created_ts_ns));
    partition_header_crcs.push_back(value.partition_identity.header_crc32c);
    partition_metadata_crcs.push_back(value.partition_identity.metadata_crc32c);
  }
  add_dict(section, arena, "template_id", std::move(template_ids));
  add_i64(section, arena, "template_fingerprint", std::move(template_fingerprints));
  add_dict(section, arena, "symbol", std::move(symbols));
  add_u32(section, arena, "uid", std::move(uids));
  add_i64(section, arena, "row_count", std::move(row_counts));
  add_i64(section, arena, "first_ts_ns", std::move(first_ts));
  add_i64(section, arena, "last_ts_ns", std::move(last_ts));
  add_i64(section, arena, "source_fingerprint", std::move(source_fingerprints));
  add_i64(section, arena, "run_identity_hash", std::move(run_identities));
  add_dict(section, arena, "partition_filename", std::move(filenames));
  add_i64(section, arena, "partition_file_size", std::move(partition_sizes));
  add_i64(section, arena, "partition_created_ts_ns", std::move(partition_created));
  add_u32(section, arena, "partition_header_crc32c", std::move(partition_header_crcs));
  add_u32(section, arena, "partition_metadata_crc32c", std::move(partition_metadata_crcs));
  return section;
}

[[nodiscard]] std::vector<RaSectionData>
build_manifest_sections(std::uint64_t generation, std::int64_t created_ts_ns,
                        std::int64_t updated_ts_ns,
                        std::span<const BacktestStrategyTemplate> templates,
                        std::span<const BacktestSeriesInfo> series) {
  std::vector<RaSectionData> sections;
  sections.reserve(4);
  sections.push_back(build_db_meta(generation, created_ts_ns, updated_ts_ns));
  sections.push_back(build_templates_section(templates));
  sections.push_back(build_template_legs_section(templates));
  sections.push_back(build_series_index_section(series));
  return sections;
}

} // namespace

} // namespace atx::vol
