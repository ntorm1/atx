# P9 RESUME 8: addendum to the standing contract (PM, 2026-10-06)

Read `resume4-dispatch.md`, `resume6-addendum.md`, (lanes) `r6-wave3-common.md`, `resume7-addendum.md`, then this
file. This file wins where they differ; a ledger "Ruling" line wins over everything. Then read your section of
`r8-dispatch.md` (it points at your section of `r7-dispatch.md`, which is still your task list), your handoff, and
grep your lane ID and every ruling id your section names in the ledger `progress.md` (the blocks from the lines
starting "- RESUME 7" and "- RESUME 8" hold the newest rulings). All files are in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/` unless a pool is named.

## Owner rules (unchanged; see `resume7-addendum.md`; the ones agents trip on most)

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/`. No push. Never `git worktree prune` / `remove`,
  never stale-lease recovery, never touch a lease. Never touch `main` or `origin/main`.
- Open no data dated 2024-01-01 or later. Never edit an expected hash, golden or pinned value. Never run
  `atx-impl/tools/holdout_gate.py`. Trial ledger `C:/atx-wt/pool-2/build-equity/trials.jsonl`: 133 lines,
  sha256 prefix `27e40f9f`; only a registered P9 cell started by the PM changes it.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>` (`equity-dev` / `equity-rel`).
  Build only through `scripts/research-build.ps1`, target-scoped, single-use tag, ONE build on the host at a time
  (HOST-2: wait at the memory gate up to 90 minutes per build; root's builds have priority).
- PERM-1: a refused command or edit is never retried and never replaced by a same-effect substitute; put the exact
  command in your final message.
- Pytest with explicit paths, `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`; `scripts/tests`
  and `atx-engine/tools/test_stage_chain.py` in SEPARATE invocations. No TDD. No subagents. Blind: no 2020-2023
  statistic, root history as one-line subjects only.
- Interpreter: the first `python` on PATH in Git Bash is `C:/atx/atx-db/.venv` (another session's; no `psutil`):
  use `C:/Program Files/Python312/python.exe` explicitly.
- Commit trailer exactly `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Sprint-dir files are git-ignored:
  `git add -f` your report / review / handoff / evidence. Never `git add` a `.mypy_cache` or `__pycache__` path.

## State every agent needs (PM check, read-only, 2026-10-06)

- Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`: docs head `1b64ce45`, code head `ebc918c3` (C3 part A
  + `atx.nav-spec/v1` in `classes.json`). Known-red set of root: EMPTY. Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` is a named skip. No lane merged yet this resume; root
  moves as lanes become APPROVE: `git -C C:/atx-wt/pool-2 log --format=%h -1` gives the head to merge.
- Exes p9-2p (Debug) / p9-2q (Release) in pool-2 are current at `ebc918c3` (EXE-CUR). Next free root tag `p9-2r`.
- Host: 15.7 GB RAM, about 2.7 GB free at the start with ten agents about to run; C: 67 GB free. Other sessions
  work in `C:/atx-wt/pool-25`, `C:/atx-wt/pool-26` and `C:/atx/atx-db` (five python processes: not yours, never
  killed). ENV-1 holds (exit 3221225794 voids a run; re-run once on a quieter host). Disk rule of HOST-2 (below
  12 GB free: one configuration at a time; below 6 GB: stop and report).
- B2's redo is NOT in root. Items in B2's files (`scripts/cycle_verdict.py`, `scripts/cycle_resume.py`,
  `scripts/wave_stage_record.py`, `scripts/cycle/spec.py`, `nav_summ`) are done LAST and only if
  `git -C C:/atx-wt/pool-2 log --format=%s -30` shows B2's redo merged; else handed off as pending.
- T2 (pool-22, `dfa4218d`) is APPROVE and merges after B2 as a unit.

## Rulings of this session every agent needs (full text in the ledger, block "- RESUME 8")

- **DIRTY-1.** A lane whose tree was dirty at the stop (B2: `task-B2-report.md`; D3: `handoff-D3.md`; SQL3:
  `handoff-SQL3.md`; OD3P: `handoff-OD3P.md`) is resumed by an agent whose FIRST act is to commit that file as it
  stands (`git add -f`; D3 fills the `@@STATE@@` / `@@HEAD@@` placeholders with the facts first) as the lane's
  handoff commit. Never rewrite history; a `WIP(...)` commit stays.
- **C3-13a.** D3 does the C3-13 fix (a kind `rule` wave naming a NAV row whose `x-flag` is `--rule <id>` SETS the
  cell's `nav.rule`; other row kinds keep the append route; tests (a) and (b); E2's raw-rule-flag guard still
  accepts) in THIS resume round, before its review.
- **ABS-1.** The IC exe's `absent-members-v1` capability (OD3P-1) is its own lane ABS on a new pool from D3W's
  APPROVE head; D3W leaves `IcComposition::add` easy to extend by a per-member skip and says where in its report.
- **D3W-EVAL.** ALCOMB-6's eval-verb half (vol / Sharpe of the momentum sleeve's series printed by
  `atx-research-eval`) is D3W's as a listed cross-lane append (a new verb or flag; flag absent = bytes unchanged);
  `eval_report.cpp` is not in B2's current file list. PRE-38 stands (the NAV exe's `ann_vol` produces the realised
  vol ratio; the sleeve line keeps the eval verb).
- **D3W-PB.** Part B (the C++ `ew-theme-aim-v2` fit, resid diagnostics, the D2-3 deletion) is a stretch lane in
  pool-23 after D3W's merge; it merges only if APPROVE at the gate, else P10 whole.
- **PRE-2B-SPLIT.** Round 2b runs as 2b-i (now: the rulings into the text) and 2b-ii (after the gate: the values).
- **A4-2.** A4 starts in the second dispatch wave (when the first of B2, SQL3, OD3P, C3b hands back).
- **DATA-1.** `C:\atx\data` is classified read-only first; only entries shown unused AND regenerable or superseded
  are deleted later, by literal path, by a separate agent; vendor / source / history-block data and anything an
  atx-db job may read go to the owner by name.
- **REV-BASE.** A first review's BASE is `git merge-base <HEAD> feat/platform-p9-20261003`; a re-review's BASE is
  the SHA the previous review names as reviewed.
- **SQL3-SEED.** SQL3's "SEE-SEED-1" placeholder is replaced by the real seed-1 counts before its review.
- **CANARY (C3-14).** A tiny-world canary whose golden digests match and that fails only the 15 s first-run wall
  limit on a loaded host is re-run once on a quieter host; the limit is never edited; two misses with matching
  digests are a timing finding for the PM, not a red.

## Context and hand-back

If you run low on context: commit what is done (`WIP(<lane>): ...` for unfinished work), write the handoff (done /
not done / next step, file:line), report DONE_WITH_CONCERNS. Do not start a build you cannot wait for.

Final message to the PM (under 200 words; details go in files): STATUS (DONE / DONE_WITH_CONCERNS / BLOCKED), head
SHA, root SHA merged, build tags, tests with pass / fail counts, items pending, files written (paths), disk freed,
any permission refusal (the exact command), open questions one line each. No logs, no 2020-2023 statistic.
