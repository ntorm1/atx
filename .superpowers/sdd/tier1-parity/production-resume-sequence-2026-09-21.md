# Production resume sequence — updated 2026-09-23

Static source/dispatch audit only. The production snapshot remains **as of
2026-09-20**. Run one guarded process tree at a time from `C:\atx\atx-db`.
Every guarded invocation needs a fresh receipt, stdout, and stderr filename.
The next-stage commands below use DuckDB at `1GB` and one thread; retain `--backup-keep 100`, and use the
dummy SEC User-Agent `atx-db/0.1 atx-research@example.com`.

## Current source position and guard prerequisite

Archive16 is terminal after the22:16UTC host headroom stop. Both stale ledgers
were recovered22:19:32UTC and CHECKPOINT passed; no new attempt rows, facts and
points remain47,941,000, schema0322. Do not repeat that completed recovery.
Next actual predecessor: `513cfbbc-096a-4186-9666-b6cc5170c4ad`.
Use fresh archive17 receipt/log/error files for a later full-source attempt.

No warehouse writer is live. User directs platform work while backfill awaits
host capacity: OPS1 pipeline-status is committed b814f4c1 and live-read verified
in8291fc44. DL1 selected-input lineage (0323,1fa721c7), FQ1 signal panel
(0324,fbba7bf2), and FQ2 decile evaluation/label basis (0325,d82f9eca) are
committed and focused-tested. Wider HEAD/schema checks remain pending after host
guard stops/refusal. All runtime/registry locks returned to root. Do not launch production over
unreviewed/uncommitted shared edits or overlap root's focused tests. Recheck
pending migrations after these tasks, using governed backup-keep100.

Source restarts retain sustained6GiB physical/8GiB commit observations over
two minutes,1GB DuckDB/one thread,2GiB process cap,fullarchive_members,replacement,
force,snapshot2026-09-20 and dummySECcontact. The unchanged runtime host stops
are1.5GiB physical/3GiB commit. A passed window does not guarantee future host
capacity; record any terminal state before another resume. The256MB/1GiB-cap
operator recovery was bookkeeping only, not a source-write capacity claim.

Full CompanyFacts->fullsubmissions->scopedCVXsource->fulluniverse run5 remains
the materialization sequence. Never infer a live process from a running ledger.

## Submissions verified resume

Run only after the source resume is terminal and inspected:

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 2 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 `
  --only submissions_load --submissions-batch-size 50 `
  --submissions-resume-from-run-id 04cf947d-53bb-49b7-a276-b3c74a2a52c8 `
  --memory-limit 1GB --threads 1 --backup-keep 100 --force `
  --run-id activation-submissions-resume `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

The loader requires this predecessor to be a terminal failed `sec_submissions`
bulk dataset run with all forms, all CIKs, history enabled, the same archive
path, and batch size 50. Before writing it verifies a matching pre-attempt
archive hash receipt, an ordered contiguous committed lineage, and retained
rows against every referenced archive member. It rejects any run that includes
`sec_bulk_download`; do not add that stage.

## CVX earnings-source acceptance wave

After the full submissions resume is terminal and its actual ledger and archive
counts are verified, run the governed source stage for only CVX CIK
`0000093410`, through the fixed snapshot. The CIK scope is intentional evidence
for this source acceptance wave. It must not be carried into run5.

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 2 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-cvx-earnings-source1-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-cvx-earnings-source1.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-cvx-earnings-source1.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 `
  --only earnings_release_facts --earnings-release-cik 0000093410 `
  --earnings-release-history-end 2026-09-20 --force `
  --memory-limit 1GB --threads 1 --backup-keep 100 `
  --run-id activation-cvx-earnings-source1 `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

Inspect the stage and source dataset receipts, including attempts, supported
tables, and uncovered reasons. The pre-submissions measurement found 426,151
unique historical Item 2.02 candidates; the full submissions resume can grow
that queue. The CVX wave is neither full historical source coverage nor proof
of the provider SLO. Schedule further measured source waves and affected
downstream reruns after the first full fundamentals build.

## Full downstream run5

After the CVX source receipt is inspected, start at `statement_points`.
This selected suffix includes corrected shares, periods, TTM, calendarization,
standardization, reconciliation (16 sequential shards), derived/market data,
universe and delisting work, forward returns, item/provider coverage, price
metrics, and final quality.

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 2 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-run5-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-run5.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-run5.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 `
  --start-stage statement_points --force --shards 16 `
  --memory-limit 1GB --threads 1 --backup-keep 100 `
  --run-id activation-run5 `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

`--shards 16` is partitioning: the reconciliation runner invokes one child at a
time. A failed stage is an inspectable capacity/correctness task; it is not a
reason to skip a later stage, widen memory, or reduce output scope.
The run5 command has no `--earnings-release-cik` or other CIK filter: it must
materialize the full universe.

## Fundamental signal research after run5

Run5 rebuilds the selected-input lineage and publishes forward-label basis
under migrations0323 through0325. The FQ1/FQ2 research commands below operate
on an existing migrated warehouse; they do not initialize or migrate it.
First inspect dated historical US-common membership and CIK coverage, then
choose an explicit bounded decision-date range and measure its build cost.
Current directory rows must never be backdated to fill a historical cohort.
An empty/blocked panel is diagnostic evidence, not a successful alpha sample.
Do not launch an unmeasured full-history panel while the host is constrained.

Replace the two date placeholders with that reviewed range. These are command
templates, not executed results. Wrap each invocation in run_memory_guarded.py
with its own fresh receipt/stdout/stderr, the existing2GiB process cap and host
thresholds. The256MB DuckDB setting is the research builder's default, not a
change to the1GB source-load budget. Run serially after the warehouse writer
is terminal. IDs must match `[a-z][a-z0-9_]{0,63}` and be unused; update the
prepared acceptance SQL if using different IDs.

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe scripts\research_fundamental_signals.py build `
  --db-path data\warehouse.duckdb --start-date REVIEWED_START_DATE `
  --end-date REVIEWED_END_DATE --as-of-date 2026-09-20 `
  --run-at 2026-09-20T22:00:00+00:00 --run-id fundamental_signals_build1 `
  --memory-limit 256MB --threads 1

# Evaluate only a completed panel; validation rejects stale/tampered manifests.
C:\atx\atx-db\.venv\Scripts\python.exe scripts\evaluate_fundamental_signals.py `
  --db-path data\warehouse.duckdb --build-run-id fundamental_signals_build1 `
  --run-id fundamental_signals_evaluation1 --as-of-date 2026-09-20 `
  --run-at 2026-09-20T22:00:00+00:00 `
  --label-source atx_forward_returns_survivorship_safe_v1 `
  --memory-limit 256MB --threads 1
```

Inspect `sql/research/fundamental-signal-decile-acceptance.sql` afterward. It
retains all five predeclared hypotheses, three splits and three horizons,
including missing results and label attrition. A short capacity sample cannot
establish the required252 spread dates and annual stability. Preserve all
predeclared results before extending the sample; do not choose hypotheses or
date ranges from favorable forward returns. Statistical candidates remain
research outputs with `production_eligible=false`.

## Required post-run outputs

Do these serially after a successful run5 receipt. `build_custom_features.py`
is deliberately outside `STAGE_ORDER`; the forward-label stage makes the
pre-existing eight-hypothesis build eligible for evaluation but does not run it.
Resolve `ACTUAL_CF1_BUILD_RUN_ID` from the persisted CF1 run manifest. The
deterministic snapshot fields remain `--as-of-date 2026-09-20` and
`--run-at 2026-09-20T22:00:00+00:00`; `run_at` is the explicit CF1 manifest
timestamp, not the wall-clock time at which the operator invokes the command.
Evaluation derives its label cutoff from the as-of snapshot (2026-09-20 22:00),
not from the operator's invocation time. Use a new evaluation run ID.

```powershell
# Evaluate the already-built eight CF1 hypotheses after forward labels exist.
C:\atx\atx-db\.venv\Scripts\python.exe scripts\build_custom_features.py evaluate `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 `
  --run-at 2026-09-20T22:00:00+00:00 --run-id ACTUAL_CF1_EVALUATION_RUN_ID `
  --build-run-id ACTUAL_CF1_BUILD_RUN_ID --memory-limit 1GB

# Read-only production metrics: activation lineage, item and provider coverage,
# recorded quality, historical gaps, and the CF1 run/evaluation inventory.
C:\atx\atx-db\.venv\Scripts\python.exe scripts\measure_tier1_readiness.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --memory-limit 1GB `
  --include-cf1 --include-fundamental-signals `
  --output-json C:\atx\.superpowers\sdd\tier1-parity\tier1-run5-measurement.json `
  --output-markdown C:\atx\.superpowers\sdd\tier1-parity\tier1-run5-measurement.md

# Only after the measured gates are assessed as eligible, emit the first release
# and its manifest/hash. Pick a new release ID and nonexistent output directory.
# The current CLI has no --created-at flag: it records its actual UTC invocation
# time internally, so do not pass an unsupported timestamp parameter.
C:\atx\atx-db\.venv\Scripts\python.exe scripts\publish_release.py `
  --db-path data\warehouse.duckdb --release-id ACTUAL_RELEASE_ID `
  --out-dir ACTUAL_NEW_RELEASE_DIRECTORY --memory-limit 1GB --threads 1 `
  --run-id ACTUAL_RELEASE_RUN_ID
```

Guard the CF1, measurement, and publication commands as well when they run on
this host; each needs a new receipt/output tuple. The measurement command
refuses to overwrite either evidence file, so those two paths must be fresh.
Publish only after reviewing the measured item/provider/quality thresholds and
the CF1 evaluation (holdout decile spreads, label coverage, HAC uncertainty,
Holm adjustment, and transaction-cost sensitivities). `production_eligible`
remaining false is recorded evidence, not a reason to relabel the signals.

After research evaluation, execute the versioned CVX quarterly EPS acceptance SQL,
the fundamental desk acceptance SQL, and both price-feature and fundamental
signal decile readout SQL
under separate guarded, read-only sessions; record actual results and gaps.
Run readiness measurement, assess item/provider/all-quality gates without
weakening thresholds, regenerate and check the data dictionary, and publish the
first release only if measured gates permit. Verify its manifest and hashes.
The 426,151 pre-resume candidates plus any added historical earnings-source
backfill remain open and cannot be counted as provider SLO proof. Run the full
non-slow suite once at the sprint gate, obtain a fresh whole-branch Codex
review, then ask before merging to main. Preserve `stash@{0}`; never apply or
drop it.
