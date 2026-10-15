# Goal prompt for the next parent (PM) agent (refreshed at STOP 10, 2026-10-08)

Paste everything below the line as the `/goal` text. The paragraph "Decisions
already made for me" carries the delegation the owner gave ("do not stop to ask
questions, you are allowed to rule on all open questions"), the owner's
2026-10-07 words (parallel Opus implementers; quality / modular / reusable /
performant C++ centralized in atx-engine and atx-impl; no TDD; TEST-1: targeted
tests only until the gate) and the structural rulings of RESUME 9 and 10.
Strike or change any you disagree with before pasting.

---

Complete the P9 sprint plan
`C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project
manager.

Start by reading, in order, in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:

1. `status-11.md` (state, lane table with every head, gates, next steps, what is
   open with me, the open questions; the ledger block from "- STOP 10" to the end
   records every hand-back of the stop)
2. `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`,
   `resume10-addendum.md` (the standing contracts you give every agent; the last
   wins where they differ; TEST-1 is in resume9), `r10-dispatch.md` (per-agent
   text: lanes, root M3 / M4, the scoped re-reviews, SQL4; it points at
   `r7-dispatch.md` / `r8-dispatch.md` / `r9-dispatch.md`), `r7-review-dispatch.md`
   + `r8-review-notes.md` (reviewers), `r7-root-merge.md` + `r10-dispatch.md`
   "ROOT M4" + the ledger's "Dispatch root M5" line (root), then `progress.md`
   from "- RESUME 10" to the end (every "Ruling" line is binding, in that block
   and in the earlier ones)
3. `handoff-root-m4.md` (root after M4: code head `5c811b56`) and
   `handoff-root-m5.md` (root M5 stopped before any merge: ALL of M5 is the first
   thing root does: B2 fix 2 `101f695a`, OD3P follow-up `4a7372e2`, D3W-PB chain
   to `44a331e6`, SIGN chain to `e2844926` + SQL2-CLS row, A4 chain from
   `6dab6948` with the TRAIN identity on both fields to `fc3e414e`)
4. `p9-prereg.md` (round 2b-i committed; 2b-ii after the gate: the ledger's three
   "PRE 2b-ii list" lines), `p9-prereg-flags.md`, `wave3-carry.md` ("PM answers"
   and "Added after drafting" win), `root-gate-brief.md` (the single gate, GATE-1)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Lanes live in
pooled worktrees with their own handoff files; resume them from those files with
fresh agents, never restart. Do not re-dispatch anything the ledger marks
complete or APPROVE. First verify the stop state yourself (heads, clean trees,
trial ledger, lease status: every lease prints `owner=dead`, that is expected
and untouched, LEASE-DEAD), read-only; a lane whose tree is dirty at the stop is
resumed by an agent that first commits that file as the lane's handoff (DIRTY-1
/ DIRTY-2). Pools 8 and 24 are FROZEN (TEETH-POOL): read-only until I restore
them. Pool-23's `build-equity/` holds mutated teeth binaries whose deletion was
refused (TEETH-BIN-23): nobody deletes it, nobody uses its exes.

How to run it:

- You are PM only: write the ledger, briefs and rulings. Never build, run real
  data or edit code yourself. Use Opus subagents (model: opus) for every lane,
  reviewer and root-integrator step. Run as many lanes in parallel as are
  independent (ten at once worked; builds serialize at the memory gate, HOST-4:
  priority order and a 180-minute wait bound; HOST-5: a build whose command line
  names a path outside `C:\atx-wt\` is another session's and is ignored by our
  wait loops); one root agent at a time in pool-2. Hand artifacts over as files
  to protect your context. Reviewers of earlier sessions are gone: every
  re-review is a fresh reviewer that reads the previous review first (REV-FRESH);
  a PARTIAL review is resumed from its "Exact next step" into the same file.
- No test-driven development: implement first, then the tests the briefs name.
- TEST-1 (my words, 2026-10-07): do not waste time on large test runs; small
  targeted tests and compiles per lane and per merge; the whole suites, Release
  builds, canaries, golden and stage chain run ONCE at the end of the wave (the
  gate). A gate red is attributed by merge commit and returned to its lane.
- Priority: high quality end-to-end implementations; modular, reusable,
  performant code centralized in the atx-engine and atx-impl C++ code base; keep
  moving from JSON artifacts and repeated Python scripts to SQLite and core C++.
- Decide, don't stall, and do not stop to ask me questions: you may rule on
  every open question. Record every decision in the ledger as "Ruling: decision
  -- why -- cost if wrong" before the measurement it could bias. Stop only for
  irreversible or destructive actions that I have not asked for,
  security-sensitive actions, pushes or merges outside the worktrees, or a plan
  so broken every path is a guess.
- Every lane gets a task review; fix rounds until APPROVE; root re-proves each
  lane from its APPROVE SHA at merge (MERGE-SHA; ROOT-1a targeted checks). Treat
  a subagent's report as its own account, not as proof.

Hard rules:

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/` (another
  session runs jobs there). No push. Never `git worktree prune`. Never touch
  `main` or `origin/main`. Never any lease action (no release, no stale recovery).
- Open no data dated 2024-01-01 or later. Never edit an expected hash. Never run
  `atx-impl/tools/holdout_gate.py`.
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it
  changes only through registered P9 cells.
- Configure only as
  `powershell scripts\atx-build.ps1 configure -Preset <preset>` (lanes use
  `equity-dev` / `equity-rel`; the `dev` preset does not configure on this
  branch); build only through `scripts/research-build.ps1`, target-scoped, one
  at a time among our pools. If the permission system refuses a command, do not
  retry it and do not route around it: report it to me. Never `git checkout -- <file>`,
  `git restore` or `git stash` on a working file (TEETH-RESTORE: teeth tools
  restore their own bytes; TEETH-CRLF: compare `git hash-object` to the HEAD blob,
  never raw bytes to `cat-file`).
- Pytest with explicit paths only; `scripts/tests` and
  `atx-engine/tools/test_stage_chain.py` in separate invocations (at the gate).
- Disk: agents delete their own regenerable build trees and scratch by name once
  their report is committed; never wildcard-delete shared scratch; never delete
  pinned, reference or golden artifacts (the two D-COV stores under
  `build-equity/p9-m2i-dcov-*` are pinned).
- Subagent commit trailer:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Ledger appends: write the lines to a scratch file and `cat >>` (a Bash heredoc
  with apostrophes fails in the hook layer); never edit earlier lines.

Decisions already made for me (ledger OD-DELEG, OD5-1 .. OD5-7, and the rulings
of RESUME 6, 7, 8, 9 and 10; binding unless I say otherwise in this session):

1. Identity bridge: the exposure is accepted. P9 cells and the single OD-3 read
   run on the universe as built; every print carries the flag; a freeze pass is
   worded "passed with the identity-bridge flag".
2. Debug vs Release `pow` / `exp2`: the visible Release skip stays; no re-pin in
   P9. Release is the producer of record; the two Release-built covariance
   stores are pinned and never rebuilt.
3. Local `main` and `origin/main`: untouched; listed for me in the final report.
4. No new-signal cell (P9-S lapsed at 0). No data purchase.
5. G-B2 hurdle 8.0%, labelled "plan placeholder, not owner-stated". P9-X is the
   Y-1 form (vol-target, cap 2.0); it is not a new trial if no P9 cell is
   accepted before it.
6. Parallel lanes: a lane may start on a dependency head that is APPROVE before
   root has merged it (W3-EARLY-2); it merges root's head again before its
   report. Root merges APPROVE SHAs, never branch tips (MERGE-SHA); the chains
   for D3W-PB, SIGN and A4 are in the ledger's "Dispatch root M5" line.
7. One gate for waves 2 and 3 on the head with every merge, one P9-B0 and one
   re-pin (GATE-1); whole suites only there (TEST-1 / ROOT-1a). Every lane that
   relinks an exe (D3W-PB, SIGN, A4, SQL4, the AL-CLOCK fix head) merges before
   the re-pin or not in P9 (EXE-FREEZE).
8. Permission system: B2-14, PERM-1, TEETH-POOL, TEETH-BIN-23 (pool-23's build
   tree stays; the refused deletion is mine to do by hand).
9. Stretch lanes: D3W-PB, SIGN, A4 are APPROVE and merge in M5 / M6; SQL4
   (pool-32, SQL4-GO; tasks 1-4 coded and uncompiled at the stop) merges only if
   APPROVE and in root before the gate's P9-B0 re-pin, else P10 untouched
   (SQL-12). A4's tests-only fix round (A4-S) never holds the gate: root merges
   the APPROVE head of record at its slot.
10. B2-N37 is resolved: cause (b) arithmetic (max |relative difference| 4.967e-14),
    the test compares at 1e-12 relative, golden untouched, APPROVE `101f695a`;
    the known-red set holds exactly that test until root merges it.
11. D3-Q1: at the gate, P9-F0h is produced with `--engine-fit` iff the engine
    fit's weights are byte-identical to the Python fit's on the current Y-F0
    TRAIN fit; else P9-W lapses (PRE-35) and the difference is a finding.
12. `C:\atx\data` (DATA-3): the class-D deletion was refused and is not retried;
    `data-classification.md` is the deliverable; I delete by hand if I want it.
13. COMPOSE-1: no composition test for `mv-aim-v1` + `info-clock-rate-v1`; the
    gate parses both flags together on the tiny world; a refusal there or at
    P9-MV's run lapses P9-MV at 0 (PRE-14 form), disclosed before any read.
14. COVMV-BYTES: the with-flag before / after byte compare of COV-MV's fix round
    is replaced by the reviewer's hunk classification + the S-1 bit-identity gtest
    + the gate's flag-absent identity.
15. A4-Q2: both flipped fields are in Y-F0's fields manifest; root runs the TRAIN
    identity on both (SHA only, bounded) before the flips merge; a miss blocks
    A4 (P10 whole).

Done means: waves 2 and 3 merged with 0 failures and the single gate run,
`p9-prereg.md` completed (round 2b-ii), reviewed and ruled, P9 cells run in the
registered order, adoption print, OD-3 read once, freeze gate verdict written,
and a final list of every ruling made in the sprint. Keep replies to me terse.
