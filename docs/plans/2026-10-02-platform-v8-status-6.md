# atx-impl v8 (mega alpha US equity long/short): status 6 at the owner stop of 2026-10-02

Written for: the next parent (PM) agent and the owner. Root worktree `C:/atx-wt/pool-2`, branch
`feat/platform-v8-20260929`. Ledger `.superpowers/sdd/platform-v8-20260929/progress.md` (section "PM session 6"
holds every ruling below in full); every merge, build, test and data run is in `integration-log.md` (sections
"cells batch 2b", "2c", "2d", "interim report, render 3").

## 1. Where the book stands

| Cell | Change | L | S2 net Sharpe | dSR | p one-sided | Verdict | N |
|---|---|---|---|---|---|---|---|
| B0c | baseline (lo3 role, delisting returns, warm start) | 1.247 | 1.133 | +.006 | .301 | accepted | 40 |
| R-1 | re-ranked composition `ew-theme-std-v1`, gross matched | 1.1474 | 1.203 | +.070 | .362 | accepted | 41 |
| R-2 | library v8.0 (fields v10; 7 admission trials, 8 re-screens) | 1.1474 | 1.256 | +.053 | .240 | accepted | 42 |
| R-3 | aim rule `ew-theme-std-aim-v1` | 1.1264 | 1.244 | -.012 | .581 | not accepted | 43 |
| R-4 | trade band | 1.1474 | 1.237 | -.019 | .857 | not accepted | 44 |

The book is R-2 (spec `scripts/specs/v8/lib-v80.json`, NAV
`build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`).

| Measure (TRAIN 2020-2023) | B0c | R-2 |
|---|---|---|
| S2 net Sharpe | 1.133 | 1.256 |
| Net annual return | 4.42% | 4.54% |
| Net Sharpe at 4x NAV | .978 | 1.178 |

Every gain is sign-level per cell; none is significant alone. No cumulative test and no freeze gate has run (V8-F).
Net return moved little because gross is matched to the parent in every cell (PM6-6): the gain shows in Sharpe
and in capacity, not in the return level. Budget: N 44 of 51; admission trials 7 of 15; re-screens 8 of 8;
history reads 0.

## 2. Rulings of PM session 6 (full text in the ledger)

- PM6-1: the route to the owner's goal is the registered cell program; no gain is claimed outside it.
- PM6-2: interim render 2 dropped (superseded by PM6-11).
- PM6-3, PM6-7: disk cleanup by inventory, ruled list, then deletion. 63.8 GiB freed (57.4 -> 121.2 GiB free).
- PM6-4: fields v10 stands at 63 reused / 7 computed (manifest `a4a060ae`); 49 / 21 was a stale count.
- PM6-5, PM6-10: FIX-6 rounds 4 and 5 (tests only).
- PM6-6: gross matching. Each construction cell runs at the L that puts its all-rows gross within .005 of its
  parent's, found from mechanics only before any return is read. Calibration runs are not trials. The R-1 run
  at L 1.247 (gross 1.0672) was calibration and stays unread. Supersedes "L is not re-derived".
- PM6-8: R-2's marginal phase residualised on the parent's combined signal only (the parent's theme file named
  11 members v8.0 removed). R-7 and R-12 only add members and keep the full pool.
- PM6-9: add-alpha's K1 plans via `--plan-json` on the 4-year role, standing for R-7 and R-12.
- PM6-11: interim render 3 on R-2 (cut to stats and equity curves at the owner's request).

## 3. Interim report

`docs/plans/2026-10-02-mega-alpha-v8-interim-pitch.html` (2,057,347 bytes, sha256 `fd3a415e`, head `ef7fe89b`),
cut to stats and equity curves at the owner's request. Book R-2. In it: interim callout with p values, each cell's L
and gross; headline B0c against R-2 (S2 net Sharpe 1.133 / 1.256, net annual 4.42% / 4.54%, gross-of-cost annual
6.05% / 5.81%, 2x 1.085 / 1.223, 4x .978 / 1.178, turnover .0341 / .0239, gross .9820 / .9860); year tables; ladder
B0a..R-4; R-2 equity curve and drawdown under each cost scenario with B0c beside it; B0c diagnostics; the rest of
R-2's book section. Checks: 0 unavailable blocks, 22 figures, 88 inline SVGs, equity figure 12,072 points, headless
Edge without console error, works offline (Google Fonts only). Not in it: a render-3 scorecard (the 2026-10-01
interim scorecard remains and is stale) and the v7 figures fig-flow, fig-ops-loop, fig-capacity (no readable v8 L
pair). Tool changes in `atx-impl/tools/mega_report/` (no digest pinned): `v8_headline` block, config-driven IC
figure and universe labels, alpha-t chart drops an empty column.

## 4. Open items

- FIX-6 round 5 is merged (`1a504629`). `scripts/tests` still reads 17 failed / 174 passed / 3 skipped, one cause:
  the spec list in `test_research_spec.py` (`NULL_PINS`, line 290) lacks `r3-aim-gain-gm.json`, written by batch 2d
  after the lane ran; the other failures follow from it. Tests-only fix through the lane in pool 10.
  `atx-impl/strategies` 163 passed.
- R-5 not started (template untouched, nothing on disk). Its ADV cap reads the inherited L: run at the parent's
  L 1.1474, then match gross.
- Remaining cells (E-38 / E-45; one-sided p per PM5-23): R-5; the risk model on the 4-year role then R-6
  (S_prior 20; convergence counts and the tripwire before any return); fields v11 (70 / 3) then R-7 (library
  wave, admission trials from the 8 left); R-8. Then R-10, R-11, R-12 (fields v12) if R-6 is accepted (R-1 was
  accepted, so R-10 and R-11 are defined); R-9 if R-6 is rejected. That is at most 7 trials: N 51 exactly.
- W0-4 re-runs (PM5-22), V8-F cumulative test and freeze gate, H-2, scorecard and pitch with 0 unavailable
  blocks, v7 pitch re-render.
- Integration 8 after the freeze gate: mining branch `1bd448cd` (pool 8), FIELDS-V9 / LIB3 `834d5a05`, the C++
  halves of R6C-3 and R6C-7, `exe_plan`'s missing `--max-memory-mib` (PM6-9); golden `0x889874a3b9b29c55` at 1
  and 4 workers.
- Owner: the Recycle Bin holds 18.8 GiB (four worktrees, pools 3, 4, 9, 11, deleted there at 05:42 on
  2026-10-02, not by this session; every lane commit is intact in git). Emptying it would take free space to
  about 140 GiB. `build-equity-rel` (1.4 GiB, Release v8-13, not adopted) was kept by the date rule.
- Owner decisions carried from status 5: OD-7 and the mined-campaign pre-registration draft, the four data asks
  to atx-db, the v9 library draft's open points.

## 5. Goal prompt for the next parent agent

```text
Finish atx-impl v8 (the mega alpha US equity long/short book) and raise its TRAIN Sharpe and net return through
the registered cell program, from the status at
`C:/atx-wt/pool-2/docs/plans/2026-10-02-platform-v8-status-6.md`. Follow the sprint plan
`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md` and the ledger
`.superpowers/sdd/platform-v8-20260929/progress.md`. Every ruling there is binding: E-1..E-45 with sub-rulings,
W0-a..n, R2-a..h, R7-a..c, PM3-1..10, PM4-1..15, PM5-1..27, PM6-1..11. `integration-log.md` holds every merge,
build, test and data run.

Working method (owner directive). Use subagent-driven development with Opus 5.5 subagents and one pool worktree
per lane (pools 7, 8, 10 exist; lease more with `scripts/lease-worktree.ps1`; pools 3, 4, 9, 11 are gone from
disk, but their commits are intact). Keep your own context small: briefs and reports are files, agent replies
are short. Do no test-driven development and run no review agent after each task. Adversarial reviews happen
only at logical break points and are read-only. Children never build or run data. One integrator at a time
merges, builds, tests and runs data in root (`C:/atx-wt/pool-2`). Make rulings yourself as "decision -- why --
cost if wrong" and write them to the ledger before the measurement they could bias. Park ledger edits outside
the repository while an integrator holds root. Merge lane work by commit SHA. Read `lane-rules.md` and
`integrator-rules.md` before dispatching. PM5-21 holds until the freeze gate: no executable, cycle script or tool
script change (build `v8-12`); spec-only fixes are preferred when the cycle refuses. PM6-6 (gross matching) binds
every construction cell.

State at the start: the book is R-2 (S2 net Sharpe 1.256, net annual 4.54%, 4x 1.178; B0c baseline 1.133 /
4.42% / .978). N 44 of 51; admission trials 7 of 15; history reads 0. Root is clean at the head the status names.

Do, in this order:
1. Read status 6 and the ledger from "PM session 6" down. Through the lane in pool 10 (tests only), add
   `r3-aim-gain-gm.json` and any other spec written since to the spec lists of `test_research_spec.py`; merge by
   SHA; `scripts/tests` and `atx-impl/strategies` must read 0 failed.
2. Cells per `task-CELLS-brief.md`, in batches, one integrator at a time, parent = the last accepted cell, with
   gross matching: R-5; the risk model on the 4-year role then R-6 (S_prior 20; convergence counts and the
   tripwire before any return; a void under E-31a is a blind fix and a re-run); fields v11 (70 / 3) then R-7
   (admission trials from the 8 left; PM6-9 plan route); R-8. Then R-10, R-11 and R-12 (fields v12) if R-6 was
   accepted; R-9 if R-6 was rejected. One-sided p per PM5-23, print both. A rejected cell is never retried. Hard
   budget N <= 51. Commit after each cell.
3. The W0-4 re-runs (PM5-22): v6.1, v7.0, v7.0-lo3, C1-C3, spo-v1, spo-v2 on the 4-year role. They add 0 to N.
   Their gates log and do not stop (PM5-18).
4. V8-F: the cumulative test and freeze gate (one-sided p, E-34; print both). Read the OD-3 history only if the
   gate asks for it. Then the H-2 measurement. Then scorecard v8, with criteria complete through the last cell.
   Fill and render the pitch config with 0 unavailable blocks; every figure must be embedded and checked in a
   headless browser before it is reported. Re-render the v7 pitch. Claim an improvement over B0c only where the
   cumulative test or a registered mechanical criterion supports it. If the freeze gate is unmet, the scorecard
   says so and names OD-3.
5. Integration 8, after the freeze gate: merge the mining branch `1bd448cd` and FIELDS-V9 `834d5a05`. Add the
   C++ halves of R6C-3 and R6C-7, and `exe_plan`'s `--max-memory-mib`. Build the mine, factory, alpha and impl
   test targets. The golden `0x889874a3b9b29c55` must hold at 1 and 4 workers; the fixture's `rung_failed` must
   be 0; `AlphaVmSlotReuse.*` must pass. Then write handoff 2, with the owner decisions (OD-7 and the
   mined-campaign pre-registration draft, the four data asks to atx-db, the v9 library draft's open points) and
   the merge command.

Hidden-data rule absolute: nothing dated 2024-01-01 or later is opened. Never touch `atx-db/`. No push. Never kill
a process you did not start. Disk: 121 GiB free; delete caches only through an inventory, a ruled list and one
integrator (PM6-3).
```
