#include "strategy_marginal_pair_cache.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/build_flavor.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/research/store/rows_cache.hpp"
#include "atx/engine/research/store/store.hpp"
#include "strategy_ic_cache_index.hpp"
#include "strategy_ic_detail.hpp" // hash_valid, metadata_text, PartialFile, publish_new

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
namespace cb = atx::engine::combine;
namespace fs = std::filesystem;
namespace icd = ic_detail;
using Json = nlohmann::json;
using PairKey = std::pair<std::string, std::string>;
constexpr std::string_view key_schema = "atx.marginal-pair-key/v1";
constexpr std::string_view shard_schema = "atx.marginal-pair-cache/v1";
constexpr std::string_view shard_suffix = ".pairs.json";
constexpr u64 shard_limit = 16ULL << 20; // one shard (every pair of 256 candidates: ~5 MB)
constexpr usize max_shards = 4096;        // shards read per key
// Tripwire for kPairwiseRowCorrelationVersion: an edit to these files must be a conscious
// bump-or-repin decision (the IC caches' source-pin recipe: SHA-256 over, per path in order,
// "<path>\n<bytes>\n<text>" with the text CRLF->LF normalised). P9 lane D2 (ruling P8):
// centred_tied_ranks calls group_rerank.hpp's for_each_centered_rank, so that header computes pair
// bits too and is the third source; the same sort and rank expression keep every pair's bits, so
// the digest was re-pinned without a version bump (the S1-PIN / S2-1 precedent).
constexpr std::array<std::string_view, 3> pair_sources{
    "atx-engine/include/atx/engine/combine/marginal_rank_ic.hpp",
    "atx-engine/src/combine/marginal_rank_ic.cpp",
    "atx-engine/include/atx/engine/combine/group_rerank.hpp"};
constexpr std::string_view pair_sources_sha256 =
    "df589a889927852b86815920e6ed59a37ea0b44d026d9ffb97ae73f2aa37c7bc";

co::Error fail(co::ErrorCode code, const std::string& message) {
  return co::Error{code, "marginal pair cache: " + message};
}
bool is_text(const Json& j, const char* key, std::string_view expected) {
  return j.is_object() && j.contains(key) && j.at(key).is_string() &&
         j.at(key).get_ref<const std::string&>() == expected;
}
Json key_json(const MarginalPairScope& scope) {
  const auto flavor = cb::marginal_rank_ic_build_flavor();
  return Json{{"schema", std::string(key_schema)},
      {"method_version", cb::kPairwiseRowCorrelationVersion},
      {"statistic", "sum over the window's rows of clamp(Pearson(centred tied ranks over the "
                    "pool's member names with a finite value), -1, 1) on >= min_names jointly "
                    "finite names; dates = the rows that counted"},
      {"role_manifest_sha256", scope.role_sha}, {"member_sha256", scope.member_sha},
      {"score_begin", scope.score_begin}, {"rows", scope.rows}, {"min_names", scope.min_names},
      {"build", std::string(icd::vm_compiler) + atx::engine::fp_flavor_suffix(flavor) + "_" +
                    atx::engine::build_flavor_token(flavor)}};
}
// The value checks of one stored pair (a shard row or, P9 lane SQL4, a pair_stat row).
co::Result<std::pair<PairKey, MarginalPairStat>> checked_pair(std::string lo, std::string hi,
                                                              u64 sum_bits, u64 dates,
                                                              usize rows) {
  const MarginalPairStat stat{std::bit_cast<f64>(sum_bits), dates};
  if (!icd::hash_valid(lo) || !icd::hash_valid(hi) || hi < lo || !std::isfinite(stat.sum) ||
      stat.dates > rows || std::abs(stat.sum) > static_cast<f64>(stat.dates))
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pair row values: " + lo + " " + hi));
  return co::Ok(std::make_pair(PairKey{std::move(lo), std::move(hi)}, stat));
}
// One stored pair: [lower payload SHA256, higher payload SHA256, sum bits, dates].
co::Result<std::pair<PairKey, MarginalPairStat>> parse_pair(const Json& row, usize rows) {
  if (!row.is_array() || row.size() != 4U || !row.at(0).is_string() || !row.at(1).is_string() ||
      !row.at(2).is_number_unsigned() || !row.at(3).is_number_unsigned())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "pair row shape"));
  return checked_pair(row.at(0).get<std::string>(), row.at(1).get<std::string>(),
                      row.at(2).get<u64>(), row.at(3).get<u64>(), rows);
}
// Holds one checked pair: a pair held with other bits refuses (`where` names its source), and so
// does a cache past kMaxMarginalCachedPairs.
co::Status place_pair(MarginalPairCache& cache, const std::pair<PairKey, MarginalPairStat>& entry,
                      const std::string& where) {
  const auto placed = cache.stats.try_emplace(entry.first, entry.second);
  const auto& held = placed.first->second;
  const bool same = std::bit_cast<u64>(held.sum) == std::bit_cast<u64>(entry.second.sum) &&
                    held.dates == entry.second.dates;
  if (!placed.second && !same)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "one pair, two statistics: " +
        entry.first.first + " " + entry.first.second + " in " + where));
  if (cache.stats.size() > kMaxMarginalCachedPairs)
    return co::Err(fail(co::ErrorCode::OutOfRange, "more than " +
        std::to_string(kMaxMarginalCachedPairs) + " cached pairs under " +
        cache.directory.string()));
  return co::Ok();
}
co::Status read_shard(const fs::path& path, const Json& key, MarginalPairCache& cache) {
  ATX_TRY(const auto text, icd::metadata_text(path.string(), shard_limit));
  const auto j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !is_text(j, "schema", shard_schema) || !j.contains("record") ||
      !j.at("record").is_object() || !j.contains("record_sha256") ||
      !j.at("record_sha256").is_string())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "malformed shard: " + path.string()));
  const auto& record = j.at("record");
  ATX_TRY(const auto digest, co::sha256_hex(record.dump()));
  if (digest != j.at("record_sha256").get<std::string>())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "shard integrity: " + path.string()));
  if (!record.contains("key") || record.at("key") != key || !record.contains("pairs") ||
      !record.at("pairs").is_array())
    return co::Err(fail(co::ErrorCode::InvalidArgument, "shard of another key: " + path.string()));
  for (const auto& row : record.at("pairs")) {
    ATX_TRY(const auto entry, parse_pair(row, cache.rows));
    ATX_TRY_VOID(place_pair(cache, entry, path.string()));
  }
  return co::Ok();
}
// The key's shard files in name order (partials and strays skipped).
co::Result<std::vector<fs::path>> shard_files(const fs::path& directory) {
  std::vector<fs::path> out; std::error_code ec;
  fs::directory_iterator it(directory, ec);
  for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (!it->path().filename().string().ends_with(shard_suffix)) continue;
    if (out.size() >= max_shards)
      return co::Err(fail(co::ErrorCode::OutOfRange, "shard listing bound: " + directory.string()));
    out.push_back(it->path());
  }
  if (ec)
    return co::Err(fail(co::ErrorCode::IoError,
                        "listing " + directory.string() + ": " + ec.message()));
  std::sort(out.begin(), out.end());
  return co::Ok(std::move(out));
}
// Every shard under the key's directory (absent: none), in name order.
co::Status read_shards(MarginalPairCache& out) {
  std::error_code ec;
  const bool present = fs::exists(out.directory, ec);
  if (ec) return co::Err(fail(co::ErrorCode::IoError, "probe " + out.directory.string()));
  if (!present) return co::Ok();
  if (!fs::is_directory(out.directory, ec) || ec)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "not a directory: " + out.directory.string()));
  // Parsed back from its own text, so number kinds compare as a shard's parsed key does.
  const auto key = Json::parse(out.key_text);
  ATX_TRY(const auto files, shard_files(out.directory));
  for (const auto& file : files) {
    ATX_TRY_VOID(read_shard(file, key, out));
    ++out.shards;
  }
  return co::Ok();
}

// ---- P9 lane SQL4: the pair cache on DIR/index.sqlite (the header's layout note) ------------
namespace st = atx::engine::research::store;
st::PairKeyRow key_row(const MarginalPairCache& cache) {
  return st::PairKeyRow{cache.key_sha, static_cast<i64>(cb::kPairwiseRowCorrelationVersion),
                        cache.key_text};
}
st::PairStatRow stat_row(const MarginalPairCache& cache, const PairKey& names,
                         const MarginalPairStat& stat) {
  // dates <= the window's rows (checked_pair, or the kernel's count), so the cast is exact.
  return st::PairStatRow{cache.key_sha, names.first, names.second, std::bit_cast<u64>(stat.sum),
                         static_cast<i64>(stat.dates)};
}
// Every pair_stat row of the key, one keyset page at a time, under the shard rows' checks. A
// page the index cannot read is a miss under its policy (its progress line written): nothing
// read is kept, the cache is empty.
co::Status read_rows(MarginalPairCache& out, CacheIndex& index) {
  const auto where = (index.directory() / std::string(st::kCacheIndexName)).string();
  std::string after_lo, after_hi;
  // Bounded: a full page adds kPairPageRows pairs and place_pair refuses past the pair bound.
  for (usize page = 0; page <= kMaxMarginalCachedPairs / kPairPageRows; ++page) {
    ATX_TRY(const auto rows, index.pair_stats_page(out.key_sha, after_lo, after_hi));
    if (!rows) {
      out.stats.clear(); out.rows_read = 0;
      return co::Ok();
    }
    for (const auto& row : *rows) {
      if (row.dates < 0)
        return co::Err(fail(co::ErrorCode::InvalidArgument,
                            "pair row values: " + row.payload_lo + " " + row.payload_hi));
      ATX_TRY(const auto entry, checked_pair(row.payload_lo, row.payload_hi, row.sum_bits,
                                             static_cast<u64>(row.dates), out.rows));
      ATX_TRY_VOID(place_pair(out, entry, where));
      ++out.rows_read;
    }
    if (rows->size() < kPairPageRows) return co::Ok();
    after_lo = rows->back().payload_lo; after_hi = rows->back().payload_hi;
  }
  return co::Err(fail(co::ErrorCode::OutOfRange, "more than " +
      std::to_string(kMaxMarginalCachedPairs) + " cached pairs in " + where));
}
// Read-through: the legacy pairs `out` holds become rows, kPairPageRows per BEGIN IMMEDIATE and
// the key row in the last, so a committed key row means every legacy pair is a row. A chunk
// skipped under the index's policy stops the import (the key stays un-indexed: imported again
// at the next open; the rows already in are re-inserted as no-ops).
co::Status import_pairs(MarginalPairCache& out, CacheIndex& index) {
  if (out.stats.empty() || !index.writable()) return co::Ok();
  out.legacy_pending = true;
  const auto key = key_row(out);
  std::vector<st::PairStatRow> chunk;
  chunk.reserve(std::min(kPairPageRows, out.stats.size()));
  usize left = out.stats.size();
  // Bounded by the pairs held (at most kMaxMarginalCachedPairs).
  for (const auto& [names, stat] : out.stats) {
    chunk.push_back(stat_row(out, names, stat));
    --left;
    if (chunk.size() < kPairPageRows && left > 0U) continue;
    ATX_TRY(const auto put, index.put_pairs(out.key_sha, chunk, left == 0U ? &key : nullptr));
    if (put.outcome == IndexPut::Skipped) return co::Ok();
    out.rows_imported += put.inserted;
    chunk.clear();
  }
  out.legacy_pending = false;
  return co::Ok();
}
co::Status open_indexed(MarginalPairCache& out, CacheIndex& index) {
  ATX_TRY(const auto key, index.find_pair_key(out.key_sha));
  if (!key) {
    ATX_TRY_VOID(read_shards(out));
    return import_pairs(out, index);
  }
  if (key->version != static_cast<i64>(cb::kPairwiseRowCorrelationVersion) ||
      key->key_text != out.key_text)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "index row of another key: " + out.key_sha));
  return read_rows(out, index);
}
} // namespace

PairKey marginal_pair_key(std::string_view a, std::string_view b) {
  return a <= b ? PairKey{std::string(a), std::string(b)} : PairKey{std::string(b), std::string(a)};
}

co::Result<MarginalPairCache> open_marginal_pair_cache(const fs::path& root,
                                                       const MarginalPairScope& scope,
                                                       CacheIndex* index) {
  if (root.empty() || !icd::hash_valid(scope.role_sha) || !icd::hash_valid(scope.member_sha) ||
      scope.rows == 0U || scope.min_names < 3U)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "scope (role and member SHA256, rows, min_names)"));
  MarginalPairCache out;
  out.key_text = key_json(scope).dump();
  ATX_TRY(out.key_sha, co::sha256_hex(out.key_text));
  out.directory = root / ("pairs" + std::to_string(cb::kPairwiseRowCorrelationVersion) + "_" +
                          out.key_sha.substr(0, 16));
  out.rows = scope.rows;
  if (index) {
    ATX_TRY_VOID(open_indexed(out, *index));
  } else {
    ATX_TRY_VOID(read_shards(out));
  }
  return co::Ok(std::move(out));
}

co::Result<std::string> store_marginal_pairs(const MarginalPairCache& cache,
                                             const MarginalPairStats& computed) {
  if (computed.empty()) return co::Ok(std::string{});
  Json pairs = Json::array();
  for (const auto& [names, stat] : computed)
    pairs.push_back(
        Json::array({names.first, names.second, std::bit_cast<u64>(stat.sum), stat.dates}));
  const Json record{{"key", Json::parse(cache.key_text)}, {"pairs", std::move(pairs)}};
  ATX_TRY(const auto digest, co::sha256_hex(record.dump()));
  const Json shard{{"schema", std::string(shard_schema)}, {"record_sha256", digest},
                   {"record", record}};
  const auto text = shard.dump() + "\n";
  std::error_code ec; fs::create_directories(cache.directory, ec);
  if (ec)
    return co::Err(fail(co::ErrorCode::IoError,
                        "directory " + cache.directory.string() + ": " + ec.message()));
  const auto name = digest.substr(0, 32) + std::string(shard_suffix);
  const auto final_path = cache.directory / name;
  const icd::PartialFile partial(final_path);
  ATX_TRY_VOID(icd::write_partial(partial, std::as_bytes(std::span(text.data(), text.size()))));
  ATX_TRY(const bool existed, icd::publish_new(partial.path, final_path));
  if (existed) {
    // Content-addressed: the same pairs computed by a concurrent run carry the same bytes.
    ATX_TRY(const auto committed, icd::metadata_text(final_path.string(), shard_limit));
    if (committed != text)
      return co::Err(fail(co::ErrorCode::AlreadyExists, "shard raced with other bytes: " + name));
  }
  return co::Ok(name);
}

co::Result<MarginalPairRowsStored> store_marginal_pair_rows(const MarginalPairCache& cache,
                                                            const MarginalPairStats& computed,
                                                            CacheIndex& index) {
  // Nothing to write: no pair, and the key row is present (rows were read) or must wait for
  // the legacy import to commit.
  if (computed.empty() && (cache.legacy_pending || cache.rows_read > 0U))
    return co::Ok(MarginalPairRowsStored{});
  // At most one row per computed pair (<= candidates^2 / 2): bounded like today's shard text.
  std::vector<st::PairStatRow> rows;
  rows.reserve(computed.size());
  for (const auto& [names, stat] : computed) rows.push_back(stat_row(cache, names, stat));
  const auto key = key_row(cache);
  ATX_TRY(const auto put,
          index.put_pairs(cache.key_sha, rows, cache.legacy_pending ? nullptr : &key));
  return co::Ok(MarginalPairRowsStored{put.inserted, put.outcome == IndexPut::Skipped});
}

MarginalPairCacheIdentity marginal_pair_cache_identity() {
  MarginalPairCacheIdentity out{cb::kPairwiseRowCorrelationVersion, {},
                                std::string(pair_sources_sha256)};
  for (const auto path : pair_sources) out.sources.emplace_back(path);
  return out;
}
} // namespace atx::impl::strategy
