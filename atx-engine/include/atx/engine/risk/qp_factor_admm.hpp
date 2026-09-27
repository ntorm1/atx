#pragma once

// atx::engine::risk — qp_factor_admm: the FACTOR-SPACE ADMM for the production constrained
// QP (Lane 6). Same problem as ConstrainedQpSolver,
//
//     minimize_w   ½ wᵀ(2λV + 2C_imp)w + q_effᵀw + κ Σ|w − w_prev|
//     subject to   l ≤ A w ≤ u,   Σ|w| ≤ G,   Σ|w − w_prev| ≤ T
//
// with V = X F Xᵀ + D, solved WITHOUT the factor-augmented KKT.
//
// ===========================================================================
//  Why (the per-iteration floor of the augmented solver)
// ===========================================================================
//  The augmented solver (qp_solver.hpp) carries y = Xᵀw as K extra variables with K dense
//  equality rows, splits the gross/turnover L1 with 2M aux columns, and factors the
//  (n + R̃)-dim KKT once. At M = 3000, K = 64 that LDLᵀ holds ~958k nonzeros, one solve
//  costs ~4.4 ms, and a warm production solve needs ~500 of them (≈ 3.5 s). The fill is a
//  property of the dense factor rows, not of the ordering (an ordering that defers the
//  dense rows only reached 661k).
//
//  Here the splitting keeps the risk EXACT in the x-update and moves only the constraints
//  into z:
//    * box rows (every row of A with exactly one nonzero: position caps, %ADV / %shares
//      folds, discretize pins) and the gross L1 budget form ONE block z_b = w projected
//      onto {lo ≤ z ≤ hi, Σ|z| ≤ G}: a soft-threshold-then-clamp at a scalar τ found by a
//      safeguarded Newton search (deterministic, O(M) per step);
//    * the remaining (dense) rows of A — dollar-neutral, factor exposure, beta, group,
//      sector-net — keep their box clamp z_d = clamp(Â_d w + y/ρ, l̂, û) (Â_d row-scaled to
//      unit ∞-norm);
//    * the turnover L1 (budget and/or κ penalty) is a second identity block z_t whose
//      update is the prox of κ‖·‖₁ + the L1-ball indicator around w_prev.
//  The x-update matrix is then  H = Δ + W Wᵀ  with Δ diagonal (2λD + 2c + σ + ρ_b + ρ_t)
//  and W = [√(2λ) X L_F | Â_dᵀ diag(√ρ_d)]  (M × (K + R_d), L_F = chol(F)), solved by
//  Woodbury through a (K + R_d)² capacitance: O(M(K + R_d)) per iteration, no M×M matrix
//  and no sparse factorization. A ρ change rebuilds the capacitance (O(M(K + R_d)²)).
//
//  The ρ schedule, over-relaxation, equality-row ρ boost, deterministic early exit and warm
//  start are the AdmmSchedule semantics of the augmented solver, applied to this splitting.
//
// ===========================================================================
//  Polish (OSQP §4, adapted) and the feasibility gate
// ===========================================================================
//  After the loop the active set is read off the final z (the clamps and soft-thresholds
//  land EXACTLY on their bounds): names fixed at a box bound, at 0 (gross soft zone) or at
//  w_prev (turnover soft zone); the signs of the free names; the dense rows at a bound; the
//  gross / turnover budgets when their τ is active. ONE equality-constrained QP over the
//  free names is solved exactly (Woodbury again, plus an n_e × n_e Schur complement). The
//  polished book is accepted only if it meets EVERY check at feas_tol (the L1 sums included)
//  and its objective exceeds the ADMM book's by no more than that book's infeasibility can
//  explain (first-order sensitivity: 2‖y‖∞·Σ violations, plus 1e-9 + 1e-7·|f|).
//  The gate checks, in original units: each box bound and dense row to cfg.feas_tol, and
//  the gross / turnover sums to (M + 1)·feas_tol — the tolerance the augmented gate
//  implies (it checks each of the M split rows and the budget row at feas_tol).
//
// ===========================================================================
//  Scope and determinism
// ===========================================================================
//  Cones (tracking error, sector SOC, robust alpha) are NOT handled: factor_admm_eligible()
//  is false for them and ConstrainedQpSolver keeps its augmented path. Every loop is order-
//  fixed; the τ searches are capped; the early exit is tested only every check_every
//  iterations. Same inputs ⇒ the same iteration count ⇒ a byte-identical book.
//
//  Warm start layout (distinct from the augmented one): x_full = w (length M); y_full =
//  [y_box (M) ; y_dense (R_d) ; y_turn (M, iff turnover)] in original units.

#include <span>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/constraints.hpp"

namespace atx::engine::risk {
class FactorModel;
struct AdmmSchedule;
struct TradeCostTerms;

// The solver knobs the factor-space path reads (a subset of QpConfig, passed by value so
// this header does not depend on qp_solver.hpp).
struct FactorAdmmConfig {
  atx::usize iters = 300;   // iteration cap (the schedule's early exit may end sooner)
  atx::f64 rho = 1.0;       // base ρ when no warm ρ is supplied
  atx::f64 sigma = 1e-6;    // proximal regularization
  atx::f64 feas_tol = 1e-6; // gate tolerance (per row; sums get (M + 1)·feas_tol)
  bool polish = true;
  ConstraintFeasibilityRule feasibility_rule{ConstraintFeasibilityRule::LegacyAbsoluteV1};
  atx::f64 feasibility_relative_tolerance{0.0};
};

struct FactorAdmmOutput {
  std::vector<atx::f64> w;    // length M
  std::vector<atx::f64> dual; // [y_box (M) ; y_dense (R_d) ; y_turn (M iff turnover)], original units
  atx::usize iters = 0;
  atx::f64 rho = 0.0;
  atx::f64 prim_res = 0.0;    // worst constraint violation of w (original units)
  atx::f64 dual_res = 0.0;    // ‖Pw + q_eff + Ãᵀy‖∞ at the ADMM iterate (scaled rows)
  bool polished = false;
};

// Eligibility is only the cone check; V2 separately refuses elastic constraints.
[[nodiscard]] bool factor_admm_eligible(const MaterializedConstraints& C) noexcept;

// Preserved declarations for the existing independent projection oracle tests.
// The numerical helper bodies are compiled only in qp_factor_admm.cpp.
namespace detail {
[[nodiscard]] atx::f64 fa_soft_clamp(atx::f64 v, atx::f64 tau,
                                     atx::f64 lo, atx::f64 hi) noexcept;
[[nodiscard]] atx::f64 fa_find_tau(const atx::core::linalg::VecX& v,
    const std::vector<atx::f64>& lo, const std::vector<atx::f64>& hi,
    const std::vector<atx::u8>& on, atx::f64 budget, atx::f64 tau_min) noexcept;
} // namespace detail

// Null costs preserve the original numerical path. Non-null costs select V2's
// checked per-name proximal splitting, hard execution limits and convergence gate.
[[nodiscard]] atx::core::Result<FactorAdmmOutput>
solve_factor_admm(const FactorModel &V, atx::f64 lambda, std::span<const atx::f64> q,
                  const MaterializedConstraints &C, const FactorAdmmConfig &cfg,
                  const AdmmSchedule &sched, std::span<const atx::f64> x0 = {},
                  std::span<const atx::f64> y0 = {}, atx::f64 rho_warm = 0.0,
                  const TradeCostTerms *costs = nullptr);

} // namespace atx::engine::risk
