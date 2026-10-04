# P9 RESUME 4: standing contract for every agent (PM, 2026-10-04)

Read this first, then your handoff / brief, then grep your lane ID in the ledger
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/progress.md` (every "Ruling" line is binding; the block
"RESUME 4" at the end holds the newest rulings and answers the open questions in your handoff). Older contracts still
apply: `resume-rulings.md`, `wave2-lane-dispatch.md` (lanes), `wave2-review-dispatch.md` (reviewers),
`root-wave2-merge-brief.md` (root), all in the same dir.

## Hard rules (owner)

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/` (another session runs jobs there; python processes
  you did not start are theirs: never kill them). No push. Never `git worktree prune`. Never stale-lease recovery.
- Open no data dated 2024-01-01 or later. Never edit an expected hash / golden / pinned value to make a test pass.
- Trial ledger `C:/atx-wt/pool-2/build-equity/trials.jsonl` is 133 lines, sha256 prefix `27e40f9f`. Nobody appends to
  it in this step. Lanes and reviewers are blind: no 2020-2023 return / IC / Sharpe / turnover / NAV statistic is read.
- Builds only through `scripts/research-build.ps1` (PowerShell 5.1: `powershell scripts\research-build.ps1 ...`),
  target-scoped, single-use tag, one build at a time on the host (memory gate; 1.5-3 GB free RAM). If the gate refuses:
  wait and retry (bounded, ~30 min total), never bypass, never raw cmake / ninja. Root's builds in pool-2 have
  priority.
- Pytest with explicit paths only; `scripts/tests` and `atx-engine/tools/test_stage_chain.py` in SEPARATE invocations;
  `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`. Kill your own child processes when done.
- No TDD: implement first, then the tests the brief / review names. No subagents.
- Work only in your own pool worktree (root: `C:/atx-wt/pool-2`). Resume from the handoff; never restart a lane, never
  rewrite history, never re-do work the ledger marks complete.
- Commit trailer, exactly: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Sprint-dir files are git-ignored
  in lanes: `git add -f` your report / review / handoff. Never `git add` a `.mypy_cache` or `__pycache__` path.
- Disk: once your report is committed, delete by name your own regenerable build object tree (`build-equity*` in your
  pool; keep receipts and logs) and your own scratch files. Never wildcard-delete in the shared scratchpad (incident
  SCRATCH-1). Never delete pinned / reference / golden artifacts, caches named by a pin, or another agent's files.
  Evidence logs live in your pool's sprint dir, not the shared scratchpad.
- `rtk` rewrites git / grep output: use `rtk proxy <cmd>` when you need raw output.
- Stop (write a handoff, report BLOCKED) only for: a design error, an identity miss, a red outside the known-red set
  you cannot attribute to your own change, anything irreversible / destructive, or anything outside your worktree.
  A slip (typo, missing include, trivial conflict) is fixed in place and logged.

## Final message to the PM (keep it under 200 words; details go in files)

`STATUS` (DONE / DONE_WITH_CONCERNS / BLOCKED), head SHA, build tags used, tests run with pass / fail counts, files
written (paths), disk freed, open questions (one line each). Do not paste logs.
