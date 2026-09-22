# AR1/AR2 production activation integration

2026-09-20. Codex implementation only. No production warehouse access, full suite,
network request, source modification or Claude invocation in this task.

## Delivered

- Activation accepts `--ticker-history-source-path` for native Parquet/TSV and
  records extraction as skipped when supplied. It forwards source diagnostics and
  provenance. The user source remains the staged updated Parquet; no ZIP extraction
  or copy of the Downloads original is required.
- Offline submissions load all forms, including delisting/merger evidence, with
  `--submissions-batch-size 50` CIKs. Existing accession replacement/history semantics
  are unchanged. Invalid batch/thread/shard counts fail before work.
- Activation and the standalone reconciliation driver default to 1GB/one thread.
  Sixteen reconciliation partitions remain sequential; the separate read-only
  symbol-planning connection now receives the same analytical limits.
- Both governed-migration connections receive fixed 1GB/one thread and disabled
  insertion-order preservation at connection creation, before base schema work.
  This preserves the existing injected governance callable signature. Migration
  budgets are deliberately conservative and independent of activation overrides.
- The real `us_common_equity_liquid_v1` writer keeps daily decisions and interval
  compression in DuckDB. The original pandas transform remains a small-input
  oracle. Publication is transactional and source/date/symbol scoped. Late bars
  and future metadata are excluded before selection. Trailing bar, dated listing,
  eligible snapshot and eligible security metadata clocks contribute to decisions.
  Unversioned current security metadata is only usable after its recorded load
  timestamp; earlier unknown classifications retain the original legacy candidate
  defaults and are explicitly not certified historical US common equity.
- `production_panels.py` shares scheduled and activation dispatch. It measures the
  actual legacy monthly grid, skips parent/projection writes for empty cohorts or
  empty factor selections, and preserves previous projection output in that case.
  Nonempty cohorts run selected retained parents then the 23 retirement projections
  in sequential calendar-year scopes, retaining complete monthly peers and the
  parent's 600-day QOP history buffer. It reports cohort counts, cohort dates below
  each factor's minimum, parent writes, factor/date output counts and absent factors.
  Cohort counts alone do not prove sufficient non-null metric peers.
- New stages refresh default Shumway terminals and reconcile codes, build the
  observed price-date calendar, publish corrected-adjusted-price forward returns
  at explicit as-of+22h cutoff, build annual coverage cohorts, measure item coverage,
  then provider coverage and all warehouse quality checks. Uncovered terminal
  reasons, missing/partial historical listing inputs and failed quality checks are
  recorded as diagnostics. Execution success does not certify economic/PIT quality.
- The final quality call supplies an optional deterministic `checked_at`; existing
  callers retain their previous default. No other library clock cleanup was added.
- Migration0312 adds missing production job/catalog metadata without overwriting
  existing operator job definitions. The current forward-return description now
  states adjusted-price legs and observed/named-policy terminals. Historical0186
  is unchanged. Only this task's migration registration/imports were committed.
  Scheduled terminal/forward/coverage jobs require explicit `as_of_date` parameters.

## Validation

Initial guarded focused selection: **124 passed, 1 default slow skip**, exit0.
Files: production activation, universe, activation ledger/stages B/C, jobs DAG,
module boundaries and schema-contract v2. One additional meaningful branch case
then passed: a nonempty 24-peer/two-year projection grid dispatches parents then
projection in exact sequential scopes without splitting peers. **125 unique
passing checks total**, no failures or broad rerun.

Both invocations used `-n0`, the reviewed Windows 3GiB process-tree guard and one
DB workload at a time. Receipts/logs are `activation-integration-tests*` and
`activation-integration-partition*` in this directory. Native Job Object peak
accounting was 0.698GiB and 0.647GiB; these fields are not RSS measurements. The
DB test slot was released immediately afterward.

Import succeeded. Ruff is clean for activation, the new shared adapter/migration,
reconciliation script and new integration tests. Existing findings in jobs,
universe and quality runner were left alone. Strict mypy passes activation and
the shared adapter; `git diff --check` passes. The public snapshot adds only this
task's `production_panels` module. No full non-slow suite has run here.

Controller one-pass independent source review reported no Critical or Important
findings before commit; its separate review record will cite the committed SHA.

## Operator commands

From `C:/atx/atx-db`, after source review and with the single heavy-workload slot.
Choose fresh receipt/log names for retries. A 3GiB cap below is permitted only if
the guard preflight confirms headroom; keep one workload at a time. Governed
migration runs automatically before the first writable activation prepass when
pending versions exist. `--backup-keep 100` preserves the existing backups.

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 3 --receipt ../.superpowers/sdd/tier1-parity/activation-prices-updated-memory.json --stdout ../.superpowers/sdd/tier1-parity/activation-prices-updated.log --stderr ../.superpowers/sdd/tier1-parity/activation-prices-updated.err -- .venv/Scripts/python.exe scripts/warehouse_activate.py --db-path data/warehouse.duckdb --as-of-date 2026-09-20 --only ticker_history_publish --ticker-history-source-path data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet --memory-limit 1GB --threads 1 --backup-keep 100 --force --run-id activation-prices-updated

.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 3 --receipt ../.superpowers/sdd/tier1-parity/activation-source-prepass-memory.json --stdout ../.superpowers/sdd/tier1-parity/activation-source-prepass.log --stderr ../.superpowers/sdd/tier1-parity/activation-source-prepass.err -- .venv/Scripts/python.exe scripts/warehouse_activate.py --db-path data/warehouse.duckdb --as-of-date 2026-09-20 --only submissions_load --only companyfacts_load --submissions-batch-size 50 --companyfacts-symbol-source archive_members --companyfacts-replace-existing --memory-limit 1GB --threads 1 --backup-keep 100 --force --run-id activation-source-prepass

.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 3 --receipt ../.superpowers/sdd/tier1-parity/activation-run5-memory.json --stdout ../.superpowers/sdd/tier1-parity/activation-run5.log --stderr ../.superpowers/sdd/tier1-parity/activation-run5.err -- .venv/Scripts/python.exe scripts/warehouse_activate.py --db-path data/warehouse.duckdb --as-of-date 2026-09-20 --start-stage statement_points --force --memory-limit 1GB --threads 1 --shards 16 --backup-keep 100 --run-id activation-run5
```

These offline prepasses require no SEC HTTP requests. If separate network stages
are selected, retain only the authorized dummy contact `atx-research@example.com`.
Run5's late start intentionally does not select source ingestion; prepasses must
finish first. Root owns execution, ledger monitoring and live measurements.

## Remaining production evidence

Full-universe retained-parent/index memory and wall time are not established by
fixtures; the process-tree guard and low DuckDB cap remain required. Missing
dated historical listing inputs cannot be manufactured from bars/current metadata.
Observed calendars and source-adjusted prices do not certify exchange calendars,
economic adjustments or historical vendor publication vintages. New custom
features and downstream adjusted-return consumer repairs are separate owners.
Regenerate DATA_DICTIONARY after all reserved migrations settle. Quality/schema
thresholds, release evidence, whole-branch review and merge approval remain root's
gates; no condition was weakened or public release/merge performed here.
