#include "atx/vol/backtest.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/vol/surface_archive.hpp" // ArchiveV2Header / ArchiveContentIdentity (R-19 identity)

namespace atx::vol {

using SnapshotPtr = std::shared_ptr<const MarketSnapshot>;
using SnapshotResult = Result<SnapshotPtr>;

namespace {

struct SnapshotCacheKey {
  std::string path;
  QueryPricingTier query_pricing_tier{QueryPricingTier::LegacyCompatible};
  // WS-ZC1: the backing is part of the identity so an entry loaded as Sealed (holding
  // a mapping) is never handed to a caller that asked for Mutable, and vice versa.
  ArchiveBacking backing{ArchiveBacking::Mutable};

  [[nodiscard]] bool operator==(const SnapshotCacheKey &) const noexcept = default;
};

struct SnapshotCacheKeyHash {
  [[nodiscard]] std::size_t operator()(const SnapshotCacheKey &key) const noexcept {
    const std::size_t path_hash = std::hash<std::string>{}(key.path);
    const std::size_t tier_hash = static_cast<std::size_t>(key.query_pricing_tier);
    const std::size_t backing_hash = static_cast<std::size_t>(key.backing);
    return path_hash ^ (tier_hash << 1u) ^ (backing_hash << 5u);
  }
};

[[nodiscard]] SnapshotCacheKey cache_key(std::string_view path,
                                         QueryPricingTier query_pricing_tier,
                                         ArchiveBacking backing) {
  return SnapshotCacheKey{std::filesystem::path{path}.lexically_normal().string(),
                          query_pricing_tier, backing};
}

[[nodiscard]] bool is_fast_tier(QueryPricingTier tier) noexcept {
  return tier == QueryPricingTier::RepresentativeFast || tier == QueryPricingTier::CarryBank;
}

[[nodiscard]] bool is_valid_build_policy(QueryCacheBuildPolicy policy) noexcept {
  switch (policy) {
  case QueryCacheBuildPolicy::Eager:
  case QueryCacheBuildPolicy::ReuseOnly:
    return true;
  }
  return false;
}

using UidSubset = std::shared_ptr<const std::vector<std::uint32_t>>;

[[nodiscard]] SnapshotResult load_snapshot(std::string path, QueryPricingTier query_pricing_tier,
                                           const UidSubset &referenced_uids,
                                           ArchiveBacking backing) {
  const std::span<const std::uint32_t> subset =
      referenced_uids ? std::span<const std::uint32_t>{*referenced_uids}
                      : std::span<const std::uint32_t>{};
  ATX_TRY(MarketSnapshot snapshot,
          MarketSnapshot::load(path, query_pricing_tier, subset, backing));
  return std::make_shared<const MarketSnapshot>(std::move(snapshot));
}

[[nodiscard]] std::shared_future<SnapshotResult> start_load(const std::string &path,
                                                            QueryPricingTier query_pricing_tier,
                                                            UidSubset referenced_uids,
                                                            ArchiveBacking backing) {
  return std::async(std::launch::async,
                    [path, query_pricing_tier, referenced_uids = std::move(referenced_uids),
                     backing] {
                      return load_snapshot(path, query_pricing_tier, referenced_uids, backing);
                    })
      .share();
}

// R-19 (F6): the current content identity of the archive at `path`, read from its
// 256-byte v2 header only. Any successful read produces a non-zero, byte-content-
// sensitive identity (see `ArchiveContentIdentity`); an unreadable / not-yet-an-
// archive file yields the default (all-zero) identity so the actual load surfaces
// the real error while the cache still keys deterministically. Called BEFORE the
// cache mutex so the small header read never serializes other cache operations.
// S4 clean break: partitions are ATXVSA2 (magic "ATXVSA20"); v1 is gone.
[[nodiscard]] ArchiveContentIdentity current_identity(const std::string &path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return {};
  }
  ArchiveV2Header header{};
  in.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) {
    return {}; // shorter than a header — not a valid archive (yet)
  }
  static constexpr char kMagic[8] = {'A', 'T', 'X', 'V', 'S', 'A', '2', '0'};
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
      header.header_size != sizeof(ArchiveV2Header)) {
    return {}; // not an ATXVSA2 archive; identity is unknown
  }
  return archive_v2_identity_from_header(header);
}

} // namespace

struct SnapshotCache::Impl {
  struct Entry {
    std::shared_future<SnapshotResult> future;
    std::list<SnapshotCacheKey>::iterator recency;
    std::size_t active_loads{0u};
    std::uint64_t generation{0u};
    // R-19 (F6): the archive content identity this entry was loaded against. A
    // later request whose freshly-read identity differs means the archive was
    // rewritten in place — the entry is stale and must be evicted, never served.
    ArchiveContentIdentity identity{};
  };

  explicit Impl(std::optional<std::size_t> max_entries_in = std::nullopt,
                UidSubset referenced_uids_in = nullptr,
                ArchiveBacking backing_in = ArchiveBacking::Mutable)
      : backing{backing_in}, max_entries{max_entries_in},
        referenced_uids{std::move(referenced_uids_in)} {}

  // WS-ZC1: the archive lifecycle this cache's loads declare. Mutable by default —
  // Sealed is opt-in and only valid for a read-only corpus.
  //
  // CONST BY CONSTRUCTION (WS-ZC regression fix). This is part of the cache key, so
  // mutating it mid-flight orphans every entry already cached under the old backing
  // rather than retuning them — see the note on SnapshotCache::archive_backing().
  // Being immutable also makes the unlocked reads in `cache_key` below race-free.
  const ArchiveBacking backing{ArchiveBacking::Mutable};

  using EntryMap = std::unordered_map<SnapshotCacheKey, Entry, SnapshotCacheKeyHash>;

  // EVICTION ORDER IS INSERTION ORDER (FIFO), NOT RECENCY (LRU) — deliberately.
  //
  // This cache's bounded mode exists for FORWARD-ONLY one-pass sweeps: a backtest
  // walks the clock once and never asks for a date again. Ranking that traversal by
  // recency is the classic sequential-flooding pathology — for a pure scan the
  // most-recently-USED entry is the most-unneeded one, so promoting each snapshot as
  // it is consumed ranks the dates already behind the cursor ABOVE the read-ahead
  // entries the next steps are about to need, and the eviction victim becomes a
  // completed prefetch. The run then reloads an archive it had already deserialized.
  // Measured on the 135-session projected replay before this change: 263 snapshot
  // loads for 135 sessions at look-ahead depth 4, wall time roughly doubled, and the
  // output bit-identical throughout — a pure pessimization nothing but a profile
  // could see. The database-buffer-pool literature's remedy for a scan is exactly
  // this: don't let the scan's own references drive replacement.
  //
  // Under insertion order the front of `recency` is the LOWEST-indexed live entry,
  // which for a forward-only walk is always behind the cursor and therefore safe to
  // drop; the caller still holds `base` by shared_ptr, so evicting its map entry
  // frees nothing it is using. That makes the working-set capacity derivable rather
  // than a coincidence: base + shifted + `depth` in flight = depth + 2, exactly what
  // `private_snapshot_cache_capacity` returns.
  //
  // Nothing is lost for the UNBOUNDED (reusable) mode, whose `trim` returns before
  // ever consulting this order.
  //
  // `promote_on_use` is intentionally ABSENT rather than a no-op function: leaving a
  // do-nothing touch() at each former call site would read as "recency is
  // maintained here" to the next reader, which is the belief this comment exists to
  // correct.

  // R-19 (F6): must hold `mutex`. If `found` is a live entry whose stored content
  // identity no longer matches `identity` (the archive at this key's path was
  // rewritten in place), EVICT it and return end() so the caller reloads the new
  // bytes — never serving the stale snapshot. Generation-guarding in
  // `release_active_load` makes any in-flight caller's later release a no-op on
  // this key, so evicting an entry with outstanding loads is safe. A default
  // (all-zero) `identity` — an unreadable / not-yet-written archive — is treated
  // as "unknown, do not evict" so a transient stat failure never thrashes a live
  // entry.
  EntryMap::iterator evict_if_stale(EntryMap::iterator found,
                                    const ArchiveContentIdentity &identity) {
    if (found == entries.end() || identity == ArchiveContentIdentity{} ||
        found->second.identity == identity) {
      return found;
    }
    recency.erase(found->second.recency);
    entries.erase(found);
    evictions.fetch_add(1u, std::memory_order_relaxed);
    retained_entries.store(static_cast<std::uint64_t>(entries.size()), std::memory_order_relaxed);
    return entries.end();
  }

  // Must be called with mutex held. A shared_future copied by a load caller owns
  // its shared state independently, but we additionally retain every in-flight
  // entry in the map so later duplicate requests continue to coalesce.
  void trim(const SnapshotCacheKey &protected_key) {
    if (!max_entries.has_value()) {
      return;
    }
    while (entries.size() > *max_entries) {
      auto victim = recency.end();
      for (auto candidate = recency.begin(); candidate != recency.end(); ++candidate) {
        if (*candidate == protected_key) {
          continue;
        }
        const auto found = entries.find(*candidate);
        if (found != entries.end() && found->second.active_loads == 0u &&
            found->second.future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
          victim = candidate;
          break;
        }
      }
      if (victim == recency.end()) {
        break; // all candidates are still loading; trim on a later operation
      }
      const auto found = entries.find(*victim);
      entries.erase(found);
      recency.erase(victim);
      evictions.fetch_add(1u, std::memory_order_relaxed);
    }
    retained_entries.store(static_cast<std::uint64_t>(entries.size()), std::memory_order_relaxed);
  }

  mutable std::mutex mutex;
  EntryMap entries;
  std::list<SnapshotCacheKey> recency; // least-recently-used at front
  std::optional<std::size_t> max_entries;
  // B1: the book's referenced uids applied to every deserialize through this cache
  // (empty/null => whole board). Immutable after construction; captured by the async
  // loader, so held by shared_ptr to avoid per-load copies.
  UidSubset referenced_uids;
  std::uint64_t next_generation{1u};
  std::atomic<std::uint64_t> loads{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> prefetches{0};
  std::atomic<std::uint64_t> retained_entries{0};
  std::atomic<std::uint64_t> evictions{0};
  std::atomic<std::uint64_t> fast_build_loads{0};
  std::atomic<std::uint64_t> reuse_only_fast_hits{0};
  std::atomic<std::uint64_t> reuse_only_cold_resolutions{0};
};

SnapshotCache::SnapshotCache() : impl_{std::make_shared<Impl>()} {}
SnapshotCache::SnapshotCache(ArchiveBacking backing)
    : impl_{std::make_shared<Impl>(std::nullopt, nullptr, backing)} {}
SnapshotCache::SnapshotCache(std::size_t max_retained_entries, ArchiveBacking backing)
    : impl_{std::make_shared<Impl>(std::max<std::size_t>(1u, max_retained_entries), nullptr,
                                   backing)} {}
SnapshotCache::SnapshotCache(std::size_t max_retained_entries,
                             std::vector<std::uint32_t> referenced_uids, ArchiveBacking backing)
    : impl_{std::make_shared<Impl>(
          std::max<std::size_t>(1u, max_retained_entries),
          referenced_uids.empty()
              ? nullptr
              : std::make_shared<const std::vector<std::uint32_t>>(std::move(referenced_uids)),
          backing)} {}
SnapshotCache::~SnapshotCache() = default;

// Immutable after construction, so this needs no lock.
ArchiveBacking SnapshotCache::archive_backing() const noexcept { return impl_->backing; }

void SnapshotCache::prefetch(std::string archive_path, QueryPricingTier query_pricing_tier) {
  (void)prefetch(std::move(archive_path), query_pricing_tier, QueryCacheBuildPolicy::Eager);
}

Status SnapshotCache::prefetch(std::string archive_path, QueryPricingTier query_pricing_tier,
                               QueryCacheBuildPolicy build_policy) {
  if (!is_valid_build_policy(build_policy)) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "SnapshotCache::prefetch: invalid query-cache build policy");
  }
  const SnapshotCacheKey requested_key = cache_key(archive_path, query_pricing_tier, impl_->backing);
  // R-19: read the archive's content identity before the lock (small header read).
  const ArchiveContentIdentity identity = current_identity(requested_key.path);
  std::lock_guard lock{impl_->mutex};
  SnapshotCacheKey effective_key = requested_key;
  auto found = impl_->evict_if_stale(impl_->entries.find(effective_key), identity);
  const bool reuse_only_fast =
      build_policy == QueryCacheBuildPolicy::ReuseOnly && is_fast_tier(query_pricing_tier);
  if (found != impl_->entries.end() && reuse_only_fast) {
    impl_->reuse_only_fast_hits.fetch_add(1u, std::memory_order_relaxed);
  } else if (found == impl_->entries.end() && reuse_only_fast) {
    impl_->reuse_only_cold_resolutions.fetch_add(1u, std::memory_order_relaxed);
    effective_key.query_pricing_tier = QueryPricingTier::ColdReference;
    found = impl_->evict_if_stale(impl_->entries.find(effective_key), identity);
  }
  if (found != impl_->entries.end()) {
    impl_->hits.fetch_add(1u, std::memory_order_relaxed);
    impl_->trim(effective_key);
    return atx::core::Ok();
  }
  impl_->prefetches.fetch_add(1u, std::memory_order_relaxed);
  impl_->loads.fetch_add(1u, std::memory_order_relaxed);
  impl_->recency.push_back(effective_key);
  const auto recency = std::prev(impl_->recency.end());
  impl_->entries.emplace(
      effective_key,
      Impl::Entry{start_load(effective_key.path, effective_key.query_pricing_tier,
                             impl_->referenced_uids, effective_key.backing),
                  recency, 0u, impl_->next_generation++, identity});
  if (is_fast_tier(effective_key.query_pricing_tier)) {
    impl_->fast_build_loads.fetch_add(1u, std::memory_order_relaxed);
  }
  impl_->retained_entries.store(static_cast<std::uint64_t>(impl_->entries.size()),
                                std::memory_order_relaxed);
  impl_->trim(effective_key);
  return atx::core::Ok();
}

Result<SnapshotPtr> SnapshotCache::load(std::string_view archive_path,
                                        QueryPricingTier query_pricing_tier) {
  return load(archive_path, query_pricing_tier, QueryCacheBuildPolicy::Eager);
}

Result<SnapshotPtr> SnapshotCache::load(std::string_view archive_path,
                                        QueryPricingTier query_pricing_tier,
                                        QueryCacheBuildPolicy build_policy) {
  if (!is_valid_build_policy(build_policy)) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "SnapshotCache::load: invalid query-cache build policy");
  }
  const SnapshotCacheKey requested_key = cache_key(archive_path, query_pricing_tier, impl_->backing);
  // R-19: read the archive's content identity before the lock (small header read).
  const ArchiveContentIdentity identity = current_identity(requested_key.path);
  SnapshotCacheKey effective_key = requested_key;
  std::shared_future<SnapshotResult> future;
  std::uint64_t generation = 0u;
  {
    std::lock_guard lock{impl_->mutex};
    auto found = impl_->evict_if_stale(impl_->entries.find(effective_key), identity);
    const bool reuse_only_fast =
        build_policy == QueryCacheBuildPolicy::ReuseOnly && is_fast_tier(query_pricing_tier);
    if (found != impl_->entries.end() && reuse_only_fast) {
      impl_->reuse_only_fast_hits.fetch_add(1u, std::memory_order_relaxed);
    } else if (found == impl_->entries.end() && reuse_only_fast) {
      impl_->reuse_only_cold_resolutions.fetch_add(1u, std::memory_order_relaxed);
      effective_key.query_pricing_tier = QueryPricingTier::ColdReference;
      found = impl_->evict_if_stale(impl_->entries.find(effective_key), identity);
    }
    if (found != impl_->entries.end()) {
      impl_->hits.fetch_add(1u, std::memory_order_relaxed);
      future = found->second.future;
      ++found->second.active_loads;
      generation = found->second.generation;
    } else {
      impl_->loads.fetch_add(1u, std::memory_order_relaxed);
      future = start_load(effective_key.path, effective_key.query_pricing_tier,
                          impl_->referenced_uids, effective_key.backing);
      impl_->recency.push_back(effective_key);
      const auto recency = std::prev(impl_->recency.end());
      generation = impl_->next_generation++;
      impl_->entries.emplace(effective_key, Impl::Entry{future, recency, 1u, generation, identity});
      if (is_fast_tier(effective_key.query_pricing_tier)) {
        impl_->fast_build_loads.fetch_add(1u, std::memory_order_relaxed);
      }
      impl_->retained_entries.store(static_cast<std::uint64_t>(impl_->entries.size()),
                                    std::memory_order_relaxed);
    }
    impl_->trim(effective_key);
  }
  const auto release_active_load = [&] {
    std::lock_guard lock{impl_->mutex};
    const auto found = impl_->entries.find(effective_key);
    if (found != impl_->entries.end() && found->second.generation == generation) {
      assert(found->second.active_loads > 0u);
      if (found->second.active_loads > 0u) {
        --found->second.active_loads;
      }
    }
    impl_->trim(effective_key);
  };
  SnapshotResult result = [&]() -> SnapshotResult {
    try {
      return future.get();
    } catch (...) {
      // A shared async state can propagate an allocation/system exception
      // instead of returning Result. Release the bounded-LRU pin before
      // preserving that exceptional contract.
      release_active_load();
      throw;
    }
  }();
  release_active_load();
  if (!result.has_value() && build_policy == QueryCacheBuildPolicy::ReuseOnly &&
      is_fast_tier(query_pricing_tier) && effective_key.query_pricing_tier == query_pricing_tier) {
    // A pre-existing/in-flight eager fast build can fail even though the archive
    // is valid for cold serving (for example, a non-Andersen-Lake surface).
    // ReuseOnly is a best-available acceleration contract, so make its outcome
    // independent of cache history and coalesce on the cold key.
    impl_->reuse_only_cold_resolutions.fetch_add(1u, std::memory_order_relaxed);
    return load(requested_key.path, QueryPricingTier::ColdReference, QueryCacheBuildPolicy::Eager);
  }
  return result;
}

SnapshotCacheStats SnapshotCache::stats() const noexcept {
  return SnapshotCacheStats{impl_->loads.load(std::memory_order_relaxed),
                            impl_->hits.load(std::memory_order_relaxed),
                            impl_->prefetches.load(std::memory_order_relaxed),
                            impl_->retained_entries.load(std::memory_order_relaxed),
                            impl_->evictions.load(std::memory_order_relaxed),
                            impl_->fast_build_loads.load(std::memory_order_relaxed),
                            impl_->reuse_only_fast_hits.load(std::memory_order_relaxed),
                            impl_->reuse_only_cold_resolutions.load(std::memory_order_relaxed)};
}

void SnapshotCache::clear() {
  std::lock_guard lock{impl_->mutex};
  impl_->entries.clear();
  impl_->recency.clear();
  impl_->retained_entries.store(0u, std::memory_order_relaxed);
}

} // namespace atx::vol
