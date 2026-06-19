#pragma once

// atx::engine::eval — Breadth instrumentation: the effective number of
// INDEPENDENT bets, the realized information coefficient, and the implied
// information-ratio decomposition.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  The Fundamental Law of Active Management (Grinold, 1989):
//
//        IR = IC · √breadth
//
//  says the information ratio of a strategy is its skill per bet (the IC — the
//  cross-sectional correlation of forecast to realized return) times the square
//  root of the number of INDEPENDENT bets it places per period. RenTech's casino
//  edge is breadth: thousands of small, weakly-correlated bets, each with a tiny
//  IC, compounding into a large IR. The catch is "independent": correlated bets
//  do not count as separate draws. The naïve count (rows of a return matrix, or
//  the number of admitted alphas) OVERSTATES breadth whenever the bets covary.
//
//  This module makes breadth MEASURABLE from the signal/return covariance. The
//  effective number of independent bets is the participation ratio of the
//  covariance eigenvalue spectrum:
//
//        N_eff = (Σ_i λ_i)² / (Σ_i λ_i²),   λ = eigenvalues of the covariance.
//
//  This is the inverse of the normalized Herfindahl index over the eigenvalues:
//  it counts how many eigen-directions carry comparable variance. Two anchoring
//  cases pin the semantics:
//    * K orthogonal equal-variance bets (cov = c·I_K, K equal eigenvalues) ⇒
//      N_eff = (K·c)² / (K·c²) = K — every direction counts, full breadth.
//    * K identical bets (rank-1 cov, one nonzero eigenvalue) ⇒ N_eff = 1 — the
//      K copies collapse to a single independent draw (closing the same crowding
//      gap S10-4 attacks, but as a measurable scalar rather than a shrink).
//
//  Given N_eff and a caller-supplied realized IC, the implied IR follows directly
//  from the Fundamental Law: IR = IC · √N_eff. The report prints breadth / IC / IR
//  alongside Sharpe so an operator can see WHICH of the two levers (skill vs.
//  independent count) a book is actually pulling.
//
// ===========================================================================
//  Numeric / determinism conventions (load-bearing)
// ===========================================================================
//  * Eigenvalues come from the atx-core symmetric eigensolver (decompose.hpp),
//    which returns them ASCENDING. We clamp each to max(λ, 0): a covariance is
//    PSD in exact arithmetic, but a finite-precision eigensolver can emit a tiny
//    NEGATIVE eigenvalue for a (near-)singular input (e.g. the rank-1 identical-
//    bets case). A negative λ is physically a zero-variance direction; clamping
//    keeps Σλ² honest and never lets a numerical artifact inflate or sign-flip
//    the ratio.
//  * Reductions run in ascending eigenvalue order (the order the solver returns),
//    so the result is run-to-run byte-identical. No RNG; pure functions.
//  * A zero matrix (every λ clamped to 0 ⇒ Σλ == 0) is DOCUMENTED to yield
//    N_eff = 0: there is no variance, hence no bet to count. Guarding the 0/0
//    avoids a NaN leaking into the report.

#include "atx/core/linalg/linalg.hpp" // atx::core::linalg::MatX
#include "atx/core/types.hpp"         // atx::f64

namespace atx::engine::eval {

// ===========================================================================
//  BreadthResult — the three scalars of the IR = IC·√breadth decomposition.
//
//  Trivial aggregate (Rule of Zero); owns nothing. Aggregate-initialized in
//  declaration order.
// ===========================================================================
struct BreadthResult {
  atx::f64 effective_n; // N_eff = (Σλ)² / Σλ² over the covariance eigenvalues (λ clamped ≥ 0)
  atx::f64 ic;          // realized information coefficient (caller-supplied skill per bet)
  atx::f64 ir;          // implied IR = ic · √effective_n (Fundamental Law of Active Management)
};

// ===========================================================================
//  effective_breadth — effective number of INDEPENDENT bets from a symmetric
//  PSD covariance (or correlation) matrix.
//
//    N_eff = (Σ_i λ_i)² / (Σ_i λ_i²),  λ = eigenvalues, each clamped to max(λ, 0).
//
//  K orthogonal equal-variance bets (cov = c·I) ⇒ N_eff = K; K identical bets
//  (rank-1 cov) ⇒ N_eff = 1. A zero matrix (Σλ == 0) ⇒ N_eff = 0 (documented —
//  no variance, no bet).
//
//  PRECONDITION (ATX_ASSERT): `cov` is square with rows() >= 1. The matrix must
//  be symmetric for the symmetric eigensolver (a covariance always is); a non-
//  symmetric input is rejected by the solver and surfaces as N_eff = 0 with a
//  logged reason rather than a silent wrong answer.
// ===========================================================================
[[nodiscard]] atx::f64 effective_breadth(const atx::core::linalg::MatX &cov);

// ===========================================================================
//  breadth_decomposition — the full IR = IC·√breadth split: N_eff from `cov`,
//  then IR = ic · √N_eff.
//
//  `ic` is the realized information coefficient (the skill term), supplied by the
//  caller because it is a property of the forecast/realization pairing, not of the
//  covariance. PRECONDITION (ATX_ASSERT): `ic` is finite.
// ===========================================================================
[[nodiscard]] BreadthResult breadth_decomposition(const atx::core::linalg::MatX &cov, atx::f64 ic);

} // namespace atx::engine::eval
