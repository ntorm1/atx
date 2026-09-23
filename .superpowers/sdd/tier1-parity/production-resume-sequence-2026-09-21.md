# Production resume sequence — updated 2026-09-23

Static source/dispatch audit only. The production snapshot remains **as of
2026-09-20**. Run one guarded process tree at a time from `C:\atx\atx-db`.
Every guarded invocation needs a fresh receipt, stdout, and stderr filename.
Keep DuckDB at `1GB` and one thread, retain `--backup-keep 100`, and use the
dummy SEC User-Agent `atx-db/0.1 atx-research@example.com`.

## Current source position and guard prerequisite

Root session 45906 (guard child 2324) is the sole live warehouse writer:
`activation-companyfacts-archive12`. It resumes the recovered archive11 dataset UUID
`694f056e-a268-4927-8500-990b62c9a9be` with full `archive_members`,
replacement, and `--force`. Its files are
`activation-companyfacts-archive12-{memory.json,log,err}`. The last verified
durable raw-fact and point count was 47,906,807, after archive11 recovery. Do not launch
another runtime or restart because the tool session yields. Treat schema
0322 as live: archive11 terminal inspection and recovery confirmed it. Archive11
added no source facts before the headroom guard stopped its verification;
both ledgers were recovered and checkpointed at00:08:15UTC on2026-09-23.

The guard's preflight remains `job_gb + 2` for both physical and commit
headroom, and its runtime stop thresholds remain physical below 1.5 GiB or
commit below 3 GiB. The two-GiB ceiling does not change DuckDB's one-GiB/
one-thread limits or the loader's bounded batches. After archive12 is terminal,
root must inspect its guard receipt, activation-stage ledger, actual companyfacts
dataset ledger and retained counts. If it failed, resolve the latest dataset
UUID from the ledger before constructing another verified resume; never infer
it from the activation run label. A pending migration is governed before the
loader opens; `--backup-keep 100` applies only after that governed path succeeds.
Use fresh receipt/log/error filenames for every subsequent attempt.

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
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --memory-limit 1GB --include-cf1 `
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

After CF1 evaluation, execute the versioned CVX quarterly EPS acceptance SQL,
the fundamental desk acceptance SQL, and the custom-feature decile readout SQL
under separate guarded, read-only sessions; record actual results and gaps.
Run readiness measurement, assess item/provider/all-quality gates without
weakening thresholds, regenerate and check the data dictionary, and publish the
first release only if measured gates permit. Verify its manifest and hashes.
The 426,151 pre-resume candidates plus any added historical earnings-source
backfill remain open and cannot be counted as provider SLO proof. Run the full
non-slow suite once at the sprint gate, obtain a fresh whole-branch Codex
review, then ask before merging to main. Preserve `stash@{0}`; never apply or
drop it.
