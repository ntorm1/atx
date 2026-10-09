#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

#include "atx/vol/api/pricing/rates_curve.hpp"

// Curve-set coverage, ported from the C ats-vol tests:
//   test_curve_cubic.c      -> YieldCurve (Fritsch-Carlson cubic Hermite)
//   test_curve_dividends.c  -> DividendForward (forward_div_corrected)
//   test_curve_htb.c        -> HtbDetector (ForwardCurve::detect_htb)
//
// Omitted from the C suite: the perf-gated `eval_throughput_under_200ns`
// (an environment-specific micro-benchmark, not a correctness check), and
// `refit_forward_consumes_projected_schedule` /
// `refit_forward_flags_inconsistent_when_schedule_stale` (exercise
// `ats_vol_curve_refit_forward_ex` against `AtsVolUniverse`/`AtsVolChain` —
// the live PCP-refit-from-quotes engine and universe types are explicitly
// out of scope for this port; see atx/vol/rates_curve.hpp's header comment).

namespace {

using atx::vol::CurveSet;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::forward_div_corrected;
using atx::vol::ForwardCurve;
using atx::vol::ForwardFlag;
using atx::vol::ForwardPoint;
using atx::vol::has_flag;
using atx::vol::HtbDetector;
using atx::vol::HtbResult;
using atx::vol::YieldCurve;

// ── YieldCurve fixture: 11-pillar OIS-style curve (matches
// test_curve_cubic.c's make_treasury_curve) ────────────────────────────────

constexpr double kPillarT[] = {1.0 / 365.25, 7.0 / 365.25, 14.0 / 365.25,
                               1.0 / 12.0,   2.0 / 12.0,   3.0 / 12.0,
                               6.0 / 12.0,   9.0 / 12.0,   1.0,
                               1.5,          2.0};
constexpr double kPillarR[] = {0.0405, 0.0410, 0.0415, 0.0420, 0.0425,
                               0.0430, 0.0440, 0.0450, 0.0455, 0.0460, 0.0465};

atx::vol::Result<YieldCurve> make_treasury_curve() {
  return YieldCurve::create(kPillarT, kPillarR);
}

TEST(YieldCurve, DfAtKnots_IsExact) {
  const auto built = make_treasury_curve();
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  for (std::size_t i = 0; i < std::size(kPillarT); ++i) {
    const double df_recovered = c.disc(kPillarT[i]);
    const double df_expected = std::exp(-kPillarR[i] * kPillarT[i]);
    EXPECT_LT(std::fabs(df_recovered - df_expected), 1e-12);
  }
}

TEST(YieldCurve, OffKnotValue_LiesInBracket) {
  const auto built = make_treasury_curve();
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  // For a monotone-decreasing log_df sequence (positive rates), the
  // Hermite interpolant stays monotone-decreasing: each off-knot DF lies
  // between its two bracketing knot DFs.
  for (std::size_t i = 0; i + 1 < std::size(kPillarT); ++i) {
    const double t_mid = 0.5 * (kPillarT[i] + kPillarT[i + 1]);
    const double df = c.disc(t_mid);
    const double df_lo = std::exp(-kPillarR[i + 1] * kPillarT[i + 1]); // later T -> smaller DF
    const double df_hi = std::exp(-kPillarR[i] * kPillarT[i]);         // earlier T -> larger DF
    EXPECT_GE(df, df_lo - 1e-9);
    EXPECT_LE(df, df_hi + 1e-9);
  }
}

TEST(YieldCurve, FritschCarlson_StrictlyMonotone) {
  const auto built = make_treasury_curve();
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  // Sweep 401 points over (0, last_pillar] and assert disc() is
  // non-increasing in T (positive zero rates -> monotone DF).
  const double t_max = kPillarT[std::size(kPillarT) - 1];
  double prev_df = 1.5; // anything > exp(0)
  for (int i = 0; i <= 400; ++i) {
    const double T = static_cast<double>(i) / 400.0 * t_max;
    if (T <= 0.0) {
      continue;
    }
    const double df = c.disc(T);
    EXPECT_LE(df, prev_df + 1e-12);
    prev_df = df;
  }
}

TEST(YieldCurve, Hermite_SmootherThanLinear_LowCurvature) {
  const auto built = make_treasury_curve();
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  // The Hermite interpolant should be smoother than a piecewise-linear one
  // on curved log_df input: the second difference at three nearby
  // off-knot points stays well below an empirical bound.
  const double T = 0.5; // between the 6m and 9m pillars
  const double h = 0.001;
  const double dlo = c.disc(T - h);
  const double dmi = c.disc(T);
  const double dhi = c.disc(T + h);
  const double curvature = std::fabs(dlo - 2.0 * dmi + dhi);
  EXPECT_LT(curvature, 1.0e-6);
}

// ── T2: short/long-end extrapolation is flat RATE, not flat DF ────────────
//
// disc() used to clamp the DISCOUNT FACTOR flat outside the pillar range. Below
// the first pillar that makes zero(T) = -log_df.front()/T = r0*t0/T, which blows
// up as T -> 0: with this fixture's pillars (t0 = 1/365.25, r0 = 4.05%) one hour
// read 97.2% and five minutes read 1108.8%. Every 0DTE/intraday path through
// MarketEnv::rate_at inherits zero(), and no test used to call it below t0.
// Above the last pillar the same clamp implies a ZERO instantaneous forward rate,
// so zero(T) = rN*tN/T decays toward 0 — the symmetric error.
//
// Both ends are now flat ZERO RATE, which is what the class contract ("zero rate
// at T", flat extrapolation) already reads as and what a market curve means by
// flat extrapolation.

// Sub-pillar year fractions: 5 min, 1 h, 12 h, 1 day (== the first pillar).
constexpr double kMin5 = 5.0 / (60.0 * 24.0 * 365.25);
constexpr double kHour1 = 1.0 / (24.0 * 365.25);
constexpr double kHour12 = 12.0 / (24.0 * 365.25);

TEST(YieldCurve, ZeroBelowFirstPillar_StaysAtTheFirstPillarRate) {
  const auto built = make_treasury_curve();
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  for (const double T : {kMin5, kHour1, kHour12, kPillarT[0]}) {
    const double z = c.zero(T);
    EXPECT_NEAR(z, kPillarR[0], 1e-12)
        << "zero(" << T << ") = " << z << " — flat-DF extrapolation divides by T";
  }
}

TEST(YieldCurve, DiscBelowFirstPillar_IsFlatRateAndTendsToOne) {
  const auto built = make_treasury_curve();
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  for (const double T : {kMin5, kHour1, kHour12}) {
    EXPECT_NEAR(c.disc(T), std::exp(-kPillarR[0] * T), 1e-15);
  }
  // The value date discounts to itself, and T <= 0 has no rate (zero() already
  // returns 0 there), so the two must agree: disc(0) == 1.
  EXPECT_DOUBLE_EQ(c.disc(0.0), 1.0);
  EXPECT_DOUBLE_EQ(c.disc(-1.0), 1.0);
  EXPECT_DOUBLE_EQ(c.zero(0.0), 0.0);
  // Continuity at the pillar: the flat-rate arm meets the pillar DF exactly.
  EXPECT_DOUBLE_EQ(c.disc(kPillarT[0]), std::exp(-kPillarR[0] * kPillarT[0]));
}

TEST(YieldCurve, ZeroAboveLastPillar_StaysAtTheLastPillarRate) {
  const auto built = make_treasury_curve();
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  constexpr std::size_t kLast = std::size(kPillarT) - 1;
  for (const double T : {kPillarT[kLast], 3.0, 5.0, 10.0, 30.0}) {
    EXPECT_NEAR(c.zero(T), kPillarR[kLast], 1e-12)
        << "zero(" << T << ") — flat-DF extrapolation implies a zero forward rate";
    EXPECT_NEAR(c.disc(T), std::exp(-kPillarR[kLast] * T), 1e-15);
  }
}

// A single-pillar curve is flat everywhere: no interior bracket exists, so both
// extrapolation arms have to carry it.
TEST(YieldCurve, SinglePillar_IsFlatRateEverywhere) {
  const double t[] = {1.0};
  const double r[] = {0.042};
  const auto built = YieldCurve::create(t, r);
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;
  for (const double T : {kMin5, 0.25, 1.0, 7.0}) {
    EXPECT_NEAR(c.zero(T), 0.042, 1e-12);
    EXPECT_NEAR(c.disc(T), std::exp(-0.042 * T), 1e-15);
  }
}

// REVL1-F4: a t = 0 first pillar has NO short end — every T > 0 is already inside
// the pillar range, so the interior interpolant handles it and the short-end arm is
// unreachable. The previous version of this test could not fail on any
// implementation: disc(-0.5)/disc(0.0) return through the unconditional T <= 0
// guard and zero(1e-6) exercises the Hermite interior either way. This one probes
// the branch that actually runs.
TEST(YieldCurve, ZeroFirstPillar_ShortEndIsTheInteriorInterpolant) {
  const double t[] = {0.0, 1.0};
  const double r[] = {0.04, 0.04};
  const auto built = YieldCurve::create(t, r);
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;

  // log_df is (0, -0.04): a flat-DF clamp would return exp(log_df.front()) == 1.0
  // for every T below the first pillar. There is no such T here, so 1e-6 must be
  // interpolated, not clamped.
  const double df = c.disc(1.0e-6);
  ASSERT_TRUE(std::isfinite(df));
  EXPECT_LT(df, 1.0);
  EXPECT_NEAR(df, std::exp(-0.04 * 1.0e-6), 1.0e-12);
  EXPECT_TRUE(std::isfinite(c.zero(1.0e-6)));
  EXPECT_NEAR(c.zero(1.0e-6), 0.04, 1.0e-6);

  // T <= 0 still discounts to 1, and the long end still holds the rate flat.
  EXPECT_DOUBLE_EQ(c.disc(-0.5), 1.0);
  EXPECT_DOUBLE_EQ(c.disc(0.0), 1.0);
  EXPECT_NEAR(c.zero(5.0), 0.04, 1.0e-12);
}

// The long-end guard IS reachable, unlike the short-end one: if every pillar is
// non-positive there is no maturity to read a rate off, and disc must not divide by
// it. (create() only requires strictly ascending pillars, not positive ones.)
TEST(YieldCurve, AllNonPositivePillars_LongEndStaysFinite) {
  const double t[] = {-2.0, -1.0};
  const double r[] = {0.04, 0.04};
  const auto built = YieldCurve::create(t, r);
  ASSERT_TRUE(built.has_value());
  const YieldCurve &c = *built;
  EXPECT_TRUE(std::isfinite(c.disc(1.0)));
  EXPECT_TRUE(std::isfinite(c.zero(1.0)));
}

TEST(YieldCurve, DefaultConstructed_DiscReturnsFlatOne) {
  const YieldCurve c;
  EXPECT_DOUBLE_EQ(c.disc(0.5), 1.0);
  EXPECT_DOUBLE_EQ(c.zero(0.5), 0.0);
  EXPECT_EQ(c.size(), 0u);
}

TEST(YieldCurve, Create_RejectsEmptyPillars) {
  const auto result = YieldCurve::create({}, {});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(YieldCurve, Create_RejectsSizeMismatch) {
  const double t[] = {0.1, 0.2};
  const double r[] = {0.01, 0.02, 0.03};
  const auto result = YieldCurve::create(t, r);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(YieldCurve, Create_RejectsNonMonotoneMaturities) {
  const double t[] = {0.1, 0.05, 0.2};
  const double r[] = {0.01, 0.02, 0.03};
  const auto result = YieldCurve::create(t, r);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

// ── DividendForward: forward_div_corrected (test_curve_dividends.c) ───────

TEST(DividendForward, EmptySchedule_ReturnsPureCarry) {
  const double S = 100.0, r = 0.05, T = 0.5;
  const double F = forward_div_corrected(S, r, T, {},
                                         /*expiry_ns=*/static_cast<std::int64_t>(1e18),
                                         /*now_ts_ns=*/0);
  EXPECT_LT(std::fabs(F - S * std::exp(r * T)), 1e-9);
}

TEST(DividendForward, SingleDividend_SubtractsPvThenCarries) {
  // now = 0 ns, expiry 6 months out. Dividend at 3 months.
  const std::int64_t now_ns = 0;
  const std::int64_t three_months_ns = static_cast<std::int64_t>(0.25 * 365.25 * 86400.0 * 1.0e9);
  const std::int64_t six_months_ns = static_cast<std::int64_t>(0.50 * 365.25 * 86400.0 * 1.0e9);

  const std::vector<DividendEvent> evs{{three_months_ns, 1.50}};
  const double S = 100.0, r = 0.05, T = 0.5;
  const double F = forward_div_corrected(S, r, T, evs, six_months_ns, now_ns);

  // Expected: (S - D * e^{-r*t1}) * e^{rT}, t1 = 0.25.
  const double expected = (S - 1.50 * std::exp(-r * 0.25)) * std::exp(r * T);
  EXPECT_LT(std::fabs(F - expected), 1.0e-3); // loose: ns-precision t_i drift
}

TEST(DividendForward, DividendsAfterExpiry_AreIgnored) {
  // Two dividends: one before expiry (3m) and one after (1y). T = 6m.
  const std::int64_t now_ns = 0;
  const std::int64_t t3m = static_cast<std::int64_t>(0.25 * 365.25 * 86400.0 * 1.0e9);
  const std::int64_t t6m = static_cast<std::int64_t>(0.50 * 365.25 * 86400.0 * 1.0e9);
  const std::int64_t t1y = static_cast<std::int64_t>(1.00 * 365.25 * 86400.0 * 1.0e9);

  const std::vector<DividendEvent> evs2{{t3m, 1.0}, {t1y, 1.0}};
  const double S = 100.0, r = 0.05, T = 0.5;
  const double f2 = forward_div_corrected(S, r, T, evs2, t6m, now_ns);

  // Equivalent to a single 1.0 dividend at t=3m.
  const std::vector<DividendEvent> ev1{{t3m, 1.0}};
  const double f1 = forward_div_corrected(S, r, T, ev1, t6m, now_ns);
  EXPECT_LT(std::fabs(f1 - f2), 1.0e-9);
}

TEST(DividendForward, PaidDividends_AreIgnored) {
  // One dividend already paid (ex < now).
  const std::int64_t now_ns = static_cast<std::int64_t>(0.10 * 365.25 * 86400.0 * 1.0e9);
  const std::int64_t t_ex = static_cast<std::int64_t>(0.05 * 365.25 * 86400.0 * 1.0e9);
  const std::int64_t t_T = static_cast<std::int64_t>(0.50 * 365.25 * 86400.0 * 1.0e9);

  const std::vector<DividendEvent> evs{{t_ex, 5.0}};
  const double S = 100.0, r = 0.05, T = 0.4; // T from now
  const double F = forward_div_corrected(S, r, T, evs, t_T, now_ns);
  EXPECT_LT(std::fabs(F - S * std::exp(r * T)), 1.0e-3);
}

TEST(DividendForward, MultipleDividends_ComposeLinearly) {
  // Sum-of-PVs lemma: D1@t1 + D2@t2 -> (S - D1*e^{-r*t1} - D2*e^{-r*t2})*e^{rT}.
  const std::int64_t now_ns = 0;
  const std::int64_t t1 = static_cast<std::int64_t>(0.10 * 365.25 * 86400.0 * 1.0e9);
  const std::int64_t t2 = static_cast<std::int64_t>(0.30 * 365.25 * 86400.0 * 1.0e9);
  const std::int64_t t_T = static_cast<std::int64_t>(0.50 * 365.25 * 86400.0 * 1.0e9);

  const std::vector<DividendEvent> evs{{t1, 0.50}, {t2, 0.75}};
  const double S = 200.0, r = 0.04, T = 0.5;
  const double F = forward_div_corrected(S, r, T, evs, t_T, now_ns);

  const double expected =
      (S - 0.50 * std::exp(-r * 0.10) - 0.75 * std::exp(-r * 0.30)) * std::exp(r * T);
  EXPECT_LT(std::fabs(F - expected), 5.0e-3);
}

TEST(DividendForward, ProjectedSchedule_HistorySkippedFutureSummed) {
  // A SPY-style mixed schedule: 4 historical (already paid) events, 4
  // forward-projected events within the option horizon, and 4 more after
  // expiry. The correction at T=1y must include exactly the middle 4.
  const std::int64_t day_ns = 86400ll * 1000000000ll;
  const std::int64_t now_ns = 365 * day_ns; // arbitrary t=0 anchor
  const std::int64_t one_quarter = 91 * day_ns;
  const std::int64_t expiry_ns = now_ns + 365 * day_ns; // ~1y horizon

  std::vector<DividendEvent> evs(12);
  for (int i = 0; i < 4; ++i) {
    const auto idx = static_cast<std::size_t>(i);
    evs[idx].ex_date_ns = now_ns - (4 - i) * one_quarter; // past
    evs[idx].amount = 1.85;
  }
  for (int i = 0; i < 4; ++i) {
    const auto idx = static_cast<std::size_t>(4 + i);
    evs[idx].ex_date_ns = now_ns + (i + 1) * one_quarter; // within horizon
    evs[idx].amount = 1.85;
  }
  for (int i = 0; i < 4; ++i) {
    const auto idx = static_cast<std::size_t>(8 + i);
    evs[idx].ex_date_ns = expiry_ns + (i + 1) * one_quarter; // past expiry
    evs[idx].amount = 1.85;
  }

  const double S = 720.0, r = 0.045, T = 1.0;
  const double F = forward_div_corrected(S, r, T, evs, expiry_ns, now_ns);

  double pv = 0.0;
  for (int i = 0; i < 4; ++i) {
    const double t_i = static_cast<double>((i + 1) * one_quarter) / (1.0e9 * 365.25 * 86400.0);
    pv += 1.85 * std::exp(-r * t_i);
  }
  const double expected = (S - pv) * std::exp(r * T);
  EXPECT_LT(std::fabs(F - expected), 1.0e-3);

  // Sanity: an all-historical schedule collapses to the no-divs forward.
  const std::vector<DividendEvent> past_only(evs.begin(), evs.begin() + 4);
  const double f_past_only = forward_div_corrected(S, r, T, past_only, expiry_ns, now_ns);
  EXPECT_LT(std::fabs(f_past_only - S * std::exp(r * T)), 1.0e-9);
}

// ── ForwardCurve / HtbDetector (test_curve_htb.c) ──────────────────────────

ForwardCurve make_forward_curve_with_q(const std::vector<double> &qs) {
  std::vector<ForwardPoint> pts(qs.size());
  for (std::size_t i = 0; i < qs.size(); ++i) {
    pts[i].T = 0.10 + 0.10 * static_cast<double>(i); // 0.10y, 0.20y, ...
    pts[i].F = 100.0;
    pts[i].q_eff = qs[i];
    pts[i].F_smoothed = 100.0;
  }
  ForwardCurve fc;
  fc.set(pts);
  return fc;
}

TEST(HtbDetector, PersistentNegativeQ_TriggersHtb) {
  // All four expiries below the -0.10 threshold.
  ForwardCurve fc = make_forward_curve_with_q({-0.15, -0.20, -0.18, -0.12});

  const HtbDetector det = HtbDetector::default_detector();
  const HtbResult res = fc.detect_htb(det);
  EXPECT_TRUE(res.is_htb);
  EXPECT_EQ(res.n_offending, 4);
  for (const ForwardPoint &fp : fc.points()) {
    EXPECT_TRUE(has_flag(fp.flags, ForwardFlag::Htb));
  }
}

TEST(HtbDetector, IsolatedAnomaly_DoesNotTrigger) {
  // One outlier, three benign.
  ForwardCurve fc = make_forward_curve_with_q({-0.30, 0.01, 0.02, 0.015});

  const HtbDetector det = HtbDetector::default_detector();
  const HtbResult res = fc.detect_htb(det);
  EXPECT_FALSE(res.is_htb);
  EXPECT_EQ(res.n_offending, 1); // persistence gate (3 expiries) protects us
}

TEST(HtbDetector, RestoredQ_ClearsHtbFlag) {
  ForwardCurve fc = make_forward_curve_with_q({-0.12, -0.20, -0.15, -0.18});
  const HtbDetector det = HtbDetector::default_detector();
  ASSERT_TRUE(fc.detect_htb(det).is_htb);

  // Mutate q_eff back to benign and clear flags, as a refit would.
  for (ForwardPoint &fp : fc.points()) {
    fp.q_eff = 0.01;
    fp.flags = ForwardFlag::None;
  }
  const HtbResult res = fc.detect_htb(det);
  EXPECT_FALSE(res.is_htb);
  EXPECT_EQ(res.n_offending, 0);
}

TEST(HtbDetector, LowTExpiries_AreSkipped) {
  // Pillars at T=0.02 (below the 0.05 default cap) with q < threshold
  // should be ignored by the low-T filter.
  std::vector<ForwardPoint> pts(3);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    pts[i].T = 0.02 + 0.01 * static_cast<double>(i);
    pts[i].F = 100.0;
    pts[i].q_eff = -0.50;
    pts[i].F_smoothed = 100.0;
  }
  ForwardCurve fc;
  fc.set(pts);

  const HtbDetector det = HtbDetector::default_detector();
  const HtbResult res = fc.detect_htb(det);
  EXPECT_FALSE(res.is_htb);
  EXPECT_EQ(res.n_offending, 0);
}

// ── ForwardCurve lookup / CurveSet wiring ──────────────────────────────────

TEST(ForwardCurve, ForwardAt_ValidIndexReturnsF) {
  std::vector<ForwardPoint> pts(2);
  pts[0].F = 123.45;
  pts[1].F = 67.89;
  ForwardCurve fc;
  fc.set(pts);
  EXPECT_NEAR(fc.forward_at(0), 123.45, 1e-12);
  EXPECT_NEAR(fc.forward_at(1), 67.89, 1e-12);
}

TEST(ForwardCurve, ForwardAt_OutOfRangeReturnsNaN) {
  const ForwardCurve fc;
  EXPECT_TRUE(std::isnan(fc.forward_at(0)));
}

TEST(CurveSet, SetYield_ConstructsUsableYieldCurve) {
  CurveSet cs;
  const double t[] = {0.5, 1.0};
  const double r[] = {0.03, 0.035};
  const auto status = cs.set_yield(t, r);
  ASSERT_TRUE(status.has_value());
  EXPECT_NEAR(cs.yield.disc(0.5), std::exp(-0.03 * 0.5), 1e-12);
}

TEST(CurveSet, SetYield_RejectsInvalidPillars) {
  CurveSet cs;
  const auto status = cs.set_yield({}, {});
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
