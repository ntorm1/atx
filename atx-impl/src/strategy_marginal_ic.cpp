#include "strategy_marginal_ic.hpp"
#include "strategy_ic_runner.hpp" // ic_cache_vm_identity: the cache root this build writes

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/combine/group_rerank.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/data/strategy_data.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace cb = atx::engine::combine;
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr f64 quiet_nan = std::numeric_limits<f64>::quiet_NaN();
constexpr usize npos = std::numeric_limits<usize>::max();
// The runner's label recipe: close[d+1+h]/close[d+1]-1, h = 21 (its orientation horizon).
constexpr usize horizon = 21, execution_delay = 1, hac_lag = 21;
constexpr u64 metadata_limit = 1ULL << 20;
constexpr usize max_listing = usize{1} << 16;
// File-format contracts of strategy_ic_runner.cpp (--candidate-cache v2, --save-combined).
constexpr std::string_view signal_schema = "atx.dsl-candidate-signal/v2";
constexpr std::string_view signal_layout = "date-major-little-endian-f64;non-finite-stored-as-quiet-NaN";
constexpr std::string_view combined_schema = "atx.dsl-combined-signal/v1";
constexpr std::string_view library_schema = "atx.dsl-ic-library/v1";
constexpr std::string_view fields_schema = "atx.research-role-fields/v1";
constexpr std::string_view output_schema = "atx.marginal-ic/v1";

co::Error fail(co::ErrorCode code, const std::string& message) { return co::Error{code, "marginal IC: " + message}; }
bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool safe_id(std::string_view s) {
  return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}
std::string text_of(const Json& j, const char* key) {
  return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string{};
}
std::optional<u64> count_of(const Json& j, const char* key) {
  if (j.is_object() && j.contains(key) && j.at(key).is_number_unsigned()) return j.at(key).get<u64>();
  return std::nullopt;
}
Json finite_or_null(f64 v) { return std::isfinite(v) ? Json(v) : Json(nullptr); }

// `limit`: metadata_limit, or ic_fields_manifest_max_bytes for a fields manifest (review B-4).
co::Result<std::string> read_text(const fs::path& path, std::string_view what, u64 limit = metadata_limit) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() <= 0 || static_cast<u64>(in.tellg()) > limit)
    return co::Err(fail(co::ErrorCode::InvalidArgument, std::string(what) + " missing, empty or over " +
        std::to_string(limit >> 20) + " MiB: " + path.string()));
  std::string text(static_cast<usize>(in.tellg()), '\0');
  in.seekg(0); in.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!in) return co::Err(fail(co::ErrorCode::IoError, std::string(what) + " read: " + path.string()));
  return co::Ok(std::move(text));
}
struct Pinned { Json json; std::string sha, text; };
// `pin` empty: the SHA256 is computed and recorded, not checked.
co::Result<Pinned> pinned_json(const fs::path& path, const std::string& pin, std::string_view what,
                               u64 limit = metadata_limit) {
  ATX_TRY(auto text, read_text(path, what, limit));
  ATX_TRY(auto sha, co::sha256_hex(text));
  if (!pin.empty() && pin != sha)
    return co::Err(fail(co::ErrorCode::InvalidArgument, std::string(what) + " SHA256 differs from its pin: " + path.string()));
  auto j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object())
    return co::Err(fail(co::ErrorCode::InvalidArgument, std::string(what) + " is not a JSON object: " + path.string()));
  return co::Ok(Pinned{std::move(j), std::move(sha), std::move(text)});
}

// ---- inputs -------------------------------------------------------------------
struct FileReceipt { fs::path path; std::string sha; u64 bytes{}; };
struct Pool {
  fs::path path; std::string sha;
  usize dates{}, names{}, score_begin{}, score_end{};
  std::string role_sha, library_sha, weights_sha; // weights_sha empty: an unweighted blend
  FileReceipt signal, member, sessions, ids;
};
co::Result<FileReceipt> receipt(const Json& files, const fs::path& dir, const std::string& name, u64 bytes) {
  if (!files.contains(name) || !files.at(name).is_object())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool manifest lacks the file receipt " + name));
  const auto& row = files.at(name); auto sha = text_of(row, "sha256"); const auto recorded = count_of(row, "bytes");
  if (!hash_valid(sha) || !recorded || *recorded != bytes)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool file receipt " + name + " (SHA256 or extent)"));
  return co::Ok(FileReceipt{dir / name, std::move(sha), bytes});
}
co::Result<Pool> read_pool(const MarginalIcConfig& cfg) {
  Pool pool; pool.path = cfg.pool_path;
  ATX_TRY(auto pinned, pinned_json(pool.path, cfg.pool_sha256, "pool combined manifest"));
  const Json& j = pinned.json; pool.sha = pinned.sha;
  const auto dates = count_of(j, "dates"), names = count_of(j, "instruments");
  const auto begin = count_of(j, "score_begin"), end = count_of(j, "score_end");
  const auto role = text_of(j, "role");
  if (text_of(j, "schema") != combined_schema || text_of(j, "status") != "complete" ||
      text_of(j, "layout") != "date-major-little-endian" || !dates || !names || !begin || !end ||
      *dates == 0U || *dates > 4096U || *names == 0U || *names > 20000U || *begin >= *end || *end != *dates ||
      !safe_id(role) || !j.contains("files") || !j.at("files").is_object())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool is not a complete atx.dsl-combined-signal/v1 manifest"));
  pool.dates = static_cast<usize>(*dates); pool.names = static_cast<usize>(*names);
  pool.score_begin = static_cast<usize>(*begin); pool.score_end = static_cast<usize>(*end);
  pool.role_sha = text_of(j, "role_manifest_sha256"); pool.library_sha = text_of(j, "library_sha256");
  if (!hash_valid(pool.role_sha) || !hash_valid(pool.library_sha))
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool role/library SHA256"));
  if (j.contains("composition_weights_sha256")) {
    pool.weights_sha = text_of(j, "composition_weights_sha256");
    if (!hash_valid(pool.weights_sha)) return co::Err(fail(co::ErrorCode::InvalidArgument, "pool composition_weights_sha256"));
  }
  const u64 cells = static_cast<u64>(pool.dates) * pool.names;
  const auto dir = pool.path.parent_path(); const auto prefix = role + "_combined"; const auto& files = j.at("files");
  ATX_TRY(pool.signal, receipt(files, dir, prefix + ".f64", cells * sizeof(f64)));
  ATX_TRY(pool.member, receipt(files, dir, prefix + "_member.u8", cells));
  ATX_TRY(pool.sessions, receipt(files, dir, prefix + "_sessions.i64", pool.dates * sizeof(i64)));
  ATX_TRY(pool.ids, receipt(files, dir, prefix + "_ids.u64", pool.names * sizeof(u64)));
  return co::Ok(std::move(pool));
}

// theme: the library row's `theme`, else its family. sign: the row's prior_sign when +1/-1.
struct Candidate { std::string id, family, theme, dsl_sha; std::optional<int> prior_sign; };
co::Result<std::vector<Candidate>> read_library(const MarginalIcConfig& cfg, std::string& sha) {
  ATX_TRY(auto pinned, pinned_json(cfg.library_path, cfg.library_sha256, "library"));
  const Json& j = pinned.json; sha = pinned.sha;
  if (text_of(j, "schema") != library_schema || !j.contains("candidates") || !j.at("candidates").is_array() ||
      j.at("candidates").empty() || j.at("candidates").size() > 256U)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "library schema or size (1..256 candidates)"));
  std::set<std::string> ids; std::vector<Candidate> out;
  for (const auto& row : j.at("candidates")) {
    Candidate c; c.id = text_of(row, "id"); c.family = text_of(row, "family"); c.theme = text_of(row, "theme");
    if (c.theme.empty()) c.theme = c.family;
    const auto dsl = text_of(row, "dsl");
    if (!safe_id(c.id) || !safe_id(c.family) || !safe_id(c.theme) || dsl.empty() || dsl.size() > 4096U ||
        !ids.insert(c.id).second)
      return co::Err(fail(co::ErrorCode::InvalidArgument, "library candidate identity: " + c.id));
    ATX_TRY(c.dsl_sha, co::sha256_hex(dsl));
    if (row.contains("prior_sign") && row.at("prior_sign").is_number_integer()) {
      const auto s = row.at("prior_sign").get<i64>();
      if (s == 1 || s == -1) c.prior_sign = static_cast<int>(s);
    }
    out.push_back(std::move(c));
  }
  return co::Ok(std::move(out));
}

// The pool's weighted members grouped into theme composites T_j = sum_{k in j} s_k w_k r_k over
// the members' centred ranks r_k (fixed denominator: an unranked member adds 0), which is the
// no-redistribution blend split by theme. Theme names come from the weights file's theme block
// (theme_standardise or theme_redistribution, read by the IC runner's ic_weights_themes, so the
// grouping is the blend's own; review B-2), else from the library rows. Under theme_standardise
// with rerank true each T_j is the blend's theme term instead: its centred tied re-rank over the
// member names with a present member of j (rerank_theme_rows).
struct Themes {
  std::string path, sha;
  std::string block;                    // the weights file's theme block; empty: library themes
  bool rerank{false};                   // theme_standardise with rerank true
  std::vector<std::string> names;       // first appearance in library order
  std::vector<usize> theme_of;          // per library candidate; npos: not a weighted member
  std::vector<f64> signed_weight;       // per library candidate: s_k w_k (0 off the book)
  std::vector<std::optional<int>> sign; // per library candidate: the pinned sign, if any
};
co::Result<int> pinned_sign(const Json& signs, const std::string& id) {
  const auto at = signs.find(id);
  if (at == signs.end() || !at->is_number_integer() || (at->get<i64>() != 1 && at->get<i64>() != -1))
    return co::Err(fail(co::ErrorCode::InvalidArgument, "themes: pinned sign +1/-1 missing for weighted member " + id));
  return co::Ok(static_cast<int>(at->get<i64>()));
}
co::Result<Themes> read_themes(const MarginalIcConfig& cfg, const Pool& pool, const std::vector<Candidate>& lib) {
  Themes t; t.theme_of.assign(lib.size(), npos); t.signed_weight.assign(lib.size(), 0.0);
  t.sign.assign(lib.size(), std::nullopt);
  if (cfg.themes_path.empty()) return co::Ok(std::move(t));
  if (pool.weights_sha.empty())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "--themes needs a pool blended with pinned composition "
        "weights (its manifest names composition_weights_sha256)"));
  ATX_TRY(auto pinned, pinned_json(cfg.themes_path, pool.weights_sha, "themes (the pool's composition weights)"));
  const Json& j = pinned.json; t.path = cfg.themes_path; t.sha = pinned.sha;
  const auto schema = text_of(j, "schema");
  if ((schema != "atx.dsl-composition-weights/v1" && schema != "atx.dsl-composition-weights/v2") ||
      text_of(j, "library_sha256") != pool.library_sha || !j.contains("weights") || !j.at("weights").is_object() ||
      !j.contains("signs") || !j.at("signs").is_object())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "themes: weights schema, pool library binding or signs block"));
  const Json& signs = j.at("signs");
  const auto grouping = ic_weights_themes(pinned.text);
  if (!grouping) return co::Err(fail(co::ErrorCode::InvalidArgument, "themes: " + grouping.error().message()));
  t.block = grouping->block; t.rerank = grouping->rerank;
  std::map<std::string, usize> index;
  for (usize k = 0; k < lib.size(); ++k) index.emplace(lib[k].id, k);
  for (const auto& item : j.at("weights").items()) {
    const f64 w = item.value().is_number() ? item.value().get<f64>() : quiet_nan;
    if (!std::isfinite(w) || w < 0)
      return co::Err(fail(co::ErrorCode::InvalidArgument, "themes: weight must be finite and >= 0: " + item.key()));
    if (!(w > 0)) continue;
    const auto at = index.find(item.key());
    if (at == index.end())
      return co::Err(fail(co::ErrorCode::InvalidArgument, "themes: pool member " + item.key() +
          " is not in --library (its cached signal builds its theme composite)"));
    ATX_TRY(const int s, pinned_sign(signs, item.key()));
    t.signed_weight[at->second] = static_cast<f64>(s) * w; t.sign[at->second] = s;
  }
  for (usize k = 0; k < lib.size(); ++k) {
    if (!t.sign[k]) {
      // Unweighted library rows keep a listed sign for orientation reporting only.
      const auto at = signs.find(lib[k].id);
      if (at != signs.end() && at->is_number_integer() && (at->get<i64>() == 1 || at->get<i64>() == -1))
        t.sign[k] = static_cast<int>(at->get<i64>());
      continue;
    }
    if (t.signed_weight[k] == 0.0) continue;
    auto name = lib[k].theme;
    if (!t.block.empty()) {
      // The runner refuses a weighted member without a theme in the block, so no pool has one.
      const auto listed = grouping->themes.find(lib[k].id);
      if (listed == grouping->themes.end())
        return co::Err(fail(co::ErrorCode::InvalidArgument, "themes: weighted member " + lib[k].id +
            " has no theme in the weights file's " + t.block + " block"));
      name = listed->second;
    }
    const auto at = std::find(t.names.begin(), t.names.end(), name);
    t.theme_of[k] = static_cast<usize>(at - t.names.begin());
    if (at == t.names.end()) t.names.push_back(name);
  }
  if (t.names.empty() || t.names.size() + 1U > cb::kMaxMarginalRegressors)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "themes: 1..10 weighted themes (the composite plus the "
        "themes may not exceed 11 regressors)"));
  return co::Ok(std::move(t));
}

// Optional fields manifest: name -> payload SHA256, bound to the pool's role.
co::Result<std::map<std::string, std::string>> read_fields(const MarginalIcConfig& cfg, const Pool& pool, std::string& sha) {
  std::map<std::string, std::string> out;
  if (cfg.fields_directory.empty()) return co::Ok(std::move(out));
  ATX_TRY(auto pinned, pinned_json(fs::path(cfg.fields_directory) / "manifest.json", std::string{}, "fields manifest",
                                   ic_fields_manifest_max_bytes));
  const Json& j = pinned.json; sha = pinned.sha;
  if (text_of(j, "schema") != fields_schema || !j.contains("role") || text_of(j.at("role"), "manifest_sha256") != pool.role_sha ||
      !j.contains("fields") || !j.at("fields").is_array())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "fields manifest schema or role binding differs from the pool's role"));
  for (const auto& row : j.at("fields")) {
    const auto name = text_of(row, "name"), payload = text_of(row, "sha256");
    if (name.empty() || !hash_valid(payload)) return co::Err(fail(co::ErrorCode::InvalidArgument, "fields manifest row " + name));
    out[name] = payload;
  }
  return co::Ok(std::move(out));
}

// ---- candidate cache (v2 entries, read in place) --------------------------------
struct CacheEntry { fs::path sidecar, payload; std::string payload_sha, vm_identity; };
using FieldShas = std::map<std::string, std::string>;
// nullopt: an intact entry for other field payloads than the pinned fields manifest names.
co::Result<std::optional<CacheEntry>> accept_sidecar(const fs::path& path, const Candidate& c, const std::string& stem,
                                                     const Pool& pool, const FieldShas& fields) {
  ATX_TRY(auto text, read_text(path, "candidate cache sidecar"));
  const auto j = Json::parse(text, nullptr, false);
  const u64 bytes = static_cast<u64>(pool.dates) * pool.names * sizeof(f64);
  auto sha = text_of(j, "payload_sha256");
  if (j.is_discarded() || text_of(j, "schema") != signal_schema || text_of(j, "candidate_id") != c.id ||
      text_of(j, "dsl_sha256") != c.dsl_sha || text_of(j, "role_manifest_sha256") != pool.role_sha ||
      text_of(j, "layout") != signal_layout || text_of(j, "payload") != stem + ".f64" ||
      count_of(j, "dates") != std::optional<u64>(pool.dates) || count_of(j, "instruments") != std::optional<u64>(pool.names) ||
      count_of(j, "bytes") != std::optional<u64>(bytes) || !hash_valid(sha) || !j.contains("field_payload_sha256") ||
      !j.at("field_payload_sha256").is_object())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "candidate cache sidecar does not describe " + c.id +
        " on the pool's role: " + path.string()));
  if (!fields.empty()) {
    for (const auto& item : j.at("field_payload_sha256").items()) {
      const auto at = fields.find(item.key());
      if (at == fields.end() || !item.value().is_string() || item.value().get<std::string>() != at->second)
        return co::Ok(std::optional<CacheEntry>{});
    }
  }
  return co::Ok(std::optional<CacheEntry>(CacheEntry{path, path.parent_path() / (stem + ".f64"), std::move(sha),
                                                     text_of(j, "vm_identity")}));
}
co::Result<std::vector<fs::path>> entry_directories(const fs::path& base) {
  std::vector<fs::path> out{base}; std::error_code ec;
  fs::directory_iterator it(base, ec);
  for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (out.size() > max_listing) return co::Err(fail(co::ErrorCode::OutOfRange, "cache listing bound: " + base.string()));
    std::error_code kind;
    if (it->is_directory(kind) && it->path().filename().string().starts_with("fp_")) out.push_back(it->path());
  }
  if (ec) return co::Err(fail(co::ErrorCode::IoError, "cache listing " + base.string() + ": " + ec.message()));
  return co::Ok(std::move(out));
}
// The first root holding an intact entry wins; two in one root (other field payloads) need --fields.
co::Result<CacheEntry> resolve_entry(const std::vector<fs::path>& roots, const Candidate& c, const Pool& pool,
                                     const FieldShas& fields) {
  const auto stem = c.id + "." + c.dsl_sha.substr(0, 16);
  for (const auto& root : roots) {
    const auto base = root / pool.role_sha;
    std::error_code ec;
    if (!fs::is_directory(base, ec)) continue;
    ATX_TRY(const auto directories, entry_directories(base));
    std::vector<CacheEntry> found;
    for (const auto& directory : directories) {
      const auto probe = directory / (stem + ".json");
      std::error_code present;
      if (!fs::exists(probe, present)) continue;
      ATX_TRY(auto entry, accept_sidecar(probe, c, stem, pool, fields));
      if (entry) found.push_back(std::move(*entry));
    }
    if (found.size() > 1U)
      return co::Err(fail(co::ErrorCode::InvalidArgument, std::to_string(found.size()) + " cache entries for " + c.id +
          " under " + base.string() + " (other field payloads); pass --fields to pick one"));
    if (found.size() == 1U) return co::Ok(std::move(found.front()));
  }
  return co::Err(fail(co::ErrorCode::NotFound, "no candidate cache entry for " + c.id + " (" + stem + ") on role " +
      pool.role_sha + "; run the u pass with --candidate-cache first"));
}

// ---- role: the runner's h 21 research label --------------------------------------
// labels: rows [begin, begin + rows) of the pool's window, date-major, NaN where undefined.
struct Labels { usize begin{}, rows{}; std::vector<f64> values; i64 first_session_ns{}, last_session_ns{}; };
template<class T> co::Result<std::vector<T>> read_pinned(const FileReceipt& file) {
  std::ifstream in(file.path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 0 || static_cast<u64>(in.tellg()) != file.bytes)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pinned payload extent: " + file.path.string()));
  std::vector<T> out(static_cast<usize>(file.bytes / sizeof(T)));
  in.seekg(0);
  // SAFETY: char writes the object representation of trivially copyable T storage of that extent.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(file.bytes));
  if (!in) return co::Err(fail(co::ErrorCode::IoError, "pinned payload read: " + file.path.string()));
  ATX_TRY(auto sha, co::sha256_hex(std::as_bytes(std::span<const T>(out))));
  if (sha != file.sha) return co::Err(fail(co::ErrorCode::InvalidArgument, "pinned payload SHA256 mismatch: " + file.path.string()));
  return co::Ok(std::move(out));
}
co::Status bind_role(const engine::data::StrategyRoleData& role, const Pool& pool) {
  if (role.manifest_sha256 != pool.role_sha)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "--role manifest SHA256 differs from the pool's role_manifest_sha256"));
  if (role.panel.dates() != pool.dates || role.panel.instruments() != pool.names ||
      role.score_begin != pool.score_begin || role.score_end != pool.score_end)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "role geometry/window differs from the pool's"));
  ATX_TRY(const auto sessions, read_pinned<i64>(pool.sessions));
  ATX_TRY(const auto ids, read_pinned<u64>(pool.ids));
  if (sessions != role.session_keys || ids != role.instrument_ids)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "role sessions/instrument ids differ from the pool's axes"));
  return co::Ok();
}
// Cumulative excluded one-day returns (the runner's guard_for): an adjacent observed step with
// |log close step| > 1.5, or above |log raw_close step| + .10.
std::vector<u32> return_guard(const engine::alpha::Panel& p, std::span<const f64> close, std::span<const f64> raw) {
  const usize d = p.dates(), n = p.instruments();
  std::vector<u32> out(d * n, 0U);
  for (usize t = 1; t < d; ++t) for (usize i = 0; i < n; ++i) {
    const usize a = (t - 1) * n + i, b = t * n + i;
    bool bad = false;
    if (p.in_universe(t - 1, i) && p.in_universe(t, i) && std::isfinite(close[a]) && std::isfinite(close[b]) &&
        close[a] > 0 && close[b] > 0) {
      const f64 r = std::log(close[b]) - std::log(close[a]);
      bad = std::abs(r) > 1.5;
      if (std::isfinite(raw[a]) && std::isfinite(raw[b]) && raw[a] > 0 && raw[b] > 0)
        bad = bad || std::abs(r) > std::abs(std::log(raw[b]) - std::log(raw[a])) + .10;
    }
    out[b] = out[a] + (bad ? 1U : 0U);
  }
  return out;
}
// The engine research label (ic_screen.cpp prepare_cache with require_endpoint_presence, delay
// 1, maturity = role end): decision-eligible at d, both endpoints present, no guarded step
// between them, finite positive decision/entry/exit closes, finite return.
co::Result<Labels> research_labels(const engine::data::StrategyRoleData& role) {
  const auto& p = role.panel; const usize n = p.instruments();
  ATX_TRY(const auto close_id, p.field_id("close"));
  ATX_TRY(const auto raw_id, p.field_id("raw_close"));
  const auto close = p.field_all(close_id), raw = p.field_all(raw_id);
  const usize lag = execution_delay + horizon;
  Labels out; out.begin = role.score_begin;
  out.rows = role.score_end > role.score_begin + lag ? role.score_end - role.score_begin - lag : 0U;
  if (out.rows < 2U) return co::Err(fail(co::ErrorCode::InvalidArgument, "role window too short for mature h 21 labels"));
  const auto guard = return_guard(p, close, raw);
  out.values.assign(out.rows * n, quiet_nan);
  for (usize row = 0; row < out.rows; ++row) {
    const usize t = out.begin + row, entry = t + execution_delay, last = entry + horizon;
    for (usize i = 0; i < n; ++i) {
      if (!p.in_universe(t, i) || role.decision_member[t * n + i] == 0) continue;
      if (!p.in_universe(entry, i) || !p.in_universe(last, i) || guard[last * n + i] != guard[entry * n + i]) continue;
      const f64 decision = close[t * n + i], from = close[entry * n + i], to = close[last * n + i];
      if (!std::isfinite(decision) || decision <= 0 || !std::isfinite(from) || !std::isfinite(to) || from <= 0 || to <= 0)
        continue;
      const f64 ret = to / from - 1.0;
      if (std::isfinite(ret)) out.values[row * n + i] = ret;
    }
  }
  out.first_session_ns = role.session_keys[out.begin];
  out.last_session_ns = role.session_keys[out.begin + out.rows - 1U];
  return co::Ok(std::move(out));
}
// Ruling E-10 (review B-3): the pool's role is the one its candidate signals were scored on,
// so a role built with --delisting-returns is refused before any payload is opened.
co::Status refuse_terminal_return_role(const MarginalIcConfig& cfg) {
  ATX_TRY(const auto manifest, read_text(cfg.role_manifest, "role manifest"));
  return engine::data::refuse_delisting_returns_signal_role(manifest, cfg.role_manifest);
}
// The role lives only inside this call: it is released before any payload is streamed.
co::Result<Labels> load_labels(const MarginalIcConfig& cfg, const Pool& pool) {
  ATX_TRY(auto role, engine::data::read_strategy_role(cfg.role_manifest, cfg.max_working_bytes));
  ATX_TRY_VOID(bind_role(role, pool));
  return research_labels(role);
}

// ---- streaming ----------------------------------------------------------------
// One date-major f64 payload, verified whole once (extent + SHA256), then read a row at a time.
class RowReader {
public:
  static co::Result<RowReader> open(const fs::path& path, const std::string& sha, usize dates, usize names) {
    std::error_code ec; const auto size = fs::file_size(path, ec);
    if (ec || static_cast<u64>(size) != static_cast<u64>(dates) * names * sizeof(f64))
      return co::Err(fail(co::ErrorCode::InvalidArgument, "payload extent: " + path.string()));
    ATX_TRY(const auto actual, co::sha256_file(path.string()));
    if (actual != sha) return co::Err(fail(co::ErrorCode::InvalidArgument, "payload SHA256 mismatch: " + path.string()));
    RowReader out; out.names_ = names; out.dates_ = dates; out.path_ = path;
    out.in_.open(path, std::ios::binary);
    if (!out.in_) return co::Err(fail(co::ErrorCode::IoError, "payload open: " + path.string()));
    return co::Ok(std::move(out));
  }
  co::Status read(usize date, std::span<f64> row) {
    if (date >= dates_ || row.size() != names_) return co::Err(fail(co::ErrorCode::Internal, "row read geometry"));
    in_.seekg(static_cast<std::streamoff>(date * names_ * sizeof(f64)), std::ios::beg);
    // SAFETY: char writes the object representation of the caller's f64 row of exactly that extent.
    in_.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(names_ * sizeof(f64)));
    if (!in_) return co::Err(fail(co::ErrorCode::IoError, "payload row read: " + path_.string()));
    return co::Ok();
  }
private:
  std::ifstream in_; fs::path path_; usize dates_{}, names_{};
};
struct Series { std::vector<f64> raw, marginal; usize spanned{}; };
struct Streams {
  RowReader pool; std::vector<RowReader> candidates; std::vector<u8> member;
};
// ew-theme-std-v1 with rerank true (review B-2): theme j's row becomes the blend's theme term,
// the centred tied re-rank of sum_{k in j} s_k w_k r_k over the member names where a member of
// j is ranked, with the IC composition's kernel and accumulation order (library order,
// combine/group_rerank.hpp). Member names without a ranked member of j keep 0 (neutral, as in
// the blend) and nonmembers NaN, as the caller initialised `theme_rows`. Unit scale: the
// blend's factor W_theme would not change the residual. `plane` holds one row (n values).
co::Status rerank_theme_rows(const Themes& themes, std::span<const u8> member,
                             const std::vector<std::vector<f64>>& ranks, std::vector<f64>& plane,
                             std::vector<std::vector<f64>>& theme_rows,
                             std::vector<std::pair<f64, usize>>& scratch) {
  const usize n = member.size();
  for (usize j = 0; j < theme_rows.size(); ++j) {
    std::fill(plane.begin(), plane.end(), quiet_nan);
    for (usize k = 0; k < ranks.size(); ++k) {
      if (themes.theme_of[k] != j) continue;
      for (usize i = 0; i < n; ++i)
        if (member[i] != 0 && std::isfinite(ranks[k][i]))
          cb::accumulate_group_cell(plane[i], themes.signed_weight[k] * ranks[k][i]);
    }
    ATX_TRY_VOID(cb::add_group_rerank(plane, n, 0U, 1U, 1.0, theme_rows[j], scratch));
  }
  return co::Ok();
}
// Per decision row: every candidate's centred rank over the pool's members, the theme
// composites, then one kernel call per candidate and one pairwise-correlation update.
co::Status stream_rows(const MarginalIcConfig& cfg, const Pool& pool, const std::vector<Candidate>& lib,
                       const Themes& themes, const Labels& labels, Streams& in, std::vector<Series>& series,
                       cb::PairwiseRowCorrelation& rho) {
  const usize n = pool.names, k_n = lib.size(), j_n = themes.names.size();
  std::vector<std::vector<f64>> raw(k_n, std::vector<f64>(n)), ranks(k_n, std::vector<f64>(n));
  std::vector<std::vector<f64>> theme_rows(j_n, std::vector<f64>(n));
  // `plane`: one row of rerank_theme_rows (inside the admission's fixed 32 MiB slack).
  std::vector<f64> composite(n), plane(themes.rerank ? n : 0U);
  // Row views are taken once: none of these buffers is resized while streaming.
  std::vector<std::span<const f64>> regressors, rank_rows;
  regressors.emplace_back(composite);
  for (const auto& row : theme_rows) regressors.emplace_back(row);
  for (const auto& row : ranks) rank_rows.emplace_back(row);
  cb::MarginalRankIcScratch scratch; std::vector<std::pair<f64, usize>> sorted;
  for (auto& s : series) { s.raw.assign(labels.rows, quiet_nan); s.marginal.assign(labels.rows, quiet_nan); }
  for (usize row = 0; row < labels.rows; ++row) {
    const usize t = labels.begin + row;
    const std::span<const u8> member(in.member.data() + t * n, n);
    ATX_TRY_VOID(in.pool.read(t, composite));
    for (usize k = 0; k < k_n; ++k) {
      ATX_TRY_VOID(in.candidates[k].read(t, raw[k]));
      ATX_TRY_VOID(cb::centred_tied_ranks(raw[k], member, ranks[k], sorted));
    }
    for (auto& row_values : theme_rows)
      for (usize i = 0; i < n; ++i) row_values[i] = member[i] != 0 ? 0.0 : quiet_nan;
    if (themes.rerank) {
      // `sorted` is free again: every candidate row above is already ranked.
      ATX_TRY_VOID(rerank_theme_rows(themes, member, ranks, plane, theme_rows, sorted));
    } else {
      for (usize k = 0; k < k_n; ++k) {
        if (themes.theme_of[k] == npos) continue;
        auto& target = theme_rows[themes.theme_of[k]];
        for (usize i = 0; i < n; ++i)
          if (member[i] != 0 && std::isfinite(ranks[k][i])) target[i] += themes.signed_weight[k] * ranks[k][i];
      }
    }
    const std::span<const f64> label(labels.values.data() + row * n, n);
    for (usize k = 0; k < k_n; ++k) {
      ATX_TRY(const auto day, cb::marginal_rank_ic_day(ranks[k], regressors, label, cfg.min_names, scratch));
      series[k].raw[row] = day.raw_ic; series[k].marginal[row] = day.marginal_ic;
      series[k].spanned += day.spanned;
    }
    ATX_TRY_VOID(rho.add_date(rank_rows));
  }
  return co::Ok();
}
co::Result<Streams> open_streams(const Pool& pool, const std::vector<CacheEntry>& entries) {
  ATX_TRY(auto composite, RowReader::open(pool.signal.path, pool.signal.sha, pool.dates, pool.names));
  ATX_TRY(auto member, read_pinned<u8>(pool.member));
  if (std::any_of(member.begin(), member.end(), [](u8 v) { return v > 1; }))
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool member mask is not binary"));
  Streams out{std::move(composite), {}, std::move(member)};
  out.candidates.reserve(entries.size());
  for (const auto& entry : entries) {
    ATX_TRY(auto reader, RowReader::open(entry.payload, entry.payload_sha, pool.dates, pool.names));
    out.candidates.push_back(std::move(reader));
  }
  return co::Ok(std::move(out));
}

Json candidate_row(const std::vector<Candidate>& lib, const Themes& themes, const std::vector<CacheEntry>& entries,
                   const std::vector<Series>& series, const cb::PairwiseRowCorrelation& rho, usize k) {
  std::vector<f64> compact;
  const auto raw = cb::summarize_rank_ic(series[k].raw, hac_lag, compact);
  const auto marginal = cb::summarize_rank_ic(series[k].marginal, hac_lag, compact);
  f64 best = quiet_nan; std::string best_id;
  for (usize m = 0; m < lib.size(); ++m) {
    const f64 r = rho.mean(k, m);
    if (std::isfinite(r) && (!std::isfinite(best) || std::abs(r) > std::abs(best))) { best = r; best_id = lib[m].id; }
  }
  const auto& c = lib[k];
  const std::optional<int> sign = themes.sign[k] ? themes.sign[k] : c.prior_sign;
  const char* source = themes.sign[k] ? "pinned-weights" : (c.prior_sign ? "library-prior-sign" : nullptr);
  return Json{{"id", c.id}, {"ic21", finite_or_null(raw.mean)}, {"ic21_hac_t", finite_or_null(raw.hac_t)},
      {"marginal_ic21", finite_or_null(marginal.mean)}, {"marginal_hac_t", finite_or_null(marginal.hac_t)},
      {"max_abs_rho", finite_or_null(std::abs(best))},
      {"max_rho_member", best_id.empty() ? Json(nullptr) : Json(best_id)},
      {"max_rho_signed", finite_or_null(best)}, {"dates", raw.dates}, {"marginal_dates", marginal.dates},
      {"spanned_dates", series[k].spanned}, {"family", c.family}, {"theme", c.theme},
      {"book_member", themes.theme_of[k] != npos},
      {"sign", sign ? Json(*sign) : Json(nullptr)}, {"sign_source", source ? Json(source) : Json(nullptr)},
      {"payload_sha256", entries[k].payload_sha}};
}
Json method_json(const MarginalIcConfig& cfg, const Themes& themes) {
  Json regressors = Json::array({"book_composite"});
  for (const auto& name : themes.names) regressors.push_back("theme:" + name);
  return Json{{"label", "close[d+1+21]/close[d+1]-1;strict-positive-observed-endpoints;role-maturity;"
                        "guard observed-adjacent-log1.5;adjusted-log-vs-raw+.10"},
      {"horizon", horizon}, {"execution_delay", execution_delay},
      {"candidate", "centred-tied-rank over the pool's member names with a finite signal"},
      {"regressors", std::move(regressors)},
      {"theme_composite", themes.rerank
          ? "ew-theme-std-v1 theme term: centred tied re-rank of the sum over the theme's weighted members of "
            "sign*weight*centred-rank, over member names with a ranked theme member; 0 on other member names; "
            "NaN off members (unit scale)"
          : "sum over weighted members of sign*weight*centred-rank; unranked member adds 0; NaN off members"},
      {"residual", "per date OLS on [1, regressors] over names with finite candidate and regressors "
                   "(combine::residualize_signal, complete orthogonal decomposition)"},
      {"ic", "paired names: finite residual and label; marginal = Pearson(residual, label rank), "
             "ic21 = Spearman(candidate, label) on the same names"},
      {"spanned", "residual norm <= 1e-10 x candidate dispersion: marginal IC 0 for that date"},
      {"hac", {{"kernel", "bartlett"}, {"lag", hac_lag}, {"small_sample_correction", true},
               {"series", "defined dates compacted in order"}}},
      {"rho", "mean over dates of the Pearson correlation of centred ranks on jointly finite names (>= min_names); "
              "max |mean| over the other library candidates"},
      {"orientation", "DSL (raw, unoriented): multiply by `sign` for the book orientation"},
      {"min_names", cfg.min_names}};
}
} // namespace

co::Result<u64> marginal_ic_working_bytes(usize dates, usize names, usize score_rows, usize candidates, usize regressors) {
  if (dates == 0U || dates > 4096U || names == 0U || names > 20000U || score_rows > dates || candidates == 0U ||
      candidates > 256U || regressors == 0U || regressors > cb::kMaxMarginalRegressors)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "working-bytes geometry"));
  // Every factor is bounded above, so no product below can overflow u64.
  const u64 cells = static_cast<u64>(dates) * names, d = dates, n = names, rows = score_rows, k = candidates;
  u64 total = 32ULL << 20;                                  // metadata, JSON, sidecars, stream buffers
  total += (16ULL << 20) + cells * 26U + d * 24U + n * 8U;  // the role (read_strategy_role admission)
  total += cells * sizeof(u32);                             // return guard while labels are built
  total += rows * n * sizeof(f64);                          // h 21 labels
  total += cells;                                           // pool member mask
  total += n * (2U * k + regressors + 4U) * sizeof(f64);    // one row per stream + kernel scratch
  total += k * rows * 2U * sizeof(f64);                     // daily raw and marginal series
  total += k * k * 16U;                                     // pairwise correlation sums
  return co::Ok(total);
}

co::Status run_marginal_ic(const MarginalIcConfig& cfg, std::ostream& progress) {
  try {
    using steady = std::chrono::steady_clock;
    const auto started = steady::now();
    const auto since = [](steady::time_point from) { return std::chrono::duration<f64>(steady::now() - from).count(); };
    if (cfg.candidate_cache_directory.empty() || cfg.library_path.empty() || cfg.pool_path.empty() ||
        cfg.role_manifest.empty() || cfg.output_directory.empty() || cfg.min_names < 3U ||
        cfg.max_working_bytes < (32ULL << 20) || cfg.max_working_bytes > (16ULL << 30) ||
        (!cfg.library_sha256.empty() && !hash_valid(cfg.library_sha256)) ||
        (!cfg.pool_sha256.empty() && !hash_valid(cfg.pool_sha256)))
      return co::Err(fail(co::ErrorCode::InvalidArgument, "bounded config (needs --candidate-cache, --library, --pool, "
          "--role, --output; --min-names >= 3; --max-memory-mib 32..16384)"));
    std::error_code ec;
    if (fs::exists(cfg.output_directory, ec) || ec)
      return co::Err(fail(co::ErrorCode::AlreadyExists, "output directory must be new: " + cfg.output_directory));
    ATX_TRY(const auto pool, read_pool(cfg));
    ATX_TRY_VOID(refuse_terminal_return_role(cfg));
    std::string library_sha;
    ATX_TRY(const auto lib, read_library(cfg, library_sha));
    ATX_TRY(const auto themes, read_themes(cfg, pool, lib));
    std::string fields_sha;
    ATX_TRY(const auto fields, read_fields(cfg, pool, fields_sha));
    const auto identity = ic_cache_vm_identity().identity;
    const std::vector<fs::path> roots{fs::path(cfg.candidate_cache_directory) / identity, fs::path(cfg.candidate_cache_directory)};
    std::vector<CacheEntry> entries; entries.reserve(lib.size());
    for (const auto& c : lib) {
      ATX_TRY(auto entry, resolve_entry(roots, c, pool, fields));
      entries.push_back(std::move(entry));
    }
    const usize lag = execution_delay + horizon;
    const usize rows = pool.score_end > pool.score_begin + lag ? pool.score_end - pool.score_begin - lag : 0U;
    const usize regressors = 1U + themes.names.size();
    ATX_TRY(const auto required, marginal_ic_working_bytes(pool.dates, pool.names, rows, lib.size(), regressors));
    if (required > cfg.max_working_bytes)
      return co::Err(fail(co::ErrorCode::Unavailable, "required_bytes=" + std::to_string(required) +
          " exceeds --max-memory-mib before any payload load"));
    progress << "marginal: candidates=" << lib.size() << " regressors=" << regressors << " rows=" << rows
             << " names=" << pool.names << " required_bytes=" << required << '\n' << std::flush;
    const auto label_started = steady::now();
    ATX_TRY(const auto labels, load_labels(cfg, pool));
    progress << "marginal: labels rows=" << labels.rows << " seconds=" << since(label_started) << '\n' << std::flush;
    const auto verify_started = steady::now();
    ATX_TRY(auto streams, open_streams(pool, entries));
    progress << "marginal: payloads verified=" << entries.size() + 1U << " seconds=" << since(verify_started) << '\n'
             << std::flush;
    const auto stream_started = steady::now();
    std::vector<Series> series(lib.size());
    cb::PairwiseRowCorrelation rho(lib.size(), cfg.min_names);
    ATX_TRY_VOID(stream_rows(cfg, pool, lib, themes, labels, streams, series, rho));
    const auto stream_seconds = since(stream_started);
    progress << "marginal: streamed rows=" << labels.rows << " seconds=" << stream_seconds << '\n' << std::flush;
    Json rows_json = Json::array();
    for (usize k = 0; k < lib.size(); ++k) rows_json.push_back(candidate_row(lib, themes, entries, series, rho, k));
    Json theme_members = Json::object();
    for (usize k = 0; k < lib.size(); ++k)
      if (themes.theme_of[k] != npos) theme_members[themes.names[themes.theme_of[k]]].push_back(lib[k].id);
    Json themes_input = themes.path.empty() ? Json(nullptr)
        : Json{{"path", themes.path}, {"sha256", themes.sha}, {"members", std::move(theme_members)}};
    // Keys added only for an ew-theme-std-v1 file, so outputs without one keep their bytes.
    if (themes.block == "theme_standardise") {
      themes_input["grouping"] = themes.block;
      themes_input["rerank"] = themes.rerank;
    }
    Json inputs{{"pool", {{"path", cfg.pool_path}, {"sha256", pool.sha}, {"role_manifest_sha256", pool.role_sha},
                          {"library_sha256", pool.library_sha},
                          {"composition_weights_sha256", pool.weights_sha.empty() ? Json(nullptr) : Json(pool.weights_sha)}}},
        {"library", {{"path", cfg.library_path}, {"sha256", library_sha}}},
        {"role", {{"path", cfg.role_manifest}, {"manifest_sha256", pool.role_sha}}},
        {"candidate_cache", {{"directory", cfg.candidate_cache_directory}, {"build_vm_identity", identity},
                             {"layout", std::string(signal_schema)}}},
        {"themes", std::move(themes_input)},
        {"fields", cfg.fields_directory.empty() ? Json(nullptr)
                                                : Json{{"directory", cfg.fields_directory}, {"manifest_sha256", fields_sha}}}};
    Json out{{"schema", std::string(output_schema)}, {"status", "complete"}, {"contract", "K6"},
        {"method", method_json(cfg, themes)}, {"inputs", std::move(inputs)},
        {"window", {{"score_begin", labels.begin}, {"rows", labels.rows}, {"first_decision_session_ns", labels.first_session_ns},
                    {"last_decision_session_ns", labels.last_session_ns}}},
        {"working_bytes", required}, {"stage_seconds", {{"stream", stream_seconds}, {"total", since(started)}}},
        {"candidates", std::move(rows_json)}};
    if (!fs::create_directory(cfg.output_directory, ec))
      return co::Err(fail(co::ErrorCode::AlreadyExists, "output directory must be new; " + ec.message()));
    const auto path = fs::path(cfg.output_directory) / "marginal_ic.json";
    std::ofstream file(path, std::ios::binary);
    file << out.dump(2) << '\n'; file.close();
    if (!file) return co::Err(fail(co::ErrorCode::IoError, "output write: " + path.string()));
    progress << "marginal: wrote " << path.string() << '\n' << std::flush;
    return co::Ok();
  } catch (const std::bad_alloc&) {
    return co::Err(fail(co::ErrorCode::Unavailable, "allocation within the admitted envelope failed"));
  } catch (const std::exception& e) {
    return co::Err(fail(co::ErrorCode::InvalidArgument, e.what()));
  }
}

int dispatch_marginal_ic(int argc, char** argv, std::ostream& out, std::ostream& err) {
  MarginalIcConfig cfg;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "atx-equity-strategy-ic marginal --candidate-cache DIR --library JSON --pool ROLE_combined.json "
               "--role MANIFEST --output NEWDIR\n"
               "    [--library-sha256 SHA] [--pool-sha256 SHA] [--themes WEIGHTS_JSON] [--fields DIR]\n"
               "    [--min-names N (50)] [--max-memory-mib N (600)]\n"
               "  Writes NEWDIR/marginal_ic.json (atx.marginal-ic/v1, contract K6): per library candidate ic21,\n"
               "  ic21_hac_t, marginal_ic21, marginal_hac_t (Bartlett lag 21), max_abs_rho, max_rho_member.\n"
               "  --pool: a --save-combined manifest; --role must be its role (bound by SHA256).\n"
               "  --themes: the pool's composition weights (bound by the pool's composition_weights_sha256);\n"
               "    adds one theme composite per weighted theme (<= 10), grouped by the file's theme block\n"
               "    (re-ranked under ew-theme-std-v1 rerank). --fields: the fields manifest whose\n"
               "    payload SHA256s pick among cache entries of one candidate.\n";
        return 0;
      }
      if (++i >= argc) throw std::invalid_argument("missing option value: " + key);
      const std::string value = argv[i];
      const auto integer = [&]() -> u64 {
        if (value.empty() || value.front() == '-') throw std::invalid_argument("unsigned integer required: " + key);
        usize used{}; const auto v = std::stoull(value, &used);
        if (used != value.size()) throw std::invalid_argument("invalid integer: " + key);
        return v;
      };
      if (key == "--candidate-cache") cfg.candidate_cache_directory = value;
      else if (key == "--library") cfg.library_path = value;
      else if (key == "--library-sha256") cfg.library_sha256 = value;
      else if (key == "--pool") cfg.pool_path = value;
      else if (key == "--pool-sha256") cfg.pool_sha256 = value;
      else if (key == "--role") cfg.role_manifest = value;
      else if (key == "--themes") cfg.themes_path = value;
      else if (key == "--fields") cfg.fields_directory = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--min-names") cfg.min_names = static_cast<usize>(integer());
      else if (key == "--max-memory-mib") {
        const auto mib = integer();
        if (mib > 16384U) throw std::invalid_argument("memory limit");
        cfg.max_working_bytes = mib << 20;
      } else throw std::invalid_argument("unknown option: " + key);
    }
    const auto status = run_marginal_ic(cfg, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) {
    err << e.what() << '\n';
    return 2;
  }
}
} // namespace atx::impl::strategy
