// P9 lane SQL4 (K-P9-13; sql-design 3.3, 3.7 group `cache`, 3.10; rulings SQL-6, SQL-11): the IC
// caches' SQLite index (strategy_ic_cache_index.hpp) behind --candidate-cache-index /
// --pair-cache-index. Flag absent: today's files and bytes whatever DIR holds; with the flag the
// rows are the entries (the signal row is the commit marker, the IC-result row the self-hashed
// record text, the pair rows the shards' statistics), legacy files are read through and
// imported, and outputs differ only in the cache blocks that record the choice.
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/db/sqlite.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/rows_cache.hpp"
#include "atx/engine/research/store/store.hpp"
#include "strategy_ic_cache_index.hpp"
#include "strategy_ic_runner.hpp"
#include "strategy_marginal_ic.hpp"
#include "strategy_marginal_pair_cache.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace st = atx::impl::strategy;
namespace store = atx::engine::research::store;
constexpr usize D = 480, N = 8;
constexpr i64 day = 86'400'000'000'000LL;

// A short scratch name (the runner tests' MAX_PATH note: a Release cache identity nests 40
// characters below the cache root).
struct Directory {
  fs::path path;
  Directory() {
    static std::atomic<unsigned> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000;
    for (unsigned a = 0; a < 32; ++a) {
      auto candidate = fs::temp_directory_path() / ("ici-" + std::to_string(stamp) + "-" +
                                                    std::to_string(sequence.fetch_add(1)));
      if (fs::create_directory(candidate)) {
        path = std::move(candidate);
        break;
      }
    }
  }
  ~Directory() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};

template <class T>
bool payload(const fs::path& dir, Json& files, const char* name, const std::vector<T>& data) {
  std::ofstream f(dir / name, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(data));
  // SAFETY: char views the object representation of the trivially copyable payload.
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  f.close();
  if (!f) return false;
  auto sha = core::sha256_file((dir / name).string());
  if (!sha) return false;
  files[name] = {{"bytes", bytes.size()}, {"sha256", *sha}};
  return true;
}
bool json_file(const fs::path& path, const Json& j, std::string& sha) {
  const auto text = j.dump(2) + "\n";
  std::ofstream out(path, std::ios::binary);
  out << text;
  out.close();
  auto digest = core::sha256_hex(text);
  if (!out || !digest) return false;
  sha = *digest;
  return true;
}
Json read_json(const fs::path& path) {
  std::ifstream in(path);
  return Json::parse(in);
}
std::string file_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::string file_sha(const fs::path& path) {
  auto sha = core::sha256_file(path.string());
  return sha ? *sha : std::string{};
}

// The runner tests' synthetic role (strategy_ic_runner_test.cpp role()): drifting closes and a
// fixed volume ladder over D sessions and N names, members from session 63.
bool role(const fs::path& dir, i64 first_day, std::string& sha, f64 direction) {
  if (!fs::create_directory(dir)) return false;
  std::vector<i64> sessions(D);
  std::vector<u8> member(D * N, 0), present(D * N, 1);
  std::vector<f64> price(D * N), volume(D * N);
  for (usize d = 0; d < D; ++d) {
    sessions[d] = (first_day + static_cast<i64>(d)) * day;
    for (usize i = 0; i < N; ++i) {
      const auto drift = direction * (static_cast<f64>(i) - 3.5) * .00002;
      price[d * N + i] = 100 * std::exp(drift * static_cast<f64>(d));
      volume[d * N + i] = 1e8 * static_cast<f64>(i + 1);
      member[d * N + i] = static_cast<u8>(d >= 63);
    }
  }
  Json files;
  if (!payload(dir, files, "sessions.i64", sessions) ||
      !payload(dir, files, "ids.u64", std::vector<u64>{10, 20, 30, 40, 50, 60, 70, 80}) ||
      !payload(dir, files, "member.u8", member) || !payload(dir, files, "present.u8", present) ||
      !payload(dir, files, "close.f64", price) || !payload(dir, files, "raw_close.f64", price) ||
      !payload(dir, files, "volume.f64", volume))
    return false;
  const Json membership{{"rule", "research-prior63-usd-adv-topn-v1"}, {"top_n", N},
      {"lookback_sessions", 63}, {"lag_sessions", 1}, {"min_raw_price_exclusive", 5},
      {"min_adv_exclusive", 5000000}, {"ties", "securityID-ascending"},
      {"missing", "complete-prior-calendar-window-required"}, {"common_stock_verified", false}};
  return json_file(dir / "manifest.json",
      {{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
       {"instrument_namespace", "spiderrock.securityID"}, {"dates", D}, {"instruments", N},
       {"score_begin", 383}, {"score_end", D}, {"score_start_ns", sessions[383]},
       {"score_end_ns", sessions.back() + day}, {"source_sha256", std::string(64, 'a')},
       {"membership_recipe", membership.dump()},
       {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
       {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
       {"volume_basis", "raw-share-volume"}, {"common_stock_verified", false},
       {"historical_vintage_verified", false},
       {"declared_output_bytes", D * N * 26 + D * 8 + N * 8}, {"files", files}},
      sha);
}
// A TRAIN and a VALIDATION role, the two-candidate library, the saved exact blend (the marginal
// verb's pool) and a cache-free config.
bool fixture(const Directory& dir, st::IcRunnerConfig& cfg) {
  if (dir.path.empty()) return false;
  cfg.library_path = (dir.path / "library.json").string();
  cfg.train_manifest = (dir.path / "train" / "manifest.json").string();
  cfg.validation_manifest = (dir.path / "validation" / "manifest.json").string();
  cfg.min_names = 3;
  cfg.min_dates = 8;
  cfg.max_working_bytes = 64ULL << 20;
  cfg.save_combined = true;
  if (!role(dir.path / "train", 17683, cfg.train_sha256, 1) ||
      !role(dir.path / "validation", 18200, cfg.validation_sha256, -1))
    return false;
  return json_file(cfg.library_path,
      {{"schema", "atx.dsl-ic-library/v1"}, {"id", "synthetic-ic-two"},
       {"fields",
        Json::array({{{"name", "close"}}, {{"name", "raw_close"}}, {{"name", "volume"}}})},
       {"families", Json::array({{{"id", "fixed_volume"}}})},
       {"candidates", Json::array({{{"id", "volume_level"}, {"family", "fixed_volume"},
                                    {"dsl", "volume"}, {"sign_policy", "train-rank-ic21"},
                                    {"horizons", {5, 21, 63}}},
                                   {{"id", "volume_rank"}, {"family", "fixed_volume"},
                                    {"dsl", "rank(volume)"}, {"sign_policy", "train-rank-ic21"},
                                    {"horizons", {5, 21, 63}}}})}},
      cfg.library_sha256);
}

struct Run {
  bool ok{};
  std::string error, log;
};
Run run_named(const Directory& dir, st::IcRunnerConfig& cfg, const std::string& name) {
  cfg.output_directory = (dir.path / name).string();
  std::ostringstream progress;
  const auto status = st::run_ic(cfg, progress);
  return {static_cast<bool>(status), status ? std::string{} : status.error().to_string(),
          progress.str()};
}
// The IC series, planned targets, orientations and saved blend: bit-identical with or without a
// cache, files or rows.
std::vector<std::string> exact_outputs() {
  std::vector<std::string> out{"recipe.json", "orientations.json"};
  for (const std::string role_name : {"train", "validation"})
    for (const auto* suffix : {"_daily_ic.csv", "_planned_targets.csv", "_combined.f64",
                               "_combined_member.u8", "_combined_finite.u8", "_combined.json"})
      out.push_back(role_name + suffix);
  return out;
}
void expect_exact_outputs(const Directory& dir, const std::string& reference,
                          const std::string& name) {
  for (const auto& file : exact_outputs()) {
    const auto expected = file_sha(dir.path / reference / file);
    ASSERT_FALSE(expected.empty()) << reference << ' ' << file;
    EXPECT_EQ(file_sha(dir.path / name / file), expected) << name << ' ' << file;
  }
}
// The document without its timings (every key ending in "seconds", at any depth).
Json without_seconds(Json j) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end();) {
      if (it.key().ends_with("seconds")) {
        it = j.erase(it);
      } else {
        *it = without_seconds(*it);
        ++it;
      }
    }
  } else if (j.is_array()) {
    for (auto& item : j) item = without_seconds(item);
  }
  return j;
}
// A role result without timings, resources and cache bookkeeping (the runner tests' rule).
Json stable_role(Json result) {
  for (const auto* key : {"wall_seconds", "stage_seconds", "workers", "admitted_working_bytes",
                          "ic_scratch_bytes", "candidate_cache", "hash_seconds", "verify_bytes"})
    result.erase(key);
  for (auto& candidate : result.at("candidates")) {
    for (const auto* key : {"wall_seconds", "stage_seconds", "signal_cache", "ic_result_cache",
                            "composition_weight", "composition_sign"})
      candidate.erase(key);
  }
  return result;
}
// Every regular file under `root` (relative '/'-path -> SHA-256), the index files excepted.
std::map<std::string, std::string> cache_files(const fs::path& root) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto name = entry.path().filename().string();
    if (name.starts_with(std::string(store::kCacheIndexName))) continue;
    out[fs::relative(entry.path(), root).generic_string()] = file_sha(entry.path());
  }
  return out;
}
usize count_ending(const std::map<std::string, std::string>& files, std::string_view suffix) {
  return static_cast<usize>(std::count_if(files.begin(), files.end(), [&](const auto& file) {
    return file.first.ends_with(suffix);
  }));
}
// `atx-research-store cache init DIR`'s create_cache (the only way an index is made).
bool init_index(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  return !ec && static_cast<bool>(store::create_cache(dir));
}
// One statement on DIR/index.sqlite through a connection of the test's own.
bool index_exec(const fs::path& dir, const std::string& sql) {
  auto db = store::open_cache(dir);
  return db && static_cast<bool>(db->exec(sql));
}
std::vector<store::IcSignalRow> signal_rows(const fs::path& dir) {
  auto db = store::open_cache(dir);
  if (!db) return {};
  auto rows = store::select_all_ic_signal(*db);
  return rows ? *rows : std::vector<store::IcSignalRow>{};
}
std::vector<store::IcResultRow> result_rows(const fs::path& dir) {
  auto db = store::open_cache(dir);
  if (!db) return {};
  auto rows = store::select_all_ic_result(*db);
  return rows ? *rows : std::vector<store::IcResultRow>{};
}
// The IC CLI on TRAIN in plan-only mode plus `extra`; its exit code, `out` its stdout.
int ic_cli(const st::IcRunnerConfig& cfg, const std::vector<std::string>& extra,
           std::string& out) {
  std::vector<std::string> args{"atx-equity-strategy-ic", "--library", cfg.library_path,
      "--library-sha256", cfg.library_sha256, "--train", cfg.train_manifest, "--train-sha256",
      cfg.train_sha256, "--min-names", "3", "--min-dates", "8", "--plan-only"};
  args.insert(args.end(), extra.begin(), extra.end());
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  std::ostringstream stdout_log, stderr_log;
  const int code =
      st::dispatch_ic(static_cast<int>(argv.size()), argv.data(), stdout_log, stderr_log);
  out = stdout_log.str();
  return code;
}
} // namespace

// An index.sqlite in the cache dir and no flag: the cache files, their bytes and the summary
// (timings aside) are exactly those of a cache dir without one, and the index is never opened.
TEST(IcCacheIndex, FlagAbsentKeepsFileLayoutAndSummary) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto cache = dir.path / "c";
  cfg.candidate_cache_directory = cache.string();
  std::map<std::string, Json> summaries;
  std::map<std::string, std::string> files;
  for (const bool with_index : {false, true}) {
    std::error_code ec;
    fs::remove_all(cache, ec);
    fs::remove_all(dir.path / "cold", ec);
    fs::remove_all(dir.path / "warm", ec);
    std::string index_sha;
    if (with_index) {
      ASSERT_TRUE(init_index(cache));
      index_sha = file_sha(cache / std::string(store::kCacheIndexName));
    }
    for (const std::string name : {"cold", "warm"}) {
      const auto run = run_named(dir, cfg, name);
      ASSERT_TRUE(run.ok) << run.error;
      EXPECT_EQ(run.log.find("cache-index"), std::string::npos) << run.log;
      const auto summary = without_seconds(read_json(dir.path / name / "summary.json"));
      if (with_index) {
        EXPECT_EQ(summary.dump(), summaries.at(name).dump()) << name;
        for (const auto& role_result : summary.at("roles"))
          EXPECT_FALSE(role_result.at("candidate_cache").contains("index"));
      } else {
        summaries[name] = summary;
      }
      if (!with_index) {
        for (const auto& file : exact_outputs())
          files[name + "/" + file] = file_sha(dir.path / name / file);
      } else {
        for (const auto& file : exact_outputs())
          EXPECT_EQ(file_sha(dir.path / name / file), files.at(name + "/" + file)) << file;
      }
    }
    const auto listing = cache_files(cache);
    if (!with_index) {
      files["cache"] = Json(listing).dump();
      // Four signal payloads and sidecars, four IC-result entries.
      EXPECT_EQ(count_ending(listing, ".f64"), 4U);
      EXPECT_EQ(count_ending(listing, ".json"), 8U);
    } else {
      EXPECT_EQ(Json(listing).dump(), files.at("cache"));
      // Never opened: same bytes, no WAL or shared-memory file beside it.
      EXPECT_EQ(file_sha(cache / std::string(store::kCacheIndexName)), index_sha);
      EXPECT_FALSE(fs::exists(cache / (std::string(store::kCacheIndexName) + "-wal")));
      EXPECT_FALSE(fs::exists(cache / (std::string(store::kCacheIndexName) + "-shm")));
    }
  }
}

// --candidate-cache-index refuses (exit 2) without --candidate-cache, with an unknown value and
// with sqlite but no DIR/index.sqlite; files is the default; the API refuses the same configs.
TEST(IcCacheIndex, FlagWithoutIndexRefused) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto cache = (dir.path / "c").string();
  std::string out;
  EXPECT_EQ(ic_cli(cfg, {"--candidate-cache-index", "sqlite"}, out), 2);
  EXPECT_EQ(ic_cli(cfg, {"--candidate-cache-index", "files"}, out), 2);
  EXPECT_EQ(ic_cli(cfg, {"--candidate-cache", cache, "--candidate-cache-index", "rows"}, out), 2);
  EXPECT_EQ(ic_cli(cfg, {"--candidate-cache", cache, "--candidate-cache-index", "sqlite"}, out), 2);
  EXPECT_FALSE(fs::exists(fs::path(cache) / std::string(store::kCacheIndexName)));
  EXPECT_EQ(ic_cli(cfg, {"--candidate-cache", cache, "--candidate-cache-index", "files"}, out), 0);
  ASSERT_TRUE(init_index(cache));
  EXPECT_EQ(ic_cli(cfg, {"--candidate-cache", cache, "--candidate-cache-index", "sqlite"}, out), 0);
  // run_ic refuses before any payload or output: no cache dir, or a file-layout report.
  auto attempt = cfg;
  attempt.candidate_cache_index = st::CacheIndexMode::Sqlite;
  auto refused = run_named(dir, attempt, "no_cache");
  EXPECT_FALSE(refused.ok);
  EXPECT_NE(refused.error.find("needs --candidate-cache"), std::string::npos) << refused.error;
  EXPECT_FALSE(fs::exists(dir.path / "no_cache"));
  attempt.candidate_cache_directory = cache;
  attempt.cache_report = true;
  refused = run_named(dir, attempt, "report");
  EXPECT_FALSE(refused.ok);
  EXPECT_NE(refused.error.find("--cache-report"), std::string::npos) << refused.error;
  EXPECT_FALSE(fs::exists(dir.path / "report"));
}

// The ic_signal row is the entry's commit marker: an indexed run writes payloads and rows, no
// sidecar or IC-result file; a payload whose row is gone is a miss (the identical payload is
// adopted, not rewritten) and the row comes back.
TEST(IcCacheIndex, RowIsTheCommitMarker) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto plain = run_named(dir, cfg, "plain");
  ASSERT_TRUE(plain.ok) << plain.error;
  const auto cache = dir.path / "c";
  ASSERT_TRUE(init_index(cache));
  cfg.candidate_cache_directory = cache.string();
  cfg.candidate_cache_index = st::CacheIndexMode::Sqlite;
  const auto cold = run_named(dir, cfg, "cold");
  ASSERT_TRUE(cold.ok) << cold.error;
  expect_exact_outputs(dir, "plain", "cold");
  const auto listing = cache_files(cache);
  EXPECT_EQ(count_ending(listing, ".f64"), 4U);
  EXPECT_EQ(count_ending(listing, ".json"), 0U); // no sidecar, no IC-result file
  ASSERT_EQ(signal_rows(cache).size(), 4U);
  EXPECT_EQ(result_rows(cache).size(), 4U);
  std::string level_payload;
  for (const auto& row : signal_rows(cache))
    if (row.id == "volume_level" && row.role_sha256 == cfg.train_sha256)
      level_payload = row.payload;
  ASSERT_FALSE(level_payload.empty());
  const auto payload_path = cache / level_payload;
  const auto payload_sha = file_sha(payload_path);
  const auto payload_time = fs::last_write_time(payload_path);
  ASSERT_TRUE(index_exec(cache, "DELETE FROM ic_signal WHERE id = 'volume_level' AND "
                                "role_sha256 = '" + cfg.train_sha256 + "';"));
  ASSERT_EQ(signal_rows(cache).size(), 3U);
  const auto resumed = run_named(dir, cfg, "resumed");
  ASSERT_TRUE(resumed.ok) << resumed.error;
  for (const auto* line :
       {"IC cache-miss volume_level role=train", "IC cache-hit volume_rank role=train",
        "IC cache-hit volume_level role=validation"})
    EXPECT_NE(resumed.log.find(line), std::string::npos) << line << "\n" << resumed.log;
  EXPECT_EQ(file_sha(payload_path), payload_sha);
  EXPECT_TRUE(fs::last_write_time(payload_path) == payload_time); // adopted, not rewritten
  EXPECT_EQ(signal_rows(cache).size(), 4U);
  expect_exact_outputs(dir, "plain", "resumed");
}

// A cache written without the index (v2 sidecars and IC-result files) is read through by an
// indexed run, verified as today, and imported: each row holds its file's exact text; with the
// files gone, the next indexed run is served from the rows alone.
TEST(IcCacheIndex, ReadThroughImportsLegacySidecar) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto cache = dir.path / "c";
  cfg.candidate_cache_directory = cache.string();
  const auto legacy = run_named(dir, cfg, "legacy");
  ASSERT_TRUE(legacy.ok) << legacy.error;
  ASSERT_TRUE(init_index(cache));
  cfg.candidate_cache_index = st::CacheIndexMode::Sqlite;
  const auto through = run_named(dir, cfg, "through");
  ASSERT_TRUE(through.ok) << through.error;
  EXPECT_EQ(through.log.find("IC VM-"), std::string::npos); // every signal served
  const auto summary = read_json(dir.path / "through" / "summary.json");
  for (const auto& role_result : summary.at("roles")) {
    EXPECT_EQ(role_result.at("candidate_cache").at("hits"), 2);
    EXPECT_EQ(role_result.at("candidate_cache").at("ic_results").at("hits"), 2);
  }
  expect_exact_outputs(dir, "legacy", "through");
  // One row per legacy entry, holding the file's bytes verbatim.
  const auto signals = signal_rows(cache);
  ASSERT_EQ(signals.size(), 4U);
  for (const auto& row : signals) {
    const auto sidecar = (cache / row.payload).replace_extension(".json");
    EXPECT_EQ(row.sidecar, file_text(sidecar)) << row.id;
    EXPECT_EQ(row.schema, "atx.dsl-candidate-signal/v2");
    EXPECT_EQ(row.payload_sha256, file_sha(cache / row.payload));
  }
  const auto results = result_rows(cache);
  ASSERT_EQ(results.size(), 4U);
  for (const auto& row : results)
    EXPECT_EQ(row.record, file_text(cache / row.entry_dir / (row.id + ".json"))) << row.id;
  // The legacy files go; the rows serve every entry.
  for (const auto& [file, sha] : cache_files(cache)) {
    (void)sha;
    if (file.ends_with(".json")) ASSERT_TRUE(fs::remove(cache / file)) << file;
  }
  const auto rows_only = run_named(dir, cfg, "rows_only");
  ASSERT_TRUE(rows_only.ok) << rows_only.error;
  EXPECT_EQ(rows_only.log.find("IC VM-"), std::string::npos);
  const auto served = read_json(dir.path / "rows_only" / "summary.json");
  for (const auto& role_result : served.at("roles")) {
    EXPECT_EQ(role_result.at("candidate_cache").at("hits"), 2);
    EXPECT_EQ(role_result.at("candidate_cache").at("ic_results").at("hits"), 2);
  }
  expect_exact_outputs(dir, "legacy", "rows_only");
}

// A row whose key parts or payload differ from the request refuses as a mismatching sidecar
// does today (never served, never silently replaced).
TEST(IcCacheIndex, KeyMismatchRefuses) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto cache = dir.path / "c";
  ASSERT_TRUE(init_index(cache));
  cfg.candidate_cache_directory = cache.string();
  cfg.candidate_cache_index = st::CacheIndexMode::Sqlite;
  const auto cold = run_named(dir, cfg, "cold");
  ASSERT_TRUE(cold.ok) << cold.error;
  const std::string where =
      " WHERE id = 'volume_rank' AND role_sha256 = '" + cfg.train_sha256 + "';";
  const std::vector<std::pair<std::string, std::string>> tampers{
      {"UPDATE ic_signal SET vm_identity = 'dslvm0_other'" + where,
       "UPDATE ic_signal SET vm_identity = (SELECT vm_identity FROM ic_signal WHERE id = "
       "'volume_level' AND role_sha256 = '" + cfg.train_sha256 + "')" + where},
      {"UPDATE ic_signal SET payload_sha256 = '" + std::string(64, '0') + "'" + where,
       "UPDATE ic_signal SET payload_sha256 = json_extract(sidecar, '$.payload_sha256')" + where}};
  for (usize k = 0; k < tampers.size(); ++k) {
    ASSERT_TRUE(index_exec(cache, tampers[k].first)) << k;
    const auto name = "tampered" + std::to_string(k);
    const auto refused = run_named(dir, cfg, name);
    EXPECT_FALSE(refused.ok) << k;
    EXPECT_NE(refused.error.find("candidate cache entry mismatch: volume_rank"), std::string::npos)
        << refused.error;
    EXPECT_EQ(read_json(dir.path / name / "summary.json").at("status"), "failed");
    ASSERT_TRUE(index_exec(cache, tampers[k].second)) << k;
  }
  const auto repaired = run_named(dir, cfg, "repaired");
  ASSERT_TRUE(repaired.ok) << repaired.error;
  expect_exact_outputs(dir, "cold", "repaired");
}

// Two connections (two CacheIndex objects on one DIR, as two IC exes would hold) write from two
// threads at once: every row lands, a key both write is inserted once and found present once.
TEST(IcCacheIndex, TwoConnectionsInterleave) {
  Directory dir;
  ASSERT_FALSE(dir.path.empty());
  ASSERT_TRUE(init_index(dir.path));
  auto first = st::CacheIndex::open(dir.path, nullptr, st::IndexAccess::ReadWrite);
  auto second = st::CacheIndex::open(dir.path, nullptr, st::IndexAccess::ReadWrite);
  ASSERT_TRUE(first && second);
  const auto row = [](const std::string& id) {
    const std::string record = R"({"id":")" + id + R"("})";
    auto sha = core::sha256_hex(record);
    return store::IcResultRow{"e/ic1_x", id, std::string(64, 'a'), record,
                              sha ? *sha : std::string(64, '0')};
  };
  constexpr usize per_thread = 48;
  std::atomic<unsigned> failures{0}, inserted_shared{0}, present_shared{0};
  const auto writer = [&](st::CacheIndex& index, const std::string& tag) {
    for (usize k = 0; k < per_thread; ++k) {
      const auto own = index.put_ic_result(row(tag + std::to_string(k)), false);
      if (!own || own->outcome != st::IndexPut::Inserted) ++failures;
      const auto shared_row = row("shared" + std::to_string(k));
      const auto shared = index.put_ic_result(shared_row, false);
      if (!shared || shared->outcome == st::IndexPut::Skipped) {
        ++failures;
      } else if (shared->outcome == st::IndexPut::Inserted) {
        ++inserted_shared;
      } else if (shared->existing && shared->existing->record == shared_row.record) {
        ++present_shared;
      } else {
        ++failures;
      }
      const auto found = index.find_ic_result("e/ic1_x", tag + std::to_string(k));
      if (!found || !*found) ++failures;
    }
  };
  {
    std::jthread a(writer, std::ref(*first), std::string("a"));
    std::jthread b(writer, std::ref(*second), std::string("b"));
  }
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(inserted_shared.load(), per_thread);
  EXPECT_EQ(present_shared.load(), per_thread);
  EXPECT_EQ(result_rows(dir.path).size(), 3U * per_thread);
}

// With the flag the summary's candidate_cache block (and --plan-only's) records the index; with
// `files` given or the flag absent it does not. A cold and a warm indexed pass reproduce the
// uncached outputs exactly, the warm one with every signal and IC result served from rows.
TEST(IcCacheIndex, SummaryRecordsIndex) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto plain = run_named(dir, cfg, "plain");
  ASSERT_TRUE(plain.ok) << plain.error;
  const auto reference = read_json(dir.path / "plain" / "summary.json");
  auto files_cfg = cfg;
  files_cfg.candidate_cache_directory = (dir.path / "f").string();
  files_cfg.candidate_cache_index = st::CacheIndexMode::Files;
  const auto files = run_named(dir, files_cfg, "files");
  ASSERT_TRUE(files.ok) << files.error;
  for (const auto& role_result : read_json(dir.path / "files" / "summary.json").at("roles"))
    EXPECT_FALSE(role_result.at("candidate_cache").contains("index"));
  const auto cache = dir.path / "c";
  ASSERT_TRUE(init_index(cache));
  cfg.candidate_cache_directory = cache.string();
  cfg.candidate_cache_index = st::CacheIndexMode::Sqlite;
  for (const std::string name : {"cold", "warm"}) {
    const bool warm = name == "warm";
    const auto run = run_named(dir, cfg, name);
    ASSERT_TRUE(run.ok) << run.error;
    if (warm) EXPECT_EQ(run.log.find("IC VM-"), std::string::npos);
    const auto summary = read_json(dir.path / name / "summary.json");
    EXPECT_EQ(summary.at("recipe_sha256"), reference.at("recipe_sha256"));
    ASSERT_EQ(summary.at("roles").size(), 2U);
    for (usize r = 0; r < 2; ++r) {
      const auto& result = summary.at("roles").at(r);
      const auto& block = result.at("candidate_cache");
      EXPECT_EQ(block.at("index"), "sqlite");
      EXPECT_EQ(block.at("hits"), warm ? 2 : 0) << name;
      EXPECT_EQ(block.at("ic_results").at("hits"), warm ? 2 : 0) << name;
      EXPECT_EQ(stable_role(result), stable_role(reference.at("roles").at(r))) << name << r;
    }
    expect_exact_outputs(dir, "plain", name);
  }
  // --plan-only reads the rows (read-only) and records the index in each role's plan.
  std::string out;
  ASSERT_EQ(ic_cli(cfg, {"--candidate-cache", cache.string(), "--candidate-cache-index", "sqlite"},
                   out),
            0);
  const auto plan = Json::parse(out);
  ASSERT_EQ(plan.at("candidate_cache").size(), 1U);
  EXPECT_EQ(plan.at("candidate_cache").at(0).at("index"), "sqlite");
  EXPECT_EQ(plan.at("candidate_cache").at(0).at("ready_entries"), 2);
}

// An IC-result row holds the self-hashed text of today's file byte for byte, keyed by the file's
// directory under DIR and its stem, with the scope key's and the record's SHA-256.
TEST(IcResultIndex, RecordTextRoundTrip) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto files_cache = dir.path / "f";
  auto files_cfg = cfg;
  files_cfg.candidate_cache_directory = files_cache.string();
  const auto files = run_named(dir, files_cfg, "files");
  ASSERT_TRUE(files.ok) << files.error;
  const auto cache = dir.path / "c";
  ASSERT_TRUE(init_index(cache));
  cfg.candidate_cache_directory = cache.string();
  cfg.candidate_cache_index = st::CacheIndexMode::Sqlite;
  const auto cold = run_named(dir, cfg, "cold");
  ASSERT_TRUE(cold.ok) << cold.error;
  const auto summary = read_json(dir.path / "cold" / "summary.json");
  const auto rows = result_rows(cache);
  ASSERT_EQ(rows.size(), 4U);
  for (const auto& row : rows) {
    EXPECT_EQ(row.record, file_text(files_cache / row.entry_dir / (row.id + ".json"))) << row.id;
    const auto entry = Json::parse(row.record);
    EXPECT_EQ(entry.at("schema"), "atx.dsl-candidate-ic/v1");
    EXPECT_EQ(row.record_sha256, entry.at("record_sha256").get<std::string>());
    auto record_sha = core::sha256_hex(entry.at("record").dump());
    ASSERT_TRUE(record_sha);
    EXPECT_EQ(row.record_sha256, *record_sha);
    auto key_sha = core::sha256_hex(entry.at("record").at("key").dump());
    ASSERT_TRUE(key_sha);
    EXPECT_EQ(row.key_sha256, *key_sha);
    bool scoped = false;
    for (const auto& role_result : summary.at("roles")) {
      const auto& ic = role_result.at("candidate_cache").at("ic_results");
      scoped = scoped || (row.entry_dir.ends_with(ic.at("subdirectory").get<std::string>()) &&
                          ic.at("key") == entry.at("record").at("key"));
    }
    EXPECT_TRUE(scoped) << row.entry_dir;
  }
  const auto warm = run_named(dir, cfg, "warm");
  ASSERT_TRUE(warm.ok) << warm.error;
  for (const auto& role_result : read_json(dir.path / "warm" / "summary.json").at("roles"))
    EXPECT_EQ(role_result.at("candidate_cache").at("ic_results").at("hits"), 2);
  expect_exact_outputs(dir, "files", "warm");
}

// A row failing its self-hash is damage to the derived index: never served, a miss with a
// progress line, rescored to the same bytes, and replaced by an intact row.
TEST(IcResultIndex, TamperedRecordIsMiss) {
  Directory dir;
  st::IcRunnerConfig cfg;
  ASSERT_TRUE(fixture(dir, cfg));
  const auto cache = dir.path / "c";
  ASSERT_TRUE(init_index(cache));
  cfg.candidate_cache_directory = cache.string();
  cfg.candidate_cache_index = st::CacheIndexMode::Sqlite;
  const auto cold = run_named(dir, cfg, "cold");
  ASSERT_TRUE(cold.ok) << cold.error;
  ASSERT_TRUE(index_exec(cache,
      "UPDATE ic_result SET record = replace(record, '\"candidate_id\":\"volume_level\"', "
      "'\"candidate_id\":\"volume_levek\"') WHERE id LIKE 'volume_level.%' AND "
      "instr(entry_dir, '" + cfg.train_sha256 + "') > 0;"));
  const auto tampered = run_named(dir, cfg, "tampered");
  ASSERT_TRUE(tampered.ok) << tampered.error;
  EXPECT_NE(tampered.log.find("row fails its self-hash: treated as a miss"), std::string::npos)
      << tampered.log;
  const auto summary = read_json(dir.path / "tampered" / "summary.json");
  const auto& train = summary.at("roles").at(0).at("candidate_cache");
  EXPECT_EQ(train.at("hits"), 2);
  EXPECT_EQ(train.at("ic_results").at("hits"), 1);
  EXPECT_EQ(summary.at("roles").at(1).at("candidate_cache").at("ic_results").at("hits"), 2);
  expect_exact_outputs(dir, "cold", "tampered");
  for (const auto& row : result_rows(cache)) {
    const auto entry = Json::parse(row.record);
    auto record_sha = core::sha256_hex(entry.at("record").dump());
    ASSERT_TRUE(record_sha);
    EXPECT_EQ(*record_sha, row.record_sha256) << row.id; // the damaged row was replaced
  }
}

namespace {
// A pair-cache scope (any valid SHA256s; 100 window rows).
st::MarginalPairScope pair_scope() {
  return st::MarginalPairScope{std::string(64, '1'), std::string(64, '2'), 0, 100, 3};
}
// Every pair of `payloads` synthetic payload SHA256s, with finite sums |sum| <= dates <= 99.
st::MarginalPairStats synthetic_pairs(usize payloads, usize salt) {
  std::vector<std::string> shas;
  for (usize i = 0; i < payloads; ++i) {
    auto sha = core::sha256_hex("payload-" + std::to_string(i));
    shas.push_back(sha ? *sha : std::string{});
  }
  st::MarginalPairStats out;
  for (usize i = 0; i < payloads; ++i)
    for (usize j = i + 1; j < payloads; ++j) {
      const auto x = static_cast<f64>((i * 31U + j * 7U + salt) % 97U);
      out.emplace(st::marginal_pair_key(shas[i], shas[j]),
                  st::MarginalPairStat{(x - 48.0) / 97.0 * 50.0, 50U + (i + j) % 50U});
    }
  return out;
}
bool same_stats(const st::MarginalPairStats& a, const st::MarginalPairStats& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
           return x.first == y.first &&
                  std::bit_cast<u64>(x.second.sum) == std::bit_cast<u64>(y.second.sum) &&
                  x.second.dates == y.second.dates;
         });
}
} // namespace

// The same pairs stored as a shard and as rows read back as the same statistics, bit for bit,
// across more than one 4096-row page; the indexed root never gets a shard directory, and a
// second store of the same pairs inserts nothing.
TEST(MarginalPairIndex, SameStatsAsShards) {
  Directory dir;
  ASSERT_FALSE(dir.path.empty());
  const auto stats = synthetic_pairs(100, 0); // 4950 pairs
  ASSERT_GT(stats.size(), st::kPairPageRows);
  const auto shards_root = dir.path / "shards";
  ASSERT_TRUE(fs::create_directory(shards_root));
  auto empty_files = st::open_marginal_pair_cache(shards_root, pair_scope(), nullptr);
  ASSERT_TRUE(empty_files) << empty_files.error().to_string();
  ASSERT_TRUE(st::store_marginal_pairs(*empty_files, stats));
  auto from_shards = st::open_marginal_pair_cache(shards_root, pair_scope(), nullptr);
  ASSERT_TRUE(from_shards);
  ASSERT_TRUE(same_stats(from_shards->stats, stats));
  const auto rows_root = dir.path / "rows";
  ASSERT_TRUE(init_index(rows_root));
  std::ostringstream progress;
  auto index = st::CacheIndex::open(rows_root, &progress, st::IndexAccess::ReadWrite);
  ASSERT_TRUE(index) << index.error().to_string();
  auto empty_rows = st::open_marginal_pair_cache(rows_root, pair_scope(), &*index);
  ASSERT_TRUE(empty_rows) << empty_rows.error().to_string();
  EXPECT_TRUE(empty_rows->stats.empty());
  EXPECT_EQ(empty_rows->rows_read, 0U);
  const auto stored = st::store_marginal_pair_rows(*empty_rows, stats, *index);
  ASSERT_TRUE(stored) << stored.error().to_string();
  EXPECT_EQ(stored->written, stats.size());
  EXPECT_FALSE(stored->skipped);
  auto from_rows = st::open_marginal_pair_cache(rows_root, pair_scope(), &*index);
  ASSERT_TRUE(from_rows) << from_rows.error().to_string();
  EXPECT_TRUE(same_stats(from_rows->stats, from_shards->stats));
  EXPECT_EQ(from_rows->rows_read, stats.size());
  EXPECT_EQ(from_rows->shards, 0U);
  EXPECT_EQ(from_rows->key_sha, from_shards->key_sha);
  EXPECT_FALSE(fs::exists(from_rows->directory));
  const auto again = st::store_marginal_pair_rows(*from_rows, stats, *index);
  ASSERT_TRUE(again) << again.error().to_string();
  EXPECT_EQ(again->written, 0U);
  EXPECT_TRUE(progress.str().empty()) << progress.str();
}

// A pair already held with other bits refuses the whole store (as two shards with different
// statistics refuse), and nothing of that store is written; a key row of another key text
// refuses the open.
TEST(MarginalPairIndex, ConflictingBitsRefused) {
  Directory dir;
  ASSERT_TRUE(init_index(dir.path));
  auto index = st::CacheIndex::open(dir.path, nullptr, st::IndexAccess::ReadWrite);
  ASSERT_TRUE(index);
  const auto stats = synthetic_pairs(8, 1);
  auto cache = st::open_marginal_pair_cache(dir.path, pair_scope(), &*index);
  ASSERT_TRUE(cache);
  ASSERT_TRUE(st::store_marginal_pair_rows(*cache, stats, *index));
  auto conflicting = synthetic_pairs(9, 1); // the 8 payloads' pairs plus 8 new ones
  auto& changed = conflicting.at(stats.begin()->first).sum;
  changed = std::nextafter(changed, std::numeric_limits<f64>::infinity());
  auto reopened = st::open_marginal_pair_cache(dir.path, pair_scope(), &*index);
  ASSERT_TRUE(reopened);
  const auto refused = st::store_marginal_pair_rows(*reopened, conflicting, *index);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("one pair, two statistics"), std::string::npos)
      << refused.error().to_string();
  auto after = st::open_marginal_pair_cache(dir.path, pair_scope(), &*index);
  ASSERT_TRUE(after);
  EXPECT_TRUE(same_stats(after->stats, stats)); // the new pairs were rolled back too
  ASSERT_TRUE(index_exec(dir.path, "UPDATE pair_key SET key_text = '{}';"));
  const auto foreign = st::open_marginal_pair_cache(dir.path, pair_scope(), &*index);
  ASSERT_FALSE(foreign);
  EXPECT_NE(foreign.error().to_string().find("index row of another key"), std::string::npos)
      << foreign.error().to_string();
}

// Legacy shards under an indexed root are read exactly as without the index and imported (in
// 4096-row chunks, the key row last); with the shards gone the rows alone serve the key.
TEST(MarginalPairIndex, ReadsLegacyShards) {
  Directory dir;
  ASSERT_TRUE(init_index(dir.path));
  const auto legacy = synthetic_pairs(100, 3);
  auto files = st::open_marginal_pair_cache(dir.path, pair_scope(), nullptr);
  ASSERT_TRUE(files);
  ASSERT_TRUE(st::store_marginal_pairs(*files, legacy));
  auto index = st::CacheIndex::open(dir.path, nullptr, st::IndexAccess::ReadWrite);
  ASSERT_TRUE(index);
  auto imported = st::open_marginal_pair_cache(dir.path, pair_scope(), &*index);
  ASSERT_TRUE(imported) << imported.error().to_string();
  EXPECT_EQ(imported->shards, 1U);
  EXPECT_EQ(imported->rows_read, 0U);
  EXPECT_EQ(imported->rows_imported, legacy.size());
  EXPECT_FALSE(imported->legacy_pending);
  EXPECT_TRUE(same_stats(imported->stats, legacy));
  std::error_code ec;
  fs::remove_all(imported->directory, ec);
  ASSERT_FALSE(ec);
  auto from_rows = st::open_marginal_pair_cache(dir.path, pair_scope(), &*index);
  ASSERT_TRUE(from_rows) << from_rows.error().to_string();
  EXPECT_EQ(from_rows->shards, 0U);
  EXPECT_EQ(from_rows->rows_read, legacy.size());
  EXPECT_TRUE(same_stats(from_rows->stats, legacy));
}

namespace {
// The marginal verb over the IC fixture's u pass (its candidate cache and saved TRAIN blend).
Json marginal_into(const Directory& dir, st::MarginalIcConfig cfg, const std::string& name) {
  cfg.output_directory = (dir.path / name).string();
  std::ostringstream progress;
  const auto status = st::run_marginal_ic(cfg, progress);
  if (!status) throw std::runtime_error(name + ": " + status.error().to_string());
  return read_json(dir.path / name / "marginal_ic.json");
}
Json without_keys(Json out, std::initializer_list<const char*> keys,
                  std::initializer_list<const char*> inputs) {
  out.erase(std::string("stage_seconds"));
  for (const char* key : keys) out.erase(std::string(key));
  for (const char* key : inputs) out.at("inputs").erase(std::string(key));
  return out;
}
} // namespace

// The marginal verb with --pair-cache: an index.sqlite in DIR and no flag leaves every byte (the
// timings aside) and the index unchanged; with --pair-cache-index sqlite a cold and a warm run
// give the shard run's rows, and only the pair_cache block (and the admitted bytes, which count
// loaded pairs) differ.
TEST(MarginalPairIndex, FlagAbsentBlockUnchanged) {
  Directory dir;
  st::IcRunnerConfig ic;
  ASSERT_TRUE(fixture(dir, ic));
  ic.candidate_cache_directory = (dir.path / "c").string();
  const auto u = run_named(dir, ic, "u");
  ASSERT_TRUE(u.ok) << u.error;
  st::MarginalIcConfig cfg;
  cfg.candidate_cache_directory = ic.candidate_cache_directory;
  cfg.library_path = ic.library_path;
  cfg.library_sha256 = ic.library_sha256;
  cfg.pool_path = (dir.path / "u" / "train_combined.json").string();
  cfg.role_manifest = ic.train_manifest;
  cfg.min_names = 3;
  const auto pairs = dir.path / "p";
  cfg.pair_cache_directory = pairs.string();
  ASSERT_TRUE(fs::create_directory(pairs));
  const auto shards = marginal_into(dir, cfg, "shards");
  ASSERT_EQ(shards.at("inputs").at("pair_cache").at("computed"), 1U);
  std::error_code ec;
  fs::remove_all(pairs, ec);
  ASSERT_TRUE(init_index(pairs));
  const auto index_sha = file_sha(pairs / std::string(store::kCacheIndexName));
  const auto absent = marginal_into(dir, cfg, "absent");
  EXPECT_EQ(without_keys(absent, {}, {}).dump(), without_keys(shards, {}, {}).dump());
  EXPECT_EQ(file_sha(pairs / std::string(store::kCacheIndexName)), index_sha);
  fs::remove_all(pairs, ec);
  ASSERT_TRUE(init_index(pairs));
  cfg.pair_cache_index = st::CacheIndexMode::Sqlite;
  const auto cold = marginal_into(dir, cfg, "cold");
  const auto warm = marginal_into(dir, cfg, "warm");
  for (const Json* out : {&cold, &warm})
    EXPECT_EQ(without_keys(*out, {"working_bytes"}, {"pair_cache"}).dump(),
              without_keys(shards, {"working_bytes"}, {"pair_cache"}).dump());
  EXPECT_EQ(cold.at("working_bytes"), shards.at("working_bytes"));
  const auto& cold_block = cold.at("inputs").at("pair_cache");
  EXPECT_EQ(cold_block.at("index"), "sqlite");
  EXPECT_FALSE(cold_block.contains("shards_read"));
  EXPECT_EQ(cold_block.at("rows_read"), 0U);
  EXPECT_EQ(cold_block.at("computed"), 1U);
  EXPECT_EQ(cold_block.at("rows_written"), 1U);
  EXPECT_EQ(cold_block.at("key_sha256"), shards.at("inputs").at("pair_cache").at("key_sha256"));
  const auto& warm_block = warm.at("inputs").at("pair_cache");
  EXPECT_EQ(warm_block.at("rows_read"), 1U);
  EXPECT_EQ(warm_block.at("hits"), 1U);
  EXPECT_EQ(warm_block.at("computed"), 0U);
  EXPECT_EQ(warm_block.at("rows_written"), 0U);
  EXPECT_FALSE(warm_block.contains("store_skipped"));
  // No shard was written beside the index.
  for (const auto& entry : fs::directory_iterator(pairs))
    EXPECT_TRUE(entry.path().filename().string().starts_with(std::string(store::kCacheIndexName)))
        << entry.path().string();
  // The CLI's usage refusals (exit 2): no --pair-cache, no index.sqlite in DIR.
  const auto marginal_cli = [&](std::vector<std::string> args) {
    args.insert(args.begin(), "marginal");
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    std::ostringstream out, err;
    return st::dispatch_marginal_ic(static_cast<int>(argv.size()), argv.data(), out, err);
  };
  EXPECT_EQ(marginal_cli({"--pair-cache-index", "sqlite"}), 2);
  EXPECT_EQ(marginal_cli({"--pair-cache", (dir.path / "none").string(), "--pair-cache-index",
                          "sqlite"}),
            2);
  EXPECT_EQ(marginal_cli({"--pair-cache", pairs.string(), "--pair-cache-index", "rows"}), 2);
}
