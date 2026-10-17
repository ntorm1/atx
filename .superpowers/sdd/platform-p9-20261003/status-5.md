# P9 sprint: PM handoff 5 (owner stop, 2026-10-04)

For the next parent (PM) agent. Supersedes `status-4.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir). An engineering status for the owner is in
`docs/plans/2026-10-04-p9-engine-impl-status.md`.

## Read order

1. This file.
2. `resume4-dispatch.md`: the standing contract every agent gets (hard rules, final-message format). Reuse it.
3. The ledger `progress.md` from the line "RESUME 4" to the end. Every "Ruling" line is binding. The list of this
   session's rulings is at the end of this file; the ledger has the full text (decision, why, cost if wrong).
4. `handoff-root-m2e.md` (root, stopped mid step group) and `handoff-root-m2d.md` (last finished root group).
5. `wave3-carry.md`: per-lane carry for wave 3. Its last sections ("PM answers to Q1-Q18" and the "Added after
   drafting" lines) win over everything above them in that file.
6. Lane handoffs, only when you dispatch that lane: pool-17 `handoff-E2.md`, pool-18 `handoff-S2R.md`,
   `al-data/task-AL-DATA-report.md` ("Fix round 2 (stopped by the owner)").

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`, unchanged
(0 trials this session).

## State in short

Wave 2 is 7 of 12 lanes merged and verified (SQL1, SQL2, A3, AL-SIG, S2, D2, COV). Three more are review-APPROVE and
wait for root (C2, AL-COMB, B2's identity part), T2 is APPROVE and merges last, E2 is in fix round 3, B2's
post-identity part must be redone after E2 merges. Wave 3 is prepared (carry file, briefs) and not dispatched. No P9
cell has run. Two findings are with the owner and gate the P9 cells and the OD-3 read (section "Open with the owner").

## Done this session

- All 17 open lane questions from status-4 ruled; two owner-level items asked and answered (OD-A3-SEAL, OD-A3-6F).
- Root M2b: A3 post-flip checks green (A3 fully verified); AL-SIG merged.
- Root M2c: S2 merged and re-proved (Debug + Release). X-5 u / w byte-identical to the P9-B0 references. Cold Release
  u 163.7 s -> 93.7 s.
- Root M2d: D2 and COV merged and verified. No identity miss.
- Lane rounds and reviews: D2 APPROVE, S2 APPROVE, COV APPROVE (re-review 2), C2 APPROVE (re-review 2), AL-COMB
  APPROVE (after moving onto D2), B2 identity part sound, E2 two fix rounds done (third running at the stop).
- Main-merge verification written (`main-merge-verify.md`).
- Wave-3 preparation: `wave3-carry.md`, `briefs/brief-COV-MV.md` (new), 18 dispatch questions ruled (W3-1..W3-18).
- AL-DATA run as a doc-only lane: found that the accepted book's universe reads 2026-dated inputs through the atx-db
  identity bridge.
- S2R: the Release-only golden red traced to the C runtime's `pow`.

## Lanes at the stop

"Stopping" means the agent was told to stop at a clean point and write a handoff when this file was written; read
that handoff for the exact final state before acting.

| lane | pool / head | state | next |
|---|---|---|---|
| SQL1, SQL2, A3, AL-SIG | merged | complete | SQL2's real-tree catalog run + X-5 cache identity run at the wave-2 gate (M2A-4) |
| S2 | merged `f6444417` | complete | S2R below |
| D2 | merged `61378337` | complete | three Suggested carried to D3 (D2-9) |
| COV | merged `694b4b95` | complete | D-COV + Debug = Release container identity at the gate (COV-7); S10 / S11 carried to COV-MV (COV-11) |
| C2 | pool-15 `4d77d3db` (review `1ea664f2`) | APPROVE, NOT merged | root M2e stopped before this merge (nothing built, nothing run). Root items: Debug + Release builds, both `BookNormalScore.*` green in Release (clears M1c-RED), X-5 gm-pair identity (substitution list written first), then delete `wave_rules.py:110-119` |
| AL-COMB | pool-19 `efb7ab2e` (review `098e6ccd`) | APPROVE, NOT merged | root merges after C2 (second half of M2e). MR-4 waits for E2's merge |
| B2 identity part | pool-14 `1b25b9a4` | sound (ledger: complete) | root M2f: merge it, measure rho of the X-5 / Y-F0 pairs first, run the TRAIN identity; `atx-research-eval` into `$canaryExes`; two literals into `classes.json` |
| B2 post-identity part | pool-14 `26fe8d50` (deletion `598d2199`, B2-10 `8fb17ad6`; review `4bed5c43`) | CHANGES_REQUIRED | redo AFTER E2 is in root (B2-12): lane merges root head, places its hunks on E2's split files, implements B2-11 (hash check at the judge, pinned verb), tests on a real v2 manifest; then re-review; then merge |
| E2 | pool-17 `42d7401e` (WIP `daaa483c`; `handoff-E2.md`); last reviewed head `d0054b32` (re-review 2 `558070e5`: 1 Required) | fix round 3: code and tests committed as `wip`, NOT verified | committed: E2-10 (`nav.leverage`), N3 test (fails with the N3 line reverted), case / single-dash spellings, route message, E2-12. Ran: `test_wave_kinds.py` + the W1-I1 test, 75 pass, seed 0, BEFORE the E2-12 edit. NOT run: full `scripts/tests` and `test_stage_chain.py` under seeds 0 and 1. Missing: "## Fix round 3" report section and the final `fix(e2)` commit. Then re-review 3, then root merge with: MR-4, the positive live-table check, C2's S13; B2's post-identity redo follows |
| S2R | pool-18 `49029727` (wip `5c9a5162`; `handoff-S2R.md`) | test split under M2c-RED-2, partly done | done: new `EveryPreW2OpcodeVmMatchesOracle`; the pinned test skips unless `_MSC_VER && _DEBUG && _DLL` (/MDd, `ucrtbased.dll`), pins unchanged; Debug alpha 783/783, S2 filter 28/28, IC 204/204. NOT done: Release build and run (expected 780 pass / 2 skip / 0 fail), report section "## Ruling M2c-RED-2". No build tree left; next tag `p9-s2r-f`. Carries an unruled question (below). Then a light review and root merge |
| T2 | pool-22 `2e9d6813` | APPROVE | merge LAST, as a unit |
| AL-DATA (doc-only) | `al-data/` in this dir | re-review CHANGES_REQUIRED (3 major); fix round 2 barely started | `atx-db-request.md` is marked DRAFT at its top (411 lines), not to be handed over. Done in round 2: only a source check of path (a) (see "Open with the owner" item 1); no text rewritten. Not started: the runnable rebuild-vs-rebuild diff, the third major, minors. Then re-review, then hand the file to the owner |

Root at the stop: docs head `6960568f`, code head `694b4b95` (the COV merge). M2e stopped before either merge: it
committed only the housekeeping docs (`081bf8cb`) and its handoff. Nothing built, nothing run. Next free build tag:
`p9-2d`.
Known-red set after M2d: pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`
(cleared by E2's merge), Release `BookNormalScore.*` x2 (cleared by C2's merge), Release
`AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED-2: becomes a named Release skip through S2R).

## Open with the owner (surfaced, NOT ruled; ask again at the start)

1. **The accepted book's universe may depend on 2026-dated inputs (ALD-6, ALD-7).** The book (67 candidates of
   `lib-v8ysb-gm`) and the role `train-2020-2023-lo3` read the atx-db identity bridge `identity-bridge-v2-pit`
   (role universe `linked-operating-v3`; 50 of 84 fields). How sure each part is:
   - Confirmed by three readers, last at `dec03454` with file:line: the bridge's primary / secondary flag is computed
     across all tiers with the 2026 backfill lines included (`identity_table.py:72-85`, `:111-112`), and the backfill
     rows are dropped only afterwards (`:187-195`).
   - Stated by the reviewer only, NOT yet re-verified: that the role universe keeps only primary lines, and that the
     2026 SEC company-ticker list changes which links the r4 rebuild accepts.
   - Unmeasured: how many pre-2024 links or universe member-days change. Only the atx-db session can run the
     rebuild-vs-rebuild diff; the request item A0 that asks for it is still a draft.
   **Gate in force: no P9 cell and no OD-3 read until the owner supplies the diff result or explicitly accepts the
   exposure.**
2. **`pow` differs between the Debug and Release C runtimes (M2c-RED-2).** The real fix (an in-repo deterministic
   `pow` / `tanh`, or one CRT for both builds) moves expected hashes, which the owner forbids. In force: no pin
   touched; the pinned-digest test is skipped, visibly, in Release; VM = oracle is asserted in every build; root records
   `ucrtbase.dll` version + SHA-256 at each P9-B0 and before each cell. The owner may approve the re-pin fix instead.
3. **Local `main`:** `origin/main` was pushed to `dec03454` at 2026-10-03 23:02:04 although the ledger said "no push";
   merged main has 6 factory gtest reds attributed by reading to main-side commits; the `dev` preset has failed to
   configure since 2026-09-19. The sprint touches none of it (MAIN-V2). pool-25's lease is kept until the owner
   answers.
4. **P9-S lapses at 0 trials (W3-15):** AL-SIG left no queueable string and no AL-DATA family clears the bar. P9 has
   no new-signal cell unless the owner directs one (for example from WQ-EXPLORE, which would count its constructions).
5. **Data purchase (OD-P9-4 / OD-P9-5):** money decision; weaker now, since no family clears the bar (ALD-4).

Earlier owner decisions still open from the plan: OD-P9-1 (return hurdle; placeholder 8.0%), OD-P9-3 (production
leverage; P9 keeps L <= 2.0).

## Open questions, NOT ruled (the next PM rules these)

- S2R: in ResearchFast, `ts_zscore` of a flat window returns 0 where AuditExact returns NaN (204 cells of the
  battery); the "NaN-only flat-window" rule is batch-only. The new VM = oracle test skips the NaN-pattern check on
  rows that carry a ResearchFast tolerance. Decide: is the 0-vs-NaN difference a documented property of ResearchFast
  (then the skip stands and the prereg says which eval mode the cells use, PATH-1), or a defect to fix in the
  streaming path (S2's files)? Check first whether any book candidate or registered P9 string uses `ts_zscore` on a
  window that can be flat.
- AL-DATA round 2: the rewrite agent disagrees with the re-review on one cited line (`identity_backfill.py:35` limits
  only the strict days; the backfill span starts at `:116`); it does not change path (a).
- None from E2 (round 3 only needs its suites, report section and re-review).

## Gates and rules the next PM must hold

- ALD-6 / ALD-7: see item 1 above. Ask before the first P9 cell even if everything else is ready.
- EXE-FREEZE: after the wave-3 P9-B0 and its re-pin commit, the NAV / IC / eval exes are not rebuilt until the
  freeze gate; a forced rebuild means P9-B0 and the re-pin are redone first.
- M2c-RED-2 (d): `ucrtbase.dll` version + SHA-256 recorded at each P9-B0 and before each cell; a change means P9-B0
  is redone. All runs of record are on the Release exes.
- B2-11 / B2-10: every P9 cell spec names `exes.eval`; verify checks key + pin; the hash is compared at the judge.
  Root adds `exes.eval` to the lineage parent specs in the W1-I2 (b) re-pin commit after the wave-2 P9-B0.
- Like with like: a cell's eval mode (PATH-1) and fit mode (ALCOMB-7) equal its P9-B0 reference's.
- W1-I2 (b): parent specs are re-pinned to the P9-B0 references after the wave-2 P9-B0, and again after wave 3.
- W3-EARLY: a wave-3 lane starts when its own hard dependencies are merged and verified in root.
- RUN-1: real-data runs use `--max-rss-mib 2048` unless measured otherwise.
- ALCOMB-8: a permission refusal is never retried verbatim and never routed around. Configure only as
  `powershell scripts\atx-build.ps1 configure -Preset <preset>`; build only as
  `powershell scripts\research-build.ps1 -Tag <tag> -Targets "<t>" -Preset <preset>`.
- Build trap: ccache can restore stale PCHs after deps are re-fetched; delete the stale PCHs by name and rebuild
  with `CCACHE_RECACHE=1`.

## Next steps, in order

1. Ask the owner the five open items (at least 1 and 2). Carry on with engineering meanwhile.
2. Root, one fresh agent per step group (pattern: the M2c / M2d / M2e prompts are summarised in the ledger's
   dispatch lines; recipe in `root-wave2-merge-brief.md`):
   - finish M2e from `handoff-root-m2e.md` (C2, then AL-COMB);
   - M2f: B2 identity part + TRAIN identity;
   - E2 merge once re-review 3 is APPROVE (with MR-4, the positive live-table check replacing D2's old "flip" item,
     C2's S13);
   - then B2 post-identity redo (lane, re-review, merge), S2R merge, T2 last.
3. Wave-2 gate (0 failed; one named Release skip per M2c-RED-2): full suites Debug + Release, golden at 1 and 4
   workers, canary goldens, P9-B0 again (substitution list written first: CM-5 keys from D2, `ic_cache_bytes` from
   B0-W2-1, cache hit / miss strings, AL-COMB's fitter fingerprint), W1-I2 (b) re-pin incl. `exes.eval`, D-COV
   (Release, after Debug = Release container identity; read both directions, COV-11), the AL-COMB Q5 zero-trial
   diagnostic, the store step (SQL2 real-tree catalog + X-5 cache identity), AL-DATA's spec amendments (ALD-2, ALD-3:
   needs a build, it changes `russell_recon`'s C++ spec and a gtest), scoreboard, G-P ticks, whole-wave-2 review.
4. Wave 3 by W3-EARLY (table below), each lane with `resume4-dispatch.md` + its brief + its `wave3-carry.md`
   section; review; fix rounds; merge; wave-3 P9-B0; re-pin.
5. PRE round 1 once B2 and E2 are APPROVE; round 2 after AL-CLOCK, COV-MV, D-COV and the wave-3 P9-B0; review; the
   PM rules `p9-prereg.md`.
6. P9 cells in the plan's order (section 5.2), minus P9-S: P9-L, P9-H, P9-M, P9-C, P9-MV (only if COV-MV is merged
   and APPROVE), P9-W, P9-X, P9-V (only if P9-X is the Y-1 form). Then the adoption print, OD-3 once, the freeze
   gate, and the final list of rulings.

## Wave-3 dispatch table (W3-EARLY, W3-11, W3-12)

| lane | pool | start when merged in root | notes |
|---|---|---|---|
| C3 | 15 | C2, COV | carries C2-9, C2-10, PM7-35 copy deletion (W3-6); E2-11 optional |
| D3 | 20 | D2, E2, AL-COMB | files per W3-7 / W3-8; carries D2-9, E2-9, ALCOMB-6, ALCOMB-7 |
| SQL3 | 17 | E2, B2 post-identity | W3-3, W3-4 (one C++ file pair), W3-5 |
| A4 (stretch) | 12 | T2 | to P10 whole if the host is saturated (W3-13); carries OD-A3-SEAL pruning, A1-SHIM |
| PRE | none | B2 and E2 APPROVE (round 1) | two rounds (W3-14) |
| AL-CLOCK (3b) | 19 | C3 | W3-16; append-only edits to C3's registry, merges before COV-MV |
| COV-MV (3b) | 24 | COV, C3 | brief `briefs/brief-COV-MV.md`; W3-10, W3-17; carries COV-11 |
| SQL4 (3b) | 18 | SQL3, S2, B2, C3, D3 | else P10 untouched (SQL-12) |
| AL-DATA | none | running as doc-only | builds nothing (ALD-4) |

Pools are released and re-leased by root after their wave-2 lane is merged. pool-16 holds foreign work: do not use.

## Rulings made this session (full text in the ledger from "RESUME 4")

Hygiene and scope: HYG-1 (unstage cache files), WQ-1 (WQ-EXPLORE stays off-ledger, P10), MAIN-V (resume main-merge
verification), MAIN-V2 (the sprint fixes nothing on main), M2A-3 (keep test group `store`), M2A-4 (SQL2 real-tree runs
at the gate), CLS-1 (two class-table rows), RUN-1 (`--max-rss-mib 2048`), W3-EARLY.

B2: B2-6 (self pair prints "na"), B2-7 (rounding-boundary lines listed, not auto-accepted), B2-8 (call-site pin stays
in the post-identity part), B2-9 (port the bisection `norm_ppf`), B2-10 (P9 cells name `exes.eval`), B2-11 (corrects
B2-10: hash compared at the judge, pinned verb), B2-12 (post-identity part redone after E2 merges).

D2: D2-7 (fitter-wide fingerprint is P10), D2-8 (P12 reference = parent value), D2-9 (no second round; three items
to D3).

COV: COV-9 (lane fixes R2b), COV-10 (multi-seed proof of the band test), COV-11 (no third round; a = 2 control and
group-value check to COV-MV; D-COV read both directions).

E2: E2-5 (exe-on-disk gate accepted), E2-6 (v1 plan text unchanged), E2-7 (role-seal check left to the builder),
E2-8 (one guard on resolved flags, every v2 kind), E2-9 (small fixes; `allow_abbrev` to D3), E2-10 (`nav.leverage`
only by a leverage wave or the gross-match stage, never above 2.0), E2-11 (no SPO rows in P9), E2-12 (test's unmapped
example id), EXE-FREEZE.

C2: C2-6 (verify / judge keep the run recipe), C2-7 (NAV dir keeps the requested-L token), C2-8 (budget case stays
Suggested), C2-9 (no silent default after the Python gm rule is deleted), C2-10 (digest re-check before
`wave_result.build` to C3).

S2: S2-6 (no pre-fix `imputed-v1` cache entry may reach P9-L), M2c-RED-1 (no Release pins, no blame build; partly
replaced), M2c-RED-2 (test split, visible Release skip, CRT recorded per cell), B0-W2-1 (`ic_cache_bytes` off the
identity list), PATH-1 (nothing renamed; cells run in the reference's eval mode).

AL-COMB: ALCOMB-5a (merge instead of rebase), ALCOMB-6 (sleeve vol / Sharpe line only with a C++ producer),
ALCOMB-7 (fit mode = reference's; sleeve into the C++ fit by D3), ALCOMB-8 (refused configure: documented form or
stop), ALCOMB-9 (predictions need a named producer).

AL-DATA: ALD-1 (definitions fixed before data), ALD-2 (request handed by the owner; amendments by root), ALD-3
(directory snapshot is not point-in-time; consumer trace), ALD-4 (bar argued post-publication: no family clears),
ALD-5 (Form ADV part dropped), ALD-6 and ALD-7 (identity bridge: owner-level gate on the cells and OD-3).

Wave 3: W3-1 .. W3-18 (see `wave3-carry.md` "PM answers").

Owner decisions recorded: OD-A3-SEAL (decode-then-drop accepted; pruning to A4), OD-A3-6F (six-field identity
accepted).

PM error to know about: B2-10 (c) named the wrong stage (verify instead of the judge); corrected by B2-11.

## Standing warnings

- Lease `-Status` shows live lanes under dead owners: never run stale-lease recovery from it. Never
  `git worktree prune`.
- Never wildcard-delete in the shared scratchpad (incident SCRATCH-1).
- Lanes delete their build trees: every claim is re-proved by root from the committed head at merge.
- Subagents from this session cannot be resumed by a new session: dispatch fresh agents from the handoff files.
- A subagent's report is its own account: reviewers and root re-prove; twice this session a reviewer overturned a
  lane's claim (AL-DATA "cannot be told from source"; B2's faked-v2 tests).
- `scripts/tests` and `atx-engine/tools/test_stage_chain.py` never in one pytest session.

## Uncommitted at the stop (pool-2)

`progress.md` (PM's ledger, modified), this file, `next-goal-prompt.md`,
`docs/plans/2026-10-04-p9-engine-impl-status.md`, later edits under `al-data/`, and the four owner PNGs under
`docs/plans/`. The first root agent of the next session commits the sprint-dir files in its housekeeping docs commit
(never the PNGs, never a cache directory).
