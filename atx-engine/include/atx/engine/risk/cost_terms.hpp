#pragma once

// atx::engine::risk — cost_terms: per-name TRADE-COST augmentation of the factor-augmented
// sparse QP (Lane 6). Adds, on top of an already-built AugmentedQp (qp_augment.hpp):
//
//   (1) per-name linear cost      Σ_i κ_i |w_i − w0_i|              (spread + commission)
//   (2) Almgren 3/2-power impact  Σ_i c_i |w_i − w0_i|^{3/2}         (square-root law)
//   (3) short-leg borrow split    Σ_i fee_i · max(0, −w_i)            (hard-to-borrow fee)
//       with a locate box         max(0, −w_i) ≤ locate_cap_i         (locate availability)
//
// ===========================================================================
//  AugmentedConeV1 formulation (the original QP + SOC ADMM)
// ===========================================================================
//  (1) aux r_i ≥ |Δ_i| via the two rows  w_i − r_i ≤ w0_i,  −w_i − r_i ≤ −w0_i; cost κ_i r_i.
//  (2) aux (u_i, s_i, τ_i): u_i ≥ |Δ_i| by the same two rows, cost c_i τ_i, and TWO rotated
//      second-order cones (MOSEK modeling cookbook, power cones via rotated quadratics):
//          2 s_i τ_i ≥ u_i²            and       2 u_i (1/8) ≥ s_i²
//      ⇒ u² ≤ 2 s τ ≤ τ √u  ⇒  τ ≥ u^{3/2}, tight at the optimum. A rotated cone
//      2ab ≥ c² is the standard SOC ‖(c, (a−b)/√2)‖₂ ≤ (a+b)/√2, emitted as a variable-apex
//      SocBlock (apex row first) so the ADMM z-update projects it with soc_project; the
//      constant 1/8 rides on the block's offset.
//  (3) aux b_i with  w_i + b_i ≥ 0  and  0 ≤ b_i ≤ locate_cap_i; cost fee_i b_i. At the
//      optimum b_i = max(0, −w_i) when fee_i > 0; with fee_i == 0 the locate box alone
//      enforces w_i ≥ −locate_cap_i.
//
//  Only names with a POSITIVE coefficient (or a finite locate cap) get columns/rows, in
//  ascending name order (R1 — the pattern is a pure function of which coefficients are
//  positive). New columns are appended AFTER the base aux block, new linear rows after the
//  base rows, and the cone blocks last, so every base offset (factor rows, tracking cones,
//  robust apex) is unchanged. With every term inactive the base AugmentedQp is returned
//  verbatim ⇒ the solve is byte-identical to ConstrainedQpSolver::solve_with_cert (R10).
//
//  FactorProxV2 explicitly uses the factor-space ADMM with per-name closed-form
//  3/2 trade proximal steps and asymmetric borrow proximal steps. Hard execution
//  caps are folded into boxes; no scalar-only polish or augmented fallback runs.
//
//  Units: coefficients are in objective units per unit weight (the caller maps bps and
//  per-period borrow into the same units as −α). w0 is the pre-trade book.

#include <span>

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/risk/cost_types.hpp"
#include "atx/engine/risk/qp_augment.hpp" // AugmentedQp
#include "atx/engine/risk/qp_solver.hpp"  // ConstrainedQpSolver, QpProblem, QpResult

namespace atx::engine::risk {

// Realized cost of a book under the terms (objective units).
struct TradeCostBreakdown {
  atx::f64 linear = 0.0;
  atx::f64 impact = 0.0;
  atx::f64 borrow = 0.0;
  [[nodiscard]] atx::f64 total() const noexcept { return linear + impact + borrow; }
};

// Validate the terms against M: every non-empty span has length M, every entry is finite
// and ≥ 0 (a locate cap may be +inf). Err(InvalidArgument) naming the offending field.
[[nodiscard]] atx::core::Status validate_cost_terms(const TradeCostTerms &terms, atx::usize m);

// Append the cost columns/rows/cones to `base` (see the header block). `base.n_w` is M.
// Err(InvalidArgument) on malformed terms. Inactive terms ⇒ `base` returned unchanged.
[[nodiscard]] atx::core::Result<AugmentedQp> append_cost_terms(AugmentedQp base,
                                                               const TradeCostTerms &terms);

// Realized per-term cost of the book `w` (length M). Err on malformed terms / length.
[[nodiscard]] atx::core::Result<TradeCostBreakdown>
evaluate_trade_costs(const TradeCostTerms &terms, std::span<const atx::f64> w);

// Build the augmented problem for `p`, append the trade-cost terms, and solve it through
// the solver's standard pipeline (Ruiz → ADMM → polish → gate). Same error contract as
// ConstrainedQpSolver::solve_with_cert plus the term validation above. `sched` (optional)
// selects the deterministic adaptive-rho ADMM; `ws` (optional) warm-starts from a previous
// costed solve's x_full / y_full (same terms layout). The explicit V2 rule instead
// requires the factor layout and checks requested stationarity/consensus tolerances
// before returning a feasible book. V1 refuses nonempty executable limit spans.
[[nodiscard]] atx::core::Result<QpResult>
solve_with_costs(const ConstrainedQpSolver &solver, const QpProblem &p, const TradeCostTerms &terms,
                 const AdmmSchedule *sched = nullptr, const WarmStart *ws = nullptr,
                 CostedSolveRule rule = CostedSolveRule::AugmentedConeV1);

} // namespace atx::engine::risk
