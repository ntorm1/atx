# risk_inputs

Offline implementation checkpoint; no real coverage acceptance claimed.
Schema `atx.alpha-panel.risk-inputs/v1`; keys `(session_date,security_id)`.
Immutable per-session Parquet units under one manifest-selected generation.
Code/source/config hashes govern resume; publisher locking and publish-last pointer
prevent incomplete builds replacing an earlier generation. Memory is bounded by
one <=20,000-line cross-section and one <=252-observation line history at a time.

| Columns | Type/unit | Definition/source |
|---|---|---|
| session_date/security_id/cik | DATE/BIGINT/BIGINT | Dated universe grid and issuer link |
| available_at | UTC TIMESTAMP | Session 22 UTC; consume with strict as-of cutoff |
| univ_all_common/univ_r3000p | BOOLEAN | Exact final dated universe denominator |
| beta_252 | nullable DOUBLE | Intercept CAPM OLS on <=252 aligned sessions, min200 complete |
| beta_ff5_MktRF/SMB/HML/RMW/CMA | nullable DOUBLE | Intercept FF5 OLS, min200 complete and full rank |
| ivol_capm_252/ivol_ff5_252 | nullable DOUBLE/decimal daily | Residual standard error with regression degrees of freedom |
| ff49 | nullable BIGINT | Strictly visible classification.ff49_no |
| size/value/momentum/volatility/liquidity/growth/leverage/quality | nullable DOUBLE/z score | Frozen formulas; sample normalization on certified all_common |
| adv_21/adv_63 | nullable DOUBLE/USD | Strictly visible native liquidity |
| spread_cs_21 | nullable DOUBLE/price fraction | Native Corwin-Schultz spread |
| si_to_io/si_shares/ftd_quantity/on_threshold_list | nullable DOUBLE/DOUBLE/DOUBLE/BOOLEAN | Explicit public borrow-proxy pack; no lending fee/utilization invention |
| cap_qualified/risk_missing_reason | BOOLEAN/nullable STRING | Independent economic-basis qualification and unavailable factor history |

Accounting/classification/liquidity/borrow use clocks strictly before the preceding
session 22 UTC. Accounting max age400 days, liquidity10, FTD60 and threshold10.
Daily exposure return windows contain no future observations; guarded vendor returns
are missing. Own factors are exact manifest/config-bound products, never benchmark
history substitutes. No post2022 risk regressions or return-derived momentum/volatility
are constructed under GC2; latest nonreturn primitives can publish. Missing factor
history stays missing. Coverage includes those missing cells on full universe/year
denominators, including every year 2020-2025, so latest completeness gates can FAIL.

CLI: `python -m atx_db.alpha_panel.risk_inputs --native-spec PATH --destination PATH
--start 2020-01-01 --end YYYY-MM-DD`, under memory guard <=1 GiB. Source/release
interface is the same as factors, plus exact own_factors `{manifest,sha256}`.
Each source is reverified before publish-last completion. No legacy prepared Boolean
certification publication API remains. CIK77159 quality exclusion shares the audit
dependency documented in factors.md. Stale/missing accounting, invalid ranks, missing
primitive inputs and insufficient histories remain nullable, never forward filled.

Registry integration must resolve exact manifest files rather than glob generations.
Dataset build rows/bytes/memory and coverage are unmeasured. Final v11/universe
snapshots, independent economic-basis audit and an exclusive guarded build are pending.
