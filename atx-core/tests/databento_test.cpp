#include "atx/external/databento.hpp"

#include "atx/core/io/parquet.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

TEST(Databento, PerFilePartialWritePersistsBeforeError) {
  using atx::test::OhlcvRow;
  // Entry 1: valid, date 2024-01-02. Entry 2: a zstd blob whose DBN magic is bad.
  const std::vector<OhlcvRow> day = {{101, kJan02, 10, 11, 9, 10, 100, 0x23}};
  auto good = atx::test::build_dbn({{101, "AAPL", 20240101, 20250101}}, day);
  auto good_zst = atx::test::zstd_compress(good);

  std::vector<std::byte> bad_dbn(64, std::byte(0));
  bad_dbn[0] = std::byte('X'); // not "DBN" -> DbnDecoder::open fails
  auto bad_zst = atx::test::zstd_compress(bad_dbn);

  auto zip = atx::test::build_zip({{"a-20240102.dbn.zst", good_zst}, {"b-bad.dbn.zst", bad_zst}});
  const fs::path zip_path = write_zip("atx_db_partial.zip", zip);
  const fs::path dest = fs::temp_directory_path() / "atx_db_partial_dest";
  fs::remove_all(dest);

  auto stats = db::load_equs_summary_zip(zip_path.string(), dest.string());
  EXPECT_FALSE(stats.has_value()); // the bad second entry aborts the load
  // ...but the first day's partition was already written (per-file streaming).
  EXPECT_TRUE(fs::exists(dest / "date=2024-01-02" / "data.parquet"));
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

namespace {
[[nodiscard]] std::string env_or_empty(const char *key) {
#if defined(_WIN32)
  char *v = nullptr;
  std::size_t n = 0;
  if (_dupenv_s(&v, &n, key) == 0 && v != nullptr) {
    std::string s(v);
    std::free(v);
    return s;
  }
  return {};
#else
  const char *v = std::getenv(key);
  return v != nullptr ? std::string(v) : std::string{};
#endif
}
} // namespace

// Env-gated smoke test against a REAL Databento EQUS.SUMMARY batch zip. Set
// ATX_EQUS_ZIP to the .zip path to enable; skipped otherwise so the suite stays
// green without the proprietary file. Validates the metadata/record byte layout
// against a real v2/v3 file: symbols must resolve to alpha tickers, not the
// numeric-id fallback.
TEST(Databento, RealEqusSummarySmoke) {
  const std::string zip = env_or_empty("ATX_EQUS_ZIP");
  if (zip.empty() || !fs::exists(fs::path{zip})) {
    GTEST_SKIP() << "set ATX_EQUS_ZIP to a real EQUS.SUMMARY .zip to run";
  }
  const fs::path dest = fs::temp_directory_path() / "atx_db_real_dest";
  fs::remove_all(dest);

  auto stats = db::load_equs_summary_zip(zip, dest.string());
  ASSERT_TRUE(stats.has_value()) << stats.error().to_string();
  std::cout << "[real] files=" << stats->files_processed << " records=" << stats->records_decoded
            << " partitions=" << stats->partitions_written << " skipped=" << stats->records_skipped
            << "\n";
  EXPECT_GT(stats->records_decoded, 0);
  EXPECT_GT(stats->partitions_written, 0);

  // Read back one partition; confirm symbols resolved (not numeric fallback).
  fs::path first;
  for (const auto &e : fs::recursive_directory_iterator(dest)) {
    if (e.path().filename() == "data.parquet") {
      first = e.path();
      break;
    }
  }
  ASSERT_FALSE(first.empty());
  auto t = io::read_parquet(first.string());
  ASSERT_TRUE(t.has_value()) << t.error().to_string();
  EXPECT_GT(t->num_rows(), 0);
  auto syms = t->strings("symbol");
  ASSERT_TRUE(syms.has_value());
  bool any_alpha = false;
  for (const auto sv : *syms) {
    for (const char ch : sv) {
      if (std::isalpha(static_cast<unsigned char>(ch)) != 0) {
        any_alpha = true;
        break;
      }
    }
    if (any_alpha) {
      break;
    }
  }
  std::cout << "[real] partition=" << first.string() << " rows=" << t->num_rows()
            << " first_symbol=" << (syms->empty() ? std::string{} : std::string(syms->front()))
            << "\n";
  EXPECT_TRUE(any_alpha) << "symbols look numeric -> metadata mapping parse may be off";
}
