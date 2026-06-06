#include "atx/core/io/parquet_writer.hpp"

#include "atx/core/io/parquet.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace io = atx::core::io;
namespace fs = std::filesystem;
using atx::i64; // vocabulary type lives in namespace atx

TEST(ParquetWriter, FlatRoundTrip) {
  const std::vector<i64> a = {1, 2, 3};
  const std::vector<std::string> sym = {"AAPL", "MSFT", "NVDA"};
  const std::vector<io::WriteColumn> cols = {
      {"v", std::span<const i64>(a)},
      {"sym", std::span<const std::string>(sym)},
  };
  const fs::path out = fs::temp_directory_path() / "atx_pqw_flat.parquet";
  fs::remove(out);
  ASSERT_TRUE(io::write_parquet(cols, out.string()).has_value());
  ASSERT_TRUE(fs::exists(out));

  auto t = io::read_parquet(out.string());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 3);
  EXPECT_EQ(t->num_columns(), 2);
}

TEST(ParquetWriter, HivePartitionsByDate) {
  const std::vector<i64> close = {10, 11, 12, 13};
  const std::vector<std::string> date = {"2024-01-02", "2024-01-02", "2024-01-03", "2024-01-03"};
  const std::vector<io::WriteColumn> cols = {
      {"close", std::span<const i64>(close)},
      {"date", std::span<const std::string>(date)},
  };
  const fs::path root = fs::temp_directory_path() / "atx_pqw_hive";
  fs::remove_all(root);
  auto n = io::write_hive_parquet(cols, root.string(), "date");
  ASSERT_TRUE(n.has_value()) << n.error().to_string();
  EXPECT_EQ(*n, 2);
  EXPECT_TRUE(fs::exists(root / "date=2024-01-02" / "data.parquet"));
  EXPECT_TRUE(fs::exists(root / "date=2024-01-03" / "data.parquet"));

  // The partition column is dropped from the file; only "close" remains.
  auto t = io::read_parquet((root / "date=2024-01-02" / "data.parquet").string());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 2);
  EXPECT_EQ(t->num_columns(), 1);
  EXPECT_EQ(t->schema().columns.at(0).name, "close");
}

TEST(ParquetWriter, HiveRejectsMissingPartitionColumn) {
  const std::vector<i64> v = {1};
  const std::vector<io::WriteColumn> cols = {{"v", std::span<const i64>(v)}};
  auto n = io::write_hive_parquet(cols, "ignored", "nope");
  EXPECT_FALSE(n.has_value());
}
