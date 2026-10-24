// spiderrock_store_test.cpp — the SpiderRock intraday store loader, the bucket ->
// MarketStateStorage builder and the SpiderRock-SHAPED synthetic store
// (api/marketdata/spiderrock_store.hpp; design 2026-10-24 §5.7, D4 revised).
//
// FIXTURE. Hand-written partitions use the Arrow C++ API directly (the
// oracle_bench_test.cpp arrangement: vcpkg Arrow targets are SYSTEM includes) so a
// partition can carry genuine NULL cells and either string encoding. The synthetic
// store is written by the shipped generator into a per-test temp dir. No licensed
// data is read anywhere in this file.

#include <gtest/gtest.h>

#include <arrow/array.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/io/file.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/marketdata/data.hpp"
#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/pricing/american.hpp"

#if defined(_WIN32)
#include <process.h> // _getpid: a private scratch dir per test process
#else
#include <unistd.h> // getpid
#endif

namespace {

namespace fs = std::filesystem;
using atx::vol::BucketStateReport;
using atx::vol::ErrorCode;
using atx::vol::MarketStateStorage;
using atx::vol::Side;
using atx::vol::SpiderRockBucket;
using atx::vol::SpiderRockStoreSpec;
using atx::vol::SyntheticSpiderRockSpec;
using atx::vol::SyntheticStoreReport;
using atx::vol::TimeSpec;
using atx::vol::UnderlierTable;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// A scratch dir private to this test and this process (atx-test-scratch already gives
// each process a private TEMP; the pid keeps it so in a binary that does not link it).
[[nodiscard]] fs::path fresh_dir(std::string_view tag) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-spiderrock-" + std::string(tag);
  if (info != nullptr) {
    name += std::string("-") + info->test_suite_name() + "-" + info->name();
  }
#if defined(_WIN32)
  name += "-" + std::to_string(::_getpid());
#else
  name += "-" + std::to_string(::getpid());
#endif
  const fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// ── Hand-written partition fixture ──────────────────────────────────────────

struct Row {
  std::string tk = "AAA";
  std::string cp = "Call";
  std::int64_t yr = 2026, mn = 9, dy = 18;
  double strike = 100.0;
  double uprc = 100.0;
  double rate = 0.04;
  double sdiv = 0.01;
  double ddiv = 0.0;
  double years = 0.1;
  double srVol = 0.25;
  double srPrc = 2.5;
  double de = 0.5, ga = 0.03, th = -0.02, ve = 0.12;
  double bidPrc = 2.4, askPrc = 2.6;
  bool null_srvol = false;
  bool null_uprc = false;
  bool null_tk = false;
  // Written only when write_partition(licensed_extras): the 2026-10-14 drop's
  // exercise style, option root (empty root = the underlier ticker) and price
  // framework (empty = NULL).
  std::string ex_type = "American";
  std::string root;
  std::string framework = "Spot";
};

struct NumField {
  const char *name;
  double Row::*field;
};
constexpr NumField kNumFields[] = {
    {"okey_xx", &Row::strike}, {"uPrc", &Row::uprc},     {"rate", &Row::rate},
    {"sdiv", &Row::sdiv},      {"ddiv", &Row::ddiv},     {"years", &Row::years},
    {"srVol", &Row::srVol},    {"srPrc", &Row::srPrc},   {"de", &Row::de},
    {"ga", &Row::ga},          {"th", &Row::th},         {"ve", &Row::ve},
    {"bidPrc", &Row::bidPrc},  {"askPrc", &Row::askPrc},
};

void write_partition(const fs::path &dir, const std::vector<Row> &rows, bool large_strings,
                     bool licensed_extras = false, std::int64_t row_group_rows = 3,
                     bool nested_lead = false) {
  ASSERT_TRUE(fs::create_directories(dir) || fs::is_directory(dir));
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  auto add_string = [&](const char *name, auto get, auto is_null) {
    std::shared_ptr<arrow::Array> arr;
    if (large_strings) {
      arrow::LargeStringBuilder b;
      for (const Row &r : rows) {
        ASSERT_TRUE((is_null(r) ? b.AppendNull() : b.Append(get(r))).ok());
      }
      ASSERT_TRUE(b.Finish(&arr).ok());
      fields.push_back(arrow::field(name, arrow::large_utf8()));
    } else {
      arrow::StringBuilder b;
      for (const Row &r : rows) {
        ASSERT_TRUE((is_null(r) ? b.AppendNull() : b.Append(get(r))).ok());
      }
      ASSERT_TRUE(b.Finish(&arr).ok());
      fields.push_back(arrow::field(name, arrow::utf8()));
    }
    arrays.push_back(std::move(arr));
  };
  add_string("undSecKey_tk", [](const Row &r) { return r.tk; },
             [](const Row &r) { return r.null_tk; });
  if (::testing::Test::HasFatalFailure()) {
    return;
  }
  add_string("okey_cp", [](const Row &r) { return r.cp; }, [](const Row &) { return false; });
  if (::testing::Test::HasFatalFailure()) {
    return;
  }
  if (licensed_extras) { // the licensed intraday drop's exType / okey_tk columns
    add_string("exType", [](const Row &r) { return r.ex_type; },
               [](const Row &r) { return r.ex_type.empty(); });
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
    add_string("okey_tk", [](const Row &r) { return r.root.empty() ? r.tk : r.root; },
               [](const Row &) { return false; });
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
    add_string("prcFramework", [](const Row &r) { return r.framework; },
               [](const Row &r) { return r.framework.empty(); });
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
  auto add_i64 = [&](const char *name, std::int64_t Row::*f) {
    arrow::Int64Builder b;
    for (const Row &r : rows) {
      ASSERT_TRUE(b.Append(r.*f).ok());
    }
    std::shared_ptr<arrow::Array> arr;
    ASSERT_TRUE(b.Finish(&arr).ok());
    fields.push_back(arrow::field(name, arrow::int64()));
    arrays.push_back(std::move(arr));
  };
  add_i64("okey_yr", &Row::yr);
  add_i64("okey_mn", &Row::mn);
  add_i64("okey_dy", &Row::dy);
  if (::testing::Test::HasFatalFailure()) {
    return;
  }
  for (const NumField &c : kNumFields) {
    arrow::DoubleBuilder b;
    for (const Row &r : rows) {
      const bool null_cell = (c.field == &Row::srVol && r.null_srvol) ||
                             (c.field == &Row::uprc && r.null_uprc);
      ASSERT_TRUE((null_cell ? b.AppendNull() : b.Append(r.*c.field)).ok());
    }
    std::shared_ptr<arrow::Array> arr;
    ASSERT_TRUE(b.Finish(&arr).ok());
    fields.push_back(arrow::field(c.name, arrow::float64()));
    arrays.push_back(std::move(arr));
  }
  { // an extra column the reader must tolerate
    arrow::DoubleBuilder b;
    for (const Row &r : rows) {
      ASSERT_TRUE(b.Append(r.srVol).ok());
    }
    std::shared_ptr<arrow::Array> arr;
    ASSERT_TRUE(b.Finish(&arr).ok());
    fields.push_back(arrow::field("atmVol", arrow::float64()));
    arrays.push_back(std::move(arr));
  }
  if (nested_lead) { // a 2-leaf struct column FIRST: arrow field index != parquet leaf
    std::vector<std::shared_ptr<arrow::Array>> kids;
    for (int k = 0; k < 2; ++k) {
      arrow::DoubleBuilder b;
      for (const Row &r : rows) {
        ASSERT_TRUE(b.Append(r.strike).ok());
      }
      std::shared_ptr<arrow::Array> arr;
      ASSERT_TRUE(b.Finish(&arr).ok());
      kids.push_back(std::move(arr));
    }
    auto nested = arrow::StructArray::Make(kids, std::vector<std::string>{"a", "b"});
    ASSERT_TRUE(nested.ok()) << nested.status().ToString();
    fields.insert(fields.begin(), arrow::field("nested", (*nested)->type()));
    arrays.insert(arrays.begin(), *nested);
  }
  const auto table =
      arrow::Table::Make(arrow::schema(fields), arrays, static_cast<std::int64_t>(rows.size()));
  auto out = arrow::io::FileOutputStream::Open((dir / "data.parquet").string());
  ASSERT_TRUE(out.ok());
  const arrow::Status st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out,
                                                      row_group_rows);
  ASSERT_TRUE(st.ok()) << st.ToString();
  ASSERT_TRUE((*out)->Close().ok());
}

[[nodiscard]] fs::path bucket_dir(const fs::path &root, std::string_view date,
                                  std::string_view bucket) {
  return root / ("date=" + std::string(date)) / ("bucket_et=" + std::string(bucket));
}

[[nodiscard]] SpiderRockStoreSpec spec_for(const fs::path &root, std::string_view date) {
  SpiderRockStoreSpec spec;
  spec.store_root = root.string();
  spec.date = std::string(date);
  return spec;
}

[[nodiscard]] std::vector<Row> three_rows() {
  std::vector<Row> rows(3);
  rows[0].strike = 95.0;
  rows[1].strike = 100.0;
  rows[1].cp = "Put";
  rows[2].strike = 105.0;
  rows[2].tk = "BBB";
  return rows;
}

// ── SpiderRockStore ─────────────────────────────────────────────────────────

TEST(SpiderRockStore, ReadsBothStringEncodings) {
  const fs::path root = fresh_dir("enc");
  write_partition(bucket_dir(root, "2026-08-14", "1000"), three_rows(), /*large=*/false);
  write_partition(bucket_dir(root, "2026-08-14", "1030"), three_rows(), /*large=*/true);
  ASSERT_FALSE(HasFatalFailure());

  const auto buckets = atx::vol::list_spiderrock_buckets(root.string(), "2026-08-14");
  ASSERT_TRUE(buckets.has_value()) << buckets.error().to_string();
  EXPECT_EQ(*buckets, (std::vector<std::string>{"1000", "1030"}));
  const auto missing = atx::vol::list_spiderrock_buckets(root.string(), "2026-08-15");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);

  UnderlierTable table;
  const SpiderRockStoreSpec spec = spec_for(root, "2026-08-14");
  const auto utf8 = atx::vol::load_spiderrock_bucket(spec, "1000", table);
  ASSERT_TRUE(utf8.has_value()) << utf8.error().to_string();
  const auto large = atx::vol::load_spiderrock_bucket(spec, "1030", table);
  ASSERT_TRUE(large.has_value()) << large.error().to_string();

  ASSERT_EQ(utf8->size(), 3u);
  ASSERT_EQ(large->size(), 3u);
  EXPECT_EQ(utf8->uid, large->uid);
  EXPECT_EQ(utf8->strike, large->strike);
  EXPECT_EQ(utf8->side, large->side);
  EXPECT_EQ(utf8->expiry_ns, large->expiry_ns);
  EXPECT_EQ(utf8->sigma, large->sigma);
  EXPECT_EQ(table.size(), 2u);
  EXPECT_EQ(table.ticker(table.find("AAA")), "AAA");
  EXPECT_EQ(table.find("ZZZ"), 0u);
  EXPECT_EQ(utf8->expiry_ns[0], atx::vol::expiry_instant_ns("2026-09-18"));
  EXPECT_EQ(utf8->bucket_et, "1000");
  EXPECT_EQ(utf8->files_opened.size(), 1u);
  EXPECT_EQ(utf8->rows_read, 3);

  const auto absent = atx::vol::load_spiderrock_bucket(spec, "1100", table);
  ASSERT_FALSE(absent.has_value());
  EXPECT_EQ(absent.error().code(), ErrorCode::NotFound);
}

TEST(SpiderRockStore, DropsNullAndBadRowsCounted) {
  std::vector<Row> rows(8);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rows[i].strike = 90.0 + static_cast<double>(i);
  }
  rows[1].null_srvol = true;                                     // null
  rows[2].null_uprc = true;                                      // null
  rows[3].rate = kNaN;                                           // bad
  rows[4].strike = -5.0;                                         // bad
  rows[5].years = std::numeric_limits<double>::infinity();       // bad
  rows[6].cp = "X";                                              // bad
  rows[7].mn = 13;                                               // bad expiry
  const fs::path root = fresh_dir("drop");
  write_partition(bucket_dir(root, "2026-08-14", "1000"), rows, true);
  ASSERT_FALSE(HasFatalFailure());

  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec_for(root, "2026-08-14"), "1000", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(b->rows_read, 8);
  EXPECT_EQ(b->rows_dropped_null, 2);
  EXPECT_EQ(b->rows_dropped_bad_input, 5);
  EXPECT_EQ(b->rows_not_selected, 0);
  ASSERT_EQ(b->size(), 1u);
  EXPECT_EQ(b->strike[0], 90.0);
  EXPECT_EQ(b->vendor_T[0], 0.1);
  EXPECT_EQ(b->bid[0], 2.4);
  EXPECT_EQ(b->ask[0], 2.6);
}

TEST(SpiderRockStore, VendorSentinelsReadAsNull) {
  std::vector<Row> rows(4);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rows[i].strike = 100.0 + static_cast<double>(i);
  }
  rows[1].srVol = -99.0; // required-column sentinel: dropped as null
  rows[2].sdiv = -9.99;  // the drop's sdiv sentinel: kept, carried as NaN
  rows[3].de = -99.0;    // optional-column sentinel: kept, carried as NaN
  const fs::path root = fresh_dir("sentinel");
  write_partition(bucket_dir(root, "2026-10-14", "1000"), rows, false);
  ASSERT_FALSE(HasFatalFailure());
  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec_for(root, "2026-10-14"), "1000", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(b->rows_dropped_null, 1);
  ASSERT_EQ(b->size(), 3u);
  EXPECT_EQ(b->rows_null_vendor, 2);
  EXPECT_EQ(b->sdiv[0], 0.01);
  EXPECT_TRUE(std::isnan(b->sdiv[1]));
  EXPECT_TRUE(std::isnan(b->vendor_delta[2]));
  EXPECT_EQ(b->vendor_delta[1], 0.5);
}

TEST(SpiderRockStore, SortsByKey) {
  auto make = [](const char *tk, const char *cp, std::int64_t dy, double strike) {
    Row r;
    r.tk = tk;
    r.cp = cp;
    r.dy = dy;
    r.strike = strike;
    return r;
  };
  // Written deliberately out of (uid, expiry, side, strike) order.
  const std::vector<Row> rows = {
      make("BBB", "Put", 18, 50.0),  make("AAA", "Put", 25, 110.0),
      make("AAA", "Call", 25, 120.0), make("AAA", "Put", 18, 90.0),
      make("AAA", "Call", 18, 100.0), make("AAA", "Call", 18, 95.0),
  };
  const fs::path root = fresh_dir("sort");
  write_partition(bucket_dir(root, "2026-08-14", "1000"), rows, true);
  ASSERT_FALSE(HasFatalFailure());

  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec_for(root, "2026-08-14"), "1000", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_EQ(b->size(), 6u);
  // uids are interned in ascending ticker order.
  EXPECT_EQ(table.find("AAA"), 1u);
  EXPECT_EQ(table.find("BBB"), 2u);
  for (std::size_t i = 1; i < b->size(); ++i) {
    const auto prev = std::make_tuple(b->uid[i - 1], b->expiry_ns[i - 1],
                                      static_cast<int>(b->side[i - 1]), b->strike[i - 1]);
    const auto cur = std::make_tuple(b->uid[i], b->expiry_ns[i], static_cast<int>(b->side[i]),
                                     b->strike[i]);
    EXPECT_LT(prev, cur) << "row " << i;
  }
  EXPECT_EQ(b->strike, (std::vector<double>{95.0, 100.0, 90.0, 120.0, 110.0, 50.0}));
}

TEST(SpiderRockStore, BucketInstantIs1600MinusOffset) {
  constexpr std::int64_t kHourNs = 3'600'000'000'000;
  const auto summer = atx::vol::spiderrock_bucket_instant_ns("2026-08-14", "1000");
  ASSERT_TRUE(summer.has_value());
  EXPECT_EQ(*summer, atx::vol::expiry_instant_ns("2026-08-14") - 6 * kHourNs);
  EXPECT_EQ(*summer, atx::vol::iso_to_ns("2026-08-14T14:00:00Z")); // EDT = UTC-4
  const auto winter = atx::vol::spiderrock_bucket_instant_ns("2026-12-14", "0940");
  ASSERT_TRUE(winter.has_value());
  EXPECT_EQ(*winter, atx::vol::iso_to_ns("2026-12-14T14:40:00Z")); // EST = UTC-5
  EXPECT_FALSE(atx::vol::spiderrock_bucket_instant_ns("2026-08-14", "1060").has_value());
  EXPECT_FALSE(atx::vol::spiderrock_bucket_instant_ns("2026-8-14", "1000").has_value());

  std::vector<Row> rows(1);
  const fs::path root = fresh_dir("instant");
  write_partition(bucket_dir(root, "2026-08-14", "1330"), rows, true);
  ASSERT_FALSE(HasFatalFailure());
  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec_for(root, "2026-08-14"), "1330", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(b->ts_ns, atx::vol::expiry_instant_ns("2026-08-14") - (2 * kHourNs + kHourNs / 2));
}

TEST(SpiderRockStore, TopNUnderliersIsDeterministic) {
  std::vector<Row> rows;
  auto add = [&](const char *tk, int n) {
    for (int i = 0; i < n; ++i) {
      Row r;
      r.tk = tk;
      r.strike = 80.0 + i;
      rows.push_back(r);
    }
  };
  add("DDD", 3);
  add("CCC", 7);
  add("AAA", 5);
  add("BBB", 7);
  const fs::path root = fresh_dir("topn");
  write_partition(bucket_dir(root, "2026-08-14", "1000"), rows, true);
  write_partition(bucket_dir(root, "2026-08-14", "1030"), rows, true);
  ASSERT_FALSE(HasFatalFailure());

  SpiderRockStoreSpec spec = spec_for(root, "2026-08-14");
  spec.max_underliers = 2;
  UnderlierTable t1;
  const auto a = atx::vol::load_spiderrock_bucket(spec, "1000", t1);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  // BBB and CCC tie at 7 rows and both beat AAA (5): ties break by ticker.
  EXPECT_EQ(t1.size(), 2u);
  EXPECT_EQ(t1.find("BBB"), 1u);
  EXPECT_EQ(t1.find("CCC"), 2u);
  EXPECT_EQ(a->size(), 14u);
  // Row groups pruned by the ticker statistics still count: every row of the
  // partition is accounted for exactly once.
  EXPECT_EQ(a->rows_read, 22);
  EXPECT_EQ(a->rows_not_selected, 8);
  EXPECT_EQ(a->rows_read, static_cast<std::int64_t>(a->size()) + a->rows_dropped_null +
                              a->rows_dropped_bad_input + a->rows_not_selected);
  const auto again = atx::vol::load_spiderrock_bucket(spec, "1030", t1);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->uid, a->uid); // stable uids across buckets with one table

  UnderlierTable t2;
  const auto b = atx::vol::load_spiderrock_bucket(spec, "1000", t2);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->uid, a->uid);
  EXPECT_EQ(b->strike, a->strike);

  SpiderRockStoreSpec listed = spec_for(root, "2026-08-14");
  listed.underliers = {"DDD", "AAA"};
  UnderlierTable t3;
  const auto c = atx::vol::load_spiderrock_bucket(listed, "1000", t3);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->size(), 8u);
  EXPECT_EQ(t3.find("AAA"), 1u);
  EXPECT_EQ(t3.find("DDD"), 2u);
}

TEST(SpiderRockStore, AmericanOnlyFiltersBeforeTopN) {
  std::vector<Row> rows;
  auto add = [&](const char *tk, const char *ex_type, int n) {
    for (int i = 0; i < n; ++i) {
      Row r;
      r.tk = tk;
      r.ex_type = ex_type;
      r.strike = 50.0 + i;
      rows.push_back(r);
    }
  };
  add("SPX", "European", 9); // the most rows, but European: never in the top-N
  add("AAA", "American", 4);
  add("BBB", "American", 3);
  add("CCC", "American", 2);
  add("MIX", "European", 3);
  add("MIX", "", 1);         // NULL exType: not American
  add("MIX", "american", 2); // case-insensitive
  const fs::path root = fresh_dir("american");
  write_partition(bucket_dir(root, "2026-10-14", "1000"), rows, /*large=*/false, true,
                  /*row_group_rows=*/1024); // one group: no ticker-stats pruning
  ASSERT_FALSE(HasFatalFailure());

  SpiderRockStoreSpec spec = spec_for(root, "2026-10-14");
  spec.max_underliers = 2;
  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec, "1000", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(table.size(), 2u);
  EXPECT_NE(table.find("AAA"), 0u);
  EXPECT_NE(table.find("BBB"), 0u);
  EXPECT_EQ(table.find("SPX"), 0u);
  EXPECT_EQ(b->size(), 7u);
  EXPECT_EQ(b->dropped_non_american, 13);
  EXPECT_EQ(b->rows_not_selected, 4); // CCC and MIX's 2 American rows: outside the top-2
  EXPECT_EQ(b->rows_read, static_cast<std::int64_t>(b->size()) + b->rows_dropped_null +
                              b->rows_dropped_bad_input + b->rows_not_selected +
                              b->dropped_non_american);

  // american_only = false admits every exercise style (and the top-N sees SPX).
  spec.american_only = false;
  UnderlierTable all;
  const auto c = atx::vol::load_spiderrock_bucket(spec, "1000", all);
  ASSERT_TRUE(c.has_value());
  EXPECT_NE(all.find("SPX"), 0u);
  EXPECT_EQ(c->dropped_non_american, 0);
}

TEST(SpiderRockStore, StandardRootsOnlyDropsAdjustedRoots) {
  auto make = [](const char *tk, const char *root, double strike) {
    Row r;
    r.tk = tk;
    r.root = root;
    r.strike = strike;
    return r;
  };
  // AAA1 is an adjusted root: its 100 call collides with AAA's 100 call key.
  const std::vector<Row> rows = {make("AAA", "AAA", 100.0), make("AAA", "AAA1", 100.0),
                                 make("AAA", "AAA1", 105.0), make("BBB", "BBB", 50.0)};
  const fs::path root = fresh_dir("roots");
  write_partition(bucket_dir(root, "2026-10-14", "1000"), rows, /*large=*/true, true, 1024);
  ASSERT_FALSE(HasFatalFailure());

  SpiderRockStoreSpec spec = spec_for(root, "2026-10-14");
  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec, "1000", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(b->size(), 2u);
  EXPECT_EQ(b->dropped_nonstandard_root, 2);
  EXPECT_EQ(b->strike, (std::vector<double>{100.0, 50.0}));
  EXPECT_EQ(b->rows_read, static_cast<std::int64_t>(b->size()) + b->rows_dropped_null +
                              b->rows_dropped_bad_input + b->rows_not_selected +
                              b->dropped_non_american + b->dropped_nonstandard_root);

  spec.standard_roots_only = false;
  UnderlierTable all;
  const auto c = atx::vol::load_spiderrock_bucket(spec, "1000", all);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->size(), 4u);
  EXPECT_EQ(c->dropped_nonstandard_root, 0);
}

// Forward-framework rows carry a per-expiry forward in uPrc (OEX on the licensed drop):
// spot_framework_only drops them after the exType filter and before the top-N count.
TEST(SpiderRockStore, SpotFrameworkOnlyDropsForwardRows) {
  std::vector<Row> rows;
  auto add = [&](const char *tk, const char *ex_type, const char *framework, int n) {
    for (int i = 0; i < n; ++i) {
      Row r;
      r.tk = tk;
      r.ex_type = ex_type;
      r.framework = framework;
      r.strike = 50.0 + i;
      rows.push_back(r);
    }
  };
  add("OEX", "American", "Forward", 5); // the most rows, but a forward in uPrc
  add("SPY", "American", "Spot", 3);
  add("SPY", "American", "spot", 1);    // case-insensitive
  rows.back().dy = 25; // its own expiry: keys stay unique
  add("XAU", "American", "", 1);        // NULL framework: not Spot
  add("IDX", "European", "Forward", 2); // exType runs first: counted as non-American
  const fs::path root = fresh_dir("framework");
  write_partition(bucket_dir(root, "2026-10-14", "1000"), rows, /*large=*/false, true,
                  /*row_group_rows=*/1024); // one group: no ticker-stats pruning
  write_partition(bucket_dir(root, "2026-10-14", "1030"), rows, /*large=*/false,
                  /*licensed_extras=*/false); // no prcFramework column: keep all
  ASSERT_FALSE(HasFatalFailure());

  SpiderRockStoreSpec spec = spec_for(root, "2026-10-14");
  spec.max_underliers = 1;
  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec, "1000", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(table.size(), 1u);
  EXPECT_NE(table.find("SPY"), 0u);
  EXPECT_EQ(b->size(), 4u);
  EXPECT_EQ(b->dropped_forward_framework, 6);
  EXPECT_EQ(b->dropped_non_american, 2);
  EXPECT_EQ(b->rows_read, static_cast<std::int64_t>(b->size()) + b->rows_dropped_null +
                              b->rows_dropped_bad_input + b->rows_not_selected +
                              b->dropped_non_american + b->dropped_nonstandard_root +
                              b->dropped_forward_framework);

  // spot_framework_only = false admits every framework (and the top-1 is OEX).
  spec.spot_framework_only = false;
  UnderlierTable all;
  const auto c = atx::vol::load_spiderrock_bucket(spec, "1000", all);
  ASSERT_TRUE(c.has_value());
  EXPECT_NE(all.find("OEX"), 0u);
  EXPECT_EQ(c->dropped_forward_framework, 0);

  // Column absent: every row is kept by this filter (exType is absent too).
  spec = spec_for(root, "2026-10-14");
  UnderlierTable plain;
  const auto d = atx::vol::load_spiderrock_bucket(spec, "1030", plain);
  ASSERT_TRUE(d.has_value()) << d.error().to_string();
  EXPECT_EQ(d->size(), rows.size());
  EXPECT_EQ(d->dropped_forward_framework, 0);
}

// Row-group pruning and the projected read address parquet LEAF columns by the arrow
// field index, which holds only for a flat schema: a nested column is refused loudly
// (with and without the top-N statistics path), never pruned silently.
TEST(SpiderRockStore, RefusesNestedSchema) {
  const fs::path root = fresh_dir("nested");
  write_partition(bucket_dir(root, "2026-10-14", "1000"), three_rows(), /*large=*/false,
                  /*licensed_extras=*/false, /*row_group_rows=*/3, /*nested_lead=*/true);
  ASSERT_FALSE(HasFatalFailure());
  for (const std::size_t cap : {std::size_t{0}, std::size_t{1}}) {
    SpiderRockStoreSpec spec = spec_for(root, "2026-10-14");
    spec.max_underliers = cap;
    UnderlierTable table;
    const auto b = atx::vol::load_spiderrock_bucket(spec, "1000", table);
    ASSERT_FALSE(b.has_value()) << "cap " << cap;
    EXPECT_EQ(b.error().code(), ErrorCode::InvalidArgument) << b.error().to_string();
    EXPECT_NE(b.error().to_string().find("data.parquet"), std::string::npos)
        << b.error().to_string();
  }
}

// ── SpiderRockStoreReal: the licensed store, env-gated ──────────────────────
//
// Skipped unless ATX_VOL_SPIDERROCK_STORE names the partitioned store root (the
// dir holding date=2026-10-14/). Reads bucket 1000 twice (uncapped, then top-100)
// and builds the state. Never runs in a lane without the licensed data.

// The store root from ATX_VOL_SPIDERROCK_STORE; empty when unset (the _dupenv_s form
// on Windows, as american_discrete_div_reference_matrix_test.cpp reads its switch).
[[nodiscard]] std::string real_store_root() {
#if defined(_WIN32)
  char *value = nullptr;
  std::size_t size = 0U;
  std::string root;
  if (::_dupenv_s(&value, &size, "ATX_VOL_SPIDERROCK_STORE") == 0 && value != nullptr) {
    root = value;
  }
  std::free(value);
  return root;
#else
  const char *value = std::getenv("ATX_VOL_SPIDERROCK_STORE");
  return value != nullptr ? std::string(value) : std::string();
#endif
}

// ── Clock gaps: our T against the vendor's year fractions ───────────────────
//
// Gaps are in days (1 d = 1 / 365.25 y), bucketed by calendar tenor (expiry - ts).

constexpr double kDayNs = 86'400.0 * 1e9;
constexpr double kDaysPerYear = atx::vol::kCalendarYearNs / kDayNs;
constexpr std::size_t kTenorBuckets = 4;
constexpr std::array<double, kTenorBuckets> kTenorUpperDays{
    1.0, 7.0, 30.0, std::numeric_limits<double>::infinity()};
constexpr std::array<const char *, kTenorBuckets> kTenorNames{"<=1d", "1-7d", "7-30d", ">30d"};

[[nodiscard]] double median_copy(std::vector<double> v) {
  if (v.empty()) {
    return kNaN;
  }
  std::sort(v.begin(), v.end());
  const std::size_t m = v.size() / 2;
  return v.size() % 2 == 1 ? v[m] : (v[m - 1] + v[m]) / 2.0;
}

[[nodiscard]] double p99_copy(std::vector<double> v) { // nearest rank
  if (v.empty()) {
    return kNaN;
  }
  std::sort(v.begin(), v.end());
  const auto rank = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(v.size())));
  return v[std::max<std::size_t>(rank, 1) - 1];
}

struct ClockGaps {
  double median_all{kNaN};
  std::array<double, kTenorBuckets> median_by{};
};

// |T(spec) - vendor| over the bucket's rows: prints the median and p99 overall and per tenor
// bucket. Rows whose vendor value is not finite, or that the clock refuses (VolTime outside
// its covered calendar), are counted and skipped.
ClockGaps print_clock_gaps(const SpiderRockBucket &b, const TimeSpec &spec,
                           const std::vector<double> &vendor, const char *label) {
  std::array<std::vector<double>, kTenorBuckets> by;
  std::vector<double> all;
  std::vector<double> signed_all;
  std::size_t refused = 0;
  for (std::size_t r = 0; r < b.size() && r < vendor.size(); ++r) {
    const auto T = atx::vol::time_to_expiry_years(b.ts_ns, b.expiry_ns[r], spec);
    if (!T.has_value() || !std::isfinite(vendor[r])) {
      ++refused;
      continue;
    }
    const double gap = (*T - vendor[r]) * kDaysPerYear;
    const double tenor = static_cast<double>(b.expiry_ns[r] - b.ts_ns) / kDayNs;
    std::size_t k = 0;
    while (k + 1 < kTenorBuckets && tenor > kTenorUpperDays[k]) {
      ++k;
    }
    by[k].push_back(std::fabs(gap));
    all.push_back(std::fabs(gap));
    signed_all.push_back(gap);
  }
  ClockGaps out;
  out.median_all = median_copy(all);
  std::printf("[clock %s] rows=%zu refused=%zu | median |gap| %.4f d, p99 %.4f d, signed "
              "median %+.4f d\n",
              label, all.size(), refused, out.median_all, p99_copy(all),
              median_copy(signed_all));
  for (std::size_t k = 0; k < kTenorBuckets; ++k) {
    out.median_by[k] = median_copy(by[k]);
    std::printf("[clock %s]   tenor %-6s rows=%8zu median |gap| %.4f d  p99 %.4f d\n", label,
                kTenorNames[k], by[k].size(), out.median_by[k], p99_copy(by[k]));
  }
  return out;
}

// One line per expiry: tenor, the vendor's median `years`, and the signed VolTime /
// Calendar365 gaps to it — the view that locates where (which expiry) a clock departs.
void print_expiry_clock_gaps(const SpiderRockBucket &b) {
  std::map<std::int64_t, std::vector<double>> per_expiry;
  for (std::size_t r = 0; r < b.size(); ++r) {
    per_expiry[b.expiry_ns[r]].push_back(b.vendor_T[r]);
  }
  const TimeSpec vol_time{atx::vol::TimeConvention::VolTime};
  for (const auto &[expiry, years] : per_expiry) {
    const double med = median_copy(years);
    const double tv = atx::vol::time_to_expiry_years(b.ts_ns, expiry, vol_time).value_or(kNaN);
    const double tc = atx::vol::time_to_expiry_years(b.ts_ns, expiry, TimeSpec{}).value_or(kNaN);
    std::printf("[clock expiry] %s tenor %7.2f d rows=%6zu years %.8f | VolTime %+.4f d  "
                "Cal365 %+.4f d\n",
                atx::vol::ns_to_iso_date(expiry).c_str(),
                static_cast<double>(expiry - b.ts_ns) / kDayNs, years.size(), med,
                (tv - med) * kDaysPerYear, (tc - med) * kDaysPerYear);
  }
}

TEST(SpiderRockStoreReal, LoadsBucket1000) {
  const std::string root = real_store_root();
  if (root.empty()) {
    GTEST_SKIP() << "ATX_VOL_SPIDERROCK_STORE unset (licensed store not configured)";
  }
  using Clock = std::chrono::steady_clock;
  auto ms_since = [](Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  };
  SpiderRockStoreSpec spec;
  spec.store_root = root;
  spec.date = "2026-10-14";
  UnderlierTable t_all;
  Clock::time_point t0 = Clock::now();
  const auto full = atx::vol::load_spiderrock_bucket(spec, "1000", t_all);
  const double full_ms = ms_since(t0);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();
  std::printf("[real 1000 uncapped] load %.1f ms: read=%lld kept=%zu null=%lld bad=%lld "
              "non_american=%lld forward_framework=%lld nonstandard_root=%lld "
              "not_selected=%lld null_vendor=%lld uids=%zu\n",
              full_ms, static_cast<long long>(full->rows_read), full->size(),
              static_cast<long long>(full->rows_dropped_null),
              static_cast<long long>(full->rows_dropped_bad_input),
              static_cast<long long>(full->dropped_non_american),
              static_cast<long long>(full->dropped_forward_framework),
              static_cast<long long>(full->dropped_nonstandard_root),
              static_cast<long long>(full->rows_not_selected),
              static_cast<long long>(full->rows_null_vendor), t_all.size());
  EXPECT_GT(full->size(), 1'500'000u);
  EXPECT_GT(full->dropped_non_american, 0);
  EXPECT_GT(full->dropped_forward_framework, 0);
  EXPECT_EQ(t_all.find("OEX"), 0u) << "OEX is Forward-framework: spot_framework_only drops it";
  EXPECT_EQ(full->rows_read, static_cast<std::int64_t>(full->size()) + full->rows_dropped_null +
                                 full->rows_dropped_bad_input + full->rows_not_selected +
                                 full->dropped_non_american + full->dropped_nonstandard_root +
                                 full->dropped_forward_framework);

  spec.max_underliers = 100;
  UnderlierTable table;
  t0 = Clock::now();
  const auto top = atx::vol::load_spiderrock_bucket(spec, "1000", table);
  const double top_ms = ms_since(t0);
  ASSERT_TRUE(top.has_value()) << top.error().to_string();
  BucketStateReport report;
  t0 = Clock::now();
  const auto st = atx::vol::market_state_from_bucket(*top, TimeSpec{}, &report);
  const double state_ms = ms_since(t0);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  std::printf("[real 1000 top-100] load %.1f ms, state %.1f ms: kept=%zu null=%lld bad=%lld "
              "non_american=%lld forward_framework=%lld not_selected=%lld | uids=%zu marks=%zu "
              "expiry_rows=%zu uids_with_divs=%zu max_ddiv_spread=%.6g dup_marks=%zu "
              "bad_sigma=%zu\n",
              top_ms, state_ms, top->size(), static_cast<long long>(top->rows_dropped_null),
              static_cast<long long>(top->rows_dropped_bad_input),
              static_cast<long long>(top->dropped_non_american),
              static_cast<long long>(top->dropped_forward_framework),
              static_cast<long long>(top->rows_not_selected), report.n_uids, report.n_marks,
              report.n_expiry_rows, report.n_uids_with_divs, report.max_ddiv_spread,
              report.n_duplicate_marks, report.n_rows_bad_sigma);
  // Clock (follow-up 2, item D): our T against the vendor's `years` on the top-100 marks.
  // Measured here (2026-10-14 1000): VolTime reproduces `years` to <= 0.02 d per expiry up
  // to the first NYSE early close (2026-11-27), then the gap steps by ~0.46 d at every
  // early-close session (VolTimeCalendar holds full closures only), so the overall median
  // is ~0.92 d, above the brief's 0.5 d adoption bar: the bench keeps calendar365 as its
  // default (docs/risk-bench.md "Clock"). Pinned: VolTime stays within 0.05 d on the
  // tenors before the first early close.
  const TimeSpec vol_time{atx::vol::TimeConvention::VolTime};
  const ClockGaps vt = print_clock_gaps(*top, vol_time, top->vendor_T, "VolTime-years");
  (void)print_clock_gaps(*top, TimeSpec{}, top->vendor_T, "Cal365-years");
  print_expiry_clock_gaps(*top);
  for (std::size_t k = 0; k + 1 < kTenorBuckets; ++k) {
    EXPECT_LE(vt.median_by[k], 0.05) << "VolTime vs years, tenor " << kTenorNames[k];
  }
  EXPECT_GE(report.n_uids, 100u);
  EXPECT_GT(top->dropped_non_american, 0);
  EXPECT_GT(report.n_uids_with_divs, 0u);
  EXPECT_EQ(report.n_duplicate_marks, 0u) << "standard roots only: keys must not collide";
  ASSERT_EQ(st->marks.size(), st->underliers.size());
  for (const auto &m : st->marks) {
    EXPECT_GE(m.sigma.size(), 100u) << "uid " << m.uid;
    for (std::size_t i = 1; i < m.sigma.size(); ++i) {
      const auto prev = std::make_tuple(m.expiry_ns[i - 1], static_cast<int>(m.side[i - 1]),
                                        m.strike[i - 1]);
      const auto cur =
          std::make_tuple(m.expiry_ns[i], static_cast<int>(m.side[i]), m.strike[i]);
      ASSERT_LT(prev, cur) << "uid " << m.uid << " row " << i;
    }
  }
}

// ── SyntheticStore ──────────────────────────────────────────────────────────

[[nodiscard]] SyntheticSpiderRockSpec small_spec() {
  SyntheticSpiderRockSpec spec;
  spec.n_underliers = 4;
  spec.n_expiries = 3;
  spec.n_strikes = 5;
  spec.seed = 7;
  return spec;
}

// Reads one double column of a written synthetic partition (the loader does not
// surface `atmVol`; the test reads it to see the ATM level move).
[[nodiscard]] std::vector<double> read_double_column(const fs::path &file, const char *name) {
  std::vector<double> out;
  auto in = arrow::io::ReadableFile::Open(file.string());
  if (!in.ok()) {
    return out;
  }
  auto reader = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
  if (!reader.ok()) {
    return out;
  }
  auto table = (*reader)->ReadTable();
  if (!table.ok()) {
    return out;
  }
  const auto col = (*table)->GetColumnByName(name);
  if (col == nullptr) {
    return out;
  }
  for (const auto &chunk : col->chunks()) {
    const auto &arr = static_cast<const arrow::DoubleArray &>(*chunk);
    for (std::int64_t i = 0; i < arr.length(); ++i) {
      out.push_back(arr.Value(i));
    }
  }
  return out;
}

TEST(SyntheticStore, WritesElevenBuckets) {
  const fs::path root = fresh_dir("eleven");
  const SyntheticSpiderRockSpec spec = small_spec();
  SyntheticStoreReport report;
  const auto st = atx::vol::write_synthetic_spiderrock_store(spec, root.string(), report);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  std::printf("[synthetic] %zu rows in %.3f s (pricing %.3f s)\n", report.rows_written,
              report.seconds, report.pricing_seconds);

  const auto buckets = atx::vol::list_spiderrock_buckets(root.string(), spec.date);
  ASSERT_TRUE(buckets.has_value());
  EXPECT_EQ(*buckets, spec.buckets_et);
  EXPECT_EQ(buckets->size(), 11u);
  EXPECT_EQ(report.files_written, 11u);
  EXPECT_EQ(report.rows_written, 11u * 4u * 3u * 5u * 2u);
  EXPECT_EQ(report.pricing_failures, 0u);

  UnderlierTable table;
  SpiderRockStoreSpec rspec = spec_for(root, spec.date);
  for (const std::string &bucket : *buckets) {
    const auto b = atx::vol::load_spiderrock_bucket(rspec, bucket, table);
    ASSERT_TRUE(b.has_value()) << b.error().to_string();
    EXPECT_EQ(b->size(), 4u * 3u * 5u * 2u) << bucket;
    EXPECT_EQ(b->rows_dropped_null + b->rows_dropped_bad_input, 0) << bucket;
  }
  EXPECT_EQ(table.size(), 4u);

  SyntheticSpiderRockSpec bad = spec;
  bad.n_expiries = 9;
  EXPECT_FALSE(atx::vol::write_synthetic_spiderrock_store(bad, root.string()).has_value());
}

TEST(SyntheticStore, MarketMovesAcrossBuckets) {
  const fs::path root = fresh_dir("moves");
  const SyntheticSpiderRockSpec spec = small_spec();
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(spec, root.string()).has_value());

  UnderlierTable table;
  const SpiderRockStoreSpec rspec = spec_for(root, spec.date);
  std::vector<SpiderRockBucket> buckets;
  for (const std::string &bucket : spec.buckets_et) {
    auto b = atx::vol::load_spiderrock_bucket(rspec, bucket, table);
    ASSERT_TRUE(b.has_value()) << b.error().to_string();
    buckets.push_back(std::move(*b));
  }
  // Strikes are LISTED contracts: identical keys in every bucket.
  for (const SpiderRockBucket &b : buckets) {
    ASSERT_EQ(b.strike, buckets.front().strike);
    ASSERT_EQ(b.expiry_ns, buckets.front().expiry_ns);
  }
  // Per uid, per expiry (rows sorted by uid, expiry, side, strike): the first and
  // last strike of the call block give a skew proxy; atmVol (read from the file)
  // gives the ATM level of every row.
  const std::size_t per_block = spec.n_strikes;
  for (std::size_t k = 1; k < buckets.size(); ++k) {
    const SpiderRockBucket &p = buckets[k - 1];
    const SpiderRockBucket &c = buckets[k];
    for (std::size_t row = 0; row + per_block <= c.size(); row += 2 * per_block) {
      EXPECT_NE(c.uprc[row], p.uprc[row]) << "spot frozen, bucket " << k << " row " << row;
      const double skew_c = c.sigma[row] - c.sigma[row + per_block - 1];
      EXPECT_GT(skew_c, 0.0) << "eSSVI rho < 0 must slope down in strike";
    }
    const auto atm_p = read_double_column(
        bucket_dir(root, spec.date, spec.buckets_et[k - 1]) / "data.parquet", "atmVol");
    const auto atm_c =
        read_double_column(bucket_dir(root, spec.date, spec.buckets_et[k]) / "data.parquet",
                           "atmVol");
    ASSERT_EQ(atm_p.size(), atm_c.size());
    ASSERT_FALSE(atm_c.empty());
    std::size_t frozen = 0;
    for (std::size_t i = 0; i < atm_c.size(); ++i) {
      frozen += atm_c[i] == atm_p[i] ? 1u : 0u;
    }
    EXPECT_EQ(frozen, 0u) << "ATM level frozen on " << frozen << " of " << atm_c.size()
                          << " rows at bucket " << k;
  }
  // The rho shock is live: a store with the same seed and skew_shock_vol = 0 draws the
  // same spot and level paths (every draw is unconditional) and the same first-bucket
  // smiles, and its skews differ from the second bucket on. (The skew proxy alone also
  // moves with spot and T, so comparing buckets of ONE store cannot show this.)
  SyntheticSpiderRockSpec flat_spec = spec;
  flat_spec.skew_shock_vol = 0.0;
  const fs::path flat_root = fresh_dir("moves-flat");
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(flat_spec, flat_root.string())
                  .has_value());
  UnderlierTable flat_table;
  const SpiderRockStoreSpec flat_rspec = spec_for(flat_root, spec.date);
  std::vector<SpiderRockBucket> flat;
  for (const std::string &bucket : spec.buckets_et) {
    auto b = atx::vol::load_spiderrock_bucket(flat_rspec, bucket, flat_table);
    ASSERT_TRUE(b.has_value()) << b.error().to_string();
    flat.push_back(std::move(*b));
  }
  ASSERT_EQ(flat.size(), buckets.size());
  EXPECT_EQ(flat.front().sigma, buckets.front().sigma);
  for (std::size_t k = 0; k < buckets.size(); ++k) {
    EXPECT_EQ(flat[k].uprc, buckets[k].uprc) << "spot path depends on the skew shock " << k;
  }
  for (std::size_t k = 1; k < buckets.size(); ++k) {
    for (std::size_t row = 0; row + per_block <= buckets[k].size(); row += 2 * per_block) {
      const double skew = buckets[k].sigma[row] - buckets[k].sigma[row + per_block - 1];
      const double skew_flat = flat[k].sigma[row] - flat[k].sigma[row + per_block - 1];
      EXPECT_NE(skew, skew_flat) << "rho shock not live, bucket " << k << " row " << row;
    }
  }
  // Time decays with the clock: vendor T shrinks bucket to bucket.
  EXPECT_LT(buckets.back().vendor_T[0], buckets.front().vendor_T[0]);
}

TEST(SyntheticStore, VendorGreeksMatchLibrary) {
  const fs::path root = fresh_dir("vendor");
  const SyntheticSpiderRockSpec spec = small_spec();
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(spec, root.string()).has_value());
  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec_for(root, spec.date), "1230", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  std::size_t checked = 0;
  for (std::size_t i = 0; i < b->size(); i += 7) {
    const double T =
        atx::vol::time_to_expiry_years(b->ts_ns, b->expiry_ns[i], TimeSpec{}).value();
    ASSERT_TRUE(bits_equal(T, b->vendor_T[i])) << "years must be the Calendar365 T, row " << i;
    const double q_eff = atx::vol::spiderrock_q_eff(b->uprc[i], b->ddiv[i], b->rate[i],
                                                    b->sdiv[i], T);
    const auto g = atx::vol::american_greeks_al(b->uprc[i], b->strike[i], T, b->sigma[i],
                                                b->rate[i], q_eff, b->side[i],
                                                atx::vol::al_fast_opts(), true, false, false);
    ASSERT_TRUE(g.has_value()) << g.error().to_string();
    EXPECT_TRUE(bits_equal(g->price, b->vendor_price[i])) << "row " << i;
    EXPECT_TRUE(bits_equal(g->delta, b->vendor_delta[i])) << "row " << i;
    EXPECT_TRUE(bits_equal(g->gamma, b->vendor_gamma[i])) << "row " << i;
    EXPECT_TRUE(bits_equal(g->vega * atx::vol::kSpiderRockVegaScale, b->vendor_vega[i]))
        << "row " << i;
    EXPECT_TRUE(bits_equal(g->theta * atx::vol::kSpiderRockThetaScale, b->vendor_theta[i]))
        << "row " << i;
    ++checked;
  }
  EXPECT_GT(checked, 10u);
  // Some uids carry an escrowed dividend PV (30% target); with 4 uids and seed 7 the
  // store is not required to have one, but the column must be finite and >= 0.
  for (const double d : b->ddiv) {
    EXPECT_TRUE(std::isfinite(d) && d >= 0.0);
  }
}

[[nodiscard]] std::string slurp(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

TEST(SyntheticStore, SeedIsReproducible) {
  const fs::path a = fresh_dir("seed-a");
  const fs::path b = fresh_dir("seed-b");
  const fs::path c = fresh_dir("seed-c");
  SyntheticSpiderRockSpec spec = small_spec();
  spec.buckets_et = {"1000", "1030"};
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(spec, a.string()).has_value());
  spec.n_threads = 1; // the fan-out degree must not change a byte
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(spec, b.string()).has_value());
  spec.seed = 8;
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(spec, c.string()).has_value());
  for (const std::string &bucket : spec.buckets_et) {
    const std::string fa = slurp(bucket_dir(a, spec.date, bucket) / "data.parquet");
    const std::string fb = slurp(bucket_dir(b, spec.date, bucket) / "data.parquet");
    const std::string fc = slurp(bucket_dir(c, spec.date, bucket) / "data.parquet");
    ASSERT_FALSE(fa.empty());
    EXPECT_EQ(fa, fb) << "same seed, bucket " << bucket;
    EXPECT_NE(fa, fc) << "different seed, bucket " << bucket;
  }
}

// The full benchmark-size store (100 uids x 8 expiries x 25 strikes x 2 sides x 11
// buckets = 440k rows). Disabled: it is a timing receipt, not a gate. Run with
//   atx-vol-risk-slice --gtest_also_run_disabled_tests
//                      --gtest_filter=SyntheticStore.DISABLED_FullSizeGenerationTiming
TEST(SyntheticStore, DISABLED_FullSizeGenerationTiming) {
  const fs::path root = fresh_dir("fullsize");
  SyntheticStoreReport report;
  const auto st =
      atx::vol::write_synthetic_spiderrock_store(SyntheticSpiderRockSpec{}, root.string(), report);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  std::printf("[synthetic full-size] rows=%zu files=%zu failures=%zu total=%.3f s pricing=%.3f s\n",
              report.rows_written, report.files_written, report.pricing_failures, report.seconds,
              report.pricing_seconds);
  EXPECT_EQ(report.rows_written, 440000u);
  EXPECT_EQ(report.pricing_failures, 0u);
  std::error_code ec;
  fs::remove_all(root, ec);
}

// ── BucketState ─────────────────────────────────────────────────────────────

TEST(BucketState, StorageHasOneMarksBlockPerUid) {
  const fs::path root = fresh_dir("state");
  const SyntheticSpiderRockSpec spec = small_spec();
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(spec, root.string()).has_value());
  UnderlierTable table;
  const auto b = atx::vol::load_spiderrock_bucket(spec_for(root, spec.date), "1000", table);
  ASSERT_TRUE(b.has_value());
  BucketStateReport report;
  const auto st = atx::vol::market_state_from_bucket(*b, TimeSpec{}, &report);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  const MarketStateStorage &s = *st;
  EXPECT_EQ(s.ts_ns, b->ts_ns);
  ASSERT_EQ(s.underliers.size(), 4u);
  ASSERT_EQ(s.marks.size(), 4u);
  ASSERT_EQ(s.term_carries.size(), 4u);
  EXPECT_EQ(report.n_uids, 4u);
  EXPECT_EQ(report.n_uids_dropped, 0u);
  EXPECT_EQ(report.n_marks, b->size());
  EXPECT_EQ(report.n_expiry_rows, 4u * 3u);
  for (std::size_t u = 0; u < 4; ++u) {
    EXPECT_EQ(s.underliers[u].uid, static_cast<std::uint32_t>(u + 1));
    EXPECT_EQ(s.marks[u].uid, s.underliers[u].uid);
    EXPECT_EQ(s.marks[u].sigma.size(), 3u * 5u * 2u);
    EXPECT_EQ(s.marks[u].vendor_price.size(), s.marks[u].sigma.size());
    EXPECT_EQ(s.marks[u].vendor_delta.size(), s.marks[u].sigma.size());
    EXPECT_EQ(s.underliers[u].term_carry.data(), s.term_carries[u].data());
    EXPECT_EQ(s.underliers[u].term_carry.size(), 3u);
    EXPECT_TRUE(s.underliers[u].cash_divs.empty());
    EXPECT_GT(s.underliers[u].spot, 0.0);
  }
  EXPECT_TRUE(s.surfaces.empty());
}

[[nodiscard]] SpiderRockBucket hand_bucket() {
  SpiderRockBucket b;
  b.ts_ns = atx::vol::spiderrock_bucket_instant_ns("2026-08-14", "1000").value();
  const std::int64_t e1 = atx::vol::expiry_instant_ns("2026-09-18");
  const std::int64_t e2 = atx::vol::expiry_instant_ns("2026-10-16");
  struct R {
    std::uint32_t uid;
    std::int64_t exp;
    Side side;
    double K, sigma, uprc, rate, sdiv, ddiv;
  };
  // Deliberately unsorted, with one exact duplicate key (uid 1, e1, Call, 100).
  const R rows[] = {
      {1, e2, Side::Put, 90.0, 0.30, 100.2, 0.040, 0.010, 0.60},
      {1, e1, Side::Call, 100.0, 0.25, 100.0, 0.041, 0.011, 0.50},
      {1, e1, Side::Call, 95.0, 0.26, 99.9, 0.042, 0.012, 0.52},
      {1, e1, Side::Call, 100.0, 0.99, 100.1, 0.043, 0.013, 0.51},
      {1, e1, Side::Put, 95.0, 0.27, 100.0, 0.044, 0.014, 0.51},
      {2, e1, Side::Call, 50.0, 0.40, 50.0, 0.030, 0.000, 0.00},
      {2, e1, Side::Put, 50.0, 0.41, 50.0, 0.030, 0.000, 0.00},
      {3, e1, Side::Call, 10.0, 0.40, kNaN, 0.030, 0.000, 0.00}, // no finite uPrc
  };
  for (const R &r : rows) {
    b.uid.push_back(r.uid);
    b.expiry_ns.push_back(r.exp);
    b.side.push_back(r.side);
    b.strike.push_back(r.K);
    b.sigma.push_back(r.sigma);
    b.uprc.push_back(r.uprc);
    b.rate.push_back(r.rate);
    b.sdiv.push_back(r.sdiv);
    b.ddiv.push_back(r.ddiv);
    b.vendor_price.push_back(r.K * 0.01);
    b.vendor_delta.push_back(0.5);
    b.vendor_gamma.push_back(0.0);
    b.vendor_vega.push_back(0.0);
    b.vendor_theta.push_back(0.0);
    b.vendor_T.push_back(0.1);
    b.bid.push_back(1.0);
    b.ask.push_back(1.1);
  }
  return b;
}

TEST(BucketState, MarksSortedAndUnique) {
  BucketStateReport report;
  const auto st = atx::vol::market_state_from_bucket(hand_bucket(), TimeSpec{}, &report);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  ASSERT_EQ(st->marks.size(), 2u); // uid 3 dropped (no finite uPrc)
  EXPECT_EQ(report.n_uids, 2u);
  EXPECT_EQ(report.n_uids_dropped, 1u);
  EXPECT_EQ(report.n_duplicate_marks, 1u);
  const auto &m = st->marks[0];
  ASSERT_EQ(m.uid, 1u);
  ASSERT_EQ(m.sigma.size(), 4u);
  for (std::size_t i = 1; i < m.sigma.size(); ++i) {
    const auto prev = std::make_tuple(m.expiry_ns[i - 1], static_cast<int>(m.side[i - 1]),
                                      m.strike[i - 1]);
    const auto cur = std::make_tuple(m.expiry_ns[i], static_cast<int>(m.side[i]), m.strike[i]);
    EXPECT_LT(prev, cur) << "row " << i;
  }
  // The FIRST row of the duplicated key is kept (sigma 0.25, not 0.99).
  EXPECT_EQ(m.strike[1], 100.0);
  EXPECT_EQ(m.sigma[1], 0.25);
  EXPECT_EQ(report.n_marks, 6u);
  // spot = median uPrc over the uid's 5 rows {100.2, 100, 99.9, 100.1, 100}.
  EXPECT_EQ(st->underliers[0].spot, 100.0);
  EXPECT_EQ(st->underliers[1].spot, 50.0);
}

TEST(BucketState, DividendApproximationReported) {
  BucketStateReport report;
  const auto st = atx::vol::market_state_from_bucket(hand_bucket(), TimeSpec{}, &report);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(report.n_uids_with_divs, 1u);
  EXPECT_EQ(report.n_expiry_rows, 3u); // uid 1: e1, e2; uid 2: e1
  // uid 1, e1: ddiv over {0.50, 0.52, 0.51, 0.51} -> spread 0.02, median 0.51.
  EXPECT_NEAR(report.max_ddiv_spread, 0.02, 1e-12);
  const auto &tc = st->term_carries[0];
  ASSERT_EQ(tc.size(), 2u);
  EXPECT_LT(tc[0].expiry_ns, tc[1].expiry_ns);
  EXPECT_EQ(tc[0].pv_divs, 0.51);
  EXPECT_EQ(tc[0].rate, (0.042 + 0.043) / 2.0); // median of 4 rates
  EXPECT_EQ(tc[1].pv_divs, 0.60);
  EXPECT_EQ(tc[1].q, 0.010);
  EXPECT_EQ(st->term_carries[1].size(), 1u);
  EXPECT_EQ(st->term_carries[1][0].pv_divs, 0.0);
  EXPECT_EQ(st->underliers[0].flat_rate, 0.042); // median of 5 rates
  EXPECT_EQ(st->underliers[0].flat_q, 0.012);
  for (const auto &u : st->underliers) {
    EXPECT_TRUE(u.cash_divs.empty()) << "D4 revised: SpiderRock states carry no schedule";
    EXPECT_EQ(u.carry, atx::vol::CarryFrom::State);
  }
  // The approximation is checkable: F = (S - pv) e^{(r - q) T}.
  const double T = 0.25;
  const double qe = atx::vol::spiderrock_q_eff(100.0, 0.51, 0.04, 0.01, T);
  const double F = (100.0 - 0.51) * std::exp((0.04 - 0.01) * T);
  EXPECT_NEAR(100.0 * std::exp((0.04 - qe) * T), F, 1e-12);
  EXPECT_TRUE(std::isnan(atx::vol::spiderrock_q_eff(100.0, 100.0, 0.04, 0.01, T)));
}

} // namespace
