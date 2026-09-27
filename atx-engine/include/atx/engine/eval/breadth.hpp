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
//  * Default PsdTraceV2 uses the equivalent trace/Frobenius identity in O(K^2)
//    time and O(1) scratch, scaling finite entries before compensated reductions.
//    Inputs must be PSD; this mode does not repair indefinite matrices. Shape,
//    finite values, nonnegative diagonal and symmetry are checked in every build.
//  * LegacyClippedEigenV1 eigenvalues come from the symmetric eigensolver,
//    which returns them ASCENDING. We clamp each to max(λ, 0): a covariance is
//    PSD in exact arithmetic, but a finite-precision eigensolver can emit a tiny
//    NEGATIVE eigenvalue for a (near-)singular input (e.g. the rank-1 identical-
//    bets case). A negative λ is physically a zero-variance direction; clamping
//    keeps Σλ² honest and never lets a numerical artifact inflate or sign-flip
//    the ratio.
//  * Each rule has a fixed reduction order and is deterministic. The two rules
//    are numerically equivalent on PSD inputs, not promised bit-identical.
//  * A zero matrix (every λ clamped to 0 ⇒ Σλ == 0) is DOCUMENTED to yield
//    N_eff = 0: there is no variance, hence no bet to count. Guarding the 0/0
//    avoids a NaN leaking into the report.

#include "atx/core/linalg/linalg.hpp" // atx::core::linalg::MatX
#include "atx/core/types.hpp"         // atx::f64

namespace atx::engine::eval {

// V2 uses tr(C)^2 / ||C||_F^2, an O(K^2) identity for symmetric PSD
// covariance. PSD is a caller precondition, not an O(K^3) validation step.
// V1 retains eigenvalue clipping and its historical reduction order for replay
// and for callers deliberately supplying an indefinite diagnostic matrix.
enum class BreadthRule : atx::u8 { LegacyClippedEigenV1 = 1, PsdTraceV2 = 2 };

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
  BreadthRule rule{BreadthRule::PsdTraceV2};
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
//  PRECONDITION: `cov` is symmetric PSD, square with rows() >= 1. V2 reports 0
//  for malformed shape/nonfinite entries/negative diagonal/asymmetry. PSD itself
//  is the caller's contract, not verified by decomposition. Use explicit V1 for
//  historical clipped-eigenvalue behavior on an indefinite diagnostic matrix.
// ===========================================================================
[[nodiscard]] atx::f64 effective_breadth(const atx::core::linalg::MatX &cov,
    BreadthRule rule = BreadthRule::PsdTraceV2);

// ===========================================================================
//  breadth_decomposition — the full IR = IC·√breadth split: N_eff from `cov`,
//  then IR = ic · √N_eff.
//
//  `ic` is the realized information coefficient (the skill term), supplied by the
//  caller because it is a property of the forecast/realization pairing, not of the
//  covariance. PRECONDITION (ATX_ASSERT): `ic` is finite.
// ===========================================================================
[[nodiscard]] BreadthResult breadth_decomposition(const atx::core::linalg::MatX &cov, atx::f64 ic,
    BreadthRule rule = BreadthRule::PsdTraceV2);

} // namespace atx::engine::eval
