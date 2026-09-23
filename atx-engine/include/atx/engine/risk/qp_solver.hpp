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

#include <algorithm> // std::max
#include <cmath>     // std::fabs, std::sqrt, std::isfinite
#include <iomanip>   // full-precision infeasibility diagnostics
#include <limits>    // default unlimited factor payload budget
#include <sstream>   // cold-path diagnostic formatting
#include <span>      // std::span (q, warm-start)
#include <string>    // std::string, std::to_string (feasibility-gate Err message)
#include <utility>   // std::move
#include <vector>    // std::vector (result)

#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/admm_schedule.hpp" // AdmmSchedule / WarmStart (Lane 6 deterministic adaptive rho)
#include "atx/engine/risk/cone.hpp"         // SocBlock + ball_project + soc_project (deterministic SOC z-projection, S8.5a/c)
#include "atx/engine/risk/constraints.hpp"  // MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp" // FactorModel (apply / specific_var / factor_cov)
#include "atx/engine/risk/kkt_ldl.hpp"      // QuasiDefiniteLdl (deterministic no-pivot KKT solve)
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

class ConstrainedQpSolver {
public:
  QpConfig cfg;

  // Up-front shape/finiteness validation of a QpProblem, exactly as solve_with_cert runs
  // it. Exposed for callers that assemble their own augmented form (cost_terms.hpp).
  [[nodiscard]] static atx::core::Status check_problem(const QpProblem &p) {
    return validate(p, p.V.n_instruments());
  }

  // Solve the augmented constrained QP. Returns the length-M weight vector w (the
  // w-block of x). Err(InvalidArgument) on a dimension mismatch up front or an
  // infeasible set after the fixed loop. NEVER a silently-clamped book. (Historical
  // contract — unchanged; delegates to solve_with_cert and discards the certificate.)
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>> solve(const QpProblem &p) const {
    namespace co = atx::core;
    ATX_TRY(QpResult r, solve_with_cert(p));
    return co::Ok(std::move(r.book));
  }

  // Solve and also return the deterministic certificate (S8.3). Same fixed-iteration
  // algorithm; the certificate is computed from the final iterates.
  [[nodiscard]] atx::core::Result<QpResult> solve_with_cert(const QpProblem &p) const {
    const atx::usize m = p.V.n_instruments();
    ATX_TRY_VOID(validate(p, m));

    // (1) Build the factor-augmented sparse form  x = [w; y; s; r]  (R4 — no dense Ã).
    const AugmentedQp aug = build_augmented(p.V, p.risk_aversion, p.q, p.C);

    // (2..8) The augmented-form solve, extracted so the S8.6 elasticity layer can re-solve
    //        a RELAXED AugmentedQp (penalized slack columns) through the IDENTICAL pipeline
    //        (same Ruiz / ADMM / polish / certificate / gate — NO new factorization, R5).
    //        The hard path runs this VERBATIM ⇒ a feasible solve stays byte-identical (R10).
    return solve_augmented_form(aug, p);
  }

  // Lane 6: the same solve driven by a deterministic adaptive-rho schedule (per-row rho with
  // equality rows boosted, rho adapted only at fixed iteration indices and snapped to a power
  // of two, optional deterministic early exit), optionally warm-started from a previous
  // QpResult's x_full / y_full. The unscheduled overload above is untouched.
  [[nodiscard]] atx::core::Result<QpResult> solve_with_cert(const QpProblem &p,
                                                            const AdmmSchedule &sched,
                                                            const WarmStart *ws = nullptr) const {
    const atx::usize m = p.V.n_instruments();
    ATX_TRY_VOID(validate(p, m));
    const AugmentedQp aug = build_augmented(p.V, p.risk_aversion, p.q, p.C);
    return solve_augmented_form(aug, p, &sched, ws);
  }

  // Solve a PRE-BUILT augmented form. The hard path (solve_with_cert) calls this directly
  // after build_augmented; the S8.6 elasticity layer (risk/elasticity.hpp) calls it with a
  // RELAXED AugmentedQp it assembled (the minimize-violation slack columns + penalty appended
  // to the w/y/aux variable space, the elastic rows widened by the slacks). `aug.n_w` is the
  // M weight block in BOTH cases, so the returned book is the first M entries either way. NO
  // new factorization (R5): the SAME Ruiz-conditioned no-pivot LDLᵀ ADMM as the hard solve.
  // The book of a NON-relaxed aug equals the hard solve's book bit-for-bit (it IS the hard
  // solve's tail).
  [[nodiscard]] atx::core::Result<QpResult> solve_augmented_form(const AugmentedQp &aug,
                                                                 const QpProblem &p) const {
    return solve_augmented_form(aug, p, nullptr, nullptr);
  }

  // As above, optionally scheduled (sched != nullptr ⇒ run_admm_scheduled) and optionally
  // warm-started (ws overrides p.x0 / p.y0). sched == nullptr && ws == nullptr is the
  // historical fixed-rho operator, byte for byte.
  [[nodiscard]] atx::core::Result<QpResult> solve_augmented_form(const AugmentedQp &aug,
                                                                 const QpProblem &p_in,
                                                                 const AdmmSchedule *sched,
                                                                 const WarmStart *ws) const {
    namespace co = atx::core;
    const QpProblem p = (ws == nullptr)
                            ? QpProblem{p_in.V, p_in.risk_aversion, p_in.q, p_in.C, p_in.x0, p_in.y0}
                            : QpProblem{p_in.V, p_in.risk_aversion, p_in.q, p_in.C, ws->x0, ws->y0};
    namespace cl = atx::core::linalg;
    const atx::usize m = p.V.n_instruments();

    // (2) Ruiz equilibration (R1, fixed cfg.ruiz_passes) — build the conditioned
    //     problem (P̄, Ã̄, q̄, l̄, ū) and the unscaling vectors (D_x, c). Disabled
    //     (identity) when cfg.ruiz_passes == 0.
    Scaling sc;
    const AugmentedQp scaled = equilibrate(aug, sc);

    // (3) Run the deterministic fixed-iteration ADMM over the conditioned form. x_bar
    //     and y_bar are in SCALED units; z_bar is the splitting variable.
    cl::VecX x_bar;
    cl::VecX y_bar;
    cl::VecX z_bar;
    QpCertificate cert;
    if (sched == nullptr) {
      ATX_TRY_VOID(run_admm(scaled, p, sc, x_bar, y_bar, z_bar));
      cert.admm_iters = cfg.iters;
      cert.rho_final = cfg.rho;
    } else {
      ATX_TRY_VOID(run_admm_scheduled(scaled, p, sc, *sched, x_bar, y_bar, z_bar, cert));
    }

    // (4) Un-scale to original units:  x = D_x x̄,  y = (1/c) E y_bar  (E = sc.e).
    cl::VecX x = unscale_primal(x_bar, sc);
    cl::VecX y = unscale_dual(y_bar, sc);

    // (5) Deterministic polish (R1, OSQP §4) — optional; guards the coherent
    //     primal/dual pair and compares objectives only to a feasible ADMM book.
    if (cfg.polish) {
      ATX_TRY_VOID(polish_book(aug, p, x, y, cert));
    }

    // (6) Certificate from the final original-unit iterates (deterministic).
    fill_certificate(aug, x, y, z_bar, sc, cert);

    // (7) Feasibility gate (R3): Ã x ∈ [l̃, ũ] within feas_tol (original units), else Err.
    ATX_TRY_VOID(check_feasible(aug, x));

    // (8) Return the w-block (first M entries) in canonical order + the certificate.
    QpResult out;
    out.book.resize(m);
    for (atx::usize i = 0; i < m; ++i) {
      out.book[i] = x[static_cast<Eigen::Index>(i)];
    }
    out.cert = cert;
    out.x_full.assign(x.data(), x.data() + x.size());
    out.y_full.assign(y.data(), y.data() + y.size());
    // (8b) S8.5c diagnostic — surface the achieved epigraph apex t of each variable-apex
    //      cone (the cone's row_start row of Ãx at the final x). PURELY a read of the
    //      already-built Ã and the returned x; it does NOT touch out.book or the solve.
    //      EMPTY when there is no variable-apex cone ⇒ no effect on any existing path.
    fill_cone_apex(aug, x, out.cone_apex);
    return co::Ok(std::move(out));
  }

private:
  using SpMat = Eigen::SparseMatrix<atx::f64>;

  // Ruiz scaling state. D_x scales the n primal columns, e scales the R̃ constraint
  // rows; c is the scalar cost scaling. All diagonal/positive; the solve runs on the
  // conditioned problem and unscales with these.
  struct Scaling {
    std::vector<atx::f64> d_x; // (n) primal column scales
    std::vector<atx::f64> e;   // (R̃) constraint row scales
    atx::f64 c = 1.0;          // scalar cost scaling
  };

  // -------------------------------------------------------------------------
  //  Up-front dimension validation (R3). Infeasible-by-shape ⇒ InvalidArgument.
  // -------------------------------------------------------------------------
  [[nodiscard]] static atx::core::Status validate(const QpProblem &p, atx::usize m) {
    namespace co = atx::core;
    if (p.q.size() != m) {
      return co::Err(co::ErrorCode::InvalidArgument, "QP: q.size() must equal M (n_instruments)");
    }
    const auto rows = p.C.A.rows();
    if (rows > 0 && static_cast<atx::usize>(p.C.A.cols()) != m) {
      return co::Err(co::ErrorCode::InvalidArgument, "QP: A.cols() must equal M");
    }
    if (p.C.l.size() != rows || p.C.u.size() != rows) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "QP: A.rows() must equal l.size() == u.size()");
    }
    if (!std::isfinite(p.C.turnover_penalty) || p.C.turnover_penalty < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "QP: turnover_penalty must be finite and nonnegative");
    }
    if (p.C.has_turnover || p.C.turnover_penalty > 0.0) {
      if (p.C.turnover_ref.size() != m) {
        return co::Err(co::ErrorCode::InvalidArgument, "QP: turnover_ref length must equal M");
      }
      for (const atx::f64 value : p.C.turnover_ref) {
        if (!std::isfinite(value)) {
          return co::Err(co::ErrorCode::InvalidArgument, "QP: turnover_ref must be finite");
        }
      }
    }
    if (p.risk_aversion < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "QP: risk_aversion must be >= 0");
    }
    return co::Ok();
  }

  // -------------------------------------------------------------------------
  //  Ruiz equilibration (R1) — FIXED cfg.ruiz_passes, symmetry-preserving. Scales
  //  the symmetric system  M = [[P, Ãᵀ],[Ã, 0]]  so every row/col of D M D has unit
  //  ∞-norm, accumulating the diagonal D = D_x ⊕ E. A final cost-scaling factor c
  //  rescales the objective row (mean column-∞-norm of P̄ after equilibration). The
  //  returned AugmentedQp is the CONDITIONED problem; `sc` carries the unscaling.
  //
  //  No ε early-exit: the pass count is fixed and IS the algorithm. With ruiz_passes
  //  == 0 the scaling is the identity (D_x = E = 1, c = 1) and `scaled` aliases `aug`
  //  numerically (a fresh copy, same values).
  // -------------------------------------------------------------------------
  [[nodiscard]] AugmentedQp equilibrate(const AugmentedQp &aug, Scaling &sc) const {
    const atx::usize n = aug.n_w + aug.n_y + aug.n_aux;
    const atx::usize r = static_cast<atx::usize>(aug.A_tilde.rows());

    sc.d_x.assign(n, 1.0);
    sc.e.assign(r, 1.0);
    sc.c = 1.0;

    // Working copies we rescale in place across passes. P stays symmetric; Ã is the
    // off-diagonal block. We track running column scales of P (= row scales, P is
    // symmetric) and the constraint row/col scales.
    SpMat P = aug.P;
    SpMat A = aug.A_tilde;

    for (atx::usize pass = 0; pass < cfg.ruiz_passes; ++pass) {
      // (a) Column ∞-norms of the symmetric system matrix M = [[P, Ãᵀ],[Ã, 0]].
      //     For a variable column j (j < n): max over P column j AND Ã column j.
      //     For a constraint column n+i: max over Ã row i (= Ãᵀ column n+i).
      std::vector<atx::f64> col_inf(n + r, 0.0);
      for (atx::usize c = 0; c < n; ++c) {
        atx::f64 mx = 0.0;
        for (SpMat::InnerIterator it(P, static_cast<int>(c)); it; ++it) {
          mx = std::max(mx, std::fabs(it.value()));
        }
        for (SpMat::InnerIterator it(A, static_cast<int>(c)); it; ++it) {
          // Ã entry (row i, col c) lives in M at row n+i, col c AND (symmetric) at
          // row c, col n+i. It contributes to BOTH this var column c's norm and the
          // constraint column n+i's norm.
          const atx::f64 v = std::fabs(it.value());
          mx = std::max(mx, v);
          const atx::usize ci = n + static_cast<atx::usize>(it.row());
          col_inf[ci] = std::max(col_inf[ci], v);
        }
        col_inf[c] = std::max(col_inf[c], mx);
      }

      // (b) The symmetric scaling step δ_j = 1/sqrt(col_inf_j) (skip zero columns).
      std::vector<atx::f64> delta(n + r, 1.0);
      for (atx::usize j = 0; j < n + r; ++j) {
        const atx::f64 cij = col_inf[j];
        delta[j] = (cij > 0.0) ? 1.0 / std::sqrt(cij) : 1.0;
      }
      // (b') Cone rows are pinned UN-scaled (δ = 1) — a cone block is projected as a
      //      EUCLIDEAN ball/SOC, and an anisotropic per-row scale would warp the ball
      //      into an ellipsoid with no closed-form projection (R5/R1). Pinning the cone
      //      rows keeps the projection isotropic and exact in Ãx units. NO-OP when
      //      aug.cones is empty ⇒ the box-only equilibration is byte-identical to S8.4.
      for (const SocBlock &blk : aug.cones) {
        for (atx::usize j = 0; j < blk.dim; ++j) {
          delta[n + blk.row_start + j] = 1.0;
        }
      }

      // (c) Apply D = diag(δ) symmetrically: P ← D_x P D_x, Ã ← E Ã D_x.
      apply_scale_sym(P, delta.data());                 // P uses δ[0..n)
      apply_scale_rect(A, delta.data() + n, delta.data()); // Ã rows use δ[n..), cols δ[0..n)

      // (d) Accumulate into the running scaling.
      for (atx::usize j = 0; j < n; ++j) {
        sc.d_x[j] *= delta[j];
      }
      for (atx::usize i = 0; i < r; ++i) {
        sc.e[i] *= delta[n + i];
      }
    }

    // (e) Cost scaling c: 1 / max(mean ∞-norm of P̄'s columns, ‖q̄_grad‖). OSQP scales
    //     the objective by the reciprocal of a representative gradient magnitude; we
    //     use the mean column ∞-norm of the equilibrated P (bounded, deterministic).
    if (cfg.ruiz_passes > 0U) {
      atx::f64 acc = 0.0;
      atx::usize cnt = 0;
      for (atx::usize c = 0; c < n; ++c) {
        atx::f64 mx = 0.0;
        for (SpMat::InnerIterator it(P, static_cast<int>(c)); it; ++it) {
          mx = std::max(mx, std::fabs(it.value()));
        }
        if (mx > 0.0) {
          acc += mx;
          ++cnt;
        }
      }
      const atx::f64 mean = (cnt > 0U) ? acc / static_cast<atx::f64>(cnt) : 1.0;
      sc.c = (mean > 0.0) ? 1.0 / mean : 1.0;
    }

    // (f) Materialize the conditioned problem. P̄ = c · (already-equilibrated P);
    //     Ã̄ = (already-equilibrated A); q̄ = c · D_x q; l̄ = E l; ū = E u.
    AugmentedQp out;
    out.n_w = aug.n_w;
    out.n_y = aug.n_y;
    out.n_aux = aug.n_aux;
    out.P = P;
    if (sc.c != 1.0) {
      out.P *= sc.c;
    }
    out.P.makeCompressed();
    out.A_tilde = A;
    out.A_tilde.makeCompressed();
    out.q_aug = aug.q_aug;
    for (atx::usize j = 0; j < n; ++j) {
      out.q_aug[static_cast<Eigen::Index>(j)] *= sc.c * sc.d_x[j];
    }
    out.l = aug.l;
    out.u = aug.u;
    for (atx::usize i = 0; i < r; ++i) {
      // l/u may carry the ±kAugInf sentinel; scaling by a finite positive e keeps the
      // band well-ordered (a huge number times a small positive stays huge-positive).
      out.l[static_cast<Eigen::Index>(i)] *= sc.e[i];
      out.u[static_cast<Eigen::Index>(i)] *= sc.e[i];
    }
    // Carry the cone blocks onto the scaled problem UNCHANGED: their rows are pinned
    // e = 1 (step b' above) so the scaled splitting variable on those rows is in the
    // SAME Ãx units as the original — the ball radius and offset apply verbatim.
    out.cones = aug.cones;
    return out;
  }

  // P ← diag(d) P diag(d) in place (symmetric). Each stored entry (row i, col j) is
  // multiplied by d[i]·d[j]. Fixed CSC traversal order.
  static void apply_scale_sym(SpMat &P, const atx::f64 *d) {
    for (int c = 0; c < P.outerSize(); ++c) {
      for (SpMat::InnerIterator it(P, c); it; ++it) {
        it.valueRef() *= d[static_cast<atx::usize>(it.row())] * d[static_cast<atx::usize>(c)];
      }
    }
  }

  // A ← diag(d_row) A diag(d_col) in place (rectangular). Entry (row i, col j) ×
  // d_row[i]·d_col[j]. Fixed CSC traversal order.
  static void apply_scale_rect(SpMat &A, const atx::f64 *d_row, const atx::f64 *d_col) {
    for (int c = 0; c < A.outerSize(); ++c) {
      for (SpMat::InnerIterator it(A, c); it; ++it) {
        it.valueRef() *=
            d_row[static_cast<atx::usize>(it.row())] * d_col[static_cast<atx::usize>(c)];
      }
    }
  }

  // -------------------------------------------------------------------------
  //  Assemble the OSQP quasi-definite KKT matrix (R5) of the (already-scaled)
  //  augmented problem:
  //      [ P + σI    Ãᵀ   ]
  //      [ Ã       −ρ⁻¹I  ]
  //  built in a FIXED triplet order (P + σI block first by column, then the Ã / Ãᵀ
  //  off-diagonals, then the −ρ⁻¹I block) so the CSC pattern is reproducible (R1).
  // -------------------------------------------------------------------------
  [[nodiscard]] SpMat build_kkt(const AugmentedQp &aug) const {
    using Trip = Eigen::Triplet<atx::f64>;
    const auto n = static_cast<int>(aug.n_w + aug.n_y + aug.n_aux);
    const int r = static_cast<int>(aug.A_tilde.rows());
    const int dim = n + r;
    const atx::f64 neg_rho_inv = -1.0 / cfg.rho;

    std::vector<Trip> trips;
    trips.reserve(static_cast<atx::usize>(aug.P.nonZeros() + 2 * aug.A_tilde.nonZeros() + dim));

    // (a) (1,1) block: P + σI. Walk P in CSC order; add σ on the diagonal exactly
    //     once per column.
    for (int c = 0; c < n; ++c) {
      bool diag_seen = false;
      for (SpMat::InnerIterator it(aug.P, c); it; ++it) {
        atx::f64 v = it.value();
        if (it.row() == c) {
          v += cfg.sigma;
          diag_seen = true;
        }
        trips.emplace_back(it.row(), c, v);
      }
      if (!diag_seen) {
        trips.emplace_back(c, c, cfg.sigma); // σ on an otherwise-empty diagonal (aux cols)
      }
    }

    // (b) off-diagonals: Ãᵀ in the top-right and Ã in the bottom-left.
    for (int c = 0; c < n; ++c) {
      for (SpMat::InnerIterator it(aug.A_tilde, c); it; ++it) {
        const int i = it.row();
        const atx::f64 v = it.value();
        trips.emplace_back(n + i, c, v); // Ã  (bottom-left)
        trips.emplace_back(c, n + i, v); // Ãᵀ (top-right)
      }
    }

    // (c) (2,2) block: −ρ⁻¹I.
    for (int i = 0; i < r; ++i) {
      trips.emplace_back(n + i, n + i, neg_rho_inv);
    }

    SpMat kkt(dim, dim);
    kkt.setFromTriplets(trips.begin(), trips.end());
    kkt.makeCompressed();
    return kkt;
  }

  // -------------------------------------------------------------------------
  //  The fixed-iteration ADMM (R1) over the (scaled) sparse augmented form. z, y
  //  zero-init (deterministic seed) OR seeded from the optional warm-start. The KKT
  //  is constant for fixed ρ, σ, so it is factored ONCE and reused every iteration.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Status run_admm(const AugmentedQp &aug, const QpProblem &p,
                                           const Scaling &sc, atx::core::linalg::VecX &x_out,
                                           atx::core::linalg::VecX &y_out,
                                           atx::core::linalg::VecX &z_out) const {
    namespace co = atx::core;
    namespace cl = atx::core::linalg;
    const auto n = static_cast<Eigen::Index>(aug.n_w + aug.n_y + aug.n_aux);
    const Eigen::Index r = aug.A_tilde.rows();
    const Eigen::Index dim = n + r;

    const SpMat kkt = build_kkt(aug);
    QuasiDefiniteLdl ldl;
    ATX_TRY_VOID(ldl.factor_symbolic(kkt, cfg.max_factor_bytes));
    ATX_TRY_VOID(ldl.factor_numeric(kkt));

    const atx::f64 rho_inv = 1.0 / cfg.rho;
    cl::VecX x = cl::VecX::Zero(n);
    cl::VecX z = cl::VecX::Zero(r);
    cl::VecX y = cl::VecX::Zero(r);

    // Warm-start (R6) — seed (x, z, y) in SCALED units. x0/y0 are in ORIGINAL units,
    // so scale them in:  x̄ = D_x⁻¹ x,  ȳ = c E⁻¹ y. z is seeded from Ã̄ x̄ (consistent
    // with the splitting variable). The loop count is UNCHANGED regardless.
    seed_warm_start(p, sc, aug, x, z, y);

    std::vector<atx::f64> rhs(static_cast<atx::usize>(dim), 0.0);
    std::vector<atx::f64> sol(static_cast<atx::usize>(dim), 0.0);

    for (atx::usize it = 0; it < cfg.iters; ++it) { // FIXED — no early-exit (R1)
      // (1) x-update: solve the KKT system. RHS top = σx − q̃; bottom = z − ρ⁻¹y.
      for (Eigen::Index i = 0; i < n; ++i) {
        rhs[static_cast<atx::usize>(i)] = cfg.sigma * x[i] - aug.q_aug[i];
      }
      for (Eigen::Index i = 0; i < r; ++i) {
        rhs[static_cast<atx::usize>(n + i)] = z[i] - rho_inv * y[i];
      }
      ldl.solve(std::span<const atx::f64>(rhs), std::span<atx::f64>(sol));
      for (Eigen::Index i = 0; i < n; ++i) {
        x[i] = sol[static_cast<atx::usize>(i)];
      }

      // (2) z-update: clamp(Ã x + y/ρ, l̃, ũ) elementwise (order-fixed), THEN overwrite
      //     each cone block's row range with the SOC/ball projection. The box rows are
      //     untouched by the cone step; with zero cone blocks the loop is byte-identical
      //     to S8.4 (R10 — the cone is purely a z-projection swap, R5).
      const cl::VecX ax = aug.A_tilde * x;
      for (Eigen::Index i = 0; i < r; ++i) {
        const atx::f64 t = ax[i] + y[i] / cfg.rho;
        z[i] = clamp(t, aug.l[i], aug.u[i]);
      }
      project_cones(aug, ax, y, z);
      // (3) y-update: y += ρ (Ã x − z).
      y += cfg.rho * (ax - z);
    }

    x_out = std::move(x);
    y_out = std::move(y);
    z_out = std::move(z);
    return co::Ok();
  }

  // -------------------------------------------------------------------------
  //  Lane 6: the SCHEDULED ADMM (admm_schedule.hpp). Differences from run_admm, all
  //  deterministic:
  //    * per-row rho: equality rows (l == u, finite, not a cone row) use
  //      rho·eq_rho_scale (OSQP rho_eq) so the factor-definition rows converge fast;
  //    * at the schedule's fixed iteration indices rho adapts by residual balancing,
  //      snapped to a power of two; a change beyond adapt_ratio rewrites the −1/rho
  //      diagonal and re-runs ONLY the numeric LDLᵀ (the pattern is unchanged);
  //    * optional early exit tested only at multiples of check_every.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Status run_admm_scheduled(const AugmentedQp &aug, const QpProblem &p,
                                                     const Scaling &sc, const AdmmSchedule &s,
                                                     atx::core::linalg::VecX &x_out,
                                                     atx::core::linalg::VecX &y_out,
                                                     atx::core::linalg::VecX &z_out,
                                                     QpCertificate &cert) const {
    namespace co = atx::core;
    namespace cl = atx::core::linalg;
    const auto n = static_cast<Eigen::Index>(aug.n_w + aug.n_y + aug.n_aux);
    const Eigen::Index r = aug.A_tilde.rows();
    const Eigen::Index dim = n + r;

    std::vector<bool> is_cone_row(static_cast<atx::usize>(r), false);
    for (const SocBlock &blk : aug.cones) {
      for (atx::usize j = 0; j < blk.dim; ++j) {
        is_cone_row[blk.row_start + j] = true;
      }
    }
    cl::VecX mult = cl::VecX::Ones(r);
    for (Eigen::Index i = 0; i < r; ++i) {
      const bool eq = aug.l[i] == aug.u[i] && std::fabs(aug.l[i]) < kAugInf;
      if (eq && !is_cone_row[static_cast<atx::usize>(i)]) {
        mult[i] = s.eq_rho_scale;
      }
    }
    atx::f64 rho = cfg.rho;
    cl::VecX rho_vec = rho * mult;
    cl::VecX rho_inv = rho_vec.cwiseInverse();

    SpMat kkt = build_kkt(aug);
    set_kkt_rho_diag(kkt, n, rho_inv);
    QuasiDefiniteLdl ldl;
    ATX_TRY_VOID(ldl.factor_symbolic(kkt, cfg.max_factor_bytes));
    ATX_TRY_VOID(ldl.factor_numeric(kkt));

    cl::VecX x = cl::VecX::Zero(n);
    cl::VecX z = cl::VecX::Zero(r);
    cl::VecX y = cl::VecX::Zero(r);
    seed_warm_start(p, sc, aug, x, z, y);

    std::vector<atx::f64> rhs(static_cast<atx::usize>(dim), 0.0);
    std::vector<atx::f64> sol(static_cast<atx::usize>(dim), 0.0);
    cl::VecX ax(r);
    cl::VecX zr(r);
    cl::VecX xt(n);
    const atx::f64 alpha = s.relax_alpha;
    atx::usize done = 0;
    for (atx::usize it = 0; it < cfg.iters; ++it) { // bounded by cfg.iters
      for (Eigen::Index i = 0; i < n; ++i) {
        rhs[static_cast<atx::usize>(i)] = cfg.sigma * x[i] - aug.q_aug[i];
      }
      for (Eigen::Index i = 0; i < r; ++i) {
        rhs[static_cast<atx::usize>(n + i)] = z[i] - rho_inv[i] * y[i];
      }
      ldl.solve(std::span<const atx::f64>(rhs), std::span<atx::f64>(sol));
      // Over-relaxation (OSQP §3, alpha in (0,2)): x ← αx̃ + (1−α)x, and the relaxed
      // splitting target zr = αÃx̃ + (1−α)z drives the z- and y-updates.
      for (Eigen::Index i = 0; i < n; ++i) {
        x[i] = alpha * sol[static_cast<atx::usize>(i)] + (1.0 - alpha) * x[i];
      }
      for (Eigen::Index i = 0; i < n; ++i) {
        xt[i] = sol[static_cast<atx::usize>(i)];
      }
      ax.noalias() = aug.A_tilde * xt;
      for (Eigen::Index i = 0; i < r; ++i) {
        zr[i] = alpha * ax[i] + (1.0 - alpha) * z[i];
      }
      for (Eigen::Index i = 0; i < r; ++i) {
        z[i] = clamp(zr[i] + rho_inv[i] * y[i], aug.l[i], aug.u[i]);
      }
      project_cones_rho(aug, zr, y, rho, z); // cone rows carry mult 1 ⇒ scalar rho
      for (Eigen::Index i = 0; i < r; ++i) {
        y[i] += rho_vec[i] * (zr[i] - z[i]);
      }
      done = it + 1U;

      const bool refactor = is_refactor_point(s, done);
      const bool check = s.early_exit && s.check_every > 0U && done % s.check_every == 0U;
      if (!refactor && !check) {
        continue;
      }
      ax.noalias() = aug.A_tilde * x;
      const AdmmResidualNorms norms = residual_norms(aug, x, z, y, ax);
      if (check && residuals_converged(norms, s)) {
        break;
      }
      if (refactor) {
        const atx::f64 next = adapt_rho(rho, norms, s);
        if (next >= rho * s.adapt_ratio || next * s.adapt_ratio <= rho) {
          rho = next;
          rho_vec = rho * mult;
          rho_inv = rho_vec.cwiseInverse();
          set_kkt_rho_diag(kkt, n, rho_inv);
          ATX_TRY_VOID(ldl.factor_numeric(kkt));
        }
      }
    }
    cert.admm_iters = done;
    cert.rho_final = rho;
    x_out = std::move(x);
    y_out = std::move(y);
    z_out = std::move(z);
    return co::Ok();
  }

  // Overwrite the (2,2) block diagonal of an assembled KKT with −rho_inv (pattern fixed).
  static void set_kkt_rho_diag(SpMat &kkt, Eigen::Index n, const atx::core::linalg::VecX &rho_inv) {
    for (Eigen::Index i = 0; i < rho_inv.size(); ++i) {
      kkt.coeffRef(n + i, n + i) = -rho_inv[i];
    }
  }

  // ∞-norm residual summary in SCALED units (the schedule's adaptation / exit input).
  [[nodiscard]] static AdmmResidualNorms residual_norms(const AugmentedQp &aug,
                                                        const atx::core::linalg::VecX &x,
                                                        const atx::core::linalg::VecX &z,
                                                        const atx::core::linalg::VecX &y,
                                                        const atx::core::linalg::VecX &ax) {
    namespace cl = atx::core::linalg;
    const cl::VecX px = aug.P * x;
    const cl::VecX aty = aug.A_tilde.transpose() * y;
    AdmmResidualNorms out;
    out.prim = (ax - z).lpNorm<Eigen::Infinity>();
    out.dual = (px + aug.q_aug + aty).lpNorm<Eigen::Infinity>();
    out.ax = ax.lpNorm<Eigen::Infinity>();
    // z on inert (±kAugInf) rows is bounded by |Ãx| there, so the ∞-norm stays finite.
    out.z = z.lpNorm<Eigen::Infinity>();
    out.px = px.lpNorm<Eigen::Infinity>();
    out.aty = aty.lpNorm<Eigen::Infinity>();
    out.q = aug.q_aug.lpNorm<Eigen::Infinity>();
    return out;
  }

  // Cone z-projection with an explicit scalar rho (the scheduled loop's current base rho).
  void project_cones_rho(const AugmentedQp &aug, const atx::core::linalg::VecX &ax,
                         const atx::core::linalg::VecX &y, atx::f64 rho,
                         atx::core::linalg::VecX &z) const {
    for (const SocBlock &blk : aug.cones) {
      const atx::usize d = blk.dim;
      std::vector<atx::f64> arg(d, 0.0);
      std::vector<atx::f64> proj(d, 0.0);
      for (atx::usize j = 0; j < d; ++j) {
        const auto row = static_cast<Eigen::Index>(blk.row_start + j);
        arg[j] = (ax[row] + y[row] / rho) + blk.offset[static_cast<Eigen::Index>(j)];
      }
      project_block_arg(blk, arg, proj);
      for (atx::usize j = 0; j < d; ++j) {
        const auto row = static_cast<Eigen::Index>(blk.row_start + j);
        z[row] = proj[j] - blk.offset[static_cast<Eigen::Index>(j)];
      }
    }
  }

  // Seed (x, z, y) from the optional warm-start (R6), in SCALED units. No-op (leaves
  // the zero seed) when x0/y0 are empty. Tolerant of a length-M x0 (w-block only).
  void seed_warm_start(const QpProblem &p, const Scaling &sc, const AugmentedQp &aug,
                       atx::core::linalg::VecX &x, atx::core::linalg::VecX &z,
                       atx::core::linalg::VecX &y) const {
    namespace cl = atx::core::linalg;
    const atx::usize n = aug.n_w + aug.n_y + aug.n_aux;
    const atx::usize r = static_cast<atx::usize>(aug.A_tilde.rows());

    bool seeded = false;
    if (!p.x0.empty()) {
      const atx::usize take = (p.x0.size() == n) ? n : (p.x0.size() == aug.n_w ? aug.n_w : 0U);
      for (atx::usize i = 0; i < take; ++i) {
        // x̄ = x / D_x  (un-scale the column scaling: original = D_x · scaled).
        x[static_cast<Eigen::Index>(i)] =
            (sc.d_x[i] != 0.0) ? p.x0[i] / sc.d_x[i] : p.x0[i];
      }
      seeded = seeded || take > 0U;
    }
    if (!p.y0.empty() && p.y0.size() == r) {
      for (atx::usize i = 0; i < r; ++i) {
        // ȳ = c · y / E  (dual scaling inverse of unscale_dual).
        y[static_cast<Eigen::Index>(i)] = sc.c * ((sc.e[i] != 0.0) ? p.y0[i] / sc.e[i] : p.y0[i]);
      }
      seeded = true;
    }
    if (seeded) {
      // Seed z consistently with the splitting variable: z = clamp(Ã̄ x̄, l̄, ū) on the
      // box rows, then the SOC/ball projection on each cone block — the SAME operator the
      // loop's z-update applies (with the seed target Ã̄ x̄, no y/ρ term, since y is being
      // seeded separately). Keeping this consistent stops the warm-started and cold paths
      // from diverging on the cone rows.
      const cl::VecX ax = aug.A_tilde * x;
      for (atx::usize i = 0; i < r; ++i) {
        z[static_cast<Eigen::Index>(i)] = clamp(ax[static_cast<Eigen::Index>(i)],
                                                aug.l[static_cast<Eigen::Index>(i)],
                                                aug.u[static_cast<Eigen::Index>(i)]);
      }
      for (const SocBlock &blk : aug.cones) {
        project_cone_block(blk, ax, z);
      }
    }
  }

  // Un-scale the primal:  x = D_x x̄  (column scaling). Returns original-unit x.
  [[nodiscard]] atx::core::linalg::VecX unscale_primal(const atx::core::linalg::VecX &x_bar,
                                                       const Scaling &sc) const {
    atx::core::linalg::VecX x = x_bar;
    for (Eigen::Index i = 0; i < x.size(); ++i) {
      x[i] *= sc.d_x[static_cast<atx::usize>(i)];
    }
    return x;
  }

  // Un-scale the dual:  y = (1/c) E ȳ. The scaled problem's dual relates to the
  // original via the row scaling E and the cost scaling c (OSQP §5).
  [[nodiscard]] atx::core::linalg::VecX unscale_dual(const atx::core::linalg::VecX &y_bar,
                                                     const Scaling &sc) const {
    atx::core::linalg::VecX y = y_bar;
    const atx::f64 inv_c = (sc.c != 0.0) ? 1.0 / sc.c : 1.0;
    for (Eigen::Index i = 0; i < y.size(); ++i) {
      y[i] *= sc.e[static_cast<atx::usize>(i)] * inv_c;
    }
    return y;
  }

  // -------------------------------------------------------------------------
  //  Deterministic polish (R1, OSQP §4). Partition the constraint rows into the
  //  active set (those clamped at a finite bound, identified by the dual sign) and
  //  the inactive set, solve ONE reduced KKT system that enforces the active rows as
  //  equalities, and run FIXED cfg.polish_refine iterative-refinement passes. The
  //  result is ACCEPTED only if finite, feasible (including cones), dual-consistent,
  //  and no worse in residuals above explicit tiny floors. Compare objectives only
  //  when the previous ADMM book is feasible. Operates on `aug` (ORIGINAL units);
  //  `x` AND `y` are replaced together, never a polished primal with stale duals.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Status polish_book(
      const AugmentedQp &aug, const QpProblem &p, atx::core::linalg::VecX &x,
      atx::core::linalg::VecX &y, QpCertificate &cert) const {
    namespace co = atx::core;
    namespace cl = atx::core::linalg;
    const auto n = static_cast<Eigen::Index>(aug.n_w + aug.n_y + aug.n_aux);
    const Eigen::Index r = aug.A_tilde.rows();
    if (r == 0) {
      return co::Ok(); // no constraints ⇒ nothing to polish
    }

    // Cone rows are NEVER linear-active: a SOC row is governed by the ball projection,
    // not a finite [l, u] bound (its band is ±kAugInf), so it must be EXCLUDED from the
    // active-set polish KKT — pinning it to ±kAugInf would corrupt the reduced solve.
    // The mask is all-false when aug.cones is empty ⇒ the partition is byte-identical to
    // S8.4 (R10).
    std::vector<bool> is_cone_row(static_cast<atx::usize>(r), false);
    for (const SocBlock &blk : aug.cones) {
      for (atx::usize j = 0; j < blk.dim; ++j) {
        is_cone_row[blk.row_start + j] = true;
      }
    }

    // (a) Active-set partition by dual sign (OSQP §4.1): a row is active-at-lower if
    //     y_i < −tol, active-at-upper if y_i > tol, else inactive. The active rows are
    //     enforced as equalities A_act x = bnd_act in the reduced KKT.
    const atx::f64 dual_tol = 1e-8;
    std::vector<int> active; // row indices, ascending (fixed order)
    std::vector<atx::f64> bnd;
    active.reserve(static_cast<atx::usize>(r));
    bnd.reserve(static_cast<atx::usize>(r));
    for (Eigen::Index i = 0; i < r; ++i) {
      if (is_cone_row[static_cast<atx::usize>(i)]) {
        continue; // cone row ⇒ handled by the projection, not the active-set KKT
      }
      const atx::f64 yi = y[i];
      if (yi < -dual_tol) {
        active.push_back(static_cast<int>(i));
        bnd.push_back(aug.l[i]);
      } else if (yi > dual_tol) {
        active.push_back(static_cast<int>(i));
        bnd.push_back(aug.u[i]);
      } else {
        // Inactive by dual; also pin rows that sit exactly on a finite bound (the
        // factor-definition equalities have l==u and tiny duals — keep them active).
        if (aug.l[i] == aug.u[i] && std::fabs(aug.l[i]) < kAugInf) {
          active.push_back(static_cast<int>(i));
          bnd.push_back(aug.l[i]);
        }
      }
    }
    const auto na = static_cast<Eigen::Index>(active.size());
    if (na == 0) {
      return co::Ok(); // no active rows ⇒ unconstrained interior; the ADMM book stands
    }

    // (b) Build the reduced KKT  [[P+σpI, A_actᵀ],[A_act, 0]]  with a tiny δ on the
    //     (2,2) block for quasi-definiteness, and factor it once with the no-pivot LDL.
    const atx::f64 sig_p = cfg.sigma; // proximal reg for the polish KKT
    const atx::f64 delta = 1e-8;      // (2,2) regularization → quasi-definite
    SpMat A_act(static_cast<int>(na), static_cast<int>(n));
    {
      using Trip = Eigen::Triplet<atx::f64>;
      std::vector<Trip> at;
      at.reserve(static_cast<atx::usize>(A_act.rows() == 0 ? 0 : aug.A_tilde.nonZeros()));
      // Column scan: for each column c, emit (k, c) for active rows present in c.
      // Build an inverse map row->active-index for O(nnz) assembly (fixed order).
      std::vector<int> row_to_k(static_cast<atx::usize>(r), -1);
      for (Eigen::Index k = 0; k < na; ++k) {
        row_to_k[static_cast<atx::usize>(active[static_cast<atx::usize>(k)])] = static_cast<int>(k);
      }
      for (int c = 0; c < static_cast<int>(n); ++c) {
        for (SpMat::InnerIterator it(aug.A_tilde, c); it; ++it) {
          const int kk = row_to_k[static_cast<atx::usize>(it.row())];
          if (kk >= 0) {
            at.emplace_back(kk, c, it.value());
          }
        }
      }
      A_act.setFromTriplets(at.begin(), at.end());
      A_act.makeCompressed();
    }

    SpMat rkkt = build_reduced_kkt(aug.P, A_act, n, na, sig_p, delta);
    QuasiDefiniteLdl ldl;
    const auto symbolic = ldl.factor_symbolic(rkkt, cfg.max_factor_bytes);
    if (!symbolic) {
      if (symbolic.error().code() == co::ErrorCode::OutOfRange) {
        return co::Err(symbolic.error()); // resource bound is mandatory, including polish
      }
      return co::Ok(); // ordinary optional polish failure ⇒ keep the ADMM book
    }
    if (!ldl.factor_numeric(rkkt)) {
      return co::Ok(); // degenerate reduced system ⇒ keep the ADMM book
    }

    // (c) The target is the UNREGULARIZED KKT system K0*t = g with g=[-q;b].
    //     Kreg=[[P+sigma I,A_act^T],[A_act,-delta I]] is only a preconditioner:
    //     solve Kreg*correction = g-K0*t, then add the correction. Refining the
    //     residual of Kreg instead would retain its regularization bias.
    //     See OSQP section 4, eqs (27)-(31): https://arxiv.org/pdf/1711.08013.
    //
    //     ATX seeds t from the ADMM primal/selected duals. This differs from OSQP's
    //     initial solve of Kreg*t=g: inactive zero-cost gross auxiliaries otherwise
    //     become zero and can violate |w|<=s despite a correct w. The seed preserves
    //     useful free auxiliary components; ALL candidate guards remain mandatory.
    //     One initial correction plus cfg.polish_refine corrections retains the
    //     exact previous solve count and the same single capped factorization.
    const Eigen::Index pdim = n + na;
    std::vector<atx::f64> rhs(static_cast<atx::usize>(pdim), 0.0);
    cl::VecX solv(pdim);
    for (Eigen::Index i = 0; i < n; ++i) {
      rhs[static_cast<atx::usize>(i)] = -aug.q_aug[i];
      solv[i] = x[i];
    }
    for (Eigen::Index k = 0; k < na; ++k) {
      rhs[static_cast<atx::usize>(n + k)] = bnd[static_cast<atx::usize>(k)];
      solv[n + k] = y[active[static_cast<atx::usize>(k)]];
    }
    std::vector<atx::f64> res(static_cast<atx::usize>(pdim), 0.0);
    std::vector<atx::f64> corr(static_cast<atx::usize>(pdim), 0.0);
    const auto correct_original_system = [&]() {
      const cl::VecX top = aug.P * solv.head(n) + A_act.transpose() * solv.tail(na);
      const cl::VecX bottom = A_act * solv.head(n);
      for (Eigen::Index i = 0; i < n; ++i) {
        res[static_cast<atx::usize>(i)] = rhs[static_cast<atx::usize>(i)] - top[i];
      }
      for (Eigen::Index k = 0; k < na; ++k) {
        res[static_cast<atx::usize>(n + k)] = rhs[static_cast<atx::usize>(n + k)] - bottom[k];
      }
      ldl.solve(std::span<const atx::f64>(res), std::span<atx::f64>(corr));
      for (Eigen::Index i = 0; i < pdim; ++i) {
        solv[i] += corr[static_cast<atx::usize>(i)];
      }
    };
    correct_original_system(); // Initial solve, expressed as a correction.
    for (atx::usize pass = 0; pass < cfg.polish_refine; ++pass) {
      correct_original_system(); // FIXED passes, no convergence-based early exit.
    }

    // (d) Reconstruct a complete, coherent primal/dual candidate. Cone rows were
    //     excluded from the linear relaxation and therefore receive ZERO duals,
    //     as do inactive linear rows. A linear-relaxation solution is usable only
    //     if it ALSO satisfies every original cone; stale cone duals are invalid.
    cl::VecX xp(n);
    cl::VecX yp = cl::VecX::Zero(r);
    for (Eigen::Index i = 0; i < n; ++i) {
      const atx::f64 v = solv[i];
      if (!std::isfinite(v)) {
        return co::Ok(); // numerical breakdown ⇒ keep the ADMM book
      }
      xp[i] = v;
    }
    // Explicit tiny residual/sign floor, as in OSQP's polish acceptance. It does
    // not relax the caller's primal tolerance or the tighter normal-cone check.
    constexpr atx::f64 tiny_residual = 1e-10;
    for (Eigen::Index k = 0; k < na; ++k) {
      const int row = active[static_cast<atx::usize>(k)];
      const atx::f64 nu = solv[n + k];
      if (!std::isfinite(nu)) {
        return co::Ok();
      }
      if (aug.l[row] != aug.u[row]) { // Equality multipliers have unrestricted sign.
        const bool lower = bnd[static_cast<atx::usize>(k)] == aug.l[row];
        if ((lower && nu > tiny_residual) || (!lower && nu < -tiny_residual)) {
          return co::Ok(); // The guessed active side has an invalid multiplier.
        }
      }
      yp[row] = nu;
    }
    const cl::VecX axp = aug.A_tilde * xp;
    if (!axp.allFinite() || !feasible_within(aug, xp, p)) {
      return co::Ok();
    }
    for (Eigen::Index i = 0; i < r; ++i) {
      if (is_cone_row[static_cast<atx::usize>(i)]) {
        continue; // yp=0 is in the normal cone at every feasible cone point.
      }
      const atx::f64 shifted = axp[i] + yp[i];
      if (!std::isfinite(shifted) ||
          std::fabs(axp[i] - clamp(shifted, aug.l[i], aug.u[i])) > cfg.feas_tol) {
        return co::Ok(); // Box normal-cone membership / complementarity failed.
      }
    }

    // Compare full original-unit primal distance (INCLUDING cones) and stationarity.
    // Above tiny floors neither may worsen, and at least one must improve unless
    // both are already tiny. This avoids accepting a worse KKT pair solely because
    // its objective is smaller. The hard feasibility gate still uses cfg.feas_tol.
    const auto residuals = [&](const cl::VecX &candidate_x, const cl::VecX &candidate_y) {
      const atx::f64 invalid = std::numeric_limits<atx::f64>::infinity();
      const cl::VecX a = aug.A_tilde * candidate_x;
      const cl::VecX stationarity = aug.P * candidate_x + aug.q_aug +
                                   aug.A_tilde.transpose() * candidate_y;
      if (!a.allFinite() || !stationarity.allFinite()) {
        return std::pair{invalid, invalid};
      }
      atx::f64 primal = 0.0;
      atx::f64 dual = 0.0;
      for (Eigen::Index i = 0; i < a.size(); ++i) {
        primal = std::max(primal, std::max(a[i] - aug.u[i], aug.l[i] - a[i]));
      }
      for (const SocBlock &blk : aug.cones) {
        std::vector<atx::f64> arg(blk.dim, 0.0);
        for (atx::usize j = 0; j < blk.dim; ++j) {
          arg[j] = a[static_cast<Eigen::Index>(blk.row_start + j)] +
                   blk.offset[static_cast<Eigen::Index>(j)];
        }
        const atx::f64 violation = cone_violation(blk, arg);
        if (!std::isfinite(violation)) {
          return std::pair{invalid, invalid};
        }
        primal = std::max(primal, violation);
      }
      for (Eigen::Index i = 0; i < stationarity.size(); ++i) {
        dual = std::max(dual, std::fabs(stationarity[i]));
      }
      return std::pair{primal, dual};
    };
    const auto [old_primal, old_dual] = residuals(x, y);
    const auto [new_primal, new_dual] = residuals(xp, yp);
    const atx::f64 primal_floor = std::min(cfg.feas_tol, tiny_residual);
    if (!std::isfinite(old_primal) || !std::isfinite(old_dual) ||
        !std::isfinite(new_primal) || !std::isfinite(new_dual) ||
        new_primal > std::max(old_primal, primal_floor) ||
        new_dual > std::max(old_dual, tiny_residual) ||
        (!(new_primal < old_primal || new_dual < old_dual) &&
         !(new_primal <= primal_floor && new_dual <= tiny_residual))) {
      return co::Ok();
    }
    const atx::f64 new_objective = objective(aug, xp);
    if (!std::isfinite(new_objective)) {
      return co::Ok();
    }
    if (feasible_within(aug, x, p)) {
      const atx::f64 old_objective = objective(aug, x);
      if (!std::isfinite(old_objective) || new_objective > old_objective + 1e-12) {
        return co::Ok();
      }
    }
    x = std::move(xp);
    y = std::move(yp);
    cert.polished = true;
    return co::Ok();
  }

  // Reduced KKT  [[P+σI, A_actᵀ],[A_act, −δI]]  (upper triangle stored is fine for the
  // no-pivot LDL, which reads the upper triangle). Fixed triplet order.
  [[nodiscard]] static SpMat build_reduced_kkt(const SpMat &P, const SpMat &A_act, Eigen::Index n,
                                               Eigen::Index na, atx::f64 sig, atx::f64 delta) {
    using Trip = Eigen::Triplet<atx::f64>;
    const Eigen::Index dim = n + na;
    std::vector<Trip> trips;
    trips.reserve(static_cast<atx::usize>(P.nonZeros() + 2 * A_act.nonZeros() + dim));
    for (int c = 0; c < static_cast<int>(n); ++c) {
      bool diag_seen = false;
      for (SpMat::InnerIterator it(P, c); it; ++it) {
        atx::f64 v = it.value();
        if (it.row() == c) {
          v += sig;
          diag_seen = true;
        }
        trips.emplace_back(it.row(), c, v);
      }
      if (!diag_seen) {
        trips.emplace_back(c, c, sig);
      }
    }
    for (int c = 0; c < static_cast<int>(n); ++c) {
      for (SpMat::InnerIterator it(A_act, c); it; ++it) {
        const int i = it.row();
        const atx::f64 v = it.value();
        trips.emplace_back(static_cast<int>(n) + i, c, v);
        trips.emplace_back(c, static_cast<int>(n) + i, v);
      }
    }
    for (Eigen::Index i = 0; i < na; ++i) {
      trips.emplace_back(static_cast<int>(n + i), static_cast<int>(n + i), -delta);
    }
    SpMat k(static_cast<int>(dim), static_cast<int>(dim));
    k.setFromTriplets(trips.begin(), trips.end());
    k.makeCompressed();
    return k;
  }

  // ½ xᵀP x + q̃ᵀx (order-fixed). Used to gate polish acceptance.
  [[nodiscard]] static atx::f64 objective(const AugmentedQp &aug,
                                          const atx::core::linalg::VecX &x) {
    const atx::core::linalg::VecX px = aug.P * x; // P stored FULL symmetric
    atx::f64 quad = 0.0;
    for (Eigen::Index i = 0; i < x.size(); ++i) {
      quad += x[i] * px[i];
    }
    atx::f64 lin = 0.0;
    for (Eigen::Index i = 0; i < x.size(); ++i) {
      lin += aug.q_aug[i] * x[i];
    }
    return 0.5 * quad + lin;
  }

  // Feasibility of x within feas_tol (used by the polish acceptance gate). Checks BOTH
  // the linear box bands AND each cone block's ball ‖Ãx + offset‖₂ ≤ radius — the polish
  // KKT does not model the cone (cone rows are excluded), so a polished book that lowers
  // the objective by VIOLATING the cone must be rejected here (else polish would silently
  // break the SOC). Order-fixed cone norm (R1).
  [[nodiscard]] bool feasible_within(const AugmentedQp &aug, const atx::core::linalg::VecX &x,
                                     const QpProblem & /*p*/) const {
    const atx::core::linalg::VecX ax = aug.A_tilde * x;
    for (Eigen::Index i = 0; i < ax.size(); ++i) {
      if (ax[i] - aug.u[i] > cfg.feas_tol) {
        return false;
      }
      if (aug.l[i] - ax[i] > cfg.feas_tol) {
        return false;
      }
    }
    return cones_feasible(aug, ax);
  }

  // Each cone block's SOC constraint within feas_tol (order-fixed norm, R1). For a
  // fixed-radius ball: ‖Ãx + offset‖₂ ≤ radius. For a variable-apex SOC (S8.5c): the apex
  // is arg[0] (the epigraph t) and the vector is arg[1..], so the constraint is
  // ‖vector‖₂ ≤ apex (NOT ‖whole block‖₂ ≤ radius). True (vacuously) when aug.cones is
  // empty ⇒ box-only path unchanged.
  [[nodiscard]] bool cones_feasible(const AugmentedQp &aug,
                                    const atx::core::linalg::VecX &ax) const {
    for (const SocBlock &blk : aug.cones) {
      std::vector<atx::f64> arg(blk.dim, 0.0);
      for (atx::usize j = 0; j < blk.dim; ++j) {
        arg[j] = ax[static_cast<Eigen::Index>(blk.row_start + j)] +
                 blk.offset[static_cast<Eigen::Index>(j)];
      }
      if (cone_violation(blk, arg) > cfg.feas_tol) {
        return false;
      }
    }
    return true;
  }

  // The signed SOC violation of a cone block given its argument `arg` (length blk.dim,
  // already offset-shifted): for a ball, ‖arg‖₂ − radius; for a variable-apex SOC,
  // ‖arg[1..]‖₂ − arg[0] (‖v‖₂ − t). > feas_tol ⇒ infeasible. Order-fixed norm (R1).
  [[nodiscard]] static atx::f64 cone_violation(const SocBlock &blk,
                                               const std::vector<atx::f64> &arg) {
    if (!blk.variable_apex) {
      return ordered_norm2(std::span<const atx::f64>(arg)) - blk.radius;
    }
    const std::span<const atx::f64> v(arg.data() + 1, arg.size() - 1U);
    return ordered_norm2(v) - arg[0]; // ‖v‖₂ ≤ t
  }

  // -------------------------------------------------------------------------
  //  Deterministic certificate (OSQP §3.4) from the final ORIGINAL-unit iterates.
  // -------------------------------------------------------------------------
  void fill_certificate(const AugmentedQp &aug, const atx::core::linalg::VecX &x,
                        const atx::core::linalg::VecX &y, const atx::core::linalg::VecX & /*z_bar*/,
                        const Scaling & /*sc*/, QpCertificate &cert) const {
    namespace cl = atx::core::linalg;
    const cl::VecX ax = aug.A_tilde * x;
    // Primal residual: distance of Ãx from the feasible band [l, u] (order-fixed ∞).
    atx::f64 pr = 0.0;
    for (Eigen::Index i = 0; i < ax.size(); ++i) {
      const atx::f64 over = ax[i] - aug.u[i];
      const atx::f64 under = aug.l[i] - ax[i];
      pr = std::max(pr, std::max(0.0, std::max(over, under)));
    }
    cert.prim_res = pr;
    // Dual residual: ‖P x + q̃ + Ãᵀ y‖∞.
    const cl::VecX px = aug.P * x; // P stored FULL symmetric
    const cl::VecX aty = aug.A_tilde.transpose() * y;
    atx::f64 dr = 0.0;
    for (Eigen::Index i = 0; i < x.size(); ++i) {
      dr = std::max(dr, std::fabs(px[i] + aug.q_aug[i] + aty[i]));
    }
    cert.dual_res = dr;
    // Infeasibility flags: large primal residual with bounded book ⇒ primal-infeasible
    // suspicion; both are conservative, deterministic heuristics (no early-exit). These
    // are diagnostic only — the feasibility gate (R3) is the hard contract.
    cert.primal_infeasible = pr > 1e-3;
    cert.dual_infeasible = false; // bounded box ⇒ the QP is never dual-infeasible here
  }

  // -------------------------------------------------------------------------
  //  S8.5c diagnostic — record the achieved epigraph apex t of each VARIABLE-APEX cone
  //  block (its row_start row of Ãx at the final original-unit x). For the robust alpha
  //  cone Ãx[row_start] == t (a single +1.0 at the epigraph column, offset 0), so this is
  //  the achieved apex against which a test asserts the SOC binds: t ≈ ‖Ω_f^{1/2} y‖₂.
  //  Fixed-apex (ball) blocks are SKIPPED — their apex is the constant radius, not a
  //  variable. EMPTY when aug.cones carries no variable-apex block (box / ball paths). A
  //  pure read of the already-built Ã and x: NO effect on the solve, book, or any pin.
  // -------------------------------------------------------------------------
  static void fill_cone_apex(const AugmentedQp &aug, const atx::core::linalg::VecX &x,
                             std::vector<atx::f64> &apex) {
    apex.clear();
    if (aug.cones.empty()) {
      return; // no cones ⇒ leave empty (box-only path)
    }
    const atx::core::linalg::VecX ax = aug.A_tilde * x;
    for (const SocBlock &blk : aug.cones) {
      if (!blk.variable_apex) {
        continue; // fixed-radius ball ⇒ no variable apex to surface
      }
      apex.push_back(ax[static_cast<Eigen::Index>(blk.row_start)]);
    }
  }

  // -------------------------------------------------------------------------
  //  Feasibility gate (R3): Ã x ∈ [l̃, ũ] within feas_tol on the box rows AND each cone
  //  block's ball ‖Ãx + offset‖₂ ≤ radius + feas_tol, else Err naming the offending row /
  //  cone. The cone rows carry ±kAugInf box bounds (so the elementwise scan passes them);
  //  their REAL constraint is the SOC, checked separately here.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Status check_feasible(const AugmentedQp &aug,
                                                 const atx::core::linalg::VecX &x) const {
    namespace co = atx::core;
    const co::linalg::VecX ax = aug.A_tilde * x;
    for (Eigen::Index i = 0; i < ax.size(); ++i) {
      const atx::f64 over = ax[i] - aug.u[i];  // > feas_tol ⇒ above upper bound
      const atx::f64 under = aug.l[i] - ax[i]; // > feas_tol ⇒ below lower bound
      if (over > cfg.feas_tol) {
        return co::Err(co::ErrorCode::InvalidArgument, infeasible_msg(i, over));
      }
      if (under > cfg.feas_tol) {
        return co::Err(co::ErrorCode::InvalidArgument, infeasible_msg(i, under));
      }
    }
    // Cone (SOC / ball) feasibility — order-fixed norm per block (R1). A variable-apex
    // SOC checks ‖v‖₂ ≤ t (apex = arg[0]); a ball checks ‖arg‖₂ ≤ radius (cone_violation).
    for (atx::usize b = 0; b < aug.cones.size(); ++b) {
      const SocBlock &blk = aug.cones[b];
      std::vector<atx::f64> arg(blk.dim, 0.0);
      for (atx::usize j = 0; j < blk.dim; ++j) {
        arg[j] = ax[static_cast<Eigen::Index>(blk.row_start + j)] +
                 blk.offset[static_cast<Eigen::Index>(j)];
      }
      const atx::f64 viol = cone_violation(blk, arg);
      if (viol > cfg.feas_tol) {
        return co::Err(co::ErrorCode::InvalidArgument, infeasible_cone_msg(b, viol));
      }
    }
    return co::Ok();
  }

  // Cone-feasibility-gate Err message (mirrors infeasible_msg: names BOTH causes —
  // a genuinely infeasible cone OR an under-converged fixed budget).
  [[nodiscard]] std::string infeasible_cone_msg(atx::usize block, atx::f64 violation) const {
    return "ConstrainedQpSolver::solve: book violates cone block " + std::to_string(block) +
           " (‖V^{1/2}(w−w_bench)‖₂ over budget) by " + std::to_string(violation) +
           " after the fixed iteration budget — the cone may be infeasible OR the budget "
           "(cfg.iters) is too low to converge (raise iters)";
  }

  // Compose the feasibility-gate Err message. It must NOT assert "infeasible" as
  // fact: a fixed-iteration ADMM can also return an under-converged book on a
  // FEASIBLE set if cfg.iters is too low for the aux-split, so the message names
  // BOTH causes and points at the iteration budget.
  [[nodiscard]] std::string infeasible_msg(Eigen::Index row, atx::f64 violation) const {
    std::ostringstream message;
    message << std::setprecision(std::numeric_limits<atx::f64>::max_digits10)
            << "ConstrainedQpSolver::solve: book violates constraint row " << row
            << " by " << violation << " tolerance=" << cfg.feas_tol
            << " after the fixed iteration budget — the set may be infeasible OR the "
               "budget (cfg.iters) is too low for the aux-split to converge";
    return message.str();
  }

  // -------------------------------------------------------------------------
  //  Cone z-projection (S8.5a, R5) — the ONLY place a cone enters the iteration.
  //  For each SocBlock the splitting variable z on its row range is projected onto the
  //  ball ‖Ãx + offset‖₂ ≤ radius: project (target + offset) onto the ball of `radius`,
  //  then subtract `offset` back so z stays in Ãx units (the splitting metric). `target`
  //  is the z-update target Ãx + y/ρ (loop) or Ãx (warm-start seed). Cone rows are pinned
  //  e = 1 in Ruiz, so on the scaled problem these rows are already in Ãx units — the
  //  ball stays a ball (no anisotropic warp). Order-fixed (ball_project's ‖·‖₂ is a
  //  single-accumulator ascending reduction). NO-OP when aug.cones is empty (R10).
  // -------------------------------------------------------------------------
  void project_cones(const AugmentedQp &aug, const atx::core::linalg::VecX &ax,
                     const atx::core::linalg::VecX &y, atx::core::linalg::VecX &z) const {
    if (aug.cones.empty()) {
      return; // box-only path ⇒ z untouched (byte-identical to S8.4, R10)
    }
    for (const SocBlock &blk : aug.cones) {
      // Loop target_j = ax[row] + y[row]/ρ; offset added before / subtracted after so z
      // stays in Ãx units. The geometry (ball vs variable-apex SOC) is delegated below.
      const atx::usize d = blk.dim;
      std::vector<atx::f64> arg(d, 0.0);  // (target + offset) — the cone argument a
      std::vector<atx::f64> proj(d, 0.0); // projected argument
      for (atx::usize j = 0; j < d; ++j) {
        const Eigen::Index row = static_cast<Eigen::Index>(blk.row_start + j);
        arg[j] = (ax[row] + y[row] / cfg.rho) + blk.offset[static_cast<Eigen::Index>(j)];
      }
      project_block_arg(blk, arg, proj);
      for (atx::usize j = 0; j < d; ++j) {
        const Eigen::Index row = static_cast<Eigen::Index>(blk.row_start + j);
        z[row] = proj[j] - blk.offset[static_cast<Eigen::Index>(j)]; // back to Ãx units
      }
    }
  }

  // Project the cone argument `arg` (length blk.dim) onto the block's geometry, writing
  // the projection into `proj`. Two geometries (S8.5c):
  //   * variable_apex == false (S8.5a/b): the FIXED-radius ball ‖arg‖₂ ≤ radius, via
  //     ball_project — the apex is the constant `radius`, only the vector is touched.
  //   * variable_apex == true  (S8.5c robust): the GENERAL SOC {(t,v): ‖v‖₂ ≤ t} whose
  //     apex t is arg[0] (the epigraph variable) and vector v is arg[1..], via the
  //     general soc_project — which writes BOTH the projected apex into proj[0] and the
  //     projected vector into proj[1..]. (A ball would pin proj[0]=radius; the variable
  //     apex must MOVE — this is the design-novel branch the G-DIFF gate checks.)
  // Order-fixed (both ball_project and soc_project use the ascending ordered_norm2, R1).
  static void project_block_arg(const SocBlock &blk, const std::vector<atx::f64> &arg,
                                std::vector<atx::f64> &proj) {
    if (!blk.variable_apex) {
      ball_project(std::span<const atx::f64>(arg), blk.radius, std::span<atx::f64>(proj));
      return;
    }
    // Variable-apex SOC: arg[0] is the apex t, arg[1..] is the vector v.
    const atx::usize d = blk.dim; // == 1 + (vector length)
    std::span<const atx::f64> v_in(arg.data() + 1, d - 1U);
    std::span<atx::f64> v_out(proj.data() + 1, d - 1U);
    proj[0] = soc_project(arg[0], v_in, v_out); // projected apex t*; v* written into v_out
  }

  // The warm-start-seed variant: target_j = ax[row] (no y/ρ term). Same project-and-
  // shift bookkeeping (and the same ball/variable-apex delegation) so the seed lands the
  // cone rows EXACTLY where the loop's z-update would (the cold and warm paths must not
  // diverge on the cone rows — including the variable-apex apex, S8.5c).
  static void project_cone_block(const SocBlock &blk, const atx::core::linalg::VecX &ax,
                                 atx::core::linalg::VecX &z) {
    const atx::usize d = blk.dim;
    std::vector<atx::f64> arg(d, 0.0);
    std::vector<atx::f64> proj(d, 0.0);
    for (atx::usize j = 0; j < d; ++j) {
      const Eigen::Index row = static_cast<Eigen::Index>(blk.row_start + j);
      arg[j] = ax[row] + blk.offset[static_cast<Eigen::Index>(j)];
    }
    project_block_arg(blk, arg, proj);
    for (atx::usize j = 0; j < d; ++j) {
      const Eigen::Index row = static_cast<Eigen::Index>(blk.row_start + j);
      z[row] = proj[j] - blk.offset[static_cast<Eigen::Index>(j)];
    }
  }

  // -------------------------------------------------------------------------
  //  Order-fixed scalar helpers (determinism, R1).
  // -------------------------------------------------------------------------
  [[nodiscard]] static atx::f64 clamp(atx::f64 v, atx::f64 lo, atx::f64 hi) noexcept {
    if (v < lo) {
      return lo;
    }
    if (v > hi) {
      return hi;
    }
    return v;
  }
};

} // namespace atx::engine::risk
