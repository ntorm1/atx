#include <gtest/gtest.h>

#include <cmath>

#include "fitting/legacy_surface.hpp"  // Surface<> (demoted, S4-T21)
#include "atx/vol/api/fitting/surface.hpp"

// Vol-surface evaluator + time-interpolation coverage, ported from the C
// ats-vol test_vol_surface.c:
//   - eSSVI evaluator: ATM total variance equals theta; w > 0 off-ATM.
//   - eSSVI gradient matches finite differences.
//   - surface bracket-find + linear-in-total-variance time interpolation.
//   - extrapolation in T (both past the longest slice, and the Sprint-26
//     short-T guard) returns NaN.
//
// Deliberately NOT reproduced (out of scope for this port — see
// surface.hpp's file header):
//   - reparam_roundtrip_in_cube (Mingone cube reparametrization; that's
//     calibration/optimizer machinery, not an evaluator).
//   - every vol_essvi_residual / bspline_* test (Sprint 11-14 wing
//     residual + B-spline basis fitter; calibration-adjacent, requires the
//     basis evaluator + fitter this port does not carry).

namespace {

using atx::vol::EssviSlice;
using atx::vol::EssviSurface;
using atx::vol::essvi_w;
using atx::vol::essvi_w_grad;
using atx::vol::ErrorCode;
using atx::vol::svi_w;
using atx::vol::SviSlice;
using atx::vol::SviSurface;

// ── Raw SVI evaluator ────────────────────────────────────────────────────

TEST(SviEvaluator, AtM_EqualsAPlusBSigma) {
  // At k_log == m, dk == 0, so w == a + b*sqrt(0 + sigma^2) == a + b*sigma
  // (sigma assumed >= 0, as any fitted slice would carry).
  SviSlice s{/*a=*/0.02, /*b=*/0.3, /*rho=*/-0.4, /*m=*/0.1, /*sigma=*/0.25,
            /*T=*/0.5};
  const double w = svi_w(s, s.m);
  EXPECT_LT(std::fabs(w - (s.a + s.b * s.sigma)), 1.0e-15);
}

TEST(SviEvaluator, Symmetric_MatchesClosedFormOffAtm) {
  SviSlice s{/*a=*/0.02, /*b=*/0.3, /*rho=*/0.0, /*m=*/0.0, /*sigma=*/0.2,
            /*T=*/0.5};
  for (int i = -10; i <= 10; ++i) {
    const double k = 0.05 * static_cast<double>(i);
    const double expected = s.a + s.b * std::sqrt(k * k + s.sigma * s.sigma);
    EXPECT_LT(std::fabs(svi_w(s, k) - expected), 1.0e-15);
  }
}

// ── eSSVI evaluator ──────────────────────────────────────────────────────

TEST(EssviEvaluator, Atm_TotalVarianceEqualsTheta) {
  // w(k=0) = (theta/2) * (1 + 0 + sqrt(rho^2 + 1 - rho^2)) = theta.
  EssviSlice s{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/-0.3, /*T=*/0.0};
  const double w = essvi_w(s, 0.0);
  EXPECT_LT(std::fabs(w - s.theta), 1.0e-15);
}

TEST(EssviEvaluator, OffAtm_TotalVarianceStaysPositive) {
  EssviSlice s{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/-0.3, /*T=*/0.0};
  for (int i = -10; i <= 10; ++i) {
    const double k = 0.05 * static_cast<double>(i);
    EXPECT_GT(essvi_w(s, k), 0.0);
  }
}

TEST(EssviEvaluator, Gradient_MatchesForwardFiniteDifference) {
  const EssviSlice s{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/-0.3, /*T=*/0.0};
  const double k = 0.10;
  const auto g = essvi_w_grad(s, k);
  const double w0 = essvi_w(s, k);
  const double h = 1.0e-6;

  EssviSlice sp = s;
  sp.theta += h;
  const double dth_fd = (essvi_w(sp, k) - w0) / h;
  EXPECT_LT(std::fabs(g.dtheta - dth_fd), 1.0e-6);

  sp = s;
  sp.phi += h;
  const double dphi_fd = (essvi_w(sp, k) - w0) / h;
  EXPECT_LT(std::fabs(g.dphi - dphi_fd), 1.0e-6);

  sp = s;
  sp.rho += h;
  const double drho_fd = (essvi_w(sp, k) - w0) / h;
  EXPECT_LT(std::fabs(g.drho - drho_fd), 1.0e-6);
}

// ── Surface time interpolation (eSSVI) ──────────────────────────────────

TEST(EssviSurface, TimeInterpolation_LinearInW_AtMidpoint) {
  EssviSurface surf(4);

  EssviSlice s0{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/0.0, /*T=*/0.25};
  EssviSlice s1{/*theta=*/0.16, /*phi=*/1.0, /*rho=*/0.0, /*T=*/1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());
  ASSERT_EQ(surf.n_slices(), 2u);

  // alpha = (0.625 - 0.25) / (1.00 - 0.25) = 0.5.
  const double t_mid = 0.625;
  const double k = 0.0;
  const double w_lo = essvi_w(s0, k);
  const double w_hi = essvi_w(s1, k);
  const double w_expected = w_lo + 0.5 * (w_hi - w_lo);

  const double w_got = surf.w(k, t_mid);
  EXPECT_LT(std::fabs(w_got - w_expected), 1.0e-14);

  const double iv_got = surf.iv(k, t_mid);
  EXPECT_LT(std::fabs(iv_got - std::sqrt(w_expected / t_mid)), 1.0e-12);
}

TEST(EssviSurface, ExactPillar_AtFirstOrLastSlice_EvaluatesDirectly) {
  EssviSurface surf(4);
  EssviSlice s0{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/-0.2, /*T=*/0.25};
  EssviSlice s1{/*theta=*/0.16, /*phi=*/1.2, /*rho=*/0.1, /*T=*/1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());

  const double k = 0.05;
  EXPECT_LT(std::fabs(surf.w(k, s0.T) - essvi_w(s0, k)), 1.0e-15);
  EXPECT_LT(std::fabs(surf.w(k, s1.T) - essvi_w(s1, k)), 1.0e-15);
}

TEST(EssviSurface, ExtrapolationPastLongestSlice_ReturnsNan) {
  EssviSurface surf(4);
  EssviSlice s0{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/0.0, /*T=*/0.25};
  EssviSlice s1{/*theta=*/0.16, /*phi=*/1.0, /*rho=*/0.0, /*T=*/1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());

  const double w = surf.w(0.0, 2.0);
  EXPECT_TRUE(std::isnan(w));
  const double iv = surf.iv(0.0, 2.0);
  EXPECT_TRUE(std::isnan(iv));
}

TEST(EssviSurface, ShortTBelowHalfFirstSlice_ReturnsNan) {
  // Sprint-26 guard: refuse to extrapolate more than 50% below the first
  // slice's T rather than silently fabricating a blown-up sigma.
  EssviSurface surf(4);
  EssviSlice s0{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/0.0, /*T=*/0.25};
  EssviSlice s1{/*theta=*/0.16, /*phi=*/1.0, /*rho=*/0.0, /*T=*/1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());

  // 0.1 < 0.5 * 0.25 == 0.125, so this must be refused.
  EXPECT_TRUE(std::isnan(surf.w(0.0, 0.1)));
}

TEST(EssviSurface, ShortTAboveHalfFirstSlice_ExtrapolatesToFirstSlice) {
  // 0.15 > 0.5 * 0.25, so the first slice is used directly (no NaN).
  EssviSurface surf(4);
  EssviSlice s0{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/0.0, /*T=*/0.25};
  EssviSlice s1{/*theta=*/0.16, /*phi=*/1.0, /*rho=*/0.0, /*T=*/1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());

  const double w = surf.w(0.0, 0.15);
  EXPECT_LT(std::fabs(w - essvi_w(s0, 0.0)), 1.0e-15);
}

TEST(EssviSurface, SetSlice_IdxAtOrPastCapacity_ReturnsOutOfRange) {
  EssviSurface surf(2);
  EssviSlice s{/*theta=*/0.04, /*phi=*/1.0, /*rho=*/0.0, /*T=*/0.25};
  const auto rc = surf.set_slice(2, s);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(surf.n_slices(), 0u);
}

TEST(EssviSurface, EmptySurface_QueriesReturnNan) {
  EssviSurface surf(4);
  EXPECT_TRUE(std::isnan(surf.w(0.0, 0.5)));
  EXPECT_TRUE(std::isnan(surf.iv(0.0, 0.5)));
}

// ── Surface time interpolation (raw SVI) ────────────────────────────────
//
// No standalone C test exercises the raw-SVI surface path (test_vol_surface.c
// only covers eSSVI), but ats_vol_surface.c's bracket-find/interpolation is
// parametrization-agnostic, so this exercises the same ported logic through
// the other instantiation.

TEST(SviSurface, TimeInterpolation_LinearInW_AtMidpoint) {
  SviSurface surf(4);
  SviSlice s0{/*a=*/0.04, /*b=*/0.1, /*rho=*/-0.2, /*m=*/0.0, /*sigma=*/0.3,
             /*T=*/0.25};
  SviSlice s1{/*a=*/0.16, /*b=*/0.2, /*rho=*/-0.2, /*m=*/0.0, /*sigma=*/0.3,
             /*T=*/1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());

  const double t_mid = 0.625;
  const double k = 0.05;
  const double w_lo = svi_w(s0, k);
  const double w_hi = svi_w(s1, k);
  const double w_expected = w_lo + 0.5 * (w_hi - w_lo);
  EXPECT_LT(std::fabs(surf.w(k, t_mid) - w_expected), 1.0e-13);
}

TEST(SviSurface, ExtrapolationPastLongestSlice_ReturnsNan) {
  SviSurface surf(4);
  SviSlice s0{/*a=*/0.04, /*b=*/0.1, /*rho=*/-0.2, /*m=*/0.0, /*sigma=*/0.3,
             /*T=*/0.25};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  EXPECT_TRUE(std::isnan(surf.w(0.0, 1.0)));
}

}  // namespace
