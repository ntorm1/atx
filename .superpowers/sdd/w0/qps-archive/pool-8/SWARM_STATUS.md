# Lane 7 (l7-riskmodel) status — Risk model: fundamental factors, hybrid APCA, validation, attribution

- Worktree: `C:/atx-wt/pool-8`  branch `feat/qps-l7-riskmodel`  base `334a7939`
- Builder: REVIEW-FIX round DONE (2026-09-23), head 05e83cf4. Ready for integration. Not merged.

## Commits (lane)
```
05e83cf4 fix review findings (PIT caps, NaN book drop, lag=1, JSON escape, n_used>=K_f)
8292d108 real-data validation scorecard harness (APNL panel -> 5 configs)
9eaadc3c opt-in factor-cov estimator via combine::CovTarget; verify WIP GEMM solve_date
2873464e WIP (vectorized per-date WLS + Gram eigen reuse) -- VERIFIED green
47aacbf7 production-shape build bench (3000x14x60x504)
b7ad0472 fundamental factors, hybrid APCA model, validation scorecard, attribution
```

## Review findings (round 1) — all 7 real, all fixed in 05e83cf4
1. MAJOR look-ahead: the converter used each name's LAST cap/sector -> Size + sqrt(cap) weights
   carried the future. FIX: ExposureSeries::cap_series + build_panel_series_pit_caps; converter
   emits cap_tn/sector_tn (T x N); bench uses per-row caps + first-observed in-universe sector.
2. NaN return at a-1 -> names now dropped from every book + renormalized; mean_excluded reported.
3. FundamentalCfg::availability_lag default 0 -> 1 (test DefaultLagHidesSameDayRelease).
4. attribution.hpp comment corrected (identity is algebraic); new real check
   MimickingBooksHaveZeroSpecificUnderWlsFactorReturns.
5. build() now errs when n_used < K_f (doc updated).
6. json_quote() escapes label / panel_dir.
7. DeterministicAndPitSafe scrambles style + cap rows < as_of too (+ negative controls).

## LOOK-AHEAD CONTAMINATED (do not use)
C:/atx/data/l7_riskmodel_scorecard_2014_t1000_20260923/l7_scorecards.json and its raw dir
(l7_riskmodel_raw_2014_t1000_20260923): static LAST cap/sector. Superseded by the PIT rerun below.

## Tests
- After 05e83cf4 (Debug, equity-dev): RiskFundamentalFactors/RiskHybridModel/RiskModelValidation/
  RiskAttribution/RiskFactorCovTarget/RiskExposures/RiskFactorModel/RiskStatFactor 72/72 PASS.
- Pre-fix full risk ctest: 322/322 PASS (1856 s).
- Post-fix: atx-build.ps1 -Ctest -Preset equity-dev -R '^(Risk|Robust|Kelly|Capacity|GpTurnover|Optimizer)'
  --exclude-regex 'RiskQpAugment\.MatchesDenseOracleAcrossBattery' -> 327/327 PASS (37 s).

## Real-data PIT rerun: DONE (81.5 s, Release)
- raw: C:/atx/data/l7_riskmodel_raw_pit_2014_t1000_20260923 (T=509 N=1254, cap_ok=1.000, 8 sector ids)
- out: C:/atx/data/l7_riskmodel_scorecard_pit_2014_t1000_20260923/l7_scorecards.json (pit_caps:true, valid JSON)
- run: build atx-engine-bench -Preset equity-rel; ATX_L7_REAL_DIR/ATX_L7_OUT_DIR;
  build-equity-rel/bin/atx-engine-bench.exe --benchmark_filter=BM_L7RealScorecard
```
label                              K    EWb    MVb  randb randIn  randQ   optb  optIn assetb  MRAD excl
fundamental_price_styles        11.0  1.090  0.923  0.978   0.48  2.582  1.043  0.85  1.064  0.195  0.3
hybrid_baing                    18.9  1.090  1.023  0.987   0.55  2.573  1.033  0.90  1.065  0.196  0.3
hybrid_mp                       19.0  1.090  1.024  0.987   0.56  2.573  1.034  0.90  1.065  0.196  0.3
hybrid_baing_ewma               18.9  1.099  1.029  1.076   0.58  2.566  1.095  0.80  1.098  0.180  0.3
hybrid_baing_lw2020_spec_ewma   18.9  1.088  0.998  0.993   0.54  2.573  1.096  0.80  1.097  0.183  0.3
```
Conclusion (PIT only): the APCA block still helps. Random books in band go from 0.48 to 0.55
and optimized books from 0.85 to 0.90, and the MinVar bias moves to about 1. hybrid_baing
remains the default. EWMA gives the best Q/MRAD, but the optimized books are biased (1.10).
The leak inflated the old numbers only slightly: the direction and ranking are unchanged.

## Next
- Deferred: fundamentals on real data (no PIT FundamentalPanel source in context panels);
  real-data attribution run (needs Lane 8 book output).
