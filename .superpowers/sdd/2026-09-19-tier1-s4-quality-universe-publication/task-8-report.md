# S4 Task 8 reconciliation report

Task: migration 0308 and deterministic full-universe release publication.
Reconciliation baseline: `cbd5488e`; shared branch `feat/tier1-parity`.

## Delivered

- Registered migration 0308 together with its body and private-module cleanup. It adds
  `publication_releases` and `publication_release_datasets`, catalogs their fields, and
  refreshes the schema-contract pin.
- Publication exports all rows and all physical columns of the six planned relations:
  security master, US-listed universe, delistings, standardized fundamentals, derived
  metrics, and daily market metrics. Historical revisions and multiple sources remain
  in the release. The security-master relation is the CUSIP-free public view.
- Every manifest includes explicit query text, exported column metadata, release keys,
  schema/record-contract/query/Parquet hashes, row and byte counts, latest completed
  activation-stage run IDs, and added/removed/changed counts against verified prior
  Parquet artifacts. Its UTF-8 bytes have a warehouse-ledger SHA256.
- Exports, activation provenance, and ledger writes share one transaction/snapshot.
  Output is prepared in a temporary sibling directory and renamed once complete.
  Release IDs and existing output directories are immutable; ordinary export or commit
  failures roll back the ledger and remove only the newly generated output.
- Exact diffs use NULL-safe key joins and a JSON row digest, preserving NULL positions
  and embedded separators. Duplicate keys and incompatible column/type changes fail
  explicitly. Missing or tampered prior manifest-declared files also fail explicitly.
- Exposes `atx-db publish-release` and the matching script wrapper. No publication or
  migration was performed against the live warehouse.

## Reconciled plan details

- The controller confirmed the later no-library-clock ruling supersedes the old plan's
  load-stamp exception. `publish_release` therefore requires `created_at`; the CLI
  supplies UTC at its boundary. Aware stamps are normalized to naive UTC and also used
  for publication-ledger load stamps.
- Delistings use `delisting_event_id` for release keys. The plan's nullable logical key
  could collapse different unresolved symbols or fan out a diff join. The public schema
  reference is unchanged; release keys are independently recorded in the manifest.
- COPY pins one writer thread, insertion-order preservation, compression and row-group
  size, then restores caller settings. This makes data bytes stable across caller
  thread/order settings with the same DuckDB version. SQL paths are escaped literals.
- Manifests retain the planned fields and additionally store the exact query, column
  metadata and keys so their hashes and diff contract are independently inspectable.

## Validation

- PASS: `.venv/Scripts/python.exe -m pytest tests/test_publication.py
  tests/test_migration_governance.py::test_bootstrap_records_checksum_for_every_migration
  -n 0 -q` — 25 passed, exit 0. This used the normal fresh content-fingerprinted 0308
  schema template (`751427869a276f4429239656`). No failures or reruns.
- Covers all six populated datasets (including unresolved delistings and historical
  revisions), Parquet round-trip, manifest/query/schema/file hashes, zero/change diffs,
  JSON digest NULL/separator handling, nullable keys, duplicate/schema rejection,
  missing/tampered predecessors, path confinement, release immutability, export/commit
  rollback, concurrent-writer snapshot consistency, completed-stage provenance, and
  identical Parquet bytes over 260,000 rows across different thread/order settings.
- PASS: touched-file `ruff check` on publication, migration 0308, migration registry and
  facade, CLI, script wrapper and publication tests.
- PASS: `mypy --strict src/atx_db/publication.py
  src/atx_db/migrations/bodies_0308.py` — no issues in two source files.
- PASS: `import atx_db` with migration head 308; `scripts/publish_release.py --help`.
- Per controller, module-boundary/schema-contract whole-file checks are owned by T4 and
  the controller. No full suite or live-warehouse command was run.

## Limits and operator follow-up

- Production activation, measured gates, and the first live release remain controller
  work. An empty dataset can be exported; publication does not claim measured coverage.
- Filesystem rename and DuckDB commit cannot form a cross-resource atomic transaction.
  Normal exceptions are cleaned up, but process/power loss in the rename/commit window
  can leave an unrecorded complete release directory requiring operator reconciliation.
- Exact comparisons require matching exported column names/types. A schema change
  requires a new baseline release without `previous_dir`.
- Byte reproducibility is guaranteed within the same DuckDB/Parquet writer version,
  not across future library versions. Large-table diffs need a full key/hash scan.
- Existing historical revision/load-stamp changes are intentionally visible in diffs,
  since comparisons cover every exported field rather than economic values alone.

Registry lock is released with the task commit report to the controller.
