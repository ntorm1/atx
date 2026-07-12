#include "atx/vol/vol_time.hpp"

#include <gtest/gtest.h>

#include <cstdint>

#include "atx/core/datetime.hpp"

// Coverage for the SpiderRock-style hybrid volatility-time clock: the ET
// civil-date/DST conversion (exercised indirectly through the public API),
// the NYSE holiday calendar, `trading_hours_between`, and the master
// `vol_time_years` formula. `ns_utc` builds UTC epoch-ns test fixtures via
// atx-core's already-validated Hinnant civil-date math (`timestamp_from_utc`)
// — it is a plain UTC constructor, independent of vol_time's own internal
// ET/DST conversion, so these tests do not assume anything about how vol_time
// computes its ET offsets internally.

namespace {

using atx::vol::trading_hours_between;
using atx::vol::VolTimeCalendar;
using atx::vol::VolTimeParams;
using atx::vol::vol_time_years;

constexpr std::int64_t kHourNs = 3600LL * 1'000'000'000LL;
constexpr std::int64_t kDayNs = 24LL * kHourNs;

std::int64_t ns_utc(int y, unsigned m, unsigned d, int hour, int minute) {
  return atx::core::time::timestamp_from_utc(y, m, d, static_cast<unsigned>(hour),
                                              static_cast<unsigned>(minute), 0U, 0U)
      .unix_nanos();
}

// ── vol_time_years ───────────────────────────────────────────────────────

// Anchor: Wed 2026-07-08 (regular trading day, EDT, UTC-4).
// 09:30 ET == 13:30 UTC, 17:00 ET == 21:00 UTC.
TEST(VolTime, FullTradingDayAtAlphaOne) {
  VolTimeParams p;
  p.alpha = 1.0;
  // Wed 2026-07-08 00:00 ET -> Thu 2026-07-09 00:00 ET covers one full session.
  const auto t0 = ns_utc(2026, 7, 8, 4, 0);  // 00:00 EDT
  const auto t1 = t0 + kDayNs;
  EXPECT_NEAR(vol_time_years(t0, t1, p, VolTimeCalendar::us_default()), 7.5 / 1890.0,
              1e-12);  // == 1/252
}

TEST(VolTime, WeekendIsPureNonTrading) {
  VolTimeParams p;  // alpha 0.7
  const auto sat0 = ns_utc(2026, 7, 11, 4, 0);  // Sat 00:00 EDT
  const auto mon0 = sat0 + 2 * kDayNs;          // Mon 00:00 EDT
  EXPECT_NEAR(vol_time_years(sat0, mon0, p, VolTimeCalendar::us_default()),
              48.0 * 0.3 / 6870.0, 1e-12);
}

TEST(VolTime, JulyFourthHolidayHasNoTradingHours) {
  // 2026-07-03 (Fri) is the NYSE observed Independence Day closure.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto t0 = ns_utc(2026, 7, 3, 4, 0);
  EXPECT_NEAR(vol_time_years(t0, t0 + kDayNs, p, VolTimeCalendar::us_default()), 0.0,
              1e-15);
}

TEST(VolTime, OneYearIsApproximatelyOne) {
  VolTimeParams p;
  const auto t0 = ns_utc(2026, 1, 2, 5, 0);
  const auto t1 = ns_utc(2027, 1, 2, 5, 0);
  EXPECT_NEAR(vol_time_years(t0, t1, p, VolTimeCalendar::us_default()), 1.0, 0.02);
}

TEST(VolTime, MonotoneNonIncreasingAsNowAdvances) {
  VolTimeParams p;  // alpha 0.7
  const auto& cal = VolTimeCalendar::us_default();
  // Fixed expiry well beyond the 2-week now-advance window below.
  const auto expiry = ns_utc(2026, 8, 15, 4, 0);
  auto now = ns_utc(2026, 7, 8, 4, 0);  // Wed 00:00 EDT

  constexpr int kSteps = 24 * 14;  // 2 weeks, stepping by 1h
  // Per-step decrease in T_vol is bounded above by a step that lands entirely
  // in a trading session (weight alpha/trading_hours_per_year, the larger of
  // the two per-hour weights) and bounded below by 0 (a step entirely outside
  // any session). Budget a 2x safety factor over that theoretical max for the
  // continuity check.
  const double step_budget = 2.0 * (p.alpha / p.trading_hours_per_year);

  double prev = vol_time_years(now, expiry, p, cal);
  for (int i = 0; i < kSteps; ++i) {
    now += kHourNs;
    const double cur = vol_time_years(now, expiry, p, cal);
    EXPECT_LE(cur, prev + 1e-12) << "step " << i << ": T_vol increased as now advanced";
    EXPECT_NEAR(cur, prev, step_budget) << "step " << i << ": discontinuous jump";
    prev = cur;
  }
}

TEST(VolTime, IntradayDecayFasterThanOvernight) {
  // 1 trading hour at alpha .7 outweighs 1 overnight hour: a/1890*.7 > .3/6870.
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto mid_session = ns_utc(2026, 7, 8, 16, 0);  // 12:00 ET
  const auto d_trading = vol_time_years(mid_session, mid_session + kHourNs, p, cal);
  const auto overnight = ns_utc(2026, 7, 9, 2, 0);  // 22:00 ET Wed
  const auto d_night = vol_time_years(overnight, overnight + kHourNs, p, cal);
  EXPECT_GT(d_trading, d_night);
  EXPECT_NEAR(d_trading, 0.7 / 1890.0, 1e-12);
  EXPECT_NEAR(d_night, 0.3 / 6870.0, 1e-12);
}

// ── DST / civil-date conversion (exercised through the public API) ───────

TEST(VolTime, WinterSessionUsesEstOffset) {
  // Wed 2026-01-07 (regular trading day, EST, UTC-5). 13:00-14:00 ET winter ==
  // 18:00-19:00 UTC, entirely inside the 09:30-17:00 ET session.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto t0 = ns_utc(2026, 1, 7, 18, 0);
  const auto t1 = ns_utc(2026, 1, 7, 19, 0);
  EXPECT_NEAR(vol_time_years(t0, t1, p, VolTimeCalendar::us_default()), 1.0 / 1890.0,
              1e-12);
}

TEST(VolTime, DstSpringForwardWeekSessionsAreExact) {
  // 2026 spring-forward: 2nd Sunday of March == 2026-03-08. Fri 2026-03-06 is
  // the last EST trading day, Mon 2026-03-09 the first EDT one. Both must
  // still resolve to exactly one full 7.5h session -- day-granularity DST
  // resolution is exact here because the transition instant itself falls on
  // the intervening (non-trading) Sunday.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto& cal = VolTimeCalendar::us_default();
  const auto fri_est = ns_utc(2026, 3, 6, 4, 0);  // 00:00 EST Fri (winter offset)
  EXPECT_NEAR(trading_hours_between(fri_est, fri_est + kDayNs, p, cal), 7.5, 1e-9);
  const auto mon_edt = ns_utc(2026, 3, 9, 4, 0);  // 00:00 EDT Mon (summer offset)
  EXPECT_NEAR(trading_hours_between(mon_edt, mon_edt + kDayNs, p, cal), 7.5, 1e-9);
}

TEST(VolTime, DstFallBackWeekSessionsAreExact) {
  // 2026 fall-back: 1st Sunday of November == 2026-11-01. Fri 2026-10-30 is
  // the last EDT trading day, Mon 2026-11-02 the first EST one.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto& cal = VolTimeCalendar::us_default();
  const auto fri_edt = ns_utc(2026, 10, 30, 4, 0);  // 00:00 EDT Fri (summer offset)
  EXPECT_NEAR(trading_hours_between(fri_edt, fri_edt + kDayNs, p, cal), 7.5, 1e-9);
  const auto mon_est = ns_utc(2026, 11, 2, 5, 0);  // 00:00 EST Mon (winter offset)
  EXPECT_NEAR(trading_hours_between(mon_est, mon_est + kDayNs, p, cal), 7.5, 1e-9);
}

// ── expiry-before/at-now (degenerate interval) ────────────────────────────

TEST(VolTime, VolTimeYearsIsZeroWhenExpiryNotAfterNow) {
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto t0 = ns_utc(2026, 7, 8, 13, 30);  // mid-session Wed
  EXPECT_EQ(vol_time_years(t0, t0, p, cal), 0.0);       // expiry == now
  EXPECT_EQ(vol_time_years(t0, t0 - kHourNs, p, cal), 0.0);  // expiry before now
}

// ── trading_hours_between ─────────────────────────────────────────────────

TEST(VolTime, TradingHoursBetweenIsZeroWhenEndNotAfterStart) {
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto t0 = ns_utc(2026, 7, 8, 13, 30);
  EXPECT_EQ(trading_hours_between(t0, t0, p, cal), 0.0);
  EXPECT_EQ(trading_hours_between(t0, t0 - kHourNs, p, cal), 0.0);
}

TEST(VolTime, TradingHoursBetweenFullSessionIsSpanHours) {
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto t0 = ns_utc(2026, 7, 8, 4, 0);  // Wed 00:00 EDT
  EXPECT_NEAR(trading_hours_between(t0, t0 + kDayNs, p, cal), 7.5, 1e-9);
}

// ── VolTimeCalendar ────────────────────────────────────────────────────────

TEST(VolTime, CalendarKnowsNyseHolidays2026) {
  const auto& cal = VolTimeCalendar::us_default();
  // 2026 New Year's Day.
  const auto new_years = static_cast<std::int32_t>(ns_utc(2026, 1, 1, 0, 0) / kDayNs);
  EXPECT_TRUE(cal.is_holiday(new_years));
  // 2026-07-08 (the trading-day anchor used throughout this file) is not.
  const auto regular_day = static_cast<std::int32_t>(ns_utc(2026, 7, 8, 0, 0) / kDayNs);
  EXPECT_FALSE(cal.is_holiday(regular_day));
}

TEST(VolTime, CalendarUnobservedNewYear2028IsNotAHoliday) {
  // 2028-01-01 falls on a Saturday and is intentionally unobserved.
  const auto& cal = VolTimeCalendar::us_default();
  const auto day = static_cast<std::int32_t>(ns_utc(2028, 1, 1, 0, 0) / kDayNs);
  EXPECT_FALSE(cal.is_holiday(day));
}

TEST(VolTime, CalendarConstructorSortsAndDedupes) {
  // Unsorted, duplicated input must still answer correctly.
  const std::int32_t d1 = static_cast<std::int32_t>(ns_utc(2026, 3, 3, 0, 0) / kDayNs);
  const std::int32_t d2 = static_cast<std::int32_t>(ns_utc(2026, 1, 1, 0, 0) / kDayNs);
  VolTimeCalendar cal({d1, d2, d1, d2});
  EXPECT_TRUE(cal.is_holiday(d1));
  EXPECT_TRUE(cal.is_holiday(d2));
  const std::int32_t other = static_cast<std::int32_t>(ns_utc(2026, 7, 8, 0, 0) / kDayNs);
  EXPECT_FALSE(cal.is_holiday(other));
}

}  // namespace
