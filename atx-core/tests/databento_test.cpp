#include "atx/external/databento.hpp"

#include "atx/core/io/parquet.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "dbn_fixture.hpp"

namespace db = atx::external::databento;
namespace io = atx::core::io;
namespace fs = std::filesystem;

namespace {

// ts in ns for 2024-01-02 and 2024-01-03 at 00:00:00 UTC.
constexpr std::uint64_t kJan02 = 1'704'153'600'000'000'000ULL;
constexpr std::uint64_t kJan03 = 1'704'240'000'000'000'000ULL;

fs::path write_zip(const std::string &name, const std::vector<std::byte> &zip) {
  const fs::path p = fs::temp_directory_path() / name;
  std::ofstream os(p, std::ios::binary);
  os.write(reinterpret_cast<const char *>(zip.data()), static_cast<std::streamsize>(zip.size()));
  return p;
}

} // namespace

TEST(Databento, LoadsZipIntoDatePartitions) {
  using atx::test::OhlcvRow;
  const std::vector<OhlcvRow> rows = {
      {101, kJan02, 1000, 1100, 990, 1050, 5000, 0x23},
      {102, kJan02, 2000, 2200, 1980, 2100, 6000, 0x23},
      {101, kJan03, 1050, 1080, 1040, 1075, 5500, 0x23},
  };
  auto dbn = atx::test::build_dbn(
      {{101, "AAPL", 20240101, 20250101}, {102, "MSFT", 20240101, 20250101}}, rows);
  auto zst = atx::test::zstd_compress(dbn);
  auto zip = atx::test::build_zip({{"equs-summary-ohlcv-1d.dbn.zst", zst}});
  const fs::path zip_path = write_zip("atx_db_load.zip", zip);
  const fs::path dest = fs::temp_directory_path() / "atx_db_dest";
  fs::remove_all(dest);

  auto stats = db::load_equs_summary_zip(zip_path.string(), dest.string());
  ASSERT_TRUE(stats.has_value()) << stats.error().to_string();
  EXPECT_EQ(stats->records_decoded, 3);
  EXPECT_EQ(stats->partitions_written, 2);
  EXPECT_EQ(stats->files_processed, 1);

  EXPECT_TRUE(fs::exists(dest / "date=2024-01-02" / "data.parquet"));
  EXPECT_TRUE(fs::exists(dest / "date=2024-01-03" / "data.parquet"));

  auto t = io::read_parquet((dest / "date=2024-01-02" / "data.parquet").string());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 2);
  auto syms = t->strings("symbol");
  ASSERT_TRUE(syms.has_value());
  EXPECT_EQ(syms->size(), 2u);
}

TEST(Databento, MissingZipErrors) {
  auto stats = db::load_equs_summary_zip("c:/no/such/file.zip", "ignored");
  ASSERT_FALSE(stats.has_value());
  EXPECT_EQ(stats.error().code(), atx::core::ErrorCode::IoError);
}

TEST(Databento, ZeroOhlcvErrors) {
  using atx::test::OhlcvRow;
  const std::vector<OhlcvRow> rows = {{1, kJan02, 0, 0, 0, 0, 0, 0x12}}; // unsupported only
  auto dbn = atx::test::build_dbn({{1, "X", 20240101, 20250101}}, rows);
  auto zst = atx::test::zstd_compress(dbn);
  auto zip = atx::test::build_zip({{"x.dbn.zst", zst}});
  const fs::path zip_path = write_zip("atx_db_zero.zip", zip);

  auto stats = db::load_equs_summary_zip(zip_path.string(),
                                         (fs::temp_directory_path() / "atx_db_zero_dest").string());
  ASSERT_FALSE(stats.has_value());
  EXPECT_EQ(stats.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Databento, SkipsUnsupportedAndCounts) {
  using atx::test::OhlcvRow;
  const std::vector<OhlcvRow> rows = {
      {1, kJan02, 10, 11, 9, 10, 100, 0x23}, {1, kJan02, 0, 0, 0, 0, 0, 0x12}, // skipped
  };
  auto dbn = atx::test::build_dbn({{1, "X", 20240101, 20250101}}, rows);
  auto zst = atx::test::zstd_compress(dbn);
  auto zip = atx::test::build_zip({{"x.dbn.zst", zst}});
  const fs::path zip_path = write_zip("atx_db_skip.zip", zip);

  auto stats = db::load_equs_summary_zip(zip_path.string(),
                                         (fs::temp_directory_path() / "atx_db_skip_dest").string());
  ASSERT_TRUE(stats.has_value()) << stats.error().to_string();
  EXPECT_EQ(stats->records_decoded, 1);
  EXPECT_EQ(stats->records_skipped, 1);
}
