# Sprint PF3-S2 — Schema-contract v2 (PIT-gap close + semantic contracts + versioning + panel contract)

**Goal:** extend pf2-S1's schema-as-contract from a *structural* contract (name / type / nullability / PIT-column presence) into a *semantic, versioned, and forward-facing* one, and retire the one debt pf2-S1 knowingly deferred. Concretely this sprint (a) **closes** the ~56-table PIT-column gap that pf2-S1 pinned as a `<=` ratchet — backfilling `is_latest_revision` and the other missing bitemporal columns on the offenders where they are meaningful, or registering explicit exemptions for the rest, driving the offender count to **0**; (b) adds a **semantic contract** — a declared unit, sign convention, scale, and natural-key per fact column — and *defines* clause (J); (c) **versions** the contract so "this is contract v2" is a first-class, drift-checked assertion rather than an emergent hash; and (d) authors the **panel export contract stub** that PF3-S10's factor-panel export will enforce. Reserved migrations **0135–0137**.

**Mandate / Owns:** `db/schema_contract.py` v2 (semantic tier on `ColumnSpec` + contract versioning), PIT-gap close in `db/quality.py` (`pit_column_presence_check`) + a `pit_exemption` registry in migrations, NEW `db/panel_contract.py` (stub), `db/tests/test_schema_contract_v2.py`.

**Must NOT touch:** the **drift-detector core** — `schema_contract.py::detect_schema_drift` / `schema_contract_sha256` / `build_contract_manifest` are pf2-S1's and are extended **additively** (new optional `ColumnSpec` fields, a widened hash), never rewritten. The **backfill/maintenance DAG** is PF3-S1's — this sprint neither drives nor extends it. The **factor-panel materialization** (`v_factor_panel`, Parquet/Arrow export) is PF3-S10's — S2 ships only the *contract* it consumes, not the export. Do not edit any landed migration (≤ 0134) or rewrite `_seed_catalog` / `_seed_field_catalog`.

**Depends on:** PF3-S1 (backfill rails landed) + pf2-S1 (schema-as-contract: `CONTRACT`, `detect_schema_drift`, the persisted `schema_contract` table from migration 0097) + pf2-S2 (migration governance: checksum + post-migration verify). Sequential in the rails wave — **after** PF3-S1, **before** PF3-S3 (the architecture decomposition must not run while S2 is reshaping `schema_contract.py`/`quality.py`).

---

## Baseline / where the cycles go

pf2-S1 made the warehouse shape a declared, drift-checked contract, but it is a *structural* contract with one deferred hole and three blind spots. Measured 2026-07-04 against `atx-impl/db`.

1. **The ~56-table PIT gap is pinned open, not closed.** `quality.py::pit_column_presence_check` (line 7083) partitions tables via the S1-2 manifest — a table is fact/derived iff any `ColumnSpec.is_pit_column` — and flags every such table missing ≥1 of the five `PIT_COLUMN_NAMES` (`as_of_date, available_at, source_loaded_at, run_id, is_latest_revision`). It fires on ~56 real pre-existing fact tables (mostly missing `is_latest_revision`); pf2 pinned that count as a `<=` ratchet rather than failing at `>0`, deferring the backfill/exemption. The check is authored `severity=critical` with `threshold_value=0.0`, so the *intent* is 0 — the ratchet is a temporary ceiling, and a genuinely new hole can hide beneath it.

2. **The contract is structural, not semantic.** A `ColumnSpec` declares `name / data_type / nullable / is_natural_key / is_pit_column / declared_in` — but nothing about **unit** (shares vs dollars vs ratio vs bps), **sign convention** (a value that is always ≥ 0 vs signed), or **scale**. `field_catalog` carries a `unit VARCHAR` column (`schema.py:80`) but it is nullable, descriptive-only, and re-derived from live columns by `_seed_field_catalog` on every bootstrap — so it is never *validated* against a declared expectation. Shares, dollars, and unit-interval ratios are indistinguishable to every check.

3. **The contract is unversioned.** `schema_contract_sha256` (line 366) hashes the sorted manifest and migration 0097 persists that `manifest_sha256` into the `schema_contract` table (`migrations.py:~7983`), so the machinery *detects that the manifest changed* — but there is no declared identity. Nothing asserts "the live warehouse is on contract **v2**"; a hash is a fingerprint, not a version, and cannot gate "the shape changed but no one bumped the version."

4. **No export contract exists for a downstream panel consumer.** PF3-S10 will export `v_factor_panel` as catalogued PIT views + partitioned Parquet/Arrow, and clause (I) requires that panel be PIT-safe by construction — keyed at `(security_id, as_of_date)`, unit/sign-coherent. Today there is nothing S10 can enforce against: no declared panel column set, no declared keys, no per-column unit/sign for the exported surface.

**Already good — do not regress:** the declarative manifest (`CONTRACT` + `build_contract_manifest`'s zero-residual reconciliation over both schema paths); `detect_schema_drift`'s pure, deterministic diff over `duckdb_tables()`/`duckdb_columns()` and its closed `DRIFT_TYPES` set (incl. `missing_pit_column`); `schema_contract_sha256` as the single comparable baseline (`migration_admin.py:271` reads it for the post-migration verify); and pf2-S1's catalog-completeness + PIT-presence gates. S2 widens these; it does not replace them.

---

## PIT / determinism + production contract

Clauses **(A)–(G)** carry forward unchanged; **(J) is DEFINED by this sprint** (every fact/metric/factor column declares unit + sign + scale, and a check fails on a value violating its declared domain), and clause (I)'s keys are pre-declared here for S10.

- **(A)** The S2-0 backfill computes `is_latest_revision` deterministically from each table's own revision lineage (the `revision_sequence = revision_count` pattern already used across `quality.py`), and derives `available_at`/`source_loaded_at`/`run_id` from load provenance where recoverable; no value is fabricated.
- **(B)/(F)** Migrations **0135–0137** only; never renumber or edit ≤ 0134. Backup-before-migrate before any live apply. **Schema/index split** across numbers per the standing precedent: **0135** = PIT-column backfill ALTERs + the `pit_exemption` registry table + its catalog rows; **0136** = the semantic-contract columns on `schema_contract` (unit/sign/scale/natural-key) + re-seed; **0137** = the `schema_contract_version` row + the `panel_contract` catalog stub. Each new table/view seeds `table_catalog` + `field_catalog` in the same migration.
- **(C)** All tests run against in-memory / template-copy DuckDB with fixtures; the live PIT-offender count, the semantic-check headline, and the version pin are operator-run and recorded in the ledger, never in pytest.
- **(D)** Every new surface is a pure function of declarations (+ `field_catalog` for unit seeding): same manifest → same semantic tier → same `schema_contract_sha256`. Backfill UPDATEs are idempotent (guarded so a re-run is a no-op).
- **(J)** Defined here: the semantic tier is the declaration and `semantic_contract_check` (S2-2) is the enforcement; it is authored `severity=critical` so PF3-S12 can gate it (clause G).

---

## Tasks

### S2-0 — Close the ~56-table PIT gap (backfill or explicit exempt; ratchet target 0)

**Root cause:** `pit_column_presence_check` reports ~56 fact/derived tables missing ≥1 PIT column — overwhelmingly `is_latest_revision` — and pf2 froze that count as a `<=` ratchet because the backfill was out of pf2 scope. A pinned ratchet tolerates the known holes *and* masks new ones, and it holds clause (A) open on the exact tables the factor store will read.

**Fix:** for each offender, do one of two things. **(a) Backfill** — `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` the missing PIT column(s) and populate them: `is_latest_revision` computed from the table's revision keys (`revision_sequence = revision_count`), `available_at`/`source_loaded_at`/`run_id` from recoverable load provenance. **(b) Exempt** — register a row in a NEW `pit_exemption(table_name, missing_columns, reason, exempted_by, exempted_at)` registry for tables where a PIT column is genuinely not meaningful (a static dimension/reference table the manifest tags fact-like). Extend `pit_column_presence_check` to subtract registered exemptions from the offender set so the ratchet target becomes **0**: every offender is either backfilled to PIT-complete or has an exemption row **with a non-empty reason**. Migration **0135**: the backfill ALTERs + populate UPDATEs (idempotent) + the `pit_exemption` table + its catalog rows.

**PIT:** (A) `is_latest_revision` derived from revision lineage, not guessed. (B) 0135 schema + catalog; UPDATEs guarded so re-apply is a no-op. (G) the check stays `severity=critical`, now with the true `threshold_value=0.0` and no ratchet ceiling.

**Accept:** after 0135 the check reports `observed_value == 0` on the live warehouse; every previously-offending table is PIT-complete or carries a `pit_exemption` row with a reason; a fixture that strips `available_at` from a **non-exempt** fact table still goes RED (the exemption registry cannot be threshold-abused to hide a live hole).

### S2-1 — Semantic column contract (unit / sign / scale / natural-key)

**Root cause:** the contract declares structure but no meaning (baseline 2); `field_catalog.unit` is nullable, descriptive, and never validated, so no check can tell shares from dollars from a `[0,1]` ratio.

**Fix:** extend `ColumnSpec` with optional semantic fields — `unit`, `sign` (∈ `signed` / `non_negative` / `non_positive` / `unit_interval` / `bounded`), `scale`, and promote `is_natural_key` into the semantic tier — seeded from the manifest declarations plus `field_catalog.unit` where present. Keep it **additive**: `detect_schema_drift`'s structural diff is untouched, and `schema_contract_sha256` widens to cover the semantic tier (one intentional hash change, tied to the S2-3 version bump). Migration **0136** adds `unit`/`sign`/`scale`/`natural_key` columns to the persisted `schema_contract` table and re-seeds them, so the semantic tier is queryable in plain SQL, not only importable Python.

**PIT:** (B) 0136 columns + re-seed + catalog. (D) the seed is a pure function of declarations + `field_catalog` — deterministic across bootstraps.

**Accept:** every fact column in the manifest resolves a non-null `unit` and `sign`; the persisted `schema_contract` rows carry them; `schema_contract_sha256` changes exactly once (the semantic tier landing) and is then stable across re-bootstraps.

### S2-2 — Semantic validation as a gated check

**Root cause:** with units and signs declared but unenforced, clause (J) is defined-on-paper only — a loader can write negative shares, a ratio outside `[0,1]`, or a debt item with the wrong sign and nothing objects.

**Fix:** NEW `semantic_contract_check` in `db/quality.py`, `severity=critical`, that reads the S2-1 semantic tier (not a hardcode) and asserts each fact column's values respect its declared domain: `non_negative` columns (shares, counts, volumes) ≥ 0; `unit_interval`/`bounded` columns within their declared bounds; sign coherence between paired items where declared (e.g. an asset item `non_negative` vs a contra/liability item `non_positive`). Register it gate-ready (clause G) so PF3-S12 can halt on it. It reuses the existing `QualityResult` + `_table_exists` machinery like the other schema checks.

**PIT:** (C) fixtures plant a negative share count and an out-of-bound ratio → RED; green on the live slice. (D) pure over the semantic manifest → deterministic rows.

**Accept:** green on the live warehouse; a fixture planting a unit/sign violation surfaces exactly that column RED; a **legitimate** negative on a `signed` column (a net loss, negative shareholders' equity) does **not** false-positive.

### S2-3 — Contract versioning + panel export contract stub

**Root cause:** the manifest hash detects change but declares no identity (baseline 3), and no export contract exists for the S10 panel consumer (baseline 4).

**Fix:** persist a `schema_contract_version` — a semantic version string (`v2`) paired with the `schema_contract_sha256` baseline — as a first-class catalog row (migration **0137**), pinned and drift-checked: a manifest change without a matching version bump fails. And author NEW `db/panel_contract.py` (stub) declaring the factor-panel export shape as data — a `PANEL_CONTRACT` manifest of the column set, the `(security_id, as_of_date)` keys, and per-column unit/sign, plus a `panel_contract_sha256`, mirroring `schema_contract.py`'s shape. It is the contract PF3-S10's `v_factor_panel` + Parquet/Arrow export will enforce (clause I keys pre-declared here). Migration 0137 catalogs the `panel_contract` table stub.

**PIT:** (B) 0137 version row + `panel_contract` catalog stub. (I) the stub encodes the PIT keys S10's lookahead test enforces — it is authored, not wired to any export in this sprint.

**Accept:** `schema_contract_version == v2`, pinned and drift-checked (a manifest change without a version bump fails); `import db.panel_contract` succeeds and exposes `PANEL_CONTRACT` + `panel_contract_sha256`; the panel column set + keys + units are declared, hashable, and stable across re-import.

---

## Sequencing & expected compounding

**S2-0 → S2-1 → S2-2 → S2-3.** S2-0 first — closing the PIT gap (or exempting with a reason) is the precondition for turning the ratchet into a hard `0` and for trusting the fact/non-fact partition the semantic tier reads. S2-1 then lands the semantic declarations; S2-2 turns them into a gate (a semantic check over an unseeded tier would be inert). S2-3 versions the now-complete contract and pre-declares the panel shape last. Compounding: once the contract is PIT-complete, semantic, and versioned, PF3-S6's ratio/metric engine lands on a contract that *validates units and signs* rather than trusting them, and PF3-S10's panel export lands on a declared, hashable shape it can refuse to drift from — the panel stub authored here is exactly what S10 enforces and S12 gates.

---

## Risks / guardrails

- **Do not threshold-away real gaps.** The exemption registry is for tables where a PIT column is genuinely not meaningful, each with a written **reason** — it is not a mechanism to keep the offender count low. A fact table that *should* carry `available_at` gets the column backfilled, not an exemption. The S2-0 fixture (strip `available_at` from a non-exempt table → RED) is the mitigation.
- **Semantic validation must not false-positive on legitimate negatives.** A net loss, negative retained earnings, or negative equity is valid on a `signed` column; only `non_negative` / `unit_interval` / `bounded` domains constrain values. Sign coherence is asserted only where the pairing is declared, never inferred from a column name.
- **Additive, not a rewrite.** `detect_schema_drift`, `schema_contract_sha256`, and `build_contract_manifest` are pf2-S1's core; S2 adds optional `ColumnSpec` fields and widens the hash exactly once — it does not restructure the detector or the manifest builder.
- **Stay in 0135–0137.** Schema/index/version split across the three numbers; never edit a landed migration; timestamped DB+WAL backup before any live apply.

---

## Bench / acceptance

- **0 missing-PIT offenders** — `pit_column_presence_check` reports `observed_value == 0` live; every former offender is PIT-complete or carries a `pit_exemption` row with a reason; the non-exempt strip fixture goes RED.
- **Every fact column carries unit + sign** — the manifest and the persisted `schema_contract` rows resolve non-null `unit`/`sign` for every fact column; `schema_contract_sha256` changes once (semantic landing) then is stable.
- **Semantic check green live / red on fixtures** — `semantic_contract_check` passes on the live slice and goes RED on a planted negative-share / out-of-bound-ratio fixture without false-positiving a legitimate negative.
- **Contract version pinned + drift-checked** — `schema_contract_version == v2`, and a manifest change without a version bump fails the pin.
- **`panel_contract` importable** — `db.panel_contract` imports, exposes `PANEL_CONTRACT` + `panel_contract_sha256`, and declares the `(security_id, as_of_date)` keys + per-column units.
- `python -m pytest atx-impl\db\tests\test_schema_contract_v2.py -q` green, and full `python -m pytest atx-impl\db\tests -q` green before commit.
- **Live-DB smoke** recorded in the ledger: pre/post PIT-offender counts, the exemption list with reasons, the semantic-check result, `schema_contract_version`, `schema_contract_sha256`, and the `run_id`.
- `PARITY_GAP.md` updated (clause J now defined/enforced; PIT gap closed); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with exact counts + run_id, caveats/next → PF3-S3 decomposition, PF3-S10 panel enforcement).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish schema-contract-v2`; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
