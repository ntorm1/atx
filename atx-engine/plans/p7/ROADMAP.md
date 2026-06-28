# p7 — Production Alpha Book + High-Performance DSL Pipeline

**Created:** 2026-06-28. **Predecessor:** [p6](../p6/ROADMAP.md) (Tradeable-Alpha Uplift).

**One-line thesis:** p6 made the machine *correct and cost-aware* and proved the loop closes on
one alpha. p7 makes it *production-grade* on two intertwined fronts the user named explicitly:
**(Track A)** robust, high-fitness alpha**s** — a deflation-surviving *book*, not a single hit; and
**(Track B)** a high-performance, operable DSL pipeline that can search the breadth such a book
requires.

**Scored against** the parent north star ([../ROADMAP.md](../ROADMAP.md)): a standing book of
robust mega-alphas — low turnover, high capacity, surviving DSR/PBO/CPCV, honest cost, promoted
Dev→UAT→PROD. Every move below moves one of: **OOS-DSR ↑, net-Sharpe ↑, turnover ↓, %ADV capacity ↑,
PBO ↓, eval-ms/genome ↓, alphas/sec ↑**.

---

## What p6 leaves us (the starting state, measured)

p6 delivered: eval/VM perf passes (S1), factory admission refactor (S2), turnover-aware search +
seed-elitism + viable-novelty (S3), cost-aware gates `rt_cost_bps`/`min_holding_days` (S4), panel
augmentation `with_alpha101_fields` + `--adv-windows` (S5), sign-correct downstream book (S6), and
S7 threaded every knob through the CLI + shipped `build-tradeable-alphas.ps1`.

**But the engine is built, not wired.** The robustness machinery exists and is *inert by default*,
and the search axis is a price/return monoculture. The four explorers that scoped this roadmap found
the gaps are overwhelmingly at the **defaults / wiring / breadth** layer, not missing capability:

1. **Deflation is computed but not gated.** DSR (`eval/deflated_sharpe.hpp`), PBO/CSCV
   (`eval/pbo.hpp`), CPCV (`eval/cpcv.hpp`) all exist and are complete — but `GateConfig`
   (`combine/gate.hpp`) screens only fitness/sharpe/turnover/corr. There is **no `min_dsr` /
   `max_pbo` gate field**. Worse, the factory **voids the trial count**
   (`src/factory/factory.cpp:983` `static_cast<void>(trial_count)`), so DSR is deflated at N=1
   regardless of sweep breadth — the multi-seed thesis is invisible to the one anti-overfit defense.
2. **The search axis is information-poor.** ~Every reported alpha is price/return. The single
   most-cited finding across all research docs: edge poverty is a *breadth* problem. Named, specific,
   unshipped signal families: **FINRA short-interest** (already built in worktree
   `track-b-information-structure`, B1–B4, 8 tests green, never merged), **IV-surface** (`atmCenI_*`
   → iv_term/iv_vrp), **liquidity/illiquidity** (illiq, observed Sharpe 1.22 / 6% turnover —
   untradeable today only because of universe+gate misalignment).
3. **Robustness factors are off.** `turnover_penalty_slope=0.0`, `weak_panel=nullptr → robust=1.0`,
   `min_regime_sharpe=0.0`, `RegimeCombiner` non-default, `conviction()` computed but uncalled in the
   active pipeline, `--capacity-floor` a confirmed **1.0 no-op stub**. Each is a one-field wiring fix.
4. **The pipeline can't yet search at book-breadth.** 29 of 34 time-series ops are **O(T×W) batch,
   recomputing the full window every cell** (`alpha/ts_ops.hpp:286`), with **no SIMD** and **no
   cross-instrument parallelism**; the variance family reverted from online to batch for numerical
   safety (`ts_ops.hpp:304`). There is **no benchmark harness** to even measure a perf win, **no
   incremental panel** (one new day = full rebuild), and survivorship bias is **documented-unfixed**
   (`data/universe.hpp:47`).

> **Anchor to the S7-6 result:** the p6 capstone hunt (`atx-impl/research/2026-06-27-tradeable-
> alpha-results.md`) names the *binding constraint* on tradeability. p7's first sprint in Track A is
> whichever gate that doc fingers. Update this paragraph with the S7-6 verdict before dispatching.

---

## Two tracks, run in parallel (the p6 ownership contract, continued)

p7 keeps p6's proven model: **disjoint file ownership ⇒ parallel sprints; one integration sprint
last.** Track A (alpha quality) and Track B (pipeline perf/operability) own disjoint subsystems and
run concurrently. The capstone (P7-C) owns the shared CLI/stage hub files and the real-data book
build, and runs last.

Every behavior-changing sprint inherits p6's **(A) opt-in / default-byte-identical contract**: new
capability behind an engine-config field defaulting to today's value; pinned goldens
(`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`, OOS
goldens) unchanged on the no-flag path; `oracle.hpp` untouched. Perf sprints inherit the **(B)
two-tier eval contract**: byte-identical wins ship in the audit path; determinism-breaking wins ship
behind `ResearchFast` and every admitted alpha is re-scored on the exact path before it is published.

---

## Track A — Robust, High-Fitness Alpha **Book**

| Sprint | Theme | Goal metric | Owns (exclusive) |
|---|---|---|---|
| **A1** | **Deflation gates + honest selection** — add `min_dsr`/`max_pbo`/`require_split_stable` to `GateConfig`; thread the *cumulative* sweep trial-count into admission DSR (kill the `static_cast<void>`); search-wide multiple-testing correction | admitted PBO ↓, selection-inflation removed; goldens byte-identical off-path | `combine/gate.hpp`, `eval/deflated_sharpe.hpp` wiring, `src/factory/factory.cpp` trial-count path + tests |
| **A2** | **Information breadth** — land FINRA short-interest (merge `track-b-information-structure` under the determinism/schema-digest decision); IV-surface + liquidity signal families into `datafields`/augment + seed catalog | # evaluable signal families ↑ (1 → ≥4); N_eff ↑ | NEW `alpha/datafields` short-interest + IV fields, seed catalog, ingest loader + tests |
| **A3** | **Turnover + capacity realism** — turnover penalty + decay-wrapped seeds as the default *tradeable* profile; stateful position-decay `WeightPolicy`; real per-name capacity vector into the combiner (kill the 1.0 stub); capacity curve (AUM→net-edge) as a first-class scorecard | net-Sharpe ↑, turnover ↓ (<0.30/day), capacity curve emitted | `loop/weight_policy.hpp` decay, `combine/combiner.hpp` capacity wiring, `cost/capacity.hpp`, `risk/capacity.hpp` + tests |
| **A4** | **Conviction-scaled sizing** — wire `conviction()` → `FitnessReport`/deploy; fractional-Kelly `f*=Σ⁻¹μ` scaled by per-name conviction + `kelly_fraction`; conviction-aware walk-forward (honest OOS of the *shipped* book) | OOS estimator matches deployed book; sized by conviction | `combine/conviction.hpp` wiring, NEW `risk/kelly_sizing.hpp`, `eval/regime_slice.hpp` WF + tests |
| **A5** | **Regime-adaptive combination (guarded)** — promote Baum-Welch/HMM regime posterior → `RegimeCombiner` per-regime weights; vol-tercile weak-panel robustness factor on by default | worst-regime Sharpe ↑; no global-blend regression | `combine/regime_combiner.hpp`, `eval/regime_slice.hpp` HMM, `src/stage_regime` consume + tests |
| **A6** | **Survivorship correctness** — delisted-symbol recovery into the security master with exit dates; survivorship caveat removed from the scorecard | backtests include delisted cohort; caveat retired | `data/universe.hpp`, `data/orats_history.hpp` symbology + tests |

> **A5 caveat (from the research):** the Baum-Welch/IBM-speech lineage is *validated history, not
> architecture* (`research/claim-verify-baumwelch-ibm-speech.md`; four HMM-centric claims refuted in
> `rentech-structure-signals-domain-mapping.md`). Regime conditioning is one tool — wire it, do not
> build the engine around it. A5 is the lowest Track-A priority.

---

## Track B — High-Performance, Operable DSL Pipeline

| Sprint | Theme | Goal metric | Owns (exclusive) |
|---|---|---|---|
| **B1** | **Benchmark harness first** — microbenchmarks for VM dispatch, Ts/Cs kernels, compile pipeline; recorded baseline (no perf claim without a before/after line) | reproducible eval-ms/genome + alphas/sec baseline | NEW `atx-engine/bench/`, bench targets + CI bench gate |
| **B2** | **Eval VM hot path** — numerically-safe *online* variance family (Welford/compensated); cross-instrument parallelism over column chunks for batch Ts ops; realize `ResearchFast` tier (currently only VM-vs-Oracle exists, no `EvalMode`) | batch Ts op ms ↓ ≥5×; seq==parallel + audit-exact preserved | `alpha/ts_ops.hpp`, `alpha/vm.hpp` EvalMode, worker parallelism + differential tests |
| **B3** | **SIMD + layout** — explicit AVX2 for cs_rank/zscore + hot Ts inner loops; O(1) `field_id`/`intern_field` maps; compile-cache LRU eviction | cs/ts kernel ms ↓; bounded compile-cache memory | `alpha/cs_ops.hpp`, `alpha/dag.hpp`, `alpha/bytecode.hpp`, `data/panel.hpp` + tests |
| **B4** | **Data pipeline production** — incremental panel append (one new day, no full rebuild); mmap + compressed (zstd) panel serialization; (stretch) incremental seg append from a daily feed | panel update s ↓ ≫; load via mmap zero-copy | `atx-impl/src/{stage_panel,serialize_panel,stage_load}.cpp`, `data/history_panel.hpp` + tests |
| **B5** | **Observability + provenance** — structured logging + log levels; real `wall_ms` in checkpoints; populate `config_json` + `engine_git_sha`; decay monitor for the standing book | every run reproducible from provenance; decay alerts | `atx-impl/src/store_progress_sink.cpp`, `stage_discover.cpp` provenance, NEW decay-monitor + tests |

---

## P7-C — Integration capstone (runs last)

Owns the shared hub files (`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}`)
and the real-data book build. Threads the Track-A/B flags through the CLI (same discipline as p6 S7),
then:

1. **Multi-seed mega-alpha sweep** on the capacity-screened universe — `sweep` accumulates a
   cross-run library under cumulative-trial DSR deflation, conviction-scored, decorrelated.
2. **Deploy the book** (not one alpha) through the sign-correct, capacity-aware, conviction-sized
   optimize/report path; emit the full scorecard: net-of-cost OOS Sharpe, DSR, PBO, CPCV, walk-
   forward, capacity curve, N_eff/IR breadth.
3. **Promote** the surviving book through persistence-v2 Dev→UAT→PROD with the decay monitor armed.
4. **Research doc** `atx-impl/research/2026-06-2X-production-book-results.md`: the book, its
   scorecard, the perf uplift vs the p6 baseline, and the determinism note (default path byte-
   identical; the production profile is the explicit opt-in, never a golden re-baseline).

---

## Sequencing

1. **Parallel wave:** A1, A2, A3, A4, A6 and B1, B2, B3, B4, B5 — disjoint files, dispatch together.
   A5 after A4. B2/B3 consume B1's harness; if B1 hasn't landed, they record a local baseline and
   note the dependency.
2. **P7-C last** — depends on the waves; owns CLI wiring + the production book build.

**If you can only do a subset (highest leverage first):**
- **A1 then A2.** Without deflation gates + cumulative-trial DSR (A1), the multi-seed sweep is an
  overfit-garbage generator — this is the "single most consequential fix" the research names. Without
  breadth (A2), there is no edge to deflate honestly. These two are the spine.
- **B1 then B2.** No perf claim is real without the harness (B1); the O(T×W) batch Ts path (B2) is
  the one bottleneck that caps the breadth A2 needs. Everything else is multiplier, not enabler.
- A3 (turnover/capacity) is what turns a positive *gross* edge into a tradeable *net* one.

## North-star acceptance (P7-C)

On the capacity-screened real panel, end-to-end produce a **deployed book** (≥5 admitted,
decorrelated, conviction-sized alphas) that is simultaneously:
- net-of-10bps OOS Sharpe **> 1.0** (book-level, vs p6's single-alpha 0.8 bar),
- DSR > 0 under **cumulative-sweep** trial-count deflation, PBO < 0.5,
- turnover < 0.30/day, capacity curve showing positive net edge at ≥ $100M AUM,
- sign-correct, conviction-sized, sane participation footprint,
- searched at a measured **≥5× alphas/sec** over the p6 baseline (B-track) —

OR a documented frontier naming the next binding constraint. Honest null remains a valid outcome.

Sprint discipline: [../docs/sprint.md](../docs/sprint.md). Implementation quality (mandatory):
[../docs/implementation-quality.md](../docs/implementation-quality.md) — every perf claim carries a
recorded before/after bench line.
