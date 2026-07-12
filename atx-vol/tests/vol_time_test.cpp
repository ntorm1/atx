#include "atx/vol/vol_time.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "atx/core/datetime.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/universe.hpp"

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
    const double actual = time_to_expiry_years(from_ns, to_ns, TimeSpec{});
    EXPECT_EQ(actual, expected) << "from=" << from_iso << " to=" << to_iso;
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
