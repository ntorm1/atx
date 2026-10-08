#include "research_dataset.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>

#include <duckdb.h>

#include "atx/core/sha256.hpp"

namespace atx::server::research_detail {
namespace {
constexpr std::int64_t day_ns = 86'400'000'000'000LL;
constexpr std::uint64_t mib = 1024ULL * 1024ULL;

template<class T, void (*Destroy)(T*)> class Handle {
public:
  ~Handle() { Destroy(&value_); }
  Handle() = default;
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&&) = delete;
  Handle& operator=(Handle&&) = delete;
  T* out() { return &value_; }
  T& get() { return value_; }
private:
  T value_{};
};
using Result = Handle<duckdb_result, duckdb_destroy_result>;
using Chunk = Handle<duckdb_data_chunk, duckdb_destroy_data_chunk>;
using Config = Handle<duckdb_config, duckdb_destroy_config>;
using Database = Handle<duckdb_database, duckdb_close>;
using Connection = Handle<duckdb_connection, duckdb_disconnect>;

std::string utf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  // SAFETY: char aliases the UTF-8 byte representation, which is copied immediately.
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string sql_literal(std::string_view text) {
  std::string result = "'";
  for (const auto c : text) { result += c; if (c == '\'') result += '\''; }
  result += '\'';
  return result;
}

bool sha_text(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
  });
}

std::uint64_t extent(const Json& receipt) {
  const auto& value = receipt.at("bytes");
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    throw std::invalid_argument("dataset receipt bytes must be a nonnegative integer");
  const auto bytes = value.get<std::uint64_t>();
  if (bytes == 0 || bytes > 256 * mib)
    throw std::invalid_argument("dataset monthly payload exceeds extent budget");
  return bytes;
}

std::chrono::sys_days file_month(std::string_view name) {
  if (name.size() != 26 || name.substr(0, 5) != "year=" || name[9] != '/' ||
      name.substr(10, 6) != "panel-" || name.substr(18) != ".parquet")
    throw std::invalid_argument("dataset file path must use year=YYYY/panel-MM.parquet");
  const auto year = name.substr(5, 4), month = name.substr(16, 2);
  const auto digits = [](std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  if (!digits(year) || !digits(month)) throw std::invalid_argument("invalid dataset file month");
  return parse_date(std::string(year) + "-" + std::string(month) + "-01");
}

std::string hash_file(const std::filesystem::path& path, std::uint64_t bytes,
                      std::stop_token stop) {
  if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != bytes)
    throw std::invalid_argument("dataset payload extent does not match pinned receipt");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open dataset payload");
  atx::core::Sha256 hash;
  std::array<std::byte, 65536> buffer;
  std::uint64_t consumed = 0;
  while (consumed < bytes) {
    checkpoint(stop, {}, 0, {});
    const auto count = std::min(bytes - consumed, static_cast<std::uint64_t>(buffer.size()));
    // SAFETY: char may alias the byte buffer; count is bounded by its allocated extent.
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count))
      throw std::runtime_error("dataset payload changed while hashing");
    const auto status = hash.update(std::span<const std::byte>{buffer.data(),
        static_cast<std::size_t>(count)});
    if (!status) throw std::runtime_error(status.error().message());
    consumed += count;
  }
  if (std::filesystem::file_size(path) != bytes)
    throw std::invalid_argument("dataset payload extent changed while hashing");
  const auto digest = hash.finalize();
  if (!digest) throw std::runtime_error(digest.error().message());
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : *digest) {
    const auto value = std::to_integer<unsigned>(byte);
    result += hex[value >> 4]; result += hex[value & 15U];
  }
  return result;
}

struct Payload {
  std::filesystem::path path;
  std::uint64_t bytes{};
  std::string sha;
};
struct Manifest {
  Json json;
  std::vector<Payload> files;
};

Manifest manifest(const ResearchDataset& dataset, const DatasetRecipe& recipe,
                  std::chrono::sys_days lower, std::stop_token stop) {
  const auto text = read_artifact(dataset.manifest_path, static_cast<std::size_t>(mib), stop);
  const auto digest = atx::core::sha256_hex(text);
  if (!digest || *digest != dataset.manifest_sha256)
    throw std::invalid_argument("dataset manifest SHA256 does not match registered pin");
  Manifest result{parse_json(text), {}};
  if (result.json.value("schema", std::string{}) != "atx.alpha-panel.panel/v2" ||
      result.json.value("status", std::string{}) != "complete")
    throw std::invalid_argument("dataset requires a complete published alpha-panel/v2 manifest");
  const auto& files = result.json.at("files");
  if (!files.is_object() || files.empty() || files.size() > 256)
    throw std::invalid_argument("dataset file catalog exceeds bounds");
  const auto root = dataset.manifest_path.parent_path();
  for (const auto& item : files.items()) {
    const auto month = file_month(item.key());
    const auto bytes = extent(item.value());
    const auto sha = item.value().at("sha256").get<std::string>();
    if (!sha_text(sha)) throw std::invalid_argument("invalid dataset payload SHA256");
    const auto next = std::chrono::sys_days{std::chrono::year_month_day{month}.year() /
        std::chrono::year_month_day{month}.month() / 1} + std::chrono::days{32};
    const auto month_end = std::chrono::sys_days{std::chrono::year_month_day{next}.year() /
        std::chrono::year_month_day{next}.month() / 1};
    if (month >= parse_date("2023-01-01") || month_end <= lower || month > recipe.end) continue;
    const auto path = std::filesystem::canonical(root / item.key());
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
      throw std::invalid_argument("dataset payload escapes registered root");
    result.files.push_back({path, bytes, sha});
  }
  if (result.files.empty())
    throw std::invalid_argument("no pinned TRAIN files for research window");
  std::uint64_t aggregate = 0;
  for (const auto& file : result.files) aggregate += file.bytes;
  if (aggregate > 2048 * mib)
    throw std::invalid_argument("dataset selected payloads exceed 2 GiB aggregate input budget");
  for (const auto& file : result.files)
    if (hash_file(file.path, file.bytes, stop) != file.sha)
      throw std::invalid_argument("dataset payload SHA256 does not match pinned receipt");
  return result;
}

void query(duckdb_connection connection, const std::string& sql, Result& result,
           std::stop_token stop) {
  checkpoint(stop, {}, 0, {});
  if (duckdb_query(connection, sql.c_str(), result.out()) != DuckDBSuccess) {
    checkpoint(stop, {}, 0, {});
    const auto* message = duckdb_result_error(&result.get());
    throw std::invalid_argument(std::string("dataset query rejected: ") +
        (message ? std::string(message).substr(0, 512) : "unknown DuckDB error"));
  }
  checkpoint(stop, {}, 0, {});
}

void configure(Config& config, std::uint64_t reader_bytes) {
  if (duckdb_create_config(config.out()) != DuckDBSuccess)
    throw std::runtime_error("cannot create dataset reader configuration");
  const auto set = [&](const char* name, const char* value) {
    if (duckdb_set_config(config.get(), name, value) != DuckDBSuccess)
      throw std::runtime_error(std::string("cannot configure dataset reader: ") + name);
  };
  const auto memory = std::to_string(reader_bytes) + "B";
  set("memory_limit", memory.c_str()); set("threads", "1"); set("temp_directory", "");
  set("max_temp_directory_size", "0B"); set("autoload_known_extensions", "false");
  set("autoinstall_known_extensions", "false"); set("allow_unsigned_extensions", "false");
  set("allow_community_extensions", "false");
}

std::string source_sql(const Manifest& source, std::chrono::sys_days lower,
                       std::chrono::sys_days upper) {
  std::string paths = "[";
  for (const auto& file : source.files) {
    if (paths.size() > 1) paths += ',';
    paths += sql_literal(utf8(file.path));
  }
  return "read_parquet(" + paths + "], hive_partitioning=false) WHERE session_date BETWEEN DATE " +
      sql_literal(date_text(lower)) + " AND DATE " + sql_literal(date_text(upper));
}

std::vector<std::int64_t> sessions(duckdb_connection connection, const std::string& source,
                                  std::stop_token stop) {
  Result result;
  query(connection, "SELECT DISTINCT epoch_ns(session_date) AS session FROM " + source +
      " ORDER BY session LIMIT 2001", result, stop);
  if (duckdb_column_type(&result.get(), 0) != DUCKDB_TYPE_BIGINT)
    throw std::invalid_argument("dataset date axis must be DATE");
  std::vector<std::int64_t> keys;
  while (true) {
    Chunk chunk; chunk.get() = duckdb_fetch_chunk(result.get());
    if (!chunk.get()) break;
    const auto vector = duckdb_data_chunk_get_vector(chunk.get(), 0);
    const auto* data = static_cast<const std::int64_t*>(duckdb_vector_get_data(vector));
    auto* validity = duckdb_vector_get_validity(vector);
    for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk.get()); ++row) {
      if (validity && !duckdb_validity_row_is_valid(validity, row))
        throw std::invalid_argument("dataset session date cannot be null");
      if (data[row] % day_ns != 0) throw std::invalid_argument("dataset date is not a market day");
      keys.push_back(data[row]);
    }
  }
  if (keys.size() > 2000) throw std::invalid_argument("dataset date axis exceeds session budget");
  return keys;
}

std::vector<std::uint64_t> cohort(duckdb_connection connection, const std::string& source,
    std::int64_t formation, std::size_t limit, std::stop_token stop) {
  Result result;
  query(connection, "SELECT security_id FROM " + source +
      " AND epoch_ns(session_date)=" + std::to_string(formation) +
      " AND member_equity=true AND isfinite(adv63) AND adv63>5000000 "
      "ORDER BY adv63 DESC,security_id ASC LIMIT " + std::to_string(limit), result, stop);
  if (duckdb_column_type(&result.get(), 0) != DUCKDB_TYPE_BIGINT)
    throw std::invalid_argument("dataset security_id must be BIGINT");
  std::vector<std::uint64_t> ids;
  while (true) {
    Chunk chunk; chunk.get() = duckdb_fetch_chunk(result.get());
    if (!chunk.get()) break;
    const auto vector = duckdb_data_chunk_get_vector(chunk.get(), 0);
    const auto* data = static_cast<const std::int64_t*>(duckdb_vector_get_data(vector));
    auto* validity = duckdb_vector_get_validity(vector);
    for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk.get()); ++row) {
      if ((validity && !duckdb_validity_row_is_valid(validity, row)) || data[row] <= 0)
        throw std::invalid_argument("dataset security ID must be positive and nonnull");
      ids.push_back(static_cast<std::uint64_t>(data[row]));
    }
  }
  std::sort(ids.begin(), ids.end());
  if (ids.size() < 2 || std::adjacent_find(ids.begin(), ids.end()) != ids.end())
    throw std::invalid_argument("formation requires at least two unique eligible securities");
  return ids;
}

struct Columns {
  std::vector<std::vector<double>> fields;
  std::vector<std::uint8_t> member;
  std::size_t guarded{}, resets{};
};

bool valid(duckdb_vector vector, idx_t row) {
  auto* mask = duckdb_vector_get_validity(vector);
  return !mask || duckdb_validity_row_is_valid(mask, row);
}

double scalar(duckdb_vector vector, idx_t row) {
  if (!valid(vector, row)) return std::numeric_limits<double>::quiet_NaN();
  const auto value = static_cast<const double*>(duckdb_vector_get_data(vector))[row];
  return std::isfinite(value) ? value : std::numeric_limits<double>::quiet_NaN();
}

void fill_chunk(duckdb_data_chunk chunk, const std::vector<std::int64_t>& keys,
    const std::vector<std::uint64_t>& ids, Columns& columns,
    std::vector<std::uint8_t>& seen, std::stop_token stop) {
  std::array<duckdb_vector, 8> vectors{};
  for (idx_t i = 0; i < vectors.size(); ++i) vectors[i] = duckdb_data_chunk_get_vector(chunk, i);
  const auto* dates = static_cast<const std::int64_t*>(duckdb_vector_get_data(vectors[0]));
  const auto* names = static_cast<const std::int64_t*>(duckdb_vector_get_data(vectors[1]));
  const auto* member = static_cast<const bool*>(duckdb_vector_get_data(vectors[6]));
  const auto* guarded = static_cast<const bool*>(duckdb_vector_get_data(vectors[7]));
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk); ++row) {
    checkpoint(stop, {}, 0, {});
    if (!valid(vectors[0], row) || !valid(vectors[1], row) || names[row] <= 0)
      throw std::invalid_argument("dataset row has invalid date/security identity");
    const auto date = std::lower_bound(keys.begin(), keys.end(), dates[row]);
    const auto name = std::lower_bound(ids.begin(), ids.end(),
        static_cast<std::uint64_t>(names[row]));
    if (date == keys.end() || *date != dates[row] || name == ids.end() ||
        *name != static_cast<std::uint64_t>(names[row]))
      throw std::invalid_argument("dataset row escapes requested axes");
    const auto day = static_cast<std::size_t>(date - keys.begin());
    const auto instrument = static_cast<std::size_t>(name - ids.begin());
    const auto index = day * ids.size() + instrument;
    if (seen[index] != 0) throw std::invalid_argument("duplicate dataset date/security row");
    seen[index] = 1;
    auto raw = scalar(vectors[2], row), ret = scalar(vectors[3], row);
    auto volume = scalar(vectors[4], row), adv = scalar(vectors[5], row);
    const auto clean = valid(vectors[7], row) && !guarded[row];
    if (!clean) { ret = std::numeric_limits<double>::quiet_NaN(); ++columns.guarded; }
    if (!(raw > 0)) raw = std::numeric_limits<double>::quiet_NaN();
    if (!(volume >= 0)) volume = std::numeric_limits<double>::quiet_NaN();
    if (!(adv >= 0)) adv = std::numeric_limits<double>::quiet_NaN();
    if (!(ret > -1)) ret = std::numeric_limits<double>::quiet_NaN();
    auto close = std::numeric_limits<double>::quiet_NaN();
    if (std::isfinite(raw) && std::isfinite(ret)) {
      const auto prior = day == 0 ? close : columns.fields[0][index - ids.size()];
      close = std::isfinite(prior) ? prior * (1 + ret) : 100.0;
      if (!std::isfinite(prior) && day != 0) ++columns.resets;
      if (!(close > 0) || !std::isfinite(close))
        throw std::invalid_argument("causal total-return index overflow or underflow");
    }
    columns.fields[0][index] = close; columns.fields[1][index] = raw;
    columns.fields[2][index] = volume; columns.fields[3][index] = ret;
    const auto dollars = raw * volume;
    columns.fields[4][index] = std::isfinite(dollars) ? dollars :
        std::numeric_limits<double>::quiet_NaN();
    columns.fields[5][index] = adv;
    columns.member[index] = static_cast<std::uint8_t>(valid(vectors[6], row) && member[row] &&
        std::isfinite(close));
  }
}

Columns materialize(duckdb_connection connection, const std::string& source,
    const std::vector<std::int64_t>& keys, const std::vector<std::uint64_t>& ids,
    const ResearchOptions& options, std::stop_token stop) {
  const auto cells = keys.size() * ids.size();
  if (static_cast<std::uint64_t>(cells) * 128ULL > options.max_working_bytes / 2)
    throw std::invalid_argument("dataset materialization exceeds working memory budget");
  std::string names;
  for (const auto id : ids) { if (!names.empty()) names += ','; names += std::to_string(id); }
  Result result;
  query(connection, "SELECT epoch_ns(session_date),security_id,raw_close,ret,volume,adv63," 
      "member_equity,ret_guarded FROM " + source + " AND security_id IN (" + names +
      ") AND epoch_ns(session_date)>=" + std::to_string(keys.front()) +
      " ORDER BY session_date,security_id LIMIT " + std::to_string(cells + 1), result, stop);
  constexpr std::array<duckdb_type, 8> types{DUCKDB_TYPE_BIGINT, DUCKDB_TYPE_BIGINT,
      DUCKDB_TYPE_DOUBLE, DUCKDB_TYPE_DOUBLE, DUCKDB_TYPE_DOUBLE, DUCKDB_TYPE_DOUBLE,
      DUCKDB_TYPE_BOOLEAN, DUCKDB_TYPE_BOOLEAN};
  for (idx_t i = 0; i < types.size(); ++i)
    if (duckdb_column_type(&result.get(), i) != types[i])
      throw std::invalid_argument("dataset required field has an incompatible physical type");
  if (duckdb_row_count(&result.get()) > cells)
    throw std::invalid_argument("dataset rows exceed bounded unique axes");
  Columns columns{std::vector<std::vector<double>>(6,
      std::vector<double>(cells, std::numeric_limits<double>::quiet_NaN())),
      std::vector<std::uint8_t>(cells, 0), 0, 0};
  std::vector<std::uint8_t> seen(cells, 0);
  while (true) {
    Chunk chunk; chunk.get() = duckdb_fetch_chunk(result.get());
    if (!chunk.get()) break;
    fill_chunk(chunk.get(), keys, ids, columns, seen, stop);
  }
  return columns;
}
} // namespace

DatasetPanel load_dataset(const ResearchDataset& dataset, const DatasetRecipe& recipe,
    const ResearchOptions& options, std::stop_token stop, const Progress& progress,
    DatasetAdmissionProfile profile) {
  checkpoint(stop, progress, 0.1, "Verifying published TRAIN monthly payload receipts");
  const auto lower = std::max(parse_date("2018-01-01"), recipe.start -
      std::chrono::days{static_cast<std::int64_t>(recipe.lookback * 2 + 40)});
  const auto source = manifest(dataset, recipe, lower, stop);
  const auto reader_bytes = std::min(128 * mib, options.max_working_bytes / 4);
  if (reader_bytes < 8 * mib) throw std::invalid_argument("dataset reader memory budget too small");
  Config config; configure(config, reader_bytes);
  Database database;
  char* error = nullptr;
  if (duckdb_open_ext(nullptr, database.out(), config.get(), &error) != DuckDBSuccess) {
    const std::string message = error ? error : "cannot open isolated dataset reader";
    duckdb_free(error);
    throw std::runtime_error(message);
  }
  duckdb_free(error);
  Connection connection;
  if (duckdb_connect(database.get(), connection.out()) != DuckDBSuccess)
    throw std::runtime_error("cannot connect isolated dataset reader");
  // The stop callback is destroyed before the connection; no callback can use a closed handle.
  std::stop_callback callback(stop, std::function<void()>{[&] {
    duckdb_interrupt(connection.get());
  }});
  { Result setup; query(connection.get(), "SET allowed_directories=[" +
      sql_literal(utf8(dataset.manifest_path.parent_path())) + "]", setup, stop); }
  { Result setup; query(connection.get(), "SET enable_external_access=false", setup, stop); }
  { Result setup; query(connection.get(), "SET lock_configuration=true", setup, stop); }
  const auto sql = source_sql(source, lower, recipe.end);
  auto keys = sessions(connection.get(), sql, stop);
  const auto start = recipe.start.time_since_epoch().count() * day_ns;
  const auto formation = std::lower_bound(keys.begin(), keys.end(), start);
  if (formation == keys.end())
    throw std::invalid_argument("no market sessions in requested window");
  const auto warmup = static_cast<std::size_t>(formation - keys.begin());
  const auto formation_key = *formation;
  if (warmup < recipe.lookback)
    throw std::invalid_argument("insufficient TRAIN DSL warmup sessions");
  const auto ids = cohort(connection.get(), sql, *formation, recipe.instrument_limit, stop);
  const auto discarded = warmup - recipe.lookback;
  keys.erase(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(discarded));
  const auto scored = keys.size() - recipe.lookback;
  if (profile == DatasetAdmissionProfile::Paper) {
    if (scored < 1 || scored > 63 || ids.size() > 128)
      throw std::invalid_argument("paper window requires 1..63 sessions and at most 128 names");
  } else if (scored < 3) {
    throw std::invalid_argument("research requires at least three bounded market sessions");
  }
  if (keys.size() > options.max_csv_rows)
    throw std::invalid_argument("dataset sessions exceed configured row bound");
  checkpoint(stop, progress, 0.3, "Loading bounded first-session cohort and causal price fields");
  auto columns = materialize(connection.get(), sql, keys, ids, options, stop);
  for (const auto& file : source.files)
    if (hash_file(file.path, file.bytes, stop) != file.sha)
      throw std::invalid_argument("dataset payload changed during read");
  auto panel = atx::engine::alpha::Panel::create(keys.size(), ids.size(), dataset_fields(),
      std::move(columns.fields), std::move(columns.member));
  if (!panel) throw std::runtime_error(panel.error().message());
  Json receipts = Json::array();
  for (const auto& file : source.files) receipts.push_back({{"path",
      utf8(file.path.lexically_relative(dataset.manifest_path.parent_path()))},
      {"bytes", file.bytes}, {"sha256", file.sha}});
  Json provenance{{"synthetic", false}, {"evaluation", "TRAIN"}, {"dataset_id", dataset.id},
      {"dataset_manifest_sha256", dataset.manifest_sha256}, {"payloads", receipts},
      {"source_publication_rules", source.json.value("rules", Json(nullptr))},
      {"source_input_manifest_sha256", source.json.value("input_manifests_sha256", Json::object())},
      {"historical_vintage_verified", false}, {"common_stock_history_verified", false},
      {"capacity_verified", false}, {"borrow_availability_verified", false},
      {"formation_date", date_text(std::chrono::sys_days{
          std::chrono::days{formation_key / day_ns}})},
      {"universe", "first simulation session member_equity; prior-session adv63 descending; "
          "security_id tie break; fixed cohort with daily published member_equity eligibility"},
      {"universe_history_verified", false},
      {"calendar", "observed monthly panel session_date axis"},
      {"source_clock", "modeled close 22:00 UTC; decision 23:00 UTC; not historical receipts"},
      {"max_payload_bytes", 2048 * mib},
      {"warmup_sessions", recipe.lookback}, {"instruments", ids.size()},
      {"guarded_return_rows", columns.guarded}, {"tri_segment_resets", columns.resets},
      {"close_convention", "causal forward observed total-return index; initial valid mark 100; "
          "guarded/missing return marks missing; restart after gap; never backward-scaled close"},
      {"missing_data_policy", "no imputation; native adverse liquidation for missing marks"}};
  checkpoint(stop, progress, 0.45, "Pinned market panel loaded and payloads reverified");
  return {std::move(*panel), std::move(keys), ids, recipe.lookback, std::move(provenance)};
}
} // namespace atx::server::research_detail
