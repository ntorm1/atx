#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/black76.hpp"       // black76_price, black76_value_and_vega
#include "atx/vol/c8.hpp"           // C8Params, c8_slice_w
#include "atx/vol/c8_calib.hpp"     // c8_fit_slice_lm, c8_calib_slice
#include "atx/vol/calib.hpp"        // CalibOpts, calib_default_opts, FitObs
#include "atx/vol/correction.hpp"   // CorrectionCache
#include "atx/vol/curve.hpp"        // CurveSet, ForwardPoint
#include "atx/vol/essvi_calib.hpp"  // essvi_fit_slice, essvi_calib_surface_sequential
#include "atx/vol/svi_calib.hpp"    // svi_calib_surface, svi_mm_calib_surface
#include "atx/vol/universe.hpp"     // Underlying, Chain, chain_index
#include "atx/vol/vol_surface.hpp"  // VolSurface, EssviParams, SviParams

// Robustness-hardening regression coverage (sprint P0-3 / P0-4). Each test
// engineers a degenerate / sparse input at one hardened site and pins the
// post-fix invariant:
//   P0-3a  eSSVI sequential theta-band collapse stays finite & positive;
//   P0-3b  converged raw-SVI / SVI-MM slice has strictly positive variance;
//   P0-3c  C8 slice fit with a rank-deficient (gradient-blind) seed is REJECTED
//          instead of silently returning a garbage fit;
//   P0-4   populated CorrectionCache eval is finite & deterministic (the bounded
//          scratch init changes no result).

namespace {

using atx::vol::black76_price;
using atx::vol::C8Params;
using atx::vol::c8_calib_slice;
using atx::vol::c8_fit_slice_lm;
using atx::vol::c8_slice_w;
using atx::vol::calib_default_opts;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CorrectionCache;
using atx::vol::CurveSet;
using atx::vol::essvi_backbone_w;
using atx::vol::essvi_fit_slice;
using atx::vol::EssviParams;
using atx::vol::FitDiag;
using atx::vol::FitObs;
using atx::vol::ForwardPoint;
using atx::vol::Parametrization;
using atx::vol::Side;
using atx::vol::svi_calib_surface;
using atx::vol::svi_mm_calib_surface;
using atx::vol::SviParams;
using atx::vol::Underlying;
using atx::vol::VolSurface;

// ── Shared builders ───────────────────────────────────────────────────────

// Permissive quote-filter opts so every near-ATM OTM leg survives the cascade.
[[nodiscard]] CalibOpts permissive_opts() {
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0e9;
  opts.min_vega_weight = 0.0;
  opts.max_spread_to_mid_pct = 0.0;  // disable the spread-to-mid filter
  opts.min_obs_per_slice = 4;
  return opts;
}

// ─────────────────────────────────────────────────────────────────────────
// P0-3a — eSSVI sequential theta-band inversion.
//
// Chains are fed OUT of T-order (large-T high-vol slice first) so the first
// slice's theta_floor exceeds the SECOND slice's theta_hi band (= 25*T). The
// sequential driver then collapses the second slice's [theta_lo, theta_hi] band;
// the guard must keep it non-inverted so the fit stays finite with positive
// fitted variance (rather than producing negative / garbage total variance).
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] std::vector<FitObs> make_essvi_obs(double theta, double phi,
                                                double rho, double T, double F) {
  EssviParams tr{};
  tr.theta = theta;
  tr.phi = phi;
  tr.rho = rho;
  tr.T = T;
  std::vector<FitObs> obs;
  const int n = 21;
  for (int i = 0; i < n; ++i) {
    const double k =
        -0.30 + 0.60 * static_cast<double>(i) / static_cast<double>(n - 1);
    const double w = essvi_backbone_w(tr, k);
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
  }
  return obs;
}

// Direct reproduction of the sequential seam: the SECOND slice is fit with a
// theta_floor equal to the FIRST slice's theta. Here the floor (aggressively)
// exceeds the slice's own theta_hi band (= kSigmaHigh^2 * T = 25*T), so the
// [theta_lo, theta_hi] cube band collapses. The hardened guard must keep the
// band strictly non-inverted (theta_lo < theta_hi) so the fit stays finite with
// positive fitted variance instead of producing negative / garbage variance.
TEST(CalibRobustness, EssviThetaFloorAboveBand_StaysFiniteAndPositive) {
  constexpr double F = 100.0;
  constexpr double T = 0.10;
  const std::vector<FitObs> obs =
      make_essvi_obs(/*theta=*/0.02, /*phi=*/1.5, /*rho=*/-0.30, T, F);

  const double theta_hi = 25.0 * T;  // kSigmaHigh^2 * T, kSigmaHigh = 5.0
  // Prior-slice theta far above this slice's whole natural theta band.
  const double aggressive_floor = 100.0 * theta_hi;

  FitDiag diag{};
  const auto res = essvi_fit_slice(obs, T, F, calib_default_opts(), &diag,
                                   aggressive_floor, /*warm=*/nullptr);
  ASSERT_TRUE(res.has_value());
  const EssviParams& fit = *res;

  EXPECT_TRUE(std::isfinite(fit.theta));
  EXPECT_GT(fit.theta, 0.0);
  EXPECT_LE(fit.theta, theta_hi + 1.0e-9);  // pinned into the collapsed band

  for (int i = -20; i <= 20; ++i) {
    const double k = 0.02 * static_cast<double>(i);
    const double w = essvi_backbone_w(fit, k);
    EXPECT_TRUE(std::isfinite(w));
    EXPECT_GT(w, 0.0) << "collapsed theta band must not yield negative variance";
  }
}

// ─────────────────────────────────────────────────────────────────────────
// P0-3b — SVI converged-slice positivity.
//
// A deep, strongly-skewed synthetic smile whose least-squares optimum sits near
// zero ATM variance. The constrained fitters (quasi-explicit non-negativity box;
// SVI-MM admissibility projection) keep the CONVERGED slice's variance strictly
// positive everywhere; the post-fit positivity gate pins that invariant.
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] double svi_w(double a, double b, double rho, double m,
                           double sigma, double k) {
  const double dk = k - m;
  return a + b * (rho * dk + std::sqrt(dk * dk + sigma * sigma));
}

[[nodiscard]] Underlying make_svi_underlying(double a, double b, double rho,
                                             double m, double sigma, double T,
                                             std::int64_t expiry_ns) {
  constexpr double F = 100.0;
  constexpr double df = 1.0;
  constexpr int n = 25;

  Underlying under{};
  under.uid = 1u;
  under.spot = F;

  Chain c{};
  c.uid = 1u;
  c.expiry_id = 0u;
  c.expiry_ns = expiry_ns;
  c.T = T;
  for (int i = 0; i < n; ++i) {
    const double k =
        -0.30 + 0.60 * static_cast<double>(i) / static_cast<double>(n - 1);
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
      const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
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

[[nodiscard]] CurveSet make_flat_curve(double F, double T,
                                       std::int64_t expiry_ns) {
  CurveSet cs{};
  cs.spot = F;
  std::vector<ForwardPoint> pts(1);
  pts[0].expiry_ns = expiry_ns;
  pts[0].T = T;
  pts[0].F = F;
  cs.forward.set(pts);
  return cs;
}

void expect_slice_variance_positive(const SviParams& s) {
  const double disc = 1.0 - s.rho * s.rho;
  const double w_min = s.a + s.b * s.sigma * std::sqrt(disc > 0.0 ? disc : 0.0);
  EXPECT_TRUE(std::isfinite(w_min));
  EXPECT_GT(w_min, 0.0) << "global-min total variance must be positive";
  for (int i = -60; i <= 60; ++i) {
    const double k = 0.01 * static_cast<double>(i);
    const double w = svi_w(s.a, s.b, s.rho, s.m, s.sigma, k);
    EXPECT_TRUE(std::isfinite(w));
    EXPECT_GT(w, 0.0);
  }
}

TEST(CalibRobustness, SviSurface_DeepSkew_ConvergedVarianceStaysPositive) {
  // Deep, steeply skewed smile with a tiny ATM variance floor.
  const double a = 0.004;
  const double b = 0.45;
  const double rho = -0.88;
  const double m = 0.06;
  const double sigma = 0.05;
  const double T = 0.10;
  const std::int64_t expiry_ns = 1'700'000'000'000'000'000LL;

  const Underlying under = make_svi_underlying(a, b, rho, m, sigma, T, expiry_ns);
  const CurveSet cs = make_flat_curve(100.0, T, expiry_ns);

  auto surf_res = VolSurface::create(1u, Parametrization::Svi, 4);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surf = std::move(surf_res).value();

  const auto rc = svi_calib_surface(surf, under, cs, permissive_opts());
  ASSERT_TRUE(rc.has_value());
  ASSERT_EQ(surf.n_slices(), 1u);
  expect_slice_variance_positive(surf.svi_slices()[0]);
}

TEST(CalibRobustness, SviMmSurface_DeepSkew_ConvergedVarianceStaysPositive) {
  const double a = 0.004;
  const double b = 0.45;
  const double rho = -0.88;
  const double m = 0.06;
  const double sigma = 0.05;
  const double T = 0.10;
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
  expect_slice_variance_positive(surf.svi_slices()[0]);
}

// ─────────────────────────────────────────────────────────────────────────
// P0-3c — C8 rank-deficient (gradient-blind) slice rejection.
//
// A seed with v_min == v places the slice on the JW-admissibility boundary: the
// central-difference Jacobian's +h perturbation of v_min steps to v_min > v,
// where c8_jw_to_raw fails, so c8_slice_grad_w returns nullopt at EVERY strike.
// The 8x8 normal system is then all-zero (rank-deficient); the LM cannot fit the
// 8 DoF. The hardened driver must REJECT the fit rather than return a silent
// garbage success.
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] C8Params mk_slice(double T, double v, double psi, double p,
                                double c, double vmin, double kappa, double qL,
                                double qR) {
  C8Params s{};
  s.T = T;
  s.F = 100.0;
  s.v = v;
  s.psi = psi;
  s.p = p;
  s.c = c;
  s.v_min = vmin;
  s.kappa = kappa;
  s.q_L = qL;
  s.q_R = qR;
  const double scale = std::sqrt(v);
  s.h_atm = scale;
  s.k_L = -2.5 * scale;
  s.h_L = scale;
  s.k_R = 2.5 * scale;
  s.h_R = scale;
  s.bumps_active = true;
  return s;
}

TEST(CalibRobustness, C8FitSliceLm_GradientRankDeficientSeed_IsRejected) {
  // Target variances from an admissible truth (so mid is finite / non-trivial).
  const C8Params truth =
      mk_slice(0.25, 0.045, -0.02, 0.45, 0.40, 0.030, 0.0, 0.0, 0.0);
  constexpr int N = 11;
  std::array<double, N> k{};
  std::array<double, N> mid{};
  std::array<double, N> spread{};
  for (int i = 0; i < N; ++i) {
    k[static_cast<std::size_t>(i)] = -0.25 + 0.05 * static_cast<double>(i);
    mid[static_cast<std::size_t>(i)] =
        c8_slice_w(truth, k[static_cast<std::size_t>(i)]);
    spread[static_cast<std::size_t>(i)] = 0.0005;
  }

  // Degenerate seed: v_min == v places every gradient FD step across the
  // admissibility boundary -> all gradients fail.
  C8Params seed = mk_slice(0.25, 0.04, -0.02, 0.45, 0.40, 0.04, 0.0, 0.0, 0.0);

  const auto rc = c8_fit_slice_lm(seed, k, mid, spread, 12, 1e-6);
  EXPECT_FALSE(rc.has_value())
      << "gradient-blind (rank-deficient) C8 fit must be rejected, not "
         "returned as a silent success";
}

TEST(CalibRobustness, C8FitSliceLm_HealthySeed_StillSucceeds) {
  // Regression guard: the rank check must NOT reject a well-posed fit.
  const C8Params truth =
      mk_slice(0.25, 0.04, -0.02, 0.45, 0.40, 0.030, -0.003, 0.005, -0.005);
  constexpr int N = 11;
  std::array<double, N> k{};
  std::array<double, N> mid{};
  std::array<double, N> spread{};
  for (int i = 0; i < N; ++i) {
    k[static_cast<std::size_t>(i)] = -0.25 + 0.05 * static_cast<double>(i);
    mid[static_cast<std::size_t>(i)] =
        c8_slice_w(truth, k[static_cast<std::size_t>(i)]);
    spread[static_cast<std::size_t>(i)] = 0.0005;
  }

  C8Params seed = truth;
  seed.v *= 1.10;  // v_min (0.030) stays comfortably below v -> gradients valid
  seed.kappa = 0.0;
  seed.q_L = 0.0;
  seed.q_R = 0.0;

  const auto rc = c8_fit_slice_lm(seed, k, mid, spread, 12, 1e-6);
  EXPECT_TRUE(rc.has_value());
}

// ─────────────────────────────────────────────────────────────────────────
// P0-4 — CorrectionCache bounded scratch init (hygiene).
//
// A populated cache's eval / eval_grad must be finite and deterministic: the
// bounded prefix init of the Clenshaw scratch must change no result vs the
// write-before-read path.
// ─────────────────────────────────────────────────────────────────────────

TEST(CalibRobustness, CorrectionCacheEval_IsFiniteAndDeterministic) {
  auto built = CorrectionCache::build(24, 16, 12, 0.05, 0.0, -0.5, 0.5, 0.1, 1.0,
                                      0.1, 0.5, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);
  ASSERT_TRUE(tbl.populated());

  for (double k_log = -0.4; k_log <= 0.4 + 1e-9; k_log += 0.2) {
    for (double T = 0.2; T <= 0.9 + 1e-9; T += 0.35) {
      for (double sigma = 0.15; sigma <= 0.45 + 1e-9; sigma += 0.15) {
        const double v0 = tbl.eval(k_log, T, sigma);
        const double v1 = tbl.eval(k_log, T, sigma);
        EXPECT_TRUE(std::isfinite(v0));
        EXPECT_GE(v0, 0.0);
        EXPECT_EQ(v0, v1);  // deterministic: scratch init leaves no residue

        double dk = 0.0;
        double dT = 0.0;
        double ds = 0.0;
        const double g0 = tbl.eval_grad(k_log, T, sigma, &dk, &dT, &ds);
        EXPECT_TRUE(std::isfinite(g0));
        EXPECT_TRUE(std::isfinite(dk));
        EXPECT_TRUE(std::isfinite(dT));
        EXPECT_TRUE(std::isfinite(ds));
        EXPECT_EQ(g0, v0);  // eval_grad value channel matches eval
      }
    }
  }
}

}  // namespace
