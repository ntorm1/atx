#pragma once

// SurfaceTimeline (kind SurfaceTimeline, `.atxstl`): uid-major fitted-surface history.
// A period file (`YYYY-MM.atxstl` by default) holds VERBATIM ATXVSA2 records behind a
// (uid, ts) index, so one uid's history over N snapshots is one binary search plus N
// zero-copy views instead of N partition point reads. It is the substrate of
// `project_series` over history and of historical VaR (design wave3-persist D12).
//
// Records are copied byte-for-byte from ATXVSA2 partitions (or encoded once through
// `write_surface_archive_v2`: there is no second surface encoder), so a view served
// here is bit-exact to the partition's own view.
//
// LIFETIME. Archives are move-only PIMPL; views and spans borrow the archive's backing
// and die with it. `PricedSurfaceView` is move-only and `SurfaceRef` holds a raw
// pointer: `collect` reserves the view vector BEFORE taking refs.
//
// FROZEN API CONTRACT (wave 3, lane P scaffold 2026-10-24). Lane P2 implements it and
// may ADD.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"    // SurfaceRef
#include "atx/vol/api/backtest/priced_surface_view.hpp" // PricedSurfaceView
#include "atx/vol/api/persist/persist_file.hpp"

namespace atx::vol {

class SurfaceDb;

inline constexpr std::string_view kSurfaceTimelineSalt = "ATXVST01";

enum class SurfaceTimelineSection : std::uint32_t {
  Meta = 1,
  UidDirectory = 2, // uid, [entry_begin, count), ticker ref
  Strings = 3,
  Entries = 4,      // TimelineEntry, sorted by (uid, ts_ns)
  Records = 5,      // verbatim ATXVSA2 records
};

[[nodiscard]] std::uint64_t surface_timeline_fingerprint() noexcept;

// One (uid, ts) surface of a period file (48 B POD, little-endian, no padding).
struct TimelineEntry {
  std::uint32_t uid{};
  std::uint16_t n_slices{};
  std::uint16_t flags{};
  std::int64_t ts_ns{};
  std::uint64_t record_offset{};   // into the Records section
  std::uint32_t record_size{};
  std::uint32_t record_crc32c{};   // the record's payload CRC (verified on view)
  std::uint64_t source_fingerprint{};
  std::uint64_t reserved{};
};
static_assert(kPersistPod<TimelineEntry> && sizeof(TimelineEntry) == 48);
static_assert(offsetof(TimelineEntry, ts_ns) == 8);
static_assert(offsetof(TimelineEntry, record_offset) == 16);
static_assert(offsetof(TimelineEntry, record_crc32c) == 28);
static_assert(offsetof(TimelineEntry, source_fingerprint) == 32);

// One ATXVSA2 partition (one market ts) to fold into a period file.
struct TimelineSource {
  std::string archive_path;
  std::int64_t ts_ns{0};
};

[[nodiscard]] PersistResult<std::vector<std::byte>>
encode_surface_timeline(std::span<const TimelineSource> sources, std::int64_t created_unix_ns);

class SurfaceTimeline {
public:
  [[nodiscard]] static PersistResult<SurfaceTimeline> open(std::string_view path,
                                                           const PersistOpenOptions &options = {});

  SurfaceTimeline(SurfaceTimeline &&) noexcept;
  SurfaceTimeline &operator=(SurfaceTimeline &&) noexcept;
  SurfaceTimeline(const SurfaceTimeline &) = delete;
  SurfaceTimeline &operator=(const SurfaceTimeline &) = delete;
  ~SurfaceTimeline();

  // Ascending.
  [[nodiscard]] std::span<const std::uint32_t> uids() const noexcept;
  // Entries of `uid` with from_ns <= ts_ns <= to_ns, ascending by ts. Allocation-free.
  [[nodiscard]] std::span<const TimelineEntry> range(std::uint32_t uid, std::int64_t from_ns,
                                                     std::int64_t to_ns) const noexcept;
  // Verify the record CRC, then a zero-copy view over the record.
  [[nodiscard]] PersistResult<PricedSurfaceView> view(const TimelineEntry &entry) const;

private:
  struct Impl;
  explicit SurfaceTimeline(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// <root>/YYYY-MM.atxstl period files behind a PersistDirectory manifest.
class SurfaceTimelineStore {
public:
  [[nodiscard]] static PersistResult<SurfaceTimelineStore> open(std::string_view root);
  [[nodiscard]] static PersistResult<std::size_t>
  build_from_surface_db(const SurfaceDb &db, std::span<const std::string> keys,
                        std::string_view root, std::int64_t created_unix_ns);

  SurfaceTimelineStore(SurfaceTimelineStore &&) noexcept;
  SurfaceTimelineStore &operator=(SurfaceTimelineStore &&) noexcept;
  SurfaceTimelineStore(const SurfaceTimelineStore &) = delete;
  SurfaceTimelineStore &operator=(const SurfaceTimelineStore &) = delete;
  ~SurfaceTimelineStore();

  // Append the views of `uid` over [from_ns, to_ns] (ascending ts) to `views`, then
  // refs to them to `refs` and their instants to `ts`; returns how many were added.
  // `views` is reserved before any ref is taken, so earlier refs stay valid.
  [[nodiscard]] PersistResult<std::size_t> collect(std::uint32_t uid, std::int64_t from_ns,
                                                   std::int64_t to_ns,
                                                   std::vector<PricedSurfaceView> &views,
                                                   std::vector<SurfaceRef> &refs,
                                                   std::vector<std::int64_t> &ts) const;

private:
  struct Impl;
  explicit SurfaceTimelineStore(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol
