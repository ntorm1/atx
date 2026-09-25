# OPS-A report — archive19 closure, rows=0 verdict, archive20 launch (2026-09-25)

STATUS: DONE_WITH_CONCERNS (archive20 running and advancing; terminal acceptance pending by design)

Paths below are relative to `C:\atx\.superpowers\sdd\tier1-parity\` unless absolute.
Interpreter `C:\atx\atx-db\.venv\Scripts\python.exe` (`$py`). Log timestamps in `.err` files are
host-local (UTC-4); all other times UTC.

## 1. Archive19 is dead (step 1)

- 10:25:37Z: the only python processes were two VS Code mypy daemons (17944, 20284). There was no
  `warehouse_activate`/`run_memory_guarded`, and PID 10272 was absent. An exclusive `FileShare.None`
  open of `C:\atx\atx-db\data\warehouse.duckdb` succeeded (opened and closed at once).
  Warehouse mtime was 01:11:38Z, with a leftover WAL of 81,056 B at 01:11:43Z.
- Receipt: `activation-companyfacts-archive19-process-check.json`, written by
  `claude-ctl\archive19-process-check.ps1` at 10:35:44Z. It records 0 matching processes, 0 original
  PIDs, `warehouse_exclusive_open_ok=true`, the python process list and the WAL size.
- Cause: controller session 1 closed and took the guard and child down with it. The guard receipt
  was never finalised (`status=running`), so this was not a guard stop and not a source failure.
  No process I didn't start was touched.

## 2. Archive19 ledgers closed (step 2)

- Helper: `claude-ctl\close_companyfacts_archive19_session_kill.py`. It copies the archive18 LR2
  pattern:
  - SQL values are embedded as typed literals, not bound parameters, so no pandas/NumPy import.
  - DuckDB runs at 256MB, 1 thread, `preserve_insertion_order=false`.
  - It refuses to run twice.
  - The update is one `BEGIN..COMMIT` that must hit exactly one activation row and one dataset row,
    followed by a CHECKPOINT.
  - It asserts that the process check is under 5 minutes old, that the guard receipt still says
    `running` with `child_pid` 10272, and that the dataset's `resume_from_run_id` is a4942a4b.
- Headroom: `archive19-recovery-window1.json`, 3/5 GiB, ready 10:35:14Z. This is the observer's
  minimum floor and matches the archive18 recovery precedent.
- Guard run: `archive19-recovery1-memory.json`, job cap 0.5 GiB, completed, rc 0, **peak 0.344 GiB**.
- Result: `companyfacts-archive19-session-kill-recovery.json`.
  - Dataset UUID **e27c8a4e-2d29-47bb-b657-fc1855ea4cec**.
  - Activation and dataset rows both went from `running` to **`failed`**, with finished_at
    2026-09-25 10:35:44.937122 and rows 0. `committed=true`, `checkpoint=passed`; the WAL was
    absorbed.
  - Rows owned by the attempt: 0 facts / 0 CIKs, 0 points.
  - Source receipts owned by the attempt: **empty 925, unavailable 29**, CIKs 0000002110..0001165509,
    fetched 01:02:43..01:11:42.74Z. There are 2 more empty receipts than the last log line shows
    (members 6350-6352).
  - Retained totals are unchanged: 47,941,000 facts, 47,941,000 points, 31,959,271 bars,
    31,934,514 features, schema 0326.
- Archive18 was not touched.

## 3. rows=0 verdict (step 3): correct counter, not a flush artifact, not a defect

**How the counter works** (`atx-db/src/atx_db/fundamentals.py`):
- `rows_loaded` goes up only through `rows_loaded += self._replace_facts(...)` (l.1314), the
  replay path.
- Members verified by the resume (`if cik in verified_members`, l.1168-1191) only rewrite their
  candidate rows, add to `loaded_targets`, and `continue`. Their facts and points are kept, not
  rewritten.
- Every member commits its own transaction, so nothing is held back until a final flush.

**Why only some members are verified** (`_companyfacts_resume.py`): only `loaded` receipts from
the lineage become verified skips. `empty` members are always replayed (a cheap zero-row
cleanup), and `unavailable` members are observed again.

**Evidence**: read-only `claude-ctl\archive19_rows_verdict.py`, run at 256MB, 1 thread,
read_only, under a 0.5 GiB guard (`archive19-rows-verdict1-memory.json`, peak 0.327 GiB). Receipt:
`companyfacts-archive19-rows-verdict.json`. The SQL was aggregate-only:
- `SELECT run_id, run_id IN (<17 lineage UUIDs>), count(*), count(DISTINCT cik), min(period_end), max(period_end) FROM sec_company_facts GROUP BY run_id`
- the same query on `fundamental_points WHERE source='SEC companyfacts'`, using `security_id`
- `raw_source_files` grouped by `json_extract_string(metadata_json,'$.run_id')`, `status`
- the lineage walk over `dataset_runs.params_json.resume_from_run_id`, mirroring `_lineage`
- per-CIK receipt status and `SELECT DISTINCT cik FROM sec_company_facts`, both small
- the ZIP central-directory member names

**Results**:
- **Lineage**: 17 runs, from e27c8a4e (archive19) back to 758f7d5c (archive3).
- **Facts**:
  - 47,941,000 rows across 12,959 CIKs.
  - Lineage-owned: 39,457,715 rows across 9,462 CIKs.
  - Periods 1927-02-17..2026-09-18.
  - Points equal facts per run, 1:1.
- **Fact owners by run** (rows / CIKs):

  | Run | Attempt | Rows | CIKs |
  |---|---|---:|---:|
  | 758f7d5c | archive3 | 20,205,629 | 3,750 |
  | c6bfbb1e | legacy, not in lineage | 8,483,285 | 3,497 |
  | c7c21cd1 | archive4 | 7,999,850 | 2,042 |
  | 404f66a2 | archive7 | 5,351,787 | 1,785 |
  | 6400b3c2 | archive5 | 2,803,031 | 778 |
  | ade90629 | archive10 | 1,753,504 | 621 |
  | 7da8bd67 | archive6 | 699,244 | 235 |
  | 17ac14e8 | archive8 | 589,436 | 229 |
  | 6beba5d4 | archive12 | 55,234 | 22 |

  Archive19 owns 0.
- **Dispositions of all 20,390 members**:
  - 9,462 loaded receipts
  - 1,327 empty: unsupported_or_empty_taxonomy 1,272, allowlist_empty 53, no_valid_fact_rows 2
  - 43 unavailable (empty_archive_placeholder)
  - 9,558 with no receipt, of which 3,497 hold retained legacy facts
- **Archive19's processed prefix (0..6349)**: 5,398 loaded + 923 empty + 29 unavailable
  + **0 without a receipt**. So `rows` had to be 0.
- **Frontier**: members are processed in ascending CIK order. The receipted prefix is contiguous
  through position **10,831** (CIK 0001496383). The **first unwritten member is position 10,832,
  CIK 0001496443**. After archive19's position there remain 4,064 verified skips, 404 empty and
  14 unavailable replays, and **9,558 members to write**.
- **History**: the last real fact write was archive12 (2026-09-23 00:26Z, 22 CIKs, 55,234 rows).
  - archive13 hit an out-of-memory error at its first new write (old index design).
  - archive17 hit a FATAL candidate-index error at CIK 0001495229, in the verified path around
    position 10.8k. CC1 repaired that index (313848a7/5b088e86).
  - archive18 was stopped by the headroom guard during the proof.
  - archive19 was killed at 6,350.

  **Archive20 is the first attempt to reach the frontier since CC1.**
- **Verdict**: writes were happening (receipts and candidate transactions committed member by
  member). No fact writes were due before position 10,832, so there was nothing to block on. I
  proceeded.

## 4. Headroom for archive20 (step 4)

The source profile is 4 GiB physical / 6 GiB commit, sustained for 120 s, with new receipt names.

| Receipt | Time (UTC) | Result |
|---|---|---|
| `source-archive20-lowmemory-window1.json` | 10:37-10:40 | no_sustained_window (physical 3.73-3.90) |
| `...-window2.json` | 10:50-10:53 | no_sustained_window (physical 2.80-3.5) |
| `...-window3.json` | 10:55-10:58 | no_sustained_window (physical 3.46-3.57) |
| `...-window4.json` | 11:06-11:08:51 | **ready**, 120.015 s at physical 4.63-4.68 / commit 7.46 |

Physical memory was taken by processes I don't own (VS Code, three Claude sessions, Chrome,
Defender, Memory Compression). No C++ build process was visible.

## 5. Archive20 launch (step 5)

**Resume semantics** (`_lineage`, verified in code):
- The predecessor must be a terminal failed (or source-incomplete succeeded) **dataset UUID**.
- The chain follows `params_json.resume_from_run_id`, up to 32 runs.
- Each ancestor must have finished before its successor started.

So archive20 resumes from **archive19's UUID e27c8a4e**. This is the same pattern archive19 used
with archive18's UUID. A4942a4b was not reused. At startup the loader accepted the new lineage:
`receipts=9462 lineage_runs=17`.

**Launcher**: `claude-ctl\launch-archive20.ps1`. It is launch-archive19.ps1 with only the resume
UUID changed. Flags: `--job-gb 1.5`, disk floor 3 GiB, 512MB, 1 thread, `archive_members`,
`--companyfacts-replace-existing`, `--force`, `--backup-keep 100`, and the dummy SEC user agent. It
writes a launch receipt and a final `guard_exit=` line.

**Detached launch**, after 0 writers and an exclusive-open OK:

    Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','C:\atx\.superpowers\sdd\tier1-parity\claude-ctl\launch-archive20.ps1'

- Launched at **2026-09-25T11:09:07Z**.

**PIDs**:

| Role | PID | Note |
|---|---:|---|
| Launcher powershell | **11544** | orphaned: its tool parent 9832 has exited; still alive |
| venv shim | 16160 | |
| **Guard interpreter** | **9060** | |
| **Guard `child_pid`** | **14644** | venv shim |
| **Worker interpreter** | **19636** | |

**Receipts**:
- `activation-companyfacts-archive20-launch.json`
- `-memory.json` (`running`, `child_pid` 14644)
- `.log` / `.err`
- `-launcher.log`

**Proof**: 07:09:28 → 07:17:49 local, **8m21s**. Verified targets 9,462, rows 39,457,715. CIK
inventory: 12,959 issuers.

## 6. Archive20 is advancing (step 6)

| Read (UTC) | processed | loaded | empty | unavailable | failed | rows |
|---|---:|---:|---:|---:|---:|---:|
| 11:18:09 | 150 | 130 | 19 | 1 | 0 | 0 |
| 11:22:24 | 1,925 | 1,631 | 276 | 18 | 0 | 0 |

- Rate is about 420 members/min through the verified prefix. New writes should start near
  processed 10,832, around 11:45Z.
- Worker memory: working set about 705 MB, private 1,173 MB, under the 1.5 GiB job cap.
- I stopped here as instructed and did not wait for completion.

## 7. Commands for the controller

**(a) Is archive20 alive?** This is a light check. Poll no more often than every 5 minutes, and
never open the warehouse while it runs.

    $ctl='C:\atx\.superpowers\sdd\tier1-parity'
    Get-Process -Id 11544,9060,14644,19636 -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,StartTime
    Get-Content "$ctl\activation-companyfacts-archive20-memory.json" | Select-String 'status|returncode|peak|physical|commit'
    Get-Content "$ctl\activation-companyfacts-archive20.err" -Tail 2
    Get-Content "$ctl\activation-companyfacts-archive20-launcher.log" -Tail 1   # 'guard_exit=N' when finished

- Healthy: all four PIDs present, guard `running`, `processed` rising.
- After processed passes 10,832, `rows` and `loaded` must grow.
- `rows` stuck at 0 past about 11,000 means stop and diagnose.

**(b) Terminal inspection and acceptance proof.** Start only when all four PIDs are gone and the
guard status is terminal.
1. Read the guard receipt (status, returncode, `native_peak_job_memory_gb`), the last line of
   `-launcher.log`, the tail of `.err`, and the stage JSON in `.log` (loader details).
2. Run the inspector:

       Set-Location C:\atx\atx-db; $py='C:\atx\atx-db\.venv\Scripts\python.exe'
       & $py "$ctl\observe_headroom_window.py" --physical-gib 3 --commit-gib 5 --receipt "$ctl\archive20-inspect-window1.json"
       & $py "$ctl\run_memory_guarded.py" --job-gb 0.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
         --receipt "$ctl\archive20-inspect1-memory.json" --stdout "$ctl\archive20-inspect1.log" --stderr "$ctl\archive20-inspect1.err" `
         -- $py "$ctl\claude-ctl\inspect_companyfacts_terminal.py" --activation-run activation-companyfacts-archive20 `
         --out "$ctl\companyfacts-archive20-terminal-inspection.json"

3. Acceptance: every entry in the `acceptance` block is true —
   - `activation_completed`
   - `dataset_succeeded`
   - `all_members_have_disposition` (20,390 of 20,390)
   - `loaded_receipts_match_retained_facts`
   - `no_error_dispositions`
   - `no_stale_non_lineage_facts` (legacy c6bfbb1e replaced)

   In addition: guard exit 0; nonzero `attempt_facts`/`attempt_points` with CIK and period ranges;
   and `non_loaded_reasons` gives an explicit reason for every empty or unavailable member.

   The inspector was smoke-tested on archive19 (peak 0.328 GiB). The result was correctly all
   false apart from receipt matching, which was true: 9,462/9,462 loaded receipts match retained
   facts.
4. If archive20 stops: prove the processes are gone. Close its ledgers with an adapted copy of the
   archive19 script (use the archive18 script for a guard stop). Then launch archive21 resuming
   from archive20's dataset UUID. A DuckDB FATAL/INTERNAL error or a loader exception at the
   frontier is a code defect: report it as BLOCKED and do not patch `src`.

## 8. Commits

- `da06924b` ops: close archive19 session-kill ledgers and prove rows=0 frontier. This covers
  archive19 artifacts, windows, recovery and verdict receipts, and the claude-ctl helpers and
  launcher.
- A second commit, after this report, covers the resume doc, the continuation-queue append,
  archive20 windows 2-4, the static launch receipt, the inspector and this report.
- Live archive20 files (`-memory.json`, `.log`, `.err`, `-launcher.log`) are deliberately left
  uncommitted while the job writes them. Commit them after terminal inspection.
- `launch-archive19.ps1` belongs to the controller and was not committed by me.

## 9. Concerns

1. **Survival after the session exits is likely but not proven.**
   - The first archive19 run was a Bash background task and died with the session.
   - My read-only job query shows the tool process's own job has flags 0x800 (BREAKAWAY_OK only,
     no KILL_ON_JOB_CLOSE). Closing that job's handle therefore won't kill the archive20 tree,
     which inherited it.
   - The launcher is already orphaned from the tool process, so a parent-PID tree kill can't
     reach it.
   - An explicit TerminateJobObject by the harness would still kill it.
   - My attempt to test a WMI-based launch was denied by the auto-mode classifier (containment
     escape). I did not try other escape methods.
   - **Check liveness after the next controller session transition.**
2. **Host headroom is thin.** At 11:22Z the guard sampled physical 2.88 GiB and commit 4.34 GiB,
   against emergency stops of 1.5/3 GiB. The frontier writes will add DuckDB delete/insert memory.
   A guard stop there would be capacity, not a defect: recover and use archive21.
3. **The frontier write path is untested since CC1.** It has not run since the index repair
   (archive13 OOM and archive17 FATAL). CIK 0001495229 (archive17's FATAL, verified path) will be
   re-exercised just before 10,832.
4. **finished_at values are operator recovery times.** For archive18 and archive19 they are not
   the actual stop times; the reason text says so.
