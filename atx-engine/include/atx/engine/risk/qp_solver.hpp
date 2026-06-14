#pragma once

// atx::engine::risk — ConstrainedQpSolver: a DETERMINISTIC fixed-iteration,
// OSQP-class operator-splitting (ADMM) solver for the constrained portfolio QP
// (S1-2). Solves
//
//     minimize_w   ½ wᵀP w + qᵀw
//     subject to   l ≤ A w ≤ u                         (the S1-1 linear rows)
//                  Σ|w|          ≤ gross_l1_budget      (gross L1, aux-split)
//                  Σ|w − w_prev| ≤ turnover_budget      (turnover L1, aux-split)
//
// with P = 2λV applied MATRIX-FREE through FactorModel::apply (V = X F Xᵀ + D is
// kept FACTORED and is NEVER materialized as a dense M×M matrix — R4). The L1
// budgets are NOT linear in w; they are folded via the standard auxiliary-variable
// split (s_i ≥ |w_i|, r_i ≥ |w_i − ref_i|) so the whole problem is a linear-
// inequality QP over the augmented variable x = [w; s; r].
//
// ===========================================================================
//  Determinism (R1) — the crux
// ===========================================================================
//  The OUTER ADMM runs EXACTLY cfg.iters passes; the INNER x-update runs EXACTLY
//  cfg.kkt_iters PCG steps. There is NO residual / convergence early-exit anywhere
//  (a convergence test makes the iteration count input-dependent, breaking byte-
//  reproducibility). All reductions (PCG dot products, clamps, the feasibility
//  scan) run in canonical ascending order. Duals y and the splitting variable z are
//  zero-initialized (a fixed deterministic seed). No RNG. Same inputs ⇒ byte-
//  identical std::vector<f64>.
//
// ===========================================================================
//  Matrix-free architecture (R4)
// ===========================================================================
//  The KKT operator of the x-update is  K = P̃ + σI + ρ ÃᵀÃ  where P̃ = blkdiag(2λV
//  on the w-block, 0 on s/r). Its matvec K·v is applied WITHOUT forming K:
//    * the 2λV piece on the w-block via FactorModel::apply (factored; O(MK+K²)),
//    * the σ·v piece elementwise,
//    * the ρ·Ãᵀ(Ã v) piece via two dense Ã / Ãᵀ matvecs.
//  Ã (the augmented constraint matrix) IS materialized as a dense MatX — that is the
//  documented constraint cost; the invariant the gate enforces is that V is never
//  densified, which holds (only Ã is dense, and Ã carries the split rows, not V).
//  The PCG preconditioner is the inverse of the diagonal 2λ·D ⊕ 0 ⊕ 0 (w-block from
//  FactorModel::specific_var) + σ + ρ·diag(ÃᵀÃ) — cheap and deterministic.
//
// ===========================================================================
//  Feasibility gate (R3)
// ===========================================================================
//  After the FIXED loop, Ã x is checked against [l̃, ũ] row-by-row. A violation
//  beyond cfg.feas_tol ⇒ Err(InvalidArgument) naming the offending block — a
//  genuinely-infeasible set (e.g. contradictory bounds) is reported, NEVER returned
//  as a silently-clamped book. The returned book is the first M entries of x.

#include <span>    // std::span (q)
#include <string>  // std::string, std::to_string (feasibility-gate Err message)
#include <utility> // std::move
#include <vector>  // std::vector (result + augmented scratch)

#include <Eigen/Dense>

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/constraints.hpp"  // MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp" // FactorModel (apply / specific_var)

namespace atx::engine::risk {

// ADMM / PCG configuration. Every count is FIXED — there is no convergence test
// anywhere (R1); the iteration budgets ARE the algorithm.
struct QpConfig {
  // FIXED outer ADMM iterations — NO early-exit (R1). Default 600: a gross/turnover
  // L1 aux-split roughly TRIPLES the primal dimension (x = [w; s; r]) and the split
  // rows converge slower than the pure-linear rows, so a feasible augmented problem
  // needs more outer steps to clear the feas_tol gate. 600 clears the common
  // augmented path out-of-the-box (200 does not — it Errs on feasible L1 problems);
  // S1-4 can still override per problem size.
  atx::usize iters = 600;
  atx::usize kkt_iters = 50; // FIXED inner PCG iterations for the x-update (no early-exit)
  atx::f64 rho = 1.0;        // ADMM constraint penalty
  atx::f64 sigma = 1e-6;     // proximal regularization (KKT well-posedness)
  atx::f64 feas_tol = 1e-6;  // post-loop feasibility tolerance (R3)
};

// The QP instance. P = 2·risk_aversion·V, applied matrix-free via V.apply (R4).
struct QpProblem {
  const FactorModel &V;             // P = 2λV via V.apply — V is NEVER densified
  atx::f64 risk_aversion;           // λ (so P = 2λV)
  std::span<const atx::f64> q;      // linear term (length M) = −α_aim
  const MaterializedConstraints &C; // l ≤ A w ≤ u + the L1 budgets
};

class ConstrainedQpSolver {
public:
  QpConfig cfg;

  // Solve the augmented constrained QP. Returns the length-M weight vector w (the
  // w-block of x). Err(InvalidArgument) on a dimension mismatch up front or an
  // infeasible set after the fixed loop. NEVER a silently-clamped book.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>> solve(const QpProblem &p) const {
    namespace co = atx::core;
    const atx::usize m = p.V.n_instruments();
    ATX_TRY_VOID(validate(p, m));

    // Augmented dimensions: x = [w (M); s (M, gross aux); r (M, turnover aux)].
    const bool has_gross = p.C.gross_l1_budget >= 0.0;
    const bool has_turn = p.C.has_turnover;
    const atx::usize n = m + m + (has_turn ? m : 0U);

    // Build the augmented constraint stack Ã x ∈ [l̃, ũ] (order-fixed) and q̃.
    AugSystem sys = build_aug_system(p, m, n, has_gross, has_turn);

    // Run the deterministic fixed-iteration ADMM over (Ã, l̃, ũ, P̃, q̃).
    const co::linalg::VecX x = run_admm(p, sys, m, n);

    // Feasibility gate (R3): Ã x ∈ [l̃, ũ] within feas_tol, else Err.
    ATX_TRY_VOID(check_feasible(sys, x));

    // Return the w-block (first M entries) in canonical order.
    std::vector<atx::f64> w(m);
    for (atx::usize i = 0; i < m; ++i) {
      w[i] = x[static_cast<Eigen::Index>(i)];
    }
    return co::Ok(std::move(w));
  }

private:
  // The augmented linear system pieces. Ã is dense (the constraint cost; V stays
  // factored). q̃ is the augmented linear term [q; 0; 0]. pinv is the per-element
  // Jacobi preconditioner 1 / diag(K), K = P̃ + σI + ρ ÃᵀÃ.
  struct AugSystem {
    atx::core::linalg::MatX a;    // R̃ × n augmented constraint matrix
    atx::core::linalg::VecX l;    // R̃ lower bounds
    atx::core::linalg::VecX u;    // R̃ upper bounds
    atx::core::linalg::VecX q;    // n augmented linear term
    atx::core::linalg::VecX pinv; // n Jacobi preconditioner (1 / diag(K))
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
    if (p.C.has_turnover && p.C.turnover_ref.size() != m) {
      return co::Err(co::ErrorCode::InvalidArgument, "QP: turnover_ref length must equal M");
    }
    if (p.risk_aversion < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "QP: risk_aversion must be >= 0");
    }
    return co::Ok();
  }

  // -------------------------------------------------------------------------
  //  Augmented-row counting — must mirror append_* emission EXACTLY (R1).
  // -------------------------------------------------------------------------
  [[nodiscard]] static atx::usize aug_row_count(const QpProblem &p, atx::usize m, bool has_gross,
                                                bool has_turn) {
    atx::usize r = static_cast<atx::usize>(p.C.A.rows()); // (1) the S1-1 linear rows
    if (has_gross) {
      r += m + m; // w_i − s_i ≤ 0 and −w_i − s_i ≤ 0
      r += m;     // s_i ≥ 0
      r += 1U;    // Σ s_i ≤ gross
    }
    if (has_turn) {
      r += m + m; // (w_i − r_i) ≤ ref_i and (−w_i − r_i) ≤ −ref_i
      r += m;     // r_i ≥ 0
      r += 1U;    // Σ r_i ≤ turnover
    }
    return r;
  }

  // Build Ã, l̃, ũ, q̃, and the Jacobi preconditioner. Order-fixed row emission:
  //   (1) [A | 0 | 0]  with [l, u];
  //   (2) gross split (if has_gross): ±w − s ≤ 0; s ≥ 0; Σs ≤ gross;
  //   (3) turnover split (if has_turn): ±w − r ≤ ±ref; r ≥ 0; Σr ≤ turnover.
  [[nodiscard]] AugSystem build_aug_system(const QpProblem &p, atx::usize m, atx::usize n,
                                           bool has_gross, bool has_turn) const {
    namespace cl = atx::core::linalg;
    const atx::usize rows = aug_row_count(p, m, has_gross, has_turn);
    const auto er = static_cast<Eigen::Index>(rows);
    const auto en = static_cast<Eigen::Index>(n);

    AugSystem sys;
    sys.a = cl::MatX::Zero(er, en);
    sys.l = cl::VecX::Zero(er);
    sys.u = cl::VecX::Zero(er);
    sys.q = cl::VecX::Zero(en);

    // q̃ = [q; 0; 0] (the aux blocks have no linear cost).
    for (atx::usize i = 0; i < m; ++i) {
      sys.q[static_cast<Eigen::Index>(i)] = p.q[i];
    }

    Eigen::Index next = 0;
    append_linear_rows(sys, p.C, m, next);
    const atx::usize s_off = m;                       // s-block column offset
    const atx::usize r_off = m + (has_turn ? m : 0U); // r-block column offset (after s)
    if (has_gross) {
      append_gross_split(sys, m, s_off, p.C.gross_l1_budget, next);
    }
    if (has_turn) {
      append_turnover_split(sys, m, r_off, p.C, next);
    }

    sys.pinv = build_precond(p, sys, m, n);
    return sys;
  }

  // (1) The S1-1 linear rows occupy the w-block columns [0, M); aux columns are 0.
  static void append_linear_rows(AugSystem &sys, const MaterializedConstraints &c, atx::usize m,
                                 Eigen::Index &next) {
    for (Eigen::Index ar = 0; ar < c.A.rows(); ++ar) {
      for (atx::usize j = 0; j < m; ++j) {
        sys.a(next, static_cast<Eigen::Index>(j)) = c.A(ar, static_cast<Eigen::Index>(j));
      }
      sys.l[next] = c.l[ar];
      sys.u[next] = c.u[ar];
      ++next;
    }
  }

  // (2) Gross split: rows enforce s_i ≥ |w_i| and Σs_i ≤ L.
  //   w_i − s_i ≤ 0   (bounds [−inf, 0]); −w_i − s_i ≤ 0   (bounds [−inf, 0]);
  //   s_i ≥ 0         (bounds [0, +inf]); Σ s_i ≤ L        (bounds [−inf, L]).
  static void append_gross_split(AugSystem &sys, atx::usize m, atx::usize s_off, atx::f64 budget,
                                 Eigen::Index &next) {
    const atx::f64 inf = kInf;
    for (atx::usize i = 0; i < m; ++i) { // w_i − s_i ≤ 0
      sys.a(next, static_cast<Eigen::Index>(i)) = 1.0;
      sys.a(next, static_cast<Eigen::Index>(s_off + i)) = -1.0;
      sys.l[next] = -inf;
      sys.u[next] = 0.0;
      ++next;
    }
    for (atx::usize i = 0; i < m; ++i) { // −w_i − s_i ≤ 0
      sys.a(next, static_cast<Eigen::Index>(i)) = -1.0;
      sys.a(next, static_cast<Eigen::Index>(s_off + i)) = -1.0;
      sys.l[next] = -inf;
      sys.u[next] = 0.0;
      ++next;
    }
    for (atx::usize i = 0; i < m; ++i) { // s_i ≥ 0
      sys.a(next, static_cast<Eigen::Index>(s_off + i)) = 1.0;
      sys.l[next] = 0.0;
      sys.u[next] = inf;
      ++next;
    }
    for (atx::usize i = 0; i < m; ++i) { // Σ s_i ≤ budget
      sys.a(next, static_cast<Eigen::Index>(s_off + i)) = 1.0;
    }
    sys.l[next] = -inf;
    sys.u[next] = budget;
    ++next;
  }

  // (3) Turnover split: rows enforce r_i ≥ |w_i − ref_i| and Σr_i ≤ T.
  //   (w_i − r_i) ≤ ref_i; (−w_i − r_i) ≤ −ref_i; r_i ≥ 0; Σ r_i ≤ T.
  static void append_turnover_split(AugSystem &sys, atx::usize m, atx::usize r_off,
                                    const MaterializedConstraints &c, Eigen::Index &next) {
    const atx::f64 inf = kInf;
    for (atx::usize i = 0; i < m; ++i) { // w_i − r_i ≤ ref_i
      sys.a(next, static_cast<Eigen::Index>(i)) = 1.0;
      sys.a(next, static_cast<Eigen::Index>(r_off + i)) = -1.0;
      sys.l[next] = -inf;
      sys.u[next] = c.turnover_ref[i];
      ++next;
    }
    for (atx::usize i = 0; i < m; ++i) { // −w_i − r_i ≤ −ref_i
      sys.a(next, static_cast<Eigen::Index>(i)) = -1.0;
      sys.a(next, static_cast<Eigen::Index>(r_off + i)) = -1.0;
      sys.l[next] = -inf;
      sys.u[next] = -c.turnover_ref[i];
      ++next;
    }
    for (atx::usize i = 0; i < m; ++i) { // r_i ≥ 0
      sys.a(next, static_cast<Eigen::Index>(r_off + i)) = 1.0;
      sys.l[next] = 0.0;
      sys.u[next] = inf;
      ++next;
    }
    for (atx::usize i = 0; i < m; ++i) { // Σ r_i ≤ T
      sys.a(next, static_cast<Eigen::Index>(r_off + i)) = 1.0;
    }
    sys.l[next] = -inf;
    sys.u[next] = c.turnover_budget;
    ++next;
  }

  // Jacobi preconditioner pinv = 1 / diag(K), K = P̃ + σI + ρ ÃᵀÃ. diag(P̃) on the
  // w-block is 2λ·D (the dominant, trivially-available part of diag(2λV) from
  // FactorModel::specific_var; the factor cross-terms are dropped — a valid SPD
  // preconditioner uses only the diagonal it cheaply has). Aux blocks get 0 from P̃.
  // Every entry adds σ + ρ·‖Ã_col‖². Column reductions order-fixed (ascending row).
  [[nodiscard]] atx::core::linalg::VecX build_precond(const QpProblem &p, const AugSystem &sys,
                                                      atx::usize m, atx::usize n) const {
    namespace cl = atx::core::linalg;
    const atx::f64 two_lambda = 2.0 * p.risk_aversion;
    const cl::VecX &dvar = p.V.specific_var(); // M floored specific variances
    cl::VecX pinv(static_cast<Eigen::Index>(n));
    for (atx::usize j = 0; j < n; ++j) {
      const auto ej = static_cast<Eigen::Index>(j);
      const atx::f64 pjj = (j < m) ? two_lambda * dvar[ej] : 0.0; // diag(P̃)
      atx::f64 ata = 0.0;                                         // ‖Ã_col_j‖²
      for (Eigen::Index ar = 0; ar < sys.a.rows(); ++ar) {
        const atx::f64 aij = sys.a(ar, ej);
        ata += aij * aij;
      }
      const atx::f64 diag = pjj + cfg.sigma + cfg.rho * ata;
      // Non-positive diagonal ⇒ drop this preconditioner term (Jacobi reduces to the
      // identity on that coordinate). With σ > 0 this is unreachable, but the guard
      // keeps pinv finite (no 1/0) if σ is ever configured to 0 — deterministic.
      pinv[ej] = (diag > 0.0) ? 1.0 / diag : 0.0;
    }
    return pinv;
  }

  // -------------------------------------------------------------------------
  //  The fixed-iteration ADMM (R1). z, y zero-init (deterministic seed).
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::linalg::VecX run_admm(const QpProblem &p, const AugSystem &sys,
                                                 atx::usize m, atx::usize n) const {
    namespace cl = atx::core::linalg;
    const auto en = static_cast<Eigen::Index>(n);
    const auto er = sys.a.rows();

    cl::VecX x = cl::VecX::Zero(en);
    cl::VecX z = cl::VecX::Zero(er);
    cl::VecX y = cl::VecX::Zero(er);
    for (atx::usize k = 0; k < cfg.iters; ++k) { // FIXED — no early-exit
      // (1) x-update: solve K x = σx − q̃ + Ãᵀ(ρ z − y) by fixed-K PCG.
      const cl::VecX rhs = cfg.sigma * x - sys.q + sys.a.transpose() * (cfg.rho * z - y);
      x = pcg_solve(p, sys, m, x, rhs, sys.pinv);
      // (2) z-update: clamp(Ã x + y/ρ, l̃, ũ) elementwise (order-fixed).
      const cl::VecX ax = sys.a * x;
      for (Eigen::Index i = 0; i < er; ++i) {
        const atx::f64 t = ax[i] + y[i] / cfg.rho;
        z[i] = clamp(t, sys.l[i], sys.u[i]);
      }
      // (3) y-update: y += ρ (Ã x − z).
      y += cfg.rho * (ax - z);
    }
    return x;
  }

  // -------------------------------------------------------------------------
  //  Fixed-K PCG for the x-update — NO early-exit (R1). Solves K x = rhs with the
  //  matrix-free operator K·v (apply_kkt) and the Jacobi preconditioner pinv.
  //  Warm-started at x0 (the prior ADMM x) so the fixed-K inner solve converges
  //  faster across outer iterations without ever testing a residual.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::linalg::VecX pcg_solve(const QpProblem &p, const AugSystem &sys,
                                                  atx::usize m, const atx::core::linalg::VecX &x0,
                                                  const atx::core::linalg::VecX &rhs,
                                                  const atx::core::linalg::VecX &pinv) const {
    namespace cl = atx::core::linalg;
    cl::VecX x = x0;
    cl::VecX r = rhs - apply_kkt(p, sys, m, x); // residual r0 = rhs − K x0
    cl::VecX zv = pinv.cwiseProduct(r);         // preconditioned residual
    cl::VecX pdir = zv;
    atx::f64 rz_old = dot(r, zv);
    for (atx::usize it = 0; it < cfg.kkt_iters; ++it) { // FIXED — no early-exit
      const cl::VecX kp = apply_kkt(p, sys, m, pdir);
      const atx::f64 denom = dot(pdir, kp);
      // CG breakdown (zero curvature pᵀKp): take a zero step rather than divide by
      // zero — deterministic, NaN-free, and the loop still runs to fixed kkt_iters.
      const atx::f64 alpha = (denom != 0.0) ? rz_old / denom : 0.0;
      x += alpha * pdir;
      r -= alpha * kp;
      zv = pinv.cwiseProduct(r);
      const atx::f64 rz_new = dot(r, zv);
      // Stalled residual (rᵀz_old == 0, exact convergence): zero β freezes the search
      // direction — deterministic, avoids 0/0; subsequent fixed steps are no-ops.
      const atx::f64 beta = (rz_old != 0.0) ? rz_new / rz_old : 0.0;
      pdir = zv + beta * pdir;
      rz_old = rz_new;
    }
    return x;
  }

  // The matrix-free KKT operator K·v = P̃·v + σ·v + ρ·Ãᵀ(Ã v):
  //   * P̃·v: 2λV on the w-block via FactorModel::apply (factored — R4); 0 on aux;
  //   * σ·v elementwise; ρ·Ãᵀ(Ã v) via two dense Ã / Ãᵀ matvecs.
  [[nodiscard]] atx::core::linalg::VecX apply_kkt(const QpProblem &p, const AugSystem &sys,
                                                  atx::usize m,
                                                  const atx::core::linalg::VecX &v) const {
    namespace cl = atx::core::linalg;
    const Eigen::Index n = v.size();
    cl::VecX out(n);

    // P̃·v on the w-block: 2λ·V·(v_w) through the FACTORED apply (never densify V).
    std::vector<atx::f64> wv(m), pw(m);
    for (atx::usize i = 0; i < m; ++i) {
      wv[i] = v[static_cast<Eigen::Index>(i)];
    }
    p.V.apply(std::span<const atx::f64>(wv), std::span<atx::f64>(pw)); // V·v_w
    const atx::f64 two_lambda = 2.0 * p.risk_aversion;
    for (atx::usize i = 0; i < m; ++i) {
      out[static_cast<Eigen::Index>(i)] = two_lambda * pw[i];
    }
    for (Eigen::Index i = static_cast<Eigen::Index>(m); i < n; ++i) {
      out[i] = 0.0; // P̃ is zero on the s/r blocks
    }
    // σ·v elementwise.
    out += cfg.sigma * v;
    // ρ·Ãᵀ(Ã v).
    const cl::VecX av = sys.a * v;
    out += cfg.rho * (sys.a.transpose() * av);
    return out;
  }

  // -------------------------------------------------------------------------
  //  Feasibility gate (R3): Ã x ∈ [l̃, ũ] within feas_tol, else Err naming block.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Status check_feasible(const AugSystem &sys,
                                                 const atx::core::linalg::VecX &x) const {
    namespace co = atx::core;
    const co::linalg::VecX ax = sys.a * x;
    for (Eigen::Index i = 0; i < ax.size(); ++i) {
      const atx::f64 over = ax[i] - sys.u[i];  // > feas_tol ⇒ above upper bound
      const atx::f64 under = sys.l[i] - ax[i]; // > feas_tol ⇒ below lower bound
      if (over > cfg.feas_tol) {
        return co::Err(co::ErrorCode::InvalidArgument, infeasible_msg(i, over));
      }
      if (under > cfg.feas_tol) {
        return co::Err(co::ErrorCode::InvalidArgument, infeasible_msg(i, under));
      }
    }
    return co::Ok();
  }

  // Compose the feasibility-gate Err message. It must NOT assert "infeasible" as
  // fact: a fixed-iteration ADMM can also return an under-converged book on a
  // FEASIBLE set if cfg.iters is too low for the aux-split, so the message names
  // BOTH causes and points at the iteration budget. Carries the offending row and
  // the violation magnitude for diagnosis. (InvalidArgument is the closest code —
  // the error domain has no Infeasible/Unconverged enumerator.)
  [[nodiscard]] std::string infeasible_msg(Eigen::Index row, atx::f64 violation) const {
    return "ConstrainedQpSolver::solve: book violates constraint row " + std::to_string(row) +
           " by " + std::to_string(violation) + " after " + std::to_string(cfg.iters) +
           " fixed iterations — the set may be infeasible OR cfg.iters is too low "
           "for the aux-split to converge (raise iters)";
  }

  // -------------------------------------------------------------------------
  //  Order-fixed reductions / scalar helpers (determinism, R1).
  // -------------------------------------------------------------------------
  // Ascending-index dot product (NOT Eigen's .dot(), to pin the summation order).
  [[nodiscard]] static atx::f64 dot(const atx::core::linalg::VecX &a,
                                    const atx::core::linalg::VecX &b) noexcept {
    atx::f64 s = 0.0;
    for (Eigen::Index i = 0; i < a.size(); ++i) {
      s += a[i] * b[i];
    }
    return s;
  }

  [[nodiscard]] static atx::f64 clamp(atx::f64 v, atx::f64 lo, atx::f64 hi) noexcept {
    if (v < lo) {
      return lo;
    }
    if (v > hi) {
      return hi;
    }
    return v;
  }

  // A large finite bound standing in for ±∞ on a one-sided split row. Far above any
  // realistic |w|/|s|/|r| so it never binds, yet finite so clamp() / the feasibility
  // scan stay well-defined (no inf arithmetic).
  static constexpr atx::f64 kInf = 1e30;
};

} // namespace atx::engine::risk
