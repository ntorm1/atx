#include "strategy_marginal_ic.hpp"
#include "strategy_ic_detail.hpp" // shared hash_valid / metadata_text (review CM s.2)
#include "strategy_ic_runner.hpp" // ic_cache_vm_identity: the cache root this build writes
#include "strategy_marginal_pair_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
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
#include "atx/engine/data/role_panel.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace cb = atx::engine::combine;
namespace fs = std::filesystem;
namespace icd = ic_detail;
using Json = nlohmann::json;
constexpr f64 quiet_nan = std::numeric_limits<f64>::quiet_NaN();
constexpr usize npos = std::numeric_limits<usize>::max();
// The runner's label recipe: close[d+1+h]/close[d+1]-1, h = 21 (its orientation horizon).
constexpr usize horizon = 21, execution_delay = 1, hac_lag = 21;
constexpr u64 metadata_limit = 1ULL << 20;
constexpr usize max_listing = usize{1} << 16;
constexpr usize max_workers = 16;          // --workers: the research IC worker bound
constexpr usize band_rows_per_worker = 16; // rows of one band chunk per worker (--workers > 1)
constexpr usize max_digests = 4096;        // --verified-digests lines
// File-format contracts of strategy_ic_runner.cpp (--candidate-cache v2, --save-combined).
constexpr std::string_view signal_schema = "atx.dsl-candidate-signal/v2";
constexpr std::string_view signal_layout = "date-major-little-endian-f64;non-finite-stored-as-quiet-NaN";
constexpr std::string_view combined_schema = "atx.dsl-combined-signal/v1";
constexpr std::string_view library_schema = "atx.dsl-ic-library/v1";
constexpr std::string_view fields_schema = "atx.research-role-fields/v1";
constexpr std::string_view output_schema = "atx.marginal-ic/v1";

co::Error fail(co::ErrorCode code, const std::string& message) { return co::Error{code, "marginal IC: " + message}; }
// The library's candidate-id rule. Kept here: the runner's copies of it (composition_id,
// theme_name, safe_name) are TU-local, none is in strategy_ic_detail.hpp.
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
using Clock = std::chrono::steady_clock;
f64 seconds_between(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<f64>(to - from).count();
}

struct Pinned { Json json; std::string sha, text; };
// The shared metadata reader plus a recorded SHA256. `pin` empty: computed and recorded, not
// checked (ic_detail::pinned_json refuses an empty pin, so it cannot serve this contract).
// `limit`: metadata_limit, or ic_fields_manifest_max_bytes for a fields manifest (review B-4).
co::Result<Pinned> pinned_document(const fs::path& path, const std::string& pin,
                                   std::string_view what, u64 limit = metadata_limit) {
  ATX_TRY(auto text, icd::metadata_text(path.string(), limit));
  ATX_TRY(auto sha, co::sha256_hex(text));
  if (!pin.empty() && pin != sha)
    return co::Err(fail(co::ErrorCode::InvalidArgument, std::string(what) + " SHA256 differs from its pin: " + path.string()));
  auto j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object())
    return co::Err(fail(co::ErrorCode::InvalidArgument, std::string(what) + " is not a JSON object: " + path.string()));
  return co::Ok(Pinned{std::move(j), std::move(sha), std::move(text)});
}
// The lines of a small UTF-8 text file: a leading BOM and each line's trailing CR dropped.
std::vector<std::string> text_lines(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    if (line.ends_with('\r')) line.remove_suffix(1);
    out.emplace_back(line);
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1U);
  }
  return out;
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
  if (!icd::hash_valid(sha) || !recorded || *recorded != bytes)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool file receipt " + name + " (SHA256 or extent)"));
  return co::Ok(FileReceipt{dir / name, std::move(sha), bytes});
}
co::Result<Pool> read_pool(const MarginalIcConfig& cfg) {
  Pool pool; pool.path = cfg.pool_path;
  ATX_TRY(auto pinned, pinned_document(pool.path, cfg.pool_sha256, "pool combined manifest"));
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
  if (!icd::hash_valid(pool.role_sha) || !icd::hash_valid(pool.library_sha))
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool role/library SHA256"));
  if (j.contains("composition_weights_sha256")) {
    pool.weights_sha = text_of(j, "composition_weights_sha256");
    if (!icd::hash_valid(pool.weights_sha)) return co::Err(fail(co::ErrorCode::InvalidArgument, "pool composition_weights_sha256"));
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
  ATX_TRY(auto pinned, pinned_document(cfg.library_path, cfg.library_sha256, "library"));
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

// --candidates FILE (P9 S1, ruling P4): one library candidate id per line, blank lines ignored.
// `listed[k]` = 1 for a listed candidate; without the option every candidate is listed.
struct Listing {
  std::vector<u8> listed; usize count{}; std::string path, sha; Json ids = Json::array();
};
co::Result<Listing> read_candidates(const MarginalIcConfig& cfg,
                                    const std::vector<Candidate>& lib) {
  Listing out; out.listed.assign(lib.size(), cfg.candidates_path.empty() ? u8{1} : u8{0});
  if (cfg.candidates_path.empty()) { out.count = lib.size(); return co::Ok(std::move(out)); }
  ATX_TRY(const auto text, icd::metadata_text(cfg.candidates_path, metadata_limit));
  ATX_TRY(out.sha, co::sha256_hex(text));
  out.path = cfg.candidates_path;
  std::map<std::string, usize> index;
  for (usize k = 0; k < lib.size(); ++k) index.emplace(lib[k].id, k);
  for (const auto& line : text_lines(text)) {
    if (line.empty()) continue;
    const auto at = index.find(line);
    if (!safe_id(line) || at == index.end())
      return co::Err(fail(co::ErrorCode::InvalidArgument,
                          "--candidates: " + line + " is not a candidate id of --library"));
    if (out.listed[at->second] != 0)
      return co::Err(fail(co::ErrorCode::InvalidArgument,
                          "--candidates: " + line + " is listed twice"));
    out.listed[at->second] = 1;
  }
  for (usize k = 0; k < lib.size(); ++k)
    if (out.listed[k] != 0) { out.ids.push_back(lib[k].id); ++out.count; }
  if (out.count == 0U)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "--candidates lists no candidate"));
  return co::Ok(std::move(out));
}

// --verified-digests FILE (P9 S1): payload SHA256s the caller already verified, one per line.
struct Verified { std::set<std::string> digests; std::string path, sha; usize accepted{}; };
co::Result<Verified> read_verified(const MarginalIcConfig& cfg) {
  Verified out;
  if (cfg.verified_digests_path.empty()) return co::Ok(std::move(out));
  ATX_TRY(const auto text, icd::metadata_text(cfg.verified_digests_path, metadata_limit));
  ATX_TRY(out.sha, co::sha256_hex(text));
  out.path = cfg.verified_digests_path;
  for (const auto& line : text_lines(text)) {
    if (line.empty()) continue;
    if (!icd::hash_valid(line) || out.digests.size() >= max_digests)
      return co::Err(fail(co::ErrorCode::InvalidArgument,
                          "--verified-digests: at most 4096 lowercase SHA256 lines; refused: " +
                          line.substr(0, 80)));
    out.digests.insert(line);
  }
  return co::Ok(std::move(out));
}

// The pool's weighted members grouped into theme composites T_j = sum_{k in j} s_k w_k r_k over
// the members' centred ranks r_k (fixed denominator: an unranked member adds 0), which is the
// no-redistribution blend split by theme. Theme names come from the weights file's theme block
// (theme_standardise or theme_redistribution, read by the IC runner's ic_weights_themes, so the
// grouping is the blend's own; review B-2), else from the library rows. Under theme_standardise
// with rerank true each T_j is the blend's theme term instead: its centred tied re-rank over the
// member names with a present member of j (rerank_theme).
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
  ATX_TRY(auto pinned, pinned_document(cfg.themes_path, pool.weights_sha, "themes (the pool's composition weights)"));
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
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "themes: 1.." + std::to_string(cb::kMaxMarginalRegressors - 1U) +
                        " weighted themes (the composite plus the themes may not exceed " +
                        std::to_string(cb::kMaxMarginalRegressors) + " regressors)"));
  return co::Ok(std::move(t));
}

// Optional fields manifest: name -> payload SHA256, bound to the pool's role.
co::Result<std::map<std::string, std::string>> read_fields(const MarginalIcConfig& cfg, const Pool& pool, std::string& sha) {
  std::map<std::string, std::string> out;
  if (cfg.fields_directory.empty()) return co::Ok(std::move(out));
  ATX_TRY(auto pinned, pinned_document(fs::path(cfg.fields_directory) / "manifest.json", std::string{}, "fields manifest",
                                       ic_fields_manifest_max_bytes));
  const Json& j = pinned.json; sha = pinned.sha;
  if (text_of(j, "schema") != fields_schema || !j.contains("role") || text_of(j.at("role"), "manifest_sha256") != pool.role_sha ||
      !j.contains("fields") || !j.at("fields").is_array())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "fields manifest schema or role binding differs from the pool's role"));
  for (const auto& row : j.at("fields")) {
    const auto name = text_of(row, "name"), payload = text_of(row, "sha256");
    if (name.empty() || !icd::hash_valid(payload)) return co::Err(fail(co::ErrorCode::InvalidArgument, "fields manifest row " + name));
    out[name] = payload;
  }
  return co::Ok(std::move(out));
}

// ---- candidate cache (v2 entries, read in place) --------------------------------
struct CacheEntry { fs::path sidecar, payload; std::string payload_sha, vm_identity; };
using FieldShas = std::map<std::string, std::string>;
// nullopt: an intact entry for other field payloads than the pinned fields manifest names.
// P9 S1 fix round 1 (DS-1): the sidecar must record `identity`, this build's VM identity, as
// the runner (v2_payload_sha) and the weights fitter require; another build's entry refuses.
co::Result<std::optional<CacheEntry>> accept_sidecar(const fs::path& path, const Candidate& c, const std::string& stem,
                                                     const Pool& pool, const FieldShas& fields,
                                                     const std::string& identity) {
  ATX_TRY(auto text, icd::metadata_text(path.string(), metadata_limit));
  const auto j = Json::parse(text, nullptr, false);
  const u64 bytes = static_cast<u64>(pool.dates) * pool.names * sizeof(f64);
  auto sha = text_of(j, "payload_sha256");
  if (j.is_discarded() || text_of(j, "schema") != signal_schema || text_of(j, "candidate_id") != c.id ||
      text_of(j, "dsl_sha256") != c.dsl_sha || text_of(j, "role_manifest_sha256") != pool.role_sha ||
      text_of(j, "layout") != signal_layout || text_of(j, "payload") != stem + ".f64" ||
      count_of(j, "dates") != std::optional<u64>(pool.dates) || count_of(j, "instruments") != std::optional<u64>(pool.names) ||
      count_of(j, "bytes") != std::optional<u64>(bytes) || !icd::hash_valid(sha) || !j.contains("field_payload_sha256") ||
      !j.at("field_payload_sha256").is_object())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "candidate cache sidecar does not describe " + c.id +
        " on the pool's role: " + path.string()));
  if (text_of(j, "vm_identity") != identity)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "candidate cache sidecar of " + c.id + " records vm_identity '" +
                        text_of(j, "vm_identity") + "', not this build's " + identity + ": " +
                        path.string()));
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
// `roots` and `identity`: marginal_cache_roots (this build's own root; sidecars must record it).
co::Result<CacheEntry> resolve_entry(const std::vector<fs::path>& roots, const Candidate& c, const Pool& pool,
                                     const FieldShas& fields, const std::string& identity) {
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
      ATX_TRY(auto entry, accept_sidecar(probe, c, stem, pool, fields, identity));
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
// The engine research label (ic_screen.cpp prepare_cache with require_endpoint_presence, delay
// 1, maturity = role end): decision-eligible at d, both endpoints present, no guarded step
// between them, finite positive decision/entry/exit closes, finite return. The guard is the
// runner's own, engine::data::research_return_guard (review CM s.2: v8 carried a verbatim copy).
co::Result<Labels> research_labels(const engine::data::StrategyRoleData& role) {
  const auto& p = role.panel; const usize n = p.instruments();
  ATX_TRY(const auto close_id, p.field_id("close"));
  const auto close = p.field_all(close_id);
  const usize lag = execution_delay + horizon;
  Labels out; out.begin = role.score_begin;
  out.rows = role.score_end > role.score_begin + lag ? role.score_end - role.score_begin - lag : 0U;
  if (out.rows < 2U) return co::Err(fail(co::ErrorCode::InvalidArgument, "role window too short for mature h 21 labels"));
  ATX_TRY(const auto guard, engine::data::research_return_guard(role));
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
  ATX_TRY(const auto manifest, icd::metadata_text(cfg.role_manifest, metadata_limit));
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
// `verify` false: a payload whose SHA256 the caller vouches for (--verified-digests); its extent
// is still checked. read() is thread-safe: band workers share one reader per payload.
class RowReader {
public:
  static co::Result<RowReader> open(const fs::path& path, const std::string& sha, usize dates,
                                    usize names, bool verify) {
    std::error_code ec; const auto size = fs::file_size(path, ec);
    if (ec || static_cast<u64>(size) != static_cast<u64>(dates) * names * sizeof(f64))
      return co::Err(fail(co::ErrorCode::InvalidArgument, "payload extent: " + path.string()));
    if (verify) {
      ATX_TRY(const auto actual, co::sha256_file(path.string()));
      if (actual != sha)
        return co::Err(fail(co::ErrorCode::InvalidArgument,
                            "payload SHA256 mismatch: " + path.string()));
    }
    RowReader out; out.names_ = names; out.dates_ = dates; out.path_ = path;
    out.lock_ = std::make_unique<std::mutex>();
    out.in_.open(path, std::ios::binary);
    if (!out.in_) return co::Err(fail(co::ErrorCode::IoError, "payload open: " + path.string()));
    return co::Ok(std::move(out));
  }
  co::Status read(usize date, std::span<f64> row) {
    if (date >= dates_ || row.size() != names_) return co::Err(fail(co::ErrorCode::Internal, "row read geometry"));
    const std::lock_guard<std::mutex> hold(*lock_);
    in_.seekg(static_cast<std::streamoff>(date * names_ * sizeof(f64)), std::ios::beg);
    // SAFETY: char writes the object representation of the caller's f64 row of exactly that extent.
    in_.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(names_ * sizeof(f64)));
    if (!in_) return co::Err(fail(co::ErrorCode::IoError, "payload row read: " + path_.string()));
    return co::Ok();
  }
private:
  std::ifstream in_; fs::path path_; usize dates_{}, names_{};
  std::unique_ptr<std::mutex> lock_; // guards in_'s seek + read pair
};
// raw / marginal: per listed candidate, one value per row (unlisted: empty, never written).
struct Series { std::vector<f64> raw, marginal; usize spanned{}; };
// candidates: a reader per needed library candidate (unneeded: nullopt, never opened).
struct Streams {
  RowReader pool; std::vector<std::optional<RowReader>> candidates; std::vector<u8> member;
};
// The read-only inputs of the streaming pass, shared by every band worker.
struct StreamPlan {
  const MarginalIcConfig& cfg;
  const Themes& themes;
  const Labels& labels;
  usize names{};              // the role's instruments: a payload row's width
  std::span<const u8> listed; // per library candidate: residualised, gets a row
  std::span<const u8> needed; // per library candidate: read and ranked every row
};
// One band worker's buffers, sized once to the role width (a row's m names fit in any of them),
// so no row allocates and every span taken over them stays valid.
struct RowScratch {
  std::vector<f64> full;                // one payload row as stored (role width)
  std::vector<usize> names;             // the row's names: its members (compacted) or every name
  std::vector<u8> member;               // their pool member flags
  std::vector<f64> composite, label, values, plane, nan_row;
  std::vector<f64> book, self_composite, self_theme; // --exclude-self only
  std::vector<std::vector<f64>> ranks;  // per library candidate (needed ones sized)
  std::vector<std::vector<f64>> themes; // per theme
  std::vector<std::span<const f64>> regressors, self_regressors, rank_rows;
  std::vector<std::pair<f64, usize>> sorted;
  cb::MarginalRankIcScratch kernel;
  f64 read_seconds{}, kernel_seconds{}, pairwise_seconds{};
};
void size_scratch(const StreamPlan& plan, RowScratch& s) {
  const usize n = plan.names, j_n = plan.themes.names.size(), k_n = plan.needed.size();
  s.full.assign(n, 0.0); s.names.reserve(n); s.member.assign(n, u8{0});
  for (auto* row : {&s.composite, &s.label, &s.values, &s.plane}) row->assign(n, quiet_nan);
  s.nan_row.assign(n, quiet_nan);
  if (plan.cfg.exclude_self)
    for (auto* row : {&s.book, &s.self_composite, &s.self_theme}) row->assign(n, quiet_nan);
  s.ranks.assign(k_n, {});
  for (usize k = 0; k < k_n; ++k) if (plan.needed[k] != 0) s.ranks[k].assign(n, quiet_nan);
  s.themes.assign(j_n, std::vector<f64>(n, quiet_nan));
  s.regressors.assign(1U + j_n, {}); s.self_regressors.assign(1U + j_n, {});
  s.rank_rows.assign(k_n, {});
  s.sorted.reserve(n);
}
std::span<f64> head(std::vector<f64>& buffer, usize m) {
  return std::span<f64>(buffer.data(), m);
}
// Payload row `t` of `reader` at the scratch's names, into `out` (m values).
co::Status gather_row(RowReader& reader, usize t, RowScratch& s, std::span<f64> out) {
  ATX_TRY_VOID(reader.read(t, s.full));
  for (usize j = 0; j < out.size(); ++j) out[j] = s.full[s.names[j]];
  return co::Ok();
}
// out[i] += s_q w_q r_q(i) for each weighted member q that `take` accepts, in library order, on
// member names where r_q is ranked: the no-redistribution sum of the v8 theme composites.
template<class Take>
void add_weighted_ranks(const Themes& themes, const RowScratch& s, usize m, Take take,
                        std::span<f64> out) {
  for (usize q = 0; q < themes.theme_of.size(); ++q) {
    if (themes.theme_of[q] == npos || !take(q)) continue;
    const f64 weight = themes.signed_weight[q]; const auto& rank = s.ranks[q];
    for (usize i = 0; i < m; ++i)
      if (s.member[i] != 0 && std::isfinite(rank[i])) out[i] += weight * rank[i];
  }
}
// Member names 0, other names NaN: every composite's starting row.
void member_zero(const RowScratch& s, std::span<f64> out) {
  for (usize i = 0; i < out.size(); ++i) out[i] = s.member[i] != 0 ? 0.0 : quiet_nan;
}
// ew-theme-std-v1 with rerank true (review B-2): theme j's row becomes the blend's theme term,
// the centred tied re-rank of sum_{k in j, k != skip} s_k w_k r_k over the member names where
// such a k is ranked, with the IC composition's kernel and accumulation order (library order,
// combine/group_rerank.hpp), added to `out` (member_zero'd by the caller: member names without a
// ranked member keep 0, neutral as in the blend, nonmembers NaN). Unit scale: the blend's factor
// W_theme would not change the residual. A row without names adds nothing.
co::Status rerank_theme(const Themes& themes, usize j, usize skip, usize m, RowScratch& s,
                        std::span<f64> out) {
  if (m == 0U) return co::Ok();
  const auto plane = head(s.plane, m);
  std::fill(plane.begin(), plane.end(), quiet_nan);
  for (usize k = 0; k < themes.theme_of.size(); ++k) {
    if (themes.theme_of[k] != j || k == skip) continue;
    const auto& rank = s.ranks[k];
    for (usize i = 0; i < m; ++i)
      if (s.member[i] != 0 && std::isfinite(rank[i]))
        cb::accumulate_group_cell(plane[i], themes.signed_weight[k] * rank[i]);
  }
  return cb::add_group_rerank(plane, m, 0U, 1U, 1.0, out, s.sorted);
}
// The row's regressors over its m names: the pool's composite, then each theme composite.
co::Status build_regressors(const Themes& themes, usize m, RowScratch& s) {
  s.regressors[0] = head(s.composite, m);
  for (usize j = 0; j < themes.names.size(); ++j) {
    const auto row = head(s.themes[j], m);
    member_zero(s, row);
    if (themes.rerank) ATX_TRY_VOID(rerank_theme(themes, j, npos, m, s, row));
    else add_weighted_ranks(themes, s, m, [&](usize q) { return themes.theme_of[q] == j; }, row);
    s.regressors[1U + j] = row;
  }
  return co::Ok();
}
// --exclude-self: member k's regressors without its own term. The book composite is the plain
// reconstruction sum over the weighted members of s w r (s.book, built per row) minus k's term;
// k's theme composite is rebuilt without k (re-ranked under rerank); other themes are kept.
co::Status self_regressors(const Themes& themes, usize k, usize m, RowScratch& s) {
  const auto composite = head(s.self_composite, m); const auto& rank = s.ranks[k];
  const f64 weight = themes.signed_weight[k];
  for (usize i = 0; i < m; ++i) {
    const bool ranked = s.member[i] != 0 && std::isfinite(rank[i]);
    composite[i] = ranked ? s.book[i] - weight * rank[i] : s.book[i];
  }
  const usize own = themes.theme_of[k];
  const auto theme = head(s.self_theme, m);
  member_zero(s, theme);
  if (themes.rerank) ATX_TRY_VOID(rerank_theme(themes, own, k, m, s, theme));
  else add_weighted_ranks(themes, s, m,
                          [&](usize q) { return themes.theme_of[q] == own && q != k; }, theme);
  s.self_regressors[0] = composite;
  for (usize j = 0; j < themes.names.size(); ++j)
    s.self_regressors[1U + j] = j == own ? std::span<const f64>(theme) : s.regressors[1U + j];
  return co::Ok();
}
// One decision row: gather the row's names, rank every needed candidate, build the regressors,
// score each listed candidate (the series cells of this row only) and take the row's pair
// values. Writes nothing shared but `series` cells at `row`, `pairs_out` and `spanned_out`.
co::Status process_row(const StreamPlan& plan, Streams& in, const cb::PairwiseRowCorrelation& rho,
                       usize row, RowScratch& s, std::vector<Series>& series,
                       std::span<f64> pairs_out, std::span<u8> spanned_out) {
  const usize n = plan.names, t = plan.labels.begin + row, k_n = plan.needed.size();
  const std::span<const u8> member(in.member.data() + t * n, n);
  s.names.clear();
  for (usize i = 0; i < n; ++i)
    if (!plan.cfg.compact_rows || member[i] != 0) s.names.push_back(i);
  const usize m = s.names.size();
  for (usize j = 0; j < m; ++j) s.member[j] = member[s.names[j]];
  const std::span<const u8> member_m(s.member.data(), m);
  auto mark = Clock::now();
  ATX_TRY_VOID(gather_row(in.pool, t, s, head(s.composite, m)));
  for (usize k = 0; k < k_n; ++k) {
    if (plan.needed[k] == 0) {
      s.rank_rows[k] = std::span<const f64>(s.nan_row.data(), m);
      continue;
    }
    const auto values = head(s.values, m), ranks = head(s.ranks[k], m);
    ATX_TRY_VOID(gather_row(*in.candidates[k], t, s, values));
    const auto ranked = Clock::now(); s.read_seconds += seconds_between(mark, ranked);
    ATX_TRY_VOID(cb::centred_tied_ranks(values, member_m, ranks, s.sorted));
    s.rank_rows[k] = ranks;
    mark = Clock::now(); s.kernel_seconds += seconds_between(ranked, mark);
  }
  // `sorted` is free again: every candidate row above is already ranked.
  ATX_TRY_VOID(build_regressors(plan.themes, m, s));
  if (plan.cfg.exclude_self) {
    member_zero(s, head(s.book, m));
    add_weighted_ranks(plan.themes, s, m, [](usize) { return true; }, head(s.book, m));
  }
  const f64* label = plan.labels.values.data() + row * n;
  for (usize j = 0; j < m; ++j) s.label[j] = label[s.names[j]];
  // m == 0 (no member name): v8's kernel returned NaN, NaN, not spanned, as the cells already hold.
  for (usize k = 0; k < k_n && m > 0U; ++k) {
    if (plan.listed[k] == 0) continue;
    std::span<const std::span<const f64>> regressors = s.regressors;
    if (plan.cfg.exclude_self && plan.themes.theme_of[k] != npos) {
      ATX_TRY_VOID(self_regressors(plan.themes, k, m, s));
      regressors = s.self_regressors;
    }
    ATX_TRY(const auto day,
            cb::marginal_rank_ic_day(head(s.ranks[k], m), regressors, head(s.label, m),
                                     plan.cfg.min_names, s.kernel));
    series[k].raw[row] = day.raw_ic; series[k].marginal[row] = day.marginal_ic;
    spanned_out[k] = day.spanned;
  }
  const auto paired = Clock::now(); s.kernel_seconds += seconds_between(mark, paired);
  ATX_TRY_VOID(rho.day_values(s.rank_rows, pairs_out));
  s.pairwise_seconds += seconds_between(paired, Clock::now());
  return co::Ok();
}
struct StageTimes { f64 read{}, kernel{}, pairwise{}; };
usize band_chunk_rows(usize workers) {
  return workers > 1U ? band_rows_per_worker * workers : 1U;
}
// The rows in chunks; inside a chunk, DetPool date bands (the composition's quotient/remainder
// split) write only their own rows' cells; after each join the chunk's pair values and spanned
// flags are added in date order, so every worker count gives the serial bits (and workers 1 is
// the v8 per-date loop: day_values then accumulate is add_date).
co::Status stream_rows(const StreamPlan& plan, Streams& in, std::vector<Series>& series,
                       cb::PairwiseRowCorrelation& rho, StageTimes& times) {
  const usize workers = plan.cfg.workers, k_n = plan.needed.size();
  std::vector<RowScratch> scratch(workers);
  for (auto& s : scratch) size_scratch(plan, s);
  std::optional<engine::parallel::DetPool> pool;
  if (workers > 1U) pool.emplace(workers);
  const usize chunk = band_chunk_rows(workers), pairs = rho.computed_pairs().size();
  std::vector<f64> pair_values(chunk * pairs);
  std::vector<u8> spanned(chunk * k_n);
  std::vector<co::Status> status;
  for (usize begin = 0; begin < plan.labels.rows; begin += chunk) {
    const usize count = std::min(chunk, plan.labels.rows - begin);
    const usize bands = std::min(count, workers * 4U);
    status.assign(bands, co::Ok());
    std::fill(spanned.begin(), spanned.end(), u8{0});
    // SAFETY (data races): bands partition the chunk's rows; band b writes only its rows' series
    // cells, pair_values and spanned rows, and status[b]; worker w alone uses scratch[w]. The
    // plan, labels, member mask and rho (day_values is const) are read-only, and every payload
    // read holds its reader's lock. parallel_for's barrier orders every write before the
    // date-ordered accumulation below.
    const auto band = [&](usize b, usize worker) {
      const usize first = count / bands * b + count % bands * b / bands;
      const usize last = count / bands * (b + 1U) + count % bands * (b + 1U) / bands;
      for (usize r = first; r < last; ++r) {
        auto done = process_row(plan, in, rho, begin + r, scratch[worker], series,
                                std::span<f64>(pair_values).subspan(r * pairs, pairs),
                                std::span<u8>(spanned).subspan(r * k_n, k_n));
        if (!done) { status[b] = std::move(done); return; }
      }
    };
    if (pool) pool->parallel_for(bands, band);
    else for (usize b = 0; b < bands; ++b) band(b, 0U);
    for (const auto& done : status) if (!done) return done; // the lowest failing band
    for (usize r = 0; r < count; ++r) {
      ATX_TRY_VOID(rho.accumulate(std::span<const f64>(pair_values).subspan(r * pairs, pairs)));
      for (usize k = 0; k < k_n; ++k)
        series[k].spanned += static_cast<usize>(spanned[r * k_n + k]);
    }
  }
  for (const auto& s : scratch) {
    times.read += s.read_seconds; times.kernel += s.kernel_seconds;
    times.pairwise += s.pairwise_seconds;
  }
  return co::Ok();
}
// The pool composite and every needed candidate payload (verified unless vouched for), plus the
// member mask.
co::Result<Streams> open_streams(const Pool& pool, const std::vector<CacheEntry>& entries,
                                 std::span<const u8> needed, Verified& verified) {
  const auto verify = [&](const std::string& sha) {
    const bool vouched = verified.digests.contains(sha);
    verified.accepted += vouched ? 1U : 0U;
    return !vouched;
  };
  ATX_TRY(auto composite, RowReader::open(pool.signal.path, pool.signal.sha, pool.dates, pool.names,
                                          verify(pool.signal.sha)));
  ATX_TRY(auto member, read_pinned<u8>(pool.member));
  if (std::any_of(member.begin(), member.end(), [](u8 v) { return v > 1; }))
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pool member mask is not binary"));
  Streams out{std::move(composite), {}, std::move(member)};
  out.candidates.resize(entries.size());
  for (usize k = 0; k < entries.size(); ++k) {
    if (needed[k] == 0) continue;
    const auto& entry = entries[k];
    ATX_TRY(auto reader, RowReader::open(entry.payload, entry.payload_sha, pool.dates, pool.names,
                                         verify(entry.payload_sha)));
    out.candidates[k].emplace(std::move(reader));
  }
  return co::Ok(std::move(out));
}
// --pair-cache: every computed pair found in the cache is seeded and no longer computed.
struct PairCacheUse { std::optional<MarginalPairCache> cache; usize hits{}; };
co::Result<PairCacheUse> seed_pairs(const MarginalIcConfig& cfg, const Pool& pool, usize rows,
                                    const std::vector<CacheEntry>& entries,
                                    cb::PairwiseRowCorrelation& rho) {
  PairCacheUse out;
  if (cfg.pair_cache_directory.empty()) return co::Ok(std::move(out));
  ATX_TRY(auto cache, open_marginal_pair_cache(cfg.pair_cache_directory,
      MarginalPairScope{pool.role_sha, pool.member.sha, pool.score_begin, rows, cfg.min_names}));
  std::vector<cb::PairSeed> seeds;
  for (const auto& [a, b] : rho.computed_pairs()) {
    const auto hit =
        cache.stats.find(marginal_pair_key(entries[a].payload_sha, entries[b].payload_sha));
    if (hit == cache.stats.end()) continue;
    seeds.push_back({a, b, hit->second.sum, static_cast<usize>(hit->second.dates)});
  }
  ATX_TRY_VOID(rho.seed(seeds));
  out.hits = seeds.size(); out.cache.emplace(std::move(cache));
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
  Json out{{"label", "close[d+1+21]/close[d+1]-1;strict-positive-observed-endpoints;role-maturity;"
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
  // Present only with --exclude-self, so outputs without it keep their bytes.
  if (cfg.exclude_self)
    out["exclude_self"] =
        "book member rows: the book composite regressor is the sum over the other weighted members "
        "of sign*weight*centred-rank (unranked adds 0; the pool's saved composite minus the "
        "member's term for a plain pinned-weights blend) and the member's theme composite is "
        "rebuilt without it (re-ranked under rerank); other rows and themes as for every row";
  return out;
}
} // namespace

MarginalCacheRoots marginal_cache_roots(const MarginalIcConfig& cfg) {
  const fs::path base(cfg.candidate_cache_directory);
  MarginalCacheRoots out;
  if (!cfg.cache_identity.empty()) {
    out.identity = cfg.cache_identity;
    out.own = base / out.identity;
  } else {
    // The runner's own rule (cache_root): DIR for the legacy identity, else DIR/<identity>.
    out.identity = ic_cache_vm_identity().identity;
    IcRunnerConfig runner;
    runner.candidate_cache_directory = cfg.candidate_cache_directory;
    out.own = icd::cache_root(runner);
  }
  // The legacy identity keeps the v8 roots in v8 order (its runner never writes DIR/<identity>);
  // any other identity reads its own root alone, never the legacy DIR (review S1 major).
  if (out.own == base) out.roots = {base / out.identity, base};
  else out.roots = {out.own};
  return out;
}

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

co::Result<u64> marginal_ic_working_bytes(usize dates, usize names, usize score_rows,
                                          usize candidates, usize regressors,
                                          const MarginalWorkingExtras& extras) {
  ATX_TRY(u64 total, marginal_ic_working_bytes(dates, names, score_rows, candidates, regressors));
  const u64 k = candidates, n = names;
  if (extras.workers == 0U || extras.workers > max_workers ||
      extras.computed_pairs > k * (k - 1U) / 2U || extras.cached_pairs > kMaxMarginalCachedPairs)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "working-bytes options (workers 1..16, pairs)"));
  const u64 workers = extras.workers, row_set = n * (2U * k + regressors + 4U) * sizeof(f64);
  total += (workers - 1U) * row_set;                               // band workers' row sets
  if (extras.exclude_self) total += workers * n * 3U * sizeof(f64); // member regressor rows
  if (workers > 1U)                                                // one band chunk of pair values
    total += band_chunk_rows(extras.workers) * (extras.computed_pairs * sizeof(f64) + k);
  total += static_cast<u64>(extras.cached_pairs) * kMarginalCachedPairBytes; // loaded pairs
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
        (!cfg.library_sha256.empty() && !icd::hash_valid(cfg.library_sha256)) ||
        (!cfg.pool_sha256.empty() && !icd::hash_valid(cfg.pool_sha256)))
      return co::Err(fail(co::ErrorCode::InvalidArgument, "bounded config (needs --candidate-cache, --library, --pool, "
          "--role, --output; --min-names >= 3; --max-memory-mib 32..16384)"));
    if (cfg.workers == 0U || cfg.workers > max_workers ||
        (cfg.exclude_self && cfg.themes_path.empty()))
      return co::Err(fail(co::ErrorCode::InvalidArgument,
                          "--workers 1..16; --exclude-self needs --themes (book members are known "
                          "only from the pool's weights)"));
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
    ATX_TRY(const auto listing, read_candidates(cfg, lib));
    ATX_TRY(auto verified, read_verified(cfg));
    // Fix round 1 (DS-1): this build's own root only, every sidecar recording this build's
    // identity, so a Debug and a Release tree never read each other's entries.
    const auto layout = marginal_cache_roots(cfg);
    const auto& identity = layout.identity;
    std::vector<CacheEntry> entries; entries.reserve(lib.size());
    for (const auto& c : lib) {
      ATX_TRY(auto entry, resolve_entry(layout.roots, c, pool, fields, identity));
      entries.push_back(std::move(entry));
    }
    const usize lag = execution_delay + horizon;
    const usize rows = pool.score_end > pool.score_begin + lag ? pool.score_end - pool.score_begin - lag : 0U;
    const usize regressors = 1U + themes.names.size();
    // Pairs: those with a listed candidate (all without --candidates), minus the cached ones.
    cb::PairwiseRowCorrelation rho(lib.size(), cfg.min_names);
    if (!cfg.candidates_path.empty()) ATX_TRY_VOID(rho.restrict_to(listing.listed));
    const auto pair_started = steady::now();
    ATX_TRY(auto pair_cache, seed_pairs(cfg, pool, rows, entries, rho));
    f64 pair_cache_seconds = since(pair_started);
    // A candidate is read and ranked when it gets a row, builds a theme composite or is in a
    // computed pair; any other payload is never opened.
    std::vector<u8> needed(lib.size(), u8{0});
    for (usize k = 0; k < lib.size(); ++k)
      needed[k] = static_cast<u8>(listing.listed[k] != 0 || themes.theme_of[k] != npos);
    for (const auto& [a, b] : rho.computed_pairs()) { needed[a] = 1; needed[b] = 1; }
    const MarginalWorkingExtras extras{cfg.workers, cfg.exclude_self, rho.computed_pairs().size(),
                                       pair_cache.cache ? pair_cache.cache->stats.size() : 0U};
    ATX_TRY(const auto required, marginal_ic_working_bytes(pool.dates, pool.names, rows, lib.size(),
                                                           regressors, extras));
    if (required > cfg.max_working_bytes)
      return co::Err(fail(co::ErrorCode::Unavailable, "required_bytes=" + std::to_string(required) +
          " exceeds --max-memory-mib before any payload load"));
    progress << "marginal: candidates=" << lib.size() << " regressors=" << regressors << " rows=" << rows
             << " names=" << pool.names << " required_bytes=" << required << '\n' << std::flush;
    if (!cfg.candidates_path.empty() || pair_cache.cache)
      progress << "marginal: listed=" << listing.count
               << " computed_pairs=" << rho.computed_pairs().size()
               << " cached_pairs=" << pair_cache.hits << '\n' << std::flush;
    const auto label_started = steady::now();
    ATX_TRY(const auto labels, load_labels(cfg, pool));
    const auto label_seconds = since(label_started);
    progress << "marginal: labels rows=" << labels.rows << " seconds=" << label_seconds << '\n'
             << std::flush;
    const auto verify_started = steady::now();
    ATX_TRY(auto streams, open_streams(pool, entries, needed, verified));
    const auto hash_seconds = since(verify_started);
    const auto opened = static_cast<usize>(std::count(needed.begin(), needed.end(), u8{1})) + 1U;
    progress << "marginal: payloads verified=" << opened - verified.accepted
             << " seconds=" << hash_seconds << '\n' << std::flush;
    const auto stream_started = steady::now();
    std::vector<Series> series(lib.size());
    for (usize k = 0; k < lib.size(); ++k) {
      if (listing.listed[k] == 0) continue;
      series[k].raw.assign(labels.rows, quiet_nan);
      series[k].marginal.assign(labels.rows, quiet_nan);
    }
    StageTimes times;
    const StreamPlan plan{cfg, themes, labels, pool.names, listing.listed, needed};
    ATX_TRY_VOID(stream_rows(plan, streams, series, rho, times));
    const auto stream_seconds = since(stream_started);
    progress << "marginal: streamed rows=" << labels.rows << " seconds=" << stream_seconds << '\n' << std::flush;
    progress << "marginal: stages hash=" << hash_seconds << " read=" << times.read
             << " kernel=" << times.kernel << " pairwise=" << times.pairwise
             << " workers=" << cfg.workers << '\n' << std::flush;
    // Newly computed pairs go to the cache before the output exists (a failed store leaves none).
    Json pair_cache_input = nullptr;
    if (pair_cache.cache) {
      const auto store_started = steady::now();
      MarginalPairStats computed;
      for (const auto& [a, b] : rho.computed_pairs())
        computed.try_emplace(marginal_pair_key(entries[a].payload_sha, entries[b].payload_sha),
                             MarginalPairStat{rho.sum(a, b), rho.dates(a, b)});
      ATX_TRY(const auto shard, store_marginal_pairs(*pair_cache.cache, computed));
      pair_cache_seconds += since(store_started);
      const auto& cache = *pair_cache.cache;
      pair_cache_input = Json{{"directory", cfg.pair_cache_directory},
          {"key_sha256", cache.key_sha}, {"shards_read", cache.shards},
          {"cached_pairs", cache.stats.size()},
          {"hits", pair_cache.hits}, {"computed", rho.computed_pairs().size()},
          {"shard", shard.empty() ? Json(nullptr) : Json(shard)}};
    }
    Json rows_json = Json::array();
    for (usize k = 0; k < lib.size(); ++k)
      if (listing.listed[k] != 0)
        rows_json.push_back(candidate_row(lib, themes, entries, series, rho, k));
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
    // P9 S1 keys, each present only with its option, so outputs without them keep their bytes.
    if (!cfg.candidates_path.empty())
      inputs["candidates"] =
          Json{{"path", listing.path}, {"sha256", listing.sha}, {"ids", listing.ids}};
    if (!cfg.verified_digests_path.empty())
      inputs["verified_digests"] = Json{{"path", verified.path}, {"sha256", verified.sha},
          {"listed", verified.digests.size()}, {"accepted", verified.accepted}};
    if (pair_cache.cache) inputs["pair_cache"] = std::move(pair_cache_input);
    // Timings are never reproducible; the keys beside stream and total are P9 S1's stage split
    // (read, kernel and pairwise are summed over band workers).
    Json stage_seconds{{"stream", stream_seconds}, {"labels", label_seconds},
        {"hash", hash_seconds}, {"read", times.read}, {"kernel", times.kernel},
        {"pairwise", times.pairwise}};
    if (!cfg.pair_cache_directory.empty()) stage_seconds["pair_cache"] = pair_cache_seconds;
    stage_seconds["total"] = since(started);
    Json out{{"schema", std::string(output_schema)}, {"status", "complete"}, {"contract", "K6"},
        {"method", method_json(cfg, themes)}, {"inputs", std::move(inputs)},
        {"window", {{"score_begin", labels.begin}, {"rows", labels.rows}, {"first_decision_session_ns", labels.first_session_ns},
                    {"last_decision_session_ns", labels.last_session_ns}}},
        {"working_bytes", required}, {"stage_seconds", std::move(stage_seconds)},
        {"candidates", std::move(rows_json)}};
    if (cfg.workers > 1U) out["workers"] = cfg.workers;
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
               "    [--candidates FILE] [--pair-cache DIR] [--verified-digests FILE]\n"
               "    [--workers N (1)] [--exclude-self]\n"
               "  Writes NEWDIR/marginal_ic.json (atx.marginal-ic/v1, contract K6): per library candidate ic21,\n"
               "  ic21_hac_t, marginal_ic21, marginal_hac_t (Bartlett lag 21), max_abs_rho, max_rho_member.\n"
               "  --pool: a --save-combined manifest; --role must be its role (bound by SHA256).\n"
               "  --themes: the pool's composition weights (bound by the pool's composition_weights_sha256);\n"
               "    adds one theme composite per weighted theme (<= 33), grouped by the file's\n"
               "    theme block\n"
               "    (re-ranked under ew-theme-std-v1 rerank). --fields: the fields manifest whose\n"
               "    payload SHA256s pick among cache entries of one candidate.\n"
               "  --candidates: one library candidate id per line; only those get rows (rho\n"
               "    for pairs with one); every candidate stays in the regressors and in the\n"
               "    max_abs_rho search.\n"
               "  --pair-cache: pair statistics reused across runs on the role.\n"
               "  --verified-digests: payload SHA256s (one per line) the caller verified; those\n"
               "    payloads are not re-hashed.\n"
               "  --workers: date bands on 1..16 threads (same bytes at every count).\n"
               "  --exclude-self (with --themes): book member rows residualised on regressors\n"
               "    without the member.\n";
        return 0;
      }
      if (key == "--exclude-self") { cfg.exclude_self = true; continue; }
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
      else if (key == "--candidates") cfg.candidates_path = value;
      else if (key == "--pair-cache") cfg.pair_cache_directory = value;
      else if (key == "--verified-digests") cfg.verified_digests_path = value;
      else if (key == "--workers") {
        const auto workers = integer();
        if (workers == 0U || workers > max_workers) throw std::invalid_argument("--workers 1..16");
        cfg.workers = static_cast<usize>(workers);
      } else if (key == "--max-memory-mib") {
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
