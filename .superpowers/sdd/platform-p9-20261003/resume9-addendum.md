# P9 RESUME 9: addendum to the standing contract (PM, 2026-10-07)

Read `resume4-dispatch.md`, `resume6-addendum.md`, (lanes) `r6-wave3-common.md`, `resume7-addendum.md`,
`resume8-addendum.md`, then this file. This file wins where they differ; a ledger "Ruling" line wins over everything.
Then read your section of `r9-dispatch.md` (it points at your sections of `r7-dispatch.md` / `r8-dispatch.md`, which
are still your task list), your handoff, and grep your lane ID and every ruling id your section names in the ledger
`progress.md` (the blocks from the lines starting "- RESUME 7", "- RESUME 8" and "- RESUME 9" hold the newest
rulings). All files are in `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/` unless a pool is named.

This file also stands in for the `status-9.md` that was never written at STOP 8 (ruling STATUS-9): the state table
below is the PM's read-only check of 2026-10-07, from git and the lanes' own files.

## TEST-1 (owner, 2026-10-07, after the R9-1 dispatch; wins over every "suites" step below and in r7/r8/r9-dispatch)

Owner's words: "do not waste time on large test runs. Do small targeted tests/compiles and wait till end of wave to
more complete." Until the gate: single-TU `check` compiles while shaping; at most one target-scoped Debug build of
your own test target per round; your named gtest filter only (never the whole target-tests); your own pytest files
only (never the whole `scripts/tests`; never the stage chain unless you edited it); Python teeth by file; at most one
C++ teeth build. Deferred to the gate (root, once): whole `scripts/tests` seeds 0 / 1, the stage chain, whole gtest
targets, every Release build, canaries, golden 1 / 4 workers, parents' flag-absent identity. A record cell for a
deferred run reads "deferred to the gate (TEST-1)". Reviewers: the lane's named filter and pytest files only.
ROOT-1a: root does merge-affected targeted checks at each merge; the whole suites once at the gate; a red there is
attributed by merge commit and returned to its lane.

## Owner rules (unchanged; the ones agents trip on most)

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/`. No push. Never `git worktree prune` / `remove`,
  never stale-lease recovery, never touch a lease. Never touch `main` or `origin/main`.
- Open no data dated 2024-01-01 or later. Never edit an expected hash, golden or pinned value. Never run
  `atx-impl/tools/holdout_gate.py`. Trial ledger `C:/atx-wt/pool-2/build-equity/trials.jsonl`: 133 lines,
  sha256 prefix `27e40f9f`; only a registered P9 cell started by the PM changes it.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>` (`equity-dev` / `equity-rel`).
  Build only through `scripts/research-build.ps1`, target-scoped, single-use tag, ONE build on the host at a time
  (HOST-2: wait at the memory gate up to 90 minutes per build; root's builds have priority; HOST-3 below).
- PERM-1: a refused command or edit is never retried and never replaced by a same-effect substitute; put the exact
  command in your final message. In particular: never `git checkout -- <file>`, `git restore`, `git stash` or any
  other discard of working-tree changes (two lanes were frozen by that refusal; TEETH-RESTORE below says how teeth
  are reverted).
- Pytest with explicit paths, `-p no:cacheprovider`, own `--basetemp` (create its PARENT dir first),
  `PYTHONDONTWRITEBYTECODE=1`; `scripts/tests` and `atx-engine/tools/test_stage_chain.py` in SEPARATE invocations.
- No test-driven development (owner): implement first, then the tests the briefs name. No subagents. Blind: no
  2020-2023 statistic, root history as one-line subjects only.
- Owner priority (2026-10-07, verbatim in the ledger): "high quality end to end implementations, modular / reusable /
  performant code centralizing in atx-engine and atx-impl C++ code base". House style: `.agents/cpp/agent.md`
  (read it before any C++); `/W4 /WX`; 100 columns; no new Python mirror of a C++ rule.
- Interpreter: `C:/Program Files/Python312/python.exe` explicitly (the first `python` on PATH in Git Bash is another
  session's venv).
- Commit trailer exactly `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Sprint-dir files are git-ignored:
  `git add -f` your report / review / handoff / evidence. Never `git add` a `.mypy_cache`, `__pycache__` or object tree.
- Records are written by editing the lane's existing report / handoff in the pool's sprint dir or by creating the
  file there (its final place); never a draft under the session scratchpad (PERM-SCRATCH: that Write was refused).

## State every agent needs (PM check, read-only, 2026-10-07)

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`: docs head `685e238b` (prereg round 2b-i), code head
`fd084e78` (SQL3 merged + `atx.run-store/v1` class row). Known-red set EMPTY. Next free root build tag `p9-2t`.
Root exes of record: `handoff-root-m1.md` (Debug p9-2r / Release p9-2s store exes; the nine p9-2p / p9-2q exes).
Trial ledger 133 lines `27e40f9f`. Host: free RAM 1.9 of 15.7 GB (seven python processes of another session),
C: 49 GB free.

| lane | pool / branch | head | state at STOP 8 (from git and the lane's files; nothing reviewed) | R9 step |
|---|---|---|---|---|
| B2 | pool-14 `feat/p9-b2-20261003` | `e86d8b7c` clean | every r7 item done; suites green on `7832719f`; on `1b15330d` (root `fd084e78` merged) all but `scripts/tests` seed 1 (stopped at 58 %) and the stage chain; object trees deleted; next tag `p9-b2-q` | finish the two suites, then re-review 2 (BASE `26fe8d50`), then root merges FIRST |
| D3 | pool-20 `feat/p9-d2-20261003` | `aca78a14` (merge of root `fd084e78`), dirty `task-D3-report.md` | C3-13 fix `0c9f75d9` committed with tests; report mostly filled; the final suites after the merge not recorded | DIRTY-1, suites, hand back; first review (CRITICAL PATH) |
| D3W | pool-23 `feat/p9-d3w-20261005` | `1e6c4336`, dirty `handoff-D3W.md` (`@@ROOT3@@`) | items 1-8 DONE; report `task-D3W-report.md`; Release `p9-d3w-h` from `c7549d6a`; object trees deleted | first review now (reviewer does DIRTY-1a first) |
| C3b | pool-15 `feat/p9-c3-20261004` | `3129ca14` (merge of root `fd084e78`), dirty `c3-logs/c3b-gt-exits.txt`, `c3b-pyt-exits.txt` | R8: S-13 `f41719c2`, two root merges; which of the handoff's steps 3-6 ran is in the logs / receipts only; handoff and report not updated since 2026-10-05 | DIRTY-1, reconstruct, finish steps 3-8, hand back; re-review |
| AL-CLOCK | FROZEN pool-8; continues in pool-30 `feat/p9-alclock2-20261007` at `1cbe5233` | pool-8: six C++ files hold teeth set 1 (revert refused), dirty handoff | code, 13 gtests, pytest, registration, template committed; teeth set 1 caught 7 / 7; NOT done: set 2, Debug build of the merged head, Release, suites, report fill | TEETH-POOL: fresh pool, copy the uncommitted records, finish, hand back; first review |
| COV-MV | FROZEN pool-24; continues in pool-31 `feat/p9-covmv2-20261007` at `b6e304ee` | pool-24: two `// TEETH` lines uncommitted (revert refused) | items 1-8 DONE in Debug (405 / 405 whole; teeth 3 / 3); NOT done: canary, Release, Debug re-run on the merged head | TEETH-POOL: fresh pool, finish, hand back; first review; merges after AL-CLOCK |
| OD3P | pool-21 `feat/p9-od3p-20261004` | `97a8157f` (merge of root `fd084e78`), dirty handoff / contract / report | fix round 1 DONE (R-1, S-1..S-5; `365b8c8b`); teeth 8 / 8 | re-review now (BASE `d78d23ce`; reviewer does DIRTY-1a first) |
| A4 (stretch) | pool-27 `feat/p9-a4-20261005` | `1e4904b7` + 25 UNCOMMITTED paths (11 modified, 14 new; +367 lines) | T2 `dfa4218d` merged (`ebfb333e`); A1-SHIM commit; then a large uncommitted slice (holdings FTD / RegSHO readers, session clock, run payload, three new test files); no hand-back, handoff stale (2026-10-05) | DIRTY-2 commit first, reconstruct, continue; merges only if APPROVE at the gate (A4-1) |
| SQL3 | pool-13 | `69b8c162` clean | MERGED in root (`fbd2c045` + `fd084e78`); item 5 waits on B2 | follow-up after B2 merges |
| T2 | pool-22 | `dfa4218d` clean | APPROVE | root merges after B2 |
| ABS | pool-28 `feat/p9-abs-20261006` | `1b64ce45` | not started | when D3W is APPROVE (ABS-1); input `od3p-absent-contract.md` |
| SIGN | pool-29 `feat/p9-sign-20261006` | `1b64ce45` | not started | when C3b is APPROVE (C3-11) |
| D3W-PB, SQL4 | - | - | not started | after D3W's merge / after SQL3, S2, B2, C3, D3 in root |
| PRE | sprint dir | round 2b-i committed (`685e238b`) | 2b-ii after the gate | - |

Leases: pools 8, 13, 15, 21, 22, 23, 24, 27, 28, 29, 30, 31 alive (this sprint); 12, 14, 17, 18, 19, 20 `owner=dead`
from older runs (lanes work in 14 and 20 regardless; no recovery); 25, 26 other sessions. None touched.

## Rulings of RESUME 9 (full text in the ledger; binding)

- **STATUS-9.** No `status-9.md`; this file and the RESUME 9 ledger block replace it.
- **DIRTY-1a.** When a lane's next step is a review (D3W, OD3P), the REVIEWER's first act is the DIRTY-1 commit of the
  stop's record files exactly as they stand (`git add -f`; message `docs(<lane>): records as they stood at STOP 8
  (DIRTY-1)`), before reading them as evidence; a reviewer never edits a lane's record.
- **DIRTY-2.** A lane with uncommitted CODE at the stop (A4) is resumed by a fresh agent whose first act is to commit
  every path `git status --short` lists under the lane's scope, as it stands, `WIP(<lane>): work uncommitted at STOP 8
  (DIRTY-2; untested as committed)`; never a rewrite, never a selective drop; then the handoff is reconstructed from
  the diff.
- **TEETH-POOL.** Pools 8 and 24 are FROZEN: no build, no commit, no edit, no git write of any kind in them; nothing
  under their `build-equity*/bin` is evidence (teeth exes). Lanes AL-CLOCK and COV-MV continue in pool-30 / pool-31 on
  the new branches above, leased by the PM at the lanes' committed heads. The fresh agent's first act copies (plain
  `cp`, read-only on the frozen pool) every lane record file that is uncommitted or newer in the frozen pool's sprint
  dir into its own pool's sprint dir and commits it (`docs(<lane>): records copied from frozen pool-N at STOP 8
  (DIRTY-1 / TEETH-POOL)`). The frozen pools wait for the owner (the exact `git checkout --` lines are in the ledger
  for the owner); root merges the NEW branches.
- **TEETH-RESTORE.** A teeth run keeps the pre-mutation bytes itself (a copy or the exact reverse replacement inside
  the teeth tool, as D3's `teeth.py` and C3b's `c3b-teeth-py.py` do), restores them in the same run, then asserts
  `git status --porcelain` empty and the file's SHA-256 equals HEAD's blob. Never `git checkout` / `restore` / `stash`.
  If a restore fails, STOP, commit nothing, report the exact state; never build on a mutated tree.
- **HOST-3.** Free RAM is ~1.9 GB: do Python-only work first; one C++ build on the host at a time (HOST-2); build
  priority root > B2 > C3b > AL-CLOCK > COV-MV > D3W / ABS > A4 / SIGN (a lower-priority agent that sees another
  build running waits; it does not start a second one). A build that cannot admit within 90 minutes: commit what is
  done, report BLOCKED-HOST with the free-RAM figure; never touch another session's processes.
- **D3W-STALE.** `scripts/specs/p9/rules/mom-volman-v1.json:50` (stale after ALCOMB-7) is NOT edited (PRE-40 form);
  prereg round 2b-ii states it; the reviewer notes it, not a finding.
- **SPDLOG-RELINK.** The shared `C:\atx-cache\deps\spdlog-build` can relink another pool's exes with no source change.
  EXE-CUR reads: the exe SHA-256 named in the receipt of the build the evidence was taken from; an exe relinked after
  that receipt is re-receipted (its SHA named) before it is evidence again. Cache layout unchanged in P9 (P10 item).
- **B2-EVAL-KEEP.** `eval_report.cpp` carries B2's PRE-15 hunks and D3W's eval-verb hunk (D3W-EVAL): keep-both at
  root's merge of the second of them; root runs the eval gtests and the PRE-15 gtest after both are in.
- **B2-SEQ.** B2's re-review 2 starts after the lane's two suites are recorded (not in parallel): the reviewer's own
  mutation on a shared working tree would corrupt the lane's run.
- **A4-3.** A4 is dispatched in this first wave (its uncommitted work must enter history; the owner's priority is C++
  centralization); its builds yield under HOST-3; A4-1 stands (merges only if APPROVE at the gate).
- **DATA-3.** The `C:\atx\data` deletion (DATA-2) was refused by the permission classifier in RESUME 8; under PERM-1
  and the goal's own line it is not retried in any form. Deliverable to the owner: `data-classification.md`.
- **C3-14 stands.** The canary's 15 s first-run limit is not edited (C3b's open question).
- **PERM-SCRATCH.** See "Owner rules" above.

## Context and hand-back

If you run low on context: commit what is done (`WIP(<lane>): ...` for unfinished work), write the handoff (done /
not done / next step, file:line), report DONE_WITH_CONCERNS. Do not start a build you cannot wait for.

Final message to the PM (under 200 words; details go in files): STATUS (DONE / DONE_WITH_CONCERNS / BLOCKED /
BLOCKED-HOST), head SHA, root SHA merged, build tags, tests with pass / fail counts, items pending, files written
(paths), disk freed, any permission refusal (the exact command), open questions one line each. No logs, no 2020-2023
statistic.
