#pragma once

// atx::engine::risk — garleanu_pedersen: the Gârleanu-Pedersen aim + value-curvature,
// the SCALAR-Λ (Λ = λΣ) one-period reduction of the GP cost-to-go, S8.7.
//
// ===========================================================================
//  What this unit is (and what it is NOT)
// ===========================================================================
//  Gârleanu & Pedersen, "Dynamic Trading with Predictable Returns and Transaction
//  Costs," J. Finance 68(6):2309-2340, 2013. For QUADRATIC trading cost + MEAN-
//  REVERTING signals the GP optimal dynamic policy is an affine feedback: each period
//  TRADE PARTWAY toward an AIM portfolio under a value-function cost-to-go, where the
//  general value matrix A_xx solves an ALGEBRAIC RICCATI equation.
//
//  WHAT WE SHIP (the adopted codebase convention, sprint-1 plan §0.6 / §0.4): the
//  SCALAR-Λ reduction. With Λ = λΣ the GP trade rate degenerates to the scalar
//  cfg.trade_rate, the aim is the MARKOWITZ portfolio of the horizon-blended alpha, and
//  the cost-to-go value matrix is A_xx = 2λV — the PLAIN single-period Markowitz Hessian
//  (NO Riccati solve is performed; A_xx is literally the one-period P = 2λV). Under this
//  reduction the cost-to-go fold is provably INERT (q = −ᾱ, P = 2λV unchanged; see below),
//  which is exactly why it stays byte-identical to the S7 single-period book on the
//  boundary (sprint-1 §0.4 pins "full trade-rate ⇒ aim == single-period Markowitz target",
//  §0.6 decides "S1.4 ships the GP aim-portfolio collapse" and records the full QP as a lift).
//
//  WHAT IS THE RECORDED LIFT (NOT shipped here): the full MATRIX-Riccati A_xx — a
//  non-trivial H>1 cost-to-go CURVATURE (A_xx ≠ 2λV) that adds genuine multi-period
//  curvature to P and only REDUCES to 2λV in the H=1 single-period limit. Computing that
//  steady-state Riccati A_xx is the recorded refinement (a steady-state Riccati does NOT
//  reduce to the one-period Hessian for H>1, so shipping it would change the augmented book
//  and risk the boundary pin). This header does NOT solve a Riccati; it applies A_xx = 2λV.
//
//  This header is the PURE closed form of the scalar-Λ reduction — the unconstrained FAST
//  PATH (no solver call) AND the differential anchor for the constrained QP.
//
//    aim_pos = A_xx⁻¹ A_xf f_t          (the position the policy trades toward)
//    trade   = x_{t-1} + Λ⁻¹A_xx(aim_pos − x_{t-1})   (Λ = λΣ ⇒ scalar trade_rate)
//
// ===========================================================================
//  The signal-model mapping (THE key S8.7 design decision — documented)
// ===========================================================================
//  GP's (f_t, decay Φ, Σ, Λ) ↔ this codebase, fixed by the SAME closed form sprint-1
//  already committed to (sprint-1 plan §A2, G&P Prop. 4 / Eq. 15) so the boundary pin
//  and the oracle both fall out by construction:
//
//    * Σ          = the FactorModel V (= XFXᵀ + D), consumed FACTORED (never densified,
//                   R4) via V.apply_inverse — the (2λV)⁻¹ apply is O(MK + K³).
//    * Λ = λΣ      ⇒ the GP trade rate is the SCALAR cfg.trade_rate ∈ (0,1] (the existing
//                   S7/S1 partial step; the matrix Riccati rate is the recorded refinement).
//    * f_t / Φ    = the per-name α cross-section with its SignalHorizon decay ALREADY
//                   baked into forecast_trajectory's rows traj.alpha[h]. The return-space
//                   GP aim ᾱ = A_xf f_t is the decay-weighted blend of those rows — here
//                   the horizon-average MultiHorizonOptimizer::gp_aim already computes:
//                   a slow (persistent) source keeps ~α_t at every row so it earns a
//                   large average; a fast source decays toward 0 so it is down-weighted
//                   (Eq. 15's per-factor 1/(1+φ_k·a/λ) discounting, realized through the
//                   decayed trajectory rows). ᾱ is the linear term q = −ᾱ.
//    * A_xx        = the Markowitz value-curvature 2λV — NOT a solved Riccati matrix but
//                   literally the single-period Hessian P the augmented QP already builds.
//                   Under the scalar-Λ reduction the cost-to-go value matrix IS the plain
//                   2λV at EVERY H (not merely in a limit), so the cost-to-go tail reduces
//                   to the single-period objective and is INERT (R10). The full matrix-
//                   Riccati A_xx (≠ 2λV for H>1) is the recorded lift; this header ships
//                   A_xx = 2λV, the curvature the unconstrained fast path uses.
//
//  ⇒ closed-form position aim  aim_pos = A_xx⁻¹ A_xf f_t = (2λV)⁻¹ ᾱ = (1/2λ)·V⁻¹ ᾱ.
//
//  Degenerate collapse (H=1 + SignalHorizon::identity() + full trade-rate): identity
//  decay ⇒ every traj row == α_t ⇒ ᾱ == α_t bit-identically (the gp_aim average of equal
//  rows). The cost-to-go tail +½(w−aim_pos)ᵀ(2λV)(w−aim_pos) = ½wᵀ(2λV)w − ᾱᵀw + const is
//  the SINGLE-PERIOD objective, so folding it changes NOTHING; and the minimal-constraint
//  dispatch (the pinned fast path) never touches P/q at all ⇒ byte-identical to the S7
//  book (R10). The GP machinery is purely additive on the AUGMENTED path.
//
// ===========================================================================
//  The cost-to-go P/q fold (the MPC trick) — matching the 2λ convention
// ===========================================================================
//  GP's value function adds a tail −½(w−aim_pos)ᵀA_xx(w−aim_pos) to the MAXIMIZE
//  objective; in our MINIMIZE QP ½wᵀPw + qᵀw it is +½(w−aim_pos)ᵀA_xx(w−aim_pos). With
//  A_xx = 2λV that expands to ½wᵀ(2λV)w − (2λV·aim_pos)ᵀw + const = ½wᵀ(2λV)w − ᾱᵀw +
//  const, i.e. EXACTLY the curvature P = 2λV the augmented build already emits plus the
//  linear term q = −ᾱ. So the fold is: q ← −ᾱ (the decay-weighted return-space aim), P
//  unchanged at 2λV (R5 — no new factorization, and no Riccati is solved: A_xx IS the
//  one-period 2λV, applied through the cached factor capacitance, never re-factored). The
//  unconstrained argmin of this objective is (2λV)⁻¹ᾱ = aim_pos.
//
// ===========================================================================
//  Determinism (R1)
// ===========================================================================
//  NO RNG / clock / map. ᾱ is the order-fixed horizon reduction (gp_aim); the (2λV)⁻¹
//  apply is the deterministic cached-Cholesky Woodbury (FactorModel::apply_inverse).
//  Same inputs ⇒ a byte-identical aim across runs/threads (G-DET).

#include <span>    // std::span
#include <vector>  // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/risk/fwd.hpp" // FactorModel (fwd decl — apply path used in the .cpp body)

namespace atx::engine::risk {

// ===========================================================================
//  GpAimValue — the closed-form GP result.
//
//  * alpha_bar  : the RETURN-space decay-weighted aim ᾱ = A_xf f_t (length M). NaN
//                 names ("no opinion") are PRESERVED here and mapped to a 0 linear
//                 coefficient by the QP fold (mirrors the existing q = −aim policy).
//  * aim_pos    : the POSITION-space closed-form aim (2λV)⁻¹ᾱ (length M) — the
//                 unconstrained fast-path book direction. NaN ᾱ names are treated as 0
//                 in the V⁻¹ apply (a no-opinion name carries no return tilt).
//
//  A_xx is the value-curvature 2λV; it is NOT materialized here (R4) — the QP fold
//  reuses the P = 2λV the augmented build already emits, and aim_pos is computed via
//  the factored (2λV)⁻¹ apply. The documented identity A_xx·aim_pos == ᾱ is what makes
//  the cost-to-go tail collapse to q = −ᾱ.
// ===========================================================================
struct GpAimValue {
  std::vector<atx::f64> alpha_bar; // ᾱ = A_xf f_t (return space) — the q = −ᾱ linear term
  std::vector<atx::f64> aim_pos;   // (2λV)⁻¹ ᾱ (position space) — closed-form / oracle target
};

// ---------------------------------------------------------------------------
//  gp_aim_and_value — the closed-form aim + value-function curvature.
//
//  Given the decay-weighted RETURN-space aim ᾱ (= MultiHorizonOptimizer::gp_aim of the
//  forecast trajectory; the A_xf f_t blend) and the factored risk model V at risk
//  aversion λ, return BOTH:
//    * alpha_bar = ᾱ (echoed through; the q = −ᾱ fold term), and
//    * aim_pos   = (2λV)⁻¹ ᾱ = (1/2λ)·V⁻¹ ᾱ  via the factored Woodbury apply (R4).
//
//  λ == 0 is the no-risk-curvature limit: A_xx = 0 has no inverse, so the position aim
//  is undefined and we return aim_pos == ᾱ (the pure-alpha direction — consistent with
//  PortfolioOptimizer's λ=0 demean(α) fast path, which is itself scale-invariant). The
//  caller's λ=0 path never uses aim_pos for a curvature fold (P = 0 then), so this is a
//  benign, documented convention, not a silent error.
//
//  Err(InvalidArgument) on a length mismatch (alpha_bar.size() != V.n_instruments()).
//  `[[nodiscard]]`. Deterministic (R1): order-fixed copy + the cached-Cholesky apply.
// ---------------------------------------------------------------------------
// Body in src/risk/garleanu_pedersen.cpp (S8.8a split). Deterministic (R1): the
// order-fixed copy + the cached-Cholesky factored (2λV)⁻¹ apply. Err(InvalidArgument)
// on a length mismatch (alpha_bar.size() != V.n_instruments()) or lambda < 0.
[[nodiscard]] atx::core::Result<GpAimValue>
gp_aim_and_value(std::span<const atx::f64> alpha_bar, const FactorModel &V, atx::f64 lambda);

} // namespace atx::engine::risk
