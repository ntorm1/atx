# Sprint PF-S1 — Canonical Item Dimension + Metric Registry

**Goal:** normalize the ~480-row cross-vendor field map into a governed `fundamental_item`
dimension (+ alias + vendor cross-walk, ~480 items) and route the ratio engine's currently
hard-coded input strings through a pure resolution shim, so 0 hard-coded metric strings remain
in the derived layer — while the `fundamental_ratios` rebuild stays byte-identical.

**Mandate / Owns (exclusive):** NEW `db/item_registry.py`; NEW `db/seeds/fundamental_items.csv`;
the resolution shim imported by `db/fundamentals.py` / `db/fundamental_statements.py` /
`db/fundamental_ratios.py`; NEW `db/tests/test_item_registry.py`. Reserved migrations
**`0061–0064`** (0061 schema+catalog, 0062 indexes, 0063 statement-point FK backfill,
0064 xbrl-point FK backfill). Appends to shared hubs only in its own regions:
`migrations.py` (new `Migration` entries after `version=59`), `table_catalog`/`field_catalog`
seeds for the three new tables.

**Must NOT touch:** `db/orchestrator.py` / `jobs.py::refresh_quant_warehouse` (PF-S2, 0065–0068);
`db/fundamentals.py::DEFAULT_CONCEPTS` + `concept_map.csv` (PF-S3, 0069–0074);
NEW `db/formula_library.py` + `formula_registry` (PF-S4, 0075–0078); identifier modules
(PF-S5); pricing/valuation modules (PF-S6); `db/xbrl_validation.py` (PF-S7); the
`fundamental_ratios.py`/`fundamental_statements.py` *lineage* columns (PF-S8, 0092–0096). Do not
edit or renumber any landed migration `0001–0060`. Do not touch the `RATIO_DEFS` math (formulae are
PF-S4's surface) — this sprint only rewires *inputs*, not operands.

---

## Baseline / where the cycles go

Anchors verified against HEAD on `feat/warehouse-parity` (2026-07-01). Every "already good"
cell was read and confirmed before noting it.

| Sink | File : region | Cost/Gap | Class |
|---|---|---|---|
| **Ratio inputs are hard-coded canonical-metric strings, not a governed dimension** — `TTM_INPUTS` (7 keys), `BALANCE_INPUTS` (4 keys), `XBRL_BALANCE_INPUTS` (9 keys), `XBRL_FLOW_INPUTS` (5 keys) are literal `key -> canonical_metric` dicts with no link to any item table; rename a `canonical_metric` in the statement map and these break silently | `fundamental_ratios.py:51` (`TTM_INPUTS`), `:60` (`BALANCE_INPUTS`), `:489` (`XBRL_BALANCE_INPUTS`), `:503` (`XBRL_FLOW_INPUTS`) | governance gap — no single source of truth for a metric's identity | P1 — resolve through registry |
| **The ~480-row cross-vendor field map is documentation, not schema** — the canonical `item_id` dictionary (§2.1 income ~47, §2.2 balance ~44, §2.3 cashflow ~27, §2.4 derived, §2.5 banks 11, §2.6 insurance 8, §2.7 REIT 8, §3 estimates 2001–2044) lives only in markdown; nothing in the DB enforces "one canonical item, N vendor codes" | `archive/research/schemas/cross_vendor_field_map.md:66` (§2 fundamentals), `:301` (§3 estimates), `:660` (§10 headline dictionary) | no `fundamental_item` dim; no vendor cross-walk | P1 — seed the three tables |
| **`item_id` exists on the statement map but is a bare `INTEGER`, not an FK** — `FundamentalStatementMapRow.item_id` (default `None`) is set on all ~200 seed rows (1001..1440) yet references no table; there is no `fundamental_item` for it to point at | `fundamental_statements.py:28` (`item_id: int \| None`), `:73` (`item_id INTEGER` column), `:155`+ (`FUNDAMENTAL_STATEMENT_MAP_ROWS` seeds with `item_id` 1001…) | dangling reference — the dimension it wants does not exist | P2 — create dim, add FK |
| **Concepts resolve to canonical strings with no ≤1-item guarantee** — the statement map COALESCEs multiple us-gaap/dei concepts onto one `canonical_metric` (e.g. `Revenues`/`SalesRevenueNet`→`revenue`, `EntityCommonStockSharesOutstanding`/`CommonStockSharesOutstanding`→`shares_outstanding`) by `concept_priority`, but nothing asserts a concept maps to at most one item | `fundamental_statements.py:158–160` (revenue 3-way COALESCE), `:241–242` (shares dei→us-gaap) | correctness risk — a mis-alias silently double-counts | C1 — ≤1 item_id quality check |

**What is already good — do not regress:**
- **Bitemporal PIT columns are present and correct on every derived surface.** `RATIO_COLUMNS`
  (`fundamental_ratios.py:67`) carries `as_of_date`, `available_at`, `is_latest_revision`, `run_id`,
  `input_codes_json`; `compute_ratio_rows` sets `available_at = max(input availabilities)`
  (`fundamental_ratios.py:600–603`) and `as_of_date = period_end`. This is clause (A) — keep it verbatim.
- **`compute_ratio_rows` is a pure DataFrame→DataFrame transform** (`fundamental_ratios.py:577`),
  unit-tested independent of DuckDB. The registry must not add DuckDB calls to this path.
- **The statement map is already a real seeded dimension** (`FUNDAMENTAL_STATEMENT_MAP_ROWS`,
  `fundamental_statements.py:155`; seeded by `seed_fundamental_statement_map`, `:471`) keyed by
  `(source, taxonomy, concept, industry_template)` with `concept_priority` COALESCE and per-`item_id`
  labels/units. PF-S1 *lifts* this into a first-class `fundamental_item` dim — it does not rewrite it.
- **`table_catalog` / `field_catalog` seeding pattern is established** (`schema.py:59`, `:74`;
  `INSERT OR REPLACE INTO table_catalog`/`field_catalog` at `migrations.py:1596`, `:1620`, etc.).
  New tables follow it exactly — one catalog row per table, one field row per column.

---

## PIT / determinism contract

This sprint applies clauses **(A)**, **(B)**, and **(D)** of the ROADMAP
[Shared PIT / determinism contract](ROADMAP.md#shared-pit--determinism-contract-every-sprint):

- **(A) Bitemporal correctness.** The item dimension is *reference data* — it carries no `as_of_date`
  fact time, but alias validity is bitemporal via `fundamental_item_alias.valid_from` / `valid_to`
  (e.g. `SalesRevenueNet` valid only pre-2018). The resolution shim, when asked for an as-of alias
  list, must respect `valid_from ≤ as_of_date < coalesce(valid_to, DATE '9999-12-31')`. No derived
  row's `available_at` computation changes — PF-S1 touches inputs' *identity*, never their timestamps.
- **(B) Append-only, catalogued migrations.** Migrations `0061–0064` are forward-only and idempotent
  (`CREATE TABLE IF NOT EXISTS`, `ADD COLUMN IF NOT EXISTS`). Schema+catalog land in `0061`; indexes in
  `0062` (schema/index split per the S5g/S5k WAL-replay precedent, ROADMAP clause B); FK backfills in
  `0063`/`0064`. Each new table seeds `table_catalog` + `field_catalog` in the same migration. Never
  renumber; never edit `0001–0060`.
- **(D) Determinism + provenance.** The seed CSV is committed and hashed; the loader is a pure offline
  read (no network — clause C). `resolve_item` / `resolve_inputs` are pure functions over in-memory
  registry rows and are unit-tested without DuckDB.

**Byte-identity mandate (the sprint's AuditExact analogue).** PF-S1 is a *resolution-layer refactor*:
after S1-3 rewires `TTM_INPUTS`/`BALANCE_INPUTS`/`XBRL_BALANCE_INPUTS` through the registry, a full
`refresh_fundamental_ratios` rebuild MUST produce **byte-identical** output — same row count, same
`ratio_id`s, same `value`/`numerator_value`/`denominator_value`, same `available_at` — as the
pre-refactor rebuild on the same inputs. The registry resolves to the *identical* `canonical_metric`
strings the dicts held today; if it does not, that is a correctness bug, not a data change.

---

## Tasks

### S1-0 — Schema: `fundamental_item` + `fundamental_item_alias` + `fundamental_item_vendor_map`

**Root cause:** there is no canonical item dimension. The statement map's `item_id`
(`fundamental_statements.py:28`, `:73`) points at nothing; the ratio engine's input dicts
(`fundamental_ratios.py:51–65`, `:489–509`) hold bare strings; the cross-vendor cross-walk lives only
in `cross_vendor_field_map.md`.

**Fix:** add three tables via migration **0061** (schema + catalog) and **0062** (indexes):

- `fundamental_item(item_id INTEGER PK, canonical_code VARCHAR NOT NULL, statement VARCHAR,
  section VARCHAR, data_type VARCHAR /* 'flow'|'instant' */, unit_type VARCHAR, sign_convention VARCHAR,
  is_derived BOOLEAN DEFAULT FALSE, definition VARCHAR, citation VARCHAR)` — one row per canonical item
  (item_id 1001…1440 fundamentals, 1501/1601/1701 industry overlays, 2001…2044 estimates), mirroring
  the §10 headline dictionary and the §2 detail rows.
- `fundamental_item_alias(item_id INTEGER, alias_scheme VARCHAR /* 'us-gaap'|'dei' */, alias_code VARCHAR,
  coalesce_priority INTEGER, valid_from DATE, valid_to DATE)` — the concept→item edges, carrying the
  same priority the statement map uses (e.g. `RevenueFromContractWithCustomerExcludingAssessedTax`
  prio 10 → `Revenues` 20 → `SalesRevenueNet` 30, all → item 1001).
- `fundamental_item_vendor_map(item_id INTEGER, vendor VARCHAR /* compustat|factset|ibes|worldscope|
  bloomberg|ciq|sharadar */, vendor_field VARCHAR, sign_note VARCHAR)` — the cross-vendor columns of
  §2/§3 (e.g. item 1001 → compustat `revt`, factset `FF_SALES`, worldscope `01001`, ciq `IQ_TOTAL_REV`).

`0062` adds `idx_fundamental_item_alias_lookup(alias_scheme, alias_code)` and
`idx_fundamental_item_canonical(canonical_code)`. `0061` seeds `table_catalog` + `field_catalog` for all
three tables, following the `migrations.py:1596`/`:1620` pattern.

**PIT/determinism:** clause (B) — schema/index split across 0061/0062; every table catalogued in the
schema migration. `valid_from`/`valid_to` on the alias table carries clause (A) bitemporal alias validity.

**Accept:** the three tables exist after `apply_pending_migrations`; `table_catalog` has 3 new rows and
`field_catalog` has one row per column; re-running the migration is a no-op (idempotent);
`fundamental_item.item_id` is unique.

---

### S1-1 — Seed loader: parse the field map into `db/seeds/fundamental_items.csv` and load the three tables

**Root cause:** the ~480-item dictionary is prose. It must become a committed, hashable, offline seed —
one-time authored, then loaded deterministically with no network (clause C).

**Fix:** a one-time authoring parse (done at plan-authoring time, not at runtime) transcribes
`cross_vendor_field_map.md` §2.1 (income ~47), §2.2 (balance ~44), §2.3 (cashflow ~27), §2.4 (derived
per-share/ratios), §2.5 (banks 11), §2.6 (insurance 8), §2.7 (REIT 8), and §3 (estimates cross-walk,
items 2001…2044) into `db/seeds/fundamental_items.csv` — one row per (item, alias/vendor) with columns
sufficient to populate all three tables (`item_id, canonical_code, statement, section, data_type,
unit_type, sign_convention, is_derived, definition, citation, alias_scheme, alias_code, coalesce_priority,
valid_from, valid_to, vendor, vendor_field, sign_note`). `IFRS is excluded` (the `ifrs-full:*` column of
§2 is not transcribed) per the pf1 constraint. The **loader** in `db/item_registry.py` reads the CSV
offline and populates `fundamental_item` / `fundamental_item_alias` / `fundamental_item_vendor_map` via
`INSERT OR REPLACE`, exactly mirroring `seed_fundamental_statement_map` (`fundamental_statements.py:471`).
Target ~480 items across income/balance/cashflow/derived + bank/insurance/REIT overlays + estimates.

**PIT/determinism:** clause (C) — CSV read only, no SEC/vendor network. Clause (D) — the CSV is committed
and its content hash recorded; same CSV → same rows. Loader is `INSERT OR REPLACE` and idempotent.

**Accept:** `fundamental_item` seeds ~480 rows (assert `count(*) BETWEEN 460 AND 500`); every
`fundamental_item_alias.item_id` FKs a live `fundamental_item`; the alias priorities for item 1001 match
the statement map's revenue COALESCE order (10/20/30); re-loading is a no-op; the CSV parses with the
stdlib `csv` module (no pandas requirement in the load path).

---

### S1-2 — Resolution shim in NEW `db/item_registry.py`

**Root cause:** callers need item identity without reaching into DuckDB or duplicating COALESCE logic.
Today the only "resolution" is the SQL `_pivot_case` (`fundamental_ratios.py:692`) keyed on literal
`canonical_metric` strings.

**Fix:** add pure, DuckDB-free functions to `db/item_registry.py`, operating on registry rows loaded once
into plain dicts/tuples:

- `resolve_item(alias_scheme, alias_code) -> item_id | None` — maps a taxonomy concept (e.g.
  `('us-gaap', 'NetIncomeLoss')`) to its canonical `item_id`.
- `resolve_inputs(canonical_code) -> list[alias]` — returns the ordered alias list for a canonical code
  (e.g. `revenue` → `[RevenueFromContractWithCustomerExcludingAssessedTax, Revenues, SalesRevenueNet]`),
  ordered by `coalesce_priority` ascending (COALESCE order), so a caller can build the same fallback the
  statement map already encodes.
- an optional `as_of` argument filters aliases by `valid_from`/`valid_to` (clause A).

Provide a `Registry` object (or module-level cache) built from either the loaded tables or the seed CSV,
so tests run without a DB. No I/O in the resolve functions themselves.

**PIT/determinism:** clause (D) — pure functions, deterministic ordering (stable sort by
`coalesce_priority` then `alias_code`). Clause (A) — `as_of` alias filtering. Unit-testable with a small
fixture, no DuckDB.

**Accept:** `resolve_item('us-gaap','NetIncomeLoss')` returns item 1031; `resolve_inputs('revenue')`
returns the three revenue aliases in priority order; an as-of before 2018 includes `SalesRevenueNet`,
after its `valid_to` excludes it; unknown scheme/code returns `None` (does not raise); all in
`test_item_registry.py` against a fixture, no network, no DuckDB connection.

---

### S1-3 — Remap the ratio engine to resolve inputs through the registry

**Root cause:** `TTM_INPUTS`/`BALANCE_INPUTS`/`XBRL_BALANCE_INPUTS`/`XBRL_FLOW_INPUTS`
(`fundamental_ratios.py:51`, `:60`, `:489`, `:503`) are hard-coded `key -> canonical_metric` dicts. The
`_pivot_case` SQL (`fundamental_ratios.py:692`) and the `SELECT` in `load_ratio_inputs` (`:769`) consume
those literal strings.

**Fix:** replace the literal RHS of each input dict with a registry lookup that returns the *identical*
`canonical_metric` string it holds today — the dict keys (`rev`, `ni`, `assets`, …) and the resulting
`canonical_metric` values are unchanged; only their *source* moves from a literal to
`item_registry.resolve_*`. Add an item linkage to each emitted ratio row (a nullable `numerator_item_id`
/ `denominator_item_id`, or an `input_item_ids_json`) resolved from `RatioDef.numerator_code` /
`denominator_code` via the registry, so a ratio row records which canonical items it consumed. The
`RATIO_DEFS` math and `compute_ratio_rows` operand logic are untouched (formulae are PF-S4's surface).

**REGRESSION — byte-identity gate.** Because the resolved strings equal the literals, the rebuilt
`fundamental_ratios` table is byte-identical: identical row count, identical `ratio_id`
(the hash of `source|security_id|ratio_code|basis|period_end|available_at`, `fundamental_ratios.py:539`),
identical `value`/`numerator_value`/`denominator_value`/`available_at`. The new item-linkage columns are
purely additive (nullable, not part of `ratio_id`), so pre-existing columns do not change.

**PIT/determinism:** clause (A) preserved verbatim (no `available_at` change); clause (D) — same inputs +
same params → same rows. This is the sprint's core AuditExact-equivalent guarantee.

**Accept:** a regression test rebuilds ratios from a fixed fixture panel twice — once with the literal
dicts (captured golden), once through the registry — and asserts the two `fundamental_ratios` frames are
equal on all pre-existing columns (row count + every value). `test_fundamental_ratios.py` stays green.
Grep proves 0 hard-coded canonical-metric string literals remain as the *authority* in the derived layer
(the registry is the single source).

---

### S1-4 — Link facts: add `item_id` FK to statement points / xbrl points via the alias map

**Root cause:** `fundamental_statement_points` (`schema.py:1092`) and `fundamental_points`
(`schema.py:1233`) carry `taxonomy`/`concept`/`canonical_metric` but no link to the canonical item
dimension, so a fact cannot be joined to its item's vendor cross-walk or definition.

**Fix:** additive backfill migrations **0063** (statement points) and **0064** (xbrl points) add a
nullable `item_id INTEGER` column (`ADD COLUMN IF NOT EXISTS`) and backfill it by joining the fact's
`(taxonomy, concept)` through `fundamental_item_alias` (equivalently the statement map's existing
`item_id`, which S1-1 has now made a real FK target). Catalog the new column in `field_catalog`. This is
additive — no existing column or value changes, and the statement-point rebuild logic
(`refresh_fundamental_statement_points`, `fundamental_statements.py:520`) is not altered (PF-S3/PF-S8 own
that surface).

**Quality check (clause A/D):** report — do not fail on — unmapped concepts. Emit a quality row (via the
established `quality_check` path) listing any `(taxonomy, concept)` with no `item_id`, and assert every
extracted concept resolves to **≤ 1** `item_id` (a concept mapping to two items is a seed bug and must
fail). Unmapped concepts are logged as a `warning`, not an error — coverage widening is PF-S3's job.

**PIT/determinism:** clause (B) — additive, idempotent, catalogued backfills in the reserved range.
Clause (D) — deterministic join, same map → same `item_id` assignments.

**Accept:** `fundamental_statement_points.item_id` and `fundamental_points.item_id` exist and are
backfilled where the alias map covers the concept; the ≤1-item_id assertion passes; the unmapped-concept
report is emitted and non-empty concepts are surfaced (not silently dropped); no pre-existing statement
value changes (byte-identity on the fact tables' non-`item_id` columns).

---

## Sequencing & expected compounding

1. **S1-0** (schema + catalog, migrations 0061/0062) first — nothing can seed or FK before the tables
   exist. Independent of every other sprint's modules.
2. **S1-1** (seed CSV + loader) next — populates the dimension S1-2/S1-3/S1-4 all read.
3. **S1-2** (pure resolution shim) — the DuckDB-free API. Depends only on S1-1's seed shape; unit-testable
   immediately.
4. **S1-3** (ratio-engine remap) — the **byte-identity gate**. Do this only after S1-2 is green and the
   pre-refactor ratio rebuild is captured as a golden. This is where a mis-alias would surface.
5. **S1-4** (fact FK backfill, migrations 0063/0064) — last; additive, and the ≤1-item_id check validates
   the whole seed.

**Expected compounding:** this is the foundation everything downstream FKs to. PF-S3 (concept coverage)
widens `fundamental_item_alias`; PF-S4 (formula library) links `formula_registry` rows to item ids; PF-S5
joins `security_id`; PF-S6 valuation multiples reference item ids for market-cap inputs; PF-S8 lineage
hangs off the same item spine. One governed dictionary replaces four ad-hoc string dicts.

---

## Risks / guardrails

- **A mis-mapped alias silently changes a ratio.** The dominant risk: if the registry resolves `revenue`
  to a different or reordered alias set than the literal dict, the rebuilt ratio value drifts. Mitigated by
  the S1-3 byte-identity regression (same row count + values) AND the S1-4 ≤1-item_id check. No merge
  without a green byte-identity assertion — treat drift as a correctness bug, not a data update.
- **Do not renumber migrations.** Latest landed is `version=59` (`migrations.py:5326`, migration `0059`).
  PF-S1 appends `Migration(version=61…64, …)` entries after it — never edit or renumber `0001–0060`, and
  never claim a number outside the reserved `0061–0064` range.
- **Keep IFRS excluded.** The `ifrs-full:*` column of §2 is not transcribed into the seed; only us-gaap /
  dei aliases populate `fundamental_item_alias`. Adding IFRS is explicitly out of scope for pf1.
- **Additive-only on fact tables.** S1-4's `item_id` column is nullable and never part of any `ratio_id`
  or statement-point key; do not backfill by mutating existing values or rebuilding the fact tables.
- **Purity of the shim.** `resolve_item`/`resolve_inputs` must not open a DuckDB connection — a DB call in
  the hot ratio path would break clause (D) and slow the rebuild. Load the registry once; resolve in memory.

---

## Bench / acceptance

- **Item dimension seeded:** `fundamental_item` has ~480 rows (`assert 460 <= count(*) <= 500`);
  `fundamental_item_alias` covers every us-gaap/dei concept the statement map seeds; three
  `table_catalog` rows added.
- **Ratio rebuild byte-identical:** assert the post-refactor `fundamental_ratios` frame equals the
  captured golden on all pre-existing columns — `assert rebuilt.equals(golden)` (row count, `ratio_id`,
  `value`, `numerator_value`, `denominator_value`, `available_at`, `is_meaningful` all unchanged); only
  additive `*_item_id` columns are new.
- **≤1 item_id per concept:** `assert` no `(taxonomy, concept)` resolves to more than one `item_id`;
  unmapped concepts reported (warning), not failed.
- **0 hard-coded metric strings in the derived layer:** the registry is the single authority for
  `TTM_INPUTS`/`BALANCE_INPUTS`/`XBRL_BALANCE_INPUTS`/`XBRL_FLOW_INPUTS`; grep confirms no literal
  `canonical_metric` dict remains as the source of truth.
- **Tests green:** `python -m pytest atx-impl\db\tests\test_item_registry.py atx-impl\db\tests\test_fundamental_ratios.py -q`
  passes; full `python -m pytest atx-impl\db\tests -q` stays green before commit.
- **Ledger:** append a `WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, domains, verification commands,
  live-DB smoke with exact `fundamental_item` count + `run_id`, caveats/next) and update `PARITY_GAP.md`.
  Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; stage
  explicit paths only — **never `git add -A`**.
