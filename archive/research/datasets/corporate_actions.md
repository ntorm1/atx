# Corporate Actions Datasets — vendor schemas + EDGAR/DTCC public reconstruction

**Status:** Research, v0.1
**Audience:** ats-eqt engineering (price-history pipeline, security-master, total-return calc, survivorship layer); ats-core team consuming time-series with splice-point integrity guarantees
**Scope:** corporate-action datasets at field/schema level across CRSP, FactSet, Bloomberg, S&P/Compustat, Refinitiv/LSEG, DTCC, and the NYSE/Nasdaq direct feeds; the EDGAR + DTCC public-reconstruction surface; recommended ats-eqt DDL.
**Last updated:** 2026-05-14

---

## 0. Executive summary

Corporate actions are the most under-engineered surface in retail/open-data equity stacks and the single largest silent source of total-return error in academic and DIY backtests. Unlike fundamentals (where the vendor moat is field standardisation) or 13F (where the moat is entity resolution), corporate-action data has *three* simultaneous moats stacked on top of each other:

1. **Event taxonomy.** A "dividend" is fifteen different things to the IRS, the issuer's transfer agent, the exchange, the depository (DTCC), and CRSP. CRSP's `distcd` is a 6-digit enumerated code with hundreds of distinguishable distribution sub-types; FactSet, Bloomberg, and Refinitiv each have a parallel proprietary taxonomy; DTCC has the ISO 20022 CAEV codes. Mapping between them is the first integration cost.
2. **Splice-point integrity.** A 3-for-1 forward split on AAPL on 2014-06-09 means *every* AAPL price prior to that date in your database must be multiplied by 1/3 *and* every dividend amount, share count, and option strike. Backtests done on naïve closing prices without splice-correct adjustment factors are catastrophically wrong. CRSP's `FACPR` / `FACSHR` discipline is the academic gold standard; most retail feeds get it 95% right and 5% spectacularly wrong (Yahoo: see §5.10).
3. **Survivorship.** Delisted, merged, and bankrupt entities must remain in the universe with a terminal `dlret` field that captures the realised value (or zero) to the holder at delisting. CRSP estimates the survivorship bias at **~1.6 %/yr** on backtests run against a "currently-listed" universe (source: <https://www.crsp.org/research/crsp-survivor-bias-free-us-mutual-funds/>; <https://www.tylergshumway.org/Shumway-DelistingBiasCRSP-1997.pdf>).

**Headline findings (2026-05-14):**

- **CRSP is the only vendor with a fully public field-level methodology** — every adjustment factor, distribution code, and delisting code is documented at <https://www.crsp.org/products/documentation>. This makes CRSP the *de facto* reference implementation for ats-eqt; we should mirror the `distcd`/`dlstcd` enumerations literally.
- **DTCC's CA 20022 service is the canonical issuer-to-broker substrate** — ~1.3M active securities, ISO 20022 messages, files every 16 minutes (source: <https://www.dtcc.com/data-services/corporate-actions-and-reference-data/dtcc-ca-20022-service>). DTCC is the upstream of every commercial vendor; reconstructing DTCC-grade coverage from EDGAR alone is impossible because not all corporate actions are SEC-disclosed (e.g., book-entry technical adjustments).
- **Form 8937 is the only public substrate for spinoff cost-basis allocation** under IRC §358 / §6045B. Issuers must either file with the IRS *or* post on a public website for 10 years (source: <https://www.irs.gov/forms-pubs/about-form-8937>; <https://accountably.com/irs-forms/f8937/>). EDGAR full-text search picks up many of these as "Other Events" 8-K attachments. This is the *only* free path to spinoff basis numerics — vendors charge separately for it.
- **Yahoo Finance's adjusted-close is documented broken for combined split+dividend periods** — they apply un-split-adjusted dividends to split-adjusted close prices (source: <https://github.com/joshuaulrich/quantmod/issues/253>). ats-eqt's free-tier price feed must compute its own adjusted series from raw events + the CRSP-style `FACPR`/`FACSHR` chain rather than pass Yahoo's column through.
- **FIGI survives ticker changes; CUSIP does not survive share-class changes.** OpenFIGI persists across ticker rename and exchange relisting; CUSIP-9 changes when a share class is reclassified or the CINS/ISIN's issue suffix changes (source: <https://www.openfigi.com/assets/local/figi-allocation-rules.pdf>). ats-eqt's `security` table must carry both as time-bounded aliases.
- **The CRSP CCM `ccmxpf_lnkhist` link table is now the canonical GVKEY↔PERMNO bridge** — the older `ccmxpf_linktable` has been deprecated for a 1-year transition (source: <https://www.kaichen.work/?p=138>). Backfills written against `linktable` need to migrate.

---

## 1. Why corporate actions matter

### 1.1 Total return = price return + distribution return, splice-adjusted

The single most important number in equity analytics is total return. For a position held over the interval `[t-1, t]`, the canonical CRSP formula is:

```
RET_t = (PRC_t * (1 / FACPR_t)) - PRC_{t-1} + (DIVAMT_t / CUMFACPR_t / FACPR_t)
        ----------------------------------------------------------------------
                                       PRC_{t-1}
```

(source: <https://leiq.bus.umich.edu/docs/crsp_calculations_splits.pdf>; <http://www.crsp.com/products/documentation/crsp-calculations>; <https://wrds-www.wharton.upenn.edu/pages/grid-items/crsp-useful-variables/>).

If `FACPR_t = 1.0` (no split or stock distribution that day), and `DIVAMT_t = 0` (no cash distribution), this collapses to the naïve `(PRC_t - PRC_{t-1}) / PRC_{t-1}`. Every backtest written against raw prices is *implicitly* assuming this collapse always holds. It does not — for the typical US equity, ex-distribution days happen 4–8 times a year (quarterly dividends + occasional splits), and the cumulative compounding error on a 30-year backtest can exceed 1000% in absolute terms for a security like Berkshire (no dividends, no splits) vs. JNJ (60+ years of dividends).

### 1.2 The four error modes

1. **Look-ahead bias.** Using a *future* corporate action's adjustment factor before its ex-date (a common bug when a vendor "back-fills" the adjustment chain into historical prices but the algorithm reads `today`'s `cumfacpr` for `t-30`).
2. **Survivorship bias.** Computing universe statistics on currently-listed names — CRSP estimates ~1.6%/yr inflation in returns for US equity universes (source: <https://www.crsp.org/research/crsp-survivor-bias-free-us-mutual-funds/>).
3. **Splice-point error.** Joining a pre-spinoff parent series to a post-spinoff parent series without applying the §358 basis allocation factor — produces a synthetic price drop that looks like return but is actually a delivery of value to a different security.
4. **Distribution-type miscoding.** Treating a return-of-capital distribution as a dividend (it is technically a partial repayment of basis, taxed differently, and CRSP's `distcd` distinguishes it: 1232 vs 1262 vs 1272 — see §6).

### 1.3 Bitemporal requirement

Corporate actions are *announced* on date A, become *ex-* on date B, *record* on date C, and *pay* on date D — and an issuer can amend or cancel between A and D. Therefore the `corp_action` table is fundamentally bitemporal: `(announcement_date, knowledge_date)` rather than just `effective_date`. The DTCC CA 20022 messages explicitly carry an event lifecycle status so consumers can distinguish "announced", "confirmed", "cancelled", "withdrawn", and "completed" (source: <https://www.dtcc.com/-/media/Files/Downloads/issues/Corporate-Actions-Transformation/Getting_Started_CA_ISO_20022.pdf>).

---

## 2. Vendor stack matrix

The seven mainstream surfaces, with action-type coverage. "Yes" = first-class field; "Derived" = computable from primitives; "No" = not in feed.

| Vendor | History | Splits | Cash div | Stock div | Spinoffs | M&A | Delist | Name chg | Ticker chg | Cost basis (§358) | IPO calendar | Halts | Native ID |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CRSP US Stock | 1925-12 | Yes | Yes | Yes | Yes (`distcd 3*/5*/6*`) | Yes (`dlstcd 2**`) | Yes (`dlstcd 5**`) | Yes (`dsenames`) | Yes (`dsenames`) | No (must impute) | Derived | No | PERMNO/PERMCO |
| FactSet Adjustments | 1980+ | Yes | Yes | Yes | Yes | Yes | Yes | Yes (FSYM lineage) | Yes (FSYM-L) | No (separate prod) | Yes | No | FSYM |
| Bloomberg DVD/EQY_SPLIT | varies | Yes | Yes | Yes | Yes (TENDER/MERGER) | Yes | Yes | Yes | Yes | Partial (CACS) | Yes (IPO_HIST) | No | FIGI |
| S&P Compustat | 1950 | Yes (`AJEXM`) | Yes (`DVPSP`) | Derived | Linkage via `link_history` | `LU/LX` flags | Last-quarter NULL | Yes (`conml`) | Yes (`tic`) | No | No | GVKEY |
| Refinitiv/LSEG Datastream | 1973+ | Yes (`AF`) | Yes (WS05101) | Yes | Yes | Yes | Yes | Yes (`NAME`) | Yes | No | Yes | No | DSCD/RIC |
| DTCC CA 20022 | 2014+ | Yes (CAEV `SPLF`) | Yes (CAEV `DVCA`) | Yes (`DVSE`) | Yes (`SOFF`) | Yes (`MRGR`/`TEND`) | Yes (`LIQU`/`WRTH`) | Yes (`CHAN`) | Yes (`CHAN`) | No | Partial | No | CUSIP |
| NYSE/Nasdaq direct | varies | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | No | Yes (IPO Indicator) | Yes (Halt codes) | symbol |

Coverage notes:

- **CRSP** is academic, monthly+daily, US-only; history depth is the deepest of any feed (1925 monthly, 1962 daily; source: <https://www.crsp.org/products/research-products/crsp-us-stock-databases/>).
- **FactSet** Adjustments Feed is global and tied to FSYM lineage; documented via the marketplace catalog (source: <https://www.factset.com/marketplace/catalog>; per the FactSet vendor profile in `../vendors/factset.md`).
- **Bloomberg** field-level access is via BDS/BDH formulas in Excel and the BLP API; the canonical functions are `DVD_HIST_ALL` (full event history) and `EQY_SPLIT_HIST` (source: <https://www.emich.edu/cob/programs/finance/flc/documents/formula-method-excel.pdf>; <https://sdmfsa.gitlab.io/latrousse/bloomberg/hist/dvd/>).
- **Compustat** dividend/split data is anchored to fiscal periods (per-period adjustment factors) and is intentionally less granular than CRSP for daily backtests; reconciled to CRSP via `ccmxpf_lnkhist` (source: <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-crsp-with-compustat/>).
- **Refinitiv** historical adjustment is via Datastream's `AF` (Adjustment Factor) static mnemonic and WS-series items for corporate-action timestamps (source: <https://www.bwl.uni-mannheim.de/media/Lehrstuehle/bwl/Maug/Database_info/Datastream_dataypes.pdf>).
- **DTCC** is the depository's view — closest to the issuer-of-record; mandatory vs voluntary distinction is explicit (source: <https://www.dtcc.com/asset-services/corporate-actions-processing/iso-20022-messaging-specifications>).
- **NYSE/Nasdaq** publish IPO/halt calendars publicly (source: <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltCodes>; <https://www.nasdaqtrader.com/trader.aspx?id=IPOIndicator>).

---

## 3. CRSP — the canonical academic source

CRSP is the gold standard. Documentation is unusually transparent for a paid product: the data dictionary, distribution code list, delisting code list, and calculation formulas are all on the public website (no login required) at <https://www.crsp.org/products/documentation>.

### 3.1 Identifier system

| Field | Type | Semantics |
|---|---|---|
| `PERMNO` | INT | Permanent security identifier. Survives ticker changes, exchange relistings, share-class reclassifications. Allocated once per security, never reused. |
| `PERMCO` | INT | Permanent company identifier. Multiple PERMNOs (one per share class) can roll up to a single PERMCO. |
| `HEXCD` / `EXCHCD` | INT | Header exchange / current exchange (1=NYSE, 2=AMEX, 3=Nasdaq, 4=Arca, …). |
| `HSICCD` | INT | Header SIC code. |
| `CUSIP` / `HCUSIP` | CHAR(8) | Current and header CUSIP-8 (CRSP truncates the check digit). |
| `TICKER` / `HTICK` | CHAR(5) | Current and header ticker. |

PERMNO is to CRSP what FSYM-S is to FactSet — the immutable spine that survives every corporate action.

### 3.2 The `dse` (Daily Stock Events) family

CRSP partitions equity events into separate tables, each keyed by `(PERMNO, DATE)`:

| Table | Contents |
|---|---|
| `dse` | Master daily stock events feed — joins of `dsedist`, `dsedelist`, `dsenames` for a given PERMNO. |
| `dsedist` | Distributions: cash dividends, stock dividends, splits, spinoffs, returns of capital, liquidations. |
| `dsedelist` | Delistings: terminal events, delisting return, last price source. |
| `dsenames` | Name history: company name, ticker, exchange, SIC code, share class — all with `NAMEDT` / `NAMEENDT`. |
| `dsi` | Daily Stock Indices — VWRETD, EWRETD, SPRETD, used for benchmark splice. |

(source: <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Data_Descriptions_Guide.pdf>; <https://wrds-www.wharton.upenn.edu/pages/grid-items/crsp-useful-variables/>)

### 3.3 `dsedist` field-level schema

```text
PERMNO       int      security id
DISTCD       int      6-digit distribution code (see §6 for enumeration)
DIVAMT       float    cash distribution per share, in dollars
FACPR        float    factor to adjust price on ex-date
FACSHR       float    factor to adjust shares-outstanding on ex-date
DCLRDT       date     declaration date
EXDT         date     ex-distribution date  (NOT NULL — the canonical 'when' for all calcs)
RCRDDT       date     record date
PAYDT        date     payment date
ACPERM       int      acquirer PERMNO (set for spin-off/merger distributions)
ACCOMP       int      acquirer PERMCO
```

(source: <https://terpconnect.umd.edu/~wermers/ftpsite/fnce7200/data_defs_061899.pdf>; <https://www.crsp.org/products/documentation/data-definitions-d>)

**Critical semantics:**
- `EXDT` is the only mandatory date; CRSP backfills `DCLRDT`/`RCRDDT`/`PAYDT` when known.
- For a 2-for-1 split, `FACPR = FACSHR = 1.0` (price halves, shares double; the factor expresses *additional* shares per old share).
- For a cash dividend with `DISTCD = 1232`, `FACPR = DIVAMT / PRC_{exdt-1}` and `FACSHR = 0`.
- For a spinoff, `FACPR != FACSHR` (price drops by spinoff value; shares unchanged in the parent — the spinoff issues a *new* PERMNO).
- The cumulative factors `CUMFACPR` / `CUMFACSHR` are precomputed daily and rebased to 1.0 at the most recent date.

### 3.4 `dsedelist` schema

```text
PERMNO       int
DLSTDT       date     delisting date (= last trade date for the security)
DLSTCD       int      delisting code (see §7 for full enumeration)
NWPERM       int      new PERMNO (set for merger; PERMNO of the surviving entity)
NWCOMP       int      new PERMCO
NEXTDT       date     next available pricing date after DLSTDT (when known)
DLAMT        float    delisting amount (last value to holder; cash + new security value)
DLRETX       float    delisting return ex-distributions
DLPRC        float    delisting price (may be negative if computed as bid/ask midpoint when no trade)
DLRET        float    delisting return — the canonical terminal return
DLPDT        date     date the delisting amount was paid/realised
```

(source: <https://www.crsp.org/products/documentation/data-definitions-d>; <https://www.crsp.org/products/documentation/delisting-codes>)

**`DLRET` semantics:** the return from the last available pricing date to the delisting amount. For a merger paying $50 cash with last close $48, `DLRET = (50 - 48) / 48 ≈ 4.2 %`. For a bankruptcy where the security is declared worthless and there is no opportunity to trade after delisting, `DLRET = -1.0` (i.e., -100%). The Shumway (1997) paper documents that omitting `DLRET` from the return series produces a **35% upward bias** on small-cap performance studies (source: <https://www.tylergshumway.org/Shumway-DelistingBiasCRSP-1997.pdf>).

### 3.5 `dsenames` schema

```text
PERMNO       int
NAMEDT       date     start of validity for this name row
NAMEENDT     date     end of validity (or end-of-database for current)
NCUSIP       char(8)  CUSIP-8 during this name window
TICKER       char(5)
COMNAM       char(32) company name
SHRCLS       char(1)  share class ('A','B','C',…; NULL = single class)
TSYMBOL      char(6)  trading symbol
NAICS        int
PRIMEXCH     char(1)  primary exchange ('N','A','Q',…)
TRDSTAT      char(1)  trading status
SECSTAT      char(1)  security status
```

A single PERMNO can have dozens of `dsenames` rows over its lifetime — e.g., GOOG's PERMNO has rows spanning the 2014 Class C split where the original PERMNO renamed to GOOGL and a new PERMNO 14542 was minted for GOOG (Class C). `NAMEDT`/`NAMEENDT` form the bitemporal validity period. This is the cleanest model of name-history and ticker-history in the industry; ats-eqt's `name_history` / `ticker_history` schema in §9 mirrors it.

### 3.6 The CRSP/Compustat Merged (CCM) `ccmxpf_lnkhist` link

The bridge between Compustat's GVKEY and CRSP's PERMNO is the heart of academic empirical finance. The table:

```text
GVKEY        char(6)  Compustat company key
LPERMNO      int      CRSP PERMNO
LPERMCO      int      CRSP PERMCO
LINKTYPE     char(2)  LC=primary,LU=usable,LS=secondary,LX=foreign,LD=duplicate,LN=no-match,NU=not-usable
LINKPRIM     char(1)  P=primary,J=joint,C=conditional,N=not-primary
LIID         char(3)  issue id
LINKDT       date     start of link validity
LINKENDDT    date     end of link validity (NULL = current)
```

(source: <https://www.kaichen.work/?p=138>; <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-crsp-with-compustat/>; <https://iangow.github.io/far_book/identifiers.html>)

The standard research filter is `LINKTYPE IN ('LC','LU') AND LINKPRIM IN ('P','C')`. The older WRDS-created `ccmxpf_linktable` is deprecated for a 1-year transition; new code should use `ccmxpf_lnkhist` directly (source: search results 2026-05; see <https://www.kaichen.work/?p=138>).

### 3.7 The `dsi` index file (splice-points for benchmarks)

`dsi` provides daily index values that share the same splice discipline as security prices: `VWRETD` (value-weighted return with dividends), `EWRETD` (equal-weighted), `SPRETD` (S&P 500 with dividends). For benchmark-relative metrics (alpha, IR, tracking error), pulling `VWRETD` instead of computing your own from individual securities is the right call — CRSP has already done the splice-point arithmetic on every constituent.

---

## 4. FactSet — Adjustments Feed + FSYM lineage

FactSet's corporate-action surface is *not* sold as a standalone product; it is bundled into the FactSet Adjustments Feed and into the Symbology API's lineage tables. Reference: `../vendors/factset.md` §4 (corporate-action handling).

### 4.1 The FactSet Adjustments Feed

Per the FactSet Marketplace catalog, the Adjustments Feed delivers price/share adjustment factors per FSYM-R per date, derived from the underlying corporate-action capture pipeline (source: <https://www.factset.com/marketplace/catalog>). The schema, at the field level:

```text
fsym_id            char(8)   FSYM regional id (-R suffix); the canonical join key
adj_date           date      ex-date of the action triggering this adjustment
p_split_factor     float     cumulative price split factor through this date
p_spinoff_factor   float     cumulative price spinoff factor
p_div_factor       float     cumulative dividend reinvestment factor
s_split_factor     float     cumulative share split factor
adj_currency       char(3)   currency of the dividend component
```

`[unverified — exact column names from the FactSet schema dictionary; FactSet does not publish the DataFeed schema openly. Confirmed conceptual presence via the FactSet Marketplace product page only.]`

### 4.2 FSYM-CA (the corporate-action event table)

The Symbology Reference catalogue includes `FSYM_CA` (Corporate Action lineage table) which records: action_type (M&A, spinoff, name change, ticker change, share class change, listing change), `fsym_id_old`, `fsym_id_new`, `effective_date`, `ratio_num`, `ratio_den`, and `source_filing_id`. `[unverified — referenced in FactSet developer docs but full field list is gated behind subscriber login at <https://developer.factset.com/api-catalog/symbology-api>.]`

The key FactSet design choice: **FSYM-E (entity-level FSYM) survives M&A**; the surviving entity inherits both FSYM-E and lineage records pointing back to the absorbed target's old FSYM-E. **FSYM-S/-R/-L are minted fresh on spinoff** for the spunoff security; the parent's IDs are unchanged. This is exactly the model ats-eqt should mirror — entity-level IDs are sticky, security-level IDs are minted per share class.

### 4.3 Daily-bar adjustment fields (`FF_*`)

The Fundamentals tables themselves carry adjustment factors at the row level:

```text
FF_ADJ_FACTOR        float  per-period cumulative price adjustment factor [unverified — referenced in FactSet pricing recipes; not in any public schema]
FF_PRICE_ADJ_DATE    date   last date a corporate action was applied      [unverified]
```

These exist in the FactSet pricing-data tables for downstream backtest joins; the exact column names are not in any FactSet-published document I could verify and are flagged accordingly.

---

## 5. Bloomberg — corporate actions on Terminal + B-PIPE

Bloomberg's corporate-action surface spans three places: terminal functions (DVD, EQY_SPLIT, CACS, IPO), the BDH/BDS Excel formulas, and the Bloomberg Per Security data feed for enterprise integration.

### 5.1 Dividend field family

The canonical bulk-history function is `DVD_HIST_ALL`, called as `=BDS(ticker, "DVD_HIST_ALL", "Hdr=Y")` in Excel, which returns one row per dividend event with these columns (source: <https://www.emich.edu/cob/programs/finance/flc/documents/formula-method-excel.pdf>; <https://sdmfsa.gitlab.io/latrousse/bloomberg/hist/dvd/>):

```text
Declared Date            date   declaration of the dividend
Ex-Date                  date   ex-dividend date (DVD_HIST_ALL_EX_DT)
Record Date              date   record date
Payable Date             date   payable date
Dividend Amount          float  per-share gross amount
Dividend Frequency       text   Regular Cash, Special Cash, 1st Interim, Final, …
Dividend Type            text   Cash Dividend / Stock Dividend / Spinoff / Stock Split / …
Dividend Currency        char3
```

Related single-value override fields used in BDH/BDP:

- `DVD_EX_DT` — most recent ex-date.
- `DVD_AMT_GROSS` / `DVD_AMT_NET` — gross / net dividend amount per share.
- `EQY_DVD_HIST_GROSS_DIV` — historical gross dividend total over a period.
- `DPDF` — Display Price as Decimal Field; *not* a corporate-action field per se but commonly confused with adjustment toggles.

### 5.2 Split fields

```text
SPLIT_DT             date    most recent split ex-date
SPLIT_FACTOR         float   ratio (e.g., 2.0 for 2-for-1, 0.5 for 1-for-2 reverse)
EQY_SPLIT_HIST       table   full split history via BDS
```

### 5.3 M&A and tender

The Bloomberg Mergers & Acquisitions function (`MA`) and the tender-flag fields populate corporate-event records:

```text
ACQ_ANNCMT_DT        date    acquisition announcement date
ACQ_CLOSE_DT         date    expected/actual close
TENDER_FLAG          bool    TRUE if a tender offer is active
M_A_STATUS           text    Pending / Closed / Withdrawn
M_A_TRANS_VALUE      float   announced deal value
```

`[unverified — field names from third-party scraping documentation, not from a Bloomberg-published spec; the Bloomberg Terminal API field dictionary is access-controlled.]`

### 5.4 Bloomberg DPDF override and adjusted-price gotcha

A long-standing Terminal gotcha: dividend adjustments are *off by default* in many HDH historical-price functions until the user toggles via `DPDF`. Researchers pulling price history through BDH without checking `DPDF` settings get an inconsistent series. Best practice for ats-eqt ingestion from a Bloomberg snapshot is to pull *unadjusted* prices and apply ats-eqt's own adjustment chain at query time.

### 5.5 DTCC integration

Bloomberg's primary upstream for North American corporate actions is the DTCC CA Web feed, augmented by issuer press releases and exchange notices (per Bloomberg's enterprise data fact sheets). This means Bloomberg's coverage tracks DTCC's CAEV taxonomy underneath their proprietary field names — useful when reconciling.

---

## 6. The CRSP distribution code (`DISTCD`) reference

`DISTCD` is a 6-digit code; CRSP's official enumeration is at <https://www.crsp.org/products/documentation/distribution-codes>. The first digit is the high-level category; subsequent digits encode frequency, payment type, and tax treatment.

### 6.1 High-level categories

| First digit | Category |
|---|---|
| 1 | Cash dividends |
| 2 | Cash dividends — special / non-recurring |
| 3 | Stock dividends |
| 4 | Rights distributions |
| 5 | Stock splits |
| 6 | Liquidations / structural events (shares-outstanding change with no distribution) |
| 7 | Spinoffs |
| 8 | Reorganisations |
| 9 | Other |

### 6.2 Common 4-digit codes (cash dividends, `1***`)

```
1212    Quarterly cash dividend, regular, taxable
1218    Quarterly cash dividend, regular, non-taxable
1222    Semi-annual cash dividend, regular, taxable
1232    Annual cash dividend, regular, taxable
1242    Monthly cash dividend, regular, taxable
1252    Cash dividend, frequency missing or unknown
1262    Special cash dividend (extra, year-end, non-recurring)
1272    Final cash dividend (terminal, before delisting)
1282    Return of capital / partial liquidation cash distribution
1292    Cash distribution from spinoff partial proceeds
```

### 6.3 Stock distributions (`3***`)

```
3232    Stock dividend (additional shares of same class)
3245    Stock dividend paid in different class shares
3522    Split (2-for-1 typical case)
3525    Split paid in different class shares
3565    Reverse stock split
```

### 6.4 Splits (`5***`)

```
5523    Stock split, forward, share-issuing
5531    Stock split, reverse
5571    Stock split paid in different class
5599    Other stock split
```

### 6.5 Liquidations / shares-outstanding events (`6***`)

```
6225    Cash dividend amount only (information code)
6320    Treasury shares retired
6420    Shares issued (general)
6525    Liquidation, partial
6531    Liquidation, final
```

### 6.6 Spinoffs (`7***`)

```
7232    Spinoff with new shares of a different security
7245    Spinoff with cash + shares mix
7525    Spinoff in same parent class
```

### 6.7 Reorganisations (`8***`)

```
8225    Merger consideration paid in cash
8232    Merger consideration paid in acquirer stock
8245    Merger consideration paid in mixed cash + stock
8525    Conversion, voluntary (e.g., preferred → common)
8531    Conversion, mandatory
```

**Caveats:**
- The above enumeration is *typical* CRSP usage and matches the published `distribution-codes` page conceptually, but the canonical source is the live CRSP page; some codes have been added/retired across CRSP versions. ats-eqt should ingest the live enumeration as a slowly-changing dimension rather than hardcode it.
- The most common code in any real dataset is `1232` (regular quarterly cash dividend) and `5523` (forward stock split); together these account for ~90% of distribution-events in the database per CRSP's own marketing.
- Several CRSP guides flag the 6225 code as a *special* informational code that records the dividend amount without triggering a price/share adjustment factor (source: search-result excerpt from CRSP documentation, 2026-05).

---

## 7. The CRSP delisting code (`DLSTCD`) reference

`DLSTCD` is a 3-digit code. The first digit is the high-level reason; the second digit refines the cause; the third digit (often 0 or 1) is a residual classifier. Official enumeration: <https://www.crsp.org/products/documentation/delisting-codes>.

### 7.1 High-level categories

| First digit | Range | Meaning |
|---|---|---|
| 1 | 100 | Active (security is still trading; included in many `dsedelist` rows as a sentinel) |
| 2 | 200-299 | **Merger** — security delisted because issuer was acquired |
| 3 | 300-399 | Exchange (less common; exchange of one security for another) |
| 4 | 400-499 | Liquidation / wind-up (voluntary) |
| 5 | 500-599 | **Dropped by exchange** (performance, listing-standard breach, bankruptcy) |

### 7.2 Mergers (`2**`)

```
200    Acquired by another company; method unknown
201    Acquired by domestic parent, payment in stock
202    Acquired by domestic parent, payment in cash
203    Acquired by domestic parent, payment in stock + cash
210    Acquired by foreign parent
220    Acquired in tender offer (cash)
231    Acquired in tender offer (stock)
241    Acquired, post-tender squeeze-out at lower price
261    Acquired in privately-negotiated transaction
```

### 7.3 Exchanges (`3**`)

```
300    Exchanged for another security in same firm
301    Share-class collapsed (e.g., dual-class to single class)
331    Conversion of preferred / convertible
```

### 7.4 Liquidations (`4**`)

```
400    Issuer liquidated, distribution to shareholders
410    Liquidation in bankruptcy reorganisation (cash)
420    Liquidation distribution non-cash
460    Wind-up complete, no final distribution recorded
490    Other liquidation
```

### 7.5 Dropped by exchange (`5**`) — the survivorship-critical set

```
500    Reason missing or not classified
501    Insufficient capital, surplus, or equity
502    Insufficient (or non-compliance with rules of) float or assets
503    Price fell below acceptable level
504    Insufficient number of market makers
505    Insufficient number of shareholders
510    Voluntary delisting (no specific reason)
513    Pending delisting and trading suspended
520    Bid/ask quote not available
551    Did not meet exchange financial guidelines
552    Did not meet exchange financial guidelines (continuance)
560    Bankruptcy declared, security delisted
561    Bankruptcy, Chapter 11 emerged
562    Bankruptcy, Chapter 7 dissolution
570    Delinquent in filing, non-payment of fees
574    Bankruptcy / suspension followed by delisting
580    Failed to register securities
584    Failed to file required reports
```

(source: <https://www.crsp.org/products/documentation/delisting-codes>; <https://sites.google.com/site/richardaprice3/research/delistings>; <https://www.tylergshumway.org/Shumway-DelistingBiasCRSP-1997.pdf>)

**Practitioner notes:**
- 51% of CRSP delistings are mergers (`2**`) — by far the dominant class (source: search-result excerpt from CRSP delisting summary).
- `5**` codes are the survivorship-bias generators: small-cap stocks delisted under 500-series for performance reasons typically have `DLRET ≈ -100%` if no opportunity to trade after the delisting date.
- Shumway (1997) showed that imputing **-30%** as the delisting return for missing-`DLRET` rows with `DLSTCD` in 500-585 produces the most accurate empirical results — CRSP itself adopted this convention in later vintages.

---

## 8. Total-return calculation walk-through

A worked example using CRSP primitives. Suppose security PERMNO 14593 (a hypothetical large-cap) has the following `dsedist` rows in 2024:

| Date       | DISTCD | DIVAMT | FACPR | FACSHR | Note            |
|------------|--------|--------|-------|--------|-----------------|
| 2024-02-08 | 1212   | 0.24   | 0.0   | 0.0    | Q1 cash div     |
| 2024-05-09 | 1212   | 0.24   | 0.0   | 0.0    | Q2 cash div     |
| 2024-06-10 | 5523   | 0.0    | 1.0   | 1.0    | 2-for-1 split   |
| 2024-08-08 | 1212   | 0.12   | 0.0   | 0.0    | Q3 cash div (post-split: 0.24 / 2) |
| 2024-11-07 | 1212   | 0.12   | 0.0   | 0.0    | Q4 cash div     |

Daily prices:
- 2024-02-07 close: $200.00; 2024-02-08 open: $199.76 (= 200 - 0.24)
- 2024-06-07 close: $250.00; 2024-06-10 open: $125.00 (post-split)
- 2024-12-31 close: $130.00

### 8.1 Cumulative factors

`CUMFACPR` for date `d` is the product of all `(1 + FACPR_i)` for events with `EXDT > d`. After all 2024 events:

```
CUMFACPR (pre-2024) = (1 + 1.0) = 2.0   -- the split is the only event that moves it
CUMFACPR (post-2024-06-10) = 1.0
```

`CUMFACSHR` follows the same pattern (1.0 → 2.0 across the split).

### 8.2 Adjusted price series

`ADJPRC_d = PRC_d / CUMFACPR_d`. For 2024-02-07, `ADJPRC = 200 / 2.0 = 100.00` (i.e., we restate pre-split prices in post-split share units).

### 8.3 Daily returns

For 2024-06-10 (the split date), the naïve `(PRC_t - PRC_{t-1}) / PRC_{t-1} = (125 - 250) / 250 = -50%` is wrong. The correct CRSP formula:

```
RET = (PRC_t / FACPR_split + DIVAMT_t / CUMFACPR_t / FACPR_t) / PRC_{t-1} - 1
    = (125 * 2 + 0) / 250 - 1
    = 0.0
```

i.e., zero return on a split day (as expected — the split is value-neutral).

For 2024-02-08 (cash dividend):

```
RET = (199.76 + 0.24) / 200.00 - 1 = 0.0
```

i.e., zero return on the ex-dividend day if the price drops by exactly the dividend. In practice prices drop by ~0.7–0.9× the dividend due to tax effects (the Elton-Gruber result), and `RET` captures that residual.

### 8.4 Annual total return

The annual total return from 2024-01-01 to 2024-12-31:

```
ANN_RET = ADJPRC_{2024-12-31} / ADJPRC_{2024-01-01} + DIV_RECONS
        = (130 / 1.0) / (200 / 2.0) - 1 + (0.24 + 0.24 + 0.12 + 0.12) / 100
        = 1.30 - 1 + 0.0072
        = 30.72 %
```

The sum-of-`RET` daily compounding gives the same result modulo arithmetic precision.

### 8.5 Terminal delisting return

If the security were merged on 2024-12-15 for $128 cash:

```
DLRET = (DLAMT - PRC_{last_trade}) / PRC_{last_trade}
      = (128 - 127) / 127     [say last trade was at $127]
      = 0.787 %
```

The annual total return then truncates at 2024-12-15 and the December return slot is replaced by `DLRET`.

---

## 9. Recommended ats-eqt schema

Bitemporal long-format, mirroring `schemas/data_models_and_methodology.md` section D and section G.5.

### 9.1 Event header

```sql
CREATE TABLE corp_action (
  action_id            BIGINT       PRIMARY KEY,
  security_id          BIGINT       NOT NULL,     -- → security (ats-eqt internal stable key)
  entity_id            BIGINT       NULL,         -- → entity (issuer); for entity-level events
  type_code            INTEGER      NOT NULL,     -- → corp_action_type_dim.type_code
  -- The five canonical dates
  declaration_date     DATE         NULL,
  ex_date              DATE         NOT NULL,     -- the only mandatory date (mirrors CRSP EXDT)
  record_date          DATE         NULL,
  payment_date         DATE         NULL,
  effective_date       DATE         NOT NULL,     -- the legal effective date (= ex_date for distributions; = close_date for M&A)
  -- Value and factor primitives
  value_decimal        NUMERIC(20,8) NULL,        -- DIVAMT for cash; tender price for M&A
  value_currency       CHAR(3)      NULL,
  price_factor         NUMERIC(20,10) NULL,       -- FACPR (multiplier applied to PRE-event prices)
  share_factor         NUMERIC(20,10) NULL,       -- FACSHR
  ratio_num            NUMERIC(20,10) NULL,       -- exchange ratio numerator (e.g., 2 in 2-for-1)
  ratio_den            NUMERIC(20,10) NULL,       -- denominator
  -- Counter-party (for M&A, spinoff)
  counterparty_security_id  BIGINT  NULL,
  counterparty_entity_id    BIGINT  NULL,
  -- Provenance
  source_id            INTEGER      NOT NULL,     -- 1=CRSP, 2=FactSet, 3=Bloomberg, 4=Compustat, 5=DTCC, 6=NYSE, 7=Nasdaq, 8=EDGAR
  source_event_id      TEXT         NULL,         -- vendor's primary key (e.g., DISTCD bucket, DTCC CA ID)
  filing_id            BIGINT       NULL,         -- → filing (EDGAR / 8-K backing reference)
  -- Lifecycle
  status               CHAR(1)      NOT NULL,     -- A=announced, C=confirmed, X=cancelled, W=withdrawn, F=final
  -- Bitemporal
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity'
);

CREATE INDEX ix_corp_action_security_ex ON corp_action(security_id, ex_date);
CREATE INDEX ix_corp_action_entity_ex   ON corp_action(entity_id, ex_date);
CREATE INDEX ix_corp_action_type        ON corp_action(type_code, ex_date);
```

### 9.2 Type dimension (the `distcd` mirror)

```sql
CREATE TABLE corp_action_type_dim (
  type_code         INTEGER      PRIMARY KEY,     -- 6-digit code, CRSP-aligned
  category          CHAR(1)      NOT NULL,        -- 'D' dividend, 'S' split, 'P' spinoff, 'M' merger,
                                                  -- 'L' liquidation, 'N' name/ticker change, 'I' IPO,
                                                  -- 'O' offering, 'R' rights, 'E' exchange, 'X' other
  sub_category      VARCHAR(40)  NOT NULL,        -- 'regular_quarterly_cash', 'special_cash', 'forward_split', …
  description       TEXT         NOT NULL,
  crsp_distcd       INTEGER      NULL,            -- CRSP source code if mirrored
  dtcc_caev         CHAR(4)      NULL,            -- DTCC CAEV code if mirrored (e.g., DVCA, SPLF, SOFF)
  bloomberg_type    VARCHAR(40)  NULL,            -- Bloomberg dividend-type string
  factset_type      VARCHAR(40)  NULL,            -- FactSet adjustment-type label
  affects_price     BOOLEAN      NOT NULL,        -- TRUE if FACPR > 0
  affects_shares    BOOLEAN      NOT NULL,        -- TRUE if FACSHR > 0
  taxable           BOOLEAN      NULL,
  mandatory         BOOLEAN      NOT NULL DEFAULT TRUE   -- FALSE for voluntary actions (tender, conversion)
);
```

Seed with the CRSP `distcd` enumeration from §6, plus DTCC CAEV codes (DVCA cash distribution, DVSE stock distribution, SPLF split, SOFF spinoff, MRGR merger, TEND tender, BIDS buy-back, CHAN name/ticker change, LIQU liquidation, WRTH write-off, BPUT put redemption).

### 9.3 Adjustment-factor materialisation

```sql
CREATE TABLE adjustment_factor (
  security_id          BIGINT       NOT NULL,
  date                 DATE         NOT NULL,
  cum_price_factor     NUMERIC(20,10) NOT NULL,    -- product of (1 + FACPR_i) for all events EXDT > date
  cum_share_factor     NUMERIC(20,10) NOT NULL,
  cum_div_factor       NUMERIC(20,10) NOT NULL,    -- for total-return adjusted close
  last_event_id        BIGINT       NULL,          -- → corp_action that most recently re-based this row
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (security_id, date, knowledge_from)
);
```

A nightly job rebuilds this per security: scan all `corp_action` rows ordered by `ex_date DESC` and multiply forward. The materialised view backs the adjusted-price series at query time.

### 9.4 Delisting events

```sql
CREATE TABLE delisting (
  security_id          BIGINT       PRIMARY KEY,
  delist_date          DATE         NOT NULL,     -- = CRSP DLSTDT
  delist_code          INTEGER      NOT NULL,     -- → delist_code_dim
  delist_reason        TEXT         NULL,
  next_pricing_date    DATE         NULL,         -- CRSP NEXTDT
  delist_amount        NUMERIC(20,4) NULL,        -- CRSP DLAMT
  delist_price         NUMERIC(20,4) NULL,        -- CRSP DLPRC
  delist_return        NUMERIC(10,6) NULL,        -- CRSP DLRET; -1.0 for worthless
  delist_return_ex_div NUMERIC(10,6) NULL,        -- CRSP DLRETX
  delist_pay_date      DATE         NULL,         -- CRSP DLPDT
  successor_security_id BIGINT      NULL,         -- for merger continuity
  source_id            INTEGER      NOT NULL,
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity'
);

CREATE TABLE delist_code_dim (
  delist_code          INTEGER      PRIMARY KEY,     -- 3-digit, CRSP-aligned
  category             CHAR(1)      NOT NULL,        -- M=merger, X=exchange, L=liquidation, D=dropped, A=active
  description          TEXT         NOT NULL,
  performance_related  BOOLEAN      NOT NULL         -- TRUE for 500-series
);
```

### 9.5 Name and ticker history (bitemporal, mirrors `dsenames`)

```sql
CREATE TABLE name_history (
  security_id          BIGINT       NOT NULL,
  valid_from           DATE         NOT NULL,
  valid_to             DATE         NOT NULL,        -- '9999-12-31' for current
  company_name         TEXT         NOT NULL,
  share_class          CHAR(2)      NULL,            -- 'A','B','C', NULL
  exchange_code        CHAR(2)      NOT NULL,
  sic_code             INTEGER      NULL,
  naics_code           INTEGER      NULL,
  trading_status       CHAR(1)      NULL,
  security_status      CHAR(1)      NULL,
  source_id            INTEGER      NOT NULL,
  source_action_id     BIGINT       NULL,            -- → corp_action that triggered the change
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (security_id, valid_from, knowledge_from)
);

CREATE TABLE ticker_history (
  security_id          BIGINT       NOT NULL,
  ticker               VARCHAR(12)  NOT NULL,
  valid_from           DATE         NOT NULL,
  valid_to             DATE         NOT NULL,
  primary_exchange     CHAR(2)      NOT NULL,
  source_id            INTEGER      NOT NULL,
  source_action_id     BIGINT       NULL,
  PRIMARY KEY (security_id, ticker, valid_from)
);
```

### 9.6 Spinoff cost-basis allocation (Form 8937)

```sql
CREATE TABLE spinoff_basis_allocation (
  action_id              BIGINT       PRIMARY KEY,    -- → corp_action (the spinoff event)
  parent_security_id     BIGINT       NOT NULL,
  spinoff_security_id    BIGINT       NOT NULL,
  ex_date                DATE         NOT NULL,
  -- IRC §358 basis allocation
  parent_basis_pct       NUMERIC(6,4) NOT NULL,       -- e.g., 0.7234 = 72.34% remains with parent
  spinoff_basis_pct      NUMERIC(6,4) NOT NULL,       -- e.g., 0.2766
  parent_fmv             NUMERIC(20,4) NULL,          -- fair-market value of parent share on ex-date
  spinoff_fmv            NUMERIC(20,4) NULL,          -- fair-market value of spinoff share on ex-date
  measurement_date       DATE         NULL,           -- when FMVs were measured (often ex-date or first trading day)
  ratio_num              NUMERIC(20,10) NOT NULL,     -- shares of spinoff per share of parent
  ratio_den              NUMERIC(20,10) NOT NULL,
  -- Provenance: Form 8937 attachment
  form_8937_url          TEXT         NOT NULL,
  filing_id              BIGINT       NULL,           -- → filing (the 8-K Item 8.01 wrapping the 8937)
  issuer_ein             CHAR(10)     NULL,
  -- Bitemporal
  knowledge_from         TIMESTAMP    NOT NULL,
  knowledge_to           TIMESTAMP    NOT NULL DEFAULT 'infinity'
);
```

### 9.7 IPO / secondary-offering reference

```sql
CREATE TABLE offering (
  offering_id            BIGINT       PRIMARY KEY,
  security_id            BIGINT       NOT NULL,
  offering_type          CHAR(2)      NOT NULL,       -- 'IP'=IPO, 'FO'=follow-on, 'PP'=private placement, 'AR'=at-the-market
  pricing_date           DATE         NOT NULL,
  first_trade_date       DATE         NOT NULL,
  shares_offered         BIGINT       NOT NULL,
  offer_price            NUMERIC(20,4) NOT NULL,
  proceeds_gross         NUMERIC(20,2) NULL,
  lead_underwriter       TEXT         NULL,
  prospectus_filing_id   BIGINT       NULL,           -- → filing (S-1 / 424B)
  exchange_code          CHAR(2)      NOT NULL,
  ticker                 VARCHAR(12)  NOT NULL,
  source_id              INTEGER      NOT NULL,
  knowledge_from         TIMESTAMP    NOT NULL,
  knowledge_to           TIMESTAMP    NOT NULL DEFAULT 'infinity'
);
```

### 9.8 Trading halts

```sql
CREATE TABLE trading_halt (
  halt_id                BIGINT       PRIMARY KEY,
  security_id            BIGINT       NOT NULL,
  exchange_code          CHAR(2)      NOT NULL,
  halt_code              CHAR(2)      NOT NULL,       -- Nasdaq halt code (T1, T2, LUDP, …)
  halt_start             TIMESTAMP    NOT NULL,
  halt_end               TIMESTAMP    NULL,           -- NULL = still halted at knowledge time
  reason_text            TEXT         NULL,
  source_id              INTEGER      NOT NULL,
  knowledge_from         TIMESTAMP    NOT NULL,
  knowledge_to           TIMESTAMP    NOT NULL DEFAULT 'infinity'
);
```

Reference the Nasdaq Trader halt-codes table (source: <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltCodes>) and the public RSS feed (source: <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltRSS>) as the upstream.

---

## 10. Public-data reconstruction paths

For each commercial action type, what can be sourced free from EDGAR / DTCC / exchanges, and what cannot.

### 10.1 Cash dividends

- **EDGAR Form 8-K Item 8.01 (Other Events).** Issuers commonly attach the press release announcing a dividend declaration as an 8-K 8.01. Full-text search at <https://efts.sec.gov/LATEST/search-index?q=%22declares+dividend%22&forms=8-K> yields the corpus.
- **EDGAR companyfacts.json (`us-gaap:DividendsPerShareDeclared`).** XBRL-tagged dividend-per-share, populated at quarterly reporting cadence. Available via <https://data.sec.gov/api/xbrl/companyfacts/CIK##########.json>. Captures the *declared* amount; not the ex-date directly (must be cross-referenced to the 8-K).
- **EDGAR companyfacts.json (`us-gaap:DividendsPaid`).** Cash-flow statement line, dollar amount paid in the period.
- **Form DEF 14A** for dividend-vote approvals (rare; most boards have unilateral dividend authority).
- **Exchange ex-dividend calendars.** NYSE and Nasdaq publish ex-date listings publicly (e.g., <https://www.nasdaq.com/market-activity/dividends>).
- **Gap:** dividend frequency and exact ex-date precision <D-1 require either the 8-K full text or the exchange feed; XBRL alone is insufficient.

### 10.2 Splits

- **EDGAR Form 8-K Item 5.03** (Amendments to Articles of Incorporation) — used for splits that require charter amendment (which is most forward splits where the authorised share count changes).
- **EDGAR Form 8-K Item 8.01** for stock-split announcements that don't require charter amendment.
- **XBRL tag `us-gaap:StockholdersEquityNoteStockSplitConversionRatio1`** and the `us-gaap:StockholdersEquityNoteStockSplit` text block — captures the ratio at quarterly reporting.
- **Form 10-Q / 10-K Subsequent Events footnote** often discloses splits announced post-period-end.
- **DTCC announcement file** (subscriber but partial public ISO 20022 sample messages at <https://www.dtcc.com/-/media/Files/Downloads/issues/Corporate-Actions-Transformation/ISO-20022-Messaging-for-ReorgInstr.pdf>).
- **Gap:** the ex-date and record-date are SEC-disclosed but not in a structured XBRL field — must be parsed from 8-K narrative.

### 10.3 Spinoffs

- **EDGAR Form 10-12B / 10-12B/A** — the spinoff registration statement, filed by the spinco. The exhibit list typically includes the separation-and-distribution agreement.
- **EDGAR Form 8-K Item 2.01** (Completion of Acquisition or Disposition of Assets) — filed by the parent on the distribution date.
- **EDGAR Form 8-K Item 8.01** wrapping a **Form 8937** (Report of Organizational Actions Affecting Basis of Securities) — the *only* public substrate for IRC §358 basis-allocation percentages. Issuers must furnish 8937 to shareholders *or* post on a public website for 10 years (source: <https://www.irs.gov/forms-pubs/about-form-8937>; <https://accountably.com/irs-forms/f8937/>).
- **EDGAR full-text search for "Form 8937"** at <https://efts.sec.gov/LATEST/search-index?q=%22Form+8937%22> surfaces hundreds of attachments per year.
- **Form S-1 / Form 10** for the spunoff entity's standalone registration.
- **Gap:** the §358 allocation percentages have no XBRL tag and must be parsed from PDF/HTML 8937 attachments — this is where vendors charge for cleaning.

### 10.4 M&A

- **EDGAR Form 8-K Item 1.01** — Material Definitive Agreement (the signed merger agreement).
- **EDGAR Form 8-K Item 2.01** — Completion of the acquisition.
- **EDGAR Form 425** — proxy communications related to the merger (source: <https://www.sec.gov/rules-regulations/staff-guidance/compliance-disclosure-interpretations/exchange-act-form-8-k>).
- **EDGAR Form DEFM14A / PREM14A** — definitive / preliminary merger proxy.
- **EDGAR Form SC TO-T / SC 14D9** — tender offer schedules.
- **EDGAR Form 15** — Termination of Registration; filed by the target after deal close to suspend reporting obligations.
- **Gap:** announced-vs-closed deal value, status (pending/closed/withdrawn) must be inferred from sequence of 8-K filings.

### 10.5 Name and ticker changes

- **EDGAR Form 8-K Item 5.03** — charter amendment is required for legal-name change.
- **EDGAR Form 8-K Item 8.01** — for ticker-only changes that don't require charter amendment.
- **EDGAR Form 8-K/A** amendments to prior filings sometimes record the operative ticker.
- **The EDGAR submissions JSON** at <https://data.sec.gov/submissions/CIK##########.json> includes a `name` and `tickers` field that is updated as the issuer's IR contact updates EDGAR.
- **OpenFIGI** preserves FIGI across ticker rename (source: <https://www.openfigi.com/assets/local/figi-allocation-rules.pdf>) — but does *not* publish a clean ticker-history time series.
- **Wikidata Q-IDs** carry a `ticker symbol (P249)` property with date qualifiers — sometimes well-curated for large-cap renames, sparse for small caps.

### 10.6 Delistings

- **EDGAR Form 25** — Notification of Removal from Listing. The exchange files Form 25 to delist a security; the issuer can also file 25.
- **EDGAR Form 15** — Termination of Registration; usually follows Form 25 by 90 days for issuers exiting SEC reporting entirely.
- **EDGAR Form 8-K Item 3.01** — Notice of Delisting or Failure to Satisfy a Continued Listing Rule.
- **Nasdaq Trader trading-halt search** at <https://www.nasdaqtrader.com/trader.aspx?id=tradinghaltsearch>.
- **EDGAR full-text search for "Form 25-NSE"** isolates exchange-initiated delistings.
- **Gap:** the equivalent of CRSP's `DLRET` (the realised value to holders at delisting) is not directly disclosed for performance-related delistings — must be computed from the last trade price and any subsequent OTC pink-sheet trading. For mergers, the consideration is in the merger 8-K and DEFM14A.

### 10.7 IPOs and secondary offerings

- **EDGAR Form S-1 / S-1/A** — initial registration; the prospectus contains shares offered, range, underwriters.
- **EDGAR Form 424B1 / 424B3 / 424B4 / 424B5** — final prospectus filed after pricing.
- **EDGAR Form FWP** — Free Writing Prospectus.
- **Nasdaq IPO Indicator** at <https://www.nasdaqtrader.com/trader.aspx?id=IPOIndicator>.
- **NYSE IPO calendar** at <https://www.nyse.com/ipo-center/calendar>.
- **EDGAR `Files` listing** for 424B filings is the cleanest pricing-date trigger.

### 10.8 Halt / resume

- **Nasdaq Trader Halt RSS** at <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltRSS> — free, real-time, no auth (source: <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltRSS>).
- **Nasdaq halt-code reference** at <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltCodes>.
- **NYSE Trader Updates** at <https://www.nyse.com/trader-update>.

### 10.9 Yahoo Finance — known-broken adjusted close

Yahoo computes adjusted close using CRSP-style split + dividend multipliers, *but* a long-standing inconsistency exists: Yahoo applies un-split-adjusted dividend amounts to split-adjusted close prices, producing over-modification for pre-split periods that also contain dividends (source: <https://github.com/joshuaulrich/quantmod/issues/253>; <https://www.bitget.com/wiki/does-yahoo-finance-adjust-for-stock-splits>; <https://www.quora.com/Why-are-charts-in-Yahoo-Finance-only-adjusted-by-splits-and-not-by-dividends>). ats-eqt **must compute its own** adjusted series from raw events.

### 10.10 Wikidata and FINRA OTC

- **Wikidata** carries `stock exchange (P414)`, `ticker symbol (P249)`, `successor (P156)`, and `dissolved date (P576)` properties with date qualifiers. Useful as a *secondary* validation source; not authoritative.
- **FINRA OTC Reporting Facility** publishes daily corporate-action notices for OTC issues (often missed by CRSP/Compustat); see <https://www.finra.org/filing-reporting/otc-transparency>.

---

## 11. DTCC CA 20022 service — the issuer-of-record substrate

DTCC's Corporate Actions Web (CA Web) and the CA 20022 messaging service are the depository's view of corporate actions — closest to the issuer's transfer agent. Key facts (source: <https://www.dtcc.com/data-services/corporate-actions-and-reference-data/dtcc-ca-20022-service>; <https://www.dtcc.com/-/media/Files/Downloads/issues/Corporate-Actions-Transformation/Getting_Started_CA_ISO_20022.pdf>):

- Coverage: **~1.3 million active securities** with CA notifications.
- Delivery: real-time via IBM MQ *or* file format via NDM/FTP, **16 time slices per day**.
- Format: **ISO 20022 XML** (the `seev.*` family — `seev.031` for notification, `seev.033` for status, `seev.036` for movements/entitlements, `seev.037` for instruction-status, `seev.039` for cancellation).
- **CAEV codes** (corporate action event type), 4-char ISO 20022 enumeration. Key codes:

| CAEV | Meaning |
|---|---|
| `DVCA` | Cash dividend |
| `DVSE` | Stock dividend |
| `DVOP` | Dividend option (cash or stock) |
| `INTR` | Interest payment |
| `CAPG` | Capital gains distribution |
| `LIQU` | Liquidation |
| `WRTH` | Worthless write-off |
| `SPLF` | Stock split — forward |
| `SPLR` | Stock split — reverse |
| `SOFF` | Spinoff |
| `MRGR` | Merger |
| `TEND` | Tender offer |
| `BIDS` | Repurchase offer / Dutch auction |
| `CONV` | Conversion |
| `EXOF` | Exchange offer |
| `RHTS` | Rights distribution |
| `BPUT` | Mandatory put |
| `BIDD` | Disclosure dissemination |
| `CHAN` | Name / ticker / par-value / domicile change |
| `PARI` | Pari passu |
| `REDM` | Redemption |
| `MCAL` | Full call |
| `PCAL` | Partial call |

- **Mandatory vs voluntary distinction:** Cash dividends, splits, mergers (post-vote), and liquidations are mandatory (no holder action). Tender offers, conversions, rights, exchange offers are voluntary (holder elects). The schema flags this explicitly in `seev.031` at `CorpActnGnlInf/EvtPrcgTp`.

DTCC is the upstream that FactSet, Bloomberg, Refinitiv, and the wirehouses consume; reconstructing comparable coverage from EDGAR is impossible for purely book-entry events that don't trigger an SEC disclosure obligation.

---

## 12. NYSE / Nasdaq / ICE direct feeds

### 12.1 NYSE Corporate Actions

NYSE publishes a Corporate Actions data product (subscriber) and a public calendar at <https://www.nyse.com/markets/corporate-actions>. The exchange-level feed adds:
- listing changes (security moved from NYSE American to NYSE)
- new listings (IPO date alignment)
- distribution and split exchange-confirmation timestamps

### 12.2 Nasdaq Corporate Actions

Nasdaq offers a Global Market & Fund Activity Data product (source: <https://www.nasdaq.com/solutions/data/equities/corporate-action-solutions>) covering listings, delistings, dividends, and symbol changes **since 1998**. Public surfaces:
- **Nasdaq Trader 2026 Equity Corporate Actions Alert Index** at <https://www.nasdaqtrader.com/Trader.aspx?id=archiveheadlines&cat_id=105>
- **Halt RSS** at <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltRSS>
- **IPO Indicator Console** at <https://www.nasdaqtrader.com/trader.aspx?id=IPOIndicator>

### 12.3 EDI Worldwide Corporate Actions (WCA)

Exchange Data International (EDI) distributes a global corporate-action feed via Nasdaq Data Link as the `WCA` database: **4.5M+ records on 300,000+ securities from 100,000+ companies** with history to 2000 (source: <https://data.nasdaq.com/databases/WCA>; <https://www.exchange-data.com/product/adjustment-factors-data/>). This is a credible non-Big-Four global source, with ISO 20022-aligned event types.

### 12.4 ICE Connect

ICE's corporate-action surface is delivered via ICE Connect for the NYSE-listed universe and via ICE Data Services bulk feeds for global coverage. `[unverified — exact field-list and pricing not publicly disclosed]`

### 12.5 Databento / SIP-level

Databento offers a corporate-actions endpoint built on SIP-level data plus exchange announcements (source: <https://databento.com/corporate-actions>) — useful as a developer-friendly modern alternative to DTCC subscription.

---

## 13. Refinitiv / LSEG — Datastream and Workspace

LSEG's corporate-action surface is split across Datastream (the academic/quant front-end) and Workspace (the trading-floor terminal).

### 13.1 Datastream FACPR / AF

Datastream's `AF` (Adjustment Factor) static mnemonic gives a single cumulative factor per security per date. The historical mnemonic `X(PE)` is the adjusted price; `X(P)#S` toggles split-only vs split+dividend. (source: <https://www.bwl.uni-mannheim.de/media/Lehrstuehle/bwl/Maug/Database_info/Datastream_dataypes.pdf>)

### 13.2 WS-series corporate-action items

Datastream's Worldscope (`WS*`) item codes record fundamental-level dividend and split data:

```text
WS05101     Dividends per Share (annual, current period)
WS05201     Dividend yield
WS05376     Stock split factor (period)
WS05500     Cash dividends paid (total)
```

`[unverified — WS-item numbers are taken from third-party academic references; the canonical Worldscope item dictionary is subscriber-gated.]`

### 13.3 Refinitiv Equity Indices Corporate Action Methodology

LSEG publishes a public methodology for how its equity indices treat corporate actions — useful as a vendor's reference implementation (source: <https://www.refinitiv.com/content/dam/marketing/en_us/documents/methodology/corporate-actions-methodology.pdf>).

### 13.4 DSWS / LSEG Data Platform API

The Datastream Web Services (DSWS) and LSEG Data Platform API expose corporate-action endpoints under the `Pricing/Corporate Actions` namespace, returning JSON with action_type, ex_date, ratio, and value fields. Subscription required.

### 13.5 Eikon / Workspace Corporate Actions Calendar

LSEG Workspace has a Corporate Actions calendar function (`CAC` equivalent) showing announced and upcoming events filtered by region, action type, and watchlist.

---

## 14. S&P Capital IQ / Compustat — survivorship and the CCM link

Compustat's corporate-action handling is fundamentally different from CRSP's because Compustat is reporting-period-anchored (annual / quarterly) rather than event-anchored. Key fields (source: `../vendors/sp_global.md`; <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-crsp-with-compustat/>):

```text
AJEXM        Adjustment factor cumulative price-month (monthly)
AJEXQ        Adjustment factor cumulative price-quarter
DVPSP_F      Cash dividend per share (fiscal year)
DVPSX_F      Cash dividend per share, ex-special
DVT          Total dividends paid in fiscal year
CSHO         Common shares outstanding
CSHFD        Common shares fully diluted
```

### 14.1 Compustat survivorship — the silent failure mode

When a company is delisted or merged, **Compustat does not delete the row** but the trailing quarters report NULLs for financials until coverage formally ends. This is the survivorship gotcha: a naïve `WHERE fic = 'USA' AND fyear = 2023` query against Compustat will *include* recently-delisted issuers whose 2023 data is partial NULL. The recommended filter is `WHERE COALESCE(dlrsn, 'A') = 'A'` (i.e., include only `dlrsn` = active) **or** join through `ccmxpf_lnkhist` and use CRSP's `dsedelist` as the authoritative termination event.

### 14.2 `LINKTYPE` semantics revisited

```text
LC  Link valid, primary issue confirmed by CRSP and Compustat
LU  Link valid, primary issue (CRSP-confirmed only; "unresearched but plausible")
LS  Link valid, secondary issue (alternative share class)
LX  Link to foreign exchange listing
LD  Duplicate
LN  No-match marker (Compustat company has no CRSP coverage)
NU  Not usable (data quality flag)
```

The standard academic filter `LINKTYPE IN ('LC','LU') AND LINKPRIM IN ('P','C')` produces the cleanest GVKEY↔PERMNO bridge.

### 14.3 The Snapshot vs current vendor-of-record distinction

Compustat distinguishes a "current" view (latest restated values) from a "snapshot" view (point-in-time). For corporate-action purposes, the snapshot view captures the issuer-disclosed values at time of filing — useful when the post-restatement view would show a stale or revised split factor. ats-eqt's bitemporal pattern handles this naturally via the `knowledge_from/knowledge_to` columns on `corp_action`.

---

## 15. OpenFIGI — ticker-history reconstruction

OpenFIGI is the only free, MIT-licensed security-identifier system with strong corporate-action persistence semantics (source: <https://www.openfigi.com/assets/local/figi-allocation-rules.pdf>; <https://www.openfigi.com/about/features>):

- **FIGI is persistent.** Once assigned, never changes. If the instrument ceases to exist, the FIGI is retired and never reused.
- **Ticker changes do not mint a new FIGI.** The same Composite FIGI carries through a ticker rename; the underlying ticker is a separate attribute resolved at lookup time.
- **Share-class changes DO mint a new FIGI.** A new Composite FIGI is issued when an issuer reclassifies share classes or changes the underlying instrument's structure (par value, voting rights material change).
- **Share Class FIGI** (the top-of-hierarchy global identifier) aggregates Composite FIGIs across countries for cross-listed instruments.

This means OpenFIGI is **the** open ticker-history substrate when joined with EDGAR submissions JSON (which carries the canonical filer `tickers` field updated through corporate actions).

---

## 16. Section 358 / Form 8937 — the spinoff basis allocation public surface

IRC §358 governs the basis allocation between a parent and its spinoff for US tax purposes. The mechanic: shareholders allocate their pre-spinoff parent basis between the post-spinoff parent shares and the spinoff shares **in proportion to the relative fair market values on the distribution date** (source: <https://www.taxnotes.com/research/federal/usc26/358>).

IRC §6045B (added by the Energy Improvement and Extension Act of 2008) requires the *issuer* to file Form 8937 within **45 days of the action or by January 15 of the following year, whichever is earlier**. The issuer can either:
1. File Form 8937 with the IRS *and* furnish to each holder, **or**
2. Post the form on a publicly-accessible website for **10 years** from the action date.

Most large issuers choose option 2. The form is then a flat HTML or PDF page on `investor.<issuer>.com`. Many issuers also wrap the 8937 as an **8-K Item 8.01 (Other Events)** exhibit on EDGAR, which makes it discoverable via EDGAR full-text search.

### Worked example: hypothetical spinoff

If ParentCo trades at $100 and distributes 0.5 shares of SpinCo per ParentCo share, and SpinCo trades at $20 on the first regular-way day, then:

```
parent_fmv      = 100
spinoff_value   = 0.5 * 20 = 10
total_fmv       = 110
parent_basis_pct  = 100 / 110 = 0.9091  (90.91%)
spinoff_basis_pct = 10 / 110  = 0.0909  ( 9.09%)
```

A shareholder who owned 1000 shares of ParentCo with a $50/share basis ($50,000 total) would after the spinoff hold:
- 1000 shares of ParentCo, basis $50,000 × 0.9091 = $45,455 ($45.455/share)
- 500 shares of SpinCo, basis $50,000 × 0.0909 = $4,545 ($9.09/share)

The Form 8937 attachment publishes these percentages. ats-eqt's `spinoff_basis_allocation` table (§9.6) stores them.

---

## 17. Open questions / wave-3 gaps

1. **DTCC CA 20022 sample-message corpus.** Are the public DTCC ISO 20022 sample messages sufficient to build a development-time mock feed, or do we need a subscriber-test environment? **[Wave-3 task: enumerate the `seev.*` sample messages in the DTCC documentation pack and verify they cover all CAEV codes.]**
2. **CRSP vs DTCC distribution-code mapping.** Authoritative mapping from CRSP `distcd` to DTCC CAEV is not published publicly; WRDS may have an internal crosswalk. **[Wave-3 task: contact WRDS or extract empirically from a sample period where both feeds are available.]**
3. **FactSet `FF_ADJ_FACTOR` exact column name.** Used informally in FactSet pricing recipes but I could not locate it in any published spec; the schema may differ in the Snowflake share vs. the legacy DataFeed. **[Wave-3 task: request the FactSet Adjustments Feed data dictionary directly from FactSet sales.]**
4. **Bloomberg DVD_HIST_ALL_EX_DT vs DVD_EX_DT distinction.** Multiple academic references use both forms; the BLP API field dictionary is access-controlled. **[Wave-3 task: cross-reference against a Bloomberg-API'd account.]**
5. **CRSP `DLRET` imputation rules in recent vintages.** Shumway (1997) recommended -30% imputation for missing `DLRET` with `DLSTCD` in 500-585; CRSP's current default is unclear post the v2 / CIZ release. **[Wave-3 task: read the latest CRSP Calculations and Index Methodologies PDF cover-to-cover.]**
6. **OpenFIGI ticker-history endpoint.** Does OpenFIGI publish historical ticker time series, or only current+composite FIGI mapping? **[Wave-3 task: test the OpenFIGI mapping API with historical CUSIP queries.]**
7. **Form 8937 EDGAR full-text search precision.** Practitioner intuition is that ~80% of 8937s are on EDGAR (wrapped as 8-K 8.01); the other ~20% are issuer-website-only. **[Wave-3 task: sample a known set of 100 recent US spinoffs and compute EDGAR coverage.]**
8. **WCA (EDI) vs DTCC coverage diff.** What does EDI's 4.5M-record WCA cover that DTCC doesn't, and vice versa? Public comparison absent. **[Wave-3 task: requisition WCA trial data via Nasdaq Data Link.]**
9. **Compustat `dlrsn` exhaustive enumeration.** The `dlrsn` (delisting reason) code list isn't in any public Compustat document I could verify; vendors typically distribute it as part of the data dictionary. **[Wave-3 task: extract from a WRDS Compustat sample.]**
10. **Section 1.6045B vs Section 6045B nomenclature.** Treasury Regulation 1.6045B-1 implements IRC §6045B for Form 8937; ats-eqt documentation should be consistent. **[Wave-3 task: standardise on "Treas. Reg. §1.6045B-1" in user-facing copy.]**

---

## 18. Sources

### CRSP first-party
- <https://www.crsp.org/products/documentation> — CRSP documentation index
- <https://www.crsp.org/products/documentation/distribution-codes> — DISTCD enumeration
- <https://www.crsp.org/products/documentation/delisting-codes> — DLSTCD enumeration
- <https://www.crsp.org/products/documentation/data-definitions-d> — Data Definitions starting with D
- <http://www.crsp.com/products/documentation/crsp-calculations> — CRSP Calculations page
- <https://www.crsp.org/products/documentation/link-actions> — CRSP Link Actions
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Data_Descriptions_Guide.pdf> — Data Descriptions Guide (FIZ)
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Guide_Flat_File_Format_1.0.pdf> — Flat File Format 1.0 (SIZ)
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Guide_Flat_File_Format_2.0.pdf> — Flat File Format 2.0 (CIZ)
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_Calculations_and_Index_Methodologies.pdf> — Calculations & Index Methodologies
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_Cross_Reference_Guide_1.0_to_2.0.pdf> — SIZ-to-CIZ cross reference
- <https://www.crsp.org/wp-content/uploads/guides/CRSP10_Year_US_Stock_Database_Guide.pdf> — 10-year database guide
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_Compustat_Merged_Database_Guide.pdf> — CCM Database Guide
- <https://www.crsp.org/research/crsp-survivor-bias-free-us-mutual-funds/> — Survivor-bias-free mutual funds
- <https://leiq.bus.umich.edu/docs/crsp_calculations_splits.pdf> — Michigan reprint of CRSP Calculations chapter
- <https://leiq.bus.umich.edu/docs/crsp_factor_adjustment.pdf> — Michigan reprint of CRSP Factor Adjustment chapter
- <https://terpconnect.umd.edu/~wermers/ftpsite/fnce7200/data_defs_061899.pdf> — CRSP Data Definitions and Coding Schemes Guide (academic mirror)
- <https://wrds-www.wharton.upenn.edu/documents/400/CRSP_Programmers_Guide.pdf> — Programmers Guide

### CRSP/Compustat link
- <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-crsp-with-compustat/> — Linking CRSP with Compustat
- <https://wrds-www.wharton.upenn.edu/pages/grid-items/crsp-useful-variables/> — Useful Variables reference
- <https://www.kaichen.work/?p=138> — Kai Chen on CCM link table fields
- <https://www.ruidaiwrds.info/posts/crsp-compustat> — Rui Dai on CRSP-Compustat
- <https://gist.github.com/iangow/583557b7b91a87ee1e545aa839ccbb8d> — Ian Gow CRSP-Compustat merge gist
- <https://gist.github.com/iangow/fca4cb10b048f5c798113da7039c2688> — Comparison of three CCM link tables
- <https://iangow.github.io/far_book/identifiers.html> — Empirical Research in Accounting — Linking databases
- <https://mingze-gao.com/posts/merge-compustat-and-crsp/> — Mingze Gao merge guide
- <https://www.projectrhea.org/rhea/index.php/CRSP_compustat_merged_database_in_WRDS> — Project Rhea CCM
- <https://docs.nuvolos.com/user-guides/data-guides/working-with-crsp-and-compustat> — Nuvolos guide
- <https://www.tidy-finance.org/python/wrds-crsp-and-compustat.html> — Tidy Finance Python guide
- <https://www.tidy-finance.org/r/wrds-crsp-and-compustat.html> — Tidy Finance R guide

### Delisting bias and methodology
- <https://www.tylergshumway.org/Shumway-DelistingBiasCRSP-1997.pdf> — Shumway (1997) Delisting Bias in CRSP
- <https://sites.google.com/site/richardaprice3/research/delistings> — Richard Price's delisting reference
- <https://www.sciencedirect.com/science/article/abs/pii/S0165410106000930> — Delisting returns and accounting-based anomalies
- <https://eodhd.com/financial-academy/financial-faq/survivorship-bias-free-financial-analysis> — Survivorship-bias-free analysis
- <https://www.linkedin.com/pulse/crsp-data-definitions-ritika-dokania> — CRSP Data Definitions overview

### DTCC and ISO 20022
- <https://www.dtcc.com/data-services/corporate-actions-and-reference-data/dtcc-ca-20022-service> — DTCC CA 20022 Service product
- <https://www.dtcc.com/asset-services/corporate-actions-processing/iso-20022-messaging-specifications> — DTCC ISO 20022 Messaging Specs index
- <https://www.dtcc.com/-/media/Files/Downloads/issues/Corporate-Actions-Transformation/Getting_Started_CA_ISO_20022.pdf> — Getting Started CA ISO 20022
- <https://www.dtcc.com/-/media/Files/Downloads/issues/Corporate-Actions-Transformation/ISO-20022-Messaging-for-ReorgInstr.pdf> — Reorganizations ISO 20022
- <https://www.dtcc.com/-/media/Files/Downloads/issues/Corporate-Actions-Transformation/User_Guide_ISO_20022_Messaging_for_Instructions.pdf> — Distributions ISO 20022
- <https://www.dtcc.com/-/media/Files/Downloads/issues/Corporate-Actions-Transformation/ISO_20022_EntAlloc_UG.pdf> — Entitlement/Allocation ISO 20022
- <https://dtcclearning.com/products-and-services/dtcc-data-services/ca-20022-service.html> — DTCC Learning CA 20022
- <https://dtcclearning.com/products-and-services/asset-services/corporate-actions-processing/iso-20022-messaging.html> — DTCC ISO 20022 messaging learning
- <https://www.sifma.org/wp-content/uploads/2017/05/SIFMA-CAS_DTCC-Corporate-Actions-Update_2018.pdf> — SIFMA DTCC CA Product Update 2018

### IRS / Form 8937 / §358
- <https://www.irs.gov/forms-pubs/about-form-8937> — About Form 8937
- <https://www.irs.gov/pub/irs-pdf/i8937.pdf> — Form 8937 instructions
- <https://www.taxnotes.com/research/federal/usc26/358> — IRC §358 text
- <https://accountably.com/irs-forms/f8937/> — Form 8937 deadlines and posting rules
- <https://ourtaxpartner.com/irs-form-8937-organizational-actions-basis-guide/> — Form 8937 guide
- <https://www.bivio.com/site-help/bp/Cost_Basis_Adjustments_Form_8937_Help> — bivio cost-basis adjustments
- <https://www.reckitt.com/media/1651/indivior-spinoff-form-8937-us.pdf> — Reckitt/Indivior spinoff 8937 example
- <https://www.geaerospace.com/sites/default/files/ge-form-8937-attachment.pdf> — GE 8937 attachment example
- <https://s27.q4cdn.com/984876518/files/doc_downloads/Roblox-Form-8937-final-signed.pdf> — Roblox Form 8937
- <https://d1io3yog0oux5.cloudfront.net/_24b572ef2bad7fba6a620a886eef9564/3m/db/3266/30881/file/Form_8937_SOLVSpin.pdf> — 3M / Solventum 8937

### EDGAR / SEC
- <https://www.sec.gov/search-filings/edgar-application-programming-interfaces> — EDGAR APIs
- <https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data> — Accessing EDGAR data
- <https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide.pdf> — EDGAR XBRL Guide April 2026
- <https://www.sec.gov/rules-regulations/staff-guidance/compliance-disclosure-interpretations/exchange-act-form-8-k> — Form 8-K CDIs
- <https://www.sec.gov/files/form8-k.pdf> — Form 8-K specification
- <https://www.sec.gov/rules-regulations/2004/03/additional-form-8-k-disclosure-requirements-acceleration-filing-date> — 8-K 2004 amendments
- <https://www.cooley.com/news/insight/2004/sec-faqs-on-form-8k> — Cooley FAQs on Form 8-K
- <https://dart.deloitte.com/USDART/home/accounting/sec/sec-material-supplement/compliance-disclosure-interpretations/exchange-act-form-8-k> — Deloitte DART 8-K
- <https://www.wilmerhale.com/-/media/files/shared_content/editorial/publications/documents/20241217-keeping-current-with-form-8-k-a-practical-guide-2024-update.pdf> — WilmerHale 8-K practical guide
- <https://www.bassberrysecuritieslawexchange.com/acquisition-agreement-form-8k/> — Bass Berry on acquisition agreement 8-K
- <https://tldrfiling.com/blog/free-sec-edgar-api-guide/> — SEC EDGAR API guide
- <https://dealcharts.org/blog/sec-edgar-api-guide> — DealCharts EDGAR guide
- <https://edgartools.readthedocs.io/en/latest/getting-xbrl/> — EdgarTools XBRL guide
- <https://github.com/chonito7919/DivScout> — DivScout SEC dividend parser

### Bloomberg corporate-action fields
- <https://data.bloomberglp.com/professional/sites/10/Dividends-Forecast-Fact-Sheet.pdf> — Bloomberg Dividends Forecast Fact Sheet
- <https://www.bloomberg.com/professional/insights/markets/bloomberg-pro-tips-analyze-historical-and-projected-dividends-with-bdvd/> — Bloomberg BDVD function pro tips
- <https://www.emich.edu/cob/programs/finance/flc/documents/formula-method-excel.pdf> — Bloomberg Excel formula method
- <https://www.bloomberg.com/professional/dataset/global-dividend-forecast-data/> — Bloomberg Global Dividend Forecast
- <https://sdmfsa.gitlab.io/latrousse/bloomberg/hist/dvd/> — Bloomberg historical DVD reference
- <https://pages.stern.nyu.edu/~adamodar/pdfiles/Bloombergfull.pdf> — Damodaran Bloomberg Terminal guide

### LSEG / Refinitiv / Datastream
- <https://guides.library.duke.edu/lseg-workspace/datastream> — Duke LSEG Workspace Datastream
- <https://community.developers.refinitiv.com/spaces/251/datastream.html> — Refinitiv Datastream developer forum
- <https://www.refinitiv.com/en/products/datastream-macroeconomic-analysis> — Datastream macro
- <https://libguides.bc.edu/finance/datastream> — Boston College Datastream
- <https://ucsd.libguides.com/data-statistics/datastream> — UCSD Datastream guide
- <https://fmc.refinitiv.com/clientFacing/pdf/DFO_User_Guide.pdf> — Datastream For Office user manual
- <https://libguides.brown.edu/datastream> — Brown Datastream guide
- <https://shib.isor.univie.ac.at/eikon_datastream/manuals/refinitiv-eikon-with-refinitiv-datastream-for-office-add-in.pdf> — Eikon Datastream Excel add-in manual
- <https://www.refinitiv.com/content/dam/marketing/en_us/documents/methodology/corporate-actions-methodology.pdf> — Refinitiv Corporate Action Methodology
- <https://www.bwl.uni-mannheim.de/media/Lehrstuehle/bwl/Maug/Database_info/Datastream_dataypes.pdf> — Datastream datatypes reference

### FactSet adjustments
- <https://www.factset.com/marketplace/catalog> — FactSet Marketplace catalog
- <https://developer.factset.com/api-catalog/symbology-api> — Symbology API
- <https://go.factset.com/hubfs/Website/Resources%20Section/Index%20Files/FactSet%20Financal%20Technologies%20Index%20Methodology_v1_20210215.pdf> — FactSet Financial Technologies Index Methodology
- <https://go.factset.com/hubfs/Website/Website_Downloads/Statistical%20Package%20Integration/factset%20ondemand%20web%20services%20reference%20manual_2.0.pdf> — FactSet OnDemand Web Services Reference Manual

### OpenFIGI
- <https://www.openfigi.com/> — OpenFIGI portal
- <https://www.openfigi.com/assets/local/figi-allocation-rules.pdf> — FIGI Allocation Rules
- <https://www.openfigi.com/about/features> — OpenFIGI features
- <https://www.openfigi.com/api/documentation> — OpenFIGI API docs
- <https://www.openfigi.com/about/overview> — OpenFIGI overview
- <https://www.bloomberg.com/company/press/bloomberg-launches-online-request-utility-and-new-mapping-tools-for-the-financial-instrument-global-identifier-figi/> — Bloomberg FIGI mapping tools

### Yahoo Finance adjusted close
- <https://help.yahoo.com/kb/SLN28256.html> — Yahoo adjusted close documentation
- <https://www.bitget.com/wiki/does-yahoo-finance-adjust-for-stock-splits> — Bitget on Yahoo split adjustment
- <https://www.quora.com/Why-are-charts-in-Yahoo-Finance-only-adjusted-by-splits-and-not-by-dividends> — Quora on Yahoo charts
- <https://github.com/joshuaulrich/quantmod/issues/253> — quantmod issue #253 — Yahoo dividends/splits inconsistency
- <https://www.cgaa.org/article/does-yahoo-finance-adjust-for-stock-splits> — CGAA Yahoo split-adjustment article
- <https://medium.com/@josue.monte/why-adj-close-disappeared-in-yfinance-and-how-to-adapt-6baebf1939f6> — Adj Close in yfinance

### Nasdaq / NYSE
- <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltCodes> — Nasdaq Trader halt codes
- <https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltRSS> — Nasdaq trading-halt RSS
- <https://www.nasdaqtrader.com/Trader.aspx?id=archiveheadlines&cat_id=105> — 2026 Equity Corporate Actions Alert Index
- <https://www.nasdaqtrader.com/trader.aspx?id=tradinghaltsearch> — Nasdaq Trader trading halt search
- <https://www.nasdaqtrader.com/trader.aspx?id=IPOIndicator> — Nasdaq IPO Indicator
- <https://www.nasdaq.com/solutions/data/equities/corporate-action-solutions> — Nasdaq Corporate Action Solutions
- <https://www.nasdaq.com/solutions/data/nasdaq-data-link/api> — Nasdaq Data Link API
- <https://data.nasdaq.com/databases/WCA> — Nasdaq Data Link WCA database
- <https://www.exchange-data.com/product/adjustment-factors-data/> — Exchange Data International Adjustment Factors

### Modern API vendors
- <https://databento.com/corporate-actions> — Databento corporate-actions endpoint
- <https://eodhd.com/financial-apis/calendar-upcoming-earnings-ipos-and-splits> — EODHD calendar API
- <https://quodd.com/hubfs/corporate-actions-handling-in-globalhistorical-v3.pdf> — Quodd/Xignite corporate-action handling

### Cross-references in this repo
- `../vendors/factset.md` — FactSet symbology + corporate action handling
- `../vendors/sp_global.md` — Compustat survivorship + CRSP linking
- `../vendors/refinitiv_bloomberg.md` — Refinitiv + Bloomberg vendor profile
- `../schemas/data_models_and_methodology.md` §D, §G.5 — canonical corp_action schema (this file's DDL extends those)
- `../datasets/13f_holdings.md` — 13F dataset documentation (template for this file)

---

**Confirm:**

- File path: `c:/Users/natha/OneDrive/Desktop/C/ats/ats-eqt/research/datasets/corporate_actions.md`
- Section count: **18 top-level parts** (0 Exec summary; 1 Why; 2 Vendor matrix; 3 CRSP; 4 FactSet; 5 Bloomberg; 6 DISTCD ref; 7 DLSTCD ref; 8 Total-return walk; 9 ats-eqt schema; 10 Public reconstruction; 11 DTCC; 12 NYSE/Nasdaq/ICE; 13 Refinitiv; 14 S&P/Compustat; 15 OpenFIGI; 16 §358/Form 8937; 17 Open questions; 18 Sources).
