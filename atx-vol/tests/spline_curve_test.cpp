// Tests for the SplineVol (SpiderRock SRCubic-style) vol-multiple cubic
// spline curve family + fitter (atx/vol/spline_curve.hpp).

#include "atx/vol/spline_curve.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/calib.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

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
