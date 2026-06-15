# Sprint S8 (p2) — Production-Grade Portfolio Optimizer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the
> FROZEN *how*; the **what** is the S8 section of [`ROADMAP.md`](ROADMAP.md) (this module embeds the sprint spec in the
> ROADMAP — there is no separate `sprint-8-…​.md` spec file). On conflict, **§0 (this plan's as-built amendment)
> overrides** the ROADMAP sketch. Code-quality gate: [`../docs/implementation-quality.md`](../docs/implementation-quality.md)
> + [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md).

**Goal:** Take the as-built p2 **S1** optimizer (`risk::{ConstrainedQpSolver, ConstraintSet, MultiHorizonOptimizer}`)
— a *functionally complete but performance-prototype* constrained multi-horizon solver — and build it into a
**commercial-grade, large-scale, deterministic** portfolio optimizer that rivals OSQP / Axioma / Barra Open Optimizer
on the problems quant shops actually face: **maximize expected alpha over a multi-period horizon subject to a full
constraint algebra** — per-name caps as **% of ADV** (participation) and **% of shares outstanding** (ownership),
**factor-exposure** and **beta** constraints, **sector caps and sector-risk (variance) budgets**, **tracking-error**,
**gross/net leverage**, **turnover budgets**, and **transaction/holding cost** in the objective — at **M in the
thousands** of names against a **factored** Barra risk model `V = X F Xᵀ + D` that is never densified, in **bitwise-
reproducible** fixed-iteration arithmetic.

The headline is **performance**: the as-built `ConstrainedQpSolver` is correct but **O(M²)** per outer iteration
because it **densifies the augmented constraint matrix Ã** (`qp_solver.hpp:193`) and runs a fixed `600 × 50` PCG with a
per-solve-rebuilt preconditioner — the cause of the **~20 s/run** `MultiHorizonOptimizer::run` regression that forced
**three `DISABLED_MultiHorizonIntegration` tests** off at S4. S8 rebuilds the solver core around the **factor-augmented
sparse QP** (`y = Xᵀw` ⇒ Hessian `blkdiag(D, F)`, a **sparse quasi-definite KKT** factored **once** with a **no-pivot
static LDLᵀ**), bringing it to **O(M·K²)**, then **completes the constraint surface**, **adds second-order-cone
support** (tracking-error / sector-risk / robust), a **constraint-hierarchy infeasibility layer**, and a **true
multi-period Gârleanu-Pedersen cost-to-go**, and finally **splits the header-only risk subsystem into header + `.cpp`
(pimpl)** to cut compile times.

**Architecture:** A solver-core rewrite-behind-a-stable-API plus additive constraint/cone/multi-period layers in the
existing `risk/` layer (namespace `atx::engine::risk`), composed so the new optimizer is a **strict generalization** of
the as-built S1 solver — never a from-scratch parallel solver:
- `risk/qp_solver.hpp` (**rewritten internals, identical public API** `ConstrainedQpSolver::solve(const QpProblem&)`):
  factor-augmented sparse ADMM over the regularized quasi-definite KKT, Ruiz equilibration, deterministic polishing,
  infeasibility certificates, warm-start.
- `risk/kkt_ldl.hpp` (**new**): the deterministic no-pivot quasi-definite LDLᵀ + cached AMD symbolic ordering — the
  engine-side fallback for the **atx-core L7 `ldl_quasidef`** lift.
- `risk/constraints.hpp` (**extended**): `ParticipationCap` (%ADV), `OwnershipCap` (%shares-out), `SectorRiskBudget`,
  `TrackingError`, plus per-descriptor **priority/elasticity** metadata.
- `risk/reference_data.hpp` (**new**): the `CapacityRef{adv, shares_out, price, nav}` inputs the %ADV / %shares caps
  materialize against (today's `capacity.hpp` has ADV/impact for a *report* only; %shares-out has **no data path**).
- `risk/cone.hpp` (**new**): SOCP cone descriptors + deterministic cone projections (tracking-error, sector-risk,
  Goldfarb-Iyengar robust uncertainty) + the √-impact cost surrogate.
- `risk/elasticity.hpp` (**new**): the constraint-hierarchy / minimize-violation layer (priority-ranked γ ladder →
  always returns a feasible-or-closest book).
- `risk/garleanu_pedersen.hpp` (**new**): the closed-form GP/Riccati aim + value-function matrix — the unconstrained
  fast path **and** the differential oracle.
- `risk/multi_horizon.hpp` (**extended**): receding-horizon constrained SPO with the GP **cost-to-go tail**; the
  `stacked_mpc` flag becomes a real (benched) path.
- `risk/optimizer.hpp` + `risk/multi_period.hpp` (**wired**): route the augmented-constraint case through the QP while
  the minimal-constraint set keeps the proven projected/proximal fast path (the boundary pins).
- **Header/source split:** `src/risk/*.cpp` move the heavy bodies out of the headers (pimpl `FactorModel` first), added
  to the existing `atx-engine` STATIC library.

The **boundary pins** hold the whole sprint honest: every regression pin of S1/S7/P4-9 (enumerated §0.4) must survive
bit-for-bit; the rewrite changes *how fast*, never *what book*.

**Tech Stack:** C++20, namespace `atx::engine::risk`. Eigen is the **already-vendored** dense-LA dependency (storage +
decompositions only; **all reductions hand-rolled in fixed index order** for determinism — the as-built rule). **No new
external solver dependency** (no OSQP / Clarabel / Mosek / BLAS) — we vendor *ideas* (QDLDL's no-pivot quasi-definite
LDLᵀ, OSQP's linsys seam), never code; cvxportfolio is GPL-3.0 (design pattern only). New numeric kernels are
**atx-core L7 requests with engine-side fallbacks** (ROADMAP rule #7). Reuses `risk::{FactorModel, FactorComponents,
ExposureMatrix, MultiPeriodOptimizer, RebalanceSchedule}`, `book::CostInputs`, `cost::*`, `atx::core::{linalg
(MatX/VecX), Result, Status}`. GoogleTest under `atx-engine/tests/src/*_test.cpp` (CONFIGURE_DEPENDS — no per-unit CMake
edit for tests). clang-cl `/W4 /permissive- /WX` **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). Build + ctest are
the gates; clang-tidy disabled (noise).

---

## §0 — As-built reconciliation amendment (the recon fixes)

> **Recon target (kickoff):** the merged p2 engine after S1–S6 (the S1 optimizer is on `feat/p2-s1-multi-horizon`,
> pending merge — confirm the merge SHA at S8-0 and amend these notes against the *actual* merged headers before
> dispatching S8.1). All file:line refs below are from the S8-0 code review; **re-verify them as the first act of
> S8-0** (OneDrive ripgrep times out — read the named files in full).

### 0.1 The constraint algebra + a general QP already exist and are CORRECT — S8 is perf + reach + wiring, not a greenfield
`risk/constraints.hpp` (362 LOC) already ships the composable descriptors `GrossNet`, `PositionCap`, `FactorExposure`,
`GroupCap`, `BetaNeutral`, `TurnoverBudget` and `ConstraintSet::materialize(const MatX& X, span w_prev, usize M) ->
Result<MaterializedConstraints>` (`constraints.hpp:134`), with a fixed row order (dollar-neutral, position box, factor
exposure, group, beta — `:153-157`) and gross-L1 / turnover carried as **metadata** (`MaterializedConstraints{MatX A;
VecX l, u; f64 gross_l1_budget; bool has_turnover; f64 turnover_budget; vector<f64> turnover_ref}`, `:105`).
`risk/qp_solver.hpp` (444 LOC) already ships a **genuinely general** OSQP-class ADMM (`run_admm`, `:329`) with a
matrix-free PCG x-update (`pcg_solve`, `:360`, Jacobi-preconditioned from `specific_var()`), `P = 2λV` applied
matrix-free via `FactorModel::apply` (`:404`), zero-init duals, order-fixed `dot()` (`:456`), no convergence early-exit
(outer `iters=600`, inner `kkt_iters=50`). **Decision:** S8 **keeps the public `QpProblem` / `ConstrainedQpSolver`
contract verbatim** and rewrites only the *internals* (§0.2); it **extends** `ConstraintSet`/`MaterializedConstraints`
(§0.3) rather than replacing them; and it **wires** the QP into the single-period drivers (§0.5).

### 0.2 The performance defect is the dense Ã, not the factored V — the fix is factor augmentation
The "matrix-free" claim holds **only for the V block**: the augmented constraint matrix `Ã` is **materialized dense**
(`qp_solver.hpp:193`) at `aug_row_count ≈ O(M)` rows over `n ≈ 3M` variables (the gross/turnover L1 splits add `2M`
aux vars), so each outer iteration is **O(M²)** and `build_precond` (O(R̃·n)) is **rebuilt every `solve()`**. At
`600 × 50` PCG matvecs this is the **~20 s/run** the S4 ledger flagged (residual #1) and the reason
`DISABLED_MultiHorizonIntegration` R1/R2/R3 were turned off. **Decision (S8.1–S8.3):** reformulate to the
**factor-augmented sparse QP** — introduce `y = Xᵀw` (K aux vars, K equality rows `y − Xᵀw = 0`) so `½wᵀVw = ½wᵀDw +
½yᵀFy` and the Hessian becomes **`P = blkdiag(D, F)`** (D diagonal M-nnz, F dense K×K with K ≤ 256). The full KKT
`[[P+σI, Ãᵀ],[Ã, −ρ⁻¹I]]` is then **sparse quasi-definite** (Vanderbei 1995): factorize **once per solve** with a
**no-pivot static LDLᵀ** (deterministic by construction — no data-dependent pivot order) and **reuse the factor every
ADMM iteration** (the matrix is constant when ρ, σ are fixed); cache the **symbolic** factorization across rebalances
(the sparsity pattern is fixed for a fixed constraint set). This takes the per-solve cost to **O(M·K²)** and removes the
dense Ã entirely. Eigen's `SparseMatrix` + a vendored QDLDL-style numeric LDLᵀ over an AMD ordering computed once at
symbolic setup.

### 0.3 %ADV and %shares-outstanding caps have no data path — capacity.hpp is a *report*, not a constraint
`risk/capacity.hpp` computes a `capacity_curve` (`:264`) of `CapacityPoint{aum, net_edge_bps}` by reading the execution
sim's `ImpactCfg` (`temp_i = Y·σ_i·part_i^δ`, `part_i = shares_i/ADV_i`, `:245`) — i.e. an **erosion report**, and the
only feedback into the optimizer is the scalar `capacity_gross` ceiling (`multi_period.hpp:138`). There is **no per-name
`|w_i|·NAV/price_i ≤ ρ·H·ADV_i` participation row and no `|w_i|·NAV/price_i ≤ κ·shares_out_i` ownership row — and
shares-outstanding data does not exist anywhere in `risk/`.** **Decision (S8.4):** add `risk/reference_data.hpp` with
`CapacityRef{span<const f64> adv, shares_out, price; f64 nav; f64 horizon_days}` and constraint descriptors
`ParticipationCap{f64 adv_frac}` / `OwnershipCap{f64 shares_frac}` that materialize to **diagonal box rows** on `w`
(both are position bounds: `|w_i| ≤ min(ρ·H·ADV_i·price_i/NAV, κ·shares_out_i·price_i/NAV, name_cap)`). These are the
cheapest constraints in the QP (diagonal), so they cost nothing structurally.

### 0.4 The regression pins are load-bearing — the rewrite changes speed, never the book
The following must survive **bit-for-bit** (the differential anchors). S8.1–S8.3 prove them on the *minimal-constraint*
path; S8.4–S8.7 prove they still hold as constraints/cones/cost-to-go are added.

- **Pin #1 (λ=0 exact):** `PortfolioOptimizer::solve(α, V, {})` with λ=0, κ=0, cap-off reduces bit-for-bit (≤1e-9) to the
  `WeightPolicy` dollar-neutral book `gross_normalize(demean(α))` (`optimizer.hpp:26-32`, branch on `risk_aversion==0.0`).
- **Pin #2 (κ=0 prox identity):** `turnover_penalty==0` ⇒ the turnover prox is exactly the identity (`optimizer.hpp:268`).
- **Pin #3 (λ>0 variance tilt) / Pin #4 (κ>0 turnover cut):** the documented monotone behaviors (`optimizer.hpp:46-48`).
- **NaN pin:** a NaN-α cell gets exactly 0 weight, excluded from demean/Σ (`optimizer.hpp:61-64`).
- **Empty-w_prev boundary:** empty `w_prev` ≡ explicit all-zero `w_prev`, bitwise (`optimizer.hpp:135-138`).
- **Signed-zero pin:** `blend_toward` special-cases `rate==1.0` to assign the target verbatim (preserves −0.0;
  `bit_cast<u64>` pin, `multi_period.hpp:158-175`, `multi_horizon.hpp:286-305`).
- **S7/S1 boundary pin (R7):** `MultiHorizonOptimizer` with one `SignalHorizon::identity()`, the minimal constraint set,
  and full trade-rate is **byte-identical** to `MultiPeriodOptimizer.run(...)` (`multi_horizon.hpp:28-37`).

> **The dispatch contract that protects the pins:** the minimal constraint set (`GrossNet [+ PositionCap]` only) keeps
> the as-built projected/proximal fast path (`PortfolioOptimizer::solve`) — it is *already* bit-for-bit on the pins and
> the rewrite **does not touch it**. The rewritten ADMM owns only the **augmented** path (any factor/group/beta/turnover/
> participation/ownership/cone constraint). This is the S1 §0.5 decision, carried forward and made explicit: the boundary
> pin exercises the fast path, the new solver owns the reach.

### 0.5 The QP is narrowly wired — only `MultiHorizonOptimizer::solve_augmented` reaches it
`ConstrainedQpSolver` is reachable **only** through `MultiHorizonOptimizer::solve_augmented` (`multi_horizon.hpp:270`,
`q = −aim`); `PortfolioOptimizer` and `MultiPeriodOptimizer` ignore it. **Decision (S8.4):** add the same minimal-vs-
augmented dispatch to `PortfolioOptimizer::solve` and `MultiPeriodOptimizer::run` so a single-period book can carry the
full constraint set too (the S2 meta-book and any single-horizon sleeve get it for free), gated so the minimal set still
takes the pinned fast path.

### 0.6 The multi-period objective is a horizon-AVERAGE aim, and stacked MPC is `NotImplemented`
`MultiHorizonOptimizer::gp_aim` (`multi_horizon.hpp:194`) collapses the forecast trajectory to a simple **horizon
average**, and `stacked_mpc=true` returns `Err(NotImplemented)` (`:154`). This is the GP *aim direction* without the GP
*trade-rate dynamics* (the Riccati value function that says **how far** to trade toward the aim under quadratic cost).
**Decision (S8.7):** add `risk/garleanu_pedersen.hpp` — the closed-form aim `A_xx⁻¹A_xf f_t` and value-function matrix
`A_xx` (the algebraic-Riccati solution for quadratic cost + mean-reverting signals) — and append its quadratic
**cost-to-go tail** `−½(w − aim)ᵀA_xx(w − aim)` to the single-period QP objective (the MPC trick: most multi-period
foresight at single-period cost). The pure unconstrained closed form is both a **fast path** (no solver) and the
**differential oracle** for the constrained QP with constraints relaxed. The full **stacked** MPO QP (O(N·H) variables)
becomes a real benched path behind `stacked_mpc=true` (no longer `NotImplemented`), recorded as the higher-cost option
for time-varying forecasts.

### 0.7 The risk subsystem is 100% header-only and `factor_model.hpp` is the include hub
There is **no `src/risk/`** — every `risk/*.hpp` is concrete (no templates) with inline-defined methods, compiled once
per consuming TU (~20 `risk_*_test.cpp`). `factor_model.hpp` (875 LOC) transitively drags Eigen + the entire S8
covariance pipeline (exposures, cov_ewma, eigen_adjust, specific_risk, stat_factor_model, vol_regime, horizon_blend) +
combine + cost into **every** TU that names `FactorModel`; `qp_solver.hpp` and both drivers include it. **Decision
(S8.8):** split the subsystem header→`.cpp` (pimpl), **`FactorModel` first** (it is the transitive root). Nothing is a
template, so the split is clean — move bodies to `src/risk/*.cpp`, add to `add_library(atx-engine STATIC ...)`
(`atx-engine/CMakeLists.txt:1`, which currently lists **zero** risk sources), keep POD config/result structs in the
headers. The `dev` preset already has PCH+unity+sccache; the `ninja` CI preset has PCH **off**, so the split's win is
largest in CI. Measure before/after.

> **Net scope shift vs the ROADMAP sketch:** S8 is **rewrite-behind-a-stable-API + additive reach**. The genuinely-new
> numeric paths are the **factor-augmented sparse KKT + no-pivot LDLᵀ** (S8.1–S8.2), the **SOCP cone projections**
> (S8.5), the **constraint-hierarchy minimize-violation solve** (S8.6), and the **GP Riccati cost-to-go** (S8.7). The
> dominant risk is a **silently-wrong-but-fast solver** — one that loses a pin, violates a constraint it claims to
> enforce, or diverges from the as-built QP on the augmented path. Every one is a named gate in §3.

---

## §1 — Design rules (non-negotiable; every unit is checked against them)

| # | Rule | Why / source |
|---|------|--------------|
| **R1** | **Deterministic fixed-iteration — no convergence early-exit.** Ruiz passes, ADMM outer count, polish refinement passes, cone-projection inner steps, and the LDLᵀ are all fixed-count and order-fixed; same inputs ⇒ byte-identical book + digest. | p2 invariant #1; as-built `qp_solver.hpp:20-27` (explicit "a convergence test makes the iteration count input-dependent, breaking byte-reproducibility"); OSQP fixed-iter [Stellato 2020]. |
| **R2** | **No look-ahead / receding-horizon PIT.** The objective sums over a forward horizon but **executes only the first move**; the trajectory `α_{t+h}=α_t·decay(h)` and the GP cost-to-go are causal functions of `≤ t` data; every fitted object is trailing-fit. Truncation-invariance is the test. | p2 invariant #2/#4; Boyd 2017 MPC look-ahead safety; the S1/S7 receding-horizon contract. |
| **R3** | **Constraints are enforced exactly, then elastically.** Hard `l ≤ Ãx ≤ u` rows + SOC cones are satisfied at the returned book within solver tolerance, proven per-constraint; on infeasibility the **constraint-hierarchy** layer returns the closest feasible book under a priority-ranked relaxation (never a silent violation, never a hard failure). | Institutional mandate discipline; Axioma constraint-elasticity; Boyd 2017 §4.6 soft constraints. |
| **R4** | **Neither V nor Ã is ever densified.** `V = XFXᵀ+D` enters via the augmentation `y=Xᵀw` (Hessian `blkdiag(D,F)`) and the constraint matrix is **sparse** (factor rows are `Xᵀ`; box/group/turnover rows are structured) — the KKT is sparse quasi-definite, O(M·K²), never O(M²). | p2 invariant #6; the as-built dense-Ã defect (§0.2); OSQP/Mosek factor-augmentation [Stellato 2020; Mosek Cookbook §5]. |
| **R5** | **Static quasi-definite factorization — no numerical pivoting.** The regularized KKT `[[P+σI, Ãᵀ],[Ã, −ρ⁻¹I]]` (σ,ρ>0) is quasi-definite ⇒ strongly factorizable in any fixed pivot order with **no pivoting** (Vanderbei Thm 2) — this single choice gives **determinism** (no data-dependent pivot branch) **and** conditioning (caps κ even when P is rank-deficient, which the factor model is). | Vanderbei 1995 *SIAM J. Optim.* 5(1):100-113; OSQP §3; QDLDL division-free no-pivot LDLᵀ. |
| **R6** | **Warm-start is decoupled from termination.** Warm-start `(x,y)` across rebalances/ADMM as an **accuracy lever under a fixed compute budget** — the loop runs its fixed count regardless of residuals, so the output stays a fixed-length operator composition (bit-identical). The thing that breaks reproducibility is data-dependent control flow, not warm-starting. | p2 invariant #1; OSQP warm-start [Stellato 2020]; the determinism crux (Agent B §F.3). |
| **R7** | **The factored covariance is consumed for free; D is floored.** `D ≥ d_min > 0` at the source (O(M), structure-preserving) makes `V≻0` and the augmentation numerically benign; F's Cholesky is the cached `FactorModel` capacitance. No bare Woodbury in the main path (unstable for tiny D — Goldfarb-Scheinberg). | p2 invariant #6; `factor_model.hpp` `kSpecificVarFloor=1e-12`; Agent B §A.1/§F.5. |
| **R8** | **Multi-period via GP aim + Riccati cost-to-go.** For quadratic cost + mean-reverting signals the optimal dynamic policy trades partway toward a horizon-decay-weighted aim; S8 appends the GP value-function tail to the single-period QP (constraints are GP's missing piece). Pure GP closed form is the unconstrained fast path **and** the oracle. | Gârleanu-Pedersen 2013 *J. Finance* 68(6); generalizes S1's `gp_aim` average; Boyd 2017 receding-horizon. |
| **R9** | **Honest, calibrated cost in the objective.** The QP prices the S6-calibrated κ in the turnover term **and** a market-impact term (√-impact via a PWL or quadratic surrogate that stays in the QP/SOCP), via `book::CostInputs`; per-horizon turnover budgets price fast vs slow churn. | p2 invariant #5; Almgren-Chriss 2000 (quadratic-in-rate impact); Boyd 2017 Eq. 2.2. |
| **R10** | **Reduce to S1/S7/P4-9 on the boundary.** All pins in §0.4 hold bit-for-bit; the rewrite is pinned to the proven layer. | `module.md` carry-forward; the S1 §0.5 / S7 boundary-pin precedent. |
| **R11** | **Differential correctness of every kernel.** The LDLᵀ (vs a dense reference factorization), Ruiz scaling (vs a reference), each cone projection (vs its closed form), the GP closed form (vs a hand-rolled Riccati), and the augmented QP (vs the as-built dense-Ã QP) each ship an obviously-correct reference + a bit-/ULP-bounded differential test. | p2 invariant #7; the p0/p1 differential-test precedent for every new compute kernel. |

**One-sentence thesis:** *S1 already proved the constraint algebra, the factored-V apply, and the schedule walk are
correct and deterministic — so S8 is the layer that (a) makes them **fast** by augmenting `y=Xᵀw` into a sparse
quasi-definite KKT factored once with a no-pivot LDLᵀ (R4/R5), (b) **completes the reach** with %ADV/%shares/sector-risk
box+cone constraints, conic tracking-error/robust, infeasibility elasticity, and the GP cost-to-go (R3/R8/R9), and (c)
stays pinned bit-for-bit to the proven layer (R10); the only genuinely-new correctness risks are the LDLᵀ/cone
determinism, the per-constraint satisfaction, and the bit-for-bit reduction.*

---

## §1A — State-of-the-art research grounding (verified literature)

> Sourced via a fan-out web + source-code research pass (2026-06-14), cross-checked against the S1 plan's §1A. Primary
> sources only (arXiv / DOI / publisher / OSS repo). A few primary PDFs (OSQP, Vanderbei, Ruiz) were read directly but
> exact constants / equation numbers must be **re-verified against the originals before coding** (flagged R-VERIFY).

### Track A — The solver core (the *how it's fast and deterministic*)
- **The canonical problem form.** OSQP: `min ½xᵀPx + qᵀx s.t. l ≤ Ax ≤ u`, P only PSD (handles our rank-deficient
  factor P), A no full-rank requirement. *Stellato, Banjac, Goulart, Bemporad, Boyd, "OSQP: An Operator Splitting Solver
  for QPs," Math. Prog. Computation 12(4):637-672, 2020 (arXiv:1711.08013).* This is exactly the as-built `QpProblem`
  form — keep it.
- **Factor augmentation (the perf keystone).** `y = Xᵀw` ⇒ `P = blkdiag(D, F)`, nonzeros drop ~M-fold (OSQP's portfolio
  example: `N(K+1)` vs `N(N+1)/2`). Same form in Mosek Portfolio Cookbook §5 and cvxportfolio `FactorModelCovariance`.
  Makes a *direct sparse factorization* cheap (≈O(M·K²)) and numerically benign — preferred over bare Woodbury (unstable
  for tiny D; *Goldfarb & Scheinberg, product-form Cholesky for dense columns, Math. Prog. 2004/05*).
- **Static quasi-definite LDLᵀ (the determinism keystone).** A matrix `[[−E, Aᵀ],[A, F]]`, E,F≻0, is quasi-definite ⇒
  strongly factorizable in **any** fixed pivot order, **no numerical pivoting** (*Vanderbei, "Symmetric Quasidefinite
  Matrices," SIAM J. Optim. 5(1):100-113, 1995, Thm 2*). OSQP's `[[P+σI, Aᵀ],[A, −ρ⁻¹I]]` is this form: σ>0 makes (1,1)
  PD even for rank-deficient P, −ρ⁻¹I handles redundant constraints. Factor **once**, reuse every iteration; cache the
  symbolic across rebalances (OSQP portfolio: 2.6-4× from caching). Ordering: AMD once at setup (*Amestoy-Davis-Duff,
  SIAM J. Matrix Anal. Appl. 17(4), 1996; Algorithm 837, ACM TOMS 30(3), 2004*).
- **Ruiz equilibration (fixed passes).** Iteratively rescale rows/cols to unit ∞-norm, symmetry-preserving, rate-½
  linear convergence — run a **fixed 10 passes**, drop the ε-test from control flow. *Ruiz, RAL-TR-2001-034, 2001.*
- **Deterministic polishing.** Partition active sets by dual sign, solve one regularized reduced KKT system, **fixed 3
  refinement passes** (no tolerance gate) → recovers ~1e-8 accuracy from a moderate ADMM solve. OSQP §4.
- **Settings to copy (R-VERIFY):** ρ=0.1, σ=1e-6, α=1.6 (over-relaxation, ~2× speedup), `adaptive_rho=off`. Infeasibility
  certificates from iterate differences (near-free, deterministic).
- **Why not IPM / active-set as primary:** IPM's KKT changes every iteration (no cached factor), Bunch-Kaufman pivoting
  is a reproducibility hazard, and IPMs **warm-start poorly** (prev optimum on the boundary → log-barrier ∞) — fatal for
  a rebalance loop. Active-set (qpOASES) warm-starts beautifully but is dense-only and has no fixed-iteration bound at
  large M. Both are recorded as **phase-2 fallbacks / oracles**, not the primary. *Clarabel: Goulart & Chen,
  arXiv:2405.12762, 2024; PIQP: Schwan et al., IEEE CDC 2023, arXiv:2304.00290; qpOASES: Ferreau et al., Math. Prog.
  Computation 6(4), 2014.*

### Track B — Multi-period & cost (the *what it optimizes*)
- **Receding-horizon convex MPO.** *Boyd, Busseti, Diamond, Kahn, Koh, Nystrup, Speth, "Multi-Period Trading via Convex
  Optimization," Found. & Trends in Optimization 3(1):1-76, 2017 (arXiv:1705.00109).* Per-period `r̂ᵀz − γ·risk − TC(z) −
  HC(w)`, execute first trade, re-solve. TC (Eq. 2.2): `a|x| + b·σ·|x|^{3/2}/√V + c·x`; HC (Eq. 2.4): `sᵀ(w)⁻`. Companion
  **cvxportfolio is GPL-3.0 — design pattern only (composable Risk/Cost/Constraint objects), copy no code.**
- **Closed-form multi-period (the fast path + oracle).** *Gârleanu & Pedersen, "Dynamic Trading with Predictable Returns
  and Transaction Costs," J. Finance 68(6):2309-2340, 2013.* Quadratic cost + mean-reverting signals ⇒ affine feedback
  (algebraic Riccati): aim `= A_xx⁻¹A_xf f_t`, trade `x_t = x_{t-1} + Λ⁻¹A_xx(aim − x_{t-1})`; under Λ=λΣ a scalar rate.
  Unconstrained ⇒ one offline Riccati solve + per-period matvecs, bitwise-reproducible — the constrained QP with
  constraints relaxed must reproduce it (the oracle). Constraints kill the closed form ⇒ the numerical QP.
- **Impact cost shape / calibration.** *Almgren & Chriss, "Optimal Execution of Portfolio Transactions," J. Risk
  3(2):5-39, 2000* (linear impact ⇒ quadratic-in-rate cost = GP's assumption); *Almgren et al., Risk 18(7), 2005*
  (temporary impact ≈ 3/5 power empirically). The 3/2-power TC term is the only "leave-the-QP" decision — use a **PWL or
  quadratic surrogate** (stays in QP, most reproducible) over a power cone.

### Track C — Constraint & commercial-parity modeling
- **The augmentation carries the constraints cheaply.** With `f = Xᵀw` exposed: factor neutrality = bounds on the
  K-vector f (rank ≤ K), beta = 1 row, sector caps = sparse 0/1 group rows, **sector risk** `‖V^{1/2}(mask_g∘w)‖₂ ≤ σ_g`
  = a **low-rank SOC cone via f_g**, tracking-error = an SOC, %ADV/%shares = diagonal box, L1 leverage/turnover = split
  aux vars. *Goldfarb & Iyengar, "Robust Portfolio Selection," Math. of OR 28(1):1-38, 2003* (robust counterpart = SOCP
  with dimension-K cones — matched to the factor structure).
- **Commercial table-stakes (must rival):** alpha-max / risk-min vs factored model, linear+PWL TC, beta, sector/group,
  position bounds, turnover-by-side, leverage/long-short (130/30), **tracking-error (SOCP, not box-QP)**, ADV/liquidity.
  **Differentiators (pick battles):** constraint-hierarchy/elasticity (Axioma flagship — highest-ROI parity feature),
  robust optimization (SOCP uncertainty), alpha-risk alignment (AAF). **MIQP boundary (deferred):** cardinality, round
  lots, min-holding, min-trade need branch-and-bound — out of S8 scope (recorded lift). *MSCI Barra Open Optimizer;
  Axioma Portfolio Optimizer (Qontigo); Saxena & Stubbs AAF, J. Risk 2012; Boyd et al., "Markowitz at Seventy,"
  arXiv:2401.05080, 2024.*

### Track D — OSS to learn from (vendor ideas, not code)
| Repo | License | Borrow |
|---|---|---|
| **OSQP** (osqp/osqp) | Apache-2.0 | problem-form API, settings taxonomy, setup→solve→update lifecycle, **abstract linsys seam** (where the factor backend plugs in), polish §4. |
| **QDLDL** (osqp/qdldl) | Apache-2.0 | the no-alloc, division-free, **no-pivot** quasi-definite LDLᵀ algorithm (reimplement; supply AMD ordering yourself). |
| **PIQP** (PREDICT-EPFL/piqp) | BSD-2 | the proximal-regularized quasidefinite skeleton (rank-deficient/PSD-P tolerant) — for the phase-2 IPM fallback. |
| **SCS** (cvxgrp/scs) | MIT | the **matrix-free CG / abstract linear-operator** path — escape hatch for extreme M (10⁵+). |
| **cvxportfolio** (cvxgrp/cvxportfolio) | **GPL-3.0** | **design pattern ONLY** — composable Risk/Cost/Constraint; never form M×M. |
| **HiGHS** (ERGO-Code/HiGHS) | MIT | ±1e30 infinity-sentinel convention (matches the as-built `kInf=1e30`), model/Hessian separation, MPS/LP round-trip oracle. |

### Track E — Numerical determinism practices (load-bearing)
Floor `D ≥ d_min` (highest leverage); σI/ρ regularization; fixed-order reductions in `Xᵀu`, `X(Fy)`, dots, residuals
(fixed CSC traversal); `/fp:precise` + `-ffp-contract=off`, pinned ISA, no x87, FTZ/DAZ off, round-nearest-even; **no RNG,
no data-dependent pivoting, fixed iteration counts everywhere**; verify with a **CI golden-bit-hash** across runs / thread
counts / machines. Condition number: factor models have κ(V) ≈ 10⁶-10⁸ — quasi-definite regularization caps it; fixed
1-3 iterative-refinement passes recover accuracy. *Benzi-Golub-Liesen, Acta Numerica 14, 2005; Ahrens-Demmel-Nguyen
(ReproBLAS), ACM TOMS 46(3):22, 2020.*

---

## §2 — Architecture & file map

```
risk/
  qp_solver.hpp        [REWRITE internals, SAME public API]  ConstrainedQpSolver / QpProblem / QpConfig
  kkt_ldl.hpp          [NEW]  quasi-definite no-pivot LDLᵀ + cached AMD symbolic  (atx-core L7 ldl_quasidef fallback)
  qp_augment.hpp       [NEW]  y=Xᵀw augmentation: builds sparse (P,Ã,l,u) from (FactorModel, MaterializedConstraints)
  constraints.hpp      [EXTEND]  + ParticipationCap, OwnershipCap, SectorRiskBudget, TrackingError, priority/elasticity
  reference_data.hpp   [NEW]  CapacityRef{adv, shares_out, price, nav, horizon_days}
  cone.hpp             [NEW]  SOC descriptors + deterministic cone projections + √-impact surrogate
  elasticity.hpp       [NEW]  constraint-hierarchy / minimize-violation (priority γ ladder)
  garleanu_pedersen.hpp[NEW]  closed-form aim + value-function matrix A_xx (fast path + oracle)
  multi_horizon.hpp    [EXTEND]  receding-horizon + GP cost-to-go tail; real stacked_mpc path
  optimizer.hpp        [WIRE]  dispatch augmented→QP, minimal→pinned fast path
  multi_period.hpp     [WIRE]  same dispatch in the schedule walk
src/risk/              [NEW dir, S8.8]  factor_model.cpp (pimpl) + qp_solver.cpp + kkt_ldl.cpp + exposures.cpp + ...
tests/src/             risk_qp_augment_test.cpp, risk_kkt_ldl_test.cpp, risk_cone_test.cpp, risk_elasticity_test.cpp,
                       risk_garleanu_pedersen_test.cpp, risk_optimizer_perf_test.cpp, + extended existing risk_*_test
bench/                 optimizer_scale_bench.cpp  (M × K sweep; the perf gate)
```

---

## §3 — Per-unit breakdown

> Sprint exceeds ~7 units ⇒ **split S8-a / S8-b** (`sprint.md` discipline). S8-a is the solver-core perf rebuild
> (the boundary pins are the gate); S8-b is the constraint/cone/multi-period reach + the header/source split. Dispatch
> **sequential** within each sub-sprint (S8.2 consumes S8.1's augmented form; S8.3 consumes S8.2's factorization;
> S8.5/S8.6/S8.7 consume S8.4's extended constraint set). Every sub-agent brief ends with the verbatim marker-commit
> reminder and the [`../docs/implementation-quality.md`](../docs/implementation-quality.md) handoff block.

### S8.0 — Marker + ledger + as-built recon + baseline capture (Effort S)
- [ ] Open `sprint-8-progress.md` (the ledger skeleton from [`../docs/sprint.md`](../docs/sprint.md)); marker commit
      `docs(p2-s8-0): open phase-8 optimizer sprint ledger`; record `Base: main @ <SHA>`.
- [ ] Confirm the `feat/p2-s1-multi-horizon` merge SHA and **re-verify every file:line in §0** against the merged
      headers; amend §0 in the ledger's "Plan adjustments" if any signature drifted.
- [ ] Lock the eight regression pins (§0.4) as a checklist the sub-agents inherit.
- [ ] **Capture the baseline:** add `bench/optimizer_scale_bench.cpp` (M ∈ {500, 1500, 3000, 5000}, K ∈ {16, 64}); run
      it on the as-built solver and record the **~20 s/run** number + per-M timings in the ledger. This is the number
      S8.3's perf gate is measured against. Re-enable the 3 `DISABLED_MultiHorizonIntegration` tests **renamed but still
      skipped** (so they compile) — they go green at S8.3.
- **Acceptance:** ledger open, §0 verified, baseline numbers recorded, pin checklist written.

### S8-a — Solver core: kill the O(M²), commercial-grade perf

#### S8.1 — Factor-augmented sparse QP reformulation (Effort L)
- [ ] Add `risk/qp_augment.hpp`: `build_augmented(const FactorModel& V, f64 λ, span<const f64> q, const
      MaterializedConstraints& C) -> AugmentedQp{Eigen::SparseMatrix<f64> P, Ã; VecX q_aug, l, u; usize n_w, n_y, n_aux}`.
      Introduce `y = Xᵀw` (K vars) with equality rows `y − Xᵀw = 0`; set `P = blkdiag(D, F)` (D from `specific_var()`
      floored ≥ d_min, F from the `FactorModel` factor cov scaled by 2λ); keep the existing gross/turnover L1 splits as
      structured sparse rows (not dense). Fixed CSC traversal order for every assembly (R1/Track E).
- [ ] Rewrite `ConstrainedQpSolver` to consume the augmented sparse form internally — **public `solve(const QpProblem&)`
      signature unchanged** (`qp_solver.hpp:98`). The ADMM x-update becomes a sparse KKT solve (S8.2) instead of the dense
      PCG; the box projection / over-relaxation / dual update are unchanged.
- [ ] **Differential gate (R11):** `risk_qp_augment_test.cpp` — on a battery of random feasible problems (varied M, K,
      constraint mixes) the augmented solver's returned book matches the **as-built dense-Ã solver** within a documented
      tolerance (target bit-identical on the minimal set; ULP-bounded ≤1e-10 on augmented sets). Keep the as-built solver
      compiled under a `_reference` name as the oracle for this test.
- **Acceptance:** augmented assembly is correct (P=blkdiag(D,F), y=Xᵀw rows), the QP matches the reference solver to
  tolerance, no dense Ã anywhere (grep the new path), all existing `risk_qp_solver_test` + `risk_multi_horizon_test`
  green.

#### S8.2 — Deterministic static quasi-definite LDLᵀ (Effort L)
- [ ] Add `risk/kkt_ldl.hpp`: `class QuasiDefiniteLdl` with `factor_symbolic(const SparseMatrix& K) ` (AMD ordering once,
      elimination tree, allocation plan — cacheable across rebalances) and `factor_numeric(...)` / `solve(span rhs, span
      x)` (no-pivot, division-free, fixed-order). Implements the QDLDL-style algorithm (reimplemented, not linked).
- [ ] Wire it as the ADMM x-update in `ConstrainedQpSolver`: assemble `[[P+σI, Ãᵀ],[Ã, −ρ⁻¹I]]` (quasi-definite by
      R5), factor **once per solve**, reuse the factor every outer iteration; cache the **symbolic** factorization on the
      solver instance keyed by the constraint sparsity pattern (rebuild numeric only when P/Ã values change). Record the
      **atx-core L7 `ldl_quasidef`** request; this header is the engine-side fallback.
- [ ] **Differential gate (R11):** `risk_kkt_ldl_test.cpp` — `L·D·Lᵀ == P_perm` reconstruction within ULP bound vs a
      dense reference factorization (Eigen `LDLT` on the densified KKT, test-only); solve accuracy `‖Kx−rhs‖` bounded;
      **determinism:** same input → byte-identical factor across two runs and across thread counts (golden-bit-hash).
- **Acceptance:** LDLᵀ reconstructs the KKT to tolerance, the cached symbolic path produces identical results to a cold
  factor, no numerical pivoting (the pivot order is the fixed AMD permutation), determinism hash stable.

#### S8.3 — ADMM rebuild: Ruiz + polish + certificates + warm-start; perf gate (Effort L)
- [ ] Add **Ruiz equilibration** (fixed 10 passes, symmetry-preserving) on `[[P,Ãᵀ],[Ã,0]]` + cost-scaling; drop the
      ε-test (R1). Add **deterministic polishing** (active-set partition by dual sign + one reduced-KKT solve + fixed 3
      refinement passes, OSQP §4). Add **infeasibility certificates** from iterate differences (deterministic). Add
      **warm-start** plumbing on `QpProblem` (optional `span<const f64> x0, y0`), decoupled from termination (R6).
- [ ] Re-tune the fixed iteration budget for the augmented form (the dense-PCG `600×50` is gone); set a problem-scaled
      default in `QpConfig` and document it. Keep **no early-exit**.
- [ ] **Re-enable the 3 `DISABLED_MultiHorizonIntegration` R1/R2/R3 tests** at full size — they must pass within CI time.
- [ ] **Perf gate** (`bench/optimizer_scale_bench.cpp`): **≥ 20× speedup** vs the S8.0 baseline; **target < 500 ms/run**
      at M=3000, K=64 (record the actual number); scaling curve is **sub-O(M²)** (fit the exponent, must be ≤ ~1.3).
- [ ] **Boundary-pin gate (R10):** the full pin checklist from §0.4 green — especially the S7 boundary pin (MultiHorizon
      H=1 identity-decay minimal-set == MultiPeriod, byte-identical) and pins #1–#4.
- **Acceptance:** the 3 re-enabled tests + all `risk_*` tests green, perf gate met with recorded numbers, every pin
  byte-identical, determinism golden-hash stable across {1,2,4,8} threads.

### S8-b — Constraint surface, conic, multi-period, commercial parity, header/source split

#### S8.4 — Complete the box/linear constraint surface + wire the QP into the single-period drivers (Effort L)
- [ ] Add `risk/reference_data.hpp`: `CapacityRef{span<const f64> adv, shares_out, price; f64 nav, horizon_days}`.
- [ ] Extend `constraints.hpp`: `ParticipationCap{f64 adv_frac}` (`|w_i| ≤ ρ·H·ADV_i·price_i/NAV`), `OwnershipCap{f64
      shares_frac}` (`|w_i| ≤ κ·shares_out_i·price_i/NAV`), `SectorRiskBudget` (net-weight variant now; the SOC variant
      lands in S8.5). `ConstraintSet::materialize` folds participation/ownership into the **position box** (diagonal,
      free structurally) by taking the elementwise min with `PositionCap`. Add per-descriptor `priority`/`elastic` fields
      (consumed by S8.6).
- [ ] **Wire the QP into the single-period path (§0.5):** add the minimal-vs-augmented dispatch to
      `PortfolioOptimizer::solve` and `MultiPeriodOptimizer::run` — minimal set (`GrossNet [+ PositionCap]`) keeps the
      pinned projected/proximal fast path; any extra constraint routes through `ConstrainedQpSolver`. Pins #1–#4 + NaN +
      empty-w_prev re-proven on the wired path.
- [ ] **Tests:** `risk_constraints_test` extended — each new cap is satisfied at the returned book (per-constraint
      assertion); a `CapacityRef` fixture proves %ADV and %shares bind correctly and the **min** composition is right.
- **Acceptance:** %ADV + %shares + sector-net caps enforced and tested, single-period drivers reach the full constraint
  set, all pins still byte-identical.

#### S8.5 — Conic (SOCP) support: tracking-error, sector-risk, robust, √-impact (Effort L)
- [ ] Add `risk/cone.hpp`: SOC descriptors + **deterministic cone projections** (fixed-form projection onto the
      second-order cone, order-fixed). Constraints: `TrackingError{span<const f64> w_bench; f64 te_budget}` (`‖V^{1/2}(w−
      w_bench)‖₂ ≤ te`), `SectorRiskBudget` SOC variant (`‖V^{1/2}(mask_g∘w)‖₂ ≤ σ_g`, low-rank via `f_g=Xᵀ(mask_g∘w)`),
      and **robust** alpha/risk uncertainty (Goldfarb-Iyengar dimension-K cones). Extend the ADMM with a cone-projection
      step alongside the box projection (ADMM handles conic via the projection operator — no new factorization).
- [ ] Add the **√-impact cost surrogate** (PWL or quadratic, folded into P/q so the problem stays QP/SOCP — R9), priced
      from `book::CostInputs`.
- [ ] **Differential gate (R11):** `risk_cone_test.cpp` — each cone projection matches its closed form within ULP; a
      tracking-error-constrained solve has `‖V^{1/2}(w−w_bench)‖₂ ≤ te + tol`; the robust solution reduces to the nominal
      as the uncertainty radius → 0.
- **Acceptance:** tracking-error + sector-risk cone + robust constraints enforced and tested, √-impact in the objective,
  determinism hash stable, the box-only path unchanged (cone count 0 ⇒ identical to S8.3).

#### S8.6 — Constraint-hierarchy / infeasibility elasticity (Effort M)
- [ ] Add `risk/elasticity.hpp`: when the QP returns an infeasibility certificate, run the **minimize-violation**
      auxiliary solve under a **priority-ranked γ ladder** (relax the lowest-priority elastic constraint first via slack
      `+γ·(violation)₊`), returning the **closest feasible book** + a report of which constraints were relaxed and by how
      much. Hard (non-elastic) constraints never relax. Fixed ladder order (R1/R3).
- [ ] **Tests:** `risk_elasticity_test.cpp` — an over-constrained problem (e.g. tracking-error + factor-neutral +
      tight gross simultaneously infeasible) returns a feasible book with the documented relaxation order; a feasible
      problem is untouched (elasticity is a no-op when feasible — byte-identical to S8.5).
- **Acceptance:** infeasible problems return a closest-feasible book + relaxation report; feasible problems unchanged;
  the ladder order is deterministic.

#### S8.7 — Multi-period: Gârleanu-Pedersen Riccati cost-to-go (Effort L)
- [ ] Add `risk/garleanu_pedersen.hpp`: the closed-form aim `aim_t = A_xx⁻¹ A_xf f_t` and value-function matrix `A_xx`
      (algebraic-Riccati solution for quadratic cost + mean-reverting signals; dense K-or-M LA, fixed-order). Expose
      `gp_aim_and_value(...)` returning both.
- [ ] Extend `multi_horizon.hpp`: replace the horizon-**average** `gp_aim` (`:194`) with the GP aim, and append the
      quadratic **cost-to-go tail** `−½(w−aim)ᵀA_xx(w−aim)` to the single-period QP objective (folds into P/q — the MPC
      trick). Implement the **stacked MPC** path (`stacked_mpc=true`, currently `Err(NotImplemented)` at `:154`) as a real
      O(N·H) augmented QP (benched, not the default).
- [ ] **Oracle gate (R11):** `risk_garleanu_pedersen_test.cpp` — with constraints relaxed and quadratic cost, the
      constrained QP's book matches the **pure GP closed form** within tolerance; the unconstrained closed form is the
      fast path (no solver call) and is byte-stable.
- [ ] **Boundary-pin (R10/R8):** with one identity-decay horizon + full trade-rate the GP cost-to-go collapses and the
      result stays byte-identical to S8.3's single-period book (the §0.4 S7 pin).
- **Acceptance:** GP cost-to-go matches the closed-form oracle, stacked MPC path benches (no longer NotImplemented),
  the boundary pin holds, look-ahead/truncation-invariance proven (R2).

#### S8.8 — Header/source split (pimpl) + compile-time measurement + close (Effort M)
- [ ] **Measure first:** record clean-build wall-clock of the `risk_*` test TUs on the `ninja` (PCH-off) preset
      pre-split.
- [ ] Split header→`.cpp` in dependency order, **`FactorModel` first** (the include hub, §0.7): move method bodies +
      `FactorModelBuilder` estimation + `detail::` kernels to `src/risk/factor_model.cpp`; the header keeps declarations +
      POD config/result structs. Then `qp_solver.cpp`, `kkt_ldl.cpp`, `exposures.cpp`, `multi_horizon.cpp`,
      `garleanu_pedersen.cpp`. Add each to `add_library(atx-engine STATIC ...)` (`atx-engine/CMakeLists.txt:1`). Pimpl
      where private Eigen members should not leak into the header (FactorModel).
- [ ] **Verify no behavior change:** the full `risk_*` suite + the boundary pins + the determinism golden-hash are
      byte-identical pre/post split (the split is purely a compilation refactor — R10).
- [ ] **Measure after:** record the post-split clean-build wall-clock + per-TU parse-cost delta; report the win in the
      ledger.
- [ ] **Close ceremony** ([`../docs/sprint.md`](../docs/sprint.md) §"Sprint close"): lift residuals into ROADMAP, bump
      the S8/S9/S10 status table, write "What S8 proves / Next sprint priorities", write `sprint8.md` user reference,
      `--no-ff` merge.
- **Acceptance:** measurable compile-time reduction (record the number), zero behavior change (pins + hash + full suite
  green), close ceremony complete.

---

## §4 — Gates (the sprint is not done until every one is green)

| Gate | What it proves | Where |
|---|---|---|
| **G-PERF** | ≥ 20× speedup vs the S8.0 baseline; target < 500 ms/run at M=3000,K=64; scaling exponent ≤ ~1.3 (sub-O(M²)). | S8.3, `bench/optimizer_scale_bench.cpp` |
| **G-PIN** | All 8 regression pins (§0.4) byte-identical; the S7 boundary pin holds through every unit. | S8.1, S8.3, S8.4, S8.7 |
| **G-DET** | Golden-bit-hash of the returned book stable across runs and {1,2,4,8} threads; no RNG, no pivoting, fixed counts. | S8.2, S8.3, S8.5 |
| **G-DIFF** | LDLᵀ vs dense reference; augmented QP vs as-built dense-Ã QP; each cone projection vs closed form; GP vs Riccati oracle — all ULP-bounded. | S8.1, S8.2, S8.5, S8.7 |
| **G-CONSTRAINT** | Every constraint the optimizer claims (factor/beta/group/%ADV/%shares/sector-risk/tracking-error/turnover/leverage) is satisfied at the returned book, proven per-constraint. | S8.4, S8.5 |
| **G-ELASTIC** | Infeasible problems return a closest-feasible book + relaxation report; feasible problems untouched. | S8.6 |
| **G-DISABLED** | The 3 `DISABLED_MultiHorizonIntegration` tests run green at full size in CI budget. | S8.3 |
| **G-COMPILE** | Measured clean-build reduction on the PCH-off preset post header/source split; zero behavior change. | S8.8 |

---

## §5 — Risks & mitigations

- **Silently-wrong-but-fast solver.** The dominant risk. Mitigation: G-DIFF pins the augmented QP to the as-built
  dense-Ã solver (kept compiled as `_reference`), and G-PIN/G-CONSTRAINT prove the book is unchanged + every constraint
  holds. The rewrite is never trusted on speed alone.
- **Determinism regression in the LDLᵀ / cone projections.** Mitigation: R5 (no-pivot quasi-definite ⇒ fixed pivot
  order), G-DET golden-hash across threads, fixed-order reductions (Track E), `/fp:precise -ffp-contract=off`.
- **SOCP scope balloon (S8.5).** The biggest/riskiest unit. Mitigation: cones enter the *existing* ADMM via a projection
  operator (no new factorization); if S8.5 overruns, the box/linear QP (S8.1–S8.4) + GP (S8.7) still ship a complete,
  faster optimizer and the cones carry to a follow-up (the sector-risk net-weight variant in S8.4 is the fallback).
- **Header/source split breaks the unity build.** Mitigation: S8.8 is last, behind a full green suite; the split is
  verified byte-identical; the `dev` preset's unity build is for the *test* target only and the new `.cpp` go in the
  STATIC lib, not the unity batch.
- **atx-core L7 `ldl_quasidef` not landed.** Mitigation: `kkt_ldl.hpp` is the shippable engine-side fallback (ROADMAP
  rule #7 precedent); the lift only *accelerates*, never *blocks*.

---

## §6 — Recorded lifts (atx-core requests + deferred scope)

- **atx-core L7 `ldl_quasidef`** — deterministic no-pivot quasi-definite LDLᵀ + AMD symbolic (S8.2 fallback is the
  engine-side shim).
- **atx-core L7 `socp_project`** — deterministic second-order-cone projection (S8.5 fallback in `cone.hpp`).
- **Phase-2 IPM fallback** (Clarabel/PIQP-style proximal-regularized, native quadratic + SOCP) for 1e-8/cold solves on
  the same augmented QP — recorded, not built.
- **MIQP layer** (cardinality, round lots, min-holding, min-trade — branch-and-bound on the convex core) — recorded,
  out of S8 scope (Barra "optimal" vs "post-opt rounding" modes).
- **Alpha-risk alignment (AAF), tax-aware, multi-account** — later commercial differentiators.
- **Full stacked MPO QP as default** — S8.7 ships it behind a flag; making it the default needs the O(N·H) perf work.

---

## §7 — References

Stellato, Banjac, Goulart, Bemporad, Boyd. *OSQP: An Operator Splitting Solver for Quadratic Programs.* Math. Prog.
Computation 12(4):637-672, 2020 (arXiv:1711.08013). ·
Vanderbei. *Symmetric Quasidefinite Matrices.* SIAM J. Optim. 5(1):100-113, 1995. ·
Ruiz. *A scaling algorithm to equilibrate both rows and columns norms in matrices.* RAL-TR-2001-034, 2001. ·
Amestoy, Davis, Duff. *An Approximate Minimum Degree Ordering Algorithm.* SIAM J. Matrix Anal. Appl. 17(4), 1996;
Algorithm 837, ACM TOMS 30(3), 2004. ·
Gârleanu, Pedersen. *Dynamic Trading with Predictable Returns and Transaction Costs.* J. Finance 68(6):2309-2340, 2013. ·
Boyd, Busseti, Diamond, Kahn, Koh, Nystrup, Speth. *Multi-Period Trading via Convex Optimization.* Found. & Trends in
Optimization 3(1):1-76, 2017 (arXiv:1705.00109). ·
Almgren, Chriss. *Optimal Execution of Portfolio Transactions.* J. Risk 3(2):5-39, 2000; Almgren et al., Risk 18(7), 2005. ·
Goldfarb, Iyengar. *Robust Portfolio Selection Problems.* Math. of OR 28(1):1-38, 2003. ·
Goldfarb, Scheinberg. *Product-form Cholesky for dense columns.* Math. Prog., 2004/05. ·
Goulart, Chen. *Clarabel.* arXiv:2405.12762, 2024. · Schwan et al. *PIQP.* IEEE CDC 2023 (arXiv:2304.00290). ·
Ferreau et al. *qpOASES.* Math. Prog. Computation 6(4), 2014. · Beck, Teboulle. *FISTA.* SIAM J. Imaging Sci. 2(1), 2009. ·
Saxena, Stubbs. *Alpha Alignment Factor.* J. Risk, 2012. · Boyd et al. *Markowitz Portfolio Construction at Seventy.*
arXiv:2401.05080, 2024. ·
Benzi, Golub, Liesen. *Numerical solution of saddle point problems.* Acta Numerica 14, 2005. ·
Ahrens, Demmel, Nguyen. *Algorithms for Efficient Reproducible Floating Point Summation (ReproBLAS).* ACM TOMS
46(3):22, 2020. ·
OSS: OSQP/QDLDL (Apache-2.0), SCS/HiGHS (MIT), PIQP/ProxQP (BSD-2), Clarabel (Apache-2.0), qpOASES (LGPL), cvxportfolio
(GPL-3.0 — design pattern only), Mosek Portfolio Optimization Cookbook §5.

*Sprint discipline: [`../docs/sprint.md`](../docs/sprint.md). Code-quality gate:
[`../docs/implementation-quality.md`](../docs/implementation-quality.md) +
[`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Predecessor: p2 S1
[`sprint-1-multi-horizon-optimization-implementation-plan.md`](sprint-1-multi-horizon-optimization-implementation-plan.md).*
