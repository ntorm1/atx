#include "atx/vol/backtest.hpp"

#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace atx::vol {

using SnapshotPtr = std::shared_ptr<const MarketSnapshot>;
using SnapshotResult = Result<SnapshotPtr>;

struct SnapshotCache::Impl {
  std::mutex mutex;
  std::unordered_map<std::string, std::shared_future<SnapshotResult>> entries;
  std::atomic<std::uint64_t> loads{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> prefetches{0};
};

namespace {

[[nodiscard]] std::string cache_key(std::string_view path) {
  return std::filesystem::path{path}.lexically_normal().string();
}

[[nodiscard]] SnapshotResult load_snapshot(std::string path) {
  ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(path));
  return std::make_shared<const MarketSnapshot>(std::move(snapshot));
}

[[nodiscard]] std::shared_future<SnapshotResult> start_load(const std::string &path) {
  return std::async(std::launch::async, [path] { return load_snapshot(path); }).share();
}

} // namespace

SnapshotCache::SnapshotCache() : impl_{std::make_shared<Impl>()} {}
SnapshotCache::~SnapshotCache() = default;

void SnapshotCache::prefetch(std::string archive_path) {
  archive_path = cache_key(archive_path);
  std::lock_guard lock{impl_->mutex};
  if (impl_->entries.contains(archive_path)) {
    impl_->hits.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  impl_->prefetches.fetch_add(1u, std::memory_order_relaxed);
  impl_->loads.fetch_add(1u, std::memory_order_relaxed);
  impl_->entries.emplace(archive_path, start_load(archive_path));
}

Result<SnapshotPtr> SnapshotCache::load(std::string_view archive_path) {
  const std::string key = cache_key(archive_path);
  std::shared_future<SnapshotResult> future;
  {
    std::lock_guard lock{impl_->mutex};
    const auto found = impl_->entries.find(key);
    if (found != impl_->entries.end()) {
      impl_->hits.fetch_add(1u, std::memory_order_relaxed);
      future = found->second;
    } else {
      impl_->loads.fetch_add(1u, std::memory_order_relaxed);
      future = start_load(key);
      impl_->entries.emplace(key, future);
    }
  }
  return future.get();
}

SnapshotCacheStats SnapshotCache::stats() const noexcept {
  return SnapshotCacheStats{impl_->loads.load(std::memory_order_relaxed),
                            impl_->hits.load(std::memory_order_relaxed),
                            impl_->prefetches.load(std::memory_order_relaxed)};
}

void SnapshotCache::clear() {
  std::lock_guard lock{impl_->mutex};
  impl_->entries.clear();
}

} // namespace atx::vol
