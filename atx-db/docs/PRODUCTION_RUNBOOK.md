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

## Activation from scratch

`atx-db activate` builds a complete warehouse from an empty directory. It is
idempotent, resumable, and deterministic: every stage is recorded in
`activation_stage_runs`, and a rerun skips stages whose newest attempt completed
unless `--force` is given. Each stage prints exactly one JSON line to stdout.

### Prerequisites

- The SpiderRock archive `tbltickerhistory3_10y.zip` on disk (3.30 GiB
  compressed; one DEFLATE member, 11,084,562,320 uncompressed bytes).
- `ATX_SEC_USER_AGENT` set to a product name and a monitored contact address.
  The ladder fails fast on any SEC stage without it.
- One durable volume with room for the disk budget below.

### Disk usage

| Artifact | Size |
| --- | --- |
| `tbltickerhistory3_10y.zip` (input, not written by the ladder) | 3.3 GB |
| `data/staging/broad-bars/tbltickerhistory3_10y.txt` (extracted TSV) | **11 GB** |
| `data/cache/companyfacts.zip` | **~1.3 GB** |
| `data/cache/submissions.zip` | **~1.5 GB** |
| `data/warehouse.duckdb` (built) | **30-40 GB** |
| Pre-migrate backup (`data/warehouse.duckdb.<label>.<timestamp>.bak`, one per governed migration run against an existing warehouse; skipped when the warehouse is already at head) | full copy of the warehouse (30-40 GB) each, `--backup-keep` most recent kept (default 3) |
| DuckDB spill, `ticker_history_publish` (`data/staging/broad-bars/duckdb-tmp/`, transient) | up to 8 GB |
| DuckDB spill, every other connection (`<db parent>/.<db name>.duckdb_tmp/`, e.g. `data/.warehouse.duckdb.duckdb_tmp/`, transient) | best-effort, sized by the query |
| **Peak total** | **~65 GB**, plus up to `--backup-keep` x 30-40 GB while pre-migrate backups accumulate on a resumed/`--only` run against a pre-head warehouse |

The staging TSV may be deleted after `ticker_history_publish` completes; keep the
`.sha256` sidecar so a later rerun can prove which extraction produced the bars.

### Commands

```powershell
$env:ATX_SEC_USER_AGENT = "atx-db/0.2 ops@example.com"
$env:ATX_DB_PATH = "D:\atx\data\warehouse.duckdb"

# 1. See the plan without touching anything.
atx-db activate --db-path $env:ATX_DB_PATH --dry-run

# 2. Run the whole ladder (multi-hour; safe to interrupt).
atx-db activate --db-path $env:ATX_DB_PATH `
  --ticker-history-zip $env:USERPROFILE\Downloads\tbltickerhistory3_10y.zip `
  --staging-dir D:\atx\data\staging\broad-bars `
  --cache-dir D:\atx\data\cache `
  --memory-limit 8GB --threads 4 --shards 16

# 3. Resume after an interruption (completed stages are skipped automatically).
atx-db activate --db-path $env:ATX_DB_PATH

# 4. Resume from an explicit point, or rerun one stage.
atx-db activate --db-path $env:ATX_DB_PATH --start-stage companyfacts_load
atx-db activate --db-path $env:ATX_DB_PATH --only standardized --force

# 5. Confirm the result.
atx-db status --db-path $env:ATX_DB_PATH --strict
```

On an existing warehouse file that has at least one migration PENDING,
`atx-db activate` runs the same governed-migration guard as
`scripts/warehouse_activate.py`: checkpoint + backup + locked apply + verify,
with automatic restore-on-failure, before the ladder ever opens its own
connection, so a bad migration can never leave a live warehouse half-upgraded,
followed by `--backup-keep`-bounded pruning of older backups. A warehouse
already at schema head skips the governed path entirely -- it is a full-file
copy plus a re-hash, so paying that cost on every resume or `--only` call
would be wasteful once a build is mostly done. A brand-new (non-existent)
database file also skips this -- `stage_migrate` bootstraps it directly -- and
so does `--dry-run`, whose contract is to touch nothing.

### Stage order

`migrate` -> `security_master` -> `symbol_directory` -> `ticker_history_extract`
-> `ticker_history_publish` -> `sec_bulk_download` -> `submissions_load` ->
`companyfacts_load` -> `statement_points` -> `periods` -> `ttm` ->
`calendarization` -> `standardized` -> `industry_templates` -> `reconciliation`
-> `provider_coverage`.

Network is limited to `security_master`, `symbol_directory`, and
`sec_bulk_download`; every other stage is offline. `sec_bulk_download` resumes a
partial transfer and records each archive's sha256 in `raw_source_files`.
`reconciliation` shells out to `scripts/refresh_reconciliation_sharded.py`, which
runs each shard in a fresh interpreter (a long-lived process was measured to
degrade a shard from ~110s to ~840s).

### Determinism

No derived or ingest path reads the wall clock. `atx_db.warehouse.now_utc_naive()`
is the only sanctioned timestamp read and is used solely for `source_loaded_at`
lineage. `atx_db.clock.utc_today()` is the only sanctioned wall-clock date read
and is called only at CLI/script edges, which pass the value down explicitly.
Library code resolves its stamp through
`atx_db.clock.resolve_as_of_date(explicit, source_max_date=...)`, which raises
rather than silently producing a non-reproducible run.

`source_loaded_at` is lineage only and is never an ordering or dedupe key: the
factor panel selects duplicates by `(available_at, run_id)` in both its pandas
and SQL read paths, and `factor_breadth.available_at` is the max of its input
availabilities, falling back to `as_of_date + 22h` rather than to the clock.

`--as-of-date` must be pinned to the same explicit value across reruns of the
network stages (`security_master`, `symbol_directory`, `sec_bulk_download`) for
those reruns to be byte-identical; left unset, each invocation resolves it from
`atx_db.clock.utc_today()` at the CLI edge, which necessarily differs run to
run.

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

## Delisting terminal returns: the Shumway policy

`delisting_terminal_returns` collapses every delisting to at most one terminal return per
`(security_id, delist_date)`, in strict priority order: an **observed** vendor `DLRET` always
wins; failing that, a deterministic **corporate-action policy** (cash merger, stock merger,
spinoff) applies when a matching `corporate_actions` row exists; failing that, the **Shumway
performance-delisting convention** applies when the delist reason is performance-related
(`atx_db.delisting.PERFORMANCE_DELIST_REASONS = {"bankruptcy", "exchange_delist", "unknown"}`)
and an event carries a real `available_at` (never invented).

The convention itself: Shumway (1997), *Journal of Finance* 52(1), estimates a mean delisting
return of about **-30%** for NYSE/AMEX performance-related delistings
(`atx_db.delisting.SHUMWAY_PERFORMANCE_DELISTING_RETURN`); Shumway & Warther (1999), *JF* 54(6),
estimate about **-55%** for Nasdaq (`SHUMWAY_NASDAQ_PERFORMANCE_DELISTING_RETURN`). The default
policy selects **-55% for Nasdaq** and **-30% for other or unresolved exchanges**, per event.
It records Nasdaq rows as `terminal_return_policy='performance_unknown_nasdaq'` and
`return_basis='shumway_nasdaq_default'`; other rows use `performance_unknown` and
`shumway_default`. Filter both imputation variants with
`WHERE terminal_return_policy IS NULL OR terminal_return_policy NOT LIKE 'performance_unknown%'`.

Exchange lookup uses `universe_us_listed_membership` (`us_listed_v1`), then `exchange_listings`,
then `nasdaq_symbol_directory`. An input must cover the delist date where it has a validity
interval, have a snapshot dated no later than that date, and be available by that date at
22:00. Future snapshots and later-known exchange changes cannot classify a historical delist.
The imputed row's `available_at` is the maximum of event and selected exchange availability.

**Default: ON.** `DelistingTerminalReturnOptions.performance_delisting_return` defaults to
`ShumwayPerformancePolicy()` (S4 preflight ruling: a survivorship-adjusted panel with
no delisting-return convention at all is a worse default than a documented, filterable
imputation). **Opt out** by constructing
`DelistingTerminalReturnOptions(performance_delisting_return=None)` before calling
`refresh_delisting_terminal_returns` — the convention is then never applied, and no row is ever
invented for a performance-related delist with no observed return. A policy object can override
the two magnitudes independently; a float remains an explicit uniform-return override.

**The non-vacuous coverage gate.** The critical
`survivorship_forward_return_drops_delisted_names` check anti-joins `delisting_terminal_returns`
against the panel — with an empty `delisting_terminal_returns` table that anti-join is itself
empty, so the check is GREEN while proving nothing (audit §4.4). The companion check
`delisting_events_without_terminal_return`
(`atx_db.quality.checks_survivorship.SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME`, severity
`error`) counts `delisting_events` rows with no matching `delisting_terminal_returns` row and
fails (threshold `0.0`, comparator `le`) the moment that count is nonzero — turning "no
delistings are covered" into a measured, non-zero failure instead of a silent pass. Running with
the Shumway policy off (`performance_delisting_return=None`) is a valid operator choice, but it
will trip this gate for every uncovered performance-related delist, by design. This companion
check has severity `error`, so the orchestrator degrades the run to `partial`; only a
`critical` failure halts it.
