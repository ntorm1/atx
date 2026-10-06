# P9 sprint: PM handoff 8 (owner stop, 2026-10-05)

For the next parent (PM) agent. Supersedes `status-7.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir). Written at the owner's second stop ("stop here and write status file + goal prompt
for next parent agent", then "dont wait just write master handoff"): FIVE lane hand-backs had NOT arrived when this
file was written (B2, D3, D3W, SQL3, OD3P); their rows below come from `git log` / `git status` at the moment of
writing, and each lane's handoff file in its pool is the truth. Verify every head with `git log` before dispatch.

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md` (new; wins where they differ). Reuse all four. Per-lane text: `r7-dispatch.md` (every
   section is still the lane's task list; resume from the lane's handoff, not from scratch). Reviewers:
   `r7-review-dispatch.md`. Root merges: `r7-root-merge.md`.
3. The ledger `progress.md` from the line "- RESUME 7" to the end. Every "Ruling" line is binding (so are the older
   ones). This session's rulings are listed at the end of this file.
4. `handoff-root-m2j-fin.md` (root: M2j finished, item 8 stopped by rule; `handoff-root-m2j.md` is its predecessor).
5. `p9-prereg.md` (still "ROUND 2a - not ruled"; Appendix IV indexes the 33 round-2b placeholders; PRE-32 .. PRE-40
   and COV-14 are NOT in the text yet) and `p9-prereg-flags.md`; `wave3-carry.md` ("PM answers" and "Added after
   drafting" win), as amended by the rulings of RESUME 5, 6 and 7.
6. Lane handoffs, when you dispatch that lane (table below).

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`, unchanged
(0 trials this session; checked by the PM at the stop).

## State in short

Wave 2: every lane but two is merged and verified in root; B2's post-identity redo (in progress) and T2 (APPROVE,
merges last) remain. Root's known-red set is EMPTY (M2j finish added `atx.nav-spec/v1` to `classes.json`). Wave 3:
C3 part A merged and its re-proof finished (items 4, 5, 1, 6, 9 hold); the C3-5 probe found a DUPLICATE `--rule` on
the NAV argv of a kind `rule` wave naming an aim row (ruled C3-13: D3's fix round). Nine lanes were running under
W3-EARLY-2 when the owner stopped: B2, D3 (guards and kinds), D3W (new: walk-forward C++), AL-CLOCK, COV-MV, C3b,
SQL3, OD3P, A4 (new stretch). None is reviewed; none is merged. The prereg is at round 2a. No P9 cell has run.
336 "Ruling" lines in the ledger at the stop (the count by `grep -c Ruling`; includes non-ruling mentions).

## Done this session

- PM: read everything in status-7's order; verified the stop state; 20 rulings (list at the end); split D3 part A
  into D3 (guards, kinds) and D3W (walk-forward C++, runner v3, fitter) under D3-2; ruled one gate for waves 2 + 3
  (GATE-1); leased pool-23 (D3W) and pool-27 (A4); wrote `resume7-addendum.md`, `r7-dispatch.md`,
  `r7-review-dispatch.md`, `r7-root-merge.md`.
- Root M2j finish (DONE except item 8): housekeeping `a15ad887`, SQL2-CLS `ebc918c3`, docs `1b64ce45`. Item 4
  (`--ann-mean-gross` changes only `summary.json`), item 5 (Release = Debug NAV on X-5 outside `/producer`), item 1
  (Release whole target-tests 390 / 0; `atx-impl-tests` 1,201 / 5 skipped / 0 on both builds), item 6, item 9
  (ROOT-1 512 / 4 / 0; stage chain 9) all HOLD; M2I-HK deletion done. Item 8 (C3-5 probe) STOPPED on the duplicate
  `--rule aim-partial-v5` (`scripts/cycle/phases.py:829` emits the parent's `nav.rule`; the row's flag is appended
  through `scripts/wave_kinds.py:385-399` and `scripts/research_spec.py:128`; the exe refused, exit 2). Next free
  build tag `p9-2r`. Exes p9-2p / p9-2q are current (no C++ / CMake change since `327c86b2`).

## Lanes at the stop

"Stopped" = told to stop at a clean point, commit (`WIP(<lane>):` for unfinished work), write the handoff, hand back.
Rows marked (hand-back received) are the lane's own account; rows marked (no hand-back yet) are from git at writing
time: read the handoff file first. A subagent of this session cannot be resumed by a new session: dispatch fresh
agents from the handoffs.

| lane | pool / branch | head at writing | state | next |
|---|---|---|---|---|
| SQL1, SQL2, A3, AL-SIG, S2, D2, COV, C2, AL-COMB, S2R, B2 identity, E2, D2R, COVR, C3 part A | merged | - | complete | gate |
| AL-DATA (doc-only) | `al-data/` | - | complete | request file goes to the owner with the freeze report |
| B2 redo follow-up 2 | pool-14 `feat/p9-b2-20261003` | `c3201a60` (merge of root `ebc918c3`), 1 dirty path at writing (no hand-back yet) | root `ebc918c3` merged; B2-11 committed (`d8f80c7a` "verify checks only exes.eval key + pin; the judge runs and checks ..."); D2R-2 (b), builds, suites, report: read `handoff-B2.md` | finish from `handoff-B2.md`; review (r7-review-dispatch; the re-reviewer checks every conflict region of both merges, B2-14); root merges first of all (C3b item 6, SQL3 item 5, OD3P T2 wait on it) |
| D3 (guards, kinds) | pool-20 `feat/p9-d2-20261003` | `d080af09` (merge of root, "before the suites"), 1 dirty path (no hand-back yet) | guards + kind wiring committed and tested (`e531257b` "C3-3 alias guard, E2-17 inputs guard, walk-forward kind: tested"); `task-D3-report.md` and `handoff-D3.md` written; the suites after the last root merge may not have finished | finish from `handoff-D3.md`; review; FIX ROUND must include C3-13 (set `nav.rule` for a `--rule` row); root merges; CRITICAL PATH (PRE-35) |
| D3W (walk-forward C++) | pool-23 `feat/p9-d3w-20261005` (base `899b5811`) | `2209821e` `WIP(d3w): fitter walk-forward flags and check_walk_forward (run_walk_forward / weights_v3 not written ...)`, clean (no hand-back yet) | engine fit `fit_walk_forward` + fit verb `--walk-forward` committed (`200cbf74`); runner reads v3 (`994210f4`); fitter post-step partly (WIP); ALCOMB-7 / -6, D2-9, D2R-3 not started; builds / tests: read `handoff-D3W.md` | finish from `handoff-D3W.md` (D3-1, D3-2); review; merge after D3; then part B and the OD3P-1 absent-members C++ (see rulings) |
| AL-CLOCK | pool-8 `feat/p9-alclock-20261004` | `120f6a25` `WIP(al-clock): ...` on merge `8a6ebe5b` (root `4dc831ac`), clean (hand-back received) | 16 C++ files (+352 / -32) UNCOMPILED: the row, `--rate` parser entry, `two_speed_fast_theta(cadence)`, refusals, per-book stats, `--info-clock-print` opt-in; one existing refusal loosened so `--capacity-curve` accepts the rate (open question, ruled ALCLOCK-3 below). NOT done: C++ tests + CMake line, `rule_flags` pytest, registration and template (text in the handoff), builds, teeth, canary, report. `build-equity` configured (equity-dev), nothing built; next tag `p9-alclock-a` | finish from `handoff-AL-CLOCK.md`; merge root `1b64ce45`+; review; merge BEFORE COV-MV; root repeats the C3-5 probe with `--rate info-clock-rate-v1` |
| COV-MV | pool-24 `feat/p9-cov-20261003` | `190eebe2` (docs; merge of root `a15ad887` = `338e0e07`), clean (hand-back received) | NO code; complete design for items 1-8 by file:line in `handoff-COV-MV.md` section "Session R7" (kernel, hook, refusals, summary, workspace term, tests, teeth). Two out-of-list edits asked (ruled COVMV-1 below). Next tag `p9-covmv-a`; first build needs a fresh configure | finish from the handoff; review (traces COV-4); merges after AL-CLOCK |
| C3b | pool-15 `feat/p9-c3-20261004` | `e28d7849` (code `278fc2d8`; root `ebc918c3` merged), clean (hand-back received) | items 1-5 DONE (Python `pm6-6` route retired; C2-9; C2-10 in `wave_result.build`; E2-16a + PRE-27 tests; `read_grid` duplicate key, exit 1 like the other grid refusals, + S-12); item 6 pending on B2; item 7 dropped (C3-11); optional S-13 / C3-10 / E2-11 not done. Build `p9-c3-n` Debug (tag `p9-c3-m` refused at the argument check, nothing built); named gtests 34/34, whole target-tests 391/391, canary 14 / 1 failed on the 15 s first-run limit (digests matched; host load; see C3-14), Python teeth 6/6, `scripts/tests` seed 0 522 / 4 skipped. NOT run: Release build, C++ teeth, seed 1, stage chain, the last root merge. Cross-lane edits in D3's files: line 18 of eight kind fixtures + three hunks of `test_wave_kinds.py` (listed in the handoff). Debug tree 2.7 GB kept | finish from `handoff-C3.md` "C3b at the second owner stop" (Release, teeth, seed 1, stage chain, root merge; item 6 iff B2 in root); review; merge before any cell (E2-16). Root: its evidence tools `m2e-logs/tools/c2_gm_pair.py:156-159`, `m2h-logs/m2h2-engine-o1.py:125` still call the deleted Python functions (root slip at merge) |
| SQL3 | pool-13 `feat/p9-sql3-20261004` | `784d2abd` `fix(sql3): seal walk fails closed on a file-system error (F-2)` on `d10d9e54` `feat(sql3): record-store writer hook, receipt store block, seal narrowing, indexe...`, 1 dirty path (no hand-back yet) | items 1-4 implemented and committed; `task-SQL3-report.md` written; the suites after the root merge `1b64ce45` (`9e44d3be`) may not have finished; item 5 pending on B2 | finish from `handoff-SQL3.md`; review; merge (root adds `atx.run-store/v1` to `classes.json`, SQL3-1) |
| OD3P | pool-21 `feat/p9-od3p-20261004` | `e9bedcc7` (merge of root `ebc918c3`), 1 dirty path (no hand-back yet) | OD3P-1 implemented (`9be598cf` "OD3P-1 absent members via the IC exe; ALCLOCK-2"); runbook committed (`05e2a373`); `task-OD3P-report.md` written before the merge; item 7 suites after the merge may not have finished; T2 pending on B2 | finish from `handoff-OD3P.md`; review; merge before the OD-3 read; hand `od3p-absent-contract.md` to the C++ lane (OD3P-1) |
| A4 (stretch) | pool-27 `feat/p9-a4-20261005` (base `1b64ce45`) | `ac4f3df0` (handoff only), clean (hand-back received) | read phase only: no code, T2 `dfa4218d` NOT merged, no design fixed | A4-1: resume from `handoff-A4.md` when a slot is free; merges only if APPROVE at the gate, else P10 whole |
| SIGN (stretch) | no pool | not dispatched | C3-11: the `sign_pm7_35` C++ verb route | when C3b is APPROVE and a slot is free |
| SQL4 (3b) | no pool | not dispatched | after SQL3, S2, B2, C3, D3 are in root; else P10 untouched (SQL-12) |
| T2 | pool-22 `feat/p9-t2-20261003`, `dfa4218d` | APPROVE | merge after B2's redo, as a unit |
| PRE | sprint dir `p9-prereg.md` | round 2a | round 2b after the wave-3 P9-B0 and the lane reports; review; the PM rules the file |

Root at the stop: code head `ebc918c3`, docs head `1b64ce45`; nothing running; no merge open; pool-2 has the PM's
uncommitted `progress.md`, `status-8.md`, `next-goal-prompt.md`, `r7-review-dispatch.md`, `r7-root-merge.md` and the
A4 section appended to `r7-dispatch.md` (the first root agent of the next session commits them in its housekeeping
docs commit; never the PNGs under `docs/plans/`).

## Open with the owner (surfaced; none blocks engineering)

1. **Permission refusals this session:** none reported by any agent that handed back (root, COV-MV, AL-CLOCK, A4,
   C3b). The five pending hand-backs may add some: read each handoff's "permission" line.
2. **Worktrees.** Pools 23 (D3W) and 27 (A4) were created by the lease script (the pool grew to 27 slots); each
   lease printed "configure failed; lease remains held" (the broken `dev` preset, OD5-3). Leases of pools 12, 14, 17,
   18, 19, 20 show `owner=dead` from older runs; lanes B2 (14) and D3 (20) work in them regardless; no stale recovery
   was done.
3. **`C:\atx\data` clean-up:** NOT in this session's goal text; nothing deleted.
4. **Rulings the owner may want to look at:** C3-11 (the Python `sign_pm7_35` stays through P9 as a named exception
   unless stretch lane SIGN lands), OD3P-1 (what "absent member" means on history: the engine's missing-member rule,
   mass stays in the theme; needs a small IC-exe flag), GATE-1 (one gate for waves 2 and 3 instead of two), D3-2
   (D3 split into two lanes), A4-1 (A4 dispatched as a stretch lane on a new pool), COV-14 (D-COV-EIG: no exclusion;
   P9-V's prediction is dominated by two eigen ranks, printed as such).
5. **Owner items for the final report** (unchanged): local `main` / `origin/main` (OD5-3), the deterministic `pow` /
   `exp2` re-pin (OD5-2), data purchase (OD5-5), the atx-db request file (AL-DATA), the orphaned scratch dir
   (SCRATCH-2), the broken `dev` preset.

## Open questions, NOT ruled (the next PM rules these)

- Whatever the five pending hand-backs list as open (read each handoff before dispatching).
- C3b: the canary's 15 s first-run limit (`ATX_E2E_MAX_SECONDS`) failed once on a loaded host with matching digests.
  Recommendation (ruled C3-14 below as a reading rule, not a code change): a canary whose digests match and only the
  wall limit trips is re-run once on a quieter host, like ENV-1; the limit is not edited.
- D3W: how the absent-members IC flag (OD3P-1) is scheduled: as D3W's second round with part B, or a parallel lane on
  D3W's approved head. Recommendation: parallel lane once D3W is APPROVE (the IC runner hunks are small and disjoint
  from part B's fit files); it must be merged before the gate (EXE-FREEZE).
- Root M2j item 8 question 2 is ruled (C3-13: yes, both probes at D3's merge).

## Gates and rules the next PM must hold (new this session; status-5, -6, -7 list the earlier ones)

- GATE-1: ONE gate for waves 2 + 3 on the head with every merge; one P9-B0, one re-pin; EXE-FREEZE from there.
- PRE-35 as read by D3-2: cells wait for lane D3 (guards) merged and APPROVE; P9-W lapses iff D3W's walk-forward or
  D3's kind wiring are not merged and APPROVE then.
- C3-13: D3's fix round sets `nav.rule` for a `--rule` row; root repeats the C3-5 probe (a) and (b) at D3's merge and
  with `--rate` at AL-CLOCK's merge.
- W3-8a: the kind files are D3's; root does not edit `SCHEMA_ONLY_FIXTURES`.
- OD3P-1: the clean-source line needs the IC exe's `absent-members-v1` capability merged before the gate, else it is
  printed "not read: no producer".
- ALCLOCK-1 / ALCLOCK-2, COV-14, PRE-40, C3-11, C3-12, SQL3-1, D3-1, A4-1 (full text in the ledger).
- Everything of status-7: ENV-1, PERM-1, EXE-CUR, SEAL-ALLOW-3, COVR-1, CRT-2, COV-13, W3-EARLY-2 / HOST-2, PRE-14,
  B2-14, C3-5 / C3-6, ROOT-1 (known-red set EMPTY), ledger appends by heredoc or scratch file, never edit earlier lines.

## Next steps, in order

1. Verify the stop state: `git log` / `git status` in pools 2, 8, 13, 14, 15, 20, 21, 23, 24, 27; read each handoff;
   trial ledger (133 lines, `27e40f9f`); `lease-worktree.ps1 -Status` (never stale recovery). A lane whose tree is
   dirty at the stop (B2, D3, SQL3, OD3P had one dirty path at writing: most likely its own handoff being written)
   is resumed by a fresh agent that first commits that file as the lane's handoff commit.
2. Root (one fresh agent): housekeeping docs commit of the sprint-dir files; then merges as lanes become APPROVE,
   per `r7-root-merge.md`.
3. Resume the lanes in parallel, each with a fresh agent from its handoff and its `r7-dispatch.md` section: B2 first
   in priority (three lanes wait on its merge), D3 (critical path; its review + fix round with C3-13), D3W, C3b,
   AL-CLOCK, COV-MV, SQL3, OD3P; A4 when a slot is free. Each gets a review (`r7-review-dispatch.md`); fix rounds
   until APPROVE; root merges in this order as they are ready: B2 redo, T2, C3b, D3, AL-CLOCK, COV-MV, D3W, SQL3,
   OD3P; then the absent-members lane, D3W part B, SQL4, SIGN, A4 if they make it.
4. The single gate (GATE-1): suites Debug + Release (0 failed, named skips only), golden at 1 and 4 workers,
   canaries, P9-B0 (substitution list first; R-2, Y-F0, Y-1 re-based on the Release v1.1 store, PRE-29;
   `--ann-mean-gross` on every reference, `--info-clock-print` on Y-F0 iff AL-CLOCK is in), the re-pin incl.
   `exes.eval`, the store step (M2A-4), scoreboard, G-P ticks, CRT record, rho of the P9 pairs, whole-wave review
   split by area. EXE-FREEZE from the re-pin.
5. PRE round 2b (33 placeholders; PRE-32 .. PRE-40 and COV-14 into the text; G-B7 step values from the v8 ledger's
   public lines), a review by a fresh agent, the PM rules `p9-prereg.md`.
6. Cells in order: P9-L, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X, P9-V (lapses per PRE-14 / PRE-35; P9-X per PRE-30 /
   PRE-34); adoption print; OD-3 once on P9-F0h (four history lines) after OD3P is merged; freeze gate; the final list
   of every ruling.

## Rulings made this session (full text in the ledger from "- RESUME 7")

Root / process: M2J-1, M2J-2, M2J-3, W3-8a, GATE-1, A4-1, POOL-MAP (R7-1, R7-2).
COV: COV-14. C3: C3-11, C3-12, C3-13, C3-14 (below), PRE-40. OD3P: OD3P-1. AL-CLOCK: ALCLOCK-1, ALCLOCK-2, ALCLOCK-3
(below). COV-MV: COVMV-1 (below). D3: D3-1, D3-2. SQL3: SQL3-1.
Owner's words recorded: STOP 7 (and "dont wait just write master handoff").

Rulings made while writing this file (also appended to the ledger):
- ALCLOCK-3: `--capacity-curve` is accepted with `info-clock-rate-v1` (the rate reads no NAV; P9-C's parent runs the
  flag); the registration states it; the reviewer checks the loosened refusal touches no other rate.
- COVMV-1: the two out-of-list edits are allowed and listed as cross-lane: a new impl file `strategy_mv_aim.{hpp,cpp}`
  with its line at the END of `atx-impl/CMakeLists.txt` (P2 tail append), and one added term in
  `nav_workspace_reserve_bytes` (`book/nav_lockstep.cpp`) so the rule's scratch counts in `--max-bytes`; flag absent
  = the reserve is unchanged (a test).
- C3-14: a tiny-world canary whose golden digests match and that fails only the first-run wall limit on a loaded
  host is re-run once on a quieter host (as ENV-1); the limit is never edited; two misses with matching digests are
  reported to the PM as a timing finding, not a red.

## Standing warnings (in addition to status-5, -6, -7)

- Nine agents at once ran here with 85-90 GB free and 4 GB free RAM; builds queue at the memory gate; C++ lanes that
  stop uncompiled (AL-CLOCK) need a build before anything else.
- Two lanes (C3b, D3) edit `scripts/tests/test_wave_kinds.py` and the kind fixtures: expect a keep-both conflict at
  the second merge; C3b listed its hunks.
- A hand-back that says "tests actually run" before the last root merge is not a green tree: the resuming agent
  re-runs the suites first.
