#include "atx/vol/backtest.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iterator>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace atx::vol {

using SnapshotPtr = std::shared_ptr<const MarketSnapshot>;
using SnapshotResult = Result<SnapshotPtr>;

namespace {

struct SnapshotCacheKey {
  std::string path;
  QueryPricingTier query_pricing_tier{QueryPricingTier::LegacyCompatible};

  [[nodiscard]] bool operator==(const SnapshotCacheKey &) const noexcept = default;
};

struct SnapshotCacheKeyHash {
  [[nodiscard]] std::size_t operator()(const SnapshotCacheKey &key) const noexcept {
    const std::size_t path_hash = std::hash<std::string>{}(key.path);
    const std::size_t tier_hash = static_cast<std::size_t>(key.query_pricing_tier);
    return path_hash ^ (tier_hash << 1u);
  }
};

[[nodiscard]] SnapshotCacheKey cache_key(std::string_view path,
                                         QueryPricingTier query_pricing_tier) {
  return SnapshotCacheKey{std::filesystem::path{path}.lexically_normal().string(),
                          query_pricing_tier};
}

[[nodiscard]] SnapshotResult load_snapshot(std::string path, QueryPricingTier query_pricing_tier) {
  ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(path, query_pricing_tier));
  return std::make_shared<const MarketSnapshot>(std::move(snapshot));
}

[[nodiscard]] std::shared_future<SnapshotResult> start_load(const std::string &path,
                                                            QueryPricingTier query_pricing_tier) {
  return std::async(std::launch::async,
                    [path, query_pricing_tier] { return load_snapshot(path, query_pricing_tier); })
      .share();
}

} // namespace

struct SnapshotCache::Impl {
  struct Entry {
    std::shared_future<SnapshotResult> future;
    std::list<SnapshotCacheKey>::iterator recency;
    std::size_t active_loads{0u};
    std::uint64_t generation{0u};
  };

  explicit Impl(std::optional<std::size_t> max_entries_in = std::nullopt)
      : max_entries{max_entries_in} {}

  using EntryMap = std::unordered_map<SnapshotCacheKey, Entry, SnapshotCacheKeyHash>;

  void touch(EntryMap::iterator entry) {
    recency.splice(recency.end(), recency, entry->second.recency);
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

  std::mutex mutex;
  EntryMap entries;
  std::list<SnapshotCacheKey> recency; // least-recently-used at front
  std::optional<std::size_t> max_entries;
  std::uint64_t next_generation{1u};
  std::atomic<std::uint64_t> loads{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> prefetches{0};
  std::atomic<std::uint64_t> retained_entries{0};
  std::atomic<std::uint64_t> evictions{0};
};

SnapshotCache::SnapshotCache() : impl_{std::make_shared<Impl>()} {}
SnapshotCache::SnapshotCache(std::size_t max_retained_entries)
    : impl_{std::make_shared<Impl>(std::max<std::size_t>(1u, max_retained_entries))} {}
SnapshotCache::~SnapshotCache() = default;

void SnapshotCache::prefetch(std::string archive_path, QueryPricingTier query_pricing_tier) {
  const SnapshotCacheKey key = cache_key(archive_path, query_pricing_tier);
  std::lock_guard lock{impl_->mutex};
  const auto found = impl_->entries.find(key);
  if (found != impl_->entries.end()) {
    impl_->hits.fetch_add(1u, std::memory_order_relaxed);
    impl_->touch(found);
    impl_->trim(key);
    return;
  }
  impl_->prefetches.fetch_add(1u, std::memory_order_relaxed);
  impl_->loads.fetch_add(1u, std::memory_order_relaxed);
  impl_->recency.push_back(key);
  const auto recency = std::prev(impl_->recency.end());
  impl_->entries.emplace(key, Impl::Entry{start_load(key.path, key.query_pricing_tier), recency, 0u,
                                          impl_->next_generation++});
  impl_->retained_entries.store(static_cast<std::uint64_t>(impl_->entries.size()),
                                std::memory_order_relaxed);
  impl_->trim(key);
}

Result<SnapshotPtr> SnapshotCache::load(std::string_view archive_path,
                                        QueryPricingTier query_pricing_tier) {
  const SnapshotCacheKey key = cache_key(archive_path, query_pricing_tier);
  std::shared_future<SnapshotResult> future;
  std::uint64_t generation = 0u;
  {
    std::lock_guard lock{impl_->mutex};
    const auto found = impl_->entries.find(key);
    if (found != impl_->entries.end()) {
      impl_->hits.fetch_add(1u, std::memory_order_relaxed);
      future = found->second.future;
      ++found->second.active_loads;
      generation = found->second.generation;
      impl_->touch(found);
    } else {
      impl_->loads.fetch_add(1u, std::memory_order_relaxed);
      future = start_load(key.path, key.query_pricing_tier);
      impl_->recency.push_back(key);
      const auto recency = std::prev(impl_->recency.end());
      generation = impl_->next_generation++;
      impl_->entries.emplace(key, Impl::Entry{future, recency, 1u, generation});
      impl_->retained_entries.store(static_cast<std::uint64_t>(impl_->entries.size()),
                                    std::memory_order_relaxed);
    }
    impl_->trim(key);
  }
  SnapshotResult result = future.get();
  {
    std::lock_guard lock{impl_->mutex};
    const auto found = impl_->entries.find(key);
    if (found != impl_->entries.end() && found->second.generation == generation) {
      --found->second.active_loads;
      impl_->touch(found);
    }
    impl_->trim(key);
  }
  return result;
}

SnapshotCacheStats SnapshotCache::stats() const noexcept {
  return SnapshotCacheStats{impl_->loads.load(std::memory_order_relaxed),
                            impl_->hits.load(std::memory_order_relaxed),
                            impl_->prefetches.load(std::memory_order_relaxed),
                            impl_->retained_entries.load(std::memory_order_relaxed),
                            impl_->evictions.load(std::memory_order_relaxed)};
}

void SnapshotCache::clear() {
  std::lock_guard lock{impl_->mutex};
  impl_->entries.clear();
  impl_->recency.clear();
  impl_->retained_entries.store(0u, std::memory_order_relaxed);
}

} // namespace atx::vol
