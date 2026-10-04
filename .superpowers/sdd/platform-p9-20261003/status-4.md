# P9 sprint: PM handoff 4 (owner stop, 2026-10-03 late evening)

For the next parent (PM) agent. Read this, then `resume-rulings.md`, then the ledger `progress.md` from the line
"RESUME 3" to the end (every "Ruling" line is binding), then `root-wave2-merge-brief.md`. Supersedes `status-3.md`
(its "How the owner wants it run" section still applies unchanged). Do not re-dispatch anything marked complete.

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Sprint dir: this dir. Root tree `C:/atx-wt/pool-2`,
branch `feat/platform-p9-20261003`, head `93a419d3` + PM docs commits. Trial ledger `build-equity/trials.jsonl`:
133 lines, sha `27e40f9f`, unchanged (0 trials this session). Next free build tag `p9-1w`.

## Done this session

- All status-3 open questions ruled (`resume-rulings.md`), plus ~45 more rulings in the ledger.
- **Wave-1 gate MET** (known-reds only) at `3c9eeb01`. P9-B0 holds (6 runs, 0 differences outside the written list);
  re-based references `build-equity/p9-b0-*` pinned. Canary goldens Debug = Release. Release IC = Debug. G-P3 IC half,
  G-P4, G-P6, G-P8 ticked. Two new reds found and fixed (`27e2e5b4` test scratch path; `aa5d4858` config sidecar).
- Whole-wave-1 review: READY_WITH_FIXES (`wave1-whole-review.md`); rulings W1-I1, W1-I2, W1-MIN.
- Wave-2 merges started (W2-ORDER: by readiness). **Merged and verified: SQL1 (`d31c5367` + N1 `8d4cc52c`), SQL2
  (`1300fd6a`, literal `1892a183`).** **A3 merged incl. the registry flip (`ff1637ac`, `08b0695d`, `1c90011a`);
  TRAIN identity holds for the six fields; post-flip checks NOT run.**
- Disk: C: ~57 GB free (was 41 at the low). Rules DISK / DISK-2 in force. `disk-audit.md` has remaining candidates.
- Equity curve for the owner: `docs/plans/2026-10-03-p9-b0-yf0-equity-curve.png` (untracked; Y-F0 re-base, wave-1
  head, TRAIN 2020-2023, five cost scenarios; Ruling PLOT-1).

## Lane state

| lane | pool / head | state | next |
|---|---|---|---|
| SQL1 | merged | complete | pool-21 build tree may be deleted (keep receipts) |
| SQL2 | merged | complete | real-tree bounded catalog run + X-5 cache identity not yet run (owner: root) |
| A3 | merged (flip in) | post-flip checks pending | `handoff-root-m2a.md`: registry check (expect `ok 92`) + 3 pytest files with the real exe; then delete pool-12 tree |
| T2 | pool-22 `2e9d6813` | complete (APPROVE) | merge LAST, as a unit; ledger correction: five expected values changed |
| AL-SIG | pool-13 `ebb081ab` | complete (ALSIG-9: root re-proves at merge) | merge now (A3 is in); literal `atx.p9-blocked-registration/v1` |
| AL-COMB | pool-19 `724ab82c` | APPROVE; waits on D2 | ALCOMB-5: rebase on D2 + S-1/S-3/S-4/S-5/S-6 round, fresh review |
| S2 | pool-18 `2ed2fc18` | fix round 1 done; re-review PARTIAL (`f6c0e964`), no finding so far | finish re-review: test teeth, digest recompute `dd3c6011`, receipts, line length |
| COV | pool-24 `2639de2e` | re-review CHANGES_REQUIRED (`a3c62ba8`) | fix round 2: R1b (half-lives 21 + negative control amp 0.5 + per-rank table), R2b (u32 geometry wrap `cov_container.cpp:631`, `:208-209`); then re-review |
| B2 | pool-14 `57543f39` | fix round 1 ~75%; `handoff-B2.md` | finish B2-R3 pin + fail-closed test, test after deletion, report; identity merge point `d5f653cc`; deletion WIP `dee6cacb` not for merge |
| D2 | pool-20 `57df8298` | fix round 1 ~85%; `handoff-D2.md` | run full pytest set with real exes (build tree kept, 2.8 GB), report section, delete tree; then re-review |
| C2 | pool-15 `3eca79bf` | fix round 1 partial; `handoff-C2.md` (C++ part NOT BUILT, Python patch not applied) | finish R1, R2 (C2-R2), R3 (C2-R3), C2-4b; build Debug + Release; re-review |
| E2 | pool-17 `f692ea36` | fix round 1 WIP `fe140597` (R1, R2, W1-I1, W1-I2a, S1-S6, S9); `handoff-E2.md` | run suites (two separate invocations, seeds 0 / 1), report; re-review |

Handoff files are in each pool's sprint dir (`C:/atx-wt/pool-N/.superpowers/sdd/platform-p9-20261003/`), root's is
`handoff-root-m2a.md` here. All agents confirmed no process left running (remaining python processes belong to the
other session's atx-db jobs). WIP commits are titled `wip(...)`.

## Next steps, in order

1. Root: A3 post-flip checks (`handoff-root-m2a.md`), then merge AL-SIG. One fresh root agent, brief
   `root-wave2-merge-brief.md`.
2. In parallel, resume lanes with fresh agents from their handoffs: B2, D2, C2, E2 (fix rounds), COV (fix round 2);
   fresh reviewer to finish S2's re-review. Each then gets a scoped re-review; on APPROVE write
   `Task <LANE>: complete` in the ledger.
3. Merge as lanes complete (W2-ORDER): E2, S2, B2 (deletion commit only after root's identity run), D2, then AL-COMB
   round (ALCOMB-5) + review + merge, C2, COV, T2 last. Per-lane root items are in the ledger: grep the lane ID.
4. Wave-2 gate: 0 failed, no known-red exception (remaining reds: pytest
   `test_fields_are_the_rule_applied_to_the_registry` (E2), Release `BookNormalScore.*` x2 (C2)). Then P9-B0 again,
   W1-I2(b) parent-spec re-pin, D-COV in Release (COV-7; read new recipe against v1.1, per-rank b, pooled with and
   without the lowest rank), whole-wave-2 review.
5. Wave 3 (C3, D3, A4, AL-DATA, AL-CLOCK, SQL3; 3b COV-MV, SQL4), PRE, `p9-prereg.md`, P9 cells, adoption print,
   OD-3 read once, freeze gate, final "Rulings I made" list. Doc pass on briefs SQL3 / SQL4 first.

## Carry into wave 3 / prereg (already ruled; do not lose)

- SQL3: narrow the `--root` seal check to the repo-relative path; candidate keyed by path (SQL2-S7); path-length
  margin; stale configure-time `git_sha` (W1-MIN M-4); SQL1-MON.
- A4: A1-SHIM; S-R2 guard on all-None stats; per-field re-hash under `--reuse`; price-field co-build dependence.
- D3: `composition_*.py` deletion once ew-theme-aim-v2 + resid diagnostics have a C++ path. P10: `horizon_stats`.
- C3: book-workers parity outside vol-target; per-worker as-of scratch in memory admission.
- AL-DATA: request to the atx-db owner to fix the Russell stage look-ahead (ALSIG-7).
- Prereg lines: timings off for pinned runs; mom-volman fixed-TRAIN schedule limit; label-terminal 30-day
  cause-classification exposure near TRAIN end (P9-L); paired-test fallback if a pair has 1 - rho < 1e-6 (B2-R1);
  candidate ids <= 24 chars (W1-I1); `hf_crowd` / `rank(russell_recon)` / `rank(gia_13f)` not queued.

## Open questions, NOT ruled

- **Owner-level (surface, do not rule alone):** on the real vendor file every row group spans the seal, so
  7,592,840 sealed rows per field have their values decoded and released unread (nothing reaches a computation or
  output; A3-FIX1 allows it). Accept, or require page-level / sorted-file pruning (A4)?
- Root M2a: (1) TRAIN identity was run on the six ported fields only, not v15's 84 (reuse would bypass the engine; a
  no-reuse 84-field build is too big for the machine): accept? (3) keep the `store` test group in pool-2's build
  tree? (4) who runs SQL2's real-tree catalog run and the X-5 cache identity?
- B2: (1) self pair prints SE "na" instead of B2-4's "SE 0"; (2) a printed value exactly on a rounding boundary can
  differ by one digit: is a report note enough; (3) B2-R3 needs edits in `research_cycle.py` / `cycle_verdict.py`
  (E2's files): who owns them; (4) port the bisection `norm_ppf` for house DSR rows or bound N. Also: the lane could
  not reproduce the reviewer's PBO 0.171 series and tests a generated grid (Python 0.1857): the re-reviewer must
  reconcile.
- D2: is the S-6 fitter-wide fingerprint wanted in P9 (changes store identity shape); should the P12 reference in
  engine-mode `parent_check` be the parent's values.
- COV: R2b may be applied by root at merge instead of a lane round (reviewer's suggestion); S9 needs a root check.
- C2 / E2: see their handoffs.

## Standing warnings

- Lease `-Status` shows live wave-2 lanes under dead wave-1 owners: never run stale-lease recovery from it.
- Never wildcard-delete in the shared scratchpad (incident SCRATCH-1).
- Lanes deleted their build trees: every claim is re-proved by root from the committed head at merge.
- ccache cap is 100 GB (more than the disk has free); owner informed, not changed.
- Uncommitted at stop: nothing in pool-2 except the untracked PNGs under `docs/plans/`.
