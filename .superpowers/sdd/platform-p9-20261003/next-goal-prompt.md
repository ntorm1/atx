# Goal prompt for the next parent (PM) agent (refreshed at STOP 9, 2026-10-07)

Paste everything below the line as the `/goal` text. The paragraph "Decisions
already made for me" carries the delegation the owner gave ("do not stop to ask
questions, you are allowed to rule on all open questions"), the owner's
2026-10-07 words (parallel Opus implementers; quality / modular / reusable /
performant C++ centralized in atx-engine and atx-impl; no TDD; TEST-1: targeted
tests only until the gate) and the structural rulings of RESUME 9. Strike or
change any you disagree with before pasting.

---

Complete the P9 sprint plan
`C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project
manager.

Start by reading, in order, in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:

1. `status-10.md` (state, lane table with every head, gates, next steps, what is
   open with me, the open questions; hand-backs marked (pending) had not arrived
   when it was written: their pool handoff / review files are the truth; the
   ledger block from "- STOP 9" records the ones that did arrive)
2. `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md` (the
   standing contracts you give every agent; the last wins where they differ and
   carries TEST-1), `r9-dispatch.md` (per-lane task lists, pointing at
   `r7-dispatch.md` / `r8-dispatch.md`), `r7-review-dispatch.md`
   - `r8-review-notes.md` (reviewers), `r7-root-merge.md` + `r9-dispatch.md`
     "ROOT" (root), then `progress.md` from "- RESUME 9" to the end (every
     "Ruling" line is binding, in that block and in the earlier ones)
3. `handoff-root-m2.md` (B2 + T2 merged, identity equal, the n37 red) and
   `handoff-root-m3.md` (M3 stopped before its first merge; its merges C3b
   `76b9f256`, D3W `f0726acb`, ABS `6a3f5043` are the first thing root does)
4. `p9-prereg.md` (round 2b-i committed; 2b-ii after the gate: the ledger's "PRE
   2b-ii list" lines), `p9-prereg-flags.md`, `wave3-carry.md` ("PM answers" and
   "Added after drafting" win), `root-gate-brief.md` (the single gate, GATE-1)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Lanes live in
pooled worktrees with their own handoff files; resume them from those files with
fresh agents, never restart. Do not re-dispatch anything the ledger marks
complete or APPROVE. First verify the stop state yourself (heads, clean trees,
trial ledger, lease status), read-only; a lane whose tree is dirty at the stop
is resumed by an agent that first commits that file as the lane's handoff
(DIRTY-1 / DIRTY-2). Pools 8 and 24 are FROZEN (TEETH-POOL): read-only until I
restore them.

How to run it:

- You are PM only: write the ledger, briefs and rulings. Never build, run real
  data or edit code yourself. Use Opus subagents (model: opus) for every lane,
  reviewer and root-integrator step. Run as many lanes in parallel as are
  independent (twelve at once worked on 2 GB free RAM; builds serialize at the
  memory gate, HOST-2 / HOST-3); one root agent at a time in pool-2. Hand
  artifacts over as files to protect your context. A finished reviewer can be
  resumed by name for a scoped re-review within the same session.
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
  `main` or `origin/main`.
- Open no data dated 2024-01-01 or later. Never edit an expected hash. Never run
  `atx-impl/tools/holdout_gate.py`.
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it
  changes only through registered P9 cells.
- Configure only as
  `powershell scripts\atx-build.ps1 configure -Preset <preset>` (lanes use
  `equity-dev` / `equity-rel`; the `dev` preset does not configure on this
  branch); build only through `scripts/research-build.ps1`, target-scoped, one
  at a time. If the permission system refuses a command, do not retry it and do
  not route around it: report it to me. Never `git checkout -- <file>`,
  `git restore` or `git stash` on a working file (TEETH-RESTORE: teeth tools
  restore their own bytes).
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
of RESUME 6, 7, 8 and 9; binding unless I say otherwise in this session):

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
   report. D3W-PB continues on D3W's branch; root merges APPROVE SHAs, never
   branch tips (MERGE-SHA).
7. One gate for waves 2 and 3 on the head with every merge, one P9-B0 and one
   re-pin (GATE-1); whole suites only there (TEST-1 / ROOT-1a).
8. Permission system: B2-14 (a merge conflict is resolved region by region with
   ordinary edits, never by a wholesale checkout, restore or copy-over), PERM-1
   (a refused command is never retried or replaced by a same-effect substitute;
   only the form the refusal's own message names may be used, once, and is
   reported), TEETH-POOL (the two lanes whose teeth reverts were refused
   continue in fresh pools 30 / 31; pools 8 / 24 wait for me).
9. Stretch lanes A4 (pool-27), SIGN (pool-29), D3W-PB (pool-23 above D3W) merge
   only if APPROVE at the gate; else P10 whole. ABS (pool-28) is NOT stretch: it
   must be in the gate head (EXE-FREEZE) or the clean-source line prints "not
   read: no producer".
10. B2-N37: the legacy n37 reproduction test red after B2's merge is fixed by B2
    fix round 2 (formatting fix, or a numeric 1e-12 comparison with the golden
    untouched); the known-red set holds exactly that test until it lands.
11. D3-Q1: at the gate, P9-F0h is produced with `--engine-fit` iff the engine
    fit's weights are byte-identical to the Python fit's on the current Y-F0
    TRAIN fit; else P9-W lapses (PRE-35) and the difference is a finding.
12. `C:\atx\data` (DATA-3): the class-D deletion was refused by the permission
    classifier in RESUME 8 and is not retried; `data-classification.md` is the
    deliverable; I delete by hand if I want it.

Done means: waves 2 and 3 merged with 0 failures and the single gate run,
`p9-prereg.md` completed (round 2b-ii), reviewed and ruled, P9 cells run in the
registered order, adoption print, OD-3 read once, freeze gate verdict written,
and a final list of every ruling made in the sprint. Keep replies to me terse.
