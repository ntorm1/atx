// atx-vol-surface-archive CLI: inspect / dump / verify exit codes and output on
// good, corrupt, truncated and missing archives and on a series directory.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "atx/vol/api/storage/fit_archive_series.hpp"
#include "support/fit_archive_fixture.hpp"

#ifndef ATX_VOL_SURFACE_ARCHIVE_CLI
#error "ATX_VOL_SURFACE_ARCHIVE_CLI must name the atx-vol-surface-archive executable"
#endif

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
constexpr std::int64_t kSnapshot = 1791988200000000000LL;

struct CliRun {
  int code{-1};
  std::string out;
};

// Paths under the build tree and the temp directory carry no spaces, so no
// cmd.exe quoting is needed.
CliRun run_cli(const fs::path &scratch, const std::string &args) {
  const fs::path exe = fs::path(ATX_VOL_SURFACE_ARCHIVE_CLI).make_preferred();
  const fs::path out = scratch / "cli-output.txt";
  const std::string command = exe.string() + " " + args + " > " + out.string() + " 2>&1";
  CliRun r;
  r.code = std::system(command.c_str());
  std::ifstream in(out, std::ios::binary);
  r.out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return r;
}

FitArchiveWriter sample_writer(std::int64_t snapshot) {
  static const auto surface = testkit::synthetic_surface();
  FitArchiveWriter w;
  EXPECT_TRUE(w.add_surface({"AAPL", snapshot, surface.slices, surface.expiry_days, surface.config, {},
                             "ordinary", "raw-liquidity"})
                  .has_value());
  EXPECT_TRUE(w.add_fit({"MSFT", snapshot, 20833, 0, &surface.slices[1].fit}).has_value());
  return w;
}

TEST(FitArchiveCli, InspectDumpVerifyAGoodArchive) {
  const auto dir = testkit::scratch_dir("cli-good");
  const auto file = (dir / "good.atxvfa").string();
  ASSERT_TRUE(sample_writer(kSnapshot).write_file(file).has_value());
  const CliRun inspect = run_cli(dir, "inspect " + file);
  EXPECT_EQ(inspect.code, 0) << inspect.out;
  EXPECT_NE(inspect.out.find("format: ATXVFA 1.0"), std::string::npos) << inspect.out;
  EXPECT_NE(inspect.out.find("fits=4"), std::string::npos) << inspect.out;
  EXPECT_NE(inspect.out.find("fit-records"), std::string::npos) << inspect.out;
  const CliRun dump = run_cli(dir, "dump " + file);
  EXPECT_EQ(dump.code, 0) << dump.out;
  EXPECT_NE(dump.out.find("surface\tAAPL"), std::string::npos) << dump.out;
  EXPECT_NE(dump.out.find("fit\tMSFT"), std::string::npos) << dump.out;
  const CliRun only = run_cli(dir, "dump " + file + " --underlier MSFT");
  EXPECT_EQ(only.code, 0);
  EXPECT_EQ(only.out.find("fit\tAAPL"), std::string::npos) << only.out;
  EXPECT_NE(only.out.find("fit\tMSFT"), std::string::npos) << only.out;
  const CliRun verify = run_cli(dir, "verify " + file);
  EXPECT_EQ(verify.code, 0) << verify.out;
  EXPECT_NE(verify.out.find(": OK"), std::string::npos) << verify.out;
}

TEST(FitArchiveCli, VerifyRefusesAndLocalisesDamage) {
  const auto dir = testkit::scratch_dir("cli-bad");
  const auto good = dir / "good.atxvfa";
  ASSERT_TRUE(sample_writer(kSnapshot).write_file(good.string()).has_value());
  std::vector<std::byte> image;
  {
    auto a = FitArchive::open(good.string(), {FitArchiveOpenMode::Read, FitArchiveVerify::Full});
    ASSERT_TRUE(a.has_value());
    image.assign(a->bytes().begin(), a->bytes().end());
    const auto *strings = a->section(FitArchiveSectionType::Strings);
    ASSERT_NE(strings, nullptr);
    image[strings->offset] ^= std::byte{0x20};
  }
  testkit::reseal(image);
  const auto bad = dir / "bad.atxvfa";
  ASSERT_TRUE(publish_fit_archive(bad.string(), image).has_value());
  const CliRun damaged = run_cli(dir, "verify " + bad.string());
  EXPECT_EQ(damaged.code, 2) << damaged.out;
  EXPECT_NE(damaged.out.find("REFUSED section-checksum"), std::string::npos) << damaged.out;
  EXPECT_NE(damaged.out.find("damaged section 1 strings"), std::string::npos) << damaged.out;

  image.resize(image.size() / 2);
  const auto cut = dir / "cut.atxvfa";
  ASSERT_TRUE(publish_fit_archive(cut.string(), image).has_value());
  const CliRun truncated = run_cli(dir, "verify " + cut.string());
  EXPECT_EQ(truncated.code, 2) << truncated.out;
  EXPECT_NE(truncated.out.find("REFUSED truncated"), std::string::npos) << truncated.out;

  const CliRun missing = run_cli(dir, "inspect " + (dir / "absent.atxvfa").string());
  EXPECT_EQ(missing.code, 2) << missing.out;
  EXPECT_NE(missing.out.find("REFUSED not-found"), std::string::npos) << missing.out;
  EXPECT_EQ(run_cli(dir, "frobnicate " + good.string()).code, 1);
}

TEST(FitArchiveCli, InspectsAndVerifiesASeriesDirectory) {
  const auto dir = testkit::scratch_dir("cli-series");
  const auto series = dir / "series";
  ASSERT_TRUE(append_fit_archive_segment(series.string(), sample_writer(kSnapshot)).has_value());
  ASSERT_TRUE(append_fit_archive_segment(series.string(), sample_writer(kSnapshot + 1)).has_value());
  const CliRun inspect = run_cli(dir, "inspect " + series.string());
  EXPECT_EQ(inspect.code, 0) << inspect.out;
  EXPECT_NE(inspect.out.find("segments: 2"), std::string::npos) << inspect.out;
  const CliRun verify = run_cli(dir, "verify " + series.string());
  EXPECT_EQ(verify.code, 0) << verify.out;
}

} // namespace
} // namespace atx::vol
