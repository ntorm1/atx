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
#include <utility>
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
  // Optional vendor numerics, also written only with licensed_extras.
  double ddiv_pv = 0.0;
  double syn_spot = 100.0;
  double years_c = 0.1;
  bool null_ddiv_pv = false;
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
  if (licensed_extras) { // the licensed drop's optional numerics
    const std::pair<const char *, double Row::*> extras[] = {
        {"ddivPv", &Row::ddiv_pv}, {"synSpot", &Row::syn_spot}, {"yearsC", &Row::years_c}};
    for (const auto &[name, field] : extras) {
      arrow::DoubleBuilder b;
      for (const Row &r : rows) {
        const bool null_cell = field == &Row::ddiv_pv && r.null_ddiv_pv;
        ASSERT_TRUE((null_cell ? b.AppendNull() : b.Append(r.*field)).ok());
      }
      std::shared_ptr<arrow::Array> arr;
      ASSERT_TRUE(b.Finish(&arr).ok());
      fields.push_back(arrow::field(name, arrow::float64()));
      arrays.push_back(std::move(arr));
    }
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
  EXPECT_EQ(utf8->carry_mode, atx::vol::SpiderRockCarryMode::ImpliedFromCalls); // spec's
  SpiderRockStoreSpec published = spec;
  published.carry_mode = atx::vol::SpiderRockCarryMode::Published;
  EXPECT_EQ(atx::vol::load_spiderrock_bucket(published, "1000", table).value().carry_mode,
            atx::vol::SpiderRockCarryMode::Published);

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

// ddivPv / synSpot / yearsC are OPTIONAL: one value per row when any file of the
// partition has the column (NaN for a NULL / -99 cell and for the rows of a file without
// it), EMPTY when none has it.
TEST(SpiderRockStore, ReadsOptionalVendorColumns) {
  std::vector<Row> rows(3);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rows[i].strike = 100.0 + static_cast<double>(i);
    rows[i].ddiv_pv = 0.5 + 0.1 * static_cast<double>(i);
    rows[i].syn_spot = 100.25;
    rows[i].years_c = 0.1 - 0.01 * static_cast<double>(i);
  }
  rows[1].null_ddiv_pv = true;
  rows[2].syn_spot = -99.0; // vendor sentinel
  const fs::path root = fresh_dir("optional");
  const fs::path with = bucket_dir(root, "2026-10-14", "1000");
  write_partition(with, rows, /*large=*/false, /*licensed_extras=*/true);
  write_partition(bucket_dir(root, "2026-10-14", "1030"), rows, /*large=*/false,
                  /*licensed_extras=*/false);
  // A second file in bucket 1000 WITHOUT the columns (sorted after data.parquet).
  std::vector<Row> later(1);
  later[0].strike = 120.0;
  const fs::path tmp = fresh_dir("optional-tmp");
  write_partition(tmp, later, /*large=*/false, /*licensed_extras=*/false);
  ASSERT_FALSE(HasFatalFailure());
  std::error_code ec;
  fs::copy_file(tmp / "data.parquet", with / "data_1.parquet", ec);
  ASSERT_FALSE(ec) << ec.message();

  UnderlierTable table;
  const SpiderRockStoreSpec spec = spec_for(root, "2026-10-14");
  const auto b = atx::vol::load_spiderrock_bucket(spec, "1000", table);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_EQ(b->size(), 4u);
  ASSERT_EQ(b->ddiv_pv.size(), 4u);
  ASSERT_EQ(b->syn_spot.size(), 4u);
  ASSERT_EQ(b->vendor_years_c.size(), 4u);
  EXPECT_EQ(b->strike, (std::vector<double>{100.0, 101.0, 102.0, 120.0}));
  EXPECT_EQ(b->ddiv_pv[0], 0.5);
  EXPECT_TRUE(std::isnan(b->ddiv_pv[1])); // NULL
  EXPECT_EQ(b->ddiv_pv[2], 0.7);
  EXPECT_TRUE(std::isnan(b->ddiv_pv[3])); // file without the column
  EXPECT_EQ(b->syn_spot[0], 100.25);
  EXPECT_TRUE(std::isnan(b->syn_spot[2])); // -99 sentinel
  EXPECT_EQ(b->vendor_years_c[2], 0.08);
  EXPECT_EQ(b->rows_null_vendor, 0) << "optional columns are not vendor-null accounting";

  const auto plain = atx::vol::load_spiderrock_bucket(spec, "1030", table);
  ASSERT_TRUE(plain.has_value()) << plain.error().to_string();
  EXPECT_EQ(plain->size(), 3u);
  EXPECT_TRUE(plain->ddiv_pv.empty());
  EXPECT_TRUE(plain->syn_spot.empty());
  EXPECT_TRUE(plain->vendor_years_c.empty());
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
  EXPECT_EQ(top->carry_mode, atx::vol::SpiderRockCarryMode::ImpliedFromCalls); // the default
  BucketStateReport report;
  std::vector<atx::vol::SpiderRockExpiryForward> forwards;
  t0 = Clock::now();
  const auto st = atx::vol::market_state_from_bucket(*top, TimeSpec{}, &report, &forwards);
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
  // Cross-check against the vendor's `yearsC` (equal to `years` except 0 on the
  // same-day expiry on this drop).
  ASSERT_EQ(top->vendor_years_c.size(), top->size()) << "the licensed drop has yearsC";
  (void)print_clock_gaps(*top, vol_time, top->vendor_years_c, "VolTime-yearsC");
  (void)print_clock_gaps(*top, TimeSpec{}, top->vendor_years_c, "Cal365-yearsC");
  print_expiry_clock_gaps(*top);
  // Lane E (design D22): the loader's default clock is VendorYears, so every expiry row
  // carries T_override = the median `years` of its rows. Per expiry, |T_override - median
  // years| is 0 by construction (asserted); per row, |T_override - years| shows how far one
  // expiry's rows disagree among themselves (printed).
  EXPECT_EQ(top->clock, atx::vol::SpiderRockClock::VendorYears);
  EXPECT_EQ(report.n_expiries_vendor_T, report.n_expiry_rows);
  {
    double max_expiry_gap = 0.0;
    std::size_t n_overrides = 0;
    std::vector<double> row_gaps;
    row_gaps.reserve(top->size());
    for (std::size_t i = 0; i < st->underliers.size(); ++i) {
      const std::vector<atx::vol::ExpiryCarry> &rows = st->term_carries[i];
      for (const atx::vol::ExpiryCarry &c : rows) {
        n_overrides += std::isfinite(c.T_override) ? 1u : 0u;
      }
      const std::uint32_t uid = st->underliers[i].uid;
      for (std::size_t r = 0; r < top->size(); ++r) {
        if (top->uid[r] != uid) {
          continue;
        }
        const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto &c) {
          return c.expiry_ns == top->expiry_ns[r];
        });
        ASSERT_NE(it, rows.end());
        row_gaps.push_back(std::fabs(it->T_override - top->vendor_T[r]) * kDaysPerYear);
      }
    }
    for (const auto &f : forwards) {
      const std::size_t i = static_cast<std::size_t>(
          std::find_if(st->underliers.begin(), st->underliers.end(),
                       [&](const auto &u) { return u.uid == f.uid; }) -
          st->underliers.begin());
      ASSERT_LT(i, st->term_carries.size());
      const auto &rows = st->term_carries[i];
      const auto it = std::find_if(rows.begin(), rows.end(),
                                   [&](const auto &c) { return c.expiry_ns == f.expiry_ns; });
      ASSERT_NE(it, rows.end());
      max_expiry_gap = std::max(max_expiry_gap, std::fabs(it->T_override - f.T_vendor));
    }
    const std::size_t rows_off = static_cast<std::size_t>(
        std::count_if(row_gaps.begin(), row_gaps.end(), [](double g) { return g != 0.0; }));
    std::printf("[clock vendor] expiry rows %zu, with T_override %zu | per expiry max "
                "|T_override - median years| %.3g d | per row |T_override - years| median %.3g "
                "d, p99 %.3g d, max %.3g d, rows off %zu of %zu\n",
                report.n_expiry_rows, n_overrides, max_expiry_gap * kDaysPerYear,
                median_copy(row_gaps), p99_copy(row_gaps),
                row_gaps.empty() ? 0.0 : *std::max_element(row_gaps.begin(), row_gaps.end()),
                rows_off, row_gaps.size());
    EXPECT_EQ(n_overrides, report.n_expiry_rows);
    EXPECT_EQ(max_expiry_gap, 0.0) << "T_override is the median years by construction";
  }
  EXPECT_TRUE(report.pv_divs_from_ddiv_pv) << "the licensed drop has ddivPv";
  ASSERT_EQ(top->ddiv_pv.size(), top->size());
  ASSERT_EQ(top->syn_spot.size(), top->size());

  // Carry implied from the vendor's own OTM calls (follow-up 2, item C).
  std::printf("[carry] implied=%zu published=%zu (rows %zu) max |F_impl/F_pub - 1| = %.6f, "
              "implied-carry time %.2f ms\n",
              report.n_expiries_implied, report.n_expiries_published, report.n_expiry_rows,
              report.max_implied_vs_published_forward_rel, report.implied_carry_seconds * 1e3);
  EXPECT_EQ(report.n_expiries_implied + report.n_expiries_published, report.n_expiry_rows);
  EXPECT_GT(report.n_expiries_implied, report.n_expiries_published);
  ASSERT_EQ(forwards.size(), report.n_expiry_rows);
  std::vector<const atx::vol::SpiderRockExpiryForward *> implied;
  std::size_t few_calls = 0;
  for (const auto &f : forwards) {
    if (f.mode == atx::vol::SpiderRockCarryMode::ImpliedFromCalls) {
      implied.push_back(&f);
    } else {
      few_calls += f.n_calls < atx::vol::kImpliedCarryMinCalls ? 1u : 0u;
    }
  }
  std::printf("[carry] published fallbacks: %zu with < %zu calls, %zu on the IQR guard\n",
              few_calls, atx::vol::kImpliedCarryMinCalls,
              report.n_expiries_published - few_calls);
  auto gap = [](const atx::vol::SpiderRockExpiryForward *f) {
    return std::fabs(f->implied_forward / f->published_forward - 1.0);
  };
  std::sort(implied.begin(), implied.end(),
            [&](const auto *a, const auto *b) { return gap(a) > gap(b); });
  for (std::size_t i = 0; i < implied.size() && i < 8; ++i) {
    const auto *f = implied[i];
    std::printf("[carry] gap #%zu %-5s %s F_impl %.4f F_pub %.4f (%+.3f %%) calls=%zu iqr=%.2e\n",
                i + 1, std::string(table.ticker(f->uid)).c_str(),
                atx::vol::ns_to_iso_date(f->expiry_ns).c_str(), f->implied_forward,
                f->published_forward, 100.0 * (f->implied_forward / f->published_forward - 1.0),
                f->n_calls, f->iqr_rel);
  }
  // DIA 2028-12-15: the vendor's calls imply F = 562.2 (the brief's measurement).
  const std::uint32_t dia = table.find("DIA");
  ASSERT_NE(dia, 0u) << "DIA is in the top-100";
  const std::int64_t dia_expiry = atx::vol::expiry_instant_ns("2028-12-15");
  const auto dia_it = std::find_if(forwards.begin(), forwards.end(), [&](const auto &f) {
    return f.uid == dia && f.expiry_ns == dia_expiry;
  });
  ASSERT_NE(dia_it, forwards.end());
  std::printf("[carry] DIA 2028-12-15: F_impl %.4f F_pub %.4f mode %s calls=%zu iqr=%.2e\n",
              dia_it->implied_forward, dia_it->published_forward,
              dia_it->mode == atx::vol::SpiderRockCarryMode::ImpliedFromCalls ? "implied"
                                                                              : "published",
              dia_it->n_calls, dia_it->iqr_rel);
  EXPECT_EQ(dia_it->mode, atx::vol::SpiderRockCarryMode::ImpliedFromCalls);
  EXPECT_NEAR(dia_it->implied_forward / 562.2 - 1.0, 0.0, 1e-3);
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
  EXPECT_FALSE(report.pv_divs_from_ddiv_pv) << "no ddivPv column: pv_divs from ddiv";
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

  // With the ddivPv column, pv_divs is ITS median per expiry (D4 wants a PRESENT value;
  // ddiv is the undiscounted amount); an expiry whose ddivPv cells are all NULL (uid 2,
  // given a nonzero ddiv {0.30, 0.40} here) falls back to the ddiv median. The spread is
  // measured on the column each expiry used: uid 2's ddiv spread 0.10 beats uid 1's
  // ddivPv spread 0.03.
  SpiderRockBucket pv = hand_bucket();
  pv.ddiv_pv = {0.58, 0.48, 0.50, 0.49, 0.47, kNaN, kNaN, 0.0};
  pv.ddiv[5] = 0.30;
  pv.ddiv[6] = 0.40;
  BucketStateReport pv_report;
  const auto st_pv = atx::vol::market_state_from_bucket(pv, TimeSpec{}, &pv_report);
  ASSERT_TRUE(st_pv.has_value()) << st_pv.error().to_string();
  EXPECT_TRUE(pv_report.pv_divs_from_ddiv_pv);
  const auto &tc_pv = st_pv->term_carries[0];
  ASSERT_EQ(tc_pv.size(), 2u);
  EXPECT_EQ(tc_pv[0].pv_divs, (0.48 + 0.49) / 2.0); // median of {0.48, 0.50, 0.49, 0.47}
  EXPECT_EQ(tc_pv[1].pv_divs, 0.58);
  EXPECT_EQ(st_pv->term_carries[1][0].pv_divs, (0.30 + 0.40) / 2.0); // ddiv fallback
  EXPECT_NEAR(pv_report.max_ddiv_spread, 0.10, 1e-12);
  SpiderRockBucket bad = pv;
  bad.ddiv_pv.pop_back();
  EXPECT_EQ(atx::vol::market_state_from_bucket(bad, TimeSpec{}).error().code(),
            ErrorCode::InvalidArgument);
}

// ── BucketState: carry implied from the vendor's calls ─────────────────────

using atx::vol::SpiderRockCarryMode;
using atx::vol::SpiderRockExpiryForward;

// European Black call on the forward: e^{-rT} [F N(d1) - K N(d2)].
[[nodiscard]] double black_call(double F, double K, double T, double sigma, double r) {
  const auto ncdf = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
  const double sd = sigma * std::sqrt(T);
  const double d1 = (std::log(F / K) + 0.5 * sd * sd) / sd;
  return std::exp(-r * T) * (F * ncdf(d1) - K * ncdf(d1 - sd));
}

// One uid (spot 100, rate 0.04, published sdiv 0.01 and ddiv 1.0) and one expiry whose
// vendor OTM call prices are European Black at the KNOWN forward `F` (a basis the
// published tuple does not give) on the vendor clock `vendor_T` (default: the Calendar365
// T of the bucket), which is also the rows' `years`. Calls at strikes 100, 105, ...
// (`n_calls` of them, a mild skew in srVol), plus rows the implied carry must ignore: an
// ITM call (K < spot) and a put, both priced at a wrong forward, and an OTM call without
// a bid.
struct ImpliedFixture {
  SpiderRockBucket bucket;
  double T{0.0};        // Calendar365 T of the expiry from the bucket instant
  double vendor_T{0.0}; // the clock the vendor prices (and `years`) are on
};

[[nodiscard]] ImpliedFixture implied_bucket(double F, std::size_t n_calls,
                                            std::string_view expiry_date = "2027-01-15",
                                            double vendor_T = kNaN) {
  ImpliedFixture fx;
  SpiderRockBucket &b = fx.bucket;
  b.ts_ns = atx::vol::spiderrock_bucket_instant_ns("2026-10-14", "1000").value();
  const std::int64_t expiry = atx::vol::expiry_instant_ns(expiry_date);
  fx.T = atx::vol::time_to_expiry_years(b.ts_ns, expiry, TimeSpec{}).value();
  fx.vendor_T = std::isnan(vendor_T) ? fx.T : vendor_T;
  constexpr double kRate = 0.04;
  auto add = [&](Side side, double K, double sigma, double price, double bid) {
    b.uid.push_back(1);
    b.expiry_ns.push_back(expiry);
    b.side.push_back(side);
    b.strike.push_back(K);
    b.sigma.push_back(sigma);
    b.uprc.push_back(100.0);
    b.rate.push_back(kRate);
    b.sdiv.push_back(0.01);
    b.ddiv.push_back(1.0);
    b.vendor_price.push_back(price);
    b.vendor_delta.push_back(0.5);
    b.vendor_gamma.push_back(0.0);
    b.vendor_vega.push_back(0.0);
    b.vendor_theta.push_back(0.0);
    b.vendor_T.push_back(fx.vendor_T);
    b.bid.push_back(bid);
    b.ask.push_back(price * 1.05 + 0.01);
  };
  const double Tv = fx.vendor_T;
  const double wrong = 0.9 * F; // the forward the ignored rows are priced at
  add(Side::Call, 95.0, 0.27, black_call(wrong, 95.0, Tv, 0.27, kRate), 1.0); // ITM
  add(Side::Put, 100.0, 0.26, black_call(wrong, 100.0, Tv, 0.26, kRate), 1.0);
  for (std::size_t i = 0; i < n_calls; ++i) {
    const double K = 100.0 + 5.0 * static_cast<double>(i);
    const double sigma = 0.25 - 0.004 * static_cast<double>(i);
    const double price = black_call(F, K, Tv, sigma, kRate);
    add(Side::Call, K, sigma, price, 0.95 * price);
  }
  add(Side::Call, 200.0, 0.3, black_call(wrong, 200.0, Tv, 0.3, kRate), 0.0); // no bid
  b.carry_mode = SpiderRockCarryMode::ImpliedFromCalls;
  return fx;
}

TEST(BucketState, ImpliedCarryReproducesVendorForward) {
  EXPECT_EQ(SpiderRockBucket{}.carry_mode, SpiderRockCarryMode::Published);
  EXPECT_EQ(SpiderRockStoreSpec{}.carry_mode, SpiderRockCarryMode::ImpliedFromCalls);
  constexpr double kF = 101.5;
  const ImpliedFixture fx = implied_bucket(kF, 5);
  const double F_pub = (100.0 - 1.0) * std::exp((0.04 - 0.01) * fx.T);
  ASSERT_GT(std::fabs(kF / F_pub - 1.0), 1e-3) << "the fixture's basis must differ";

  BucketStateReport report;
  std::vector<SpiderRockExpiryForward> forwards;
  const auto st = atx::vol::market_state_from_bucket(fx.bucket, TimeSpec{}, &report, &forwards);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  ASSERT_EQ(st->term_carries.size(), 1u);
  ASSERT_EQ(st->term_carries[0].size(), 1u);
  const atx::vol::ExpiryCarry &c = st->term_carries[0][0];
  EXPECT_EQ(c.pv_divs, 0.0);
  EXPECT_EQ(c.rate, 0.04);
  const double spot = st->underliers[0].spot;
  EXPECT_EQ(spot, 100.0);
  EXPECT_NEAR(spot * std::exp((c.rate - c.q) * fx.T) / kF - 1.0, 0.0, 1e-8);
  EXPECT_EQ(report.n_expiries_implied, 1u);
  EXPECT_EQ(report.n_expiries_published, 0u);
  EXPECT_EQ(report.n_expiry_rows, 1u);
  EXPECT_NEAR(report.max_implied_vs_published_forward_rel, std::fabs(kF / F_pub - 1.0), 1e-8);
  EXPECT_GE(report.implied_carry_seconds, 0.0);
  ASSERT_EQ(forwards.size(), 1u);
  EXPECT_EQ(forwards[0].uid, 1u);
  EXPECT_EQ(forwards[0].expiry_ns, st->term_carries[0][0].expiry_ns);
  EXPECT_EQ(forwards[0].mode, SpiderRockCarryMode::ImpliedFromCalls);
  EXPECT_EQ(forwards[0].n_calls, 5u) << "ITM call, put and no-bid call are not used";
  EXPECT_NEAR(forwards[0].implied_forward / kF - 1.0, 0.0, 1e-9);
  EXPECT_NEAR(forwards[0].published_forward / F_pub - 1.0, 0.0, 1e-12);
  EXPECT_LT(forwards[0].iqr_rel, 1e-9);

  // The same bucket under Published keeps the D4 row.
  SpiderRockBucket published = fx.bucket;
  published.carry_mode = SpiderRockCarryMode::Published;
  BucketStateReport pub_report;
  std::vector<SpiderRockExpiryForward> pub_forwards;
  const auto pub = atx::vol::market_state_from_bucket(published, TimeSpec{}, &pub_report,
                                                      &pub_forwards);
  ASSERT_TRUE(pub.has_value()) << pub.error().to_string();
  EXPECT_EQ(pub->term_carries[0][0].pv_divs, 1.0);
  EXPECT_EQ(pub->term_carries[0][0].q, 0.01);
  EXPECT_EQ(pub_report.n_expiries_implied, 0u);
  EXPECT_EQ(pub_report.n_expiries_published, 1u);
  ASSERT_EQ(pub_forwards.size(), 1u);
  EXPECT_EQ(pub_forwards[0].mode, SpiderRockCarryMode::Published);
  EXPECT_EQ(pub_forwards[0].n_calls, 0u) << "Published mode inverts nothing";
}

// The inversion runs on the vendor's `years`; the row's q is on the STATE's clock, so the
// engine's spot e^{(rate - q) T_state} reproduces the implied F whatever the two clocks
// are: (a) a VolTime state over Calendar365 vendor years, (b) a Calendar365 state over
// vendor years two days longer.
TEST(BucketState, ImpliedCarryUsesTheStateClock) {
  constexpr double kF = 101.5;
  const TimeSpec vol_time{atx::vol::TimeConvention::VolTime};
  struct Case {
    const char *name;
    ImpliedFixture fx;
    TimeSpec time;
  };
  const ImpliedFixture cal = implied_bucket(kF, 5);
  const Case cases[] = {
      {"VolTime state, Calendar365 years", cal, vol_time},
      {"Calendar365 state, years + 2 d", implied_bucket(kF, 5, "2027-01-15", cal.T + 2.0 / 365.25),
       TimeSpec{}},
  };
  for (const Case &k : cases) {
    const SpiderRockBucket &b = k.fx.bucket;
    const double T_state =
        atx::vol::time_to_expiry_years(b.ts_ns, b.expiry_ns.front(), k.time).value();
    ASSERT_GT(std::fabs(T_state - k.fx.vendor_T), 0.25 / 365.25) << k.name; // > 6 h
    BucketStateReport report;
    std::vector<SpiderRockExpiryForward> forwards;
    const auto st = atx::vol::market_state_from_bucket(b, k.time, &report, &forwards);
    ASSERT_TRUE(st.has_value()) << k.name << ": " << st.error().to_string();
    ASSERT_EQ(forwards.size(), 1u) << k.name;
    EXPECT_EQ(forwards[0].mode, SpiderRockCarryMode::ImpliedFromCalls) << k.name;
    const atx::vol::ExpiryCarry &c = st->term_carries[0][0];
    const double spot = st->underliers[0].spot;
    const double engine_F = spot * std::exp((c.rate - c.q) * T_state);
    EXPECT_NEAR(engine_F / forwards[0].implied_forward - 1.0, 0.0, 1e-12) << k.name;
    EXPECT_NEAR(engine_F / kF - 1.0, 0.0, 1e-9) << k.name;
    // On the vendor clock the same q would NOT give F: the clock is the state's.
    EXPECT_GT(std::fabs(spot * std::exp((c.rate - c.q) * k.fx.vendor_T) / kF - 1.0), 1e-6)
        << k.name;
  }
}

// An accepted implied forward still falls back to the Published row when the state's
// clock has no positive T for the expiry: (a) VolTime refuses an expiry beyond its
// calendar (2033), (b) an expiry before the bucket instant (vendor years kept positive).
TEST(BucketState, ImpliedCarryFallsBackWhenTheStateClockRefuses) {
  constexpr double kF = 101.5;
  struct Case {
    const char *name;
    ImpliedFixture fx;
    TimeSpec time;
  };
  const Case cases[] = {
      {"beyond the VolTime calendar", implied_bucket(kF, 5, "2033-01-21"),
       TimeSpec{atx::vol::TimeConvention::VolTime}},
      {"expiry before the bucket", implied_bucket(kF, 5, "2026-10-09", 0.1), TimeSpec{}},
  };
  for (const Case &k : cases) {
    const SpiderRockBucket &b = k.fx.bucket;
    const auto T_state = atx::vol::time_to_expiry_years(b.ts_ns, b.expiry_ns.front(), k.time);
    ASSERT_TRUE(!T_state.has_value() || *T_state <= 0.0) << k.name;
    BucketStateReport report;
    std::vector<SpiderRockExpiryForward> forwards;
    const auto st = atx::vol::market_state_from_bucket(b, k.time, &report, &forwards);
    ASSERT_TRUE(st.has_value()) << k.name << ": " << st.error().to_string();
    ASSERT_EQ(forwards.size(), 1u) << k.name;
    EXPECT_EQ(forwards[0].mode, SpiderRockCarryMode::Published) << k.name;
    EXPECT_EQ(forwards[0].n_calls, 5u) << k.name;
    EXPECT_NEAR(forwards[0].implied_forward / kF - 1.0, 0.0, 1e-9) << k.name << ": solved";
    EXPECT_TRUE(std::isnan(forwards[0].published_forward)) << k.name;
    EXPECT_EQ(st->term_carries[0][0].pv_divs, 1.0) << k.name; // the Published (D4) row
    EXPECT_EQ(st->term_carries[0][0].q, 0.01) << k.name;
    EXPECT_EQ(report.n_expiries_implied, 0u) << k.name;
    EXPECT_EQ(report.n_expiries_published, 1u) << k.name;
  }
}

TEST(BucketState, ImpliedCarryFallsBackWhenCallsAreFew) {
  const ImpliedFixture fx = implied_bucket(101.5, 2);
  BucketStateReport report;
  std::vector<SpiderRockExpiryForward> forwards;
  const auto st = atx::vol::market_state_from_bucket(fx.bucket, TimeSpec{}, &report, &forwards);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  const atx::vol::ExpiryCarry &c = st->term_carries[0][0];
  EXPECT_EQ(c.pv_divs, 1.0); // the Published (D4) row
  EXPECT_EQ(c.q, 0.01);
  EXPECT_EQ(report.n_expiries_implied, 0u);
  EXPECT_EQ(report.n_expiries_published, 1u);
  EXPECT_EQ(report.max_implied_vs_published_forward_rel, 0.0);
  ASSERT_EQ(forwards.size(), 1u);
  EXPECT_EQ(forwards[0].mode, SpiderRockCarryMode::Published);
  EXPECT_EQ(forwards[0].n_calls, 2u);
  EXPECT_LT(forwards[0].n_calls, atx::vol::kImpliedCarryMinCalls);
  EXPECT_NEAR(forwards[0].implied_forward / 101.5 - 1.0, 0.0, 1e-9) << "solved, not used";
}

TEST(BucketState, ImpliedCarryRejectsInconsistentCalls) {
  ImpliedFixture fx = implied_bucket(101.5, 4);
  // Corrupt the highest-strike call of the four (rows: ITM call, put, 4 calls, no-bid).
  SpiderRockBucket &b = fx.bucket;
  b.vendor_price[5] *= 1.3;
  b.ask[5] *= 1.3;
  BucketStateReport report;
  std::vector<SpiderRockExpiryForward> forwards;
  const auto st = atx::vol::market_state_from_bucket(b, TimeSpec{}, &report, &forwards);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->term_carries[0][0].pv_divs, 1.0); // Published
  EXPECT_EQ(report.n_expiries_implied, 0u);
  EXPECT_EQ(report.n_expiries_published, 1u);
  ASSERT_EQ(forwards.size(), 1u);
  EXPECT_EQ(forwards[0].mode, SpiderRockCarryMode::Published);
  EXPECT_EQ(forwards[0].n_calls, 4u);
  EXPECT_GT(forwards[0].iqr_rel, atx::vol::kImpliedCarryMaxIqrRel);
}

// ── BucketState: the vendor clock (lane E, design D22) ─────────────────────

using atx::vol::SpiderRockClock;

// Under SpiderRockClock::VendorYears every ExpiryCarry row carries T_override = the median
// of its rows' `years` (finite > 0), the state prices the expiry at that T, and the implied
// carry inverts AND expresses q on that one clock; under State (a hand-built bucket's
// default) T_override stays NaN while the diagnostics still report the median. The loader
// copies the spec's clock (VendorYears by default).
TEST(BucketState, VendorYearsFillsExpiryTOverride) {
  EXPECT_EQ(SpiderRockBucket{}.clock, SpiderRockClock::State);
  EXPECT_EQ(SpiderRockStoreSpec{}.clock, SpiderRockClock::VendorYears);
  // hand_bucket's rows: uid 1 e2 put; uid 1 e1 C100, C95, C100 (dup key), P95; uid 2 e1
  // C50, P50; uid 3 (no finite uPrc).
  SpiderRockBucket b = hand_bucket();
  b.vendor_T = {0.20, 0.10, 0.13, 0.11, 0.12, 0.105, 0.115, 0.3};
  const double med_u1_e1 = (0.11 + 0.12) / 2.0; // every row of the run, the dup included
  const double med_u2_e1 = (0.105 + 0.115) / 2.0;

  BucketStateReport rep;
  std::vector<SpiderRockExpiryForward> fw;
  const auto st = atx::vol::market_state_from_bucket(b, TimeSpec{}, &rep, &fw);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(rep.n_expiries_vendor_T, 0u);
  ASSERT_EQ(fw.size(), 3u);
  EXPECT_EQ(fw[0].T_vendor, med_u1_e1);
  EXPECT_EQ(fw[1].T_vendor, 0.20);
  EXPECT_EQ(fw[2].T_vendor, med_u2_e1);
  for (const auto &rows : st->term_carries) {
    for (const atx::vol::ExpiryCarry &c : rows) {
      EXPECT_TRUE(std::isnan(c.T_override)) << "State clock: no override";
    }
  }

  b.clock = SpiderRockClock::VendorYears;
  const auto vt = atx::vol::market_state_from_bucket(b, TimeSpec{}, &rep, &fw);
  ASSERT_TRUE(vt.has_value()) << vt.error().to_string();
  EXPECT_EQ(rep.n_expiries_vendor_T, 3u);
  ASSERT_EQ(vt->term_carries.size(), 2u);
  ASSERT_EQ(vt->term_carries[0].size(), 2u);
  EXPECT_EQ(vt->term_carries[0][0].T_override, med_u1_e1);
  EXPECT_EQ(vt->term_carries[0][1].T_override, 0.20);
  EXPECT_EQ(vt->term_carries[1][0].T_override, med_u2_e1);
  ASSERT_EQ(fw.size(), 3u);
  EXPECT_EQ(fw[0].T_vendor, med_u1_e1);
  // The state prices the expiry at its override.
  MarketStateStorage storage = std::move(*vt);
  for (std::size_t i = 0; i < storage.underliers.size(); ++i) {
    storage.underliers[i].term_carry = storage.term_carries[i];
  }
  const auto state = atx::vol::make_market_state(storage);
  ASSERT_TRUE(state.has_value()) << state.error().to_string();
  const auto rc = state->resolve(1u, 95.0, storage.term_carries[0][0].expiry_ns, Side::Call);
  ASSERT_EQ(rc.status, atx::vol::RiskLaneStatus::Ok);
  EXPECT_EQ(rc.T, med_u1_e1);

  // No usable years: a non-positive median, or no column at all, keeps the TimeSpec clock.
  SpiderRockBucket neg = b;
  neg.vendor_T[0] = -0.01; // uid 1 e2's only row
  const auto st_neg = atx::vol::market_state_from_bucket(neg, TimeSpec{}, &rep);
  ASSERT_TRUE(st_neg.has_value());
  EXPECT_TRUE(std::isnan(st_neg->term_carries[0][1].T_override));
  EXPECT_EQ(st_neg->term_carries[0][0].T_override, med_u1_e1);
  EXPECT_EQ(rep.n_expiries_vendor_T, 2u);
  SpiderRockBucket none = b;
  none.vendor_T.clear();
  const auto st_none = atx::vol::market_state_from_bucket(none, TimeSpec{}, &rep);
  ASSERT_TRUE(st_none.has_value());
  EXPECT_EQ(rep.n_expiries_vendor_T, 0u);

  // The implied carry on the vendor clock: years two days longer than the calendar T; the
  // row's q reproduces F at T_override (the clock the kernel prices on), and the Published
  // diagnostic forward is on it too.
  constexpr double kF = 101.5;
  const ImpliedFixture cal = implied_bucket(kF, 5);
  ImpliedFixture fx = implied_bucket(kF, 5, "2027-01-15", cal.T + 2.0 / 365.25);
  fx.bucket.clock = SpiderRockClock::VendorYears;
  const auto im = atx::vol::market_state_from_bucket(fx.bucket, TimeSpec{}, &rep, &fw);
  ASSERT_TRUE(im.has_value()) << im.error().to_string();
  ASSERT_EQ(fw.size(), 1u);
  EXPECT_EQ(fw[0].mode, SpiderRockCarryMode::ImpliedFromCalls);
  const atx::vol::ExpiryCarry &c = im->term_carries[0][0];
  EXPECT_EQ(c.T_override, fx.vendor_T);
  EXPECT_EQ(fw[0].T_vendor, fx.vendor_T);
  const double spot = im->underliers[0].spot;
  EXPECT_NEAR(spot * std::exp((c.rate - c.q) * c.T_override) / kF - 1.0, 0.0, 1e-9);
  EXPECT_NEAR(fw[0].published_forward /
                      ((100.0 - 1.0) * std::exp((0.04 - 0.01) * fx.vendor_T)) - 1.0,
              0.0, 1e-12);

  // The loader copies the spec's clock into the bucket.
  const fs::path root = fresh_dir("clock");
  SyntheticSpiderRockSpec syn = small_spec();
  syn.buckets_et = {"1000"};
  ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(syn, root.string()).has_value());
  UnderlierTable t1;
  const auto loaded = atx::vol::load_spiderrock_bucket(spec_for(root, syn.date), "1000", t1);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(loaded->clock, SpiderRockClock::VendorYears);
  SpiderRockStoreSpec state_spec = spec_for(root, syn.date);
  state_spec.clock = SpiderRockClock::State;
  UnderlierTable t2;
  EXPECT_EQ(atx::vol::load_spiderrock_bucket(state_spec, "1000", t2).value().clock,
            SpiderRockClock::State);
  std::error_code ec;
  fs::remove_all(root, ec);
}

} // namespace
