#include "storage/snapshot_pool.hpp"

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
#include <thread> // std::this_thread::get_id — the executor-dispatch observable
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/pricing_executor.hpp" // pricing_executor() — the one persistent pool
#include "atx/vol/api/storage/surface_archive.hpp"         // ArchiveV2Header, archive_v2_identity_from_header

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

// Run `fn` when this object dies, on the normal path AND on an unwind. Used for
// exactly one thing here — guaranteeing a loader publishes its entry (L7) — so it
// is deliberately the minimal form rather than a general utility. `fn` must be
// noexcept: it runs during unwinding.
template <class F> class ScopeExit {
public:
  explicit ScopeExit(F fn) noexcept : fn_{std::move(fn)} {}
  ~ScopeExit() { fn_(); }
  ScopeExit(const ScopeExit &) = delete;
  ScopeExit &operator=(const ScopeExit &) = delete;
  ScopeExit(ScopeExit &&) = delete;
  ScopeExit &operator=(ScopeExit &&) = delete;

private:
  F fn_;
};

// One pooled date. Written ONCE by its loader; `ready` is the release/acquire
// publication edge for everything else in it (invariant L1).
struct PoolEntry {
  std::atomic<bool> ready{false};
  std::mutex mutex; // guards the publication + the condition variable only
  std::condition_variable cv;
  SnapshotPtr snapshot; // set iff the load succeeded
  // PRE-SEEDED, and that is the point (invariant L7). An entry is inserted into
  // its shard BEFORE its load runs, so between the insert and the publication it
  // is visible-but-not-ready. If the loader UNWINDS in that window — `bad_alloc`
  // out of the mmap or out of an error-message concatenation, `system_error` out
  // of a lock — the publication guard still marks it ready, and THIS is the
  // answer every parked waiter gets. Seeding it means a ready entry can never be
  // `{no snapshot, no error}`, which a waiter would otherwise read as a
  // successful load of a null snapshot. The string is allocated here, on the
  // insert path, precisely so the unwind path never has to allocate one.
  std::optional<atx::core::Error> error{atx::core::Error{
      ErrorCode::Internal, "SnapshotPool: the archive load for this date did not complete — an "
                           "exception escaped the loader. The entry was evicted, so a later "
                           "acquire retries."}};
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
  std::atomic<std::uint64_t> executor_dispatched_loads{0};

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

  // ── THE LOADER PATH ────────────────────────────────────────────────────────
  //
  // L7 — PUBLICATION IS UNCONDITIONAL. From the insert above to the publication
  // below the entry is in the shard map with `ready == false`, and a thread
  // parked at L4 has NO other way out: nothing but this publication can satisfy
  // its predicate. So every exit from this region must publish — including an
  // exception, which several statements below can genuinely throw
  // (`MarketSnapshot::load` is not noexcept, the mismatch message concatenates
  // strings, `make_shared` allocates, a lock can raise `system_error`). Leaving
  // that window on an unwind would wedge the key FOREVER: the waiters hang, every
  // later acquire of the key hangs, and inside a `warm()` dispatch the parked
  // workers never reach the join barrier, so `warm` hangs too — one `bad_alloc`
  // on one date would wedge every run in the process, and it would surface as a
  // CI timeout rather than a red test.
  //
  // `publish` is therefore `noexcept` and runs from a scope guard. It publishes
  // whatever has been decided so far; an ABANDONED loader (neither a snapshot nor
  // a reported failure, i.e. an exception is in flight) keeps the entry's
  // pre-seeded error and is evicted exactly like a reported failure.
  ArchiveContentIdentity probed{};
  std::optional<atx::core::Error> failure;
  SnapshotPtr snapshot;
  bool published = false;

  const auto publish = [&]() noexcept {
    published = true;
    const bool abandoned = snapshot == nullptr && !failure.has_value();
    try {
      if (abandoned || failure.has_value()) {
        // Never cache a failure: drop the entry so a later acquire re-attempts.
        // The waiters already parked on it still get this answer (they hold the
        // handle), and the eviction happens BEFORE the publication so no thread
        // can find the entry, see it ready-and-failed, and re-park.
        impl_->failed_loads.fetch_add(1u, std::memory_order_relaxed);
        const std::unique_lock lock{shard.mutex};
        const auto found = shard.entries.find(key);
        if (found != shard.entries.end() && found->second == entry) {
          shard.entries.erase(found);
          impl_->resident_entries.fetch_sub(1u, std::memory_order_relaxed);
        }
      }
      const std::lock_guard lock{entry->mutex};
      entry->identity = probed;
      entry->snapshot = snapshot;
      if (!abandoned) {
        // Moved, not copied: the failure path is the one most likely to be an
        // out-of-memory unwind, and a move-assign allocates nothing.
        entry->error = std::move(failure);
      }
      entry->ready.store(true, std::memory_order_release);
    } catch (...) {
      // NOTHING may prevent the publication — a missed store here is exactly the
      // hang this guard exists to prevent. The pre-seeded error keeps the
      // outcome honest even if the assignments above did not run.
      entry->ready.store(true, std::memory_order_release);
    }
    entry->cv.notify_all();
  };
  const ScopeExit publish_guard{[&]() noexcept {
    if (!published) {
      publish();
    }
  }};

  // L3 — the loader holds NO pool lock across the header probe or the mmap.
  const std::string &path = key.path;
  impl_->identity_probes.fetch_add(1u, std::memory_order_relaxed);
  probed = probe_identity(path);

  impl_->archive_opens.fetch_add(1u, std::memory_order_relaxed);
  Result<MarketSnapshot> loaded =
      MarketSnapshot::load(path, query_pricing_tier, /*referenced_uids=*/{}, ArchiveBacking::Sealed);

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

  publish();
  // `failure` was moved into the entry; the published entry is immutable, so it
  // is the authority for what this call returns.
  if (entry->error.has_value()) {
    return atx::core::Err(*entry->error);
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
  // degrades to inline rather than self-oversubscribing.
  //
  // WHETHER THIS ACTUALLY FANS OUT DEPENDS ON THE WINDOW SIZE. `run_dynamic`
  // inlines below its own threshold (`kInlineThreshold == 4`, pricing_executor.cpp),
  // so a warm of 1-3 refs — which is what `RunConfig::prefetch_depth`'s default of
  // 2 produces — runs entirely on the calling thread and never touches a worker.
  // That is the correct behaviour (a 2-archive fan-out cannot amortize a worker
  // wake), but it means the "acquisitions run on pool workers" half of invariant
  // L4 only executes at a look-ahead depth of 4 or more. `executor_dispatched_loads`
  // below is what makes that observable instead of assumed.
  const std::thread::id dispatcher = std::this_thread::get_id();
  pricing_executor().run_dynamic(refs.size(), /*n_threads=*/0u, [&](std::size_t i, unsigned) {
    if (std::this_thread::get_id() != dispatcher) {
      impl_->executor_dispatched_loads.fetch_add(1u, std::memory_order_relaxed);
    }
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
                           impl_->failed_loads.load(std::memory_order_relaxed),
                           impl_->executor_dispatched_loads.load(std::memory_order_relaxed)};
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
