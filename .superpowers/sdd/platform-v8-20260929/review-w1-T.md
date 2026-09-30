# Review W1 part 2, area T: every test file of the v8 work (41ac94fd..81abfa12)

Snapshot `C:/atx-wt/pool-2` at 81abfa12 (code identical to 237486fe). Read-only: nothing built, no C++ test run, no
real data, no repository file edited. Two claims were checked with Python probes on the synthetic test fixtures
(scratchpad scripts that patch a function in memory only; noted where used).

Scope: the 33 paths of `git diff --stat 41ac94fd 81abfa12 -- '*test*' '*tests/*'`. That is 32 test files plus
`atx-impl/tools/backtest_integrity.py`, which the glob matches but which is a tool (read as the diff for context; area
P owns it). Also read: `scripts/tests/test_research_ledger.py`, outside the range, because the brief names its DSR N
test. Test files of the earlier wave-1 lanes (merged before 41ac94fd, for example `strategy_nav_replay_test.cpp`) are
outside this brief's range and were not reviewed.

Severity: **I** important, **M** medium, **m** minor. No I finding: no test hides a defect that the code under test is
known to have. The four M findings are tests that cannot fail for the behaviour their brief says they pin, on a path
that feeds a trial cell.

---

## Findings

### T-1 (M) ew-theme-std-v1: no runner test can fail for a wrong rule 1 (the weighted mean of signed member ranks)

- Where: `atx-impl/tests/strategy_ic_composition_test.cpp:362-381` (V8Fixture: every sign +1; theme b members
  b1 = j, b2 = (j + 8) % 9, b3 = (8 j) % 9), `:399` (`OneMemberThemeHasSameDispersionAsOthers`), `:429`
  (`MemberCapRedistributes`, weights {.5, 1/6, 1/6, 1/6} and {.25, .25, .25, .25});
  `atx-engine/tests/combine/combine_group_rerank_test.cpp:93-116` (`EveryGroupEntersWithTheSameDispersion`);
  `atx-impl/tests/strategy_ic_runner_test.cpp:2925` (the rerank-on run is only asserted to differ from v1).
- What is wrong: the three b members' centred ranks sum to exactly b1's rank on every name, so the theme composite is
  b1's rank divided by 3, in b1's order. Within-theme weights are equal and every sign is +1. So a standardise path
  that ranks only the first member of each theme, drops the sign s_k, or drops the within-theme weight w_k gives the
  reference blend `v8_ref_blend` exactly, on both fixtures and both weight vectors. Only the overwrite bug (last
  member wins) is caught. The GroupRerank "same dispersion" case has the same shape (composite = single / 3). The
  runner's rerank-on case checks only `on != v1`. Rule 1 of the registration is the part R-1 adds, and no test of it
  can fail. The code (`strategy_ic_composition.cpp:282-318`: `sign * weight * r` accumulated per member) is correct by
  reading.
- Blocks: the R-1 cell (N 41) and every later cell on this composition (the E-27 aim variant for R-3). Also the
  brief's claim that `OneMemberThemeHasSameDispersionAsOthers` and `MemberCapRedistributes` pin the rule.
- Fix: use members that disagree in order (for example b2 reversed on half the names), one member with sign -1, and
  unequal tier shares inside a theme (.8 / .55 / .4). Compare against `v8_ref_blend`. In the runner test, assert the
  rerank-on blend against an independent reference, not only inequality.
- Verified: a Python port of the fixtures and of `v8_ref_blend` (scratchpad `comp.py`). Max |difference| from the
  reference is 0 for first-member-only, unsigned and unweighted, and .44 / .66 for last-member-wins. C++ not run.

### T-2 (M) gscore7_lowbm: neither the oracle nor the point-in-time test can see which session's me_company is read

- Where: `atx-engine/tools/test_research_fields_v8_quarters.py:67` (`close_of`: every line's price grows by the same
  factor 1 + .0005 i); `:385-413` (the point-in-time test scales every line's raw_close by 1.9 from row CUT); `:344`
  (the oracle comparison). Code: `atx-engine/tools/research_fields_v8.py:481-496`.
- What is wrong: bm = be / me_company enters the G-score only through the bottom-tercile membership, that is the rank
  order and the 1/3 quantile. In this world me_company(t) / me_company(t-1) is one factor common to every name, and
  the mutation scales every line alike. So the tercile is the same whichever session's me_company is used.
  Probe: `gscore_rows` patched in memory to read me_company of session t (E-20 registers t-1), and of session t+1 (a
  look-ahead). Both patches pass `test_values_equal_the_definitions` and
  `test_field_at_t_unchanged_when_rows_after_t_mutate`. A sanity patch (every value NaN) fails both. The test's
  comment at `:409` ("me_company moved at t itself: ... gscore reads t-1 only") claims exactly the guard it cannot
  give. The code reads t-1 (correct by reading).
- Blocks: E-20's rule "bm uses me_company at t-1", the hidden-data discipline of F-C (member `gscore_lowbm` of R-7),
  and fields v11.
- Fix: give each line its own drift, and in the point-in-time test scale one line's raw_close from row CUT, not all of
  them. The oracle and the mutation then separate t-1 from t and from t+1.
- Verified: probe run, scratchpad `probe_gscore_lag.py`, modes nan_all (2 failures), same_day (0), lookahead (0).

### T-3 (M) The spo-v2 identity pin is the placeholder 0, so `SpoV3.V1AndV2DigestsUnchanged` skips on every run

- Where: `atx-impl/tests/strategy_spo_v3_pin_test.cpp:40-41` and `:80-83` (GTEST_SKIP). Contrast
  `strategy_spo_pin_test.cpp`, where SpoPin fails (ADD_FAILURE) when its pins are unset.
- What is wrong:
  - R-6 extracted `fixed_positions`, `market_terms` and `accumulate_plan` from the spo `plan()`, and E-26 changed the
    v7 parser.
  - spo-v1 is pinned (SpoPin). spo-v2 is not.
  - The integration log records v2 digests printed by the R6 head and by v8-5. Both builds are post-R6, so their
    equality says nothing about the extraction.
  - The skip is counted in every summary ("222 passed, 1 skipped") and blocks nothing.
  - The capture protocol is feasible. Every API it uses exists at 41ac94fd (checked with git grep: `v2_params`,
    `diagnostics_csv`, `spo_engine`, `calibration`, `fixed_nav_scenarios`, and the fixture's `write_risk_model` and
    `Role`). It needs a pre-R6 build in a pool tree.
- Blocks: plan R-6 step 3 ("identities: spo-v1 and spo-v2 digests") before the R-6 cell. Also W0-4 step 4, which
  re-runs the ledgered spo-v2 cell on the 4-year role with the post-R6 exe as a window re-run that adds 0 trials. If
  the extraction moved spo-v2 bytes, that re-run is another rule counted as the same trial.
- Fix: capture on a pre-R6 pool build. Until then, make the unset pin fail (as SpoPin does) instead of skipping.
- Verified: by reading the test and the integration log (v8-4e and v8-5 rows), and by git grep of the pre-R6 API.

### T-4 (M) spo-v3 solver and rule: nothing independent checks the factor-covariance path, the outside gap or gamma's units

- Where:
  - `atx-engine/tests/book/book_target_tracking_test.cpp:91-297`: the nine TargetTracking tests.
  - `:50` and `:145`: `external_gap` is 0 in every engine test.
  - `:154`: the golden-section reference minimises the code's own `tracking_terms`.
  - `atx-impl/tests/strategy_spo_v3_test.cpp:192-199` and `:270-272`: gamma and TE are compared only with the
    engine's own `aim_vol` and `tracking_error_current`.
- What is wrong: no test compares a multi-name optimum with nonzero group and style loadings to an independent
  solution. Per test:
  - ZeroCost: w = a under any positive semi-definite metric.
  - Diagonal: F = 0.
  - OneName: intercept only.
  - HigherCost: monotone under any metric.
  - LimitsAndBoxes: checks constraints, and the beta edge (true for any positive definite metric), against a free
    solve by the same solver.
  - TradeLimitShare, Indefinite (eigenvalue count of F alone) and Repeats: no optimum check.

  So a solver or `tracking_terms` that dropped the group one-hot columns, mis-signed `external_gap`, or computed
  sigma_aim without the sqrt(252) annualisation (gamma 15.9 times too large) passes every engine test and every SpoV3
  test. The E-14 criterion (aim correlation) and the TE the cell reports go through the same code. Area A checked the
  algebra by reading; nothing pins it.
- Blocks: the R-6 cell (N 46), the E-14 criterion reading and the E-31 convergence counts.
- Fix:
  - n = 6, intercept + 2 groups + 1 style, zero costs, net equality. Check against the closed form
    w = a - S^-1 1 (1' S^-1 1)^-1 (1'a - net), with S = B F B' + D built densely in the test.
  - A case with nonzero `external_gap`.
  - A KKT residual check with the costs on.
  - At the rule level, assert aim_vol = sqrt(252 a' S a) from a dense S of the fixture risk store.
- Verified by reading the tests. C++ not run.

### T-5 (m) The R-1 identity test is tautological: `rerank: false` is the plain pinned path

- Where: `atx-impl/tests/strategy_ic_runner_test.cpp:2925-2975`; the R-1 report ("identity device").
- What is wrong: "v1" and "off" both run the plain pinned-weights path; the test asserts their recipes are equal
  apart from the weights pin. Neither the test nor the R-1 identity cell (plan step 3) executes any line of
  `add_standardised` or `add_group_rerank`. The identity proves that the parser ignores a block with `rerank: false`,
  not that the new path is correct. The lane disclosed the device. This finding is listed so the identity cell is not
  read as evidence for the rule. The rule's evidence would be T-1's fixture.
- Verified.

### T-6 (m) Identity tests compare two paths of one build; no committed pin ties flag-off output to the pre-change bytes

- Where: `atx-impl/tests/strategy_live_test.cpp:1606` (`HoldBand.ZeroBandNavRunIsByteIdentical`), `:1901`
  (`EmittedHoldingsUnchangedWithoutADeclaredBand`), `:2052` (`AdvHold.LargeQIsByteIdentical`);
  `strategy_target_replay_test.cpp` (`HoldBand.ZeroBandIsByteIdenticalToV5`); `strategy_spo_v3_test.cpp:536`
  (`ShapingFlagsAbsentIsByteIdentical`).
- What is wrong: each test compares the flag absent with the flag at its neutral value, inside one build. That the
  flag-absent output equals the output from before the change rests on reading the code and on root identities
  i1-i7, none of which had run at 81abfa12 (integration log: "Identities: none run"). spo-v3 has no digest at all:
  any change to the solver or to the aim path before the R-6 cell changes the registered cell with no test failing.
- Fix: run i1-i7 before the first cell. Add a spo-v3 replay digest (the `replay` procedure of
  `strategy_spo_digest.hpp` on Role(40, 12, 53)).
- Verified: reading and the integration log.

### T-7 (m) Hold-band and ADV-cap tests: three gaps

- (a) `atx-engine/tests/book/book_target_shaping_test.cpp:155-180`: the refusal lambda takes `HoldBandState` by value
  and compares only the desired row. "Refused before anything changes" is therefore not checked for the state, the
  part that carries across decisions.
- (b) `atx-impl/tests/strategy_live_test.cpp:1772` (`GridSharesTheBandAndOneCadence`) refuses a capped variant against
  an uncapped base. No grid test has two capped variants with different `--aim-leverage`, so A-1's failing case is
  untested (cite A-1; FIX-AB owns the fix and should add it).
- (c) No `decide --check-replay` parity test runs under `--adv-hold-q`; `CheckReplayMatchesEmittedHoldings` covers
  the hold band only. `AdvHold.DeployPinAndDecisionRecord` checks the pin and the decision.json keys, not parity.
  Minor: v8 deploys nothing (E-16).
- Verified.

### T-8 (m) The DSR N test pins a same-window "window" re-run at 0 trials; the cycle tests pin summ argv without `--dsr-ledger` / `--protocol v8`

- Where: `scripts/tests/test_research_ledger.py:68-83`. Line "prior/c" has `rerun_of` a and `rerun_basis` window, but
  the same `research_window_id` and the same 2020 sessions as a; the expected trial_counts are
  [1, 0, 0, 1, 1, 1, 0]. Also `scripts/tests/test_research_cycle_roles.py:236-238` and
  `scripts/tests/test_research_cycle.py:1264-1274`.
- What is wrong: v8-prereg item 2 exempts only re-runs on the longer window. The fixture's window line is on the same
  window, so the named DSR N test asserts C-4's defect as correct behaviour. The two cycle tests pin a `--dsr-n` summ
  argv without `--dsr-ledger` or `--protocol v8` (C-1, C-2).
- Consequence for FIX-C: its fixes will break these three expectations. They must be rewritten with the fix, not
  re-pinned to whatever the code then prints. Read literally, FIX-C's binding rule for C-3..C-5 ("unless the rerun
  line names a `rerun_of` ... with a `defect:` line") would also refuse the W0-4 step 4 window re-runs, whose targets
  carry no defect. The rewritten test should keep one legitimate window re-run, on another window id.
- Verified by reading. C-1, C-2 and C-4 are part 1's findings.

### T-9 (m) The Appendix A tests read k from a ledger line kind that no production writer emits

- Where: `atx-impl/tools/test_mega_report_v8.py:142` (`rec(99, 'admission', 7)`), `:540-548`;
  `atx-impl/tools/test_mega_report_v8_render.py:301` ("admission trials this sprint 7 plus 8 re-screens").
- What is wrong: the rendered "7 plus 8" comes from a synthetic admission line. Per C-7, nothing in the cycle writes
  such a line, so the real render will print "0 plus 8". The tests prove the renderer, not the count.
- Fix, with C-7: a test that runs `run --screen` on the fixture and renders the resulting ledger.
- Verified by reading. C-7 is part 1's finding, partly unverified there.

### T-10 (m) R-1's mechanical criterion reads executed turnover, not the registered planned turnover

- Where:
  - `docs/plans/mega-alpha-v8-pitch.config.json:73` and the test config in `test_mega_report_v8.py:197`.
  - Plan section 9, R-1 (line 698): "planned turnover per unit gross not higher than the parent's". Sections 7 and 9
    are the registration by v8-prereg item 11.
  - nav_summ's `tau_gmv_mean` is `one_way_turnover_gmv` over executed sessions (`nav_summ.py:12`, `:259-266`).
- What is wrong: the ladder computes R-1's criterion chip from `tau_gmv_mean / mean_gross_leverage_all_rows`. Plan
  12.1's table says only "turnover per unit gross", and the config's text comes from there. Executed turnover differs
  from planned turnover by the fill mechanics (caps, blocked orders, drift). Area P owns the config; the finding is
  listed here because the report tests adopt the substitution.
- Fix: before R-1 is read, a ruling that names the statistic, or a planned-turnover metric (the daily CSV's
  `planned_turnover`) in nav_summ and in the config.
- Verified by reading.

### T-11 (m) The pool test pins the pooled line out of V[SR], even when every era lies inside TRAIN

- Where: `atx-impl/tools/test_nav_summ_pool.py:268-269`: `(cells, legacy_cells) == (0, 1)` for a pool of two TRAIN
  eras that carries the window id. Code: `backtest_integrity.dsr_variance`.
- What is wrong: v8-prereg item 3 takes V[SR] over "the ledgered cells re-run on 2020-2023 plus the v8 cells". A
  pooled cell adds 1 to N but never enters V. The H-1 report lists this as a deviation, and no ruling covers it (E-17
  rules on frozen-book semantics only). No OD-3 read happens in v8, so no v8 number moves. It needs a ruling before
  any pooled read.
- Verified.

### T-12 (m) Field seal tests run under the conftest rebinding

- `atx-engine/tools/test_research_fields_v8.py:32` and `:386`: `tool.SEAL == rw.SEAL` compares two values that
  `atx-engine/tools/conftest.py` rebinds to the superseded 2025-01-01. A builder that hard-coded 2025-01-01 would
  pass; one that hard-coded the correct 2024-01-01 would fail. The real guard is `test_research_window.py`'s
  fresh-interpreter check of `prepare_research_fields.Role` (cite B-10, C-19).
- `test_research_fields_v8_quarters.py:485-486` pins the new v8 module's counter as
  `rows_available_on_or_after_2025_dropped` under a 2024 seal: B-9's misnomer carried into a new producer's
  `source_checks` (cite B-9; rulings W0-k and E-11 make it metadata).
- Verified.

### T-13 (m) The E-19 plan-rows test runs, by default, on a hand-written plan derived from the recipe it is compared with

- Where: `atx-impl/strategies/test_generate_library.py:28` (PLAN = `ATX_V71_PLAN_JSON` or
  `alphas/fixtures/v71_plan_k1.json`, "written by hand from contract K1 and the v7.1 recipe"), `:61-90`.
- What is wrong: every default run compares the recipe with a hand copy of itself. The real exe plan
  (`build-equity/v8-i3-plan-v71.json`, which differs on five values) was checked once by lane A2 and is not
  committed, and no runner sets the environment variable. The per-member node bound `0 < node_count <= max` is
  already implied by the equality of the maxima asserted just above it.
- Fix: commit the real K1 plan JSON as the fixture; it holds no data statistic.
- Verified.

---

## Table of findings

| ID | sev | file:line | one line |
|---|---|---|---|
| T-1 | M | `strategy_ic_composition_test.cpp:362` | std-rule fixtures pass a first-member-only, unsigned or unweighted theme composite |
| T-2 | M | `test_research_fields_v8_quarters.py:67`, `:385` | uniform price paths: me_company of t or t+1 passes the gscore oracle and PIT test |
| T-3 | M | `strategy_spo_v3_pin_test.cpp:40` | spo-v2 digest placeholder 0; the identity test skips on every run |
| T-4 | M | `book_target_tracking_test.cpp:91-297`, `strategy_spo_v3_test.cpp:192` | no independent reference for the factor path, external_gap, gamma units |
| T-5 | m | `strategy_ic_runner_test.cpp:2925` | R-1 identity runs the plain path only; proves the parser |
| T-6 | m | `strategy_live_test.cpp:1606`, `:1901`, `:2052`; `strategy_spo_v3_test.cpp:536` | identities within one build; i1-i7 not run; no spo-v3 digest |
| T-7 | m | `book_target_shaping_test.cpp:159`, `strategy_live_test.cpp:1772` | state unchecked on refusal; A-1 grid case and ADV check-replay untested |
| T-8 | m | `test_research_ledger.py:68`, `test_research_cycle_roles.py:236` | DSR N test pins a same-window window re-run (C-4); argv pinned without C-1/C-2 flags |
| T-9 | m | `test_mega_report_v8.py:142` | Appendix A k from a synthetic admission line (C-7) |
| T-10 | m | `mega-alpha-v8-pitch.config.json:73`, `test_mega_report_v8.py:197` | R-1 criterion on executed, not registered planned, turnover |
| T-11 | m | `test_nav_summ_pool.py:268` | pooled line kept out of V[SR]; no ruling |
| T-12 | m | `test_research_fields_v8.py:386`, `test_research_fields_v8_quarters.py:485` | seal equality vacuous under conftest (B-10); 2025 counter name (B-9) |
| T-13 | m | `test_generate_library.py:28` | E-19 test compares the recipe with a hand copy of itself by default |

Counts: I 0, M 4, m 9.

## Pins and identities (which pins are real, which are placeholders)

| pin or identity | where | state |
|---|---|---|
| SpoPin spo-v1 weights 0xda6b6871e7e267c5, replay 0xaabdbb72f99a6e13 | `strategy_spo_pin_test.cpp:43-44` | real (captured pre-W1b); the refactor into `strategy_spo_digest.hpp` keeps the fold order |
| SpoV3 spo-v1 (= SpoPin's) | `strategy_spo_v3_pin_test.cpp:36-37` | real |
| SpoV3 spo-v2 | `strategy_spo_v3_pin_test.cpp:40-41` | **placeholder 0, skips** (T-3) |
| spo-v3 digest | none | absent (T-6) |
| FactoryOos version_id 916304603 and 3123399341 | `factory_oos_test.cpp:897`, `:1597` | re-pinned under E-22a after the LegacyBandsV1 probe restored the old pins (integration 4 part A) |
| mega_report v7 golden c42c5fad..., 109,868 bytes | `test_mega_report_v8.py:38-39` | recorded by the lane at ec47b8ce (before v8.py); not re-derived here |
| v7 pitch config LF SHA 73f80583... | `test_mega_report_v8_render.py:41` | real |
| hold band 0, ADV Q 1e9, E-16 export, spo-v3 flags absent | T-6 list | real tests within one build; old-vs-new (i1-i7) not run |
| R-1 identity (rerank false) | `strategy_ic_runner_test.cpp:2925` | tautological (T-5) |
| E3 alone = single-role cycle; one-era pool = single | `test_research_cycle_roles.py:277`, `test_nav_summ_pool.py:117` | real, byte level (plan lines, calls, every output byte, ledger) |

## Checked, no finding

- Hold band: the boundary (|dr| = b is kept), first set, an ineligible day, the zero-band identity, a hand-computed
  held row (`NameInsideBandKeepsDesired`), a missing day, the decide chain against the replay, and check-replay from
  both export layouts with a negative control (stateless positions give mismatches).
- ADV cap: residual breach recomputed from the execution ADV; side gross and one common factor per side; large-Q
  identity in every daily and events CSV; the E-15 capacity pass; refusals with Q = 0 controls. The ADV window itself
  is pinned independently by the pre-existing `strategy_nav_replay_test.cpp:670-690`, so reusing `execution_adv` in
  `capped_decision` is not circular.
- Era pooling (era_pool, pooled fitter, nav_summ `--pool`, ledger lines, roles loop, era audit):
  - refusals cover order, overlap, straddle and the seal; the repository window is read from its JSON, so the conftest
    rebinding does not affect it;
  - the pooled admission equals screen_v4 on the concatenated factors, with an independently written tau formula;
  - the audit proves with an audit hook that close, raw_close and a return field are never opened, and that no value
    key or sentinel appears;
  - pooled ledger counts are [0, 0, 1];
  - an identity re-run appends nothing.
- Composition rules (Python): tier shares, the cap cascade against a loop port, the regrade table, registry
  precedence, the fitter end to end and the identity graft.
- F-A, F-B, F-D and E-21:
  - oracles written from the definitions;
  - point-in-time mutations with a clock exactly at the mark and one hour after; window edges; 22:30 UTC; amendments;
  - EPS rule boundaries (|g0| = 6, a zero denominator, exactly 12 finite);
  - QuarterIndex against brute force, the tie included;
  - E-21 reuse: mixed, self, chained, a producer edit, a pre-E-21 prior, and the refusal before any output.
- Report:
  - the v7 golden digest and the with/without-registration differential;
  - one owner block per input (each input removed in turn);
  - the content seal on years and `session_ns`;
  - ladder checks;
  - freeze-gate items, with the boundary p = .10 correctly not met;
  - E-11 document roots, and memmap payloads seal-checked before the stat.
- DSR N test otherwise: N equals `trial_counts` + 1 at plan time and at nav_summ time, and an identity run under
  another name counts once.
- Tolerances: none found wide enough to hide a defect. The largest are 1e-8, where registered (spo-v3 and solver);
  elsewhere 1e-12 to 1e-15. `GrossCapIsSlackOnFixture`'s .75 x 2L is reasoned, not measured, and bounds one side only.
- Pinned part-1 behaviour (not repeated as findings): `test_mega_report_seal.py:20` reads the all-digit 16-character
  part "2024"x4 as a hash (C-20a); `test_book_diagnostics.py:230` pins the median fee (25 bps) for unrated shorts
  (C-18); the spo-v3 aim correlation is the planned book's (A-4); no spo-v3 test runs with `--warm-start-sessions`
  (A-2: only `NavV7Hook.SideFilesExcludeWarmUp`, spo-v2); the price reuse test's prior includes `ceq_iss_5y`, so the
  panel extent of B-12 and the imported calendar of B-1 are not exercised. The pooled fit does not refuse
  `ew-theme-std-v1` and runs the rule (probe `probe_pool_std.py`: reached the rule's own cap refusal on the pool
  world), but no test covers it; it is the integration log's open item.

## Coverage

| file | lines read | result |
|---|---|---|
| `atx-engine/tests/book/book_target_shaping_test.cpp` | 1-296 (new) | T-7a |
| `atx-engine/tests/book/book_target_tracking_test.cpp` | 1-309 (new) | T-4 |
| `atx-engine/tests/combine/combine_group_rerank_test.cpp` | 1-131 (new), and `group_rerank.hpp` 1-88 | T-1 |
| `atx-engine/tests/factory/factory_oos_test.cpp` | diff (+12: two re-pins, 888-897, 1588-1597) | clean (E-22a followed) |
| `atx-engine/tools/test_era_pool.py` | 1-131 (new) | clean |
| `atx-engine/tools/test_prepare_research_fields_module_reuse.py` | diff (+8) | clean |
| `atx-engine/tools/test_prepare_research_fields_sec.py` | diff (+3, oracle key) | clean |
| `atx-engine/tools/test_research_fields_price.py` | diff (+76: E-21 test, producer order test) | clean (B-1, B-12 not exercised) |
| `atx-engine/tools/test_research_fields_v8.py` | 1-402 (new) | T-12 |
| `atx-engine/tools/test_research_fields_v8_quarters.py` | 1-595 (new); probe run | T-2, T-12 |
| `atx-impl/strategies/test_generate_library.py` | diff (+17), 1-60, 90-140 | T-13 |
| `atx-impl/tests/CMakeLists.txt` | diff (+5) | clean |
| `atx-impl/tests/strategy_ic_composition_test.cpp` | diff (+187: 297-486); source 160-340 | T-1 |
| `atx-impl/tests/strategy_ic_runner_test.cpp` | diff (+138: 2897-3038) | T-1, T-5 |
| `atx-impl/tests/strategy_live_test.cpp` | diff (+717: 188-340 fixture edits, 1509-2210) | T-6, T-7b/c |
| `atx-impl/tests/strategy_spo_digest.hpp` | 1-148 (new); pre-R6 API checked by git grep | clean |
| `atx-impl/tests/strategy_spo_pin_test.cpp` | diff (refactor onto the header) | clean |
| `atx-impl/tests/strategy_spo_test.cpp` | diff (+71: `NavV7Hook.SideFilesExcludeWarmUp`) | clean (A-2 untested for spo-v3) |
| `atx-impl/tests/strategy_spo_v3_pin_test.cpp` | 1-87 (new) | T-3 |
| `atx-impl/tests/strategy_spo_v3_test.cpp` | 1-633 (new) | T-4, T-6 |
| `atx-impl/tests/strategy_target_replay_test.cpp` | diff (+203: 1229-1434) | clean |
| `atx-impl/tools/backtest_integrity.py` | diff (+170; a tool matched by the glob, area P) | context only |
| `atx-impl/tools/test_book_diagnostics.py` | 1-345, 520-643 read; 345-520 (Cell fixture builder) skimmed | clean (pins C-18) |
| `atx-impl/tools/test_composition_rules.py` | 1-274 (new) | clean |
| `atx-impl/tools/test_era_data_audit.py` | 1-236 (new) | clean |
| `atx-impl/tools/test_fit_composition_weights.py` | diff (+3) | clean |
| `atx-impl/tools/test_fit_composition_weights_pool.py` | 1-226 (new); probe run | clean |
| `atx-impl/tools/test_mega_report_seal.py` | diff (+90) | clean (pins C-20a) |
| `atx-impl/tools/test_mega_report_v8.py` | 1-626 (new) | T-9, T-10 |
| `atx-impl/tools/test_mega_report_v8_render.py` | 1-140 read; 140-452 every test and assertion read, fixture writers skimmed; committed config cells and gate printed | T-9, T-10 |
| `atx-impl/tools/test_nav_summ_pool.py` | 1-304 (new) | T-11 |
| `scripts/tests/test_research_cycle.py` | diff (+134); 1255-1290 (`test_dsr_n_from_ledger`) | T-8 |
| `scripts/tests/test_research_cycle_roles.py` | 1-416 (new) | T-8 |
| `scripts/tests/test_research_ledger.py` (outside the range; the brief's DSR N test) | 1-128 | T-8 |

34 rows. Context read: the part-2 brief; `global-constraints.md`; `v8-prereg.md`; `progress.md` (E-1..E-34, W0-a..m);
`review-w1-A/B/C.md`; the FIX-AB and FIX-C briefs; the briefs and reports of R-1, R-4, R-6, H, F-3, G and REPORT
(the R-5 report was not read; its tests were read against plan section 9 and the R-4 report's E-15/E-16 notes); plan
sections 7 (W0-4), 9, 10, 11 and 12.1; integration-log sections "integration 4" parts A and B.
