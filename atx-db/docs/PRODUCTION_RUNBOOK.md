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

Some analytical commands default to a 4 GB DuckDB cap and four threads. For the
current roughly 16 GiB host, explicitly start with **1 GB and one DuckDB thread**,
one heavy process tree at a time, under the controller's aggregate memory guard
with a cap of at most 4 GiB and physical/commit headroom checks. A DuckDB setting
does not cap pandas or other Python allocations. The guard limits the process
tree; it does not prove that an unbounded operation can finish within that cap.
Use bounded local test concurrency (`-n 0`); CI's four-worker lane runs on a
separate machine and is not the local resource policy.

## Activation from scratch

`atx-db activate` runs the warehouse build ladder from an empty directory. It is
resumable: every stage is recorded in
`activation_stage_runs`, and a rerun skips stages whose newest attempt completed
unless `--force` is given. Each stage prints exactly one JSON line to stdout.
Stage completion records execution; coverage, historical population and release
readiness must be measured separately.

### Current rebuild evidence (2026-09-20)

The current [activation measurements](TIER1_ACTIVATION_STATUS.md) record run4's
`statement_points` failure, the successful corrected-price publication, and
the subsequent partial companyfacts archive3 load. Archive3 failed at COMMIT
at 22:02:44 UTC; 38,500,008 raw facts and the same number of fundamental points
were retained. No full downstream fundamentals build has completed. That record supersedes the older
[activation handoff](../../docs/superpowers/handoffs/2026-09-20-tier1-parity-handoff.md)
status; source-ingestion counts are not full-universe coverage or quality gates.
Run4 has stopped. Before any new warehouse operation, verify that no replacement
writer is active and follow the controller's serialized, guarded launch policy.

**Price-source correction:** both archive loaders now map finite positive
`close * cumulReturnFactor` to `equity_daily_bars.adjusted_close`; missing,
nonpositive, nonfinite or overflowing factors/products produce NULL. The vendor's
`closePr` is the adjusted **prior** session close and remains a raw source field.
Bars display historical `ticker_tk`; current `todayTicker` remains metadata and
the legacy current-symbol/CIK association is explicitly unverified. Repeated
positive vendor-ID/date keys are quarantined together, never resolved by volume.
The raw TSV/archive is preserved; preprojection counts and original-`dn` adjacency
diagnostics are recorded separately from postpublication uniqueness checks.
`returnFactor` is distribution-inclusive; canonical `split_factor` is NULL.
Adjusted-return consumer repairs are committed. Corrected prices were published
at 18:00:09 UTC: 31,959,271 rows across 34,251 warehouse security IDs, through
2026-09-18. Return-dependent production surfaces still require rebuilding;
31,934,514 custom-feature rows are built, but forward-label evaluation is pending.
Internal agreement does not verify every economic adjustment.
The vendor lists 05:00 CT T+1
delivery; the backfill's session-date + 22-hour availability is a modeling
assumption, not verified historical publication or revision timing.
[SpiderRock TickerHistory3 dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/).

The directory snapshot is dated 2026-09-20, after the latest loaded bar. It
cannot certify historical common-equity membership by being backdated. The
historical top-3000 common-equity cohort remains unestablished; the existing
liquidity cohort is a different population. Item coverage now measures the
separate annual PIT top-3000 population, all completed FY2015+ years and explicit
missing/undersized cohorts against the unchanged 110-item / 90% gate. Registry item counts are static contract
breadth. Neither those counts nor dated proof slices in the
[provider design](FUNDAMENTALS_PROVIDER_DESIGN.md) certify the rebuilt warehouse.

The ladder includes archive-wide companyfacts selection, all-form submissions,
terminal returns, legacy-cohort factor projection, adjusted-price forward returns,
annual item coverage and final quality measurement. Historical listing evidence
remains an input prerequisite. These stages require measured production outcomes;
successful execution alone does not certify the data.

### Prerequisites

- The retained price source. This rebuild uses the updated native
  `TickerHistory3.parquet`, staged under `data/staging/broad-bars/2026-09-20-updated/`
  (3,617,973,507 bytes). The older `tbltickerhistory3_10y.zip` and extracted TSV
  remain preserved source evidence. Use `--ticker-history-source-path` for the
  native file; it does not require ZIP extraction.
- `ATX_SEC_USER_AGENT` set to a product name and a monitored contact address.
  The ladder fails fast on any SEC stage without it.
- One durable volume with room for the disk budget below.

### Disk usage

| Artifact | Size |
| --- | --- |
| `tbltickerhistory3_10y.zip` (input, not written by the ladder) | 3.3 GB |
| Updated `TickerHistory3.parquet` (current published price source) | 3.62 GB |
| `data/staging/broad-bars/tbltickerhistory3_10y.txt` (extracted TSV) | **11 GB** |
| `data/cache/companyfacts.zip` | **~1.3 GB** |
| `data/cache/submissions.zip` | **~1.5 GB** |
| `data/warehouse.duckdb` (planning estimate, not a measured final size) | **30-40 GB** |
| Pre-migrate backup (`data/warehouse.duckdb.<label>.<timestamp>.bak`, one per governed migration run against an existing warehouse; skipped when the warehouse is already at head) | full copy of the warehouse (30-40 GB) each, `--backup-keep` most recent kept (default 3) |
| DuckDB spill, `ticker_history_publish` (`data/staging/broad-bars/duckdb-tmp/`, transient) | up to 8 GB |
| DuckDB spill, every other connection (`<db parent>/.<db name>.duckdb_tmp/`, e.g. `data/.warehouse.duckdb.duckdb_tmp/`, transient) | best-effort, sized by the query |
| **Peak total (planning estimate)** | **~65 GB**, plus up to `--backup-keep` x 30-40 GB while pre-migrate backups accumulate on a resumed/`--only` run against a pre-head warehouse |

Preserve the actual staging TSV, original archive and `.sha256` sidecar for this
rebuild. The sidecar alone is not retained source evidence. Cleanup requires a
separate explicit controller retention decision; publication does not authorize
deleting any source or backup.

### Commands

```powershell
$env:ATX_SEC_USER_AGENT = "atx-db/0.1 atx-research@example.com"
$env:ATX_DB_PATH = "D:\atx\data\warehouse.duckdb"
$activationDate = "2026-09-20" # Pin the actual observation date consistently.
$activationPython = ".\.venv\Scripts\python.exe"
$memoryGuard = "..\.superpowers\sdd\tier1-parity\run_memory_guarded.py"
$guardReceipts = "..\.superpowers\sdd\tier1-parity"

# 1. See the plan without touching anything.
atx-db activate --db-path $env:ATX_DB_PATH --dry-run --as-of-date $activationDate

# 2. Run only after the controller clears the pending input/memory prerequisites.
# Each guard receipt must have a new name; existing receipts cannot be overwritten.
& $activationPython $memoryGuard --job-gb 4 `
  --receipt "$guardReceipts\activation-from-scratch-memory.json" -- `
  $activationPython scripts/warehouse_activate.py --db-path $env:ATX_DB_PATH `
  --ticker-history-zip $env:USERPROFILE\Downloads\tbltickerhistory3_10y.zip `
  --staging-dir D:\atx\data\staging\broad-bars `
  --cache-dir D:\atx\data\cache `
  --memory-limit 1GB --threads 1 --shards 16 `
  --as-of-date $activationDate --backup-keep 100

# 3. Resume after an interruption (completed stages are skipped automatically).
& $activationPython $memoryGuard --job-gb 4 `
  --receipt "$guardReceipts\activation-resume-memory.json" -- `
  $activationPython scripts/warehouse_activate.py --db-path $env:ATX_DB_PATH `
  --memory-limit 1GB --threads 1 --shards 16 `
  --as-of-date $activationDate --backup-keep 100

# 4. Resume from an explicit point, or rerun one stage.
& $activationPython $memoryGuard --job-gb 4 `
  --receipt "$guardReceipts\activation-companyfacts-memory.json" -- `
  $activationPython scripts/warehouse_activate.py --db-path $env:ATX_DB_PATH `
  --start-stage companyfacts_load --memory-limit 1GB --threads 1 --shards 16 `
  --as-of-date $activationDate --backup-keep 100
& $activationPython $memoryGuard --job-gb 4 `
  --receipt "$guardReceipts\activation-standardized-memory.json" -- `
  $activationPython scripts/warehouse_activate.py --db-path $env:ATX_DB_PATH `
  --only standardized --force --memory-limit 1GB --threads 1 --shards 16 `
  --as-of-date $activationDate --backup-keep 100

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

For this rebuild, use the authorized dummy SEC contact shown above; never send
the user's email. Activation examples retain up to 100 backups to preserve the
existing files; confirm that retention exceeds existing backups plus new ones
before proceeding. Do not use the default retention of three to prune the
user's backups. The standalone `warehouse_migrate.py` command backs up and
migrates without the activation retention option.

### Stage order

`migrate` -> `security_master` -> `symbol_directory` -> `ticker_history_extract`
-> `ticker_history_publish` -> `sec_bulk_download` -> `submissions_load` ->
`companyfacts_load` -> `statement_points` -> `periods` -> `ttm` ->
`calendarization` -> `standardized` -> `industry_templates` -> `reconciliation`
-> `derived_metrics` -> `market_daily` -> `legacy_liquid_universe`
-> `factor_projections` -> `delisting_evidence`
-> `universe_us_listed` -> `delisting_terminal_returns` -> `trading_calendar`
-> `survivorship_forward_returns` -> `item_coverage` -> `provider_coverage`
-> `equity_price_metrics` -> `quality`.

The cohort uses the original price/liquidity rules, with dated inputs only;
unclassified bar candidates are not proof of historical US common-equity listing.
Its full daily decisions and interval compression stay in DuckDB. The 23 retired
factor mappings and retained parents run in sequential calendar-year partitions
with complete monthly peers. Empty cohorts skip parents and preserve prior
projection output with an explicit degraded diagnostic; empty output and
insufficient peers are recorded, never treated as continuity evidence.

`delisting_terminal_returns` also reconciles codes and reports uncovered reasons.
The observed calendar precedes the adjusted-price forward panel. `item_coverage`
builds the separate annual cohort before measuring items; `quality` runs all
checks using the explicit as-of date at 22:00. Failures remain visible in the
stage detail and quality tables; no stage completion flips a schema condition.

`equity_price_metrics` builds adjusted returns, momentum, volatility, liquidity,
drawdown, beta, correlation and cross-sectional ranks in DuckDB. It stages the
complete result, builds the primary-key table in sequential prefix batches, and
publishes with an atomic table swap. Its full-scale memory and coverage must be
measured separately from the successful daily-bar publication.

An operator-supplied `--ticker-history-source-path` accepts the native staged
Parquet or TSV and makes extraction a recorded no-op. The updated user source is
`data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet`; preserve the
Downloads original and old source. After governed migration and source review,
run offline source prepasses separately from the downstream rebuild:

```powershell
.venv/Scripts/python.exe scripts/warehouse_activate.py --db-path data/warehouse.duckdb `
  --as-of-date 2026-09-20 --only ticker_history_publish `
  --ticker-history-source-path data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet `
  --memory-limit 1GB --threads 1 --backup-keep 100 --force --run-id activation-prices-updated
.venv/Scripts/python.exe scripts/warehouse_activate.py --db-path data/warehouse.duckdb `
  --as-of-date 2026-09-20 --only submissions_load --only companyfacts_load `
  --submissions-batch-size 50 --companyfacts-symbol-source archive_members `
  --companyfacts-replace-existing --memory-limit 1GB --threads 1 `
  --backup-keep 100 --force --run-id activation-source-prepass
.venv/Scripts/python.exe scripts/warehouse_activate.py --db-path data/warehouse.duckdb `
  --as-of-date 2026-09-20 --start-stage statement_points --force `
  --memory-limit 1GB --threads 1 --shards 16 --backup-keep 100 --run-id activation-run5
```

Wrap each command in the reviewed controller process-tree memory guard, with a
fresh receipt and a cap supported by current physical/commit headroom. Run one
heavy command at a time. Use the project `.venv/Scripts/python.exe` for both the
guard and its child; the locked DuckDB runtime is 1.5.5. Bare system Python may
load another version and is not a valid production or test runtime here.
Submissions use all forms and 50-CIK flush batches;
reconciliation's separate symbol-planning connection gets the same session cap.
Scheduler dataset IDs use the same panel adapters. Supply explicit `as_of_date`
in scheduled terminal/forward/coverage job parameters; projection and cohort
jobs also accept date scopes. Keep the legacy and annual cohort IDs distinct.

Repeat `--only` for multiple stages; selections run in ladder order. A slice
does not build omitted prerequisites. LEI/FIGI activation is deferred and has
no active stage or vendor-file flags. The existing `industry_templates` stage
remains; expanded industry templates and their fixture corpus are deferred.

Network is limited to `security_master`, `symbol_directory`, and
`sec_bulk_download`; every other stage is offline. `sec_bulk_download` resumes a
partial transfer and records each archive's sha256 in `raw_source_files`.
`reconciliation` shells out to `scripts/refresh_reconciliation_sharded.py`, which
runs the sixteen shards **sequentially, with one shard child active at a time**.
Each shard gets a fresh interpreter; sixteen partitions are not sixteen workers.
The controller guard covers the supervisor and its descendants and refuses a
launch with insufficient headroom. Its reviewed code is versioned at `0e6737fa`;
see [current memory evidence](TIER1_ACTIVATION_STATUS.md#memory-and-pending-gates).

### Determinism

The inspected factor-panel pandas and SQL paths select duplicates by
`(available_at, run_id)`. `factor_breadth.available_at` is the maximum input
availability, with an `as_of_date + 22h` fallback. Those are scoped contracts;
they do not establish determinism for every ingestion and identity path.

The plain ticker-history adapter now reuses only unambiguous existing vendor
links; competing links fall back to a stable vendor key without ordering by
warehouse load timestamps. Positive collision keys no longer depend on ticker
display. The bulk `_create_symbol_map` still uses `current_date` and
`source_loaded_at` to select current-symbol identity candidates, so changes in
the current metadata or load history can alter its mapping. Correcting that
remaining behavior belongs to the historical identity follow-on; labeling a
source vintage unknown does not make its identity selection deterministic.

Pin `--as-of-date` consistently across network-stage reruns. Commands that omit
it resolve a current date at the CLI edge. A pinned date does not freeze remote
source revisions or imply byte-identical downloads. Modeled timestamps likewise
do not establish historical archive availability; the price-source limitation
above still applies.

## Universe and delisting evidence

After prerequisites have completed and the warehouse writer has exited, a
targeted rebuild uses the current CLI syntax:

```powershell
& $activationPython $memoryGuard --job-gb 4 `
  --receipt "$guardReceipts\activation-universe-memory.json" -- `
  $activationPython scripts/warehouse_activate.py --db-path $env:ATX_DB_PATH `
  --only delisting_evidence --only universe_us_listed --force `
  --memory-limit 1GB --threads 1 --shards 16 `
  --as-of-date $activationDate --backup-keep 100
```

`delisting_evidence` reads landed SEC Form 25/25-NSE, Nasdaq deletes, SEC Form 15
and archive last-trade evidence, then folds the sources into `delisting_events`
by precedence. Default submissions ingestion does not establish that every
required delisting form is present; inventory and supplement those inputs
before measuring evidence coverage. Archive inference requires more than 30
later observed trading sessions and carries the availability of that evidence.
An archive-end observation alone does not establish a delisting.

`universe_us_listed` builds `universe_us_listed_membership`. Qualifying securities
with no resolved CIK remain as `has_cik = false`; the
`universe_us_listed(store, as_of_date, require_cik=True)` accessor selects the
fundamentals subset. `market_cap_decile` is an interval-start attribute. The
builder requires dated listing/type evidence: a bar-observed candidate or a
current listing snapshot is not proof of historical US common-equity membership.

The current ladder's evidence stage does not refresh terminal returns. The
Shumway policy below applies when `refresh_delisting_terminal_returns` is run;
its implementation and successful evidence generation do not certify terminal
coverage or survivorship-adjusted forward returns.

## Publishing a release

Wait for ingestion to release the single-writer warehouse. Apply all pending
migrations with the governed command before publishing; publication checks for
pending migrations read-only and refuses to open a writable store if any remain.
Check the full pending set, not only the highest migration number. Preserve
existing backups.

```powershell
python scripts/warehouse_migrate.py --db-path $env:ATX_DB_PATH
atx-db status --db-path $env:ATX_DB_PATH --strict

# After measuring and reviewing release quality, create the first baseline.
atx-db publish-release --db-path $env:ATX_DB_PATH --release-id 2026-09-20 `
  --out-dir data/releases

# A later release may compare against a verified, existing predecessor.
atx-db publish-release --db-path $env:ATX_DB_PATH --release-id 2026-09-27 `
  --out-dir data/releases --previous-dir data/releases/2026-09-20
```

Publication writes six Parquet datasets: `security_master`, `universe`,
`delistings`, `fundamentals_core`, `derived_metrics` and `market_daily`, plus
`manifest.json`, and records them in `publication_releases` and
`publication_release_datasets`. `fundamentals_core` references the existing
`ATX.US.FUNDAMENTALS/standardized` public schema; there is no separate
`fundamentals-core` schema code. Existing release directories cannot be
overwritten.

The first release has no predecessor and null added/removed/changed counts.
Later releases verify predecessor files and compute exact key-level diffs when
their schemas are compatible. The manifest records schema, query and Parquet
hashes, row counts and activation run IDs. Verify those artifacts against the
recorded hashes; a successful export is not a quality pass. The CLI supplies
the creation timestamp, so separate CLI invocations do not promise identical
manifest hashes even when dataset contents are unchanged.

Run `run_warehouse_quality_checks` and review its actual results before release.
No critical failures is necessary but does not establish the entire Tier-1 gate:
inspect error-severity terminal gaps, annual cohort/item coverage, identities,
shares, universe exclusions, projected factor breadth and provider SLO conditions.
Publication does not enforce those checks itself. Keep unresolved inputs and
`pending`/`degraded` surfaces visible in the release evidence; never change
observation dates to hide the age of the last loaded prices.

## Documentation and retained artifacts

Run `python scripts/generate_data_dictionary.py` after registry, public-schema,
universe or delisting vocabulary changes. It reads repository definitions only;
`python scripts/generate_data_dictionary.py --check` fails CI when
[DATA_DICTIONARY.md](DATA_DICTIONARY.md) differs. It never measures a warehouse.
`scripts/measure_item_coverage.py --write-docs` is the separate producer of
`docs/ITEM_COVERAGE.md`; publish that report only with its actual cohort, fiscal
years and known measurement limitations.

Retirement wave 2 (`dae4220e`) removed `abnormal_capex` and `operating_leverage`
and disabled the `market_cap`, `enterprise_value` and `valuation_multiples`
schedules. Historical valuation tables and their rows remain. The separate
18-module S3 retirement and compatibility work landed in `72d38a93`. Its code
and fixture evidence is not evidence of an activated production factor panel.

The optional checkout-root `warehouse_template.duckdb` is distinct from the
active test templates under `.pytest_cache/db_schema_templates/<fingerprint>/`.
The current test harness references the latter. This documentation pass leaves
both alone; verify local process use and file ownership before any later cleanup.
Existing migration backups and stashes are also preserved.

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
