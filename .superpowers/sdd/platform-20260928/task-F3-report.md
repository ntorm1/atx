# Task F3 report: risk model robust to one outlier (R2 I-2) -- atx-risk-v1.1
Branch feat/platform-v7-f3-riskrobust-20260929 (pool-7, base c8503bb3, 1 commit). Nothing built or run (hard rule): expect one /W4 /WX pass.
Test expectations were checked in numpy on the same LCG draws (scratch only, synthetic).

**Rules** (none uses returns; config in strategy_risk_model.hpp RiskModelConfig)
1. `standardize_style`: fence at median +- k 1.4826 MAD of the eligible names' values, then (USE4) cap-weighted mean 0 and equal-weighted SD 1 of the fenced values, clip 3, missing 0.
   k = 3.5 = the Iglewicz-Hoaglin (1993) modified-z cutoff. 1.4826 = 1/Phi^-1(.75) (Hampel 1974; Rousseeuw-Croux 1993). MAD 0 (over half tied) -> style 0 that date, counted as `unscaled_descriptor_dates`.
   Stated bound for one name moved anywhere: |dz_other| <= omega(k+u)s/sd + 3(k+u)^2 s^2/(2(n-1)sd^2) + .01 (omega = cap share, u = its clean robust z). v1 moved them by O(1).
2. Style validity (`regress_cross_section`, both regressions): a style enters only if its effective names (sum w z^2)^2 / sum (w z^2)^2 >= 10. This is the Herfindahl numbers-equivalent (Adelman 1969); 10 = min_industry_names, the model's thin-factor floor. Healthy columns: ~22 at 100 rows, ~350 at 1,800.
   A one-name dummy scores 1. Its factor return is missing that session, and F2's structural forecast covers the factor.
3. `structural_specific_vol`: every style exposure is clamped to the fitted names' [min, max]. sigma_STR is bounded to the fitted names' type-7 (Hyndman-Fan 1996) [p1, p99] sigma_TS (1%/99% = the usual winsorisation). Without a fit, the fallback is their cap-weighted median, with the same bounds.
   `shrink_to_size_deciles`: the target is now the lower cap-weighted decile median, not the cap-weighted mean. A name with under 50% of its decile's cap moves it by at most one rank. The engine intensity formula is kept (q .1).
4. `check_specific_variance`: every finite daily D must lie in (0, 1.0), else OutOfRange: "daily specific variance V of instrument ID at session NS (YYYY-MM-DD) is outside (0, 1): refused". Nothing is clamped.
   run_risk_model checks each session before any sink sees it, so the verb exits 1 and writes no manifest.
5. Manifest: `model` "atx-risk-v1.1" (schema stays atx.risk-model/v1, so spo RiskStore reads it unchanged), recipe.model plus a recipe text and sources for every rule.
   The `robustness` block is repeated in bias_summary.json as `risk_model_robustness`. It holds: max_daily_specific_variance (value, session, instrument); min_style_dispersion and min_style_effective_names over regressed styles (value, style, session); style_dates_dropped (total and per style); structural_names_at_sigma_bound / _with_clamped_exposure (name-sessions); fenced_descriptor_values; unscaled_descriptor_dates; invariant_refusals 0.
   diagnostics.csv appends 10 per-session columns. stdout prints `risk robustness (atx-risk-v1.1): ...`. The bias harness is unchanged.

**Files**: atx-impl/src/strategy_risk_model.{hpp,cpp}, strategy_risk_verb.cpp, tools/equity_strategy_risk.cpp (comment), tests/strategy_risk_model_test.cpp.

**Tests** (gtest, synthetic, not built): RiskRobust.OneExtremeOutlierMovesOtherStyleZOnlyWithinTheStatedBound (n 1,000, median-cap and largest-cap name at +-1e12: worst .020/.069 vs bound .032/.086; v1 gave ~3), .StandardizeStyleBoundariesAndNamesOutsideTheUniverse, .DegenerateStyleIsDroppedEverySessionAndCountedInTheManifest (verb: value on 4 of 200 names -> 299/299 sessions dropped; random family ok, dropped 0; manifest == bias_summary block), .StructuralSpecificVolIsClampedToTheFitAndBoundedToItsSigmaQuantiles (exp(40 x 3) ~ 1e50 -> p99), .SizeDecileTargetIsRobustToOneCorruptName, .SpecificVarianceOutsideTheBoundIsRefusedNamingInstrumentAndDate, .TheDriverRefusesTheRunAtTheFirstOutOfRangeSession; RiskWls.DegenerateStyleColumnIsNotRegressedAndReported; RiskModel.AssetGrowthOutlierOnAShortHistoryNameStaysBounded (the I-2 chain end to end: late-listed name with asset growth 1e9 -> z 3, others' SD ~.97, D < 5e-3).
No existing test was edited. Their tolerances hold: normal descriptors are not fenced at 3.5, planted columns have >= 30 effective names, and planted D ~1e-4 is inside the bound.

**Root commands** (pool-2 after merge)
1. `powershell -File build-equity\mega-build.ps1 -Tag v7-f3 -Targets atx-impl-strategy-target-tests,atx-equity-strategy-risk`
2. `build-equity\bin\atx-impl-strategy-target-tests.exe --gtest_filter=Risk*:Spo*`
3. `python scripts/run_bounded_research.py --output build-equity/v7-f3-risk-run --seconds 180 --max-rss-mib 1536 --min-free-mib 512 -- build-equity\bin\atx-equity-strategy-risk.exe risk --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7/manifest.json --fields-sha256 1d1fa87a00d519bcf08fbec83f3fd17029e23650a98a9af26e99ddb1f1a73ee1 --output build-equity/v7-f3-risk --emit-exposures all`
   Accept on: exit 0 (0 refusals); robustness.max_daily_specific_variance < 1 (expect <= ~1e-2); `risk bias factor` b in [.9, 1.1]; random family ok; wall <= 40 s; RSS <= 600 MiB (F2 took 21.9 s / 455 MiB; F3 adds O(n) per session).

**What changes vs the F2 numbers (pin 17f9328f)**
- asset_growth from 2020-05-12 (259 dates) and profitability (19): the one-name dummy is gone. Other names keep z SD ~1, so those two factor-return series and their F rows become real factor tests.
- Every style: v1's 5-SD fence was set with the tails inside the SD. For heavy-tailed descriptors (value, E/P, asset growth, leverage, SI), body z rise and factor returns shrink a little; near-normal ones (size, beta, momentum, liquidity) barely move. So every style b, the VRA lambda^2 and W1/spo inputs move a little.
- Specific: the 176-179-name spike (D up to 3.9e12) disappears. The median target sits below the cap-weighted mean of a right-skewed sigma, so shrunk D falls slightly and random-family b may rise a few %. The 20-session window no longer understates random risk.
- Styles dropped: expected ~0 once the standardisation is robust; any drop is counted per style.

**Untested**: compile, every gtest, all real-data numbers (bounds hit, drops, max D, b, wall, RSS). Whether any lo1 descriptor has a zero MAD on some date (reported as unscaled_descriptor_dates, not refused). Deliberately out of scope: dispersion in the shrinkage intensity is still the RMS around the target. A corrupt name therefore lowers its decile-mates' shrinkage (they stay between their own sigma and the target). spo's specific ceiling (R2 M-1) is left alone.
