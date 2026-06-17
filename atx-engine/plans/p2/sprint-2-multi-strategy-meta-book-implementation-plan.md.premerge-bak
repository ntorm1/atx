# Sprint S2 (p2) — Multi-Strategy Meta-Book & Risk Budgeting — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the
> FROZEN *how*; the **what** is the S2 section of [`ROADMAP.md`](ROADMAP.md#s2--multi-strategy-meta-book--risk-budgeting--proposed)
> (this module embeds the S2 spec in the ROADMAP — there is no separate `sprint-2-…​.md` spec file). On conflict,
> **§0 (this plan's as-built amendment) overrides** the ROADMAP sketch.

**Goal:** Generalize `p1` S7's *single combined book* and `p2` S1's *single-sleeve `MultiHorizonOptimizer`* into a
**multi-strategy meta-book**: each **sleeve** (universe × signal-horizon × signal-family) wraps its own S1 optimizer over
its own library subset and emits a per-period target book; a **meta-allocator** sets cross-sleeve **capital/risk weights**
`c_s` under a **risk budget** (ERC / HRP / inverse-vol) and a **portfolio-of-books fractional-Kelly** fund leverage; the
sleeve books are **netted into one fund book** `W = Σ_s c_s·w_s` so offsetting child trades **cross internally** (the cost
saving *measured in-sim*, never invented); a **cross-sleeve risk model** aggregates factor exposure and component risk
over the **shared** `FactorModel`; and a **fund-level report** carries **attribution-by-sleeve** (Euler-exact). The
load-bearing guarantee: with **one sleeve, full capital, no crossing**, the meta-book reduces **bit-for-bit** to that
sleeve's S1 book — the regression anchor against the proven layer. This is the `p2` fund-scale lens: S5/S6 hang new
signal families off sleeves, S8 routes the whole library through the meta-book.

**Architecture:** A new `fund/` layer (namespace `atx::engine::fund`) — the meta-book is a **strict composition over** the
as-built single-book primitives, never a rewrite. `fund/sleeve.hpp` (a `Sleeve` = membership id-list + tags + a wrapped
`risk::MultiHorizonOptimizer`, producing the sleeve book series), `fund/meta_allocator.hpp` (the risk-budget +
portfolio-Kelly capital-weight solve → `c_s`), `fund/cross_sleeve_risk.hpp` (shared-`V` aggregate exposure, fund risk
split, sleeve-return covariance + component risk), `fund/netting.hpp` (the internal-crossing net book + gross/net
turnover + priced crossing benefit), and `fund/meta_book.hpp` (the `MetaBook` driver — runs sleeves, allocates trailing,
nets, executes the fund first move; plus the `FundReport` + attribution-by-sleeve). The **boundary pin** holds the sprint
honest: one sleeve + `c=1` + no vol-target ⇒ the fund book schedule is **byte-identical** to `MultiHorizonOptimizer.run`.

**Tech Stack:** C++20, header-only inline (`#pragma once`), namespace `atx::engine::fund`. Reuses
`risk::{MultiHorizonOptimizer, MultiHorizonConfig, MultiHorizonResult, HorizonSources, SignalHorizon, FactorModel,
FactorComponents, RebalanceSchedule}`, `book::{CostInputs, AllocationConfig, size_book, effective_breadth}`,
`combine::{compute_metrics, AlphaMetrics, CombinedSignalSource, AlphaCombiner, pairwise_complete_corr}`,
`library::{Library, AlphaId, LifecycleState}`, `eval::{compute_return_metrics, deflated_sharpe}`,
`atx::core::{linalg (MatX/VecX), Result, Status, ErrorCode}`. GoogleTest (`atx-engine/tests/*_test.cpp`,
CONFIGURE_DEPENDS — no per-unit CMake edit). clang-cl `/W4 /permissive-` **+ `/WX`** (`ATX_WERROR=ON`, default). **There
is NO `/fp:precise` flag in the tree (§0.10)** — determinism rests on **order-fixed reductions**, not a compiler FP mode.
Build + ctest are the gates; clang-tidy disabled (noise).

---

## §0 — As-built reconciliation amendment (the recon fixes)

> **Recon target (kickoff):** the merged `p1` S1–S8 engine **+ `p2` S1** (the `MultiHorizonOptimizer`, merged to `main`
> @ `13dbacf`). This sprint is cut from `main` after S1 merge. Reconnaissance against the as-built `risk/` + `book/` +
> `combine/` + `library/` layers surfaces the load-bearing corrections below; each changes a unit's scope. **Run the
> recon as the first act of S2-0** and amend these notes against the *actual* merged SHAs before dispatching S2.1.

### 0.1 There is NO `Strategy`/`Sleeve`/`MetaBook` type — S2 builds the whole `fund/` layer greenfield
A tree-wide grep for `Sleeve`, `class Strategy`, `MetaBook`, `RiskBudget` returns **zero** matches outside plan markdown.
The closest existing abstractions are `ISignalSource` (the loop seam, namespace `atx::engine` — **not** `atx::engine::loop`)
and `book::BookPipeline` (the **single-fund** orchestrator that drives one combined mega-alpha through *one*
`MultiPeriodOptimizer`). **Decision:** S2 introduces a new `fund/` layer (namespace `atx::engine::fund`) — `Sleeve`,
`MetaAllocator`, `CrossSleeveRisk`, `Netting`, `MetaBook` — composing over the single-book primitives. Nothing existing is
rewritten; `BookPipeline` stays the single-fund path and is the **structural precedent** the meta-book generalizes.

### 0.2 `library::Library` exposes NO subset/filter — the sleeve owns its own membership + tags
`Library` addresses alphas only by dense `AlphaId` (`0..n_alphas()`); the only selectable axis is **lifecycle state**, via
an O(n) `state_as_of(id, t)` scan (the pattern `BookPipeline` uses). There is **no by-universe / by-family / by-tag**
query, and **no universe/family metadata is stored** per alpha (`AlphaRecordView`/`Provenance` is the only per-alpha
metadata). **Consequence for S2.1:** a sleeve's "library subset over universe × horizon × signal-family" **cannot** come
from `Library` — the `Sleeve` carries its **own** `std::vector<library::AlphaId> members` + a `SleeveTag{universe, family}`
and its own `SignalHorizon` assignment. The sleeve mega-alpha is a `combine::CombinedSignalSource` over its members (the
natural per-sleeve wrapper, non-owning constituents). No `Library` API is added (membership lives in the sleeve, not the
shared store). Estimating membership from a clustering of the library is a recorded **residual → S4/S8**.

### 0.3 `book::allocation` is scalar / single-book — the meta-allocator is a NEW portfolio-of-books layer
The as-built `book::size_book(sr_book, sigma_book, capacity_gross, AllocationConfig{fractional_kelly=0.5, max_gross=4.0})`
is **scalar**: one book Sharpe, one book vol, one capacity ceiling → one capacity-bounded fractional-Kelly gross
`L = clip(c·SR/σ, 0, min(capacity_gross, max_gross))`. `effective_breadth(cov_eigenvalues)` takes a `VecX` of eigenvalues.
There is **no portfolio-of-books / vector fractional-Kelly and no cross-book covariance Kelly.** **Consequence for S2.2:**
the meta-allocator is the **N-book lift** of `size_book` — it solves the *coupled* allocation `f = c·Σ⁻¹μ` over sleeves (or,
robustly, an ERC/HRP risk budget over the sleeve covariance `Ω` to dodge the inversion) under per-sleeve capacity **box**
constraints (the scalar `capacity_gross` clip lifted to `c_s ≤ cap_s`). "Extending `book::allocation`" means **composing**
the fractional-Kelly `c` + capacity clip, not subclassing — `size_book` is reused as the *per-sleeve* scalar cap; the
cross-sleeve coupling is new (§4.2).

### 0.4 `FactorModel` exposes `exposures()` (X) + `specific_var()` (D) but NOT `F` — the factor/specific split needs no new accessor
The built `FactorModel` makes `exposures()` (M×K `X`) and `specific_var()` (M `D`) public; the K×K factor covariance `F` is
**private** (only reachable as `FactorComponents.F` from `FactorModelBuilder::build_components` *before* `create`). The
forward `apply` (V·in), `apply_inverse` (V⁻¹·in), and `risk(w) = wᵀVw` are all public (the S1 additive touch). **Decision:**
S2.3 needs the factor-vs-specific decomposition `σ²_fund = b_fundᵀF·b_fund + WᵀD·W`; it gets it **without touching
`FactorModel`** via the identity
`factor_var = risk(W) − WᵀD·W` (since `risk(W)=WᵀVW=WᵀXFXᵀW + WᵀDW` and `WᵀDW` comes from `specific_var()`). The aggregate
factor exposure `b_fund = Σ_s c_s·(Xᵀw_s)` is formed from `exposures()` directly (as `book/report.hpp` does). **Net: S2 has
ZERO engine-source touches outside the new `fund/` headers** (cleaner than S1, which added two `FactorModel` accessors). A
`FactorModel::factor_cov()` accessor (the S1-D3-style 2-line read-passthrough) is the recorded *alternative* if a direct
`F` read is later wanted — not needed now.

### 0.5 `accumulate_report` consumes `risk::MultiPeriodResult`, not `MultiHorizonResult` — the fund report is built fresh
`book::accumulate_report(const risk::MultiPeriodResult&, …)` and `book::BookPipeline` both drive
`risk::MultiPeriodOptimizer`, not `MultiHorizonOptimizer`. `MultiHorizonResult` and `MultiPeriodResult` are
**field-identical** (`books`/`turnover`/`cost_bps`) but **distinct types** — a sleeve wrapping `MultiHorizonOptimizer`
(S2.1) produces `MultiHorizonResult`, which `accumulate_report` will not accept. **Decision:** S2.5 builds a **fresh
`FundReport`** from the *netted fund book series* (a new `std::vector<std::vector<f64>>`, not any single sleeve's result),
reusing **`combine::compute_metrics`** for the ONE Sharpe convention (so fund + per-sleeve metrics are bit-comparable). The
sprint does **not** force the `MultiHorizonResult`→`MultiPeriodResult` adapter into the hot path; if a sleeve sub-report is
wanted it adapts by field-copy (recorded, trivial). The `risk::MultiPeriodResult`/`MultiHorizonResult` duplication is a
recorded **residual** (unify into one `risk::BookSchedule` POD → S8 cleanup).

### 0.6 NO attribution / netting / meta-allocator primitive exists anywhere — all of S2's numeric kernels are greenfield
`book/report.hpp` exposes only raw `Xᵀw` `factor_exposures` and **explicitly defers** the OLS/Brinson per-factor pnl split
("DEFERRED RESIDUAL (YAGNI)"); `combine/correlation.hpp` has `pairwise_complete_corr(a,b)` but **no**
`pairwise_complete_cov`; `blend_toward`/`l1_diff` are **private statics** of `MultiPeriodOptimizer`/`MultiHorizonOptimizer`
(not reusable — replicate, exactly as S1 did). **Consequence:** the four genuinely-new numeric kernels of S2 are (a) the
**risk-budget / ERC capital-weight solve** (S2.2, §4.2), (b) the **cross-sleeve risk aggregation + component risk** (S2.3,
§4.3), (c) the **internal-crossing netting + priced benefit** (S2.4, §4.4), and (d) the **Euler attribution-by-sleeve**
(S2.5, §4.5). Each ships an obviously-correct reference + a differential test (R8). The dominant correctness risk is a
**look-ahead-leaking allocator** — one that sets `c_s` from *future* realized sleeve returns; the trailing-risk-budget gate
(R2) is the load-bearing guard (§3).

### 0.7 Net-after-optimize is the design — sleeves optimize independently, the fund crosses ex-post
The fork (RenTech "single unified re-optimized book" vs N independently-optimized sleeve books, §1A B2): a **joint** QP over
all sleeves' alphas is globally optimal but smears sleeve identity (no clean attribution) and is an O(Σ|sleeve|) mega-solve;
**net-after-optimize** keeps sleeves modular + attributable and crosses ex-post. **Decision:** S2 ships **net-after-optimize**
(each sleeve runs its own `MultiHorizonOptimizer.run` blind to the others; the fund nets the realized sleeve books). It is
provably near-optimal when cross-sleeve correlation is low and costs are separable (B2/B5); S2 **measures** the
approximation error — reporting (a) cross-sleeve **position correlation** `corr(w_s, w_t)` and (b) the **fraction of names
traded by ≥2 sleeves** the same period — so the controller sees where a joint solve would add value. The joint mega-QP is a
recorded **lift → S8**.

### 0.8 The risk budget is TRAILING — the central look-ahead trap of S2
The meta-allocator sets `c_s` at period `t` from the **sleeve covariance `Ω`** and per-sleeve vols, both estimated from
**realized sleeve P&L over a trailing window `≤ t`** (R2). Using period-`t` (or future) realized sleeve returns to weight
period-`t` capital is **look-ahead** — the single easiest invariant to break in S2. **Decision:** the `MetaBook` driver
runs in two passes — (1) each sleeve's full `MultiHorizonOptimizer.run` (independent, causal); (2) a fund-level schedule
walk where period `s`'s `c[s]` uses only sleeve P&L from periods `< s` (a trailing `risk_lookback` window). Truncation-
invariance at the fund boundary is the R2 test. At `s=0` (no history) the allocator falls back to **inverse-capacity** or
**equal** weights (recorded), never a peek.

### 0.9 Honest cost — the crossing benefit is priced through the SAME calibrated cost model, never invented
The internal-crossing saving (§1A B1) is `crossing_benefit = Cost(per-sleeve Δ traded separately) − Cost(net book ΔW)`. It
must be priced through the **same** `book::CostInputs` (`round_trip_cost_bps` linear term, the S6-calibrated κ for the
turnover term) the sleeves and the single-fund path use — not a fabricated spread. **Decision:** S2.4 computes
`T_gross = Σ_iΣ_s|c_s·Δw_{s,i}|` and `T_net = Σ_i|Σ_s c_s·Δw_{s,i}|`, asserts `T_net ≤ T_gross` (triangle inequality, R3)
and `crossing_benefit ≥ 0`, and prices the difference through `CostInputs`. A negative benefit is a **cost-model bug**, not
a feature — the gate flags it (R3/R6).

### 0.10 No `/fp:precise` / `-ffp-contract` flag exists — determinism is order-fixed reductions only
The root `CMakeLists.txt` sets `/W4 /permissive-` (+ `/WX` when `ATX_WERROR=ON`, the default) but **no** `/fp:precise` /
`-ffp-contract=off` anywhere (a tree-wide grep confirms; cf. the S1 plan's aspirational `/fp:precise` mention and the
recorded gap). **Consequence:** S2 determinism (R1) rests **entirely on order-fixed reductions** (ascending sleeve / name /
factor index, fixed iteration counts, no `std::unordered_*` iteration in numeric paths) — it cannot lean on a compiler FP
mode. Every S2 reduction (the ERC solve, the netting sum, the Euler attribution, the covariance build) is written
order-fixed; `-Wconversion`-clean (explicit `static_cast` at every `size_t`↔`Eigen::Index` boundary, the linalg convention).

> **Net scope shift vs the ROADMAP sketch:** S2 is **additive and compositional** — the sleeve wraps the proven S1
> optimizer (§0.1), reuses the scalar Kelly + capacity clip (§0.3), the shared factored `V` (§0.4), and the calibrated
> cost (§0.9). The four genuinely-new numeric kernels are the **risk-budget capital solve** (§4.2), the **cross-sleeve risk
> aggregation** (§4.3), the **internal-crossing netting** (§4.4), and the **Euler attribution** (§4.5). The dominant
> correctness risks are a **look-ahead-leaking allocator** (trailing risk budget §0.8), a **netting that fabricates a
> crossing benefit** (§0.9), an **attribution that doesn't sum to the fund** (Euler additivity), and a **meta-book that
> fails to reduce to S1** on the one-sleeve boundary. Each is a named gate in §3.

---

## §1 — Research foundation: the meta-book design rules (with citations)

Derived from the research north-stars (`renaissance-technologies-systems-deep-dive.md` §6.1, `worldquant-systems-deep-
dive.md` §6.2/§8, `finding-alphas-book-deep-dive.md` §8) and the carried-forward `p0`/`p1` invariants. **Non-negotiable**;
every S2 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **R1** | **Deterministic fixed-iteration allocation — no convergence early-exit.** The ERC/risk-budget solve runs a fixed iteration count; the netting sum, the covariance build, the Euler attribution, and every reduction are order-fixed (ascending sleeve/name/factor index); same inputs ⇒ byte-identical fund book schedule + digest. | `p1` invariant #1; the S1 fixed-iteration ADMM precedent; risk-budget convex solve uniqueness [Spinu 2013; Griveau-Billion-Richard-Roncalli 2013]. Determinism is order-fixed (§0.10), not an FP flag. |
| **R2** | **No look-ahead — the risk budget is TRAILING.** `c_s` at period `t` uses only sleeve P&L / vols / correlations from periods `≤ t` (a trailing window); the sleeve combiner weights are trailing-fit; the netting is a same-timestamp aggregation of already-known sleeve targets. Truncation-invariance at the fund boundary is the test. | `p1` invariant #2/#4; the fit/apply firewall; the S1 receding-horizon contract; the §0.8 trailing-risk-budget decision. |
| **R3** | **Netting is exact and the crossing benefit is non-negative.** `W = Σ_s c_s·w_s` exactly; `T_net ≤ T_gross` (triangle inequality); `crossing_benefit ≥ 0` — a violation is a cost-model bug, asserted, never shipped. | Internal-crossing algebra (WorldQuant §6.2, Kakushadze 2016); the convex/separable-cost framing [Boyd et al. 2017]; the §0.9 honest-cost decision. |
| **R4** | **Attribution is Euler-exact.** Per-sleeve **return** contributions `c_s·R_s` sum to fund return; per-sleeve **risk** contributions `c_s·(Ωc)_s/σ_fund` sum to fund σ (homogeneity-degree-1 Euler identity), to fp tolerance; the crossing benefit is fully allocated (pro-rata by crossed volume), never orphaned. | Grinold-Kahn marginal-contribution decomposition [Grinold-Kahn 2000]; Euler risk-contribution identity [Meucci 2007; Roncalli 2013]; Brinson allocation/selection [Brinson-Hood-Beebower 1986; Brinson-Fachler 1985]. |
| **R5** | **The risk budget is achieved, not approximated away.** The ERC/RB solve achieves `RC_s = b_s·σ_fund` within tolerance (the convex log-barrier optimum); the portfolio-of-books fractional-Kelly fund leverage is the `f = c·Σ⁻¹μ` growth-optimal tilt, capacity-clipped per sleeve. | Risk budgeting [Maillard-Roncalli-Teïletche 2010; Roncalli 2013]; HRP robustness [López de Prado 2016]; multi-strategy Kelly = MV at γ=1 [Kelly 1956; Thorp 2006]. |
| **R6** | **Shared factored covariance — never re-estimated, never densified.** Fund risk uses the SAME `FactorModel` (one `X, F, D`) all sleeves see; aggregate exposure `b_fund = Σ_s c_s·Xᵀw_s` is linear; the factor/specific split is `risk(W) − WᵀDW` (no `F` accessor, no dense M×M). Calibrated κ via `book::CostInputs`. | `p1` invariant #5/#6; the S1/S8 factored `V` path; the §0.4 zero-touch decision. |
| **R7** | **Reduce to S1 on the boundary.** With one sleeve, full capital weight (`c=1`), and no vol-target rescale (so the fund book == the sleeve book), the meta-book is **bit-identical** to that sleeve's `MultiHorizonOptimizer.run`. The generalization is pinned to the proven S1 layer. | `module.md` carry-forward discipline; the S1→S7 boundary-pin precedent (this sprint's regression anchor). |
| **R8** | **Differential correctness of every kernel.** The ERC solve (CCD vs Newton cross-check + vs a hand-built equicorrelation closed form), the netting (vs a brute-force gross/net), the component risk (Euler-sum == σ_fund), and the diversification measures each ship an obviously-correct reference + a bit-/ULP-bounded differential test. | `p1` invariant #7; the `p0`/`p1`/S1 differential-test precedent for every new compute kernel. |

**One-sentence thesis:** *S1 already proves one sleeve's constrained multi-horizon book is deterministic, look-ahead-safe,
and pinned to S7 — so S2 is the layer that (a) wraps N sleeves over their own library subsets (R-membership), (b) sets a
**trailing** cross-sleeve risk budget + portfolio-Kelly leverage (R2/R5), (c) **nets** them into one fund book whose
crossing benefit is honestly priced (R3/R6), and (d) attributes the fund's return + risk back to sleeves Euler-exactly
(R4); pinned bit-for-bit to S1 on the one-sleeve boundary (R7); the only genuinely-new correctness risks are the
allocator's look-ahead safety + determinism, the netting's benefit-non-negativity, and the attribution's additivity.*

---

## §1A — State-of-the-art research grounding (verified literature)

> Sourced via a two-stream web-research pass (allocation theory + meta-book mechanics) with per-citation VERIFIED/UNVERIFIED
> tagging (2026-06-13). Primary sources confirmed against arXiv / DOI / publisher where marked **VERIFIED**; **UNVERIFIED**
> = a canonical result stated from established knowledge whose exact primary text was not re-extracted this pass (the
> citation's existence is confirmed; treat the precise equation as standard-but-not-re-quoted). Full citations in the
> **References** section.

### Track A — Capital & risk allocation (the *what* of the meta-allocator)

**A1 — Risk budgeting / Equal-Risk-Contribution (the default allocator).** [grounds R5, §4.2] **[VERIFIED]**
Euler-homogeneity of `σ(w)=sqrt(wᵀΣw)` (degree 1) gives the exact additive split `σ(w)=Σ_i RC_i` with marginal
`MRC_i=(Σw)_i/σ(w)` and total `RC_i=w_i·(Σw)_i/σ(w)`. The **risk-budget** portfolio solves `RC_i=b_i·σ(w)` (`Σb_i=1`,
`b_i>0`); **ERC** is `b_i≡1/N`. The **solvable form** is the strictly-convex **log-barrier**
`min ½wᵀΣw − Σ_i b_i·ln(w_i)` on `w>0` (stationarity `w_i·(Σw)_i=b_i` is exactly the budget condition) — a **unique**
solution, **no matrix inversion**, no PSD-only requirement beyond `Σ⪰0`. Solved by **cyclical coordinate descent** (scalar
quadratic per coordinate, positive root) or **Newton** (Spinu; Hessian `Σ+diag(b_i/w_i²)` SPD ⇒ quadratic convergence);
ERC vol sits between min-variance and equal-weight (`σ_MV ≤ σ_ERC ≤ σ_EW`); under equicorrelation `w_i ∝ 1/σ_i`
(inverse-vol). *— Maillard, Roncalli, Teïletche (2010); Spinu (2013); Griveau-Billion, Richard, Roncalli (2013);
Roncalli (2013) textbook. **This is the §4.2 `erc_log_barrier` kernel; the inverse-vol equicorrelation form is the cheap
`s=0` fallback (§0.8); CCD-vs-Newton is the R8 differential cross-check.***

**A2 — Hierarchical Risk Parity (the estimation-error-robust alternative).** [grounds R5, §4.2] **[VERIFIED]**
Three stages: (1) **tree clustering** on correlation-distance `d_ij=sqrt(½(1−ρ_ij))` (a proper metric) + agglomerative
linkage; (2) **quasi-diagonalization** (seriate `Σ` so correlated sleeves are adjacent); (3) **recursive bisection** —
split the seriated list, weight each half by inverse-variance `w̃_i=(1/Σ_ii)/Σ_j(1/Σ_jj)`, split factor
`α=1−V₁/(V₁+V₂)`, recurse; final weight = product of split factors. **Never inverts `Σ`** ⇒ immune to "Markowitz's curse"
(the `Σ⁻¹` amplification of the smallest, noisiest eigenvalues) and robust on near-singular sleeve correlation (e.g. many
sleeves sharing a signal family). *— López de Prado (2016); NCO upgrade (López de Prado 2020). **The `HierarchicalRiskParity`
method (§4.2) for the large-N / noisy-`Ω` regime; stage-1 `d=sqrt(0.5(1−ρ))` also groups correlated sleeves into
super-sleeves.***

**A3 — Fractional Kelly across correlated strategies (the fund leverage).** [grounds R5, §4.2] **[VERIFIED]**
For jointly-Gaussian excess returns `(μ,Σ)` the growth-optimal (max-log-wealth) bet vector is `f*=Σ⁻¹μ` — **identical to
unconstrained mean-variance at γ=1** (`argmax wᵀμ−½wᵀΣw`). Growth `g(f)=fᵀμ−½fᵀΣf`, max `g*=½μᵀΣ⁻¹μ=½·SR_fund²`.
**Fractional** Kelly scales `f=c·Σ⁻¹μ` (`c≈0.25–0.5`): half-Kelly keeps **75%** of full-Kelly growth at **half** the
variance (`g(c·f*)=(c−½c²)·μᵀΣ⁻¹μ`), the flat-topped-optimum asymmetry — robust to `μ`/`Σ` estimation error and
drawdown-controlling. *— Kelly (1956); Thorp (2006); MacLean-Thorp-Ziemba (2011). **The fund-level fractional-Kelly leverage
scalar `c` (§4.2): the N-book lift of the scalar `book::size_book` — `f=c·Σ⁻¹μ` over sleeves vs the per-book `c·SR/σ`
(§0.3). Routed through ERC/HRP weights × fund-vol-target to dodge the `Σ⁻¹` inversion when `Ω` is noisy.***

**A4 — Grinold-Kahn fundamental law / portfolio-of-portfolios (the objective rationale).** [grounds R4/R5] **[VERIFIED]**
`IR=IC·sqrt(BR)` (breadth `BR` = number of *independent* bets); with a transfer coefficient `IR=TC·IC·sqrt(BR)` (`TC<1`
from constraints/costs **[UNVERIFIED — Clarke-de Silva-Thorley 2002 not re-extracted]**); value-add `VA*=IR²/(4λ)`. Across
sleeves: uncorrelated `IR_fund=sqrt(ΣIR_k²)`; correlated `IR_fund²=aᵀR⁻¹a` (`a`=sleeve IRs, `R`=sleeve correlation) — lower
correlation ⇒ higher `R⁻¹` quadratic form ⇒ higher fund IR. *— Grinold (1989); Grinold-Kahn (2000). **The reason the
meta-book wants many low-correlated sleeves — `IR_fund²=aᵀR⁻¹a` is the quantity the allocator implicitly maximizes;
`TC` maps to the capacity/cost clips eroding ideal weights.***

**A5 — Diversification measures (the fund-level gate) + vol targeting.** [grounds R4, §4.5] **[VERIFIED]**
*Effective number of bets* (Meucci 2009): PCA `Σ=EΛEᵀ`, principal weights `ω̃=Eᵀw`, diversification distribution
`p_i=(ω̃_i²λ_i)/Σ_j(ω̃_j²λ_j)`, **`N_Ent=exp(−Σ_i p_i·ln p_i)`** (1 ⇒ one bet, N ⇒ N independent bets). *Diversification
ratio* (Choueifaty-Coignard 2008): `DR(w)=(Σ_i w_iσ_i)/σ(w)≥1`, MDP `=argmax DR`, cheap proxy `N_DR=DR²`. *Vol targeting*
(Moreira-Muir 2017): weight `∝1/σ̂²` raises Sharpe; fund scale `k=σ*/σ_fund`, gross-clipped `k=min(σ*/σ_fund, G/‖W‖₁)`.
*— Meucci (2009); Choueifaty-Coignard (2008); Moreira-Muir (2017). **`N_Ent` (or `DR²`) is the fund-level diversification
report/gate (§4.5); the gross-clipped vol-target `k` is the final fund stage (§4.2).***

### Track B — Meta-book mechanics (the *how* of netting / attribution / risk)

**B1 — Internal trade crossing / netting (the cost saving).** [grounds R3/R6, §4.4] **[VERIFIED quote]**
Fund net trade `ΔW_i=Σ_s c_s·Δw_{s,i}`; **gross** turnover `T_gross=Σ_iΣ_s|c_s·Δw_{s,i}|` vs **net**
`T_net=Σ_i|Σ_s c_s·Δw_{s,i}|`; triangle inequality ⇒ `T_net≤T_gross` (equality iff every sleeve trades the same sign on
every name). The **crossed (saved) volume** on name `i` is `Σ_s|c_s·Δw_{s,i}| − |Σ_s c_s·Δw_{s,i}| ≥ 0` (two-sleeve
buy/sell: `2·min(|x|,|y|)` round-trip never reaches the market). **Crossing alpha** =
`Cost(per-sleeve Δ separate) − Cost(net ΔW)` priced through the same model; for a convex impact term the saving is *larger*
than the linear-only estimate (splitting avoids the steep part of the impact curve). Look-ahead-safe by construction (a
same-timestamp aggregation of already-known sleeve targets — no realized return consulted). *— WorldQuant "automatic
internal crossing of trades" [Kakushadze 2016, *101 Formulaic Alphas*; Tulchinsky et al. 2019, *Finding Alphas* ch.7];
impact term [Almgren-Chriss 2000]. **The §4.4 netting kernel + the `T_net≤T_gross` / `benefit≥0` R3 invariants.***

**B2 — Unified book vs portfolio-of-books (the design fork).** [grounds R-membership, §0.7] **[VERIFIED as characterization]**
**Joint optimize** (one QP over all sleeves' alphas): globally optimal, crosses ex-ante, but smears sleeve identity (no
clean attribution) and is an O(Σ|sleeve|) mega-solve. **Net-after-optimize** (independent sleeves, ex-post cross): modular,
attributable; near-optimal when (a) **cross-sleeve correlation is low** (the joint risk cross term `2c_s c_t·w_sᵀV w_t ≈ 0`)
and (b) **costs are separable** across names and sleeves rarely trade the same name same bar. *— RenTech single-unified-model
characterization (secondary); Boyd et al. (2017) separable per-asset cost `φ(Δw)=Σ_i(a_i|Δw_i|+b_i|Δw_i|^{3/2}+…)`;
Kolm-Ritter decoupling. **S2 ships net-after-optimize (§0.7) and measures the approximation error (cross-sleeve position
correlation + multi-sleeve-name fraction); the joint mega-QP is the recorded S8 lift.***

**B3 — Performance attribution by sleeve (the report).** [grounds R4, §4.5] **[VERIFIED]**
*Brinson-Hood-Beebower (1986):* `Allocation_i=(w_p,i−w_b,i)R_b,i`, `Selection_i=w_b,i(R_p,i−R_b,i)`,
`Interaction_i=(w_p,i−w_b,i)(R_p,i−R_b,i)`, summing to `R_p−R_b`. *Brinson-Fachler (1985):* benchmark-relative allocation
`(w_p,i−w_b,i)(R_b,i−R_b)` (positive when active weight + segment excess share a sign — preferred). *Grinold-Kahn:* fund
return is linear/additive `R_fund=Σ_s c_s·R_s` (marginal `c_s·R_s`); fund **risk** Euler-decomposes
`MCR_risk_s=c_s·(Ωc)_s/σ_fund`, `Σ_s MCR_risk_s=σ_fund`. The **crossed P&L** is allocated **pro-rata by contributed crossed
volume** (default) or **marginal/Shapley** (flagged; arXiv:2102.05799). *— Brinson-Hood-Beebower (1986); Brinson-Fachler
(1985); Grinold-Kahn (2000); Shapley attribution (2021). **The §4.5 `SleeveAttribution` — Euler-sum asserted (R4); sleeve
treated as a "segment" so Brinson drops in if a benchmark is supplied.***

**B4 — Cross-sleeve risk aggregation (the shared `V`).** [grounds R6, §4.3] **[VERIFIED]**
Aggregate factor exposure is **linear**: `b_fund=XᵀW=Σ_s c_s·(Xᵀw_s)`. Fund risk via the shared `V=XFXᵀ+D`:
`σ²_fund=WᵀVW=b_fundᵀF·b_fund + WᵀD·W` (factor + specific). Sleeve-return covariance `Ω` (S×S) from trailing P&L; component
risk `RC_s=c_s·(Ωc)_s/sqrt(cᵀΩc)`, `Σ RC_s=σ_fund` (Euler); component VaR `CVaR_s=c_s·z_α·(Ωc)_s/σ_fund`,
`Σ CVaR_s=VaR_fund`. *— Grinold-Kahn (2000); Roncalli (2013); Meucci (2007) "Risk Contributions from Generic User-Defined
Factors". **§4.3 computes `b_fund` from `exposures()`, `σ_fund` from `risk(W)`, the factor/specific split via `risk(W)−WᵀDW`
(§0.4, no `F` accessor), and `RC_s` from `Ω`; the Euler-sum is the R4/R8 test.***

**B5 — The meta-book design synthesis.** S2 = **net-after-optimize** (B2) modular sleeves (§0.1) + a **trailing** ERC/HRP
risk budget (A1/A2, §0.8) + a **portfolio-of-books fractional-Kelly** fund leverage (A3, the N-book lift of `size_book`
§0.3) + **internal crossing** with an honestly-priced benefit (B1, §0.9) + **Euler attribution** (B3/B4) + a **Meucci/`DR²`
diversification gate** (A5). Pinned bit-for-bit to S1 on the one-sleeve boundary (R7). The recorded lifts: a dedicated
`core::linalg::risk_budget` ERC solver (§2.1), the joint mega-QP (§0.7), and library-driven sleeve membership/clustering
(§0.2 → S4/S8).

---

## §2 — File structure

### 2.1 atx-core / Pattern-B requests (decided at kickoff)

> The engine adds no general-purpose primitive (project rule). S2 records the cross-module edge and ships on existing
> primitives, exactly as `p1` S1–S8 + `p2` S1 did:
>
> 1. **L7 `risk_budget` → atx-core.** A deterministic convex risk-budget / ERC solve (log-barrier CCD/Newton over a
>    covariance + budget vector, supporting a generic degree-1 risk measure). Ship on an **engine-local fixed-iteration
>    CCD/Newton** (`fund::erc_log_barrier`, S2.2); the dedicated `core::linalg::risk_budget` is the recorded lift. Engine
>    fallback is shippable now and bit-deterministic (N = #sleeves is small ⇒ a fixed-count Newton is cheap + exact).
> 2. **Risk contributions / Euler decomposition → engine-local.** `σ(w)`, `MRC`, `RC` are a handful of factored-`V`
>    applies + an `Ω`-multiply; no atx-core request. Reuse `FactorModel::{risk, apply, exposures, specific_var}`.
> 3. **`book::size_book` / `effective_breadth`, calibrated κ, capacity ceiling → already in `p1` (S7/S6).** Reuse
>    `book::{AllocationConfig, size_book, effective_breadth, CostInputs}`; no request — S2.2 composes them per sleeve.
> 4. **`combine::pairwise_complete_corr` → already in `p1`.** Reuse for the sleeve-return correlation; build the covariance
>    `Ω` from it order-fixed (no `pairwise_complete_cov` exists, §0.6) — no request.

### 2.2 Engine files (this sprint builds these — new `fund/` layer, namespace `atx::engine::fund`)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/fund/sleeve.hpp` | `SleeveTag`, `SleeveConfig` (wrapped `MultiHorizonConfig` + `members` id-list + tag + per-sleeve `capacity_gross`), `Sleeve` — wraps a `risk::MultiHorizonOptimizer` over its library subset; produces the sleeve book series (§4.1/§0.1/§0.2) | S2-1 |
| `include/atx/engine/fund/meta_allocator.hpp` | `RiskBudgetMethod`, `MetaAllocatorConfig`, `CapitalWeights`, `MetaAllocator` — ERC/HRP/inverse-vol risk budget + portfolio-of-books fractional-Kelly + gross-clipped vol-target → `c_s`; fixed-iteration log-barrier, per-sleeve capacity box (§4.2/§0.3/§0.8/R1/R5) | S2-2 |
| `include/atx/engine/fund/cross_sleeve_risk.hpp` | `FundRisk`, `fund_risk(...)`, `sleeve_return_cov(...)` — shared-`V` aggregate exposure `b_fund=Σc_sXᵀw_s`, fund risk split `risk(W)−WᵀDW`, sleeve covariance `Ω`, Euler component risk/VaR (§4.3/§0.4/R4/R6) | S2-3 |
| `include/atx/engine/fund/netting.hpp` | `NetResult`, `net_fund_book(...)` — `W=Σc_sw_s`, `T_gross`/`T_net`, priced `crossing_benefit≥0`; the internal-crossing kernel (§4.4/§0.9/R3) | S2-4 |
| `include/atx/engine/fund/meta_book.hpp` | `MetaBookConfig`, `SleeveAttribution`, `MetaBookResult`, `FundReport`, `MetaBook` — the two-pass driver (sleeves → trailing allocate → net → fund first move) + fund report + attribution-by-sleeve; the S1 boundary pin (§4.5/§0.5/§0.7/R2/R4/R7) | S2-5 |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)
`fund_sleeve_test.cpp` (S2-1), `fund_meta_allocator_test.cpp` (S2-2 + the ERC-exactness / CCD-vs-Newton differential),
`fund_cross_sleeve_risk_test.cpp` (S2-3 + the Euler-sum proof), `fund_netting_test.cpp` (S2-4 + the
`T_net≤T_gross` / benefit≥0 / opposite-books-net-zero invariants),
`fund_meta_book_integration_test.cpp` (S2-5, the determinism + look-ahead + netting + attribution + **S1 boundary-pin**
capstone). Bench: `bench/meta_book_bench.cpp` (S2-5: funds/sec across `(S sleeves, N universe, K factors)`; the ERC solve
cost; net-after-optimize vs the cross-sleeve correlation it approximates).

### 2.4 Ledger
`sprint-2-progress.md` (S2-0), updated per unit (copy `p2` `sprint-1-progress.md` shape). S2 is **6 units incl. marker** —
within the 4–7 ceiling, **no split expected**; split S2-a (S2-0…S2-2: sleeve + allocator) / S2-b (S2-3…S2-5: risk +
netting + driver) only if S2.2 (allocator) or S2.5 (driver) over-run, exactly as `p1` S4/S7 did.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Suite_Condition_ExpectedResult`; cover happy path, **boundaries** (the S1 reduction
  pin §0.1/R7; a single-sleeve fund; two identical sleeves → crossing benefit 0, fund = capital-weighted book; two exactly
  opposite sleeves → net book 0, benefit = full gross; an all-NaN sleeve P&L column; an empty schedule; `s=0` no-history
  allocation fallback §0.8; a zero-vol sleeve; a degenerate `Ω` (perfectly correlated sleeves) → HRP/ERC robustness; a
  per-sleeve capacity box that binds; vol-target vs gross-cap binding §4.2), and the **invariant proofs** (R1 two-builds-
  byte-identical, R2 truncation-invariance at the fund fit boundary, R3 `T_net≤T_gross`+`benefit≥0`, R4 Euler-sum ==
  fund return/σ, R7 the bit-for-bit S1 reduction).
- **Determinism (R1):** the ERC solve runs a **fixed iteration count** (never a residual test); the netting sum, the
  covariance build, the Euler attribution, and every reduction are order-fixed (ascending sleeve/name/factor index); a
  **two-builds-equal** test (same inputs → byte-identical fund book schedule + digest) is mandatory for S2-2 and S2-5. No
  RNG in S2 (the allocator is deterministic by construction). **Determinism is order-fixed reductions, not `/fp:precise`
  (§0.10).**
- **No look-ahead / PIT (R2):** the risk budget `c_s` at period `t` reads only sleeve P&L / vols / correlations from
  periods `≤ t` (a trailing window); the sleeve combiner weights are trailing-fit; the netting is a same-timestamp
  aggregation. **Truncation-invariance is the test** at the fund fit boundary. `s=0` falls back to inverse-capacity/equal,
  never a peek.
- **Netting exactness + honest benefit (R3/R6):** `W = Σ_s c_s·w_s` exactly; `T_net ≤ T_gross` (asserted); the crossing
  benefit is priced through the **same** `book::CostInputs` (round-trip bps + S6-κ) and is `≥ 0` (a negative is a flagged
  cost-model bug, never shipped). The fund risk uses the SAME `FactorModel` — no re-estimation, no dense M×M.
- **Attribution additivity (R4):** per-sleeve return contributions sum to fund return and per-sleeve risk contributions
  sum to `σ_fund` within fp tolerance (the Euler identity); the crossing benefit is fully allocated.
- **No hot-path alloc (`p1` #6):** the fund schedule walk reuses pre-sized scratch (the `Ω` build, the allocator workspace,
  the netting buffer are pre-sized once); cold paths (the ERC factor, the trailing-window covariance) may allocate
  (documented).
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures (dim
  mismatch, empty sleeves, singular `Ω`, infeasible risk budget, capacity below floor); weakest sufficient types
  (`std::span`, `const&`, `std::string_view`); functions ≤ ~60 lines; **reuse `risk::{MultiHorizonOptimizer, FactorModel}`,
  `book::{CostInputs, size_book, effective_breadth, AllocationConfig}`, `combine::{compute_metrics, pairwise_complete_corr}`,
  `eval::*` — do NOT reinvent the optimizer, the scalar Kelly, the cost model, the correlation helper, or the Sharpe**; the
  only new numeric kernels are the risk-budget solve, the cross-sleeve risk aggregation, the netting, and the attribution
  (the ERC solver recorded as the Pattern-B lift).
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl, `ATX_WERROR=ON`). **No `/fp:precise` flag exists** (§0.10) —
  `-Wconversion`-clean (explicit `static_cast` at every `size_t`↔`Eigen::Index` boundary). clang-tidy disabled — the
  strict build + ctest are the gate.
- **clangd noise:** ignore squiggles; only a real `cmake --build` + ctest are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree (REUSE, do NOT rebuild):
risk/multi_horizon.hpp (MultiHorizonOptimizer::run(sched, sources_at, model_at, CostInputs) -> MultiHorizonResult{books,
turnover, cost_bps} — the S1 constrained multi-horizon optimizer EACH SLEEVE WRAPS; you compose N of these, you do NOT
modify it; ONE sleeve + c=1 + no vol-target => fund book BYTE-IDENTICAL to it, the boundary pin); risk/factor_model.hpp
(FactorModel V=XFXᵀ+D: risk(w)=wᵀVw, apply (V·in), exposures()=X (M×K), specific_var()=D — the SHARED risk model all
sleeves see; fund factor risk = risk(W) − WᵀDW, NO F accessor needed, NEVER densify M×M); book/allocation.hpp
(size_book(sr, σ, capacity_gross, AllocationConfig{fractional_kelly, max_gross}), effective_breadth(eigvals) — the SCALAR
single-book Kelly you LIFT to N books: per-sleeve capacity becomes a box constraint, the cross-sleeve Σ⁻¹/ERC coupling is
new); book/multi_period.hpp (book::CostInputs{kappa, round_trip_cost_bps, capacity_gross} — the S6-calibrated cost the
crossing benefit is priced through, ONE model); combine/metrics.hpp (compute_metrics(pnl, positions, n, book_size) ->
AlphaMetrics{sharpe,...} — ONE Sharpe convention for fund AND per-sleeve); combine/correlation.hpp
(pairwise_complete_corr(a,b) — the sleeve-return correlation helper; NO pairwise_complete_cov exists, build Ω order-fixed);
library/library.hpp (Library: AlphaId dense 0..n_alphas(), state_as_of(id,t); NO subset/family/universe filter — the
SLEEVE owns its own member id-list + tags). atx-core: Result<T>/Status/Ok/Err(ErrorCode, msg), ErrorCode has
{InvalidArgument, OutOfRange, NotImplemented, Internal, ...} — NO Infeasible/Unbounded enumerator (use InvalidArgument with
a descriptive message); linalg MatX/VecX (Eigen, column-major; explicit static_cast at size_t<->Index boundaries).

THIS SPRINT'S DOMINANT RISK IS A LOOK-AHEAD-LEAKING / NON-ADDITIVE META-BOOK: an allocator that sets c_s from FUTURE
sleeve returns; a netting that fabricates a crossing benefit; an attribution that doesn't sum to the fund; a meta-book that
does NOT reduce to S1 on the one-sleeve boundary. The gates:
  - DETERMINISM (R1): the ERC/risk-budget solve runs a FIXED iteration count, NEVER a residual test; the netting sum, the
    Ω build, the Euler attribution all order-fixed (ascending sleeve/name/factor). Two builds of SAME inputs =>
    BYTE-IDENTICAL fund book schedule + digest. Determinism is order-fixed reductions, NOT a compiler fp flag.
  - NO LOOK-AHEAD (R2): c_s at period t reads ONLY sleeve P&L/vol/corr from periods <= t (a TRAILING window); sleeve
    combiner weights trailing-fit; netting is a same-timestamp aggregation. TRUNCATION-INVARIANT at the fund boundary.
    s=0 falls back to inverse-capacity/equal, NEVER a peek.
  - NETTING + HONEST COST (R3/R6): W = Σ c_s·w_s exactly; T_net <= T_gross (triangle, asserted); crossing_benefit >= 0,
    priced through the SAME book::CostInputs. A negative benefit is a cost-model BUG, flag it. Fund risk uses the SHARED
    FactorModel; NO re-estimation, NO dense M×M.
  - ATTRIBUTION (R4): per-sleeve return contributions sum to R_fund; per-sleeve risk contributions sum to σ_fund (Euler).
    The crossing benefit is fully allocated (pro-rata by crossed volume).
  - REDUCE TO S1 (R7): one sleeve + c=1 + no vol-target => fund book BYTE-IDENTICAL to MultiHorizonOptimizer.run. The
    load-bearing regression.

PIT, NaN/delisted names get 0 weight and round-trip verbatim (no survivorship). Reuse ONE cost model (book::CostInputs),
ONE risk model (the shared FactorModel), ONE Sharpe (combine::compute_metrics). Header-only inline. Functions <= ~60 lines.
Build gate: cmake --build build --preset ninja --target atx-engine-tests (/W4 /permissive- /WX, ATX_WERROR=ON)
+ ctest --preset ninja -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER git add -A;
after committing run `git show HEAD --stat` (only your files); never touch atx-core/* or atx-tsdb/*; do not push.
End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 Sleeve — wrap an S1 optimizer over a library subset (S2-1)

A sleeve is a thin, owning identity: its **membership** (an `AlphaId` id-list the sleeve maintains, since `Library` cannot
slice §0.2), its **tags** (universe × signal-family), its **horizon** (the `SignalHorizon` in the wrapped config), and the
wrapped `risk::MultiHorizonOptimizer`. It produces the per-period sleeve book series — and, for R7, *is* the S1 optimizer
plus metadata.

```cpp
// section: fund/sleeve.hpp  (namespace atx::engine::fund)
struct SleeveTag { std::string universe; std::string family; };  // free-form membership labels (§0.2)

struct SleeveConfig {
  risk::MultiHorizonConfig mh;                       // the wrapped S1 optimizer (incl SignalHorizon per source)
  std::vector<library::AlphaId> members;             // the library SUBSET this sleeve owns (Library can't slice, §0.2)
  SleeveTag tag{};
  atx::f64 capacity_gross = 1e9;                     // per-sleeve capacity ceiling → the meta-allocator's box (§0.3)
};

class Sleeve {
public:
  SleeveConfig cfg;
  // Run this sleeve's MultiHorizonOptimizer over the schedule → the sleeve's per-period target books.
  // sources_at builds the {(alpha cross-section, SignalHorizon)} for THIS sleeve's members (its mega-alpha).
  // PURE DELEGATION: this is MultiHorizonOptimizer{cfg.mh}.run(...) — the R7 boundary pin is structural.
  [[nodiscard]] atx::core::Result<risk::MultiHorizonResult>
  run(const risk::RebalanceSchedule& sched,
      const std::function<risk::HorizonSources(atx::usize)>& sources_at,
      const std::function<const risk::FactorModel&(atx::usize)>& model_at,
      const book::CostInputs& cost) const
  {
    return risk::MultiHorizonOptimizer{cfg.mh}.run(sched, sources_at, model_at, cost);  // §0.1: compose, don't rewrite
  }
  [[nodiscard]] atx::usize n_members() const noexcept { return cfg.members.size(); }
};
```
**Membership (§0.2):** `members` is the sleeve's own id-list (a clustering / hand-assignment); the sleeve mega-alpha is a
`combine::CombinedSignalSource` over those members (built by the caller, fed through `sources_at`). **R7 (boundary pin):**
`Sleeve::run` *is* `MultiHorizonOptimizer::run` — a one-sleeve meta-book that capital-weights it `1.0` and skips netting/vol-
target is byte-identical to S1 by construction; S2-1's test asserts the delegation is transparent (same fixture ⇒ same
`MultiHorizonResult`). **Differential (R8):** a sleeve over a single-member subset == that member's own book.

### 4.2 Meta-allocator — risk budget + portfolio-of-books Kelly → capital weights (S2-2)

The N-book lift of `book::size_book`. Given the **trailing** sleeve covariance `Ω` (S×S, §4.3), per-sleeve vols, and
per-sleeve capacities, compute the risk-budget weights (ERC log-barrier / HRP / inverse-vol), apply the portfolio-of-books
fractional-Kelly fund leverage, and gross-clip the vol-target. **Fixed iteration count, no early-exit** (R1).

```cpp
// section: fund/meta_allocator.hpp  (namespace atx::engine::fund)
enum class RiskBudgetMethod : atx::u8 { InverseVol, EqualRiskContribution, HierarchicalRiskParity };

struct MetaAllocatorConfig {
  RiskBudgetMethod method   = RiskBudgetMethod::EqualRiskContribution;  // A1 default
  std::vector<atx::f64> risk_budget;     // b_s (Σb=1, b>0); EMPTY => equal (ERC). (A1)
  atx::f64   fractional_kelly = 0.3;     // c: fund-level portfolio-of-books Kelly leverage (A3; estimation-robust)
  atx::f64   target_vol     = 0.0;       // σ* fund vol target; 0 => Kelly scale only (A5)
  atx::f64   max_gross      = 4.0;       // fund gross cap G (the clip §4.2 / A5)
  atx::usize solve_iters    = 64;        // FIXED Newton/CCD count for the ERC solve — NO early-exit (R1)
};

struct CapitalWeights { std::vector<atx::f64> c; };   // per-sleeve capital weight (post Kelly + clip)

class MetaAllocator {
public:
  MetaAllocatorConfig cfg;

  // Omega = trailing sleeve-return covariance (S×S, §4.3); sleeve_vol = sqrt(diag Ω); caps = per-sleeve capacity box.
  // TRAILING-fit (R2): Omega/vols come from periods <= t only. Order-fixed, fixed-iteration (R1).
  [[nodiscard]] atx::core::Result<CapitalWeights>
  allocate(const atx::core::linalg::MatX& Omega, std::span<const atx::f64> sleeve_vol,
           std::span<const atx::f64> caps) const
  {
    const atx::usize S = sleeve_vol.size();
    if (Omega.rows() != static_cast<atx::core::linalg::MatX::Index>(S)) return atx::core::Err(/*InvalidArgument*/);
    // (1) risk-budget weights w_rb (Σ=1): ERC log-barrier (A1) / HRP (A2) / inverse-vol (A1 equicorrelation fallback).
    VecX w_rb = risk_budget_weights(Omega, sleeve_vol);          // fixed-iter; method dispatch
    // (2) portfolio-of-books fractional-Kelly fund leverage (A3): scale toward c·Σ⁻¹μ via the fund-vol target.
    //     sigma_fund = sqrt(w_rbᵀ Ω w_rb); kelly+vol-target scale, gross-clipped (A5):
    atx::f64 sigma_fund = std::sqrt(quad_form(Omega, w_rb));     // order-fixed
    atx::f64 k_vol  = (cfg.target_vol > 0.0 && sigma_fund > 0.0) ? cfg.target_vol / sigma_fund : 1.0;
    atx::f64 k = cfg.fractional_kelly * k_vol;
    // (3) gross clip + per-sleeve capacity box: c_s = clip( k·w_rb_s , 0 , cap_s ); then enforce Σ|c|·... ≤ G.
    VecX c = apply_kelly_caps(w_rb, k, caps, cfg.max_gross);     // min(vol-implied, gross-implied) clip (A5)
    return atx::core::Ok(to_vector(c));
  }
private:
  // ERC: min ½wᵀΩw − Σ b_s ln w_s (Spinu log-barrier, A1). Cyclical coordinate descent OR Newton, FIXED solve_iters.
  // Per-coordinate CCD update: w_s = (−β + sqrt(β² + 4 Ω_ss b_s)) / (2 Ω_ss), β = Σ_{t≠s} w_t Ω_st ; then renorm Σ=1.
  static atx::core::linalg::VecX erc_log_barrier(const atx::core::linalg::MatX& Omega,
                                                 std::span<const atx::f64> b, atx::usize iters);
  // HRP (A2): corr-distance d=sqrt(0.5(1−ρ)) → linkage → quasi-diagonalize → recursive bisection. No Ω⁻¹.
  static atx::core::linalg::VecX hrp_weights(const atx::core::linalg::MatX& Omega);
  static atx::core::linalg::VecX inverse_vol(std::span<const atx::f64> sleeve_vol);  // w_s ∝ 1/σ_s (A1 equicorr)
};
```
**Risk-budget exactness (R5):** the ERC log-barrier optimum satisfies `RC_s = b_s·σ_fund` — the S2-2 test asserts the
returned weights' risk contributions match the budget within tolerance. **Determinism (R1):** `solve_iters` is fixed; CCD/
Newton sweep order is ascending; `risk_budget_weights`, `quad_form`, and the clip are order-fixed. **Differential (R8):**
CCD vs Newton must agree to tolerance on a shared `Ω`; both vs the **equicorrelation closed form** `w_s ∝ 1/σ_s` (A1) on a
constant-correlation fixture. **Capacity box (§0.3):** `apply_kelly_caps` clips each `c_s ≤ cap_s` (the scalar `size_book`
capacity lifted to a box), then the gross clip `k = min(σ*/σ_fund, G/‖w_rb‖₁)` (A5). **`s=0` fallback (§0.8):** an empty/
degenerate `Ω` ⇒ `inverse_vol` (or equal) — never a peek.

### 4.3 Cross-sleeve risk model — aggregate exposure, fund risk split, component risk (S2-3)

The shared `FactorModel` (one `X, F, D` all sleeves see) gives the fund's factor exposure and risk for free; the sleeve
covariance `Ω` gives the component risk that feeds the allocator. **No re-estimation, no dense M×M, no `F` accessor** (§0.4).

```cpp
// section: fund/cross_sleeve_risk.hpp  (namespace atx::engine::fund)
struct FundRisk {
  atx::f64 sigma_fund   = 0.0;           // sqrt(WᵀVW) via FactorModel::risk(W)            (R6)
  atx::f64 factor_var   = 0.0;           // = risk(W) − WᵀDW  (= b_fundᵀF b_fund; NO F accessor, §0.4)
  atx::f64 specific_var = 0.0;           // = WᵀDW from specific_var()
  std::vector<atx::f64> b_fund;          // aggregate factor exposure Σ_s c_s·(Xᵀ w_s) from exposures()  (B4)
  std::vector<atx::f64> risk_contrib;    // RC_s = c_s·(Ω c)_s / σ_fund ; Σ RC_s = σ_fund (Euler, R4)
};

// Fund risk from the netted book W = Σ c_s w_s + the shared V + the sleeve covariance Ω. Order-fixed (R1).
[[nodiscard]] atx::core::Result<FundRisk>
fund_risk(std::span<const std::span<const atx::f64>> sleeve_books,   // w_s (one span per sleeve)
          std::span<const atx::f64> c, const risk::FactorModel& V,
          const atx::core::linalg::MatX& Omega)
{
  const atx::usize M = V.n_instruments(), S = c.size();
  std::vector<atx::f64> W(M, 0.0);                                   // W = Σ_s c_s w_s, ascending sleeve then name (R1/R3)
  for (atx::usize s = 0; s < S; ++s)
    for (atx::usize i = 0; i < M; ++i) W[i] += c[s] * sleeve_books[s][i];
  FundRisk r;
  atx::f64 var_total = V.risk(W);                                    // WᵀVW (factored, R6)
  r.specific_var = dot_diag(V.specific_var(), W);                    // WᵀDW (D from specific_var(), §0.4)
  r.factor_var   = var_total - r.specific_var;                       // = b_fundᵀF b_fund (no F accessor)
  r.sigma_fund   = std::sqrt(std::max(var_total, 0.0));
  r.b_fund       = agg_exposure(V.exposures(), sleeve_books, c);     // Σ_s c_s Xᵀw_s (B4)
  r.risk_contrib = euler_component_risk(Omega, c, r.sigma_fund);     // RC_s, Σ = σ_fund (R4) — uses Ω (sleeve cov)
  return atx::core::Ok(std::move(r));
}

// Sleeve-return covariance Ω (S×S) from trailing per-sleeve P&L. Reuse combine::pairwise_complete_corr; build cov
// order-fixed (no pairwise_complete_cov exists, §0.6). Ω_st = corr(pnl_s, pnl_t)·σ_s·σ_t. TRAILING window only (R2).
[[nodiscard]] atx::core::Result<atx::core::linalg::MatX>
sleeve_return_cov(std::span<const std::span<const atx::f64>> sleeve_pnl);
```
**Euler additivity (R4):** `euler_component_risk` returns `RC_s = c_s·(Ωc)_s/σ_fund`; the S2-3 test asserts `Σ_s RC_s ==
σ_fund` (≡ `sqrt(cᵀΩc)`) to fp tolerance — the load-bearing risk-attribution proof. **Factored `V` (R6):** `sigma_fund`
goes through `FactorModel::risk` (no dense M×M); the factor/specific split is the `risk(W) − WᵀDW` identity (§0.4). **Note
the two risk views:** `Ω` (S×S, sleeve-return covariance, drives the allocator + component risk) vs the shared `V` (M×M
factored, drives fund factor exposure + total σ) — both consistent at the fund book `W`. **Trailing (R2):**
`sleeve_return_cov` consumes only `≤ t` P&L; the test truncates the P&L panel and asserts `Ω` (hence `c`) is unchanged.

### 4.4 Internal-crossing netting — net book + gross/net turnover + priced benefit (S2-4)

The honest crossing measurement. Sum the sleeve target deltas into the fund net trade; the offsetting flow crosses
internally; price the saving through the **same** cost model (§0.9). **`T_net ≤ T_gross` and `benefit ≥ 0` are asserted
invariants** (R3).

```cpp
// section: fund/netting.hpp  (namespace atx::engine::fund)
struct NetResult {
  std::vector<atx::f64> fund_book;        // W = Σ_s c_s w_s (order-fixed, R3)
  atx::f64 turnover_gross   = 0.0;        // Σ_i Σ_s |c_s·Δw_{s,i}|   (sleeves traded SEPARATELY)
  atx::f64 turnover_net     = 0.0;        // Σ_i |Σ_s c_s·Δw_{s,i}|   ≤ gross (triangle, R3)
  atx::f64 crossing_benefit_bps = 0.0;    // (gross − net) priced through cost — ≥ 0 (R3/R6)
  atx::f64 crossed_fraction = 0.0;        // (gross − net) / gross — the internal-cross rate (a report metric)
};

// Net the sleeve books at one period into the fund order; measure the crossing benefit. w_s = book at t, w_s_prev at t-1.
// PIT-safe (§0.9): a same-timestamp aggregation of already-known sleeve targets — NO future data.
[[nodiscard]] atx::core::Result<NetResult>
net_fund_book(std::span<const std::span<const atx::f64>> sleeve_books,
              std::span<const std::span<const atx::f64>> sleeve_prev,
              std::span<const atx::f64> c, const book::CostInputs& cost)
{
  const atx::usize S = c.size(), M = sleeve_books.empty() ? 0 : sleeve_books[0].size();
  NetResult r; r.fund_book.assign(M, 0.0);
  atx::f64 t_gross = 0.0, t_net = 0.0;
  for (atx::usize i = 0; i < M; ++i) {                              // ascending name (R1)
    atx::f64 net_delta = 0.0, gross_delta = 0.0, w_i = 0.0;
    for (atx::usize s = 0; s < S; ++s) {                           // ascending sleeve (R1)
      atx::f64 prev = sleeve_prev.empty() ? 0.0 : sleeve_prev[s][i];
      atx::f64 d = c[s] * (sleeve_books[s][i] - prev);
      net_delta   += d;                                            // Σ_s c_s Δw_{s,i}
      gross_delta += std::fabs(d);                                 // Σ_s |c_s Δw_{s,i}|
      w_i         += c[s] * sleeve_books[s][i];
    }
    r.fund_book[i] = w_i;
    t_net   += std::fabs(net_delta);                              // |Σ_s c_s Δw_{s,i}|
    t_gross += gross_delta;
  }
  r.turnover_gross = t_gross; r.turnover_net = t_net;             // INVARIANT t_net <= t_gross (R3) — asserted in tests
  r.crossing_benefit_bps = (t_gross - t_net) * cost.round_trip_cost_bps;   // ≥ 0 (R3/R6); convex-impact variant §4.4.1
  r.crossed_fraction = (t_gross > 0.0) ? (t_gross - t_net) / t_gross : 0.0;
  return atx::core::Ok(std::move(r));
}
```
**Invariants (R3):** `turnover_net ≤ turnover_gross` (triangle inequality) and `crossing_benefit_bps ≥ 0` are asserted in
every S2-4 test; a violation is a cost-model bug. **Boundary cases:** two **identical** sleeves ⇒ `crossed_fraction = 0`
(same-sign, no offset), `fund_book = Σc_s·w` ; two exactly **opposite** sleeves with equal capital ⇒ `fund_book = 0`,
`crossed_fraction = 1` (full internal cross). **`#4.4.1` — convex-impact variant:** for a nonlinear impact term
(`a|q|+b|q|^{3/2}`, §1A B1/B2) the benefit is priced on each turnover separately (`cost(T_gross_i) − cost(T_net_i)` per
name) — strictly **larger** than the linear estimate; the linear `round_trip_cost_bps` form is the default, the convex form
a recorded refinement. **Net-after-optimize approximation metric (§0.7):** the driver also reports cross-sleeve position
correlation + the multi-sleeve-name fraction so the controller sees the joint-vs-modular gap.

### 4.5 Meta-book driver — two-pass walk, fund report, attribution-by-sleeve, S1 boundary pin (S2-5)

The capstone. **Pass 1:** each sleeve runs its own `MultiHorizonOptimizer.run` (independent, causal §0.7). **Pass 2:** a
fund-level schedule walk — at period `s`, compute `Ω` from **trailing** (`< s`) sleeve P&L (§0.8/R2), allocate `c[s]`
(§4.2), net the fund book (§4.4), aggregate risk (§4.3), execute the fund first move. Then the `FundReport` +
attribution-by-sleeve (Euler-exact §4.5 / R4).

```cpp
// section: fund/meta_book.hpp  (namespace atx::engine::fund)
struct MetaBookConfig {
  MetaAllocatorConfig alloc;
  atx::usize risk_lookback = 60;        // TRAILING window for Ω / vols (R2; §0.8)
};

struct SleeveAttribution {
  std::vector<atx::f64> return_contrib;  // c_s·R_s ; Σ = R_fund (R4)
  std::vector<atx::f64> risk_contrib;    // c_s·(Ωc)_s/σ_fund ; Σ = σ_fund (R4)
  std::vector<atx::f64> crossing_credit; // crossing benefit pro-rata by contributed crossed volume (B3; Σ = total)
};

struct FundReport {                      // built FRESH from the netted series (§0.5) — reuses combine::compute_metrics
  std::vector<atx::f64> equity_curve, gross_leverage, net_exposure, turnover_net, turnover_gross, crossing_benefit_bps;
  combine::AlphaMetrics fund_metrics{};  // the ONE Sharpe convention (§0.5)
  atx::f64 effective_bets = 0.0;         // Meucci N_Ent over Ω (A5 diversification gate)
  SleeveAttribution attribution{};
};

struct MetaBookResult {
  std::vector<std::vector<atx::f64>> fund_books;   // netted fund book per period (the MPC first move per rebalance)
  std::vector<CapitalWeights>        capital;      // c_s per period (TRAILING-allocated)
  std::vector<risk::MultiHorizonResult> sleeve_results;  // pass-1 per-sleeve books (for attribution + sub-reports)
  FundReport report;
};

class MetaBook {
public:
  MetaBookConfig cfg;
  std::vector<Sleeve> sleeves;

  // sources_at(s_idx, period) -> the HorizonSources for sleeve s_idx at the period. model -> the SHARED FactorModel.
  [[nodiscard]] atx::core::Result<MetaBookResult>
  run(const risk::RebalanceSchedule& sched,
      const std::function<risk::HorizonSources(atx::usize sleeve, atx::usize period)>& sources_at,
      const std::function<const risk::FactorModel&(atx::usize period)>& model_at,
      const book::CostInputs& cost) const
  {
    MetaBookResult out;
    // PASS 1: each sleeve runs independently (net-after-optimize §0.7) — causal, blind to the others (R2).
    for (atx::usize j = 0; j < sleeves.size(); ++j) {
      ATX_TRY(risk::MultiHorizonResult sr,
              sleeves[j].run(sched, [&](atx::usize p){ return sources_at(j, p); }, model_at, cost));
      out.sleeve_results.push_back(std::move(sr));
    }
    // PASS 2: fund overlay — trailing allocate, net, first move. period s uses sleeve P&L from < s ONLY (R2).
    std::vector<std::vector<atx::f64>> prev_books;                 // sleeve books at s-1 (for netting deltas)
    for (atx::usize s = 0; s < sched.periods.size(); ++s) {
      atx::core::linalg::MatX Omega = trailing_sleeve_cov(out.sleeve_results, s, cfg.risk_lookback);  // < s (R2/§0.8)
      ATX_TRY(CapitalWeights cw, MetaAllocator{cfg.alloc}.allocate(Omega, trailing_vol(...), sleeve_caps()));
      std::vector<std::span<const atx::f64>> books_s = period_books(out.sleeve_results, s);   // w_s at s
      ATX_TRY(NetResult nr, net_fund_book(books_s, prev_spans(prev_books), cw.c, cost));      // §4.4
      out.fund_books.push_back(nr.fund_book);
      out.capital.push_back(std::move(cw));
      record_fund_period(out.report, nr, /*pnl via model_at(period) returns*/);              // equity, turnover, benefit
      prev_books = to_vectors(books_s);
    }
    out.report.attribution    = attribute_by_sleeve(out, sched, cost);   // Euler-exact (R4) + crossing pro-rata (B3)
    out.report.fund_metrics   = combine::compute_metrics(/*fund pnl, fund positions*/);       // ONE Sharpe (§0.5)
    out.report.effective_bets = meucci_effective_bets(/*final Ω, c*/);                         // A5 gate
    return atx::core::Ok(std::move(out));
  }
};
```
**Boundary pin (R7/§0.1):** one sleeve + `cfg.alloc` yielding `c=[1.0]` (single-sleeve ERC is trivially `1`) + no
vol-target (`target_vol=0`, `fractional_kelly=1`) ⇒ netting is a no-op (`net == gross`, `W = w_0`), and `fund_books[s] ==
sleeve_results[0].books[s]` **byte-identical**. **The mandatory regression test** — it pins the meta-book to the proven S1
layer. **Determinism (R1):** pass-1 sleeves are deterministic (S1); pass-2 is order-fixed (ascending sleeve/name, fixed-
iter allocate, order-fixed Ω + netting + attribution). **Look-ahead (R2):** `trailing_sleeve_cov(..., s, lookback)` reads
sleeve P&L strictly `< s`; truncating the panel after period `t` leaves all fund books `≤ t` byte-identical — the
truncation-invariance test. **Attribution (R4):** `attribute_by_sleeve` returns per-sleeve return + risk contributions that
**sum to** the fund return and `σ_fund` (asserted), with the crossing benefit allocated pro-rata by contributed crossed
volume (B3); `s=0` no-history allocation uses the inverse-capacity/equal fallback (§0.8).

#### 4.5.1 — Integration, determinism + look-ahead + attribution proofs, bench, close (S2-5)

- **Integration test** (`fund_meta_book_integration_test.cpp`): data → shared S8 `V` → N sleeves (each its own S1
  optimizer over its member subset) → trailing risk budget → net fund book → fund report, asserting **all gates
  simultaneously:** R1 (two builds byte-identical fund book schedule + digest), R2 (truncation-invariance at the fund fit
  boundary — the trailing risk budget reads no future), R3 (`T_net≤T_gross`, `benefit≥0` every period), R4 (per-sleeve
  return + risk contributions sum to fund return + σ_fund), R7 (the one-sleeve config reduces to S1 bit-for-bit).
- **Bench** (`meta_book_bench.cpp`): funds/sec across `(S∈{2,4,8,16} sleeves, N∈{500,1000,3000}, K factors)`; the ERC
  solve cost vs S; the net-after-optimize approximation (cross-sleeve position correlation + multi-sleeve-name fraction)
  vs a joint-solve reference on a small fixture (the §0.7 gap, S8 lift); the O(S²) `Ω` + O(N·S) netting profile.
- **Close ceremony:** residuals → ROADMAP backlog (the joint mega-QP §0.7; library-driven sleeve membership/clustering
  §0.2 → S4/S8; the `core::linalg::risk_budget` lift §2.1; the convex-impact crossing variant §4.4.1; the
  `MultiPeriodResult`/`MultiHorizonResult` unification §0.5); ROADMAP status table `⏳ → ✅ <sha>`; `Last reviewed` bump;
  "What S2 proves" + "Next sprint priorities" baton; user reference `sprint2.md`; merge (`--no-ff`).

---

## Exit criteria

- A `Sleeve` wraps a `risk::MultiHorizonOptimizer` over its own `AlphaId` member subset + universe/family tags (the
  library cannot slice §0.2), producing its per-period book series — and a one-sleeve config delegates transparently to S1.
- The `MetaAllocator` sets cross-sleeve capital weights `c_s` from a **trailing** risk budget (ERC log-barrier / HRP /
  inverse-vol) + a portfolio-of-books fractional-Kelly fund leverage + a gross-clipped vol-target, fixed-iteration and
  per-sleeve-capacity-boxed; the ERC optimum achieves `RC_s = b_s·σ_fund` within tolerance (R5).
- The cross-sleeve risk model aggregates factor exposure `b_fund = Σ c_s·Xᵀw_s` and fund risk over the **shared**
  `FactorModel` with the `risk(W) − WᵀDW` factor/specific split (no `F` accessor, no dense M×M, §0.4/R6); component risk
  `RC_s` sums to `σ_fund` (Euler, R4).
- The netting produces `W = Σ c_s·w_s` with `T_net ≤ T_gross` and a crossing benefit `≥ 0` priced through the **same**
  `book::CostInputs` — a negative benefit is a flagged cost-model bug, never shipped (R3/R6).
- The fund report carries attribution-by-sleeve whose per-sleeve return + risk contributions **sum to** the fund return +
  `σ_fund` (R4), the crossing benefit fully allocated; the fund Sharpe uses `combine::compute_metrics` (the ONE convention).
- **The boundary pin holds:** one sleeve + full capital + no vol-target ⇒ the fund book schedule is **byte-identical** to
  that sleeve's `MultiHorizonOptimizer.run` (R7).
- The integration test passes all five gates simultaneously; the bench reports funds/sec + the net-after-optimize
  approximation error + the O(S²)/O(N·S) profile.
- `/W4 /permissive- /WX` (`ATX_WERROR=ON`) clean — **no `/fp:precise` flag** (determinism order-fixed, §0.10); one test
  file per unit; full engine suite stays green per unit.

## Invariants this sprint must prove

All eight carried-forward invariants (ROADMAP "Carried-forward invariants"), with four explicit stress points:
1. **Determinism (R1)** — the ERC/risk-budget solve is the new fixed-iteration kernel; it must run a fixed count (no
   residual exit), and the full driver's two-builds digest must be byte-identical (order-fixed, not `/fp:precise`). No RNG.
2. **No look-ahead (R2)** — the risk budget is **trailing** (`c_s` reads only `≤ t` sleeve P&L); the netting is a
   same-timestamp aggregation; truncation-invariance at the fund fit boundary is the test. **The central S2 trap.**
3. **Attribution additivity (R4)** — per-sleeve return + risk contributions sum exactly (Euler) to the fund; the crossing
   benefit is fully allocated. Without this, "attribution-by-sleeve" is a fiction.
4. **Reduction to S1 (R7)** — the meta-book is pinned bit-for-bit to the proven single-sleeve optimizer on the boundary;
   without this, S2 is not demonstrably a superset of the layer it composes.

## Dependencies

- **Upstream (`p1` + `p2` S1, assumed merged):** S1 (`MultiHorizonOptimizer` + `MultiHorizonConfig`/`Result` +
  `HorizonSources` + `SignalHorizon`; the shared `FactorModel` accessors `risk`/`apply`/`exposures`/`specific_var`), S8
  (the cleaned `FactorModel V` the sleeves + fund trade), S7 (`book::{size_book, effective_breadth, AllocationConfig,
  CostInputs}`; `RebalanceSchedule`), S6 (calibrated κ), `p1` S1 (`combine::compute_metrics`, `eval::*`, reused verbatim),
  `combine::{CombinedSignalSource, pairwise_complete_corr}`, `library::{Library, AlphaId, LifecycleState}`.
- **atx-core (already available — reuse, no edge):** `MatX`/`VecX`, `linalg` Cholesky/`solve_spd` (for the ERC Newton step),
  `Result`/`Status`/`Err(ErrorCode, msg)`.
- **atx-core (Pattern B — new edge raised by this sprint):**

| S2 unit | Needs from `atx-core` | Engine-side fallback (shippable now) |
|---|---|---|
| S2.2 | **L7 `risk_budget`** — deterministic convex risk-budget / ERC solve (log-barrier CCD/Newton, generic degree-1 risk measure) | engine-local fixed-iteration `fund::erc_log_barrier` (CCD + Newton cross-check); N = #sleeves small ⇒ a fixed-count solve is exact + cheap |

## Explicitly NOT in this sprint

- **No joint mega-QP over all sleeves** — S2 ships **net-after-optimize** (independent sleeves, ex-post cross, §0.7); the
  globally-optimal joint solve over all sleeves' alphas is the recorded lift → S8.
- **No library-driven sleeve membership / clustering** — a `Sleeve` carries a **caller-supplied** `AlphaId` member list +
  tags (§0.2); auto-clustering the library into sleeves (universe × family discovery) is a recorded residual → S4/S8.
- **No live / streaming / optimal execution** — S2 emits target fund weights/turnover + the measured crossing benefit;
  routing the netted order is the broker's job (the meta-book *measures* the saving in-sim, it does not trade).
- **No new general-purpose primitive in the engine** (anti-roadmap #7) — the ERC solver is a recorded `core::linalg`
  request (Pattern B), engine-side fallback only until the L7 kernel lands.
- **No deep-learning / alt-data signals** — sleeves wrap whatever `ISignalSource`s they are handed (formulaic/ML today);
  NN families are **S5**, alt-data is **S6** (they hang new sleeves off this layer).
- **No tail-risk / CVaR risk budget as the default** — the Gaussian-vol ERC is the default; the generic-risk-measure RB
  (CVaR/ES via the same log-barrier, Roncalli 2013) is a recorded config extension.

## Baton → next

S2 hands the `p2` frontier a **fund-scale measurement instrument**: a meta-book that nets N constrained-multi-horizon
sleeves into one risk-budgeted fund with honest internal crossing and Euler-exact attribution. **S5** (deep-learning
alphas) and **S6** (alt-data / multi-asset) each ship as **new sleeves** dropped into the meta-book — their signal families
are robustness-gated then risk-budgeted alongside the formulaic/ML sleeves. **S8** (the robust-alpha capstone) routes the
**whole library** through the meta-book as the portfolio-scale lens — the fund-level report + attribution is what the
terminal lockbox evaluation reads. With S2 closed, the `p2` book-construction track (S1 → S2) is complete; the alpha-depth
track (S3 → S4) feeds it the robust signals every sleeve trades.

---

## References

> All primary sources, tagged during the 2026-06-13 two-stream research pass (allocation theory + meta-book mechanics)
> with per-citation VERIFIED/UNVERIFIED confirmation. Bibliographic anchors (DOI / arXiv / venue) confirmed against
> publisher + arXiv + authors' pages where marked. **[LB]** = load-bearing (a unit's design rests on it); **[S]** =
> supporting; **[X]** = a circulated claim that **failed/lacked verification** — recorded so the doc does not propagate it
> as fact.

### Track A — capital & risk allocation

1. **[LB]** S. Maillard, T. Roncalli, J. Teïletche. *The Properties of Equally Weighted Risk Contribution Portfolios.*
   Journal of Portfolio Management **36**(4):60–70, 2010. DOI: 10.3905/jpm.2010.36.4.060. **[VERIFIED]** *(ERC properties,
   `σ_MV ≤ σ_ERC ≤ σ_EW`, inverse-vol equicorrelation form — the §4.2 default allocator.)*
2. **[LB]** T. Roncalli. *Introduction to Risk Parity and Budgeting.* Chapman & Hall/CRC, 2013. ISBN 9781482207156.
   arXiv:1403.1889. **[VERIFIED]** *(RB general theory `RC_i=b_i·R(w)`, log-barrier, uniqueness for `b>0`, generic
   degree-1 risk measure — §4.2/§4.3.)*
3. **[S]** F. Spinu. *An Algorithm for Computing Risk Parity Weights.* SSRN 2297383, 2013. **[VERIFIED]** *(the convex
   log-barrier model + Newton solve — the §4.2 `erc_log_barrier` kernel.)*
4. **[S]** T. Griveau-Billion, J.-C. Richard, T. Roncalli. *A Fast Algorithm for Computing High-Dimensional Risk Parity
   Portfolios.* arXiv:1311.4057, 2013. **[VERIFIED]** *(cyclical coordinate descent convergence — the §4.2 CCD path + R8
   cross-check.)*
5. **[LB]** M. López de Prado. *Building Diversified Portfolios that Outperform Out of Sample.* Journal of Portfolio
   Management **42**(4):59–69, 2016. SSRN 2708678. **[VERIFIED]** *(Hierarchical Risk Parity — the estimation-error-robust
   §4.2 alternative; corr-distance `d=sqrt(0.5(1−ρ))`.)*
6. **[S]** M. López de Prado. *Machine Learning for Asset Managers.* Cambridge University Press, 2020. **[VERIFIED — book;
   NCO method attribution standard]** *(Nested Clustered Optimization — the §1A A2 hybrid upgrade.)*
7. **[LB]** J. L. Kelly. *A New Interpretation of Information Rate.* Bell System Technical Journal **35**(4):917–926, 1956.
   DOI: 10.1002/j.1538-7305.1956.tb03809.x. **[VERIFIED]** *(the growth-optimal criterion; `f*=Σ⁻¹μ` — §4.2 fund leverage.)*
8. **[LB]** E. O. Thorp. *The Kelly Criterion in Blackjack, Sports Betting, and the Stock Market.* In *Handbook of Asset
   and Liability Management* Vol. 1, North-Holland, 2006, pp. 385–428. **[VERIFIED]** *(fractional Kelly `c·Σ⁻¹μ`,
   estimation-error robustness — §4.2 / A3.)*
9. **[S]** L. C. MacLean, E. O. Thorp, W. T. Ziemba (eds.). *The Kelly Capital Growth Investment Criterion: Theory and
   Practice.* World Scientific, 2011. **[VERIFIED — standard anthology]** *(the fractional-Kelly theory+practice corpus.)*
10. **[S]** R. C. Grinold. *The Fundamental Law of Active Management.* Journal of Portfolio Management **15**(3):30–37, 1989.
    **[VERIFIED]** *(`IR=IC·sqrt(BR)` — the §1A A4 objective rationale.)*
11. **[LB]** R. C. Grinold, R. N. Kahn. *Active Portfolio Management*, 2nd ed. McGraw-Hill, 2000. ISBN 0070248826.
    **[VERIFIED]** *(portfolio-of-portfolios `IR_fund²=aᵀR⁻¹a`; marginal-contribution-to-risk — §4.3/§4.5 attribution.)*
12. **[X]** R. Clarke, H. de Silva, S. Thorley. *Portfolio Constraints and the Fundamental Law of Active Management.*
    Financial Analysts Journal **58**(5):48–66, 2002. **[UNVERIFIED — not re-extracted this pass]** *(transfer coefficient
    `TC`; cite with caution.)*
13. **[S]** A. Meucci. *Managing Diversification.* Risk **22**(5):74–79, 2009. SSRN 1358533. **[VERIFIED]** *(effective
    number of bets `N_Ent=exp(−Σp ln p)` — the §4.5 diversification gate.)*
14. **[S]** Y. Choueifaty, Y. Coignard. *Toward Maximum Diversification.* Journal of Portfolio Management **35**(1):40–51,
    2008. **[VERIFIED]** *(diversification ratio `DR=(wᵀσ)/σ(w)`, MDP, `N_DR=DR²` — the §4.5 cheap gate.)*
15. **[S]** A. Moreira, T. Muir. *Volatility-Managed Portfolios.* Journal of Finance **72**(4):1611–1644, 2017.
    DOI: 10.1111/jofi.12513. NBER w22208. **[VERIFIED]** *(per-sleeve `∝1/σ̂²` vol targeting — the §4.2 fund-vol scale.)*

### Track B — meta-book mechanics

16. **[LB]** Z. Kakushadze. *101 Formulaic Alphas.* Wilmott **2016**(84):72–81. arXiv:1601.00991. **[VERIFIED — quote
    confirmed]** *("automatic internal crossing of trades" → execution-cost saving — the §4.4 netting motivation.)*
17. **[S]** I. Tulchinsky et al. (eds.). *Finding Alphas: A Quantitative Approach to Building Trading Strategies*, 2nd ed.
    Wiley, 2019 (Ch. 7 "Turnover" — crossing effects, turnover control). **[VERIFIED — book + chapter]** *(the
    mega-alpha-crossing cost mechanics, §4.4.)*
18. **[LB]** R. Almgren, N. Chriss. *Optimal Execution of Portfolio Transactions.* Journal of Risk **3**(2):5–39, 2000.
    **[VERIFIED venue]**; closed-form `E(x)/V(x)` is the **canonical textbook form** but **[UNVERIFIED at exact primary
    text this pass]**. *(the impact term the crossing benefit is priced against — §4.4.1.)*
19. **[LB]** S. Boyd, E. Busseti, S. Diamond, R. Kahn, K. Koh, P. Nystrup, J. Speth. *Multi-Period Trading via Convex
    Optimization.* Foundations and Trends in Optimization **3**(1):1–76, 2017. DOI: 10.1561/2400000023. arXiv:1705.00109.
    **[VERIFIED]**; exact per-asset cost coefficients **[UNVERIFIED]**. *(separable-across-assets cost → the net-after-
    optimize ≈ joint-optimize argument, §0.7/§1A B2.)*
20. **[S]** P. N. Kolm, G. Ritter. *Modern Perspectives on Reinforcement Learning in Finance.* SSRN 3449401, 2019.
    **[VERIFIED]** *(cost-structure-governs-decoupling — the §0.7 separability argument.)*
21. **[LB]** G. P. Brinson, L. R. Hood, G. L. Beebower. *Determinants of Portfolio Performance.* Financial Analysts Journal
    **42**(4):39–44, 1986. DOI: 10.2469/faj.v42.n4.39. **[VERIFIED]** *(allocation/selection/interaction — §4.5.)*
22. **[S]** G. P. Brinson, N. Fachler. *Measuring Non-US Equity Portfolio Performance.* Journal of Portfolio Management
    **11**(3):73–76, 1985. **[VERIFIED]** *(benchmark-relative allocation — the preferred §4.5 form.)*
23. **[S]** A. Meucci. *Risk Contributions from Generic User-Defined Factors.* Risk, 2007. SSRN 930034. **[VERIFIED]**
    *(Euler decomposition of any degree-1 risk measure → marginal/component/incremental VaR — §4.3.)*
24. **[S]** *Portfolio Performance Attribution via Shapley Value.* arXiv:2102.05799, 2021. **[VERIFIED — exists]** *(the
    marginal/Shapley crossing-benefit attribution mode, §4.5; default is pro-rata.)*

> **Verification caveats carried from the research pass:** (a) **pod-shop numerics** (drawdown stops, gross-leverage bands)
> are **trade-press / industry lore — [X] not citable**; the rigorous vol-targeting anchor is Moreira-Muir (15). (b) The
> Clarke-de Silva-Thorley transfer coefficient (12) was **not re-extracted** — verify before quoting. (c) The Almgren-Chriss
> discrete closed form (18) was **not re-extracted from the primary PDF**; the equations are the universally-cited textbook
> form — cite the primary directly before publishing. (d) The RenTech "single unified re-optimized book" is a **secondary-
> source characterization** of a secretive firm, used only as design narrative for the §0.7 fork. (e) No single canonical
> *crossing-network microstructure* paper was pinned — the Boyd separable-cost convex framing (19) is the strongest formal
> anchor for the netting argument and is sufficient for S2.
