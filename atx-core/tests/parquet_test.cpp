#include <atx/core/io/parquet.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>

#include <arrow/api.h>
#include <parquet/arrow/writer.h>
#include <arrow/io/file.h>
#include <filesystem>

using namespace atx::core::io;

namespace {
std::string temp_path(const char* stem) {
  auto dir = std::filesystem::temp_directory_path();
  return (dir / (std::string{"atx_pq_"} + stem + ".parquet")).string();
}

// Writes `table` to `path` with the given compression. Aborts the test on error.
void write_table(const std::shared_ptr<arrow::Table>& table, const std::string& path,
                 arrow::Compression::type codec = arrow::Compression::SNAPPY,
                 int64_t row_group_size = 1024) {
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  auto props = parquet::WriterProperties::Builder().compression(codec)->build();
  auto arrow_props = parquet::ArrowWriterProperties::Builder().build();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out,
                                         row_group_size, props, arrow_props).ok());
}

// Builds a 2-column int64/double table with `n` rows: i, i*1.5.
std::shared_ptr<arrow::Table> make_numeric_table(int64_t n) {
  arrow::Int64Builder a; arrow::DoubleBuilder b;
  for (int64_t i = 0; i < n; ++i) { (void)a.Append(i); (void)b.Append(static_cast<double>(i) * 1.5); }
  std::shared_ptr<arrow::Array> aa, ba; (void)a.Finish(&aa); (void)b.Finish(&ba);
  auto schema = arrow::schema({arrow::field("id", arrow::int64()),
                               arrow::field("val", arrow::float64())});
  return arrow::Table::Make(schema, {aa, ba});
}
} // namespace

TEST(Parquet, MissingFileReturnsNotFound) {
  auto r = read_parquet("this_file_does_not_exist_12345.parquet");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::NotFound);
}

TEST(Parquet, ScanReportsSchemaAndMetadata) {
  auto path = temp_path("schema");
  write_table(make_numeric_table(2500), path);  // 1024 row-group size -> 3 groups

  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->num_rows(), 2500);
  EXPECT_EQ(lz->num_row_groups(), 3);
  const Schema& s = lz->schema();
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s.columns[0].name, "id");
  EXPECT_EQ(s.columns[0].dtype, DType::Int64);
  EXPECT_EQ(s.columns[1].name, "val");
  EXPECT_EQ(s.columns[1].dtype, DType::Float64);
}
