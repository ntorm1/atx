# Lane W0-R0 report — Risk estimator blockers

## Outcome

DONE. All four cited defects (R-03, R-04, R-05, R-06) are closed in the owned files, each
changed numeric rule has a versioned V1 enum that reproduces the pre-W0 arithmetic, and the
whole `atx-engine-risk-tests` executable is green (467 tests: 466 passed, 1 Nightly skip).
One acceptance item ("run under UBSan/ASan") is met by the substitute the brief prescribes
(no sanitizer preset exists) and is marked MET-substitute for the owner to accept or waive.

## Branch / SHA

`feat/w0-r0` @ the commit that adds this report (code commit `40e26729`, report commit on
top). Base: W0 base `458d0bef480a624e258070c9d45174a9984466bf`, then
`merge --no-ff feat/w0-integration` (O1 + E0b already merged there). Pool: `C:\atx-wt\pool-8`,
lease run id `aes-w0-r0` (held by the orchestrator; this lane never touched the lease).
Tree clean at the end.

## Files changed (owned files + new test files only)

| File | Change |
|---|---|
| `atx-engine/include/atx/engine/risk/exposures.hpp` | New enums `ExposureTiming`, `ZScoreRule`, `SpecificFloorRule`; new `CovarianceConfig` fields `specific_floor`, `specific_min_obs` (21), `specific_floor_frac` (0.1); new `FactorModelConfig` fields `exposure_timing`, `zscore_rule`; `PitSideInputs` (per-date cap/group) + `kNoGroup`; `detail::zscore_column_v2`; a `build_exposures(panel, cfg, row, PitSideInputs)` overload; `kNoGroup` names dropped when sectors are emitted. |
| `atx-engine/include/atx/engine/risk/factor_model.hpp` | `detail::SpecificFloorStats`, `detail::floor_specific_variances`, `detail::exposure_row`, `detail::map_columns`, `kNoColumn`; `FactorReturnPanel`; `FactorModelBuilder` PIT overloads of `build` / `build_components`, `regression_exposures`, `factor_returns`; private passes now take `PitSideInputs` + X[0]. |
| `atx-engine/src/risk/factor_model.cpp` | Lagged per-date design (`date_design`), empty-sector-column drop, identity scatter of coefficients (`scatter_factor_returns`), thin-name floor on D and on the pass-A bootstrap weights, `run_passes` shared by `build_components` / `factor_returns`, PIT side inputs threaded through every pass (robust path uses the exposure date's cap). |
| `atx-engine/tests/risk/risk_exposures_test.cpp` | `SizeIsLnCapStandardized` pinned to `ZScoreRule::EqualWeightV1` (R-06; see below). |
| `atx-engine/tests/risk/risk_w0r0_fixture.hpp` (new) | `StorePanel` with `view_at(k)` (future rows physically in the buffer), deterministic splitmix64/Box-Muller RNG, mean/t helper. |
| `atx-engine/tests/risk/risk_w0r0_pit_test.cpp` (new) | Suite `RiskFactorModelPit` (7 tests). |
| `atx-engine/tests/risk/risk_w0r0_sector_columns_test.cpp` (new) | Suite `RiskSectorColumnsById` (5 tests). |
| `atx-engine/tests/risk/risk_w0r0_thin_floor_test.cpp` (new) | Suite `RiskThinNameFloor` (4 tests). |

No CMake edits, no new `src/` files.

## What changed, in plain terms

- **R-03 (lagged exposures).** The regression passes now explain the return
  r_s = close(s)/close(s+1) − 1 with exposures built at row s+1, i.e. from data that
  ends at the previous close. Every style window (momentum, volatility, beta, adv) therefore
  ends before r_s is realized. The model's own exposures X[0] are still built at row 0
  (they are the forecast exposures for the next return). `ExposureTiming::ContemporaneousV1`
  restores the old row-s behaviour.
- **R-04 (sector columns by id).** Each date is solved with its own columns. Its
  coefficients are then written into the model's factor-return series by column identity
  (sector by group id, style by factor). A model factor with no members on a date gets
  return 0 there and is counted in `FactorReturnPanel::missing`. A group that exists at a
  date but not today is regressed on (so residuals stay right) but not carried. The old
  out-of-bounds read of `fit->beta[c]` cannot happen: the scatter loop iterates the date's
  own K_s and asserts `j < K`. Additionally, a sector whose members all lost their return on
  a date is now dropped from that date's design instead of making the design rank-deficient
  and silently skipping the whole date.
- **R-05 (thin-name floor).** Under the default `SpecificFloorRule::StructuralMedianV2`, a
  name with fewer than `min(21, max(2, ⌈full/2⌉))` residuals is shrunk (in σ space, weight
  γ = n/min_obs on its own estimate) toward a structural prediction: an OLS of ln D on the
  exposures of the well-observed names (plus an intercept when there are no sector
  columns), clamped to the well-observed range, with the median as fallback. Then every
  D is floored at 0.1·median(D). The same thin-name rule is applied to the pass-A bootstrap
  variances d0, because a 1-residual name otherwise gets a 1e12 WLS weight and dictates its
  sector's factor return. `SpecificFloorRule::NoneV1` restores the old behaviour.
- **R-06 (z-scores and PIT cap/group).** The default `ZScoreRule::CapWeightedWinsorV2`
  winsorizes each style descriptor iteratively at ±3σ around the equal-weight mean, then
  standardizes with the cap-weighted mean and the equal-weight std, and clips z to ±3.
  (Centring the winsor bounds on the cap-weighted mean was tried first and rejected: one
  mega-cap outlier captures that centre.) Without caps the mean is equal-weighted.
  `PitSideInputs::per_date(cap, group, n_rows)` carries a cap and a group for every panel
  row, and every regression date reads the values of its own exposure row. The old
  `(market_cap, group_id)` overloads are kept and are documented as the static broadcast
  form (`PitSideInputs::broadcast`), which is only correct when those values are really
  constant over the window.

## Acceptance table

| Plan acceptance item | Test(s) | Measured result | Status |
|---|---|---|---|
| Future-perturbation invariance of the model built at t | `RiskFactorModelPit.ModelBuiltAtTInvariantToFuturePerturbation`; per date: `RiskFactorModelPit.RegressionExposuresInvariantToFuturePerturbation`; lag row: `RiskFactorModelPit.LaggedExposureRowIsThePriorClose` | Model at t (view with 12 future rows physically in the same buffer; future closes, volumes, caps and groups perturbed): X, F, D byte-identical. Per date: LaggedV2 X identical at 3/3 dates when every close/volume at rows 0..s is perturbed; ContemporaneousV1 X moved at 3/3 dates. | MET |
| (R-03 measured effect) | `RiskFactorModelPit.ContemporaneousLiquidityPremiumIsRemoved` | Pure-noise panel (80 names, 150 dates) with same-day volume response: Liquidity factor return ContemporaneousV1 mean 0.005818, t = 22.62; LaggedV2 mean 0.000251, t = 0.95. | MET |
| A group missing at s > 0 builds with no out-of-bounds access (run under UBSan/ASan) | `RiskSectorColumnsById.GroupMissingAtOlderDatesBuildsAndMapsById`, `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read`, `RiskSectorColumnsById.VanishedGroupIsRegressedButNotCarried` | K = 3, group 3 absent on 20 of 30 dates: build, build_components and factor_returns succeed; every column equals its own group's planted return to 1e-10; `missing[g3] = 20`. The exact pre-W0 read (`beta[2]` on a K_s = 2 fit) aborts under this Debug build's checked indexing (`EXPECT_DEATH` passes). No sanitizer preset exists in the repo, so this is the Debug `/RTC1` + Eigen/ATX_ASSERT substitute the brief prescribes. | MET-substitute (no sanitizer preset) |
| No D < 0.1·median | `RiskThinNameFloor.ThinNameNoLongerLooksRiskless`, `RiskThinNameFloor.GlobalFloorAndDegenerateCases`, `RiskThinNameFloor.StructuralFallbackRecoversPlantedModel` | 30 names, one with 1 residual: count of D < 0.1·median = 0. Thin-name D: V1 = 0 (→1e-12 in create), V2 = 2.380e-04 (median 3.436e-04, well-observed range 7.510e-05..9.553e-04). Minimum-variance weight in the thin name: V1 0.3782, V2 0.0292 (largest other name 0.1002). | MET |
| (R-05, WLS weights) | `RiskThinNameFloor.ThinNameDoesNotDictateItsSectorReturn` | Sector-3 return on the thin name's first date: thin name r = −0.07823; V1 f = −0.07823 (copied); V2 f = −0.01314. | MET |
| (R-04, PIT groups) | `RiskSectorColumnsById.PitGroupReassignmentRecoversPlantedReturns`, `RiskSectorColumnsById.MapColumnsMatchesByIdentity` | Name switches sector: max \|f − planted\| with PIT groups 1.648e-16, with today's groups broadcast 6.757e-04. | MET |
| (R-06, z-score + PIT cap) | `RiskFactorModelPit.ZScoreIsCapWeightedAndWinsorized`, `RiskFactorModelPit.CapAndGroupAreReadPerDate`, `RiskFactorModelPit.SideInputShapeAndCoverageAreChecked` | Planted 40-log-point ln-adv error: V1 outlier z = 6.189 and the other 39 names squeezed to sd 0.136; V2 outlier z = 2.681, others sd 0.888, max \|z\| = 2.681. Two-name hand case matches 1 − μ_cap, 3 − μ_cap to 1e-12. Historical Size/sector exposures use that date's cap/group; broadcast differs. | MET |

## Defect table

| ID | Status | How / where |
|---|---|---|
| R-03 | CLOSED | `factor_model.cpp` `date_design` builds X at `detail::exposure_row(cfg.exposure_timing, s)` = s+1 by default; `ExposureTiming` enum in `exposures.hpp`. The exposures.hpp window helpers are unchanged: lagging the row makes every window end at s+1. The fundamental `ResidVol` style (plan text "resid-vol windows") lives in `fundamental_factors.hpp` / the hybrid builder, which this lane does not own; `FactorModelBuilder` never emits it. See integration note 3. |
| R-04 | CLOSED | `factor_model.cpp` `date_design` (per-date K_s, empty-sector drop), `scatter_factor_returns` + `detail::map_columns` (identity mapping) in both the WLS and robust passes; `accumulate_ols` uses K_s. |
| R-05 | CLOSED | `detail::floor_specific_variances` applied in `build_components` (after the specific-risk method, before VRA); the same rule on d0 in `accumulate_ols`. `SpecificFloorRule` enum + `specific_min_obs` / `specific_floor_frac` in `CovarianceConfig`. `FactorModel::create`'s 1e-12 numerical guard is unchanged. The APCA (statistical) variant is not changed: it keeps only complete-history names, so it has no thin names. |
| R-06 | CLOSED (library) | `ZScoreRule::CapWeightedWinsorV2` in `build_exposures` (`detail::zscore_column_v2`); `PitSideInputs` + PIT overloads of `build_exposures`, `FactorModelBuilder::build/build_components`, used by every pass and by the APCA row-0 cross-section. The production callers still pass static spans (no caps and a static group map today); passing per-date cap/group there is outside this lane's files (integration note 1). |

## Existing test expectation changes

| Test | Change | Defect |
|---|---|---|
| `RiskExposures.SizeIsLnCapStandardized` | Now sets `cfg.zscore_rule = ZScoreRule::EqualWeightV1` explicitly. Its hand-computed values {−1, +1} are the equal-weight formula; the new default gives {1 − μ_cap, 3 − μ_cap} = {−1.7616, 0.2384}, pinned in `RiskFactorModelPit.ZScoreIsCapWeightedAndWinsorized`. | R-06 |

No other existing risk test changed; none was skipped, disabled or deleted.

## Evidence

All commands were run from `C:\atx-wt\pool-8` with `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'` after
checking free RAM (3.2 GB, 4.6 GB, 4.5 GB, 2.3 GB, 3.4 GB at the build calls; all ≥ 2 GB).
`build-equity\CMakeCache.txt` already had `ATX_TEST_GROUPS:STRING=risk` (the brief's group),
so no reconfigure was needed.

1. Merge integration — `git -C C:\atx-wt\pool-8 merge --no-ff feat/w0-integration -m "w0-r0: merge feat/w0-integration"`
   → `Merge made by the 'ort' strategy.` (46 files, O1 + E0b content), exit 0.

2. Single-TU check — `powershell -NoProfile -File scripts\atx-build.ps1 check -Preset equity-dev atx-engine\src\risk\factor_model.cpp`
   ```
   [1/4] Building CXX object atx-engine\CMakeFiles\atx-engine.dir\src\risk\factor_model.cpp.obj
   exit=0
   ```

3. Owning-target build (final) — `powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-risk-tests`
   ```
   [2/3] Linking CXX executable bin\atx-engine-risk-tests.exe
   exit=0
   ```
   (The previous full rebuild after the header change: `[26/76] Linking CXX static library
   lib\atx-engine.lib` … `[73/76] Linking CXX executable bin\atx-engine-risk-tests.exe`,
   exit=0, no warnings under /W4 /WX other than the toolchain's unrelated `/MP` notice.)

4. Anchored suites — `powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'`
   ```
   === RiskFactorModelPit
   7/7 Test #413: RiskFactorModelPit.ZScoreIsCapWeightedAndWinsorized ...................   Passed    0.06 sec
   100% tests passed, 0 tests failed out of 7
   exit=0
   === RiskSectorColumnsById
   5/5 Test #418: RiskSectorColumnsById.MapColumnsMatchesByIdentity ..................   Passed    0.06 sec
   100% tests passed, 0 tests failed out of 5
   exit=0
   === RiskThinNameFloor
   4/4 Test #422: RiskThinNameFloor.GlobalFloorAndDegenerateCases ............   Passed    0.06 sec
   100% tests passed, 0 tests failed out of 4
   exit=0
   ```

5. Whole owning executable — `build-equity\bin\atx-engine-risk-tests.exe --gtest_brief=1`
   ```
   [W0-R0 evidence] future-perturbation: LaggedV2 X identical at 3/3 dates; ContemporaneousV1 X moved at 3/3 dates
   [W0-R0 evidence] pure-noise Liquidity factor return: ContemporaneousV1 mean=0.005818 t=22.62 (n=150); LaggedV2 mean=0.000251 t=0.95 (n=150)
   [W0-R0 evidence] planted ln-adv outlier: V1 outlier z=6.189, spread of the other 39 names sd=0.136; V2 outlier z=2.681, others sd=0.888, max|z|=2.681
   [W0-R0 evidence] group-missing build: K=3, dates=30, group-3 missing on 20 dates, max|f - planted| checked at 1e-10
   [W0-R0 evidence] sector switch: max |f - planted| PIT groups=1.648e-16, static (today's) groups=6.757e-04
   [W0-R0 evidence] thin name (1 residual): D V1=0.000e+00 V2=2.380e-04 (median 3.436e-04, thick range 7.510e-05..9.553e-04); min-variance weight V1=0.3782 V2=0.0292 (max other name V2 0.1002)
   [W0-R0 evidence] sector-3 return on the thin name's first date: thin r=-0.07823, V1 f=-0.07823, V2 f=-0.01314
   ..\atx-engine\tests\risk_qp_augment_test.cpp(528): Skipped
   [==========] 467 tests from 62 test suites ran. (199174 ms total)
   [  PASSED  ] 466 tests.
   [  SKIPPED ] 1 test.
   exit=0
   ```
   (The two `kelly_sizing.cpp CHECK failed` lines in the raw output are the expected output of
   existing death tests.)

### Diagnostics (failed attempts; not used to support any claim)

- First full run after the source change: 1 failure, `RiskExposures.SizeIsLnCapStandardized`
  (V1 hand values vs the new cap-weighted default) — resolved by pinning that test to
  `EqualWeightV1` (R-06, table above).
- First build with the new header: `FactorReturnSeries` clashed with the hybrid model's type
  of the same name → renamed mine to `FactorReturnPanel`.
- First run of the new suites: `ThinNameNoLongerLooksRiskless` expected the V1 thin-name
  minimum-variance weight to exceed 0.9; measured 0.3782 (sector-factor risk still limits the
  position). The threshold was set from the measurement (> 0.3, > 3× the largest V2 name,
  > 10× the V2 weight); the V2 assertions were not relaxed.

## Golden-digest old → new

None. No test in the tree hard-codes a factor-model digest; the atx-impl digest tests compare
two runs of the same build (equality across paths / determinism), so they are unaffected by
construction. They are not in this lane's build group and were not run here (see
integration note 2).

## Deviations from brief

1. The ASan/UBSan item is met by the brief's substitute (Debug checked indexing + explicit
   index assertions + a death test on the exact pre-W0 read) — MET-substitute.
2. Beyond the literal build list, two closely related repairs were needed for the cited
   defects to be closed in practice: (a) the thin-name rule is also applied to the pass-A
   bootstrap WLS weights (R-05; otherwise the thin name dictates its sector's return), and
   (b) a sector with no surviving members on a date is dropped from that date's design
   instead of skipping the date (R-04). Both are behind the same versioned enums except (b),
   which only changes dates the old code discarded as rank-deficient.
3. The winsor centre is the equal-weight mean, not the cap-weighted mean (reason above);
   standardization still uses the cap-weighted mean as the plan asks.
4. New public diagnostics (`regression_exposures`, `factor_returns`, `FactorReturnPanel`) were
   added as the causality-harness seam; they share the build's arithmetic (`run_passes`).

## Integration notes

1. **R-06 wiring (Track R, W3-R4 / owner of `atx-impl/src/stage_riskmodel.cpp`, and Track B
   for `book/pipeline.hpp`):** production callers still use the static
   `(market_cap, group_id)` overloads. To be fully point-in-time they should pass
   `PitSideInputs::per_date(cap, group, n_rows)` with one row per panel row of the
   `PanelWindowView` (rows `[0, estimation_window]` at least under the default lag). Today
   `stage_riskmodel` passes no caps and a static group map (R-01), so the practical effect
   is limited to groups until caps are wired.
2. **Lag lookback (+1 row) in `stage_riskmodel.cpp:245-253`:** with lagged exposures the
   oldest estimation date reads exposures one row older. `deepest_lookback` should become
   `max(style lookbacks) + 1` (Momentum 253, Beta 254, Vol 61, Liquidity 21; sectors-only
   still 1 plus the lag = 2). Without it, the oldest date of a Momentum- or Vol-limited
   window becomes unusable (one date lost; no error). Out-of-group tests that call the
   factor path and should be re-run at the integration gate: `atx-engine` book
   (`book_pipeline_test`), data (`data_adapt_factor_test`, `data_altdata_subsumption_test`),
   core (`phase4_integration_test`), and atx-impl (`stage_riskmodel_test`,
   `stage_riskmodel_dead_factor_test`, `stage_optimize_riskmodel_test`,
   `stage_optimize_pit_test`, `stage_optimize_neutralize_test`,
   `stage_optimize_dead_alpha_*`, `stage_metabook_*riskmodel*/dead_alpha*`,
   `stage_combine_riskmodel_wire_test`). None pins a hard-coded factor-model value that I
   could find, but none was run in this lane (group `risk` only).
3. **ResidVol (W1-R2 or owner of `fundamental_factors.hpp` / `hybrid_factor_model`):** the
   hybrid builder has its own return/style rows; its residual-volatility window should
   end at s−1 by the same rule (not reachable from `FactorModelBuilder`).
4. `kNoGroup` (= `u32` max) is now a reserved group id meaning "unclassified at this date";
   such names are dropped from that date's cross-section when sector columns are emitted.

## Ledger candidates

- W0-R0: on a pure-noise panel with same-day volume response, contemporaneous exposures made
  a spurious Liquidity factor return with t = 22.6 (150 days); the lagged rule gives t = 0.95.
- W0-R0: a 1-residual name got D = 0 (→1e-12) and 37.8% of a 30-name minimum-variance
  book pre-W0; StructuralMedianV2 gives D = 2.4e-4 and 2.9%.
- W0-R0: winsorizing around a cap-weighted centre is unsafe (one mega-cap captures it); centre
  the ±3σ bounds on the equal-weight mean, then standardize with the cap-weighted mean.

## Post-merge sync (feat/w0-integration → feat/w0-r0)

- Pre-merge lane head: `16728a9d` (already contained an earlier sync from feat/w0-integration,
  point-in-time risk estimator commit `40e26729`, lane report `d5d7d9a0`).
- `feat/w0-integration` had advanced past that point (A0 lane merged: alpha kernel correctness,
  new `eval/hac.hpp`, factory canonical/crossover, learn/latent/gbt/tcn/nn changes, plus D0/E0A/L0
  lane reports). None of it touches `risk/factor_model.{hpp,cpp}` or `risk/exposures.hpp`.
- `git -C C:\atx-wt\pool-8 merge --no-ff feat/w0-integration -m "w0-r0: merge feat/w0-integration" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"`
  — clean merge, **no conflicts** (93 files changed, all outside the risk lane's owned files).
  Merge commit: `dbea09f1012bbeabfd296721d826ba006e9d29c9`.
- Rebuild: `Set-Location C:\atx-wt\pool-8; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-risk-tests`
  — exit 0 (linked `bin\atx-engine-risk-tests.exe`; a first attempt returned exit 1 under RAM
  pressure with all steps otherwise complete through linking — an immediate rebuild with nothing
  changed relinked cleanly at exit 0, treated as transient host load, not a code issue).
- Anchored suite reruns (`ctest --test-dir build-equity -R '^<Suite>\.'`, `-j 1`):
  - `RiskFactorModelPit.*` — 7/7 passed (1.88s).
  - `RiskSectorColumnsById.*` — 5/5 passed (1.19s).
  - `RiskThinNameFloor.*` — 4/4 passed (0.61s).
- Whole owning executable, `build-equity\bin\atx-engine-risk-tests.exe --gtest_brief=1`:
  466 passed, 1 skipped (Nightly-gated dense-oracle battery, opt-in via `ATX_NIGHTLY`/
  `ATX_RISK_NIGHTLY`, correctly not run here), 0 failed. Exit 0.
- No code changes were needed post-merge; the risk lane's files were untouched by the merge.
- Head after this sync + report commit: see structured result `head_sha`.
