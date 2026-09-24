# Lane 10 (l10-fundzoo) status — branch feat/qps-l10-fundzoo (pool-11) — REVIEW ROUND 1 FIXED (head 6b949d7f)

## Review round 1 (4 findings), all confirmed and addressed
1. [major] SR-id->CIK bridge is a current-ticker match (survivor look-ahead): CONFIRMED (atx-db ticker_history.py
   _apply_security_ids -> security_master.security_ids_for_symbols joins today_ticker to 2026 sec_company_tickers).
   No PIT ticker/CUSIP source in the warehouse backup (xbrl_filing_facts empty; sec_submissions has no tickers).
   Fix: disclosure + measurement. The harness now accepts ATX_L10_SURVIVOR_CONTEXTS and reports survivor/nonsurvivor
   splits, plus bridged/unbridged for the momentum ref. Measured bridge rate: 56-63% for survivors vs 1-3% for
   non-survivors on top-1000 (51-55% vs 5-11% on top-3000). The report verdict and limitations say the results are
   survivor-conditioned, the "ticker bridging rejected" wording is corrected, and the features must not go to lanes 5/9
   as clean.
2. [minor] share scale errors: CONFIRMED. Fix: the export drops a pair when |log10(shares/lag)| >= 2
   (129 knowledge states rejected). Export v2 is in C:\atx\data\equity_fund_fields_l10v2_20260923.
   inv_iss t1000 moves from +0.0181 (t 1.39) to +0.0180 (t 1.37).
3. [minor] 60 expressions / 120 trials: CONFIRMED; the report is fixed and the manifest now names the ref as not a trial.
4. [minor] |t|>2 wording: CONFIRMED; the six wrong-sign |t|>2 trials are listed, and an F-score construction/selection
   note is added.

## Commits (on top of 63e3c4a3)
- 75cbf945 fix(tools): share-pair plausibility + bridge caveat in the export manifest (tool v2)
- 6376d07b test(zoo): survivor / bridge split diagnostic in the real-data harness
- 6b949d7f docs(reviews): report revised

## Tests
- python -m unittest atx-engine/tests/tools/test_export_fundamental_fields.py : 9/9 OK
- ctest -Preset equity-dev -R '^(FundamentalFields|FundamentalZoo|SeedParse|FinraShort)\.' : 25/25 pass (RealDataIcReport skipped)
- Real data: build-equity-rel\bin\atx-impl-tests.exe --gtest_filter=FundamentalZoo.RealDataIcReport PASSED in 339 s
  -> C:\atx\data\equity_fund_zoo_ic_l10v2_20260923 (v1 dirs superseded)

## Notes
- C:\atx-cache\deps\spdlog-build is shared across presets and worktrees. A concurrent dev check and rel build broke a
  link (_ITERATOR_DEBUG_LEVEL mismatch). Build sequentially.
- build-equity-rel is configured in pool-11 (groups=data).

## Next / deferred
- Point-in-time SR-id -> CIK bridge (CIK/CUSIP or former tickers with validity dates); needed before any fundamental feature is admitted
- eps_revision, QMJ growth/payout, NOA / O-score; liquidity-floored mask; zoo correlation matrix
