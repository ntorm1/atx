# Sprint S1 (p2) — Constrained Multi-Horizon Portfolio Optimization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the
> FROZEN *how*; the **what** is the S1 section of [`ROADMAP.md`](ROADMAP.md#s1--constrained-multi-horizon-portfolio-optimization--proposed)
> (this module embeds the S1 spec in the ROADMAP — there is no separate `sprint-1-…​.md` spec file). On conflict,
> **§0 (this plan's as-built amendment) overrides** the ROADMAP sketch.

**Goal:** Generalize `p1` S7's *receding-horizon driver* (a chain of single-period proximal solves over a schedule)
into a **true constrained multi-horizon portfolio optimizer**: signals carry an explicit **decay horizon**; the
optimizer trades toward a **forward-looking aim portfolio** that blends horizon-decayed forecasts (Gârleanu-Pedersen);
a **full constraint algebra** (factor-exposure / sector / beta / gross-net / position / per-horizon turnover budget)
is enforced **exactly** by a **deterministic fixed-iteration QP/ADMM solver**; the optimizer **executes only the first
move** (MPC, look-ahead-safe) and trades `p1` S8's cleaned `V`. This is the `p2` spine — S2 (meta-book), S4 (execution),
S5/S6 (signals/data), and S8 (capstone) all route through it.

**Architecture:** Four new headers in the existing `risk/` layer (namespace `atx::engine::risk`), composed so the new
optimizer is a **strict generalization** of the as-built S7 `MultiPeriodOptimizer`, never a rewrite:
`risk/constraints.hpp` (the composable constraint algebra → materializes to the QP's `(A, l, u)`),
`risk/qp_solver.hpp` (the deterministic fixed-iteration ADMM that enforces them, with `P = 2λV` applied via the
existing `FactorModel` Woodbury path), `risk/horizon.hpp` (the signal-horizon taxonomy + the forward forecast
trajectory), and `risk/multi_horizon.hpp` (the `MultiHorizonOptimizer` — builds the Gârleanu-Pedersen aim portfolio
from the trajectory, solves the constrained QP toward it, executes the first move). The **boundary pin** is the
contract that holds the whole sprint honest: with **one horizon, the minimal constraint set, and full trade-rate**,
`MultiHorizonOptimizer` must reduce **bit-for-bit** to S7's chained `PortfolioOptimizer::solve` — the regression
anchor against the proven layer.

**Tech Stack:** C++20, header-only inline (`#pragma once`), namespace `atx::engine::risk`. Reuses
`risk::{PortfolioOptimizer, OptimizerConfig, FactorModel, FactorModelBuilder, FactorComponents, ExposureMatrix,
build_exposures, MultiPeriodOptimizer, RebalanceSchedule, MultiPeriodConfig, MultiPeriodResult}`,
`book::CostInputs`, `combine::{CombinedSignalSource}`, `cost::{cost_aware_knobs, CostKnobs}`, `eval::*` (no new Sharpe),
`atx::core::{linalg (MatX/VecX/ols/symmetric_eig), simd::dot, Result, Status}`. GoogleTest
(`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake edit). clang-cl `/W4 /permissive- /WX` **+ strict
FP** (`/fp:precise`, `-ffp-contract=off`). Build + ctest are the gates; clang-tidy disabled (noise).

---

## §0 — As-built reconciliation amendment (the recon fixes)

> **Recon target (kickoff):** the merged `p1` S1–S8 engine. This sprint is cut from `main` after `p1` S7/S8 merge
> (the user's directive: S7/S8 assumed complete). Reconnaissance against the as-built risk layer surfaces the
> load-bearing corrections below; each changes a unit's scope. **Run the recon as the first act of S1-0** and amend
> these notes against the *actual* merged SHAs before dispatching S1.1.

### 0.1 The S7 `MultiPeriodOptimizer` is multi-*period*, not multi-*horizon* — S1 upgrades the inner objective, keeps the schedule walk
The as-built `risk::MultiPeriodOptimizer::run(sched, alpha_at, model_at, CostInputs)` (`risk/multi_period.hpp`) walks an
ascending `RebalanceSchedule`, threads `w_prev` from the prior realized book, calls `PortfolioOptimizer::solve(a, V,
w_prev)` with the **current-period α only**, applies a scalar Gârleanu-Pedersen `trade_rate` (`blend_toward`), and
executes the first move. It is *multi-period* (turnover measured across the real schedule) but **not multi-horizon**:
the inner objective is single-period and sees no forward trajectory. **Decision:** S1 introduces
`risk::MultiHorizonOptimizer` that **reuses the S7 schedule-walk skeleton verbatim** (same `RebalanceSchedule`,
`w_prev` threading, first-move execution, cost accounting) and **replaces only the inner solve**: instead of one
`solve` on the current α, it (a) builds a forward forecast trajectory (S1.3), (b) collapses it to a Gârleanu-Pedersen
**aim portfolio** (S1.4), and (c) solves a **constrained QP toward the aim** (S1.2) under the **full constraint
algebra** (S1.1). S7's `MultiPeriodOptimizer` stays in place, untouched, as the boundary-pin oracle (§0.5).

### 0.2 `PortfolioOptimizer::solve` enforces only `{Σw=0, Σ|w|≤L, |w_i|≤cap}` — the richer constraints need a real QP
The as-built single-period solver (`risk/optimizer.hpp`) runs a **fixed-iteration (`max_iters=64`) projected/proximal
loop**: gradient step toward the smooth target, L1-prox soft-threshold toward `w_prev` (the κ turnover term), then
projection onto `{Σw=0, Σ|w|≤L, |w_i|≤cap}`. There is **no** mechanism for **factor-exposure constraints**
(`|Xᵀw| ≤ b`), **sector/group caps**, **beta neutrality**, or **per-horizon turnover budgets**. **Decision:** S1.2
builds `risk::ConstrainedQpSolver` — a deterministic **fixed-iteration ADMM** (OSQP form `min ½wᵀPw + qᵀw s.t.
l ≤ Aw ≤ u`, fixed iteration count, **no residual early-exit**) — and S1.1 builds the `ConstraintSet` that materializes
`(A, l, u)` from composable descriptors. The as-built projected/proximal loop is **kept** as the engine-side fallback
*and* as the minimal-constraint code path the boundary pin (§0.5) exercises. **`P = 2λV` is never densified** — its
products go through the existing `FactorModel::apply_inverse` / a factored `apply` (Woodbury, O(MK+K³)), exactly as the
as-built optimizer does.

### 0.3 `FactorModel` exposes `X` via the S7 `build_components` refactor — S1's factor-exposure constraints reuse it
S7-3 extracted `FactorModelBuilder::build_components(...) -> Result<FactorComponents{MatX X; MatX F; VecX D; usize
fit_end;}>` (the estimation pulled out of `build()`, which became a thin wrapper) so dead-alpha columns could augment
the model. **Consequence:** S1.1's `FactorExposure` constraint reads the **same `X`** (the M×K exposure matrix) to form
`|Xᵀw| ≤ b` — no new accessor, no re-estimation. If S8's cleaning changed `build_components`' return shape, S1-0 records
the delta. Beta neutrality is the special case where the constraint row is the market-beta column of `X` (or a supplied
β vector).

### 0.4 No wall-clock / no horizon type — horizon is a per-signal decay parameter, PIT keys on the as-of period index
(Inherited from S4/S7 §0.4.) The combine/risk layer carries no timestamp; time is a positional `usize` as-of period
index, and `RebalanceSchedule.periods` are ascending indices. There is **no signal-horizon type**. **Consequence:** S1
defines `risk::SignalHorizon{f64 halflife_periods; f64 ic_decay;}` (a signal's forecast decay in *period* units) and a
`HorizonForecast` that, at as-of period `t`, projects the current α forward as `α_{t+h} = α_t · decay(h)` (the
PIT-causal forward trajectory — reading it uses only `≤ t` data, R2). Multi-horizon = **multiple signal sources with
different `SignalHorizon`s**, each projected on its own decay; the trajectory is their horizon-weighted superposition.
"PIT" is operationalized exactly as S7: a decision at `t` reads only data with index `≤ t`.

### 0.5 The boundary pin is the load-bearing regression — `MultiHorizonOptimizer` must reduce to S7 bit-for-bit
**Decision:** the single most important test of S1 is the **degenerate-equivalence pin**: configure
`MultiHorizonOptimizer` with (a) **one** `SignalHorizon` whose decay is the identity (`halflife→∞`, so the trajectory
is constant `α_t`), (b) the **minimal constraint set** (`dollar_neutral + gross_leverage + name_cap` only — exactly
S7's feasible set), and (c) **full trade-rate** (aim == single-period Markowitz target). Under this configuration the
QP's feasible region equals `PortfolioOptimizer::solve`'s, the aim collapses to the single-period target, and
`MultiHorizonOptimizer.run(...)` must produce a book schedule **byte-identical** to `MultiPeriodOptimizer.run(...)`.
This pins the generalization to the proven layer and is mandatory for S1.2 (the solver matches the proximal loop on the
minimal set) **and** S1.5 (the full driver matches S7). If the ADMM cannot match the proximal loop bit-for-bit on the
minimal set, the boundary-pin test uses the **as-built proximal loop as the minimal-set code path** (dispatch on
"no extra constraints") and the ADMM owns only the augmented-constraint path — recorded as a §0 decision, not a silent
divergence.

### 0.6 The aim-portfolio collapse vs the full stacked MPC QP — S1 ships the collapse, records the stack as the lift
Boyd 2017's exact MPC stacks the horizon variables `(w_t,…,w_{t+H})` into one big QP and executes the first block —
O(N·H) variables. Gârleanu-Pedersen 2013 prove that for **quadratic** costs the optimal policy trades toward an **aim
portfolio** that is a horizon-decay-weighted blend of the per-horizon Markowitz portfolios — an **O(N)** collapse.
**Decision:** S1.4 ships the **GP aim-portfolio collapse** (default): build the aim from the trajectory, solve a single
**constrained** QP toward it (the constraints are GP's missing piece — GP's closed form is unconstrained). The full
stacked MPC QP is a `MultiHorizonConfig` flag (`stacked_mpc=false` default) and a recorded **lift** (it needs the QP at
O(N·H), benchable but not the production path at the 3–5k-name scale). This keeps the solver O(N) per rebalance and
deterministic, and matches S7's "execute the first move."

### 0.7 The forecast trajectory needs alpha *and* its decay — the combiner gives weights, not horizons
The as-built `combine::CombinedSignalSource` emits a per-date combined α but carries **no horizon metadata** (a
signal's IC-decay rate is not stored). **Consequence:** S1.3 takes the `SignalHorizon` as **caller-supplied config per
signal source** (estimated offline from the alpha's autocorrelation/IC-decay, or set by the sleeve — the S2 consumer
supplies it). For S1's own tests, horizons are fixture inputs. The trajectory builder consumes
`{(ISignalSource*, SignalHorizon)}` pairs and the current as-of period. Estimating `SignalHorizon` from realized IC is
a recorded **residual → S2** (the sleeve owns horizon assignment).

> **Net scope shift vs the ROADMAP sketch:** S1 is **additive and compositional** — the schedule walk (§0.1), the
> factored `V` apply (§0.2), and the `X` exposure matrix (§0.3) all already exist; S1 *upgrades the inner objective,
> adds the constraint algebra + the QP that enforces it, and adds the horizon trajectory + aim portfolio.* The two
> genuinely-new numeric paths are the **fixed-iteration constrained ADMM** (§0.2, S1.2) and the **multi-horizon aim
> portfolio** (§0.6, S1.4). The dominant correctness risk is a **silently-wrong optimizer** — one that loses
> determinism in the ADMM, leaks look-ahead through the forward trajectory, violates a constraint it claims to
> enforce, or fails to reduce to S7 on the boundary. Each is a named gate in §3.

---

## §1 — Research foundation: the multi-horizon optimizer design rules (with citations)

Derived from the research north-stars (`renaissance-technologies-systems-deep-dive.md` §5.1/§6.1, `worldquant-systems-
deep-dive.md` §6.1/§6.5, `backtest-loop-execution-sim-deep-dive.md` §0) and the carried-forward `p0`/`p1` invariants.
**Non-negotiable**; every S1 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **R1** | **Deterministic fixed-iteration solver — no convergence early-exit.** The ADMM runs a fixed iteration count; the KKT factorization, every projection/clip, the trajectory build, and the aim blend are order-fixed; same inputs ⇒ byte-identical book schedule + digest. | `p1` invariant #1; the as-built `optimizer.hpp` fixed-`max_iters` rule; ADMM fixed-iteration determinism [Boyd 2011; OSQP, Stellato et al. 2020]; FISTA fixed-K [Beck-Teboulle 2009]. |
| **R2** | **No look-ahead / receding-horizon PIT.** The objective sums over a forward horizon but **executes only the first move** (MPC); the forward trajectory `α_{t+h}=α_t·decay(h)` is a causal function of `≤ t` data (a *projection*, not a peek); every fitted object is trailing-fit. Truncation-invariance is the test. | `p1` invariant #2/#4; Boyd et al. 2017 *Multi-Period Trading via Convex Optimization* (MPC look-ahead safety); Rawlings-Mayne-Diehl MPC 2017; the S7 receding-horizon contract. |
| **R3** | **Constraints are enforced exactly, not penalized softly.** Factor-exposure / sector / beta / gross-net / position / turnover-budget constraints become **hard** `l ≤ Aw ≤ u` rows in the QP — a constraint the optimizer claims to honor must be satisfied at the returned book (within solver tolerance), proven per-constraint. | Institutional mandate discipline; Boyd CVXPY/`cvxportfolio` constraint algebra; the `p1`-S8 factor model `V=XFXᵀ+D` whose `X` the exposure rows reuse. |
| **R4** | **The factored covariance is never densified.** `P = 2λV` products and the QP's `V`-dependent terms route through `FactorModel::apply_inverse` / a factored apply (Woodbury, O(MK+K³)); no dense M×M is ever formed — the S8 cleaning is consumed for free. | `p1` invariant #6 (no hot-path alloc at 3–5k names); the as-built `factor_model.hpp` Woodbury path; `combine-billion` O(N)-not-O(N²) lesson (WQ §6.2). |
| **R5** | **The aim portfolio is the GP forward blend; trade partway toward it.** For quadratic costs the optimal dynamic policy trades toward a horizon-decay-weighted blend of per-horizon Markowitz portfolios (the aim), not the myopic single-period target — this is the multi-horizon crux that a chained single-period driver cannot express. | Gârleanu & Pedersen 2013, *Dynamic Trading with Predictable Returns and Transaction Costs*; generalizes S7's scalar `trade_rate`; RenTech §5.1 (cost-throttled multi-horizon hold). |
| **R6** | **Calibrated, honest cost in the turnover budget.** The QP's turnover term / no-trade band uses the **S6-calibrated κ** (via `book::CostInputs`), not a default; per-horizon turnover budgets price fast vs slow signals' churn correctly. | `p1` invariant #5; RenTech §7.2 (cost = the strategy, ~2-day avg hold from cost-throttling); WQ §6.5 (turnover as cost, not return-killer). |
| **R7** | **Reduce to S7 on the boundary.** With one identity-decay horizon, the minimal constraint set, and full trade-rate, the new optimizer is **bit-identical** to S7's chained single-period book. The generalization is pinned to the proven layer. | `module.md` carry-forward discipline; the regression-anchor precedent (S7's "single-period == one solve" pin, S7 plan §4.1). |
| **R8** | **Differential correctness of every kernel.** The ADMM (vs a reference QP / KKT solve), the trajectory (vs a hand-rolled decay), the aim blend, and the constraint materialization each ship an obviously-correct reference + a bit-/ULP-bounded differential test. | `p1` invariant #7; the `p0`/`p1` differential-test precedent for every new compute kernel. |

**One-sentence thesis:** *S7 already proves the schedule walk, the factored-`V` apply, the calibrated-κ turnover term,
and the first-move execution are deterministic and look-ahead-safe — so S1 is the layer that (a) adds the forward
horizon trajectory + GP aim portfolio (R5), (b) adds the constraint algebra + the fixed-iteration ADMM that enforces it
exactly (R3/R1), and (c) is pinned to S7 on the boundary (R7); the only genuinely-new correctness risks are the ADMM's
determinism + constraint-satisfaction, the trajectory's look-ahead safety, and the bit-for-bit reduction to S7.*

---

## §1A — State-of-the-art research grounding (verified literature)

> Sourced via a fan-out web-research pass with 3-vote adversarial verification (2026-06-13), then a direct-fetch
> verification pass for the solver internals (Track B). **Every reference below is verified primary-source**
> (arXiv / DOI / publisher); full citations + links in the **References** section. Where a circulated claim failed
> verification it is flagged so the doc does not propagate it.

### Track A — Multi-horizon / multi-period portfolio optimization (the *what*)

**A1 — Boyd, Busseti, Diamond, Kahn, Koh, Nystrup & Speth (2017), *Multi-Period Trading via Convex Optimization* — the MPC backbone.** [grounds R1/R2, §4.4]
Each period's trade solves a convex problem trading off **expected return, risk, transaction cost, and holding cost**
(the canonical four-term *stage* objective). The multi-period method is **receding-horizon / model-predictive control
(MPC)**: plan a sequence of trades over a horizon using forecasts of future quantities, **execute only the first**,
then re-optimize. The single-period form traces to Markowitz; the multi-period form to MPC. The companion open-source
`cvxportfolio` is the authors' reference implementation, exposing exactly two optimizer policies —
`SinglePeriodOptimization` and `MultiPeriodOptimization` (the MPC signature) — each a maximized algebraic combination of
cost objects (`ReturnsForecast`, `TcostModel`, …) plus a constraint list. *This is the literal structure of S1's
`MultiHorizonOptimizer.run` (§4.4): a per-period constrained solve inside a schedule walk that executes the first
move; the four-term objective is the single-period stage objective embedded in the MPC loop.*

**A2 — Gârleanu & Pedersen (2013), *Dynamic Trading with Predictable Returns and Transaction Costs* — the aim portfolio.** [grounds R5, §4.4]
The canonical **closed-form** optimal dynamic policy under **quadratic** trading costs and signals with **different
mean-reversion speeds**. Two principles: **"aim in front of the target"** and **"trade partially toward the aim."** The
updated portfolio is a linear combination of the current portfolio and an **aim portfolio**, itself a weighted average
of the current Markowitz portfolio (the moving target) and the **expected future Markowitz portfolios** — the
forward-looking target S1.4 builds. The trade rate is generally a **matrix** (steady-state Riccati solution) that
collapses to a **scalar fraction `a/λ`** in the special case `Λ = λΣ`:
`x_t = (1 − a/λ)·x_{t−1} + (a/λ)·aim_t`. **Proposition 4 / Eq. (15)** gives the multi-horizon signal-blending rule: each
factor `f_t^k` is scaled by `1 / (1 + φ_k·a/γ)`, where `φ_k` is that factor's **alpha-decay rate** — **slower-decaying
signals get more weight** (trade more aggressively on persistent signals; their benefit accrues over longer periods);
the differential weighting exists **only** under transaction costs (zero cost ⇒ aim = Markowitz). *This is the exact
formula S1.4's `gp_aim` implements (§4.4).*

**A3 — Kolm & Ritter — the duality, the closed form, the many-asset decomposition.** [grounds R5/R7, §4.4]
(a) *Multiperiod Portfolio Selection and Bayesian Dynamic Models* (Risk, 2015): a **duality theorem** recasting
multiperiod mean-variance-minus-cost utility as **Bayesian MAP sequence estimation** — `log[p(y|x)·p(x)] = K·utility(x)`,
hidden states `x_t` = optimal holdings, observations `y_t = (γΣ_t)⁻¹·α_t` = the unconstrained Markowitz portfolios.
(b) Under **quadratic** cost the MAP path is **closed-form via the Kalman smoother / LQG**: the optimal trade is **linear
in the state**, `Δx_t = L_t·s_t`, `s_t = (f_t, x_t)`, `L_t` from a **Riccati** equation. Non-quadratic cost ⇒
non-Gaussian transition ⇒ particle-filter + Viterbi, *or* coordinate descent.
(c) **Many-asset decomposition:** with **separable** (additive-across-assets) cost `C_t = Σ_i C_t^i`, multiperiod
optimization over N assets reduces to a sequence of single-asset problems via **blockwise coordinate descent**
(Tseng 2001 — any limit point is a global minimizer under convexity; the one-at-a-time update is critical, an
all-at-once/Jacobi update may not converge). *Grounds S1's factored, per-name-separable structure and the LQG/Riccati
reading of the aim portfolio.*

**A4 — Brokmann, Itkin, Muhle-Karbe & Schmidt (2024/25), *Tackling Nonlinear Price Impact with Linear Strategies* — the LQR feedback rule + the nonlinear-cost shortcut.** [grounds R5/R6, §0.6, §4.4]
Re-derives G&P for the AR(1)-signal ergodic case as an explicit **linear feedback rule**
`q_t = K_f·f_t + K_q·q_{t−1}` (trade `Δq_t = K_f·f_t + (K_q − 1)·q_{t−1}`), with closed-form gains
`K_f = (α/2λ₂)·1/(1 − ρ + ξ)`, `K_q = 1/(1 + ξ)`, `ξ = (γσ²/4λ₂)·(1 + √(1 + 8λ₂/γσ²))`, depending on signal
persistence `ρ`, risk aversion `γ`, return variance `σ²`, and quadratic cost `λ₂`. **Key practical result:** for
realistic **nonlinear (power-law) impact**, keep the *linear* G&P/LQR rule and merely **tune a single effective
quadratic cost `λ`** — the linear strategy lands **within 2%** of the high-accuracy numerical optimum (Kolm-Ritter
Viterbi benchmark) across a wide risk range; the effective `λ` is found by maximizing an explicit scalar function (no
heavy numerics). *Justifies S1 shipping the GP aim-collapse (quadratic-cost closed form) as the production path and
treating S6's calibrated round-trip cost as the tunable effective `λ` — the full nonlinear stacked-MPC QP is the lift,
not the default (§0.6).*

**A5 — Ma & Smith (2025), alpha-decay specification.** Models multi-horizon signal decay as a finite-lag
cross-correlation `Y_t = ρ₀·X_t + ρ₁·X_{t−1} + … + ρ_{k−1}·X_{t−k+1} + ε_t` (current signal dominant, `ρ₀ ≫ ρ_j`).
*Grounds the discrete `SignalHorizon` decay (§4.3); a preprint / single-asset toy model — supporting, not
load-bearing.*
> **Refuted (do NOT cite as fact):** the same preprint's claim that the optimal policy is a **no-trade-band** strategy
> with a Taylor-approx boundary failed verification (1-2). S1's no-trade band stands on the **L1 / turnover-budget
> constraint** (§4.1) + the G&P partial-adjustment rate — **not** on a derived no-trade-zone theorem.

### Track B — The proprietary constrained QP solver (the *how*)

> S1.2 builds a **proprietary, in-house** constrained-QP core (anti-roadmap #6 keeps the dedicated kernel an atx-core
> `qp_admm` request; the engine fallback ships first). The design space is surveyed below from the verified primary
> literature; we choose **operator-splitting ADMM (OSQP-class)** for the reasons in **B5**.

**B1 — OSQP: Stellato, Banjac, Goulart, Bemporad & Boyd (2020) — the chosen algorithmic basis.** [grounds R1/R3, §4.2]
Solves `min ½·xᵀP x + qᵀx  s.t.  l ≤ A x ≤ u` by ADMM. Per iteration (verified verbatim from the OSQP docs):
- **(1) KKT solve** for `(x̃^{k+1}, ν^{k+1})` of the **quasi-definite** system
  `[[P + σI, Aᵀ], [A, −ρ⁻¹I]] · [x̃; ν] = [σ·x^k − q ; z^k − ρ⁻¹·y^k]`;
- **(2) projection** `z^{k+1} ← Π_{[l,u]}( z̃^{k+1} + ρ⁻¹·y^k )` (clip onto the box `[l,u]`);
- **(3) dual** `y^{k+1} ← y^k + ρ·( z̃^{k+1} − z^{k+1} )`;
with over-relaxation `α` and penalty `ρ`. The KKT coefficient matrix is **constant across iterations** (depends only on
`P, A, σ, ρ`) ⇒ **factor once** (LDLᵀ quasi-definite, or an indirect/CG solve), **cache, reuse** — only a `ρ` change
triggers refactorization. **Termination:** `‖r_prim‖_∞ ≤ ε_prim` and `‖r_dual‖_∞ ≤ ε_dual`, with `r_prim = A x − z`,
`r_dual = P x + q + Aᵀy`. **Primal-infeasibility certificate:** a `v` with `Aᵀv = 0` and `uᵀv₊ + lᵀv₋ < 0`.
**Dual-infeasibility certificate:** an `s` with `P s = 0`, `qᵀs < 0`, and `(A s)_i` consistent with each bound type.
Uses **Ruiz equilibration** preconditioning and is **warm-startable** (seed `x, y` from the prior solve). *This is the
exact skeleton of S1.2's `ConstrainedQpSolver` (§4.2).* — *Math. Program. Comput. 12(4):637–672; arXiv:1711.08013.*

**B2 — Factor-structure exploitation — the CORRECTED complexity.** [grounds R4, §4.2]
A factor covariance `V = X F Xᵀ + D` (k factors, k ≪ N, `D` diagonal ≻ 0) makes `P = 2λV`
**low-rank-plus-diagonal**. Two verified routes avoid a dense N×N:
- **Conic / variable-substitution (MOSEK Portfolio Cookbook):** introduce factor-exposure auxiliaries `b = Xᵀw` and
  split risk into a k-dim factor term + an N-dim diagonal specific term; the model matrix `G = [X·√F, √D]` carries
  **N·(k+1)** nonzeros vs a dense Cholesky's **N·(N+1)/2** — a **factor-of-N** storage + solve-time reduction
  (empirically orders of magnitude at N ≈ 16,000).
- **Woodbury / capacitance (the KKT route):**
  `(D + X F Xᵀ)⁻¹ = D⁻¹ − D⁻¹X·(F⁻¹ + Xᵀ D⁻¹ X)⁻¹·XᵀD⁻¹`; the only matrix inverted is the **k×k capacitance**
  `C = F⁻¹ + Xᵀ D⁻¹ X`. Cost is **O(N·k² + k³)** — **NOT O(k²)** (a circulated O(k²) claim was adversarially
  **refuted**; the corrected complexity is recorded here as the one S1.2 implements).
*S1.2 keeps `P`-applies matrix-free through the as-built `FactorModel::apply_inverse` (the Woodbury path, §0.2/R4) — the
KKT solve uses the indirect/CG variant so no dense N×N is ever formed.* — *Boyd, Johansson, Kahn, Schiele & Schmelzer
(2024) "Markowitz Portfolio Construction at Seventy", arXiv:2401.05080 (JPM); MOSEK Portfolio Cookbook §"Factor models".*

**B3 — The alternative algorithmic families (the survey — why not them).**
- **Active-set — qpOASES** (Ferreau, Kirches, Potschka, Bock, Diehl, 2014): a parametric **online active-set** method,
  excellent for **small/medium, warm-started sequences of QPs** (MPC), but active-set combinatorics scale poorly to the
  N = 3–5k-name regime with a large constraint count. — *Math. Program. Comput. 6:327–363.*
- **Interior-point — PIQP** (Schwan, Jiang, Kuhn, Jones, 2023): **proximal IPM + Proximal Method of Multipliers**;
  robust on ill-conditioned QPs without requiring LICQ; C++/Eigen, allocation-free, pivoting-free. Strong accuracy, but
  a **per-iteration factorization** and a **convergence-dependent (non-fixed) iteration count** — at odds with the
  determinism invariant (R1). — *arXiv:2304.00290; IEEE CDC 2023.*
- **Interior-point conic — Clarabel** (Goulart & Chen, 2024): homogeneous-embedding IPM with quadratic objectives
  (LP/QP/SOCP/SDP/exp/pow cones). General and accurate; same fixed-iteration tension as PIQP. — *arXiv:2405.12762.*
- **Operator-splitting conic — SCS** (O'Donoghue, Chu, Parikh, Boyd, 2016): ADMM on the **homogeneous self-dual
  embedding** with a matrix-free indirect-CG option — the conic cousin of OSQP. — *J. Optim. Theory Appl. 169(3):1042–1068; arXiv:1312.3039.*

**B4 — Numerical machinery (verified canon).**
- **Ruiz equilibration** (Ruiz, 2001): iterative row/column ∞-norm scaling to equilibrate the KKT data — OSQP's
  preconditioner; cheap, deterministic, conditioning-improving. — *RAL-TR-2001-034.*
- **Anderson acceleration** (Anderson 1965; Walker & Ni 2011): a fixed-point-iteration accelerator over a window of `m`
  prior residuals — an optional ADMM speedup, admissible **only** in a deterministic fixed-`m` variant (R1). —
  *J. ACM 12(4):547–560; SIAM J. Numer. Anal. 49(4):1715–1735.*

**B5 — The proprietary-solver design decision (synthesis).**
S1.2 builds an **OSQP-class operator-splitting ADMM** (B1) specialized to atx-engine — **not** an IPM (PIQP/Clarabel)
or active-set (qpOASES) — because:
1. **Determinism (R1):** ADMM runs a **fixed iteration count** with **no convergence early-exit**; the constant-KKT,
   factor-once structure makes a fixed-K run bit-reproducible. IPM and active-set iteration counts are data-dependent —
   a determinism hazard the backtest cannot accept.
2. **Factor structure (R4 / B2):** the constant `P = 2λV` is consumed **matrix-free** through the as-built
   `FactorModel` Woodbury/capacitance path — no dense N×N at 3–5k names; O(N·k² + k³) per solve.
3. **Warm-start (B1):** the receding-horizon walk re-solves a *slowly-varying* QP each rebalance; ADMM warm-starts from
   `w_prev`'s `(x, y)` for a large iteration saving — exactly the MPC regime OSQP/qpOASES target, and the structural
   reason a from-scratch IPM re-solve is wasteful here.
4. **Constraints as `l ≤ A x ≤ u` (R3):** the entire S1.1 algebra (factor-exposure, group, beta, gross-net, position,
   turnover) is **linear inequalities** — OSQP's native form; the L1 turnover / no-trade band is the standard
   auxiliary-variable split.

> **Boundary pin (R7):** on the minimal constraint set the ADMM fixed point must match the as-built proximal
> `PortfolioOptimizer::solve` (itself an ADMM-class projection loop); if exact bitwise parity across the two algorithms
> is infeasible, the minimal set **dispatches to the proven proximal loop** and the new ADMM owns only the
> augmented-constraint path (§0.5) — recorded, never silent.

---

## §2 — File structure

### 2.1 atx-core / Pattern-B requests (decided at kickoff)

> The engine adds no general-purpose primitive (project rule). S1 records the cross-module edge and ships on existing
> primitives, exactly as `p1` S1–S8 did:
>
> 1. **L7 `qp_admm` → atx-core.** A fixed-iteration constrained QP (OSQP-style ADMM with a pre-factorized KKT system).
>    Ship on an **engine-local fixed-iteration ADMM** (`risk::ConstrainedQpSolver`, S1.2) whose `P`-products route
>    through `FactorModel::apply_inverse`; the dedicated, KKT-pre-factorized `core::linalg::qp_admm` is the recorded
>    lift. Engine fallback is shippable now and bit-deterministic.
> 2. **`FactorModel` factored apply → already in `p1`.** Woodbury `apply_inverse` exists; no request.
> 3. **eRank / effective-breadth, calibrated κ, capacity ceiling → already in `p1` (S7/S6).** Reuse `book::CostInputs`
>    + `book::allocation`; no request.

### 2.2 Engine files (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/risk/constraints.hpp` | `LinearConstraint`, `FactorExposure`, `GroupCap`, `BetaNeutral`, `GrossNet`, `PositionCap`, `TurnoverBudget`; `ConstraintSet` → materializes `(A, l, u)` over S8's `X` (§4.1/§0.3/R3) | S1-1 |
| `include/atx/engine/risk/qp_solver.hpp` | `QpProblem` (`P`-as-`FactorModel` + `q` + `ConstraintSet`), `QpConfig` (fixed iters, ρ, σ), `ConstrainedQpSolver` — deterministic fixed-iteration ADMM, factored-`V` apply, no early-exit (§4.2/§0.2/R1/R3/R4) | S1-2 |
| `include/atx/engine/risk/horizon.hpp` | `SignalHorizon`, `HorizonForecast`, `forecast_trajectory(sources, horizons, as_of, H)` — PIT-causal forward `α_{t…t+H}` (§4.3/§0.4/§0.7/R2) | S1-3 |
| `include/atx/engine/risk/multi_horizon.hpp` | `MultiHorizonConfig`, `MultiHorizonResult`, `MultiHorizonOptimizer` — GP aim portfolio from the trajectory, constrained QP toward it, first-move execution; reuses `RebalanceSchedule` + the S7 walk (§4.4/§0.1/§0.5/§0.6/R5/R7) | S1-4 |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)
`risk_constraints_test.cpp` (S1-1), `risk_qp_solver_test.cpp` (S1-2), `risk_horizon_test.cpp` (S1-3),
`risk_multi_horizon_test.cpp` (S1-4 + the boundary-pin regression vs `MultiPeriodOptimizer`),
`risk_multi_horizon_integration_test.cpp` (S1-5, the determinism + look-ahead + all-constraints-satisfied capstone).
Bench: `bench/multi_horizon_bench.cpp` (S1-5: rebalances/sec across `(N universe, K factors, H horizon, |constraints|)`;
ADMM iteration cost; aim-collapse vs stacked-MPC).

### 2.4 Ledger
`sprint-1-progress.md` (S1-0), updated per unit (copy `p1`'s `sprint-7-progress.md` shape). S1 is **6 units incl.
marker** — within the 4–7 ceiling, **no split expected**; split S1-a (S1-0…S1-2: constraints + solver) / S1-b
(S1-3…S1-5: horizon + driver + integration) only if S1.2 (ADMM) or S1.4 (driver) over-run, exactly as `p1` S4/S7 did.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Suite_Condition_ExpectedResult`; cover happy path, **boundaries** (the S7
  reduction pin §0.5; a single-name book; an all-NaN α column → 0 weight; an infeasible constraint set → `Err`, not a
  silent clamp; an empty schedule; a one-horizon vs three-horizon book; a binding factor-exposure constraint vs a slack
  one; trade-rate 0 (no trade) vs 1 (full); zero turnover budget), and the **invariant proofs** (R1 two-builds-byte-
  identical, R2 truncation-invariance at the fit boundary, R3 every claimed constraint satisfied at the returned book,
  R7 the bit-for-bit S7 reduction).
- **Determinism (R1):** the ADMM runs a **fixed iteration count** (never a residual test); the KKT factorization,
  every clip/projection, the trajectory build, and the aim blend are order-fixed (ascending instrument/factor index); a
  **two-builds-equal** test (same inputs → byte-identical book schedule + digest) is mandatory for S1-2 and S1-5. No
  RNG in S1 (the optimizer is deterministic by construction; any future sampled path carries a recorded seed).
- **No look-ahead / PIT (R2):** the forward trajectory is a **projection** `α_{t+h}=α_t·decay(h)` (no future data
  read); the QP forecast, the factor model, and the calibrated cost are trailing-fit; the driver executes **only the
  first move**. **Truncation-invariance is the test** at every fitted boundary.
- **Constraint exactness (R3):** for every constraint type, a test asserts the returned book **satisfies** it within
  the solver tolerance (`|Xᵀw|≤b` holds; `Σw=0`; `Σ|w|≤L`; `|w_i|≤cap`; per-group cap; β-neutral); an **infeasible**
  set returns `Err(Infeasible)`, never a quietly-violated book.
- **Factored `V` (R4):** `P=2λV` products go through `FactorModel::apply_inverse` / a factored apply; **no dense M×M is
  ever allocated** — a test on a 3–5k-name model confirms the memory profile stays O(MK), not O(M²).
- **Calibrated honest cost (R6):** the turnover term's κ is the S6-calibrated value (`book::CostInputs.kappa`); never a
  raw default. Per-horizon turnover budgets are config, recorded.
- **No hot-path alloc (`p1` #6):** the rebalance loop reuses pre-sized scratch (the ADMM allocates its KKT factor once
  per solve; the driver pre-sizes the schedule-length book matrix); cold paths (factorization, trajectory build) may
  allocate (documented).
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures
  (dim mismatch, infeasible set, non-SPD `P`, empty schedule, unknown horizon); weakest sufficient types (`std::span`,
  `const&`, `std::string_view`); functions ≤ ~60 lines; **reuse `risk::{PortfolioOptimizer, FactorModel,
  MultiPeriodOptimizer, RebalanceSchedule}`, `book::CostInputs`, `cost::*`, `eval::*` — do NOT reinvent the schedule
  walk, the factored apply, the cost model, or the Sharpe**; the only new numeric kernels are the constraint
  materialization, the ADMM, the horizon trajectory, and the aim blend (the ADMM recorded as the Pattern-B lift).
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) **+ strict FP** (`/fp:precise`, `-ffp-contract=off`).
  clang-tidy disabled — the strict build + ctest are the gate.
- **clangd noise:** ignore squiggles; only a real `cmake --build` + ctest are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree (REUSE, do NOT rebuild):
risk/optimizer.hpp (PortfolioOptimizer::solve(alpha, FactorModel V, w_prev) — the ALREADY-deterministic fixed-iteration
max αᵀw−λwᵀVw−κ‖w−w_prev‖₁ projected/proximal solver, max_iters fixed, NaN α => 0 weight; this is your MINIMAL-CONSTRAINT
code path AND your boundary-pin oracle — you GENERALIZE it, you do NOT rewrite it); risk/multi_period.hpp
(MultiPeriodOptimizer::run(sched, alpha_at, model_at, CostInputs) — the receding-horizon SCHEDULE WALK you reuse
verbatim; you replace ONLY the inner solve with the multi-horizon QP); risk/factor_model.hpp (FactorModel V=XFXᵀ+D,
apply_inverse/risk/neutralize — the FACTORED apply; P=2λV products go THROUGH apply_inverse, you NEVER densify M×M;
build_components -> {X,F,D,fit_end} gives you the exposure matrix X for factor-exposure constraints); risk/exposures.hpp
(ExposureMatrix, build_exposures, FactorModelConfig); book/* (CostInputs{kappa, round_trip_cost_bps, capacity_gross} —
the S6-calibrated cost; allocation — capacity ceiling); eval/* (deflated_sharpe / compute_metrics — ONE convention,
reuse). atx-core linalg (MatX/VecX/ols/symmetric_eig/solve_spd) — fixed sign/order conventions for reproducibility.

THIS SPRINT'S DOMINANT RISK IS A SILENTLY-WRONG OPTIMIZER: an ADMM that loses determinism (residual early-exit) or
violates a constraint it claims to enforce; a forward trajectory that leaks look-ahead; a multi-horizon driver that
does NOT reduce to S7 on the boundary. The gates:
  - DETERMINISM (R1): the ADMM runs a FIXED iteration count, NEVER a residual test; KKT factor, clips, trajectory,
    aim blend all order-fixed. Two builds of the SAME inputs => BYTE-IDENTICAL book schedule + digest.
  - LOOK-AHEAD / RECEDING-HORIZON (R2): the forward trajectory is a PROJECTION α_{t+h}=α_t·decay(h) (NO future read);
    execute ONLY the first move. TRUNCATION-INVARIANT at every fitted boundary.
  - CONSTRAINT EXACTNESS (R3): every claimed constraint (|Xᵀw|≤b, Σw=0, Σ|w|≤L, |w_i|≤cap, group cap, β-neutral) is
    SATISFIED at the returned book within tolerance; an INFEASIBLE set returns Err(Infeasible), NOT a quietly-clamped book.
  - FACTORED V (R4): P=2λV goes through FactorModel::apply_inverse; NO dense M×M ever allocated (memory profile O(MK)).
  - GP AIM (R5): trade toward the horizon-decay-weighted forward blend, not the myopic single-period target.
  - REDUCE TO S7 (R7): one identity-decay horizon + minimal constraints + full trade-rate => book BYTE-IDENTICAL to
    MultiPeriodOptimizer.run. This is the load-bearing regression; if the ADMM cannot match the proximal loop bitwise on
    the minimal set, dispatch the minimal set to the proximal loop and let the ADMM own only the augmented path (record it).

PIT, NaN/delisted names get 0 weight and round-trip verbatim (no survivorship). Reuse ONE cost model (book::CostInputs),
ONE factored apply (FactorModel), ONE Sharpe (eval::). Header-only inline. Functions <= ~60 lines.
Build gate: cmake --build build --config Debug --target atx-engine-tests (/W4 /permissive- /WX + /fp:precise)
+ ctest -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER git add -A;
after committing run `git show HEAD --stat` (only your files); never touch atx-core/* or atx-tsdb/*; do not push.
End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 Constraint algebra → `(A, l, u)` materialization (S1-1)

The richer constraint set, as composable descriptors that materialize into the QP's linear-inequality form
`l ≤ A w ≤ u`. The factor-exposure rows reuse S8's `X` (§0.3); everything is order-fixed for determinism.

```cpp
// section: risk/constraints.hpp  (namespace atx::engine::risk)
// Each descriptor knows how to emit its rows of (A, l, u). M = universe size, K = #factors.
struct GrossNet     { atx::f64 gross_leverage; bool dollar_neutral; };           // Σ|w|≤L ; Σw=0
struct PositionCap  { atx::f64 name_cap; };                                      // |w_i| ≤ cap  (box rows)
struct FactorExposure {                                                          // |Xᵀw|_k ≤ b_k  for k in factors
  std::vector<atx::usize> factor_cols;   std::vector<atx::f64> bound;            // reuses FactorComponents.X
};
struct GroupCap     { std::span<const atx::usize> group_id; std::vector<atx::f64> cap; }; // |Σ_{i∈g} w_i| ≤ cap_g
struct BetaNeutral  { std::span<const atx::f64> beta; atx::f64 tol = 0.0; };     // |βᵀw| ≤ tol
struct TurnoverBudget { atx::f64 max_turnover; };                                // Σ|w_i − w_prev_i| ≤ T (per horizon)

struct ConstraintSet {
  GrossNet gross{};   std::optional<PositionCap> pos;   std::optional<FactorExposure> fexp;
  std::optional<GroupCap> grp;   std::optional<BetaNeutral> beta;   std::optional<TurnoverBudget> turn;

  // Materialize l ≤ A w ≤ u. `X` is FactorComponents.X (M×K); `w_prev` keys the turnover rows.
  // Order-fixed: gross/net, then box, then factor, then group, then beta, then turnover (R1).
  // Returns Err(Infeasible) only on contradictory bounds (e.g. name_cap·M < 1 with Σ|w|=1).
  [[nodiscard]] atx::core::Result<MaterializedConstraints>
  materialize(const atx::core::linalg::MatX& X, std::span<const atx::f64> w_prev, atx::usize M) const;
};

struct MaterializedConstraints {
  atx::core::linalg::MatX A;          // (R × M)  — kept SPARSE-friendly (most rows are 1-hot or X columns)
  atx::core::linalg::VecX l, u;       // R bounds; turnover/L1 rows use the standard auxiliary-variable split (§4.2)
};
```
**Determinism/exactness (R1/R3):** rows emitted in a fixed canonical order; `materialize` is pure; the test battery
proves each descriptor's row(s) encode the claimed inequality (a hand-built `w` on/over the boundary is accepted/
rejected by `A w ∈ [l,u]`). **`X` reuse (§0.3):** `FactorExposure`/`BetaNeutral` read columns of `FactorComponents.X`
directly — no re-estimation.

### 4.2 Deterministic fixed-iteration constrained ADMM (S1-2)

`min ½wᵀP w + qᵀw  s.t.  l ≤ A w ≤ u`, with `P = 2λV` applied through the `FactorModel` Woodbury path (never densified),
`q = −α_aim`, and the L1 turnover term handled by the standard auxiliary-variable split. **Fixed iteration count, no
residual early-exit** (R1) — the determinism crux.

```cpp
// section: risk/qp_solver.hpp  (namespace atx::engine::risk)
struct QpConfig {
  atx::usize iters = 200;     // FIXED — no convergence test (R1). Tuned for the 3–5k-name regime at S1-5.
  atx::f64   rho   = 1.0;     // ADMM penalty (constraint split)
  atx::f64   sigma = 1e-6;    // proximal regularization (KKT well-posedness)
};

struct QpProblem {
  const FactorModel& V;                 // P = 2λV, applied via V.apply_inverse / a factored apply (R4) — NEVER dense
  atx::f64           risk_aversion;     // λ
  std::span<const atx::f64> q;          // = −α_aim  (the linear term; aim from §4.4)
  const MaterializedConstraints& C;     // l ≤ A w ≤ u  (§4.1)
};

class ConstrainedQpSolver {
public:
  QpConfig cfg;
  // OSQP-form ADMM (Stellato 2020), fixed-iteration. KKT system [P+σI, Aᵀ; A, −1/ρ I] is factored ONCE per solve
  // (P+σI applied via the factored V — the indirect solve uses fixed-K PCG so no dense M×M is formed, R4).
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve(const QpProblem& p) const
  {
    // x = working weights; z = constraint image (Ax); y = dual. All zero-init (deterministic seed).
    VecX x = VecX::Zero(M), z = VecX::Zero(R), y = VecX::Zero(R);
    for (atx::usize k = 0; k < cfg.iters; ++k) {              // FIXED count — the determinism crux (R1)
      // (1) KKT step: solve (P+σI)x̃ = σx − q + Aᵀ(ρ z − y) ; P-apply through factored V (R4), fixed-K PCG.
      x = kkt_solve(p, /*rhs*/ sigma_times(x) - p.q + p.C.A.transpose()*(cfg.rho*z - y));
      // (2) z-update: clip A x + y/ρ onto [l, u] (the projection — box + linear rows, order-fixed).
      VecX Ax = p.C.A * x;
      z = clamp(Ax + y / cfg.rho, p.C.l, p.C.u);
      // (3) dual update.
      y += cfg.rho * (Ax - z);
    }
    // Feasibility check AFTER the fixed loop: if A x violates [l,u] beyond tol => Err(Infeasible) (R3, never a silent clamp).
    if (!feasible(p.C, x, kFeasTol)) return atx::core::Err(Status::Infeasible);
    return atx::core::Ok(to_vector(x));
  }
private:
  // P-apply: (P+σI)⁻¹ via the factored V (Woodbury) + σ-regularization, fixed-K PCG. NO dense M×M (R4).
  VecX kkt_solve(const QpProblem& p, const VecX& rhs) const;
};
```
**Boundary pin (R7, §0.5):** with the minimal `ConstraintSet` (gross/net + name-cap only), the ADMM's fixed point must
match `PortfolioOptimizer::solve` to tolerance; the test asserts this on a shared fixture. If exact bitwise match is
infeasible across the two algorithms, the **dispatch fallback** (§0.5) routes the minimal set to the as-built proximal
loop and the ADMM owns only the augmented-constraint path — recorded, not silent. **Differential (R8):** the ADMM is
checked against a small dense reference QP (direct KKT solve on a ≤50-name problem) within a bounded tolerance.

#### 4.2.1 — Solver data structures, factored-`V` KKT, scaling, warm-start, certificates (grounds §1A B1/B2/B4)

The solver is a workspace object built **once per problem shape** (the rebalance schedule re-solves a slowly-varying QP,
so the factorization + scaling are amortized across periods — the warm-start regime B1/B5).

```cpp
// section: risk/qp_solver.hpp  (namespace atx::engine::risk)
struct QpScaling {                       // Ruiz equilibration (B4): iterative row/col ∞-norm balancing of [P Aᵀ; A 0]
  atx::core::linalg::VecX D_scale;       // M diag scaling of variables    (deterministic, fixed sweep count)
  atx::core::linalg::VecX E_scale;       // R diag scaling of constraints
  atx::f64                c_cost = 1.0;  // objective scaling
};

struct QpWorkspace {                     // built ONCE per shape; reused across the schedule walk (warm-start, B1)
  const FactorModel*  V = nullptr;       // P = 2λV consumed MATRIX-FREE (R4) — never densified
  QpScaling           scal;              // Ruiz factors (cold-path; computed once)
  // The k×k capacitance C = F⁻¹ + Xᵀ(D+σ-shift)⁻¹X, factored ONCE (Woodbury, B2). KKT (P+σI)-applies route through it.
  atx::core::linalg::MatX C_chol;        // Cholesky of the k×k capacitance — the ONLY dense factorization (k≪N)
  VecX x, y, z;                          // primal / dual / aux — PERSIST across periods for warm-start
};
```

- **Factored KKT solve (B2, R4) — the O(N·k² + k³) path, NOT a dense N×N.** Each ADMM iteration's `(P+σI)`-apply uses
  `P + σI = 2λ(XFXᵀ + D) + σI`. The diagonal part `2λD + σI` inverts elementwise (O(N)); the low-rank correction goes
  through the **k×k capacitance** `C = F⁻¹ + Xᵀ(2λD+σI)⁻¹X·(2λ)` via Woodbury — `C` is Cholesky-factored **once** per
  shape (O(N·k² + k³)), then each apply is O(N·k). The dense `M×M` is **never** formed (R4); the memory profile is
  O(N·k), proven by the S1-5 bench. *(Corrects the refuted O(k²) claim — the capacitance form + factor is O(N·k²+k³).)*
- **Ruiz equilibration (B4).** A fixed-sweep-count (e.g. 10) ∞-norm row/column balancing of the KKT data, computed once
  on the scaled problem; deterministic (no convergence test), conditioning-improving. The unscaled solution + residuals
  + infeasibility certificates are recovered by un-applying `D_scale/E_scale/c_cost`.
- **Warm-start (B1/B5).** `x, y, z` persist on the workspace; period `s+1` seeds from period `s`'s solution. Because the
  schedule QP varies slowly, warm-started fixed-K ADMM converges in far fewer effective iterations than a cold solve —
  the structural reason ADMM beats a from-scratch IPM here (B5).
- **Infeasibility certificates (B1, R3).** After the fixed loop, if the scaled residuals expose a **primal-infeasibility
  certificate** (`δy` with `Aᵀδy ≈ 0`, `uᵀ(δy)₊ + lᵀ(δy)₋ < 0`) or a **dual-infeasibility certificate** (`δx` with
  `Pδx ≈ 0`, `qᵀδx < 0`, `(Aδx)` within bound types) the solver returns `Err(Infeasible)` / `Err(Unbounded)` — never a
  silently-clamped book (R3). A genuinely-infeasible constraint set (e.g. `name_cap·N < gross_leverage`) hits this path.
- **Determinism (R1).** Fixed `iters`; the KKT/Cholesky, the Ruiz sweeps, the projection clip, and every reduction are
  order-fixed; no residual early-exit. Two builds ⇒ byte-identical `x`. The certificate check is read-only (it does not
  branch the iteration count).

### 4.3 Multi-horizon forecast trajectory (S1-3)

A signal carries a **decay horizon**; at as-of period `t` the forward trajectory is the horizon-decayed *projection* of
the current forecast — causal, no future read (R2). Multi-horizon = a superposition over signal sources with different
horizons.

```cpp
// section: risk/horizon.hpp  (namespace atx::engine::risk)
struct SignalHorizon {                 // a signal's forecast decay, in PERIOD units (§0.4)
  atx::f64 halflife_periods;           // forecast halves every `halflife_periods` (∞ => identity, the S7-pin case §0.5)
  // decay(h) = 2^{−h / halflife_periods}  (the per-step forecast persistence; Grinold-Kahn IC-decay analogue)
};

struct HorizonForecast { std::vector<std::vector<atx::f64>> alpha;  // alpha[h][i] = forecast at horizon h, name i
                         atx::usize H; };

// Build the forward trajectory α_{t…t+H} from {(source, horizon)} at as-of period t. PIT-causal: each source emits
// its CURRENT cross-section α_t(source) (a function of ≤ t data), projected forward by its own decay. The trajectory
// is the horizon-weighted superposition. NO future data is read — this is a projection, not a peek (R2).
[[nodiscard]] atx::core::Result<HorizonForecast>
forecast_trajectory(std::span<const std::pair<const loop::ISignalSource*, SignalHorizon>> sources,
                    atx::usize as_of, atx::usize H)
{
  // For each h in [0,H], sum over sources: α[h][i] += decay_s(h) · α_t(source_s)[i]   (NaN => skip, weight 0; R-survivorship)
  // Order-fixed: ascending source, then ascending name. Returns Err on empty sources / dim mismatch.
}
```
**Look-ahead (R2):** `α_t(source)` reads only `≤ t`; the forward values are a deterministic decay of `α_t`, so
truncating the panel after `t` leaves the trajectory byte-identical (the truncation-invariance test). **Horizon source
(§0.7):** `SignalHorizon` is caller-supplied (the S2 sleeve assigns it; S1 tests use fixtures) — estimating it from
realized IC-decay is a recorded residual → S2.

### 4.4 Multi-horizon optimizer — GP aim portfolio + constrained QP + first-move execution (S1-4)

Reuse the S7 schedule walk; replace the inner solve. At each rebalance: build the trajectory (§4.3), collapse it to the
**Gârleanu-Pedersen aim portfolio** (the horizon-decay-weighted blend of per-horizon Markowitz targets), solve the
**constrained QP** (§4.2) toward the aim under the full constraint algebra (§4.1), execute only the first move (MPC).

```cpp
// section: risk/multi_horizon.hpp  (namespace atx::engine::risk)
struct MultiHorizonConfig {
  atx::f64        risk_aversion;        // λ
  ConstraintSet   constraints;          // the full algebra (§4.1) — minimal set reduces to S7 (R7)
  QpConfig        qp;                   // the ADMM knobs (§4.2)
  atx::usize      horizon = 1;          // H (lookahead periods); H=1 + identity decay => S7 boundary (§0.5)
  atx::f64        trade_rate = 1.0;     // GP partial-trade toward aim ∈ (0,1]; 1 => full aim (the S7 `trade_rate` analogue)
  bool            stacked_mpc = false;  // false => GP aim-collapse (O(N), default §0.6); true => full stacked MPC QP (the lift)
};

struct MultiHorizonResult {
  std::vector<std::vector<atx::f64>> books;     // one weight vector per schedule period (MPC first-move per rebalance)
  std::vector<atx::f64>              turnover;   // per-period Σ|book − w_prev|
  std::vector<atx::f64>              cost_bps;   // per-period calibrated cost on that turnover (R6)
};

class MultiHorizonOptimizer {
public:
  MultiHorizonConfig cfg;

  // sources_at(s) -> the {(ISignalSource, SignalHorizon)} active at schedule period s (trailing-fit, causal R2).
  // model_at(s) -> the S8-cleaned FactorModel at s. cost -> S6-calibrated CostInputs (R6). Reuses the S7 walk skeleton.
  [[nodiscard]] atx::core::Result<MultiHorizonResult>
  run(const RebalanceSchedule& sched,
      const std::function<HorizonSources(atx::usize s)>& sources_at,
      const std::function<const FactorModel&(atx::usize s)>& model_at,
      const book::CostInputs& cost) const
  {
    MultiHorizonResult out;
    std::vector<atx::f64> w_prev;                              // EMPTY at s=0 => flat
    ConstrainedQpSolver solver{cfg.qp};
    for (atx::usize s = 0; s < sched.periods.size(); ++s) {
      const FactorModel& V = model_at(sched.periods[s]);
      ATX_TRY(HorizonForecast traj, forecast_trajectory(sources_at(sched.periods[s]).pairs,
                                                        sched.periods[s], cfg.horizon));    // §4.3 (R2)
      // GP aim portfolio: horizon-decay-weighted blend of per-horizon Markowitz targets (Gârleanu-Pedersen 2013, R5).
      // aim = Σ_h ω_h · markowitz(traj.alpha[h], V, λ), ω_h ∝ persistence(h); aim is the FORWARD-looking ideal book.
      std::vector<atx::f64> aim = gp_aim(traj, V, cfg.risk_aversion);                        // O(N) collapse (§0.6)
      // Constrained QP toward the aim under the full algebra (R3); κ turnover from calibrated cost (R6).
      ATX_TRY(MaterializedConstraints C, cfg.constraints.with_turnover_kappa(cost.kappa)
                                              .materialize(components_X(V), w_prev, universe(V)));
      ATX_TRY(std::vector<atx::f64> target, solver.solve(QpProblem{V, cfg.risk_aversion, neg(aim), C}));
      // Partial trade toward target (GP trade-rate; 1 => full). At trade_rate=1 + H=1 + identity decay + minimal C,
      // this is BIT-IDENTICAL to MultiPeriodOptimizer (the boundary pin, R7/§0.5).
      std::vector<atx::f64> book = blend_toward(w_prev, target, cfg.trade_rate);             // order-fixed (R1)
      out.turnover.push_back(l1_diff(book, w_prev));
      out.cost_bps.push_back(out.turnover.back() * cost.round_trip_cost_bps);                // R6
      out.books.push_back(book);
      w_prev = std::move(book);                                // thread forward — the multi-period crux (reused from S7)
    }
    return atx::core::Ok(std::move(out));
  }
private:
  static std::vector<atx::f64> gp_aim(const HorizonForecast&, const FactorModel&, atx::f64 lambda);  // R5, factored V (R4)
};
```
**Reduction to S7 (R7/§0.5):** `horizon=1` + identity-decay `SignalHorizon` (trajectory == constant `α_t`) + minimal
`ConstraintSet` + `trade_rate=1` ⇒ `gp_aim` collapses to the single-period Markowitz target, the QP's feasible region
equals `PortfolioOptimizer::solve`'s, and `run(...)` is byte-identical to `MultiPeriodOptimizer.run(...)`. **The
mandatory regression test.** **Determinism (R1):** the schedule is ascending, `gp_aim`/`blend_toward`/`l1_diff` are
order-fixed, the QP is fixed-iteration. **Look-ahead (R2):** the trajectory is causal, the driver executes only the
first move.

#### 4.4.1 — The verified aim-portfolio closed forms (`gp_aim`; grounds §1A A2/A3/A4)

`gp_aim` is **not** ad-hoc — it is the Gârleanu-Pedersen closed form. Three verified, mutually-consistent constructions
(the implementation ships A2 as the kernel; A3/A4 are the cross-checks and the determinism reading):

- **A2 — the per-factor decay-weighted aim (the kernel, G&P Prop. 4 / Eq. 15).** With the current Markowitz target
  `m_t = (γV)⁻¹·α_t` decomposed over factors `α_t = Σ_k f_t^k`, the aim **down-weights each factor by its alpha decay**:
  ```
  aim_t = (γV)⁻¹ · Σ_k [ 1 / (1 + φ_k · a/γ) ] · f_t^k
  ```
  where `φ_k` = factor `k`'s mean-reversion (alpha-decay) rate and `a` is the trade-rate scale. A **persistent** factor
  (`φ_k` small) is scaled down **less** ⇒ gets **more** aim weight; a fast factor is discounted. With zero cost the
  scaling → 1 and `aim_t = m_t` (Markowitz). *This is exactly `SignalHorizon.halflife_periods → φ_k` (§4.3): the horizon
  taxonomy IS the per-factor `φ_k` the formula needs.*
- **the trade-rate (A2).** `book = w_prev + Λ_rate·(aim_t − w_prev)`; `Λ_rate` is generally a **matrix** (steady-state
  Riccati), collapsing to the **scalar** `a/λ` when `Λ = λΣ` — i.e. S1's `cfg.trade_rate` is the scalar special case,
  and the matrix form is a recorded refinement. The `(γV)⁻¹`-applies route through the factored `FactorModel` (R4), so
  `gp_aim` is O(N·k² + k³), never dense (§4.2.1).
- **A4 — the AR(1) LQR cross-check (Brokmann et al.).** For a single AR(1) signal the closed form is the linear feedback
  `q_t = K_f·f_t + K_q·q_{t−1}`, `K_f = (α/2λ₂)·1/(1−ρ+ξ)`, `K_q = 1/(1+ξ)`,
  `ξ = (γσ²/4λ₂)(1+√(1+8λ₂/γσ²))`. **S1's scalar-horizon unit test pins `gp_aim` + `trade_rate` against these exact
  gains** (the differential-correctness oracle, R8) — and confirms the practical result that under nonlinear impact the
  *linear* rule with an effective `λ` (= S6's calibrated round-trip cost) lands within ~2% of the numerical optimum,
  which is **why the GP aim-collapse is the production path and the stacked-MPC QP is the lift** (§0.6).
- **A3 — the LQG/Riccati reading (Kolm-Ritter).** Under quadratic cost the multi-horizon MAP path is the Kalman-smoother
  / LQG solution `Δx_t = L_t·s_t`, `s_t = (f_t, x_t)`, `L_t` from a Riccati equation — the same linear-in-state policy,
  which is why a **fixed-iteration** solve (no convergence search) is sound here, and why the many-asset problem is
  per-name **separable** (the factored `V` + diagonal `D` give exactly the additive structure BCD needs).

> **Net:** `gp_aim` implements the Eq. (15) decay-weighted blend (A2), is unit-tested against the Brokmann AR(1) gains
> (A4), and inherits its determinism/separability justification from the LQG reading (A3). The horizon `φ_k` comes from
> `SignalHorizon` (§4.3); the effective cost `λ` comes from S6 (`CostInputs`, R6).

### 4.5 Integration, determinism + look-ahead proofs, bench, close (S1-5)

- **Integration test** (`risk_multi_horizon_integration_test.cpp`): data → S8 cleaned `V` → multi-source trajectory →
  GP aim → constrained QP → first-move book over a multi-period schedule, asserting **all four gates simultaneously**:
  R1 (two builds byte-identical book schedule + digest), R2 (truncation-invariance at the fit boundary), R3 (every
  constraint satisfied at every period's book), R7 (the degenerate config reduces to S7 bit-for-bit).
- **Bench** (`multi_horizon_bench.cpp`): rebalances/sec across `(N∈{500,1000,3000,5000}, K factors, H∈{1,3,5},
  |constraints|)`; ADMM iteration cost; **aim-collapse vs stacked-MPC** wall-clock + book divergence; the memory
  profile proof (O(MK), not O(M²) — R4).
- **Close ceremony:** residuals → ROADMAP backlog (the stacked-MPC QP lift §0.6; horizon-from-IC estimation → S2 §0.7;
  the dedicated `core::linalg::qp_admm` lift §2.1; the dispatch-fallback decision if §0.5 triggered); ROADMAP status
  table `⏳ → ✅ <sha>`; `Last reviewed` bump; "What S1 proves" + "Next sprint priorities" baton; user reference
  `sprint1.md`; merge (`--no-ff`).

---

## Exit criteria

- The constraint algebra materializes factor-exposure / sector / beta / gross-net / position / turnover-budget into
  exact `l ≤ Aw ≤ u` rows; every claimed constraint is satisfied at the returned book; an infeasible set returns
  `Err`, not a silent clamp (R3).
- The fixed-iteration ADMM is **determinism-stable** (two builds byte-identical) and routes `P=2λV` through the
  factored `FactorModel` apply with **no dense M×M** at the 3–5k-name scale (R1/R4).
- The forward trajectory is a PIT-causal projection (truncation-invariant); multi-horizon = a superposition over
  signal sources with distinct `SignalHorizon`s (R2).
- The `MultiHorizonOptimizer` trades toward the Gârleanu-Pedersen aim portfolio under the full constraint algebra,
  executes only the first move, and prices the S6-calibrated κ (R5/R6).
- **The boundary pin holds:** one identity-decay horizon + minimal constraints + full trade-rate ⇒ the book schedule
  is **byte-identical** to `p1` S7's `MultiPeriodOptimizer` (R7).
- The integration test passes all four gates simultaneously; the bench reports rebalances/sec + the aim-collapse vs
  stacked-MPC trade-off + the O(MK) memory proof.
- `/W4 /permissive- /WX` + `/fp:precise` clean; one test file per unit; full engine suite stays green per unit.

## Invariants this sprint must prove

All seven carried-forward invariants (ROADMAP "Carried-forward invariants"), with three explicit stress points:
1. **Determinism (R1)** — the ADMM is the new fixed-iteration kernel; it must run a fixed count (no residual exit), and
   the full driver's two-builds digest must be byte-identical. No RNG in S1.
2. **No look-ahead (R2)** — the forward trajectory is a *projection* of `≤ t` data (not a peek); the driver executes
   only the first move; truncation-invariance at the fit boundary is the test.
3. **Reduction to S7 (R7)** — the generalization is pinned bit-for-bit to the proven single-period book on the
   boundary; without this, S1 is not demonstrably a superset of the layer it replaces.

## Dependencies

- **Upstream (`p1`, assumed merged):** S7 (`MultiPeriodOptimizer` + `RebalanceSchedule` + `PortfolioOptimizer::solve` +
  `FactorComponents.X` via `build_components`; `book::CostInputs`), S8 (the cleaned `FactorModel V` the QP trades), S6
  (calibrated κ), S1 (`eval::*`, reused verbatim).
- **atx-core (already available — reuse, no edge):** L7 `symmetric_eig`/`solve_spd`/`ols` (`decompose.hpp`/`spd.hpp`/
  `regression.hpp`), `MatX`/`VecX`, `simd::dot`.
- **atx-core (Pattern B — new edge raised by this sprint):**

| S1 unit | Needs from `atx-core` | Engine-side fallback (shippable now) |
|---|---|---|
| S1.2 | **L7 `qp_admm`** — fixed-iteration constrained QP (OSQP-style ADMM, pre-factorized KKT) | engine-local fixed-iteration ADMM (`ConstrainedQpSolver`) with `P`-apply through `FactorModel::apply_inverse` (fixed-K PCG) |

## Explicitly NOT in this sprint

- **No multi-strategy / meta-book** — S1 ships **one** sleeve's optimizer; combining sleeves into a fund is **S2**.
- **No live / streaming** — S1 is sim-side; the live spine is **S3**.
- **No optimal execution** — S1 emits target weights/turnover; child-order scheduling is **S4**. (S1 prices the
  calibrated *round-trip* cost via `CostInputs`, not an execution schedule.)
- **No deep-learning / alt-data signals** — S1 consumes whatever `ISignalSource`s it is handed (formulaic/ML today);
  NN signals are **S5**, alt-data is **S6**.
- **No full stacked-MPC QP as the production path** — the GP aim-collapse is the default (§0.6); the O(N·H) stacked QP
  is a benched config flag + a recorded lift.
- **No new general-purpose primitive in the engine** (anti-roadmap #6) — the QP solver is a recorded `core::linalg`
  request (Pattern B), engine-side fallback only until the L7 kernel lands.

## Baton → next

S1 hands **S2** a constrained multi-horizon optimizer that a sleeve wraps directly: S2's `Sleeve` is "a universe ×
horizon × signal-family book = one `MultiHorizonOptimizer` + its library subset," and S2's meta-allocator nets the
sleeve books into one fund under a cross-sleeve risk budget. S1 also hands **S4** the target/turnover series its
execution scheduler trades, and **S8** the optimizer the full-fund orchestrator routes every signal through. With S1
closed, the `p2` spine exists — every later sprint composes over it.

---

## References

> All primary sources, verified during the 2026-06-13 research pass (fan-out web search + 3-vote adversarial
> verification for Track A; direct-fetch verification for the Track B solver internals). Bibliographic anchors
> (DOI / arXiv / venue) confirmed against publisher + arXiv + authors' pages. **[LB]** = load-bearing (a unit's design
> rests on it); **[S]** = supporting; **[X]** = a circulated claim that **failed verification** — recorded so the doc
> does not propagate it.

### Track A — multi-horizon / multi-period optimization

1. **[LB]** S. Boyd, E. Busseti, S. Diamond, R. N. Kahn, K. Koh, P. Nystrup, J. Speth. *Multi-Period Trading via
   Convex Optimization.* Foundations and Trends in Optimization **3**(1):1–76, 2017. DOI: 10.1561/2400000023.
   arXiv:1705.00109. Companion library: **cvxportfolio** — <https://www.cvxportfolio.com/> ·
   <https://github.com/cvxgrp/cvxportfolio>. *(MPC/receding-horizon backbone; the per-period-solve-in-a-schedule-walk
   structure of §4.4.)*
2. **[LB]** N. Gârleanu, L. H. Pedersen. *Dynamic Trading with Predictable Returns and Transaction Costs.* Journal of
   Finance **68**(6):2309–2340, 2013. DOI: 10.1111/jofi.12080. NBER WP w15205 — <https://www.nber.org/papers/w15205>.
   *(The aim portfolio; Prop. 4 / Eq. 15 decay-weighted signal blend `1/(1+φ_k·a/γ)`; the §4.4.1 kernel.)*
3. **[LB]** P. N. Kolm, G. Ritter. *Multiperiod Portfolio Selection and Bayesian Dynamic Models.* Risk **28**(3):50–54,
   2015. SSRN: 2472768 — <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2472768>. *(MAP/HMM duality; Kalman-
   smoother/LQG closed form; the §4.4.1 A3 determinism + separability reading.)*
4. **[S]** P. N. Kolm, G. Ritter. *Dynamic Replication and Hedging: A Reinforcement Learning Approach.* Journal of
   Financial Data Science **1**(1):159–171, 2019. SSRN: 3281235. *(RL-as-trading-cost-aware-control grounding; single-
   option, framing only.)*
5. **[S]** P. N. Kolm, G. Ritter. *Modern Perspectives on Reinforcement Learning in Finance.* 2020. SSRN: 3449401.
   *(Frames transaction-cost-aware portfolio trading as a canonical intertemporal-choice problem.)*
6. **[S]** P. Tseng. *Convergence of a Block Coordinate Descent Method for Nondifferentiable Minimization.* Journal of
   Optimization Theory and Applications **109**:475–494, 2001. DOI: 10.1023/A:1017501703105. *(BCD limit-point
   guarantee — the many-asset decomposition of §4.4.1 A3.)*
7. **[LB]** X. Brokmann, D. Itkin, J. Muhle-Karbe, P. Schmidt. *Tackling Nonlinear Price Impact with Linear Strategies.*
   Mathematical Finance **35**(2):422–440, 2025. DOI: 10.1111/mafi.12449. SSRN: 4584448. LSE eprints: 125888.
   *(AR(1) LQR feedback gains — the §4.4.1 A4 differential-correctness oracle; the "linear rule + effective λ within 2%
   of optimum" result justifying the aim-collapse production path, §0.6.)*
8. **[S]** Y. Ma, A. Smith. *On the Effect of Alpha Decay and Transaction Costs on the Multi-period Optimal Trading
   Strategy.* arXiv:2502.04284, 2025 (preprint, single-asset). *(Finite-lag alpha-decay specification, §4.3.)*
   **[X]** its no-trade-band-strategy claim **failed verification (1-2)** — not relied on.

### Track B — proprietary constrained QP solver

9. **[LB]** B. Stellato, G. Banjac, P. Goulart, A. Bemporad, S. Boyd. *OSQP: An Operator Splitting Solver for Quadratic
   Programs.* Mathematical Programming Computation **12**(4):637–672, 2020. DOI: 10.1007/s12532-020-00179-2.
   arXiv:1711.08013. Docs: <https://osqp.org/>. *(The ADMM iteration, quasi-definite KKT, Ruiz scaling, warm-start,
   primal/dual infeasibility certificates — the §4.2 / §4.2.1 skeleton.)*
10. **[LB]** S. Boyd, K. Johansson, R. Kahn, P. Schiele, T. Schmelzer. *Markowitz Portfolio Construction at Seventy.*
    Journal of Portfolio Management, 2024. arXiv:2401.05080. SSRN: 4695694. *(Factor-structure exploitation in the
    portfolio QP — the corrected O(N·k²+k³) Woodbury/capacitance route, §4.2.1 / B2.)*
11. **[S]** R. Schwan, Y. Jiang, D. Kuhn, C. N. Jones. *PIQP: A Proximal Interior-Point Quadratic Programming Solver.*
    IEEE CDC 2023. arXiv:2304.00290. Code: <https://github.com/PREDICT-EPFL/piqp> (C++/Eigen). *(IPM+PMM alternative;
    surveyed + rejected for the non-fixed iteration count, B3/B5.)*
12. **[S]** P. J. Goulart, Y. Chen. *Clarabel: An Interior-Point Solver for Conic Programs with Quadratic Objectives.*
    2024. arXiv:2405.12762. Code: <https://github.com/oxfordcontrol/Clarabel.rs>. *(Interior-point conic alternative,
    B3.)*
13. **[S]** B. O'Donoghue, E. Chu, N. Parikh, S. Boyd. *Conic Optimization via Operator Splitting and Homogeneous
    Self-Dual Embedding.* Journal of Optimization Theory and Applications **169**(3):1042–1068, 2016. arXiv:1312.3039.
    Code (SCS): <https://github.com/cvxgrp/scs>. *(The conic operator-splitting cousin of OSQP; matrix-free indirect-CG
    option, B3.)*
14. **[S]** H. J. Ferreau, C. Kirches, A. Potschka, H. G. Bock, M. Diehl. *qpOASES: A Parametric Active-Set Algorithm
    for Quadratic Programming.* Mathematical Programming Computation **6**:327–363, 2014. DOI: 10.1007/s12532-014-0071-1.
    *(Online active-set alternative; surveyed + scope-bounded for large N, B3.)*
15. **[S]** D. Ruiz. *A Scaling Algorithm to Equilibrate Both Rows and Columns Norms in Matrices.* Rutherford Appleton
    Laboratory tech. report RAL-TR-2001-034, 2001. *(Ruiz equilibration preconditioner, §4.2.1 / B4.)*
16. **[S]** D. G. Anderson. *Iterative Procedures for Nonlinear Integral Equations.* Journal of the ACM **12**(4):547–560,
    1965. — H. F. Walker, P. Ni. *Anderson Acceleration for Fixed-Point Iterations.* SIAM Journal on Numerical Analysis
    **49**(4):1715–1735, 2011. *(Optional fixed-window ADMM accelerator, B4.)*
17. **[S]** MOSEK ApS. *Portfolio Optimization Cookbook — §"Factor models".*
    <https://docs.mosek.com/portfolio-cookbook/factormodels.html>. *(Conic factor-exposure reformulation `b = Xᵀw`;
    N·(k+1) vs N·(N+1)/2 nonzeros — the factor-of-N reduction, §4.2.1 / B2.)*

> **Verification caveats carried from the research pass:** (a) the Kolm-Ritter 2015 internals were verified against the
> authors' companion materials + SSRN, not the paywalled Risk article body; (b) the BCD global-minimizer guarantee
> (Tseng) holds under the convexity assumption Kolm-Ritter add; (c) Brokmann et al. is recent frontier (2024 accepted /
> 2025 published), not settled canon; (d) the O(k²) factor-KKT complexity was **refuted** — the implemented complexity
> is **O(N·k² + k³)** (§4.2.1 / B2). The Track B solver references (9–17) were verified in a **second, direct-fetch
> pass** after the first workflow surfaced the sources without verifying their internals.
