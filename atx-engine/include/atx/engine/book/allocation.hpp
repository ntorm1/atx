#pragma once

// atx::engine::book — capacity-bounded fractional-Kelly allocation (S7-4, §4.4).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  Two cold-path free functions that turn a SIZED book into a gross-leverage
//  target, plus the diversification cross-check the book report records:
//
//    * size_book(SR, σ, capacity_gross, cfg) — the fractional-Kelly gross target
//        L = clip( c·SR/σ , 0 , min(capacity_gross, max_gross) ).
//        c (cfg.fractional_kelly) is the Kelly fraction — half-Kelly (0.5) by
//        default, the standard variance-drag haircut. The HARD upper clip is the
//        smaller of the capacity ceiling and cfg.max_gross, so the allocated
//        gross PROVABLY never exceeds the capacity ceiling (R7) regardless of how
//        aggressive the Kelly target is. σ <= 0 short-circuits to 0 (a degenerate
//        / undefined book vol allocates nothing), and a negative Kelly (a
//        negative book Sharpe) clamps to 0 — we never lever a negative-edge book.
//
//    * effective_breadth(eigenvalues) — the participation ratio of the alpha-
//        return covariance eigenspectrum, (Σλ)²/Σλ². This is the Grinold-Kahn
//        breadth read off the eigenvalues: many weakly-correlated alphas spread
//        the spectrum ⇒ BR≈N; a collinear pile concentrates it in one eigenvalue
//        ⇒ BR≈1. Non-positive eigenvalues (numerical noise floor) are clamped to
//        0 in both sums so a slightly-indefinite sample covariance is well-posed.
//
// ===========================================================================
//  Where capacity comes from (NOT here)
// ===========================================================================
//  capacity_gross is a SCALAR passed in by the S7-5 pipeline, which derives it via
//  cost::capacity_point(risk::capacity_curve(...)). This unit takes the calibrated
//  ceiling as a plain number so the allocation math has no cost/capacity-curve
//  dependency — it is pure scalar arithmetic.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG / clock / map. effective_breadth reduces the eigenvalues in canonical
//  ascending index order (fixed); size_book is branch-free scalar math. Same
//  inputs ⇒ same outputs. Both noexcept (no allocation, no throwing call).

#include <algorithm> // std::clamp, std::min

#include "atx/core/linalg/linalg.hpp" // VecX (alpha-return covariance eigenvalues)
#include "atx/core/types.hpp"         // f64

#include <Eigen/Core> // Eigen::Index (eigenvalue-vector indexing)

namespace atx::engine::book {

// ===========================================================================
//  AllocationConfig — the fractional-Kelly knobs.
// ===========================================================================
struct AllocationConfig {
  atx::f64 fractional_kelly = 0.5; // c ∈ (0,1]: L = c·SR/σ (half-Kelly default haircut)
  atx::f64 max_gross = 4.0;        // hard gross cap regardless of Kelly / capacity
};

// Participation ratio of the alpha-return covariance eigenspectrum: (Σλ)²/Σλ².
// s1 = Σ max(λ_i, 0); s2 = Σ max(λ_i, 0)²; if s2 <= 0 return 0; else s1²/s2.
// Order-fixed ascending i; ignores non-positive eigenvalues (a slightly-indefinite
// sample covariance still yields a well-posed breadth). Many weakly-correlated
// alphas ⇒ BR≈N; collinear ⇒ BR≈1.
[[nodiscard]] inline atx::f64 effective_breadth(const atx::core::linalg::VecX &cov_eigenvalues) noexcept {
  atx::f64 s1 = 0.0; // Σ max(λ,0)
  atx::f64 s2 = 0.0; // Σ max(λ,0)²
  for (Eigen::Index i = 0; i < cov_eigenvalues.size(); ++i) {
    const atx::f64 lambda = cov_eigenvalues[i] > 0.0 ? cov_eigenvalues[i] : 0.0;
    s1 += lambda;
    s2 += lambda * lambda;
  }
  if (s2 <= 0.0) {
    return 0.0;
  }
  return s1 * s1 / s2;
}

// Fractional-Kelly gross-leverage target, capacity-bounded (R7):
//   L = clip( c·SR_book/σ_book , 0 , min(capacity_gross, max_gross) ).
// σ_book <= 0 short-circuits to 0 (undefined book vol allocates nothing); a
// negative Kelly clamps to 0 (never lever a negative-edge book). The hard upper
// clip is the smaller of the capacity ceiling and cfg.max_gross, so the allocated
// gross PROVABLY never exceeds the capacity ceiling.
[[nodiscard]] inline atx::f64 size_book(atx::f64 sr_book, atx::f64 sigma_book,
                                        atx::f64 capacity_gross, const AllocationConfig &cfg) noexcept {
  if (sigma_book <= 0.0) {
    return 0.0;
  }
  const atx::f64 kelly = cfg.fractional_kelly * sr_book / sigma_book;
  return std::clamp(kelly, 0.0, std::min(capacity_gross, cfg.max_gross));
}

} // namespace atx::engine::book
