#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/arb.hpp"           // arb_check_total_surface_all
#include "atx/vol/black76.hpp"       // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"         // CalibOpts, FitObs, build_observations
#include "atx/vol/curve.hpp"         // CurveSet, ForwardPoint
#include "atx/vol/essvi_calib.hpp"   // the unit under test
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

}  // namespace
