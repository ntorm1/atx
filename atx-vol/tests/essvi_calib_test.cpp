#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/arb.hpp"           // arb_check_total_surface_all
#include "atx/vol/black76.hpp"       // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"         // CalibOpts, FitObs, build_observations
#include "atx/vol/curve.hpp"         // CurveSet, ForwardPoint
#include "atx/vol/essvi_calib.hpp"   // the unit under test
#include "atx/vol/parallel_for.hpp"  // atx_auto_worker_count (env-cap determinism)
#include "atx/vol/universe.hpp"      // Underlying, Chain, chain_index
#include "atx/vol/vol_surface.hpp"   // EssviParams, VolSurface, essvi_reparam_to_natural

// eSSVI calibrator coverage, ported from the C ats-vol tests
// (test_calibrate_essvi.c): per-slice synthetic-surface recovery, the analytic
// cube Jacobian vs central finite differences, an end-to-end surface fit with
// static-arb validation, and the Mingone sequential theta-monotone guarantee.
//
// Every fixture is generated from a KNOWN eSSVI surface: quotes are Black-76
// prices at the model IVs, so a faithful fit must invert them and recover the
// generating (theta, phi, rho) to a few bps of vol.

namespace {

using atx::vol::arb_check_total_surface_all;
using atx::vol::black76_price;
using atx::vol::black76_value_and_vega;
using atx::vol::build_observations;
using atx::vol::calib_default_opts;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveSet;
using atx::vol::ErrorCode;
using atx::vol::essvi_backbone_w;
using atx::vol::essvi_calib_surface;
using atx::vol::essvi_calib_surface_sequential;
using atx::vol::essvi_fit_slice;
using atx::vol::essvi_reparam_to_natural;
using atx::vol::essvi_w_cube_grad;
using atx::vol::EssviNatural;
using atx::vol::EssviParams;
using atx::vol::FitDiag;
using atx::vol::FitObs;
using atx::vol::ForwardPoint;
using atx::vol::Parametrization;
using atx::vol::Side;
using atx::vol::Underlying;
using atx::vol::VolSurface;

// ── Helpers ──────────────────────────────────────────────────────────────

// A bare backbone slice from natural parameters (rho_scale == 0 => symmetric).
EssviParams backbone(double theta, double phi, double rho, double T) {
  EssviParams s{};
  s.theta = theta;
  s.phi = phi;
  s.rho = rho;
  s.T = T;
  return s;
}

double slice_iv(const EssviParams& s, double k, double T) {
  return std::sqrt(essvi_backbone_w(s, k) / T);
}

// ── Test 1: per-slice synthetic recovery ─────────────────────────────────

TEST(EssviFitSlice, RecoversSyntheticSlice_WithinFewBps) {
  const double theta_true = 0.040;
  const double phi_true = 1.5;
  const double rho_true = -0.30;
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth = backbone(theta_true, phi_true, rho_true, T);

  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k = -0.40 + 0.80 * static_cast<double>(i) /
                                  static_cast<double>(n - 1);
    const double w = essvi_backbone_w(truth, k);
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

  FitDiag diag{};
  const auto res = essvi_fit_slice(obs, T, F, calib_default_opts(), &diag);
  ASSERT_TRUE(res.has_value());
  const EssviParams& fit = *res;

  double max_dv = 0.0;
  double max_rel_w = 0.0;
  for (int i = -50; i <= 50; ++i) {
    const double k = 0.01 * static_cast<double>(i);
    const double w_true = essvi_backbone_w(truth, k);
    const double w_fit = essvi_backbone_w(fit, k);
    max_rel_w = std::max(max_rel_w, std::fabs(w_fit - w_true) / w_true);
    max_dv = std::max(max_dv, std::fabs(slice_iv(fit, k, T) - slice_iv(truth, k, T)));
  }

  EXPECT_LT(max_rel_w, 0.01);                        // < 1 % in w(k)  (C bound)
  EXPECT_LT(max_dv, 2.0e-3);                         // a few bps of vol
  EXPECT_LT(diag.rmse_vol_vega_weighted, 1.0e-3);    // C bound
  // The cube coordinates recover the natural params (loose: w-recovery above
  // is the load-bearing assertion; (theta, phi) can trade off slightly).
  EXPECT_NEAR(fit.theta, theta_true, 3.0e-3);
  EXPECT_NEAR(fit.rho, rho_true, 3.0e-2);
}

// FT-C3 (B3a): eSSVI lee_project enforced theta*phi*(1+|rho|) <= 4/T with theta =
// TOTAL variance. The true Lee/Gatheral wing constraint in total variance is
// T-free (theta*phi*(1+|rho|) <= 4 — exactly the Mingone cube's own bound
// essvi_phi_max). At T > 1 the 4/T form is over-tight (4/T = 2 at T = 2): a
// high-vol steep-skew 2y slice whose wing slope is admissible (2.69 <= 4) but
// exceeds 4/T gets its wings silently flattened below market. Post-fix the wings
// track the quotes.
TEST(EssviFitSlice, LongDatedSteepWings_NotFlattenedByLeeProjection) {
  const double theta_true = 1.20;   // high ATM total variance (sigma_atm ~ 0.77 @ T=2)
  const double phi_true = 1.40;
  const double rho_true = -0.60;    // steep skew
  const double T = 2.0;
  const double F = 100.0;
  // theta*phi*(1+|rho|) = 1.2*1.4*1.6 = 2.688 : admissible (<= 4) but > 4/T = 2.
  const EssviParams truth = backbone(theta_true, phi_true, rho_true, T);

  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k = -0.80 + 1.60 * static_cast<double>(i) / static_cast<double>(n - 1);
    const double w = essvi_backbone_w(truth, k);
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

  FitDiag diag{};
  const auto res = essvi_fit_slice(obs, T, F, calib_default_opts(), &diag);
  ASSERT_TRUE(res.has_value());
  const EssviParams& fit = *res;

  // The fitted wing slope must NOT be clamped to the over-tight 4/T = 2.
  const double wing = fit.theta * fit.phi * (1.0 + std::fabs(fit.rho));
  EXPECT_GT(wing, 2.3)
      << "fitted wing slope clamped to the over-tight 4/T bound: " << wing;

  // Deep-wing IV must track the quotes (a few bps), not be flattened below them.
  double max_dv = 0.0;
  for (int i = 0; i < n; ++i) {
    const double dv = std::fabs(slice_iv(fit, obs[i].k, T) - obs[i].sigma_mkt);
    max_dv = std::max(max_dv, dv);
  }
  EXPECT_LT(max_dv, 3.0e-3) << "wing IV flattened below quotes (max dv=" << max_dv << ")";
}

// FT-P (B6b): the eSSVI LM lm_step's per-damping-trial MatX(3,3)/VecX(3)
// allocation is replaced with reused thread_local buffers (bit-identical solve —
// no numerical change). Pin the fitted cube params on a fixed fixture so any
// accidental numeric drift is caught bit-for-bit.
TEST(EssviFitSlice, FixedFixtureFit_IsBitIdentical) {
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth = backbone(0.040, 1.5, -0.30, T);
  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k = -0.40 + 0.80 * static_cast<double>(i) / static_cast<double>(n - 1);
    const double w = essvi_backbone_w(truth, k);
    FitObs o{};
    o.k = k; o.w_mkt = w; o.sigma_mkt = std::sqrt(w / T);
    o.weight_w = 1.0; o.active_weight_w = 1.0; o.F = F; o.K = F * std::exp(k); o.df = 1.0;
    obs.push_back(o);
  }
  const auto res = essvi_fit_slice(obs, T, F, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  const EssviParams f = *res;
  // Goldens captured from the pre-refactor build (heap MatX(3,3)/VecX(3) per LM
  // damping trial); the thread_local-buffer refactor must preserve them.
  EXPECT_EQ(f.theta, 0.040000000000000001);
  EXPECT_EQ(f.phi, 1.4999999999999996);
  EXPECT_EQ(f.rho, -0.30000000000000004);
}

TEST(EssviFitSlice, ThetaFloor_RaisesAtmTotalVariance) {
  // Observations generated from a LOW ATM total variance (theta = 0.04).
  const double theta_true = 0.040;
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth = backbone(theta_true, 1.5, -0.30, T);

  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k = -0.40 + 0.80 * static_cast<double>(i) /
                                  static_cast<double>(n - 1);
    const double w = essvi_backbone_w(truth, k);
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

  // Unfloored: the fit recovers the (low) truth theta == w(0).
  const auto free_fit = essvi_fit_slice(obs, T, F, calib_default_opts());
  ASSERT_TRUE(free_fit.has_value());
  EXPECT_NEAR(free_fit->theta, theta_true, 3.0e-3);

  // A theta floor ABOVE the data's ATM total variance forces the fit's ATM level
  // up to the floor — the calendar-monotone seam run_surface_parity drives with
  // the previous slice's theta. The fit cannot place theta below the floor.
  const double floor = 0.080;  // 2x the truth ATM total variance
  const auto floored =
      essvi_fit_slice(obs, T, F, calib_default_opts(), nullptr, floor);
  ASSERT_TRUE(floored.has_value());
  EXPECT_GE(floored->theta, floor - 1.0e-9);
}

TEST(EssviFitSlice, EmptyObservations_ReturnsInvalidArgument) {
  const std::vector<FitObs> obs;
  const auto res = essvi_fit_slice(obs, 0.5, 100.0, calib_default_opts());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Warm-start (tick-to-quote incremental refit) ─────────────────────────

namespace {

// A skewed / curved synthetic obs grid — the neutral cold seed (p=0.3, lambda
// ~ATM-symmetric) sits far from this optimum, so the cold LM has real work to
// do and a warm seed can visibly cut it.
std::vector<FitObs> synth_obs(const EssviParams& truth, double T, double F,
                              double w_scale = 1.0) {
  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k =
        -0.40 + 0.80 * static_cast<double>(i) / static_cast<double>(n - 1);
    const double w = essvi_backbone_w(truth, k) * w_scale;
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

}  // namespace

// A warm seed taken from the CONVERGED fit of the same data must reproduce that
// fit (the optimum is unchanged) while spending no more LM work than the cold
// path — the warm start begins at the answer.
TEST(EssviFitSlice, WarmStart_FromOwnOptimumIsIdempotentAndCheaper) {
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth = backbone(0.040, 2.0, -0.55, T);
  const std::vector<FitObs> obs = synth_obs(truth, T, F);

  FitDiag cold_diag{};
  const auto cold = essvi_fit_slice(obs, T, F, calib_default_opts(), &cold_diag);
  ASSERT_TRUE(cold.has_value());

  FitDiag warm_diag{};
  const auto warm = essvi_fit_slice(obs, T, F, calib_default_opts(), &warm_diag,
                                    0.0, &*cold);
  ASSERT_TRUE(warm.has_value());

  // Same optimum in w-space (the load-bearing equivalence).
  double max_dw = 0.0;
  for (int i = -40; i <= 40; ++i) {
    const double k = 0.01 * static_cast<double>(i);
    max_dw = std::max(max_dw,
                      std::fabs(essvi_backbone_w(*warm, k) -
                                essvi_backbone_w(*cold, k)));
  }
  EXPECT_LT(max_dw, 1.0e-6);
  // Warm from the optimum spends no more inner LM steps than the cold seed.
  EXPECT_LE(warm_diag.inner_iters_total, cold_diag.inner_iters_total);
}

// The core tick-to-quote claim: after a small quote move, refitting warm from
// the pre-tick slice converges in strictly fewer inner LM iterations than a
// cold refit — at the same fit quality.
TEST(EssviFitSlice, WarmStart_ReducesIterationsOnTick) {
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth = backbone(0.040, 2.0, -0.55, T);

  // Pre-tick fit (the retained slice).
  FitDiag pre_diag{};
  const auto pre =
      essvi_fit_slice(synth_obs(truth, T, F), T, F, calib_default_opts(),
                      &pre_diag);
  ASSERT_TRUE(pre.has_value());

  // A tick: the whole slice's total variance nudges up 0.5 %.
  const std::vector<FitObs> ticked = synth_obs(truth, T, F, 1.005);

  FitDiag cold_diag{};
  const auto cold =
      essvi_fit_slice(ticked, T, F, calib_default_opts(), &cold_diag);
  ASSERT_TRUE(cold.has_value());

  FitDiag warm_diag{};
  const auto warm = essvi_fit_slice(ticked, T, F, calib_default_opts(),
                                    &warm_diag, 0.0, &*pre);
  ASSERT_TRUE(warm.has_value());

  EXPECT_LT(warm_diag.inner_iters_total, cold_diag.inner_iters_total);
  // Same landing quality — warm is cheaper, not worse.
  EXPECT_NEAR(warm_diag.rmse_vol_vega_weighted,
              cold_diag.rmse_vol_vega_weighted, 5.0e-4);
}

// prior_strength shrinks the warm refit toward the prior: when new data pulls
// the skew one way, a strong prior holds rho closer to the pre-tick value than
// an unregularized (strength 0) warm fit does.
TEST(EssviFitSlice, PriorStrength_ShrinksTowardPrior) {
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth_prior = backbone(0.040, 2.0, -0.55, T);

  const auto prior =
      essvi_fit_slice(synth_obs(truth_prior, T, F), T, F, calib_default_opts());
  ASSERT_TRUE(prior.has_value());

  // New quotes imply a materially different (less negative) skew.
  const EssviParams truth_new = backbone(0.040, 2.0, -0.20, T);
  const std::vector<FitObs> new_obs = synth_obs(truth_new, T, F);

  CalibOpts free_opts = calib_default_opts();
  free_opts.prior_strength = 0.0;
  const auto unreg =
      essvi_fit_slice(new_obs, T, F, free_opts, nullptr, 0.0, &*prior);
  ASSERT_TRUE(unreg.has_value());

  CalibOpts shrunk_opts = calib_default_opts();
  shrunk_opts.prior_strength = 2.0;  // strong pull toward the prior cube
  const auto shrunk =
      essvi_fit_slice(new_obs, T, F, shrunk_opts, nullptr, 0.0, &*prior);
  ASSERT_TRUE(shrunk.has_value());

  // The shrunk fit's skew stays closer to the prior than the free fit's.
  const double d_free = std::fabs(unreg->rho - prior->rho);
  const double d_shrunk = std::fabs(shrunk->rho - prior->rho);
  EXPECT_LT(d_shrunk, d_free);
}

// ── Test 2: analytic cube Jacobian vs central FD ─────────────────────────

TEST(EssviCubeGrad, MatchesCentralFiniteDifference) {
  const double T = 0.5;
  const double psi = 0.45;
  const double p = 0.30;
  const double lambda = 0.52;
  const EssviNatural nat = essvi_reparam_to_natural(psi, p, lambda, T);
  EssviParams s{};
  s.theta = nat.theta;
  s.phi = nat.phi;
  s.rho = nat.rho;
  s.psi = psi;
  s.p = p;
  s.lambda = lambda;
  s.T = T;

  const auto w_at = [&](double a, double b, double c, double k) {
    const EssviNatural n = essvi_reparam_to_natural(a, b, c, T);
    return essvi_backbone_w(backbone(n.theta, n.phi, n.rho, T), k);
  };

  const double eps = 1.0e-6;
  double max_rel = 0.0;
  for (int i = -10; i <= 10; ++i) {
    const double k = 0.03 * static_cast<double>(i);
    const std::array<double, 3> g = essvi_w_cube_grad(s, k);
    const double fd_psi =
        (w_at(psi + eps, p, lambda, k) - w_at(psi - eps, p, lambda, k)) /
        (2.0 * eps);
    const double fd_p =
        (w_at(psi, p + eps, lambda, k) - w_at(psi, p - eps, lambda, k)) /
        (2.0 * eps);
    const double fd_l =
        (w_at(psi, p, lambda + eps, k) - w_at(psi, p, lambda - eps, k)) /
        (2.0 * eps);
    const auto rel = [](double an, double fd) {
      const double scale = std::max(std::fabs(fd), 1.0e-6);
      return std::fabs(an - fd) / scale;
    };
    max_rel = std::max({max_rel, rel(g[0], fd_psi), rel(g[1], fd_p),
                        rel(g[2], fd_l)});
  }
  EXPECT_LT(max_rel, 1.0e-3);  // FD truncation error dominates
}

// ── Surface fixture ──────────────────────────────────────────────────────

// A three-expiry surface with SHARED (phi, rho) and strictly-increasing theta:
// w scales linearly in theta at every k, so the surface is calendar-arb-free by
// construction (the calendar projection is a no-op) and recovery is exact.
struct SurfaceFixture {
  static constexpr double kF = 100.0;
  static constexpr double kPhi = 1.0;
  static constexpr double kRho = -0.25;
  std::vector<double> ts{0.25, 0.50, 1.00};
  std::vector<double> thetas{0.02, 0.045, 0.10};

  [[nodiscard]] EssviParams truth(std::size_t i) const {
    return backbone(thetas[i], kPhi, kRho, ts[i]);
  }

  // Build the underlier with Black-76 priced chains (df == 1, flat/zero rate).
  [[nodiscard]] Underlying make_under() const {
    Underlying u;
    u.uid = 1u;
    u.ticker = "TEST";
    u.spot = kF;

    std::vector<double> strikes;
    for (double K = 84.0; K <= 116.0 + 1e-9; K += 2.0) {
      strikes.push_back(K);
    }

    for (std::size_t si = 0; si < ts.size(); ++si) {
      const double T = ts[si];
      const EssviParams tr = truth(si);
      Chain c;
      c.uid = 1u;
      c.expiry_id = static_cast<std::uint16_t>(si);
      c.expiry_ns = static_cast<std::int64_t>((T * 3.15e16));
      c.T = T;
      c.strikes = strikes;
      const std::size_t n2 = strikes.size() * 2u;
      c.bids.assign(n2, 0.0);
      c.asks.assign(n2, 0.0);
      c.mids.assign(n2, 0.0);
      c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
      c.bid_sizes.assign(n2, 1);
      c.ask_sizes.assign(n2, 1);
      c.ts_ns.assign(n2, 0);
      c.flags.assign(n2, 0u);

      for (std::size_t s = 0; s < strikes.size(); ++s) {
        const double K = strikes[s];
        const double k = std::log(K / kF);
        const double sig = std::sqrt(essvi_backbone_w(tr, k) / T);
        for (int side_i = 0; side_i < 2; ++side_i) {
          const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
          const std::size_t idx = chain_index(static_cast<std::uint16_t>(s), side);
          const double mid = black76_price(kF, K, T, sig, 1.0, side);
          const double vega = black76_value_and_vega(kF, K, T, sig, 1.0, side).vega;
          // Half-spread small enough to clear the spread/vega and
          // spread/mid filters at every strike.
          const double half = std::min(0.005 * vega, 0.25 * mid);
          c.mids[idx] = mid;
          c.bids[idx] = mid - half;
          c.asks[idx] = mid + half;
        }
      }
      u.chains.push_back(std::move(c));
    }
    return u;
  }

  // Flat forward == kF per expiry; empty yield => df == 1.
  [[nodiscard]] CurveSet make_curves(const Underlying& u) const {
    CurveSet cs;
    cs.spot = kF;
    std::vector<ForwardPoint> fps;
    for (const Chain& c : u.chains) {
      ForwardPoint fp{};
      fp.expiry_ns = c.expiry_ns;
      fp.T = c.T;
      fp.F = kF;
      fps.push_back(fp);
    }
    cs.forward.set(fps);
    return cs;
  }
};

// ── Test 3: end-to-end surface recovery + no arbitrage ───────────────────

TEST(EssviCalibSurface, RecoversSyntheticSurface_WithinTolerance) {
  const SurfaceFixture fx;
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;

  FitDiag diag{};
  const auto st = essvi_calib_surface(surface, under, curves, calib_default_opts(),
                                      &diag);
  ASSERT_TRUE(st.has_value());
  ASSERT_EQ(surface.n_slices(), under.chains.size());
  EXPECT_GT(diag.n_quotes_used, 0u);

  // Recover the generating IV across a k x T grid to a few bps.
  double max_dv = 0.0;
  for (std::size_t si = 0; si < surface.n_slices(); ++si) {
    const EssviParams tr = fx.truth(si);
    for (int i = -12; i <= 12; ++i) {
      const double k = 0.01 * static_cast<double>(i);
      const double iv_true = slice_iv(tr, k, fx.ts[si]);
      const double iv_fit =
          surface.iv_on_slice(static_cast<std::uint16_t>(si), k);
      max_dv = std::max(max_dv, std::fabs(iv_fit - iv_true));
    }
  }
  EXPECT_LT(max_dv, 2.0e-3);  // < 20 bps across the anchored grid
  EXPECT_LT(surface.diagnostics().rmse_vol, 2.0e-3);

  // No static arbitrage on the fitted surface.
  const auto arb = arb_check_total_surface_all(surface, -0.5, 0.5, 64u);
  ASSERT_TRUE(arb.has_value());
  EXPECT_EQ(arb->n_calendar, 0u);
  EXPECT_EQ(arb->n_butterfly, 0u);
}

// FT-C9a (B5c): the alternate eSSVI driver (essvi_calib_surface[_sequential])
// ran arb_project_calendar_essvi — the quality-destroying "Project"-style theta
// bump the README warns about — by DEFAULT whenever validate_no_arb was set
// (true by default), silently moving the ATM total-variance level to remove a
// calendar crossing. That theta bump must be an EXPLICIT opt-in, not folded into
// validate_no_arb; the default must leave the ATM level unmoved.
TEST(EssviCalibSurface, AlternateDriverDefault_DoesNotThetaBumpCrossing) {
  const double kF = 100.0;
  const double phi = 1.0;
  const double rho = -0.25;
  const std::array<double, 2> ts{0.25, 0.50};
  const std::array<double, 2> thetas{0.060, 0.040};  // crossing: theta2 < theta1

  Underlying u;
  u.uid = 1u;
  u.ticker = "X";
  u.spot = kF;
  std::vector<double> strikes;
  for (double K = 84.0; K <= 116.0 + 1e-9; K += 2.0) {
    strikes.push_back(K);
  }
  for (std::size_t si = 0; si < 2; ++si) {
    const EssviParams tr = backbone(thetas[si], phi, rho, ts[si]);
    Chain c;
    c.uid = 1u;
    c.expiry_id = static_cast<std::uint16_t>(si);
    c.expiry_ns = static_cast<std::int64_t>(ts[si] * 3.15e16);
    c.T = ts[si];
    c.strikes = strikes;
    const std::size_t n2 = strikes.size() * 2u;
    c.bids.assign(n2, 0.0);
    c.asks.assign(n2, 0.0);
    c.mids.assign(n2, 0.0);
    c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
    c.bid_sizes.assign(n2, 1);
    c.ask_sizes.assign(n2, 1);
    c.ts_ns.assign(n2, 0);
    c.flags.assign(n2, 0u);
    for (std::size_t s = 0; s < strikes.size(); ++s) {
      const double K = strikes[s];
      const double k = std::log(K / kF);
      const double sig = std::sqrt(essvi_backbone_w(tr, k) / ts[si]);
      for (int side_i = 0; side_i < 2; ++side_i) {
        const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
        const std::size_t idx = chain_index(static_cast<std::uint16_t>(s), side);
        const double mid = black76_price(kF, K, ts[si], sig, 1.0, side);
        const double vega = black76_value_and_vega(kF, K, ts[si], sig, 1.0, side).vega;
        const double half = std::min(0.005 * vega, 0.25 * mid);
        c.mids[idx] = mid;
        c.bids[idx] = mid - half;
        c.asks[idx] = mid + half;
      }
    }
    u.chains.push_back(std::move(c));
  }
  CurveSet cs;
  cs.spot = kF;
  std::vector<ForwardPoint> fps;
  for (const Chain& c : u.chains) {
    ForwardPoint fp{};
    fp.expiry_ns = c.expiry_ns;
    fp.T = c.T;
    fp.F = kF;
    fps.push_back(fp);
  }
  cs.forward.set(fps);

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, u.chains.size());
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;
  const auto st = essvi_calib_surface(surface, u, cs, calib_default_opts());
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  ASSERT_EQ(surface.n_slices(), 2u);

  const double iv1 = surface.iv_on_slice(0u, 0.0);
  const double iv2 = surface.iv_on_slice(1u, 0.0);
  const double theta1 = iv1 * iv1 * ts[0];
  const double theta2 = iv2 * iv2 * ts[1];
  // Default must NOT run the theta bump: the ATM crossing is preserved and the
  // T2 ATM level stays near its raw independent fit (0.040).
  EXPECT_LT(theta2, theta1)
      << "default alternate driver theta-bumped the crossing (theta2=" << theta2
      << " theta1=" << theta1 << ")";
  EXPECT_NEAR(theta2, 0.040, 5.0e-3)
      << "ATM level moved from its raw fit (theta2=" << theta2 << ")";
}

TEST(EssviCalibSurface, NonEssviSurface_ReturnsInvalidArgument) {
  const SurfaceFixture fx;
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  auto surf_res = VolSurface::create(1u, Parametrization::Svi, under.chains.size());
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;

  const auto st = essvi_calib_surface(surface, under, curves, calib_default_opts());
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

// ── Test 4: Mingone sequential theta-monotone ────────────────────────────

TEST(EssviCalibSurfaceSequential, ProducesThetaMonotoneSurface) {
  const SurfaceFixture fx;
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;

  const auto st = essvi_calib_surface_sequential(surface, under, curves,
                                                 calib_default_opts());
  ASSERT_TRUE(st.has_value());
  ASSERT_GT(surface.n_slices(), 1u);

  const auto slices = surface.essvi_slices();
  for (std::size_t i = 1; i < slices.size(); ++i) {
    EXPECT_GE(slices[i].theta, slices[i - 1].theta - 1.0e-12);
  }

  // Still recovers the generating surface (the theta floor is non-binding here).
  double max_dv = 0.0;
  for (std::size_t si = 0; si < surface.n_slices(); ++si) {
    const EssviParams tr = fx.truth(si);
    for (int i = -10; i <= 10; ++i) {
      const double k = 0.01 * static_cast<double>(i);
      const double iv_fit =
          surface.iv_on_slice(static_cast<std::uint16_t>(si), k);
      max_dv = std::max(max_dv, std::fabs(iv_fit - slice_iv(tr, k, fx.ts[si])));
    }
  }
  EXPECT_LT(max_dv, 2.0e-3);

  const auto arb = arb_check_total_surface_all(surface, -0.5, 0.5, 64u);
  ASSERT_TRUE(arb.has_value());
  EXPECT_EQ(arb->n_calendar, 0u);
}

// ── Test 5: surface-level warm-start threading ───────────────────────────

// A warm prior surface at the SAME tenors cuts the total LM work of a refit
// on a slightly-perturbed re-generation of the same board, at the same
// recovery quality — the surface-driver analogue of
// EssviFitSlice.WarmStart_ReducesIterationsOnTick.
TEST(EssviCalibSurface, WarmPrior_ReducesTotalIterations) {
  const SurfaceFixture fx;
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  // Pre-tick fit (the retained prior surface).
  auto prior_surf_res =
      VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(prior_surf_res.has_value());
  VolSurface prior_surface = *prior_surf_res;
  const auto prior_st = essvi_calib_surface(prior_surface, under, curves,
                                            calib_default_opts());
  ASSERT_TRUE(prior_st.has_value());

  // A tick: every slice's ATM total variance nudges up 0.5 % (tenors
  // unchanged, so every prior slice matches exactly).
  SurfaceFixture fx_ticked = fx;
  for (double& th : fx_ticked.thetas) {
    th *= 1.005;
  }
  const Underlying under_ticked = fx_ticked.make_under();
  const CurveSet curves_ticked = fx_ticked.make_curves(under_ticked);

  auto cold_surf_res = VolSurface::create(1u, Parametrization::Essvi,
                                          under_ticked.chains.size());
  ASSERT_TRUE(cold_surf_res.has_value());
  VolSurface cold_surface = *cold_surf_res;
  FitDiag cold_diag{};
  const auto cold_st = essvi_calib_surface(
      cold_surface, under_ticked, curves_ticked, calib_default_opts(), &cold_diag);
  ASSERT_TRUE(cold_st.has_value());

  auto warm_surf_res = VolSurface::create(1u, Parametrization::Essvi,
                                          under_ticked.chains.size());
  ASSERT_TRUE(warm_surf_res.has_value());
  VolSurface warm_surface = *warm_surf_res;
  FitDiag warm_diag{};
  const auto warm_st =
      essvi_calib_surface(warm_surface, under_ticked, curves_ticked,
                          calib_default_opts(), &warm_diag, &prior_surface);
  ASSERT_TRUE(warm_st.has_value());

  EXPECT_LT(warm_diag.inner_iters_total, cold_diag.inner_iters_total);

  // Same recovery tolerance as RecoversSyntheticSurface_WithinTolerance.
  double max_dv = 0.0;
  for (std::size_t si = 0; si < warm_surface.n_slices(); ++si) {
    const EssviParams tr = fx_ticked.truth(si);
    for (int i = -12; i <= 12; ++i) {
      const double k = 0.01 * static_cast<double>(i);
      const double iv_true = slice_iv(tr, k, fx_ticked.ts[si]);
      const double iv_fit =
          warm_surface.iv_on_slice(static_cast<std::uint16_t>(si), k);
      max_dv = std::max(max_dv, std::fabs(iv_fit - iv_true));
    }
  }
  EXPECT_LT(max_dv, 2.0e-3);
}

// `prior == nullptr` (default or explicit) must take today's exact cold path:
// two cold fits of the same board reproduce bit-for-bit identical params.
TEST(EssviCalibSurface, WarmPrior_NullIsByteIdentical) {
  const SurfaceFixture fx;
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  auto surf_a_res =
      VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(surf_a_res.has_value());
  VolSurface surface_a = *surf_a_res;
  const auto st_a =
      essvi_calib_surface(surface_a, under, curves, calib_default_opts());
  ASSERT_TRUE(st_a.has_value());

  auto surf_b_res =
      VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(surf_b_res.has_value());
  VolSurface surface_b = *surf_b_res;
  const auto st_b =
      essvi_calib_surface(surface_b, under, curves, calib_default_opts(),
                          /*out_diag=*/nullptr, /*prior=*/nullptr);
  ASSERT_TRUE(st_b.has_value());

  ASSERT_EQ(surface_a.n_slices(), surface_b.n_slices());
  const auto slices_a = surface_a.essvi_slices();
  const auto slices_b = surface_b.essvi_slices();
  for (std::size_t i = 0; i < slices_a.size(); ++i) {
    EXPECT_EQ(slices_a[i].theta, slices_b[i].theta);
    EXPECT_EQ(slices_a[i].phi, slices_b[i].phi);
    EXPECT_EQ(slices_a[i].rho, slices_b[i].rho);
    EXPECT_EQ(slices_a[i].psi, slices_b[i].psi);
    EXPECT_EQ(slices_a[i].p, slices_b[i].p);
    EXPECT_EQ(slices_a[i].lambda, slices_b[i].lambda);
  }
}

// A prior surface whose slices are all far outside the tenor-gap tolerance
// (all maturities shifted by a year) must fall back to the cold seed exactly,
// producing bit-identical results to a plain cold fit.
TEST(EssviCalibSurface, WarmPrior_MismatchedTenorFallsBackCold) {
  const SurfaceFixture fx;
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  auto cold_surf_res =
      VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(cold_surf_res.has_value());
  VolSurface cold_surface = *cold_surf_res;
  const auto cold_st =
      essvi_calib_surface(cold_surface, under, curves, calib_default_opts());
  ASSERT_TRUE(cold_st.has_value());

  // A prior surface fit at wildly different tenors (+1 year each): every
  // |T_prior - T| exceeds kWarmPriorMaxTenorGap (5/365), so no slice matches.
  SurfaceFixture fx_far = fx;
  for (double& t : fx_far.ts) {
    t += 1.0;
  }
  const Underlying under_far = fx_far.make_under();
  const CurveSet curves_far = fx_far.make_curves(under_far);
  auto far_surf_res =
      VolSurface::create(1u, Parametrization::Essvi, under_far.chains.size());
  ASSERT_TRUE(far_surf_res.has_value());
  VolSurface far_surface = *far_surf_res;
  const auto far_st = essvi_calib_surface(far_surface, under_far, curves_far,
                                          calib_default_opts());
  ASSERT_TRUE(far_st.has_value());

  auto mismatched_surf_res =
      VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(mismatched_surf_res.has_value());
  VolSurface mismatched_surface = *mismatched_surf_res;
  const auto mismatched_st =
      essvi_calib_surface(mismatched_surface, under, curves,
                          calib_default_opts(), /*out_diag=*/nullptr,
                          &far_surface);
  ASSERT_TRUE(mismatched_st.has_value());

  ASSERT_EQ(cold_surface.n_slices(), mismatched_surface.n_slices());
  const auto slices_cold = cold_surface.essvi_slices();
  const auto slices_mismatched = mismatched_surface.essvi_slices();
  for (std::size_t i = 0; i < slices_cold.size(); ++i) {
    EXPECT_EQ(slices_cold[i].theta, slices_mismatched[i].theta);
    EXPECT_EQ(slices_cold[i].phi, slices_mismatched[i].phi);
    EXPECT_EQ(slices_cold[i].rho, slices_mismatched[i].rho);
    EXPECT_EQ(slices_cold[i].psi, slices_mismatched[i].psi);
    EXPECT_EQ(slices_cold[i].p, slices_mismatched[i].p);
    EXPECT_EQ(slices_cold[i].lambda, slices_mismatched[i].lambda);
  }
}

// ── C2.1: intra-name expiry parallelism determinism ──────────────────────
//
// The `n_workers` knob on `essvi_calib_surface` fans the per-expiry chain loop
// across a thread pool. It is a PERF-only knob: the fitted params, slice
// order/count, and FitDiag must be bit-identical at every worker count and vs
// the serial path. These three tests pin exactly that (mirror of
// tests/curve_fit_parallel_test.cpp's parallel-fit determinism suite).

// Portable ATX_VOL_FIT_WORKERS env set/unset + RAII guard (mirrors
// tests/curve_fit_parallel_test.cpp): restores the prior value on scope exit
// even if an ASSERT_* exits the test early, so env state never leaks between
// tests sharing the process.
#if defined(_MSC_VER)
inline void set_fit_workers_env(const char* value) {
  ::_putenv_s("ATX_VOL_FIT_WORKERS", value);
}
inline void unset_fit_workers_env() { ::_putenv_s("ATX_VOL_FIT_WORKERS", ""); }
#else
inline void set_fit_workers_env(const char* value) {
  ::setenv("ATX_VOL_FIT_WORKERS", value, 1);
}
inline void unset_fit_workers_env() { ::unsetenv("ATX_VOL_FIT_WORKERS"); }
#endif

class FitWorkersEnvGuard {
 public:
  FitWorkersEnvGuard() {
#if defined(_MSC_VER)
    char* prev = nullptr;
    std::size_t prev_n = 0;
    had_prev_ =
        (::_dupenv_s(&prev, &prev_n, "ATX_VOL_FIT_WORKERS") == 0) && (prev != nullptr);
    if (prev != nullptr) {
      prev_val_ = prev;
      std::free(prev);
    }
#else
    const char* prev = std::getenv("ATX_VOL_FIT_WORKERS");
    had_prev_ = prev != nullptr;
    if (prev != nullptr) {
      prev_val_ = prev;
    }
#endif
  }
  FitWorkersEnvGuard(const FitWorkersEnvGuard&) = delete;
  FitWorkersEnvGuard& operator=(const FitWorkersEnvGuard&) = delete;
  ~FitWorkersEnvGuard() {
    if (had_prev_) {
      set_fit_workers_env(prev_val_.c_str());
    } else {
      unset_fit_workers_env();
    }
  }

 private:
  bool had_prev_ = false;
  std::string prev_val_;
};

// An N-expiry calendar-arb-free fixture (shared phi/rho, strictly-increasing
// theta so w is monotone in T at every k — the calendar projection is a no-op
// and every chain recovers exactly). Wider than the 3-expiry SurfaceFixture so
// an 8-worker fan-out actually gets distinct chains to steal.
[[nodiscard]] SurfaceFixture make_wide_fixture(std::size_t n_expiries) {
  SurfaceFixture fx;
  fx.ts.clear();
  fx.thetas.clear();
  for (std::size_t i = 0; i < n_expiries; ++i) {
    fx.ts.push_back(0.1 + 0.1 * static_cast<double>(i));
    fx.thetas.push_back(0.02 + 0.012 * static_cast<double>(i));
  }
  return fx;
}

void expect_surface_bit_identical(const VolSurface& a, const VolSurface& b) {
  ASSERT_EQ(a.n_slices(), b.n_slices());
  const auto sa = a.essvi_slices();
  const auto sb = b.essvi_slices();
  for (std::size_t i = 0; i < sa.size(); ++i) {
    EXPECT_EQ(sa[i].theta, sb[i].theta) << "theta @" << i;
    EXPECT_EQ(sa[i].phi, sb[i].phi) << "phi @" << i;
    EXPECT_EQ(sa[i].rho, sb[i].rho) << "rho @" << i;
    EXPECT_EQ(sa[i].psi, sb[i].psi) << "psi @" << i;
    EXPECT_EQ(sa[i].p, sb[i].p) << "p @" << i;
    EXPECT_EQ(sa[i].lambda, sb[i].lambda) << "lambda @" << i;
    EXPECT_EQ(sa[i].T, sb[i].T) << "T @" << i;
  }
}

void expect_diag_bit_identical(const FitDiag& a, const FitDiag& b) {
  EXPECT_EQ(a.rmse_vol_vega_weighted, b.rmse_vol_vega_weighted);
  EXPECT_EQ(a.max_residual_vol, b.max_residual_vol);
  EXPECT_EQ(a.outer_iters, b.outer_iters);
  EXPECT_EQ(a.inner_iters_total, b.inner_iters_total);
  EXPECT_EQ(a.n_quotes_used, b.n_quotes_used);
}

// The worker count changes only WHICH thread fits a chain — never the result.
// Fit the same 8-expiry board with n_workers=1 (serial) and n_workers=8
// (fanned) and assert every slice param + FitDiag field is bit-for-bit equal.
TEST(EssviCalibSurface, BitIdenticalAcrossWorkers) {
  const SurfaceFixture fx = make_wide_fixture(8);
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  auto s1 = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  auto s8 = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(s1.has_value());
  ASSERT_TRUE(s8.has_value());
  VolSurface surf1 = *s1;
  VolSurface surf8 = *s8;

  FitDiag d1{};
  FitDiag d8{};
  const auto st1 = essvi_calib_surface(surf1, under, curves, calib_default_opts(),
                                       &d1, /*prior=*/nullptr, /*n_workers=*/1u);
  const auto st8 = essvi_calib_surface(surf8, under, curves, calib_default_opts(),
                                       &d8, /*prior=*/nullptr, /*n_workers=*/8u);
  ASSERT_TRUE(st1.has_value());
  ASSERT_TRUE(st8.has_value());
  ASSERT_EQ(surf1.n_slices(), 8u);
  expect_surface_bit_identical(surf1, surf8);
  expect_diag_bit_identical(d1, d8);
}

// The ATX_VOL_FIT_WORKERS env cap only changes what the AUTO (n_workers=0) case
// resolves to — never the fitted result. Fit twice with n_workers=0: once with
// the env forcing serial (=1), once with it unset (hardware_concurrency), and
// assert bit-identical output.
TEST(EssviCalibSurface, EnvCapIsPerfOnly) {
  FitWorkersEnvGuard env_guard;  // restores ATX_VOL_FIT_WORKERS on scope exit

  const SurfaceFixture fx = make_wide_fixture(8);
  const Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  set_fit_workers_env("1");
  ASSERT_EQ(atx::vol::atx_auto_worker_count(), 1u);
  auto sc = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(sc.has_value());
  VolSurface surf_capped = *sc;
  FitDiag d_capped{};
  const auto st_c =
      essvi_calib_surface(surf_capped, under, curves, calib_default_opts(),
                          &d_capped, /*prior=*/nullptr, /*n_workers=*/0u);
  ASSERT_TRUE(st_c.has_value());

  unset_fit_workers_env();
  auto sa = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(sa.has_value());
  VolSurface surf_auto = *sa;
  FitDiag d_auto{};
  const auto st_a =
      essvi_calib_surface(surf_auto, under, curves, calib_default_opts(),
                          &d_auto, /*prior=*/nullptr, /*n_workers=*/0u);
  ASSERT_TRUE(st_a.has_value());

  ASSERT_EQ(surf_capped.n_slices(), 8u);
  expect_surface_bit_identical(surf_capped, surf_auto);
  expect_diag_bit_identical(d_capped, d_auto);
}

// A MIDDLE expiry fails to build observations, so it consumes no slice slot:
// the surviving chains must compact into contiguous ascending-T slots. Pins
// the prefix-sum compaction logic — serial(1) and parallel(8) must produce the
// identical compacted slice set + identical FitDiag drop/used counts.
TEST(EssviCalibSurface, PartialFailureCompaction) {
  const SurfaceFixture fx = make_wide_fixture(8);
  Underlying under = fx.make_under();
  const CurveSet curves = fx.make_curves(under);

  // Knock out expiry index 3: wipe all its quotes so build_observations fails
  // the min-obs floor and the chain is skipped (no slot, no calendar hole).
  const std::size_t kBad = 3;
  for (double& v : under.chains[kBad].bids) {
    v = 0.0;
  }
  for (double& v : under.chains[kBad].asks) {
    v = 0.0;
  }
  for (double& v : under.chains[kBad].mids) {
    v = 0.0;
  }

  auto s1 = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  auto s8 = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(s1.has_value());
  ASSERT_TRUE(s8.has_value());
  VolSurface surf1 = *s1;
  VolSurface surf8 = *s8;

  FitDiag d1{};
  FitDiag d8{};
  const auto st1 = essvi_calib_surface(surf1, under, curves, calib_default_opts(),
                                       &d1, /*prior=*/nullptr, /*n_workers=*/1u);
  const auto st8 = essvi_calib_surface(surf8, under, curves, calib_default_opts(),
                                       &d8, /*prior=*/nullptr, /*n_workers=*/8u);
  ASSERT_TRUE(st1.has_value());
  ASSERT_TRUE(st8.has_value());

  // One chain dropped → 7 compacted slices; the failed middle chain leaves no
  // hole and the surface stays strictly ascending in T.
  EXPECT_EQ(surf1.n_slices(), 7u);
  expect_surface_bit_identical(surf1, surf8);
  expect_diag_bit_identical(d1, d8);

  const auto sl = surf1.essvi_slices();
  for (std::size_t i = 1; i < sl.size(); ++i) {
    EXPECT_GT(sl[i].T, sl[i - 1].T);
  }
}

}  // namespace
