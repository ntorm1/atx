# atx platform v8 -- status 3 (PM session 3, owner stop, 2026-09-30)

Root `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, head `18176b4d` (code head `48c625fc`, build `v8-9a`),
tree clean. Ledger: `.superpowers/sdd/platform-v8-20260929/progress.md` (rulings E-1..E-45 and sub-rulings, PM3-1..
PM3-10, W0-a..n, all binding). Every merge, build and test: `integration-log.md`. Previous status: status 2
(`2026-09-30-platform-v8-status-2.md`). Goal prompt for the next PM: `2026-09-30-platform-v8-next-goal-prompt-3.md`.

## 0. State in one paragraph

Steps 1-4 of goal prompt 2 are done and step 5 is half done. Every I and M finding of the Wave 1 review (part 1 and
the completed part 2) is fixed and merged or on a finished lane; a scoped re-review confirmed 21 / 21 part-1 fixes
addressed. Integration 5 (fix lanes, R45, A2, H3) and integration 6 part A (R-8, ERA, DLRET, LIB2) are merged and
green; the mining verb compiled first time and its fixture acceptance passes. All eight identities on the 3-year
role pass and the spo-v2 pin is captured. Wave 0 R1-R9 is built on 2020-2023 (projection, base role, repair,
bridge, fundamental events, roles lo1 and lo3, admission probe); no field, signal, IC pass or cell exists yet, so
**no statistic from the 2020-2023 window has been read**. N is still 37, admission trials 0. The owner's directive
for more alpha lanes produced R-8 (risk target), two combination rules (R-10 `ic-shrink-v1`, R-11 `theme-resid-v1`)
and three library v8.2 candidates (R-12 screen set), all registered blind inside the N <= 51 and 15-trial budgets
(Ruling E-38). Four lanes are finished but unmerged (FIX-2, FIX-3, COMB2, ORTH); MINE-FIX stopped mid-way.

## 1. What this session did (in order)

| step | result | where |
|---|---|---|
| Review part 2 (T tests, N unread files, P post-7af37e9d) | I 0, M 10, m 22; all M fixed by FIX-2 / ERA | review-w1-T/N/P.md |
| Fix lanes FIX-C (12 fixes), FIX-AB (8) | merged d3b9513d, d3e6d855 | task-FIX-C/AB-report.md |
| R45 E-25 finished | merged 4b57a6af | task-E25-report.md |
| Scoped re-review of the fixes + first read of R45 / A2 code | 21 / 21 addressed; new M F-1, F-8, F-9, F-10 | review-w1-fixes.md |
| Integration 5 A / B / C | 4 lanes + H3 merged; v8-6..v8-8; identities 1-8 PASS | integration-log.md |
| Reviews of the mining verb and the optimiser | MINE: M 10; SPO: M 1 (E-31 not self-enforced) | review-mine.md, review-spo.md |
| ERA (E-35 pooled fit, P-1, E-41, E-35a) | merged 1f2c98ab | task-ERA-report.md |
| FIX-2 (P-2, P-3, N-1, N-2, N-3, T-1, T-2, T-4) + round 1 (SPO-1/4/5, E-14a) | finished, unmerged | task-FIX-2-report.md |
| FIX-3 (F-1, F-8, F-9, F-10, F-14, minors) + round 1 (E-27b) | finished, unmerged | task-FIX-3-report.md |
| Alpha lanes RISK (R-8), COMB2 (R-10), ORTH (R-11), LIB2 (v8.2), DLRET | RISK, LIB2, DLRET merged; COMB2, ORTH unmerged | task-R-8/R-10/R-11/LIB2/DLRET-report.md |
| MINE-FIX (MINE-1..10) | in progress at the stop | task-MINE-FIX-report.md |
| Wave 0 part 1 (R1-R9) | all PASS; roles lo1 / lo3 n 5,922, 1,155 members, 1,405 sessions | integration-log.md "Wave 0 part 1" |

## 2. Lanes at the stop

| lane | pool | branch head | content | merged |
|---|---|---|---|---|
| FIX-2 | 7 | `de32b9ad` | part-2 fixes; round 1 complete: SPO-1 / SPO-5 void and refusals (ad50e574), SPO-4 (dd1b923e), E-14a (8ca8326e), fixture (f1cd0e43) | no (N-2 only, via RISK) |
| FIX-3 | 10 | `75acb091` | F-1, F-8, F-9, F-10, F-14, F-2/3/5/6; round 1 E-27b (`ew-theme-aim-v2`) | no |
| COMB2 | 11 | `1e8af5b8` | `ic-shrink-v1`, `ic-shrink-aim-v1`, r10.json | no |
| ORTH | 4 | `c1cc57ce` | `theme-resid-v1`, r11.json | no |
| MINE-FIX | 8 | `d7f98f1b` | MINE-1, 5, 4, 3, 2 done (73a0da26, 7ea78ade, 5858228f, 45a0dc9c, 631a81a0); MINE-7, 8, 6, 9, 10 remain | no |
| ERA | 3 | `ebc254f0` | merged | yes |
| RISK | 9 | `35bcda95` | merged | yes |

Merge order for integration 6 part B: FIX-2, FIX-3, COMB2, ORTH (MINE-FIX in integration 7). Part-B edits ruled:
drop ERA's `pooled_aim_weights` elif so the pooled path runs FIX-3's `theme_gain_weights` and implements
`ew-theme-aim-v2` (E-27b), un-skip the one-era equality test; delete the ew-theme-v1 / aim-v1 entries of the r10
parent map (E-45); every C++ of FIX-2, FIX-3, COMB2, ORTH is uncompiled (gtest filters in each report).

## 3. Rulings made this session (all in progress.md, all before any read)

| id | decision |
|---|---|
| PM3-1..4, 6, 8..10 | lane splits, pool reuse, review split, fix-lane 2 / ERA / FIX-3 / MINE-FIX dispatch, Wave 0 split (R1-R9 before integration 6) |
| PM3-5 / 5a | reuse-count expectation; withdrawn: identity 6 is 49 / 14; v10 from v9 49 / 21, v11 70 / 3 |
| PM3-7 | spo-v2 pin captured after identity 7 passed (no pre-R6 build) |
| E-35, E-35a | pooled fit supports ew-theme-std-v1, std-aim-v1, aim-v2 |
| E-36 | prereg rule 8 governs: marginal IC gates nothing; R-7 acceptance = rule 5 + "turnover not higher" |
| E-37 | spo-v3 takes --capacity-curve (report-only); R-9 undefined on an spo-v3 parent |
| E-27a, E-27b | gains renormalised inside each theme on any parent; new id `ew-theme-aim-v2`; aim-v1 keeps v5 |
| E-33a | campaign registry.count = new records, total beside it |
| E-14a | E-14 correlation on the traded book after d's trades vs the aim at d |
| E-31a | a primary-book run with limits_unmet > 0 on a scored decision voids itself; --spo-tol / --spo-iters refused |
| E-32a | campaign --budget mandatory; --pool mandatory; overlap-corrected hurdle (factor pinned by MINE-FIX) |
| E-38 | slots: R-8 at 48; if R-6 accepted (R-9 undefined) slots 49-51 = R-10, R-11, R-12; else R-9 runs, R-10..12 to v9 |
| E-39 | B0c's label role exists for either winner (lo1 already takes --delisting-returns since F-0; runbook R15 stale) |
| E-40 | R-8 built as registered (plan rule verbatim) |
| E-41 | history reads add 0 to N, counted apart in Appendix A |
| E-42 | exch_switch joins filing_events; roster cap 64 holds; day_rev_freq withdrawn at 0 trials if the vendor open is absent |
| E-43 | R-8 acceptance = rule 5 + realised vol in [.8, 1.2] x 5% each year (plan's "one SE" text is an expectation) |
| E-44 | R-10 / R-11 run on the last accepted parent; aim variants; mechanical criterion = R-1's turnover |
| E-45 | R-10 / R-11 defined only on an accepted R-1 parent |
| W0-n | fields v9 pins the live regsho_threshold stage; the R13 field overlap under W0-a is the check |

Trial accounting: TRAIN construction cells 37; admission trials 0; validation reads 2 (before v8); history reads 0;
2024+ never opened (one disclosure: LIB2 read availability-share columns for 2024-2026 in an atx-db doc, no statistic
used). Nothing from 2020-2023 read beyond building roles.

## 4. Verified in root this session

Builds v8-6, v8-6a, v8-7, v8-7a, v8-8, v8-9, v8-9a all clean, no /W4 /WX finding in 5 lanes' first compiles. Suites
at 48c625fc: target 253, book 154, strategy 46, ic 105, mine 18, factory 387, impl 979 / 5 skipped / 1 known
failure (ConfigJsonNotInDiscoverDigest); Python strategies 163 + 9, engine tools 252, impl tools 482 / 2 skipped,
scripts 168 / 3 skipped; tiny_world unmoved. Identities 1-8 PASS (digests in the log). Mining fixture acceptance
PASS, golden 0x889874a3b9b29c55 at 1 and 4 workers. Wave 0 R1-R9 PASS (R9 admission 1,897 MiB, slots 8). Disk 60 GB
free at the stop (46 GB after R9; the atx-db session freed space).

## 5. Open findings and risks

- E-15 stands as recorded (ADV cap at the run's initial NAV at every multiple; FIX-2 kept it). Minor: SPO-3 (NaN iterate), SPO-6 (risk model
  version unchecked), F-4, F-7, F-11, F-12, F-13, T-10 (R-1 planned turnover), C-14..C-22 untouched.
- MINE-FIX half done; nothing in v8 runs a campaign, so it is off the critical path.
- About 6,000 lines of lane C++ (FIX-2, FIX-3-none, COMB2, ORTH) uncompiled until part B.
- RISK: the registered rule scales the aim, so realised vol is expected near 5% / 1.247 under aim-partial-v5; the
  band test may fail for that reason (E-43: not re-parameterised). Check the v1.1 risk store's capped_specific
  count before R-8.
- E-27a / E-27b cap: a fit with fewer than 2T admitted members is refused on any std or aim-v2 composition.
- nav_summ multi-dir re-runs: a re-run cell is scored alone (procedure, FIX-3 concern 3).
- Host memory about 3.5 GB free while the atx-db session runs; the R-1 weighted pass needs about 2.6 GiB (E-28).

## 6. Pitch determination (unchanged from status 2)

No v8 metric exists; a pitch with improved numbers is not justified. Earliest re-based pitch: after B0c (step 6 of
the goal prompt); improvement pitch: after V8-F (step 8). The platform status that can be stated honestly grew:
every review finding that could bias a number is fixed, the research tooling self-enforces E-31 and the ledger
rules, and four more registered levers (R-8, R-10, R-11, R-12) exist behind flags with synthetic-data tests.
