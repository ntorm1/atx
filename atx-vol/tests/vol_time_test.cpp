#include "atx/vol/api/core/vol_time.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>

#include "atx/core/datetime.hpp"
#include "atx/vol/api/marketdata/data.hpp"
#include "atx/vol/api/backtest/panel.hpp"
#include "atx/vol/api/fitting/s3.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/marketdata/universe.hpp"

// Coverage for the SpiderRock-style hybrid volatility-time clock: the ET
// civil-date/DST conversion (exercised indirectly through the public API),
// the NYSE holiday calendar, `trading_hours_between`, and the master
// `vol_time_years` formula. `ns_utc` builds UTC epoch-ns test fixtures via
// atx-core's already-validated Hinnant civil-date math (`timestamp_from_utc`)
// — it is a plain UTC constructor, independent of vol_time's own internal
// ET/DST conversion, so these tests do not assume anything about how vol_time
// computes its ET offsets internally.

namespace {

using atx::vol::data_install;
using atx::vol::ErrorCode;
using atx::vol::iso_to_ns;
using atx::vol::QuoteFrame;
using atx::vol::make_synthetic_american_panel;
using atx::vol::S3Params;
using atx::vol::SessionInputs;
using atx::vol::Side;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::time_to_expiry_years;
using atx::vol::TimeConvention;
using atx::vol::TimeSpec;
using atx::vol::trading_hours_between;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::VolaSession;
using atx::vol::VolTimeCalendar;
using atx::vol::VolTimeParams;
using atx::vol::vol_time_years;
using atx::vol::year_fraction;

constexpr std::int64_t kHourNs = 3600LL * 1'000'000'000LL;
constexpr std::int64_t kDayNs = 24LL * kHourNs;

std::int64_t ns_utc(int y, unsigned m, unsigned d, int hour, int minute) {
  return atx::core::time::timestamp_from_utc(y, m, d, static_cast<unsigned>(hour),
                                              static_cast<unsigned>(minute), 0U, 0U)
      .unix_nanos();
}

// Days-since-epoch of a civil date, via the same already-validated atx-core UTC
// constructor `ns_utc` uses (independent of vol_time's internal civil-date math).
std::int32_t day_utc(int y, unsigned m, unsigned d) {
  return static_cast<std::int32_t>(ns_utc(y, m, d, 0, 0) / kDayNs);
}

// Unwraps a vol-time clock Result in the tests that assert a NUMBER: a
// fail-closed coverage error here is a test failure, never a silent sentinel.
double ok(const atx::vol::Result<double>& r) {
  EXPECT_TRUE(r.has_value()) << "vol-time clock failed: "
                             << (r.has_value() ? std::string{} : r.error().to_string());
  return r.has_value() ? *r : std::numeric_limits<double>::quiet_NaN();
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
  EXPECT_NEAR(ok(vol_time_years(t0, t1, p, VolTimeCalendar::us_default())), 6.5 / 1638.0,
              1e-12);  // == 1/252 (1638 == 252 * 6.5)
}

TEST(VolTime, WeekendIsPureNonTrading) {
  VolTimeParams p;  // alpha 0.7
  const auto sat0 = ns_utc(2026, 7, 11, 4, 0);  // Sat 00:00 EDT
  const auto mon0 = sat0 + 2 * kDayNs;          // Mon 00:00 EDT
  EXPECT_NEAR(ok(vol_time_years(sat0, mon0, p, VolTimeCalendar::us_default())),
              48.0 * 0.3 / 7122.0, 1e-12);
}

TEST(VolTime, JulyFourthHolidayHasNoTradingHours) {
  // 2026-07-03 (Fri) is the NYSE observed Independence Day closure.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto t0 = ns_utc(2026, 7, 3, 4, 0);
  EXPECT_NEAR(ok(vol_time_years(t0, t0 + kDayNs, p, VolTimeCalendar::us_default())), 0.0,
              1e-15);
}

TEST(VolTime, OneYearIsApproximatelyOne) {
  VolTimeParams p;
  const auto t0 = ns_utc(2026, 1, 2, 5, 0);
  const auto t1 = ns_utc(2027, 1, 2, 5, 0);
  EXPECT_NEAR(ok(vol_time_years(t0, t1, p, VolTimeCalendar::us_default())), 1.0, 0.02);
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

  double prev = ok(vol_time_years(now, expiry, p, cal));
  for (int i = 0; i < kSteps; ++i) {
    now += kHourNs;
    const double cur = ok(vol_time_years(now, expiry, p, cal));
    EXPECT_LE(cur, prev + 1e-12) << "step " << i << ": T_vol increased as now advanced";
    EXPECT_NEAR(cur, prev, step_budget) << "step " << i << ": discontinuous jump";
    prev = cur;
  }
}

TEST(VolTime, IntradayDecayFasterThanOvernight) {
  // 1 trading hour at alpha .7 outweighs 1 overnight hour: .7/1638 > .3/7122.
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto mid_session = ns_utc(2026, 7, 8, 16, 0);  // 12:00 ET
  const auto d_trading = ok(vol_time_years(mid_session, mid_session + kHourNs, p, cal));
  const auto overnight = ns_utc(2026, 7, 9, 2, 0);  // 22:00 ET Wed
  const auto d_night = ok(vol_time_years(overnight, overnight + kHourNs, p, cal));
  EXPECT_GT(d_trading, d_night);
  EXPECT_NEAR(d_trading, 0.7 / 1638.0, 1e-12);
  EXPECT_NEAR(d_night, 0.3 / 7122.0, 1e-12);
}

// ── Vendor-clock regression pins (measured, not derived) ─────────────────
//
// The three numbers below are REGRESSION PINS ON SPIDERROCK'S OWN CLOCK, not
// on our arithmetic. They were measured by differencing the vendor's published
// `years` column across adjacent expiries in the licensed oracle store
// (C:\atx-cache\oracle\spiderrock\date=2026-08-14, 74 expiries x 19 intraday
// buckets); vol_time.hpp's header carries the full derivation and the
// independent intraday-slope confirmation. Their job here is to fail loudly if
// anyone re-touches `VolTimeParams`' defaults: the previous 1890/6870 + 7.5h
// values missed the trading-day increment by 0.47%, which is exactly the kind
// of drift that hides inside a plausible-looking T.
//
// HONEST SCOPE, two parts, both important:
//
// (a) Day-level increments alone do NOT uniquely identify the (alpha,
//     session-width) split -- a 7.5h day at alpha = 0.710606 reproduces the
//     first two to ~1e-9 and the third to ~6e-8. What rules that alternative
//     out is the intraday slope, the annual normalisation and the weekend
//     increment (header evidence 3-5); the slope is a regression estimate and
//     so cannot itself be pinned at this tolerance. These pins guard the clock
//     we selected; the header records why we selected it.
//
// (b) These are NOT the constants SpiderRock DOCUMENTS. Their published pages
//     state 1890/6870 with a 7.5h session, essentially unanimously. We match
//     their published `years` DATA for trade date 2026-08-14, which disagrees
//     with their prose. If these pins ever start failing against a freshly
//     pulled store, the live question is whether the vendor's data moved onto
//     its own documented convention -- not whether our arithmetic slipped.
TEST(VolTime, VendorMeasuredDayIncrementsArePinned) {
  VolTimeParams p;  // production defaults: alpha 0.7, 1638/7122, 09:30-16:00 ET
  const auto& cal = VolTimeCalendar::us_default();

  // One trading day, close to close: Tue 2026-07-07 16:00 ET -> Wed 07-08
  // 16:00 ET (both regular EDT sessions, no closure between them).
  // 6.5 trading h + 17.5 non-trading h.
  const auto tue_close = ns_utc(2026, 7, 7, 20, 0);   // 16:00 EDT
  const auto wed_close = ns_utc(2026, 7, 8, 20, 0);   // 16:00 EDT
  EXPECT_NEAR(ok(vol_time_years(tue_close, wed_close, p, cal)), 0.003514930, 1e-9);

  // One non-trading day: Sat 2026-07-11 16:00 ET -> Sun 07-12 16:00 ET.
  // 0 trading h + 24 non-trading h.
  const auto sat = ns_utc(2026, 7, 11, 20, 0);
  EXPECT_NEAR(ok(vol_time_years(sat, sat + kDayNs, p, cal)), 0.001010952, 1e-9);

  // Weekend roll: Fri 2026-07-10 16:00 ET -> Mon 07-13 16:00 ET == one trading
  // day + two non-trading days (6.5 trading h + 65.5 non-trading h). This is
  // the span most sensitive to where the session CLOSES: a wider session would
  // credit post-close trading hours on the Friday leg.
  const auto fri_close = ns_utc(2026, 7, 10, 20, 0);
  const auto mon_close = ns_utc(2026, 7, 13, 20, 0);
  EXPECT_NEAR(ok(vol_time_years(fri_close, mon_close, p, cal)), 0.005536834, 1e-9);
}

// ── DST / civil-date conversion (exercised through the public API) ───────

TEST(VolTime, WinterSessionUsesEstOffset) {
  // Wed 2026-01-07 (regular trading day, EST, UTC-5). 13:00-14:00 ET winter ==
  // 18:00-19:00 UTC, entirely inside the 09:30-16:00 ET session.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto t0 = ns_utc(2026, 1, 7, 18, 0);
  const auto t1 = ns_utc(2026, 1, 7, 19, 0);
  EXPECT_NEAR(ok(vol_time_years(t0, t1, p, VolTimeCalendar::us_default())), 1.0 / 1638.0,
              1e-12);
}

TEST(VolTime, DstSpringForwardWeekSessionsAreExact) {
  // 2026 spring-forward: 2nd Sunday of March == 2026-03-08. Fri 2026-03-06 is
  // the last EST trading day, Mon 2026-03-09 the first EDT one. Both must
  // still resolve to exactly one full 6.5h session -- day-granularity DST
  // resolution is exact here because the transition instant itself falls on
  // the intervening (non-trading) Sunday.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto& cal = VolTimeCalendar::us_default();
  const auto fri_est = ns_utc(2026, 3, 6, 4, 0);  // 00:00 EST Fri (winter offset)
  EXPECT_NEAR(ok(trading_hours_between(fri_est, fri_est + kDayNs, p, cal)), 6.5, 1e-9);
  const auto mon_edt = ns_utc(2026, 3, 9, 4, 0);  // 00:00 EDT Mon (summer offset)
  EXPECT_NEAR(ok(trading_hours_between(mon_edt, mon_edt + kDayNs, p, cal)), 6.5, 1e-9);
}

TEST(VolTime, DstFallBackWeekSessionsAreExact) {
  // 2026 fall-back: 1st Sunday of November == 2026-11-01. Fri 2026-10-30 is
  // the last EDT trading day, Mon 2026-11-02 the first EST one.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto& cal = VolTimeCalendar::us_default();
  const auto fri_edt = ns_utc(2026, 10, 30, 4, 0);  // 00:00 EDT Fri (summer offset)
  EXPECT_NEAR(ok(trading_hours_between(fri_edt, fri_edt + kDayNs, p, cal)), 6.5, 1e-9);
  const auto mon_est = ns_utc(2026, 11, 2, 5, 0);  // 00:00 EST Mon (winter offset)
  EXPECT_NEAR(ok(trading_hours_between(mon_est, mon_est + kDayNs, p, cal)), 6.5, 1e-9);
}

// ── expiry-before/at-now (degenerate interval) ────────────────────────────

TEST(VolTime, VolTimeYearsIsZeroWhenExpiryNotAfterNow) {
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto t0 = ns_utc(2026, 7, 8, 13, 30);  // mid-session Wed
  EXPECT_EQ(ok(vol_time_years(t0, t0, p, cal)), 0.0);       // expiry == now
  EXPECT_EQ(ok(vol_time_years(t0, t0 - kHourNs, p, cal)), 0.0);  // expiry before now
}

// ── trading_hours_between ─────────────────────────────────────────────────

TEST(VolTime, TradingHoursBetweenIsZeroWhenEndNotAfterStart) {
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto t0 = ns_utc(2026, 7, 8, 13, 30);
  EXPECT_EQ(ok(trading_hours_between(t0, t0, p, cal)), 0.0);
  EXPECT_EQ(ok(trading_hours_between(t0, t0 - kHourNs, p, cal)), 0.0);
}

TEST(VolTime, TradingHoursBetweenFullSessionIsSpanHours) {
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto t0 = ns_utc(2026, 7, 8, 4, 0);  // Wed 00:00 EDT
  EXPECT_NEAR(ok(trading_hours_between(t0, t0 + kDayNs, p, cal)), 6.5, 1e-9);
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
  const std::int32_t d1 = day_utc(2026, 3, 3);
  const std::int32_t d2 = day_utc(2026, 1, 1);
  VolTimeCalendar cal({d1, d2, d1, d2}, day_utc(2026, 1, 1), day_utc(2026, 12, 31));
  EXPECT_TRUE(cal.is_holiday(d1));
  EXPECT_TRUE(cal.is_holiday(d2));
  const std::int32_t other = day_utc(2026, 7, 8);
  EXPECT_FALSE(cal.is_holiday(other));
}

// ── Coverage window: fail closed outside the table (plan item 1.10) ────────
//
// The defect: `us_default()` enumerates NYSE full closures for 2024-2028 ONLY,
// yet was wired unconditionally into `time_to_expiry_years(VolTime)`. Outside
// that span every real closure reads as "not in the table" -> a full 6.5h
// trading session, silently, with no diagnostic — corrupting every vol-time
// number derived from it. Memorial Day 2020 (Mon 2020-05-25, NYSE fully
// closed) accrued a full session's trading hours before this guard existed.

TEST(VolTime, CalendarDeclaresItsUsDefaultCoverageWindow) {
  const auto& cal = VolTimeCalendar::us_default();
  EXPECT_EQ(cal.first_covered_day(), day_utc(2024, 1, 1));
  EXPECT_EQ(cal.last_covered_day(), day_utc(2028, 12, 31));
  EXPECT_TRUE(cal.covers(day_utc(2024, 1, 1)));
  EXPECT_TRUE(cal.covers(day_utc(2028, 12, 31)));
  EXPECT_FALSE(cal.covers(day_utc(2023, 12, 31)));
  EXPECT_FALSE(cal.covers(day_utc(2029, 1, 1)));
}

TEST(VolTime, TradingHoursBetweenBeforeWindowFailsClosed) {
  VolTimeParams p;
  p.alpha = 1.0;
  // Mon 2020-05-25 = Memorial Day, a full NYSE closure the 2024-2028 table
  // cannot see. Pre-fix this returned a full session's trading hours.
  const auto t0 = ns_utc(2020, 5, 25, 4, 0);  // 00:00 EDT
  const auto res = trading_hours_between(t0, t0 + kDayNs, p, VolTimeCalendar::us_default());
  ASSERT_FALSE(res.has_value()) << "silently accrued " << *res << " trading hours";
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(VolTime, TradingHoursBetweenAfterWindowFailsClosed) {
  VolTimeParams p;
  const auto t0 = ns_utc(2029, 1, 2, 4, 0);
  const auto res = trading_hours_between(t0, t0 + kDayNs, p, VolTimeCalendar::us_default());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(VolTime, VolTimeYearsStraddlingWindowEndFailsClosed) {
  // Starts inside the window, ends past it: the uncovered tail is exactly the
  // part that would be silently credited, so the whole query must fail.
  VolTimeParams p;
  const auto now = ns_utc(2028, 12, 20, 15, 0);
  const auto expiry = ns_utc(2029, 1, 19, 21, 0);
  const auto res = vol_time_years(now, expiry, p, VolTimeCalendar::us_default());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(VolTime, VolTimeYearsStraddlingWindowStartFailsClosed) {
  VolTimeParams p;
  const auto now = ns_utc(2023, 12, 20, 15, 0);
  const auto expiry = ns_utc(2024, 1, 19, 21, 0);
  const auto res = vol_time_years(now, expiry, p, VolTimeCalendar::us_default());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(VolTime, VolTimeYearsAtWindowBoundaryDaysSucceeds) {
  // The window is INCLUSIVE on both ends: the first and last covered SESSIONS
  // are answerable in full and must not be rejected.
  VolTimeParams p;
  p.alpha = 1.0;
  const auto& cal = VolTimeCalendar::us_default();
  // First covered day 2024-01-01 is itself a listed closure, so the first
  // covered session is Tue 2024-01-02.
  const auto first_session = ns_utc(2024, 1, 2, 5, 0);  // 00:00 EST
  EXPECT_NEAR(ok(trading_hours_between(first_session, first_session + kDayNs, p, cal)), 6.5,
              1e-9);
  // Last covered session: Fri 2028-12-29 (2028-12-30/31 are the weekend).
  const auto last_session = ns_utc(2028, 12, 29, 5, 0);
  EXPECT_NEAR(ok(trading_hours_between(last_session, last_session + kDayNs, p, cal)), 6.5, 1e-9);
  // The very next weekday, Mon 2029-01-01, is one day past the window. Its
  // status is unknown to the table (it IS a real NYSE closure), so it fails
  // closed rather than accruing a full session.
  const auto past_end = ns_utc(2029, 1, 1, 5, 0);
  const auto res = trading_hours_between(past_end, past_end + kDayNs, p, cal);
  ASSERT_FALSE(res.has_value()) << "silently accrued " << *res << " trading hours";
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(VolTime, ExtendedHoursPaddingDayOutsideWindowFailsClosed) {
  // Regression for the review finding: the coverage test must sit at the
  // ACCRUAL site, not on the interval's own day span.
  //
  // The session loop walks one padding day beyond each end of the interval.
  // Those padding days are non-contributing ONLY while a session cannot cross a
  // UTC midnight (`session_open_hour_et + session_span_hours <= 19` ET) -- and
  // both fields are caller-supplied and unvalidated. Here a 09:30-21:30 ET
  // extended-hours window puts Mon 2020-06-15's close at 01:30Z on 06-16, so
  // that day accrues 1.5h into an interval whose OWN days are both covered. A
  // window-vs-interval precheck waves it straight through and silently credits
  // an uncovered day -- exactly the defect this task closes.
  VolTimeParams p;
  p.alpha = 1.0;
  p.session_open_hour_et = 9.5;
  p.session_span_hours = 12.0;  // closes 21:30 ET == 01:30Z the NEXT UTC day
  // Covered window opens Tue 2020-06-16; the padding day Mon 2020-06-15 is not
  // covered. No listed closures, so nothing skips 06-15 for another reason.
  const VolTimeCalendar cal({}, day_utc(2020, 6, 16), day_utc(2020, 6, 30));
  const auto start = ns_utc(2020, 6, 16, 0, 0);   // both interval days covered
  const auto end = ns_utc(2020, 6, 16, 12, 0);    // ends before 06-16's own open
  const auto res = trading_hours_between(start, end, p, cal);
  ASSERT_FALSE(res.has_value())
      << "accrued " << *res << " h, all of it from the UNCOVERED 2020-06-15 session";
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);

  // Same params one day later: the padding day (Tue 2020-06-16) is covered, so
  // the identical spill-across-midnight shape is answerable and does accrue.
  const auto ok_start = ns_utc(2020, 6, 17, 0, 0);
  const auto ok_end = ns_utc(2020, 6, 17, 12, 0);
  EXPECT_NEAR(ok(trading_hours_between(ok_start, ok_end, p, cal)), 1.5, 1e-9);
}

TEST(VolTime, SpanBeyondTheDayLoopBoundFailsClosed) {
  // The day loop is bounded to ~20 years (JPL rule 2). It used to CLAMP the loop
  // to that bound and answer anyway, so an interval longer than the bound
  // silently stopped accruing at year ~20 and reported the rest of the span as
  // pure non-trading time -- a T_vol that is wrong by whole years, with a
  // plausible-looking value and no diagnostic. Only the closure window used to
  // stand in the way, and a caller-supplied calendar can declare a window wide
  // enough to clear it, which is exactly the reachable case here: a 25-year
  // window, a 21-year interval, every day covered.
  VolTimeParams p;
  p.alpha = 1.0;
  const VolTimeCalendar wide({}, day_utc(2000, 1, 1), day_utc(2025, 12, 31));
  const auto start = ns_utc(2000, 1, 3, 5, 0);
  const auto end = ns_utc(2021, 1, 4, 5, 0);  // ~21 years: past the loop bound
  const auto res = trading_hours_between(start, end, p, wide);
  ASSERT_FALSE(res.has_value()) << "silently reported " << *res
                                << " trading hours from a truncated day loop";
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);

  // Just inside the bound the same shape is answerable: 19 years of covered
  // sessions accrue (a full session is 6.5h at alpha 1, ~252 a year).
  const auto in_bound_end = ns_utc(2019, 1, 3, 5, 0);
  const double hours = ok(trading_hours_between(start, in_bound_end, p, wide));
  EXPECT_GT(hours, 19.0 * 250.0 * 6.5);
}

TEST(VolTime, DegenerateIntervalOutsideWindowStillReturnsZero) {
  // `end <= start` reads no calendar day at all, so it stays answerable
  // regardless of coverage — this is what keeps an already-expired contract on
  // a pre-2024 fixture a plain T <= 0, not a hard error.
  VolTimeParams p;
  const auto& cal = VolTimeCalendar::us_default();
  const auto t0 = ns_utc(2019, 6, 3, 14, 0);
  EXPECT_EQ(ok(vol_time_years(t0, t0, p, cal)), 0.0);
  EXPECT_EQ(ok(vol_time_years(t0, t0 - kHourNs, p, cal)), 0.0);
  EXPECT_EQ(ok(trading_hours_between(t0, t0, p, cal)), 0.0);
}

TEST(VolTime, TimeToExpiryVolTimeOutsideWindowFailsClosed) {
  // The production entry point (`time_to_expiry_years`, wired unconditionally
  // to `us_default()`) propagates the coverage error rather than handing a
  // caller a silently-wrong T.
  TimeSpec spec;
  spec.convention = TimeConvention::VolTime;
  const auto before = time_to_expiry_years(ns_utc(2021, 3, 1, 15, 0), ns_utc(2021, 4, 1, 20, 0),
                                           spec);
  ASSERT_FALSE(before.has_value());
  EXPECT_EQ(before.error().code(), ErrorCode::OutOfRange);

  const auto after = time_to_expiry_years(ns_utc(2030, 3, 1, 15, 0), ns_utc(2030, 4, 1, 20, 0),
                                          spec);
  ASSERT_FALSE(after.has_value());
  EXPECT_EQ(after.error().code(), ErrorCode::OutOfRange);
}

TEST(VolTime, TimeToExpiryCalendar365OutsideWindowIsUnaffected) {
  // Calendar365 reads no calendar, so the coverage guard must not touch it:
  // the whole historical fit/serve/backtest corpus rides this branch and every
  // pre-2024 / post-2028 fixture in it must keep working, bit-for-bit.
  const auto from = ns_utc(1998, 7, 2, 13, 30);
  const auto to = ns_utc(2031, 9, 19, 20, 0);
  const auto res = time_to_expiry_years(from, to, TimeSpec{});
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(*res, static_cast<double>(to - from) / atx::vol::kCalendarYearNs);
}

TEST(VolTime, InWindowVolTimeTIsUnchangedToTheLastBit) {
  // The coverage guard is a PRE-LOOP gate: an answerable in-window query must
  // come back bit-for-bit what it always did. Pinned literal (not a tolerance)
  // over a 6.5-week span that crosses 6 weekends and no closure: Wed 2026-07-08
  // 09:30 ET -> Fri 2026-08-21 16:00 ET, alpha = 0.7 default.
  //
  // REPINNED 2026-08-23 with the measured hour budget (1638/7122, 6.5h
  // session): 0.12692948406922205 -> 0.1273869699522606, +0.36%. The literal
  // was only ever a bit-exactness guard on the coverage gate, and it was
  // derived from the superseded 1890/6870 + 7.5h clock, so it moves with the
  // constants by construction -- it is not independent evidence for either.
  VolTimeParams p;
  const auto now = ns_utc(2026, 7, 8, 13, 30);     // 09:30 EDT
  const auto expiry = ns_utc(2026, 8, 21, 20, 0);  // 16:00 EDT
  const auto res = vol_time_years(now, expiry, p, VolTimeCalendar::us_default());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(*res, 0.1273869699522606);
}

TEST(VolTime, CustomCalendarWindowIsHonoredIndependentlyOfUsDefault) {
  // The window travels with the calendar, not with a global constant: a
  // caller-supplied table declares its own span and is trusted exactly there.
  VolTimeParams p;
  p.alpha = 1.0;
  // 2020 table covering June 2020 only, with Fri 2020-06-19 marked closed.
  VolTimeCalendar cal({day_utc(2020, 6, 19)}, day_utc(2020, 6, 1), day_utc(2020, 6, 30));
  const auto closed = ns_utc(2020, 6, 19, 4, 0);
  EXPECT_NEAR(ok(trading_hours_between(closed, closed + kDayNs, p, cal)), 0.0, 1e-9);
  const auto open_day = ns_utc(2020, 6, 18, 4, 0);
  EXPECT_NEAR(ok(trading_hours_between(open_day, open_day + kDayNs, p, cal)), 6.5, 1e-9);
  // One day past its declared window: unknown, therefore closed to queries.
  const auto outside = ns_utc(2020, 7, 1, 4, 0);
  EXPECT_FALSE(trading_hours_between(outside, outside + kDayNs, p, cal).has_value());
}

// ── time_to_expiry_years / TimeSpec (production T convention, I3) ─────────

// "YYYY-MM-DD HH:MM:SS", uniform over a wide civil-date range (day capped at
// 28 to sidestep month-length edge cases -- irrelevant to what this test
// checks: the Calendar365 arithmetic, not the ISO parser).
std::string random_iso_datetime(std::mt19937_64& rng) {
  std::uniform_int_distribution<int> year_d(2015, 2035);
  std::uniform_int_distribution<int> month_d(1, 12);
  std::uniform_int_distribution<int> day_d(1, 28);
  std::uniform_int_distribution<int> hour_d(0, 23);
  std::uniform_int_distribution<int> minute_d(0, 59);
  std::uniform_int_distribution<int> second_d(0, 59);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year_d(rng), month_d(rng),
               day_d(rng), hour_d(rng), minute_d(rng), second_d(rng));
  return std::string(buf);
}

TEST(VolTime, TimeToExpiryDefaultBitIdenticalToYearFraction) {
  // 20 random (from, to) instant pairs, round-tripped through the SAME ISO
  // parse (`iso_to_ns`) both `year_fraction` and `time_to_expiry_years` sit
  // on top of: the default-`TimeSpec` path must reproduce `year_fraction`'s
  // result EXACTLY (they now share one expression/constant -- see
  // vol_time.hpp's kCalendarYearNs doc), not just to within a tolerance.
  std::mt19937_64 rng(20260712ULL); // fixed seed: deterministic, reproducible
  for (int i = 0; i < 20; ++i) {
    const std::string from_iso = random_iso_datetime(rng);
    const std::string to_iso = random_iso_datetime(rng);
    const std::int64_t from_ns = iso_to_ns(from_iso);
    const std::int64_t to_ns = iso_to_ns(to_iso);
    ASSERT_NE(from_ns, std::int64_t{0}) << "bad fixture ISO: " << from_iso;
    ASSERT_NE(to_ns, std::int64_t{0}) << "bad fixture ISO: " << to_iso;

    const double expected = year_fraction(from_iso, to_iso);
    const auto actual = time_to_expiry_years(from_ns, to_ns, TimeSpec{});
    ASSERT_TRUE(actual.has_value()) << actual.error().to_string();
    EXPECT_EQ(*actual, expected) << "from=" << from_iso << " to=" << to_iso;
  }
}

// ── SessionInputs::time e2e (production fit/serve path, I3) ───────────────

// Flat-20%-smile, two-expiry synthetic board spanning several weekends.
[[nodiscard]] SynthPanelSpec flat_smile_spec() {
  SynthPanelSpec spec;
  spec.uid = "FLATVOL";
  spec.snapshot_iso = "2026-06-19";
  spec.spot = 100.0;
  spec.r = 0.03;
  const std::vector<std::string> isos = {"2026-09-18", "2027-03-19"};
  for (const std::string& iso : isos) {
    const double T = year_fraction(spec.snapshot_iso, iso);
    spec.expiries.push_back(SynthExpiry{iso, T, S3Params{0.20, 0.0, 0.0}});
  }
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K);
  }
  spec.half_spread_frac = 0.02;
  return spec;
}

TEST(VolTime, SessionFitUnderVolTimeServesConsistentGreeks) {
  const SynthPanelSpec spec = flat_smile_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const std::int64_t now_ns = iso_to_ns(spec.snapshot_iso);

  // ── VolTime-fit session (through from_frame: the production one-step
  // install+build path, whose guard requires in.time == frame.time) ───────
  TimeSpec vol_time_spec;
  vol_time_spec.convention = TimeConvention::VolTime;

  QuoteFrame frame_vol = panel->frame;
  frame_vol.time = vol_time_spec;  // the frame carries its own T convention

  SessionInputs in_vol;
  in_vol.S = spec.spot;
  in_vol.r = spec.r;
  in_vol.now_ts_ns = now_ns;
  in_vol.time = vol_time_spec;  // must match frame_vol.time (from_frame guard)
  const auto sess_vol = VolaSession::from_frame(frame_vol, in_vol);
  ASSERT_TRUE(sess_vol.has_value()) << sess_vol.error().to_string();  // (a) fit converges

  ASSERT_FALSE(sess_vol->expiries().empty());
  const double atm_T_vol = sess_vol->expiries().front().T;
  const double atm_F_vol = sess_vol->forward_at(atm_T_vol);
  ASSERT_GT(atm_F_vol, 0.0);

  const double iv_vol = sess_vol->iv(atm_F_vol, atm_T_vol);
  EXPECT_NEAR(iv_vol, 0.20, 0.01);  // (b) served ATM iv ~ 20%

  const auto greeks_vol = sess_vol->greeks(atm_F_vol, atm_T_vol, Side::Call);
  ASSERT_TRUE(greeks_vol.has_value()) << greeks_vol.error().to_string();
  EXPECT_LT(greeks_vol->theta, 0.0);  // (c) theta sign sane (time decay, long call)

  // ── (d) default TimeSpec bit-identical to a pinned pre-task value ──────
  // The pin IS the historical `data_install(u, frame)` call over an untouched
  // frame -- every OTHER caller in the codebase still uses exactly this
  // pattern (frame.time default-constructed), so it is by definition what
  // pre-I3 code produced and still produces today.
  Universe u_pin;
  const auto uid_pin = data_install(u_pin, panel->frame);  // untouched frame: the pin
  ASSERT_TRUE(uid_pin.has_value()) << uid_pin.error().to_string();
  const auto under_pin = u_pin.get_underlying(*uid_pin);
  ASSERT_TRUE(under_pin.has_value());

  QuoteFrame frame_default = panel->frame;
  frame_default.time = TimeSpec{};  // explicitly-set default == untouched default
  Universe u_default;
  const auto uid_default = data_install(u_default, frame_default);
  ASSERT_TRUE(uid_default.has_value()) << uid_default.error().to_string();
  const auto under_default = u_default.get_underlying(*uid_default);
  ASSERT_TRUE(under_default.has_value());

  ASSERT_EQ((*under_pin)->chains.size(), (*under_default)->chains.size());
  for (std::size_t i = 0; i < (*under_pin)->chains.size(); ++i) {
    EXPECT_EQ((*under_pin)->chains[i].T, (*under_default)->chains[i].T);
  }

  SessionInputs in_default;
  in_default.S = spec.spot;
  in_default.r = spec.r;
  in_default.now_ts_ns = now_ns;
  // in_default.time left default-constructed: TimeConvention::Calendar365.

  const auto sess_pin = VolaSession::build(**under_pin, in_default);
  const auto sess_default = VolaSession::build(**under_default, in_default);
  ASSERT_TRUE(sess_pin.has_value()) << sess_pin.error().to_string();
  ASSERT_TRUE(sess_default.has_value()) << sess_default.error().to_string();

  ASSERT_FALSE(sess_pin->expiries().empty());
  ASSERT_FALSE(sess_default->expiries().empty());
  const double atm_T_pin = sess_pin->expiries().front().T;
  const double atm_T_default = sess_default->expiries().front().T;
  EXPECT_EQ(atm_T_pin, atm_T_default);

  const double atm_F_pin = sess_pin->forward_at(atm_T_pin);
  const double atm_F_default = sess_default->forward_at(atm_T_default);
  EXPECT_EQ(atm_F_pin, atm_F_default);
  EXPECT_EQ(sess_pin->iv(atm_F_pin, atm_T_pin), sess_default->iv(atm_F_default, atm_T_default));

  const auto greeks_pin = sess_pin->greeks(atm_F_pin, atm_T_pin, Side::Call);
  const auto greeks_default = sess_default->greeks(atm_F_default, atm_T_default, Side::Call);
  ASSERT_TRUE(greeks_pin.has_value()) << greeks_pin.error().to_string();
  ASSERT_TRUE(greeks_default.has_value()) << greeks_default.error().to_string();
  EXPECT_EQ(*greeks_pin, *greeks_default);  // bit-identical AmericanGreeks bundle

  // Sanity: VolTime materially compresses the front-expiry T relative to
  // Calendar365 for this multi-month span with several intervening weekends.
  EXPECT_LT(atm_T_vol, atm_T_default);
}

// The mixed-convention guard: from_frame must REFUSE to build when the
// session's TimeSpec disagrees with the frame's own -- in either direction.
// (A silent mismatch would fit chains under one clock while the session
// records the other; the brief requires this to be impossible.)
TEST(VolTime, FromFrameRejectsMixedConventionSession) {
  const SynthPanelSpec spec = flat_smile_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  SessionInputs in;
  in.S = spec.spot;
  in.r = spec.r;
  in.now_ts_ns = iso_to_ns(spec.snapshot_iso);

  // (1) VolTime frame + default (Calendar365) SessionInputs::time -> rejected.
  QuoteFrame frame_vol = panel->frame;
  frame_vol.time.convention = TimeConvention::VolTime;
  const auto mixed_a = VolaSession::from_frame(frame_vol, in);
  ASSERT_FALSE(mixed_a.has_value());
  EXPECT_EQ(mixed_a.error().code(), ErrorCode::InvalidArgument);

  // (2) Default frame + VolTime SessionInputs::time -> rejected.
  SessionInputs in_vol = in;
  in_vol.time.convention = TimeConvention::VolTime;
  const auto mixed_b = VolaSession::from_frame(panel->frame, in_vol);
  ASSERT_FALSE(mixed_b.has_value());
  EXPECT_EQ(mixed_b.error().code(), ErrorCode::InvalidArgument);

  // (3) Matching specs (both directions) build fine.
  const auto ok_default = VolaSession::from_frame(panel->frame, in);
  EXPECT_TRUE(ok_default.has_value())
      << (ok_default.has_value() ? "" : ok_default.error().to_string());
  const auto ok_vol = VolaSession::from_frame(frame_vol, in_vol);
  EXPECT_TRUE(ok_vol.has_value())
      << (ok_vol.has_value() ? "" : ok_vol.error().to_string());
}

}  // namespace
