# Refinitiv (LSEG) and Bloomberg — Equity Fundamentals & Supply Chain Vendor Research

> Competitive research for `ats-eqt`, an open-source-data competitor to FactSet / S&P / Refinitiv / Bloomberg for equity fundamentals + supply chain. Researched 2026-05-09. Inline citations link to source URLs. Items flagged `[unverified]` could not be cross-confirmed within this research pass.

---

## 1. Refinitiv (LSEG) — Worldscope / Datastream / I/B/E/S

### 1.1 Worldscope Fundamentals — schema and field naming

Worldscope is the long-running global standardized fundamentals dataset originally built by Wright Investors' Service / Disclosure, acquired by Primark, then Thomson Financial, then Thomson Reuters, and now operated by LSEG. It is the canonical non-Compustat fundamentals dataset, especially outside the US (source: https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals).

**Data templates.** Worldscope normalizes filings into four industry templates: Industrial, Bank, Insurance, and Other Financial. Each template has its own balance-sheet / income-statement / cash-flow line items and ratios (source: https://bizlib247.wordpress.com/2013/04/11/worldscope-coverage-and-data-definitions/).

**Field naming conventions.** Worldscope uses two parallel naming schemes:

- **Numeric "WS item" codes** (5-digit), e.g. `01001` = Net Sales or Revenues, `01051` = Cost of Goods Sold, `02001` = Cash & Short-Term Investments, `03051` = Total Long-Term Debt, `04001` = Cash from Operations. Identifier "20003 / 20103 / 20203 / 20303 / 20403 / 20503 / 20603" fields hold linked security identifiers per listing (source: https://www.tilburguniversity.edu/sites/default/files/download/WorldScopeDatatypeDefinitionsGuide_2.pdf, http://www.alacra.com/alacra/help/wscope_definitions.pdf).
- **Mnemonic `WS.*` datatypes** used in Datastream / Workspace / DSWS APIs, e.g. `WS.NetSales`, `WS.EPSReportDate`, `WS.EarningsReportFrequency`, `WS.OperatingIncome` (source: https://datateamoftheeur.wordpress.com/category/worldscope/).

**Coverage breadth.** LSEG marketing: 104,000+ companies in 120+ countries, 99% of global market cap, 30+ developed and emerging markets fully standardized, history back to 1980 with statistically significant coverage from 1985 forward. WRDS/library guides: ~31,000 active + ~9,000 inactive companies and 1,500+ data items (source: https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals, https://bizlib247.wordpress.com/2013/04/11/worldscope-coverage-and-data-definitions/).

**Templates / industry-specific items.** Banks, insurance, and other financials carry parallel item ranges (e.g., insurance claims/reserves items in the `06xxx`–`07xxx` range, banking items in `04xxx`–`05xxx`) so that comparable cross-industry queries must explicitly align on the per-template item codes (source: https://www.tilburguniversity.edu/sites/default/files/download/WorldScopeDatatypeDefinitionsGuide_2.pdf).

### 1.2 Datastream — point-in-time, vintages, restatements

Datastream is the long-history time-series engine that wraps Worldscope, I/B/E/S, plus economic series and market data into a unified cross-asset time-series store. It is sold within LSEG Workspace and as a dedicated DSWS (Datastream Web Service) API (source: https://wrds-www.wharton.upenn.edu/documents/1492/Thomson_Refinitiv_Datastream.pdf).

**Point-in-time (PIT) model.** Refinitiv sells "Point-In-Time" as a separate content layer that "provides access to original and restated values as they become available, with original data never being overwritten" — i.e., bitemporal storage of (period, asof) pairs. The PIT product covers fundamentals (Worldscope) and consensus estimates (I/B/E/S), and is positioned for backtesting and restatement-aware research (source: https://solutions.refinitiv.com/point-in-time, https://www.refinitiv.com/en/products/datastream-macroeconomic-analysis).

**Vintage retrieval.** In Datastream / DSWS, a user can request both first-reported (or "frozen") and current values; in Excel/DFO and DSWS the convention is to use the datatype with a vintage suffix or the `~AA=` argument forms documented in the DFO User Guide (source: https://fmc.refinitiv.com/clientFacing/pdf/DFO_User_Guide.pdf, https://community.developers.refinitiv.com/questions/70294/how-to-get-a-point-in-time-vintage-data-from-a-for.html). [unverified — exact symbol is documented per series]

**Macroeconomic vintages.** For macro series, Datastream exposes original-and-revised vintages for the major statistical series (BEA, OECD, etc.), which is an important dimension for backtesting macro-overlay equity strategies (source: https://www.refinitiv.com/en/products/datastream-macroeconomic-analysis).

### 1.3 I/B/E/S Estimates — detail vs summary, broker IDs, item types

I/B/E/S (Institutional Brokers' Estimate System) is the canonical analyst-estimates dataset, born in 1976 at Lynch, Jones & Ryan and now operated by LSEG. It is the dataset most academic estimate-revision papers reference (source: https://en.wikipedia.org/wiki/Institutional_Brokers%27_Estimate_System, https://www.lseg.com/en/data-analytics/financial-data/company-data/ibes-estimates).

**File structure.** Five files split across detail and summary branches (source: https://www.library.kent.edu/files/IBES_GuideUS.pdf, https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-ibes-demo/):

- Detail History — Estimates (analyst-by-analyst forecasts)
- Detail History — Actuals (companies' reported actuals on the I/B/E/S basis)
- Detail History — Excluded Estimates (forecasts excluded from consensus due to data-quality flags)
- Summary History — Summary Statistics (mean, median, high, low, std-dev, # estimates per period)
- Summary History — Actuals, Pricing & Ancillary

**Detail file primary key / sort order.** `(I/B/E/S Ticker, Fiscal Period Indicator, Broker Code [ESTIMATOR], Estimate Date)`. Analyst code (`ANALYS`) identifies the individual analyst within a broker (source: https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf).

**Item types ("measure").** 20+ forecast measures including EPS (canonical), Sales/Revenue, EBITDA, Pre-Tax Profit, Net Income, Cash Flow, Dividend per Share, Long-Term Growth (LTG), Price Targets, Recommendations (source: https://www.lseg.com/en/data-analytics/financial-data/company-data/ibes-estimates). Period indicators map FY1, FY2…, Q1, Q2… to a period number anchored by the company's fiscal year-end so cross-company comparisons don't require fiscal alignment (source: https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf).

**Adjusted vs unadjusted estimates.** Two parallel feeds: unadjusted (the value as the analyst originally entered it) and adjusted (back-adjusted for splits and stock dividends). This is critical for PIT replay; the WRDS "Note on IBES Unadjusted Data" warns researchers that consensus build-ups must be done on unadjusted values to avoid look-ahead from later split adjustments (source: https://wrds-www.wharton.upenn.edu/documents/5/A_Note_on_IBES_Unadjusted_Data_pdf.pdf).

**Broker / analyst identifier rebase (Oct 18, 2018).** LSEG rotated identifiers — ~13.8% of broker IDs and ~30.7% of analyst IDs were reassigned. Any historical research that joins on raw broker/analyst codes must apply the LSEG-published mapping table to bridge pre-/post-2018 (source: https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf).

### 1.4 Coverage and history depth (combined)

- Worldscope: 104K companies, 120+ countries, 1980/1985 onward (source: https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals).
- I/B/E/S: 22,000+ companies in 100+ markets globally, U.S. history from 1976 forward and international from ~1987 (source: https://www.lseg.com/en/data-analytics/financial-data/company-data/ibes-estimates). [unverified — start dates per geography]
- Datastream: extends time-series back to the 1960s for the major U.S./UK indices and a much larger universe of macro series (source: https://wrds-www.wharton.upenn.edu/documents/1492/Thomson_Refinitiv_Datastream.pdf).

### 1.5 Delivery channels

- **LSEG Workspace** (formerly Refinitiv Eikon, formerly Thomson Reuters Eikon) — the desktop terminal product. Native Excel/Python/R add-ins; bundles Datastream when subscribed (source: https://en.wikipedia.org/wiki/Eikon, https://libraries.wm.edu/databases/lseg-workspace-previously-known-refinitiv-eikon).
- **Refinitiv Data Platform (RDP) APIs** — REST-based platform exposing pricing, ESG, news, research, fundamentals and reference data; supports request-response, alert (async push), bulk, and streaming delivery patterns. AppKey-based auth issued through Workspace or AppKeyGenerator (source: https://developers.lseg.com/en/api-catalog/refinitiv-data-platform/refinitiv-data-platform-apis, https://developers.lseg.com/content/dam/devportal/api-families/refinitiv-data-platform/refinitiv-data-platform-apis/documentation/rdp_api_getting_started_guide.pdf).
- **DataScope Select / Tick History** — bulk and historical (tick / time-and-sales / venue-PCAP) delivery, deployed on AWS shared-storage and GCP/BigQuery; "S3-Direct" lets clients pull via Amazon S3 without transiting Refinitiv's network for materially better throughput on medium-to-large extracts. RTH dates back to 1996 (source: https://www.refinitiv.com/en/financial-data/market-data/tick-history, https://developers.lseg.com/en/article-catalog/article/boost-tick-history-downloads-with-aws, https://developers.lseg.com/en/article-catalog/article/big-data-tick-history-google-bigquery, https://aws.amazon.com/marketplace/pp/prodview-yi3aovwrufwua).
- **Real-time feeds (RDF-D / RTDS / Elektron)** — institutional managed data feeds for low-latency consumers. [unverified — current product naming]
- **DSWS (Datastream Web Service)** — programmatic access to the Datastream time-series store (source: https://fmc.refinitiv.com/clientFacing/pdf/DFO_User_Guide.pdf).

---

## 2. Refinitiv identifier / symbology

### 2.1 PermID (open)

- **What it is.** PermID is LSEG's permanent, open identifier for organizations, securities, instruments, quotes, individuals, and other entities. PermIDs do not encode meaning — attributes about the entity are looked up via the PermID API (source: https://permid.org/, https://developers.lseg.com/content/dam/devportal/api-families/open-permid/permid-entity-search/documentation/permid-apis-user-guide-apr-2020.pdf).
- **Coverage.** Per LSEG's own developer docs: ~13M organizations, ~550K equity instruments, ~3M equity quotes (source: https://github.com/Refinitiv-API-Samples/Article.OpenPermID.Python.APIs/blob/master/help.md).
- **APIs.** REST endpoints for Entity Search, Entity Lookup, Record Matching (POST a CSV of records to bulk-resolve), and Intelligent Tagging (NLP entity extraction) (source: https://developers.lseg.com/en/api-catalog/open-perm-id/permid-entity-search).
- **License.** Free and open. This is one of the few "free LSEG content" hooks competitors can build on, comparable to OpenFIGI on the Bloomberg side.

### 2.2 RIC (Refinitiv Instrument Code) — closed, licensed

- Ticker-style code identifying a security on a specific venue: `<root>.<exchange-code>`, e.g. `IBM.N` (NYSE), `MSFT.O` (NASDAQ), `VOD.L` (LSE), `7203.T` (Tokyo) (source: https://en.wikipedia.org/wiki/Refinitiv_Identification_Code, https://www.usek.edu.lb/Content/Assets/20240205WorkspaceWAinstrumentCode.pdf).
- Heavy semantic encoding for derivatives (continuation chains, expiry months, etc.).
- **Licensing.** RICs were originally proprietary; the European Commission's 2012 antitrust settlement forced Thomson Reuters to allow customers to use RICs to map to alternative providers' instrument codes — i.e., RICs are licensed but the *mapping rights* were freed (source: https://en.wikipedia.org/wiki/Refinitiv_Identification_Code).

### 2.3 TRBC — open via PermID

- Five-level hierarchy: 10 Economic Sectors → 28 Business Sectors → 54 Industry Groups → 136 Industries → 837 Activities (source: https://www.lseg.com/en/data-analytics/financial-data/indices/trbc-business-classification, https://en.wikipedia.org/wiki/The_Refinitiv_Business_Classification, https://www.equidam.com/resources/trbc-fact-sheet.pdf).
- Market-based methodology: companies are classified by the markets they serve, not by raw product taxonomy.
- **Open access.** TRBC codes are exposed via the PermID open APIs alongside organization metadata, which is meaningful for an open-source competitor — TRBC can be used as a free GICS-equivalent without a Refinitiv data contract.

### 2.4 PermID ↔ GLEIF LEI

- PermIDs and LEIs are not 1:1. An organization has exactly one PermID, but may have many LEIs (per legal entity / subsidiary), and conversely two PermIDs occasionally end up sharing a single LEI when the legal-entity hierarchy doesn't line up cleanly with the PermID's economic-entity definition (source: https://community.developers.refinitiv.com/questions/105553/how-is-it-possible-two-permids-have-the-same-assoc.html).
- The PermID Entity Search and Record Matching APIs accept LEI as an input field and will resolve to PermID; this is the mainstream cross-walk path (source: https://developers.lseg.com/en/api-catalog/open-perm-id/permid-entity-search).
- GLEIF runs a mapping certification program for ISIN, BIC, MIC, etc., but PermID is *not* itself a GLEIF-certified mapping (LSEG self-publishes this linkage) (source: https://www.gleif.org/en/lei-data/lei-mapping).

---

## 3. Refinitiv data collection methodology

### 3.1 Worldscope capture pipeline

- **Original model.** Human analyst capture from primary source documents (annual reports, 20-F / 10-K filings, prospectuses, news clippings) into per-template global data forms; a single team standardizes line-item mapping for cross-country comparability. This is the "Wright/Disclosure" heritage that makes Worldscope distinctive vs. Compustat (which historically did much the same in the U.S. only) (source: https://www.tilburguniversity.edu/sites/default/files/download/WorldScopeDatatypeDefinitionsGuide_2.pdf).
- **Automation evolution.** [unverified — exact dates] LSEG has steadily shifted toward semi-automated extraction (XBRL ingestion for SEC filers, structured filings in ESMA / TDnet / SEDAR, internal ML-assisted classification) layered on top of the historical analyst-capture spine.
- **Restatement handling.** Worldscope keeps both as-first-reported and as-currently-restated values; the PIT product layer is what surfaces the asof axis. Within Datastream the default request returns latest-restated unless a vintage qualifier is supplied (source: https://solutions.refinitiv.com/point-in-time, https://community.developers.refinitiv.com/questions/70294/how-to-get-a-point-in-time-vintage-data-from-a-for.html).

### 3.2 I/B/E/S contribution model

- **Broker contribution.** Sell-side brokers submit forecasts directly to LSEG via a contribution interface; LSEG normalizes them onto the I/B/E/S template (e.g., picking GAAP vs. operating EPS per the broker's stated basis), and standardizes to a "company-as-defined-by-broker" basis so consensus aggregates apples-to-apples (source: https://www.lseg.com/en/data-analytics/financial-data/company-data/ibes-estimates, https://www.library.kent.edu/files/IBES_GuideUS.pdf).
- **Excluded-estimates file.** Forecasts that fall outside data-quality bands (stale, off-basis, off-period, errored) are excluded from the consensus but retained in the Excluded Estimates file — useful for academic robustness checks and for any platform that wants to show "raw vs scrubbed" estimates (source: https://www.library.kent.edu/files/IBES_GuideUS.pdf).
- **Broker/analyst rebase 2018.** As above (§1.3), the 2018 ID rotation is the defining recent-history methodology event for IBES users.

### 3.3 QA and restatement handling

- Worldscope and IBES both ship "as-first-reported" alongside "current" values via the PIT layer; restatement events are surfaced in the data as new vintages without overwriting prior vintages (source: https://solutions.refinitiv.com/point-in-time).
- IBES additionally publishes actual values on the IBES basis (operating EPS) and on the GAAP basis to allow estimate-vs-actual comparisons against a like-for-like number (source: https://www.lseg.com/en/data-analytics/financial-data/company-data/ibes-estimates).

---

## 4. Bloomberg fundamentals data model

### 4.1 Field naming conventions

Bloomberg fields use uppercase snake-case mnemonics that mostly originated as 80s-era Bloomberg Terminal mnemonics (source: https://bautheac.github.io/BBGsymbols/, https://www.wu.ac.at/fileadmin/wu/s/library/databases_info_image/Bloomberg_BQL_Fundamentals_FactSheet.pdf):

- `SALES_REV_TURN` — adjusted revenue (DT id `IS010` per BBGsymbols catalog).
- `IS_INC_BEF_XO_ITEM` — income before extraordinary items.
- `NORMALIZED_INCOME` / `IS_NORMALIZED_INCOME` — net income normalized for one-time / non-recurring items.
- `EBITDA`, `EBIT`, `NET_INCOME`, `IS_OPER_INC`, `BS_TOT_ASSET`, `BS_LT_BORROW`, `CF_CASH_FROM_OPER`, etc. follow a `<statement-prefix>_<line-item>` template (`IS_*`, `BS_*`, `CF_*`).
- Estimate fields prefix `BEST_*` (Bloomberg Estimate), e.g. `BEST_EPS`, `BEST_SALES`, `BEST_EBITDA`, `BEST_TARGET_PRICE`.

[unverified — exact `IS_INC_BEF_XO_ITEM` definition wording]

### 4.2 Standardized vs as-reported toggle

- Bloomberg ships both "as-reported" (matching the filing) and standardized / "Adjusted" views for actuals, consensus estimates, and company guidance, all available LTM and per period.
- Toggle is via the `FA_ADJUSTED` (Fundamental Analysis Adjusted Override, DT id `DT094`) field at request time — `Y` for adjusted, `N` for GAAP — or globally via the FA Defaults user preference (source: https://data.bloomberglp.com/professional/sites/10/189913_CDS_REF_Fundamentals_SFCT_DIG.pdf, https://bautheac.github.io/BBGsymbols/).
- Bloomberg also exposes a "comparable" basis that pre-applies industry-template normalization (e.g., insurance-industry template differs from industrial), conceptually similar to Worldscope's per-template item codes.

### 4.3 BEst — Bloomberg Estimates

- Aggregated broker-contributed forecasts; users can pull on consensus level (mean / median / high / low / std-dev / # estimates) or analyst-detail level via `EEB <GO>` (Estimates Consensus Detail) (source: https://library.wu.ac.at/bib/fit4research/wp-content/uploads/2024/02/Forecasts_manuals_Bloomberg.pdf, https://faq.library.upenn.edu/business/faq/45772).
- Coverage breadth comparable to IBES; the typical industry view is that Bloomberg BEst and Refinitiv IBES are the two leading consensus datasets, with Bloomberg being more widely used buy-side and IBES more widely used in academic finance because of the WRDS distribution (source: https://gsb-research-help.stanford.edu/library/faq/297570).
- BEst contributes to terminal valuation analytics (DDM, multiples, scenario tools); detail-level estimates power EEB and ANR-style analyst-recommendations products (source: https://assets.bbhub.io/professional/sites/10/Bloomberg-US-Analyst-Recommendations-Index-Methodology.pdf).

### 4.4 Period types

- Bloomberg supports actual periods (`A` annual, `Q` quarter, `S` semi, `LTM`), period-over-period growth (`PCT_CHG`, `YOY_*`), trailing windows, and forward-period estimates (`FY1`, `FY2`, `FQ1`, etc.).
- Point-in-time access uses Bloomberg's COFI (Company Financials, Estimates and Pricing Point-in-Time) product, which delivers as-first-published and revised vintages on a (period, asof) grain — the Bloomberg analog to Refinitiv PIT (source: https://www.bloomberg.com/professional/products/data/enterprise-catalog/cofi/).

### 4.5 BICS hierarchy

- Bloomberg Industry Classification System: hierarchical revenue-driven classification, every company classified to at least Level 4 and as deep as Level 7 (source: https://assets.bbhub.io/professional/sites/10/BICS-2024-Changes.pdf, https://en.wikipedia.org/wiki/BICS).
- Per Bloomberg's own equity-indices methodology, Level 1 has 11 sectors and the full system reaches up to ~2,294 unique buckets across 7 levels (source: https://www.conseq.cz/getmedia/475278c1-bc71-4ca5-95ac-c9d5a1a7d014/Bloomberg-Global-Equity-Indices-Methodology-2312.pdf.aspx, https://www.thegoldensource.com/bloomberg-and-industry-classifications/).
- Methodology: Bloomberg analysts assign primary classification by primary revenue source (with operating income and assets as secondary tiebreakers) (source: https://data.bloomberglp.com/professional/sites/10/Classification-Data-Fact-Sheet.pdf).
- BICS is **proprietary** — there is no open BICS feed, in contrast to TRBC which is open via PermID. This is a meaningful product gap an open competitor can target.

### 4.6 Coverage breadth

- Bloomberg's enterprise data catalog cites 70M+ securities and 40,000+ data fields across reference, ESG, pricing, risk, fundamentals, estimates and history (source: https://professional.bloomberg.com/products/data/data-management/data-license/, https://www.bloomberg.com/company/press/bloomberg-announces-port-enterprise-data-delivery-to-snowflake-with-data-license-plus-dl/).
- ESG coverage: 15,000+ companies with 5,100+ ESG fields; GHG estimates extend the universe to 130,000+ companies (source: https://www.bloomberg.com/professional/insights/sustainable-finance/bloombergs-greenhouse-gas-emissions-estimates-model-a-summary-of-challenges-and-modeling-solutions/, https://professional.bloomberg.com/globalassets/professional/solutions/sustainable-finance/scores/bloomberg-esg-scores-methodology.pdf).

---

## 5. Bloomberg SPLC — supply chain analysis

### 5.1 What it is

`SPLC <GO>` on the Bloomberg Terminal renders, for any company in the universe, a graph of suppliers (left), customers (right), and competitors. Edges carry estimated dollar revenue / cost flow, percent of supplier revenue, and percent of customer COGS (source: https://libguides.brooklyn.cuny.edu/c.php?g=364715&p=6613630, https://libguides.nypl.org/c.php?g=1084166&p=8025921).

### 5.2 Schema (edges and node types)

Per the Cranfield / NYPL / Wharton library guides and the published Bloomberg case study (source: https://blogs.cranfield.ac.uk/library/supply-chain-bloomberg-workspace/, https://data.bloomberglp.com/professional/sites/10/233552_CDS_REF_SupplyChain_CASE_DIG-2.pdf):

- **Node types.** Public companies, private companies (cited as ~100K+ in the network), and a smaller set of competitor / peer nodes.
- **Edge types.**
  - Supplier→Company (cost edge from the company's perspective)
  - Company→Customer (revenue edge from the company's perspective)
  - Reverse-disclosed counterparts (when a supplier discloses the customer rather than vice versa)
  - Competitor edges (peer set used for relative analytics)
- **Edge attributes.** Estimated revenue (USD), estimated cost (USD), percent of total revenue / COGS, geographic exposure of the edge, commodity exposure, sustainability tags, and confidence score.

### 5.3 Sourcing

- Public filings (10-K, 10-Q, 20-F, annual reports), reseller/channel disclosures, customer-concentration mandatory disclosures (US "10% customer" rule), proxy statements, and earnings call transcripts.
- News-derived edges via Bloomberg's NLP pipeline on its newswire.
- Bloomberg uses a proprietary algorithm to *estimate* the dollar value when companies disclose only percentages or only one side of the relationship, which is the differentiator vs. FactSet Revere (source: https://libguides.nypl.org/c.php?g=1084166&p=8025921).

### 5.4 Confidence scoring and refresh

- Each estimated edge dollar value carries a confidence indicator. [unverified — exact scale; library guides describe but do not publish the numeric scheme.]
- Refresh: rolling, with edges refreshed as new filings/news are ingested (continuous), and periodic reviews of edge validity. [unverified — formal refresh SLA not published.]

### 5.5 Coverage scale

- Bloomberg public coverage: ~23,000 public companies and ~96,000 private, with ~900,000 total relationships and ~200,000 *quantified* (dollar-valued) supplier-customer relationships (source: https://www.bloomberg.com/professional/solutions/corporations/supply-chain/, https://onlinelibrary.wiley.com/doi/full/10.1111/jscm.12294).

### 5.6 Versus FactSet Revere

| Dimension | Bloomberg SPLC | FactSet Revere |
|---|---|---|
| Total entities | ~119K (public + private) | ~25K public + selected subsidiaries |
| Edges | ~900K relationships, ~200K quantified | ~144K business relationships |
| Quantification | Yes, proprietary dollar estimates | Generally not (no monetary value modeling) |
| History | [unverified — depth varies by edge] | Back to 2003 |
| Bulk extract | Limited (Excel/API top-20 only on standard license) | Bulk dataset license available |
| Direct vs reverse edges | Both, integrated into one graph | Explicit "direct" vs "reverse" labels |

(source: https://www.library.hbs.edu/databases-cases-and-more/datasets/factset-revere-supply-chain-relationships, https://onlinelibrary.wiley.com/doi/full/10.1111/jscm.12294, https://lippincottlibrary.wordpress.com/2021/12/10/untangling-the-supply-chains-part-1/)

The recent academic methodological survey (Culot 2023, *J. Supply Chain Mgmt*) concluded Bloomberg and FactSet are the higher-quality vendors, with Bloomberg's monetary-value modeling being its single biggest differentiator and FactSet's bulk-data ergonomics being theirs. CompuStat's customer file and Mergent are noticeably weaker (source: https://onlinelibrary.wiley.com/doi/full/10.1111/jscm.12294).

---

## 6. Bloomberg delivery channels

### 6.1 Bloomberg Terminal

- Sui-generis terminal product. List ~$31,980/seat/yr single-seat; ~$28,320/seat/yr at multi-seat rates as of January 2025 after a 6.5% increase. Multi-seat discounts step from 5% (5–9 terminals) to 20% (50+) and custom enterprise pricing at 500+ seats (source: https://godeldiscount.com/blog/bloomberg-terminal-cost-2026, https://www.neugroup.com/bloomberg-terminals-how-much-more-youll-pay-next-year/, https://costbench.com/software/financial-data-terminals/bloomberg-terminal/).
- Typical contract: 2-year minimum, monthly invoicing.
- Includes terminal access, BEst, fundamentals, news, IB chat, BQL/BQNT compute, and the SPLC supply-chain graph.

### 6.2 BPipe (B-PIPE)

- Bloomberg's enterprise real-time market-data feed (the institutional alternative to subscribing per-seat). Launched on cloud in recent years.
- Pricing not published; sold per data-rights tier and entitlement (asset class × geography × consumer count). [unverified — list pricing is undisclosed]

### 6.3 Data License (DL) — bulk reference + fundamentals

- Bulk delivery of reference, pricing, fundamentals, ESG, regulatory, risk content. Covers 70M+ securities and 40K+ fields across the catalog (source: https://professional.bloomberg.com/products/data/data-management/data-license/).
- Traditional delivery is per-day file drops (SFTP) keyed off PRODUCT × FIELD × CUSIP/FIGI universe.

### 6.4 Data License Plus (DL+)

- Managed-service modernization of DL: aggregates licensed Bloomberg data + multi-vendor ESG into a single Unified Data Model.
- Now distributed as a **Snowflake Native App** (and via Snowflake Marketplace) that provisions a customer's DL subscriptions directly into Snowflake; PORT Enterprise portfolio analytics also delivered via the DL+/Snowflake integration (source: https://www.bloomberg.com/company/press/bloomberg-announces-port-enterprise-data-delivery-to-snowflake-with-data-license-plus-dl/, https://www.bloomberg.com/company/press/bloomberg-simplifies-data-management-with-new-snowflake-native-app-in-the-data-cloud/).
- BigQuery integration also available. [unverified — current GA status]

### 6.5 BQL / BQuant — programmatic

- **BQL** (Bloomberg Query Language): SQL-like declarative API for terminal-side compute. Supports `let()` clauses, screening, aggregation, point-in-time fields, and functions over universes; runs on Bloomberg's servers so download volume is limited to the result set (source: https://michael-mao.gitbook.io/bloomberg/bql/bloomberg-query-language-bql, https://guides.nyu.edu/bloombergguide/bloomberg-query-language-bql).
- **BQNT (BQuant)**: hosted JupyterLab + Python environment with BQL access; targeted at quants doing factor research, signal backtests, and scenario tooling (source: https://www.bloomberg.com/professional/products/bloomberg-terminal/research/bquant/, https://data.bloomberglp.com/professional/sites/10/489937_BBGT_BQUANT_Overview_MINI-1.pdf).
- Excel/Python desktop APIs (`blpapi`, BDP/BDH/BDS) round out the programmatic stack.

### 6.6 OpenFIGI (open)

- `openfigi.com` provides free identifier mapping (POST a list of ISINs/CUSIPs/tickers, get back FIGIs and Bloomberg composite tickers). Rate-limited but free for non-bulk use; useful as the open identifier hook on the Bloomberg side (source: https://www.openfigi.com/api/documentation, https://www.openfigi.com/about/overview).

---

## 7. Bloomberg identifier system

### 7.1 FIGI (open standard)

- 12-character alphanumeric: chars 1–2 = certified provider prefix (Bloomberg uses `BB`), char 3 = constant `G` (Global), chars 4–11 = randomly assigned consonants/digits (no vowels to avoid offensive words and to disambiguate from CUSIP), char 12 = mod-10 double-add-double check digit (source: https://en.wikipedia.org/wiki/Financial_Instrument_Global_Identifier, https://www.openfigi.com/assets/local/figi-allocation-rules.pdf).
- FIGI carries no semantic information — all attributes are looked up via the OpenFIGI API.

### 7.2 Three FIGI levels

- **Exchange / Venue FIGI** — unique per (instrument, listing venue).
- **Composite FIGI** — country/market-level, aggregates multiple venue listings within a single country.
- **Share Class FIGI** — global, aggregates composite FIGIs across countries for the same share class. Useful for global single-stock workflows (source: https://www.openfigi.com/about/overview, https://en.wikipedia.org/wiki/Financial_Instrument_Global_Identifier).

### 7.3 Bloomberg Global ID, ticker, composite ticker

- "Bloomberg Global ID" was renamed to FIGI in 2014 when the standard was adopted by the Object Management Group with Bloomberg as the registration authority and certified provider (source: https://www.bloomberg.com/company/press/whats-name-bloomberg-global-id-reborn-figi/).
- **Bloomberg ticker.** Format `<TICKER> <EXCH> <YELLOW-KEY>`, e.g. `IBM US Equity`, `VOD LN Equity`, `7203 JT Equity`. The exchange code is a 2-letter Bloomberg pricing-source code.
- **Composite ticker.** `<TICKER> US Equity` style with `US` (or other country composite) routes to the country-composite FIGI rather than a specific listing.
- Yellow keys: `Equity`, `Govt`, `Corp`, `Mtge`, `M-Mkt`, `Muni`, `Pfd`, `Comdty`, `Index`, `Curncy`, `Client`. Selects the "asset class" search context.

### 7.4 FIGI ↔ ISIN / CUSIP

- OpenFIGI's mapping API accepts ISIN, CUSIP, SEDOL, WKN, etc., and returns the matching FIGI(s) (multiple if asking at the venue level for a multi-listed security).
- Composite FIGI is approximately 1:1 with `(ISIN, country)`; share-class FIGI is closer to 1:1 with ISIN globally for equities, though share-class consolidation rules can diverge (source: https://www.openfigi.com/api/documentation).

---

## 8. Pricing signals (gathered ranges)

| Product | Approx. price | Source |
|---|---|---|
| Bloomberg Terminal — single seat | $31,980/yr (2025, +6.5% YoY) | https://godeldiscount.com/blog/bloomberg-terminal-cost-2026 |
| Bloomberg Terminal — multi-seat | $28,320/yr/seat | https://godeldiscount.com/blog/bloomberg-terminal-cost-2026 |
| Bloomberg Terminal — 50+ seats | ~$22,660/yr/seat (~20% off) | https://godeldiscount.com/blog/bloomberg-terminal-cost-2026 |
| Bloomberg Terminal — Year 1 TCO | ~$34K–$35.5K with hardware/setup | https://godeldiscount.com/blog/bloomberg-terminal-cost-2026 |
| BPipe | Custom, undisclosed (data-rights × consumer count) [unverified] | https://professional.bloomberg.com/products/data/data-management/data-license/ |
| Data License (DL) | Custom per content pack [unverified] | https://professional.bloomberg.com/products/data/data-management/data-license/ |
| Data License Plus (Snowflake) | Custom; on top of DL subscription [unverified] | https://www.bloomberg.com/company/press/bloomberg-announces-port-enterprise-data-delivery-to-snowflake-with-data-license-plus-dl/ |
| LSEG Workspace base license | $1,500–$3,000/user/month [unverified third-party] | https://www.vendr.com/buyer-guides/refinitiv |
| LSEG Workspace data add-ons | $500–$2,000+/user/month (asset-class packs) | https://www.vendr.com/buyer-guides/refinitiv |
| LSEG Workspace specialty content | $200–$1,000+/user/month (premium research, fundamentals, alt-data) | https://www.vendr.com/buyer-guides/refinitiv |
| Mid-market Workspace deployment | $150K–$400K/yr ACV (10–25 users) | https://www.vendr.com/buyer-guides/refinitiv |
| Large enterprise Workspace | $1M+/yr | https://www.vendr.com/buyer-guides/refinitiv |
| OpenFIGI | $0 (rate-limited) | https://www.openfigi.com/api/documentation |
| PermID | $0 | https://permid.org/ |

**Strategic read for ats-eqt.** The headline gap — "I want SPLC-style supply-chain intelligence and a BICS-equivalent classification but I am not paying $30K/seat or $1M/yr enterprise" — is real, validated by both vendor pricing and the academic-research community's documented preference for Bloomberg + FactSet over freely available alternatives. The two open hooks (PermID for organizations, OpenFIGI for instruments) are the natural identifier spine for an open-source competitor, since they remove the licensing friction that RIC and Bloomberg ticker carry.

---

## 9. Sources

### Refinitiv / LSEG — fundamentals and estimates
- LSEG Worldscope Fundamentals product page — https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals
- Worldscope Datatype Definitions Guide (Issue 6, 2007) — https://www.tilburguniversity.edu/sites/default/files/download/WorldScopeDatatypeDefinitionsGuide_2.pdf
- Worldscope File Specifications (June 2008) — https://libapp.lib.ncku.edu.tw/libref/handout/20110107_WorldscopeFileSpecificationsJune2008.pdf
- Alacra Worldscope definitions — http://www.alacra.com/alacra/help/wscope_definitions.pdf
- Erasmus EDSC Worldscope tips — https://datateamoftheeur.wordpress.com/category/worldscope/
- LSE Library — Worldscope coverage — https://bizlib247.wordpress.com/2013/04/11/worldscope-coverage-and-data-definitions/
- LSEG IBES Estimates product page — https://www.lseg.com/en/data-analytics/financial-data/company-data/ibes-estimates
- IBES Detail History US Guide — https://www.library.kent.edu/files/IBES_GuideUS.pdf
- WRDS / Tilburg Overview of IBES on WRDS — https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf
- WRDS Note on IBES Unadjusted Data — https://wrds-www.wharton.upenn.edu/documents/5/A_Note_on_IBES_Unadjusted_Data_pdf.pdf
- IBES on Datastream guide — https://manchester-uk.libanswers.com/loader?fid=10871&type=1&key=c5f6a61d6ce6662dbf91abaaa3c8a138

### Refinitiv / LSEG — Datastream, RDP, Tick History
- Refinitiv Point-In-Time product — https://solutions.refinitiv.com/point-in-time
- Datastream Macroeconomic Analysis — https://www.refinitiv.com/en/products/datastream-macroeconomic-analysis
- WRDS Datastream brochure — https://wrds-www.wharton.upenn.edu/documents/1492/Thomson_Refinitiv_Datastream.pdf
- Datastream for Office (DFO) User Guide — https://fmc.refinitiv.com/clientFacing/pdf/DFO_User_Guide.pdf
- LSEG developer Q&A — point-in-time vintage from forecast series — https://community.developers.refinitiv.com/questions/70294/how-to-get-a-point-in-time-vintage-data-from-a-for.html
- Refinitiv Tick History product page — https://www.refinitiv.com/en/financial-data/market-data/tick-history
- Tick History on AWS Marketplace — https://aws.amazon.com/marketplace/pp/prodview-yi3aovwrufwua
- Tick History downloads via AWS — https://developers.lseg.com/en/article-catalog/article/boost-tick-history-downloads-with-aws
- Tick History in Google BigQuery — https://developers.lseg.com/en/article-catalog/article/big-data-tick-history-google-bigquery
- Refinitiv Data Platform APIs catalog — https://developers.lseg.com/en/api-catalog/refinitiv-data-platform/refinitiv-data-platform-apis
- RDP Getting Started guide — https://developers.lseg.com/content/dam/devportal/api-families/refinitiv-data-platform/refinitiv-data-platform-apis/documentation/rdp_api_getting_started_guide.pdf

### PermID, RIC, TRBC
- PermID home — https://permid.org/
- PermID APIs User Guide (Apr 2020) — https://developers.lseg.com/content/dam/devportal/api-families/open-permid/permid-entity-search/documentation/permid-apis-user-guide-apr-2020.pdf
- PermID Entity Search API catalog — https://developers.lseg.com/en/api-catalog/open-perm-id/permid-entity-search
- LSEG community — multiple PermIDs sharing an LEI — https://community.developers.refinitiv.com/questions/105553/how-is-it-possible-two-permids-have-the-same-assoc.html
- GLEIF LEI mapping — https://www.gleif.org/en/lei-data/lei-mapping
- Wikipedia — Refinitiv Identification Code — https://en.wikipedia.org/wiki/Refinitiv_Identification_Code
- LSEG RIC Symbology Card (USEK PDF) — https://www.usek.edu.lb/Content/Assets/20240205WorkspaceWAinstrumentCode.pdf
- LSEG TRBC product page — https://www.lseg.com/en/data-analytics/financial-data/indices/trbc-business-classification
- TRBC fact sheet — https://www.equidam.com/resources/trbc-fact-sheet.pdf
- Wikipedia — The Refinitiv Business Classification — https://en.wikipedia.org/wiki/The_Refinitiv_Business_Classification

### Bloomberg — fundamentals, BEst, BICS, BQL
- BBGsymbols R package field catalog — https://bautheac.github.io/BBGsymbols/
- Bloomberg Fundamentals fact sheet — https://data.bloomberglp.com/professional/sites/10/189913_CDS_REF_Fundamentals_SFCT_DIG.pdf
- Bloomberg BQL Fundamentals fact sheet — https://www.wu.ac.at/fileadmin/wu/s/library/databases_info_image/Bloomberg_BQL_Fundamentals_FactSheet.pdf
- Bloomberg COFI (Company Financials, Estimates and Pricing PIT) — https://www.bloomberg.com/professional/products/data/enterprise-catalog/cofi/
- Bloomberg Forecasts Manual (WU Vienna) — https://library.wu.ac.at/bib/fit4research/wp-content/uploads/2024/02/Forecasts_manuals_Bloomberg.pdf
- Bloomberg US Analyst Recommendations Index Methodology — https://assets.bbhub.io/professional/sites/10/Bloomberg-US-Analyst-Recommendations-Index-Methodology.pdf
- BICS 2024 Changes — https://assets.bbhub.io/professional/sites/10/BICS-2024-Changes.pdf
- Bloomberg Equity Indices BICS Hierarchy Change Announcement — https://assets.bbhub.io/professional/sites/10/Bloomberg-Equity-Indices-BICS-Hierarchy-Change-Announcement.pdf
- Bloomberg Classification Data fact sheet — https://data.bloomberglp.com/professional/sites/10/Classification-Data-Fact-Sheet.pdf
- Bloomberg Global Equity Indices Methodology (Dec 2023) — https://www.conseq.cz/getmedia/475278c1-bc71-4ca5-95ac-c9d5a1a7d014/Bloomberg-Global-Equity-Indices-Methodology-2312.pdf.aspx
- GoldenSource — Bloomberg & Industry Classifications overview — https://www.thegoldensource.com/bloomberg-and-industry-classifications/
- BQL primer (Michael Mao) — https://michael-mao.gitbook.io/bloomberg/bql/bloomberg-query-language-bql
- NYU BQL guide — https://guides.nyu.edu/bloombergguide/bloomberg-query-language-bql
- BQuant Desktop product — https://www.bloomberg.com/professional/products/bloomberg-terminal/research/bquant/

### Bloomberg — SPLC supply chain
- Bloomberg Supply Chain solution page — https://www.bloomberg.com/professional/solutions/corporations/supply-chain/
- Bloomberg SPLC predictive modeling case study — https://data.bloomberglp.com/professional/sites/10/233552_CDS_REF_SupplyChain_CASE_DIG-2.pdf
- Cranfield library — Bloomberg/Workspace supply chain — https://blogs.cranfield.ac.uk/library/supply-chain-bloomberg-workspace/
- NYPL guide — Company Supply Chain Analysis — https://libguides.nypl.org/c.php?g=1084166&p=8025921
- Brooklyn College guide — Bloomberg supply chain — https://libguides.brooklyn.cuny.edu/c.php?g=364715&p=6613630
- Emory SPLC handout (2011) — https://libraries.emory.edu/sites/default/files/migrated-documents/db-pages/bloomberg-splc.pdf
- Culot 2023 — academic critique of supply-chain databases (J. Supply Chain Mgmt) — https://onlinelibrary.wiley.com/doi/full/10.1111/jscm.12294
- HBS Baker — FactSet Revere Supply Chain Relationships — https://www.library.hbs.edu/databases-cases-and-more/datasets/factset-revere-supply-chain-relationships
- Wharton Lippincott blog — "Untangling the (Supply) Chains" — https://lippincottlibrary.wordpress.com/2021/12/10/untangling-the-supply-chains-part-1/

### Bloomberg — delivery, Data License, ESG
- Bloomberg Data License product page — https://professional.bloomberg.com/products/data/data-management/data-license/
- DL+ Snowflake / PORT Enterprise press release — https://www.bloomberg.com/company/press/bloomberg-announces-port-enterprise-data-delivery-to-snowflake-with-data-license-plus-dl/
- DL+ Snowflake Native App press release — https://www.bloomberg.com/company/press/bloomberg-simplifies-data-management-with-new-snowflake-native-app-in-the-data-cloud/
- Bloomberg ESG Scores Methodology (Dec 2025) — https://professional.bloomberg.com/globalassets/professional/solutions/sustainable-finance/scores/bloomberg-esg-scores-methodology.pdf
- Bloomberg GHG Estimates model summary — https://www.bloomberg.com/professional/insights/sustainable-finance/bloombergs-greenhouse-gas-emissions-estimates-model-a-summary-of-challenges-and-modeling-solutions/
- Bloomberg ESG sustainability fact sheet — https://assets.bbhub.io/professional/sites/10/GHG.pdf

### Bloomberg — FIGI / OpenFIGI
- OpenFIGI overview — https://www.openfigi.com/about/overview
- OpenFIGI API documentation — https://www.openfigi.com/api/documentation
- FIGI Allocation Rules PDF — https://www.openfigi.com/assets/local/figi-allocation-rules.pdf
- Wikipedia — Financial Instrument Global Identifier — https://en.wikipedia.org/wiki/Financial_Instrument_Global_Identifier
- Bloomberg press — Bloomberg Global ID becomes FIGI — https://www.bloomberg.com/company/press/whats-name-bloomberg-global-id-reborn-figi/
- Bloomberg press — OpenFIGI request utility launch — https://www.bloomberg.com/company/press/bloomberg-launches-online-request-utility-and-new-mapping-tools-for-the-financial-instrument-global-identifier-figi/

### Pricing
- Bloomberg Terminal Cost 2026 (godeldiscount) — https://godeldiscount.com/blog/bloomberg-terminal-cost-2026
- Why Bloomberg Terminal is so expensive (godeldiscount) — https://godeldiscount.com/blog/why-is-bloomberg-terminal-so-expensive
- NeuGroup — Bloomberg Terminals 2025 price increase — https://www.neugroup.com/bloomberg-terminals-how-much-more-youll-pay-next-year/
- Costbench Bloomberg Terminal pricing — https://costbench.com/software/financial-data-terminals/bloomberg-terminal/
- Vendr — Refinitiv / LSEG pricing — https://www.vendr.com/buyer-guides/refinitiv
- Vendr — LSEG marketplace — https://www.vendr.com/marketplace/refinitiv
