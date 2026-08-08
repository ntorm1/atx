#include "atx/vol/research/snapshot_pool.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/vol/detail/pricing_executor.hpp" // pricing_executor() — the one persistent pool
#include "atx/vol/surface_archive.hpp"         // ArchiveV2Header, archive_v2_identity_from_header

namespace atx::vol {

using SnapshotPtr = std::shared_ptr<const MarketSnapshot>;

namespace {

// Pool key. The DATE is the pool's notion of identity (it is what `trim`
// compares), but the archive path and the query tier are part of the key too so
// that one date served from two roots, or at two accuracy tiers, never aliases.
struct PoolKey {
  std::string date;
  std::string path; // lexically normalized
  QueryPricingTier query_pricing_tier{QueryPricingTier::LegacyCompatible};

  [[nodiscard]] bool operator==(const PoolKey &) const noexcept = default;
};

struct PoolKeyHash {
  [[nodiscard]] std::size_t operator()(const PoolKey &key) const noexcept {
    const std::size_t d = std::hash<std::string>{}(key.date);
    const std::size_t p = std::hash<std::string>{}(key.path);
    const std::size_t t = static_cast<std::size_t>(key.query_pricing_tier);
    return d ^ (p << 1u) ^ (t << 5u);
  }
};

[[nodiscard]] PoolKey make_key(std::string_view date, std::string_view archive_path,
                               QueryPricingTier query_pricing_tier) {
  return PoolKey{std::string{date},
                 std::filesystem::path{archive_path}.lexically_normal().string(),
                 query_pricing_tier};
}

// Shard selector: hash of the DATE only, so every tier/root variant of one date
// lands in the same shard and `trim`'s date sweep stays a per-shard map walk.
[[nodiscard]] std::size_t shard_of(std::string_view date, std::size_t n_shards) noexcept {
  return std::hash<std::string_view>{}(date) % n_shards;
}

// The archive's content identity, read from its 256-byte v2 header only. Byte-
// content sensitive (see `ArchiveContentIdentity`); an unreadable or not-yet-an-
// archive file yields the default (all-zero) identity so the load that follows
// surfaces the real error. Mirrors `current_identity` in snapshot_cache.cpp —
// deliberately a copy rather than a shared symbol, because that one is a private
// implementation detail of a different cache with a different (per-call, R-19
// rewrite-detecting) contract, and the two must be free to diverge.
[[nodiscard]] ArchiveContentIdentity probe_identity(const std::string &path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return {};
  }
  ArchiveV2Header header{};
  in.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) {
    return {};
  }
  static constexpr char kMagic[8] = {'A', 'T', 'X', 'V', 'S', 'A', '2', '0'};
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
      header.header_size != sizeof(ArchiveV2Header)) {
    return {};
  }
  return archive_v2_identity_from_header(header);
}

// One pooled date. Written ONCE by its loader; `ready` is the release/acquire
// publication edge for everything else in it (invariant L1).
struct PoolEntry {
  std::atomic<bool> ready{false};
  std::mutex mutex; // guards the publication + the condition variable only
  std::condition_variable cv;
  SnapshotPtr snapshot;                // set iff the load succeeded
  std::optional<atx::core::Error> error; // set iff it failed
  std::uint64_t generation{0};
  ArchiveContentIdentity identity{};
};

using EntryPtr = std::shared_ptr<PoolEntry>;

struct Shard {
  mutable std::shared_mutex mutex;
  std::unordered_map<PoolKey, EntryPtr, PoolKeyHash> entries;
};

} // namespace

struct SnapshotPool::Impl {
  explicit Impl(std::size_t n_shards) : shards(std::max<std::size_t>(1u, n_shards)) {}

  std::vector<Shard> shards;
  std::atomic<std::uint64_t> next_generation{1};
  std::atomic<std::uint64_t> archive_opens{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> coalesced_waits{0};
  std::atomic<std::uint64_t> identity_probes{0};
  std::atomic<std::uint64_t> resident_entries{0};
  std::atomic<std::uint64_t> trimmed_entries{0};
  std::atomic<std::uint64_t> failed_loads{0};

  [[nodiscard]] Shard &shard_for(std::string_view date) noexcept {
    return shards[shard_of(date, shards.size())];
  }
  [[nodiscard]] const Shard &shard_for(std::string_view date) const noexcept {
    return shards[shard_of(date, shards.size())];
  }
};

SnapshotPool::SnapshotPool(std::size_t n_shards) : impl_{std::make_unique<Impl>(n_shards)} {}
SnapshotPool::~SnapshotPool() = default;

std::size_t SnapshotPool::shard_count() const noexcept { return impl_->shards.size(); }

Result<SnapshotPtr> SnapshotPool::acquire(std::string_view date, std::string_view archive_path,
                                          QueryPricingTier query_pricing_tier) {
  const PoolKey key = make_key(date, archive_path, query_pricing_tier);
  Shard &shard = impl_->shard_for(date);

  // L1 — warm read path: shared lock only, just long enough to copy the handle.
  EntryPtr entry;
  {
    const std::shared_lock lock{shard.mutex};
    const auto found = shard.entries.find(key);
    if (found != shard.entries.end()) {
      entry = found->second;
    }
  }

  bool is_loader = false;
  if (entry == nullptr) {
    // L2 — write path: unique lock, double-checked insert. Exactly one thread
    // leaves this block as the loader for this (key, generation).
    const std::unique_lock lock{shard.mutex};
    const auto found = shard.entries.find(key);
    if (found != shard.entries.end()) {
      entry = found->second;
    } else {
      entry = std::make_shared<PoolEntry>();
      entry->generation = impl_->next_generation.fetch_add(1u, std::memory_order_relaxed);
      shard.entries.emplace(key, entry);
      impl_->resident_entries.fetch_add(1u, std::memory_order_relaxed);
      is_loader = true;
    }
  }

  if (!is_loader) {
    impl_->hits.fetch_add(1u, std::memory_order_relaxed);
    if (!entry->ready.load(std::memory_order_acquire)) {
      // L4 — block on a RUNNING loader; no lock of ours is held here.
      impl_->coalesced_waits.fetch_add(1u, std::memory_order_relaxed);
      std::unique_lock lock{entry->mutex};
      entry->cv.wait(lock, [&] { return entry->ready.load(std::memory_order_acquire); });
    }
    if (entry->error.has_value()) {
      return atx::core::Err(*entry->error);
    }
    return entry->snapshot;
  }

  // L3 — the loader holds NO pool lock across the header probe or the mmap.
  const std::string &path = key.path;
  impl_->identity_probes.fetch_add(1u, std::memory_order_relaxed);
  const ArchiveContentIdentity probed = probe_identity(path);

  impl_->archive_opens.fetch_add(1u, std::memory_order_relaxed);
  Result<MarketSnapshot> loaded =
      MarketSnapshot::load(path, query_pricing_tier, /*referenced_uids=*/{}, ArchiveBacking::Sealed);

  std::optional<atx::core::Error> failure;
  SnapshotPtr snapshot;
  if (!loaded.has_value()) {
    failure = loaded.error();
  } else if (probed != ArchiveContentIdentity{} && probed != loaded->source_identity()) {
    // The bytes moved between the probe and the map. Under the Sealed
    // declaration this cannot happen, so it means the declaration was false —
    // fail closed rather than pool a snapshot whose provenance we just watched
    // change.
    failure = atx::core::Error{ErrorCode::InvalidArgument,
                               "SnapshotPool: archive '" + path +
                                   "' changed between identity probe and load; a pooled corpus "
                                   "must be read-only (ArchiveBacking::Sealed) for the pool's "
                                   "lifetime"};
  } else {
    snapshot = std::make_shared<const MarketSnapshot>(std::move(*loaded));
  }

  if (failure.has_value()) {
    // Never cache a failure: drop the entry so a later acquire re-attempts. The
    // waiters already parked on it still get this answer (they hold the handle).
    impl_->failed_loads.fetch_add(1u, std::memory_order_relaxed);
    const std::unique_lock lock{shard.mutex};
    const auto found = shard.entries.find(key);
    if (found != shard.entries.end() && found->second == entry) {
      shard.entries.erase(found);
      impl_->resident_entries.fetch_sub(1u, std::memory_order_relaxed);
    }
  }

  {
    const std::lock_guard lock{entry->mutex};
    entry->identity = probed;
    entry->snapshot = snapshot;
    entry->error = failure;
    entry->ready.store(true, std::memory_order_release);
  }
  entry->cv.notify_all();

  if (failure.has_value()) {
    return atx::core::Err(*failure);
  }
  return snapshot;
}

Status SnapshotPool::warm(std::span<const SnapshotRef> refs,
                          QueryPricingTier query_pricing_tier) {
  if (refs.empty()) {
    return atx::core::Ok();
  }
  // One slot per ref so the failure reported below is chosen by REF INDEX, never
  // by which worker happened to finish first.
  std::vector<std::optional<atx::core::Error>> failures(refs.size());
  // Routed through the ONE persistent pricing pool: no thread is created per
  // load, and a nested call (a run already executing inside executor work)
  // degrades to inline rather than self-oversubscribing. A single-ref warm
  // resolves to a fully inline dispatch on the calling thread.
  pricing_executor().run_dynamic(refs.size(), /*n_threads=*/0u, [&](std::size_t i, unsigned) {
    auto got = acquire(refs[i].date, refs[i].archive_path, query_pricing_tier);
    if (!got.has_value()) {
      failures[i] = got.error();
    }
  });
  for (const std::optional<atx::core::Error> &failure : failures) {
    if (failure.has_value()) {
      return atx::core::Err(*failure);
    }
  }
  return atx::core::Ok();
}

std::size_t SnapshotPool::trim(std::string_view dates_before) {
  std::size_t removed = 0;
  for (Shard &shard : impl_->shards) {
    const std::unique_lock lock{shard.mutex};
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
      if (it->first.date < dates_before) {
        it = shard.entries.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
  }
  if (removed != 0u) {
    impl_->trimmed_entries.fetch_add(removed, std::memory_order_relaxed);
    impl_->resident_entries.fetch_sub(removed, std::memory_order_relaxed);
  }
  return removed;
}

void SnapshotPool::clear() {
  std::size_t removed = 0;
  for (Shard &shard : impl_->shards) {
    const std::unique_lock lock{shard.mutex};
    removed += shard.entries.size();
    shard.entries.clear();
  }
  if (removed != 0u) {
    impl_->trimmed_entries.fetch_add(removed, std::memory_order_relaxed);
    impl_->resident_entries.fetch_sub(removed, std::memory_order_relaxed);
  }
}

SnapshotPoolStats SnapshotPool::stats() const noexcept {
  return SnapshotPoolStats{impl_->archive_opens.load(std::memory_order_relaxed),
                           impl_->hits.load(std::memory_order_relaxed),
                           impl_->coalesced_waits.load(std::memory_order_relaxed),
                           impl_->identity_probes.load(std::memory_order_relaxed),
                           impl_->resident_entries.load(std::memory_order_relaxed),
                           impl_->trimmed_entries.load(std::memory_order_relaxed),
                           impl_->failed_loads.load(std::memory_order_relaxed)};
}

ArchiveContentIdentity SnapshotPool::identity_of(std::string_view date,
                                                 std::string_view archive_path,
                                                 QueryPricingTier query_pricing_tier) const {
  const PoolKey key = make_key(date, archive_path, query_pricing_tier);
  const Shard &shard = impl_->shard_for(date);
  EntryPtr entry;
  {
    const std::shared_lock lock{shard.mutex};
    const auto found = shard.entries.find(key);
    if (found == shard.entries.end()) {
      return {};
    }
    entry = found->second;
  }
  if (!entry->ready.load(std::memory_order_acquire)) {
    return {};
  }
  return entry->identity;
}

} // namespace atx::vol
