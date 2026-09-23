# Fundamental / quality alpha zoo on real 2013-2018 — lane 10 (2026-09-23)

**Verdict: NO CANDIDATE.** None of the 61 pre-registered fundamental expressions (122 trials: 61 x h in {5, 21};
the two universe cuts are restrictions of the same trials) clears even a naive |t| > 2 on the pooled non-overlapping
h = 21 rank IC, let alone a multiple-testing bar (Bonferroni over 122 trials at 5% two-sided needs |t| ~ 3.5; the
Harvey-Liu-Zhu t > 3 hurdle likewise). Nothing was tuned after seeing results: no expression, horizon, lag,
staleness cap or universe was changed. 2019 was never loaded; every evaluated session is < 2019-01-01 (asserted in
code).

What *is* informative — the signs and ordering match the published 2013-2018 record:

* **Gross profitability (Novy-Marx 2013)** is the strongest family on both cuts: pooled rank IC +0.017 (top-1000,
  t 1.67, 4/6 years positive) and +0.023 (top-3000, t 1.82, 4/5), 0.03-0.10 correlation to 12-1 momentum, and very
  slow (1 - rho_rank = 0.0005 per session). Gross margin (`qual_gmar`) is similar.
* **Net share issuance (Pontiff-Woodgate 2008)** is next: +0.018 / +0.015, t 1.39 / 1.16, 4/6 and 4/5 years
  positive, *negative* correlation to momentum (-0.08), so it diversifies.
* **SUE / PEAD (quarterly net income, seasonal random walk)** +0.013 / +0.009, t 0.75 / 1.47, correlation to
  momentum +0.23 (expected: earnings news and price momentum overlap).
* **Value was the worst family**: B/M pooled rank IC -0.024 (t -2.06, positive in 1/6 years; +0.086 in 2016, the one
  value-rally year). This is the documented 2010s value drawdown, not a bug: the 2016 sign flip lines up with
  momentum's -0.11 and GP/A's -0.07 in the same year. Low leverage and QMJ safety are also negative (junk rallied),
  which drags every QMJ composite below zero.
* Equal-weight composites (`comp_all`, QMJ, FF5, mispricing) do **not** beat their best member: they average a
  negative-IC value leg into positive-IC profitability/issuance legs, and their rank-of-sum needs every leg to be
  finite, cutting coverage to 13-29% of admitted names.

Recommended input to the lane-5 combiner / lane-9 miner (as *features*, not accepted alphas): `qual_gpa`, `inv_iss`,
`pead_sue`, plus `qual_gmar` and `acc_sloan` — positive in >= 4/6 years, low correlation with momentum (mutual correlation not
yet measured). They should enter a multiple-testing-controlled combination; this evidence does not support trading any of
them alone.

## Data and point-in-time construction

| Item | Value |
|---|---|
| Fundamentals source | SEC Company Facts bulk `C:\atx\atx-db\data\cache\companyfacts.zip` (20,390 CIKs; sha256 in the export manifest) |
| Id bridge | warehouse `security_identifier_history` (`TBLTICKERHISTORY_SECURITY_ID` -> `SEC-CIK-*`), read-only from `warehouse.duckdb.pre-migrate.20260922-235808.bak` (the live warehouse is write-locked by a running Company Facts activation; its `fundamental_points` is partial, 74/1254 top-1000 names) |
| Panel securities (all 2013-2019 contexts, both cuts) | 5,461 SpiderRock ids; 2,485 bridge to a CIK; 2,004 have >= 1 snapshot; 2,976 have no SEC-CIK bridge (ETFs, delisted/renamed names under TBLTICKERHISTORY-namespace ids); 53 CIKs absent from Company Facts |
| Snapshots | 61,291 filing rows, `C:\atx\data\equity_fund_fields_l10_20260923\points.csv` (sha256 prefix 3f4105407e87842d) + `manifest.json`, `availability_audit.json`, `unmapped_securities.csv` |
| Clock | `available_date` = filed + 1 day (warehouse policy `sec_filed_date_plus_46h_v1`), then **+1 session** lag in the aligner; facts filed on or after 2020-01-01 are never exported |
| Selection | per field, the visible record with the latest fiscal period (ties -> later filing); forward fill capped at 400 days from filing and 550 days from period end |
| Filing lag (filed - period end, days) | 10-Q median 36 (p95 42); 10-K median 57 (p95 76); 20-F median 100 |
| Staleness | median age of the book-equity value in use: 85-91 days since period end |
| TTM | a direct 12-month fact, else four chained discrete quarters (YTD differences derive missing quarters) |
| Shares | weighted-average diluted shares, year-ago value **as restated** in the latest filing (split-consistent). Cover-page dei counts are not restated and booked AAPL's 2014 7:1 split as +560% issuance; this was caught on the first export and fixed before any evaluation ran |
| SUE | quarterly net income, (NI_q - NI_q-4) / sd(previous 8 seasonal differences), >= 4 required (net income instead of EPS because it is split-invariant) |

Coverage of admitted cells in each evaluation year (`alignment.json`, top-1000): book equity 47-59%, net income TTM
48-60%, gross profit TTM 29-39% (many issuers, notably financials, tag neither GrossProfit nor cost of revenue), SUE
41-53%. Top-3000 is about 10 points lower. The ceiling is the id bridge (~57% of top-1000 names map to a CIK), not the
filings. **Every IC below is therefore measured on the covered sub-universe**, which leans toward continuously
listed domestic filers.

## Evaluation protocol

* Contexts: `equity_scorecard16_ctx_<year>_t1000|t3000_20260920`, years 2013-2018 (there is no t3000 2017 context).
  Each context's warm-up year feeds the time-series operators; only sessions in its final calendar year are scored.
* Engine: `atx::engine::eval::compute_cross_section_ic` (the frozen unit behind `equity-ic`), `DropMissingForward`,
  `AverageRanksV1`, 10 quantiles, >= 30 names per date, 1,000 circular-block bootstrap draws, 10 bps trade cost,
  365 bps annual borrow. Admission mask = the context universe mask. The `equity-ic` stage hard-codes its frozen
  family list and design-note hash, so the zoo runs through the same engine unit via the opt-in
  `FundamentalZoo.RealDataIcReport` harness (`atx-impl/tests/fundamental_zoo_test.cpp`) instead of editing the stage.
* Pooled statistics: the per-date h = 21 rank IC is concatenated over years; ICIR = mean / sd of that series; the t
  statistic uses every 21st emitted date (non-overlapping), the honest one under overlapping horizons.
* Reference: 12-1 momentum `rank(delay(close,21)/delay(close,252)-1)` evaluated identically (not a trial). Its
  pooled top-1000 rank IC of +0.028 matches checkpoint 16's +0.030 over the same years, which cross-checks the
  harness.
* Outputs: `C:\atx\data\equity_fund_zoo_ic_l10_20260923\` — `ic_by_year.csv` (per context x signal, with bootstrap
  CI, gross spread and decile turnover), `zoo_pooled.csv`, `alignment.json`, `manifest.json`. Runtime 30 min on the
  debug build.

## Full pooled table (sorted by top-1000 t)

`1-rho` = implied one-way turnover per session from the lag-1 rank autocorrelation (fundamentals change quarterly,
so it is tiny; decile turnover at h = 21 is in `ic_by_year.csv`). Coverage = names with a finite signal / admitted
names.

| signal | T1k rank IC h21 | T1k ICIR h21 | T1k t (non-overlap) | T1k yrs>0 | T1k rank IC h5 | T3k rank IC h21 | T3k t | T3k yrs>0 | 1-rho (T1k) | corr to 12-1 mom (T1k) | signal coverage (T1k) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `qual_gmar` | +0.0154 | +0.125 | +1.81 | 4/6 | +0.0096 | +0.0200 | +1.33 | 3/5 | 0.0005 | +0.06 | 0.30 |
| `qual_gpa` | +0.0165 | +0.130 | +1.67 | 4/6 | +0.0113 | +0.0232 | +1.82 | 4/5 | 0.0005 | +0.03 | 0.31 |
| `qual_gpa_sn` | +0.0148 | +0.117 | +1.60 | 4/6 | +0.0107 | +0.0217 | +1.68 | 4/5 | 0.0045 | +0.03 | 0.31 |
| `inv_iss_sn` | +0.0185 | +0.175 | +1.41 | 4/6 | +0.0064 | +0.0160 | +1.18 | 4/5 | 0.0045 | -0.07 | 0.48 |
| `inv_iss` | +0.0181 | +0.170 | +1.39 | 4/6 | +0.0066 | +0.0154 | +1.16 | 4/5 | 0.0017 | -0.08 | 0.48 |
| `acc_sloan` | +0.0103 | +0.129 | +1.01 | 4/6 | +0.0071 | +0.0019 | +0.20 | 4/5 | 0.0022 | +0.05 | 0.51 |
| `acc_sloan_sn` | +0.0095 | +0.119 | +0.92 | 4/6 | +0.0065 | +0.0030 | +0.34 | 4/5 | 0.0050 | +0.05 | 0.51 |
| `qual_qmj_prof` | +0.0053 | +0.044 | +0.87 | 4/6 | +0.0067 | +0.0047 | +0.35 | 3/5 | 0.0008 | +0.05 | 0.28 |
| `inv_buyback` | +0.0122 | +0.140 | +0.85 | 4/6 | +0.0044 | +0.0149 | +1.17 | 4/5 | 0.0022 | -0.05 | 0.48 |
| `comp_mispricing` | +0.0056 | +0.046 | +0.77 | 3/6 | +0.0029 | +0.0112 | +0.87 | 2/5 | 0.0015 | -0.05 | 0.29 |
| `pead_sue_w` | +0.0129 | +0.171 | +0.75 | 4/6 | +0.0125 | +0.0085 | +1.47 | 3/5 | 0.0101 | +0.23 | 0.44 |
| `pead_sue` | +0.0129 | +0.171 | +0.75 | 4/6 | +0.0125 | +0.0085 | +1.47 | 3/5 | 0.0101 | +0.23 | 0.44 |
| `acc_pct` | +0.0041 | +0.054 | +0.68 | 4/6 | +0.0025 | +0.0087 | +0.78 | 4/5 | 0.0023 | +0.00 | 0.51 |
| `comp_mispricing_sn` | +0.0050 | +0.042 | +0.67 | 3/6 | +0.0024 | +0.0111 | +0.86 | 2/5 | 0.0049 | -0.05 | 0.29 |
| `ref_momentum_12_1` | +0.0280 | +0.141 | +0.65 | 4/6 | +0.0257 | +0.0190 | +0.58 | 3/5 | 0.0056 | +1.00 | 0.69 |
| `pead_sue_sn` | +0.0117 | +0.159 | +0.63 | 4/6 | +0.0118 | +0.0076 | +1.33 | 3/5 | 0.0125 | +0.22 | 0.44 |
| `acc_ts` | +0.0152 | +0.184 | +0.61 | 3/6 | -0.0035 | +0.0089 | +1.45 | 2/5 | 0.0070 | -0.01 | 0.23 |
| `inv_comp` | +0.0068 | +0.069 | +0.53 | 3/6 | -0.0004 | +0.0089 | +0.70 | 4/5 | 0.0021 | -0.11 | 0.48 |
| `qual_cfoa` | +0.0055 | +0.062 | +0.33 | 4/6 | +0.0087 | +0.0022 | +0.26 | 3/5 | 0.0008 | +0.05 | 0.51 |
| `qual_cbop` | +0.0066 | +0.076 | +0.33 | 4/6 | +0.0081 | +0.0053 | +0.16 | 4/5 | 0.0009 | +0.04 | 0.40 |
| `val_bp_ts` | +0.0018 | +0.011 | +0.15 | 2/6 | +0.0165 | +0.0067 | +0.71 | 2/5 | 0.0208 | -0.46 | 0.21 |
| `comp_all` | +0.0010 | +0.008 | +0.04 | 3/6 | +0.0023 | +0.0066 | +0.43 | 3/5 | 0.0034 | -0.11 | 0.24 |
| `qual_roe` | +0.0013 | +0.015 | +0.03 | 3/6 | +0.0073 | +0.0015 | +0.17 | 3/5 | 0.0009 | +0.04 | 0.47 |
| `pead_dey` | -0.0069 | -0.077 | +0.03 | 2/6 | +0.0025 | +0.0037 | +0.28 | 2/5 | 0.0257 | +0.00 | 0.43 |
| `qual_roe_sn` | +0.0011 | +0.013 | -0.03 | 3/6 | +0.0068 | +0.0012 | +0.19 | 3/5 | 0.0037 | +0.04 | 0.47 |
| `comp_all_sn` | +0.0007 | +0.006 | -0.06 | 3/6 | +0.0017 | +0.0065 | +0.39 | 3/5 | 0.0067 | -0.11 | 0.24 |
| `pead_sue_fresh` | +0.0065 | +0.079 | -0.07 | 3/6 | +0.0105 | +0.0131 | +1.35 | 4/5 | 0.0098 | +0.23 | 0.32 |
| `pead_de_p` | -0.0044 | -0.056 | -0.09 | 3/6 | +0.0065 | +0.0007 | -0.05 | 3/5 | 0.0180 | +0.20 | 0.43 |
| `comp_ep_sue` | +0.0098 | +0.117 | -0.09 | 5/6 | +0.0114 | +0.0103 | +0.77 | 4/5 | 0.0090 | +0.06 | 0.44 |
| `qual_op` | -0.0009 | -0.008 | -0.14 | 3/6 | +0.0058 | +0.0022 | -0.02 | 3/5 | 0.0006 | +0.02 | 0.37 |
| `qual_op_sn` | -0.0009 | -0.009 | -0.18 | 3/6 | +0.0055 | +0.0026 | +0.07 | 3/5 | 0.0039 | +0.02 | 0.37 |
| `comp_gp_bp` | -0.0011 | -0.010 | -0.37 | 3/6 | -0.0033 | +0.0110 | +0.98 | 3/5 | 0.0019 | -0.22 | 0.29 |
| `comp_gp_bp_sn` | -0.0009 | -0.008 | -0.38 | 3/6 | -0.0029 | +0.0094 | +0.84 | 3/5 | 0.0056 | -0.22 | 0.29 |
| `val_sp` | +0.0014 | +0.010 | -0.48 | 3/6 | -0.0009 | +0.0087 | +0.37 | 3/5 | 0.0010 | -0.17 | 0.45 |
| `val_sp_sn` | +0.0004 | +0.003 | -0.53 | 3/6 | -0.0021 | +0.0082 | +0.32 | 3/5 | 0.0039 | -0.17 | 0.45 |
| `qual_roa` | -0.0034 | -0.035 | -0.55 | 3/6 | +0.0046 | -0.0036 | -0.21 | 2/5 | 0.0008 | +0.03 | 0.51 |
| `comp_ff5_sn` | -0.0128 | -0.094 | -0.70 | 2/6 | -0.0060 | -0.0047 | -0.26 | 2/5 | 0.0060 | -0.22 | 0.37 |
| `val_cfp` | -0.0021 | -0.016 | -0.72 | 3/6 | -0.0009 | +0.0101 | +0.28 | 3/5 | 0.0017 | -0.20 | 0.51 |
| `val_cfp_sn` | -0.0028 | -0.021 | -0.75 | 3/6 | -0.0021 | +0.0097 | +0.23 | 3/5 | 0.0045 | -0.21 | 0.51 |
| `comp_ff5` | -0.0136 | -0.099 | -0.76 | 2/6 | -0.0063 | -0.0046 | -0.23 | 2/5 | 0.0028 | -0.22 | 0.37 |
| `comp_q` | -0.0079 | -0.085 | -0.77 | 2/6 | -0.0022 | -0.0012 | +0.13 | 1/5 | 0.0023 | -0.05 | 0.47 |
| `val_ep` | -0.0035 | -0.028 | -0.85 | 3/6 | +0.0013 | +0.0059 | +0.02 | 4/5 | 0.0024 | -0.16 | 0.51 |
| `val_ep_sn` | -0.0036 | -0.029 | -0.88 | 3/6 | +0.0007 | +0.0044 | -0.09 | 4/5 | 0.0051 | -0.16 | 0.51 |
| `inv_ag_ss` | -0.0110 | -0.121 | -0.91 | 2/6 | -0.0094 | -0.0001 | +0.34 | 3/5 | 0.0062 | -0.12 | 0.51 |
| `val_ep_grank` | -0.0036 | -0.029 | -0.91 | 3/6 | +0.0006 | +0.0044 | -0.11 | 3/5 | 0.0087 | -0.16 | 0.51 |
| `comp_magic` | -0.0040 | -0.037 | -0.92 | 3/6 | +0.0033 | -0.0009 | -0.33 | 2/5 | 0.0020 | -0.09 | 0.51 |
| `inv_ag_sn` | -0.0116 | -0.126 | -0.93 | 2/6 | -0.0098 | +0.0002 | +0.35 | 3/5 | 0.0060 | -0.12 | 0.51 |
| `val_comp_sn` | -0.0072 | -0.046 | -1.01 | 4/6 | -0.0063 | +0.0015 | -0.31 | 3/5 | 0.0059 | -0.26 | 0.42 |
| `inv_ag` | -0.0120 | -0.130 | -1.01 | 2/6 | -0.0105 | -0.0004 | +0.25 | 3/5 | 0.0032 | -0.13 | 0.51 |
| `val_comp` | -0.0077 | -0.049 | -1.03 | 4/6 | -0.0062 | +0.0018 | -0.28 | 3/5 | 0.0032 | -0.26 | 0.42 |
| `qual_qmj` | -0.0176 | -0.112 | -1.07 | 2/6 | -0.0120 | -0.0054 | -0.66 | 2/5 | 0.0012 | +0.02 | 0.13 |
| `qual_droe` | -0.0123 | -0.188 | -1.09 | 2/6 | -0.0005 | -0.0088 | -1.12 | 2/5 | 0.0045 | +0.14 | 0.37 |
| `qual_qmj_sn` | -0.0168 | -0.107 | -1.10 | 2/6 | -0.0123 | -0.0054 | -0.66 | 2/5 | 0.0060 | +0.03 | 0.13 |
| `qual_droa` | -0.0127 | -0.197 | -1.27 | 1/6 | -0.0010 | -0.0100 | -1.65 | 2/5 | 0.0047 | +0.16 | 0.40 |
| `qual_qmj_safe` | -0.0368 | -0.237 | -1.39 | 1/6 | -0.0186 | -0.0320 | -1.84 | 2/5 | 0.0007 | +0.00 | 0.22 |
| `val_bp_sn` | -0.0235 | -0.177 | -2.03 | 1/6 | -0.0171 | -0.0082 | -0.95 | 2/5 | 0.0036 | -0.27 | 0.48 |
| `val_bp` | -0.0240 | -0.181 | -2.06 | 1/6 | -0.0177 | -0.0078 | -0.88 | 2/5 | 0.0010 | -0.27 | 0.48 |
| `val_logbp` | -0.0240 | -0.181 | -2.06 | 1/6 | -0.0177 | -0.0078 | -0.88 | 2/5 | 0.0010 | -0.27 | 0.48 |
| `val_bp_ss` | -0.0241 | -0.183 | -2.07 | 1/6 | -0.0174 | -0.0096 | -1.03 | 2/5 | 0.0039 | -0.27 | 0.48 |
| `qual_fscore` | -0.0187 | -0.249 | -2.42 | 2/6 | -0.0051 | -0.0006 | -0.26 | 2/5 | 0.0069 | +0.09 | 0.22 |
| `qual_lowlev` | -0.0290 | -0.311 | -2.62 | 0/6 | -0.0117 | -0.0233 | -1.62 | 1/5 | 0.0003 | -0.04 | 0.50 |

## Per-year detail, top-1000 (rank IC h = 21; bootstrap 95% CI where it excludes zero)

| year | GP/A | net issuance | SUE | B/M | low leverage | 12-1 momentum |
|---|---|---|---|---|---|---|
| 2013 | -0.007 | **+0.049** [+0.016, +0.084] | -0.008 | -0.035 | -0.043 | +0.097 |
| 2014 | +0.031 | +0.019 | +0.007 | -0.009 | -0.034 | +0.039 |
| 2015 | +0.039 | -0.007 | **+0.046** [+0.023, +0.069] | -0.047 | -0.036 | +0.127 |
| 2016 | **-0.071** | +0.030 | -0.025 | **+0.086** | -0.011 | -0.113 |
| 2017 | **+0.046** [+0.003, +0.088] | +0.027 | **+0.037** [+0.015, +0.059] | -0.052 | -0.010 | +0.065 |
| 2018 | **+0.075** [+0.039, +0.112] | -0.011 | +0.022 | -0.094 | -0.040 | -0.037 |

## Limitations (read before using any number)

1. Coverage is about 50% of top-1000 names, set by the SR-id -> CIK bridge. Ticker-based bridging was examined and
   rejected: 49 of 536 unbridged 2014 names match a *current* SEC ticker, many of them unrelated later issuers
   (e.g. BEAM, DYN, SI).
2. Company Facts values are as-filed XBRL facts. Restatements enter only on their own filing date, but the bulk
   file is a 2026 snapshot, so facts the SEC later *removed* cannot be reconstructed.
3. The definitions are TTM/annual approximations: FF operating profitability lacks interest expense; QMJ lacks its
   growth and payout blocks; Piotroski uses 7 of 9 signals; Stambaugh-Yuan uses 5 of 11 anomalies.
4. The admission mask is the raw context universe (price >= 1), not checkpoint 21's liquidity floor.
5. There are only six (five) yearly observations, so sign-stability counts are weak evidence either way.

## Reproduce

```powershell
python atx-engine\tools\export_fundamental_fields.py --companyfacts C:\atx\atx-db\data\cache\companyfacts.zip `
  --warehouse C:\atx\atx-db\data\warehouse.duckdb.pre-migrate.20260922-235808.bak `
  --contexts "C:/atx/data/equity_scorecard16_ctx_*_t*_20260920" --out <fresh dir>
$env:ATX_L10_FUND_POINTS = "<fresh dir>\points.csv"
$env:ATX_L10_FUNDZOO_OUT = "<fresh output dir>"
$env:ATX_L10_CONTEXTS    = "<semicolon-joined equity_scorecard16_ctx_<2013..2018>_<cut>_20260920 dirs>"
build-equity\bin\atx-impl-tests.exe --gtest_filter=FundamentalZoo.RealDataIcReport
```
