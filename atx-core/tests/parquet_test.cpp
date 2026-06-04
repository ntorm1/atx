#include <atx/core/io/parquet.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>

#include <arrow/api.h>
#include <arrow/util/compression.h>
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

// Builds a string/bool/timestamp[ms] table with a null in the string column, to
// exercise the Task-4 dtype bridges (null -> empty view / stored value).
std::shared_ptr<arrow::Table> make_mixed_table() {
  arrow::StringBuilder s; (void)s.Append("a"); (void)s.AppendNull(); (void)s.Append("c");
  arrow::BooleanBuilder bo; (void)bo.Append(true); (void)bo.Append(false); (void)bo.Append(true);
  arrow::TimestampBuilder ts(arrow::timestamp(arrow::TimeUnit::MILLI), arrow::default_memory_pool());
  (void)ts.Append(1000); (void)ts.Append(2000); (void)ts.Append(3000);
  std::shared_ptr<arrow::Array> sa, ba, ta; (void)s.Finish(&sa); (void)bo.Finish(&ba); (void)ts.Finish(&ta);
  auto schema = arrow::schema({arrow::field("name", arrow::utf8()),
                               arrow::field("flag", arrow::boolean()),
                               arrow::field("t", arrow::timestamp(arrow::TimeUnit::MILLI))});
  return arrow::Table::Make(schema, {sa, ba, ta});
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

TEST(Parquet, CollectReadsAllRows) {
  auto path = temp_path("collect");
  write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 100);
  EXPECT_EQ(t->num_columns(), 2);
}

TEST(Parquet, ToColumnCopiesValues) {
  auto path = temp_path("tocol");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  auto col = t->to_column<double>("val");
  ASSERT_TRUE(col.has_value());
  ASSERT_EQ(col->size(), 10u);
  EXPECT_DOUBLE_EQ((*col)[3], 4.5);
}

TEST(Parquet, ColumnViewAliasesBuffer) {
  auto path = temp_path("view");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  auto v = t->column_view<int64_t>("id");
  ASSERT_TRUE(v.has_value());
  ASSERT_EQ(v->size(), 10u);
  EXPECT_EQ((*v)[7], 7);
}

TEST(Parquet, ToColumnWrongTypeIsInvalidArgument) {
  auto path = temp_path("wrongtype");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->to_column<double>("id").error().code(),  // id is int64
            atx::core::ErrorCode::InvalidArgument);
}

TEST(Parquet, ToColumnUnknownColumnIsInvalidArgument) {
  auto path = temp_path("nocol");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->to_column<double>("does_not_exist").error().code(),
            atx::core::ErrorCode::InvalidArgument);
}

TEST(Parquet, ColumnViewMultiChunkIsContiguous) {
  auto path = temp_path("multichunk");
  write_table(make_numeric_table(2500), path); // 1024 row-group size -> 3 row groups
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  auto v = t->column_view<int64_t>("id");
  ASSERT_TRUE(v.has_value());
  ASSERT_EQ(v->size(), 2500u);
  EXPECT_EQ((*v)[0], 0);
  EXPECT_EQ((*v)[2499], 2499);
  int64_t sum = 0;
  for (int64_t x : *v) { sum += x; }
  EXPECT_EQ(sum, 2499LL * 2500 / 2);
  auto c = t->to_column<double>("val");
  ASSERT_TRUE(c.has_value());
  ASSERT_EQ(c->size(), 2500u);
  EXPECT_DOUBLE_EQ((*c)[2499], 2499.0 * 1.5);
}

TEST(Parquet, ToFrameHasNumericColumns) {
  auto path = temp_path("toframe");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  auto f = t->to_frame();
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->has_column("id"));
  EXPECT_TRUE(f->has_column("val"));
}

TEST(Parquet, StringsAccessor) {
  auto path = temp_path("strings"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  auto s = t->strings("name"); ASSERT_TRUE(s.has_value());
  ASSERT_EQ(s->size(), 3u);
  EXPECT_EQ((*s)[0], "a");
  EXPECT_EQ((*s)[1], "");   // null -> empty view
  EXPECT_EQ((*s)[2], "c");
}

TEST(Parquet, StringsWrongTypeIsInvalidArgument) {
  auto path = temp_path("strbad"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->strings("flag").error().code(), atx::core::ErrorCode::InvalidArgument); // flag is bool
}

TEST(Parquet, BoolBridge) {
  auto path = temp_path("bool"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  auto c = t->to_column<bool>("flag"); ASSERT_TRUE(c.has_value());
  ASSERT_EQ(c->size(), 3u);
  EXPECT_TRUE((*c)[0]); EXPECT_FALSE((*c)[1]); EXPECT_TRUE((*c)[2]);
}

TEST(Parquet, BoolColumnViewIsNotImplemented) {
  auto path = temp_path("boolview"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->column_view<bool>("flag").error().code(), atx::core::ErrorCode::NotImplemented);
}

TEST(Parquet, TimestampBridge) {
  auto path = temp_path("ts"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  auto c = t->to_column<atx::core::time::Timestamp>("t"); ASSERT_TRUE(c.has_value());
  ASSERT_EQ(c->size(), 3u);
  EXPECT_EQ((*c)[0].unix_nanos(), 1'000'000'000); // 1000 ms -> 1e9 ns
}

TEST(Parquet, SchemaReportsAllDtypes) {
  auto path = temp_path("dtypes"); write_table(make_mixed_table(), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  ASSERT_NE(lz->schema().find("name"), nullptr);
  EXPECT_EQ(lz->schema().find("name")->dtype, DType::String);
  EXPECT_EQ(lz->schema().find("flag")->dtype, DType::Bool);
  EXPECT_EQ(lz->schema().find("t")->dtype, DType::Timestamp);
}

TEST(Parquet, SelectProjectsColumns) {
  auto path = temp_path("select");
  write_table(make_numeric_table(50), path);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->select({"val"}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_columns(), 1);
  ASSERT_EQ(t->schema().size(), 1u);
  EXPECT_EQ(t->schema().columns[0].name, "val");
  // projected column still materializes correctly
  auto col = t->to_column<double>("val");
  ASSERT_TRUE(col.has_value());
  EXPECT_DOUBLE_EQ((*col)[49], 49.0 * 1.5);
}

TEST(Parquet, SelectUnknownColumnIsInvalidArgument) {
  auto path = temp_path("selbad");
  write_table(make_numeric_table(5), path);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->select({"nope"}).collect().error().code(),
            atx::core::ErrorCode::InvalidArgument);
}

TEST(Parquet, SelectEmptyKeepsAllColumns) {
  auto path = temp_path("selall");
  write_table(make_numeric_table(5), path);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->collect(); // no select -> all columns
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_columns(), 2);
}

TEST(Parquet, AllCodecsRoundTrip) {
  struct C { arrow::Compression::type codec; const char* stem; };
  std::vector<C> codecs = {
      {arrow::Compression::UNCOMPRESSED, "raw"},
      {arrow::Compression::SNAPPY,       "snappy"},
      {arrow::Compression::GZIP,         "gzip"},
      {arrow::Compression::ZSTD,         "zstd"},
      // Parquet's "LZ4" wire format maps to Arrow's LZ4_HADOOP (Hadoop-compatible
      // LZ4 framing).  LZ4 (block) and LZ4_FRAME both crash the Parquet thrift
      // codec encoder in this Arrow build, so we use LZ4_HADOOP here.
      {arrow::Compression::LZ4_HADOOP,   "lz4"},
  };
  if (arrow::util::Codec::IsAvailable(arrow::Compression::BROTLI)) {
    codecs.push_back({arrow::Compression::BROTLI, "brotli"});
  }
  for (const auto& c : codecs) {
    auto path = temp_path(c.stem);
    write_table(make_numeric_table(500), path, c.codec);
    auto t = read_parquet(path);
    ASSERT_TRUE(t.has_value()) << c.stem;
    EXPECT_EQ(t->num_rows(), 500) << c.stem;
    auto col = t->to_column<double>("val");
    ASSERT_TRUE(col.has_value()) << c.stem;
    EXPECT_DOUBLE_EQ((*col)[499], 499.0 * 1.5) << c.stem;
  }
}
