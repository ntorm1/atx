# pf1 — Fundamentals Parity (FactSet / S&P GMI Compustat)

**Created:** 2026-07-01. Scoped against the warehouse north star
([../../WAREHOUSE_PARITY_NEXT_AGENT_README.md](../../WAREHOUSE_PARITY_NEXT_AGENT_README.md)):
a production-quality, point-in-time warehouse that rivals FactSet and S&P GMI Compustat for US
equity fundamentals.

**Predecessor:** the S3–S45 tranche series ([../../WAREHOUSE_PARITY_TRANCHES.md](../../WAREHOUSE_PARITY_TRANCHES.md))
built breadth across ten domains (fundamentals, 13F, insider, corp-actions, pricing, short-interest,
macro, reference, estimates schema, off-exchange). pf1 is the **re-focus**: it drives the fundamentals
spine to true provider parity — every metric, every ratio, every derived ratio, linked through a
strong schema, driven by real background job management — and parks the already-built adjacent domains
in maintenance. It is grounded in a full code + research review (2026-07-01) of the warehouse
(`atx-impl/db`, 122 tables, migrations through `0060`, ~465 offline tests) and the vendor-parity
research corpus (`archive/research`: cross-vendor field map ~480 canonical rows, Compustat/FactSet/IBES
schemas, CRSP corp-action canon, four-date PIT semantics).

**Source:** this session's three-scout review (warehouse code, research corpus, tranche ledger) +
the design spec [docs/superpowers/specs/2026-07-01-fundamentals-parity-design.md](../../../docs/superpowers/specs/2026-07-01-fundamentals-parity-design.md).

---

## The facts that define the work (measured this session)

The fundamentals spine is real and at Compustat *breadth* — but not Compustat *depth or trust*. Four
concrete walls block true parity:

1. **Metrics are ad-hoc strings, not a governed dictionary.** ~200 XBRL concepts are extracted and
   the ratio engine references canonical inputs as **hard-coded strings** (`fundamental_ratios.py`
   TTM_INPUTS/BALANCE_INPUTS). There is no `fundamental_item` dimension mirroring Compustat's ~300
   annual / ~100 quarterly items or FactSet's 750+, and no vendor cross-walk. Rename a concept and the
   ratio engine breaks silently. The ~480-row cross-vendor field map
   (`archive/research/schemas/cross_vendor_field_map.md`) is the data dictionary and is not yet in the
   schema. → **PF-S1, PF-S3.**
2. **The derived layer is code, not a library — and misses whole families.** 53 ratio codes exist as
   Python lambdas in `RATIO_DEFS`; the four distress scores are monolithic lambdas with no citations.
   There is no formula registry, and **no price-based valuation multiples at all** (P/E, P/B, P/S,
   EV/EBITDA, market cap) because `equity_daily_bars` (2012–14) and `sec_company_facts` (2017–26) do
   not overlap. → **PF-S4, PF-S6.**
3. **Facts do not link cleanly, and vintages are lost.** There is no sticky FSYM-style entity/security
   identifier spine (FIGI/LEI/ISIN); `accession_number` exists on `sec_company_facts` but is **not
   propagated** to statement points or ratios, so no ratio can answer "which filing changed this?" and
   the four-date invariant (`pdate ≤ rdq ≤ fdate ≤ ldate`) is not enforced end-to-end. → **PF-S5,
   PF-S8.**
4. **Jobs are a hardcoded sequential loop; XBRL trust is unproven.** `refresh_quant_warehouse`
   (`jobs.py`) has no dependency inference, no watermark-driven incremental load, no retry/resume, no
   run manifests or audit. And XBRL validation is SQL-only linkbase checks with **1,364 standing
   failures** deferred as "as-reported quirks" — not dimension-aware, so real errors can hide. →
   **PF-S2, PF-S7.**

---

## The eight sprints

| Sprint | Theme | Goal metric | Doc |
|---|---|---|---|
| **PF-S1** | **Canonical item dimension + metric registry** — normalize the ~480-row cross-vendor field map into `fundamental_item` (+ alias + vendor map); every fact/ratio resolves inputs through the registry, not hard-coded strings | item dim seeded from field map; ratio engine reads registry; 0 hard-coded metric strings in derived layer | [sprint-1-canonical-item-dimension.md](sprint-1-canonical-item-dimension.md) |
| **PF-S2** | **In-repo job orchestrator** — pure-Python DAG: dependency inference, watermark-driven incremental, retry/backoff, resume-from-checkpoint, run manifests + `etl_job_audit` | interrupted rebuild resumes; incremental run touches only stale datasets; every run recorded | [sprint-2-job-orchestrator.md](sprint-2-job-orchestrator.md) |
| **PF-S3** | **XBRL concept coverage → full canonical set** — widen ~200 → full dictionary + bank/insurance/REIT overlays; lift the 5-security XBRL-extra ceiling; reconcile statement-map gaps | concept coverage ≥ canonical target; xbrl_metric universe ≥ ratio universe; statement-map allowlist reconciled | [sprint-3-concept-coverage.md](sprint-3-concept-coverage.md) |
| **PF-S4** | **Formula library + full ratio/derived expansion** — ratios+scores become declarative `formula_registry` rows (definition-as-data + citations); add DuPont, coverage, accruals, cash-conversion-cycle, per-share suite, more quality/distress scores | formula count materially up; every formula has a registry row + citation + unit test; engine reads registry | [sprint-4-formula-library.md](sprint-4-formula-library.md) |
| **PF-S5** | **Identifier spine (FIGI/LEI/ISIN)** — sticky FSYM-style entity/security IDs, offline OpenFIGI/GLEIF injectable, bitemporal alias, CUSIP internal-only | fundamentals link to stable security_id/entity_id + FIGI; as-of identifier readers PIT-correct | [sprint-5-identifier-spine.md](sprint-5-identifier-spine.md) |
| **PF-S6** | **Modern pricing overlap + valuation multiples** — ingest 2015+ daily bars to intersect fundamentals; market cap, P/E, P/B, P/S, EV/EBITDA, EV/Sales, FCF/earnings/dividend yield, PIT-safe | valuation multiples emitted over overlap window; price `available_at` vs fundamental `available_at` enforced | [sprint-6-pricing-valuation-multiples.md](sprint-6-pricing-valuation-multiples.md) |
| **PF-S7** | **XBRL validation + DQC hardening** — dimension-aware calculation-linkbase validation resolving the 1,364 standing fails properly; cross-table referential checks; DQC rule subset | standing failures triaged real-vs-artifact with dimension awareness; new referential checks green; no tolerance hacks | [sprint-7-xbrl-validation.md](sprint-7-xbrl-validation.md) |
| **PF-S8** | **Restatement vintage lineage + as-of parity** — propagate accession/filing_date through statement_points→ratios; four-date audit chain; "which filing changed this ratio"; quarterly-TTM stitching | ratio restatement traceable to accession; `pdate≤rdq≤fdate≤ldate` enforced; as-of returns filing-correct vintage | [sprint-8-restatement-lineage.md](sprint-8-restatement-lineage.md) |

---

## Primary-module ownership + shared append-only hubs

Unlike the C++ `p6` plan, this warehouse has **shared hub modules every tranche must touch** —
`schema.py`, `migrations.py`, `jobs.py`, `quality.py`, `watermarks.py`, `lake.py`, `asof.py`,
`parity.py`, plus the `PARITY_GAP.md` / `WAREHOUSE_PARITY_TRANCHES.md` docs. Rather than reserve them
for a final sprint, pf1 treats them as **append-only coordination surfaces**: each sprint appends new
tables / registry entries / checks / readers under a **reserved migration-number range** and never
edits a prior migration or another sprint's region.

| Sprint | Primary modules (substantially owns / creates) | Reserved migrations |
|---|---|---|
| PF-S1 | NEW `db/item_registry.py`, NEW `db/seeds/fundamental_items.csv`, resolution shim imported by `fundamentals.py`/`fundamental_statements.py`/`fundamental_ratios.py`; `db/tests/test_item_registry.py` | `0061–0064` |
| PF-S2 | NEW `db/orchestrator.py`, `jobs.py` `refresh_quant_warehouse` rewrite, `etl_job_*` tables; `db/tests/test_orchestrator.py` | `0065–0068` |
| PF-S3 | `db/fundamental_statements.py` (concept catalog + statement map), `db/fundamentals.py` `DEFAULT_CONCEPTS`, NEW `db/seeds/concept_map.csv`; `db/tests/test_concept_coverage.py` | `0069–0074` |
| PF-S4 | NEW `db/formula_library.py`, `db/fundamental_ratios.py` refactor to consume it, `formula_registry` table; `db/tests/test_formula_library.py` | `0075–0078` |
| PF-S5 | NEW `db/identifiers_figi.py`, NEW `db/identifiers_lei.py`, `db/security_master.py` resolution, `identifier_*` extensions; `db/tests/test_identifier_spine.py` | `0079–0083` |
| PF-S6 | NEW `db/pricing_bulk.py` (2015+ injectable bars), `db/ticker_history.py` extension, NEW `db/valuation_multiples.py`; `db/tests/test_valuation_multiples.py` | `0084–0087` |
| PF-S7 | `db/xbrl_validation.py` (dimension-aware), `db/quality.py` multi-table check type; `db/tests/test_referential_quality.py` | `0088–0091` |
| PF-S8 | `db/fundamental_statements.py` + `db/fundamental_ratios.py` lineage columns, `db/asof.py` vintage readers, TTM stitching; `db/tests/test_restatement_lineage.py` | `0092–0096` |

**Overlap note.** PF-S4, PF-S6, PF-S8 all touch `fundamental_ratios.py` in different regions — run
them **sequentially** (S4 → S6 → S8), never concurrently in the same tree. PF-S1‖PF-S2 and PF-S3‖PF-S5
and PF-S4‖PF-S7 own disjoint primary modules and MAY run concurrently in isolated worktrees.

---

## Shared PIT / determinism contract (every sprint)

Every sprint's tasks state which clauses apply.

**(A) Bitemporal correctness.** Every fact and derived row carries `as_of_date` (business/period time),
`available_at` (knowledge time), `source_loaded_at`, `run_id`, and `is_latest_revision`. A derived value
sets `available_at = max(input.available_at)`. As-of readers enforce
`valid_from ≤ as_of_date < coalesce(valid_to, DATE '9999-12-31')` **and** `available_at ≤ as_of_ts`. No
lookahead: a value is never visible before every input's `available_at`. This is the warehouse's system
of record and is non-negotiable — the analogue of p6's AuditExact.

**(B) Append-only, catalogued migrations.** Migrations are forward-only and idempotent
(`CREATE TABLE IF NOT EXISTS` / `ADD COLUMN IF NOT EXISTS`). Each sprint uses only its reserved range
(above); never renumber or edit a landed migration. Every new table/view seeds `table_catalog` +
`field_catalog` in the same migration (views are catalogued like tables — see S7a). A WAL-replay risk on
live migration is mitigated by splitting schema and index into two migration numbers (see S5g/S5k
precedent) and preserving a timestamped DB+WAL backup before the live apply.

**(C) Offline / no-network tests.** Every test runs against in-memory DuckDB with fixture or injected
data. No SEC / FRED / FINRA / OpenFIGI / GLEIF network calls in the test path. Live connectors stay
behind injectable file options (`--*-file`, `--*-zip`); live smoke checks are operator-run and recorded
in the ledger, never in pytest. Keep the suite light and fast.

**(D) Determinism + provenance.** `compute_*` transforms are pure (pandas in → long DataFrame out) and
unit-tested independent of DuckDB. Every derived row records `input_codes_json` and, from PF-S8,
`source_accession` / `filed_date`. Same inputs + same run params → same rows.

**Process (all sprints):** never `git add -A` (the tree carries many unrelated dirty/untracked files —
stage explicit paths); never push unless asked; commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. New module ⇒ new `test_*.py`
under `atx-impl/db/tests`. `python -m pytest atx-impl\db\tests -q` green before commit. Update
`PARITY_GAP.md` status and append a row to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains,
verification commands, live-DB smoke with exact counts + run_id, caveats/next).

---

## Sequencing

1. **Foundation wave — PF-S1 ‖ PF-S2.** Disjoint primary modules (`item_registry.py` vs
   `orchestrator.py`/`jobs.py`), disjoint migration ranges. Land both first; everything downstream links
   through S1's item dimension and rebuilds through S2's orchestrator.
2. **Coverage + linkage wave — PF-S3 ‖ PF-S5.** S3 (concepts) depends on S1's item dim and consumes S2's
   incremental loads; S5 (identifiers) depends on S1. Disjoint modules → may run concurrently.
3. **Derivation + trust wave — PF-S4 ‖ PF-S7.** S4 (formula library) depends on S1+S3; S7 (validation)
   depends on S3. Disjoint modules → may run concurrently.
4. **Valuation — PF-S6.** Depends on S4 (formula registry) + S5 (identifier join). Touches
   `fundamental_ratios.py` — after S4.
5. **Vintage — PF-S8 (last).** Depends on S1+S3+S4; coordinates the `fundamental_ratios.py` region with
   S6. Closes Compustat-Snapshot parity.

**If you can only do a subset:** PF-S1 (the dictionary — everything links to it) → PF-S4 (the derived
library, the visible parity surface) → PF-S6 (valuation multiples, the most-cited missing ratios) →
PF-S2 (make the rebuild trustworthy) → PF-S8 (vintages) → PF-S3/S5/S7 (depth + trust). PF-S1 alone
converts the warehouse from "ad-hoc strings" to a governed dictionary; PF-S1+S4+S6 closes the most
visible fundamentals-parity gaps vs Compustat/FactSet.

---

## North star (pf1 acceptance)

A fresh agent can, **fully offline**, rebuild the fundamentals spine through the PF-S2 orchestrator and
get, PIT-safely:

- every canonical Compustat / FactSet US-equity fundamental item present as a normalized
  `fundamental_item` row with the vendor cross-walk (us-gaap / Compustat / FactSet / IBES);
- every statement fact and every ratio linked to `item_id` + `security_id` + source accession vintage;
- the **full** ratio / derived-ratio set — statement ratios *and* price-based valuation multiples —
  computed over the price×fundamental overlap, each with numerator/denominator + `available_at` lineage;
- XBRL validation that is dimension-aware with **zero unexplained** standing failures; and
- incremental, resumable job runs recorded with manifests + an audit trail.

Parity gap vs FactSet / S&P GMI Compustat **fundamentals** is then closed on the six axes pf1 owns:
item coverage, ratio breadth, valuation multiples, identifier linkage, restatement vintage, and job
orchestration. Out of scope for pf1 (parked): ESG, supply-chain, licensed estimate feeds, and the
non-fundamental derived domains (13F/insider/macro/short-interest analytics) already built S3–S45.
