#include <atx/core/io/parquet.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>

#include <arrow/api.h>
#include <arrow/util/compression.h>
#include <parquet/arrow/writer.h>
#include <arrow/io/file.h>
#include <filesystem>
#include <fstream>
#include <limits>

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

// Builds a single uint64 column "u" with values 0..n-1, to exercise the
// signed/unsigned wrap guard in the predicate engine.
std::shared_ptr<arrow::Table> make_uint64_table(int64_t n) {
  arrow::UInt64Builder u;
  for (int64_t i = 0; i < n; ++i) { (void)u.Append(static_cast<uint64_t>(i)); }
  std::shared_ptr<arrow::Array> ua; (void)u.Finish(&ua);
  auto schema = arrow::schema({arrow::field("u", arrow::uint64())});
  return arrow::Table::Make(schema, {ua});
}

// Builds a single double column "d" = [1.0, NaN, 3.0], to exercise NaN handling
// (NaN is unequal to everything: comparisons drop it, except !=).
std::shared_ptr<arrow::Table> make_nan_table() {
  arrow::DoubleBuilder d;
  (void)d.Append(1.0);
  (void)d.Append(std::numeric_limits<double>::quiet_NaN());
  (void)d.Append(3.0);
  std::shared_ptr<arrow::Array> da; (void)d.Finish(&da);
  auto schema = arrow::schema({arrow::field("d", arrow::float64())});
  return arrow::Table::Make(schema, {da});
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

TEST(Parquet, FilterGreaterThan) {
  auto path = temp_path("filt"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{90}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 10);               // ids 90..99
  auto v = t->column_view<int64_t>("id"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 90);
  EXPECT_EQ((*v)[9], 99);
}

TEST(Parquet, FilterConjunction) {
  auto path = temp_path("filt2"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{10}}})
              .filter(Predicate{"id", Compare::Lt, Scalar{int64_t{20}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 10);               // ids 10..19
}

TEST(Parquet, FilterEqMatchesOne) {
  auto path = temp_path("filteq"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Eq, Scalar{int64_t{42}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 1);
  auto v = t->column_view<int64_t>("id"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 42);
}

TEST(Parquet, FilterTypeMismatchIsInvalidArgument) {
  auto path = temp_path("filt3"); write_table(make_numeric_table(10), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  // id is int64; comparing against a string literal must be rejected.
  EXPECT_EQ(lz->filter(Predicate{"id", Compare::Eq, Scalar{std::string{"x"}}}).collect().error().code(),
            atx::core::ErrorCode::InvalidArgument);
}

TEST(Parquet, FilterUnknownColumnIsInvalidArgument) {
  auto path = temp_path("filt4"); write_table(make_numeric_table(10), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->filter(Predicate{"ghost", Compare::Eq, Scalar{int64_t{1}}}).collect().error().code(),
            atx::core::ErrorCode::InvalidArgument);
}

TEST(Parquet, FilterWithSelectDropsPredicateColumn) {
  auto path = temp_path("filtsel"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  // select only "val" but filter on "id": id must be read for the predicate,
  // then dropped from the output so only "val" remains.
  auto t = lz->select({"val"}).filter(Predicate{"id", Compare::Ge, Scalar{int64_t{90}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 10);
  EXPECT_EQ(t->num_columns(), 1);
  ASSERT_EQ(t->schema().size(), 1u);
  EXPECT_EQ(t->schema().columns[0].name, "val");
}

TEST(Parquet, FilterUInt64VsNegativeLiteral) {
  auto path = temp_path("filtu64"); write_table(make_uint64_table(5), path); // u = 0..4
  // A negative literal is strictly less than every unsigned cell:
  // u >= -1 keeps all 5; u < -1 keeps none. (Guards the signed/unsigned wrap.)
  auto lz1 = LazyParquet::scan(path); ASSERT_TRUE(lz1.has_value());
  auto t1 = lz1->filter(Predicate{"u", Compare::Ge, Scalar{int64_t{-1}}}).collect();
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(t1->num_rows(), 5);
  auto lz2 = LazyParquet::scan(path); ASSERT_TRUE(lz2.has_value());
  auto t2 = lz2->filter(Predicate{"u", Compare::Lt, Scalar{int64_t{-1}}}).collect();
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(t2->num_rows(), 0);
}

TEST(Parquet, FilterNaNRows) {
  auto path = temp_path("filtnan"); write_table(make_nan_table(), path); // d = [1, NaN, 3]
  // d < 5.0  -> keeps 1.0 and 3.0 (NaN dropped)                 -> 2 rows
  auto lz1 = LazyParquet::scan(path); ASSERT_TRUE(lz1.has_value());
  auto t1 = lz1->filter(Predicate{"d", Compare::Lt, Scalar{5.0}}).collect();
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(t1->num_rows(), 2);
  // d != 2.0 -> keeps 1.0, NaN, 3.0 (NaN unequal to everything) -> 3 rows
  auto lz2 = LazyParquet::scan(path); ASSERT_TRUE(lz2.has_value());
  auto t2 = lz2->filter(Predicate{"d", Compare::Ne, Scalar{2.0}}).collect();
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(t2->num_rows(), 3);
}

TEST(Parquet, RowGroupPruning) {
  // 3 row groups of 1000 rows: ids 0..2999. Filter id >= 2500 -> only group 3 matches.
  auto path = temp_path("prune");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, /*row_group=*/1000);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{2500}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 500);                 // 2500..2999
  auto st = lz->stats();
  EXPECT_EQ(st.row_groups_total, 3);
  EXPECT_EQ(st.row_groups_pruned, 2);            // groups 1 and 2 skipped by stats
}

TEST(Parquet, RowGroupPruningKeepsAllWhenNoPredicate) {
  auto path = temp_path("noprune");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, 1000);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 3000);
  EXPECT_EQ(lz->stats().row_groups_total, 3);
  EXPECT_EQ(lz->stats().row_groups_pruned, 0);
}

TEST(Parquet, RowGroupPruningAllMatchNoPrune) {
  auto path = temp_path("allmatch");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, 1000);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{0}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 3000);
  EXPECT_EQ(lz->stats().row_groups_pruned, 0);   // every group can match
}

// Two int64 row groups: group A = 1000x (2^53+1), group B = 1000x 0. A naive
// double-domain prune rounds 2^53+1 down to 2^53, so `Gt 2^53` would wrongly skip
// group A and drop its 1000 matching rows. Exact int64 pruning keeps group A.
TEST(Parquet, RowGroupPruningLargeInt64Exact) {
  constexpr int64_t kBig = 9007199254740993LL; // 2^53 + 1
  constexpr int64_t kThresh = 9007199254740992LL; // 2^53
  arrow::Int64Builder a;
  for (int i = 0; i < 1000; ++i) { (void)a.Append(kBig); } // group A
  for (int i = 0; i < 1000; ++i) { (void)a.Append(0); }    // group B
  std::shared_ptr<arrow::Array> aa; (void)a.Finish(&aa);
  auto schema = arrow::schema({arrow::field("big", arrow::int64())});
  auto table = arrow::Table::Make(schema, {aa});
  auto path = temp_path("bigint");
  write_table(table, path, arrow::Compression::SNAPPY, /*row_group=*/1000); // 2 groups

  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"big", Compare::Gt, Scalar{int64_t{kThresh}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 1000);                 // all of group A; NOT skipped
  auto v = t->column_view<int64_t>("big"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], kBig);

  // Ne 2^53 must keep every row (2^53 is present in neither group): all 2000.
  auto lz2 = LazyParquet::scan(path); ASSERT_TRUE(lz2.has_value());
  auto t2 = lz2->filter(Predicate{"big", Compare::Ne, Scalar{int64_t{kThresh}}}).collect();
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(t2->num_rows(), 2000);
}

// Two uint64 row groups: group A holds a value >= 2^63 (reads back negative if
// mis-signed as int64), group B holds small values. With unsigned pruning disabled
// no group is skipped, so `Ge 1` must return every value >= 1 (the big value plus
// the non-zero small ones) -- the high-value rows must NOT be lost.
TEST(Parquet, RowGroupPruningUInt64NoWrongSkip) {
  constexpr uint64_t kHuge = 9223372036854775809ULL; // 2^63 + 1
  arrow::UInt64Builder u;
  for (int i = 0; i < 1000; ++i) { (void)u.Append(kHuge); }                 // group A
  for (int i = 0; i < 1000; ++i) { (void)u.Append(static_cast<uint64_t>(i)); } // group B: 0..999
  std::shared_ptr<arrow::Array> ua; (void)u.Finish(&ua);
  auto schema = arrow::schema({arrow::field("u", arrow::uint64())});
  auto table = arrow::Table::Make(schema, {ua});
  auto path = temp_path("huge_u64");
  write_table(table, path, arrow::Compression::SNAPPY, /*row_group=*/1000); // 2 groups

  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"u", Compare::Ge, Scalar{int64_t{1}}}).collect();
  ASSERT_TRUE(t.has_value());
  // 1000 huge values (group A) + 999 non-zero small values (group B: 1..999).
  EXPECT_EQ(t->num_rows(), 1999);
  EXPECT_EQ(lz->stats().row_groups_pruned, 0);    // unsigned columns are never pruned
}

TEST(Parquet, LimitOffset) {
  auto path = temp_path("slice"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->offset(10).limit(5).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 5);
  auto v = t->column_view<int64_t>("id"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 10);
  EXPECT_EQ((*v)[4], 14);
}

TEST(Parquet, LimitOnly) {
  auto path = temp_path("limit"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->limit(3).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 3);
  auto v = t->column_view<int64_t>("id"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 0);
  EXPECT_EQ((*v)[2], 2);
}

TEST(Parquet, OffsetBeyondEndIsEmpty) {
  auto path = temp_path("offend"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->offset(1000).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 0);
}

TEST(Parquet, LimitAfterFilter) {
  auto path = temp_path("limfilt"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  // filter id>=50 (50 rows), then take the first 5: ids 50..54.
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{50}}}).limit(5).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 5);
  auto v = t->column_view<int64_t>("id"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 50);
  EXPECT_EQ((*v)[4], 54);
}

TEST(Parquet, NoLimitReturnsAll) {
  auto path = temp_path("nolim"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 100);
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

TEST(Parquet, StreamSumsToCollect) {
  auto path = temp_path("stream");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, 1000);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto stream = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{500}}}).stream();
  ASSERT_TRUE(stream.has_value());
  int64_t total = 0; int batches = 0;
  for (;;) {
    auto batch = stream->next();
    ASSERT_TRUE(batch.has_value());
    if (!batch->has_value()) { break; }
    total += (*batch)->num_rows();
    ++batches;
  }
  EXPECT_EQ(total, 2500);   // ids 500..2999
  EXPECT_GE(batches, 1);
}

TEST(Parquet, StreamRespectsProjection) {
  auto path = temp_path("streamproj");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, 1000);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto stream = lz->select({"val"}).stream();
  ASSERT_TRUE(stream.has_value());
  auto batch = stream->next();
  ASSERT_TRUE(batch.has_value());
  ASSERT_TRUE(batch->has_value());
  EXPECT_EQ((*batch)->num_columns(), 1);
  EXPECT_EQ((*batch)->schema().columns[0].name, "val");
}

TEST(Parquet, StreamWithLimitStopsEarly) {
  auto path = temp_path("streamlim");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, 1000);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto stream = lz->limit(5).stream();
  ASSERT_TRUE(stream.has_value());
  int64_t total = 0;
  for (;;) {
    auto batch = stream->next();
    ASSERT_TRUE(batch.has_value());
    if (!batch->has_value()) { break; }
    total += (*batch)->num_rows();
  }
  EXPECT_EQ(total, 5);
}

TEST(Parquet, StreamAllPrunedYieldsNothing) {
  auto path = temp_path("streamprune");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, 1000);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto stream = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{100000}}}).stream();
  ASSERT_TRUE(stream.has_value());
  auto batch = stream->next();
  ASSERT_TRUE(batch.has_value());
  EXPECT_FALSE(batch->has_value());   // all 3 groups pruned -> no batches
}

TEST(Parquet, CorruptFileIsParseError) {
  auto path = temp_path("corrupt");
  {
    std::ofstream f(path, std::ios::binary);
    f << "not a parquet file at all, just some random bytes ......";
  }
  auto r = read_parquet(path);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::ParseError);
}

TEST(Parquet, ScanCorruptFileIsParseError) {
  auto path = temp_path("corrupt2");
  {
    std::ofstream f(path, std::ios::binary);
    f << "PAR1garbagePAR1"; // looks parquet-ish but is not a valid footer
  }
  auto r = LazyParquet::scan(path);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::ParseError);
}

TEST(Parquet, NestedColumnSchemaUnsupported) {
  // list<int64> column -> schema reports Unsupported; bridge to a numeric T errors.
  arrow::ListBuilder lb(arrow::default_memory_pool(),
                        std::make_shared<arrow::Int64Builder>());
  auto* vb = static_cast<arrow::Int64Builder*>(lb.value_builder());
  (void)lb.Append(); (void)vb->Append(1); (void)vb->Append(2);
  (void)lb.Append(); (void)vb->Append(3);
  std::shared_ptr<arrow::Array> la;
  (void)lb.Finish(&la);
  auto schema = arrow::schema({arrow::field("l", arrow::list(arrow::int64()))});
  auto table = arrow::Table::Make(schema, {la});
  auto path = temp_path("nested");
  write_table(table, path);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  ASSERT_NE(lz->schema().find("l"), nullptr);
  EXPECT_EQ(lz->schema().find("l")->dtype, DType::Unsupported);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  // bridging a nested/list column to a numeric Column<T> must error (type mismatch).
  EXPECT_EQ(t->to_column<int64_t>("l").error().code(),
            atx::core::ErrorCode::InvalidArgument);
}

TEST(Parquet, ToFrameSkipsUnsupportedColumns) {
  // a table with an int64 column AND a list column: to_frame keeps the numeric, skips the list.
  arrow::Int64Builder ib; (void)ib.Append(10); (void)ib.Append(20);
  arrow::ListBuilder lb(arrow::default_memory_pool(), std::make_shared<arrow::Int64Builder>());
  auto* vb = static_cast<arrow::Int64Builder*>(lb.value_builder());
  (void)lb.Append(); (void)vb->Append(1);
  (void)lb.Append(); (void)vb->Append(2);
  std::shared_ptr<arrow::Array> ia, la; (void)ib.Finish(&ia); (void)lb.Finish(&la);
  auto schema = arrow::schema({arrow::field("n", arrow::int64()),
                               arrow::field("l", arrow::list(arrow::int64()))});
  auto table = arrow::Table::Make(schema, {ia, la});
  auto path = temp_path("mixednested"); write_table(table, path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  auto f = t->to_frame(); ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->has_column("n"));
  EXPECT_FALSE(f->has_column("l"));   // unsupported column skipped
}
