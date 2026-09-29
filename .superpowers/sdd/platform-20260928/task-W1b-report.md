# Task W1b report: spo-v2 corrections after the R2 review
Branch feat/platform-v7-w1b-spo-20260929 (pool-3, base c8503bb3): cd01f74e (spo-v1 digest pin, base API only), 1e7655ac (W1b), this report. Nothing was compiled or run; expect one /W4 /WX pass.

| flag / rule | spo-v1 | spo-v2 |
|---|---|---|
| --spo-gross G: hard cap on planned gross, refused outside (0, 1.5 x --aim-leverage] | --aim-leverage | 1.0 |
| --alpha-horizon h (independent of --spo-horizon; NaN refused; uniform 1/sqrt(h), declared) | 1 | 21 |
| --specific-ceiling / --specific-ceiling-void on\|off | inf / off | 1.0 / on |
| gamma | max(gamma_vol, gamma_bind) | gamma_vol; gamma_bind after the aim, report only (NaN + gamma_bind_note) |
| recipe / summary / extras key | spo_v1 | spo_v2 |

- **Unchanged:** --spo-horizon 1/theta, ic .02, w-max .01, q .05, p .01, iters 500, tol 1e-8, target vol .05, beta .02. The aim-partial-v5 shadow keeps --aim-leverage. G is recorded as parameters.gross_budget in recipe v7.spo_v2, extras spo_v2 and summary v7.declarations.spo_v2.
- **Void (M-1):** v7::capture() now returns Status and runs after the replay, before publish_nav. With the void on, any clamped decision makes the replay fail before <output> exists. dispatch_nav_v7 then writes spo_diagnostics.csv, v7_transfer_coefficient.csv and v7_extras.json (status void, void_reason, spo_v2.tripwire) and exits 3. No recipe, summary, daily or events file is written and no "net Sharpe" line is printed. With the void on, --emit-holdings is refused.
- **Tripwire keys:** per book, capped_specific_decisions and capped_specific_names_max (renamed from max_capped_specific); per run, extras spo_v2.tripwire and summary v7.spo_v2_tripwire (status clear, void or tripped).
- **M-3 ledger correction:** exante_vol, exante_vol_current and exante_vol_shadow are annualised, sqrt(252 x daily variance). This is stated in the header, the declaration and extras diagnostics_units. v1's .0066 is .66%/yr, not "~10% ann" as progress.md says.
- **Files:** src/strategy_spo.{hpp,cpp}; src/strategy_nav_v7.{hpp,cpp}; src/strategy_nav_replay.cpp (one line: ATX_TRY_VOID(v7::capture)); tests/strategy_spo_test.cpp; new tests/strategy_spo_pin_test.cpp and tests/strategy_spo_fixture.hpp (shared, frozen); tests/CMakeLists.txt. strategy_risk_model.* is untouched.

**New tests**
- SpoPin.SpoV1PlannedWeightsOfTwoLockstepBooks_MatchThePinnedDigest and SpoPin.SpoV1ReplayAndDiagnosticsColumns1To38_MatchThePinnedDigest. Unpinned, so both fail and print `[spo-pin]`.
- SpoHook.SpoV2BindsTheGrossBudgetOnPostRampDecisionsWithPlannedGrossG: 60 names and 59 decisions; binds on >= 90% of post-ramp decisions; gross = G to 1e-10 when binding.
- SpoTripwire.TheClampFeedsAlphaAndTheVoidStopsTheRunAtCapture; SpoTripwire.AVoidRunExitsThreeWithDiagnosticsAndNoNavOrReturnFile (CLI end to end); SpoHook.ParseGrossBudgetAndVoidFlags_RefuseOutOfRangeAndHoldingsWithTheVoid; SpoCalibration.V2GammaBindFailureIsReportedAsNaNWithANoteAndNeverAborts.
- Updated: SpoHook.ParseSpoV2..., SpoHook.SpoV2CalibratesToTheVolTarget... (shadow alpha ratio now sqrt(21)), SpoHook.ParseRoutes..., SpoCalibration.V1GammaKeeps...

**Root commands**
0. Pin capture (W2 golden protocol): check out cd01f74e detached in pool-3, build (1), run `--gtest_filter=SpoPin.*` and read weights= and replay=. Check out feat/platform-v7-w1b-spo-20260929, build and run again: both values must match. Then pin pinned_weights and pinned_replay in tests/strategy_spo_pin_test.cpp.
1. `powershell -File build-equity\mega-build.ps1 -Tag v7-w1b -Targets atx-impl-strategy-target-tests,atx-equity-strategy-targets`
2. `build-equity\bin\atx-impl-strategy-target-tests.exe --gtest_filter=Spo*:NavV7Hook*:AimV6*:TransferCoefficient*:StrategyNavReplay*:NavV5*:TargetReplayV5*:StrategyLive*`
3. Identity (b), `--rule spo-v1` on pin 897ffdf2. Base line V61 (also used in 4):
   `python scripts/run_bounded_research.py --output <OUT>-run --seconds 180 --max-rss-mib 1536 --min-free-mib 512 -- build-equity\bin\atx-equity-strategy-targets.exe nav --combined build-equity/mega-v61w-train-ew-1/train_combined.json --combined-sha256 62bc30a3bf1ee047c200e35064f8cfe18c089a77adb3f651e4615b3c05d1c6c0 --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7/manifest.json --fields-sha256 1d1fa87a00d519bcf08fbec83f3fd17029e23650a98a9af26e99ddb1f1a73ee1 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache --ic-book .02 --w-max .01 --adv-cap-q .05 --adv-trade-p .01 --spo-iters 500 --spo-tol 1e-8 --target-vol .05 --spo-books primary`
   - Run V61 + `--rule spo-v1 --risk-model build-equity/v7-w1-risk-all --risk-model-sha256 897ffdf24748baf043031aef2ac559f9f17b3038c3ecbc00ba2b46bd9d19967e --output build-equity/v7-w1b-id-spo-v1` and compare with build-equity/mega-nav-v61u-spo-v1-L1.247.
   - Must match byte for byte: daily_ and events_ CSVs of linear-6bps-stale5-v1+swap-fin-v1 and modeled-1bn-stale5-v1+swap-fin-v1, and spo_diagnostics.csv columns 1-38 including the header (session..rho, 1,508 rows).
   - Expected identical: v7_transfer_coefficient.csv. Expected to differ, by design: recipe.json, summary.json, v7_extras.json, and diagnostics columns 39-44 (the rejected cell has no such columns).
4. spo-v2 cell, after F3's pin and the prereg line: V61 with <OUT> = build-equity/mega-nav-v61u-spo-v2-L1.0, plus `--rule spo-v2 --risk-model <RISK_DIR> --risk-model-sha256 <RISK_SHA> --spo-gross 1.0 --alpha-horizon 21 --specific-ceiling 1 --specific-ceiling-void on --output build-equity/mega-nav-v61u-spo-v2-L1.0`. Exit 3 is VOID, not a trial: read v7_extras.json spo_v2.tripwire. On exit 0, check tripwire.status == "clear" before reading any return.

**Untested (no build)**
- /WX and every gtest.
- The binding test's synthetic magnitudes are estimates: diagonal GK gives an aim gross of ~4-6 against G 1, and alpha 3.9e-4 z per session against a 3e-5 amortized cost and 1.2e-4 borrow.
- Admission of the CLI test's artifact, which reuses the nav-replay layout.
