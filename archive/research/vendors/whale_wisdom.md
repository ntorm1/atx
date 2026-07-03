# WhaleWisdom — Competitive Profile for ats-eqt

**Research date:** 2026-05-16
**Subject:** WhaleWisdom.com — privately-held SEC-filings aggregator focused on 13F institutional holdings, with 13D/G activist coverage, Form 3/4 insider data, and Form D / Form ADV adjacencies.
**Scope:** the canonical mid-market 13F vendor — public surface, API, data model, history depth, methodology (WhaleScore), pricing, licensing posture, and the build-out required for ats-eqt to be a credible direct competitor.
**Companion files:** `factset.md` (Ownership / Revere), `sp_global.md` (Compustat / Capital IQ), `refinitiv_bloomberg.md` (LSEG Ownership), `supply_chain_specialists.md`. Dataset-level deep-dives in `datasets/13f_holdings.md` and `datasets/insider_ownership.md`.

---

## 0. Executive summary

WhaleWisdom is the **single most important head-to-head benchmark for ats-eqt's 13F product**. It is not a tier-1 enterprise vendor (FactSet / S&P / Bloomberg / LSEG own that lane) and it is not a free SEC-mirror (`13F.info`, `sec-api.io` free tier, Quiver retail). It sits in a deliberate **$300–$500/yr "prosumer + boutique buy-side" wedge** that is also where the most direct demand for a developer-friendly open-data competitor exists.

Four findings frame the rest of the document:

1. **The data substrate is fully public and identical to ours.** WhaleWisdom's own About page admits: *"WhaleWisdom.com SEC filing data (13F's, 13D/G's, Form 3/4) collection is done in house and is entirely automated with little to no human intervention"* (source: <https://whalewisdom.com/info/about>). There is no proprietary data acquisition pipeline to clone — only an automated EDGAR ingester. ats-eqt's `holdings_13f.ingest` module already implements the equivalent.
2. **The moat is entity resolution + analytics, not ingest.** WhaleWisdom's defensible value-adds are: (a) a stable internal **filer_id / stock_id** keyspace that survives CIK churn / CUSIP reissuance, (b) the **WhaleScore** regression-based fund-quality metric, (c) a **Backtester** that supports portfolio cloning with realistic frictions, and (d) cross-form joins (13F ↔ 13D/G ↔ Form 4) glued to those internal IDs.
3. **The API is the product.** The customer-facing site is a thin shell over a single endpoint (`/shell/command`) with 12+ commands, HMAC-SHA1 signed requests, 20 req/min, and a 9-quarter free tier (source: <https://whalewisdom.com/shell/api_help>, <https://whalewisdom.com/help/api>). Enterprise gets *unlimited API + nightly FTP* — which is also exactly what a buy-side developer wants and what ats-eqt should ship as the headline integration.
4. **Their robots.txt and ToS forbid scraping; the EDGAR substrate beneath them does not.** The cleanest competitive posture for ats-eqt is **never to scrape WhaleWisdom**: build directly from EDGAR + FIGI + open identifiers, then expose a **superset API** (`/v1/holdings`, `/v1/holders`, etc.) that a WhaleWisdom Enterprise customer could swap in.

The Phase-0 13F dataset in `ats-eqt` already covers the *ingestion* half. The remaining gap to a saleable WhaleWisdom-replacement product is **~6 engineer-months of analytics + UX + entity-resolution** (see §9 for the build plan).

---

## 1. Company & corporate snapshot

| Item | Value | Source |
|---|---|---|
| Legal/public name | WhaleWisdom.com | <https://whalewisdom.com/info/about> |
| Founded | **2008** | <https://whalewisdom.com/info/about> |
| Corporate form | Privately held, no parent-company affiliation disclosed | <https://whalewisdom.com/info/about> |
| Public address | None published | <https://whalewisdom.com/info/contact> |
| Contact | `contact@whalewisdom.com` (single email + anonymous web form) | <https://whalewisdom.com/info/contact> |
| Disclaimer | *"whalewisdom.com is an aggregator of public SEC filings and is not affiliated with any of the companies shown on the website"* | <https://whalewisdom.com/info/contact> |
| Headcount | Not disclosed — site copy implies a small team; automated pipeline framing reinforces that | inferred |
| Revenue / ARR | Not disclosed; estimable from subscription mix (see §7) | — |

Notable: **no listed corporate office, no listed founders, no LinkedIn corporate page, no investor disclosure**. The operation reads as a small, owner-operated SaaS — perhaps the single most replicable competitor in this study.

---

## 2. Product surface

WhaleWisdom is a single web product (whalewisdom.com) plus an Excel Add-in plus a REST API. There is no terminal, no desktop client, no separate enterprise UI.

### 2.1 Public site sections (verified from sitemap.xml + manual fetch)

| Path | Purpose | Auth required | Renderable without JS |
|---|---|---|---|
| `/` | Home / marketing | No | Yes (marketing only) |
| `/info/*` | Static info pages (about, features, faq, subscription_info, whalescores, statistics_tab, contact, terms, disclaimer, privacy_policy, trademarks, ccpa) | No | Yes |
| `/help/api` | API command reference | No | Yes |
| `/shell` | Interactive API tester | No | No (JS-rendered) |
| `/shell/api_help` | API auth docs | No | Yes |
| `/shell/command(.html|.json|.csv)` | API endpoint | Optional (free tier: last 8 quarters; subscriber: full) | n/a |
| `/stock/{TICKER}` | Stock profile (holders, charts, options, 13D/G, insider) | No to view; some sub-tabs JS-only | No (JS-rendered) |
| `/filer/{slug}` | Filer profile (holdings, history, WhaleScore, alerts) | No to view; subscriber for downloads | No (JS-rendered) |
| `/dashboard2/*` | Backtester, screener, 13D/G search, fund performance evaluator, overlap matrix, heat map | Login | No (JS-rendered) |
| `/schedule13d` | **Retired** — redirects to `/dashboard2/other/schedule13` | n/a | n/a |
| `/filing/latest_filings` | Live ticker of incoming 13F filings | No | Partial (table loads via XHR) |
| `/report/heat_map` | Quarterly heatmap visualization | No (with subscriber depth) | No (JS-rendered) |

(Sitemap analysis: <https://whalewisdom.com/sitemap.xml>; manual fetch of each path.)

### 2.2 Feature surface

From the marketing pages (<https://whalewisdom.com/info/features>, <https://whalewisdom.com/info/subscription_info>):

**Analytics tools**
- **Backtester** — *"Backtest the 13F holdings of one or more firms and see how cloning their portfolio would compare to the market."*
- **Combined Holdings** — *"Show the combined portfolio of multiple firm's 13F holdings over time."*
- **Consensus Holdings** — *"Get consensus stock picks for a group of firms based on the group's 13F holdings."*
- **13F Heat Map** — quarterly visualization of hottest/coldest stocks
- **13F Trend Charts** — historical trend analysis per stock or filer
- **13F Stats** — aggregate quarterly statistics
- **Overlap Matrix** — fund-to-fund holding-overlap visualisation
- **WhaleTrader** — real-time tracking dashboard

**Screening & discovery**
- **13F Stock Screener** — search stocks by 13F turnover, portfolio-ranking metrics, WhaleScore-weighted ownership
- **13F Fund Performance Evaluator** — compare firm performance since 2001
- **Sector Search** — holdings aggregated by GICS sector
- **Insider Backtester** — Pro-only; backtests Form 4 insider transactions

**Cross-form data**
- **Schedule 13D/G Search** — *"WhaleWisdom processes new Schedule 13D/G filings daily"*; coverage back to **2006** (source: <https://whalewisdom.com/dashboard2/other/schedule13>)
- **Form 4 (insider transactions)** search
- **Form ADV** (investment adviser registration) — light coverage
- **Form D** (Reg D private placements) — Excel-download limited to 3,000 records per request (Standard tier)
- **N-SAR** — referenced; coverage shallow `[unverified — exact depth not documented]`

**Integrations**
- **Excel Add-in** — Windows (MSI installer, 1.0.39, January 2021) and Mac (Office Store v1.0.0); pulls 13F / 13D/G into Excel ranges; subscriber-only (source: <https://whalewisdom.com/info/excel_add_in>)
- **Developer API** — see §3
- **Email Alerts** — fire on new filings for specific filers, stocks, or watchlists
- **Nightly FTP** — Enterprise only; full 13F + 13D/G dumps

---

## 3. API — the de-facto product interface

This section is the centre of the document because the API **is** what a competing buy-side / quant customer will benchmark `ats-eqt` against.

### 3.1 Endpoint shape

Single endpoint: `https://whalewisdom.com/shell/command(.html|.json|.csv)`
Single query parameter: `args` = URL-encoded JSON request body.

All commands are dispatched via the `command` field inside `args`. Format negotiation is done via the file-extension suffix (`.html` default; `.json` and `.csv` supported where the command produces tabular data) (source: <https://whalewisdom.com/help/api>).

### 3.2 Authentication (HMAC-SHA1)

Two keys per user: a **shared access key** (public identifier) and a **secret access key** (never transmitted). Each signed request must include four query parameters: `args`, `api_shared_key`, `api_sig`, `timestamp` (source: <https://whalewisdom.com/shell/api_help>).

Signing procedure (verbatim from the docs, Ruby example):
```ruby
digest = OpenSSL::Digest::Digest.new('sha1')
hmac   = OpenSSL::HMAC.digest(digest, secret_access_key, args + "\n" + timestamp)
sig    = Base64.encode64(hmac).chomp.gsub(/\n/, '')
```
Timestamp format: ISO 8601 `2011-06-01T13:00:01Z`. Replay protection is enforced by an undocumented TTL on the signed window.

Alternative auth methods: browser session cookie (interactive use only); plain shared-key (legacy). HMAC-SHA1 is the recommended path for headless / automated clients.

### 3.3 Command catalogue

The full list of public API commands and parameters (from <https://whalewisdom.com/help/api>):

| Command | Required params | Optional params | Output |
|---|---|---|---|
| `quarters` | — | — | List of quarter IDs (`q_id`), end-dates, and per-tier access flags |
| `stock_lookup` | `name` **or** `symbol` | — | Stock ID, name, ticker, status |
| `filer_lookup` | one of `name` / `cik` / `id` / `city` / `state` / `state_incorporation` / `business_phone` / `irs_number` | `offset` (paginate beyond 1,000) | Filer ID, name, CIK |
| `stock_comparison` | `stockid`, `q1id`, `q2id` | `order`, `dir` | Per-filer change in shares of the stock between two quarters |
| `holdings_comparison` | `filerid`, `q1id`, `q2id` | `order`, `dir`, `filter` (SHARES/CALL/PUT/PRN), `stockid` | Per-stock delta in a filer's portfolio between two quarters |
| `export` | `filer_id`, `quarters[]`, `output` (1 single CSV, 2 per-quarter), `columns[]`, `email` | — | Async CSV export emailed to the user; **Standard tier capped at 50 filers/qtr** |
| `holdings` | `filer_ids[]` | `quarter_ids[]`, `stock_ids[]`, `all_quarters`, `sort`, `dir`, `limit`, `columns[]`, `include_13d` (1/0) | All holdings for given filers, optionally augmented with their 13D/G positions |
| `holders` | `stock_ids[]` | `filer_ids[]`, `quarter_ids[]`, `all_quarters`, `sort`, `dir`, `limit`, `columns[]`, `include_13d`, `hedge_funds_only` | All filers holding the given stocks |
| `filer_metadata` | `id` | — | Static descriptive fields for a filer (name, CIK, address, fund type, WhaleScore series) |

Holdings/holders columns are addressed by integer index `0–28`. Notable items in the column list (reconstructed from the API docs):

| Idx | Field | Notes |
|---|---|---|
| 0 | `filer_id` | Internal WhaleWisdom filer key |
| 1 | `filer_name` | Display name |
| 2 | `quarter_id` | FK to `quarters` |
| 3 | `stock_id` | Internal WhaleWisdom stock key |
| 4 | `symbol` | Ticker — may be null if unlisted |
| 5 | `cusip` | 9-char CUSIP (returned despite licensing concerns — see §6) |
| 6 | `name_of_issuer` | As reported in `infoTable.nameOfIssuer` |
| 7 | `title_of_class` | As reported |
| 8 | `value` | Market value, units shift in 2023-Q1 (see Quirks §5) |
| 9 | `shares` | Share count |
| 10 | `shares_type` | SH or PRN |
| 11 | `put_call` | NULL / PUT / CALL |
| 12 | `investment_discretion` | SOLE / DFND / OTR |
| 13 | `sole_voting` | int |
| 14 | `shared_voting` | int |
| 15 | `no_voting` | int |
| 16 | `percent_of_portfolio` | Computed: value / sum(value within filer-quarter) |
| 17 | `percent_ownership` | Computed: shares / shares_outstanding |
| 18 | `prior_shares` | Shares held in prior quarter |
| 19 | `share_change` | shares − prior_shares |
| 20 | `share_change_pct` | (shares − prior_shares) / prior_shares |
| 21–28 | filing metadata | `accession_number`, `filing_date`, `period_of_report`, `quarter_end_price`, … |

Most fields are direct echoes of the EDGAR `infoTable` XSD (see `datasets/13f_holdings.md` §A.4). The value-added fields are 16–20 (per-portfolio weighting, change-from-prior-quarter, percent-ownership) plus the join keys 0/3/2 — these are the join surface ats-eqt must match.

### 3.4 Rate limits & access tiers

- **20 requests / minute**, hard cap across all subscription levels (source: <https://whalewisdom.com/help/api>).
- **Free / non-subscriber:** *"Access to the last 8 quarters worth of data not including the current quarter."* — i.e., a rolling 9-quarter window with the current quarter blacked out.
- **Standard ($300/yr):** Full quarterly history back to 2001-Q1; export capped at 50 filers/qtr.
- **Pro ($500/yr):** 200 filers/qtr export, Insider Backtester unlocked.
- **Enterprise (custom):** *"Unlimited 13F data via the API"* + *"Nightly FTP Update Files for 13F/D/G"*.

### 3.5 Interactive shell

`/shell` is a browser UI that signs requests in-page using your stored keys — it doubles as a self-service Postman. It is JS-rendered, behind login, and not part of the documented surface (`api_help` references it as a "tester").

---

## 4. Data model (what they store, inferred from the API)

The WhaleWisdom internal schema is not published. Reconstructing it from API behaviour, response shapes, and the column dictionary:

### 4.1 Core tables (inferred)

| Table (inferred) | Purpose | Primary key | Notable columns |
|---|---|---|---|
| `quarters` | Calendar of 13F filing quarters | `id` | `filing_date_end`, `name` (`Q1 2026`), `access_tier` |
| `filers` | Section-13(f) institutional investment managers | `id` | `cik`, `name`, `address`, `state`, `state_incorporation`, `business_phone`, `irs_number`, `fund_type`, `aum_latest`, `whalescore_series_id` |
| `stocks` | 13(f)-eligible securities | `id` | `cusip`, `symbol`, `name_of_issuer`, `figi` `[unverified — not surfaced in API]`, `sector`, `mcap_latest`, `shares_outstanding` |
| `holdings` | Fact table — one row per (filer, quarter, security, attribution) | composite `(filer_id, quarter_id, stock_id, put_call, investment_discretion)` | `value`, `shares`, `shares_type`, voting authority breakdown, derived `percent_of_portfolio` / `percent_ownership` |
| `schedule13` | 13D/G filings | `id` | `filer_id`, `stock_id`, `event_date`, `percent_owned`, `intent_code` (D=active, G=passive), `filing_url` |
| `form4` | Insider transactions | `id` | `insider_id`, `issuer_stock_id`, `transaction_date`, `transaction_code`, `shares`, `price`, `post_balance` |
| `whalescores` | Per-filer, per-quarter score | composite `(filer_id, quarter_id)` | `whalescore`, `whalescore_1y_avg`, components (return, vol, alpha, concentration) |
| `alerts` / `watchlists` | User-side | per-user | n/a |

### 4.2 Joinability

Every fact table joins to `filers` and `stocks` via WhaleWisdom-minted integer IDs (`filer_id`, `stock_id`). This is the same architectural choice as FactSet's FSYM and CIQ's GVKEY — **stable internal keys** insulated from churn in upstream (CIK, CUSIP, ticker). The IDs are intentionally opaque (e.g. Berkshire = `filer_id=349`, Apple ≈ `stock_id=3598` — both visible in the API docs' example URLs).

For ats-eqt: these internal IDs are the chief identifier-resolution problem WhaleWisdom has solved that we have not yet hardened. See §9 build-plan item E.

### 4.3 What WhaleWisdom likely does NOT model

The API does not expose any of the following — strong signal these are not first-class entities:
- Restatements / amendment lineage (a 13F-HR/A appears to **overwrite** the prior `holdings` rows for that filer-quarter; no `knowledge_date` or `as-was` view is offered).
- Section 16 transactions disaggregated by Form 3 (initial), 4 (changes), 5 (annual cleanup) — only Form 4 surfaces in the Insider Backtester.
- Form SHO / 13F-2 short positions — compliance is now pushed to 2028 by the December 2025 SEC exemption (source: <https://www.sec.gov/newsroom/press-releases/2025-37>), so no vendor offers this yet.
- N-PORT fund-level monthly holdings (the 2024 amendments increased disclosure cadence). FactSet, S&P, LSEG all consume N-PORT — WhaleWisdom does not appear to.
- Confidential-treatment reveals (positions later disclosed after a CTR expires).

**ats-eqt opportunity:** every one of these is a wedge — particularly N-PORT integration, which is the single biggest data-quality differentiator vs. WhaleWisdom available today.

---

## 5. Methodology & data quality

### 5.1 Sourcing

Self-described: *"SEC filing data (13F's, 13D/G's, Form 3/4) collection is done in house and is entirely automated with little to no human intervention."* (<https://whalewisdom.com/info/about>). Refresh: *"WhaleWisdom checks with the SEC every hour to see if any new 13F filings have been posted. As soon as any are found they are automatically processed and released."* (<https://whalewisdom.com/info/faq>).

This implies a polling loop against either EDGAR full-text search (`https://efts.sec.gov/LATEST/search-index`) or the per-CIK submissions JSON. ats-eqt's `holdings_13f.ingest.edgar` module already exposes equivalent functionality; the latency floor on SEC dissemination plus a 60-min poll → typical WhaleWisdom availability ~60–90 min after filing acceptance.

### 5.2 WhaleScore (the only proprietary methodology)

The single piece of WhaleWisdom-specific IP. Quoted methodology (<https://whalewisdom.com/info/whalescores>):

> *"Measures showing the strongest relationship with predicting future Alpha are determined through regression analysis. Funds are ranked in terms of these metrics and scored against each other and against the S&P 500."*

Implementation specs:
- **Universe filter:** Not banks / insurance / trusts / pensions; **5–750 holdings**; **≥ 3 years of history**; **≥ 20% of portfolio concentrated in top 20 holdings** (i.e., active managers, not closet-indexers).
- **Lookback:** 3-year window for most metrics, 5-year for some.
- **Cadence:** Quarterly. The "WhaleScore 1y Avg" is the simple mean of the trailing 4 quarterly scores.
- **WhaleScore 2.0 (Feb 2020):** *"newly designed metrics to identify a fund portfolio's potential to outperform over time and a revised mixture of risk-return measures with the highest predictability for future Alpha."*
- **Output:** Single score per filer-quarter; presumably scaled (likely 0–100 or z-scored) — exact transformation not published `[unverified]`.

**For ats-eqt:** WhaleScore is a regression-derived fund-quality metric. It is replicable in under a quarter of work with public data (CRSP for returns, FF/Carhart for risk factors, 13F for holdings, simple panel-regression toolchain). The work has been done by academics for free — see e.g. *Cohen, Polk, Silli 2010* "Best Ideas" and the *Cremers-Petajisto* "Active Share" literature. ats-eqt should ship an equivalent (call it `WhaleScore`-equivalent — or differentiate with a name like `ManagerAlpha` / `ATSquality`) as a launch feature.

### 5.3 Known limitations they admit

- *"automated collection without human oversight increases data error risk and recommends users proceed cautiously"* (<https://whalewisdom.com/info/about>). Honest but a real product gap — there is no documented QA layer, no human content reviewers.
- No restatement / amendment lineage (see §4.3). A `13F-HR/A` from 2 years ago will silently change a backtest result. **This is a backtesting hazard** ats-eqt should fix from day zero with a bitemporal `(period_end, knowledge_date)` schema — already specified in `datasets/13f_holdings.md` and `schemas/data_models_and_methodology.md`.
- No PIT for derived fields (`percent_of_portfolio`, `percent_ownership` re-compute as `shares_outstanding` is revised post-corporate-actions).
- 13D/G coverage starts only **2006** — incumbents (FactSet, LSEG) reach back further (1986+ for some series).

### 5.4 Quirk: 2023-Q1 value-units cutover

A side-effect of the 2022 13F amendments (SEC release 34-94313): from 2023-Q1, `infoTable.value` is reported in **dollars** rather than thousands of dollars. WhaleWisdom's `value` column reflects this; downstream join-arithmetic must branch on `period_of_report >= 2023-01-01` or pre-multiply older rows by 1,000. (Source: <https://www.toppanmerrill.com/blog/sec-updates-edgar-on-jan-3-2023-for-form-13f-changes/>; reflected in `holdings_13f.ingest.unit_fix`.)

---

## 6. Identifier & licensing posture

### 6.1 What WhaleWisdom uses

- **CUSIP** — returned via the API column index 5, displayed in the UI for browsing.
- **Internal `stock_id`, `filer_id`** — opaque integers; effectively their permanent identifiers.
- **Ticker** — convenience field; can be null/stale.
- **CIK** — accepted as a search key in `filer_lookup`.
- **FIGI** — *not present in the documented column dictionary*. WhaleWisdom does not appear to expose FIGI even though SEC 2022 amendments allow it as an alternative 13F identifier (source: <https://www.openfigi.com/about/regulations>).

### 6.2 The CUSIP licensing exposure

WhaleWisdom appears to rely on the **"SEC public-record carve-out"** stance — CUSIPs sourced from a public SEC filing, redistributed in a paid product, *without* a CGS licence. This is exactly the posture the Dinosaur Financial Group class action (S.D.N.Y. 2022-03-04 v. CGS / S&P / FactSet / ABA) is litigating. Status as of 2026-05 is unsettled.

ats-eqt's planned posture (from `datasets/13f_holdings.md` Part B) — **cross-walk every CUSIP to FIGI at ingest and never re-publish the CUSIP downstream** — is the lower-risk option and **a marketable differentiator** versus WhaleWisdom for legally-cautious buyers (academics, redistribution-heavy fintechs).

### 6.3 Terms of service stance

The `/info/terms` and `/info/disclaimer` pages return 404 to anonymous fetches, but extracts captured in third-party reviews and the homepage footer indicate the standard prohibitions: no scraping, no automated access outside the API, no redistribution. Enterprise subscribers receive *limited* redistribution rights under bespoke contracts (specifically for the nightly FTP feed). For ats-eqt's posture: **we should not scrape WhaleWisdom**; we should build directly from EDGAR.

### 6.4 robots.txt (verbatim summary)

For research integrity, here are the directives we read from <https://whalewisdom.com/robots.txt>:

- `User-agent: *` — Disallowed: `/insider/`, `/stock/holdings`, `/filing/view/`, `/Archives`, plus several export-format paths
- `User-agent: Bingbot` — `Crawl-delay: 1`, plus `/filer/stock_history` additionally disallowed
- `User-agent: facebookexternalhit` / `FacebookBot` / `Applebot` — `Disallow: /` (full block)
- No `Sitemap:` directive in robots.txt despite `/sitemap.xml` existing

**Implication for our crawler (§8):** the "discovery crawler" we ship in ats-eqt MUST honour `/info/`, `/help/`, `/shell/api_help` (allowed) and MUST NOT touch the disallowed paths. It is a **catalogue mapper** — not a data extractor.

---

## 7. Pricing model

Verbatim from <https://whalewisdom.com/info/subscription_info>:

| Tier | Price | Group / watchlist size | Export cap (per 90 days) | API depth | FTP | Notable |
|---|---|---|---|---|---|---|
| **Free** | $0 | 5 filers per group | None — view only | 9-qtr rolling (current excluded) | — | 13F Heat Map; alerts; backtest; ads |
| **Standard** | $90 / 3 mo or **$300/yr** | 10 filers, 10 stocks | **50** stocks AND 50 funds | Full quarterly history to 2001 | — | Excel Add-in; Form D export ≤3,000 rows; ad-free; WhaleIndex |
| **Pro** | $150 / 3 mo or **$500/yr** | 50 filers, 50 stocks | **200** stocks/filers | Same | — | Insider Backtester; Combined Holdings; advanced backtest (WhaleScore filter, 30/50-holding clones, hedging) |
| **Enterprise** | Custom contact | Up to 5 team members; **unlimited** filer groups | Unlimited | **Unlimited API** + live-data feed calls | **Nightly FTP** 13F + 13D + 13G | Custom payment; no trial; no refunds |

### 7.1 Pricing implications for ats-eqt positioning

| Buyer segment | WhaleWisdom price | What they're actually paying for | ats-eqt entry price proposal |
|---|---|---|---|
| Retail / hobbyist | $90/qtr Standard | Ad-free UI + Excel | Free tier with full FIGI-keyed bulk parquet (no Excel UI) — undercuts WhaleWisdom on the data; loses on the UI |
| Boutique buy-side / RIA | $300–$500/yr Pro | Backtester, Combined Holdings | $500–$2,000/yr Pro tier with API + N-PORT integration + restatement-aware data |
| Hedge fund / quant team | $5k–$50k/yr Enterprise (estimated `[unverified]`) | Nightly FTP, unlimited API | $5k–$25k/yr — undercut Enterprise by 30–50% with redistributable FIGI-keyed bulk |
| Academic / replication packages | Often skip WhaleWisdom for WRDS | n/a | Free academic licence with redistribution rights (WhaleWisdom forbids redistribution; this is a wedge) |

ats-eqt's structural advantage: **redistributable open-data pricing**. WhaleWisdom's structural advantage: a **decade of brand recognition + a polished UI**.

---

## 8. Public surface map (for the discovery crawler)

This section enumerates everything a "polite mapping crawler" should be able to discover from public WhaleWisdom pages **without** violating robots.txt or terms.

### 8.1 Allowed (per robots.txt + ToS)

- `/sitemap.xml` — top-level URL index
- `/info/*` — all 13 informational pages
- `/help/api`, `/shell/api_help` — API docs
- `/filing/latest_filings` — public ticker (only the XHR-loaded table is gated by JS, not by robots)

### 8.2 Disallowed paths (must skip)

- `/insider/*`
- `/stock/holdings`
- `/filing/view/*`
- `/Archives`
- `/filer/stock_history`
- Any `*.csv` / `*.xls` / `*.xlsx` export paths

### 8.3 Inferred-but-not-allowed paths

These appear in the sitemap or in navigation links but are JS-rendered and gated behind login; do not crawl, only catalogue:
- `/dashboard2/other/schedule13`
- `/dashboard2/*` (backtester, screener, overlap matrix, heat map)
- `/stock/{TICKER}` (Holders / Activity / Schedule 13D-G / Insider / Charts tabs)
- `/filer/{slug}` (Holdings, History, WhaleScore tabs)
- `/report/heat_map`

### 8.4 What our crawler should produce

- A **catalogue** of public WhaleWisdom URLs → page title → brief content snapshot (text-only first 500 chars) → asset type (info | api-doc | data-product | gated)
- A **comparison report** that flags, for each WhaleWisdom feature, whether ats-eqt has parity (P), gap (G), or wedge advantage (W)
- A **terms-of-service compliance log** that records the robots.txt + the per-URL decision at crawl time (`allowed=true` / `skipped_reason=...`)

This is exactly the script delivered in [scripts/whalewisdom_crawl.py](../../scripts/whalewisdom_crawl.py) — see §10.

---

## 9. The 12-month build to a direct WhaleWisdom-equivalent

ats-eqt already has the ingestion half. The work remaining to ship a credible head-to-head competitor:

| Month | Workstream | Output |
|---|---|---|
| 1 | **Entity master:** mint stable `ats_filer_id` from CIK lineage; mint stable `ats_security_id` from FIGI + secondary keys | `identifiers.filer_master`, `identifiers.security_master` |
| 2 | **Manager-entity rollup:** join sub-advisor / parent-co / fund-family hierarchies; reconcile 13F manager <-> Form ADV adviser | Resolves the ~2–4% per-quarter filer churn |
| 3 | **Bitemporal holdings table:** `(filer, security, period_end, knowledge_date)` — the PIT view WhaleWisdom does not expose | `holdings_13f.bitemporal` view; replaces overwrite-on-amendment |
| 4 | **WhaleScore-equivalent metric** (we'd call it e.g. `ATSquality` or `ManagerAlpha`): 5-factor active-share / concentration / persistence regression | Quarterly fund-quality score per `(ats_filer_id, quarter_end)` |
| 5 | **Backtester v1:** portfolio-clone simulation with realistic frictions (lag, slippage, share-class handling) | API endpoint + Jupyter starter notebook |
| 6 | **N-PORT integration** (the biggest single wedge vs. WhaleWisdom): monthly fund-level holdings since Sept-2024 | `holdings_nport` parallel dataset |
| 7 | **13D/G + Form 4 surface:** activist tracker + insider cluster-buy/sell analytics | `insider_ownership.*` per `datasets/insider_ownership.md` |
| 8 | **Crowding + overlap matrix** analytics: smart-money cluster detection, sector-rotation heatmap | API endpoints + reference dashboards |
| 9 | **API parity surface:** every WhaleWisdom command → ats-eqt equivalent (`/v1/quarters`, `/v1/holdings`, `/v1/holders`, `/v1/filer_metadata`, …) plus N-PORT, PIT, and FIGI-only modes | OpenAPI spec + Python/JS SDKs |
| 10 | **Bulk + parquet distribution:** nightly partitioned parquet on S3/R2 + FIGI-keyed CSV — the "Enterprise FTP" replacement | Public bucket + signed-URL premium tier |
| 11 | **Excel & Sheets add-ins:** mirror WhaleWisdom's Excel functions; add Google Sheets via a Workspace add-on | distributable add-ins |
| 12 | **Launch + academic-licence rollout** | Public 1.0 |

**Cost envelope (rough):** 2 senior + 1 junior + 0.5 data-eng for 12 months ≈ $1.0–1.4M fully loaded — *one-tenth* the spend on a FactSet Ownership replacement at the same coverage. The WhaleWisdom moat is *not* in the ingestion or normalization but in the productized API + analytics layer.

---

## 10. The companion crawler

See **[scripts/whalewisdom_crawl.py](../../scripts/whalewisdom_crawl.py)** — a robots.txt-aware, low-rate, read-only discovery crawler. It:

1. Loads and parses `https://whalewisdom.com/robots.txt`; respects `Disallow` and `Crawl-delay`.
2. Walks the public surface starting from the `sitemap.xml` and `/info/*` index.
3. For each allowed URL: fetches, extracts the title + first 500 chars of visible text + any in-page links pointing back to `whalewisdom.com`.
4. Emits one JSONL row per URL into `data/whalewisdom_catalogue.jsonl`.
5. Optional `--search <term>` mode: case-insensitive text search across the captured catalogue (no second-pass network calls).
6. Optional `--compare` mode: writes a Markdown gap matrix (`data/whalewisdom_gap_matrix.md`) comparing WhaleWisdom features against ats-eqt's `holdings_13f` module surface.

It is intentionally **not** an API client, **not** an authenticated scraper, and **not** a bulk-downloader. It exists to (a) give a fresh inventory of WhaleWisdom's public surface at any given date, (b) feed competitive analysis with hard URLs + text snippets, (c) document what we *don't* fetch.

---

## 11. Strategic takeaways for ats-eqt

1. **WhaleWisdom is a $300–$500/yr SaaS sitting on a publicly-free data substrate**. The moat is UI + analytics + an opaque internal ID space — none of those is technically deep. A six-engineer-month sprint produces feature parity on the data side; the remaining gap is brand + UX.
2. **The API is the product.** Customers do not care about whalewisdom.com per se — they care about programmatic access to clean, normalized 13F + 13D/G + Form 4 with stable joinable IDs. ats-eqt should mirror the API command surface (`quarters`, `stock_lookup`, `filer_lookup`, `holdings_comparison`, `holdings`, `holders`, `filer_metadata`) so a switch-over is a base-URL change.
3. **The PIT / bitemporal gap is our biggest wedge.** WhaleWisdom overwrites on amendment; ats-eqt's planned `(period_end, knowledge_date)` model lets buy-side quants run honest backtests. This is a single-sentence pitch advantage.
4. **N-PORT is the second wedge.** Monthly fund-level holdings (mandatory disclosure since 2024 amendments) are not in WhaleWisdom's surface. They are an obvious institutional-grade differentiator.
5. **FIGI-only distribution is the third wedge.** WhaleWisdom serves CUSIPs and rolls the legal dice. ats-eqt FIGI-keyed bulk parquet is redistributable, academically-friendly, and CGS-licence-free. This unlocks segments WhaleWisdom cannot reach (replication packages, fintech SDKs, redistribution-as-a-service).
6. **WhaleScore is replicable and not a barrier.** Ship an equivalent quality metric with the v1 launch.
7. **Do not scrape WhaleWisdom.** The marginal data we'd gain is zero (all data is in EDGAR); the legal and reputational cost is real; the only reason to touch their site is competitive intelligence — which the included crawler handles politely.

---

## 12. Sources

### WhaleWisdom first-party
- [About WhaleWisdom](https://whalewisdom.com/info/about) — founding (2008), automated-collection admission, scope of forms processed.
- [Features](https://whalewisdom.com/info/features) — Backtester, Combined Holdings, Consensus Holdings, Heat Map, Overlap Matrix, WhaleTrader, Insider Backtester, Excel Add-in.
- [FAQ](https://whalewisdom.com/info/faq) — 13F definition, hourly EDGAR-poll cadence, 2001-Q1 history start, free-tier limit (9 quarters).
- [Subscription pricing](https://whalewisdom.com/info/subscription_info) — Free / Standard $300/yr / Pro $500/yr / Enterprise custom; export caps; nightly FTP at Enterprise.
- [WhaleScore methodology](https://whalewisdom.com/info/whalescores) — regression-derived alpha-prediction metric; 3y/5y lookback; quarterly cadence; 5–750 holdings + 20% top-20 concentration filter; WhaleScore 2.0 since Feb 2020.
- [API reference](https://whalewisdom.com/help/api) — full command catalogue, 20 req/min rate limit, 8-quarter free window, 29-column dictionary.
- [API authentication](https://whalewisdom.com/shell/api_help) — HMAC-SHA1 signing procedure, shared/secret key issuance, ISO 8601 timestamp, replay TTL.
- [Excel Add-in](https://whalewisdom.com/info/excel_add_in) — Windows 1.0.39 / Mac 1.0.0; subscriber-only; refresh button + most-recent-quarter mode.
- [Contact page](https://whalewisdom.com/info/contact) — single contact email; explicit "aggregator of public SEC filings, not affiliated" disclaimer.
- [Schedule 13D/G dashboard](https://whalewisdom.com/dashboard2/other/schedule13) — coverage back to 2006; search by date / ticker / investor.
- [Sitemap](https://whalewisdom.com/sitemap.xml) — public URL structure; 1,000+ `/stock/{TICKER}` pages; Schedule13D `daily` refresh; info pages `0.9` priority.
- [robots.txt](https://whalewisdom.com/robots.txt) — disallowed paths, Bingbot `Crawl-delay: 1`, full block on Facebook/Apple crawlers.
- [Home page](https://whalewisdom.com/) — *"Invest like a Wall Street money manager — at a Main Street price."*

### Cross-reference (existing ats-eqt research)
- [datasets/13f_holdings.md](../datasets/13f_holdings.md) — 13F dataset deep-dive (CUSIP licensing, SEC XSD, vendor matrix).
- [datasets/insider_ownership.md](../datasets/insider_ownership.md) — Schedule 13D/G, Form 3/4/5, N-PORT, Form 144, congressional disclosures.
- [vendors/factset.md](factset.md) — FactSet Ownership (the upmarket comparable to WhaleWisdom).
- [vendors/sp_global.md](sp_global.md) — S&P CIQ Ownership.
- [vendors/refinitiv_bloomberg.md](refinitiv_bloomberg.md) — LSEG Ownership + Bloomberg OWN / HDS.

### Regulatory / public-source primary
- [SEC EDGAR Form 13F XML Technical Specification (current draft v1.6)](https://www.sec.gov/edgar/filer-information/specifications/form13fxmltechspec-draft) — `infoTable` XSD ats-eqt mirrors directly.
- [SEC press release 2025-37 — Form SHO / 13F-2 compliance extension to 2028](https://www.sec.gov/newsroom/press-releases/2025-37) — short-position regime not active for any vendor yet.
- [OpenFIGI regulations page](https://www.openfigi.com/about/regulations) — FIGI as SEC-permitted 13F alternative identifier since June 2022.
- [Toppan Merrill — 2023-Q1 13F value-units cutover note](https://www.toppanmerrill.com/blog/sec-updates-edgar-on-jan-3-2023-for-form-13f-changes/) — units change from $K to $1 effective 2023-01-03.

### Third-party comparison / analyst
- [Dakota — WhaleWisdom vs Opportunity Hunter vs sec-api](https://www.dakota.com/resources/blog/whalewisdom-opportunity-hunter-sec-api-which-is-right-for-you) — independent feature comparison.
- [Dakota — Top 13F Databases 2023](https://www.dakota.com/resources/blog/the-top-13f-databases-for-2023) — list-with-prices.
- [WRDS Thomson Reuters s34 ownership data issues note](https://wrds-www.wharton.upenn.edu/pages/support/research-wrds/research-guides/research-note-regarding-thomson-reuters-ownership-data-issues/) — the pre-2013 backfile data-quality story.
- [WatersTechnology — CUSIP licensing debate](https://www.waterstechnology.com/data-management/7951850/as-legal-letters-fly-cusip-licensing-debate-rolls-on) — WhaleWisdom's exposure surface.
- [WatersTechnology — Dinosaur Financial Group class action](https://www.waterstechnology.com/regulation/7936086/class-action-lawsuit-takes-aim-at-cusip-sp-factset-aba) — open litigation.

---

## Closing notes

WhaleWisdom is the single clearest *direct-replacement* target for ats-eqt's 13F product line. Unlike FactSet / S&P / LSEG / Bloomberg — where direct replacement is implausible in a 12-month horizon — WhaleWisdom is **fully replicable on its data side in 6 engineer-months and saleable as a strict superset within 12** if we ship N-PORT + PIT + FIGI-only distribution. The included `scripts/whalewisdom_crawl.py` gives ongoing competitive-monitoring without depending on their internals or violating their ToS.
