#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_butterfly_svi_mm
#include "atx/vol/black76.hpp"      // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"        // FitObs, CalibOpts, calib_default_opts
#include "atx/vol/curve.hpp"        // CurveSet, ForwardPoint
#include "atx/vol/svi_calib.hpp"    // fitters + JW conversions
#include "atx/vol/universe.hpp"     // Underlying, Chain, chain_index
#include "atx/vol/vol_surface.hpp"  // VolSurface, SviParams, Parametrization

// Coverage for the per-slice raw-SVI calibrators (svi_calib.hpp), ported
// alongside the C ats-vol quasi-explicit SVI fitter (test_calibrate_svi.c) and
// the Martini-Mingone constrained LM (test_calibrate_svi_mm.c):
//   - SVI-on-SVI synthetic recovery: the quasi-explicit fitter recovers a
//     known (a, b, rho, m, sigma) tuple to within 1% relative on w(k);
//   - SVI-MM synthetic recovery + Mingone admissibility of the fitted slice;
//   - JW round-trip raw -> jw -> raw;
//   - surface drivers on a synthetic single-slice chain.

namespace {

using atx::vol::arb_check_butterfly_svi_mm;
using atx::vol::black76_price;
using atx::vol::black76_value_and_vega;
using atx::vol::CalibOpts;
using atx::vol::calib_default_opts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveSet;
using atx::vol::ErrorCode;
using atx::vol::FitDiag;
using atx::vol::FitObs;
using atx::vol::ForwardPoint;
using atx::vol::Parametrization;
using atx::vol::Side;
using atx::vol::svi_fit_slice;
using atx::vol::svi_jw_to_raw;
using atx::vol::svi_mm_calib_surface;
using atx::vol::svi_mm_fit_slice;
using atx::vol::svi_calib_surface;
using atx::vol::svi_raw_to_jw;
using atx::vol::SviJwParams;
using atx::vol::SviParams;
using atx::vol::Underlying;
using atx::vol::VolSurface;

// Raw SVI total variance — independent copy so the tests do not depend on the
// impl under test.
[[nodiscard]] double svi_w(double a, double b, double rho, double m,
                           double sigma, double k) {
  const double dk = k - m;
  return a + b * (rho * dk + std::sqrt(dk * dk + sigma * sigma));
}

// Uniform log-moneyness grid of unit-weight observations (ports the C
// `build_uniform_obs`). Only the (k, sigma_mkt, w_mkt, weight) fields matter to
// the quasi-explicit fitter.
[[nodiscard]] std::vector<FitObs> build_uniform_obs(std::size_t n, double k_lo,
                                                    double k_hi, double a,
                                                    double b, double rho,
                                                    double m, double sigma,
                                                    double T) {
  std::vector<FitObs> obs(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double k =
        k_lo + (k_hi - k_lo) * static_cast<double>(i) / static_cast<double>(n - 1);
    const double w = svi_w(a, b, rho, m, sigma, k);
    FitObs& o = obs[i];
    o.k = k;
    o.sigma_mkt = std::sqrt(w / T);
    o.w_mkt = w;
    o.weight_w = 1.0;
    o.active_weight_w = 1.0;
  }
  return obs;
}

// Price-domain observation grid at a known raw-SVI slice (ports the C
// `build_obs_from_svi`): F = 100, df = 1, OTM leg per strike, mid = B76 at the
// planted sigma, spread = 1% of mid, vega computed inline.
[[nodiscard]] std::vector<FitObs> build_obs_from_svi(std::size_t n, double k_lo,
                                                     double k_hi, double a,
                                                     double b, double rho,
                                                     double m, double sigma,
                                                     double T) {
  std::vector<FitObs> obs(n);
  const double F = 100.0;
  const double df = 1.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double k =
        k_lo + (k_hi - k_lo) * static_cast<double>(i) / static_cast<double>(n - 1);
    const double K = F * std::exp(k);
    const double w = svi_w(a, b, rho, m, sigma, k);
    const double sig = std::sqrt(w / T);
    const Side side = (K >= F) ? Side::Call : Side::Put;
    const double mid = black76_price(F, K, T, sig, df, side);
    const double vega = black76_value_and_vega(F, K, T, sig, df, side).vega;
    FitObs& o = obs[i];
    o.k = k;
    o.sigma_mkt = sig;
    o.w_mkt = w;
    o.K = K;
    o.F = F;
    o.df = df;
    o.mid = mid;
    o.spread = 0.01 * (mid > 0.01 ? mid : 0.01);
    o.vega = vega;
    o.side = side;
    o.weight_w = 1.0;
    o.active_weight_w = 1.0;
  }
  return obs;
}

// Max relative error of the fitted total-variance curve vs truth over a dense
// central grid (the C tests' recovery metric).
[[nodiscard]] double max_rel_w_error(const SviParams& fit, double a, double b,
                                     double rho, double m, double sigma,
                                     int i_lo, int i_hi) {
  double max_rel = 0.0;
  for (int i = i_lo; i <= i_hi; ++i) {
    const double k = 0.01 * static_cast<double>(i);
    const double w_true = svi_w(a, b, rho, m, sigma, k);
    const double w_fit = svi_w(fit.a, fit.b, fit.rho, fit.m, fit.sigma, k);
    const double rel = std::fabs(w_fit - w_true) / w_true;
    if (rel > max_rel) {
      max_rel = rel;
    }
  }
  return max_rel;
}

// ── SVI quasi-explicit recovery ──────────────────────────────────────────

TEST(SviCalib, RecoverSyntheticParams_WithinOnePercentOnWk) {
  const double a = 0.012;
  const double b = 0.06;
  const double rho = -0.40;
  const double m = -0.02;
  const double sigma = 0.18;
  const double T = 0.5;

  const std::vector<FitObs> obs =
      build_uniform_obs(41, -0.40, 0.40, a, b, rho, m, sigma, T);
  const CalibOpts opts = calib_default_opts();
  FitDiag diag{};

  const auto res =
      svi_fit_slice(std::span<const FitObs>(obs), T, 100.0, opts, &diag);
  ASSERT_TRUE(res.has_value());

  const double max_rel = max_rel_w_error(res.value(), a, b, rho, m, sigma, -50, 50);
  EXPECT_LT(max_rel, 0.01);
  EXPECT_LT(diag.rmse_vol_vega_weighted, 1.0e-4);
  EXPECT_EQ(diag.n_quotes_used, 41u);
}

TEST(SviCalib, RecoveryRobustToInitialSeed_PositiveSkew) {
  const double a = 0.020;
  const double b = 0.10;
  const double rho = +0.20;  // positive skew (rare but valid)
  const double m = +0.05;
  const double sigma = 0.30;
  const double T = 1.0;

  const std::vector<FitObs> obs =
      build_uniform_obs(31, -0.50, 0.50, a, b, rho, m, sigma, T);
  const CalibOpts opts = calib_default_opts();

  const auto res = svi_fit_slice(std::span<const FitObs>(obs), T, 100.0, opts);
  ASSERT_TRUE(res.has_value());

  const double max_rel = max_rel_w_error(res.value(), a, b, rho, m, sigma, -40, 40);
  EXPECT_LT(max_rel, 0.02);
}

TEST(SviCalib, EmptyObservations_ReturnsInvalidArgument) {
  const std::vector<FitObs> obs;
  const auto res = svi_fit_slice(std::span<const FitObs>(obs), 0.5, 100.0,
                                 calib_default_opts());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(SviCalib, NonPositiveT_ReturnsInvalidArgument) {
  const std::vector<FitObs> obs =
      build_uniform_obs(11, -0.2, 0.2, 0.01, 0.05, -0.3, 0.0, 0.15, 0.5);
  const auto res =
      svi_fit_slice(std::span<const FitObs>(obs), 0.0, 100.0, calib_default_opts());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── SVI-MM constrained recovery + admissibility ──────────────────────────

TEST(SviMmCalib, RecoverSyntheticAdmissibleSlice) {
  const double T = 0.5;
  const double a = 0.012;
  const double b = 0.060;
  const double rho = -0.30;
  const double m = -0.02;
  const double sigma = 0.18;

  const std::vector<FitObs> obs =
      build_obs_from_svi(41, -0.40, 0.40, a, b, rho, m, sigma, T);

  CalibOpts opts = calib_default_opts();
  // Disable Morozov / wing-floor so the LM runs to its FP-noise minimum on
  // noiseless synthetic data (mirrors the C test's opts).
  opts.morozov_stop = false;
  opts.wing_floor_alpha = 0.0;

  FitDiag diag{};
  const auto res =
      svi_mm_fit_slice(std::span<const FitObs>(obs), T, 100.0, opts, &diag);
  ASSERT_TRUE(res.has_value());
  const SviParams fit = res.value();

  const double max_rel = max_rel_w_error(fit, a, b, rho, m, sigma, -50, 50);
  EXPECT_LT(max_rel, 0.01);

  // The fitted slice must satisfy every Mingone inequality.
  const auto adm = arb_check_butterfly_svi_mm(fit, T);
  EXPECT_EQ(adm.n_violations, 0u);
}

TEST(SviMmCalib, FittedSlice_IsAlwaysAdmissible_EvenFromWideData) {
  // A steeper / higher-vol smile still projects into the polytope.
  const double T = 0.25;
  const double a = 0.02;
  const double b = 0.30;
  const double rho = -0.55;
  const double m = 0.03;
  const double sigma = 0.22;

  const std::vector<FitObs> obs =
      build_obs_from_svi(31, -0.35, 0.35, a, b, rho, m, sigma, T);
  CalibOpts opts = calib_default_opts();
  opts.morozov_stop = false;

  const auto res = svi_mm_fit_slice(std::span<const FitObs>(obs), T, 100.0, opts);
  ASSERT_TRUE(res.has_value());

  const auto adm = arb_check_butterfly_svi_mm(res.value(), T);
  EXPECT_EQ(adm.n_violations, 0u);
}

// ── JW conversions ───────────────────────────────────────────────────────

TEST(SviJw, RawToJwToRaw_RoundTripsOnAsymmetricSmile) {
  SviParams raw{};
  raw.a = 0.012;
  raw.b = 0.060;
  raw.rho = -0.40;
  raw.m = -0.02;
  raw.sigma = 0.18;
  raw.T = 0.5;

  const SviJwParams jw = svi_raw_to_jw(raw);
  EXPECT_GT(jw.v, 0.0);
  EXPECT_GT(jw.p, 0.0);
  EXPECT_GT(jw.c, 0.0);
  EXPECT_GT(jw.v_min, 0.0);

  const auto back = svi_jw_to_raw(jw);
  ASSERT_TRUE(back.has_value());
  const SviParams r = back.value();
  EXPECT_NEAR(r.a, raw.a, 1.0e-10);
  EXPECT_NEAR(r.b, raw.b, 1.0e-10);
  EXPECT_NEAR(r.rho, raw.rho, 1.0e-10);
  EXPECT_NEAR(r.m, raw.m, 1.0e-10);
  EXPECT_NEAR(r.sigma, raw.sigma, 1.0e-10);
  EXPECT_NEAR(r.T, raw.T, 1.0e-15);
}

TEST(SviJw, RawToJwToRaw_RoundTripsOnSymmetricSmile) {
  SviParams raw{};
  raw.a = 0.015;
  raw.b = 0.08;
  raw.rho = -0.25;
  raw.m = 0.0;  // symmetric => beta == 0 branch of the inverse
  raw.sigma = 0.20;
  raw.T = 1.0;

  const SviJwParams jw = svi_raw_to_jw(raw);
  const auto back = svi_jw_to_raw(jw);
  ASSERT_TRUE(back.has_value());
  const SviParams r = back.value();
  EXPECT_NEAR(r.a, raw.a, 1.0e-10);
  EXPECT_NEAR(r.b, raw.b, 1.0e-10);
  EXPECT_NEAR(r.rho, raw.rho, 1.0e-10);
  EXPECT_NEAR(r.m, raw.m, 1.0e-10);
  EXPECT_NEAR(r.sigma, raw.sigma, 1.0e-10);
}

TEST(SviJw, JwToRaw_InfeasibleTuple_ReturnsOutOfRange) {
  SviJwParams jw{};
  jw.v = 0.05;
  jw.psi = -0.1;
  jw.p = 0.2;
  jw.c = 0.3;
  jw.v_min = 0.10;  // v_min > v is infeasible
  jw.T = 0.5;
  const auto res = svi_jw_to_raw(jw);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

// ── Surface drivers on a synthetic single-slice chain ────────────────────

namespace {

// Build a synthetic single-expiry chain whose OTM legs price a known raw-SVI
// slice at F = 100, df = 1. Both call and put legs are populated at every
// strike; the observation builder picks the OTM leg.
[[nodiscard]] Underlying make_svi_underlying(double a, double b, double rho,
                                             double m, double sigma, double T,
                                             std::int64_t expiry_ns) {
  constexpr double F = 100.0;
  constexpr double df = 1.0;
  constexpr int n = 25;

  Underlying under{};
  under.uid = 1u;

  Chain c{};
  c.uid = 1u;
  c.expiry_id = 0u;
  c.expiry_ns = expiry_ns;
  c.T = T;

  for (int i = 0; i < n; ++i) {
    const double k = -0.30 + 0.60 * static_cast<double>(i) /
                                 static_cast<double>(n - 1);
    c.strikes.push_back(F * std::exp(k));
  }
  const std::size_t two_n = 2u * c.strikes.size();
  c.bids.assign(two_n, 0.0);
  c.asks.assign(two_n, 0.0);
  c.mids.assign(two_n, 0.0);
  c.flags.assign(two_n, 0u);

  for (std::size_t s = 0; s < c.strikes.size(); ++s) {
    const double K = c.strikes[s];
    const double k = std::log(K / F);
    const double sig = std::sqrt(svi_w(a, b, rho, m, sigma, k) / T);
    for (int side_i = 0; side_i < 2; ++side_i) {
      const Side side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const std::size_t idx =
          chain_index(static_cast<std::uint16_t>(s), side);
      const double price = black76_price(F, K, T, sig, df, side);
      c.bids[idx] = price * 0.999;
      c.asks[idx] = price * 1.001;
      c.mids[idx] = price;
    }
  }

  under.chains.push_back(std::move(c));
  return under;
}

[[nodiscard]] CurveSet make_flat_curve(double F, double T,
                                       std::int64_t expiry_ns) {
  CurveSet cs{};
  cs.spot = F;
  std::vector<ForwardPoint> pts(1);
  pts[0].expiry_ns = expiry_ns;
  pts[0].T = T;
  pts[0].F = F;
  cs.forward.set(pts);
  // Yield left default (empty) => disc(T) == 1.0, matching df = 1 pricing.
  return cs;
}

// Permissive filter opts so every near-ATM OTM leg survives the quote cascade.
[[nodiscard]] CalibOpts permissive_opts() {
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0e9;
  opts.min_vega_weight = 0.0;
  opts.max_spread_to_mid_pct = 0.0;  // disable the spread-to-mid filter
  return opts;
}

}  // namespace

TEST(SviCalibSurface, FitsSyntheticSlice_LowRmse) {
  const double a = 0.012;
  const double b = 0.06;
  const double rho = -0.40;
  const double m = -0.02;
  const double sigma = 0.18;
  const double T = 0.5;
  const std::int64_t expiry_ns = 1'700'000'000'000'000'000LL;

  const Underlying under = make_svi_underlying(a, b, rho, m, sigma, T, expiry_ns);
  const CurveSet cs = make_flat_curve(100.0, T, expiry_ns);

  auto surf_res = VolSurface::create(1u, Parametrization::Svi, 4);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surf = std::move(surf_res).value();

  FitDiag diag{};
  const auto rc = svi_calib_surface(surf, under, cs, permissive_opts(), &diag);
  ASSERT_TRUE(rc.has_value());
  ASSERT_EQ(surf.n_slices(), 1u);

  const SviParams fit = surf.svi_slices()[0];
  EXPECT_GT(diag.n_quotes_used, 0u);
  const double max_rel = max_rel_w_error(fit, a, b, rho, m, sigma, -25, 25);
  EXPECT_LT(max_rel, 0.02);
}

TEST(SviCalibSurface, WrongParametrization_ReturnsInvalidArgument) {
  const std::int64_t expiry_ns = 1'700'000'000'000'000'000LL;
  const Underlying under =
      make_svi_underlying(0.012, 0.06, -0.4, -0.02, 0.18, 0.5, expiry_ns);
  const CurveSet cs = make_flat_curve(100.0, 0.5, expiry_ns);

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, 4);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surf = std::move(surf_res).value();

  const auto rc = svi_calib_surface(surf, under, cs, permissive_opts());
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::InvalidArgument);
}

TEST(SviMmCalibSurface, FitsSyntheticSlice_Admissible) {
  const double a = 0.012;
  const double b = 0.06;
  const double rho = -0.40;
  const double m = -0.02;
  const double sigma = 0.18;
  const double T = 0.5;
  const std::int64_t expiry_ns = 1'700'000'000'000'000'000LL;

  const Underlying under = make_svi_underlying(a, b, rho, m, sigma, T, expiry_ns);
  const CurveSet cs = make_flat_curve(100.0, T, expiry_ns);

  auto surf_res = VolSurface::create(1u, Parametrization::SviMm, 4);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surf = std::move(surf_res).value();

  CalibOpts opts = permissive_opts();
  opts.morozov_stop = false;

  const auto rc = svi_mm_calib_surface(surf, under, cs, opts);
  ASSERT_TRUE(rc.has_value());
  ASSERT_EQ(surf.n_slices(), 1u);

  const SviParams fit = surf.svi_slices()[0];
  const auto adm = arb_check_butterfly_svi_mm(fit, fit.T);
  EXPECT_EQ(adm.n_violations, 0u);
}

}  // namespace
