# P9 RESUME 10: addendum to the standing contract (PM, 2026-10-08)

Read `resume4-dispatch.md`, `resume6-addendum.md`, (lanes) `r6-wave3-common.md`, `resume7-addendum.md`,
`resume8-addendum.md`, `resume9-addendum.md` (its TEST-1 section is carried verbatim and stands), then this file.
This file wins where they differ; a ledger "Ruling" line wins over everything. Then read your section of
`r10-dispatch.md`, your lane's handoff / review file in its pool, and grep your lane ID and every ruling id your
section names in the ledger `progress.md` (the blocks from the lines starting "- RESUME 9" and "- RESUME 10" hold
the newest rulings). All files are in `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/` unless a pool is
named.

## Owner rules (unchanged; the ones agents trip on most)

- Your shell starts in `C:/atx`. NEVER run git, a build, a test or an edit there: `cd` to your pool (or use
  `git -C <pool>`) before anything. Never build, switch or commit in `C:/atx`. Never touch `atx-db/`. No push. Never
  `git worktree prune` / `remove`, never stale-lease recovery, never touch a lease (every lease prints `owner=dead`:
  that is expected, ruling LEASE-DEAD; leave it). Never touch `main` or `origin/main`. Never any git write in pools 8,
  24 (frozen) or 2 (root agents excepted).
- Open no data dated 2024-01-01 or later. Never edit an expected hash, golden or pinned value. Never run
  `atx-impl/tools/holdout_gate.py`. Trial ledger `C:/atx-wt/pool-2/build-equity/trials.jsonl`: 133 lines, sha256
  prefix `27e40f9f`; only a registered P9 cell started by the PM changes it.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>` (`equity-dev` / `equity-rel`).
  Build only through `scripts/research-build.ps1`, target-scoped, single-use tag, ONE build on the host at a time
  (HOST-2 / HOST-3 / HOST-4 below).
- PERM-1: a refused command or edit is never retried and never replaced by a same-effect substitute; put the exact
  command in your final message. Never `git checkout -- <file>`, `git restore`, `git stash` or any other discard of
  working-tree changes (TEETH-RESTORE: a teeth tool keeps and restores its own bytes and asserts the SHA).
- Pytest with explicit paths, `-p no:cacheprovider`, own `--basetemp` (create its PARENT dir first),
  `PYTHONDONTWRITEBYTECODE=1`, interpreter `C:/Program Files/Python312/python.exe` explicitly; `scripts/tests` and
  `atx-engine/tools/test_stage_chain.py` in SEPARATE invocations (at the gate only: TEST-1).
- TEST-1 (owner): targeted checks only until the gate: single-TU `check` compiles while shaping; at most one
  target-scoped Debug build of your own test target (plus the exe its tests need) per round; your named gtest filter
  only; your own pytest files only; Python teeth by file; at most one C++ teeth build. Whole suites, Release, canaries,
  golden, stage chain: the gate's, once. A record cell for a deferred run reads "deferred to the gate (TEST-1)".
- No test-driven development (owner): implement first, then the tests the briefs name. No subagents. Blind: no
  2020-2023 statistic, root history as one-line subjects only. Owner priority: "high quality end to end
  implementations, modular / reusable / performant code centralizing in atx-engine and atx-impl C++ code base";
  house style `.agents/cpp/agent.md` (read before any C++); `/W4 /WX`; 100 columns; no new Python mirror of a C++
  rule.
- Commit trailer exactly `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Sprint-dir files are git-ignored:
  `git add -f` your report / review / handoff / evidence. Never `git add` a `.mypy_cache`, `__pycache__` or object
  tree. Records are written in the pool's sprint dir (their final place), never as a draft under the session
  scratchpad (PERM-SCRATCH). The Bash heredoc fails on long texts with apostrophes (hook layer): write long record
  files with the editor tool.
- `rtk` rewrites git / grep output: use `rtk proxy <cmd>` when you need raw output.

## State every agent needs (PM check, read-only, 2026-10-08)

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`: docs head `baefd8a9`, code head `fcb0200e` (B2 + T2
merged; C3b / D3W / ABS NOT yet merged: root M3 does that during this wave; `git -C C:/atx-wt/pool-2 log
--format=%h -1` gives the head to merge). Known-red set: exactly
`atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced` (root-only; B2-N37) until B2 fix round 2
lands. Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` is a named skip. Next free root build tag
`p9-2u`. Host: free RAM 1.7 of 15.7 GB, one foreign python process, no build running at dispatch; C: 58 GB free.
ENV-1 holds (exit 3221225794 voids a run; re-run once on a quieter host).

| lane | pool / branch | head at dispatch | state | R10 step |
|---|---|---|---|---|
| C3b, D3W, ABS | 15 / 23 / 28 | `76b9f256` / `f0726acb` / `6a3f5043` | APPROVE | root M3 merges them now |
| D3 | 20 `feat/p9-d2-20261003` | `e197b3b6` | APPROVE | root M4 |
| AL-CLOCK | 30 `feat/p9-alclock2-20261007` | `a662d1c2` (APPROVE `e9b3a323`, code `1cbe5233`) | fix round 1 designed, not started | finish fix 1; re-review; root M4 (fix head if APPROVE by its slot, else `e9b3a323`) |
| COV-MV | 31 `feat/p9-covmv2-20261007` | `12f5c094` (WIP `cc63c38f`; APPROVE `e9e794b3`, code `846739c1`) | fix round 1 coded, untested | finish fix 1; re-review; root M4 after AL-CLOCK |
| OD3P | 21 `feat/p9-od3p-20261004` | `e7d7c23a` (WIP `a1348616`; APPROVE `dc11c977`) | follow-up coded, untested | finish; re-review 2; root merges `dc11c977` in M4 and the follow-up in M5 |
| B2 fix 2 | 14 `feat/p9-b2-20261003` | `73b4acfd` (= root; APPROVE `46ffe3dc` merged) | nothing done | B2-N37; re-review 3; root M5 |
| SQL3 item 5 | 13 `feat/p9-sql3-20261004` | `489d9a7e` (WIP) | call lines coded, untested | tests, report; review 2; root M5 |
| D3W-PB (stretch) | 23, above D3W | `a0c7427c` (review PARTIAL; lane `460f5ba2`, pre-deletion `c044f5f6`) | 1 Required (R-1) | fix 1 (one commit), then the review resumed |
| A4 (stretch) | 27 `feat/p9-a4-20261005` | `9e80a7d5` (review PARTIAL; lane `7c9140c4`, pre-flip `6dab6948`) | no verdict yet | review resumed by a fresh reviewer |
| SIGN (stretch) | 29 `feat/p9-sign-20261006` | `422e9fad` (three WIP commits; no handoff) | verb + gtests + stage call coded, never built | SIGN-RESUME |
| SQL4 | - | - | not dispatched | PM decides at the gate (SQL-12) |
| PRE | sprint dir | round 2b-i `685e238b` | 2b-ii after the gate | - |

Frozen pools 8 and 24: read-only (TEETH-POOL). Every lease `owner=dead`: untouched (LEASE-DEAD).

## Rulings of RESUME 10 (full text in the ledger; binding)

- **LEASE-DEAD.** Every pool lease shows `owner=dead`; no recovery, no lease write; lanes continue in their pools.
- **HOST-4.** One C++ build on the host at a time; Python-only work first; build priority this wave: root > B2 fix 2 >
  AL-CLOCK fix 1 > COV-MV fix 1 > D3W-PB (fix, then its reviewer's teeth build) > SIGN > A4 review. A lower-priority
  agent that sees a build running (`ninja` / `cmake --build` / `cl` / `clang-cl` in the process list) waits (poll with
  60 s quiet, as `m2-logs/build-wait.log` shows) and does not start a second one. Wait bound 180 minutes per build;
  then commit what is done and hand back BLOCKED-HOST with the free-RAM figure.
- **REV-FRESH.** Every re-review / resumed review this wave is a fresh reviewer: read the previous review file first,
  BASE = the SHA it names as reviewed (REV-BASE), review BASE..HEAD only, re-run the lane's named filter and pytest
  files (TEST-1), write `task-<LANE>-rereview-<n>.md` (a resumed PARTIAL review writes its verdict into the same file).
- **B2-FIX2-EXE.** B2 fix 2 uses `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity` (read-only) and `ATX_EQUITY_BIN` =
  its OWN Debug bin (one build of `atx-research-eval`, tag `p9-b2-r`); never pool-2's bin; blind diff only.
- **SIGN-RESUME.** First act: `handoff-SIGN.md` reconstructed from the three WIP commit bodies, committed; then merge
  root, check, one build, tests, identity, teeth, report, deletion LAST.
- **D3WPB-SEQ.** The D3W-PB fix commit lands before the review is resumed (shared working tree).
- **ROOT-M3-SCOPE.** Root M3 = step 0 + C3b / D3W / ABS; M4 and M5 follow on hand-back.
- **FU-REVIEWS.** The file names and BASEs of every scoped review this wave (ledger).

## Context and hand-back

If you run low on context: commit what is done (`WIP(<lane>): ...` for unfinished work), write the handoff (done /
not done / next step, file:line), report DONE_WITH_CONCERNS. Do not start a build you cannot wait for.

Final message to the PM (under 200 words; details go in files): STATUS (DONE / DONE_WITH_CONCERNS / BLOCKED /
BLOCKED-HOST), head SHA, root SHA merged, build tags, tests with pass / fail counts, items pending, files written
(paths), disk freed, any permission refusal (the exact command), open questions one line each. No logs, no 2020-2023
statistic.
