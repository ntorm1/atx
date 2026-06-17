// risk_factor_model_test.cpp — P4-7a: FactorModel factored-covariance apply-math.
//
// FactorModel stores a Barra-style FACTORED covariance V = X F Xᵀ + D
//   X : M×K exposures, F : K×K factor covariance (SPD), D : M specific variances
// and applies it WITHOUT ever materializing the dense M×M V:
//   risk(w)               = wᵀ V w   = (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i²   (O(MK+K²))
//   apply_inverse(in,out) = V⁻¹·in   via Woodbury                     (O(MK+K³))
//   neutralize(signal)    = s − X (XᵀX)⁻¹ Xᵀ s  (factor residualization, in place)
// plus the carried fit window [fit_begin, fit_end).
//
// This is P4-7a — the orchestrator's split of plan-P4-7; the per-date WLS that
// ESTIMATES X, F, D (FactorModelBuilder) is P4-7b. So here X, F, D are GIVEN.
//
// Coverage (plan §8 P4-7, apply-math subset):
//   * K=1, M=3: dense V = X*F*Xᵀ + diag(D) built IN THE TEST; risk(w) == wᵀVw.
//   * K=2, M=4: same cross-check against a dense V.
//   * apply_inverse round-trips: V·(V⁻¹x) ≈ x and V⁻¹·(V·x) ≈ x (within ~1e-9).
//   * neutralize orthogonality: after neutralize, Xᵀ·s ≈ 0 per factor column.
//   * Boundary: a zero specific-variance instrument (D_i=0 → floored → V PD,
//     apply_inverse finite/no NaN).
//   * fit window: create([a,b)); accessors return them.
//   * Construction Err: F not K×K, D length != M, fit_begin>=fit_end, non-SPD F.

#include <cmath>   // std::isnan, std::isfinite
#include <cstdint> // fixed-width
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_risk_factor_model_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::FactorModel;
using atx::engine::risk::kNeutralizeRidge;
using atx::engine::risk::kSpecificVarFloor;

// Dense V = X F Xᵀ + diag(D), M×M. TEST-ONLY cross-check oracle; production never
// materializes this. D is floored the same way the model floors it, so the dense V
// matches the model's effective covariance even for a zero-specific-variance row.
[[nodiscard]] MatX dense_v(const MatX &x, const MatX &f, const VecX &d) {
  MatX v = x * f * x.transpose();
  for (Eigen::Index i = 0; i < d.size(); ++i) {
    const f64 di = d[i] < kSpecificVarFloor ? kSpecificVarFloor : d[i];
    v(i, i) += di;
  }
  return v;
}

// Construct a FactorModel from raw column-major buffers (copies into MatX/VecX).
[[nodiscard]] FactorModel make_model(const MatX &x, const MatX &f, const VecX &d, usize fb = 0U,
                                     usize fe = 10U) {
  auto r = FactorModel::create(x, f, d, fb, fe);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// ===========================================================================
//  K=1 analytic V + risk cross-check
// ===========================================================================
TEST(RiskFactorModel, OneFactorRiskMatchesDenseQuadratic) {
  // M=3, K=1.
  MatX x(3, 1);
  x << 1.0, 0.5, -2.0;
  MatX f(1, 1);
  f << 0.04;
  VecX d(3);
  d << 0.10, 0.20, 0.05;

  const FactorModel m = make_model(x, f, d);
  EXPECT_EQ(m.n_instruments(), 3U);
  EXPECT_EQ(m.n_factors(), 1U);

  const MatX v = dense_v(x, f, d);
  const std::vector<std::vector<f64>> ws = {
      {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {1.0, -1.0, 0.5}, {-0.3, 0.7, 1.2}};
  for (const auto &wv : ws) {
    Eigen::Map<const VecX> w(wv.data(), 3);
    const f64 expected = (w.transpose() * v * w)(0, 0);
    EXPECT_NEAR(m.risk(std::span<const f64>(wv.data(), wv.size())), expected, 1e-12);
  }
}

// ===========================================================================
//  K=2, M=4 risk cross-check vs dense
// ===========================================================================
TEST(RiskFactorModel, TwoFactorRiskMatchesDenseQuadratic) {
  MatX x(4, 2);
  x << 1.0, 0.0, 0.5, 1.0, -1.0, 0.5, 0.2, -0.7;
  MatX f(2, 2);
  f << 0.05, 0.01, 0.01, 0.03; // SPD
  VecX d(4);
  d << 0.10, 0.15, 0.08, 0.20;

  const FactorModel m = make_model(x, f, d);
  EXPECT_EQ(m.n_instruments(), 4U);
  EXPECT_EQ(m.n_factors(), 2U);

  const MatX v = dense_v(x, f, d);
  const std::vector<std::vector<f64>> ws = {
      {1.0, 0.0, 0.0, 0.0}, {0.25, -0.25, 0.5, -0.5}, {1.3, -0.4, 0.2, 0.9}};
  for (const auto &wv : ws) {
    Eigen::Map<const VecX> w(wv.data(), 4);
    const f64 expected = (w.transpose() * v * w)(0, 0);
    EXPECT_NEAR(m.risk(std::span<const f64>(wv.data(), wv.size())), expected, 1e-12);
  }
}

// ===========================================================================
//  apply_inverse round-trips with the dense V (both directions)
// ===========================================================================
TEST(RiskFactorModel, ApplyInverseRoundTripsToIdentity) {
  MatX x(4, 2);
  x << 1.0, 0.0, 0.5, 1.0, -1.0, 0.5, 0.2, -0.7;
  MatX f(2, 2);
  f << 0.05, 0.01, 0.01, 0.03;
  VecX d(4);
  d << 0.10, 0.15, 0.08, 0.20;

  const FactorModel m = make_model(x, f, d);
  const MatX v = dense_v(x, f, d);

  const std::vector<f64> xv = {1.0, -2.0, 0.5, 3.0};
  std::vector<f64> yv(4, 0.0);
  m.apply_inverse(std::span<const f64>(xv.data(), xv.size()), std::span<f64>(yv.data(), yv.size()));

  // Forward: V·(V⁻¹x) ≈ x.
  Eigen::Map<const VecX> y(yv.data(), 4);
  const VecX vy = v * y;
  for (Eigen::Index i = 0; i < 4; ++i) {
    EXPECT_NEAR(vy[i], xv[static_cast<usize>(i)], 1e-9);
  }

  // Reverse: V⁻¹·(V·x) ≈ x.
  Eigen::Map<const VecX> xm(xv.data(), 4);
  const VecX vx = v * xm;
  std::vector<f64> vxv(vx.data(), vx.data() + vx.size());
  std::vector<f64> back(4, 0.0);
  m.apply_inverse(std::span<const f64>(vxv.data(), vxv.size()),
                  std::span<f64>(back.data(), back.size()));
  for (usize i = 0; i < 4U; ++i) {
    EXPECT_NEAR(back[i], xv[i], 1e-9);
  }
}

// ===========================================================================
//  neutralize: residual is orthogonal to every factor column (Xᵀ s ≈ 0)
// ===========================================================================
TEST(RiskFactorModel, NeutralizeResidualOrthogonalToFactors) {
  MatX x(5, 2);
  x << 1.0, 0.0, 0.5, 1.0, -1.0, 0.5, 0.2, -0.7, 0.9, 0.3;
  MatX f(2, 2);
  f << 0.05, 0.01, 0.01, 0.03;
  VecX d(5);
  d << 0.10, 0.15, 0.08, 0.20, 0.12;

  const FactorModel m = make_model(x, f, d);

  std::vector<f64> s = {1.0, -2.0, 0.5, 3.0, -1.5};
  m.neutralize(std::span<f64>(s.data(), s.size()));

  Eigen::Map<const VecX> sm(s.data(), 5);
  const VecX xts = x.transpose() * sm; // K-vector of column·residual dots
  for (Eigen::Index k = 0; k < xts.size(); ++k) {
    EXPECT_NEAR(xts[k], 0.0, 1e-9);
  }
}

// ===========================================================================
//  Boundary: a zero specific-variance instrument is floored → V still PD,
//  apply_inverse stays finite (no NaN/inf).
// ===========================================================================
TEST(RiskFactorModel, ZeroSpecificVarianceIsFlooredAndInvertible) {
  MatX x(3, 1);
  x << 1.0, 0.5, -2.0;
  MatX f(1, 1);
  f << 0.04;
  VecX d(3);
  d << 0.0, 0.10, 0.05; // first instrument has zero idiosyncratic variance

  const FactorModel m = make_model(x, f, d);

  const std::vector<f64> in = {1.0, -1.0, 2.0};
  std::vector<f64> out(3, 0.0);
  m.apply_inverse(std::span<const f64>(in.data(), in.size()),
                  std::span<f64>(out.data(), out.size()));
  for (const f64 o : out) {
    EXPECT_TRUE(std::isfinite(o));
  }

  // Round-trips against the floored dense V too. The flooring leaves a 1e-12
  // diagonal entry, so V is intentionally ill-conditioned (cond ~1e12) and the
  // round-trip carries the expected ~1e-4 relative error — a loose tol here just
  // confirms the inverse is the genuine V⁻¹, not a coincidence; the finiteness
  // assertion above is the real boundary check.
  const MatX v = dense_v(x, f, d);
  Eigen::Map<const VecX> y(out.data(), 3);
  const VecX vy = v * y;
  for (usize i = 0; i < 3U; ++i) {
    EXPECT_NEAR(vy[i], in[i], 1e-4);
  }
}

// ===========================================================================
//  fit window is carried through to the accessors
// ===========================================================================
TEST(RiskFactorModel, FitWindowAccessors) {
  MatX x(2, 1);
  x << 1.0, -1.0;
  MatX f(1, 1);
  f << 0.04;
  VecX d(2);
  d << 0.1, 0.1;

  const FactorModel m = make_model(x, f, d, 7U, 42U);
  EXPECT_EQ(m.fit_begin(), 7U);
  EXPECT_EQ(m.fit_end(), 42U);
}

// ===========================================================================
//  Construction errors: dim mismatches, empty window, non-SPD F
// ===========================================================================
TEST(RiskFactorModel, CreateRejectsFactorCovDimMismatch) {
  MatX x(3, 2);
  x << 1.0, 0.0, 0.5, 1.0, -1.0, 0.5;
  MatX f(1, 1); // should be 2×2
  f << 0.04;
  VecX d(3);
  d << 0.1, 0.1, 0.1;
  EXPECT_FALSE(FactorModel::create(x, f, d, 0U, 1U).has_value());
}

TEST(RiskFactorModel, CreateRejectsSpecificVarLengthMismatch) {
  MatX x(3, 1);
  x << 1.0, 0.5, -2.0;
  MatX f(1, 1);
  f << 0.04;
  VecX d(2); // should be length 3
  d << 0.1, 0.1;
  EXPECT_FALSE(FactorModel::create(x, f, d, 0U, 1U).has_value());
}

TEST(RiskFactorModel, CreateRejectsEmptyFitWindow) {
  MatX x(2, 1);
  x << 1.0, -1.0;
  MatX f(1, 1);
  f << 0.04;
  VecX d(2);
  d << 0.1, 0.1;
  EXPECT_FALSE(FactorModel::create(x, f, d, 5U, 5U).has_value()); // begin == end
  EXPECT_FALSE(FactorModel::create(x, f, d, 6U, 5U).has_value()); // begin > end
}

TEST(RiskFactorModel, CreateRejectsNonSpdFactorCov) {
  MatX x(3, 2);
  x << 1.0, 0.0, 0.5, 1.0, -1.0, 0.5;
  MatX f(2, 2);
  f << 1.0, 2.0, 2.0, 1.0; // symmetric but indefinite (eigenvalues 3, -1)
  VecX d(3);
  d << 0.1, 0.1, 0.1;
  EXPECT_FALSE(FactorModel::create(x, f, d, 0U, 1U).has_value());
}

// kNeutralizeRidge is exposed for callers / tests to reason about the guard scale.
TEST(RiskFactorModel, RidgeConstantIsTiny) {
  EXPECT_GT(kNeutralizeRidge, 0.0);
  EXPECT_LT(kNeutralizeRidge, 1e-6);
}

// risk() accumulates g_k into a fixed K-stack buffer (kMaxFactorsStack == 256), so a
// model with K > that bound would overrun it in a release build where the in-risk()
// ATX_ASSERT is compiled out. create() must reject K > kMaxFactorsStack up front; K=257
// (one past the bound) → Err. F = I_257 is SPD; M is kept tiny since only K is checked.
TEST(RiskFactorModel, CreateRejectsFactorCountAboveStackBound) {
  constexpr Eigen::Index kOverBound = 257; // kMaxFactorsStack (256) + 1
  MatX x = MatX::Zero(2, kOverBound);
  x(0, 0) = 1.0; // nonzero so X is not all-zero, but K is what triggers the reject
  const MatX f = MatX::Identity(kOverBound, kOverBound);
  VecX d(2);
  d << 0.1, 0.1;
  EXPECT_FALSE(FactorModel::create(x, f, d, 0U, 1U).has_value());
}


}  // namespace atxtest_risk_factor_model_test
