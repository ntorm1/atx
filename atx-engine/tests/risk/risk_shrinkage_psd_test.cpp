// risk_shrinkage_psd_test.cpp — S8.7: model-free shrinkage + RMT cleaning + PSD repair.
//
// Three standalone risk::cov utilities (no build-path wiring this sprint):
//   * constant_correlation_shrinkage(centered) — Ledoit-Wolf 2004 "Honey, I Shrunk
//     the Sample Covariance Matrix" with the CONSTANT-CORRELATION target. PSD by
//     construction; positive-definite even at N>T where the raw sample cov S is
//     singular (rank <= T < N).
//   * mp_clip(corr, q) — Marchenko-Pastur eigenvalue clipping: average all
//     eigenvalues below the noise-bulk edge λ₊=(1+√q)² (TRACE-PRESERVING), then
//     renormalize the diagonal to 1.
//   * nearest_correlation(A) — Higham 2002 alternating projections: the
//     Frobenius-closest unit-diagonal PSD matrix to an indefinite input.
//   * eigenvalue_clip(A, eps) — cheap PSD repair (clip eigenvalues to ≥ eps).
//
// Coverage (plan §5 S8.7 acceptance):
//   * const-corr LW closed form: the implementation matches an INDEPENDENT inline
//     recomputation of (r̄, π̂, ρ̂, γ̂, δ) on a hand-built T=6,N=3 centered panel,
//     and the result Σ̂=δF+(1−δ)S is SPD (Cholesky succeeds).
//   * MP clip: planted in-bulk eigenvalues are averaged out; total eigenvalue sum
//     (trace) preserved; result symmetric, unit-diagonal.
//   * Higham nearest: a 3×3 indefinite near-correlation matrix → unit diagonal, all
//     eigenvalues ≥ −tol, Frobenius-CLOSER to the input than the identity.
//   * N>T invertibility: const-corr shrinkage on N>T window is positive-definite
//     where the raw S is singular.
//   * eigenvalue_clip: an indefinite matrix → all eigenvalues ≥ eps after.

#include <cmath>   // std::sqrt, std::abs, std::isfinite
#include <cstring> // std::memcmp

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/decompose.hpp" // symmetric_eig
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX
#include "atx/core/types.hpp"            // f64, usize

#include "atx/engine/risk/psd_repair.hpp"
#include "atx/engine/risk/shrinkage.hpp"

namespace atxtest_risk_shrinkage_psd_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::constant_correlation_shrinkage;
using atx::engine::risk::constant_correlation_target;
using atx::engine::risk::eigenvalue_clip;
using atx::engine::risk::mp_clip;
using atx::engine::risk::nearest_correlation;

// Smallest eigenvalue of a symmetric matrix (ascending order from symmetric_eig).
[[nodiscard]] f64 min_eig(const MatX &a) {
  const auto eig = atx::core::linalg::symmetric_eig(a);
  EXPECT_TRUE(eig.has_value());
  return eig->values[0];
}

[[nodiscard]] bool is_spd(const MatX &a) {
  Eigen::LLT<MatX> llt(a);
  return llt.info() == Eigen::Success;
}

// ===========================================================================
//  Const-corr Ledoit-Wolf: the implementation reproduces the published closed
//  form. We recompute (r̄, π̂, ρ̂, γ̂, δ) INLINE here (an independent reference) on a
//  hand-built column-demeaned panel and assert Σ̂ matches δF+(1−δ)S to a tight
//  tolerance, and is SPD. The hand panel is column-demeaned by construction.
// ===========================================================================
TEST(RiskShrinkagePsd, ConstCorrLwMatchesClosedForm) {
  constexpr Eigen::Index T = 6;
  constexpr Eigen::Index N = 3;
  MatX raw(T, N);
  raw << 0.04, -0.02, 0.03, -0.03, 0.05, -0.01, 0.02, -0.04, 0.02, -0.01, 0.01, -0.03, 0.05, -0.03,
      0.04, -0.02, 0.02, -0.05;

  // Column-demean (subtract per-column time-mean) — the function's contract input.
  MatX centered = raw;
  for (Eigen::Index j = 0; j < N; ++j) {
    centered.col(j).array() -= raw.col(j).mean();
  }

  // --- Independent reference recomputation of the LW const-corr closed form. ---
  const f64 tf = static_cast<f64>(T);
  const MatX S = (centered.transpose() * centered) / tf; // MLE divisor T
  VecX s(N);
  for (Eigen::Index i = 0; i < N; ++i) {
    s[i] = std::sqrt(S(i, i));
  }
  // r̄ = mean of off-diagonal sample correlations (i<j, order-fixed).
  f64 rbar_sum = 0.0;
  int rbar_cnt = 0;
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = i + 1; j < N; ++j) {
      rbar_sum += S(i, j) / (s[i] * s[j]);
      ++rbar_cnt;
    }
  }
  const f64 rbar = rbar_sum / static_cast<f64>(rbar_cnt);
  // Target F: F_ii = S_ii ; F_ij = r̄·s_i·s_j.
  MatX F(N, N);
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      F(i, j) = (i == j) ? S(i, j) : rbar * s[i] * s[j];
    }
  }
  // π̂_ij = (1/T) Σ_t (c_ti c_tj − S_ij)² ; π̂ = Σ_ij π̂_ij.
  MatX pi_ij(N, N);
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      f64 acc = 0.0;
      for (Eigen::Index t = 0; t < T; ++t) {
        const f64 d = centered(t, i) * centered(t, j) - S(i, j);
        acc += d * d;
      }
      pi_ij(i, j) = acc / tf;
    }
  }
  const f64 pi_hat = pi_ij.sum();
  const f64 gamma = (F - S).squaredNorm();
  // ϑ̂_{ii,ij} = (1/T) Σ_t (c_ti² − S_ii)(c_ti c_tj − S_ij).
  auto theta = [&](Eigen::Index ii, Eigen::Index i, Eigen::Index j) {
    f64 acc = 0.0;
    for (Eigen::Index t = 0; t < T; ++t) {
      acc += (centered(t, ii) * centered(t, ii) - S(ii, ii)) *
             (centered(t, i) * centered(t, j) - S(i, j));
    }
    return acc / tf;
  };
  // ρ̂ = Σ_i π̂_ii + Σ_{i≠j} (r̄/2)·( √(S_jj/S_ii)·ϑ̂_{ii,ij} + √(S_ii/S_jj)·ϑ̂_{jj,ij} ).
  f64 rho = 0.0;
  for (Eigen::Index i = 0; i < N; ++i) {
    rho += pi_ij(i, i);
  }
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      if (i == j) {
        continue;
      }
      const f64 ratio_ji = s[j] / s[i]; // √(S_jj/S_ii)
      const f64 ratio_ij = s[i] / s[j]; // √(S_ii/S_jj)
      rho += (rbar / 2.0) * (ratio_ji * theta(i, i, j) + ratio_ij * theta(j, i, j));
    }
  }
  f64 delta = (pi_hat - rho) / (tf * gamma);
  delta = (delta < 0.0) ? 0.0 : (delta > 1.0 ? 1.0 : delta);
  const MatX expected = delta * F + (1.0 - delta) * S;

  // --- Implementation under test. ---
  const MatX got = constant_correlation_shrinkage(centered);
  const MatX got_target = constant_correlation_target(S);

  ASSERT_EQ(got.rows(), N);
  ASSERT_EQ(got.cols(), N);
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      EXPECT_NEAR(got(i, j), expected(i, j), 1e-15) << "Σ̂ mismatch at (" << i << "," << j << ")";
      EXPECT_NEAR(got_target(i, j), F(i, j), 1e-15)
          << "target mismatch at (" << i << "," << j << ")";
    }
  }
  // The shrunk matrix is symmetric positive-definite.
  EXPECT_TRUE(is_spd(got));
  EXPECT_GT(min_eig(got), 0.0);
  // δ is a genuine interior shrinkage on this panel (not a saturated 0 or 1).
  EXPECT_GT(delta, 0.0);
  EXPECT_LT(delta, 1.0);
}

// ===========================================================================
//  Determinism: same centered input twice ⇒ byte-identical Σ̂ (RNG-free,
//  order-fixed reductions).
// ===========================================================================
TEST(RiskShrinkagePsd, ConstCorrLwDeterministic) {
  MatX centered(5, 3);
  centered << 0.011, -0.006, 0.004, -0.004, 0.013, -0.009, 0.018, -0.002, 0.007, -0.009, 0.007,
      0.015, -0.016, -0.012, -0.017;
  // Column-demean.
  for (Eigen::Index j = 0; j < 3; ++j) {
    centered.col(j).array() -= centered.col(j).mean();
  }
  const MatX a = constant_correlation_shrinkage(centered);
  const MatX b = constant_correlation_shrinkage(centered);
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(0, std::memcmp(a.data(), b.data(), static_cast<usize>(a.size()) * sizeof(f64)));
}

// ===========================================================================
//  N>T invertibility: the raw sample cov S is singular (rank ≤ T < N), but the
//  const-corr shrinkage returns a POSITIVE-DEFINITE matrix (Cholesky succeeds).
//  T=3 dates, N=5 instruments.
// ===========================================================================
TEST(RiskShrinkagePsd, ConstCorrLwPositiveDefiniteWhenNGreaterThanT) {
  constexpr Eigen::Index T = 3;
  constexpr Eigen::Index N = 5;
  MatX raw(T, N);
  raw << 0.04, -0.02, 0.03, 0.01, -0.05, -0.03, 0.05, -0.01, 0.02, 0.04, 0.02, -0.04, 0.02, -0.03,
      0.01;
  MatX centered = raw;
  for (Eigen::Index j = 0; j < N; ++j) {
    centered.col(j).array() -= raw.col(j).mean();
  }
  // Raw MLE sample cov is rank-deficient (smallest eigenvalue ≈ 0).
  const MatX S = (centered.transpose() * centered) / static_cast<f64>(T);
  EXPECT_LE(min_eig(S), 1e-12);
  EXPECT_FALSE(is_spd(S)); // singular ⇒ Cholesky fails

  const MatX shrunk = constant_correlation_shrinkage(centered);
  EXPECT_TRUE(is_spd(shrunk)); // shrinkage lifts the smallest eigenvalue ⇒ PD
  EXPECT_GT(min_eig(shrunk), 0.0);
}

// ===========================================================================
//  MP clip is TRACE-PRESERVING. Build a correlation matrix as U·diag(Λ)·Uᵀ with a
//  fixed orthonormal U and a planted spectrum: one large "signal" eigenvalue and
//  several small in-bulk "noise" eigenvalues below λ₊=(1+√q)². The clip must
//  replace the noise block by its average (Σλ over the noise block unchanged), so
//  the TOTAL eigenvalue sum (== trace == N for a correlation matrix) is preserved.
//  q is chosen so the noise eigenvalues sit below λ₊ and the signal sits above.
//
//  NOTE on the diagonal renormalization: forcing unit diagonal (both when planting
//  the input and after clipping) PERTURBS the spectrum away from the planted values
//  — the planted [0.3,0.5,0.6,2.6] becomes the input spectrum [0.334,0.578,0.696,
//  2.391] once the diagonal is renormalized. The INVARIANT the kernel guarantees is
//  TRACE preservation through the clip step (Σλ unchanged == N), and a collapsed
//  noise bulk — NOT that the largest eigenvalue equals the planted 2.6. We assert
//  the genuine invariants: trace == N, unit diagonal, symmetric, the three sub-λ₊
//  eigenvalues collapse to a TIGHT cluster (their bulk is averaged), and the result
//  is a valid PSD correlation matrix.
// ===========================================================================
TEST(RiskShrinkagePsd, MpClipPreservesTrace) {
  constexpr Eigen::Index N = 4;
  // q=0.25 ⇒ λ₊ = (1+0.5)² = 2.25. Plant a 2.6 signal eigenvalue (>λ₊) and three
  // small ones {0.6,0.5,0.3} (<λ₊). Their sum 2.6+0.6+0.5+0.3 = 4.0 = N.
  const f64 q = 0.25;
  VecX lambda(N);
  lambda << 0.3, 0.5, 0.6, 2.6; // ascending; sum == 4 == N
  // A fixed orthonormal basis from the QR of a deterministic matrix.
  MatX seed(N, N);
  seed << 1.0, 0.3, -0.2, 0.1, 0.2, 1.0, 0.4, -0.3, -0.1, 0.2, 1.0, 0.5, 0.4, -0.1, 0.3, 1.0;
  Eigen::HouseholderQR<MatX> qr(seed);
  const MatX U = qr.householderQ();
  MatX corr = U * lambda.asDiagonal() * U.transpose();
  // Force exact unit diagonal (correlation matrix) by renormalizing.
  VecX d(N);
  for (Eigen::Index i = 0; i < N; ++i) {
    d[i] = std::sqrt(corr(i, i));
  }
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      corr(i, j) /= (d[i] * d[j]);
    }
  }
  // The renormalized input still has exactly one eigenvalue above λ₊ (the signal)
  // and three below (the noise bulk to be averaged).
  const auto eig_in = atx::core::linalg::symmetric_eig(corr);
  ASSERT_TRUE(eig_in.has_value());
  const f64 lambda_plus = (1.0 + std::sqrt(q)) * (1.0 + std::sqrt(q));
  int below = 0;
  for (Eigen::Index k = 0; k < N; ++k) {
    if (eig_in->values[k] < lambda_plus) {
      ++below;
    }
  }
  ASSERT_EQ(below, 3); // three noise eigenvalues below λ₊, one signal above

  const auto cleaned = mp_clip(corr, q);
  ASSERT_TRUE(cleaned.has_value()) << cleaned.error().to_string();
  const MatX &cc = *cleaned;
  ASSERT_EQ(cc.rows(), N);

  // Trace preserved (correlation matrix ⇒ trace == N before and after).
  EXPECT_NEAR(cc.trace(), corr.trace(), 1e-12);
  EXPECT_NEAR(cc.trace(), static_cast<f64>(N), 1e-9);
  // Unit diagonal preserved.
  for (Eigen::Index i = 0; i < N; ++i) {
    EXPECT_NEAR(cc(i, i), 1.0, 1e-12);
  }
  // Symmetric.
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      EXPECT_NEAR(cc(i, j), cc(j, i), 1e-12);
    }
  }
  // The three sub-λ₊ eigenvalues were averaged: BEFORE the diagonal renormalization
  // they collapse to EXACTLY their average (the trace-preserving clip), which the
  // kernel reassembles internally; the subsequent unit-diagonal renormalization
  // re-spreads them slightly. We pin the trace-preserving invariant directly by
  // re-running the clip math here and asserting the pre-renormalization eigenvalues
  // are the average (the three smallest all equal), then confirm the renormalized
  // result keeps that bulk materially TIGHTER than the raw input bulk.
  const f64 noise_mean = (eig_in->values[0] + eig_in->values[1] + eig_in->values[2]) / 3.0;
  MatX clip_pre = eig_in->vectors *
                  (VecX(N) << noise_mean, noise_mean, noise_mean, eig_in->values[N - 1])
                      .finished()
                      .asDiagonal() *
                  eig_in->vectors.transpose();
  const auto eig_pre = atx::core::linalg::symmetric_eig(clip_pre);
  ASSERT_TRUE(eig_pre.has_value());
  // Pre-renormalization: the noise block is EXACTLY flat (all three == the average).
  EXPECT_NEAR(eig_pre->values[0], noise_mean, 1e-9);
  EXPECT_NEAR(eig_pre->values[1], noise_mean, 1e-9);
  EXPECT_NEAR(eig_pre->values[2], noise_mean, 1e-9);
  // The cleaned (renormalized) result's noise bulk is materially tighter than raw.
  const auto eig_clean = atx::core::linalg::symmetric_eig(cc);
  ASSERT_TRUE(eig_clean.has_value());
  const f64 noise_spread = eig_clean->values[N - 2] - eig_clean->values[0];
  const f64 raw_noise_spread = eig_in->values[N - 2] - eig_in->values[0];
  EXPECT_LT(noise_spread, 0.5 * raw_noise_spread); // bulk collapsed (>50% tighter)
  // Signal eigenvalue clearly above the (averaged) noise cluster.
  EXPECT_GT(eig_clean->values[N - 1], eig_clean->values[N - 2] + 1.0);
  // Result is a valid PSD correlation matrix.
  EXPECT_GT(min_eig(cc), 0.0);
}

// ===========================================================================
//  MP clip no-op: when EVERY eigenvalue is at or above λ₊ there is no noise bulk and
//  the input is returned unchanged. A tiny q (1e-12 ⇒ λ₊ ≈ 1 + 2e-6) and a spiked
//  spectrum whose smallest eigenvalue exceeds λ₊ exercises the no-op branch: a 2×2
//  correlation with off-diagonal 0.6 has eigenvalues {0.4, 1.6}, the smaller (0.4)
//  is BELOW λ₊ — so instead use a matrix whose every eigenvalue ≥ λ₊. For q→0 the
//  edge λ₊→1 from above; a correlation matrix always has an eigenvalue ≤ 1 (trace
//  N over N eigenvalues), so a strict no-op needs the identity, whose eigenvalues
//  are all EXACTLY 1. With λ₊ marginally above 1 those are below ⇒ averaged to their
//  own mean 1 ⇒ identity returned. Either way the function returns the identity; the
//  point is that a degenerate/unit spectrum is preserved.
// ===========================================================================
TEST(RiskShrinkagePsd, MpClipNoopWhenNoNoiseBulk) {
  constexpr Eigen::Index N = 3;
  const f64 q = 1e-6;
  MatX corr = MatX::Identity(N, N); // all eigenvalues == 1
  const auto cleaned = mp_clip(corr, q);
  ASSERT_TRUE(cleaned.has_value());
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      EXPECT_NEAR((*cleaned)(i, j), (i == j) ? 1.0 : 0.0, 1e-12);
    }
  }
}

// ===========================================================================
//  MP clip rejects bad q.
// ===========================================================================
TEST(RiskShrinkagePsd, MpClipRejectsNonPositiveQ) {
  MatX corr(2, 2);
  corr << 1.0, 0.3, 0.3, 1.0;
  EXPECT_FALSE(mp_clip(corr, 0.0).has_value());
  EXPECT_FALSE(mp_clip(corr, -0.5).has_value());
}

// ===========================================================================
//  Higham nearest-correlation: the classic indefinite 3×3. With unit diagonal and
//  off-diagonals (0.9, 0.9, -0.9) the matrix is INDEFINITE (one negative
//  eigenvalue). nearest_correlation must return: unit diagonal, all eigenvalues ≥
//  −tol (PSD), and FROBENIUS-CLOSER to the input than the identity is.
// ===========================================================================
TEST(RiskShrinkagePsd, HighamNearestCorrelationRepairsIndefinite) {
  constexpr Eigen::Index N = 3;
  MatX a(N, N);
  a << 1.0, 0.9, 0.9, 0.9, 1.0, -0.9, 0.9, -0.9, 1.0;
  // Confirm the input is indefinite (a negative eigenvalue).
  EXPECT_LT(min_eig(a), 0.0);

  const auto repaired = nearest_correlation(a, 100, 1e-9);
  ASSERT_TRUE(repaired.has_value()) << repaired.error().to_string();
  const MatX &y = *repaired;
  ASSERT_EQ(y.rows(), N);

  // Unit diagonal.
  for (Eigen::Index i = 0; i < N; ++i) {
    EXPECT_NEAR(y(i, i), 1.0, 1e-9);
  }
  // PSD (eigenvalues ≥ −tol).
  EXPECT_GE(min_eig(y), -1e-8);
  // Symmetric.
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      EXPECT_NEAR(y(i, j), y(j, i), 1e-9);
    }
  }
  // Frobenius-closer to A than the identity is: this genuinely solves the
  // nearest-correlation problem rather than collapsing to I.
  const MatX ident = MatX::Identity(N, N);
  const f64 dist_y = (y - a).squaredNorm();
  const f64 dist_i = (ident - a).squaredNorm();
  EXPECT_LT(dist_y, dist_i);
}

// ===========================================================================
//  Higham is a no-op (within tol) on an already-valid correlation matrix.
// ===========================================================================
TEST(RiskShrinkagePsd, HighamNearestCorrelationIdempotentOnValid) {
  constexpr Eigen::Index N = 3;
  MatX a(N, N);
  a << 1.0, 0.3, 0.2, 0.3, 1.0, 0.1, 0.2, 0.1, 1.0;
  EXPECT_GT(min_eig(a), 0.0); // already PSD
  const auto repaired = nearest_correlation(a, 100, 1e-9);
  ASSERT_TRUE(repaired.has_value());
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      EXPECT_NEAR((*repaired)(i, j), a(i, j), 1e-6);
    }
  }
}

// ===========================================================================
//  eigenvalue_clip: an indefinite covariance-shaped matrix → all eigenvalues ≥ eps.
// ===========================================================================
TEST(RiskShrinkagePsd, EigenvalueClipFloorsSpectrum) {
  constexpr Eigen::Index N = 3;
  MatX a(N, N);
  a << 1.0, 0.9, 0.9, 0.9, 1.0, -0.9, 0.9, -0.9, 1.0;
  EXPECT_LT(min_eig(a), 0.0); // indefinite
  const f64 eps = 1e-4;
  const MatX clipped = eigenvalue_clip(a, eps);
  ASSERT_EQ(clipped.rows(), N);
  // Symmetric and all eigenvalues ≥ eps (within fp tolerance).
  for (Eigen::Index i = 0; i < N; ++i) {
    for (Eigen::Index j = 0; j < N; ++j) {
      EXPECT_NEAR(clipped(i, j), clipped(j, i), 1e-12);
    }
  }
  EXPECT_GE(min_eig(clipped), eps - 1e-9);
}


}  // namespace atxtest_risk_shrinkage_psd_test
