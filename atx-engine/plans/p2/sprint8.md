# Sprint 8 — Production-Grade Factor-Augmented Portfolio Optimizer

**Branch:** `feat/p2-s8-optimizer`
**Status:** Complete — whole-branch review ✅ Mergeable (zero Critical/Important, 2026-06-18)
**Ledger:** [`sprint-8-progress.md`](sprint-8-progress.md)
**Plan:** [`sprint-8-optimizer-implementation-plan.md`](sprint-8-optimizer-implementation-plan.md)

---

## What this sprint delivers

Sprint 8 upgrades the portfolio optimizer from the S1 fixed-iteration QP baseline into a
**commercial-grade factor-augmented quadratic programming engine** with a full constraint and
cone surface. Every prior guarantee is preserved bit-for-bit.

---

## Performance

The as-built solver had an O(M²) defect: it densified the constraint matrix Ã at every
iteration, making each ADMM step O(M²) in the number of assets. Sprint 8 eliminates it.

The new solver uses a factor-augmented sparse KKT system (`y = Xᵀw`, Hessian
`P = blkdiag(2λD, 2λF)`) paired with a deterministic no-pivot quasi-definite LDLᵀ
factorization (AMD ordering; QDLDL-style, no sqrt, fixed-sign pivots). The factor is computed
once per solve and reused across all ADMM iterations. Ruiz equilibration (10 fixed passes) and
deterministic polish (3 fixed refinement passes, active-set partitioned by dual sign) are
applied on top.

Measured speedup (PCH-off debug build, M=3000 assets, K=64 factors, same fixed-iteration
budget as the baseline):

- Core solve (polish off): **35.1×** faster, 62 s → 1.8 s
- Full solve (polish on):  **21.5×** faster, 62 s → 2.9 s
- Scaling exponent over M∈{500..5000}: **~M^1.12** (was ~M^2.0)

The factor-augmented sparse form is O(M·K²) in fill and near-linear in practice.

---

## Determinism guarantees

Every prior determinism invariant is preserved and extended:

- Same inputs → byte-identical book and result digest across all runs and all thread counts.
- No RNG, no clock, no pivoting, no unordered containers in any hot path.
- Fixed iteration counts at every level: Ruiz passes (10), ADMM outer (fixed per config),
  polish refinement passes (3), cone projection inner steps (fixed).
- The 8 regression pins (§0.4 of the plan) are held bit-for-bit through every sprint unit.
- The S7 boundary pin holds through S8.4 and S8.7: a single-horizon, full-trade-rate, minimal
  constraint set solve is byte-identical to `MultiPeriodOptimizer`.
- Golden-bit hash stable across `Eigen::setNbThreads` ∈ {1, 2, 4, 8}.

---

## Constraint surface

The optimizer now enforces the full commercial constraint algebra. Every constraint listed below
is either a hard linear row or a second-order cone; all are enforced within solver tolerance and
proven per-constraint by the test suite.

### Linear constraints (hard rows)

| Constraint | API |
|---|---|
| Dollar-neutral (`Σw = 0`) | existing gross/net rows |
| Gross-leverage cap (`Σ|w| ≤ L`) | `ConstraintSet::gross_leverage` |
| Net-leverage band | `ConstraintSet::net_leverage` |
| Per-asset position cap (`|w_i| ≤ c_i`) | `ConstraintSet::position_cap` |
| %ADV cap (`|w_i| ≤ a_i · ADV_i`) | `ConstraintSet::adv_cap` |
| %shares-outstanding cap | `ConstraintSet::shares_cap` |
| Factor-exposure budgets (`|Xᵀw| ≤ b`) | `ConstraintSet::factor_exposure` |
| Beta neutrality | `ConstraintSet::beta` |
| Sector-net budgets | `ConstraintSet::sector_net` |
| Turnover budget (L1-split auxiliary rows) | `ConstraintSet::turnover_budget` |

### Conic constraints (SOCP)

| Constraint | Form | API |
|---|---|---|
| Tracking-error budget | `‖[L_Fᵀy; sqrt(D)∘w]‖₂ ≤ te` (K+M rows, one SocBlock) | `ConstraintSet::track` |
| Per-sector risk budget | `‖[L_Fᵀ Xᵀ(mask_g∘w); sqrt(D)∘(mask_g∘w)]‖₂ ≤ σ_g` (one K+M SocBlock per finite-σ sector) | `ConstraintSet::sector_risk` |
| Robust optimization | worst-case-α over Goldfarb-Iyengar factor ellipsoid → epigraph `t ≥ ‖Ω_f^{1/2}y‖₂`, cost `+κt` (variable-apex SOC) | `ConstraintSet::robust` (κ > 0) |

### Cost terms in the objective

| Cost | Form | API |
|---|---|---|
| √-impact quadratic surrogate | `P[i,i] += 2c_i`, `q[i] += -2c_i·w_prev_i` (PSD-preserving, inert when off) | `ConstraintSet::impact` (c_i ≥ 0) |

### Constraint hierarchy and infeasibility elasticity

When the hard constraint set is infeasible, `solve_elastic` returns the closest feasible book
under a priority-ranked relaxation rather than a hard error. The relaxation rule:

- Hard rows (`priority = kHard`) are never relaxed — they bind exactly or the solve fails.
- Elastic rows are relaxed under a penalty `γ_p = 2 · 4^priority` (higher priority → more
  expensive to relax → relaxed less).
- Elastic ball cones are rebuilt as variable-apex SOC with a radius slack.
- Gross and turnover L1 budgets get a dedicated budget-row slack.
- A feasible problem is byte-identical to the plain hard-path solve (elastic path unreached).

---

## Multi-period cost-to-go (Gârleanu-Pedersen)

`gp_aim_and_value` computes the horizon-decay-weighted aim portfolio (ᾱ) and the cost-to-go
curvature A_xx under the scalar-Λ reduction (A_xx = 2λV, the principled default for the sprint-1
scalar trade-rate convention per plan §0.6). Folding the cost-to-go tail into the single-period
QP reduces to `q = −ᾱ`, `P = 2λV` — byte-identical to the augmented book at the boundary and
consistent with the S7 multi-period boundary pin (H=1 reduces to the single-period Markowitz
target, bit-for-bit).

The shipped GP implementation is correct for the scalar-Λ case; the richer matrix-Λ
(finite-horizon backward-Riccati A_xx(H) ≠ 2λV) and the true joint O(N·H) stacked QP with
inter-period turnover coupling are recorded lifts (see Residuals below).

---

## Header/source split and compile-time improvement

S8.8a split `FactorModel` (the dominant include hub, 42 translation-unit fan-out) and
`garleanu_pedersen` + `multi_horizon` from header-only to `.cpp` bodies in `src/risk/`. The
dead `QpConfig::kkt_iters` field was also removed.

Measured on the PCH-off CI preset (`ninja`, 27 risk translation units, `-j8`):
clean-build time reduced by **~31%** (191.58 s → ~132 s median). All byte-identity pins and the
full risk test suite (220/220) pass unchanged. Build behavior is identical to pre-split.

---

## Known residuals (post-merge, not blockers)

The following items are legitimate post-merge follow-up work. Each feature they relate to is
fully functional on the shipped path; these are driver-convenience wiring and richer
generalizations, not present-but-inert features.

1. **√-impact coefficient pricing wiring.** The optimizer-side surrogate is complete. The driver
   does not yet derive `c_i` from `exec::ImpactCfg` / `capacity.hpp::impact_cost_bps`; tests
   inject coefficients by hand. A driver-side pass that fits a local quadratic to
   `Y·σ_i·part^δ` at expected trade size completes the end-to-end pricing trace.

2. **Elasticity driver auto-wiring.** `solve_elastic` is the opt-in entry. The single-period
   drivers (`PortfolioOptimizer`, `MultiHorizonOptimizer::solve_augmented`) return
   `Result<vector>` with no relaxation-report channel; a report-carrying return type + seam
   swap is required to auto-invoke elasticity and surface the relaxation report to callers.

3. **Matrix-Riccati A_xx + true stacked-MPC QP.** Shipped: scalar-Λ A_xx = 2λV (correct for
   the sprint-1 convention). Lift: finite-horizon backward-Riccati A_xx(H) reducing to 2λV at
   H=1, and the joint O(N·H) stacked QP with inter-period turnover coupling. A dedicated
   follow-up sprint is needed.

4. **qp_solver / kkt_ldl / exposures header→.cpp split.** FactorModel and the multi-period
   headers were split in S8.8a. The remaining three headers (qp_solver ~900 LOC, kkt_ldl sparse
   LDL, exposures hot inline helpers) are the next fan-out reduction pass.

5. **Iter-budget / auto-rho tuning.** Cone and elastic-relaxed solves need per-caller rho/iters
   above `QpConfig` defaults. Defaults are unchanged (byte-identity preserved); a problem-scaled
   default or auto-rho heuristic would prevent spurious infeasibility Errs for production callers
   wiring tight box caps or cones.

---

## Gate summary

| Gate | Meaning | Result |
|---|---|---|
| G-PERF | ≥20× speedup at M=3000/K=64; exponent ≤1.3 | **PASS** — 35.1× core / 21.5× full; ~M^1.12 |
| G-PIN | All 8 regression pins + S7 boundary pin bit-identical | **PASS** |
| G-DET | Golden-hash stable across runs and {1,2,4,8} threads | **PASS** |
| G-DIFF | QP vs dense oracle ≤1e-8; cones vs closed form; GP vs hand oracle | **PASS** |
| G-CONSTRAINT | Every constraint proven per-constraint | **PASS** |
| G-ELASTIC | Feasible ⇒ byte-identical; infeasible ⇒ relaxation report | **PASS** |
| G-DISABLED | 3 `MultiHorizonIntegration` tests re-enabled green (482/415/240 ms) | **PASS** |
| G-COMPILE | ~31% PCH-off build reduction; zero behavior change | **PASS** |
