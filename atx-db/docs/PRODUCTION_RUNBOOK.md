# atx-db production runbook

## Runtime model

`atx-db` uses one DuckDB writer per warehouse. Schedule ingestion and refresh
jobs serially. Readers should use read-only connections or published Parquet
snapshots. Keep `ATX_DB_PATH`, cache, staging, and DuckDB spill storage on the
same durable volume when possible.

Set `ATX_SEC_USER_AGENT` to a product name and monitored contact address before
making SEC requests. The 13F loader validates ZIP contents, resumes partial
downloads, records source hashes and byte counts, and atomically replaces one
source archive at a time.

## Deploy

1. Build and install the wheel in an isolated Python 3.12 environment.
2. Point `ATX_DB_PATH` at the candidate warehouse.
3. Run the governed migration script; it checkpoints and backs up an existing
   database before applying pending migrations.
4. Run `atx-db status --strict` and the focused smoke lane.
5. Ingest sources and materialize derived datasets.
6. Publish a read-only warehouse or lake snapshot only after quality checks pass.

```powershell
python scripts/warehouse_migrate.py --db-path $env:ATX_DB_PATH
atx-db status --db-path $env:ATX_DB_PATH --strict
python scripts/db_dev_tests.py --smoke --workers 0
```

Large analytical refreshes default to a 4 GB DuckDB memory cap, four worker
threads, disk spilling beside the warehouse, and unordered inserts. Override
`--memory-limit` or `--threads` when a deployment has a different resource
envelope.

## 13F recovery

Each SEC archive is an idempotent partition keyed by `source_period`. Re-running
the same archive deletes and reloads only that partition. Failed attempts are
recorded in `raw_source_files`; `.part` downloads are resumable. Extracted TSVs
are removed after a successful or failed load unless `--keep-extracted` is set.

After any raw 13F correction or late archive, rerun
`atx-db refresh-13f-amendments` for the affected report-period range. A full
refresh is the safest release procedure because trailing z-scores depend on the
manager's prior 24 report quarters.

For signal research, use `--skip-effective-positions`; corrections and rate
denominators remain complete, while unchanged portfolios are not duplicated
into a second large fact table. Next-filing exit outcomes reconstruct the raw
filing chain directly and do not depend on that optional materialization.

Instrument mapping and backtests default to the top 20 average-z candidates per
quarter and exclude stress quarters. The source post discloses ranking and the
quiet-quarter condition but not a capacity cutoff, so `--max-rank-per-quarter`
and `--include-stress` expose both assumptions for sensitivity runs. Backtests
also enforce a point-in-time $2B-$10B market-cap range by default to implement
the post's US mid-cap cohort; both bounds are CLI options.

## Release checks

- `atx-db status --strict` reports the current migration version.
- Raw archive row counts and hashes exist in `raw_source_files`.
- Effective positions have one row per manager/report-period/position key.
- Amendment correction rows have no duplicate correction IDs.
- Research uses `available_at`, never report dates, to form signals.
- Secrets and proprietary CUSIP support columns are excluded from lake exports.
