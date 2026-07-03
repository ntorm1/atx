# Sprint PF-S5 — Identifier Spine (FIGI/LEI/ISIN)

**Goal:** sticky FSYM-style entity/security IDs with offline FIGI/LEI/ISIN resolution; bitemporal alias; CUSIP internal-only; fundamentals link to stable IDs. Reserved migrations 0079-0083.

**Mandate / Owns:** NEW `db/identifiers_figi.py`, NEW `db/identifiers_lei.py`, `db/security_master.py` resolution, `identifier_*` table extensions, `db/tests/test_identifier_spine.py`.

**Must NOT touch:** the ratio/formula engine, `item_registry` (PF-S1 — depend on it).

**Depends on:** PF-S1.

---

## Baseline / where the cycles go

The warehouse already keys everything off a `security_id` minted from CIK (`cik_security_id` in
`warehouse.py`, called from `normalize_company_tickers` in `security_master.py`). That is the whole
identity model: one CIK, one security. It has four concrete gaps.

| Wall | Where it lives today | What is missing |
|---|---|---|
| **No sticky entity/security separation.** `security_id = CIK-derived`, `issuer_id = "CIK-" + cik`. | `security_master.py:51`, `security_master.py:187` | No `entity_id` that survives an M&A/ticker change and no `security_id` per share class. A CIK reassignment or a class-A/class-B split has nowhere to land. This is the FSYM entity↔security model the vendor corpus documents. |
| **No FIGI/LEI/ISIN coverage.** `security_identifier_history.id_type` only ever carries `'CIK'` and `'TICKER'` (see the two `pd.DataFrame` blocks in `upsert_security_master_from_frame`). | `security_master.py:201-231` | No FIGI (the redistributable primary), no LEI (the entity anchor), no ISIN. The `id_type` column is already free-text, so the schemes are additive — but nothing populates them. |
| **CUSIP is not partitioned as internal-only.** There is no CUSIP column anywhere in the identifier tables. | `schema.py:307-319` (`security_identifier_history`) | When 13F / corp-action ingestion needs `cusip → security_id`, CUSIP has to live *somewhere*. The 13F research (`archive/research/datasets/13f_holdings.md` §B.3) is explicit: persist CUSIP in a non-redistributable internal column, expose only FIGI/ticker/entity_id downstream. That partition does not exist yet. |
| **`identifier_same_source_self_overlaps=528` standing failure.** Ownership-issuer seeds emit one open-ended row per filing per key. | `security_master.py:116-171` (`dedupe_open_identifier_intervals`, `collapse_identifier_history_open_duplicates`) | The collapse repair exists and is idempotent, but 528 self-overlaps still stand in the live DB — the repair is not wired into every seeder's write path, so the count is a standing quality fail rather than zero. |

**Already good — do not regress.** Two things are right and must survive this sprint intact:

- **`security_identifier_history` is genuinely bitemporal.** It carries `valid_from` / `valid_to`
  (business-time interval) **and** `available_at` (knowledge time), plus `as_of_date`, `source`,
  `run_id`. The as-of reader `security_ids_for_symbols` (`security_master.py:57-110`) already resolves
  a ticker through a priority-ranked `UNION ALL` over `sec_company_tickers` → history → listings, with
  `valid_to IS NULL OR valid_to >= current_date` interval filtering and a deterministic
  `QUALIFY row_number()` tie-break. New schemes plug straight into this shape.
- **`identifier_resolution_candidates` / `identifier_resolution_decisions` are a curated accept/reject
  ledger** (`schema.py:475-528`). Candidates carry `match_method` + `confidence` + `candidate_status`;
  decisions carry `decision_status` / `decision_method` / `decided_by` / `decided_at` /
  `effective_from`. The catalog note already reads *"Candidates are evidence, not automatic merges; use
  confidence/status before accepting."* This is the machinery a CUSIP→FIGI collapse must route through.
  Do not bypass it with a blind `INSERT`.

---

## PIT / determinism contract

ROADMAP clauses **(A)** bitemporal correctness, **(B)** append-only catalogued migrations,
**(C)** offline / no-network tests all apply to every task below.

- **(A)** Every new alias row (FIGI/LEI/ISIN/CUSIP) carries `valid_from` / `valid_to` /
  `available_at` / `as_of_date` / `source` / `run_id`, exactly like the existing CIK/TICKER rows.
  As-of readers enforce `valid_from ≤ as_of_date < coalesce(valid_to, DATE '9999-12-31')` **and**
  `available_at ≤ as_of_ts`. A resolved `security_id`/`entity_id` on a fundamental fact is never
  visible before its alias evidence's `available_at`.
- **(B)** Migrations 0079-0083 only; forward-only, idempotent (`ADD COLUMN IF NOT EXISTS`,
  `CREATE TABLE IF NOT EXISTS`). Schema and index are split across two numbers (0079 schema,
  0080 index) per the S5g/S5k WAL-replay precedent, and each new table/column seeds `table_catalog` +
  `field_catalog` in the same migration.
- **(C)** The FIGI and LEI loaders read **OPERATOR-SUPPLIED offline snapshots** — an OpenFIGI bulk
  mapping export (FIGI is MIT-licensed, zero-cost, no redistribution restriction —
  `13f_holdings.md` §B.2/§B.3) and a GLEIF Golden Copy file (free). No OpenFIGI / GLEIF network call
  ever enters the pytest path; live connectors stay behind injectable `--figi-file` / `--lei-file`
  options and are operator-run, recorded in the ledger.
- **CUSIP boundary (hard rule).** CUSIP is licensed (CGS/FactSet; active antitrust class action —
  `13f_holdings.md` §B.1). It is used **internally only** for `cusip → figi` / `cusip → security_id`
  matching and never leaves its internal column into any exported/public/lake object.

---

## Tasks

### S5-0 — Entity/security ID model (FSYM-style, sticky)

**Root cause.** Identity is a 1:1 CIK derivation (`cik_security_id`); there is no entity layer above
`security_id` and no per-share-class layer below `securities`. An M&A event, a CIK reassignment, or a
dual-class listing has nowhere to be represented, so identity silently breaks on exactly the events a
provider-grade spine must survive.

**Fix.** Introduce the two-tier FSYM model:
- `entity_id` — the sticky corporate entity that survives M&A / ticker change / CIK reassignment
  (the LEI-anchored level).
- `security_id` — per share class, keyed under an `entity_id` (the FIGI-anchored level).

Add an `entity_id` column to `securities` (nullable, backfilled to the current CIK-entity so existing
rows are non-breaking) and extend `security_identifier_history` to carry the new schemes
`FIGI` / `LEI` / `ISIN` (the `id_type` column is already free-text, so this is data, not DDL) plus an
internal-only CUSIP column. **Migration 0079** = schema (`ADD COLUMN IF NOT EXISTS entity_id`,
internal CUSIP column, catalog seeds); **migration 0080** = the covering indexes, split off per the
WAL-replay precedent.

**PIT.** New columns are bitemporal-native; the entity backfill sets `available_at` to the migration
run's knowledge time and `valid_from` to the earliest existing sighting (mirroring the
`min(valid_from)` carry-forward already in `upsert_security_master_from_frame`).

**Accept.** `securities.entity_id` present and non-null for every existing row; a fixture M&A event
(two CIKs → one `entity_id`) resolves both securities to the same entity as-of the merger date and to
distinct entities before it; catalog rows exist for every new column.

---

### S5-1 — Offline FIGI loader (`identifiers_figi.py`)

**Root cause.** No `cusip → figi` resolution exists, so the warehouse cannot expose the one
redistributable primary identifier the 13F/corp-action domains need, and cannot join to FIGI-keyed
vendor data.

**Fix.** A new module that reads an **injectable** OpenFIGI mapping export (CSV or JSON, passed via
`--figi-file`; the shape produced by `POST /v3/mapping` or a bulk dump — `13f_holdings.md` §B.3). For
each row: resolve `cusip → figi`, attach `figi` (and `ticker`) to the matched `security_id`, and
persist the source CUSIP in the **internal-only** column added in S5-0. Downstream readers expose only
`figi` / `ticker` / `entity_id` — never CUSIP. Ambiguous or low-confidence `cusip → figi` matches are
written as `identifier_resolution_candidates` rows (with `match_method` + `confidence`) and routed
through `identifier_resolution_decisions`, not merged blindly.

**PIT.** Each FIGI alias row is a bitemporal `security_identifier_history` row
(`id_type='FIGI'`, `valid_from`/`valid_to`/`available_at`/`source='OpenFIGI'`/`run_id`).
`compute_*` transform is pure (mapping frame in → long alias frame out), unit-tested without DuckDB.

**Accept.** Given a fixture OpenFIGI file, securities gain FIGI aliases; CUSIP lands only in the
internal column; a `cusip → figi` conflict produces a `candidate` + `decision` row rather than a merge;
no network call in the test path.

---

### S5-2 — Offline LEI loader (`identifiers_lei.py`)

**Root cause.** There is no entity-level anchor. `entity_id` (S5-0) needs an external, stable,
free identifier to hang on, and the parent/subsidiary structure (who-owns-whom) is absent.

**Fix.** A new module that reads an **injectable** GLEIF Golden-Copy export (CSV, passed via
`--lei-file`; GLEIF is free). Derive `cik ↔ lei` where the Golden Copy exposes a usable US-registration
crosswalk, attach `LEI` aliases to the `entity_id` level, and optionally ingest GLEIF **Level-2**
relationship records (direct/ultimate parent edges) as entity→entity parent links to support the
sticky-entity rollup S5-0 introduced.

**PIT.** LEI alias rows and parent edges are bitemporal (`valid_from`/`valid_to`/`available_at`);
a Level-2 relationship carries its own GLEIF-reported validity window. Pure transform, DuckDB-free
unit test.

**Accept.** Given a fixture GLEIF file, entities gain LEI aliases; a fixture parent relationship
produces an entity→entity edge; `cik ↔ lei` derivation is deterministic and offline.

---

### S5-3 — Link fundamentals to the stable spine

**Root cause.** `sec_company_facts` is keyed by CIK; statement points and ratios inherit that key.
They do not carry a resolved `security_id` / `entity_id`, so a fundamental fact cannot be joined
FIGI-side to pricing/13F, and identity is re-derived ad hoc at every read.

**Fix.** Resolve `sec_company_facts.CIK → security_id / entity_id` through the S5-0 spine and add the
FK so every fundamental fact — and, by inheritance, every ratio — carries `security_id`. Provide
**PIT-correct as-of identifier readers** (extend the existing `security_ids_for_symbols` pattern:
priority-ranked `UNION ALL`, interval filter, `QUALIFY row_number()` tie-break) so a fact filed at
time *T* resolves through the identifier state **as known at T**, honoring `available_at`. Must NOT
touch the ratio/formula engine internals — only add the carried `security_id` column and the reader;
depend on PF-S1's `item_registry` for the item side.

**PIT.** The resolved identifier obeys `available_at ≤ fact.available_at` (no lookahead: a fact
never resolves through an alias the warehouse did not yet know at filing time).

**Accept.** Every `sec_company_facts` row resolves to a `security_id` + `entity_id` (or lands in the
resolution ledger as unresolved); ratios carry the resolved `security_id`; the as-of reader returns the
filing-time-correct identifier for a fixture with a mid-history ticker change.

---

### S5-4 — Quality: CUSIP export scan + resolve the 528 self-overlaps

**Root cause.** Two standing quality risks: CUSIP could leak into an exported object, and
`identifier_same_source_self_overlaps=528` is an unresolved standing failure because the
open-interval collapse (`collapse_identifier_history_open_duplicates`) is not applied on every write
path.

**Fix.**
1. Add an **export-scan quality check** that fails if any lake-exported / public / catalogued-public
   object contains the internal CUSIP column (or a CUSIP-shaped value) — the enforcement of the
   `13f_holdings.md` §B.3 boundary.
2. Wire `dedupe_open_identifier_intervals` into the seeder write path (so new open-ended duplicates
   never land) and run `collapse_identifier_history_open_duplicates` as a one-time repair migration
   step, then either **drive `identifier_same_source_self_overlaps` to 0** or document each residual
   as a genuine closed-interval (a real ticker change is a legitimate non-overlapping interval and
   must not be collapsed — see the docstring's closed-interval carve-out).

**PIT.** The collapse keeps the earliest `(valid_from, available_at, rowid)` row per key (the true
first disclosure), preserving knowledge-time correctness; closed intervals are never touched.

**Accept.** Export-scan check green (zero CUSIP in any exported/public object);
`identifier_same_source_self_overlaps` = 0 or every residual documented with rationale in the ledger.

---

## Sequencing & expected compounding

1. **S5-0 (model) first** — the entity/security tiers and the extended alias schema are the substrate
   everything else writes into. Nothing can attach FIGI/LEI/CUSIP before the columns exist.
2. **S5-1 (FIGI) ‖ S5-2 (LEI)** — independent loaders against disjoint schemes and disjoint injectable
   files; may proceed in parallel once S5-0 lands. FIGI hangs on `security_id`, LEI on `entity_id`.
3. **S5-3 (link)** — needs FIGI/LEI populated so the resolved `security_id`/`entity_id` are meaningful.
4. **S5-4 (quality)** — last; scans the finished surface and closes the standing self-overlap failure.

**Compounding.** A stable `security_id` unblocks **PF-S6** (the price↔fundamental valuation-multiple
join has no key without it) and every future cross-domain linkage (13F holdings, corp actions, insider)
resolves through the same FSYM spine instead of re-deriving identity per domain.

---

## Risks / guardrails

| Risk | Mitigation |
|---|---|
| **CUSIP leaks into an exported / public / lake object.** | S5-4 export-scan quality check fails the build on any CUSIP presence in an exported or catalogued-public object; CUSIP stays in the internal-only column and never enters a downstream reader. |
| **A wrong `cusip → figi` collapse merges two distinct securities.** | Route every non-trivial match through `identifier_resolution_candidates` (`match_method` + `confidence`) → `identifier_resolution_decisions` (curated accept/reject); never auto-merge on a low-confidence hit. |
| **Loaders reach the network in a test.** | Loaders are offline / injectable only (`--figi-file`, `--lei-file`); no OpenFIGI/GLEIF call in the pytest path (ROADMAP (C)); live fetch is operator-run and ledger-recorded. |
| **Collapsing a real ticker change as a "self-overlap."** | The collapse touches only open-ended (`valid_to IS NULL`) rows per the existing docstring carve-out; closed intervals (a genuine ticker change) are preserved. |
| **Entity backfill re-derives identity and breaks existing joins.** | `entity_id` is added nullable and backfilled to the current CIK-entity so every existing `security_id` is non-breaking; the FSYM split is additive. |

---

## Bench / acceptance

- Fundamentals link to a stable `security_id` / `entity_id` + FIGI over the resolvable universe;
  unresolved CIKs land in the resolution ledger, not silently dropped.
- As-of identifier readers enforce the bitemporal contract (`valid_from ≤ as_of_date < coalesce(valid_to,'9999-12-31')` **and** `available_at ≤ as_of_ts`); a mid-history ticker-change fixture resolves to the filing-time-correct identifier.
- CUSIP absent from every exported / public / lake object (S5-4 export scan green).
- FIGI and LEI loaders are offline / injectable only; no network in the test path.
- `identifier_same_source_self_overlaps` = 0, or every residual documented as a legitimate
  closed interval.
- `python -m pytest atx-impl\db\tests\test_identifier_spine.py -q` green; full
  `python -m pytest atx-impl\db\tests -q` green before commit.
- `PARITY_GAP.md` status updated and a ledger row appended to `WAREHOUSE_PARITY_TRANCHES.md`
  (start/end SHA, domains, verification commands, live-DB smoke with exact counts + run_id,
  caveats/next).

**Process.** Migrations 0079-0083 only; never renumber a landed migration. Never `git add -A` — stage
explicit paths (the tree carries unrelated dirty/untracked files). Never push unless asked. Commit
trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
