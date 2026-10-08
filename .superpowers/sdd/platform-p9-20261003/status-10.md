# P9 sprint: PM handoff 10 (owner stop 9, 2026-10-07)

For the next parent (PM) agent. Supersedes `status-8.md` (no `status-9.md` exists: ruling STATUS-9; the RESUME 9 state
table is in `resume9-addendum.md`). The goal prompt to start the next session is in `next-goal-prompt.md` (same dir,
refreshed at this stop). Written at the owner's stop ("stop here"); hand-backs that had not arrived when this file was
written are marked (pending) and their pool handoff / review files are the truth. Verify every head with `git log`
before dispatch. The ledger `progress.md` block from "- STOP 9" records every hand-back as it arrived.

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md` (incl. its TEST-1 section; wins where they
   differ). Per-lane text: `r9-dispatch.md` (points at `r7-dispatch.md` / `r8-dispatch.md`, still the task lists).
   Reviewers: `r7-review-dispatch.md` + `r8-review-notes.md`. Root: `r7-root-merge.md` + `r9-dispatch.md` "ROOT".
3. The ledger `progress.md` from the line "- RESUME 9" to the end. Every "Ruling" line is binding (so are the older
   ones); the RESUME 9 rulings are listed at the end of this file.
4. `handoff-root-m2.md` (root: B2 + T2 merged, identity equal, the n37 red) and `handoff-root-m3.md` (root M3 stopped
   before its first merge).
5. `p9-prereg.md` (round 2b-i committed at `685e238b`; 2b-ii after the gate) and `p9-prereg-flags.md`;
   `wave3-carry.md` ("PM answers" and "Added after drafting" win); `root-gate-brief.md` (the single gate, GATE-1).
6. Lane handoffs / reviews in the pools, when you dispatch that lane (table below).

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`, unchanged
(0 trials this session).

## State in short

Wave 2 is COMPLETE in root: B2's post-identity redo (`5dfb0ba1`) and T2 (`27e69387`) merged in RESUME 9; TRAIN
identity equal on X-5 and Y-F0. Wave 3: every lane has handed back; EIGHT lanes are APPROVE (OD3P, D3W, B2, C3b,
COV-MV, ABS, AL-CLOCK, D3; D3 is the critical path, PRE-35's D3 side met); SQL3 was merged in RESUME 8. Root M3
(merges of C3b, D3W, ABS) was dispatched and stopped before its first merge. Three fix rounds (B2 n37, COV-MV
performance items, AL-CLOCK calibrate test) and two follow-ups (SQL3 item 5, OD3P item 6) were in flight at the stop.
Stretch: D3W-PB DONE with a PARTIAL review (provisional CHANGES_REQUIRED, see its file), A4 DONE with a PARTIAL
review (no finding so far), SIGN in progress. The known-red set holds ONE entry (the legacy n37 test, B2-N37) until
B2 fix round 2 lands. No gate run, no P9 cell run. The prereg is at round 2b-i. Owner rule TEST-1 (2026-10-07): targeted
tests only until the gate; whole suites, Release, canaries, stage chain run ONCE at the gate.

Root at the stop: code head `fcb0200e`, docs head `baefd8a9`; `progress.md` has uncommitted lines after 752 (the next
root agent's step 0 commits them with `status-10.md` and `next-goal-prompt.md`, `git add -f`; never the PNGs). Debug
object tree `build-equity/` kept (exes of record: `p9-2t`, `handoff-root-m2.md`); next free tag `p9-2u`. Host at the
stop: free RAM about 2-3 GB (another session's python processes), C: about 58 GB free.

## Lanes at the stop

| lane | pool / branch | head | state | next |
|---|---|---|---|---|
| SQL1, SQL2, A3, AL-SIG, S2, D2, COV, C2, AL-COMB, S2R, B2 identity, E2, D2R, COVR, C3 part A, SQL3, B2 redo, T2 | merged | root `fcb0200e` | complete | gate |
| B2 fix round 2 (n37) | pool-14 `feat/p9-b2-20261003` | `73b4acfd`-based; (pending) | B2-N37: cause (a) formatting -> code fix, or (b) arithmetic -> numeric 1e-12 compare, golden untouched | finish from `handoff-B2.md`; scoped re-review (resume the B2 reviewer, BASE `46ffe3dc`); root merges as a follow-up; clears the known-red set |
| C3b | pool-15 `feat/p9-c3-20261004` | `76b9f256` APPROVE | review `task-C3b-review.md` 0 Required | root M3 merges `76b9f256` (keep-both with B2; D3 later) |
| D3 | pool-20 `feat/p9-d2-20261003` | `e197b3b6` APPROVE (code `699440bb`) | review `task-D3-review.md` 0 Required; D3-S1 ruled (P10 guard + gate grep) | root M4 merges `e197b3b6`; C3-5 probes (a) / (b), C3-13; keep-both with C3b's fixture lines / hunks and B2's `:171` line |
| D3W | pool-23 `feat/p9-d3w-20261005` | `f0726acb` APPROVE (code `1e6c4336`) | review `task-D3W-review.md` 0 Required / 10 Suggested (D3W-S) | root M3 merges `f0726acb` (B2-EVAL-KEEP on `eval_report.cpp`) |
| D3W-PB (stretch) | pool-23, same branch above D3W | `a0c7427c` review PARTIAL (lane head `460f5ba2` = deletion; pre-deletion `c044f5f6`) | lane DONE; review PARTIAL "provisional CHANGES_REQUIRED" (pending hand-back; read `task-D3W-PB-review.md`) | resume the D3W-PB reviewer; fix round if Required; merges only if APPROVE at the gate (D3W-PB) |
| ABS | pool-28 `feat/p9-abs-20261006` | `6a3f5043` APPROVE (code `5dae4f69`) | review `task-ABS-review.md` 0 Required; ABS-2 .. ABS-5 | root M3 merges `6a3f5043` after D3W; SQL2-CLS rows `atx.od3-flagged-members/v1`, `atx.carry-weights/v1`; MUST be in the gate head (EXE-FREEZE) |
| AL-CLOCK | pool-30 `feat/p9-alclock2-20261007` (pool-8 FROZEN) | `a662d1c2` (fix-1 handoff; APPROVE head `e9b3a323`, code `1cbe5233`) | APPROVE; fix round 1 (ALCLOCK-S: S-3 calibrate gtest REQUIRED, S-1 / S-2 / S-4 text) stopped before any edit, design in the handoff; ALCLOCK-S1a / S3a ruled | finish fix round 1 from `handoff-AL-CLOCK.md`; scoped re-review (resume the AL-CLOCK reviewer, BASE `e9b3a323`); root M4 merges the fix head, else `e9b3a323`; BEFORE COV-MV |
| COV-MV | pool-31 `feat/p9-covmv2-20261007` (pool-24 FROZEN) | `e9e794b3` APPROVE (code `846739c1`); fix round 1 (pending; pool-31 had 9 dirty paths at the stop) | COVMV-S: S-1 / S-2 / S-6 REQUIRED (allocation, print-only abort, per-decision axes check) | finish from `handoff-COV-MV.md`; scoped re-review (resume the COV-MV reviewer, BASE `bda51b98`); root M4 merges after AL-CLOCK (CMakeLists tail: AL-CLOCK's block first) |
| OD3P | pool-21 `feat/p9-od3p-20261004` | `e7d7c23a` (follow-up handoff) above `a1348616` WIP (UNTESTED) and `dc11c977` APPROVE | follow-up: item 6 code + S-d code written, untested; S-a, S-b, S-c not done; OD3P-T2V / OD3P-SKIP ruled | finish from `handoff-OD3P.md` (tests, teeth, docs); scoped re-review (resume the OD3P reviewer, BASE `97a8157f`); root merges `dc11c977` in its slot and the follow-up as a second merge |
| SQL3 item 5 | pool-13 `feat/p9-sql3-20261004` | `2d02466e` (merge of root; 3 dirty at the stop; pending) | the three call lines | finish from `handoff-SQL3.md`; scoped review; short root merge (Python only) |
| A4 (stretch) | pool-27 `feat/p9-a4-20261005` | `9e80a7d5` review PARTIAL (lane `7c9140c4`; pre-flip `6dab6948`; flips `69141218`, `59d807ea`) | lane DONE_WITH_CONCERNS; review read to `holdings_regsho.cpp:200`, no finding so far; A4-Q1 .. Q3, A4-REV ruled | resume the A4 reviewer from its "Exact next step"; merges only if APPROVE at the gate (A4-1) |
| SIGN (stretch) | pool-29 `feat/p9-sign-20261006` | `9c360cde` (merge of root; 7 dirty at the stop; pending) | C3b and root merged; implementation in progress | finish from `handoff-SIGN.md`; review; merges only if APPROVE at the gate (C3-11) |
| SQL4 (3b) | no pool | not dispatched | after SQL3, S2, B2, C3, D3 in root; else P10 untouched (SQL-12) | PM decides at the gate |
| PRE | sprint dir `p9-prereg.md` | round 2b-i | 2b-ii after the gate: the running list in the ledger ("PRE 2b-ii list") | review, then the PM rules the file |

Frozen pools (TEETH-POOL): pool-8 (AL-CLOCK, six C++ files hold teeth set 1) and pool-24 (COV-MV, two `// TEETH`
lines) wait for the OWNER; the exact `git checkout --` lines are in the ledger ("PERMISSION REFUSALS carried from
RESUME 8"); nothing in them is built, committed or evidence.

## Open with the owner (surfaced; none blocks engineering)

1. **Permission refusals** (PERM-1 held, nothing retried): none new in RESUME 9. Carried: the two teeth reverts (pools
   8, 24), the DATA-2 deletion dispatch, B2's scratch Write, and the earlier ones; full list in the ledger.
2. **Frozen pools 8 and 24**: restore by hand with the two `git checkout --` lines, or leave them; the lanes continue in
   pools 30 / 31 on new branches (root merges those).
3. **`C:\atx\data` clean-up**: DATA-3: the DATA-2 deletion was refused in RESUME 8 and is not retried; the deliverable
   is `data-classification.md` (A 2 / B 78 / C / D 99 / U); the owner deletes class D by hand if wanted.
4. **Host**: another session's python processes hold most of the RAM (1.9-2.7 GB free of 15.7); builds admit at
   1000 MiB and serialize (HOST-2 / HOST-3). The gate's Release builds will be slow on this host.
5. **Rulings the owner may want to look at**: TEST-1 (your words, as read), TEETH-POOL (fresh pools instead of the
   refused reverts), MERGE-SHA, B2-N37 (a numeric 1e-12 comparison for a legacy golden if the difference is
   arithmetic), PRE-15-N (124), D3-Q1 (P9-W needs an `--engine-fit` parent: identity proven at the gate or P9-W
   lapses), D3WPB-Q2 (walk-forward rows keep TRAIN-fitted aim gains: disclosed), ABS-4 (absence by signal blanking),
   COVMV-S / ALCLOCK-S (fix rounds ordered on Suggested items under your quality bar).
6. **Owner items for the final report** (unchanged): local `main` / `origin/main` (OD5-3), the deterministic `pow` /
   `exp2` re-pin (OD5-2), data purchase (OD5-5), the atx-db request file (AL-DATA), the orphaned scratch dir
   (SCRATCH-2), the broken `dev` preset, the shared `spdlog-build` relink (SPDLOG-RELINK, P10).

## Open questions, NOT ruled (the next PM rules these)

- Whatever the pending hand-backs list as open (B2 fix 2, SQL3 item 5, COV-MV fix 1, D3W-PB review, SIGN): read each
  handoff / review before dispatching.
- D3W-PB review's provisional CHANGES_REQUIRED: read `task-D3W-PB-review.md`; if a Required stands, one fix round in
  pool-23, else P10 whole (D3W-PB).
- Whether root M3 should also merge D3 (`e197b3b6`) and the AL-CLOCK / COV-MV APPROVE heads if their fix rounds are
  not done when root reaches their slots (the rulings say: merge the APPROVE head, the round follows as a second merge).

## Gates and rules the next PM must hold (new this session; status-8 lists the earlier ones)

- TEST-1 / ROOT-1a: targeted checks per lane and per merge; the whole suites, Release, canaries, golden, stage chain
  ONCE at the gate; a gate red is attributed by merge commit and returned to its lane.
- MERGE-SHA: root merges APPROVE SHAs, never branch tips (D3W-PB shares D3W's branch).
- B2-EVAL-KEEP (`eval_report.cpp` keep-both at D3W's merge, then eval gtests + the PRE-15 gtest); C3B-6 done in root;
  the CMakeLists tail order AL-CLOCK then COV-MV (W3-9).
- Known-red set = `test_legacy_n37_numbers_reproduced` only, until B2 fix round 2 merges; the gate needs it empty.
- D3-Q1 (engine-fit identity on Y-F0 at the gate, else P9-W lapses), D3-Q2 / D3-S1 (gate greps for `fields.output`
  and direct `nav.rule` in P9 manifests), D3-Q3 (P9-W names `--walk-forward-min-decisions 21`), PRE-15-N (124),
  ALCLOCK-4 (realised leverage printed from the summary), A4-Q2 (TRAIN identity iff the field is in the parent's
  manifest), ABS must be in the gate head (EXE-FREEZE).
- TEETH-RESTORE (tools restore their own bytes; never `git checkout` / `restore` / `stash`), TEETH-POOL, HOST-3,
  SPDLOG-RELINK (EXE-CUR by receipt), DIRTY-1 / 1a / 2, PERM-SCRATCH.
- Everything of status-8: GATE-1, PRE-35, C3-13, W3-8a, OD3P-1, ALCLOCK-1 / -2 / -3, COV-14, PRE-40, C3-11, C3-12,
  SQL3-1 / -2, D3-1 / -2, A4-1, ENV-1, PERM-1, EXE-CUR, SEAL-ALLOW-3, COVR-1, CRT-2, COV-13, W3-EARLY-2 / HOST-2,
  PRE-14, B2-14, C3-5 / C3-6, ROOT-1, ledger appends by scratch file + `cat >>` (a heredoc with apostrophes fails in
  the hook layer), never edit earlier lines.

## Next steps, in order

1. Verify the stop state: `git log` / `git status` in pools 2, 13, 14, 15, 20, 21, 23, 27, 28, 29, 30, 31 (8 and 24
   frozen, read-only); read each pending handoff; trial ledger (133, `27e40f9f`); `lease-worktree.ps1 -Status` (never
   stale recovery). DIRTY-1 / DIRTY-2 for any dirty tree.
2. Root M3 (fresh root agent): step 0 housekeeping docs commit (`progress.md`, `status-10.md`, `next-goal-prompt.md`,
   `git add -f`); then the merges as dispatched (ledger "Dispatch root M3"): C3b `76b9f256`, D3W `f0726acb`, ABS
   `6a3f5043`, each with its review's "What root must check at merge" and ROOT-1a; then M4: D3 `e197b3b6`, AL-CLOCK
   (fix head or `e9b3a323`), COV-MV (fix head or `e9e794b3`), OD3P `dc11c977` (+ follow-up), SQL3 item 5, B2 fix 2,
   D3W-PB / A4 / SIGN iff APPROVE.
3. In parallel: finish the three fix rounds and two follow-ups from their handoffs (fresh agents); scoped re-reviews by
   RESUMING the original reviewers (names in the ledger: `review-B2`, `review-COVMV`, `review-ALCLOCK`, `review-OD3P`;
   if the session is new they are gone: dispatch fresh scoped reviewers with REV-BASE); resume the D3W-PB and A4
   reviewers from their "Exact next step"; finish SIGN and review it.
4. The single gate (GATE-1; `root-gate-brief.md`): suites Debug + Release (0 failed, named skips only), golden at 1 and 4
   workers, canaries, the parents' flag-absent identity, D3-Q1's engine-fit identity, the gate greps (D3-Q2, D3-S1),
   P9-B0 (substitution list first; R-2, Y-F0, Y-1 re-based on the Release v1.1 store, PRE-29; `--ann-mean-gross` on
   every reference; `--info-clock-print` on Y-F0 iff AL-CLOCK is in), the re-pin incl. `exes.eval`, the store step
   (M2A-4, non-default catalog path, SQL3-2), scoreboard, G-P ticks, CRT record, rho of the P9 pairs, the whole-wave
   review split by area. EXE-FREEZE from the re-pin.
5. PRE round 2b-ii (Appendix IV values + the ledger's "PRE 2b-ii list": D3W-S S-6, D3W-STALE, PRE-40, PRE-15-N,
   ALCLOCK-4, D3-Q1, D3-Q3, D3WPB-Q2, A4-Q2, the deferred runs), a review by a fresh agent, the PM rules the file.
6. Cells in order: P9-L, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X, P9-V (lapses per PRE-14 / PRE-35 / D3-Q1; P9-X per
   PRE-30 / PRE-34); adoption print; OD-3 once on P9-F0h (four history lines) after OD3P + ABS are merged; freeze gate;
   the final list of every ruling.

## Rulings made this session (full text in the ledger from "- RESUME 9")

Process: STATUS-9, DIRTY-1a, DIRTY-2, TEETH-POOL, TEETH-RESTORE, HOST-3, PERM-SCRATCH, TEST-1 (owner), MERGE-SHA,
REV (R9-REVIEWS), POOL-MAP (R9-1), A4-3, DATA-3, STOP 9.
Lanes: D3W-STALE, SPDLOG-RELINK, B2-EVAL-KEEP, B2-SEQ, B2-S, B2-N37, PRE-15-N, T2-BUILD, OD3P-S, OD3P-T2V, OD3P-SKIP,
D3W-S, D3WPB-Q1, D3WPB-Q2, ABS-2, ABS-3, ABS-4, ABS-5, ABS-S, C3B-6, C3B-S, ALCLOCK-4, ALCLOCK-T2, ALCLOCK-S,
ALCLOCK-S1a, ALCLOCK-S3a, COVMV-S, D3-Q1, D3-Q2, D3-Q3, D3-S1, D3-S, A4-Q1, A4-Q2, A4-Q3, A4-REV, C3-14 (stands).
Owner's words recorded: the /goal text of 2026-10-07 (OWNER WORDS), TEST-1, STOP 9.

## Standing warnings (in addition to status-8)

- Twelve agents at once ran here with 1.9-2.7 GB free RAM; builds queued at the memory gate and took 10-40 minutes
  each; the wait loops do not enforce the HOST-3 priority order (first come): dispatch the critical-path builders first.
- A hand-back that says a test "ran" before TEST-1 reached the agent is honest evidence; the gate's run is the run of
  record (TEST-1).
- Resuming a finished reviewer by name re-opens it with its context (cheap scoped re-reviews); a resumed agent that was
  already done may produce a second notification saying so.
- The `.superpowers/` dir is git-ignored: `git status` hides untracked records; `ls -lt` each pool's sprint dir to find
  them (AL-CLOCK's report and logs existed only untracked in frozen pool-8 and were copied to pool-30 by `cp`).
