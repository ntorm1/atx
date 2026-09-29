# Parity scorecard

Generated 2026-09-29 by `python -m atx_db.parity.scorecard` from `src/atx_db/parity/catalog.csv` (370 rows: plan section 1.1 domains, design-spec items, seed items, CRSP and panel fields) and the stage lake `C:/atx/atx-db/data/alpha_panel/v1`. Measurement year 2025; TRAIN window 2020-01-01..2022-12-31. Every number is read from the file named in its row; `not measured` means no published measurement exists. Nothing is estimated.

Sources read (SHA-256 prefix):

- `identity/audit.json` 8d512437abe78836
- `fundamentals/manifest.json` 9f9b2f85f6bcd5c7
- `reference/manifest.json` 2f09dcbc1b737add
- `thirteenf/manifest.json` 8974170f64b4a002
- `insider/manifest.json` dcd3f1aa4ba6e266
- `ftd/manifest.json` a76d69bed49829d9
- `short_volume_ext/manifest.json` 7007a13c226a1730
- `earnings_calendar/manifest.json` 9a4a976b03d0ad04
- `metrics/coverage.parquet`: absent (S0.1 section 5 metrics not published yet)

## Summary

| domain | rows | with a lake column | measured | sprints |
|---|---|---|---|---|
| 01 Security master, symbology | 7 | 6 | 1 | S2 |
| 02 Daily prices, returns, shares, market cap | 10 | 9 | 0 | S3 |
| 03 Distributions, corporate actions | 5 | 5 | 0 | S3 |
| 04 Delisting returns | 3 | 3 | 0 | S3 |
| 05 Index membership | 3 | 1 | 0 | S3 |
| 06 Fundamentals, annual and quarterly | 134 | 38 | 39 | S4 |
| 07 Footnotes, segments | 1 | 0 | 0 | S4 |
| 08 Bank, insurer, REIT and utility formats | 60 | 1 | 0 | S4 |
| 09 FX for non-USD filers | 1 | 1 | 1 | S4 |
| 10 Institutional and fund ownership | 3 | 3 | 1 | S5 |
| 11 Insiders | 2 | 2 | 1 | S5 |
| 12 Short interest, fails, lending | 5 | 5 | 1 | S5, S8 |
| 13 Events | 2 | 2 | 1 | S6 |
| 14 Classification | 2 | 2 | 0 | S7 |
| 15 Filing text | 1 | 0 | 0 | S7 |
| 16 Estimates | 45 | 0 | 0 | S6, S8 |
| 17 Options | 2 | 2 | 0 | S8 |
| 18 Macro, rates, FX, factors | 1 | 0 | 1 | S1 |
| 19 Characteristic library | 82 | 6 | 2 | S9 |
| 20 Operations | 1 | 0 | 0 | S10 |
| **total** | 370 | 86 | 48 | |

## Fundamentals gate (design spec: >= 110 items at >= 0.90, FY2015-2025)

- catalog fundamentals items: 133; carried by a lake column: 38; with a measured value: 38
- items whose best measured column is >= 0.90 on the all-filing-events basis (2025): 17 (the gate's top-3000 market-cap basis is not measured yet)

## Domains (plan section 1.1)

| domain | tier-1 reference | lake stage | target | measured | sprint |
|---|---|---|---|---|---|
| 01 Security master, symbology | CRSP stocknames, CCM links, FactSet Symbology | identity_table; security_master | PIT links ≥ 95% of member_equity cells from 2019; CUSIP/ISIN/FIGI/LEI histories; exchange and share-code history | 0.7838 = member_equity cells linked by a PIT tier (strict + name) (2025)<br>0.9877 = member_equity cells linked by any tier (2025) | S2 |
| 02 Daily prices, returns, shares, market cap | CRSP DSF/MSF | prices; panel | validated returns; daily shares by class; 1990+ only if U4 bought | not measured | S3 |
| 03 Distributions, corporate actions | CRSP DSE, FactSet CA | corporate_actions | declare, record and pay dates; spin-off ratios | not measured | S3 |
| 04 Delisting returns | CRSP DLRET | delisting | exact for cash deals, flagged imputation for the rest | not measured | S3 |
| 05 Index membership | S&P, Russell constituents | - | R1000/R2000/R3000 and S&P 500 proxies | not measured | S3 |
| 06 Fundamentals, annual and quarterly | Compustat FUNDA/FUNDQ, FactSet FF | fundamentals; export_fundamental_events | ≥ 110 items at ≥ 90% (tier-1 gate); stretch 250 | 31 of 70 event columns >= 0.90 ex-structural (events 2025) | S4 |
| 07 Footnotes, segments | Compustat Segments, pension, debt schedules | notes | segments, debt maturities, leases, pension, SBC, tax | not measured | S4 |
| 08 Bank, insurer, REIT and utility formats | Compustat Bank / FS | fundamentals | bank items (loans, deposits, NII, provisions, capital); FFO | not measured | S4 |
| 09 FX for non-USD filers | FactSet FX | reference | converted, flagged | 25 H.10 currencies in reference/fx_daily (107,863 rows) | S4 |
| 10 Institutional and fund ownership | FactSet Ownership | thirteenf | + mutual funds, filer type, activist stakes | 0.9993 = member_equity cells with visible 13F institutional shares (2025) | S5 |
| 11 Insiders | FactSet Insiders | insider | + Form 144, net-buying measures | 4,288 = issuers with an open-market Form 4 purchase or sale (count) (2025) | S5 |
| 12 Short interest, fails, lending | FINRA, Markit | short_interest; short_volume; ftd; regsho_threshold; borrow_proxy | Reg SHO complete; lending via license adapter | 0.9869 = member_equity cells with a mapped FTD row in the prior 30 days (2025)<br>0.9999 = member_equity cells with a short-volume row (2025) | S5, S8 |
| 13 Events | StreetAccount, SDC-lite | sec_filings; earnings_calendar | M&A deals, IPO/SEO, buybacks, dividends, guidance, halts, governance | 0.859 = member_equity cells with a visible primary 8-K 2.02 within 364 days (2025) | S6 |
| 14 Classification | GICS, RBICS | fundamentals | + NAICS (approximate), text industries | not measured | S7 |
| 15 Filing text | FactSet Filings | sec_filings | risk-factor and MD&A change, length, readability | not measured | S7 |
| 16 Estimates | I/B/E/S, FactSet Estimates | - | license adapter + guidance substitute | not measured | S6, S8 |
| 17 Options | OptionMetrics | prices; panel | skew, volume, OI (atx-vol or license) | not measured | S8 |
| 18 Macro, rates, FX, factors | FRED, French | reference | yield curve, FX, VIX, factor files | 37 FRED series; business days missing since 2010: 0..20 | S1 |
| 19 Characteristic library | JKP, OSAP | characteristics; panel | ≥ 250 characteristics, replicated | not measured | S9 |
| 20 Operations | vendor SLAs | lake | daily incremental, SLOs, reproducible releases | not measured | S10 |

## Items

### 01 Security master, symbology

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| permno | - | PERMNO | - | prices.security_id | one key per listing line, delisted included | not measured | S2 |
| ncusip | - | NCUSIP | - | - | >= 99.5% of 13F SH value maps; ISIN check digits valid 100% | not measured | S2 |
| shrcd | - | SHRCD | - | security_master.finra_type | share-code-style history per line | not measured | S2 |
| exchcd | - | EXCHCD | - | security_master.exchange | exchange history per line | not measured | S2 |
| ticker | - | TICKER | - | prices.ticker | dated ticker per line | not measured | S2 |
| universe_us_listed | - | - | - | panel.member_equity | design spec market data / quality gates | not measured | S2 |

### 02 Daily prices, returns, shares, market cap

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| prc | - | PRC | - | prices.close | validated closes 2012-03-26+ | not measured | S3 |
| ret | - | RET | - | prices.ret | >= 99% of sampled daily returns within 1 bp | not measured | S3 |
| vol | - | VOL | - | prices.volume | member_equity cells | not measured | S3 |
| shrout | - | SHROUT | - | panel.shares_out | >= 99% of member_equity cells; two sources within 5% on >= 95% | not measured | S3 |
| mktcap | - | SHROUT*PRC | - | panel.me_company | >= 99% of member_equity cells | not measured | S3 |
| adj_close | - | - | - | prices.adj_close | design spec market data / quality gates | not measured | S3 |
| total_return | - | - | - | prices.ret | design spec market data / quality gates | not measured | S3 |
| shares_outstanding_daily | - | - | - | panel.shares_out | design spec market data / quality gates | not measured | S3 |
| shares_cross_source_check | - | - | - | - | design spec market data / quality gates | not measured | S3 |

### 03 Distributions, corporate actions

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| cfacpr | - | CFACPR | - | corporate_actions.return_factor | ex-date factors | not measured | S3 |
| divamt | - | DIVAMT | - | corporate_actions.implied_cash | declare date for >= 90% of member-line cash dividends from 2019 | not measured | S3 |
| split_ratio | - | - | - | corporate_actions.split_ratio | design spec market data / quality gates | not measured | S3 |
| dividend_cash | - | - | - | corporate_actions.implied_cash | design spec market data / quality gates | not measured | S3 |

### 04 Delisting returns

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| dlret | - | DLRET | - | delisting.dlret | exact DLRET for >= 80% of M&A delistings from 2018 | not measured | S3 |
| delisting_return | - | - | - | delisting.dlret | design spec market data / quality gates | not measured | S3 |

### 05 Index membership

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| ewretd | - | EWRETD | - | panel.mkt_ret | CRSP-style EW/VW market returns; French Mkt rho >= 0.99 (<= 2022) | not measured | S3 |
| vwretd | - | VWRETD | - | - | French Mkt-RF rho >= 0.99 on monthly windows ending <= 2022-12 | not measured | S3 |

### 06 Fundamentals, annual and quarterly

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| revenue | sale\|revt\|saleq\|revtq | - | FF_SALES | fundamentals.sale_q\|sale_ttm | sale_ttm >= 0.96 (mega-alpha section 2, linked-USD non-structural); >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | sale_q 0.8471 (events 2025, ex-structural)<br>sale_ttm 0.865 (events 2025, ex-structural) | S4 |
| cost_of_revenue | cogs\|cogsq | - | FF_COGS | fundamentals.cogs_q\|cogs_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | cogs_q 0.5986 (events 2025, ex-structural)<br>cogs_ttm 0.6167 (events 2025, ex-structural) | S4 |
| gross_profit | gp | - | FF_GROSS_INC | fundamentals.gp_q\|gp_ttm | gp_ttm >= 0.90 (mega-alpha section 2, linked-USD non-structural); >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | gp_q 0.6794 (events 2025, ex-structural)<br>gp_ttm 0.6954 (events 2025, ex-structural) | S4 |
| sga_expense | xsga\|xsgaq | - | FF_SGA | fundamentals.xsga_q\|xsga_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | xsga_q 0.8316 (events 2025, ex-structural)<br>xsga_ttm 0.8429 (events 2025, ex-structural) | S4 |
| rd_expense | xrd\|xrdq | - | FF_RD_EXP | fundamentals.xrd_ttm | xrd_ttm >= 0.95 (mega-alpha section 2, linked-USD non-structural); >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | xrd_ttm 0.9434 (events 2025, ex-structural) | S4 |
| depreciation_amortization | dp\|dpq | - | FF_DEP_AMORT_EXP | fundamentals.dp_q\|dp_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | dp_q 0.748 (events 2025, ex-structural)<br>dp_ttm 0.773 (events 2025, ex-structural) | S4 |
| operating_income | oiadp\|oiadpq | - | FF_OPER_INC | fundamentals.oi_q\|oi_ttm | oi_ttm >= 0.92 (mega-alpha section 2, linked-USD non-structural); >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | oi_q 0.861 (events 2025, ex-structural)<br>oi_ttm 0.8751 (events 2025, ex-structural) | S4 |
| ebitda | oibdp\|oibdpq | - | FF_EBITDA_OPER | fundamentals.ebitda_q\|ebitda_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | ebitda_q 0.7282 (events 2025, ex-structural)<br>ebitda_ttm 0.7533 (events 2025, ex-structural) | S4 |
| interest_expense | xint\|xintq | - | FF_INT_EXP_TOT | fundamentals.xint_q\|xint_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | xint_q 0.7552 (events 2025, ex-structural)<br>xint_ttm 0.7675 (events 2025, ex-structural) | S4 |
| nonoperating_income | nopi | - | FF_NON_OPER_INC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| special_items | spi | - | FF_SPECIAL_ITEMS | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| pretax_income | pi\|piq | - | FF_PRETAX_INC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| income_tax | txt\|txtq | - | FF_INC_TAX | fundamentals.txt_q | txt_q >= 0.95 (mega-alpha section 2, linked-USD non-structural); >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | txt_q 0.8567 (events 2025, ex-structural) | S4 |
| income_before_extraordinary | ib\|ibq | - | FF_NET_INC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| minority_interest_income | mii | - | FF_MIN_INT_INC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| net_income | ni\|niq | - | FF_NET_INC_TOT | fundamentals.ni_q\|ni_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | ni_q 0.9325 (events 2025, ex-structural)<br>ni_ttm 0.9423 (events 2025, ex-structural) | S4 |
| net_income_common | ibcom\|ibcomq | - | FF_NET_INC_AVAIL | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| discontinued_operations | do | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| extraordinary_items | xido | - | FF_NET_INC_DISC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| eps_basic | epspx\|epspxq | - | FF_EPS_BASIC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| eps_diluted | epsfx\|epsfxq | - | FF_EPS_DIL | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| shares_basic_weighted | cshpri\|cshprq | - | FF_SHS_BASIC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| shares_diluted_weighted | cshfd\|cshfdq | - | FF_SHS_DIL | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| dividends_common | dvc\|dvcy | - | FF_DIV_CASH\|FF_DIV_COM | fundamentals.dvc_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | dvc_ttm 0.913 (events 2025, ex-structural) | S4 |
| dividends_preferred | dvp\|dvpy | - | FF_DIV_PFD\|FF_DIV_PFD_CF | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| stock_compensation | stkco | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| cash_and_equivalents | che\|cheq | - | FF_CASH_ST | fundamentals.che | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | che 0.9708 (events 2025, ex-structural) | S4 |
| short_term_investments | ivst | - | FF_ST_INVEST | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| receivables | rect\|rectq | - | FF_RECV_NET | fundamentals.rect | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | rect 0.6153 (events 2025, ex-structural) | S4 |
| inventory | invt\|invtq | - | FF_INVENT | fundamentals.invt | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | invt 0.4847 (events 2025, ex-structural) | S4 |
| other_current_assets | aco | - | FF_OTH_CURR_ASSET | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| total_current_assets | act\|actq | - | FF_ASSETS_CURR | fundamentals.act | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | act 0.9125 (events 2025, ex-structural) | S4 |
| ppe_gross | ppegt\|ppegtq | - | FF_PPE_GROSS | fundamentals.ppegt | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | ppegt 0.4005 (events 2025, ex-structural) | S4 |
| ppe_net | ppent\|ppentq | - | FF_PPE_NET | fundamentals.ppe | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | ppe 0.7395 (events 2025, ex-structural) | S4 |
| goodwill | gdwl\|gdwlq | - | FF_INTANG_GW | fundamentals.gdwl | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | gdwl 0.9351 (events 2025, ex-structural) | S4 |
| intangibles | intan\|intanq | - | FF_INTANG | fundamentals.intan | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | intan 0.9187 (events 2025, ex-structural) | S4 |
| long_term_investments | ivao | - | FF_INVEST_LT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| other_assets | ao | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| total_assets | at\|atq | - | FF_ASSETS | fundamentals.at | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | at 0.9777 (events 2025, ex-structural) | S4 |
| accounts_payable | ap\|apq | - | FF_PAYABLES | fundamentals.ap | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | ap 0.8094 (events 2025, ex-structural) | S4 |
| short_term_debt | dlc\|dlcq | - | FF_DEBT_ST | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| taxes_payable | txp | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| other_current_liabilities | lco | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| total_current_liabilities | lct\|lctq | - | FF_LIAB_CURR | fundamentals.lct | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | lct 0.9095 (events 2025, ex-structural) | S4 |
| long_term_debt | dltt\|dlttq | - | FF_DEBT_LT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| deferred_taxes | txditc | - | FF_DEFERRED_TX | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| other_liabilities | lo | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| total_liabilities | lt\|ltq | - | FF_LIAB | fundamentals.lt | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | lt 0.9756 (events 2025, ex-structural) | S4 |
| minority_interest | mib\|mibq | - | FF_MIN_INT_BS | fundamentals.mib | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | mib 0.916 (events 2025, ex-structural) | S4 |
| preferred_stock | pstk\|pstkq | - | FF_PREF_STK | fundamentals.pstk | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | pstk 0.9795 (events 2025, ex-structural) | S4 |
| common_equity | ceq\|ceqq | - | FF_COM_EQ_TOT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| retained_earnings | re\|req | - | FF_RETAIN_EARN | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| treasury_stock | tstk\|tstkq | - | FF_TREAS_STK | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| stockholders_equity | seq\|seqq | - | FF_EQ_TOT | fundamentals.seq | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | seq 0.9779 (events 2025, ex-structural) | S4 |
| shares_outstanding | csho\|cshoq | - | FF_SHS_OUTSTND | fundamentals.shrs_q | shrs_q >= 0.97 (mega-alpha section 2, linked-USD non-structural); >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | shrs_q 0.9942 (events 2025, ex-structural) | S4 |
| accumulated_depreciation | dpact | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| cfo | oancf\|oancfy | - | FF_CASH_FROM_OPER | fundamentals.cfo_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | cfo_ttm 0.9325 (events 2025, ex-structural) | S4 |
| capex | capx\|capxy | - | FF_CAPEX | fundamentals.capx_ttm | capx_ttm >= 0.95 (mega-alpha section 2, linked-USD non-structural); >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | capx_ttm 0.8915 (events 2025, ex-structural) | S4 |
| acquisitions | aqc\|aqcy | - | FF_ACQUIS | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| investing_cash_flow | ivncf\|ivncfy | - | FF_CASH_FROM_INVEST | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| dividends_paid | dv | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| share_repurchase | prstkc\|prstkcy | - | FF_STOCK_REPUR | fundamentals.prstkc_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | prstkc_ttm 0.8704 (events 2025, ex-structural) | S4 |
| share_issuance | sstk\|sstky | - | FF_STOCK_ISSUE | fundamentals.sstk_ttm | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | sstk_ttm 0.8078 (events 2025, ex-structural) | S4 |
| debt_issuance | dltis | - | FF_DEBT_ISSUE | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| debt_reduction | dltr | - | FF_DEBT_REDUC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| financing_cash_flow | fincf\|fincfy | - | FF_CASH_FROM_FIN | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| change_in_cash | chech | - | FF_NET_CHG_CASH | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| cf_depreciation | dpc\|dpcy | - | FF_DEP_AMORT_CF | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| cf_stock_compensation | sc | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| deferred_tax_cf | txdc | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| working_capital_change | wcapc | - | FF_CHG_WC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| operating_lease_liabilities | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| finance_lease_liabilities | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| capitalized_software | capsft | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| employees | emp | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| selling_expense_only | - | - | FF_SGA_SELL | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| g_and_a_expense_only | - | - | FF_SGA_GEN_ADMIN | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| advertising | xad | - | FF_SGA_MKT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| operating_expenses_total | - | - | FF_OPER_EXP | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| depreciation_only | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| amortization_of_intangibles | am | - | FF_AMORT_EXP | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| ebitda_standardised | ebitda | - | FF_EBITDA | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| ebit__1017 | ebit | - | FF_EBIT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| interest_expense_debt_only | - | - | FF_INT_EXP_DEBT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| current_tax | txc | - | FF_INC_TAX_CURR | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| deferred_tax | txdi | - | FF_INC_TAX_DEFER | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| equity_in_affiliates | - | - | FF_EQ_AFF_INC | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| eps_basic_incl_extra | epspi\|epspiq | - | FF_EPS_BASIC_EXT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| eps_diluted_incl_extra | epsfi\|epsfiq | - | FF_EPS_DIL_EXT | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| eps_diluted_ltm | epsf12 | - | FF_EPS_DIL | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| normalised_income | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| common_dividends_declared_per_share | dvpsx_f | - | FF_DIV_PS | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| entity_public_float_shares | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| treasury_stock_shares | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| class_a_common_shares_outstanding | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| class_b_common_shares_outstanding | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| class_c_common_shares_outstanding | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| class_d_common_shares_outstanding | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| cash_only | ch\|chq | - | FF_CASH | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| prepaid_expense | - | - | FF_PREPAID | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| other_intangibles | - | - | FF_INTANG_OTH | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| operating_lease_rou_asset | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| deferred_tax_assets | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| other_lt_assets | - | - | FF_OTH_LT_ASSET | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| accrued_liabilities | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| current_portion_of_lt_debt | dd1 | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| total_debt | - | - | FF_DEBT | fundamentals.debt | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | debt 0.9799 (events 2025, ex-structural) | S4 |
| operating_lease_liability | - | - | FF_OPER_LEASES_PV | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| deferred_revenue | - | - | - | fundamentals.drev | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | drev 0.4282 (events 2025, ex-structural) | S4 |
| other_lt_liabilities | - | - | FF_OTH_LT_LIAB | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| common_stock_at_par | - | - | FF_COM_STK_PAR | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| additional_paid_in_capital | - | - | FF_COM_STK_PAR | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| aoci | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| equity_incl_non_controlling | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| temporary_equity | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| cfo_continuing_ops | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| capex_broader_incl_intangibles | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| stock_based_compensation | - | - | FF_STOCK_COMP | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| divestitures | sppe | - | FF_DIVEST | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| net_change_in_debt | - | - | - | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| change_in_ar | recch | - | FF_CHG_AR | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| change_in_inventory | - | - | FF_CHG_INV | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| change_in_ap | - | - | FF_CHG_AP | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| fx_effect_on_cash | exre | - | FF_EXCH_RATE_CF | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| free_cash_flow__1325 | - | - | FF_FCF | - | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | not measured | S4 |
| identity_assets_eq_liabilities_plus_equity | - | - | - | - | design spec market data / quality gates | not measured | S4 |
| identity_gross_profit | - | - | - | - | design spec market data / quality gates | not measured | S4 |
| identity_cash_flow | - | - | - | - | design spec market data / quality gates | not measured | S4 |
| fsds_benchmark | - | - | - | - | design spec market data / quality gates | not measured | S4 |
| be | - | - | - | fundamentals.be | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | be 0.9779 (events 2025, ex-structural) | S4 |
| sue | - | - | - | fundamentals.sue | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | sue 0.8696 (events 2025, ex-structural) | S4 |
| buyback_authorized | - | - | - | fundamentals.buyback_authorized | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | buyback_authorized 0.0192 (events 2025, ex-structural) | S4 |
| buyback_remaining | - | - | - | fundamentals.buyback_remaining | >= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit) | buyback_remaining 0.1092 (events 2025, ex-structural) | S4 |

### 08 Bank, insurer, REIT and utility formats

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| net_interest_income | - | - | FF_NET_INT_INC | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| interest_income | - | - | FF_INT_INC | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| interest_expense_bank | intexp | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| provision_for_loan_losses | pln | - | FF_PROV_LOAN_LOSS | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| loans_net | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| deposits | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| allowance_for_loan_losses | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| noninterest_income | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| noninterest_expense | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| premiums_earned | pncia | - | FF_PREM_EARNED | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| benefits_and_claims | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| policy_reserves | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| investment_income_insurance | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| rental_revenue | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| ffo | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| real_estate_investments_net | - | - | - | - | bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5) | not measured | S4 |
| net_interest_margin | nim | - | FF_NIM | - | industry template items (S4.5) | not measured | S4 |
| interest_income_total | intinc | - | - | - | industry template items (S4.5) | not measured | S4 |
| allowance_for_loan_and_lease_losses | alll | - | FF_ALLL | - | industry template items (S4.5) | not measured | S4 |
| non_performing_loans | - | - | FF_NPL | - | industry template items (S4.5) | not measured | S4 |
| net_charge_offs | - | - | FF_NCO | - | industry template items (S4.5) | not measured | S4 |
| total_loans | tll | - | FF_LOANS_TOT | - | industry template items (S4.5) | not measured | S4 |
| total_deposits | tdsa | - | FF_DEPOSITS_TOT | - | industry template items (S4.5) | not measured | S4 |
| tier_1_capital | - | - | FF_TIER1_CAP | - | industry template items (S4.5) | not measured | S4 |
| tier_1_capital_ratio | - | - | FF_TIER1_RATIO | - | industry template items (S4.5) | not measured | S4 |
| cet1_common_equity_tier_1 | - | - | FF_CET1 | - | industry template items (S4.5) | not measured | S4 |
| risk_weighted_assets | - | - | FF_RWA | - | industry template items (S4.5) | not measured | S4 |
| efficiency_ratio | - | - | FF_EFFICIENCY_RATIO | - | industry template items (S4.5) | not measured | S4 |
| premiums_written | - | - | FF_PREM_WRITTEN | - | industry template items (S4.5) | not measured | S4 |
| loss_reserves | losres | - | FF_LOSS_RESERVE | - | industry template items (S4.5) | not measured | S4 |
| insurance_benefits_paid | benefits | - | - | - | industry template items (S4.5) | not measured | S4 |
| unpaid_claim_liability | ucl | - | - | - | industry template items (S4.5) | not measured | S4 |
| loss_ratio | - | - | FF_LOSS_RATIO | - | industry template items (S4.5) | not measured | S4 |
| expense_ratio | - | - | FF_EXP_RATIO | - | industry template items (S4.5) | not measured | S4 |
| combined_ratio | - | - | FF_COMB_RATIO | - | industry template items (S4.5) | not measured | S4 |
| investment_portfolio | - | - | FF_INVEST_PORT | - | industry template items (S4.5) | not measured | S4 |
| insurance_float | - | - | FF_FLOAT | - | industry template items (S4.5) | not measured | S4 |
| funds_from_operations_ffo | - | - | FF_FFO | - | industry template items (S4.5) | not measured | S4 |
| ffo_per_share | - | - | FF_FFO_PS | - | industry template items (S4.5) | not measured | S4 |
| adjusted_ffo_affo | - | - | FF_AFFO | - | industry template items (S4.5) | not measured | S4 |
| affo_per_share | - | - | FF_AFFO_PS | - | industry template items (S4.5) | not measured | S4 |
| net_operating_income_noi | - | - | FF_NOI | - | industry template items (S4.5) | not measured | S4 |
| same_store_noi | - | - | FF_NOI_SAME_STORE | - | industry template items (S4.5) | not measured | S4 |
| occupancy_rate | - | - | FF_OCCUPANCY | - | industry template items (S4.5) | not measured | S4 |
| rent_per_square_foot | - | - | FF_RENT_PSF | - | industry template items (S4.5) | not measured | S4 |
| gross_leasable_area | - | - | FF_GLA | - | industry template items (S4.5) | not measured | S4 |
| net_asset_value_per_share | - | - | FF_NAV | - | industry template items (S4.5) | not measured | S4 |
| capitalisation_rate | - | - | FF_CAP_RATE | - | industry template items (S4.5) | not measured | S4 |
| ffo_payout_ratio | - | - | FF_FFO_PAYOUT | - | industry template items (S4.5) | not measured | S4 |
| utility_operating_revenue | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| utility_rate_base | - | - | FF_UTILITY_RATE_BASE | - | industry template items (S4.5) | not measured | S4 |
| utility_ppe_rate_base | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| utility_operating_income | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| utility_depreciation_amortization | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| broker_dealer_revenue | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| segregated_cash_securities | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| payables_broker_dealers | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| receivables_broker_dealers | - | - | - | - | industry template items (S4.5) | not measured | S4 |
| net_capital | - | - | FF_NET_CAPITAL | - | industry template items (S4.5) | not measured | S4 |

### 10 Institutional and fund ownership

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| inst_shares | - | - | FactSet Ownership: institutional shares | thirteenf.inst_shares | fund ownership for >= 95% of member_equity cells from 2020 | not measured | S5 |
| filer_type | - | - | FactSet Ownership: holder type | thirteenf.filer_type | a type for >= 90% of 13F SH value per quarter | not measured | S5 |

### 11 Insiders

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| insider_net_buying | - | - | FactSet Insiders | insider.insider_last_code | 144 notices with available_at; nets per issuer and month | not measured | S5 |

### 12 Short interest, fails, lending

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| short_interest | - | - | - | short_interest.si_shares | member_equity cells per year | not measured | S5 |
| short_volume | - | - | - | short_volume.sv_total_volume | member_equity cells per year | not measured | S5 |
| fails_to_deliver | - | - | - | ftd.ftd_quantity | member_equity cells per year | not measured | S5 |
| threshold_list | - | - | - | regsho_threshold.regsho_last_list_date | threshold lists for all 5 listing markets 2018+ | not measured | S5 |

### 13 Events

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| earnings_announcement | - | - | FactSet Events / StreetAccount | earnings_calendar.earn_last_utc | >= 95% of member_equity issuers covered, FPIs included | not measured | S6 |

### 14 Classification

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| siccd | - | SICCD | - | fundamentals.sic | SIC for 100% of linked issuers | not measured | S7 |

### 16 Estimates

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| eps_diluted_normalized | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| eps_diluted_gaap | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| eps_basic__2003 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| eps_beginning_of_period | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| sales_revenue | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| ebitda | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| ebit__2007 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| operating_profit | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| net_income | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| cash_flow_per_share | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| capex__2011 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| free_cash_flow__2012 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| fcf_per_share__2013 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| dividends_per_share | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| book_value_per_share__2015 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| gross_profit__2016 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| gross_margin__2017 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| operating_margin__2018 | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| effective_tax_rate | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| return_on_equity | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| return_on_assets | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| net_asset_value_reit_cef | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| funds_from_operations_reit | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| long_term_growth | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| target_price_12m | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| recommendation | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| recommendation_count_buys | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| recommendation_count_holds | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| recommendation_count_sells | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| number_of_estimators | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| stdev_of_estimates | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| high_low_estimate | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| surprise | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| beat_meet_miss_flag | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| forward_p_e | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| same_store_sales_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| arpu_telecom_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| subscribers_telecom_streaming_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| load_factor_airline_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| ask_rpk_airline_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| production_oil_e_and_p_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| production_gold_miner_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| aum_asset_manager_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |
| noi_reit_kpi | - | - | - | - | adapter contract tests pass on mocks (S8.1) | not measured | S8 |

### 17 Options

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| iv_atm | - | - | - | prices.iv_atm_21d | >= 95% of optionable member lines | not measured | S8 |

### 19 Characteristic library

| item | Compustat | CRSP | FactSet | stage.column | target | measured | sprint |
|---|---|---|---|---|---|---|---|
| flow_ttm | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| balance_avg2 | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| eps_ttm | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| sales_per_share | - | - | FF_SALES_PS | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| book_per_share | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| cfo_per_share | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| fcf_per_share | - | - | FF_FCF_PS | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| dividends_per_share | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| market_cap | mkvalt\|mkvaltq | - | FF_MKT_CAP_TOT | panel.me_company | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| enterprise_value | - | - | FF_EV | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| ev_ebitda | - | - | FF_EV_EBITDA | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| ev_sales | - | - | FF_EV_SALES | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| pe_ttm | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| pb | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| ps_ttm | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| pcf_ttm | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| fcf_yield | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| dividend_yield | - | - | FF_DIV_YIELD | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| earnings_yield | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| shareholder_yield | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| gross_margin | - | - | FF_GROSS_MARGIN | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| operating_margin | - | - | FF_OPER_MARGIN | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| net_margin | - | - | FF_NET_MARGIN | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| ebitda_margin | - | - | FF_EBITDA_MARGIN | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| roa | - | - | FF_ROA | characteristics.roa | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| roe | - | - | FF_ROE | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| roic | - | - | FF_ROIC | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| roic_ex_goodwill | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| gross_profitability | - | - | - | characteristics.gpa | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| cash_profitability | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| asset_turnover | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_revenue | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_gross_profit | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_operating_income | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_net_income | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_eps_diluted | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_cfo | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_fcf | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_total_assets | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_shares_outstanding | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_book_value | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_capex | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| growth_employees | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| total_debt | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| net_debt | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| debt_to_equity | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| debt_to_assets | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| net_debt_ebitda | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| interest_coverage | - | - | FF_INT_COVER | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| current_ratio | - | - | FF_CURR_RATIO | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| quick_ratio | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| cash_ratio | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| total_accruals | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| percent_accruals | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| noa | - | - | - | fundamentals.noa | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | noa 0.9736 (events 2025, ex-structural) | S9 |
| delta_noa | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| piotroski_f | - | - | - | fundamentals.fscore | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | fscore 0.4897 (events 2025, ex-structural) | S9 |
| altman_z | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| beneish_m | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| ohlson_o | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| earnings_variability | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| asset_growth | - | - | - | characteristics.asset_growth | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| capex_to_depreciation | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| capex_to_sales | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| external_financing | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| net_equity_issuance | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| net_debt_issuance | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| payout_ratio | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| buyback_yield | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| total_payout_yield | - | - | - | - | >= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9) | not measured | S9 |
| book_value_per_share__1401 | - | - | FF_BV_PS | - | >= 250 characteristics (S9) | not measured | S9 |
| tangible_book_value_per_share | - | - | FF_TANG_BV_PS | - | >= 250 characteristics (S9) | not measured | S9 |
| cash_per_share | - | - | FF_CASH_PS | - | >= 250 characteristics (S9) | not measured | S9 |
| p_e | - | - | FF_PE | - | >= 250 characteristics (S9) | not measured | S9 |
| p_b | - | - | FF_PB | - | >= 250 characteristics (S9) | not measured | S9 |
| p_s | - | - | FF_PS | - | >= 250 characteristics (S9) | not measured | S9 |
| debt_equity | - | - | FF_DEBT_EQUITY | - | >= 250 characteristics (S9) | not measured | S9 |
| dso_days | - | - | FF_RECV_DAYS | - | >= 250 characteristics (S9) | not measured | S9 |
| dio_days | - | - | FF_INV_DAYS | - | >= 250 characteristics (S9) | not measured | S9 |
| dpo_days | - | - | FF_PAY_DAYS | - | >= 250 characteristics (S9) | not measured | S9 |
| cash_conversion_cycle | - | - | FF_CCC | - | >= 250 characteristics (S9) | not measured | S9 |
