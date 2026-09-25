# Research anomaly catalog

Generated from `atx-db/src/atx_db/seeds/research_anomaly_catalog.csv` by
`atx_db.research.catalog.render_anomaly_catalog_markdown()`; `tests/test_research_catalog.py` fails when this file is stale.
Edit the CSV (and `EXCLUDED_SEED_METRICS`), never this file by hand.

Every row is a pre-registered hypothesis: `expected_sign` +1 means a higher value predicts higher 1-12 month forward returns. Evaluation tests the sign; it never chooses it. `prior_evidence`: `published_anomaly` (the metric, or its standard construction, is a published anomaly with this sign), `published_analogue` (a close published relative; the sign is carried over), `economic_conjecture` (the sign is argued, not published). Qualification should treat conjectures as exploratory.

Admission: `eligible`; `eligible_with_caveat` (a known construction hazard, noted per row); `blocked_incomparable_origin` (the derived engine labels every quarterly value `value_origin='incomparable'`, which the research gate rejects; not testable until the engine can prove comparability).

Clocks are inherited, never declared freely: `conservative_filing_46h` = SEC filing date + 46h (sec_filed_date_plus_46h_v1); modeled, not measured delivery; `modeled_trade_date_22h` = bar trade_date + 22h; modeled end-of-day availability of the daily bar; `max_filing_46h_trade_date_22h` = latest of the filing clock of every fundamental input and the bar clock. Minimum history counts fiscal quarters (`q`) and daily bars (`s`) needed for one value, derived from the metric's expression and its dependencies.

Compositions are market-scaled ratios declared here and computed at formation by the feature store (R2b), clock = latest input clock.

## Class counts

| class | role | rows | research-eligible |
|---|---|---:|---:|
| value | anomaly | 12 | 12 |
| profitability | anomaly | 15 | 15 |
| quality | anomaly | 6 | 6 |
| growth | anomaly | 46 | 42 |
| investment | anomaly | 14 | 14 |
| accruals | anomaly | 5 | 5 |
| leverage | anomaly | 10 | 10 |
| payout_issuance | anomaly | 10 | 10 |
| efficiency | anomaly | 6 | 6 |
| earnings_stability | anomaly | 1 | 0 |
| size | control | 2 | 2 |
| momentum | control | 4 | 4 |
| reversal | control | 1 | 1 |
| volatility | control | 2 | 2 |
| **all** | | **134** | **129** |

## value

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `earnings_yield` | `earnings_yield` (daily) | +1 | Trailing twelve-month earnings available to common over market capitalization (E/P). | Basu 1977 (Journal of Finance); Fama and French 1992 (Journal of Finance) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `book_to_market` | `book_to_market` (daily) | +1 | Latest common book equity over market capitalization (B/M). | Rosenberg Reid and Lanstein 1985 (Journal of Portfolio Management); Fama and French 1992 (Journal of Finance) | published_anomaly | rank_normal | max_filing_46h_trade_date_22h | 1q/1s | eligible_with_caveat: Negative common equity gives a negative ratio; Fama-French drop non-positive book equity. |
| `fcf_yield` | `fcf_yield` (daily) | +1 | Trailing twelve-month free cash flow over market capitalization. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Hou Karolyi and Kho 2011 (Review of Financial Studies) | published_analogue | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `dividend_yield` | `dividend_yield` (daily) | +1 | Trailing twelve-month common dividends over market capitalization (D/P). | Litzenberger and Ramaswamy 1979 (Journal of Financial Economics); Naranjo Nimalendran and Ryngaert 1998 (Journal of Finance) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `rd_to_market_equity` | `rd_to_market_equity` (daily) | +1 | Trailing twelve-month research and development expense over market capitalization. | Chan Lakonishok and Sougiannis 2001 (Journal of Finance) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `gross_profit_to_ev` | `gross_profit_to_ev` (daily) | +1 | Trailing twelve-month gross profit over enterprise value. | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis); Novy-Marx 2013 (Journal of Financial Economics) | published_analogue | rank_normal | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat: Enterprise value is non-positive for net-cash firms and then flips the yield sign. |
| `cfo_to_ev` | `cfo_to_ev` (daily) | +1 | Trailing twelve-month operating cash flow over enterprise value. | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis); Lakonishok Shleifer and Vishny 1994 (Journal of Finance) | published_analogue | rank_normal | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat: Enterprise value is non-positive for net-cash firms and then flips the yield sign. |
| `ebit_to_ev` | `ebit_to_ev` (daily) | +1 | Trailing twelve-month operating income over enterprise value. | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis) | published_analogue | rank_normal | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat: Enterprise value is non-positive for net-cash firms and then flips the yield sign. |
| `sales_to_ev` | `sales_to_ev` (daily) | +1 | Trailing twelve-month revenue over enterprise value. | Barbee Mukherji and Raines 1996 (Financial Analysts Journal); Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis) | published_analogue | rank_normal | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat: Enterprise value is non-positive for net-cash firms and then flips the yield sign. |
| `sales_to_price` | `metric:revenue_ttm` / `metric:market_cap` | +1 | Trailing twelve-month revenue over market capitalization (S/P); the monotone inverse of ps_ttm. | Barbee Mukherji and Raines 1996 (Financial Analysts Journal); Lakonishok Shleifer and Vishny 1994 (Journal of Finance) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `cfo_to_price` | `metric:cfo_ttm` / `metric:market_cap` | +1 | Trailing twelve-month operating cash flow over market capitalization (C/P); the monotone inverse of pcf_ttm. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `ebitda_to_ev` | `metric:ebitda_ttm` / `metric:enterprise_value` | +1 | Trailing twelve-month EBITDA over enterprise value; the monotone inverse of ev_ebitda (the enterprise multiple). | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis) | published_anomaly | rank_normal | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat: Enterprise value is non-positive for net-cash firms and then flips the yield sign. |

## profitability

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `gross_margin` | `gross_margin` (ttm) | +1 | Trailing twelve-month gross profit over trailing revenue. | Novy-Marx 2013 (Journal of Financial Economics); Fama and French 2015 (Journal of Financial Economics) | published_analogue | winsor_z | conservative_filing_46h | 4q/0s | eligible |
| `operating_margin` | `operating_margin` (ttm) | +1 | Trailing twelve-month operating income over trailing revenue. | Fama and French 2015 (Journal of Financial Economics); Ball Gerakos Linnainmaa and Nikolaev 2015 (Journal of Financial Economics) | published_analogue | winsor_z | conservative_filing_46h | 4q/0s | eligible |
| `net_margin` | `net_margin` (ttm) | +1 | Trailing twelve-month net income over trailing revenue. | Haugen and Baker 1996 (Journal of Financial Economics); Fama and French 2015 (Journal of Financial Economics) | published_analogue | winsor_z | conservative_filing_46h | 4q/0s | eligible |
| `ebitda_margin` | `ebitda_margin` (ttm) | +1 | Trailing twelve-month EBITDA over trailing revenue. | Fama and French 2015 (Journal of Financial Economics); Ball Gerakos Linnainmaa and Nikolaev 2015 (Journal of Financial Economics) | published_analogue | winsor_z | conservative_filing_46h | 4q/0s | eligible |
| `gross_margin_q` | `gross_margin_q` (q) | +1 | Single-quarter gross profit over single-quarter revenue. | Novy-Marx 2013 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | winsor_z | conservative_filing_46h | 1q/0s | eligible_with_caveat: A single-quarter level carries fiscal seasonality. |
| `operating_margin_q` | `operating_margin_q` (q) | +1 | Single-quarter operating income over single-quarter revenue. | Fama and French 2015 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | winsor_z | conservative_filing_46h | 1q/0s | eligible_with_caveat: A single-quarter level carries fiscal seasonality. |
| `net_margin_q` | `net_margin_q` (q) | +1 | Single-quarter net income over single-quarter revenue. | Haugen and Baker 1996 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | winsor_z | conservative_filing_46h | 1q/0s | eligible_with_caveat: A single-quarter level carries fiscal seasonality. |
| `roa` | `roa` (ttm) | +1 | Trailing twelve-month net income over average total assets. | Haugen and Baker 1996 (Journal of Financial Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `roe` | `roe` (ttm) | +1 | Trailing twelve-month net income to common over average common equity. | Hou Xue and Zhang 2015 (Review of Financial Studies); Haugen and Baker 1996 (Journal of Financial Economics) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible_with_caveat: Negative average common equity inverts the ratio so a loss can read as a positive return. |
| `roic` | `roic` (ttm) | +1 | Trailing twelve-month NOPAT over average invested capital net of cash. | Fama and French 2015 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible_with_caveat: Invested capital net of cash can be non-positive for cash-rich firms and then flips the sign. |
| `roic_ex_goodwill` | `roic_ex_goodwill` (ttm) | +1 | Trailing twelve-month NOPAT over average invested capital net of cash and goodwill. | Fama and French 2015 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible_with_caveat: Invested capital net of cash and goodwill can be non-positive and then flips the sign. |
| `gross_profitability` | `gross_profitability` (ttm) | +1 | Trailing twelve-month gross profit over average total assets. | Novy-Marx 2013 (Journal of Financial Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `operating_profitability` | `operating_profitability` (ttm) | +1 | Trailing twelve-month operating income over average total assets. | Ball Gerakos Linnainmaa and Nikolaev 2015 (Journal of Financial Economics); Fama and French 2015 (Journal of Financial Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `cash_profitability` | `cash_profitability` (ttm) | +1 | Trailing operating income plus depreciation less working-capital accrual changes over average total assets. | Ball Gerakos Linnainmaa and Nikolaev 2016 (Journal of Financial Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `cfo_to_assets` | `cfo_to_assets` (ttm) | +1 | Trailing twelve-month operating cash flow over average total assets. | Ball Gerakos Linnainmaa and Nikolaev 2016 (Journal of Financial Economics); Piotroski 2000 (Journal of Accounting Research) | published_analogue | winsor_z | conservative_filing_46h | 5q/0s | eligible |

## quality

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `piotroski_f` | `piotroski_f` (ttm) | +1 | Nine binary signals of profitability and its change and of liquidity and leverage and issuance and efficiency (0 to 9). | Piotroski 2000 (Journal of Accounting Research) | published_anomaly | winsor_z | conservative_filing_46h | 9q/0s | eligible |
| `altman_z_book` | `altman_z_book` (ttm) | +1 | Altman Z-score with book equity in the equity-to-liabilities term. | Altman 1968 (Journal of Finance); Dichev 1998 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 4q/0s | eligible |
| `altman_z` | `altman_z` (daily) | +1 | Altman Z-score with market equity in the equity-to-liabilities term. | Dichev 1998 (Journal of Finance); Altman 1968 (Journal of Finance) | published_anomaly | rank_normal | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `beneish_m` | `beneish_m` (ttm) | -1 | Beneish eight-variable earnings-manipulation M-score. | Beneish 1999 (Financial Analysts Journal); Beneish Lee and Nichols 2013 (Financial Analysts Journal) | published_anomaly | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `ohlson_o` | `ohlson_o` (ttm) | -1 | Ohlson O-score bankruptcy index (no GNP deflator). | Ohlson 1980 (Journal of Accounting Research); Dichev 1998 (Journal of Finance); Griffin and Lemmon 2002 (Journal of Finance) | published_anomaly | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `tax_to_book_income` | `tax_to_book_income` (ttm) | +1 | Trailing twelve-month income tax expense over trailing net income. | Lev and Nissim 2004 (The Accounting Review) | published_analogue | rank_normal | conservative_filing_46h | 4q/0s | eligible_with_caveat: Negative net income inverts the ratio. |

## growth

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `revenue_growth_yoy` | `revenue_growth_yoy` (ttm) | -1 | Year-over-year growth of trailing twelve-month revenue. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `revenue_cagr_1y` | `revenue_cagr_1y` (ttm) | -1 | One-year compound growth of trailing revenue with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `revenue_cagr_3y` | `revenue_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing revenue with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 16q/0s | eligible |
| `gross_profit_growth_yoy` | `gross_profit_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month gross profit. | Novy-Marx 2015 (NBER Working Paper 20984); Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `operating_income_growth_yoy` | `operating_income_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month operating income. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance); Novy-Marx 2015 (NBER Working Paper 20984) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `net_income_growth_yoy` | `net_income_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month net income over the absolute prior base. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance); Foster Olsen and Shevlin 1984 (The Accounting Review) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `eps_diluted_growth_yoy` | `eps_diluted_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month diluted EPS over the absolute prior base. | Bernard and Thomas 1989 (Journal of Accounting Research); Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `cfo_growth_yoy` | `cfo_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month operating cash flow. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `fcf_growth_yoy` | `fcf_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month free cash flow. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `tax_expense_change_yoy` | `tax_expense_change_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month income tax expense. | Thomas and Zhang 2011 (Journal of Accounting Research) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `revenue_growth_qoq` | `revenue_growth_qoq` (ttm) | +1 | Quarter-over-quarter growth of trailing twelve-month revenue. | Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | economic_conjecture | rank_normal | conservative_filing_46h | 5q/0s | blocked_incomparable_origin: A one-quarter change of a trailing sum compares overlapping 365-day spans; the engine labels every value incomparable. |
| `eps_diluted_growth_qoq` | `eps_diluted_growth_qoq` (ttm) | +1 | Quarter-over-quarter growth of trailing twelve-month diluted EPS. | Bernard and Thomas 1989 (Journal of Accounting Research) | economic_conjecture | rank_normal | conservative_filing_46h | 5q/0s | blocked_incomparable_origin: Per-share one-quarter pair over overlapping trailing spans without a split guard; every value is labeled incomparable. |
| `eps_cagr_3y` | `eps_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing diluted EPS with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 16q/0s | eligible |
| `cfo_cagr_3y` | `cfo_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing operating cash flow with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 16q/0s | eligible |
| `gross_profit_cagr_3y` | `gross_profit_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing gross profit with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Daniel and Titman 2006 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 16q/0s | eligible |
| `operating_income_cagr_3y` | `operating_income_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing operating income with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 16q/0s | eligible |
| `ebitda_cagr_3y` | `ebitda_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing EBITDA with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 16q/0s | eligible |
| `fcf_cagr_3y` | `fcf_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing free cash flow with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 16q/0s | eligible |
| `gross_margin_change_yoy` | `gross_margin_change_yoy` (ttm) | +1 | Year-over-year change in trailing gross margin in fraction points. | Abarbanell and Bushee 1998 (The Accounting Review); Piotroski 2000 (Journal of Accounting Research) | published_anomaly | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `operating_margin_change_yoy` | `operating_margin_change_yoy` (ttm) | +1 | Year-over-year change in trailing operating margin in fraction points. | Abarbanell and Bushee 1998 (The Accounting Review); Akbas Jiang and Koch 2017 (The Accounting Review) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `net_margin_change_yoy` | `net_margin_change_yoy` (ttm) | +1 | Year-over-year change in trailing net margin in fraction points. | Soliman 2008 (The Accounting Review); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `operating_profitability_change_yoy` | `operating_profitability_change_yoy` (ttm) | +1 | Year-over-year change in operating income over average assets. | Akbas Jiang and Koch 2017 (The Accounting Review); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_analogue | rank_normal | conservative_filing_46h | 9q/0s | eligible |
| `roe_change_yoy` | `roe_change_yoy` (ttm) | +1 | Year-over-year change in trailing return on average common equity. | Hou Xue and Zhang 2020 (Review of Financial Studies); Hou Mo Xue and Zhang 2021 (Review of Finance) | published_analogue | rank_normal | conservative_filing_46h | 9q/0s | eligible_with_caveat: Inherits roe: negative average common equity inverts the ratio. |
| `gross_margin_q_change_yoy` | `gross_margin_q_change_yoy` (q) | +1 | Single-quarter gross margin less the same fiscal quarter's margin a year earlier. | Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `operating_margin_q_change_yoy` | `operating_margin_q_change_yoy` (q) | +1 | Single-quarter operating margin less the same fiscal quarter's margin a year earlier. | Abarbanell and Bushee 1998 (The Accounting Review); Akbas Jiang and Koch 2017 (The Accounting Review) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `net_margin_q_change_yoy` | `net_margin_q_change_yoy` (q) | +1 | Single-quarter net margin less the same fiscal quarter's margin a year earlier. | Soliman 2008 (The Accounting Review); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `revenue_q_growth_yoy` | `revenue_q_growth_yoy` (q) | +1 | Single-quarter revenue growth over the same fiscal quarter a year earlier. | Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `gross_profit_q_growth_yoy` | `gross_profit_q_growth_yoy` (q) | +1 | Single-quarter gross-profit growth over the same fiscal quarter a year earlier. | Novy-Marx 2015 (NBER Working Paper 20984); Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | economic_conjecture | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `operating_income_q_growth_yoy` | `operating_income_q_growth_yoy` (q) | +1 | Single-quarter operating-income growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `net_income_q_growth_yoy` | `net_income_q_growth_yoy` (q) | +1 | Single-quarter net-income growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `eps_diluted_q_growth_yoy` | `eps_diluted_q_growth_yoy` (q) | +1 | Single-quarter diluted-EPS growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `eps_basic_q_growth_yoy` | `eps_basic_q_growth_yoy` (q) | +1 | Single-quarter basic-EPS growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `cfo_q_growth_yoy` | `cfo_q_growth_yoy` (q) | +1 | Single-quarter operating-cash-flow growth over the same fiscal quarter a year earlier. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `fcf_q_growth_yoy` | `fcf_q_growth_yoy` (q) | +1 | Single-quarter free-cash-flow growth over the same fiscal quarter a year earlier. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `revenue_q_growth_qoq` | `revenue_q_growth_qoq` (q) | +1 | Single-quarter revenue growth over the immediately preceding fiscal quarter. | Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `gross_profit_q_growth_qoq` | `gross_profit_q_growth_qoq` (q) | +1 | Single-quarter gross-profit growth over the immediately preceding fiscal quarter. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `operating_income_q_growth_qoq` | `operating_income_q_growth_qoq` (q) | +1 | Single-quarter operating-income growth over the immediately preceding fiscal quarter. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `net_income_q_growth_qoq` | `net_income_q_growth_qoq` (q) | +1 | Single-quarter net-income growth over the immediately preceding fiscal quarter. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `cfo_q_growth_qoq` | `cfo_q_growth_qoq` (q) | +1 | Single-quarter operating-cash-flow growth over the immediately preceding fiscal quarter. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `fcf_q_growth_qoq` | `fcf_q_growth_qoq` (q) | +1 | Single-quarter free-cash-flow growth over the immediately preceding fiscal quarter. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `eps_diluted_q_growth_qoq` | `eps_diluted_q_growth_qoq` (q) | +1 | Single-quarter diluted-EPS growth over the immediately preceding fiscal quarter. | Bernard and Thomas 1989 (Journal of Accounting Research) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | blocked_incomparable_origin: Per-share one-quarter pair: the prior quarter is as first reported and not split-adjusted; incomparable until a split guard exists (A4 ruling). |
| `eps_basic_q_growth_qoq` | `eps_basic_q_growth_qoq` (q) | +1 | Single-quarter basic-EPS growth over the immediately preceding fiscal quarter. | Bernard and Thomas 1989 (Journal of Accounting Research) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | blocked_incomparable_origin: Per-share one-quarter pair: the prior quarter is as first reported and not split-adjusted; incomparable until a split guard exists (A4 ruling). |
| `eps_diluted_q_growth_yoy_accel` | `eps_diluted_q_growth_yoy_accel` (q) | +1 | This quarter's seasonal diluted-EPS growth less the previous quarter's seasonal growth. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics) | published_anomaly | rank_normal | conservative_filing_46h | 6q/0s | eligible |
| `revenue_q_growth_yoy_accel` | `revenue_q_growth_yoy_accel` (q) | +1 | This quarter's seasonal revenue growth less the previous quarter's seasonal growth. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics); Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | published_analogue | rank_normal | conservative_filing_46h | 6q/0s | eligible |
| `gross_margin_q_change_yoy_accel` | `gross_margin_q_change_yoy_accel` (q) | +1 | This quarter's seasonal gross-margin change less the previous quarter's seasonal change. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics); Abarbanell and Bushee 1998 (The Accounting Review) | economic_conjecture | rank_normal | conservative_filing_46h | 6q/0s | eligible |
| `operating_margin_q_change_yoy_accel` | `operating_margin_q_change_yoy_accel` (q) | +1 | This quarter's seasonal operating-margin change less the previous quarter's seasonal change. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics); Abarbanell and Bushee 1998 (The Accounting Review) | economic_conjecture | rank_normal | conservative_filing_46h | 6q/0s | eligible |

## investment

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `asset_growth` | `asset_growth` (q) | -1 | Year-over-year growth of total assets. | Cooper Gulen and Schill 2008 (Journal of Finance); Fama and French 2015 (Journal of Financial Economics) | published_anomaly | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `total_assets_cagr_3y` | `total_assets_cagr_3y` (q) | -1 | Three-year compound annual growth of total assets with positive endpoints required. | Cooper Gulen and Schill 2008 (Journal of Finance); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | rank_normal | conservative_filing_46h | 13q/0s | eligible |
| `book_value_growth_yoy` | `book_value_growth_yoy` (q) | -1 | Year-over-year growth of common book equity. | Cooper Gulen and Schill 2008 (Journal of Finance); Fama and French 2015 (Journal of Financial Economics) | economic_conjecture | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `common_equity_cagr_3y` | `common_equity_cagr_3y` (q) | -1 | Three-year compound annual growth of common equity with positive endpoints required. | Cooper Gulen and Schill 2008 (Journal of Finance); Fama and French 2015 (Journal of Financial Economics) | economic_conjecture | rank_normal | conservative_filing_46h | 13q/0s | eligible |
| `delta_noa` | `delta_noa` (q) | -1 | Year-over-year change in net operating assets over lagged total assets. | Hirshleifer Hou Teoh and Zhang 2004 (Journal of Accounting and Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `capex_growth_yoy` | `capex_growth_yoy` (ttm) | -1 | Year-over-year growth of trailing twelve-month capital expenditure. | Anderson and Garcia-Feijoo 2006 (Journal of Finance); Xing 2008 (Review of Financial Studies) | published_anomaly | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `capex_q_growth_yoy` | `capex_q_growth_yoy` (q) | -1 | Single-quarter capital-expenditure growth over the same fiscal quarter a year earlier. | Anderson and Garcia-Feijoo 2006 (Journal of Finance); Xing 2008 (Review of Financial Studies) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `capex_q_growth_qoq` | `capex_q_growth_qoq` (q) | -1 | Single-quarter capital-expenditure growth over the immediately preceding fiscal quarter. | Anderson and Garcia-Feijoo 2006 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `capex_to_depreciation` | `capex_to_depreciation` (ttm) | -1 | Trailing capital expenditure over trailing depreciation and amortization. | Titman Wei and Xie 2004 (Journal of Financial and Quantitative Analysis) | published_analogue | rank_normal | conservative_filing_46h | 4q/0s | eligible |
| `capex_to_sales` | `capex_to_sales` (ttm) | -1 | Trailing capital expenditure over trailing revenue. | Titman Wei and Xie 2004 (Journal of Financial and Quantitative Analysis) | published_analogue | winsor_z | conservative_filing_46h | 4q/0s | eligible |
| `rd_intensity_sales` | `rd_intensity_sales` (ttm) | +1 | Trailing research and development expense over trailing revenue. | Chan Lakonishok and Sougiannis 2001 (Journal of Finance); Lev and Sougiannis 1996 (Journal of Accounting and Economics) | economic_conjecture | rank_normal | conservative_filing_46h | 4q/0s | eligible |
| `rd_expense_growth_yoy` | `rd_expense_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month research and development expense. | Eberhart Maxwell and Siddique 2004 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 8q/0s | eligible |
| `rd_expense_q_growth_yoy` | `rd_expense_q_growth_yoy` (q) | +1 | Single-quarter R&D expense growth over the same fiscal quarter a year earlier. | Eberhart Maxwell and Siddique 2004 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `rd_expense_q_growth_qoq` | `rd_expense_q_growth_qoq` (q) | +1 | Single-quarter R&D expense growth over the immediately preceding fiscal quarter. | Eberhart Maxwell and Siddique 2004 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 2q/0s | eligible_with_caveat: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |

## accruals

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `total_accruals` | `total_accruals` (ttm) | -1 | Trailing net income less operating cash flow over average total assets. | Sloan 1996 (The Accounting Review) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `percent_accruals` | `percent_accruals` (ttm) | -1 | Trailing net income less operating cash flow over absolute trailing net income. | Hafzalla Lundholm and Van Winkle 2011 (The Accounting Review) | published_anomaly | rank_normal | conservative_filing_46h | 4q/0s | eligible |
| `working_capital_accruals` | `working_capital_accruals` (ttm) | -1 | Year-over-year change in operating working capital over average total assets. | Sloan 1996 (The Accounting Review) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `rsst_accruals` | `rsst_accruals` (ttm) | -1 | Change in net operating and long-term investment assets over average total assets (broad accruals). | Richardson Sloan Soliman and Tuna 2005 (Journal of Accounting and Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `noa_to_assets` | `noa_to_assets` (q) | -1 | Net operating assets over lagged total assets (cumulative accruals). | Hirshleifer Hou Teoh and Zhang 2004 (Journal of Accounting and Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |

## leverage

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `debt_to_equity` | `debt_to_equity` (q) | -1 | Interest-bearing debt over stockholders equity (book leverage). | George and Hwang 2010 (Journal of Financial Economics); Penman Richardson and Tuna 2007 (Journal of Accounting Research) | published_analogue | rank_normal | conservative_filing_46h | 1q/0s | eligible_with_caveat: Negative stockholders equity inverts the ratio so the most levered firms read as unlevered. |
| `debt_to_assets` | `debt_to_assets` (q) | -1 | Interest-bearing debt over total assets. | George and Hwang 2010 (Journal of Financial Economics); Penman Richardson and Tuna 2007 (Journal of Accounting Research) | published_analogue | winsor_z | conservative_filing_46h | 1q/0s | eligible |
| `long_term_debt_to_assets` | `long_term_debt_to_assets` (q) | -1 | Long-term debt over total assets. | George and Hwang 2010 (Journal of Financial Economics) | published_analogue | winsor_z | conservative_filing_46h | 1q/0s | eligible |
| `net_debt_ebitda` | `net_debt_ebitda` (ttm) | -1 | Debt net of cash over trailing EBITDA. | George and Hwang 2010 (Journal of Financial Economics); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | economic_conjecture | rank_normal | conservative_filing_46h | 4q/0s | eligible_with_caveat: Non-positive trailing EBITDA inverts the ratio. |
| `interest_coverage` | `interest_coverage` (ttm) | +1 | Trailing operating income over absolute trailing interest expense. | Campbell Hilscher and Szilagyi 2008 (Journal of Finance); Dichev 1998 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 4q/0s | eligible |
| `current_ratio` | `current_ratio` (q) | +1 | Current assets over current liabilities. | Piotroski 2000 (Journal of Accounting Research); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 1q/0s | eligible |
| `quick_ratio` | `quick_ratio` (q) | +1 | Current assets less inventory over current liabilities. | Piotroski 2000 (Journal of Accounting Research); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 1q/0s | eligible |
| `cash_ratio` | `cash_ratio` (q) | +1 | Cash and short-term investments over current liabilities. | Palazzo 2012 (Journal of Financial Economics); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | published_analogue | rank_normal | conservative_filing_46h | 1q/0s | eligible |
| `debt_to_market` | `metric:total_debt_q` / `metric:market_cap` | +1 | Interest-bearing debt over market capitalization (market leverage). | Bhandari 1988 (Journal of Finance); Fama and French 1992 (Journal of Finance) | published_anomaly | rank_normal | max_filing_46h_trade_date_22h | 1q/1s | eligible |
| `assets_to_market` | `item:total_assets` / `metric:market_cap` | +1 | Total assets over market capitalization (A/ME market leverage). | Fama and French 1992 (Journal of Finance) | published_anomaly | rank_normal | max_filing_46h_trade_date_22h | 1q/1s | eligible |

## payout_issuance

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `net_equity_issuance` | `net_equity_issuance` (ttm) | -1 | Trailing equity issued less repurchased over average total assets. | Pontiff and Woodgate 2008 (Journal of Finance); Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `net_debt_issuance` | `net_debt_issuance` (ttm) | -1 | Trailing long-term debt issued less repaid over average total assets. | Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics); Spiess and Affleck-Graves 1999 (Journal of Financial Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `external_financing` | `external_financing` (ttm) | -1 | Net equity plus net debt financing over average total assets. | Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `shares_growth_yoy` | `shares_growth_yoy` (q) | -1 | Year-over-year growth of period-end shares outstanding. | Pontiff and Woodgate 2008 (Journal of Finance); Daniel and Titman 2006 (Journal of Finance) | published_anomaly | rank_normal | conservative_filing_46h | 5q/0s | eligible_with_caveat: Period-end share counts are as reported: a split between the two period ends reads as issuance until a split-adjusted share basis exists. |
| `buyback_ratio` | `buyback_ratio` (ttm) | +1 | Trailing repurchases less issuance over average total assets. | Ikenberry Lakonishok and Vermaelen 1995 (Journal of Financial Economics); Pontiff and Woodgate 2008 (Journal of Finance) | published_analogue | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `payout_ratio` | `payout_ratio` (ttm) | +1 | Trailing common dividends over trailing earnings available to common. | Arnott and Asness 2003 (Financial Analysts Journal) | economic_conjecture | rank_normal | conservative_filing_46h | 4q/0s | eligible_with_caveat: Negative earnings invert the ratio. |
| `buyback_yield` | `buyback_yield` (daily) | +1 | Trailing repurchases less issuance over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance); Ikenberry Lakonishok and Vermaelen 1995 (Journal of Financial Economics) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `net_payout_yield` | `net_payout_yield` (daily) | +1 | Trailing dividends plus repurchases less issuance over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `total_payout_yield` | `total_payout_yield` (daily) | +1 | Trailing gross dividends plus gross repurchases over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance) | published_anomaly | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `shareholder_yield` | `shareholder_yield` (daily) | +1 | Trailing net payout plus net debt paydown over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance); Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics) | published_analogue | winsor_z | max_filing_46h_trade_date_22h | 4q/1s | eligible |

## efficiency

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `asset_turnover` | `asset_turnover` (ttm) | +1 | Trailing revenue over average total assets. | Soliman 2008 (The Accounting Review); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | winsor_z | conservative_filing_46h | 5q/0s | eligible |
| `asset_turnover_change_yoy` | `asset_turnover_change_yoy` (ttm) | +1 | Year-over-year change in revenue over average total assets. | Soliman 2008 (The Accounting Review) | published_anomaly | winsor_z | conservative_filing_46h | 9q/0s | eligible |
| `dso_days` | `dso_days` (ttm) | -1 | Days sales outstanding: average receivables times 365 over trailing revenue. | Wang 2019 (Journal of Financial Economics) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `dio_days` | `dio_days` (ttm) | -1 | Days inventory outstanding: average inventory times 365 over trailing cost of revenue. | Wang 2019 (Journal of Financial Economics); Thomas and Zhang 2002 (Review of Accounting Studies) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `dpo_days` | `dpo_days` (ttm) | +1 | Days payables outstanding: average payables times 365 over trailing cost of revenue. | Wang 2019 (Journal of Financial Economics) | published_analogue | rank_normal | conservative_filing_46h | 5q/0s | eligible |
| `cash_conversion_cycle` | `cash_conversion_cycle` (ttm) | -1 | DSO plus DIO less DPO in days. | Wang 2019 (Journal of Financial Economics) | published_anomaly | rank_normal | conservative_filing_46h | 5q/0s | eligible |

## earnings_stability

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `earnings_variability` | `earnings_variability` (ttm) | -1 | Twelve-quarter standard deviation of year-over-year trailing diluted-EPS growth. | Huang 2009 (Journal of Empirical Finance); Dichev and Tang 2009 (Journal of Accounting and Economics) | published_analogue | rank_normal | conservative_filing_46h | 19q/0s | blocked_incomparable_origin: _derived_annual.lower_span marks every stdev_q span incoherent so every value is labeled incomparable. |

## size

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `market_cap` | `market_cap` (daily) | -1 | Price times point-in-time shares outstanding. | Banz 1981 (Journal of Financial Economics); Fama and French 1992 (Journal of Finance) | published_anomaly | log_winsor_z | max_filing_46h_trade_date_22h | 0q/1s | eligible |
| `dollar_volume_20d` | `dollar_volume_20d` (daily) | -1 | Twenty-day average daily dollar trading volume. | Brennan Chordia and Subrahmanyam 1998 (Journal of Financial Economics); Amihud 2002 (Journal of Financial Markets) | published_anomaly | log_winsor_z | modeled_trade_date_22h | 0q/20s | eligible |

## momentum

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `momentum_12_1` | `momentum_12_1` (daily) | +1 | Total return from 252 to 21 trading days before formation (skips the latest month). | Jegadeesh and Titman 1993 (Journal of Finance); Carhart 1997 (Journal of Finance) | published_anomaly | winsor_z | modeled_trade_date_22h | 0q/253s | eligible |
| `total_return_12m` | `total_return_12m` (daily) | +1 | Total return over the last 252 trading days. | Jegadeesh and Titman 1993 (Journal of Finance) | published_analogue | winsor_z | modeled_trade_date_22h | 0q/253s | eligible |
| `total_return_6m` | `total_return_6m` (daily) | +1 | Total return over the last 126 trading days. | Jegadeesh and Titman 1993 (Journal of Finance) | published_anomaly | winsor_z | modeled_trade_date_22h | 0q/127s | eligible |
| `total_return_3m` | `total_return_3m` (daily) | +1 | Total return over the last 63 trading days. | Jegadeesh and Titman 1993 (Journal of Finance) | published_analogue | winsor_z | modeled_trade_date_22h | 0q/64s | eligible |

## reversal

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `total_return_1m` | `total_return_1m` (daily) | -1 | Total return over the last 21 trading days. | Jegadeesh 1990 (Journal of Finance); Lehmann 1990 (Quarterly Journal of Economics) | published_anomaly | winsor_z | modeled_trade_date_22h | 0q/22s | eligible |

## volatility

| feature | source | sign | definition | reference | evidence | transform | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|
| `realized_vol_60d` | `realized_vol_60d` (daily) | -1 | Annualized 60-day realized volatility of daily log returns. | Ang Hodrick Xing and Zhang 2006 (Journal of Finance); Baker Bradley and Wurgler 2011 (Financial Analysts Journal) | published_analogue | log_winsor_z | modeled_trade_date_22h | 0q/61s | eligible |
| `realized_vol_252d` | `realized_vol_252d` (daily) | -1 | Annualized 252-day realized volatility of daily log returns. | Baker Bradley and Wurgler 2011 (Financial Analysts Journal); Ang Hodrick Xing and Zhang 2006 (Journal of Finance) | published_analogue | log_winsor_z | modeled_trade_date_22h | 0q/253s | eligible |

## Seed metrics that are not research features

| metric | reason |
|---|---|
| `acquisitions_ttm` | dollar_level_input |
| `beneish_aqi` | component_of:beneish_m |
| `beneish_depi` | component_of:beneish_m |
| `beneish_dsri` | component_of:beneish_m |
| `beneish_gmi` | component_of:beneish_m |
| `beneish_lvgi` | component_of:beneish_m |
| `beneish_sgai` | component_of:beneish_m |
| `beneish_sgi` | component_of:beneish_m |
| `beneish_tata` | component_of:beneish_m |
| `book_per_share` | per_share_level |
| `capex_q` | dollar_level_input |
| `capex_ttm` | dollar_level_input |
| `cash_per_share` | per_share_level |
| `cash_st_investments_q` | dollar_level_input |
| `cfo_per_share` | per_share_level |
| `cfo_ttm` | dollar_level_input |
| `change_in_inventory_yoy` | dollar_level_input |
| `change_in_payables_yoy` | dollar_level_input |
| `change_in_receivables_yoy` | dollar_level_input |
| `common_dividends_ttm` | dollar_level_input |
| `common_equity_avg2` | dollar_level_input |
| `common_equity_q` | dollar_level_input |
| `cost_of_revenue_ttm` | dollar_level_input |
| `debt_issuance_ttm` | dollar_level_input |
| `debt_reduction_ttm` | dollar_level_input |
| `depreciation_ttm` | dollar_level_input |
| `dividends_paid_ttm` | dollar_level_input |
| `dividends_per_share_ttm` | per_share_level |
| `ebitda_q` | dollar_level_input |
| `ebitda_ttm` | dollar_level_input |
| `effective_tax_rate_ttm` | component_of:roic |
| `enterprise_value` | dollar_level_input |
| `eps_basic_ttm` | per_share_level |
| `eps_diluted_ttm` | per_share_level |
| `eps_ttm` | per_share_level |
| `ev_ebitda` | inverse_cataloged:ebitda_to_ev |
| `ev_sales` | inverse_cataloged:sales_to_ev |
| `fcf_per_share` | per_share_level |
| `fcf_q` | dollar_level_input |
| `fcf_ttm` | dollar_level_input |
| `gross_profit_q` | dollar_level_input |
| `gross_profit_ttm` | dollar_level_input |
| `income_tax_ttm` | dollar_level_input |
| `interest_expense_ttm` | dollar_level_input |
| `inventory_avg2` | dollar_level_input |
| `invested_capital_avg2` | dollar_level_input |
| `invested_capital_ex_goodwill_avg2` | dollar_level_input |
| `invested_capital_ex_goodwill_q` | dollar_level_input |
| `invested_capital_q` | dollar_level_input |
| `net_debt` | dollar_level_input |
| `net_income_common_ttm` | dollar_level_input |
| `net_income_ttm` | dollar_level_input |
| `noa` | dollar_level_input |
| `nopat_ttm` | dollar_level_input |
| `ohlson_chin` | component_of:ohlson_o |
| `ohlson_clca` | component_of:ohlson_o |
| `ohlson_futl` | component_of:ohlson_o |
| `ohlson_intwo` | component_of:ohlson_o |
| `ohlson_nita` | component_of:ohlson_o |
| `ohlson_oeneg` | component_of:ohlson_o |
| `ohlson_tlta` | component_of:ohlson_o |
| `ohlson_wcta` | component_of:ohlson_o |
| `operating_income_ttm` | dollar_level_input |
| `operating_working_capital_q` | dollar_level_input |
| `payables_avg2` | dollar_level_input |
| `pb` | inverse_cataloged:book_to_market |
| `pcf_ttm` | inverse_cataloged:cfo_to_price |
| `pe_ttm` | inverse_cataloged:earnings_yield |
| `pretax_income_ttm` | dollar_level_input |
| `ps_ttm` | inverse_cataloged:sales_to_price |
| `rd_expense_ttm` | dollar_level_input |
| `receivables_avg2` | dollar_level_input |
| `revenue_ttm` | dollar_level_input |
| `sales_per_share` | per_share_level |
| `sga_expense_ttm` | dollar_level_input |
| `share_issuance_ttm` | dollar_level_input |
| `share_repurchase_ttm` | dollar_level_input |
| `stock_compensation_ttm` | dollar_level_input |
| `stockholders_equity_avg2` | dollar_level_input |
| `tangible_book_value_per_share` | per_share_level |
| `total_assets_avg2` | dollar_level_input |
| `total_debt_avg2` | dollar_level_input |
| `total_debt_q` | dollar_level_input |
| `total_payout_ttm` | dollar_level_input |
