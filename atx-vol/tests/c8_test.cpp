#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "atx/vol/c8.hpp"
#include "atx/vol/detail/legacy_c8_surface.hpp"  // C8Surface (demoted, S4-T21)
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_backbone_w

// Coverage for the C8 parametric volatility family — evaluator, compact-support
// bump basis, the SVI Jump-Wings reparametrization, the eSSVI warm-start seed,
// and the Roper no-arbitrage projection. Ported from the C ats-vol tests
// test_c8_evaluator.c / test_c8_basis.c / test_c8_jw_reparam.c /
// test_c8_arb_projection.c (same cases and tolerances).

namespace {

using atx::vol::C8Jw;
using atx::vol::C8Params;
using atx::vol::C8RawSvi;
using atx::vol::C8Surface;
using atx::vol::c8_arb_project;
using atx::vol::c8_basis_atm;
using atx::vol::c8_basis_left;
using atx::vol::c8_basis_right;
using atx::vol::c8_jw_to_raw;
using atx::vol::c8_jw_to_raw_jac;
using atx::vol::c8_min_roper_g;
using atx::vol::c8_raw_svi_w;
using atx::vol::c8_raw_to_jw;
using atx::vol::c8_seed_from_essvi;
using atx::vol::c8_slice_grad_w;
using atx::vol::c8_slice_w;
using atx::vol::EssviParams;
using atx::vol::essvi_backbone_w;

// Build a C8 slice with window scales derived from sigma_atm*sqrt(T) = sqrt(v).
C8Params mk_slice(double T, double v, double psi, double p, double c,
                  double vmin, double kappa, double qL, double qR) {
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
  s.h_atm = 1.0 * scale;
  s.k_L = -2.5 * scale;
  s.h_L = 1.0 * scale;
  s.k_R = 2.5 * scale;
  s.h_R = 1.0 * scale;
  s.bumps_active = true;
  return s;
}

// Baseline JW backbone shared across the evaluator tests. psi=-0.01 keeps
// beta = rho - 2*psi/b non-zero so jw_to_raw avoids the symmetric-smile branch.
C8Params mk_baseline(double kappa, double qL, double qR) {
  return mk_slice(0.25, 0.04, -0.01, 0.4, 0.4, 0.038, kappa, qL, qR);
}

// ── Basis: compact support / shape ────────────────────────────────────────

TEST(C8Basis, Atm_OutsideSupport_IsZero) {
  EXPECT_EQ(c8_basis_atm(-1.5, 1.0), 0.0);
  EXPECT_EQ(c8_basis_atm(1.5, 1.0), 0.0);
  EXPECT_EQ(c8_basis_atm(-1.000001, 1.0), 0.0);
  EXPECT_EQ(c8_basis_atm(1.000001, 1.0), 0.0);
}

TEST(C8Basis, Atm_Reflected_IsEven) {
  for (double k = 0.05; k < 0.99; k += 0.07) {
    EXPECT_NEAR(c8_basis_atm(k, 1.0), c8_basis_atm(-k, 1.0), 1e-15);
  }
}

TEST(C8Basis, Atm_AtCenter_EqualsNegOneSeventh) {
  // B_atm(0;1) = (1)^2 * (0 - 1/7) = -1/7 (the level-preserving 1/7 knot).
  EXPECT_NEAR(c8_basis_atm(0.0, 1.0), -1.0 / 7.0, 1e-15);
}

TEST(C8Basis, Atm_OverSupport_IntegratesToZero) {
  // Trapezoid on a 401-point grid over [-1, 1]; closed-form integral is 0.
  const int N = 401;
  const double h = 1.0;
  double s = 0.0;
  for (int i = 0; i < N; ++i) {
    const double k = -h + 2.0 * h * static_cast<double>(i) / static_cast<double>(N - 1);
    const double wt = (i == 0 || i == N - 1) ? 0.5 : 1.0;
    s += wt * c8_basis_atm(k, h);
  }
  s *= 2.0 * h / static_cast<double>(N - 1);
  EXPECT_NEAR(s, 0.0, 1e-6);
}

TEST(C8Basis, Atm_AtSupportBoundary_IsZero) {
  EXPECT_NEAR(c8_basis_atm(-1.0, 1.0), 0.0, 1e-15);
  EXPECT_NEAR(c8_basis_atm(1.0, 1.0), 0.0, 1e-15);
}

TEST(C8Basis, Left_OutsideSupportAndRightHalf_IsZero) {
  EXPECT_EQ(c8_basis_left(-3.6, -2.5, 1.0), 0.0);
  EXPECT_EQ(c8_basis_left(-1.4, -2.5, 1.0), 0.0);
  // On the right half (k > k_L), max(0, -u) = 0.
  EXPECT_NEAR(c8_basis_left(-2.0, -2.5, 1.0), 0.0, 1e-15);
}

TEST(C8Basis, Left_AtCenter_IsZero) {
  EXPECT_NEAR(c8_basis_left(-2.5, -2.5, 1.0), 0.0, 1e-15);
}

TEST(C8Basis, Left_OnLeftHalf_MatchesQuadratic) {
  // u = -0.5 (k = -3.0): (1 - 0.25)^2 * 0.25 = 0.140625.
  EXPECT_NEAR(c8_basis_left(-3.0, -2.5, 1.0), 0.140625, 1e-12);
}

TEST(C8Basis, Right_OutsideSupportAndLeftHalf_IsZero) {
  EXPECT_EQ(c8_basis_right(1.4, 2.5, 1.0), 0.0);
  EXPECT_EQ(c8_basis_right(3.6, 2.5, 1.0), 0.0);
  EXPECT_NEAR(c8_basis_right(2.0, 2.5, 1.0), 0.0, 1e-15);
}

TEST(C8Basis, Right_OnRightHalf_MatchesQuadratic) {
  // u = 0.5 (k = 3.0): 0.5625 * 0.25 = 0.140625.
  EXPECT_NEAR(c8_basis_right(3.0, 2.5, 1.0), 0.140625, 1e-12);
}

TEST(C8Basis, LeftRight_AtAtm_HaveDisjointSupport) {
  EXPECT_EQ(c8_basis_left(0.0, -2.5, 1.0), 0.0);
  EXPECT_EQ(c8_basis_right(0.0, 2.5, 1.0), 0.0);
}

// ── JW reparametrization ──────────────────────────────────────────────────

TEST(C8Jw, RawSvi_AtAtm_MatchesClosedForm) {
  const C8RawSvi raw{0.01, 0.1, -0.3, -0.05, 0.1};
  const double expect =
      raw.a + raw.b * (raw.rho * (-raw.m) + std::sqrt(raw.m * raw.m + raw.sigma * raw.sigma));
  EXPECT_NEAR(c8_raw_svi_w(0.0, raw), expect, 1e-15);
}

TEST(C8Jw, RawSvi_DeepWing_ApproachesLinearAsymptote) {
  const C8RawSvi raw{0.01, 0.1, -0.3, 0.0, 0.05};
  const double k = 5.0;
  const double w_asymptote = raw.a + raw.b * (1.0 + raw.rho) * k;
  EXPECT_NEAR(c8_raw_svi_w(k, raw), w_asymptote, 5e-3);
}

TEST(C8Jw, RawToJwToRaw_AdmissibleSmile_RoundTrips) {
  const double T = 0.25;
  const C8RawSvi in{0.04, 0.4, -0.4, -0.1, 0.2};
  const auto jw = c8_raw_to_jw(in, T);
  ASSERT_TRUE(jw.has_value());
  const auto raw = c8_jw_to_raw(*jw, T, 1e-4);
  ASSERT_TRUE(raw.has_value());
  EXPECT_NEAR(raw->a, in.a, 1e-10);
  EXPECT_NEAR(raw->b, in.b, 1e-10);
  EXPECT_NEAR(raw->rho, in.rho, 1e-10);
  EXPECT_NEAR(raw->m, in.m, 1e-10);
  EXPECT_NEAR(raw->sigma, in.sigma, 1e-10);
}

TEST(C8Jw, JwToRaw_MinimumAtAtm_UsesSigmaFloor) {
  // v == v_min (smile minimum at ATM): m = 0, sigma = sigma_floor.
  const C8Jw jw{0.04, 0.0, 0.2, 0.2, 0.04};
  const auto raw = c8_jw_to_raw(jw, 0.25, 1e-4);
  ASSERT_TRUE(raw.has_value());
  EXPECT_NEAR(raw->m, 0.0, 1e-15);
  EXPECT_NEAR(raw->sigma, 1e-4, 1e-15);
}

// Analytic JW->raw Jacobian d(a,b,rho,m,sigma)/d(v,psi,p,c,v_min) vs central
// finite differences of c8_jw_to_raw. Generic admissible points are built by
// round-tripping moderate raw-SVI tuples through c8_raw_to_jw so the recovered
// smile sits well away from every branch boundary (no active rho/beta/sigma
// clamp, non-degenerate m, denom O(0.1+)). Mirrors
// EssviCubeGrad.MatchesCentralFiniteDifference.
TEST(C8JwToRawJac, MatchesCentralFiniteDifference) {
  const double T = 0.25;
  const double sf = 1e-4;

  // Moderate raw tuples (a,b,rho,m,sigma): |alpha|=|sigma/m| ~ O(1) and varied
  // rho keep denom away from 0 and v-v_min ~ O(1e-2), so central FD is clean.
  const std::array<C8RawSvi, 5> raws{{
      {0.02, 0.40, -0.30, -0.15, 0.15},
      {0.03, 0.35, 0.25, 0.20, 0.16},
      {0.05, 0.50, -0.40, 0.18, 0.12},
      {0.04, 0.45, 0.35, -0.20, 0.22},
      {0.06, 0.30, -0.50, 0.25, 0.10},
  }};

  double max_rel = 0.0;
  for (const C8RawSvi& raw : raws) {
    const auto jw = c8_raw_to_jw(raw, T);
    ASSERT_TRUE(jw.has_value());
    const auto jac = c8_jw_to_raw_jac(*jw, T, sf);
    ASSERT_TRUE(jac.has_value());

    C8Jw base = *jw;
    double* col[5] = {&base.v, &base.psi, &base.p, &base.c, &base.v_min};
    for (std::size_t j = 0; j < 5; ++j) {
      const double saved = *col[j];
      const double h = 1e-6 * (std::fabs(saved) + 1.0);
      *col[j] = saved + h;
      const auto rp = c8_jw_to_raw(base, T, sf);
      *col[j] = saved - h;
      const auto rm = c8_jw_to_raw(base, T, sf);
      *col[j] = saved;
      ASSERT_TRUE(rp.has_value());
      ASSERT_TRUE(rm.has_value());
      const double inv = 1.0 / (2.0 * h);
      const std::array<double, 5> fd{
          (rp->a - rm->a) * inv,     (rp->b - rm->b) * inv,
          (rp->rho - rm->rho) * inv, (rp->m - rm->m) * inv,
          (rp->sigma - rm->sigma) * inv};
      for (std::size_t i = 0; i < 5; ++i) {
        const double an = (*jac)[i][j];
        const double scale = std::max(std::fabs(fd[i]), 1.0);
        const double rel = std::fabs(an - fd[i]) / scale;
        max_rel = std::max(max_rel, rel);
        EXPECT_LT(rel, 1e-8) << "raw{" << raw.a << "} partial d raw[" << i
                             << "]/d jw[" << j << "] analytic=" << an
                             << " fd=" << fd[i];
      }
    }
  }
  std::printf("[ c8_jw_to_raw_jac ] worst FD-vs-analytic rel error = %.3e\n",
              max_rel);
  EXPECT_LT(max_rel, 1e-8);
}

// sigma-floor clamp ACTIVE (non-degenerate m): v-v_min tiny-but-nonzero drives
// sigma = alpha*m below sigma_floor, pinning sigma to the constant floor. The
// documented convention is a zero sigma-row (row 4). m stays a real DoF.
TEST(C8JwToRawJac, SigmaFloorClamp_ZerosSigmaRow) {
  const double T = 0.25;
  const double sf = 1e-4;
  // b=0.4, rho=0, beta=0.05 (non-degenerate); v-v_min=5e-8 (> 1e-12) so the
  // non-degenerate branch runs, yet sigma = alpha*m ~ 1e-4- gets floored.
  const C8Jw jw{0.04, -0.01, 0.4, 0.4, 0.04 - 5e-8};
  const auto raw = c8_jw_to_raw(jw, T, sf);
  ASSERT_TRUE(raw.has_value());
  EXPECT_NEAR(raw->sigma, sf, 1e-18);  // clamp actually active
  const auto jac = c8_jw_to_raw_jac(jw, T, sf);
  ASSERT_TRUE(jac.has_value());
  for (std::size_t j = 0; j < 5; ++j) {
    EXPECT_EQ((*jac)[4][j], 0.0) << "sigma-row col " << j;
  }
}

// Degenerate-m branch (v == v_min): m and sigma are both constants (0,
// sigma_floor) -> rows 3 and 4 are identically zero.
TEST(C8JwToRawJac, DegenerateM_ZerosMAndSigmaRows) {
  const double T = 0.25;
  const double sf = 1e-4;
  const C8Jw jw{0.04, 0.0, 0.2, 0.2, 0.04};  // v == v_min
  const auto raw = c8_jw_to_raw(jw, T, sf);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->m, 0.0);
  EXPECT_EQ(raw->sigma, sf);
  const auto jac = c8_jw_to_raw_jac(jw, T, sf);
  ASSERT_TRUE(jac.has_value());
  for (std::size_t j = 0; j < 5; ++j) {
    EXPECT_EQ((*jac)[3][j], 0.0) << "m-row col " << j;
    EXPECT_EQ((*jac)[4][j], 0.0) << "sigma-row col " << j;
  }
}

TEST(C8Jw, SeedFromEssvi_AtSampleKnots_MatchesEssvi) {
  EssviParams src{};
  src.theta = 0.04;
  src.phi = 5.0;
  src.rho = -0.3;
  src.T = 0.25;
  src.F = 100.0;

  const auto dst = c8_seed_from_essvi(src);
  ASSERT_TRUE(dst.has_value());

  const double scale = std::sqrt(0.04);
  const std::array<double, 5> k_check{-1.5 * scale, -0.5 * scale, 0.0,
                                      0.5 * scale, 1.5 * scale};
  for (double kc : k_check) {
    EXPECT_NEAR(essvi_backbone_w(src, kc), c8_slice_w(*dst, kc), 5e-3);
  }
}

// ── Evaluator ─────────────────────────────────────────────────────────────

TEST(C8Evaluator, BackboneOnly_MatchesRawSviFromJw) {
  const C8Params s = mk_baseline(0.0, 0.0, 0.0);
  const auto raw = c8_jw_to_raw(C8Jw{s.v, s.psi, s.p, s.c, s.v_min}, s.T, 1e-4);
  ASSERT_TRUE(raw.has_value());
  for (double k = -1.0; k <= 1.0; k += 0.13) {
    EXPECT_NEAR(c8_slice_w(s, k), c8_raw_svi_w(k, *raw), 1e-12);
  }
}

TEST(C8Evaluator, NegativeKappa_BendsAtmCurvatureDown) {
  const C8Params s0 = mk_baseline(0.0, 0.0, 0.0);
  const C8Params s1 = mk_baseline(-0.02, 0.0, 0.0);
  const double eps = 0.05;
  const double d2_0 = c8_slice_w(s0, eps) - 2.0 * c8_slice_w(s0, 0.0) + c8_slice_w(s0, -eps);
  const double d2_1 = c8_slice_w(s1, eps) - 2.0 * c8_slice_w(s1, 0.0) + c8_slice_w(s1, -eps);
  EXPECT_LT(d2_1, d2_0);
}

TEST(C8Evaluator, WingBumps_AtAtm_ContributeNothing) {
  const C8Params s = mk_baseline(0.0, 1.0, 1.0);
  const C8Params s_clean = mk_baseline(0.0, 0.0, 0.0);
  EXPECT_NEAR(c8_slice_w(s, 0.0), c8_slice_w(s_clean, 0.0), 1e-12);
}

TEST(C8Evaluator, BumpsInactiveFlag_DisablesBumps) {
  C8Params s = mk_baseline(-0.05, 0.5, 0.5);
  s.bumps_active = false;
  const C8Params s_clean = mk_baseline(0.0, 0.0, 0.0);
  EXPECT_NEAR(c8_slice_w(s, 0.2), c8_slice_w(s_clean, 0.2), 1e-12);
}

TEST(C8Evaluator, Gradient_MatchesCentralFiniteDifference) {
  C8Params s = mk_slice(0.25, 0.04, -0.05, 0.4, 0.4, 0.035, -0.005, 0.01, 0.01);
  const double k_test = -0.15;
  const auto grad = c8_slice_grad_w(s, k_test);
  ASSERT_TRUE(grad.has_value());

  const std::array<double*, 8> params{&s.v,   &s.psi, &s.p,   &s.c,
                                      &s.v_min, &s.kappa, &s.q_L, &s.q_R};
  for (std::size_t j = 0; j < 8; ++j) {
    const double saved = *params[j];
    const double h = 1e-5;
    *params[j] = saved + h;
    const double w_p = c8_slice_w(s, k_test);
    *params[j] = saved - h;
    const double w_m = c8_slice_w(s, k_test);
    *params[j] = saved;
    const double fd = (w_p - w_m) / (2.0 * h);
    EXPECT_NEAR((*grad)[j], fd, 1e-5);
  }
}

TEST(C8Surface, SurfaceW_AtPillar_RoutesToSliceW) {
  auto surf_res = C8Surface::create(1u, 4);
  ASSERT_TRUE(surf_res.has_value());
  C8Surface surf = std::move(*surf_res);

  C8Params sl = mk_slice(0.25, 0.04, -0.01, 0.4, 0.4, 0.038, 0.0, 0.0, 0.0);
  sl.bumps_active = false;
  ASSERT_TRUE(surf.set_slice(0, sl).has_value());

  EXPECT_NEAR(surf.w(0.0, sl.T), c8_slice_w(sl, 0.0), 1e-12);
}

// ── No-arbitrage projection ───────────────────────────────────────────────

TEST(C8Arb, CleanBackbone_IsArbFree) {
  const C8Params s = mk_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.038, 0.0, 0.0, 0.0);
  EXPECT_GE(c8_min_roper_g(s), -1e-6);
}

TEST(C8Arb, Projection_OnArbFreeSlice_IsIdempotent) {
  C8Params s = mk_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.038, 0.0, 0.0, 0.0);
  c8_arb_project(s);
  EXPECT_NEAR(s.arb_damping_factor, 1.0, 1e-12);
}

TEST(C8Arb, Projection_OnArbViolator_DampsAndRestoresConvexity) {
  // Aggressive negative kappa breaks the butterfly condition on the grid.
  C8Params s = mk_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.038, -0.5, 0.0, 0.0);
  c8_arb_project(s);
  EXPECT_GE(s.arb_damping_factor, 0.0);
  EXPECT_LE(s.arb_damping_factor, 1.0);
  EXPECT_GE(c8_min_roper_g(s), -1e-5);
}

}  // namespace
