# Sprint PF2-S1 — Schema-as-contract + drift detection + catalog enforcement

**Goal:** make the warehouse schema a declared, enforced contract instead of the union of two imperative `CREATE IF NOT EXISTS` code paths: reconcile `schema.py::ensure_quant_schema` and `migrations.py::MIGRATIONS` into one declarative manifest, add a live-vs-contract drift detector, gate "every table is catalogued and carries the bitemporal PIT columns," and ship a queryable as-of warehouse data-catalog reader. This is the first pf2 sprint and it *defines* clause (E). Reserved migrations 0097–0099.

**Mandate / Owns:** NEW `db/schema_contract.py` (declarative manifest + `detect_schema_drift`), catalog-enforcement + PIT-column checks in `db/quality.py`, a warehouse-catalog as-of reader in `db/asof.py`, `db/tests/test_schema_contract.py`.

**Must NOT touch:** the migration *runner* checksum/apply-lock/backup surface (`apply_pending_migrations`, `schema_migrations.checksum` writes) — that is **PF2-S2**'s job. S1 *defines* the contract that S2's post-migration verify checks against; it does not wire checksum enforcement. Also do not edit any landed migration (≤ version 88) or rewrite `_seed_catalog` / `_seed_field_catalog` logic — S1 reads and enforces over them, additively.

**Depends on:** pf1 only. pf1-S4-3's catalogued-view + as-of pattern (`_formula_registry_catalog_view` / `v_formula_registry`, migration 77; `formula_registry_asof` in `db/asof.py`) is the template S1-3 extends. Sequential *before* PF2-S2 (S2's backup/checkpoint/DR governance runs migrations on the rails S1 lays).

---

## Baseline / where the cycles go

Schema "truth" is not written down anywhere; it is emergent from code, and nothing checks the live DB against an intended shape. Measured 2026-07-03 against `atx-impl/db`.

1. **Truth is SPLIT across two imperative paths.** ~60 core tables are defined in `schema.py::ensure_quant_schema` (`CREATE TABLE IF NOT EXISTS` for `schema_migrations`, `source_systems`, `dataset_catalog`, `table_catalog`, `field_catalog`, …). Everything from pf1-S1 onward (`fundamental_item`, `formula_registry`, `identifier_spine`, `xbrl_validation_results`, …) is defined **only** in `migrations.py`, whose `MIGRATIONS` list is non-contiguous (present: 1–77, 79–83, 88 = `xbrl_validation_dimensional_evidence`; 78/84–87/89–96 are gaps/reserved). Both paths use `IF NOT EXISTS`. There is **no single declarative manifest** — no code answers "what tables/columns *should* exist" without executing both paths against a live connection.

2. **`table_catalog` is hand-seeded and silently lags.** `_seed_catalog` (`schema.py:1989`) writes `table_catalog(table_name PK, layer, entity, grain, description, natural_key_json, pit_notes, …)` via `INSERT OR REPLACE` literals (~line 2232), and each migration hand-inserts its own rows. **Nothing asserts `duckdb_tables()` ⊆ `table_catalog`.** A new table can land fully functional with no catalog row and nothing fails; the catalog drifts behind the schema with no signal.

3. **`field_catalog` is auto-derived but never validated against a baseline.** `_seed_field_catalog` (`schema.py:2366`) bulk-reloads `field_catalog(table_name, field_name, semantic_type, description, nullable, unit, source_field, …)` from `duckdb_columns()` on every bootstrap. So it always *mirrors* the live columns — which is exactly why it can never *detect* drift: it re-describes whatever is there. Nothing compares the live column set to an expected declared set.

4. **A schema-hash mechanism exists — but only for lake files, not the live DB.** `lake.py::_schema_sha256` (`lake.py:168`) stamps a `schema_sha256` per export and re-checks it against `expected_schema_sha256` on read. That discipline (declare a shape, hash it, alert on mismatch) is precisely what the live warehouse lacks. There is no analogous baseline for `duckdb_tables()`/`duckdb_columns()`.

5. **PIT-column presence is assumed, never enforced.** Clause (A) requires every fact/derived row to carry `as_of_date, available_at, source_loaded_at, run_id, is_latest_revision`, but no check asserts a fact table actually has those columns. A fact table shipped without `available_at` would pass every existing check and silently break as-of readers.

6. **The `checksum` column is dead.** `schema_migrations(version VARCHAR PK, description, checksum VARCHAR, applied_at)` has `checksum` but `apply_pending_migrations` inserts tracking rows **without ever writing it**. S1 does the schema-*shape* contract; wiring `checksum` enforcement is **PF2-S2** — do not duplicate it here.

**Already good — do not regress:**
- **The idempotent bootstrap short-circuit.** `connection.py::_schema_is_current` (max integer `MIGRATIONS` version vs `schema_migrations`) and `initialize()`'s `ensure_quant_schema` → `apply_pending_migrations` ordering stay. The drift check is *additive* and must not force a full rebuild on every run.
- **The auto field-catalog.** `_seed_field_catalog`'s single-statement bulk `INSERT OR REPLACE` from `duckdb_columns()` (the N+1 fix) stays the source of field descriptions; S1 reads it, does not replace it.
- **The pf1-S4-3 catalogued-view + as-of precedent.** `v_formula_registry` + `formula_registry_asof` (`FORMULA_REGISTRY_ASOF_SQL`, `_register_filter`, `_normalize_strings`, read-only `connect`) is the exact shape S1-3 reuses — a view catalogued with its own `table_catalog`/`field_catalog` rows plus a bitemporal Python reader.

---

## PIT / determinism + production contract

Clauses **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, and **(E)** schema-as-contract apply in full; **(E) is DEFINED by this sprint.** **(A)** applies to S1-3's as-of reader, **(D)** to the drift detector, **(F)** to the migration split, **(G)** to S1-1's gated checks (adopted incrementally).

- **(E)** No table lands without a contract row (columns/types/nullability/natural key/required PIT columns) **and** a `table_catalog` entry. `detect_schema_drift` fails if live schema diverges from the manifest or if any `duckdb_tables()` table is uncatalogued. This is the clause's first implementation.
- **(B)/(F)** Migrations **0097–0099** only (integer versions 97–99 in `MIGRATIONS`; never renumber, never edit ≤88). Schema-vs-index **split** across numbers per the S5g/S5k WAL precedent: `0097` `schema_contract` table + its own `table_catalog`/`field_catalog` seed; `0098` its indexes; `0099` the warehouse-catalog view. Timestamped DB+WAL backup before any live apply.
- **(C)** All tests run against in-memory / template-copy DuckDB with fixture tables. No network. The live drift-scan headline (undeclared/uncatalogued counts) is operator-run and recorded in the ledger, never in pytest.
- **(A)/(D)** The drift detector is a pure read over `duckdb_tables()`/`duckdb_columns()` + the manifest → deterministic rows. S1-3's reader gates on definition validity like `formula_registry_asof` (no lookahead).

---

## Tasks

### S1-0 — Declarative schema contract + live-vs-contract drift detector *(the big one)*

**Root cause:** there is no declared schema — "truth" is whatever executing `ensure_quant_schema` + `MIGRATIONS` leaves behind, and `field_catalog` is re-derived from the live columns so it can never disagree with them. Nothing can answer "does the live DB match its intended shape," so a dropped column, a type change, or an uncatalogued new table is invisible.

**Fix:** NEW `db/schema_contract.py` exposing a declarative manifest — `CONTRACT: dict[table_name -> ColumnSpec[]]` with `(name, data_type, nullable, is_natural_key, is_pit_column, declared_in∈{schema_py,migration})` — persisted to a NEW `schema_contract` table (migration **0097**; indexes **0098**), catalogued in the same migration per (B). `detect_schema_drift(store) -> list[DriftRow]` diffs live `duckdb_tables()`/`duckdb_columns()` against the manifest and emits typed drift: `undeclared_table`, `uncatalogued_table` (in `duckdb_tables()` but absent from `table_catalog`), `missing_table`, `undeclared_column`, `missing_column`, `type_mismatch`, `nullability_mismatch`, `missing_pit_column`. A `schema_contract_sha256` (mirroring `lake.py::_schema_sha256` over the sorted manifest) is the single comparable baseline S2's post-migration verify will later read.

**PIT:** (D) pure diff over introspection + manifest → deterministic. (B) 0097 schema + catalog, 0098 indexes.

**Accept:** on a fixture DB matching the manifest, `detect_schema_drift` returns `[]`; planting an extra column, a missing `available_at`, and an uncatalogued table each surfaces exactly one typed drift row; `schema_contract_sha256` is stable across re-bootstraps of the same schema.

### S1-1 — Catalog-completeness + PIT-column-presence as gated quality checks

**Root cause:** `table_catalog` is hand-seeded with no `duckdb_tables()` ⊆ `table_catalog` guard (fact 2) and PIT-column presence is never asserted (fact 5), so both drift silently.

**Fix:** register two first-class checks in `db/quality.py` reusing the existing `QualityResult` + `_table_exists` machinery: (a) **catalog-completeness** — every `duckdb_tables()` table (minus a curated ephemeral/internal skiplist: `duckdb_%`, `sqlite_%`, `pragma_%`, registered temp relations) has a `table_catalog` row; (b) **PIT-column presence** — every table whose contract marks it a fact/derived layer carries all five PIT columns `as_of_date, available_at, source_loaded_at, run_id, is_latest_revision`. Both read the S1-0 manifest so the fact/non-fact partition is data, not a hardcode. Author them `severity=critical` so PF2-S10 can gate them (clause G, incrementally). Any catalog rows needed fold into migration **0097**; no new migration otherwise.

**PIT:** (C) fixtures with a planted uncatalogued table and a PIT-column-stripped fact table. (G) checks authored gate-ready.

**Accept:** both checks green on the live warehouse (0 uncatalogued, 0 PIT-missing) and red on fixtures with a planted uncatalogued table / a fact table missing `available_at`; existing `quality.py` checks unaffected.

### S1-2 — Contract coverage for both schema paths (schema.py + migrations.py reconciled)

**Root cause:** the manifest is only trustworthy if it covers *both* imperative paths; a contract that knows only `schema.py`'s ~60 core tables would flag every migration-defined table (`fundamental_item`, `formula_registry`, `xbrl_validation_results`, …) as `undeclared`.

**Fix:** `schema_contract.py::build_contract_manifest()` reconciles both sources into one manifest the S1-0 detector reads — the `ensure_quant_schema` core tables and every table introduced by a `MIGRATIONS` entry — tagging each row's `declared_in` so a future reader knows which path owns it. Seed those rows into `schema_contract` during migration **0097**'s `up()`. Add a coverage test asserting `manifest.tables == duckdb_tables()` on a freshly bootstrapped DB (both paths applied) — i.e. the two code paths and the declared contract are reconciled to zero residual.

**PIT:** (B) rows seeded in 0097, catalogued. (C) offline over a fully-migrated in-memory DB.

**Accept:** on a fresh bootstrap, coverage residual is 0 (no table in either path is missing from the manifest, none in the manifest is absent live); `declared_in` correctly attributes each table to `schema_py` vs `migration`.

### S1-3 — Warehouse data-catalog as-of reader + CLI

**Root cause:** pf1-S4-3 made *formulas* queryable (`formula_registry_asof`), but tables, fields, and lineage are only readable by introspecting a live connection; there is no single as-of surface answering "what did the warehouse catalog look like on date D."

**Fix:** migration **0099** adds `v_warehouse_catalog` — a view joining `table_catalog` + `field_catalog` (+ `v_formula_registry` for formula lineage) into one row-per-(table,field) catalog surface, catalogued with its own `table_catalog`/`field_catalog` rows exactly like `_formula_registry_catalog_view`. Add `warehouse_catalog_asof(as_of_date, …, store=None, tables=None, layers=None)` to `db/asof.py`, mirroring `formula_registry_asof` (`_normalize_strings`, `_register_filter`, read-only `connect`, module `__init__` export) and gating on catalog `updated_at ≤ as_of_ts`. A thin CLI (`python -m db.asof warehouse-catalog --as-of …`) prints the resolved catalog.

**PIT:** (A) reader gates on availability, no lookahead. (B) 0099 view catalogued.

**Accept:** `warehouse_catalog_asof` returns table+field+formula lineage filterable by `tables`/`layers`; an as-of date before a table's catalog `updated_at` excludes it; view carries its own catalog rows.

---

## Sequencing & expected compounding

**S1-0 → S1-2 → S1-1 → S1-3.** S1-0 lays the manifest + detector (load-bearing — everything reads it). S1-2 makes that manifest *complete* over both schema paths (a detector over a partial manifest is worse than none — it cries wolf), so it lands before the gated checks. S1-1 then turns catalog/PIT completeness into gates on the now-trustworthy manifest. S1-3 exposes the enforced catalog as an as-of surface last. Compounding: once drift is a green gate, PF2-S2 can wire `schema_migrations.checksum` + a post-migration verify *against the S1 contract* with a real baseline to compare to; every downstream content sprint (S3–S9) that adds a fact table now lands on rails that *refuse* an uncatalogued or PIT-incomplete table.

---

## Risks / guardrails

- **Manifest completeness is the whole game.** An incomplete manifest makes the detector flag legitimate tables `undeclared` and trains operators to ignore it. S1-2's zero-residual coverage test is the mitigation and must land before S1-1's gates are enforced.
- **Do not force a full rebuild.** The drift scan must not defeat `_schema_is_current`'s short-circuit; run it as an on-demand check / quality gate, not inside the hot `initialize()` path.
- **Ephemeral relations must not read as drift.** Registered temp relations (e.g. `_field_catalog_seed`) and `duckdb_%`/`sqlite_%`/`pragma_%` internals are excluded from both the drift scan and catalog-completeness, matching `_seed_field_catalog`'s existing filter.
- **Stay in S1's lane.** No `checksum` writes, no apply-lock, no backup runner — those are PF2-S2. S1 only *defines and verifies shape*.
- **Migration/WAL safety.** Schema (0097) / index (0098) / view (0099) split per the S5g/S5k precedent; timestamped DB+WAL backup before any live apply; strictly within 0097–0099.

---

## Bench / acceptance

- `detect_schema_drift` returns `[]` on the live warehouse (0 undeclared, 0 uncatalogued, 0 missing-PIT); planted-drift fixtures each surface exactly one typed row.
- Catalog-completeness + PIT-presence checks green live, red on planted fixtures; `schema_contract_sha256` stable across re-bootstrap.
- `warehouse_catalog_asof` returns table/field/formula lineage as-of; excludes rows catalogued after the as-of date.
- `python -m pytest atx-impl\db\tests\test_schema_contract.py -q` green, and full `python -m pytest atx-impl\db\tests -q` green before commit.
- **Live-DB smoke** recorded in the ledger: pre/post `undeclared`/`uncatalogued`/`missing-PIT` counts, `schema_contract_sha256`, and the `run_id`.
- `PARITY_GAP.md` status updated (clause E now defined/enforced); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with exact counts + run_id, caveats/next → PF2-S2 checksum wiring).

**Process:** never `git add -A` (stage explicit paths); never push unless asked. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
