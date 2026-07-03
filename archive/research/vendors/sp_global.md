# S&P Global Market Intelligence — Competitive Profile

> Research target for **ats-eqt** (open-source equity fundamentals + supply chain alternative). Covers Compustat, Capital IQ Pro, Panjiva, S&P Global Marketplace, GICS, and CIQ identifiers.
>
> Compiled: 2026-05-09. Items the team could not fully verify from primary public sources are tagged `[unverified]`.

---

## 1. Company & product overview

### 1.1 Corporate structure

S&P Global Inc. (NYSE: SPGI) is the parent. After the 2022 IHS Markit merger and subsequent divestitures (notably CUSIP Global Services to FactSet, see §5), the company reports five operating segments in its 2024 10-K:

| Segment | FY2024 revenue | Notes |
|---|---|---|
| Ratings | ~$4.4B (+31% YoY) | Credit ratings business; not the focus of ats-eqt |
| **Market Intelligence** | **~$4.6B (+6% YoY)** | **Compustat, Capital IQ, Panjiva live here** |
| Commodity Insights (formerly Platts) | ~$2.1B (+10%) | Energy / commodities benchmarks |
| Mobility (CARFAX et al.) | ~$1.6B [unverified exact] | Auto data, ex-IHS Markit |
| Indices (S&P DJI, joint w/ CME) | ~$1.6B (+16%) | S&P 500, DJIA, GICS co-owner |

Total FY2024 revenue ~$14.21B, +14% YoY (source: https://investor.spglobal.com/news-releases/news-details/2025/SP-Global-Reports-Fourth-Quarter-and-Full-Year-2024-Results/default.aspx; https://www.sec.gov/Archives/edgar/data/64040/000006404025000052/spgi-20241231.htm).

The relevant unit for ats-eqt is **Market Intelligence**, which packages Compustat fundamentals, Capital IQ entity / transaction data, Panjiva supply chain, and S&P Global Marketplace as the cloud delivery surface.

### 1.2 Product portfolio for fundamentals + supply chain

```
                       S&P Global Market Intelligence
                                  |
  +-----------------+---------------------+----------------+
  |                 |                     |                |
  Compustat     Capital IQ Pro       Panjiva       S&P Global Marketplace
  (fundamentals) (terminal +         (trade data)   (cloud delivery,
                  ownership)                          Snowflake/Xpressfeed)
                                                          |
                                                  GICS codes (joint w/MSCI)
                                                  CIQ entity & security IDs
```

### 1.3 Differentiators they advertise

- **History depth**: Compustat annual back to 1950, quarterly back to 1962 (https://en.wikipedia.org/wiki/Compustat); point-in-time snapshots back to 1987 (https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database).
- **Standardization**: human-curated mapping of every line item to a standardized chart-of-accounts so cross-company / cross-period / cross-jurisdiction comparisons hold.
- **Coverage**: they advertise ~99,000 global securities representing ~99% of world market cap (https://en.wikipedia.org/wiki/Compustat).
- **Identifier ecosystem**: GVKEY (Compustat), CIQ companyId / securityId / tradingItemId, and historically CUSIP (until 2022).
- **One-stop**: Compustat fundamentals, ownership, transactions (M&A/equity/debt), estimates, transcripts, ESG, Panjiva trade — all keyed off a single identifier graph.
- **Cloud delivery**: Snowflake Secure Data Sharing for Marketplace customers (https://press.spglobal.com/2020-09-09-S-P-Global-Market-Intelligence-and-Snowflake-Collaborate-to-Enable-Seamless-Delivery-of-Financial-and-Alternative-Data).

---

## 2. Compustat data model

### 2.1 Xpressfeed (XPF) — the file/format/loader

"Xpressfeed" is both the bulk delivery channel and the canonical relational format. S&P ships a **Loader** binary that auto-creates the schema and indexes in a target RDBMS (SQL Server, Oracle, DB2, PostgreSQL, and now Snowflake). Xpressfeed delivers `.txt`/`.gz` flat files plus a manifest; the loader applies inserts/updates idempotently. Two file types: **Full History** files (weekly, complete refresh) and **Update files** (incremental since the last full).

Naming convention is package-prefixed; for Compustat North America the core fundamentals are split into:

- `co_afnd1`, `co_afnd2` — Annual Fundamentals (split into two physical tables purely so each row fits within historic RDBMS row-width limits; conceptually one wide table of ~300 standardized annual items).
- `co_afnddc1`, `co_afnddc2` — Annual Data Codes (footnote / source codes for each fundamental item).
- `co_ifndq` — Interim (quarterly) fundamentals, ~100 standardized items.
- `co_ifndsa` — Interim semiannual fundamentals (used for international filers reporting H1/H2 only).
- `company` — entity master: GVKEY, conm (company name), tic (ticker), cusip, cik, fic (country of incorporation), state, sic, naics, gind, ggroup, gsubind, gsector (GICS), ein, costat (active/inactive flag), dlrsn (delisting reason), ipodate, dldte, fyrc (fiscal year-end month).
- `co_idesind` — Industry descriptor / industry classification history (so users can join on a company's GICS-as-of-date rather than its current GICS).
- `co_busdesc`, `co_busdescl` — Business description segment data.
- `co_busseg`, `co_geoseg` — Business / geographic segment financials (SFAS 131 segments).
- `co_filedate` — Filing / SEC submission dates per period.
- `co_industry` — Period-specific industry assignment.
- `co_amkt`, `co_imkt` — Annual / interim market data (price, shares, mkt cap).
- `co_adesind`, `co_idesind` — Annual / interim descriptive industry tables.
- `secd`, `sec_dprc`, `sec_div`, `sec_split` — Security-level daily prices, dividends, splits.
- `idx_ann`, `idx_idx`, `idx_index` — S&P / market index reference tables.
- `names_ix`, `idxcst_his` — Index constituent history.

(Sources: Columbia ITG XPF reference page https://www8.gsb.columbia.edu/itg/faculty/databaseff/compustatx; "Compustat Xpressfeed — Understanding the Data" user guide PDF https://w3.loibl.com/uni/xf_understanding_the_data.pdf; UNIST library mirror of "Using the Data" https://library.unist.ac.kr/libguide/wp-content/uploads/sites/2/2018/11/compustat.pdf; WRDS sample programs https://wrds-www.wharton.upenn.edu/.)

The XPF format introduced **mnemonic field names** (e.g., `at`, `revt`, `niq`) replacing the old numeric "DataItem #" system, plus a much richer manual ("Understanding the Data" + "Using the Data" guides).

### 2.2 Field-level coverage

Compustat North America Fundamentals Annual carries **~300+ standardized line items**; quarterly carries **~100**. Compustat Global / International (formerly "Global Vantage", launched 1993) carries comparable annual depth with IFRS/local-GAAP variants and adds securities and index modules. Together S&P advertises **>3,000 fields** when descriptive, market, and segment tables are counted (https://en.wikipedia.org/wiki/Compustat; https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database).

Representative annual mnemonics (FUNDA / `co_afnd*`):

- Balance sheet: `at` (Assets — Total), `lt` (Liabilities — Total), `ceq` (Common Equity), `seq` (Stockholders' Equity — Total), `che` (Cash & ST Investments), `dltt` (Long-Term Debt), `dlc` (Debt in Current Liabilities), `ppent` (Net PP&E), `invt` (Inventories), `rect` (Receivables — Total), `ap` (Accounts Payable).
- Income statement: `revt` (Revenue — Total), `sale` (Sales), `cogs`, `xsga` (SG&A), `xrd` (R&D), `dp` (D&A), `ebit`, `ebitda`, `oibdp` (Operating Income Before D&A), `ni` (Net Income), `epspx` (EPS — Excluding Extraordinary), `epspi` (EPS — Including).
- Cash flow: `oancf` (Operating CF), `ivncf` (Investing CF), `fincf` (Financing CF), `capx` (CapEx), `dpc` (D&A from cash flow), `xidoc`.
- Misc: `csho` (Common Shares Outstanding), `prcc_f` (Fiscal Year-End Price), `mkvalt` (Market Cap), `dvc` (Common Dividends), `dvp` (Preferred Dividends).

Quarterly (FUNDQ / `co_ifndq`) variants append `q`: `atq`, `ltq`, `revtq`, `niq`, `oibdpq`, `cheq`, `saleq`, `dpq`, `ibq`, `cshoq`, `ceqq`, `seqq`, `txdiq`, `epspxq`, plus the critical timing fields `rdq`, `fyearq`, `fqtr` (https://ionmihai.github.io/finsets/01_wrds/compq.html).

### 2.3 Point-in-time treatment

This is one of S&P's most-defended differentiators against open-source SEC/EDGAR-derived datasets:

- **Compustat Snapshot (a.k.a. "Compustat Point-in-Time", "PIT")** — a separate add-on product (additional license fee). Each row is keyed not just by `(gvkey, datadate)` but also by an `as-of` timestamp so you can reproduce the exact view of a financial fact as of any historical date. Snapshot is structured as a "long" set of nested rows where each `(gvkey, fyearq, fqtr)` accumulates one row per restatement event.
- **"As First Reported" vs "Most Recently Restated"** — Snapshot lets you pick either lens. The "first reported" lens is critical for backtesting (no look-ahead), the "restated" lens is critical for fundamental research.
- **Snapshot history depth**: monthly snapshots go back to **1987** (https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database). Earlier history is restated-only.
- The standard (non-Snapshot) Compustat tables are **restatement-overwriting** by default — each `(gvkey, datadate)` row holds the most recent values. Older states are gone unless you have Snapshot or are reading Update files chronologically. This is why Snapshot is the academically preferred product (https://www.oreilly.com/library/view/equity-valuation-and/9780470929919/chap12-sec33.html).

### 2.4 Date conventions

| Field | Meaning |
|---|---|
| `datadate` | Period-end date for the fundamental row (e.g., 2024-12-31 for FY2024). |
| `fyear` / `fyearq` | Fiscal year (and `fqtr` for quarter). |
| `fyr` | Company's fiscal year-end month (1–12). |
| `rdq` | **Report Date Quarterly** — date earnings were first publicly released. Critical for any event-study timing. ~98% accurate vs. press-release sources (https://yuzhu.run/nail-down-earnings-time/). |
| `fdate` | **Final Date** — when S&P considers the quarter/year "final" (i.e., 10-K/10-Q filed and standardized). |
| `pdate` | Preliminary date (Snapshot only) — when the first preliminary capture entered the database. |
| `ldate` | Last update / latest snapshot date. |
| `dldte` | Delisting date (in `company`). |
| `ipodate` | First trading date. |

The `rdq` / `fdate` / `pdate` / `ldate` quartet is what makes a true PIT reconstruction possible — `pdate <= rdq <= fdate <= ldate` for any well-behaved quarter.

### 2.5 Cadence

- Xpressfeed Full History files: weekly.
- Update files: typically **daily** for North America, weekly for some Global/International corners.
- Real-time press release ingest into Snapshot: intraday for active S&P 1500 / Russell 3000 names [unverified for full universe].
- WRDS academic mirror typically lags Xpressfeed by one to seven days (https://wrds-www.wharton.upenn.edu/).

### 2.6 History depth & regional coverage

| Database | Region | Annual back to | Quarterly back to | Notes |
|---|---|---|---|---|
| Compustat North America | US, Canada | 1950 | 1962 | All USD. Active + inactive (delisted) preserved. |
| Compustat Global / Vantage | Non-NA developed + EM | 1987 | 1989–1995 (varies) | Local + USD currency rows; IFRS + local-GAAP. |
| Compustat International | Subset of Global broken out | 1987 | 1989+ | Often packaged together w/ Global. |
| Compustat Bank | US + global banks | ~1987 | ~1987 | Bank-specific call-report-style schema (different items: `tdsa`, `tlres`, `nim`, etc.) |
| Compustat Insurance / Utility | Niche industries | ~1987 | ~1987 | Industry-overlay items. |

Snapshot/PIT add-on coverage starts 1987 (https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database).

---

## 3. Capital IQ schema

Capital IQ (originally a separate company acquired by S&P in 2004, now folded under Market Intelligence) is the **entity / transaction / ownership / estimates / transcripts** layer. Where Compustat is wide-table, Capital IQ uses an **EAV-flavored long format**.

### 3.1 Core tables

| Table | Purpose | Primary key |
|---|---|---|
| `ciqCompany` | Company master (name, address, GICS, status, web, founded, employees) | `companyId` |
| `ciqCompanyType` | Public/private/fund/etc. classification | `companyTypeId` |
| `ciqCompanyUltimateParent` | Corporate hierarchy roll-up | `companyId` |
| `ciqSecurity` | Security-level (one company can have many securities) | `securityId` |
| `ciqTradingItem` | Listing-level (one security can trade on many venues = many trading items) | `tradingItemId` |
| `ciqExchange` | Exchange master | `exchangeId` |
| `ciqFinPeriod` | Defines a reporting period (year, qtr, calendar vs. fiscal) per company | `financialPeriodId` |
| `ciqFinPeriodType` | Period type lookup (Annual, Q1/Q2/Q3/Q4, LTM, YTD, Calendar variants) | `periodTypeId` |
| `ciqFinCollection` | A "collection" of items reported together for a period (one filing) | `financialCollectionId` |
| `ciqFinCollectionType` | Collection types (Press Release, 10-K, 10-Q, Original, Restated, Filing, Pro-Forma) | `financialCollectionTypeId` |
| `ciqFinInstance` | A specific filing/version of a collection (this is the instance you point at) | `financialInstanceId` |
| `ciqFinInstanceItem` | The actual fact rows: one row per (instance, financialItem) | `financialInstanceId, financialItemId` |
| `ciqFinancialItem` | Item dictionary (~5,000+ standardized items: Revenue, EBITDA, Cash, etc.) | `financialItemId` |
| `ciqEstimateConsensus`, `ciqEstimateNumericData` | Sell-side & buy-side estimate consensus + per-broker contributions | various |
| `ciqEstimatePeriod`, `ciqEstimateOriginType` | Estimates period + origin-of-estimate (Mgmt Guidance vs Sell-side vs Buy-side) | `estimatePeriodId`, `estimateOriginTypeId` |
| `ciqTransaction`, `ciqTransactionToCompany` | M&A / equity / debt / private placement events | `transactionId` |
| `ciqOwnership` | Institutional & insider holdings | various |
| `ciqKeyDev` | "Key Developments" — 160+ event types (earnings, guidance, M&A rumors, exec changes, lawsuits, etc.) | `keyDevId` |
| `ciqTranscript` | Earnings call transcripts | `transcriptId` |

(Sources: https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-capital-iq-with-compustat/; https://wrds-www.wharton.upenn.edu/pages/grid-items/introduction-capital-iq/; field references in WRDS_CIQSYMBOL documentation.)

### 3.2 The "long format" data model

Compustat is *wide*: one row per `(gvkey, datadate)` with hundreds of columns. Capital IQ Financials is *long*:

```
ciqFinInstanceItem
-----------------
financialInstanceId    INT      -- which filing instance
financialItemId        INT      -- which line item (e.g., 1001 = Revenue)
periodTypeId           INT      -- Annual / Q1 / LTM / Calendar Annual / etc.
dataItemValue          DECIMAL  -- the number
currencyId             INT
unitTypeId             INT      -- thousands / millions / actual
```

To reconstruct a single-period income statement you join:
`ciqCompany → ciqFinInstance → ciqFinCollection → ciqFinPeriod → ciqFinInstanceItem → ciqFinancialItem`,
filtered by `financialCollectionTypeId` (e.g., "10-K Original" vs "10-K Restated") and `periodTypeId`.

This long form trades query ergonomics for **expressiveness**: you can pull "all items reported on the original 10-K plus all subsequent restatements plus all preliminary press-release versions" with one model. It's effectively a relational implementation of a fact-witness graph: `(company, period, source, asof) → fact`.

### 3.3 Item IDs & period type IDs

`ciqFinancialItem` is a flat dictionary of ~5,000 items each with a stable `financialItemId`. These IDs are public knowledge among CIQ users (e.g., `1001` is widely cited as Revenue [unverified exact ID]). Item IDs map ~1:1 onto Capital IQ Excel Plug-in mnemonics like `IQ_TOTAL_REV`, `IQ_GROSS_PROFIT`, `IQ_EBITDA`, `IQ_NI`.

`ciqFinPeriodType` enumerates Annual, IQ_Q (quarter), IQ_LTM, IQ_YTD, plus calendar-period variants (IQ_CY) for cross-fiscal-year comparability.

`ciqEstimateOriginType` separates **Mgmt Guidance** (issued by the company), **Consensus** (aggregated sell-side), **Buy-side** (selected institutional contributors), and the underlying broker estimates.

### 3.4 Estimates & guidance storage

Each estimate is a fact row: `(companyId, estimatePeriodId, estimateOriginTypeId, brokerEstimateId, asOfDate, value)`. A consensus is a roll-up of broker rows for a given period and item. Management guidance lives in the same shape but with `estimateOriginTypeId = "Management"` and is linked to the issuing key-development event (`ciqKeyDev`). This enables "did mgmt beat their own guidance?" queries directly without text mining.

---

## 4. Panjiva supply chain data model

### 4.1 Acquisition history & scope

Panjiva was founded 2006 by Josh Green and Jim Psota. **Acquired by S&P Global in February 2018** (https://en.wikipedia.org/wiki/Panjiva). Today Panjiva is delivered both as a standalone Panjiva.com subscription and as Xpressfeed tables alongside Compustat.

### 4.2 Data sourcing

The core data is **government-mandated bill-of-lading and customs declarations**, sourced country-by-country:

- **United States**: CBP Automated Manifest System (AMS) sea cargo data. Public under FOIA; Panjiva is one of several commercial republishers (others: ImportGenius, Datamyne) but is the broadest at S&P scale. Air & rail manifest data are *not* included by AMS, which is a structural gap (see §4.6). (Source: Federal Reserve FEDS paper "Bill of Lading Data in International Trade Research" https://www.federalreserve.gov/econres/feds/files/2021066pap.pdf.)
- **Latin America** (heavy coverage): Brazil, Mexico (export side, MX exports), Bolivia, Chile, Colombia, Ecuador, Panama, Paraguay, Peru, Uruguay, Venezuela.
- **Asia** (variable): India, Indonesia, Pakistan, Sri Lanka, Vietnam, Philippines (historical), Turkey.
- **China**: covered 2011–2018, then **went dark** in 2018 when Chinese authorities restricted re-publication of customs declarations. Panjiva still infers Chinese trade flows via shipper records on US imports.

S&P advertises 21 country sources today (https://panjiva.com/import-export/by-country); the Stanford GSB Library lists ~14 countries with full bill-of-lading depth. The discrepancy comes from "country of source" vs "country of trade-flow". (Sources: https://libguides.stanford.edu/blogs/library/new/14145/new-resource-panjiva-supply-chain-intelligence; https://panjiva.com/.)

### 4.3 Schema in Xpressfeed

Panjiva's Xpressfeed tables are partitioned by year and direction:

```
panjivaUSImport2024, panjivaUSImport2023, ... panjivaUSImport2007
panjivaMXExport2024, panjivaMXExport2023, ...
panjivaUSImpHSCode2024 (one row per (panjivaRecordId, hsCode))
panjivaCompanyCrossRef            -- Panjiva entity → CIQ companyId mapping
panjivaHSClassification           -- HS code reference
```

(Source: GitHub S-P-Quantamental notebook https://github.com/S-P-Quantamental/Ship-to-Shore-Mapping-the-Global-Supply-Chain-with-Panjiva-Shipping-Data-in-Xpressfeed.)

Representative shipment record fields:

| Field | Description |
|---|---|
| `panjivaRecordId` | Unique shipment identifier |
| `arrivalDate`, `shpmtDate` | Arrival at destination port / shipping date |
| `shpName`, `shpAddress`, `shpCity`, `shpCountry`, `shpPanjivaId` | Shipper (foreign exporter) |
| `conName`, `conAddress`, `conCity`, `conState`, `conPanjivaId` | Consignee (US importer) |
| `notifyParty`, `forwarder`, `carrier` | Other parties |
| `vesselName`, `voyageNumber`, `vesselIMO` | Vessel ID (deduped against Lloyd's registry) |
| `portOfLading`, `portOfUnlading` | UN/LOCODE port codes |
| `containerCount`, `volumeTEU`, `weightT`, `weightKg` | Quantity |
| `valueOfGoodsUSD` | Declared value (when present — many CBP records have value masked) |
| `hsCode` (multi-valued via `panjivaUSImpHSCode*`) | Harmonized System product code, often inferred from goods description if Customs stripped it |
| `goodsDescription` | Free-text description |
| `containerNumber`, `containerSizeType`, `sealNumber` | Container-level detail |
| `billOfLadingNumber`, `masterBillOfLadingNumber` | The BOL itself |

Panjiva advertises >100 fields per shipment record [unverified exact count] and roughly **1.5–2 billion total shipment records across all countries** [unverified — S&P marketing has cited "billions"].

### 4.4 Aggregation into supplier/buyer relationships

Raw bills are noisy: a single importer may appear under 50+ name spellings. Panjiva runs an **entity resolution pipeline** that:

1. Normalizes the raw `shpName` / `conName` strings (case, punctuation, corporate suffix).
2. Clusters via address + tax ID + vessel/route co-occurrence.
3. Assigns a stable `panjivaCompanyId` to each cluster.
4. Cross-references against `ciqCompany` via `panjivaCompanyCrossRef` to give each Panjiva entity (where possible) a CIQ `companyId`.

Aggregations (supplier→buyer relationships, port flows, HS-code rollups) are computed downstream from this resolved entity layer, which is how you get "Apple's #3 supplier in 2024 was Foxconn" insights from raw US Customs strings.

In the U.S., the actual **HS code is removed by Customs before public release**; Panjiva *imputes* it from the goods description text using NLP (https://www.federalreserve.gov/econres/feds/files/2021066pap.pdf). Imputation accuracy is a known weakness — academic work has flagged disagreement rates with WTO trade aggregates.

### 4.5 Refresh cadence

- US imports: daily refresh on Panjiva.com; daily Xpressfeed update files for paying subscribers [unverified — S&P documents say "daily" but lag may be 24–72h].
- Latin American exports: typically weekly.
- China (legacy 2011–2018): static.

### 4.6 Known limitations

- **Land trade excluded**: U.S.-Canada and U.S.-Mexico flows that move by truck/rail are **not in CBP's AMS feed**. This is the most-cited limitation in academic literature — the FRB paper notes that bilateral US trade with Canada and Mexico is "largely excluded from bill-of-lading data" because both are predominantly land-borne (https://www.federalreserve.gov/econres/feds/files/2021066pap.pdf). Panjiva backfills MX *exports* via MX customs declarations, which captures part of this.
- **Air freight excluded**: AMS is sea-only; high-value, low-weight goods (semiconductors, pharmaceuticals) are systematically under-counted.
- **Value masking**: many CBP records have declared value redacted at importer request via the "Importer Confidentiality" program.
- **China gap 2018+**: as above.
- **HS code imputation noise**: text-derived HS codes fail on cryptic goods descriptions.
- **Coverage uneven across countries**: e.g., India has solid coverage, Japan has effectively none.

---

## 5. Identifier system

### 5.1 The CIQ identifier triplet

Capital IQ has a **3-level identifier hierarchy**:

```
companyId  ----<  securityId  ----<  tradingItemId
(entity)        (issued security)    (listing on a venue)
```

Example for AT&T:
- `companyId` = 18921 (the entity AT&T Corp; survives reorgs that don't dissolve the legal entity).
- `securityId` for AT&T common stock (one per security class).
- `tradingItemId` for AT&T common stock listed on NYSE; a separate `tradingItemId` if it's also on a German exchange via ADR/sponsored-listing.

(Source: WRDS https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-capital-iq-with-compustat/.)

This hierarchy is more nuanced than CUSIP (which conflates security and listing) and is one reason CIQ IDs are preferred for global multi-listing analysis.

### 5.2 Compustat's GVKEY

`gvkey` is a 6-digit zero-padded string that uniquely identifies a Compustat company (entity). It's the join key for everything in `co_*` tables. There is an **N:1 mapping from CIQ `companyId` → Compustat `gvkey`** (CIQ has many private companies that don't have a `gvkey`).

### 5.3 Cross-references to industry IDs

WRDS provides six standard concordance tables:
`WRDS_CIK`, `WRDS_CIQSYMBOL`, `WRDS_CUSIP`, `WRDS_GVKEY`, `WRDS_ISIN`, `WRDS_TICKER`. Each maps a CIQ `companyId` (and securityId/tradingItemId where relevant) to the named external identifier.

| External ID | Notes |
|---|---|
| CUSIP | 9-char, North America. **Important: S&P sold CUSIP Global Services to FactSet in March 2022 for $1.925B**. CUSIP is now operated by FactSet under license from the ABA, *not* by S&P Global. (Source: https://investor.factset.com/news-releases/news-release-details/factset-completes-acquisition-cusip-global-services.) S&P retains a license to use CUSIP in its products. |
| ISIN | 12-char ISO-standard global identifier; CGS is the US National Numbering Agency for the ISIN scheme. |
| SEDOL | UK NNA. |
| Ticker | Exchange-specific; multiple tickers per security across venues. |
| CIK | SEC filer ID. |
| LEI | Legal Entity Identifier (ISO 17442). Cross-referenced but not a primary CIQ key. |

### 5.4 Symbology / concordance file structure

The Xpressfeed `security` and `secd` tables carry the relevant cross-IDs per security. WRDS_CIQSYMBOL and the WRDS_GVKEY linking table are the canonical concordance files in the academic distribution. Format is wide-row CSV/parquet, typically `(companyid, gvkey, securityid, tradingitemid, ticker, cusip, isin, primary_flag, active_flag, exchange, asof_start, asof_end)`. The `asof_start/end` columns are critical — tickers and CUSIPs are reassigned over time (e.g., post-merger).

---

## 6. Data collection & quality methodology

### 6.1 Historical: human analyst capture

The original Compustat process from 1962 onwards was **human analyst capture from 10-K paper filings**. Analysts mapped each filer's chart of accounts to S&P's standardized item dictionary, applying judgment about non-standard line items (e.g., "Restructuring & related charges, net" → which Compustat item?). This is the lineage of the current item dictionary — many oddly-named items reflect 1970s-1980s industrial America. (Source: https://sites.bu.edu/qm222projectcourse/files/2014/08/compustat_users_guide-2003.pdf.)

### 6.2 Modern automation

S&P has progressively automated the pipeline:

- **XBRL ingestion**: SEC XBRL filings (mandatory since 2009 for large filers) are parsed automatically and pre-populate item values. However, **Compustat does not equal XBRL**: S&P explicitly retains its own standardization layer. A 2011 paper documented that 17 of 30 commonly-used variables differed significantly between Compustat values and as-XBRL-filed values, attributing the divergence to S&P's proprietary normalizations (e.g., reclassifying items that SEC tags inconsistently). (Sources: https://www.aabri.com/manuscripts/11798.pdf; "Lost in Standardization" https://business.columbia.edu/sites/default/files-efs/imce-uploads/CEASA/Events%20Page/revisiting_accounting-based_return_anomalies.pdf.)
- **Press release ingest**: a real-time team captures preliminary numbers from PR Newswire / Business Wire to populate the Snapshot "preliminary" rows ahead of the 10-K/10-Q filing.
- **HTML parsing**: where XBRL is missing or low-quality, S&P falls back to HTML parsing of the filing.
- **Stewards**: human "data stewards" remain in the loop for non-standard items, M&A re-classifications, and segment data. S&P has explicitly stated stewards monitor reporting changes and are "well-versed in U.S. GAAP and IFRS reporting standards" (https://xbrl.us/harmonizing-accounting-data-standards/).

### 6.3 QA pipeline

Disclosed QA layers include cross-period continuity checks, balance-sheet identities (`AT = LT + SEQ`), fundamentals-vs-press-release reconciliation, and footnote-code annotations (`co_afnddc1`/`co_afnddc2`) that flag values as estimated, reclassified, or sourced-from-footnote. Detail beyond that is proprietary.

### 6.4 GAAP/IFRS standardization at field level

Compustat ships **one standardized item dictionary**. For Compustat Global, each fact carries an `accounting_standard` flag (US-GAAP / IFRS / local-GAAP) but the column it lands in is the same — i.e., S&P forces both into a single canonical chart-of-accounts. This is what enables apples-to-apples cross-listing comparisons and is the headline value-add over raw XBRL/EDGAR. The trade-off is loss of fidelity: an IFRS-only concept (e.g., revaluation reserves) may collapse into a U.S.-GAAP-shaped bucket and lose nuance.

---

## 7. Delivery technology

### 7.1 Xpressfeed (legacy / still primary for institutionals)

- **Format**: pipe/tab-delimited flat files in a versioned, well-documented schema.
- **Loader**: cross-platform binary (Windows / Linux) that auto-creates DDL and loads files into SQL Server, Oracle, DB2, PostgreSQL, Snowflake, and (via partner adapters) Databricks.
- **Manifest**: each delivery includes a manifest XML for atomic batch detection.
- **Cadence**: Full History weekly + Update files daily.
- **Transport**: SFTP historically; S3 / Azure Blob / GCS for cloud-native customers.
- (Source: S&P brochure https://www.spglobal.com/marketintelligence/en/documents/130337_update-xf-brochure-to-include-panjiva_ltr_v2.pdf.)

### 7.2 S&P Global Marketplace (cloud-native)

Launched ~2019, expanded with the Snowflake partnership in September 2020. Marketplace exposes >20 datasets (Compustat, Capital IQ, Panjiva, ESG, transcripts, etc.) via:

- **Snowflake Secure Data Sharing**: zero-copy, zero-ETL — customer queries S&P data directly inside their Snowflake account.
- **Databricks Delta Sharing** [unverified — Marketplace lists Databricks but degree of native integration unclear].
- **Parquet / S3 distribution** for bulk download.
- **REST APIs** for selected datasets (CIQ Estimates, Key Devs).

(Sources: https://press.spglobal.com/2020-09-09-S-P-Global-Market-Intelligence-and-Snowflake-Collaborate-to-Enable-Seamless-Delivery-of-Financial-and-Alternative-Data; https://www.support.marketplace.spglobal.com/content/dam/spglobal/mi/en/documents/marketplace/snowflake/sp-global-data-on-snowflake.pdf.)

### 7.3 WRDS (Wharton Research Data Services) — academic distribution

Wharton's WRDS platform has been the de-facto academic distribution channel for Compustat/CIQ since the 1990s:

- Hosts a near-mirror of Compustat (NA, Global, Bank, Execucomp, Snapshot) and Capital IQ (transactions, key devs, ownership, transcripts).
- SQL Server / PostgreSQL / SAS / Stata / Python / R access via web + programmatic.
- Provides curated "linking tables" (`ccmxpf_lnkhist` for CRSP↔Compustat, `wrds_ciqsymbol` for CIQ).
- The platform is the bedrock of accounting/finance academic research; a huge fraction of published empirical finance papers cite WRDS-Compustat as the data source.

### 7.4 API / programmatic access

- **Capital IQ Excel Plug-in**: still the dominant retail-analyst entry point. Functions like `=IQ_TOTAL_REV("MSFT", "IQ_LTM")` issue REST calls to a Capital IQ data backend.
- **CIQ "On Demand" REST API**: documented developer portal at marketintelligence.spglobal.com. Supports JSON / XML response formats. Rate-limited per seat.
- **Marketplace REST APIs**: per-dataset.

---

## 8. Pricing signals & licensing

S&P does not publish list prices. Public signals:

| Product | Pricing signal |
|---|---|
| Compustat (commercial) | Bundled into Capital IQ Pro tiers or Xpressfeed contracts. Standalone Xpressfeed enterprise contracts: low-six-figures to low-seven-figures USD/yr depending on packages, geography, and # of derived products. [unverified specifics] |
| Compustat (academic via WRDS) | Universities pay WRDS a tier-based subscription; Compustat NA + Global + Snapshot together is roughly **$25K–$70K/yr** for a mid-sized research university (https://www.econjobrumors.com/topic/wrds-data-cost; https://www.econjobrumors.com/topic/crsp-and-compustat-subscription). Often co-purchased with CRSP for ~$70K combined. |
| Capital IQ Pro per-seat | **$12K–$30K/user/yr**, with $18K–$25K typical for a standard seat that includes Excel plug-in + private company data; volume discounts can drop large deployments to $5K–$12K/user/yr. (Source: https://costbench.com/software/financial-data-terminals/sp-capital-iq/.) |
| Panjiva (standalone, panjiva.com) | Not publicly listed; commercial-banking tier is reportedly $20K–$50K/yr [unverified]. |
| Panjiva (in Xpressfeed) | Add-on to enterprise Xpressfeed; typical add-on cost reportedly $50K–$150K/yr depending on country coverage [unverified]. |
| Bulk data licensing | Negotiated per-dataset; redistribution rights priced separately. Marketplace+Snowflake adds a Snowflake compute pass-through. |

For a Bloomberg comparison: Bloomberg Terminal = $28,320–$31,980/user/yr (https://costbench.com/software/financial-data-terminals/bloomberg-terminal/). Capital IQ Pro is positioned as the **30–50% cheaper** alternative for fundamentals-heavy users (less optimized for fixed-income/FX trading where Bloomberg dominates).

### 8.1 Licensing notes for an open competitor

- **Compustat redistribution is hard-licensed**: derivative datasets that "look like" Compustat invite legal scrutiny.
- **GICS is jointly owned by S&P + MSCI** and licensed; an open competitor would need to either license GICS or use a free alternative (NAICS, ICB-via-FTSE, or build its own taxonomy).
- **CUSIP is now FactSet/ABA-owned** and is licensed per-use; an open competitor should anchor on CIK + LEI + ISIN (where free) and use FIGI (Bloomberg's open identifier) where possible.
- **Panjiva's underlying data (US CBP) is FOIA-public** — there is no IP barrier to re-collecting US AMS data; the moat is entity resolution and HS-code imputation.

---

## 9. Sources

1. https://investor.spglobal.com/news-releases/news-details/2025/SP-Global-Reports-Fourth-Quarter-and-Full-Year-2024-Results/default.aspx — S&P Global FY2024 earnings release.
2. https://www.sec.gov/Archives/edgar/data/64040/000006404025000052/spgi-20241231.htm — S&P Global Inc 10-K FY2024.
3. https://s29.q4cdn.com/690959130/files/doc_financials/2024/ar/S-P-Global-2024-Annual-Report.pdf — S&P Global 2024 Annual Report.
4. https://www.spglobal.com/en/annual-reports/2024 — S&P Global Annual Report 2024 portal.
5. https://en.wikipedia.org/wiki/S%26P_Global — Wikipedia: S&P Global.
6. https://en.wikipedia.org/wiki/Compustat — Wikipedia: Compustat.
7. https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database — LSEG / Refinitiv on the S&P Compustat database (history, point-in-time, depth).
8. https://w3.loibl.com/uni/xf_understanding_the_data.pdf — "Compustat Xpressfeed Understanding the Data" user guide.
9. https://library.unist.ac.kr/libguide/wp-content/uploads/sites/2/2018/11/compustat.pdf — "Compustat Xpressfeed Using the Data" user guide (UNIST library mirror).
10. https://www8.gsb.columbia.edu/itg/faculty/databaseff/compustatx — Columbia Business School ITG: Compustat Xpressfeed reference.
11. https://wrds-www.wharton.upenn.edu/pages/grid-items/compustat-global-wrds-basics/ — WRDS: Compustat Global basics.
12. https://wrds-www.wharton.upenn.edu/pages/grid-items/compustat-execucomp-basics/ — WRDS: Compustat Execucomp basics.
13. https://wrds-www.wharton.upenn.edu/demo/compustat/form/ — WRDS Compustat Annual Fundamentals demo (variable list).
14. https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-capital-iq-with-compustat/ — WRDS: Capital IQ linking.
15. https://wrds-www.wharton.upenn.edu/pages/grid-items/introduction-capital-iq/ — WRDS: Introduction to Capital IQ.
16. https://wrds-www.wharton.upenn.edu/pages/grid-items/capital-iq-introduction/ — WRDS Capital IQ landing.
17. https://wrds-www.wharton.upenn.edu/pages/grid-items/capital-iq-transcripts/ — WRDS Capital IQ Transcripts.
18. https://wrds-www.wharton.upenn.edu/documents/403/CRSP_-_Compustat_Merged_Database_CCM_NiIeIWV.pdf — CRSP/Compustat Merged DB doc.
19. https://ionmihai.github.io/finsets/01_wrds/compq.html — finsets: Compustat quarterly variables (atq, ltq, niq, oibdpq, rdq, etc.).
20. https://yuzhu.run/nail-down-earnings-time/ — academic note on RDQ field accuracy.
21. https://www.oreilly.com/library/view/equity-valuation-and/9780470929919/chap12-sec33.html — Equity Valuation appendix on Compustat Point-in-Time / IBES.
22. https://som.yale.edu/sites/default/files/2024-07/Re-Standardized%20Financial%20Statement%20Data.pdf — Yale paper: re-standardized financial statement data (Compustat critique).
23. https://www.aabri.com/manuscripts/11798.pdf — "Data differences — XBRL versus Compustat".
24. https://business.columbia.edu/sites/default/files-efs/imce-uploads/CEASA/Events%20Page/revisiting_accounting-based_return_anomalies.pdf — Columbia: "Lost in Standardization".
25. https://xbrl.us/harmonizing-accounting-data-standards/ — XBRL US: harmonizing accounting standards (Compustat stewards quote).
26. https://sites.bu.edu/qm222projectcourse/files/2014/08/compustat_users_guide-2003.pdf — Compustat User's Guide 2003 (analyst capture lineage).
27. http://volweb.utk.edu/~pdaves/Computerhelp/COMPUSTAT/Compustat_manuals/user_02.pdf — Understanding Compustat North America Database, ch. 2.
28. http://larryschrenk.com/Capital%20IQ/Excel%20Plug-in%20Manual.pdf — S&P Capital IQ Excel Plug-in Manual (Jan 2017).
29. http://larryschrenk.com/Compustat/Documentation/COMPUSTAT%20(Global)%20Data%20Guide%20(2002).pdf — Compustat Global Data Guide (2002).
30. https://www.scribd.com/document/331103626/tech-faq-12478964 — S&P Capital IQ API Usage Guide.
31. https://www.scribd.com/document/637489366/spglobalapidevelopersguide — S&P Global API Developer's Guide.
32. https://www.scribd.com/document/161578486/Ciq-Financials-Methodology — CIQ Financials Methodology document.
33. https://libguides.nypl.org/CapitalIQ/FinancialsGlossary — NYPL Capital IQ Financials Glossary guide.
34. https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro — Capital IQ Pro product page.
35. https://www.spglobal.com/content/dam/spglobal/mi/en/documents/general/S-P-Capital-IQ-Platform-Brochure.pdf — Capital IQ Platform Brochure.
36. https://costbench.com/software/financial-data-terminals/sp-capital-iq/ — Capital IQ pricing 2026.
37. https://costbench.com/software/financial-data-terminals/bloomberg-terminal/ — Bloomberg Terminal pricing 2026 (comparison anchor).
38. https://www.wallstreetprep.com/knowledge/bloomberg-vs-capital-iq-vs-factset-vs-thomson-reuters-eikon/ — WSP comparison: Bloomberg vs CapIQ vs FactSet vs Refinitiv.
39. https://www.spglobal.com/market-intelligence/en/solutions/products/panjiva-supply-chain-intelligence — Panjiva product page.
40. https://en.wikipedia.org/wiki/Panjiva — Wikipedia: Panjiva.
41. https://panjiva.com/ — Panjiva home / coverage.
42. https://panjiva.com/import-export/by-country — Panjiva country coverage.
43. https://www.federalreserve.gov/econres/feds/files/2021066pap.pdf — Flaaen/Wang FEDS paper "Bill of Lading Data in International Trade Research".
44. https://onlinelibrary.wiley.com/doi/abs/10.1111/roie.12657 — Flaaen/Wang Wiley publication of same paper.
45. https://github.com/S-P-Quantamental/Ship-to-Shore-Mapping-the-Global-Supply-Chain-with-Panjiva-Shipping-Data-in-Xpressfeed — S&P sample notebook with Panjiva schema field names.
46. https://www.spglobal.com/content/dam/spglobal/mi/en/documents/general/Ship-to-Shore-Mapping-the-Global-Supply-Chain-with-Panjiva-Shipping-Data-in-Xpressfeed.pdf — accompanying PDF write-up.
47. https://www.spglobal.com/marketintelligence/en/documents/130337_update-xf-brochure-to-include-panjiva_ltr_v2.pdf — Xpressfeed brochure including Panjiva.
48. https://libguides.stanford.edu/blogs/library/new/14145/new-resource-panjiva-supply-chain-intelligence — Stanford GSB on Panjiva coverage.
49. https://press.spglobal.com/2020-09-09-S-P-Global-Market-Intelligence-and-Snowflake-Collaborate-to-Enable-Seamless-Delivery-of-Financial-and-Alternative-Data — Snowflake partnership announcement.
50. https://www.support.marketplace.spglobal.com/content/dam/spglobal/mi/en/documents/marketplace/snowflake/sp-global-data-on-snowflake.pdf — S&P Global Data on Snowflake doc.
51. https://www.snowflake.com/en/customers/all-customers/case-study/sandp-global/ — Snowflake case study on S&P Global.
52. https://www.marketplace.spglobal.com/en/datasets/panjiva-supply-chain-intelligence-(22) — Marketplace dataset card for Panjiva.
53. https://en.wikipedia.org/wiki/Global_Industry_Classification_Standard — Wikipedia GICS.
54. https://www.msci.com/indexes/index-resources/gics — MSCI GICS landing.
55. https://www.spglobal.com/spdji/en/landing/topic/gics/ — S&P DJI GICS landing.
56. https://www.spglobal.com/spdji/en/documents/methodologies/methodology-gics.pdf — GICS methodology PDF.
57. https://www.msci.com/indexes/documents/methodology/1_MSCI_Global_Industry_Classification_Standard_GICS_Methodology_20240801.pdf — MSCI GICS methodology PDF (Aug 2024).
58. https://www.spglobal.com/content/dam/spglobal/mi/en/documents/general/112727-GICS-Mapbook_2018_v3_Letter_DigitalSpreads.pdf — GICS mapbook (PDF).
59. https://www.cusip.com/ — CUSIP Global Services (now FactSet).
60. https://en.wikipedia.org/wiki/CUSIP — Wikipedia CUSIP.
61. https://investor.factset.com/news-releases/news-release-details/factset-completes-acquisition-cusip-global-services — FactSet completes CGS acquisition (March 2022).
62. https://investor.factset.com/news-releases/news-release-details/factset-acquire-cusip-global-services-1925-billion — FactSet to acquire CGS for $1.925B.
63. https://www.prnewswire.com/news-releases/cusip-global-services-and-american-bankers-association-joint-statement-on-factset-acquisition-of-cgs-from-sp-global-301450881.html — CGS/ABA statement on FactSet acquisition.
64. https://www.library.hbs.edu/find/databases/capital-iq-identifiers — HBS Baker Library on CIQ identifiers.
65. https://www.insead.edu/library/company-identifiers — INSEAD library on company identifiers.
66. https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-sustainalytics-with-compustat/ — WRDS linking matrix example.
67. https://guides.nyu.edu/wrds/linking-suite — NYU WRDS linking queries.
68. https://www.econjobrumors.com/topic/wrds-data-cost — academic anecdata on WRDS pricing.
69. https://www.econjobrumors.com/topic/crsp-and-compustat-subscription — CRSP+Compustat combined cost discussion.
70. https://spre.wharton.upenn.edu/ — S&P Global Academic Research Essentials.
71. https://datateamoftheeur.wordpress.com/category/wrds-compustat/ — Erasmus Univ. WRDS Compustat tips.
72. https://bizlib247.wordpress.com/2014/02/21/compustat-fundamentals-finding-data-tips/ — Compustat fundamentals tips.
