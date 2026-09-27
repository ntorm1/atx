#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "atx/core/linalg/linalg.hpp" // MatX
#include "atx/engine/eval/breadth.hpp"

namespace atxtest_eval_breadth_test {

using namespace atx::engine::eval;
using atx::core::linalg::MatX;

// K orthogonal equal-variance bets: cov = c·I_K has K equal eigenvalues, so the
// participation ratio is exactly K — full breadth, every direction counts.
TEST(Breadth, OrthogonalEqualVarIsK) {
  const Eigen::Index K = 4;
  const MatX cov = 2.0 * MatX::Identity(K, K);
  EXPECT_NEAR(effective_breadth(cov), 4.0, 1e-9);
}

// K identical bets: cov = c·(ones K×K) is rank-1 (one nonzero eigenvalue = K·c,
// the rest 0), so N_eff collapses to 1 — K perfectly-correlated copies are a
// single independent draw. Looser tolerance: the eigensolver emits tiny negative
// eigenvalues for the (K-1) null directions, which the clamp absorbs.
TEST(Breadth, IdenticalBetsIsOne) {
  const Eigen::Index K = 5;
  const MatX cov = 0.7 * MatX::Ones(K, K);
  EXPECT_NEAR(effective_breadth(cov), 1.0, 1e-6);
}

// IR = IC·√breadth on a planted-orthogonal spectrum: cov = I_K ⇒ N_eff = K, and
// the decomposition multiplies the caller's IC by √K. Also pins the passthrough
// fields (effective_n, ic) so the struct wiring is exercised end-to-end.
TEST(Breadth, IrEqualsIcSqrtBreadth) {
  const Eigen::Index K = 9;
  const MatX cov = MatX::Identity(K, K);
  const atx::f64 ic = 0.05;
  const BreadthResult r = breadth_decomposition(cov, ic);
  EXPECT_NEAR(r.effective_n, 9.0, 1e-9);
  EXPECT_EQ(r.ic, 0.05);
  EXPECT_NEAR(r.ir, 0.05 * std::sqrt(9.0), 1e-6);
}

// Known-eigenvalue intermediate: a diagonal cov reads its eigenvalues straight
// off the diagonal, so N_eff = (Σλ)²/Σλ² is hand-computable. λ = {1,2,3}:
//   Σλ = 6, Σλ² = 1+4+9 = 14 ⇒ N_eff = 36/14 = 2.571428...
TEST(Breadth, KnownEigenvalueIntermediate) {
  MatX cov = MatX::Zero(3, 3);
  cov(0, 0) = 1.0;
  cov(1, 1) = 2.0;
  cov(2, 2) = 3.0;
  const atx::f64 expected = (6.0 * 6.0) / 14.0; // (Σλ)² / Σλ²
  EXPECT_NEAR(effective_breadth(cov), expected, 1e-9);
}

// Determinism: the same covariance yields a byte-identical breadth and identical
// BreadthResult fields across two independent calls (no RNG, order-fixed).
TEST(Breadth, TwoRunsEqual) {
  MatX cov = MatX::Zero(3, 3);
  cov(0, 0) = 1.5;
  cov(1, 1) = 0.5;
  cov(2, 2) = 4.0;
  cov(0, 1) = cov(1, 0) = 0.25; // a non-diagonal symmetric PSD perturbation
  EXPECT_EQ(effective_breadth(cov), effective_breadth(cov));
  const BreadthResult a = breadth_decomposition(cov, 0.03);
  const BreadthResult b = breadth_decomposition(cov, 0.03);
  EXPECT_EQ(a.effective_n, b.effective_n);
  EXPECT_EQ(a.ic, b.ic);
  EXPECT_EQ(a.ir, b.ir);
}

// Degenerate: a zero matrix has no variance and no bet to count — documented as
// N_eff = 0 (the 0/0 guard), which must NOT leak a NaN into the report.
TEST(Breadth, ZeroMatrixIsZero) {
  const MatX cov = MatX::Zero(4, 4);
  const atx::f64 n_eff = effective_breadth(cov);
  EXPECT_EQ(n_eff, 0.0);
  EXPECT_FALSE(std::isnan(n_eff));
  // The decomposition then yields a finite, zero IR regardless of the IC.
  const BreadthResult r = breadth_decomposition(cov, 0.1);
  EXPECT_EQ(r.ir, 0.0);
}

// A single 1×1 covariance is one bet: N_eff = (λ)²/λ² = 1 for any positive λ.
TEST(Breadth, SingleNameIsOne) {
  MatX cov(1, 1);
  cov(0, 0) = 3.3;
  EXPECT_NEAR(effective_breadth(cov), 1.0, 1e-12);
}

TEST(Breadth, TraceIdentityMatchesIndependentEigenRuleOnFullAndDeficientRank) {
  for (const Eigen::Index observations : {7, 37}) {
    MatX observations_by_name(observations, 19);
    for (Eigen::Index t = 0; t < observations; ++t)
      for (Eigen::Index a = 0; a < 19; ++a)
        observations_by_name(t, a) = std::sin(static_cast<double>((t + 1) * (a + 3))) +
                                    0.1 * static_cast<double>(a % 3);
    const MatX cov = observations_by_name.transpose() * observations_by_name;
    const auto reference = effective_breadth(cov, BreadthRule::LegacyClippedEigenV1);
    const auto fast = breadth_decomposition(cov, 0.02);
    EXPECT_EQ(fast.rule, BreadthRule::PsdTraceV2);
    EXPECT_NEAR(fast.effective_n, reference, 1e-10);
  }
}

TEST(Breadth, TraceIdentityIsScaleInvariantAtFiniteExtremes) {
  MatX cov = MatX::Identity(7, 7);
  cov(0, 1) = cov(1, 0) = 0.5;
  const auto reference = effective_breadth(cov, BreadthRule::LegacyClippedEigenV1);
  for (const double scale : {1e-280, 1.0, 1e280}) {
    const MatX scaled = scale * cov;
    EXPECT_NEAR(effective_breadth(scaled), reference, 1e-10);
  }
  MatX indefinite = MatX::Zero(2, 2);
  indefinite(0, 0) = -1.0; indefinite(1, 1) = 2.0;
  EXPECT_DOUBLE_EQ(effective_breadth(indefinite, BreadthRule::LegacyClippedEigenV1), 1.0);
}

TEST(Breadth, TraceRuleRejectsMalformedCovarianceBeforeReduction) {
  EXPECT_EQ(effective_breadth(MatX::Zero(0, 0)), 0.0);
  EXPECT_EQ(effective_breadth(MatX::Zero(2, 3)), 0.0);
  MatX cov = MatX::Identity(2, 2);
  cov(0, 1) = 0.25;
  EXPECT_EQ(effective_breadth(cov), 0.0);
  cov(0, 1) = std::numeric_limits<double>::infinity();
  EXPECT_EQ(effective_breadth(cov), 0.0);
}

} // namespace atxtest_eval_breadth_test
