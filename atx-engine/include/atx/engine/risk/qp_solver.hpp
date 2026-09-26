#pragma once

// atx::engine::risk — ConstrainedQpSolver: a DETERMINISTIC fixed-iteration,
// OSQP-class operator-splitting (ADMM) solver for the constrained portfolio QP
// (S1-2 / S8.1 / S8.2 / S8.3). Solves
//
//     minimize_w   ½ wᵀP w + qᵀw
//     subject to   l ≤ A w ≤ u                         (the S1-1 linear rows)
//                  Σ|w|          ≤ gross_l1_budget      (gross L1, aux-split)
//                  Σ|w − w_prev| ≤ turnover_budget      (turnover L1, aux-split)
//
// with P = 2λV.
//
// ===========================================================================
//  S8.1 REWRITE — factor-augmented SPARSE KKT (kills the O(M²) dense-Ã defect)
// ===========================================================================
//  The as-built solver materialized the augmented constraint matrix Ã DENSE over
//  n≈3M variables and ran a fixed 600×50 Jacobi-PCG — O(M²)/iter (§0.2). S8.1
//  reformulates the SAME QP into the FACTOR-AUGMENTED SPARSE form (qp_augment.hpp):
//  introduce y = Xᵀw so the dense XFXᵀ risk term moves into K factor space and the
//  Hessian becomes the sparse  P = blkdiag(2λD, 2λF, 0)  over x = [w; y; s; r]. The
//  public API (`solve(const QpProblem&)`, `QpProblem`, `QpConfig`) is UNCHANGED in
//  shape (S8.3 ADDS optional warm-start fields to QpProblem and re-tunes/extends
//  QpConfig — both backward-compatible aggregate additions); only the internals
//  change. `q = −α_aim`, `P = 2λV` — the same problem the oracle
//  (`qp_solver_reference.hpp`) solves the dense way, agreeing at the optimum.
//
//  The ADMM x-update is now a DIRECT SPARSE KKT SOLVE of the OSQP quasi-definite
//  system (Vanderbei 1995; OSQP §3, Stellato 2020)
//
//      [ P + σI    Ãᵀ   ] [ x ]   [ σ x_k − q̃              ]
//      [ Ã       −ρ⁻¹I  ] [ ν ] = [ z_k − ρ⁻¹ y_k          ]
//
//  factored ONCE per solve by the deterministic no-pivot QuasiDefiniteLdl
//  (kkt_ldl.hpp, S8.2) — a static AMD-ordered LDLᵀ that is byte-identical across
//  threads/builds (no Bunch-Kaufman pivot branch). V is NEVER densified (R4).
//
// ===========================================================================
//  S8.3 — Ruiz equilibration + re-tuned budget + polish + certificates + warm-start
// ===========================================================================
//  (A) RE-TUNED BUDGET. The dense-PCG-era `iters=600` default is gone. The sparse
//      direct-KKT x-update is EXACT (no inner PCG — the old `kkt_iters` count was
//      removed in S8.8a), so the only thing the outer count buys is ADMM
//      primal/dual convergence. The default is a FIXED iters=300 (see QpConfig::iters)
//      — honored VERBATIM, never problem-scaled (R1: the budget IS the algorithm). The
//      Ruiz conditioning + polish let 300 clear the common augmented path; NO early-exit.
//
//  (B) RUIZ EQUILIBRATION (R1 — fixed 10 passes, symmetry-preserving). Before the
//      ADMM, the symmetric system matrix  M = [[P, Ãᵀ],[Ã, 0]]  is rescaled by a
//      diagonal D over a FIXED `ruiz_passes` (no ε early-exit) so every row/col of
//      DMD has unit ∞-norm; a cost-scaling factor c rescales the objective. The
//      scaling D splits into D_x (variables) and E (constraint rows); the solve runs
//      on the conditioned problem  (P̄,Ã̄,q̄,l̄,ū) = (c·D_x P D_x, E Ã D_x, c·D_x q,
//      E l, E u)  and the returned book is UN-scaled back to original units
//      (w = D_x x̄ on the w-block). This conditions the no-pivot LDLᵀ on the
//      ill-conditioned L1-split cases (Ruiz 2001; OSQP §5).
//
//  (C) DETERMINISTIC POLISH (R1 — fixed 3 refinement passes, OSQP §4). After the
//      fixed ADMM, the active set is partitioned by the dual sign, ONE reduced-KKT
//      system is solved for the high-accuracy book, and a FIXED `polish_refine`
//      iterative-refinement passes tighten it. The polished book is ACCEPTED only if
//      its primal/dual pair passes feasibility and residual checks. The objective
//      must not increase when the prior ADMM book is itself feasible; an infeasible
//      iterate's objective cannot veto a feasible polish. The choice is a pure
//      deterministic function of the iterates (no tolerance gate on loop counts).
//
//  (D) INFEASIBILITY CERTIFICATES (deterministic, from iterate differences). The
//      primal/dual residuals and the OSQP infeasibility detectors are computed from
//      the final iterate deltas and surfaced on `QpResult::cert` — near-free,
//      deterministic, no early-exit.
//
//  (E) WARM-START (R6 — decoupled from termination). `QpProblem::x0/y0` optionally
//      seed the ADMM (x,z,y) instead of the zero seed. The loop STILL runs its fixed
//      count regardless, so the output stays a fixed-length operator composition —
//      warm-start is an accuracy lever, never a control-flow change.
//
// ===========================================================================
//  Determinism (R1 / R5 / G-DET)
// ===========================================================================
//  Every count is FIXED — Ruiz passes, ADMM outer iters, polish refinement passes.
//  There is NO residual / convergence early-exit anywhere; the iteration budget IS
//  the algorithm. Duals y and the splitting variable z are zero-initialized (or set
//  from the optional warm-start). The KKT is assembled in a fixed CSC traversal
//  order (qp_augment.hpp / build_kkt) and factored ONCE per solve, then reused every
//  iteration. The factorization is the no-pivot QuasiDefiniteLdl (S8.2): symbolic
//  (AMD order + elimination tree) then numeric, both order-fixed and purely serial.
//  No RNG, no clock, no threading. All hand-rolled reductions run in canonical
//  ascending order. Same inputs ⇒ a byte-identical book on ANY thread count (G-DET).
//
// ===========================================================================
//  Feasibility gate (R3)
// ===========================================================================
//  After the FIXED loop (and optional polish), Ã x is checked against [l̃, ũ]
//  row-by-row in ORIGINAL units. A violation beyond cfg.feas_tol ⇒ Err(InvalidArgument)
//  naming the offending row — a genuinely infeasible set is reported, NEVER returned
//  as a silently-clamped book. The returned book is the first M entries (w-block).

#include <limits>    // default unlimited factor payload budget
#include <span>      // std::span (q, warm-start)
#include <vector>    // std::vector (result)


#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/admm_schedule.hpp" // AdmmSchedule / WarmStart (Lane 6 deterministic adaptive rho)
#include "atx/engine/risk/constraints.hpp"  // MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp" // FactorModel (apply / specific_var / factor_cov)
#include "atx/engine/risk/qp_augment.hpp"   // build_augmented / AugmentedQp

namespace atx::engine::risk {

// ADMM configuration. Every count is FIXED — there is no convergence test anywhere
// (R1); the iteration budget IS the algorithm.
struct QpConfig {
  // FIXED outer ADMM iterations — NO early-exit (R1). The `iters` value is the HARD
  // outer count: the loop runs exactly this many passes, never more or fewer (it is
  // never a floor/ceiling on a residual early-exit — there is none). S8.3 re-tune: the
  // dense-PCG-era 600 default is replaced by 300, because the sparse rewrite's x-update
  // is an EXACT direct KKT solve (no inner PCG to amortize — the old `kkt_iters` is gone) AND
  // S8.3's Ruiz equilibration conditions the KKT + the deterministic polish recovers
  // the final-digit accuracy the old solver chased with raw outer iterations. 300
  // clears the common augmented path (pure-linear AND the gross/turnover L1 aux-split,
  // which Ruiz conditioning lets converge in far fewer outer steps than the 600/1600
  // the un-equilibrated solver needed). Callers needing a specific budget (the bench's
  // 4×4 apples-to-apples gate, the integration tests) set this explicitly and it is
  // honored VERBATIM — no silent problem-scaled bump (that would break the bench's
  // fixed-budget comparison and R1's "the budget IS the algorithm").
  atx::usize iters = 300;
  // (S8.8a) The dead `kkt_iters` field — the old inner-PCG count, unused since the
  // S8.1 direct-KKT rewrite made the x-update exact — is REMOVED. The reference/oracle
  // solver (qp_solver_reference.hpp) keeps its own real `kkt_iters` (a genuine PCG depth).
  atx::f64 rho = 1.0;        // ADMM constraint penalty
  atx::f64 sigma = 1e-6;     // proximal regularization (KKT well-posedness / quasi-definiteness)
  atx::f64 feas_tol = 1e-6;  // post-loop feasibility tolerance (R3)

  // ---- S8.3 additions (all FIXED counts — R1) -----------------------------
  atx::usize ruiz_passes = 10; // FIXED Ruiz equilibration passes (R1, no ε early-exit).
                               // 0 ⇒ scaling disabled (identity D, c=1) — used by tests
                               // that want the raw un-equilibrated path for comparison.
  bool polish = true;          // run the deterministic active-set polish after the ADMM.
  atx::usize polish_refine = 3; // FIXED iterative-refinement passes inside the polish.
  // Maximum requested Lp/Li/Lx/D/Dinv payload for EACH direct factorization
  // (ADMM and optional polish). Symbolic/runtime/other solver storage is excluded.
  // A budget failure is a solve error even when the factorization is for polish.
  atx::u64 max_factor_bytes = std::numeric_limits<atx::u64>::max();
  // Lane 6: route the SCHEDULED solve (solve_with_cert(p, sched, ws)) through the factor-space
  // ADMM (qp_factor_admm.hpp) — risk exact in a Woodbury x-update, O(M(K + R_d)) per
  // iteration, no KKT factorization — whenever the set carries no cone. Off by default: the
  // augmented path and every byte pin are unchanged. The two paths share cfg.iters (a cap
  // under an early-exit schedule), rho, sigma, feas_tol and polish; ruiz_passes and
  // max_factor_bytes do not apply to the factor-space path. Its x_full / y_full use their
  // own layout (x_full = w; see qp_factor_admm.hpp); a warm start from the other path's
  // layout is detected by length and ignored (a cold start, never a mis-seeded one).
  bool factor_space = false;
  ConstraintFeasibilityRule feasibility_rule{ConstraintFeasibilityRule::LegacyAbsoluteV1};
  atx::f64 feasibility_relative_tolerance{0.0};
};

// A deterministic infeasibility / convergence certificate, computed from the FINAL
// iterate differences (OSQP §3.4) — near-free and order-fixed (R1). Surfaced on
// QpResult; `solve()` keeps the historical Result<vector> contract and discards it.
struct QpCertificate {
  atx::f64 prim_res = 0.0; // ‖Ãx − z‖∞  (primal residual, original units)
  atx::f64 dual_res = 0.0; // ‖P x + q̃ + Ãᵀy‖∞ (dual residual, original units)
  bool primal_infeasible = false; // OSQP primal-infeasibility detector fired
  bool dual_infeasible = false;   // OSQP dual-infeasibility detector fired
  bool polished = false;          // the returned book is the polished one (else the ADMM book)
  // Lane 6: ADMM iterations actually run (== cfg.iters unless a schedule's deterministic
  // early exit fired) and the final base rho (== cfg.rho on the unscheduled path).
  atx::usize admm_iters = 0;
  atx::f64 rho_final = 0.0;
  bool factor_space = false; // the factor-space ADMM produced this result (cfg.factor_space)
};

// The QP instance. P = 2·risk_aversion·V (V is NEVER densified — the augmentation
// re-expresses it in K factor space via FactorModel::factor_cov / specific_var).
//
// S8.3 adds the OPTIONAL warm-start spans x0/y0 (R6). They default to empty (the
// historical zero-seed behavior); the existing aggregate initialization
// `QpProblem{V, λ, q, C}` is unchanged (the new fields default-construct).
struct QpProblem {
  const FactorModel &V;             // P = 2λV via the y=Xᵀw augmentation — V never densified
  atx::f64 risk_aversion;           // λ (so P = 2λV)
  std::span<const atx::f64> q;      // linear term (length M) = −α_aim
  const MaterializedConstraints &C; // l ≤ A w ≤ u + the L1 budgets
  // Optional warm-start (R6) — decoupled from termination. If non-empty:
  //   x0: length n = n_w+n_y+n_aux (the full augmented primal). A length-M x0 is
  //       accepted as the w-block only (y/aux seeded 0) for caller convenience.
  //   y0: length R̃ (the augmented dual). Empty ⇒ the historical zero seed.
  std::span<const atx::f64> x0 = {};
  std::span<const atx::f64> y0 = {};
};

// The full solve result: the book plus the deterministic certificate. `solve()`
// returns just the book (historical contract); `solve_with_cert()` returns both.
struct QpResult {
  std::vector<atx::f64> book; // length-M weight vector (the w-block of x)
  QpCertificate cert;
  // S8.5c diagnostic — the achieved epigraph apex t of each VARIABLE-APEX cone block,
  // in cone-emission order (entry b is block b's apex; non-variable-apex / ball blocks
  // contribute NO entry). The apex is the cone's row_start row of Ãx at the returned
  // (un-scaled, polished) x: for the robust alpha cone Ãx[row_start] == t and the SOC
  // enforces ‖Ω_f^{1/2} y‖₂ ≤ t, so at the optimum t == ‖Ω_f^{1/2} y‖₂ (the epigraph
  // binds). Surfaced so a test can assert the SOC is TIGHT (not merely that the penalty
  // moved the book) without a behavior change: the returned book and every byte-identity
  // pin are untouched — `solve()` discards this field, and it is EMPTY whenever the
  // problem carries no variable-apex cone (box-only / ball-only paths, R10).
  std::vector<atx::f64> cone_apex;
  // Lane 6 warm-start handles: the full augmented primal x = [w; y; aux] and dual (one
  // entry per augmented row), ORIGINAL units, after polish. Feed them back through
  // WarmStart on the next solve of a problem with the SAME augmented layout.
  std::vector<atx::f64> x_full;
  std::vector<atx::f64> y_full;
};

// Implementation details live in src/risk/qp_solver.cpp. Keeping the solver
// boundary here prevents changes to Ruiz/KKT/ADMM/polish from rebuilding callers.
class ConstrainedQpSolver {
public:
  QpConfig cfg;
  [[nodiscard]] static atx::core::Status check_problem(const QpProblem& p);
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>> solve(const QpProblem& p) const;
  [[nodiscard]] atx::core::Result<QpResult> solve_with_cert(const QpProblem& p) const;
  [[nodiscard]] atx::core::Result<QpResult> solve_with_cert(
      const QpProblem& p, const AdmmSchedule& sched, const WarmStart* ws = nullptr) const;
  [[nodiscard]] atx::core::Result<QpResult> solve_augmented_form(
      const AugmentedQp& aug, const QpProblem& p) const;
  [[nodiscard]] atx::core::Result<QpResult> solve_augmented_form(
      const AugmentedQp& aug, const QpProblem& p,
      const AdmmSchedule* sched, const WarmStart* ws) const;
};

} // namespace atx::engine::risk
