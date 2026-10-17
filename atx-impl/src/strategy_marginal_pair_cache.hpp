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
//
// P9 lane SQL4 (--pair-cache-index sqlite; strategy_ic_cache_index.hpp): DIR/index.sqlite holds
// the key as one pair_key row (version, key text) and its pairs as pair_stat rows (the two
// payload SHA256s, the sum's bits, the dates); no shard is written. Open: with the key row, every
// pair_stat row of the key, page by page in key order, under the shard rows' value checks;
// without it, today's shards are read exactly as without the index and imported (pages of
// kPairPageRows rows, the key row in the last, so a key row means every legacy pair is a row).
// Store: the computed pairs in one BEGIN IMMEDIATE, ON CONFLICT DO NOTHING, a present pair with
// other bits refusing as two shards with different statistics do. Once the key row exists the
// shards are not read again (a pair only in a later shard is a miss: computed, same bits). The
// pair bound and the admission bytes are those of the shards; a page and an import chunk are
// transient, inside the verb's 32 MiB metadata term. A null index is today's code and bytes.
class CacheIndex;
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
  atx::usize shards{};             // shards read (with an index: legacy shards read through)
  MarginalPairStats stats;         // every cached pair of the key
  // P9 lane SQL4, with an index only (else 0 / false):
  atx::usize rows_read{};     // pair_stat rows read
  atx::usize rows_imported{}; // legacy shard pairs this open inserted as rows
  bool legacy_pending{};      // legacy pairs read whose import did not commit (no key row yet)
};
// Reads every shard under the key's directory (absent: an empty cache). Err on a malformed,
// foreign-key or damaged shard, or one pair with two different statistics. `index` (P9 lane
// SQL4; null: the shards, today's code): the key's rows, legacy shards read through (above); a
// key row of another version or key text refuses as a shard of another key; a page the index
// cannot read is a miss (an empty cache, the index's progress line written).
[[nodiscard]] atx::core::Result<MarginalPairCache> open_marginal_pair_cache(
    const std::filesystem::path& root, const MarginalPairScope& scope, CacheIndex* index);
// Publishes `computed` (pairs absent from the cache) as one shard; returns its file name, or ""
// when `computed` is empty. Err on an I/O failure or a racing shard with other bytes.
[[nodiscard]] atx::core::Result<std::string> store_marginal_pairs(
    const MarginalPairCache& cache, const MarginalPairStats& computed);
// P9 lane SQL4: the store of an indexed cache (open_marginal_pair_cache with `index`).
struct MarginalPairRowsStored {
  atx::usize written{}; // pair rows inserted (a pair a concurrent run stored first is not counted)
  bool skipped{};       // the write was skipped under the index's failure policy
};
// `computed` as pair_stat rows in one BEGIN IMMEDIATE, with the key's pair_key row unless legacy
// pairs are still to import. Err when a present pair or key row carries other values (nothing is
// written then); a busy or I/O failure is a skipped store (progress line), never an Err.
[[nodiscard]] atx::core::Result<MarginalPairRowsStored> store_marginal_pair_rows(
    const MarginalPairCache& cache, const MarginalPairStats& computed, CacheIndex& index);
// The engine sources whose edit can change a cached pair's bits, pinned against
// combine::kPairwiseRowCorrelationVersion (test MarginalIc.PairCacheSourcesPinned).
struct MarginalPairCacheIdentity {
  int version{};
  std::vector<std::string> sources;
  std::string sources_sha256;
};
[[nodiscard]] MarginalPairCacheIdentity marginal_pair_cache_identity();
} // namespace atx::impl::strategy
