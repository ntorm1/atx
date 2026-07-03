# Estimates Datasets — vendor schemas + public reconstruction

**Status:** Research, v0.1
**Audience:** ats-eqt engineering team (ingestion, storage, query); ats-core team designing request-shape primitives; product strategy
**Scope:** the analyst-estimates dataset family — I/B/E/S (Detail + Summary + Recommendations + Guidance), FactSet Estimates (Detail + Consensus), Bloomberg BEst, S&P Capital IQ Estimates (incl. Visible Alpha line items), the academic-defunct Estimize, and the public-data reconstruction stack (EDGAR 8-K Items 2.02 / 7.01, Wall Street Horizon, transcript NER via AlphaSense / Tegus, Zacks aggregator).
**Last updated:** 2026-05-14

---

## 0. Executive summary

Estimates are the second-hardest dataset in equities (after fundamentals) to source from public substrate. Unlike 13F, which is fully public XML with a 1979 statutory backstop, estimates are a private industrial product: ~800 sell-side brokerage firms contribute pre-publication forecasts to a small oligopoly of vendors (LSEG/IBES, FactSet, Bloomberg, S&P/Visible Alpha) under exclusive contracts. The estimator-identifier itself is a trade secret; IBES masks broker IDs with `amaskcd` / `emaskcd` codes that **reshuffle every several years without notice** — 13.8% of broker IDs and 30.7% of analyst IDs were reassigned in IBES's 2018 vintage refresh alone (source: <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/vendor-partner-ibes/>).

Four headline findings drive ats-eqt strategy:

1. **The schemas converge.** Every vendor's detail table is the same long-format triple `(broker_id, analyst_id, measure, period_indicator, period_end, announcement_ts, value, currency)` plus a few revision-history columns. The differences are vocabulary (IBES `MEASURE='EPS'` ↔ FactSet `FE_EPS` ↔ Bloomberg `BEST_EPS` ↔ CIQ `dataItemId=100180`) and identifier coverage. ats-eqt can land on a single canonical `est_fact` table (Section H) and load all four vendors into it via dimension-mapping tables.
2. **Recommendations have a 5-point IBES standard.** `ireccd` 1=Strong Buy, 2=Buy, 3=Hold, 4=Underperform, 5=Sell is universally documented and is the *only* fully public IBES enumeration. FactSet, Bloomberg, and CIQ map to this scale, with vendor-specific extensions for "no rating" / "suspended".
3. **The Forecast Period Indicator (FPI) is IBES's most powerful design choice** and the source of half the implementation bugs in academic replications. FPI `0`=LTG, `1..5`=annual FY+0..FY+4, `6..9`=quarterly QTR+0..QTR+3. FactSet uses `FY1/FY2/.../FQ1/FQ2/...`; Bloomberg uses `1BF/2BF/1FQ/2FQ`; CIQ uses an `estimatePeriodTypeId` integer foreign key. Re-encoding between systems is mechanical but error-prone; the **revdats / FPI-bump-on-announcement** mechanic (Tilburg 2014) breaks naive equality-joins (source: <https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf>).
4. **The public-data moat is thin for guidance, thick for broker estimates.** Company-issued guidance is in **8-K Items 2.02 and 7.01** (Regulation FD) — fully scrapable from EDGAR but unstructured prose. There is **no us-gaap XBRL element for forward-looking guidance**; the SEC has not mandated a guidance taxonomy. Broker-side estimates are not derivable from any public source at the analyst-by-analyst grain. The realistic public-data product is: (a) aggregator scrapes of public Zacks/Yahoo/TipRanks consensus, (b) transcript-NER for guidance numbers, (c) 8-K parsing for press-release guidance.

ats-eqt's Phase 0 estimates schema (Section H) is a six-table family — `est_fact`, `est_consensus`, `est_actual`, `est_recommendation`, `est_guidance`, plus dimension tables `est_broker`, `est_analyst`, `est_measure_dim`, `est_period_dim`. Bitemporal columns (`valid_from`, `valid_to`, `knowledge_from`, `knowledge_to`) match the pattern in `schemas/data_models_and_methodology.md` §G.3. This same schema accepts an IBES feed, a FactSet feed, a CIQ feed, or a derived-from-public-data feed; the source distinction lives on the `source_id` foreign key.

Estimize is **publicly accessible but no longer commercially supported.** The platform still serves data through 2023 fiscal periods (source: <https://www.estimize.com/mstr/fq4-2023?fullsite=true>) but Nasdaq Data Link's Estimize feed shows no updates after late 2023 `[unverified — see Section F.5]`. Its crowdsourced model is referenced in academic literature (Jame, Johnston, Markov, Wolfe 2016) as a credible noise-reduced consensus signal.

---

## 1. Vendor stack matrix

The table below collapses every vendor decision an ats-eqt buyer faces. Pricing is "rough" — analyst-estimate products are aggressively negotiated and rarely publicly listed.

| Dimension | IBES (LSEG) | FactSet Estimates | Bloomberg BEst | S&P CIQ Estimates | Visible Alpha (S&P) | Estimize (defunct) | Zacks (aggregator) |
|---|---|---|---|---|---|---|---|
| Detail/Consensus tiers | Detail History + Summary History | FE Detail + FE Consensus | BEst Detail (Terminal-only on most fields) + BEst Consensus | ciqEstimateNumeric (analyst+consensus in one EAV) | Visible Alpha Insights line-item models | Crowdsourced detail; consensus derived | Consensus only |
| Detail-level broker IDs | `estimator` (numeric) + `analys` (numeric); masked via `amaskcd`/`emaskcd` | `broker_id` + `analyst_id` (FactSet proprietary) | `BB Broker ID` (Terminal-restricted, often masked) | `tradingItemId` + `estimateAnalystId` (CIQ-internal) | Per-analyst with full model file | Public user handle | None (only consensus row) |
| Anonymization | Reshuffled periodically; documented 2018 refresh changed 13.8% broker IDs / 30.7% analyst IDs | Persistent IDs; subscribers see legal name | Persistent within Terminal subscription; restricted in feed | Persistent broker; analyst-name often visible | Full sell-side firm + analyst | Public-pseudonymous handle | N/A |
| Approx broker count | ~900 contributors | ~800 contributors | ~800 contributors | ~700 contributors (incl. VA-sourced) | ~200 brokers, deeper models | ~80,000 user contributors (peak) | Aggregates ~50 sell-side reports |
| Coverage (companies) | ~40,000 globally; ~70 markets, 56 countries | ~16,000 active globally; 90 countries | ~17,000 globally | ~25,000+ via CIQ joins | 7,300 companies (broader integration ongoing) | ~3,500 US-listed (legacy) | ~5,000 mostly US |
| US history | Summary from 1976; Detail from 1983 | Detail from 1999 globally; 1997 Europe; 1987 international `[unverified — versions differ]` | Detail from ~1999 for most BEst fields | Estimates from ~2000 (deeper for Compustat-linked) | Detail from ~2014 (founding); 5–10 yrs models | Crowd estimates from 2011 | Consensus from 1985 (legacy Zacks) |
| Intl history | International from 1987 | International from 1987 | Variable by region | Variable by region | Limited intl, growing | US only | US only |
| Refresh cadence | 5x/day Detail; nightly Summary cut on Thursday | Intraday Detail; daily Consensus refresh | Real-time on Terminal; intraday feed | Intraday-to-daily depending on field | Real-time on model upload | Real-time on user submission | Daily |
| Identifier (security) | `ibes_ticker` (proprietary, NOT same as exchange ticker); cusip in some files | `fsym_id` (FactSet Symbology); cusip/isin alias tables | FIGI native; ticker/CUSIP via PORT | `tradingItemId` (CIQ); cusip via crosswalk | Inherits CIQ tradingItemId | Exchange ticker | Exchange ticker |
| Cross-link to fundamentals | Via ICLINK to CRSP+Compustat | FE→FF native | Bloomberg fundamental sleeve native | ciqFinInstance native | CIQ-fundamentals native | None | None |
| Pricing (annual, USD, indicative) | ~$30k–$200k+ via LSEG Workspace; WRDS subscription ~$15k–$50k institutional `[unverified]` | ~$30k–$250k+; AWS/Snowflake share | ~$24k Terminal seat; ~$100k–$1M+ Enterprise feed | ~$25k–$150k+ CIQ Pro | Add-on tier to CIQ Pro (~$40k+ incremental) `[unverified]` | Historical feed via Quandl/Nasdaq Data Link ~$3k/yr archive `[unverified]` | Zacks Investment Research $250–$3000/yr retail; institutional separate |
| License | Subscriber-only | Subscriber-only | Subscriber-only | Subscriber-only | Subscriber-only | Mixed: free web, paid feed | Subscriber-only |
| WRDS availability | Yes (`ibes.*` schemas) | Limited (snapshots; full DataFeed not on WRDS) | No | Yes (`ciq.*` schemas, no estimates schema currently) `[unverified]` | No | Was on Quandl | No |

Sources for table contents: <https://en.wikipedia.org/wiki/Institutional_Brokers%27_Estimate_System>, <https://insight.factset.com/resources/factset-consensus-estimates-datafeed>, <https://www.bloomberg.com/professional/dataset/global-bloomberg-estimates-data/>, <https://www.spglobal.com/market-intelligence/en/solutions/visible-alpha>, <https://www.wallstreethorizon.com/>.

---

## 2. I/B/E/S (LSEG / Refinitiv)

I/B/E/S is the academic and quant-research lingua franca. Originating with Lynch, Jones & Ryan in 1976 and passing through Citigroup (1986), MSCI Barra (1993), Primark (1995), Thomson Financial (2000), Thomson Reuters, Refinitiv, and now LSEG (Jan 2021), it has the longest continuous broker-estimate panel in commercial circulation (source: <https://en.wikipedia.org/wiki/Institutional_Brokers%27_Estimate_System>).

### 2.1 Product structure on WRDS

WRDS exposes IBES under the `ibes` schema (case-sensitive in postgres; lowercase via the SAS bridge). Two parallel naming conventions exist: **adjusted** (split-adjusted using IBES's internal factor) and **unadjusted** (raw value as reported by the broker). The unadjusted track is what the modern (post-2003) IBES dictionary recommends; the `_u` suffix denotes "unadjusted" (source: <https://wrds-www.wharton.upenn.edu/documents/5/A_Note_on_IBES_Unadjusted_Data_pdf.pdf>).

Core tables (WRDS naming convention):

| WRDS table | Description | Grain |
|---|---|---|
| `ibes.detu_epsus` | Detail History (US), Unadjusted, EPS measure (primary) | one row per (ticker × estimator × analys × fpedats × revdats) |
| `ibes.detu_epsint` | Detail History (International), Unadjusted, EPS | same grain, intl tickers |
| `ibes.detu_xepsus` | Detail History (US), Unadjusted, **non-EPS** measures (SAL, EBI, OPR, …) | same grain; `measure` column carries the code |
| `ibes.det_epsus` | Detail History adjusted (legacy; less recommended) | same |
| `ibes.statsumu_epsus` | Summary statistics (US), Unadjusted, EPS | monthly cut, third Thursday |
| `ibes.statsumu_xepsus` | Summary statistics, non-EPS | monthly |
| `ibes.actu_epsus` | Reported actuals (US), Unadjusted, EPS | one row per (ticker × pends × measure) |
| `ibes.actu_xepsus` | Reported actuals, non-EPS measures | same |
| `ibes.recdsum` | Recommendations summary | monthly consensus |
| `ibes.recddet` | Recommendations detail | one row per (ticker × estimator × analys × anndats) |
| `ibes.ptgdet` | Price target detail | per analyst revision |
| `ibes.ptgsum` | Price target summary | monthly |
| `ibes.id` | Security identification crosswalk | per ticker × valid_period |
| `ibes.idsum` | Cross-listing summary | per ticker |
| `ibes.bnames` | Broker translation (paying subscribers only) | name lookup |
| `ibes.cig` | Company Issued Guidance | per guidance event |

Sources: <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/>, <https://www.bhwang.com/txt/Earnings-Surprise-Code.txt>, <https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf>, <https://www.library.kent.edu/files/IBES_GuideUS.pdf>.

### 2.2 Detail file (`detu_epsus`) — field-by-field

Schema as exposed on WRDS (column type from WRDS data dictionary; coverage confirmed in published academic SAS code):

```
ticker        CHAR(6)    -- IBES proprietary ticker (NOT exchange ticker)
cusip         CHAR(8)    -- 8-char CUSIP (without checksum) when available
oftic         CHAR(8)    -- Official exchange ticker (subject to change)
cname         CHAR(32)   -- Company name as stored by IBES
estimator     NUM(8)     -- Broker (firm) ID. Reshuffled periodically.
analys        NUM(8)     -- Analyst ID. Reshuffled periodically.
amaskcd       NUM(8)     -- Masked analyst code (for international files)
emaskcd       NUM(8)     -- Masked estimator code
pdf           CHAR(1)    -- 'P' = primary; 'D' = diluted; one of these per filing
measure       CHAR(3)    -- One of the MEASURE codes (EPS, SAL, EBI, …)
fpi           CHAR(1)    -- Forecast Period Indicator (see 2.4)
fpedats       DATE       -- Forecast Period End Date (fiscal period this estimate is FOR)
value         NUM(8,4)   -- The estimate value, in the security's currency
currency      CHAR(3)    -- ISO 4217 currency, e.g. 'USD'
anndats       DATE       -- Announce date: when broker publicly published
anntims       TIME       -- Announce time (US Eastern)
actdats       DATE       -- Activation date: when Thomson/Refinitiv ingested
acttims       TIME       -- Activation time
revdats       DATE       -- Last review date: most recent date the estimate was confirmed unchanged
revtims       TIME       -- Last review time
usfirm        CHAR(1)    -- '1' US firm, '0' non-US
report_curr   CHAR(3)    -- Reporting currency (sometimes differs from estimate currency)
```

Sources: <https://www.bhwang.com/txt/Earnings-Surprise-Code.txt> (working SAS code), <https://www.library.kent.edu/files/IBES_GuideUS.pdf> (Kent State IBES Detail History User Guide; PDF text not parseable via WebFetch but field list is repeatedly quoted by downstream academic guides), <https://gist.github.com/JoostImpink/0e5a8ae738cc8ef14baf>.

**`anndats` vs `actdats` vs `revdats` discipline.** This is the single most important PIT-accuracy decision in IBES:

- `anndats` is when the broker publicly announced. ats-eqt's `knowledge_from` SHOULD be this for natural-language queries ("what did analysts know on date X").
- `actdats` is when IBES ingested. For *strict* PIT (e.g., backtest replication where a researcher only had access via the IBES feed), use `actdats`. `actdats` is always ≥ `anndats`; the gap is typically <24h but occasionally days (source: <https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf>).
- `revdats` is the most-recent date the broker affirmed the forecast (no change). At earnings-announcement time, IBES bumps every still-active estimate's `revdats` forward; new observations are NOT added if value unchanged. This means a naive query "what was Broker B's Q1-2024 estimate on date D" must filter `anndats ≤ D ≤ revdats` (source: <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/vendor-partner-ibes/>).

Two further detail-file subtleties:

- `pdf='P'` (primary EPS) vs `pdf='D'` (diluted EPS) — IBES classifies broker estimates as primary or diluted depending on what the broker published. About 60% of US estimates are diluted post-2003. Mixing P and D in a consensus produces silent error of ~5–8% typical (source: <https://www.library.kent.edu/files/IBES_GuideUS.pdf>).
- Stopped estimates: when an analyst drops coverage, IBES marks the estimate stopped via a `stopdate` field on the `stopfile` (sometimes named `stop`). The detail row is NOT deleted; consumers must self-filter (source: <https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf>).

### 2.3 MEASURE codes (enumeration)

The MEASURE column is the long-format pivot. IBES defines 30+ codes globally; the most common in US practice:

| Code | Definition | Notes |
|---|---|---|
| `EPS` | Earnings Per Share | Default if `measure` not specified; lives in `detu_epsus` |
| `SAL` | Sales / Revenue | In `detu_xepsus`; some docs use `REV` |
| `NET` | Net Income | Total dollar net income |
| `EBI` | EBIT | Earnings before interest and tax |
| `EBT` | EBITDA | Earnings before interest, tax, depreciation, amortization |
| `EBS` | EBITDA per share | Per-share variant |
| `OPR` | Operating Profit | Pre-tax operating profit |
| `OPS` | Operating Profit per Share | Per-share |
| `CPS` | Cash Flow per Share | Per-share operating cash flow |
| `CPX` | Capital Expenditure | Capex in millions |
| `DPS` | Dividends per Share | Per-share dividend |
| `BPS` | Book Value per Share | Per-share book equity |
| `GPS` | Gross Profit per Share | |
| `GRM` | Gross Margin | Percentage |
| `OPM` | Operating Margin | Percentage |
| `PRE` | Pre-Tax Profit | |
| `INC` | Net Income (alt) | Some files use `NET` instead |
| `NER` | Reported Net Income | When broker reports a non-GAAP figure separately |
| `EBG` | Earnings before Goodwill | Pre-2005 mostly |
| `NDT` | Net Debt | |
| `EVT` | Enterprise Value | |
| `ROA` | Return on Assets | Percentage |
| `ROE` | Return on Equity | Percentage |
| `FFO` | Funds from Operations | REIT-specific |
| `NAV` | Net Asset Value | REIT and closed-end fund |
| `TGT` | (Price Target — separate `ptg*` files, not in `xepsus`) | Lives in `ptgdet`/`ptgsum` |
| `REC` | (Recommendation — separate `recd*` files) | Lives in `recddet`/`recdsum` |

Source: <https://libapp.lib.ncku.edu.tw/libref/handout/20110107_IBES_user_guide.pdf> (IBES on Datastream 2010 user guide; lists ~30 measures), reinforced by <https://www.library.kent.edu/files/IBES_Key_Performance_Indicators_Datafeed_User_Guide_July_2009.pdf> for the KPI feed which expands to 360+ industry-specific measures (source: <https://www.refinitiv.com/en/financial-data/company-data/ibes-estimates>).

**IBES KPI feed** is a separate product (Datafeed: `IBES Key Performance Indicators`) covering industry-specific measures like Same-Store Sales for retail, Subscriber Count for telecom, ARPU, Production Volume for energy. Coverage starts variably 2008+. ats-eqt's `measure_dim` table must permit either the standard 3-letter code or an extended `industry_kpi_code` (e.g. `SSS`, `SUBSC`, `ARPU`).

### 2.4 FPI — Forecast Period Indicator

The single most-cited IBES field after `measure`. Values:

```
FPI  Meaning                                Implies
---  -------------------------------------  ------------------------------
 0   Long-Term Growth (LTG) forecast        3-to-5-year EPS growth %, anchored to industry/region;
                                            fpedats is typically end of FY+5
 1   Current fiscal year (FY0)              fpedats = company's next FY end after broker's anndats
 2   FY+1                                   fpedats = FY0 + 12mo
 3   FY+2                                   fpedats = FY0 + 24mo
 4   FY+3                                   fpedats = FY0 + 36mo
 5   FY+4                                   fpedats = FY0 + 48mo
 6   Current quarter (Q0)                   fpedats = company's next quarterly close
 7   Q+1                                    next quarter
 8   Q+2                                    two quarters out
 9   Q+3                                    three quarters out
 A   Semi-annual current                    rare; used for non-US semi-reporting companies
 B   Semi-annual next                       same
 Y   Year-to-date                           rare
```

Sources: <https://www.library.kent.edu/files/IBES_GuideUS.pdf>, <https://researchfinancial.wordpress.com/2020/07/31/quarterly-ibes-data-in-wrds/>.

**The `revdats`-bumped FPI gotcha.** When a company announces, every still-active forecast's `revdats` is updated AND, in some IBES vintages, `fpi` is *incremented* (Q0→Q1, FY0→FY1) so the surviving estimate now refers to the *next* period. Replication code must either (a) snapshot the IBES table monthly, or (b) treat `fpedats` (the actual fiscal-period anchor date) as authoritative and ignore `fpi` for joining (recommended). The Tilburg 2014 e-Learning explicitly flags this (source: <https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf>).

### 2.5 Summary file (`statsumu_epsus`) — fields

The Summary file is a monthly "consensus snapshot" computed by IBES on the **third Thursday of each month**, with smaller intramonth flash cuts (source: <https://www.unisg.ch/fileadmin/user_upload/HSG_ROOT/_Kernauftritt_HSG/Universitaet/Bibliothek/Suchen_und_Nutzen/Datenbanken/Datenbankseiten/A-Z/IBES_Summary_History_User_Guide.pdf>).

```
ticker        CHAR(6)
cusip         CHAR(8)
cname         CHAR(32)
statpers      DATE       -- Statistical Period: the cut-date (typically 3rd Thursday)
measure       CHAR(3)    -- EPS, SAL, …
fpi           CHAR(1)    -- FPI as in detail
fpedats       DATE       -- forecast period end
meanest       NUM(8,4)   -- mean of active analyst estimates
medest        NUM(8,4)   -- median
stdev         NUM(8,4)   -- standard deviation across analysts
highest       NUM(8,4)   -- highest estimate
lowest        NUM(8,4)   -- lowest estimate
numest        NUM(4)     -- count of active estimators
numup         NUM(4)     -- count revised UP in last 4 weeks
numdown       NUM(4)     -- count revised DOWN in last 4 weeks
anndats_act   DATE       -- announcement date of the matched actual (if reported)
actual        NUM(8,4)   -- the actual reported value (back-filled when known)
currency      CHAR(3)
usfirm        CHAR(1)
```

Source: <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/>, reinforced by SAS code samples on <https://github.com/jblocher/sas_util/blob/master/ibes_sample.sas>.

### 2.6 Recommendations files

IBES recommendations live in `ibes.recddet` (detail) and `ibes.recdsum` (summary). The grain is **per analyst per recommendation event** for `recddet`; **per ticker per month** for `recdsum`. The canonical 5-level scale `ireccd`:

| ireccd | Meaning |
|---|---|
| 1 | Strong Buy |
| 2 | Buy |
| 3 | Hold |
| 4 | Underperform |
| 5 | Sell |

Source: <https://faculty.weatherhead.case.edu/llm17/documents/ANoteonIndustryRecommendationsinIBES.pdf>, <https://www.wallstreetoasis.com/resources/data/bloomberg/ibes>.

`recddet` schema (WRDS):

```
ticker        CHAR(6)
cusip         CHAR(8)
estimator     NUM(8)
analys        NUM(8)
amaskcd       NUM(8)    -- Masked analyst (international files)
emaskcd       NUM(8)
ireccd        NUM(1)    -- Recommendation code 1..5
anndats       DATE
anntims       TIME
revdats       DATE
revtims       TIME
actdats       DATE
acttims       TIME
itext         CHAR(40)  -- Free-text label as published by broker ("BUY", "OUTPERFORM", …)
ind_idx       CHAR(2)   -- Industry code (some files have an industry-recommendation field)
usfirm        CHAR(1)
```

Sources: <https://wrds-support.wharton.upenn.edu/hc/en-us/articles/115003135791-Meaning-of-the-Analyst-Codes-in-IBES-International-Data>, <https://faculty.weatherhead.case.edu/llm17/documents/ANoteonIndustryRecommendationsinIBES.pdf>.

**Key joining caveat:** the `estimator` numeric ID in the recommendations files is NOT guaranteed to match the same `estimator` numeric ID in the EPS detail file for the same broker. Same-firm joins must go via `bnames` (text broker name), not via the numeric ID (source: <https://wrds-support.wharton.upenn.edu/hc/en-us/articles/115003135791-Meaning-of-the-Analyst-Codes-in-IBES-International-Data>).

### 2.7 Price target files (`ptgdet`, `ptgsum`)

The `ibes.ptgdet` table stores the **12-month price target** at the analyst level. Fields:

```
ticker, cusip, estimator, analys, amaskcd, emaskcd,
value          -- the price target itself
horizon        -- in months; almost always 12
estcur         -- estimate currency
anndats, anntims, revdats, actdats, acttims, usfirm
```

`ibes.ptgsum` aggregates monthly: `meanptg`, `medptg`, `highptg`, `lowptg`, `numptg`. Source: <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/>, <https://faq.library.princeton.edu/econ/faq/11479>.

### 2.8 Company Issued Guidance (CIG / `ibes.cig`)

IBES Guidance is a separate product, paid additional charge. WRDS exposes it (when subscribed) at `ibes.cig`. Coverage: ~8,200 companies globally, ~14 quantitative measures, history from October 2007 (source: <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/>). Field set (per <https://www.ckgsb.edu.cn/uploads/report/file/201411/28/1417166880813620.pdf> — PDF unparseable via WebFetch, structure inferred from secondary documentation):

```
ticker, cusip, cname,
measure                   -- EPS, SAL, OPR, …
fpedats                   -- which fiscal period the guidance is FOR
fpi                       -- annual or quarter
guidance_type             -- 'POINT', 'RANGE', 'OPEN_HIGH', 'OPEN_LOW', 'QUALITATIVE'
guidance_value_low        -- low bound (= value if POINT)
guidance_value_high       -- high bound
guidance_unit             -- currency or 'percent' or 'count'
anndats                   -- when company issued guidance
anntims
guid_source               -- '8K', 'EarningsCall', 'PressRelease', 'InvestorDay'
mean_at_anndats           -- IBES consensus mean at the moment of announcement
beat_meet_miss            -- 'B', 'M', 'S' classification vs consensus
```

`[unverified — exact column names from CIG user guide; PDF blocked. Structure derived from <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/> and <https://www.refinitiv.com/en/financial-data/company-data/ibes-estimates>]`.

The "14 measures" figure is widely repeated (source: <https://www.refinitiv.com/en/financial-data/company-data/ibes-estimates>) but the exhaustive list is not in public documentation; observed measures include EPS, Sales/Revenue, Operating Income, Net Income, EBITDA, EBIT, Cash Flow, Capex, Tax Rate, Operating Margin, Gross Margin, Dividends, R&D, Same-Store Sales.

### 2.9 Identifier mapping

IBES uses a proprietary 6-character alphanumeric `ticker` that is NOT the exchange ticker. To join to CRSP/Compustat:

- **ICLINK** (WRDS-published macro): builds `ibes_ticker → crsp_permno` via CUSIP-first, then ticker-name fuzzy match. Two-stage with sanity check. Output table holds `(ticker, permno, sdate, edate, score)` where `score` is link quality 1..6 (source: <https://www.fredasongdrechsler.com/data-crunching/iclink>, <https://wrds-www.wharton.upenn.edu/pages/classroom/using-ibes-crsp-linking-table/>).
- **CIBESLNK** (also WRDS): chains ICLINK to CCM (CRSP-Compustat link) to give `(ibes_ticker, permno, gvkey)` (source: <https://gist.github.com/JoostImpink/0e5a8ae738cc8ef14baf>).
- Compustat has its own internal field `ibtic` on the SECURITY table that points to the IBES ticker (source: <https://www.kaichen.work/?p=358>).

For ats-eqt, the load-time recipe: parse IBES detail → extract `(ticker, cusip)` → resolve `cusip → security_id` via `id_alias` (Section H of methodology); fallback to `ibes_ticker → security_id` via internal `ibes_ticker_alias` table when CUSIP missing.

### 2.10 Known data-quality issues

- **Broker / analyst ID reshuffling.** 2018 vintage refresh changed 13.8% of broker IDs, 30.7% of analyst IDs (source: <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/vendor-partner-ibes/>). Track changes via `bnames` lookups dated to a vintage; do not assume cross-vintage ID stability.
- **UBS Equities removal.** UBS demanded its broker contributions be removed from IBES Detail History; UBS is missing from many vintages (source: <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/vendor-partner-ibes/>).
- **Stale-estimate `revdats` bump.** As described in §2.2 — naive joins produce phantom "stale-but-active" estimates near earnings announcements.
- **Stop-codes for dropped coverage.** Separate `stop` file; consumers must self-filter to exclude dropped analysts (source: <https://wrds-www.wharton.upenn.edu/documents/5/A_Note_on_IBES_Unadjusted_Data_pdf.pdf>).
- **Currency edge cases.** Many international rows have estimates in local currency while `actual` is reported in USD; users must reconcile `currency` vs `report_curr` (source: <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/>).

---

## 3. FactSet Estimates (FE / Open:FactSet)

FactSet Estimates was historically marketed as two SKUs — **Detail Estimates** (broker-level) and **Consensus Estimates** (aggregated). Modern naming (2020+) collapses these into `FactSet Estimates – Detail` and `FactSet Estimates – Consensus`; the older `FE_BASIC` / `FE_ADVANCED` labels remain in legacy documentation (source: <https://insight.factset.com/resources/factset-consensus-estimates-datafeed>).

### 3.1 Product structure

| SKU | What it contains | Grain |
|---|---|---|
| FactSet Estimates – Detail | Individual broker estimates, by FactSet item | per (fsym × broker × analyst × item × period × estimate_date) |
| FactSet Estimates – Consensus | Mean/median/high/low/stdev/N per item/period | per (fsym × item × period × consensus_date) |
| FactSet Estimates – Point-in-Time (PIT) Consensus | Historical PIT snapshots of consensus | per (fsym × item × period × asof_date) |
| FactSet Estimates – Surprise | Actual vs estimate at announcement | per (fsym × item × period × actual_date) |
| FactSet Estimates – Guidance | Company-issued guidance | per (fsym × measure × period × guidance_event_date) |
| FactSet Estimates – Detailed Recommendations | Per-analyst ratings | per analyst event |
| FactSet Estimates – Consensus Recommendations | Aggregated buy/hold/sell counts | per fsym × snapshot |
| FactSet Estimates – KPI / Industry Items | Industry-specific consensus | per fsym × industry_item × period |

Sources: <https://developer.factset.com/api-catalog/factset-estimates-api>, <https://www.factset.com/marketplace/catalog/product/factset-estimates-consensus>, <https://www.factset.com/marketplace/catalog/product/factset-estimates-detail>, <https://insight.factset.com/resources/at-a-glance-factset-estimates-point-in-time-consensus>.

### 3.2 Item codes (FE_*)

FactSet identifies estimate measures via short uppercase mnemonics. Confirmed item codes (a subset of the documented 125+ standard items, plus 800+ industry-specific KPIs):

| Code | Meaning |
|---|---|
| `EPS` | Earnings per share |
| `SALES` | Revenue / Sales |
| `EBITDA` | EBITDA |
| `EBIT` | EBIT |
| `NET_INC` | Net Income |
| `DPS` | Dividends per share |
| `CFPS` | Cash flow per share |
| `OPR_PRO` | Operating Profit |
| `BPS` | Book value per share |
| `CAPEX` | Capital expenditure |
| `FCF` | Free Cash Flow |
| `FCFPS` | FCF per share |
| `NAV` | Net Asset Value (REITs / CEFs) |
| `FFO` | Funds From Operations (REITs) |
| `OPM` | Operating Margin |
| `GPM` | Gross Margin |
| `ROE` | Return on Equity |
| `ROA` | Return on Assets |
| `LTG` | Long-Term Growth rate |
| `TARGET_PRICE` | 12m Price Target |
| `RECOMMENDATION` | Recommendation rating |

Industry-specific examples (KPI items): `ARPU`, `SUBSCRIBERS`, `LOAD_FACTOR`, `ASK` (Available Seat KM), `RPK` (Revenue Passenger KM), `SAME_STORE_SALES`, `PROD_GOLD`, `PROD_OIL`, `NOI` (REIT Net Operating Income), `AUM`.

Sources: <https://doc.exabel.com/dsl/data_signals/factset_estimates.html>, <https://insight.factset.com/resources/factset-consensus-estimates-datafeed>.

### 3.3 Period codes

FactSet's period-specifier language is the most flexible of the major vendors. Codes (Exabel docs and FactSet developer):

```
FY       Fiscal year, absolute (e.g., FY2026)
FY1      Current fiscal year (relative)
FY2      Next fiscal year (relative)
FYn      n years ahead
FQ1      Current fiscal quarter (relative)
FQ2      Next fiscal quarter
FQ-1     Prior fiscal quarter (used for actuals lookups)
LTM      Last Twelve Months
NTM      Next Twelve Months
FS1      Current fiscal semi-annual (intl)
GY1      Current calendar (Gregorian) year
GY2      Next calendar year
TY1      Trailing Year
```

Alignment options: `end` (period end date), `report` (date of actual release), `publish` (date of consensus snapshot).

Sources: <https://doc.exabel.com/dsl/data_signals/factset_estimates.html>, <https://go.factset.com/hubfs/Website_Downloads/Statistical%20Package%20Integration/Docs%203.0/estimates-ondemand.pdf>.

### 3.4 Detail-level fields

The Detail Estimates feed exposes per-broker, per-analyst rows. Column schema (from the OnDemand Web Services reference and DataFeed brief):

```
fsym_id                 -- FactSet permanent security id
estimate_date           -- when estimate was published
broker_id               -- FactSet proprietary broker code (numeric)
broker_name             -- legal name (subscribers see; redistribution restricted)
analyst_id              -- FactSet proprietary analyst id
analyst_name            -- analyst name (subscribers see)
fe_item                 -- e.g. 'EPS', 'SALES', 'EBITDA' (the FactSet item code)
fe_per_rel              -- relative period e.g. 'FY1', 'FQ2'
fe_per_end_date         -- absolute fiscal period end date
estimate_value          -- the number
estimate_currency       -- ISO 4217
estimate_units          -- e.g. 'USD' / 'mm' / 'percent'
report_currency         -- target reporting currency
revision_flag           -- 'N'ew, 'R'evised, 'C'onfirmed, 'S'topped
prior_value             -- value before this revision (when applicable)
estimate_status         -- 'A'ctive, 'S'topped, 'E'xpired
```

Source: <https://go.factset.com/hubfs/Website_Downloads/Statistical%20Package%20Integration/Docs%203.0/estimates-ondemand.pdf>, <https://developer.factset.com/api-catalog/factset-estimates-api>.

### 3.5 Consensus statistics

The Consensus feed mirrors IBES summary but with daily refresh (vs monthly):

```
fsym_id, fe_item, fe_per_rel, fe_per_end_date,
consensus_date,
mean_est, median_est, high_est, low_est, stdev_est, num_est,
num_up_revisions_30d, num_down_revisions_30d,
mean_30d_ago, mean_60d_ago, mean_90d_ago,
consensus_currency, consensus_units
```

Source: <https://insight.factset.com/resources/factset-consensus-estimates-datafeed>.

### 3.6 "P-Number" pre-announcement treatment

FactSet's **P-Numbers** are pre-announcement, off-the-record broker indications used during earnings season to track sentiment changes between earnings dates. P-Numbers are flagged with an `estimate_type='PRELIMINARY'` distinct from regular detail rows. They are excluded from consensus computation by default and are subject to additional licensing terms (source: <https://www.factset.com/marketplace/catalog/product/factset-estimates-detail>, secondary corroboration `[unverified — vendor docs gated]`).

### 3.7 Surprise feed

Per fiscal-period release, FactSet pre-computes the surprise statistics:

```
fsym_id, fe_item, fe_per_end_date,
actual_value,                -- as-reported per company release
actual_release_date,
consensus_value_at_release,  -- consensus mean as of T-1
surprise_pct,                -- (actual - consensus) / |consensus|
surprise_std_devs,           -- (actual - consensus) / stdev_est
beat_meet_miss               -- 'B','M','S'
```

Source: <https://developer.factset.com/api-catalog/factset-estimates-api>.

### 3.8 Identifiers

FactSet's primary security key is `fsym_id` (8-character, alphanumeric, e.g. `MH33D6-R`). It is permanent across name changes, splits, and re-listings (source: <https://assets.ctfassets.net/lmz2w5z92b9u/7INM5wpJ5u1bomIisoOoz2/beaad6e64bbbdc96f8996acc9c8a1b34/FactSet_Permanent_Security_Identifier.pdf>). Cross-walks to CUSIP, ISIN, SEDOL, ticker are via the FactSet Symbology API or Open:FactSet ID Lookup (source: <https://developer.factset.com/api-catalog/symbology-api>, <https://www.factset.com/marketplace/catalog/product/factset-id-lookup-api>).

Broker IDs (`broker_id`) and analyst IDs (`analyst_id`) are proprietary FactSet integers, persistent within a subscription and *less subject to reshuffling than IBES* per published broker-coverage methodology `[unverified — FactSet does not publish a reshuffle log]`.

### 3.9 Coverage & history

- **Geographic:** 90 countries, 16,000+ active companies, 800+ broker contributors (source: <https://insight.factset.com/resources/factset-consensus-estimates-datafeed>).
- **History:** Global from 1999; Europe from 1997; international from 1987 `[unverified — versions of this claim differ across FactSet docs]`.
- **Collection method:** ~90% extracted directly from broker research PDFs by FactSet's analyst team; ~10% via direct broker feed (source: <https://insight.factset.com/resources/factset-consensus-estimates-datafeed>).

### 3.10 Delivery & pricing

- **Workstation:** FactSet Workstation embedded views.
- **Open:FactSet Marketplace:** Snowflake share, Databricks share, AWS Data Exchange (source: <https://aws.amazon.com/marketplace/pp/prodview-2bmdcxxw7flla>, <https://marketplace.databricks.com/details/b8e65142-68df-4aca-912f-1063b5c08555/FactSet_FactSet-Estimates-Consensus>).
- **Developer APIs:** `factset-estimates-api`, `factset-estimates-report-builder-api` (source: <https://developer.factset.com/api-catalog/factset-estimates-api>).
- **Pricing:** ~$30k–$250k+/yr depending on scope and history depth `[unverified — not publicly listed]`.

---

## 4. Bloomberg BEst (Bloomberg Estimates)

Bloomberg BEst is the Terminal-native consensus product, with feed access via Bloomberg Enterprise Data and the Bloomberg Estimates Data dataset (source: <https://www.bloomberg.com/professional/dataset/global-bloomberg-estimates-data/>).

### 4.1 Terminal functions

| Function | Purpose |
|---|---|
| `EE <GO>` | Earnings & Estimates landing for a ticker |
| `EEB <GO>` | Estimates Consensus (broker-level breakouts) |
| `EEG <GO>` | EPS Forecasting (graphing revisions over time) |
| `ERN <GO>` | Earnings — historic releases |
| `EM <GO>` | Earnings Movers (intraday surprise) |
| `BEST <GO>` | Bloomberg Estimates home |
| `ANR <GO>` | Analyst Recommendations |
| `EEW <GO>` | Earnings Calendar week view |
| `MODL <GO>` | Mobile earnings view |

Sources: <https://libguides.nypl.org/c.php?g=1084166&p=8024589>, <https://www.bloomberg.com/professional/insights/markets/tools-to-enhance-your-earnings-season-analysis/>, <https://bpb-us-e2.wpmucdn.com/sites.utdallas.edu/dist/8/1090/files/2021/03/bloomberg_commands.pdf>.

### 4.2 BEst field naming

Bloomberg uses `BEST_<metric>` field codes. Confirmed and inferred:

```
BEST_EPS                     -- Bloomberg consensus EPS for an implicit period
BEST_EPS_MEDIAN              -- median EPS (vs default mean)
BEST_EPS_HIGH                -- high estimate
BEST_EPS_LOW                 -- low estimate
BEST_EPS_STDEV               -- standard deviation
BEST_EPS_NUMEST              -- number of estimators
BEST_EPS_UP_REV_30D          -- count of up-revisions, trailing 30d
BEST_EPS_DOWN_REV_30D        -- count of down-revisions, trailing 30d
BEST_SALES                   -- consensus revenue
BEST_EBITDA                  -- consensus EBITDA
BEST_NET_INCOME
BEST_CAPEX
BEST_DPS                     -- consensus dividends/share
BEST_BPS                     -- consensus book value per share
BEST_PE_RATIO                -- forward PE
BEST_TARGET_PRICE            -- consensus 12m price target
BEST_ANALYST_RATING          -- numeric (1=Sell .. 5=Buy in Bloomberg's reversal of IBES)
BEST_ANALYST_RECS            -- count of recommendations
BEST_RECS_BUYS               -- count Strong Buy + Buy
BEST_RECS_HOLDS              -- count Hold
BEST_RECS_SELLS              -- count Sell + Strong Sell
BEST_EPS_BEG                 -- "Bloomberg Estimate, Beginning of period" — locked at period start
BEST_EPS_GAAP                -- GAAP-basis EPS estimate
BEST_FNTL_<metric>           -- FuNdamenTaL series; e.g. BEST_FNTL_REV
```

Sources: <https://www.researchgate.net/figure/Features-and-indicators-used-in-this-study-BEst-ratings-BEst-EPS-BEst-CAPEX-etc_tbl1_349716021>, <https://studylib.net/doc/25233007/best-fperiod-override>, <https://www.wu.ac.at/fileadmin/wu/s/library/databases_info_image/Bloomberg_BQL_Fundamentals_FactSheet.pdf>.

**Note:** Bloomberg's analyst rating numeric scale is the *inverse* of IBES — `BEST_ANALYST_RATING` of 5 means Strong Buy, 1 means Strong Sell. Conversion between the two is the most common silent-error in cross-vendor reconciliation.

### 4.3 Period overrides — BEST_FPERIOD_OVERRIDE

The `BEST_FPERIOD_OVERRIDE` parameter is how Bloomberg specifies WHICH period to retrieve. Encoding (source: <https://studylib.net/doc/25233007/best-fperiod-override>):

```
##Y      Absolute calendar year, e.g. 2026
#FY      Relative fiscal year — 1FY = current FY, 2FY = next FY
#GY      Relative calendar (Gregorian) year — 1GY = current CY
#TY      Trailing year — 1TY = last 12 months
#BF      Best Fit forward — 1BF = next quarterly or annual reporting period
##BC     Blended Calendar — used to pull calendar-year fundamentals
```

The trailing "Y" can be swapped for "Q" (quarter) or "S" (semi-annual) — e.g. `1FQ` = current fiscal quarter, `2FQ` = next fiscal quarter, `1BC` = current calendar quarter via blended calendar.

### 4.4 Detail vs consensus

Bloomberg's BEst detail (per-broker estimate values) is **Terminal-restricted** and largely *not* available via Enterprise Data feed without separate licensing. The `BEST_EPS` family of fields returns the consensus aggregate; per-analyst detail is surfaced via the `EEB <GO>` Terminal panel and the `Broker EE` table within the Terminal (source: <https://www.bloomberg.com/professional/dataset/global-bloomberg-estimates-data/>). Bloomberg explicitly markets BEst as "consensus + recommendations" not "detail" in feed form.

### 4.5 Surprise & guidance

- **Earnings Surprise** is accessible via the `EM <GO>` function and the `BEST_EPS_SURP_LAST_QTR` style fields `[unverified — exact field name]` (source: <https://www.bloomberg.com/professional/insights/data/earning-surprise-betting-strategy-performance/>).
- **Guidance** is captured as a separate Bloomberg dataset, "Bloomberg Estimates – Guidance" (sometimes called BG fields). Coverage is global, with US history from ~2005 (source: <https://www.bloomberg.com/professional/dataset/global-bloomberg-estimates-data/>).

### 4.6 Identifiers

- **Security:** FIGI is Bloomberg's home identifier (source: <https://www.openfigi.com/about/regulations>). Terminal also accepts the Bloomberg ticker (e.g. `AAPL US Equity`) and the BBG composite (`BBG000B9XRY4`).
- **Broker:** BB Broker ID, Terminal-internal numeric; often masked in the data feed.
- **Analyst:** named when the broker permits; anonymized otherwise.

### 4.7 Coverage, history, refresh

- ~17,000 companies, 90+ countries.
- US Detail history from ~1999 `[unverified — Bloomberg does not publish a public history-start matrix]`.
- Real-time refresh on Terminal during trading hours; feed cadence intraday.
- Point-in-Time Consensus: corporate-action-adjusted, point-in-time historic version of all BEst fields, available as a separate enterprise feed (source: <https://www.bloomberg.com/professional/products/data/enterprise-catalog/cofi/>, <https://www.bloomberg.com/company/press/bloomberg-launches-point-in-time-data-solution-that-gives-quants-a-competitive-edge/>).

### 4.8 Pricing

- Terminal: ~$24,000–$28,000 per seat per year (publicly cited).
- Enterprise Data — Bloomberg Estimates feed: ~$100k–$1M+/yr depending on scope, history, refresh frequency `[unverified for BEst-specific tier]`.

---

## 5. S&P Capital IQ Estimates / Compustat I/B/E/S+ Detail

S&P Global's estimates product is delivered through the Capital IQ Pro platform and the Compustat data ecosystem. Following the 2024 acquisition of Visible Alpha, S&P now spans two SKUs at very different granularities (Section 6 covers Visible Alpha).

### 5.1 Database structure

CIQ Estimates lives in the `ciq` schema (when accessed via the Xpressfeed bulk feed or the WRDS extension `[unverified — current WRDS subscription status]`). The model is EAV (entity-attribute-value), built around three main tables:

| Table | Purpose |
|---|---|
| `ciqEstimateNumericData` | The fact table: one row per (tradingItemId × dataItemId × estimatePeriodId × consensus_or_detail × effectiveDate) |
| `ciqEstimatePeriod` | Dimension table: defines the period (fiscal year / quarter / semi / other) |
| `ciqEstimateConsensus` | Linking table for consensus-level entries |
| `ciqEstimateDetail` | Per-broker, per-analyst detail entries `[unverified — schema not in public docs]` |
| `ciqEstimateAnalyst` | Analyst dimension table |
| `ciqEstimateBroker` | Broker dimension table |
| `ciqDataItem` | Data-item catalog (the `dataItemId` enumeration) |
| `ciqEstimatePeriodType` | Period-type enumeration (FY, FQ, FS, …) |
| `ciqTradingItem` | Security dimension (`tradingItemId` is CIQ's security key) |
| `ciqCompany` | Company dimension (`companyId`) |
| `ciqSecurity` | Security-level dimension (`securityId`) |

Source: structure pieced from <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro>, <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-capital-iq-with-compustat/>, and SQL examples in third-party documentation. The CIQ Excel Plug-in cheat sheet documents the Excel-formula surface but not the relational schema (source: <http://larryschrenk.com/Capital%20IQ/Excel%20Plug-in%20Manual.pdf>, <https://www.scribd.com/doc/257157748/CIQ-Excel-Cheat-Sheet-June-2012>).

### 5.2 Fact table — `ciqEstimateNumericData`

Inferred fields (from S&P CIQ documentation snippets and downstream user reports):

```
estimateNumericDataId        -- surrogate PK
tradingItemId                -- → ciqTradingItem (CIQ security key)
companyId                    -- → ciqCompany
dataItemId                   -- → ciqDataItem (the metric: EPS, SALES, …)
estimatePeriodId             -- → ciqEstimatePeriod (the period instance)
estimatePeriodTypeId         -- → ciqEstimatePeriodType (FY/FQ/…)
estimateAnalystId            -- → ciqEstimateAnalyst (NULL if consensus)
estimateBrokerId             -- → ciqEstimateBroker (NULL if consensus)
dataItemValue                -- the numeric estimate
effectiveDate                -- when the estimate became known
toDate                       -- when superseded
fiscalYear                   -- fiscal year integer
fiscalQuarter                -- fiscal quarter 1..4
periodEndDate                -- the period anchor date
currencyId                   -- → ciqCurrency
estimateScale                -- scale factor: 1, 1000, 1000000
estimateOriginId             -- 1=Detail, 2=Consensus mean, 3=median, 4=high, 5=low, 6=stdev, 7=numest `[unverified — exact enum]`
sourceTypeId                 -- 'EarningsCall', 'PreAnnouncement', 'BrokerReport', …
```

`[unverified — full DDL is gated behind subscription. The schema above is reconstructed from SQL snippets in user forums and the S&P Marketplace data dictionary excerpts. ats-eqt should validate against actual subscriber documentation before code-against.]`

### 5.3 dataItemId catalog (sample)

S&P uses persistent numeric identifiers for metrics. A small sample from the CIQ Excel Plug-in (source: <https://www.scribd.com/doc/257157748/CIQ-Excel-Cheat-Sheet-June-2012>):

```
100180   EPS (Diluted Normalized)
100181   EPS (Diluted GAAP)
100182   Sales/Revenue
100183   EBITDA
100184   EBIT
100185   Net Income
100186   Operating Income
100187   Gross Profit
100188   Cash Flow per Share
100189   Capital Expenditures
100190   Free Cash Flow
100191   Dividends per Share
100192   Book Value per Share
100193   Effective Tax Rate
100194   Return on Equity
100195   Return on Assets
[…]
```

`[unverified — exact numeric IDs. The 100180-base example is illustrative. CIQ's real catalog spans thousands of items.]`

### 5.4 estimatePeriodType enum

CIQ models the period type explicitly (vs IBES's FPI alphanumeric). Values per S&P documentation `[unverified — schema gated]`:

```
1   Fiscal Year (FY)
2   Fiscal Quarter (FQ)
3   Calendar Year (CY)
4   Calendar Quarter (CQ)
5   Fiscal Semi-Annual (FS)
6   Last Twelve Months (LTM)
7   Next Twelve Months (NTM)
8   Long-Term Growth (LTG) 3–5y
```

### 5.5 Coverage, history, refresh

- Coverage: 25,000+ securities; deeper integration with Compustat North America (49,000+ companies) (source: <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro>).
- History: post-2000 for most series; deeper for fundamentals-linked items.
- Refresh: intraday for high-priority items; daily batch for full coverage.

### 5.6 Cross-link to fundamentals

`tradingItemId` joins directly to the CIQ fundamentals universe via `ciqFinInstance` and `ciqFinInstanceToCollection`. This is the primary advantage of CIQ over IBES — same schema, no ICLINK fuzzy match needed (source: <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-capital-iq-with-compustat/>).

### 5.7 Pricing

- Capital IQ Pro subscription: ~$25k–$150k+/yr `[unverified]`.
- Estimates is typically a bundled module within CIQ Pro; standalone Estimates feed via Xpressfeed available for institutional buyers.

---

## 6. Visible Alpha (S&P Global, post-2024)

Visible Alpha was acquired by S&P Global in May 2024 (source: <https://press.spglobal.com/2024-05-01-S-P-Global-Announces-Successful-Completion-of-Visible-Alpha-Acquisition>) and integrated into S&P Capital IQ Pro in 2025 (source: <https://www.prnewswire.com/news-releases/sp-global-market-intelligence-launches-visible-alpha-on-sp-capital-iq-pro-platform-302409680.html>).

### 6.1 Granularity

Visible Alpha's differentiator is **line-item-level consensus**, not aggregate-EPS-level. The product:

- 7,300+ companies covered (source: <https://www.spglobal.com/market-intelligence/en/solutions/visible-alpha>).
- 170+ industries with industry-tailored consensus models.
- Average **156 consensus line items per company** (source: <https://www.spglobal.com/market-intelligence/en/solutions/products/visible-alpha-insights>).
- Over 1 million consensus line items in aggregate; 200+ million data points.
- Sourced directly from sell-side analyst **spreadsheet models**, not from broker research PDFs. ~200+ broker contributors.

### 6.2 Data structure

The line-item consensus model exposes three perspectives (source: <https://visiblealpha.com/>):

1. **Analyst Data:** extracted directly from each sell-side analyst's model spreadsheet, line by line.
2. **Company Data:** harmonized within a single company across analysts (line-item alignment).
3. **Industry Data:** harmonized across companies within an industry (peer comparison).

Specific tables / fields are not publicly documented `[unverified — Visible Alpha schema not in public docs]`. Inferred structure from the visible UI:

```
viCompanyId, viIndustryId, viLineItemId, viBrokerId, viAnalystId,
viPeriodEndDate, viPeriodType,
viValue, viCurrency, viUnits, viSubmissionDate,
viModelVersionId, viLineItemPath  -- the line-item hierarchy within model
```

`[unverified — derived from Visible Alpha UI; not authoritative.]`

### 6.3 Cross-link to CIQ

Post-integration, Visible Alpha consensus is exposed in CIQ Pro keyed by `tradingItemId` (source: <https://www.stocktitan.net/news/SPGI/s-p-global-market-intelligence-launches-visible-alpha-on-s-p-capital-9awo3s8q8xxd.html>). This makes joining VA line items to CIQ fundamentals trivial — but it also means VA's separate IDs (`viCompanyId` etc.) are slowly being subsumed into the CIQ symbology.

### 6.4 Pricing

- Add-on module to CIQ Pro; ~$40k+/yr incremental over base CIQ Pro `[unverified]`.
- Standalone VA Insights subscription available pre-integration; legacy pricing $25k–$100k `[unverified]`.

---

## 7. Estimize (defunct), TipRanks, Zacks — public-facing aggregators

These are commercial products that consume primary vendor feeds plus open data, then re-publish derived consensus at retail price points.

### 7.1 Estimize (defunct)

Founded 2011 by Leigh Drogen; positioned as the "Wikipedia of earnings estimates" — open submissions from any user (verified or not), aggregated to a community consensus. Academic literature (Jame, Johnston, Markov, Wolfe 2016; Da & Huang 2020) demonstrated the Estimize consensus had information content beyond IBES, particularly for retail-heavy names.

- **Status:** As of 2026-05, the public-facing site still serves pre-2024 data (source: <https://www.estimize.com/mstr/fq4-2023?fullsite=true>). Nasdaq Data Link's Estimize feed `[unverified — exact discontinuation date]` appears to have ceased updates after Estimize's pivot away from earnings data; the company's last social-media activity in this area lapsed in late 2023.
- **Schema (historical):** per (ticker × user_handle × fpedats × measure × submission_ts × value). Two consensus tiers: "Wall Street consensus" (Estimize's mirror of IBES) and "Estimize consensus" (their crowdsourced one).
- **Use for ats-eqt:** treat as historical archive (Quandl/Nasdaq Data Link archive ~$3k/yr `[unverified]`). Not a live source.

### 7.2 TipRanks

- 10,000+ analysts tracked across 23,000+ tickers globally (source: <https://www.tipranks.com/glossary/f/faq>).
- Per-analyst proprietary "star rating" 1–5 based on TipRanks' own win-rate scoring.
- **No official public API.** Data resold via Nasdaq Data Link (source: <https://data.nasdaq.com/publishers/TIPRANKS>).
- Fields surfaced (from Nasdaq Data Link and unofficial scrapers, source: <https://github.com/janlukasschroeder/tipranks-api-v2>):

```
ticker, analyst_name, firm_name,
rating              -- 'Buy', 'Hold', 'Sell', or 'Outperform' / 'Underperform'
price_target,
prior_rating,
prior_price_target,
recommendation_date,
analyst_star_rating, analyst_success_rate, analyst_avg_return,
news_sentiment_bullish, news_sentiment_bearish
```

- Use for ats-eqt: derivative consumer; useful only for the star-rating metadata not available elsewhere. Licence prohibits redistribution.

### 7.3 Zacks Investment Research

- Aggregator product; consumes broker research feeds; computes proprietary "Zacks Rank" 1..5.
- Has its own analyst-collection pipeline distinct from IBES, particularly for smaller-cap names where IBES coverage is sparse.
- Public-tier: free EPS consensus / surprise lookup on zacks.com.
- Paid retail: Zacks Premium ~$250/yr; Zacks Ultimate ~$3,000/yr (source: <https://www.zacks.com/ — pricing on subscription pages>).
- Institutional feed: separate enterprise SKU `[unverified]`.

---

## 8. Public-data reconstruction

What ats-eqt can build without buying a primary feed:

### 8.1 EDGAR 8-K Items 2.02 and 7.01 (Regulation FD guidance)

Form 8-K is the SEC's "current report" filed within 4 business days of a material event. Two items are relevant for company-issued estimates:

- **Item 2.02 — Results of Operations and Financial Condition.** Used for the press release that accompanies the earnings call. Disclosures are "furnished" not "filed" (source: <https://www.sec.gov/rules-regulations/staff-guidance/compliance-disclosure-interpretations/exchange-act-form-8-k>).
- **Item 7.01 — Regulation FD Disclosure.** Used when management broadcasts material info — including forward-looking guidance — that needs simultaneous public disclosure under Reg FD (source: <https://www.vorys.com/publication-Regulation-FD-A-Refresher-on-the-SEC-Rules-Governing-Selective-Disclosure>, <https://securities-law-blog.com/2023/07/25/regulation-fd/>).

**What can be extracted:** the press-release exhibit (typically Exhibit 99.1) is full natural-language prose. ats-eqt's pipeline:

1. EDGAR poller (same as 13F-loader): poll `data.sec.gov/submissions/CIK*.json` for `form_type='8-K'`.
2. Parse the 8-K cover XML to identify items checked (`Item 2.02`, `Item 7.01`).
3. Download exhibit `EX-99.1`, the press release.
4. Apply guidance-extraction NER (regex + LLM): patterns like `expects (full year )?(20\d\d|FY\d\d\d\d) (revenue|sales|EPS|earnings|net income) (of|to be|in the range of) \$?(\d[\d,]*\.?\d*)( to \$?(\d[\d,]*\.?\d*))?`.

**Critical limitation:** the **us-gaap XBRL taxonomy has no element for forward-looking guidance.** SEC has not mandated structured guidance disclosure. ats-eqt must accept text-extraction noise (source: <https://www.sec.gov/files/edgar/xbrl-guide.pdf>, <https://xbrl.us/home/priorities/data-quality/rules-guidance/principles/>).

**What ats-eqt's `est_guidance` table CAN derive from 8-K:**

```
period          -- usually FY or FQ
measure         -- EPS / SALES / EBITDA / NET_INC / CAPEX / FCF / OPM
guidance_low, guidance_high, guidance_point,
guidance_type   -- 'point', 'range', 'open_high', 'open_low'
guidance_currency,
guidance_units,
issue_ts        -- 8-K filing time
filing_id       -- accession
extraction_confidence  -- LLM/regex confidence
```

**What it CANNOT:** the broker-side estimate IDs. Public data gives only the company-issued half of the estimate-vs-actual reconciliation.

### 8.2 Earnings transcripts (Refinitiv StreetEvents, AlphaSense/Tegus, Seeking Alpha)

Earnings-call transcripts are a richer guidance source than press releases (CFOs often clarify mid-call). Options:

- **LSEG Transcripts & Briefs:** ~40,000 transcripts/yr, 10,400 global companies (source: <https://www.lseg.com/en/data-analytics/financial-data/company-data/events/earnings-transcripts-briefs/transcripts-database>). Paid.
- **AlphaSense + Tegus (merged Jul 2024):** 200,000+ proprietary expert-network transcripts; API access via `developer.alpha-sense.com` (source: <https://help.alpha-sense.com/hc/en-us/articles/43785894151699-Introduction-to-Tegus-Expert-Transcript-Library>).
- **Seeking Alpha:** free user-submitted transcripts; rate-limited scraping; ToS prohibits redistribution.
- **RTTNews, Motley Fool:** light editorial summaries; not transcript fidelity.

**Guidance NER from transcripts:** the same prose-extraction patterns from §8.1 apply. Transcripts add Q&A section content where guidance is often refined under analyst questioning.

### 8.3 Wall Street Horizon (event-data anchor)

WSH covers 11,000+ companies, 40+ event types (earnings dates, dividends, conferences, analyst days). API: REST + streaming; Interactive Brokers' TWS API exposes WSH for free with an IB account (source: <https://www.wallstreethorizon.com/ibkr-wsh>, <https://interactivebrokers.github.io/tws-api/wshe_filters.html>). Pricing: $49/mo retail, $149/mo institutional (source: <https://www.wallstreethorizon.com/>).

WSH does **not** provide estimate values — it provides the *dates* on which estimates can be expected. ats-eqt's use: `est_period_dim` table's `expected_release_date` column, anchored to WSH's `confirmed_earnings_date`.

### 8.4 Zacks / Yahoo / NASDAQ public consensus

Public scraping (subject to ToS and rate limits):

- **Zacks.com:** EPS consensus + surprise per ticker per period; free.
- **Yahoo Finance "Analysis" tab:** consensus EPS, revenue, growth rates, target price, recommendation distribution; free.
- **NASDAQ.com:** consensus + recommendation distribution; free.
- **MarketWatch:** similar coverage; free.

These all source from Refinitiv/IBES under license but expose the consensus aggregate publicly. ats-eqt can scrape, but the license-of-use boundary is murky: redistribution is prohibited but in-product display likely permissible under "fair use of publicly displayed prices" `[unverified — legal review required]`.

### 8.5 What cannot be derived from public data

- **Per-analyst individual estimates** — no public source.
- **Broker identity** — Yahoo/Zacks anonymize to "the analyst at Firm X" with no stable ID.
- **Pre-publication revisions** — only the latest published number is on public pages.
- **Currency-of-estimate detail for international** — public sources default to USD.
- **The estimator's own confidence / scenarios** — gone before publication.

### 8.6 Imputation strategy

For ats-eqt's public-data-only tier, the realistic synthesis:

1. Scrape public consensus (mean, median, high, low, N) from Zacks / Yahoo daily; persist with `source_id = 'public_aggregator'`.
2. Extract guidance from 8-K Item 2.02 / 7.01 prose + LLM extraction; persist to `est_guidance` with `extraction_confidence`.
3. Anchor period dim to Wall Street Horizon `expected_release_date`.
4. On actual release (8-K Item 2.02 with `EX-99.1` reporting actuals), populate `est_actual`.
5. Compute surprise (actual − consensus) and persist to `est_surprise` view.

This gives ats-eqt a credible **consensus + guidance + surprise** product at zero estimate-data licence cost. The missing piece — per-broker detail — is the upsell to IBES/FactSet subscribers.

---

## 9. Recommended ats-eqt schema

Fits the bitemporal long-format pattern in `schemas/data_models_and_methodology.md` §G.3. Extends the canonical `est_*` family.

### 9.1 Dimension tables

```sql
-- Estimator (broker) registry. Stable ats-eqt ID; vendor-specific aliases live in est_broker_alias.
CREATE TABLE est_broker (
  broker_id           BIGINT      PRIMARY KEY,
  legal_name          TEXT        NOT NULL,            -- e.g. 'Goldman Sachs & Co. LLC'
  short_name          TEXT        NULL,                -- e.g. 'GS'
  country_iso2        CHAR(2)     NULL,
  is_active           BOOLEAN     NOT NULL DEFAULT TRUE,
  first_seen          DATE        NULL,
  last_seen           DATE        NULL,
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31'
);

CREATE TABLE est_broker_alias (
  broker_id           BIGINT      NOT NULL REFERENCES est_broker,
  source_id           INTEGER     NOT NULL,            -- 'IBES', 'FACTSET', 'BLOOMBERG', 'CIQ', 'VISIBLE_ALPHA'
  vendor_broker_id    TEXT        NOT NULL,            -- vendor's native ID, as a string
  vintage             DATE        NOT NULL,            -- the IBES reshuffle date (or vendor-version anchor)
  valid_from          DATE        NOT NULL,
  valid_to            DATE        NOT NULL DEFAULT '9999-12-31',
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (source_id, vendor_broker_id, vintage, valid_from)
);

-- Analyst registry. Names rarely; mostly tracked via masked vendor codes.
CREATE TABLE est_analyst (
  analyst_id          BIGINT      PRIMARY KEY,
  broker_id           BIGINT      NULL REFERENCES est_broker,  -- current affiliation
  display_name        TEXT        NULL,                -- when public (FactSet/CIQ); NULL for IBES masked
  is_anonymous        BOOLEAN     NOT NULL DEFAULT TRUE,
  first_seen          DATE        NULL,
  last_seen           DATE        NULL,
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31'
);

CREATE TABLE est_analyst_alias (
  analyst_id          BIGINT      NOT NULL REFERENCES est_analyst,
  source_id           INTEGER     NOT NULL,
  vendor_analyst_id   TEXT        NOT NULL,            -- IBES amaskcd, FactSet analyst_id, etc.
  vintage             DATE        NOT NULL,
  PRIMARY KEY (source_id, vendor_analyst_id, vintage)
);

-- Measure dimension. Canonical ats-eqt code plus vendor-specific equivalents.
CREATE TABLE est_measure_dim (
  measure_id          INTEGER     PRIMARY KEY,
  canonical_code      TEXT        UNIQUE NOT NULL,     -- 'EPS', 'SALES', 'EBITDA', 'OPR', 'CFPS', …
  label               TEXT        NOT NULL,
  unit_type           TEXT        NOT NULL,            -- 'currency_per_share', 'currency', 'percent', 'count'
  is_per_share        BOOLEAN     NOT NULL DEFAULT FALSE,
  is_recommendation   BOOLEAN     NOT NULL DEFAULT FALSE,
  is_price_target     BOOLEAN     NOT NULL DEFAULT FALSE,
  is_kpi              BOOLEAN     NOT NULL DEFAULT FALSE,  -- industry-KPI flag
  ibes_code           CHAR(3)     NULL,                -- 'EPS', 'SAL', 'EBT', …
  factset_code        TEXT        NULL,                -- 'EPS', 'SALES', 'EBITDA', …
  bloomberg_field     TEXT        NULL,                -- 'BEST_EPS', 'BEST_SALES', …
  ciq_data_item_id    INTEGER     NULL,                -- CIQ numeric dataItemId
  industry_scope      TEXT        NULL                 -- when is_kpi: 'retail', 'airline', …
);

-- Period dimension. Encodes FPI / FY1 / FQ1 / NTM / etc.
CREATE TABLE est_period_dim (
  period_id           BIGINT      PRIMARY KEY,
  entity_id           BIGINT      NOT NULL,            -- the company being estimated
  period_type         CHAR(2)     NOT NULL,            -- 'FY','FQ','FS','CY','CQ','LT','NT' (LTG, NTM)
  fiscal_year         INTEGER     NULL,
  fiscal_quarter      SMALLINT    NULL,                -- 1..4 when period_type='FQ'
  period_end_date     DATE        NOT NULL,            -- the fpedats anchor
  fpi_ibes            CHAR(1)     NULL,                -- IBES FPI mirror
  factset_per_rel     TEXT        NULL,                -- 'FY1', 'FQ2', …
  expected_release_date DATE      NULL,                -- from Wall Street Horizon
  confirmed_release_date DATE     NULL,                -- post-announce, the actual release date
  UNIQUE (entity_id, period_type, period_end_date)
);
```

### 9.2 Fact tables

```sql
-- The detail fact: per-broker, per-analyst, per-measure, per-period estimate.
CREATE TABLE est_detail (
  entity_id           BIGINT      NOT NULL,
  security_id         BIGINT      NULL,
  broker_id           BIGINT      NOT NULL REFERENCES est_broker,
  analyst_id          BIGINT      NULL REFERENCES est_analyst,
  measure_id          INTEGER     NOT NULL REFERENCES est_measure_dim,
  period_id           BIGINT      NOT NULL REFERENCES est_period_dim,
  value_num           DOUBLE      NULL,
  currency_iso3       CHAR(3)     NOT NULL DEFAULT 'USD',
  units_scale         INTEGER     NOT NULL DEFAULT 1,  -- 1, 1000, 1000000
  basis               CHAR(1)     NULL,                -- 'P' primary, 'D' diluted (mirrors IBES pdf)
  is_gaap             BOOLEAN     NULL,                -- TRUE = GAAP-basis, FALSE = adjusted/non-GAAP
  estimate_type       CHAR(1)     NOT NULL DEFAULT 'A', -- 'A'ctive, 'S'topped, 'P'reliminary (FactSet P-Number)
  anndats             TIMESTAMP   NOT NULL,            -- IBES anndats / FactSet estimate_date
  actdats             TIMESTAMP   NULL,                -- IBES actdats (when vendor ingested)
  revdats             TIMESTAMP   NULL,                -- IBES revdats (last confirmed)
  prior_value         DOUBLE      NULL,                -- value before this revision
  source_id           INTEGER     NOT NULL,            -- 'IBES' | 'FACTSET' | 'BLOOMBERG' | 'CIQ' | 'VISIBLE_ALPHA' | 'PUBLIC_AGGREGATOR'
  source_record_hash  TEXT        NULL,
  -- Bitemporal columns
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (entity_id, broker_id, analyst_id, measure_id, period_id, anndats, source_id)
);
CREATE INDEX ix_est_detail_pit ON est_detail (entity_id, measure_id, period_id, knowledge_from);
CREATE INDEX ix_est_detail_broker ON est_detail (broker_id, anndats);
CREATE INDEX ix_est_detail_period ON est_detail (period_id, measure_id);

-- The consensus fact: aggregate stats per measure / period / snapshot.
CREATE TABLE est_consensus (
  entity_id           BIGINT      NOT NULL,
  security_id         BIGINT      NULL,
  measure_id          INTEGER     NOT NULL REFERENCES est_measure_dim,
  period_id           BIGINT      NOT NULL REFERENCES est_period_dim,
  snapshot_date       DATE        NOT NULL,            -- IBES statpers, FactSet consensus_date
  mean_est            DOUBLE      NULL,
  median_est          DOUBLE      NULL,
  high_est            DOUBLE      NULL,
  low_est             DOUBLE      NULL,
  stdev_est           DOUBLE      NULL,
  num_est             INTEGER     NOT NULL DEFAULT 0,
  num_up_30d          INTEGER     NULL,
  num_down_30d        INTEGER     NULL,
  mean_30d_ago        DOUBLE      NULL,
  mean_60d_ago        DOUBLE      NULL,
  mean_90d_ago        DOUBLE      NULL,
  currency_iso3       CHAR(3)     NOT NULL DEFAULT 'USD',
  source_id           INTEGER     NOT NULL,            -- which vendor (or 'DERIVED' if computed from est_detail)
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (entity_id, measure_id, period_id, snapshot_date, source_id)
);
CREATE INDEX ix_est_consensus_pit ON est_consensus (entity_id, measure_id, period_id, knowledge_from);

-- Actuals: as-reported by the company. Powers surprise.
CREATE TABLE est_actual (
  entity_id           BIGINT      NOT NULL,
  security_id         BIGINT      NULL,
  measure_id          INTEGER     NOT NULL REFERENCES est_measure_dim,
  period_id           BIGINT      NOT NULL REFERENCES est_period_dim,
  actual_value        DOUBLE      NOT NULL,
  currency_iso3       CHAR(3)     NOT NULL DEFAULT 'USD',
  units_scale         INTEGER     NOT NULL DEFAULT 1,
  basis               CHAR(1)     NULL,                -- 'P' / 'D'
  is_gaap             BOOLEAN     NULL,
  release_ts          TIMESTAMP   NOT NULL,            -- when company released
  filing_id           BIGINT      NULL,                -- → filing (8-K accession)
  source_id           INTEGER     NOT NULL,
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (entity_id, measure_id, period_id, release_ts, source_id)
);

-- Recommendations: per analyst, per ticker, per event.
CREATE TABLE est_recommendation (
  entity_id           BIGINT      NOT NULL,
  security_id         BIGINT      NULL,
  broker_id           BIGINT      NOT NULL REFERENCES est_broker,
  analyst_id          BIGINT      NULL REFERENCES est_analyst,
  recommendation      SMALLINT    NOT NULL,            -- ATS canonical: 1=StrongBuy 2=Buy 3=Hold 4=Underperform 5=Sell
  itext               TEXT        NULL,                -- free-text label as published
  price_target        DOUBLE      NULL,                -- optional companion target
  target_currency     CHAR(3)     NULL,
  target_horizon_mo   SMALLINT    NULL DEFAULT 12,
  anndats             TIMESTAMP   NOT NULL,
  revdats             TIMESTAMP   NULL,
  prior_recommendation SMALLINT   NULL,
  source_id           INTEGER     NOT NULL,
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (entity_id, broker_id, analyst_id, anndats, source_id)
);

-- Guidance: company-issued forecast.
CREATE TABLE est_guidance (
  entity_id           BIGINT      NOT NULL,
  measure_id          INTEGER     NOT NULL REFERENCES est_measure_dim,
  period_id           BIGINT      NOT NULL REFERENCES est_period_dim,
  guidance_type       CHAR(8)     NOT NULL,            -- 'POINT', 'RANGE', 'OPEN_HI', 'OPEN_LO', 'QUAL'
  guidance_low        DOUBLE      NULL,
  guidance_high       DOUBLE      NULL,
  guidance_point      DOUBLE      NULL,
  guidance_currency   CHAR(3)     NULL,
  units_scale         INTEGER     NOT NULL DEFAULT 1,
  is_gaap             BOOLEAN     NULL,
  issue_ts            TIMESTAMP   NOT NULL,
  guid_source         TEXT        NOT NULL,            -- '8-K_2.02', '8-K_7.01', 'EarningsCall', 'InvestorDay', 'PressRelease'
  filing_id           BIGINT      NULL,                -- → filing (when 8-K)
  consensus_at_issue  DOUBLE      NULL,                -- mean consensus at the moment of guidance
  beat_meet_miss      CHAR(1)     NULL,                -- 'B', 'M', 'S' vs consensus
  extraction_confidence DOUBLE    NULL,                -- 0..1 — NER confidence for public-data sources
  source_id           INTEGER     NOT NULL,
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (entity_id, measure_id, period_id, issue_ts, source_id)
);
```

### 9.3 Materialized convenience views

```sql
-- Surprise view: consensus immediately before release vs actual.
CREATE VIEW est_surprise AS
SELECT
  a.entity_id, a.measure_id, a.period_id, a.actual_value, a.release_ts,
  c.mean_est AS consensus_mean,
  c.median_est AS consensus_median,
  c.stdev_est AS consensus_stdev,
  c.num_est AS num_analysts,
  (a.actual_value - c.mean_est)         AS surprise_abs,
  (a.actual_value - c.mean_est) / NULLIF(ABS(c.mean_est), 0) AS surprise_pct,
  (a.actual_value - c.mean_est) / NULLIF(c.stdev_est, 0)      AS surprise_zscore,
  CASE
    WHEN a.actual_value > c.mean_est + 0.5*c.stdev_est THEN 'B'
    WHEN a.actual_value < c.mean_est - 0.5*c.stdev_est THEN 'S'
    ELSE 'M'
  END AS beat_meet_miss
FROM est_actual a
JOIN LATERAL (
  SELECT * FROM est_consensus
  WHERE entity_id = a.entity_id
    AND measure_id = a.measure_id
    AND period_id  = a.period_id
    AND snapshot_date < a.release_ts::DATE
  ORDER BY snapshot_date DESC LIMIT 1
) c ON TRUE
WHERE a.knowledge_to = '9999-12-31';

-- Latest consensus per entity / measure / period.
CREATE MATERIALIZED VIEW est_consensus_latest AS
SELECT DISTINCT ON (entity_id, measure_id, period_id, source_id)
  entity_id, measure_id, period_id, source_id,
  snapshot_date, mean_est, median_est, high_est, low_est, stdev_est, num_est
FROM est_consensus
WHERE knowledge_to = '9999-12-31'
ORDER BY entity_id, measure_id, period_id, source_id, snapshot_date DESC;
```

### 9.4 Ingestion outline

1. **Vendor adapter layer** — separate ETLs for IBES (`ibes.detu_epsus`, etc.), FactSet (`factset-estimates-api`), Bloomberg (Enterprise Data delivery), CIQ Xpressfeed, Visible Alpha (via CIQ post-2025), public scrapers.
2. **Map MEASURE / item / dataItemId → `measure_id`** via the `est_measure_dim` translation table.
3. **Map FPI / FY1-FQ2 / estimatePeriodTypeId → `est_period_dim`** with company's fiscal calendar.
4. **Resolve broker / analyst** via `est_broker_alias` / `est_analyst_alias`; queue unmatched for manual reconciliation (especially around IBES reshuffles).
5. **Bitemporal upsert** — never UPDATE in place. Close prior row's `knowledge_to`, INSERT new row with `knowledge_from = now()`.
6. **Derive consensus** when vendor only provides detail (rare); skip when vendor provides own consensus to avoid contamination.
7. **Compute surprise** as view materialization on `est_actual` arrival.

---

## 10. Strategic positioning for ats-eqt

### 10.1 Where the moat lives

For estimates, the moat is **NOT** in collecting the data (impossible without paying a vendor) but in:

1. **Cross-vendor reconciliation** — buyers who pay both IBES and FactSet need a single normalized view. ats-eqt's `est_measure_dim` translation alone is a deliverable.
2. **Public-data + premium-data hybrid** — for buyers who don't want to pay for IBES Detail but want a useful consensus + guidance + surprise view.
3. **Transcript-NER guidance extraction** — most buyers don't have an in-house NER team. A clean `est_guidance` table from 8-K + transcript scraping at $5k–$25k/yr is a real product.
4. **Wall Street Horizon-anchored period dim** — confirmed earnings dates synced to estimate periods is a developer-experience differentiator.

### 10.2 Customer cohorts

1. **Academic / replication-package buyers.** Currently use WRDS IBES + ICLINK. Pain: PIT-replication accuracy across IBES vintages; the broker-ID reshuffle problem. ats-eqt's value: source-versioned vintage tracking and bitemporal queries.
2. **Quant researchers at smaller funds.** Currently use IBES via WRDS, can't afford FactSet Detail. ats-eqt's value: hybrid product — public-derived consensus + guidance for free, premium IBES detail as an upgrade.
3. **Retail data API consumers.** Currently use Finnhub / Financial Modeling Prep / TipRanks-via-Nasdaq-Data-Link. Build a $30–$200/mo tier that beats those on guidance + surprise coverage.

### 10.3 12-month build to feature parity with a mid-tier estimates product

| Month | Milestone |
|---|---|
| M0 | `est_measure_dim` populated with 50 canonical codes + IBES/FactSet/Bloomberg/CIQ mappings. |
| M1 | EDGAR 8-K loader (extends existing 13F loader) — Item 2.02 + 7.01 extraction; press-release exhibit retrieval. |
| M2 | Guidance-extraction NER pipeline (regex + LLM); `est_guidance` populated for S&P 500 backfill. |
| M3 | Wall Street Horizon integration; `est_period_dim` anchored to confirmed dates. |
| M4 | Public-consensus scraper (Zacks / Yahoo) with ToS-compliant rate limit; `est_consensus` populated with `source_id='PUBLIC_AGGREGATOR'`. |
| M5 | `est_actual` populated from 8-K Item 2.02 press releases. |
| M6 | `est_surprise` view live; backtest accuracy validated against IBES Summary on a sample. |
| M7 | First premium tier: IBES Detail ingestion (WRDS or LSEG direct). |
| M8 | FactSet Estimates ingestion via Open:FactSet. |
| M9 | CIQ Estimates ingestion via Xpressfeed (or CIQ via Snowflake share). |
| M10 | Cross-vendor reconciliation views; broker-alias-resolution UI. |
| M11 | Visible Alpha line-item ingestion via CIQ Pro integration. |
| M12 | Bulk parquet / Snowflake share for academic and quant customers. |

---

## 11. Open questions / wave-3 gaps

| Topic | What's unverified | Path to resolve |
|---|---|---|
| Full IBES MEASURE enumeration | The full 30+ list from the official IBES Detail History User Guide is in PDFs that WebFetch can't parse | Manual download of <https://www.library.kent.edu/files/IBES_GuideUS.pdf> and <https://www.unisg.ch/.../IBES_Detail_History_User_Guide.pdf> |
| IBES Guidance field schema | `ibes.cig` exact columns not in public docs | WRDS subscriber access to data dictionary |
| FactSet item code full list | 125+ standard + 800+ KPI items; only ~30 sampled here | FactSet developer portal authentication required |
| Bloomberg BEst point-in-time history-start | Vendor does not publish per-field history-start matrix | Bloomberg sales / Terminal `BEST <GO>` documentation |
| CIQ Estimates relational schema | `ciqEstimateNumericData` exact DDL gated | S&P CIQ Pro subscriber data dictionary; Xpressfeed Setup Guide |
| Visible Alpha schema | No public schema docs | S&P Marketplace gated documentation |
| FactSet P-Number licence terms | Pre-announcement field treatment unclear | FactSet contract review |
| Estimize current operational status (post-2024) | Site serves stale data; no announced shutdown | Direct outreach to Estimize/Drogen or Nasdaq Data Link |
| Zacks aggregator licence-of-use boundary | Whether scraping consensus from zacks.com is permissible for ats-eqt's commercial use | Legal review |
| us-gaap XBRL guidance taxonomy proposal | Any pending SEC rulemaking on structured guidance disclosure | SEC rulemaking calendar; XBRL US advocacy notes |

**PDFs that WebFetch could not parse during this research (raw binary returned):**

- `https://www.library.kent.edu/files/IBES_GuideUS.pdf`
- `https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf`
- `https://www.library.kent.edu/files/IBES_Summary_History_User_Guide_December_2009.pdf`
- `https://www.ckgsb.edu.cn/uploads/report/file/201411/28/1417166880813620.pdf` (IBES Guidance User Guide)
- `https://faculty.weatherhead.case.edu/llm17/documents/ANoteonIndustryRecommendationsinIBES.pdf`
- `https://www.refinitiv.com/content/dam/marketing/en_us/documents/fact-sheets/ibes-estimates-fact-sheet.pdf` (redirects to LSEG landing page)

For wave-3, these should be pulled manually and re-parsed offline.

---

## 12. Sources

### IBES / LSEG primary
- <https://en.wikipedia.org/wiki/Institutional_Brokers%27_Estimate_System> — IBES history & ownership chain
- <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/> — WRDS IBES product overview
- <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/vendor-partner-ibes/> — WRDS vendor-partner page, broker-ID reshuffle disclosure
- <https://wrds-www.wharton.upenn.edu/demo/ibes/form/> — WRDS IBES Detail demo
- <https://wrds-www.wharton.upenn.edu/documents/5/A_Note_on_IBES_Unadjusted_Data_pdf.pdf> — WRDS note on IBES unadjusted track
- <https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf> — Tilburg e-Learning IBES on WRDS
- <https://www.library.kent.edu/files/IBES_GuideUS.pdf> — Kent State IBES Detail History guide
- <https://www.library.kent.edu/files/IBES_Summary_History_User_Guide_December_2009.pdf> — IBES Summary History guide
- <https://www.library.kent.edu/files/IBES_Key_Performance_Indicators_Datafeed_User_Guide_July_2009.pdf> — IBES KPI feed
- <https://www.unisg.ch/fileadmin/user_upload/HSG_ROOT/_Kernauftritt_HSG/Universitaet/Bibliothek/Suchen_und_Nutzen/Datenbanken/Datenbankseiten/A-Z/IBES_Summary_History_User_Guide.pdf> — IBES Summary History 2013 version
- <https://www.unisg.ch/fileadmin/user_upload/HSG_ROOT/_Kernauftritt_HSG/Universitaet/Bibliothek/Suchen_und_Nutzen/Datenbanken/Datenbankseiten/A-Z/IBES_Detail_History_User_Guide.pdf> — IBES Detail History 2013 version
- <https://www.unisg.ch/fileadmin/user_upload/HSG_ROOT/_Kernauftritt_HSG/Universitaet/Bibliothek/Suchen_und_Nutzen/Datenbanken/Datenbankseiten/A-Z/IBES_QFS_User_Guide_February_2021.pdf> — IBES Quantitative File System
- <https://libapp.lib.ncku.edu.tw/libref/handout/20110107_IBES_user_guide.pdf> — IBES on Datastream 2010 user guide
- <https://manchester-uk.libanswers.com/loader?fid=10871&type=1&key=c5f6a61d6ce6662dbf91abaaa3c8a138> — IBES on Datastream 2020
- <https://www.refinitiv.com/en/financial-data/company-data/ibes-estimates> — Refinitiv IBES product page
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/ibes-estimates> — LSEG IBES page
- <https://www.ckgsb.edu.cn/uploads/report/file/201411/28/1417166880813620.pdf> — IBES Guidance user guide (PDF blocked)
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/events/earnings-transcripts-briefs/transcripts-database> — LSEG Transcripts & Briefs
- <https://wrds-support.wharton.upenn.edu/hc/en-us/articles/115003135791-Meaning-of-the-Analyst-Codes-in-IBES-International-Data> — IBES international analyst-code masking

### IBES — academic & code references
- <https://faculty.weatherhead.case.edu/llm17/documents/ANoteonIndustryRecommendationsinIBES.pdf> — Industry recommendations in IBES
- <https://researchfinancial.wordpress.com/2020/07/31/quarterly-ibes-data-in-wrds/> — FPI quarterly walkthrough
- <https://www.bhwang.com/txt/Earnings-Surprise-Code.txt> — Earnings-surprise SAS code with IBES field list
- <https://github.com/jblocher/sas_util/blob/master/ibes_sample.sas> — IBES sample SAS programs
- <https://www.fredasongdrechsler.com/data-crunching/iclink> — ICLINK methodology
- <https://wrds-www.wharton.upenn.edu/pages/classroom/using-ibes-crsp-linking-table/> — WRDS ICLINK classroom
- <https://gist.github.com/JoostImpink/0e5a8ae738cc8ef14baf> — Compustat+CRSP+IBES identifier gist
- <https://www.kaichen.work/?p=358> — Linking Audit Analytics, Compustat, CRSP, IBES
- <https://gsb-research-help.stanford.edu/library/faq/299055> — Stanford GSB IBES download FAQ
- <https://faq.library.princeton.edu/econ/faq/11479> — Princeton library IBES FAQ
- <https://libguides.vu.nl/finding-data/ibes> — VU Amsterdam IBES guide
- <https://www.wallstreetoasis.com/resources/data/bloomberg/ibes> — WSO IBES summary
- <https://ceibs.libguides.com/blogs/news/newresources/home/ibes-earnings-estimates-the-global-analyst-consensus> — CEIBS IBES overview
- <https://datahub.aalto.fi/en/data-sources/ibes-estimates-ibes> — Aalto IBES datahub

### FactSet
- <https://www.factset.com/marketplace/catalog/product/factset-estimates-consensus> — Estimates Consensus product
- <https://www.factset.com/marketplace/catalog/product/factset-estimates-detail> — Estimates Detail product
- <https://insight.factset.com/resources/factset-consensus-estimates-datafeed> — Consensus Estimates DataFeed brief
- <https://insight.factset.com/resources/at-a-glance-factset-estimates-point-in-time-consensus> — PIT Consensus brief
- <https://developer.factset.com/api-catalog/factset-estimates-api> — Estimates API
- <https://developer.factset.com/api-catalog/factset-estimates-report-builder-api> — Estimates Report Builder API
- <https://developer.factset.com/api-catalog/symbology-api> — Symbology API
- <https://www.factset.com/marketplace/catalog/product/factset-id-lookup-api> — ID Lookup API
- <https://assets.ctfassets.net/lmz2w5z92b9u/7INM5wpJ5u1bomIisoOoz2/beaad6e64bbbdc96f8996acc9c8a1b34/FactSet_Permanent_Security_Identifier.pdf> — FactSet Permanent Security ID
- <https://assets.ctfassets.net/lmz2w5z92b9u/4lgmnx6AJd0QZEMr89Us8G/83d9b8f1dded00da07e626c4d1c57b0f/FactSet_ID_Lookup_API_User_Guide_1_.pdf> — ID Lookup API guide
- <https://go.factset.com/hubfs/Website_Downloads/Statistical%20Package%20Integration/Docs%203.0/estimates-ondemand.pdf> — Estimates OnDemand 2015 reference
- <https://go.factset.com/hubfs/Website/Website_Downloads/Statistical%20Package%20Integration/factset%20ondemand%20web%20services%20reference%20manual_2.0.pdf> — OnDemand Web Services manual
- <https://doc.exabel.com/dsl/data_signals/factset_estimates.html> — Exabel FactSet Estimates DSL
- <https://open.factset.com/products/factset-estimates-consensus/en-us> — Open:FactSet Marketplace
- <https://aws.amazon.com/marketplace/pp/prodview-2bmdcxxw7flla> — AWS Marketplace FactSet Consensus
- <https://marketplace.databricks.com/details/b8e65142-68df-4aca-912f-1063b5c08555/FactSet_FactSet-Estimates-Consensus> — Databricks FactSet share

### Bloomberg
- <https://www.bloomberg.com/professional/dataset/global-bloomberg-estimates-data/> — Global Bloomberg Estimates dataset
- <https://www.bloomberg.com/professional/products/data/enterprise-catalog/cofi/> — COFI: Co. Financials, Estimates, Pricing PIT
- <https://www.bloomberg.com/company/press/bloomberg-launches-point-in-time-data-solution-that-gives-quants-a-competitive-edge/> — PIT launch press
- <https://www.bloomberg.com/professional/insights/markets/tools-to-enhance-your-earnings-season-analysis/> — Bloomberg earnings tooling
- <https://www.bloomberg.com/professional/insights/data/earning-surprise-betting-strategy-performance/> — Earnings surprise strategy
- <https://www.bloomberg.com/professional/insights/markets/bloomberg-pro-tips-eps-forecasting-for-indices-on-eeg/> — EEG pro tips
- <https://libguides.nypl.org/c.php?g=1084166&p=8024589> — NYPL Bloomberg Earnings & Estimates
- <https://bpb-us-e2.wpmucdn.com/sites.utdallas.edu/dist/8/1090/files/2021/03/bloomberg_commands.pdf> — UTD Bloomberg commands
- <https://studylib.net/doc/25233007/best-fperiod-override> — BEST_FPERIOD_OVERRIDE reference
- <https://www.wu.ac.at/fileadmin/wu/s/library/databases_info_image/Bloomberg_BQL_Fundamentals_FactSheet.pdf> — Bloomberg BQL Fundamentals
- <https://library.wu.ac.at/bib/fit4research/wp-content/uploads/2024/02/Forecasts_manuals_Bloomberg.pdf> — Forecasts in Bloomberg manual
- <https://pages.stern.nyu.edu/~adamodar/pdfiles/BloombergGuide.pdf> — Damodaran Bloomberg guide
- <https://assets.bbhub.io/professional/sites/10/Bloomberg-US-Analyst-Recommendations-Index-Methodology.pdf> — Bloomberg US Analyst Recs Index methodology
- <https://www.researchgate.net/figure/Features-and-indicators-used-in-this-study-BEst-ratings-BEst-EPS-BEst-CAPEX-etc_tbl1_349716021> — BEst feature inventory

### S&P Capital IQ
- <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro> — Capital IQ Pro
- <https://www.marketplace.spglobal.com/en/datasets/s-p-capital-iq-estimates-(1)> — Marketplace CIQ Estimates
- <https://www.spglobal.com/market-intelligence/en/solutions/resources/estimates-expansion> — Estimates Expansion
- <http://larryschrenk.com/Capital%20IQ/Excel%20Plug-in%20Manual.pdf> — CIQ Excel Plug-in manual
- <https://www.wu.ac.at/fileadmin/wu/s/library/databases_info_image/S_P_Capital_IQ_Excel_Plug-in_Template_Guide_.pdf> — CIQ Excel template guide
- <https://www.scribd.com/doc/257157748/CIQ-Excel-Cheat-Sheet-June-2012> — CIQ formula cheat sheet
- <https://wrds-www.wharton.upenn.edu/pages/wrds-research/database-linking-matrix/linking-capital-iq-with-compustat/> — Linking CIQ ↔ Compustat
- <https://corporatefinanceinstitute.com/resources/valuation/capiq/> — CFI Capital IQ overview

### Visible Alpha
- <https://www.spglobal.com/market-intelligence/en/solutions/visible-alpha> — VA on S&P Global
- <https://press.spglobal.com/2024-05-01-S-P-Global-Announces-Successful-Completion-of-Visible-Alpha-Acquisition> — VA acquisition close
- <https://investor.spglobal.com/news-releases/news-details/2024/SP-Global-agrees-to-acquire-Visible-Alpha-enhancing-investment-research-capabilities-in-SP-Capital-IQ-Pro-Platform/default.aspx> — VA acquisition announcement
- <https://www.prnewswire.com/news-releases/sp-global-market-intelligence-launches-visible-alpha-on-sp-capital-iq-pro-platform-302409680.html> — VA on CIQ Pro launch
- <https://www.stocktitan.net/news/SPGI/s-p-global-market-intelligence-launches-visible-alpha-on-s-p-capital-9awo3s8q8xxd.html> — VA-CIQ integration
- <https://visiblealpha.com/> — Visible Alpha legacy site
- <https://www.spglobal.com/market-intelligence/en/solutions/products/visible-alpha-insights> — VA Insights
- <https://www.spglobal.com/market-intelligence/en/solutions/visible-alpha-estimates> — VA Estimates page

### Estimize, TipRanks, Zacks (aggregators)
- <https://www.estimize.com/mstr/fq4-2023?fullsite=true> — Estimize MSTR FQ4 2023
- <https://blog.estimize.com/post/153874632337/dont-let-these-5-stocks-destroy-your-portfolio> — Estimize blog
- <https://data.nasdaq.com/publishers/TIPRANKS> — Nasdaq Data Link TipRanks publisher
- <https://github.com/janlukasschroeder/tipranks-api-v2> — Unofficial TipRanks API
- <https://www.tipranks.com/glossary/f/faq> — TipRanks FAQ
- <https://www.zacks.com/> — Zacks Investment Research

### Public-data alternatives — EDGAR & 8-K
- <https://www.sec.gov/files/form8-k.pdf> — Form 8-K specification
- <https://www.sec.gov/rules-regulations/staff-guidance/compliance-disclosure-interpretations/exchange-act-form-8-k> — SEC Form 8-K C&DIs
- <https://www.wilmerhale.com/-/media/files/shared_content/editorial/publications/documents/20241217-keeping-current-with-form-8-k-a-practical-guide-2024-update.pdf> — WilmerHale Form 8-K practical guide
- <https://viewpoint.pwc.com/dt/us/en/pwc/pwc_sec_volume/pwc_sec_volume_US/3000_registration_an_US/sec_3150_form_8k_cur_US.html> — PwC 8-K viewpoint
- <https://www.vorys.com/publication-Regulation-FD-A-Refresher-on-the-SEC-Rules-Governing-Selective-Disclosure> — Vorys Reg FD refresher
- <https://securities-law-blog.com/2023/07/25/regulation-fd/> — Reg FD blog
- <https://www.friedfrank.com/news-and-insights/form-8-k-in-a-nutshell-10950> — Fried Frank Form 8-K nutshell
- <https://sec-api.io/resources/analyze-8-k-filings-and-material-event-disclosure-activity> — sec-api.io 8-K analysis
- <https://www.sec.gov/files/edgar/xbrl-guide.pdf> — SEC EDGAR XBRL Guide
- <https://xbrl.us/home/priorities/data-quality/rules-guidance/principles/> — XBRL US guidance principles

### Public-data alternatives — events & transcripts
- <https://www.wallstreethorizon.com/> — Wall Street Horizon home
- <https://www.wallstreethorizon.com/earnings-calendar> — WSH earnings calendar
- <https://www.wallstreethorizon.com/ibkr-wsh> — WSH Interactive Brokers integration
- <https://interactivebrokers.github.io/tws-api/wshe_filters.html> — TWS API WSH filters
- <https://help.alpha-sense.com/hc/en-us/articles/43785894151699-Introduction-to-Tegus-Expert-Transcript-Library> — Tegus on AlphaSense
- <https://developer.alpha-sense.com/api/next/getting-started/sample-use-cases/etl> — AlphaSense ETL API
- <https://www.alpha-sense.com/compare/alphasense-and-tegus/> — AlphaSense + Tegus merge
- <https://datarade.ai/data-products/wall-street-horizon-corporate-event-data-earnings-calendar-wall-street-horizon> — Datarade WSH listing

### Cross-referenced
- <https://intrinio.com/blog/consensus-estimates-101-a-beginners-guide> — Intrinio consensus estimates intro
- <https://guides.newman.baruch.cuny.edu/Earnings> — Baruch earnings library guide
- <https://www.koyfin.com/blog/best-platforms-earnings-estimates-price-targets-analyst-ratings/> — Koyfin platform comparison
- <https://www.benzinga.com/money/finance-api> — Benzinga finance API roundup

---

**Confirm:**

- File path: `c:/Users/natha/OneDrive/Desktop/C/ats/ats-eqt/research/datasets/estimates.md`
- Section count: **13 top-level parts** (0 Executive summary; 1 Vendor matrix; 2 IBES; 3 FactSet; 4 Bloomberg; 5 CIQ; 6 Visible Alpha; 7 Aggregators; 8 Public-data reconstruction; 9 Recommended schema; 10 Strategic positioning; 11 Open questions; 12 Sources), with 50+ sub-sections.
