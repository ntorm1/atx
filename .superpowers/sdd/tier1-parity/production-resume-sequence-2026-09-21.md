# Production resume sequence — 2026-09-21

Static source/dispatch audit only. The production snapshot remains **as of
2026-09-20**. Run one guarded process tree at a time from `C:\atx\atx-db`.
Every guarded invocation needs a fresh receipt, stdout, and stderr filename.
Keep DuckDB at `1GB` and one thread, retain `--backup-keep 100`, and use the
dummy SEC User-Agent `atx-db/0.1 atx-research@example.com`.

## Current source position and guard prerequisite

Archive5 is terminal. Root recovered its actual companyfacts dataset UUID as
`6400b3c2-f0d1-4f47-bcf0-95aadd9241de`, with 43,417,902 retained raw facts and
points and schema 0319. Archive6 is now the sole live writer: it began at
21:54:46 UTC under the root-approved two-GiB guard, resuming from that UUID and
verifying 6,570 receipts. Its 21:57 UTC snapshot was 1.58 GiB private / 1.14
GiB working, with 3.57 GiB physical and 9.68 GiB commit headroom. Do not launch
another companyfacts command or reuse archive6's filenames while it is live.

The guard's preflight remains `job_gb + 2` for both physical and commit
headroom, and its runtime stop thresholds remain physical below 1.5 GiB or
commit below 3 GiB. The two-GiB ceiling was a root production decision; it did
not change DuckDB's one-GiB/one-thread limits or the loader's bounded batches.
After archive6 is terminal, inspect its receipt, activation-stage ledger, and
new companyfacts dataset receipt before advancing. A pending migration, if any,
is governed before the loader opens; `--backup-keep 100` applies only after that
governed path succeeds.

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

## Full downstream run5

After the submissions receipt proves completion, start at `statement_points`.
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

## Concrete source mismatches resolved here

* `production-resume-after-cf5.md` still names archive4 UUID
  `c7c21cd1-c7c4-4de8-af64-4f2877ef6f59`; after archive5's terminal recovery,
  the next resume must use `6400b3c2-f0d1-4f47-bcf0-95aadd9241de`.
* Both `--backup-keep 100` and `--backup-keep=100` are valid argparse forms;
  there is no flag-spelling defect. The outer guard owns the job cap; DuckDB
  resource flags belong to the child activation command.
* CF1 evaluation has no activation stage. Omitting the separate evaluator would
  leave `custom_feature_evaluations` unrefreshed even if run5 reaches `quality`.
* `publish-release` accepts `--release-id`, `--out-dir`, optional
  `--previous-dir`, `--memory-limit`, `--threads`, and `--run-id`; it has no
  `--created-at` CLI argument. Current `cli.py` supplies `datetime.now(UTC)` to
  the publication library, so an operator-facing fixed release timestamp is
  unavailable without a source change.

No main merge follows this sequence. Preserve `stash@{0}`; after the release
evidence, run the full non-slow suite once, obtain the whole-branch Codex review,
and ask before merging to main.
