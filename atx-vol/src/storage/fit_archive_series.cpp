// ATXVFA snapshot series: segment append, manifest, cross-segment lookup and
// compaction. Spec: atx-vol/docs/storage/ARCHIVE_FORMAT.md section 9.

#include "atx/vol/api/storage/fit_archive_series.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "storage/writer_lock.hpp"

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
using Type = FitArchiveSectionType;

[[nodiscard]] std::string segment_name(std::uint64_t sequence) {
  std::string digits = std::to_string(sequence);
  if (digits.size() < 12) digits.insert(0, 12 - digits.size(), '0');
  return "seg-" + digits + ".atxvfa";
}

[[nodiscard]] std::string entry_name(const FitArchiveSeriesSegment &e) {
  const auto end = std::find(e.file_name.begin(), e.file_name.end(), '\0');
  return std::string(e.file_name.begin(), end);
}

struct Range {
  std::int64_t min{std::numeric_limits<std::int64_t>::max()};
  std::int64_t max{std::numeric_limits<std::int64_t>::min()};
  template <class R> void scan(std::span<const R> rows) noexcept {
    for (const R &r : rows) {
      min = std::min(min, r.key.snapshot_ns);
      max = std::max(max, r.key.snapshot_ns);
    }
  }
};

[[nodiscard]] FitArchiveSeriesSegment entry_of(const FitArchive &a, std::uint64_t sequence,
                                               const std::string &name) {
  FitArchiveSeriesSegment e{};
  e.sequence = sequence;
  e.file_size = a.header().file_size;
  e.file_crc32c = a.footer().file_crc32c;
  e.header_crc32c = a.header().header_crc32c;
  Range r;
  r.scan(a.surfaces());
  r.scan(a.fits());
  r.scan(a.expiry_meta());
  r.scan(a.ladders());
  r.scan(a.markov());
  e.min_snapshot_ns = r.min <= r.max ? r.min : 0;
  e.max_snapshot_ns = r.min <= r.max ? r.max : 0;
  e.surface_records = a.surfaces().size();
  e.fit_records = a.fits().size();
  e.expiry_records = a.expiry_meta().size();
  e.ladder_records = a.ladders().size();
  e.markov_records = a.markov().size();
  std::copy_n(name.begin(), std::min(name.size(), e.file_name.size() - 1), e.file_name.begin());
  return e;
}

// A bitwise OR, not a sum: the counts come from the manifest file, and a u64 sum
// of forged counts can wrap to 0 and hide a segment from lookups and from the
// append duplicate check.
[[nodiscard]] bool has_records(const FitArchiveSeriesSegment &e) noexcept {
  return (e.surface_records | e.fit_records | e.expiry_records | e.ladder_records | e.markov_records) != 0u;
}

[[nodiscard]] bool overlaps(const FitArchiveSeriesSegment &a, const FitArchiveSeriesSegment &b) noexcept {
  return has_records(a) && has_records(b) && a.min_snapshot_ns <= b.max_snapshot_ns &&
         b.min_snapshot_ns <= a.max_snapshot_ns;
}

[[nodiscard]] ArchiveResult<std::vector<FitArchiveSeriesSegment>> read_manifest(const fs::path &dir,
                                                                               bool required) {
  const fs::path path = dir / std::string(kFitArchiveSeriesManifest);
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    if (required) return archive_error(ArchiveStatus::NotFound, Type::SeriesSegments);
    return std::vector<FitArchiveSeriesSegment>{};
  }
  // Read, never map: the manifest is replaced on every append.
  auto manifest = FitArchive::open(path.string(), {FitArchiveOpenMode::Read, FitArchiveVerify::Paranoid});
  if (!manifest) return tl::unexpected(manifest.error());
  const auto segments = manifest->series_segments();
  return std::vector<FitArchiveSeriesSegment>(segments.begin(), segments.end());
}

[[nodiscard]] ArchiveResult<FitArchive> open_segment(const fs::path &dir, const FitArchiveSeriesSegment &e,
                                                     const FitArchiveOpenOptions &options) {
  // The manifest's record pass already refused a foreign name; checked again
  // here because this is where the name becomes a path.
  if (!fit_archive_segment_name_ok(e)) {
    return archive_error(ArchiveStatus::RecordInvalid, Type::SeriesSegments, e.sequence);
  }
  auto a = FitArchive::open((dir / entry_name(e)).string(), options);
  if (!a) return tl::unexpected(a.error());
  if (a->header().segment_sequence != e.sequence || a->header().file_size != e.file_size ||
      a->footer().file_crc32c != e.file_crc32c || a->header().header_crc32c != e.header_crc32c) {
    return archive_error(ArchiveStatus::SegmentMismatch, Type::SeriesSegments, e.sequence);
  }
  return a;
}

[[nodiscard]] ArchiveOutcome publish_manifest(const fs::path &dir,
                                              const std::vector<FitArchiveSeriesSegment> &entries,
                                              const FitArchiveSeriesOptions &options, std::uint64_t latest) {
  const FitArchiveSectionInput section{Type::SeriesSegments,
                                       1,
                                       0,
                                       static_cast<std::uint32_t>(sizeof(FitArchiveSeriesSegment)),
                                       8,
                                       kFitArchiveSectionRequired,
                                       std::as_bytes(std::span<const FitArchiveSeriesSegment>{entries})};
  auto image = build_fit_archive(std::span{&section, 1}, {options.created_unix_ns, options.archive_id, latest});
  if (!image) return tl::unexpected(image.error());
  return publish_fit_archive((dir / std::string(kFitArchiveSeriesManifest)).string(), *image);
}

[[nodiscard]] ArchiveResult<detail::WriterLock> lock_series(const fs::path &dir,
                                                           const FitArchiveSeriesOptions &options) {
  auto lock = detail::WriterLock::acquire((dir / std::string(kFitArchiveSeriesLock)).string(),
                                          options.lock_timeout);
  if (!lock) {
    const bool busy = lock.error().code() == atx::core::ErrorCode::Unavailable;
    return archive_error(busy ? ArchiveStatus::Busy : ArchiveStatus::IoError, Type::SeriesSegments);
  }
  return std::move(*lock);
}

[[nodiscard]] std::uint64_t next_sequence(const std::vector<FitArchiveSeriesSegment> &entries) noexcept {
  std::uint64_t latest = 0;
  for (const FitArchiveSeriesSegment &e : entries) latest = std::max(latest, e.sequence);
  return latest + 1u;
}

// Refuse any record of `fresh` whose key already exists in `old`.
template <class R, class Find>
[[nodiscard]] ArchiveOutcome refuse_duplicates(const FitArchive &fresh, std::span<const R> rows,
                                               const FitArchive &old, Type type, Find find) noexcept {
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto id = old.find_underlier(fresh.underlier_name(rows[i].key.underlier));
    if (!id) continue;
    FitArchiveKey key = rows[i].key;
    key.underlier = *id;
    if (find(old, key) != nullptr) return archive_error(ArchiveStatus::DuplicateKey, type, i);
  }
  return {};
}

[[nodiscard]] ArchiveOutcome refuse_all_duplicates(const FitArchive &fresh, const FitArchive &old) noexcept {
  const ArchiveOutcome checks[] = {
      refuse_duplicates(fresh, fresh.surfaces(), old, Type::SurfaceRecords,
                        [](const FitArchive &a, const FitArchiveKey &k) { return a.find_surface(k.underlier, k.snapshot_ns); }),
      refuse_duplicates(fresh, fresh.fits(), old, Type::FitRecords,
                        [](const FitArchive &a, const FitArchiveKey &k) { return a.find_fit(k); }),
      refuse_duplicates(fresh, fresh.expiry_meta(), old, Type::ExpiryMeta,
                        [](const FitArchive &a, const FitArchiveKey &k) { return a.find_expiry(k); }),
      refuse_duplicates(fresh, fresh.ladders(), old, Type::Ladders,
                        [](const FitArchive &a, const FitArchiveKey &k) { return a.find_ladder(k); }),
      refuse_duplicates(fresh, fresh.markov(), old, Type::MarkovRecords,
                        [](const FitArchive &a, const FitArchiveKey &k) { return a.find_markov(k); }),
  };
  for (const ArchiveOutcome &ok : checks) {
    if (!ok) return ok;
  }
  return {};
}

} // namespace

bool fit_archive_segment_name_ok(const FitArchiveSeriesSegment &segment) noexcept {
  constexpr std::string_view kPrefix{"seg-"};
  constexpr std::string_view kSuffix{".atxvfa"};
  constexpr std::size_t kMinDigits = 12;
  std::array<char, 24> digits{};
  const auto [digits_end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), segment.sequence);
  if (ec != std::errc{}) return false;
  const auto n = static_cast<std::size_t>(digits_end - digits.data());
  const std::size_t pad = n < kMinDigits ? kMinDigits - n : 0;
  const auto &name = segment.file_name;
  const std::size_t length = kPrefix.size() + pad + n + kSuffix.size();
  if (length >= name.size()) return false; // at least one NUL must follow
  std::size_t at = 0;
  const auto take = [&](std::string_view part) noexcept {
    const bool same = std::equal(part.begin(), part.end(), name.begin() + static_cast<std::ptrdiff_t>(at));
    at += part.size();
    return same;
  };
  if (!take(kPrefix)) return false;
  for (std::size_t k = 0; k < pad; ++k) {
    if (name[at++] != '0') return false;
  }
  return take(std::string_view{digits.data(), n}) && take(kSuffix) &&
         std::all_of(name.begin() + static_cast<std::ptrdiff_t>(at), name.end(), [](char c) { return c == '\0'; });
}

ArchiveResult<std::uint64_t> append_fit_archive_segment(std::string_view directory,
                                                        const FitArchiveWriter &writer,
                                                        const FitArchiveSeriesOptions &options) {
  const fs::path dir{std::string(directory)};
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return archive_error(ArchiveStatus::IoError, Type::SeriesSegments);
  auto lock = lock_series(dir, options);
  if (!lock) return tl::unexpected(lock.error());
  auto entries = read_manifest(dir, false);
  if (!entries) return tl::unexpected(entries.error());
  const std::uint64_t sequence = next_sequence(*entries);
  auto image = writer.finish({options.created_unix_ns, options.archive_id, sequence});
  if (!image) return tl::unexpected(image.error());
  auto fresh = FitArchive::open_bytes(*image, nullptr);
  if (!fresh) return tl::unexpected(fresh.error());
  const std::string name = segment_name(sequence);
  const FitArchiveSeriesSegment entry = entry_of(*fresh, sequence, name);
  for (const FitArchiveSeriesSegment &old_entry : *entries) {
    if (!overlaps(old_entry, entry)) continue;
    auto old = open_segment(dir, old_entry, {FitArchiveOpenMode::Mapped, FitArchiveVerify::Framing});
    if (!old) return tl::unexpected(old.error());
    if (ArchiveOutcome ok = refuse_all_duplicates(*fresh, *old); !ok) return tl::unexpected(ok.error());
  }
  if (ArchiveOutcome ok = publish_fit_archive((dir / name).string(), *image); !ok) {
    return tl::unexpected(ok.error());
  }
  entries->push_back(entry);
  if (ArchiveOutcome ok = publish_manifest(dir, *entries, options, sequence); !ok) {
    return tl::unexpected(ok.error());
  }
  return sequence;
}

ArchiveResult<FitArchiveCompaction> compact_fit_archive_series(std::string_view directory, std::size_t first,
                                                              std::size_t count,
                                                              const FitArchiveSeriesOptions &options) {
  const fs::path dir{std::string(directory)};
  auto lock = lock_series(dir, options);
  if (!lock) return tl::unexpected(lock.error());
  auto entries = read_manifest(dir, true);
  if (!entries) return tl::unexpected(entries.error());
  const std::size_t begin = std::min(first, entries->size());
  const std::size_t end = begin + std::min(count, entries->size() - begin);
  if (end - begin < 2) return archive_error(ArchiveStatus::InvalidArgument, Type::SeriesSegments);
  FitArchiveWriter writer;
  for (std::size_t i = begin; i < end; ++i) {
    // Read mode: the file is closed once copied, so it can be deleted below.
    auto segment = open_segment(dir, (*entries)[i], {FitArchiveOpenMode::Read, FitArchiveVerify::Full});
    if (!segment) return tl::unexpected(segment.error());
    if (ArchiveOutcome ok = writer.copy_from(*segment); !ok) return tl::unexpected(ok.error());
  }
  FitArchiveCompaction out;
  out.sequence = next_sequence(*entries);
  out.merged = end - begin;
  auto image = writer.finish({options.created_unix_ns, options.archive_id, out.sequence});
  if (!image) return tl::unexpected(image.error());
  auto fresh = FitArchive::open_bytes(*image, nullptr);
  if (!fresh) return tl::unexpected(fresh.error());
  const std::string name = segment_name(out.sequence);
  if (ArchiveOutcome ok = publish_fit_archive((dir / name).string(), *image); !ok) {
    return tl::unexpected(ok.error());
  }
  std::vector<FitArchiveSeriesSegment> next(entries->begin(), entries->begin() + static_cast<std::ptrdiff_t>(begin));
  next.push_back(entry_of(*fresh, out.sequence, name));
  next.insert(next.end(), entries->begin() + static_cast<std::ptrdiff_t>(end), entries->end());
  if (ArchiveOutcome ok = publish_manifest(dir, next, options, out.sequence); !ok) {
    return tl::unexpected(ok.error());
  }
  for (std::size_t i = begin; i < end; ++i) {
    std::error_code ec;
    if (fit_archive_segment_name_ok((*entries)[i]) && fs::remove(dir / entry_name((*entries)[i]), ec) && !ec) {
      ++out.removed;
    } else {
      ++out.kept;
    }
  }
  return out;
}

ArchiveResult<FitArchiveSeries> FitArchiveSeries::open(std::string_view directory,
                                                       const FitArchiveOpenOptions &options) {
  const fs::path dir{std::string(directory)};
  auto entries = read_manifest(dir, true);
  if (!entries) return tl::unexpected(entries.error());
  FitArchiveSeries series;
  series.segments_.reserve(entries->size());
  for (const FitArchiveSeriesSegment &e : *entries) {
    auto segment = open_segment(dir, e, options);
    if (!segment) return tl::unexpected(segment.error());
    series.segments_.push_back(std::move(*segment));
  }
  series.manifest_ = std::move(*entries);
  return series;
}

namespace {

[[nodiscard]] bool covers(const FitArchiveSeriesSegment &e, std::int64_t snapshot_ns) noexcept {
  return has_records(e) && e.min_snapshot_ns <= snapshot_ns && snapshot_ns <= e.max_snapshot_ns;
}

} // namespace

FitArchiveSeries::Hit<FitArchiveFitRecord>
FitArchiveSeries::find_fit(std::string_view underlier, std::int64_t snapshot_ns, std::int32_t expiry_day,
                           std::uint32_t sub) const noexcept {
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    if (!covers(manifest_[i], snapshot_ns)) continue;
    const auto id = segments_[i].find_underlier(underlier);
    if (!id) continue;
    const FitArchiveKey key{*id, expiry_day, snapshot_ns, sub, 0};
    if (const auto *r = segments_[i].find_fit(key)) return {&segments_[i], r};
  }
  return {};
}

FitArchiveSeries::Hit<FitArchiveSurfaceRecord>
FitArchiveSeries::find_surface(std::string_view underlier, std::int64_t snapshot_ns) const noexcept {
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    if (!covers(manifest_[i], snapshot_ns)) continue;
    const auto id = segments_[i].find_underlier(underlier);
    if (!id) continue;
    if (const auto *r = segments_[i].find_surface(*id, snapshot_ns)) return {&segments_[i], r};
  }
  return {};
}

FitArchiveSeries::Hit<FitArchiveExpiryMeta>
FitArchiveSeries::find_expiry(std::string_view underlier, std::int64_t snapshot_ns, std::int32_t expiry_day,
                              std::uint32_t sub) const noexcept {
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    if (!covers(manifest_[i], snapshot_ns)) continue;
    const auto id = segments_[i].find_underlier(underlier);
    if (!id) continue;
    const FitArchiveKey key{*id, expiry_day, snapshot_ns, sub, 0};
    if (const auto *r = segments_[i].find_expiry(key)) return {&segments_[i], r};
  }
  return {};
}

} // namespace atx::vol
