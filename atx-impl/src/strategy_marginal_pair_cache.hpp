#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
// The marginal verb's pairwise-rho cache (P9 S1, CM-1 / CM s.5): `--pair-cache DIR`.
//
// A pair's statistic -- the sum over the pool's mature rows of the clamped daily correlation of
// two candidates' centred ranks over the pool's member names, and the number of rows that
// counted (engine combine::PairwiseRowCorrelation) -- depends on the two payloads and on the
// role, never on the pool's blend, library or themes. It is keyed by (role, member mask, window,
// min_names, method version, build flavour of the computing TU) plus the two payload SHA256s and
// reused by every later marginal run on the role: across waves only new candidates' pairs are
// computed.
//
// Layout: DIR/pairs<v>_<key16>/<record32>.pairs.json, key16 = the first 16 hex of SHA256(key
// text), record32 the first 32 of the shard's record SHA256. One run writes at most one shard
// (the pairs it computed), published no-replace and content-addressed, so concurrent writers
// never collide; a reader loads every shard of its key. A shard is self-hashed and records the
// full key, so a key16 prefix collision or a damaged shard refuses instead of serving, and one
// pair in two shards must carry the same bits. Sums are stored as their IEEE bit pattern (a JSON
// unsigned), so a hit reproduces the computed statistic exactly.
struct MarginalPairStat {
  atx::f64 sum{};
  atx::u64 dates{};
};
// At most this many pairs are held per key (a role's cache), each counted at the byte bound
// below by the marginal verb's working-bytes admission (two 64-hex keys, the map node, the stat).
inline constexpr atx::usize kMaxMarginalCachedPairs = atx::usize{1} << 18;
inline constexpr atx::u64 kMarginalCachedPairBytes = 256U;
// (lower payload SHA256, higher payload SHA256) -> the pair's statistic. The statistic is
// symmetric bit for bit (every moment and product commutes), so the pair's order is free.
using MarginalPairStats = std::map<std::pair<std::string, std::string>, MarginalPairStat>;
[[nodiscard]] std::pair<std::string, std::string> marginal_pair_key(std::string_view a,
                                                                    std::string_view b);
// Everything a pair's bits depend on besides its two payloads (the method version and the
// computing TU's build flavour are added by open_marginal_pair_cache).
struct MarginalPairScope {
  std::string role_sha, member_sha; // the role manifest and the pool's member mask payload
  atx::usize score_begin{}, rows{}, min_names{};
};
struct MarginalPairCache {
  std::string key_text, key_sha;   // canonical key JSON and its SHA256
  std::filesystem::path directory; // DIR/pairs<v>_<key16>
  atx::usize rows{};               // the window's rows (a stored date count never exceeds it)
  atx::usize shards{};             // shards read
  MarginalPairStats stats;         // every cached pair of the key
};
// Reads every shard under the key's directory (absent: an empty cache). Err on a malformed,
// foreign-key or damaged shard, or one pair with two different statistics.
[[nodiscard]] atx::core::Result<MarginalPairCache> open_marginal_pair_cache(
    const std::filesystem::path& root, const MarginalPairScope& scope);
// Publishes `computed` (pairs absent from the cache) as one shard; returns its file name, or ""
// when `computed` is empty. Err on an I/O failure or a racing shard with other bytes.
[[nodiscard]] atx::core::Result<std::string> store_marginal_pairs(
    const MarginalPairCache& cache, const MarginalPairStats& computed);
// The engine sources whose edit can change a cached pair's bits, pinned against
// combine::kPairwiseRowCorrelationVersion (test MarginalIc.PairCacheSourcesPinned).
struct MarginalPairCacheIdentity {
  int version{};
  std::vector<std::string> sources;
  std::string sources_sha256;
};
[[nodiscard]] MarginalPairCacheIdentity marginal_pair_cache_identity();
} // namespace atx::impl::strategy
