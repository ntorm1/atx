# Production resume sequence — updated 2026-09-24

**23:48UTC prerequisites complete:** CC1 live target-index repair is durably
verified, all 12,959 candidate rows/constraints/index definitions unchanged;
native peak 0.829GiB/cap 1GiB. SA1 9a7a3ea8 passed actual full retained-directory
measurement at 0.592GiB and affected focused/integration checks. No repeat of
either task is due. Source archive18 below is next after sustained headroom;
predecessor dd52e571-5786-42c5-bfaa-d7122b033912, 512MB/one thread, 1.5GiB cap,
full archive_members/replacement/force. All source and release gates remain.

**23:16UTC terminal:** archive17 passed full retained fact/point proof, then
failed candidate COMMIT on a persisted nonunique target-index inconsistency.
Native peak 1.223964691GiB at 512MB/one thread under 1.5GiB; no new raw facts.
Both ledgers terminal failed; no operator recovery due. CC1 physical index
repair must pass its backup/data/catalog/index verification before source
resume. Next actual predecessor is `dd52e571-5786-42c5-bfaa-d7122b033912`.
Use fresh **archive18** artifacts/run ID; do NOT execute the historical
archive17 command below again. Preserve full scope, replacement and proof.
SA1 bounded ZIP integration checks/retained-directory probe are also pending.

Next source command, ONLY after CC1 live repair is durably verified and the
120second 4/6GiB headroom window holds. DG1 adds a 3GiB disk floor without
changing memory stops or source scope. Execute from `C:\atx\atx-db`:

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 1.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive18-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive18.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive18.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --only companyfacts_load `
  --companyfacts-symbol-source archive_members --companyfacts-replace-existing `
  --companyfacts-resume-from-run-id dd52e571-5786-42c5-bfaa-d7122b033912 `
  --memory-limit 512MB --threads 1 --backup-keep 100 --force `
  --run-id activation-companyfacts-archive18 `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

The archive17 command farther below is retained historical evidence only.

**22:54UTC ACTIVE:** archive17 command below has started. Root session89002,
guard child7860; sole heavy job.9462receipts/14lineage runs inventoried and
full fact fingerprints running. Do not launch that command again or overlap
another heavy database/test workload. First inspect its actual process and
terminal artifacts. No source completion or new writes claimed yet.

**22:52UTC completed:** MM1 is06ab073f. Governed migrate0326b SUCCEEDED at512MB,
native peak1.209381104GiB under1.5GiB. migration0326-verify1 confirms applied
323..326, removed optional indexes, unchanged source constraints, cleared lock
and a matching retained backup SHA256. Do not run the migration command again.
All12backups remain. CVX desk2 now reaches the actual query but has unresolved
qualified ownership and zero numeric states; full materialization is still due.
Next command is full CompanyFacts archive17 below, after current headroom check.

**22:48UTC correction:** first migrate0326 failed at its final checkpoint under
the1.5GiB process cap; governed restore returned the warehouse to322. Catalog,
checksum and estimated-count equivalence/cleared lock plus backup hash verified
in migration0326-restore-proof2. MM1's512MB governed/restore startup budget passed
four focused real success/failure cases. After its commit, use migrate0326b
below with the SAME1.5GiB process cap. Archive17 has not started.

**2026-09-24 22:38UTC reconciliation:** both head6 failures are repaired and
accepted. UM1's populated0314 upgrade passed at256MB/one thread,1GiB cap,
0.859GiB peak. SM1 source-phase recycling/open-time budgets and SI1 optional
source-index migration0326 passed9isolated and17integration checks; root single
reviews are clean. Live catalog confirms schema322 and all3optional indexes.
After committing these accepted changes, governed migrate323..326 and full
archive17 are next. Do not repeat the prior passing batch or branch review.

User explicitly allows lower launch requirements after efficiency changes.
Use `low-memory-resume-profile-2026-09-24.md`: sustained4GiB physical/6GiB
commit120seconds,1.5GiB process cap for the next migration and source trials.
Migration and archive17 use512MB/one thread after MM1. These are
measured experiments pending actual new source commits, not release proof.

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

Archive17 uses the scoped low-memory profile above,fullarchive_members,
replacement,force,snapshot2026-09-20 and dummySECcontact. Other stages still
require their recorded capacity profile until separately measured. Runtime host stops
are1.5GiB physical/3GiB commit. A passed window does not guarantee future host
capacity; record any terminal state before another resume. The256MB/1GiB-cap
operator recovery was bookkeeping only, not a source-write capacity claim.

Full CompanyFacts->fullsubmissions->scopedCVXsource->fulluniverse run5 remains
the materialization sequence. Never infer a live process from a running ledger.

## Pending migration and full CompanyFacts resume

After the required schema/numeric checks pass, inspect free disk before the
governed migration. The last measured warehouse file was11.999GiB and free
C: space was34.494GiB on2026-09-23; these are dated observations, not reserved
capacity. Preserve all existing backups and retain --backup-keep100. Apply
0323..0326 under the experimental1.5GiB guard with fresh artifacts:

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 1.5 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-migrate0326b-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-migrate0326b.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-migrate0326b.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --only migrate `
  --memory-limit 512MB --threads 1 --backup-keep 100 --run-id activation-migrate0326b `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

Inspect the terminal migration receipt and actual applied versions. Only after
that job is terminal and the sustained source memory window is met, resume
the entire retained CompanyFacts archive. The argument is the actual failed
dataset UUID, not an activation label. Do not repeat archive16 recovery.

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 1.5 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive17-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive17.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive17.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --only companyfacts_load `
  --companyfacts-symbol-source archive_members --companyfacts-replace-existing `
  --companyfacts-resume-from-run-id 513cfbbc-096a-4186-9666-b6cc5170c4ad `
  --memory-limit 512MB --threads 1 --backup-keep 100 --force `
  --run-id activation-companyfacts-archive17 `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

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

After research evaluation, execute the qualified CVX reader below and both
price-feature and fundamental signal decile readout SQL under separate guarded,
read-only sessions; record actual results and gaps. The frozen CVX SQL v1/v2
remains a raw comparison diagnostic and does not qualify selected leaves. Use the
DS2 reader below for the fundamental desk screen: it validates the selected
completed default FQ1 panel before executing the prepared SQL in the same
transaction. Bare SQL is not evidence that its panel digest was validated.
The reader reports the latest observed decision session separately from the
September20 report date and keeps missing next-session research eligibility
separate from qualified accounting inputs. It does not invent a later entry
bar or include weekend filings in Friday's information set.

The ED1 CVX read was executed before materialization as cvx-eps-desk1 and
correctly returned schema_prerequisite_missing/exit2 with no numeric rows:
schema322 lacks migration323's selected_input_refs_hash/json. The next run
must use a fresh output tuple after migration and source/materialization:

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 2 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk2-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk2.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk2.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\read_quarterly_eps_growth.py `
  --db-path data\warehouse.duckdb --cik 0000093410 `
  --content-as-of 2026-09-20T22:00:00Z `
  --start 2025-10-01 --end 2026-07-01 --latest 3 `
  --output-json C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk2.json
```

Exit0 means the requested count of qualified numerical observations, not
release certification. Inspect the exact three fiscal periods and compare
the documented independent source benchmark; current lookup clocks do not
qualify historical market identity. Missing/ambiguous/NULL/lineage-rejected
states return explicit diagnostics and exit2. Source gaps are never repaired
by subtracting cumulative EPS.

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 2 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\fundamental-desk-screen1-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\fundamental-desk-screen1.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\fundamental-desk-screen1.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\read_fundamental_desk_screen.py `
  --db-path data\warehouse.duckdb --build-run-id fundamental_signals_build1 `
  --report-as-of 2026-09-20 `
  --output-json C:\atx\.superpowers\sdd\tier1-parity\fundamental-desk-screen1.json
```

All output paths must be new. The reader itself sets256MB/one-thread DuckDB
limits and bounds preview rows/bytes. A refusal is missing evidence, not a
passing empty screen. Never mark the live acceptance complete from tiny
fixtures or a prepared query alone.

Run readiness measurement, assess item/provider/all-quality gates without
weakening thresholds, regenerate and check the data dictionary, and publish the
first release only if measured gates permit. Verify its manifest and hashes.
The 426,151 pre-resume candidates plus any added historical earnings-source
backfill remain open and cannot be counted as provider SLO proof. Run the full
non-slow suite once at the sprint gate. The whole-branch Codex static review
of727e6b90 is recorded in whole-branch-review-2026-09-24.md; its Important and
Moderate repairs are accepted in09fed606 and9e0b4ffa with17focused checks.
Record any later production-repair review and affected paths; repeat review
only for Critical fixes under the standing ruling. Runtime gates remain open.
Then ask before merging to main. Preserve `stash@{0}`; never apply or
drop it.
