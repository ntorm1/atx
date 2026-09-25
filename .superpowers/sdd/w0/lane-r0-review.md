# Lane W0-R0 review

## Verdict
APPROVE

(Zero blocker and zero major findings. There are six minor findings. One acceptance item,
"run under UBSan/ASan", is MET-substitute and needs an owner waiver; see the end of this file.)

## Reviewed SHA
f1d9d5fade9304d966d8e9bc1566b805699eb4dc (`feat/w0-r0`). Lane diff = `git diff feat/w0-integration...HEAD`.
The tree was clean at review.

## Evidence
All commands were run by the reviewer from `C:\atx-wt\pool-8` with `CMAKE_BUILD_PARALLEL_LEVEL=2`.
Free RAM before the build calls was 3.3 GB and 3.8 GB.

1. Owning-target build: `powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-risk-tests`
   ```
   [8/10] Linking CXX static library lib\spdlogd.lib
   [9/10] Linking CXX executable bin\atx-engine-risk-tests.exe
   exit=0
   ```
   The `check` of `atx-engine\src\risk\factor_model.cpp` printed `ninja: no work to do.` (exit 0). The
   object is current at HEAD. The build flags are `/Ob0 /Od /RTC1 -MDd ... /W4 /permissive- /WX` with
   no NDEBUG, so Eigen asserts and checked STL are live.
2. Whole owning executable: `build-equity\bin\atx-engine-risk-tests.exe --gtest_brief=1`
   ```
   [W0-R0 evidence] future-perturbation: LaggedV2 X identical at 3/3 dates; ContemporaneousV1 X moved at 3/3 dates
   [W0-R0 evidence] pure-noise Liquidity factor return: ContemporaneousV1 mean=0.005818 t=22.62 (n=150); LaggedV2 mean=0.000251 t=0.95 (n=150)
   [W0-R0 evidence] planted ln-adv outlier: V1 outlier z=6.189, spread of the other 39 names sd=0.136; V2 outlier z=2.681, others sd=0.888, max|z|=2.681
   [W0-R0 evidence] group-missing build: K=3, dates=30, group-3 missing on 20 dates, max|f - planted| checked at 1e-10
   [W0-R0 evidence] sector switch: max |f - planted| PIT groups=1.648e-16, static (today's) groups=6.757e-04
   [W0-R0 evidence] thin name (1 residual): D V1=0.000e+00 V2=2.380e-04 (median 3.436e-04, thick range 7.510e-05..9.553e-04); min-variance weight V1=0.3782 V2=0.0292 (max other name V2 0.1002)
   [W0-R0 evidence] sector-3 return on the thin name's first date: thin r=-0.07823, V1 f=-0.07823, V2 f=-0.01314
   ..\atx-engine\tests\risk_qp_augment_test.cpp(528): Skipped   (Nightly-gated, pre-existing)
   [==========] 467 tests from 62 test suites ran. (178220 ms total)
   [  PASSED  ] 466 tests.
   [  SKIPPED ] 1 test.
   exit=0
   ```
   Every measured number matches the lane report exactly.
3. Anchored suites: `scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>\.'`
   ```
   === RiskFactorModelPit     100% tests passed, 0 tests failed out of 7   exit=0
   === RiskSectorColumnsById  100% tests passed, 0 tests failed out of 5   exit=0
   === RiskThinNameFloor      100% tests passed, 0 tests failed out of 4   exit=0
   ```
4. Ownership: `git diff --name-only feat/w0-integration...HEAD` lists `exposures.hpp`,
   `factor_model.hpp`, `factor_model.cpp`, `tests/risk/risk_exposures_test.cpp` (expectation pin,
   R-06), the new files `tests/risk/risk_w0r0_{fixture.hpp,pit_test,sector_columns_test,thin_floor_test}.cpp`
   and `.superpowers/sdd/w0/lane-r0-report.md`. All of these are within scope.

### Acceptance items, checked against real output and the test code
| Item | Verdict | Basis |
|---|---|---|
| Future-perturbation invariance of the model built at t | MET | `RegressionExposuresInvariantToFuturePerturbation` perturbs every close and volume at rows 0..s. The X that explains r_s stays byte-identical under LaggedV2 and moves under ContemporaneousV1 at 3/3 dates, so the test discriminates the old code. `ContemporaneousLiquidityPremiumIsRemoved` measures the effect at the model level (t 22.6 → 0.95). `LaggedExposureRowIsThePriorClose` shows the passes use the same `exposure_row`: `date_design` and `regression_exposures` share `build_exposures(panel,cfg,exposure_row(s),side)`. The model-level test `ModelBuiltAtTInvariantToFuturePerturbation` does not discriminate (minor finding 1). |
| Group missing at s>0 builds with no OOB (UBSan/ASan) | MET-substitute | `GroupMissingAtOlderDatesBuildsAndMapsById` runs the K_s=2 < K=3 dates to completion under Debug checked indexing and asserts every column against its planted group return to 1e-10, with missing=20. `CheckedIndexingCatchesThePreW0Read` shows the old `beta[2]` read aborts in this build. No sanitizer preset exists in the repo, so this needs a waiver. |
| No D < 0.1·median | MET for the fundamental builder (the production path) | `ThinNameNoLongerLooksRiskless` counts 0 names below 0.1·median on the `build_components` D. `GlobalFloorAndDegenerateCases` covers the unit contract. Raising values below the median to frac·median leaves the median unchanged, and VRA scales D uniformly, so the invariant survives to `create`. The APCA variant is not floored (minor finding 4). |

### Defect IDs, read at the cited code
- **R-03 CLOSED.** `factor_model.cpp` `date_design` builds X at `exposure_row(timing, s)` = s+1. `step_return(r)` = close(r)/close(r+1), so every window at row s+1 excludes close(s). ContemporaneousV1 is retained.
- **R-04 CLOSED.** `scatter_factor_returns` + `map_columns` write by column identity. The loop bound is the date's K_s, with `ATX_ASSERT(j<k)` and a check that the beta size equals K_s. It is used in both the WLS and robust passes. `accumulate_ols` no longer uses the model K.
- **R-05 CLOSED.** `floor_specific_variances` is applied after `specific_variances` and before VRA. The same thin-name rule is applied to the pass-A d0 WLS weights. NoneV1 is retained.
- **R-06 CLOSED in the owned files.** `zscore_column_v2` is the default. PitSideInputs is threaded through every pass, including the robust cap and the row-0 build. Production wiring in `stage_riskmodel.cpp` (owned by W3-R4, together with R-01) is honestly deferred in the report's integration note 1.

### Tests weakened?
No. The only pre-existing test change is `RiskExposures.SizeIsLnCapStandardized`, which is pinned to `ZScoreRule::EqualWeightV1`. Its hand values are the V1 formula, so this is legitimate under R-06, and the V2 values are pinned in the new suite. The report discloses one threshold set from measurement: the V1 weight > 0.3 in ThinNameNoLongerLooksRiskless. That threshold asserts the defect is present, and the V2 assertions are strict. No DISABLED_ tests and no new GTEST_SKIP, apart from the NDEBUG guard on the Debug-only death test.

## Findings
| path:line | severity | problem | required fix |
|---|---|---|---|
| atx-engine/tests/risk/risk_w0r0_pit_test.cpp:185 | minor | `ModelBuiltAtTInvariantToFuturePerturbation` would pass on the pre-W0 code. PanelView cannot address rows newer than its row 0, and the test slices the side inputs itself, so the test cannot fail. The R-03 proof rests on the per-date test, which is sound. | Label this test as a harness-shape smoke test in the report. Optionally, add a model-level check that perturbs close(s) for an interior s and compares the pass-B series. |
| atx-engine/src/risk/factor_model.cpp:900 | minor | A model factor with no members on a date gets factor return 0, and that 0 enters `factor_covariance`. This deflates that factor's variance roughly in proportion to its missing fraction (20/30 in the fixture). `missing` is computed but never used by `build_components`. The behaviour is documented and is a strict improvement over the pre-W0 UB. | Track as a W1 risk item: either treat missing dates as missing in the covariance (pairwise or EWMA over observed dates) or rescale. At minimum, record it in the ledger or the defect register. |
| atx-engine/include/atx/engine/risk/exposures.hpp:733 | minor | The final ±3 clip of z after cap-weighted centring truncates the small-cap tail of a skewed descriptor. For Size with ln-cap sd σ, the cap-weighted centre sits about σ above the equal-weight mean. At σ=2, the names below z_eq≈-1 (about 16%) are all pinned at -3. This does not affect production yet, because no caps are passed (R-01). | Before caps are wired (W3-R4), decide whether the post-standardization clip is wanted. USE4 trims the raw descriptors and does not clip after cap-weighted centring. Record the choice. |
| atx-engine/src/risk/factor_model.cpp:744 | minor | The statistical (APCA) variant built by the same `FactorModelBuilder::build` does not apply the 0.1·median floor, so the "No D < 0.1·median" invariant does not hold there. A near-stale complete-history name can still get a tiny D. This path is not used by `stage_riskmodel` and is outside R-05's cited lines. | Apply the global frac·median floor to s_n in `build_stat_factor_model`, or state explicitly in the report that the acceptance is scoped to the fundamental builder. |
| atx-engine/src/risk/factor_model.cpp:492,972 | minor | `std::clamp(specific_floor_frac, 0, 1)` passes NaN through unchanged. A NaN config would set every D and d0 to NaN, because `!(d >= NaN)` is true. Config inputs are not validated. | Reject a non-finite `specific_floor_frac` (Err InvalidArgument in `run_passes`), or treat NaN as 0. |
| atx-engine/include/atx/engine/risk/exposures.hpp:157 | minor | The header says each V1 enumerator reproduces pre-W0 "bit-for-bit". Two changes are not gated: the empty-sector-column drop in `date_design`, which uses dates the old code skipped as rank-deficient, and the `kNoGroup` drop. No test pins all-V1 output against a pre-W0 digest. The report discloses the first change in deviation 2(b). | Soften the comment ("except dates the old code skipped as rank-deficient"). Optionally, add an all-V1 digest pin on a fixture where K_s==K. |

Integration-gate note (not a finding against this lane): the new defaults (LaggedV2, CapWeightedWinsorV2, StructuralMedianV2) change `build_components` output for the out-of-group consumers listed in the report's integration note 2. These are book_pipeline, data_adapt_factor, phase4_integration, and the atx-impl stage_riskmodel and stage_optimize tests. None of them was run in this lane, per the brief's group `risk`. I spot-read `data_adapt_factor_test`: window 4 sectors-only and window 21 Liquidity-only both remain feasible under the lag. The integration gate must run them.

## Checked
- [x] .agents/cpp/agent.md §10 checklist applied to the diff. No UB found. `cap_at`/`group_at` subspans are guarded by `validate` + `covers` on every call path. `weights[r]` is used only when the size equals m. The map/scatter indices are asserted. `min_obs=0` cannot divide by zero, since both sites guard it. There is no narrowing: all casts are explicit. The switches on `ZScoreRule`/`SpecificFloorRule` are exhaustive. The winsor loop is bounded at 16 passes. Error paths return Err (InvalidArgument/OutOfRange) and are tested. /W4 /WX builds clean.
- [x] Diff stays inside the brief's files-in-scope, plus new test files and the lane sdd report.
- [x] Evidence in the report matches its claims. I re-ran the whole executable and the three anchored suites, and every printed measurement is identical.
- [x] Every acceptance item was re-run and its test code read. Each item's test is non-vacuous against the old code, except the model-level smoke test in minor finding 1.
- [x] Every cited defect ID was read at the code: R-03, R-04 and R-05 CLOSED; R-06 CLOSED in the owned files, with production wiring deferred to W3-R4.
- [x] No existing test weakened. There is one V1 pin, tied to R-06.
- [x] Changed numeric defaults are behind versioned enums (ExposureTiming, ZScoreRule, SpecificFloorRule). No golden digest was re-baselined.
- [x] Causality-harness registration is not required in W0 (RULES: from W1 on). `regression_exposures`/`factor_returns` are provided as the seam.
- [x] The work is real and wired into the production path. `stage_riskmodel` calls `FactorModelBuilder::build_components`, which now runs the lagged, id-mapped and floored estimator by default. PIT caps and groups are library-only until W3-R4 wires them.

## Waiver needed
The "run the test under the UBSan/ASan config" part of acceptance item 2 is infeasible, because the repo has no sanitizer preset (agent.md §8). It is met by the brief-prescribed substitute: a Debug `/RTC1` + `-MDd` build with Eigen/ATX_ASSERT checked indexing, a death test on the exact pre-W0 read, and the corrected path run to completion on the same fixture. The owner must accept or waive this.

## Re-review 1

- **Reviewer:** fresh fix-only re-reviewer (round 1). Lane head `bde13cc8e131fc6359bba6aa201b6125c66c7f27`. Fix range `f1d9d5fa..bde13cc8`: `75743944` (the review itself) and `bde13cc8` (fix pass 1).
- **Verdict: APPROVE.** All 6 minors are addressed: 4 are fixed in code with discriminating tests, 1 is tracked as a W1 item, and 1 is fixed in documentation. The original review had no blockers or majors. Nothing regressed, and no test was weakened.

### Evidence (re-run by this reviewer from `C:\atx-wt\pool-8`, `CMAKE_BUILD_PARALLEL_LEVEL=2`, 2.78 GB free)
- `scripts\atx-build.ps1 build -Preset equity-dev atx-engine-risk-tests` → `[9/10] Linking CXX executable bin\atx-engine-risk-tests.exe`, exit 0 (/W4 /WX).
- Whole executable `build-equity\bin\atx-engine-risk-tests.exe --gtest_brief=1` → `471 tests from 62 test suites ran`, `470 PASSED`, `1 SKIPPED` (pre-existing nightly gate `risk_qp_augment_test.cpp:528`), exit 0. Every `[W0-R0 evidence]` line matches the report byte for byte, including all pre-existing numbers from the first review.

### Per finding
| # | Finding | Status | Evidence |
|---|---|---|---|
| 1 | `ModelBuiltAtTInvariantToFuturePerturbation` is non-discriminating | **FIXED** | The test header now reads "HARNESS-SHAPE SMOKE TEST (not the R-03 discriminator)", and the report's acceptance row demotes it the same way. New `PassBSeriesInvariantToInteriorPerturbation` has an exact-fit DGP (r_s = f_s·z_{s+1}, dollar volume set directly, so the pass-A d0 weights cannot matter). It perturbs closes at rows 0..s0−1 and volumes at 0..s0. Measured: LaggedV2 max\|Δf\| over s≥s0 = 2.429e-17, and recovery against the planted f = 3.1e-17. The test checks that the perturbation is live (\|Δf_{s0−1}\| = 2.4e-2) and that ContemporaneousV1 moves (\|Δf_s0\| = 8.2e-4 > 1e-6), so it discriminates against the pre-W0 timing within the test itself. |
| 2 | Missing-factor return 0 enters F | **FIXED (tracked)** | This is the W1 item the finding asked for. There is a `KNOWN LIMITATION (W1 risk item ...)` comment at the `fkept` compaction in `build_components`. Report integration note 5 names the owner (W1-R2 or the W1 covariance owner), the magnitude and the fix options, and it appears in the ledger candidates. Behaviour is unchanged, as intended. |
| 3 | Post-centring ±3 clip pins the small-cap tail | **FIXED** | The decision is recorded in the `ZScoreRule` doc, the `zscore_column_v2` contract comment, report row 3 and integration note 6 (which warns downstream not to assume \|z\|≤3). The clip is removed: `x = (x − μ_w)·inv_sigma`. The ±3σ winsorizing of the raw descriptor, which is the brief's "±3 winsorizing", is kept. New `SizeSmallCapTailKeepsItsOrder` covers 400 names at ln-cap sd 2: 26 names below −3 keep their order, 1 name at the minimum, 0 adjacent ties, monotone in cap, cap-weighted mean 0 and eq-std 1 at 1e-12. The only other place `kZScoreWinsor` appears is `exposures.hpp` (grep). |
| 4 | APCA path not floored | **FIXED** | `build_stat_factor_model` applies `detail::floor_at_median`, shared with `floor_specific_variances`, under `StructuralMedianV2`. NoneV1 keeps the raw s_n, and the switch is exhaustive. `StatisticalModelIsFlooredToo`: the stale name moves from 4.289e-12 to 3.491e-05 (= 0.1 × median 3.491e-04). Exactly 1 name changes, the median is unchanged, and 0 names are below the floor. |
| 5 | NaN `specific_floor_frac` poisons D/d0 | **FIXED** | `run_passes` (serving `build_components`, `factor_returns` and `build`) and `build_stat_factor_model` return `Err(InvalidArgument)` for a non-finite frac. `effective_floor_frac` maps a non-finite value to 0 in both the kernel and the pass-A d0 floor (`accumulate_ols` is reached only through `run_passes`, grep). `NonFiniteFloorFracIsRejected` covers NaN and ±Inf on all 3 entry points, with finite controls that build and give a finite D, plus the kernel's behaviour on direct calls. |
| 6 | V1 "bit-for-bit" overclaim | **FIXED** | The comment is rewritten to "restores the pre-W0 arithmetic OF ITS OWN RULE". It lists the three unversioned defect fixes, (a) the empty-sector drop, (b) id mapping and (c) the kNoGroup drop, and names the panels on which pre-W0 output is reproduced. The optional digest pin was not added, and the report justifies this (no pre-W0 digest exists in the tree). That is acceptable, because the pin was optional. |

### Regression / test-integrity check
- The only existing expectation that changed is in this lane's own `ZScoreIsCapWeightedAndWinsorized`, which pinned the clip that finding 3 removed. The report lists the change.
  - `max|z| ≤ 3+1e-12` became `max|z − mean_eq(z)| ≤ 3+1e-6`. Measured value: 3.000000044. The 1e-6 allowance covers the bounded-iteration residual δ, which is documented in the contract. The old exact bound held only because of the removed clip.
  - The cap-weighted-mean check tightened from a conditional 0.05 to an unconditional 1e-12, and the eq-std check became unconditional. On balance the test is stronger.
- No test was skipped, disabled or deleted. The test count went from 467 to 471 (4 new tests). No golden digest was re-baselined.
- Production impact of the clip removal is nil today. Without caps, μ_w = μ_eq, so \|z\| ≤ 3+δ with δ ≈ 4e-8 in the worst measured case. After caps are wired, the change is intended (note 6).
- The diff stays in owned files (`exposures.hpp`, `factor_model.hpp`, `factor_model.cpp`), the lane test files and the lane sdd report. There are no CMake edits and no new `src/` files.

### New findings introduced by the fix
None.
