# PF3 — Signal-Ready Fundamentals Factor Warehouse (design)

**Created:** 2026-07-04. Successor to **pf2**
([../../../atx-impl/plans/pf2/ROADMAP.md](../../../atx-impl/plans/pf2/ROADMAP.md)), which drove US-equity
fundamentals to FactSet / S&P GMI Compustat *depth* (standardization engine, PIT snapshots + vintages,
industry-specialized templates, calendarization/TTM, segment/footnote sub-ledgers, preliminary
press-release capture, populated valuation multiples + cross-vendor reconciliation) on a *production
surface* (schema-as-contract + drift, migration governance + backup/DR, quality-as-SLO gating,
observability, storage management).

**Assumes pf1 PF-S1…PF-S8 and pf2 PF2-S1…PF2-S10 have landed** (merged to `main`). Where PF3 references
a pf1/pf2 deliverable whose exact landed name differs at implementation time, the PF3 implementer
reconciles to the landed name.

**Design spec location.** This document is the PF3 design record. The concrete roadmap and per-sprint
plan files live under `atx-impl/plans/pf3/` (ROADMAP.md, IMPLEMENT_ALL_SPRINTS_GOAL.md, and
`sprint-N-*.md`), generated from this spec.

---

## The northstar (PF3 acceptance)

> A fresh agent can, on a **governed backfill DAG**, materialize a **point-in-time, backtest-ready factor
> panel** over the full US common-equity universe — with every FactSet / S&P GMI Compustat ratio and
> derived metric present, and *surpassed* on four committed axes — exported as **schema-contracted views
> plus partitioned Parquet/Arrow** that pipe directly into a quant backtesting engine, accompanied by
> **factor-level signal evaluation** (information coefficient, decay, turnover, crowding) and
> **orchestrator-gated** data quality, all **reproducibly rebuildable** through the pf1/pf2 orchestrator.

pf1 proved the **spine and engine**. pf2 proved the **depth and the production surface** on a ~1-year
proof slice. PF3 delivers the two things a raw depth-warehouse still lacks to be a *quant product*: a
**signal-ready factor store** (the flagship) sitting on **production backfill rails** (the DAG,
schema-contract v2, and an architecture that can carry the load).

### The four "surpass" axes (all committed for PF3)

A FactSet/Compustat parity target is table stakes; PF3 commits to going **beyond** vendor capability on
four axes, each with a concrete, evidenced deliverable at capstone:

1. **Full lineage / transparency.** Every standardized value, ratio, metric, and factor traces —
   queryably, PIT-safely — to its source XBRL fact(s), formula, standardization rule, and restatement
   vintage. Vendors ship black-box numbers; PF3 ships an auditable chain.
2. **Signal-native derived factors.** Factors vendors do not compute: accruals quality, PIT
   revisions-momentum, standardization-delta anomalies, and footnote/segment-derived signals — first-class
   in the factor catalog.
3. **PIT-perfect vintages + freshness.** As-first-reported vs most-recently-restated everywhere in the
   factor inputs (extending pf2-S4), plus preliminary press-release capture (pf2-S8) ahead of vendor
   consensus timestamps.
4. **Open cross-vendor reconciliation.** Continuous `fact_disagreement` against multiple injectable
   baselines with a **published agreement SLA** — audit-grade, not opaque.

---

## Current state (measured 2026-07-04 against the repo)

PF3 is **not greenfield**. The warehouse already carries the seeds of every PF3 track; the work is to
*complete, densify, and productionize* them.

1. **A partial signal layer already exists.** `db/features.py` (a feature engine with a
   `FEATURE_DEFINITIONS` catalog: returns, momentum, volatility), `db/alpha_research.py`
   (`AlphaExpressionSpec` with cross-sectional `zscore_cs` alphas), and `db/formula_library.py`
   (`formula_registry` definition-as-data). Lake surfaces already present: `feature_definitions`,
   `feature_values`, `feature_set_catalog`, `feature_build_manifests`, `feature_dependency_edges`,
   `alpha_signal_values`, `alpha_expression_catalog`, `alpha_backtest_manifests`, and a
   `v_alpha_daily_panel` view. PF3 **completes** this into a governed, PIT-safe factor store — it does not
   start from zero.
2. **A DAG orchestrator already exists but is dataset-scoped.** pf1-S2's `DatasetOrchestrator` builds a
   deterministic dataset-id DAG from `DATASET_REGISTRY`, records `etl_job_runs` / `etl_job_steps` /
   `etl_job_audit`, applies watermark-driven incremental skips, and supports retry/backoff/resume. It is
   *rebuild orchestration*, not a *windowed historical backfill + ongoing-maintenance* engine. PF3
   extends it into both.
3. **The data is a proof slice, not a backfill.** `equity_daily_bars` holds ~3.18M rows for 2012–2014;
   companyfacts fundamentals are 2017–2026. The near-empty price×fundamental overlap is exactly why
   pf2-S9 valuation multiples emit so few rows. Dense factors need a real historical price backfill —
   deferred out of pf2 by design, owned by PF3.
4. **Several modules are monoliths.** `db/migrations.py` (~502K), `db/quality.py` (~309K), `db/asof.py`
   (~161K), `db/estimates.py` (~150K), `db/fundamental_statements.py` (~129K). These grew append-only
   across ~45 sprints; they are correct but hard to reason about and hard to extend safely. PF3 decomposes
   the load-bearing ones behind stable interfaces.
5. **A known PIT-column gap is pinned.** pf2-S1's `pit_column_presence` gate correctly fires on ~56
   pre-existing fact tables missing ≥1 PIT column (mostly `is_latest_revision`). pf2 pinned this as a
   `<=` ratchet and deferred the backfill/exemption to "when critical checks halt." PF3 owns closing it.
6. **Parity still has core gaps.** Float/treasury shares, EV components, observed DLRET terminal returns,
   and some share-class detail are Partial/Missing in `db/PARITY_GAP.md` — the valuation inputs the factor
   store depends on. (ESG, supply-chain, licensed vendor estimate feeds, and international/IFRS remain
   parked — see Out of Scope.)

**Downstream consumer.** The existing quant engine (`atx-engine`) is a C++ **options** pipeline over
ORATS (`load → panel → discover → combine → optimize → report`, consuming `.seg`/`.bin`). PF3's factor
panel is therefore delivered **engine-agnostic** — schema-contracted DuckDB views plus partitioned
Parquet/Arrow in the lake — so any backtester (including a future atx-engine equity mode) can consume it
PIT-safely, rather than coupling PF3 to one engine's native binary format.

---

## Two tracks

- **Track A — Signal-ready content (the flagship).** Complete fundamental coverage → a comprehensive
  ratio/metric engine → a **factor definition framework + PIT factor engine** with cross-sectional
  operators → fundamental & cross-domain factor families → a **backtest export panel** → a **signal
  evaluation surface**.
- **Track B — Production rails.** A full **backfill + incremental-maintenance DAG**; **schema-contract
  v2** (close the 56-table PIT gap; semantic unit/sign/scale contracts; a panel export contract); and an
  **architecture decomposition** of the load-bearing monoliths behind stable module interfaces.

---

## The twelve sprints

| Sprint | Track | Theme | Goal metric | Reserved migrations |
|---|---|---|---|---|
| **PF3-S1** | Rails | **Backfill + maintenance DAG** — extend `DatasetOrchestrator` with a windowed/chunked/**resumable backfill mode** and an **incremental maintenance mode**; per-partition watermarks; bounded parallel fan-out; dead-letter + retry; DAG-run observability | full-history backfill runs resumably + idempotently on a slice; an immediate incremental re-run is a no-op; DAG state (per-partition watermark, step status) is queryable | 0132–0134 |
| **PF3-S2** | Rails | **Schema-contract v2** — close pf2-S1's 56-table PIT-column gap (backfill or explicit exempt); add semantic contracts (unit / sign / scale / natural-key) to every fact column; version the contract; author the **panel export contract** | 0 missing-PIT offenders (ratchet at 0); every fact column carries unit + sign in the contract; contract version pinned + drift-checked | 0135–0137 |
| **PF3-S3** | Rails | **Architecture decomposition** — split the load-bearing monoliths (`migrations` / `quality` / `asof` / `estimates`) into cohesive sub-packages behind stable public interfaces; add a module-boundary + import-graph lint gate | each decomposed module is a package of focused units; public API is regression-locked (unchanged imports); boundary lint is green | 0138–0139 |
| **PF3-S4** | Content | **PIT universe + price backfill** — a point-in-time US common-equity **universe membership** surface; historical price-bar backfill (2014→present) driven through the S1 DAG; a dense **price×fundamental overlap** | universe membership is PIT-queryable; the overlap is dense enough that multiples and factors emit real rows across the window | 0140–0143 |
| **PF3-S5** | Content | **Fundamentals completeness** — close the remaining core parity gaps the factor store needs (float/treasury shares, enterprise-value components, observed DLRET, share-class detail); every canonical item populated on the slice | valuation inputs complete; EV computable per security-day; no core-item stubs remain on the proof slice | 0144–0147 |
| **PF3-S6** | Content | **Ratio & metric engine v2** — complete the FactSet/Compustat ratio & metric catalog (margins, returns, efficiency, leverage, coverage, liquidity, per-share, **growth/CAGR**, valuation), formula-registry-driven, with full lineage and PIT/TTM/calendar-awareness | the full ratio catalog emits; every ratio traces to source facts + formula + vintage; TTM/calendar-aligned | 0148–0151 |
| **PF3-S7** | Content | **Factor framework + PIT engine + cross-sectional operators** — a definition-as-data **factor catalog** (extending `features.py`) with a dependency DAG and PIT-safe compute; governed **rank / zscore / winsorize / neutralize** (sector / size / beta) operators | factor catalog + dependency DAG exist; cross-sectional operators are PIT-safe; a leakage/lookahead test is green | 0152–0155 |
| **PF3-S8** | Content | **Fundamental factor families** — value, quality, profitability, growth, investment, leverage, **accruals**, and distress families (Piotroski F, Altman Z, Novy-Marx gross profitability, Sloan accruals) plus the **signal-native** set (revisions momentum, standardization-delta, footnote/segment-derived) | each fundamental family emits a PIT factor panel; the signal-native factors (surpass axis 2) are present and lineage-traced | 0156–0159 |
| **PF3-S9** | Content | **Cross-domain factors + unified panel** — integrate the existing price/liquidity (`equity_price_metrics`), estimate-revision, 13F-flow, short-interest, and insider surfaces into **one factor namespace**; assemble the unified panel | every domain lands in one factor namespace with consistent keys/units; the unified panel assembles across domains | 0160–0163 |
| **PF3-S10** | Content | **Backtest export contract** — `v_factor_panel` catalogued **PIT views** plus partitioned **Parquet/Arrow** export to the lake; schema-contracted, universe-filtered, engine-agnostic | the panel exports to views + Parquet/Arrow; the S2 export contract is enforced; a consumer loads it PIT-safely with zero lookahead | 0164–0167 |
| **PF3-S11** | Content | **Signal evaluation surface** — rank-IC and IC decay, quantile/decile spread returns, turnover, factor-to-factor correlation / crowding, breadth; factor-level DQC | every factor is scored (IC / decay / turnover / breadth); leakage and coverage DQC are registered as gated checks | 0168–0171 |
| **PF3-S12** | Rails | **Production capstone** — backfill proof + a codified incremental-maintenance schedule; factor-panel quality **gated in the orchestrator** (halt on panel-critical); observability (factor freshness SLA, panel row-count anomaly, lineage-completeness); a reproducible full-rebuild + backfill runbook; the **surpass-ledger flip**; whole-branch review | the orchestrator halts on a panel-critical check; a fresh-agent rebuild is deterministic; all four surpass axes are evidenced in the ledger | 0172–0175 |

---

## Sequencing

1. **Rails wave — PF3-S1 → PF3-S2 → PF3-S3 (sequential).** Land the backfill/maintenance DAG, then
   schema-contract v2 (which the export contract and the PIT-gap close depend on), then the architecture
   decomposition — so the content sprints build on governed, decomposed rails.
2. **Data + ratio foundation — PF3-S4 → PF3-S5 → PF3-S6 (sequential; they share the `fundamental_*`
   surfaces and the price backfill feeds the ratios).** Densify the data, complete coverage, then
   complete the ratio/metric engine that reads it.
3. **Factor build — PF3-S7 → PF3-S8 → PF3-S9 (sequential; they share the factor engine).** Framework +
   operators first, then fundamental families, then cross-domain integration into one namespace.
4. **Export + evaluation — PF3-S10 → PF3-S11.** S10 may begin once S9's factor namespace lands; S11
   scores the exported panel.
5. **Production capstone — PF3-S12 (last).** Gates, monitors, and reproducibly rebuilds everything PF3
   built; closes with the surpass-ledger flip and a whole-branch review.

**If only a subset is possible:** PF3-S1 + PF3-S4 (rails + dense data — the precondition for any real
factor) → PF3-S7 (factor framework) → PF3-S8 (fundamental factors) → PF3-S10 (export) → PF3-S12 (make it
trustworthy). S1+S4+S7+S8+S10 is the minimal end-to-end "fundamentals → factors → backtest panel" slice.

---

## Shared contract (every sprint)

PF3 inherits the pf1 clauses **(A)–(D)** and pf2 clauses **(E)–(G)** unchanged, and adds **(H)–(J)**.

- **(A) Bitemporal correctness / no lookahead.** Every fact/derived row carries `as_of_date`,
  `available_at`, `source_loaded_at`, `run_id`, `is_latest_revision`. A derived value sets
  `available_at = max(input.available_at)`. As-of readers gate on the valid window **and**
  `available_at ≤ as_of_ts`.
- **(B) Append-only, catalogued migrations.** Forward-only, idempotent; each sprint uses only its
  reserved range; every new table/view seeds `table_catalog` + `field_catalog` in the same migration;
  split schema from index across migration numbers; back up before any live apply.
- **(C) Offline / no-network tests.** Every test runs against in-memory / template-copy DuckDB with
  fixtures or injected data. No SEC / FRED / FINRA / OpenFIGI / GLEIF / vendor network in pytest. Live
  connectors stay behind injectable file options; live smoke is operator-run and recorded in the ledger.
- **(D) Determinism + provenance.** `compute_*` transforms are pure (pandas in → long DataFrame out),
  unit-tested independent of DuckDB. Every derived row records its input lineage. Same inputs + params →
  same rows.
- **(E) Schema-as-contract.** No table lands without a contract row **and** a `table_catalog` entry; the
  drift check fails on divergence or any uncatalogued table.
- **(F) Backup-before-migrate.** Every live migration apply is preceded by a scripted CHECKPOINT +
  timestamped backup and followed by a verify; WAL-split discipline is the standing invariant.
- **(G) Quality-gated.** A check authored `severity=critical` is wired into the orchestrator and *halts*
  the affected run. Each sprint that adds a load-bearing invariant registers it as a gated check.
- **(H) Backfill-safe *(new — PF3-S1).*** Every backfilled surface is windowed, chunked, resumable, and
  idempotent: re-running a completed window is a no-op; a partial window resumes without duplication;
  per-partition watermarks record progress. No unbounded full-table rewrites.
- **(I) Panel PIT-safety *(new — PF3-S10).*** The exported factor panel is point-in-time by construction:
  a row keyed at `(security_id, as_of_date)` uses only inputs with `available_at ≤ as_of_date`,
  cross-sectional operators rank only within the as-of cross-section, and universe membership is applied
  as-of. A lookahead-detection test gates the export.
- **(J) Semantic contract *(new — PF3-S2).*** Every fact/metric/factor column declares unit, sign
  convention, and scale in the contract; a check fails if a value violates its declared unit/sign domain.

**Data posture (PF3-specific).** Following pf2: content sprints ship an injectable loader + engine +
offline fixtures, then an operator-run proof slice with live counts recorded in the ledger. **PF3 builds
and proves the backfill DAG but does not execute the full multi-year historical backfill in-module** —
the full-history load is a documented, resumable operator job. PF3 proves determinism, resumability, and
idempotency on a bounded slice; the operator runs the archive.

**Process (all sprints).** Each sprint runs in its **own git worktree** off the integration mainline
(`main`), merged back at sprint end, via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>` — schema
churn never touches the primary tree and the git-ignored multi-GB DB is never copied. Controller:
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; implementers use TDD +
verification-before-completion). Never `git add -A`; commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. New module ⇒ new `test_*.py`.
`python -m pytest atx-impl\db\tests -q` green in the worktree before every commit; operator live-DB smoke
runs against the shared DB in the primary tree (backed up first, clause F). Per sprint: update
`PARITY_GAP.md` and append a `WAREHOUSE_PARITY_TRANCHES.md` row.

---

## Primary-module ownership + reserved ranges

Like pf1/pf2, PF3 treats the shared hubs (`schema.py`, `migrations.py`, `jobs.py`, `orchestrator.py`,
`quality.py`, `asof.py`, `lake.py`, `parity.py`, plus `PARITY_GAP.md` / `WAREHOUSE_PARITY_TRANCHES.md`)
as **append-only coordination surfaces**. Each sprint appends under its reserved migration range and
never edits a prior migration or another sprint's region.

| Sprint | Primary modules (owns / creates) | Reserved migrations |
|---|---|---|
| PF3-S1 | `db/orchestrator.py` backfill/maintenance extension, NEW `db/backfill.py`, NEW `scripts/warehouse_backfill.py`; `db/tests/test_backfill.py` | 0132–0134 |
| PF3-S2 | `db/schema_contract.py` v2 (semantic + versioned), PIT-gap close in `db/quality.py`/migrations, NEW `db/panel_contract.py` stub; `db/tests/test_schema_contract_v2.py` | 0135–0137 |
| PF3-S3 | decomposition of `db/migrations.py` / `db/quality.py` / `db/asof.py` / `db/estimates.py` into sub-packages, NEW module-boundary lint; `db/tests/test_module_boundaries.py` | 0138–0139 |
| PF3-S4 | NEW `db/universe.py` (PIT membership), price backfill via `db/pricing_bulk.py` + S1 DAG; `db/tests/test_universe.py` | 0140–0143 |
| PF3-S5 | `db/shares_outstanding.py` (float/treasury), NEW `db/enterprise_value.py`, `db/delisting.py` DLRET populate; `db/tests/test_enterprise_value.py` | 0144–0147 |
| PF3-S6 | `db/fundamental_ratios.py` + `db/formula_library.py` completion, NEW `db/metric_engine.py`; `db/tests/test_metric_engine.py` | 0148–0151 |
| PF3-S7 | NEW `db/factors/` package (`catalog.py`, `engine.py`, `cross_section.py`) extending `db/features.py`; `db/tests/test_factor_engine.py` | 0152–0155 |
| PF3-S8 | NEW `db/factors/fundamental_families.py`, seed `db/seeds/factor_definitions.csv`; `db/tests/test_fundamental_factors.py` | 0156–0159 |
| PF3-S9 | NEW `db/factors/cross_domain.py`, unified-namespace assembly; `db/tests/test_cross_domain_factors.py` | 0160–0163 |
| PF3-S10 | NEW `db/factor_panel.py`, `v_factor_panel` views, lake Parquet/Arrow export; `db/tests/test_factor_panel.py` | 0164–0167 |
| PF3-S11 | NEW `db/signal_eval.py` (IC/decay/turnover/crowding), factor DQC in `db/quality.py`; `db/tests/test_signal_eval.py` | 0168–0171 |
| PF3-S12 | `db/orchestrator.py` panel gating, `db/observability.py` factor SLAs, `db/parity.py` surpass-ledger, rebuild runbook; `db/tests/test_pf3_capstone.py` | 0172–0175 |

**Overlap note.** PF3-S4/S5/S6 touch the `fundamental_*`/pricing surfaces and PF3-S7/S8/S9 share the
`db/factors/` package — run each group **sequentially within itself**, never concurrently in the same
tree. Disjoint-module sprints may run concurrently in isolated worktrees.

---

## Out of scope (parked for PF4+)

ESG / sustainability, the supply-chain relationship graph, licensed vendor estimate feeds
(IBES/broker-detail), and international / IFRS / ESEF fundamentals. PF3 is the **US-equity
fundamentals → ratios → factors → backtest** spine: complete, signal-ready, and production-backed. The
parked domains are natural PF4 successors and are logged as such in `PARITY_GAP.md`.

---

## Success = the northstar, evidenced

PF3 is done when the northstar holds and the capstone ledger evidences all four surpass axes: a
governed backfill DAG feeds a dense, complete, standardized fundamentals warehouse; a governed factor
engine produces a PIT-safe backtest-ready panel exported to contracted views + Parquet/Arrow; every
number and factor is lineage-traceable; signal-native factors and PIT-perfect vintages are present;
cross-vendor agreement is tracked to a published SLA; factor-level signal evaluation scores every factor;
and orchestrator-gated quality plus a reproducible rebuild make the whole surface operationally
trustworthy.
