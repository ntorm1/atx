# pf3 — Signal-Ready Fundamentals Factor Warehouse (backtest panel + backfill DAG + arch v2)

**Created:** 2026-07-04. Successor to **pf2** ([../pf2/ROADMAP.md](../pf2/ROADMAP.md)), which drove US-equity
fundamentals to FactSet / S&P GMI Compustat **depth** (standardization engine, PIT snapshot + as-first-reported
vintages, industry-specialized statement templates, calendarization/TTM, segment/footnote sub-ledgers,
preliminary press-release capture, populated valuation multiples + cross-vendor reconciliation) on a
**production surface** (schema-as-contract + drift, migration governance + backup/DR, quality-as-SLO gating,
observability, storage management).

**Scoped against** the warehouse north star ([../../WAREHOUSE_PARITY_NEXT_AGENT_README.md](../../WAREHOUSE_PARITY_NEXT_AGENT_README.md)),
the residual gap matrix ([../../db/PARITY_GAP.md](../../db/PARITY_GAP.md)), and a 2026-07-04 code review of the
existing partial signal layer (`db/features.py`, `db/alpha_research.py`, `db/formula_library.py`, the
`feature_*` / `alpha_*` lake surfaces), the dataset-scoped orchestrator (`db/orchestrator.py`), and the
proof-slice data posture. Design spec:
[../../../docs/superpowers/specs/2026-07-04-fundamentals-factor-warehouse-pf3-design.md](../../../docs/superpowers/specs/2026-07-04-fundamentals-factor-warehouse-pf3-design.md).

**Assumes pf1 PF-S1…PF-S8 and pf2 PF2-S1…PF2-S10 have landed** (merged to `main`). Where pf3 references a
pf1/pf2 deliverable whose exact landed name differs at implementation time, the pf3 implementer reconciles to
the landed name.

---

## The northstar (pf3 acceptance)

> A fresh agent can, on a **governed backfill DAG**, materialize a **point-in-time, backtest-ready factor
> panel** over the full US common-equity universe — with every FactSet / S&P GMI Compustat ratio and derived
> metric present, and *surpassed* on four committed axes — exported as **schema-contracted views plus
> partitioned Parquet/Arrow** that pipe directly into a quant backtesting engine, accompanied by
> **factor-level signal evaluation** (information coefficient, decay, turnover, crowding) and
> **orchestrator-gated** data quality, all **reproducibly rebuildable** through the pf1/pf2 orchestrator.

pf1 proved the **spine and engine**; pf2 proved the **depth and the production surface** on a ~1-year proof
slice. pf3 delivers the two things a depth-warehouse still lacks to be a *quant product*: a **signal-ready
factor store** (the flagship) on **production backfill rails** (the DAG, schema-contract v2, and an
architecture that can carry the load). It is a **two-track** module:

- **Track A — Signal-ready content (flagship).** Complete fundamental coverage → a comprehensive ratio/metric
  engine → a factor definition framework + PIT factor engine with cross-sectional operators → fundamental &
  cross-domain factor families → a backtest export panel → a signal evaluation surface.
- **Track B — Production rails.** A full backfill + incremental-maintenance DAG; schema-contract v2 (close
  pf2's 56-table PIT gap; semantic unit/sign/scale contracts; a panel export contract); and an architecture
  decomposition of the load-bearing monoliths.

### The four "surpass" axes (all committed for pf3)

1. **Full lineage / transparency.** Every standardized value, ratio, metric, and factor traces — queryably,
   PIT-safely — to its source XBRL fact(s), formula, standardization rule, and restatement vintage.
2. **Signal-native derived factors.** Factors vendors do not compute: accruals quality, PIT
   revisions-momentum, standardization-delta anomalies, footnote/segment-derived signals.
3. **PIT-perfect vintages + freshness.** As-first-reported vs most-recently-restated everywhere in the factor
   inputs (extending pf2-S4), plus preliminary press-release capture (pf2-S8) ahead of vendor timestamps.
4. **Open cross-vendor reconciliation.** Continuous `fact_disagreement` vs multiple injectable baselines with
   a **published agreement SLA**.

---

## The facts that define the work (measured 2026-07-04 against the repo)

pf3 is **not greenfield** — the warehouse already carries the seeds of every track; the work is to complete,
densify, and productionize them.

1. **A partial signal layer already exists.** `db/features.py` (a feature engine with a `FEATURE_DEFINITIONS`
   catalog: returns / momentum / volatility), `db/alpha_research.py` (`AlphaExpressionSpec` with
   cross-sectional `zscore_cs` alphas), `db/formula_library.py` (`formula_registry` definition-as-data). Lake
   surfaces present: `feature_definitions`, `feature_values`, `feature_set_catalog`, `feature_build_manifests`,
   `feature_dependency_edges`, `alpha_signal_values`, `alpha_expression_catalog`, `alpha_backtest_manifests`,
   and a `v_alpha_daily_panel` view. → pf3 **completes** this into a governed, PIT-safe factor store. **S7–S11.**
2. **The DAG orchestrator is dataset-scoped, not a backfill engine.** pf1-S2's `DatasetOrchestrator` builds a
   dataset-id DAG from `DATASET_REGISTRY`, records `etl_job_runs`/`etl_job_steps`/`etl_job_audit`, applies
   watermark-driven incremental skips, and supports retry/backoff/resume — but it is *rebuild orchestration*,
   not a *windowed historical backfill + ongoing-maintenance* engine. → **S1.**
3. **The data is a proof slice, not a backfill.** `equity_daily_bars` ≈ 3.18M rows for 2012–2014;
   companyfacts fundamentals are 2017–2026. The near-empty price×fundamental overlap is exactly why pf2-S9
   valuation multiples emit so few rows. Dense factors need a real historical price backfill — deferred out of
   pf2 by design. → **S4.**
4. **Several modules are monoliths.** `db/migrations.py` (~502K), `db/quality.py` (~309K), `db/asof.py`
   (~161K), `db/estimates.py` (~150K), `db/fundamental_statements.py` (~129K) — correct but hard to reason
   about and extend safely. → **S3.**
5. **A known PIT-column gap is pinned.** pf2-S1's `pit_column_presence` gate fires on ~56 pre-existing fact
   tables missing ≥1 PIT column (mostly `is_latest_revision`); pf2 pinned it as a `<=` ratchet and deferred
   the backfill/exemption. → **S2.**
6. **Core parity gaps remain.** Float/treasury shares, EV components, observed DLRET terminal returns, and
   share-class detail are Partial/Missing in `db/PARITY_GAP.md` — the valuation inputs the factor store
   depends on. (ESG, supply-chain, licensed estimate feeds, IFRS remain parked.) → **S5.**

**Downstream consumer.** The existing quant engine (`atx-engine`) is a C++ **options** pipeline over ORATS
(`load → panel → discover → combine → optimize → report`, consuming `.seg`/`.bin`). pf3's factor panel is
delivered **engine-agnostic** — schema-contracted DuckDB views plus partitioned Parquet/Arrow in the lake —
so any backtester (including a future atx-engine equity mode) can consume it PIT-safely, rather than coupling
pf3 to one engine's native binary format.

---

## The twelve sprints

| Sprint | Track | Theme | Goal metric | Doc |
|---|---|---|---|---|
| **PF3-S1** | Rails | **Backfill + maintenance DAG** — extend `DatasetOrchestrator` with a windowed/chunked/**resumable backfill mode** + an **incremental maintenance mode**; per-partition watermarks; bounded parallel fan-out; dead-letter + retry; DAG-run observability | full-history backfill runs resumably + idempotently on a slice; an immediate incremental re-run is a no-op; DAG state queryable | [sprint-1-backfill-maintenance-dag.md](sprint-1-backfill-maintenance-dag.md) |
| **PF3-S2** | Rails | **Schema-contract v2** — close pf2-S1's 56-table PIT-column gap (backfill or explicit exempt); add semantic contracts (unit/sign/scale/natural-key) to every fact column; version the contract; author the **panel export contract** | 0 missing-PIT offenders; every fact column carries unit + sign; contract version pinned + drift-checked | [sprint-2-schema-contract-v2.md](sprint-2-schema-contract-v2.md) |
| **PF3-S3** | Rails | **Architecture decomposition** — split the load-bearing monoliths (`migrations`/`quality`/`asof`/`estimates`) into cohesive sub-packages behind stable public interfaces; add a module-boundary + import-graph lint gate | each decomposed module is a package of focused units; public API regression-locked; boundary lint green | [sprint-3-architecture-decomposition.md](sprint-3-architecture-decomposition.md) |
| **PF3-S4** | Content | **PIT universe + price backfill** — a PIT US common-equity **universe membership** surface; historical price-bar backfill (2014→present) through the S1 DAG; a dense **price×fundamental overlap** | universe PIT-queryable; overlap dense enough that multiples and factors emit real rows | [sprint-4-universe-price-backfill.md](sprint-4-universe-price-backfill.md) |
| **PF3-S5** | Content | **Fundamentals completeness** — close remaining core parity gaps the factor store needs (float/treasury shares, EV components, observed DLRET, share-class); every canonical item populated | valuation inputs complete; EV computable per security-day; no core-item stubs on the slice | [sprint-5-fundamentals-completeness.md](sprint-5-fundamentals-completeness.md) |
| **PF3-S6** | Content | **Ratio & metric engine v2** — complete the Compustat/FactSet ratio & metric catalog (margins/returns/efficiency/leverage/coverage/liquidity/per-share/**growth-CAGR**/valuation), formula-registry-driven, full lineage + PIT/TTM/calendar-aware | full ratio catalog emits; every ratio traces to source facts + formula + vintage; TTM/calendar-aligned | [sprint-6-ratio-metric-engine-v2.md](sprint-6-ratio-metric-engine-v2.md) |
| **PF3-S7** | Content | **Factor framework + PIT engine + cross-sectional operators** — definition-as-data **factor catalog** (extends `features.py`) with a dependency DAG and PIT-safe compute; governed **rank/zscore/winsorize/neutralize** (sector/size/beta) | factor catalog + dependency DAG; cross-sectional operators PIT-safe; leakage/lookahead test green | [sprint-7-factor-framework-engine.md](sprint-7-factor-framework-engine.md) |
| **PF3-S8** | Content | **Fundamental factor families** — value/quality/profitability/growth/investment/leverage/**accruals**/distress (Piotroski F, Altman Z, Novy-Marx, Sloan) plus **signal-native** (revisions momentum, standardization-delta, footnote/segment-derived) | each family emits a PIT factor panel; signal-native factors present + lineage-traced | [sprint-8-fundamental-factor-families.md](sprint-8-fundamental-factor-families.md) |
| **PF3-S9** | Content | **Cross-domain factors + unified panel** — integrate price/liquidity (`equity_price_metrics`), estimate revisions, 13F flow, short-interest, insider into **one factor namespace**; assemble the unified panel | every domain in one namespace with consistent keys/units; unified panel assembles | [sprint-9-cross-domain-factors.md](sprint-9-cross-domain-factors.md) |
| **PF3-S10** | Content | **Backtest export contract** — `v_factor_panel` catalogued **PIT views** plus partitioned **Parquet/Arrow** export to the lake; schema-contracted, universe-filtered, engine-agnostic | panel exports to views + Parquet/Arrow; export contract enforced; a consumer loads it PIT-safely with zero lookahead | [sprint-10-backtest-export-panel.md](sprint-10-backtest-export-panel.md) |
| **PF3-S11** | Content | **Signal evaluation surface** — rank-IC + IC decay, quantile/decile spread returns, turnover, factor-to-factor correlation/crowding, breadth; factor DQC | every factor scored (IC/decay/turnover/breadth); leakage + coverage DQC gated | [sprint-11-signal-evaluation.md](sprint-11-signal-evaluation.md) |
| **PF3-S12** | Rails | **Production capstone** — backfill proof + codified incremental-maintenance schedule; factor-panel quality **gated in the orchestrator**; observability (factor freshness SLA, panel anomaly, lineage-completeness); reproducible full-rebuild + backfill runbook; **surpass-ledger flip**; whole-branch review | orchestrator halts on a panel-critical check; rebuild deterministic; 4 surpass axes evidenced | [sprint-12-production-capstone.md](sprint-12-production-capstone.md) |

---

## Primary-module ownership + shared append-only hubs

Like pf1/pf2, pf3 treats the shared hubs (`schema.py`, `migrations.py`, `jobs.py`, `orchestrator.py`,
`quality.py`, `asof.py`, `lake.py`, `parity.py`, plus `PARITY_GAP.md` / `WAREHOUSE_PARITY_TRANCHES.md`) as
**append-only coordination surfaces**: each sprint appends new tables / factors / checks / readers under a
**reserved migration range** and never edits a prior migration or another sprint's region.

| Sprint | Primary modules (substantially owns / creates) | Reserved migrations |
|---|---|---|
| PF3-S1 | `db/orchestrator.py` backfill/maintenance extension, NEW `db/backfill.py`, NEW `scripts/warehouse_backfill.py`; `db/tests/test_backfill.py` | `0132–0134` |
| PF3-S2 | `db/schema_contract.py` v2 (semantic + versioned), PIT-gap close in `db/quality.py`/migrations, NEW `db/panel_contract.py` stub; `db/tests/test_schema_contract_v2.py` | `0135–0137` |
| PF3-S3 | decomposition of `db/migrations.py` / `db/quality.py` / `db/asof.py` / `db/estimates.py` into sub-packages, NEW module-boundary lint; `db/tests/test_module_boundaries.py` | `0138–0139` |
| PF3-S4 | NEW `db/universe.py` (PIT membership), price backfill via `db/pricing_bulk.py` + S1 DAG; `db/tests/test_universe.py` | `0140–0143` |
| PF3-S5 | `db/shares_outstanding.py` (float/treasury), NEW `db/enterprise_value.py`, `db/delisting.py` DLRET populate; `db/tests/test_enterprise_value.py` | `0144–0147` |
| PF3-S6 | `db/fundamental_ratios.py` + `db/formula_library.py` completion, NEW `db/metric_engine.py`; `db/tests/test_metric_engine.py` | `0148–0151` |
| PF3-S7 | NEW `db/factors/` package (`catalog.py`, `engine.py`, `cross_section.py`) extending `db/features.py`; `db/tests/test_factor_engine.py` | `0152–0155` |
| PF3-S8 | NEW `db/factors/fundamental_families.py`, seed `db/seeds/factor_definitions.csv`; `db/tests/test_fundamental_factors.py` | `0156–0159` |
| PF3-S9 | NEW `db/factors/cross_domain.py`, unified-namespace assembly; `db/tests/test_cross_domain_factors.py` | `0160–0163` |
| PF3-S10 | NEW `db/factor_panel.py`, `v_factor_panel` views, lake Parquet/Arrow export; `db/tests/test_factor_panel.py` | `0164–0167` |
| PF3-S11 | NEW `db/signal_eval.py` (IC/decay/turnover/crowding), factor DQC in `db/quality.py`; `db/tests/test_signal_eval.py` | `0168–0171` |
| PF3-S12 | `db/orchestrator.py` panel gating, `db/observability.py` factor SLAs, `db/parity.py` surpass-ledger, rebuild runbook; `db/tests/test_pf3_capstone.py` | `0172–0175` |

**Overlap note.** PF3-S4/S5/S6 touch the `fundamental_*`/pricing surfaces; PF3-S7/S8/S9 share the
`db/factors/` package — run each group **sequentially within itself**, never concurrently in the same tree.
Disjoint-module sprints MAY run concurrently in isolated worktrees.

---

## Shared PIT / determinism + production contract (every sprint)

pf1 clauses **(A)–(D)** and pf2 clauses **(E)–(G)** carry forward unchanged; pf3 adds **(H)–(J)**.

**(A) Bitemporal correctness / no lookahead.** Every fact/derived row carries `as_of_date`, `available_at`,
`source_loaded_at`, `run_id`, `is_latest_revision`. A derived value sets `available_at = max(input.available_at)`.
As-of readers gate on the valid window **and** `available_at ≤ as_of_ts`.

**(B) Append-only, catalogued migrations.** Forward-only, idempotent (`CREATE … IF NOT EXISTS` /
`ADD COLUMN IF NOT EXISTS`). Each sprint uses only its reserved range; never renumber or edit a landed
migration. Every new table/view seeds `table_catalog` + `field_catalog` in the same migration. Split schema
from index across migration numbers; preserve a timestamped DB+WAL backup before any live apply.

**(C) Offline / no-network tests.** Every test runs against in-memory / template-copy DuckDB with fixture or
injected data. No SEC / FRED / FINRA / OpenFIGI / GLEIF / vendor network in pytest. Live connectors stay
behind injectable file options; live smoke is operator-run and recorded in the ledger.

**(D) Determinism + provenance.** `compute_*` transforms are pure (pandas in → long DataFrame out),
unit-tested independent of DuckDB. Every derived row records its input lineage. Same inputs + params → same rows.

**(E) Schema-as-contract *(pf2-S1).*** No table lands without a contract row **and** a `table_catalog` entry;
the drift check fails on divergence or any uncatalogued table.

**(F) Backup-before-migrate *(pf2-S2).*** Every live migration apply is preceded by a scripted CHECKPOINT +
timestamped backup and followed by a verify; WAL-split discipline is the standing invariant.

**(G) Quality-gated *(pf2-S10).*** A check authored `severity=critical` is wired into the orchestrator and
*halts* the affected run. Each sprint that adds a load-bearing invariant registers it as a gated check.

**(H) Backfill-safe *(new — PF3-S1).*** Every backfilled surface is windowed, chunked, resumable, and
idempotent: re-running a completed window is a no-op; a partial window resumes without duplication; per-partition
watermarks record progress. No unbounded full-table rewrites.

**(I) Panel PIT-safety *(new — PF3-S10).*** The exported factor panel is point-in-time by construction: a row
keyed at `(security_id, as_of_date)` uses only inputs with `available_at ≤ as_of_date`, cross-sectional
operators rank only within the as-of cross-section, and universe membership is applied as-of. A
lookahead-detection test gates the export.

**(J) Semantic contract *(new — PF3-S2).*** Every fact/metric/factor column declares unit, sign convention,
and scale in the contract; a check fails if a value violates its declared unit/sign domain.

**Data posture (pf3-specific).** Content sprints ship an injectable loader + engine + offline fixtures, then
an operator-run proof slice with live counts recorded in the ledger. **pf3 builds and proves the backfill DAG
but does not execute the full multi-year historical backfill in-module** — the full-history load is a
documented, resumable operator job. pf3 proves determinism, resumability, and idempotency on a bounded slice;
the operator runs the archive.

**Process (all sprints):** each sprint runs in its **own git worktree** off the integration mainline (`main`),
merged back at sprint end, via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>` — schema churn never
touches the primary tree and the git-ignored multi-GB DB is never copied. Controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; implementers use TDD +
verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked; commit
trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. New module ⇒ new
`test_*.py`. `python -m pytest atx-impl\db\tests -q` green in the worktree before commit; operator live-DB
smoke runs against the shared DB in the primary tree (backed up first, clause F). Update `PARITY_GAP.md` and
append a `WAREHOUSE_PARITY_TRANCHES.md` row per sprint.

---

## Sequencing

1. **Rails wave — PF3-S1 → PF3-S2 → PF3-S3 (sequential).** Land the backfill/maintenance DAG, then
   schema-contract v2 (the export contract and PIT-gap close depend on it), then the architecture
   decomposition — so content sprints build on governed, decomposed rails.
2. **Data + ratio foundation — PF3-S4 → PF3-S5 → PF3-S6 (sequential; share `fundamental_*`/pricing).** Densify
   the data, complete coverage, then complete the ratio/metric engine that reads it.
3. **Factor build — PF3-S7 → PF3-S8 → PF3-S9 (sequential; share `db/factors/`).** Framework + operators first,
   then fundamental families, then cross-domain integration into one namespace.
4. **Export + evaluation — PF3-S10 → PF3-S11.** S10 may begin once S9's factor namespace lands; S11 scores the
   exported panel.
5. **Production capstone — PF3-S12 (last).** Gates, monitors, and reproducibly rebuilds everything pf3 built;
   closes with the surpass-ledger flip and a whole-branch review.

**If you can only do a subset:** PF3-S1 + PF3-S4 (rails + dense data — the precondition for any real factor) →
PF3-S7 (factor framework) → PF3-S8 (fundamental factors) → PF3-S10 (export) → PF3-S12 (make it trustworthy).
S1+S4+S7+S8+S10 is the minimal end-to-end "fundamentals → factors → backtest panel" slice.

---

## North star (pf3 acceptance)

A fresh agent can, **on a governed backfill DAG** (built + proven on a slice; full-history backfill an
operator job), rebuild the fundamentals warehouse and get, PIT-safely:

- a **complete** US-equity fundamentals surface — every canonical item populated, standardized (pf2-S3), with
  a **full Compustat/FactSet ratio & metric catalog** (pf3-S6), each number **lineage-traced** to source facts
  + formula + vintage;
- a **factor store** — a definition-as-data catalog with a dependency DAG, PIT-safe cross-sectional operators
  (rank/zscore/winsorize/neutralize), fundamental factor families (value/quality/profitability/growth/accruals/
  distress) plus **signal-native** factors vendors don't ship, unified with cross-domain (price/estimate/13F/
  short-interest/insider) factors into **one namespace**;
- a **backtest-ready panel** — `v_factor_panel` catalogued PIT views + partitioned Parquet/Arrow export,
  schema-contracted and universe-filtered, that pipes directly into a quant backtester with **zero lookahead**;
- **factor-level signal evaluation** — IC/decay/turnover/crowding/breadth scoring every factor; and
- a **production platform** — a backfill + incremental-maintenance DAG, schema-contract v2 (unit/sign/version,
  0 missing-PIT), a **decomposed** architecture, orchestrator-gated factor quality, and factor observability —
  such that the whole surface is reproducibly rebuildable and operationally trustworthy.

Parity vs FactSet / S&P GMI Compustat **fundamentals + derived factors** is then closed *and surpassed* on the
four committed axes (full lineage, signal-native factors, PIT-perfect vintages + freshness, open cross-vendor
reconciliation) — on a production-grade backfill/maintenance DAG and a decomposed, contract-hardened codebase.
**Out of scope for pf3 (parked for PF4+):** ESG, supply-chain graph, licensed vendor estimate feeds, and
international / IFRS / ESEF fundamentals.
