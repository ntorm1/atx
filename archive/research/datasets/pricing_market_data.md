# Pricing & Market Data — vendor schemas + public reconstruction

**Status:** Research, v0.1
**Audience:** ats-eqt engineering team (ingestion, storage, query); ats-core team designing the columnar substrate for daily + intraday bars
**Scope:** the equity pricing layer — daily and intraday OHLCV, adjusted prices, total-return indices, market cap, shares outstanding, ADTV, short interest, and the surrounding identifier and options metadata, across CRSP, Compustat CCM, FactSet, Bloomberg, LSEG/Refinitiv, NYSE TAQ, Polygon, Tiingo, Alpaca, IEX, Yahoo, FINRA, and the major short-interest commercial vendors. Public-data reconstruction paths and a recommended ats-eqt schema that fits the bitemporal long-format pattern in `schemas/data_models_and_methodology.md`.
**Last updated:** 2026-05-14

---

## 0. Executive summary

Pricing is the join key for everything else. Every back-test, every factor model, every PIT join in ats-eqt — fundamentals, ownership, supply chain, ESG — ultimately resolves a `(security_id, as-of-date) → adjusted_close, total_return, market_cap` lookup. Get the price stack wrong and every downstream product inherits the error.

Five headline findings from this research pass:

1. **CRSP is still the academic gold standard but no longer the only option.** The DSF/MSF schema (PERMNO, PRC, RET, CFACPR, CFACSHR, SHROUT) has been stable since 1962 and is what every credible academic paper uses, but the institutional buyer can substitute FactSet OA Hub Prices, Bloomberg DL Pricing, or LSEG Datastream with comparable PIT discipline and broader global coverage. The differentiator is **delisting return handling** — CRSP's `dlret` / `dlstcd` fields and the Shumway-Warther adjustment remain the cleanest open methodology for survivorship-free returns (source: <https://ionmihai.github.io/finsets/01_wrds/crspd.html>).
2. **"Adjusted close" means three different things across vendors.** CRSP exposes raw `PRC` plus a cumulative price factor `CFACPR` (split-adjusted price = `PRC / CFACPR`) and a separate cumulative shares factor `CFACSHR`. Compustat ships `PRCCD` (close) + `AJEXDI` (per-day adjustment factor) + `TRFD` (total-return factor) in `co_secd`/`sec_dprc`/`sec_dtrt`. Tiingo and Yahoo ship a single `adjClose` that conflates splits + dividends. Polygon's `adjusted=true` flag applies split adjustment only. Naive cross-vendor joins on "adjusted close" are wrong by single-digit basis points on stable names and by 5–30% on names with large special dividends or spinoffs.
3. **The post-2023 retail-API tier is good enough for survivorship-free daily bars going forward, but the deep history still requires WRDS or an LSEG contract.** Polygon's `/v2/aggs` endpoint returns a clean `(o, h, l, c, v, vw, n)` schema with 50k-bar/request batches and 5 years of history at the $29/mo Starter tier (source: <https://polygon.io/pricing>). Tiingo gives 30+ years for the largest names at $10–$50/mo. Alpaca's free paper-data feed is IEX-only (~3% of consolidated volume) which limits its use for back-testing but is fine for live paper trading.
4. **Intraday is a different cost curve.** Daily bars are commoditised at sub-$50/month. Full intraday — every trade and every quote at microsecond precision — is still a per-TB problem. NYSE Daily TAQ raw files compress to roughly 3-6 GB/trading day across Trades + Quotes + NBBO (source: <https://www.nyse.com/publicdocs/nyse/data/Daily_TAQ_Client_Spec_v4.2.pdf>); WRDS hosts the cleaned mirror from 1993-09 forward. Polygon's full-tick Advanced tier ($199–$499/mo) and Databento's per-symbol-day pricing are the only sub-$10k/yr ways to get raw OPRA-style intraday quote data; everything else clears six figures (source: <https://databento.com/datasets/OPRA.PILLAR>).
5. **Short interest is the worst-served headline series in equity data.** FINRA bi-monthly settle-date short interest is free, but **on a 4-6 business-day lag from settlement** and at security-level granularity only — manager-level short positions are not publicly observable in the US until Form SHO Rule 13f-2 first filings, now extended to **2028-02-14** (source: <https://www.sec.gov/newsroom/press-releases/2025-37>; see `13f_holdings.md` §G.5). FINRA *daily* short-volume files (free) report short trades by exchange/TRF and are useful as a high-frequency proxy but are not a substitute for the settle-date stock figure. S3 Partners' Black App is the commercial gold standard but starts at ~$25k/yr (source: <https://www.s3partners.com/short-interest-data>; pricing `[unverified]`).

The recommendation in §7 is a bitemporal long-format pricing schema with five tables — `bar_daily`, `bar_intraday`, `quote_eod`, `shares_outstanding_history`, `short_interest`, plus a cross-link `adjustment_factor_history` that anchors `corporate_actions.md` (forthcoming, wave-3) to the daily bar table.

---

## 1. Why this dataset matters

Pricing is the dataset every other dataset joins to. The full transitive closure of joins from `bar_daily` covers:

- **Back-tests.** Total-return calculation: `cum_return(t0, t1) = ∏ (1 + RET_d)` requires daily total returns. A single missing dividend re-investment day produces a ~1% bias per missed coupon on a high-dividend name.
- **Factor construction.** Book-to-market uses `(book equity / mkt_cap)` where `mkt_cap = PRC × SHROUT` from CRSP, or `PRCCD × CSHOC` from Compustat. The two answers differ because CRSP `SHROUT` is in thousands and only updated at month-end (DSF) or daily (DSF for some PERMCOs), while Compustat `CSHOC` is filing-driven and updates only on 10-Q/8-K (source: <http://kaichen.work/?p=248>).
- **Survivorship-free universes.** The textbook 1.6%/yr survivorship bias (Brown-Goetzmann-Ibbotson-Ross 1992; see `data_models_and_methodology.md` §0.4) is realised entirely through pricing — specifically through the requirement to include `dlret`-adjusted delisting returns for PERMNOs that drop off CRSP. Vendors that don't preserve delisted PERMNOs produce optimistically biased back-tests by ~1.6%/yr on US equity for the 1926-2024 window.
- **Liquidity screens.** ADTV (average daily traded value) = `mean(PRC × VOL)` over a rolling window. ATS gating, market-impact models, and position-sizing all flow from this number.
- **Event studies.** Earnings-day abnormal returns, M&A announcement returns, dividend ex-day drift — all require minute-level or daily total returns aligned to event timestamps.
- **Order-routing back-tests for ats-crypto / cross-asset.** Intraday OHLCV at second granularity drives the maker-taker latency simulations that ats-core's HFT side uses.

The market for this data is mature, the schemas have been stable for decades, and the moat is in **adjustment-factor curation, delisting handling, and intraday quote depth** — not in the bar table itself.

---

## 2. Vendor stack matrix

Comparative view across the ten reference vendors. "T+lag" is the typical end-of-day delivery delay vs the trading day.

| Vendor / product | Granularity | History start | T+lag | Adjustment quality | License model | Indicative cost / yr |
|---|---|---|---|---|---|---|
| **CRSP DSF/MSF** | Daily + monthly | 1925-12 (NYSE), 1962-07 (full) | T+1 (academic mirror lags 1-3d) | Cleanest delisting; split + dividend factors separate | Academic via WRDS; institutional via S&P | ~$10–25k academic; six-figure institutional `[unverified]` |
| **Compustat CCM `co_secd`** | Daily | 1962 + tighter from 1985 | T+1 | `AJEXDI` + `TRFD` separated; needs CCM link to PERMNO | Xpressfeed contract | Bundled with Compustat fundamentals |
| **FactSet Prices API / OA Hub** | Daily + intraday | ~1980 (US); ~1990 (intl) | T+1 (D), real-time (intraday) | Multi-listing-aware; FSYM-keyed | Subscriber | $50k+ as add-on `[unverified]` |
| **Bloomberg DL Pricing / BPipe** | Daily + intraday + tick | ~1980 (deep names), 1996+ tick | Real-time; T+0 EOD | Bloomberg composite vs primary exchange (key choice); CIE close convention | Subscriber + entitlement | Terminal $32k/seat; DL custom |
| **LSEG/Refinitiv Datastream + RTH** | Daily (DS); tick (RTH) | DS: 1962 US, 1970s+ intl; RTH: 1996-01 | T+1; near-real-time RTH | RI (total return) separate from P (price); adjustment factor PAF | Subscriber | Workspace $20–50k/seat; RTH per-TB |
| **NYSE Daily TAQ (via WRDS)** | Trade + Quote + NBBO | 1993-09 to today | T+1 | Raw; consumer rolls own bars | Bulk file + SFTP; AWS | WRDS-included academic; ~$30–80k commercial `[unverified]` |
| **Polygon.io** | Tick + intraday + daily | 2003-09-10 | Real-time (paid); 15-min delayed (Starter) | Split-only via `adjusted=true` flag | SaaS API | $29–$499/mo |
| **Tiingo** | Daily + intraday (IEX) | 1962 for biggest US names | T+1 | Split + dividend baked into `adjClose` | SaaS API | $10–$50/mo |
| **Alpaca Markets** | Daily + intraday | 2016 (intraday); 2000+ (daily) | Real-time | Adj close included | SaaS / brokerage bundled | Free for paper |
| **IEX Cloud** | Daily + intraday (IEX feed only) | 2014-04 (IEX inception); 5y historical via aggregation | T+1 / streaming | Adjusted prices via `chart` endpoint | SaaS API (retired 2024-08; legacy data) | $9–$199/mo (legacy) |
| **Yahoo Finance (yfinance)** | Daily + intraday | 1962 for major US names | T+0 EOD | Single `Adj Close` field; known spinoff caveats | Unofficial / no SLA | Free |
| **FINRA short interest** | Bi-monthly (sec-level) | 2008 onward (settle-date file) | T+8-9 from settlement | Raw; no float adjustment | Public | $0 |
| **S3 Partners Black App** | Daily / intraday | ~2010 onward | Real-time | Float-adjusted; in-house | Subscriber | $25k+/yr `[unverified]` |
| **OPRA SIP options** | Tick | 1996+ depending on republisher | Real-time | n/a (options-specific) | Per-distributor | Six-figure +; or $99–999/mo retail |

Each row is detailed below; pricing where unconfirmed is flagged `[unverified]`.

---

## 3. Per-vendor deep dives

### 3.1 CRSP US Stock Database — DSF, MSF, DSI

The Center for Research in Security Prices, University of Chicago Booth, is the canonical academic price file. Distributed via WRDS as `crsp.dsf` (daily) and `crsp.msf` (monthly), with parallel indexes `crsp.dsi`/`crsp.msi` and the events file `crsp.dseall` (covered in `corporate_actions.md` — forthcoming, wave-3).

**Coverage.** NYSE (back to 1925-12), AMEX (1962-07), NASDAQ (1972-12), NYSE Arca (2006). Both active and **delisted** issues are preserved with full price-and-return history; the `dlstcd` (delisting code) and `dlret` (delisting return) fields in `crsp.dsedelist` are the gold-standard survivorship-bias correction (source: <https://ionmihai.github.io/finsets/01_wrds/crspd.html>).

**Identifier model.** `PERMNO` is an 8-digit security-level permanent identifier, stable through ticker changes, mergers, name changes, exchange transfers. `PERMCO` is the company-level permanent identifier; one company can issue multiple securities (common, preferred, multiple share classes), each with its own PERMNO. The names file `crsp.dsenames` carries `(PERMNO, NAMEDT, NAMEENDT, COMNAM, TICKER, EXCHCD, SHRCD, SICCD, HSICCD, NCUSIP)` — a time-bounded history of every (ticker, exchange, name) tuple per PERMNO.

**Daily file `crsp.dsf` field-level schema:**

```
PERMNO        INT     security-level permanent identifier
DATE          DATE    trading day
BIDLO         FLOAT   bid (low) — when no actual trade, signed negative
ASKHI         FLOAT   ask (high) — when no actual trade, signed negative
PRC           FLOAT   closing price; negative sign = bid-ask midpoint (no trade)
VOL           INT     trading volume (shares)
RET           FLOAT   daily holding-period total return; includes ord/special div + splits
BID           FLOAT   closing bid
ASK           FLOAT   closing ask
SHROUT        INT     shares outstanding, in thousands
CFACPR        FLOAT   cumulative adjustment factor — price
CFACSHR       FLOAT   cumulative adjustment factor — shares
OPENPRC       FLOAT   open price (added 1992)
NUMTRD        INT     number of trades (NASDAQ-only historically)
RETX          FLOAT   ex-dividend daily return
HEXCD         INT     header exchange code (current)
HSICCD        INT     header SIC code
```

(Variable list cross-referenced from <https://ionmihai.github.io/finsets/01_wrds/crspd.html> and the CRSP US Stock Database guide <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Data_Descriptions_Guide.pdf>.)

**The PRC sign convention.** When the security did not trade on the date but a bid-ask was available, CRSP stores `PRC = -(bid+ask)/2`. Naive `abs()` over PRC is the standard correction. The semantics:

```
PRC > 0  → traded close
PRC < 0  → no trade; |PRC| = midquote
PRC IS NULL or 0 → security halted with no quote
```

**CFACPR / CFACSHR — the cumulative adjustment factors.** These are *backward-cumulative* factors anchored to a reference date (currently the most recent month-end). To get a split-adjusted price for date `t`: `adj_price_t = PRC_t / CFACPR_t`. To get a split-adjusted share count: `adj_shares_t = SHROUT_t × CFACSHR_t`. In most cases `CFACPR == CFACSHR`, but they diverge for **stock dividends paid in different-class shares** and **reverse splits with cash-in-lieu** (source: <https://wrds-www.wharton.upenn.edu/pages/about/recent-announcements/>; specifically the worked example in <http://kaichen.work/?p=248>).

**RET — total return methodology.** CRSP's `RET` is the daily holding-period return:

```
RET_t = (PRC_t × (1/CFACPR_t) + DIV_t × (1/CFACPR_t)) / (PRC_{t-1} × (1/CFACPR_{t-1})) - 1
```

i.e., the return earned by a holder of one share over `(t-1, t]`, with dividends reinvested at the ex-date. CRSP applies a fix when `PRC < 0` (bid-ask midpoint substitute), and a separate handling for the delisting day:

- If the last available price is a midquote, `RET` on the delisting day uses `dlret` from `crsp.dsedelist` (the post-delisting recovery value, often zero for bankruptcy, or an OTC bulletin board price).
- Shumway-Warther (1999) and Johnson-Zhao (2007) propose specific imputations for cases where `dlret IS NULL`; these are implemented in the `delist_adj_ret()` helper in academic packages (source: <https://ionmihai.github.io/finsets/01_wrds/crspd.html>).

**Monthly file `crsp.msf`.** Identical schema with `DATE = month-end trading day`, plus `RET` defined as month-over-month total return. All quants who don't need daily resolution use MSF because it's ~30× smaller and the join-cost on Compustat (which is at most quarterly) is identical.

**Daily index file `crsp.dsi`.** Returns and levels for S&P 500, NYSE composite, NASDAQ composite, AMEX, and CRSP equal- and value-weighted market indexes. Fields:

```
DATE         DATE
SPRTRN       FLOAT    S&P 500 daily total return
SPINDX       FLOAT    S&P 500 closing level
VWRETD       FLOAT    CRSP value-weighted return including distributions
VWRETX       FLOAT    CRSP value-weighted return excluding distributions
EWRETD       FLOAT    CRSP equal-weighted return including distributions
EWRETX       FLOAT    CRSP equal-weighted return excluding distributions
TOTVAL       FLOAT    total market value of all CRSP universe stocks
TOTCNT       INT      count of stocks in CRSP universe
USDCNT       INT      count of US-domiciled stocks
USDVAL       FLOAT    total US market value
```

`vwretd` is the single most-cited risk-factor return series in empirical finance (source: <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Data_Descriptions_Guide.pdf>).

**Events file `crsp.dseall`** carries dividends, splits, name changes, delistings, exchange transfers. Cross-referenced as the canonical corporate-actions table in the forthcoming `corporate_actions.md`. For the purposes of pricing, `crsp.dseall` is the source of the per-event split ratio and per-event dividend amount that CFACPR and CFACSHR are computed from.

**Cadence.** WRDS refreshes CRSP monthly (~25th business day of the following month). The CRSP-direct fastpath is roughly T+1 for institutional subscribers (source: <https://www.crsp.org/products/research-products/>).

**Pricing.** Academic-site license through WRDS is the dominant channel; combined CRSP + Compustat for a mid-sized research university is ~$70k/yr (source: `sp_global.md` §8, citing <https://www.econjobrumors.com/topic/crsp-and-compustat-subscription>). Institutional pricing is opaque; the typical buy-side quote runs into six figures `[unverified]`.

### 3.2 Compustat CRSP Merged (CCM)

CCM is the join package — Compustat fundamentals (GVKEY-keyed) cross-linked to CRSP daily/monthly (PERMNO-keyed). Three tables matter for pricing:

**`crsp_a_ccm.ccmxpf_linktable` (also `ccmxpf_lnkhist`).** The link table. Fields (source: <https://www.otago.ac.nz/library/pdf/CRSPCompustatguide09.pdf>):

```
GVKEY         CHAR(6)   Compustat company identifier
LPERMNO       INT       CRSP PERMNO during link period
LPERMCO       INT       CRSP PERMCO during link period
LINKDT        DATE      first effective date of the link
LINKENDDT     DATE      last effective date; NULL = active
LINKTYPE      CHAR(2)   LC, LU, LX, LD, LS, LN, NU, NR (see below)
LINKPRIM      CHAR(1)   P (primary), C (primary-but-conflicted), J (joint), N (non-primary)
LIID          CHAR(5)   Compustat issue ID
USEDFLAG      CHAR(1)   legacy flag — deprecated as of WRDS Feb 2014
```

Link-type codes:

```
LC    Link confirmed by research (highest quality)
LU    Link unresearched but unambiguous
LS    Link confirmed; "secondary" share class
LX    Soft link — security exists on both sides but doesn't match cleanly
LD    Soft link — duplicate of LX
LN    No link — Compustat issue cannot be matched
NR    No link required — issue doesn't have a CRSP equivalent
NU    No link — match attempted but failed
```

Academic best practice is to filter on `LINKTYPE IN ('LC','LU','LS')` AND `LINKPRIM IN ('P','C')` to avoid double-counting securities (source: <https://www.kaichen.work/?p=138>).

**`comp.co_secd` (Security Daily).** Daily security-level price and volume for the Compustat universe. Fields:

```
GVKEY         CHAR(6)
IID           CHAR(5)        issue ID (which share class)
DATADATE      DATE
CURCDD        CHAR(3)        currency code for the daily price
PRCCD         FLOAT          close price (in CURCDD)
PRCHD         FLOAT          high price
PRCLD         FLOAT          low price
PRCOD         FLOAT          open price
CSHTRD        BIGINT         trading volume (shares)
CSHOC         BIGINT         common shares outstanding (filing-driven)
DVI           FLOAT          cash dividend per share (ex-date)
EPSFXD        FLOAT          diluted EPS (LTM, daily-updated for some industries)
EXCHG         INT            exchange code
SECSTAT       CHAR(1)        A (active), I (inactive)
TPCI          CHAR(2)        share type — 0 (common), 1 (warrant), etc.
```

(Sources: <http://finabase.blogspot.com/2017/10/return-data-and-market-value-in.html>; <https://community.portfolio123.com/uploads/short-url/tHKKrq3JHjUaqym1zJrNvj0egTK.pdf>.)

**`comp.sec_dprc` (Daily Price Adjustments).** Holds the per-day adjustment factor `AJEXDI` (Adjustment Factor — Daily, ex-distributions) used for split adjustment in Compustat.

**`comp.sec_dtrt` (Daily Total-Return).** Holds `TRFD` (Total Return Factor — Daily). The Compustat total-return formula:

```
total_return_index_t = (PRCCD_t / AJEXDI_t) × TRFD_t
```

This is the Compustat equivalent of CRSP's `RET` — accumulated as a level, not a per-day return. Conversion to per-day return: `RET_t = (TRI_t / TRI_{t-1}) - 1`.

**Market cap in Compustat.** `MKVALT` exists only annually in `co_afnd*`. Daily market cap requires `PRCCD × CSHOC` from `co_secd`. Note that `CSHOC` updates only when the company files a new share count (10-Q, 8-K, or proxy) — so for daily market-cap series, **prefer CRSP's `SHROUT`** (which is updated daily for active names).

### 3.3 FactSet Prices

FactSet's pricing product is split across three channels: the historical bulk feed `FF_SEC_PRICES`, the developer-facing `/factset-prices-api` REST endpoint, and the OA Hub / Snowflake share for cloud delivery.

**`FF_SEC_PRICES` (historical bulk).** Long-format daily fact table keyed by FSYM-R (regional security ID). Documented fields `[partially-verified — exact column names from FactSet docs require subscriber login]`:

```
fsym_id              CHAR(8)     FSYM regional ID (FSYM-R)
p_date               DATE        trade date
p_price              FLOAT       close price (local currency)
p_price_open         FLOAT       open
p_price_high         FLOAT       high
p_price_low          FLOAT       low
p_volume             BIGINT      volume
p_currency           CHAR(3)
p_split              FLOAT       split factor (1.0 default)
p_div                FLOAT       cash dividend (ex-date)
p_total_return       FLOAT       daily total return (split + div adjusted)
p_price_adj          FLOAT       adjusted close (split + div)
```

(Source: <https://developer.factset.com/api-catalog/factset-prices-api> for the API surface; bulk schema documented in the FactSet Standard DataFeed Prices manual `[unverified — gated]`. Exabel's third-party reference at <https://doc.exabel.com/dsl/data_signals/factset_prices_shares.html> confirms the FSYM-R keying and the price + shares + split + dividend separation.)

**`/factset-prices-api` REST endpoints.** As of the 2026-Q1 catalog:

```
GET /factset-prices/v1/prices             — daily/weekly/monthly OHLCV by ID
GET /factset-prices/v1/returns            — daily/cumulative total/price return
GET /factset-prices/v1/dividends          — declared/ex/record/pay-date dividends
GET /factset-prices/v1/splits             — split events
GET /factset-prices/v1/shares             — shares-outstanding history
GET /factset-prices/v1/high-low           — period high/low summary
GET /factset-prices/v1/security-prices    — intraday delayed
```

(Source: <https://www.factset.com/marketplace/catalog/product/factset-prices-and-returns-api>; <https://developer.factset.com/api-catalog/factset-prices-api>.)

**Adjustment-factor handling.** FactSet ships both `price` (raw) and `price_adj` (adjusted-for-split-and-dividends) as separate columns; the `splitAdjust` and `divAdjust` query parameters on the API let the caller pick split-only, total, or none. This is materially cleaner than Yahoo / Tiingo, which only ship the conflated `adjClose`.

**History depth.** Reportedly 1980 for major US names, late-1990s for global, real-time + 25 years of intraday tick `[unverified]`.

**Pricing.** Bundled into the broader FactSet Workstation or as an enterprise DataFeed add-on. No public rate card; institutional estimates $50k+ as a Prices-only feed on top of fundamentals `[unverified]` (source: `vendors/factset.md` §7).

### 3.4 Bloomberg Price History

Bloomberg's pricing layer is accessed via three surfaces: Terminal (BDP/BDH formulas), BPipe (institutional real-time feed), and Data License (DL) bulk delivery. The DL+ Snowflake Native App is the modern delivery channel (source: `vendors/refinitiv_bloomberg.md` §6.4).

**Core daily/intraday fields.** Bloomberg's pricing mnemonics:

```
PX_LAST              FLOAT   last trade / close price
PX_OPEN              FLOAT   open price
PX_HIGH              FLOAT   period high
PX_LOW               FLOAT   period low
PX_VOLUME            BIGINT  share volume
PX_TURNOVER          FLOAT   notional turnover (price × volume)
PX_BID               FLOAT   closing bid
PX_ASK               FLOAT   closing ask
PX_MID               FLOAT   bid-ask midpoint
LAST_TRADE           FLOAT   most recent intraday trade
TOT_RETURN_INDEX_GROSS_DVDS  FLOAT  total-return level (gross dividends reinvested)
EQY_SH_OUT           BIGINT  shares outstanding (basic)
EQY_SH_OUT_REAL      BIGINT  shares outstanding excluding treasury
CUR_MKT_CAP          FLOAT   current market cap
HISTORICAL_MARKET_CAP FLOAT  market cap on a given date
DAY_TO_DAY_TOT_RETURN_GROSS_DVDS FLOAT  daily total return
SPLIT_RATIO          FLOAT   split factor for the event date
DVD_HIST             TABLE   historical dividend series
```

(Source: <https://bautheac.github.io/BBGsymbols/>; <https://github.com/dappled/AFData/blob/master/albertfriedMarketData/src/bbgRequestor/bloomberg/BbgNames.java>.)

**BPipe historical pricing service.** BPipe delivers normalised real-time and intraday-historical pricing via Bloomberg's enterprise transport. Subscribers entitle on a per-asset-class × per-region × per-consumer basis; the feed shape is the same `PX_*` mnemonic set. Historical pulls beyond ~5 years are routed to the DL request system rather than BPipe streaming.

**Composite vs primary exchange.** Bloomberg tickers carry an exchange suffix yellow-key combination:

```
IBM US Equity        composite — Bloomberg's "best execution" close
IBM UN Equity        primary — NYSE only
IBM UQ Equity        primary — NASDAQ only
VOD LN Equity        composite — UK
VOD LI Equity        primary — London Stock Exchange
```

The composite ticker resolves to a country-level **composite FIGI** (see `refinitiv_bloomberg.md` §7.2) and aggregates trades across all venues in that country. The primary ticker resolves to the venue-level FIGI. The two tickers produce *different* daily close prices because:

- The primary ticker shows the venue's official 4:00 PM (or local) close.
- The composite ticker often shows a 4:00:00.001 PM trade reported via TRF or ATS after the official close, plus Bloomberg's own "official close" determination logic which prioritises the primary listing venue but doesn't always match it.

**The BBG vs ICE/Refinitiv close-price discrepancy.** Three vendors can disagree on a single day's close for a single US large-cap by 1-5 basis points, sometimes 10-50 bps on illiquid names. The root cause:

- **NYSE/NASDAQ official close** is the price set by the closing auction (LMP for NYSE, NASDAQ Closing Cross for NASDAQ). This is the price index providers and ETF NAV calculators use.
- **Refinitiv "consolidated close"** is the last consolidated tape print regardless of venue — usually a late TRF print.
- **Bloomberg "composite close"** is Bloomberg's proprietary determination; defaults to the official close on the primary venue but switches to the latest consolidated print if there's a "late-tape" rule trigger.
- **ICE Data Services "Pricing & Reference"** uses ICE's own end-of-day evaluation, which for equities is the official close, but for ADRs and dual-listed names may use the home-market close converted at the 4:00 PM FX fix.

Cross-vendor reconciliation of "the" close price is a known multi-week project for any cross-vendor data lake. The ats-eqt schema should store **both raw and official-auction close** when available, and tag the source explicitly (see §7).

**Pricing.** Terminal $31,980/seat/yr (2025); BPipe and DL custom (source: `refinitiv_bloomberg.md` §8).

### 3.5 Refinitiv / LSEG Datastream + Tick History (RTH)

**Datastream — daily.** Datastream's mnemonic-keyed time-series engine is the longest commercially-distributed daily price file. Core mnemonics:

```
P     price (close, local currency)              — adjustment-factor backward-cumulative
RI    return index — total return, base 100      — Datastream's adj close equivalent
PI    price index — split-adjusted, no div       — analog of CRSP PRC/CFACPR
MV    market value (capitalisation, local ccy)   — daily mkt cap
VO    volume (shares)
PO    opening price
PH    high price
PL    low price
NOSH  number of shares (in millions)
PE    P/E ratio
DY    dividend yield
EPS   earnings per share, trailing
UP    unadjusted price
UVP   unadjusted volume
```

(Sources: <https://bigiavi.sba.unibo.it/cataloghi-e-risorse-online/eikon-datastream/datastream_guida.pdf>; <https://finm-32900.github.io/lectures/Week7/LSEG_datastream.html>.)

**The Datastream Price Adjustment Factor (PAF).** Datastream applies splits and stock dividends to `P` *retroactively* — `P` series in Datastream are already split-adjusted. The unadjusted price is `UP`. The implied adjustment factor: `PAF_t = UP_t / P_t`. This is the opposite convention from CRSP (which stores raw `PRC` and a separate `CFACPR`). Naive cross-vendor joins on "price" misalign between CRSP and Datastream every time there's a split.

**RI vs PI.** `RI` includes dividend reinvestment; `PI` does not. Both are normalised to 100 at the base date. To convert to per-day total return: `RET_t = RI_t / RI_{t-1} - 1`.

**Tick History (RTH).** Refinitiv Tick History — formerly the SIRCA / Securities Industry Research Centre of Asia-Pacific archive — covers global intraday tick-by-tick trade and quote data from 1996-01 to today (source: <https://www.refinitiv.com/en/financial-data/market-data/tick-history>). Delivery via DataScope Select bulk, AWS S3-direct, or BigQuery (source: <https://developers.lseg.com/en/article-catalog/article/boost-tick-history-downloads-with-aws>; <https://developers.lseg.com/en/article-catalog/article/big-data-tick-history-google-bigquery>).

RTH file schema, abbreviated:

```
RIC                  CHAR(20)   Refinitiv Instrument Code (venue-specific)
DateTime             TIMESTAMP  microsecond precision
GMT Offset           INT
Type                 CHAR(5)    Trade | Quote | Quote Bid Ask | Auction
Price                FLOAT
Volume               BIGINT
Bid Price / Ask Price  FLOAT
Bid Size / Ask Size    BIGINT
Exchange Time        TIMESTAMP  venue-side timestamp
Qualifiers           VARCHAR    condition codes (e.g. "TRF[USA]", "Open[USA]")
Seq No               BIGINT
Activity Type Description  VARCHAR
```

**RIC composite tickers.** RICs use venue-specific suffixes (e.g. `IBM.N` for NYSE, `MSFT.O` for NASDAQ, `VOD.L` for LSE, `7203.T` for Tokyo). There's also a country-composite RIC pattern with `.composite` suffix or via the `<ticker>` chain, which aggregates across venues — the Refinitiv equivalent of Bloomberg's `IBM US`.

**DataStream Macro and RWS web API.** Datastream's macro side carries vintages for revised economic series (BEA, OECD, ECB). The DSWS (Datastream Web Service) REST API provides programmatic access for both pricing and macro: `GET /DSWSClient/V1/DSService.svc/rest/GetData?token=…&instruments=…&datatypes=…&date=…` (source: <https://fmc.refinitiv.com/clientFacing/pdf/DFO_User_Guide.pdf>).

**RKD vs DSWS.** Reuters Knowledge Direct (RKD) is the legacy enterprise feed; DSWS is the modern web-services replacement. RKD is still in production for some institutional contracts but is gated `[unverified — exact sunset date]`.

**Pricing.** Workspace + Datastream pack ~$20–50k/seat; RTH per-TB pricing on AWS Marketplace, typical hedge-fund deployment $50–500k/yr `[unverified]` (source: `refinitiv_bloomberg.md` §8).

### 3.6 NYSE TAQ (Trade and Quote)

The Daily TAQ product is the closest the US equity market has to a single canonical intraday data source. TAQ contains every trade and every quote reported to the consolidated tape — meaning every CTA participant (NYSE, NYSE American, Arca, Chicago, BX, EDGA, EDGX, IEX, BATS Y, BATS Z, MIAX Pearl, MEMX, LTSE, NSX, NYSE National) and every UTP participant (NASDAQ-listed equivalents) (source: <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/nyse-trade-and-quote-taq/>).

**File types in the Daily TAQ distribution (v4.2, 2025):**

```
Master file        per-symbol reference (ticker → CUSIP, exchange, security type)
Trades file        every trade print
Quotes (BBO) file  every quote update from every venue
NBBO file          consolidated National Best Bid and Offer per trade
LULD (limit up/limit down) bands file
Admin Messages file (halts, resumptions, indicator changes)
```

(Source: <https://www.nyse.com/publicdocs/nyse/data/Daily_TAQ_Client_Spec_v4.2.pdf>.)

**WRDS-distributed Daily TAQ tables:**

```
taqm_msec.cqm_YYYYMMDD     — millisecond consolidated quotes for one day
taqm_msec.ctm_YYYYMMDD     — millisecond consolidated trades for one day
taqm_msec.nbbom_YYYYMMDD   — millisecond NBBO file
taqm_msec.mastm_YYYYMMDD   — master file
```

Earlier-era WRDS Monthly TAQ uses the prefix `taq.cq_YYYYMM` / `taq.ct_YYYYMM`, with second-precision timestamps for 1993-09 to 2003-08 and millisecond for 2003-09 onward; the Daily TAQ schema (microsecond from 2014, nanosecond from 2015-06) supersedes Monthly TAQ from 2014 (source: <https://www.nyse.com/market-data/historical/daily-taq>).

**Trade record fields (Daily TAQ v4.2 Trades file, abbreviated):**

```
Time                  TIMESTAMP(9)  participant timestamp (nanosecond from 2015-06)
SIP_Time              TIMESTAMP(9)  SIP receipt time
Symbol                CHAR(16)
Exchange              CHAR(1)       CTA participant code (N=NYSE, Q=NASDAQ, etc.)
Trade_Volume          BIGINT
Trade_Price           DECIMAL(20,4)
Trade_Stop_Stock_Indicator  CHAR(1)
Trade_Correction_Indicator  CHAR(2)
Sequence_Number       BIGINT
Trade_Id              BIGINT
Source_of_Trade       CHAR(1)
Trade_Reporting_Facility  CHAR(1)
Participant_Timestamp TIMESTAMP(9)
Trade_Through_Exempt_Indicator  CHAR(1)
Sale_Condition        CHAR(4)       e.g. " @ " (regular), "F" (intermarket sweep)
```

**Quote record fields (Daily TAQ v4.2 Quotes/BBO file):**

```
Time                  TIMESTAMP(9)
Symbol                CHAR(16)
Exchange              CHAR(1)
Bid_Price             DECIMAL(20,4)
Bid_Size              BIGINT
Offer_Price           DECIMAL(20,4)
Offer_Size            BIGINT
Quote_Condition       CHAR(1)
Sequence_Number       BIGINT
National_BBO_Indicator CHAR(1)
FINRA_BBO_Indicator   CHAR(1)
Quote_Cancel_Correction CHAR(1)
Source_Of_Quote       CHAR(1)
NBBO_LULB_Indicator   CHAR(1)       (limit up / limit down)
```

(Field lists abbreviated from the Daily TAQ Client Specification v4.2 — full spec at <https://www.nyse.com/publicdocs/nyse/data/Daily_TAQ_Client_Spec_v4.2.pdf>.)

**NBBO file.** A reconstructed best-bid-best-offer at every quote update, computed by NYSE from all venue Quote messages. Used by the SEC for Reg NMS compliance evaluation and by quants for execution-quality back-tests.

**Volume and storage.** A typical trading day produces:

- Trades file: ~50-100 GB uncompressed; ~10-20 GB gzipped
- Quotes file: ~150-300 GB uncompressed; ~30-60 GB gzipped
- NBBO file: ~20-40 GB uncompressed

(Approximate figures from WRDS storage documentation `[unverified — exact day-over-day varies with volatility]`.)

**Roll-up methodology — OHLCV from raw TAQ.** The canonical approach:

1. Filter trades to `Sale_Condition NOT IN ('Q', 'I', '4', '5', '6', '7', '8', '9')` — i.e., drop opening/closing-auction prints if you want continuous-session bars; or include them if you want regulatory-style OHLCV.
2. For each (symbol, bar_seconds) bucket:
   - `open = first Trade_Price`
   - `high = max(Trade_Price)`
   - `low = min(Trade_Price)`
   - `close = last Trade_Price`
   - `volume = sum(Trade_Volume)`
   - `vwap = sum(Trade_Price × Trade_Volume) / sum(Trade_Volume)`
   - `num_trades = count(*)`
3. For NBBO-anchored quote bars: take the last NBBO update before each bar boundary, plus the median bid/ask over the bar.

Academic references for this roll-up: Holden & Jacobsen 2014 (`Liquidity measurement problems in fast, competitive markets`) and the WRDS TAQ research methodology guide.

### 3.7 Polygon.io / Tiingo / Alpaca / IEX — modern API pricing

**Polygon.io.** The most credible mid-market commercial pricing API. Aggregates endpoint:

```
GET /v2/aggs/ticker/{stocksTicker}/range/{multiplier}/{timespan}/{from}/{to}

Path:
  stocksTicker  case-sensitive ticker (e.g., AAPL)
  multiplier    integer
  timespan      second | minute | hour | day | week | month | quarter | year
  from / to     YYYY-MM-DD or ms timestamp

Query:
  adjusted      bool (default true)  — applies split adjustment
  sort          asc | desc
  limit         int (max 50000, default 5000)

Response shape (.results[]):
  o             float   open
  h             float   high
  l             float   low
  c             float   close
  v             int     volume
  vw            float   volume-weighted average price
  t             int     unix-ms timestamp (period start)
  n             int     transaction count
  otc           bool    OTC indicator (omitted when false)
```

(Source: <https://polygon.io/docs/stocks/get_v2_aggs_ticker__stocksticker__range__multiplier___timespan___from___to>; via massive.com redirect.)

**Polygon history limits by plan:**

```
Basic        Free       5 calls/min     2 years history    EOD only
Starter      $29/mo     unlimited       5 years history    15-min delayed
Developer    $79/mo     unlimited       10 years           15-min delayed
Advanced     $199/mo    unlimited       all history        real-time SIP
Business     $499/mo    unlimited       all history        real-time + WebSocket
```

(Source: <https://polygon.io/pricing>; pricing snapshot as of 2026-04 `[unverified]`.)

Polygon history runs from 2003-09-10 onward (the SIP archive start). Polygon also offers a `/v3/reference/dividends`, `/v3/reference/splits`, and `/v3/reference/tickers` for corporate actions and ticker metadata.

**Tiingo.** Daily EOD focus, with intraday IEX data on the paid tier. Daily endpoint response:

```
GET /tiingo/daily/{ticker}/prices?startDate=…&endDate=…

Per row:
  date           ISO 8601 date
  open           float (unadjusted)
  high           float
  low            float
  close          float
  volume         int
  adjOpen        float  (split + dividend adjusted)
  adjHigh        float
  adjLow         float
  adjClose       float
  adjVolume      int
  divCash        float  (cash dividend on this ex-date)
  splitFactor    float  (split factor on this date, 1.0 default)
```

(Source: <https://www.tiingo.com/documentation/end-of-day>.)

The presence of both `divCash` and `splitFactor` per-row is a critical differentiator vs Yahoo — it means a consumer can reconstruct their own `adj_close` with any custom adjustment policy.

**Tiingo pricing.** Free for personal/non-commercial use with 50 calls/hour and 1k symbols/day. Power $10/mo unlocks intraday IEX. Commercial $50/mo lifts the rate limit and licenses redistribution `[unverified — pricing 2026]` (source: <https://www.tiingo.com/about/pricing>).

**Tiingo history.** Claims 30+ years for the biggest names, ~3 years for the long tail; the back-fill source is undisclosed but appears to be a reconciled mix of Nasdaq Basic + IEX + sourced corporate actions `[unverified]`.

**Alpaca Markets.** Brokerage-bundled API. Historical bars endpoint:

```
GET /v2/stocks/bars

Query:
  symbols     comma-separated
  timeframe   1Min | 5Min | 15Min | 30Min | 1Hour | 1Day | 1Week | 1Month
  start, end  RFC-3339
  adjustment  raw | split | dividend | all
  feed        sip | iex | otc

Response per bar:
  t  RFC-3339 timestamp (start of bar)
  o, h, l, c, v
  n  number of trades
  vw VWAP
```

(Source: <https://docs.alpaca.markets/us/reference/stockbars>.)

**SIP vs IEX feeds.** Alpaca's free tier (paper trading) is **IEX-only**, which represents ~3% of US consolidated equity volume. Production trading on the paid tier ($99/mo Algo Trader Plus) entitles the SIP feed (full consolidated tape). For back-testing universes, IEX-only data is missing ~97% of trades — fine for live signal generation, not fine for survivorship-correct back-tests.

**IEX Cloud (retired 2024-08).** IEX Cloud's `/stable/stock/{symbol}/chart` endpoint shipped:

```
date, minute, label,
open, close, high, low, volume,
notional, numberOfTrades,
marketOpen, marketClose, marketHigh, marketLow, marketVolume,
marketNotional, marketNumberOfTrades,
marketChangeOverTime, changeOverTime,
average, marketAverage,
uOpen, uHigh, uLow, uClose, uVolume     (u-prefix = unadjusted)
```

(Source: <https://iexcloud.io/docs/api/>; service retired 2024-08-31 — legacy data only.)

The `u*` columns are the unadjusted variants and the non-`u*` columns are pre-split-adjusted. The "market*" columns reflect the consolidated tape, the non-prefixed columns are IEX-only. The retirement of IEX Cloud in 2024-08 left a real gap in the $20–100/mo SaaS-data tier; Polygon and Tiingo absorbed most of the displaced volume.

### 3.8 Yahoo Finance (yfinance and the Yahoo Query API)

The Yahoo Finance unofficial JSON endpoints — `query1.finance.yahoo.com/v8/finance/chart/{symbol}` and `query2.finance.yahoo.com/v7/finance/download/{symbol}` — power the `yfinance` Python package and a dozen other free libraries.

**Endpoint shape:**

```
GET https://query1.finance.yahoo.com/v8/finance/chart/AAPL
    ?period1=946684800&period2=1715731200
    &interval=1d
    &events=history,div,splits
    &includeAdjustedClose=true

Response (truncated):
  chart.result[0].meta            ticker metadata
  chart.result[0].timestamp[]     unix-second timestamps
  chart.result[0].indicators.quote[0]:
      open[], high[], low[], close[], volume[]
  chart.result[0].indicators.adjclose[0].adjclose[]
  chart.result[0].events.dividends.{ts}: {amount, date}
  chart.result[0].events.splits.{ts}:    {numerator, denominator, splitRatio, date}
```

**Adjusted Close methodology.** Yahoo's `Adj Close` applies a *proportional* adjustment: for every split or dividend ex-date `e`, all prior closes are multiplied by `1 / split_ratio` and `1 - (dividend / close_{e-1})` respectively, applied cumulatively (source: <https://github.com/ranaroussi/yfinance/issues/1749>; <https://www.quantvps.com/blog/yahoo-finance-api-documentation>).

**Known caveats:**

- **Spinoffs are inconsistently handled.** When a parent spins off a subsidiary, the correct adjustment is `1 - (spinoff_share_value / parent_close_{e-1})` — but Yahoo sometimes records the spinoff as a dividend equal to the spinoff's first-day close, which over- or under-states the adjustment depending on the spin-day volatility (`[unverified — case-by-case]`).
- **Occasional 0/null bars.** Yahoo's data ingest occasionally emits a bar where one of OHLCV is zero or null on a low-volume day; downstream code must validate-and-impute.
- **Rate limits.** Unofficial; Yahoo will issue 429s or block IPs at ~2000 req/hr per IP, plus an active anti-scraping system that requires session cookies (yfinance 0.2.40+ handles this with a built-in session manager) (source: <https://github.com/ranaroussi/yfinance>).
- **The `Close` column already returns adjusted close.** As of yfinance 0.2.x, the `auto_adjust=True` default (changed in 2024) means the `Close` column is actually the adjusted close — there is no separate `Adj Close` in the default dataframe. Set `auto_adjust=False` to get back the raw close and an explicit `Adj Close` column (source: <https://github.com/ranaroussi/yfinance/issues/1749>).
- **Look-ahead bias risk.** Yahoo's `Adj Close` is always restated to the latest cumulative adjustment factor, so a historical back-test that pulls Yahoo data today gets a *different* `Adj Close` than the same query would have returned a year ago. This is the cardinal sin for PIT integrity and is the primary reason Yahoo is academically unacceptable.

**Cost.** Free, no auth, no SLA, no redistribution rights. Production use is a license violation per Yahoo's ToS (source: <https://policies.yahoo.com/us/en/yahoo/terms/utos/index.htm>).

### 3.9 Short Interest

**FINRA bi-monthly short interest (free, public).** FINRA publishes a settle-date short-interest file twice per month, on the 15th-of-month and end-of-month settlement dates (with end-of-prior-month settle-date dissemination cadence: typically 7-8 business days after settle). Schema (source: <https://www.finra.org/sites/default/files/Equity_Short_Interest_Data_File_Download_API.pdf>):

```
issueSymbolIdentifier        VARCHAR     ticker
settlementDate               DATE        settlement date for the report
issueName                    VARCHAR     security name
marketCategoryCode           CHAR(1)     N (NYSE), Q (NASDAQ), A (AMEX), etc.
currentShortShareNumber      BIGINT      short shares as of settlementDate
previousShortShareNumber     BIGINT      short shares from the previous settle
changePercent                FLOAT       % change vs previous
averageShortShareNumber      BIGINT      average daily share volume over the report period
revisionFlag                 CHAR(1)     R (revised) | NULL
```

Cadence: bi-monthly (twice per month) for OTC securities since 2007; bi-monthly for exchange-listed equities since 1995, weekly briefly in 1986-1990, monthly before that. Format: CSV/JSON/XML via the FINRA Data API at <https://www.finra.org/finra-data/browse-catalog/equity-short-interest/data>.

**FINRA daily short-volume files (free, public).** *Different product* — daily aggregated trade volume that was sold short, by exchange/TRF. Schema (source: <https://www.finra.org/sites/default/files/2020-12/short-sale-volume-user-guide.pdf>):

```
Date                  DATE
Symbol                VARCHAR
ShortVolume           BIGINT     short-sale volume during regular hours
ShortExemptVolume     BIGINT     short-sale-exempt volume
TotalVolume           BIGINT     all trade volume reported to this venue
Market                CHAR(1)    N (NYSE TRF), Q (NASDAQ TRF Carteret),
                                 B (NASDAQ TRF Chicago), D (ADF)
```

This is **not** a short-interest substitute — short-volume is the *flow* of short trades on a given day, short-interest is the *stock* of outstanding short positions as of a settlement date. The ratio `ShortVolume / TotalVolume` is a useful daily proxy but understates true short-flow because it excludes off-exchange short trades not reported to a FINRA TRF.

URL pattern: `https://api.finra.org/data/group/otcMarket/name/regShoDaily?settlementDate=YYYY-MM-DD` (source: <https://www.finra.org/finra-data/browse-catalog/short-sale-volume-data/daily-short-sale-volume-files>).

**S3 Partners Black App (commercial).** Real-time short-interest and securities-financing analytics, delivered via the Bloomberg App Portal and direct API (source: <https://www.s3partners.com/short-interest-data>; <https://www.prnewswire.com/news-releases/s3-partners-launches-real-time-short-interest-analytics-on-the-bloomberg-app-portal-300289459.html>). Coverage: 15,000+ securities globally. Methodology: aggregated bank/broker inventory feeds plus regulatory filings plus voice-broker rate confirmations. Headline metrics: short shares, short notional, days-to-cover, % of float, borrow fee, utilization, cost-to-borrow. Pricing: $25k+/yr `[unverified]`.

**IHS Markit / S&P Securities Finance (commercial).** Former IHS Markit Securities Finance, now part of S&P Global. The institutional benchmark for stock-loan flow data: tracks ~$30T in lendable inventory across 20k+ funds. Schema includes per-security daily borrow rate, utilization, on-loan quantity, lendable quantity (source: <https://www.spglobal.com/marketintelligence/en/solutions/products/securities-finance>). Pricing: $50k+/yr `[unverified]`.

**Schema sketch (vendor-consensus short-interest table):**

```
security_id                   BIGINT     internal
settle_date                   DATE       settlement date
short_interest_quantity       BIGINT     shares short
average_daily_volume          BIGINT     ADV over reporting period
days_to_cover                 FLOAT      short_interest / ADV
short_percent_of_float        FLOAT      short_interest / float
borrow_fee_bps                FLOAT      annualised borrow fee (commercial only)
utilization                   FLOAT      on-loan / lendable (commercial only)
source                        VARCHAR    FINRA | S3 | Markit | IBKR
valid_from / valid_to / knowledge_from / knowledge_to
```

See §7.5 for the recommended ats-eqt `short_interest` table.

### 3.10 Options data (short note; not the primary focus)

For completeness — options/derivatives identifiers and the canonical OPRA feed get a brief treatment here; deep coverage is deferred to a future `options_data.md` wave-3 file.

**OPRA SIP.** The Options Price Reporting Authority is the consolidated tape for all US listed options (16 exchanges including CBOE, ISE, BOX, NYSE Arca Options, NASDAQ Options). The OPRA feed runs ~15 GB/sec peak and ~10 TB/trading day uncompressed — making it the largest financial data feed in existence. Schema is OCC/OSI option symbol (e.g. `AAPL  240419C00170000`) plus trade/quote tuples with venue, price, size, condition codes (source: <https://databento.com/datasets/OPRA.PILLAR>; <https://databento.com/microstructure/opra>).

**Commercial options data vendors (key players):**

- **ORATS** — historical EOD options chains, implied vols, Greeks; $99–$799/mo (source: <https://orats.com/>).
- **IVolatility** — historical options with IV surface, $250–$5000/mo.
- **Cboe LiveVol** — Cboe's institutional options analytics platform; six-figure subscriber.
- **Tackle Trading / Tackle 25** — retail-focused options-flow data.
- **Databento OPRA** — per-symbol-day OPRA tick replay; pay-per-use.
- **Polygon.io options** — included in Polygon's Stocks Advanced + Options Bundle ($199–$499/mo).
- **FactSet OPRA Real-Time Feed** — institutional add-on (source: <https://www.factset.com/marketplace/catalog/product/options-price-reporting-authority-opra-real-time-feed>).

The ats-eqt schema should anticipate but not yet model the options layer — a parallel `option_chain`, `option_quote`, `option_trade` set of tables would mirror §7 with a strike+expiry+right+underlying primary key. Out of scope for wave-2.

---

## 4. The price-adjustment math

This is the single most-confused topic in equity data engineering. Three different "adjusted" semantics, four different vendor conventions, one set of correct formulas.

### 4.1 Split adjustment

For a split ratio `s` on ex-date `e` (e.g. a 2-for-1 split: `s = 2`; a 1-for-10 reverse split: `s = 0.1`):

```
adj_price_t     = raw_price_t × ( product over events e > t of (1 / s_e) )
adj_shares_t    = raw_shares_t × ( product over events e > t of s_e )
adj_volume_t    = raw_volume_t × ( product over events e > t of s_e )
```

So a stock that was $100 on day t and underwent a 2-for-1 split on day t+5 has `adj_price_t = $50` after the split takes effect — but the same stock would still report `raw_price_t = $100`.

CRSP stores the *backward-cumulative* version: `CFACPR_t` is `product over events e > t of s_e` (so always ≥ 1 for forward splits, ≤ 1 for reverse splits). Then `adj_price_t = PRC_t / CFACPR_t`. CFACPR is renormalised periodically so today's CFACPR equals 1.

### 4.2 Dividend adjustment (total return)

For an ordinary cash dividend `d_e` on ex-date `e`, the price-only adjustment is:

```
adj_price_t = raw_price_t × ( product over events e > t of (1 - d_e / close_{e-1}) )
```

This proportional method (used by Yahoo and CRSP for `PRC`-adjusted-for-distributions) preserves the price-time series shape while making it tradable as a single instrument. The alternative ("subtractive" adjustment) subtracts `d_e` from all prior closes — used by some legacy systems and produces negative prices for high-dividend stocks over long histories, which is wrong.

### 4.3 Total return

The cleanest formulation is per-day total return:

```
TR_t = (close_t + div_t) / close_{t-1} - 1
```

where `div_t = 0` on non-ex days. Cumulative total return is `∏(1 + TR_t)`. CRSP's `RET` is exactly this. Datastream's `RI` is the cumulative form normalised to 100. Compustat's `(PRCCD / AJEXDI) × TRFD` is the cumulative form normalised to PRCCD's level.

### 4.4 Worked example: AAPL 2014-06-09 7-for-1 split + Q3 2014 dividend

Suppose:

```
2014-06-06 (Friday):       PRC = 645.57   (raw close)
2014-06-09 (Monday, ex):   PRC =  93.70   (post-split close)
                            split ratio s = 7
2014-08-14 (next ex-div):  PRC =  97.50, div = 0.47
```

CRSP storage (snapshot taken today, with no further splits):

```
date         PRC      CFACPR    CFACSHR   RET
2014-06-06   645.57   7.0       7.0       (prior day return)
2014-06-09    93.70   1.0       1.0       (93.70 - 645.57/7) / (645.57/7) ≈ 1.59%
                                          [for the actual day the stock was up
                                          marginally on a split-adjusted basis]
2014-08-14    97.50   1.0       1.0       (97.50 + 0.47) / prev_close - 1
```

Conversion to a continuous "adjusted close" series:

```
adj_close_2014-06-06 = 645.57 / 7 = 92.224
adj_close_2014-06-09 = 93.70  / 1 = 93.70

Total-return level (base 100 at start):
  TRI_t = TRI_{t-1} × (1 + RET_t)
```

This is what every back-test resolves to. The fact that CRSP gives you `PRC`, `CFACPR`, and `RET` separately, rather than a single conflated `Adj Close`, is precisely why CRSP is the canonical academic source — the consumer makes the adjustment choice explicit. Yahoo's `Adj Close` and Tiingo's `adjClose` collapse this into a single column and irreversibly lose the per-event detail.

### 4.5 The conflated-`adj_close` problem

When a consumer joins on `adj_close` across vendors:

```
Vendor          AAPL 2014-06-06 "Adj Close" today
CRSP            92.224  (split-only)
CRSP RET integrated 84.xx  (total-return level)
Tiingo adjClose 84.61  (split + dividend cumulative, today's snapshot)
Yahoo Adj Close 84.61  (same as Tiingo, both proportional)
Bloomberg PX_LAST(historical)  645.57 (raw — no adjustment)
Bloomberg TOT_RETURN_INDEX_GROSS_DVDS  similar to Tiingo
Compustat (PRCCD/AJEXDI) × TRFD       depends on snapshot date
```

The values diverge by 8-12% because some include dividend reinvestment and others don't. **The ats-eqt schema stores BOTH `adj_close_split` (split-only) AND `adj_close_total` (split + dividend), keyed to the as-of snapshot date.** See §7.

---

## 5. Public-data reconstruction

Per-source mapping for an ats-eqt build that relies on public/freemium tier data only:

| Source | Granularity | Coverage | Free-tier limit | License | Best for |
|---|---|---|---|---|---|
| Polygon.io free | EOD daily | US equities | 5 calls/min, 2 years history, EOD only | Personal | Recent daily for prototype back-tests |
| Yahoo Finance (yfinance) | EOD + 1-min intraday (60d) | Global | Unofficial; ~2000 req/hr per IP | ToS violation for prod | Quick daily history; PIT integrity unsafe |
| Alpaca free | EOD + intraday (IEX) | US equities | Unlimited, IEX-only | Paper-trading | Live signal generation in paper trading |
| IEX Cloud free (legacy) | EOD + intraday (IEX) | US | 50k messages/mo | Retired 2024-08 | Archival only |
| NYSE daily summary | EOD OHLCV | NYSE-listed only | Public | Public | Single-venue close prices, official-auction close |
| NASDAQ daily summary | EOD OHLCV | NASDAQ-listed | Public | Public | Single-venue close prices |
| Cboe Daily Market Statistics | Daily summary | Cboe options + Cboe equities | Public | Public | Options daily summary, P/C ratios |
| FINRA Daily Short Volume | Daily | All FINRA-reported short trades | Public | Public | Short-trade flow proxy |
| FINRA Bi-Monthly Short Interest | Bi-monthly | All exchange-listed | Public | Public | Settle-date short interest (the stock figure) |
| SEC EDGAR (Forms 10-K, 10-Q, 8-K) | Filing-driven | All public | Public | Public | Share-count history from filings |
| Sharadar Core (Nasdaq Data Link) | Daily EOD | US | Paid sample; bulk paid | Subscriber | Pre-cleaned, PIT, ~$50/mo |
| Quandl / Nasdaq Data Link `WIKI/PRICES` (frozen 2018-04) | Daily EOD | US | Public | CC-BY-NC 4.0 | Static historical back-fill to 2018-04 |

**Recommended public-data composition:**

1. **2018-04 backfill:** Use Quandl/Nasdaq `WIKI/PRICES` (frozen) as the deep historical layer for US equities, 1962-04-01 → 2018-04-11.
2. **2018-04 → present, daily:** Polygon.io Starter ($29/mo) for adjusted daily bars across the full US equity universe with proper survivorship handling.
3. **Daily corporate actions:** Polygon `/v3/reference/splits` + `/v3/reference/dividends` for ex-date events; cross-validated against SEC 8-K filings.
4. **Shares outstanding:** SEC EDGAR XBRL filings (`dei:EntityCommonStockSharesOutstanding` tag) for filing-date share-count snapshots, interpolated forward to next filing.
5. **Short interest:** FINRA Bi-Monthly Short Interest free API.
6. **Short volume (high-frequency proxy):** FINRA Daily Short Volume.
7. **Survivorship correction:** SEC EDGAR Form 25 (Notice of Delisting) + Form 15 (Termination of Registration); join to the daily price universe to identify dropped tickers; flag with a `delisting_date` and final price.

This composition replicates ~85-90% of the CRSP daily file content (excluding the 1962-2018 backfill which Quandl WIKI doesn't fully cover for inactive PERMNOs) for under $40/month + an EDGAR poller.

The headline gaps in pure-public reconstruction:

- **Pre-1995 inactive tickers.** Quandl WIKI does not contain pre-2003 delisted tickers reliably; the only fully survivorship-free pre-2003 US daily file is CRSP.
- **Intraday quote depth.** TAQ-equivalent quote data is not publicly available; the closest is Polygon's full-tick Advanced ($199/mo) for 2003-09 forward.
- **Manager-level short positions.** Not publicly observable until Form SHO Rule 13f-2 effective 2028-02-14.

---

## 6. The Cboe / NYSE / NASDAQ public summary files

Worth a dedicated note because they're the cleanest free source for **official-auction close prices**:

- **NYSE Daily Market Summary.** <https://www.nyse.com/markets/us-equity-volumes/historical>. Free CSV download per day with `Symbol, Open, High, Low, Close, Volume, ClosingAuctionPrice, ClosingAuctionVolume`. Limited to NYSE-listed names. Volume excludes off-exchange (TRF) trades.
- **NASDAQ Basic / TotalView Daily Summary.** Free daily CSV of NASDAQ-listed names with the NASDAQ Closing Cross price. Covers ~3,500 NASDAQ-listed equities.
- **Cboe BZX/BYX/EDGX/EDGA Daily Market Summary.** <https://www.cboe.com/us/equities/market_statistics/>. Free aggregate volume statistics; full per-symbol close summaries are subscriber-gated in DataShop.

For an ats-eqt build that wants **survivorship-free official-close prices** without the WRDS/CRSP price tag, daily ingestion of the NYSE + NASDAQ + Cboe summary files is the cleanest free path — at the cost of needing to merge across 3 vendor schemas and handling cross-listing.

---

## 7. Recommended ats-eqt schema

Fits the bitemporal long-format pattern in `schemas/data_models_and_methodology.md` §F and §G. All tables are bitemporal where state evolves (`valid_from / valid_to / knowledge_from / knowledge_to`); the per-bar price tables are append-only because a daily bar, once published, doesn't change valid time — only its adjustment factor does, which lives in a separate cross-link table.

### 7.1 `bar_daily` — the core daily OHLCV fact

```sql
CREATE TABLE bar_daily (
  security_id          BIGINT       NOT NULL,     -- → security (ats-eqt internal)
  date                 DATE         NOT NULL,     -- trading day
  open                 DOUBLE       NOT NULL,
  high                 DOUBLE       NOT NULL,
  low                  DOUBLE       NOT NULL,
  close                DOUBLE       NOT NULL,     -- raw close (last trade)
  volume               BIGINT       NOT NULL,     -- consolidated tape volume
  vwap                 DOUBLE       NULL,         -- volume-weighted avg price
  num_trades           INTEGER      NULL,         -- count of trades (CTA + UTP)
  -- Multiple "close" semantics (see §3.4 for why)
  raw_close            DOUBLE       NOT NULL,     -- copy of close, explicit
  official_auction_close  DOUBLE    NULL,         -- closing-cross / LMP price
  consolidated_tape_close DOUBLE    NULL,         -- last consolidated print
  close_source         VARCHAR(8)   NOT NULL,     -- 'AUCTION'|'TAPE'|'BID_ASK'
  -- Adjusted derivatives, snapshotted as of `knowledge_from`
  adj_close_split      DOUBLE       NOT NULL,     -- split-only, anchored to today
  adj_close_total      DOUBLE       NOT NULL,     -- split + dividend, anchored to today
  cfacpr               DOUBLE       NOT NULL,     -- cumulative split factor
  cfacshr              DOUBLE       NOT NULL,     -- cumulative shares factor
  -- Currency + venue
  currency             CHAR(3)      NOT NULL,
  primary_exchange_mic CHAR(4)      NOT NULL,
  -- Provenance
  source_id            INTEGER      NOT NULL,     -- CRSP|COMP|POLY|TIINGO|BBG|LSEG|YAHOO|...
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (security_id, date, source_id, knowledge_from)
);

CREATE INDEX ix_bar_daily_date ON bar_daily(date, security_id);
CREATE INDEX ix_bar_daily_sec_range ON bar_daily(security_id, date) WHERE knowledge_to = '9999-12-31';
```

Notes:

- The three close columns (`raw_close`, `official_auction_close`, `consolidated_tape_close`) are deliberately stored separately. Most consumers use `close` (alias for `raw_close`); index providers and ETF NAV consumers use `official_auction_close`; market-microstructure researchers use `consolidated_tape_close`.
- The `adj_close_*` columns are *snapshotted* — they reflect the cumulative adjustment factor as of `knowledge_from`. When a new split occurs, all prior rows are NOT updated in place; instead, a new row with a later `knowledge_from` is inserted (or `cfacpr` is updated on a separate `adjustment_factor_history` row — see §7.6). This preserves PIT semantics: a back-test asof 2020-01-01 sees the 2020 adjustment factor; the same back-test asof 2026-01-01 sees the cumulative-through-2025 factor.
- `source_id` is a foreign key into a `source` dimension table that tracks vendor-level provenance. A single `(security_id, date)` can have multiple rows from different sources, and the ats-eqt query layer applies a vendor-priority policy to resolve.

### 7.2 `bar_intraday` — sub-daily bars

```sql
CREATE TABLE bar_intraday (
  security_id          BIGINT       NOT NULL,
  ts                   TIMESTAMP(9) NOT NULL,     -- nanosecond-precision bar start
  bar_seconds          INTEGER      NOT NULL,     -- 1, 5, 60, 300, 3600, 86400
  open                 DOUBLE       NOT NULL,
  high                 DOUBLE       NOT NULL,
  low                  DOUBLE       NOT NULL,
  close                DOUBLE       NOT NULL,
  volume               BIGINT       NOT NULL,
  vwap                 DOUBLE       NULL,
  num_trades           INTEGER      NULL,
  exchange_mic         CHAR(4)      NULL,         -- NULL = consolidated
  feed                 CHAR(8)      NOT NULL,     -- 'SIP', 'IEX', 'OTC', 'ARCA', ...
  source_id            INTEGER      NOT NULL,
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (security_id, ts, bar_seconds, feed, source_id, knowledge_from)
);

-- Partition by (year, month) on ts; cluster by security_id within partition.
```

### 7.3 `quote_eod` — end-of-day bid/ask

```sql
CREATE TABLE quote_eod (
  security_id          BIGINT       NOT NULL,
  date                 DATE         NOT NULL,
  bid                  DOUBLE       NOT NULL,
  ask                  DOUBLE       NOT NULL,
  bid_size             INTEGER      NULL,
  ask_size             INTEGER      NULL,
  midquote             DOUBLE       GENERATED ALWAYS AS ((bid + ask)/2) STORED,
  spread_bps           DOUBLE       GENERATED ALWAYS AS (10000*(ask-bid)/((bid+ask)/2)) STORED,
  close_type           CHAR(1)      NOT NULL,     -- 'A' auction, 'T' tape, 'B' bid-ask mid (no trade)
  source_id            INTEGER      NOT NULL,
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (security_id, date, source_id, knowledge_from)
);
```

`close_type = 'B'` captures the CRSP bid-ask-midpoint convention (PRC stored negative) — this is critical for survivorship-free analytics on illiquid names that occasionally don't trade.

### 7.4 `shares_outstanding_history`

```sql
CREATE TABLE shares_outstanding_history (
  security_id          BIGINT       NOT NULL,
  eff_date             DATE         NOT NULL,     -- effective date (or filing date)
  basic_shares         BIGINT       NOT NULL,     -- common shares issued and outstanding
  diluted_shares       BIGINT       NULL,         -- diluted share count (from latest 10-Q EPS)
  treasury_shares      BIGINT       NULL,         -- treasury holdings
  float_estimate       BIGINT       NULL,         -- public float (basic - insider - 5%+ holders)
  share_class          VARCHAR(16)  NOT NULL DEFAULT 'COMMON',
  source_id            INTEGER      NOT NULL,     -- 'EDGAR-XBRL'|'CRSP'|'COMP'|'BBG'|...
  valid_from           DATE         NOT NULL,
  valid_to             DATE         NOT NULL DEFAULT '9999-12-31',
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (security_id, eff_date, source_id, knowledge_from)
);
```

Three different "shares outstanding" sources to reconcile:

- **EDGAR XBRL** (`dei:EntityCommonStockSharesOutstanding`) — filing-driven, exact, but only updates on 10-Q / 10-K / 8-K.
- **CRSP `SHROUT`** — daily-updated for active names; in thousands; reflects CRSP's analyst-curated count.
- **Compustat `CSHOC`** — filing-driven; updates with `co_secd`.
- **Bloomberg `EQY_SH_OUT`** — Bloomberg's daily count; usually matches CRSP within 0.5%.

### 7.5 `short_interest`

```sql
CREATE TABLE short_interest (
  security_id                  BIGINT       NOT NULL,
  settle_date                  DATE         NOT NULL,    -- settlement date (or trade date for daily)
  cadence                      CHAR(1)      NOT NULL,    -- 'B'i-monthly, 'D'aily-flow, 'I'ntraday
  short_quantity               BIGINT       NULL,        -- shares short (stock) or shorted today (flow)
  short_exempt_quantity        BIGINT       NULL,        -- daily-flow only
  total_volume                 BIGINT       NULL,        -- daily-flow only
  short_percent_of_float       DOUBLE       NULL,
  days_to_cover                DOUBLE       NULL,
  borrow_fee_bps               DOUBLE       NULL,        -- commercial only
  utilization                  DOUBLE       NULL,        -- commercial only
  source_id                    INTEGER      NOT NULL,    -- 'FINRA-BI'|'FINRA-DAILY'|'S3'|'MARKIT'|...
  market_category              CHAR(1)      NULL,        -- N|Q|A|B|D for FINRA daily
  valid_from                   DATE         NOT NULL,
  valid_to                     DATE         NOT NULL DEFAULT '9999-12-31',
  knowledge_from               TIMESTAMP    NOT NULL,
  knowledge_to                 TIMESTAMP    NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (security_id, settle_date, cadence, source_id, knowledge_from)
);
```

### 7.6 `adjustment_factor_history`

Cross-link to the forthcoming `corporate_actions.md`. Every split, stock dividend, spinoff, and ordinary dividend produces a row.

```sql
CREATE TABLE adjustment_factor_history (
  security_id          BIGINT       NOT NULL,
  ex_date              DATE         NOT NULL,
  event_type           VARCHAR(16)  NOT NULL,    -- 'SPLIT'|'STOCK_DIV'|'CASH_DIV'|'SPINOFF'|'RIGHTS'
  event_ref_id         BIGINT       NULL,        -- → corporate_action row in corporate_actions schema
  factor_price         DOUBLE       NOT NULL,    -- multiplier to apply to prior-day prices to get post-event price space
  factor_shares        DOUBLE       NOT NULL,    -- multiplier to apply to prior shares outstanding
  factor_volume        DOUBLE       NOT NULL,    -- = factor_shares for splits
  ratio_numerator      DOUBLE       NULL,        -- raw split ratio if available
  ratio_denominator    DOUBLE       NULL,
  cash_div_amount      DOUBLE       NULL,        -- for CASH_DIV
  cash_div_currency    CHAR(3)      NULL,
  spinoff_security_id  BIGINT       NULL,        -- → security for SPINOFF type
  spinoff_share_ratio  DOUBLE       NULL,
  source               VARCHAR(8)   NOT NULL,    -- 'CRSP'|'EDGAR'|'POLY'|'TIINGO'|...
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (security_id, ex_date, event_type, source, knowledge_from)
);
```

The `cfacpr` column in `bar_daily` is then materialised by walking forward over `adjustment_factor_history` and accumulating `factor_price`; same for `cfacshr` with `factor_shares`.

### 7.7 Cross-references

- `security_id` joins to ats-eqt's `security` table from `data_models_and_methodology.md` §G.1.
- `event_ref_id` in `adjustment_factor_history` joins to the corporate-action fact table in `corporate_actions.md` (forthcoming).
- `source_id` is a foreign key into a `source` dimension that tracks vendor-level provenance per row.

### 7.8 PIT view

The same pattern as `data_models_and_methodology.md` §G.2:

```sql
CREATE VIEW bar_daily_pit AS
SELECT * FROM bar_daily
WHERE knowledge_from <= :asof_date
  AND :asof_date < knowledge_to;
```

A query "what did CRSP think AAPL closed at on 2014-06-09, as known on 2014-08-01?" resolves through this view by passing `:asof_date = '2014-08-01'` and `source_id = 'CRSP'`.

---

## 8. Storage strategy notes

### 8.1 Columnar layout

`bar_daily` is a textbook fit for columnar storage: ~25 columns, mostly DOUBLE / BIGINT, with very high time-locality (consecutive rows for the same security on consecutive dates). Recommended layout:

- **Primary partition:** by `date` year. ~250 trading days × ~10,000 securities × 25 cols × 8 bytes = ~500 MB per year per security in uncompressed form, compressing to ~50-150 MB with zstd-3.
- **Secondary cluster:** by `security_id` within partition. Cache-line co-locates same-security consecutive-day rows for fast range scans.
- **Compression:** zstd at level 3; per-column dictionary encoding for `currency`, `primary_exchange_mic`, `close_source`, `source_id`. DELTA encoding for `date`. DELTA_BINARY_PACKED for `volume` and `num_trades`.

`bar_intraday` is the volume challenge. At 1-second bars × 6.5 hr trading × 10k symbols = ~234M rows/day. Storage strategies:

- Partition by `(year, month)` on `ts`; **further partition by `bar_seconds`** so that the daily-and-bigger aggregates are physically separate from the second-and-minute granularity.
- Cluster by `security_id` within partition.
- Use Parquet row-group size ~64-128 MB so a single Parquet file lands at 1-4 GB.
- Layout: `s3://ats-eqt/pricing/v1/bar_intraday/year=2026/month=05/bar_seconds=60/part-000.parquet`.

`bar_intraday` 1-second-bar storage at full universe is ~50-100 TB/yr `[unverified, depends on universe and after compression]`. 1-minute bars are ~1-2 TB/yr. Most quant teams down-sample to 1-minute for back-test storage and retain 1-second only for the last rolling 90 days.

### 8.2 Tick storage and compression

TAQ-equivalent raw tick data (NBBO + trade tape) is the highest-volume tier. Patterns:

- **Per-day, per-symbol files.** One file per `(security_id, date)`. For S&P 500 + Russell 2000 = ~2500 symbols × ~250 days = ~625k files/yr at typical 1-50 MB per file (compressed). This is the WRDS Daily TAQ-on-S3 pattern.
- **Per-day, all-symbols files.** One file per date with all symbols. Larger files (~10-60 GB compressed), better for daily back-tests that scan the cross-section.
- **Dedicated tick formats.** Apache Arrow Flight, kdb+ splayed tables, and the Databento Memory Buffer Protocol (MBP) format outperform Parquet for tick replay by ~5-10× on read.

### 8.3 The ats-core ANSE mapping

ats-core's append-only segment-file format (the same one ats-crypto uses for tick-level Solana order books) maps onto `bar_intraday` directly:

- Each segment is a `(security_id, bar_seconds, day)` triple.
- Segments are written append-only as bars are finalised.
- The columnar block layout uses ats-core's existing per-column compression (run-length + dictionary + zstd).
- PIT semantics: segment supersession is via tombstone records, so an `adjustment_factor_history` change that retroactively updates `cfacpr` is implemented as a new segment with `knowledge_from = now()` rather than an in-place edit.

This is the same pattern in `data_models_and_methodology.md` §F.6.

### 8.4 Partitioning strategy by query pattern

| Query pattern | Optimal partition key |
|---|---|
| Single-security back-test over long history | Cluster by `security_id`; partition by year |
| Single-day cross-sectional scan (e.g. universe sort by mkt cap) | Partition by `date`; no cluster |
| PIT-as-of-date join with fundamentals | Partition by `knowledge_from` year-month |
| Intraday execution-quality replay | Partition by `(date, bar_seconds)` |
| Survivorship analysis (active + delisted) | Cluster by `security_id`, no `is_active` filter |

ats-eqt's recommended default: `PARTITION BY year(date), security_chunk` where `security_chunk = security_id % 32`. This balances single-security range scans (32× fan-out, all in one chunk) against cross-sectional scans (32× parallelism).

---

## 9. Open questions / wave-3 gaps

1. **Spinoff adjustment policy.** No two vendors agree on the canonical formula for spinoff-driven price adjustment. ats-eqt needs an explicit policy (proportional-dividend approach? value-attribution at first traded close?) and a regression suite vs CRSP + Tiingo + Yahoo to detect drift. Tracking item for wave-3.
2. **The corporate_actions.md cross-link.** This document forward-references `corporate_actions.md` (forthcoming, wave-3) for the per-event corporate-action fact table. The `event_ref_id` column in `adjustment_factor_history` is the join key. Needs to be written before the §7.6 schema can be deployed.
3. **OPRA options data scope.** Section 3.10 punts on options coverage. A future `options_data.md` should cover OPRA, ORATS, CBOE LiveVol, plus the IV-surface storage problem.
4. **Cross-listing canonical-price policy.** When the same security trades in multiple jurisdictions (ADR + home market, Canadian dual-listed, etc.), which venue's close is "the" close? Datastream uses the primary listing's close converted at the FX fix; Bloomberg uses the composite. ats-eqt needs an explicit per-security `canonical_price_venue` choice in the security master.
5. **Crypto pricing layer.** ats-crypto uses an independent pricing infrastructure (Pyth, Birdeye, Jupiter aggregator) for Solana/HFT work. The schema in §7 is equity-specific; a parallel `crypto_bar_daily / crypto_bar_intraday` set of tables is implied but not designed here.
6. **Index constituent history.** The S&P 500 / Russell 2000 / NASDAQ-100 constituent histories (additions, deletions, weights) are a separate dataset from pricing, not yet covered in any ats-eqt wave-2 file. Likely belongs in `index_data.md` (wave-3).
7. **Pre-1993 intraday.** TAQ starts 1993-09. There is no public source of pre-1993 US intraday data; the only academic dataset is the ISSM (Institute for the Study of Security Markets) tape, hosted at Rutgers and now severely restricted in access. ats-eqt should explicitly scope its intraday history to 1993-09 → present.
8. **The single-vendor delisting-return gap.** CRSP's `dlret` field is the only one of the vendors surveyed that consistently provides post-delisting recovery returns. Polygon and Tiingo just drop delisted tickers from the active universe with no terminal return. ats-eqt needs a custom delisting-return pipeline (likely sourced from a combination of FINRA Form 25 + OTC Markets Pink Open Market data + SEC Form 15) to match CRSP's quality on the open-data side. Estimated 4-8 week build.
9. **The Bloomberg-vs-CRSP-vs-Tiingo close-price reconciliation regression.** No vendor publishes their close-price formula in full. ats-eqt should run a daily regression suite that compares `close` across all enrolled vendors per symbol and flags drift > 50 bps; treat as a data-quality alert.
10. **PIT integrity of free APIs.** Yahoo and Polygon both restate their `adj_close` every time a corporate action occurs, with no historical-snapshot retention. An ats-eqt back-test built on Polygon-as-source for prior periods has no way to recover the contemporaneous adjustment factor. This is the fundamental reason CRSP remains the academic standard and is the strongest argument for paying for an institutional pricing source rather than building purely on public APIs.

---

## 10. Sources

### CRSP
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Data_Descriptions_Guide.pdf> — CRSP US Stock & Indexes Data Descriptions Guide
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Guide_Flat_File_Format_1.0.pdf> — CRSP Flat File Format Guide
- <https://www.crsp.org/products/research-products/> — CRSP product page
- <https://www.crsp.org/products/documentation/data-definitions-d> — CRSP data definitions
- <https://terpconnect.umd.edu/~wermers/ftpsite/fnce7200/data_defs_061899.pdf> — CRSP Data Definitions PDF (academic mirror)
- <https://ionmihai.github.io/finsets/01_wrds/crspd.html> — CRSP DSF field list and delisting-adjust helper
- <https://wrds-www.wharton.upenn.edu/demo/crsp/form/> — WRDS CRSP demo
- <https://gsb-research-help.stanford.edu/library/faq/277946> — Stanford CRSP download knowledge base
- <https://www.sfu.ca/sasdoc/sashtml/ets/chap10/sect39.htm> — SAS CRSP Stock Files reference
- <http://kaichen.work/?p=248> — CRSP vs Compustat market value methodology

### Compustat / CCM
- <https://www.otago.ac.nz/library/pdf/CRSPCompustatguide09.pdf> — CRSP/Compustat Merged Database Guide
- <https://docs.nuvolos.com/user-guides/data-guides/working-with-crsp-and-compustat> — Working with CRSP and Compustat
- <https://mingze-gao.com/posts/merge-compustat-and-crsp/> — Merging Compustat and CRSP
- <https://www.kaichen.work/?p=138> — CCM linktable analysis
- <https://gist.github.com/iangow/583557b7b91a87ee1e545aa839ccbb8d> — CRSP-Compustat merge brief
- <https://www.projectrhea.org/rhea/index.php/CRSP_compustat_merged_database_in_WRDS> — Rhea CCM walkthrough
- <https://sites.google.com/site/ruidaiwrds/data/linking-crsp-and-compustat> — Rui Dai CCM guide
- <https://www.ruidaiwrds.info/posts/crsp-compustat> — CRSP-Compustat linking
- <https://gist.github.com/iangow/fca4cb10b048f5c798113da7039c2688> — Comparison of three CCM link tables
- <http://finabase.blogspot.com/2017/10/return-data-and-market-value-in.html> — Databaser: return data and market value in Compustat
- <https://community.portfolio123.com/uploads/short-url/tHKKrq3JHjUaqym1zJrNvj0egTK.pdf> — Compustat Xpressfeed Understanding the Data
- <https://www.scribd.com/document/330073981/S-P500-Daily-Metadata> — Compustat Daily Price variables
- <https://www.marketplace.spglobal.com/en/datasets/compustat-financials-(8)> — S&P Compustat Financials dataset
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/standardized-fundamentals/sp-compustat-database> — LSEG on S&P Compustat
- <http://volweb.utk.edu/~pdaves/Computerhelp/COMPUSTAT/Compustat_manuals/user_02.pdf> — Compustat NA Database manual ch.2

### FactSet Prices
- <https://www.factset.com/marketplace/catalog/product/factset-prices-and-returns-api> — FactSet Prices & Returns API
- <https://developer.factset.com/api-catalog/factset-prices-api> — FactSet Prices API developer page
- <https://go.factset.com/hubfs/Website/Website_Downloads/Statistical%20Package%20Integration/factset%20ondemand%20web%20services%20reference%20manual_2.0.pdf> — FactSet OnDemand Web Services manual
- <https://assets.ctfassets.net/lmz2w5z92b9u/6pYFSlTdzePhGtyyP5g6z9/90d73346337cc18e14b295b17dcc19bd/FactSetExchangeDataFeed_DataModel_V2.0L.pdf> — FactSet Exchange DataFeed Data Model
- <https://doc.exabel.com/dsl/data_signals/factset_prices_shares.html> — Exabel FactSet Prices & Shares reference
- <https://go.factset.com/hubfs/Website_Downloads/Exchange%20DataFeed/data%20service%20manual%202.0b.pdf> — FactSet Data Service manual

### Bloomberg
- <https://bautheac.github.io/BBGsymbols/> — Bloomberg field-name catalog
- <https://github.com/dappled/AFData/blob/master/albertfriedMarketData/src/bbgRequestor/bloomberg/BbgNames.java> — Bloomberg field constants
- <https://data.bloomberglp.com/professional/sites/10/189913_CDS_REF_Fundamentals_SFCT_DIG.pdf> — Bloomberg Fundamentals fact sheet
- <https://professional.bloomberg.com/products/data/data-management/data-license/> — Bloomberg Data License
- <https://www.bloomberg.com/company/press/bloomberg-announces-port-enterprise-data-delivery-to-snowflake-with-data-license-plus-dl/> — DL+ on Snowflake
- <https://godeldiscount.com/blog/bloomberg-terminal-cost-2026> — Bloomberg Terminal pricing
- <https://michael-mao.gitbook.io/bloomberg/bql/bloomberg-query-language-bql> — BQL primer (Michael Mao)
- <https://www.bloomberg.com/professional/products/bloomberg-terminal/research/bquant/> — BQuant
- <https://libfaq.smu.edu.sg/faq/134746> — Bloomberg Excel formulas (BDP/BDH/BDS)

### LSEG / Refinitiv
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals> — LSEG Worldscope
- <https://wrds-www.wharton.upenn.edu/documents/1492/Thomson_Refinitiv_Datastream.pdf> — Datastream brochure
- <https://fmc.refinitiv.com/clientFacing/pdf/DFO_User_Guide.pdf> — Datastream for Office user guide
- <https://bigiavi.sba.unibo.it/cataloghi-e-risorse-online/eikon-datastream/datastream_guida.pdf/@@download/file/Datastream_guida.pdf> — Datastream guide
- <https://finm-32900.github.io/lectures/Week7/LSEG_datastream.html> — LSEG Datastream finance-32900 lecture
- <https://www.refinitiv.com/en/financial-data/market-data/tick-history> — Refinitiv Tick History
- <https://developers.lseg.com/en/article-catalog/article/boost-tick-history-downloads-with-aws> — RTH on AWS
- <https://developers.lseg.com/en/article-catalog/article/big-data-tick-history-google-bigquery> — RTH on BigQuery
- <https://aws.amazon.com/marketplace/pp/prodview-yi3aovwrufwua> — RTH on AWS Marketplace
- <https://solutions.refinitiv.com/point-in-time> — Refinitiv Point-In-Time
- <https://developers.lseg.com/en/api-catalog/refinitiv-data-platform/refinitiv-data-platform-apis> — RDP API catalog

### NYSE TAQ
- <https://www.nyse.com/market-data/historical/daily-taq> — NYSE Daily TAQ product page
- <https://www.nyse.com/publicdocs/nyse/data/Daily_TAQ_Client_Spec_v4.2.pdf> — Daily TAQ Client Spec v4.2 (Aug 2025)
- <https://www.nyse.com/publicdocs/nyse/data/Daily_TAQ_Client_Spec_v4.0.pdf> — Daily TAQ Client Spec v4.0
- <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/nyse-trade-and-quote-taq/> — WRDS NYSE TAQ page
- <https://wrds-www.wharton.upenn.edu/documents/1301/NYSE_TAQ_PRINT.pdf> — WRDS TAQ documentation
- <https://www.library.hbs.edu/find/databases/trades-and-quotes-taq> — HBS Baker library on TAQ
- <https://library.uncw.edu/eresources/new_york_stock_exchange_trade_quote_nyse_taq> — UNCW TAQ guide

### Polygon.io
- <https://polygon.io/docs/stocks/get_v2_aggs_ticker__stocksticker__range__multiplier___timespan___from___to> — Polygon Aggregates endpoint
- <https://polygon.io/pricing> — Polygon pricing tiers
- <https://polygon.io/docs/stocks> — Polygon Stocks API docs

### Tiingo
- <https://www.tiingo.com/documentation/end-of-day> — Tiingo EOD API
- <https://www.tiingo.com/documentation/iex> — Tiingo IEX intraday API
- <https://www.tiingo.com/> — Tiingo home
- <https://github.com/hydrosquall/tiingo-python> — tiingo-python SDK
- <https://business-science.github.io/riingo/> — R interface to Tiingo

### Alpaca
- <https://docs.alpaca.markets/us/reference/stockbars> — Alpaca historical bars endpoint
- <https://alpaca.markets/learn/fetch-historical-data> — How to fetch historical data
- <https://docs.alpaca.markets/us/docs/market-data-faq> — Alpaca market data FAQ
- <https://docs.alpaca.markets/us/docs/getting-started-with-alpaca-market-data> — Getting started

### IEX Cloud (legacy)
- <https://iexcloud.io/docs/api/> — IEX Cloud legacy API
- <https://iexcloud.io/core-data-catalog> — IEX Cloud data catalog
- <https://iexcloud.org/top-stock-api-guide> — IEX Cloud retirement notice
- <https://addisonlynch.github.io/iexfinance/stable/stocks.html> — iexfinance Python SDK

### Yahoo Finance
- <https://github.com/ranaroussi/yfinance> — yfinance library
- <https://github.com/ranaroussi/yfinance/issues/1749> — Close vs Adj Close discussion
- <https://www.quantvps.com/blog/yahoo-finance-api-documentation> — Yahoo Finance API documentation
- <https://algotrading101.com/learn/yfinance-guide/> — yfinance complete guide
- <https://github.com/gadicc/yahoo-finance2> — yahoo-finance2 (Node)
- <https://eohne.github.io/YFinance.jl/dev/Prices/> — YFinance.jl Prices

### FINRA / public short-interest data
- <https://www.finra.org/filing-reporting/regulatory-filing-systems/short-interest> — FINRA Short Interest Reporting
- <https://www.finra.org/finra-data/browse-catalog/equity-short-interest> — Equity Short Interest catalog
- <https://www.finra.org/finra-data/browse-catalog/equity-short-interest/data> — Equity Short Interest data download
- <https://www.finra.org/finra-data/browse-catalog/equity-short-interest/glossary> — Equity Short Interest glossary
- <https://www.finra.org/sites/default/files/Equity_Short_Interest_Data_File_Download_API.pdf> — Equity Short Interest data file API spec
- <https://www.finra.org/sites/default/files/notice_doc_file_ref/Regulatory-Notice-16-32.pdf> — Reg Notice 16-32 (short interest reporting)
- <https://www.finra.org/filing-reporting/short-interest/regulation-filing-applications-instructions> — Reporting instructions
- <https://www.finra.org/filing-reporting/regulatory-filing-systems/short-interest/faq> — Short Interest FAQ
- <https://www.finra.org/finra-data/browse-catalog/short-sale-volume-data> — Short Sale Volume data
- <https://www.finra.org/finra-data/browse-catalog/short-sale-volume-data/daily-short-sale-volume-files> — Daily Short Sale Volume files
- <https://www.finra.org/sites/default/files/2020-12/short-sale-volume-user-guide.pdf> — Short Sale Volume user guide
- <https://www.finra.org/rules-guidance/notices/information-notice-051019> — Understanding short sale volume
- <https://shortvolume.com/> — shortvolume.com aggregator
- <https://blog.otcmarkets.com/2023/05/08/what-investors-should-know-about-finra-daily-short-sale-volume-data/> — OTC Markets on FINRA daily data

### NYSE / Cboe / NASDAQ public summary
- <https://www.nyse.com/publicdocs/nyse/data/NYSE_Group_Short_Interest_Client_Specification_v1.6.pdf> — NYSE Group Short Interest spec
- <https://www.nyse.com/markets/us-equity-volumes/historical> — NYSE historical volumes
- <https://datashop.cboe.com/> — Cboe DataShop
- <https://www.cboe.com/data/market_statistics/> — Cboe Market Statistics
- <https://www.cboe.com/us/options/market_statistics/historical_data/> — Cboe historical options data
- <https://www.cboe.com/markets/us/options/market-statistics/daily> — Cboe US Options Daily Market Stats

### Commercial short-interest vendors
- <https://www.s3partners.com/> — S3 Partners home
- <https://www.s3partners.com/short-interest-data> — S3 short interest data
- <https://www.s3partners.com/data-predictive> — S3 predictive analytics
- <https://www.prnewswire.com/news-releases/s3-partners-launches-real-time-short-interest-analytics-on-the-bloomberg-app-portal-300289459.html> — S3 Black App launch
- <https://aws.amazon.com/marketplace/pp/prodview-l3a2kf5yecuy6> — S3 on AWS Marketplace
- <https://www.spglobal.com/marketintelligence/en/solutions/products/securities-finance> — S&P Securities Finance (ex-Markit)

### OPRA / Options
- <https://databento.com/datasets/OPRA.PILLAR> — Databento OPRA.PILLAR dataset
- <https://databento.com/microstructure/opra> — Databento OPRA microstructure guide
- <https://www.bmlltech.com/products/bmll-data-feed/opra-data> — BMLL OPRA feed
- <https://dxfeed.com/market-data/options/opra/> — dxFeed OPRA
- <https://developer.ice.com/fixed-income-data-services/catalog/opra-options-price-reporting-authority> — ICE OPRA
- <https://www.lseg.com/en/data-analytics/financial-data/pricing-and-market-data/options-data/options-price-reporting-authority> — LSEG OPRA
- <https://databento.com/docs/venues-and-datasets/opra-pillar> — OPRA feed spec
- <https://www.bmlltech.com/files/documents/BMLL-Trades-OPRA.pdf> — BMLL Trades OPRA schema
- <https://www.factset.com/marketplace/catalog/product/options-price-reporting-authority-opra-real-time-feed> — FactSet OPRA real-time
- <https://orats.com/> — ORATS options historical
