#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "atx/vol/vol_surface.hpp"

// Coverage for the calibration-grade vol-surface representation
// (vol_surface.hpp), ported alongside the C ats-vol eSSVI/SVI evaluators and
// surface time interpolation. Distinct from surface_test.cpp, which exercises
// the minimal 3-parameter evaluator in surface.hpp.

namespace {

using atx::vol::EssviCube;
using atx::vol::EssviNatural;
using atx::vol::EssviParams;
using atx::vol::ErrorCode;
using atx::vol::essvi_backbone_w;
using atx::vol::essvi_lambda_from_rho;
using atx::vol::essvi_natural_to_reparam;
using atx::vol::essvi_phi_max;
using atx::vol::essvi_reparam_to_natural;
using atx::vol::essvi_residual_w;
using atx::vol::essvi_rho_from_lambda;
using atx::vol::essvi_total_w;
using atx::vol::essvi_w_grad3;
using atx::vol::essvi_w_grad4;
using atx::vol::kTMinEval;
using atx::vol::Parametrization;
using atx::vol::ResidualBasisKind;
using atx::vol::svi_total_w;
using atx::vol::SviParams;
using atx::vol::VolSurface;

constexpr std::uint16_t kNoMatch = 0xFFFF;

// ── eSSVI backbone ───────────────────────────────────────────────────────

TEST(EssviBackbone, Atm_ZeroLogMoneyness_EqualsTheta) {
  // At k = 0: pk = 0, inner = rho^2 + (1 - rho^2) = 1, so w = theta.
  EssviParams s{};
  s.theta = 0.04;
  s.phi = 1.0;
  s.rho = -0.3;
  EXPECT_NEAR(essvi_backbone_w(s, 0.0), s.theta, 1.0e-15);
}

TEST(EssviBackbone, SymmetricRho_OffAtm_MatchesHandComputedValue) {
  // theta=0.04, phi=1, rho=0, k=0.2:
  //   w = 0.5*0.04*(1 + 0 + sqrt(0.04 + 1)) = 0.02*(1 + sqrt(1.04)).
  EssviParams s{};
  s.theta = 0.04;
  s.phi = 1.0;
  s.rho = 0.0;
  EXPECT_NEAR(essvi_backbone_w(s, 0.2), 0.040396078054371138, 1.0e-12);
}

TEST(EssviBackbone, AsymmetricRhoBlend_MatchesConstantRhoEff) {
  // With rho_scale > 0 the effective rho blends toward rho_R via the
  // tanh factor; evaluating that blend must equal a plain slice whose
  // constant rho equals rho_eff(k).
  EssviParams s_blend{};
  s_blend.theta = 0.04;
  s_blend.phi = 1.0;
  s_blend.rho = -0.4;
  s_blend.rho_R = 0.0;
  s_blend.rho_scale = 0.5;

  const double k = 0.3;
  const double bf = 0.5 * (1.0 + std::tanh(k / s_blend.rho_scale));
  const double rho_eff = s_blend.rho + (s_blend.rho_R - s_blend.rho) * bf;

  EssviParams s_plain{};
  s_plain.theta = 0.04;
  s_plain.phi = 1.0;
  s_plain.rho = rho_eff;  // rho_scale == 0 => no blend

  EXPECT_NEAR(essvi_backbone_w(s_blend, k), essvi_backbone_w(s_plain, k),
              1.0e-14);
}

// ── eSSVI wing residual (HINGE_QUAD) ─────────────────────────────────────

TEST(EssviResidual, HingeQuad_OutsideBand_MatchesHandComputedValue) {
  // resid_scale=0.5, k=0.4 => y=0.8, yc=0.4; basis[3]=0.4, basis[4]=0.16.
  // coef[3]=2, coef[4]=3 => dw = 2*0.4 + 3*0.16 = 1.28.
  EssviParams s{};
  s.resid_scale = 0.5;
  s.resid_basis_kind = ResidualBasisKind::HingeQuad;
  s.resid_n_basis = 5;
  s.resid_coef[3] = 2.0;
  s.resid_coef[4] = 3.0;
  EXPECT_NEAR(essvi_residual_w(s, 0.4), 1.28, 1.0e-15);
}

TEST(EssviResidual, InsideInnerBand_ReturnsZero) {
  // resid_scale=0.5, k=0.1 => y=0.2 (|y| <= 0.4): both hinges are zero.
  EssviParams s{};
  s.resid_scale = 0.5;
  s.resid_basis_kind = ResidualBasisKind::HingeQuad;
  s.resid_n_basis = 5;
  s.resid_coef[1] = 7.0;
  s.resid_coef[2] = 9.0;
  s.resid_coef[3] = 2.0;
  s.resid_coef[4] = 3.0;
  EXPECT_NEAR(essvi_residual_w(s, 0.1), 0.0, 1.0e-15);
}

TEST(EssviResidual, ScaleNotPositive_ReturnsZero) {
  EssviParams s{};
  s.resid_scale = 0.0;  // residual disabled
  s.resid_coef[3] = 2.0;
  EXPECT_EQ(essvi_residual_w(s, 0.4), 0.0);
}

// ── eSSVI total variance ─────────────────────────────────────────────────

TEST(EssviTotal, ResidualDisabled_EqualsBackbone) {
  EssviParams s{};
  s.theta = 0.04;
  s.phi = 1.0;
  s.rho = -0.3;
  s.resid_scale = 0.0;         // residual off
  s.resid_coef[3] = 100.0;     // must be ignored
  for (int i = -6; i <= 6; ++i) {
    const double k = 0.05 * static_cast<double>(i);
    EXPECT_NEAR(essvi_total_w(s, k), essvi_backbone_w(s, k), 1.0e-15);
  }
}

TEST(EssviTotal, ResidualEnabled_EqualsBackbonePlusResidual) {
  EssviParams s{};
  s.theta = 0.04;
  s.phi = 1.0;
  s.rho = -0.2;
  s.resid_scale = 0.5;
  s.resid_basis_kind = ResidualBasisKind::HingeQuad;
  s.resid_n_basis = 5;
  s.resid_coef[3] = 0.01;
  s.resid_coef[4] = 0.02;

  const double k = 0.4;
  const double expected = essvi_backbone_w(s, k) + essvi_residual_w(s, k);
  EXPECT_GT(expected, 0.0);  // positivity net not triggered here
  EXPECT_NEAR(essvi_total_w(s, k), expected, 1.0e-15);
}

// ── eSSVI gradients ──────────────────────────────────────────────────────

TEST(EssviGrad, Grad3_MatchesCentralFiniteDifferenceOfBackbone) {
  EssviParams s{};
  s.theta = 0.04;
  s.phi = 1.0;
  s.rho = -0.3;  // rho_scale == 0 => plain rho path
  const double k = 0.1;
  const double h = 1.0e-6;
  const auto g = essvi_w_grad3(s, k);

  EssviParams sp = s;
  EssviParams sm = s;
  sp.theta = s.theta + h;
  sm.theta = s.theta - h;
  const double d_th =
      (essvi_backbone_w(sp, k) - essvi_backbone_w(sm, k)) / (2.0 * h);
  EXPECT_NEAR(g[0], d_th, 1.0e-6);

  sp = s;
  sm = s;
  sp.phi = s.phi + h;
  sm.phi = s.phi - h;
  const double d_phi =
      (essvi_backbone_w(sp, k) - essvi_backbone_w(sm, k)) / (2.0 * h);
  EXPECT_NEAR(g[1], d_phi, 1.0e-6);

  sp = s;
  sm = s;
  sp.rho = s.rho + h;
  sm.rho = s.rho - h;
  const double d_rho =
      (essvi_backbone_w(sp, k) - essvi_backbone_w(sm, k)) / (2.0 * h);
  EXPECT_NEAR(g[2], d_rho, 1.0e-6);
}

TEST(EssviGrad, Grad4_SymmetricMode_CollapsesToGrad3WithZeroRightWing) {
  EssviParams s{};
  s.theta = 0.05;
  s.phi = 1.2;
  s.rho = -0.25;
  s.rho_scale = 0.0;  // symmetric => bf == 0

  const double k = 0.15;
  const auto g3 = essvi_w_grad3(s, k);
  const auto g4 = essvi_w_grad4(s, k);
  EXPECT_NEAR(g4[0], g3[0], 1.0e-15);
  EXPECT_NEAR(g4[1], g3[1], 1.0e-15);
  EXPECT_NEAR(g4[2], g3[2], 1.0e-15);
  EXPECT_EQ(g4[3], 0.0);  // no right-wing sensitivity in symmetric mode
}

// ── Mingone cube reparametrization ───────────────────────────────────────

TEST(EssviReparam, NaturalToCubeToNatural_RoundTripsOnAdmissibleInput) {
  const double T = 0.5;
  const double theta = 0.04;
  const double phi = 1.0;
  const double rho = -0.3;

  const EssviCube cube = essvi_natural_to_reparam(theta, phi, rho, T);
  EXPECT_GE(cube.psi, 0.0);
  EXPECT_LE(cube.psi, 1.0);
  EXPECT_GE(cube.p, 0.0);
  EXPECT_LE(cube.p, 1.0);
  EXPECT_GE(cube.lambda, 0.0);
  EXPECT_LE(cube.lambda, 1.0);

  const EssviNatural nat =
      essvi_reparam_to_natural(cube.psi, cube.p, cube.lambda, T);
  EXPECT_NEAR(nat.theta, theta, 1.0e-9);
  EXPECT_NEAR(nat.phi, phi, 1.0e-9);
  EXPECT_NEAR(nat.rho, rho, 1.0e-9);
}

TEST(EssviReparam, RhoLambda_ScalarMapsAreInverses) {
  const double lambda = 0.35;
  const double rho = essvi_rho_from_lambda(lambda);
  EXPECT_NEAR(essvi_lambda_from_rho(rho), lambda, 1.0e-12);
  // lambda = 0.5 sits at rho = 0 by construction.
  EXPECT_NEAR(essvi_rho_from_lambda(0.5), 0.0, 1.0e-15);
}

// ── Butterfly bound ──────────────────────────────────────────────────────

TEST(EssviPhiMax, KnownTheta_ReturnsMinOfTheTwoBounds) {
  // theta=0.04, rho=0 => s=0.04; b1=4/0.04=100, b2=2/sqrt(0.04)=10; min=10.
  EXPECT_NEAR(essvi_phi_max(0.04, 0.0), 10.0, 1.0e-12);
}

TEST(EssviPhiMax, DegenerateInputs_ReturnZero) {
  EXPECT_EQ(essvi_phi_max(0.0, 0.5), 0.0);    // theta <= 0
  EXPECT_EQ(essvi_phi_max(-1.0, 0.5), 0.0);   // theta <= 0
  EXPECT_EQ(essvi_phi_max(0.04, 1.0), 0.0);   // |rho| >= 1
  EXPECT_EQ(essvi_phi_max(0.04, -1.5), 0.0);  // |rho| >= 1
}

// ── Raw SVI ──────────────────────────────────────────────────────────────

TEST(SviEval, AtM_EqualsAPlusBSigma) {
  // At k = m: dk = 0, r = sigma, so w = a + b*sigma.
  SviParams s{};
  s.a = 0.02;
  s.b = 0.3;
  s.rho = -0.4;
  s.m = 0.1;
  s.sigma = 0.25;
  EXPECT_NEAR(svi_total_w(s, s.m), s.a + s.b * s.sigma, 1.0e-15);
}

TEST(SviEval, OffM_MatchesClosedForm) {
  SviParams s{};
  s.a = 0.02;
  s.b = 0.3;
  s.rho = -0.4;
  s.m = 0.1;
  s.sigma = 0.25;
  for (int i = -8; i <= 8; ++i) {
    const double k = 0.05 * static_cast<double>(i);
    const double dk = k - s.m;
    const double expected =
        s.a + s.b * (s.rho * dk + std::sqrt(dk * dk + s.sigma * s.sigma));
    EXPECT_NEAR(svi_total_w(s, k), expected, 1.0e-15);
  }
}

// ── VolSurface slice storage ─────────────────────────────────────────────

TEST(VolSurfaceSlices, Create_ZeroCapacity_ReturnsInvalidArgument) {
  const auto res = VolSurface::create(1u, Parametrization::Essvi, 0);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(VolSurfaceSlices, SetEssviSlices_GrowsHighWaterAndReadsBack) {
  auto res = VolSurface::create(7u, Parametrization::Essvi, 4);
  ASSERT_TRUE(res.has_value());
  VolSurface surf = std::move(res).value();
  EXPECT_EQ(surf.uid(), 7u);
  EXPECT_EQ(surf.param(), Parametrization::Essvi);
  EXPECT_EQ(surf.capacity(), 4u);
  EXPECT_EQ(surf.n_slices(), 0u);

  EssviParams s0{};
  s0.theta = 0.04;
  s0.T = 0.25;
  EssviParams s1{};
  s1.theta = 0.16;
  s1.T = 1.0;
  ASSERT_TRUE(surf.set_slice_essvi(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice_essvi(1, s1).has_value());

  EXPECT_EQ(surf.n_slices(), 2u);
  ASSERT_EQ(surf.essvi_slices().size(), 2u);
  EXPECT_EQ(surf.essvi_slices()[0].theta, 0.04);
  EXPECT_EQ(surf.essvi_slices()[1].theta, 0.16);
}

TEST(VolSurfaceSlices, SetEssviSlice_IdxAtOrPastCapacity_ReturnsOutOfRange) {
  auto res = VolSurface::create(1u, Parametrization::Essvi, 2);
  ASSERT_TRUE(res.has_value());
  VolSurface surf = std::move(res).value();
  EssviParams s{};
  const auto rc = surf.set_slice_essvi(2, s);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(surf.n_slices(), 0u);
}

TEST(VolSurfaceSlices, SetSviSliceOnEssviSurface_ReturnsInvalidArgument) {
  auto res = VolSurface::create(1u, Parametrization::Essvi, 2);
  ASSERT_TRUE(res.has_value());
  VolSurface surf = std::move(res).value();
  SviParams s{};
  const auto rc = surf.set_slice_svi(0, s);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::InvalidArgument);
}

TEST(VolSurfaceSlices, SviSurface_SetSviSlices_ReadsBack) {
  auto res = VolSurface::create(3u, Parametrization::Svi, 3);
  ASSERT_TRUE(res.has_value());
  VolSurface surf = std::move(res).value();
  SviParams s0{};
  s0.a = 0.04;
  s0.T = 0.25;
  ASSERT_TRUE(surf.set_slice_svi(0, s0).has_value());
  EXPECT_EQ(surf.n_slices(), 1u);
  ASSERT_EQ(surf.svi_slices().size(), 1u);
  EXPECT_EQ(surf.svi_slices()[0].a, 0.04);
  // eSSVI mutator is rejected on an SVI surface.
  EssviParams e{};
  EXPECT_FALSE(surf.set_slice_essvi(0, e).has_value());
}

// ── VolSurface evaluation / time interpolation ───────────────────────────

namespace {
// Two ascending-T eSSVI slices; residual off so total == backbone == theta
// at k = 0.
[[nodiscard]] VolSurface make_two_slice_essvi() {
  auto res = VolSurface::create(1u, Parametrization::Essvi, 4);
  EssviParams s0{};
  s0.theta = 0.04;
  s0.phi = 1.0;
  s0.rho = 0.0;
  s0.T = 0.25;
  EssviParams s1{};
  s1.theta = 0.16;
  s1.phi = 1.0;
  s1.rho = 0.0;
  s1.T = 1.0;
  VolSurface surf = std::move(res).value();
  (void)surf.set_slice_essvi(0, s0);
  (void)surf.set_slice_essvi(1, s1);
  return surf;
}
}  // namespace

TEST(VolSurfaceInterp, EmptySurface_QueriesReturnNan) {
  auto res = VolSurface::create(1u, Parametrization::Essvi, 4);
  ASSERT_TRUE(res.has_value());
  VolSurface surf = std::move(res).value();
  EXPECT_TRUE(std::isnan(surf.w(0.0, 0.5)));
  EXPECT_TRUE(std::isnan(surf.iv(0.0, 0.5)));
}

TEST(VolSurfaceInterp, Midpoint_LinearInTotalVariance) {
  const VolSurface surf = make_two_slice_essvi();
  // alpha = (0.625 - 0.25) / (1.0 - 0.25) = 0.5; at k=0, w_lo=0.04, w_hi=0.16.
  const double w_expected = 0.04 + 0.5 * (0.16 - 0.04);  // 0.10
  EXPECT_NEAR(surf.w(0.0, 0.625), w_expected, 1.0e-14);
  EXPECT_NEAR(surf.iv(0.0, 0.625), std::sqrt(w_expected / 0.625), 1.0e-12);
}

TEST(VolSurfaceInterp, ExactPillars_EvaluateSliceDirectly) {
  const VolSurface surf = make_two_slice_essvi();
  EXPECT_NEAR(surf.w(0.0, 0.25), 0.04, 1.0e-15);
  EXPECT_NEAR(surf.w(0.0, 1.0), 0.16, 1.0e-15);
}

TEST(VolSurfaceInterp, PastLongestSlice_ReturnsNan) {
  const VolSurface surf = make_two_slice_essvi();
  EXPECT_TRUE(std::isnan(surf.w(0.0, 2.0)));
  EXPECT_TRUE(std::isnan(surf.iv(0.0, 2.0)));
}

TEST(VolSurfaceInterp, ShortTBelowHalfFirstSlice_ReturnsNan) {
  const VolSurface surf = make_two_slice_essvi();
  // 0.1 < 0.5 * 0.25 == 0.125: refused.
  EXPECT_TRUE(std::isnan(surf.w(0.0, 0.1)));
}

TEST(VolSurfaceInterp, ShortTAboveHalfFirstSlice_UsesFirstSlice) {
  const VolSurface surf = make_two_slice_essvi();
  // 0.15 > 0.125: first slice used directly.
  EXPECT_NEAR(surf.w(0.0, 0.15), 0.04, 1.0e-15);
}

// ── S2-2.3: iv() divides by the CALLER's raw T ───────────────────────────────
//
// `w()` FLOORS its argument to kTMinEval before bracketing; `iv()` deliberately
// does not (it divides by the un-floored T, matching the C's
// ats_vol_surface_iv). Nothing guarded that divisor. For any surface whose first
// slice sits at or inside ~2 * kTMinEval — a 0DTE board in its last minutes —
// the short-T guard (`T_floored < 0.5 * T0`) does NOT fire at T = 0, so `w()`
// returns a finite positive variance and `sqrt(w / 0.0)` hands the caller +inf
// as an implied vol. The header already promises NaN wherever the evaluation is
// refused; +inf is a number a caller will act on.
namespace {
// First slice at exactly kTMinEval — the floor w() applies — so a T = 0 query
// survives the short-T guard and reaches the divide.
[[nodiscard]] VolSurface make_expiring_essvi() {
  auto res = VolSurface::create(1u, Parametrization::Essvi, 4);
  EssviParams s0{};
  s0.theta = 0.04;
  s0.phi = 1.0;
  s0.rho = 0.0;
  s0.T = kTMinEval;
  EssviParams s1{};
  s1.theta = 0.16;
  s1.phi = 1.0;
  s1.rho = 0.0;
  s1.T = 0.25;
  VolSurface surf = std::move(res).value();
  (void)surf.set_slice_essvi(0, s0);
  (void)surf.set_slice_essvi(1, s1);
  return surf;
}
} // namespace

// The companion half of 2.3: `w()` divides by `(T_hi - T_lo)` and the class has
// no strictly-ascending slice invariant (`set_slice_*` is index-addressed and
// resizes with default T = 0, so ANY ordering is representable). The divisor is
// nevertheless positive for every bracket the guards let through — `T <= T0`,
// `T == T_last` and `T > T_last` all return early, so the search runs only for
// T0 < T < T_last, and it maintains `slice_T(lo) <= T < slice_T(hi)` whether or
// not the slices are sorted (lo is only assigned from a probe that compared
// `<= T`, or stays 0 where T > T0; hi only from a probe that compared `> T`, or
// stays n-1 where T < T_last). This pins that reasoning against a future edit of
// the guards: a non-monotone surface may return a MEANINGLESS number, but never
// an infinity out of a zero-width bracket.
TEST(VolSurfaceInterp, NonMonotoneSlices_WWeightIsNeverInfinite) {
  const std::vector<std::vector<double>> term_axes{
      {0.10, 0.20, 0.20, 0.40},  // duplicate interior pillars
      {0.10, 0.20, 0.20, 0.20},  // duplicate at the long end
      {0.10, 0.90, 0.20, 1.00},  // unsorted interior
      {0.50, 0.10, 0.30, 0.80},  // unsorted at the front
      {0.20, 0.20, 0.20, 0.20},  // fully degenerate
  };
  for (const std::vector<double> &Ts : term_axes) {
    auto res = VolSurface::create(1u, Parametrization::Essvi, Ts.size());
    ASSERT_TRUE(res.has_value());
    VolSurface surf = std::move(res).value();
    for (std::size_t i = 0; i < Ts.size(); ++i) {
      EssviParams s{};
      s.theta = 0.04 + 0.01 * static_cast<double>(i);
      s.phi = 1.0;
      s.rho = 0.0;
      s.T = Ts[i];
      ASSERT_TRUE(surf.set_slice_essvi(i, s).has_value());
    }
    for (const double T : {0.05, 0.15, 0.20, 0.25, 0.30, 0.45, 0.90, 1.00}) {
      const double w = surf.w(0.0, T);
      EXPECT_FALSE(std::isinf(w)) << "T = " << T << " w = " << w;
      const double sigma = surf.iv(0.0, T);
      EXPECT_FALSE(std::isinf(sigma)) << "T = " << T << " iv = " << sigma;
    }
  }
}

TEST(VolSurfaceInterp, IvAtZeroT_ReturnsNanRatherThanInfinity) {
  const VolSurface surf = make_expiring_essvi();
  // Non-vacuity: w() itself still serves this query off the floored T, so the
  // rejection below is attributable to iv()'s divisor and nothing else.
  ASSERT_TRUE(std::isfinite(surf.w(0.0, 0.0)));
  ASSERT_GT(surf.w(0.0, 0.0), 0.0);
  EXPECT_TRUE(std::isnan(surf.iv(0.0, 0.0))) << "iv = " << surf.iv(0.0, 0.0);
}

TEST(VolSurfaceInterp, IvAtNonFiniteOrNegativeT_ReturnsNan) {
  const VolSurface surf = make_expiring_essvi();
  EXPECT_TRUE(std::isnan(surf.iv(0.0, -1.0)));
  EXPECT_TRUE(std::isnan(surf.iv(0.0, std::numeric_limits<double>::infinity())));
  EXPECT_TRUE(std::isnan(surf.iv(0.0, std::numeric_limits<double>::quiet_NaN())));
}

// The positive-T contract is untouched: a legitimate query still divides by the
// caller's own T, NOT by the floored one w() bracketed on.
TEST(VolSurfaceInterp, IvBelowTheEvalFloor_StillDividesByTheCallersT) {
  const VolSurface surf = make_expiring_essvi();
  const double T = 0.5 * kTMinEval; // > 0, below the floor w() applies
  const double w = surf.w(0.0, T);
  ASSERT_TRUE(std::isfinite(w) && w > 0.0);
  EXPECT_EQ(surf.iv(0.0, T), std::sqrt(w / T));
}

TEST(VolSurfaceInterp, FindExactT_MatchesWithinTickTolerance) {
  const VolSurface surf = make_two_slice_essvi();
  EXPECT_EQ(surf.find_exact_T(0.25), std::uint16_t{0});
  EXPECT_EQ(surf.find_exact_T(1.0), std::uint16_t{1});
  EXPECT_EQ(surf.find_exact_T(0.5), kNoMatch);
}

TEST(VolSurfaceInterp, IvOnSlice_UsesSliceOwnT) {
  const VolSurface surf = make_two_slice_essvi();
  const double k = 0.05;
  // Slice 0 has T = 0.25; iv_on_slice divides that slice's variance by 0.25.
  const auto& s0 = surf.essvi_slices()[0];
  const double expected = std::sqrt(essvi_total_w(s0, k) / 0.25);
  EXPECT_NEAR(surf.iv_on_slice(0, k), expected, 1.0e-12);

  // Out-of-range slice index yields NaN.
  EXPECT_TRUE(std::isnan(surf.iv_on_slice(5, k)));
  EXPECT_TRUE(std::isnan(surf.iv_on_slice(kNoMatch, k)));
}

}  // namespace
