# CC1 — bounded candidate target-index repair

## Production acceptance

Restore the existing nonunique
`idx_identifier_resolution_candidates_target` without changing schema version,
candidate rows, candidate IDs, provenance, constraints, or other indexes.
`candidate-index-inspect1.json` proves that candidate
`5b162a89-35e4-58eb-bfb5-ae11932bb2d4` at target
`SEC-COMPANYFACTS-UNRESOLVED-CIK-0001495229` is present in the table but omitted by
the current target index. Archive17 therefore failed at candidate COMMIT even
though it stayed within its memory cap.

The repair must make the default target IndexScan and forced sequential query
return the same candidate after checkpoint/reopen. Every candidate row and the
entire existing column/constraint/index contract must remain identical. A later
full archive18 resume from `dd52e571-5786-42c5-bfaa-d7122b033912` must prove the
source write path; successful repair alone does not complete source ingestion.

## Scope and resource ownership

Owned files are `rebuild_candidate_target_index.py`, this brief,
`candidate-index-repair-report.md`, and
`atx-db/tests/test_candidate_index_repair.py`. No production package, migration,
registry, source-loader, or schema edits are authorized. Root runs all runtime
commands serially under the existing memory guard and obtains one independent
review before live mutation. The implementer runs no Python, tests, or live SQL.

Connections receive a 256 MB or 512 MB limit and one thread at opening. Python
row hashing consumes 256-row batches. Preserve all twelve full warehouse
backups and `stash@{0}`. The repair adds a complete local candidate-table Parquet
backup and catalog manifest, with streamed SHA256 and exact row verification,
instead of another full warehouse copy.

## Commands

Read-only inspection, using a fresh output directory:

```powershell
python .superpowers/sdd/tier1-parity/rebuild_candidate_target_index.py `
  --db-path C:/atx/atx-db/data/warehouse.duckdb `
  --artifact-dir C:/atx/.superpowers/sdd/tier1-parity/candidate-index-inspection2 `
  --candidate-id 5b162a89-35e4-58eb-bfb5-ae11932bb2d4 `
  --target-security-id SEC-COMPANYFACTS-UNRESOLVED-CIK-0001495229 `
  --memory-limit 256MB --inspect-only
```

After focused checks and independent review, root runs the same command under
the memory guard with `--artifact-dir .../candidate-index-repair1` and without
`--inspect-only`. New repairs refuse existing output paths. To complete an
interrupted repair, replace `--artifact-dir` with explicit
`--resume-artifact-dir .../candidate-index-repair1`, retaining the exact database,
candidate, target, and memory-profile arguments. This path verifies the original
backup hashes, rows, and catalog before any action; it never starts another DROP.

Focused command from `atx-db`, also root-controlled:

```powershell
python -m pytest tests/test_candidate_index_repair.py -n0 -o addopts= -q
```

## Failure handling

The first focused run found that installed DuckDB rejects same-name DROP/CREATE
at transaction COMMIT (`BoundIndex::CreateDeltaIndex is not supported for this
index type`). No live repair was attempted. The corrected implementation uses
two durable phases: commit DROP, checkpoint/reopen and verify that only the
target index is absent, then execute exact CREATE in its own transaction.

Every intermediate or failed state has `safe_to_resume_source=false`. Root must
not start ingestion until final checkpoint/reopen verification completes.
Explicit resume accepts only unchanged rows/catalog with the target index
absent, or the original complete catalog after CREATE already committed. The
latter path verifies the existing index and never drops it again. Other drift
or changed backup artifacts are refused. Original failed state is retained in a
timestamped history file before a resume attempt updates current state. The utility does not
initialize schemas, restore rows, alter the primary key, migrate, retry source
work, or certify release readiness.
