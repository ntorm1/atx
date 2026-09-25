# Production resume sequence — current 2026-09-25 11:20UTC (OPS-A)

The snapshot stays **2026-09-20**. Execute from `C:\atx\atx-db`, one guarded
process tree at a time. Use fresh artifact/run names; inspect actual process,
terminal ledgers and receipts before proceeding. Never infer completion from
an empty stage or infer an active process from a running ledger. Preserve all
12 backups, unrelated work and **stash@{0}**. Do not merge without approval.

Completed prerequisites: UM1 populated upgrade, SM1 source recycling, SI1
migration0326, MM1 governed migration, CC1 live target-index repair, SA1 bounded
submissions directory, LR2 live archive18 recovery, SM2 bounded unresolved-issuer
summary (committed `7080a478`, reviewed, 5 focused passes) and the archive19
session-kill ledger recovery (`da06924b`). Their accepted reviews and checks
stand. Schema326 is verified; do not repeat migrations or recoveries (archive18
and archive19 recoveries are both DONE). Prior commands are retained in Git
history and the dated program/run ledgers.

Archive18 recovery completed at0.315971375GiB peak under0.5GiB, with all original
scans and CHECKPOINT. Both stale ledgers are failed; no new attempt rows.
Raw counts (unchanged through archive19): 47,941,000 facts/points each,
31,959,271 bars, 31,934,514 features. These counts do not qualify canonical
fundamentals, provider coverage or release eligibility.

**ACTIVE since 2026-09-25T11:09:07Z: archive20** (sole warehouse writer, launched
detached by OPS-A; see the archive20 section below for PIDs, liveness checks and
terminal inspection). Do NOT start any other warehouse job while it is alive.

Resource profiles: CompanyFacts and the accepted SA1 submissions experiment use
512MB/one thread,1.5GiB native cap,120s sustained4GiB physical/6GiB commit.
Other production stages retain1GB/one thread,2GiB cap and120s sustained6/8GiB
until separately measured. Guard preflight and1.5/3GiB host emergency stops
remain unchanged. Use the data-path3GiB disk floor for every runtime workload.
A failed capacity trial requires diagnosis; keep scope and proof unchanged.

## CompanyFacts archive19 — TERMINAL failed (session kill), ledgers recovered

Launched 2026-09-25T00:53:55Z (window4 ready), predecessor a4942a4b. Full
retained proof passed (receipts 9,462 / lineage 16 / verified rows 39,457,715).
Killed ~01:11:43Z when controller session 1 closed (guard + child PID 10272
killed; guard receipt stayed stale `running`; not a guard stop, not a source
failure). Last progress 6350/20390 loaded=5398 empty=923 unavailable=29
failed=0 rows=0. Dataset UUID **e27c8a4e-2d29-47bb-b657-fc1855ea4cec**.

Recovery (OPS-A, `claude-ctl\close_companyfacts_archive19_session_kill.py`, LR2
pattern, guarded 0.5GiB, peak 0.344GiB, window `archive19-recovery-window1`
3/5GiB ready): activation row and dataset row both `failed`, finished_at
2026-09-25 10:35:44.937122 (operator recovery time), rows 0, CHECKPOINT passed
(the 81,056B WAL was absorbed). Receipt `companyfacts-archive19-session-kill-recovery.json`;
liveness proof `activation-companyfacts-archive19-process-check.json`.

`rows=0` verdict (read-only `claude-ctl\archive19_rows_verdict.py`, receipt
`companyfacts-archive19-rows-verdict.json`): the counter is correct, not a
flush-time artifact and not a defect. `rows` counts only facts written by the
replay path (`_replace_facts`); verified members skip facts and only increment
`loaded`. Archive19's processed prefix 0..6349 was 5,398 verified loaded + 923
empty + 29 unavailable + **0 unreceipted**; archive19 owns 0 facts / 0 points and
committed 925 empty + 29 unavailable receipts through 01:11:42.74Z. Targets are
CIK-ascending; the lineage-receipted prefix is contiguous through position
10,831 (CIK 0001496383). **First unwritten member = position 10,832, CIK
0001496443.** Remaining: 4,064 verified skips, 404 empty and 14 unavailable
replays, then **9,558 members to write** (3,497 of them replace legacy
non-lineage facts owned by run c6bfbb1e, 8,483,285 rows). The last real fact write
was archive12 (2026-09-23 00:26Z); archive13 (OOM, old index design) and
archive17 (FATAL candidate index at CIK 0001495229, repaired by CC1) failed at
or just before that frontier. Archive20 is the first attempt to reach it since
CC1.

## Full CompanyFacts archive20 — RUNNING (launched 2026-09-25T11:09:07Z)

Resume semantics (verified in `_companyfacts_resume._lineage`): the predecessor
must be a terminal failed/source-incomplete **dataset UUID**; lineage follows
`params_json.resume_from_run_id` (max 32) and requires each ancestor's
finished_at <= its successor's started_at. Archive20 resumes from archive19's
UUID (same pattern as archive19 → archive18's UUID); lineage length 17, accepted
at start (`receipts=9462 lineage_runs=17`). Window `source-archive20-lowmemory-window4`
ready 11:08:51Z (windows 1–3 failed: physical 2.8–3.9GiB).

Launched DETACHED: `Start-Process powershell -WindowStyle Hidden -PassThru
-ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',
'C:\atx\.superpowers\sdd\tier1-parity\claude-ctl\launch-archive20.ps1'`.
Launcher powershell PID **11544** (orphaned from its tool parent; alive), venv
shim 16160 → guard interpreter **9060**, guard `child_pid` **14644** (venv shim)
→ worker interpreter **19636**. Launch receipt
`activation-companyfacts-archive20-launch.json`; guard stdout + final
`guard_exit=` line in `activation-companyfacts-archive20-launcher.log`.
Executed command (inside the launcher):

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 1.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive20-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive20.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-companyfacts-archive20.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --only companyfacts_load `
  --companyfacts-symbol-source archive_members --companyfacts-replace-existing `
  --companyfacts-resume-from-run-id e27c8a4e-2d29-47bb-b657-fc1855ea4cec `
  --memory-limit 512MB --threads 1 --backup-keep 100 --force `
  --run-id activation-companyfacts-archive20 `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

**Liveness (light, poll >= 5 min apart; never open the warehouse while alive):**

```powershell
$ctl='C:\atx\.superpowers\sdd\tier1-parity'
Get-Process -Id 11544,9060,14644,19636 -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,StartTime
Get-Content "$ctl\activation-companyfacts-archive20-memory.json" | Select-String 'status|returncode|peak|physical|commit'
Get-Content "$ctl\activation-companyfacts-archive20.err" -Tail 2
Get-Content "$ctl\activation-companyfacts-archive20-launcher.log" -Tail 1   # 'guard_exit=N' once finished
```

A progress line whose `rows=` stays 0 is expected until processed passes
10,832; after that `rows` and `loaded` must grow. Watch the first ~50 members
past 10,832 (first new writes since archive12) and CIK 0001495229 (~10,8xx,
archive17's FATAL candidate-index point, now in the verified path).

**Terminal inspection (only after all four PIDs are gone and the guard receipt
status is `completed`/`failed`/`stopped_*`):**

1. Read `activation-companyfacts-archive20-memory.json` (status, returncode,
   native_peak_job_memory_gb), `-launcher.log` last line, `.err` tail and the
   stage JSON payload in `.log` (loader details: outcome, completed/replayed/
   resumed targets, empty/unavailable/failed counts, facts, fundamental_points).
2. Observe a 3/5GiB window with a new receipt, then run the read-only inspector
   under the 0.5GiB guard with new receipt names:

```powershell
Set-Location C:\atx\atx-db; $ctl='C:\atx\.superpowers\sdd\tier1-parity'; $py='C:\atx\atx-db\.venv\Scripts\python.exe'
& $py "$ctl\observe_headroom_window.py" --physical-gib 3 --commit-gib 5 --receipt "$ctl\archive20-inspect-window1.json"
& $py "$ctl\run_memory_guarded.py" --job-gb 0.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt "$ctl\archive20-inspect1-memory.json" --stdout "$ctl\archive20-inspect1.log" --stderr "$ctl\archive20-inspect1.err" `
  -- $py "$ctl\claude-ctl\inspect_companyfacts_terminal.py" --activation-run activation-companyfacts-archive20 `
  --out "$ctl\companyfacts-archive20-terminal-inspection.json"
```

   Outcome-1 acceptance = its `acceptance` block all true: activation
   `completed`, dataset `succeeded`, `members_without_disposition` 0 of 20,390,
   `loaded_receipt_fact_count_mismatches` 0, no `error` dispositions, and
   `fact_ciks_without_lineage_loaded_receipt` 0 (legacy c6bfbb1e rows replaced);
   plus guard exit 0 and nonzero `attempt_facts`/`attempt_points` with CIK and
   period ranges. Empty/unavailable members count as dispositions only with
   their explicit reasons (`non_loaded_reasons`). Smoke-tested on archive19
   (0.328GiB peak): 9,462 loaded receipts match retained facts exactly.
3. If archive20 stops (guard stop, session kill, or source failure): prove
   process absence, close its ledgers with a copy of
   `claude-ctl\close_companyfacts_archive19_session_kill.py` adapted to the
   actual stop evidence (archive18 script for guard stops), then launch
   archive21 resuming from **archive20's dataset UUID**. Max 3 relaunches for
   this stage (archive19 was the first under OPS-1/OPS-A; archive20 second).
   A DuckDB FATAL/INTERNAL or loader exception at the frontier is a code
   defect: stop and report, do not patch src/.

Require actual complete source coverage/dispositions before advancing through
full submissions, scoped CVX source and full-universe run5. A source error or
host kill leaves an explicit dependency and a terminal-state inspection task.

## Submissions verified resume

Run only after the source resume is terminal and inspected:

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 1.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 `
  --only submissions_load --submissions-batch-size 50 `
  --submissions-resume-from-run-id 04cf947d-53bb-49b7-a276-b3c74a2a52c8 `
  --memory-limit 512MB --threads 1 --backup-keep 100 --force `
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
  --job-gb 2 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
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
  --job-gb 2 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
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
qualification of a production-stage budget. Run serially after the warehouse writer
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

The ED1 CVX reader has already run twice. Desk1 exposed the now-repaired
schema prerequisite; desk2 reached schema326 but returned
issuer_ownership_unresolved, zero qualified/numeric observations, exit2.
Run desk3 below only after source and downstream materialization:

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 2 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk3-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk3.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk3.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\read_quarterly_eps_growth.py `
  --db-path data\warehouse.duckdb --cik 0000093410 `
  --content-as-of 2026-09-20T22:00:00Z `
  --start 2025-10-01 --end 2026-07-01 --latest 3 `
  --output-json C:\atx\.superpowers\sdd\tier1-parity\cvx-eps-desk3.json
```

Exit0 means the requested count of qualified numerical observations, not
release certification. Inspect the exact three fiscal periods and compare
the documented independent source benchmark; current lookup clocks do not
qualify historical market identity. Missing/ambiguous/NULL/lineage-rejected
states return explicit diagnostics and exit2. Source gaps are never repaired
by subtracting cumulative EPS.

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 2 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
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
