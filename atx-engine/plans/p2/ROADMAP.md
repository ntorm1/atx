# Module p2 — atx-engine v3: The Unified Fund — Live-Capable, Multi-Horizon, Multi-Strategy Book at Scale (`p2`)

**Last reviewed:** 2026-06-13
**Started:** planned; no sprint open yet (assumes `p1` S1–S8 closed — S7/S8 treated as DONE per the user's kickoff directive).
**Source:** distills [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
+ [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
+ [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md),
reviewed against the as-built `p1` factory + operating book (S1–S8) and the frozen `p1` invariants.
The `p2` frontier is exactly the set of items `p1`'s anti-roadmap **explicitly deferred to a later module** — live
broker connectivity, distributed/cluster compute, deep-learning signals, alternative data, multi-asset/intraday.
Governed by [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md).
**Goal:** Take `p1`'s *industrialized alpha factory + one deterministic operating book* and turn it into a
**unified fund** — many sleeves across **horizons** and signal families (formulaic GA · learned ML · deep-learning),
combined under **one constrained multi-horizon portfolio optimizer**, executed through a **live-capable execution
spine** with realistic microstructure, **scaled across machines**, and fed by **alternative data** — the convergence
of WorldQuant's *mega-alpha-at-scale* and RenTech's *single unified live book*, **without weakening a single one of
`p0`/`p1`'s correctness invariants.**

---

## What changes from p1 → p2

`p0` answered *"can I trust one backtest?"* (the skeleton: data → DSL → backtest → combine → risk).
`p1` answered *"can I operate a factory?"* — mass discovery (S3 GA + S5 ML), a living library (S4), honest gating at
scale (S1 DSR/PBO/CPCV), calibrated cost + capacity (S6), vendor-grade covariance (S8), and **one** deterministic
multi-period operating book (S7).

`p2` answers *"can I run it as a **fund**?"* — **how many books at once** (multi-strategy sleeves across universes and
horizons, netted into one), **how does capital flow across horizons** (a true constrained multi-horizon optimizer, not
a chained single-period driver), **how does it touch the market** (a live-capable OMS/EMS with sim↔live parity and
optimal execution), **how does it scale past one box** (deterministic distributed compute), **how much breadth can the
inputs carry** (alternative data + multi-asset). The differentiator moves from *"a believable backtest,
industrialized"* to *"a believable backtest, industrialized, **and operated as a unified fund that could trade
live.**"*

| Dimension | `p1` baseline (S1–S8) | **`p2` v3 target** |
|---|---|---|
| Optimizer | single-period proximal solve, chained receding-horizon over a schedule (S7) | **true constrained multi-horizon convex program** (multiple decay horizons; full factor/sector/beta/turnover-budget constraint algebra; fixed-iteration QP/ADMM) |
| Book count | one unified mega-alpha book (S7) | **N sleeves** (universe × horizon × signal-family) netted into **one fund** with cross-sleeve risk budgeting + internal trade crossing |
| Signal families | formulaic GA (S3) + learned ridge/GBT/HMM (S5) | + **deep-learning sequence alphas** (temporal conv / GRU-lite / attention-lite / autoencoder factors), deflated-gated |
| Execution | √-impact fill model in `ExecutionSimulator` (p0) | + **optimal execution** (Almgren-Chriss scheduling, child-order slicing, microstructure-aware fills, internal crossing) |
| Live | simulator only (anti-roadmap #1) | **live-capable spine** (OMS/EMS + streaming adapter + paper-broker bridge + sim↔live parity gate) |
| Data | price-volume + classifications, US-equity daily bars | + **alternative data** (fundamental/analyst/news/sentiment, PIT-versioned) + **multi-asset/region + intraday** |
| Compute | deterministic single-box multicore (S2) | **deterministic distributed/cluster** (order-fixed cross-machine merge; digest == single-box) |
| Identity | a believable backtest, industrialized | a believable backtest, industrialized, **operated as a live-capable unified fund** |

When scope creep argues, this table, the carried-forward invariants (below), and the v3 anti-roadmap are where it goes
to die.

---

## Companion docs index

| Doc | Type | Covers |
|---|---|---|
| [`sprint-1-multi-horizon-optimization-implementation-plan.md`](sprint-1-multi-horizon-optimization-implementation-plan.md) | implementation plan | **S1** frozen *how* — constraint algebra, fixed-iteration QP/ADMM, multi-horizon forecast trajectory, Gârleanu-Pedersen aim portfolio, integration with S7/S8. The first sprint's per-unit plan. |

**Pending (created at each sprint kickoff/close):**
- `sprint-{N}-<theme>.md` — sprint spec (the *what*) for S2–S8. **For S1, the embedded ROADMAP section below is the
  spec** (the implementation plan is the *how*); standalone specs for S2–S8 are written at their kickoffs.
- `sprint-{N}-<theme>-implementation-plan.md` — frozen per-unit *how*, written at each sprint kickoff (NOT now).
- `sprint-{N}-progress.md` — sprint ledgers, opened at kickoff (sub-sprints `Sn-a/Sn-b` if a sprint exceeds ~7 units,
  per the `p0` 4a/4b/4c precedent).
- `sprintN.md` — user reference, written at each sprint close.

**Sprint/module discipline:** unchanged — [`../docs/sprint.md`](../docs/sprint.md),
[`../docs/module.md`](../docs/module.md), [`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Unit numbering:** `p2` sprints are **S1…S8**; ledger units are **`S{n}-{m}`** (e.g. `S1-0` marker, `S1-1`…). These
labels are namespaced by `p2/`; when cross-referencing `p1` (which also uses S1…S8), qualify as **"p2 S1"** vs
**"p1 S1"**. Renumber-don't-reuse on carry-forward (`sprint.md` §"Carry-forward").

---

## Sibling / upstream modules

- **Upstream:** `atx-core` stdlib + `atx-tsdb`. The engine remains a pure **consumer** of atx-core L0–L9 and adds no
  general-purpose primitives. `p2` leans hardest on **L4** (`concurrent` — now extended from a single-box pool to a
  **multi-node deterministic work distributor**, S7), **L7** (`linalg`/`solve` — a real **QP/ADMM** solver for S1, and
  **deterministic NN primitives** for S5), **L8** (`time`/`domain` — a wall-clock source distinct from the sim clock,
  S3), and **L9** (`frame` — PIT as-of versioning for alternative data, S6). Every new numeric/infra need is an
  **atx-core request raised as a cross-module edge here** (Pattern B, table below), never built inside the engine.
- **Sibling:** `p1` — atx-engine v2, the alpha factory + operating book. See [`../p1/`](../p1/). `p2` builds strictly
  **on top of** the closed `p1` seams (`risk::{PortfolioOptimizer, MultiPeriodOptimizer, FactorModel}`,
  `book::{BookPipeline, BookReport, allocation}`, `library::Library`, `combine::{AlphaCombiner, CombinedSignalSource}`,
  `factory::ResearchDriver`, `eval::*`, `cost::*`, `parallel::DetPool`) and must not re-duplicate any of them.

---

## Strategic positioning (v3)

`p0` earned the right to be *believed*; `p1` earned the right to be *productive*; `p2` earns the right to be
*operated*. The identity shifts from **"a deterministic bias-free alpha factory"** to **"a deterministic bias-free
alpha factory that runs as a live-capable unified fund."** The distinction between a research platform and a fund is
not a cleverer signal — `p1` already mass-produces and validates signals — it is the **operating layer**: many books
across horizons combined under one constrained objective, executed against the market with the cost model RenTech
calls its "secret weapon," scaled past one machine, and runnable on the *same code path* live as in sim.

| Capability | RenTech (Medallion) | WorldQuant (BRAIN) | **atx-engine v3 target** |
|---|---|---|---|
| Optimizer | one unified model, re-optimized several times/hour (§6.1) | bounded-regression / O(N) mega-alpha (§6.1/§6.2) | **constrained multi-horizon convex program** over S8's cleaned `V` (S1) |
| Book structure | **single** unified book across asset classes (Laufer, §6.1) | mega-alpha pool + SuperAlphas (§4.1) | **N sleeves netted into one fund** with cross-sleeve risk budget (S2) |
| Horizon | seconds → multi-day; **~2-day avg hold**, cost-throttled (§5.1) | ~0.6–6.4-day holding (101-Alphas) | **explicit multi-horizon** (fast/slow signal-decay buckets) optimized jointly (S1) |
| Execution | proprietary cost model = the "secret weapon"; self-correcting / internal crossing (§7.2) | "automatic internal crossing of trades" (§6.2) | **Almgren-Chriss scheduling + child-order slicing + cross-sleeve netting** (S4/S2) |
| Live vs sim | one codebase, sim and live share the path (§9.5) | WebSim → real capital | **sim↔live parity gate; one code path** (S3) |
| Signals | statistical ensemble; HMM/noisy-channel heritage (§4.2) | formulaic + ML; 125k+ data fields (§8) | GA + ML + **deep-learning sequence alphas**, deflated-gated (S5) |
| Data | petabyte PIT warehouse; obsessive cleaning (§6.2) | 125,000+ PIT data fields, 17 regions (§8) | **alt-data (PIT-versioned) + multi-asset/region + intraday** (S6) |
| Compute | big iron, re-optimize hourly (§6.1) | WebSim global fan-out, 250k users (§4.1) | **deterministic distributed/cluster** (digest == single-box, S7) |

The two north-stars converge on one operating thesis: *a single unified book — fed by millions of weak,
mostly-uncorrelated signals across horizons, combined honestly, executed with exact cost — re-optimized continuously
and run live.* `p1` built the signals and one book; `p2` builds the **fund that operates them.**

---

## Assumed baseline — `p0` (Phases 1–4) + `p1` (S1–S8), treated as DONE

`p2` builds strictly **on top of** the following and must **not** re-duplicate any of it; it extends through the
established seams.

- **p0 Phases 1–4** — event spine, deterministic bar-driven backtest loop, alpha-DSL + vectorized VM, mega-alpha
  combiner + Barra factor risk + turnover-penalized single-period optimizer + the **fit/apply firewall**. ✅
- **p1 S1 — Evaluation & Validation Spine** — `PerfMetrics`, Deflated-Sharpe, PBO, purged+embargoed CPCV, bias-audit
  gate battery. The truth layer every `p2` signal/sleeve admission reuses **verbatim**. ✅
- **p1 S2 — Parallel Compute Substrate** — `parallel::DetPool` + order-fixed `parallel_evaluate`/`parallel_cpcv`;
  determinism survives parallelism (digest invariance). The local primitive `p2` S7 lifts to multi-node. ✅
- **p1 S3 / S4 / S4b — Formulaic factory + persistent library + ResearchDriver** — GA mining, canonical-hash dedup,
  correlation-neighbor index, lifecycle state machine, continuous mine→admit→replay. ✅
- **p1 S5 — Learned signals & ML combiner** — PIT feature matrix, ridge/lasso/elastic-net/GBT learned alphas, HMM
  regimes, deflated-gated ensemble combiner. The base `p2` S5 deep-learning extends. ✅
- **p1 S6 — Cost calibration & capacity** — calibrated δ/Y/γ/η, Almgren-Chriss temp/perm split, per-alpha/mega
  capacity curves, cost-aware gating, borrow accrual. ✅
- **p1 S7 — Portfolio construction & lifecycle** — `risk::MultiPeriodOptimizer` (receding-horizon driver over
  `PortfolioOptimizer::solve`), `book::DecayMonitor`, dead-alpha→risk-factor, capacity-bounded Kelly allocation,
  `book::BookReport`, `book::BookPipeline` E2E. **The single-book operating layer `p2` S1/S2 generalize.** ✅ (assumed)
- **p1 S8 — Vendor-grade covariance** — robust regression, EWMA+Newey-West factor cov, eigenfactor de-biasing,
  specific-risk blend, VRA, APCA, shrinkage/PSD toolkit. **The cleaned `V` the `p2` S1 optimizer trades.** ✅ (assumed)

> **The hard dependency:** `p2` S1 (multi-horizon optimizer) consumes `p1` S7's `MultiPeriodOptimizer` + S8's
> `FactorModel`. S2 (meta-book) consumes S1. S3 (live spine) and S7 (distributed) need **no** new `p1` capability and
> can open first on the infra track. S5/S6 (signal breadth) consume `p1` S1's validation + S4's library.

---

## The v3 sprint arc

> Status rows stay `⏳ proposed` and are **not** promoted to commitments until a sprint actually opens
> (`module.md`: don't document hopes as commitments). Each sprint freezes its implementation plan at kickoff.
> Effort/unit counts are estimates to re-scope at kickoff (4–7 units/sprint; a sprint that would exceed ~7 splits into
> `Sn-a`/`Sn-b`).

### Dependency DAG

```
                        p1 complete (S1–S8 closed)
                                  │
        ┌─────────────────────────┼──────────────────────────┐
        ▼                         ▼                            ▼
  S1 multi-horizon          S3 live spine               S7 distributed
   optimizer (SPINE)        (sim↔live parity)           (lifts p1-S2 to N nodes)
        │                         │                            │
        ▼                         ▼                            │
  S2 multi-strategy         S4 optimal execution               │
   meta-book ◄──────────────────┘  (microstructure)            │
        │                                                       │
        ├──────────────► S5 deep-learning alphas (gated by p1-S1 + p2-S1)
        ├──────────────► S6 alt-data + multi-asset + intraday
        ▼                                                       ▼
       S8 — The Unified Fund: full integration + live-readiness capstone
            (consumes S1·S2·S3·S4·S5·S6·S7 — the p2 done-gate)
```

> **Two tracks run in parallel from day one** (the `p1` discipline): the *operate-the-book* track
> (S1 → S2 → S5/S6 → S8) and the *touch-the-market-at-scale* track (S3 → S4, and S7 independently). They converge at
> S8. **S1 is the spine** — like `p1` S1 was the truth layer, `p2` S1 is the layer everything routes through; nothing
> downstream is coherent without it.

---

### S1 — Constrained Multi-Horizon Portfolio Optimization  ✅ DONE (branch `feat/p2-s1-multi-horizon`, pending merge — merge is the user's gate) ([impl-plan](sprint-1-multi-horizon-optimization-implementation-plan.md) · [progress](sprint-1-progress.md))
**Theme:** The spine — **the optimizer everything routes through.** `p1` S7 ships a *receding-horizon driver* that
chains single-period proximal solves over a schedule (threading `w_prev`, executing only the first move). That is
multi-*period* but not multi-*horizon*: each inner solve optimizes a single-period objective and the constraint set is
`{Σw=0, Σ|w|≤L, |w_i|≤cap}` only. S1 builds the **true constrained multi-horizon convex program**: signals carry an
explicit **decay horizon** (fast/slow); the objective optimizes today's book against a **forward trajectory** of
horizon-decayed forecasts `{α_t,…,α_{t+H}}` summed under risk-adjusted, cost-aware utility (Boyd et al. 2017,
*Multi-Period Trading via Convex Optimization*), executes only the realized first move (MPC, look-ahead-safe); a
**full constraint algebra** (factor-exposure `|Xᵀw|≤b` over S8's `X`, sector/group caps, beta neutrality, per-horizon
turnover budgets, gross/net leverage, position/ADV caps, no-trade bands); a **fixed-iteration QP/ADMM** solver
(OSQP-style, pre-factorized KKT — determinism by fixed iteration count, no convergence early-exit) that handles the
richer constraint set exactly; and the **Gârleanu-Pedersen aim portfolio** (trade partway toward a forward-blended
target — the closed form for quadratic costs that generalizes S7's scalar `trade_rate`). Trades S8's cleaned `V`;
consumed by S2 and S8. *Grounded in RenTech §5.1/§6.1 (cost-throttled multi-horizon, single unified re-optimized book),
WorldQuant §6.1/§6.5 (bounded regression, turnover-as-cost), Boyd 2017, Gârleanu-Pedersen 2013, OSQP (Stellato 2020).*
**Needs only `p1` S7 + S8** — opens first.

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

### S2 — Multi-Strategy Meta-Book & Risk Budgeting  ⏳ proposed
**Theme:** Tie it together at the **fund** level — Laufer's single unified book, made multi-sleeve. A
**`Strategy`/`Sleeve`** abstraction (each sleeve = a universe × horizon × signal-family book wrapping its own S1
optimizer + library subset), a **meta-allocator** that combines sleeves into one fund (cross-sleeve weights via
risk-budgeting / a portfolio-of-books fractional-Kelly that extends `p1` S7's `book::allocation`), a **cross-sleeve
risk model** (one shared factor model; aggregate factor exposure and sleeve-correlation netting), and **internal trade
crossing/netting** — sum sleeve target positions into one fund order before execution so offsetting child trades
cancel (the WorldQuant "automatic internal crossing of trades" cost saving, §6.2; RenTech's self-correcting rebalancer,
§7.2). Fund-level reporting with **attribution-by-sleeve**. *Grounded in RenTech §6.1 (single unified book),
WorldQuant §6.2 (internal crossing), Grinold-Kahn (risk budgeting across breadth sources).* Consumes S1.

| # | Unit | Effort | Status |
|---|---|---|---|
| S2.0 | Marker + ledger | S | ⏳ |
| S2.1 | `Sleeve` abstraction (universe × horizon × signal-family; wraps an S1 optimizer + a library subset; PIT) | M | ⏳ |
| S2.2 | Meta-allocator (sleeves → one fund; cross-sleeve risk budget; portfolio-of-books Kelly extending `book::allocation`) | L | ⏳ |
| S2.3 | Cross-sleeve risk model (shared `FactorModel`; aggregate exposure; sleeve-return correlation) | M | ⏳ |
| S2.4 | Internal trade crossing/netting (net sleeve targets into one fund order; net-cost saving measured) | M | ⏳ |
| S2.5 | Fund-level report + attribution-by-sleeve + close | M | ⏳ |

### S3 — Live-Capable Execution Spine & Sim/Live Parity  ⏳ proposed
**Theme:** From simulator to **live-capable** — the item `p1` anti-roadmap #1 deferred. An **OMS/EMS seam** (an order
lifecycle state machine: `new→sent→ack→partial→filled→canceled/rejected`, idempotent and deterministic-replayable), a
**streaming/real-time data adapter** (a live `IDataHandler` variant that honors the *same* structural PIT contract — a
**wall-clock** `L8` time source distinct from the monotonic sim clock), a **paper-trading bridge** (an `IBroker`
interface + a deterministic simulated-broker / paper impl — **no real broker credentials in scope**, an adapter
interface only), **order/fill reconciliation + crash-safe replayable journal**, and the load-bearing **sim↔live parity
gate**: the *same* `book::BookPipeline` code path drives sim and (paper) live, and a parity test proves the live
decision stream matches the sim decision stream on identical inputs (RenTech §9.5 "sim and live share the path"; the
backtest-loop deep-dive §0 per-slice crank + §1 LEAN/Zipline settle-before-decide ordering). *Grounded in RenTech §9.5,
backtest-loop deep-dive §0/§1.* **Infra track — no new `p1` dependency**; can open concurrently with S1.

| # | Unit | Effort | Status |
|---|---|---|---|
| S3.0 | Marker + ledger | S | ⏳ |
| S3.1 | OMS/EMS order-lifecycle state machine (idempotent, deterministic-replayable; reuses p0 Signal/Order/Fill payloads) | L | ⏳ |
| S3.2 | Streaming/real-time data adapter (live `IDataHandler`; wall-clock `L8` vs sim-clock; same structural PIT gate) | M | ⏳ |
| S3.3 | `IBroker` seam + deterministic paper/simulated-broker bridge (no live creds) | M | ⏳ |
| S3.4 | Order/fill reconciliation + crash-safe replayable journal (state recovery) | M | ⏳ |
| S3.5 | **Sim↔live parity gate** (same `BookPipeline` path; matching decision stream proof) + close | M | ⏳ |

### S4 — Optimal Execution & Microstructure  ⏳ proposed
**Theme:** The "secret weapon," deepened from a fill model to an **execution policy**. `p0`'s `ExecutionSimulator`
prices a single parent fill with √-impact; S4 adds the **execution layer above it**: an **Almgren-Chriss optimal
execution scheduler** (the temp/perm split `p1` S6 calibrated, turned into an optimal trade trajectory over an
execution horizon under a risk-aversion knob), **child-order slicing + participation control** (VWAP/TWAP/Implementation-
Shortfall schedules; a POV cap), a **microstructure-aware fill model** (LOB-lite / queue-position / spread-crossing for
the intraday execution horizon), **execution alpha** + **internal-crossing-aware net cost** (S2's netting feeds the
scheduler so crossed trades incur no impact). *Grounded in backtest-loop deep-dive §0 (Almgren-Chriss `γ=0.314`,
`η=0.142`; √-law `I=Y·σ√(Q/V)`), RenTech §5.1/§7.2 (cost-throttled, self-correcting, ~2-day avg hold from seconds-scale
capability), WorldQuant §6.2 (internal crossing).* Consumes S3 (the OMS the scheduler emits child orders into) + S2
(netting).

| # | Unit | Effort | Status |
|---|---|---|---|
| S4.0 | Marker + ledger | S | ⏳ |
| S4.1 | Almgren-Chriss optimal execution scheduler (temp/perm trajectory; risk-aversion; reuses S6-calibrated coeffs) | L | ⏳ |
| S4.2 | Child-order slicing + participation control (VWAP/TWAP/IS schedules; POV cap) | M | ⏳ |
| S4.3 | Microstructure-aware fill model (LOB-lite / queue position / spread crossing; intraday) | L | ⏳ |
| S4.4 | Execution alpha + internal-crossing-aware net cost (S2 netting → scheduler) | M | ⏳ |
| S4.5 | Intraday execution horizon integration + bench (impact vs schedule) + close | M | ⏳ |

### S5 — Deep-Learning Sequence Alphas  ⏳ proposed
**Theme:** The deferred NN frontier — `p1` anti-roadmap #7: *"NN ensembles are p2-or-later, gated by evidence the
simpler combiners are saturated."* S5 honors that gate by building NN signals **only behind `p1` S1's deflated-Sharpe
/ PBO admission battery** (the validation spine exists precisely so this is safe). A **PIT sequence-feature tensor
builder** (extends `p1` S5's feature matrix to temporal windows — no look-ahead), a **deterministic NN training
substrate** (seeded, fixed-iteration, order-fixed — the determinism invariant's hardest stress point; the NN kernels
are an atx-core Pattern-B request), **temporal-conv / GRU-lite** sequence alphas + an **attention-lite / autoencoder
statistical-factor** alpha (the noisy-channel / latent-state heritage, RenTech §3.2/§4.2, expressed as a learned
sequence model), each emitting a cross-section that **plugs into the pool exactly like a formulaic alpha** and is
admitted only through the deflated gate. *Grounded in RenTech §3.6/§4.2 (statistical sequence modeling, HMM/noisy-
channel heritage), WorldQuant §3.2 (the millions-of-weak-alphas thesis at the model frontier), `p1` S1/S5.* Consumes
`p1` S1 (gate), S2/S4b (library), `p2` S1 (the optimizer it feeds).

| # | Unit | Effort | Status |
|---|---|---|---|
| S5.0 | Marker + ledger | S | ⏳ |
| S5.1 | PIT sequence-feature tensor builder (temporal windows over `p1` S5 features; no look-ahead) | M | ⏳ |
| S5.2 | Deterministic NN training substrate (seeded, fixed-iteration, order-fixed) + temporal-conv/GRU-lite alpha — atx-core L7 NN request | L | ⏳ |
| S5.3 | Attention-lite / autoencoder statistical-factor alpha (latent-state heritage) | L | ⏳ |
| S5.4 | Deflated-Sharpe + PBO admission gate for NN alphas (reuses `p1` S1 verbatim; the anti-overfit firewall) | M | ⏳ |
| S5.5 | NN-alpha → library + combiner/sleeve integration + bench + close | M | ⏳ |

### S6 — Alternative Data & Multi-Asset Universe  ⏳ proposed
**Theme:** More data, more breadth — the deferred data frontier (`p1` anti-roadmap #3), opened **cautiously, behind the
PIT + bias-audit gates.** WorldQuant exposes **125,000+ PIT data fields across 17 regions** (§8); RenTech's first
durable edge was *better, deeper, cleaner data* (§6.2/§7.1). S6 adds **PIT alternative-data datafield ingestion**
(fundamental/analyst with as-of / effective-date / restatement versioning — the report-date-vs-effective-date trap),
**news/sentiment/event datafields** (PIT-safe, surfaced through the existing `trade_when` event-gating operator), a
**multi-asset / multi-region universe** (beyond US-equity daily bars; cross-asset correlation in the risk model), an
**intraday-bar universe + horizon** (sub-daily panel feeding S4), and **alt-data bias-audit gates** that extend `p1`
S1's battery to the data-snooping risk at 125k-field scale. *Grounded in WorldQuant §8/§4.3 (datafields, universes,
regions, delay), RenTech §7.1/§9.1 (PIT correctness, as-of versioning, survivorship), `p1` S1 (bias audits).* Consumes
`p1` S1; feeds S5 (richer features) + S4 (intraday).

| # | Unit | Effort | Status |
|---|---|---|---|
| S6.0 | Marker + ledger | S | ⏳ |
| S6.1 | PIT alt-data datafield ingestion (fundamental/analyst; as-of/effective-date/restatement versioning) | L | ⏳ |
| S6.2 | News/sentiment/event datafields (PIT-safe; `trade_when` event gating) | M | ⏳ |
| S6.3 | Multi-asset / multi-region universe (cross-asset correlation in risk model) | L | ⏳ |
| S6.4 | Intraday-bar universe + horizon (sub-daily panel; feeds S4) | M | ⏳ |
| S6.5 | Alt-data bias-audit gates (extends `p1` S1 battery; 125k-field data-snooping) + close | M | ⏳ |

### S7 — Distributed / Scale-Out Compute  ⏳ proposed
**Theme:** Lift the scale ceiling — the deferred distributed compute (`p1` anti-roadmap #4: *"Single-box multicore is
the scale ceiling for v2 … cross-machine eval = p2"*). WorldQuant's WebSim fans hundreds of thousands of backtests/day
across a global population (§4.1/§8); RenTech re-optimizes a unified book over a petabyte warehouse (§6.1/§6.2). S7
extends `p1` S2's `parallel::DetPool` from one box to **N nodes** while preserving the non-negotiable: **the
distributed result digest must equal the single-box digest.** A **deterministic distributed work distributor**
(order-fixed cross-machine merge — the S2 reduce-by-sort lifted across the network; an atx-core L4 multi-node request),
a **sharded distributed library** (the S4 library partitioned across nodes with a consistent dedup + correlation
index), **distributed batch-eval + distributed CPCV** (mining/eval/folds fanned across nodes), the **digest-invariance
proof** (`{1 node} == {N nodes}` byte-identical) + **deterministic fault tolerance** (a failed shard re-runs to the
same result). *Grounded in WorldQuant §4.1/§8 (global fan-out scale), RenTech §6.1/§6.2 (continuous re-optimization at
scale), `p1` S2 (the determinism-under-parallelism invariant this must not break).* **Needs only `p1` S2** — independent
infra track.

| # | Unit | Effort | Status |
|---|---|---|---|
| S7.0 | Marker + ledger | S | ⏳ |
| S7.1 | Deterministic multi-node work distributor (order-fixed cross-machine merge; extends `DetPool`) — atx-core L4 request | L | ⏳ |
| S7.2 | Sharded distributed library (partitioned store; consistent dedup + correlation-neighbor index) | L | ⏳ |
| S7.3 | Distributed batch-eval + distributed CPCV (fan mining/eval/folds across nodes) | M | ⏳ |
| S7.4 | Digest-invariance proof (`1 node == N nodes` byte-identical) + deterministic fault tolerance (failed-shard replay) | M | ⏳ |
| S7.5 | Scale bench (3–5k universe, 10⁶–10⁹ alphas across nodes) + close | M | ⏳ |

### S8 — The Unified Fund: Full Integration + Live-Readiness Capstone  ⏳ proposed
**Theme:** Tie **everything** together and prove it — the `p2` done-gate. A **full-fund orchestrator** that runs the
whole pipeline top-to-bottom: alt-data (S6) → GA + ML + deep-learning signals (S3 factory · S5 NN) → library (`p1` S4)
→ **constrained multi-horizon optimize** (S1) → **multi-strategy meta-book + internal crossing** (S2) → **optimal
execution** (S4) → **live-capable spine with sim↔live parity** (S3) → **distributed at scale** (S7) → reproducible
fund report + **live-readiness report** (fund-scale capacity, sim/live parity, risk-budget utilization). The capstone
test proves **all carried-forward invariants simultaneously** — determinism (incl. the seeded NN + distributed paths),
no look-ahead (truncation-invariance at every fitted boundary + the live wall-clock gate), no survivorship, honest
calibrated cost, **and sim↔live parity** — top-to-bottom at fund scale. *Grounded in RenTech §6.1/§9.5 (one unified
live book), WorldQuant §4.1/§6 (mega-alpha at scale), the full carried-forward `p0`/`p1` invariant set.* Consumes
**everything**.

| # | Unit | Effort | Status |
|---|---|---|---|
| S8.0 | Marker + ledger | S | ⏳ |
| S8.1 | Full-fund orchestrator (alt-data → GA+ML+NN → library → S1 optimize → S2 meta-book → S4 exec → S3 live → S7 distributed) | L | ⏳ |
| S8.2 | Live-readiness report (fund-scale capacity, sim/live parity, risk-budget utilization; reproducible headless artifact) | M | ⏳ |
| S8.3 | All-invariants-simultaneously capstone test (determinism + PIT + no-survivorship + honest-cost + sim↔live parity, fund scale) | L | ⏳ |
| S8.4 | End-to-end fund-scale bench + `p2` close ceremony (residuals lifted, ROADMAP bumped, user refs) | M | ⏳ |

---

## Carried-forward invariants (non-negotiable — every sprint, proven by test not hope)

These are `p0`'s + `p1`'s load-bearing guarantees. `p2` **must not** weaken any; live (S3), deep learning (S5), and
distributed (S7) are the three places they are easiest to break, so each gets an explicit proof.

1. **Determinism.** Same input → byte-identical result digest. No RNG except **seeded, deterministic** training/search
   (S5 NN fits, any sampled execution) whose seed is part of the recorded artifact. Order-fixed reductions in canonical
   id order — **including the distributed cross-machine merge** (S7): the N-node digest must equal the single-box
   digest, and **the live (paper) decision stream must match the sim decision stream** on identical inputs (S3 parity
   gate).
2. **No look-ahead.** Every fitted object (combiner weights, gate stats, factor model, mined/learned/NN parameters,
   calibrated cost, regime states, multi-horizon forecast trajectory) is estimated on a trailing window and applied OOS
   only — the fit/apply firewall, extended by S1's purge+embargo. The MPC optimizer **executes only the first move** of
   its horizon (S1). The live adapter reads only `≤ wall-clock-now` (S3). **Truncation-invariance is the test.**
3. **No survivorship.** Delisted symbols pass through with their final bar across sleeves, the live spine, and every
   alt-data / multi-asset build.
4. **Point-in-time.** Out-of-universe / not-yet-knowable data reads NaN; alt-data as-of/restatement versioning (S6) and
   the live wall-clock gate (S3) transition on PIT boundaries only — no restated value leaks into a past decision.
5. **Honest cost.** Cost is a first-class decision input — S1's optimizer prices the S6-calibrated κ in its turnover
   budget; S4's scheduler trades the calibrated impact trajectory; S2's netting reduces it; capacity is reported at
   fund scale (S8).
6. **No hot-path allocation.** Steady-state loops (the rebalance crank, the live order loop, the per-node eval loop)
   allocate zero. Cold paths (solve, fit, NN train, schedule, distribute) may allocate.
7. **Differential correctness.** Any new compute kernel (the QP/ADMM solver, the multi-horizon trajectory, NN forward/
   backward, the execution scheduler, the distributed merge) ships with an obviously-correct reference and a
   bit-/ULP-bounded differential test.

---

## Cross-module dependencies (Pattern B — edges live here)

`p2` raises new `atx-core` numeric/infra needs. Each is an edge recorded here, not a primitive built in the engine.

| p2 unit | Needs from `atx-core` | Engine-side fallback (shippable now) |
|---|---|---|
| S1.2 | **L7 `qp_admm`** — fixed-iteration constrained QP (OSQP-style ADMM, pre-factorized KKT) | fixed-iteration ADMM/projected-proximal loop wrapping the as-built `risk::PortfolioOptimizer` projection primitives |
| S3.2 | **L8 `wall_clock`** — a real-time monotonic source distinct from the sim clock | engine-local steady-clock wrapper behind the `IDataHandler` seam |
| S4.1 | **L7 `almgren_chriss`** — closed-form optimal-execution trajectory | engine-local closed form over the S6-calibrated `(γ, η, σ)` |
| S5.2 | **L7 (or new L-tier) NN primitives** — deterministic autodiff / fixed-iteration SGD + conv/GRU/attention kernels | engine-local fixed-iteration seeded mini-batch trainer over `core::linalg` (the dedicated kernel is the lift) |
| S6.1 | **L9 `as_of_frame`** — PIT as-of / effective-date / restatement versioning in the frame/tsdb | engine-local as-of index over the existing PIT panel + tsdb |
| S7.1 | **L4 `dist_pool`** — deterministic multi-node task distribution (order-fixed cross-machine merge) | engine-local multi-process `DetPool` over a deterministic IPC shim (the network lift is the request) |

**Sequencing consequence:** S1, S3, S7 can scaffold immediately against atx-core *contracts* (TDD red first),
green-gating as each L-tier lands; every unit ships an engine-side fallback so `p2` is never *blocked* on atx-core —
only *accelerated* by it (the `p1` S1–S8 precedent: ship the fallback, record the lift).

---

## v3 anti-roadmap (explicitly NOT doing in p2)

`p2` **lifts five items** `p1` deferred (because the substrate now exists), and **keeps the rest.**

**Lifted (now in scope):**
- ~~No live broker / market connectivity~~ → **S3 (live-capable spine + paper bridge)** — but **paper/simulated only**;
  real-broker credentials/certification stay out (below).
- ~~No distributed/cluster compute~~ → **S7 (deterministic distributed eval)** — digest-invariant, not a best-effort
  scale-out.
- ~~No deep-learning / neural nets~~ → **S5** — but **only** behind `p1` S1's deflated-Sharpe/PBO gate; the
  "ML overfits at this N" warning is honored by gating, not ignored.
- ~~No alternative data~~ → **S6** — but **only** PIT-versioned and behind the bias-audit battery.
- ~~No intraday~~ → **S6.4 / S4** — sub-daily bars for execution; not tick-by-tick HFT (below).

**Still NOT doing:**
1. **No real-broker execution / certification.** S3 ships a paper/simulated-broker *interface*; wiring a real venue
   (FIX certification, credentials, exchange membership, regulatory/compliance surface) is out — `p2` proves
   live-*capability* and sim↔live *parity*, not production trading.
2. **No tick-by-tick HFT / co-located microsecond strategies.** S4/S6 reach intraday (sub-daily bars); the
   matching-engine / latency-arbitrage regime is a different system.
3. **No full exchange matching engine.** S4's microstructure model is LOB-*lite* (queue position / spread crossing for
   cost fidelity), not a venue.
4. **No cross-machine job orchestration framework.** S7 is a deterministic *eval* distributor, not a general
   Kubernetes/Spark-style scheduler — it does one thing (fan the digest-invariant workload) and proves invariance.
5. **No UI / dashboard.** Headless engine + reproducible artifacts; every report (S2/S8) emits files, not pixels.
6. **No new general-purpose primitives in the engine.** QP solver, NN kernels, distributed pool, as-of frame →
   **atx-core requests** (Pattern B above), with engine-side fallbacks only until the L-tier kernels land.
7. **No abandoning the daily-bar correctness model for sim.** Intraday (S6.4) is additive — the daily-bar PIT spine
   stays the trustworthy default; intraday is a separate, explicitly-gated path.

When scope creep argues, the strategic-positioning table + this anti-roadmap settle it.

---

## Strategic decisions — open forks (resolve at sprint-close cadence)

Per `module.md` §10, the act of writing the decision down removes the fork. These are open at module open:

- **Fork: is `p2` S1 a *generalization* of `p1` S7's optimizer, or a *replacement*?** Provisional: **generalization** —
  S1 must reduce bit-for-bit to S7's chained single-period book in the one-horizon / minimal-constraint limit (the
  regression anchor). *Resolve at S1 close once the boundary pin is proven.*
- **Fork: sleeves share one factor model, or one per sleeve?** Provisional: **one shared cross-sleeve `FactorModel`**
  (S2.3) so fund-level exposure aggregates cleanly; per-sleeve models are a residual if a sleeve's universe diverges.
  *Resolve at S2 kickoff.*
- **Fork: distributed transport — multi-process-on-one-box first, or network from the start?** Provisional:
  **multi-process deterministic IPC first** (proves digest-invariance without network nondeterminism), network as the
  S7 lift. *Resolve at S7 kickoff.*
- **Fork: deep-learning depth ceiling.** Provisional: **stop at temporal-conv / GRU-lite / attention-lite + autoencoder
  factors** (the models the research supports at this N); transformers/large-sequence models stay gated by evidence the
  S5 models are saturated. *Resolve at S5 close.*

---

## Recommended sequencing

If you can only do one sprint: **S1** (constrained multi-horizon optimizer) — it is the spine every later sprint routes
through; a multi-strategy fund, optimal execution, and the capstone are all incoherent without the optimizer that ties
horizons and constraints into one book. Like `p1` S1 was the truth layer, `p2` S1 is the operating spine.

If two: add **S2** (multi-strategy meta-book) — the "tie everything together" deliverable at the fund level, and the
direct consumer of S1.

If three: add **S3** (the live-capable spine) — independent of S1/S2, opens concurrently on the infra track; proves
sim↔live parity, the credibility gate for "operated as a fund."

Then **S4** (optimal execution, on S3), **S5 + S6** (signal/data breadth — parallelizable once S2 lands), **S7**
(distributed scale, independent), and finally **S8** (the unified-fund capstone — prove the whole pipeline E2E, live-
capable, at scale).

**Two tracks run in parallel from day one:** the *operate-the-book* track (S1 → S2 → S5/S6 → S8) and the
*touch-the-market-at-scale* track (S3 → S4, and S7 independently). They converge at S8.

---

*Sprint discipline: same as `p0`/`p1` — see [`../docs/sprint.md`](../docs/sprint.md). Module structure: per
[`../docs/module.md`](../docs/module.md). Code-quality gate: per
[`../docs/implementation-quality.md`](../docs/implementation-quality.md) and
[`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Research north-stars:
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md) ·
[`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) ·
[`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md).
Sibling module: **p1** — atx-engine v2 alpha factory + operating book. See [`../p1/`](../p1/).*
