# Production resume sequence — updated 2026-09-23

Static source/dispatch audit only. The production snapshot remains **as of
2026-09-20**. Run one guarded process tree at a time from `C:\atx\atx-db`.
Every guarded invocation needs a fresh receipt, stdout, and stderr filename.
The next-stage commands below use DuckDB at `1GB` and one thread; retain `--backup-keep 100`, and use the
dummy SEC User-Agent `atx-db/0.1 atx-research@example.com`.

## Current source position and guard prerequisite

Archive15 is stopped: session15424 is missing and native20652/guard child18896
were absent at22:01UTC. The last receipt was stale-running and the last log was
point-fingerprint verification; no host-stop or source error is asserted.
Root recovered both stale ledgers22:01:31UTC and CHECKPOINT passed. No new
attempt rows; retained facts=points47,941,000 and schema0322.
Next actual predecessor: `4932365c-4b61-4437-b634-11b2a7284f6e`.
Evidence:companyfacts-archive15-interruption-recovery.json. Do not rerun recovery.

No warehouse writer is live. Archive16 waits for sustained headroom above
6GiB physical/8GiB commit (three observations over at least two minutes), then
uses unchanged2GiB job guard,1GB DuckDB,1thread,fullarchive_members,
replacement,force,snapshot2026-09-20,backup-keep100,dummySECcontact.
Current observations are inarchive16-headroom-observations.jsonl. Compiler
bursts returned during this turn, so a single good sample is insufficient.
Guard runtime thresholds remain1.5GiB physical/3GiB commit.
Use fresh archive16 filenames and prepare its terminal SQL from archive15's.

Archive13's512MB query limit failed atCOMMIT, so it is not the source-write
configuration. Dataset failure-ledger recovery is tested/reviewed in15235456.
That code handles exceptions but cannot execute after its process disappears.
After each stopped source job inspect guard and actual run ledgers, then derive
the next UUID. Never restart a live session or overlap warehouse runtimes.
Other workloads remain untouched.

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
