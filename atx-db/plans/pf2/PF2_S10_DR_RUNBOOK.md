# PF2-S10 DR Runbook

Use this from the primary checkout only. Do not run live compaction or a live
full rebuild from a throwaway worktree.

## Preconditions

- Confirm the working tree is clean for `atx-impl` and the live DB path is the
  primary `atx-impl/db/atx_impl.duckdb`.
- Run the PF2-S2 governed backup/migration path before any live schema apply:
  `python scripts/warehouse_migrate.py --db-path db/atx_impl.duckdb --label pf2-s10-live`.
- Record the backup path, backup SHA-256, applied schema version, and
  `schema_contract_sha256` in the ledger.

## Restore From Backup

1. Stop any process holding the DuckDB file.
2. Restore the latest approved `.bak` using `db.migration_admin.restore_database`
   or the PF2-S2 recovery helper when a WAL backup is involved.
3. Open the restored DB through `DuckDBStore.initialize()` and verify migration
   checksums plus the schema contract.
4. Run `CHECKPOINT` after the restored DB opens cleanly.

## Compaction

1. From the primary tree, open the DB with `DuckDBStore`.
2. Run `checkpoint_and_compact(store, backup_dir=<approved-backup-dir>)`.
3. Confirm `migration_backup_registry` has the compaction backup artifact.
4. Run `record_storage_stats(store)` and record `db_size_bytes`,
   `wal_size_bytes`, `checked_at`, and table count.

## Gated Rebuild

1. Review `quality_check_registry` before enabling gates. If known legacy PIT
   gaps are not yet backfilled, stage explicit threshold/exemption policy there
   rather than editing Python.
2. Run:
   `python scripts/warehouse_rebuild.py --db-path db/atx_impl.duckdb --since <YYYY-MM-DD> --until <YYYY-MM-DD>`.
3. Record `warehouse_rebuild_runs.rebuild_run_id`, `orchestrator_run_id`,
   `git_sha`, status, and per-dataset counts.
4. If a critical gate halts, inspect `etl_job_audit.action='step_quality_gate_halt'`
   and the matching `data_quality_checks` rows before resuming.

## Live Smoke Ledger

Record these values in `WAREHOUSE_PARITY_TRANCHES.md` or the operator ledger:

- Backup path, bytes, SHA-256, and schema version.
- Critical gate trip/revert run id and failed check name.
- `warehouse_storage_stats.db_size_bytes` and `wal_size_bytes`.
- Freshness-SLA sweep count and any row-count anomaly count.
- Partitioned lake sample object, partition count, skipped partition count.
- Rebuild id, git SHA, status, and per-dataset row counts.
