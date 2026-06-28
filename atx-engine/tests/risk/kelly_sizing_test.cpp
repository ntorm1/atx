// kelly_sizing_test.cpp — p7 S5-0: the NUMERICAL CONTRACT for the conviction-
// scaled fractional-Kelly sizing layer that S5 wires into the live deploy path.
//
// The whole sprint trusts kelly_size() to compute f* = V^{-1} mu EXACTLY before it
// is wired into stage_combine (S5-2). These tests pin that math on HAND-SOLVED
// diagonal fixtures so a regression in the engine library is caught here, at the
// unit level, rather than as a silent mis-size downstream.
//
// Oracle: a D-ONLY FactorModel (X all-zero ⇒ X F Xᵀ vanishes ⇒ V = diag(D)) makes
// V^{-1}mu = mu_i / D_i, the exact closed form every assertion below is checked
// against. This is the same minimal SPD construction the sibling
// risk_kelly_sizing_test.cpp uses; here the fixtures are the VERBATIM S5-0 plan
// values (V = diag(0.01,0.04) ⇒ V^{-1}mu = [10,5]) so the bench table maps 1:1.
//
// Suite name KellySizingMath (distinct from risk_kelly_sizing_test.cpp's
// KellySizing) so both files build into atx-engine-risk-tests without TEST-name
// collision. Caught by `ctest -R KellySizingMath`.

#include <cmath>  // std::fabs
#include <span>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/kelly_sizing.hpp"

namespace atxtest_risk_kelly_sizing_math_test {

using atx::f64;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::FactorModel;
using atx::engine::risk::kelly_size;
using atx::engine::risk::KellyConfig;
using atx::engine::risk::KellyWeights;

// A D-ONLY FactorModel: X all-zero ⇒ the factor block X F Xᵀ is exactly 0, so
// V = diag(D) and V^{-1}mu = mu_i / D_i. K=1 (one zero exposure column) is the
// minimal SPD F the model accepts; F's value is irrelevant since X is zero.
[[nodiscard]] FactorModel diag_model(const VecX &d) {
  const Eigen::Index m = d.size();
  const MatX x = MatX::Zero(m, 1);
  MatX f(1, 1);
  f << 1.0;
  auto r = FactorModel::create(x, f, d, 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

[[nodiscard]] VecX vec(std::initializer_list<f64> xs) {
  VecX v(static_cast<Eigen::Index>(xs.size()));
  Eigen::Index i = 0;
  for (const f64 x : xs) {
    v[i++] = x;
  }
  return v;
}

// (a) 2×2 diagonal covariance, exact hand-solved f*:
//   mu = [0.1, 0.2], V = diag(0.01, 0.04) ⇒ V^{-1}mu = [10.0, 5.0].
//   kelly_fraction=1.0, conviction=[1,1], max_gross<=0 (clamp disabled)
//   ⇒ weights = [10.0, 5.0] exactly. At kelly_fraction=0.25 ⇒ [2.5, 1.25].
TEST(KellySizingMath, TwoByTwoDiagonalFullKellyExact) {
  const VecX d = vec({0.01, 0.04});
  const VecX mu = vec({0.1, 0.2});
  const VecX conv = vec({1.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig full;
  full.kelly_fraction = 1.0;
  full.max_gross = -1.0; // <= 0 disables the gross clamp
  const KellyWeights kf = kelly_size(mu, m, conv, full);

  ASSERT_EQ(kf.weights.size(), 2);
  EXPECT_NEAR(kf.weights[0], 10.0, 1e-12);
  EXPECT_NEAR(kf.weights[1], 5.0, 1e-12);
  EXPECT_DOUBLE_EQ(kf.scale_applied, 1.0); // clamp disabled
  EXPECT_NEAR(kf.gross, 15.0, 1e-12);

  KellyConfig quarter;
  quarter.kelly_fraction = 0.25;
  quarter.max_gross = -1.0;
  const KellyWeights kq = kelly_size(mu, m, conv, quarter);
  EXPECT_NEAR(kq.weights[0], 2.5, 1e-12);
  EXPECT_NEAR(kq.weights[1], 1.25, 1e-12);
}

// (b) 3×3 diagonal covariance: per-name scaling is INDEPENDENT — name i's weight
//   depends only on mu[i] and V[ii] (no cross-coupling, since V is diagonal).
//   mu=[0.1,0.2,-0.3], V=diag(0.01,0.04,0.09) ⇒ V^{-1}mu = [10, 5, -3.333..].
TEST(KellySizingMath, ThreeByThreeDiagonalPerNameIndependent) {
  const VecX d = vec({0.01, 0.04, 0.09});
  const VecX mu = vec({0.1, 0.2, -0.3});
  const VecX conv = vec({1.0, 1.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = -1.0;
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  ASSERT_EQ(kw.weights.size(), 3);
  EXPECT_NEAR(kw.weights[0], 0.1 / 0.01, 1e-12);   // 10.0
  EXPECT_NEAR(kw.weights[1], 0.2 / 0.04, 1e-12);   // 5.0
  EXPECT_NEAR(kw.weights[2], -0.3 / 0.09, 1e-12);  // -3.3333...

  // Independence proof: perturb mu[0] only; weights[1], weights[2] must not move.
  const VecX mu2 = vec({0.5, 0.2, -0.3});
  const KellyWeights kw2 = kelly_size(mu2, m, conv, cfg);
  EXPECT_NEAR(kw2.weights[0], 0.5 / 0.01, 1e-12);  // 50.0 — changed
  EXPECT_EQ(kw2.weights[1], kw.weights[1]);        // unchanged, exactly
  EXPECT_EQ(kw2.weights[2], kw.weights[2]);        // unchanged, exactly
}

// (c) Per-name conviction scaling: 2×2, conviction=[0.5, 1.0], kelly_fraction=1.0:
//   weights = [10.0*0.5, 5.0*1.0] = [5.0, 5.0]. The product conviction[i]*f*[i]
//   is exact (0.5 and 1.0 are representable; the multiply introduces no drift).
TEST(KellySizingMath, PerNameConvictionScalingExact) {
  const VecX d = vec({0.01, 0.04});
  const VecX mu = vec({0.1, 0.2});
  const VecX conv = vec({0.5, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = -1.0;
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  EXPECT_NEAR(kw.weights[0], 5.0, 1e-12); // 10.0 halved
  EXPECT_NEAR(kw.weights[1], 5.0, 1e-12); // 5.0 unchanged
}

// (d) Gross clamp: 2×2, kelly_fraction=1.0, conviction=[1,1], max_gross=1.0:
//   pre-clamp weights [10, 5], gross 15 > 1 ⇒ scale = 1/15, weights = [0.667, 0.333],
//   realized gross == 1.0, scale_applied == 1/15.
TEST(KellySizingMath, GrossClampBindsToMaxGross) {
  const VecX d = vec({0.01, 0.04});
  const VecX mu = vec({0.1, 0.2});
  const VecX conv = vec({1.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = 1.0;
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  const f64 pre_gross = 15.0;
  EXPECT_NEAR(kw.scale_applied, 1.0 / pre_gross, 1e-12);
  EXPECT_NEAR(kw.gross, 1.0, 1e-12);
  EXPECT_NEAR(kw.weights[0], 10.0 / pre_gross, 1e-12); // 0.6666...
  EXPECT_NEAR(kw.weights[1], 5.0 / pre_gross, 1e-12);  // 0.3333...
}

// (e) kelly_fraction=0.0 → zero weights. This is the INERT-DEFAULT proof: when the
//   caller passes fraction=0, the sizing layer contributes nothing. All weights
//   exactly 0.0, gross exactly 0.0, scale_applied exactly 1.0 (clamp not binding —
//   0 never exceeds a positive max_gross).
TEST(KellySizingMath, ZeroFractionGivesZeroWeightsInert) {
  const VecX d = vec({0.01, 0.04});
  const VecX mu = vec({0.1, 0.2});
  const VecX conv = vec({1.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 0.0;
  cfg.max_gross = 1.0;
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  ASSERT_EQ(kw.weights.size(), 2);
  EXPECT_EQ(kw.weights[0], 0.0); // exactly zero, not merely near
  EXPECT_EQ(kw.weights[1], 0.0);
  EXPECT_EQ(kw.gross, 0.0);
  EXPECT_DOUBLE_EQ(kw.scale_applied, 1.0);
}

// (f) Zero-conviction name → EXACTLY 0 weight; the other name is untouched by the
//   conviction scale. conviction=[0.0, 1.0]: weights[0]==0 exactly, weights[1]==5.
TEST(KellySizingMath, ZeroConvictionNameIsExactlyZero) {
  const VecX d = vec({0.01, 0.04});
  const VecX mu = vec({0.1, 0.2});
  const VecX conv = vec({0.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = -1.0;
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  EXPECT_EQ(kw.weights[0], 0.0);          // exact zero (0.0 * finite == 0.0)
  EXPECT_NEAR(kw.weights[1], 5.0, 1e-12); // unchanged by the conviction scale
}

// (g) Twice-run determinism: same inputs in the same process ⇒ bit-for-bit
//   identical weights/gross/scale (no RNG, no mutable global).
TEST(KellySizingMath, TwiceRunIsBitIdentical) {
  const VecX d = vec({0.01, 0.04, 0.09});
  const VecX mu = vec({0.1, 0.2, -0.3});
  const VecX conv = vec({0.5, 1.0, 0.25});
  const FactorModel m = diag_model(d);

  KellyConfig cfg; // defaults (quarter-Kelly, max_gross 1.0)
  const KellyWeights a = kelly_size(mu, m, conv, cfg);
  const KellyWeights b = kelly_size(mu, m, conv, cfg);

  ASSERT_EQ(a.weights.size(), b.weights.size());
  for (Eigen::Index i = 0; i < a.weights.size(); ++i) {
    EXPECT_EQ(a.weights[i], b.weights[i]); // exact, element-wise
  }
  EXPECT_EQ(a.gross, b.gross);
  EXPECT_EQ(a.scale_applied, b.scale_applied);
}

} // namespace atxtest_risk_kelly_sizing_math_test
