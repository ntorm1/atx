# Platform v8: status 5 (owner stop, PM session 5, 2026-10-01)

Root worktree `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`. Ledger of record:
`.superpowers/sdd/platform-v8-20260929/progress.md` (rulings PM5-1..PM5-27 this session); every merge, build, test
and data run is in `integration-log.md`. Previous status: `docs/plans/2026-10-01-platform-v8-status-4.md`.

## 0. State in one paragraph

The platform is merged, built and green through integration 7 (build `v8-12`, every research executable on one
tag). Wave 0 is closed: pins written, protocol line chained, both base specs locked. Three cells are ledgered on
TRAIN 2020-2023: B0a (N 38), B0b (N 39, accepted) and B0c (N 40), the v8 baseline, S2 net Sharpe +1.133 with
mechanics passing. R-1 was constructed and failed mechanics on gross (1.067 against a limit of 1.05); none of its
returns was read and it is not ledgered; its status is the first open ruling. Nothing after R-1 has run. N 40;
admission trials 0; history reads 0; 2024+ never opened. An interim scorecard and pitch were rendered on B0a, B0b
and B0c; no improvement is claimed. Five side lanes for v9 alpha generation are finished and unmerged.

## 1. What this session did (in order)

1. Resumed and finished both FIX-4 lanes (FIX-4a `98ef8d89`, FIX-4b `0b093a4c`).
2. Integration 6 part C (`v8-11`): both merged, 0 compile fixes, 0 test fixes, identity 4 passes; the E-31a void exit
   path is now tested end to end.
3. Scoped read-only review of the FIX-4 diff: 0 invalidating, 2 major, 5 minor. The tie rule (PM4-12), theme
   order (PM4-11), pooled fit (PM4-7), parent check and guards all match. Two ladder mismatches fixed by lane
   FIX-5 (`d2304773`, Python only), merged in integration 6 part D.
4. Owner directive mid-session: more parallel lanes; the design may change to prioritise alpha generation.
   Result: integration 7 moved before the locks (PM5-6), and wave AG opened (section 2).
5. Integration 7 (`v8-12`): MINE-FIX `20e7bd19` merged; the golden `0x889874a3b9b29c55` holds at 1 and 4 workers;
   identities 1, 4, 7, 8 pass; every research executable on one tag.
6. Wave 0 part 2b: plan-only, the cold u pass, the signal overlap (bit-identical, 48 / 48 candidates). The daily
   IC overlap fell in W0-a's stop class: the oriented column of three candidates and the u-pass combined row.
   Cause shown by test (PM5-16, PM5-18): raw columns identical for all 48; the three oriented columns are exact
   negations; the combined row is bit-identical under the 3-year signs. The book trades prior signs, so nothing
   it uses moved. Then R14: pins, protocol line, lock on base-lo1.
7. Cells batch 1a: B0a, lock base-lo3, B0b. Batch 1b: the delisting-returns role, B0c, diagnostics G-1..G-3,
   Release A/B (not adopted). Batch 2a: R-1 to the mechanics read, then the owner stop.
8. Interim report (PM5-27).

## 2. Lanes at the stop (all trees clean)

| lane | pool | branch | head | content | merged |
|---|---|---|---|---|---|
| FIX-6 | 10 | feat/platform-v8-fix6-20261001 | `702cf051` | tests only: the spec test fixtures are independent of locks, parents, generated specs and filled flags (PM5-20, PM5-24); `scripts/tests` 186 passed / 4 skipped on the lane | no: merge before the next cell |
| wave AG mining | 8 | feat/platform-v8-minejoin-20261001 | `1bd448cd` | MINE-MEM + MINE-STAT + MINE-RUN joined; members streamed by date; member files verified once; engine slot-reuse test for every op. C++ never compiled | no: integration 8, after the freeze gate |
| FIELDS-V9 / LIB3 | 11 | feat/platform-v8-lib3-20261001 | `834d5a05` | v9 library draft (10 prior-class candidates) and two draft field builders, new files only | no: integration 8 |
| FIX-4a, FIX-4b, FIX-5, MINE-FIX | - | - | - | merged (integrations 6C, 6D, 7) | yes |

In root without the merge, `scripts/tests/test_research_spec.py` fails on its fixtures (locks and the R-1 parent
edit); FIX-6 fixes exactly that.

Wave AG numbers (from the lanes' models, uncompiled): default 4-year mining campaign 10.85 GiB at the start of the
session, now 3,161 MiB at 1 worker and 4,572 MiB at 4; the first real campaign's shape 2,765 / 3,978 MiB. Overlap
factor by budget band 1.47 / 1.54 / 1.63; budget ceiling 10,000; confirm factor 1.77 / 1.96 / 2.15 by reads.
Pre-registration draft, spec template and runbook for the first mined campaign are written (15 owner decisions).

## 3. Rulings made this session (all in progress.md, each before the read it could bias)

| id | decision |
|---|---|
| PM5-1..4 | FIX-4 lane choices accepted (strict module hashes in the R-11 parent check; optional parent flags; no rewording of the infeasible-cap message; a shared test header) |
| PM5-5 | the scoped review ran beside integration 6 part C on the frozen lane heads |
| PM5-6 | integration 7 (MINE-FIX) moved before the locks; supersedes PM4-2's timing |
| PM5-7 | wave AG: four alpha-generation lanes beside the cells, registered for v9; the v8 trial program untouched |
| PM5-8, 9, 14 | mined-v1 silent points: an undefined rho fails (members and kept candidates); rho step before the cap; separate confirm factor; the recipe carries both tables |
| PM5-10 | mining memory target 2,560 MiB (not met on the default shape) |
| PM5-11 | "planned turnover per unit gross" (R-1, R-10, R-11) is executed `tau_gmv_mean / mean_gross_leverage_all_rows` on S2 |
| PM5-12 | fix round FIX-5 (Python only) before the locks; the C++ halves of R6C-3 and R6C-7 deferred to integration 8 |
| PM5-13, 15, 19 | follow-up lanes: FIELDS-V9; MINE-JOIN; the engine slot-reuse test |
| PM5-16, 18 | W0-a on the daily IC overlap: stop until a cause test; the book stays on prior signs; a sign-agreement gate applies to new members only, and in a re-run of a ledgered cell it logs and does not stop |
| PM5-17 | base-lo3 locks after B0a (its input is B0a's output) |
| PM5-20, 24 | the spec test fixtures are fixed in tests only |
| PM5-21 | freeze list: no executable and no cycle or tool script changes from the first cell to the freeze gate |
| PM5-22 | the W0-4 re-runs of the v7 cells run in one dispatch after the last construction cell, before V8-F |
| PM5-23 | the one-sided p comes from a `nav_summ --bundle` run without `--ledger` that must reproduce the cycle |
| PM5-25 | B0c stands with gross .926 at the first scored row; the warm-start length is not tuned after a read |
| PM5-26 | two unreferenced Release A/B caches removed |
| PM5-27 | the interim report: an interim config copy, B0a / B0b / B0c only, R-1 as text, no improvement claim |

Disclosures this session: the overlap tool's relative difference showed that three candidates' whole-window
orientation signs differ between the 3-year and the 4-year window (a coarse statistic touched by 2023); an
integrator printed a 3-year (2020-2022) run log with per-candidate IC and sign lines; a script printed that 7 of 48
candidates had no 3-year orientation; the LIB3 lane saw literature statistics for 2005-2024 and filing counts for
2024-2026 in existing documents. None was used in a ruling.

## 4. Verified in root this session

Build `v8-12`: IC `ab7e2cbd`, NAV / targets `5497c89d`, risk `8967952c`, mine `cd661fe9`. Suites at integration 7:
mine 31, factory 390, target 259, book 155, strategy 46, ic 145, combine 233, impl 1,022 / 5 skipped / 1 known
failure; Python strategies 163 + 9, engine tools 253, impl tools 578 / 1 skipped, scripts 183 / 3 skipped before
the locks. Identities 1, 4, 7, 8 pass on `v8-12`.

Cells (S2, TRAIN 2020-2023):

| cell | role | N | net Sharpe | mechanics | verdict |
|---|---|---|---|---|---|
| B0a | lo1 | 38 | +1.125 | pass | re-base ledgered |
| B0b | lo3 | 39 | +1.139 | pass | accepted (dSR +.0135, SE .0403, p one-sided .382, two-sided .775) |
| B0c | lo3, delisting returns, warm start 60 | 40 | +1.133 | pass | baseline by declaration |
| R-1 | lo3, composition v8 | (40) | not read | FAIL: gross 1.0672 | open |

B0c by year (net Sharpe / return / volatility): 2020 +.32 / +1.4% / 4.5%; 2021 +2.35 / +7.7% / 3.2%; 2022 +1.94 /
+8.6% / 4.3%; 2023 +.12 / +0.3% / 3.4%. Capacity: 1.133 at 1x, 1.085 at 2x, .978 at 4x. Stressed borrow fee:
1.069. Diagnostics (gate nothing): four fast members carry 60% of trades; ten themes act as about 4.9 independent
bets; variance 66% factor, 18% industry, 16% specific. Release build: IC passes byte-identical, NAV differs in the
last digit of one cost column; not adopted.

## 5. Open findings and risks

- **R-1's status (first ruling of the next session).** Rule 5 gives "not accepted" on mechanics. Before it is
  ledgered, one read-only code look at why gross rose from .982 to 1.067 under the re-rank. If it is what the
  registered composition does: ledger R-1 rejected at N 41, R-10 and R-11 are undefined (E-45), R-3 runs
  `ew-theme-aim-v2`. If a tool or spec defect is shown: prereg rule 7 (defect line, blind fix, re-run, no new
  trial). Re-deriving L after a read is not an option. No return of R-1 has been read; keep it so until ruled.
- **The deflated Sharpe is not meaningful yet.** Its cross-trial variance rests on three cells until the W0-4
  re-runs of v6.1, v7.0, v7.0-lo3, C1-C3, spo-v1, spo-v2 on the 4-year role exist (PM5-22).
- **B0c's Sharpe is carried by 2021 and 2022**; 2020 and 2023 are near flat. The plan rates an unmet freeze gate
  as likely.
- **The lock pins no executable or script.** PM5-21 is the rule that keeps them fixed; the freeze list is in the
  log section "Wave 0 part 2c".
- **Wave AG C++ is uncompiled** (about 5,000 lines in the mining branch, plus the engine slot test). Integration 8
  must hold the golden at 1 and 4 workers and `rung_failed == 0` on the fixture.
- **Owner decisions pending:** OD-7 and the 15 choices in the mined-campaign pre-registration draft (the
  discover / confirm split now binds on the confirm side); four data asks to atx-db (filing text, N-PORT,
  fundamentals notes, put-wing implied volatility); eight registration points on the v9 library draft.
- The Release build recompiled spdlog in the shared `C:/atx-cache/deps`.
- Disk 35 GiB free; 30 GB is the precondition of every data build; fields v10..v12 and a risk model are to come.
- Minor findings still open from status 4: R6B-C-7, R6B-C-8, SPO-3, SPO-6, F-4, F-7, F-11, F-12, F-13, T-10
  (closed by PM5-11), C-14..C-22; from this session R6C-3 (C++ half), R6C-7.

## 6. Report determination

An interim pitch and scorecard exist, rendered by the report tool from an interim copy of the config (Ruling
PM5-27; log section "interim report (owner stop)"; root head `5ef4eb99`):
`docs/plans/2026-10-01-mega-alpha-v8-interim-pitch.html` (sha `93206179`),
`docs/plans/2026-10-01-mega-alpha-scorecard-v8-interim.md` (sha `3a13fe3f`),
`docs/plans/mega-alpha-v8-pitch.interim.config.json`. They state B0c as the re-based baseline and claim no
improvement: no accepted construction cell and no cumulative test exist. 37 blocks render unavailable: the
bundle and `v8.final`, the paired files and NAV summaries of the cells not run, the member-horizon and V8-F book
blocks, and the diagnostics block (eight split files exist; no tool verb combines them into `diagnostics-v8.json`,
so the scorecard reads each id from its own file). The deflated Sharpe figures in the interim are computed over
three cells and are not meaningful (PM5-22). The ladder shows the plan's N 41 for R-1; the ledger holds 40. The
full v8 report needs the remaining cells, the re-runs, the V8-F cumulative test and the freeze gate. The
registered config, template, tools and ledger are unchanged; the report tests pass (131).

## 7. Next steps (detail in goal prompt 5)

0. Merge FIX-6 `702cf051` (tests only). Rule on R-1 (section 5).
1. Cells from R-2 in registered order per `task-CELLS-brief.md`: fields v10, R-2, R-3, R-4, R-5; the risk model,
   R-6, fields v11, R-7, R-8; then R-9, or R-10..R-12, by E-38 / E-45.
2. The W0-4 re-runs (PM5-22).
3. V8-F cumulative test and freeze gate; H-2; scorecard v8; pitch; the v7 pitch re-rendered.
4. Integration 8 after the freeze gate: the mining branch `1bd448cd`, FIELDS-V9 `834d5a05`, the deferred C++
   halves of R6C-3 and R6C-7. Then handoff 2 and the merge command for the owner.
