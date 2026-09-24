# Fundamental / quality alpha zoo on real 2013-2018 — lane 10 (2026-09-23, revised after review)

**Verdict: NO CANDIDATE, and every number below is survivor-conditioned.**

* **Trials.** The fixture has **60** pre-registered expressions, so there are **120 declared trials** (60 x h in {5, 21}).
  The two universe cuts restrict the same trials. The 61st row in the outputs is the 12-1 momentum reference, which
  the harness adds and which is not a trial. The run manifest records `zoo_lines = 60`, `trials_declared = 120`.
* **Significance.** None of the 60 expressions clears |t| > 2 **in the literature-predicted direction** on the pooled
  non-overlapping h = 21 rank IC. None comes near a multiple-testing bar either: Bonferroni over 120 trials at 5%
  two-sided needs |t| of about 3.5, and the Harvey-Liu-Zhu hurdle is t > 3. Six trials do exceed |t| = 2 **with the
  wrong sign** (top-1000, h = 21). They are trials and count against the same bar:
  `qual_lowlev` t = -2.62, `qual_fscore` -2.42, `val_bp_ss` -2.07, `val_bp` -2.06, `val_logbp` -2.06 and
  `val_bp_sn` -2.03. The four B/M rows are one signal with four scalings (pairwise near-identical ranks), not four
  independent findings.
* **Survivorship (look-ahead selection).** The SpiderRock-id to CIK bridge is a current-ticker match. atx-db
  `ticker_history.py::_apply_security_ids` maps each id's `today_ticker` through
  `security_master.py::security_ids_for_symbols` against the 2026 SEC `company_tickers` file. So a name gets
  fundamentals only if its ticker is still SEC-listed today. Measured on the evaluation years, among top-1000 names
  still in the universe at the end of 2018, 56-63% are bridged. Among names that left by then, only **1-3%** are
  bridged (top-3000: 51-55% vs 5-11%). The evaluated fundamental cross-section is therefore almost entirely future
  survivors. That is look-ahead selection, not a mere coverage gap, and it hits the distress, quality, leverage and
  issuance families hardest. **The signs and sizes of `qual_gpa`, `inv_iss`, `qual_lowlev`, `qual_fscore`, `pead_sue`
  and value are not point-in-time clean.** See "Survivorship diagnostic" below.
* Nothing was tuned after seeing results. No expression, horizon, lag, staleness cap or universe was changed. The
  review fixes (share-scale filter, survivor split) change no signal definition. 2019 was never loaded; every
  evaluated session is before 2019-01-01, which the code asserts.

What the survivor-conditioned sample shows (descriptive, not evidence of alpha):

* **Gross profitability (Novy-Marx 2013)** is the strongest family on both cuts. Pooled rank IC is +0.017 on top-1000
  (t 1.67, 4/6 years positive) and +0.023 on top-3000 (t 1.82, 4/5). It has 0.03-0.10 correlation to 12-1 momentum
  and moves very slowly (1 - rho_rank = 0.0005 per session). Gross margin (`qual_gmar`) behaves about the same.
* **Net share issuance (Pontiff-Woodgate 2008)** comes next. Pooled rank IC is +0.018 / +0.016 (t 1.37 / 1.18), and
  its correlation to momentum is negative (-0.08). These are the numbers after the share-scale fix; before it they
  were +0.018 / +0.015 (t 1.39 / 1.16). The mis-scaled pairs were rare among admitted names.
* **SUE / PEAD** (quarterly net income, seasonal random walk) comes in at +0.013 / +0.009 (t 0.75 / 1.47), with
  correlation to momentum +0.23.
* **Value was negative** over 2013-2018. B/M pooled rank IC is -0.024 (t -2.06, positive in only 1 of 6 years, +0.086
  in 2016). This matches the documented 2010s value drawdown. Low leverage and QMJ safety are also negative, which
  pulls every QMJ composite below zero.
* **Negative Piotroski F-score (`qual_fscore`, t -2.42 on top-1000, -0.26 on top-3000).** The construction limits
  what this can mean. The score uses 7 of the 9 signals, and a year-ago `delta(..., 252)` on three derived fields
  plus the gross-profitability requirement leave a finite score on only 22% of admitted names. Those names are
  mostly non-financial survivors. Piotroski (2000) documents the effect inside high-B/M firms, where failures supply
  the short leg. The survivor bridge removes almost all failures (see below), so the short leg is missing. Among
  top-3000 non-survivors that are bridged (about 19 names per date, thin), F-score IC is +0.028. Treat the negative
  top-1000 sign as a construction and selection artifact, not as evidence against the anomaly.
* Equal-weight composites do **not** beat their best member. They average a negative-IC value leg into
  positive-IC profitability and issuance legs, and needing every leg finite cuts coverage to 13-29%.

**Use downstream.** Do not pass `qual_gpa`, `inv_iss` or `pead_sue` to lanes 5 or 9 as clean features. Before they
enter a combiner, replace the bridge with a point-in-time one: CIK or CUSIP, or former tickers with validity dates.
Until then any use must label them **survivor-conditioned**, and a result built on them cannot be admitted.

## Survivorship diagnostic

Survivors are ids in the universe on any of the final 21 sessions of the 2018 top-1000 or top-3000 context. This is
an ex-post split for diagnosis only and never a signal input. It is set with `ATX_L10_SURVIVOR_CONTEXTS` and written
to `zoo_pooled_splits.csv` and to `alignment.json` under `survivorship`.

| context (eval year) | names | survivors | bridge rate, survivors | bridge rate, non-survivors |
|---|---|---|---|---|
| 2013 top-1000 | 1,228 | 979 | 61.7% | 1.6% |
| 2014 top-1000 | 1,249 | 1,008 | 62.0% | 2.9% |
| 2015 top-1000 | 1,264 | 1,064 | 62.5% | 2.0% |
| 2016 top-1000 | 1,245 | 1,104 | 60.5% | 1.4% |
| 2017 top-1000 | 1,689 | 1,558 | 55.6% | 0.8% |
| 2013 top-3000 | 3,572 | 2,262 | 54.6% | 7.9% |
| 2014 top-3000 | 3,530 | 2,393 | 54.8% | 9.7% |
| 2015 top-3000 | 3,591 | 2,604 | 53.9% | 10.4% |
| 2016 top-3000 | 3,572 | 2,763 | 53.1% | 10.6% |

(2018 rows are survivors by construction and are omitted.)

What this shows:

1. On top-1000 at most 7 bridged non-survivors are admitted in any year. That is below the 30-names-per-date floor,
   so no fundamental IC is measurable for non-survivors, and the fundamental IC equals the survivor IC. For example
   `qual_gpa` is +0.0165 overall and +0.0161 on survivors. The bias cannot be estimated from this data. It can only
   be removed with a point-in-time bridge.
2. Where the population is large enough (top-3000, 32-64 bridged non-survivors used per date on average; the later years fall below the 30-name floor), several
   signs flip: `val_bp` -0.008 on survivors vs +0.027 on non-survivors, `qual_fscore` -0.005 vs +0.028,
   `qual_qmj_safe` -0.038 (t -2.13) vs +0.005, `pead_sue` +0.006 vs +0.019. All non-survivor t values are below 1.1
   in magnitude, so these are small-sample hints, not results.
3. The selection also changes a pure price signal. 12-1 momentum on top-1000 gives +0.034 on survivors vs +0.011 on
   non-survivors, and +0.020 on bridged vs +0.033 on unbridged names. So conditioning on the bridge moves the IC
   of a signal that uses no fundamentals.

## Data and point-in-time construction

| Item | Value |
|---|---|
| Fundamentals source | SEC Company Facts bulk `C:\atx\atx-db\data\cache\companyfacts.zip` (sha256 in the export manifest) |
| Id bridge | warehouse `security_identifier_history` (`TBLTICKERHISTORY_SECURITY_ID` -> `SEC-CIK-*`), read-only from `warehouse.duckdb.pre-migrate.20260922-235808.bak`. **Current-ticker match, survivor-conditioned** (above). The warehouse has no point-in-time alternative: `xbrl_filing_facts` (dei:TradingSymbol) is empty and `sec_submissions` has no ticker column |
| Panel securities (2013-2018 contexts, both cuts) | 5,187 SpiderRock ids; 2,352 bridge to a CIK; 1,908 have >= 1 snapshot; 2,835 have no SEC-CIK bridge; 53 CIKs are absent from Company Facts |
| Snapshots (export v2) | 59,995 filing rows in `C:\atx\data\equity_fund_fields_l10v2_20260923\points.csv` (sha256 c8c0dc573714d1e5...), plus `manifest.json`, `availability_audit.json` and `unmapped_securities.csv`. v1 (`..._l10_20260923`) is superseded: it has no share-scale filter, and its id list also covered the 2019 contexts |
| Clock | `available_date` = filed + 1 day (warehouse policy `sec_filed_date_plus_46h_v1`), then **+1 session** lag in the aligner. Facts filed on or after 2020-01-01 are never exported |
| Selection | per field, the visible record with the latest fiscal period (ties go to the later filing). Forward fill stops 400 days after filing or 550 days after period end, whichever comes first |
| Filing lag (filed - period end, days) | 10-Q median 36 (p95 42); 10-K median 57 (p95 76); 20-F median 100 |
| TTM | a direct 12-month fact, else four chained discrete quarters (YTD differences fill in missing quarters) |
| Shares | weighted-average diluted shares, with the year-ago value **as restated** in the latest filing (split-consistent). **Scale filter (review fix):** a (shares, shares_lag1y) pair whose ratio is 100x or more in either direction is dropped. This covers the XBRL thousands/millions errors near 10^+-3 and 10^+-6, e.g. 51,471 vs 51,175,000. It rejected 129 company-filing knowledge states, and no pair of 100x or more remains in points.csv. Pairs between 10x and 100x (92 rows in v1) are kept as possible mergers or reverse splits |
| SUE | quarterly net income, (NI_q - NI_q-4) / sd(previous 8 seasonal differences), >= 4 required |

Coverage of admitted top-1000 cells: book equity 47-59%, gross profit 29-39%, SUE 41-53%. Top-3000 runs about 10
points lower.

## Evaluation protocol

* Contexts: `equity_scorecard16_ctx_<year>_t1000|t3000_20260920`, years 2013-2018 (there is no top-3000 2017
  context). Only sessions in each context's final calendar year are scored.
* Engine: `atx::engine::eval::compute_cross_section_ic` with `DropMissingForward`, `AverageRanksV1`, 10 quantiles,
  >= 30 names per date, 1,000 bootstrap draws (200 for split rows, which report only means and t), 10 bps trade cost
  and 365 bps annual borrow. The admission mask is the context universe mask. The run goes through the opt-in
  `FundamentalZoo.RealDataIcReport` harness.
* Pooled statistics: per-date h = 21 rank IC concatenated over years. ICIR = mean / sd. The t statistic uses every
  21st emitted date (non-overlapping).
* Reference: 12-1 momentum, which is not a trial. Its pooled top-1000 IC of +0.028 matches checkpoint 16's +0.030.
* Outputs are in `C:\atx\data\equity_fund_zoo_ic_l10v2_20260923\`: `ic_by_year.csv`, `zoo_pooled.csv`,
  `zoo_pooled_splits.csv`, `alignment.json` and `manifest.json` (schema v2). The release build (`equity-rel`) ran in
  339 s. The v1 outputs (`..._l10_20260923`) are superseded.

## Full pooled table (export v2, sorted by top-1000 t)

| signal | T1k rank IC h21 | T1k ICIR h21 | T1k t (non-overlap) | T1k yrs>0 | T1k rank IC h5 | T3k rank IC h21 | T3k t | T3k yrs>0 | 1-rho (T1k) | corr to 12-1 mom (T1k) | signal coverage (T1k) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `qual_gmar` | +0.0154 | +0.125 | +1.81 | 4/6 | +0.0096 | +0.0200 | +1.33 | 3/5 | 0.0005 | +0.06 | 0.30 |
| `qual_gpa` | +0.0165 | +0.130 | +1.67 | 4/6 | +0.0113 | +0.0232 | +1.82 | 4/5 | 0.0005 | +0.03 | 0.31 |
| `qual_gpa_sn` | +0.0148 | +0.117 | +1.60 | 4/6 | +0.0107 | +0.0217 | +1.68 | 4/5 | 0.0045 | +0.03 | 0.31 |
| `inv_iss_sn` | +0.0185 | +0.175 | +1.39 | 4/6 | +0.0064 | +0.0162 | +1.19 | 4/5 | 0.0045 | -0.07 | 0.48 |
| `inv_iss` | +0.0180 | +0.170 | +1.37 | 4/6 | +0.0065 | +0.0156 | +1.18 | 4/5 | 0.0017 | -0.08 | 0.48 |
| `acc_sloan` | +0.0103 | +0.129 | +1.01 | 4/6 | +0.0071 | +0.0019 | +0.20 | 4/5 | 0.0022 | +0.05 | 0.51 |
| `acc_sloan_sn` | +0.0095 | +0.119 | +0.92 | 4/6 | +0.0065 | +0.0030 | +0.34 | 4/5 | 0.0050 | +0.05 | 0.51 |
| `qual_qmj_prof` | +0.0053 | +0.044 | +0.87 | 4/6 | +0.0067 | +0.0047 | +0.35 | 3/5 | 0.0008 | +0.05 | 0.28 |
| `inv_buyback` | +0.0122 | +0.139 | +0.84 | 4/6 | +0.0044 | +0.0149 | +1.17 | 4/5 | 0.0022 | -0.05 | 0.48 |
| `comp_mispricing` | +0.0057 | +0.047 | +0.77 | 3/6 | +0.0029 | +0.0113 | +0.88 | 2/5 | 0.0015 | -0.05 | 0.29 |
| `pead_sue_w` | +0.0129 | +0.171 | +0.75 | 4/6 | +0.0125 | +0.0085 | +1.47 | 3/5 | 0.0101 | +0.23 | 0.44 |
| `pead_sue` | +0.0129 | +0.171 | +0.75 | 4/6 | +0.0125 | +0.0085 | +1.47 | 3/5 | 0.0101 | +0.23 | 0.44 |
| `comp_mispricing_sn` | +0.0051 | +0.043 | +0.68 | 3/6 | +0.0023 | +0.0112 | +0.87 | 2/5 | 0.0049 | -0.05 | 0.29 |
| `acc_pct` | +0.0041 | +0.054 | +0.68 | 4/6 | +0.0025 | +0.0087 | +0.78 | 4/5 | 0.0023 | +0.00 | 0.51 |
| `ref_momentum_12_1` (reference, not a trial) | +0.0280 | +0.141 | +0.65 | 4/6 | +0.0257 | +0.0190 | +0.58 | 3/5 | 0.0056 | +1.00 | 0.69 |
| `pead_sue_sn` | +0.0117 | +0.159 | +0.63 | 4/6 | +0.0118 | +0.0076 | +1.33 | 3/5 | 0.0125 | +0.22 | 0.44 |
| `acc_ts` | +0.0152 | +0.184 | +0.61 | 3/6 | -0.0035 | +0.0089 | +1.45 | 2/5 | 0.0070 | -0.01 | 0.23 |
| `inv_comp` | +0.0069 | +0.070 | +0.52 | 3/6 | -0.0004 | +0.0091 | +0.72 | 4/5 | 0.0021 | -0.11 | 0.48 |
| `qual_cfoa` | +0.0055 | +0.062 | +0.33 | 4/6 | +0.0087 | +0.0022 | +0.26 | 3/5 | 0.0008 | +0.05 | 0.51 |
| `qual_cbop` | +0.0066 | +0.076 | +0.33 | 4/6 | +0.0081 | +0.0053 | +0.16 | 4/5 | 0.0009 | +0.04 | 0.40 |
| `val_bp_ts` | +0.0018 | +0.011 | +0.15 | 2/6 | +0.0165 | +0.0067 | +0.71 | 2/5 | 0.0208 | -0.46 | 0.21 |
| `comp_all` | +0.0011 | +0.009 | +0.05 | 3/6 | +0.0023 | +0.0068 | +0.44 | 3/5 | 0.0034 | -0.10 | 0.24 |
| `qual_roe` | +0.0013 | +0.015 | +0.03 | 3/6 | +0.0073 | +0.0015 | +0.17 | 3/5 | 0.0009 | +0.04 | 0.47 |
| `pead_dey` | -0.0069 | -0.077 | +0.03 | 2/6 | +0.0025 | +0.0037 | +0.28 | 2/5 | 0.0257 | +0.00 | 0.43 |
| `qual_roe_sn` | +0.0011 | +0.013 | -0.03 | 3/6 | +0.0068 | +0.0012 | +0.19 | 3/5 | 0.0037 | +0.04 | 0.47 |
| `comp_all_sn` | +0.0008 | +0.007 | -0.05 | 3/6 | +0.0016 | +0.0067 | +0.40 | 3/5 | 0.0067 | -0.11 | 0.24 |
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
| `qual_fscore` | -0.0186 | -0.248 | -2.42 | 2/6 | -0.0051 | -0.0005 | -0.26 | 2/5 | 0.0069 | +0.09 | 0.22 |
| `qual_lowlev` | -0.0290 | -0.311 | -2.62 | 0/6 | -0.0117 | -0.0233 | -1.62 | 1/5 | 0.0003 | -0.04 | 0.50 |

## Per-year detail, top-1000 (rank IC h = 21; bootstrap 95% CI shown where it excludes zero)

| year | GP/A | net issuance | SUE | B/M | low leverage | F-score | 12-1 momentum |
|---|---|---|---|---|---|---|---|
| 2013 | -0.007 | **+0.049** [+0.016, +0.084] | -0.008 | **-0.035** [-0.066, -0.004] | -0.043 | **-0.053** [-0.083, -0.023] | +0.097 |
| 2014 | +0.031 | +0.019 | +0.007 | -0.009 | **-0.034** [-0.060, -0.005] | +0.020 | +0.039 |
| 2015 | +0.039 | -0.008 | **+0.046** [+0.023, +0.069] | **-0.047** [-0.087, -0.004] | **-0.036** [-0.056, -0.016] | **-0.030** [-0.047, -0.012] | +0.127 |
| 2016 | **-0.071** [-0.121, -0.028] | +0.030 | -0.025 | **+0.086** [+0.044, +0.126] | -0.011 | **-0.038** [-0.061, -0.011] | -0.113 |
| 2017 | **+0.046** [+0.003, +0.088] | +0.027 | **+0.037** [+0.015, +0.059] | **-0.052** [-0.091, -0.012] | -0.010 | +0.001 | +0.065 |
| 2018 | **+0.075** [+0.039, +0.112] | -0.011 | +0.022 | **-0.094** [-0.154, -0.035] | -0.040 | -0.022 | -0.037 |

## Limitations (read before using any number)

1. **Survivor-conditioned id bridge (look-ahead).** This is the main limitation; see the verdict and the diagnostic
   above. An earlier version of this report called it a coverage limitation and said ticker-based bridging had been
   "examined and rejected". That was wrong: the bridge used here *is* a current-ticker match, made upstream in atx-db.
   What was rejected was a second current-ticker match added in this lane (49 of 536 unbridged 2014 names hit a
   *current* SEC ticker, many of them unrelated later issuers, e.g. BEAM, DYN, SI). A point-in-time bridge needs
   CIK/CUSIP or former tickers with validity dates, and the warehouse does not hold these yet.
2. Company Facts values are as-filed XBRL facts. Restatements enter on their own filing date, but the bulk file is a
   2026 snapshot, so facts the SEC later removed cannot be recovered.
3. The definitions are approximations: FF operating profitability lacks interest expense; QMJ lacks its growth and
   payout blocks; Piotroski uses 7 of 9 signals; Stambaugh-Yuan uses 5 of 11 anomalies.
4. The admission mask is the raw context universe (price >= 1), not checkpoint 21's liquidity floor.
5. There are only six yearly observations on top-1000 and five on top-3000, so sign-stability counts are weak
   evidence either way.
6. Share counts consistently mis-scaled in both current and year-ago values (about 2-3% of names by shares x price vs
   market cap) do not affect net issuance, which is a ratio. No zoo expression uses the share level.

## Reproduce

```powershell
python atx-engine\tools\export_fundamental_fields.py --companyfacts C:\atx\atx-db\data\cache\companyfacts.zip `
  --warehouse C:\atx\atx-db\data\warehouse.duckdb.pre-migrate.20260922-235808.bak `
  --contexts "C:/atx/data/equity_scorecard16_ctx_201[3-8]_t*_20260920" --out <fresh dir>
$env:ATX_L10_FUND_POINTS = "<fresh dir>\points.csv"
$env:ATX_L10_FUNDZOO_OUT = "<fresh output dir>"
$env:ATX_L10_CONTEXTS    = "<semicolon-joined equity_scorecard16_ctx_<2013..2018>_<cut>_20260920 dirs>"
$env:ATX_L10_SURVIVOR_CONTEXTS = "<2018 t1000 dir>;<2018 t3000 dir>"
build-equity-rel\bin\atx-impl-tests.exe --gtest_filter=FundamentalZoo.RealDataIcReport
```
