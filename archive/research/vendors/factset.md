# FactSet — Competitive Profile for ats-eqt

**Research date:** 2026-05-09
**Subject:** FactSet Research Systems Inc. (NYSE/NASDAQ: FDS)
**Scope:** Equity fundamentals, estimates, supply-chain (Revere), ESG (Truvalue), symbology (FSYM, CGS/CUSIP), concordance, and delivery technology
**Goal:** Establish a concrete benchmark for an open-source-data competitor (`ats-eqt`) covering equity fundamentals + supply-chain graphs.

---

## 1. Company & Product Overview

### Corporate snapshot
- **Parent:** FactSet Research Systems Inc., publicly listed on NYSE and Nasdaq under ticker **FDS** (founded 1978, HQ Norwalk, CT).
- **FY2025 (ended Aug 31, 2025) revenue:** **$2,321.7 M**, up 5.4% YoY (source: https://investor.factset.com/news-releases/news-release-details/factset-reports-results-fourth-quarter-and-fiscal-2025).
- **Annual Subscription Value (ASV):** **$2,405.6 M** at Aug 31, 2025 (vs. $2,255.4 M YoY); organic ASV $2,370.9 M (+5.7%) (source: same release).
- **Geographic mix:** Three reporting segments — Americas, EMEA, APAC. APAC led ASV growth at 7%, Americas 6%, EMEA 4% (FY25 release).
- **Client mix:** Buy-side ~82% of organic ASV, sell-side ~18%; Wealth was the fastest-growing client segment at 10% growth, Partnerships & CGS at 8%.
- **Workforce:** Tens of thousands of employees globally including a very large Hyderabad content-operations centre (FactSet Systems India Pvt Ltd, established 2017 in Raidurg, Hyderabad — research / ESG / fundamentals / product analyst pipelines staffed there) (source: https://www.indiamart.com/company/6936534/aboutus.html, https://www.talentify.io/job/research-analyst-hyderabad-telangana-factset-r21167).

### Product portfolio at a glance
FactSet bundles three layers: (a) the **Workstation** (formerly the desktop terminal), (b) **Content & Technology Solutions (CTS)** — DataFeeds, APIs, Snowflake share, CGS — and (c) **Wealth/Portfolio Lifecycle Management** apps (Cobalt, IRN, Portware/EMS).

Core data products relevant to ats-eqt:
- **FactSet Fundamentals** — global financials, GAAP/IFRS standardised
- **FactSet Fundamentals Point-in-Time** — restatement-aware "as-was" view
- **FactSet Estimates (Consensus + PIT Consensus)** — 800+ broker contributors
- **FactSet Supply Chain Relationships** (formerly Revere) — global B2B graph
- **FactSet RBICS** — proprietary 6-level industry classification (replaces GICS for many quant shops)
- **FactSet Truvalue** — NLP-driven ESG scores aligned to SASB
- **FactSet Symbology / Entity API / Concordance API / ID Lookup**
- **CUSIP Global Services (CGS)** — acquired from S&P Global in March 2022 for **$1.925 B** (source: https://investor.factset.com/news-releases/news-release-details/factset-completes-acquisition-cusip-global-services). Manages CUSIP/CINS/ISIN issuance for ~50M instruments and ran ~$175M annual revenue at acquisition.
- **Open:FactSet Marketplace** — third-party / alt-data catalogue (satellite, sentiment, ESG, etc.)
- **StreetAccount** — premium real-time news (acquired 2012)

### Competitive differentiators they market
1. **Connected symbology** — every FactSet feed links via the FSYM permanent ID family, so fundamentals, estimates, ownership, supply chain, and ESG join cleanly without ETL.
2. **Point-in-time** discipline across Fundamentals AND Estimates — explicitly called out as a defence vs. lookahead bias.
3. **RBICS** — they pitch this as more granular and economically coherent than GICS/ICB.
4. **CGS ownership** post-2022 means they sit at the centre of issuance-time identifier creation.
5. **Open:FactSet Marketplace** — curated alt-data with FactSet doing the entity-mapping work for buyers (source: https://investor.factset.com/news-releases/news-release-details/factset-expands-its-data-offering-launch-data-marketplace/).

---

## 2. Equity Fundamentals Data Model

### Primary tables / feeds
The Fundamentals product splits into **Basic** and **Advanced** packages and a set of industry-specific extension tables. Schema names appear as `ff_v3.*` in FactSet-hosted SQL access (Snowflake share, WRDS) (source: https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html).

| Table / Feed | Scope | Approx. fields |
|---|---|---|
| **FF_BASIC** (`ff_v3.ff_basic_af`, `_qf`, `_saf`, `_ltm`, `_ytd`) | Top-level B/S, I/S, CF | ~130 |
| **FF_ADVANCED** (`ff_v3.ff_advanced_af` + variants) | Decomposition of FF_BASIC items into sub-components | ~500 |
| **FF_BASIC_DER** | Derived ratios (margins, growth, returns) on top of FF_BASIC | derived |
| **FF_INFOTECH** | Tech-industry KPIs (e.g. R&D-related) | extension |
| **FF_REIT** | REIT-specific items (FFO, AFFO, NOI, occupancy) | extension |
| **FF_BANK / FF_INS / FF_OFIN** | Industry templates: Bank, Insurance, Other Financial, plus default Commercial profile | extension |

(sources: http://famouswiki.pbworks.com/w/page/66716998/Background%20On%20FactSet%20Databases%20and%20Data%20Items, https://www.wiso.uni-hamburg.de/bibliothek/recherche/datenbanken/unternehmensdaten/factset-fundamentals.pdf, https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html)

### Field-level coverage
FactSet's **At-a-Glance** for Fundamentals DataFeed states **750+ data items** in the DataFeed across the standardised templates (source: https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed). The Point-in-Time variant exposes **400+ items** specifically tracked through restatement history (source: https://www.factset.com/marketplace/catalog/product/factset-fundamentals-point-in-time).

### Point-in-time vs as-reported vs restated
This is one of FactSet's strongest capabilities and a critical benchmark for ats-eqt:

- The PIT product tracks **the full lifecycle** of every data item: *estimate → preliminary → originally reported → restated*, combined into a single time series with per-row knowledge dates (source: https://www.factset.com/marketplace/catalog/product/factset-fundamentals-point-in-time).
- Each numeric carries a **source filing reference** and a **first-seen / last-modified** date so backtests can reconstruct the universe as of any historical perspective date.
- FactSet explicitly markets that **~half the universe sees data changes at any point in time**, and PIT is the only safe view for backtesting (source: same product page).
- Coverage was originally 4 countries (US, CA, AU, NZ) and has expanded to **20 countries** including 16 European markets, with **25+ years of PIT history** for the original four (source: same product page).

### Frequency
Fundamentals are delivered as Annual (`af`), Semi-Annual (`saf`), Quarterly (`qf`), LTM (`ltm`), YTD-Cal (`ytd_cal`), YTD-Fiscal (`ytd_fiscal`) — i.e. five+ frequency variants of each table (source: https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html).

### History depth
- Annual: from **1980** globally (https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed)
- Semi-annual: from **1994**
- Quarterly: from **1995**
- WRDS confirms **1962-present** for international FactSet Fundamentals (`factset_ff_int`) and **1963-present** for North America (`factset_ff_usc`) — so the underlying corpus is deeper than the marketed standardised history (source: https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/factset/).

### Coverage
- **86,000+ global companies** including inactive issuers (https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed)
- **115+ countries** of incorporation/listing
- **Industry-templated** standardisation across four profiles: Commercial, Bank, Insurance, Other Financial
- **GAAP↔IFRS reconciliation** built into the standardised model so fields are comparable cross-border
- ADRs and dual-listings handled via the FSYM regional ID layer (see §4)
- New IPOs: minimum **$25M gross proceeds** for inclusion; current period available "on listing day", earlier periods within five business days (https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed).

### Data acquisition pipeline
Document feed: 8-Ks, Business Wire, PRNewswire, stock exchanges, regulatory bodies, company websites — "documents generally available in seconds or hours" of publication; collection within minutes-to-hours; standardisation by Hyderabad-based content analysts following industry-specific templates (sources: https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed, https://www.talentify.io/job/research-analyst-hyderabad-telangana-factset-r21167).

---

## 3. Supply-Chain Data Model (Revere)

FactSet Supply Chain Relationships is the rebrand of **Revere Data**, acquired by FactSet in 2013. It is a directed, typed graph between corporate entities.

### Edge taxonomy
- **4 main edge categories:** customer, supplier, partner, competitor
- **13 sub-types** (e.g., distribution partner, manufacturing partner, licensee, JV partner, strategic alliance, etc.) — exact sub-type list isn't fully published outside the methodology guide, but the "13 key company relationship types" framing is consistent across product pages and AWS Marketplace listings (sources: https://aws.amazon.com/marketplace/pp/prodview-h6mqbgeckx2gk, https://www.factset.com/marketplace/catalog/product/factset-supply-chain-relationships).

Each edge carries attributes:
- direction (from/to FSYM entity ID)
- edge type + sub-type
- **relationship keywords** (the source phrase / context tag)
- **relevance ranking** (proprietary "strength" score)
- **revenue dependency** when disclosed (e.g. "Apple = 18% of supplier X revenue")
- **first-seen / last-confirmed dates** (so the graph can be reconstructed as-of)

### Sourcing methodology
- **Primary sources only** — annual filings (10-K, 20-F, equivalent), investor presentations, official press releases (https://aws.amazon.com/marketplace/pp/prodview-h6mqbgeckx2gk).
- Direct disclosure ("A says B is its customer") is recorded; **reverse-linked** disclosures populate the back-edge for the non-disclosing party — so even a company that doesn't publish its supply chain shows up as the inferred counterparty.
- Specialised analyst teams (Hyderabad + global) read filings and tag relationships against a controlled keyword vocabulary — this is **human-curated**, not pure NLP.
- Updated **weekly** (WRDS) (https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/factset/).

### Coverage scale
Best public numbers (somewhat conflicting across sources — flagged where so):

- ~**31,000 entities** globally per the AWS Marketplace breakdown:
  - North America: 6,500 (since 2003)
  - Asia: 16,500 (since 2013)
  - Europe: 5,500 (since 2011)
  - Latin America: 600 (since 2016)
  - Africa: 500 (since 2014)
  - Middle East: 1,000 (since 2014)
  - Pacific: 1,000 (since 2014)
  (source: https://aws.amazon.com/marketplace/pp/prodview-h6mqbgeckx2gk)
- A 2022 academic critique (Culot, JSCM 2023) cites **~270,000 active relationships** across **~25,000 public firms** (source: https://onlinelibrary.wiley.com/doi/10.1111/jscm.12294).
- Other FactSet marketing cites **>144,000 relationships** across **>25,000 public companies + select subs** with history to 2003 — **[partially-verified, some inconsistency between sources, may reflect different snapshots over time]**.
- WRDS "factset_revere_supply_chain" schema is reported at only ~4 GiB compressed — small enough to be a relationship-edge table, not deep historical revisions.

### Companion classification feeds
Revere also delivers two adjacent products that share the same entity universe:
- **FactSet Geographic Revenue** — revenue by country/region per company, history to 1992 (https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/factset/).
- **FactSet Industry Classification (RBICS feed)** — see §4 below; history to 1945.

### Refresh cadence
- Bulk feed: **weekly**
- API-based access (FactSet Supply Chain API): refreshed continuously as analysts confirm/add edges.

---

## 4. Identifier / Symbology System

This is FactSet's most strategic asset. After acquiring CGS in 2022 they own both the issuance-time identifier (CUSIP) and their own permanent symbology layered on top.

### FSYM hierarchy
FactSet's Permanent Security Identifier (FSYM) is hierarchical with three security-level tiers plus an entity tier (sources: https://developer.factset.com/api-catalog/symbology-api, https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html, https://assets.ctfassets.net/lmz2w5z92b9u/7INM5wpJ5u1bomIisoOoz2/beaad6e64bbbdc96f8996acc9c8a1b34/FactSet_Permanent_Security_Identifier.pdf):

| ID type | Suffix | What it identifies | Example use |
|---|---|---|---|
| **fsymEntityId** | (no suffix, e.g. `000C7F-E`) | The legal entity (issuer) | Joining ownership, supply chain, ESG, fundamentals' issuer-level fields |
| **fsymSecurityId** | `-S` | A security across all listings/regions | Top-level security record |
| **fsymRegionalId** | `-R` | One regional/currency series of a security | **Required** key for joining FF_* fundamentals tables |
| **fsymListingId** | `-L` | Specific exchange listing | Tick / pricing data joins |

Each FSYM ID is **permanent** through corporate actions: ticker changes, exchange relistings, mergers, spinoffs do not mint a new FSYM — FactSet maintains lineage tables to record the event.

### Cross-references maintained
The Symbology API and `factset_common` schema map FSYM to:
- **CUSIP / CINS** (issuance-time, FactSet-controlled post-CGS acquisition)
- **ISIN**
- **SEDOL**
- **Ticker** (current and historical)
- **LEI**
- **OpenFIGI / Bloomberg**
- **CRSP PERMNO** (via WRDS `factset_crsp_link`) (source: https://wrds-www.wharton.upenn.edu/documents/1366/Factset_CRSP_Linking_Overview.pdf)
- **IBES TICKER** for estimates linkage
- **SIC, NAICS** and FactSet's own RBICS

### Corporate-action handling
- M&A: surviving entity inherits target's history through linkage tables; FSYM-E is preserved on survivor; target's IDs marked inactive with effective date.
- Spinoff: new FSYM-E minted for spunoff entity, parent FSYM-E unchanged; FactSet emits a "linkage" record so historical research can follow either side.
- Ticker change / re-listing: same FSYM-S/-R, new FSYM-L, ticker history table updated.
- Dual listing / ADR: multiple FSYM-R rows under one FSYM-S; one of them is flagged as "primary" via the regional security record.

This is materially better than naive ticker-based time series: a backtest joined on FSYM-R is naturally robust to the corporate-action gauntlet that wrecks naive ticker pipelines.

### Entity API + Concordance API
- **Entity API** exposes the entity-master directly: relationships, hierarchy (parent/sub), addresses, identifiers, structure type (PubCo/PvtCo/Fund/Govt/etc.).
- **Concordance API** does fuzzy entity-name resolution: client supplies `(name, URL, country, optional 3rd-party ID)` and gets back ranked candidates with similarity scores. The matching algorithm uses **TF-IDF over character trigrams** of entity names — for example "factset" → trigrams `{"fac", "act", "cts", "tse", "set"}`. Up to 25 names per request, returns ≤20 candidates plus a "proposed match" (source: https://developer.factset.com/api-catalog/factset-concordance-api).

This is the workflow pattern ats-eqt should plan to replicate: an open trigram-similarity matcher over a permanent-ID master is a credible MVP for entity resolution.

---

## 5. Data-Collection & Quality Methodology

### Collection
- **Filings & disclosures**: Hyderabad-based fundamentals analyst pool ingests SEC EDGAR (10-K, 10-Q, 8-K, 20-F), exchange filings (LSE RNS, TSX, EU Storage Mechanism, JPX, etc.), company IR sites, press wires (Business Wire, PRNewswire).
- **Estimates**: ~90% from broker research reports (PDF / email / portal), ~10% from broker flat-file feeds with QA checks (https://insight.factset.com/resources/factset-consensus-estimates-datafeed).
- **Supply chain & RBICS**: human analysts read filings, classify against controlled vocab, encode in a Java-based internal tool with QA workflow (https://insight.factset.com/resources/factset-revere-business-industry-classifications-datafeed).
- **ESG (Truvalue)**: NLP pipeline ingests **150,000+ unstructured sources** in **30+ languages** including news, NGO reports, trade blogs, social — fully automated, scored against the **26 SASB material categories** (https://insight.factset.com/resources/at-a-glance-factset-truvalue-sasb-scores-datafeed).

### Normalization across GAAP / IFRS
- Industry-templated standardisation: every issuer assigned to one of `Commercial / Bank / Insurance / Other-Financial` profiles, then standardised to a normalized template within that profile.
- Each FF_ field has a **definition formula** in the public Data Dictionary (Marketplace Catalog → Resources → Data Item Definitions) so users can reproduce or audit each derived line.
- Reporting-currency captured separately; FX-translated fields available with method tagged.

### Restatement / errata handling
- Fundamentals: every restatement keeps both the original-as-reported row and the restated row with knowledge dates, surfaced via the PIT product. The non-PIT product collapses to "best-known"; PIT is the audit-trail view.
- Estimates: PIT consensus snapshots fix the consensus per local-midnight per company — analyst revisions after the snapshot don't leak into prior days. Excludes dilution adjustments, currency changes, and post-snapshot QA corrections by design (https://insight.factset.com/resources/at-a-glance-factset-estimates-point-in-time-consensus).

### QA pipeline
- "Hundreds of algorithmic quality control checks" on every data item (https://insight.factset.com/resources/factset-consensus-estimates-datafeed)
- Workflow monitoring, file consistency, integration checks, multi-asset cross-checks (FactSet India job descriptions corroborate this).
- The **Sharp Estimate algorithm** is a published proprietary method: detects clusters of broker revisions that move in the same direction within a short window and flags a "Sharp Event Date"; consensus computed from post-event analysts only — defends against stale estimates dragging the mean (https://insight.factset.com/resources/at-a-glance-factset-estimates-point-in-time-consensus).

### Data-ops headcount
Public LinkedIn / job-board signal indicates **multiple thousands** of content / research / ESG analysts in Hyderabad alone (FactSet Systems India), plus US/UK/EU specialised teams **[unverified — exact count not disclosed in 10-K]**. This is the real moat — the cost to replicate a 1,000+ analyst sweatshop is the principal barrier to entry for a serious fundamentals competitor.

---

## 6. Database / Delivery Technology

### Delivery formats
FactSet has converged on multiple parallel delivery rails (clients pick by stack):

1. **Snowflake Data Share** — direct share into the customer's Snowflake account; primary modern delivery; covered for Fundamentals, PIT Estimates, Geographic Revenue, Supply Chain, RBICS, Truvalue (sources: https://developer.factset.com/recipe-catalog/instant-standard-datafeeds-delivered-snowflake, https://go.factset.com/hubfs/Website/Resources%20Section/Brochures/factset-data-solutions-via-snowflake-brochure.pdf).
2. **Standard DataFeed** — historical legacy: zip files of pipe-delimited / CSV / fixed-width on FactSet SFTP, daily/weekly cadence.
3. **Bulk Data API** — programmatic listing of zip files for download (replacement for raw SFTP).
4. **Content API (REST)** — direct queryable access to FactSet-hosted data, returns JSON; per-product API surfaces (Fundamentals API, Estimates API, Symbology API, Entity API, Supply Chain API, RBICS API, Concordance API, ID Lookup API, StreetAccount News API).
5. **AWS Data Exchange** — Redshift datashare (e.g. Supply Chain Relationships) (https://aws.amazon.com/marketplace/pp/prodview-h6mqbgeckx2gk).
6. **Databricks Marketplace** — Delta share (https://marketplace.databricks.com/details/5172c774-1978-47f1-81b0-43c334c29cff/FactSet_FactSet-Supply-Chain-Relationships).
7. **Cobalt API & Data Delivery Services** — abstracted transport layer that targets Snowflake / S3 / Azure blob.

### Access surfaces
- **FactSet Workstation** — Windows/web desktop terminal (the legacy front-end).
- **FactSet for Excel** — sidecar plug-in with formula language (FQL — FactSet Query Language; FDS — FactSet Data Service — older formula syntax).
- **FactSet APIs** (developer.factset.com) — OAuth2 + Basic Auth options, official SDKs in Python / .NET / TypeScript / Ruby / Java (visible on PyPI / NuGet / npm — e.g. `fds.sdk.FactSetFundamentals`, `fds.sdk.FactSetConcordance`, `fds.sdk.StreetAccountNews`).
- **FactSet OnDemand / Web Services** — older XML/RPC interface still active for some products (FactSet OnDemand Web Services Reference Manual v2.0.2).

### Internal technology hints
- Snowflake is the strategic delivery platform — FactSet was an anchor partner on Snowflake Marketplace at launch in 2020 (https://www.businesswire.com/news/home/20200526005142/).
- The fundamentals/estimates analyst tooling appears to be a **Java-based proprietary system** (RBICS data guide explicitly mentions this; FactSet careers postings reference Java/Postgres/Snowflake stacks).
- Real-time pricing (Cobalt / Portware) implies a separate market-data infra **[unverified specifics]**.

---

## 7. Pricing & Licensing Signals

FactSet does not publish a rate card. Triangulating across third-party sources:

| Tier | Indicative annual cost / user | Notes |
|---|---|---|
| Workstation Basic | $4,000 – $12,000 | Equity + estimates + workstation UI |
| Workstation Standard | $12,000 – $25,000 | + advanced screening / portfolio tools |
| Workstation Premium | $25,000 – $50,000 | "Fully-loaded" with most premium content |
| **Enterprise / API / Datafeed** | "Contact sales" — typically **$100k – multi-$M / year** for raw feeds | Per-product licensing with bulk discounts |

(sources: https://costbench.com/software/financial-data-terminals/factset/, https://www.trustradius.com/products/factset/pricing, https://www.g2.com/products/factset-workstation/pricing)

### Licensing structure
- **Per-user / per-seat** for the Workstation (with usage tracking — multi-tab / shared-account abuse is monitored).
- **Per-product / per-feed** for DataFeeds: Fundamentals, Estimates, Supply Chain, RBICS, Truvalue all priced separately.
- **Enterprise multi-year deals** with 25-50% discounts at 50+ user / 100+ user tiers commonly reported.
- **Redistribution restrictions**: derived works are usually allowed for internal research; redistribution to clients requires a separate redistribution license (a major reason fintechs build on top instead of resell).

### Strategic pricing signal for ats-eqt
The fully-loaded Fundamentals + Estimates + Supply Chain + RBICS + ESG bundle for an institutional quant team commonly clears **$500K – $2M+ per year**. A credible open-data competitor that delivers 70-80% of the coverage with a clean PIT model could find serious traction in the long tail (boutique funds, family offices, academia, fintech start-ups) where FactSet's price tag is prohibitive.

---

## 8. Sources

### FactSet first-party
- [FY2025 Q4 Earnings Release](https://investor.factset.com/news-releases/news-release-details/factset-reports-results-fourth-quarter-and-fiscal-2025) — Revenue $2.32B, ASV $2.41B, segment growth.
- [FY2025 Annual Report (10-K)](https://investor.factset.com/static-files/50cb77f8-83b8-4d1d-8c61-cce0af84a86b) — full financial filing.
- [FactSet completes CGS acquisition (Mar 2022)](https://investor.factset.com/news-releases/news-release-details/factset-completes-acquisition-cusip-global-services) — $1.925B CUSIP acquisition.
- [FactSet to Acquire Truvalue Labs](https://investor.factset.com/news-releases/news-release-details/factset-enters-agreement-acquire-truvalue-labs) — ESG NLP acquisition.
- [FactSet Pricing landing page](https://www.factset.com/factset-pricing) — official "contact us" pricing posture.
- [FactSet Marketplace: Fundamentals](https://www.factset.com/marketplace/catalog/product/factset-fundamentals) — Fundamentals product.
- [FactSet Marketplace: Fundamentals Point-in-Time](https://www.factset.com/marketplace/catalog/product/factset-fundamentals-point-in-time) — PIT product.
- [FactSet Marketplace: Estimates Consensus](https://www.factset.com/marketplace/catalog/product/factset-estimates-consensus) — Estimates product.
- [FactSet Marketplace: Supply Chain Relationships](https://www.factset.com/marketplace/catalog/product/factset-supply-chain-relationships) — Revere supply-chain product.
- [FactSet Marketplace: RBICS Focus](https://www.factset.com/marketplace/catalog/product/factset-rbics) — RBICS product.
- [FactSet Marketplace: RBICS with Revenue](https://www.factset.com/marketplace/catalog/product/factset-rbics-with-revenue) — Revenue-tagged industry product.
- [FactSet Marketplace: Truvalue Scores & Spotlights](https://www.factset.com/marketplace/catalog/product/factset-truvalue-scores-and-spotlights) — ESG scoring product.
- [FactSet Marketplace: ID Lookup API](https://www.factset.com/marketplace/catalog/product/factset-id-lookup-api) — symbol resolution.
- [FactSet Marketplace: Workstation](https://www.factset.com/marketplace/catalog/product/factset-workstation) — terminal product.
- [FactSet Marketplace: StreetAccount](https://www.factset.com/marketplace/catalog/product/streetaccount) — premium news.
- [FactSet Insight: Fundamentals DataFeed at-a-glance](https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed) — 86k companies, 750+ items, history.
- [FactSet Insight: Consensus Estimates DataFeed at-a-glance](https://insight.factset.com/resources/factset-consensus-estimates-datafeed) — 800 contributors, 16k companies, KPIs.
- [FactSet Insight: Estimates PIT Consensus at-a-glance](https://insight.factset.com/resources/at-a-glance-factset-estimates-point-in-time-consensus) — Sharp Estimate algorithm.
- [FactSet Insight: RBICS DataFeed at-a-glance](https://insight.factset.com/resources/factset-revere-business-industry-classifications-datafeed) — 14×6 matrix, methodology.
- [FactSet Insight: Truvalue SASB Scores DataFeed at-a-glance](https://insight.factset.com/resources/at-a-glance-factset-truvalue-sasb-scores-datafeed) — 26 SASB categories, 6 score types.
- [FactSet brochure: ESG from Truvalue Labs](https://go.factset.com/hubfs/Website/Resources%20Section/Brochures/esg-data-and-analytics-from-truvalue-labs-brochure.pdf) — sources, NLP pipeline.
- [FactSet brochure: Data Solutions via Snowflake](https://go.factset.com/hubfs/Website/Resources%20Section/Brochures/factset-data-solutions-via-snowflake-brochure.pdf) — Snowflake delivery.
- [FactSet Permanent Security Identifier whitepaper (PDF, 2017)](https://assets.ctfassets.net/lmz2w5z92b9u/7INM5wpJ5u1bomIisoOoz2/beaad6e64bbbdc96f8996acc9c8a1b34/FactSet_Permanent_Security_Identifier.pdf) — FSYM hierarchy whitepaper.
- [FactSet Concordance Service Methodology PDF](https://assets.ctfassets.net/lmz2w5z92b9u/6PwLI8eGYVMHVKIURBUGrJ/42d73d962f3226ed9f1e985e957891aa/Methodology.pdf) — TF-IDF trigram matching.
- [FactSet RBICS Methodology Guide PDF](https://assets.ctfassets.net/lmz2w5z92b9u/67nHF3Io7Zg8Ka1eQSqWsi/73277f7a9bc6250c727c8625bdc55164/factset_rbics_methodology_guide.pdf) — full classification methodology.
- [FactSet OnDemand Web Services Reference Manual v2.0.2 PDF](https://go.factset.com/hubfs/Website/Statistical%20Package%20Integration/factset%20ondemand%20web%20services%20reference%20manual_2.0.pdf) — legacy XML API spec.
- [FactSet Open:FactSet Marketplace launch (investor release)](https://investor.factset.com/news-releases/news-release-details/factset-expands-its-data-offering-launch-data-marketplace/) — alt-data marketplace.

### FactSet Developer Portal (API specs)
- [Symbology API](https://developer.factset.com/api-catalog/symbology-api) — FSYM / ISIN / CUSIP / SEDOL conversion.
- [FactSet Entity API](https://developer.factset.com/api-catalog/factset-entity-api) — entity master.
- [FactSet Concordance API](https://developer.factset.com/api-catalog/factset-concordance-api) — fuzzy entity matching.
- [FactSet Fundamentals API](https://developer.factset.com/api-catalog/factset-fundamentals-api) — fundamentals REST surface.
- [FactSet Estimates API](https://developer.factset.com/api-catalog/factset-estimates-api) — estimates REST surface.
- [FactSet Supply Chain API](https://developer.factset.com/api-catalog/factset-supply-chain-api) — Revere REST surface.
- [FactSet RBICS API](https://developer.factset.com/api-catalog/factset-rbics-api) — RBICS REST surface.
- [Standard DataFeed API](https://developer.factset.com/api-catalog/standard-datafeed-api) — bulk-file listing.
- [Snowflake delivery recipe](https://developer.factset.com/recipe-catalog/instant-standard-datafeeds-delivered-snowflake) — Snowflake data-share guide.

### Third-party / academic / partner channels
- [WRDS: FactSet data vendor page](https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/factset/) — schema names, table sizes, history starting points.
- [WRDS: FactSet Data Print PDF](https://wrds-www.wharton.upenn.edu/documents/1149/FactSet_Data_on_WRDS_PRINT.pdf) — ditto.
- [WRDS: FactSet–CRSP Linking](https://wrds-www.wharton.upenn.edu/documents/1366/Factset_CRSP_Linking_Overview.pdf) — FSYM↔PERMNO linkage methodology.
- [Harvard Baker Library: FactSet Revere Supply Chain Relationships](https://www.library.hbs.edu/databases-cases-and-more/datasets/factset-revere-supply-chain-relationships) — 4 categories + 13 sub-types.
- [Quantopian docs: FactSet Fundamentals](https://www.quantopian.com/docs/data-reference/factset_fundamentals) — historical reference.
- [Exabel docs: FactSet Fundamentals](https://doc.exabel.com/dsl/data_signals/factset_fundamentals.html) — third-party integration view.
- [Exabel docs: FactSet Estimates](https://doc.exabel.com/dsl/data_signals/factset_estimates.html) — third-party Estimates view.
- [Exabel mapping companies & securities](https://help.exabel.com/docs/mapping-to-companies-securities) — FSYM mapping practice.
- [Hamburg University FactSet Fundamentals PDF guide](https://www.wiso.uni-hamburg.de/bibliothek/recherche/datenbanken/unternehmensdaten/factset-fundamentals.pdf) — academic-library how-to.
- [my1396 FactSet 101 personal-notes tutorial](https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html) — concrete `ff_v3` SQL examples and FSYM-R join requirement.
- [Famous Wiki: Background on FactSet Databases](http://famouswiki.pbworks.com/w/page/66716998/Background%20On%20FactSet%20Databases%20and%20Data%20Items) — older table reference.
- [AWS Marketplace: FactSet Supply Chain Relationships](https://aws.amazon.com/marketplace/pp/prodview-h6mqbgeckx2gk) — entity counts by region.
- [Databricks Marketplace: FactSet Supply Chain](https://marketplace.databricks.com/details/5172c774-1978-47f1-81b0-43c334c29cff/FactSet_FactSet-Supply-Chain-Relationships) — Delta-share availability.
- [AWS Marketplace: FactSet Estimates - Consensus](https://aws.amazon.com/marketplace/pp/prodview-2bmdcxxw7flla) — Estimates on AWS.
- [Snowflake announcement: FactSet on Snowflake Marketplace (May 2020)](https://www.businesswire.com/news/home/20200526005142/en/Snowflake-Announces-FactSet-Data-Now-Available-on-Snowflake-Data-Marketplace) — anchor partner.
- [Culot et al., JSCM 2023 — academic critique of supply-chain databases](https://onlinelibrary.wiley.com/doi/10.1111/jscm.12294) — independent verification of ~270k relationships / ~25k firms.
- [CostBench: FactSet pricing](https://costbench.com/software/financial-data-terminals/factset/) — third-party price triangulation.
- [TrustRadius: FactSet Workstation pricing](https://www.trustradius.com/products/factset/pricing) — same.
- [G2: FactSet Workstation pricing](https://www.g2.com/products/factset-workstation/pricing) — same.
- [FactSet Systems India profile (IndiaMart)](https://www.indiamart.com/company/6936534/aboutus.html) — Hyderabad operations centre.
- [PRNewswire / ABA joint statement on CGS](https://www.prnewswire.com/news-releases/cusip-global-services-and-american-bankers-association-joint-statement-on-factset-acquisition-of-cgs-from-sp-global-301450881.html) — CGS divestiture context.
- [Lippincott Library Datapoints: Untangling the Supply Chains pt.1](https://lippincottlibrary.wordpress.com/2021/12/10/untangling-the-supply-chains-part-1/) — academic-library walkthrough of Revere.
- [The Good Lobby Scorecard: Truvalue SASB Scores](https://www.thegoodlobby.eu/wp-content/uploads/2024/12/TGL-Scorecard-FactSet-Truvalue-SASB-Scores-DataFeed.pdf) — third-party ESG-product audit.

---

## Closing notes for ats-eqt design

A few engineering implications a competitor should internalise:

1. **PIT is non-negotiable.** A fundamentals product without point-in-time history is a research toy, not a backtest substrate. Plan the database schema around `(entity, item, frequency, period_end, knowledge_date)` from day zero.
2. **Entity master is the moat.** FactSet's permanent-ID family (FSYM) + CGS ownership is *the* lock-in. ats-eqt should anchor on **OpenFIGI + LEI + permanent open-source minted IDs** with a public crosswalk that cleanly handles M&A / spinoff lineage.
3. **RBICS is replaceable but expensive.** A 6-level open classification trained on filings + RBICS-style revenue tagging is a tractable but multi-quarter project. NAICS + GICS-mapping is a credible MVP fallback.
4. **Supply-chain graph is the most "scrapeable" piece.** ~25k public + select subs and ~270k edges is *small* by graph standards. Filing-NER + reverse-linking is well-trodden NLP. This is the highest-leverage open-data wedge.
5. **Truvalue-style ESG = pure NLP automation.** No human moat. Any competitor with a serious LLM pipeline can reproduce SASB-aligned scoring in a quarter.
6. **Estimates are the hardest piece to clone openly** — broker research is contractual / paywalled. Probable answer for ats-eqt: skip consensus estimates initially, lean on company guidance + retail-analyst aggregation (Estimize-style) as v1.
