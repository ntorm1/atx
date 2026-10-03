# Dataset ratios

Stage ``ratios`` (tier1-v4 task 4; gap analysis section 2 item 3): a WRDS Financial Ratios Suite (WFRS) analog,
point in time, built from ``compustat_std`` (``--std-stage``; task 5 rebuilds on the v11 stage) and the market stages
(``market/shares_daily``, ``prices``). No new source.

Outputs (rule ``wfrs-pit-v2``, schema ``atx.alpha-panel.ratios/v2``):

* ``ratios/fund_state.parquet`` -- issuer fundamental state vintages keyed (cik, available_at): one row per distinct
  ``available_at`` of the issuer's ``fundq`` / ``funda`` rows (every filing, amendment and restatement vintage). A
  row holds what ``fundq_asof(t)`` / ``funda_asof(t)`` return for any t just after its ``available_at`` (rows with
  ``available_at`` <= the row's, the latest vintage per (cik, datadate) by (available_at, accession)): trailing
  flows, balances, averages, annual history and every price-free ratio. Consumers take the latest row with
  ``available_at`` < their cutoff (strict, GC1).
* ``ratios/split_events.parquet`` -- issuer split / reverse-split events (vendor ``corporate_actions`` of the
  issuer's common lines) with the cumulative log ratio, used to put per-share EPS on one share basis.
* ``ratios/monthly/year=YYYY/ratios_monthly.parquet`` -- (session_date = month-end session m, security_id) for every
  common line (link_table_v3 ``linkprim`` P/J, point-in-time tiers strict / dated / name known by 22:00 UTC of m)
  with a price at m, from 2018-01 to the last complete calendar month. GC1 monthly: fundamentals with
  ``available_at`` < 22:00 UTC of m; prices and shares of m. ``available_at`` = 22:00 UTC of m.
* ``ratios_asof(d)`` (``macros_sql``) -- the daily as-of view: the same columns for session d from the state row with
  ``available_at`` < 22:00 UTC of session d-1 and the shares / raw close of d (GC1 daily).

Definitions (``RATIOS``, WFRS names; 66 original ratios + 3 honestly named extensions + 4 composite scores). Inputs per issuer state:

* Quarter lags: the visible fundq rows (latest vintage per datadate) are placed on quarter offsets j = round((D -
  datadate) / 91.3125 days) from the latest datadate D, kept when within 20 days of j quarters (closest wins), j = 0..8.
* TTM flows (``<x>_ttm``): when the lag-0 row is fiscal quarter 4 and an genuine annual-period funda vintage of D is visible,
  the fiscal-year value; else the sum of the four discrete quarters j = 0..3 (all four rows present and non-NULL);
  else (WFRS: annual data when quarterly data are unavailable) the latest fiscal-year value FY0 when its datadate is
  within 365 days of D (``ttm_fy0_items`` lists the items that fell back); else NULL -- a missing quarter is never
  zero-filled. YTD cash-flow items (oancfy capxy dvy xrdy sstky prstkcy): fqtr 4 = the YTD (the fiscal year);
  fqtr k < 4 = YTD(j0) + YTD(prior fiscal-year end, j = k, fqtr 4) - YTD(j = 4, same fqtr); else the FY0 fallback.
  ``ttm_src`` = ``annual`` / ``q4`` / NULL (the row structure).
* Balances: lag 0 (``<x>_q0``) and lag 4 (``<x>_q4``, the same quarter a year earlier); averages ("most recent two
  periods", WFRS) = (q0 + q4) / 2, NULL when either balance is missing; ``avg_fallback`` flags unavailable lag-4 assets.
* EPS: per-share TTM sums put every quarter on the share basis of the state's date by the issuer's split events
  between the quarter's filing date and that date (a split before a filing is already restated in it); ``csho_b`` =
  the lag-0 cshoq on the same basis. ``epsfi_ttm`` (incl. discontinued operations): annual EPSFI or a flagged FY0 fallback;
  fundq carries no quarterly total EPSFI, so no NI/IB inference is permitted.
* Book equity ``be`` = seq + txditc - pstk (the fundamentals rule; txditc from the latest annual row within 400 days,
  else NULL). Invested capital ``icapt`` = dltt + pstk + mib + ceq. EV debt = dltt + dlc + pstk + mib -
  che. Unreported ``mib`` remains NULL; every missing input makes the derived ratio NULL.
* Annual rows (funda, including later comparative restatements): FY0 = the latest visible fiscal year, FY-k by 365.25-day offsets (+-30 d).
  ``ptpm``, ``efftax``, ``adv_sale`` (no quarterly pretax income / advertising in fundq), ``capei``, the EPS growth
  of ``peg_trailing`` and the composite scores use annual values.
* Market equity ``me`` = sum over the issuer's common lines (linkprim P/J, modal FINRA type not preferred / warrant /
  unit / right / note / ETF / ETN / fund -- the market_shares rule) of ``shares_daily.shrout`` x raw close of the
  session (NULL when a priced common line has no shrout); ``prc`` = the line's raw close. P/E uses issuer aggregate ME / NI or IB in USD. It is an explicitly qualified cap/earnings analog;
  diluted EPS, cover-date shares and ordinary/ADR per-share conventions do not enter its denominator; ``eps_adj`` reports the issuer splits between the state's date and
  the session.
* Staleness: quarterly-based values are NULL when session - D > 400 days (550 when the lag-0 form is 20-F / 40-F,
  annual-only filers); annual-based values when session - FY0 datadate > 550 days.

Commands (every one under ``run_memory_guarded.py``)::

    python -m atx_db.alpha_panel.ratios build    [--std-stage compustat_std] [--stage ratios]
    python -m atx_db.alpha_panel.ratios validate [--stage ratios]        # coverage, spearman, ranges
    python -m atx_db.alpha_panel.ratios handcheck [--n 20]               # SEC 10-K inline XBRL (network)


## Formula authority and unavailable primitives

[WRDS manual, appendix pp4-7](https://people.tamu.edu/~cschulman/ECMT461/Termp/WRDS_Industry_Financial_Ratio_Manual.pdf) is the named-ratio authority. Primarype_exi/pe_inc are dilutedEPS multiples; PEG_trailing is its3yeargrowth multiple; these three remain NULL until certified class/ADR/EPS/share/split alignment is available. divyield requires an indicated dividend rate and is NULL until Task13 supplies it. Explicit ratio_missing_primitives carries these reasons; these missing inputs remain in coverage denominators.

Three distinct extensions are pe_issuer_ni=issuerME/parentNI_TTM, pe_issuer_ib=issuerME/IB_TTM and divyield_paid=paidcommoncashdividends_TTM/issuerME. [Wharton chapter8](https://finance.wharton.upenn.edu/~mrrobert/resources/FDM/fdm-20230823.pdf) supports the cap/earnings representation. Market equity aggregates actual session raw close times shares over commonP/J lines; all amounts are USD. These are class-aggregate analogs; preferred distributions are not independently subtracted from NI. No dilutedEPS equivalence is claimed.

ROE uses NI/average(SEQ+TXDITC); npm and after-tax returns use NI. debt_at=LT/AT, debt_assets=(DLTT+DLC)/AT. The machine mapping records every implemented formula rather than asserting uncertified vendor numerical equivalence.

Daily macros share monthly formula SQL; daily fundamental cutoff is prior calendar session22UTC strictly; monthly is month-end22UTC strictly. Vintages include successive clocks and genuine annual comparative restatements reported in later10Q filings.

## Columns

|name|type|unit|definition/source|clock and history|
|---|---|---|---|---|
|capei|DOUBLE|ratio/score (cash_conversion: days)|Shiller's cyclically adjusted P/E: market equity / mean net income of the last five fiscal years (nominal); compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|
|bm|DOUBLE|ratio/score (cash_conversion: days)|Book/Market: book equity / market equity; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|evm|DOUBLE|ratio/score (cash_conversion: days)|Enterprise value multiple: (me + dltt + dlc + pstk + mib - che) / EBITDA (oibdp) TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pe_issuer_ib|DOUBLE|ratio/score (cash_conversion: days)|Issuer aggregate common market equity / income before extraordinary items TTM (USD); cap/earnings analog, not diluted EPS P/E; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pe_issuer_ni|DOUBLE|ratio/score (cash_conversion: days)|Issuer aggregate common market equity / parent net income TTM (USD); cap/earnings analog, not diluted EPS P/E; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pe_exi|DOUBLE|ratio/score (cash_conversion: days)|WFRS price / diluted EPS excluding EI; NULL until certified security-class/ADR/EPS share-basis alignment is available; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pe_inc|DOUBLE|ratio/score (cash_conversion: days)|WFRS price / diluted EPS including EI; NULL until certified security-class/ADR/EPS share-basis alignment is available; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|ps|DOUBLE|ratio/score (cash_conversion: days)|Price/Sales: market equity / sales TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pcf|DOUBLE|ratio/score (cash_conversion: days)|Price/Cash flow: market equity / operating cash flow TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|dpr|DOUBLE|ratio/score (cash_conversion: days)|Dividend payout ratio: common cash dividends TTM / income before extraordinary items TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|peg_trailing|DOUBLE|ratio/score (cash_conversion: days)|Trailing PEG: pe_exi / 3-year compound annual growth (percent) of annual diluted EPS excl. EI (both > 0); compustat_std/market|qa freshness; strict availability; missing actual input/history remains NULL|
|divyield_paid|DOUBLE|ratio/score (cash_conversion: days)|Dividend yield: common cash dividends TTM / market equity; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|ptb|DOUBLE|ratio/score (cash_conversion: days)|Price/Book: market equity / common equity; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|divyield|DOUBLE|ratio/score (cash_conversion: days)|Indicated dividend rate / price (WFRS); NULL: no source-backed indicated rate primitive; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|npm|DOUBLE|ratio/score (cash_conversion: days)|Net profit margin: net income TTM / sales TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|opmbd|DOUBLE|ratio/score (cash_conversion: days)|Operating profit margin before depreciation: oibdp TTM / sales TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|opmad|DOUBLE|ratio/score (cash_conversion: days)|Operating profit margin after depreciation: oiadp TTM / sales TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|gpm|DOUBLE|ratio/score (cash_conversion: days)|Gross profit margin: (sales - cogs) TTM / sales TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|ptpm|DOUBLE|ratio/score (cash_conversion: days)|Pre-tax profit margin: pretax income / sales (latest fiscal year); compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|
|cfm|DOUBLE|ratio/score (cash_conversion: days)|Cash flow margin: (income before extraordinary items + depreciation) TTM / sales TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|roa|DOUBLE|ratio/score (cash_conversion: days)|Return on assets: oibdp TTM / average total assets; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|roe|DOUBLE|ratio/score (cash_conversion: days)|Return on equity: NI TTM / average (parent stockholder equity + deferred taxes and investment tax credit); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|roce|DOUBLE|ratio/score (cash_conversion: days)|Return on capital employed: EBIT (NI + tax + interest) TTM / average (dltt + dlc + ceq); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|efftax|DOUBLE|ratio/score (cash_conversion: days)|Effective tax rate: income taxes / pretax income (latest fiscal year); compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|
|aftret_eq|DOUBLE|ratio/score (cash_conversion: days)|After-tax return on average common equity: NI TTM / average ceq; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|aftret_invcapx|DOUBLE|ratio/score (cash_conversion: days)|After-tax return on invested capital: (NI + interest) TTM / invested capital; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|aftret_equity|DOUBLE|ratio/score (cash_conversion: days)|After-tax return on total stockholders' equity: NI TTM / average seq; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pretret_noa|DOUBLE|ratio/score (cash_conversion: days)|Pre-tax return on net operating assets: oiadp TTM / average (ppent + act - lct); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pretret_earnat|DOUBLE|ratio/score (cash_conversion: days)|Pre-tax return on total earning assets: oiadp TTM / average (ppent + act); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|gprof|DOUBLE|ratio/score (cash_conversion: days)|Gross profit / total assets (WFRS GProf); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|equity_invcap|DOUBLE|ratio/score (cash_conversion: days)|Common equity / invested capital; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|debt_invcap|DOUBLE|ratio/score (cash_conversion: days)|Long-term debt / invested capital; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|totdebt_invcap|DOUBLE|ratio/score (cash_conversion: days)|Total debt (dltt + dlc) / invested capital; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|capital_ratio|DOUBLE|ratio/score (cash_conversion: days)|Capitalization ratio: dltt / (dltt + ceq + pstk); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|int_debt|DOUBLE|ratio/score (cash_conversion: days)|Interest TTM / average long-term debt; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|int_totdebt|DOUBLE|ratio/score (cash_conversion: days)|Interest TTM / average total debt; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|cash_lt|DOUBLE|ratio/score (cash_conversion: days)|Cash and short-term investments / total liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|invt_act|DOUBLE|ratio/score (cash_conversion: days)|Inventory / current assets; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|rect_act|DOUBLE|ratio/score (cash_conversion: days)|Receivables / current assets; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|debt_at|DOUBLE|ratio/score (cash_conversion: days)|Total liabilities / total assets (WFRS debt_at); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|debt_ebitda|DOUBLE|ratio/score (cash_conversion: days)|Total debt / EBITDA TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|short_debt|DOUBLE|ratio/score (cash_conversion: days)|Short-term debt / total debt; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|curr_debt|DOUBLE|ratio/score (cash_conversion: days)|Current liabilities / total liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|lt_debt|DOUBLE|ratio/score (cash_conversion: days)|Long-term debt / total liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|profit_lct|DOUBLE|ratio/score (cash_conversion: days)|Operating income before depreciation TTM / current liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|ocf_lct|DOUBLE|ratio/score (cash_conversion: days)|Operating cash flow TTM / current liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|cash_debt|DOUBLE|ratio/score (cash_conversion: days)|Operating cash flow TTM / total debt; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|fcf_ocf|DOUBLE|ratio/score (cash_conversion: days)|Free cash flow / operating cash flow; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|lt_ppent|DOUBLE|ratio/score (cash_conversion: days)|Total liabilities / total tangible assets (AT - INTAN); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|dltt_be|DOUBLE|ratio/score (cash_conversion: days)|Long-term debt / book equity; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|de_ratio|DOUBLE|ratio/score (cash_conversion: days)|Total debt/equity: total liabilities / shareholders' equity (common and preferred); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|debt_assets|DOUBLE|ratio/score (cash_conversion: days)|Financial debt / total assets; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|debt_capital|DOUBLE|ratio/score (cash_conversion: days)|Total debt/capital: (ap + dlc + dltt) / (ap + dlc + dltt + ceq + pstk); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|intcov|DOUBLE|ratio/score (cash_conversion: days)|After-tax interest coverage: (xint + ib) TTM / xint TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|intcov_ratio|DOUBLE|ratio/score (cash_conversion: days)|Interest coverage ratio: EBIT (NI + tax + interest) TTM / xint TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|cash_ratio|DOUBLE|ratio/score (cash_conversion: days)|Cash and short-term investments / current liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|quick_ratio|DOUBLE|ratio/score (cash_conversion: days)|Quick ratio (acid test): (current assets - inventories) / current liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|curr_ratio|DOUBLE|ratio/score (cash_conversion: days)|Current ratio: current assets / current liabilities; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|cash_conversion|DOUBLE|ratio/score (cash_conversion: days)|Cash conversion cycle (days): 365 x (inventories / cogs + receivables / sales - payables / cogs), current balances and TTM flows; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|at_turn|DOUBLE|ratio/score (cash_conversion: days)|Asset turnover: sales TTM / average total assets; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|inv_turn|DOUBLE|ratio/score (cash_conversion: days)|Inventory turnover: cogs TTM / average inventories; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|pay_turn|DOUBLE|ratio/score (cash_conversion: days)|Payables turnover: (cogs TTM + change in inventories over the year) / average accounts payable; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|rect_turn|DOUBLE|ratio/score (cash_conversion: days)|Receivables turnover: sales TTM / average receivables; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|sale_equity|DOUBLE|ratio/score (cash_conversion: days)|Sales TTM / stockholders' equity; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|sale_invcap|DOUBLE|ratio/score (cash_conversion: days)|Sales TTM / invested capital; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|sale_nwc|DOUBLE|ratio/score (cash_conversion: days)|Sales TTM / working capital (act - lct); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|accrual|DOUBLE|ratio/score (cash_conversion: days)|Accruals / average total assets: (ib - oancf) TTM / average at (Sloan sign); compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|rd_sale|DOUBLE|ratio/score (cash_conversion: days)|R&D expense TTM / sales TTM; compustat_std/market|q freshness; strict availability; missing actual input/history remains NULL|
|adv_sale|DOUBLE|ratio/score (cash_conversion: days)|Advertising expense / sales (latest fiscal year); compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|
|piotroski_f|INTEGER|ratio/score (cash_conversion: days)|Piotroski (2000) F-score: sum of nine indicators on annual data (f_roa ib/at_t-1 > 0; f_cfo oancf/at_t-1 > 0; f_droa change in roa > 0; f_accrual cfo > roa; f_dlever change in dltt / average at < 0; f_dliquid change in current ratio > 0; f_eqoff sstk = 0; f_dmargin change in gross margin > 0; f_dturn change in sale/at_t-1 > 0); NULL unless all nine are defined; compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|
|altman_z|DOUBLE|ratio/score (cash_conversion: days)|Altman (1968) Z = 1.2 (act - lct)/at + 1.4 re/at + 3.3 oiadp/at + 0.6 me/lt + 1.0 sale/at (latest fiscal year; me of the session); compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|
|ohlson_o|DOUBLE|ratio/score (cash_conversion: days)|Ohlson (1980) model 1 O = -1.32 - 0.407 ln(at, USD millions, undeflated) + 6.03 lt/at - 1.43 (act - lct)/at + 0.0757 lct/act - 1.72 [lt > at] - 2.37 ni/at - 1.83 oancf/lt + 0.285 [ni < 0 both years] - 0.521 (ni_t - ni_t-1)/(|ni_t| + |ni_t-1|) (latest fiscal year); compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|
|beneish_m|DOUBLE|ratio/score (cash_conversion: days)|Beneish (1999) 8-variable M = -4.84 + 0.920 DSRI + 0.528 GMI + 0.404 AQI + 0.892 SGI + 0.115 DEPI - 0.172 SGAI + 4.679 TATA - 0.327 LVGI (fiscal years t and t-1; TATA = (ib - oancf)/at); compustat_std/market|a freshness; strict availability; missing actual input/history remains NULL|

session_date/datadate/fy0_datadate:DATE; security_id/cik/fqtr/hist_days/me_lines:BIGINT; available_at/fund_available_at:TIMESTAMP UTC; fresh_q/fresh_a/fx_converted:BOOLEAN; market values:DOUBLE; other descriptors and ratio_missing_primitives:VARCHAR. Keys are(session_date,security_id), issuer states(cik,available_at). Source clocks and stale flags are retained. Missing averages require both actual prior-year/current balances; missing or zero denominator never becomes zero. ratio_undefined marks zero-denominator exclusions; ratio_history_missing records missing history separately; absent_items marks missing history, never structural proof.

## Measured yearly coverage

Exact formula/primitive/producer mapping is ratios.requirements.json; ratios.coverage.json stores every eligible count, all-linked fraction, primitive count and source/universe hash. Eligibility uses only explicitbank/insurercogs/inventory applicability, declared zero-denominator exclusions; observed absence stays missing. All-common/R3000 universes retain rows missing from ratios in denominators. Source unavailable counterparts are separately visible primary FAILs.

coverage; role totals {"composites": {"fails": ["piotroski_f", "altman_z", "ohlson_o", "beneish_m"], "n_pass": 0, "total": 4}, "extensions": {"fails": [], "n_pass": 3, "total": 3}, "primary": {"fails": ["evm", "pe_exi", "pe_inc", "peg_trailing", "divyield", "opmbd", "opmad", "gpm", "cfm", "roa", "roe", "roce", "aftret_invcapx", "pretret_noa", "pretret_earnat", "gprof", "int_debt", "int_totdebt", "invt_act", "rect_act", "debt_ebitda", "curr_debt", "profit_lct", "ocf_lct", "debt_capital", "intcov", "intcov_ratio", "cash_ratio", "quick_ratio", "curr_ratio", "cash_conversion", "inv_turn", "pay_turn", "rect_turn", "sale_invcap", "sale_nwc", "adv_sale"], "n_pass": 29, "total": 66}}

|metric|2020|2021|2022|2023|2024|2025|gate|
|---|---|---|---|---|---|---|---|
|accrual|0.9525|0.9183|0.9437|0.973|0.9697|0.9652|PASS|
|adv_sale|0.3702|0.3678|0.3726|0.3673|0.3593|0.3628|FAIL|
|aftret_eq|0.9526|0.9214|0.945|0.9732|0.9717|0.9679|PASS|
|aftret_equity|0.9531|0.9214|0.9454|0.974|0.9719|0.968|PASS|
|aftret_invcapx|0.761|0.7464|0.7549|0.7523|0.753|0.7586|FAIL|
|altman_z|0.7506|0.7496|0.7638|0.7719|0.7719|0.7745|FAIL|
|at_turn|0.9405|0.91|0.9345|0.9619|0.9563|0.9513|PASS|
|beneish_m|0.5785|0.5693|0.5807|0.5967|0.5882|0.589|FAIL|
|bm|0.9591|0.9433|0.9601|0.9728|0.9715|0.971|PASS|
|capei|0.9361|0.919|0.9398|0.9577|0.9604|0.9536|PASS|
|capital_ratio|0.9869|0.9893|0.989|0.9909|0.9912|0.9907|PASS|
|cash_conversion|0.4929|0.4628|0.4753|0.4842|0.4856|0.4954|FAIL|
|cash_debt|0.9752|0.9619|0.9762|0.9851|0.9836|0.9808|PASS|
|cash_lt|0.9846|0.9866|0.9858|0.9884|0.9882|0.9878|PASS|
|cash_ratio|0.8004|0.821|0.8145|0.8102|0.8105|0.8133|FAIL|
|cfm|0.9237|0.8947|0.9137|0.9196|0.9191|0.9187|FAIL|
|curr_debt|0.7982|0.8199|0.8125|0.808|0.8094|0.8126|FAIL|
|curr_ratio|0.801|0.8224|0.8157|0.811|0.8115|0.8138|FAIL|
|de_ratio|0.9869|0.9893|0.989|0.9907|0.9912|0.9907|PASS|
|debt_assets|0.9899|0.9923|0.993|0.9941|0.9934|0.9921|PASS|
|debt_at|0.9869|0.9893|0.989|0.9907|0.9912|0.9907|PASS|
|debt_capital|0.8008|0.8192|0.8157|0.8087|0.8013|0.7997|FAIL|
|debt_ebitda|0.8503|0.8254|0.846|0.8496|0.8487|0.8489|FAIL|
|debt_invcap|0.9068|0.9086|0.9045|0.9061|0.9082|0.9053|PASS|
|divyield|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|divyield_paid|0.953|0.9268|0.9516|0.9676|0.9647|0.9635|PASS|
|dltt_be|0.9733|0.9595|0.9745|0.9857|0.9837|0.9833|PASS|
|dpr|0.9667|0.9415|0.9648|0.9785|0.975|0.9738|PASS|
|efftax|0.962|0.9381|0.9656|0.9778|0.9761|0.9753|PASS|
|equity_invcap|0.9068|0.9086|0.9045|0.9061|0.9082|0.9053|PASS|
|evm|0.7749|0.7528|0.7712|0.7749|0.7757|0.7713|FAIL|
|fcf_ocf|0.9474|0.9185|0.9383|0.9549|0.9507|0.9486|PASS|
|gpm|0.7835|0.7585|0.767|0.7809|0.7865|0.7974|FAIL|
|gprof|0.7686|0.7405|0.7556|0.768|0.7708|0.7799|FAIL|
|int_debt|0.8179|0.7888|0.8079|0.8255|0.8231|0.8265|FAIL|
|int_totdebt|0.8185|0.7908|0.8087|0.8258|0.8237|0.827|FAIL|
|intcov|0.819|0.8056|0.8173|0.8132|0.8103|0.8162|FAIL|
|intcov_ratio|0.8073|0.795|0.8088|0.8049|0.8017|0.8078|FAIL|
|inv_turn|0.5255|0.4936|0.5075|0.5296|0.5353|0.5456|FAIL|
|invt_act|0.5896|0.5792|0.5882|0.5988|0.6016|0.6081|FAIL|
|lt_debt|0.9869|0.9894|0.989|0.9907|0.9912|0.9907|PASS|
|lt_ppent|0.9264|0.9313|0.9358|0.9351|0.9287|0.9337|PASS|
|npm|0.961|0.9443|0.9639|0.9721|0.9689|0.966|PASS|
|ocf_lct|0.7857|0.7828|0.7995|0.8043|0.8026|0.8056|FAIL|
|ohlson_o|0.7712|0.774|0.7794|0.7949|0.7962|0.7972|FAIL|
|opmad|0.8743|0.8657|0.8806|0.8859|0.883|0.8813|FAIL|
|opmbd|0.8427|0.8234|0.8397|0.8432|0.8427|0.841|FAIL|
|pay_turn|0.5149|0.4821|0.4961|0.5139|0.5146|0.5236|FAIL|
|pcf|0.9596|0.9357|0.9607|0.9737|0.9716|0.9699|PASS|
|pe_exi|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|pe_inc|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|pe_issuer_ib|0.9629|0.9434|0.9643|0.9753|0.9739|0.972|PASS|
|pe_issuer_ni|0.9629|0.9434|0.9648|0.9764|0.9748|0.9728|PASS|
|peg_trailing|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|piotroski_f|0.6615|0.661|0.6734|0.6831|0.6885|0.6925|FAIL|
|pretret_earnat|0.7216|0.7065|0.7353|0.7636|0.7476|0.7397|FAIL|
|pretret_noa|0.7196|0.7041|0.7339|0.7623|0.7467|0.7389|FAIL|
|profit_lct|0.7522|0.7385|0.7551|0.758|0.7578|0.7606|FAIL|
|ps|0.9478|0.9304|0.9509|0.9607|0.9584|0.9551|PASS|
|ptb|0.9724|0.9713|0.975|0.9787|0.9795|0.9781|PASS|
|ptpm|0.9521|0.9358|0.9563|0.9657|0.9625|0.9614|PASS|
|quick_ratio|0.5882|0.5778|0.5868|0.5977|0.6006|0.6073|FAIL|
|rd_sale|0.9511|0.9359|0.9593|0.968|0.9656|0.9634|PASS|
|rect_act|0.712|0.7195|0.7334|0.7229|0.7089|0.7088|FAIL|
|rect_turn|0.6922|0.6711|0.6979|0.7179|0.6993|0.6961|FAIL|
|roa|0.8312|0.7971|0.8198|0.8423|0.8371|0.8351|FAIL|
|roce|0.8025|0.778|0.7933|0.8068|0.8051|0.8093|FAIL|
|roe|0.9303|0.8984|0.9154|0.9577|0.9644|0.9582|FAIL|
|sale_equity|0.9584|0.9428|0.9606|0.9691|0.967|0.9656|PASS|
|sale_invcap|0.8799|0.864|0.8769|0.8852|0.8847|0.8807|FAIL|
|sale_nwc|0.7796|0.7844|0.7953|0.7991|0.7964|0.7988|FAIL|
|short_debt|0.9882|0.9902|0.9917|0.9932|0.9925|0.991|PASS|
|totdebt_invcap|0.9068|0.9086|0.9045|0.9061|0.9082|0.9053|PASS|

coverage_univ_all_common; role totals {"composites": {"fails": ["piotroski_f", "altman_z", "ohlson_o", "beneish_m"], "n_pass": 0, "total": 4}, "extensions": {"fails": [], "n_pass": 3, "total": 3}, "primary": {"fails": ["evm", "pe_exi", "pe_inc", "peg_trailing", "divyield", "opmbd", "opmad", "gpm", "cfm", "roa", "roe", "roce", "aftret_invcapx", "pretret_noa", "pretret_earnat", "gprof", "int_debt", "int_totdebt", "invt_act", "rect_act", "debt_ebitda", "curr_debt", "profit_lct", "ocf_lct", "debt_capital", "intcov", "intcov_ratio", "cash_ratio", "quick_ratio", "curr_ratio", "cash_conversion", "at_turn", "inv_turn", "pay_turn", "rect_turn", "sale_invcap", "sale_nwc", "adv_sale"], "n_pass": 28, "total": 66}}

|metric|2020|2021|2022|2023|2024|2025|gate|
|---|---|---|---|---|---|---|---|
|accrual|0.9527|0.9121|0.9194|0.9745|0.9727|0.9668|PASS|
|adv_sale|0.3998|0.3815|0.4157|0.4255|0.4237|0.4275|FAIL|
|aftret_eq|0.9551|0.9182|0.9255|0.9761|0.9742|0.968|PASS|
|aftret_equity|0.9556|0.9183|0.9259|0.9765|0.9743|0.968|PASS|
|aftret_invcapx|0.7627|0.7363|0.7483|0.7468|0.743|0.7572|FAIL|
|altman_z|0.7555|0.7357|0.7672|0.7848|0.7854|0.7888|FAIL|
|at_turn|0.9363|0.893|0.9022|0.952|0.9509|0.9443|FAIL|
|beneish_m|0.5828|0.56|0.5738|0.5941|0.5844|0.5896|FAIL|
|bm|0.9664|0.9423|0.9622|0.9796|0.9782|0.9787|PASS|
|capei|0.9302|0.9031|0.9261|0.9522|0.9514|0.9418|PASS|
|capital_ratio|0.9965|0.9955|0.9965|0.9975|0.9978|0.9979|PASS|
|cash_conversion|0.4908|0.4564|0.4616|0.4682|0.4607|0.4658|FAIL|
|cash_debt|0.98|0.956|0.9721|0.9862|0.9864|0.9847|PASS|
|cash_lt|0.9937|0.9919|0.9931|0.9942|0.9948|0.995|PASS|
|cash_ratio|0.8039|0.8195|0.8285|0.8266|0.8289|0.8309|FAIL|
|cfm|0.9307|0.8935|0.9203|0.9364|0.933|0.9347|FAIL|
|curr_debt|0.8049|0.8211|0.8296|0.8282|0.8304|0.8326|FAIL|
|curr_ratio|0.8049|0.8204|0.8284|0.8272|0.8289|0.8316|FAIL|
|de_ratio|0.9965|0.9954|0.9964|0.9975|0.9978|0.9979|PASS|
|debt_assets|0.9975|0.9963|0.9973|0.998|0.9984|0.9979|PASS|
|debt_at|0.9964|0.9954|0.9964|0.9974|0.9978|0.9977|PASS|
|debt_capital|0.805|0.8192|0.8315|0.8236|0.8175|0.8167|FAIL|
|debt_ebitda|0.8152|0.7888|0.8168|0.8296|0.8286|0.8353|FAIL|
|debt_invcap|0.9294|0.9316|0.9315|0.9316|0.9326|0.9349|PASS|
|divyield|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|divyield_paid|0.9577|0.924|0.9479|0.9719|0.97|0.9704|PASS|
|dltt_be|0.9759|0.9519|0.9713|0.9872|0.9857|0.9857|PASS|
|dpr|0.9669|0.9332|0.9569|0.9791|0.977|0.977|PASS|
|efftax|0.9587|0.9207|0.9486|0.9691|0.9663|0.9661|PASS|
|equity_invcap|0.9294|0.9316|0.9315|0.9316|0.9326|0.9349|PASS|
|evm|0.7514|0.7297|0.7573|0.7687|0.7698|0.7761|FAIL|
|fcf_ocf|0.956|0.9181|0.9427|0.964|0.9601|0.9562|PASS|
|gpm|0.7666|0.7251|0.7575|0.7787|0.7725|0.7856|FAIL|
|gprof|0.7326|0.6852|0.711|0.7308|0.7274|0.7385|FAIL|
|int_debt|0.7962|0.7458|0.7495|0.7986|0.7973|0.8004|FAIL|
|int_totdebt|0.7988|0.7491|0.7512|0.7994|0.7958|0.8019|FAIL|
|intcov|0.797|0.7653|0.7742|0.7716|0.7679|0.7829|FAIL|
|intcov_ratio|0.787|0.7522|0.7633|0.7609|0.7567|0.771|FAIL|
|inv_turn|0.5269|0.4866|0.4799|0.5116|0.5087|0.5169|FAIL|
|invt_act|0.592|0.566|0.5673|0.5756|0.5724|0.5798|FAIL|
|lt_debt|0.9971|0.9958|0.9964|0.9974|0.9979|0.9979|PASS|
|lt_ppent|0.9296|0.9318|0.9399|0.9335|0.9279|0.9345|PASS|
|npm|0.9561|0.9213|0.9457|0.9622|0.9599|0.9585|PASS|
|ocf_lct|0.7848|0.7708|0.8021|0.8186|0.8185|0.8201|FAIL|
|ohlson_o|0.7742|0.7644|0.7718|0.8039|0.8106|0.8129|FAIL|
|opmad|0.8208|0.7985|0.8257|0.8407|0.8401|0.8424|FAIL|
|opmbd|0.8002|0.7759|0.8054|0.8193|0.8176|0.8212|FAIL|
|pay_turn|0.518|0.478|0.4726|0.4999|0.492|0.4979|FAIL|
|pcf|0.9654|0.9326|0.958|0.9793|0.9778|0.9771|PASS|
|pe_exi|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|pe_inc|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|pe_issuer_ib|0.9687|0.9403|0.9636|0.9807|0.9792|0.9786|PASS|
|pe_issuer_ni|0.9685|0.9403|0.9638|0.9804|0.9789|0.9782|PASS|
|peg_trailing|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|piotroski_f|0.6624|0.6392|0.6649|0.6823|0.6681|0.6734|FAIL|
|pretret_earnat|0.7194|0.6918|0.7007|0.7547|0.7489|0.748|FAIL|
|pretret_noa|0.7168|0.6894|0.6987|0.7533|0.7481|0.747|FAIL|
|profit_lct|0.7571|0.7355|0.7648|0.7767|0.7764|0.7831|FAIL|
|ps|0.9473|0.9127|0.9369|0.9551|0.9533|0.9516|PASS|
|ptb|0.9881|0.9865|0.988|0.9908|0.9909|0.9913|PASS|
|ptpm|0.9528|0.9159|0.9406|0.9585|0.9566|0.9564|PASS|
|quick_ratio|0.591|0.5655|0.5667|0.5754|0.5722|0.5795|FAIL|
|rd_sale|0.9478|0.9138|0.9389|0.9564|0.9543|0.9524|PASS|
|rect_act|0.6695|0.6598|0.6749|0.6714|0.6646|0.6661|FAIL|
|rect_turn|0.6374|0.6024|0.6064|0.6425|0.6339|0.633|FAIL|
|roa|0.798|0.7644|0.7766|0.8209|0.8192|0.821|FAIL|
|roce|0.79|0.7498|0.7495|0.7774|0.7734|0.7814|FAIL|
|roe|0.9242|0.8897|0.8674|0.9465|0.9594|0.9508|FAIL|
|sale_equity|0.9569|0.9243|0.9475|0.9634|0.9614|0.9604|PASS|
|sale_invcap|0.8901|0.8612|0.8832|0.8975|0.8973|0.8985|FAIL|
|sale_nwc|0.7718|0.7588|0.7877|0.8005|0.8|0.8025|FAIL|
|short_debt|0.9976|0.9954|0.9961|0.9971|0.9976|0.997|PASS|
|totdebt_invcap|0.9294|0.9316|0.9315|0.9316|0.9326|0.9349|PASS|

coverage_univ_r3000p; role totals {"composites": {"fails": ["piotroski_f", "altman_z", "ohlson_o", "beneish_m"], "n_pass": 0, "total": 4}, "extensions": {"fails": ["divyield_paid"], "n_pass": 2, "total": 3}, "primary": {"fails": ["capei", "evm", "pe_exi", "pe_inc", "ps", "peg_trailing", "divyield", "opmbd", "opmad", "gpm", "ptpm", "cfm", "roa", "roe", "roce", "aftret_invcapx", "pretret_noa", "pretret_earnat", "gprof", "equity_invcap", "debt_invcap", "totdebt_invcap", "int_debt", "int_totdebt", "invt_act", "rect_act", "debt_ebitda", "curr_debt", "profit_lct", "ocf_lct", "fcf_ocf", "lt_ppent", "debt_capital", "intcov", "intcov_ratio", "cash_ratio", "quick_ratio", "curr_ratio", "cash_conversion", "at_turn", "inv_turn", "pay_turn", "rect_turn", "sale_invcap", "sale_nwc", "rd_sale", "adv_sale"], "n_pass": 19, "total": 66}}

|metric|2020|2021|2022|2023|2024|2025|gate|
|---|---|---|---|---|---|---|---|
|accrual|0.9087|0.9155|0.9284|0.9473|0.9541|0.9471|PASS|
|adv_sale|0.3512|0.3573|0.3815|0.3854|0.3847|0.3896|FAIL|
|aftret_eq|0.9105|0.9175|0.9309|0.9477|0.955|0.948|PASS|
|aftret_equity|0.9108|0.9175|0.9314|0.9483|0.9551|0.948|PASS|
|aftret_invcapx|0.7247|0.7324|0.742|0.7385|0.7392|0.7517|FAIL|
|altman_z|0.6642|0.6827|0.709|0.7142|0.7114|0.7022|FAIL|
|at_turn|0.8943|0.8997|0.9109|0.9265|0.9284|0.9208|FAIL|
|beneish_m|0.524|0.5343|0.5466|0.5653|0.559|0.5534|FAIL|
|bm|0.9069|0.9162|0.9317|0.9417|0.9418|0.9388|PASS|
|capei|0.8699|0.884|0.8965|0.9115|0.9184|0.9082|FAIL|
|capital_ratio|0.9259|0.94|0.956|0.962|0.9614|0.9598|PASS|
|cash_conversion|0.4366|0.4349|0.4424|0.4368|0.4305|0.4312|FAIL|
|cash_debt|0.9009|0.9157|0.9363|0.9464|0.9477|0.9436|PASS|
|cash_lt|0.9232|0.9367|0.9523|0.9587|0.9589|0.9577|PASS|
|cash_ratio|0.6884|0.7134|0.7395|0.7392|0.7359|0.7259|FAIL|
|cfm|0.8748|0.8817|0.8995|0.8995|0.9022|0.9012|FAIL|
|curr_debt|0.6888|0.7136|0.7401|0.7401|0.7367|0.7269|FAIL|
|curr_ratio|0.6894|0.7145|0.7409|0.7406|0.737|0.7269|FAIL|
|de_ratio|0.926|0.9398|0.9559|0.9619|0.9614|0.9598|PASS|
|debt_assets|0.927|0.941|0.957|0.9626|0.962|0.9598|PASS|
|debt_at|0.9258|0.9398|0.9559|0.9619|0.9614|0.9598|PASS|
|debt_capital|0.6938|0.7181|0.7445|0.7386|0.7289|0.7184|FAIL|
|debt_ebitda|0.7676|0.7794|0.8026|0.8014|0.8018|0.7966|FAIL|
|debt_invcap|0.857|0.8726|0.885|0.8951|0.8957|0.8967|FAIL|
|divyield|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|divyield_paid|0.8968|0.9044|0.9204|0.9337|0.9328|0.9293|FAIL|
|dltt_be|0.9236|0.9345|0.9515|0.9597|0.9606|0.9582|PASS|
|dpr|0.9136|0.9227|0.94|0.9507|0.9509|0.9483|PASS|
|efftax|0.9052|0.9171|0.9378|0.9454|0.949|0.9475|PASS|
|equity_invcap|0.857|0.8726|0.885|0.8951|0.8957|0.8967|FAIL|
|evm|0.7027|0.716|0.7362|0.7382|0.739|0.7322|FAIL|
|fcf_ocf|0.8964|0.9025|0.9195|0.9319|0.9288|0.9238|FAIL|
|gpm|0.7099|0.7108|0.7313|0.7377|0.7342|0.7346|FAIL|
|gprof|0.6938|0.691|0.7099|0.7169|0.7115|0.7119|FAIL|
|int_debt|0.7646|0.7691|0.7814|0.786|0.7942|0.7927|FAIL|
|int_totdebt|0.7678|0.7719|0.7833|0.7877|0.7964|0.7966|FAIL|
|intcov|0.7664|0.7718|0.7838|0.7735|0.7741|0.7855|FAIL|
|intcov_ratio|0.7512|0.759|0.7734|0.7627|0.7644|0.7763|FAIL|
|inv_turn|0.471|0.4682|0.4708|0.48|0.482|0.4862|FAIL|
|invt_act|0.5173|0.523|0.5333|0.5304|0.5279|0.5325|FAIL|
|lt_debt|0.9259|0.9398|0.9559|0.9619|0.9614|0.9598|PASS|
|lt_ppent|0.8625|0.8778|0.8983|0.9016|0.8958|0.8993|FAIL|
|npm|0.9047|0.9138|0.9296|0.9321|0.9325|0.9283|PASS|
|ocf_lct|0.6854|0.7058|0.7358|0.7388|0.7356|0.7243|FAIL|
|ohlson_o|0.6746|0.6952|0.7069|0.7287|0.733|0.7207|FAIL|
|opmad|0.7807|0.7942|0.8182|0.8181|0.8141|0.8069|FAIL|
|opmbd|0.7579|0.7691|0.7954|0.793|0.7923|0.7861|FAIL|
|pay_turn|0.4615|0.4581|0.4596|0.4656|0.4619|0.4642|FAIL|
|pcf|0.9046|0.9135|0.9301|0.9413|0.9419|0.9376|PASS|
|pe_exi|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|pe_inc|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|pe_issuer_ib|0.9075|0.9167|0.9329|0.9423|0.9428|0.9386|PASS|
|pe_issuer_ni|0.9075|0.9168|0.9331|0.9423|0.9431|0.9387|PASS|
|peg_trailing|0.0|0.0|0.0|0.0|0.0|0.0|FAIL|
|piotroski_f|0.5853|0.5989|0.6239|0.6351|0.633|0.6282|FAIL|
|pretret_earnat|0.6366|0.6525|0.6746|0.6999|0.6955|0.6759|FAIL|
|pretret_noa|0.6362|0.6516|0.6741|0.6993|0.6952|0.6758|FAIL|
|profit_lct|0.6655|0.6822|0.7078|0.71|0.709|0.6978|FAIL|
|ps|0.8887|0.8962|0.9107|0.9158|0.917|0.9127|FAIL|
|ptb|0.9093|0.9217|0.9366|0.9443|0.9431|0.9406|PASS|
|ptpm|0.8947|0.9042|0.9219|0.9243|0.9259|0.923|FAIL|
|quick_ratio|0.5168|0.5226|0.533|0.5304|0.5279|0.5325|FAIL|
|rd_sale|0.8968|0.9073|0.9255|0.9281|0.9291|0.925|FAIL|
|rect_act|0.6032|0.6167|0.6405|0.6379|0.6253|0.6146|FAIL|
|rect_turn|0.6001|0.6032|0.6155|0.6309|0.6203|0.6063|FAIL|
|roa|0.7574|0.7653|0.785|0.7964|0.7965|0.7887|FAIL|
|roce|0.7596|0.765|0.7742|0.7732|0.7794|0.7869|FAIL|
|roe|0.8858|0.8947|0.8867|0.9247|0.9451|0.9367|FAIL|
|sale_equity|0.9055|0.9148|0.9305|0.9332|0.9336|0.9299|PASS|
|sale_invcap|0.8372|0.8476|0.8609|0.8674|0.8697|0.868|FAIL|
|sale_nwc|0.6797|0.6998|0.7279|0.7299|0.7258|0.715|FAIL|
|short_debt|0.907|0.9231|0.9438|0.9512|0.9497|0.9465|PASS|
|totdebt_invcap|0.857|0.8726|0.885|0.8951|0.8957|0.8967|FAIL|

## Limits and rebuild

No source-backed indicated dividend-rate/classEPS certification yet. Narrow inlineXBRL chains can miss issuer-custom concepts and upstream source defaults; handcheck exceptions remain FAIL with keyed primitive evidence. Ohlson uses nominal assets in millions without an unavailableGNPdeflator; CAPEI uses nominalNI. Dated FINRAline typing uses the latest strictly visible observation. Quarter and annual date tolerances are supplemented by fiscal sequence/calendar screens; transition history stays NULL. Annual source-duration certification remains unavailable. Range envelopes are diagnostics, without complete source-backed literature certification. No return statistics are computed.

Task5 chain: compustat_std build --fund-stage fundamentals_v11 --stage <standardized>; ratios build --std-stage <standardized>. Raw fundamentals is not a ratios input. Task5 owns expanded profitability/growth primitive matrix and source recovery; Task7/16 integrate only named verified formulas. Universe refinement requires later coverage refresh.

Source manifest SHA256: 9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325; universe manifest SHA256: 33a15585807fa242fc4e7eb38ec3c1902bf043ba442f57b2e2a8f45dbad7071d.

Coverage snapshot update: Task 2 subsequently published an intermediate universe and will publish its final universe after the remaining status audit. The recorded 33a15585807fa242fc4e7eb38ec3c1902bf043ba442f57b2e2a8f45dbad7071d coverage snapshot is therefore historical, not current universe coverage. GC3 refresh is pending the final Task 2 hash; the controller will coordinate one coverage refresh. No full ratio rebuild or relabeling of the earlier measurements was performed.


## Fix round 1 ? review-T4-r0

R1 addresses I1/I2/I3 locally; I4 remains open. 26 focused offline tests PASS. Missing retained history/FY0 is now in the coverage denominator and separately exposed in `ratio_history_missing`; only genuine zero denominators and explicit sector applicability exclude cells. Empty structural-mask mappings produce valid SQL. Observed missing-input wording no longer calls absence structural.

Identity links and latest dated FINRA classification require `available_at < cutoff`, using strict prior-calendar-session22UTC daily and month-end22UTC monthly. Same-session newly known links cannot select earlier fundamentals. No all-time modal FINRA classification participates in eligibility. Missing dated FINRA evidence remains unknown, governed by the visible common-link contract; this is not independent historical class certification.

Quarter pairing requires fiscal-quarter progression, fiscal-year consistency and unchanged fiscal calendar as well as elapsed-day tolerance. Annual pairing requires fiscal-year progression, unchanged calendar, and preceding end dates350?380days apart. Tests retain raw annual rows while excluding a335day comparative and a fiscal-calendar transition from annual history pairing. Rejected annual history makes annual/fallback ratios NULL; missing history remains eligible. The standardized producer does not expose period start/duration: this end-date screen is **not source-backed full-period duration certification**. Task5 must publish dated period start/end or a verified full-period/comparability flag. Missing certification remains an explicit consumer qualification, not an equivalence claim.

Ohlson still uses nominal assets/OCF proxy; complete formula/composite source-model certification remains unverified. No hard gate is waived. Main independent arithmetic covers33primary+3extensions; two additional existing-primitive formulas (`aftret_equity`, `profit_lct`) are checked separately by t4-r1-extra-sourcecheck.py.31primary and4composites remain unverified. Both-absent cells never certify formulas. No broader source discovery was performed.

The guarded R1 rebuild/validate/handcheck/extra-sourcecheck commands use `run_memory_guarded.py --job-gb`1.0/.8/.6/.6 respectively, receipt/stdout/stderr paths `logs/t4-r1-{build,validate,handcheck,extra-sourcecheck}.*`, with child commands `-m atx_db.alpha_panel.ratios build --buckets32`, `validate`, `handcheck --n20`, and `../.superpowers/sdd/tier1-v4/t4-r1-extra-sourcecheck.py`. Working directory C:/atx/atx-db; PYTHONPATH=C:/atx/atx-db/src; OPENBLAS_NUM_THREADS=1. Offline command from C:/atx: `atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' atx-db/tests/test_alpha_panel_ratios.py -q`;26passed in19.44s.

GC3 remains pending. R1 validation overlapped Task2 partial output publication: its all-common/R3000 measurements are **provisional/invalid for final acceptance**, regardless of recorded manifest hash. The controller must refresh against completed final output-integrity hashes. Earlier33a15585 measurements remain historical; neither snapshot is relabeled latest.

Exact latest evidence follows (t4-r1-evidence.json); previous sections describe the pre-review snapshot:


The four previously mismatching issuer-date cash cases were local independent-extraction defects: add actually reported MarketableSecuritiesCurrent or use explicitly reported CashCashEquivalentsAndShortTermInvestments. Retained t4-r1-source-cases.json records exact facts (no source-value fitting); the final module fixes these chains and fixtures cover priority/no double counting. Final build preserves source-hash lineage; main sourcecheck was rerun. Coverage/range/correlation formulas were unchanged by this sourcecheck-only correction, so their prior R1 measurements remain applicable with the stated universe invalidity.

Task5 source/history gaps and Task13 indicated dividends remain open. Primary coverage is24/66 member; extensions2/3; composites0/4. Correlations remain below.99 and unsupported/range failures remain open. Provisional all-common/R3000 counts19/66 and11/66 are not valid final acceptance evidence. Preliminary attribution execution from C:/atx failed on a relative artifact path (exit1); successful attribution-2 used C:/atx/atx-db.

```json
{
  "code_matches_manifest": true,
  "input_matches_manifest": true,
  "input_sha256": "9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325",
  "coverage_snapshot_status": {
    "is_latest_universe": false,
    "gc3": "PENDING controller final publication and output-integrity verification",
    "overlap_coverage": "provisional/invalid for final acceptance: Task2 partial publication overlapped validation",
    "measured_manifest_sha256": "d30ef0408b1501bea38fc7e733da7e99df4d4bc9156e3f23b3d65ba39a46031e",
    "manifest_hash_alone_certifies_output_integrity": false
  },
  "gates": {
    "coverage": "FAIL",
    "coverage_univ_all_common": "FAIL",
    "coverage_univ_r3000p": "FAIL",
    "handcheck": "FAIL",
    "ranges": "FAIL",
    "spearman": "FAIL"
  },
  "coverage_roles": {
    "coverage": {
      "composites": {
        "fails": [
          "piotroski_f",
          "altman_z",
          "ohlson_o",
          "beneish_m"
        ],
        "n_pass": 0,
        "total": 4
      },
      "extensions": {
        "fails": [
          "divyield_paid"
        ],
        "n_pass": 2,
        "total": 3
      },
      "primary": {
        "fails": [
          "capei",
          "bm",
          "evm",
          "pe_exi",
          "pe_inc",
          "dpr",
          "peg_trailing",
          "divyield",
          "opmbd",
          "opmad",
          "gpm",
          "ptpm",
          "cfm",
          "roa",
          "roe",
          "roce",
          "efftax",
          "aftret_invcapx",
          "pretret_noa",
          "pretret_earnat",
          "gprof",
          "int_debt",
          "int_totdebt",
          "invt_act",
          "rect_act",
          "debt_ebitda",
          "curr_debt",
          "profit_lct",
          "ocf_lct",
          "debt_capital",
          "intcov",
          "intcov_ratio",
          "cash_ratio",
          "quick_ratio",
          "curr_ratio",
          "cash_conversion",
          "inv_turn",
          "pay_turn",
          "rect_turn",
          "sale_invcap",
          "sale_nwc",
          "adv_sale"
        ],
        "n_pass": 24,
        "total": 66
      }
    },
    "coverage_univ_all_common": {
      "composites": {
        "fails": [
          "piotroski_f",
          "altman_z",
          "ohlson_o",
          "beneish_m"
        ],
        "n_pass": 0,
        "total": 4
      },
      "extensions": {
        "fails": [
          "divyield_paid"
        ],
        "n_pass": 2,
        "total": 3
      },
      "primary": {
        "fails": [
          "capei",
          "bm",
          "evm",
          "pe_exi",
          "pe_inc",
          "ps",
          "dpr",
          "peg_trailing",
          "divyield",
          "opmbd",
          "opmad",
          "gpm",
          "ptpm",
          "cfm",
          "roa",
          "roe",
          "roce",
          "efftax",
          "aftret_invcapx",
          "pretret_noa",
          "pretret_earnat",
          "gprof",
          "int_debt",
          "int_totdebt",
          "invt_act",
          "rect_act",
          "debt_ebitda",
          "curr_debt",
          "profit_lct",
          "ocf_lct",
          "fcf_ocf",
          "dltt_be",
          "debt_capital",
          "intcov",
          "intcov_ratio",
          "cash_ratio",
          "quick_ratio",
          "curr_ratio",
          "cash_conversion",
          "at_turn",
          "inv_turn",
          "pay_turn",
          "rect_turn",
          "sale_invcap",
          "sale_nwc",
          "rd_sale",
          "adv_sale"
        ],
        "n_pass": 19,
        "total": 66
      }
    },
    "coverage_univ_r3000p": {
      "composites": {
        "fails": [
          "piotroski_f",
          "altman_z",
          "ohlson_o",
          "beneish_m"
        ],
        "n_pass": 0,
        "total": 4
      },
      "extensions": {
        "fails": [
          "divyield_paid"
        ],
        "n_pass": 2,
        "total": 3
      },
      "primary": {
        "fails": [
          "capei",
          "bm",
          "evm",
          "pe_exi",
          "pe_inc",
          "ps",
          "pcf",
          "dpr",
          "peg_trailing",
          "divyield",
          "npm",
          "opmbd",
          "opmad",
          "gpm",
          "ptpm",
          "cfm",
          "roa",
          "roe",
          "roce",
          "efftax",
          "aftret_invcapx",
          "pretret_noa",
          "pretret_earnat",
          "gprof",
          "equity_invcap",
          "debt_invcap",
          "totdebt_invcap",
          "int_debt",
          "int_totdebt",
          "invt_act",
          "rect_act",
          "debt_ebitda",
          "curr_debt",
          "profit_lct",
          "ocf_lct",
          "cash_debt",
          "fcf_ocf",
          "lt_ppent",
          "dltt_be",
          "debt_capital",
          "intcov",
          "intcov_ratio",
          "cash_ratio",
          "quick_ratio",
          "curr_ratio",
          "cash_conversion",
          "at_turn",
          "inv_turn",
          "pay_turn",
          "rect_turn",
          "sale_equity",
          "sale_invcap",
          "sale_nwc",
          "rd_sale",
          "adv_sale"
        ],
        "n_pass": 11,
        "total": 66
      }
    }
  },
  "handcheck": {
    "issuers": 20,
    "pass": false,
    "rows": 40,
    "rule": "``n`` seeded linked member issuers (USD 10-K filers, issuer-primary line, no split between the 10-K and the\n    date), two month-end dates each: the first month-end on which each of the issuer's earliest and latest 10-Ks\n    2021-2025 is the state's lag-0 row (fiscal-year TTM path). For each, fetch the 10-K's primary document\n    (``sec_http``, <= 5 req/s), read its inline XBRL facts and recompute ``HAND_RATIOS`` from the filing's values\n    (``hand_items``) and the row's market inputs (me, prc, eps_adj); match = relative difference <= 1e-6.",
    "summary": {
      "absent_in_filing": 297,
      "absent_in_ratios": 1,
      "both_absent": 143,
      "match": 999
    }
  },
  "supplementary_counts": {
    "match": 59,
    "source_absent": 15,
    "both_absent": 6
  },
  "one_sided_evidence": "t4-r1-hand-attribution.json: keyed source primitive absence vs retained state; narrow extraction absence does not establish upstream corruption",
  "receipt_summary": {
    "build": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.6231,
      "run_seconds": 146.1
    },
    "validate": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.5433,
      "run_seconds": 19.0
    },
    "handcheck": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.2848,
      "run_seconds": 25.0
    },
    "extra-sourcecheck": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.082,
      "run_seconds": 1.0
    },
    "final-build": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.6282,
      "run_seconds": 135.1
    },
    "final-handcheck": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.2876,
      "run_seconds": 29.0
    },
    "attribution-2": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.1756,
      "run_seconds": 5.0
    },
    "source-cases": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.1577,
      "run_seconds": 2.0
    },
    "final-attribution": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.1753,
      "run_seconds": 5.0
    }
  }
}
```

Remaining one-sided ROE case: CIK1817358, accession0001817358-22-000039, FY2022-01-29, session2022-03-31. Filing prior TXDITC138.703m is present; eligible prior annual-state TXDITC is NULL. This is an unavailable retained/comparable annual-history path, not a demonstrated arithmetic defect or proof that the issuer primitive is absent. Task5 must reconcile source-history/period certification; no current-only average or tax plug is introduced.297 narrow-chain source absences remain uncertified. Final main arithmetic:999matches,143both-absent,297source-absent,1ratio-absent,0numerical mismatches; supplementary80cells:59matches,6both-absent,15source-absent.
