// risk_eigen_adjust_test.cpp — S8.3: Monte-Carlo eigenfactor risk adjustment.
//
// eigen_adjust(F, sims, amplify, seed) cleans a K×K factor covariance F by the
// Menchero-Wang-Orr (MWO) procedure: it eigendecomposes F = U·diag(D)·Uᵀ (treating
// that as truth), SIMULATES `sims` factor-return histories drawn from N(0, D) in the
// eigenbasis, re-estimates each simulated covariance, projects it back onto the
// original eigenbasis, and measures the per-eigenfactor sampling bias
//   v(k) = (1/M) Σ_m √(D̃_m(k)/D(k)) .
// It then rescales each eigenvariance D̂(k) = γ(k)²·D(k), γ(k) = a·(v(k)−1)+1, and
// rotates back F̂ = U·diag(D̂)·Uᵀ. PSD-preserving (γ²>0). a = `amplify` (default 1.0).
//
// Coverage (plan §4 / §5 task S8.3 acceptance):
//   * Identity at a=0: γ≡1 ⇒ F̂ ≡ F (within tight tolerance).
//   * Small-eigenfactor correction: a clear large/small eigenstructure ⇒ the SMALLEST
//     eigenfactor's measured v(k) > 1 and its eigenvariance is INFLATED; F̂ is SPD.
//   * Seed determinism (the invariant crux): same seed ⇒ BYTE-IDENTICAL F̂; two
//     different seeds ⇒ F̂ differs.
//   * SPD preserved for a non-trivial a (a=1.0).

#include <cmath>   // std::sqrt, std::isfinite (ill-conditioned finiteness check)
#include <cstring> // std::memcmp (byte-identity seed determinism check)
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/decompose.hpp" // symmetric_eig (eigenstructure oracle)
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/eigen_adjust.hpp"

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::eigen_adjust;

// Build a K×K SPD matrix F = U·diag(d)·Uᵀ from a given eigen-spectrum `d` and an
// orthonormal basis derived from a fixed non-trivial seed matrix (QR of a Vandermonde-
// like matrix) so the eigenvectors are NOT axis-aligned (a genuine rotation).
[[nodiscard]] MatX make_spd_from_spectrum(const std::vector<f64> &d) {
  const Eigen::Index k = static_cast<Eigen::Index>(d.size());
  MatX a(k, k);
  for (Eigen::Index i = 0; i < k; ++i) {
    for (Eigen::Index j = 0; j < k; ++j) {
      // A deterministic, well-conditioned, non-symmetric seed matrix.
      a(i, j) =
          1.0 / (1.0 + static_cast<f64>(i) + 2.0 * static_cast<f64>(j)) + ((i == j) ? 1.0 : 0.0);
    }
  }
  const Eigen::HouseholderQR<MatX> qr(a);
  const MatX u = qr.householderQ() * MatX::Identity(k, k); // orthonormal basis
  VecX dv(k);
  for (Eigen::Index i = 0; i < k; ++i) {
    dv[i] = d[static_cast<usize>(i)];
  }
  MatX f = u * dv.asDiagonal() * u.transpose();
  return 0.5 * (f + f.transpose()); // re-symmetrize against round-off
}

// Smallest eigenvalue of a symmetric matrix (SPD test: > 0).
[[nodiscard]] f64 min_eig(const MatX &m) {
  const auto eig = atx::core::linalg::symmetric_eig(m);
  EXPECT_TRUE(eig.has_value());
  return eig->values[0]; // ascending
}

// ===========================================================================
//  Identity at a=0: γ(k)=0·(v−1)+1 ≡ 1 ⇒ D̂ ≡ D ⇒ F̂ ≡ F. The simulation still
//  runs (sims>0) but the amplify gate zeroes the bias, so the result is F to
//  round-off (only the eig-decompose / re-rotate path touches it).
// ===========================================================================
TEST(EigenAdjust, AmplifyZeroIsIdentity) {
  const MatX f = make_spd_from_spectrum({0.01, 0.04, 0.25, 1.0});
  const auto adj = eigen_adjust(f, /*sims=*/200U, /*amplify=*/0.0, /*seed=*/42U);
  ASSERT_TRUE(adj.has_value()) << (adj ? "" : adj.error().to_string());
  const MatX &fhat = *adj;
  ASSERT_EQ(fhat.rows(), f.rows());
  ASSERT_EQ(fhat.cols(), f.cols());
  for (Eigen::Index i = 0; i < f.rows(); ++i) {
    for (Eigen::Index j = 0; j < f.cols(); ++j) {
      EXPECT_NEAR(fhat(i, j), f(i, j), 1e-12) << "(" << i << "," << j << ")";
    }
  }
}

// ===========================================================================
//  Small-eigenfactor correction: with a wide eigen-spread the SMALLEST eigenfactor
//  is under-forecast in sampling (its simulated bias v(k) > 1). The adjustment must
//  INFLATE that eigenvariance (D̂_small > D_small) and keep F̂ SPD.
// ===========================================================================
TEST(EigenAdjust, SmallEigenfactorIsInflatedAndSpd) {
  const std::vector<f64> spectrum{0.01, 0.04, 0.25, 1.0}; // ascending, wide spread
  const MatX f = make_spd_from_spectrum(spectrum);
  const auto eig0 = atx::core::linalg::symmetric_eig(f);
  ASSERT_TRUE(eig0.has_value());
  const VecX d0 = eig0->values; // ascending; d0[0] is the smallest

  const auto adj = eigen_adjust(f, /*sims=*/2000U, /*amplify=*/1.0, /*seed=*/7U);
  ASSERT_TRUE(adj.has_value()) << (adj ? "" : adj.error().to_string());
  const MatX &fhat = *adj;

  // Recover D̂ by projecting F̂ onto the ORIGINAL eigenbasis (D̂(k) = uₖᵀ F̂ uₖ).
  const MatX &u = eig0->vectors;
  const MatX dhat_full = u.transpose() * fhat * u;

  // The smallest eigenfactor (index 0) must be inflated by the correction.
  const f64 d_small = d0[0];
  const f64 dhat_small = dhat_full(0, 0);
  EXPECT_GT(dhat_small, d_small) << "smallest eigenvariance must be inflated";

  // Implied v(k) for the smallest eigenfactor: γ = √(D̂/D), v = (γ−1)/a + 1 = γ at a=1.
  const f64 gamma_small = std::sqrt(dhat_small / d_small);
  EXPECT_GT(gamma_small, 1.0) << "smallest-eigenfactor simulated bias v(k) > 1";

  // F̂ stays symmetric positive-definite.
  EXPECT_GT(min_eig(fhat), 0.0);
}

// ===========================================================================
//  Seed determinism (the invariant crux). Same (F, sims, a, seed) ⇒ BYTE-IDENTICAL
//  F̂ run-to-run; two DIFFERENT seeds ⇒ a different F̂ (the simulation is the only
//  RNG site and it is fully seeded).
// ===========================================================================
TEST(EigenAdjust, SameSeedByteIdenticalDifferentSeedDiffers) {
  const MatX f = make_spd_from_spectrum({0.02, 0.1, 0.5, 1.0, 2.0});
  const usize sims = 500U;
  const f64 a = 1.0;

  const auto r1 = eigen_adjust(f, sims, a, /*seed=*/12345U);
  const auto r2 = eigen_adjust(f, sims, a, /*seed=*/12345U);
  const auto r3 = eigen_adjust(f, sims, a, /*seed=*/67890U);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r3.has_value());

  ASSERT_EQ(r1->size(), r2->size());
  ASSERT_EQ(r1->size(), r3->size());
  const auto bytes = static_cast<std::size_t>(r1->size()) * sizeof(f64);

  // Same seed ⇒ byte-identical (memcmp of the raw f64 storage).
  EXPECT_EQ(std::memcmp(r1->data(), r2->data(), bytes), 0)
      << "same seed must produce a byte-identical F̂";

  // Different seed ⇒ at least one element differs.
  EXPECT_NE(std::memcmp(r1->data(), r3->data(), bytes), 0)
      << "a different seed must produce a different F̂";
}

// ===========================================================================
//  SPD preserved for a non-trivial amplify (a=1.0) on a moderately ill-conditioned
//  spectrum: the rescaled F̂ must still factor (min eigenvalue > 0).
// ===========================================================================
TEST(EigenAdjust, SpdPreservedAtUnitAmplify) {
  const MatX f = make_spd_from_spectrum({0.005, 0.05, 0.3, 0.8, 1.5, 3.0});
  const auto adj = eigen_adjust(f, /*sims=*/1000U, /*amplify=*/1.0, /*seed=*/99U);
  ASSERT_TRUE(adj.has_value()) << (adj ? "" : adj.error().to_string());
  EXPECT_GT(min_eig(*adj), 0.0);
  // Cholesky must succeed (FactorModel::create's SPD gate).
  Eigen::LLT<MatX> llt(*adj);
  EXPECT_EQ(llt.info(), Eigen::Success);
}

// ===========================================================================
//  Ill-conditioned F: a very wide eigenvalue spread (~1e-8 … 1.0, several near-
//  degenerate small directions) stresses the round-off guard — the projection
//  ûₖᵀ F ûₖ can round NEGATIVE for a tiny eigendirection, which an unfloored √(·)
//  would turn into NaN and propagate into F̂. Pin: every F̂ element is FINITE and
//  F̂ stays SPD (Cholesky succeeds / min eigenvalue > 0).
// ===========================================================================
TEST(EigenAdjust, IllConditionedFStaysFiniteAndSpd) {
  // Spectrum spanning ~8 orders of magnitude with clustered tiny directions.
  const MatX f = make_spd_from_spectrum({1e-8, 2e-8, 5e-8, 1e-4, 0.01, 1.0});
  const auto adj = eigen_adjust(f, /*sims=*/2000U, /*amplify=*/1.0, /*seed=*/2024U);
  ASSERT_TRUE(adj.has_value()) << (adj ? "" : adj.error().to_string());
  const MatX &fhat = *adj;

  // No NaN / Inf anywhere in F̂ (the numerator-floor guard's contract).
  for (Eigen::Index i = 0; i < fhat.rows(); ++i) {
    for (Eigen::Index j = 0; j < fhat.cols(); ++j) {
      EXPECT_TRUE(std::isfinite(fhat(i, j))) << "non-finite at (" << i << "," << j << ")";
    }
  }
  // SPD preserved (min eigenvalue > 0; Cholesky succeeds).
  EXPECT_GT(min_eig(fhat), 0.0);
  Eigen::LLT<MatX> llt(fhat);
  EXPECT_EQ(llt.info(), Eigen::Success);
}

// ===========================================================================
//  Boundary: sims == 0 is a no-op identity (the build hook's default; the adjustment
//  is opt-in). Returns F unchanged with no RNG draws.
// ===========================================================================
TEST(EigenAdjust, ZeroSimsIsNoOpIdentity) {
  const MatX f = make_spd_from_spectrum({0.1, 0.5, 1.0});
  const auto adj = eigen_adjust(f, /*sims=*/0U, /*amplify=*/1.0, /*seed=*/3U);
  ASSERT_TRUE(adj.has_value());
  const MatX &fhat = *adj;
  for (Eigen::Index i = 0; i < f.rows(); ++i) {
    for (Eigen::Index j = 0; j < f.cols(); ++j) {
      EXPECT_EQ(fhat(i, j), f(i, j));
    }
  }
}

} // namespace
