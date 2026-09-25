# Lane W0-D0 review

## Verdict
APPROVE

Zero blocker, zero major. Four minor findings below. Owner waivers are needed for three items
that cannot be settled inside the lane (vwap deviation; build_real_panel wiring and golden digest
left to G0).

## Reviewed SHA
`1f2810740d14efbaf9a9443fb43860e68fa751a7` (feat/w0-d0). Merge base with feat/w0-integration =
W0 base `458d0bef480a624e258070c9d45174a9984466bf`.

## Evidence
All commands run by the reviewer in `C:\atx-wt\pool-4`, preset equity-dev,
`CMAKE_BUILD_PARALLEL_LEVEL=2`, free RAM 5.15 GB before building.

Builds (exit 0):
```
build -Preset equity-dev atx-engine-data-tests   -> [9/10] Linking CXX executable bin\atx-engine-data-tests.exe  exit=0
build -Preset equity-dev atx-engine-alpha-tests  -> [10/11] Linking CXX executable bin\atx-engine-alpha-tests.exe exit=0
```
Single-TU `/W4 /WX` compiles of every changed source (exit 0, 0 warnings other than the
pre-existing `/MP unused` driver note):
```
adjust / align / context / corporate_actions / finra_short / history_panel / real_panel / universe
  check exit=0 warnings=0   (each)
```
Lane suites, direct (`atx-engine-data-tests.exe --gtest_filter=*W0d0* --gtest_brief=1`):
```
[tri-gap] 3% payer, 5y: RatioChainV2 step=0.000000  ReanchorV1 step=-0.132351
[align-event] dividend total legacy=2.00 event-join=0.50 (true 0.50)
[corp-tie] legacy leaked post-split shares on 3 pre-split days; V2 on 0
[corp-rebase] market-cap step at split: V2=0.000 legacy=-0.500
[finra-lag] Thu 2019-03-14 usable=17981 legacy_early_sessions=1   (Fri 2, MLK 2, Thanksgiving 2, Bush 3, Good Fri 1, Mon 0)
[finra-lag] loader: legacy leaked 11 cells, NYSE rule leaked 0
[perturb] history cells compared=3060 augmented cells compared=5355
[resnapshot] CloseV1 dollar_volume cells halved=112/112; RawCloseV2 moved=0
[real-candle] max |open/close - raw ratio|: TriScaledV2=2.22e-16 MixedV1=0.99
[==========] 36 tests from 8 test suites ran. (1300 ms total)
[  PASSED  ] 36 tests.   exit=0
```
Anchored ctest (`scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'`), all exit 0:
```
^DataLevelBasis_ 6/6  ^DataFinraLag_ 4/4  ^DataAdjustGap_ 5/5  ^DataAlignEvent_ 7/7
^DataCorpActRebase_ 5/5  ^DataContextAsOf_ 3/3  ^DataHistoryPanelFuturePerturb_ 2/2
^DataUniverseNanFloor_ 4/4   (100% tests passed each)
```
Whole data executable, excluding only the 14 tests that read real data under `C:\atx\data`
(DataRealPanel.* x5, DataCorporateActions smoke x5, DataAdjust.SplitAdjustedNoDiscontinuityAtKnownAaplSplits,
OratsE2ESmoke.{RealPartition..., OperatorOratsZip}, DataUniverse.SurvivorshipCaveatDocumentedOrDeferred):
```
[==========] 234 tests from 36 test suites ran. (17311 ms total)
[  PASSED  ] 234 tests.   exit=0
+ OratsE2ESmoke.SyntheticPartitionRunsUnchangedRobustPipeline (synthetic): 1 test PASSED exit=0
=> 235 passed, matching the report's count.
```
Whole alpha executable (`atx-engine-alpha-tests.exe --gtest_brief=1`):
```
[==========] 678 tests from 255 test suites ran. (68870 ms total)
[  PASSED  ] 678 tests.   exit=0
```

## Findings
path:line | severity | problem | required fix
---|---|---|---
`atx-engine/src/data/adjust.cpp:96-100` | minor | RatioChainV2 resumes TRI = prev_tri·S_t/S_last with r = 0, so a cash dividend whose ex-date is the resumption cell (or falls on a NaN-close cell inside the gap; with D-05's event join the dividend now lands on exactly that one session) is dropped: close {100, NaN, 99}, div {0, 0, 1.0} gives TRI 99, a phantom -1% step. The -14% accumulated-dividend loss (D-04) is fixed exactly as the plan's formula specifies; this is a residual one-dividend gap the formula does not cover and no test pins it. | Follow-up (owned file): carry the split-basis dividends seen on gap cells plus the resumption cell into the resume step, TRI = prev_tri·(S_t + ΣD_adj)/S_last, and add a DataAdjustGap test with the ex-date on the resumption cell and one inside the gap.
`atx-engine/include/atx/engine/alpha/augment.hpp:33,204` (+ report "Build items" row "vwap") | minor | The brief's build item says "Build dollar_volume, adv{d} and vwap from raw_close × volume". vwap was deliberately left as the typical price on close's (adjusted) basis and tagged adjusted_level. The reasoning (a raw vwap next to an adjusted close would recreate D-03 and put the snapshot factor into close/vwap) is sound, but the report marks the row "MET (with deviation)". It is a deviation from the brief, not a met item. | Relabel the row DEVIATION and get an owner waiver (listed under waiver_needed). No code change is required if the owner accepts it.
`atx-engine/src/data/real_panel.cpp:415,447`; `atx-engine/tests/data/data_real_panel_e2e_test.cpp:169` | minor | The D-03 candle restatement and the D-05 corp align options are wired into `build_real_panel`, but no in-lane test goes through that function. The databento hive needs a UInt64 volume and the atx-core writer has only i64/f64/string/timestamp, which I confirmed at `real_panel.cpp:181` (`column_view<atx::u64>`). The smoke golden digest `0x2a22a873483d9157` is knowingly stale (it will fail at G0), and two G0 risks are untested: the new D-05 `require_coverage` guard (OutOfRange if price dates pass the master), and the V2 row-dated shares rule on the real master (`DataCorporateActions.SharesOutstandingPitForwardFill` expects the 2009 filing to be visible on its filed date). I read the wiring and it is correct. | G0 (orchestrator) must run the 14 excluded real-data tests, re-pin `kGoldenDigest` with an old->new row tied to D-03/D-04/D-05/D-09, and confirm the coverage guard and the SharesOutstandingPitForwardFill test pass. Listed under waiver_needed.
`atx-engine/src/data/history_panel.cpp:80-82` (`history_field_level_basis`) | minor | The tags are name-only: `dollar_volume`/`adv{d}` are always Raw. They are wrong for a panel augmented with `DollarVolumeBasis::CloseV1`, and for any caller that runs `datafields::with_datafields` directly on a history panel, which derives adv from the adjusted close. One such caller is `atx-impl/src/stage_discover.cpp:315` (capacity screen), whenever the incoming panel has no adv{W} yet. That caller is not in the report's Integration notes. | Add an Integration note for `stage_discover.cpp:315` (use raw_close or with_alpha101_fields). Document in `history_field_level_basis` that the Raw tag holds only for RawCloseV2 augmentation, or let the tag take the DollarVolumeBasis.

## Defect closure (read at the cited code)
- D-01 CLOSED: `augment.hpp:196-258` pre-derives dollar_volume = raw_close·volume (universe-masked) and adv{d} = `rolling_mean` of it, with the same order, masks and formulas as `datafields::with_datafields` (I compared it line by line). Tags are in `HistoryPanel::field_basis` and `RealPanel::field_basis`, and a build fails with Internal on an untagged field. `stage_equity_mine.cpp:1375` gets the fix with no edit. `FactorResnapshotLeavesRawLiquidityInvariant` would fail on the old code (CloseV1 halves 112/112 cells).
- D-02 (engine) CLOSED: `finra_short.cpp:195-244`. I checked the NYSE session calendar by hand on all 7 fixtures, including 1990/2040 day constants, the unscheduled-closure list, the pre-1998 MLK exception and Juneteenth from 2022 (atx-core). The legacy `int` overload keeps CalendarDaysV1, and every existing caller passes an explicit int. I0b owns the config default (noted in the report).
- D-03 CLOSED (function level; wiring unexercised, see Findings): `real_panel.cpp:349-362,434-450`.
- D-04 CLOSED per the plan formula: `adjust.cpp:94-102`. `data_adjust_test.cpp:393` still expects 105 (only the comment changed).
- D-05 CLOSED: `align.cpp:108-157`. For the staleness calculation, `lower_bound` over available_dates gives index <= d whenever the as-of row exists, so there is no unsigned underflow. Pre-axis rows fail closed and the coverage guard is in place. `corp_action_align_options` is used by `build_real_panel`.
- D-06 (rebase + tie) CLOSED: `corporate_actions.cpp:333-392`. The heap-based incremental resolution only takes rows dated <= d, and the rebase is f(d)/f(row), consistent with `split_adjusted = raw·factor`. D-06 PIT-shares source DEFERRED to W2-D3, as briefed and justified.
- D-08 CLOSED: `context.cpp:175-195` returns Err in every build type and leaves the cache untouched. Moved-from accessors return Err instead of dereferencing null.
- D-09 CLOSED: `universe.cpp:176-235`. A disabled floor passes NaN, an enabled floor fails it, NaN ADV sorts last in top-N (a strict weak order), and the new traded-price guard keeps `MembershipExcludesNanPriceCells` green without changing its expectation.

## Checked
- [x] .agents/cpp/agent.md §10 checklist on the diff. No UB found. The references in `augment.hpp` into `data` are not used after `data.push_back`. Casts are explicit and nothing narrows implicitly. Loops are bounded (the NYSE walk is capped by the calendar range). Every switch is exhaustive with a fallback return, unknown enum values fall to a defined rule, and lengths and lags are validated with Err paths. Changed TUs are clean under `/W4 /WX`. I did not run the hygiene preset (PCH off, full reconfigure; not affordable at this RAM). I checked includes by hand instead: `<queue>`, `<functional>`, `<array>`, `<cmath>`, `<algorithm>` and `<optional>` were added where used.
- [x] File ownership: `git diff --name-only feat/w0-integration...HEAD` is 16 owned src/header files, 8 new `tests/data/data_w0d0_*_test.cpp` files, 2 existing tests with comment-only edits (D-04, D-09) and the lane report. There are no CMake edits and no new `src/*.cpp`. `augment.hpp` changes are limited to the dollar_volume/adv/vwap derivation, its signature and its doc comment.
- [x] No test weakened. Existing test diffs change comments only; no expectation or tolerance changed, and no DISABLED_ or GTEST_SKIP was added.
- [x] Versioned enums keep every old behaviour: DollarVolumeBasis::CloseV1, FinraLagRule::CalendarDaysV1 (+ int overload), TriGapRule::ReanchorV1, CorpAlignRule::AsOfForwardFillV1 / empty AlignOptions, SharesPitRule::AsFiledV1, NanFloorRule::NanFailsV1, RealPanelPriceBasis::MixedV1. Defaults move to V2. There is a golden-digest table, but its "new" value is not measured (see Findings).
- [x] Acceptance items re-run and test code read:
  - Future perturbation: it compares all fields, the mask, the basis tags and the Alpha101-derived fields at t in {4, 11, 12, 20}. There is a control (row t+1 must differ) and a non-trivial mask (warm-up and binding top-N). Note that this invariance also held on the legacy code, because the factor is row-local. It is a guard, not a detector. The D-01 look-ahead itself is detected by `FactorResnapshotLeavesRawLiquidityInvariant`. It covers the uncompacted panel only (`compact_to_universe=false`, the default); the header documents the compaction caveat.
  - FINRA Thu/Fri/holiday: 7 hand-verified fixtures plus a loader test on a real session axis (11 legacy leaked cells, 0 V2).
  - 3% payer across a gap: step 0.000000 vs -0.132351 legacy; it would fail on the old code.
- [x] Evidence in the report matches what I re-ran (36 lane tests, 235 data, 678 alpha, 8 anchored suites).
- [x] Owning executables pass whole, except the real-data tests that data discipline forbids a lane from running (G0 item).
