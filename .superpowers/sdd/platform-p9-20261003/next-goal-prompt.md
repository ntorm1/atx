# Goal prompt for the next parent (PM) agent (refreshed at STOP 12, 2026-10-15 16:30)

Paste everything below the line as the `/goal` text. The paragraph "Decisions
already made for me" carries the delegation the owner gave ("do not stop to ask
questions, you are allowed to rule on all open questions"), the owner's
2026-10-07 words (parallel Opus implementers; quality / modular / reusable /
performant C++ centralized in atx-engine and atx-impl; no TDD; TEST-1: targeted
tests only until the gate), the owner's 2026-10-15 words (parallel progress,
PAR-1) and the structural rulings of RESUME 9 .. 12. Strike or change any you
disagree with before pasting. Three items wait on the owner (status-13 "Open
with the owner"): the `index.sqlite` deletion (GATE-IDX), OD3-DB, the
clean-up owner-calls.

---

Complete the P9 sprint plan
`C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project
manager.

Start by reading, in order, in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:

1. `status-13.md` (state, tracks table with every head, what is open with me,
   next steps; the ledger block from "- RESUME 12" to the end records every
   hand-back and ruling of the last session)
2. `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`,
   `resume10-addendum.md`, `resume11-addendum.md`, `resume12-addendum.md` (the
   standing contracts you give every agent; the last wins where they differ),
   `r12-dispatch.md` ("ROOT GATE" filled), `r12-post-gate.md` (PRE-2BII parts
   A / B, GATE-REV parts 1 / 2, the CELLS runbook and hand-back protocol,
   OD3-PREP, OD-3 with the OD3-GO checkpoint, DERIV, FREEZE),
   `r12-prompts/` (READY dispatch prompts: `gate-prompt-template.md` for the
   gate chain, `gaterev-prompt.md`, `cells-prompt.md`, `od3prep-prompt.md`,
   `deriv-prompt.md`; `ledger-cells-rulings-draft.md` = the CELLS-1 .. 6,
   ADOPT-1, OD3-1 / OD3-GO rulings NOT yet in the ledger: append them before
   the first cell), `r11-dispatch.md` "ROOT GATE" + "ROOT GATE: rulings from the
   whole-wave review" (still the gate's rulings text), `root-gate-brief.md`,
   then `progress.md` from "- RESUME 12" to the end (every "Ruling" line is
   binding, in that block and in the earlier ones)
3. `handoff-root-m6b.md` (M6B-FINISH: SQL4 checks green, SQL4 in the gate head;
   "What the gate root must know"), `handoff-root-gate-1.md` (the gate root
   stopped at OWNER STOP 12: what ran, "Exact next step"), `root-gate-report.md`
   as far as written
4. `p9-prereg.md` (round 2b-ii part A done at `7890883e`; part B after the
   gate), `rulings-index.md` (`5f48a25d`), `p9-od3-runbook.md` (OD-3)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Gate head
`06dc9e5a` (every merge of waves 2 and 3 in; SQL4 checks green). First verify
the stop state yourself (heads, clean trees, trial ledger 133 lines sha
`27e40f9f`, `build-equity/research-store/` absent, lease status: every lease
prints `owner=dead` except pool-32 `owner=alive`; both expected and untouched,
LEASE-DEAD; whether `build-equity/mega-candidate-cache-v8-lo3/index.sqlite`
still exists), read-only. Pools 8 and 24 are FROZEN (TEETH-POOL). Pool-23's
`build-equity/` holds mutated teeth binaries (TEETH-BIN-23): nobody deletes
it, nobody uses its exes. `C:/atx/atx-db/` is never opened (A4-DB) unless I
answer OD3-DB yes (read-only, the era builds only).

How to run it:

- You are PM only: write the ledger, briefs and rulings. Never build, run real
  data or edit code yourself. Use Opus subagents (model: opus) for every root,
  reviewer, PRE, DERIV and cells step. One root agent at a time in pool-2's
  working tree for code / builds / runs; chain fresh root agents through
  `handoff-root-gate-<n>.md` / `handoff-root-cells-<n>.md` when one runs low
  on context. Docs agents (PRE, reviewers, IDX, DERIV) may work beside root in
  pool-2's sprint dir but commit ONLY by pathspec (PAR-1). Hand artifacts over
  as files to protect your context. Reviewers of earlier sessions are gone: any
  re-review is a fresh reviewer that reads the previous review first (REV-FRESH).
- My words (2026-10-15): do not get blocked on long-running builds or trials;
  always make progress in parallel where possible (PAR-1 and its family:
  PRE-2BII-PAR, GATE-REV-PAR, GATE-PAR, OD3-PAR). The hard rules are not
  loosened by it: one C++ build among our pools, one bounded real-data run per
  pool, one root in pool-2's tree.
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

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/` (OD3-DB:
  only if I say yes, read-only, the era builds). No push. Never `git worktree
  prune`. Never touch `main` or `origin/main`. Never any lease action (no
  release, no stale recovery).
- Open no data dated 2024-01-01 or later. Never edit an expected hash. Never run
  `atx-impl/tools/holdout_gate.py` (the OD-3 runbook F.1 / `p9-od3-runbook.md`
  orders it never).
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it
  changes only through registered P9 cells.
- Configure only as
  `powershell scripts\atx-build.ps1 configure -Preset <preset>` (`equity-dev` /
  `equity-rel`; the `dev` preset does not configure on this branch); build only
  through `scripts/research-build.ps1`, target-scoped, one at a time among our
  pools. If the permission system refuses a command, do not retry it and do not
  route around it: report it to me (PERM-1); never re-do a refused deletion for
  a subagent (that is permission laundering). Never `git checkout -- <file>`,
  `git restore` or `git stash` on a working file (TEETH-RESTORE; TEETH-CRLF).
- Pytest with explicit paths only; `scripts/tests` and
  `atx-engine/tools/test_stage_chain.py` in separate invocations (at the gate).
- Disk: agents delete their own regenerable build trees and scratch by name once
  their report is committed; never wildcard-delete shared scratch; never delete
  pinned, reference or golden artifacts (the D-COV stores under
  `build-equity/p9-m2i-dcov-*` and `p9-m2d-cov-risk-container` are pinned; the
  real cache `mega-candidate-cache-v8-lo3`; every P9-B0 reference dir).
- Subagent commit trailer:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Ledger appends: write the lines to a scratch file and `cat >>` (a Bash heredoc
  with apostrophes fails in the hook layer); never edit earlier lines. Root
  commits the ledger and the PM's dispatch docs with its docs commits
  (DISPATCH-COMMIT); the PM never commits in pool-2.

Decisions already made for me (ledger OD-DELEG, OD5-1 .. OD5-7, and the rulings
of RESUME 6 .. 12; binding unless I say otherwise in this session):

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
   accepted before it (PRE-30); its parent is P9-F0h (PRE-MVX, MVX-F0H:
   every P9-F0 mention in B.7 / B.8 that names the leverage cells' base reads
   P9-F0h); P9-V pairs with P9-X; PRE-13 stands whole on P9-F0h (MVX-PRE13); a
   two-store NAV is P10.
6. One gate (GATE-1) on the head with every merge, one P9-B0 and one re-pin;
   whole suites only there. Step 0 DONE (GATE-0); step 8 DONE (WAVE-REV-EARLY;
   SQL4-ADD); steps 1-7 and 9 in progress by the root chain from
   `handoff-root-gate-1.md`. The gate's procedure rulings: GATE-EXES,
   GATE-COVDIFF, D3-Q1a, COMPOSE-1, COVMV-S2 S-b + NAV-S S-2 as COVMV-PIN now
   reads them (the `mv-aim-v1.json` sentence is NOT edited: pinned by the
   prereg; PRE-40 form), IC-S (G-P5 PARTIAL), PY-S S-6, SQL4-S S-1 / S-2 /
   S-3 / S-6 (done at M6B-FINISH), SQL-11, GATE-IDX (the leftover
   `index.sqlite`: mine to delete; no run of record names that cache while it
   exists), GATE-PAR. EXE-FREEZE from the re-pin.
7. A4: merged at its pre-flip head `6dab6948`; the flips and the fix-1 tests are
   P10 whole (A4-DB, A4-P10-FLIPS, A4-FIX1-P10). Python stays the producer of
   record for the two flipped fields; Y-F0's manifest `26fee5ce` is unchanged.
8. SQL4: APPROVE `44a76937`, merged at `06dc9e5a`; M6B-FINISH's checks all
   green (SQL4-CUTOFF-2): SQL4 is in the gate head; a gate red in its 24 files
   attributes to `06dc9e5a`.
9. Permission system: PERM-1, TEETH-POOL, TEETH-BIN-23; the refused deletions of
   this session (`index.sqlite`; the old scratch catalog) are mine by hand.
10. B2-N37 resolved; the known-red set is EMPTY.
11. D3-Q1 / D3-Q1a: at the gate, P9-F0h is produced with `--engine-fit` iff the
    engine fit's weights equal the Python fit's with `provenance.engine_fit`
    stripped (and the two admission files whole); else P9-W lapses (PRE-35).
12. `C:\atx\data` (DATA-3), `atx-db-request.md` (AL-DATA): unchanged, mine.
13. COMPOSE-1, COVMV-BYTES, MV-FLAG-P0 (root at the OD-3 P0 step): as ruled.
14. OD3-DB: the OD-3 era builds need read-only access to the atx-db stage paths
    listed in `status-13.md`; if I have not said yes by the OD-3 step, OD-3 is
    "not read in P9", G-B8 "not evaluated", the freeze verdict says so.
15. The cells run one at a time with one hand-back each and my explicit
    ACCEPTED / REJECTED line per cell written by the PM (CELLS-1 .. 6 as
    drafted); hand cells get an independent DERIV re-derivation (CELLS-2); the
    OD-3 read has a pre-read checkpoint (OD3-GO).

Done means: the single gate run green (0 failed, named skips only) on the head
with every merge, `p9-prereg.md` completed (round 2b-ii part B), reviewed and
ruled, P9 cells run in the registered order with explicit adopt / reject per
cell, the adoption print, OD-3 read once on P9-F0h (or recorded "not read" per
OD3-DB), the freeze gate verdict written (plan 4.4 / prereg G), and a final
report with every ruling made in the sprint (from `rulings-index.md`, re-checked
against the ledger) and the owner items. Keep replies to me terse.
