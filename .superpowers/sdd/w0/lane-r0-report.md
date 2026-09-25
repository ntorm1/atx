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

## Fix pass 1 (review `lane-r0-review.md`, verdict APPROVE, 6 minor)

The review had no blockers and no majors. Four minors are fixed in code, one is recorded as a
W1 item (with an in-code note), and one is fixed as a documentation change. The owner-waiver
item (UBSan/ASan, MET-substitute) is unchanged.

Files touched: `risk/exposures.hpp`, `risk/factor_model.hpp`, `src/risk/factor_model.cpp`
(owned); `tests/risk/risk_w0r0_pit_test.cpp` and `tests/risk/risk_w0r0_thin_floor_test.cpp`
(lane test files). There are no CMake edits and no new `src/` files.

| # | Finding | Action | Evidence |
|---|---|---|---|
| 1 | `ModelBuiltAtTInvariantToFuturePerturbation` cannot fail on pre-W0 code | **Relabelled** as a harness-shape smoke test in its header comment. It does not count as an R-03 proof. **Added** the discriminating model-level test `RiskFactorModelPit.PassBSeriesInvariantToInteriorPerturbation` (details below). | `interior perturbation (s0=12): LaggedV2 max\|df\| over s>=s0 = 2.429e-17, \|f - planted\| max = 3.123e-17, \|df(s0-1)\| = 2.428e-02; ContemporaneousV1 \|df(s0)\| = 8.188e-04` |
| 2 | A factor with no members on a date gets return 0, and that 0 enters `factor_covariance` | **Tracked as a W1 risk item** (not fixed here; the reviewer asked for tracking). There is a `KNOWN LIMITATION (W1 risk item)` comment at the `fkept` compaction in `build_components`. It is recorded below under "Integration notes (fix pass 1)" as item 5, and in the ledger candidates. | code comment, `factor_model.cpp` `build_components` |
| 3 | The final ±3 clip of z after cap-weighted centring pins the small-cap tail | **Decided and implemented now**, before W3-R4 wires caps: the post-centring clip is **removed**. This follows USE4: trim (winsorize) the raw descriptor at ±3σ around the equal-weight mean, then standardize with the cap-weighted mean and the equal-weight σ, and never clip z itself. As a result, the cap-weighted mean of z is now exactly 0 and the equal-weight std is exactly 1 in every case. Before, both held only when the clip did not bind. \|z\| can exceed 3 by \|μ_eq−μ_w\|/σ, which is intended. The header contract and the `ZScoreRule` doc were rewritten. New test `RiskFactorModelPit.SizeSmallCapTailKeepsItsOrder`. | `Size z (ln-cap sd 2, n=400): min z=-4.432, names below -3 (pinned by a post-centring clip)=26, names at the minimum=1, adjacent ties=0` |
| 4 | The APCA variant does not apply the 0.1·median floor | **Fixed.** `build_stat_factor_model` applies the same global floor to s_n under `StructuralMedianV2`, and `NoneV1` keeps the raw s_n. The floor code is factored into `detail::floor_at_median`, which both builders share, so "no D < 0.1·median" now holds on both paths. New test `RiskThinNameFloor.StatisticalModelIsFlooredToo`: exactly one name changes, the median is unchanged, and 0 names are below the floor. | `APCA floor: stale-name D NoneV1=4.289e-12 V2=3.491e-05 (median 3.491e-04)` |
| 5 | A NaN `specific_floor_frac` turns every D and d0 into NaN | **Fixed.** `run_passes` (which serves `build_components`, `factor_returns` and `build`) and `build_stat_factor_model` return `Err(InvalidArgument)` for a non-finite frac. The detail kernel treats a non-finite frac as 0 (`effective_floor_frac`), which protects direct callers, and the pass-A d0 floor uses the same helper. New test `RiskThinNameFloor.NonFiniteFloorFracIsRejected` covers NaN and ±Inf on all three entry points, with finite controls that build, plus the kernel behaviour. | test passes (below) |
| 6 | The claim that V1 enumerators reproduce pre-W0 "bit-for-bit" overclaims | **Comment softened** (`exposures.hpp` §W0-R0 rules). Each V1 enumerator restores its own rule. An all-V1 config is not a full pre-W0 replay because of three unversioned defect fixes: (a) the empty-sector-column drop, (b) identity column mapping, (c) the `kNoGroup` drop. The comment names the panels on which pre-W0 output is reproduced. The optional all-V1 digest pin was **not added**: no pre-W0 digest exists in the tree, and making one means building and running the base commit's code, which is outside this lane's single-target loop. | comment |

### Details

**`PassBSeriesInvariantToInteriorPerturbation` (finding 1).** The DGP is r_s = f_s·z_{s+1} exactly, where z_{s+1} is the Liquidity z-score at row s+1. Dollar volume is set directly, so ln adv20 does not depend on the closes. Every LaggedV2 date is therefore an exact fit, and its WLS solution does not depend on the pass-A d0 weights. That matters because the perturbation does move those weights through the newer dates.

The test perturbs the closes at rows 0..s0−1 and the volumes at rows 0..s0, with s0 = 12 inside a 40-date window. Every LaggedV2 factor return at s ≥ s0 must then stay the same and must equal the planted f_s. ContemporaneousV1 regresses r_s0 on row s0, which carries the perturbed volume, so its f_s0 moves. The test would therefore fail on the pre-W0 estimator. It also checks that f_{s0−1} moves, to show the perturbation is live inside the window.

### Existing test expectations changed in fix pass 1

- `RiskFactorModelPit.ZScoreIsCapWeightedAndWinsorized` is this lane's own test. It pinned the removed post-centring clip (finding 3).
  - (b) `EXPECT_LE(max|z|, 3+1e-12)` is replaced by the bound in the frame where the winsorizing is defined, `max|z − mean_eq(z)| ≤ 3 + 1e-6`, plus a new `cap-weighted mean of z == 0 (1e-12)`. The 1e-6 allowance is for the residual of the bounded 16-pass winsorizing iteration, which converges geometrically. Measured: `max|z - mean_eq(z)| = 3.000000044155`, `cap-weighted mean of z = -1.823e-15`. Before the fix, the ±3 bound on |z| was exact only because z itself was clipped. The header contract states this residual (δ).
  - (c) The `if (max|z| < 3) exact else 0.05` branch is replaced by the exact contract, applied unconditionally (strengthened).
- No other existing test changed. No test was skipped, disabled or deleted.

### Golden digests (fix pass 1)
None changed. The APCA floor and the clip removal do not touch any pinned digest in the risk target. The whole executable is green, as shown below.

### Evidence (fix pass 1)
All commands were run from `C:\atx-wt\pool-8` with `CMAKE_BUILD_PARALLEL_LEVEL=2`. Free RAM before the builds was 2.62, 4.72 and 3.43 GB.

- `scripts\atx-build.ps1 check -Preset equity-dev atx-engine\src\risk\factor_model.cpp` → exit 0.
- `scripts\atx-build.ps1 build -Preset equity-dev atx-engine-risk-tests` → `[73/74] Linking CXX executable bin\atx-engine-risk-tests.exe`, exit 0 (/W4 /WX clean).
- The first run of the new tests failed `ZScoreIsCapWeightedAndWinsorized` with `max_dev2 = 3.0000000441548011 vs 3.0000000000010001`. That run exposed the non-converged winsorizing residual that the old z-clip had been hiding. The fix was to document δ in the contract and bound it at 1e-6 in the test, as described above. After that the target was rebuilt (exit 0) and the runs below were made.
- Anchored ctest runs (`scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>\.'`):
  ```
  === RiskFactorModelPit     100% tests passed, 0 tests failed out of 9   exit=0
  === RiskSectorColumnsById  100% tests passed, 0 tests failed out of 5   exit=0
  === RiskThinNameFloor      100% tests passed, 0 tests failed out of 6   exit=0
  ```
- Whole owning executable `build-equity\bin\atx-engine-risk-tests.exe --gtest_brief=1`:
  ```
  [W0-R0 evidence] future-perturbation: LaggedV2 X identical at 3/3 dates; ContemporaneousV1 X moved at 3/3 dates
  [W0-R0 evidence] pure-noise Liquidity factor return: ContemporaneousV1 mean=0.005818 t=22.62 (n=150); LaggedV2 mean=0.000251 t=0.95 (n=150)
  [W0-R0 evidence] planted ln-adv outlier: V1 outlier z=6.189, spread of the other 39 names sd=0.136; V2 outlier z=2.681, others sd=0.888, max|z|=2.681
  [W0-R0 evidence] V2 winsor bound: max|z - mean_eq(z)| = 3.000000044155, cap-weighted mean of z = -1.823e-15
  [W0-R0 evidence] Size z (ln-cap sd 2, n=400): min z=-4.432, names below -3 (pinned by a post-centring clip)=26, names at the minimum=1, adjacent ties=0
  [W0-R0 evidence] interior perturbation (s0=12): LaggedV2 max|df| over s>=s0 = 2.429e-17, |f - planted| max = 3.123e-17, |df(s0-1)| = 2.428e-02; ContemporaneousV1 |df(s0)| = 8.188e-04
  [W0-R0 evidence] group-missing build: K=3, dates=30, group-3 missing on 20 dates, max|f - planted| checked at 1e-10
  [W0-R0 evidence] sector switch: max |f - planted| PIT groups=1.648e-16, static (today's) groups=6.757e-04
  [W0-R0 evidence] thin name (1 residual): D V1=0.000e+00 V2=2.380e-04 (median 3.436e-04, thick range 7.510e-05..9.553e-04); min-variance weight V1=0.3782 V2=0.0292 (max other name V2 0.1002)
  [W0-R0 evidence] sector-3 return on the thin name's first date: thin r=-0.07823, V1 f=-0.07823, V2 f=-0.01314
  [W0-R0 evidence] APCA floor: stale-name D NoneV1=4.289e-12 V2=3.491e-05 (median 3.491e-04)
  ..\atx-engine\tests\risk_qp_augment_test.cpp(528): Skipped   (Nightly-gated, pre-existing)
  [==========] 471 tests from 62 test suites ran. (195193 ms total)
  [  PASSED  ] 470 tests.
  [  SKIPPED ] 1 test.
  exit=0
  ```
  Every pre-existing evidence number is identical to the review run. There are 4 new tests (467 → 471).

### Updated acceptance rows
| Item | Test(s) | Result | Status |
|---|---|---|---|
| Future-perturbation invariance of the model built at t | `RegressionExposuresInvariantToFuturePerturbation` (per date), **`PassBSeriesInvariantToInteriorPerturbation` (model level, discriminating)**; `ModelBuiltAtTInvariantToFuturePerturbation` is a harness-shape smoke test only | LaggedV2 max\|Δf\| over s≥s0 = 2.4e-17; V1 \|Δf_s0\| = 8.2e-4 | MET |
| Group missing at s>0, no OOB (UBSan/ASan) | unchanged | unchanged | MET-substitute (owner waiver) |
| No D < 0.1·median | `ThinNameNoLongerLooksRiskless`, `GlobalFloorAndDegenerateCases`, **`StatisticalModelIsFlooredToo`** | 0 names below the floor on both the fundamental and the APCA builder | MET (both builders) |

### Integration notes (fix pass 1)
5. **W1 risk item (W1-R2 or the W1 covariance owner). Missing-factor zeros in F.** A model
   factor with no members on a date carries return 0, which is counted in
   `FactorReturnPanel::missing`, and that 0 enters `factor_covariance` / the EWMA covariance as
   an observation. This deflates that factor's variance roughly in proportion to its missing
   fraction (20/30 dates in the `RiskSectorColumnsById` fixture). Possible fixes: estimate F
   over the observed dates only (pairwise, or EWMA with weights that skip missing dates), or
   rescale by observed/used. `missing` is already computed and returned for this purpose.
   There is no production impact until PIT groups are wired (W3-R4), because today's static
   group map gives every model sector members on every date.
6. **W3-R4 (caps wiring).** Once caps are passed, Size and every other style z can go past ±3
   on the small-cap side by \|μ_eq − μ_w\|/σ. This is by design (USE4). Downstream consumers
   must not assume \|z\| ≤ 3. No code outside `exposures.hpp` references `kZScoreWinsor`
   (grep). The whole risk target is green with the change. Out-of-group consumers are
   already on the integration-gate list (integration note 2).

### Ledger candidates (fix pass 1)
- W0-R0 fix 1: cap-weighted centring shifts z by about σ_ln for lognormal caps. A ±3 clip after
  centring would pin the small-cap tail (26 of 400 names at ln-cap sd 2). Trim the raw descriptor
  only (USE4).
- W0-R0 fix 1: the missing-factor return 0 enters F as an observation. This is a W1 covariance
  item, and `FactorReturnPanel::missing` carries the counts.
