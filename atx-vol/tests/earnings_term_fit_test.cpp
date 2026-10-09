#include "atx/vol/api/analytics/earnings_term_fit.hpp"
#include "atx/vol/api/fitting/sr_tenor_grid.hpp"
#include "atx/vol/api/core/vol_time.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "atx/core/datetime.hpp"

// Coverage for the earnings term-fit core's pure primitives:
//   - `censored_atm_vol` (Task 1): per-expiry censored total-variance ->
//     censored ATM vol, see earnings_term_fit.hpp's own model comment.
//   - `SrTenorGrid`/`advance_trading_days`/`tenor_years` (Task 2): the
//     SpiderRock fixed TRADING-day tenor grid and its calendar-aware
//     tenor -> year-fraction converter, see sr_tenor_grid.hpp's own model
//     comment for why this is NOT a naive `N/365.25`.
//   - `fit_term_curve_for_emove`/`term_curve_value` (Task 3): the censored
//     term-curve fit -- for a FIXED eMove, search `decay` (1D, bounded) and
//     solve `{st,lt}` by linear LSQ at each candidate, see
//     earnings_term_fit.hpp's own model comment for the full contract.
//   - `fit_earnings_term` (Task 4): the joint {eMove,st,lt,decay} fit -- the
//     OUTER golden-section search over `emove` wrapping
//     `fit_term_curve_for_emove` as its inner solve, see
//     earnings_term_fit.hpp's own doc comment for the full contract.

namespace {

using atx::vol::advance_trading_days;
using atx::vol::censored_atm_vol;
using atx::vol::CensorObsInput;
using atx::vol::EarningsFitConfig;
using atx::vol::fit_earnings_term;
using atx::vol::fit_term_curve_for_emove;
using atx::vol::SrTenorGrid;
using atx::vol::tenor_years;
using atx::vol::term_curve_value;
using atx::vol::TermCurve;
using atx::vol::time_to_expiry_years;
using atx::vol::TimeSpec;
using atx::vol::VolTimeCalendar;

// Builds a UTC epoch-ns test fixture via atx-core's already-validated Hinnant
// civil-date math (`timestamp_from_utc`) -- independent of vol_time's/
// sr_tenor_grid's own internal day-index math, so these tests do not assume
// anything about how the code under test computes its day boundaries
// internally (mirrors vol_time_test.cpp's `ns_utc` helper).
std::int64_t ns_utc(int y, unsigned m, unsigned d, int hour, int minute) {
  return atx::core::time::timestamp_from_utc(y, m, d, static_cast<unsigned>(hour),
                                              static_cast<unsigned>(minute), 0U, 0U)
      .unix_nanos();
}

TEST(EarningsTermFit_CensoredAtmVol, StripsEventVariance) {
  // w_dirty = sigma_C^2*T + n*e^2 ; recover sigma_C.
  const double T = 0.25, sigmaC = 0.30, e = 0.06;
  const std::size_t n = 1;
  const double w = sigmaC * sigmaC * T + static_cast<double>(n) * e * e;
  const CensorObsInput o{T, w, n};
  EXPECT_NEAR(censored_atm_vol(o, e, 1e-10), sigmaC, 1e-9);
}

TEST(EarningsTermFit_CensoredAtmVol, FloorsOnOvershoot) {
  const CensorObsInput o{0.02, 1e-6, 3}; // n*e^2 >> w
  const double v = censored_atm_vol(o, 0.10, 1e-10);
  EXPECT_TRUE(std::isfinite(v));
  EXPECT_GE(v, 0.0);
}

// ── SrTenorGrid (Task 2) ────────────────────────────────────────────────────

TEST(SrTenorGrid, TradingDays_MatchTickerHistoryColumns) {
  // Matches tbltickerhistory's censored-term columns / nEarnCnt_Nd windows:
  // 21/mo, 63/qtr, 252/yr, all in TRADING days (not calendar days).
  constexpr std::array<int, 12> kExpected{5, 10, 21, 42, 63, 84, 105, 126, 189, 252, 378, 504};
  EXPECT_EQ(SrTenorGrid::kTradingDays, kExpected);
  EXPECT_EQ(SrTenorGrid::kTradingDays.front(), 5);
  EXPECT_EQ(SrTenorGrid::kTradingDays.back(), 504);
  EXPECT_EQ(SrTenorGrid::kTradingDays.size(), 12U);
}

TEST(SrTenorGrid, PresidentsDay2026_IsNyseHoliday) {
  // Precondition for AdvanceTradingDays_SkipsWeekendAndHoliday below: confirms
  // VolTimeCalendar::us_default() actually carries 2026-02-16 (Presidents
  // Day) as a full NYSE closure, per the header's claimed 2024-2032 coverage.
  constexpr std::int64_t kDayNs = 24LL * 3600LL * 1'000'000'000LL;
  const auto day = static_cast<std::int32_t>(ns_utc(2026, 2, 16, 0, 0) / kDayNs);
  EXPECT_TRUE(VolTimeCalendar::us_default().is_holiday(day));
}

TEST(SrTenorGrid, AdvanceTradingDays_SkipsWeekendAndHoliday) {
  // Fri 2026-02-13 16:00 UTC + 1 trading day: Sat 02-14/Sun 02-15 are
  // weekend, Mon 02-16 is NYSE Presidents Day (confirmed above) -- the next
  // trading day is Tue 02-17. Asserts the EXACT resulting civil date/time
  // (not just "> fri"), so this catches an off-by-one-day bug the weaker
  // brief-suggested `EXPECT_GT` would miss.
  const std::int64_t fri = ns_utc(2026, 2, 13, 16, 0);
  const auto& cal = VolTimeCalendar::us_default();
  const auto next = advance_trading_days(fri, 1, cal);
  ASSERT_TRUE(next.has_value()) << next.error().to_string();
  EXPECT_EQ(*next, ns_utc(2026, 2, 17, 16, 0));  // intraday time-of-day preserved
}

TEST(SrTenorGrid, AdvanceTradingDays_SkipsPlainWeekendNoHoliday) {
  // Wed 2026-07-08 (regular trading week, no holiday nearby) + 3 trading days
  // -> Mon 2026-07-13 (Thu 07-09, Fri 07-10, then Sat/Sun skipped, Mon
  // 07-13 is the 3rd trading day).
  const std::int64_t wed = ns_utc(2026, 7, 8, 20, 30);
  const auto next = advance_trading_days(wed, 3, VolTimeCalendar::us_default());
  ASSERT_TRUE(next.has_value()) << next.error().to_string();
  EXPECT_EQ(*next, ns_utc(2026, 7, 13, 20, 30));
}

TEST(SrTenorGrid, AdvanceTradingDays_ZeroIsNoOp) {
  const std::int64_t now = ns_utc(2026, 2, 13, 16, 0);
  const auto same = advance_trading_days(now, 0, VolTimeCalendar::us_default());
  ASSERT_TRUE(same.has_value()) << same.error().to_string();
  EXPECT_EQ(*same, now);
}

// Fail-closed (defect 1.10, sr_tenor_grid): a day the calendar does not COVER
// must never be counted as a trading day. Mon 2026-02-16 is a real NYSE full
// closure (pinned by PresidentsDay2026_IsNyseHoliday above), but a calendar whose
// table stops on Fri 2026-02-13 cannot say so -- `is_holiday` answers false for
// "not a listed closure" and for "outside the table" alike, and the stepper used
// to read that false as "open", returning Presidents Day itself as the next
// trading day. Every tenor derived from such a step was wrong with no diagnostic.
TEST(SrTenorGrid, AdvanceTradingDays_OutsideCalendarWindowFailsClosed) {
  constexpr std::int64_t kDayNs = 24LL * 3600LL * 1'000'000'000LL;
  const auto first = static_cast<std::int32_t>(ns_utc(2026, 2, 9, 0, 0) / kDayNs);
  const auto last = static_cast<std::int32_t>(ns_utc(2026, 2, 13, 0, 0) / kDayNs);
  const VolTimeCalendar narrow{std::vector<std::int32_t>{}, first, last};
  const std::int64_t fri = ns_utc(2026, 2, 13, 16, 0);
  const auto out = advance_trading_days(fri, 1, narrow);
  ASSERT_FALSE(out.has_value()) << "silently counted the uncovered 2026-02-16 closure";
  EXPECT_EQ(out.error().code(), atx::vol::ErrorCode::OutOfRange);
}

// The window gates exactly the days whose closure status can MOVE the answer.
// Weekends are decided without the table (a Saturday is a Saturday at any date),
// so a step whose every counted day is covered succeeds even though the stepper
// walked across an intervening weekend.
TEST(SrTenorGrid, AdvanceTradingDays_WithinCoveredDaysSucceedsAcrossAWeekend) {
  constexpr std::int64_t kDayNs = 24LL * 3600LL * 1'000'000'000LL;
  const auto first = static_cast<std::int32_t>(ns_utc(2026, 7, 6, 0, 0) / kDayNs);  // Mon
  const auto last = static_cast<std::int32_t>(ns_utc(2026, 7, 13, 0, 0) / kDayNs);  // next Mon
  const VolTimeCalendar narrow{std::vector<std::int32_t>{}, first, last};
  const std::int64_t thu = ns_utc(2026, 7, 9, 20, 30);
  const auto out = advance_trading_days(thu, 2, narrow); // Fri 07-10, [weekend], Mon 07-13
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  EXPECT_EQ(*out, ns_utc(2026, 7, 13, 20, 30));
}

// The bounded-loop guard reports instead of returning a truncated day: a calendar
// that closes every day in the stepped window has no n-th trading day at all, and
// the pre-Result code returned whatever day the bound stopped on (debug-asserted,
// silent in release).
TEST(SrTenorGrid, AdvanceTradingDays_NoTradingDayWithinTheStepBoundFailsClosed) {
  constexpr std::int64_t kDayNs = 24LL * 3600LL * 1'000'000'000LL;
  const auto start = static_cast<std::int32_t>(ns_utc(2026, 7, 6, 0, 0) / kDayNs);
  std::vector<std::int32_t> every_day;
  every_day.reserve(60);
  for (std::int32_t d = start; d < start + 60; ++d) {
    every_day.push_back(d);
  }
  const VolTimeCalendar shut{std::move(every_day), start, start + 60};
  const auto out = advance_trading_days(ns_utc(2026, 7, 6, 16, 0), 1, shut);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), atx::vol::ErrorCode::OutOfRange);
}

// A negative horizon is a caller contract violation, not a no-op: it used to
// degrade to "return now_ns" in release (assert-only in debug), which reads
// downstream as a legitimate zero-day tenor.
TEST(SrTenorGrid, AdvanceTradingDays_NegativeHorizonIsRejected) {
  const auto out =
      advance_trading_days(ns_utc(2026, 2, 13, 16, 0), -1, VolTimeCalendar::us_default());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(SrTenorGrid, TenorYears_252TdApproxOneYear) {
  const std::int64_t now = ns_utc(2026, 2, 13, 16, 0);
  const auto y = tenor_years(now, 252, TimeSpec{}); // Calendar365 default
  ASSERT_TRUE(y.has_value()) << y.error().to_string();
  EXPECT_GT(*y, 0.95);
  EXPECT_LT(*y, 1.05);
}

TEST(SrTenorGrid, TenorYears_ComposesAdvanceAndTimeToExpiry) {
  // Definitional check: tenor_years must equal time_to_expiry_years applied
  // to advance_trading_days's own output, under us_default() -- i.e. no
  // hidden second calendar or convention path.
  const std::int64_t now = ns_utc(2026, 2, 13, 16, 0);
  const TimeSpec spec{};
  const auto expiry = advance_trading_days(now, 21, VolTimeCalendar::us_default());
  ASSERT_TRUE(expiry.has_value()) << expiry.error().to_string();
  const auto composed = tenor_years(now, 21, spec);
  const auto direct = time_to_expiry_years(now, *expiry, spec);
  ASSERT_TRUE(composed.has_value()) << composed.error().to_string();
  ASSERT_TRUE(direct.has_value()) << direct.error().to_string();
  EXPECT_DOUBLE_EQ(*composed, *direct);
}

// `tenor_years` propagates the stepper's coverage error rather than folding it
// into a plausible-looking year-fraction -- including on the DEFAULT Calendar365
// convention, whose year-fraction reads no calendar but whose EXPIRY INSTANT is
// still produced by the trading-day walk. Mon 2032-12-20 leaves only eight
// covered sessions in the year (12-24 is the observed Christmas closure), so a
// 21 trading-day walk from it leaves us_default()'s 2024-2032 window.
TEST(SrTenorGrid, TenorYears_StepOutsideTheCalendarWindowFailsClosed) {
  const std::int64_t now = ns_utc(2032, 12, 20, 16, 0);
  const auto y = tenor_years(now, 21, TimeSpec{}); // Calendar365 default
  ASSERT_FALSE(y.has_value());
  EXPECT_EQ(y.error().code(), atx::vol::ErrorCode::OutOfRange);
}

// ── fit_term_curve_for_emove / term_curve_value (Task 3) ───────────────────

TEST(EarningsTermFit_FitTermCurve, RecoversPlantedCurve) {
  // Pure diffusive (no events, n=0), no censoring noise: w_dirty = sc^2*T
  // exactly reproduces sc via censored_atm_vol at emove=0, so the 2x2 LSQ +
  // decay search should recover {st,lt,decay} to near machine precision.
  const double st = 0.45, lt = 0.25, decay = 3.0, e = 0.0;
  std::vector<CensorObsInput> obs;
  for (const double T : {0.02, 0.05, 0.10, 0.25, 0.5, 1.0, 2.0}) {
    const double sc = lt + (st - lt) * std::exp(-decay * T);
    obs.push_back({T, sc * sc * T, 0}); // pure diffusive, n=0
  }
  const EarningsFitConfig cfg{};
  const TermCurve c = fit_term_curve_for_emove(obs, e, cfg);
  EXPECT_NEAR(c.st, st, 1e-4);
  EXPECT_NEAR(c.lt, lt, 1e-4);
  EXPECT_NEAR(c.decay, decay, 1e-2);
  EXPECT_LT(c.rms_resid, 1e-6);
  EXPECT_NEAR(term_curve_value(c, 0.25), lt + (st - lt) * std::exp(-decay * 0.25), 1e-4);
}

TEST(EarningsTermFit_FitTermCurve, EmptyObsReturnsFiniteFlatCurve) {
  // No listed expiries -> the per-decay 2x2 Gram matrix is identically
  // zero (n=0) at every candidate decay, i.e. maximally singular. This must
  // fall back to a finite flat curve rather than divide by a zero
  // determinant or propagate NaN/Inf.
  const std::vector<CensorObsInput> obs;
  const EarningsFitConfig cfg{};
  const TermCurve c = fit_term_curve_for_emove(obs, 0.05, cfg);
  EXPECT_TRUE(std::isfinite(c.st));
  EXPECT_TRUE(std::isfinite(c.lt));
  EXPECT_GT(c.decay, 0.0); // decay is searched strictly > 0
  EXPECT_TRUE(std::isfinite(c.rms_resid));
  EXPECT_GE(c.rms_resid, 0.0);
}

TEST(EarningsTermFit_FitTermCurve, AllEqualTGuardsSingularSolve) {
  // Every observation shares one T: at any candidate decay, b_i=exp(-decay*T)
  // is the SAME constant for every i, so the {1, b} basis collapses onto one
  // column and the 2x2 normal-equations determinant is exactly 0. Must still
  // return a finite (flat, A=0) fit -- never divide by that zero determinant.
  std::vector<CensorObsInput> obs;
  const double T = 0.5;
  for (const double sigma : {0.20, 0.22, 0.18}) {
    obs.push_back({T, sigma * sigma * T, 0});
  }
  const EarningsFitConfig cfg{};
  const TermCurve c = fit_term_curve_for_emove(obs, 0.0, cfg);
  EXPECT_TRUE(std::isfinite(c.st));
  EXPECT_TRUE(std::isfinite(c.lt));
  EXPECT_GT(c.decay, 0.0);
  EXPECT_TRUE(std::isfinite(c.rms_resid));
  EXPECT_GE(c.rms_resid, 0.0);
}

// ── fit_earnings_term (Task 4) ──────────────────────────────────────────────

TEST(EarningsTermFit_FitEarningsTerm, RecoversEmoveAndCurve) {
  // Planted {st,lt,decay,emove}: dirty w = censored(planted curve) +
  // n*emove^2, event counts strictly increasing with T so eMove is actually
  // identified (unlike the AllZeroEvents / all-n-equal under-identified
  // cases below). The outer golden-section search over emove should recover
  // the planted quadruple to well within its bracket's convergence floor.
  const double st = 0.55, lt = 0.28, decay = 4.0, e = 0.07;
  struct P {
    double T;
    std::size_t n;
  };
  std::vector<P> pts{{0.03, 1}, {0.06, 1}, {0.12, 1}, {0.25, 2}, {0.5, 2}, {1.0, 2}, {2.0, 3}};
  std::vector<CensorObsInput> obs;
  for (auto p : pts) {
    const double sc = lt + (st - lt) * std::exp(-decay * p.T);
    const double w = sc * sc * p.T + static_cast<double>(p.n) * e * e; // dirty = censored + event
    obs.push_back({p.T, w, p.n});
  }
  // 12 tenor year-fractions (calendar-approx here; the tool uses tenor_years
  // from Task 2).
  std::array<double, 12> tt{};
  const std::array<int, 12> td{5, 10, 21, 42, 63, 84, 105, 126, 189, 252, 378, 504};
  for (std::size_t i = 0; i < 12; ++i) {
    tt[i] = static_cast<double>(td[i]) / 252.0; // trading-day basis
  }
  EarningsFitConfig cfg{};
  cfg.tenor_T = tt;
  const auto r = fit_earnings_term(obs, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->emove, e, 2e-3);
  EXPECT_NEAR(r->st, st, 3e-3);
  EXPECT_NEAR(r->lt, lt, 3e-3);
  // decay is only weakly constrained by a curve-fit's own T-spread alone
  // (see fit_term_curve_for_emove's own RecoversPlantedCurve test, which
  // uses the same 1e-2 tolerance for an equivalent noiseless-model
  // recovery); pinning it here (not just emove/st/lt) closes the gap where
  // a gross decay regression could otherwise slip through this, the core
  // joint-fit test, undetected.
  EXPECT_NEAR(r->decay, decay, 1e-2);
  EXPECT_EQ(r->fit_code, atx::vol::EmoveFitCode::Minimum);
  EXPECT_EQ(r->expiry_count, obs.size());
  ASSERT_EQ(r->atm_cen.size(), 12u);
  EXPECT_NEAR(r->atm_cen[0], lt + (st - lt) * std::exp(-decay * tt[0]), 3e-3); // 5d parametric read
}

TEST(EarningsTermFit_FitEarningsTerm, AllZeroEvents_EmoveZero) {
  // No scheduled event before either listed expiry: emove is not
  // identifiable from this obs set at all (nothing to censor out) -- this is
  // the caller's ex-event curve, still Ok (not Err), per the brief's special
  // case.
  std::vector<CensorObsInput> obs{{0.05, 0.3 * 0.3 * 0.05, 0}, {0.5, 0.28 * 0.28 * 0.5, 0}};
  const auto r = fit_earnings_term(obs, EarningsFitConfig{});
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->emove, 0.0, 1e-9);
  EXPECT_EQ(r->fit_code, atx::vol::EmoveFitCode::CenterFlat);
  EXPECT_EQ(r->expiry_count, obs.size());
}

TEST(EarningsTermFit_FitEarningsTerm, TooFewObs_InvalidArgument) {
  const std::vector<CensorObsInput> obs{{0.25, 0.05, 1}};
  const auto r = fit_earnings_term(obs, EarningsFitConfig{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(EarningsTermFit_FitEarningsTerm, NonPositiveT_InvalidArgument) {
  const std::vector<CensorObsInput> obs{{0.0, 0.05, 1}, {0.5, 0.04, 1}};
  const auto r = fit_earnings_term(obs, EarningsFitConfig{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(EarningsTermFit_FitEarningsTerm, NonPositiveWDirty_InvalidArgument) {
  const std::vector<CensorObsInput> obs{{0.25, 0.0, 1}, {0.5, 0.04, 1}};
  const auto r = fit_earnings_term(obs, EarningsFitConfig{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(EarningsTermFit_FitEarningsTerm, OptimumAboveBracket_RightBound) {
  // Same planted data as RecoversEmoveAndCurve (true emove=0.07), but the
  // search bracket's upper end (0.02) sits BELOW the true value -- the
  // objective keeps improving all the way to emove_hi within the bracket, so
  // the search should pin at the right bound rather than report an interior
  // Minimum.
  const double st = 0.55, lt = 0.28, decay = 4.0, e = 0.07;
  struct P {
    double T;
    std::size_t n;
  };
  std::vector<P> pts{{0.03, 1}, {0.06, 1}, {0.12, 1}, {0.25, 2}, {0.5, 2}, {1.0, 2}, {2.0, 3}};
  std::vector<CensorObsInput> obs;
  for (auto p : pts) {
    const double sc = lt + (st - lt) * std::exp(-decay * p.T);
    const double w = sc * sc * p.T + static_cast<double>(p.n) * e * e;
    obs.push_back({p.T, w, p.n});
  }
  EarningsFitConfig cfg{};
  cfg.emove_hi = 0.02;
  const auto r = fit_earnings_term(obs, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->fit_code, atx::vol::EmoveFitCode::RightBound);
  EXPECT_NEAR(r->emove, cfg.emove_hi, 1e-4);
}

TEST(EarningsTermFit_FitEarningsTerm, OptimumBelowBracket_LeftBound) {
  // Mirrors OptimumAboveBracket_RightBound: true emove=0.07 sits BELOW the
  // search bracket's lower end (0.10) this time -- the objective keeps
  // improving all the way down to emove_lo within the bracket, so the search
  // should pin at the left bound.
  const double st = 0.55, lt = 0.28, decay = 4.0, e = 0.07;
  struct P {
    double T;
    std::size_t n;
  };
  std::vector<P> pts{{0.03, 1}, {0.06, 1}, {0.12, 1}, {0.25, 2}, {0.5, 2}, {1.0, 2}, {2.0, 3}};
  std::vector<CensorObsInput> obs;
  for (auto p : pts) {
    const double sc = lt + (st - lt) * std::exp(-decay * p.T);
    const double w = sc * sc * p.T + static_cast<double>(p.n) * e * e;
    obs.push_back({p.T, w, p.n});
  }
  EarningsFitConfig cfg{};
  cfg.emove_lo = 0.10;
  cfg.emove_hi = 0.30;
  const auto r = fit_earnings_term(obs, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->fit_code, atx::vol::EmoveFitCode::LeftBound);
  EXPECT_NEAR(r->emove, cfg.emove_lo, 1e-4);
}

TEST(EarningsTermFit_FitEarningsTerm, ZeroMaxIters_MaxSteps) {
  // cfg.max_iters == 0: the golden-section refine loop never runs, so the
  // local refine bracket (roughly two coarse-grid steps wide) never
  // collapses below the convergence tolerance -- MaxSteps takes priority
  // over whatever bound/flat diagnosis the (unconverged) bracket might
  // otherwise suggest.
  const double st = 0.55, lt = 0.28, decay = 4.0, e = 0.07;
  struct P {
    double T;
    std::size_t n;
  };
  std::vector<P> pts{{0.03, 1}, {0.06, 1}, {0.12, 1}, {0.25, 2}, {0.5, 2}, {1.0, 2}, {2.0, 3}};
  std::vector<CensorObsInput> obs;
  for (auto p : pts) {
    const double sc = lt + (st - lt) * std::exp(-decay * p.T);
    const double w = sc * sc * p.T + static_cast<double>(p.n) * e * e;
    obs.push_back({p.T, w, p.n});
  }
  EarningsFitConfig cfg{};
  cfg.max_iters = 0;
  const auto r = fit_earnings_term(obs, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->fit_code, atx::vol::EmoveFitCode::MaxSteps);
}

TEST(EarningsTermFit_FitEarningsTerm, SingleFrontFloor_DoesNotBiasEmoveDown) {
  // Same planted curve/emove as RecoversEmoveAndCurve (st=0.55, lt=0.28,
  // decay=4.0, e=0.07), but the FRONT observation (T=0.03) is a noisy/stale
  // quote: its dirty total variance is fixed at n*0.04^2, so its RAW
  // censored variance (w_dirty - n*emove^2) crosses the floor at
  // emove ~= 0.04 -- WELL below the true emove=0.07 -- and stays floored
  // for every candidate emove from there up through the [0.25,0.30]
  // overshoot plateau. The TRUE optimum itself therefore floors this one
  // front point. For that floor to bind exactly at the planted emove, the
  // front quote's OWN implied diffusive vol must genuinely be ~0 (a
  // structural consequence of "floors at e", not an arbitrarily extreme
  // choice) -- so this single stale point legitimately perturbs the
  // (unweighted, 7-point) LSQ fit; the fix's job is to stop the search from
  // being wrongly EXCLUDED from the whole region beyond the clamp, not to
  // make the fit immune to a real outlier's influence.
  //
  // Verified (Python replica of this exact algorithm, both pre- and
  // post-fix, against this exact dataset) that the pre-fix bug -- ranking
  // ANY candidate that floors even one event-bearing observation as
  // +infinity -- disqualifies the ENTIRE [~0.04, 0.30] region outright
  // (including the true optimum at 0.07) and pins the search at
  // emove_lo=0.0, the search bracket's OWN left edge -- nowhere near 0.04,
  // let alone 0.07. Post-fix, with the other 6 event-bearing points still a
  // strict majority non-floored, the search instead finds a genuine
  // interior `Minimum` well past the clamp threshold, in the true
  // optimum's neighborhood (not laser-precise, given the legitimate
  // single-outlier perturbation above, but a real, substantial correction
  // toward the truth, not away from it).
  const double st = 0.55, lt = 0.28, decay = 4.0, e = 0.07;
  struct P {
    double T;
    std::size_t n;
  };
  std::vector<P> pts{{0.06, 1}, {0.12, 1}, {0.25, 2}, {0.5, 2}, {1.0, 2}, {2.0, 3}};
  std::vector<CensorObsInput> obs;
  for (auto p : pts) {
    const double sc = lt + (st - lt) * std::exp(-decay * p.T);
    const double w = sc * sc * p.T + static_cast<double>(p.n) * e * e;
    obs.push_back({p.T, w, p.n});
  }
  // Front point: T=0.03, n=1, dirty total variance fixed at 1*0.04^2 (NOT
  // the planted-curve value) -- a stale quote whose implied diffusive vol
  // is ~0 once censored, clamping at the floor for emove >= ~0.04.
  obs.insert(obs.begin(), CensorObsInput{0.03, 0.04 * 0.04, 1});

  std::array<double, 12> tt{};
  const std::array<int, 12> td{5, 10, 21, 42, 63, 84, 105, 126, 189, 252, 378, 504};
  for (std::size_t i = 0; i < 12; ++i) {
    tt[i] = static_cast<double>(td[i]) / 252.0;
  }
  EarningsFitConfig cfg{};
  cfg.tenor_T = tt;
  const auto r = fit_earnings_term(obs, cfg);
  ASSERT_TRUE(r.has_value());
  // A genuine interior optimum was found (not forced to the emove_lo bound
  // the pre-fix bug produces), clearly past the ~0.04 clamp threshold --
  // demonstrating the search actually explored and selected from the
  // region beyond the clamp instead of being excluded from it -- and within
  // a generous (given the legitimate single-outlier perturbation explained
  // above), but still discriminating, distance of the true planted 0.07:
  // comfortably rules out both the pre-fix emove=0.0 result and the
  // "excluded entirely / pinned at the clamp threshold" failure mode this
  // fix targets.
  EXPECT_EQ(r->fit_code, atx::vol::EmoveFitCode::Minimum);
  EXPECT_GT(r->emove, 0.05);
  EXPECT_NEAR(r->emove, e, 0.08);
}

TEST(EarningsTermFit_FitEarningsTerm, AllNEqualNonzero_ClassifiesMinimum) {
  // Every observation shares the SAME nonzero event count (n=1): a constant
  // event lump is only WEAKLY identified -- separable, in principle, from
  // the T-scaled shape of the term curve itself, since a flexible-enough
  // curve can partially compensate -- but it IS identified: the objective
  // still has a genuine, if shallow, local minimum. This must classify
  // Minimum, NOT CenterFlat (CenterFlat is reserved for the all-n==0
  // special case and a genuinely flat objective; pins the corrected
  // contract after the review found the module's prior doc/behavior
  // conflated "weakly identified" with "flat").
  const double st = 0.50, lt = 0.30, decay = 3.0, e = 0.05;
  struct P {
    double T;
    std::size_t n;
  };
  std::vector<P> pts{{0.05, 1}, {0.10, 1}, {0.25, 1}, {0.5, 1}, {1.0, 1}, {2.0, 1}};
  std::vector<CensorObsInput> obs;
  for (auto p : pts) {
    const double sc = lt + (st - lt) * std::exp(-decay * p.T);
    const double w = sc * sc * p.T + static_cast<double>(p.n) * e * e;
    obs.push_back({p.T, w, p.n});
  }
  const auto r = fit_earnings_term(obs, EarningsFitConfig{});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(std::isfinite(r->emove));
  EXPECT_EQ(r->fit_code, atx::vol::EmoveFitCode::Minimum);
}

} // namespace
