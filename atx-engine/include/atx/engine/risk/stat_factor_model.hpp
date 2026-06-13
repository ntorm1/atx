#pragma once

// atx::engine::risk — statistical factor model via APCA (T×T Gram) — S8.6 KERNELS.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  The Asymptotic Principal Components (APCA, Connor-Korajczyk 2-pass) kernels
//  that build a STATISTICAL FactorModel when FactorModelConfig.n_stat_factors > 0,
//  replacing the deferred `NotImplemented` rung. Unlike the fundamental path
//  (Barra-style style/sector exposures regressed cross-sectionally), the
//  statistical path EXTRACTS latent factors directly from the return panel.
//
//  The asymptotic argument (Connor-Korajczyk 1986) is for N ≫ T: with many assets
//  per date, the cross-sectional average of idiosyncratic returns vanishes, so the
//  leading eigenvectors of the T×T cross-product Gram Ω = (1/N)·Rᵀ·R consistently
//  estimate the factor-RETURN time series (a T-vector per factor). This header
//  forms that T×T Gram EXPLICITLY — NOT the N×N sample covariance atx-core pca()
//  forms (verified atx-core/include/atx/core/linalg/pca.hpp:67), which is O(N²) and
//  the WRONG decomposition direction when N ≫ T.
//  // PATTERN-B: atx-core apca (T×T Gram) is the L7 lift; engine forms the Gram +
//  // symmetric_eig here.
//
// ===========================================================================
//  The 2-pass algorithm (the kernels below implement these steps)
// ===========================================================================
//  Inputs: a complete-case return panel R (N×T, row n = asset, column t = date,
//  newest t=0), column-demeaned per asset (each row's time-mean subtracted).
//  K = the requested factor count. Preconditions N > T, T > K, N > K.
//
//  Pass 1 (equal-weighted):
//    1. Ω = (1/N)·Rᵀ·R                          (gram_matrix)        — T×T SPD
//    2. symmetric_eig(Ω) → values ASCENDING; the top-K (largest-K) eigenvectors,
//       reversed so column 0 is the LARGEST-variance factor, are Fhat (T×K),
//       sign-pinned (top_k_factors).
//    3. B = R·Fhat·(Fhatᵀ·Fhat)⁻¹               (exposures)          — N×K
//       s_n = pop-var over T of the residual row (R_n − B_n·Fhatᵀ)  (specific_var).
//
//  Pass 2 (GLS, opt-in via apca_gls_reweight, default true):
//    Reweight each row R_w[n,:] = R[n,:]/√s_n, recompute Ω_gls = (1/N)·R_wᵀ·R_w,
//    re-extract Fhat (final factor returns). Recover the FINAL B and s_n from the
//    UN-weighted R (exposures + specific reported in the ORIGINAL return scale).
//    apca_gls_reweight == false ⇒ Pass 1 is final.
//
//  The caller (FactorModelBuilder::build_stat_factor_model) then assembles
//  X = B, F = factor_covariance(Fhat, factor_cov_shrink), D = s_n, and calls
//  FactorModel::create. See factor_model.hpp for the orchestration.
//
// ===========================================================================
//  Sign-pinning (determinism)
// ===========================================================================
//  symmetric_eig is deterministic but an eigenvector's SIGN is arbitrary (v and −v
//  are both unit eigenvectors). We pin each Fhat column so its first NON-ZERO entry
//  is positive — a canonical choice that makes the whole pipeline (Fhat, B, F)
//  byte-identical on replay. B uses the same signed columns (B = R·Fhat·…), and F
//  is a covariance of Fhat (sign-invariant), so the only place the convention bites
//  is making Fhat itself reproducible.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG, no clock, no map iteration. Every reduction runs in canonical ascending
//  order (asset n, then date t / factor k). PURE given (R, K, gls). symmetric_eig
//  is deterministic; the sign-pin removes the only residual ambiguity.

#include <cmath>   // std::sqrt
#include <utility> // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/decompose.hpp" // symmetric_eig, EigResult
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX (column-major Eigen)
#include "atx/core/linalg/solve.hpp"     // solve_spd

namespace atx::engine::risk::detail {

// Specific-variance floor before the GLS 1/√s_n reweight: a zero-residual asset
// (R_n exactly spanned by the factors) would divide by zero. Mirrors
// specific_risk.hpp / FactorModel's kSpecificVarFloor magnitude — far below any
// real return variance, so it never tilts a well-populated asset's weight.
inline constexpr atx::f64 kStatSpecificFloor = 1e-12;

// Tiny ridge on the K×K Fhatᵀ·Fhat system when it is rank-deficient. Fhatᵀ·Fhat is
// the Gram of K full-rank eigenvector columns ⇒ SPD in exact arithmetic; the ridge
// is a numerical guard for a degenerate (duplicated-eigenvalue) factor block so the
// exposures solve still returns rather than failing. Far below the unit-norm
// eigenvector scale, so it does not bias a well-conditioned block.
inline constexpr atx::f64 kFactorGramRidge = 1e-12;

// Column-demean R IN PLACE: subtract each asset row's time-mean over its T returns.
// Order-fixed (ascending asset n, then date t via Eigen's row mean). R is N×T.
inline void demean_rows(atx::core::linalg::MatX &r) noexcept {
  for (Eigen::Index n = 0; n < r.rows(); ++n) {
    const atx::f64 mean = r.row(n).mean();
    r.row(n).array() -= mean;
  }
}

// The T×T Gram Ω = (1/N)·Rᵀ·R (R is N×T, N = R.rows()). The asymptotic-PCA trick:
// eigen-decomposing this T×T matrix (cheap when N ≫ T) recovers the factor-RETURN
// series, NOT the N×N asset covariance. SPD by construction (a scaled Gram).
[[nodiscard]] inline atx::core::linalg::MatX gram_matrix(const atx::core::linalg::MatX &r) {
  const atx::f64 inv_n = 1.0 / static_cast<atx::f64>(r.rows());
  return (r.transpose() * r) * inv_n; // T×T
}

// Pin an eigenvector column's sign so its first NON-ZERO entry is positive (the
// canonical determinism choice — see header). `eps` guards against treating a tiny
// numerical residual as the "first nonzero". Operates IN PLACE on `col`.
inline void sign_pin_column(Eigen::Ref<atx::core::linalg::VecX> col) noexcept {
  for (Eigen::Index r = 0; r < col.size(); ++r) {
    const atx::f64 v = col[r];
    if (v > 0.0) {
      return; // first nonzero already positive ⇒ canonical
    }
    if (v < 0.0) {
      col = -col; // flip so the first nonzero entry is positive
      return;
    }
  }
  // All-zero column (degenerate) ⇒ no sign to pin; leave as-is.
}

// Extract the top-K factor-return columns from a T×T Gram eigendecomposition.
// symmetric_eig returns eigenpairs ASCENDING, so the top-K (largest eigenvalues)
// are the LAST K columns; we REVERSE them so output column 0 is the LARGEST-variance
// factor (descending-variance order, matching the fundamental path's leading-factor
// convention). Each extracted column is sign-pinned. Returns Fhat (T×K).
[[nodiscard]] inline atx::core::linalg::MatX top_k_factors(const atx::core::linalg::EigResult &eig,
                                                           atx::usize k) {
  const Eigen::Index t = eig.values.size();
  const Eigen::Index kk = static_cast<Eigen::Index>(k);
  atx::core::linalg::MatX fhat(t, kk);
  for (Eigen::Index c = 0; c < kk; ++c) {
    fhat.col(c) = eig.vectors.col(t - 1 - c); // largest eigenvalue first
    sign_pin_column(fhat.col(c));
  }
  return fhat;
}

// Exposures B = R·Fhat·(Fhatᵀ·Fhat)⁻¹ (N×K). Equivalent to regressing each asset's
// return row on the K factor-return columns; we solve the K×K SPD system
// (Fhatᵀ·Fhat) Zᵀ = (R·Fhat)ᵀ once via solve_spd per RHS column (the K×K system is
// tiny). A rank-deficient Fhatᵀ·Fhat (duplicated eigenvalues) is guarded with a tiny
// ridge so the solve still returns. R is N×T, Fhat is T×K.
[[nodiscard]] inline atx::core::Result<atx::core::linalg::MatX>
exposures(const atx::core::linalg::MatX &r, const atx::core::linalg::MatX &fhat) {
  const Eigen::Index n = r.rows();
  const Eigen::Index k = fhat.cols();
  atx::core::linalg::MatX gram = fhat.transpose() * fhat; // K×K SPD
  gram.diagonal().array() += kFactorGramRidge;            // rank-deficiency guard
  const atx::core::linalg::MatX rf = r * fhat;            // N×K  (R·Fhat)
  atx::core::linalg::MatX b(n, k);
  // Solve the K×K SPD system per asset row: b_n = (FhatᵀFhat)⁻¹·(R·Fhat)_nᵀ. The RHS
  // is length K (the regression of asset n's returns on the K factors). Order-fixed
  // ascending asset row.
  for (Eigen::Index row = 0; row < n; ++row) {
    const atx::core::linalg::VecX rhs = rf.row(row).transpose(); // length K
    ATX_TRY(atx::core::linalg::VecX sol, atx::core::linalg::solve_spd(gram, rhs));
    b.row(row) = sol.transpose();
  }
  return atx::core::Ok(std::move(b));
}

// Per-asset specific variance s_n = population variance over T of the residual row
// R_n − B_n·Fhatᵀ (the part of asset n's returns NOT explained by the factors).
// R is already column-demeaned, so the residual row is mean-≈0; we use the
// order-fixed sum-of-squares / T (population variance), floored to kStatSpecificFloor
// so the GLS 1/√s_n reweight is finite. Returns a length-N vector. R is N×T.
[[nodiscard]] inline atx::core::linalg::VecX
specific_variances(const atx::core::linalg::MatX &r, const atx::core::linalg::MatX &b,
                   const atx::core::linalg::MatX &fhat) {
  const Eigen::Index n = r.rows();
  const Eigen::Index t = r.cols();
  const atx::core::linalg::MatX fitted = b * fhat.transpose(); // N×T  (B·Fhatᵀ)
  atx::core::linalg::VecX s(n);
  const atx::f64 inv_t = 1.0 / static_cast<atx::f64>(t);
  for (Eigen::Index row = 0; row < n; ++row) {
    atx::f64 ss = 0.0; // Σ_t resid² (order-fixed ascending date)
    for (Eigen::Index col = 0; col < t; ++col) {
      const atx::f64 e = r(row, col) - fitted(row, col);
      ss += e * e;
    }
    const atx::f64 var = ss * inv_t; // population variance (R is demeaned)
    s[row] = (var < kStatSpecificFloor) ? kStatSpecificFloor : var;
  }
  return s;
}

// One APCA pass: form the Gram, eigendecompose, take the top-K sign-pinned factor
// returns. `r_for_gram` is the (possibly GLS-reweighted) panel whose Gram is
// decomposed. Returns Fhat (T×K). Propagates a symmetric_eig failure.
[[nodiscard]] inline atx::core::Result<atx::core::linalg::MatX>
apca_factor_returns(const atx::core::linalg::MatX &r_for_gram, atx::usize k) {
  const atx::core::linalg::MatX gram = gram_matrix(r_for_gram);
  ATX_TRY(atx::core::linalg::EigResult eig, atx::core::linalg::symmetric_eig(gram));
  return atx::core::Ok(top_k_factors(eig, k));
}

// GLS row-reweight: R_w[n,:] = R[n,:] / √s_n (s_n already floored > 0). Down-weights
// high-idiosyncratic-variance assets so the second Gram emphasizes the systematic
// structure (Connor-Korajczyk Pass 2). R is N×T; `s` is length N. Returns N×T.
[[nodiscard]] inline atx::core::linalg::MatX gls_reweight(const atx::core::linalg::MatX &r,
                                                          const atx::core::linalg::VecX &s) {
  atx::core::linalg::MatX rw = r;
  for (Eigen::Index n = 0; n < rw.rows(); ++n) {
    rw.row(n) /= std::sqrt(s[n]);
  }
  return rw;
}

} // namespace atx::engine::risk::detail
