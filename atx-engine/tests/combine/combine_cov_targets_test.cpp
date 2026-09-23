// combine_cov_targets_test.cpp — Lane 5: covariance shrinkage targets.
//
//   * LW2020 nonlinear (c<1 and the c>1 null-space variant) matches an independent
//     numpy port of the authors' Matlab code to 1e-8 (fixtures/cov_targets_fixture.inc).
//   * LW2003 constant-correlation matches the covCor.m port (Σ and δ).
//   * LW2004 identity path equals combiner.hpp's in-tree LW intensity.
//   * On a spiked model LW2020 beats LW2004 in Frobenius loss.
//   * Validation: T too small / non-finite input → Err.
//
// Suite: CombineCovTargets

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/cov_targets.hpp"

namespace atx_test_l5_combine_cov_targets {

#include "fixtures/cov_targets_fixture.inc"

using atx::f64;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
namespace cb = atx::engine::combine;

// Mirror of gen_cov_targets_fixture.py::lcg_matrix (exact integer LCG).
MatX lcg_matrix(std::uint64_t seed, Eigen::Index t, Eigen::Index n) {
  std::uint64_t s = seed;
  MatX out(t, n);
  for (Eigen::Index r = 0; r < t; ++r) {
    for (Eigen::Index c = 0; c < n; ++c) {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      const f64 u = (static_cast<f64>(s >> 11U) / 9007199254740992.0) * 2.0 - 1.0;
      out(r, c) = u * (1.0 + 0.25 * static_cast<f64>(c));
    }
  }
  const VecX f = out.col(0);
  for (Eigen::Index c = 1; c < n; ++c) {
    out.col(c) += 0.5 * f;
  }
  return out;
}

void expect_matches(const MatX &got, const double *ref, Eigen::Index n, f64 tol) {
  ASSERT_EQ(got.rows(), n);
  ASSERT_EQ(got.cols(), n);
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      EXPECT_NEAR(got(i, j), ref[i * n + j], tol) << "(" << i << "," << j << ")";
    }
  }
}

// Gaussian draws via Box-Muller over the same LCG (deterministic).
struct Gauss {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
  }
  f64 next() {
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

TEST(CombineCovTargets, Lw2020MatchesReferenceWhenCBelowOne) {
  const MatX x = lcg_matrix(12345U, 60, 8);
  const auto r = cb::shrink_nonlinear_lw2020(x);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  expect_matches(*r, kLw2020A, 8, 1e-8);
}

TEST(CombineCovTargets, Lw2020MatchesReferenceNullSpaceVariantWhenCAboveOne) {
  const MatX x = lcg_matrix(777U, 24, 30);
  const auto r = cb::shrink_nonlinear_lw2020(x);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  expect_matches(*r, kLw2020B, 30, 1e-8);
  // The null-space variant must still be positive definite.
  const Eigen::SelfAdjointEigenSolver<MatX> es(*r);
  EXPECT_GT(es.eigenvalues().minCoeff(), 0.0);
}

TEST(CombineCovTargets, Lw2003ConstCorrMatchesReference) {
  const MatX x = lcg_matrix(12345U, 60, 8);
  const auto r = cb::shrink_lw_const_corr(x);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_NEAR(r->delta, kLw2003CcADelta, 1e-12);
  expect_matches(r->sigma, kLw2003CcA, 8, 1e-12);
}

TEST(CombineCovTargets, Lw2004IdentityMatchesInTreeIntensity) {
  const MatX x = lcg_matrix(99U, 40, 12);
  const auto r = cb::shrink_lw_identity(x);
  ASSERT_TRUE(r.has_value());
  const MatX xc = x.rowwise() - x.colwise().mean();
  const MatX s = (xc.transpose() * xc) / static_cast<f64>(x.rows());
  const f64 ref = cb::detail::ledoit_wolf_intensity(s, xc);
  EXPECT_NEAR(r->delta, ref, 1e-12);
  EXPECT_GT(r->delta, 0.0);
  EXPECT_LT(r->delta, 1.0);
}

TEST(CombineCovTargets, Lw2020BeatsLw2004OnSpikedModel) {
  constexpr Eigen::Index kN = 100;
  constexpr Eigen::Index kT = 200;
  // Σ = diag(1) with three spikes (25, 10, 5) along the first coordinates.
  VecX sd = VecX::Ones(kN);
  sd[0] = 5.0;
  sd[1] = std::sqrt(10.0);
  sd[2] = std::sqrt(5.0);
  f64 loss_nl = 0.0;
  f64 loss_lin = 0.0;
  for (std::uint64_t seed = 1U; seed <= 3U; ++seed) {
    Gauss g{seed * 7919U};
    MatX x(kT, kN);
    for (Eigen::Index t = 0; t < kT; ++t) {
      for (Eigen::Index i = 0; i < kN; ++i) {
        x(t, i) = sd[i] * g.next();
      }
    }
    const MatX truth = sd.array().square().matrix().asDiagonal();
    const auto nl = cb::shrink_nonlinear_lw2020(x);
    const auto lin = cb::shrink_lw_identity(x);
    ASSERT_TRUE(nl.has_value());
    ASSERT_TRUE(lin.has_value());
    loss_nl += (*nl - truth).squaredNorm();
    loss_lin += (lin->sigma - truth).squaredNorm();
  }
  EXPECT_LT(loss_nl, loss_lin) << "nl=" << loss_nl << " lin=" << loss_lin;
}

TEST(CombineCovTargets, RmtClipPreservesVariancesAndIsSymmetric) {
  const MatX x = lcg_matrix(4242U, 80, 20);
  const auto r = cb::shrink_rmt_clip(x);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const auto s = cb::sample_covariance(x);
  ASSERT_TRUE(s.has_value());
  for (Eigen::Index i = 0; i < 20; ++i) {
    EXPECT_NEAR((*r)(i, i), (*s)(i, i), 1e-9 * (*s)(i, i));
  }
  EXPECT_LT((*r - r->transpose()).cwiseAbs().maxCoeff(), 1e-12);
}

TEST(CombineCovTargets, EstimateCovarianceDispatchesEveryTarget) {
  const MatX x = lcg_matrix(5U, 40, 6);
  for (const cb::CovTarget tgt :
       {cb::CovTarget::Sample, cb::CovTarget::LwIdentity, cb::CovTarget::LwConstCorr,
        cb::CovTarget::LwNonlinear, cb::CovTarget::RmtClip}) {
    const auto r = cb::estimate_covariance(x, tgt);
    ASSERT_TRUE(r.has_value()) << static_cast<int>(tgt);
    EXPECT_EQ(r->rows(), 6);
    EXPECT_TRUE(r->allFinite());
  }
}

TEST(CombineCovTargets, RejectsTooFewRowsAndNonFinite) {
  EXPECT_FALSE(cb::shrink_nonlinear_lw2020(lcg_matrix(1U, 12, 4)).has_value());
  EXPECT_FALSE(cb::sample_covariance(MatX::Zero(1, 3)).has_value());
  MatX bad = lcg_matrix(1U, 20, 3);
  bad(3, 1) = std::nan("");
  EXPECT_FALSE(cb::shrink_lw_identity(bad).has_value());
}

} // namespace atx_test_l5_combine_cov_targets
