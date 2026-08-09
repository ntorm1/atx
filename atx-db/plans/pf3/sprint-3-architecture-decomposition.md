# Sprint PF3-S3 — Architecture decomposition (split the load-bearing monoliths behind stable interfaces)

**Goal:** the load-bearing coordination hubs grew append-only across ~45 sprints into monoliths — measured 2026-07-04, `db/migrations.py` is **10,854 lines (~502K)**, `db/quality.py` **7,255 lines (~309K)**, `db/asof.py` **4,244 lines (~161K)**, and `db/estimates.py` **3,873 lines (~150K)** (with `db/fundamental_statements.py` at **1,985 lines / ~129K** trailing) — correct, but past the point where any one of them can be held in an editor's working memory or edited without collateral risk. Split the four load-bearing ones — `migrations.py`, `quality.py`, `asof.py`, `estimates.py` — into cohesive **sub-packages** behind **stable public interfaces** (every existing import keeps resolving unchanged), and add a **module-boundary + import-graph lint gate** so the packages cannot silently re-tangle back into monoliths. This is a **pure-refactor sprint**: behavior-preserving and regression-locked — no table, check, reader, or migration changes shape. Reserved migrations **0138–0139** (used only if catalog rows physically relocate; the work is overwhelmingly code, not schema).

**Mandate / Owns:** decomposition of `db/migrations.py` / `db/quality.py` / `db/asof.py` / `db/estimates.py` into packages (`db/migrations/`, `db/quality/`, `db/asof/`, `db/estimates/`), a NEW module-boundary + import-graph lint (`db/module_boundaries.py` or equivalent), and `db/tests/test_module_boundaries.py`. Re-export shims that preserve every landed import path.

**Must NOT touch:** behavior — no migration renumbering, no migration-body edits, no quality-check semantics change, no as-of reader logic change, no schema drift, no `table_catalog`/`field_catalog` content change. The public import surface must keep working verbatim: `from db.migrations import MIGRATIONS`, `from db.quality import run_warehouse_quality_checks`, `from db.asof import fundamentals_asof`, `from db import fundamentals_asof` (the `db/__init__.py` re-exports), `from db.estimates import EstimateActualsDataset` — all preserved via package `__init__` re-export shims. No content sprint's reserved region (0140+) is entered. `fundamental_statements.py` is **content-owned** (pf2) — decompose opportunistically only if it is genuinely in the way, never as a mandate.

**Depends on:** PF3-S1 (backfill/maintenance DAG) and PF3-S2 (schema-contract v2). Sequential — the **last of the rails wave**, landing before the content wave so every content sprint (S4–S12) extends a focused, boundary-linted package instead of appending to a monolith. Critically depends on **pf2-S2's migration checksum governance** (`schema_migrations.checksum`, `verify_migration_checksums`), which this sprint must not break.

---

## Baseline / where the cycles go

The hubs are append-only coordination surfaces (ROADMAP "Primary-module ownership") — the discipline that kept ~45 sprints from colliding is exactly what let each hub grow without bound. Measured 2026-07-04 against `atx-impl/db`.

1. **`migrations.py` — 10,854 lines / ~502K, one flat registry + ~224 inline bodies.** `MIGRATIONS: list[Migration]` is declared at **line 10235**; the `Migration` frozen dataclass at **line 23**; roughly **224 `Migration(...)` entries** each wire a `version=` to an `up` builder, and ~200 module-level `_*` body functions (`_estimate_detail_panel`, `_insider_ownership`, `_delisting_events`, `_shares_outstanding_history`, …) precede the list. Interleaved with all of this is the **runner + governance surface** that must stay intact: `_migration_source_checksum` (**line 37**), `verify_migration_checksums` (**line 99**), `_backfill_missing_migration_checksums` (**line 138**), `_validate_migration_registry` (**line 89**), `_migration_by_version` (**line 84**), and the apply-lock trio `claim_apply_lock` / `release_apply_lock` / `acquire_apply_lock`. One file mixes governance machinery with 200+ unrelated schema bodies.
2. **`quality.py` — 7,255 lines / ~309K, one registry function holding ~320 check literals.** The single public entry `run_warehouse_quality_checks` (**line 7133**) iterates specs produced by `_check_specs(...)` (**line 265**), which returns a `tuple[SqlQualityCheck, ...]` built from roughly **320 `SqlQualityCheck(...)` / `ReferentialQualityCheck(...)` construction sites** spanning every domain (fundamentals, valuation, estimates, pricing, ownership, PIT/catalog). The dataclasses `SqlQualityCheck` (**line 72**), `QualityResult` (**line 87**), `ReferentialQualityCheck` (**line 136**) and helper `_main_objects` (**line 98**) sit atop one ~7K-line wall of inline SQL.
3. **`asof.py` — 4,244 lines / ~161K, ~50 SQL constants + ~56 readers, all re-exported.** There are **50 `*_ASOF_SQL` string constants** (`SECURITY_MASTER_ASOF_SQL`, `FUNDAMENTALS_ASOF_SQL`, `THIRTEENF_POSITIONING_ASOF_SQL`, …) and **56 `*_asof()` reader functions** (`security_master_asof`, `fundamentals_asof`, `fundamental_statements_asof`, …) sharing a small helper core (`_normalize_strings`, `_register_filter`, `end_of_day_asof_ts`). Every reader is re-exported through `db/__init__.py` (`from .asof import …`), so the public import surface is unusually wide and any split must preserve **two** import paths per reader (`from db.asof import …` and `from db import …`).
4. **`estimates.py` — 3,873 lines / ~150K, 8 Datasets + a forest of column specs.** Eight `Dataset` subclasses (`EstimateMeasureSeedDataset`, `EstimateActualsDataset`, `EstimateSurpriseDataset`, …) with option dataclasses (`EstimateActualsOptions`, `EstimateSurpriseOptions`, …), plus a large block of column-spec constants and alias maps (`ESTIMATE_DETAIL_COLUMNS`, `ESTIMATE_CONSENSUS_COLUMNS`, `DETAIL_COLUMN_ALIASES`, `IBES_MEASURE_MAP`, …) — a natural seam between column contracts, compute, and dataset wiring.
5. **`fundamental_statements.py` — 1,985 lines / ~129K — content-owned, note only.** Included in the monolith census for completeness; it is a pf2 content module, so decompose it only opportunistically, never as an S3 deliverable.
6. **Why size is a real, recurring cost.** These four total **~28,000 lines / ~1.28 MB**. Size is not cosmetic: (a) a 10K-line file cannot be held in context, so every edit is made partially blind; (b) append-only growth means new rows land far from related ones, so internal boundaries are implicit and unenforced; (c) the merge-conflict surface is enormous — two sprints appending near the same registry tail collide; (d) a targeted change (one migration body, one check) forces reasoning over the whole file. Every remaining pf3 content sprint appends to these same hubs; left monolithic they compound the cost sprint over sprint.

**Already good — do not regress:**
- **The stable public APIs.** `MIGRATIONS`, `run_warehouse_quality_checks`, the 56 `*_asof` readers (and their `db/__init__.py` re-exports), and the estimate `Dataset` classes are the contract every caller and test depends on. Decomposition is invisible to them.
- **pf2-S2's deterministic migration governance.** The `MIGRATIONS` ordering, `_validate_migration_registry`, and the `schema_migrations.checksum` computed by `_migration_source_checksum` + verified by `verify_migration_checksums` are load-bearing invariants. They must survive the split byte-for-byte (see Risks).
- **The catalogued-view + as-of pattern (pf1-S4-3 / pf2-S1).** `v_formula_registry`/`formula_registry_asof` and `v_warehouse_catalog`/`warehouse_catalog_asof` — the reader shape `asof.py` is built on — is preserved unchanged.
- **The schema-contract + drift gate (pf2-S1/S2).** `detect_schema_drift` and the catalog/PIT-presence checks stay green throughout; a refactor that moved a table or dropped a catalog row would be caught by them, which is the safety net.

---

## PIT / determinism + production contract

Clauses **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, **(D)** determinism + provenance, **(E)** schema-as-contract, and **(F)** backup-before-migrate all apply — but this sprint's overriding contract is **behavior preservation under regression lock**. It ships **no new behavior**; its acceptance is that the entire existing surface is provably unchanged.

- **(B)/(F)** Migrations **0138–0139** are reserved but expected to stay **unused** — the decomposition is a code move, not a schema change. They are drawn on *only* if a catalog row must physically relocate (e.g. a `table_catalog` seed moving with its migration body); any such migration is additive, idempotent, catalogued in the same migration, and preceded by a timestamped DB+WAL backup. No landed migration (≤ 0137) is renumbered or edited.
- **(C)** All boundary/coverage tests run offline over in-memory / template-copy DuckDB or over pure source/AST introspection. No network.
- **(D)** The public-API snapshot and import-graph analysis are pure, deterministic reads over the source tree and the imported modules — same tree → same snapshot.
- **(E)** `detect_schema_drift` and the catalog/PIT checks must return identical results pre- and post-split; a decomposition that perturbs the schema surface fails the drift gate immediately.
- The **checksum invariant** is the tightest constraint and is treated as a first-class production contract for this sprint: `verify_migration_checksums` must pass unchanged (see S3-0 and Risks).

---

## Tasks

### S3-0 — Decompose `migrations.py` into a `db/migrations/` package behind a stable `MIGRATIONS`

**Root cause:** 10,854 lines mix the migration **runner + checksum governance** (dataclass, `_migration_source_checksum`, `verify_migration_checksums`, apply-lock) with ~224 registry entries and ~200 inline `_*` body builders — the single hardest hub to edit safely, and the one carrying the sharpest invariant.

**Fix:** convert `db/migrations.py` into a package `db/migrations/`: a `_runner.py` (dataclass `Migration`, `_migration_source_checksum`, `verify_migration_checksums`, `_backfill_missing_migration_checksums`, `_validate_migration_registry`, apply-lock) and **per-range body modules** (e.g. `bodies_0001_0040.py`, … grouped by reserved range or domain), each holding the `Migration(...)` entries and their `_*` builders. A `registry.py` assembles the ordered `MIGRATIONS: list[Migration]` by concatenating the per-range lists in version order; `db/migrations/__init__.py` re-exports `MIGRATIONS` and the runner surface so `from db.migrations import MIGRATIONS` / `verify_migration_checksums` resolve unchanged. Proposed layout:

```
db/migrations/
  __init__.py         # re-exports MIGRATIONS + runner surface (stable interface)
  _runner.py          # Migration, checksum, verify, backfill, validate, apply-lock
  registry.py         # assembles ordered MIGRATIONS from per-range lists
  bodies_0001_0040.py # Migration(...) entries + their _* body builders
  bodies_0041_0088.py
  bodies_0089_0137.py
```

**Regression-lock** the applied version set (same 224 versions, same order, `_validate_migration_registry` green) and the pf2-S2 checksums. **Risk called out explicitly:** `_migration_source_checksum` hashes `inspect.getsource(up)` **plus** every direct module-level helper it resolves via `migration.up.__globals__` (recursively to depth 4). Splitting source therefore threatens the checksum on two axes — (1) the `up` body's normalized source text must move **byte-identical** (the hasher dedents/strips/normalizes newlines, so whitespace-only diffs are absorbed, but any real edit changes the hash), and (2) **each migration's `up` and all its direct helpers must live in the same module** so `up.__globals__` still resolves them; a helper stranded in another module drops out of the payload and silently changes the digest. Move each `up` together with its helper cluster into one body module; do not reflow bodies. The safest ordering is a mechanical, one-body-at-a-time move with `verify_migration_checksums` run after each batch, so a digest regression is localized to the last cluster moved.

**PIT:** (D) checksum is a pure function of co-located source — preserved by co-location. (B) 0138/0139 only if a catalog row relocates.

**Accept:** `from db.migrations import MIGRATIONS` yields the identical ordered 224-version list; `_validate_migration_registry` green; `verify_migration_checksums` passes against the live `schema_migrations.checksum` with **zero** mismatches; `test_migrations.py` + `test_migration_governance.py` green unchanged.

### S3-1 — Decompose `quality.py` into a `db/quality/` package behind a stable registry interface

**Root cause:** ~320 check literals live inside one `_check_specs()` function in a 7,255-line file; adding a domain's checks means appending into a shared tuple in a file no one can hold in context.

**Fix:** convert `db/quality.py` into `db/quality/`: a `_types.py` (`SqlQualityCheck`, `ReferentialQualityCheck`, `QualityResult`), a `_runner.py` (`run_warehouse_quality_checks`, `_main_objects`, `_passes`), and **per-domain check modules** (`checks_fundamental.py`, `checks_valuation.py`, `checks_estimates.py`, `checks_pricing.py`, `checks_catalog_pit.py`, …), each exposing a function returning its `tuple[SqlQualityCheck, ...]`. Proposed layout:

```
db/quality/
  __init__.py         # re-exports run_warehouse_quality_checks + dataclasses
  _types.py           # SqlQualityCheck, ReferentialQualityCheck, QualityResult
  _runner.py          # run_warehouse_quality_checks, _main_objects, _passes, _check_specs aggregator
  checks_fundamental.py / checks_valuation.py / checks_estimates.py
  checks_pricing.py / checks_ownership.py / checks_catalog_pit.py
```

`_check_specs()` becomes a thin aggregator concatenating the per-domain tuples in a stable order; `db/quality/__init__.py` re-exports `run_warehouse_quality_checks` and the dataclasses. All ~320 existing checks still register, in the same order, and run with identical `check_name` / `severity` / SQL — no semantics change. The stable order is load-bearing: two checks may share a `check_name`-adjacent detail ordering, so the aggregator must reproduce `_check_specs`'s original concatenation sequence exactly.

**PIT:** (C) offline over in-memory DuckDB fixtures. (G) `severity=critical` checks stay gate-wired; the orchestrator sees the identical set.

**Accept:** `run_warehouse_quality_checks` returns the identical set of `QualityResult`s (same `check_name`s, statuses, severities) on a fixture DB; the full check-name set is byte-identical to a pinned pre-split snapshot; `test_quality*.py` green unchanged.

### S3-2 — Decompose `asof.py` + `estimates.py` into packages behind stable public readers

**Root cause:** `asof.py` (4,244 lines, 50 SQL constants + 56 readers, all double-re-exported) and `estimates.py` (3,873 lines, 8 Datasets + a forest of column specs) each bundle many independent domains into one file.

**Fix:** convert `db/asof.py` into `db/asof/` grouped by domain (`fundamentals.py`, `pricing.py`, `estimates.py`, `ownership.py`, `identifiers.py`, …), each holding its `*_ASOF_SQL` constants and `*_asof` readers, with the shared helper core (`_normalize_strings`, `_register_filter`, `end_of_day_asof_ts`) in `_common.py`. `db/asof/__init__.py` re-exports **all 56 readers** so both `from db.asof import fundamentals_asof` **and** the `db/__init__.py` `from .asof import …` block keep resolving (the double import path is the tightest constraint here). Proposed layout:

```
db/asof/
  __init__.py    # re-exports all 56 *_asof readers (feeds db/__init__.py too)
  _common.py     # _normalize_strings, _register_filter, end_of_day_asof_ts, _month_end
  fundamentals.py / pricing.py / estimates.py / ownership.py / identifiers.py
```

Convert `db/estimates.py` into `db/estimates/` splitting column-spec/alias constants (`_columns.py`), compute helpers (`_compute.py`), and the eight `Dataset` classes (`datasets.py`); `db/estimates/__init__.py` re-exports the `Dataset` classes and option dataclasses. Proposed layout:

```
db/estimates/
  __init__.py    # re-exports the 8 Dataset classes + option dataclasses
  _columns.py    # ESTIMATE_*_COLUMNS, *_ALIASES, IBES_MEASURE_MAP, RECOMMENDATION_*
  _compute.py    # _compute_sue_series, series/typing helpers, _hash_id
  datasets.py    # EstimateMeasureSeedDataset, EstimateActualsDataset, EstimateSurpriseDataset, …
```

Re-export shims keep every landed import working. Note that `db/asof/estimates.py` (the estimate readers) and `db/estimates/` (the estimate Datasets) are distinct concerns and must not import each other's privates — the boundary lint (S3-3) enforces this.

**PIT:** (A) reader logic — availability gating, no lookahead — is moved verbatim, not rewritten. (D) pure SQL constants + readers move unchanged.

**Accept:** every one of the 56 `*_asof` readers imports from both `db.asof` and `db`; the eight estimate `Dataset` classes import from `db.estimates`; a reader smoke test (as-of gating on a fixture) returns identical rows pre/post-split; `test_asof*.py` + `test_estimates*.py` green unchanged.

### S3-3 — Module-boundary + import-graph lint gate (NEW)

**Root cause:** nothing prevents a decomposed package from re-tangling — a `checks_valuation.py` reaching into `checks_fundamental.py`'s private helpers, or an import cycle between `asof` sub-modules — which would erase the sprint's gains within a few content sprints.

**Fix:** NEW `db/module_boundaries.py` + `db/tests/test_module_boundaries.py`. The lint asserts, over the decomposed packages: (a) **no cross-package private imports** — a sub-module may import another package only through its `__init__` public surface, never its `_private` internals; (b) **no import cycles** — the intra-package import graph is a DAG; (c) **public-API surface unchanged** — the exported names of `db.migrations`, `db.quality`, `db.asof`, `db.estimates` (and the `db` re-exports) match a **pinned snapshot** captured before the split, so no symbol silently appears or disappears. The snapshot is generated deterministically from the modules' public `__all__`/dir surface and checked in as a fixture (e.g. `db/tests/data/public_api_snapshot.json`), regenerated only by an explicit, reviewed operation — never silently by the test. The import-graph analysis is a pure AST walk over the package source (parse each module, collect `import`/`from … import` targets, build the edge set), so it needs no live DB and no execution of migration bodies. The boundary rules are declared as data (allowed-package-dependency table) so a future content sprint that adds a legitimate new package edge does so by an explicit, reviewed rule change, not a silent lint relaxation.

**PIT:** (C) pure AST + import-graph analysis, offline, deterministic. (E) the API-surface snapshot is the enforceable contract that decomposition preserved every public name.

**Accept:** the boundary lint is green on the freshly decomposed tree; planting a cross-package private import, an import cycle, and a dropped public symbol each turns exactly one assertion red; the pinned public-API snapshot is byte-identical to the pre-split surface.

---

## Sequencing & expected compounding

**S3-0 → S3-1 → S3-2 → S3-3.** Decompose the sharpest, most invariant-laden hub first (`migrations.py` — the checksum risk is the gate on everything), then `quality.py`, then the `asof`/`estimates` pair, then stand up the boundary lint over all four now-decomposed packages last (it needs them to exist to pin their surfaces).

**Decomposition method (uniform per hub).** For each hub: (1) capture a pre-split public-API snapshot and a green baseline (`pytest -q` pass count + the hub's own governance tests); (2) create the package skeleton and re-export shims **first**, verifying imports resolve before any body moves; (3) move code in mechanical, behavior-preserving batches — bodies verbatim, no reflow — re-running the hub's tests after each batch so a regression is localized; (4) confirm the post-split public-API snapshot is byte-identical. This keeps every intermediate state importable and testable, so a bad move is caught immediately rather than at the end. Compounding: once the four hubs are packages behind stable interfaces **and** the boundary lint is a green gate, every remaining pf3 content sprint (S4–S12) extends a **focused module** — S5 appends estimate checks into `db/quality/checks_estimates.py`, S6 adds ratio readers into `db/asof/fundamentals.py`, S8 adds migration bodies into a per-range module — instead of appending into a 10K-line wall. The merge-conflict surface collapses (disjoint modules, disjoint diffs) and the boundary lint refuses any sprint that re-tangles the packages.

---

## Risks / guardrails

- **The checksum invariant is the sharpest risk.** pf2-S2 hashes each migration's `up` source plus its direct module-level helpers (resolved via `up.__globals__`, recursively to depth 4). If a body's normalized source text changes, or a helper is stranded in a different module than its `up`, `_migration_source_checksum` produces a different digest and `verify_migration_checksums` breaks against the live `schema_migrations.checksum`. **Mitigation:** move each `up` and its full helper cluster together into one body module (preserving `__globals__` resolution), move bodies **verbatim** (no reflow — the hasher absorbs whitespace/newline normalization but not real edits), and run `test_migrations.py` + `test_migration_governance.py` as the gate. If a body genuinely cannot move without a source change, the checksum-backfill path (`_backfill_missing_migration_checksums`) and its implications must be handled explicitly and recorded in the ledger — do not let it silently re-backfill.
- **The double import path on `asof` readers.** All 56 `*_asof` readers are re-exported through `db/__init__.py`; a split that preserves `db.asof.X` but forgets the `db.X` re-export breaks callers subtly. The public-API snapshot (S3-3) covers both surfaces.
- **Re-export shims prevent import breakage.** Every package `__init__` re-exports the full pre-split public surface; the pinned snapshot proves it.
- **This sprint changes NO behavior.** A full offline suite pass with **the same pass count** as pre-split is the acceptance — any delta in collected tests or results is a regression, not a refactor.
- **Migration/WAL safety.** 0138/0139 only if a catalog row relocates; timestamped DB+WAL backup before any live apply; strictly within the reserved range.
- **Circular-import hazard on package split.** Moving code into sub-modules can introduce import cycles that a monolith never had (e.g. `_runner` importing a body module that imports `_runner`). Keep the dependency direction strictly one-way — bodies depend on `_types`/`_common`, `registry`/`_runner` depend on bodies, `__init__` depends on everything — and let the S3-3 DAG check catch any accidental back-edge.
- **`db/__init__.py` is the widest blast radius.** Its `from .asof import …` block re-exports 56 readers by name; a rename or dropped export during the split breaks `from db import X` for downstream modules and tests silently at import time. Diff the `db` public surface against the pinned snapshot before merge.

---

## Bench / acceptance

- Full `python -m pytest atx-impl\db\tests -q` **green and unchanged** — identical pass count pre- and post-decomposition (the primary acceptance for a pure refactor).
- Module-boundary lint green: no cross-package private imports, no import cycles; planted violations each fail exactly one assertion.
- Public-API snapshot **identical** to the pre-split surface for `db.migrations` / `db.quality` / `db.asof` / `db.estimates` and the `db` re-exports.
- `verify_migration_checksums` passes with zero mismatches; `test_migrations.py` + `test_migration_governance.py` green; `detect_schema_drift` returns identical results pre/post.
- `run_warehouse_quality_checks` returns the identical `QualityResult` set; all 56 `*_asof` readers and the 8 estimate `Dataset` classes import from both their package and their landed path.
- `python -m pytest atx-impl\db\tests\test_module_boundaries.py -q` green before commit.
- `PARITY_GAP.md` updated (architecture-decomposition milestone; no parity content changed); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, decomposed modules, boundary-lint + checksum-verify commands, "no behavior change — refactor" caveat, next → PF3-S4).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
