#pragma once

// atx::engine::risk — EWMA split-half-life factor covariance + Newey-West (S8.2).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  ewma_factor_covariance(fseries, vol_hl, corr_hl, nw_lags) replaces the as-built
//  single-MLE→scaled-identity Ledoit-Wolf factor-covariance path with the vendor
//  recipe a risk shop actually trades:
//
//    * EWMA VARIANCES at the FAST half-life `vol_hl`     (short-horizon vol level)
//    * EWMA CORRELATIONS at the SLOW half-life `corr_hl` (stable co-movement)
//    * RECOMBINE  F_ij = ρ_ij·σ_i·σ_j ,  F_ii = σ_i²
//    * NEWEY-WEST Bartlett serial-correlation adjustment  F += Σ_d (1−d/(L+1))(Γ_d+Γ_dᵀ)
//    * SPD eigenvalue FLOOR so FactorModel::create's Cholesky always succeeds.
//
//  Splitting the half-lives lets the variance track recent regime shifts while the
//  correlation stays anchored on a longer window — the single-window MLE is both
//  ill-conditioned at T≈K and forced to share one decay for level and structure.
//
// ===========================================================================
//  Row order (LOAD-BEARING) + the half-life weighting
// ===========================================================================
//  `fseries` is a T×K matrix of per-date factor returns whose ROW 0 IS THE NEWEST
//  date (the as-built PanelView is newest-first; date s=0 is the present cross-
//  section). The EWMA "k steps back" index is therefore the row index r itself
//  (row 0 ⇒ k=0 ⇒ weight 1). The weight on row r for half-life H is
//
//      w_r = 2^(−r/H)            (geometric decay; w_r/w_{r+1} = 2^(1/H) exactly)
//
//  with the SENTINEL `H == 0 ⇒ equal weights (w_r ≡ 1)` — the no-EWMA-tilt path.
//
//  APPROXIMATION (documented): the builder COMPACTS under-determined dates out of
//  `fseries` (a date with M_s < K is skipped, so the kept rows are not contiguous
//  in calendar time). We weight by the ROW index r — i.e. the r-th-NEWEST KEPT
//  date — not by the true calendar gap. For a dense window this is exact; for a
//  sparse one it slightly over-weights the rows after a gap. This matches how the
//  P4 path already treats the compacted `fkept` rows as the factor-return series.
//
// ===========================================================================
//  Newey-West weighting choice (documented)
// ===========================================================================
//  The contemporaneous covariance Γ_0 is the EWMA-split F (it already carries the
//  half-life tilt). The Newey-West LAG autocovariances Γ_d (d ≥ 1) are computed
//  EQUAL-WEIGHTED — the standard Bartlett HAC estimator — over the equal-weighted
//  demeaned series, and ADDED to that EWMA Γ_0:
//
//      F_final = F_split + Σ_{d=1..L} (1 − d/(L+1)) (Γ_d + Γ_dᵀ)
//
//  Mixing an EWMA Γ_0 with equal-weighted lag terms is the pragmatic choice: the
//  lag terms are a small serial-correlation CORRECTION, and an equal-weighted HAC
//  estimator is the well-understood reference whose AR(1) long-run-variance has a
//  closed form the test pins. `nw_lags == 0 ⇒ no adjustment` (F == F_split).
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG, no clock, no map iteration. Every reduction runs in canonical ascending
//  order (row, then factor; ascending lag d). Same input ⇒ byte-identical F.

#include <cmath>   // std::pow, std::sqrt
#include <utility> // std::move

#include <Eigen/Dense>

#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/decompose.hpp" // symmetric_eig (SPD floor)
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX (column-major Eigen)

namespace atx::engine::risk {

// Floor applied to every eigenvalue of the final F so it is positive-DEFINITE
// (FactorModel::create's Cholesky succeeds). Relative to the matrix trace so it
// scales with the factor-return magnitude rather than being an absolute constant
// that could dominate a tiny-variance block or vanish against a large one.
inline constexpr atx::f64 kEwmaSpdFloorRel = 1e-10;

namespace detail {

// EWMA weight vector over `n` rows for half-life `H`: w_r = 2^(−r/H), r = 0..n−1
// (row 0 = newest = weight 1). H == 0 ⇒ equal weights (w_r ≡ 1), the no-tilt
// sentinel. Order-fixed (ascending r). `n == 0` returns an empty vector.
[[nodiscard]] inline atx::core::linalg::VecX ewma_weights(atx::usize n, atx::usize half_life) {
  atx::core::linalg::VecX w(static_cast<Eigen::Index>(n));
  if (half_life == 0U) {
    w.setOnes();
    return w;
  }
  const atx::f64 inv_h = 1.0 / static_cast<atx::f64>(half_life);
  for (atx::usize r = 0U; r < n; ++r) {
    w[static_cast<Eigen::Index>(r)] = std::pow(2.0, -static_cast<atx::f64>(r) * inv_h);
  }
  return w;
}

// EWMA-weighted column means: μ_c = (Σ_r w_r x_{r,c}) / (Σ_r w_r). One scan per
// column, order-fixed ascending row. `sw` (= Σ_r w_r) is returned via `sw_out` so
// the caller reuses it for the weighted second moments. T must be >= 1.
[[nodiscard]] inline atx::core::linalg::VecX ewma_means(const atx::core::linalg::MatX &x,
                                                        const atx::core::linalg::VecX &w,
                                                        atx::f64 &sw_out) noexcept {
  const Eigen::Index t = x.rows();
  const Eigen::Index k = x.cols();
  atx::f64 sw = 0.0;
  for (Eigen::Index r = 0; r < t; ++r) {
    sw += w[r];
  }
  sw_out = sw;
  atx::core::linalg::VecX mu(k);
  for (Eigen::Index c = 0; c < k; ++c) {
    atx::f64 acc = 0.0;
    for (Eigen::Index r = 0; r < t; ++r) {
      acc += w[r] * x(r, c);
    }
    mu[c] = acc / sw;
  }
  return mu;
}

// EWMA-weighted covariance at one half-life: Cov_ij = Σ_r w_r (x_{r,i}−μ_i)(x_{r,j}−μ_j)/Σ_r w_r,
// with μ the SAME-half-life EWMA mean. Symmetric K×K. Order-fixed (ascending row,
// then i, then j). Used both for the vol-HL variances (its diagonal) and the
// corr-HL correlations (normalized below).
[[nodiscard]] inline atx::core::linalg::MatX ewma_cov(const atx::core::linalg::MatX &x,
                                                      atx::usize half_life) {
  const Eigen::Index t = x.rows();
  const Eigen::Index k = x.cols();
  const atx::core::linalg::VecX w = ewma_weights(static_cast<atx::usize>(t), half_life);
  atx::f64 sw = 0.0;
  const atx::core::linalg::VecX mu = ewma_means(x, w, sw);
  atx::core::linalg::MatX cov = atx::core::linalg::MatX::Zero(k, k);
  for (Eigen::Index r = 0; r < t; ++r) {
    const atx::f64 wr = w[r];
    for (Eigen::Index i = 0; i < k; ++i) {
      const atx::f64 di = x(r, i) - mu[i];
      for (Eigen::Index j = 0; j < k; ++j) {
        cov(i, j) += wr * di * (x(r, j) - mu[j]);
      }
    }
  }
  cov /= sw;
  return cov;
}

// Recombine the split-half-life covariance: variances σ_i² from the vol-HL EWMA
// covariance diagonal, correlations ρ_ij from the corr-HL EWMA covariance, into
// F_ij = ρ_ij·σ_i·σ_j with F_ii = σ_i². A non-positive vol-HL or corr-HL diagonal
// (a degenerate constant column) yields a zero correlation for that pair (and the
// SPD floor below lifts the diagonal); ρ is otherwise the standard normalization.
// Symmetric by construction. Order-fixed.
[[nodiscard]] inline atx::core::linalg::MatX
recombine_split(const atx::core::linalg::MatX &vol_cov, const atx::core::linalg::MatX &corr_cov) {
  const Eigen::Index k = vol_cov.rows();
  atx::core::linalg::VecX sigma(k);
  for (Eigen::Index i = 0; i < k; ++i) {
    const atx::f64 v = vol_cov(i, i);
    sigma[i] = (v > 0.0) ? std::sqrt(v) : 0.0;
  }
  atx::core::linalg::MatX f(k, k);
  for (Eigen::Index i = 0; i < k; ++i) {
    f(i, i) = vol_cov(i, i); // F_ii = σ_i² exactly (vol-HL variance)
    for (Eigen::Index j = i + 1; j < k; ++j) {
      const atx::f64 denom = std::sqrt(corr_cov(i, i) * corr_cov(j, j));
      const atx::f64 rho = (denom > 0.0) ? corr_cov(i, j) / denom : 0.0;
      const atx::f64 fij = rho * sigma[i] * sigma[j];
      f(i, j) = fij;
      f(j, i) = fij;
    }
  }
  return f;
}

// Newey-West Bartlett serial-correlation term: Σ_{d=1..L} (1 − d/(L+1)) (Γ_d + Γ_dᵀ),
// where Γ_d = (1/T) Σ_{r=d..T−1} (x_r − μ)(x_{r−d} − μ)ᵀ is the EQUAL-WEIGHTED lag-d
// autocovariance (μ the equal-weighted column mean). Newest-first rows: pairing row
// r with r−d (a NEWER row) is the same lag-d product as in calendar order. L == 0 or
// L >= T returns a zero K×K (no usable lag). Order-fixed: ascending d, then i, then j.
[[nodiscard]] inline atx::core::linalg::MatX newey_west(const atx::core::linalg::MatX &x,
                                                        atx::usize nw_lags) {
  const Eigen::Index t = x.rows();
  const Eigen::Index k = x.cols();
  atx::core::linalg::MatX add = atx::core::linalg::MatX::Zero(k, k);
  if (nw_lags == 0U || static_cast<Eigen::Index>(nw_lags) >= t) {
    return add;
  }
  // Equal-weighted column mean (the HAC demeaning level).
  atx::core::linalg::VecX mu(k);
  for (Eigen::Index c = 0; c < k; ++c) {
    mu[c] = x.col(c).mean();
  }
  const atx::f64 inv_t = 1.0 / static_cast<atx::f64>(t);
  const atx::f64 lp1 = static_cast<atx::f64>(nw_lags + 1U);
  for (atx::usize d = 1U; d <= nw_lags; ++d) {
    const Eigen::Index dd = static_cast<Eigen::Index>(d);
    const atx::f64 bart = 1.0 - static_cast<atx::f64>(d) / lp1; // Bartlett taper
    atx::core::linalg::MatX gamma = atx::core::linalg::MatX::Zero(k, k);
    for (Eigen::Index r = dd; r < t; ++r) {
      for (Eigen::Index i = 0; i < k; ++i) {
        const atx::f64 di = x(r, i) - mu[i];
        for (Eigen::Index j = 0; j < k; ++j) {
          gamma(i, j) += di * (x(r - dd, j) - mu[j]);
        }
      }
    }
    gamma *= inv_t;
    add += bart * (gamma + gamma.transpose());
  }
  return add;
}

// Floor every eigenvalue of the symmetric K×K `f` at ε = kEwmaSpdFloorRel · tr(f)/K
// so the result is positive-DEFINITE (Cholesky succeeds). symmetric_eig gives the
// orthonormal eigendecomposition F = V Λ Vᵀ; we clamp Λ_k ← max(Λ_k, ε) and
// reassemble. If the eigensolver fails (it should not for a finite symmetric input)
// or the trace is non-positive, fall back to a tiny absolute diagonal ridge so the
// kernel still returns an SPD matrix. PSD-restoring, symmetric. Documented in §4.6.
[[nodiscard]] inline atx::core::linalg::MatX psd_floor(atx::core::linalg::MatX f) {
  const Eigen::Index k = f.rows();
  // Symmetrize defensively (NW transpose sum is symmetric in exact arithmetic; this
  // kills any ULP asymmetry before the eigensolver's symmetry check).
  f = 0.5 * (f + f.transpose().eval());
  const atx::f64 tr = f.trace();
  const atx::f64 eps =
      (tr > 0.0) ? kEwmaSpdFloorRel * tr / static_cast<atx::f64>(k) : kEwmaSpdFloorRel;
  const auto eig = atx::core::linalg::symmetric_eig(f);
  if (!eig) {
    f.diagonal().array() += eps; // fallback ridge (should be unreachable)
    return f;
  }
  atx::core::linalg::VecX lam = eig->values;
  bool floored = false;
  for (Eigen::Index i = 0; i < k; ++i) {
    if (lam[i] < eps) {
      lam[i] = eps;
      floored = true;
    }
  }
  if (!floored) {
    return f; // already PD with margin — leave it byte-for-byte untouched
  }
  const atx::core::linalg::MatX &v = eig->vectors;
  return v * lam.asDiagonal() * v.transpose();
}

} // namespace detail

// ===========================================================================
//  ewma_factor_covariance — the S8.2 kernel.
//
//  Contract:
//    * `fseries` : T×K per-date factor returns, ROW 0 = NEWEST date (see header).
//      Precondition T >= 1 and K >= 1 (the builder guarantees used >= K >= 1).
//    * `vol_hl`  : half-life (rows) for the variance EWMA;  0 ⇒ equal weights.
//    * `corr_hl` : half-life (rows) for the correlation EWMA; 0 ⇒ equal weights.
//    * `nw_lags` : Newey-West Bartlett lag count L; 0 ⇒ no serial-corr adjustment.
//    * Returns a K×K SYMMETRIC POSITIVE-DEFINITE MatX (eigenvalue-floored). PURE,
//      RNG-free, order-fixed ⇒ byte-identical for identical inputs.
//  Steps: vol-HL EWMA cov → variances; corr-HL EWMA cov → correlations; recombine
//  F_ij = ρ_ij·σ_i·σ_j; add the equal-weighted Newey-West Bartlett lag sum; SPD-floor.
// ===========================================================================
[[nodiscard]] inline atx::core::linalg::MatX
ewma_factor_covariance(const atx::core::linalg::MatX &fseries, atx::usize vol_hl,
                       atx::usize corr_hl, atx::usize nw_lags) {
  const atx::core::linalg::MatX vol_cov = detail::ewma_cov(fseries, vol_hl);
  const atx::core::linalg::MatX corr_cov = detail::ewma_cov(fseries, corr_hl);
  atx::core::linalg::MatX f = detail::recombine_split(vol_cov, corr_cov);
  f += detail::newey_west(fseries, nw_lags); // Γ_0 (EWMA) + Bartlett lag sum
  return detail::psd_floor(std::move(f));
}

} // namespace atx::engine::risk
