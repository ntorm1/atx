# P9 sprint: PM handoff 6 (owner stop, 2026-10-04)

For the next parent (PM) agent. Supersedes `status-5.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir).

## Read order

1. This file.
2. `resume4-dispatch.md`: the standing contract every agent gets (hard rules, final-message format). Reuse it.
3. The ledger `progress.md` from the line "RESUME 5" to the end. Every "Ruling" line is binding (so are the older
   ones; the RESUME 4 block is summarised in `status-5.md`). This session's rulings are listed at the end of this file.
4. `handoff-root-m2h.md` (root, stopped inside the E2 merge's verification) and `handoff-root-m2g.md` (last finished
   root group), then `root-wave2-gate-report.md` (the gate diagnostics already run).
5. `p9-prereg.md` (PRE's round-1 draft). Its section H ("Open for the PM") is answered by rulings PRE-1 .. PRE-27 and
   PRE-7a in the ledger; the draft text does not carry them yet (round 2 writes them in).
6. `wave3-carry.md` ("PM answers" and "Added after drafting" win), as amended by this session's rulings (table below).
7. Lane handoffs, only when you dispatch that lane: pool-15 `handoff-C3.md`, pool-24 `handoff-COVR.md` and
   `task-COVR-report.md`, pool-20 `task-D2R-report.md`, pool-17 `task-E2-rereview-4.md` ("What root must check").

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`, unchanged
(0 trials this session; checked by the PM at the stop).

## State in short

Wave 2: 11 of 12 lanes are in root (SQL1, SQL2, A3, AL-SIG, S2, D2, COV, C2, AL-COMB, S2R as S2's follow-up, B2's
identity part) and E2's merge commit is made (`430c29e8`) with its verification unfinished. Still to merge for wave 2:
D2R (a 2-test red fix, done, unreviewed), B2's post-identity redo (not started), T2 (APPROVE, last). The wave-2 gate
proper has not run; five of its zero-trial diagnostics have (GATE-EARLY). Wave 3: C3 part A is in fix round 1; nothing
else is dispatched. PRE round 1 is drafted and its 26 open items are ruled. No P9 cell has run. The owner delegated
every open decision to the PM during this session (ledger line OD-DELEG); the five former owner items are ruled
(OD5-1 .. OD5-5).

## Done this session

- Owner items ruled under delegation: identity-bridge exposure accepted with a flag on every print (OD5-1), Release
  `pow` skip kept (OD5-2), `main` untouched (OD5-3), no new-signal cell (OD5-4), no data purchase (OD5-5), G-B2 hurdle
  8.0% labelled as the plan's placeholder (OD5-6), L <= 2.0 (OD5-7).
- Root M2e: C2 and AL-COMB merged and re-proved; Release `BookNormalScore` red cleared; X-5 gm-pair identity holds
  (L-prime 1.1720 exact). Root M2f: S2R and B2's identity part merged; the Release lit-golden red is now a named skip;
  TRAIN identity of the C++ `eval` verb vs the Python statistics holds (largest difference 5e-14, 0 boundary lines).
- Root M2g (gate diagnostics, GATE-EARLY): AL-DATA amendments 5.1 / 5.2 applied; ALD-DIAG printed; leverage-pair rho
  measured; Q5 run (q .0502, q_lit .0671); S2R-DIAG 0 cells; COV-7 identity MISS found (D-COV not run).
- Lanes: E2 fix rounds 3 and 4 and re-reviews 3 and 4 (APPROVE); S2R finished and reviewed (APPROVE); AL-DATA rounds
  2 and 3, re-review 2 and a delta check (APPROVE, complete); C3 part A built and reviewed (CHANGES_REQUIRED, fix
  round 1 running at the stop); D2R (fix for a red that entered root with D2's merge); COVR (the COV-7 miss
  attributed to the C runtime's `exp2`).
- PRE round 1: `p9-prereg.md` (755 lines, 29 round-2 placeholders); the OD-3 tooling gap scoped (OD3-TOOL).

## Lanes at the stop

| lane | pool / head | state | next |
|---|---|---|---|
| SQL1, SQL2, A3, AL-SIG, S2, D2, COV, C2, AL-COMB, S2R, B2 identity | merged | complete | gate proper |
| AL-DATA (doc-only) | `al-data/` | complete (delta check APPROVE) | `al-data/atx-db-request.md` goes to the owner with the freeze report (OD5-1 (iv)); 5.3 is a P10 note |
| E2 | pool-17 `dd857198` (code `80c322ef`) | APPROVE; merge commit `430c29e8` in root, slip `1dd0ac46`, SQL2-CLS `59893e9f`; verification NOT finished | finish M2h from `handoff-root-m2h.md`: ROOT-1 suites, live-table check, the engine match stage on real exes (tiny-world), O1 values (E2-16), W1-I1 margin |
| D2R | pool-20 `283043ce` | done, NOT reviewed | light review; root merge (clears the two e27b reds); root re-runs the two exe-gated tests |
| COVR | pool-24 `494ec9b2` | done, NOT reviewed | light review; root merge; root shows Release = Release on TRAIN (two runs, 1 vs N workers); then D-COV and COV steps 4, 5 in Release; record the `atx-cov-v1` store pins (COV-12, W3-10) |
| C3 part A | pool-15, fix round 1 commit `9a7c490f` (review `99b676f7`: 2 Required, 8 Suggested) | fix round 1 stopped by the owner: read `handoff-C3.md` for what is built and tested | finish the round (R-1, R-2, C3-3, PRE-7 / PRE-7a), delta re-review, root merge (X-5 / Y-F0 `--spec` vs argv identity, Release = Debug NAV, wall time, `atx.nav-spec/v1`, W3-8 fixture flip) |
| C3 part B ("C3b") | pool-15 | not started | after E2's merge is verified in root: C2-11 (retire the Python `pm6-6` mode), C2-9, C2-10, W3-6 list, E2-16; own review and merge before the wave-3 gate |
| B2 post-identity | pool-14 `26fe8d50` | CHANGES_REQUIRED; redo not started | after E2 is verified in root (B2-12): lane merges root, places its hunks on E2's split files, B2-11, PRE-15 (DSR_hand verb row), D2R-2 (b) (`nav_summ` `allow_abbrev=False`); re-review; merge |
| T2 | pool-22 `2e9d6813` | APPROVE | merge LAST, as a unit; A4 is dispatched at that merge or moves to P10 (W3-13) |
| PRE | sprint dir `p9-prereg.md` | round 1 drafted; H1-H26 ruled | round 2 after AL-CLOCK, COV-MV, D-COV and the wave-3 P9-B0; then a review; then the PM rules the file |
| OD3P (new, PRE-21) | a free pool | not dispatched | after E2 and B2's redo are in root; port of the OD-3 tooling from branch `feat/platform-v8-xpre-20261002`; needed before the OD-3 read, not before the cells |

Root at the stop: see `handoff-root-m2h.md` for the exact heads and what was run. From git at the stop: code head
`59893e9f` (E2 merge `430c29e8` + slip + SQL2-CLS). Next free build tag: `p9-2m` unless that handoff says otherwise.
Known-red set: the two e27b tests of `scripts/tests/test_research_spec.py` (cleared by D2R's merge); the E2 pytest
`test_fields_are_the_rule_applied_to_the_registry` should be green after E2's merge (confirm in the handoff); Release
lit-golden is a named skip (M2c-RED-2).

## Open with the owner (surfaced; none of these blocks engineering)

1. **Worktree removal was refused by the permission system.** The owner asked for unused worktrees to be deleted.
   Pools 8, 13, 21, 23 were checked safe and their leases released, then `git worktree remove` was denied; not retried.
   They exist as free trees (about 1 GB). The owner can run
   `git -C C:/atx-wt/pool-2 worktree remove --force C:/atx-wt/pool-<N>` for N = 8, 13, 21, 23. Kept on purpose:
   pool-7 (only copy of the OD-3 tooling branch's work tree; the branch itself is in git), pool-10 (77 uncommitted
   entries), pool-16 (foreign work), pool-25 (main-merge verification), pool-26 (another live session).
2. **`C:\atx\data` clean-up was asked for and NOT done.** Surveyed read-only only; nothing deleted. 14 GB, 187 entries.
   What is known is in the ledger line "OWNER INSTRUCTION ... C:\atx\data". Before deleting anything the next PM must
   finish the classification (do the engine / impl tests and benches read the `tickerhistory_training*` trees at run
   time; are the OD-3 era roles built from them; does a running atx-db job read `C:/atx/data`), delete only entries
   shown unused and regenerable or superseded (the `equity_scorecard16..21_*` and other September experiment outputs
   are the likely set, about 2.5 GB), and name vendor / source / history-block data to the owner instead of deleting
   it. One entry is dated 2026 (`tickerhistory_20260501_20260615_20260919`): never open it.
3. **Rulings the owner may want to look at before the cells run** (all made under OD-DELEG): OD5-1 (the single-use
   history block is read on a universe partly chosen with 2026 knowledge; cannot be re-read), PRE-1 (P9-X is the Y-1
   form, so the levered book gets no history read), PRE-10 (OD-3 reads the last accepted book that does not read the
   risk model).
4. **Owner items for the final report:** local `main` / `origin/main` (pushed merge, 6 factory reds, broken `dev`
   preset; OD5-3), the deterministic `pow` / `exp2` re-pin (OD5-2; COVR found the same cause in the risk estimator),
   data purchase (OD5-5), the atx-db request file (AL-DATA).
5. **Permission refusals this session:** `git restore --source=<sha> --worktree` (pool-17, E2 lane; not retried);
   `git worktree remove` (PM; not retried).

## Open questions, NOT ruled (the next PM rules these)

- None blocking. PRE-24: before P9-MV is registered in round 2 the PM reads the public spo-v1 / spo-v2 v7 cell
  outcomes (cov-design 5.2).
- COV step 4 uses the kept container `build-equity/p9-m2d-cov-risk-container`, which a Debug exe wrote; if its NAV
  identity against the P9-B0 reference misses by the `exp2` ULP when read by Release exes, apply COV-12's reading and
  rule it then.
- Whatever `handoff-root-m2h.md` and `handoff-C3.md` list as open.

## Gates and rules the next PM must hold (new this session; `status-5.md` lists the earlier ones, all still in force)

- OD5-1: every cell print, the adoption print, the OD-3 print and the freeze report carry the identity-bridge flag
  (text per ALD-9); a freeze pass is worded "passed with the identity-bridge flag".
- ROOT-1: at every merge root runs the whole `scripts/tests` (one seed at least) and `test_stage_chain.py` separately.
- COV-12: Release is the producer of record of the COV store; Release = Release replaces Debug = Release.
- PRE-7a: `ann_mean_gross` is opt-in; no existing pinned byte moves; every P9 cell and its reference name it alike.
- PRE-14: P9-C lapses if AL-CLOCK, P9-W if D3, P9-MV if COV-MV is not merged and APPROVE when the cells start.
- PRE-2 / PRE-5 / PRE-11 / PRE-27: P9-L, P9-X, P9-V run by hand as v1 cycles, spec and verdict arithmetic committed
  first, verdict re-derived by a second agent, parent of record = the plain recipe at the published L-prime.
- E2-15 / E2-16 / E2-17 / C3-3: guards on the reference side, `ref.leverage`, `inputs.*` and flag aliases must be in
  root (C3b, D3) before any cell runs.
- RUN-2: bounded runs state `--seconds` <= 600. RUN-1: `--max-rss-mib 2048`.
- OD3-TOOL / PRE-21: `atx-impl/tools/holdout_gate.py` is the 2024+ reader and is NEVER run in P9; the OD-3 read waits
  for OD3P.
- Pools: 8, 13, 21, 23 are free trees; the lease script takes the lowest-numbered free tree first, so the next leases
  land there whatever W3-11 says (POOL-MAP: record the real pool). Never `git worktree prune`.
- Ledger appends: a shell heredoc failed once on a long block; write the block to a scratch file and append it.

## Next steps, in order

1. Root (one fresh agent per step group): finish M2h from `handoff-root-m2h.md`. Then, as reviews allow: merge D2R,
   merge COVR and run D-COV (Release), merge C3 part A.
2. Reviews to dispatch now: D2R (light), COVR (light). Lanes to dispatch now: C3 fix round 1 finish (pool-15), then
   its delta re-review. Once E2 is verified in root: B2 post-identity redo (pool-14), C3b (after part A is merged),
   D3 (pool-20, after D2R is merged), OD3P (after B2's redo). Once C3 part A is merged: AL-CLOCK (pool-19), then
   COV-MV (pool-24, after COVR is merged). SQL3 after B2's redo; SQL4 after SQL3 / C3 / D3; A4 at T2's merge.
3. T2 merge last; then the wave-2 gate proper: suites Debug + Release (0 failed, named skips only), golden at 1 and 4
   workers, canaries, P9-B0 (substitution list first), W1-I2 (b) re-pin incl. `exes.eval`, the store step (M2A-4),
   scoreboard, G-P ticks, whole-wave-2 review.
4. Wave-3 merges, wave-3 gate, wave-3 P9-B0 and re-pin (R-2 and Y-F0 re-based, PRE-22). EXE-FREEZE from there.
5. PRE round 2 (fills the 29 placeholders and writes rulings PRE-1 .. PRE-27 into the text), prereg review, PM rules
   `p9-prereg.md`.
6. Cells in order: P9-L, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X, P9-V (lapses per PRE-14); adoption print; OD-3 once
   (four books, PRE-16 .. PRE-20) after OD3P is merged; freeze gate; the final list of every ruling (the ledger has
   195 "Ruling" lines at this stop).

## Wave-3 dispatch table (replaces status-5's; carries added this session in the last column)

| lane | pool (W3-11; real pool recorded at lease) | start when verified in root | added carries |
|---|---|---|---|
| C3 fix round 1, then C3b | 15 | now; C3b after E2 | PRE-7 / PRE-7a, C3-3 row field, C2-11, C2-9, C2-10, E2-16, D2R-2 (a) text |
| D3 | 20 | D2R, E2, AL-COMB | C3-3 (alias guard), E2-17 (`inputs.*` guard), E2 S1 route message (optional); NOT E2-9 (D2R did it); the positive live-table check as a test if root did not add it |
| B2 redo | 14 | E2 | B2-11, PRE-15, D2R-2 (b) |
| OD3P | free | E2, B2 redo | PRE-20, PRE-21 |
| SQL3 | 17 | E2, B2 redo | - |
| AL-CLOCK (3b) | 19 | C3 part A | - |
| COV-MV (3b) | 24 | COV, COVR, C3 part A (after AL-CLOCK's merge it merges root) | PRE-9, COV-11 |
| SQL4 (3b) | 18 | SQL3, S2, B2, C3, D3 | - |
| A4 (stretch) | 12 | T2 | - |

## Rulings made this session (full text in the ledger from "RESUME 5")

Owner delegation: OD-DELEG (the owner's words), OD5-1 .. OD5-7.
Process: LEDGER-1 (root commits the ledger), ROOT-1 (full `scripts/tests` at every merge), GATE-EARLY (gate
diagnostics run early), RUN-2 (`--seconds` <= 600), DISK-3 (which worktrees go; outcome: removal refused).
S2R: S2R-1, S2R-2 (ResearchFast flat-window property; fix is P10), S2R-3 (no fix round).
AL-DATA: ALD-8 .. ALD-12.
E2: E2-13 .. E2-18. C2: C2-11. B2: B2-13. D2R: D2R-1, D2R-2.
COV / AL-COMB: COV-12 (the COV-7 miss), ALCOMB-10 (Q5 on Y-F0).
C3: C3-1 .. C3-4. Wave 3: W3-14a (PRE round 1 early).
Prereg: PRE-1 .. PRE-27, PRE-7a (the PM saw Q5's print before ruling PRE-6 and adopted the blind lane's threshold
unchanged; the ledger says so).
Finding: OD3-TOOL (the OD-3 tooling is on an unmerged branch; the plan names the wrong tool).

## Standing warnings (in addition to status-5's)

- A subagent from this session cannot be resumed by a new session: dispatch fresh agents from the handoff files.
- Lease `-Status` shows live lanes under dead owners: never run stale-lease recovery. Release only with the record's
  own run id.
- The PRE lane saw one root commit body through `git log` (it says it used none of it): tell blind agents not to read
  root's log beyond SHAs.
- Disk fell from 48 GB to about 35 GB free during builds and came back as lanes deleted their trees; watch it.

## Uncommitted at the stop (pool-2)

`progress.md` (PM's ledger), this file, `next-goal-prompt.md`, and whatever `handoff-root-m2h.md` lists. The first
root agent of the next session commits the sprint-dir files in its housekeeping docs commit (never the PNGs under
`docs/plans/`, never a cache directory).
