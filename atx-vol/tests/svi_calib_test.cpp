#include <gtest/gtest.h>

#include <algorithm>
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

// ── q90 selection: nth_element must reproduce the old partial-sort ────────
//
// The SVI-MM IRLS Huber threshold is the q90 of half-spread-normalized price
// residuals. C2.2 replaces an O(n^2) partial selection sort with
// std::nth_element on the same scratch — the q90 VALUE must be identical
// (identical downstream weights). Both algorithms are replicated here (the
// selection sort copied verbatim from the pre-C2.2 code) as the oracle, since
// the production routine is file-local. Exercised with duplicates and (pre-abs)
// negative residuals across several n so 0.90*n lands on varied indices.

// Pre-C2.2 partial selection sort up to q_idx, returning rnorm[q_idx].
[[nodiscard]] double q90_selection_sort(std::vector<double> rnorm) {
  const std::size_t n = rnorm.size();
  std::size_t q_idx = static_cast<std::size_t>(0.90 * static_cast<double>(n));
  if (q_idx >= n) {
    q_idx = n - 1;
  }
  for (std::size_t i = 0; i <= q_idx && i < n; ++i) {
    std::size_t mn = i;
    for (std::size_t j = i + 1; j < n; ++j) {
      if (rnorm[j] < rnorm[mn]) {
        mn = j;
      }
    }
    std::swap(rnorm[i], rnorm[mn]);
  }
  return rnorm[q_idx];
}

// C2.2 replacement: std::nth_element to the same q_idx, returning rnorm[q_idx].
[[nodiscard]] double q90_nth_element(std::vector<double> rnorm) {
  const std::size_t n = rnorm.size();
  std::size_t q_idx = static_cast<std::size_t>(0.90 * static_cast<double>(n));
  if (q_idx >= n) {
    q_idx = n - 1;
  }
  std::nth_element(rnorm.begin(),
                   rnorm.begin() + static_cast<std::ptrdiff_t>(q_idx),
                   rnorm.end());
  return rnorm[q_idx];
}

TEST(SviCalib, NthElementQ90MatchesSelectionSort) {
  // Raw residuals (with negatives + duplicates) and matching half-spreads; the
  // production code forms rnorm[i] = |resid[i]| / hs[i] before selecting q90.
  const std::vector<std::vector<double>> resid_cases = {
      {-3.0, 1.0, -1.0, 2.0, 0.5, 0.5, -0.5, 4.0, -2.0, 2.0},        // n=10, dup 0.5/2.0
      {0.0, -0.0, 1.5, -1.5, 1.5, 3.0, -3.0, 0.25, 0.25, 0.25, 9.0}, // n=11, dup 1.5/0.25
      {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0},                    // n=7, all-equal-abs
      {5.0, -4.0, 3.0, -2.0, 1.0, 0.0, -1.0, 2.0, -3.0, 4.0, -5.0, 6.0, -7.0}, // n=13
      {2.0},                                                         // n=1 edge
  };
  for (const std::vector<double>& resid : resid_cases) {
    // hs alternates to make normalization non-trivial (still all-positive rnorm).
    std::vector<double> rnorm(resid.size(), 0.0);
    for (std::size_t i = 0; i < resid.size(); ++i) {
      const double hs = (i % 2 == 0) ? 0.5 : 0.25;
      rnorm[i] = std::fabs(resid[i]) / hs;
    }
    EXPECT_EQ(q90_selection_sort(rnorm), q90_nth_element(rnorm))
        << "n=" << rnorm.size();
  }
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

// FT-P (B6b): the SVI-MM LM's solve_spd_dense per-call MatX(5,5)/VecX(5)
// allocation is replaced with reused thread_local buffers (bit-identical solve —
// no numerical change). Pin the fitted params on a fixed noiseless fixture so any
// accidental numeric drift is caught bit-for-bit.
TEST(SviMmCalib, FixedFixtureFit_IsBitIdentical) {
  const double T = 0.5;
  const std::vector<FitObs> obs =
      build_obs_from_svi(41, -0.40, 0.40, 0.012, 0.060, -0.30, -0.02, 0.18, T);
  CalibOpts opts = calib_default_opts();
  opts.morozov_stop = false;
  opts.wing_floor_alpha = 0.0;
  const auto res = svi_mm_fit_slice(std::span<const FitObs>(obs), T, 100.0, opts);
  ASSERT_TRUE(res.has_value());
  const SviParams f = res.value();
  // Goldens re-captured for plan item 2.5 (Black-76 put leg switched from the
  // 1−Φ(d) complement to Φ(−d)). 20 of these 41 strikes are puts, and the LM's
  // residual/Jacobian pass prices them with `black76_value_and_vega`, so the
  // per-observation residuals move by an ulp and the search settles on a
  // marginally different point of the same FP-noise floor. The shift is ~2e-15
  // relative — three orders below the fitter's own tol_param — and neither
  // vector is nearer the noiseless generating params (0.012, 0.06, -0.30,
  // -0.02, 0.18) than the other: both sit ~1e-11 away. This pin exists to catch
  // ACCIDENTAL drift from refactors that claim to be numerically neutral, so a
  // deliberate numerical change re-baselines it rather than failing it.
  // Previous: 0.01200000000168963, 0.059999999997054999, -0.30000000000922628,
  //           -0.019999999998334177, 0.17999999997839855.
  EXPECT_EQ(f.a, 0.012000000001691179);
  EXPECT_EQ(f.b, 0.059999999997051592);
  EXPECT_EQ(f.rho, -0.30000000000921206);
  EXPECT_EQ(f.m, -0.019999999998325333);
  EXPECT_EQ(f.sigma, 0.17999999997837973);
}

// Plan item 1.8(a) — a candidate parameter step whose model prices are not
// finite must poison the SSE, not have those rows silently dropped.
//
// `svi_mm_sse` skipped every observation whose Black-76 price came back
// non-finite. A Black-76 price is non-finite only when sigma_pred is — i.e. only
// when the candidate's total variance has overflowed — so the rows that got
// skipped were exactly the evidence that the candidate is garbage. With ALL rows
// skipped the accumulator stays at its 0.0 seed, the diverging step scores a
// perfect SSE of zero, and the LM accepts it.
//
// An SSE that drops rows is also not comparable across candidates: two
// candidates that drop different rows are being scored on different data sets.
//
// Fixture: a clean synthetic price-domain smile with one overflowing quote mid.
// The weighted normal-equation gradient J^T W r then overflows to -inf, the
// damped solve returns a non-finite step, and the projected iterate has
// non-finite total variance.
TEST(SviMmCalib, DivergingStepWithNonFinitePrices_IsRejected) {
  const double T = 0.5;
  std::vector<FitObs> obs =
      build_obs_from_svi(41, -0.40, 0.40, 0.012, 0.060, -0.30, -0.02, 0.18, T);
  obs[20].mid = 1.0e306;  // overflows J^T W r -> non-finite LM step

  CalibOpts opts = calib_default_opts();
  opts.morozov_stop = false;
  opts.wing_floor_alpha = 0.0;

  const auto res = svi_mm_fit_slice(std::span<const FitObs>(obs), T, 100.0, opts);
  ASSERT_TRUE(res.has_value());
  const SviParams f = res.value();

  // The diverging step must have been rejected and the fit left on a finite
  // candidate (here the quasi-explicit seed).
  EXPECT_TRUE(std::isfinite(f.a)) << "a = " << f.a;
  EXPECT_TRUE(std::isfinite(f.b)) << "b = " << f.b;
  EXPECT_TRUE(std::isfinite(f.rho)) << "rho = " << f.rho;
  EXPECT_TRUE(std::isfinite(f.m)) << "m = " << f.m;
  EXPECT_TRUE(std::isfinite(f.sigma)) << "sigma = " << f.sigma;
  // ...and it must still price to finite, strictly positive total variance.
  for (int i = -40; i <= 40; ++i) {
    const double k = 0.01 * static_cast<double>(i);
    const double w = svi_w(f.a, f.b, f.rho, f.m, f.sigma, k);
    ASSERT_TRUE(std::isfinite(w)) << "w(" << k << ") = " << w;
    EXPECT_GT(w, 0.0) << "k = " << k;
  }
}

// Plan item 1.8(b) — a rank-deficient (non positive-definite) quasi-explicit
// normal matrix must not be laundered into an Ok(a = b = rho = 0) slice.
//
// `build_and_solve_normal` zeroed the free coordinates when the 3x3 Cholesky
// reported a non-PD matrix, and `svi_blls_inner` then recomputed a perfectly
// ordinary finite SSE at that all-zero point. Every Nelder-Mead vertex scored
// the same way, so the degenerate point won the search and `svi_fit_slice`
// returned Ok with a = b = rho = 0 — a slice with identically zero total
// variance.
//
// Fixture: every quote on the same strike. The design rows (1, u_i, v_i) are
// then identical, so the weighted normal matrix is rank 1 at every (m, sigma).
[[nodiscard]] std::vector<FitObs> build_single_strike_obs(std::size_t n, double k,
                                                          double w, double T) {
  std::vector<FitObs> obs(n);
  const double F = 100.0;
  const double K = F * std::exp(k);
  const double sig = std::sqrt(w / T);
  const Side side = (K >= F) ? Side::Call : Side::Put;
  for (FitObs& o : obs) {
    o.k = k;
    o.sigma_mkt = sig;
    o.w_mkt = w;
    o.K = K;
    o.F = F;
    o.df = 1.0;
    o.mid = black76_price(F, K, T, sig, 1.0, side);
    o.spread = 0.01 * o.mid;
    o.vega = black76_value_and_vega(F, K, T, sig, 1.0, side).vega;
    o.side = side;
    o.weight_w = 1.0;
    o.active_weight_w = 1.0;
  }
  return obs;
}

TEST(SviCalib, RankDeficientNormalMatrix_DoesNotReturnDegenerateSlice) {
  const double T = 0.5;
  // One usable quote: the design rows cannot determine three linear
  // coefficients, so the weighted 3x3 normal matrix is rank 1 at every
  // (m, sigma) the Nelder-Mead visits.
  const std::vector<FitObs> obs = build_single_strike_obs(1, 0.05, 0.02, T);

  const auto res =
      svi_fit_slice(std::span<const FitObs>(obs), T, 100.0, calib_default_opts());
  if (res.has_value()) {
    const SviParams f = res.value();
    ADD_FAILURE() << "returned Ok on a rank-deficient fit: a=" << f.a
                  << " b=" << f.b << " rho=" << f.rho << " m=" << f.m
                  << " sigma=" << f.sigma;
  } else {
    EXPECT_EQ(res.error().code(), ErrorCode::Unavailable);
  }

  // Same defect via zeroed weights: every design row drops out of the
  // accumulation, so the normal matrix is identically zero.
  std::vector<FitObs> zero_weight =
      build_obs_from_svi(11, -0.20, 0.20, 0.012, 0.060, -0.30, -0.02, 0.18, T);
  for (FitObs& o : zero_weight) {
    o.weight_w = 0.0;
    o.active_weight_w = 0.0;
  }
  const auto zres = svi_fit_slice(std::span<const FitObs>(zero_weight), T, 100.0,
                                  calib_default_opts());
  if (zres.has_value()) {
    const SviParams f = zres.value();
    ADD_FAILURE() << "returned Ok on a zero normal matrix: a=" << f.a
                  << " b=" << f.b << " rho=" << f.rho;
  }
}

// ...and the SVI-MM fitter, which seeds from `svi_fit_slice`, must fall back to
// its hardcoded seed rather than start the LM from the degenerate a=b=rho=0
// point (which the admissibility projector then pins at the b = 1e-8 edge).
TEST(SviMmCalib, RankDeficientSeed_FallsBackToHealthySeed) {
  const double T = 0.5;
  const std::vector<FitObs> obs = build_single_strike_obs(1, 0.05, 0.02, T);

  const auto res = svi_mm_fit_slice(std::span<const FitObs>(obs), T, 100.0,
                                    calib_default_opts());
  ASSERT_TRUE(res.has_value());
  const SviParams f = res.value();

  // The healthy fallback seed is b = 0.1 / rho = -0.20 / sigma = 0.10; the
  // degenerate a=b=rho=0 seed collapses b onto the projector's 1e-8 edge pad,
  // leaving a flat slice with no smile at all.
  EXPECT_GT(f.b, 1.0e-4) << "b collapsed onto the admissibility edge pad";
  EXPECT_GT(svi_w(f.a, f.b, f.rho, f.m, f.sigma, obs[0].k), 0.0);
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

// FT-C5 (B5a): the symmetric-smile branch (beta ~ 0) of svi_jw_to_raw computed
// sigma = (v - v_min)*T / (...); with v == v_min the numerator is zero so
// sigma == 0 was returned Ok. A sigma=0 raw-SVI slice has a total-variance kink
// at k=m (infinite density). The asymmetric branch already rejects sigma <= 0;
// the symmetric branch must too.
TEST(SviJw, JwToRaw_SymmetricVEqualsVMin_RejectsZeroSigma) {
  SviJwParams jw{};
  jw.v = 0.05;
  jw.v_min = 0.05;  // v_min == v => numerator zero => sigma == 0
  jw.psi = 0.0;     // beta = 0 => symmetric branch
  jw.p = 0.20;
  jw.c = 0.20;      // p == c => rho = 0
  jw.T = 0.5;
  const auto res = svi_jw_to_raw(jw);
  ASSERT_FALSE(res.has_value())
      << "symmetric-branch sigma=0 must be rejected (infinite-density kink)";
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

// Build a synthetic single-expiry chain over a WIDE log-moneyness range whose
// OTM legs price a known raw-SVI slice at F = 100, df = 1. Same construction as
// make_svi_underlying but with a caller-chosen k-range and strike count so a
// steep-wing (Lee-violating) truth is well sampled on both wings.
[[nodiscard]] Underlying make_svi_underlying_range(double a, double b, double rho,
                                                   double m, double sigma,
                                                   double T, double k_lo,
                                                   double k_hi, int n,
                                                   std::int64_t expiry_ns) {
  constexpr double F = 100.0;
  constexpr double df = 1.0;
  Underlying under{};
  under.uid = 1u;
  Chain c{};
  c.uid = 1u;
  c.expiry_id = 0u;
  c.expiry_ns = expiry_ns;
  c.T = T;
  for (int i = 0; i < n; ++i) {
    const double k =
        k_lo + (k_hi - k_lo) * static_cast<double>(i) / static_cast<double>(n - 1);
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
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(s), side);
      const double price = black76_price(F, K, T, sig, df, side);
      c.bids[idx] = price * 0.999;
      c.asks[idx] = price * 1.001;
      c.mids[idx] = price;
    }
  }
  under.chains.push_back(std::move(c));
  return under;
}

// C-1 regression (WS-C): a butterfly-arbitrage SVI smile must NEVER be served by
// the surface driver. A caller can build a Parametrization::Svi VolSurface via
// this driver and serve it DIRECTLY (VolSurface::w -> svi_total_w), bypassing
// the fit_slice_curve butterfly gate. The driver must therefore repair-or-drop a
// violating slice at the source, not merely tally it.
TEST(SviCalibSurface, ButterflyInadmissibleFit_IsRepairedOrDropped_NeverServed) {
  // Steep-wing truth well past the Lee wing-slope bound: b*(1+|rho|) = 6*1.4 =
  // 8.4 >> 4/T = 4. The quasi-explicit raw-SVI fit reproduces the steepness and
  // lands OUTSIDE the Martini-Mingone polytope.
  const double a = 0.04;
  const double b = 6.0;
  const double rho = -0.40;
  const double m = 0.0;
  const double sigma = 0.06;
  const double T = 1.0;
  const std::int64_t expiry_ns = 1'700'000'000'000'000'000LL;

  // Disable the post-fit sigma clamp so the steep slice reaches the butterfly
  // gate rather than being dropped for wing vol.
  CalibOpts opts = permissive_opts();
  opts.max_post_fit_sigma = 0.0;

  // (1) Non-vacuity: the RAW per-slice fitter really does produce a butterfly-arb
  //     slice for this data — so the driver's gate has something to catch.
  const std::vector<FitObs> obs =
      build_obs_from_svi(41, -0.55, 0.55, a, b, rho, m, sigma, T);
  FitDiag raw_diag{};
  const auto raw = svi_fit_slice(std::span<const FitObs>(obs), T, 100.0, opts,
                                 &raw_diag);
  ASSERT_TRUE(raw.has_value());
  EXPECT_GT(arb_check_butterfly_svi_mm(raw.value(), T).n_violations, 0u)
      << "fixture no longer induces a raw butterfly violation; make the wing "
         "steeper";

  // (2) The surface driver serves NOTHING arbitrageable, whether it repaired the
  //     slice onto the polytope or dropped it.
  const Underlying under =
      make_svi_underlying_range(a, b, rho, m, sigma, T, -0.55, 0.55, 41, expiry_ns);
  const CurveSet cs = make_flat_curve(100.0, T, expiry_ns);
  auto surf_res = VolSurface::create(1u, Parametrization::Svi, 4);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surf = std::move(surf_res).value();

  FitDiag diag{};
  const auto rc = svi_calib_surface(surf, under, cs, opts, &diag);
  (void)rc;  // Ok (repaired+served) or NotFound (dropped) — both are arb-free.
  for (std::size_t i = 0; i < surf.n_slices(); ++i) {
    const SviParams served = surf.svi_slices()[i];
    EXPECT_EQ(arb_check_butterfly_svi_mm(served, served.T).n_violations, 0u)
        << "served SVI slice " << i << " is butterfly-inadmissible";
  }
  // The gate was actually exercised: the raw fit's pre-repair violation was
  // recorded before the driver repaired/dropped it.
  EXPECT_GT(diag.n_butterfly_viol, 0u);
}

// C-2 sanity (WS-C): with the quasi-explicit Nelder-Mead simplex budget restored
// to the C-parity 200 (was collapsing to max_inner_iter = 12), a wide, skewed
// smile fits to depth. Loose bound — the exact RMSE delta vs the 12-cap is
// reported in the sprint notes, not pinned here (it would be brittle).
TEST(SviCalib, WideSkewedSmile_FitsToDepth) {
  const double a = 0.02;
  const double b = 0.30;
  const double rho = -0.60;  // strong skew
  const double m = 0.0;
  const double sigma = 0.50;  // wide
  const double T = 1.0;

  const std::vector<FitObs> obs =
      build_uniform_obs(61, -1.00, 1.00, a, b, rho, m, sigma, T);
  FitDiag diag{};
  const auto res =
      svi_fit_slice(std::span<const FitObs>(obs), T, 100.0, calib_default_opts(),
                    &diag);
  ASSERT_TRUE(res.has_value());
  const double max_rel = max_rel_w_error(res.value(), a, b, rho, m, sigma, -80, 80);
  // At the old max_inner_iter=12 cap this wide/skewed smile under-fits to
  // max_rel_w ~ 3.2e-4 (vega-wtd vol RMSE ~ 3.5e-5); at the restored 200-move
  // C-parity budget it fits to ~4e-9 / ~5e-10 — well within this bound.
  EXPECT_LT(max_rel, 0.02);
}

// FT-C1 (B1): the quasi-explicit Nelder-Mead must return a BOX-CONSISTENT vertex.
// A short-dated, sharply kinked smile drives the (m, sigma) optimum onto the
// sigma_min = 1e-3 bound. `nm_eval` clamps (m, sigma) BY VALUE before the inner
// BLLS solve, but the pre-fix `nm_search` wrote the RAW (unclamped) best vertex
// back, so a reflection/expansion step that pushed sigma < 0 could win while its
// recorded SSE belonged to the clamped sigma=1e-3 point. The (u,v)->(a,b,rho) map
// then paired the clamped linear solution with an out-of-box sigma:
// b_fit = c_raw / sigma_cur flips sign / blows up, and the downstream butterfly
// gate launders the b<=0 slice into a near-flat one served Ok. Assert the returned
// slice is in-box (sigma >= sigma_min, b > 0) and genuinely non-flat (fits the
// kink far better than the best constant-variance slice).
TEST(SviCalib, KinkedShortDatedSmile_ReturnsBoxConsistentNonFlatSlice) {
  // Truth sigma (3e-4) is far BELOW the fitter's sigma_min = 1e-3, so the optimum
  // saturates the bound — the exact FT-C1 trigger.
  const double a = 5.0e-4;
  const double b = 0.12;
  const double rho = -0.30;
  const double m = 0.0;
  const double sigma_true = 3.0e-4;
  const double T = 0.019;  // ~1 week

  const std::vector<FitObs> obs =
      build_uniform_obs(41, -0.30, 0.30, a, b, rho, m, sigma_true, T);
  FitDiag diag{};
  const auto res =
      svi_fit_slice(std::span<const FitObs>(obs), T, 100.0, permissive_opts(), &diag);
  ASSERT_TRUE(res.has_value());
  const SviParams fit = res.value();

  // (1) Box consistency: sigma must not have escaped the [sigma_min, sigma_max]
  //     box, and b must keep its sign (b = c_raw / sigma; a negative sigma flips
  //     it). sigma_min in the fitter is 1e-3.
  EXPECT_GE(fit.sigma, 1.0e-3 - 1.0e-9)
      << "returned sigma out of box (FT-C1 negative/under-min vertex won): sigma="
      << fit.sigma << " b=" << fit.b;
  EXPECT_GT(fit.b, 0.0)
      << "returned b non-positive (sign flipped by out-of-box sigma): b=" << fit.b;

  // (2) Non-flat: the fitted slice must track the kink far better than the best
  //     constant-variance ("flat") slice. Flat baseline = weighted-mean total
  //     variance, b = 0.
  double sw = 0.0;
  double sww = 0.0;
  for (const FitObs& o : obs) {
    sw += o.weight_w;
    sww += o.weight_w * o.w_mkt;
  }
  const double w_flat = sww / sw;
  double rmse_flat = 0.0;
  double rmse_fit = 0.0;
  for (const FitObs& o : obs) {
    const double sig_flat = std::sqrt(std::max(w_flat, 1.0e-12) / T);
    const double w_fit = svi_w(fit.a, fit.b, fit.rho, fit.m, fit.sigma, o.k);
    const double sig_fit = std::sqrt(std::max(w_fit, 1.0e-12) / T);
    rmse_flat += o.weight_w * (sig_flat - o.sigma_mkt) * (sig_flat - o.sigma_mkt);
    rmse_fit += o.weight_w * (sig_fit - o.sigma_mkt) * (sig_fit - o.sigma_mkt);
  }
  rmse_flat = std::sqrt(rmse_flat / sw);
  rmse_fit = std::sqrt(rmse_fit / sw);
  EXPECT_LT(rmse_fit, 0.5 * rmse_flat)
      << "fitted slice is ~flat (FT-C1 launder): rmse_fit=" << rmse_fit
      << " rmse_flat=" << rmse_flat << " b=" << fit.b;
}

// FT-C3 (B3b): the SVI-MM "Lee bound" used b*(1+|rho|) <= 4/T with w = TOTAL
// variance. The correct total-variance wing-slope bound is T-free: b*(1+|rho|)
// <= 4. At short T the 4/T form is huge (T=1wk -> 4/T ~ 208), so a
// moment-exploding wing (b*(1+|rho|)=100) passed the "Lee" gate untouched. Both
// the admissibility check and the projector must use the T-free bound.
TEST(SviMmCalib, ShortDatedSteepWing_LeeBoundIsTFree) {
  const double T = 1.0 / 52.0;  // ~1 week
  // (1) The admissibility CHECK must flag it. Today 4/T ~ 208 >= 100 => 0 viol.
  {
    SviParams s{};
    s.a = 0.01; s.b = 100.0; s.rho = 0.0; s.m = 0.0; s.sigma = 0.05; s.T = T;
    EXPECT_GT(arb_check_butterfly_svi_mm(s, T).n_violations, 0u)
        << "moment-exploding short-dated wing must be flagged (T-free Lee bound)";
  }
  // (2) The PROJECTOR must pull the wing back under the T-free bound. Today it
  //     leaves b = 100 (4/T bound never binds), so nothing moves on the Lee axis.
  {
    SviParams s{};
    s.a = 0.01; s.b = 100.0; s.rho = 0.0; s.m = 0.0; s.sigma = 0.05; s.T = T;
    const bool moved = atx::vol::svi_project_mm(s, T);
    EXPECT_TRUE(moved) << "steep short-dated wing must be projected";
    EXPECT_LE(s.b * (1.0 + std::fabs(s.rho)), 4.0 + 1.0e-6)
        << "projected wing slope still exceeds the T-free Lee bound: "
        << s.b * (1.0 + std::fabs(s.rho));
    EXPECT_EQ(arb_check_butterfly_svi_mm(s, T).n_violations, 0u)
        << "projected slice must be admissible under the T-free bound";
  }
}

}  // namespace
