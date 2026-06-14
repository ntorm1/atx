# Module p2 — atx-engine v3: The Robust Alpha Engine — Multi-Horizon Backtesting & Provable Alpha Discovery at Scale (`p2`)

**Last reviewed:** 2026-06-14
**Started:** planned; no sprint open yet (assumes `p1` S1–S8 closed — S7/S8 treated as DONE per the user's kickoff directive).
**Source:** distills [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
+ [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
+ [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
+ [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md),
reviewed against the as-built `p1` factory + DSL/VM + library + operating book (S1–S8) and the frozen `p1` invariants.
The `p2` frontier is the **depth** `p1` mass-produced but never proved: `p1` built a *working* alpha factory (a 65-op DSL,
a genetic miner, a deflated-gate, a persistent library); `p2` makes that factory **express more, search smarter, and
prove the alpha it admits is actually robust** — across regimes, walk-forward, a sealed lockbox, and net of calibrated
cost. Governed by [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md).
**Goal:** Take `p1`'s *industrialized-but-shallow* alpha factory and turn it into a **robust alpha engine** — a deeper
expression substrate (richer, correctly-typed, fully-mutable DSL), a smarter genetic search (multi-objective Pareto
selection, behavioral diversity, cost-aware mining), and an end-to-end **signal-generation pipeline that proves the
admitted library holds real, survivable alpha** (synthetic-recovery · regime/walk-forward · final lockbox ·
capacity/cost), constructed into a multi-horizon backtested book — **without weakening a single one of `p0`/`p1`'s
correctness invariants.** **Live execution is out of scope: it is delegated to broker execution algorithms.** `p2`
proves we can *find* robust alpha; *trading* it is the broker's job.

---

## What changes from p1 → p2

`p0` answered *"can I trust one backtest?"* (the skeleton: data → DSL → backtest → combine → risk).
`p1` answered *"can I operate a factory?"* — mass discovery (S3 GA + S5 ML), a living library (S4), honest gating at
scale (S1 DSR/PBO/CPCV), calibrated cost + capacity (S6), vendor-grade covariance (S8), and **one** deterministic
multi-period operating book (S7).

`p2` answers *"can I prove the factory finds **robust** alpha?"* — **how much can the language express** (a deeper DSL:
regression-residual neutralization, the BRAIN time-series superset, a richer datafield family, a fully-mutable operator
space), **how smart is the search** (multi-objective Pareto selection over return/diversification/robustness/cost,
behavioral novelty, cost-aware mining — not single-objective hill-climbing on a turnover-floor proxy), and **how do we
know the admitted alpha is real** (a robustness battery: synthetic-alpha recovery, regime/walk-forward survival, a
never-touched final lockbox, capacity/cost survival). The differentiator moves from *"a believable backtest,
industrialized"* to *"a believable backtest, industrialized, **and proven to discover robust, survivable alpha.**"*

| Dimension | `p1` baseline (S1–S8) | **`p2` v3 target** |
|---|---|---|
| DSL expressiveness | 65 ops; demean-by-group neutralize only; `op_swap` **disabled** (VM-corruption defect narrows the mutable space) | **deepened substrate** — regression-residual neutralization, BRAIN-superset `ts_*` ops, richer datafield family, **`op_swap` fixed + re-enabled**, grammar-typed (valid-by-construction) generation (S3) |
| Genetic search | single-objective `raw = wq×diversify×robust`; structural-hash novelty; turnover-floor cost proxy | **multi-objective Pareto/NSGA-II** over {return, diversification, robustness, cost}; **behavioral/phenotypic** diversity; **cost-aware mining** at target AUM (S4) |
| Robustness proof | a pure-noise panel admits ~0 vs a real-signal panel (the S3 non-vacuousness check) | + **synthetic-alpha recovery**, **regime/walk-forward survival**, a **sealed final lockbox**, **capacity/cost survival** — the full robust-alpha battery (S4 + S8) |
| Signal families | formulaic GA (S3) + learned ridge/GBT/HMM (S5) | + **deep-learning sequence alphas** (temporal conv / GRU-lite / attention-lite / autoencoder factors), deflated-gated (S5) |
| Book construction | single-period proximal solve, chained receding-horizon (S7) | **true constrained multi-horizon convex program** to measure admitted alpha at portfolio scale (S1) + **N-sleeve meta-book** (S2) |
| Data | price-volume + classifications, US-equity daily bars; **one fixed in-memory `Panel` per run**, everything else computed internally from price | **bring-your-own-data**: a PIT-versioned **dataset catalog** — **price the only required input**, with **features / external signals / factor-model / reference** as optional typed plug-ins (lineage-tracked); alt-data becomes *a plugged dataset, not special-cased* (S6); multi-asset/region + intraday universe is structural breadth (S9) |
| Compute | deterministic single-box multicore (S2) | **deterministic distributed/cluster** for mass mining (order-fixed cross-machine merge; digest == single-box) (S7) |
| Execution | √-impact fill model in `ExecutionSimulator` (p0), used to **cost-gate** admission | **unchanged** — the sim's calibrated cost still gates admission; *live/optimal execution is delegated to broker algorithms (out of scope)* |
| Identity | a believable backtest, industrialized | a believable backtest, industrialized, **and proven to discover robust, survivable alpha** |

When scope creep argues, this table, the carried-forward invariants (below), and the v3 anti-roadmap are where it goes
to die.

---

## Companion docs index

| Doc | Type | Covers |
|---|---|---|
| [`sprint-1-multi-horizon-optimization-implementation-plan.md`](sprint-1-multi-horizon-optimization-implementation-plan.md) | implementation plan | **S1** frozen *how* — constraint algebra, fixed-iteration QP/ADMM, multi-horizon forecast trajectory, Gârleanu-Pedersen aim portfolio, integration with S7/S8. The book-construction track's per-unit plan. |
| [`sprint-2-multi-strategy-meta-book-implementation-plan.md`](sprint-2-multi-strategy-meta-book-implementation-plan.md) | implementation plan | **S2** frozen *how* — the `Sleeve` abstraction (wraps an S1 optimizer over a library subset), the cross-sleeve risk-budget meta-allocator (ERC/HRP + portfolio-of-books fractional-Kelly), shared-`V` cross-sleeve risk aggregation, internal trade crossing/netting (priced in-sim), Euler-exact attribution-by-sleeve. Pinned bit-for-bit to S1 on the one-sleeve boundary. The book-construction track's second per-unit plan. |
| [`sprint-4-genetic-search-robust-signal-pipeline-implementation-plan.md`](sprint-4-genetic-search-robust-signal-pipeline-implementation-plan.md) | implementation plan | **S4** frozen *how* — the scalar→vector objective swap at the four `search_driver.cpp` seams, engine-local NSGA-II (`pareto.hpp`, no atx-core primitive), behavioral/phenotypic novelty (signal-profile distance + archive), cost-aware mining (`cost_aware.hpp`/`capacity.hpp` routed into the objective), the robustness battery (synthetic recovery + vol-regime/walk-forward survival + sealed lockbox), and the E2E mine→multi-objective gate→admit→combine→book pipeline. Boundary-pinned bit-for-bit to the `p1` scalar miner; aligned to S3's enabled `op_swap` + grammar generation. The alpha-depth track's second per-unit plan. |
| [`sprint-5-deep-learning-sequence-alphas-implementation-plan.md`](sprint-5-deep-learning-sequence-alphas-implementation-plan.md) | implementation plan | **S5** frozen *how* — the deterministic CPU-only NN substrate (pluggable `Module`/`Layer`/`Optimizer`/`Loss`/`Trainer`, hand-written backward passes, no autodiff/GPU), the PIT trailing-window sequence-feature builder over `learn::FeatureMatrix`, four tiny architectures (TCN default + GRU-lite + attention-lite + autoencoder factor), the `LearnedModel`/`ModelKind` extension + lookback live adapter, and deflation+PBO+S4-robustness gating with the architecture/seed sweep in the trial count. Pinned bit-for-bit to the `p1` S5 linear learner + atx-core PCA on the linear boundary. The signal-family track's deep-learning plan. |
| [`sprint-7-distributed-scale-out-compute-implementation-plan.md`](sprint-7-distributed-scale-out-compute-implementation-plan.md) | implementation plan | **S7** frozen *how* — the substrate-agnostic `IExecutor` seam, cross-platform multi-process shared-memory substrate (Win32 + POSIX), deterministic heterogeneous-cost scheduler, library partition-and-merge, the five-path digest-invariance proof. Reframed (per kickoff) to **single-box multi-process + scale-out-ready**: ships threads + processes on one machine; the N-node network transport is the recorded atx-core L4 lift. |
| [`sprint-6-data-signal-feature-reference-abstraction-layer.md`](sprint-6-data-signal-feature-reference-abstraction-layer.md) | sprint spec | **S6** *what* — the **BYO-data abstraction layer**: a typed PIT-versioned `data::Dataset` (Price/Feature/Signal/Reference roles) + a `DatasetCatalog` registry (schema/lineage/as-of versioning) + a `DataContext` facade that replaces the bare `alpha::Panel` into the factory/book/fund. **Price is the only required input**; features/signals/factor-model/reference are optional plug-ins with price-only fallbacks. Subsumes alt-data; external precomputed signals enter both as ML features and as gated library admissions; full BYO factor model (X/F/D) + reference datasets. |
| [`sprint-6-data-signal-feature-reference-abstraction-layer-implementation-plan.md`](sprint-6-data-signal-feature-reference-abstraction-layer-implementation-plan.md) | implementation plan | **S6** frozen *how* (written ahead of kickoff) — the `atx::engine::data::` layer retrofit: `Dataset`/`DatasetSchema`/`DatasetCatalog`/`DataContext` + the ingestion-alignment-PIT rail + differential-pinned adapters lowering into `alpha::Panel` (== `with_datafields`), `learn::FeatureMatrix`, `risk::FactorModel::create`, and gated `library::Library` admit. §0 as-built recon (exact signatures + the CMake source-registration fix), the resolved forks (`FactorModelArtifact` ∈ `data::`; price-defines-canonical-axis alignment), and the price-only boundary pin (`DataContext`→`BookPipeline` digest ≡ legacy). |

**Pending (created at each sprint kickoff/close):**
- `sprint-3-alpha-dsl-expression-substrate.md` and `sprint-4-genetic-search-robust-signal-pipeline.md` — the **two new
  sprint specs** (the *what*), written alongside this re-theme; the per-unit implementation plans (the *how*) freeze at
  each sprint's kickoff (NOT now).
- `sprint-6-data-signal-feature-reference-abstraction-layer.md` — the **S6 sprint spec** (the *what*), written
  alongside this re-theme (the BYO-data abstraction layer); its per-unit implementation plan freezes at kickoff (NOT now).
- `sprint-{N}-<theme>.md` — sprint spec (the *what*) for the remaining sprints (S5, S8, S9), written at their
  kickoffs. **For S1 and S2, the embedded ROADMAP section below + the existing impl-plan are the spec.**
- `sprint-{N}-<theme>-implementation-plan.md` — frozen per-unit *how*, written at each sprint kickoff (NOT now).
- `sprint-{N}-progress.md` — sprint ledgers, opened at kickoff (sub-sprints `Sn-a/Sn-b` if a sprint exceeds ~7 units,
  per the `p0` 4a/4b/4c precedent).
- `sprintN.md` — user reference, written at each sprint close.

**Sprint/module discipline:** unchanged — [`../docs/sprint.md`](../docs/sprint.md),
[`../docs/module.md`](../docs/module.md), [`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Unit numbering:** `p2` sprints are **S1…S8**; ledger units are **`S{n}-{m}`** (e.g. `S3-0` marker, `S3-1`…). These
labels are namespaced by `p2/`; when cross-referencing `p1` (which also uses S1…S8), qualify as **"p2 S3"** vs
**"p1 S3"**. Renumber-don't-reuse on carry-forward (`sprint.md` §"Carry-forward"). **Note:** the `p2` S3/S4 slots were
re-pointed in this re-theme — they previously held *live-execution* sprints (now out of scope), and now hold the two
alpha-depth sprints. S1 (optimizer) and S7 (distributed) keep their numbers so their frozen impl-plan docs stay valid.

---

## Sibling / upstream modules

- **Upstream:** `atx-core` stdlib + `atx-tsdb`. The engine remains a pure **consumer** of atx-core L0–L9 and adds no
  general-purpose primitives. `p2` leans hardest on **L6** (`cross_section`/`rolling` — the widened DSL kernels, S3),
  **L7** (`linalg`/`solve`/`regression` — a real **QP/ADMM** solver for S1, the cross-sectional **residualizer** for
  S3, and **deterministic NN primitives** for S5), **L4** (`concurrent` — extended from a single-box pool to a
  **multi-node deterministic work distributor**, S7), and **L9** (`frame` — PIT as-of/effective-date versioning +
  dataset lineage, the `DatasetCatalog` backbone, S6). Every new numeric/infra need is an **atx-core request raised as a cross-module edge here** (Pattern B,
  table below), never built inside the engine.
- **Sibling:** `p1` — atx-engine v2, the alpha factory + operating book. See [`../p1/`](../p1/). `p2` builds strictly
  **on top of** the closed `p1` seams (`alpha::{Engine, Program, registry, oracle}`,
  `factory::{SearchDriver, Factory, ResearchDriver, fitness, OpCatalog}`, `library::Library`,
  `combine::{AlphaCombiner, CombinedSignalSource}`, `eval::{deflated_sharpe, cpcv, pbo}`,
  `risk::{PortfolioOptimizer, MultiPeriodOptimizer, FactorModel}`, `book::{BookPipeline, BookReport, allocation}`,
  `cost::*`, `parallel::DetPool`) and must not re-duplicate any of them. **S3 deepens `alpha::` + `factory::OpCatalog`;
  S4 deepens `factory::{SearchDriver, fitness}`.**
- **Downstream sibling:** `p3` — atx-engine v4, real-data validation & robustness verification. See [`../p3-impl/`](../p3-impl/).
  `p3` runs the `p2` mine→prove pipeline on **real US equity data** (databento OHLCV ⋈ security-master corporate actions),
  building the real-data ingestion adapters (corporate actions, total-return, universe) **on top of** the `p2` S6 BYO-data
  layer + S4 robustness battery. It adds **no** new alpha-discovery capability — it hardens the real-data seam and measures honestly.

---

## Strategic positioning (v3)

`p0` earned the right to be *believed*; `p1` earned the right to be *productive*; `p2` earns the right to be
*trusted to find robust alpha*. The identity shifts from **"a deterministic bias-free alpha factory"** to **"a
deterministic bias-free alpha factory that is *proven* to discover robust, survivable alpha."** The distinction is not
*more* alphas — `p1` already mass-produces them — it is **depth and proof**: a language rich enough to express the real
signals, a search smart enough to find non-redundant ones, and a validation regime stringent enough that what survives
generalizes out-of-sample, out-of-regime, past a sealed lockbox, and net of cost. What we do *with* the proven alpha
(size it, route child orders, touch a venue) is the broker's execution layer — out of scope here.

| Capability | RenTech (Medallion) | WorldQuant (BRAIN) | **atx-engine v3 target** |
|---|---|---|---|
| Expression language | proprietary statistical R&D stack | "Fast Expression" DSL, 100s of operators (§3.3) | **deepened DSL** — regression-residual neutralize, BRAIN-superset `ts_*`, richer datafields, fully-mutable op space (S3) |
| Discovery search | in-house statistical R&D; HMM heritage (§4.2) | formulaic mining 19→10⁷ alphas; fitness+corr gate (§4.1/§6.1) | **multi-objective Pareto GA** + behavioral diversity + cost-aware mining (S4) |
| Robustness proof | OOS discipline, "trust slowly" (§7) | fitness + correlation + IS/OOS gating | **synthetic recovery · regime/walk-forward · sealed lockbox · capacity/cost** (S4 + S8) |
| Signals | statistical ensemble; HMM/noisy-channel heritage (§4.2) | formulaic + ML; 125k+ data fields (§8) | GA + ML + **deep-learning sequence alphas**, deflated-gated (S5) |
| Book construction | one unified model, re-optimized several times/hour (§6.1) | bounded-regression / O(N) mega-alpha (§6.1/§6.2) | **constrained multi-horizon convex program** over S8's cleaned `V` + N-sleeve meta-book (S1/S2) |
| Data | petabyte PIT warehouse; obsessive cleaning (§6.2) | 125,000+ PIT data fields, 17 regions (§8) | **BYO dataset catalog** — price-required; features/signals/factor-model/reference optional, PIT-versioned, lineage-tracked (S6); multi-asset/region + intraday universe (S9) |
| Compute | big iron, re-optimize hourly (§6.1) | WebSim global fan-out, 250k users (§4.1) | **deterministic distributed/cluster** for mass mining (digest == single-box, S7) |
| Execution | proprietary cost model = the "secret weapon" (§7.2) | "automatic internal crossing of trades" (§6.2) | **delegated to broker execution algorithms (out of scope)**; the sim's S6-calibrated cost still *gates* admission |

The north-star thesis is unchanged but the emphasis moves: *a library of weak, mostly-uncorrelated signals is only
worth anything if it is **real**.* `p1` built the library; `p2` proves the library's contents are robust — a richer
language to express them, a smarter search to find non-redundant ones, and a battery of out-of-sample tests that
separate signal from snooping.

---

## Assumed baseline — `p0` (Phases 1–4) + `p1` (S1–S8), treated as DONE

`p2` builds strictly **on top of** the following and must **not** re-duplicate any of it; it extends through the
established seams.

- **p0 Phases 1–4** — event spine, deterministic bar-driven backtest loop, alpha-DSL + vectorized VM (lexer→Pratt
  parser→typecheck→hash-consed DAG→bytecode→columnar VM + differential oracle), mega-alpha combiner + Barra factor
  risk + turnover-penalized single-period optimizer + the **fit/apply firewall**. ✅ **The DSL/VM `p2` S3 deepens.**
- **p1 S1 — Evaluation & Validation Spine** — `PerfMetrics`, Deflated-Sharpe, PBO, purged+embargoed CPCV, bias-audit
  gate battery. The truth layer every `p2` admission reuses **verbatim**; `p2` S4's robustness battery extends it. ✅
- **p1 S2 — Parallel Compute Substrate** — `parallel::DetPool` + order-fixed `parallel_evaluate`/`parallel_cpcv`;
  determinism survives parallelism (digest invariance). The local primitive `p2` S7 lifts to multi-node. ✅
- **p1 S3 / S4 / S4b — Formulaic factory + persistent library + ResearchDriver** — GA mining (mutation/crossover/
  pool-aware fitness/seeded search driver), canonical-hash dedup, SimHash correlation-neighbor index, lifecycle state
  machine, continuous mine→admit→replay. ✅ **The GA `p2` S4 deepens (multi-objective, diversity, cost-aware);
  carries the known residuals — `op_swap` disabled (fixed in S3.4), single-objective fitness, structural-only
  novelty.**
- **p1 S5 — Learned signals & ML combiner** — PIT feature matrix, ridge/lasso/elastic-net/GBT learned alphas, HMM
  regimes, deflated-gated ensemble combiner. The base `p2` S5 deep-learning extends. ✅
- **p1 S6 — Cost calibration & capacity** — calibrated δ/Y/γ/η, Almgren-Chriss temp/perm split, per-alpha/mega
  capacity curves, cost-aware gating, borrow accrual. **The calibrated cost `p2` S4.3 mines against and S8 proves
  survival of.** ✅
- **p1 S7 — Portfolio construction & lifecycle** — `risk::MultiPeriodOptimizer` (receding-horizon driver over
  `PortfolioOptimizer::solve`), `book::DecayMonitor`, dead-alpha→risk-factor, capacity-bounded Kelly allocation,
  `book::BookReport`, `book::BookPipeline` E2E. **The single-book operating layer `p2` S1/S2 generalize to measure
  admitted alpha at portfolio scale.** ✅ (assumed)
- **p1 S8 — Vendor-grade covariance** — robust regression, EWMA+Newey-West factor cov, eigenfactor de-biasing,
  specific-risk blend, VRA, APCA, shrinkage/PSD toolkit. **The cleaned `V` the `p2` S1 optimizer trades.** ✅ (assumed)

> **The hard dependencies:** `p2` S4 (genetic search) consumes `p2` S3 (the widened substrate it searches) + `p1` S1
> (the deflated gate) + S6 (calibrated cost). `p2` S1 (multi-horizon optimizer) consumes `p1` S7's
> `MultiPeriodOptimizer` + S8's `FactorModel`. S2 (meta-book) consumes S1. S5 (deep-learning signals) consumes `p1` S1's
> validation + `p1` S4's library + `p2` S4's robustness battery. **`p2` S6 (the BYO-data abstraction layer) is a
> foundational retrofit over the existing Panel/feature/factor/library seams — the whole pipeline reads its inputs
> through it, and the price-only path pins bit-for-bit to today.** S9 (multi-asset/intraday universe) extends S6's
> catalog post-capstone. **S3 opens first on the alpha-depth track; S1 opens first on the book-construction track; S6
> and S7 are independent foundational infra.**

---

## The v3 sprint arc

> Status rows stay `⏳ proposed` and are **not** promoted to commitments until a sprint actually opens
> (`module.md`: don't document hopes as commitments). Each sprint freezes its implementation plan at kickoff.
> Effort/unit counts are estimates to re-scope at kickoff (4–7 units/sprint; a sprint that would exceed ~7 splits into
> `Sn-a`/`Sn-b`).

### Dependency DAG

```
                 p1 complete (S1–S8 closed: DSL+VM, GA factory, library, ML, cost, covariance)
                                          │
  S6 BYO-DATA ABSTRACTION LAYER (FOUNDATIONAL data substrate — a price-required dataset catalog;
   features / external signals / factor-model / reference are optional typed plug-ins; the whole
   pipeline below reads its inputs through the DataContext facade; alt-data is subsumed as a
   plugged dataset) ── retrofit under the existing seams; price-only path pins bit-for-bit to today
                                          │
        ┌─────────────────────────────────┼─────────────────────────────┐
        ▼                                 ▼                              ▼
  S3 alpha DSL &                    S1 multi-horizon                S7 distributed
   expression substrate (SPINE)      optimizer                      (lifts p1-S2 to N nodes;
   (widen the language)              (book construction)             mass-mining throughput)
        │                                 │                              │
        ▼                                 ▼                              │
  S4 genetic search &              S2 multi-strategy                     │
   robust signal pipeline           meta-book                            │
   (smarter search + PROVE          (alpha at portfolio scale)           │
    robust alpha)                                                        │
        │                                                                │
        ├──────────────► S5 deep-learning alphas (richer signal family; gated by p1-S1)
        ▼                                                                ▼
       S8 — The Robust-Alpha Proof Capstone: full pipeline + terminal lockbox eval
            (synthetic recovery · regime/walk-forward · sealed lockbox · capacity/cost —
             consumes S1·S2·S3·S4·S5·S6·S7 over the BYO DataContext — the p2 done-gate)
                                          │
                                          ▼
       S9 — Multi-Asset / Multi-Region / Intraday Universe (POST-CAPSTONE structural breadth —
            extends the proven BYO engine to new universes/horizons via S6's catalog; PIT + bias-gated)
```

> **Two tracks run in parallel from day one** (the `p1` discipline): the *deepen-the-signal* track
> (S3 → S4 → S5 → S8) and the *construct-the-book* track (S1 → S2), with S7 independent infra and **S6 the
> foundational BYO-data substrate the whole pipeline reads through**. They converge at S8; S9 (universe breadth)
> extends the proven engine post-capstone.
> **S3→S4 is the spine** — `p2` exists to prove the factory finds robust alpha, and S3 (a language rich enough to
> express it) plus S4 (a search smart enough to find it, and a battery stringent enough to prove it) is that proof.
> Nothing downstream means anything if the admitted alpha is overfit garbage.

---

### S1 — Constrained Multi-Horizon Portfolio Optimization  ✅ DONE (branch `feat/p2-s1-multi-horizon`, pending merge — merge is the user's gate) ([impl-plan](sprint-1-multi-horizon-optimization-implementation-plan.md) · [progress](sprint-1-progress.md))
**Theme:** Book construction — **turn admitted alpha into a measurable portfolio.** `p1` S7 ships a *receding-horizon
driver* that chains single-period proximal solves over a schedule (threading `w_prev`, executing only the first move).
That is multi-*period* but not multi-*horizon*: each inner solve optimizes a single-period objective and the constraint
set is `{Σw=0, Σ|w|≤L, |w_i|≤cap}` only. S1 builds the **true constrained multi-horizon convex program**: signals carry
an explicit **decay horizon** (fast/slow); the objective optimizes today's book against a **forward trajectory** of
horizon-decayed forecasts `{α_t,…,α_{t+H}}` summed under risk-adjusted, cost-aware utility (Boyd et al. 2017,
*Multi-Period Trading via Convex Optimization*), executes only the realized first move (MPC, look-ahead-safe); a
**full constraint algebra** (factor-exposure `|Xᵀw|≤b` over S8's `X`, sector/group caps, beta neutrality, per-horizon
turnover budgets, gross/net leverage, position/ADV caps, no-trade bands); a **fixed-iteration QP/ADMM** solver
(OSQP-style, pre-factorized KKT — determinism by fixed iteration count, no convergence early-exit) that handles the
richer constraint set exactly; and the **Gârleanu-Pedersen aim portfolio** (trade partway toward a forward-blended
target — the closed form for quadratic costs that generalizes S7's scalar `trade_rate`). Trades S8's cleaned `V`;
consumed by S2 and S8. It is the *measurement instrument* for the robust alpha S3/S4 produce — the portfolio-scale lens
on the library. *Grounded in RenTech §5.1/§6.1 (cost-throttled multi-horizon, single unified re-optimized book),
WorldQuant §6.1/§6.5 (bounded regression, turnover-as-cost), Boyd 2017, Gârleanu-Pedersen 2013, OSQP (Stellato 2020).*
**Needs only `p1` S7 + S8** — opens first on the book-construction track.

| # | Unit | Effort | Status |
|---|---|---|---|
| S1.0 | Marker + ledger + as-built recon vs `risk::MultiPeriodOptimizer`/`PortfolioOptimizer` (D1–D8) | S | ✅ `1ec419f` |
| S1.1 | Constraint-algebra layer (composable factor-exposure / sector / beta / gross-net / position / turnover-budget descriptors over S8's `X`) — `risk/constraints.hpp`, 25 tests | M | ✅ `971cc6b` |
| S1.2 | Fixed-iteration constrained QP/ADMM solver (OSQP-style, matrix-free factored-`V` via new `FactorModel::apply`; deterministic, no early-exit) — `risk/qp_solver.hpp`, 13 tests; atx-core L7 `qp_admm` recorded as the lift | L | ✅ `1ebf01f` |
| S1.3 | Multi-horizon forecast model (signal-horizon taxonomy; per-horizon decay; forward trajectory `α_{t…t+H}`; PIT pure kernel D8) — `risk/horizon.hpp`, 12 tests | M | ✅ `66a1542` |
| S1.4 | Multi-horizon objective + Gârleanu-Pedersen aim portfolio (forward-blended target; MPC execute-first-move; boundary-pin dispatch D9) — `risk/multi_horizon.hpp`, 15 tests | L | ✅ `70f93ca` |
| S1.5 | Integrate over S7 `MultiPeriodOptimizer` + S8 cleaned `V`; 4-gates integration test (R1/R2/R3/R7) + `bench/multi_horizon_bench.cpp` + close | M | ✅ `dedd3fd` |

> **Boundary pin:** with one horizon, `trade_rate=1`, and the `{Σw=0,Σ|w|≤L,|w_i|≤cap}` constraint set only, S1 must
> reduce **bit-for-bit** to `p1` S7's chained single-period book — the regression anchor against the proven layer.

### S2 — Multi-Strategy Meta-Book & Risk Budgeting  ✅ DELIVERED (2026-06-14) ([impl-plan](sprint-2-multi-strategy-meta-book-implementation-plan.md) · [progress](sprint-2-progress.md))
**Theme:** Measure admitted alpha at the **fund** level — Laufer's single unified book, made multi-sleeve, as a
portfolio-scale lens on library breadth. A **`Strategy`/`Sleeve`** abstraction (each sleeve = a universe × horizon ×
signal-family book wrapping its own S1 optimizer + library subset), a **meta-allocator** that combines sleeves into one
fund (cross-sleeve weights via risk-budgeting / a portfolio-of-books fractional-Kelly that extends `p1` S7's
`book::allocation`), a **cross-sleeve risk model** (one shared factor model; aggregate factor exposure and
sleeve-correlation netting), and **internal trade crossing/netting** — sum sleeve target positions into one fund order
so offsetting child trades cancel (the cost saving stays *measured in the backtest*; actually routing the netted order
is the broker's job). Fund-level reporting with **attribution-by-sleeve**. *Grounded in RenTech §6.1 (single unified
book), WorldQuant §6.2 (internal crossing), Grinold-Kahn (risk budgeting across breadth sources).* Consumes S1.

| # | Unit | Effort | Status |
|---|---|---|---|
| S2.0 | Marker + ledger | S | ✅ `f93fece` |
| S2.1 | `Sleeve` abstraction (universe × horizon × signal-family; wraps an S1 optimizer + a library subset; PIT) | M | ✅ `2d4718e` |
| S2.2 | Meta-allocator (sleeves → one fund; cross-sleeve risk budget; portfolio-of-books Kelly extending `book::allocation`) | L | ✅ `7c25c49` |
| S2.3 | Cross-sleeve risk model (shared `FactorModel`; aggregate exposure; sleeve-return correlation) | M | ✅ `014756f` |
| S2.4 | Internal trade crossing/netting (net sleeve targets into one fund order; net-cost saving measured in-sim) | M | ✅ `ca4d181` |
| S2.5 | Fund-level report + attribution-by-sleeve + close | M | ✅ `03c208d` |

### S3 — Alpha DSL & Expression Substrate  ⏳ proposed
**Theme:** The spine, part one — **widen and correct WHAT the factory can express.** `p1`'s DSL is production-grade
(65 ops, Pratt parser, hash-consed DAG, bit-exact differential oracle) but expressively shallow in three load-bearing
ways: it neutralizes only by **group-demean** (no regression-residual — the standard size/sector-neutral construction,
research §6.5); it is **missing the BRAIN time-series superset** (`ts_regression`, `ts_decay_exp`, `ts_entropy`,
`ts_moment`, and friends — research §1.4) and a richer datafield family (vwap/adv/volume operators the WQ Alpha101 set
leans on); and the genetic miner runs with **`op_swap` disabled** (a p1 S3-1 residual — an analyze-valid mutation that
corrupts the VM at evaluate), which silently *narrows the mutable operator space* the search can reach. S3 deepens the
substrate so S4's search explores a **richer, correctly-typed, fully-mutable** space: it adds the general
cross-sectional residualizer (exposing `indneutralize` as its demean special case), the BRAIN-superset operators
(each with an oracle reference, lookback-rail causality, and an explicit NaN policy), the missing datafield family,
**fixes and re-enables `op_swap`**, and moves generation from *random-then-reject* to **grammar-typed
(valid-by-construction)**. It closes on a differential-conformance suite plus an **Alpha101-subset reproduction**
fixture that proves the engine can actually express the canonical public alphas. *Grounded in the alpha-DSL deep-dive
§1 (WorldQuant Fast Expression), §1.4 (BRAIN operator surface), §6.5 (neutralization mechanics), §2/§3 (Qlib/Zipline
operator hierarchies), WorldQuant §3.3 (operator catalog), `p1` P3/S3.* **Needs only the `p0` DSL/VM + `p1` factory
OpCatalog** — opens first on the alpha-depth track.

| # | Unit | Effort | Status |
|---|---|---|---|
| S3.0 | Marker + ledger + as-built recon vs `alpha::{registry, vm, oracle, ts_ops, cs_ops, typecheck}` + `factory::OpCatalog` | S | ✅ |
| S3.1 | Regression-residual neutralization — general cross-sectional residualizer (regress signal on group dummies + log-cap/style factors per date, keep residual; `indneutralize` = demean special case); oracle + bit-exact differential + look-ahead rail | M | ✅ |
| S3.2 | BRAIN-superset time-series operators (`ts_regression`/`ts_decay_exp`/`ts_entropy`/`ts_moment` + audit `ts_backfill`/`ts_quantile` parity); each with oracle reference, lookback propagation, explicit `min_periods`/NaN policy | L | ✅ |
| S3.3 | Cross-sectional/vector gap-fill (`quantile`, `reverse`, `vec_sum`/`vec_avg`, winsorize/normalize variants) + vwap/adv/volume datafield family on the `Panel` | M | ✅ |
| S3.4 | **Fix the `op_swap` VM-corruption defect** (p1 S3-1 residual: analyze-valid genome corrupts the VM SlotPool at evaluate) + re-enable `op_swap`; differential VM/oracle stress across every `(shape, dtype, arity)` bucket | M | ✅ |
| S3.5 | Grammar-typed expression generation (shape/dtype-aware genome init + mutation-candidate buckets → valid-by-construction, not random-then-reject; raises generation yield, cuts wasted eval) | M | ✅ |
| S3.6 | Operator differential-conformance suite (oracle == VM bit-exact across the widened op set) + **Alpha101-subset reproduction fixtures** (encode + evaluate the canonical public alphas) + DSL bench + close | M | ✅ |

> **Boundary pin:** every pre-existing `p1` alpha formula must compile and evaluate **bit-for-bit identically** after
> S3 — the widened op set and grammar-typed generation are *additive*; the differential oracle (`oracle == VM`) is the
> non-regression anchor on the proven 65-op core.

### S4 — Genetic Search & Robust Signal Pipeline  ✅ shipped (2026-06-14, merged `--no-ff` into `main`)
**Theme:** The spine, part two — **search smarter, and PROVE the alpha is robust.** `p1`'s GA is a solid seeded
evolutionary loop but a *shallow optimizer of a shallow objective*: it maximizes a single scalar
`raw = wq × diversify × robust`, measures novelty only as **canonical-hash distance** (two behaviorally-identical
expressions that hash differently look "novel"), and prices cost as a **0.125 turnover floor** rather than the
S6-calibrated impact at a target AUM. And while `p1` S3 proved *non-vacuousness* (a pure-noise panel admits ~0 vs a
real-signal panel), it never proved **robustness** — that an admitted alpha survives out-of-regime, walk-forward, a
sealed holdout, and realistic cost. S4 fixes both halves. The search becomes **multi-objective**: a Pareto/NSGA-II
front over {return, diversification, robustness, cost} with deterministic non-dominated sorting + crowding distance,
replacing the scalar collapse; novelty becomes **behavioral/phenotypic** (signal-return-profile distance + a behavioral
archive) so the population can't collapse onto one syntactic neighborhood; and mining becomes **cost-aware** (the
S6-calibrated κ at target AUM enters the objective, so an alpha's mining score is net-of-cost). The pipeline then
proves robustness with the full battery — **synthetic-alpha recovery** (plant a known alpha in synthetic data, prove
the GA recovers it *and* a pure-noise panel admits ~0 under the same bar), **regime/walk-forward survival** (re-eval
admitted alphas across bull/bear/high-vol regimes + rolling windows; admit only out-of-regime survivors), and a
**sealed final lockbox** (carve a never-touched terminal holdout, sealed until S8) — culminating in the **E2E robust
signal-generation pipeline**: mine → multi-objective gate → library admit → combine → multi-horizon book backtest,
deterministic and replayable, demonstrating the populated library holds **real, survivable, net-of-cost** alpha.
*Grounded in WorldQuant §1/§5/§9 (formulaic mining, fitness, the factory), the alpha-DSL deep-dive §3.3 (GP miners),
Deb et al. 2002 (NSGA-II), Lehman-Stanley 2011 (novelty search), Bailey/López de Prado (deflation, lockbox discipline),
RenTech §7 (ruthless OOS), `p1` S1/S3/S4b/S6.* Consumes S3 (the substrate), `p1` S1 (the deflated gate), `p1` S4
(library), `p1` S6 (calibrated cost); feeds S5/S6 and the S8 capstone.

| # | Unit | Effort | Status |
|---|---|---|---|
| S4.0 | Marker + ledger + as-built recon vs `factory::{search_driver, fitness, research_driver, op_catalog}` | S | ✅ `c32cd85` |
| S4.1 | Multi-objective Pareto selection (NSGA-II — deterministic non-dominated sort + crowding distance over {return, diversification, robustness, cost}; replaces scalar `raw`; seeded, order-fixed) | L | ✅ `1ec0fcd` |
| S4.2 | Behavioral/phenotypic diversity (signal-return-profile novelty + behavioral archive, beyond canonical-hash distance; anti-collapse pressure) | M | ✅ `06876cb` |
| S4.3 | Cost-aware mining fitness (S6-calibrated impact/turnover at target AUM into the objective — net-of-cost mining, not the 0.125 turnover-floor proxy) | M | ✅ `286efc8` |
| S4.4 | Robustness battery — synthetic-alpha recovery (plant → recover → noise-reject) · regime/walk-forward re-eval (out-of-regime survivors only) · **lockbox reservation** (seal terminal holdout for S8) *(shipped as S4.4a measurement + S4.4b lockbox/hook)* | L | ✅ `15808cc`·`ee3d881` |
| S4.5 | E2E robust signal-generation pipeline (mine → multi-obj gate → library admit → combine → multi-horizon book backtest; deterministic; the proof the library holds robust net-of-cost alpha) + bench + close | L | ✅ `378c523` |

> **Boundary pin:** in the degenerate single-objective limit (collapse the Pareto front to the lone `raw` objective,
> disable behavioral novelty and cost-awareness), S4's search must reduce **bit-for-bit** to `p1` S3's seeded
> evolutionary loop — the regression anchor against the proven miner.

> **What S4 proved (shipped 2026-06-14, full suite 1345 pass / 5 disabled, `/W4 /WX /fp:precise` clean):** the search is now
> **multi-objective** — engine-local NSGA-II (`factory/pareto.hpp`: deterministic non-dominated sort + crowding, canon-id
> tie-break) over a {wq, diversify, robust, behavioral-novelty, −cost} vector, with the **boundary pin held bit-for-bit**
> (`SearchResult.digest == 0xa83f0d3e0b41a18d`) through every unit and at the pipeline level (collapsed `RobustResearchDriver`
> == plain `ResearchDriver`). Novelty is **behavioral** (OOS-PnL `1−|corr|` + a FIFO archive) — proven to invert the
> structural-hash ranking on a headline fixture. Mining is **cost-aware** (`cost::round_trip_cost_bps` book-aggregate at
> `target_aum`, hand-checked 369.65 bps). Robustness is **proven**: synthetic-alpha recovery (plant→recover, β=0 noise→0),
> vol-tercile regime + walk-forward survival, and a content-addressed **sealed lockbox** (`eval/lockbox.hpp`; reads into the
> sealed region trap; S8.2 opens it once). The **E2E `factory/robust_pipeline.hpp`** (mine→multi-obj gate→admit→combine→p2-S1
> book) grows a robust library by ~0 on noise and replays byte-identically across {1,2,4,8} workers. Determinism axis
> `(master,run,gen,idx)` + DetPool worker-invariance held throughout.
>
> **Residual backlog (lifted from the S4 ledger):** (1) optimize the augmented-QP `MultiHorizonOptimizer::run` perf
> (~20 s/run) and re-enable the 3 `DISABLED_` `MultiHorizonIntegration` R1/R2/R3 tests (disabled at S4 for dev-loop speed);
> (2) the S4.5 book stage uses a K=1 placeholder SPD `FactorModel` — wire a trailing-fit risk model over the visible panel;
> (3) the robust subset is re-screened in the pipeline combine/book stage (append-only lifecycle records but does not
> un-admit) — a future sprint could wire a true pre-admit robustness gate inside `mine_into` so the library is robust-only.
>
> **Next sprint priorities:** the spine continues at **S5** (deep-learning sequence alphas — must pass this S4 robustness
> battery + the `p1` S1 deflated gate) and converges with the book-construction track (S1→S2) at the **S8** robust-alpha
> capstone, where the lockbox `eval/lockbox.hpp` reserved here is opened exactly once.

### S5 — Deep-Learning Sequence Alphas  ✅ done ([impl-plan](sprint-5-deep-learning-sequence-alphas-implementation-plan.md) · [progress](sprint-5-progress.md))
**Theme:** A richer **signal family** — `p1` anti-roadmap #7: *"NN ensembles are p2-or-later, gated by evidence the
simpler combiners are saturated."* S5 honors that gate by building NN signals **only behind `p1` S1's deflated-Sharpe
/ PBO admission battery** (the validation spine exists precisely so this is safe) and feeding them through the **same
S4 robustness battery** as any other alpha. A **PIT sequence-feature tensor builder** (extends `p1` S5's feature
matrix to temporal windows — no look-ahead), a **deterministic NN training substrate** (seeded, fixed-iteration,
order-fixed — the determinism invariant's hardest stress point; the NN kernels are an atx-core Pattern-B request),
**temporal-conv / GRU-lite** sequence alphas + an **attention-lite / autoencoder statistical-factor** alpha (the
noisy-channel / latent-state heritage, RenTech §3.2/§4.2, expressed as a learned sequence model), each emitting a
cross-section that **plugs into the library exactly like a formulaic alpha** and is admitted only through the deflated
gate. *Grounded in RenTech §3.6/§4.2 (statistical sequence modeling, HMM/noisy-channel heritage), WorldQuant §3.2 (the
millions-of-weak-alphas thesis at the model frontier), `p1` S1/S5.* Consumes `p1` S1 (gate), `p1` S4 (library), `p2`
S4 (the robustness battery it must also pass).

| # | Unit | Effort | Status |
|---|---|---|---|
| S5.0 | Marker + ledger | S | ✅ `d309a9b` |
| S5.1 | PIT sequence-feature tensor builder (temporal windows over `p1` S5 features; no look-ahead) | M | ✅ `fe978a6` |
| S5.2 | Deterministic NN training substrate (seeded, fixed-iteration, order-fixed) + temporal-conv/GRU-lite alpha — atx-core L7 NN request | L | ✅ `a01e53c`·`4d438cc`·`d28691d` |
| S5.3 | Attention-lite / autoencoder statistical-factor alpha (latent-state heritage) | L | ✅ `417a1f8`·`10724f5` |
| S5.4 | Deflated-Sharpe + PBO admission gate for NN alphas (reuses `p1` S1 verbatim; the anti-overfit firewall) + S4 robustness-battery pass | M | ✅ `747da91` |
| S5.5 | NN-alpha → library + combiner/sleeve integration + bench + close | M | ✅ `a193552`·`93a9c41` |

### S6 — Data / Signal / Feature / Reference Abstraction Layer — the BYO-Data Engine  ⏳ proposed ([spec](sprint-6-data-signal-feature-reference-abstraction-layer.md) · [impl-plan](sprint-6-data-signal-feature-reference-abstraction-layer-implementation-plan.md)) · split **S6-a / S6-b** (>7 units)
**Theme:** Turn the engine into a **bring-your-own-data pipeline.** Today atx-engine is wired to *one fixed in-memory
`alpha::Panel` per run*, and every exogenous input — features, factor model, reference classifications — is computed
*internally from price* (`FeatureMatrix` reads Panel fields + pool alphas; `FactorModelBuilder` regresses style/sector
exposures out of the price panel; sector/cap enter as ad-hoc optional spans). There is **no seam to plug in your own
data**: no way to say *"here is my fundamental feature set,"* *"here is my precomputed signal library,"* or *"here is my
factor model — use it instead of computing one."* S6 builds that seam as a **first-class abstraction layer**: a typed,
PIT-versioned **`data::Dataset`** (roles *Price / Feature / Signal / Reference*, each with a schema, an as-of/effective-
date/restatement policy, and a lineage record), a **`DatasetCatalog`** registry (register/resolve named datasets,
enforce schema, track provenance, resolve PIT versions deterministically), an ingestion+alignment+PIT-validation rail
(reconcile heterogeneous *(date × instrument)* grids onto the canonical axis; enforce no-look-ahead / no-survivorship /
NaN policy **at ingestion**), and a **`DataContext`** facade that replaces the bare-Panel argument threaded through
`factory::{Factory, ResearchDriver}`, `book::BookPipeline`, and the `fund::` meta-book — lowering each registered
dataset into the existing concrete structures (`alpha::Panel`, `learn::FeatureMatrix`, `risk::FactorModel`,
`library::Library`) via differential-pinned adapters. **The only required input is a price dataset**; features, external
signals, a factor model, and reference data are all *optional* plug-ins, each with a price-only fallback that computes
internally exactly as today. Two consequences re-scope the deferred frontier: **alt-data stops being special-cased** —
fundamental/analyst/news/sentiment become ordinary `Feature`/`Reference` dataset instances (no bespoke per-source code;
news/sentiment still event-gated through the existing `trade_when` operator) — and **external precomputed signals become
first-class**, entering *both* as ML features *and* as direct `library::Library` admissions, gated by the **same**
deflated-Sharpe + S4 robustness battery as a mined alpha. The structural-universe frontier (multi-asset/region,
intraday horizon) carves out to **S9**. *Grounded in WorldQuant §8/§4.3 (the 125k-field PIT datafield catalog,
universes, regions, delay), RenTech §6.2/§7.1/§9.1 (deeper-cleaner-data edge, PIT as-of versioning, survivorship), `p1`
S1 (bias audits the ingestion rail extends), `p1` S5 (the feature matrix), `p1` S8 (the factor model).* **S6-a needs only
the `p0`/`p1` Panel/FeatureMatrix/FactorModel/Library seams** (opens concurrently — a foundational retrofit the whole
pipeline reads through); **S6-b's external-signal admission (S6.5) additionally consumes `p2` S4's robustness battery**
(so it follows S4's battery landing). Feeds S3 (richer fields), S5 (richer features), S4 (more candidates + BYO signals),
and is consumed by the S8 capstone.

> **Scope forks resolved at theme (per `module.md` §10 — write the decision, kill the fork):** (1) deliverable surface =
> **full dataset catalog/registry** (typed datasets + schema + PIT versioning + lineage), *not* in-engine file readers
> — format parsing stays the user's / atx-tsdb's job (anti-roadmap #7); (2) old S6 alt-data is **subsumed** into the
> abstraction, multi-asset/intraday **defers to S9**; (3) external signals enter **both** as features **and** as gated
> library admissions; (4) the factor-model seam is **full BYO** (external X/F/D ingestion) **plus** reference-dataset
> ingestion into the internal builder.

**S6-a — abstraction core + price + feature**

| # | Unit | Effort | Status |
|---|---|---|---|
| S6.0 | Marker + ledger + as-built recon vs `data::*`, `alpha::Panel`/`datafields`, `learn::FeatureMatrix`, `risk::{FactorModel, exposures}`, `library::Library`, `book::BookPipeline` seams | S | ⏳ |
| S6.1 | `data::Dataset` + `DatasetSchema` — typed, PIT-versioned *(date × instrument)* named-column store (roles Price/Feature/Signal/Reference; dtype; provenance record) + differential tests | M | ⏳ |
| S6.2 | `data::DatasetCatalog` registry — register/resolve-by-name, schema enforcement, lineage/provenance graph, deterministic ordering, PIT as-of version resolution — atx-core **L9 `as_of_frame`** edge | L | ⏳ |
| S6.3 | Ingestion + alignment + PIT-validation rail — reconcile heterogeneous grids onto the canonical date/universe axis; enforce no-look-ahead / no-survivorship / NaN policy / as-of versioning **at ingest**; truncation-invariant | L | ⏳ |
| S6.4 | PriceDataset → `alpha::Panel` adapter (subsumes `with_datafields`; **price-only fallback** = today's path) + FeatureDataset → `learn::FeatureMatrix` external-feature injection | M | ⏳ |

**S6-b — signal + factor-model + reference + subsumption + wiring + capstone**

| # | Unit | Effort | Status |
|---|---|---|---|
| S6.5 | SignalDataset — external precomputed signals enter **both** as ML features **and** as direct `library::Library` admissions, gated by the deflated-Sharpe + S4 robustness battery (the BYO-signal first-class path) | L | ⏳ |
| S6.6 | BYO factor model — `FactorModelArtifact` ingestion (external X/F/D → `risk::FactorModel::create`) + ReferenceDataset ingestion (sector/group_id, market_cap, custom styles → internal `FactorModelBuilder`); price-only fallback computes internally as today | L | ⏳ |
| S6.7 | Alt-data subsumption — re-express fundamental/analyst/news/sentiment as `Feature`/`Reference` dataset instances (no bespoke per-source code; news/sentiment event-gated via existing `trade_when`); proves the abstraction generalizes the deferred frontier | M | ⏳ |
| S6.8 | `DataContext` facade wiring through `factory::{Factory, ResearchDriver}` + `book::BookPipeline` + `fund::` meta-book (replace bare-`Panel` args via adapter; **boundary pin** — price-only DataContext ≡ today bit-for-bit) | M | ⏳ |
| S6.9 | E2E BYO capstone test (price + plugged feature + plugged signal + plugged factor model → mine/admit/combine → multi-horizon book → portfolio; deterministic digest) + catalog/lineage report + ingestion/DSL bench + close | L | ⏳ |

> **Boundary pin:** with **only** a price dataset registered and no optional plug-ins, the `DataContext` path must reduce
> **bit-for-bit** to today's fixed-`alpha::Panel` `BookPipeline` path — the regression anchor that proves the abstraction
> is purely additive and breaks none of S1–S5/S7's proven layers.

### S7 — Distributed / Scale-Out Compute  ⏳ proposed ([impl-plan](sprint-7-distributed-scale-out-compute-implementation-plan.md))
**Theme:** Lift the scale ceiling for **mass mining** — the deferred distributed compute (`p1` anti-roadmap #4:
*"Single-box multicore is the scale ceiling for v2 … cross-machine eval = p2"*). WorldQuant's WebSim fans hundreds of
thousands of backtests/day across a global population (§4.1/§8); RenTech re-optimizes a unified book over a petabyte
warehouse (§6.1/§6.2). S7 lifts `p1` S2's single-substrate `parallel::DetPool` into a **substrate-agnostic
`IExecutor` abstraction** while preserving the non-negotiable: **the digest must be invariant across substrate and
worker count.** A **substrate-agnostic executor seam** (the Ray-`init`/Dask-`Client`/Spark-`master` "swap only the
placement handle" pattern — `WorkUnit`/`submit`/`gather`/slots/digest are transport-agnostic), a **cross-platform
multi-process shared-memory substrate** (Win32 `CreateFileMapping` + POSIX `shm_open`; read-only zero-copy inputs +
pre-indexed slots), a **deterministic heterogeneous-cost scheduler** (NUMA/affinity/first-touch; load-balance with
placement decoupled from steal order), a **partition-and-merge library** (the S4 library admit made process-safe via
disjoint-`AlphaId` partitions + canonical manifest merge), the **digest-invariance proof**
(`{sequential == ThreadExecutor@{1,N} == ProcessExecutor@{1,N}}` byte-identical) + **deterministic fault tolerance**
(a failed shard re-runs bit-identical because shards are pure). This is the throughput substrate the S3/S4 mass-mining
needs — fan the genetic search across machines without breaking the determinism the robustness proofs depend on.
> **Reframe (kickoff directive):** S7 ships the robust **single-machine** substrate — **threads + multi-process shared
> memory on one box** — with the scale-out abstraction fully in place; the **N-node network transport** is interface-only
> + a recorded **atx-core L4 `dist_pool` lift**, and the **cross-machine canonical-byte-order digest** is its companion
> lift. The impl-plan §0 overrides this sketch on conflict.
*Grounded in WorldQuant §4.1/§8 (global fan-out scale), RenTech §6.1/§6.2 (continuous re-optimization at scale), `p1` S2
(the determinism-under-parallelism invariant this must not break), and the verified executor/determinism/IPC literature —
Ray, Dask, Spark, P2300, oneTBB, Roofline, Drepper (impl-plan §1A).* **Needs only `p1` S2** — independent infra track.

| # | Unit | Effort | Status |
|---|---|---|---|
| S7.0 | Marker + ledger + as-built recon vs `parallel::{DetPool, parallel_evaluate, parallel_cpcv, parallel_backtests}`, `digest.hpp`, `shm_bar_feed`, factory/library hazard map | S | ⏳ |
| S7.1 | `IExecutor` seam + `WorkUnit`/shard model + deterministic gather; `ThreadExecutor` (DetPool adapter, the degenerate 1-process case); `fwd.hpp` SWITCH POINT — atx-core L4 request | M | ⏳ |
| S7.2 | Cross-platform shared-memory substrate (`ShmSegment`: Win32 `CreateFileMapping` + POSIX `shm_open`; read-only zero-copy inputs + pre-indexed slots; reuses `SegmentReader`) | L | ⏳ |
| S7.3 | `ProcessExecutor` — the primary multi-process pool over `ShmSegment` (worker-process lifecycle; deterministic dispatch; pure-shard exec; parent gather+digest) | L | ⏳ |
| S7.4 | Deterministic heterogeneous-cost scheduler (cost-ordered dispatch + NUMA/affinity/first-touch/topology; no bit contact) | M | ⏳ |
| S7.5 | Port workloads onto `IExecutor` (batch-eval + CPCV + backtests + GA mine loop) + library partition-and-merge (disjoint-`AlphaId`, canonical manifest merge) | M | ⏳ |
| S7.6 | Five-path digest-invariance proof + deterministic fault tolerance (failed-shard bit-identical replay) + scale bench (bandwidth/NUMA knee) + close | M | ⏳ |

### S8 — The Robust-Alpha Proof Capstone  ⏳ proposed
**Theme:** Tie **everything** together and prove the headline claim — *the factory discovers robust, survivable alpha.*
The `p2` done-gate. A **full-pipeline orchestrator** that runs top-to-bottom: **BYO `DataContext` (S6)** → GA (S3-deepened
substrate, S4-smartened search) + ML + deep-learning signals (S5) → library (`p1` S4) → **constrained multi-horizon
optimize** (S1) → **multi-strategy meta-book** (S2) → **distributed at scale** (S7) → **open the S4.4-reserved final
lockbox exactly once** → reproducible **robust-alpha report** (the four proofs side by side: synthetic-recovery,
regime/walk-forward survival, lockbox generalization, capacity/cost survival). The capstone test proves **all
carried-forward invariants simultaneously** — determinism (incl. the seeded NN + distributed paths), no look-ahead
(truncation-invariance at every fitted boundary), no survivorship, honest calibrated cost, **and the robustness
invariant** (out-of-regime survival + lockbox generalization) — top-to-bottom at library scale. *Grounded in RenTech
§6.1/§7 (one unified book, ruthless OOS), WorldQuant §4.1/§6 (mega-alpha at scale), Bailey/López de Prado (lockbox
discipline, deflation), the full carried-forward `p0`/`p1` invariant set.* Consumes **everything**.

| # | Unit | Effort | Status |
|---|---|---|---|
| S8.0 | Marker + ledger | S | ⏳ |
| S8.1 | Full-pipeline orchestrator (BYO DataContext → GA+ML+NN → library → S1 optimize → S2 meta-book → S7 distributed) | L | ⏳ |
| S8.2 | Terminal lockbox evaluation (open the S4.4-reserved holdout exactly once; the never-before-seen generalization test) | M | ⏳ |
| S8.3 | Robust-alpha report (synthetic-recovery + regime/walk-forward + lockbox + capacity/cost, side by side; reproducible headless artifact) | M | ⏳ |
| S8.4 | All-invariants-simultaneously capstone test (determinism + PIT + no-survivorship + honest-cost + **out-of-regime + lockbox generalization**, library scale) | L | ⏳ |
| S8.5 | End-to-end library-scale bench + `p2` close ceremony (residuals lifted, ROADMAP bumped, user refs) | M | ⏳ |

### S9 — Multi-Asset / Multi-Region / Intraday Universe  ⏳ proposed (post-capstone)
**Theme:** Structural **universe breadth** — the part of the old S6 frontier the abstraction layer does *not* absorb,
deferred to **after** the S8 capstone so the proof runs first over the trustworthy US-equity daily-bar spine. Where S6
makes *new datasets* pluggable on the existing axis, S9 widens the **axis itself**: a **multi-asset / multi-region
universe** (beyond US-equity daily bars — cross-asset correlation in the risk model, region/currency tags on the
`DatasetCatalog` schema) and an **intraday-bar universe + horizon** (sub-daily panel as a richer *signal* substrate —
more bars, more alpha; **not** an execution layer). Both ride S6's catalog (a non-US or intraday panel is *just another
registered `Price` dataset with different universe/region/delay metadata*) and stay **behind the PIT + bias-audit
gates** — the daily-bar correctness model remains the default; new universes are explicitly-gated additive signal
paths. *Grounded in WorldQuant §8/§4.3 (17 regions, universes, delay), RenTech §7.1 (cross-asset breadth), `p1` S1 (bias
audits), `p2` S6 (the catalog it extends), `p2` S8 (the proof it must not weaken).* Consumes S6 (catalog) + S8 (the
proven engine); re-runs the S8 invariant battery over each new universe.

| # | Unit | Effort | Status |
|---|---|---|---|
| S9.0 | Marker + ledger + as-built recon vs `data::DatasetCatalog` universe/region schema + `risk::FactorModel` cross-asset seam | S | ⏳ |
| S9.1 | Multi-asset / multi-region universe (region/currency schema tags; cross-asset correlation in the risk model; survivorship across asset classes) | L | ⏳ |
| S9.2 | Intraday-bar universe + horizon (sub-daily `Price` dataset; intraday horizon feeds the S3 datafield family + S5 features; daily-bar spine stays default) | M | ⏳ |
| S9.3 | Cross-universe bias-audit gates (extend `p1` S1 + S6 ingestion rail to multi-region/intraday data-snooping) + re-run S8 invariant battery per universe + close | M | ⏳ |

---

## Carried-forward invariants (non-negotiable — every sprint, proven by test not hope)

These are `p0`'s + `p1`'s load-bearing guarantees, plus one new to `p2`. `p2` **must not** weaken any; deep learning
(S5) and distributed (S7) are the two places the old ones are easiest to break, and the robustness invariant (8) is the
*new* load-bearing claim the whole module exists to prove.

1. **Determinism.** Same input → byte-identical result digest. No RNG except **seeded, deterministic** training/search
   (S4 GA, S5 NN fits) whose seed is part of the recorded artifact. Order-fixed reductions in canonical id order —
   **including the distributed cross-machine merge** (S7): the N-node digest must equal the single-box digest.
2. **No look-ahead.** Every fitted object (combiner weights, gate stats, factor model, mined/learned/NN parameters,
   calibrated cost, regime states, multi-horizon forecast trajectory, **the residual-neutralization regression** S3.1)
   is estimated on a trailing window and applied OOS only — the fit/apply firewall, extended by S1's purge+embargo and
   S4's walk-forward. The MPC optimizer **executes only the first move** of its horizon (S1). **Truncation-invariance
   is the test.**
3. **No survivorship.** Delisted symbols pass through with their final bar across sleeves, every alt-data / multi-asset
   build, and every regime/walk-forward re-slice.
4. **Point-in-time.** Out-of-universe / not-yet-knowable data reads NaN; **the S6 ingestion+alignment rail is the new
   PIT enforcement point** — every plugged dataset's as-of/effective-date/restatement versioning transitions on PIT
   boundaries only, so no restated value leaks into a past decision (the alignment of heterogeneous BYO grids is
   truncation-invariant by test); the lockbox (S4.4/S8) is sealed to *everything* upstream until its single terminal open.
5. **Honest cost.** Cost is a first-class decision input — S4's **mining fitness** prices the S6-calibrated κ
   (net-of-cost mining), S1's optimizer prices it in its turnover budget, and S8 reports capacity/cost-survival at
   library scale.
6. **No hot-path allocation.** Steady-state loops (the rebalance crank, the per-node eval loop, the per-generation
   eval) allocate zero. Cold paths (solve, fit, NN train, mine-generation, distribute) may allocate.
7. **Differential correctness.** Any new compute kernel (the widened DSL operators + residualizer S3, the QP/ADMM
   solver S1, the NSGA-II sort S4, NN forward/backward S5, the distributed merge S7) ships with an obviously-correct
   reference and a bit-/ULP-bounded differential test. **The `oracle == VM` equality extends to every S3 operator.**
8. **Robustness (new in `p2`).** No alpha is *trusted* on full-sample OOS alone. An admitted alpha must survive the
   S4 battery — **synthetic-recovery** (the search finds planted signal and rejects noise), **out-of-regime /
   walk-forward** survival, and **net-of-cost** capacity — and the capstone library must generalize past the
   **sealed final lockbox** opened exactly once (S8). Proven by the robust-alpha report, not asserted.

---

## Cross-module dependencies (Pattern B — edges live here)

`p2` raises new `atx-core` numeric/infra needs. Each is an edge recorded here, not a primitive built in the engine.

| p2 unit | Needs from `atx-core` | Engine-side fallback (shippable now) |
|---|---|---|
| S3.1 | **L7 `cs_residualize`** — per-date cross-sectional regression-residual (group dummies + style; deterministic) | engine-local QR/normal-equations residualizer over `core::linalg` on the valid set |
| S3.2 | **L6 `rolling_ext`** — `ts_regression`/`ts_entropy`/`ts_moment` rolling kernels | engine-local strided-window kernels in `ts_ops.hpp` with oracle parity (the existing pattern) |
| S4.1 | **L7 `nondominated_sort`** — deterministic non-dominated sort + crowding distance | engine-local O(N²) NSGA-II sort (deterministic; the N-per-generation is small) |
| S1.2 | **L7 `qp_admm`** — fixed-iteration constrained QP (OSQP-style ADMM, pre-factorized KKT) | fixed-iteration ADMM/projected-proximal loop wrapping `risk::PortfolioOptimizer` projection primitives |
| S5.2 | **L7 (or new L-tier) NN primitives** — deterministic autodiff / fixed-iteration SGD + conv/GRU/attention kernels | engine-local fixed-iteration seeded mini-batch trainer over `core::linalg` (the dedicated kernel is the lift) |
| S6.2 | **L9 `as_of_frame`** — PIT as-of/effective-date/restatement versioning + dataset lineage in the frame/tsdb (the `DatasetCatalog` versioning backbone) | engine-local as-of index + lineage map over the existing PIT panel + tsdb |
| S7.1 | **L4 `dist_pool`** — deterministic multi-node task distribution (order-fixed cross-machine merge) | engine-local multi-process `DetPool` over a deterministic IPC shim (the network lift is the request) |

**Sequencing consequence:** S3, S1, S7 can scaffold immediately against atx-core *contracts* (TDD red first),
green-gating as each L-tier lands; every unit ships an engine-side fallback so `p2` is never *blocked* on atx-core —
only *accelerated* by it (the `p1` S1–S8 precedent: ship the fallback, record the lift).

---

## v3 anti-roadmap (explicitly NOT doing in p2)

`p2` **lifts four items** `p1` deferred (because the substrate now exists), and **keeps the rest — including, now
explicitly, all of live/optimal execution.**

**Lifted (now in scope):**
- ~~No distributed/cluster compute~~ → **S7 (deterministic distributed eval)** — digest-invariant, not a best-effort
  scale-out; for mass mining.
- ~~No deep-learning / neural nets~~ → **S5** — but **only** behind `p1` S1's deflated-Sharpe/PBO gate and the S4
  robustness battery; the "ML overfits at this N" warning is honored by gating, not ignored.
- ~~No alternative data~~ → **S6** — but as **plugged datasets** through the BYO abstraction (not special-cased),
  **only** PIT-versioned and behind the bias-audit battery.
- ~~No intraday~~ → **S9.2** — sub-daily bars as a richer *signal* substrate; not tick-by-tick HFT, and **not** an
  execution layer (below).

**Still NOT doing:**
1. **No live broker / market connectivity (delegated).** `p2` is a research + backtesting engine. Live order routing,
   OMS/EMS, streaming adapters, paper/real-broker bridges, sim↔live parity — **all delegated to broker execution
   algorithms** and out of scope. `p2` proves we can *find* robust alpha; the broker trades it.
2. **No optimal-execution / microstructure modeling (delegated).** Almgren-Chriss child-order scheduling, VWAP/TWAP/IS
   slicing, LOB-lite / queue-position fill models — out of scope. The `p0` `ExecutionSimulator`'s √-impact fill +
   S6-calibrated cost is retained **only to gate admission** (honest net-of-cost backtesting), not to schedule trades.
   Execution scheduling is the broker's job.
3. **No tick-by-tick HFT / co-located microsecond strategies.** S9 reaches intraday (sub-daily bars) as *signal*; the
   matching-engine / latency-arbitrage regime is a different system.
4. **No full exchange matching engine.** No venue, no order book — `p2` never touches a market.
5. **No cross-machine job orchestration framework.** S7 is a deterministic *eval* distributor, not a general
   Kubernetes/Spark-style scheduler — it does one thing (fan the digest-invariant mining workload) and proves
   invariance.
6. **No UI / dashboard.** Headless engine + reproducible artifacts; every report (S2/S8) emits files, not pixels.
7. **No new general-purpose primitives in the engine.** QP solver, NN kernels, distributed pool, as-of frame,
   residualizer, NSGA-II sort → **atx-core requests** (Pattern B above), with engine-side fallbacks only until the
   L-tier kernels land.
8. **No abandoning the daily-bar correctness model.** Intraday (S9.2) is additive — the daily-bar PIT spine stays the
   trustworthy default; intraday is a separate, explicitly-gated signal path.
9. **No in-engine data-format readers.** S6 is a typed in-memory dataset *catalog* + *contract* — CSV/Parquet/Arrow/JSON
   parsing stays the user's (or atx-tsdb's) job, lowered into a `data::Dataset` at the boundary. The engine ingests
   aligned arrays/spans + a schema, never a file format.

When scope creep argues, the strategic-positioning table + this anti-roadmap settle it.

---

## Strategic decisions — open forks (resolve at sprint-close cadence)

Per `module.md` §10, the act of writing the decision down removes the fork. These are open at module open:

- **Fork: is `p2` S4's multi-objective search a *generalization* of `p1` S3's scalar miner, or a *replacement*?**
  Provisional: **generalization** — S4 must reduce bit-for-bit to S3's seeded loop in the single-objective /
  novelty-off / cost-off limit (the regression anchor). *Resolve at S4 close once the boundary pin is proven.*
- **Fork: `op_swap` fix — repair the VM SlotPool corruption, or constrain the mutation to provably-safe buckets?**
  Provisional: **repair the root cause** (the analyze-valid-but-VM-corrupting genome is a real VM/typecheck gap worth
  closing) and re-enable across all buckets; bucket-constraint is the fallback if the root cause proves deep.
  *Resolve at S3.4.*
- **Fork: regression-residual neutralization — group-dummies only, or group + style (log-cap/beta)?** Provisional:
  **general residualizer** (regress on group dummies *and* a style block), with `indneutralize` exposed as the
  demean-only special case. *Resolve at S3.1.*
- **Fork: lockbox size + split.** Provisional: **a single contiguous terminal holdout** (most recent N% of the panel),
  sealed to the entire factory/library until the one S8 open. Embargo width inherited from `p1` S1's CPCV. *Resolve at
  S4.4 kickoff.*
- **Fork: behavioral-novelty distance metric — signal-return correlation, or rank-IC profile?** Provisional:
  **signal-return-profile correlation** over a held-out window (cheap, deterministic, reuses the corr machinery);
  rank-IC profile is a residual if correlation collapses distinct behaviors. *Resolve at S4.2.*
- **Fork: S6 abstraction scope — RESOLVED at re-theme (2026-06-14).** Deliverable = a **full dataset catalog/registry**
  (typed PIT-versioned `Dataset` + `DatasetCatalog` + `DataContext`), **not** in-engine file readers (parsing stays the
  user's job — anti-roadmap #9); alt-data is **subsumed** as plugged datasets while multi-asset/intraday **defers to
  S9**; external precomputed signals enter **both** as ML features **and** as gated `library::Library` admissions; the
  factor-model seam is **full BYO** (external X/F/D) **plus** reference-dataset ingestion into the internal builder. The
  open per-unit forks (e.g. the exact alignment policy for ragged BYO grids, whether `FactorModelArtifact` lives in
  `data::` or `risk::`) freeze in the S6 impl-plan at kickoff.

---

## Recommended sequencing

If you can only do one sprint: **S3** (alpha DSL & expression substrate) — it is the spine's first half; a smarter
search, a deeper library, and the whole robustness proof are incoherent if the language can't express the real signals
(no regression-residual neutralization, no BRAIN `ts_*`, a crippled `op_swap`). Widen the language before you search
it harder.

If two: add **S4** (genetic search & robust signal pipeline) — the spine's second half and the headline deliverable:
multi-objective search over the S3-widened space, and the battery that *proves* the admitted alpha is robust
(synthetic-recovery, regime/walk-forward, lockbox-reservation, cost-aware). S3→S4 together are why `p2` exists.

If three: add **S1** (constrained multi-horizon optimizer) — independent book-construction track, opens concurrently;
the portfolio-scale instrument that measures the robust alpha S3/S4 produce.

Then **S2** (meta-book, on S1), **S6** (the BYO-data abstraction layer — a foundational retrofit that makes the engine
*bring-your-own-data*; opens concurrently since it only touches the existing Panel/feature/factor/library seams and
pins bit-for-bit on the price-only path), **S5** (deep-learning signals — robustness-gated once S4's battery lands),
**S7** (distributed scale for mass mining, independent), and finally **S8** (the robust-alpha capstone — run the whole
pipeline E2E over the BYO `DataContext` and open the sealed lockbox exactly once). **S9** (multi-asset/region +
intraday universe breadth) extends the proven engine *after* the capstone.

**Two tracks run in parallel from day one:** the *deepen-the-signal* track (S3 → S4 → S5 → S8) and the
*construct-the-book* track (S1 → S2), with S7 independent infra. They converge at S8.

---

*Sprint discipline: same as `p0`/`p1` — see [`../docs/sprint.md`](../docs/sprint.md). Module structure: per
[`../docs/module.md`](../docs/module.md). Code-quality gate: per
[`../docs/implementation-quality.md`](../docs/implementation-quality.md) and
[`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Research north-stars:
[`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) ·
[`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) ·
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md) ·
[`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md).
Sibling module: **p1** — atx-engine v2 alpha factory + operating book. See [`../p1/`](../p1/).*
