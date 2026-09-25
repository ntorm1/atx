// W0-D0 — FINRA short-interest publication lag counted in NYSE sessions (D-02).
//
// Suite: DataFinraLag_W0d0
//
// FINRA releases short interest after the close on the 7th business day after the
// settlement date, so a value is first usable on the 8th NYSE session after it.
// The legacy rule (settlement + 10 calendar days) leaks 1-2 sessions whenever the
// settlement is a Thursday or Friday. Every fixture below is a real 2018/2019
// calendar date (synthetic values; no real FINRA data is read), checked by hand:
//
//   settlement              sessions 1..7 after it (release after close on 7)   usable
//   Mon 2019-03-11          12 13 14 15 18 19 20                                Thu 03-21
//   Thu 2019-03-14          15 18 19 20 21 22 25                                Tue 03-26
//   Fri 2019-03-15          18 19 20 21 22 25 26                                Wed 03-27
//   Thu 2019-01-17 (MLK)    18 22 23 24 25 28 29  (Mon 01-21 closed)            Wed 01-30
//   Thu 2018-11-15 (Thanksg)16 19 20 21 23 26 27  (Thu 11-22 closed)            Wed 11-28
//   Fri 2018-11-30 (Bush)   03 04 06 07 10 11 12  (Wed 12-05 closed)            Thu 12-13
//   Mon 2019-04-15 (GoodFri)16 17 18 22 23 24 25  (Fri 04-19 closed)            Fri 04-26

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/finra_short.hpp"

namespace atx_test_w0_d0_finra_lag {

namespace fs = std::filesystem;
using atx::core::ErrorCode;
using atx::core::time::days_from_civil;
using atx::core::time::Timestamp;
using atx::engine::data::DateKey;
using atx::engine::data::finra_first_usable_day;
using atx::engine::data::FinraLagRule;
using atx::engine::data::FinraPublicationLag;
using atx::engine::data::InstKey;
using atx::engine::data::load_finra_features;
using atx::engine::data::nyse_add_sessions;
using atx::engine::data::nyse_is_session;

namespace {

constexpr atx::i64 kNsPerDay = 86'400LL * 1'000'000'000LL;

[[nodiscard]] atx::i64 day(int y, unsigned m, unsigned d) { return days_from_civil(y, m, d); }

struct Case {
  const char *label;
  atx::i64 settlement;
  atx::i64 usable;              // expected first usable day (NyseSessionsV2 default)
  atx::i64 legacy_sessions_early; // how many sessions the legacy rule leaked
};

[[nodiscard]] std::vector<Case> cases() {
  return {
      {"Mon 2019-03-11", day(2019, 3, 11), day(2019, 3, 21), 0},
      {"Thu 2019-03-14", day(2019, 3, 14), day(2019, 3, 26), 1},
      {"Fri 2019-03-15", day(2019, 3, 15), day(2019, 3, 27), 2},
      {"Thu 2019-01-17 MLK", day(2019, 1, 17), day(2019, 1, 30), 2},
      {"Thu 2018-11-15 Thanksgiving", day(2018, 11, 15), day(2018, 11, 28), 2},
      {"Fri 2018-11-30 Bush closure", day(2018, 11, 30), day(2018, 12, 13), 3},
      {"Mon 2019-04-15 Good Friday", day(2019, 4, 15), day(2019, 4, 26), 1},
  };
}

// Count NYSE sessions in [from, to) — used to measure the legacy leak.
[[nodiscard]] atx::i64 sessions_between(atx::i64 from, atx::i64 to) {
  atx::i64 n = 0;
  for (atx::i64 d = from; d < to; ++d) {
    n += nyse_is_session(d).value() ? 1 : 0;
  }
  return n;
}

struct TempRoot {
  fs::path path;
  explicit TempRoot(const std::string &tag) {
    path = fs::temp_directory_path() / ("atx_w0d0_finra_" + tag);
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
  }
  ~TempRoot() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempRoot(const TempRoot &) = delete;
  TempRoot &operator=(const TempRoot &) = delete;
};

// One partition holding one row per (symbol, settlement day); dtc = `dtc`.
[[nodiscard]] bool write_rows(const fs::path &root, const std::string &dir,
                              const std::vector<std::string> &syms,
                              const std::vector<atx::i64> &settle_days,
                              const std::vector<atx::f64> &dtc) {
  std::vector<Timestamp> settle;
  std::vector<atx::i64> zeros(syms.size(), 1);
  std::vector<atx::f64> chg(syms.size(), 0.0);
  for (const atx::i64 d : settle_days) {
    settle.push_back(Timestamp::from_unix_nanos(d * kNsPerDay));
  }
  const std::vector<atx::core::io::WriteColumn> cols = {
      {"settlement_date", std::span<const Timestamp>(settle)},
      {"symbol", std::span<const std::string>(syms)},
      {"current_short_position_quantity", std::span<const atx::i64>(zeros)},
      {"average_daily_volume_quantity", std::span<const atx::i64>(zeros)},
      {"days_to_cover_quantity", std::span<const atx::f64>(dtc)},
      {"change_percent", std::span<const atx::f64>(chg)},
  };
  const fs::path p = root / ("date=" + dir) / "part-00000.parquet";
  return atx::core::io::write_parquet(cols, p.string()).has_value();
}

} // namespace

// The session calendar: weekends, rule holidays, unscheduled closures, the pre-1998
// MLK rule, and the supported range.
TEST(DataFinraLag_W0d0, NyseCalendarKnownSessionsAndClosures) {
  EXPECT_TRUE(nyse_is_session(day(2019, 3, 14)).value());        // Thursday
  EXPECT_FALSE(nyse_is_session(day(2019, 3, 16)).value());       // Saturday
  EXPECT_FALSE(nyse_is_session(day(2019, 3, 17)).value());       // Sunday
  EXPECT_FALSE(nyse_is_session(day(2019, 1, 21)).value());       // MLK 2019
  EXPECT_TRUE(nyse_is_session(day(1997, 1, 20)).value());        // MLK before NYSE observed it
  EXPECT_FALSE(nyse_is_session(day(1998, 1, 19)).value());       // first NYSE MLK closure
  EXPECT_FALSE(nyse_is_session(day(2019, 4, 19)).value());       // Good Friday
  EXPECT_FALSE(nyse_is_session(day(2018, 11, 22)).value());      // Thanksgiving
  EXPECT_FALSE(nyse_is_session(day(2018, 12, 5)).value());       // G.H.W. Bush mourning
  EXPECT_FALSE(nyse_is_session(day(2012, 10, 29)).value());      // Hurricane Sandy
  EXPECT_FALSE(nyse_is_session(day(2001, 9, 14)).value());       // September 11 closure
  EXPECT_FALSE(nyse_is_session(day(2015, 7, 3)).value());        // July 4 on a Saturday
  EXPECT_TRUE(nyse_is_session(day(2010, 12, 31)).value());       // New Year on Sat: open
  // 2019 has 252 NYSE sessions.
  EXPECT_EQ(sessions_between(day(2019, 1, 1), day(2020, 1, 1)), 252);
  EXPECT_EQ(nyse_is_session(day(1989, 12, 29)).error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(nyse_add_sessions(day(2019, 3, 14), -1).error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(nyse_add_sessions(day(2019, 3, 14), 0).value(), day(2019, 3, 14));
}

// Thursday / Friday / holiday settlements land on the 8th session; the legacy
// calendar-day rule is measurably early on exactly those.
TEST(DataFinraLag_W0d0, ThursdayFridayAndHolidaySettlementsLandOnTheEighthSession) {
  const FinraPublicationLag v2{};
  const FinraPublicationLag v1{FinraLagRule::CalendarDaysV1, 10, false};
  for (const Case &c : cases()) {
    const auto usable = finra_first_usable_day(c.settlement, v2);
    ASSERT_TRUE(usable.has_value()) << c.label;
    EXPECT_EQ(*usable, c.usable) << c.label;
    EXPECT_TRUE(nyse_is_session(*usable).value()) << c.label;
    // Exactly 8 sessions after settlement (7 + 1 for the after-close release).
    EXPECT_EQ(sessions_between(c.settlement + 1, *usable + 1), 8) << c.label;

    const atx::i64 legacy = finra_first_usable_day(c.settlement, v1).value();
    EXPECT_EQ(legacy, c.settlement + 10) << c.label;
    // Sessions the legacy rule exposed the value before it was public.
    const atx::i64 early = legacy < *usable ? sessions_between(legacy, *usable) : 0;
    EXPECT_EQ(early, c.legacy_sessions_early) << c.label;
    std::printf("[finra-lag] %-28s usable=%lld legacy_early_sessions=%lld\n", c.label,
                static_cast<long long>(*usable), static_cast<long long>(early));
  }
  // Without the after-close +1 the value lands on the release session itself.
  FinraPublicationLag same_day = v2;
  same_day.after_close = false;
  EXPECT_EQ(finra_first_usable_day(day(2019, 3, 14), same_day).value(), day(2019, 3, 25));
  const FinraPublicationLag negative{FinraLagRule::NyseSessionsV2, -1, true};
  EXPECT_EQ(finra_first_usable_day(day(2019, 3, 14), negative).error().code(),
            ErrorCode::InvalidArgument);
}

// The loader places each observation on its usable session and never before, on a
// session-axis panel; the default overload is the NYSE rule.
TEST(DataFinraLag_W0d0, LoaderPlacesValuesOnTheUsableSessionNotBefore) {
  TempRoot root("loader");
  std::vector<std::string> syms;
  std::vector<atx::i64> settles;
  std::vector<atx::f64> dtc;
  std::unordered_map<std::string, InstKey> sym_to_inst;
  const std::vector<Case> all = cases();
  for (atx::usize k = 0; k < all.size(); ++k) {
    syms.push_back("S" + std::to_string(k));
    settles.push_back(all[k].settlement);
    dtc.push_back(1.0 + static_cast<atx::f64>(k));
    sym_to_inst.emplace(syms.back(), static_cast<InstKey>(k));
  }
  ASSERT_TRUE(write_rows(root.path, "all", syms, settles, dtc));

  // Panel axis: every NYSE session from 2018-11-01 to 2019-05-31.
  std::vector<DateKey> dates;
  for (atx::i64 d = day(2018, 11, 1); d <= day(2019, 5, 31); ++d) {
    if (nyse_is_session(d).value()) {
      dates.push_back(d);
    }
  }
  const atx::usize n = all.size();
  auto v2 = load_finra_features(root.path.string(), dates, sym_to_inst, n, {});
  auto v1 = load_finra_features(root.path.string(), dates, sym_to_inst, n, {}, /*days=*/10);
  ASSERT_TRUE(v2.has_value()) << v2.error().to_string();
  ASSERT_TRUE(v1.has_value()) << v1.error().to_string();

  atx::usize leaked_cells_v1 = 0;
  for (atx::usize k = 0; k < n; ++k) {
    std::optional<atx::usize> first_v2;
    std::optional<atx::usize> first_v1;
    for (atx::usize d = 0; d < dates.size(); ++d) {
      const atx::usize cell = d * n + k;
      if (!first_v2 && !std::isnan(v2->si_dtc[cell])) {
        first_v2 = d;
      }
      if (!first_v1 && !std::isnan(v1->si_dtc[cell])) {
        first_v1 = d;
      }
      if (dates[d] < all[k].usable) {
        EXPECT_TRUE(std::isnan(v2->si_dtc[cell])) << all[k].label << " leaked on " << dates[d];
        leaked_cells_v1 += std::isnan(v1->si_dtc[cell]) ? 0U : 1U;
      }
    }
    ASSERT_TRUE(first_v2.has_value()) << all[k].label;
    EXPECT_EQ(dates[*first_v2], all[k].usable) << all[k].label;
    EXPECT_DOUBLE_EQ(v2->si_dtc[*first_v2 * n + k], dtc[k]);
    ASSERT_TRUE(first_v1.has_value());
    EXPECT_EQ(static_cast<atx::i64>(*first_v2 - *first_v1), all[k].legacy_sessions_early)
        << all[k].label;
  }
  // 0 + 1 + 2 + 2 + 2 + 3 + 1 = 11 session-cells exposed early by the legacy rule.
  EXPECT_EQ(leaked_cells_v1, 11U);
  std::printf("[finra-lag] loader: legacy leaked %zu cells, NYSE rule leaked 0\n",
              static_cast<std::size_t>(leaked_cells_v1));
}

// A matched settlement outside the calendar range fails closed under the NYSE rule;
// a negative lag fails for both overloads.
TEST(DataFinraLag_W0d0, OutOfRangeSettlementAndNegativeLagFailClosed) {
  TempRoot root("range");
  ASSERT_TRUE(write_rows(root.path, "old", {"OLD"}, {day(1985, 6, 3)}, {2.0}));
  const std::vector<DateKey> dates = {day(1985, 7, 1)};
  const std::unordered_map<std::string, InstKey> map = {{"OLD", 0}};
  auto v2 = load_finra_features(root.path.string(), dates, map, 1, {});
  ASSERT_FALSE(v2.has_value());
  EXPECT_EQ(v2.error().code(), ErrorCode::OutOfRange);
  auto v1 = load_finra_features(root.path.string(), dates, map, 1, {}, 10);
  ASSERT_TRUE(v1.has_value()) << "the legacy calendar-day rule has no range limit";
  auto neg = load_finra_features(root.path.string(), dates, map, 1, {}, -1);
  ASSERT_FALSE(neg.has_value());
  EXPECT_EQ(neg.error().code(), ErrorCode::InvalidArgument);
}

} // namespace atx_test_w0_d0_finra_lag
