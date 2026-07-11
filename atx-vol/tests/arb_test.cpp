#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "atx/vol/arb.hpp"

#include "atx/vol/black76.hpp"
#include "atx/vol/dense_slice.hpp"
#include "atx/vol/vol_curve.hpp"

// Coverage for the static-arbitrage validators (arb.hpp), ported from the C
// ats-vol library (ats_arb.c). Calendar / butterfly checks, SVI-MM
// admissibility, calendar projection / repair, and the Sprint-08 quote
// pre-fit filters. The filter cases mirror the C `test_prefit_filter.c`
// assertions/tolerances; the surface-arb cases construct known slices (the C
// exercised arb only indirectly through the calibration smoke tests).

namespace {

using atx::vol::ArbViolation;
using atx::vol::arb_check_all;
using atx::vol::arb_check_butterfly;
using atx::vol::arb_check_butterfly_svi_mm;
using atx::vol::arb_check_butterfly_svi_mm_surface;
using atx::vol::arb_check_calendar;
using atx::vol::arb_check_total_surface_all;
using atx::vol::arb_filter_quotes_ex;
using atx::vol::arb_project_calendar_essvi;
using atx::vol::arb_project_calendar_essvi_pair;
using atx::vol::arb_project_calendar_svi;
using atx::vol::arb_project_calendar_svi_pair;
using atx::vol::arb_project_calendar_c8_pair;
using atx::vol::black76_price;
using atx::vol::c8_slice_w;
using atx::vol::C8Curve;
using atx::vol::C8Params;
using atx::vol::ConvexDenseCurve;
using atx::vol::ConvexSliceFit;
using atx::vol::CurveSet;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::EssviParams;
using atx::vol::EssviCurve;
using atx::vol::essvi_total_w;
using atx::vol::filter_default_opts;
using atx::vol::FilterOpts;
using atx::vol::has_flag;
using atx::vol::Parametrization;
using atx::vol::prefit_filter_underlier;
using atx::vol::QuoteBatch;
using atx::vol::QuoteFlag;
using atx::vol::Side;
using atx::vol::SviParams;
using atx::vol::SviCurve;
using atx::vol::svi_total_w;
using atx::vol::Universe;
using atx::vol::VolSurface;

// ── Surface builders ──────────────────────────────────────────────────────

// Two ascending-T eSSVI slices with a shared (phi=1, rho=0) backbone shape and
// residual off, so total w scales linearly with theta. Passing th0 > th1
// hand-builds a calendar-violating surface; th0 < th1 a monotone one.
[[nodiscard]] VolSurface make_essvi_2slice(double th0, double T0, double th1,
                                           double T1) {
  auto res = VolSurface::create(1u, Parametrization::Essvi, 2);
  VolSurface surf = std::move(res).value();
  EssviParams s0{};
  s0.theta = th0;
  s0.phi = 1.0;
  s0.rho = 0.0;
  s0.T = T0;
  EssviParams s1{};
  s1.theta = th1;
  s1.phi = 1.0;
  s1.rho = 0.0;
  s1.T = T1;
  (void)surf.set_slice_essvi(0, s0);
  (void)surf.set_slice_essvi(1, s1);
  return surf;
}

[[nodiscard]] VolSurface make_svi_2slice(const SviParams &s0,
                                         const SviParams &s1) {
  auto res = VolSurface::create(1u, Parametrization::Svi, 2);
  VolSurface surf = std::move(res).value();
  (void)surf.set_slice_svi(0, s0);
  (void)surf.set_slice_svi(1, s1);
  return surf;
}

[[nodiscard]] VolSurface make_svi_1slice(const SviParams &s0) {
  auto res = VolSurface::create(1u, Parametrization::Svi, 1);
  VolSurface surf = std::move(res).value();
  (void)surf.set_slice_svi(0, s0);
  return surf;
}

// A raw-SVI slice with a very steep wing (b well past the Lee bound) — its
// Roper density goes negative in the wings (butterfly arbitrage).
[[nodiscard]] SviParams steep_svi_slice() {
  SviParams s{};
  s.a = 0.04;
  s.b = 4.0;
  s.rho = 0.0;
  s.m = 0.0;
  s.sigma = 0.1;
  s.T = 1.0;
  return s;
}

// A trivial constant-vol convex slice at (T,F) — flat smile sigma. 5 strikes
// around F; European call prices at flat sigma are convex/arb-free, so the
// convex-QP-shaped fixture below feeds fit output directly rather than the
// fitter itself.
[[nodiscard]] ConvexSliceFit flat_slice(double T, double F, double df,
                                        double sigma) {
  ConvexSliceFit s;
  s.T = T;
  s.F = F;
  s.df = df;
  for (int i = -2; i <= 2; ++i) {
    const double K = F * std::exp(0.05 * i);
    s.u.push_back(K);
    s.C.push_back(black76_price(F, K, T, sigma, df, Side::Call));
  }
  return s;
}

}  // namespace

// ── Calendar check ────────────────────────────────────────────────────────

TEST(ArbCalendar, MonotoneSurface_NoViolations) {
  const VolSurface surf = make_essvi_2slice(0.04, 0.25, 0.16, 1.0);
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().empty());
}

TEST(ArbCalendar, LongerMaturityLowerVariance_FlaggedEveryGridPoint) {
  // slice0 (T=0.25) carries larger total variance than slice1 (T=1.0): a
  // calendar crossing at every one of the 8 sampled k-points.
  const VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  const auto &v = res.value();
  ASSERT_EQ(v.size(), 8u);
  for (const ArbViolation &viol : v) {
    EXPECT_EQ(viol.kind, ArbViolation::Kind::Calendar);
    EXPECT_GT(viol.slack, 0.0);  // slack = w(T1) - w(T2) > 0
    EXPECT_EQ(viol.T1, 0.25);    // shorter maturity recorded distinctly
    EXPECT_EQ(viol.T2, 1.0);
  }
}

TEST(ArbCalendar, EmptyOrSingleSlice_NoOpEmpty) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().empty());
}

// ── Calendar check (CurveSurface: ConvexDense/SVI served path) ─────────────

TEST(ArbCheckCalendarCurveSurface, FlagsCrossing) {
  CurveSurface surf;
  // T1=0.25 with HIGH vol, T2=0.50 with LOW vol -> w(k,T2) < w(k,T1): calendar
  // arb.
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.25, 100.0, 1.0, 0.40)));
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.50, 100.0, 1.0, 0.20)));
  const auto v = arb_check_calendar(surf, -0.2, 0.2, 21);
  ASSERT_TRUE(v.has_value());
  EXPECT_FALSE(v->empty());
  EXPECT_EQ(v->front().kind, ArbViolation::Kind::Calendar);
}

TEST(ArbCheckCalendarCurveSurface, CleanStackNoViolation) {
  CurveSurface surf;
  // Monotone total variance: same sigma -> w = sigma^2 * T is increasing in T.
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.25, 100.0, 1.0, 0.25)));
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.50, 100.0, 1.0, 0.25)));
  const auto v = arb_check_calendar(surf, -0.2, 0.2, 21);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(v->empty());
}

TEST(ArbButterflyCurve, IndependentCheckerFlagsServedSviShape) {
  const SviCurve curve(steep_svi_slice(), 1.0);
  const auto violations = arb_check_butterfly(curve, -0.5, 0.5, 128);
  ASSERT_TRUE(violations.has_value());
  EXPECT_FALSE(violations->empty());
}

// ── Butterfly check ───────────────────────────────────────────────────────

TEST(ArbButterfly, WellBehavedEssvi_NoViolations) {
  const VolSurface surf = make_essvi_2slice(0.04, 0.25, 0.16, 1.0);
  const auto res = arb_check_butterfly(surf, -0.5, 0.5, 64);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().empty());
}

TEST(ArbButterfly, SteepSviWing_NegativeDensityFlagged) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_butterfly(surf, -0.5, 0.5, 64);
  ASSERT_TRUE(res.has_value());
  const auto &v = res.value();
  ASSERT_FALSE(v.empty());
  for (const ArbViolation &viol : v) {
    EXPECT_EQ(viol.kind, ArbViolation::Kind::Butterfly);
    EXPECT_EQ(viol.T1, 1.0);
    EXPECT_EQ(viol.T2, 1.0);  // T2 == T1 for butterfly
    EXPECT_GT(viol.slack, 0.0);
  }
}

TEST(ArbButterfly, KMaxNotAboveKMin_ReturnsInvalidArgument) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_butterfly(surf, 0.5, -0.5, 64);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Combined check ────────────────────────────────────────────────────────

TEST(ArbCheckAll, ConcatenatesCalendarThenButterfly) {
  const VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto res = arb_check_all(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  std::uint32_t n_cal = 0;
  for (const ArbViolation &viol : res.value()) {
    if (viol.kind == ArbViolation::Kind::Calendar) {
      ++n_cal;
    }
  }
  EXPECT_EQ(n_cal, 8u);  // calendar crossing at every grid point
}

TEST(ArbCheckAll, PropagatesButterflyInvalidArgument) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_all(surf, 0.5, -0.5, 8);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Total-surface counts ──────────────────────────────────────────────────

TEST(ArbTotalSurface, CalendarViolationsCounted) {
  const VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto res = arb_check_total_surface_all(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value().n_calendar, 8u);
  EXPECT_EQ(res.value().n_butterfly, 0u);
}

// ── SVI-MM admissibility ──────────────────────────────────────────────────

TEST(ArbSviMm, AdmissibleSlice_NoViolations) {
  SviParams s{};
  s.a = 0.04;
  s.b = 0.3;
  s.rho = -0.4;
  s.m = 0.0;
  s.sigma = 0.25;
  s.T = 1.0;
  const auto adm = arb_check_butterfly_svi_mm(s, 1.0);
  EXPECT_EQ(adm.n_violations, 0u);
  EXPECT_NEAR(adm.max_slack, 0.0, 1.0e-15);
}

TEST(ArbSviMm, LeeBoundViolation_OneViolationWithSlack) {
  // b*(1+|rho|) = 5*1.4 = 7 > 4/T = 4; slack = 3. w_min stays >= 0.
  SviParams s{};
  s.a = 0.04;
  s.b = 5.0;
  s.rho = -0.4;
  s.m = 0.0;
  s.sigma = 0.25;
  s.T = 1.0;
  const auto adm = arb_check_butterfly_svi_mm(s, 1.0);
  EXPECT_EQ(adm.n_violations, 1u);
  EXPECT_NEAR(adm.max_slack, 3.0, 1.0e-9);
}

TEST(ArbSviMm, NonPositiveB_FlaggedByFirstInequality) {
  SviParams s{};
  s.a = 0.1;
  s.b = -0.1;
  s.rho = 0.0;
  s.m = 0.0;
  s.sigma = 0.2;
  s.T = 1.0;
  const auto adm = arb_check_butterfly_svi_mm(s, 1.0);
  EXPECT_EQ(adm.n_violations, 1u);
  EXPECT_NEAR(adm.max_slack, 0.1, 1.0e-9);
}

TEST(ArbSviMm, SurfaceWalker_SumsPerSliceViolations) {
  auto res = VolSurface::create(2u, Parametrization::SviMm, 2);
  VolSurface surf = std::move(res).value();
  SviParams good{};
  good.a = 0.04;
  good.b = 0.3;
  good.rho = -0.4;
  good.m = 0.0;
  good.sigma = 0.25;
  good.T = 1.0;
  SviParams bad{};
  bad.a = 0.04;
  bad.b = 5.0;
  bad.rho = -0.4;
  bad.m = 0.0;
  bad.sigma = 0.25;
  bad.T = 1.0;
  (void)surf.set_slice_svi(0, good);
  (void)surf.set_slice_svi(1, bad);

  const auto res_adm = arb_check_butterfly_svi_mm_surface(surf);
  ASSERT_TRUE(res_adm.has_value());
  EXPECT_EQ(res_adm.value().n_violations, 1u);
  EXPECT_NEAR(res_adm.value().max_slack, 3.0, 1.0e-9);
}

TEST(ArbSviMm, SurfaceWalker_NonSviMm_NoOpZero) {
  // A plain SVI surface with a Lee-violating slice: the walker still reports
  // zero because it only enforces the polytope for SVI_MM-tagged surfaces.
  const VolSurface surf = make_svi_2slice(steep_svi_slice(), steep_svi_slice());
  const auto res = arb_check_butterfly_svi_mm_surface(surf);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value().n_violations, 0u);
}

// ── Calendar projection / repair ──────────────────────────────────────────

TEST(ArbProjectCalendarSvi, RestoresMonotonicity) {
  SviParams s0{};
  s0.a = 0.10;
  s0.b = 0.1;
  s0.rho = 0.0;
  s0.m = 0.0;
  s0.sigma = 0.1;
  s0.T = 0.25;
  SviParams s1{};
  s1.a = 0.02;  // lower than s0 at every k => calendar crossing
  s1.b = 0.1;
  s1.rho = 0.0;
  s1.m = 0.0;
  s1.sigma = 0.1;
  s1.T = 1.0;
  VolSurface surf = make_svi_2slice(s0, s1);

  const auto before = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(before.has_value());
  EXPECT_FALSE(before.value().empty());

  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());

  const auto after = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(after.value().empty());
  EXPECT_GT(surf.svi_slices()[1].a, 0.02);  // longer slice's `a` was bumped up
}

TEST(ArbProjectCalendarSvi, IdempotentOnAlreadyMonotone) {
  SviParams s0{};
  s0.a = 0.10;
  s0.b = 0.1;
  s0.sigma = 0.1;
  s0.T = 0.25;
  SviParams s1{};
  s1.a = 0.02;
  s1.b = 0.1;
  s1.sigma = 0.1;
  s1.T = 1.0;
  VolSurface surf = make_svi_2slice(s0, s1);

  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());
  const double a_first = surf.svi_slices()[1].a;
  // Second projection must not move an already-repaired surface.
  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());
  EXPECT_NEAR(surf.svi_slices()[1].a, a_first, 1.0e-15);
  const auto after = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(after.value().empty());
}

TEST(ArbProjectCalendarSvi, WrongParametrization_NoOpOk) {
  VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const double theta_before = surf.essvi_slices()[1].theta;
  // SVI projector on an eSSVI surface is a no-op (Ok), leaving it untouched.
  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());
  EXPECT_EQ(surf.essvi_slices()[1].theta, theta_before);
}

TEST(ArbProjectCalendarSvi, KMaxNotAboveKMin_ReturnsInvalidArgument) {
  SviParams s0{};
  s0.a = 0.10;
  s0.b = 0.1;
  s0.sigma = 0.1;
  s0.T = 0.25;
  SviParams s1{};
  s1.a = 0.02;
  s1.b = 0.1;
  s1.sigma = 0.1;
  s1.T = 1.0;
  VolSurface surf = make_svi_2slice(s0, s1);
  const auto rc = arb_project_calendar_svi(surf, 0.3, -0.3, 32);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::InvalidArgument);
}

TEST(ArbProjectCalendarEssvi, RestoresMonotonicity) {
  VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto before = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(before.has_value());
  EXPECT_FALSE(before.value().empty());

  ASSERT_TRUE(arb_project_calendar_essvi(surf, -0.3, 0.3, 32).has_value());

  const auto after = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(after.value().empty());
  EXPECT_GT(surf.essvi_slices()[1].theta, 0.04);  // theta bumped up
}

TEST(ArbProjectCalendarEssvi, IdempotentOnAlreadyMonotone) {
  VolSurface surf = make_essvi_2slice(0.04, 0.25, 0.16, 1.0);
  const double theta_before = surf.essvi_slices()[1].theta;
  ASSERT_TRUE(arb_project_calendar_essvi(surf, -0.3, 0.3, 32).has_value());
  EXPECT_NEAR(surf.essvi_slices()[1].theta, theta_before, 1.0e-15);
}

TEST(ArbProjectCalendarPair, EssviClosesSharedGridAndPreservesButterfly) {
  EssviParams previous{};
  previous.theta = 0.09;
  previous.phi = 0.8;
  previous.rho = -0.3;
  previous.T = 0.25;
  previous.F = 100.0;
  EssviParams current = previous;
  current.theta = 0.025;
  current.T = 0.50;
  const std::function<double(double)> floor =
      [previous](double k) { return essvi_total_w(previous, k); };

  const auto projection =
      arb_project_calendar_essvi_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_TRUE(projection.has_value()) << projection.error().to_string();
  EXPECT_GT(projection->passes, 0u);
  for (int i = 0; i <= 64; ++i) {
    const double k = -0.6 + 1.2 * static_cast<double>(i) / 64.0;
    EXPECT_GE(essvi_total_w(current, k), floor(k) - 1.0e-7);
  }
  const EssviCurve served(current, 1.0);
  const auto shape = arb_check_butterfly(served, -0.6, 0.6, 256);
  ASSERT_TRUE(shape.has_value());
  EXPECT_TRUE(shape->empty());
}

TEST(ArbProjectCalendarPair, SviUsesShapePreservingLevelShift) {
  SviParams previous{};
  previous.a = 0.08;
  previous.b = 0.10;
  previous.rho = -0.25;
  previous.m = 0.0;
  previous.sigma = 0.20;
  previous.T = 0.25;
  SviParams current = previous;
  current.a = 0.01;
  current.T = 0.50;
  const double b_before = current.b;
  const std::function<double(double)> floor =
      [previous](double k) { return svi_total_w(previous, k); };

  const auto projection =
      arb_project_calendar_svi_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_TRUE(projection.has_value()) << projection.error().to_string();
  EXPECT_EQ(projection->passes, 1u);
  EXPECT_DOUBLE_EQ(current.b, b_before);
  for (int i = 0; i <= 64; ++i) {
    const double k = -0.6 + 1.2 * static_cast<double>(i) / 64.0;
    EXPECT_GE(svi_total_w(current, k), floor(k) - 1.0e-7);
  }
  const SviCurve served(current, 1.0);
  const auto shape = arb_check_butterfly(served, -0.6, 0.6, 256);
  ASSERT_TRUE(shape.has_value());
  EXPECT_TRUE(shape->empty());
}

TEST(ArbProjectCalendarPair, C8LevelShiftThenRevalidatesBumps) {
  C8Params current{};
  current.T = 0.50;
  current.F = 100.0;
  current.v = 0.025;
  current.v_min = 0.022;
  current.psi = -0.004;
  current.p = 0.20;
  current.c = 0.18;
  current.kappa = -0.001;
  const std::function<double(double)> floor =
      [](double) { return 0.06; };

  const auto projection =
      arb_project_calendar_c8_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_TRUE(projection.has_value()) << projection.error().to_string();
  EXPECT_GT(projection->passes, 0u);
  for (int i = 0; i <= 64; ++i) {
    const double k = -0.6 + 1.2 * static_cast<double>(i) / 64.0;
    EXPECT_GE(c8_slice_w(current, k), floor(k) - 1.0e-7);
  }
  const C8Curve served(current, 1.0);
  const auto shape = arb_check_butterfly(served, -0.6, 0.6, 256);
  ASSERT_TRUE(shape.has_value());
  EXPECT_TRUE(shape->empty());
}

// ── Quote pre-fit filters (mirrors test_prefit_filter.c) ──────────────────

TEST(ArbFilter, DefaultOpts_MatchOrdinaryProfile) {
  const FilterOpts o = filter_default_opts();
  EXPECT_EQ(o.stale_seconds, 30);
  EXPECT_NEAR(o.wide_spread_pct, 1.50, 1.0e-12);
  EXPECT_NEAR(o.penny_floor, 0.05, 1.0e-12);
}

TEST(ArbFilter, ExFilter_FlagsEachRowUniquely) {
  // Row layout matches the C fixture: 0 clean, 1 LOCKED, 2 CROSSED, 3 WIDE,
  // 4 PENNY, 5 STALE.
  const std::int64_t now = 1'000'000'000'000LL;
  const std::vector<double> bids = {0.50, 0.10, 0.20, 0.50, 0.02, 0.50};
  const std::vector<double> asks = {0.55, 0.10, 0.10, 1.50, 0.04, 0.55};
  std::vector<std::int64_t> ts(6, now);
  ts[5] = now - 100LL * 1'000'000'000LL;  // 100s ago
  const std::vector<std::uint8_t> flags_in(6, std::uint8_t{0});

  QuoteBatch b{};
  b.bids = bids;
  b.asks = asks;
  b.ts_ns = ts;
  b.flags = flags_in;

  FilterOpts o = filter_default_opts();
  o.now_ts_ns = now;
  o.stale_seconds = 30;
  o.wide_spread_pct = 0.50;  // tighter than default for the test
  o.penny_floor = 0.05;
  o.min_vega_filter = 0.0;  // skip vega (no vega column)

  std::vector<std::uint8_t> out(6, std::uint8_t{0xFF});
  const auto res = arb_filter_quotes_ex(b, o, {}, out);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value(), 5u);  // rows 1..5 each gained a bit

  const auto qf = [](std::uint8_t v) { return static_cast<QuoteFlag>(v); };
  EXPECT_EQ(out[0], 0u);  // clean row fully overwritten to 0
  EXPECT_TRUE(has_flag(qf(out[1]), QuoteFlag::Locked));
  EXPECT_FALSE(has_flag(qf(out[1]), QuoteFlag::Crossed));
  EXPECT_TRUE(has_flag(qf(out[2]), QuoteFlag::Crossed));
  EXPECT_FALSE(has_flag(qf(out[2]), QuoteFlag::Locked));
  EXPECT_TRUE(has_flag(qf(out[3]), QuoteFlag::WideSpread));
  EXPECT_TRUE(has_flag(qf(out[4]), QuoteFlag::Penny));
  EXPECT_TRUE(has_flag(qf(out[5]), QuoteFlag::Stale));
}

TEST(ArbFilter, LowVega_OnlyWhenVegasSupplied) {
  const std::int64_t now = 1'000'000'000'000LL;
  const std::vector<double> bids = {0.50, 0.10, 0.20, 0.50, 0.02, 0.50};
  const std::vector<double> asks = {0.55, 0.10, 0.10, 1.50, 0.04, 0.55};
  const std::vector<std::int64_t> ts(6, now);
  const std::vector<std::uint8_t> flags_in(6, std::uint8_t{0});

  QuoteBatch b{};
  b.bids = bids;
  b.asks = asks;
  b.ts_ns = ts;
  b.flags = flags_in;

  FilterOpts o = filter_default_opts();
  o.now_ts_ns = now;
  o.min_vega_filter = 1.0e-3;

  const std::vector<double> vegas = {1.0e-5, 1.0, 1.0, 1.0, 1.0, 1.0};
  std::vector<std::uint8_t> out(6, std::uint8_t{0});
  const auto res = arb_filter_quotes_ex(b, o, vegas, out);
  ASSERT_TRUE(res.has_value());

  const auto qf = [](std::uint8_t v) { return static_cast<QuoteFlag>(v); };
  EXPECT_TRUE(has_flag(qf(out[0]), QuoteFlag::LowVega));   // vega below threshold
  EXPECT_FALSE(has_flag(qf(out[1]), QuoteFlag::LowVega));  // locked, not low-vega
}

TEST(ArbFilter, ExFilter_FlagsOutTooSmall_ReturnsInvalidArgument) {
  const std::vector<double> bids = {0.50, 0.10};
  const std::vector<double> asks = {0.55, 0.10};
  QuoteBatch b{};
  b.bids = bids;
  b.asks = asks;
  std::vector<std::uint8_t> out(1, std::uint8_t{0});  // shorter than batch
  const auto res = arb_filter_quotes_ex(b, filter_default_opts(), {}, out);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(ArbFilter, PrefitUnderlier_StampsChainFlags) {
  Universe u;
  const auto uid = u.intern_ticker("SPY").value();
  const auto eid = u.add_expiry(uid, 2'000'000'000'000LL).value();
  (void)u.add_strike(uid, eid, 100.0);  // strike index 0
  auto *under = u.get_underlying(uid).value();
  auto &c = under->chains[eid];

  const std::int64_t now = 1'000'000'000'000LL;
  const std::size_t ic = atx::vol::chain_index(std::uint16_t{0}, Side::Call);
  const std::size_t ip = atx::vol::chain_index(std::uint16_t{0}, Side::Put);
  c.bids[ic] = 0.10;  // locked (bid == ask)
  c.asks[ic] = 0.10;
  c.ts_ns[ic] = now;
  c.bids[ip] = 0.02;  // penny (bid < 0.05 floor)
  c.asks[ip] = 0.04;
  c.ts_ns[ip] = now;

  const CurveSet cs;
  FilterOpts o = filter_default_opts();
  o.now_ts_ns = now;
  const auto res = prefit_filter_underlier(*under, cs, o, 0);
  ASSERT_TRUE(res.has_value());
  EXPECT_GE(res.value(), 2u);

  const auto qf = [](std::uint8_t v) { return static_cast<QuoteFlag>(v); };
  EXPECT_TRUE(has_flag(qf(c.flags[ic]), QuoteFlag::Locked));
  EXPECT_TRUE(has_flag(qf(c.flags[ip]), QuoteFlag::Penny));
}
