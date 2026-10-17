// ATXVFA snapshot series: append without rewriting, cross-segment lookup,
// duplicate refusal, manifest/segment mismatch, compaction, writer lock.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
