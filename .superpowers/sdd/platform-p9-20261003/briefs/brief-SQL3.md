# P9 lane briefs (paste one section, plus "Rules for every P9 lane", into an Opus 5.5 implementer's dispatch)

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md` (cited as "plan §n"). Finding ids (F-n, P9-Rn, NV-n, OR-n, FD-n, CM-n,
DS-n) and contracts (K-P9-n) are defined in plan §0 and §2.3. Literature ids (lit §n, [n], F1..F18) refer to
`docs/plans/2026-10-02-p9-literature-review.md`. Root fills `<frozen-sha>` and the pool at dispatch.

## Rules for every P9 lane

1. Read first: `.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding: never build C++, never run real data,
   never dispatch subagents, never push, never touch `atx-db/`, files only with the Write / Edit tools because the
   shell hook breaks heredocs), then plan §0.6 and §2.2-§2.3, then the review files your brief names. For C++:
   `.agents/cpp/agent.md` first; write code that compiles first time under clang-cl 18 `/W4 /permissive- /WX`
   (no unused variables, sign conversions or shadowing; 100-column limit; copy the owning file's idiom).
2. Work only in your leased pool on your branch (`feat/p9-<id>-20261003`, base `<frozen-sha>`). Lease:
   `powershell scripts\lease-worktree.ps1 -Branch feat/p9-<id>-20261003 -Base <frozen-sha> -Agent p9-<id>
   -RunId p9-<id>-20261003 -HeartbeatId p9-<id>-hb -MaxPool 20` (root may have leased it for you; check `-Status`).
3. Blind. Do not open any return, IC, Sharpe, turnover or NAV output of 2020-2023 (`build-equity/` NAV, cards,
   marginal, admission and diagnostics files are closed; manifests, receipts and field lists are open). Nothing dated
   2024-01-01 or later is opened by you or by code you run. The numbers in status 7 and the ledger are public.
4. Identity discipline: every change is behind a flag or provably value-preserving; flag absent = byte-identical; say
   exactly how root verifies it (targets, gtest filters, argv, the expected byte-identical files, any substitution
   list). Never edit an expected hash. A Python copy of a C++ rule is deleted only in a later slice, after root's
   identity run (plan §0.6).
5. PM8-12: numerical and research logic goes in atx-engine C++ (generic) or atx-impl C++ (strategy-specific) with
   gtests; Python is orchestration, specs, receipts and reports. No new versioned copy of any script; no new
   `research_fields_*.py` builder module (plan DEC-5).
6. Stay inside "Files in scope". Touching a file another lane owns is a lane failure unless the brief names it as a
   cross-lane edit; list every such edit in the report. CMake: append one block at the end of the owning list.
7. Implement first, then the tests named in the brief (they are the acceptance contract) plus what pins behaviour.
   Run pytest yourself on synthetic data: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   <files>`. C++ tests are written, not run; name the anchored gtest filters root will run.
8. Commit per task: `git add <your files>`, conventional message, trailer
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
9. Report: `.superpowers/sdd/platform-p9-20261003/task-<ID>-report.md`, committed with `git add -f`, in the
   `.agents/harness/TEMPLATES.md` "Lane report" shape: outcome, branch / SHA, files changed, evidence (each pytest
   command with exit code 0 and output tail), how root verifies (build targets, gtest filters, identity runs with
   argv), deviations, cross-lane edits, open risks, 0-3 ledger candidates.
10. Final reply to the PM: at most 15 lines (status DONE / DONE_WITH_CONCERNS / BLOCKED, commit SHAs per task, one test
    line, concerns). An adversarial reviewer reads your exact SHA before merge; fix rounds get a new review.

---

## Lane SQL3: dual-write of the Python record writers (stage 2)

**Pool / branch:** 17 (E2's warm tree, released after wave 2), `feat/p9-sql3-20261003`, run id `p9-sql3-20261003`,
heartbeat `p9-sql3-hb`. **Wave:** 3; base = the post-wave-2 P9-B0 head. **Effort:** M. **Serves:** infrastructure.
**Merge slot:** wave 3, after A4 (slot 6). Merge only when no wave is in flight (ruling E1-STALE).
**Needs merged:** SQL1, SQL2, E2.

**Goal.** Every Python writer of a stage-2 record class (bounded-run receipts and start receipts, stage receipts, NAV
cycle bindings, cycle verdicts and their copies, wave results, candidate registrations) ingests the file it has just
written into the catalog and checks that the rows re-render the file's bytes, when the catalog exists. JSON stays the
authority. Ruling SQL-6: a store choice selected by file presence must be written into the run receipt, so the bounded
runner gains a `store` block that records whether the catalog hook was on and which cache index files the command
could select; with no catalog and no index file the block is absent and every byte is today's.

**Read:** `sql-design.md` §3.9-§3.10, §4, §6 and `progress.md` rulings SQL-1..SQL-9; SQL2's report and its `ingest` CLI and
`research_store_identity.check_one`; E1's and E2's reports (the files below and where E2's split left the binding and
verdict writers); `wave2-carry.md` "All lanes"; `scripts/tests/test_wave_driver.py`, `test_wave_hardening.py`,
`test_cycle_resume.py` (to keep them green).

**Contracts.** Reads K-P9-13 (the `ingest` verb, class ids from `classes.json`, exit codes), K-P9-10 (receipt keys).
Writes the receipt key `store` (K-P9-13 addendum; `atx.bounded-research-run/v1` gains an optional key, absent by
default, so the schema id does not change).

**Files in scope (owned in wave 3; E1 owned them in wave 1, E2 in wave 2).**
- New: `scripts/research_store_hook.py`, `scripts/tests/test_research_store_hook.py`.
- Existing: `scripts/run_bounded_research.py`, `atx-engine/tools/stage_chain.py` (+ `test_stage_chain.py`),
  `scripts/wave_context.py` (wires the stage-chain hook), `scripts/cycle_resume.py`, `scripts/cycle_verdict.py`,
  `scripts/wave_stage_record.py`, `scripts/wave_queue.py`, and the tests that cover them (cases added, none
  weakened). If E2 moved the binding or verdict writer into `scripts/cycle/`, the PM names that file at dispatch and
  confirms D3 does not own it in wave 3.
**Forbidden:** `scripts/wave_manifest.py` and `scripts/cycle/**` beyond a file the PM names (D3 wires the walk-forward
kind there), `backtest_integrity.py` / `research_ledger.py` (the ledger is ingested by the catalog, never by a writer
hook), every C++ file, any argv, spec key or manifest key, and any JSON byte other than the receipt `store` block.

**Cross-lane edits:** none (the PM confirms at dispatch that no wave-3 lane owns the listed files).

**Tasks, in order.**
1. `research_store_hook.record(root, path, cls)`: a no-op unless `<root>/build-equity/research-store/catalog.sqlite`
   and `<root>/build-equity/bin/atx-research-store.exe` both exist (checked once per process; a test root never has
   them); then `atx-research-store ingest --catalog DB --class cls --path P` (60 s timeout, output captured), then
   `research_store_identity.check_one(DB, P, cls)`. Any failure prints one line `store: WARNING <cls> <path>: <why>`
   to stderr. It never raises, never changes an exit code, never writes beside the JSON, never retries a writer.
   `research_store_hook.selection(root, argv) -> dict | None`: `catalog` = `{"path": <relpath>, "exe_sha256": <sha>}`
   when the hook is on, else `null`; `indexes` = the sorted POSIX relpaths of every `index.sqlite` found at `<d>/` or
   `<d>/*/` for each argv token (or `--flag=value` value) that names an existing directory (the places
   `record_store.RecordStore` looks, brief-SQL1 task 4); returns None when both are empty.
2. **Receipt `store` block (ruling SQL-6).** `run_bounded_research.py` writes `"store": {"schema":
   "atx.run-store/v1", "catalog": ..., "indexes": [...]}` into `start.json` and `receipt.json`, after the existing keys,
   only when `selection` is not None; otherwise the key is absent and both files are byte-identical to today. The
   hook enables itself only through this recorded selection: what the receipt says is what ran.
3. `stage_chain.Chain(..., on_write=None)`: a generic callback called with the path after each receipt write (ok and
   `.failed-k`); default None = today. `wave_context` passes the hook for its chains.
4. One call after each write: the runner after `start.json` and after `receipt.json`; the binding writer; the verdict
   writer (main file and `verdicts/<mode>-k.json` copy); the record stage after `wave-result.json`; `wave_queue` after
   each candidate write and each emitted manifest. Classes as registered in `classes.json`.
5. **Only if the PM names the file at dispatch (PQ-3):** the spec keys that pass SQL4's `--candidate-cache-index` /
   `--pair-cache-index` into argv, and their `cycle_resume.REUSE_NEUTRAL` status as the PM rules. Otherwise root
   passes the flags by direct argv until P10.
6. Tests (below), report.

**Tests (written after implementing).** `test_research_store_hook.py`: no catalog -> `subprocess.run` never called
and every written file byte-identical to a run without the hook; with a fake exe (records its argv) -> exactly one
`ingest` per write with the right class and path; a failing exe -> one warning line, the writer's return value and
exit code unchanged; `check_one` reporting a mismatch -> one warning line; `selection` finds an `index.sqlite` at
`<d>/` and `<d>/*/`, ignores non-directory tokens, returns None when nothing is found. `test_run_bounded_research`
cases: no catalog and no index -> `start.json` / `receipt.json` byte-identical to today (golden from the pre-change
code); an index under an argv directory -> the `store` block present in both files with that path; the catalog on
-> `catalog` filled. `test_stage_chain.py`: `on_write` called once per ok receipt and once per failed receipt, never
for a skipped (done) stage. The E1 / E2 suites (`scripts/tests` under `PYTHONHASHSEED` 0 and 1, `atx-engine/tools`)
pass unchanged.

**Root verifies.** No build (Python only; SQL2's `atx-research-store` already built). pytest `scripts/tests` under two
seeds, `atx-engine/tools`, `atx-impl/tools`: 0 failed.

**Flag-absent identity (root procedure).** (1) The tiny-world fake wave of E1's identity procedure, no catalog: every
wave and run file byte-identical to the pre-merge head (E1's comparison: argv, specs, wave files; time keys aside as
E1 ruled). (2) The same wave with a catalog initialised in the tiny-world root and the built exe present: every file
byte-identical to (1) except, by substitution list, the `store` block in each `start.json` / `receipt.json` and the
digests that cover those two files' bytes (root lists them from the diff; no other key may differ); the catalog holds
one row per receipt / binding / verdict / wave result / candidate written, and `research_store_identity.py` over that
catalog reports 0 mismatches. (3) The first real P9 wave after merge runs with the real catalog present; the
integration log records the mismatch count (expected 0) and every receipt carries the `store` block. Real research
cells may use the record-store index from this merge on (ruling SQL-6: the choice is now in the receipt).

**SQLite dependency (ruling SQL-4):** none (Python stdlib `sqlite3` through SQL1's accessor; the C++ side is SQL2's).

**Out of scope:** C++ writers (NAV, IC, fitter, fields) dual-writing (P10); reader / bundle outputs and `nav_summ`
outputs (stage 1 in P9); retiring any JSON (P10); making the scoreboard read the store (P10).
