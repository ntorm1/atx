# pf2 — Fundamentals Depth + Production Warehouse (FactSet / S&P GMI Compustat)

**Created:** 2026-07-03. Successor to **pf1** ([../pf1/ROADMAP.md](../pf1/ROADMAP.md)), which drove the
US-equity fundamentals **spine** to Compustat/FactSet parity on six axes (canonical item dimension,
in-repo job orchestrator, XBRL concept coverage, declarative formula registry, FIGI/LEI identifier
spine, valuation-multiple scaffold, dimension-aware XBRL validation, restatement vintage lineage).

**Scoped against** the warehouse north star
([../../WAREHOUSE_PARITY_NEXT_AGENT_README.md](../../WAREHOUSE_PARITY_NEXT_AGENT_README.md)) and a full
2026-07-03 code + research review: the residual fundamentals-parity gap analysis
([../../db/PARITY_GAP.md](../../db/PARITY_GAP.md)), a cross-vendor depth study of what FactSet /
S&P GMI Compustat deliver over raw XBRL, and a production DB/schema-management state map of
`atx-impl/db`. Design spec:
[docs/superpowers/specs/2026-07-03-fundamentals-parity-pf2-design.md](../../../docs/superpowers/specs/2026-07-03-fundamentals-parity-pf2-design.md).

**Assumes pf1 PF-S1…PF-S8 have landed** (item dictionary, orchestrator, formula registry, identifier
spine, valuation-multiple scaffold `valuation_multiples.py`, dimension-aware validation, restatement
lineage). Where pf2 references a pf1 deliverable whose exact name differs at implementation time, the
pf2 implementer reconciles to the landed name.

---

## The northstar (pf2 acceptance)

> Bring `atx-impl/db` to point-in-time parity with FactSet / S&P GMI Compustat for **US equity
> fundamentals**: every metric, ratio, and derived ratio, linked through a strong schema, driven by
> real job management **and production-level database and schema management**.

pf1 proved the **spine and engine**. pf2 delivers the **depth and the production platform** — the two
things a real provider has that a raw-XBRL extraction still lacks. It is a **two-track** module:

- **Track A — Content depth.** The provider capabilities the spine does not yet have: a
  *standardization engine* (the #1 Compustat differentiator), PIT as-of-month snapshots +
  as-first-reported vintages, full industry-specialized statement templates, segment data, footnote
  sub-ledgers, fiscal-calendar normalization, preliminary/press-release capture, populated valuation
  multiples, and cross-vendor reconciliation.
- **Track B — Production platform.** The operational surface pf1 never owned: schema-as-contract with
  drift detection, migration governance with backup/checkpoint/DR, quality-checks-as-SLO-gates wired
  into the orchestrator, data observability (freshness SLA, anomaly detection, size monitoring), and
  storage management (vacuum/compaction, partitioned lake, reproducible rebuild).

---

## The facts that define the work (measured 2026-07-03 against the live warehouse)

Fundamentals are at Compustat *breadth* and pf1 gave them *governed structure* — but not provider
*depth* and not a *production platform*. Concrete walls, all measured:

1. **No standardization engine — the single biggest parity gap.** pf1's `fundamental_item` dictionary
   maps concepts, but there is no engine that *normalizes* each filer's idiosyncratic us-gaap tags +
   custom extensions into one fixed, cross-company/cross-time template with governed sign / scale /
   combination / missing-value rules. Compustat's standardization (~300 annual / ~100 quarterly
   comparable line items, analyst-mapped) is what makes cross-company fundamentals *comparable*; raw
   XBRL extraction never achieves it. → **PF2-S3.**
2. **No PIT snapshot / as-first-reported vintage.** pf1-S8 has restatement lineage on *facts*, but no
   Compustat-style monthly as-of reconstruction and no `as_first_reported` (unrestated) vs
   `most_recently_restated` split. The **ratio** surface still stores only the latest-revision input
   vintage (`fundamental_ratios` v1: "restatement-vintage ratio history is the planned refinement").
   Point-in-time snapshots are what defeat look-ahead/survivorship bias in backtests. → **PF2-S4.**
3. **Industry templates stop at overlays.** pf1-S3 built bank/insurance/REIT *overlay* map rows
   (37 item_ids), but no full normalized templates. Vendors run distinct statement schemas per
   industry (FactSet 4-profile: Commercial / Bank / Insurance / Other-Financial; Compustat INDL vs FS
   split). **Utility (rate base) and broker-dealer templates are entirely absent**; REIT FFO/AFFO is a
   footnote/press-release construct not in the primary us-gaap statements. → **PF2-S5.**
4. **No fiscal-calendar normalization; quarterly-TTM blocked.** Companies have different fiscal-year
   ends, so you cannot sum or compare them without FYE→calendar mapping (Compustat FYR rule),
   period-length / 52-53-week flags, and calendar-aligned LTM/TTM. pf1's TTM stitch is blocked on the
   current cache (no ~9-mo YTD duration bucket). → **PF2-S6.**
5. **Segment + footnote detail deliberately excluded.** pf1's offline extractor takes only
   *consolidated* (zero-dimension-member) facts — a fact is entity-level "iff its `filing_context_id`
   has zero rows in `xbrl_filing_dimensions`". The **~14k dimensional facts are thrown away**: no
   segment data (business/geographic/product/customer) and no footnote sub-ledgers (pension/OPEB,
   leases, deferred-tax, SBC), all of which vendors normalize. → **PF2-S7.**
6. **No preliminary / press-release capture.** Earnings hit the tape via 8-K Item 2.02 EX-99
   press releases days-to-weeks before the 10-K/Q, often not XBRL-tagged. pf1 has the four-date model
   (`rdq`) but no actual preliminary-fact capture, no preliminary→final overwrite, and no GAAP-vs-street
   EPS basis tag (a ~5-8% silent surprise error when mixed). → **PF2-S8.**
7. **Valuation multiples emit ~0 rows; no cross-vendor reconciliation; DQC partial.** pf1-S6's
   `valuation_multiples` is a scaffold: broad price is 2012–2015, companyfacts fundamentals are
   2017–2026, so the PIT price×fundamental join yields **~0 live rows**. There is no `fact_disagreement`
   cross-vendor reconciliation, and pf1-S7 ships only a DQC *subset* (the XBRL-US DQC library is ~196
   approved rules). → **PF2-S9.**
8. **The platform is thin.** The ~288 quality checks are **ungated — run only from tests, zero calls
   from `orchestrator.py`/`jobs.py`**; a `failed` check is just a row. Schema truth is **split** across
   `schema.py` + `migrations.py` with an **unenforced `table_catalog`** and an **unused migration
   `checksum`**. There is **no backup/checkpoint/vacuum/compaction/drift/anomaly/freshness-SLA/size
   code** — the 14.1 GB single-file DB's two 2026-06-29 WAL-replay crashes (S5g/S5k) were recovered by
   hand, leaving unmanaged multi-GB `.bak` artifacts. → **PF2-S1, PF2-S2, PF2-S10.**

---

## The ten sprints

| Sprint | Track | Theme | Goal metric | Doc |
|---|---|---|---|---|
| **PF2-S1** | Platform | **Schema-as-contract + drift + catalog enforcement** — one declarative schema manifest reconciling `schema.py`+`migrations.py`; live-vs-contract drift detector (wires the dead `checksum`); enforce every table catalogued + carries PIT columns; queryable warehouse data-catalog | 0 undeclared tables/columns; drift check green; every table has a `table_catalog` row + PIT-column assertion; catalog reader returns table/field/formula lineage as-of | [sprint-1-schema-contract.md](sprint-1-schema-contract.md) |
| **PF2-S2** | Platform | **Migration governance + backup/checkpoint/DR** — checksum-enforced append-only invariant, advisory apply-lock, scripted pre-flight CHECKPOINT+backup+verify+restore, codified WAL runbook, `.bak` retention | editing a landed migration fails a check; live apply auto-backs-up + verifies; restore reproduces a known state; migration tests derived from `MIGRATIONS` (not hand-list) | [sprint-2-migration-governance.md](sprint-2-migration-governance.md) |
| **PF2-S3** | Content | **Standardization engine (flagship)** — `fundamental_standardized` fact over the item dictionary with governed map/combine/sign/scale/missing rules (definition-as-data); custom-extension routing + exception report | standardized template of ~300 annual/~100 qtr items; every filer tag routes to a canonical item or the exception report; rebuild deterministic + PIT-safe | [sprint-3-standardization-engine.md](sprint-3-standardization-engine.md) |
| **PF2-S4** | Content | **PIT snapshot + as-first-reported vintage** — Compustat-style monthly as-of reconstruction; `as_first_reported`/unrestated vs most-recently-restated; extend pf1-S8 lineage into ratio-vintage history; as-of-month reader | as-of-month reader returns the DB as it stood that month; first-reported ≠ restated captured; ratio vintages queryable | [sprint-4-pit-snapshot-vintage.md](sprint-4-pit-snapshot-vintage.md) |
| **PF2-S5** | Content | **Industry-specialized templates** — full normalized bank/insurance/**utility**/**broker-dealer** templates + REIT **FFO/AFFO** (INDL vs FS split); industry ratio families | each industry profile has its own normalized statement + ratio family; a bank/insurer/utility/REIT emits template-correct ratios (NIM, combined ratio, FFO-payout, …) | [sprint-5-industry-templates.md](sprint-5-industry-templates.md) |
| **PF2-S6** | Content | **Calendarization + fiscal normalization + quarterly-TTM** — FYE→calendar (FYR rule), period-length/52-53-week flags, calendar-aligned LTM/TTM; unblock quarterly-TTM stitching | non-Dec-FYE issuers map to the correct calendar period; 53-week quarters flagged; calendar-aligned TTM emits; quarterly stitch works on the proof slice | [sprint-6-calendarization-ttm.md](sprint-6-calendarization-ttm.md) |
| **PF2-S7** | Content | **Segment data + footnote sub-ledgers** — business/geographic/product/customer segments; pension/OPEB, leases, deferred-tax, SBC normalized sub-ledgers from the excluded `xbrl_filing_dimensions`; reconciliation-to-consolidated guards | segment + footnote surfaces populated on the proof slice; segment totals reconcile to consolidated within tolerance; guards flag mismatches | [sprint-7-segments-footnotes.md](sprint-7-segments-footnotes.md) |
| **PF2-S8** | Content | **Press-release / preliminary→final + estimate-basis** — 8-K Item 2.02 EX-99 ingestion (non-XBRL), preliminary→final overwrite with vintage retention, RDQ flash timestamp; GAAP-vs-street EPS basis tag wired to pf1 est_actual/surprise | preliminary earnings captured before the 10-K/Q with `available_at`=release; final overwrite retains the preliminary vintage; every EPS carries a basis tag | [sprint-8-preliminary-press-release.md](sprint-8-preliminary-press-release.md) |
| **PF2-S9** | Content+ | **Populated valuation multiples + cross-vendor recon + DQC-196** — load ~1yr recent price×fundamental overlap so multiples emit; `fact_disagreement` (XBRL vs injectable Sharadar/SimFin); extend DQC toward ~196 rules | P/E, P/B, P/S, EV/EBITDA emit real rows over the overlap; `fact_disagreement` >99% agreement tracked; DQC subset materially wider with documented skips | [sprint-9-valuation-reconciliation-dqc.md](sprint-9-valuation-reconciliation-dqc.md) |
| **PF2-S10** | Platform | **Quality-as-SLO gating + observability + storage + reproducible rebuild (capstone)** — wire the 288 checks into the orchestrator as gates (severity, thresholds-as-data, halt-on-critical, anomaly + freshness-SLA on `data_quality_checks`); VACUUM/compaction/size-monitoring; partitioned/incremental lake; deterministic full-rebuild + DR runbook; parity-ledger flip | orchestrator halts on a critical check; freshness SLA + row-count anomaly surface; DB size tracked + compacted; fresh-agent rebuild is deterministic; whole-branch review | [sprint-10-quality-slo-observability.md](sprint-10-quality-slo-observability.md) |

---

## Primary-module ownership + shared append-only hubs

Like pf1, pf2 treats the shared hubs (`schema.py`, `migrations.py`, `jobs.py`, `quality.py`,
`watermarks.py`, `lake.py`, `asof.py`, `parity.py`, `orchestrator.py`, plus `PARITY_GAP.md` /
`WAREHOUSE_PARITY_TRANCHES.md`) as **append-only coordination surfaces**: each sprint appends new
tables / rules / checks / readers under a **reserved migration range** and never edits a prior migration
or another sprint's region.

| Sprint | Primary modules (substantially owns / creates) | Reserved migrations |
|---|---|---|
| PF2-S1 | NEW `db/schema_contract.py`, catalog-enforcement in `db/quality.py`, catalog reader in `db/asof.py`; `db/tests/test_schema_contract.py` | `0097–0099` |
| PF2-S2 | NEW `db/migration_admin.py` (backup/checkpoint/restore + `.bak` retention), `migrations.py` runner hardening (checksum + advisory lock), NEW `scripts/warehouse_migrate.py`; `db/tests/test_migration_governance.py` | `0100–0102` |
| PF2-S3 | NEW `db/standardization.py`, NEW `db/seeds/standardization_rules.csv`, `fundamental_standardized` table, `fundamental_ratios.py` reads standardized inputs; `db/tests/test_standardization.py` | `0103–0106` |
| PF2-S4 | NEW `db/pit_snapshot.py`, `db/fundamental_ratios.py` vintage extension, `db/asof.py` as-of-month reader; `db/tests/test_pit_snapshot.py` | `0107–0109` |
| PF2-S5 | NEW `db/industry_templates.py`, `db/fundamental_statements.py` statement-map region, `formula_registry` industry families; `db/tests/test_industry_templates.py` | `0110–0113` |
| PF2-S6 | NEW `db/calendarization.py`, `db/fundamental_statements.py` TTM region; `db/tests/test_calendarization.py` | `0114–0116` |
| PF2-S7 | NEW `db/segments.py`, NEW `db/footnotes.py`, `segment_*`/`footnote_*` tables; `db/tests/test_segments.py`, `db/tests/test_footnotes.py` | `0117–0120` |
| PF2-S8 | NEW `db/press_release.py` (8-K 2.02 EX-99), `db/estimates.py` basis tag; `db/tests/test_press_release.py` | `0121–0123` |
| PF2-S9 | `db/valuation_multiples.py` (populate), NEW `db/fact_disagreement.py`, `db/xbrl_validation.py` DQC extension; `db/tests/test_valuation_multiples.py`, `db/tests/test_fact_disagreement.py` | `0124–0127` |
| PF2-S10 | `db/quality.py` gating, NEW `db/observability.py`, NEW `db/storage_admin.py`, `db/lake.py` partitioning, `orchestrator.py` gate wiring; `db/tests/test_quality_gating.py`, `db/tests/test_observability.py` | `0128–0131` |

**Overlap note.** PF2-S3/S4/S9 touch `fundamental_ratios.py` and PF2-S5/S6 touch
`fundamental_statements.py`, each in different regions — run those **sequentially within a module**,
never concurrently in the same tree. Sprints owning disjoint NEW modules MAY run concurrently in
isolated worktrees (see Sequencing).

---

## Shared PIT / determinism + production contract (every sprint)

Every sprint's tasks state which clauses apply. pf1 clauses **(A)–(D)** carry forward unchanged; pf2
adds **(E)–(G)**.

**(A) Bitemporal correctness.** Every fact/derived row carries `as_of_date`, `available_at`,
`source_loaded_at`, `run_id`, `is_latest_revision`. A derived value sets
`available_at = max(input.available_at)`. As-of readers gate on `valid_from ≤ as_of_date <
coalesce(valid_to, '9999-12-31')` **and** `available_at ≤ as_of_ts`. No lookahead.

**(B) Append-only, catalogued migrations.** Forward-only, idempotent (`CREATE … IF NOT EXISTS` /
`ADD COLUMN IF NOT EXISTS`). Each sprint uses only its reserved range; never renumber or edit a landed
migration. Every new table/view seeds `table_catalog` + `field_catalog` in the same migration. Split
schema from index across migration numbers (the S5g/S5k WAL precedent) and preserve a timestamped
DB+WAL backup before any live apply.

**(C) Offline / no-network tests.** Every test runs against in-memory / template-copy DuckDB with
fixture or injected data. No SEC / FRED / FINRA / OpenFIGI / GLEIF / vendor network calls in pytest.
Live connectors stay behind injectable file options; live smoke is operator-run and recorded in the
ledger.

**(D) Determinism + provenance.** `compute_*` transforms are pure (pandas in → long DataFrame out),
unit-tested independent of DuckDB. Every derived row records `input_codes_json` and source lineage.
Same inputs + same run params → same rows.

**(E) Schema-as-contract *(new — PF2-S1).*** No table lands without a contract row (columns / types /
nullability / natural key / required PIT columns) **and** a `table_catalog` entry. A drift check fails
if the live schema diverges from its contract or if any `duckdb_tables()` table is uncatalogued.

**(F) Backup-before-migrate *(new — PF2-S2).*** Every live migration apply is preceded by a scripted
CHECKPOINT + timestamped backup and followed by a verify; the WAL-split discipline and a documented
recovery runbook are the standing invariant, not tribal knowledge.

**(G) Quality-gated *(new — PF2-S10, adopted incrementally).*** A check authored with `severity=critical`
is wired into the orchestrator and *halts* the affected run — quality results are consumed, not just
recorded. Each sprint that adds a load-bearing invariant registers it as a gated check.

**Data posture (pf2-specific).** Content sprints ship an injectable loader + engine + offline fixtures,
then an operator-run **~1-year recent proof slice** with live counts recorded in the ledger. No large
historical backfills in this module — prove the pipeline on a recent slice; deep backfill is a later
operator concern.

**Process (all sprints):** each sprint runs in its **own git worktree** off the integration mainline
(`atx-impl/scripts/new_db_worktree.sh new <slug>`), merged back at sprint end (`… finish <slug>`) — the
schema churn never touches the primary tree and the git-ignored 14 GB DB is never copied. Never
`git add -A` (stage explicit paths); never push unless asked; commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. New module ⇒ new `test_*.py`.
`python -m pytest atx-impl\db\tests -q` green in the worktree before commit; operator live-DB smoke runs
against the shared DB in the primary tree (backed up first, clause F). Update `PARITY_GAP.md` status and
append a `WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, domains, verification commands, live-DB smoke
with exact counts + run_id, caveats/next).

---

## Sequencing

1. **Platform-foundation wave — PF2-S1 → PF2-S2 (sequential).** Land the schema contract + drift
   detector and the migration/backup governance *first*, so every content sprint churns the schema on
   governed rails with real backups (directly answering the WAL-crash history). S2 depends on S1's
   contract to verify post-migration schema.
2. **Standardization core — PF2-S3 → PF2-S4 (sequential).** S3 (standardization engine) is the
   flagship and everything downstream reads the standardized template; S4 (snapshot/vintage) vintages
   what S3 standardizes. Both touch `fundamental_ratios.py` → serial.
3. **Depth wave — PF2-S5 → PF2-S6 (sequential, both touch `fundamental_statements.py`) ‖ PF2-S7 ‖
   PF2-S8.** S7 (segments/footnotes: NEW `segments.py`/`footnotes.py`) and S8 (NEW `press_release.py`
   + `estimates.py`) own disjoint modules and MAY run concurrently with the S5→S6 chain in isolated
   worktrees. All depend on S3's standardized surface.
4. **Populate + reconcile — PF2-S9.** Depends on S3 (standardized) + S6 (calendarized) + pf1-S6's
   price loader; touches `fundamental_ratios.py` → after S3/S4.
5. **Production capstone — PF2-S10 (last).** Gates and monitors every surface pf2 built; needs them all
   present. Closes with the parity-ledger flip and a whole-branch review.

**If you can only do a subset:** PF2-S3 (standardization — the biggest visible parity lever) → PF2-S1
+ PF2-S2 (the production rails that protect everything) → PF2-S9 (populated valuation multiples, the
most-cited missing ratios) → PF2-S4 (vintages) → PF2-S10 (make it trustworthy in production). S3 alone
converts the warehouse from "as-reported extraction" to "comparable standardized financials"; S3+S9
closes the most visible depth gaps vs Compustat/FactSet.

---

## North star (pf2 acceptance)

A fresh agent can, **fully offline** (with an operator-run ~1yr recent proof slice for the
data-dependent surfaces), rebuild the fundamentals warehouse through the pf1 orchestrator on a
**governed, backed-up, drift-checked** platform and get, PIT-safely:

- every filer's fundamentals **standardized** into one comparable cross-company/cross-time template;
- a **point-in-time as-of-month snapshot** with as-first-reported (unrestated) vs restated vintages,
  extended into **ratio-vintage** history;
- full **industry-specialized** statement templates + ratio families (bank / insurance / utility /
  broker-dealer / REIT-FFO), **calendarized** across fiscal-year-end differences (incl. 52-53-week);
- **segment** and **footnote** sub-ledgers from the previously-discarded dimensional facts;
- **preliminary/press-release** earnings captured before the 10-K/Q with basis-tagged EPS;
- **populated valuation multiples** over the price×fundamental overlap and a **cross-vendor
  reconciliation** with a >99% agreement gate; and
- a **production platform** — schema-as-contract with drift detection, migration governance with
  automated backup/checkpoint/DR, quality checks **gated** in the orchestrator with freshness-SLA +
  anomaly observability, and managed storage (vacuum/compaction, partitioned lake) — such that the
  whole warehouse is reproducibly rebuildable and operationally trustworthy.

Parity gap vs FactSet / S&P GMI Compustat **fundamentals** is then closed on the axes pf2 owns:
standardization, point-in-time vintages, industry depth, calendarization, segment/footnote detail,
preliminary capture, populated multiples, cross-vendor reconciliation — on a production-grade database
and schema-management platform. Out of scope for pf2 (still parked): ESG, supply-chain, licensed
estimate feeds, international (IFRS/ESEF), and the non-fundamental derived domains already built S3–S45.
