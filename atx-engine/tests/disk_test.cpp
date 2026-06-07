#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "atx/engine/data/disk.hpp"

namespace fs = std::filesystem;
using atx::engine::data::DiskStore;
using atx::engine::data::Store;

namespace {
fs::path unique_tmp() {
  static int counter = 0;
  return fs::temp_directory_path() /
         ("atx_disk_test_" + std::to_string(++counter) + "_" +
          std::to_string(static_cast<long long>(reinterpret_cast<std::uintptr_t>(&counter))));
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
