# Module p1 — atx-engine v2: The Alpha Factory & Portfolio Brain (`p1`)

**Last reviewed:** 2026-06-13
**Status:** **S1 (Evaluation & Validation Spine) + S2 (Parallel Compute Substrate) + S3 (Formulaic Alpha Factory) + S4b (Automated Alpha Engine — factory↔library integration) CLOSED**
(`feat/atx-core-stdlib`; close ledgers [`sprint-1-progress.md`](sprint-1-progress.md),
[`sprint-2-progress.md`](sprint-2-progress.md), [`sprint-3-progress.md`](sprint-3-progress.md)). p0 Phases 1–4 are complete (Phase-4 mega-alpha layer
closed at `f2d22f4`). S4–S7 remain `⏳ proposed`; the factory track continues (S3 ✅ → **S4** → S5 → S7), and
S6 (cost) can open concurrently (S2 parallel is done — it handed S3 cross-core throughput).
**Source:** distills [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
+ [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md),
reviewed against the as-built `p0` engine and the frozen [`../p0/phase-4-signal-combination-risk-implementation-plan.md`](../p0/phase-4-signal-combination-risk-implementation-plan.md).
Governed by [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md).
**Goal:** Take the `p0` *trustworthy backtester* and turn it into a **full end-to-end quant pipeline** that
**discovers · predicts · combines · optimizes · evaluates massive alpha libraries** — the WorldQuant
"alpha factory" (mine 10⁵–10⁹ formulaic alphas) fused with the RenTech "one unified model" discipline
(learned signals, cost fidelity, ruthless out-of-sample validation), all preserving `p0`'s correctness
invariants at scale.

---

## What changed from p0 → p1

`p0` answered *"can I trust one backtest?"* — determinism, point-in-time, no survivorship, honest cost, a
DSL+VM that turns a string into a signal, and (P4) a layer that gates a *pool* and combines it into one
mega-alpha over a Barra risk model. **`p0` is the skeleton: data → DSL → backtest → combine → risk.**

`p1` answers *"can I operate a factory?"* — **where do the alphas come from** (automated discovery,
formulaic *and* learned), **how do millions of them live** (a persistent, deduplicated, lifecycle-managed
library), **how do I not fool myself at that scale** (deflated-Sharpe / PBO / purged CV gating *before*
anything is admitted), **how fast** (deterministic multicore), **how honest is the cost** (calibrated
impact + capacity), and **how does it run as a book** (multi-period construction + live decay monitoring +
dead-alpha recycling). The differentiator is no longer "a believable backtest" — it is **a believable
backtest, industrialized.**

| Dimension | `p0` baseline (incl. P4) | **`p1` v2 target** |
|---|---|---|
| Alpha origin | hand-authored strings | **mined** (genetic/param search over the DSL) **+ learned** (ridge/GBM/HMM) |
| Library size | a pool you load | **10⁵–10⁹**, persisted, deduped, lifecycle-managed |
| Combination | equal→rank→IC→shrink→bounded-reg (linear) | + **ML/ensemble combiner** + regime-conditional weighting |
| Validation | fit/apply firewall (truncation-invariant) | + **deflated Sharpe · PBO · purged+embargoed CPCV** as an admission gate |
| Evaluation | equity curve, fills, turnover | + Sharpe/Sortino/DD/Calmar/IR/appraisal/hit-rate/holding + IS-vs-OOS decomposition |
| Cost | √-impact model w/ **uncalibrated** defaults | **calibrated** δ/Y/γ/η + per-alpha/mega capacity curves |
| Compute | single-thread (determinism) | **deterministic multicore** (order-fixed merge, per-shard isolation) |
| Lifecycle | one backtest run | discover→gate→combine→optimize→monitor→decay→recycle |

When scope creep argues with you, this table, the carried-forward invariants (below), and the v2
anti-roadmap are where it goes to die.

---

## Companion docs index

| Doc | Type | Covers |
|---|---|---|
| [`sprint-1-evaluation-validation.md`](sprint-1-evaluation-validation.md) | sprint spec | **S1** — rich metrics + Deflated-Sharpe/PBO + purged-embargoed CPCV + bias-audit gates. The truth layer, built first. |
| [`sprint-2-parallel-compute.md`](sprint-2-parallel-compute.md) | sprint spec | **S2** — deterministic multicore task pool + parallel batch-eval + parallel folds. Throughput substrate. |
| [`sprint-3-formulaic-alpha-factory.md`](sprint-3-formulaic-alpha-factory.md) | sprint spec | **S3** — genetic/param search over the DSL; pool-aware fitness; canonical-hash dedup. The headline. |
| [`sprint-4-library-management.md`](sprint-4-library-management.md) | sprint spec | **S4** — persistent alpha library: store, dedup/correlation index, lifecycle, online corr-to-pool. |
| [`sprint-5-learned-signals-ml-combiner.md`](sprint-5-learned-signals-ml-combiner.md) | sprint spec | **S5** — PIT feature matrix + learned alphas (ridge/lasso/GBM) + HMM regimes + ML mega-combiner. |
| [`sprint-6-cost-calibration-capacity.md`](sprint-6-cost-calibration-capacity.md) | sprint spec | **S6** — calibrate δ/Y/γ/η; Almgren-Chriss temp/perm; capacity curves; cost-aware gating; borrow accrual. |
| [`sprint-7-portfolio-lifecycle.md`](sprint-7-portfolio-lifecycle.md) | sprint spec | **S7** — multi-period optimizer + decay monitor + dead-alpha→risk-factor + capital allocation + full E2E. |
| [`sprint-8-risk-covariance-construction.md`](sprint-8-risk-covariance-construction.md) | sprint spec | **S8** — vendor-grade covariance: robust regression + EWMA/Newey-West factor cov + eigenfactor cleaning + specific-risk blend + VRA + APCA statistical factors + shrinkage/PSD toolkit. Deepens the P4 risk model to Barra/Axioma parity. |

**Pending (created at each sprint kickoff/close):**
- `sprint-{N}-progress.md` — sprint ledgers (one per sprint, opened at kickoff; sub-sprints `Sn-a/Sn-b` if a
  sprint exceeds ~7 units, per `p0`'s 4a/4b/4c precedent).
- `sprint-{N}-<theme>-implementation-plan.md` — frozen per-unit implementation plan, written at each sprint
  kickoff (NOT now — these specs scope the *what*; the kickoff freezes the *how*).

**Sprint/module discipline:** unchanged from `p0` — [`../docs/sprint.md`](../docs/sprint.md),
[`../docs/module.md`](../docs/module.md), [`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Unit numbering:** p1 sprints are **S1…S7**; ledger units are **`S{n}-{m}`** (e.g. `S1-0` marker, `S1-1`…).
Renumber-don't-reuse on carry-forward (sprint.md §"Carry-forward").

---

## Sibling / upstream modules

- **Upstream:** `atx-core` stdlib. The engine remains a pure **consumer** of atx-core L0–L9 and adds no
  general-purpose primitives. p1 leans harder on **L5** (`simd`), **L6** (`cross_section`/`rolling`/
  `online_stats`), **L7** (`linalg`/`regression`/`pca`/`solve`), and introduces a need for **L4**
  (`concurrent` — a deterministic task/work-stealing pool) for S2. New numeric needs (e.g. an HMM/Baum-Welch
  kernel, gradient-boosted-tree primitives, an L-BFGS/coordinate-descent solver for S5; an L1/QP solver
  refinement for S7) are **atx-core requests, raised as cross-module edges here** (Pattern B), never built
  inside the engine.

---

## Strategic positioning (v2)

`p0` earned the right to be *believed*. `p1` earns the right to be *productive*. The identity shifts from
**"a deterministic bias-free backtester"** to **"a deterministic bias-free alpha factory"** — the
distinction that separates a backtesting library from Medallion/BRAIN is not a cleverer signal, it is the
**industrialization**: mass discovery, honest gating at scale, a living library, and a cost model accurate
to the last basis point (RenTech: at a 50.75% hit rate, *cost fidelity dominates profitability*).

| Capability | RenTech (Medallion) | WorldQuant (BRAIN) | **atx-engine v2 target** |
|---|---|---|---|
| Discovery | in-house statistical R&D; HMM heritage | crowdsourced formulaic mining (19→10⁷ alphas) | **genetic mining over the DSL** (S3) **+ learned signals** (S5) |
| Library | one ~500k-LOC unified model | mega-alpha pool, corr-gated | **persistent, deduped, lifecycle-managed library** (S4) |
| Combination | single unified optimizer (all signals) | bounded-reg / O(N) over the pool | P4 linear rungs **+ ML/regime combiner** (S5) |
| Validation | OOS discipline, "trust slowly" | fitness + correlation gate | **deflated-Sharpe · PBO · purged CPCV** admission gate (S1) |
| Cost | proprietary, the "secret weapon" | gross-of-cost | **calibrated √-impact + capacity** (S6) |
| Compute | petabyte warehouse, big iron | WebSim fan-out | **deterministic single-box multicore** (S2); distributed = anti-roadmap |
| Operation | live unified book, capacity-capped | SuperAlphas, real capital | **multi-period book + decay monitor + recycling** (S7) |

The two north-stars **converge on one proven thesis**: *millions of weak, mostly-uncorrelated signals,
combined honestly, beat any single clever insight* — and *the combination layer and cost model must be
exact*. v2 is the machine that produces and operates those millions.

---

## Assumed baseline — `p0` (Phases 1–5) + Phase 4 (treated as DONE)

p1 builds strictly **on top of** the following. p1 must **not** re-duplicate any of it; it extends through
the established seams (`ISignalSource`, `WeightPolicy`, `AlphaStore`/`AlphaStreams`, `BacktestResult`).

- **Phase 1 — Event spine:** Disruptor bus, event taxonomy, sim clock + PIT gate, k-way-merge `DataHandler`,
  determinism replay harness. ✅
- **Phase 2 — Backtest loop:** `RollingPanel` (sealed-row PIT), `ISignalSource` seam, `WeightPolicy`
  (winsorize→rank/zscore→dollar-neutral→gross-norm), `Portfolio` (exact-Decimal ledger), `Market`,
  `ExecutionSimulator` (√-impact temp→fill / perm→mark + slippage + commission + latency + partial fills),
  `BacktestLoop` crank. ✅
- **Phase 3 / 3b / 3c — Alpha DSL + vectorized VM:** lexer→Pratt parser→typecheck→hash-consed DAG (CSE)→
  bytecode→columnar VM, 64+ operators, oracle differential equality, `compile_batch` cross-alpha CSE,
  `extract_streams`/`AlphaStreams`, `VmSignalSource` + `Delay` knob. ✅
- **Phase 4 — Mega-alpha layer (ASSUMED DONE):** `AlphaStore` + pool, fitness/correlation/turnover gates,
  combiner rungs (equal→rank→IC→Ledoit-Wolf shrinkage→bounded/ridge regression), Barra factor risk
  `V=XFXᵀ+D` (factored), neutralization stages (group/factor/decay/truncation) on `WeightPolicy`,
  turnover-penalized risk-aware optimizer, capacity-curve stub, and the **fit/apply firewall** invariant
  (fitted objects estimated on a trailing window, applied OOS only). ✅ *closed at `f2d22f4` (Phase-4b).*
- **Phase 5 — Cost/validation (PARTIAL in p0):** p0 ships the cost *model* with documented-but-uncalibrated
  defaults and a basic walk-forward. p1 **S1 and S6 deepen this** (deflated-Sharpe/PBO/CPCV; coefficient
  calibration + capacity) — see those sprints for the exact boundary vs the p0 Phase-5 sketch.

> **The one hard external dependency (now satisfied):** Phase 4 is closed (`f2d22f4`), so S3/S4/S5/S7 (which
> consume the pool, gates, combiner, and risk model) are unblocked. S1, S2, and S6 never depended on P4;
> **S1 opened first and is now closed.**

---

## The v2 sprint arc

> Status rows stay `⏳ proposed` and are **not** promoted to commitments until a sprint actually opens
> (module.md: don't document hopes as commitments). Each sprint freezes its implementation plan at kickoff.
> Effort/unit counts are estimates to be re-scoped at kickoff (4–7 units/sprint; a sprint that would exceed
> ~7 splits into `Sn-a`/`Sn-b`).

### Dependency DAG

```
            P0 + P4 (assumed done)
                   │
        ┌──────────┼───────────────┐
        ▼          ▼                ▼
   S1 eval     S2 parallel       S6 cost           (S1·S2·S6 need NO P4 — can open first)
        │          │                │
        └────┬─────┘                │
             ▼                      │
         S3 factory ───► S4 library │
                              │     │
                              ▼     │
                          S5 ML ◄───┘
                              │
              S8 risk-cov ────┤      (deepens P4 risk model; needs only P4+S6 — truth/cost track)
              (Barra/Axioma)  ▼
                  S7 portfolio + lifecycle  (consumes S3·S4·S5·S6 + P4; trades S8's cleaned V)
```

> **S8 placement:** S8 sits on the *truth/throughput/cost* track (needs only **P4 + S6**, no S3/S4/S5). It deepens
> the P4 factored covariance to vendor grade and **strengthens S7** — S7's multi-period optimizer trades S8's
> cleaned `V`, and S7.3 (dead-alpha→risk-factor) reuses S8.6's statistical-factor append path. Open S8 **before
> or alongside S7**.

### S1 — Evaluation & Validation Spine  ✅ CLOSED `2158a17` ([spec](sprint-1-evaluation-validation.md) · [ledger](sprint-1-progress.md) · [user ref](sprint-1.md))
**Theme:** Build the *truth layer first*. A factory that mass-produces alphas without an honest scorer
mass-produces overfit garbage — the data-snooping bias is the dominant failure mode at 10⁵–10⁹ candidates.
Rich performance metrics on `BacktestResult`/`AlphaStreams`, the **Deflated Sharpe Ratio** and **PBO**
(probability of backtest overfitting) to correct for multiple testing, a **purged + embargoed CPCV**
harness extending P4's fit/apply firewall, and a reusable **bias-audit gate battery** (survivorship,
look-ahead, snooping) every downstream sprint reuses. *Grounded in RenTech §7 (OOS discipline) + WorldQuant
§4 (fitness/correlation gating).* **No P4 dependency** — opens first.

| # | Unit | Effort | Status |
|---|---|---|---|
| S1.0 | Marker + ledger | S | ✅ |
| S1.1 | `PerfMetrics` suite (Sharpe/Sortino/DD/Calmar/IR/appraisal/hit-rate/holding/monthly) over equity + AlphaStreams | M | ✅ |
| S1.2 | Deflated Sharpe Ratio + multiple-testing haircut (Bailey/López de Prado) | M | ✅ |
| S1.3 | PBO via combinatorially-symmetric cross-validation | M | ✅ |
| S1.4 | Purged + embargoed CPCV harness (extends P4 walk-forward; truncation-invariant) | L | ✅ |
| S1.5 | Bias-audit gate battery (reusable survivorship/look-ahead/snooping asserts) + close | M | ✅ |

> **Closed 2026-06-07** — 36 new tests, full engine suite 1623/1623 green; `eval::` (stats_ext/perf_metrics/
> deflated_sharpe/pbo/cpcv) + `validation::bias_audit`. Residuals lifted to the Pattern-B backlog below:
> atx-core L6 stats (norm_cdf/ppf/skew/kurt/median), monthly-decomposition-needs-dates, appraisal-vs-real-factor.

### S2 — Parallel Compute Substrate  ✅ closed ([spec](sprint-2-parallel-compute.md))
**Theme:** Deterministic throughput before the factory needs it. A deterministic task pool (atx-core L4
request), **parallel batch-eval** (partition the alpha set / date-shards, **order-fixed merge** so the
result is byte-identical to single-thread), parallel backtests and parallel CPCV folds, per-worker arenas
over a shared read-only panel. The non-negotiable: **determinism survives parallelism** — the wyhash digest
must equal the single-thread digest. *Grounded in WorldQuant §9 (mass eval requires fan-out) + the p0
determinism invariant.* **No P4 dependency** — can open concurrently with S1.

| # | Unit | Effort | Status |
|---|---|---|---|
| S2.0 | Marker + ledger | S | ✅ |
| S2.1 | Deterministic task pool (engine-local `DetPool` fallback; atx-core L4 `concurrent` request recorded; fixed worker count, atomic dispenser) | M | ✅ |
| S2.2 | Parallel batch-eval: fan per-root `Program`s across per-worker stateful Engines; order-fixed `SignalSet` merge | L | ✅ |
| S2.3 | Parallel backtests / parallel CPCV folds (per-fold isolation over const `AlphaStreams`, reduce-by-sort) | M | ✅ |
| S2.4 | Determinism-under-parallelism proof (digest == single-thread; {1,2,4,8} invariance) + bench + close | M | ✅ |

> **Closed 2026-06-07** (`d7a1b75`) — 27 new tests, full engine suite 882/882 green; `parallel::` (`DetPool` +
> `signal_set_digest`/`result_table_digest` + `parallel_evaluate` + `parallel_cpcv`/`parallel_backtests` +
> reduce-by-sort aggregate). Backbone = hand-rolled deterministic pool (oneTBB evaluated + rejected); determinism
> by construction (map + fixed-order assemble + reduce-by-sort), validated by the {1,2,4,8}==single-thread digest
> matrix. Residuals → Pattern-B backlog below: **atx-core `concurrent/task_pool.hpp` lift** (engine `DetPool` →
> L4), strategy-B CSE-preserving batch-eval (needs `Engine::evaluate_root`), Linux-CI TSan job, lowest-index-error
> tie-break test, NUMA panel partitioning (profile-gated).

### S3 — Formulaic Alpha Factory  ✅ CLOSED `5f57a34` ([spec](sprint-3-formulaic-alpha-factory.md) · [ledger](sprint-3-progress.md) · [user ref](sprint-3.md))
**Theme:** The headline — **automated discovery**. Genetic/evolutionary + parameter search over the DSL
expression space (the WQ 19→10⁷ engine), reusing the existing VM/CSE substrate. AST mutation (op-swap,
field-swap, fractional-window perturbation) and subtree crossover; a population driver with selection +
elitism + **novelty/diversity pressure**; **pool-aware fitness** = WQ fitness formula × *marginal
correlation contribution to the pool* (a new alpha's worth is its diversification, not its standalone
Sharpe); **canonical-hash dedup** via DAG normalization so structurally-equivalent expressions are never
re-evaluated. Consumes S1 (the scorer), S2 (throughput), P4 (pool + gates). *Grounded in WorldQuant §1/§5/§9
(formulaic alphas, fitness, the factory).*

| # | Unit | Effort | Status |
|---|---|---|---|
| S3.0 | Marker + ledger | S | ✅ |
| S3.1 | AST mutation operators (op-swap, field-swap, window/const perturbation; type-safe = produces valid programs) | M | ✅ |
| S3.2 | Subtree crossover + canonical-hash dedup (DAG normalization → never re-eval an equivalent expr) | M | ✅ |
| S3.3 | Parameter optimizer (grid/random/CMA-ES over fractional window/scale constants) | M | ✅ |
| S3.4 | Pool-aware fitness (WQ fitness × marginal corr-to-pool; reuses P4 gate stats + S1 metrics) | M | ✅ |
| S3.5 | Evolutionary search driver (population, selection, elitism, novelty pressure; deterministic, seeded) | L | ✅ |
| S3.6 | Factory integration (mine→gate→admit loop over S2 workers) + bench (alphas/sec) + close | M | ✅ |

> **Closed 2026-06-08** (`5f57a34`) — 38 factory tests across 8 suites + integration; full engine suite
> 1769/1769 green; header-only `factory::` (genome rebuild substrate + OpCatalog + mutation/crossover +
> sound canonical-hash dedup + sep-CMA-ES param search + pool-aware deflated fitness + seeded search driver +
> mine→gate→admit `Factory`), **atx-core unmodified**. The crown jewel — **a pure-noise population admits
> NOTHING while a real-signal panel admits survivors under the SAME gate+deflation bar, with deflation (N =
> the running trial count) doing the killing** — proven non-vacuous end-to-end. Residuals → Pattern-B backlog
> below: **full rotation-invariant CMA-ES → atx-core L7 `eigh`/`cma`** (sep-diagonal CMA-ES shipped),
> `parallel_evaluate`-in-driver swap (single-thread fresh-Engine fallback shipped pending S2 warm-up+reuse
> verification on evolved candidates), the **op_swap VM-SlotPool-corruption fix** (`enable_op_swap=false`),
> a first-class scoring-layer universe mask (vs the alternate-Panel sub-universe re-eval), and persisted
> cross-run dedup (→ S4 library-wide index).

### S4 — Massive Alpha Library Management  ⏳ proposed ([spec](sprint-4-library-management.md))
**Theme:** Make 10⁵–10⁹ alphas *live*. A persistent, append-only **library store** (the P4 `AlphaStore`
scaled to disk-backed columnar PnL/position matrices), a **canonical-hash dedup index** + a
**correlation-neighbor index** for fast marginal-correlation gating, **online/streaming corr-to-pool** to
kill the O(N²) re-gate that breaks at scale, alpha **lifecycle** state machine (candidate→admitted→live→
decaying→dead→recycled), and **versioned, reproducible library builds** (a library snapshot replays
byte-identically). *Grounded in WorldQuant §2/§5/§10 (datafield/library management, lifecycle) + Kakushadze
dead-alpha reuse.*

| # | Unit | Effort | Status |
|---|---|---|---|
| S4.0 | Marker + ledger | S | ⏳ |
| S4.1 | Disk-backed append-only library store (columnar pnl/pos; mmap zero-copy read; scales P4 `AlphaStore`) | L | ⏳ |
| S4.2 | Canonical-hash dedup index (structural identity across the whole library) | M | ⏳ |
| S4.3 | Correlation-neighbor index + online/streaming corr-to-pool (incremental gate, not O(N²) recompute) | L | ⏳ |
| S4.4 | Lifecycle state machine (candidate→admitted→live→decaying→dead→recycled; PIT transitions) | M | ⏳ |
| S4.5 | Versioned reproducible library builds (snapshot replay = byte-identical) + close | M | ⏳ |

### S4b — The Automated Alpha Engine (factory↔library integration)  ✅ CLOSED `54a53f4` ([spec](sprint-4b-automated-alpha-engine.md) · [ledger](sprint-4b-progress.md) · [user ref](sprint-4b.md))
**Theme:** Fuse the two finished halves — S3 mined into an *ephemeral* in-memory pool (O(N), evaporates) while S4's
*persistent* library admit path (library-wide dedup → O(neighbors) corr → P4 floors → segmented store + PIT
lifecycle + content-addressed manifest) sat unused, with **zero** references between them. S4b is the integration
milestone that makes them **one automated, persistent, deflation-gated discovery engine with end-to-end seeded
replay**: a `ResearchDriver` continuous mine→admit→repeat loop over a fixed panel, scoring each candidate's
marginal contribution to the *persistent* library at O(neighbors) scale, deflating against the running trial count
(the S1 bar stays factory-side, wrapped around `library::admit`), recording a round-trippable formula
(`unparse(Ast)`), and replaying byte-identically — manifest `version_id` included. Library = single source of truth.
*Grounded in WorldQuant §2/§5/§9/§10 + RenTech §1/§7 (continuous statistical discovery + ruthless OOS discipline).*

| # | Unit | Effort | Status |
|---|---|---|---|
| S4b.0 | Marker + ledger + as-built seam | S | ✅ |
| S4b.1 | `unparse(alpha::Ast)→string` + round-trip-through-`canonical_hash` soundness (the formula record) | M | ✅ |
| S4b.2 | `PoolView` seam + library-backed pool-aware fitness (O(neighbors) `worst_corr_to_pool`; legacy O(N) retained) | M | ✅ |
| S4b.3 | `Factory::mine_into(library)` — factory→library admit bridge (deflation factory-side; old ephemeral `mine()` test-only) | M | ✅ |
| S4b.4 | `ResearchDriver` — continuous mine→admit→repeat over a fixed panel; seed axis `(master,run,gen,idx)`; checkpoint/resume | L | ✅ |
| S4b.5 | E2E `ResearchEngine` (F1/F4/F6 + unparse-at-scale) + scale-lever bench + close | M | ✅ |

> **Closed 2026-06-09** (`54a53f4`) — 15 new engine tests (`ResearchEngine` F1/F4/F6 + unparse-at-scale, plus the
> per-unit `AlphaUnparse`/`FactoryPoolView`/`FactoryMineInto`/`ResearchDriver` suites) + `research_bench`; full
> engine suite **983/983** green; header-only additions (`alpha::unparse`, `factory::{PoolView, AlphaStorePool,
> LibraryPool, Factory::mine_into, ResearchDriver}`, `library::Library::worst_corr_to_pool` — the one additive
> facade accessor), **atx-core unmodified**. The proof: **a seeded engine run mines a population, scores marginal
> contribution to the *persistent* library at O(neighbors), deflates against the running trial count, admits
> survivors into the durable deduplicated library, and replays byte-identically (manifest `version_id` included) —
> while a pure-noise panel grows the library by ~0 across 4 seeds under the same bar, and a reopen-from-disk re-mine
> never re-admits a duplicate.** Scale lever benched ~3.35× (O(neighbors) vs O(N)) at a 4096-alpha pool, widening
> with size. Residuals → backlog: ranking-tiebreak asymmetry vs legacy `mine()`, Provenance lineage stub
> (`parent_hashes`/`mutation_op`), in-search selection vs the persistent library (needs a `SearchDriver` `PoolView`
> overload), and the re-eval adapter (re-parse `expr_source`→compile→eval over a new panel) → **S7**. The
> populated persistent library is the **baton to S5** (a real pool for the ML/regime combiner).

### S5 — Learned Signals & ML Combiner  ✅ CLOSED on `feat/atx-core-stdlib` (unmerged) ([spec](sprint-5-learned-signals-ml-combiner.md) · [ledger](sprint-5-progress.md) · [user ref](sprint-5.md))
**Theme:** The RenTech direction — **learned predictors as first-class alphas**, and a combiner that goes
beyond linear. A point-in-time **feature-matrix builder** (DSL outputs + raw fields → a panel feature
tensor), **learned signal models** (ridge/lasso/elastic-net, gradient-boosted trees) trained walk-forward
that emit a cross-section and plug into the pool exactly like a formulaic alpha, an **HMM regime detector**
(Baum-Welch — RenTech's noisy-channel heritage) for regime-conditional combination, and a **nonlinear/
ensemble mega-combiner** (stacking over the gated pool) that is **only admitted through S1's deflated-Sharpe
gate** (ML overfits hardest at this N — the p0 anti-roadmap's exact warning, now addressed *because* the
validation spine exists). Deterministic training (seeded, order-fixed). Consumes S1, S2, S4, P4. *Grounded
in RenTech §1/§2/§3 (no-theory statistical modeling, HMM, the combination crown jewel).*

| # | Unit | Effort | Status |
|---|---|---|---|
| S5.0 | Marker + ledger | S | ✅ (S5-0; +§0.8 Eigen-link retired) |
| S5.1 | PIT feature-matrix builder (DSL signals + raw fields → panel tensor; no look-ahead) | M | ✅ (S5-1 features + multi-horizon labels; S5-2 latent PCA + interactions) |
| S5.2 | Linear learned alphas (ridge/lasso/elastic-net; walk-forward trained; → `ISignalSource`) — atx-core L7 solver request | M | ✅ (S5-3; elastic-net CD Pattern-B + ridge, genuine-OOS firewall) |
| S5.3 | Gradient-boosted-tree learned alpha (deterministic; atx-core GBT primitive request) | L | ✅ (S5-4; histogram GBT, seed-robust M3) |
| S5.4 | HMM regime detector (Baum-Welch) + regime-conditional combination weights | L | ✅ (S5-5 log-space Baum-Welch; S5-6 regime-conditional stacking) |
| S5.5 | Nonlinear ensemble mega-combiner (stacking over the gated pool; deflated-Sharpe-gated) + close | L | ✅ (S5-6 stacking, deflation-gated vs linear; S5-7 integration + bench + close) |

> Likely splits **S5-a** (features + linear + GBT: S5.1–S5.3) / **S5-b** (HMM regimes + ensemble combiner:
> S5.4–S5.5) at kickoff. Each learned-model unit raises a specific atx-core numeric request (Pattern B).

### S6 — Cost Calibration & Capacity  ✅ closed @ `c4c3ff3` ([spec](sprint-6-cost-calibration-capacity.md))
**Theme:** RenTech's "secret weapon" — **cost fidelity**. p0 ships the √-impact model with *uncalibrated*
2001-era defaults; at a microscopic per-trade edge, a fraction of a basis point of cost error flips the
sign of the strategy. Calibrate δ/Y/γ/η to realized fills (or reference impact data), refine the
**Almgren-Chriss temporary-vs-permanent split**, compute **per-alpha and mega-alpha capacity curves** (the
AUM at which √-impact erodes net edge to zero — RenTech "report capacity, not just return"), wire
**cost-aware fitness/gating** so the factory and combiner price turnover correctly, and add **borrow/
short-financing accrual** (a p0 residual). *Grounded in RenTech §4 (cost = the strategy) + WorldQuant §8
(linear cost, turnover-aware fitness).* **No P4 dependency for calibration core** — can open concurrently
with S1/S2; capacity-on-the-mega-alpha needs P4.

| # | Unit | Effort | Status |
|---|---|---|---|
| S6.0 | Marker + ledger | S | ✅ `e45bd2a` |
| S6.1 | Cost-coefficient calibration harness (fit δ/Y/γ/η to realized fills / reference; report fit quality) | L | ✅ `6a8cd0c` |
| S6.2 | Almgren-Chriss temp/perm split refinement (no temp-cost leakage into forward mark) | M | ✅ `3c8e109` |
| S6.3 | Capacity curve (per-alpha + per-mega-alpha; AUM→net-edge=0) | M | ✅ `9f3a8c5` |
| S6.4 | Cost-aware fitness/gating hook (turnover/cost budget into S3 fitness + P4 combiner) | M | ✅ `84223d5` |
| S6.5 | Borrow / short-financing accrual + close | S | ✅ `c4c3ff3` |

### S7 — Portfolio Construction & Production Lifecycle  ⏳ proposed ([spec](sprint-7-portfolio-lifecycle.md))
**Theme:** Tie it all into an **operating mega-alpha**. A **multi-period / dynamic optimizer** (extends
P4's single-period optimizer with a turnover-aware rebalancing schedule and look-ahead-safe forecasts), an
**alpha-decay monitor** (live-vs-backtest drift detection that demotes alphas through the S4 lifecycle),
**dead-alpha→risk-factor** extraction (Kakushadze: flatlined alphas encode no-money-here directions; feed
them into the P4 risk model as endogenous factors), **capital allocation** across the mega-alpha with
book-level reporting artifacts, and the **full end-to-end integration test**: data → mine (S3) → store (S4)
→ evaluate/gate (S1) → combine (S5/P4) → optimize → cost (S6) → report — deterministic, point-in-time,
cost-honest, top to bottom. *Grounded in WorldQuant §6/§7/§10 + RenTech §6 (Kelly/breadth sizing, the
unified book).* Consumes everything.

| # | Unit | Effort | Status |
|---|---|---|---|
| S7.0 | Marker + ledger | S | ⏳ |
| S7.1 | Multi-period / dynamic optimizer (extends P4 optimizer; turnover-aware schedule; deterministic solver) | L | ⏳ |
| S7.2 | Alpha-decay monitor (live-vs-backtest drift → S4 lifecycle demotion; PIT) | M | ⏳ |
| S7.3 | Dead-alpha → risk-factor extraction (Kakushadze; feeds P4 `FactorModel`) | M | ⏳ |
| S7.4 | Capital allocation across the mega-alpha + book-level reporting artifacts | M | ⏳ |
| S7.5 | Full E2E pipeline integration test (data→mine→store→eval→combine→optimize→cost→report; deterministic) + close | L | ⏳ |

### S8 — Vendor-Grade Risk Model: Covariance Construction & Cleaning  ✅ shipped — S8-a + S8-b ([spec](sprint-8-risk-covariance-construction.md) · [S8-a ledger](sprint-8a-progress.md) · [S8-a plan](sprint-8a-covariance-construction-implementation-plan.md) · [S8-b ledger](sprint-8b-progress.md) · [S8-b plan](sprint-8b-regime-statistical-shrinkage-implementation-plan.md))
**Theme:** Deepen the P4 factored risk model `V = X F Xᵀ + D` from *correct-but-minimal* to *Barra/Axioma-grade*.
P4 keeps the covariance factored and applies it via Woodbury, but estimates `F` as one scaled-identity-LW-shrunk
sample covariance and `D` as plain residual variance — missing the four cleaning layers every risk shop applies.
S8 adds **robust √-cap/Huber factor regression**, **EWMA factor covariance with split vol/correlation half-lives +
Newey-West**, the **eigenfactor Monte-Carlo de-biasing** that corrects the ~40% small-eigenfactor risk under-forecast
(the one new, *seeded*, RNG site), an **EWMA+NW+structural specific-risk blend**, a **Volatility Regime Adjustment**,
the **APCA statistical-factor path** (retires the `n_stat_factors` `NotImplemented`), and a **model-free
shrinkage/RMT/PSD toolkit** (constant-correlation + nonlinear Ledoit-Wolf, Marchenko-Pastur clip, Higham repair).
*Grounded in [`../../research/covariance-matrix-construction-massive-universe-deep-dive.md`](../../research/covariance-matrix-construction-massive-universe-deep-dive.md)
(25/25 adversarially-verified vendor + peer-reviewed claims).* **Needs only P4 + S6** — truth/cost track; splits
**S8-a** (S8.1–S8.4 construction core) / **S8-b** (S8.5–S8.8 regime/statistical/shrinkage/integration) at kickoff.

| # | Unit | Effort | Status |
|---|---|---|---|
| S8.0 | Marker + ledger | S | ✅ `241a654` |
| S8.1 | Robust cross-sectional factor regression (√-cap + Huber IRLS; industry sum-to-zero) | M | ✅ `77c4562` |
| S8.2 | EWMA factor covariance — split vol/correlation half-lives + Newey-West | L | ✅ `c195d29` |
| S8.3 | Eigenfactor risk adjustment (Monte-Carlo de-biasing; seeded; `a=1.0` not 1.4) | L | ✅ `fb52fd2` |
| S8.4 | Specific-risk model — EWMA + Newey-West + structural blend (+ ISC hook) | M | ✅ `cb01c07` |
| S8.5 | Volatility Regime Adjustment (VRA) + bias-stat diagnostic | M | ✅ `a939e67` |
| S8.6 | APCA statistical factor model (fills `n_stat_factors`; shares S7.3's append seam) | L | ✅ `a74da77` |
| S8.7 | Model-free shrinkage + RMT clip + Higham PSD-repair toolkit | M | ✅ `3ce8cfe` |
| S8.8 | Short/long-horizon blend + integration + bench + close | L | ✅ `89d7c74` |

---

## Carried-forward invariants (non-negotiable — every sprint, proven by test not hope)

These are `p0`'s load-bearing guarantees. p1 **must not** weaken any of them; multicore (S2) and learned
models (S5) are the two places they are easiest to break, so each gets an explicit determinism proof.

1. **Determinism.** Same input → byte-identical result digest (wyhash fold over the ordered semantic
   stream). No RNG except **seeded, deterministic** training/search (S3 search, S5 model fits) whose seed is
   part of the recorded artifact. Order-fixed reductions in canonical instrument/alpha-id order — **including
   the parallel merge** (S2): the multicore digest must equal the single-thread digest.
2. **No look-ahead.** Every fitted object (combiner weights, gate stats, factor model, **mined fitness**,
   **learned model parameters**, **calibrated cost coefficients**, **regime states**) is estimated on a
   trailing window and applied OOS only — the **fit/apply firewall** P4 introduced, extended by S1's
   purge+embargo. Truncation-invariance is the test.
3. **No survivorship.** Delisted symbols pass through with their final bar; never retroactively removed —
   preserved through the library store (S4) and every learned-feature build (S5).
4. **Point-in-time.** Out-of-universe / not-yet-knowable data reads NaN; the library lifecycle (S4) and the
   decay monitor (S7) transition state on PIT boundaries only.
5. **Honest cost.** Cost is a first-class decision input, not a post-hoc deduction — S6 makes it *calibrated*
   and *capacity-bounded*; the factory (S3) and combiner (S5) price turnover/cost in their objectives.
6. **No hot-path allocation.** Steady-state loops allocate zero (rings/arenas pre-sized). Cold paths
   (compile, fit, calibrate, mine-generation) may allocate. Per-worker arenas in S2.
7. **Differential correctness.** Any new compute kernel (search ops, learned-model scorers, calibration math,
   parallel reductions) ships with an obviously-correct reference and a bit-/ULP-bounded differential test
   against it.

---

## Cross-module dependencies (Pattern B — edges live here)

p1 raises new `atx-core` numeric/infra needs. Each is an edge recorded here, not a primitive built in the
engine.

| p1 unit | Needs from `atx-core` | Note |
|---|---|---|
| S2.1 | **L4** `concurrent` — a deterministic task/work pool (fixed workers, no time-dependent scheduling) | core blocker for all parallelism |
| S1.2/S1.3 | **L6** `stats` — t-dist / variance-of-Sharpe helpers (DSR/PBO) | small additions |
| S3.3 | **L7** `eigh`/`cma` — full rotation-invariant CMA-ES (eigendecomposition of the covariance) | S3 shipped a separable diagonal-covariance CMA-ES engine-local; the rotation-invariant optimizer is the lift |
| S5.2 | **L7** coordinate-descent / L-BFGS solver for elastic-net | linear learned alphas |
| S5.3 | **L7** (or new) gradient-boosted-tree primitive (deterministic histogram split) | GBT learned alpha |
| S5.4 | **L7** HMM / Baum-Welch (forward-backward, log-space) | regime detection |
| S6.1 | **L7** robust regression / least-squares for impact-coefficient fitting | cost calibration |
| S7.1 | **L7** QP / projected-gradient refinement for the multi-period objective | dynamic optimizer |
| S8.1 | **L7** `huber_irls` — robust/Huber IRLS least-squares | robust factor regression; engine-side IRLS-over-`wls` fallback |
| S8.6 | **L7** `apca` — asymptotic PCA on the `T×T` Gram (`N≫T`) | statistical factors; `symmetric_eig`-on-Gram fallback |
| S8.7 | **L7** `nonlinear_shrinkage` (QIS) + `mp_clip` + optional `nearest_corr` | shrinkage/RMT/PSD toolkit; constant-corr LW + Higham-via-`symmetric_eig` fallback |

**Sequencing consequence:** S1, S2, S6 can scaffold immediately against atx-core *contracts* (TDD red
first), green-gating as each atx-core layer lands. S3/S4/S5/S7 wait on Phase-4 being merged. If atx-core L4
(`concurrent`) slips, S2 slips and S3's throughput target degrades to single-thread (still correct, just
slower) — note it as a risk at S2 kickoff.

---

## v2 anti-roadmap (explicitly NOT doing in p1)

The p0 anti-roadmap mostly holds; p1 **lifts two items** it explicitly addresses, and **keeps the rest**.

**Lifted (now in scope, *because* the substrate now exists):**
- ~~No ML autoML alpha mining~~ → **S3 (formulaic mining) + S5 (learned signals)** — but **only** behind
  S1's deflated-Sharpe/PBO admission gate. The p0 warning ("ML overfits at this N") is honored by building
  the validation spine *first*.
- ~~No distributed/cluster compute~~ → **partially lifted: single-box multicore (S2).** True
  distributed/cluster compute **stays out** (below).

**Still NOT doing:**
1. **No live broker / market connectivity.** v2 is still a simulator + factory. Live execution is a later
   module (p2).
2. **No full exchange matching engine.** ExecutionSim models fills/impact/capacity, not a limit-order-book
   matching venue.
3. **No alternative data** (news/satellite/etc.). Price-volume + classifications first — matches the verified
   WorldQuant alpha inputs and keeps the PIT discipline tractable.
4. **No distributed/cluster compute.** Single-box **multicore** is the scale ceiling for v2 (S2).
   Job-orchestration across machines, sharded cross-machine eval = p2. (RenTech's petabyte warehouse is not
   the lesson to copy first; the *discipline* is.)
5. **No UI / dashboard.** Headless engine + reproducible artifacts; reporting (S7) emits files, not pixels.
6. **No new general-purpose primitives in the engine.** Solvers, GBT, HMM, task-pool → **atx-core requests**,
   not engine code (Pattern B edges above).
7. **No deep-learning / neural nets.** S5 stops at GBT + linear + HMM — the models the research supports at
   this N. NN ensembles are p2-or-later, gated by evidence the simpler combiners are saturated.

When scope creep argues, the strategic-positioning table + this anti-roadmap settle it.

---

## Recommended sequencing

If you can only do one sprint: **S1** (evaluation & validation) — without an honest, deflated scorer, every
later sprint manufactures overfit. Nothing downstream is trustworthy without it.

If two: add **S2** (parallel compute) — independent of S1, opens concurrently; the throughput the factory
needs, with determinism preserved.

If three: add **S3** (the formulaic factory) — the headline capability, the first time the engine
*discovers* rather than *evaluates*. It is only safe because S1 exists.

Then **S4** (give the mined alphas a home), **S5 + S6** (learned combiner + calibrated cost — parallelizable
once S4 and the cost core land), and finally **S7** (operate it as a book, prove the whole pipeline E2E).

**Two tracks can run in parallel from day one:** the *truth+throughput+cost* track (S1, S2, S6 — no P4
dependency) and the *factory* track (S3→S4→S5→S7 — gated on Phase-4 merging). They converge at S7.

---

*Sprint discipline: per [`../docs/sprint.md`](../docs/sprint.md). Module structure: per
[`../docs/module.md`](../docs/module.md). Code quality gate: per
[`../docs/implementation-quality.md`](../docs/implementation-quality.md) and
[`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Research north-stars:
[`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) ·
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md).*
