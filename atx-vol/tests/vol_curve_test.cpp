#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_butterfly_svi_mm, arb_check_butterfly_slice
#include "atx/vol/black76.hpp"      // black76_value_and_vega
#include "atx/vol/calib.hpp"        // CalibOpts, FitObs
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

TEST(VolCurve, SviProjectMmRepairsLeeViolation) {
  // The gate PROJECTS an inadmissible fit before rejecting; verify that repair
  // primitive directly. A steep-wing slice (b*(1+|rho|) far past 4/T) is Lee-
  // inadmissible; svi_project_mm must move it back into the polytope.
  SviParams s{};
  s.a = 0.04;
  s.b = 4.0;  // b*(1+|rho|) = 4 >> 4/T = 2  (T = 2)
  s.rho = 0.0;
  s.m = 0.0;
  s.sigma = 0.1;
  s.T = 2.0;
  ASSERT_GT(arb_check_butterfly_svi_mm(s, s.T).n_violations, 0u);

  const bool moved = svi_project_mm(s, s.T);
  EXPECT_TRUE(moved);
  EXPECT_EQ(arb_check_butterfly_svi_mm(s, s.T).n_violations, 0u);
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
