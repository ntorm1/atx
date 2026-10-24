// ATXVFA snapshot series: append without rewriting, cross-segment lookup,
// duplicate refusal, manifest/segment mismatch, compaction, writer lock, and
// manifests that try to name a file outside the series.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "atx/vol/api/storage/fit_archive_series.hpp"
#include "storage/writer_lock.hpp"
#include "support/fit_archive_fixture.hpp"

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
using testkit::diff_slice;

constexpr std::int64_t kSnapshot = 1791988200000000000LL;
constexpr std::int64_t kStep = 3600LL * 1000000000LL;

const testkit::SyntheticSurface &surface() {
  static const testkit::SyntheticSurface s = testkit::synthetic_surface();
  return s;
}

FitArchiveWriter snapshot_writer(std::int64_t snapshot_ns) {
  FitArchiveWriter w;
  EXPECT_TRUE(w.add_surface({"AAPL", snapshot_ns, surface().slices, surface().expiry_days, surface().config, {},
                             "ordinary", "raw-liquidity"})
                  .has_value());
  EXPECT_TRUE(w.add_fit({"MSFT", snapshot_ns, 20833, 0, &surface().slices[1].fit}).has_value());
  return w;
}

std::vector<char> slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void expect_series_serves(const FitArchiveSeries &series, std::size_t snapshots) {
  for (std::size_t s = 0; s < snapshots; ++s) {
    const std::int64_t snap = kSnapshot + static_cast<std::int64_t>(s) * kStep;
    const auto surf = series.find_surface("AAPL", snap);
    ASSERT_TRUE(surf) << "snapshot " << s;
    auto decoded = decode_surface(*surf.archive, *surf.record);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), surface().slices.size());
    for (std::size_t i = 0; i < decoded->size(); ++i) EXPECT_EQ(diff_slice((*decoded)[i], surface().slices[i]), "");
    const auto fit = series.find_fit("MSFT", snap, 20833);
    ASSERT_TRUE(fit);
    auto msft = decode_fit(*fit.archive, *fit.record);
    ASSERT_TRUE(msft.has_value());
    testkit::Diff d;
    testkit::diff_fit(d, *msft, surface().slices[1].fit);
    EXPECT_EQ(d.first(), "");
  }
  EXPECT_FALSE(series.find_fit("MSFT", kSnapshot + 99 * kStep, 20833));
  EXPECT_FALSE(series.find_fit("NFLX", kSnapshot, 20833));
}

TEST(FitArchiveSeries, AppendNeverRewritesEarlierSegmentsAndRoutesLookups) {
  const auto dir = testkit::scratch_dir("series");
  std::vector<char> first_segment;
  for (std::size_t s = 0; s < 3; ++s) {
    const auto w = snapshot_writer(kSnapshot + static_cast<std::int64_t>(s) * kStep);
    auto seq = append_fit_archive_segment(dir.string(), w, {100 + static_cast<std::int64_t>(s), 77});
    ASSERT_TRUE(seq.has_value()) << to_string(seq.error().status);
    EXPECT_EQ(*seq, s + 1);
    if (s == 0) first_segment = slurp(dir / "seg-000000000001.atxvfa");
  }
  EXPECT_EQ(slurp(dir / "seg-000000000001.atxvfa"), first_segment);
  EXPECT_TRUE(fs::exists(dir / std::string(kFitArchiveSeriesManifest)));
  EXPECT_FALSE(fs::exists(dir / std::string(kFitArchiveSeriesLock))) << "lock released";
  auto series = FitArchiveSeries::open(dir.string());
  ASSERT_TRUE(series.has_value()) << to_string(series.error().status);
  ASSERT_EQ(series->segment_count(), 3u);
  for (std::size_t s = 0; s < 3; ++s) {
    EXPECT_EQ(series->manifest()[s].sequence, s + 1);
    EXPECT_EQ(series->segment(s).header().segment_sequence, s + 1);
    EXPECT_EQ(series->segment(s).header().archive_id, 77u);
    EXPECT_EQ(series->manifest()[s].fit_records, 4u);
  }
  expect_series_serves(*series, 3);
}

TEST(FitArchiveSeries, AppendRefusesKeysAlreadyInTheSeries) {
  const auto dir = testkit::scratch_dir("dup");
  ASSERT_TRUE(append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot)).has_value());
  auto again = append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot));
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().status, ArchiveStatus::DuplicateKey);
  auto series = FitArchiveSeries::open(dir.string());
  ASSERT_TRUE(series.has_value());
  EXPECT_EQ(series->segment_count(), 1u);
  EXPECT_FALSE(fs::exists(dir / "seg-000000000002.atxvfa"));
}

TEST(FitArchiveSeries, SegmentThatDiffersFromItsManifestEntryIsRefused) {
  const auto dir = testkit::scratch_dir("mismatch");
  ASSERT_TRUE(append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot)).has_value());
  ASSERT_TRUE(append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot + kStep)).has_value());
  // A valid archive, same sequence, different content.
  FitArchiveWriter impostor({0, 0, 1});
  ASSERT_TRUE(impostor.write_file((dir / "seg-000000000001.atxvfa").string()).has_value());
  auto series = FitArchiveSeries::open(dir.string());
  ASSERT_FALSE(series.has_value());
  EXPECT_EQ(series.error().status, ArchiveStatus::SegmentMismatch);
  fs::remove(dir / "seg-000000000002.atxvfa");
  auto missing = FitArchiveSeries::open(dir.string());
  ASSERT_FALSE(missing.has_value());
}

TEST(FitArchiveSeries, CompactionMergesBitExactlyAndDeletesReplacedSegments) {
  const auto dir = testkit::scratch_dir("compact");
  for (std::size_t s = 0; s < 3; ++s) {
    ASSERT_TRUE(append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot + static_cast<std::int64_t>(s) * kStep))
                    .has_value());
  }
  auto done = compact_fit_archive_series(dir.string());
  ASSERT_TRUE(done.has_value()) << to_string(done.error().status);
  EXPECT_EQ(done->sequence, 4u);
  EXPECT_EQ(done->merged, 3u);
  EXPECT_EQ(done->removed, 3u);
  EXPECT_EQ(done->kept, 0u);
  EXPECT_FALSE(fs::exists(dir / "seg-000000000001.atxvfa"));
  auto series = FitArchiveSeries::open(dir.string());
  ASSERT_TRUE(series.has_value()) << to_string(series.error().status);
  ASSERT_EQ(series->segment_count(), 1u);
  EXPECT_EQ(series->manifest()[0].sequence, 4u);
  EXPECT_EQ(series->manifest()[0].fit_records, 12u);
  expect_series_serves(*series, 3);
  // Appending after compaction continues the sequence.
  auto next = append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot + 3 * kStep));
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, 5u);
  auto single = compact_fit_archive_series(dir.string(), 1, 1);
  ASSERT_FALSE(single.has_value());
  EXPECT_EQ(single.error().status, ArchiveStatus::InvalidArgument);
}

// ── Segment names (review MINOR-1: a manifest must never reach outside its folder) ──

// A two-segment series plus, beside it, an exact copy of its first segment: the
// file a manifest entry named "..\seg-000000000001.atxvfa" would reach. The copy
// matches entry 0's sequence, size and CRCs, so only the name check can keep a
// reader, or compaction's delete, away from it.
struct EscapeFixture {
  fs::path series;
  fs::path manifest;
  fs::path outside;
  std::vector<std::byte> outside_bytes;
  std::vector<std::byte> manifest_bytes;
};

EscapeFixture escape_fixture(std::string_view tag) {
  EscapeFixture f;
  const fs::path root = testkit::scratch_dir(tag);
  f.series = root / "series";
  f.manifest = f.series / std::string(kFitArchiveSeriesManifest);
  f.outside = root / "seg-000000000001.atxvfa";
  EXPECT_TRUE(append_fit_archive_segment(f.series.string(), snapshot_writer(kSnapshot)).has_value());
  EXPECT_TRUE(append_fit_archive_segment(f.series.string(), snapshot_writer(kSnapshot + kStep)).has_value());
  fs::copy_file(f.series / "seg-000000000001.atxvfa", f.outside);
  f.outside_bytes = testkit::read_file(f.outside);
  f.manifest_bytes = testkit::read_file(f.manifest);
  EXPECT_FALSE(f.outside_bytes.empty());
  return f;
}

// Put the pristine manifest back; each case forges from it.
void restore_manifest(const EscapeFixture &f) {
  ASSERT_TRUE(publish_fit_archive(f.manifest.string(), f.manifest_bytes).has_value());
}

TEST(FitArchiveSeries, SegmentNameRuleAcceptsOnlyTheWritersOwnName) {
  const auto entry = [](std::uint64_t sequence, std::string_view name) {
    FitArchiveSeriesSegment e{};
    e.sequence = sequence;
    std::copy_n(name.begin(), std::min(name.size(), e.file_name.size()), e.file_name.begin());
    return e;
  };
  EXPECT_TRUE(fit_archive_segment_name_ok(entry(1, "seg-000000000001.atxvfa")));
  EXPECT_TRUE(fit_archive_segment_name_ok(entry(1000000000000ull, "seg-1000000000000.atxvfa")));
  EXPECT_TRUE(fit_archive_segment_name_ok(entry(~0ull, "seg-18446744073709551615.atxvfa")));
  EXPECT_FALSE(fit_archive_segment_name_ok(entry(1, "seg-0000000000001.atxvfa"))) << "13 digits for 1";
  EXPECT_FALSE(fit_archive_segment_name_ok(entry(1000000000000ull, "seg-01000000000000.atxvfa")));
  EXPECT_FALSE(fit_archive_segment_name_ok(entry(1, "SEG-000000000001.atxvfa")));
  EXPECT_FALSE(fit_archive_segment_name_ok(entry(1, "seg-000000000001.atxvfa.")));
  EXPECT_FALSE(fit_archive_segment_name_ok(entry(1, "")));
  EXPECT_FALSE(fit_archive_segment_name_ok(entry(1, std::string(40, 'x')))) << "no NUL terminator";
  for (const std::string &name : testkit::foreign_segment_names()) {
    EXPECT_FALSE(fit_archive_segment_name_ok(entry(1, name))) << testkit::printable(name);
  }
}

// Check point 1 of 3, the manifest's record pass: an entry may only carry the
// writer's own name for its sequence. Anything else is RecordInvalid at every
// verify level, before any segment path is formed.
TEST(FitArchiveSeries, ForeignSegmentNameIsRefusedWhenTheManifestOpens) {
  const auto f = escape_fixture("name-manifest");
  for (const std::string &name : testkit::foreign_segment_names()) {
    restore_manifest(f);
    testkit::forge_segment_name(f.series, 0, name);
    for (const auto verify : {FitArchiveVerify::Framing, FitArchiveVerify::Full, FitArchiveVerify::Paranoid}) {
      auto opened = FitArchive::open(f.manifest.string(), {FitArchiveOpenMode::Read, verify});
      ASSERT_FALSE(opened.has_value()) << testkit::printable(name) << " verify " << static_cast<int>(verify);
      EXPECT_EQ(opened.error().status, ArchiveStatus::RecordInvalid) << testkit::printable(name);
      EXPECT_EQ(opened.error().section, static_cast<std::uint32_t>(FitArchiveSectionType::SeriesSegments));
      EXPECT_EQ(opened.error().detail, 0u);
    }
  }
  // Control: the forge itself is harmless when it writes the writer's own name.
  restore_manifest(f);
  testkit::forge_segment_name(f.series, 0, "seg-000000000001.atxvfa");
  EXPECT_TRUE(
      FitArchive::open(f.manifest.string(), {FitArchiveOpenMode::Read, FitArchiveVerify::Paranoid}).has_value());
  EXPECT_EQ(testkit::read_file(f.outside), f.outside_bytes);
}

// Check point 2 of 3, where a name becomes a path: FitArchiveSeries::open (also
// behind CLI inspect/verify) never follows a foreign name, in either open mode.
TEST(FitArchiveSeries, SeriesOpenNeverFollowsAForeignSegmentName) {
  const auto f = escape_fixture("name-open");
  for (const std::string &name : testkit::foreign_segment_names()) {
    restore_manifest(f);
    testkit::forge_segment_name(f.series, 0, name);
    for (const auto mode : {FitArchiveOpenMode::Mapped, FitArchiveOpenMode::Read}) {
      auto series = FitArchiveSeries::open(f.series.string(), {mode, FitArchiveVerify::Paranoid});
      ASSERT_FALSE(series.has_value()) << testkit::printable(name);
      EXPECT_EQ(series.error().status, ArchiveStatus::RecordInvalid)
          << testkit::printable(name) << " got " << to_string(series.error().status);
      EXPECT_EQ(series.error().section, static_cast<std::uint32_t>(FitArchiveSectionType::SeriesSegments));
    }
  }
  EXPECT_EQ(testkit::read_file(f.outside), f.outside_bytes);
  restore_manifest(f);
  auto control = FitArchiveSeries::open(f.series.string());
  ASSERT_TRUE(control.has_value());
  EXPECT_EQ(control->segment_count(), 2u);
}

// Check point 3 of 3 and the end-to-end property: compaction neither opens nor
// deletes a file outside the series. The sentinel beside the series is a valid
// copy of segment 1, so following the name would copy it and then delete it.
TEST(FitArchiveSeries, CompactionNeverOpensOrDeletesAFileOutsideTheSeries) {
  for (const std::string escape : {"..\\", "../"}) {
    const auto f = escape_fixture(escape == "../" ? "compact-slash" : "compact-backslash");
    testkit::forge_segment_name(f.series, 0, escape + "seg-000000000001.atxvfa");
    const auto forged_manifest = testkit::read_file(f.manifest);
    auto done = compact_fit_archive_series(f.series.string());
    // The sentinel is neither deleted nor rewritten...
    EXPECT_TRUE(fs::exists(f.outside)) << escape << " sentinel deleted";
    EXPECT_EQ(testkit::read_file(f.outside), f.outside_bytes) << escape;
    // ...because compaction refused before opening any segment...
    EXPECT_FALSE(done.has_value()) << escape << " compaction followed the name";
    if (!done) {
      EXPECT_EQ(done.error().status, ArchiveStatus::RecordInvalid) << escape;
      EXPECT_EQ(done.error().section, static_cast<std::uint32_t>(FitArchiveSectionType::SeriesSegments));
    }
    // ...and left the series as it was: same manifest, both segments, no merge.
    EXPECT_EQ(testkit::read_file(f.manifest), forged_manifest) << escape;
    EXPECT_TRUE(fs::exists(f.series / "seg-000000000001.atxvfa")) << escape;
    EXPECT_TRUE(fs::exists(f.series / "seg-000000000002.atxvfa")) << escape;
    EXPECT_FALSE(fs::exists(f.series / "seg-000000000003.atxvfa")) << escape;
  }
}

// Manifest record counts come from the file: forged counts whose u64 sum wraps
// to 0 must not make a segment look empty (it would vanish from lookups and
// from the append duplicate check).
TEST(FitArchiveSeries, RecordCountsWhoseSumWrapsStillRouteLookups) {
  const auto dir = testkit::scratch_dir("counts");
  ASSERT_TRUE(append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot)).has_value());
  testkit::forge_segment(dir, 0, [](FitArchiveSeriesSegment &e) {
    e.surface_records = 0x8000000000000000ull;
    e.fit_records = 0x8000000000000000ull;
  });
  auto series = FitArchiveSeries::open(dir.string());
  ASSERT_TRUE(series.has_value()) << to_string(series.error().status);
  ASSERT_EQ(series->manifest()[0].fit_records, 0x8000000000000000ull);
  EXPECT_TRUE(series->find_surface("AAPL", kSnapshot));
  EXPECT_TRUE(series->find_fit("MSFT", kSnapshot, 20833));
  auto again = append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot));
  ASSERT_FALSE(again.has_value()) << "an append of the same keys must still be a duplicate";
  EXPECT_EQ(again.error().status, ArchiveStatus::DuplicateKey);
}

TEST(FitArchiveSeries, HeldLockMakesWritersBusyAndMissingSeriesIsNotFound) {
  const auto dir = testkit::scratch_dir("lock");
  auto missing = FitArchiveSeries::open(dir.string());
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().status, ArchiveStatus::NotFound);
  auto held = detail::WriterLock::acquire((dir / std::string(kFitArchiveSeriesLock)).string());
  ASSERT_TRUE(held.has_value());
  FitArchiveSeriesOptions options;
  options.lock_timeout = std::chrono::milliseconds{50};
  auto busy = append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot), options);
  ASSERT_FALSE(busy.has_value());
  EXPECT_EQ(busy.error().status, ArchiveStatus::Busy);
  held->release();
  EXPECT_TRUE(append_fit_archive_segment(dir.string(), snapshot_writer(kSnapshot), options).has_value());
}

} // namespace
} // namespace atx::vol
