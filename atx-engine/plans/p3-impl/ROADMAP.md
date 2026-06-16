# Module p3 — atx-engine v4: Real-Data Validation & Robustness Verification (`p3`)

**Last reviewed:** 2026-06-15
**Started:** 2026-06-14 — **S1 open** (worktree `atx-wt/p3-s1`, branch `feat/p3-s1-real-data`, base `main @ b796a3b`). Assumes `p0` Phases 1–4, `p1` S1–S8, `p2` S1–S8 closed. **p2 S5 (deep-learning sequence alphas) and S6 (BYO-data abstraction layer) are now MERGED to `main`** — `feat/p2-s5-dl-alphas` and `feat/p2-s6-data-layer` were merged before kickoff; the S6 merge commit is `b796a3b` (the p3 worktree base). The kickoff "assume done" directive is therefore satisfied by real merged code, not a synthetic integration (see S1-0 ledger).
**Source:** the user's p3 kickoff directive — *"take real US equity data and apply the atx-engine pipeline on it as a validation run and verification of the pipeline's quality and robustness"* — reconciled against the as-built ingestion stack (`atx-core` parquet IO + `atx-tsdb` segment loaders + `atx-engine` feeds/`segment_panel`) and the `p2` S6 BYO-data layer. Distills [`../../research/finding-alphas-book-deep-dive.md`](../../research/finding-alphas-book-deep-dive.md) + [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md), and the real-data semantics in [`../../../docs/us_split_adjustment_factors.md`](../../../docs/us_split_adjustment_factors.md) + [`../../../atx-core/plans/databento-loader-design.md`](../../../atx-core/plans/databento-loader-design.md). Governed by [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md).
**Goal:** Take a pipeline that has only ever been validated on **synthetic / planted-signal** panels and run it end-to-end on **real US equity data** — databento daily OHLCV joined to a security-master of corporate actions (split/adjustment factors, cash dividends, shares outstanding, GICS sectors) — as a **validation run and robustness verification**. Two questions: *(1) does the engine correctly ingest and adjust real data* (corporate actions, total-return, universe, survivorship), and *(2) when it does, does the mine→validate→robust pipeline find anything real* — or does the full deflation/CPCV/regime/lockbox battery correctly admit ~nothing on a real, mostly-efficient market? **Live execution stays out of scope** (delegated to broker algorithms, per `p2`). p3 does **not** add new alpha-discovery capability; it **hardens the seam between real data and the existing pipeline**, then **measures honestly**.

---

## What changes from p2 → p3

`p0` answered *"can I trust one backtest?"* `p1` answered *"can I operate a factory?"* `p2` answered *"can I prove the factory finds **robust** alpha?"* — but every one of those proofs ran on **panels we built ourselves**: planted-drift signal panels, pure-noise panels, synthetic-recovery fixtures. The determinism, the deflation non-vacuousness, the regime/walk-forward/lockbox battery — all demonstrated against data with a *known* ground truth.

`p3` answers the question those proofs deferred: ***"does any of it survive contact with real US equity data?"*** That is a different kind of work. There is no new search operator, no new objective, no new solver. The frontier is the **plumbing and the adjustments** that stand between a folder of parquet files and the `alpha::Panel` the engine already knows how to mine — corporate actions, total-return construction, a real (survivorship-caveated) universe, market-cap and liquidity screens — and then the **honest measurement** of what the unchanged pipeline does when fed that panel.

| Dimension | `p2` baseline (S1–S8) | **`p3` v4 target** |
|---|---|---|
| Input data | one fixed in-memory `Panel` per run, or the S6 `DatasetCatalog` over **synthetic** datasets; planted-drift / pure-noise fixtures | **real on-disk US equity data** — databento daily OHLCV (`data/databento/equs_ohlcv_1d_by_date/`) ⋈ security master (`data/us_split_adjustment_factors/`), loaded through the as-built parquet→segment→`Dataset`→`Panel` stack |
| Corporate actions | **none** — engine has zero split/dividend/adjustment handling; the raw databento close is unadjusted | **total-return adjusted close** — split factor (`cumulative_adjustment_factor`) + reinvested `cash_dividend` folded into a PIT-correct adjusted Dataset field; validated against known AAPL split/dividend events |
| Universe | fixed at panel construction; present-bitmap membership; no economic screen | **constructed universe** — `shares_outstanding`→market cap, trailing-ADV liquidity screen, GICS sector labels, PIT membership mask, with the **survivorship bias documented** (the security master is listed-only) |
| Coverage | 3-symbol smoke (`AAPL` + 2) | **expanded** — the python builder re-run over the databento universe to give corporate-action coverage on a **liquid subset** (hundreds of names) for the benchmark |
| Pipeline documentation | spread across research deep-dives + per-sprint plans; no single "how data flows in" reference | **two canonical companion docs** — an alpha-pipeline reference (Panel→DSL→backtest→fitness→search→robust→lockbox) and a data-ingestion reference (parquet→segment→`Dataset`→`Catalog`→`adapt_panel`→`Panel`, with the adjustment/dividend/universe semantics) |
| Validation truth | known ground truth (planted signal / pure noise) | **unknown ground truth** — a real, mostly-efficient market; the deflation/CPCV/regime/lockbox gates must be trusted to separate signal from snooping with **no oracle** |
| Identity | a believable backtest, industrialized, proven to find robust alpha on synthetic data | the same engine, **proven to ingest real US equity data correctly and measured honestly against it** |

When scope creep argues, this table and the v4 anti-roadmap are where it goes to die. **p3 is a hardening + measurement module, not a capability module.** If a proposed unit adds a new search operator, a new objective, a new solver, or a new asset class, it does not belong in p3.

---

## Companion docs index

| Doc | Type | Covers |
|---|---|---|
| [`sprint-1-real-data-hardening-implementation-plan.md`](sprint-1-real-data-hardening-implementation-plan.md) | implementation plan | **S1** frozen *how* — the per-unit decomposition of the real-data hardening sprint: security-master ingestion into a typed PIT `Dataset`, total-return adjustment, universe/market-cap/liquidity/sector construction, the end-to-end real-data ingestion path (databento ⋈ corp-actions ⋈ universe → `Catalog` → `adapt_panel` → `Panel`), digest-pinned, plus the coverage-expansion data-build. The hardening track's per-unit plan. |
| [`alpha-pipeline-reference.md`](alpha-pipeline-reference.md) | reference | **S1-1** — the alpha pipeline map: Panel + DSL substrate → backtest/sim kernel + cost model → fitness + the 5-slot objective vector → `SearchDriver`/NSGA-II/behavioral-archive/dedup → robustness battery → deflation/PBO → determinism → the public entry points (`Factory::mine`, `ResearchDriver::run`, `RobustResearchDriver::run`). Every claim `file:line`-anchored at HEAD. |
| [`data-ingestion-reference.md`](data-ingestion-reference.md) | reference | **S1-1** — the data-ingestion map: the on-disk §0.5 datasets → the ingestion-stack layer table → corporate-action semantics (total-return model + the return-invariance ⇒ non-leak argument) → the survivorship-caveated universe → the `build_real_panel` E2E recipe → coverage/rebuild. Every claim `file:line`-anchored at HEAD. |

**Pending (created at each sprint kickoff/close):**
- `sprint-1-real-data-hardening.md` — optional sprint spec (the *what*); **for S1 and S2 the embedded ROADMAP sections below + the impl-plan are the spec** (p2 precedent), so this is only written if S1 grows enough open forks to need its own *what*.
- `sprint-2-real-data-benchmark-implementation-plan.md` — frozen per-unit *how* for S2, written at **S2 kickoff (NOT now)**. S2 stays outline-grade in this ROADMAP until then.
- `sprint-{1,2}-progress.md` — sprint ledgers, opened at each kickoff (sub-sprints `Sn-a/Sn-b` only if a sprint exceeds ~7 units, per the `p0` 4a/4b/4c precedent).
- `sprint{1,2}.md` — user reference, written at each sprint close.

**Sprint/module discipline:** unchanged — [`../docs/sprint.md`](../docs/sprint.md), [`../docs/module.md`](../docs/module.md), [`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Unit numbering:** `p3` sprints are **S1–S2**; ledger units are **`S{n}-{m}`** (e.g. `S1-0` marker, `S1-1`…). These labels are namespaced by `p3/`; when cross-referencing `p1`/`p2` (which also use S1…S8), qualify as **"p3 S1"** vs **"p2 S1"**. Renumber-don't-reuse on carry-forward (`sprint.md` §"Carry-forward").

---

## Sibling / upstream modules

- **Upstream:**
  - `atx-core` — the stdlib (L0–L10). p3 leans on **L10 `io::parquet`** (the lazy projection/predicate scan that reads the security-master and databento parquet) and **L8 `datetime`** (the trading-calendar / as-of date axis). p3 adds **no** atx-core primitive; any new numeric/IO need is raised as a cross-module edge, never built in the engine.
  - `atx-tsdb` — the sealed-segment storage + parquet loaders (`load_parquet`, `load_parquet_scaled`, `build_dated_segments`) that pivot long parquet into per-date `.seg` files and the zero-copy `SegmentReader`. p3 is a **consumer**: it joins a second dataset (the security master) into the segment→Panel path; it does not change the segment format.
  - **Source data + tooling** — the on-disk datasets at [`../../../data/`](../../../data/) and the python builders at [`../../../python/scripts/`](../../../python/scripts/) (`build_us_split_adjustments.py`, `extract_databento_equs_ohlcv_1d.py`). p3 **consumes** these and, in S1-6, **re-runs** the security-master builder over a wider universe; it does not re-architect them.
- **Sibling:** `p2` — atx-engine v3, the robust alpha engine. See [`../p2/`](../p2/). p3 builds strictly **on top of** the closed `p2` seams — the S6 BYO-data layer (`data::{Dataset, DatasetSchema, DatasetCatalog}`, `align.hpp`, `adapt_panel`, `adapt_feature`), the S4 robustness battery (`factory::RobustResearchDriver`, `eval::{regime_slice, lockbox, cpcv, deflated_sharpe, pbo}`, `factory::pareto`), the S5 deep-learning alphas, the S7/S8 book + covariance — and must **not** re-duplicate any of them. p3's only engine additions are the **real-data adapters** (corporate-action ingestion, total-return adjustment, universe construction) that sit *between* the on-disk parquet and the S6 `Dataset`.

**Cross-module dependency (Pattern B — RESOLVED at S1-0):** p3's entire S1 substrate is the `p2` **S6 data layer**. At kickoff this was the module's #1 precondition; it is now **resolved** — S5 (`feat/p2-s5-dl-alphas`) and S6 (`feat/p2-s6-data-layer`) are **merged to `main`**, and the p3 worktree branches off `main @ b796a3b` (the S6 merge commit). S1-0 verified the S6 seams (`data::{Dataset, DatasetSchema, DatasetCatalog}`, `align.hpp`, `adapt_panel`, `adapt_feature`) are present and the full atx-engine suite is green on the base (1657/0/0, the 2 `*_NOT_BUILT` ctest sentinels are unbuilt sibling binaries, not failures). The dependency is no longer "not-yet-merged".

---

## Strategic positioning (v4)

`p0` earned the right to be *believed*; `p1` earned the right to be *productive*; `p2` earned the right to be *trusted to find robust alpha* — **on data we made up**. `p3` earns the right to say *"and it works on real data, and here is exactly how well."* The identity shifts from **"a deterministic bias-free alpha factory proven on synthetic panels"** to **"a deterministic bias-free alpha factory proven to correctly ingest real US equity data, with an honest scorecard of what it finds."**

The distinction is **not** a bigger search or a richer language — p2 already has those. It is the **last mile of trust**: a synthetic-only proof is a proof about the *machinery*; a real-data run is a proof about the *world*. The two failure modes p3 exists to catch are (a) **silent data bugs** — an unadjusted split that fabricates a 50% overnight "return," a forward-filled fundamental that leaks the future, a survivorship-selected universe that flatters every backtest — and (b) **false confidence** — declaring "we find alpha" because a snooped fit cleared a gate that was only ever tuned against planted signal.

| Capability | A naive real-data backtest | A vendor research desk | **atx-engine v4 target (p3)** |
|---|---|---|---|
| Corporate actions | "adjusted close" from one vendor, taken on faith | obsessive multi-source reconciliation; total-return series | **explicit, PIT-correct total-return** — split factor + reinvested dividend folded in-engine, validated against known events, with the adjclose-mode builder output as a cross-check oracle |
| Universe | "whatever symbols are in the file" (survivorship-biased) | PIT index membership, delisting-aware | **constructed + caveated** — market-cap/ADV/sector screens with PIT membership, and the **listed-only survivorship bias written into the scorecard** (not hidden) |
| Determinism | none — pandas, floats, run-to-run drift | reproducible pipelines | **byte-identical** — the real-data Panel build is digest-pinned, same as every synthetic fixture before it |
| Overfitting control | in-sample Sharpe, maybe one OOS split | IS/OOS + multiple-testing awareness | the **full p2 battery unchanged** — deflated Sharpe at running trial count, CPCV, regime/walk-forward survival, sealed lockbox — now pointed at real data |
| Honesty | "we found alpha!" | "here's the net-of-cost, post-deflation, survivorship-adjusted number" | a **scorecard that reports the caveats as first-class results** — short history (2024-07→), listed-only universe, coverage limits — and treats *"the gates correctly admitted ~nothing"* as a **passing** validation outcome |

The north-star: *a real-data run is only worth running if its result is **trustworthy whether or not it finds alpha.*** Finding a robust alpha is a win; **proving the pipeline correctly rejects snooping on a real efficient market is an equally important win.** p3 is built so both outcomes are publishable.

---

## Assumed baseline — `p0` + `p1` + `p2`, treated as DONE

p3 builds strictly **on top of** the following and must **not** re-duplicate any of it; it extends through the named seams.

**Solid (shipped, depended-upon):**
- **Ingestion stack (`atx-core` + `atx-tsdb`).** `atx::core::io::ParquetTable` (lazy Arrow scan, column projection/predicate pushdown). `atx::tsdb::{load_parquet, load_parquet_scaled, build_dated_segments}` pivots long parquet → sealed per-date `.seg`; `SegmentReader` gives zero-copy O(1) cell access with a no-look-ahead `cutoff_index`. `atx::external::databento::load_equs_summary_zip` decodes the raw DBN zip → date-partitioned parquet (already run; output is on disk).
- **Segment→Panel bridge (`atx-engine`).** `alpha::attach_segment_panel` (`segment_panel.hpp`) maps a segment, slices a date window, derives a PIT universe → `MappedPanel` (borrowed `Panel`). `alpha::build_augmented_panel` (`datafields.hpp`) appends `dollar_volume`, `vwap`, `adv{d}`.
- **S6 BYO-data layer (`feat/p2-s6-data-layer`, treated DONE).** `data::Dataset` + `DatasetSchema` (typed, PIT-versioned, Price/Feature/Signal/Reference roles); `DatasetCatalog` (registry + as-of/lineage); `align.hpp` (ingestion + alignment + truncation-invariant PIT rail); `adapt_panel` (`PriceDataset`→`Panel`, == `with_datafields`); `adapt_feature` (`FeatureDataset`→`Panel` merge + `FeatureMatrix` injection).
- **The full p2 mine→prove pipeline.** `factory::{SearchDriver, Factory, ResearchDriver, RobustResearchDriver, fitness, pareto, behavior}`; `eval::{deflated_sharpe, cpcv, pbo, regime_slice, lockbox, synthetic_alpha}`; `combine::*`; `risk::{MultiPeriodOptimizer, FactorModel}`; `book::BookPipeline`; `parallel::DetPool`. All boundary-pinned to golden digests.
- **Determinism contracts.** `signal_set_digest` (raw-f64-byte canonical hash), `seed_for` (seed-by-id, no worker/thread/time), the golden `SearchResult.digest` boundary pin. p3 inherits these verbatim and adds a new pin for the real-data Panel.

**Critical gaps — the real-data seam p3 closes (each maps to a S1 unit):**
1. **No corporate-action handling anywhere in the engine.** The databento close is **unadjusted**; nothing reads `cumulative_adjustment_factor` or `cash_dividend`. A split or dividend ex-date is currently a fabricated overnight return. → **S1-2, S1-3.**
2. **No security-master ingestion.** The corporate-action / fundamental parquet (`security_master.parquet` + the by-symbol partitions) has no loader into the `Dataset` layer. → **S1-2.**
3. **No economic universe.** No market cap (`shares_outstanding` unused), no liquidity screen, no sector labels in the Panel, no PIT membership beyond raw presence; the listed-only **survivorship bias is undocumented**. → **S1-4.**
4. **No single real-data ingestion path.** The pieces (parquet→segment, segment→Panel, S6 `Dataset`/`Catalog`/adapters) exist but **nothing joins databento OHLCV to the security master and produces an adjusted, universe-screened `Panel`** — and no test exercises the real on-disk files. → **S1-5.**
5. **Coverage is a 3-symbol smoke.** Corporate-action coverage (`AAPL` + 2) cannot support a benchmark over the databento universe. → **S1-6.**
6. **No canonical "how it flows" documentation.** The alpha pipeline and the data ingestion are mapped only across research docs + per-sprint plans. → **S1-1.**

---

## The v4 sprint arc

```
                p2 S5 (DL alphas) + p2 S6 (BYO-data layer)  ── treated DONE ──┐
                                                                              │
   real on-disk data (databento OHLCV ⋈ security master)  ───────────────────┤
                                                                              ▼
                          ┌─────────────────────────────────────────────────────────┐
              S1          │  REAL-DATA HARDENING                                      │
   "make the engine eat   │   S1-1 docs ─► S1-2 sec-master ingest ─► S1-3 total-ret   │
    real data correctly"  │                              └─► S1-4 universe/mktcap/liq  │
                          │   S1-2,3,4 ─► S1-5 E2E real-data path (digest-pinned)      │
                          │   S1-6 coverage expansion (python builder, liquid subset)  │
                          └───────────────────────────┬─────────────────────────────┘
                                                       ▼
                          ┌─────────────────────────────────────────────────────────┐
              S2          │  REAL-DATA BENCHMARK                                      │
   "run the pipeline on   │   S2-1 benchmark harness ─► S2-2 reference baselines       │
    real data, measure    │   S2-3 scorecard ─► S2-4 findings + P4 next-steps          │
    honestly"             └─────────────────────────────────────────────────────────┘
```

### S1 — Real-Data Hardening

**Theme:** Tie the as-built ingestion stack, the S6 data layer, and the on-disk real data into **one coherent, PIT-correct, digest-pinned real-data path** — corporate actions applied, a real universe constructed, coverage expanded — so the existing pipeline can be pointed at real US equity data without fabricating returns or leaking the future.

**Sprint merge:** ✅ closed 2026-06-15 — `--no-ff` into `main` (worktree `atx-wt/p3-s1`, branch `feat/p3-s1-real-data`, 15 commits `a47959c..ff14687`). 7/8 units done, S1-6 ⚠️ partial (crawl deferred); suite 1657 → **1681/0/0**; user ref [`sprint1.md`](sprint1.md).
**Plan + ledger:** [`sprint-1-real-data-hardening-implementation-plan.md`](sprint-1-real-data-hardening-implementation-plan.md) · [`sprint-1-progress.md`](sprint-1-progress.md) (opened at S1-0).

| # | Item | Effort | Status | Ledger unit | Files (or Notes) |
|---|------|--------|--------|-------------|------------------|
| 1.0 | Marker — open ledger, as-built recon, **record S5/S6 merge-base reality** + seam map | Light | ✅ `a47959c` | S1-0 | `sprint-1-progress.md` |
| 1.1 | **Documentation** — author `alpha-pipeline-reference.md` (Panel→DSL→backtest→fitness→search→robust→lockbox) + `data-ingestion-reference.md` (parquet→segment→`Dataset`→`Catalog`→`adapt_panel`→`Panel`; adjustment/dividend/universe semantics). The user's "to start" ask | Moderate | ✅ `c95c883` | S1-1 | two `.md` docs under `p3-impl/`; every claim line-anchored at HEAD; survivorship doc-caveat test SKIP→PASS (suite 1681/0/0, 0 skip) |
| 1.2 | **Security-master ingestion** — load `security_master.parquet` + by-symbol partitions into a typed PIT `data::Dataset` (Reference/corporate-action role); as-of + lineage; PIT (knowledge-date, not effective-date, leak guard) | Moderate | ✅ `60e91a0` | S1-2 | `data/corporate_actions.{hpp,cpp}` + test (+`ParquetTable::date32_days`/`null_mask` atx-core lift) |
| 1.3 | **Total-return adjustment** — fold split (`cumulative_adjustment_factor`) + reinvested `cash_dividend` into an adjusted total-return close field; PIT-correct; validated vs known AAPL split/div events, cross-checked vs `--mode adjclose` builder output | Moderate | ✅ `21eae78` | S1-3 | `data/adjust.{hpp,cpp}` + test |
| 1.4 | **Universe / market-cap / liquidity / sector** — `shares_outstanding`→market cap; trailing-ADV liquidity screen; GICS sector field; PIT membership mask; **survivorship caveat documented** | Moderate | ✅ `76f4fc1` | S1-4 | `data/universe.{hpp,cpp}` + test |
| 1.5 | **Real-data E2E path** — databento segment ⋈ corp-actions ⋈ universe → `DatasetCatalog` → `adapt_panel` → `Panel`; end-to-end test over the **actual on-disk smoke data**; **golden digest pin** on the assembled Panel | Heavy | ✅ `d38f35d` | S1-5 | `data/real_panel.{hpp,cpp}` + E2E test; golden digest `0x2a22a873483d9157` (reads the real databento parquet directly — as-built divergence from `build_dated_segments`) |
| 1.6 | **Coverage expansion** — re-run `build_us_split_adjustments.py` over the databento universe → expanded security master for a **liquid subset**; documented data-build (offline web crawl, run + recorded, not blocking engine code) | Moderate | ⚠️ partial | S1-6 | live crawl **deferred** (operator decision; no external request issued) — smoke-3 retained; selection rule (top-N≈500 by trailing-21d $-vol) + polite resume command + verification checklist recorded in `data-ingestion-reference.md` §6.1 + the S1-6 ledger row. Non-blocking per plan; S2 runs on smoke-3 with the coverage caveat |
| 1.7 | Close — residuals→backlog, status table, `sprint1.md`, `--no-ff` merge | Light | ✅ done | S1-close | `sprint1.md` user ref + ledger baton + backlog residuals (atx-core date32/null lift, tsdb hive-seam gap, untracked build script) + this status table; `--no-ff` merge into `main` |

**Exit criteria:** A single documented call assembles a **byte-reproducible** (`digest`-pinned) real-data `Panel` from the on-disk databento OHLCV + security master, with **(a)** split+dividend total-return adjustment validated against known AAPL corporate-action events (≤ tolerance vs the adjclose-mode oracle), **(b)** a market-cap/ADV/sector-screened PIT universe with the survivorship bias explicitly documented, and **(c)** no look-ahead (the as-of ingestion uses knowledge-date, not effective-date; a truncation-invariance test passes). Coverage is expanded to a liquid subset sufficient to seed S2. The two reference docs exist and are accurate to the as-built code. **Validation evidence lives in the S1-5 E2E test + the S1-3 event-validation test; the full benchmark is S2, not S1.**

### S2 — Real-Data Benchmark *(outline — frozen at S2 kickoff, not now)*

**Theme:** Point the **unchanged** p2 mine→validate→robust→book pipeline at the S1 real-data Panel over the expanded liquid universe, and produce an **honest scorecard**: what survives deflation/CPCV/regime/lockbox, how it correlates, what it costs, and — stated as a first-class result — whether the gates correctly admit ~nothing on a real efficient market.

**Sprint merge:** ⏳ pending (not opened).
**Plan + ledger:** `sprint-2-real-data-benchmark-implementation-plan.md` (written at S2 kickoff) · `sprint-2-progress.md`.

| # | Item | Effort | Status | Ledger unit | Notes |
|---|------|--------|--------|-------------|-------|
| 2.0 | Marker — open ledger, freeze S2 scope against the S1 real-data Panel | Light | ⏳ pending | S2-0 | depends on S1 merged |
| 2.1 | **Benchmark harness** — load expanded liquid universe → real-data `Panel` → `RobustResearchDriver` (mine→gate→robust→lockbox→book) over real data; deterministic, digest-pinned, time-budgeted | Heavy | ⏳ pending | S2-1 | reuses S1-5 path |
| 2.2 | **Reference baselines** — evaluate canonical stylized-fact alphas (momentum, short-term reversal, low-vol, size) as references: does the framework reproduce known signs/magnitudes on real data? | Moderate | ⏳ pending | S2-2 | sanity oracle for the harness |
| 2.3 | **Scorecard** — what cleared each gate; cross-correlations; capacity/cost; net-of-cost post-deflation numbers; **caveats (short history, listed-only universe, coverage) reported as results** | Moderate | ⏳ pending | S2-3 | `sprint2.md` scorecard |
| 2.4 | **Findings + next-steps** — what works, what doesn't, can we find real alpha, and the prioritized P4 backlog this run surfaces | Light | ⏳ pending | S2-4 | findings doc |
| 2.5 | Close — residuals→backlog, status, merge | Light | ⏳ pending | S2-close | — |

**Exit criteria (provisional — finalized at kickoff):** A reproducible benchmark run over the liquid universe with a written scorecard that (a) reproduces at least the **sign** of the reference stylized-fact alphas (sanity), (b) reports the post-deflation / net-of-cost survivors with cross-correlations and capacity, and (c) states the survivorship/history/coverage caveats explicitly. ***"The battery correctly admitted ~nothing"* is an acceptable, passing outcome** — the failure outcome is an *unexplained* admission (snooping the gates) or a data bug that fabricates returns.

---

## ROADMAP-item ↔ ledger-unit mapping

p3's ROADMAP items map **1:1** to ledger units (`1.N` ↔ `S1-N`, `2.N` ↔ `S2-N`), so no separate bridge table is needed (per `module.md` §7, the bridge is only required when they diverge — as p0 Phase 4 did). If a unit sub-splits (`S1-5a/S1-5b`) during execution, record the split in the ledger and add a bridge row here at that time.

---

## Phase 1d / future-work backlog *(seeded; lifted from completed sprints on close)*

These are deliberately **out of S1/S2** and parked here so they are not silently dropped:

- **Dividend cash-flow simulation** *(carried from the S1-3 fork)* — the chosen model is **adjusted total-return** (dividends folded into the return series). Modeling dividends as explicit cash flows on ex-date inside `ExecutionSimulator` / the book (share-count changes on splits, cash credit on dividends) is a larger lift deferred to P4. Total-return is correct for *signal + relative PnL*; cash-flow accounting matters for *absolute book NAV* and is where live-like fidelity goes.
- **Delisted / survivorship recovery** *(carried from S1-4)* — the security-master builder is **listed-only** (Nasdaq Trader active symbols; `docs/us_split_adjustment_factors.md` "Limitations"). A true survivorship-free universe needs a delisting-event source. p3 **documents** the bias rather than fixing it; recovery is P4.
- **Broad GICS coverage** *(carried from S1-4)* — `gics_*` is licensed/sparse; SEC SIC is the open fallback. A fully-classified universe needs a licensed sector map. p3 uses what's open and labels the gaps.
- **Intraday / multi-asset / multi-region** — p3 is **US equity daily** only. The S6 layer + databento OPRA option files on disk (`xom_opra_*`) make intraday/options a natural P4+, but they are out of v4 scope.
- **Point-in-time fundamental restatement** — `shares_outstanding` is forward-filled by SEC fact `end` date with `shares_filed_date` for provenance. True bitemporal restatement handling (first-print vs revised) is a P4 refinement; p3 uses knowledge-date filing dates as the leak guard.

---

## Anti-roadmap — what p3 is explicitly NOT doing

1. **No new alpha-discovery capability.** No new DSL operator, objective, search strategy, or solver. p3 runs the *existing* pipeline on real data. New capability is p4+.
2. **No live execution.** Unchanged from p2 — order routing / venue interaction is the broker's job. p3 stops at a backtested book.
3. **No new asset class or frequency.** US equity daily only. No options (despite the OPRA files on disk), no intraday, no FX/futures/crypto.
4. **No dividend cash-flow engine.** Total-return adjustment only; absolute-NAV cash-flow accounting is backlogged (above).
5. **No survivorship-free universe.** p3 *documents* the listed-only bias; it does not source delisting events to fix it.
6. **No new vendor integration.** databento + the existing open-data builder (Yahoo/SEC/Nasdaq) only. No new paid data feed.
7. **No re-architecture of `atx-tsdb` or the S6 layer.** p3 is a *consumer* that adds adapters between on-disk parquet and `Dataset`; it does not change the segment format or the catalog API.
8. **No "make the benchmark find alpha."** The benchmark reports what it finds. Tuning gates until something passes is exactly the snooping p3 exists to detect.

---

## Strategic decisions — resolved at kickoff

The two material forks were resolved by the user at p3 kickoff (2026-06-14):

1. **Corporate-action model → adjusted total-return series.** Build a split+dividend-adjusted close as a PIT `Dataset` field; alpha signals **and** backtest returns use the adjusted/total-return series; dividends are folded into returns, not modeled as cash flows. Rationale: the engine has zero cash-flow machinery; total-return is the correct, testable, lowest-risk path for a *signal-quality validation*. The raw cash-flow sim is backlogged to P4. *(Settles the S1-3 fork.)*
2. **Benchmark universe → expand coverage first.** S1-6 re-runs the security-master builder over the databento universe so corporate actions cover a **liquid subset** (hundreds of names); S2 benchmarks on that subset. Rationale: the 3-symbol intersection is too thin for a credible alpha hunt; the broad-databento-with-partial-adjustment option was rejected as too incorrect to anchor a validation claim. Cost: an external-web data-build dependency + runtime added to S1. *(Settles the S2-universe fork.)*

Open forks deferred to their sprints: the exact liquidity threshold + universe size (S1-4 / S2-1) and the S2 search budget (S2-1).

**Settled at S1-close (2026-06-15):** the **total-return reinvestment tolerance** fork (S1-3) — the oracle cross-check uses the documented **hand-fixture fallback** (5 published AAPL ex-date triples) at `ε_oracle = 1e-9`, with split-continuity at `ε_event = 1e-6`; the `--mode adjclose` web-crawl oracle was not needed. The S1-4 universe **defaults** shipped (`adv_window=21`, `min_adv_usd=1e6`, `min_mktcap_usd=0`, `top_n_by_adv=0`), but the *final benchmark* threshold + universe size stay an open S2-1 fork (they depend on the still-deferred S1-6 coverage expansion).

---

## Recommended sequencing

If you can only do one slice, do **S1-1 → S1-2 → S1-3 → S1-5** (document the pipeline, ingest the security master, get total-return adjustment correct, and prove the end-to-end real-data Panel builds deterministically). That alone closes the dangerous gap — a real-data path that **does not fabricate returns** — and is independently valuable even if the benchmark never runs.

If two slices, add **S1-4 + S1-6** (a real universe + expanded coverage) so the panel is economically meaningful and S2 has something to chew on.

If three, run **S2** end-to-end and publish the scorecard.

The one unit you must not skip or rush is **S1-3 (total-return adjustment)** validated against known events: a wrong adjustment silently fabricates returns and poisons *every* downstream number. The one precondition you must not leave implicit is **S1-0's S5/S6 merge-base record**: the whole module assumes them DONE.

---

*Sprint discipline: same as `p0`/`p1`/`p2` — marker + per-unit + close commits, `--no-ff` merge, no push unless asked. See [`../docs/sprint.md`](../docs/sprint.md) and [`../docs/module.md`](../docs/module.md). Implementation quality is mandatory for every code unit: [`../docs/implementation-quality.md`](../docs/implementation-quality.md).*
