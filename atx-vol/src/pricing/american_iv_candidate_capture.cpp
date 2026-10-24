#include "pricing/american_iv_candidate_capture.hpp"

#include <algorithm>
#include <atomic>

namespace atx::vol::research {
namespace {
// Foreign threads only access process-lifetime atomics, never the scoped owner.
// Default seq_cst ordering publishes the active interval. Its enclosing caller
// joins workers before destruction, as required by the capture contract.
std::atomic<bool> active{false};
std::atomic<std::uint64_t> foreign_events{0};
thread_local IvCandidateCapture *local_owner = nullptr;

[[nodiscard]] IvCandidateCapture *current_owner() noexcept {
  if (local_owner != nullptr) return local_owner;
  if (active.load()) ++foreign_events;
  return nullptr;
}
} // namespace

bool iv_candidate_capture_enabled() noexcept {
#ifdef ATX_VOL_CAPTURE_PRICE_CANDIDATES
  return true;
#else
  return false;
#endif
}

IvCandidateCapture::IvCandidateCapture(std::span<IvCandidateCaptureRecord> records,
    std::span<IvCandidateCacheLifetime> caches) noexcept : records_(records), caches_(caches) {
  std::fill(records_.begin(), records_.end(), IvCandidateCaptureRecord{});
  std::fill(caches_.begin(), caches_.end(), IvCandidateCacheLifetime{});
}
bool IvCandidateCapture::valid() const noexcept {
  if (active_ || !finished_ || invalid_ || record_overflow_ || cache_overflow_ ||
      attempted_ == 0 || attempted_ != record_count_) return false;
  return std::all_of(records().begin(), records().end(), [](const auto &record) {
      if (!record.trace.complete || record.trace.outcome == IvCandidateOutcome::Incomplete)
        return false;
      return std::all_of(record.trace.phases.begin(), record.trace.phases.end(), [](const auto &phase) {
        return phase.visited ? phase.complete && phase.outcome != IvCandidatePhaseOutcome::NotVisited
                             : !phase.complete && phase.outcome == IvCandidatePhaseOutcome::NotVisited;
      });
    }) && std::all_of(cache_lifetimes().begin(), cache_lifetimes().end(), [](const auto &cache) {
      return cache.closed;
    });
}
std::span<const IvCandidateCaptureRecord> IvCandidateCapture::records() const noexcept {
  return records_.first(record_count_);
}
std::span<const IvCandidateCacheLifetime> IvCandidateCapture::cache_lifetimes() const noexcept {
  return caches_.first(cache_count_);
}
IvCandidateCacheLifetime *IvCandidateCapture::find_cache(const IvPriceSeedFloorCache *cache) noexcept {
  // Addresses are opaque lifetime keys, never a means to access storage.
  const auto address = reinterpret_cast<std::uintptr_t>(cache);
  for (auto &entry : caches_.first(cache_count_))
    if (!entry.closed && entry.address == address) return &entry;
  return nullptr;
}

IvCandidateCaptureScope::IvCandidateCaptureScope(IvCandidateCapture &capture) noexcept {
  foreign_before_ = foreign_events.load();
  bool expected = false;
  if (!iv_candidate_capture_enabled() || capture.finished_ ||
      !active.compare_exchange_strong(expected, true)) {
    capture.invalid_ = true;
    if (local_owner != nullptr) local_owner->invalid_ = true;
    ++foreign_events;
    return;
  }
  capture.active_ = true;
  owner_ = &capture;
  local_owner = &capture;
}
IvCandidateCaptureScope::~IvCandidateCaptureScope() {
  if (owner_ == nullptr) return;
  active.store(false);
  owner_->invalid_ = owner_->invalid_ || foreign_events.load() != foreign_before_;
  owner_->active_ = false;
  owner_->finished_ = true;
  local_owner = nullptr;
}

IvPriceCandidateTrace *candidate_capture_record(IvPriceSeedFloorCache *cache) noexcept {
  auto *owner = current_owner();
  if (owner == nullptr) return nullptr;
  const auto sequence = owner->attempted_++;
  std::uint64_t cache_id = 0;
  if (cache != nullptr) {
    const auto *entry = owner->find_cache(cache);
    if (entry == nullptr) owner->invalid_ = true;
    else cache_id = entry->id;
  }
  if (owner->record_count_ == owner->records_.size()) {
    owner->record_overflow_ = true;
    return nullptr;
  }
  auto &record = owner->records_[owner->record_count_++];
  record = {};
  record.sequence = sequence;
  record.cache_id = cache_id;
  return &record.trace;
}
void candidate_capture_cache_created(const IvPriceSeedFloorCache *cache) noexcept {
  auto *owner = current_owner();
  if (owner == nullptr) return;
  if (owner->find_cache(cache) != nullptr) { owner->invalid_ = true; return; }
  if (owner->cache_count_ == owner->caches_.size()) {
    owner->cache_overflow_ = true;
    return;
  }
  auto &entry = owner->caches_[owner->cache_count_++];
  entry = {};
  entry.id = owner->cache_count_;
  entry.address = reinterpret_cast<std::uintptr_t>(cache);
  entry.created_before_call = owner->attempted_;
}
void candidate_capture_cache_destroyed(const IvPriceSeedFloorCache *cache) noexcept {
  auto *owner = current_owner();
  if (owner == nullptr) return;
  auto *entry = owner->find_cache(cache);
  if (entry == nullptr) { owner->invalid_ = true; return; }
  entry->final_state = cache->snapshot();
  entry->closed_after_calls = owner->attempted_;
  entry->closed = true;
}
} // namespace atx::vol::research
