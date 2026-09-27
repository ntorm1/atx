# Research anomaly catalog

Generated from `atx-db/src/atx_db/seeds/research_anomaly_catalog.csv` by
`atx_db.research.catalog.render_anomaly_catalog_markdown()`; `tests/test_research_catalog.py` fails when this file is stale.
Edit the CSV (and `EXCLUDED_SEED_METRICS`), never this file by hand.

Every row is a pre-registered hypothesis: `expected_sign` +1 means a higher value predicts higher 1-12 month forward returns; `two-sided` pre-registers no direction where published evidence disagrees (always with caveat `mixed_evidence` and never graded `published_anomaly`). Evaluation tests the sign; it never chooses it. `prior_evidence`: `published_anomaly` (the metric, or its standard construction, is a published anomaly with this sign), `published_analogue` (a close published relative; the sign is carried over), `economic_conjecture` (the sign is argued, not published). Qualification should treat conjectures as exploratory.

`hypothesis_family` groups near-duplicates and same-construct variants (one economic hypothesis); multiple-testing and deduplication work over families, not rows.

Admission: `eligible`; `eligible_with_caveat` (a known construction hazard, coded and noted per row); `blocked_incomparable_origin` (the derived engine labels every quarterly value `value_origin='incomparable'`, which the research gate rejects; not testable until the engine can prove comparability); `blocked_known_bias` (the engine can label values comparable but a known construction bias is noted per row); `blocked_duplicate_hypothesis` (a second construction of a cataloged hypothesis, cataloged for diagnostics: `sue_ni_event` duplicates the tested `sue_ni`).

`domain` is enforced by the feature store before any transform: an out-of-domain value is excluded or carried as the named separate indicator, never ranked as valid. A rule without an operand tests the feature value itself.

Clocks are inherited, never declared freely: `conservative_filing_46h` = SEC filing date + 46h (sec_filed_date_plus_46h_v1); modeled, not measured delivery; `modeled_trade_date_22h` = bar trade_date + 22h; modeled end-of-day availability of the daily bar; `max_filing_46h_trade_date_22h` = latest of the filing clock of every fundamental input and the bar clock; `finra_publication_modeled` = FINRA settlement + 8 business days at 22:00 UTC (never before the loader's settlement + 10 days + 22h, nor before the clock of a verified share count it reads); modeled dissemination, not measured. Minimum history counts fiscal quarters (`q`) and daily bars (`s`) needed for one value, derived from the metric's expression and its dependencies.

Compositions are market-scaled ratios declared here and computed at formation by the feature store (R2b), clock = latest input clock.

Panel natives (source `(daily, panel)`) are price/liquidity features the research panel computes from the line's own daily bars on XNYS session windows (`research.panel.NATIVE_FEATURES`); their minimum history is the panel's declared one and their clock is the bar clock, or the latest input clock when a filed share count is read.

Research-store sources (source `(<window>, <kind>)`: `event` = P3 earnings events, `factor_exposure` = P4 factor exposures, `ownership` = P9 13F and FINRA features) are read by the feature store from one pinned sealed source version (`research.features.EXTERNAL_FEATURES`); their clock and minimum history are declared from each producer's rules (`EXTERNAL_FEATURE_SHAPES`).

Research metadata (see the section below): `population`, `evidence_class`, `publication_year`, `jkp_theme` and `wave` on every row.

## Class counts

| class | role | rows | research-eligible |
|---|---|---:|---:|
| value | anomaly | 12 | 12 |
| profitability | anomaly | 23 | 23 |
| quality | anomaly | 8 | 8 |
| growth | anomaly | 52 | 49 |
| investment | anomaly | 20 | 20 |
| accruals | anomaly | 5 | 5 |
| leverage | anomaly | 14 | 14 |
| payout_issuance | anomaly | 11 | 11 |
| efficiency | anomaly | 13 | 13 |
| earnings_stability | anomaly | 5 | 4 |
| liquidity | anomaly | 14 | 14 |
| ownership | anomaly | 5 | 5 |
| event_timing | anomaly | 1 | 1 |
| size | control | 4 | 4 |
| momentum | control | 14 | 14 |
| reversal | control | 5 | 5 |
| volatility | control | 18 | 18 |
| **all** | | **224** | **220** |

## value

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `earnings_yield` | `earnings_yield` (daily) | +1 | Trailing twelve-month earnings available to common over market capitalization (E/P). | Basu 1977 (Journal of Finance); Fama and French 1992 (Journal of Finance); Jaffe Keim and Westerfield 1989 (Journal of Finance) | published_anomaly | earnings_to_price | winsor_z | loss_firms_separate | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [non_monotone]: Loss firms leave the ranked domain and are carried as a separate indicator (Fama-French 1992 E(+)/P); across all firms the relation is U-shaped (Jaffe Keim and Westerfield 1989). |
| `book_to_market` | `book_to_market` (daily) | +1 | Latest common book equity over market capitalization (B/M). | Rosenberg Reid and Lanstein 1985 (Journal of Portfolio Management); Fama and French 1992 (Journal of Finance) | published_anomaly | book_to_market | rank_normal | negative_book_excluded | max_filing_46h_trade_date_22h | 1q/1s | eligible_with_caveat [sign_flip]: Negative common equity gives a negative ratio; Fama-French drop non-positive book equity. |
| `fcf_yield` | `fcf_yield` (daily) | +1 | Trailing twelve-month free cash flow over market capitalization. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Hou Karolyi and Kho 2011 (Review of Financial Studies) | published_analogue | cash_flow_to_price | winsor_z | loss_firms_separate | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [non_monotone]: Negative free cash flow leaves the ranked domain and is carried as a separate indicator. |
| `dividend_yield` | `dividend_yield` (daily) | +1 | Trailing twelve-month common dividends over market capitalization (D/P). | Litzenberger and Ramaswamy 1979 (Journal of Financial Economics); Naranjo Nimalendran and Ryngaert 1998 (Journal of Finance); Keim 1985 (Journal of Financial Economics); Christie 1990 (Journal of Financial Economics) | published_analogue | dividend_yield | winsor_z | zero_payer_separate | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [non_monotone, coverage_bias]: U-shaped: zero payers earn high returns (Keim 1985; Christie 1990) so non-payers leave the ranked domain and the sign is tested among payers; dividends are read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero so a non-payer that never tags a dividend has no value. |
| `rd_to_market_equity` | `rd_to_market_equity` (daily) | +1 | Trailing twelve-month research and development expense over market capitalization. | Chan Lakonishok and Sougiannis 2001 (Journal of Finance) | published_anomaly | rd_to_market | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `gross_profit_to_ev` | `gross_profit_to_ev` (daily) | +1 | Trailing twelve-month gross profit over enterprise value. | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis); Novy-Marx 2013 (Journal of Financial Economics) | published_analogue | enterprise_value_yield | rank_normal | positive_denominator_required:metric:enterprise_value | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [sign_flip, presence_rule, unguarded_zero, coverage_bias]: Enterprise value is non-positive for net-cash firms and then flips the yield sign; its debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and missing preferred stock and minority interest read as zero. |
| `cfo_to_ev` | `cfo_to_ev` (daily) | +1 | Trailing twelve-month operating cash flow over enterprise value. | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis); Lakonishok Shleifer and Vishny 1994 (Journal of Finance) | published_analogue | enterprise_value_yield | rank_normal | positive_denominator_required:metric:enterprise_value | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [sign_flip, presence_rule, unguarded_zero, coverage_bias]: Enterprise value is non-positive for net-cash firms and then flips the yield sign; its debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and missing preferred stock and minority interest read as zero. |
| `ebit_to_ev` | `ebit_to_ev` (daily) | +1 | Trailing twelve-month operating income over enterprise value. | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis) | published_analogue | enterprise_value_yield | rank_normal | positive_denominator_required:metric:enterprise_value | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [sign_flip, presence_rule, unguarded_zero, coverage_bias]: Enterprise value is non-positive for net-cash firms and then flips the yield sign; its debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and missing preferred stock and minority interest read as zero. |
| `sales_to_ev` | `sales_to_ev` (daily) | +1 | Trailing twelve-month revenue over enterprise value. | Barbee Mukherji and Raines 1996 (Financial Analysts Journal); Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis) | published_analogue | enterprise_value_yield | rank_normal | positive_denominator_required:metric:enterprise_value | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [sign_flip, presence_rule, unguarded_zero, coverage_bias]: Enterprise value is non-positive for net-cash firms and then flips the yield sign; its debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and missing preferred stock and minority interest read as zero. |
| `sales_to_price` | `metric:revenue_ttm` / `metric:market_cap` | +1 | Trailing twelve-month revenue over market capitalization (S/P); the monotone inverse of ps_ttm. | Barbee Mukherji and Raines 1996 (Financial Analysts Journal); Lakonishok Shleifer and Vishny 1994 (Journal of Finance) | published_anomaly | sales_to_price | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `cfo_to_price` | `metric:cfo_ttm` / `metric:market_cap` | +1 | Trailing twelve-month operating cash flow over market capitalization (C/P); the monotone inverse of pcf_ttm. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | cash_flow_to_price | winsor_z | loss_firms_separate | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [non_monotone]: Negative operating cash flow leaves the ranked domain and is carried as a separate indicator. |
| `ebitda_to_ev` | `metric:ebitda_ttm` / `metric:enterprise_value` | +1 | Trailing twelve-month EBITDA over enterprise value; the monotone inverse of ev_ebitda (the enterprise multiple). | Loughran and Wellman 2011 (Journal of Financial and Quantitative Analysis) | published_anomaly | enterprise_value_yield | rank_normal | positive_denominator_required:metric:enterprise_value | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [sign_flip, presence_rule, unguarded_zero, coverage_bias]: Enterprise value is non-positive for net-cash firms and then flips the yield sign; its debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and missing preferred stock and minority interest read as zero. |

## profitability

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `gross_margin` | `gross_margin` (ttm) | +1 | Trailing twelve-month gross profit over trailing revenue. | Novy-Marx 2013 (Journal of Financial Economics); Fama and French 2015 (Journal of Financial Economics) | economic_conjecture | gross_margin | winsor_z | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `operating_margin` | `operating_margin` (ttm) | +1 | Trailing twelve-month operating income over trailing revenue. | Fama and French 2015 (Journal of Financial Economics); Ball Gerakos Linnainmaa and Nikolaev 2015 (Journal of Financial Economics) | published_analogue | operating_margin | winsor_z | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `net_margin` | `net_margin` (ttm) | +1 | Trailing twelve-month net income over trailing revenue. | Haugen and Baker 1996 (Journal of Financial Economics); Fama and French 2015 (Journal of Financial Economics) | published_analogue | net_margin | winsor_z | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `ebitda_margin` | `ebitda_margin` (ttm) | +1 | Trailing twelve-month EBITDA over trailing revenue. | Fama and French 2015 (Journal of Financial Economics); Ball Gerakos Linnainmaa and Nikolaev 2015 (Journal of Financial Economics) | published_analogue | operating_margin | winsor_z | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `gross_margin_q` | `gross_margin_q` (q) | +1 | Single-quarter gross profit over single-quarter revenue. | Novy-Marx 2013 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | economic_conjecture | gross_margin | winsor_z | unrestricted | conservative_filing_46h | 1q/0s | eligible_with_caveat [fiscal_seasonality]: A single-quarter level carries fiscal seasonality. |
| `operating_margin_q` | `operating_margin_q` (q) | +1 | Single-quarter operating income over single-quarter revenue. | Fama and French 2015 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | operating_margin | winsor_z | unrestricted | conservative_filing_46h | 1q/0s | eligible_with_caveat [fiscal_seasonality]: A single-quarter level carries fiscal seasonality. |
| `net_margin_q` | `net_margin_q` (q) | +1 | Single-quarter net income over single-quarter revenue. | Haugen and Baker 1996 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | net_margin | winsor_z | unrestricted | conservative_filing_46h | 1q/0s | eligible_with_caveat [fiscal_seasonality]: A single-quarter level carries fiscal seasonality. |
| `roa` | `roa` (ttm) | +1 | Trailing twelve-month net income over average total assets. | Haugen and Baker 1996 (Journal of Financial Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | return_on_assets | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `roe` | `roe` (ttm) | +1 | Trailing twelve-month net income to common over average common equity. | Hou Xue and Zhang 2015 (Review of Financial Studies); Haugen and Baker 1996 (Journal of Financial Economics) | published_analogue | return_on_equity | rank_normal | negative_book_excluded:metric:common_equity_avg2 | conservative_filing_46h | 5q/0s | eligible_with_caveat [sign_flip]: Negative average common equity inverts the ratio so a loss can read as a positive return. |
| `roic` | `roic` (ttm) | +1 | Trailing twelve-month NOPAT over average invested capital net of cash. | Fama and French 2015 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | return_on_invested_capital | rank_normal | positive_denominator_required:metric:invested_capital_avg2 | conservative_filing_46h | 5q/0s | eligible_with_caveat [sign_flip, presence_rule, unguarded_zero, coverage_bias]: Invested capital net of cash can be non-positive for cash-rich firms and then flips the sign; its debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and missing minority interest reads as zero. |
| `roic_ex_goodwill` | `roic_ex_goodwill` (ttm) | +1 | Trailing twelve-month NOPAT over average invested capital net of cash and goodwill. | Fama and French 2015 (Journal of Financial Economics); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | return_on_invested_capital | rank_normal | positive_denominator_required:metric:invested_capital_ex_goodwill_avg2 | conservative_filing_46h | 5q/0s | eligible_with_caveat [sign_flip, presence_rule, unguarded_zero, coverage_bias]: Invested capital net of cash and goodwill can be non-positive and then flips the sign; its debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and missing minority interest and goodwill read as zero. |
| `gross_profitability` | `gross_profitability` (ttm) | +1 | Trailing twelve-month gross profit over average total assets. | Novy-Marx 2013 (Journal of Financial Economics) | published_anomaly | gross_profitability | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `operating_profitability` | `operating_profitability` (ttm) | +1 | Trailing twelve-month operating income over average total assets. | Ball Gerakos Linnainmaa and Nikolaev 2015 (Journal of Financial Economics); Fama and French 2015 (Journal of Financial Economics) | published_anomaly | operating_profitability | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `cash_profitability` | `cash_profitability` (ttm) | +1 | Trailing operating income plus depreciation less working-capital accrual changes over average total assets. | Ball Gerakos Linnainmaa and Nikolaev 2016 (Journal of Financial Economics) | published_analogue | cash_profitability | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [construct_deviation, presence_rule]: Ball Gerakos Linnainmaa and Nikolaev add R&D back to operating profit and this construction does not; an issuer that has never tagged a mapped inventory alias has zero inventory change. |
| `cfo_to_assets` | `cfo_to_assets` (ttm) | +1 | Trailing twelve-month operating cash flow over average total assets. | Ball Gerakos Linnainmaa and Nikolaev 2016 (Journal of Financial Economics); Piotroski 2000 (Journal of Accounting Research) | published_analogue | cash_profitability | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `roe_q` | `roe_q` (q) | +1 | Single-quarter net income over common equity at the prior quarter end (q-factor ROE). | Hou Xue and Zhang 2015 (Review of Financial Studies); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | return_on_equity | rank_normal | guarded_in_definition | conservative_filing_46h | 2q/0s | eligible_with_caveat [fiscal_seasonality, construct_deviation]: A single-quarter level carries fiscal seasonality; the value is labeled quarterly only when the opening balance is proven to be the prior quarter end; the numerator is net income including discontinued and extraordinary items where Hou Xue and Zhang use income before extraordinary items. |
| `roa_q` | `roa_q` (q) | +1 | Single-quarter net income over total assets at the prior quarter end. | Balakrishnan Bartov and Faurel 2010 (Journal of Accounting and Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | return_on_assets | winsor_z | guarded_in_definition | conservative_filing_46h | 2q/0s | eligible_with_caveat [fiscal_seasonality, construct_deviation]: A single-quarter level carries fiscal seasonality; the value is labeled quarterly only when the opening balance is proven to be the prior quarter end; the numerator is net income including discontinued and extraordinary items where the published constructions use income before extraordinary items. |
| `rnoa_q` | `rnoa_q` (q) | +1 | Single-quarter operating income over net operating assets at the prior quarter end. | Soliman 2008 (The Accounting Review); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | return_on_net_operating_assets | rank_normal | guarded_in_definition | conservative_filing_46h | 2q/0s | eligible_with_caveat [fiscal_seasonality, presence_rule, coverage_bias]: A single-quarter level carries fiscal seasonality; the value is labeled quarterly only when the opening balance is proven to be the prior quarter end; net operating assets net out debt which is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value). |
| `rnoa` | `rnoa` (ttm) | +1 | Trailing twelve-month operating income over net operating assets at the start of the trailing year. | Soliman 2008 (The Accounting Review); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | return_on_net_operating_assets | rank_normal | guarded_in_definition | conservative_filing_46h | 5q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Net operating assets net out debt; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |
| `gross_profitability_q` | `gross_profitability_q` (q) | +1 | Single-quarter gross profit over total assets at the prior quarter end. | Novy-Marx 2013 (Journal of Financial Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | gross_profitability | winsor_z | guarded_in_definition | conservative_filing_46h | 2q/0s | eligible_with_caveat [fiscal_seasonality]: A single-quarter level carries fiscal seasonality; the value is labeled quarterly only when the opening balance is proven to be the prior quarter end. |
| `operating_profitability_q` | `operating_profitability_q` (q) | +1 | Single-quarter gross profit less SG&A (before R&D; selling plus general and administrative expense when no SG&A total is tagged) over total assets at the prior quarter end. | Ball Gerakos Linnainmaa and Nikolaev 2015 (Journal of Financial Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | operating_profitability | winsor_z | guarded_in_definition | conservative_filing_46h | 2q/0s | eligible_with_caveat [fiscal_seasonality]: A single-quarter level carries fiscal seasonality; the value is labeled quarterly only when the opening balance is proven to be the prior quarter end. |
| `cfo_to_assets_q` | `cfo_to_assets_q` (q) | +1 | Single-quarter operating cash flow over total assets at the prior quarter end. | Ball Gerakos Linnainmaa and Nikolaev 2016 (Journal of Financial Economics); Sloan 1996 (The Accounting Review) | published_analogue | cash_profitability | winsor_z | guarded_in_definition | conservative_filing_46h | 2q/0s | eligible_with_caveat [fiscal_seasonality]: A single-quarter level carries fiscal seasonality; the value is labeled quarterly only when the opening balance is proven to be the prior quarter end. |
| `fcf_to_assets` | `fcf_to_assets` (ttm) | +1 | Trailing twelve-month free cash flow over average total assets. | Asness Frazzini and Pedersen 2019 (Review of Accounting Studies) | published_analogue | cash_profitability | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |

## quality

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `piotroski_f` | `piotroski_f` (ttm) | +1 | Nine binary signals of profitability and its change and of liquidity and leverage and issuance and efficiency (0 to 9). | Piotroski 2000 (Journal of Accounting Research) | published_anomaly | piotroski_f_score | winsor_z | unrestricted | conservative_filing_46h | 9q/0s | eligible_with_caveat [split_basis, presence_rule, coverage_bias]: The equity-offering signal compares period-end share counts a year apart and is comparable only where daily-bar split epochs (R1d) rebase both counts and labeled incomparable otherwise (piotroski_f_cash_issuance reads issuance from cash proceeds instead); long-term debt missing beside reported short-term debt is absent within the reported total and is zero for an issuer that has never tagged a mapped debt alias while a switch to an unmapped alias leaves the leverage signal and so the score without a value. |
| `piotroski_f_cash_issuance` | `piotroski_f_cash_issuance` (ttm) | +1 | Nine binary Piotroski signals (0 to 9) with the no-equity-issuance signal read from trailing cash proceeds of equity issuance. | Piotroski 2000 (Journal of Accounting Research) | published_analogue | piotroski_f_score | winsor_z | unrestricted | conservative_filing_46h | 9q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Equity issuance is read from cash proceeds (option exercises included) only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and is never imputed as none since an absent discrete quarter cannot prove absence from a year-to-date or annual fact (non-issuers that never tag the concept have no score); long-term debt missing beside reported short-term debt is absent within the reported total and is zero for an issuer that has never tagged a mapped debt alias while a switch to an unmapped alias leaves the leverage signal and so the score without a value. |
| `altman_z_book` | `altman_z_book` (ttm) | +1 | Altman Z-score with book equity in the equity-to-liabilities term. | Altman 1968 (Journal of Finance); Dichev 1998 (Journal of Finance) | published_analogue | distress_risk | rank_normal | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `altman_z` | `altman_z` (daily) | +1 | Altman Z-score with market equity in the equity-to-liabilities term. | Dichev 1998 (Journal of Finance); Altman 1968 (Journal of Finance) | published_anomaly | distress_risk | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 4q/1s | eligible |
| `beneish_m` | `beneish_m` (ttm) | -1 | Beneish eight-variable earnings-manipulation M-score. | Beneish 1999 (Financial Analysts Journal); Beneish Lee and Nichols 2013 (Financial Analysts Journal) | published_anomaly | earnings_manipulation | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `ohlson_o` | `ohlson_o` (ttm) | -1 | Ohlson O-score bankruptcy index (no GNP deflator). | Ohlson 1980 (Journal of Accounting Research); Dichev 1998 (Journal of Finance); Griffin and Lemmon 2002 (Journal of Finance) | published_anomaly | distress_risk | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `tax_to_book_income` | `tax_to_book_income` (ttm) | +1 | Trailing twelve-month income tax expense over trailing net income. | Lev and Nissim 2004 (The Accounting Review) | published_analogue | tax_to_book_income | rank_normal | positive_denominator_required:metric:net_income_ttm | conservative_filing_46h | 4q/0s | eligible_with_caveat [sign_flip]: Negative net income inverts the ratio. |
| `cash_to_assets` | `cash_to_assets` (q) | +1 | Cash and short-term investments over total assets. | Palazzo 2012 (Journal of Financial Economics) | published_anomaly | cash_holdings | winsor_z | guarded_in_definition | conservative_filing_46h | 1q/0s | eligible |

## growth

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `revenue_growth_yoy` | `revenue_growth_yoy` (ttm) | two-sided | Year-over-year growth of trailing twelve-month revenue. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Jegadeesh and Livnat 2006 (Journal of Accounting and Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_analogue | revenue_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible_with_caveat [mixed_evidence]: Mixed one-year evidence: the trailing growth rate averages four seasonal quarterly growth rates whose freshest component is the positively signed revenue_q_growth_yoy. |
| `revenue_cagr_3y` | `revenue_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing revenue with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | published_analogue | revenue_growth_multi_year | rank_normal | guarded_in_definition | conservative_filing_46h | 16q/0s | eligible |
| `gross_profit_growth_yoy` | `gross_profit_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month gross profit. | Novy-Marx 2015 (NBER Working Paper 20984); Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | economic_conjecture | gross_profit_growth | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `operating_income_growth_yoy` | `operating_income_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month operating income. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance); Novy-Marx 2015 (NBER Working Paper 20984) | published_analogue | earnings_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `net_income_growth_yoy` | `net_income_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month net income over the absolute prior base. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance); Foster Olsen and Shevlin 1984 (The Accounting Review) | published_analogue | earnings_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `eps_diluted_growth_yoy` | `eps_diluted_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month diluted EPS over the absolute prior base. | Bernard and Thomas 1989 (Journal of Accounting Research); Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | published_analogue | earnings_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible_with_caveat [split_basis]: Trailing diluted EPS sums four quarters each filed on its own split basis and is comparable only where the quarters share one filing clock or daily-bar split epochs (R1d) rebase them and labeled incomparable otherwise. |
| `cfo_growth_yoy` | `cfo_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month operating cash flow. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | cash_flow_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `fcf_growth_yoy` | `fcf_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month free cash flow. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | cash_flow_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `tax_expense_change_yoy` | `tax_expense_change_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month income tax expense. | Thomas and Zhang 2011 (Journal of Accounting Research) | published_analogue | tax_expense_growth | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `revenue_growth_qoq` | `revenue_growth_qoq` (ttm) | +1 | Quarter-over-quarter growth of trailing twelve-month revenue. | Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | economic_conjecture | revenue_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | blocked_incomparable_origin [trailing_span_overlap]: A one-quarter change of a trailing sum compares overlapping 365-day spans; the engine labels every value incomparable. |
| `eps_diluted_growth_qoq` | `eps_diluted_growth_qoq` (ttm) | +1 | Quarter-over-quarter growth of trailing twelve-month diluted EPS. | Bernard and Thomas 1989 (Journal of Accounting Research) | economic_conjecture | profit_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | blocked_incomparable_origin [trailing_span_overlap, split_basis]: A one-quarter change of trailing EPS compares overlapping 365-day spans so the engine labels every value incomparable (its split basis would also be gated per row). |
| `eps_cagr_3y` | `eps_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing diluted EPS with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | profit_growth_multi_year | rank_normal | guarded_in_definition | conservative_filing_46h | 16q/0s | eligible_with_caveat [split_basis]: Per-share trailing EPS twelve quarters apart is comparable only where daily-bar split epochs (R1d) rebase both ends to one split basis and labeled incomparable otherwise. |
| `cfo_cagr_3y` | `cfo_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing operating cash flow with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | cash_flow_growth_multi_year | rank_normal | guarded_in_definition | conservative_filing_46h | 16q/0s | eligible |
| `gross_profit_cagr_3y` | `gross_profit_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing gross profit with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | profit_growth_multi_year | rank_normal | guarded_in_definition | conservative_filing_46h | 16q/0s | eligible |
| `operating_income_cagr_3y` | `operating_income_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing operating income with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | profit_growth_multi_year | rank_normal | guarded_in_definition | conservative_filing_46h | 16q/0s | eligible |
| `ebitda_cagr_3y` | `ebitda_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing EBITDA with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | profit_growth_multi_year | rank_normal | guarded_in_definition | conservative_filing_46h | 16q/0s | eligible |
| `fcf_cagr_3y` | `fcf_cagr_3y` (ttm) | -1 | Three-year compound annual growth of trailing free cash flow with positive endpoints required. | Lakonishok Shleifer and Vishny 1994 (Journal of Finance); Chan Karceski and Lakonishok 2003 (Journal of Finance) | economic_conjecture | cash_flow_growth_multi_year | rank_normal | guarded_in_definition | conservative_filing_46h | 16q/0s | eligible |
| `gross_margin_change_yoy` | `gross_margin_change_yoy` (ttm) | +1 | Year-over-year change in trailing gross margin in fraction points. | Abarbanell and Bushee 1998 (The Accounting Review); Piotroski 2000 (Journal of Accounting Research) | published_anomaly | gross_margin_change | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `operating_margin_change_yoy` | `operating_margin_change_yoy` (ttm) | +1 | Year-over-year change in trailing operating margin in fraction points. | Abarbanell and Bushee 1998 (The Accounting Review); Akbas Jiang and Koch 2017 (The Accounting Review) | published_analogue | operating_margin_change | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `net_margin_change_yoy` | `net_margin_change_yoy` (ttm) | +1 | Year-over-year change in trailing net margin in fraction points. | Soliman 2008 (The Accounting Review); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | net_margin_change | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `operating_profitability_change_yoy` | `operating_profitability_change_yoy` (ttm) | +1 | Year-over-year change in operating income over average assets. | Akbas Jiang and Koch 2017 (The Accounting Review); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_analogue | profitability_change | rank_normal | unrestricted | conservative_filing_46h | 9q/0s | eligible |
| `roe_change_yoy` | `roe_change_yoy` (ttm) | +1 | Year-over-year change in trailing return on average common equity. | Hou Xue and Zhang 2020 (Review of Financial Studies); Hou Mo Xue and Zhang 2021 (Review of Finance) | published_analogue | profitability_change | rank_normal | guarded_in_definition | conservative_filing_46h | 9q/0s | eligible |
| `gross_margin_q_change_yoy` | `gross_margin_q_change_yoy` (q) | +1 | Single-quarter gross margin less the same fiscal quarter's margin a year earlier. | Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | gross_margin_change | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `operating_margin_q_change_yoy` | `operating_margin_q_change_yoy` (q) | +1 | Single-quarter operating margin less the same fiscal quarter's margin a year earlier. | Abarbanell and Bushee 1998 (The Accounting Review); Akbas Jiang and Koch 2017 (The Accounting Review) | published_analogue | operating_margin_change | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `net_margin_q_change_yoy` | `net_margin_q_change_yoy` (q) | +1 | Single-quarter net margin less the same fiscal quarter's margin a year earlier. | Soliman 2008 (The Accounting Review); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | net_margin_change | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `revenue_q_growth_yoy` | `revenue_q_growth_yoy` (q) | +1 | Single-quarter revenue growth over the same fiscal quarter a year earlier. | Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | published_analogue | revenue_surprise | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `gross_profit_q_growth_yoy` | `gross_profit_q_growth_yoy` (q) | +1 | Single-quarter gross-profit growth over the same fiscal quarter a year earlier. | Novy-Marx 2015 (NBER Working Paper 20984); Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | economic_conjecture | gross_profit_growth | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `operating_income_q_growth_yoy` | `operating_income_q_growth_yoy` (q) | +1 | Single-quarter operating-income growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | earnings_surprise | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `net_income_q_growth_yoy` | `net_income_q_growth_yoy` (q) | +1 | Single-quarter net-income growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | earnings_surprise | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `eps_diluted_q_growth_yoy` | `eps_diluted_q_growth_yoy` (q) | +1 | Single-quarter diluted-EPS growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | earnings_surprise | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [split_basis, filing_clock_lag]: Per-share quarters a year apart are comparable on one filing's restated comparative or daily-bar split epochs (R1d) and labeled incomparable otherwise; filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release. |
| `eps_basic_q_growth_yoy` | `eps_basic_q_growth_yoy` (q) | +1 | Single-quarter basic-EPS growth over the same fiscal quarter a year earlier. | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | earnings_surprise | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [split_basis, filing_clock_lag]: Per-share quarters a year apart are comparable on one filing's restated comparative or daily-bar split epochs (R1d) and labeled incomparable otherwise; filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release. |
| `cfo_q_growth_yoy` | `cfo_q_growth_yoy` (q) | +1 | Single-quarter operating-cash-flow growth over the same fiscal quarter a year earlier. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | cash_flow_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `fcf_q_growth_yoy` | `fcf_q_growth_yoy` (q) | +1 | Single-quarter free-cash-flow growth over the same fiscal quarter a year earlier. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | cash_flow_growth_1y | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `revenue_q_growth_qoq` | `revenue_q_growth_qoq` (q) | +1 | Single-quarter revenue growth over the immediately preceding fiscal quarter. | Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | economic_conjecture | revenue_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `gross_profit_q_growth_qoq` | `gross_profit_q_growth_qoq` (q) | +1 | Single-quarter gross-profit growth over the immediately preceding fiscal quarter. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | profit_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `operating_income_q_growth_qoq` | `operating_income_q_growth_qoq` (q) | +1 | Single-quarter operating-income growth over the immediately preceding fiscal quarter. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | economic_conjecture | profit_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `net_income_q_growth_qoq` | `net_income_q_growth_qoq` (q) | +1 | Single-quarter net-income growth over the immediately preceding fiscal quarter. | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | economic_conjecture | profit_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `cfo_q_growth_qoq` | `cfo_q_growth_qoq` (q) | +1 | Single-quarter operating-cash-flow growth over the immediately preceding fiscal quarter. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | cash_flow_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `fcf_q_growth_qoq` | `fcf_q_growth_qoq` (q) | +1 | Single-quarter free-cash-flow growth over the immediately preceding fiscal quarter. | Novy-Marx 2015 (NBER Working Paper 20984) | economic_conjecture | cash_flow_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `eps_diluted_q_growth_qoq` | `eps_diluted_q_growth_qoq` (q) | +1 | Single-quarter diluted-EPS growth over the immediately preceding fiscal quarter. | Bernard and Thomas 1989 (Journal of Accounting Research) | economic_conjecture | profit_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter, split_basis]: Adjacent quarters come from different filings so the per-share pair is comparable only where daily-bar split epochs (R1d) rebase it and labeled incomparable otherwise; sequential quarters also carry fiscal seasonality and unequal quarter lengths. |
| `eps_basic_q_growth_qoq` | `eps_basic_q_growth_qoq` (q) | +1 | Single-quarter basic-EPS growth over the immediately preceding fiscal quarter. | Bernard and Thomas 1989 (Journal of Accounting Research) | economic_conjecture | profit_growth_sequential | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter, split_basis]: Adjacent quarters come from different filings so the per-share pair is comparable only where daily-bar split epochs (R1d) rebase it and labeled incomparable otherwise; sequential quarters also carry fiscal seasonality and unequal quarter lengths. |
| `eps_diluted_q_growth_yoy_accel` | `eps_diluted_q_growth_yoy_accel` (q) | +1 | This quarter's seasonal diluted-EPS growth less the previous quarter's seasonal growth. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics) | published_anomaly | earnings_acceleration | rank_normal | unrestricted | conservative_filing_46h | 6q/0s | eligible_with_caveat [split_basis, filing_clock_lag]: Built from per-share seasonal growth: comparable only where each EPS pair shares a filing clock or daily-bar split epochs (R1d) and labeled incomparable otherwise; filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release. |
| `revenue_q_growth_yoy_accel` | `revenue_q_growth_yoy_accel` (q) | +1 | This quarter's seasonal revenue growth less the previous quarter's seasonal growth. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics); Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | published_analogue | revenue_acceleration | rank_normal | unrestricted | conservative_filing_46h | 6q/0s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `gross_margin_q_change_yoy_accel` | `gross_margin_q_change_yoy_accel` (q) | +1 | This quarter's seasonal gross-margin change less the previous quarter's seasonal change. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics); Abarbanell and Bushee 1998 (The Accounting Review) | economic_conjecture | margin_acceleration | rank_normal | unrestricted | conservative_filing_46h | 6q/0s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `operating_margin_q_change_yoy_accel` | `operating_margin_q_change_yoy_accel` (q) | +1 | This quarter's seasonal operating-margin change less the previous quarter's seasonal change. | He and Narayanamoorthy 2020 (Journal of Accounting and Economics); Abarbanell and Bushee 1998 (The Accounting Review) | economic_conjecture | margin_acceleration | rank_normal | unrestricted | conservative_filing_46h | 6q/0s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `sue_ni` | `sue_ni` (q) | +1 | Seasonal change in quarterly net income over the sample standard deviation of the eight preceding seasonal changes (standardized unexpected earnings on net income). | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research); Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | published_analogue | earnings_surprise | rank_normal | unrestricted | conservative_filing_46h | 13q/0s | eligible_with_caveat [filing_clock_lag, construct_deviation]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift; net income replaces EPS before extraordinary items to stay share-basis-free. |
| `sue_revenue` | `sue_revenue` (q) | +1 | Seasonal change in quarterly revenue over the sample standard deviation of the eight preceding seasonal changes (standardized unexpected revenue). | Jegadeesh and Livnat 2006 (Journal of Accounting and Economics) | published_analogue | revenue_surprise | rank_normal | unrestricted | conservative_filing_46h | 13q/0s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `earnings_surprise_to_market` | `metric:ni_q_change_yoy` / `metric:market_cap` | +1 | Seasonal change in quarterly net income over market capitalization (price-scaled earnings surprise). | Livnat and Mendenhall 2006 (Journal of Accounting Research); Bernard and Thomas 1989 (Journal of Accounting Research) | published_analogue | earnings_surprise | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 5q/1s | eligible_with_caveat [filing_clock_lag]: Filing-clocked (10-Q plus 46 hours) so the value arrives weeks after the earnings release and misses the announcement return and early drift. |
| `roe_q_change_yoy` | `roe_q_change_yoy` (q) | +1 | Single-quarter ROE less the same fiscal quarter's ROE a year earlier (dRoe). | Hou Xue and Zhang 2020 (Review of Financial Studies); Hou Mo Xue and Zhang 2021 (Review of Finance) | published_anomaly | profitability_change | rank_normal | guarded_in_definition | conservative_filing_46h | 6q/0s | eligible |
| `roa_q_change_yoy` | `roa_q_change_yoy` (q) | +1 | Single-quarter ROA less the same fiscal quarter's ROA a year earlier (dRoa). | Balakrishnan Bartov and Faurel 2010 (Journal of Accounting and Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | profitability_change | rank_normal | guarded_in_definition | conservative_filing_46h | 6q/0s | eligible |
| `ear_m1p1` | `ear_m1p1` (event, event) | +1 | Market-adjusted return (the line's one-session adjusted-close return less the equal-weighted bar-return market of the panel's eligible lines) summed over sessions E-1 to E+1 around the latest visible earnings announcement session E (the earliest 8-K Item 2.02 after the period end else the periodic filing). | Chan Jegadeesh and Lakonishok 1996 (Journal of Finance); Brandt Kishore Santa-Clara and Venkatachalam 2008 (working paper) | published_anomaly | earnings_surprise | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 0q/4s | eligible_with_caveat [coverage_bias]: Events are keyed by original 10-K/10-Q periods so a release never followed by a periodic filing is absent and only linked primary lines carry a value; announcement times are EDGAR acceptance stamps (an unknown time takes the next session) and early closes are not modeled. |
| `sue_ni_event` | `sue_ni_event` (event, event) | +1 | The panel's sue_ni state of the latest visible earnings event's fiscal period read at the later of the state's own clock and the event clock (the close of event session E+1). | Foster Olsen and Shevlin 1984 (The Accounting Review); Bernard and Thomas 1989 (Journal of Accounting Research); Chan Jegadeesh and Lakonishok 1996 (Journal of Finance) | published_analogue | earnings_surprise | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 13q/0s | blocked_duplicate_hypothesis [duplicate_hypothesis, filing_clock_lag, construct_deviation]: Duplicate of the primary sue_ni: the same derived states on a clock never earlier than their filing clock and only where an earnings event is visible so it adds no timing and at most sue_ni's coverage; the feature store never builds both (one SUE hypothesis). |

## investment

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `asset_growth` | `asset_growth` (q) | -1 | Year-over-year growth of total assets. | Cooper Gulen and Schill 2008 (Journal of Finance); Fama and French 2015 (Journal of Financial Economics) | published_anomaly | asset_growth | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `total_assets_cagr_3y` | `total_assets_cagr_3y` (q) | -1 | Three-year compound annual growth of total assets with positive endpoints required. | Cooper Gulen and Schill 2008 (Journal of Finance); Hou Xue and Zhang 2015 (Review of Financial Studies) | published_analogue | asset_growth | rank_normal | guarded_in_definition | conservative_filing_46h | 13q/0s | eligible |
| `book_value_growth_yoy` | `book_value_growth_yoy` (q) | -1 | Year-over-year growth of common book equity. | Cooper Gulen and Schill 2008 (Journal of Finance); Fama and French 2015 (Journal of Financial Economics) | economic_conjecture | book_equity_growth | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `common_equity_cagr_3y` | `common_equity_cagr_3y` (q) | -1 | Three-year compound annual growth of common equity with positive endpoints required. | Cooper Gulen and Schill 2008 (Journal of Finance); Fama and French 2015 (Journal of Financial Economics) | economic_conjecture | book_equity_growth | rank_normal | guarded_in_definition | conservative_filing_46h | 13q/0s | eligible |
| `delta_noa` | `delta_noa` (q) | -1 | Year-over-year change in net operating assets over lagged total assets. | Hirshleifer Hou Teoh and Zhang 2004 (Journal of Accounting and Economics); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | noa_growth | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Net operating assets net out debt; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |
| `capex_growth_yoy` | `capex_growth_yoy` (ttm) | -1 | Year-over-year growth of trailing twelve-month capital expenditure. | Anderson and Garcia-Feijoo 2006 (Journal of Finance); Xing 2008 (Review of Financial Studies) | published_anomaly | capex_growth | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `capex_q_growth_yoy` | `capex_q_growth_yoy` (q) | -1 | Single-quarter capital-expenditure growth over the same fiscal quarter a year earlier. | Anderson and Garcia-Feijoo 2006 (Journal of Finance); Xing 2008 (Review of Financial Studies) | published_analogue | capex_growth | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `capex_q_growth_qoq` | `capex_q_growth_qoq` (q) | -1 | Single-quarter capital-expenditure growth over the immediately preceding fiscal quarter. | Anderson and Garcia-Feijoo 2006 (Journal of Finance) | economic_conjecture | capex_growth | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `capex_to_depreciation` | `capex_to_depreciation` (ttm) | -1 | Trailing capital expenditure over trailing depreciation and amortization. | Titman Wei and Xie 2004 (Journal of Financial and Quantitative Analysis) | economic_conjecture | capex_intensity | rank_normal | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `capex_to_sales` | `capex_to_sales` (ttm) | -1 | Trailing capital expenditure over trailing revenue. | Titman Wei and Xie 2004 (Journal of Financial and Quantitative Analysis) | economic_conjecture | capex_intensity | winsor_z | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `rd_intensity_sales` | `rd_intensity_sales` (ttm) | +1 | Trailing research and development expense over trailing revenue. | Chan Lakonishok and Sougiannis 2001 (Journal of Finance); Lev and Sougiannis 1996 (Journal of Accounting and Economics) | economic_conjecture | rd_intensity | rank_normal | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `rd_expense_growth_yoy` | `rd_expense_growth_yoy` (ttm) | +1 | Year-over-year growth of trailing twelve-month research and development expense. | Eberhart Maxwell and Siddique 2004 (Journal of Finance) | published_analogue | rd_growth | rank_normal | unrestricted | conservative_filing_46h | 8q/0s | eligible |
| `rd_expense_q_growth_yoy` | `rd_expense_q_growth_yoy` (q) | +1 | Single-quarter R&D expense growth over the same fiscal quarter a year earlier. | Eberhart Maxwell and Siddique 2004 (Journal of Finance) | economic_conjecture | rd_growth | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `rd_expense_q_growth_qoq` | `rd_expense_q_growth_qoq` (q) | +1 | Single-quarter R&D expense growth over the immediately preceding fiscal quarter. | Eberhart Maxwell and Siddique 2004 (Journal of Finance) | economic_conjecture | rd_growth | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [sequential_quarter]: Sequential quarters carry fiscal seasonality and unequal quarter lengths; quarterly origin only when adjacency is proven. |
| `investment_to_assets` | `investment_to_assets` (q) | -1 | Year-over-year change in net PP&E plus the year-over-year change in inventory over total assets four quarters earlier (I/A). | Lyandres Sun and Zhang 2008 (Review of Financial Studies); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | investment_to_assets | winsor_z | guarded_in_definition | conservative_filing_46h | 5q/0s | eligible_with_caveat [construct_deviation, presence_rule]: Net PP&E replaces the gross PP&E of Lyandres Sun and Zhang so one PP&E basis spans every issuer and fiscal quarter (gross PP&E is often reported only in the annual note and would jump each fiscal fourth quarter by the year's depreciation); a filer moving to the combined PP&E and finance-lease right-of-use alias at ASC 842 adoption shows a one-time change equal to that asset; an issuer that has never tagged a mapped inventory alias has zero inventory change and otherwise missing inventory has no value. |
| `inventory_change_to_assets` | `inventory_change_to_assets` (q) | -1 | Year-over-year change in inventory over average total assets. | Thomas and Zhang 2002 (Review of Accounting Studies); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | inventory_change | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [presence_rule]: An issuer that has never tagged a mapped inventory alias has zero inventory change; otherwise (a switch to an unmapped alias included) missing inventory has no value. |
| `capex_to_assets` | `capex_to_assets` (ttm) | -1 | Trailing twelve-month capital expenditure over average total assets. | Titman Wei and Xie 2004 (Journal of Financial and Quantitative Analysis); Polk and Sapienza 2009 (Review of Financial Studies) | published_analogue | capex_intensity | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `capex_growth_2y` | `capex_growth_2y` (ttm) | -1 | Two-year growth of trailing twelve-month capital expenditure. | Anderson and Garcia-Feijoo 2006 (Journal of Finance); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | capex_growth | rank_normal | unrestricted | conservative_filing_46h | 12q/0s | eligible |
| `capex_growth_3y` | `capex_growth_3y` (ttm) | -1 | Three-year growth of trailing twelve-month capital expenditure. | Anderson and Garcia-Feijoo 2006 (Journal of Finance); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | capex_growth | rank_normal | unrestricted | conservative_filing_46h | 16q/0s | eligible |
| `rd_to_assets` | `rd_to_assets` (ttm) | +1 | Trailing twelve-month R&D expense over average total assets. | Li 2011 (Review of Financial Studies); Chan Lakonishok and Sougiannis 2001 (Journal of Finance) | published_analogue | rd_intensity | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |

## accruals

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `total_accruals` | `total_accruals` (ttm) | -1 | Trailing net income less operating cash flow over average total assets. | Sloan 1996 (The Accounting Review) | published_anomaly | accruals | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `percent_accruals` | `percent_accruals` (ttm) | -1 | Trailing net income less operating cash flow over absolute trailing net income. | Hafzalla Lundholm and Van Winkle 2011 (The Accounting Review) | published_anomaly | accruals | rank_normal | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `working_capital_accruals` | `working_capital_accruals` (ttm) | -1 | Year-over-year change in operating working capital over average total assets. | Sloan 1996 (The Accounting Review) | published_anomaly | accruals | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Short-term debt missing beside reported long-term debt is absent within the reported total (an intermittent revolver or current portion keeps the value) and is zero for an issuer that has never tagged a mapped debt alias; otherwise missing short-term debt has no value rather than a spurious accrual. |
| `rsst_accruals` | `rsst_accruals` (ttm) | -1 | Change in net operating assets less debt (total assets less cash and short-term investments less total liabilities) over average total assets (broad accruals: the change in book equity less the change in cash and short-term investments). | Richardson Sloan Soliman and Tuna 2005 (Journal of Accounting and Economics) | published_anomaly | accruals | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [construct_deviation]: Debt cancels (net operating assets less debt equals total assets less cash and short-term investments less total liabilities) so no debt concept is read; Richardson Sloan Soliman and Tuna net short-term investments against financial assets separately and keep preferred stock and minority interest outside common equity while here they sit in the residual. |
| `noa_to_assets` | `noa_to_assets` (q) | -1 | Net operating assets over lagged total assets (cumulative accruals). | Hirshleifer Hou Teoh and Zhang 2004 (Journal of Accounting and Economics) | published_anomaly | noa_level | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Net operating assets net out debt; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |

## leverage

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `debt_to_equity` | `debt_to_equity` (q) | -1 | Interest-bearing debt over stockholders equity (book leverage). | George and Hwang 2010 (Journal of Financial Economics); Penman Richardson and Tuna 2007 (Journal of Accounting Research) | published_analogue | book_leverage | rank_normal | negative_book_excluded:item:stockholders_equity | conservative_filing_46h | 1q/0s | eligible_with_caveat [sign_flip, presence_rule, coverage_bias]: Negative stockholders equity inverts the ratio so the most levered firms read as unlevered; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |
| `debt_to_assets` | `debt_to_assets` (q) | -1 | Interest-bearing debt over total assets. | George and Hwang 2010 (Journal of Financial Economics); Penman Richardson and Tuna 2007 (Journal of Accounting Research) | published_analogue | book_leverage | winsor_z | unrestricted | conservative_filing_46h | 1q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |
| `long_term_debt_to_assets` | `long_term_debt_to_assets` (q) | -1 | Long-term debt over total assets. | George and Hwang 2010 (Journal of Financial Economics) | published_analogue | book_leverage | winsor_z | unrestricted | conservative_filing_46h | 1q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Long-term debt missing beside reported short-term debt is absent within the reported total and is zero for an issuer that has never tagged a mapped debt alias; otherwise (a switch to an unmapped alias) missing long-term debt has no value. |
| `net_debt_ebitda` | `net_debt_ebitda` (ttm) | -1 | Debt net of cash over trailing EBITDA. | George and Hwang 2010 (Journal of Financial Economics); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | economic_conjecture | debt_service | rank_normal | positive_denominator_required:metric:ebitda_ttm | conservative_filing_46h | 4q/0s | eligible_with_caveat [sign_flip, presence_rule, coverage_bias]: Non-positive trailing EBITDA inverts the ratio; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |
| `interest_coverage` | `interest_coverage` (ttm) | +1 | Trailing operating income over absolute trailing interest expense. | Campbell Hilscher and Szilagyi 2008 (Journal of Finance); Dichev 1998 (Journal of Finance) | economic_conjecture | debt_service | rank_normal | unrestricted | conservative_filing_46h | 4q/0s | eligible |
| `current_ratio` | `current_ratio` (q) | +1 | Current assets over current liabilities. | Piotroski 2000 (Journal of Accounting Research); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | economic_conjecture | liquidity | rank_normal | unrestricted | conservative_filing_46h | 1q/0s | eligible |
| `quick_ratio` | `quick_ratio` (q) | +1 | Current assets less inventory over current liabilities. | Piotroski 2000 (Journal of Accounting Research); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | economic_conjecture | liquidity | rank_normal | unrestricted | conservative_filing_46h | 1q/0s | eligible_with_caveat [presence_rule]: Inventory is zero only for an issuer that reports total assets and has never tagged a mapped inventory alias; a switch to an unmapped alias or a skipped quarter has no value rather than the current ratio. |
| `cash_ratio` | `cash_ratio` (q) | +1 | Cash and short-term investments over current liabilities. | Palazzo 2012 (Journal of Financial Economics); Campbell Hilscher and Szilagyi 2008 (Journal of Finance) | published_analogue | cash_holdings | rank_normal | unrestricted | conservative_filing_46h | 1q/0s | eligible |
| `debt_to_market` | `metric:total_debt_q` / `metric:market_cap` | two-sided | Interest-bearing debt over market capitalization (market leverage). | Bhandari 1988 (Journal of Finance); Fama and French 1992 (Journal of Finance); Penman Richardson and Tuna 2007 (Journal of Accounting Research) | published_analogue | market_leverage | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 1q/1s | eligible_with_caveat [mixed_evidence, presence_rule, coverage_bias]: Mixed evidence and largely subsumed by B/M: report it controlled for book_to_market; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |
| `assets_to_market` | `item:total_assets` / `metric:market_cap` | two-sided | Total assets over market capitalization (A/ME market leverage). | Fama and French 1992 (Journal of Finance); Penman Richardson and Tuna 2007 (Journal of Accounting Research) | published_analogue | market_leverage | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 1q/1s | eligible_with_caveat [mixed_evidence]: Mixed evidence and largely subsumed by B/M: report it controlled for book_to_market. |
| `debt_to_assets_change_yoy` | `debt_to_assets_change_yoy` (q) | -1 | Year-over-year change in total debt over total assets. | Piotroski 2000 (Journal of Accounting Research) | published_analogue | leverage_change | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Debt is zero only for an issuer that has never tagged a mapped debt alias and a component missing beside a reported one is absent within the reported total; a switch to an unmapped alias has no value and never reads as a deleveraging change. |
| `net_debt_to_book_equity` | `net_debt_to_book_equity` (q) | -1 | Total debt less cash and short-term investments over common equity (financing leverage). | Penman Richardson and Tuna 2007 (Journal of Accounting Research) | published_analogue | book_leverage | rank_normal | guarded_in_definition | conservative_filing_46h | 1q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Debt is zero only for an issuer that has never tagged a mapped debt alias and a component missing beside a reported one is absent within the reported total; a switch to an unmapped alias has no value (never a spurious net-cash reading). |
| `current_ratio_change_yoy` | `current_ratio_change_yoy` (q) | +1 | Year-over-year change in current assets over current liabilities. | Piotroski 2000 (Journal of Accounting Research) | published_analogue | liquidity_change | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `operating_leverage` | `operating_leverage` (ttm) | +1 | Trailing cost of revenue plus trailing SG&A over average total assets. | Novy-Marx 2011 (Review of Finance) | published_anomaly | operating_leverage | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [construct_deviation]: XBRL SG&A excludes separately reported R&D which the Compustat XSGA of Novy-Marx includes. |

## payout_issuance

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `net_equity_issuance` | `net_equity_issuance` (ttm) | -1 | Trailing equity issued less repurchased over average total assets. | Pontiff and Woodgate 2008 (Journal of Finance); Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics) | published_anomaly | equity_issuance | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [coverage_bias]: Issuance and repurchases are read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero since an absent discrete quarter cannot prove absence from a year-to-date or annual fact; firms that never repurchase or never issue have no value so the heavy-issuer short leg is thinned. |
| `net_debt_issuance` | `net_debt_issuance` (ttm) | -1 | Trailing long-term debt issued less repaid over average total assets. | Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics); Spiess and Affleck-Graves 1999 (Journal of Financial Economics) | published_anomaly | net_debt_issuance | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [coverage_bias]: Debt issuance and repayment are read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero; firms without one of the flows have no value. |
| `external_financing` | `external_financing` (ttm) | -1 | Net equity plus net debt financing over average total assets. | Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics) | published_anomaly | external_financing | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [coverage_bias]: Each financing flow is read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero; firms missing any flow (often non-repurchasers) have no value. |
| `shares_growth_yoy` | `shares_growth_yoy` (q) | -1 | Year-over-year growth of period-end shares outstanding. | Pontiff and Woodgate 2008 (Journal of Finance); Daniel and Titman 2006 (Journal of Finance) | published_anomaly | equity_issuance | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [split_basis]: Period-end share counts are balances that no filing restates for a split and are comparable only where daily-bar split epochs (R1d) rebase both counts to one split basis and labeled incomparable otherwise. |
| `payout_ratio` | `payout_ratio` (ttm) | +1 | Trailing common dividends over trailing earnings available to common. | Arnott and Asness 2003 (Financial Analysts Journal) | economic_conjecture | payout_ratio | rank_normal | positive_denominator_required:metric:net_income_common_ttm | conservative_filing_46h | 4q/0s | eligible_with_caveat [sign_flip, coverage_bias]: Negative earnings invert the ratio; dividends are read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero so non-payers that never tag a dividend have no value. |
| `buyback_yield` | `buyback_yield` (daily) | +1 | Trailing repurchases less issuance over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance); Ikenberry Lakonishok and Vermaelen 1995 (Journal of Financial Economics) | published_anomaly | equity_issuance | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [coverage_bias]: Repurchases and issuance are read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero; firms without one of the flows have no value. |
| `net_payout_yield` | `net_payout_yield` (daily) | +1 | Trailing dividends plus repurchases less issuance over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance) | published_anomaly | payout_yield | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [coverage_bias]: Dividends repurchases and issuance are each read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero so buyback-only and dividend-only firms that never tag the other flow have no value. |
| `total_payout_yield` | `total_payout_yield` (daily) | +1 | Trailing gross dividends plus gross repurchases over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance) | published_anomaly | payout_yield | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [coverage_bias]: Dividends and repurchases are each read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero so buyback-only and dividend-only firms that never tag the other flow have no value. |
| `shareholder_yield` | `shareholder_yield` (daily) | +1 | Trailing net payout plus net debt paydown over market capitalization. | Boudoukh Michaely Richardson and Roberts 2007 (Journal of Finance); Bradshaw Richardson and Sloan 2006 (Journal of Accounting and Economics) | published_analogue | payout_yield | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 4q/1s | eligible_with_caveat [coverage_bias]: Each payout and debt flow is read only when tagged in every quarter of the trailing year (the fiscal-year bucket falls back to the annual value) and never imputed as zero; firms missing any flow have no value. |
| `share_issuance_1y` | `share_issuance_1y` (q) | -1 | Log change in quarterly weighted-average basic shares over the same fiscal quarter a year earlier (a weighted-share analogue of split-adjusted shares outstanding). | Pontiff and Woodgate 2008 (Journal of Finance); Daniel and Titman 2006 (Journal of Finance) | published_analogue | equity_issuance | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible_with_caveat [split_basis, coverage_bias]: Fiscal fourth quarters have no value where only annual weighted shares are reported; the two counts are comparable on one filing's restated comparative or daily-bar split epochs (R1d) and labeled incomparable otherwise. |
| `share_issuance_3y` | `share_issuance_3y` (q) | -1 | Three-year log change in period-end shares outstanding. | Daniel and Titman 2006 (Journal of Finance); Pontiff and Woodgate 2008 (Journal of Finance) | published_anomaly | equity_issuance | rank_normal | unrestricted | conservative_filing_46h | 13q/0s | eligible_with_caveat [split_basis]: Period-end share counts twelve quarters apart are never restated for splits and are comparable only where daily-bar split epochs (R1d) rebase both counts to one split basis and labeled incomparable otherwise. |

## efficiency

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `asset_turnover` | `asset_turnover` (ttm) | +1 | Trailing revenue over average total assets. | Soliman 2008 (The Accounting Review); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | asset_turnover | winsor_z | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `asset_turnover_change_yoy` | `asset_turnover_change_yoy` (ttm) | +1 | Year-over-year change in revenue over average total assets. | Soliman 2008 (The Accounting Review) | published_anomaly | asset_turnover_change | winsor_z | unrestricted | conservative_filing_46h | 9q/0s | eligible |
| `dso_days` | `dso_days` (ttm) | -1 | Days sales outstanding: average receivables times 365 over trailing revenue. | Wang 2019 (Journal of Financial Economics) | published_analogue | cash_conversion_cycle | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `dio_days` | `dio_days` (ttm) | -1 | Days inventory outstanding: average inventory times 365 over trailing cost of revenue. | Wang 2019 (Journal of Financial Economics); Thomas and Zhang 2002 (Review of Accounting Studies) | published_analogue | cash_conversion_cycle | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `dpo_days` | `dpo_days` (ttm) | +1 | Days payables outstanding: average payables times 365 over trailing cost of revenue. | Wang 2019 (Journal of Financial Economics) | published_analogue | cash_conversion_cycle | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `cash_conversion_cycle` | `cash_conversion_cycle` (ttm) | -1 | DSO plus DIO less DPO in days. | Wang 2019 (Journal of Financial Economics) | published_anomaly | cash_conversion_cycle | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `sga_to_sales` | `sga_to_sales` (ttm) | two-sided | Trailing twelve-month SG&A over trailing revenue. | Eisfeldt and Papanikolaou 2013 (Journal of Finance); Novy-Marx 2013 (Journal of Financial Economics) | economic_conjecture | sga_intensity | winsor_z | unrestricted | conservative_filing_46h | 4q/0s | eligible_with_caveat [mixed_evidence, construct_deviation]: Eisfeldt and Papanikolaou capitalize SG&A into an organization-capital stock over assets within industries; a flow over sales is a conjecture. |
| `sga_growth_less_sales_growth` | `sga_growth_less_sales_growth` (q) | -1 | Year-over-year quarterly SG&A growth less year-over-year quarterly revenue growth. | Lev and Thiagarajan 1993 (Journal of Accounting Research); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | fundamental_signals | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `inventory_growth_less_sales_growth` | `inventory_growth_less_sales_growth` (q) | -1 | Year-over-year inventory growth less year-over-year quarterly revenue growth. | Lev and Thiagarajan 1993 (Journal of Accounting Research); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | fundamental_signals | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `receivables_growth_less_sales_growth` | `receivables_growth_less_sales_growth` (q) | -1 | Year-over-year receivables growth less year-over-year quarterly revenue growth. | Lev and Thiagarajan 1993 (Journal of Accounting Research); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | fundamental_signals | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `sales_growth_less_gross_profit_growth` | `sales_growth_less_gross_profit_growth` (q) | -1 | Year-over-year quarterly revenue growth less year-over-year quarterly gross-profit growth. | Lev and Thiagarajan 1993 (Journal of Accounting Research); Abarbanell and Bushee 1998 (The Accounting Review) | published_analogue | gross_margin_change | rank_normal | unrestricted | conservative_filing_46h | 5q/0s | eligible |
| `noa_turnover` | `noa_turnover` (ttm) | +1 | Trailing twelve-month revenue over net operating assets at the start of the trailing year (ATO). | Soliman 2008 (The Accounting Review); Hou Xue and Zhang 2020 (Review of Financial Studies) | published_anomaly | asset_turnover | rank_normal | guarded_in_definition | conservative_filing_46h | 5q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Net operating assets net out debt; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |
| `noa_turnover_change_yoy` | `noa_turnover_change_yoy` (ttm) | +1 | Year-over-year change in trailing revenue over opening net operating assets. | Soliman 2008 (The Accounting Review) | published_anomaly | asset_turnover_change | rank_normal | guarded_in_definition | conservative_filing_46h | 9q/0s | eligible_with_caveat [presence_rule, coverage_bias]: Net operating assets net out debt; debt is zero only for an issuer that has never tagged a mapped debt alias (a switch to an unmapped alias has no value) and a component missing beside a reported one is absent within the reported total. |

## earnings_stability

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `earnings_variability` | `earnings_variability` (ttm) | -1 | Twelve-quarter standard deviation of year-over-year trailing diluted-EPS growth. | Huang 2009 (Journal of Empirical Finance); Dichev and Tang 2009 (Journal of Accounting and Economics) | published_analogue | earnings_variability | rank_normal | unrestricted | conservative_filing_46h | 19q/0s | blocked_incomparable_origin [trailing_span_overlap, split_basis]: Its elements are trailing-twelve-month growth rates whose 365-day spans never pass the stdev_q single-quarter chain proof so every value is labeled incomparable. |
| `roe_variability_8q` | `roe_variability_8q` (q) | -1 | Eight-quarter sample standard deviation of single-quarter ROE. | Asness Frazzini and Pedersen 2019 (Review of Accounting Studies); Mohanram 2005 (Review of Accounting Studies) | published_analogue | profitability_variability | rank_normal | unrestricted | conservative_filing_46h | 9q/0s | eligible_with_caveat [fiscal_seasonality]: Fiscal seasonality in single-quarter ROE inflates the dispersion of seasonal businesses. |
| `roa_variability_8q` | `roa_variability_8q` (q) | -1 | Eight-quarter sample standard deviation of single-quarter ROA. | Mohanram 2005 (Review of Accounting Studies); Dichev and Tang 2009 (Journal of Accounting and Economics) | published_analogue | profitability_variability | rank_normal | unrestricted | conservative_filing_46h | 9q/0s | eligible_with_caveat [fiscal_seasonality]: Fiscal seasonality in single-quarter ROA inflates the dispersion of seasonal businesses. |
| `cfo_variability_8q` | `cfo_variability_8q` (q) | -1 | Eight-quarter sample standard deviation of single-quarter operating cash flow over opening total assets. | Huang 2009 (Journal of Empirical Finance) | published_analogue | cash_flow_variability | rank_normal | unrestricted | conservative_filing_46h | 9q/0s | eligible_with_caveat [fiscal_seasonality]: Fiscal seasonality in single-quarter operating cash flow inflates the dispersion of seasonal businesses. |
| `sales_growth_variability_8q` | `sales_growth_variability_8q` (q) | -1 | Eight-quarter sample standard deviation of year-over-year quarterly revenue growth. | Mohanram 2005 (Review of Accounting Studies) | published_analogue | sales_growth_variability | rank_normal | unrestricted | conservative_filing_46h | 12q/0s | eligible |

## liquidity

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `amihud_illiquidity_21d` | `amihud_illiquidity_21d` (daily, panel) | +1 | Mean absolute daily return per dollar traded (x 1e9) over the positive-volume days of the last 21 XNYS sessions. | Amihud 2002 (Journal of Financial Markets) | published_anomaly | trading_liquidity | rank_normal | unrestricted | modeled_trade_date_22h | 0q/22s | eligible_with_caveat [coverage_bias]: A line with fewer than 19 positive-volume days or observed returns in the window has no value (zero_volume_in_window or window_gaps) so the most thinly traded names of the illiquid tail are missing. |
| `turnover_21d` | `turnover_21d` (daily, panel) | -1 | Mean daily share volume over the last 21 XNYS sessions divided by the verified DEI shares outstanding. | Datar Naik and Radcliffe 1998 (Journal of Financial Markets) | published_anomaly | trading_liquidity | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 0q/21s | eligible_with_caveat [coverage_bias]: Only lines with a verified DEI share count have a value: multi-class and ADR and unlinked lines and withheld counts are missing and a window holding an exact split or stock-dividend ratio (R1d classifier) has no value. |
| `zero_trade_21d` | `zero_trade_21d` (daily, panel) | +1 | Share of the observed XNYS sessions among the last 21 with zero volume (at least 15 observed sessions). | Liu 2006 (Journal of Financial Economics) | published_analogue | zero_trading_days | rank_normal | unrestricted | modeled_trade_date_22h | 0q/21s | eligible_with_caveat [construct_deviation, vendor_zero_volume_absent]: Liu's measure is the turnover-adjusted count of zero-trading days over twelve months; here the share of zero-volume bars among the line's observed sessions of the window (a session without a vendor bar is not counted). Data support: the retained vendor file carries no zero-volume bars outside about 2016-2019 so the share is zero for every line in most formation months (ruling C-82: not registered in w1). |
| `zero_trade_252d` | `zero_trade_252d` (daily, panel) | +1 | Share of the observed XNYS sessions among the last 252 with zero volume (at least 200 observed sessions). | Liu 2006 (Journal of Financial Economics) | published_analogue | zero_trading_days | rank_normal | unrestricted | modeled_trade_date_22h | 0q/252s | eligible_with_caveat [construct_deviation, vendor_zero_volume_absent]: Liu's measure is the turnover-adjusted count of zero-trading days over twelve months; here the share of zero-volume bars among the line's observed sessions of the window (a session without a vendor bar is not counted). Data support: the retained vendor file carries no zero-volume bars outside about 2016-2019 so the share is zero for every line in most formation months (ruling C-82: not registered in w1). |
| `turnover_126d` | `turnover_126d` (daily, panel) | -1 | Mean daily share volume over the last 126 XNYS sessions (each day restated to the lag date's share basis by the vendor factor) over the vendor share count lagged 90 days (at least 100 observed sessions). | Datar Naik and Radcliffe 1998 (Journal of Financial Markets) | published_anomaly | trading_liquidity | rank_normal | unrestricted | modeled_trade_date_22h | 0q/126s | eligible_with_caveat [construct_deviation]: The denominator is the unverified vendor share count of the line's last bar at least 90 days before formation (A8 modeled lag: vendor runs start at the filing cover date) restated through the vendor adjustment factor which also carries dividends (a few tenths of a percent a quarter); an ADR line counts ADS. |
| `turnover_252d` | `turnover_252d` (daily, panel) | -1 | Mean daily share volume over the last 252 XNYS sessions (each day restated to the lag date's share basis by the vendor factor) over the vendor share count lagged 90 days (at least 200 observed sessions). | Datar Naik and Radcliffe 1998 (Journal of Financial Markets) | published_anomaly | trading_liquidity | rank_normal | unrestricted | modeled_trade_date_22h | 0q/252s | eligible_with_caveat [construct_deviation]: The denominator is the unverified vendor share count of the line's last bar at least 90 days before formation (A8 modeled lag: vendor runs start at the filing cover date) restated through the vendor adjustment factor which also carries dividends (a few tenths of a percent a quarter); an ADR line counts ADS. |
| `std_turn_126d` | `std_turn_126d` (daily, panel) | -1 | Standard deviation of daily turnover (volume restated to the lag date's share basis over the vendor share count lagged 90 days) over the last 126 XNYS sessions (at least 100 observed sessions). | Chordia Subrahmanyam and Anshuman 2001 (Journal of Financial Economics) | published_anomaly | turnover_volatility | rank_normal | unrestricted | modeled_trade_date_22h | 0q/126s | eligible_with_caveat [construct_deviation]: The denominator is the unverified vendor share count of the line's last bar at least 90 days before formation (A8 modeled lag) restated through the vendor adjustment factor; an ADR line counts ADS. |
| `std_dvol_126d` | `std_dvol_126d` (daily, panel) | -1 | Standard deviation of daily dollar volume (close times volume) over the last 126 XNYS sessions (at least 100 observed sessions). | Chordia Subrahmanyam and Anshuman 2001 (Journal of Financial Economics) | published_anomaly | turnover_volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/126s | eligible |
| `ami_126d` | `ami_126d` (daily, panel) | +1 | One million times the mean absolute daily return per dollar traded (close times volume) over the positive-volume daily returns of the last 126 XNYS sessions (at least 100). | Amihud 2002 (Journal of Financial Markets) | published_anomaly | trading_liquidity | rank_normal | unrestricted | modeled_trade_date_22h | 0q/127s | eligible_with_caveat [coverage_bias]: A line with fewer than 100 positive-volume daily returns in the window has no value so the most thinly traded names of the illiquid tail are missing. |
| `ami_252d` | `ami_252d` (daily, panel) | +1 | One million times the mean absolute daily return per dollar traded (close times volume) over the positive-volume daily returns of the last 252 XNYS sessions (at least 200). | Amihud 2002 (Journal of Financial Markets) | published_anomaly | trading_liquidity | rank_normal | unrestricted | modeled_trade_date_22h | 0q/253s | eligible_with_caveat [coverage_bias]: A line with fewer than 200 positive-volume daily returns in the window has no value so the most thinly traded names of the illiquid tail are missing. |
| `bidask_cs_21d` | `bidask_cs_21d` (daily, panel) | +1 | Corwin-Schultz high-low spread: the mean over the two-session pairs of the last 21 XNYS sessions of the overnight-adjusted estimate on vendor-factor-adjusted highs and lows with negative estimates set to zero (at least 15 pairs). | Amihud and Mendelson 1986 (Journal of Financial Economics); Corwin and Schultz 2012 (Journal of Finance) | published_analogue | bid_ask_spread | rank_normal | unrestricted | modeled_trade_date_22h | 0q/22s | eligible |
| `bidask_ar_21d` | `bidask_ar_21d` (daily, panel) | +1 | Abdi-Ranaldo close-high-low spread: the square root of the positive part of four times the mean over the two-session pairs of the last 21 XNYS sessions of the log close minus its log mid-range times the log close minus the next session's log mid-range (at least 15 pairs). | Amihud and Mendelson 1986 (Journal of Financial Economics); Abdi and Ranaldo 2017 (Review of Financial Studies) | published_analogue | bid_ask_spread | rank_normal | unrestricted | modeled_trade_date_22h | 0q/22s | eligible |
| `dolvol_126d` | `dolvol_126d` (daily, panel) | -1 | Mean daily dollar volume (close times volume) over the last 126 XNYS sessions (at least 100 observed sessions; log by the transform). | Brennan Chordia and Subrahmanyam 1998 (Journal of Financial Economics) | published_anomaly | dollar_volume | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/126s | eligible |
| `price_delay_52w` | `price_delay_52w` (daily, panel) | +1 | Hou-Moskowitz delay D1: one minus the R-squared of weekly returns on the weekly equal-weighted market over the R-squared with four weekly market lags added over the 52 complete weeks to formation (at least 40 weeks). | Hou and Moskowitz 2005 (Review of Financial Studies) | published_anomaly | price_delay | winsor_z | unrestricted | modeled_trade_date_22h | 0q/280s | eligible_with_caveat [construct_deviation]: Weekly returns from the vendor-factor-repaired adjusted close and the compounded equal-weighted P3 market (daily bar returns winsorized at 50 percent) where Hou and Moskowitz use the CRSP value-weight market; a week counts only when it is complete before the formation session. |

## ownership

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `io_ratio_13f` | `io_ratio_13f` (13f_quarter, ownership) | +1 | 13F common shares held by all managers in the owner's mapped CUSIPs at the latest report quarter past its filing deadline over the line's verified DEI share count at that quarter end. | Gompers and Metrick 2001 (Quarterly Journal of Economics); Nagel 2005 (Journal of Financial Economics) | published_analogue | institutional_ownership | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 1q/0s | eligible_with_caveat [coverage_bias, io_above_one]: CUSIP-to-owner identity is reconstructed and modeled not certified (a CUSIP mapped through a current snapshot is flagged cusip_survivor_conditioned); visible from the quarter's filing deadline (quarter end plus 45 days plus 46 hours) at the earliest; only verified DEI share counts give a value (multi-class and unlinked lines have none); IO above one is kept and counted. |
| `io_change_13f` | `io_change_13f` (13f_quarter, ownership) | +1 | Change in 13F institutional ownership from the previous report quarter to the latest one (both as known at the same cutoff and each over its own quarter-end verified share count). | Nofsinger and Sias 1999 (Journal of Finance); Sias Starks and Titman 2006 (Journal of Business) | published_analogue | institutional_demand | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 2q/0s | eligible_with_caveat [coverage_bias, io_above_one]: CUSIP-to-owner identity is reconstructed and modeled not certified (a CUSIP mapped through a current snapshot is flagged cusip_survivor_conditioned); visible from the quarter's filing deadline (quarter end plus 45 days plus 46 hours) at the earliest; only verified DEI share counts give a value and both quarters must have one; IO above one is kept and counted. |
| `breadth_change_13f` | `breadth_change_13f` (13f_quarter, ownership) | +1 | Change in the number of 13F managers holding the stock from the previous report quarter to the latest one over the managers with a visible full filing in both quarters (counting only those managers). | Chen Hong and Stein 2002 (Journal of Financial Economics) | published_anomaly | ownership_breadth | rank_normal | unrestricted | conservative_filing_46h | 2q/0s | eligible_with_caveat [coverage_bias]: CUSIP-to-owner identity is reconstructed and modeled not certified (a CUSIP mapped through a current snapshot is flagged cusip_survivor_conditioned); visible from the quarter's filing deadline (quarter end plus 45 days plus 46 hours) at the earliest; a value needs the stock held in both quarters. |
| `short_interest_ratio` | `short_interest_ratio` (short_interest, ownership) | -1 | Latest published FINRA short interest over the line's verified DEI share count at the last session on or before the settlement date. | Asquith Pathak and Ritter 2005 (Journal of Financial Economics); Boehmer Huszar and Jordan 2010 (Journal of Financial Economics) | published_anomaly | short_interest | rank_normal | unrestricted | finra_publication_modeled | 0q/0s | eligible_with_caveat [coverage_bias]: Visible from a modeled FINRA publication clock (settlement plus 8 business days at 22:00 UTC) never the settlement date; symbol-date line identity; only verified DEI share counts give a value (unlinked and multi-class lines have none). |
| `days_to_cover_si` | `days_to_cover_si` (short_interest, ownership) | -1 | Latest published FINRA short interest over FINRA's average daily volume of the reporting period. | Hong Li Ni Scheinkman and Yan 2015 (NBER working paper) | published_analogue | short_interest | rank_normal | unrestricted | finra_publication_modeled | 0q/0s | eligible_with_caveat [coverage_bias]: Visible from a modeled FINRA publication clock (settlement plus 8 business days at 22:00 UTC) never the settlement date; symbol-date line identity; no value when a split falls inside the reporting period (short interest and volume on mixed share bases). |

## event_timing

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `days_since_announcement` | `days_since_announcement` (event, event) | +1 | Calendar days from the latest visible earnings announcement session to the formation date (an event at most 200 days old). | Frazzini and Lamont 2007 (NBER working paper); Barber De George Lehavy and Trueman 2013 (Journal of Financial Economics) | published_analogue | earnings_announcement_premium | rank_normal | unrestricted | max_filing_46h_trade_date_22h | 0q/0s | eligible_with_caveat [construct_deviation, coverage_bias]: The published premium is keyed to the predicted announcement month; days since the last announcement is a monotone proxy that ignores fiscal-calendar shifts and stops being monotone beyond one quarter (late filers); events are keyed by original 10-K/10-Q periods and only linked primary lines carry a value. |

## size

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `market_cap` | `market_cap` (daily) | -1 | Price times point-in-time shares outstanding. | Banz 1981 (Journal of Financial Economics); Fama and French 1992 (Journal of Finance) | published_anomaly | size | log_winsor_z | positive_value_required | max_filing_46h_trade_date_22h | 0q/1s | eligible |
| `dollar_volume_20d` | `dollar_volume_20d` (daily) | -1 | Twenty-day average daily dollar trading volume. | Brennan Chordia and Subrahmanyam 1998 (Journal of Financial Economics); Amihud 2002 (Journal of Financial Markets) | published_anomaly | trading_liquidity | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/20s | eligible |
| `prc_log` | `prc_log` (daily, panel) | -1 | Close at the formation session (log by the transform). | Blume and Husic 1973 (Journal of Finance); Miller and Scholes 1982 (Journal of Business) | published_anomaly | share_price | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/1s | eligible |
| `me_line_log` | `me_line_log` (daily, panel) | -1 | Line market value: the close at the formation session times the vendor share count lagged 90 days restated to the formation session's share basis (log by the transform). | Banz 1981 (Journal of Financial Economics); Fama and French 1992 (Journal of Finance) | published_anomaly | size | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/63s | eligible_with_caveat [construct_deviation]: One price line's market value from its unverified vendor share count (not the issuer total across share classes); the count is the line's last bar at least 90 days before formation (A8 modeled lag; me_basis vendor_shares_lag90) restated through the vendor factor. |

## momentum

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `momentum_12_1` | `momentum_12_1` (daily) | +1 | Total return from 252 to 21 trading days before formation (skips the latest month). | Jegadeesh and Titman 1993 (Journal of Finance); Carhart 1997 (Journal of Finance) | published_anomaly | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/253s | eligible |
| `total_return_12m` | `total_return_12m` (daily) | +1 | Total return over the last 252 trading days. | Jegadeesh and Titman 1993 (Journal of Finance) | published_analogue | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/253s | eligible |
| `total_return_6m` | `total_return_6m` (daily) | +1 | Total return over the last 126 trading days. | Jegadeesh and Titman 1993 (Journal of Finance) | published_anomaly | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/127s | eligible |
| `total_return_3m` | `total_return_3m` (daily) | +1 | Total return over the last 63 trading days. | Jegadeesh and Titman 1993 (Journal of Finance) | published_analogue | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/64s | eligible |
| `pct_from_high_252d` | `pct_from_high_252d` (daily, panel) | +1 | Adjusted close over its highest adjusted close in the last 252 XNYS sessions minus one. | George and Hwang 2004 (Journal of Finance) | published_anomaly | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/252s | eligible |
| `ret_12_1` | `ret_12_1` (daily, panel) | +1 | Cumulative return over months t-12 to t-2 (t the holding month): the repaired adjusted close at the last observed session of formation month F-1 over that of F-12; at least 200 observed sessions. | Jegadeesh and Titman 1993 (Journal of Finance); Carhart 1997 (Journal of Finance) | published_anomaly | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/252s | eligible |
| `ret_6_1` | `ret_6_1` (daily, panel) | +1 | Cumulative return over months t-6 to t-2: the repaired adjusted close at the last observed session of month F-1 over that of F-6; at least 100 observed sessions. | Jegadeesh and Titman 1993 (Journal of Finance) | published_anomaly | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/126s | eligible |
| `ret_9_1` | `ret_9_1` (daily, panel) | +1 | Cumulative return over months t-9 to t-2: the repaired adjusted close at the last observed session of month F-1 over that of F-9; at least 150 observed sessions. | Jegadeesh and Titman 1993 (Journal of Finance) | published_anomaly | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/189s | eligible |
| `ret_12_7` | `ret_12_7` (daily, panel) | +1 | Cumulative return over months t-12 to t-7: the repaired adjusted close at the last observed session of month F-6 over that of F-12; at least 100 observed sessions. | Novy-Marx 2012 (Journal of Financial Economics) | published_anomaly | intermediate_momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/252s | eligible |
| `chmom` | `chmom` (daily, panel) | -1 | Change in six-month momentum: the months t-6 to t-2 return minus the same return six months earlier (months t-12 to t-8: formation months F-5 to F-1 minus F-11 to F-7); at least 200 observed sessions over formation months F-11 to F-1. | Gettleman and Marks 2006 (working paper); Green Hand and Zhang 2017 (Review of Financial Studies) | published_analogue | momentum_change | winsor_z | unrestricted | modeled_trade_date_22h | 0q/252s | eligible |
| `frog_in_pan` | `frog_in_pan` (daily, panel) | -1 | Information discreteness: the sign of the months t-12 to t-2 return times the share of negative minus the share of positive daily returns over those months; at least 200 daily returns. | Da Gurun and Warachka 2014 (Review of Financial Studies) | published_analogue | information_discreteness | winsor_z | unrestricted | modeled_trade_date_22h | 0q/252s | eligible |
| `seas_1_1an` | `seas_1_1an` (daily, panel) | +1 | Same-calendar-month return one year ago: the return of month t-12 (formation month F-11) with at least 15 observed sessions in that month. | Heston and Sadka 2008 (Journal of Financial Economics) | published_anomaly | return_seasonality | winsor_z | unrestricted | modeled_trade_date_22h | 0q/252s | eligible |
| `seas_2_5an` | `seas_2_5an` (daily, panel) | +1 | Mean same-calendar-month return of years 2 to 5 (formation months F-23 F-35 F-47 and F-59) with at least 3 of the 4 months present (each with at least 15 observed sessions). | Heston and Sadka 2008 (Journal of Financial Economics) | published_anomaly | return_seasonality | winsor_z | unrestricted | modeled_trade_date_22h | 0q/1260s | eligible |
| `prc_highprc_252d` | `prc_highprc_252d` (daily, panel) | +1 | Repaired adjusted close at the formation session over its highest repaired adjusted close over the last 252 XNYS sessions (at least 200 observed sessions). | George and Hwang 2004 (Journal of Finance) | published_anomaly | momentum | winsor_z | unrestricted | modeled_trade_date_22h | 0q/252s | eligible |

## reversal

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `total_return_1m` | `total_return_1m` (daily) | -1 | Total return over the last 21 trading days. | Jegadeesh 1990 (Journal of Finance); Lehmann 1990 (Quarterly Journal of Economics) | published_anomaly | short_term_reversal | winsor_z | unrestricted | modeled_trade_date_22h | 0q/22s | eligible |
| `runup_m21_m2` | `runup_m21_m2` (event, event) | two-sided | Market-adjusted return (the line's one-session return less the equal-weighted bar-return market) summed over sessions E-21 to E-2 before the latest visible earnings announcement session E. | Aboody Lehavy and Trueman 2010 (Review of Accounting Studies) | published_analogue | earnings_announcement_runup | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 0q/21s | eligible_with_caveat [mixed_evidence, coverage_bias]: Published evidence disagrees on the sign after a pre-announcement run-up (continuation into the announcement versus a reversal after it) so it is tested two-sided; events are keyed by original 10-K/10-Q periods and only linked primary lines carry a value. |
| `ret_36_13` | `ret_36_13` (daily, panel) | -1 | Cumulative return over months t-36 to t-13: the repaired adjusted close at the last observed session of month F-12 over that of F-36; at least 400 observed sessions. | De Bondt and Thaler 1985 (Journal of Finance) | published_anomaly | long_term_reversal | winsor_z | unrestricted | modeled_trade_date_22h | 0q/756s | eligible |
| `ret_60_13` | `ret_60_13` (daily, panel) | -1 | Cumulative return over months t-60 to t-13: the repaired adjusted close at the last observed session of month F-12 over that of F-60; at least 800 observed sessions. | De Bondt and Thaler 1985 (Journal of Finance) | published_anomaly | long_term_reversal | winsor_z | unrestricted | modeled_trade_date_22h | 0q/1260s | eligible |
| `ret_1_0` | `ret_1_0` (daily, panel) | -1 | Return of the formation month: the repaired adjusted close at the formation session over that at the last observed session of month F-1; at least 15 observed sessions in the month. | Jegadeesh 1990 (Journal of Finance); Lehmann 1990 (Quarterly Journal of Economics) | published_anomaly | short_term_reversal | winsor_z | unrestricted | modeled_trade_date_22h | 0q/22s | eligible |

## volatility

| feature | source | sign | definition | reference | evidence | family | transform | domain | clock | history | admission |
|---|---|:-:|---|---|---|---|---|---|---|---|---|
| `realized_vol_60d` | `realized_vol_60d` (daily) | -1 | Annualized 60-day realized volatility of daily log returns. | Ang Hodrick Xing and Zhang 2006 (Journal of Finance); Baker Bradley and Wurgler 2011 (Financial Analysts Journal) | published_analogue | volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/61s | eligible |
| `realized_vol_252d` | `realized_vol_252d` (daily) | -1 | Annualized 252-day realized volatility of daily log returns. | Baker Bradley and Wurgler 2011 (Financial Analysts Journal); Ang Hodrick Xing and Zhang 2006 (Journal of Finance) | published_analogue | volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/253s | eligible |
| `max_daily_return_21d` | `max_daily_return_21d` (daily, panel) | -1 | Largest daily return over the last 21 XNYS sessions. | Bali Cakici and Whitelaw 2011 (Journal of Financial Economics) | published_anomaly | volatility | winsor_z | unrestricted | modeled_trade_date_22h | 0q/22s | eligible |
| `downside_deviation_60d` | `downside_deviation_60d` (daily, panel) | two-sided | Annualized zero-target semideviation (root mean square of the negative daily returns) over the last 60 XNYS sessions. | Ang Chen and Xing 2006 (Review of Financial Studies); Ang Hodrick Xing and Zhang 2006 (Journal of Finance) | published_analogue | volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/61s | eligible_with_caveat [mixed_evidence]: Published evidence disagrees on the sign: Ang Chen and Xing price downside beta positively while the low-volatility literature prices total volatility negatively; tested two-sided. |
| `beta_mkt_252d` | `beta_mkt_252d` (252d, factor_exposure) | -1 | OLS slope of the line's daily excess return on the daily value-weighted research-universe market excess return over the trailing 252 sessions (at least 200 observations; P4 research_factor_exposures). | Black Jensen and Scholes 1972 (Studies in the Theory of Capital Markets); Frazzini and Pedersen 2014 (Journal of Financial Economics) | published_anomaly | market_beta | winsor_z | unrestricted | max_filing_46h_trade_date_22h | 0q/253s | eligible_with_caveat [construct_deviation]: The market is the research universe weighted by verified DEI caps only (not CRSP) with the risk-free rate from FRED DTB3 (current vintage) when cached; plain OLS without the Frazzini-Pedersen shrinkage and correlation split; the daily market starts at the first formation so the first year of formations is burn-in (insufficient_obs). |
| `ivol_252d` | `ivol_252d` (252d, factor_exposure) | -1 | Annualized standard deviation of the residuals of the 252-session one-factor market model of daily excess returns (at least 200 observations; P4 research_factor_exposures). | Ang Hodrick Xing and Zhang 2006 (Journal of Finance); Ang Hodrick Xing and Zhang 2009 (Journal of Financial Economics) | published_anomaly | idiosyncratic_volatility | log_winsor_z | positive_value_required | max_filing_46h_trade_date_22h | 0q/253s | eligible_with_caveat [construct_deviation]: Residuals of a one-factor market model over 252 sessions where the published measure uses Fama-French three-factor residuals over one month; the market is the verified-DEI-cap value-weighted research universe; the first year of formations is burn-in (insufficient_obs). |
| `rvol_21d` | `rvol_21d` (daily, panel) | -1 | Sample standard deviation of daily log returns over the last 21 XNYS sessions (at least 15 returns). | Ang Hodrick Xing and Zhang 2006 (Journal of Finance) | published_analogue | volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/22s | eligible |
| `rvol_252d` | `rvol_252d` (daily, panel) | -1 | Sample standard deviation of daily log returns over the last 252 XNYS sessions (at least 200 returns). | Ang Hodrick Xing and Zhang 2006 (Journal of Finance); Baker Bradley and Wurgler 2011 (Financial Analysts Journal) | published_analogue | volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/253s | eligible |
| `rmax5_21d` | `rmax5_21d` (daily, panel) | -1 | Mean of the five largest daily returns over the last 21 XNYS sessions (at least 15 returns). | Bali Cakici and Whitelaw 2011 (Journal of Financial Economics) | published_anomaly | volatility | winsor_z | unrestricted | modeled_trade_date_22h | 0q/22s | eligible |
| `rmax1_21d` | `rmax1_21d` (daily, panel) | -1 | Largest daily return over the last 21 XNYS sessions (at least 15 returns). | Bali Cakici and Whitelaw 2011 (Journal of Financial Economics) | published_anomaly | volatility | winsor_z | unrestricted | modeled_trade_date_22h | 0q/22s | eligible |
| `rskew_252d` | `rskew_252d` (daily, panel) | -1 | Sample skewness of daily returns over the last 252 XNYS sessions (at least 200 returns). | Boyer Mitton and Vorkink 2010 (Review of Financial Studies); Amaya Christoffersen Jacobs and Vasquez 2015 (Journal of Financial Economics) | published_analogue | skewness | winsor_z | unrestricted | modeled_trade_date_22h | 0q/253s | eligible |
| `beta_ew_252d` | `beta_ew_252d` (daily, panel) | -1 | OLS slope of daily returns on the equal-weighted market (the P3 sealed convention: the mean of the bar returns of the prior formation's spine lines each winsorized at 50 percent per ruling C-55) over the last 252 XNYS sessions (at least 200 pairs). | Black Jensen and Scholes 1972 (Studies in the Theory of Capital Markets); Frazzini and Pedersen 2014 (Journal of Financial Economics) | published_analogue | market_beta | winsor_z | unrestricted | modeled_trade_date_22h | 0q/253s | eligible_with_caveat [construct_deviation]: Reported only (ruling C-24 and policy v4 reported_only): the value-weight market beta (beta_mkt_252d in wave w4_events) is the pre-registered tested hypothesis and this equal-weight-market twin is evaluated and reported but never gated; no risk-free rate is subtracted (a constant shift leaves the slope unchanged). |
| `ivol_ew_252d` | `ivol_ew_252d` (daily, panel) | -1 | Residual standard deviation of the regression of daily returns on the equal-weighted market (P3 convention with bar returns winsorized at 50 percent) over the last 252 XNYS sessions (at least 200 pairs). | Ang Hodrick Xing and Zhang 2006 (Journal of Finance); Ang Hodrick Xing and Zhang 2009 (Journal of Financial Economics) | published_analogue | idiosyncratic_volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/253s | eligible_with_caveat [construct_deviation]: Reported only (ruling C-24 and policy v4 reported_only): the Fama-French-residual idiosyncratic volatility is the pre-registered tested hypothesis and this equal-weight-market CAPM twin is evaluated and reported but never gated. |
| `ivol_ew_21d` | `ivol_ew_21d` (daily, panel) | -1 | Residual standard deviation of the regression of daily returns on the equal-weighted market (P3 convention with bar returns winsorized at 50 percent) over the last 21 XNYS sessions (at least 15 pairs). | Ang Hodrick Xing and Zhang 2006 (Journal of Finance); Ang Hodrick Xing and Zhang 2009 (Journal of Financial Economics) | published_analogue | idiosyncratic_volatility | log_winsor_z | positive_value_required | modeled_trade_date_22h | 0q/22s | eligible_with_caveat [construct_deviation]: Reported only (ruling C-24 and policy v4 reported_only): the Fama-French-residual idiosyncratic volatility is the pre-registered tested hypothesis and this one-month equal-weight-market CAPM twin is evaluated and reported but never gated (the published measure uses one month of three-factor residuals). |
| `beta_dimson_252d` | `beta_dimson_252d` (daily, panel) | -1 | Dimson beta: the sum of the slopes of one regression of daily returns on the equal-weighted market at t-1 and t and t+1 over the last 252 XNYS sessions (pairs whose t+1 is after the formation session dropped; at least 200 pairs). | Dimson 1979 (Journal of Financial Economics); Frazzini and Pedersen 2014 (Journal of Financial Economics) | published_analogue | market_beta | winsor_z | unrestricted | modeled_trade_date_22h | 0q/253s | eligible_with_caveat [construct_deviation]: The market is the equal-weighted P3 convention (bar returns of the prior formation's spine lines each winsorized at 50 percent) where the published constructions use the value-weight market; no risk-free rate is subtracted. |
| `beta_down_252d` | `beta_down_252d` (daily, panel) | -1 | OLS slope of daily returns on the equal-weighted market over the days the market fell in the last 252 XNYS sessions (at least 100 such days). | Ang Chen and Xing 2006 (Review of Financial Studies); Jensen Kelly and Pedersen 2023 (Journal of Finance) | published_analogue | downside_beta | winsor_z | unrestricted | modeled_trade_date_22h | 0q/253s | eligible_with_caveat [construct_deviation]: The market is the equal-weighted P3 convention (bar returns of the prior formation's spine lines each winsorized at 50 percent) where the published constructions use the value-weight market; no risk-free rate is subtracted. |
| `coskew_252d` | `coskew_252d` (daily, panel) | -1 | Harvey-Siddique coskewness: the mean of the CAPM residual times the squared demeaned equal-weighted market over the root mean squared residual times the mean squared demeaned market over the last 252 XNYS sessions (at least 200 pairs). | Harvey and Siddique 2000 (Journal of Finance) | published_anomaly | coskewness | winsor_z | unrestricted | modeled_trade_date_22h | 0q/253s | eligible_with_caveat [construct_deviation]: Daily returns on the equal-weighted P3 market (bar returns winsorized at 50 percent) over 252 sessions where Harvey and Siddique use monthly returns on the value-weight market over five years. |
| `beta_bab_1260d` | `beta_bab_1260d` (daily, panel) | -1 | Frazzini-Pedersen beta: the correlation of overlapping three-session log returns with the equal-weighted market over the 60 months to formation (at least 750) times the ratio of the line's to the market's daily log-return volatility over the last 252 XNYS sessions (at least 120 line returns). | Frazzini and Pedersen 2014 (Journal of Financial Economics) | published_anomaly | market_beta | winsor_z | unrestricted | modeled_trade_date_22h | 0q/1260s | eligible_with_caveat [construct_deviation]: The market is the equal-weighted P3 convention (bar returns winsorized at 50 percent) where Frazzini and Pedersen use the value-weight market; the beta is not shrunk toward one (a rank-preserving affine map that leaves every tested variant unchanged); the correlation window is 60 calendar months rather than 1260 sessions. |

## Hypothesis families with more than one member

| family | members |
|---|---|
| accruals | `total_accruals`, `percent_accruals`, `working_capital_accruals`, `rsst_accruals` |
| asset_growth | `asset_growth`, `total_assets_cagr_3y` |
| asset_turnover | `asset_turnover`, `noa_turnover` |
| asset_turnover_change | `asset_turnover_change_yoy`, `noa_turnover_change_yoy` |
| bid_ask_spread | `bidask_cs_21d`, `bidask_ar_21d` |
| book_equity_growth | `book_value_growth_yoy`, `common_equity_cagr_3y` |
| book_leverage | `debt_to_equity`, `debt_to_assets`, `long_term_debt_to_assets`, `net_debt_to_book_equity` |
| capex_growth | `capex_growth_yoy`, `capex_q_growth_yoy`, `capex_q_growth_qoq`, `capex_growth_2y`, `capex_growth_3y` |
| capex_intensity | `capex_to_depreciation`, `capex_to_sales`, `capex_to_assets` |
| cash_conversion_cycle | `dso_days`, `dio_days`, `dpo_days`, `cash_conversion_cycle` |
| cash_flow_growth_1y | `cfo_growth_yoy`, `fcf_growth_yoy`, `cfo_q_growth_yoy`, `fcf_q_growth_yoy` |
| cash_flow_growth_multi_year | `cfo_cagr_3y`, `fcf_cagr_3y` |
| cash_flow_growth_sequential | `cfo_q_growth_qoq`, `fcf_q_growth_qoq` |
| cash_flow_to_price | `fcf_yield`, `cfo_to_price` |
| cash_holdings | `cash_ratio`, `cash_to_assets` |
| cash_profitability | `cash_profitability`, `cfo_to_assets`, `cfo_to_assets_q`, `fcf_to_assets` |
| debt_service | `net_debt_ebitda`, `interest_coverage` |
| distress_risk | `altman_z_book`, `altman_z`, `ohlson_o` |
| earnings_growth_1y | `operating_income_growth_yoy`, `net_income_growth_yoy`, `eps_diluted_growth_yoy` |
| earnings_surprise | `operating_income_q_growth_yoy`, `net_income_q_growth_yoy`, `eps_diluted_q_growth_yoy`, `eps_basic_q_growth_yoy`, `sue_ni`, `earnings_surprise_to_market`, `ear_m1p1`, `sue_ni_event` |
| enterprise_value_yield | `gross_profit_to_ev`, `cfo_to_ev`, `ebit_to_ev`, `sales_to_ev`, `ebitda_to_ev` |
| equity_issuance | `net_equity_issuance`, `shares_growth_yoy`, `buyback_yield`, `share_issuance_1y`, `share_issuance_3y` |
| fundamental_signals | `sga_growth_less_sales_growth`, `inventory_growth_less_sales_growth`, `receivables_growth_less_sales_growth` |
| gross_margin | `gross_margin`, `gross_margin_q` |
| gross_margin_change | `gross_margin_change_yoy`, `gross_margin_q_change_yoy`, `sales_growth_less_gross_profit_growth` |
| gross_profit_growth | `gross_profit_growth_yoy`, `gross_profit_q_growth_yoy` |
| gross_profitability | `gross_profitability`, `gross_profitability_q` |
| idiosyncratic_volatility | `ivol_252d`, `ivol_ew_252d`, `ivol_ew_21d` |
| liquidity | `current_ratio`, `quick_ratio` |
| long_term_reversal | `ret_36_13`, `ret_60_13` |
| margin_acceleration | `gross_margin_q_change_yoy_accel`, `operating_margin_q_change_yoy_accel` |
| market_beta | `beta_mkt_252d`, `beta_ew_252d`, `beta_dimson_252d`, `beta_bab_1260d` |
| market_leverage | `debt_to_market`, `assets_to_market` |
| momentum | `momentum_12_1`, `total_return_12m`, `total_return_6m`, `total_return_3m`, `pct_from_high_252d`, `ret_12_1`, `ret_6_1`, `ret_9_1`, `prc_highprc_252d` |
| net_margin | `net_margin`, `net_margin_q` |
| net_margin_change | `net_margin_change_yoy`, `net_margin_q_change_yoy` |
| operating_margin | `operating_margin`, `ebitda_margin`, `operating_margin_q` |
| operating_margin_change | `operating_margin_change_yoy`, `operating_margin_q_change_yoy` |
| operating_profitability | `operating_profitability`, `operating_profitability_q` |
| payout_yield | `net_payout_yield`, `total_payout_yield`, `shareholder_yield` |
| piotroski_f_score | `piotroski_f`, `piotroski_f_cash_issuance` |
| profit_growth_multi_year | `eps_cagr_3y`, `gross_profit_cagr_3y`, `operating_income_cagr_3y`, `ebitda_cagr_3y` |
| profit_growth_sequential | `eps_diluted_growth_qoq`, `gross_profit_q_growth_qoq`, `operating_income_q_growth_qoq`, `net_income_q_growth_qoq`, `eps_diluted_q_growth_qoq`, `eps_basic_q_growth_qoq` |
| profitability_change | `operating_profitability_change_yoy`, `roe_change_yoy`, `roe_q_change_yoy`, `roa_q_change_yoy` |
| profitability_variability | `roe_variability_8q`, `roa_variability_8q` |
| rd_growth | `rd_expense_growth_yoy`, `rd_expense_q_growth_yoy`, `rd_expense_q_growth_qoq` |
| rd_intensity | `rd_intensity_sales`, `rd_to_assets` |
| return_on_assets | `roa`, `roa_q` |
| return_on_equity | `roe`, `roe_q` |
| return_on_invested_capital | `roic`, `roic_ex_goodwill` |
| return_on_net_operating_assets | `rnoa_q`, `rnoa` |
| return_seasonality | `seas_1_1an`, `seas_2_5an` |
| revenue_growth_sequential | `revenue_growth_qoq`, `revenue_q_growth_qoq` |
| revenue_surprise | `revenue_q_growth_yoy`, `sue_revenue` |
| short_interest | `short_interest_ratio`, `days_to_cover_si` |
| short_term_reversal | `total_return_1m`, `ret_1_0` |
| size | `market_cap`, `me_line_log` |
| trading_liquidity | `dollar_volume_20d`, `amihud_illiquidity_21d`, `turnover_21d`, `turnover_126d`, `turnover_252d`, `ami_126d`, `ami_252d` |
| turnover_volatility | `std_turn_126d`, `std_dvol_126d` |
| volatility | `realized_vol_60d`, `realized_vol_252d`, `max_daily_return_21d`, `downside_deviation_60d`, `rvol_21d`, `rvol_252d`, `rmax5_21d`, `rmax1_21d` |
| zero_trading_days | `zero_trade_21d`, `zero_trade_252d` |

## Domain rules

| rule | meaning |
|---|---|
| `unrestricted` | every finite value is in the hypothesis domain |
| `guarded_in_definition` | the definition itself yields no value outside the domain (a non-positive opening balance or endpoint has no value); nothing further to enforce |
| `positive_value_required` | values at or below zero are out of the domain (log-scaled levels) |
| `positive_denominator_required` | out of the domain when the named denominator is at or below zero or missing: a non-positive denominator flips the ratio's meaning |
| `negative_book_excluded` | out of the domain when book equity (the named operand, else the value) is at or below zero, the Fama-French convention |
| `loss_firms_separate` | a value whose earnings or cash-flow numerator (the named operand, else the value) is at or below zero leaves the ranked domain and is carried as a separate loss indicator (Fama-French 1992 E(+)/P) |
| `zero_payer_separate` | a zero value (a non-payer) leaves the ranked domain and is carried as a separate indicator; the zero-dividend puzzle makes the relation U-shaped |

## Caveat codes

| code | meaning |
|---|---|
| `sign_flip` | a non-positive denominator or book value inverts the ratio's meaning |
| `fiscal_seasonality` | a single-quarter level or dispersion carries fiscal seasonality |
| `sequential_quarter` | adjacent fiscal quarters differ in season and length |
| `split_basis` | a per-share or share-count comparison or window: comparable only on one filing's clock or a split basis proven from daily bars (R1d), so the research gate admits it row by row |
| `trailing_span_overlap` | trailing-twelve-month spans never form a single-quarter chain |
| `non_monotone` | the published relation is U-shaped or holds only within a subgroup |
| `mixed_evidence` | published evidence disagrees on the sign; the hypothesis is two-sided |
| `coverage_bias` | an input is missing for a non-random group of filers |
| `presence_rule` | a value can rest on a zero imputed from a balance's absence under a presence guard: debt or inventory only when the issuer has never tagged a mapped alias up to that quarter; a switch to an unmapped alias has no value (flows are never imputed: an absent discrete quarter cannot prove absence from a year-to-date or annual fact) |
| `unguarded_zero` | a missing optional balance (preferred stock, minority interest, goodwill, other intangibles) is read as zero with no presence guard |
| `construct_deviation` | the definition deviates from the published construction (see the note) |
| `filing_clock_lag` | the filing clock trails the market's first information (the earnings release) |
| `io_above_one` | 13F shares above the verified share count (double counting, lending, stale counts) are kept, never clipped, and counted per formation |
| `duplicate_hypothesis` | a second construction of a cataloged hypothesis (DUPLICATE_HYPOTHESES names the primary row); cataloged for diagnostics, never tested beside the primary |
| `vendor_zero_volume_absent` | the retained vendor bar file carries no zero-volume bars outside about 2016-2019, so a zero-trading share is zero for every line in most months and cannot measure what it claims (ruling C-82: not registered in w1 until a volume source has them) |

## Research metadata

`population` names the firms a hypothesis is defined on (coverage is measured against it); `evidence_class` is derived: `replication` for a published anomaly or analogue with a pre-registered sign, `discovery` for an economic conjecture or a two-sided hypothesis; `publication_year` is the year the reference cites for the anomaly's first publication: the earliest cited year, except for the rows an explicit override list names (a later cited year; `altman_z` 1998, `altman_z_book` 1998, `ohlson_o` 1998) (published rows only); `jkp_theme` is the Jensen-Kelly-Pedersen (2023) theme cluster of the JKP characteristic measuring the same construct, else the theme the construct belongs to, else `none`; `wave` is the pre-registration wave whose frozen catalog digest evaluates the row.

| population | meaning |
|---|---|
| `all` | every firm of the research universe |
| `rd_reporters` | firms that report research and development expense (a missing R&D tag is never read as zero) |
| `dividend_payers` | firms that pay common dividends (a non-payer that never tags a dividend has no value) |
| `inventory_holders` | firms that report inventory (no presence-rule zero is read) |
| `advertising_reporters` | firms that report advertising expense |
| `interest_payers` | firms that report interest expense (a firm without debt has no coverage ratio) |

| evidence class | meaning |
|---|---|
| `replication` | a published anomaly or published analogue with a pre-registered sign (one-sided test) |
| `discovery` | an economic conjecture, or a two-sided hypothesis with no pre-registered sign |

| wave | meaning |
|---|---|
| `w0_existing` | the catalog rows that existed before the tier-1 v2 waves (R1a-R1b, CB1) |
| `w1_price` | price and friction natives computed from the retained daily bars |
| `w2_fund_a` | accounting characteristics, fundamental batch A |
| `w3_compositions` | market-scaled compositions and market-dependent scores |
| `w4_events` | event features and pinned research-store sources (P3 earnings events, P4 factor exposures, EDGAR filing events) |
| `w5_ownership` | 13F institutional ownership and FINRA short interest |

| JKP theme | rows | research-eligible |
|---|---:|---:|
| `accruals` | 5 | 5 |
| `debt_issuance` | 3 | 3 |
| `investment` | 31 | 30 |
| `low_leverage` | 17 | 17 |
| `low_risk` | 27 | 26 |
| `momentum` | 12 | 12 |
| `profit_growth` | 44 | 42 |
| `profitability` | 19 | 19 |
| `quality` | 18 | 18 |
| `seasonality` | 4 | 4 |
| `size` | 13 | 13 |
| `short_term_reversal` | 3 | 3 |
| `value` | 22 | 22 |
| `none` | 6 | 6 |

| wave | rows | research-eligible |
|---|---:|---:|
| `w0_existing` | 175 | 172 |
| `w1_price` | 38 | 38 |
| `w2_fund_a` | 0 | 0 |
| `w3_compositions` | 0 | 0 |
| `w4_events` | 6 | 5 |
| `w5_ownership` | 5 | 5 |

| feature | population | evidence class | publication year | JKP theme | wave |
|---|---|---|---:|---|---|
| `earnings_yield` | all | replication | 1977 | value | w0_existing |
| `book_to_market` | all | replication | 1985 | value | w0_existing |
| `fcf_yield` | all | replication | 1994 | value | w0_existing |
| `dividend_yield` | dividend_payers | replication | 1979 | value | w0_existing |
| `rd_to_market_equity` | rd_reporters | replication | 2001 | size | w0_existing |
| `gross_profit_to_ev` | all | replication | 2011 | value | w0_existing |
| `cfo_to_ev` | all | replication | 1994 | value | w0_existing |
| `ebit_to_ev` | all | replication | 2011 | value | w0_existing |
| `sales_to_ev` | all | replication | 1996 | value | w0_existing |
| `sales_to_price` | all | replication | 1994 | value | w0_existing |
| `cfo_to_price` | all | replication | 1994 | value | w0_existing |
| `ebitda_to_ev` | all | replication | 2011 | value | w0_existing |
| `gross_margin` | all | discovery |  | profitability | w0_existing |
| `operating_margin` | all | replication | 2015 | profitability | w0_existing |
| `net_margin` | all | replication | 1996 | profitability | w0_existing |
| `ebitda_margin` | all | replication | 2015 | profitability | w0_existing |
| `gross_margin_q` | all | discovery |  | profitability | w0_existing |
| `operating_margin_q` | all | replication | 2015 | profitability | w0_existing |
| `net_margin_q` | all | replication | 1996 | profitability | w0_existing |
| `roa` | all | replication | 1996 | quality | w0_existing |
| `roe` | all | replication | 1996 | profitability | w0_existing |
| `roic` | all | replication | 2015 | profitability | w0_existing |
| `roic_ex_goodwill` | all | replication | 2015 | profitability | w0_existing |
| `gross_profitability` | all | replication | 2013 | quality | w0_existing |
| `operating_profitability` | all | replication | 2015 | quality | w0_existing |
| `cash_profitability` | all | replication | 2016 | quality | w0_existing |
| `cfo_to_assets` | all | replication | 2000 | profitability | w0_existing |
| `piotroski_f` | all | replication | 2000 | profitability | w0_existing |
| `piotroski_f_cash_issuance` | all | replication | 2000 | profitability | w0_existing |
| `altman_z_book` | all | replication | 1998 | low_leverage | w0_existing |
| `altman_z` | all | replication | 1998 | low_leverage | w0_existing |
| `beneish_m` | all | replication | 1999 | accruals | w0_existing |
| `ohlson_o` | all | replication | 1998 | profitability | w0_existing |
| `tax_to_book_income` | all | replication | 2004 | seasonality | w0_existing |
| `revenue_growth_yoy` | all | discovery | 1994 | investment | w0_existing |
| `revenue_cagr_3y` | all | replication | 1994 | investment | w0_existing |
| `gross_profit_growth_yoy` | all | discovery |  | profit_growth | w0_existing |
| `operating_income_growth_yoy` | all | replication | 1996 | profit_growth | w0_existing |
| `net_income_growth_yoy` | all | replication | 1984 | profit_growth | w0_existing |
| `eps_diluted_growth_yoy` | all | replication | 1989 | profit_growth | w0_existing |
| `cfo_growth_yoy` | all | discovery |  | profit_growth | w0_existing |
| `fcf_growth_yoy` | all | discovery |  | profit_growth | w0_existing |
| `tax_expense_change_yoy` | all | replication | 2011 | profit_growth | w0_existing |
| `revenue_growth_qoq` | all | discovery |  | investment | w0_existing |
| `eps_diluted_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `eps_cagr_3y` | all | discovery |  | investment | w0_existing |
| `cfo_cagr_3y` | all | discovery |  | investment | w0_existing |
| `gross_profit_cagr_3y` | all | discovery |  | investment | w0_existing |
| `operating_income_cagr_3y` | all | discovery |  | investment | w0_existing |
| `ebitda_cagr_3y` | all | discovery |  | investment | w0_existing |
| `fcf_cagr_3y` | all | discovery |  | investment | w0_existing |
| `gross_margin_change_yoy` | all | replication | 1998 | quality | w0_existing |
| `operating_margin_change_yoy` | all | replication | 1998 | profit_growth | w0_existing |
| `net_margin_change_yoy` | all | replication | 1998 | profit_growth | w0_existing |
| `operating_profitability_change_yoy` | all | replication | 2017 | profit_growth | w0_existing |
| `roe_change_yoy` | all | replication | 2020 | profit_growth | w0_existing |
| `gross_margin_q_change_yoy` | all | replication | 1998 | quality | w0_existing |
| `operating_margin_q_change_yoy` | all | replication | 1998 | profit_growth | w0_existing |
| `net_margin_q_change_yoy` | all | replication | 1998 | profit_growth | w0_existing |
| `revenue_q_growth_yoy` | all | replication | 2006 | investment | w0_existing |
| `gross_profit_q_growth_yoy` | all | discovery |  | profit_growth | w0_existing |
| `operating_income_q_growth_yoy` | all | replication | 1984 | profit_growth | w0_existing |
| `net_income_q_growth_yoy` | all | replication | 1984 | profit_growth | w0_existing |
| `eps_diluted_q_growth_yoy` | all | replication | 1984 | profit_growth | w0_existing |
| `eps_basic_q_growth_yoy` | all | replication | 1984 | profit_growth | w0_existing |
| `cfo_q_growth_yoy` | all | discovery |  | profit_growth | w0_existing |
| `fcf_q_growth_yoy` | all | discovery |  | profit_growth | w0_existing |
| `revenue_q_growth_qoq` | all | discovery |  | investment | w0_existing |
| `gross_profit_q_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `operating_income_q_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `net_income_q_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `cfo_q_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `fcf_q_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `eps_diluted_q_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `eps_basic_q_growth_qoq` | all | discovery |  | profit_growth | w0_existing |
| `eps_diluted_q_growth_yoy_accel` | all | replication | 2020 | profit_growth | w0_existing |
| `revenue_q_growth_yoy_accel` | all | replication | 2006 | profit_growth | w0_existing |
| `gross_margin_q_change_yoy_accel` | all | discovery |  | profit_growth | w0_existing |
| `operating_margin_q_change_yoy_accel` | all | discovery |  | profit_growth | w0_existing |
| `asset_growth` | all | replication | 2008 | investment | w0_existing |
| `total_assets_cagr_3y` | all | replication | 2008 | investment | w0_existing |
| `book_value_growth_yoy` | all | discovery |  | investment | w0_existing |
| `common_equity_cagr_3y` | all | discovery |  | investment | w0_existing |
| `delta_noa` | all | replication | 2004 | investment | w0_existing |
| `capex_growth_yoy` | all | replication | 2006 | investment | w0_existing |
| `capex_q_growth_yoy` | all | replication | 2006 | investment | w0_existing |
| `capex_q_growth_qoq` | all | discovery |  | investment | w0_existing |
| `capex_to_depreciation` | all | discovery |  | investment | w0_existing |
| `capex_to_sales` | all | discovery |  | investment | w0_existing |
| `rd_intensity_sales` | rd_reporters | discovery |  | low_leverage | w0_existing |
| `rd_expense_growth_yoy` | rd_reporters | replication | 2004 | investment | w0_existing |
| `rd_expense_q_growth_yoy` | rd_reporters | discovery |  | investment | w0_existing |
| `rd_expense_q_growth_qoq` | rd_reporters | discovery |  | investment | w0_existing |
| `total_accruals` | all | replication | 1996 | accruals | w0_existing |
| `percent_accruals` | all | replication | 2011 | accruals | w0_existing |
| `working_capital_accruals` | all | replication | 1996 | accruals | w0_existing |
| `rsst_accruals` | all | replication | 2005 | accruals | w0_existing |
| `noa_to_assets` | all | replication | 2004 | debt_issuance | w0_existing |
| `debt_to_equity` | all | replication | 2007 | low_leverage | w0_existing |
| `debt_to_assets` | all | replication | 2007 | low_leverage | w0_existing |
| `long_term_debt_to_assets` | all | replication | 2010 | low_leverage | w0_existing |
| `net_debt_ebitda` | all | discovery |  | low_leverage | w0_existing |
| `interest_coverage` | interest_payers | discovery |  | low_leverage | w0_existing |
| `current_ratio` | all | discovery |  | low_leverage | w0_existing |
| `quick_ratio` | all | discovery |  | low_leverage | w0_existing |
| `cash_ratio` | all | replication | 2008 | low_leverage | w0_existing |
| `debt_to_market` | all | discovery | 1988 | value | w0_existing |
| `assets_to_market` | all | discovery | 1992 | value | w0_existing |
| `net_equity_issuance` | all | replication | 2006 | value | w0_existing |
| `net_debt_issuance` | all | replication | 1999 | seasonality | w0_existing |
| `external_financing` | all | replication | 2006 | debt_issuance | w0_existing |
| `shares_growth_yoy` | all | replication | 2006 | value | w0_existing |
| `payout_ratio` | dividend_payers | discovery |  | value | w0_existing |
| `buyback_yield` | all | replication | 1995 | value | w0_existing |
| `net_payout_yield` | all | replication | 2007 | value | w0_existing |
| `total_payout_yield` | all | replication | 2007 | value | w0_existing |
| `shareholder_yield` | all | replication | 2006 | value | w0_existing |
| `asset_turnover` | all | replication | 2008 | quality | w0_existing |
| `asset_turnover_change_yoy` | all | replication | 2008 | profit_growth | w0_existing |
| `dso_days` | all | replication | 2019 | quality | w0_existing |
| `dio_days` | inventory_holders | replication | 2002 | quality | w0_existing |
| `dpo_days` | all | replication | 2019 | quality | w0_existing |
| `cash_conversion_cycle` | inventory_holders | replication | 2019 | quality | w0_existing |
| `earnings_variability` | all | replication | 2009 | low_risk | w0_existing |
| `market_cap` | all | replication | 1981 | size | w0_existing |
| `dollar_volume_20d` | all | replication | 1998 | size | w0_existing |
| `amihud_illiquidity_21d` | all | replication | 2002 | size | w0_existing |
| `turnover_21d` | all | replication | 1998 | low_risk | w0_existing |
| `momentum_12_1` | all | replication | 1993 | momentum | w0_existing |
| `total_return_12m` | all | replication | 1993 | momentum | w0_existing |
| `total_return_6m` | all | replication | 1993 | momentum | w0_existing |
| `total_return_3m` | all | replication | 1993 | momentum | w0_existing |
| `pct_from_high_252d` | all | replication | 2004 | momentum | w0_existing |
| `total_return_1m` | all | replication | 1990 | short_term_reversal | w0_existing |
| `realized_vol_60d` | all | replication | 2006 | low_risk | w0_existing |
| `realized_vol_252d` | all | replication | 2006 | low_risk | w0_existing |
| `max_daily_return_21d` | all | replication | 2011 | low_risk | w0_existing |
| `downside_deviation_60d` | all | discovery | 2006 | low_risk | w0_existing |
| `roe_q` | all | replication | 2015 | profitability | w0_existing |
| `roa_q` | all | replication | 2010 | quality | w0_existing |
| `rnoa_q` | all | replication | 2008 | profitability | w0_existing |
| `rnoa` | all | replication | 2008 | profitability | w0_existing |
| `gross_profitability_q` | all | replication | 2013 | quality | w0_existing |
| `operating_profitability_q` | all | replication | 2015 | quality | w0_existing |
| `cfo_to_assets_q` | all | replication | 1996 | profitability | w0_existing |
| `fcf_to_assets` | all | replication | 2019 | profitability | w0_existing |
| `sue_ni` | all | replication | 1984 | profit_growth | w0_existing |
| `sue_revenue` | all | replication | 2006 | profit_growth | w0_existing |
| `earnings_surprise_to_market` | all | replication | 1989 | profit_growth | w0_existing |
| `roe_q_change_yoy` | all | replication | 2020 | profit_growth | w0_existing |
| `roa_q_change_yoy` | all | replication | 2010 | profit_growth | w0_existing |
| `sga_to_sales` | all | discovery |  | quality | w0_existing |
| `sga_growth_less_sales_growth` | all | replication | 1993 | profit_growth | w0_existing |
| `inventory_growth_less_sales_growth` | inventory_holders | replication | 1993 | profit_growth | w0_existing |
| `receivables_growth_less_sales_growth` | all | replication | 1993 | profit_growth | w0_existing |
| `sales_growth_less_gross_profit_growth` | all | replication | 1993 | quality | w0_existing |
| `noa_turnover` | all | replication | 2008 | quality | w0_existing |
| `noa_turnover_change_yoy` | all | replication | 2008 | profit_growth | w0_existing |
| `roe_variability_8q` | all | replication | 2005 | low_leverage | w0_existing |
| `roa_variability_8q` | all | replication | 2005 | low_leverage | w0_existing |
| `cfo_variability_8q` | all | replication | 2009 | low_risk | w0_existing |
| `sales_growth_variability_8q` | all | replication | 2005 | low_risk | w0_existing |
| `investment_to_assets` | all | replication | 2008 | investment | w0_existing |
| `inventory_change_to_assets` | all | replication | 2002 | investment | w0_existing |
| `capex_to_assets` | all | replication | 2004 | investment | w0_existing |
| `capex_growth_2y` | all | replication | 2006 | investment | w0_existing |
| `capex_growth_3y` | all | replication | 2006 | investment | w0_existing |
| `rd_to_assets` | rd_reporters | replication | 2001 | low_leverage | w0_existing |
| `share_issuance_1y` | all | replication | 2006 | value | w0_existing |
| `share_issuance_3y` | all | replication | 2006 | value | w0_existing |
| `debt_to_assets_change_yoy` | all | replication | 2000 | debt_issuance | w0_existing |
| `net_debt_to_book_equity` | all | replication | 2007 | low_leverage | w0_existing |
| `current_ratio_change_yoy` | all | replication | 2000 | low_leverage | w0_existing |
| `operating_leverage` | all | replication | 2011 | quality | w0_existing |
| `cash_to_assets` | all | replication | 2012 | low_leverage | w0_existing |
| `ear_m1p1` | all | replication | 1996 | profit_growth | w4_events |
| `runup_m21_m2` | all | discovery | 2010 | short_term_reversal | w4_events |
| `days_since_announcement` | all | replication | 2007 | none | w4_events |
| `sue_ni_event` | all | replication | 1984 | profit_growth | w4_events |
| `beta_mkt_252d` | all | replication | 1972 | low_risk | w4_events |
| `ivol_252d` | all | replication | 2006 | low_risk | w4_events |
| `io_ratio_13f` | all | replication | 2001 | none | w5_ownership |
| `io_change_13f` | all | replication | 1999 | none | w5_ownership |
| `breadth_change_13f` | all | replication | 2002 | none | w5_ownership |
| `short_interest_ratio` | all | replication | 2005 | none | w5_ownership |
| `days_to_cover_si` | all | replication | 2015 | none | w5_ownership |
| `ret_12_1` | all | replication | 1993 | momentum | w1_price |
| `ret_6_1` | all | replication | 1993 | momentum | w1_price |
| `ret_9_1` | all | replication | 1993 | momentum | w1_price |
| `ret_12_7` | all | replication | 2012 | momentum | w1_price |
| `ret_36_13` | all | replication | 1985 | investment | w1_price |
| `ret_60_13` | all | replication | 1985 | investment | w1_price |
| `chmom` | all | replication | 2006 | momentum | w1_price |
| `frog_in_pan` | all | replication | 2014 | momentum | w1_price |
| `seas_1_1an` | all | replication | 2008 | seasonality | w1_price |
| `seas_2_5an` | all | replication | 2008 | seasonality | w1_price |
| `ret_1_0` | all | replication | 1990 | short_term_reversal | w1_price |
| `rvol_21d` | all | replication | 2006 | low_risk | w1_price |
| `rvol_252d` | all | replication | 2006 | low_risk | w1_price |
| `rmax5_21d` | all | replication | 2011 | low_risk | w1_price |
| `rmax1_21d` | all | replication | 2011 | low_risk | w1_price |
| `rskew_252d` | all | replication | 2010 | low_risk | w1_price |
| `beta_ew_252d` | all | replication | 1972 | low_risk | w1_price |
| `ivol_ew_252d` | all | replication | 2006 | low_risk | w1_price |
| `ivol_ew_21d` | all | replication | 2006 | low_risk | w1_price |
| `beta_dimson_252d` | all | replication | 1979 | low_risk | w1_price |
| `beta_down_252d` | all | replication | 2006 | low_risk | w1_price |
| `coskew_252d` | all | replication | 2000 | low_risk | w1_price |
| `beta_bab_1260d` | all | replication | 2014 | low_risk | w1_price |
| `zero_trade_21d` | all | replication | 2006 | size | w1_price |
| `zero_trade_252d` | all | replication | 2006 | size | w1_price |
| `turnover_126d` | all | replication | 1998 | low_risk | w1_price |
| `turnover_252d` | all | replication | 1998 | low_risk | w1_price |
| `std_turn_126d` | all | replication | 2001 | low_risk | w1_price |
| `std_dvol_126d` | all | replication | 2001 | low_risk | w1_price |
| `ami_126d` | all | replication | 2002 | size | w1_price |
| `ami_252d` | all | replication | 2002 | size | w1_price |
| `bidask_cs_21d` | all | replication | 1986 | size | w1_price |
| `bidask_ar_21d` | all | replication | 1986 | size | w1_price |
| `prc_log` | all | replication | 1973 | size | w1_price |
| `prc_highprc_252d` | all | replication | 2004 | momentum | w1_price |
| `me_line_log` | all | replication | 1981 | size | w1_price |
| `dolvol_126d` | all | replication | 1998 | size | w1_price |
| `price_delay_52w` | all | replication | 2005 | low_risk | w1_price |

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
| `buyback_ratio` | negation_of:net_equity_issuance |
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
| `debt_alias_ever_q` | presence_indicator |
| `debt_alias_seen_20q` | presence_indicator |
| `debt_alias_seen_4q` | presence_indicator |
| `debt_alias_seen_q` | presence_indicator |
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
| `inventory_alias_ever_q` | presence_indicator |
| `inventory_alias_seen_20q` | presence_indicator |
| `inventory_alias_seen_4q` | presence_indicator |
| `inventory_alias_seen_q` | presence_indicator |
| `inventory_avg2` | dollar_level_input |
| `invested_capital_avg2` | dollar_level_input |
| `invested_capital_ex_goodwill_avg2` | dollar_level_input |
| `invested_capital_ex_goodwill_q` | dollar_level_input |
| `invested_capital_q` | dollar_level_input |
| `net_debt` | dollar_level_input |
| `net_income_common_ttm` | dollar_level_input |
| `net_income_ttm` | dollar_level_input |
| `ni_q_change_yoy` | dollar_level_input |
| `ni_q_change_yoy_sd8` | dollar_level_input |
| `no_equity_issuance_ttm` | component_of:piotroski_f_cash_issuance |
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
| `revenue_cagr_1y` | duplicate_of:revenue_growth_yoy |
| `revenue_q_change_yoy` | dollar_level_input |
| `revenue_q_change_yoy_sd8` | dollar_level_input |
| `revenue_ttm` | dollar_level_input |
| `sales_per_share` | per_share_level |
| `sga_expense_ttm` | dollar_level_input |
| `share_issuance_ttm` | dollar_level_input |
| `share_repurchase_ttm` | dollar_level_input |
| `stock_compensation_ttm` | dollar_level_input |
| `stockholders_equity_avg2` | dollar_level_input |
| `sustainable_growth` | conflicting_prior:book_value_growth_yoy |
| `tangible_book_value_per_share` | per_share_level |
| `total_assets_avg2` | dollar_level_input |
| `total_debt_avg2` | dollar_level_input |
| `total_debt_q` | dollar_level_input |
| `total_payout_ttm` | dollar_level_input |
