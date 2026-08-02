#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <span>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_butterfly_svi_mm, arb_check_butterfly_slice
#include "atx/vol/black76.hpp"      // black76_value_and_vega
#include "atx/vol/calib.hpp"        // CalibOpts, FitObs
#include "atx/vol/spline_curve.hpp" // fit_spline_vol_slice
#include "atx/vol/svi_calib.hpp"    // svi_project_mm
#include "atx/vol/types.hpp"        // Side
#include "atx/vol/vol_curve.hpp"    // fit_slice_curve, CurveConfig, SviCurve, C8Curve
#include "atx/vol/vol_surface.hpp"  // SviParams

// Task C2.5: the raw-SVI and C8 serving seams (fit_slice_curve) must never
// hand back a butterfly-arbitrageable slice. These tests fit a standard
// synthetic board and assert the served slice passes its butterfly check, plus
// a direct unit test of the Mingone projection primitive the SVI gate relies on
// to repair (rather than reject) an inadmissible fit.

namespace {

using atx::vol::arb_check_butterfly_slice;
using atx::vol::arb_check_butterfly_svi_mm;
using atx::vol::black76_value_and_vega;
using atx::vol::C8Curve;
using atx::vol::CalibOpts;
using atx::vol::ConvexRepairSpec;
using atx::vol::CurveConfig;
using atx::vol::fit_slice_curve;
using atx::vol::FitObs;
using atx::vol::IVolCurve;
using atx::vol::Side;
using atx::vol::svi_project_mm;
using atx::vol::SviCurve;
using atx::vol::SviParams;
using atx::vol::VolCurveKind;

// A smooth, admissible synthetic smile as FitObs rows (European-equivalent
// inputs, as fit_slice_curve consumes). sigma(k) is a gentle skewed parabola,
// always positive over +/-20% log-moneyness.
[[nodiscard]] std::vector<FitObs> make_smile_obs(double T, double F, double df,
                                                 int n) {
  std::vector<FitObs> obs;
  obs.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double frac = static_cast<double>(i) / static_cast<double>(n - 1);
    const double k = -0.20 + 0.40 * frac;
    const double sigma = 0.22 - 0.08 * k + 0.30 * k * k;  // > 0 on the range
    const double K = F * std::exp(k);
    const Side side = (k >= 0.0) ? Side::Call : Side::Put;
    const auto vv = black76_value_and_vega(F, K, T, sigma, df, side);
    const double price = vv.price;
    const double vega = vv.vega;
    const double spread = std::max(0.01, 0.02 * price);
    FitObs o{};
    o.k = k;
    o.sigma_mkt = sigma;
    o.w_mkt = sigma * sigma * T;
    o.weight_w = (vega * vega) / (spread * spread);
    o.active_weight_w = o.weight_w;
    o.K = K;
    o.F = F;
    o.df = df;
    o.mid = price;
    o.spread = spread;
    o.vega = vega;
    o.noise_sigma = spread / std::max(vega, 1.0e-6);
    o.side = side;
    obs.push_back(o);
  }
  return obs;
}

}  // namespace

TEST(VolCurve, SviServedSliceIsButterflyAdmissible) {
  constexpr double T = 0.5;
  constexpr double F = 100.0;
  constexpr double df = 0.99;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  CurveConfig cfg;
  cfg.kind = VolCurveKind::Svi;
  const auto curve = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_TRUE(curve.has_value()) << curve.error().to_string();

  const IVolCurve* const cv = curve->get();
  ASSERT_EQ(cv->kind(), VolCurveKind::Svi);
  // The acceptance pin: closed-form Martini-Mingone admissibility on the SERVED
  // raw-SVI slice is exactly zero (the serving gate validated / projected it).
  const auto& sp = static_cast<const SviCurve*>(cv)->slice();
  const auto adm = arb_check_butterfly_svi_mm(sp, T);
  EXPECT_EQ(adm.n_violations, 0u);
}

TEST(VolCurve, SviPairProjectionActsOnlyOnTheTradeableOverlap) {
  // The sp100-2026 XOM/CVX poison, distilled: the previous slice's total
  // variance TOWERS over the current slice only OUTSIDE both slices' quoted
  // ranges (an extrapolated-wing "crossing" no traded strike witnesses). The
  // pair projection must ignore it: the served slice's ATM level stays the
  // fit's own level, not the fit plus a wing-fiction-sized shift.
  constexpr double T = 0.5;
  constexpr double F = 100.0;
  constexpr double df = 0.99;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);  // k in [-0.20, 0.20]

  // Below the current slice everywhere the quotes live (|k| <= 0.30), towering
  // beyond it. w_cur(0) ~ 0.0242; w_prev(+-0.6) ~ 0.151.
  const std::function<double(double)> w_prev = [](double k) {
    return 0.001 + 0.5 * std::max(0.0, std::fabs(k) - 0.30);
  };

  CurveConfig cfg;
  cfg.kind = VolCurveKind::Svi;
  const auto curve = fit_slice_curve(cfg, obs, F, T, df, w_prev,
                                     /*calendar_floor_knots=*/{},
                                     /*prev_data_k_range=*/{-0.22, 0.22});
  ASSERT_TRUE(curve.has_value()) << curve.error().to_string();
  // ATM total variance ~ 0.22^2 * 0.5 = 0.0242 from the quotes. The pre-fix
  // code adds the k=+-0.6 deficit (~0.11) to `a`, landing near 0.13.
  const double w_atm = (*curve)->w(0.0);
  EXPECT_LT(w_atm, 0.05) << "ATM level moved by an out-of-overlap wing deficit";
  EXPECT_GT(w_atm, 0.015);
}

TEST(VolCurve, SviPairProjectionStillRepairsInsideTheOverlap) {
  // A genuine SMALL crossing at quoted strikes must still be repaired: the
  // floor sits ~3% above the fit's ATM total variance at k=0.
  constexpr double T = 0.5;
  constexpr double F = 100.0;
  constexpr double df = 0.99;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  const std::function<double(double)> w_prev = [](double) { return 0.0250; };

  CurveConfig cfg;
  cfg.kind = VolCurveKind::Svi;
  const auto curve = fit_slice_curve(cfg, obs, F, T, df, w_prev,
                                     /*calendar_floor_knots=*/{},
                                     /*prev_data_k_range=*/{-0.22, 0.22});
  ASSERT_TRUE(curve.has_value()) << curve.error().to_string();
  for (int i = 0; i <= 20; ++i) {
    const double k = -0.20 + 0.40 * static_cast<double>(i) / 20.0;
    EXPECT_GE((*curve)->w(k), 0.0250 - 1.0e-7) << "k=" << k;
  }
}

TEST(VolCurve, SviProjectMmRepairsLeeViolation) {
  // The gate PROJECTS an inadmissible fit before rejecting; verify that repair
  // primitive directly. A steep-wing slice (b*(1+|rho|) past the T-free Lee bound
  // of 4) is Lee-inadmissible; svi_project_mm must move it back into the polytope.
  // FT-C3: the bound is T-free, so b*(1+|rho|)=6 > 4 is inadmissible at any T.
  SviParams s{};
  s.a = 0.04;
  s.b = 6.0;  // b*(1+|rho|) = 6 > 4 (T-free Lee bound)
  s.rho = 0.0;
  s.m = 0.0;
  s.sigma = 0.1;
  s.T = 2.0;
  ASSERT_GT(arb_check_butterfly_svi_mm(s, s.T).n_violations, 0u);

  const bool moved = svi_project_mm(s, s.T);
  EXPECT_TRUE(moved);
  EXPECT_EQ(arb_check_butterfly_svi_mm(s, s.T).n_violations, 0u);
}

// A permissive CalibOpts that disables the observation-builder spread filters and
// the post-fit sigma clamp so a butterfly-arb slice reaches the admission gate
// rather than being dropped for an unrelated reason.
[[nodiscard]] CalibOpts vc_permissive_opts() {
  CalibOpts o = atx::vol::calib_default_opts();
  o.max_spread_vol = 1.0e9;
  o.min_vega_weight = 0.0;
  o.max_spread_to_mid_pct = 0.0;
  o.max_post_fit_sigma = 0.0;
  return o;
}

// FT-C2 (B2a): the SVI served-path butterfly admission is necessary-conditions-
// only (the Martini-Mingone 5-condition polytope) plus a density scan restricted
// to |k| <= 0.6. A raw-SVI slice can pass BOTH while carrying Durrleman g<0 in a
// wing at |k| > 0.6 (the canonical Vogt counterexample sits there; g<0 on
// [0.64, 0.79] and clean on [-0.6, 0.6]). This fixture is a positive-skew,
// right-shifted slice (a=0, b=0.13, rho=0.50, m=0.50, sigma=0.30) with exactly
// that shape and, unlike Vogt (a<0, unreachable by the a>=0 quasi-explicit
// fitter), it is fit-reproduced from |k|<=0.6 quotes. The served path must scan
// the FULL quoted range +/- 0.5 (parametric closed form, cost nil) and refuse it.
TEST(VolCurve, SviServedSliceRejectsWingButterflyArb) {
  const double a = 0.0, b = 0.13, rho = 0.50, m = 0.50, sigma = 0.30;
  const double T = 1.0, F = 100.0, df = 1.0;
  auto wt = [&](double k) {
    const double dk = k - m;
    return a + b * (rho * dk + std::sqrt(dk * dk + sigma * sigma));
  };
  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k = -0.6 + 1.2 * i / (n - 1);
    const double w = wt(k);
    const double sig = std::sqrt(w / T);
    const double K = F * std::exp(k);
    const Side side = (k >= 0.0) ? Side::Call : Side::Put;
    const auto vv = black76_value_and_vega(F, K, T, sig, df, side);
    FitObs o{};
    o.k = k; o.sigma_mkt = sig; o.w_mkt = w; o.K = K; o.F = F; o.df = df;
    o.mid = vv.price; o.spread = std::max(0.001, 0.01 * vv.price); o.vega = vv.vega;
    o.side = side; o.weight_w = 1.0; o.active_weight_w = 1.0;
    obs.push_back(o);
  }

  const CalibOpts po = vc_permissive_opts();

  // Non-vacuity: the RAW fit passes the 5-condition mm gate and is clean on the
  // old [-0.6, 0.6] scan, yet carries genuine wing butterfly arb over the quoted
  // range +/- 0.5. So a range-complete gate has something real to catch.
  const auto raw = atx::vol::svi_fit_slice(std::span<const FitObs>(obs), T, F, po);
  ASSERT_TRUE(raw.has_value());
  const SviParams sp = raw.value();
  auto wf = [&](double k) {
    return sp.a + sp.b * (sp.rho * (k - sp.m) +
                          std::sqrt((k - sp.m) * (k - sp.m) + sp.sigma * sp.sigma));
  };
  EXPECT_EQ(arb_check_butterfly_svi_mm(sp, T).n_violations, 0u)
      << "fixture must PASS the necessary-conditions gate";
  const auto narrow = arb_check_butterfly_slice(wf, T, -0.6, 0.6, 256u);
  ASSERT_TRUE(narrow.has_value());
  EXPECT_TRUE(narrow->empty()) << "fixture must be clean on the old [-0.6,0.6] scan";
  const auto wide = arb_check_butterfly_slice(wf, T, -1.1, 1.1, 512u);
  ASSERT_TRUE(wide.has_value());
  EXPECT_FALSE(wide->empty()) << "fixture must carry real wing arb over quoted+/-0.5";

  // Served path: must REFUSE. Pre-fix serves it (RED); post-fix rejects (GREEN).
  CurveConfig cfg;
  cfg.kind = VolCurveKind::Svi;
  cfg.parametric = po;
  const auto served = fit_slice_curve(cfg, obs, F, T, df);
  EXPECT_FALSE(served.has_value())
      << "served-path admitted a raw-SVI slice with wing butterfly arb over the "
         "quoted range +/- 0.5";
}

// FT-C7 (B2b): a caller-pinned SplineVol bypasses the selector, and the spline
// fit computes its butterfly-violation count as a DIAGNOSTIC only (never rejects
// or projects). A sharp ATM vol bump fits a spline whose total variance is non-
// convex on the bump flanks (negative risk-neutral density). The pinned path must
// gate on the served shape and fail admission.
TEST(VolCurve, PinnedSplineVolRejectsButterflyArb) {
  const double T = 0.5, F = 100.0, df = 1.0;
  std::vector<FitObs> obs;
  const int n = 31;
  for (int i = 0; i < n; ++i) {
    const double k = -0.5 + 1.0 * i / (n - 1);
    const double sig = 0.25 + 0.35 * std::exp(-150.0 * k * k);  // sharp ATM bump
    const double K = F * std::exp(k);
    const Side side = (k >= 0.0) ? Side::Call : Side::Put;
    const auto vv = black76_value_and_vega(F, K, T, sig, df, side);
    FitObs o{};
    o.k = k; o.sigma_mkt = sig; o.w_mkt = sig * sig * T; o.K = K; o.F = F; o.df = df;
    o.mid = vv.price; o.spread = std::max(0.001, 0.01 * vv.price); o.vega = vv.vega;
    o.side = side; o.weight_w = 1.0; o.active_weight_w = 1.0;
    obs.push_back(o);
  }

  // Non-vacuity: the raw spline fit (bypassing the new gate) carries real
  // butterfly arb over its data range +/- 0.5.
  const auto raw = atx::vol::fit_spline_vol_slice(std::span<const FitObs>(obs), F, T, df);
  ASSERT_TRUE(raw.has_value()) << raw.error().to_string();
  const IVolCurve* const rc = raw->get();
  const auto bf = arb_check_butterfly_slice(
      [rc](double k) { return rc->w(k); }, T, -1.0, 1.0, 256u);
  ASSERT_TRUE(bf.has_value());
  EXPECT_FALSE(bf->empty()) << "fixture spline must carry real butterfly arb";

  // Pinned served path must REFUSE. Pre-fix serves it (RED); post-fix rejects.
  CurveConfig cfg;
  cfg.kind = VolCurveKind::SplineVol;
  const auto served = fit_slice_curve(cfg, obs, F, T, df);
  EXPECT_FALSE(served.has_value())
      << "pinned SplineVol path served a butterfly-arbitrageable slice";
}

TEST(VolCurve, C8ServedSlicePassesGridButterflyCheck) {
  constexpr double T = 0.25;
  constexpr double F = 100.0;
  constexpr double df = 0.995;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);  // >= 8 for C8

  CurveConfig cfg;
  cfg.kind = VolCurveKind::C8;
  const auto curve = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_TRUE(curve.has_value()) << curve.error().to_string();

  const IVolCurve* const cv = curve->get();
  ASSERT_EQ(cv->kind(), VolCurveKind::C8);
  // Served C8 slice must pass the same grid g-check its accept gate applied.
  const auto bf = arb_check_butterfly_slice(
      [cv](double k) { return cv->w(k); }, T, -0.7, 0.7, 64u);
  ASSERT_TRUE(bf.has_value());
  EXPECT_TRUE(bf->empty());
}

// The 2026-08 SPY backfill failure mode in miniature: the previous slice's
// total variance crosses the current fit only inside a narrow bump centered
// on the oracle band edge k = -0.50 — a k the legacy repair lattice
// (-0.60 + i * 0.01875) never samples. The bump peak (9e-8) also sits below
// the legacy 1e-7 acceptance, so the legacy loop must serve an oracle-fatal
// crossing; a ConvexRepairSpec pinned to the oracle's Balanced calendar grid
// (65 pts over [-0.50, 0.50], which contains -0.50 exactly) with a 1e-9
// tolerance must repair it.
TEST(VolCurve, ConvexRepairSpecRepairsOffLatticeCalendarCrossing) {
  constexpr double T = 0.10;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;

  const auto base = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  const IVolCurve& base_curve = **base;

  // 1e-9 below baseline everywhere (floor inactive at nodes) except the bump:
  // +9e-8 at k=-0.50, gone by the nearest legacy lattice points at
  // -0.51875 / -0.4875 (exp(-(0.01875/3e-3)^2) ~ 1e-17).
  const auto w_prev = [&base_curve](double k) {
    const double z = (k + 0.50) / 3.0e-3;
    return base_curve.w(k) - 1.0e-9 + 9.0e-8 * std::exp(-z * z);
  };

  const auto legacy = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  const double legacy_crossing = w_prev(-0.50) - (*legacy)->w(-0.50);
  EXPECT_GT(legacy_crossing, 1.0e-8)
      << "fixture must reproduce the failure mode: legacy repair serves a "
         "crossing the admission oracle rejects";

  cfg.convex_repair = ConvexRepairSpec{};
  cfg.convex_repair->k_min = -0.50;
  cfg.convex_repair->k_max = 0.50;
  cfg.convex_repair->grid_points = 65;
  cfg.convex_repair->tolerance = 1.0e-9;
  const auto strict = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(strict.has_value()) << strict.error().to_string();
  const double strict_crossing = w_prev(-0.50) - (*strict)->w(-0.50);
  EXPECT_LE(strict_crossing, 1.0e-8)
      << "strict repair must close the crossing at the oracle grid k";
}

// extra_node_ks must become exact QP floor nodes even at a k on NEITHER the
// legacy lattice NOR the spec grid (here -0.517, outside the spec band):
// this is the mechanism the pricer recovery rung uses to promote the
// oracle's reported violation k's.
TEST(VolCurve, ConvexRepairSpecExtraNodeKsBecomeExactFloorNodes) {
  constexpr double T = 0.10;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;
  const auto base = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  const IVolCurve& base_curve = **base;

  const auto w_prev = [&base_curve](double k) {
    const double z = (k + 0.517) / 2.5e-3;
    return base_curve.w(k) - 1.0e-9 + 9.0e-8 * std::exp(-z * z);
  };

  ConvexRepairSpec spec;
  spec.k_min = -0.50;
  spec.k_max = 0.50;
  spec.grid_points = 65;
  spec.tolerance = 1.0e-9;

  cfg.convex_repair = spec; // grid alone cannot see k=-0.517 (outside band)
  const auto miss = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(miss.has_value()) << miss.error().to_string();
  EXPECT_GT(w_prev(-0.517) - (*miss)->w(-0.517), 1.0e-8);

  spec.extra_node_ks = {-0.517};
  cfg.convex_repair = spec;
  const auto hit = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(hit.has_value()) << hit.error().to_string();
  EXPECT_LE(w_prev(-0.517) - (*hit)->w(-0.517), 1.0e-8)
      << "promoted node must carry the w_prev floor exactly";
}

TEST(VolCurve, ConvexRepairSpecInvalidSpecRejected) {
  constexpr double T = 0.10;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;
  cfg.convex_repair = ConvexRepairSpec{};
  cfg.convex_repair->grid_points = 1; // inclusive grid needs >= 2 points
  const auto one_point = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_FALSE(one_point.has_value());
  EXPECT_EQ(one_point.error().code(), atx::core::ErrorCode::InvalidArgument);

  cfg.convex_repair = ConvexRepairSpec{};
  cfg.convex_repair->k_min = 0.5;
  cfg.convex_repair->k_max = -0.5; // inverted band
  const auto inverted = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_FALSE(inverted.has_value());
  EXPECT_EQ(inverted.error().code(), atx::core::ErrorCode::InvalidArgument);
}
