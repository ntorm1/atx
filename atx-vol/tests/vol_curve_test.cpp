#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <span>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_butterfly_svi_mm, arb_check_butterfly_slice
#include "atx/vol/black76.hpp"      // black76_value_and_vega
#include "atx/vol/calib.hpp"        // CalibOpts, FitObs
#include "atx/vol/essvi_calib.hpp"  // essvi_fit_slice
#include "atx/vol/spline_curve.hpp" // fit_spline_vol_slice
#include "atx/vol/svi_calib.hpp"    // svi_project_mm
#include "atx/vol/types.hpp"        // Side
#include "atx/vol/vol_curve.hpp"    // fit_slice_curve, CurveConfig, SviCurve, C8Curve
#include "atx/vol/vol_surface.hpp"  // SviParams, EssviParams, essvi_backbone_w, essvi_total_w

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

// C-8 (FIT-C5) fixture: CORE obs (|k| <= 0.15) are pure eSSVI-backbone-
// consistent quotes; WING obs (k in [0.55, 1.2], beyond the HINGE_QUAD dead
// band at 0.4 * scale = 0.48 for scale = kmax = 1.2) carry a deliberate
// upward quadratic bump the rigid 3-parameter backbone cannot reproduce.
[[nodiscard]] std::vector<FitObs> make_essvi_wing_residual_obs() {
  const double T = 0.5, F = 100.0;
  atx::vol::EssviParams tr{};
  tr.theta = 0.04;
  tr.phi = 1.0;
  tr.rho = -0.25;
  tr.T = T;

  const auto push = [&](std::vector<FitObs>& obs, double k, double w) {
    FitObs o{};
    o.k = k;
    o.w_mkt = w;
    o.sigma_mkt = std::sqrt(w / T);
    o.weight_w = 1.0;
    o.active_weight_w = 1.0;
    o.F = F;
    o.K = F * std::exp(k);
    o.df = 1.0;
    obs.push_back(o);
  };

  std::vector<FitObs> obs;
  // A LARGE core count is load-bearing, not cosmetic: the joint (core + wing)
  // LM otherwise lets the 3-parameter backbone itself chase part of the wing
  // bump (phi drifting several-fold off truth was observed at n_core ~ 25-70),
  // which contaminates the ridge-LS "leftover" the residual layer fits and
  // pulls a spurious HINGE_QUAD linear (yc) term out of what should be a pure
  // quadratic bump — that stray linear term's OWN kink (HINGE_QUAD is only
  // C0 at its dead-band boundary) then trips the narrow scan on its own,
  // independent of the deliberate wing arb this fixture targets. Outweighing
  // the 6 wing points ~65:1 anchors the backbone to the core-only optimum
  // (verified: recovers truth to fp precision) and leaves the wing bump for
  // the residual layer to absorb cleanly.
  const int n_core = 400;
  for (int i = 0; i < n_core; ++i) {
    const double k = -0.15 + 0.30 * static_cast<double>(i) /
                                  static_cast<double>(n_core - 1);
    push(obs, k, atx::vol::essvi_backbone_w(tr, k));
  }
  constexpr double kScale = 1.2;         // == kmax over all obs (matches
                                         // fit_wing_residual's own `scale`)
  constexpr double kResidInnerY = 0.4;  // HINGE_QUAD's dead-band boundary
  const std::array<double, 6> wing_k{0.75, 0.85, 0.95, 1.05, 1.15, 1.20};
  for (const double k : wing_k) {
    double w = atx::vol::essvi_backbone_w(tr, k);
    const double y = std::clamp(k / kScale, -1.0, 1.0);
    if (y > kResidInnerY) {
      const double yc = y - kResidInnerY;
      w += 0.5 * yc * yc;
    }
    push(obs, k, w);
  }
  return obs;
}

// FIT-C5 (mirrors FT-C2's SviServedSliceRejectsWingButterflyArb): the eSSVI
// backbone is butterfly-arb-free EVERYWHERE by construction (the Mingone
// cube-space fit enforces the Lee/Gatheral-Jacquier bound), but its optional
// HINGE_QUAD wing-residual layer is NOT projected onto the admissible cone
// (the per-slice Roper projector is out of port scope; see the PORT NOTE on
// `fit_wing_residual`, essvi_calib.cpp) — the same class of served-arb gap
// FT-C2 closed for raw-SVI. This fixture is quoted out to |k| = 1.2 with a
// HINGE_QUAD-shaped wing bump the rigid backbone cannot absorb: the residual
// reproduces it verbatim and carries genuine Durrleman g < 0 beyond k = 0.6,
// invisible to the OLD fixed [-0.6, 0.6] scan. The served path must scan the
// full quoted range +/- 0.5 (FT-C2/FIT-C5) and refuse it.
TEST(VolCurve, EssviServedSliceRejectsWingButterflyArb) {
  const double T = 0.5, F = 100.0, df = 1.0;
  const std::vector<FitObs> obs = make_essvi_wing_residual_obs();

  CalibOpts po = vc_permissive_opts();
  po.residual_disable = false;
  po.residual_basis_kind = atx::vol::ResidualBasisKind::HingeQuad;

  // Non-vacuity: the RAW fit passes clean on the old [-0.6, 0.6] scan yet
  // carries genuine wing butterfly arb over the quoted range +/- 0.5.
  const auto raw =
      atx::vol::essvi_fit_slice(std::span<const FitObs>(obs), T, F, po);
  ASSERT_TRUE(raw.has_value());
  const atx::vol::EssviParams sp = raw.value();
  ASSERT_GT(sp.resid_scale, 0.0)
      << "fixture must actually engage the wing-residual layer";
  const std::function<double(double)> wf = [&](double k) {
    return atx::vol::essvi_total_w(sp, k);
  };
  const auto narrow = arb_check_butterfly_slice(wf, T, -0.6, 0.6, 256u);
  ASSERT_TRUE(narrow.has_value());
  EXPECT_TRUE(narrow->empty()) << "fixture must be clean on the old [-0.6,0.6] scan";
  // Mirrors validate_served_shape_over_quotes' own window: min(-0.6, k_lo-0.5)
  // .. max(0.6, k_hi+0.5) for this fixture's obs (k in [-0.15, 1.20]).
  const auto wide = arb_check_butterfly_slice(wf, T, -0.65, 1.70, 256u);
  ASSERT_TRUE(wide.has_value());
  EXPECT_FALSE(wide->empty()) << "fixture must carry real wing arb over quoted+/-0.5";

  // Served path: must REFUSE.
  CurveConfig cfg;
  cfg.kind = VolCurveKind::Essvi;
  cfg.parametric = po;
  const auto served = fit_slice_curve(cfg, obs, F, T, df);
  EXPECT_FALSE(served.has_value())
      << "served-path admitted an eSSVI slice with wing butterfly arb over the "
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

// Task F-4 fix round 1, finding F1. `ConvexRepairSpec{}` exists to reproduce
// the `convex_repair == nullopt` path's fixed lattice, and all FOUR of its
// defaults were hand-kept duplicates of that path's own constants -- three
// literals living in vol_curve.cpp's anonymous namespace, plus a sixth copy of
// the library's calendar tolerance. They feed the SAME variable through the two
// branches of one ternary (`calendar_tol`, vol_curve.cpp), so a drift in either
// direction changes which crossings a fit accepts, selected by nothing but
// whether an `optional` is engaged.
//
// Both halves are pinned here, because either alone is insufficient: the
// compile-time half would pass if the shared constants were themselves wrong,
// and the behavioural half would pass today on equal-but-unlinked literals.
TEST(VolCurve, ConvexRepairSpecDefaultsAreTheNullOptLattice) {
  const ConvexRepairSpec spec{};

  // (a) Compile-time: every default NAMES its source. `EXPECT_EQ` on doubles is
  // deliberate -- these must be the same constant, not merely close.
  EXPECT_EQ(spec.k_min, atx::vol::kConvexCalendarLatticeKMin);
  EXPECT_EQ(spec.k_max, atx::vol::kConvexCalendarLatticeKMax);
  EXPECT_EQ(spec.grid_points, atx::vol::kConvexCalendarLatticeIntervals + 1u);
  EXPECT_EQ(spec.tolerance, atx::vol::kCalendarTotalVarianceTol);

  // (b) Behavioural: the two branches of the ternary agree on a real fit. The
  // fixture carries a calendar crossing of +9e-8 at k = -0.45 -- deliberately
  // sized BELOW the 1e-7 acceptance, so both branches must SERVE it.
  //
  // WHAT (b) DISCRIMINATES AGAINST, stated exactly (Task F-4 fix round 2, N3):
  // a TIGHTENING tolerance drift, and a band or grid that moved off the
  // lattice. Sweeping the drifted `tolerance` on this fixture: at 5e-8 and
  // below the crossing stops being accepted, the branch promotes a node and
  // refits, and the served curve moves by 4.499090e-03 -- 4.5e9x this gate. (A
  // tight enough drift can instead make that refit fail KKT certification, in
  // which case the ASSERT_TRUE above fires; either way (b) is loud.)
  //
  // A LOOSENING drift is INVISIBLE to (b): 8.9e-08 sits below 1e-7 and below
  // 1e-6 alike, so both branches keep serving it identically and (b) passes.
  // Half (a) is what covers loosening -- which is why both halves ship, and
  // why neither is redundant.
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
    const double z = (k + 0.45) / 3.0e-3;
    return base_curve.w(k) - 1.0e-9 + 9.0e-8 * std::exp(-z * z);
  };

  const auto null_opt = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(null_opt.has_value()) << null_opt.error().to_string();

  cfg.convex_repair = ConvexRepairSpec{};
  const auto defaulted = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(defaulted.has_value()) << defaulted.error().to_string();

  // The crossing must actually be present, or (b) is vacuous.
  const double crossing = w_prev(-0.45) - (*null_opt)->w(-0.45);
  EXPECT_GT(crossing, 1.0e-8) << "fixture must carry a sub-tolerance crossing";

  for (const double k : {-0.60, -0.45, -0.25, 0.0, 0.25, 0.60}) {
    EXPECT_NEAR((*null_opt)->w(k), (*defaulted)->w(k), 1.0e-12)
        << "nullopt and ConvexRepairSpec{} disagree at k=" << k;
  }
}

// Task P-5 (FIT-P1): the calendar-admission scan_k (fit_slice_curve's
// ConvexDense branch) used to invert the fitted node price to an implied vol
// via ConvexSliceFit::iv() and square it back to a total variance just to
// compare against w_prev -- pure waste, since the floor is enforced in PRICE
// space by fit_convex_slice's own cfloor rows. This pins that the
// price-space rewrite selects the IDENTICAL set of floor violations (hence
// the identical required_k growth and served curve) as the pre-P-5
// vol-space scan, across a fixture matrix spanning every scan_k call site:
// legacy slack (no violation expected), legacy crossing that the coarse
// 64-interval lattice cannot see (no violation on THIS lattice either --
// verbatim ConvexRepairSpecRepairsOffLatticeCalendarCrossing's fixture),
// strict on-lattice (repairs), and strict off-lattice via extra_node_ks
// (verbatim ConvexRepairSpecExtraNodeKsBecomeExactFloorNodes's fixture).
//
// `kPreP5*` are served w(k) at 9 representative k's (both wings, the
// kink/repair regions, and ATM) captured from the UNMODIFIED (pre-P-5,
// iv()-inversion) scan -- revert d283efe to reproduce the capture. The full
// 41-point / 4-case comparison ran at max observed drift ~8e-13, driven
// entirely by the UNRELATED iv() bisection early-exit task P-5 also lands,
// not by a different floor being chosen). 1e-9 here is ~1e4x that noise
// floor and ~1e6x below the ~1e-3 scale a genuinely different floor
// selection would move these values by (this bug WAS caught this way during
// implementation: an earlier, less faithful price-space rewrite that
// skipped iv()'s wing safe-price projection moved these same points by
// 3e-3-5e-3 -- a completely different set of floors chosen, not noise).
TEST(VolCurve, CalendarScanPriceSpaceSelectsIdenticalFloorsAsPreP5Baseline) {
  constexpr double T = 0.10;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);
  constexpr std::array<double, 9> kSampleK = {-0.60, -0.51, -0.33, -0.21, 0.0,
                                              0.21,  0.33,  0.51,  0.60};

  const auto expect_matches = [&](const char* label, const IVolCurve& curve,
                                  const std::array<double, 9>& pinned) {
    for (std::size_t i = 0; i < kSampleK.size(); ++i) {
      EXPECT_NEAR(curve.w(kSampleK[i]), pinned[i], 1.0e-9)
          << label << " k=" << kSampleK[i];
    }
  };

  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;
  const auto base = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  const IVolCurve& base_curve = **base;

  // Case A: mostly-slack w_prev (no violations expected on the legacy lattice).
  const auto w_prev_slack = [&base_curve](double k) { return base_curve.w(k) - 1.0e-3; };
  const auto slack = fit_slice_curve(cfg, obs, F, T, df, w_prev_slack);
  ASSERT_TRUE(slack.has_value()) << slack.error().to_string();
  static constexpr std::array<double, 9> kPreP5Slack = {
      0.023073812939698696, 0.016885898743741863, 0.0073482334401989667,
      0.0061095771687040704, 0.0048400976716167256, 0.005410183306104998,
      0.0071283048771612623, 0.015982888220143317, 0.021649702357522241,
  };
  expect_matches("slack", **slack, kPreP5Slack);

  // Case B: legacy off-lattice crossing (verbatim ConvexRepairSpecRepairsOffLatticeCalendarCrossing
  // fixture) -- exercises the coarse 64-interval legacy scan and at least one refit pass.
  // The bump is narrow enough (3e-3 width) that the legacy lattice never samples
  // inside it, so this case's served curve is expected to equal Case A's exactly.
  const auto w_prev_bump = [&base_curve](double k) {
    const double z = (k + 0.50) / 3.0e-3;
    return base_curve.w(k) - 1.0e-9 + 9.0e-8 * std::exp(-z * z);
  };
  const auto legacy = fit_slice_curve(cfg, obs, F, T, df, w_prev_bump);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  expect_matches("legacy_crossing", **legacy, kPreP5Slack);

  // Case C: strict ConvexRepairSpec on-lattice (same bump, dense 65-point grid).
  CurveConfig strict_cfg = cfg;
  strict_cfg.convex_repair = ConvexRepairSpec{};
  strict_cfg.convex_repair->k_min = -0.50;
  strict_cfg.convex_repair->k_max = 0.50;
  strict_cfg.convex_repair->grid_points = 65;
  strict_cfg.convex_repair->tolerance = 1.0e-9;
  const auto strict = fit_slice_curve(strict_cfg, obs, F, T, df, w_prev_bump);
  ASSERT_TRUE(strict.has_value()) << strict.error().to_string();
  static constexpr std::array<double, 9> kPreP5StrictOnLattice = {
      0.023073812939698696, 0.016885898743741863, 0.015900422507329481,
      0.0080931546180809163, 0.004840097628925997, 0.0054101833061062842,
      0.0071283048771754376, 0.015982888220143317, 0.021649702357522241,
  };
  expect_matches("strict_on_lattice", **strict, kPreP5StrictOnLattice);

  // Case D: strict ConvexRepairSpec off-lattice via extra_node_ks (verbatim
  // ConvexRepairSpecExtraNodeKsBecomeExactFloorNodes fixture).
  const auto w_prev_off = [&base_curve](double k) {
    const double z = (k + 0.517) / 2.5e-3;
    return base_curve.w(k) - 1.0e-9 + 9.0e-8 * std::exp(-z * z);
  };
  CurveConfig off_cfg = cfg;
  ConvexRepairSpec off_spec;
  off_spec.k_min = -0.50;
  off_spec.k_max = 0.50;
  off_spec.grid_points = 65;
  off_spec.tolerance = 1.0e-9;
  off_spec.extra_node_ks = {-0.517};
  off_cfg.convex_repair = off_spec;
  const auto off_lattice = fit_slice_curve(off_cfg, obs, F, T, df, w_prev_off);
  ASSERT_TRUE(off_lattice.has_value()) << off_lattice.error().to_string();
  static constexpr std::array<double, 9> kPreP5StrictOffLattice = {
      0.023073812939698696, 0.021083968927089494, 0.016052219842009045,
      0.0081063700823840162, 0.0048400976266193101, 0.0054101833061063068,
      0.0071283048771760829, 0.015982888220143317, 0.021649702357522241,
  };
  expect_matches("strict_off_lattice", **off_lattice, kPreP5StrictOffLattice);
}
