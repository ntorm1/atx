#pragma once

// ATXVFA snapshot series: append without rewriting, read across segments,
// compact. A series is a directory of immutable segment files
// `seg-<sequence>.atxvfa` plus the manifest `series.atxvfa`, itself an ATXVFA
// file holding one SeriesSegments section. Spec and the segments-versus-log
// rationale: atx-vol/docs/storage/ARCHIVE_FORMAT.md section 9.
//
// Writers (append, compact) serialise across processes on `series.lock`
// (detail::WriterLock). Readers never map the manifest (it is replaced on every
// append), map the segments it lists, and refuse a segment whose size,
// whole-file CRC, header CRC or sequence differs from its manifest entry
// (SegmentMismatch). An open FitArchiveSeries is immutable and thread-safe;
// lookups are allocation-free.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/api/storage/fit_archive.hpp"

namespace atx::vol {

class FitArchiveWriter;

inline constexpr std::string_view kFitArchiveSeriesManifest{"series.atxvfa"};
inline constexpr std::string_view kFitArchiveSeriesLock{"series.lock"};

struct FitArchiveSeriesOptions {
  std::int64_t created_unix_ns{0};
  std::uint64_t archive_id{0};
  std::chrono::milliseconds lock_timeout{5000};
};

// Publish `writer`'s records as the next segment of the series in `directory`
// (created when absent), then the manifest that lists it. Returns the new
// segment's sequence. DuplicateKey when a record's key already exists in a
// segment whose snapshot range overlaps; Busy when another writer holds the
// lock past the timeout; IoError on a failed publish (the published series is
// then unchanged: an unreferenced segment file may remain).
[[nodiscard]] ArchiveResult<std::uint64_t>
append_fit_archive_segment(std::string_view directory, const FitArchiveWriter &writer,
                           const FitArchiveSeriesOptions &options = {});

struct FitArchiveCompaction {
  std::uint64_t sequence{0}; // the merged segment
  std::size_t merged{0};     // segments replaced
  std::size_t removed{0};    // old segment files deleted
  std::size_t kept{0};       // old files still present (e.g. mapped by a reader on Windows)
};

// Merge manifest entries [first, first + count) (clamped) into one new segment,
// copying records as stored (bit-exact, see FitArchiveWriter::copy_from), publish
// the manifest with the merged entry in their place, then delete the replaced
// files best-effort. InvalidArgument when fewer than two segments are selected.
[[nodiscard]] ArchiveResult<FitArchiveCompaction>
compact_fit_archive_series(std::string_view directory, std::size_t first = 0,
                           std::size_t count = std::numeric_limits<std::size_t>::max(),
                           const FitArchiveSeriesOptions &options = {});

class FitArchiveSeries {
public:
  // Read the manifest (NotFound when absent) and open every listed segment.
  [[nodiscard]] static ArchiveResult<FitArchiveSeries> open(std::string_view directory,
                                                            const FitArchiveOpenOptions &options = {});

  [[nodiscard]] std::span<const FitArchiveSeriesSegment> manifest() const noexcept { return manifest_; }
  [[nodiscard]] std::size_t segment_count() const noexcept { return segments_.size(); }
  // Precondition: i < segment_count().
  [[nodiscard]] const FitArchive &segment(std::size_t i) const noexcept { return segments_[i]; }

  template <class R> struct Hit {
    const FitArchive *archive{nullptr};
    const R *record{nullptr};
    [[nodiscard]] explicit operator bool() const noexcept { return record != nullptr; }
  };

  // Route by the manifest's snapshot ranges, then the segment index. A record
  // exists in at most one segment (append refuses duplicates).
  [[nodiscard]] Hit<FitArchiveFitRecord> find_fit(std::string_view underlier, std::int64_t snapshot_ns,
                                                  std::int32_t expiry_day,
                                                  std::uint32_t sub = 0) const noexcept;
  [[nodiscard]] Hit<FitArchiveSurfaceRecord> find_surface(std::string_view underlier,
                                                          std::int64_t snapshot_ns) const noexcept;
  [[nodiscard]] Hit<FitArchiveExpiryMeta> find_expiry(std::string_view underlier, std::int64_t snapshot_ns,
                                                      std::int32_t expiry_day,
                                                      std::uint32_t sub) const noexcept;

private:
  FitArchiveSeries() = default;
  std::vector<FitArchiveSeriesSegment> manifest_;
  std::vector<FitArchive> segments_;
};

} // namespace atx::vol
