#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/api/fitting/arb.hpp"           // arb_check_total_surface_all
#include "atx/vol/api/pricing/black76.hpp"       // black76_price, black76_value_and_vega
#include "atx/vol/api/fitting/calib.hpp"         // CalibOpts, FitObs, build_observations
#include "fitting/essvi_calib.hpp"   // the unit under test
#include "core/parallel_for.hpp"  // atx_auto_worker_count (env-cap determinism)
#include "atx/vol/api/pricing/rates_curve.hpp"   // CurveSet, ForwardPoint
#include "atx/vol/api/marketdata/universe.hpp"      // Underlying, Chain, chain_index
#include "atx/vol/api/fitting/vol_surface.hpp"   // EssviParams, VolSurface, essvi_reparam_to_natural

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

// Plan item 1.6 — the dense-residual butterfly guard must reject a window whose
// TOTAL variance has collapsed onto the hot-path positivity floor.
//
// `essvi_total_w` floors w at 1e-12 whenever the residual layer drives it
// non-positive, so a guard testing `w > 0` can never fire. A fully clamped
// stencil is then perfectly flat (w == w(k±h) == 1e-12), which makes the
// Lee/Roper density read g = +1 — "arb-free" — and the greedy projection accepts
// a residual that serves ~1e-6 vol.
//
// Fixture: a near-expiry (1 hour) slice quoted at 2 % vol — total variance
// ~4.6e-8 — with a near-money quote gap, fit under a calendar-monotone theta
// floor set by a much fatter prior expiry. The residual layer must cancel a
// backbone ~40x the quoted level, and at that variance scale the g formula is
// dominated by (1 - k·w'/2w)² so a collapsed core still scores non-negative.
TEST(EssviFitSlice, DenseResidualCollapsedWindow_IsNotServedAsNearZeroVol) {
  const double T = 1.0 / (365.25 * 24.0);  // ~1 hour to expiry
  const double F = 100.0;
  const double vol = 0.02;
  const double theta_true = vol * vol * T;
  const EssviParams truth = backbone(theta_true, 1.5, -0.30, T);

  // Wing-ish quotes only: |k| in [0.02, 0.05], no at-the-money row.
  std::vector<FitObs> obs;
  for (int i = 0; i < 8; ++i) {
    const double k = (i < 4) ? -(0.02 + 0.03 * (3 - i) / 3.0)
                             : (0.02 + 0.03 * (i - 4) / 3.0);
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
    o.vega = 20.0;
    o.spread = 0.05;
    obs.push_back(o);
  }

  CalibOpts opts = calib_default_opts();
  opts.residual_disable = false;
  opts.residual_basis_kind = atx::vol::ResidualBasisKind::C2Bspline;
  // The previous (fatter) expiry pins this slice's ATM total variance 40x above
  // its own quotes — the seam essvi_calib_surface_sequential drives.
  const double theta_floor = 40.0 * theta_true;

  const auto res = essvi_fit_slice(obs, T, F, opts, nullptr, theta_floor);
  ASSERT_TRUE(res.has_value());

  // Sweep the guard's own evaluation window (+/- 1.15 * kmax).
  const double kmax = 0.05;
  double min_w = std::numeric_limits<double>::infinity();
  double k_at_min = 0.0;
  for (int i = 0; i <= 200; ++i) {
    const double k =
        -1.15 * kmax + 2.30 * kmax * static_cast<double>(i) / 200.0;
    const double w = atx::vol::essvi_total_w(*res, k);
    if (w < min_w) {
      min_w = w;
      k_at_min = k;
    }
  }

  // No point may sit at (or below) the hot-path positivity floor: that is the
  // clamped state the guard exists to reject.
  EXPECT_GT(min_w, 1.0e-12) << "collapsed onto the w floor at k = " << k_at_min;
  // ...and the served vol must be an economically real number, not ~1 bp.
  EXPECT_GT(std::sqrt(min_w / T), 0.01)
      << "serves " << std::sqrt(min_w / T) << " vol at k = " << k_at_min;
}

// ── T3: the HINGE_QUAD wing-residual layer's admissible region ───────────
//
// Measured on the T1c attempts export (lqbench 2026-08-03, sp100 2026-07-22,
// `--preset robust --fit-path production`): 37 of the 60 BUILD-stage eSSVI-
// primary rejections are `arb_repair_calendar_residual` refusing a board. That
// guard (`arb.cpp:1030`) is reachable ONLY when the lower slice carries a
// residual layer, and it fires AFTER `arb_project_calendar_essvi` has already
// made the BACKBONES calendar-monotone — so what it is reporting is
// `w_total(upper) < w_backbone(upper)`, i.e. the UPPER slice's residual being
// negative. The two profiles that enable this layer in production
// (`IndexEtfUltraLiquid`, `LiquidSingleName`) both select HINGE_QUAD, whose fit
// is a plain ridge LS with coefficients unconstrained in sign and, per the
// PORT NOTE it carries, no density projection either.
//
// R1 (constructive, replaces the corrective repair): a per-slice residual layer
// may only ADD total variance, and must leave the Lee/Roper density
// non-negative. Every cross-slice ordering guarantee in this system is
// established on BACKBONES; a per-slice layer with no knowledge of its
// neighbours has no information that could tell it how much variance it may
// safely subtract, and `arb_repair_calendar_residual` can only damp the LOWER
// slice's residual, so it is structurally unable to repair a violation the
// UPPER slice's residual caused.

namespace {

// Quotes on an eSSVI backbone with the two wings bent in OPPOSITE directions
// outside the residual layer's dead band (|k| > kResidInnerY * kmax = 0.2 here).
// eSSVI wings are asymptotically linear in |k|, so neither bend is reachable by
// the backbone: the unconstrained ridge LS answers with a POSITIVE call-side
// residual and a NEGATIVE put-side one — the exact shape that voids a
// backbone-established calendar ordering.
std::vector<FitObs> bent_wing_obs(const EssviParams& truth, double T, double F,
                                  double put_bend, double call_bend,
                                  double bend_start = 0.20) {
  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k =
        -0.50 + 1.00 * static_cast<double>(i) / static_cast<double>(n - 1);
    const double u = std::max(0.0, std::fabs(k) - bend_start);
    const double bend = (k < 0.0) ? put_bend : call_bend;
    double w = essvi_backbone_w(truth, k) + bend * u * u;
    if (w < 1.0e-6) {
      w = 1.0e-6;
    }
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

CalibOpts hinge_quad_opts() {
  CalibOpts o = calib_default_opts();
  o.residual_disable = false;
  o.residual_basis_kind = atx::vol::ResidualBasisKind::HingeQuad;
  o.residual_n_basis_terms = 5;
  return o;
}

// Worst (most negative) value of total - backbone over the layer's own
// evaluation window.
double worst_residual(const EssviParams& s, double kmax, double* k_at = nullptr) {
  double worst = 0.0;
  for (int i = 0; i <= 400; ++i) {
    const double k =
        -1.15 * kmax + 2.30 * kmax * static_cast<double>(i) / 400.0;
    const double d = atx::vol::essvi_total_w(s, k) - essvi_backbone_w(s, k);
    if (d < worst) {
      worst = d;
      if (k_at != nullptr) {
        *k_at = k;
      }
    }
  }
  return worst;
}

}  // namespace

// R1, part 1 — the layer may not subtract variance from the backbone it sits on.
TEST(EssviWingResidual, NeverSubtractsVarianceFromTheBackbone) {
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth = backbone(0.040, 1.5, -0.30, T);
  // Put wing bent DOWN (unreachable concavity), call wing bent UP.
  const std::vector<FitObs> obs = bent_wing_obs(truth, T, F, -0.15, 0.15);

  const auto res = essvi_fit_slice(obs, T, F, hinge_quad_opts());
  ASSERT_TRUE(res.has_value());
  ASSERT_GT(res->resid_scale, 0.0)
      << "fixture must actually exercise the residual layer";

  double k_at = 0.0;
  const double worst = worst_residual(*res, 0.50, &k_at);
  EXPECT_GE(worst, 0.0) << "residual subtracts " << -worst
                        << " of total variance at k = " << k_at;
}

// R1, part 2 — the deferred per-slice density projection (PORT NOTE in
// `fit_wing_residual`). A steep LINEAR wing is unreachable by the butterfly-
// capped backbone, so the ridge LS parks the excess slope in the residual, where
// nothing bounds it: -(w'^2/4)(1/4 + 1/w) then craters the Lee/Roper density.
// The eSSVI backbone is butterfly-free by construction, so this layer is the
// ONLY source of a butterfly violation on an eSSVI slice.
TEST(EssviWingResidual, KeepsTheLeeRoperDensityNonNegative) {
  const double T = 0.25;
  const double F = 100.0;
  const EssviParams truth = backbone(0.030, 1.5, -0.30, T);

  std::vector<FitObs> obs;
  const int n = 41;
  for (int i = 0; i < n; ++i) {
    const double k =
        -0.50 + 1.00 * static_cast<double>(i) / static_cast<double>(n - 1);
    const double u = std::max(0.0, std::fabs(k) - 0.20);
    const double w = essvi_backbone_w(truth, k) + 1.20 * u;  // steep, linear
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

  const auto res = essvi_fit_slice(obs, T, F, hinge_quad_opts());
  ASSERT_TRUE(res.has_value());

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, 1u);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;
  ASSERT_TRUE(surface.set_slice_essvi(0u, *res).has_value());

  const auto bf = atx::vol::arb_check_butterfly(surface, -0.5, 0.5, 129u);
  ASSERT_TRUE(bf.has_value());
  EXPECT_TRUE(bf->empty()) << bf->size() << " butterfly violations, first at k = "
                           << (bf->empty() ? 0.0 : bf->front().k_log);
}

// R1, part 3 — the production failure, end to end. Two fitted slices whose
// BACKBONES are calendar-ordered by construction (shared phi/rho, theta scaled),
// with the upper slice's put wing quoted below anything eSSVI can reach. The
// shipped `run_surface_parity` order is project-then-repair
// (`surface_parity.cpp:542-543`); the repair must not refuse the board.
TEST(EssviWingResidual, TwoSliceSurfaceSurvivesTheShippedProjectThenRepair) {
  const double F = 100.0;
  const double phi = 1.5;
  const double rho = -0.30;
  const std::array<double, 2> ts{0.25, 0.50};
  const std::array<double, 2> thetas{0.030, 0.036};  // ordered, +20 %

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, 2u);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;

  for (std::size_t si = 0; si < 2u; ++si) {
    const EssviParams truth = backbone(thetas[si], phi, rho, ts[si]);
    // Lower slice: both wings bent UP. Upper slice: only its DEEPEST put quotes
    // bent down, so the backbone barely reacts (the projection still orders the
    // two backbones inside its 10 % budget) and the miss lands in the residual.
    const std::vector<FitObs> obs =
        (si == 0u)
            ? bent_wing_obs(truth, ts[si], F, 0.10, 0.10)
            : bent_wing_obs(truth, ts[si], F, -3.0, 0.10, /*bend_start=*/0.40);
    const auto res = essvi_fit_slice(obs, ts[si], F, hinge_quad_opts());
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(surface.set_slice_essvi(si, *res).has_value());
  }

  // The certified band and grid `run_surface_parity` actually repairs over.
  const auto proj = atx::vol::arb_project_calendar_essvi(surface, -0.5, 0.5, 25u);
  ASSERT_TRUE(proj.has_value()) << proj.error().to_string();
  const auto rep = atx::vol::arb_repair_calendar_residual(surface, -0.5, 0.5, 25u);
  EXPECT_TRUE(rep.has_value())
      << "build refused: " << (rep.has_value() ? "" : rep.error().to_string());
}

// (N1), the OTHER half of the BUILD-stage refusals — 12 lqbench + 11 sp100
// boards where `arb_project_calendar_essvi` reports "needs a cumulative ATM
// level scale of X, beyond the fidelity budget 1.100000". That is the fitted
// theta term structure inverting by more than 10 %, and it is a CORRECTIVE
// failure for the same reason R1 was: on the production path
// (`CalendarRepair::Project`, pricer_fitter.cpp:1250) every slice is fit
// independently — `run_surface_parity` calls `essvi_fit_slice` with the
// DEFAULT `theta_floor = 0` (surface_parity.cpp:426) — and the inversion is
// only discovered afterwards, when a level scale is the only tool left.
//
// The constructive seam for (N1) already exists — `essvi_fit_slice`'s
// `theta_floor`, which the production caller leaves at its 0 default. This test
// MEASURES what closing that one seam is worth, and records that it is not
// enough on its own: with theta pinned equal, the fitted (phi, rho) are still
// free, so the WINGS cross and the projection is still asked for a level scale
// beyond its budget. That is plan §2.1's (N2) — the wing-slope ordering
// `psi_2 +/- chi_2 >= psi_1 +/- chi_1` — and it needs the previous slice's
// (psi, chi), which `essvi_fit_slice` has no parameter for.
TEST(EssviCalendarOrdering, ThetaFloorShrinksButDoesNotCloseTheProjectionGap) {
  const double F = 100.0;
  const double phi = 1.5;
  const double rho = -0.30;
  const std::array<double, 2> ts{0.25, 0.50};
  // The quotes themselves invert at the ATM level by ~20 % — twice the 10 %
  // fidelity budget `arb_project_calendar_essvi` will spend on a level scale.
  const std::array<double, 2> thetas{0.050, 0.040};

  // Worst w_lower/w_upper over the certified band == the `max_ratio` the
  // projection must pay for, so it is directly comparable to its 1.10 budget.
  const auto worst_backbone_ratio = [&](bool floored) {
    std::array<EssviParams, 2> fitted{};
    double theta_floor = 0.0;
    for (std::size_t si = 0; si < 2u; ++si) {
      const EssviParams truth = backbone(thetas[si], phi, rho, ts[si]);
      const std::vector<FitObs> obs = bent_wing_obs(truth, ts[si], F, 0.0, 0.0);
      const auto res = essvi_fit_slice(obs, ts[si], F, calib_default_opts(),
                                       nullptr, floored ? theta_floor : 0.0);
      EXPECT_TRUE(res.has_value());
      fitted[si] = *res;
      theta_floor = res->theta;
    }
    double worst = 1.0;
    for (int i = 0; i <= 200; ++i) {
      const double k = -0.5 + 1.0 * static_cast<double>(i) / 200.0;
      const double w_lo = essvi_backbone_w(fitted[0], k);
      const double w_hi = essvi_backbone_w(fitted[1], k);
      if (w_hi > 1.0e-15) {
        worst = std::max(worst, w_lo / w_hi);
      }
    }
    return worst;
  };

  const double unfloored = worst_backbone_ratio(false);
  const double floored = worst_backbone_ratio(true);

  // (N1) unwired — today's production path (surface_parity.cpp:426).
  EXPECT_GT(unfloored, 1.0 + atx::vol::kCalendarRepairMaxAtmShiftFrac)
      << "fixture no longer reproduces the production BUILD refusal";
  // (N1) wired: strictly better, and the ATM level itself is repaired...
  EXPECT_LT(floored, unfloored);
  // ...but the wings are not, so the board is still refused. (N2) is required.
  EXPECT_GT(floored, 1.0 + atx::vol::kCalendarRepairMaxAtmShiftFrac)
      << "theta floor alone now suffices on this fixture — re-derive the (N2) "
         "sizing before quoting this test as evidence for it";
}

// ── (N2), the wing-slope half of the ordering ────────────────────────────
//
// T3b. `essvi_fit_slice` now takes the whole previous slice (`calendar_prev`)
// rather than a bare theta scalar, so it can impose BOTH plan §2.1 necessary
// conditions in one place — and the production caller
// (`run_surface_parity`, CalendarRepair::Project) passes it.

namespace {

// The T3 fixture: two expiries whose QUOTES invert at the ATM level by ~20 %,
// twice `arb_project_calendar_essvi`'s 10 % level-scale budget. `ordered`
// selects whether slice 2 is fit against slice 1 (N1+N2) or independently.
std::array<EssviParams, 2> fit_inverted_pair(bool ordered) {
  const double F = 100.0;
  const double phi = 1.5;
  const double rho = -0.30;
  const std::array<double, 2> ts{0.25, 0.50};
  const std::array<double, 2> thetas{0.050, 0.040};

  std::array<EssviParams, 2> fitted{};
  for (std::size_t si = 0; si < 2u; ++si) {
    const EssviParams truth = backbone(thetas[si], phi, rho, ts[si]);
    const std::vector<FitObs> obs = bent_wing_obs(truth, ts[si], F, 0.0, 0.0);
    const EssviParams* prev = (ordered && si > 0u) ? &fitted[si - 1u] : nullptr;
    const auto res = essvi_fit_slice(obs, ts[si], F, calib_default_opts(),
                                     nullptr, 0.0, nullptr, prev);
    EXPECT_TRUE(res.has_value());
    if (res.has_value()) {
      fitted[si] = *res;
    }
  }
  return fitted;
}

// Plan §2.1 coordinates: psi = theta*phi, chi = rho*psi. The asymptotic wing
// slopes are (psi +/- chi)/2, so (N2) is `psi +/- chi` non-decreasing in T.
double wing_right(const EssviParams& s) {
  return s.theta * s.phi * (1.0 + s.rho);
}
double wing_left(const EssviParams& s) {
  return s.theta * s.phi * (1.0 - s.rho);
}

}  // namespace

// The constraint itself: an independently fit far slice may carry SMALLER wing
// slopes than the near one (which forces a finite-k crossing); with
// `calendar_prev` supplied it may not.
TEST(EssviCalendarOrdering, CalendarPrevOrdersBothWingSlopes) {
  const std::array<EssviParams, 2> various = fit_inverted_pair(false);
  const std::array<EssviParams, 2> fixed = fit_inverted_pair(true);

  // The fixture must actually exercise (N2): free fits invert at least one wing.
  ASSERT_TRUE(wing_right(various[1]) < wing_right(various[0]) ||
              wing_left(various[1]) < wing_left(various[0]))
      << "fixture no longer inverts a wing slope";

  // (N1) — the ATM level, w(0) == theta.
  EXPECT_GE(fixed[1].theta, fixed[0].theta * (1.0 - 1.0e-12));
  // (N2) — both wings, to the cube's own interior resolution (kCubeEdge).
  EXPECT_GE(wing_right(fixed[1]), wing_right(fixed[0]) * (1.0 - 1.0e-5));
  EXPECT_GE(wing_left(fixed[1]), wing_left(fixed[0]) * (1.0 - 1.0e-5));
}

// The production consequence. `run_surface_parity` (CalendarRepair::Project)
// assembles the slices and then runs `arb_project_calendar_essvi` over the
// certified band; on 12 lqbench + 11 sp100 boards that projection REFUSED the
// board because the level scale it needed exceeded the fidelity budget. Fitting
// the pair under (N1)+(N2) must leave the projection a job it can do.
TEST(EssviCalendarOrdering, OrderedPairSurvivesTheProjectionThatRefusedIt) {
  const auto build = [](const std::array<EssviParams, 2>& slices) {
    auto surf_res = VolSurface::create(1u, Parametrization::Essvi, 2u);
    EXPECT_TRUE(surf_res.has_value());
    VolSurface surface = *surf_res;
    for (std::size_t si = 0; si < 2u; ++si) {
      EXPECT_TRUE(surface.set_slice_essvi(si, slices[si]).has_value());
    }
    return surface;
  };

  VolSurface free_surface = build(fit_inverted_pair(false));
  const auto free_proj =
      atx::vol::arb_project_calendar_essvi(free_surface, -0.5, 0.5, 25u);
  ASSERT_FALSE(free_proj.has_value())
      << "fixture no longer reproduces the production BUILD refusal";

  VolSurface fixed_surface = build(fit_inverted_pair(true));
  const auto fixed_proj =
      atx::vol::arb_project_calendar_essvi(fixed_surface, -0.5, 0.5, 25u);
  EXPECT_TRUE(fixed_proj.has_value())
      << "still refused: " << (fixed_proj.has_value()
                                   ? std::string{}
                                   : fixed_proj.error().to_string());
}

// A slice whose free fit already dominates its predecessor must keep its
// historical parameters BIT for BIT — the constraint is a floor, not a nudge.
TEST(EssviCalendarOrdering, AlreadyOrderedPrev_IsBitIdentical) {
  const double T = 0.5;
  const double F = 100.0;
  const EssviParams truth = backbone(0.040, 1.5, -0.30, T);
  const std::vector<FitObs> obs = bent_wing_obs(truth, T, F, 0.0, 0.0);

  const auto plain = essvi_fit_slice(obs, T, F, calib_default_opts());
  ASSERT_TRUE(plain.has_value());

  // A predecessor far below this slice in BOTH level and wing slopes: theta
  // under the cube band's own floor, psi = theta*phi eight orders down.
  const EssviParams tiny_prev = backbone(1.0e-8, 1.0e-2, -0.30, 0.25);
  ASSERT_LT(wing_right(tiny_prev), wing_right(*plain));
  ASSERT_LT(wing_left(tiny_prev), wing_left(*plain));

  const auto constrained = essvi_fit_slice(obs, T, F, calib_default_opts(),
                                           nullptr, 0.0, nullptr, &tiny_prev);
  ASSERT_TRUE(constrained.has_value());
  EXPECT_EQ(constrained->theta, plain->theta);
  EXPECT_EQ(constrained->phi, plain->phi);
  EXPECT_EQ(constrained->rho, plain->rho);
  EXPECT_EQ(constrained->psi, plain->psi);
  EXPECT_EQ(constrained->p, plain->p);
  EXPECT_EQ(constrained->lambda, plain->lambda);
}

// ── (S1), the closer ─────────────────────────────────────────────────────
//
// T3c. (N1)+(N2) are NECESSARY, not sufficient (Pasquazzi 2023 Prop 4.14), and
// T3b measured the gap: a 400k-draw search over pairs satisfying (N1)+(N2) plus
// the butterfly cap finds in-band w_1/w_2 up to 133x, while adding
// (S1) psi_2/theta_2 <= psi_1/theta_1 bounds it at 1.0 over 200k feasible draws.
// With psi := theta*phi that ratio IS phi, so (S1) reads phi_2 <= phi_1.
//
// The mechanism, in closed form: as rho -> -1 the backbone
// w(k) = 1/2[theta + chi*k + sqrt(psi^2 k^2 + 2 theta chi k + theta^2)] collapses
// toward 0 for k > theta/psi. Two slices with rho near -1 both collapse; (N2)
// only orders the ASYMPTOTIC slopes and happily admits a far slice that
// collapses EARLIER (smaller theta/psi) than the near one, which is exactly a
// finite-k in-band crossing. (S1) is theta_2/psi_2 >= theta_1/psi_1 — precisely
// the statement that the far slice may not collapse first.
namespace {

// A pair from that search: (N1), both (N2) wings and the butterfly cap all hold,
// yet the worst in-band w_prev/w_cur is 3.10. Capping phi_2 at phi_1 takes it to
// exactly 1.0.
constexpr double kS1PrevTheta = 1.3768023970676702;
constexpr double kS1PrevPhi = 0.08843717750445117;
constexpr double kS1PrevRho = 0.7794126898568101;
constexpr double kS1CurTheta = 1.4897394941338273;
constexpr double kS1CurPhi = 1.765062179540711;
constexpr double kS1CurRho = -0.9083205515270472;

double worst_band_ratio(const EssviParams& prev, const EssviParams& cur) {
  double worst = 1.0;
  for (int i = 0; i < 25; ++i) {
    const double k = -0.5 + 1.0 * static_cast<double>(i) / 24.0;
    const double w_lo = essvi_backbone_w(prev, k);
    const double w_hi = essvi_backbone_w(cur, k);
    if (w_hi > 1.0e-15) {
      worst = std::max(worst, w_lo / w_hi);
    }
  }
  return worst;
}

}  // namespace

TEST(EssviCalendarOrdering, S1ClosesTheInBandCrossingN1N2Admit) {
  const double F = 100.0;
  const double t_prev = 1.00;
  const double t_cur = 1.20;
  const EssviParams prev =
      backbone(kS1PrevTheta, kS1PrevPhi, kS1PrevRho, t_prev);
  const EssviParams truth = backbone(kS1CurTheta, kS1CurPhi, kS1CurRho, t_cur);

  // The fixture must be one (N1)+(N2) ADMIT and a real in-band crossing, or the
  // test proves nothing about (S1).
  ASSERT_GE(truth.theta, prev.theta) << "(N1) must already hold on the truth";
  ASSERT_GE(wing_right(truth), wing_right(prev) * (1.0 - 1.0e-12));
  ASSERT_GE(wing_left(truth), wing_left(prev) * (1.0 - 1.0e-12));
  ASSERT_GT(worst_band_ratio(prev, truth), 1.5)
      << "fixture no longer crosses in band under (N1)+(N2)";

  const std::vector<FitObs> obs = bent_wing_obs(truth, t_cur, F, 0.0, 0.0);
  const auto fit = essvi_fit_slice(obs, t_cur, F, calib_default_opts(), nullptr,
                                   0.0, nullptr, &prev);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();

  // The closer did its job: no crossing anywhere in the certified band.
  EXPECT_DOUBLE_EQ(worst_band_ratio(prev, *fit), 1.0);
  // ...by the stated mechanism, not by accident.
  EXPECT_LE(fit->phi, prev.phi * (1.0 + 1.0e-9)) << "(S1) phi_2 <= phi_1";
  // ...and the NECESSARY conditions are still honoured. (S1) is a ceiling on the
  // same cube axis (N2) floors, and necessary must beat sufficient.
  EXPECT_GE(fit->theta, prev.theta * (1.0 - 1.0e-12));
  EXPECT_GE(wing_right(*fit), wing_right(prev) * (1.0 - 1.0e-5));
  EXPECT_GE(wing_left(*fit), wing_left(prev) * (1.0 - 1.0e-5));
}

// (S1) is EARNED, never free. A pair the necessary conditions already order
// inside the band must keep its (N1)+(N2) fit BIT for BIT — the sufficient
// condition costs fit quality and may only be paid where it buys something.
TEST(EssviCalendarOrdering, S1IsNotAppliedToAnAlreadyOrderedPair) {
  const double F = 100.0;
  const double t_prev = 0.25;
  const double t_cur = 0.50;
  // `prev` has a SMALL phi, so an unconditional (S1) would cap this slice hard;
  // it does not cross in band, so nothing may be capped at all.
  const EssviParams prev = backbone(0.030, 0.35, -0.30, t_prev);
  const EssviParams truth = backbone(0.070, 1.20, -0.30, t_cur);
  ASSERT_GT(truth.phi, prev.phi) << "fixture must VIOLATE (S1) to be a test of it";

  const std::vector<FitObs> obs = bent_wing_obs(truth, t_cur, F, 0.0, 0.0);
  const auto with_prev = essvi_fit_slice(obs, t_cur, F, calib_default_opts(),
                                         nullptr, 0.0, nullptr, &prev);
  ASSERT_TRUE(with_prev.has_value()) << with_prev.error().to_string();
  ASSERT_DOUBLE_EQ(worst_band_ratio(prev, *with_prev), 1.0)
      << "fixture must not cross in band, or this tests the wrong branch";

  // The observable signature of "(S1) was not applied": phi is left FREE to
  // exceed the predecessor's, which the cap would have forbidden outright.
  // (Bit-identity itself is pinned by `AlreadyOrderedPrev_IsBitIdentical`, whose
  // `tiny_prev` carries phi = 1e-2 — two orders below the fit's — so that test
  // is red the moment (S1) is applied unconditionally. It cannot be re-used
  // here: this fixture's predecessor DOES bind (N2), so the (N1)+(N2)
  // projection legitimately moves the cube by ulps.)
  EXPECT_GT(with_prev->phi, prev.phi)
      << "(S1) was applied to a pair that did not need it";
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

// A 2-expiry board with shared (phi, rho) and a genuine ATM calendar crossing
// between the two slices: since phi/rho are identical, w scales purely by
// theta at every k (the same reasoning SurfaceFixture documents below), so
// theta2 < theta1 is a crossing EVERYWHERE, not just at k=0. Shared by the
// theta-project opt-in-repair test and the validate_no_arb honest-audit test
// below — same crossing, two different knobs.
struct TwoExpiryBoard {
  Underlying under;
  CurveSet curves;
};

[[nodiscard]] TwoExpiryBoard make_theta_crossing_board(double theta1, double theta2,
                                                       double phi, double rho) {
  const double kF = 100.0;
  const std::array<double, 2> ts{0.25, 0.50};
  const std::array<double, 2> thetas{theta1, theta2};

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
  return TwoExpiryBoard{std::move(u), std::move(cs)};
}

// FT-C9a (B5c): the alternate eSSVI driver (essvi_calib_surface[_sequential])
// ran arb_project_calendar_essvi — the quality-destroying "Project"-style theta
// bump the README warns about — by DEFAULT whenever validate_no_arb was set
// (true by default), silently moving the ATM total-variance level to remove a
// calendar crossing. That theta bump must be an EXPLICIT opt-in, not folded into
// validate_no_arb; the default must leave the ATM level unmoved.
//
// C-8 made `validate_no_arb` an honest audit that now BAILS on exactly this
// crossing (see ValidateNoArb_BailsOnCalendarInversion below), so this test
// disables the audit explicitly to isolate its own concern — whether the
// theta-project REPAIR runs — from the (separate, now-live) validate_no_arb
// gate. Without this, default opts correctly refuse the call outright before
// the theta-bump question is even reachable.
TEST(EssviCalibSurface, AlternateDriverDefault_DoesNotThetaBumpCrossing) {
  const TwoExpiryBoard board = make_theta_crossing_board(0.060, 0.040, 1.0, -0.25);

  CalibOpts opts = calib_default_opts();
  opts.validate_no_arb = false;

  auto surf_res =
      VolSurface::create(1u, Parametrization::Essvi, board.under.chains.size());
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;
  const auto st = essvi_calib_surface(surface, board.under, board.curves, opts);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  ASSERT_EQ(surface.n_slices(), 2u);

  const double iv1 = surface.iv_on_slice(0u, 0.0);
  const double iv2 = surface.iv_on_slice(1u, 0.0);
  const double theta1 = iv1 * iv1 * 0.25;
  const double theta2 = iv2 * iv2 * 0.50;
  // Default must NOT run the theta bump: the ATM crossing is preserved and the
  // T2 ATM level stays near its raw independent fit (0.040).
  EXPECT_LT(theta2, theta1)
      << "default alternate driver theta-bumped the crossing (theta2=" << theta2
      << " theta1=" << theta1 << ")";
  EXPECT_NEAR(theta2, 0.040, 5.0e-3)
      << "ATM level moved from its raw fit (theta2=" << theta2 << ")";
}

// C-8 (FIT-C1): `CalibOpts::validate_no_arb`'s documented contract (calib.hpp)
// is "run the static-arb validators at the end and bail on a violation" — it
// was dead on this driver: `out_diag->n_butterfly_viol` was unconditionally
// stamped 0 and nothing ever bailed, regardless of the knob. The SAME crossing
// board as above, with the audit left ON (calib_default_opts()'s true
// default): the driver must now report the REAL nonzero calendar-violation
// count and refuse to serve the surface, instead of silently serving it.
TEST(EssviCalibSurface, ValidateNoArb_BailsOnCalendarInversion) {
  const TwoExpiryBoard board = make_theta_crossing_board(0.060, 0.040, 1.0, -0.25);

  auto surf_res =
      VolSurface::create(1u, Parametrization::Essvi, board.under.chains.size());
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;

  const CalibOpts opts = calib_default_opts();
  ASSERT_TRUE(opts.validate_no_arb) << "this test exercises the DEFAULT";
  FitDiag diag{};
  const auto st =
      essvi_calib_surface(surface, board.under, board.curves, opts, &diag);
  ASSERT_FALSE(st.has_value())
      << "validate_no_arb must bail on a genuine calendar crossing";
  EXPECT_EQ(st.error().code(), ErrorCode::Unavailable);
  EXPECT_GT(diag.n_calendar_viol, 0u)
      << "FitDiag must carry the REAL violation count, not a stamped zero";
}

// C-8 (FIT-C5) / R1b: a single-chain board whose CORE strikes are pure eSSVI-
// backbone-consistent quotes and whose WING strikes carry a deliberate upward
// quadratic bump the rigid 3-parameter backbone cannot reproduce. The optional
// HINGE_QUAD wing-residual layer is the ONLY part of an eSSVI slice that can
// reproduce it, and reproduced verbatim that bump is a genuine Durrleman g < 0
// butterfly violation in the wing — which is precisely what `fit_wing_residual`'s
// (R1b) density projection damps out before the slice is ever assembled.
constexpr double kWingBoardF = 100.0;
constexpr double kWingBoardT = 0.5;

[[nodiscard]] std::vector<double> wing_board_strikes() {
  std::vector<double> strikes;
  for (double K = 86.0; K <= 116.0 + 1e-9; K += 2.0) {  // core: |k| <~ 0.15
    strikes.push_back(K);
  }
  for (const double K : {120.0, 130.0, 140.0, 150.0, 160.0, 170.0, 180.0}) {
    strikes.push_back(K);  // wing: out to k ~ 0.59
  }
  return strikes;
}

// Mirrors fit_wing_residual's own `scale = kmax` (max |k| over the obs).
[[nodiscard]] double wing_board_scale() {
  return std::log(wing_board_strikes().back() / kWingBoardF);
}

[[nodiscard]] EssviParams wing_board_truth() {
  return backbone(0.04, 1.0, -0.25, kWingBoardT);
}

// The total variance this board QUOTES at k, stated ONCE so the assertions can
// measure the level the fixture actually injected: a second hand-copied bump in
// a test body would be free to drift out of step with the board it checks.
// The shape is HINGE_QUAD's own dead band (kResidInnerY == 0.4) — only the wing
// beyond it carries the bump, so nothing but a large positive `yc^2` coefficient
// in the residual layer can reproduce it.
[[nodiscard]] double wing_board_quoted_w(double k) {
  const double y = std::clamp(k / wing_board_scale(), -1.0, 1.0);
  const double yc = (y > 0.4) ? (y - 0.4) : 0.0;
  return essvi_backbone_w(wing_board_truth(), k) + 0.5 * yc * yc;
}

[[nodiscard]] Underlying make_wing_residual_violation_board() {
  const std::vector<double> strikes = wing_board_strikes();

  Underlying u;
  u.uid = 1u;
  u.ticker = "X";
  u.spot = kWingBoardF;
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.expiry_ns = static_cast<std::int64_t>(kWingBoardT * 3.15e16);
  c.T = kWingBoardT;
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
    const double k = std::log(K / kWingBoardF);
    const double sig = std::sqrt(wing_board_quoted_w(k) / kWingBoardT);
    for (int side_i = 0; side_i < 2; ++side_i) {
      const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(s), side);
      const double mid = black76_price(kWingBoardF, K, kWingBoardT, sig, 1.0, side);
      const double vega =
          black76_value_and_vega(kWingBoardF, K, kWingBoardT, sig, 1.0, side).vega;
      const double half = std::min(0.005 * vega, 0.25 * mid);
      c.mids[idx] = mid;
      c.bids[idx] = mid - half;
      c.asks[idx] = mid + half;
    }
  }
  u.chains.push_back(std::move(c));
  return u;
}

// C-8 (FIT-C1/FIT-C5) meets R1b. This board USED to be served with its bump
// intact: `fit_wing_residual` was a bare unconstrained ridge LS, and the audit
// below was the only thing standing between it and a served butterfly
// arbitrage. The merged layer projects rather than hopes — (R1a) holds each wing
// inside its non-negativity cone and (R1b) halves the offending wing until the
// Lee/Roper density is non-negative over +/- 1.15*kmax, a band that strictly
// contains this audit's fixed +/- 0.5. So the guarantee worth pinning is the
// STRONGER one: the violation never reaches the audit, and the surface IS served.
//
// Green must stay expensive. `n_butterfly_viol == 0` on its own would hold just
// as well if the counter went back to the stamped zero FIT-C1 was raised
// against, and a served surface on its own would hold just as well if the
// residual layer never ran at all. So the zero is corroborated by an INDEPENDENT
// re-scan over the audit's own band, and the layer is pinned to have ENGAGED
// (`resid_scale` is written only on fit_wing_residual's commit path) and to have
// been DAMPED (served wing variance far below the level the board quoted).
TEST(EssviCalibSurface, WingResidualButterflyViolationIsDensityProjectedNotServed) {
  const Underlying under = make_wing_residual_violation_board();
  CurveSet curves;
  curves.spot = under.spot;
  std::vector<ForwardPoint> fps;
  for (const Chain& c : under.chains) {
    ForwardPoint fp{};
    fp.expiry_ns = c.expiry_ns;
    fp.T = c.T;
    fp.F = under.spot;
    fps.push_back(fp);
  }
  curves.forward.set(fps);

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, under.chains.size());
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;

  CalibOpts opts = calib_default_opts();
  opts.residual_disable = false;
  opts.residual_basis_kind = atx::vol::ResidualBasisKind::HingeQuad;
  ASSERT_TRUE(opts.validate_no_arb) << "this test exercises the DEFAULT";

  FitDiag diag{};
  const auto st = essvi_calib_surface(surface, under, curves, opts, &diag);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(diag.n_butterfly_viol, 0u);
  EXPECT_EQ(diag.n_calendar_viol, 0u);

  // The counter is a CLAIM about the served surface; this is the same claim
  // computed independently, over the band and grid the driver itself audits
  // (essvi_calib.cpp kNoArbAuditKMin/KMax/Grid). A stamped zero survives one of
  // these two checks, never both.
  const auto arb = arb_check_total_surface_all(surface, -0.5, 0.5, 64u);
  ASSERT_TRUE(arb.has_value()) << arb.error().to_string();
  EXPECT_EQ(arb->n_butterfly, 0u)
      << "diag claims a clean surface the independent re-scan disagrees with";

  ASSERT_EQ(surface.n_slices(), 1u);
  const EssviParams served = surface.essvi_slices()[0];
  // R1b's OTHER exits (dead-band break, halving budget exhausted) leave the
  // slice backbone-only. That is safe, but it would make every assertion above
  // vacuous — a backbone with no residual is butterfly-free by construction. If
  // this fires, the FIXTURE stopped reaching the layer; it is not a code defect.
  ASSERT_GT(served.resid_scale, 0.0)
      << "fixture no longer exercises the wing-residual layer at all";
  EXPECT_GT(std::fabs(served.resid_coef[3]) + std::fabs(served.resid_coef[4]), 0.0)
      << "committed residual carries no CALL wing, the only wing this board bumps";

  // ENGAGED: variance was added where the board bumped its quotes. DAMPED: the
  // served level is a fraction of what was quoted, which is what (R1b) does to a
  // wing the risk-neutral density cannot support.
  const double k_deep = wing_board_scale();
  const double k_mid = 0.7 * k_deep;
  EXPECT_GT(atx::vol::essvi_total_w(served, k_mid), essvi_backbone_w(served, k_mid));
  EXPECT_LT(atx::vol::essvi_total_w(served, k_deep), wing_board_quoted_w(k_deep))
      << "the quoted wing bump was served through, not projected away";
}

// C-8 (FIT-C1) backstop coverage, at UNIT level. Now that R1b projects the
// residual, the end-to-end test above can no longer drive the audit's BUTTERFLY
// branch to a nonzero count — it exercises the projection, not the count — so
// the branch would otherwise go untested. Hand the audit a surface that
// genuinely breaches the Lee/Roper density, over the audit's own band and grid,
// so the count stays pinned to a real measurement rather than a constant.
//
// HONEST SCOPE: this is NOT end-to-end coverage, and does not claim to be. R1b
// certifies g >= 0 over +/- 1.15*kmax while this audit scans +/- 0.5, so the
// backstop's genuine remaining domain is boards with kmax < 0.435, where the
// annulus (1.15*kmax, 0.5] is never inspected by the projection. Threading a
// board through that annulus is real modelling work and is NOT done here.
TEST(EssviCalibSurface, NoArbAuditButterflyBranchCountsARealViolation) {
  EssviParams s = backbone(0.040, 1.0, -0.25, 0.5);
  // A steep LINEAR call wing: -(w'^2/4)(1/4 + 1/w) craters the density with no
  // curvature at all, so the violation is the density rule's own verdict rather
  // than an artifact of a stencil straddling the hinge basis's kink.
  s.resid_basis_kind = atx::vol::ResidualBasisKind::HingeQuad;
  s.resid_scale = 0.5;
  s.resid_n_basis = 5;
  s.resid_coef[3] = 0.40;

  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, 1u);
  ASSERT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;
  ASSERT_TRUE(surface.set_slice_essvi(0u, s).has_value());

  const auto counts = arb_check_total_surface_all(surface, -0.5, 0.5, 64u);
  ASSERT_TRUE(counts.has_value()) << counts.error().to_string();
  EXPECT_GT(counts->n_butterfly, 0u)
      << "the audit's butterfly branch missed a hand-built density breach";
  // One slice: the calendar branch has nothing to compare against, so anything
  // but zero here would mean these counts are not measurements.
  EXPECT_EQ(counts->n_calendar, 0u);
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
