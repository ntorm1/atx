# Goal prompt for the next parent (PM) agent (refreshed at STOP 11, 2026-10-15)

Paste everything below the line as the `/goal` text. The paragraph "Decisions
already made for me" carries the delegation the owner gave ("do not stop to ask
questions, you are allowed to rule on all open questions"), the owner's
2026-10-07 words (parallel Opus implementers; quality / modular / reusable /
performant C++ centralized in atx-engine and atx-impl; no TDD; TEST-1: targeted
tests only until the gate) and the structural rulings of RESUME 9, 10 and 11.
Strike or change any you disagree with before pasting.

---

Complete the P9 sprint plan
`C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project
manager.

Start by reading, in order, in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:

1. `status-12.md` (state, lane table with every head, gates, next steps, what is
   open with me; the ledger block from "- RESUME 11" to the end records every
   hand-back and ruling of the last session)
2. `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`,
   `resume10-addendum.md`, `resume11-addendum.md` (the standing contracts you
   give every agent; the last wins where they differ), `r11-dispatch.md` (the
   root-group form, "WAVE REVIEW", and the READY gate dispatch text: "ROOT GATE"
   + "ROOT GATE: rulings from the whole-wave review"), `r7-review-dispatch.md` +
   `r8-review-notes.md` (reviewers), `r7-root-merge.md` + `root-gate-brief.md`
   (root), then `progress.md` from "- RESUME 11" to the end (every "Ruling"
   line is binding, in that block and in the earlier ones)
3. `handoff-root-m6.md`, `handoff-root-gate0.md`, `handoff-root-m6b.md` (root
   after the SQL4 merge at `06dc9e5a`; whether its checks finished)
4. the five `wave-review-<area>.md` files and pool-32 `task-SQL4-review.md`
   (the whole-wave review is DONE; no code defect; its Required items are
   procedure rulings already folded into `r11-dispatch.md`)
5. `p9-prereg.md` (round 2b-i; 2b-ii after the gate: the ledger's five
   "PRE 2b-ii list" lines), `p9-prereg-flags.md`, `wave3-carry.md`,
   `root-gate-brief.md` (the single gate, GATE-1)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Every lane
of waves 2 and 3 is merged or closed (P10). First verify the stop state yourself
(heads, clean trees, trial ledger 133 lines sha `27e40f9f`, lease status: every
lease prints `owner=dead` except pool-32 `owner=alive`; both expected and
untouched, LEASE-DEAD), read-only. Pools 8 and 24 are FROZEN (TEETH-POOL).
Pool-23's `build-equity/` holds mutated teeth binaries (TEETH-BIN-23): nobody
deletes it, nobody uses its exes. `C:/atx/atx-db/` is never opened (A4-DB).

How to run it:

- You are PM only: write the ledger, briefs and rulings. Never build, run real
  data or edit code yourself. Use Opus subagents (model: opus) for every root,
  reviewer and cells step. One root agent at a time in pool-2; chain fresh root
  agents through `handoff-root-gate-<n>.md` when one runs low on context. Hand
  artifacts over as files to protect your context. Reviewers of earlier sessions
  are gone: any re-review is a fresh reviewer that reads the previous review
  first (REV-FRESH).
- TEST-1 (my words, 2026-10-07): the whole suites, Release builds, canaries,
  golden and stage chain run ONCE at the gate. A gate red is attributed by merge
  commit and returned to its lane (BLOCKED; the merge is not reverted).
- Priority: high quality end-to-end implementations; modular, reusable,
  performant code centralized in the atx-engine and atx-impl C++ code base; keep
  moving from JSON artifacts and repeated Python scripts to SQLite and core C++.
- Decide, don't stall, and do not stop to ask me questions: you may rule on
  every open question. Record every decision in the ledger as "Ruling: decision
  -- why -- cost if wrong" before the measurement it could bias. Stop only for
  irreversible or destructive actions that I have not asked for,
  security-sensitive actions, pushes or merges outside the worktrees, or a plan
  so broken every path is a guess.
- Treat a subagent's report as its own account, not as proof; root re-proves.

Hard rules:

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/`. No push.
  Never `git worktree prune`. Never touch `main` or `origin/main`. Never any
  lease action (no release, no stale recovery).
- Open no data dated 2024-01-01 or later. Never edit an expected hash. Never run
  `atx-impl/tools/holdout_gate.py` except as the OD-3 runbook (prereg F.1)
  orders it, once, by the cells root agent.
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it
  changes only through registered P9 cells.
- Configure only as
  `powershell scripts\atx-build.ps1 configure -Preset <preset>` (`equity-dev` /
  `equity-rel`; the `dev` preset does not configure on this branch); build only
  through `scripts/research-build.ps1`, target-scoped, one at a time among our
  pools. If the permission system refuses a command, do not retry it and do not
  route around it: report it to me (PERM-1). Never `git checkout -- <file>`,
  `git restore` or `git stash` on a working file (TEETH-RESTORE; TEETH-CRLF).
- Pytest with explicit paths only; `scripts/tests` and
  `atx-engine/tools/test_stage_chain.py` in separate invocations (at the gate).
- Disk: agents delete their own regenerable build trees and scratch by name once
  their report is committed; never wildcard-delete shared scratch; never delete
  pinned, reference or golden artifacts (the D-COV stores under
  `build-equity/p9-m2i-dcov-*` and `p9-m2d-cov-risk-container` are pinned).
- Subagent commit trailer:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Ledger appends: write the lines to a scratch file and `cat >>` (a Bash heredoc
  with apostrophes fails in the hook layer); never edit earlier lines. Root
  commits the ledger and the PM's dispatch docs with its docs commits
  (DISPATCH-COMMIT); the PM never commits in pool-2.

Decisions already made for me (ledger OD-DELEG, OD5-1 .. OD5-7, and the rulings
of RESUME 6 .. 11; binding unless I say otherwise in this session):

1. Identity bridge: the exposure is accepted. P9 cells and the single OD-3 read
   run on the universe as built; every print carries the flag; a freeze pass is
   worded "passed with the identity-bridge flag".
2. Debug vs Release `pow` / `exp2`: the visible Release skip stays; no re-pin in
   P9. Release is the producer of record; the pinned covariance stores are never
   rebuilt (CRT-2).
3. Local `main` and `origin/main`: untouched; listed for me in the final report.
4. No new-signal cell (P9-S lapsed at 0). No data purchase.
5. G-B2 hurdle 8.0%, labelled "plan placeholder, not owner-stated". P9-X is the
   Y-1 form (vol-target, cap 2.0); it is not a new trial if no P9 cell is
   accepted before it (PRE-30); its parent is P9-F0h (PRE-MVX: the last accepted
   book that does not read `--risk-model`; P9-V pairs with P9-X; a two-store NAV
   is P10).
6. One gate (GATE-1) on the head with every merge, one P9-B0 and one re-pin;
   whole suites only there. Step 0 of the gate is DONE (GATE-0); step 8 (the
   whole-wave review) is DONE (WAVE-REV-EARLY; SQL4-ADD). The gate's procedure
   rulings: GATE-EXES (the two research exes in the frozen set), GATE-COVDIFF
   (cov-diff on the three container stores), D3-Q1a (engine-fit identity with
   `provenance.engine_fit` stripped), COMPOSE-1 (the fake-store parse check of
   both rules), COVMV-S2 S-b + NAV-S S-2 (the `mv-aim-v1.json` sentence at the
   re-pin iff not yet pinned), IC-S (G-P5 prints PARTIAL: four pairs unlisted),
   PY-S S-6 (`ledger_n` vs the verb's N by hand at the scoreboard step), SQL4-S
   S-1 / S-2 / S-3 / S-6 (root's SQL4 checks), SQL-11 (no cache flag on any cell
   argv). EXE-FREEZE from the re-pin.
7. A4: merged at its pre-flip head `6dab6948`; the flips and the fix-1 tests are
   P10 whole (A4-DB, A4-P10-FLIPS, A4-FIX1-P10). Python stays the producer of
   record for the two flipped fields; Y-F0's manifest `26fee5ce` is unchanged.
8. SQL4: APPROVE `44a76937`, merged at `06dc9e5a` before the gate (SQL4-CUTOFF
   met); if M6b's checks left a red, that merge is BLOCKED and SQL4 is P10 at the
   gate (the merge not reverted; the gate head taken as it stands, stated).
9. Permission system: PERM-1, TEETH-POOL, TEETH-BIN-23; the A4 re-apply refusal
   (R11 #1) is mine to do by hand if I want the fix-1 tests in.
10. B2-N37 resolved (1e-12 relative; golden untouched); the known-red set is
    EMPTY.
11. D3-Q1 / D3-Q1a: at the gate, P9-F0h is produced with `--engine-fit` iff the
    engine fit's weights equal the Python fit's with `provenance.engine_fit`
    stripped (and the two admission files whole); else P9-W lapses (PRE-35).
12. `C:\atx\data` (DATA-3), `atx-db-request.md` (AL-DATA): unchanged, mine.
13. COMPOSE-1, COVMV-BYTES, MV-FLAG-P0 (root at the OD-3 P0 step): as ruled.

Done means: the single gate run green (0 failed, named skips only) on the head
with every merge, `p9-prereg.md` completed (round 2b-ii), reviewed and ruled,
P9 cells run in the registered order with explicit adopt / reject per cell, the
adoption print, OD-3 read once on P9-F0h, the freeze gate verdict written
(plan 4.4 / prereg G), and a final report with every ruling made in the sprint
and the owner items. Keep replies to me terse.
