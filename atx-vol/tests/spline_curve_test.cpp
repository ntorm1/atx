// Tests for the SplineVol (SpiderRock SRCubic-style) vol-multiple cubic
// spline curve family + fitter (atx/vol/spline_curve.hpp).

#include "atx/vol/api/fitting/spline_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/arb.hpp"  // CalendarPairProjection (project_calendar result)
#include "atx/vol/api/fitting/calib.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"

namespace {

using atx::vol::CurveConfig;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::FitObs;
using atx::vol::fit_slice_curve;
using atx::vol::fit_spline_vol_slice;
using atx::vol::IVolCurve;
using atx::vol::kSrMoneynessGrid;
using atx::vol::Result;
using atx::vol::SplineFitOpts;
using atx::vol::SplineVolCurve;
using atx::vol::SviParams;
using atx::vol::svi_total_w;
using atx::vol::VolCurveKind;

// The fitter always succeeds into a SplineVolCurve; every test wants at the
// concrete params (mult / z / n_butterfly_viol), so downcast once here.
const SplineVolCurve &as_spline(const IVolCurve &c) {
  return static_cast<const SplineVolCurve &>(c);
}

std::vector<FitObs> flat_smile_obs(double iv, int n, double k_half_width) {
  std::vector<FitObs> obs(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = (n > 1) ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.5;
    FitObs o;
    o.k = -k_half_width + t * (2.0 * k_half_width);
    o.sigma_mkt = iv;
    o.weight_w = 1.0;
    obs[static_cast<std::size_t>(i)] = o;
  }
  return obs;
}

// Raw-SVI-generated observations: iv_i = sqrt(svi_total_w(params, k_i) / T),
// uniform tight weights. Hand-checkable against the closed-form svi_total_w.
std::vector<FitObs> svi_smile_obs(const SviParams &p, double T, int n, double k_half_width) {
  std::vector<FitObs> obs(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = (n > 1) ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.5;
    const double k = -k_half_width + t * (2.0 * k_half_width);
    const double w = svi_total_w(p, k);
    FitObs o;
    o.k = k;
    o.sigma_mkt = std::sqrt(w / T);
    o.weight_w = 1.0;
    obs[static_cast<std::size_t>(i)] = o;
  }
  return obs;
}

TEST(SplineVol, FlatSmileRoundTrip) {
  const std::vector<FitObs> obs = flat_smile_obs(0.20, 15, 0.5);
  const double F = 100.0, T = 0.5, df = 0.99;

  auto fitted = fit_spline_vol_slice(obs, F, T, df);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const IVolCurve &curve = **fitted;
  EXPECT_EQ(curve.kind(), VolCurveKind::SplineVol);

  const auto &params = as_spline(curve).params();
  EXPECT_NEAR(params.atm_vol, 0.20, 1.0e-6);
  ASSERT_FALSE(params.mult.empty());
  for (double m : params.mult) {
    EXPECT_NEAR(m, 1.0, 1.0e-6);
  }

  for (int i = -5; i <= 5; ++i) {
    const double k = 0.1 * static_cast<double>(i);
    EXPECT_NEAR(curve.iv(k), 0.20, 1.0e-6) << "k=" << k;
  }
}

TEST(SplineVol, RecoversSviSmile) {
  SviParams svi;
  svi.a = 0.02;
  svi.b = 0.4;
  svi.rho = -0.3;
  svi.m = 0.0;
  svi.sigma = 0.4;
  const double T = 0.25, F = 100.0, df = 0.98;
  const double k_half_width = 0.6;

  const std::vector<FitObs> obs = svi_smile_obs(svi, T, 25, k_half_width);
  auto fitted = fit_spline_vol_slice(obs, F, T, df);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const IVolCurve &curve = **fitted;

  double sse = 0.0;
  int n = 0;
  for (int i = 0; i <= 100; ++i) {
    const double k = -k_half_width + (2.0 * k_half_width) * static_cast<double>(i) / 100.0;
    const double model_iv = curve.iv(k);
    const double true_iv = std::sqrt(svi_total_w(svi, k) / T);
    ASSERT_TRUE(std::isfinite(model_iv)) << "k=" << k;
    const double e = model_iv - true_iv;
    sse += e * e;
    ++n;
  }
  const double rmse = std::sqrt(sse / static_cast<double>(n));
  EXPECT_LT(rmse, 2.0e-3) << "rmse=" << rmse;
}

TEST(SplineVol, WingsAreFlat) {
  const std::vector<FitObs> obs = flat_smile_obs(0.20, 15, 0.5);
  auto fitted = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const IVolCurve &curve = **fitted;
  const auto &params = as_spline(curve).params();
  ASSERT_FALSE(params.z.empty());

  const double sqrtT = std::sqrt(curve.T());
  const double scale = params.atm_vol * sqrtT;
  const double k_far1 = 40.0 * scale;
  const double k_far2 = 1000.0 * scale;
  const double k_boundary = params.z.back() * scale;

  // Two points both comfortably beyond the outermost knot must be identical
  // (bit-exact: both clamp to the exact same stored z.back()).
  EXPECT_DOUBLE_EQ(curve.iv(k_far1), curve.iv(k_far2));
  // And must match the value at the clamp boundary itself (tight tolerance,
  // not bit-exact: k_boundary round-trips z through a multiply/divide).
  EXPECT_NEAR(curve.iv(k_far1), curve.iv(k_boundary), 1.0e-9);
}

TEST(SplineVol, DofCountsActiveKnots) {
  // Narrow board: all quotes within +/-0.05 log-moneyness of the money.
  const std::vector<FitObs> obs = flat_smile_obs(0.20, 10, 0.05);
  auto fitted = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const IVolCurve &curve = **fitted;
  EXPECT_LT(curve.dof(), kSrMoneynessGrid.size());
  EXPECT_GE(curve.dof(), 4u);
}

TEST(SplineVol, CloneIsDeepAndIdentical) {
  SviParams svi;
  svi.a = 0.02;
  svi.b = 0.4;
  svi.rho = -0.3;
  svi.m = 0.0;
  svi.sigma = 0.4;
  const double T = 0.25, F = 100.0, df = 0.98;
  const std::vector<FitObs> obs = svi_smile_obs(svi, T, 25, 0.6);

  auto fitted = fit_spline_vol_slice(obs, F, T, df);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const std::unique_ptr<IVolCurve> &original = *fitted;
  std::unique_ptr<IVolCurve> cloned = original->clone();

  ASSERT_NE(cloned.get(), original.get());
  EXPECT_EQ(cloned->kind(), original->kind());
  EXPECT_EQ(cloned->dof(), original->dof());
  for (int i = -12; i <= 12; ++i) {
    const double k = 0.05 * static_cast<double>(i);
    EXPECT_DOUBLE_EQ(cloned->w(k), original->w(k)) << "k=" << k;
  }
}

TEST(SplineVol, DispatchThroughFitSliceCurve) {
  const std::vector<FitObs> obs = flat_smile_obs(0.20, 15, 0.5);
  CurveConfig cfg;
  cfg.kind = VolCurveKind::SplineVol;

  auto fitted = fit_slice_curve(cfg, obs, 100.0, 0.5, 0.99);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  EXPECT_EQ((*fitted)->kind(), VolCurveKind::SplineVol);

  CurveSurface surface;
  surface.push(std::move(*fitted));
  ASSERT_EQ(surface.n_slices(), 1u);
  EXPECT_TRUE(std::isfinite(surface.w(0.0, 0.5)));
  EXPECT_TRUE(std::isfinite(surface.iv(0.0, 0.5)));
  EXPECT_NEAR(surface.iv(0.0, 0.5), 0.20, 1.0e-6);
}

TEST(SplineVol, RejectsDegenerateInputs) {
  const std::vector<FitObs> obs = flat_smile_obs(0.20, 15, 0.5);
  const SplineFitOpts opts{};  // default min_obs = 6

  const std::vector<FitObs> few(obs.begin(), obs.begin() + 3);
  auto too_few = fit_spline_vol_slice(few, 100.0, 0.5, 0.99, opts);
  ASSERT_FALSE(too_few.has_value());
  EXPECT_EQ(too_few.error().code(), ErrorCode::InvalidArgument);

  auto bad_f = fit_spline_vol_slice(obs, -1.0, 0.5, 0.99, opts);
  ASSERT_FALSE(bad_f.has_value());
  EXPECT_EQ(bad_f.error().code(), ErrorCode::InvalidArgument);

  auto bad_t = fit_spline_vol_slice(obs, 100.0, -0.1, 0.99, opts);
  ASSERT_FALSE(bad_t.has_value());
  EXPECT_EQ(bad_t.error().code(), ErrorCode::InvalidArgument);

  auto bad_df = fit_spline_vol_slice(obs, 100.0, 0.5, 0.0, opts);
  ASSERT_FALSE(bad_df.has_value());
  EXPECT_EQ(bad_df.error().code(), ErrorCode::InvalidArgument);
}

// T2: `natural_spline_m` divides by the knot gaps h[t] / h[t+1] (and by b[0],
// and by the Thomas pivot) with no zero or pivot check, and the entry point
// validated only `grid.size() >= 4` -- never that the grid ASCENDS, which
// `SplineVolParams::z` is documented to do and nothing enforced. One duplicated
// or unsorted knot made the whole second-derivative vector NaN and every served
// IV NaN, out of a fit that reported SUCCESS. Validate at the boundary.
TEST(SplineVol, RejectsNonAscendingKnotGrid) {
  const std::vector<FitObs> obs = flat_smile_obs(0.20, 15, 0.5);

  // The exact repro: a duplicated knot => h == 0 => division by zero.
  const std::vector<double> duplicated{-1.0, 0.0, 0.0, 1.0};
  SplineFitOpts dup_opts{};
  dup_opts.grid = duplicated;
  const auto dup = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99, dup_opts);
  ASSERT_FALSE(dup.has_value()) << "a duplicated knot must not fit";
  EXPECT_EQ(dup.error().code(), ErrorCode::InvalidArgument);

  const std::vector<double> unsorted{-1.0, 0.5, 0.0, 1.0};
  SplineFitOpts unsorted_opts{};
  unsorted_opts.grid = unsorted;
  const auto out_unsorted = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99, unsorted_opts);
  ASSERT_FALSE(out_unsorted.has_value());
  EXPECT_EQ(out_unsorted.error().code(), ErrorCode::InvalidArgument);

  const std::vector<double> non_finite{-1.0, 0.0,
                                       std::numeric_limits<double>::quiet_NaN(), 1.0};
  SplineFitOpts non_finite_opts{};
  non_finite_opts.grid = non_finite;
  const auto out_non_finite = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99, non_finite_opts);
  ASSERT_FALSE(out_non_finite.has_value());
  EXPECT_EQ(out_non_finite.error().code(), ErrorCode::InvalidArgument);

  // Control: the same board on a strictly ascending grid still fits, so the
  // guard rejects the degeneracy and not the fixture.
  const std::vector<double> ascending{-1.0, -0.25, 0.25, 1.0};
  SplineFitOpts ok_opts{};
  ok_opts.grid = ascending;
  const auto ok = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99, ok_opts);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  EXPECT_TRUE(std::isfinite(ok.value()->iv(0.0)));
}

// A hand-built or DESERIALIZED `SplineVolParams` never passes through the
// fitter's entry guard, so the spline core itself must not divide by a zero
// gap. It answers "no curvature" (the natural-spline answer for data it cannot
// difference) instead of poisoning every served point with NaN.
TEST(SplineVol, DegenerateParamsServeFiniteVolRatherThanNaN) {
  atx::vol::SplineVolParams p;
  p.atm_vol = 0.20;
  p.z = {-1.0, 0.0, 0.0, 1.0}; // duplicated knot: NOT reachable through the fitter
  p.mult = {1.0, 1.1, 1.1, 1.0};
  p.z_lo_valid = -1.0;
  p.z_hi_valid = 1.0;
  const SplineVolCurve curve(p, /*T=*/0.25, /*F=*/100.0, /*df=*/0.98);

  EXPECT_TRUE(std::isfinite(curve.iv(0.0)));
  EXPECT_TRUE(std::isfinite(curve.iv(-0.10)));
  EXPECT_TRUE(std::isfinite(curve.iv(0.10)));
}

TEST(SplineVol, ButterflyViolationCounterOnConvexData) {
  // A Martini-Mingone admissible raw-SVI slice (arb-free by construction) is
  // clean synthetic data: the fitted spline should introduce no butterfly
  // violations on the diagnostic scan.
  SviParams svi;
  svi.a = 0.02;
  svi.b = 0.4;
  svi.rho = -0.3;
  svi.m = 0.0;
  svi.sigma = 0.4;
  const double T = 0.25, F = 100.0, df = 0.98;
  const std::vector<FitObs> obs = svi_smile_obs(svi, T, 25, 0.6);

  auto fitted = fit_spline_vol_slice(obs, F, T, df);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const IVolCurve &curve = **fitted;
  EXPECT_EQ(as_spline(curve).params().n_butterfly_viol, 0u);
}

TEST(SplineVol, ButterflyViolationCounterFiresOnAnArbitrageableSmile) {
  // Companion to the convex-data case above, which only ever observed a ZERO
  // count -- so nothing pinned the COUNTING branch of the post-fit scan, which
  // is exactly the branch the shared-FD-rule unification moved. This slice's
  // wing sits far past the Lee bound, so the fitted spline inherits a genuinely
  // negative Durrleman density.
  SviParams svi;
  svi.a = 0.04;
  svi.b = 4.0;
  svi.rho = 0.0;
  svi.m = 0.0;
  svi.sigma = 0.1;
  const double T = 1.0, F = 100.0, df = 1.0;
  const std::vector<FitObs> obs = svi_smile_obs(svi, T, 25, 0.6);

  auto fitted = fit_spline_vol_slice(obs, F, T, df);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const std::uint32_t n_viol = as_spline(**fitted).params().n_butterfly_viol;
  std::printf("[bf-anchor] spline  n_butterfly_viol=%llu\n",
              static_cast<unsigned long long>(n_viol));
  // Exact, not just non-zero: this diagnostic is fed by the spline fitter AND by
  // the shared FD rule, so a drift in either moves it. PROCEDURE if it turns red:
  // establish which of the two moved before re-pinning -- a fitter change that
  // legitimately reshapes the smile may re-pin here, but a change in the density
  // stencil or its tolerance must not, because the FD rule is shared with every
  // arb_check_butterfly entry and those are pinned bit-for-bit in arb_test.cpp.
  EXPECT_EQ(n_viol, 94u);
}

// ── Self-review edge cases (not in the brief's fixed list, added for safety
// margin on numerical degeneracies the brief flags: n_obs < min_obs is
// already covered by RejectsDegenerateInputs above) ─────────────────────────

TEST(SplineVol, AllSameStrikeDoesNotCrashAndFailsCleanly) {
  // Every observation at the identical strike collapses the design matrix to
  // rank 1 (every basis row is identical); the fit should error out cleanly
  // via the SPD solve rather than produce garbage or crash.
  std::vector<FitObs> obs(10);
  for (FitObs &o : obs) {
    o.k = 0.0;
    o.sigma_mkt = 0.20;
    o.weight_w = 1.0;
  }
  auto fitted = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99);
  if (fitted.has_value()) {
    EXPECT_TRUE(std::isfinite((*fitted)->w(0.0)));
  } else {
    EXPECT_NE(fitted.error().code(), ErrorCode::Unknown);
  }
}

TEST(SplineVol, ZeroWeightObsAreFilteredAndRejected) {
  // weight_w <= 0 is not a usable WLS weight; every row is dropped by the
  // validity filter, so this degenerates to the "fewer than min_obs valid
  // observations" InvalidArgument path (not a divide-by-zero).
  std::vector<FitObs> obs = flat_smile_obs(0.20, 15, 0.5);
  for (FitObs &o : obs) {
    o.weight_w = 0.0;
  }
  auto fitted = fit_spline_vol_slice(obs, 100.0, 0.5, 0.99);
  ASSERT_FALSE(fitted.has_value());
  EXPECT_EQ(fitted.error().code(), ErrorCode::InvalidArgument);
}

TEST(SplineVol, CalendarProjectionClearsInteriorCrossingOnTradeableOverlap) {
  // A near-money (in-sample) calendar crossing: a HIGH-vol short slice whose
  // total variance sits above a LOW-vol long slice across the shared tradeable
  // range (short ATM w = 0.55^2*0.10 = 0.0303 > long ATM w = 0.20^2*0.5 = 0.020).
  // This is the crossing the projection must clear on the overlap.
  const double F = 100.0;
  const double T_short = 0.10;
  const double T_long = 0.50;

  const std::vector<FitObs> short_obs = flat_smile_obs(0.55, 15, 0.30);  // wide, high vol
  auto short_fit = fit_spline_vol_slice(short_obs, F, T_short, 0.99);
  ASSERT_TRUE(short_fit.has_value()) << short_fit.error().to_string();
  auto &short_curve = static_cast<SplineVolCurve &>(**short_fit);

  const std::vector<FitObs> long_obs = flat_smile_obs(0.20, 15, 0.30);  // wide, low vol
  auto long_fit = fit_spline_vol_slice(long_obs, F, T_long, 0.98);
  ASSERT_TRUE(long_fit.has_value()) << long_fit.error().to_string();
  auto &long_curve = static_cast<SplineVolCurve &>(**long_fit);

  const auto w_prev = [&](double k) { return short_curve.w(k); };
  const double k_min = -0.60, k_max = 0.60;
  const std::uint32_t n_grid = 64;
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);

  // is_extrapolated marks the flat wing beyond the observed strikes (|k| ~ 0.30).
  EXPECT_TRUE(long_curve.is_extrapolated(0.59));
  EXPECT_FALSE(long_curve.is_extrapolated(0.0));

  // Project against the short slice, handing over its data range so enforcement
  // is confined to the tradeable OVERLAP.
  const auto [kp_lo, kp_hi] = short_curve.data_k_range();
  auto proj = long_curve.project_calendar(w_prev, k_min, k_max, n_grid, kp_lo, kp_hi);
  ASSERT_TRUE(proj.has_value()) << proj.error().to_string();
  EXPECT_GT(proj->max_deficit_before, 1.0e-4) << "the scenario must exhibit a real crossing";

  // Post-projection: no calendar arb over the tradeable overlap (where BOTH
  // slices carry quotes and neither is extrapolating).
  double max_deficit = 0.0;
  for (std::uint32_t g = 0; g <= n_grid; ++g) {
    const double k = k_min + dk * static_cast<double>(g);
    if (short_curve.is_extrapolated(k) || long_curve.is_extrapolated(k)) {
      continue;
    }
    max_deficit = std::max(max_deficit, w_prev(k) - long_curve.w(k));
  }
  EXPECT_LE(max_deficit, 1.0e-6) << "residual calendar deficit on the tradeable overlap";
}

TEST(SplineVol, LambdaZeroStillSolvesOnWellPosedData) {
  SviParams svi;
  svi.a = 0.02;
  svi.b = 0.4;
  svi.rho = -0.3;
  svi.m = 0.0;
  svi.sigma = 0.4;
  const double T = 0.25, F = 100.0, df = 0.98;
  const std::vector<FitObs> obs = svi_smile_obs(svi, T, 25, 0.6);

  SplineFitOpts opts;
  opts.lambda = 0.0;
  auto fitted = fit_spline_vol_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  EXPECT_TRUE(std::isfinite((*fitted)->iv(0.0)));
}

}  // namespace
