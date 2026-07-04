#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/data/disk.hpp"

namespace fs = std::filesystem;
using atx::engine::data::DiskStore;
using atx::engine::data::Store;

namespace {
fs::path unique_tmp() {
  static int counter = 0;
  return fs::temp_directory_path() / ("atx_disk_test_" + std::to_string(++counter));
}
} // namespace

TEST(DiskStore, OpenCreatesStoreDirs) {
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value()) << st.error().message();
  EXPECT_TRUE(fs::exists(root / "OHLC1D"));
  EXPECT_TRUE(fs::exists(root / "OHLC1M"));
  EXPECT_TRUE(fs::exists(root / "OPRA_BBO"));
  EXPECT_TRUE(fs::exists(root / "OCHAIN"));
  fs::remove_all(root);
}

TEST(DiskStore, PartitionPathFormat) {
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value());
  const fs::path p = st->partition_path(Store::Ohlc1M, "2026-06-05");
  EXPECT_EQ(p, root / "OHLC1M" / "date=2026-06-05" / "data.parquet");
  fs::remove_all(root);
}

TEST(DiskStore, WriteThenScanPartitionRoundTrip) {
  using atx::i64;
  namespace io = atx::core::io;
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value());

  const std::vector<std::string> sym{"AAA", "BBB"};
  const std::vector<i64> bid{100, 200};
  const std::vector<i64> ask{101, 202};
  const std::vector<io::WriteColumn> cols{
      {"symbol", std::span<const std::string>(sym)},
      {"bid_px", std::span<const i64>(bid)},
      {"ask_px", std::span<const i64>(ask)},
  };
  auto w = st->write_partition(Store::Ohlc1M, "2026-06-05", cols);
  ASSERT_TRUE(w.has_value()) << w.error().message();
  EXPECT_TRUE(fs::exists(st->partition_path(Store::Ohlc1M, "2026-06-05")));

  // Scoped so the LazyParquet/ParquetTable (and their open Arrow file handle)
  // are destroyed before remove_all — Windows cannot delete an open file.
  {
    auto lazy = st->scan_partition(Store::Ohlc1M, "2026-06-05");
    ASSERT_TRUE(lazy.has_value()) << lazy.error().message();
    auto tbl = lazy->collect();
    ASSERT_TRUE(tbl.has_value()) << tbl.error().message();
    EXPECT_EQ(tbl->num_rows(), 2);
    auto bids = tbl->column_view<i64>("bid_px");
    ASSERT_TRUE(bids.has_value());
    EXPECT_EQ((*bids)[0], 100);
    EXPECT_EQ((*bids)[1], 200);
  }
  fs::remove_all(root);
}

TEST(DiskStore, ListDatesSortedAscending) {
  using atx::i64;
  namespace io = atx::core::io;
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value());
  const std::vector<std::string> sym{"AAA"};
  const std::vector<i64> v{1};
  const std::vector<io::WriteColumn> cols{
      {"symbol", std::span<const std::string>(sym)}, {"x", std::span<const i64>(v)}};
  for (const char* d : {"2026-06-05", "2026-06-03", "2026-06-04"}) {
    ASSERT_TRUE(st->write_partition(Store::Ohlc1D, d, cols).has_value());
  }
  auto dates = st->list_dates(Store::Ohlc1D);
  ASSERT_TRUE(dates.has_value()) << dates.error().message();
  ASSERT_EQ(dates->size(), 3u);
  EXPECT_EQ((*dates)[0], "2026-06-03");
  EXPECT_EQ((*dates)[1], "2026-06-04");
  EXPECT_EQ((*dates)[2], "2026-06-05");
  fs::remove_all(root);
}
