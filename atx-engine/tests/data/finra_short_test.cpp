// atx::engine::data — FINRA short-interest loader tests (Track B1).
//
// Suite: FinraShort
//
// All fixtures are SYNTHETIC parquet authored in-process with
// atx::core::io::write_parquet (no real FINRA download). Each test stages one or
// more date=YYYY-MM-DD/part-00000.parquet partitions under a fresh temp dir and
// drives load_finra_features on a hand-built panel axis (epoch-day dates +
// ticker->column map), so the causality / forward-fill / derived-math contracts
// are verified deterministically.
//
// settlement_date is written as an Arrow TIMESTAMP (write_parquet has no date32
// writer); the loader accepts either DATE32 (real downloader) or TIMESTAMP
// (synthetic) and normalizes both to epoch-days, so these fixtures exercise the
// SAME causal placement code path the real partitions hit.
//
// Tests:
//   1. CausalityNoLookAhead       — a value is NaN on every panel date strictly
//                                    before publish_day, present on/after, and
//                                    forward-filled until the next obs supersedes.
//   2. DerivedFieldMath           — si_dtc == days_to_cover; si_util == short/shares;
//                                    si_chg == change_percent at a known cell.
//   3. UtilAdvFallback            — with no shares, si_util == short/ADV.
//   4. MissingCoverageStaysNaN    — a panel instrument absent from FINRA is NaN on
//                                    all dates; a FINRA symbol absent from the map
//                                    is dropped (never misplaced onto a column).

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <filesystem>

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/finra_short.hpp"

namespace atxtest_finra_short {

namespace fs = std::filesystem;
using atx::core::time::Timestamp;
using atx::engine::data::DateKey;
using atx::engine::data::FinraFeatures;
using atx::engine::data::InstKey;
using atx::engine::data::load_finra_features;

namespace {

constexpr atx::i64 kNsPerDay = 86'400LL * 1'000'000'000LL;

// A single FINRA row to materialize into a synthetic partition.
struct FinraRow {
  std::string symbol;
  atx::i64 settle_day; // epoch-day of settlement_date
  atx::i64 current_short;
  atx::i64 avg_daily_volume;
  atx::f64 days_to_cover;
  atx::f64 change_percent;
};

// Stage one date=YYYY-MM-DD/part-00000.parquet partition under `root`. The
// settlement_date column is a TIMESTAMP at midnight UTC (write_parquet has no
// date32 writer); the partition dir name is cosmetic — the loader reads dates
// from the column, not the path.
[[nodiscard]] bool write_partition(const fs::path& root, const std::string& date_dir,
                                   const std::vector<FinraRow>& rows) {
  std::vector<std::string> symbol;
  std::vector<Timestamp> settle;
  std::vector<atx::i64> cur_short;
  std::vector<atx::i64> adv;
  std::vector<atx::f64> dtc;
  std::vector<atx::f64> chg;
  for (const auto& r : rows) {
    symbol.push_back(r.symbol);
    settle.push_back(Timestamp::from_unix_nanos(r.settle_day * kNsPerDay));
    cur_short.push_back(r.current_short);
    adv.push_back(r.avg_daily_volume);
    dtc.push_back(r.days_to_cover);
    chg.push_back(r.change_percent);
  }
  const std::vector<atx::core::io::WriteColumn> cols = {
      {"settlement_date", std::span<const Timestamp>(settle)},
      {"symbol", std::span<const std::string>(symbol)},
      {"current_short_position_quantity", std::span<const atx::i64>(cur_short)},
      {"average_daily_volume_quantity", std::span<const atx::i64>(adv)},
      {"days_to_cover_quantity", std::span<const atx::f64>(dtc)},
      {"change_percent", std::span<const atx::f64>(chg)},
  };
  const fs::path path = root / ("date=" + date_dir) / "part-00000.parquet";
  return atx::core::io::write_parquet(cols, path.string()).has_value();
}

// A unique temp root per test (deleted at scope exit).
struct TempRoot {
  fs::path path;
  explicit TempRoot(const std::string& tag) {
    path = fs::temp_directory_path() /
           ("atx_finra_" + tag + "_" + std::to_string(::testing::UnitTest::GetInstance()
                                                          ->random_seed()) +
            "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
  }
  ~TempRoot() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Causality: no look-ahead, then forward-fill.
// ---------------------------------------------------------------------------
TEST(FinraShort, CausalityNoLookAhead) {
  TempRoot root("causal");

  // ONE instrument ("AAA" -> column 0). Two FINRA observations:
  //   settle_day=100, dtc=3.0  -> publish_day = 100 + lag(10) = 110
  //   settle_day=130, dtc=7.0  -> publish_day = 130 + 10      = 140
  ASSERT_TRUE(write_partition(root.path, "p1",
                              {{"AAA", 100, /*short=*/0, /*adv=*/0, /*dtc=*/3.0, /*chg=*/0.0}}));
  ASSERT_TRUE(write_partition(root.path, "p2",
                              {{"AAA", 130, /*short=*/0, /*adv=*/0, /*dtc=*/7.0, /*chg=*/0.0}}));

  // Panel dates straddle both publish boundaries: 105,109,110,120,139,140,150.
  const std::vector<DateKey> dates = {105, 109, 110, 120, 139, 140, 150};
  const std::unordered_map<std::string, InstKey> sym_to_inst = {{"AAA", 0}};

  auto res = load_finra_features(root.path.string(), dates, sym_to_inst, /*instruments=*/1,
                                 /*shares=*/{}, /*publication_lag_days=*/10);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const FinraFeatures& f = *res;
  ASSERT_EQ(f.dates, dates.size());
  ASSERT_EQ(f.instruments, 1u);

  // Before the first publish_day (110): NaN.
  EXPECT_TRUE(std::isnan(f.si_dtc[0])) << "date 105 < publish 110 must be NaN";
  EXPECT_TRUE(std::isnan(f.si_dtc[1])) << "date 109 < publish 110 must be NaN";
  // On/after first publish, before second publish (140): the first value (3.0),
  // forward-filled.
  EXPECT_DOUBLE_EQ(f.si_dtc[2], 3.0); // 110 == publish -> visible
  EXPECT_DOUBLE_EQ(f.si_dtc[3], 3.0); // 120 -> forward-fill
  EXPECT_DOUBLE_EQ(f.si_dtc[4], 3.0); // 139 < 140 -> still first obs
  // On/after second publish (140): the second value (7.0) supersedes.
  EXPECT_DOUBLE_EQ(f.si_dtc[5], 7.0); // 140 == publish
  EXPECT_DOUBLE_EQ(f.si_dtc[6], 7.0); // 150 -> forward-fill of second obs

  // Explicit no-look-ahead assertion: the second obs (value 7.0) must NEVER
  // appear on any date < its publish_day (140).
  for (atx::usize d = 0; d < dates.size(); ++d) {
    if (dates[d] < 140) {
      EXPECT_NE(f.si_dtc[d], 7.0) << "look-ahead: obs2 leaked onto date " << dates[d];
    }
  }
}

// ---------------------------------------------------------------------------
// 2. Derived-field math at a known visible cell.
// ---------------------------------------------------------------------------
TEST(FinraShort, DerivedFieldMath) {
  TempRoot root("math");
  // settle_day=10 -> publish_day=20 with lag 10.
  ASSERT_TRUE(write_partition(
      root.path, "p1",
      {{"BBB", 10, /*short=*/2'000'000, /*adv=*/500'000, /*dtc=*/4.5, /*chg=*/12.5}}));

  const std::vector<DateKey> dates = {30}; // >= publish_day 20 -> visible
  const std::unordered_map<std::string, InstKey> sym_to_inst = {{"BBB", 0}};

  // shares = 10,000,000 -> si_util = 2,000,000 / 10,000,000 = 0.2
  const std::vector<atx::f64> shares = {10'000'000.0};

  auto res = load_finra_features(root.path.string(), dates, sym_to_inst, /*instruments=*/1, shares,
                                 /*publication_lag_days=*/10);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const FinraFeatures& f = *res;

  EXPECT_DOUBLE_EQ(f.si_dtc[0], 4.5);  // == days_to_cover_quantity
  EXPECT_DOUBLE_EQ(f.si_chg[0], 12.5); // == change_percent
  EXPECT_DOUBLE_EQ(f.si_util[0], 0.2); // == short / shares
  EXPECT_EQ(f.util_from_shares, 1u);
  EXPECT_EQ(f.util_from_adv, 0u);
}

// ---------------------------------------------------------------------------
// 3. si_util ADV fallback when shares is unavailable.
// ---------------------------------------------------------------------------
TEST(FinraShort, UtilAdvFallback) {
  TempRoot root("adv");
  ASSERT_TRUE(write_partition(
      root.path, "p1",
      {{"CCC", 10, /*short=*/3'000'000, /*adv=*/1'000'000, /*dtc=*/3.0, /*chg=*/0.0}}));

  const std::vector<DateKey> dates = {30};
  const std::unordered_map<std::string, InstKey> sym_to_inst = {{"CCC", 0}};

  // Empty shares -> ADV fallback: 3,000,000 / 1,000,000 = 3.0
  auto res = load_finra_features(root.path.string(), dates, sym_to_inst, /*instruments=*/1,
                                 /*shares=*/{}, /*publication_lag_days=*/10);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const FinraFeatures& f = *res;
  EXPECT_DOUBLE_EQ(f.si_util[0], 3.0);
  EXPECT_EQ(f.util_from_shares, 0u);
  EXPECT_EQ(f.util_from_adv, 1u);
}

// ---------------------------------------------------------------------------
// 4. Missing coverage stays NaN; an out-of-map symbol is dropped, not misplaced.
// ---------------------------------------------------------------------------
TEST(FinraShort, MissingCoverageStaysNaN) {
  TempRoot root("missing");
  // FINRA has "AAA" (-> col 0) and "ZZZ" (NOT in the map). Panel has 2 columns
  // (col 0 = AAA, col 1 = DDD which FINRA never reports).
  ASSERT_TRUE(write_partition(
      root.path, "p1",
      {{"AAA", 10, /*short=*/1, /*adv=*/1, /*dtc=*/9.0, /*chg=*/1.0},
       {"ZZZ", 10, /*short=*/1, /*adv=*/1, /*dtc=*/99.0, /*chg=*/9.0}}));

  const std::vector<DateKey> dates = {30};
  const std::unordered_map<std::string, InstKey> sym_to_inst = {{"AAA", 0}};

  auto res = load_finra_features(root.path.string(), dates, sym_to_inst, /*instruments=*/2,
                                 /*shares=*/{}, /*publication_lag_days=*/10);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const FinraFeatures& f = *res;
  ASSERT_EQ(f.instruments, 2u);

  // Col 0 (AAA) is covered.
  EXPECT_DOUBLE_EQ(f.si_dtc[0 * 2 + 0], 9.0);
  // Col 1 (DDD) has no FINRA record -> NaN (never imputed).
  EXPECT_TRUE(std::isnan(f.si_dtc[0 * 2 + 1])) << "uncovered instrument must stay NaN";
  // ZZZ (value 99.0) must NOT be misplaced onto column 1.
  EXPECT_NE(f.si_dtc[0 * 2 + 1], 99.0);
  EXPECT_GE(f.symbols_unmatched, 1u); // ZZZ counted as unmatched
}

} // namespace atxtest_finra_short
