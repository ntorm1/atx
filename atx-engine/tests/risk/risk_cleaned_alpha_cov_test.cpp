// risk_cleaned_alpha_cov_test.cpp — p8 S1-3: cleaned_alpha_cov accessor.
//
// S1-3 exposes a PURE covariance-cleaning accessor the combiner can call
// (behind RiskModelConfig.kind==Factor) instead of the raw
// combine::detail::mle_covariance it uses today (stage_combine.cpp:755) — a
// seam, not a stage_combine.cpp edit (Sprint 3 owns that file and threads the
// call; see the sprint-1 ledger's seam-handoff note). cleaned_alpha_cov reuses
// the existing S8.7 toolkit VERBATIM: constant_correlation_shrinkage (Ledoit-
// Wolf constant-correlation target) -> mp_clip (Marchenko-Pastur eigen-clip on
// the correlation form) -> eigenvalue_clip (psd_repair's strict-PD floor). No
// new estimator math.
//
// Suite: CleanedAlphaCov
//
// Tests (per sprint-1-risk-model-covariance.md S1-3 Accept):
//   * ShrinksSpuriousEigenvalue — on a fixture where the sample covariance
//       (N≈T, the noise-dominated regime) carries a spurious large eigenvalue,
//       the cleaned covariance's min-variance weights are STRICTLY more
//       diversified (lower max weight) than the raw sample-covariance weights.
//   * ReturnsPsd                — the returned matrix is SPD (Cholesky
//       succeeds via risk::FactorModel::create's own SPD gate, reused as the
//       oracle — no independent SPD check invented here).
//   * PureAndDeterministic      — same input -> byte-identical output across
//       two independent calls (no RNG, no clock, no map iteration).

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/risk/factor_model.hpp" // FactorModel::create -- the SPD oracle

namespace atxtest_cleaned_alpha_cov {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::data::cleaned_alpha_cov;
using atx::engine::risk::FactorModel;

// Min-variance weights (unconstrained, up-to-scale) for a covariance `cov`:
// w ∝ cov⁻¹·1, normalized to Σw = 1. Used as the "downstream decision" the
// spurious-eigenvalue test measures diversification through (a standard
// diagnostic for comparing a noisy vs cleaned covariance -- lower max|w_i|
// means the cleaned covariance is not chasing one dominant noisy direction).
[[nodiscard]] VecX min_variance_weights(const MatX& cov) {
  const Eigen::Index n = cov.rows();
  const VecX ones = VecX::Ones(n);
  const VecX raw = cov.ldlt().solve(ones);
  const f64 sum = raw.sum();
  return (std::fabs(sum) > 1e-12) ? VecX(raw / sum) : raw;
}

// A T x N return panel (T close to N, the classic noise-dominated regime)
// built from a LOW-RANK common factor + small idiosyncratic noise (a fixed
// deterministic sinusoid, not RNG) PLUS ONE extra column driven by a much
// larger-amplitude independent oscillation -- the "spurious large eigenvalue"
// sample covariances exhibit in the N~T regime (Marchenko-Pastur noise bulk
// plus one outlier direction the shrinkage + eigen-clip should tame).
[[nodiscard]] MatX make_noisy_return_window(usize t, usize n) {
  MatX r(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(n));
  for (usize row = 0; row < t; ++row) {
    const f64 common = 0.01 * std::sin(0.2 * static_cast<f64>(row));
    for (usize col = 0; col < n; ++col) {
      f64 idio = 0.004 * std::sin(0.7 * static_cast<f64>(row) + static_cast<f64>(col));
      if (col == n - 1) {
        // The spurious-outlier column: large, near-independent oscillation.
        idio += 0.05 * std::sin(1.3 * static_cast<f64>(row) + 0.5);
      }
      r(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(col)) = common + idio;
    }
  }
  // Column-demean (cleaned_alpha_cov's contract: a column-demeaned T x N panel).
  const VecX col_mean = r.colwise().mean();
  for (Eigen::Index c = 0; c < r.cols(); ++c) {
    r.col(c).array() -= col_mean[c];
  }
  return r;
}

[[nodiscard]] MatX sample_cov(const MatX& centered) {
  const Eigen::Index t = centered.rows();
  return (t > 0) ? MatX((centered.transpose() * centered) / static_cast<f64>(t))
                 : MatX::Zero(centered.cols(), centered.cols());
}

TEST(CleanedAlphaCov, ShrinksSpuriousEigenvalueAndDiversifies) {
  constexpr usize T = 20, N = 18; // N approx T -- the noise-dominated regime
  const MatX centered = make_noisy_return_window(T, N);

  const MatX raw = sample_cov(centered);
  const MatX cleaned = cleaned_alpha_cov(centered);

  const VecX w_raw = min_variance_weights(raw);
  const VecX w_clean = min_variance_weights(cleaned);

  const f64 max_raw = w_raw.array().abs().maxCoeff();
  const f64 max_clean = w_clean.array().abs().maxCoeff();

  EXPECT_LT(max_clean, max_raw)
      << "expected the cleaned covariance's min-variance weights to be MORE "
         "diversified (lower max|w|) than the raw sample covariance's: "
      << "max_clean=" << max_clean << " max_raw=" << max_raw;
}

TEST(CleanedAlphaCov, ReturnsPsd) {
  constexpr usize T = 20, N = 18;
  const MatX centered = make_noisy_return_window(T, N);
  const MatX cleaned = cleaned_alpha_cov(centered);

  // Reuse FactorModel::create's own SPD gate as the oracle: X = Identity(N)
  // (K=N) makes V = X F Xᵀ + D == F + D exactly, so with D floored to a tiny
  // constant, create succeeds iff `cleaned` (== F here) is SPD -- the same
  // Cholesky check create applies to any real factor covariance.
  const MatX x = MatX::Identity(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(N));
  const VecX d = VecX::Constant(static_cast<Eigen::Index>(N), 1e-12);
  auto model_r = FactorModel::create(x, cleaned, d, 0, T);
  ASSERT_TRUE(model_r.has_value())
      << "cleaned_alpha_cov's output failed FactorModel::create's SPD gate: "
      << model_r.error().message();
}

TEST(CleanedAlphaCov, PureAndDeterministic) {
  constexpr usize T = 20, N = 18;
  const MatX centered = make_noisy_return_window(T, N);
  const MatX a = cleaned_alpha_cov(centered);
  const MatX b = cleaned_alpha_cov(centered);
  ASSERT_EQ(a.rows(), b.rows());
  ASSERT_EQ(a.cols(), b.cols());
  for (Eigen::Index r = 0; r < a.rows(); ++r) {
    for (Eigen::Index c = 0; c < a.cols(); ++c) {
      EXPECT_EQ(a(r, c), b(r, c)) << "mismatch at (" << r << "," << c << ")";
    }
  }
}

} // namespace atxtest_cleaned_alpha_cov
