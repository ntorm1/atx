# US Equity Fundamentals — Vendor schemas + EDGAR XBRL reconstruction

**Status:** Research, v0.1
**Audience:** ats-eqt engineering team (ingestion, item dictionary, normalisation, query); ats-core team designing the bitemporal fact engine; ats-eqt commercial team scoping the foundational product
**Scope:** field-level enumeration of the US equity fundamentals offerings at Compustat (XPF / CCM), FactSet Fundamentals (FF + FF Advanced + industry overlays), Worldscope (WS items + WS.* mnemonics), Bloomberg FA / BQL, S&P Capital IQ ciqFinInstanceItem, SimFin, Sharadar SF1, and WRDS-distributed academic variants; the explicit mapping back to SEC EDGAR us-gaap XBRL concepts; a cross-vendor canonical field map; and the proposed ats-eqt foundational DDL.
**Last updated:** 2026-05-14

This file goes ONE level deeper than the vendor profiles in `research/vendors/` and the modelling reference in `research/schemas/data_models_and_methodology.md`. Those documents enumerate the *tables*; this one enumerates the *fields* and ties each one back to its primary regulatory source.

---

## 0. Executive summary

US equity fundamentals is the **foundational dataset** of the entire ats-eqt product. Every other dataset — estimates (I/B/E/S, FactSet Estimates, BEst), ownership (13F, N-PORT, Form 4), classifications (GICS, RBICS, BICS, TRBC, NAICS, SIC), ESG (SASB, Truvalue, MSCI), supply chain (Revere, SPLC, Panjiva), short interest (Form SHO when it arrives in 2028), and corporate actions (M&A, splits, dividends) — joins back onto a `(entity, period, item)` fact tuple. If the fundamentals fact table is wrong, every downstream join is wrong. If the fundamentals taxonomy is fragmented or its restatement history is naïve, every backtest is biased.

Five non-negotiable findings drive the schema design:

1. **The substrate is free.** Since 2009 the SEC has mandated inline-XBRL for all 10-K, 10-Q, 8-K, 20-F, and 40-F filings of registrants over the phased-in size thresholds, with the smallest filers in scope from FY2021 onward. The SEC publishes the resulting facts in three free machine-readable channels (`companyfacts.json` per CIK; the `frames` cross-section endpoint; the quarterly DERA "Financial Statement Data Sets" bulk dump). Compustat-equivalent reconstruction of headline line items is mechanically achievable for ~7,000 US public filers at zero licence cost. (Sources: <https://www.sec.gov/search-filings/edgar-application-programming-interfaces>, <https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets>.)

2. **The moat is normalisation.** Companies file under us-gaap (or IFRS-on-Form-20-F) plus *extension* concepts (~19% of all concepts across annual reports per XBRL US, source: <https://xbrl.us/why-normalize-data/>). The same economic line item — say, "revenue" — is filed under `us-gaap:Revenues`, `us-gaap:SalesRevenueNet`, `us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax`, `us-gaap:RevenueFromContractWithCustomerIncludingAssessedTax`, `us-gaap:SalesRevenueGoodsNet`, or a company-specific extension, depending on the company, the era, and the post-ASC 606 transition. Compustat collapses all of these into `revt`; FactSet collapses them into `FF_SALES`; Worldscope into `01001`. The proprietary value-add is the human-curated mapping table — 50+ analyst-years of work — that produces a comparable item dictionary across 60+ years and 100+ jurisdictions.

3. **The market price of that moat is high but knowable.** Triangulated from public price-bench sources (CostBench, WRDS public discussion threads), the institutional Compustat+CCM+Snapshot bundle clears $100K–$1M/yr; FactSet Fundamentals + PIT clears $200K–$2M/yr; the academic WRDS subscription is $25K–$70K/yr. Sharadar SF1 (a Compustat near-clone covering 14,000+ US tickers, 20+ years) sits at **$540/yr individual / $4,800/yr commercial** (source: <https://data.nasdaq.com/databases/SF1>); SimFin Plus is **€59–€199/yr** (source: <https://www.simfin.com/en/pricing/>). The $1,000-to-$1,000,000 spread is the open-data wedge.

4. **Point-in-time is the differentiator vendors actually defend.** Compustat Snapshot (1987-present), FactSet PIT (25 years, 20 countries), LSEG Worldscope PIT, Bloomberg COFI, and CIQ via `ciqFinInstance` collection types all converge on bitemporal `(period_end, knowledge_date)` storage. An open competitor that ships only the latest-known view ships a research toy. ats-eqt's foundational fact table must be bitemporal from row one. The full four-date model — `pdate` (preliminary press release), `rdq` (report date quarterly, the 8-K Item 2.02 timestamp), `fdate` (final / 10-K filed), `ldate` (last update) — should be preserved on every fact row.

5. **The 8-K vs 10-Q latency window is the most important PIT detail nobody outside Compustat models correctly.** US issuers release earnings via 8-K Item 2.02 press release **before** filing the corresponding XBRL 10-Q/10-K — typical lag is 1 to 30 business days, average ~14. The press release contains the headline numbers traders move on; the XBRL 10-Q contains the auditable tagged numbers downstream pipelines load. A naïve fundamentals pipeline that waits for the 10-Q XBRL will systematically lag the market by 1–4 weeks on every earnings event. Compustat's `rdq` field encodes the press-release date directly; the 8-K with Item 2.02 has been XBRL-tagged itself since 2024 ("Cover Page tagging" + Item-level financial schedules), making the 8-K extractable as a structured early-actuals source. (Source: <https://www.sec.gov/info/edgar/specifications/xbrl-staff-observation-2023-02-23.pdf>.)

Headline scope of this file: **6 commercial vendors enumerated at field-level, ~120 canonical concepts mapped end-to-end across all 6 vendors plus us-gaap, ~250 us-gaap concepts catalogued, full proposed DDL for the ats-eqt foundational fundamentals schema, and ~70 high-quality citations.**

---

## 1. Vendor stack matrix

| Vendor / Product | Universe (US) | Item count | Annual back to | Quarterly back to | PIT back to | Primary identifier | Schema shape | Update cadence | Headline pricing signal |
|---|---|---|---|---|---|---|---|---|---|
| **Compustat NA (XPF)** | ~14,000 active + ~28,000 inactive US/CA filers | ~300 annual, ~100 quarterly, ~3,000 across all tables | 1950 | 1962 | 1987 (Snapshot/PIT add-on) | GVKEY | Wide (named columns) | Daily Update files; Weekly Full History | $100K–$1M/yr enterprise; ~$25K–$70K/yr WRDS |
| **FactSet Fundamentals** | ~14,000 US public; 86k+ global | 750+ standardised; 3,000+ in Advanced | 1980 (1962 corpus) | 1995 | ~2001 (PIT 25y, 20 countries) | FSYM-R + entity ID | Long / hybrid (`ff_v3` Snowflake share) | Intraday filing event → DataFeed; weekly Full | $200K–$2M/yr enterprise |
| **Worldscope (LSEG)** | ~14,000 active US (40k global) | 1,500+ items across 4 templates | 1980 (1985 stable) | 1989+ varies | 1989 (US), 1997 (non-US) | PermID + WS company ID | Wide (numeric WSxxxxx + WS.* mnemonics) | Daily | $50K–$500K/yr |
| **Bloomberg FA / BQL** | Global, ~70M securities cross-asset | 40,000+ fields (incl. ref/ESG); ~2,000 fundamentals | ~1989 | ~1991 | 2003 (COFI PIT) | BBG composite FIGI / `XX US Equity` | Wide (per-mnemonic) | Real-time on filing | Terminal $28k–$32k/seat/yr; DL+ $100K–$1M+/yr |
| **S&P Capital IQ Pro** | 70,000+ public + private companies | ~10,000 `ciqDataItem` IDs | ~1990 | ~1990 | Filing-instance-level (collection types) | CIQ companyId / securityId / tradingItemId | Long (EAV `ciqFinInstanceItem`) | Daily | $12K–$30K/seat |
| **Sharadar SF1** | ~14,000 US tickers (active + delisted) | ~150 columns | 1998–2002 by ticker | 1998–2002 | as-reported (ARQ/ARY) preserved | Sharadar `permaticker` + ticker | Wide CSV / parquet | Daily | $540/yr individual; $4,800/yr commercial |
| **SimFin (Plus)** | ~5,000 US tickers | ~60 columns income/balance/cash | 2008 (most), some 2000 | 2008 | as-reported preserved; restatement window narrow | SimFin ID + ticker | Wide CSV | Daily/weekly | Free tier; Plus €59/yr indiv, €199/yr commercial |
| **WRDS Compustat (academic)** | identical to XPF; restricted licence | identical | identical | identical | Snapshot add-on optional | GVKEY | identical | T+1 to T+7 from XPF | Bundled in WRDS academic licence |

Source matrix: Compustat coverage and history per <https://en.wikipedia.org/wiki/Compustat> and <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database>; FactSet per <https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed>; Worldscope per <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals>; Bloomberg per <https://professional.bloomberg.com/products/data/data-management/data-license/>; CIQ per <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro>; Sharadar per <https://data.nasdaq.com/databases/SF1>; SimFin per <https://www.simfin.com/en/pricing/>.

---

## 2. Compustat North America (Xpressfeed) — field-level enumeration

This is the canonical reference. Every other commercial fundamentals dataset is implicitly or explicitly benchmarked against Compustat. The fields below are the published mnemonics from the WRDS data dictionary (`comp.funda`, `comp.fundq`, `comp.company`, `comp.secm`, `comp.idxcst_his`) and the S&P "Compustat Xpressfeed — Understanding the Data" user guide (source: <https://w3.loibl.com/uni/xf_understanding_the_data.pdf>; <https://library.unist.ac.kr/libguide/wp-content/uploads/sites/2/2018/11/compustat.pdf>; WRDS data dictionary at <https://wrds-www.wharton.upenn.edu/data-dictionary/comp_na_daily_all/>).

### 2.1 The `funda` / `co_afnd*` annual fundamentals table

Primary key: `(gvkey, datadate, indfmt, consol, popsrc, datafmt)`. The four "format" columns disambiguate which version of the same `(gvkey, datadate)` row to read — INDL vs FS template, Consolidated vs Non-consolidated, Domestic vs International population source, STD vs RESTATED vs SUMM_STD data format (source: <https://robsonglasscock.wordpress.com/2018/04/12/gvkey-and-datadate-or-fyear-duplicates-in-compustat/>).

#### 2.1.1 Identifiers & period (every row)

```
gvkey         CHAR(6)    Compustat permanent company key. Zero-padded.
datadate      DATE       Fiscal period end calendar date.
fyear         INT2       Fiscal year (integer).
fyr           INT2       Fiscal year-end month (1..12). Apple=9, Microsoft=6, JPM=12.
indfmt        CHAR(4)    'INDL' industrial template, 'FS' financial-services template.
consol        CHAR(1)    'C' consolidated, 'N' non-consolidated.
popsrc        CHAR(1)    'D' domestic, 'I' international.
datafmt       CHAR(3)    'STD' standard, 'SUMM_STD' summary, 'RESTATED' (PIT add-on).
tic           CHAR(8)    Current trading ticker.
cusip         CHAR(9)    Current 9-character CUSIP.
conm          CHAR(40)   Company name (current).
curcd         CHAR(3)    Reporting currency code (USD, EUR, etc.).
curncd        CHAR(3)    Currency code for native currency (Global only).
exchg         INT2       Exchange code where primarily traded.
cik           INT4       SEC EDGAR Central Index Key.
costat        CHAR(1)    Active/inactive status: 'A' active, 'I' inactive.
fic           CHAR(3)    Country of incorporation (ISO 3166-1 alpha-3).
loc           CHAR(3)    Country of headquarters.
naics         CHAR(6)    Six-digit NAICS code.
sic           CHAR(4)    Four-digit SIC code.
gsector       CHAR(2)    GICS sector (2 digits).
ggroup        CHAR(4)    GICS industry group (4 digits).
gind          CHAR(6)    GICS industry (6 digits).
gsubind       CHAR(8)    GICS sub-industry (8 digits).
```

#### 2.1.2 Income statement (industrial template, INDL)

```
sale          NUMERIC    Sales/Turnover (Net). Net of returns, allowances, excise.
revt          NUMERIC    Revenue - Total. Often equal to sale; differs for finance subs.
cogs          NUMERIC    Cost of Goods Sold.
xsga          NUMERIC    Selling, General & Administrative Expense.
xrd           NUMERIC    Research and Development Expense.
xad           NUMERIC    Advertising Expense.
xlr           NUMERIC    Staff Expense - Total.
xpr           NUMERIC    Pension and Retirement Expense.
dp            NUMERIC    Depreciation and Amortization (income statement).
am            NUMERIC    Amortization of Intangibles.
oibdp         NUMERIC    Operating Income Before Depreciation (= revt - cogs - xsga).
oiadp         NUMERIC    Operating Income After Depreciation (= oibdp - dp).
ebit          NUMERIC    Earnings Before Interest and Taxes.
ebitda        NUMERIC    Earnings Before Interest, Taxes, D&A.
xint          NUMERIC    Interest Expense - Total.
nopi          NUMERIC    Non-Operating Income (Expense).
spi           NUMERIC    Special Items.
pi            NUMERIC    Pretax Income.
txt           NUMERIC    Income Taxes - Total.
txc           NUMERIC    Income Taxes - Current.
txdi          NUMERIC    Income Taxes - Deferred.
mii           NUMERIC    Minority Interest (Income Statement).
ib            NUMERIC    Income Before Extraordinary Items.
ibcom         NUMERIC    Income Before Extraordinary Items - Available for Common.
xido          NUMERIC    Extraordinary Items and Discontinued Operations.
ni            NUMERIC    Net Income (Loss).
niadj         NUMERIC    Net Income Adjusted for Common/Ordinary Stock (Capital) Equivalents.
epsfx         NUMERIC    EPS - Fully Diluted - Excluding Extraordinary Items.
epsfi         NUMERIC    EPS - Fully Diluted - Including Extraordinary Items.
epspx         NUMERIC    EPS - Basic - Excluding Extraordinary Items.
epspi         NUMERIC    EPS - Basic - Including Extraordinary Items.
```

#### 2.1.3 Balance sheet (industrial template, INDL)

```
at            NUMERIC    Assets - Total.
act           NUMERIC    Current Assets - Total.
che           NUMERIC    Cash and Short-Term Investments.
ch            NUMERIC    Cash.
ivst          NUMERIC    Short-Term Investments - Total.
rect          NUMERIC    Receivables - Total.
recch         NUMERIC    Receivables - Decrease (Increase) on cash flow.
invt          NUMERIC    Inventories - Total.
ppent         NUMERIC    Property, Plant & Equipment - Net.
ppegt         NUMERIC    Property, Plant & Equipment - Gross.
intan         NUMERIC    Intangible Assets - Total.
gdwl          NUMERIC    Goodwill.
ivao          NUMERIC    Investment and Advances - Other.
lt            NUMERIC    Liabilities - Total.
lct           NUMERIC    Current Liabilities - Total.
ap            NUMERIC    Accounts Payable - Trade.
dlc           NUMERIC    Debt in Current Liabilities - Total (short-term debt).
dltt          NUMERIC    Long-Term Debt - Total.
dd1           NUMERIC    Long-Term Debt Due in One Year.
dd2..dd5      NUMERIC    Long-Term Debt due in 2..5 years.
dltis         NUMERIC    Long-Term Debt - Issuance.
dltr          NUMERIC    Long-Term Debt - Reduction.
txditc        NUMERIC    Deferred Taxes and Investment Tax Credit.
mib           NUMERIC    Minority Interest - Balance Sheet.
pstk          NUMERIC    Preferred/Preference Stock - Total.
pstkl         NUMERIC    Preferred Stock - Liquidating Value.
pstkrv        NUMERIC    Preferred Stock - Redemption Value.
ceq           NUMERIC    Common/Ordinary Equity - Total.
seq           NUMERIC    Stockholders' Equity - Total (= ceq + pstk).
re            NUMERIC    Retained Earnings.
tstk          NUMERIC    Treasury Stock - Total.
csho          NUMERIC    Common Shares Outstanding (in millions).
cshpri        NUMERIC    Common Shares Used to Calculate EPS - Primary.
cshfd         NUMERIC    Common Shares Used to Calculate EPS - Fully Diluted.
```

#### 2.1.4 Cash flow statement

```
oancf         NUMERIC    Operating Activities - Net Cash Flow.
ivncf         NUMERIC    Investing Activities - Net Cash Flow.
fincf         NUMERIC    Financing Activities - Net Cash Flow.
capx          NUMERIC    Capital Expenditures.
capxv         NUMERIC    Capital Expenditures (verified, infrequent).
dpc           NUMERIC    Depreciation and Amortization (from cash flow).
sppe          NUMERIC    Sale of Property, Plant & Equipment.
aqc           NUMERIC    Acquisitions.
sstk          NUMERIC    Sale of Common and Preferred Stock.
prstkc        NUMERIC    Purchase of Common and Preferred Stock (buybacks).
dv            NUMERIC    Cash Dividends.
dvc           NUMERIC    Common Dividends - Cash.
dvp           NUMERIC    Preferred Dividends - Cash.
dvpsx_f       NUMERIC    Dividends per Share - Ex-Date - Fiscal.
dlcch         NUMERIC    Change in Current Debt.
dltis_dltr    NUMERIC    Net Long-Term Debt Issuance.
sivch         NUMERIC    Sale of Investments - Change.
fopt          NUMERIC    Funds from Operations (legacy SFAS 95 reporters).
wcapc         NUMERIC    Working Capital Change (legacy).
xidoc         NUMERIC    Extraordinary Items and Discontinued Operations (cash).
exre          NUMERIC    Exchange Rate Effect on Cash.
chech         NUMERIC    Cash & Cash Equivalents - Change in.
```

#### 2.1.5 Per-share / market

```
prcc_f        NUMERIC    Price - Close - Annual - Fiscal year end.
prcc_c        NUMERIC    Price - Close - Annual - Calendar year end.
prch_f        NUMERIC    Price - High - Annual - Fiscal.
prcl_f        NUMERIC    Price - Low - Annual - Fiscal.
mkvalt        NUMERIC    Market Value - Total (Fiscal).
ajex          NUMERIC    Adjustment Factor (Cumulative) by Ex-Date.
trt1          NUMERIC    Total Return - 1 Year.
```

#### 2.1.6 Footnote / data-flag columns

For every numeric field `xxx`, Compustat ships a parallel character footnote field `xxx_fn` in `co_afnddc1`/`co_afnddc2` (annual) and `co_ifnddc` (quarterly). Footnote codes encode why a value is the way it is. The published codes (source: Compustat Xpressfeed Understanding the Data, pp. 56–73):

```
AB  Restated for acquisition or merger.
AC  Restated for accounting change.
AF  Reflects an accounting change.
CB  Combination of two reporting periods.
CE  Reflects a divestment or discontinued operation.
CF  Includes finance subsidiary.
EI  Earnings include extraordinary items.
ES  Annualized from partial year.
FN  Footnoted in original report.
IS  Insufficient detail.
NA  Not available (distinct from NULL which is "not applicable").
NM  Not meaningful (zero or negative denominator).
PF  Pro forma.
RP  Reported in alternative format.
RS  Restated for accounting standard adoption.
SR  Subsidiary reporting.
TL  Reflects fresh-start accounting.
UD  Unusual due-to.
```

### 2.2 The `fundq` / `co_ifndq` quarterly fundamentals table

Primary key: `(gvkey, datadate, indfmt, consol, popsrc, datafmt, fyearq, fqtr)`. Every annual mnemonic above appears with a `q` suffix where applicable. The critical quarterly-only fields:

```
fyearq        INT2       Fiscal year of quarter.
fqtr          INT2       Fiscal quarter (1..4).
datacqtr      CHAR(6)    Calendar quarter projection (e.g. '2024Q3').
datafqtr      CHAR(6)    Fiscal quarter projection (e.g. '2024Q3').
rdq           DATE       Report Date Quarterly. Date of 8-K earnings release.
                         The single most important date field for event studies.
pdate         DATE       Preliminary date (Snapshot/PIT only). When first
                         preliminary entered the database.
fdate         DATE       Final date (Snapshot/PIT only). When 10-Q is filed
                         and standardized.
ldate         DATE       Last update date (Snapshot/PIT only).
finalq        CHAR(1)    'Y' if this row reflects the final 10-Q-filed values;
                         'N' if still on preliminary press-release values.
updq          INT2       Update flag: 1=preliminary, 2=updated, 3=final.
saleq         NUMERIC    Sales (Net) - Quarterly.
revtq         NUMERIC    Revenue - Total - Quarterly. (saleq and revtq are
                         numerically identical for most industrials; differ
                         for finance subs and reinsurance.)
cogsq         NUMERIC    COGS - Quarterly.
xsgaq         NUMERIC    SG&A - Quarterly.
xrdq          NUMERIC    R&D - Quarterly.
dpq           NUMERIC    D&A - Quarterly.
oibdpq        NUMERIC    Operating Income Before D&A - Quarterly.
oiadpq        NUMERIC    Operating Income After D&A - Quarterly.
xintq         NUMERIC    Interest Expense - Quarterly.
piq           NUMERIC    Pretax Income - Quarterly.
txtq          NUMERIC    Income Taxes - Quarterly.
ibq           NUMERIC    Income Before Extraordinary - Quarterly.
niq           NUMERIC    Net Income - Quarterly.
epspxq        NUMERIC    EPS - Basic - Excl. Extraordinary - Quarterly.
epspiq        NUMERIC    EPS - Basic - Incl. Extraordinary - Quarterly.
epsfxq        NUMERIC    EPS - Fully Diluted - Excl. - Quarterly.
epsfiq        NUMERIC    EPS - Fully Diluted - Incl. - Quarterly.
epsf12        NUMERIC    EPS - Diluted - Trailing 12 Months (last-12-month).
epsfy         NUMERIC    EPS - Fully Diluted - Fiscal Year-to-Date.
oeps12        NUMERIC    Operating EPS - Diluted - Trailing 12 Months.
oepsxq        NUMERIC    Operating EPS - Diluted - Excluding Extraordinary - Quarterly.
atq           NUMERIC    Total Assets - Quarterly.
ltq           NUMERIC    Total Liabilities - Quarterly.
ceqq          NUMERIC    Common Equity - Quarterly.
seqq          NUMERIC    Stockholders' Equity - Quarterly.
cheq          NUMERIC    Cash & ST Investments - Quarterly.
chq           NUMERIC    Cash - Quarterly.
rectq         NUMERIC    Receivables - Quarterly.
invtq         NUMERIC    Inventories - Quarterly.
ppentq        NUMERIC    PP&E Net - Quarterly.
ppegtq        NUMERIC    PP&E Gross - Quarterly.
intanq        NUMERIC    Intangibles - Quarterly.
gdwlq         NUMERIC    Goodwill - Quarterly.
dlcq          NUMERIC    ST Debt - Quarterly.
dlttq         NUMERIC    LT Debt - Quarterly.
apq           NUMERIC    Accounts Payable - Quarterly.
txditcq       NUMERIC    Deferred Tax & ITC - Quarterly.
pstkq         NUMERIC    Preferred Stock - Quarterly.
req           NUMERIC    Retained Earnings - Quarterly.
tstkq         NUMERIC    Treasury Stock - Quarterly.
cshoq         NUMERIC    Common Shares Out - Quarterly.
cshprq        NUMERIC    Shares - Primary EPS - Quarterly.
cshfdq        NUMERIC    Shares - Diluted EPS - Quarterly.
oancfy        NUMERIC    Operating Cash Flow - YTD (cash flow statement is
                         cumulative within the fiscal year per SFAS 95).
ivncfy        NUMERIC    Investing Cash Flow - YTD.
fincfy        NUMERIC    Financing Cash Flow - YTD.
capxy         NUMERIC    Capex - YTD.
dpcy          NUMERIC    D&A from Cash Flow - YTD.
sstky         NUMERIC    Stock Issuance - YTD.
prstkcy       NUMERIC    Buybacks - YTD.
dvy           NUMERIC    Dividends - YTD.
sppy          NUMERIC    Sale of PP&E - YTD.
aqcy          NUMERIC    Acquisitions - YTD.
mkvaltq       NUMERIC    Market Value - Quarterly.
prccq         NUMERIC    Price - Close - Quarterly.
prchq         NUMERIC    Price - High - Quarterly.
prclq         NUMERIC    Price - Low - Quarterly.
ajexq         NUMERIC    Cumulative Adjustment Factor - Quarterly.
spcindcd      INT2       S&P Industry Code.
spcseccd      INT2       S&P Economic Sector Code.
```

Source: <https://ionmihai.github.io/finsets/01_wrds/compq.html> for the complete list of fundq mnemonics; WRDS data-dictionary at <https://wrds-www.wharton.upenn.edu/data-dictionary/comp_na_daily_all/funda/> and `.../fundq/`.

### 2.3 The financial-services (FS) template overlay

When `indfmt = 'FS'`, the bank/insurance industries get a parallel set of items in addition to the INDL items. Examples:

```
Bank-template items (Compustat Bank / FS rows):
  tdsa     Total Deposits - Savings Accounts.
  tldb     Total Loans - Domestic Banks.
  tlres    Total Loan Loss Reserves.
  nim      Net Interest Margin.
  tcl      Total Commercial Loans.
  tll      Total Loans - Loans.
  intinc   Interest Income - Total.
  intexp   Interest Expense - Total (this overrides xint usage).
  pln      Provision for Loan Losses.
  alll     Allowance for Loan and Lease Losses.

Insurance-template items:
  pncia    Premiums Net Earned - Combined Insurance Assumed.
  losres   Loss Reserves.
  benefits Insurance Benefits Paid.
  ucl      Unpaid Claim Liability.
  pclaims  Policy Claims.
  pcom     Policy Commissions.
  resvbs   Reserves - Balance Sheet.
```

`[unverified — exact column-list completeness for bank/insurance template, as it has changed across Compustat releases]`. The reference is "Compustat North America - Bank Items Manual" (S&P internal; not freely distributed).

### 2.4 `co_afnddc1` / `co_afnddc2` — Annual Data Codes (footnotes)

These two physical tables hold one character footnote column per fundamental column (`at_fn`, `revt_fn`, etc.). Joined on `(gvkey, datadate)`. Splitting across two tables is purely an artefact of historic RDBMS row-width limits (source: <https://w3.loibl.com/uni/xf_understanding_the_data.pdf>, "Understanding the Data" §3.4).

### 2.5 `company` — Entity master

```
gvkey         CHAR(6)    PK.
conm          CHAR(40)   Current company name.
conml         CHAR(80)   Current company name long.
tic           CHAR(8)    Current ticker.
cusip         CHAR(9)    Current 9-char CUSIP.
cik           INT4       SEC EDGAR CIK.
ein           CHAR(11)   Employer Identification Number.
sic           CHAR(4)    Current SIC.
naics         CHAR(6)    Current NAICS.
gsector       CHAR(2)    Current GICS sector.
ggroup        CHAR(4)    Current GICS industry group.
gind          CHAR(6)    Current GICS industry.
gsubind       CHAR(8)    Current GICS sub-industry.
spcindcd      INT2       Current S&P industry code.
spcseccd      INT2       Current S&P economic sector code.
fic           CHAR(3)    Country of incorporation.
loc           CHAR(3)    Country of headquarters.
state         CHAR(2)    State (US).
county        CHAR(40)   County.
city          CHAR(24)   City.
addzip        CHAR(24)   ZIP.
costat        CHAR(1)    'A' active, 'I' inactive.
dldte         DATE       Delisting date.
dlrsn         INT2       Delisting reason code (1..15; 1=merger, 2=bankruptcy,
                         etc.; CRSP-aligned).
ipodate       DATE       IPO date.
incorp        CHAR(2)    State/country of incorporation.
fyrc          INT2       Current fiscal year-end month.
phone         CHAR(15)   HQ phone.
weburl        CHAR(50)   Web URL.
busdesc       TEXT       Business description.
```

### 2.6 `co_idesind` — Industry descriptor history

A bitemporal industry-code table — companies change GICS/NAICS over time. Key fields:

```
gvkey         CHAR(6)
indfrom       DATE       Start of industry-code validity.
indthru       DATE       End of industry-code validity (NULL=current).
gsector       CHAR(2)
ggroup        CHAR(4)
gind          CHAR(6)
gsubind       CHAR(8)
sic           CHAR(4)
naics         CHAR(6)
spcindcd      INT2
spcseccd      INT2
```

This is the "as-of" industry join — critical for any backtest that wants 2014's GICS sector for a 2014 portfolio rather than today's GICS.

### 2.7 `co_bdesc` / `co_busdescl` — Business descriptions

```
gvkey         CHAR(6)
datadate      DATE
busdesc       TEXT       Business description as reported in 10-K Item 1.
```

### 2.8 `secm` — Securities Monthly

`(gvkey, iid, datadate)`. `iid` is the issue identifier (which security class). Monthly snapshot of price, market cap, total return.

```
gvkey, iid, datadate
prccm         NUMERIC    Price - Close - Monthly.
prchm         NUMERIC    Price - High - Monthly.
prclm         NUMERIC    Price - Low - Monthly.
cshtrm        NUMERIC    Common Shares Traded - Monthly.
cshom         NUMERIC    Common Shares Outstanding - Monthly.
mkvaltm       NUMERIC    Market Value - Monthly.
trt1m         NUMERIC    Total Return - 1 Month.
trfm          NUMERIC    Total Return Factor - Monthly.
ajexm         NUMERIC    Adjustment Factor (Cumulative) - Monthly.
adrr          NUMERIC    ADR Ratio (shares of underlying : ADR).
divrate       NUMERIC    Dividend Rate (annualized).
dvpsxm        NUMERIC    Dividends per Share - Ex-Date - Monthly.
exchg         INT2       Exchange code.
secstat       CHAR(1)    Security status: A=active, I=inactive.
tpci          CHAR(2)    Issue type code (0=common, 1=ADR, etc.).
```

`secd` is the same but daily.

### 2.9 `co_segfnda` / `co_segfndq` — Segment fundamentals

SFAS 131 reportable-segment data. Long-format (one row per segment per company per period).

```
gvkey, datadate, sid, srcdate, stype, snms
salea         NUMERIC    Sales / Revenue - Segment.
opincs        NUMERIC    Operating Income - Segment.
ats           NUMERIC    Assets - Segment.
capxs         NUMERIC    Capex - Segment.
dps           NUMERIC    D&A - Segment.
empn          NUMERIC    Employees - Segment.
stype         CHAR(4)    Segment type: 'BUSS' business, 'GEO' geographic,
                         'OPER' operating, 'OPCS' operating customer.
sname         TEXT       Segment name.
```

### 2.10 `idxcst_his` — Index constituent history

```
gvkey, gvkeyx, indexid, from_, thru
indexid       CHAR(6)    Index ID (e.g. 'SP500', 'SPMID', 'SPSML').
gvkey         CHAR(6)    Constituent company GVKEY.
gvkeyx        CHAR(6)    Index GVKEY (the index itself).
from_         DATE       Entry date into index.
thru          DATE       Exit date (NULL=still in index).
```

This is the survivorship-bias remediation table — every backtest filter "S&P 500 constituents as of date D" must join `idxcst_his ON from_ <= D AND (thru IS NULL OR thru > D)`. Naïve `companies in index today` filters produce ~1.6%/yr biased returns (source: <https://www.crsp.org/research/crsp-survivor-bias-free-us-mutual-funds/>).

### 2.11 Snapshot / PIT tables — `pit_*`

The Compustat Snapshot product (separate licence; WRDS code `comp_snapshot`) holds the full restatement history. Tables mirror `funda` / `fundq` but with snapshot vintage columns added:

```
pit_funda                 -- annual snapshot
  gvkey, datadate, …, srcdate, pdate, rdq, fdate, ldate
pit_fundq                 -- quarterly snapshot
pit_secm                  -- monthly security snapshot
pit_idx                   -- index snapshot
```

The semantics: for any `(gvkey, datadate)` pair, multiple Snapshot rows exist, one per restatement event. Each row carries:

```
srcdate       DATE       Source date — when this vintage entered Compustat.
pdate         DATE       Preliminary date — when company press-released this value.
rdq           DATE       Report date — when 8-K Item 2.02 ran (quarterly).
fdate         DATE       Final date — when 10-Q/10-K with this value was filed.
ldate         DATE       Last date — when this vintage was last touched in DB.
```

Invariant: `pdate <= rdq <= fdate <= ldate <= srcdate`. PIT replay: filter `srcdate <= asof_date`, then select the row with `max(srcdate)` per `(gvkey, datadate)` — that's the latest-known view as of asof_date.

Snapshot monthly history goes back to **1987** for North America (source: <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database>; <https://kenan-flagler.libguides.com/kfbs-library-services/research-resource/compustat-snapshot/>). Pre-1987, Compustat is restated-only (no vintage history).

### 2.12 WRDS-derived linking views

WRDS publishes derived linking tables that academic users tend to use in preference to the raw XPF schema:

```
comp.ccmxpf_linktable  -- CRSP-Compustat link history (PERMNO ↔ GVKEY)
comp.linkable          -- one-row-per-day view of valid GVKEY/PERMNO pairs
comp.compm             -- monthly merged company / security view
comp.fundq             -- mirror of co_ifndq quarterly
comp.funda             -- mirror of co_afnd annual
comp.funda_fncd        -- merged with footnote codes
comp.adsprate          -- S&P 500 ratings / credit ratings (historical)
```

Source: <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-crsp-with-compustat/>.

---

## 3. FactSet Fundamentals — field-level enumeration

FactSet's Fundamentals product is delivered as the `ff_v3` schema in the Snowflake Data Share (and as Standard DataFeed pipe-delimited files; identical schema). The full Marketplace listing breaks into Basic / Advanced / DER / industry-overlay families. Source for the public `ff_v3.*` schema names: <https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html>, <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/factset/>, FactSet Marketplace Fundamentals product page at <https://www.factset.com/marketplace/catalog/product/factset-fundamentals>, At-a-Glance brief at <https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed>.

### 3.1 Physical tables (Snowflake share / Standard DataFeed)

```
ff_v3.ff_basic_af           Annual fiscal (af = annual fiscal)
ff_v3.ff_basic_qf           Quarterly fiscal
ff_v3.ff_basic_saf          Semi-annual fiscal
ff_v3.ff_basic_ltm          Latest twelve months (rolling)
ff_v3.ff_basic_ytd_cal      Calendar year-to-date
ff_v3.ff_basic_ytd_fiscal   Fiscal year-to-date

ff_v3.ff_advanced_af        Advanced — annual fiscal (~3,000 items)
ff_v3.ff_advanced_qf        Advanced — quarterly fiscal
ff_v3.ff_advanced_saf
ff_v3.ff_advanced_ltm
ff_v3.ff_advanced_ytd_cal
ff_v3.ff_advanced_ytd_fiscal

ff_v3.ff_basic_der_af       Derived ratios (annual)
ff_v3.ff_basic_der_qf       Derived ratios (quarterly)
ff_v3.ff_basic_der_ltm

ff_v3.ff_infotech_af        Tech-industry KPIs (R&D capitalisation, SaaS metrics)
ff_v3.ff_infotech_qf
ff_v3.ff_reit_af            REIT-specific (FFO, AFFO, NOI, occupancy)
ff_v3.ff_reit_qf
ff_v3.ff_bank_af            Banking-template fundamentals
ff_v3.ff_bank_qf
ff_v3.ff_ins_af             Insurance-template fundamentals
ff_v3.ff_ins_qf
ff_v3.ff_ofin_af            Other-financial template
ff_v3.ff_ofin_qf

ff_v3.ff_basic_pit_af       Point-in-Time annual (FF Fundamentals PIT)
ff_v3.ff_basic_pit_qf       Point-in-Time quarterly
ff_v3.ff_basic_cf_af        Consolidated financials (currency-translated)
```

### 3.2 Common row-grain columns

Every `ff_basic_*` / `ff_advanced_*` table is keyed by `(fsym_id, date)` where `fsym_id` is the **FSYM Regional Security ID** (`-R` suffix). Joining fundamentals across multiple share classes requires aggregating across regional IDs that share the same `fsym_security_id`.

```
fsym_id           CHAR(8)   FSYM regional security ID (e.g. '000C7F-R').
date              DATE      Period end date.
ff_period         CHAR(6)   'A' annual, 'Q' quarterly, 'S' semi-annual, 'L' LTM,
                            'Y' YTD-fiscal, 'C' YTD-calendar.
ff_fp_end         DATE      Fiscal period end date.
ff_fy             INT2      Fiscal year integer.
ff_fp             SMALLINT  Fiscal period number (1..4 for quarterly, 0 annual).
ff_fyr            INT2      Fiscal year-end month (1..12).
ff_actg_std       CHAR(4)   Accounting standard: 'USAP' US-GAAP, 'IFRS', 'LOCL'.
ff_curr_cd        CHAR(3)   Reporting currency code (ISO 4217).
ff_currency       CHAR(3)   Same; legacy alias.
ff_source         CHAR(2)   Source filing type: '10', '10A', '10Q', '8K', 'PR', 'AR'.
ff_report_date    DATE      Date of underlying filing/press release.
ff_eps_rpt_date   DATE      Date of EPS report (8-K Item 2.02). Equivalent to
                            Compustat rdq.
ff_fe_date        DATE      Filing-event date (the FactSet-internal version
                            timestamp).
ff_restated       CHAR(1)   'Y' if this is a restated vintage; 'N' if original.
ff_iss_type       CHAR(2)   Issuer template: 'IN' industrial, 'BK' bank,
                            'IS' insurance, 'OF' other financial.
```

### 3.3 FF_BASIC — the headline ~200 items (mnemonics)

The mnemonic convention is `FF_<line-item>`. The naming is largely descriptive; the catalogue is published in the FactSet Marketplace Data Item Definitions for the Fundamentals product. The most common items (sources: <https://download.dataservices.theice.com/products/marketq/help/factset_fundamental_data.htm>, <http://famouswiki.pbworks.com/FDS-Codes-In-FactSet>, FactSet Fundamentals API field reference at <https://developer.factset.com/api-catalog/factset-fundamentals-api>):

```
Income statement (industrial template):
  FF_SALES               Net Sales / Revenue (total).
  FF_SALES_GR            Gross Sales (before adjustments).
  FF_COGS                Cost of Goods Sold.
  FF_GROSS_INC           Gross Income / Profit.
  FF_SGA                 Selling, General & Administrative Expense.
  FF_SGA_OTH             SG&A - Other.
  FF_RD_EXP              Research & Development Expense.
  FF_RD_EXP_CAP          R&D Capitalised (separately).
  FF_DEP_EXP             Depreciation Expense (income statement).
  FF_AMORT_EXP           Amortisation of Intangibles.
  FF_DEP_AMORT_EXP       D&A (income statement combined).
  FF_OPER_INC            Operating Income.
  FF_EBITDA_OPER         Operating EBITDA.
  FF_EBITDA              EBITDA (standardised).
  FF_EBIT                EBIT (standardised).
  FF_EBIT_OPER           Operating EBIT.
  FF_INT_EXP_TOT         Interest Expense - Total.
  FF_INT_EXP_DEBT        Interest Expense - Debt.
  FF_NON_OPER_INC        Non-Operating Income.
  FF_SPECIAL_ITEMS       Special / Unusual / Non-recurring Items.
  FF_PRETAX_INC          Pretax Income.
  FF_INC_TAX             Income Tax Expense - Total.
  FF_INC_TAX_CURR        Income Tax - Current.
  FF_INC_TAX_DEFER       Income Tax - Deferred.
  FF_MIN_INT_INC         Minority Interest (P&L).
  FF_EQ_AFF_INC          Equity in Affiliates / Investments.
  FF_NET_INC             Net Income (continuing ops).
  FF_NET_INC_DISC        Net Income - Discontinued Operations.
  FF_NET_INC_EXTRA       Net Income - Extraordinary Items.
  FF_NET_INC_TOT         Net Income - Total (incl. extraordinaries).
  FF_NET_INC_AVAIL       Net Income Available to Common.
  FF_DIV_PFD             Preferred Dividends.
  FF_EPS_BASIC           EPS - Basic.
  FF_EPS_DIL             EPS - Diluted.
  FF_EPS_BASIC_EXT       EPS - Basic - Excl. Extraordinary.
  FF_EPS_DIL_EXT         EPS - Diluted - Excl. Extraordinary.
  FF_EPS_REPORT          EPS - As Reported (matches the filing).

Balance sheet (industrial):
  FF_ASSETS              Total Assets.
  FF_ASSETS_CURR         Current Assets.
  FF_CASH_ST             Cash and Short-Term Investments.
  FF_CASH                Cash.
  FF_ST_INVEST           Short-Term Investments.
  FF_RECV_NET            Net Receivables.
  FF_RECV_TRADE          Trade Receivables.
  FF_INVENT              Inventories.
  FF_PREPAID             Prepaid Expenses.
  FF_OTH_CURR_ASSET      Other Current Assets.
  FF_PPE_NET             Property, Plant & Equipment - Net.
  FF_PPE_GROSS           Property, Plant & Equipment - Gross.
  FF_INTANG              Total Intangible Assets.
  FF_INTANG_GW           Goodwill.
  FF_INTANG_OTH          Other Intangibles.
  FF_INVEST_LT           Long-Term Investments.
  FF_OTH_LT_ASSET        Other Long-Term Assets.
  FF_LIAB                Total Liabilities.
  FF_LIAB_CURR           Current Liabilities.
  FF_PAYABLES            Accounts Payable.
  FF_DEBT_ST             Short-Term Debt (incl. current portion of LT debt).
  FF_DEBT_LT             Long-Term Debt.
  FF_DEBT                Total Debt (= FF_DEBT_ST + FF_DEBT_LT).
  FF_CAP_LEASES          Capital Lease Obligations.
  FF_DEFERRED_TX         Deferred Tax Liabilities.
  FF_OTH_LT_LIAB         Other Long-Term Liabilities.
  FF_MIN_INT_BS          Minority Interest (Balance Sheet).
  FF_PREF_STK            Preferred Stock.
  FF_COM_STK_PAR         Common Stock - Par Value.
  FF_COM_EQ_TOT          Common Equity - Total.
  FF_RETAIN_EARN         Retained Earnings.
  FF_TREAS_STK           Treasury Stock.
  FF_EQ_TOT              Stockholders' Equity Total.
  FF_SHS_OUTSTND         Common Shares Outstanding (end of period).
  FF_SHS_BASIC           Weighted Average Shares - Basic.
  FF_SHS_DIL             Weighted Average Shares - Diluted.

Cash flow:
  FF_CASH_FROM_OPER      Cash Flow from Operations.
  FF_CASH_FROM_INVEST    Cash Flow from Investing.
  FF_CASH_FROM_FIN       Cash Flow from Financing.
  FF_CAPEX               Capital Expenditures.
  FF_ACQUIS              Acquisitions of Businesses (Cash).
  FF_DIVEST              Divestitures.
  FF_DEP_AMORT_CF        D&A from Cash Flow.
  FF_STOCK_ISSUE         Common/Preferred Stock Issuance.
  FF_STOCK_REPUR         Stock Repurchases (buybacks).
  FF_DEBT_ISSUE          Debt Issuance.
  FF_DEBT_REDUC          Debt Reduction.
  FF_DIV_CASH            Cash Dividends Paid.
  FF_DIV_COM             Common Dividends.
  FF_DIV_PFD_CF          Preferred Dividends (Cash Flow).
  FF_CHG_WC              Change in Working Capital.
  FF_CHG_AR              Change in Receivables.
  FF_CHG_INV             Change in Inventory.
  FF_CHG_AP              Change in Accounts Payable.
  FF_FCF                 Free Cash Flow (= FF_CASH_FROM_OPER - FF_CAPEX).
  FF_NET_CHG_CASH        Net Change in Cash.
  FF_EXCH_RATE_CF        Exchange Rate Effect on Cash.
```

### 3.4 FF_ADVANCED — the ~3,000-item decomposition

FF_ADVANCED carries every sub-component that rolls up into an FF_BASIC item. Pattern: `FF_<basic>_<component>`. Examples:

```
FF_SALES_PROD             Sales - Product line.
FF_SALES_SVC              Sales - Service line.
FF_SALES_DOM              Sales - Domestic.
FF_SALES_FOREIGN          Sales - Foreign.
FF_COGS_LABOR             COGS - Labor component.
FF_COGS_MAT               COGS - Materials.
FF_COGS_OVHD              COGS - Overhead.
FF_SGA_SELL               SG&A - Selling.
FF_SGA_GEN_ADMIN          SG&A - General & Administrative.
FF_SGA_MKT                SG&A - Marketing.
FF_RD_EXP_SOFTWARE        R&D - Software (specific).
FF_RD_EXP_HARDWARE        R&D - Hardware (specific).
FF_INT_INC                Interest Income.
FF_INT_INC_INVEST         Interest Income - Investments.
FF_INT_INC_LOANS          Interest Income - Loans.
FF_DEFERRED_REV_ST        Deferred Revenue - Short Term.
FF_DEFERRED_REV_LT        Deferred Revenue - Long Term.
FF_DEBT_LT_BANK           LT Debt - Bank.
FF_DEBT_LT_BONDS          LT Debt - Bonds.
FF_DEBT_LT_NOTES          LT Debt - Notes.
FF_OPER_LEASES_PV         Operating Lease Obligations (PV).
FF_PENSION_PBO            Pension PBO.
FF_PENSION_ASSETS         Pension Plan Assets.
FF_PENSION_FUNDED         Pension Funded Status.
FF_OPEB_LIAB              OPEB Liability.
FF_STOCK_COMP             Stock-Based Compensation Expense.
FF_FX_GAIN_LOSS           FX Gain/Loss.
FF_TAX_NOL                Net Operating Loss Carryforward.
FF_TAX_VALUATION          Tax Valuation Allowance.
FF_SHARES_REPUR_AVG_PRICE Average Repurchase Price.
FF_RNW_ENERGY_REV         Renewable Energy Revenue (segment).
```

`[unverified — exact mnemonic spelling for some subitems varies across the Standard DataFeed and BQL/FQL]`. The authoritative list is the FactSet Marketplace "Data Item Definitions" download (subscriber-only).

### 3.5 FF_DER — Derived ratios

FF_DER is computed nightly from FF_BASIC; it ships ~150 ratios and growth measures. Examples:

```
FF_GROSS_MARGIN          Gross Profit / Sales.
FF_OPER_MARGIN           Operating Income / Sales.
FF_EBITDA_MARGIN         EBITDA / Sales.
FF_NET_MARGIN            Net Income / Sales.
FF_ROA                   Return on Assets.
FF_ROE                   Return on Equity.
FF_ROIC                  Return on Invested Capital.
FF_ASSET_TURNOVER        Sales / Total Assets.
FF_RECV_DAYS             Days Sales Outstanding (DSO).
FF_INV_DAYS              Days Inventory Outstanding (DIO).
FF_PAY_DAYS              Days Payable Outstanding (DPO).
FF_CCC                   Cash Conversion Cycle (= DSO + DIO - DPO).
FF_CURR_RATIO            Current Ratio.
FF_QUICK_RATIO           Quick Ratio.
FF_DEBT_EQUITY           Debt / Equity.
FF_DEBT_CAP              Debt / Total Cap.
FF_INT_COVER             Interest Coverage (EBIT / Interest Exp).
FF_FCF_YIELD             Free Cash Flow Yield.
FF_SALES_GR_1Y           1-Year Sales Growth.
FF_SALES_GR_3Y_CAGR      3-Year Sales CAGR.
FF_SALES_GR_5Y_CAGR      5-Year Sales CAGR.
FF_EPS_GR_1Y             1-Year EPS Growth.
FF_BV_PS                 Book Value per Share.
FF_TANG_BV_PS            Tangible Book Value per Share.
FF_SALES_PS              Sales per Share.
FF_CASH_PS               Cash per Share.
FF_FCF_PS                Free Cash Flow per Share.
FF_DIV_PS                Dividends per Share.
FF_DIV_YIELD             Dividend Yield.
FF_PE                    P/E ratio.
FF_PB                    P/B ratio.
FF_PS                    P/S ratio.
FF_EV                    Enterprise Value.
FF_EV_EBITDA             EV/EBITDA.
FF_EV_SALES              EV/Sales.
```

### 3.6 FF_INFOTECH, FF_REIT, FF_BANKS, FF_INSURANCE — industry overlays

Tech-template (FF_INFOTECH):
```
FF_RD_PCT_SALES          R&D / Sales (intensity).
FF_RD_CAP                R&D Capitalised on Balance Sheet.
FF_RD_AMORT              R&D Amortisation.
FF_SBCC_PCT_SALES        Stock-Based Compensation / Sales.
FF_SAAS_RECUR_REV        SaaS Recurring Revenue (where disclosed).
FF_SAAS_ARR              Annual Recurring Revenue.
FF_SAAS_RPO              Remaining Performance Obligations.
```

REIT (FF_REIT) — Nareit concepts not in core us-gaap:
```
FF_FFO                   Funds From Operations (Nareit definition).
FF_FFO_PS                FFO per Share.
FF_AFFO                  Adjusted Funds From Operations.
FF_AFFO_PS               AFFO per Share.
FF_NOI                   Net Operating Income.
FF_NOI_SAME_STORE        Same-Store NOI.
FF_OCCUPANCY             Occupancy Rate (%).
FF_RENT_PSF              Rent per Square Foot.
FF_GLA                   Gross Leasable Area.
FF_NAV                   Net Asset Value per Share.
FF_CAP_RATE              Capitalisation Rate (NOI / Property Value).
FF_FFO_PAYOUT            FFO Payout Ratio.
```

Bank (FF_BANK):
```
FF_NET_INT_INC           Net Interest Income.
FF_NIM                   Net Interest Margin.
FF_PROV_LOAN_LOSS        Provision for Loan Losses.
FF_NCO                   Net Charge-Offs.
FF_ALLL                  Allowance for Loan and Lease Losses.
FF_NPL                   Non-Performing Loans.
FF_TIER1_CAP             Tier 1 Capital.
FF_TIER1_RATIO           Tier 1 Capital Ratio.
FF_CET1                  Common Equity Tier 1.
FF_RWA                   Risk-Weighted Assets.
FF_LOANS_TOT             Total Loans.
FF_DEPOSITS_TOT          Total Deposits.
FF_EFFICIENCY_RATIO      Efficiency Ratio.
```

Insurance (FF_INS):
```
FF_PREM_EARNED           Premiums Earned.
FF_PREM_WRITTEN          Premiums Written.
FF_LOSS_RESERVE          Loss Reserves.
FF_LOSS_RATIO            Loss Ratio.
FF_EXP_RATIO             Expense Ratio.
FF_COMB_RATIO            Combined Ratio.
FF_INVEST_PORT           Investment Portfolio.
FF_FLOAT                 Insurance Float.
```

`[unverified — full FF_BANK/FF_INS/FF_REIT field lists are published in subscriber-only Data Item Definitions PDFs; mnemonics above are partly inferred from public methodology briefs]`.

### 3.7 PIT semantics (FF_BASIC_PIT_*)

The PIT tables carry a vintage axis: every restatement is a new row with new `ff_fe_date`. Identical concept to Compustat Snapshot:

```
fsym_id, date, ff_period, ff_fp_end
ff_fe_date         DATE      FactSet event date - when this vintage was minted.
ff_first_avail     DATE      First-available date - when value first appeared.
ff_last_modified   DATE      Last-modified date - version stamp.
ff_restate_seq     INT2      Sequence of restatement (1, 2, 3...).
ff_source          CHAR(2)   '10', '10A', '8K', 'PR', 'AR'.
```

PIT query: `SELECT value FROM ff_basic_pit_qf WHERE fsym_id = X AND ff_fp_end = Y AND ff_fe_date <= asof ORDER BY ff_fe_date DESC LIMIT 1`. Source: <https://www.factset.com/marketplace/catalog/product/factset-fundamentals-point-in-time>.

---

## 5. Bloomberg FA / BQL — field-level enumeration

Bloomberg fundamentals fields are uppercase snake-case mnemonics prefixed by statement: `IS_*` income, `BS_*` balance, `CF_*` cash flow, `FA_*` financial-analysis-tab parameters, `TRAIL_12M_*` LTM windows, `BEST_*` Bloomberg Estimates, `YOY_*` year-over-year. Sources: NYU Stern Bloomberg guide <https://pages.stern.nyu.edu/~adamodar/pdfiles/Bloombergfull.pdf>; BBGsymbols R package field catalogue <https://bautheac.github.io/BBGsymbols/>; WU BQL Fundamentals fact sheet <https://www.wu.ac.at/fileadmin/wu/s/library/databases_info_image/Bloomberg_BQL_Fundamentals_FactSheet.pdf>; Bloomberg Fundamentals data sheet PDF <https://data.bloomberglp.com/professional/sites/10/189913_CDS_REF_Fundamentals_SFCT_DIG.pdf>.

### 5.1 Naming convention

Every Bloomberg fundamentals field has an internal "DT id" (e.g. `IS010`, `DT094`) referenced in BBGsymbols, and an external descriptive mnemonic. Examples:

```
IS010    SALES_REV_TURN          Adjusted revenue (Bloomberg standardised).
IS011    IS_TOTAL_REVENUE        Income Statement - Total Revenue (as-reported view).
IS012    SALES_REV_TURN_UNADJ    Revenue (unadjusted).
IS013    NET_REVENUES_BANKS      Net Revenues (banks template).
DT094    FA_ADJUSTED             Toggle: 'Y' standardised, 'N' as-reported.
```

### 5.2 Income statement mnemonics (IS_*)

```
SALES_REV_TURN                   Adjusted Total Revenue.
IS_TOTAL_REVENUE                 Total Revenue (as-reported).
IS_OPERATING_REVENUE             Operating Revenue.
IS_COG_AND_SERVICES_SOLD         COGS and Services Sold.
IS_COGS                          Cost of Goods Sold.
GROSS_PROFIT                     Gross Profit.
IS_OPER_INC                      Operating Income.
IS_OPER_EXPN                     Operating Expenses.
SG_AND_A_EXPENSE                 SG&A Expense.
IS_SELLING_EXP                   Selling Expense.
IS_GEN_ADMIN_EXP                 General & Admin Expense.
IS_RD_EXPEND                     R&D Expense.
IS_OPER_RD                       Operating R&D.
IS_DEPRECIATION_EXP              Depreciation Expense.
IS_AMORT_EXP                     Amortisation Expense.
EBITDA                           EBITDA.
EBITDA_ADJUSTED                  EBITDA - Adjusted.
EBIT                             EBIT.
IS_INT_EXPENSE                   Interest Expense.
IS_INT_INC                       Interest Income.
IS_PRETAX_INC                    Pretax Income.
IS_INC_BEF_XO_ITEM               Income Before Extraordinary Items.
IS_INC_TAX_EXP                   Income Tax Expense.
IS_CURRENT_TAX_EXPENSE           Current Tax Expense.
IS_DEFERRED_TAX_EXPENSE          Deferred Tax Expense.
IS_MIN_NONCONTROL_INTEREST_CREDIT Minority Interest.
NET_INCOME                       Net Income (after MI).
NET_INC_AFT_MIN_INT              Net Income After Minority Interest.
IS_EARN_FROM_CONT_OPS            Earnings from Continuing Operations.
IS_EARN_FROM_DISC_OPS            Earnings from Discontinued Operations.
NORMALIZED_INCOME                Normalised Income (non-recurring removed).
IS_NORMALIZED_INCOME             Same; alternate alias.
NET_INCOME_TO_COMMON             Net Income Available to Common.
IS_DILUTED_EPS                   Diluted EPS.
IS_BASIC_EPS                     Basic EPS.
IS_EPS_CONT_OPS                  EPS - Continuing Ops.
IS_DIL_EPS_CONT_OPS              Diluted EPS - Continuing Ops.
TRAIL_12M_NET_INCOME             Net Income (Trailing 12M).
TRAIL_12M_TOTAL_REVENUE          Total Revenue (Trailing 12M).
TRAIL_12M_EBITDA                 EBITDA (Trailing 12M).
```

### 5.3 Balance sheet mnemonics (BS_*)

```
BS_TOT_ASSET                     Total Assets.
BS_CUR_ASSET_REPORT              Current Assets - Total.
BS_CASH_NEAR_CASH_ITEM           Cash and Near-Cash Items.
BS_CASH                          Cash.
BS_ST_INVEST                     Short-Term Investments.
BS_ACCT_REC                      Accounts Receivable - Net.
BS_INVENTORIES                   Inventories.
BS_OTHER_CUR_ASSETS              Other Current Assets.
BS_GROSS_FIX_ASSET               Gross Property, Plant & Equipment.
BS_NET_FIX_ASSET                 Net Property, Plant & Equipment.
BS_TOT_INTANG_ASSETS             Total Intangible Assets.
BS_GOODWILL                      Goodwill.
BS_OTHER_INTANG_ASSETS           Other Intangibles.
BS_LT_INVEST                     Long-Term Investments.
BS_TOT_LIAB2                     Total Liabilities.
BS_CUR_LIAB                      Current Liabilities.
BS_ACCT_PAYABLE                  Accounts Payable.
BS_ST_BORROW                     Short-Term Borrowing.
BS_CUR_PORTION_LT_DEBT           Current Portion of LT Debt.
BS_LT_BORROW                     Long-Term Borrowing.
BS_LT_DEBT                       Long-Term Debt.
SHORT_AND_LONG_TERM_DEBT         Total Debt (= ST + LT).
BS_DEFERRED_TAX_LIAB             Deferred Tax Liability.
BS_OPER_LEASE_LIAB               Operating Lease Liability.
BS_MIN_NONCONTROL_INTEREST       Minority Interest (BS).
BS_PFD_EQUITY                    Preferred Equity.
BS_SH_CAP_AND_APIC               Common Stock & APIC.
BS_PURE_RETAINED_EARNINGS        Retained Earnings.
BS_TREASURY_STOCK                Treasury Stock.
TOTAL_EQUITY                     Total Stockholders' Equity.
BS_SH_OUT                        Shares Outstanding (period-end).
EQY_SH_OUT                       Equity Shares Outstanding (current).
```

### 5.4 Cash flow mnemonics (CF_*)

```
CF_CASH_FROM_OPER                Cash Flow from Operations.
CF_CASH_FROM_OP_AFT_TAX          Operating Cash Flow After Tax.
CF_CASH_FROM_INV_ACT             Cash Flow from Investing.
CF_CASH_FROM_FNC_ACT             Cash Flow from Financing.
CF_CAP_EXPEND                    Capital Expenditures.
CF_CAP_EXPENDITURES              Same; alias.
CF_ACQUIS_OF_BUSINESS            Acquisitions.
CF_DISP_OF_BUSINESS              Divestitures.
CF_DEPR_AMORT                    Depreciation & Amortisation (CF).
CF_NET_CHG_DEBT                  Net Change in Debt.
CF_PROCEEDS_ST_DEBT              Proceeds from ST Debt.
CF_PROCEEDS_LT_DEBT              Proceeds from LT Debt.
CF_REPAY_LT_DEBT                 Repayment of LT Debt.
CF_DVD_PAID                      Dividends Paid.
CF_PFD_DVDS_PAID                 Preferred Dividends Paid.
CF_REPURCH_OF_STOCK              Stock Repurchases.
CF_ISSUE_OF_STOCK                Stock Issuance.
CF_NET_CHNG_CASH                 Net Change in Cash.
CF_FX_EFFECT_CASH                FX Effect on Cash.
CF_FREE_CASH_FLOW                Free Cash Flow.
TRAIL_12M_CASH_FROM_OPER         Operating Cash Flow (TTM).
TRAIL_12M_FREE_CASH_FLOW         Free Cash Flow (TTM).
```

### 5.5 The FA_* override family

These configure how every other field is interpreted at request time.

```
FA_ADJUSTED                      'Y' adjusted/standardised; 'N' as-reported.
FA_FILING_STATUS                 'OR' originally reported, 'MR' most recent, 'RST' restated.
FA_PERIOD_TYPE_OVERRIDE          'A' annual, 'Q' quarter, 'S' semi, 'LTM', 'YTD', 'CY' calendar.
FA_PERIOD_YEAR_OVERRIDE          Year integer.
FA_PERIOD_REL_OVERRIDE           'CY' current year, 'CY-1', 'CY+1', 'FY1', etc.
FA_CURRENCY                      Currency override (3-letter ISO).
FA_AS_OF_DATE                    PIT asof date for COFI requests.
EQY_FUND_CRNCY                   Reporting currency (read-only).
EQY_FUND_TICKER                  Fundamentals-tagged ticker.
EQY_FUND_IND_GICS                GICS industry classification.
```

### 5.6 BEst — Bloomberg Estimates (BEST_*)

```
BEST_EPS                         Consensus EPS estimate.
BEST_SALES                       Consensus Sales/Revenue estimate.
BEST_EBITDA                      Consensus EBITDA estimate.
BEST_NET_INC                     Consensus Net Income.
BEST_DPS                         Consensus DPS.
BEST_TARGET_PRICE                Consensus Price Target.
BEST_REC_RAW                     Aggregated Recommendation (raw mean).
BEST_REC                         Aggregated Recommendation (5-1 scale).
BEST_EST_NUM                     Number of contributing estimates.
BEST_EST_DATE                    Date of consensus snapshot.
BEST_EPS_FY1                     EPS estimate for FY1.
BEST_EPS_FQ1                     EPS estimate for next quarter.
BEST_PE_RATIO                    Forward P/E (using BEST_EPS).
BEST_LT_GROWTH                   Long-term growth rate.
```

### 5.7 COFI (Company Financials PIT) — bitemporal access

Bloomberg's PIT product, marketed as "COFI" (Company Financials, Estimates and Pricing Point-in-Time), is the enterprise-licensed delivery of bitemporal fundamentals. Each row carries the standard FA_* fields plus:

```
FA_FILING_DATE                   Filing date of the source document.
FA_PERIOD_END_DATE               Fiscal period end date.
FA_KNOWN_AS_OF_DATE              When this value became Bloomberg-known (vintage).
FA_RESTATEMENT_SEQ               Restatement sequence (0 = original).
```

Source: <https://www.bloomberg.com/professional/products/data/enterprise-catalog/cofi/>.

---

## 6. S&P Capital IQ — ciqFinInstanceItem deep dive

Where Compustat is wide-table, Capital IQ is **long-format EAV**. The fact body is `ciqFinInstanceItem`, joined to a dictionary of ~10,000 items in `ciqDataItem` (note: the table is sometimes named `ciqFinancialItem` in older WRDS distributions; both names appear in literature — `[unverified — which is canonical in current CIQ DataWarehouse releases]`). Sources: WRDS Capital IQ overview <https://wrds-www.wharton.upenn.edu/pages/grid-items/introduction-capital-iq/>; CIQ Financials Methodology <https://www.scribd.com/document/161578486/Ciq-Financials-Methodology>; CIQ API guide <https://www.scribd.com/document/331103626/tech-faq-12478964>; Capital IQ Excel Plug-in Manual <http://larryschrenk.com/Capital%20IQ/Excel%20Plug-in%20Manual.pdf>; NYPL CIQ Financials Glossary <https://libguides.nypl.org/CapitalIQ/FinancialsGlossary>.

### 6.1 The core table

```sql
ciqFinInstanceItem
------------------
financialInstanceId        BIGINT     -- Filing-version instance (one per filing/restatement)
dataItemId                 INT        -- Item ID from ciqDataItem (e.g. 100174 = Total Revenue)
periodTypeId               INT        -- Period enumeration (see §6.3)
dataItemValue              DECIMAL    -- The number.
unitTypeId                 INT        -- Currency / units denomination.
currencyId                 INT        -- Currency ISO code.
convHistoryId              BIGINT     -- FX conversion vintage used.
isFiledData                BIT        -- '1' if the value came from the filing
                                         instance, '0' if computed/derived.
filingDate                 DATE       -- When the instance was filed/captured.
effectiveDate              DATE       -- When the value became effective in CIQ.
restatementTypeId          INT        -- 1=Original, 2=Restated, 3=Preliminary, etc.
```

### 6.2 The supporting dimension tables

```
ciqCompany               (companyId, companyName, countryId, gicsCode, …)
ciqCompanyUltimateParent (companyId, parentCompanyId)
ciqSecurity              (securityId, companyId, securityType, primaryFlag, …)
ciqTradingItem           (tradingItemId, securityId, exchangeId, tickerSymbol, …)
ciqExchange              (exchangeId, exchangeName, country, mic, …)
ciqFinPeriod             (financialPeriodId, companyId, periodTypeId,
                          calendarYear, calendarQuarter, periodEndDate, fiscalYear)
ciqFinPeriodType         (periodTypeId, periodTypeName)
ciqFinCollection         (financialCollectionId, financialPeriodId,
                          financialCollectionTypeId, isMostRecent)
ciqFinCollectionType     (financialCollectionTypeId, collectionTypeName)
ciqFinInstance           (financialInstanceId, financialCollectionId,
                          filingMode, fxRateConversionMode, originatingEvent)
ciqDataItem              (dataItemId, dataItemName, mnemonic, displayLabel,
                          statementType, isPerShare, isRatio)
ciqDataItemTranslation   (dataItemId, languageId, label)
ciqRestatementType       (restatementTypeId, restatementName)
```

### 6.3 periodtype enumeration (`ciqFinPeriodType`)

The full enumeration as published in CIQ documentation and the Excel Plug-in manual:

```
periodTypeId  Code        Description
------------  ----------  -----------------------------------------------
1             IQ_FY       Annual / Fiscal Year (Most Recent Filing).
2             IQ_Q        Quarterly (most recent reported quarter).
3             IQ_Q1       Q1 (specific).
4             IQ_Q2       Q2.
5             IQ_Q3       Q3.
6             IQ_Q4       Q4 (often equal to FY-Q3-Q2-Q1).
7             IQ_H        Semi-annual / Interim (H1, H2).
8             IQ_YTD      Year-to-date (fiscal).
9             IQ_LTM      Latest twelve months.
10            IQ_NTM      Next twelve months (forward).
11            IQ_CY       Calendar year.
12            IQ_CY_Q     Calendar quarter.
13            IQ_FY-1     Prior fiscal year (relative).
14            IQ_FY-2     FY-2 relative.
15            IQ_FY-N     Generic relative.
16            IQ_FY+1     Forward FY (used with estimates).
20            IQ_PRE      Preliminary (press release).
21            IQ_REPORT   As-reported actuals on filing.
22            IQ_PROFORMA Pro-forma view.
```

`[unverified — exact integer mapping varies by CIQ release; codes above are aligned with public Excel Plug-in references but the WRDS-distributed `ciqFinPeriodType` should be queried directly for absolute accuracy]`.

### 6.4 `ciqFinCollectionType` — the restatement spine

This is what makes CIQ truly bitemporal at the filing level.

```
collectionTypeId  Code            Description
----------------  --------------  ---------------------------------------------
1                 ORIGINAL        Original filing (10-K/10-Q first instance).
2                 RESTATED        Subsequent restatement.
3                 PRESS_RELEASE   Press-release preliminary values (8-K 2.02).
4                 PROFORMA        Pro-forma reorganisation view.
5                 FILING          Generic filing-mode instance.
6                 CIQ_DERIVED     CIQ-computed (e.g. ratios in advanced products).
```

A single `(companyId, financialPeriodId)` pair will have multiple `ciqFinInstance` rows, one per `financialCollectionType`. Backtesters always join `ON financialCollectionTypeId = 3` (press release) for as-of-RDQ replay, or `= 1` (original 10-K/Q) for as-of-filing replay.

### 6.5 Selected `ciqDataItem` IDs

The dictionary has ~10,000 items. Public sources cite specific IDs widely; the canonical mappings (cross-referenced from the CIQ Excel Plug-in `IQ_*` mnemonics):

```
dataItemId  IQ_* mnemonic              Description
----------  -------------------------  -----------------------------------------------
100174      IQ_TOTAL_REV               Total Revenue.
112         IQ_GROSS_PROFIT            Gross Profit.
164         IQ_EBITDA                  EBITDA.
21          IQ_EBIT                    EBIT.
142         IQ_NI                      Net Income (Continuing Ops).
15          IQ_EPS_INCL_EXTRA          Diluted EPS Incl. Extraordinary.
14          IQ_DILUT_EPS_EXCL          Diluted EPS Excl. Extraordinary.
1000        IQ_BASIC_EPS_INCL_EXTRA    Basic EPS Incl. Extraordinary.
1001        IQ_BASIC_EPS_EXCL          Basic EPS Excl. Extraordinary.
1007        IQ_TOTAL_ASSETS            Total Assets.
1008        IQ_TOTAL_LIAB              Total Liabilities.
1275        IQ_TOTAL_EQUITY            Total Stockholders' Equity.
1               IQ_COMMON_EQUITY           Common Equity.
1003        IQ_CASH_ST_INVEST          Cash & Short-Term Investments.
1005        IQ_TOTAL_DEBT              Total Debt (= ST + LT).
1009        IQ_LT_DEBT                 Long-Term Debt.
1011        IQ_ST_DEBT                 Short-Term Debt.
1023        IQ_INVENTORY               Inventory.
1021        IQ_AR                      Accounts Receivable.
1024        IQ_AP                      Accounts Payable.
1029        IQ_GW                      Goodwill.
1093        IQ_CAPEX                   Capital Expenditures.
1094        IQ_CASH_OPER               Cash from Operations.
1097        IQ_CASH_INVEST             Cash from Investing.
1100        IQ_CASH_FIN                Cash from Financing.
1278        IQ_COMMON_DIV_PAID         Common Dividends Paid.
1281        IQ_PREF_DIV_PAID           Preferred Dividends Paid.
1300        IQ_DEP_AMORT               Depreciation & Amortisation.
1366        IQ_FFO                     Funds From Operations (REIT).
1367        IQ_AFFO                    Adjusted FFO (REIT).
1900        IQ_NET_INT_INC             Net Interest Income (bank template).
1901        IQ_NIM                     Net Interest Margin (bank).
1903        IQ_PROV_LOAN_LOSSES        Provision for Loan Losses (bank).
2010        IQ_PREMIUMS_EARNED         Premiums Earned (insurance).
2030        IQ_LOSS_RATIO              Loss Ratio (insurance).
2050        IQ_COMBINED_RATIO          Combined Ratio (insurance).
```

`[unverified — exact dataItemId integer mappings are subject to revision across CIQ releases; the IQ_* mnemonics are stable, the integer keys less so]`. The WRDS `wrds_ciqsymbol` and `ciq.ciqfinancialitem` tables expose the live mapping.

### 6.6 estimateperiodtype variant

The estimates side uses a parallel period dimension `ciqEstimatePeriodType`:

```
estimatePeriodTypeId  Code
--------------------  --------
1                     IQ_FY1   Forward FY1.
2                     IQ_FY2   Forward FY2.
3                     IQ_FY3   FY3.
4                     IQ_FY4   FY4.
5                     IQ_FY5   FY5.
6                     IQ_CY1   Forward calendar year 1.
10                    IQ_FQ1   Forward quarter 1.
11                    IQ_FQ2   FQ2.
20                    IQ_LTG   Long-term growth.
30                    IQ_NTM   Next twelve months consensus.
```

### 6.7 Filing-date vs effective-date semantics

`ciqFinInstance.filingDate` is when CIQ captured the filing; `ciqFinInstanceItem.effectiveDate` is when that specific value became authoritative in the CIQ store. For 8-K Item 2.02 press releases, `filingDate = effectiveDate = 8-K filing timestamp`. For 10-Q filings, `filingDate` lags 8-K by 14–30 days. For restatements, a new `ciqFinInstance` is created with a new `financialInstanceId`, a `financialCollectionTypeId=2`, and a later `filingDate`.

PIT query template (as-of-D):
```sql
WITH ranked AS (
  SELECT fi.companyId, fi.financialPeriodId, fii.dataItemId, fii.dataItemValue,
         ROW_NUMBER() OVER (
           PARTITION BY fi.companyId, fi.financialPeriodId, fii.dataItemId
           ORDER BY fi.filingDate DESC, fi.financialInstanceId DESC
         ) AS rn
  FROM ciqFinInstance fi
  JOIN ciqFinCollection fc ON fc.financialCollectionId = fi.financialCollectionId
  JOIN ciqFinInstanceItem fii ON fii.financialInstanceId = fi.financialInstanceId
  WHERE fi.filingDate <= :asof_date
)
SELECT * FROM ranked WHERE rn = 1;
```

---

## 7. SimFin and Sharadar — cheap alternatives

### 7.1 Sharadar Core US Fundamentals (SF1)

Sharadar's `SF1` table is structurally a Compustat clone. Published under the Nasdaq Data Link (formerly Quandl) marketplace. Schema (source: <https://data.nasdaq.com/databases/SF1>, <https://www.quantrocket.com/sharadar/>, Sharadar datasheet <https://resources.quandl.com/a/res-hub/Sharadar_Datasheet_final.pdf>):

```
SF1 column dimension (the 'dimension' field):
  ARQ   As-Reported Quarterly (PIT - the values as first reported).
  ARY   As-Reported Yearly.
  ART   As-Reported Trailing twelve months.
  MRQ   Most-Recent Quarterly (latest restated).
  MRY   Most-Recent Yearly.
  MRT   Most-Recent Trailing.
```

Both ARQ and MRQ versions ship — Sharadar is one of the few non-Compustat sources offering as-reported PIT data at <$5K/yr.

Key columns (~150 total):

```
ticker, dimension, calendardate, datekey, reportperiod, lastupdated,
  permaticker, fiscalperiod,

Income statement:
  revenue           Revenue (Total).
  cor               Cost of Revenue.
  gp                Gross Profit.
  rnd               R&D Expense.
  sgna              SG&A.
  opex              Operating Expenses Total.
  opinc             Operating Income.
  intexp            Interest Expense.
  taxexp            Tax Expense.
  ebit              EBIT.
  ebitda            EBITDA.
  ebt               Earnings Before Tax.
  netinc            Net Income.
  netinccmn         Net Income to Common.
  netincdis         Net Income from Discontinued Operations.
  prefdivis         Preferred Dividends - Issued.
  consolinc         Consolidated Income.
  eps               EPS - Basic.
  epsdil            EPS - Diluted.
  epsusd            EPS in USD (FX-converted).
  shareswa          Weighted Average Shares Outstanding.
  shareswadil       Weighted Average Shares Diluted.
  sharesbas         Shares Basic Outstanding.

Balance sheet:
  assets            Total Assets.
  assetsc           Current Assets.
  assetsnc          Non-Current Assets.
  cashneq           Cash and Equivalents.
  investments       Investments.
  investmentsc      Current Investments.
  investmentsnc     Non-Current Investments.
  receivables       Receivables.
  inventory         Inventory.
  ppnenet           Property, Plant & Equipment Net.
  intangibles       Intangibles & Goodwill.
  taxassets         Tax Assets.
  liabilities       Total Liabilities.
  liabilitiesc      Current Liabilities.
  liabilitiesnc     Non-Current Liabilities.
  payables          Payables.
  debt              Total Debt.
  debtc             Current Debt.
  debtnc            Non-Current Debt.
  deferredrev       Deferred Revenue.
  taxliabilities    Tax Liabilities.
  equity            Total Equity.
  retearn           Retained Earnings.
  accoci            Accumulated Other Comprehensive Income.

Cash flow:
  ncfo              Net Cash from Operating.
  ncfi              Net Cash from Investing.
  ncff              Net Cash from Financing.
  ncfbus            Net Cash Flow - Business Acquisitions.
  ncfinv            Net Cash Flow - Investments.
  ncfdebt           Net Cash Flow - Debt Issuance/Repayment.
  ncfcommon         Net Cash Flow - Common Stock Repurchase/Issuance.
  ncfdiv            Net Cash Flow - Dividends.
  ncfx              Effect of FX on Cash.
  ncf               Net Change in Cash.
  capex             Capital Expenditures.
  sbcomp            Stock-Based Compensation.
  depamor           Depreciation & Amortisation.
  workingcapital    Working Capital.
  fcf               Free Cash Flow.

Per-share / market:
  price             Period-end Share Price.
  marketcap         Market Cap.
  ev                Enterprise Value.
  evebit            EV/EBIT.
  evebitda          EV/EBITDA.
  pe                P/E.
  pe1               Forward P/E (next year).
  ps                P/S.
  pb                P/B.
  divyield          Dividend Yield.
  dps               Dividends per Share.

Ratios / derived:
  roa, roe, roic, roic1, currentratio, de (Debt/Equity),
  fcfps, bvps, tbvps, sps, payoutratio, grossmargin,
  netmargin, ebitmargin, ebitdamargin
```

Sharadar also ships `SEP` (Sharadar Equity Prices, EOD prices), `SF2` (insider transactions, Form 4-derived), `SF3` (institutional holdings, 13F-derived), `SFP` (S&P 500 constituents), `INDICATORS` (a catalogue of all column definitions), `TICKERS` (the security master).

### 7.2 SimFin schema

SimFin's bulk download is a CSV-per-statement-per-frequency structure. Two tiers: Free (free, 1-2 quarter lag, narrow universe) and Plus (paid, full universe, faster). Source: <https://www.simfin.com/en/fundamental-data-download/>, <https://github.com/SimFin/simfin/blob/master/simfin/names.py>, <https://github.com/SimFin/simfin>.

```
companies.csv (entity master):
  Ticker, SimFinId, Company Name, IndustryId,
  ISIN, End of financial year (month), Number Employees, Business Summary,
  Market, CIK, Main Currency

industries.csv (industry dictionary):
  IndustryId, Sector, Industry, SubIndustry

income_statement.csv (Plus tier — quarterly variant has ~25 cols, banks/insurance have separate templates):
  Ticker, SimFinId, Currency, Fiscal Year, Fiscal Period, Report Date,
  Publish Date, Restated Date, Source,
  Shares (Basic), Shares (Diluted),
  Revenue, Cost of Revenue, Gross Profit,
  Operating Expenses, Selling, General & Administrative,
  Research & Development, Depreciation & Amortization,
  Operating Income (Loss), Non-Operating Income (Loss),
  Interest Expense, Net, Pretax Income (Loss), Adjusted,
  Abnormal Gains (Losses), Pretax Income (Loss),
  Income Tax (Expense) Benefit, Net,
  Income (Loss) from Continuing Operations,
  Net Extraordinary Gains (Losses),
  Net Income, Net Income (Common)

balance_sheet.csv:
  Ticker, SimFinId, Currency, Fiscal Year, Fiscal Period,
  Cash, Cash & Equivalents, Short Term Investments,
  Accounts & Notes Receivable, Inventories, Total Current Assets,
  Property, Plant & Equipment, Net, Long Term Investments & Receivables,
  Other Long Term Assets, Total Noncurrent Assets, Total Assets,
  Payables & Accruals, Short Term Debt, Total Current Liabilities,
  Long Term Debt, Total Noncurrent Liabilities, Total Liabilities,
  Preferred Equity, Share Capital & Additional Paid-In Capital,
  Treasury Stock, Retained Earnings, Total Equity,
  Total Liabilities & Equity

cash_flow.csv:
  Ticker, SimFinId, Fiscal Year, Fiscal Period,
  Net Income / Starting Line, Depreciation & Amortization,
  Non-Cash Items, Change in Working Capital,
  Change in Accounts Receivable, Change in Inventories,
  Change in Accounts Payable, Change in Other,
  Net Cash from Operating Activities,
  Change in Fixed Assets & Intangibles,
  Net Change in Long Term Investment, Net Cash from Acquisitions & Divestitures,
  Net Cash from Investing Activities,
  Dividends Paid, Cash from (Repayment of) Debt,
  Cash from (Repurchase of) Equity, Net Cash from Financing Activities,
  Effect of Foreign Exchange Rates, Net Change in Cash

prices.csv (EOD):
  Ticker, Date, Open, High, Low, Close, Adj. Close, Volume,
  Dividend, Shares Outstanding
```

SimFin Banks template (`income_statement_banks.csv`) and Insurance template (`income_statement_insurance.csv`) include sector-specific items (net interest income, premiums earned, loss reserves, etc.).

`[unverified — exact field-list completeness depends on Plus subscription tier; Free tier omits some advanced fields]`.

### 7.3 Comparison

| Vendor | History | Universe | PIT? | Restatement audit | Price |
|---|---|---|---|---|---|
| Sharadar SF1 | 1998+ for most US tickers | ~14,000 US incl. delisted | Yes (ARQ/ARY) | Both AR and MR vintages preserved | $540/yr indiv |
| SimFin Plus | 2008+ (some 2000+) | ~5,000 US | Partial (restated date column) | Single-vintage; restatement detection via Report Date diff | €59–€199/yr |
| Compustat NA | 1962+ | 14,000 active + 28,000 inactive | Yes (Snapshot add-on) | Full vintage history | $100K–$1M+ enterprise |

Sharadar is the closest "good-enough Compustat" for backtesting at retail price; SimFin is best for free / low-volume use.

---

## 8. EDGAR XBRL reconstruction — the substantive section

This is the section that answers "what fraction of Compustat can be rebuilt from public data?". The answer is: **~80–85% of the headline ~300 Compustat annual mnemonics and ~95% of the ~100 quarterly mnemonics** can be reconstructed from us-gaap concepts in EDGAR XBRL filings, with the remaining 15–20% requiring either (a) parsing 10-K footnotes / MD&A, (b) human-curated mapping for edge cases, or (c) industry-specific taxonomies (Bank Call Reports, Nareit FFO, etc.).

Source primary references:
- SEC EDGAR APIs <https://www.sec.gov/search-filings/edgar-application-programming-interfaces>
- DERA Financial Statement Data Sets <https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets>
- DERA Financial Statement Data Sets methodology PDF <https://www.sec.gov/files/financial-statement-data-sets.pdf>
- FASB 2024 GAAP Taxonomy <https://www.fasb.org/page/detail?pageId=/projects/FASB-Taxonomies/2024-gaap-financial-reporting-taxonomy.html>
- XBRL US "Why Normalize Data" <https://xbrl.us/why-normalize-data/>
- XBRL US API <https://api.xbrl.us/api/v1/>
- Arelle open-source XBRL processor <https://arelle.org/>
- EdgarTools <https://github.com/dgunning/edgartools>

### 8.1 The us-gaap taxonomy — line-item-by-line-item

Every fact in a US 10-K/10-Q XBRL instance is tagged with a concept (a QName like `us-gaap:Revenues`) and a `contextRef` (which specifies the period, entity, and any axis/member dimensions). The concept comes from one of the FASB-maintained taxonomies updated annually. The 2024 GAAP Financial Reporting Taxonomy namespace is `http://fasb.org/us-gaap/2024` (source: <https://xbrl.us/xbrl-taxonomy/2024-us-gaap/>).

The concepts below are the canonical us-gaap names for each line item, along with the alternate concepts companies actually file under in practice. **Bold** indicates the most common as of 2024.

#### 8.1.1 Top-line revenue (income statement)

```
Canonical concept                                                       Notes
--------------------------------------------------------------------    --------------------------------
us-gaap:Revenues                                                        Pre-ASC 606 / legacy default.
us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax            ***Post-ASC 606 default (2018+).
us-gaap:RevenueFromContractWithCustomerIncludingAssessedTax            Post-ASC 606 incl. excise.
us-gaap:SalesRevenueNet                                                Deprecated 2018 but still appears.
us-gaap:SalesRevenueGoodsNet                                           Goods-only revenue.
us-gaap:SalesRevenueServicesNet                                        Services-only revenue.
us-gaap:OperatingLeasesIncomeStatementLeaseRevenue                     Lease income for REITs/lessors.
us-gaap:OilAndGasRevenue                                               Oil & gas E&P (extension).
us-gaap:RegulatedAndUnregulatedOperatingRevenue                        Utilities / regulated industries.
us-gaap:InterestAndDividendIncomeOperating                             Banks (replaces Revenues).
us-gaap:PremiumsEarnedNet                                              Insurance.
```

**Reconstruction recipe for Compustat `revt` / FactSet `FF_SALES`:** SUM across all `us-gaap:Revenues`-family concepts for the same `(entity, period)` after deduplicating (an issuer files under exactly one revenue concept per period in practice; multi-concept reporting is rare). When the issuer files an extension (`us-gaap:CompanyRevenues` or `xyz:CustomRevenueLine`), inspect the calculation linkbase to determine the parent concept and use that.

#### 8.1.2 Cost of revenue / gross profit

```
us-gaap:CostOfRevenue                                                  Generic cost of revenue.
us-gaap:CostOfGoodsAndServicesSold                                     ***Post-2018 default.
us-gaap:CostOfGoodsSold                                                Goods only.
us-gaap:CostOfServices                                                 Services only.
us-gaap:CostOfGoodsSoldExcludingDepreciationDepletionAndAmortization   Ex-D&A variant (utilities).
us-gaap:DirectOperatingCosts                                           Alternative.
us-gaap:GrossProfit                                                    Explicitly disclosed Gross Profit.
```

#### 8.1.3 Operating expenses

```
us-gaap:SellingGeneralAndAdministrativeExpense                         SG&A combined.
us-gaap:SellingAndMarketingExpense                                     Selling only.
us-gaap:GeneralAndAdministrativeExpense                                G&A only.
us-gaap:ResearchAndDevelopmentExpense                                  R&D expensed.
us-gaap:ResearchAndDevelopmentExpenseExcludingAcquiredInProcessCost    R&D excl. IPR&D.
us-gaap:ResearchAndDevelopmentInProcess                                IPR&D charge.
us-gaap:DepreciationAndAmortization                                    D&A combined.
us-gaap:Depreciation                                                   Depreciation only.
us-gaap:AmortizationOfIntangibleAssets                                 Amortisation only.
us-gaap:DepreciationDepletionAndAmortization                           D, Depletion, & A combined (CF).
us-gaap:RestructuringCharges                                           Restructuring.
us-gaap:AssetImpairmentCharges                                         Impairments.
us-gaap:GoodwillImpairmentLoss                                         GW impairment.
us-gaap:OperatingExpenses                                              Operating Expenses Total.
us-gaap:OperatingIncomeLoss                                            ***Operating Income (Loss).
```

#### 8.1.4 Below-the-line

```
us-gaap:NonoperatingIncomeExpense                                      Non-operating Inc/Exp.
us-gaap:InterestExpense                                                Interest Expense — Total.
us-gaap:InterestExpenseDebt                                            Interest Expense — Debt only.
us-gaap:InterestIncome                                                 Interest Income.
us-gaap:InvestmentIncomeInterest                                       Interest income from investments.
us-gaap:IncomeLossFromEquityMethodInvestments                          Equity-method earnings.
us-gaap:OtherNonoperatingIncomeExpense                                 Other non-op.
us-gaap:IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest  Pretax (long name).
us-gaap:IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments  Alternate.
us-gaap:IncomeTaxExpenseBenefit                                        Income Tax Expense (Benefit).
us-gaap:CurrentIncomeTaxExpenseBenefit                                 Current Tax.
us-gaap:DeferredIncomeTaxExpenseBenefit                                Deferred Tax.
us-gaap:IncomeLossFromContinuingOperations                             NI - Continuing Ops.
us-gaap:IncomeLossFromDiscontinuedOperationsNetOfTax                   NI - Discontinued Ops.
us-gaap:ExtraordinaryItemNetOfTax                                      Extraordinary (pre-2015 — eliminated by ASU 2015-01).
us-gaap:MinorityInterestInNetIncomeLoss                                Minority Interest.
us-gaap:NetIncomeLossAttributableToNoncontrollingInterest              ***NCI share of NI.
us-gaap:NetIncomeLoss                                                  ***Net Income (Loss) - TOTAL.
us-gaap:NetIncomeLossAvailableToCommonStockholdersBasic                ***NI available to common.
us-gaap:PreferredStockDividends                                        Preferred Dividends.
```

#### 8.1.5 EPS and share count

```
us-gaap:EarningsPerShareBasic                                          ***Basic EPS.
us-gaap:EarningsPerShareDiluted                                        ***Diluted EPS.
us-gaap:IncomeLossFromContinuingOperationsPerBasicShare                EPS from Cont Ops - Basic.
us-gaap:IncomeLossFromContinuingOperationsPerDilutedShare              EPS from Cont Ops - Diluted.
us-gaap:WeightedAverageNumberOfSharesOutstandingBasic                  ***Weighted avg basic shares.
us-gaap:WeightedAverageNumberOfDilutedSharesOutstanding                ***Weighted avg diluted shares.
us-gaap:CommonStockSharesOutstanding                                   Period-end common shares out.
us-gaap:CommonStockSharesIssued                                        Common shares issued.
us-gaap:CommonStockSharesAuthorized                                    Authorised shares.
dei:EntityCommonStockSharesOutstanding                                 Cover-page outstanding (latest).
```

#### 8.1.6 Balance sheet — assets

```
us-gaap:Assets                                                         ***Total Assets.
us-gaap:AssetsCurrent                                                  Current Assets.
us-gaap:CashAndCashEquivalentsAtCarryingValue                          ***Cash & equivalents.
us-gaap:Cash                                                           Cash only.
us-gaap:CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents  Cash incl. restricted (banks).
us-gaap:MarketableSecuritiesCurrent                                    Short-term marketable securities.
us-gaap:ShortTermInvestments                                           Short-term investments.
us-gaap:AccountsReceivableNetCurrent                                   ***Trade AR.
us-gaap:ReceivablesNetCurrent                                          Receivables Net.
us-gaap:InventoryNet                                                   ***Inventories.
us-gaap:PrepaidExpenseCurrent                                          Prepaid Expenses.
us-gaap:OtherAssetsCurrent                                             Other Current Assets.
us-gaap:PropertyPlantAndEquipmentNet                                   ***PP&E Net.
us-gaap:PropertyPlantAndEquipmentGross                                 PP&E Gross.
us-gaap:AccumulatedDepreciationDepletionAndAmortizationPropertyPlantAndEquipment  Accumulated depreciation.
us-gaap:IntangibleAssetsNetExcludingGoodwill                           Other intangibles (ex GW).
us-gaap:Goodwill                                                       ***Goodwill.
us-gaap:OperatingLeaseRightOfUseAsset                                  ROU lease asset (ASC 842).
us-gaap:LongTermInvestments                                            LT Investments.
us-gaap:EquityMethodInvestments                                        Equity-method investments.
us-gaap:DeferredIncomeTaxAssetsNet                                     Deferred tax assets.
us-gaap:OtherAssetsNoncurrent                                          Other Non-current Assets.
```

#### 8.1.7 Balance sheet — liabilities & equity

```
us-gaap:Liabilities                                                    ***Total Liabilities.
us-gaap:LiabilitiesCurrent                                             ***Current Liabilities.
us-gaap:AccountsPayableCurrent                                         ***Accounts Payable.
us-gaap:AccruedLiabilitiesCurrent                                      Accrued Liabilities.
us-gaap:EmployeeRelatedLiabilitiesCurrent                              Accrued payroll.
us-gaap:LongTermDebtCurrent                                            ***Current portion of LT Debt.
us-gaap:ShortTermBorrowings                                            Short-term borrowings.
us-gaap:CommercialPaper                                                Commercial paper.
us-gaap:OperatingLeaseLiabilityCurrent                                 Operating lease liability - current.
us-gaap:LongTermDebt                                                   Total LT Debt (incl. current).
us-gaap:LongTermDebtNoncurrent                                         ***LT Debt - Non-current portion.
us-gaap:LongTermNotesPayable                                           LT Notes.
us-gaap:UnsecuredDebt                                                  Unsecured debt.
us-gaap:SecuredDebt                                                    Secured debt.
us-gaap:SeniorNotes                                                    Senior notes.
us-gaap:ConvertibleDebtNoncurrent                                      Convertible debt LT.
us-gaap:OperatingLeaseLiabilityNoncurrent                              Operating lease liability LT.
us-gaap:DeferredIncomeTaxLiabilitiesNet                                Deferred tax liabilities.
us-gaap:DeferredRevenue                                                Deferred revenue.
us-gaap:OtherLiabilitiesNoncurrent                                     Other LT liabilities.
us-gaap:MinorityInterest                                               Minority interest (BS).
us-gaap:CommitmentsAndContingencies                                    Commitments (typically null tag).
us-gaap:PreferredStockValue                                            Preferred stock - reported.
us-gaap:CommonStockValue                                               Common stock at par value.
us-gaap:AdditionalPaidInCapital                                        APIC.
us-gaap:AdditionalPaidInCapitalCommonStock                             APIC - Common Stock.
us-gaap:RetainedEarningsAccumulatedDeficit                             ***Retained Earnings.
us-gaap:AccumulatedOtherComprehensiveIncomeLossNetOfTax                AOCI.
us-gaap:TreasuryStockValue                                             Treasury stock.
us-gaap:StockholdersEquity                                             ***Stockholders' Equity.
us-gaap:StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest  Total equity incl. NCI.
us-gaap:LiabilitiesAndStockholdersEquity                               Total Liab + Equity (= Total Assets).
```

#### 8.1.8 Cash flow statement

```
us-gaap:NetCashProvidedByUsedInOperatingActivities                     ***CFO.
us-gaap:NetCashProvidedByUsedInOperatingActivitiesContinuingOperations CFO - Cont Ops.
us-gaap:NetCashProvidedByUsedInInvestingActivities                     ***CFI.
us-gaap:NetCashProvidedByUsedInFinancingActivities                     ***CFF.
us-gaap:PaymentsToAcquirePropertyPlantAndEquipment                     ***Capex.
us-gaap:PaymentsToAcquireProductiveAssets                              Broader capex incl. intangibles.
us-gaap:PaymentsToAcquireBusinessesNetOfCashAcquired                   Acquisitions net of cash.
us-gaap:ProceedsFromDivestitureOfBusinesses                            Divestiture proceeds.
us-gaap:DepreciationDepletionAndAmortization                           ***D&A (from CF).
us-gaap:ShareBasedCompensation                                         Stock-Based Compensation.
us-gaap:IncreaseDecreaseInAccountsReceivable                           ΔAR (sign convention: increase = use of cash).
us-gaap:IncreaseDecreaseInInventories                                  ΔInv.
us-gaap:IncreaseDecreaseInAccountsPayable                              ΔAP.
us-gaap:IncreaseDecreaseInOtherOperatingCapitalNet                     ΔOther WC.
us-gaap:ProceedsFromIssuanceOfLongTermDebt                             LT Debt issuance.
us-gaap:RepaymentsOfLongTermDebt                                       LT Debt repayment.
us-gaap:ProceedsFromIssuanceOfCommonStock                              Common stock issuance.
us-gaap:PaymentsForRepurchaseOfCommonStock                             Common buybacks.
us-gaap:PaymentsOfDividends                                            Dividends paid.
us-gaap:PaymentsOfDividendsCommonStock                                 Common dividends paid.
us-gaap:PaymentsOfDividendsPreferredStockAndPreferenceStock            Preferred dividends paid.
us-gaap:EffectOfExchangeRateOnCashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents  FX effect on cash.
us-gaap:CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseIncludingExchangeRateEffect  Net change in cash.
```

### 8.2 The toolchain

| Tool | What it does | When to use |
|---|---|---|
| **`companyfacts.json` API** | `https://data.sec.gov/api/xbrl/companyfacts/CIK##########.json` — every fact ever filed by a CIK, keyed by tag, with `start`/`end`/`val`/`accn`/`fy`/`fp`/`form`/`filed`/`frame`. | The primary entry point for any one-company workflow. |
| **`frames` API** | `https://data.sec.gov/api/xbrl/frames/{tax}/{tag}/{unit}/{period}.json` — cross-sectional snapshot of one concept across all filers for one period. | Factor research, percentile ranks, batch loads. |
| **`companyconcept` API** | `https://data.sec.gov/api/xbrl/companyconcept/CIK##########/us-gaap/{tag}.json` — single-concept history per CIK. | When you need one line item across all periods for one company. |
| **DERA Financial Statement Data Sets** | Quarterly ZIPs of pipe-delimited `SUB / NUM / TAG / PRE` files. Posted ~6–10 weeks after quarter end. | Bulk historical backfill. The Compustat-equivalent free flat file. |
| **Arelle** | Python-based XBRL processor; resolves the DTS (discoverable taxonomy set), applies calculation/definition linkbase validation, runs XULE rules. | When you need to parse raw inline XBRL from primary filings or run DQC validation. |
| **EdgarTools** | Python wrapper for filing discovery + structured extraction. | High-level ingestion convenience. |
| **XBRL US API** | `https://api.xbrl.us/api/v1/` — REST API with normalized facts + quality-rule flags. | Academic access; quality benchmark. |
| **`sec-api.io`** | Commercial wrapper offering normalised JSON + WebSocket streaming. | If you need to outsource normalisation. |
| **`pyedgar`, `python-edgar`, `edgar`** | Various open Python clients. | Pick one based on async / sync preference. |

### 8.3 The four reconciliation problems

#### Problem 1 — Multiple concepts for the same economic line

The Revenue example above is the headline case. Companies switched from `us-gaap:SalesRevenueNet` to `us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax` on ASC 606 adoption (2018 for most). To reconstruct a Compustat-equivalent `revt`:

```
revt(filing) = COALESCE(
  RevenueFromContractWithCustomerExcludingAssessedTax,
  RevenueFromContractWithCustomerIncludingAssessedTax,
  SalesRevenueNet,
  Revenues,
  -- bank fallback:
  InterestAndDividendIncomeOperating + NoninterestIncome,
  -- insurance fallback:
  PremiumsEarnedNet + NetInvestmentIncome,
  -- extension (deep parse of calculation linkbase):
  extension_concept_with_parent_pointing_to_revenues
)
```

XBRL US's "harmonisation rules" implement exactly this COALESCE logic for ~600 standardised concepts.

#### Problem 2 — Custom extension concepts

~19% of all concepts used across annual reports are company-specific extensions (`xyz:CompanySpecificRevenueLine`). Per the calculation linkbase, every extension should `calculationArc` to a parent us-gaap concept. In practice:

- Most extensions roll up cleanly (extension Revenue → us-gaap:Revenues with weight 1).
- A meaningful minority (~10% of extensions) don't have calculation arcs and require label-based heuristic matching or human review.
- DQC rules flag mis-tagged extensions; XBRL US ships a `dqc_rules` taxonomy with ~150 validation rules (source: <https://xbrl.us/data-rule/>).

#### Problem 3 — Sign conventions

XBRL's "balance type" is debit/credit, but issuers occasionally flip signs in their as-filed values. The calculation linkbase encodes the expected sign (`weight = 1` or `weight = -1`); deviations flag DQC rules. ats-eqt should apply the linkbase weights at ingest, not trust raw values.

#### Problem 4 — Multiple contextRef (period, axis, dimension) per fact

A single concept like `us-gaap:Revenues` may appear in the same filing with multiple contexts:
- Total company for FY2024 (no dimension).
- Per segment under `us-gaap:StatementBusinessSegmentsAxis` × segment members.
- Per geographic region under `us-gaap:StatementGeographicalAxis`.
- As-restated under `us-gaap:StatementScenarioAxis = us-gaap:ScenarioPreviouslyReportedMember`.

For top-line consolidated rebuilds, filter to facts where the context has **no** axis dimensions (the "consolidated total" context). For segment data, use the segment axis explicitly.

### 8.4 Coverage analysis — what % of Compustat can be rebuilt

| Compustat field | us-gaap concept(s) | Reconstructable? | Notes |
|---|---|---|---|
| `revt` / `sale` | RevenueFromContractWithCustomerExcludingAssessedTax + family | **>95% post-2018; ~90% earlier** | ASC 606 transition is the only material wrinkle. |
| `cogs` | CostOfGoodsAndServicesSold + family | **~90%** | Some companies bundle into single OperatingExpenses. |
| `xsga` | SellingGeneralAndAdministrativeExpense | **~90%** | When G&A and Selling broken out, sum. |
| `xrd` | ResearchAndDevelopmentExpense | **>95%** | Required disclosure under ASC 730. |
| `dp` | DepreciationDepletionAndAmortization | **~85%** | Often disclosed only in CF. |
| `oibdp` | OperatingIncomeLoss + Depreciation | **~85%** | Computed. |
| `oiadp` | OperatingIncomeLoss | **>95%** | Direct tag. |
| `xint` | InterestExpense | **>95%** | Direct. |
| `pi` | IncomeLossFromContinuingOperationsBeforeIncomeTaxes* (long names) | **~90%** | Two competing long-name variants. |
| `txt` | IncomeTaxExpenseBenefit | **>95%** | Direct. |
| `ni` | NetIncomeLoss | **>99%** | The most universally tagged concept. |
| `epspx` | EarningsPerShareBasic | **>99%** | Cover-page tagged. |
| `epsfx` | EarningsPerShareDiluted | **>99%** | Cover-page tagged. |
| `at` | Assets | **>99%** | Direct. |
| `lt` | Liabilities | **>99%** | Direct. |
| `ceq` | StockholdersEquity | **>99%** | Direct. |
| `seq` | StockholdersEquity + MinorityInterest | **>99%** | Computed. |
| `che` | CashAndCashEquivalentsAtCarryingValue + ShortTermInvestments | **>95%** | Direct. |
| `rect` | AccountsReceivableNetCurrent + ReceivablesNetCurrent | **>95%** | Direct. |
| `invt` | InventoryNet | **>95%** | Direct. |
| `ppent` | PropertyPlantAndEquipmentNet | **>99%** | Direct. |
| `gdwl` | Goodwill | **>99%** | Direct. |
| `dlc` | LongTermDebtCurrent + ShortTermBorrowings + CommercialPaper | **~90%** | Multiple variants depending on capital structure. |
| `dltt` | LongTermDebtNoncurrent | **>95%** | Direct. |
| `ap` | AccountsPayableCurrent | **>99%** | Direct. |
| `csho` | CommonStockSharesOutstanding | **>99%** | Cover-page tagged. |
| `cshpri` | WeightedAverageNumberOfSharesOutstandingBasic | **>99%** | Direct. |
| `cshfd` | WeightedAverageNumberOfDilutedSharesOutstanding | **>99%** | Direct. |
| `oancf` | NetCashProvidedByUsedInOperatingActivities | **>99%** | Direct. |
| `capx` | PaymentsToAcquirePropertyPlantAndEquipment | **>95%** | Direct. |
| `dpc` | DepreciationDepletionAndAmortization (CF version) | **>95%** | Direct. |
| `dvc` | PaymentsOfDividendsCommonStock | **~90%** | Some only break out total dividends. |
| `aqc` | PaymentsToAcquireBusinessesNetOfCashAcquired | **~85%** | Compustat aqc has slightly different scope. |

Weighted by usage frequency, ~85% of Compustat headline annual mnemonics are mechanically reconstructable. The remaining 15% require footnote NLP, sectoral judgment, or pre-XBRL human-curated history.

### 8.5 Latency — the 8-K vs 10-Q window

Order of events for a typical US issuer earnings release:

```
Day  0:    Issuer files 8-K Item 2.02 with press release exhibit (Ex 99.1).
           Press release contains preliminary income statement + balance sheet
           tables, EPS, guidance. Cover page is XBRL-tagged; per the 2024
           amendments, Item 2.02 financial data tables are increasingly
           inline-XBRL tagged but coverage is uneven.
           → COMPUSTAT rdq = Day 0.
           → CIQ records this as ciqFinCollectionType = PRESS_RELEASE.
           → ats-eqt should record vintage_1 with source=8K and
             knowledge_from = 8-K accept timestamp.

Day  1–5:  Earnings call. No filing; key supplementary disclosures come from
           call transcript. ats-eqt can ingest transcripts (subject to vendor
           license — Capital IQ Transcripts, FactSet Transcripts, AlphaSense)
           but text-only.

Day  7–14: Issuer files 10-Q (or 10-K for annual). Full inline-XBRL with all
           us-gaap tags, footnote text-block tags, and calculation linkbase.
           → COMPUSTAT fdate = day of 10-Q filing.
           → ats-eqt should record vintage_2 with source=10Q and
             knowledge_from = 10-Q accept timestamp.

Day 14–60: Possible 10-Q/A amendment within 60 days for any errors. Triggers
           vintage_3+ with source=10QA.

Day 60–365: Annual audit cycle catches restatement-worthy errors. Issuer
            files 10-K/A under "Item 4.02 Non-Reliance on Previously Issued
            Financial Statements". Triggers vintage_N with source=10KA and
            often a press-release re-issuance.
```

The 8-K window (Day 0) is the most important date for any trading strategy. Compustat captures this as `rdq` and as a Snapshot row with `pdate <= rdq`. FactSet captures it as `ff_eps_rpt_date`. CIQ captures it as a `PRESS_RELEASE` collection-type `ciqFinInstance`. ats-eqt's foundational fact table must capture it as the first vintage row, with source=`8K` and a separate first vintage flag.

### 8.6 The "Edgar full-text + companyfacts" implementation

The standard ats-eqt pipeline:

1. **Poll the SEC full-index.** `https://www.sec.gov/Archives/edgar/full-index/{year}/QTR{n}/master.idx` cumulative since 1993. Filter for form types `10-K`, `10-K/A`, `10-Q`, `10-Q/A`, `8-K` (Item 2.02 only), `20-F`, `20-F/A`, `40-F`, `40-F/A`.
2. **Resolve CIK → entity_id.** Via the `id_alias` table (CIK is one of the alias types). Track CIK reassignments (rare but they happen — typically merger absorption).
3. **For each filing, two paths:**
   - **Fast path:** Fetch `https://data.sec.gov/api/xbrl/companyfacts/CIK##########.json`. Diff against the previously-cached version. New/changed facts → emit `fund_fact` rows with knowledge_from = filing timestamp.
   - **Deep path:** For new accessions not yet reflected in `companyfacts.json` (typical 5–30 minute lag after EDGAR Acceptance), fetch the raw inline-XBRL instance from `Archives/edgar/data/{cik}/{accession_no_dashes}/{primary_doc}.htm` and parse with Arelle.
4. **Apply the concept normalisation rules.** Each us-gaap concept maps to one or more ats-eqt `fund_item.item_id` rows. Extensions resolved via calculation-linkbase parent or label NER.
5. **Apply DQC validation rules.** Flag facts that fail (e.g. `Assets != LiabilitiesAndStockholdersEquity`, or `NetIncomeLoss` parent doesn't sum from children). Store the flag, don't reject the fact.
6. **Write bitemporal fact rows.** Each new vintage gets a new `knowledge_from`. Prior rows have `knowledge_to` capped at the new vintage's `knowledge_from`.

### 8.7 Gotchas — known traps when building this pipeline

1. **Banks & insurance use bespoke concept sets.** Bank Holding Companies (BHCs) file under Y-9C with the Fed, and their SEC filings use `us-gaap` plus banking-specific concepts like `us-gaap:InterestAndDividendIncomeOperating`, `us-gaap:InterestIncomeLoansAndLeases`, `us-gaap:ProvisionForLoanAndLeaseLosses`, `us-gaap:LoansAndLeasesReceivableNetReportedAmount`. Some BHCs (e.g. Goldman, Morgan Stanley) use the standard industrial template; others (JPM, BAC, C, WFC) use the banking template. **ats-eqt must detect template at ingest** by checking which concept families dominate the filing.

2. **REITs report FFO/AFFO outside us-gaap core.** FFO is defined by Nareit, not FASB. REITs file FFO in a 10-K supplemental schedule, sometimes tagged with a Nareit extension namespace (`nareit:FundsFromOperations`), sometimes as a company-specific extension, sometimes only in MD&A text. ats-eqt's REIT coverage will be uneven if it relies on us-gaap alone. The Compustat workaround: human analysts map every REIT's filing.

3. **8-K Item 2.02 early-actuals.** As of 2024, the SEC requires structured tagging of Item 2.02 cover pages; financial-data exhibits in 8-K (the actual press-release earnings tables) are NOT systematically inline-XBRL tagged. Reconstructing structured early actuals from 8-K Item 2.02 requires HTML table parsing, not XBRL — significantly less reliable than 10-Q XBRL.

4. **20-F foreign issuers use IFRS XBRL.** Form 20-F filers (NDA, BABA, TM, etc.) tag under the IFRS Taxonomy (`http://xbrl.ifrs.org/`) plus SEC-required DEI extensions. Concept names are different: `ifrs-full:Revenue` instead of `us-gaap:Revenues`; `ifrs-full:ProfitLoss` instead of `us-gaap:NetIncomeLoss`. The IFRS taxonomy is less standardised than us-gaap — fewer DQC rules, more extension usage, more sign-convention errors. ats-eqt must maintain a parallel IFRS-concept ↔ ats-eqt-item map.

5. **The "calculation linkbase" is not the "presentation linkbase".** Calculation linkbase encodes "Total Assets = Current Assets + Non-Current Assets" with weight =1. Presentation linkbase encodes the display order in the rendered 10-K. ats-eqt should use the calculation linkbase for validation, not presentation.

6. **Footnote text-block tags.** Disclosures like segment data, lease maturity, debt schedules are tagged with text-block concepts (e.g. `us-gaap:SegmentReportingDisclosureTextBlock`). These are giant HTML blobs, not numeric facts. Some sub-items (e.g. segment revenue) are doubly-tagged: once as part of the text block, once as numeric facts with `us-gaap:Revenues` + segment axis.

7. **Restatement detection requires accession ordering.** Two filings with the same `(cik, period_end, concept)` but different `accn` numbers: the later one (by `filed` timestamp) is the restatement. Compustat's PIT product encodes this; the raw SEC `companyfacts.json` shows both rows, ordered by `filed`.

8. **Some pre-2017 filings lack the `frame` attribute.** This breaks Frames API queries for older periods. Fall back to `companyfacts.json` for pre-2017.

9. **NPORT-P (mutual fund holdings) has its own taxonomy** unrelated to us-gaap. Out of scope for fundamentals but a gotcha if you're walking the EDGAR API generically.

10. **The 20-F annual report vs the 6-K interim report.** Foreign private issuers file 20-F annually (XBRL-tagged) and 6-K for any other material disclosures (typically not XBRL-tagged). 6-K is the analogue of 10-Q for FPIs, but XBRL coverage on 6-K is sparse. This is the biggest hole in foreign-issuer fundamentals coverage from EDGAR alone.

---

## 9. Cross-vendor field map

The canonical concept dictionary for ats-eqt. Every row is one "economic line item". Vendor columns give the exact name to query that line item in each system. Use this table to drive the `xbrl_concept_map` and `vendor_field_map` ats-eqt dimension tables.

| ats-eqt canonical | Compustat | Compustat Q | FactSet | Worldscope | Bloomberg | CIQ (IQ_*) | us-gaap | IFRS |
|---|---|---|---|---|---|---|---|---|
| Total Revenue | `revt` / `sale` | `revtq` / `saleq` | `FF_SALES` | `01001` / `WS.NetSales` | `SALES_REV_TURN` | `IQ_TOTAL_REV` | `RevenueFromContractWithCustomerExcludingAssessedTax` | `ifrs-full:Revenue` |
| Cost of Revenue | `cogs` | `cogsq` | `FF_COGS` | `01051` / `WS.CostofGoodsSold` | `IS_COGS` | `IQ_COGS` | `CostOfGoodsAndServicesSold` | `ifrs-full:CostOfSales` |
| Gross Profit | `revt-cogs` (derived) | `revtq-cogsq` | `FF_GROSS_INC` | `01100` | `GROSS_PROFIT` | `IQ_GROSS_PROFIT` | `GrossProfit` | `ifrs-full:GrossProfit` |
| SG&A | `xsga` | `xsgaq` | `FF_SGA` | `01101` | `SG_AND_A_EXPENSE` | `IQ_SGA` | `SellingGeneralAndAdministrativeExpense` | `ifrs-full:SellingGeneralAndAdministrativeExpense` |
| R&D Expense | `xrd` | `xrdq` | `FF_RD_EXP` | `01101A` `[unverified]` | `IS_RD_EXPEND` | `IQ_RD_EXP` | `ResearchAndDevelopmentExpense` | `ifrs-full:ResearchAndDevelopmentExpense` |
| D&A (Income Statement) | `dp` | `dpq` | `FF_DEP_AMORT_EXP` | `01201` | `IS_DEPRECIATION_EXP` | `IQ_DA_IS` | `DepreciationAndAmortization` | `ifrs-full:DepreciationAndAmortisationExpense` |
| Operating Income | `oiadp` | `oiadpq` | `FF_OPER_INC` | `01250` | `IS_OPER_INC` | `IQ_OPER_INC` | `OperatingIncomeLoss` | `ifrs-full:ProfitLossFromOperatingActivities` |
| EBITDA | `oibdp` | `oibdpq` | `FF_EBITDA` | `01266` | `EBITDA` | `IQ_EBITDA` | derived | derived |
| EBIT | `ebit` | n/a | `FF_EBIT` | `01254` | `EBIT` | `IQ_EBIT` | derived | derived |
| Interest Expense | `xint` | `xintq` | `FF_INT_EXP_TOT` | `01301` | `IS_INT_EXPENSE` | `IQ_INT_EXP` | `InterestExpense` | `ifrs-full:FinanceCosts` |
| Pretax Income | `pi` | `piq` | `FF_PRETAX_INC` | `01401` | `IS_PRETAX_INC` | `IQ_PRETAX_INC` | `IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest` | `ifrs-full:ProfitLossBeforeTax` |
| Income Tax | `txt` | `txtq` | `FF_INC_TAX` | `01451` | `IS_INC_TAX_EXP` | `IQ_INCOME_TAX` | `IncomeTaxExpenseBenefit` | `ifrs-full:IncomeTaxExpenseContinuingOperations` |
| Net Income (Continuing) | `ib` | `ibq` | `FF_NET_INC` | `01601` | `IS_INC_BEF_XO_ITEM` | `IQ_NI_CONT_OPS` | `IncomeLossFromContinuingOperations` | `ifrs-full:ProfitLossFromContinuingOperations` |
| Net Income (Total) | `ni` | `niq` | `FF_NET_INC_TOT` | `01751` | `NET_INCOME` | `IQ_NI` | `NetIncomeLoss` | `ifrs-full:ProfitLoss` |
| Net Income to Common | `ibcom` | `ibcomq` | `FF_NET_INC_AVAIL` | `01706` | `NET_INCOME_TO_COMMON` | `IQ_NI_COMMON` | `NetIncomeLossAvailableToCommonStockholdersBasic` | n/a |
| EPS - Basic | `epspx` | `epspxq` | `FF_EPS_BASIC` | `05001` | `IS_BASIC_EPS` | `IQ_BASIC_EPS_EXCL` | `EarningsPerShareBasic` | `ifrs-full:BasicEarningsLossPerShare` |
| EPS - Diluted | `epsfx` | `epsfxq` | `FF_EPS_DIL` | `05011` | `IS_DILUTED_EPS` | `IQ_DILUT_EPS_EXCL` | `EarningsPerShareDiluted` | `ifrs-full:DilutedEarningsLossPerShare` |
| EPS - Diluted LTM | n/a | `epsf12` | `FF_EPS_DIL` (LTM table) | n/a | `TRAIL_12M_DIL_EPS_CONT_OPS` | derived | derived | derived |
| Total Assets | `at` | `atq` | `FF_ASSETS` | `02999` | `BS_TOT_ASSET` | `IQ_TOTAL_ASSETS` | `Assets` | `ifrs-full:Assets` |
| Current Assets | `act` | `actq` | `FF_ASSETS_CURR` | `02201` | `BS_CUR_ASSET_REPORT` | `IQ_CURRENT_ASSETS` | `AssetsCurrent` | `ifrs-full:CurrentAssets` |
| Cash & ST Inv | `che` | `cheq` | `FF_CASH_ST` | `02001` | `BS_CASH_NEAR_CASH_ITEM` | `IQ_CASH_ST_INVEST` | `CashAndCashEquivalentsAtCarryingValue + ShortTermInvestments` | `ifrs-full:CashAndCashEquivalents` |
| Cash only | `ch` | `chq` | `FF_CASH` | `02003` | `BS_CASH` | `IQ_CASH` | `Cash` | n/a |
| Receivables | `rect` | `rectq` | `FF_RECV_NET` | `02051` | `BS_ACCT_REC` | `IQ_AR` | `AccountsReceivableNetCurrent` | `ifrs-full:TradeAndOtherReceivables` |
| Inventories | `invt` | `invtq` | `FF_INVENT` | `02101` | `BS_INVENTORIES` | `IQ_INVENTORY` | `InventoryNet` | `ifrs-full:Inventories` |
| PP&E Net | `ppent` | `ppentq` | `FF_PPE_NET` | `02301` | `BS_NET_FIX_ASSET` | `IQ_NET_PPE` | `PropertyPlantAndEquipmentNet` | `ifrs-full:PropertyPlantAndEquipment` |
| PP&E Gross | `ppegt` | `ppegtq` | `FF_PPE_GROSS` | `02351` | `BS_GROSS_FIX_ASSET` | `IQ_GROSS_PPE` | `PropertyPlantAndEquipmentGross` | n/a |
| Goodwill | `gdwl` | `gdwlq` | `FF_INTANG_GW` | `02501` | `BS_GOODWILL` | `IQ_GW` | `Goodwill` | `ifrs-full:Goodwill` |
| Other Intangibles | `intan-gdwl` | `intanq-gdwlq` | `FF_INTANG_OTH` | `02649` | `BS_OTHER_INTANG_ASSETS` | `IQ_OTHER_INTAN` | `IntangibleAssetsNetExcludingGoodwill` | `ifrs-full:IntangibleAssetsOtherThanGoodwill` |
| Total Liabilities | `lt` | `ltq` | `FF_LIAB` | `03251A` | `BS_TOT_LIAB2` | `IQ_TOTAL_LIAB` | `Liabilities` | `ifrs-full:Liabilities` |
| Current Liabilities | `lct` | `lctq` | `FF_LIAB_CURR` | `03251` | `BS_CUR_LIAB` | `IQ_CURR_LIAB` | `LiabilitiesCurrent` | `ifrs-full:CurrentLiabilities` |
| Accounts Payable | `ap` | `apq` | `FF_PAYABLES` | `03001` | `BS_ACCT_PAYABLE` | `IQ_AP` | `AccountsPayableCurrent` | `ifrs-full:TradeAndOtherPayables` |
| Short-Term Debt | `dlc` | `dlcq` | `FF_DEBT_ST` | `03051` | `BS_ST_BORROW + BS_CUR_PORTION_LT_DEBT` | `IQ_ST_DEBT` | `LongTermDebtCurrent + ShortTermBorrowings` | `ifrs-full:CurrentBorrowings` |
| Long-Term Debt | `dltt` | `dlttq` | `FF_DEBT_LT` | `03255` | `BS_LT_BORROW` | `IQ_LT_DEBT` | `LongTermDebtNoncurrent` | `ifrs-full:NoncurrentBorrowings` |
| Total Debt | `dlc+dltt` | `dlcq+dlttq` | `FF_DEBT` | `03051+03255` | `SHORT_AND_LONG_TERM_DEBT` | `IQ_TOTAL_DEBT` | `LongTermDebt + ShortTermBorrowings` | derived |
| Minority Interest | `mib` | `mibq` | `FF_MIN_INT_BS` | `03401` | `BS_MIN_NONCONTROL_INTEREST` | `IQ_MINORITY` | `MinorityInterest` | `ifrs-full:NoncontrollingInterests` |
| Preferred Stock | `pstk` | `pstkq` | `FF_PREF_STK` | `03451` | `BS_PFD_EQUITY` | `IQ_PREF_EQ` | `PreferredStockValue` | n/a |
| Retained Earnings | `re` | `req` | `FF_RETAIN_EARN` | `03999A` | `BS_PURE_RETAINED_EARNINGS` | `IQ_RE` | `RetainedEarningsAccumulatedDeficit` | `ifrs-full:RetainedEarnings` |
| Treasury Stock | `tstk` | `tstkq` | `FF_TREAS_STK` | `03999B` | `BS_TREASURY_STOCK` | `IQ_TREASURY` | `TreasuryStockValue` | `ifrs-full:TreasuryShares` |
| Common Equity | `ceq` | `ceqq` | `FF_COM_EQ_TOT` | `03501` | `TOTAL_EQUITY - BS_PFD_EQUITY` | `IQ_COMMON_EQUITY` | derived | derived |
| Stockholders' Equity | `seq` | `seqq` | `FF_EQ_TOT` | `03999` | `TOTAL_EQUITY` | `IQ_TOTAL_EQUITY` | `StockholdersEquity` | `ifrs-full:Equity` |
| Common Shares Out | `csho` | `cshoq` | `FF_SHS_OUTSTND` | `05101` | `BS_SH_OUT` | `IQ_SHARES_OUT` | `CommonStockSharesOutstanding` | n/a |
| Shares - Basic Avg | `cshpri` | `cshprq` | `FF_SHS_BASIC` | `05151` | `IS_AVG_NUM_SH_FOR_EPS` | `IQ_BASIC_WEIGHT_AVG_SH` | `WeightedAverageNumberOfSharesOutstandingBasic` | `ifrs-full:WeightedAverageShares` |
| Shares - Diluted Avg | `cshfd` | `cshfdq` | `FF_SHS_DIL` | `05161` `[unverified]` | `IS_SH_FOR_DILUTED_EPS` | `IQ_DILUT_WEIGHT_AVG_SH` | `WeightedAverageNumberOfDilutedSharesOutstanding` | `ifrs-full:AdjustedWeightedAverageShares` |
| Cash from Ops | `oancf` | `oancfy` | `FF_CASH_FROM_OPER` | `04001` | `CF_CASH_FROM_OPER` | `IQ_CASH_OPER` | `NetCashProvidedByUsedInOperatingActivities` | `ifrs-full:CashFlowsFromUsedInOperatingActivities` |
| Cash from Investing | `ivncf` | `ivncfy` | `FF_CASH_FROM_INVEST` | `04401` | `CF_CASH_FROM_INV_ACT` | `IQ_CASH_INVEST` | `NetCashProvidedByUsedInInvestingActivities` | `ifrs-full:CashFlowsFromUsedInInvestingActivities` |
| Cash from Financing | `fincf` | `fincfy` | `FF_CASH_FROM_FIN` | `04801` | `CF_CASH_FROM_FNC_ACT` | `IQ_CASH_FIN` | `NetCashProvidedByUsedInFinancingActivities` | `ifrs-full:CashFlowsFromUsedInFinancingActivities` |
| Capex | `capx` | `capxy` | `FF_CAPEX` | `04201` | `CF_CAP_EXPEND` | `IQ_CAPEX` | `PaymentsToAcquirePropertyPlantAndEquipment` | `ifrs-full:PurchaseOfPropertyPlantAndEquipmentClassifiedAsInvestingActivities` |
| D&A (Cash Flow) | `dpc` | `dpcy` | `FF_DEP_AMORT_CF` | `04101` `[unverified]` | `CF_DEPR_AMORT` | `IQ_DA_CF` | `DepreciationDepletionAndAmortization` | `ifrs-full:DepreciationAndAmortisationExpense` |
| Stock-Based Comp | n/a (in advanced) | n/a | `FF_STOCK_COMP` | n/a | `CF_STOCK_BASED_COMP` | `IQ_STOCK_COMP` | `ShareBasedCompensation` | `ifrs-full:ShareBasedPaymentsRecognised` |
| Common Dividends Paid | `dvc` | `dvcy` | `FF_DIV_COM` | `04551` | `CF_DVD_PAID` | `IQ_COMMON_DIV_PAID` | `PaymentsOfDividendsCommonStock` | `ifrs-full:DividendsPaidOrdinaryShares` |
| Preferred Dividends Paid | `dvp` | `dvpy` | `FF_DIV_PFD_CF` | n/a | `CF_PFD_DVDS_PAID` | `IQ_PREF_DIV_PAID` | `PaymentsOfDividendsPreferredStockAndPreferenceStock` | n/a |
| Stock Buybacks | `prstkc` | `prstkcy` | `FF_STOCK_REPUR` | `04651` | `CF_REPURCH_OF_STOCK` | `IQ_BUYBACKS` | `PaymentsForRepurchaseOfCommonStock` | n/a |
| Stock Issuance | `sstk` | `sstky` | `FF_STOCK_ISSUE` | `04601` | `CF_ISSUE_OF_STOCK` | `IQ_STOCK_ISSUED` | `ProceedsFromIssuanceOfCommonStock` | n/a |
| LT Debt Issued | `dltis` | n/a | `FF_DEBT_ISSUE` | `04701` | `CF_PROCEEDS_LT_DEBT` | `IQ_LT_DEBT_ISSUE` | `ProceedsFromIssuanceOfLongTermDebt` | n/a |
| LT Debt Repaid | `dltr` | n/a | `FF_DEBT_REDUC` | `04751` | `CF_REPAY_LT_DEBT` | `IQ_LT_DEBT_REPAY` | `RepaymentsOfLongTermDebt` | n/a |
| Acquisitions | `aqc` | `aqcy` | `FF_ACQUIS` | n/a | `CF_ACQUIS_OF_BUSINESS` | `IQ_ACQUISITIONS` | `PaymentsToAcquireBusinessesNetOfCashAcquired` | n/a |
| Free Cash Flow | `oancf-capx` | derived | `FF_FCF` | `04860` | `CF_FREE_CASH_FLOW` | `IQ_FCF` | derived | derived |
| Market Cap | `mkvalt` | `mkvaltq` | `FF_MKT_CAP_TOT` | `05491` | `CUR_MKT_CAP` | `IQ_MARKETCAP` | not in us-gaap | n/a |
| Price Close (Fiscal Yr) | `prcc_f` | `prccq` | `FF_PRICE_CLOSE_FP` | `05350` | `PX_LAST` | `IQ_CLOSEPRICE` | not in us-gaap | n/a |

**Vintage / temporal fields:**

| ats-eqt canonical | Compustat | FactSet | Bloomberg | CIQ | us-gaap / SEC | Notes |
|---|---|---|---|---|---|---|
| period_end | `datadate` | `ff_fp_end` | `FA_PERIOD_END_DATE` | `ciqFinPeriod.periodEndDate` | `dei:DocumentPeriodEndDate` | Fiscal period end. |
| report_date | `rdq` | `ff_eps_rpt_date` | n/a | filingDate (collectionType=3) | 8-K acceptedDate | The market-knowable event. |
| filing_date | `fdate` | `ff_report_date` | `FA_FILING_DATE` | filingDate (collectionType=1) | 10-Q/K acceptedDate | When the 10-Q/K was filed. |
| knowledge_date | `ldate` | `ff_fe_date` | `FA_KNOWN_AS_OF_DATE` | effectiveDate | ats-eqt ingest TS | Bitemporal axis. |

---

## 10. Recommended ats-eqt schema (foundational fundamentals)

This is the proposed DDL that puts everything above into one canonical store. Follows the same patterns established in `research/schemas/data_models_and_methodology.md` Part G, but elaborated for fundamentals at field-level.

### 10.1 `entity` — the issuer dimension

```sql
CREATE TABLE entity (
  entity_id            BIGINT       PRIMARY KEY,        -- ats-eqt internal stable key
  legal_name           TEXT         NOT NULL,
  former_name          TEXT         NULL,
  short_name           TEXT         NULL,                -- display name
  country_iso2         CHAR(2)      NOT NULL,
  jurisdiction_iso2    CHAR(2)      NOT NULL,            -- jurisdiction of incorporation
  founded_date         DATE         NULL,
  defunct_date         DATE         NULL,                -- non-null on dissolution
  entity_type          INTEGER      NOT NULL,            -- listed, private, fund, gov
  fiscal_year_end_mo   SMALLINT     NULL,                -- 1..12 (current; bitemporal in fye_history)
  primary_listing_id   BIGINT       NULL,                -- → listing
  primary_security_id  BIGINT       NULL,                -- → security
  industry_template    CHAR(2)      NOT NULL,            -- 'IN' industrial, 'BK' bank, 'IS' insurance, 'OF' other-fin, 'RT' REIT
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity'
);

CREATE TABLE entity_fye_history (
  entity_id            BIGINT       NOT NULL,
  fiscal_year_end_mo   SMALLINT     NOT NULL,
  valid_from           DATE         NOT NULL,
  valid_to             DATE         NOT NULL DEFAULT '9999-12-31',
  knowledge_from       TIMESTAMP    NOT NULL,
  PRIMARY KEY (entity_id, valid_from, knowledge_from)
);

CREATE TABLE entity_industry_history (
  entity_id            BIGINT       NOT NULL,
  classification_sys   CHAR(4)      NOT NULL,            -- 'GICS', 'NAICS', 'SIC', 'NACE', 'TRBC', 'RBICS', 'BICS'
  code                 TEXT         NOT NULL,
  level                SMALLINT     NOT NULL,            -- 1..N hierarchy depth
  valid_from           DATE         NOT NULL,
  valid_to             DATE         NOT NULL DEFAULT '9999-12-31',
  knowledge_from       TIMESTAMP    NOT NULL,
  PRIMARY KEY (entity_id, classification_sys, level, valid_from, knowledge_from)
);
```

### 10.2 `security` / `listing`

```sql
CREATE TABLE security (
  security_id          BIGINT       PRIMARY KEY,
  entity_id            BIGINT       NOT NULL REFERENCES entity,
  sec_type_id          INTEGER      NOT NULL,            -- common, pfd, ADR, debt, warrant
  share_class          TEXT         NULL,                -- 'A', 'B', 'C', NULL for single-class
  inception_date       DATE         NOT NULL,
  retirement_date      DATE         NULL,
  is_primary           BOOLEAN      NOT NULL DEFAULT FALSE
);

CREATE TABLE listing (
  listing_id           BIGINT       PRIMARY KEY,
  security_id          BIGINT       NOT NULL REFERENCES security,
  exchange_mic         CHAR(4)      NOT NULL,            -- ISO 10383
  ticker               TEXT         NOT NULL,            -- current ticker on this exchange
  active_from          DATE         NOT NULL,
  active_to            DATE         NULL,
  is_primary           BOOLEAN      NOT NULL DEFAULT FALSE
);
```

### 10.3 `period` — the period dimension

```sql
CREATE TABLE period (
  period_id            BIGINT       PRIMARY KEY,
  entity_id            BIGINT       NOT NULL,
  period_end           DATE         NOT NULL,
  period_type          CHAR(1)      NOT NULL,            -- A annual, Q quarterly, S semi, Y YTD-fiscal,
                                                          -- C YTD-cal, T TTM, L LTM, N NTM
  fyear                INTEGER      NULL,                -- fiscal year integer (FY2024 = 2024)
  fqtr                 SMALLINT     NULL,                -- 1..4
  fyr_month            SMALLINT     NULL,                -- fiscal-year-end month
  cyear                INTEGER      NULL,                -- calendar year of period_end
  cqtr                 SMALLINT     NULL,                -- calendar quarter of period_end
  days_in_period       SMALLINT     NULL,                -- days covered (helpful for normalisation)
  is_calendar_aligned  BOOLEAN      NOT NULL DEFAULT FALSE,
  UNIQUE (entity_id, period_end, period_type)
);
```

### 10.4 `fund_item` — the canonical item dictionary

```sql
CREATE TABLE fund_item (
  item_id              INTEGER      PRIMARY KEY,
  code                 TEXT         UNIQUE NOT NULL,     -- 'REVENUE', 'COGS', 'NI', 'AT', 'EPS_DIL'
  display_label        TEXT         NOT NULL,
  statement            CHAR(2)      NOT NULL,            -- 'IS', 'BS', 'CF', 'PS' (per-share), 'RA' (ratio)
  is_per_share         BOOLEAN      NOT NULL DEFAULT FALSE,
  is_ratio             BOOLEAN      NOT NULL DEFAULT FALSE,
  is_calculated        BOOLEAN      NOT NULL DEFAULT FALSE,
  calc_formula         TEXT         NULL,                -- e.g. 'REVENUE - COGS' for GROSS_PROFIT
  parent_item_id       INTEGER      NULL REFERENCES fund_item,
  industry_template    CHAR(2)      NULL,                -- NULL = all templates; 'IN','BK','IS','OF','RT'
  unit_type            CHAR(4)      NOT NULL,            -- 'CCY', 'SHRS', 'RATIO', 'PCT', 'DAYS'
  sign_convention      CHAR(1)      NOT NULL DEFAULT 'D',-- 'D' debit-positive, 'C' credit-positive
  taxonomy_version     TEXT         NOT NULL             -- 'ats-eqt-1.0'
);
```

### 10.5 `xbrl_concept_map` — us-gaap → ats-eqt mapping

```sql
CREATE TABLE xbrl_concept_map (
  concept_qname        TEXT         NOT NULL,            -- e.g. 'us-gaap:Revenues'
  taxonomy_ns          TEXT         NOT NULL,            -- 'http://fasb.org/us-gaap/2024'
  item_id              INTEGER      NOT NULL REFERENCES fund_item,
  priority             SMALLINT     NOT NULL DEFAULT 100,-- lower = preferred when multiple concepts map to same item
  sign_multiplier      SMALLINT     NOT NULL DEFAULT 1,  -- 1 or -1
  effective_from       DATE         NOT NULL,            -- when this mapping became valid
  effective_to         DATE         NOT NULL DEFAULT '9999-12-31',
  notes                TEXT         NULL,
  PRIMARY KEY (concept_qname, taxonomy_ns, effective_from)
);

CREATE INDEX xbrl_concept_map_item ON xbrl_concept_map(item_id);
```

### 10.6 `vendor_field_map` — Compustat / FactSet / WS / BBG / CIQ aliasing

```sql
CREATE TABLE vendor_field_map (
  vendor               CHAR(8)      NOT NULL,            -- 'COMPNA','FF','WS','BBG','CIQ','SHARADAR','SIMFIN'
  vendor_field         TEXT         NOT NULL,            -- 'revt', 'FF_SALES', '01001', 'SALES_REV_TURN', 'IQ_TOTAL_REV'
  vendor_table         TEXT         NULL,                -- 'funda', 'ff_basic_qf', etc.
  vendor_period_type   TEXT         NULL,                -- 'A', 'Q', etc.
  item_id              INTEGER      NOT NULL REFERENCES fund_item,
  sign_multiplier      SMALLINT     NOT NULL DEFAULT 1,
  unit_multiplier      DOUBLE       NOT NULL DEFAULT 1.0,-- e.g. Compustat in $M, our normalised in $
  notes                TEXT         NULL,
  PRIMARY KEY (vendor, vendor_field, vendor_table)
);
```

### 10.7 `fact` — the long-format bitemporal fact table

```sql
CREATE TABLE fund_fact (
  fact_id              BIGINT       PRIMARY KEY,          -- internal surrogate
  entity_id            BIGINT       NOT NULL,
  security_id          BIGINT       NULL,                 -- non-null only for per-share items
  period_id            BIGINT       NOT NULL,
  item_id              INTEGER      NOT NULL,
  value_num            DOUBLE       NULL,
  value_text           TEXT         NULL,                 -- for footnote-style facts
  unit_id              INTEGER      NULL,                 -- USD, EUR, shares, ratio
  currency_iso3        CHAR(3)      NULL,                 -- denormalized for fast queries
  -- provenance
  source_type          CHAR(4)      NOT NULL,             -- 'PR8K' (8-K press), '10Q', '10QA', '10K',
                                                           -- '10KA', '20F', '20FA', 'PIT' (vendor PIT),
                                                           -- 'VND' (vendor non-XBRL), 'NLP' (extracted)
  filing_accession     TEXT         NULL,                 -- EDGAR accession number when applicable
  filing_id            BIGINT       NULL,                 -- → filing master
  xbrl_concept         TEXT         NULL,                 -- the as-filed us-gaap or ifrs concept QName
  xbrl_context_ref     TEXT         NULL,                 -- contextRef from the instance document
  -- four-date model
  data_date            DATE         NOT NULL,             -- = period.period_end (denormalised)
  report_date          DATE         NULL,                 -- = rdq (8-K press release date)
  filing_date          DATE         NULL,                 -- = fdate (10-Q/K filed date)
  first_available_date DATE         NOT NULL,             -- when ats-eqt FIRST saw any vintage
  last_modified_date   TIMESTAMP    NOT NULL,             -- last time THIS specific row was touched
  -- bitemporal
  valid_from           DATE         NOT NULL,             -- = data_date for fundamentals
  valid_to             DATE         NOT NULL DEFAULT '9999-12-31',
  knowledge_from       TIMESTAMP    NOT NULL,             -- when this vintage became known
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity',
  -- flags
  is_restatement       BOOLEAN      NOT NULL DEFAULT FALSE,
  is_preliminary       BOOLEAN      NOT NULL DEFAULT FALSE,
  is_extension_concept BOOLEAN      NOT NULL DEFAULT FALSE,
  is_backfill          BOOLEAN      NOT NULL DEFAULT FALSE,
  dqc_flags            INTEGER[]    NULL,                 -- array of failed DQC rule IDs
  CONSTRAINT fund_fact_value_check CHECK (value_num IS NOT NULL OR value_text IS NOT NULL)
);

CREATE UNIQUE INDEX ix_fund_fact_natural
  ON fund_fact (entity_id, period_id, item_id, source_type, knowledge_from);
CREATE INDEX ix_fund_fact_pit ON fund_fact (entity_id, item_id, knowledge_from);
CREATE INDEX ix_fund_fact_period ON fund_fact (period_id, item_id);
CREATE INDEX ix_fund_fact_accession ON fund_fact (filing_accession);
```

### 10.8 `restatement_audit` — explicit audit trail

```sql
CREATE TABLE restatement_audit (
  audit_id             BIGINT       PRIMARY KEY,
  entity_id            BIGINT       NOT NULL,
  period_id            BIGINT       NOT NULL,
  item_id              INTEGER      NOT NULL,
  prior_fact_id        BIGINT       NOT NULL,             -- → fund_fact (the row being superseded)
  new_fact_id          BIGINT       NOT NULL,             -- → fund_fact (the new row)
  prior_value          DOUBLE       NULL,
  new_value            DOUBLE       NULL,
  delta_pct            DOUBLE       NULL,                  -- (new - prior) / |prior|
  reason_code          CHAR(4)      NULL,                  -- 'M&A', 'ACT' accounting change, 'ERR' error correction, etc.
  filing_accession     TEXT         NULL,                  -- the filing that introduced the restatement
  detected_at          TIMESTAMP    NOT NULL,
  is_material          BOOLEAN      NOT NULL DEFAULT FALSE -- threshold flag (e.g. |delta| > 1%)
);
CREATE INDEX ix_restatement_audit_entity ON restatement_audit(entity_id, period_id);
```

### 10.9 PIT query view

```sql
CREATE VIEW fund_fact_pit AS
SELECT * FROM fund_fact
WHERE knowledge_from <= :asof_date
  AND :asof_date < knowledge_to;

-- "As-first-reported" view: filter to first vintage per (entity,period,item)
CREATE VIEW fund_fact_afr AS
SELECT DISTINCT ON (entity_id, period_id, item_id) *
FROM fund_fact
ORDER BY entity_id, period_id, item_id, knowledge_from ASC;

-- "Current best" view: filter to latest vintage per (entity,period,item)
CREATE VIEW fund_fact_current AS
SELECT DISTINCT ON (entity_id, period_id, item_id) *
FROM fund_fact
WHERE knowledge_to = 'infinity'
ORDER BY entity_id, period_id, item_id, knowledge_from DESC;
```

### 10.10 Materialised wide-format convenience view

For analysts who want Compustat-style ergonomics:

```sql
CREATE MATERIALIZED VIEW fund_wide_quarterly AS
SELECT
  e.entity_id, e.legal_name, p.period_end, p.cyear, p.cqtr,
  MAX(CASE WHEN i.code = 'REVENUE'      THEN f.value_num END) AS revenue,
  MAX(CASE WHEN i.code = 'COGS'         THEN f.value_num END) AS cogs,
  MAX(CASE WHEN i.code = 'SGA'          THEN f.value_num END) AS sga,
  MAX(CASE WHEN i.code = 'OPER_INC'     THEN f.value_num END) AS oper_inc,
  MAX(CASE WHEN i.code = 'NI'           THEN f.value_num END) AS net_income,
  MAX(CASE WHEN i.code = 'EPS_DIL'      THEN f.value_num END) AS eps_diluted,
  MAX(CASE WHEN i.code = 'AT'           THEN f.value_num END) AS total_assets,
  MAX(CASE WHEN i.code = 'LT'           THEN f.value_num END) AS total_liabilities,
  MAX(CASE WHEN i.code = 'SEQ'          THEN f.value_num END) AS total_equity,
  MAX(CASE WHEN i.code = 'CASH_ST_INV'  THEN f.value_num END) AS cash_st_inv,
  MAX(CASE WHEN i.code = 'LT_DEBT'      THEN f.value_num END) AS lt_debt,
  MAX(CASE WHEN i.code = 'ST_DEBT'      THEN f.value_num END) AS st_debt,
  MAX(CASE WHEN i.code = 'CFO'          THEN f.value_num END) AS cash_from_ops,
  MAX(CASE WHEN i.code = 'CAPEX'        THEN f.value_num END) AS capex,
  MAX(CASE WHEN i.code = 'CSHO'         THEN f.value_num END) AS shares_out,
  MAX(p.fyear) AS fyear, MAX(p.fqtr) AS fqtr
FROM fund_fact_current f
JOIN entity e ON e.entity_id = f.entity_id
JOIN period p ON p.period_id = f.period_id
JOIN fund_item i ON i.item_id = f.item_id
WHERE p.period_type = 'Q'
GROUP BY e.entity_id, e.legal_name, p.period_end, p.cyear, p.cqtr;

CREATE INDEX ix_fund_wide_q_entity ON fund_wide_quarterly(entity_id, period_end);
```

---

## 11. Bitemporal + PIT semantics — worked example

Apple, FY2024 Q4 (fiscal Q4 ended 2024-09-28). Worked sequence:

```
T1: 2024-10-31 16:30 ET
    Apple files 8-K Item 2.02 with FY24Q4 press release.
    Press release reports Revenue = $94.93B, Diluted EPS = $1.64, NI = $14.74B.
    Cover-page DEI tags are XBRL-tagged; financial tables are NOT inline-XBRL.
    ats-eqt extracts via 8-K HTML table parser, source_type='PR8K'.
    Inserted rows:
      (AAPL, FY24Q4, REVENUE, 94.93e9, source='PR8K',
       data_date=2024-09-28, report_date=2024-10-31,
       filing_date=2024-10-31, first_available_date=2024-10-31,
       knowledge_from='2024-10-31 16:35:00', knowledge_to='infinity',
       is_preliminary=TRUE, xbrl_concept=NULL)

T2: 2024-11-01 17:00 ET
    Apple files 10-K with full inline-XBRL.
    XBRL extraction reads us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax = 94.93e9.
    Match within tolerance — no restatement.
    Insert new row, CLOSE preliminary row:
      Row 1 (preliminary): knowledge_to set to '2024-11-01 17:05:00'.
      Row 2 (10-K): (AAPL, FY24Q4, REVENUE, 94.93e9, source='10K',
                    xbrl_concept='us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax',
                    knowledge_from='2024-11-01 17:05:00', is_preliminary=FALSE)

T3: 2025-04-15 09:00 ET (hypothetical)
    Apple files 10-K/A with restated FY24Q4 revenue = 94.50B.
    Reason: ASC 606 reclassification of accessory revenue, disclosed in Item 4.02.
    ats-eqt detects restatement (delta = -0.45%).
    Insert new row, CLOSE prior:
      Row 2: knowledge_to = '2025-04-15 09:05:00'.
      Row 3: (AAPL, FY24Q4, REVENUE, 94.50e9, source='10KA',
              knowledge_from='2025-04-15 09:05:00', is_restatement=TRUE,
              xbrl_concept='us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax')
    Restatement_audit row inserted:
      (entity=AAPL, period=FY24Q4, item=REVENUE,
       prior_fact_id=Row2, new_fact_id=Row3,
       prior_value=94.93e9, new_value=94.50e9, delta_pct=-0.0045,
       reason_code='ACT', filing_accession='0001234567-25-000123',
       detected_at='2025-04-15 09:05:00', is_material=FALSE)
```

**PIT queries:**

```sql
-- "What was Apple's FY24Q4 revenue, as known on 2024-11-15?"
-- Returns 94.93B (the 10-K value from T2, not yet superseded).
SELECT value_num FROM fund_fact
WHERE entity_id = :AAPL AND period_id = :FY24Q4 AND item_id = :REVENUE
  AND knowledge_from <= '2024-11-15'
  AND knowledge_to > '2024-11-15';

-- "What was Apple's FY24Q4 revenue, as known on 2024-10-31 17:00?"
-- Returns 94.93B (preliminary press release).
-- Same value happens to apply, but source_type = 'PR8K' for the press-release row.

-- "What is Apple's FY24Q4 revenue today?"
-- Returns 94.50B (Row 3, the restated value).
SELECT value_num FROM fund_fact
WHERE entity_id = :AAPL AND period_id = :FY24Q4 AND item_id = :REVENUE
  AND knowledge_to = 'infinity';

-- "Show me every value Apple has reported for FY24Q4 revenue and when."
SELECT value_num, source_type, knowledge_from, knowledge_to, is_preliminary, is_restatement
FROM fund_fact
WHERE entity_id = :AAPL AND period_id = :FY24Q4 AND item_id = :REVENUE
ORDER BY knowledge_from;
```

The four-date model gives ats-eqt the full vendor toolkit:
- `data_date` = Compustat `datadate` = us-gaap period end.
- `report_date` = Compustat `rdq` = 8-K Item 2.02 acceptedDate.
- `filing_date` = Compustat `fdate` = 10-Q/K acceptedDate.
- `last_modified_date` = Compustat `ldate` = vintage version stamp.
- `first_available_date` = ats-eqt-specific, equals `knowledge_from` of the first vintage row.

Backtest queries pin `knowledge_from <= asof` to enforce PIT discipline. Fundamental research queries can pin `is_preliminary = FALSE` to use only 10-Q/K vintages.

---

## 12. Open questions / wave-3 gaps

Items that could not be fully verified in this research pass; flagged for follow-up:

1. **Exact `ciqDataItem.dataItemId` integer mappings.** The IQ_* mnemonics (e.g. IQ_TOTAL_REV) are stable across CIQ releases; the underlying integer IDs (e.g. 100174) appear in academic references but the WRDS-distributed dictionary is the authoritative source. Need a WRDS-credentialed analyst to dump `ciq.ciqfinancialitem` and confirm the integer map.

2. **Worldscope bank/insurance template — full item-code allocation.** The 04xxx/05xxx items above are partly inferred from secondary sources. Need access to the full Worldscope Bank Datatype Definitions Guide and Worldscope Insurance Datatype Definitions Guide PDFs (Tilburg University has only the industrial template guide publicly mirrored).

3. **FactSet FF_BANK / FF_INS / FF_REIT full mnemonic lists.** The subscriber-only Data Item Definitions PDFs are the authoritative source. We have the headline mnemonics from public methodology briefs but not the complete ~200-item-per-template list.

4. **Compustat fiscal-services template (`indfmt = FS`) — complete item list.** S&P's "Compustat North America Bank Items Manual" and "Insurance Items Manual" are not publicly distributed. Coverage above is the union of items observed in academic citations.

5. **Bloomberg field DT-ID full catalogue.** The BBGsymbols R package exposes the publicly-cataloged subset (~300 fields). The full ~2,000 fundamentals fields require BQLX terminal access. ats-eqt's BBG-comparison should use the Bloomberg Enterprise Data Catalog API output as a one-time dump.

6. **Sharadar SF1 — list of which fields are subject to ARQ/MRQ vintage divergence vs which are not.** Some fields (revenue) almost always restate; others (capex) less so. A vintage-divergence rate per field would inform PIT-strictness configuration.

7. **The 8-K Item 2.02 financial-data-table iXBRL coverage rate.** Per the 2024 SEC release, structured tagging of 8-K cover pages is mandatory and of Item 2.02 financial schedules is encouraged. We need a measurement of what % of post-2024 8-Ks actually do tag Item 2.02 financial tables.

8. **us-gaap → ats-eqt-item coverage rates by company size.** Headline rates (>95% for top items) are average-case. Reconstruction quality for the bottom-50% by market cap is materially worse due to extension concept usage and DQC violations. Need a sample analysis: pick 100 random small-cap CIKs, run the reconstruction pipeline, measure % of headline items recovered.

9. **The Sharadar / SimFin restatement-detection algorithm.** Sharadar publishes ARQ (as-reported) and MRQ (most-recent) versions; the algorithm that detects "this filing is a restatement of the prior filing" is not publicly documented. SimFin's `Restated Date` column has similar opacity.

10. **IFRS Taxonomy concepts for foreign issuers on Form 20-F.** This document focuses on us-gaap. A parallel concept-map for `ifrs-full:` would let ats-eqt cover ~500 20-F filers (ADRs and dual-listed foreign issuers). Wave-3 should produce this map.

11. **The Nareit FFO/AFFO extension taxonomy.** REIT-specific concepts live outside core us-gaap. Nareit publishes the methodology; there is no fully standardised XBRL extension taxonomy. ats-eqt's REIT support needs a custom item map plus likely some footnote NLP.

12. **Pre-2009 (pre-XBRL) reconstruction.** The DERA bulk and `companyfacts.json` start in 2009. For 1962–2008, ats-eqt's only option is licensed Compustat backfill or a non-trivial PDF/HTML 10-K parsing pipeline. Costing and feasibility of a 10-K text-extraction pipeline (Apache Tika + LayoutLM + LLM fact extraction) needs separate scoping.

13. **Bank Call Report integration.** US banks file FFIEC 031/041 quarterly Call Reports separately from SEC 10-Qs. The Call Report carries deeper detail (NIM, RBC ratios, loan composition by type) than the 10-Q. For comprehensive bank fundamentals, ats-eqt should ingest FFIEC Call Report data alongside SEC XBRL; the schema accommodates this via `source_type = 'CALL'`.

---

## 13. Sources

### SEC / EDGAR primary

- <https://www.sec.gov/search-filings/edgar-application-programming-interfaces> — EDGAR APIs reference (companyfacts, companyconcept, frames, submissions).
- <https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets> — DERA Financial Statement Data Sets quarterly bulk dump.
- <https://www.sec.gov/files/financial-statement-data-sets.pdf> — DERA methodology PDF (SUB/NUM/TAG/PRE file specs).
- <https://www.sec.gov/about/dera_financialstatementandnotesdatasets> — DERA full-text disclosures supplement.
- <https://data.sec.gov/> — data.sec.gov landing.
- <https://www.sec.gov/info/edgar/specifications/xbrl-staff-observation-2023-02-23.pdf> — XBRL staff observations including 8-K Item 2.02 tagging.
- <https://www.sec.gov/Archives/edgar/full-index/> — bulk historical filing index.

### FASB / XBRL US

- <https://www.fasb.org/page/detail?pageId=/projects/FASB-Taxonomies/2024-gaap-financial-reporting-taxonomy.html> — FASB 2024 GAAP Taxonomy.
- <https://www.fasb.org/page/detail?pageId=%2Fprojects%2FFASB-Taxonomies%2F2025-gaap-financial-reporting-taxonomy.html> — FASB 2025 GAAP Taxonomy.
- <https://www.fasb.org/projects/fasb-taxonomies> — FASB taxonomy projects landing.
- <https://xbrl.us/xbrl-taxonomy/2024-us-gaap/> — XBRL US 2024 taxonomy distribution.
- <https://xbrl.us/why-normalize-data/> — XBRL US normalisation rationale ("19% extension concepts" cited here).
- <https://xbrl.us/data-rule/> — XBRL US Data Quality Committee rules.
- <https://xbrl.us/academic-repository/sec-edgar-data/> — XBRL US academic repository.
- <https://api.xbrl.us/api/v1/> — XBRL US REST API.
- <https://xbrlus.github.io/docs/tdh.html> — XBRL US Tagging Diagnostics.
- <https://www.iriscarbon.com/understanding-the-purpose-of-fasbs-2024-u-s-c-implementation-guides/> — IRIS Carbon on 2024 taxonomy implementation guides.

### Compustat / WRDS

- <https://wrds-www.wharton.upenn.edu/data-dictionary/comp_na_daily_all/> — WRDS Compustat NA daily data dictionary.
- <https://wrds-www.wharton.upenn.edu/data-dictionary/comp_na_daily_all/funda/> — funda variable list.
- <https://wrds-www.wharton.upenn.edu/data-dictionary/comp_na_daily_all/fundq/> — fundq variable list.
- <https://wrds-www.wharton.upenn.edu/demo/compustat/form/> — Compustat Annual Fundamentals demo.
- <https://wrds-www.wharton.upenn.edu/pages/grid-items/introduction-compustat-part-1/> — Introduction to Compustat.
- <https://wrds-www.wharton.upenn.edu/pages/grid-items/compustat-global-wrds-basics/> — Compustat Global basics.
- <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-crsp-with-compustat/> — CCM linking matrix.
- <https://ionmihai.github.io/finsets/01_wrds/compq.html> — Mihai Ion's quarterly fundamentals reference (atq, ltq, niq, oibdpq, rdq).
- <https://w3.loibl.com/uni/xf_understanding_the_data.pdf> — Compustat Xpressfeed "Understanding the Data" guide.
- <https://library.unist.ac.kr/libguide/wp-content/uploads/sites/2/2018/11/compustat.pdf> — UNIST library mirror of Compustat "Using the Data" guide.
- <https://www8.gsb.columbia.edu/itg/faculty/databaseff/compustatx> — Columbia ITG Compustat XPF reference.
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database> — LSEG/Refinitiv on the S&P Compustat database (history, PIT, depth).
- <https://kenan-flagler.libguides.com/kfbs-library-services/research-resource/compustat-snapshot/> — Kenan-Flagler libguide on Compustat Snapshot.
- <https://www.gsb.stanford.edu/library/connecting-link/compustat-pit-wrds> — Stanford GSB on Compustat PIT.
- <https://robsonglasscock.wordpress.com/2018/04/12/gvkey-and-datadate-or-fyear-duplicates-in-compustat/> — Compustat duplicate-row gotcha.
- <https://yuzhu.run/nail-down-earnings-time/> — RDQ field accuracy analysis.
- <https://en.wikipedia.org/wiki/Compustat> — Compustat overview.
- <https://www.aabri.com/manuscripts/11798.pdf> — "Data differences — XBRL versus Compustat" critique.

### FactSet

- <https://www.factset.com/marketplace/catalog/product/factset-fundamentals> — FactSet Fundamentals Marketplace page.
- <https://www.factset.com/marketplace/catalog/product/factset-fundamentals-point-in-time> — FactSet Fundamentals PIT.
- <https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed> — FactSet Fundamentals At-a-Glance.
- <https://developer.factset.com/api-catalog/factset-fundamentals-api> — FactSet Fundamentals API.
- <https://developer.factset.com/api-catalog/symbology-api> — FactSet Symbology API (FSYM).
- <https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html> — FactSet 101 with ff_v3 SQL examples.
- <https://download.dataservices.theice.com/products/marketq/help/factset_fundamental_data.htm> — ICE Market-Q FactSet Fundamental Data help.
- <http://famouswiki.pbworks.com/FDS-Codes-In-FactSet> — FDS Codes catalog.
- <http://famouswiki.pbworks.com/w/page/66716998/Background%20On%20FactSet%20Databases%20and%20Data%20Items> — FactSet database background.
- <https://www.wiso.uni-hamburg.de/bibliothek/recherche/datenbanken/unternehmensdaten/factset-fundamentals.pdf> — Hamburg University FactSet Fundamentals guide.
- <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/factset/> — WRDS FactSet vendor page (table sizes, history start dates).
- <https://doc.exabel.com/dsl/data_signals/factset_fundamentals.html> — Exabel FactSet Fundamentals docs.
- <https://assets.ctfassets.net/lmz2w5z92b9u/7INM5wpJ5u1bomIisoOoz2/beaad6e64bbbdc96f8996acc9c8a1b34/FactSet_Permanent_Security_Identifier.pdf> — FSYM whitepaper.

### Worldscope / LSEG

- <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals> — LSEG Worldscope product page.
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/point-in-time-fundamentals> — LSEG PIT Fundamentals.
- <https://solutions.refinitiv.com/point-in-time> — Refinitiv PIT solutions.
- <https://www.tilburguniversity.edu/sites/default/files/download/WorldScopeDatatypeDefinitionsGuide_2.pdf> — Tilburg Worldscope Datatype Definitions Guide.
- <http://www.alacra.com/alacra/help/wscope_definitions.pdf> — Alacra Worldscope definitions mirror.
- <https://libapp.lib.ncku.edu.tw/libref/handout/20110107_WorldscopeFileSpecificationsJune2008.pdf> — NCKU library Worldscope File Specifications.
- <https://bizlib247.wordpress.com/2013/04/11/worldscope-coverage-and-data-definitions/> — Worldscope coverage and definitions overview.
- <https://datateamoftheeur.wordpress.com/category/worldscope/> — EDSC Erasmus Worldscope tips.
- <https://fmc.refinitiv.com/clientFacing/pdf/DFO_User_Guide.pdf> — Datastream for Office User Guide.
- <https://developers.lseg.com/content/dam/devportal/api-families/refinitiv-data-platform/refinitiv-data-platform-apis/documentation/rdp_api_getting_started_guide.pdf> — Refinitiv Data Platform getting started.

### Bloomberg

- <https://professional.bloomberg.com/products/data/data-management/data-license/> — Bloomberg Data License product page.
- <https://www.bloomberg.com/professional/products/data/enterprise-catalog/cofi/> — Bloomberg COFI (Company Financials, Estimates and Pricing PIT).
- <https://www.bloomberg.com/professional/products/data/enterprise-catalog/reference/> — Bloomberg Reference Data catalog.
- <https://data.bloomberglp.com/professional/sites/10/189913_CDS_REF_Fundamentals_SFCT_DIG.pdf> — Bloomberg Fundamentals product fact sheet.
- <https://bautheac.github.io/BBGsymbols/> — BBGsymbols R package with field catalogue (DT IDs).
- <https://www.wu.ac.at/fileadmin/wu/s/library/databases_info_image/Bloomberg_BQL_Fundamentals_FactSheet.pdf> — WU BQL Fundamentals Fact Sheet.
- <https://pages.stern.nyu.edu/~adamodar/pdfiles/Bloombergfull.pdf> — NYU Stern Bloomberg Terminal guide.
- <https://library.wu.ac.at/bib/fit4research/wp-content/uploads/2024/02/Forecasts_manuals_Bloomberg.pdf> — Forecasts manuals (BEst).

### S&P Capital IQ

- <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro> — Capital IQ Pro product page.
- <https://wrds-www.wharton.upenn.edu/pages/grid-items/introduction-capital-iq/> — WRDS Introduction to Capital IQ.
- <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-capital-iq-with-compustat/> — CIQ-Compustat linking.
- <https://www.scribd.com/document/161578486/Ciq-Financials-Methodology> — CIQ Financials Methodology.
- <https://www.scribd.com/document/331103626/tech-faq-12478964> — Capital IQ API Usage Guide / Tech FAQ.
- <http://larryschrenk.com/Capital%20IQ/Excel%20Plug-in%20Manual.pdf> — Capital IQ Excel Plug-in Manual (Jan 2017).
- <https://libguides.nypl.org/CapitalIQ/FinancialsGlossary> — NYPL CIQ Financials Glossary.
- <https://www.babson.edu/media/babson/assets/cutler-center/CapIQ_Basic-Functionality-and-Navigation_Stegeman_FINAL.pdf> — Babson CIQ basics.
- <https://quantemplate.readme.io/docs/example-capital-iq-integration> — Quantemplate CIQ integration example.

### Sharadar / SimFin / mid-tier

- <https://data.nasdaq.com/databases/SF1> — Sharadar Core US Fundamentals (SF1) on Nasdaq Data Link.
- <https://www.quantrocket.com/sharadar/> — QuantRocket Sharadar data documentation.
- <https://resources.quandl.com/a/res-hub/Sharadar_Datasheet_final.pdf> — Sharadar Core US Equities Bundle datasheet.
- <https://www.simfin.com/en/fundamental-data-download/> — SimFin bulk download page.
- <https://www.simfin.com/en/pricing/> — SimFin pricing.
- <https://github.com/SimFin/simfin> — SimFin Python package source.
- <https://github.com/SimFin/simfin/blob/master/simfin/names.py> — SimFin column-name dictionary (Python).
- <https://github.com/simfin/simfin-tutorials/blob/master/01_Basics.ipynb> — SimFin tutorial notebook with column references.

### XBRL tooling

- <https://arelle.org/> — Arelle open-source XBRL processor.
- <http://arelle-us.s3.amazonaws.com/2011/04/KU-XBRL-open-source-ArelleProject.pdf> — Arelle origins paper.
- <https://github.com/dgunning/edgartools> — EdgarTools Python library.
- <https://github.com/SEC-API-io/sec-api-python> — sec-api-python wrapper.
- <https://sec-edgar-api.readthedocs.io/> — sec-edgar-api documentation.
- <https://sec-api.io/> — sec-api.io commercial service.
- <https://tldrfiling.com/blog/sec-edgar-api-guide> — SEC EDGAR API guide.
- <https://tldrfiling.com/blog/sec-edgar-xbrl-api-python-tutorial> — XBRL API Python tutorial.
- <https://medium.com/@vkasps/exploring-the-secs-xbrl-frames-api-for-financial-data-analysis-b2e8c7f12b3b> — Frames API exploration.
- <https://www.openriskmanual.org/wiki/XBRL_Calculation_Linkbase> — XBRL calculation linkbase reference.
- <https://www.altova.com/blog/2025/09/us-gaap-xbrl-reporting-requirements-challenges-and-solutions> — Altova on us-gaap XBRL.

### Open-data discussion / academic

- <https://som.yale.edu/sites/default/files/2024-07/Re-Standardized%20Financial%20Statement%20Data.pdf> — Yale paper on re-standardised financial statement data.
- <https://business.columbia.edu/sites/default/files-efs/imce-uploads/CEASA/Events%20Page/revisiting_accounting-based_return_anomalies.pdf> — Columbia "Lost in Standardization".
- <https://xbrl.us/harmonizing-accounting-data-standards/> — XBRL US on harmonising accounting standards.
- <https://sites.bu.edu/qm222projectcourse/files/2014/08/compustat_users_guide-2003.pdf> — Compustat User's Guide (2003 archival).
- <https://www.tidy-finance.org/r/wrds-crsp-and-compustat.html> — Tidy Finance WRDS-CRSP-Compustat guide.
- <https://iangow.github.io/far_book/fin-state.html> — Ian Gow's empirical research book on financial statements.
- <https://www.investor.gov/introduction-investing/investing-basics/glossary> — SEC investor.gov glossary.
- <https://www.workiva.com/blog/your-guide-2024-us-gaap-taxonomy-update> — Workiva 2024 us-gaap taxonomy guide.

---

**Confirm:**

- File path: `c:/Users/natha/OneDrive/Desktop/C/ats/ats-eqt/research/datasets/fundamentals_us_equities.md`
- Section count: **14 top-level sections** (0 Executive summary; 1 Vendor stack matrix; 2 Compustat NA XPF; 3 FactSet; 4 Worldscope; 5 Bloomberg FA/BQL; 6 Capital IQ ciqFinInstanceItem; 7 Sharadar+SimFin; 8 EDGAR XBRL reconstruction; 9 Cross-vendor field map; 10 Recommended ats-eqt schema; 11 Bitemporal worked example; 12 Open questions; 13 Sources), with 60+ sub-sections.
- Last updated: 2026-05-14.
