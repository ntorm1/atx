# P9 RESUME 7: addendum to the standing contract (PM, 2026-10-05)

Read `resume4-dispatch.md`, then `resume6-addendum.md`, then (lanes) `r6-wave3-common.md`, then this file. This file
wins where they differ; a ledger "Ruling" line wins over everything. Then read your section of `r7-dispatch.md`, your
handoff, and grep your lane ID and every ruling id your section names in the ledger `progress.md` (the block from the
line starting "- RESUME 7" to the end holds this session's rulings). All files are in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/` unless a pool is named.

## Owner rules (unchanged; restated)

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/`. No push. Never `git worktree prune` / `remove`,
  never stale-lease recovery, never touch a lease. Never touch `main` or `origin/main`.
- Open no data dated 2024-01-01 or later. Never edit an expected hash, golden or pinned value. Never run
  `atx-impl/tools/holdout_gate.py`.
- Trial ledger `C:/atx-wt/pool-2/build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`. No lane, reviewer
  or doc agent opens it for writing; root changes it only through a registered P9 cell the PM has started.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>` (`equity-dev` / `equity-rel`; the
  `dev` preset does not configure on this branch). Build only through `scripts/research-build.ps1`, target-scoped,
  single-use tag, ONE build on the host at a time (memory gate; the host has 15.7 GB RAM and about nine agents run
  at once: wait at the gate, up to 90 minutes per build, HOST-2).
- If the permission system refuses a command: do NOT retry it and do NOT route around it (PERM-1). Put the refused
  command in your final message.
- Pytest with explicit paths only; `scripts/tests` and `atx-engine/tools/test_stage_chain.py` in SEPARATE
  invocations; `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`.
- No TDD: implement first, then the tests your brief, carry, handoff and dispatch section name. No subagents.
- Other live sessions work in `C:/atx-wt/pool-25` and `C:/atx-wt/pool-26`: their trees and processes are not yours.
  Python processes you did not start are never killed.

## State every agent needs

- Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`: docs head `4dc831ac`, code head `327c86b2` (C3 part A
  merged). Root moves during this session: `git -C C:/atx-wt/pool-2 log --format=%h -1` gives the head to merge.
- Known-red set of root: EMPTY, with ONE expected red until root's SQL2-CLS commit for `atx.nav-spec/v1` lands
  (root M2j item 6): `atx-engine/tools/test_research_store_classes.py` (the classes guard). A lane that sees only
  that red on a tree merged from root says so and goes on; any other red is the lane's to explain. ENV-1 holds
  (a child exiting 3221225794 voids the run; re-run once on a quieter host).
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` is a named skip, not a red.

## Rulings of this session every lane needs (full text in the ledger)

- **W3-EARLY-2 holds.** Lanes run in parallel on approved heads; first step: `git merge feat/platform-p9-20261003`
  (never rebase); last step before the report: merge root's then-current head again and re-run your suites.
- **W3-8a.** The kind files (`scripts/wave_kinds.py`, `scripts/wave_stage_kinds.py`,
  `scripts/tests/test_wave_kinds.py`, `scripts/specs/p9/kinds/**`) are lane D3's (pool-20). Root does not edit them.
  Another lane's edit there is the smallest append and is listed by file and hunk.
- **D3-2.** D3 part A is two lanes: D3 (pool-20: guards, kind wiring, kind tests) and D3W (walk-forward C++, v3
  runner, fitter, ALCOMB-6 / -7, D2-9, D2R-3). The IC runner files and the fitter are D3W's.
- **B2 is not in root yet.** An item that sits in B2's files (`scripts/cycle_verdict.py`, `scripts/cycle_resume.py`,
  `scripts/wave_stage_record.py`, `scripts/cycle/spec.py`, `nav_summ`) is done LAST and only if
  `git -C C:/atx-wt/pool-2 log --format=%s -30` shows B2's redo merged; otherwise hand it off as pending.
- **GATE-1.** One gate for waves 2 and 3, on the head with every merge in. A lane that is not APPROVE and merged by
  then does not merge in P9.
- **Context.** If you run low on context: commit what is done (unfinished work as `WIP(<lane>): ...`), write the
  handoff (done / not done / next step, file:line), report DONE_WITH_CONCERNS. Do not start a build you cannot wait for.

## Final message to the PM (under 200 words; details go in files)

STATUS (DONE / DONE_WITH_CONCERNS / BLOCKED), head SHA, root SHA merged, build tags, tests with pass / fail counts,
items pending, files written (paths), disk freed, any permission refusal (the exact command), open questions one line
each. No logs, no 2020-2023 statistic.
