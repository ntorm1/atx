#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pricing/american_iv_candidate_trace.hpp"

namespace atx::vol::research {

struct IvCandidateCaptureRecord {
  std::uint64_t sequence = 0, cache_id = 0;
  IvPriceCandidateTrace trace;
};
struct IvCandidateCacheLifetime {
  std::uint64_t id = 0;
  // Diagnostic identity only; never dereferenced or retained as an owner.
  std::uintptr_t address = 0;
  std::uint64_t created_before_call = 0, closed_after_calls = 0;
  bool closed = false;
  IvPriceSeedFloorSnapshot final_state;
};

// Single-thread, one-use capture into caller-owned, disjoint storage. Storage and
// this owner must outlive its scope. Every cache observed by a candidate must be
// constructed and destroyed inside the scope. All worker activity must be joined
// before scope exit; a foreign-thread event invalidates evidence without touching
// the owner's non-atomic storage. No capture operation allocates.
class IvCandidateCapture {
public:
  IvCandidateCapture(std::span<IvCandidateCaptureRecord> records,
                     std::span<IvCandidateCacheLifetime> caches) noexcept;
  IvCandidateCapture(const IvCandidateCapture &) = delete;
  IvCandidateCapture &operator=(const IvCandidateCapture &) = delete;
  IvCandidateCapture(IvCandidateCapture &&) = delete;
  IvCandidateCapture &operator=(IvCandidateCapture &&) = delete;
  ~IvCandidateCapture() = default;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::span<const IvCandidateCaptureRecord> records() const noexcept;
  [[nodiscard]] std::span<const IvCandidateCacheLifetime> cache_lifetimes() const noexcept;
  [[nodiscard]] std::uint64_t attempted_calls() const noexcept { return attempted_; }
  [[nodiscard]] bool record_overflow() const noexcept { return record_overflow_; }
  [[nodiscard]] bool cache_overflow() const noexcept { return cache_overflow_; }

private:
  friend class IvCandidateCaptureScope;
  friend IvPriceCandidateTrace *candidate_capture_record(IvPriceSeedFloorCache *) noexcept;
  friend void candidate_capture_cache_created(const IvPriceSeedFloorCache *) noexcept;
  friend void candidate_capture_cache_destroyed(const IvPriceSeedFloorCache *) noexcept;
  [[nodiscard]] IvCandidateCacheLifetime *find_cache(const IvPriceSeedFloorCache *) noexcept;
  std::span<IvCandidateCaptureRecord> records_;
  std::span<IvCandidateCacheLifetime> caches_;
  std::size_t record_count_ = 0, cache_count_ = 0;
  std::uint64_t attempted_ = 0;
  bool active_ = false, finished_ = false, invalid_ = false;
  bool record_overflow_ = false, cache_overflow_ = false;
};

class IvCandidateCaptureScope {
public:
  explicit IvCandidateCaptureScope(IvCandidateCapture &capture) noexcept;
  ~IvCandidateCaptureScope();
  IvCandidateCaptureScope(const IvCandidateCaptureScope &) = delete;
  IvCandidateCaptureScope &operator=(const IvCandidateCaptureScope &) = delete;
  IvCandidateCaptureScope(IvCandidateCaptureScope &&) = delete;
  IvCandidateCaptureScope &operator=(IvCandidateCaptureScope &&) = delete;
  [[nodiscard]] bool attached() const noexcept { return owner_ != nullptr; }
private:
  IvCandidateCapture *owner_ = nullptr;
  std::uint64_t foreign_before_ = 0;
};

[[nodiscard]] bool iv_candidate_capture_enabled() noexcept;
// Internal hooks are called only in an explicitly enabled library build.
[[nodiscard]] IvPriceCandidateTrace *candidate_capture_record(IvPriceSeedFloorCache *) noexcept;
void candidate_capture_cache_created(const IvPriceSeedFloorCache *) noexcept;
void candidate_capture_cache_destroyed(const IvPriceSeedFloorCache *) noexcept;

} // namespace atx::vol::research
