# Sprint PF4-S7 — Panel release engine (immutable semver releases + Arrow/Feather + pinnable retention)

**Track:** Product (Track C). **Goal:** turn the ephemeral, UUID-keyed panel export into an
**immutable, versioned, checksummed panel _release_ product**. Today `db/factor_panel.export_factor_panel`
runs the S10 export gate and writes `v_factor_panel` to `db/lake/<export_run_id>/…` — a raw-UUID directory
with a `_manifest.json` but **no version identity, no changelog, no Arrow output, and no protection against a
retention prune**. This sprint lands `db/panel_release.py::publish_release(version="YYYY.MM.patch")`, which
gates → exports the panel **and its declared companion objects** to a directory **keyed by SEMVER**, emits a
**release manifest** (version, git_sha, `panel_contract_sha256`, per-file sha256, row/byte counts, source
watermarks) plus a **generated CHANGELOG** (factor add/remove + row deltas vs the prior release), and is
**content-addressed** so re-publishing identical inputs is a **no-op** (clause **K**). Alongside, `db/lake.py`
gains an **Arrow/Feather** writer beside the existing Parquet+ZSTD path (**both emitted and validated**), and
retention is extended so it **NEVER prunes a pinned release**. Reserved migrations **0195–0197**.

**Mandate / Owns:** NEW `db/panel_release.py` (the release engine: `publish_release`, `pin_release` /
`unpin_release`, `prune_releases`, `validate_release`, a `read`/`publish`/`pin`/`prune` CLI); the Arrow/Feather
writer + validation added **inside** `db/lake.py`'s existing `LakehouseExporter` / `validate_lake_export`
(append-only, opt-in flag — the 100+ existing `DEFAULT_EXPORT_OBJECTS` are untouched); migrations
**0195–0197** (`db/migrations/bodies_0195_0197.py` + registry wiring); and `db/tests/test_panel_release.py`.

**Must NOT touch:** the S10 `panel_contract` **definition** (`db/panel_contract.py`) or the S10 export gate
logic (`db/factor_panel.py::factor_panel_export_gate_report` / `assert_factor_panel_export_ready`) — S7
**reuses** the gate at the release boundary, it does not re-author or weaken it; the S8/S9 factor engines
(`db/factors/`) — S7 **consumes** the landed `v_factor_panel` namespace; any landed migration (≤ 0194) or
another sprint's reserved region; the existing single-format export behaviour of the 100+
`DEFAULT_EXPORT_OBJECTS` (Arrow is **opt-in**, default off, so `test_factor_panel.py` and every other
lake-export test stay byte-identical green).

**Depends on:** PF3-S10 (`v_factor_panel` + `v_factor_panel_wide` catalogued views, the export gate,
`lake.py`'s `LakehouseExporter` / `_schema_sha256` / `_manifest.json` / `_directory_sha256` /
`validate_lake_export` machinery, `panel_contract.PANEL_CONTRACT_SHA256`), and `db/jobs.py::current_git_sha`.
Sequential **after** PF4-S6; PF4-S8 (the `atx-panel` SDK) then **reads** the releases S7 publishes (release
pinning is the SDK's `release=` argument), and PF4-S10 fronts them. pf4 uses migrations from **0176**; S1–S6
claim **0176–0194**; S7 owns **0195–0197** (reconcile the body-file import order in `registry.py` to whatever
S1–S6 landed).

---

## Baseline / where the cycles go

The panel export is real and gated, but it is a *build artifact*, not a *release*. Measured 2026-07-06 against
`atx-impl/db`.

1. **Exports are ephemeral, UUID-keyed, and un-versioned.** `LakehouseExporter._export_partitioned_object`
   (`lake.py:661`) writes partitioned objects to `self.lake_root / export_run_id / object_name`, and
   `_safe_prune_run_dirs` (`lake.py:345`) *only* recognises directories whose name parses as a `uuid.UUID`.
   `export_factor_panel` (`factor_panel.py:742`) hands the panel to that path — so a "release" is a random
   UUID folder with no `YYYY.MM.patch` identity, no `git_sha`, no changelog, and nothing that lets a
   downstream quant team **pin** a reproducible version. There is no release registry table anywhere.

2. **Arrow is claimed but absent — only Parquet+ZSTD is written.** A grep of `db/` for an Arrow/Feather writer
   returns **nothing**; the only columnar writers are the two `COPY … (FORMAT PARQUET, COMPRESSION ZSTD)`
   statements at `lake.py:575` and `lake.py:696`. The northstar promises "pandas **or zero-copy Arrow** out";
   the SDK (PF4-S8) needs a zero-copy Arrow entry point that does not exist yet. `pyarrow==18.0.0` is already a
   pinned dependency (`requirements.txt:3`), so the writer is buildable offline today.

3. **Retention would happily delete an important release.** `export_objects` (`lake.py:439`) computes `keep`
   from `lake_partition_specs.retention_runs` and calls `_safe_prune_run_dirs(lake_root, keep_run_ids)`, which
   `shutil.rmtree`s every UUID dir not in the newest-N set. There is **no `pinned` concept** — a curated,
   downstream-pinned release directory has exactly the same standing as a throwaway build and would be pruned.
   Clause **K** ("a pinned release is never mutated or pruned") has **no substrate**.

4. **No content-addressing / no-op semantics.** Re-running `export_factor_panel` always mints a fresh
   `export_run_id` and a fresh directory even when the panel content is byte-for-byte identical. There is no
   fingerprint that says "this is the same release; do nothing." Clause **K** ("re-publishing identical inputs
   is a no-op") is unmet.

**Already good — do not regress:**
- **`lake.py`'s schema-hash + manifest + directory-hash discipline.** `_schema_sha256` (`lake.py:185`),
  `_object_schema` (`lake.py:190`), the per-object `_manifest.json`, `_directory_sha256` (`lake.py:335`), and
  the `expected_schema_sha256` re-check in `validate_lake_export` are the exact contract S7 reuses — the Arrow
  writer slots **beside** the Parquet writer in the same manifest, never a parallel one.
- **The S10 export gate.** `assert_factor_panel_export_ready(store)` (`factor_panel.py:564`) already raises on
  contract/lookahead/non-member violations; `publish_release` calls it **before** writing a single byte
  (clause **G**/**I** reuse), so a bad panel aborts the release exactly as it aborts an export.
- **`current_git_sha`.** `db/jobs.py:1412` already shells `git -C <root> rev-parse HEAD` with a `None`
  fallback and an injectable `repo_root`; the release manifest reuses it (injectable in tests → offline).

---

## PIT / determinism + production contract

Clauses **(B)** append-only catalogued migrations, **(E)** schema-as-contract, **(G)** quality-gated,
**(I)** panel PIT-safety, and the **new (K) release immutability** all apply — and **(K) is the clause this
sprint introduces and enforces.**

- **(K) Release immutability.** A published release is **content-addressed**: its `content_hash` is a
  deterministic in-DB digest over `(panel_contract_sha256, per-object schema_sha256, per-object row count,
  per-object canonical data digest)`. `publish_release` computes the digest **first**; if a `panel_release`
  row already carries that `content_hash`, it returns that release as a **no-op** — no new directory, no new
  registry row (enforced by a `UNIQUE` index on `content_hash`). Re-publishing a *different* content under an
  *existing* version **raises** (a version dir is never mutated). A **pinned** release is never pruned or
  overwritten.
- **(I) PIT-safety at the release boundary.** `publish_release` runs `assert_factor_panel_export_ready(store)`
  before export — the S10 lookahead/contract/universe gate is the release's admission control. A planted
  future-dated input turns the gate red and **aborts the release** (no dir, no registry row).
- **(G) Quality-gated.** The critical `factor_panel_export_contract` check is the gate above; the release
  cannot be published while it is red. Retention/pin operations never bypass it.
- **(E) Schema-as-contract.** Every new table (`panel_release`, `release_file`, `panel_release_retention_config`)
  and the `v_panel_release_latest` view seeds a `table_catalog` + `field_catalog` row via
  `_catalog_fields_for_tables(conn, …)` and re-pins schema-contract-v2 via `_refresh_schema_contract_v2_pin(conn)`
  **in the same migration** (the S10 pattern in `bodies_0164_0167.py`). The panel's `schema_sha256` and
  `PANEL_CONTRACT_SHA256` are recorded in the manifest and re-checked on `validate_release`.
- **(B) Append-only catalogued migrations.** Migrations **0195–0197** only, schema split from index, backup
  before any live apply:
  - **0195** — `panel_release` registry + `release_file` per-file manifest tables + catalog + `dataset_catalog`.
  - **0196** — `v_panel_release_latest` view + catalog.
  - **0197** — indexes (incl. the `content_hash` UNIQUE index), `panel_release_retention_config` +
    pinned-retention config, and the consumer-facing catalog polish.
- **(D) Determinism.** The content digest is order-independent (aggregated over `to_json(t)` sorted by the
  JSON text); the manifest is `json.dumps(sort_keys=True)`; the same inputs + version → identical
  `content_hash` and identical manifest modulo `published_at`/`git_sha`.

---

## Tasks

### S7-0 — Arrow/Feather writer + validation in `db/lake.py` (both formats emitted, both validated)

**Root cause:** the roadmap/design promise "Parquet **+ Arrow**" but a grep of `db/` finds only two
`COPY … (FORMAT PARQUET, COMPRESSION ZSTD)` writers (`lake.py:575`, `lake.py:696`) and **no** Arrow/Feather
writer. The SDK's zero-copy Arrow entry point has nothing to read.

**Fix (TDD — write the failing test first):** add an **opt-in** Arrow/Feather writer to `LakehouseExporter`,
threaded through both file paths without disturbing existing callers.

- Add `emit_arrow: bool = False` to `LakehouseExporter.export_objects` and pass it into
  `_export_single_file_object` and `_export_partitioned_object`. Default `False` keeps the 100+
  `DEFAULT_EXPORT_OBJECTS` single-format and every existing manifest byte-identical (no regression to
  `test_factor_panel.py::test_factor_panel_exports_partitioned_lake_object_with_schema_contract`).
- After each Parquet part is written, when `emit_arrow` is set, write a Feather-v2 sibling:

  ```python
  import pyarrow.feather as feather
  arrow_path = output_path.with_name("part-00000.arrow")
  table = store.con.execute(f"SELECT * FROM {quoted_object_name} WHERE {predicate}").fetch_arrow_table()
  feather.write_feather(table, str(arrow_path), compression="zstd")
  arrow_byte_count = arrow_path.stat().st_size
  arrow_sha256 = file_sha256(arrow_path)
  ```

  (For the single-file object, drop the `WHERE {predicate}`.) `pyarrow==18.0.0` is already pinned; guard the
  import with a clear `RuntimeError("Arrow export requires pyarrow")` if it is ever absent.
- Record Arrow alongside Parquet in the manifest: each partitioned `files[]` entry gains `arrow_path`,
  `arrow_sha256`, `arrow_byte_count`; the single-file and top-level manifest gains the same keys plus
  `"formats": ["parquet", "arrow"]`. The Parquet keys are unchanged.
- Extend `validate_lake_export` so that when a manifest carries Arrow entries it also validates each `.arrow`
  file: read it back with `pyarrow.feather.read_table(path)`, and assert `num_rows`, `st_size`, and
  `file_sha256` equal the manifest's `arrow_*` fields — reusing the existing `_problem(...)` records
  (new problem kinds `missing_arrow`, `arrow_sha256_mismatch`, `arrow_byte_count_mismatch`,
  `arrow_row_count_mismatch`). Validation stays read-only (pure pyarrow read; no DB mutation).

**PIT:** (E) the Arrow file carries the same schema/rows as the Parquet part — `schema_sha256` unchanged; (D)
Feather bytes are deterministic for identical input + pyarrow version.

**Accept:** with `emit_arrow=True`, each partition dir holds `part-00000.parquet` **and** `part-00000.arrow`;
the manifest lists both with distinct sha256/byte counts; `validate_lake_export` returns `problems == []` and
its `files_readable` counts the Arrow files; `pyarrow.feather.read_table` round-trips the row count. With
`emit_arrow=False` (default) the export and every existing test are byte-identical to today.

### S7-1 — `panel_release` + `release_file` tables + `v_panel_release_latest` view (migrations 0195 / 0196)

**Root cause:** there is no registry that records a semver'd, checksummed, pinnable release or its per-file
manifest, and no "latest release" surface a consumer/SDK can point at. Clause **K** has no substrate.

**Fix:** create `db/migrations/bodies_0195_0197.py` (importing `_catalog_fields_for_tables` from
`bodies_0001_0137` and `_refresh_schema_contract_v2_pin` from `bodies_0140_0143`, mirroring
`bodies_0164_0167.py`) and append its `MIGRATIONS` into `registry.py` after the highest landed pf4 body file.

- **Migration 0195** — `_pf4_s7_panel_release_registry(conn)`:

  ```sql
  CREATE TABLE IF NOT EXISTS panel_release (
      release_version         VARCHAR PRIMARY KEY,        -- 'YYYY.MM.patch'
      content_hash            VARCHAR NOT NULL,           -- deterministic in-DB digest (clause K)
      panel_contract_sha256   VARCHAR NOT NULL,
      git_sha                 VARCHAR,
      release_dir             VARCHAR NOT NULL,
      manifest_path           VARCHAR NOT NULL,
      changelog_path          VARCHAR NOT NULL,
      object_count            INTEGER NOT NULL,
      file_count              INTEGER NOT NULL,
      row_count               BIGINT  NOT NULL,
      byte_count              BIGINT  NOT NULL,
      factor_count            INTEGER NOT NULL,
      source_watermarks_json  VARCHAR NOT NULL DEFAULT '{}',
      prior_release_version   VARCHAR,
      pinned                  BOOLEAN NOT NULL DEFAULT false,
      superseded              BOOLEAN NOT NULL DEFAULT false,
      status                  VARCHAR NOT NULL DEFAULT 'published',
      published_at            TIMESTAMP NOT NULL DEFAULT now(),
      updated_at              TIMESTAMP NOT NULL DEFAULT now()
  );
  CREATE TABLE IF NOT EXISTS release_file (
      release_version  VARCHAR NOT NULL,
      object_name      VARCHAR NOT NULL,
      format           VARCHAR NOT NULL,     -- 'parquet' | 'arrow'
      partition_key    VARCHAR,              -- JSON partition key, or NULL for single-file
      relative_path    VARCHAR NOT NULL,     -- path relative to release_dir (semver-rooted)
      absolute_path    VARCHAR NOT NULL,
      rows             BIGINT  NOT NULL,
      byte_count       BIGINT  NOT NULL,
      sha256           VARCHAR NOT NULL,
      schema_sha256    VARCHAR NOT NULL,
      exported_at      TIMESTAMP NOT NULL DEFAULT now(),
      PRIMARY KEY (release_version, object_name, format, relative_path)
  );
  ```

  Seed `dataset_catalog` (`dataset_id='panel_release'`, `primary_table='panel_release'`,
  `pit_column='published_at'`, `available_at_column='published_at'`) and two `table_catalog` rows
  (`panel_release`, `release_file`, `layer='control'`) exactly like the S10 `lake_export_object_contract` row,
  then `_catalog_fields_for_tables(conn, ("panel_release", "release_file"))` and
  `_refresh_schema_contract_v2_pin(conn)`.

- **Migration 0196** — `_pf4_s7_panel_release_latest_view(conn)`:

  ```sql
  CREATE OR REPLACE VIEW v_panel_release_latest AS
  SELECT *
  FROM panel_release
  WHERE NOT superseded
    AND status = 'published'
  QUALIFY row_number() OVER (
      ORDER BY published_at DESC, release_version DESC
  ) = 1;
  ```

  Catalogue the view (`table_catalog` `layer='view'`, natural key `["release_version"]`, PIT notes: "latest
  non-superseded published panel release; SDK reads this when `release=` is unset"), then
  `_catalog_fields_for_tables(conn, ("v_panel_release_latest",))` and `_refresh_schema_contract_v2_pin(conn)`.

**PIT:** (B) 0195 tables + 0196 view each catalogued in the same migration; schema split from index (indexes
land in 0197); (E) contract + catalog rows land with the tables.

**Accept:** after `apply_pending_migrations`, `panel_release`, `release_file`, and `v_panel_release_latest`
exist and are catalogued; a `db/tests/test_panel_release.py` migration test asserts
`(SELECT count(*) FROM table_catalog WHERE table_name IN ('panel_release','release_file','v_panel_release_latest')) == 3`
and that `missing_table_catalog_entries` / `missing_field_catalog_entries` quality checks stay 0 on the
`tmp_store` fixture.

### S7-2 — `publish_release()` — semver dir + manifest + changelog + content-addressed no-op

**Root cause:** nothing publishes a versioned, checksummed, immutable release with a changelog; `export_factor_panel`
mints a random-UUID dir with no version identity, no diff, and no dedupe.

**Fix (TDD):** implement `db/panel_release.py`:

```python
DEFAULT_RELEASE_ROOT = Path(__file__).resolve().parent / "releases"
DEFAULT_RELEASE_OBJECTS = ("v_factor_panel", "v_factor_panel_wide")  # panel + declared companions
_VERSION_RE = re.compile(r"^\d{4}\.(0[1-9]|1[0-2])\.\d+$")           # YYYY.MM.patch

@dataclass(frozen=True)
class PanelRelease:
    release_version: str
    release_dir: Path
    manifest_path: Path
    changelog_path: Path
    content_hash: str
    git_sha: str | None
    row_count: int
    byte_count: int
    factor_count: int
    file_count: int
    pinned: bool
    reused: bool           # True when an identical prior release was returned (no-op)

def publish_release(
    version: str,
    *,
    db_path: Path | str = DEFAULT_DB_PATH,
    release_root: Path | str = DEFAULT_RELEASE_ROOT,
    objects: tuple[str, ...] = DEFAULT_RELEASE_OBJECTS,
    pin: bool = False,
    git_sha: str | None = None,        # injectable for offline/deterministic tests
) -> PanelRelease: ...
```

Sequence inside `publish_release`:
1. Validate `version` against `_VERSION_RE`; raise `ValueError` on a non-`YYYY.MM.patch` string.
2. Open the store (`connect(db_path, read_only=False)`) and call `assert_factor_panel_export_ready(store)`
   — the S10 gate. A red gate raises before any byte is written (clause **G**/**I**).
3. Compute `content_hash = _panel_content_digest(store, objects)`: for each object in **sorted** order,
   `schema_sha256` (reuse `lake._object_schema` + `lake._schema_sha256`), `SELECT count(*)`, and an
   order-independent data digest
   `SELECT coalesce(md5(string_agg(to_json(t), chr(10) ORDER BY to_json(t))), '') FROM {object} t`; hash the
   `json.dumps({"panel_contract_sha256": PANEL_CONTRACT_SHA256, "objects": [...]}, sort_keys=True,
   separators=(",",":"))` with `hashlib.sha256`.
4. **Content-address / no-op (clause K):** `SELECT release_version FROM panel_release WHERE content_hash = ?`.
   If a row exists, return `PanelRelease(reused=True, ...)` for that existing release — **no new directory, no
   new registry row, no changelog rewrite**. (If `pin=True`, flip that existing release's `pinned` and return.)
5. **Immutability guard:** if `<release_root>/<version>/` already exists **or** a `panel_release` row for
   `version` exists with a **different** `content_hash`, raise
   `ValueError(f"release {version} already published with different content")` — a version dir is never
   mutated.
6. **Export both formats to the semver dir.** Point the exporter at the version root and emit Arrow:
   `LakehouseExporter(db_path=db_path, lake_root=Path(release_root)/version).export_objects(objects,
   incremental=False, emit_arrow=True)`. The physical parquet+arrow parts land under
   `<release_root>/<version>/…`; record every part in `release_file` with `relative_path` computed
   relative to `<release_root>/<version>/` (semver-rooted, never a bare UUID in the release identity).
7. **Emit `_release_manifest.json`** at `<release_root>/<version>/_release_manifest.json`:
   `{version, git_sha (or current_git_sha()), panel_contract_sha256, content_hash, published_at,
   object_schemas:{object: schema_sha256}, files:[{object_name, format, relative_path, sha256, byte_count,
   rows}], row_count, byte_count, factor_count, file_count, source_watermarks, pinned}`.
   `source_watermarks` per object = `SELECT max(available_at), max(as_of_date) FROM {object}`.
8. **Generate `CHANGELOG.md`** at `<release_root>/<version>/CHANGELOG.md` via `_generate_changelog(...)`:
   diff vs the prior release (the most recent existing `panel_release` by `published_at`, recorded as
   `prior_release_version`). Compute the panel factor set as `SELECT DISTINCT factor_id FROM v_factor_panel`;
   report **factors added**, **factors removed**, **row delta** (`row_count` vs prior), **byte delta**, and a
   **contract-hash change** flag. For the first release, the changelog reads `Initial release.` with the full
   factor list. Persist the structured diff into the manifest too.
9. Insert the `panel_release` row (with `pinned=pin`) and `release_file` rows in one transaction; mark the
   prior release `superseded=true`. Return `PanelRelease(reused=False, ...)`.

Add a `main(argv)` CLI: `python -m db.panel_release publish --version 2026.07.0 [--pin]`,
`… read [--version …]` (prints manifest JSON; defaults to `v_panel_release_latest`), `… pin --version …`,
`… prune --retain N`.

**PIT:** (K) content-addressed no-op + immutability guard; (I)/(G) gate runs before write; (D) digest +
manifest deterministic.

**Accept:** `publish_release("2026.07.0", …, git_sha="deadbeef")` on the `tmp_store` fixture creates
`<release_root>/2026.07.0/` containing `_release_manifest.json`, `CHANGELOG.md`, and `v_factor_panel/` +
`v_factor_panel_wide/` parquet+arrow parts; the manifest's `content_hash` equals the `panel_release` row's;
every `release_file.sha256` equals the on-disk `file_sha256`; a second identical `publish_release("2026.07.0")`
returns `reused=True` with the **same `content_hash`, no new dir, and `SELECT count(*) FROM panel_release == 1`**;
`publish_release("2026.07.0")` after mutating the panel raises the immutability error; a planted future-dated
input (the S10 adversarial fixture) makes `publish_release` raise `factor panel export gate failed` and writes
nothing.

### S7-3 — Pin / unpin + pinned-safe retention prune + indexes/config (migration 0197)

**Root cause:** `lake.py`'s retention (`_safe_prune_run_dirs`) prunes by UUID recency with no `pinned`
concept — a curated, downstream-pinned release would be deleted. Clause **K** ("a pinned release is never
pruned") is unmet, and there is no config governing release retention.

**Fix:**
- **Migration 0197** — `_pf4_s7_panel_release_retention_config(conn)`:

  ```sql
  CREATE INDEX IF NOT EXISTS idx_panel_release_pinned_published ON panel_release(pinned, published_at);
  CREATE UNIQUE INDEX IF NOT EXISTS ux_panel_release_content_hash ON panel_release(content_hash);
  CREATE INDEX IF NOT EXISTS idx_release_file_version ON release_file(release_version);
  CREATE TABLE IF NOT EXISTS panel_release_retention_config (
      config_id        VARCHAR PRIMARY KEY DEFAULT 'default',
      retain_unpinned  INTEGER NOT NULL DEFAULT 5,
      enabled          BOOLEAN NOT NULL DEFAULT true,
      updated_at       TIMESTAMP NOT NULL DEFAULT now()
  );
  INSERT OR REPLACE INTO panel_release_retention_config (config_id, retain_unpinned, enabled, updated_at)
  VALUES ('default', 5, true, now());
  ```

  Catalogue `panel_release_retention_config`, then `_catalog_fields_for_tables(...)` +
  `_refresh_schema_contract_v2_pin(conn)`. (The `ux_panel_release_content_hash` UNIQUE index is the DB-level
  backstop for the S7-2 no-op contract.)
- **`pin_release(version, *, db_path, store=None)` / `unpin_release(version, …)`** — flip
  `panel_release.pinned` and `updated_at`; raise `ValueError` if the version is unknown.
- **`prune_releases(*, retain=None, db_path, release_root, store=None)`** — read `retain_unpinned` from
  `panel_release_retention_config` when `retain is None`; select **unpinned** releases ordered by
  `published_at DESC`, keep the newest `retain`, and for each older unpinned release `shutil.rmtree` its
  `release_dir` (guarded to stay under `release_root.resolve()`) and delete its `panel_release` +
  `release_file` rows. **Pinned releases are excluded from the candidate set entirely** — they are never
  counted against the retention window and their directories are never touched. Return the list of pruned
  versions.

**PIT:** (K) pinned-safe retention is the clause's teeth; (B) 0197 indexes/config catalogued; the prune path
never runs the export gate and never mutates a surviving release's bytes.

**Accept:** publish three unpinned releases + one pinned older release; `prune_releases(retain=1)` deletes the
two oldest **unpinned** dirs + rows, keeps the newest unpinned, and **keeps the pinned release's dir + row
regardless of age**; `SELECT pinned FROM panel_release` shows the pinned survivor; the pinned release's
`_release_manifest.json` is unchanged on disk (byte-identical) after the prune.

### S7-4 — Suite green + live proof-slice + ledger/parity closeout

**Root cause:** a release engine is only trustworthy when the full offline suite is green, a live proof-slice
is recorded, and the parity ledger reflects clause **K** landing.

**Fix:**
- Run `python -m pytest atx-impl\db\tests\test_panel_release.py -q` **and** the full
  `python -m pytest atx-impl\db\tests -q` green **from `atx-impl/`** (never from `db/` — `db/calendar.py`
  shadows stdlib `calendar`). Confirm `test_factor_panel.py` and every existing lake-export test remain green
  (Arrow is opt-in; existing manifests byte-identical).
- **Live proof-slice** (operator-run on the shared DB per the pf4 gated posture, recorded in the ledger):
  publish one release from a bounded slice and record — `release_version`, `content_hash`, `git_sha`,
  `factor_count`, `row_count`, `byte_count`, `file_count`, the parquet **and** arrow sha256 set, the
  `validate_release` result (0 problems), and a re-publish showing `reused=True` (no new dir).
- **Docs:** update `db/PARITY_GAP.md` — record clause **K** enforced (immutable, content-addressed, checksummed
  semver releases with Arrow+Parquet + pinned-safe retention); note Domain 6 pricing/Product surface now
  exposes a versioned release product. Append a `WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, domains =
  panel release engine + lake Arrow writer, verification commands, live proof-slice counts + `content_hash` +
  release_version, caveats/next → PF4-S8 `atx-panel` SDK reads these releases via `release=`).

**PIT:** (B)/(E)/(K) all evidenced; (C) offline suite green, live smoke operator-run and recorded.

**Accept:** both pytest commands green from `atx-impl/`; the ledger row carries exact live counts + the
release `content_hash`; `PARITY_GAP.md` reflects clause **K**.

---

## Sequencing & expected compounding

**S7-0 → S7-1 → S7-2 → S7-3 → S7-4.** S7-0 lands the Arrow/Feather writer + validation in `lake.py` (the dual
format the release must emit). S7-1 lays the `panel_release` / `release_file` registry + latest view (the
identity/manifest substrate). S7-2 then implements `publish_release` — gate → dual-format export to a semver
dir → manifest + changelog → content-addressed no-op — which needs both the writer (S7-0) and the registry
(S7-1). S7-3 bolts pin/unpin + pinned-safe retention + the `content_hash` UNIQUE backstop onto that registry.
S7-4 greens the suite and flips the ledger. **Compounding:** the immutable, semver'd, Arrow+Parquet release is
the exact artifact **PF4-S8**'s `atx-panel` SDK pins (`read_panel(..., release="2026.07.0")`), **PF4-S9**'s
data dictionary documents, and **PF4-S10**'s served tier fronts — clause **K** is the contract the whole
Product track relies on.

## Risks / guardrails

- **The no-op must be a true no-op.** Compute `content_hash` **before** touching disk; if it matches an
  existing release, return it — do not write a staging dir, do not mint a registry row. The
  `ux_panel_release_content_hash` UNIQUE index is the DB backstop; the Python check is the fast path.
- **A version dir is immutable.** Re-publishing *different* content under an existing version must **raise**,
  never overwrite. Only an identical-content re-publish is allowed (as a no-op).
- **Pinned is sacred.** `prune_releases` must exclude pinned releases from the candidate set entirely — never
  count them against `retain`, never `rmtree` their dir. Guard every `rmtree` to stay under
  `release_root.resolve()` (mirror `_safe_prune_run_dirs`).
- **Arrow is opt-in.** `emit_arrow` defaults `False`; the 100+ `DEFAULT_EXPORT_OBJECTS` and every existing
  lake-export test stay byte-identical. Only `publish_release` passes `emit_arrow=True`.
- **Do not weaken the S10 gate.** Reuse `assert_factor_panel_export_ready` unchanged; the release boundary is
  strictly stricter than the export boundary, never looser.
- **Stay in lane.** Do not edit `panel_contract.py`, the gate in `factor_panel.py`, or any migration ≤ 0194.
  Strictly migrations **0195–0197**; DB+WAL backup before any live apply.

## Bench / acceptance

- `db/lake.py` emits **and validates** Arrow/Feather beside Parquet+ZSTD when `emit_arrow=True`; default-off
  keeps every existing export byte-identical.
- `publish_release("YYYY.MM.patch")` gates → writes an **immutable semver dir** with `_release_manifest.json`
  (version, git_sha, `panel_contract_sha256`, per-file sha256, row/byte counts, source watermarks) +
  `CHANGELOG.md` + per-file parquet **and** arrow parts recorded in `release_file`.
- Re-publishing identical inputs is a **no-op**: same `content_hash`, `reused=True`, **no new dir**, one
  `panel_release` row.
- The changelog reflects a **factor added** between two releases (added/removed factor sets + row delta).
- A **pinned release survives** a `prune_releases` that removes an unpinned one; its manifest is byte-identical
  after the prune.
- **Determinism:** two identical publishes yield an identical `content_hash` and identical manifest modulo
  `published_at`/`git_sha`.
- `python -m pytest atx-impl\db\tests\test_panel_release.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green (run from `atx-impl/`) before commit.
- **Live proof-slice** recorded in the ledger: `release_version`, `content_hash`, `git_sha`, `factor_count`,
  `row_count`, `byte_count`, `file_count`, parquet+arrow sha256 set, `validate_release` = 0 problems, and a
  `reused=True` re-publish.
- `db/PARITY_GAP.md` updated (clause **K** enforced; versioned release product landed); a
  `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live proof-slice
  with exact counts + `content_hash` + release_version, caveats/next → PF4-S8 SDK).

**Process:** own git worktree off `main`, merged at sprint end via
`atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller `superpowers:subagent-driven-development`
(fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage
explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
