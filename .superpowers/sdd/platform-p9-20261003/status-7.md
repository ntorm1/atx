# P9 sprint: PM handoff 7 (owner stop, 2026-10-04)

For the next parent (PM) agent. Supersedes `status-6.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir).

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, then `resume6-addendum.md` (wins where they differ; ENV-1,
   PERM-1), then, for wave-3 lanes, `r6-wave3-common.md` (how lanes run in parallel now). Reuse all three.
3. The ledger `progress.md` from the line "RESUME 6" to the end. Every "Ruling" line is binding (so are the older
   ones; the RESUME 5 block is summarised in `status-6.md`). This session's rulings are listed at the end of this file.
4. `handoff-root-m2j.md` (root, stopped inside the verification of C3 part A's merge) and `handoff-root-m2i.md` (last
   finished root group: exes, store pins, empty known-red set), then `root-wave2-gate-report.md` item 4 (d) (D-COV).
5. `p9-prereg.md` (now "ROUND 2a - not ruled": all rulings to PRE-31 are in the text; Appendix IV indexes the 33
   `[ROUND 2b: ...]` placeholders by who delivers them; its last section lists 8 contradictions, ruled by PRE-32 ..
   PRE-39, which the text does NOT carry yet) and `p9-prereg-flags.md` (the flag list, ruled by PRE-31).
6. `wave3-carry.md` ("PM answers" and "Added after drafting" win), as amended by the rulings of RESUME 5 and 6.
7. Lane handoffs, when you dispatch that lane (table below).

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`, unchanged
(0 trials this session; checked by the PM at the stop).

## State in short

Wave 2: every lane but two is merged and verified in root (SQL1, SQL2, A3, AL-SIG, S2, D2, COV, C2, AL-COMB, S2R,
B2's identity part, E2, D2R, COVR). Root's known-red set is EMPTY since M2i (Release lit-golden is a named skip).
Still to merge for wave 2: B2's post-identity redo (in progress, stopped) and T2 (APPROVE, merges last). The wave-2
gate proper has not run; its zero-trial diagnostics have, now including D-COV in Release with both covariance stores
pinned. Wave 3: C3 part A is APPROVE and its merge commit is in root (`327c86b2`) with the re-proof unfinished; six
lanes (D3, AL-CLOCK, COV-MV, C3b, SQL3, OD3P) were dispatched in parallel under W3-EARLY-2 and stopped by the owner
early in their work. The prereg is at round 2a. No P9 cell has run. 313 "Ruling" lines in the ledger at the stop.

## Done this session

- Root M2h finished (the missing handoff rebuilt from git and logs): E2 merged and verified; the engine match stage
  shown on real exes (tiny-world); O1 measured (the engine route keeps the reference's leverage).
- Root M2i: D2R and COVR merged and re-proved; ROOT-1 512 pass / 4 skip / 0 fail; Release = Release holds on TRAIN for
  both recipes; D-COV run in Release (tables in the gate report); COV step 4 holds; store pins recorded:
  `build-equity/p9-m2i-dcov-v11` (manifest `39736647...23c7`; P9-X and the re-based Y-1) and
  `build-equity/p9-m2i-dcov-covv1` (manifest `ea58691f...f0f3`; P9-MV, P9-V). Both are kept, never rebuilt (CRT-2).
- Reviews: D2R (APPROVE), COVR (APPROVE), C3 part A delta re-review (APPROVE, after fix round 1 was finished).
- Doc lanes: PRE-FLAGS (`p9-prereg-flags.md`: 55 clean, 10 flagged, 2 flagged-unverified; drop list of 12) and PRE
  round 2a (`p9-prereg.md`).
- PM: the public spo-v1 / spo-v2 v7 outcomes read (PRE-24a); 35 rulings.

## Lanes at the stop

"Stopped" = the agent was told to stop at a clean point, commit (unfinished work as a `WIP(<lane>):` commit), and
write its handoff. The rows below are filled from the hand-backs; where a hand-back had not arrived when this file was
written the row says so, and the handoff file in the pool is the truth. Verify every head with `git log` before you
dispatch. A subagent of this session cannot be resumed by a new session: dispatch fresh agents from the handoffs.

| lane | pool / branch | state at the stop | next |
|---|---|---|---|
| SQL1, SQL2, A3, AL-SIG, S2, D2, COV, C2, AL-COMB, S2R, B2 identity, E2, D2R, COVR | merged | complete | gate proper |
| AL-DATA (doc-only) | `al-data/` | complete | `al-data/atx-db-request.md` goes to the owner with the freeze report |
| C3 part A | pool-15 `feat/p9-c3-20261004`, APPROVE head `0dbfa459` | merge commit `327c86b2` in root; re-proof NOT finished | finish M2j from `handoff-root-m2j.md` |
| C3b (part B) | pool-15, same branch, head `081e2090` (root `5c57c1ef` merged) | stopped: NO code yet; file:line designs for items 1-5 in the handoff; item 7 (W3-6) has a design gap (see "Open questions") | finish from `handoff-C3.md` ("C3b at the owner stop"); review; merge. Items in B2's files go last, after B2 is in root |
| B2 post-identity redo | pool-14 `feat/p9-b2-20261003`, head `14b881c9` | stopped: root `5c57c1ef` MERGED (`c37ec671`; every conflict region resolved by an ordinary edit, table in the handoff; B2-R3's hunks moved onto `cycle/cli.py`, `cycle/spec.py`, `cycle/phases.py`); PRE-15 done; NOT done: B2-11 with R1 / R2, the re-test of the Python-statistics deletion, D2R-2 (b), any build after the merge. One test run on the merged tree: 99 pass / 3 skip / 2 fail (both need the built eval verb): suites NOT known green | finish from `handoff-B2.md` under B2-14 (conflict regions by ordinary edits, no wholesale checkout); scoped re-review; root merge with TRAIN identity. Unblocks the last items of C3b, SQL3, OD3P |
| D3 part A | pool-20 `feat/p9-d2-20261003` (D2's branch), head `f6726dd6` (root `9c7aa654` merged) | stopped: WIP commit `0389e868` UNTESTED (both guards and the walk-forward kind wiring; `test_wave_kinds.py` not updated: reds expected on this head); walk-forward C++, live-table test, ALCOMB-7 / -6, D2-9, D2R-3 not started | finish part A from `handoff-D3.md` (guards + walk-forward first, PRE-35); review; merge. CRITICAL PATH: no cell starts before D3's two guards are merged and APPROVE |
| D3 part B | pool-20 | not started | after part A: C++ `ew-theme-aim-v2` fit, resid diagnostics, the D2-3 Python deletion in its own commits; gates no cell |
| AL-CLOCK | pool-8 `feat/p9-alclock-20261004`, head `ebaf1e3f` (root `5c57c1ef` merged) | stopped: NO code yet; design by file:line in the handoff; `source_sample_end` 2019 (Bowles et al.), so the rule is FLAGGED (PRE-31) | finish from `handoff-AL-CLOCK.md`; review; merge BEFORE COV-MV (W3-9); root repeats the C3-5 probe with `info-clock-rate-v1` |
| COV-MV | pool-24 `feat/p9-cov-20261003` (COV's branch), head `b8f714c9` (root `5c57c1ef` and C3 `0dbfa459` merged) | stopped: NO code yet; design by file:line in the handoff (`FactorModel::apply_inverse` inverts F, so the rule needs its own singular-safe form, COV-13 (c)) | finish from `handoff-COV-MV.md`; review (traces COV-4: store row d only); merges after AL-CLOCK |
| SQL3 | pool-13 `feat/p9-sql3-20261004`, head `2e0b79f6` (base `5c57c1ef`) | stopped: NO code yet; writers by symbol, `store` block design, W3-4 / W3-5 designs and test plan in the handoff | finish from `handoff-SQL3.md`; the verdict / binding hooks go last, after B2 is in root; review; merge |
| OD3P | pool-21 `feat/p9-od3p-20261004`, head `80484f0b` (root `9c7aa654` merged) | stopped: items 1, 2, 4, 5 done (carry tool, T1 / T4, `make_cycle` refusal, 12-id drop list, `--risk-model` refusal, record documents; 29 + 24 tests passed BEFORE the root merge, not re-run); T2, the runbook and the full suites not done; a concern on the clean-source line (see "Open questions") | finish from `handoff-OD3P.md`; T2 (one-sided p on the verb, in `nav_summ`) goes last, after B2 is in root; review; merge before the OD-3 read (not on the cells' path) |
| SQL4 (3b) | no pool yet | not dispatched | after SQL3, S2, B2, C3, D3 are in root; else P10 untouched (SQL-12) |
| A4 (stretch) | pool-12 | not dispatched | at T2's merge, or P10 whole if the host is saturated (W3-13) |
| T2 | pool-22 `feat/p9-t2-20261003`, head `dfa4218d` (report `2e9d6813` + APPROVE review) | APPROVE | merge LAST of wave 2, as a unit |
| PRE | sprint dir `p9-prereg.md` | round 2a written; PRE-32 .. PRE-39 not yet in the text | round 2b after the wave-3 P9-B0 and the AL-CLOCK / COV-MV / D3 / OD3P reports; then a review; then the PM rules the file |

Root at the stop (`handoff-root-m2j.md`): code head `327c86b2` (C3 part A merged, 0 conflicts), docs head `4dc831ac`,
no merge open, nothing running. Builds p9-2p (Debug) and p9-2q (Release; NAV exe `adcf72d4`), 0 warnings; next free
tag `p9-2r`. DONE in M2j: the 33 named gtests on both builds; Debug whole target-tests 390 passed; tiny-world canary
15 on both goldens; item 2 (C3-6): X-5 and Y-F0 NAV differ from their references only at the producer keys (X-5
against its C1 reference `p9-c1-s2-x5-nav`: it had no P9-B0 run); item 3: `--spec` runs equal the argv runs on all
27 files; Part B D-COV-EIG: NO rank 00-02 is structurally null in either recipe, so the third pooled row equals the
all-ranks row (attempt 1 void, the amendment committed before attempt 2). NOT done: items 4-9 (the opt-in check,
Release = Debug NAV and wall time, SQL2-CLS for `atx.nav-spec/v1`, the W3-8 fixture flip, the C3-5 probe, ROOT-1),
Release whole target-tests, `atx-impl-tests`, the deletion of `p9-m2i-cov-y1-nav`.
**Known-red set of root: one EXPECTED red until item 6 lands: the classes guard (the literal `atx.nav-spec/v1` is not
in `classes.json` yet).** Nothing else; ROOT-1 has not been run on this head. The store producer exe p9-2o
`ccc73cd1` was replaced in `build-equity-rel/bin` by a newer build (`0afaffcc`); the stores are pinned bytes (CRT-2).

## Open with the owner (surfaced; none blocks engineering)

1. **Permission refusals this session** (none retried as it was):
   - D2R reviewer: an `Edit` of `scripts/tests/test_research_spec.py` (a temporary removal of one assert for a mutation
     test) was refused ("Security Test Removal"). Effect: one loop's teeth are unproven by mutation.
   - C3 lane: a Bash chain `rm -rf %TEMP%/c3-navid-dry && cd pool-15 && git switch --detach 99b676f7 && ...` and an
     `Edit` of the lane's own `fix1-nav-identity.py`. Effect: the pre-round vs post-round exe comparison did not run;
     root proves flag-absent neutrality against the P9-B0 references instead (C3-6). `%TEMP%/c3-navid-dry` (12 MB) is
     left in place.
   - B2 lane: `git checkout --theirs scripts/research_cycle.py` ("Irreversible Local Destruction") during the merge of
     root. The merge was aborted. **Ruling B2-14 is the owner's to confirm or reverse:** the follow-up resolves each
     conflict region by an ordinary edit and uses no wholesale substitute (no checkout / restore / copy-over).
   - Root M2i: `rm -rf "$S/$d"` (its own pytest basetemps, through shell variables) was blocked; the agent then ran the
     literal-path form the check's own message offered. **Ruling PERM-1** allows exactly that and nothing wider.
2. **Worktrees.** Pools 8, 13, 21, 23 no longer existed when leases were taken (the owner removed them); the lease
   script re-created 8, 13 and 21 for AL-CLOCK, SQL3 and OD3P. Each lease printed "configure failed; lease remains
   held": the one-time `dev` preset configure fails on this branch (it names `atx-vol`, `atx-kb`, `atx-agent-db`,
   `atx-options-engine`, which the branch does not have). The lanes use `equity-dev` / `equity-rel`. An owner item with
   OD5-3 (the broken `dev` preset).
3. **`C:\atx\data` clean-up** (asked for in the session before): NOT in this session's goal text and NOT done.
   Nothing was deleted. What is known is in the ledger line "OWNER INSTRUCTION ... C:\atx\data" and in status-6.
4. **Rulings the owner may want to look at before the cells run** (all made under the delegation): OD5-1, PRE-1,
   PRE-10 (from status-6), and from this session: PRE-24a (P9-MV stays registered although its nearest precedent,
   spo-v2, lost .79 Sharpe), PRE-29 (P9-X and P9-V read Release-built stores, so P9-X is not byte-comparable with
   v8's Y-1), PRE-30 / PRE-34 (P9-X is not a new trial if no P9 cell is accepted before it), PRE-31 (a flagged rule
   is dropped from the clean-source history line, like a flagged member), COV-13 (the reading rule for eigen ranks
   00-02), W3-EARLY-2 (lanes start on approved heads before root merges them).
5. **Owner items for the final report** (unchanged): local `main` / `origin/main` (OD5-3), the deterministic `pow` /
   `exp2` re-pin (OD5-2), data purchase (OD5-5), the atx-db request file (AL-DATA). New: the previous session's
   orphaned scratch dir (SCRATCH-2).

## Open questions, NOT ruled (the next PM rules these)

- Whatever the eight stop handoffs list as open (read each before dispatching). Known from the hand-backs, each with
  the PM's recommendation (NOT a ruling; the owner's stop came first):
  - C3b item 7 (W3-6), a design gap: deleting the Python `sign_pm7_35` leaves the wave screen with no sign rule; B1's
    C++ predicate is reachable only inside `atx-research-admission screen`, which the wave does not call.
    Recommendation: keep the Python copy through P9 as a named G-P5 exception (P9-S lapsed, so no P9 cell runs an
    admission screen) and make "the wave calls the C++ predicate" a P10 item; drop W3-6 from C3b.
  - C3b found two prereg-pinned rule files with a stale `manifest_note` "gross_match pm6-6" (not edited).
    Recommendation: a pinned registration is not edited; the prereg states that the note is stale and that the
    templates name `pm6-6-engine`; root checks the templates before the re-pin.
  - OD3P: the IC runner can make a member "absent" only as weight 0, which also cuts its theme's weight, and
    theme-erc-v1 books (Y-F0) refuse it outright; the clean-source history line of PRE-31 probably needs a small C++
    change. Recommendation: scope it in OD3P's next round (what "absent with TRAIN weights carried" must mean for
    theme masses under theme-erc), rule it before any history run, and give the C++ change to D3 part B or a small
    follow-up lane; if it cannot land, the clean-source line is printed "not read" (it decides nothing).
  - AL-CLOCK: P9-C's "turnover share up" compares two books, so the reference needs the same print. Recommendation:
    a print-only opt-in flag named alike on cell and reference, as PRE-7a did for `ann_mean_gross`.
  - AL-CLOCK's rule is flagged (`source_sample_end` 2019), so under PRE-31 the clean-source book drops it.
  - SQL3: the new literal `atx.run-store/v1`. Recommendation: the lane lists it; root adds it to `classes.json` at the
    merge, as for every other literal (SQL2-CLS).
  - D3: the fitter's producer fingerprint forbids any edit inside `fit` / `fit_prior`; the walk-forward design in the
    handoff works around it. Read that design before the lane resumes.
- Root M2j's four questions: (1) resume order: recommendation: as the handoff proposes (4, 5, the rest of 1, 6, 7, 9,
  8 last); (2) X-5's reference `p9-c1-s2-x5-nav`: recommendation: confirm (it is X-5's reference of record); (4) keep
  a copy of the store producer exe p9-2o `ccc73cd1`: recommendation: not needed (the stores are pinned bytes; the
  receipt names the exe hash), unless a copy still exists in the build receipts dir.
- D-COV-EIG's outcome (root question 3): the reading rule ran as fixed: no rank is structurally null, so by COV-13
  the three ranks stay in every pooled row and are a FINDING (near-null factor directions the model under-forecasts
  by a factor above 1,000 in both recipes); P9-V's predictions quote the all-ranks row, which ranks 00-01 dominate.
  Recommendation: let the rule stand (it was fixed before the look; changing the exclusion now is tuning); the prereg
  prints the per-rank table and says the pooled eigen figure is dominated by two ranks; COV-MV's singular-safe form
  and its "share on ranks 00-02" print (COV-13 (c)) matter more now; the model defect is a P10 item for the risk model.
- The round-2a prereg text does not carry PRE-32 .. PRE-39; round 2b writes them in.

## Gates and rules the next PM must hold (new this session; status-5 and status-6 list the earlier ones)

- ENV-1: a test run with a child exiting 3221225794 (0xC0000142) is void, re-run once; never a known red.
- PERM-1: a refused command is never retried or replaced by a same-effect substitute; only the form the refusal's own
  message names may be used, once, and is reported.
- EXE-CUR: every identity or run of record names the exe's tag, SHA-256 and source commit; a stale exe is rebuilt first.
- SEAL-ALLOW-3: `--seal-allow <run's UTC date>=SEAL-ALLOW` only after the per-hit check; part of every cell's driver argv.
- COVR-1: every real-data COV run through the bounded runner; `covr_cmp.py` only compares.
- CRT-2: the two D-COV stores are pinned bytes, never rebuilt in P9.
- COV-13: the eigen reading rule is fixed; COV-MV must stay defined for a singular factor covariance.
- W3-EARLY-2 / HOST-2: lanes start on approved heads; one build at a time; 90-minute gate wait; disk thresholds 12 / 6 GB.
- PRE-35: no cell before D3's two guards are merged and APPROVE; P9-W lapses if the walk-forward items are not in then.
- PRE-14 (unchanged): P9-C lapses without AL-CLOCK, P9-MV without COV-MV, merged and APPROVE when the cells start.
- B2-14: no wholesale conflict resolution in B2's merge.
- C3-5 / C3-6: at C3's and AL-CLOCK's merges root probes which `--rule` token reaches the NAV argv; flag-absent NAV
  identity is shown against the P9-B0 references.
- ROOT-1 (unchanged): the whole `scripts/tests` and the stage chain at every merge. Known-red set: EMPTY.
- Ledger appends: write long blocks with a quoted heredoc (`<<'EOF'`) or a scratch file; never edit earlier lines.

## Next steps, in order

1. Verify the stop state: `git log` / `git status` in pools 2, 8, 13, 14, 15, 20, 21, 24; read each handoff; check the
   trial ledger (133 lines, `27e40f9f`); `lease-worktree.ps1 -Status` (never stale recovery).
2. Root (one fresh agent per step group): finish M2j from `handoff-root-m2j.md` (C3 part A's re-proof items, the
   C3-5 probe, D-COV-EIG, ROOT-1).
3. Resume the stopped lanes in parallel, each with a fresh agent from its handoff: B2 follow-up first in priority
   (three lanes wait on its merge for their last items), D3 part A (critical path), AL-CLOCK, COV-MV, C3b, SQL3, OD3P.
   Each gets a task review; fix rounds until APPROVE; root merges in this order as they are ready: B2 redo, C3b,
   AL-CLOCK, COV-MV, D3 part A, SQL3, OD3P; then D3 part B, SQL4.
4. T2 merge last of wave 2; A4 then or P10. Wave-2 gate proper on that head: suites Debug + Release (0 failed, named
   skips only), golden at 1 and 4 workers, canaries, P9-B0 (substitution list first), W1-I2 (b) re-pin incl.
   `exes.eval`, the store step (M2A-4), scoreboard, G-P ticks, whole-wave-2 review. (Wave-3 lanes merged before it are
   covered by the same gate head.)
5. Wave-3 gate, wave-3 P9-B0 and re-pin: R-2, Y-F0 and Y-1 re-based (Y-1 on the Release v1.1 store, PRE-29);
   `--ann-mean-gross` named on every reference (PRE-7a); CRT record; rho of the P9 pairs. EXE-FREEZE from there.
6. PRE round 2b (33 placeholders; PRE-32 .. PRE-39 into the text; G-B7 step values from the v8 ledger's public lines),
   prereg review by a fresh agent, the PM rules `p9-prereg.md`.
7. Cells in order: P9-L, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X, P9-V (lapses per PRE-14 / PRE-35; P9-X per PRE-30 /
   PRE-34); adoption print; OD-3 once on P9-F0h (four history lines; clean-source book per PRE-31) after OD3P is
   merged; freeze gate; the final list of every ruling (313 "Ruling" lines at this stop).

## Wave-3 dispatch table (replaces status-6's)

| lane | real pool | carries added in RESUME 6 |
|---|---|---|
| C3b | 15 | C3-7 (grid reader duplicate keys), S-12 assertion, E2-16a test only, D2R-2 (a) text; optional C3-10 log line, S-13 |
| D3 part A | 20 | PRE-35 split; D2R-3 (S1, S2); the live-table check as a test; KINDS-TW optional |
| B2 redo | 14 | B2-14; PRE-15 done before the merge (verb row `dsr.tot.tot_minus_m`) |
| AL-CLOCK | 8 | C3-5 probe at merge (root) |
| COV-MV | 24 | PRE-24a prints, COV-13 (c) singular-F test and the eigen 00-02 share print, PRE-29 store pin `ea58691f` |
| SQL3 | 13 | hooks in B2's files last |
| OD3P | 21 | PRE-31: 12-id drop list and the clean-source base book; T2 last |
| SQL4, A4 | not leased | - |

## Rulings made this session (full text in the ledger from "RESUME 6")

Process / host: M2H-1, ENV-1, EXE-CUR, SCRATCH-2, W3-EARLY-2, HOST-2, PERM-1, M2I-HK.
Seal: SEAL-ALLOW-3. E2: E2-16a. Kinds: KINDS-TW.
D2R / COVR / COV: D2R-3, COVR-1, COV-13, CRT-2.
C3: C3-5, C3-6, C3-7, C3-8, C3-9, C3-10. B2: B2-14.
Prereg: PRE-24a, PRE-28, PRE-29, PRE-30, PRE-31, PRE-32, PRE-33, PRE-34, PRE-35, PRE-36, PRE-37, PRE-38, PRE-39.
Owner's words recorded: the parallelism note (before W3-EARLY-2) and STOP 6.

## Standing warnings (in addition to status-5's and status-6's)

- Eight to nine agents at once is what the host took in this session with 34-85 GB free; builds still go one at a
  time, so C++ lanes queue at the memory gate. Watch disk when several lanes hold Debug and Release trees.
- Lanes started under W3-EARLY-2 carry other lanes' approved heads; each must merge root's current head again before
  its report, and root merges in the order of step 3.
- The lease script picks the lowest free pool number and re-creates missing trees; record the real pool (POOL-MAP).
- A background agent keeps "running" while its own wait-loops or deletions run; its handoff file and commits show the
  real state sooner than its final message.

## Uncommitted at the stop (pool-2)

`progress.md` (the PM's last lines), this file, `next-goal-prompt.md`, and whatever `handoff-root-m2j.md` lists. The
first root agent of the next session commits the sprint-dir files in its housekeeping docs commit (never the PNGs
under `docs/plans/`, never a cache directory).
