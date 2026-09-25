# Lane W0-E0a review

## Verdict

APPROVE. There are no blocker or major findings. There are five minor findings and two
owner waivers or confirmations, listed below.

## Reviewed SHA

`1db2bb43d5afcba5254cd22a28bbe94e4c96c964` on `feat/w0-e0a`.
- The merge base with `feat/w0-integration` is `458d0bef480a624e258070c9d45174a9984466bf`, which is the W0 base.
- The tree was clean before this review file was added.

## Evidence

All commands were run by the reviewer in `C:\atx-wt\pool-5`. Free RAM was 5.15 GB before the build. `CMAKE_BUILD_PARALLEL_LEVEL=2`.

1. Build: `scripts\atx-build.ps1 build -Preset equity-dev atx-engine-eval-tests atx-engine-combine-tests`.
   Result: `[9/11] Linking CXX executable bin\atx-engine-eval-tests.exe`, `[10/11] Linking CXX executable bin\atx-engine-combine-tests.exe`, `build_exit=0`.
2. `/W4 /WX` single-TU checks, all exit 0 with no warnings or errors. The files were:
   - `src\eval\cross_section_ic.cpp`
   - `src\combine\signal_combiner.cpp`
   - `src\combine\orthogonalize.cpp`
   - `tests\eval\eval_w0e0a_hac_test.cpp`
   - `tests\combine\combine_w0e0a_hac_tstat_test.cpp`
   - `atx-impl\src\stage_equity_ic.cpp`. This is the consumer. It still compiles against the new header.
3. Anchored runs with `-Ctest -Preset equity-dev -R`, all exit 0:
   - `^EvalHac`: 14/14
   - `^EvalIcCoverage`: 3/3
   - `^EvalIcDelay`: 6/6
   - `^CombineHacTstat`: 6/6
   - `EvalIcCaps` (4 tests) ran inside the whole executable in step 4.
4. Whole executables with `--gtest_brief=1`:
   ```
   atx-engine-eval-tests.exe     [==========] 222 tests from 29 test suites ran. (99342 ms total)  [  PASSED  ] 222 tests.  eval_exit=0
   atx-engine-combine-tests.exe  [==========] 183 tests from 34 test suites ran. (7267 ms total)   [  PASSED  ] 183 tests.  combine_exit=0
   ```
   The base had 195 eval tests and 177 combine tests. The lane added 27 eval tests and 6 combine tests. No test was removed.
5. Measurement lines reproduced verbatim. These match the report:
   ```
   W0E0A_FROZEN_DIGEST variant=1 restriction=0 digest=0x3eff877612745386   (and the other three base digests, all equal)
   [EvalHac] statsmodels fixture: 27 cases, max |t - t_sm| = 1.066e-14, max |se/se_sm - 1| = 1.739e-15
   [EvalIcCoverage] MA(20), n=1750, 2000 reps: HansenHodrickV1 0.9450  NeweyWestV1 0.9135  naive-IID 0.3145
   [EvalIcDelay] same-day reversal ... delay 0 IC 0.4852 ... delay 1 IC 0.0029 (HAC t 0.58)
   [EvalIcCaps] 6624 x 1750 computed: dates emitted 1728, n_used/date 6624, IC mean 0.22512, HAC t 13.552
   [CombineHacTstatStoreWinsor] winsor 3: max |z| ZScore 3.0000, ZScoreRestandardizeV1 4.2426
   ```
6. The statsmodels fixture was independently regenerated. The committed generator was copied to the scratchpad and run there with `py -3.12` (statsmodels 0.14.1, numpy 1.26.4). The output is byte-identical to the committed `hac_statsmodels_fixture.hpp`, with 27 HAC cases. A separate check confirmed that statsmodels honours `cov_kwds={'kernel': 'uniform'}`: at lag 20 the uniform and Bartlett standard errors differ (0.00680 vs 0.00579). So the uniform cases are real statsmodels output.
7. The frozen digests were measured on the base.
   - Commit `2ba5802b` is parented on `458d0bef` and adds only `eval_w0e0a_frozen_v1_test.cpp`, with the four digests.
   - `git diff 2ba5802b HEAD` on that file adds only the two V1 config lines and the new defaults test. The digest fold was not narrowed.
8. Independent Monte Carlo of the production circular-block percentile CI. This is numpy, 2000 reps, B = 2000, n = 1750, on equal-weight MA(20) data:
   - L = 11 (HalfHorizonV1): 0.8090
   - L = 42 (TwoHorizonV2, the default): **0.9250**
   - L = 63: 0.9315

   See finding 1.

## Findings

| path:line | severity | problem | required fix |
|---|---|---|---|
| `atx-engine/tests/eval/eval_w0e0a_ic_coverage_test.cpp:144-145`; `cross_section_ic.hpp:210` | minor | The [93%, 97%] coverage item is met on the new HAC interval `ic_mean_hac` (94.5%, 2000 reps). The wiring to `make_hac` is pinned exactly by `CrossSectionIc_HacFieldsAreHacOfTheEmittedIcSeries`. The E-02 bootstrap interval `ic_mean_ci` under the default `TwoHorizonV2` still under-covers: the reviewer measured 92.5% independently at n = 1750. The bootstrap test only asserts `v2 > v1 + 0.05`, so any regression in the default bootstrap's coverage level is not pinned. The lane disclosed this honestly (Deviation 2). | Owner confirmation that the HAC interval satisfies the item (see waiver_needed). Optionally add an absolute lower bound on V2 bootstrap coverage, for example `>= 0.90`, so a later block-rule change cannot regress silently. |
| `atx-engine/include/atx/engine/eval/cross_section_ic.hpp:835-837` vs `atx-engine/src/eval/cross_section_ic.cpp:963` | minor | Contract mismatch. The `preflight_cross_section_ic` doc says it checks "the horizon count and bootstrap draws are within their maxima". The implementation does neither; only `validate()` does. A standalone preflight with `horizons.size() > kMaxIcHorizons` or `bootstrap_draws > kMaxBootstrapDraws` is accepted if the budget allows. The byte arithmetic is overflow-checked, so there is no UB. | Either add the two checks to preflight or remove the claim from the doc comment. |
| `atx-engine/src/combine/signal_combiner.cpp:183,198`; report "Integration notes" | minor | E-15 changed the combiner-visible numerics through `ic_matrix(s, w)`, whose default is now `WinsorizedV2`. This affects GK and ICIR-EWMA weights, not only t-stats; the zoo IR print moved. `RawV1` cannot be selected per combiner. The report's Deviation 1 and the combiner integration note mention only `tstat_rule`. | Extend the integration note: the combiner-struct owner must also add an `IcReturnTreatment` field threaded to `ic_matrix`, beside `tstat_rule`. |
| `atx-engine/src/combine/signal_combiner.cpp:54` (`kCombineTStatRule`) | minor | Newey-West at the NW-1994 automatic lag does not know the horizon. On an MA(20) null at T = 500 it still rejects 13.3% of the time against a nominal 5% (`CombineHacTstat` print). The test bound `< 0.15` accepts that. E-03 is materially fixed (IID rejected 67.2%), but the combine sites stay about 2.7x liberal. | Record this as a known limitation. When the combiner headers gain `tstat_rule`, consider a horizon-aware floor, lag >= h - 1, as the eval side already uses. |
| `atx-engine/include/atx/engine/eval/hac.hpp:339-341` | minor | The doc says `politis_white` is "the `arch` package's `optimal_block_length`". The code follows the paper's m^ rule (lags m+1..m+K_N normalized by R(0)). arch tests lags m..m+K_N-1 with a different correlation normalization, so m^ differs by one lag. The test reference is a numpy transcription of the same formula, not arch, so arch parity is not proven. | Reword the doc to "Politis-White (2004) / Patton-Politis-White (2009)" without claiming arch parity, or verify against arch. |

## Checked

- [x] `.agents/cpp/agent.md` §10 checklist applied to the diff.
  - No UB found. Every lag loop is bounded by n - 1. `politis_white` indices stay ≤ m_max ≤ n - 1. `horizons.back() + execution_delay` is bounded before any subtraction.
  - The `make_hac` `horizon - 1` subtraction is safe because horizon ≥ 1 is validated. The preflight u64 arithmetic is overflow-checked.
  - `winsorize_row_copy` is bounded by both spans. `bootstrap_mean_interval` guards `block_len == 0` (reason 3) and a short `draw_stats`.
  - Switches are exhaustive (`kernel_weight`, `mean_tstat`, `block_len_for_rule`, rule validation). Out-of-range enum casts are rejected, and tests cover that.
  - Every changed TU compiled cleanly under `/W4 /WX`.
  - `column_tstats` takes a span over `x.col(c).data()`. This is valid because `MatX = Eigen::MatrixXd` is column-major (`linalg.hpp:48`) and the parameter is a materialized `const MatX&`.
- [x] Every acceptance item was checked against real output and the test code.
  - NW vs statsmodels at 1e-8: MET. The fixture regenerates identically. Observed error is 1e-14.
  - MA(20) coverage: MET on the HAC interval (94.5%). See finding 1 and the waiver.
  - Same-day reversal: MET. Delay 0 IC is 0.485; delay 1 IC is 0.003 with HAC t 0.58. The fixture is the right one: r_{t+2} is independent of the signal.
  - 6,624 × 1,750: MET end to end. Preflight scratch bytes equal the plan's actual scratch bytes exactly. The 16,384-wide and 16,384-tall panels also compute.
  - Frozen V1: MET. The digests were measured on the base in a separate commit. The fold covers every point, summary and interval field.
- [x] Every cited defect was checked at the code.
  - E-02: `block_len_for_rule` and PolitisWhiteV2 in compute. Closed; residual bootstrap under-coverage noted in finding 1.
  - E-03: `make_hac` in eval, plus `column_tstats`, ICIR-EWMA (VIF) and `marginal_ic`. Closed; `hrp.hpp:386` is outside the cited locations and is noted for its owner.
  - E-08: `preflight_cross_section_ic`. Closed.
  - E-09: entry row `t + d` drives the prices, the terminal triple, `last_valid_mark` and `days_forward`; embargo is `h + d`. Closed in the engine. The `stage_equity_ic.cpp:112-113` wiring is correctly deferred to W0-I0b with a concrete note, including the `common_sample_dates` trap.
  - E-15: `zscore_row` single pass, and `ic_matrix` `WinsorizedV2`. Closed.
- [x] Ownership. `git diff --name-only feat/w0-integration...HEAD` shows only:
  - the six owned files, each within its sub-scope (signal_combiner t-stat sites, orthogonalize `marginal_ic`, signal_store winsor fix);
  - new `eval_w0e0a_*` and `combine_w0e0a_*` tests;
  - the fixture script and header that the brief authorizes under `tests/eval/fixtures/`;
  - the lane report.

  There are no CMake edits and no new `src/*.cpp`.
- [x] No test was weakened. The only pre-existing test diff is two lines in `eval_cross_section_ic_test.cpp` `good_config()`, which pin `HalfHorizonV1` and delay 0. That keeps the frozen checkpoint-14 expectations, tied to E-02 and E-09. No expectation was edited, and there is no `DISABLED_` or `GTEST_SKIP`. Test counts only grew.
- [x] Versioned enums.
  - `BlockLenRule::HalfHorizonV1`, `execution_delay = 0`, `TStatRule::IidV1`, `SignalNormalize::ZScoreRestandardizeV1` and `IcReturnTreatment::RawV1` each reproduce the old behaviour. Tests show this bit for bit.
  - The golden-digest old→new table is in the report and tied to E-02 and E-09.
  - Combiner-level selection is not possible from owned files; see waiver_needed.
- [x] The owning executables pass whole: eval 222/222 and combine 183/183.
- [x] The evidence in the report matches the claims. Every measured number was reproduced.

## Waivers / owner confirmations needed

1. **MA(20) coverage vehicle.** The owner should confirm that the published HAC interval `ic_mean_hac` (94.5%) satisfies "95% CI coverage in [93%, 97%]". The default bootstrap `ic_mean_ci` measures about 92.5% at L = 2h, which follows the plan's own "≥ 2h" prescription.
2. **Combiner-level V1 reproducibility.** The `IidV1` t-stat rule and the `RawV1` IC return treatment can be selected only at kernel level. Plumbing them into `IcirEwmaCombiner`, `GrinoldKahnCombiner`, `FamaMacBethRidge`, `KakushadzeRegression` and `marginal_ic` needs `signal_combiner.hpp` and `orthogonalize.hpp`, which this lane does not own. This is deferred to the owner of those headers.

## Re-review 1

Fresh fix-only re-review of lane head `a95df360c7cebd26bca2e9bc4972c402531dbc96`. The fix range is `1db2bb43..a95df360`: `c0a3a461` (review file) and `a95df360` (fix pass 1).

### Verdict

APPROVE. All five minor findings are fixed as required. There is no regression and no test was weakened. The only open item is waiver 1 (owner confirmation of the coverage vehicle), which no fixer can close.

### Evidence (reviewer-run, pool-5, `equity-dev`, `CMAKE_BUILD_PARALLEL_LEVEL=2`, 5.56 GB free before the build)

- `build -Preset equity-dev atx-engine-eval-tests atx-engine-combine-tests`: `[9/11] Linking ... atx-engine-eval-tests.exe`, `[10/11] Linking ... atx-engine-combine-tests.exe`, `build_exit=0`.
- `atx-engine-eval-tests.exe --gtest_brief=1`: `223 tests from 29 test suites ran ... [  PASSED  ] 223 tests.` `eval_exit=0`. That is 222 plus the one new caps test.
- `atx-engine-combine-tests.exe --gtest_brief=1`: `183 tests from 34 test suites ran ... [  PASSED  ] 183 tests.` `combine_exit=0`.
- `--gtest_filter=EvalIcCaps.*:EvalCrossSectionIc.Config_*`: 14/14 OK. This includes `EvalIcCaps.StandalonePreflight_EnforcesHorizonCountAndDrawMaxima` and the pre-existing `EvalCrossSectionIc.Config_BoundedMaxima_RejectOversizedHorizonsQuantilesAndDraws`, which still returns the same codes.
- The prints were reproduced verbatim:
  - `[EvalIcCoverage] MA(20), n=1750, 2000 reps: HansenHodrickV1 0.9450  NeweyWestV1 0.9135  naive-IID 0.3145`
  - `[EvalIcCoverage] CBB on MA(20), n=1000, B=199, 400 reps: HalfHorizonV1 (L=11) 0.8175  TwoHorizonV2 (L=42) 0.9300`
  - `[CombineHacTstat] MA(20) null, T=500, 600 columns: |t|>1.96 rate NW-auto 0.1333, IID 0.6717`
- `git diff 1db2bb43 HEAD -- atx-engine/tests` removes no lines. The two test edits only add assertions or a test. There is no `DISABLED_` or `GTEST_SKIP`.

### Per finding

1. **FIXED (the optional part); the owner confirmation is still open.**
   - `BootstrapBlockRule_V2MovesMa20CoverageTowardNominal` now also asserts `EXPECT_GE(v2.rate(), 0.90)`. The measured value is 0.9300.
   - The seeds are fixed (`Xoshiro256pp`, "no clock"), so the bound is deterministic, not flaky.
   - Waiver 1 still needs the owner: is `ic_mean_hac` (0.9450) the vehicle for the [93%, 97%] item?
2. **FIXED.**
   - The `kMaxIcHorizons` check (InvalidArgument) and the `kMaxBootstrapDraws` check (OutOfRange) moved verbatim from `validate` into `preflight_cross_section_ic` (`cross_section_ic.cpp:980-991`), before the byte arithmetic. The codes and messages are unchanged.
   - `validate` calls preflight at line 145, before the old check sites, so every plan and compute path is still guarded.
   - The one observable change is ordering: an oversize config is now rejected before the span-shape checks. That is benign, and no test depends on it.
   - The header's Errors list now names both checks. The new test pins both sides of each boundary with a 1 TiB budget.
   - There are no other callers of `preflight_cross_section_ic` in the tree.
3. **FIXED.**
   - The report's Deviation 1 and Integration notes now state that the GK and ICIR-EWMA **weights** moved through the `WinsorizedV2` default of `ic_matrix`.
   - They ask for an `IcReturnTreatment` field threaded to `signal_combiner.cpp:188,203`. Those line references are correct at HEAD.
4. **FIXED (as required: recorded).**
   - The report has a Known limitations section giving 13.3% against a nominal 5%, and IID at 67.2%.
   - There is a comment at `kCombineTStatRule` (`signal_combiner.cpp:55-59`).
   - The integration note asks for an h - 1 lag floor when `tstat_rule` lands.
   - The test bound `< 0.15` is unchanged. The code is unchanged apart from the comment.
5. **FIXED.**
   - The `hac.hpp:339-343` doc now cites Politis-White (2004) and Patton-Politis-White (2009) only.
   - It states explicitly that arch parity is NOT verified and gives the reason (the lag window differs by one).
   - It says the test reference is an independent numpy transcription.
   - This is a doc-only change.

### New findings introduced by the fix

None.
