# Dataset `compustat_std` (fundq / funda, Compustat-shaped, point in time)

Code `src/atx_db/alpha_panel/compustat_std.py` (rule `cstd-pit-v2`, schema `atx.alpha-panel.compustat-std/v1`);
registration stub `docs/datasets/compustat_std.registry.json`. Tier1-v4 task 3 (gap analysis section 2 item 2).
Built from the fundamentals stage (`--fund-stage`, default `fundamentals_v10`; task 5 rebuilds it on
`fundamentals_v11`) and `earnings_calendar`; no new source.

## Files and keys

| file | rows (build 2026-10-03) | bytes | keys | content |
| --- | --- | --- | --- | --- |
| `compustat_std/fundq.parquet` | 747,073 (17,010 CIKs) | 70,163,590 | (cik, datadate, fyearq, fqtr, accession) | filing and restatement vintages |
| `compustat_std/funda.parquet` | 235,563 (15,875 CIKs) | 45,917,402 | (cik, datadate, fyear, accession) | annual-form base periods and their restatement vintages: annual items + 81 catalog fields |
| `compustat_std/validation.json` | | | | gate measurements (coverage, rdq, sum4, balance, fsds, cutoff, handcheck) |
| `compustat_std/manifest.json` | | | | schema, code SHA-256, file SHA-256, input manifest SHA-256, counts, `gate_status` |

datadate 2004-12-31 .. 2026-08-31; available_at 2010-01-05 19:00:00 .. 2026-09-19 22:00:00 UTC.
406,166 first-reported rows, 325,843 restatement rows, 7,267 FX-converted rows, 29 rows without a fiscal
quarter (no annual FY row and no usable Q/FY label). `(cik, datadate, accession)` is unique in each file.

## Point-in-time rule

* `available_at` (TIMESTAMP, naive UTC) is the clock of every column of a row: the fundamentals event's
  `available_at` (EDGAR acceptance, or `filed` + 46 h when FSDS lacks the accession; FX-converted filers: the latest
  H.10 rate publication used). A row is visible at session d only if `available_at` < 22:00 UTC of session d-1.
* Every derived value uses only inputs with a clock at or before the row's `available_at`: the fiscal-year-end
  month from FY rows already received, quarterly-history vintages, Company Facts facts at their filing clock,
  8-K 2.02 announcements already accepted, H.10 rates already published. The `cutoff` gate rebuilds the stage from
  the inputs visible before five cutoffs and compares it cell by cell with the published rows.
* Vintages are new rows: amendments and later restatements of older datadates add rows; nothing is overwritten.
  `vintage_kind` (VARCHAR) distinguishes `filing` from `restatement`. Funda requires an annual-form base period
  with fqtr 4. A later 10-Q can restate that annual period while retaining its annual values and base calendar;
  its own current quarterly TTM values never become a funda base row. `first_reported` marks
  the earliest vintage of a (cik, datadate); `is_restated` / `restated_items` (fundamentals S4.6) mark filings that
  changed a previously reported value by more than 0.5%.
* As-of macros (serving catalog; `<root>` = the alpha-panel build root):

```sql
CREATE OR REPLACE MACRO fundq_asof(ts) AS TABLE SELECT * FROM read_parquet('<root>/compustat_std/fundq.parquet')
  WHERE available_at < CAST(ts AS TIMESTAMP)
  QUALIFY row_number() OVER (PARTITION BY cik, datadate ORDER BY available_at DESC, accession DESC) = 1;
CREATE OR REPLACE MACRO funda_asof(ts) AS TABLE SELECT * FROM read_parquet('<root>/compustat_std/funda.parquet')
  WHERE available_at < CAST(ts AS TIMESTAMP)
  QUALIFY row_number() OVER (PARTITION BY cik, datadate ORDER BY available_at DESC, accession DESC) = 1;
```

  `compustat_std.macros_sql(stage_dir)` returns the same statements; the stagelake vintage `<view>_as_of` macro with
  keys (cik, datadate) and order (available_at, accession) is identical. The issuer's latest state at t:
  `SELECT * FROM fundq_asof(t) QUALIFY row_number() OVER (PARTITION BY cik ORDER BY datadate DESC) = 1`.
* Staleness: a row describes datadate as known at `available_at`; consumers bound it by session - datadate
  (fundamentals convention: <= 400 days) or by the next expected filing. `rdq` is informational, never a clock.
* Window: rows with datadate outside [1990-01-01, build date + 365 days] are excluded and counted
  (`excluded_outside_datadate_window`): events 0, quarterly_history 108 (period
  ends before 1990), Company Facts EPS/IB/DO/share facts 0. The raw Company Facts extract holds
  1,058 facts dated after 2027-10-02 (up to 6016-06-30) and 663 before 1990; none reaches the fundamentals events.

## Fiscal calendar

* effective month of a date = its month, or the previous month when the day is <= 7 (52/53-week years such as
  Apple's 2022-10-01);
* `fyr` = effective month of the latest FY-labelled annual-form datadate visible at the row, else inferred from
  the row's own label (Qk / annual FY): effective month + 3 (4 - k). A 10-Q labelled FY cannot set/reset fyr.
  Restatement vintages retain their base period's calendar.
* `fqtr` = ((effective month - fyr - 1) mod 12) div 3 + 1 (dates win over the DEI label: 1,922 of
  308,729 Q1-Q3-labelled rows differ, mostly fiscal-year changes and mislabelled filings);
* `fyearq` / `fyear` = calendar year of the fiscal year end the quarter belongs to, minus 1 when `fyr` < 6
  (Compustat: Walmart's year ending 2024-01-31 is fyear 2023).

## Descriptor columns

fundq; funda has the same columns with `fyear` for `fyearq`, no `fqtr`, `fx_rate_avg_ttm` for `fx_rate_avg_q` and no
`*_src`. Clock of every column: `available_at`.

| column | type | definition | source |
| --- | --- | --- | --- |
| `cik` | BIGINT | SEC CIK | events |
| `datadate` | DATE | statement period end of the vintage (actual date; Compustat rounds 52/53-week ends to the month end) | events `period_end` |
| `fyearq` | BIGINT | Compustat fiscal year (fiscal calendar above) | derived |
| `fqtr` | BIGINT | fiscal quarter 1-4 | derived |
| `fyr` | BIGINT | fiscal-year-end month | derived |
| `accession` | VARCHAR | EDGAR accession of the filing that created the vintage | events |
| `form` | VARCHAR | 10-Q, 10-K, 10-QT, 10-KT, 20-F, 40-F (+ /A) | events |
| `filed` | DATE | EDGAR filing date | events |
| `clock_utc` | TIMESTAMP | filing clock (acceptance, UTC) | events |
| `available_at` | TIMESTAMP | row clock (PIT rule above) | events |
| `rdq` | DATE | report date of quarterly earnings: earliest of the ET date of a primary 8-K item 2.02 matched to datadate (fiscal_period_end within 7 days, accepted by available_at) and the first vintage's filing date (ET acceptance date; `filed` for the modeled fc1 clock) | earnings_calendar, events |
| `rdq_src` | VARCHAR | `8k_202`, or `filing` (no 8-K before the filing: the 10-Q/10-K was the first release) | derived |
| `curcd` | VARCHAR | reporting currency (money items are USD; FX-converted when `fx_converted`) | events `currency` |
| `fx_converted` | BOOLEAN | money items converted from `curcd` at H.10 rates | events |
| `fx_rate`, `fx_rate_avg_q`, `fx_rate_avg_ttm` | DOUBLE | USD per unit of `curcd`: datadate spot, 91-day and 365-day means | events |
| `first_reported` | BOOLEAN | earliest vintage of (cik, datadate) | derived |
| `vintage_kind` | VARCHAR | `filing` for the current period, `restatement` for an older period changed by a later filing | derived |
| `is_restated`, `restated_items` | BOOLEAN, VARCHAR | filing rows inherit upstream >0.5% watched-chain markers; restatement rows are true and list changed items | events (S4.6), history deltas |
| `is_amendment` | BOOLEAN | form ends with /A | events |
| `nonreliance_402_at` | TIMESTAMP | latest 8-K item 4.02 within 365 days | events |
| `fiscal_year`, `fiscal_period` | INTEGER, VARCHAR | SEC DEI fiscal year / period labels of datadate | events |
| `fin_template`, `sic` | VARCHAR, INTEGER | financial template (bank, insurer, reit, utility, other) and SIC in force; industry items are structurally NaN outside their SIC ranges | events |
| `zero_filled` | VARCHAR | fundamentals-stage item names zero-filled by its zero rules (events and catalog lists) | events, catalog |
| `ibq_src`, `epspxq_src` | VARCHAR | derivation path of `ibq` / `epspxq` (below) | derived |

## fundq items

DOUBLE; money USD, per-share USD per share, shares in shares. Clock `available_at`; staleness as above.

| column | type | unit | definition | source |
| --- | --- | --- | --- | --- |
| `saleq` | DOUBLE | USD | Sales/turnover (net), discrete quarter (fundamentals sale_q: revenue chain; banks interest + noninterest income; Q4: 3-month fact, else FY - 9M) | events.parquet `sale_q` |
| `cogsq` | DOUBLE | USD | Cost of goods sold, discrete quarter | events.parquet `cogs_q` |
| `xsgaq` | DOUBLE | USD | Selling, general and administrative expense, discrete quarter | events.parquet `xsga_q` |
| `oiadpq` | DOUBLE | USD | Operating income after depreciation, discrete quarter | events.parquet `oi_q` |
| `oibdpq` | DOUBLE | USD | Operating income before depreciation = oiadpq + dpq | events.parquet `ebitda_q` |
| `dpq` | DOUBLE | USD | Depreciation and amortization, discrete quarter | events.parquet `dp_q` |
| `xintq` | DOUBLE | USD | Interest expense, discrete quarter | events.parquet `xint_q` |
| `txtq` | DOUBLE | USD | Income taxes - total, discrete quarter (0 when the income statement shows no tax line) | events.parquet `txt_q` |
| `niq` | DOUBLE | USD | Net income (loss) attributable to the parent, discrete quarter | events.parquet `ni_q` |
| `cshoq` | DOUBLE | shares | Common shares outstanding (dei cover count dated end-15d..end+120d, else balance-sheet shares at datadate, else weighted-average shares) | events.parquet `shrs_q` |
| `atq` | DOUBLE | USD | Assets - total | events.parquet `at` |
| `ltq` | DOUBLE | USD | Liabilities - total (Liabilities, else assets - equity incl. NCI) | events.parquet `lt` |
| `ceqq` | DOUBLE | USD | Common/ordinary equity - total = seqq - pstkq | catalog.parquet `ceq` |
| `seqq` | DOUBLE | USD | Stockholders' equity - parent | events.parquet `seq` |
| `teqq` | DOUBLE | USD | Stockholders' equity - total incl. noncontrolling interest (reported, else seqq + mibq) | catalog.parquet `teq` |
| `cheq` | DOUBLE | USD | Cash and short-term investments | events.parquet `che` |
| `actq` | DOUBLE | USD | Current assets - total | events.parquet `act` |
| `lctq` | DOUBLE | USD | Current liabilities - total | events.parquet `lct` |
| `invtq` | DOUBLE | USD | Inventories - total | events.parquet `invt` |
| `rectq` | DOUBLE | USD | Receivables - total | events.parquet `rect` |
| `apq` | DOUBLE | USD | Accounts payable - trade | events.parquet `ap` |
| `ppentq` | DOUBLE | USD | Property, plant and equipment - total (net) | events.parquet `ppe` |
| `dlcq` | DOUBLE | USD | Debt in current liabilities | catalog.parquet `dlc` |
| `dlttq` | DOUBLE | USD | Long-term debt - total (excluding current maturities) | catalog.parquet `dltt` |
| `pstkq` | DOUBLE | USD | Preferred/preference stock (capital) - total (0 when unreported and assets exist) | events.parquet `pstk` |
| `mibq` | DOUBLE | USD | Noncontrolling interest (balance sheet) | events.parquet `mib` |
| `gdwlq` | DOUBLE | USD | Goodwill | events.parquet `gdwl` |
| `intanq` | DOUBLE | USD | Intangible assets - total incl. goodwill (fundamentals catalog column intano = gdwl + intan) | catalog.parquet `intano` |
| `ibq` | DOUBLE | USD | Income before extraordinary items and discontinued operations, discrete quarter (see ibq_src) | derived (Company Facts extract `_work/cf`, events, catalog) |
| `epspxq` | DOUBLE | USD/share | EPS (basic) excluding extraordinary items and discontinued operations, quarter (see epspxq_src) | derived (Company Facts extract `_work/cf`, events, catalog) |
| `epsfxq` | DOUBLE | USD/share | EPS (diluted) excluding extraordinary items and discontinued operations, quarter | derived (Company Facts extract `_work/cf`, events, catalog) |
| `cshprq` | DOUBLE | shares | Common shares used to calculate basic EPS (weighted average of the quarter) | derived (Company Facts extract `_work/cf`, events, catalog) |
| `oancfy` | DOUBLE | USD | Operating activities - net cash flow, fiscal year to date | fqtr 4: events `cfo_ttm`; fqtr 1-3: sum of quarterly_history `cfo_q` |
| `capxy` | DOUBLE | USD | Capital expenditures, fiscal year to date | fqtr 4: events `capx_ttm`; fqtr 1-3: sum of quarterly_history `capx_q` |
| `dvy` | DOUBLE | USD | Cash dividends paid on common stock (fundamentals dvc chain; preferred excluded), fiscal year to date | fqtr 4: events `dvc_ttm`; fqtr 1-3: sum of quarterly_history `dvc_q` |
| `prstkcy` | DOUBLE | USD | Purchase of common and preferred stock, fiscal year to date | fqtr 4: events `prstkc_ttm`; fqtr 1-3: sum of quarterly_history `prstkc_q` |
| `sstky` | DOUBLE | USD | Sale of common and preferred stock, fiscal year to date | fqtr 4: events `sstk_ttm`; fqtr 1-3: sum of quarterly_history `sstk_q` |
| `xrdy` | DOUBLE | USD | Research and development expense, fiscal year to date | fqtr 4: events `xrd_ttm`; fqtr 1-3: sum of quarterly_history `xrd_q` |



Derivations: `ibq` = 3-month fact of the catalog `ib` chain (IncomeLossFromContinuingOperations, ...) at datadate
(`ib_3m`); at fqtr 4 FY minus nine-month facts of one concept (`ib_fy_less_9m`); else `niq - doq` (`niq_no_do` when
doq = 0, `niq_less_doq`), doq = 3-month (fqtr 4: FY - 9M) discontinued-operations fact, else 0 when the catalog DO TTM
is 0 or no non-zero DO fact of any duration ends at datadate. `epspxq`/`epsfxq` = continuing-operations EPS
(3-month, else fqtr 4 FY - 9M), total EPS only when doq = 0 (Compustat EPSPX excludes discontinued operations).
`cshprq` = 3-month weighted-average basic shares, fqtr 4 else 4 x FY - 3 x 9M. Facts come from
`<fund stage>/_work/cf` at their accession's clock (concept priority first, then the latest clock); FX-converted rows
multiply money and per-share facts by `fx_rate_avg_q`. YTD items: fqtr 4 = the event TTM (the fiscal-year fact at a
10-K); fqtr 1-3 = sum of the fiscal year's discrete quarters (quarter ends 3 j months before datadate, latest
quarterly_history vintage <= available_at), NULL when a quarter is missing, 0 when the TTM is 0; FX rows convert each
quarter at its own 91-day H.10 mean (current quarter: `fx_rate_avg_q`).

Derivation paths over all rows: ibq {'ib_3m': 110166, 'ib_fy_less_9m': 25958, 'niq_less_doq': 2285, 'niq_no_do': 582763, 'null': 25901}; epspxq {'cont_3m': 92087, 'cont_fy_less_9m': 18160, 'null': 111372, 'total_3m': 426994, 'total_fy_less_9m': 98460};
rdq {'8k_202': 420211, 'filing': 326862}.

## funda items

| column | type | unit | definition | source |
| --- | --- | --- | --- | --- |
| `sale` | DOUBLE | USD | Sales/turnover (net), fiscal year | events.parquet `sale_ttm` |
| `cogs` | DOUBLE | USD | Cost of goods sold, fiscal year | events.parquet `cogs_ttm` |
| `xsga` | DOUBLE | USD | Selling, general and administrative expense, fiscal year | events.parquet `xsga_ttm` |
| `gp` | DOUBLE | USD | Gross profit (loss), fiscal year | events.parquet `gp_ttm` |
| `oiadp` | DOUBLE | USD | Operating income after depreciation, fiscal year | events.parquet `oi_ttm` |
| `oibdp` | DOUBLE | USD | Operating income before depreciation = oiadp + dp | events.parquet `ebitda_ttm` |
| `dp` | DOUBLE | USD | Depreciation and amortization, fiscal year | events.parquet `dp_ttm` |
| `xint` | DOUBLE | USD | Interest expense, fiscal year | events.parquet `xint_ttm` |
| `ni` | DOUBLE | USD | Net income (loss) attributable to the parent, fiscal year | events.parquet `ni_ttm` |
| `csho` | DOUBLE | shares | Common shares outstanding (as cshoq) | events.parquet `shrs_q` |
| `at` | DOUBLE | USD | Assets - total | events.parquet `at` |
| `lt` | DOUBLE | USD | Liabilities - total | events.parquet `lt` |
| `seq` | DOUBLE | USD | Stockholders' equity - parent | events.parquet `seq` |
| `che` | DOUBLE | USD | Cash and short-term investments | events.parquet `che` |
| `act` | DOUBLE | USD | Current assets - total | events.parquet `act` |
| `lct` | DOUBLE | USD | Current liabilities - total | events.parquet `lct` |
| `invt` | DOUBLE | USD | Inventories - total | events.parquet `invt` |
| `rect` | DOUBLE | USD | Receivables - total | events.parquet `rect` |
| `ap` | DOUBLE | USD | Accounts payable - trade | events.parquet `ap` |
| `ppent` | DOUBLE | USD | Property, plant and equipment - total (net) | events.parquet `ppe` |
| `ppegt` | DOUBLE | USD | Property, plant and equipment - total (gross) | events.parquet `ppegt` |
| `pstk` | DOUBLE | USD | Preferred/preference stock (capital) - total | events.parquet `pstk` |
| `mib` | DOUBLE | USD | Noncontrolling interest (balance sheet) | events.parquet `mib` |
| `gdwl` | DOUBLE | USD | Goodwill | events.parquet `gdwl` |
| `intano` | DOUBLE | USD | Other intangibles (excluding goodwill; fundamentals intan) | events.parquet `intan` |
| `oancf` | DOUBLE | USD | Operating activities - net cash flow, fiscal year | events.parquet `cfo_ttm` |
| `capx` | DOUBLE | USD | Capital expenditures, fiscal year | events.parquet `capx_ttm` |
| `dvc` | DOUBLE | USD | Cash dividends paid on common stock, fiscal year | events.parquet `dvc_ttm` |
| `dvt` | DOUBLE | USD | Dividends - total (declared), fiscal year | events.parquet `dvt_ttm` |
| `prstkc` | DOUBLE | USD | Purchase of common and preferred stock, fiscal year | events.parquet `prstkc_ttm` |
| `sstk` | DOUBLE | USD | Sale of common and preferred stock, fiscal year | events.parquet `sstk_ttm` |
| `xrd` | DOUBLE | USD | Research and development expense, fiscal year | events.parquet `xrd_ttm` |

Catalog items (fundamentals `catalog.parquet`) under their Compustat names (lower-case first mnemonic token).
Columns are renamed where the catalog label contradicts Compustat semantics: catalog `intano` = gdwl + intan =
Compustat INTAN; catalog `epspx_ttm`/`epsfx_ttm` = total EPS = Compustat EPSPI/EPSFI; catalog `epspi_ttm`/`epsfi_ttm`
= continuing-operations EPS = Compustat EPSPX/EPSFX. `tempeq` carries upstream `mibt` (temporary/mezzanine
equity); `nicon` carries upstream `nicon_ttm` (income available to common, basic). These two extensions have no
verified Compustat equivalent; neither MIBT nor NIADJ is claimed. TTM flows at an annual base filing are fiscal-year values.

| column | catalog column | unit | definition |
| --- | --- | --- | --- |
| `ch` | `ch` | USD | catalog ch (mnemonic CH, stock), at datadate |
| `ivst` | `ivst` | USD | catalog ivst (mnemonic IVST, stock), at datadate |
| `xpp` | `xpp` | USD | catalog xpp (mnemonic XPP, stock), at datadate |
| `aco` | `aco` | USD | catalog aco (mnemonic ACO, stock), at datadate |
| `dpact` | `dpact` | USD | catalog dpact (mnemonic DPACT, stock), at datadate |
| `intan` | `intano` | USD | catalog intano (mnemonic INTANO, derived), at datadate; gdwl + intan (after their zero fills), else IntangibleAssetsNetIncludingGoodwill; published as Compustat INTAN (catalog label INTANO differs) |
| `rouant` | `rou` | USD | catalog rou (mnemonic ROUANT, stock), at datadate |
| `ivao` | `ivao` | USD | catalog ivao (mnemonic IVAO, stock), at datadate |
| `txdba` | `txdba` | USD | catalog txdba (mnemonic TXDBA, stock), at datadate |
| `ao` | `ao` | USD | catalog ao (mnemonic AO, stock), at datadate |
| `xacc` | `xacc` | USD | catalog xacc (mnemonic XACC, stock), at datadate |
| `dlc` | `dlc` | USD | catalog dlc (mnemonic DLC, derived), at datadate; debt in current liabilities as in debt: DebtCurrent, else current LTD + short-term borrowings; 0 when at exists and neither is reported |
| `dd1` | `dd1` | USD | catalog dd1 (mnemonic DD1, stock), at datadate |
| `dltt` | `dltt` | USD | catalog dltt (mnemonic DLTT, derived), at datadate; long-term debt excluding current maturities as in debt (0 when at exists and no long-term debt concept is reported) |
| `llo` | `llo` | USD | catalog llo (mnemonic LLO, stock), at datadate |
| `txditc` | `txditc` | USD | catalog txditc (mnemonic TXDITC, derived), at datadate; be's deferred-tax component: DeferredIncomeTaxLiabilitiesNet, else DeferredTaxLiabilitiesNoncurrent, carried 400 days, else 0 when at exists |
| `lo` | `lo` | USD | catalog lo (mnemonic LO, stock), at datadate |
| `cstk` | `cstk` | USD | catalog cstk (mnemonic CSTK, stock), at datadate |
| `caps` | `caps` | USD | catalog caps (mnemonic CAPS, stock), at datadate |
| `re` | `re` | USD | catalog re (mnemonic RE, stock), at datadate |
| `acominc` | `acominc` | USD | catalog acominc (mnemonic ACOMINC, stock), at datadate |
| `tstk` | `tstk` | USD | catalog tstk (mnemonic TSTK, stock), at datadate |
| `ceq` | `ceq` | USD | catalog ceq (mnemonic CEQ, derived), at datadate; seq - pstk (pstk 0 when unreported) |
| `teq` | `teq` | USD | catalog teq (mnemonic TEQ, derived), at datadate; StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest, else seq + mib |
| `lse` | `lse` | USD | catalog lse (mnemonic LSE, stock), at datadate; else at (the balance sheet identity) |
| `tempeq` | `mibt` | USD | temporary/mezzanine equity; non-Compustat extension (upstream MIBT/TEMP label is misleading) |
| `txp` | `txp` | USD | catalog txp (mnemonic TXP, stock), at datadate |
| `lco` | `lco` | USD | catalog lco (mnemonic LCO, derived), at datadate; lct - ap - dlc - txp (Compustat's LCT = AP + DLC + TXP + LCO; txp 0 when missing), else the chain; structural where lct is |
| `wcap` | `wcap` | USD | catalog wcap (mnemonic WCAP, derived), at datadate; act - lct (structural where act/lct are) |
| `xopr` | `xopr_ttm` | USD | catalog xopr_ttm (mnemonic XOPR, derived), fiscal year; sale_ttm - (oi_ttm + dp_ttm) (Compustat: OIBDP = SALE - XOPR) |
| `xsell` | `xsell_ttm` | USD | catalog xsell_ttm (mnemonic XSELL, flow), fiscal year |
| `xad` | `xad_ttm` | USD | catalog xad_ttm (mnemonic XAD, flow), fiscal year |
| `am` | `am_ttm` | USD | catalog am_ttm (mnemonic AM, flow), fiscal year |
| `idit` | `idit_ttm` | USD | catalog idit_ttm (mnemonic IDIT, flow), fiscal year |
| `nopi` | `nopi_ttm` | USD | catalog nopi_ttm (mnemonic NOPI, flow), fiscal year |
| `spi` | `spi_ttm` | USD | catalog spi_ttm (mnemonic SPI, flow), fiscal year; first reported concept of the chain (not the sum of all special items) |
| `pi` | `pi_ttm` | USD | catalog pi_ttm (mnemonic PI, flow), fiscal year; else ni_ttm + txt_ttm |
| `txt` | `txt_ttm` | USD | catalog txt_ttm (mnemonic TXT, derived), fiscal year; income-tax chain TTM; 0 (catalog_zero_filled) when the income statement has no tax line and no tax fact within 460 days |
| `txc` | `txc_ttm` | USD | catalog txc_ttm (mnemonic TXC, flow), fiscal year |
| `txdi` | `txdi_ttm` | USD | catalog txdi_ttm (mnemonic TXDI, flow), fiscal year |
| `mii` | `mii_ttm` | USD | catalog mii_ttm (mnemonic MII, flow), fiscal year |
| `esub` | `esub_ttm` | USD | catalog esub_ttm (mnemonic ESUB, flow), fiscal year |
| `do` | `do_ttm` | USD | catalog do_ttm (mnemonic DO, flow), fiscal year |
| `ib` | `ib_ttm` | USD | catalog ib_ttm (mnemonic IB, flow), fiscal year; else ni_ttm - do_ttm |
| `xido` | `xido_ttm` | USD | catalog xido_ttm (mnemonic XIDO, flow), fiscal year |
| `nicon` | `nicon_ttm` | USD | net income available to common (basic); non-Compustat extension, not verified NIADJ (diluted numerator) |
| `dvp` | `dvp_ttm` | USD | catalog dvp_ttm (mnemonic DVP, flow), fiscal year |
| `epspi` | `epspx_ttm` | USD/share | catalog epspx_ttm (mnemonic EPSPX, per_share), fiscal year; published as Compustat EPSPI (catalog label EPSPX differs) |
| `epsfi` | `epsfx_ttm` | USD/share | catalog epsfx_ttm (mnemonic EPSFX, per_share), fiscal year; published as Compustat EPSFI (catalog label EPSFX differs) |
| `epspx` | `epspi_ttm` | USD/share | catalog epspi_ttm (mnemonic EPSPI, per_share), fiscal year; else epspx when do_ttm = 0; published as Compustat EPSPX (catalog label EPSPI differs) |
| `epsfx` | `epsfi_ttm` | USD/share | catalog epsfi_ttm (mnemonic EPSFI, per_share), fiscal year; else epsfx when do_ttm = 0; published as Compustat EPSFX (catalog label EPSFI differs) |
| `cshpri` | `cshpri` | shares | catalog cshpri (mnemonic CSHPRI, avg_shares), at datadate |
| `cshfd` | `cshfd` | shares | catalog cshfd (mnemonic CSHFD, avg_shares), at datadate |
| `cshi` | `cshi` | shares | catalog cshi (mnemonic CSHI, stock), at datadate; shares issued (csho is shrs_q) |
| `citotal` | `ci_ttm` | USD | catalog ci_ttm (mnemonic CITOTAL, flow), fiscal year |
| `ivncf` | `ivncf_ttm` | USD | catalog ivncf_ttm (mnemonic IVNCF, flow), fiscal year |
| `fincf` | `fincf_ttm` | USD | catalog fincf_ttm (mnemonic FINCF, flow), fiscal year |
| `capxint` | `capxint_ttm` | USD | catalog capxint_ttm (mnemonic CAPXINT, flow), fiscal year |
| `stkco` | `stkco_ttm` | USD | catalog stkco_ttm (mnemonic STKCO, flow), fiscal year |
| `aqc` | `aqc_ttm` | USD | catalog aqc_ttm (mnemonic AQC, flow), fiscal year |
| `sppiv` | `sppiv_ttm` | USD | catalog sppiv_ttm (mnemonic SPPIV, flow), fiscal year |
| `dltis` | `dltis_ttm` | USD | catalog dltis_ttm (mnemonic DLTIS, flow), fiscal year |
| `dltr` | `dltr_ttm` | USD | catalog dltr_ttm (mnemonic DLTR, flow), fiscal year |
| `dlcch` | `dlcch_ttm` | USD | catalog dlcch_ttm (mnemonic DLCCH, flow), fiscal year |
| `dvpd` | `dvpd_ttm` | USD | catalog dvpd_ttm (mnemonic DVPD, flow), fiscal year |
| `dv` | `dv_ttm` | USD | catalog dv_ttm (mnemonic DV, derived), fiscal year; PaymentsOfDividends TTM, else dvc_ttm + dvpd_ttm |
| `recch` | `recch_ttm` | USD | catalog recch_ttm (mnemonic RECCH, flow), fiscal year |
| `invch` | `invch_ttm` | USD | catalog invch_ttm (mnemonic INVCH, flow), fiscal year |
| `apalch` | `apalch_ttm` | USD | catalog apalch_ttm (mnemonic APALCH, flow), fiscal year |
| `exre` | `exre_ttm` | USD | catalog exre_ttm (mnemonic EXRE, flow), fiscal year |
| `chech` | `chech_ttm` | USD | catalog chech_ttm (mnemonic CHECH, flow), fiscal year |
| `txdc` | `txdc_ttm` | USD | catalog txdc_ttm (mnemonic TXDC, flow), fiscal year |
| `txpd` | `txpd_ttm` | USD | catalog txpd_ttm (mnemonic TXPD, flow), fiscal year |
| `intpn` | `intpn_ttm` | USD | catalog intpn_ttm (mnemonic INTPN, flow), fiscal year |
| `niint` | `niint_ttm` | USD | catalog niint_ttm (mnemonic NIINT, flow), fiscal year; bank net interest income |
| `iditbk` | `idit_bank_ttm` | USD | catalog idit_bank_ttm (mnemonic IDITBK, flow), fiscal year |
| `pcl` | `pcl_ttm` | USD | catalog pcl_ttm (mnemonic PCL, flow), fiscal year |
| `lntal` | `lntal` | USD | catalog lntal (mnemonic LNTAL, stock), at datadate |
| `dptc` | `dptc` | USD | catalog dptc (mnemonic DPTC, stock), at datadate |
| `prem` | `prem_ttm` | USD | catalog prem_ttm (mnemonic PREM, flow), fiscal year |
| `benef` | `benef_ttm` | USD | catalog benef_ttm (mnemonic BENEF, flow), fiscal year |

## Measured coverage

Basis: first-reported fundq rows, datadate in the year, 10-Q/10-K-type forms (20-F/40-F annual-only rows excluded),
CIK linked to a `member_equity` cell that year (`identity/link_table_v3`). Command: `compustat_std validate`.

| year | issuer_quarters | saleq | ibq | atq | ceqq | cheq | cshoq | oancfy | capxy | niq | epspxq | epsfxq | cshprq | dvy | xrdy | teqq | dlttq |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2020 | 10,077 | 0.965 | 0.997 | 0.998 | 0.995 | 0.993 | 0.997 | 0.980 | 0.944 | 0.998 | 0.951 | 0.944 | 0.943 | 0.947 | 0.924 | 0.995 | 0.998 |
| 2021 | 10,929 | 0.967 | 0.996 | 0.996 | 0.994 | 0.993 | 0.997 | 0.977 | 0.932 | 0.997 | 0.936 | 0.931 | 0.936 | 0.918 | 0.924 | 0.994 | 0.997 |
| 2022 | 10,068 | 0.971 | 0.997 | 0.997 | 0.994 | 0.992 | 0.998 | 0.991 | 0.956 | 0.998 | 0.953 | 0.942 | 0.951 | 0.958 | 0.936 | 0.994 | 0.997 |
| 2023 | 9,399 | 0.972 | 0.999 | 0.997 | 0.995 | 0.992 | 0.997 | 0.992 | 0.953 | 0.999 | 0.957 | 0.947 | 0.952 | 0.971 | 0.935 | 0.995 | 0.997 |
| 2024 | 9,526 | 0.968 | 0.996 | 0.996 | 0.994 | 0.989 | 0.998 | 0.987 | 0.947 | 0.996 | 0.956 | 0.946 | 0.951 | 0.961 | 0.935 | 0.994 | 0.997 |
| 2025 | 9,353 | 0.964 | 0.996 | 0.996 | 0.994 | 0.988 | 0.998 | 0.983 | 0.942 | 0.997 | 0.951 | 0.941 | 0.946 | 0.962 | 0.934 | 0.994 | 0.996 |

Gates (saleq/ibq/atq/ceqq/cheq/cshoq >= 0.95 and oancfy/capxy >= 0.92 every year): PASS.

### rdq (first-reported 10-Q/10-QT rows of linked member issuers)

| year | periods | rdq_populated | rdq_8k_same_date (gate) | rdq_from_8k_pit (strict) |
| --- | --- | --- | --- | --- |
| 2020 | 7,599 | 1.0000 | 0.9108 | 0.8610 |
| 2021 | 8,214 | 1.0000 | 0.9015 | 0.8500 |
| 2022 | 7,569 | 1.0000 | 0.9417 | 0.8938 |
| 2023 | 7,075 | 1.0000 | 0.9459 | 0.9008 |
| 2024 | 7,170 | 1.0000 | 0.9411 | 0.8891 |
| 2025 | 7,070 | 1.0000 | 0.9267 | 0.8769 |

Gate metric `rdq_8k_same_date` >= 0.95 (ruling T3-b): FAIL. Counts a matched primary 8-K accepted
on or before the ET date of the row's filing clock (filed fallback), with strict PIT share reported alongside.
This gate relaxation changes no published item clock or rdq sourcing. Residual calendar coverage routes to Task 9.

### Four quarters vs the annual (latest vintages; annual-base periods of linked member issuers)

| year | saleq FYs | saleq within 0.5% | niq FYs | niq within 0.5% | ibq within 0.5% (info) |
| --- | --- | --- | --- | --- | --- |
| 2020 | 2,301 | 0.9770 | 2,385 | 0.9807 | 0.8961 |
| 2021 | 2,492 | 0.9655 | 2,578 | 0.9697 | 0.8990 |
| 2022 | 2,375 | 0.9768 | 2,443 | 0.9779 | 0.8998 |
| 2023 | 2,221 | 0.9797 | 2,282 | 0.9838 | 0.9103 |
| 2024 | 2,234 | 0.9781 | 2,299 | 0.9839 | 0.9197 |
| 2025 | 2,132 | 0.9789 | 2,225 | 0.9847 | 0.9533 |

Gate (saleq and niq within 0.5% on >= 98% every year): FAIL. Restatement vintages improve saleq to
0.9655-0.9797 and niq to 0.9697-0.9847. Upstream v10 Q4 derivation and history limits remain for Task 5.
Annual values move by the available quarterly-history deltas; the sum4 metric therefore partly shares inputs
with the derivation and is not an independent annual-source benchmark.

### Balance identity (latest vintage per (cik, datadate) with atq, linked member issuers)

| year | issuer-quarters | atq = ltq + teqq (gate) | with temporary equity (info) | ltq or teqq missing |
| --- | --- | --- | --- | --- |
| 2020 | 10,375 | 0.9124 | 0.9427 | 39 |
| 2021 | 11,272 | 0.9020 | 0.9424 | 40 |
| 2022 | 10,387 | 0.9195 | 0.9599 | 42 |
| 2023 | 9,700 | 0.9280 | 0.9635 | 31 |
| 2024 | 9,829 | 0.9287 | 0.9688 | 29 |
| 2025 | 9,614 | 0.9245 | 0.9688 | 22 |

Gate (atq = ltq + teqq within 0.5% on >= 97%): FAIL. Temporary equity is neither liability nor permanent equity;
with the same period's latest visible catalog temporary-equity term the share is 0.9424-0.9688.
Upstream v10 balance/history issues remain for Task 5; the information term never uses another datadate.

### Other gates

* FSDS (own-filing FSDS NUM cells, datadate 2020-2025, stratified by column and year): cells 6084, strata 234, quota 26, mapped 6039 (0.9926), matched 6030 (0.9985 of mapped): PASS (>= 98%).
  Per column in `validation.json` (`fsds.by_col`).
* Cutoff rebuild (90 seeded CIKs incl. 19 FX-converted; cutoffs 2017-06-30, 2020-03-31, 2021-08-13, 2023-05-10,
  2025-11-14 22:00 UTC): 19,956 rows, 1,691,724 cells, 0 differences: PASS.
* Hand check (30 seeded first-reported 10-Q rows 2021-2025 vs the filing's inline XBRL from EDGAR): 0 mismatches over
  240 item checks: PASS (row by row in `.superpowers/sdd/tier1-v4/task-3-report.md`).

## Commands

```bash
cd atx-db; export PYTHONPATH=src OPENBLAS_NUM_THREADS=1      # every command under the memory guard
G="../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60"
python $G -- python -m atx_db.alpha_panel.compustat_std build --fund-stage fundamentals_v10 --earn-stage earnings_calendar --buckets 32
python $G -- python -m atx_db.alpha_panel.compustat_std validate     # coverage rdq sum4 balance fsds
python $G -- python -m atx_db.alpha_panel.compustat_std cutoff       # 0 differences required
python $G -- python -m atx_db.alpha_panel.compustat_std handcheck    # network: SEC via atx_db.sec_http (<= 5 req/s)
```

Measured: build 67 s, peak 0.69 GiB (guard cap 0.8 GiB; DuckDB 450 MB; 8 CIK buckets); validate 37 s / 0.57 GiB;
cutoff 15 s / 0.51 GiB; handcheck 22 s / 0.12 GiB. Scratch `_tmp/compustat_std/facts.parquet` (33 MB, the prepared
Company Facts subset) is kept for the cutoff check.

## Known limits

* Later filings that change older quarterly-history values emit additional fundq/funda vintages. Values supported
  by history move by the visible delta from the base filing; flows use four-quarter deltas, YTD uses visible
  quarters, and EPS/IB/share facts are recomputed. Other catalog fields and share/debt descriptors retain base
  values where the upstream history has no corresponding item. FX restatements retain the base conversion rates.
* Quarterly items inherit the fundamentals stage's derivations (discrete quarter = 3-month fact, else YTD
  difference): filer tagging errors in a Q4 3-month fact pass through.
* `cshoq` for multi-class issuers: Company Facts has no dimensional dei counts, so the balance-sheet or weighted-
  average share count is used (documented fundamentals rule); it can differ from the class-summed cover count (one
  hand-check issuer: -0.7%).
* `ltq` excludes temporary equity; `atq = ltq + teqq` fails where temporary equity exists.
* `rdq` 8-K sourcing is limited by `earnings_calendar` coverage (no FPI 6-K; issuers that never file item 2.02).
* `ibq`/`epspxq` are NULL when the period has discontinued operations and the filing gives no continuing-operations
  value; IFRS continuing-operations EPS concepts are sparse in the extract.
* `dvy` is common dividends paid (fundamentals dvc chain); funda carries both `dvc` and the catalog `dv` (total).
* The fiscal-year-end month follows FY rows already received: quarters of a new fiscal calendar before the transition
  10-KT keep the old `fyr`.
