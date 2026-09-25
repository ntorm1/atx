# Lane W0-E0a report — Inference: HAC, block length, execution delay, caps

## Outcome

DONE. All five accept items are met with named tests and measured numbers. All five cited
defects (E-02, E-03, E-08, E-09, E-15) are closed inside the owned files. Both owning test
targets are green: eval 222/222 and combine 183/183.

Two things are narrower than the brief's wording, and both are explained under Deviations:

- The combine t-stat rule can be selected only at the kernel level. The combiner structs
  live in headers this lane does not own.
- The bootstrap interval under the new block rule measured 93.0% coverage. The published
  HAC interval is the one held to the [93%, 97%] band, and it measured 94.5%.

## Branch / SHA

- Branch: `feat/w0-e0a` in `C:\atx-wt\pool-5`.
- Code commits:
  - `2ba5802b` pins the frozen digests measured on the base.
  - `2f814fbe` is the implementation plus tests.
  - `cc353a47` updates a header doc comment.
- This report is committed on top of those. The final HEAD is the SHA the lane returns.
- The tree is clean after the report commit.

## Frozen base / lease

- `base_sha=458d0bef480a624e258070c9d45174a9984466bf`, the W0 base.
- `worktree=C:\atx-wt\pool-5`, `lease_run_id=aes-w0-e0a`.
- The orchestrator took the lease before dispatch. This lane never ran
  `lease-worktree.ps1`.
- `git merge --no-ff feat/w0-integration` returned "Already up to date."

## Files changed

All files are owned by this lane, or are new test or fixture files that RULES §2 allows.

| File | Change |
|---|---|
| `atx-engine/include/atx/engine/eval/hac.hpp` | **new**, header-only. Contents: `Kernel {BartlettV1, UniformV1}`; `TStatRule {IidV1, NeweyWestAutoV2}` with default `NeweyWestAutoV2`; `long_run_sum`; `mean_inference`, which matches statsmodels OLS-on-constant with `cov_type='HAC'`; `newey_west_rule_of_thumb_lag`; `newey_west_auto_lag` (Newey-West 1994); `mean_tstat`; `ewma_variance_inflation`; `politis_white` (the Politis-White automatic block length). |
| `atx-engine/include/atx/engine/eval/cross_section_ic.hpp` | **Enums and config:** `BlockLenRule {HalfHorizonV1, TwoHorizonV2 (default), PolitisWhiteV2}`; `IcHacRule {HansenHodrickV1 (default), NeweyWestV1}`; new config fields `block_len_rule`, `execution_delay` (default 1), `hac_rule` and `max_working_bytes` (default 1 GiB). **Output fields:** `HacInterval` (`ic_mean_hac` and `rank_ic_mean_hac` on each sample block) and `IcHorizonSummary::{execution_delay, embargo, block_len_rule}`. **New functions:** `label_embargo`, `detail::block_len_for_rule`, `IcSizing` and `preflight_cross_section_ic`, `bootstrap_mean_interval`. **Caps:** 4096 became sanity bounds of 65,536 dates and 262,144 instruments. |
| `atx-engine/src/eval/cross_section_ic.cpp` | Validation of the new rules. Preflight replaces the fixed caps. The entry row is `t + delay`, and it drives prices, the terminal triple and `days_forward`. The evaluable range is `T - h - delay`. `make_hac` fills the HAC intervals. The PolitisWhiteV2 block is computed per horizon. `draw_percentiles` is now shared with the public `bootstrap_mean_interval`. |
| `atx-engine/src/combine/signal_combiner.cpp` | T-stat sites only. `column_tstats` now calls `hac::mean_tstat`. ICIR-EWMA computes `t = icir * sqrt(n_eff / VIF)`. |
| `atx-engine/src/combine/orthogonalize.cpp` | The `marginal_ic` t-stat only, now `hac::mean_tstat`. |
| `atx-engine/include/atx/engine/combine/signal_store.hpp` | The winsor fix only (E-15). `SignalNormalize::ZScore` now clips after standardizing and does nothing further. The appended `ZScoreRestandardizeV1` keeps the old behaviour. `IcReturnTreatment {RawV1, WinsorizedV2 (default)}` controls the returns inside `ic_matrix`. New helper `winsorize_row_copy`. |
| `atx-engine/tests/eval/eval_w0e0a_frozen_v1_test.cpp` | **new.** Suite `EvalHac`: digests measured on the base, and the new default digests pinned. |
| `atx-engine/tests/eval/eval_w0e0a_hac_test.cpp` | **new.** Suite `EvalHac`. |
| `atx-engine/tests/eval/eval_w0e0a_ic_coverage_test.cpp` | **new.** Suite `EvalIcCoverage`. |
| `atx-engine/tests/eval/eval_w0e0a_ic_delay_test.cpp` | **new.** Suite `EvalIcDelay`. |
| `atx-engine/tests/eval/eval_w0e0a_ic_caps_test.cpp` | **new.** Suite `EvalIcCaps`. |
| `atx-engine/tests/combine/combine_w0e0a_hac_tstat_test.cpp` | **new.** Suites `CombineHacTstat` and `CombineHacTstatStoreWinsor`. |
| `atx-engine/tests/eval/fixtures/gen_hac_statsmodels_fixture.py` | **new.** The committed generator. Uses statsmodels 0.14.1 and numpy 1.26.4 under `py -3.12`. No network. |
| `atx-engine/tests/eval/fixtures/hac_statsmodels_fixture.hpp` | **new.** Generated output of the script above. |
| `atx-engine/tests/eval/eval_cross_section_ic_test.cpp` | `good_config` now sets `block_len_rule = HalfHorizonV1` and `execution_delay = 0`. See the defect table. |

No `CMakeLists.txt` edits and no new `src/*.cpp`.

## Acceptance table

| Plan accept item | Test(s) | Measured result | Status |
|---|---|---|---|
| NW t matches a statsmodels fixture to 1e-8 | `EvalHac.MeanInference_MatchesStatsmodelsFixture_To1e8` (plus `LagRules_MatchStatsmodelsDefaultAndIndependentNw94` and `PolitisWhite_MatchesIndependentReference`) | 27 statsmodels cases: Bartlett and uniform kernels, lags 0/5/20, with and without `use_correction`, on AR(1) n=300, MA(20) n=500 and white-noise n=25 series. Max \|t − t_sm\| = **1.066e-14**; max \|se/se_sm − 1\| = **1.739e-15**. The fixture comes from real statsmodels, not a reimplementation, so this item is not marked PARTIAL. | MET |
| A simulated MA(20) IC series gives 95% CI coverage in [93%, 97%] over 2000 repetitions | `EvalIcCoverage.HacCi_Ma20Series_CoverageWithin93To97PercentOver2000Reps` | Setup: n=1750, 2000 reps. The default published HAC interval (`IcHacRule::HansenHodrickV1`) covered **0.9450**. For contrast: NeweyWestV1 0.9135 and naive IID **0.3145**. Uniform-to-Bartlett fallbacks: 0. `EvalHac.CrossSectionIc_HacFieldsAreHacOfTheEmittedIcSeries` shows that `ic_mean_hac` is exactly this routine applied to the emitted series. Bootstrap for comparison (`BootstrapBlockRule_V2MovesMa20CoverageTowardNominal`, n=1000, B=199, 400 reps): V1 (L=11) **0.8175**, V2 (L=42) **0.9300**. | MET (on the HAC interval; see Deviations) |
| A same-day reversal signal's IC collapses at delay 1 | `EvalIcDelay.SameDayReversal_IcCollapsesAtDelayOne` | Panel of 300 × 200, h=1. Delay 0: IC **0.4852** (theory 0.488), HAC t 144.0, rank IC 0.4671. Delay 1: IC **0.0029**, HAC t 0.58, rank IC 0.0014. | MET |
| A panel of 6,624 ids × 1,750 dates is accepted | `EvalIcCaps.T3000Union_6624IdsBy1750Dates_IsAcceptedAndComputed` (plus `SixteenK_PreflightAcceptsAndBothExtentsCompute`) | Preflight, plan and compute all succeed on 11,592,000 cells. Every one of the 1,728 dates emits with n_used 6624, block 42, and both the bootstrap interval and the HAC interval are reportable. The engine's working set is 746,616 B against a 1 GiB budget. **16,384**: a 16384 × 16384 preflight is accepted (working set 3.67 MB; caller input 9.66 GB), and 30 × 16384 and 16384 × 3 panels both compute. | MET |
| Frozen streams reproduce under V1 | `EvalHac.FrozenStreams_ReproduceUnderV1` (plus all 63 pre-existing `EvalCrossSectionIc*` cases) | The four digests were measured by this test on base `458d0bef` in commit `2ba5802b`, before any code change. They are reproduced exactly under `HalfHorizonV1` with `execution_delay = 0`. Coverage: 5 horizons × {Drop, Terminal} × {full, ex34}, with every point, summary and interval bit folded in. `FrozenStreams_DefaultsMoveOffV1AndArePinned` shows that each correction alone moves every stream. | MET |

## Defect table

| ID | Status | How / where |
|---|---|---|
| E-02 | CLOSED | Added `BlockLenRule` in `cross_section_ic.hpp`. The default `TwoHorizonV2` gives L = max(floor, 2h): {5, 10, 20, 42, 126} for h ∈ {1, 5, 10, 21, 63}. `PolitisWhiteV2` lengthens L to ceil(b_CB) of the horizon's IC series. `HalfHorizonV1` keeps the old streams bit for bit. Measured on MA(20) bootstrap coverage: 81.8% → 93.0%. |
| E-03 | CLOSED | **eval:** the HAC `ic_mean_hac` and `rank_ic_mean_hac` are published beside `naive_t` (Hansen-Hodrick at lag max(h−1, NW rule of thumb); NW option). **combine:** `column_tstats` (GK/FMB/Kakushadze), ICIR-EWMA (through the weighted VIF) and `marginal_ic` use `TStatRule::NeweyWestAutoV2`, and `IidV1` is reproducible at the kernel level. Measured: null rejection on MA(20) 67.2% (IID) → 13.3% (HAC). ICIR-EWMA t 8.49 → 2.51 (VIF 11.5), so the haircut now bites. marginal_ic t 4.55 → 1.34. |
| E-08 | CLOSED | The 4096 caps became sanity bounds (65,536 / 262,144). `preflight_cross_section_ic` computes exact, overflow-checked scratch and result bytes and enforces `max_working_bytes` before any `.assign()`. 6,624 × 1,750 is computed end to end. The budget is exact to the byte (`WorkingSetBudget_IsExactAndBinding`). |
| E-09 | CLOSED (engine) | `execution_delay` defaults to 1. The forward window is rows t+d → t+d+h. Prices, the terminal triple and `days_forward` are read at the entry row. `label_embargo(h, d) = h + d` is exported and echoed per horizon. **Deferred:** the consumer wiring at `atx-impl/src/stage_equity_ic.cpp:112-113` belongs to W0-I0b, as the brief says (see Integration notes). |
| E-15 | CLOSED | `SignalNormalize::ZScore` standardizes and then clips, so every stored value is within ±winsor. Measured: max \|z\| 3.0000, against 4.2426 under the old re-standardizing. The old row is kept as `ZScoreRestandardizeV1` (bit-exact, tested). `ic_matrix` correlates against returns winsorized per date at mean ± 3 sd by default (`IcReturnTreatment::WinsorizedV2`), and `RawV1` gives the old IC exactly. Measured on one outlier: raw IC −0.104, winsorized IC 0.119, clean IC 0.396. |

**Changed existing test (RULES §2):** In `eval_cross_section_ic_test.cpp`, `good_config()` now sets `cfg.block_len_rule = BlockLenRule::HalfHorizonV1; cfg.execution_delay = 0U;`. Every case in that file pins checkpoint-14 numbers, which were published under the E-02 block rule and the E-09 signal-close convention. With these two lines the file keeps pinning those frozen numbers through the versioned legacy path. No expectation value was edited, and no test was skipped or deleted.

## Evidence

All commands were run from `C:\atx-wt\pool-5`, each in its own PowerShell call.

1. Reconfigure. The cache had `ATX_TEST_GROUPS=eval` and the brief needs `eval;combine`.
   Command: `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev -Groups "eval;combine"`.
   Result: `-- Build files have been written to: C:/atx-wt/pool-5/build-equity`, and the cache then read `ATX_TEST_GROUPS:STRING=eval;combine`. Exit code 0.
2. Baselines on the unchanged base, for the must-stay-green comparison:
   - eval: `[==========] 195 tests from 25 test suites ran. [  PASSED  ] 195 tests. exit=0`
   - combine: `[==========] 177 tests from 32 test suites ran. [  PASSED  ] 177 tests. exit=0`
3. Frozen digests measured on the base, before any code change (commit `2ba5802b`):
   ```
   W0E0A_FROZEN_DIGEST variant=1 restriction=0 digest=0x3eff877612745386
   W0E0A_FROZEN_DIGEST variant=1 restriction=1 digest=0xdee7f78c1999af44
   W0E0A_FROZEN_DIGEST variant=2 restriction=0 digest=0x5f2c2f0e33647808
   W0E0A_FROZEN_DIGEST variant=2 restriction=1 digest=0x0d8febe9dd429f91
   ```
4. Type checks, each exit 0, no diagnostics: `scripts\atx-build.ps1 check -Preset equity-dev` on `atx-engine\src\eval\cross_section_ic.cpp`, `atx-engine\src\combine\signal_combiner.cpp` and `atx-engine\src\combine\orthogonalize.cpp`.
   Output tail: `[1/2] Building CXX object atx-engine\CMakeFiles\atx-engine.dir\src\...obj`.
5. Final build, with free RAM at 3.90 GB:
   Command: `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-eval-tests atx-engine-combine-tests`.
   ```
   [17/19] Linking CXX executable bin\atx-engine-eval-tests.exe
   [18/19] Linking CXX executable bin\atx-engine-combine-tests.exe
   build_exit=0
   ```
6. Anchored suites. Command: `powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'`.
   ```
   ^EvalHac          100% tests passed, 0 tests failed out of 14   (5.61 sec)   exit=0
   ^EvalIcCoverage   100% tests passed, 0 tests failed out of 3    (6.70 sec)   exit=0
   ^EvalIcDelay      100% tests passed, 0 tests failed out of 6    (0.92 sec)   exit=0
   ^EvalIcCaps       100% tests passed, 0 tests failed out of 4    (119.87 sec) exit=0
   ^CombineHacTstat  100% tests passed, 0 tests failed out of 6    (1.16 sec)   exit=0
   ```
   Per-test timing: `EvalIcCaps.T3000Union_6624IdsBy1750Dates_IsAcceptedAndComputed` passed in 88.48 s (Debug `/Od`). Every other new test takes 5 s or less.
7. Whole owning executables, run once each:
   ```
   build-equity\bin\atx-engine-eval-tests.exe --gtest_brief=1
   [==========] 222 tests from 29 test suites ran. (104029 ms total)
   [  PASSED  ] 222 tests.
   eval_exit=0
   build-equity\bin\atx-engine-combine-tests.exe --gtest_brief=1
   [==========] 183 tests from 34 test suites ran. (8825 ms total)
   [  PASSED  ] 183 tests.
   combine_exit=0
   ```
   The `CHECK failed` lines in both runs come from existing death tests. They were present on the base as well.
8. Measurement lines printed by the new tests (verbatim):
   ```
   [EvalHac] statsmodels fixture: 27 cases, max |t - t_sm| = 1.066e-14, max |se/se_sm - 1| = 1.739e-15
   [EvalHac] MA(20) n=500: iid t=1.2820, NW-auto t=0.3669 (lag 17), ratio 3.495 (sqrt(21)=4.583)
   [EvalHac] PolitisWhiteV2 on a phi=0.97 IC series: b_CB=43.305 -> L=44 (TwoHorizonV2 gives 5)
   [EvalIcCaps] 6624 x 1750: cells 11592000, input 417326000 B, scratch 492528 B, result 254088 B, working 746616 B (budget 1073741824 B)
   [EvalIcCaps] 6624 x 1750 computed: dates emitted 1728, n_used/date 6624, IC mean 0.22512, HAC t 13.552
   [EvalIcCaps] 16384 x 16384 preflight: working 3672616 B, caller input 9663807488 B
   [EvalIcCoverage] MA(20), n=1750, 2000 reps: HansenHodrickV1 0.9450  NeweyWestV1 0.9135  naive-IID 0.3145  (uniform->Bartlett fallbacks 0)
   [EvalIcCoverage] CBB on MA(20), n=1000, B=199, 400 reps: HalfHorizonV1 (L=11) 0.8175  TwoHorizonV2 (L=42) 0.9300
   [EvalIcDelay] same-day reversal, 300 dates x 200 names, h=1: delay 0 IC 0.4852 (HAC t 144.00, rank IC 0.4671)  delay 1 IC 0.0029 (HAC t 0.58, rank IC 0.0014)
   [CombineHacTstat] MA(20) null, T=500, 600 columns: |t|>1.96 rate NW-auto 0.1333, IID 0.6717 (nominal 0.05)
   [CombineHacTstat] Kakushadze alpha-return t: NW-auto 3.042 vs IID 8.133
   [CombineHacTstat] ICIR-EWMA on an h=21 overlapping IC: t IID 8.487 -> HAC 2.506 (VIF 11.47)
   [CombineHacTstat] marginal_ic t: site 1.3430, NW-auto on rebuilt series 1.3430, IID 4.5506
   [CombineHacTstatStoreWinsor] winsor 3: max |z| ZScore 3.0000, ZScoreRestandardizeV1 4.2426
   [CombineHacTstatStoreWinsor] IC clean 0.3963, raw-with-outlier -0.1039, winsorized 0.1191
   ```
9. Fixture generation. Command: `py -3.12 atx-engine/tests/eval/fixtures/gen_hac_statsmodels_fixture.py`.
   Output: `wrote ...\hac_statsmodels_fixture.hpp (27 HAC cases)`. Exit code 0.

## Golden-digest old → new

The table below is for the frozen-stream panel in `eval_w0e0a_frozen_v1_test.cpp`: 260 × 40, horizons {1, 5, 10, 21, 63}. "Old" is the base behaviour, which is still reproduced under V1 with delay 0. "New" is the new defaults, `TwoHorizonV2` with `execution_delay = 1`.

| Configuration | Old (base, = V1 with delay 0) | New (defaults) | Defect |
|---|---|---|---|
| DropMissingForward, full | `0x3eff877612745386` | `0xbc4a3f1cf4b04442` | E-02, E-09 |
| DropMissingForward, ex34 | `0xdee7f78c1999af44` | `0xba5d12625946780b` | E-02, E-09 |
| IncludeAuditedTerminalV1, full | `0x5f2c2f0e33647808` | `0x29a588c156f4db4d` | E-02, E-09 |
| IncludeAuditedTerminalV1, ex34 | `0x0d8febe9dd429f91` | `0x4b6ee3e63e34da79` | E-02, E-09 |

No existing golden digest in the eval or combine targets needed a new baseline. The combine zoo print line `[zoo OOS IR/day]` moved slightly (FMB 0.3455 → 0.3456, GK 0.3318 → 0.3325) because of E-15. It is printed for information only, and its test's assertions still pass.

## Deviations from brief

1. **The combine t-stat rule cannot be selected per combiner.**
   - What was done: E-03 is fixed at the three combine sites, and the default moved to `NeweyWestAutoV2`. The old behaviour is reproducible at the kernel level:
     - `eval::hac::mean_tstat(x, TStatRule::IidV1)` equals the old formula bit for bit (tested);
     - `ewma_variance_inflation(..., IidV1)` returns exactly 1.0, which gives back the old ICIR-EWMA t bit for bit.
   - What is missing: `IcirEwmaCombiner`, `GrinoldKahnCombiner`, `FamaMacBethRidge`, `KakushadzeRegression` and `marginal_ic` have no `tstat_rule` field or parameter. Adding one needs `combine/signal_combiner.hpp` and `combine/orthogonalize.hpp`, which this lane does not own. So a frozen combiner fit cannot yet re-run through its public struct under IidV1. See the integration note.
2. **Which interval the coverage item is measured on.**
   - The acceptance item is met on the HAC interval that `cross_section_ic` publishes (`ic_mean_hac`): 94.5%.
   - The circular-block bootstrap under the new `TwoHorizonV2` rule measured 93.0% (n=1000, B=199, 400 reps), up from 81.8% under V1. Its taper, which works like a Bartlett kernel, keeps it a few points short of nominal at L = 2h.
   - Both numbers are recorded honestly above. If the owner wants the *bootstrap* interval held to [93%, 97%] at every n, the lever is `PolitisWhiteV2` or a longer block.
3. **Test runtime.** The 6,624 × 1,750 end-to-end test runs in about 88 s under Debug `/Od`. It aliases the read-only spans to keep memory near 120 MB. It uses one horizon to bound the time.
4. **No full-label runs, and atx-impl was not built.** RULES forbid full-label runs in a lane, and atx-impl is outside this lane's targets. So the atx-impl consumers were not rebuilt or run here.
   - They should still compile: every new config field has a default, and no signature was removed.
   - Their numbers will move under the new defaults. See the integration notes.

## Integration notes

- **W0-I0b, `atx-impl/src/stage_equity_ic.cpp`**
  - **E-09 wiring.** The config at lines ~1160-1173 (`eval::CrossSectionIcConfig base{}`) now picks up `execution_delay = 1`, `block_len_rule = TwoHorizonV2` and `hac_rule = HansenHodrickV1` by default. To re-derive the checkpoint-14/22 artifacts, set `base.block_len_rule = eval::BlockLenRule::HalfHorizonV1; base.execution_delay = 0;`.
  - **Common sample (required for new runs).** `common_sample_dates = dates - kHorizons.back()` (line ~1172) must become `dates - eval::label_embargo(kHorizons.back(), base.execution_delay)`. Under delay 1, the old value puts signal row `dates - 64` into the h=63 prefix. That row has no exit row, so it counts as a prefix gap and voids the h=63 common block.
  - **Metadata strings.** `kAlignment` (line 112, "signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1") must describe the delay. The `"block_len_rule", "max(5, ceil(h/2))"` string (line ~334) and `trial_ledger.hpp:89-90` (`block_len_rule` / `block_lens {5,5,5,11,32}`) must follow the rule actually used. For `TwoHorizonV2` that is "max(5, 2h)" with {5, 10, 20, 42, 126}.
  - **New outputs to serialize.** Serialize `full/common.ic_mean_hac` and `rank_ic_mean_hac`: t, se, lo, hi, lag, kernel, fell_back, reportable, and `unreportable_reason`. That reason field adds the HAC-only code 5, "zero long-run variance". Also serialize `IcHorizonSummary::{execution_delay, embargo, block_len_rule}`.
  - **Tests that may move.** Expected values in `stage_equity_ic_test.cpp`, `fundamental_zoo_test.cpp` and `trial_ledger_test.cpp` may shift.
- **Owner of `combine/signal_combiner.hpp` and `combine/orthogonalize.hpp` (W1-E1 or W2-E3)**
  - Add `eval::hac::TStatRule tstat_rule{eval::hac::kDefaultTStatRule}` to the four combiner structs, and a matching `marginal_ic` parameter. Then thread it to `column_tstats`, the ICIR-EWMA site and `marginal_ic`. The kernels already take the rule.
  - Update the `MarginalIc::tstat` doc comment (`orthogonalize.hpp:65`, which still says "mean_ic / (sd/√n)").
  - `combine/hrp.hpp:386` still computes an IID t-stat. It is outside this lane's file set, so E-03 at that site is left to the hrp owner. The fix is `eval::hac::mean_tstat(col, kDefaultTStatRule)`.
- **Walk-forward and lockbox consumers (W0-E0b E-17, W1-E1).** Embargo at least `eval::label_embargo(h, execution_delay)` rows between a fit window's last signal row and the first evaluated row.
- **Stage-1-sized runs (189 dates).** Under `TwoHorizonV2`, horizons 10 and above become bootstrap-unreportable, because floor(n/L) < 10. The HAC interval needs n/(lag+1) ≥ 10. This is the intended E-02 behaviour, not a defect.

## Ledger candidates

1. Measured on MA(20) IC coverage with n=1750 and 2000 reps: Hansen-Hodrick at lag max(h−1, NW rule of thumb) gives 94.5%. Newey-West at the auto lag gives 91.4%. The naive IID interval gives 31.5%.
2. Measured on the circular-block bootstrap for MA(20) with n=1000: the V1 block of 11 covers 81.8%. A 2h block of 42 covers 93.0%. The Politis-White b_CB for an MA(20) series of n=500 is about 30.
3. Measured `cross_section_ic` cost at 6,624 × 1,750 with one horizon: the engine's working set is 0.75 MB and caller spans are 417 MB. Debug compute takes about 88 s, mostly in the per-date sorts.
