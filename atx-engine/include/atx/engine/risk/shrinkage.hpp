#pragma once

// atx::engine::risk — model-free shrinkage + Marchenko-Pastur RMT cleaning (S8.7).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  A STANDALONE risk::cov toolkit (NOT wired into FactorModel::build this sprint)
//  for building a well-conditioned, model-free covariance / correlation directly
//  from a return panel:
//
//    * constant_correlation_shrinkage(centered) — Ledoit-Wolf 2004 optimal linear
//      shrinkage toward the CONSTANT-CORRELATION target. The companion to the
//      canonical scaled-identity LW in combine::detail::ledoit_wolf_intensity:
//      same kernel structure (sample cov S, asymptotic-variance π̂, target distance
//      γ̂, the clamped intensity δ), a DIFFERENT target F (constant correlation
//      rather than μ·I) and the matching ρ̂ closed form. PSD by construction;
//      positive-definite even at N>T where the raw sample cov S is rank-deficient.
//
//    * mp_clip(corr, q) — Marchenko-Pastur eigenvalue clipping. All eigenvalues
//      below the noise-bulk edge λ₊=(1+√q)² are replaced by their average
//      (TRACE-PRESERVING), then the diagonal is renormalized to 1 so the result
//      stays a correlation matrix.
//
// ===========================================================================
//  DRY with the canonical scaled-identity LW
// ===========================================================================
//  combine::detail::ledoit_wolf_intensity STAYS the home of the scaled-identity
//  (μ·I) target. S8.7 does NOT fork that kernel — it adds the constant-correlation
//  TARGET and its ρ̂. The two share the same skeleton:
//      S = (1/T)·centeredᵀ·centered   (MLE divisor T — coherent with the combiner)
//      π̂ = Σ_ij (1/T) Σ_t (c_ti c_tj − S_ij)²        (per-entry asymptotic variance)
//      γ̂ = ‖F − S‖²_F                                 (target misspecification)
//      δ = clamp( (π̂ − ρ̂) / (T·γ̂), 0, 1 )
//  Only F (and hence γ̂ and ρ̂) differ between the two targets.
//
// ===========================================================================
//  References
// ===========================================================================
//  Ledoit & Wolf (2004) "Honey, I Shrunk the Sample Covariance Matrix", J.
//  Portfolio Management — the constant-correlation target and the ρ̂ closed form
//  (their Appendix). Marchenko-Pastur law / Bun-Bouchaud-Potters — the noise-bulk
//  edge λ₊ and eigenvalue clipping.
//
//  // PATTERN-B: atx-core nonlinear_shrinkage (QIS/QuEST) is the L7 lift; the
//  engine ships const-corr LW + MP clip here. Nonlinear (QIS/QuEST) shrinkage is
//  NOT built this sprint.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG, no clock, no map iteration. Every reduction runs in canonical ascending
//  order (date t, then row i, then column j). Same input ⇒ byte-identical output.

#include <algorithm> // std::clamp
#include <cmath>     // std::sqrt
#include <utility>   // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp"            // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/linalg/decompose.hpp" // symmetric_eig
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX
#include "atx/core/types.hpp"            // f64, usize

namespace atx::engine::risk {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

namespace detail {

// Per-column standard deviations s_i = √S_ii of an N×N sample covariance. A
// non-positive diagonal entry maps to s_i = 0 (the caller guards the 1/s_i divide).
[[nodiscard]] inline VecX sample_stddevs(const MatX &s) {
  const Eigen::Index n = s.rows();
  VecX out(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    const atx::f64 v = s(i, i);
    out[i] = (v > 0.0) ? std::sqrt(v) : 0.0;
  }
  return out;
}

// Mean of the off-diagonal sample correlations C_ij = S_ij/(s_i s_j) over i<j
// (order-fixed). Returns 0 when N<2 or every pair has a degenerate s_i==0 (no
// usable correlation signal ⇒ the const-corr target collapses to a diagonal).
[[nodiscard]] inline atx::f64 mean_off_diagonal_correlation(const MatX &s, const VecX &sd) {
  const Eigen::Index n = s.rows();
  atx::f64 sum = 0.0;
  atx::f64 count = 0.0;
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = i + 1; j < n; ++j) {
      const atx::f64 denom = sd[i] * sd[j];
      if (denom > 0.0) {
        sum += s(i, j) / denom;
        count += 1.0;
      }
    }
  }
  return (count > 0.0) ? sum / count : 0.0;
}

} // namespace detail

// Constant-correlation Ledoit-Wolf target F from the sample covariance S:
//   F_ii = S_ii ;  F_ij = r̄·s_i·s_j  (i≠j),   s_i = √S_ii,
//   r̄ = mean of the off-diagonal sample correlations.
// Contract: `s` is an N×N sample covariance (symmetric, non-negative diagonal). A
// degenerate column (S_ii ≤ 0 ⇒ s_i = 0) zeroes that row/column off-diagonal — the
// target degrades to the diagonal of S there, which is the graceful no-signal case.
[[nodiscard]] inline MatX constant_correlation_target(const MatX &s) {
  const Eigen::Index n = s.rows();
  const VecX sd = detail::sample_stddevs(s);
  const atx::f64 rbar = detail::mean_off_diagonal_correlation(s, sd);
  MatX f(n, n);
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      f(i, j) = (i == j) ? s(i, j) : rbar * sd[i] * sd[j];
    }
  }
  return f;
}

namespace detail {

// The constant-correlation ρ̂ term (Ledoit-Wolf 2004, Appendix). Sum of the diagonal
// per-entry asymptotic variances plus the off-diagonal cross-covariance correction:
//   ρ̂ = Σ_i π̂_ii
//     + Σ_{i≠j} (r̄/2)·( √(S_jj/S_ii)·ϑ̂_{ii,ij} + √(S_ii/S_jj)·ϑ̂_{jj,ij} )
// with (order-fixed sums over t)
//   π̂_ii = (1/T) Σ_t (c_ti² − S_ii)²
//   ϑ̂_{ii,ij} = (1/T) Σ_t ( (c_ti² − S_ii)·(c_ti c_tj − S_ij) )
//   ϑ̂_{jj,ij} = (1/T) Σ_t ( (c_tj² − S_jj)·(c_ti c_tj − S_ij) )
// √(S_jj/S_ii) = s_j/s_i since s_i = √S_ii. A degenerate s_i==0 contributes nothing
// (the divide-guard skips that pair) — consistent with the target's degradation.
[[nodiscard]] inline atx::f64 const_corr_rho(const MatX &centered, const MatX &s, const VecX &sd,
                                             atx::f64 rbar) {
  const Eigen::Index t = centered.rows();
  const Eigen::Index n = s.rows();
  const atx::f64 tf = static_cast<atx::f64>(t);
  atx::f64 rho = 0.0;
  // Diagonal contribution Σ_i π̂_ii.
  for (Eigen::Index i = 0; i < n; ++i) {
    atx::f64 acc = 0.0;
    for (Eigen::Index k = 0; k < t; ++k) {
      const atx::f64 d = centered(k, i) * centered(k, i) - s(i, i);
      acc += d * d;
    }
    rho += acc / tf;
  }
  // Off-diagonal cross-covariance correction.
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      if (i == j || sd[i] <= 0.0 || sd[j] <= 0.0) {
        continue;
      }
      atx::f64 theta_ii = 0.0; // ϑ̂_{ii,ij}
      atx::f64 theta_jj = 0.0; // ϑ̂_{jj,ij}
      for (Eigen::Index k = 0; k < t; ++k) {
        const atx::f64 prod_off = centered(k, i) * centered(k, j) - s(i, j);
        theta_ii += (centered(k, i) * centered(k, i) - s(i, i)) * prod_off;
        theta_jj += (centered(k, j) * centered(k, j) - s(j, j)) * prod_off;
      }
      theta_ii /= tf;
      theta_jj /= tf;
      rho += (rbar / 2.0) * ((sd[j] / sd[i]) * theta_ii + (sd[i] / sd[j]) * theta_jj);
    }
  }
  return rho;
}

// π̂ = Σ_ij (1/T) Σ_t (c_ti c_tj − S_ij)² — total per-entry asymptotic variance.
// Order-fixed (t outer? no: i,j outer with t inner so each entry's sum is exact and
// independent — the grand total is order-fixed either way; we sum entries i<j? no,
// the full N² grid to match the closed form). Reuses the SAME quantity the canonical
// scaled-identity kernel forms as Σ_t ‖r_t r_tᵀ − S‖²_F / T.
[[nodiscard]] inline atx::f64 const_corr_pi(const MatX &centered, const MatX &s) {
  const Eigen::Index t = centered.rows();
  const Eigen::Index n = s.rows();
  const atx::f64 tf = static_cast<atx::f64>(t);
  atx::f64 pi_sum = 0.0;
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      atx::f64 acc = 0.0;
      for (Eigen::Index k = 0; k < t; ++k) {
        const atx::f64 d = centered(k, i) * centered(k, j) - s(i, j);
        acc += d * d;
      }
      pi_sum += acc / tf;
    }
  }
  return pi_sum;
}

} // namespace detail

// Constant-correlation Ledoit-Wolf shrinkage Σ̂ = δ·F + (1−δ)·S.
//
// Contract: `centered` is a T×N COLUMN-DEMEANED return panel (each column's
// time-mean already subtracted). Sample cov S = (1/T)·centeredᵀ·centered (MLE
// divisor T, coherent with combine::detail::mle_covariance). The intensity
//   δ = clamp( (π̂ − ρ̂) / (T·γ̂), 0, 1 )
// with γ̂ = ‖F − S‖²_F. PSD by construction: a convex combination of the PSD sample
// cov S and the PD-ish constant-correlation target F. At N>T (S rank-deficient and
// singular) a positive δ lifts the smallest eigenvalue ⇒ the result is PD.
//
// GRACEFUL DEGRADE (non-fallible MatX return): a degenerate window (T<2, N<1, or
// γ̂==0 meaning F already equals S so there is nothing to shrink) yields δ=0 ⇒ the
// raw S is returned. This is the documented well-defined fallback; the function does
// not allocate a Result for these edge cases because S is always a valid answer.
[[nodiscard]] inline MatX constant_correlation_shrinkage(const MatX &centered) {
  const Eigen::Index t = centered.rows();
  const Eigen::Index n = centered.cols();
  const MatX s = (t > 0) ? MatX((centered.transpose() * centered) / static_cast<atx::f64>(t))
                         : MatX(MatX::Zero(n, n));
  if (t < 2 || n < 1) {
    return s; // too few observations / empty cross-section ⇒ no shrinkage signal
  }
  const VecX sd = detail::sample_stddevs(s);
  const atx::f64 rbar = detail::mean_off_diagonal_correlation(s, sd);
  const MatX f = constant_correlation_target(s);
  const atx::f64 gamma = (f - s).squaredNorm();
  if (gamma <= 0.0) {
    return s; // target already equals S ⇒ δ=0, nothing to shrink
  }
  const atx::f64 pi_hat = detail::const_corr_pi(centered, s);
  const atx::f64 rho = detail::const_corr_rho(centered, s, sd, rbar);
  const atx::f64 tf = static_cast<atx::f64>(t);
  const atx::f64 delta = std::clamp((pi_hat - rho) / (tf * gamma), 0.0, 1.0);
  return delta * f + (1.0 - delta) * s;
}

// Marchenko-Pastur eigenvalue clipping of a correlation matrix.
//
// Contract: `corr` is an N×N symmetric correlation matrix (unit diagonal); `q = N/T`
// is the aspect ratio of the return panel it was estimated from (q>0). The noise-bulk
// upper edge for unit-variance noise is λ₊ = (1+√q)². Every eigenvalue Λ_k < λ₊ is
// "noise"; ALL of them are replaced by their average λ̄ = (Σ_{noise} Λ_k)/n_noise
// (TRACE-PRESERVING — the clipped block's sum is unchanged, so tr(C_clean)=tr(C)=N).
// C_clean = U·diag(Λ_clipped)·Uᵀ; the diagonal is then renormalized to 1 so the
// result remains a correlation matrix. If NO eigenvalue is below λ₊ (no noise bulk),
// `corr` is returned unchanged.
//
// Err: q ≤ 0 (InvalidArgument), N < 1 (InvalidArgument), or symmetric_eig failure.
[[nodiscard]] inline Result<MatX> mp_clip(const MatX &corr, atx::f64 q) {
  if (q <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "mp_clip: q must be positive");
  }
  if (corr.rows() < 1 || corr.cols() != corr.rows()) {
    return Err(ErrorCode::InvalidArgument, "mp_clip: corr must be a non-empty square matrix");
  }
  const Eigen::Index n = corr.rows();
  ATX_TRY(const auto eig, atx::core::linalg::symmetric_eig(corr));
  const atx::f64 lambda_plus = (1.0 + std::sqrt(q)) * (1.0 + std::sqrt(q));
  // Sum the noise block (eigenvalues < λ₊) and count it (order-fixed ascending).
  atx::f64 noise_sum = 0.0;
  Eigen::Index n_noise = 0;
  for (Eigen::Index k = 0; k < n; ++k) {
    if (eig.values[k] < lambda_plus) {
      noise_sum += eig.values[k];
      ++n_noise;
    }
  }
  if (n_noise == 0) {
    return Ok(MatX(corr)); // no noise bulk below λ₊ ⇒ nothing to clean
  }
  const atx::f64 lambda_bar = noise_sum / static_cast<atx::f64>(n_noise);
  // Reassemble with the noise block flattened to its average (trace-preserving).
  VecX clipped = eig.values;
  for (Eigen::Index k = 0; k < n; ++k) {
    if (clipped[k] < lambda_plus) {
      clipped[k] = lambda_bar;
    }
  }
  MatX cleaned = eig.vectors * clipped.asDiagonal() * eig.vectors.transpose();
  // Renormalize the diagonal to 1 so it stays a correlation matrix.
  VecX inv_root(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    const atx::f64 d = cleaned(i, i);
    inv_root[i] = (d > 0.0) ? 1.0 / std::sqrt(d) : 0.0;
  }
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      cleaned(i, j) *= inv_root[i] * inv_root[j];
    }
  }
  return Ok(std::move(cleaned));
}

} // namespace atx::engine::risk
