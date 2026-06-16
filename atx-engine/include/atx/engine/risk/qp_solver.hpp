#pragma once

// atx::engine::risk — ConstrainedQpSolver: a DETERMINISTIC fixed-iteration,
// OSQP-class operator-splitting (ADMM) solver for the constrained portfolio QP
// (S1-2 / S8.1). Solves
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
//  public API (`solve(const QpProblem&)`, `QpProblem`, `QpConfig`) is UNCHANGED;
//  only the internals change. `q = −α_aim`, `P = 2λV` — the same problem the
//  oracle (`qp_solver_reference.hpp`) solves the dense way, agreeing at the optimum.
//
//  The ADMM x-update is now a DIRECT SPARSE KKT SOLVE of the OSQP quasi-definite
//  system (Vanderbei 1995; OSQP §3, Stellato 2020)
//
//      [ P + σI    Ãᵀ   ] [ x ]   [ σ x_k − q̃              ]
//      [ Ã       −ρ⁻¹I  ] [ ν ] = [ z_k − ρ⁻¹ y_k          ]
//
//  whose x-block is ALGEBRAICALLY IDENTICAL to the as-built reduced normal-equation
//  K x = σx_k − q̃ + Ãᵀ(ρ z_k − y_k) with K = P + σI + ρ ÃᵀÃ (eliminate ν: ν = ρ(Ãx
//  − z_k) + y_k). So the box projection (z-clamp), the over-relaxation (none — α=1,
//  as in the as-built), and the dual update (y += ρ(Ãx − z)) are UNCHANGED; only the
//  linear solve swaps from dense PCG to the sparse KKT factorization. V is NEVER
//  densified (R4); the KKT is sparse quasi-definite (R5) and is factored by the
//  deterministic no-pivot QuasiDefiniteLdl (kkt_ldl.hpp, S8.2) — a static AMD-ordered
//  LDLᵀ that is byte-identical across threads/builds (no Bunch-Kaufman pivot branch).
//
// ===========================================================================
//  Determinism (R1)
// ===========================================================================
//  The OUTER ADMM runs EXACTLY cfg.iters passes; there is NO residual / convergence
//  early-exit. Duals y and the splitting variable z are zero-initialized. The KKT is
//  assembled in a fixed CSC traversal order (qp_augment.hpp) and factored ONCE per
//  solve (the matrix is constant for fixed ρ, σ) then reused every iteration. The
//  factorization is the no-pivot QuasiDefiniteLdl (S8.2): symbolic (AMD order +
//  elimination tree) then numeric, both order-fixed and purely serial. No RNG. All
//  hand-rolled reductions run in canonical ascending order. Same inputs ⇒ a
//  byte-identical book on ANY thread count (G-DET).
//
// ===========================================================================
//  Feasibility gate (R3)
// ===========================================================================
//  After the FIXED loop, Ã x is checked against [l̃, ũ] row-by-row. A violation
//  beyond cfg.feas_tol ⇒ Err(InvalidArgument) naming the offending row — a genuinely
//  infeasible set is reported, NEVER returned as a silently-clamped book. The
//  returned book is the first M entries (the w-block) of x.

#include <span>    // std::span (q)
#include <string>  // std::string, std::to_string (feasibility-gate Err message)
#include <utility> // std::move
#include <vector>  // std::vector (result)

#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/constraints.hpp"  // MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp" // FactorModel (apply / specific_var / factor_cov)
#include "atx/engine/risk/kkt_ldl.hpp"      // QuasiDefiniteLdl (deterministic no-pivot KKT solve)
#include "atx/engine/risk/qp_augment.hpp"   // build_augmented / AugmentedQp

namespace atx::engine::risk {

// ADMM configuration. Every count is FIXED — there is no convergence test anywhere
// (R1); the iteration budget IS the algorithm.
struct QpConfig {
  // FIXED outer ADMM iterations — NO early-exit (R1). Default 600: a gross/turnover
  // L1 aux-split roughly TRIPLES the primal dimension (x = [w; y; s; r]) and the
  // split rows converge slower than the pure-linear rows, so a feasible augmented
  // problem needs more outer steps to clear the feas_tol gate. 600 clears the common
  // augmented path out-of-the-box; S8.3 re-tunes the budget for the sparse rewrite.
  atx::usize iters = 600;
  atx::usize kkt_iters = 50; // RETAINED for API compatibility (the old inner-PCG count;
                             // the direct KKT solve is exact so this is now unused).
  atx::f64 rho = 1.0;        // ADMM constraint penalty
  atx::f64 sigma = 1e-6;     // proximal regularization (KKT well-posedness / quasi-definiteness)
  atx::f64 feas_tol = 1e-6;  // post-loop feasibility tolerance (R3)
};

// The QP instance. P = 2·risk_aversion·V (V is NEVER densified — the augmentation
// re-expresses it in K factor space via FactorModel::factor_cov / specific_var).
struct QpProblem {
  const FactorModel &V;             // P = 2λV via the y=Xᵀw augmentation — V never densified
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

    // (1) Build the factor-augmented sparse form  x = [w; y; s; r]  (R4 — no dense Ã).
    const AugmentedQp aug = build_augmented(p.V, p.risk_aversion, p.q, p.C);

    // (2) Run the deterministic fixed-iteration ADMM over the sparse augmented form.
    co::linalg::VecX x;
    ATX_TRY_VOID(run_admm(aug, x));

    // (3) Feasibility gate (R3): Ã x ∈ [l̃, ũ] within feas_tol, else Err.
    ATX_TRY_VOID(check_feasible(aug, x));

    // (4) Return the w-block (first M entries) in canonical order.
    std::vector<atx::f64> w(m);
    for (atx::usize i = 0; i < m; ++i) {
      w[i] = x[static_cast<Eigen::Index>(i)];
    }
    return co::Ok(std::move(w));
  }

private:
  using SpMat = Eigen::SparseMatrix<atx::f64>;

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
  //  Assemble the OSQP quasi-definite KKT matrix (R5):
  //      [ P + σI    Ãᵀ   ]
  //      [ Ã       −ρ⁻¹I  ]
  //  built in a FIXED triplet order (P + σI block first by column, then the Ã / Ãᵀ
  //  off-diagonals, then the −ρ⁻¹I block) so the CSC pattern is reproducible (R1).
  //  Quasi-definite for σ, ρ > 0 (Vanderbei Thm 2): the (1,1) block P+σI ≻ 0 even
  //  for the rank-deficient factor P, and the (2,2) block −ρ⁻¹I ≺ 0.
  // -------------------------------------------------------------------------
  [[nodiscard]] SpMat build_kkt(const AugmentedQp &aug) const {
    using Trip = Eigen::Triplet<atx::f64>;
    const auto n = static_cast<int>(aug.n_w + aug.n_y + aug.n_aux);
    const int r = static_cast<int>(aug.A_tilde.rows());
    const int dim = n + r;
    const atx::f64 neg_rho_inv = -1.0 / cfg.rho;

    std::vector<Trip> trips;
    trips.reserve(static_cast<atx::usize>(aug.P.nonZeros() + 2 * aug.A_tilde.nonZeros() + dim));

    // (a) (1,1) block: P + σI. Walk P in CSC (column-major) order; add σ on the
    //     diagonal exactly once per column (P has an explicit diagonal entry on the
    //     w/y blocks; the aux columns are all-zero in P so their σ is added here).
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

    // (b) off-diagonals: Ãᵀ in the top-right (rows [0,n), cols [n,n+r)) and Ã in the
    //     bottom-left (rows [n,n+r), cols [0,n)). Walk Ã once in CSC order; each entry
    //     (row i, col j) contributes Ã at (n+i, j) and Ãᵀ at (j, n+i).
    for (int c = 0; c < n; ++c) {
      for (SpMat::InnerIterator it(aug.A_tilde, c); it; ++it) {
        const int i = it.row(); // constraint row
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
  //  The fixed-iteration ADMM (R1) over the sparse augmented form. z, y zero-init
  //  (deterministic seed). The KKT matrix is constant for fixed ρ, σ, so it is
  //  factored ONCE and the factor is reused every outer iteration.
  //
  //  Each iteration solves the KKT system for (x, ν) with RHS [σx − q̃ ; z − ρ⁻¹y];
  //  the x-block returned is ALGEBRAICALLY IDENTICAL to the as-built reduced solve
  //  (P+σI+ρÃᵀÃ) x = σx − q̃ + Ãᵀ(ρz − y) (eliminate ν). The z-clamp and y-update
  //  below are the as-built lines VERBATIM (no over-relaxation, α = 1).
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Status run_admm(const AugmentedQp &aug,
                                           atx::core::linalg::VecX &x_out) const {
    namespace co = atx::core;
    namespace cl = atx::core::linalg;
    const auto n = static_cast<Eigen::Index>(aug.n_w + aug.n_y + aug.n_aux);
    const Eigen::Index r = aug.A_tilde.rows();
    const Eigen::Index dim = n + r;

    // Factor the KKT ONCE per solve (constant for fixed ρ, σ — R5/R6) with the
    // deterministic no-pivot QuasiDefiniteLdl. The symbolic (AMD + etree) and numeric
    // passes run once here; the factor is then reused every outer iteration. The
    // symbolic/numeric seam supports cross-solve caching of the symbolic structure
    // (the KKT pattern is fixed for a fixed constraint set), but that is DEFERRED:
    // factor-once-per-solve is the required behavior and keeps `solve()` const without
    // a mutable cache member churning the public API.
    const SpMat kkt = build_kkt(aug);
    QuasiDefiniteLdl ldl;
    ATX_TRY_VOID(ldl.factor_symbolic(kkt));
    ATX_TRY_VOID(ldl.factor_numeric(kkt));

    const atx::f64 rho_inv = 1.0 / cfg.rho;
    cl::VecX x = cl::VecX::Zero(n);
    cl::VecX z = cl::VecX::Zero(r);
    cl::VecX y = cl::VecX::Zero(r);
    std::vector<atx::f64> rhs(static_cast<atx::usize>(dim), 0.0);
    std::vector<atx::f64> sol(static_cast<atx::usize>(dim), 0.0);

    for (atx::usize it = 0; it < cfg.iters; ++it) { // FIXED — no early-exit
      // (1) x-update: solve the KKT system. RHS top = σx − q̃; bottom = z − ρ⁻¹y.
      for (Eigen::Index i = 0; i < n; ++i) {
        rhs[static_cast<atx::usize>(i)] = cfg.sigma * x[i] - aug.q_aug[i];
      }
      for (Eigen::Index i = 0; i < r; ++i) {
        rhs[static_cast<atx::usize>(n + i)] = z[i] - rho_inv * y[i];
      }
      ldl.solve(std::span<const atx::f64>(rhs), std::span<atx::f64>(sol));
      for (Eigen::Index i = 0; i < n; ++i) { // the x-block (ν-block implied by z/y below)
        x[i] = sol[static_cast<atx::usize>(i)];
      }

      // (2) z-update: clamp(Ã x + y/ρ, l̃, ũ) elementwise (order-fixed). VERBATIM.
      const cl::VecX ax = aug.A_tilde * x;
      for (Eigen::Index i = 0; i < r; ++i) {
        const atx::f64 t = ax[i] + y[i] / cfg.rho;
        z[i] = clamp(t, aug.l[i], aug.u[i]);
      }
      // (3) y-update: y += ρ (Ã x − z). VERBATIM.
      y += cfg.rho * (ax - z);
    }

    x_out = std::move(x);
    return co::Ok();
  }

  // -------------------------------------------------------------------------
  //  Feasibility gate (R3): Ã x ∈ [l̃, ũ] within feas_tol, else Err naming the row.
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
