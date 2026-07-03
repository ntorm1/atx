# ats-eqt — Reference Industry / Sector Classifications Dataset

**Status:** Research, v0.1
**Audience:** ats-eqt engineering team (entity master, classification layer); ats-core team consuming sector tags for screening / factor models
**Scope:** the industry / sector / business classification taxonomies used across the major vendors at code level — what each one looks like as raw bytes, who owns it, what it licences for, and how (and how badly) the cross-walks line up. Covers GICS, ICB, TRBC, BICS, RBICS, NAICS, SIC, ISIC, NACE, plus the niche/derived schemes (Fama-French, BEA, Wikidata) that an open competitor needs to know about.
**Last updated:** 2026-05-14

---

## 0. Executive summary

Industry classification is the *quietest* moat in financial data. Every quant pipeline, every factor model, every screener, every peer-group analysis silently depends on one of seven or eight named taxonomies, and the four that index providers actually price portfolios on — **GICS** (S&P + MSCI), **ICB** (LSEG/FTSE Russell), **TRBC** (LSEG), and **BICS** (Bloomberg) — are *all* closed-licensed commercial products. The only deep, open alternatives are **NAICS** (US Census Bureau, public domain), **SIC** (frozen at 1987 but still the SEC's de facto issuer tag), **ISIC** (UN), **NACE** (Eurostat), and the bottom-up academic schemes from Kenneth French.

The headline numbers for an ats-eqt design:

| Taxonomy | Levels | Leaf count | Code length | Vendor | Open? |
|---|---|---|---|---|---|
| GICS (v12, 2023-03-17) | 4 | 163 sub-industries | 8-digit numeric | S&P DJI + MSCI (joint) | No |
| ICB (v2019) | 4 | 173 subsectors | 8-digit numeric | LSEG / FTSE Russell | No |
| TRBC (2020 / 2012 rev.) | 5 | ~898 activities | 10-digit numeric | LSEG | **Yes, via PermID** |
| BICS (2024 update) | up to 7 | ~2.3k segments (est.) | variable numeric | Bloomberg | No |
| RBICS | 6 | ~1,500–1,600 sub-industries | numeric (matrix-keyed) | FactSet | No |
| NAICS (2022) | 5 + national | 1,057 six-digit codes | 2/3/4/5/6-digit | US Census Bureau | **Yes (public domain)** |
| SIC (frozen 1987) | 4 | ~1,000 four-digit codes | 4-digit | BLS (frozen) / SEC fork | **Yes (public domain)** |
| ISIC rev. 4 (rev. 5 pending) | 4 | ~419 classes | 1-4 alphanumeric | UN Statistics Division | **Yes (open)** |
| NACE rev. 2.1 (2025) | 4 | 615 classes | letter+digit | Eurostat / EU | **Yes (open)** |
| Fama-French (12/17/30/48/49) | 1 | 5–49 industries | SIC-derived | Academic (Tuck) | **Yes (free)** |

Five take-aways that drive the ats-eqt design:

1. **The four closed taxonomies are deep moats.** GICS / ICB / TRBC / BICS are jointly the basis of S&P 500, FTSE All-World, MSCI ACWI, Russell, NASDAQ, and Bloomberg-family indices. Without a licence, ats-eqt cannot publish "GICS Sector = Information Technology" downstream — only an *equivalent* derived bucket from a public taxonomy.
2. **TRBC is the one open hook with index-grade depth.** Because LSEG exposes TRBC codes via the free PermID API, it is the only 5-level vendor-grade taxonomy that an open competitor can rebadge and ship at zero licence cost. This is the single most important asymmetry in the classification market.
3. **The SEC has not migrated off SIC.** Every 10-K, 10-Q, 13F, and N-PORT filing on EDGAR carries an `assigned-sic` code from a 1987-frozen taxonomy. Any US public-equity pipeline that joins EDGAR data *must* still know SIC, regardless of how the downstream consumer prefers to view sectors.
4. **All cross-walks are many-to-many.** No two taxonomies bijectively map. The most-cited academic GICS↔NAICS crosswalk (Weingarden, Federal Reserve) maps 144 GICS sub-industries to 989 NAICS-6 codes — a M:N relation by construction (source: <https://sites.google.com/site/alisonweingarden/links/industries>).
5. **RBICS's multi-tag-with-revenue-weights model is the only one of the eight that supports diversified conglomerates faithfully.** Every other system forces a "primary classification" choice. ats-eqt's data model must support both single-primary and multi-weighted classifications from day one or it will be incompatible with RBICS-shaped downstream workflows.

---

## 1. Why classifications matter

Industry / sector tags are a load-bearing dimension in nearly every equity workflow:

- **Index construction.** S&P 500 sectors (GICS), MSCI ACWI (GICS), FTSE All-World (ICB), STOXX Europe 600 (ICB), MSCI World Sector Indices (GICS), Russell 1000 Growth/Value (ICB-derived). The float-adjusted market-cap rollups *are* the sector taxonomy.
- **Factor models.** Industry dummies are the canonical cross-sectional control in Fama-MacBeth and Barra-style risk models. Fama-French published *three* (FF12, FF17, FF30, FF48, FF49) variants because granularity choice materially changes anomaly t-stats (source: <https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_48_ind_port.html>).
- **Peer-group analysis.** Sell-side comparable-company analysis ("comps tables"), private-market multiples, M&A precedent screens — all keyed on sub-industry-level peer sets.
- **Regression covariates.** Industry-fixed effects in any panel regression of accounting / event-study returns are conventionally GICS-Industry-Group (4-digit) or NAICS-3 / NAICS-4. The choice is rarely robustness-checked, and it changes published results.
- **Regulatory disclosure.** EU CSRD / ESRS sector standards anchor on NACE rev. 2.1 (source: <https://efrag-website.azurewebsites.net/Assets/Download?assetUrl=%2Fsites%2Fwebpublishing%2FMeeting+Documents%2F2311080942079508%2F05-04+-+Sector+Classification+SEC+1+-+NACE+2-1+-+SR+TEG+240115.pdf>). SEC EDGAR review-team routing keys on SIC. ESMA EMIR uses MiFID issuer categories.
- **Concentration / risk overlays.** Sector concentration limits in UCITS / 40-Act funds; CFTC large-trader sector buckets; ESG sector-exposure scorecards.
- **Macro overlays.** BEA Industry Economic Accounts (NAICS-derived) for GDP-by-industry; Federal Reserve Industrial Production by NAICS sector; Eurostat short-term indicators by NACE.

In practice, *every* downstream consumer of ats-eqt sees a sector tag somewhere. The question is not whether to ship classifications, but how many, in what form, and with what redistribution posture.

---

## 2. Taxonomy stack matrix

A side-by-side of the nine canonical taxonomies plus the niche ones, on the dimensions that matter for ingestion and storage:

| Taxonomy | Vendor of record | Latest rev. | Levels | Sectors (L1) | L2 | L3 | L4 | L5+ | Code | Licence | Notable adopters |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **GICS** | S&P DJI + MSCI (joint) | v12, 2023-03-17 | 4 | 11 sectors | 25 industry groups | 74 industries | 163 sub-industries | — | 8-digit numeric | Closed; per-seat / per-deployment licence | S&P 500, MSCI ACWI, Russell, NASDAQ sectors |
| **ICB** | LSEG / FTSE Russell | v2019 (since 2019-07-01) | 4 | 11 industries | 20 supersectors | 45 sectors | 173 subsectors | — | 8-digit numeric (2/4/6/8) | Closed; per-product licence | FTSE All-World, STOXX, NASDAQ, JSE, Borsa Italiana |
| **TRBC** | LSEG | 2020 (incl. 2012 5-level extension) | 5 | 10 economic sectors | 33 business sectors | 62 industry groups | 154 industries | 898 activities | 2/4/6/8/10-digit numeric | **Open via PermID API** for codes; data licence for series | LSEG indices, Workspace, Datastream |
| **BICS** | Bloomberg | 2024 update (EOD 2024-06-07) | up to 7 | 11 L1 sectors | ~49 L2 industry groups [unverified] | ~196 L3 industries [unverified] | ~712 L4 sub-industries [unverified] | up to 2,294 unique L1-L7 buckets [unverified] | Variable-length numeric | Closed; Bloomberg subscriber-only | Bloomberg indices, BPipe, DL/DL+ |
| **RBICS** | FactSet (Revere) | rolling | 6 | 14 economies (12 anchor + 2 specialty) | 36 sectors | 105 subsectors | 316 industry groups | 759 industries / 1,603 sub-industries | Numeric matrix-keyed | Closed; FactSet subscriber-only | FactSet Workstation, Snowflake share, STOXX revenue-thematic indices |
| **NAICS** | US Census Bureau (joint w/ Statistics Canada + INEGI Mexico) | 2022 | 5 + national | 20 sectors (2-digit) | ~99 subsectors (3-digit) | ~311 industry groups (4-digit) | ~700 NAICS industries (5-digit) | 1,057 US national industries (6-digit) | 2/3/4/5/6-digit numeric | **Public domain** | US Economic Census, BLS CES, BEA, IRS |
| **SIC** | BLS / OMB (frozen 1987); SEC fork | 1987 (frozen) | 4 | 11 divisions (letters A-K) | ~83 major groups (2-digit) | ~416 industry groups (3-digit) | ~1,005 industries (4-digit) | — | 4-digit numeric | **Public domain** | SEC EDGAR `assigned-sic`, OSHA, CRSP `HSICCD/SICCD`, Compustat `sic/sich` |
| **ISIC** | UN Statistics Division | rev. 4 (rev. 5 endorsed 2023, pending) | 4 | 21 sections (letters A-U) | 88 divisions | 238 groups | 419 classes | — | letter + 2/3/4-digit numeric | **Open** | UN Comtrade, World Bank, OECD |
| **NACE** | Eurostat / EU Commission | rev. 2.1 (applies from 2025-01-01) | 4 | 22 sections (letters A-V) | 88 divisions | 272 groups | 615 classes | — | letter + 2/3/4-digit numeric | **Open (CC-BY-equivalent)** | Eurostat, EU CSRD/ESRS, national statistical offices |
| **Fama-French 48** | Tuck (Ken French) | rolling | 1 | 48 industries (49 with "Other") | — | — | — | — | SIC-derived | **Free (academic)** | Academic asset-pricing research |
| **BEA Industry Accounts** | US BEA | annual | ~3 | 21 NAICS-derived super-sectors | 71 detailed | — | — | — | NAICS-derived | **Public domain** | US GDP-by-industry |
| **Wikidata `industry` (P452)** | Wikimedia | continuous | flat (Q-IDs) | n/a | n/a | n/a | n/a | n/a | Q-ID | **CC0** | Open knowledge graph linkage |

Sources for the counts: GICS at <https://en.wikipedia.org/wiki/Global_Industry_Classification_Standard> and <https://classification.codes/classifications/industry/gics/>; ICB at <https://www.lseg.com/en/ftse-russell/industry-classification-benchmark-icb> and <https://classification.codes/classifications/industry/icb>; TRBC at <https://en.wikipedia.org/wiki/The_Refinitiv_Business_Classification>, <https://classification.codes/classifications/industry/trbc>, <https://www.equidam.com/resources/trbc-fact-sheet.pdf>; BICS at <https://www.thegoldensource.com/bloomberg-and-industry-classifications/>, <https://www.conseq.cz/getmedia/475278c1-bc71-4ca5-95ac-c9d5a1a7d014/Bloomberg-Global-Equity-Indices-Methodology-2312.pdf.aspx>; RBICS at <https://insight.factset.com/resources/factset-revere-business-industry-classifications-datafeed> and the RBICS Methodology Guide PDF; NAICS at <https://www.census.gov/naics/> and <https://siccode.com/page/structure-of-naics-codes>; SIC at <https://en.wikipedia.org/wiki/Standard_Industrial_Classification>; ISIC at <https://unstats.un.org/unsd/classifications/Econ/isic>; NACE at <https://ec.europa.eu/eurostat/web/nace>.

---

## Part A — Per-taxonomy deep dives

### A.1 GICS — Global Industry Classification Standard

- **Vendor of record:** Jointly owned by **S&P Dow Jones Indices** and **MSCI Inc.** Trademark `GICS` is jointly held by McGraw Hill Financial (S&P parent) and MSCI (source: <https://en.wikipedia.org/wiki/Global_Industry_Classification_Standard>). Distributed under the **GICS Direct** product (source: <https://www.spglobal.com/marketintelligence/en/documents/gics-direct-brochure.pdf>).
- **History:** Launched 1999 jointly by MSCI + S&P. Sector-level revisions: 2016 (Real Estate carved out of Financials into its own sector), 2018 (Telecommunication Services renamed Communication Services and absorbed media/entertainment from Consumer Discretionary plus interactive media from Information Technology). Most recent: **2023-03-17** (Version 12) — Industry / sub-industry level revisions, no new sectors. Notable 2023 changes:
  - Discontinued `Internet & Direct Marketing Retail` sub-industry; companies reclassified by nature of goods sold (apparel, food, etc.).
  - Merged `General Merchandise Stores` and `Department Stores` into new `Broadline Retail` sub-industry.
  - Discontinued `Data Processing & Outsourced Services`; transaction/payment-processing companies moved to a new `Transaction & Payment Processing Services` sub-industry under Financials; remaining BPO/IT-services companies moved to Industrials.
  - Discontinued `Trucking`, split into `Passenger Ground Transportation` + `Cargo Ground Transportation`.
  - (sources: <https://fwcook.com/revisions-to-global-industry-classification-standard-gics-codes-to-be-implemented-in-march-2023/>, <https://www.westendadvisors.com/wp-content/uploads/2023/03/GICS-Sector-Revisions-Update-PRES-202303.pdf>, <https://www.indexologyblog.com/2023/01/27/gics-changes-are-approaching/>)
- **Structure (post-2023):**

  | Level | Code digits | Count | Example |
  |---|---|---|---|
  | Sector | 2 | 11 | `20` = Industrials |
  | Industry Group | 4 | 25 | `2010` = Capital Goods |
  | Industry | 6 | 74 | `201020` = Construction & Engineering |
  | Sub-Industry | 8 | 163 | `25201020` = Apparel, Accessories & Luxury Goods; `20304010` = Rail Transportation |

  The 8-digit code is left-padded numeric so that the first 2 digits give the sector, first 4 the industry group, etc. The classification system pads codes with leading zeros and is designed to be Excel-formatting-resistant (i.e., codes are "20304010" not the number 20,304,010) (source: <https://classification.codes/classifications/industry/gics/>).

- **Licensing:** Closed; **GICS Direct** is a paid subscription priced by client type and size (source: <https://www.spglobal.com/marketintelligence/en/documents/gics-direct-brochure.pdf>). Redistribution is contractually restricted; using GICS to "verify or correct data, or any compilation of data or index" is prohibited without further licence (source: <https://www.msci.com/documents/10199/5973a128-47f0-4317-b083-716a10207b50>). Public-facing financial products typically require a separate index-license layer on top.
- **Vendor field appearances:**
  - Compustat: `gsector` (2-digit), `ggroup` (4), `gind` (6), `gsubind` (8) in the `company` table; historical view in `co_idesind`.
  - FactSet: `FF_GICS_SECTOR`, `FF_GICS_IND_GROUP`, `FF_GICS_INDUSTRY`, `FF_GICS_SUB_IND` (via FactSet Fundamentals + the FF_RBICS / FF_SYM bridge tables).
  - Bloomberg: `GICS_SECTOR_NAME`, `GICS_INDUSTRY_GROUP_NAME`, `GICS_INDUSTRY_NAME`, `GICS_SUB_INDUSTRY_NAME` (terminal mnemonics).
  - LSEG Workspace / Datastream: `WC07021` family, plus `TR.GICS*` mnemonics where licensed.

### A.2 ICB — Industry Classification Benchmark

- **Vendor of record:** **LSEG / FTSE Russell**. Jointly launched 2005 by FTSE + Dow Jones. Dow Jones divested its 50% stake in 2011 and developed its own (Dow Jones Industry Classification Benchmark) afterwards; FTSE became sole owner. Now operated by FTSE Russell, an LSEG business unit (source: <https://en.wikipedia.org/wiki/Industry_Classification_Benchmark>, <https://www.lseg.com/en/ftse-russell/industry-classification-benchmark-icb>).
- **History:** The lineage dates to 1962 when FT Actuaries built the original classification for the FT All-Share Index, revised in 1970 and 1994. The modern ICB launched January 2006, replacing earlier FTSE-Dow-Jones systems. **2019 redesign** (effective 2019-07-01) integrated the legacy Russell Global Sectors (RGS) classification into ICB and expanded sector definitions (source: <https://www.lseg.com/en/ftse-russell/industry-classification-benchmark-icb>).
- **Structure (post-2019 V.2):**

  | Level | Code digits | Count | Example |
  |---|---|---|---|
  | Industry | 2 | 11 | `45` = Consumer Staples |
  | Supersector | 4 | 20 | `4510` = Food, Beverage and Tobacco |
  | Sector | 6 | 45 | `451010` = Beverages |
  | Subsector | 8 | 173 | `45101020` = Soft Drinks |

  Like GICS, ICB uses an 8-digit numeric code that nests cleanly: the first 2 digits give the industry, first 4 the supersector, etc. (source: <https://classification.codes/classifications/industry/icb>).

- **Licensing:** Closed; FTSE Russell licenses ICB per-product to exchanges and index vendors. ICB is the official classification for FTSE All-World, FTSE Global Equity Index Series, STOXX Europe 600, NASDAQ-listed Nordic indices, and is used by Athens, Cyprus, Borsa Kuwait, Borsa Italiana, JSE Johannesburg, SIX Swiss Exchange, and (since 2020) NASDAQ migrated its sector indexes onto ICB (source: <https://www.nasdaqtrader.com/TraderNews.aspx?id=fpnews2020-3>).
- **Vendor field appearances:**
  - LSEG Datastream: `INDC` / `ICBSC` / `ICBSSC` / `ICBSC8` fields; mnemonics `WC.ICB*`.
  - LSEG Workspace: `TR.ICBIndustry`, `TR.ICBSupersector`, `TR.ICBSector`, `TR.ICBSubsector`.
  - Bloomberg (where licensed): `ICB_*_NAME` family.
  - FactSet (where licensed via FTSE): `FF_ICB_*` tables, less commonly used than GICS/RBICS.

### A.3 TRBC — The Refinitiv Business Classification

- **Vendor of record:** **LSEG** (Refinitiv brand pre-2023). Originated as the **Reuters Business Sector Scheme (RBSS)** in 2004; rebranded **TRBC** after Thomson Corporation acquired Reuters in 2008; rebadged again under Refinitiv after the 2018 Blackstone/Thomson Reuters carve-out; now back under LSEG after the 2021 acquisition (source: <https://en.wikipedia.org/wiki/The_Refinitiv_Business_Classification>).
- **History / versions:** RBSS 2004 → TRBC 2008 → TRBC 2012 (added the fifth level, "Activities") → TRBC 2020 (current). Coverage: 72,000+ public companies in 130 countries, history to 1999 (source: same Wikipedia article).
- **Structure (TRBC 2020):**

  | Level | Code digits | Count | Example |
  |---|---|---|---|
  | Economic Sector | 2 | 10 (or 13 in marketed structure depending on version) | `50` = Energy |
  | Business Sector | 4 | 33 (or 28 depending on source) | `5020` = Renewable Energy |
  | Industry Group | 6 | 62 (or 54 depending on source) | `502010` = Renewable Energy Equipment & Services |
  | Industry | 8 | 154 (or 136) | `50201010` = Wind Systems & Equipment-shape industry |
  | Activity | 10 | 898 (or 837 / 916 depending on version) | `5020101010` = Wind Systems & Equipment activity |

  The counts vary across LSEG, Wikipedia, and the Equidam fact sheet — they reflect different snapshots of the rolling 2020-vintage rev. with periodic adds. `[unverified — exact counts as of 2026-05]`. The marketing material (Equidam fact sheet) cites "10 Economic Sectors → 28 Business Sectors → 54 Industry Groups → 136 Industries → 837 Activities"; Wikipedia (LSEG-sourced) cites "10 / 33 / 62 / 154 / 898"; the project brief cites "13 / 33 / 62 / 154 / 916". All are within the same family — the difference is whether you count specialty / non-equity buckets and which dated rev. is the reference.

- **Licensing:** *This is the standout.* **TRBC codes themselves are exposed via the open PermID API at zero cost** (source: <https://permid.org/>, <https://developers.lseg.com/en/api-catalog/open-perm-id/permid-entity-search>). LSEG's developer documentation explicitly returns TRBC fields in a PermID Entity Search response. This makes TRBC the only deep, index-grade taxonomy with an open hook on the public internet. The historical *time series* of classification assignments (i.e., "what was company X's TRBC code as of date D") is licensed via Datastream / Workspace.
- **Vendor field appearances:**
  - PermID API: `entity.organization.industryGroupTypeCode`, `entity.organization.activityCode` (free).
  - LSEG Workspace / Datastream: `TR.TRBCEconomicSector`, `TR.TRBCBusinessSector`, `TR.TRBCIndustryGroup`, `TR.TRBCIndustry`, `TR.TRBCActivity`; numeric codes via `WC.TRBC*` mnemonics.
  - FactSet: `TR_*` mnemonics where TRBC is licensed alongside.
  - Bloomberg: not natively exposed; users typically reverse-map via PermID lookup.

### A.4 BICS — Bloomberg Industry Classification System

- **Vendor of record:** **Bloomberg L.P.** Pure-proprietary, distributed via Bloomberg Terminal, BPipe, and Data License (DL / DL+ on Snowflake).
- **History:** Bloomberg has run an internal industry taxonomy since the 1990s. The system reached its current "up to 7-level" form during the 2000s. Most recent material public revision: **2024 update effective EOD 2024-06-07** (source: <https://assets.bbhub.io/professional/sites/27/Bloomberg-Industry-Classification-System-BICS-Hierarchy-Change-June-2024.pdf>), with sector moves in financial services and additions for EV-adjacent industries.
- **Structure:** 7 levels. Bloomberg classifies *every* company to at least Level 4 and as deep as Level 7 (source: <https://www.thegoldensource.com/bloomberg-and-industry-classifications/>):

  | Level | Conventional name | Approx. count (2024) | Source |
  |---|---|---|---|
  | L1 | Sector | 11 | Bloomberg Global Equity Indices Methodology |
  | L2 | Industry Group | ~49 [unverified] | project brief / triangulated |
  | L3 | Industry | ~196 [unverified] | project brief / triangulated |
  | L4 | Sub-Industry | ~712 [unverified] | project brief / triangulated |
  | L5–L7 | Segments | up to 2,294 unique L1-L7 buckets total [unverified] | Bloomberg Equity Indices methodology |

  Bloomberg has not published an authoritative public count of L2–L4 buckets. The 2,294 total-bucket figure is the most widely cited public number (source: <https://www.conseq.cz/getmedia/475278c1-bc71-4ca5-95ac-c9d5a1a7d014/Bloomberg-Global-Equity-Indices-Methodology-2312.pdf.aspx>). The project brief's per-level counts (10/49/196/712/2144) appear consistent with the order of magnitude but are not first-party-published.

  Code format is variable-length numeric. Level 1 is 2-digit, deeper levels concatenate more digits but the exact format and zero-padding rules are not in public documentation. `[unverified]`

- **Methodology:** Bloomberg analysts assign classifications using primary revenue source as the lead measure; operating income and assets as secondary tiebreakers (source: <https://data.bloomberglp.com/professional/sites/10/Classification-Data-Fact-Sheet.pdf>).
- **Licensing:** Closed; Bloomberg subscriber-only. There is no public BICS lookup endpoint comparable to TRBC-via-PermID.
- **Vendor field appearances:**
  - Bloomberg Terminal: `BICS_LEVEL_1_SECTOR_NAME` through `BICS_LEVEL_7_SUB_INDUSTRY_NAME` (or `..._CODE`); also exposed via BQL (`bql.data.bics_level_1_sector_name()`).
  - DL / DL+: BICS fields available in fundamentals + reference data packs at per-field licensing.

### A.5 RBICS — Revere Business Industry Classification System

- **Vendor of record:** **FactSet** (acquired Revere Data in 2013). Pure-proprietary.
- **Structure:** 6 levels in a 14-by-6 matrix shape: 12 anchor industries + 2 specialty industries at L1, each with five additional layers below (source: <https://insight.factset.com/resources/factset-revere-business-industry-classifications-datafeed>, <https://assets.ctfassets.net/lmz2w5z92b9u/67nHF3Io7Zg8Ka1eQSqWsi/73277f7a9bc6250c727c8625bdc55164/factset_rbics_methodology_guide.pdf>):

  | Level | Conventional name | Count |
  |---|---|---|
  | L1 | Economy | 14 (12 anchor + 2 specialty) |
  | L2 | Sector | 36 |
  | L3 | Sub-Sector | 105 |
  | L4 | Industry Group | 316 |
  | L5 | Industry | 759 |
  | L6 | Sub-Industry (Specific Business) | 1,603 |

  Levels 1–3 reflect market-based grouping (stock co-movement), Levels 3–6 reflect product/service-based grouping ("bottom-up patented approach").

- **Multi-tag with revenue weights — THE DIFFERENTIATOR.** Unlike GICS/ICB/TRBC/BICS (which force a single primary classification), the **RBICS Revenue** package tags a company with *multiple* L6 codes weighted by revenue share. A diversified conglomerate like Berkshire Hathaway or Amazon receives 5–20+ revenue-weighted L6 tags rather than a single "primary". This is what makes RBICS the academic favourite for sector-attribution analysis (source: <https://aws.amazon.com/marketplace/pp/prodview-a7h773qqdu5mc>).
- **Coverage:** RBICS Focus ~48,000 liquid publicly-traded companies; Extended Universe ~3M entities at L4 only; Revenue variant ~45,000 companies multi-tagged; Tradenames adds ~170,000 product/service strings linked to L6 codes (source: insight.factset.com URL above).
- **History depth:** US major history extends to 2012; global to 2014. RBICS Methodology guide cites depth to 1945 for the underlying corpus.
- **Code format:** Numeric, matrix-indexed; FactSet has not published the exact digit layout publicly. `[unverified — exact code length]`. Codes are typically delivered as opaque strings keyed to the L1-L6 hierarchy in the FF_RBICS_* tables.
- **Licensing:** Closed; FactSet subscriber-only.
- **Vendor field appearances:**
  - FactSet: `FF_RBICS_L1` through `FF_RBICS_L6` (codes) and `FF_RBICS_L1_NAME` through `FF_RBICS_L6_NAME`; revenue-weighted tags in the `FF_RBICS_REV` table.
  - STOXX revenue-thematic indices license RBICS revenue weightings as the source signal (source: <https://stoxx.com/thematic-indices/revenue-based-thematic-indices/>).

### A.6 NAICS — North American Industry Classification System

- **Vendor of record:** **US Census Bureau** (Office of Management and Budget), jointly with **Statistics Canada** and **INEGI (Mexico)**.
- **History:** Adopted 1997 to replace SIC, in coordination with NAFTA. Five-year revision cycle: 1997 → 2002 → 2007 → 2012 → 2017 → **2022** (current). NAICS 2027 cycle is in progress.
- **Structure (2022):**

  | Level | Code digits | Count | Example |
  |---|---|---|---|
  | Sector | 2 | 20 sectors (numbered `11` through `92`; some sectors span ranges, e.g. Manufacturing = `31-33`) | `54` = Professional, Scientific, & Technical Services |
  | Subsector | 3 | ~99 | `541` = Professional, Scientific, & Technical Services |
  | Industry Group | 4 | ~311 | `5411` = Legal Services |
  | NAICS Industry | 5 | ~700 | `54111` = Offices of Lawyers |
  | National Industry (US-specific) | 6 | **1,057** | `541110` = Offices of Lawyers (US national detail) |

  At the 5-digit level, codes are common across all three NAFTA / USMCA countries; the 6th digit is national-specific to allow each country to add resolution. (source: <https://www.census.gov/naics/>, <https://siccode.com/page/structure-of-naics-codes>).

- **Licensing:** **Public domain** (US Census Bureau works are not copyrightable). All NAICS code lists, definitions, and historical concordances are downloadable as XLSX/CSV for free at <https://www.census.gov/naics/?68967>.
- **Concordances published by Census Bureau** (source: <https://www.census.gov/eos/www/naics/concordances/concordances.html>):
  - NAICS 2022 ↔ NAICS 2017 ↔ NAICS 2012 ↔ NAICS 2007 ↔ NAICS 2002 ↔ NAICS 1997.
  - NAICS ↔ SIC 1987 (the canonical "bridge" used by every researcher).
  - NAICS ↔ ISIC rev. 4.
- **Vendor field appearances:**
  - Compustat: `naics`, `naicsh` (historical).
  - FactSet: `FG_NAICS_CODE` family.
  - Bloomberg: `NAICS_CODE`, `NAICS_SUBSECTOR`, etc.
  - LSEG: `WC.NAICS*` mnemonics.
  - SEC EDGAR: NAICS *not* exposed on filer cover pages; only `assigned-sic` is.

### A.7 SIC — Standard Industrial Classification

- **Vendor of record:** US **Bureau of Labor Statistics / Office of Management and Budget** (frozen 1987 vintage). **SEC** maintains its own slightly modified fork for EDGAR.
- **History:** Established 1937 by US federal interagency committee. Revised 1941, 1945, 1949, 1957, 1963, 1967, 1972, 1977, **1987 (final)**. Deprecated 1997 in favour of NAICS for Census Bureau and BLS use, but **the SEC, OSHA, FDIC, and several other regulators never migrated** (source: <https://en.wikipedia.org/wiki/Standard_Industrial_Classification>).
- **Structure:**

  | Level | Code | Count | Example |
  |---|---|---|---|
  | Division | letter A–K | 11 | `D` = Manufacturing |
  | Major Group | 2-digit | ~83 | `36` = Electronic & Other Electrical Equipment |
  | Industry Group | 3-digit | ~416 | `367` = Electronic Components & Accessories |
  | Industry | 4-digit | ~1,005 | `3672` = Printed Circuit Boards |

  Code ranges: Agriculture 0100–0999, Mining 1000–1499, Construction 1500–1799, Manufacturing 2000–3999, Transportation/Comm/Utilities 4000–4999, Wholesale Trade 5000–5199, Retail Trade 5200–5999, Finance/Insurance/Real Estate 6000–6799, Services 7000–8999, Public Administration 9100–9729, Nonclassifiable 9900–9999.

- **The SEC fork.** The SEC publishes a slightly modified SIC list at <https://www.sec.gov/search-filings/standard-industrial-classification-sic-code-list>. It is *not* identical to the 1987 OMB list; the SEC adds and renames a handful of codes to suit corporate-filer review-team routing. Key SEC-specific buckets:
  - `8888` = **Foreign Governments** — used for sovereign filers (Republic of Finland, Commonwealth of Australia, etc.) (source: <https://www.secinfo.com/$/SEC/SIC.asp?Industry=8888>).
  - `9995` = **Non-Classifiable Establishments** (SEC-specific).
  - `9999` = **Nonclassifiable Establishments** (matches the BLS 1987 final-catchall).
  - The SEC list also includes a handful of investment-fund-specific codes (`6770` Blank Checks; `6199` Finance Services) that are heavily used for SPACs and shell companies.

- **Licensing:** **Public domain.** The SEC publishes its modified list as a static HTML page; researchers parse via scraping. CRSP's `HSICCD`/`SICCD`, Compustat's `sic`/`sich`, and OSHA's industry queries all share lineage from the 1987 OMB vintage with minor agency-specific differences.
- **CRSP vs Compustat SIC.** Both CRSP and Compustat maintain their own SIC code per security/company. They disagree on roughly **36% of jointly covered firms** (source: <https://www.semanticscholar.org/paper/Differences-between-COMPUSTAT-and-CRSP-SIC-codes-on-Guenther-Rosman/c0303ced40f51b32e930762f83d37918e969eebb>). CRSP exposes `HSICCD` (header SIC — last non-zero SIC seen) and historical `SICH`; Compustat exposes `sic` (current) and `sich` (historical). Modern CRSP CIZ format renames `HSICCD` to `SICCD`. Any backtest that joins CRSP↔Compustat needs to choose which SIC to trust, and the choice changes results materially.
- **Vendor field appearances:**
  - SEC EDGAR: `assigned-sic` field on every filer's CIK header (`https://www.sec.gov/cgi-bin/browse-edgar?action=getcompany&SIC=XXXX`).
  - CRSP: `HSICCD`, `SICCD`, `SICH` (legacy `SCIH`).
  - Compustat: `sic`, `sich`, `co_idesind.sich`.
  - Bloomberg: `SIC_CODE`, `SIC_INDUSTRY_NAME`.
  - LSEG: `WC.SICCode`, `TR.SICCode`.

### A.8 ISIC — International Standard Industrial Classification

- **Vendor of record:** **United Nations Statistics Division**.
- **History:** Rev. 1 (1958) → Rev. 2 (1968) → Rev. 3 (1989) → Rev. 3.1 (2002) → **Rev. 4 (2008)** → Rev. 5 (endorsed by UN Statistical Commission 2023, pending publication and adoption).
- **Structure (Rev. 4):**

  | Level | Code | Count | Example |
  |---|---|---|---|
  | Section | letter A–U | 21 | `C` = Manufacturing |
  | Division | 2-digit | 88 | `26` = Computer, electronic, optical products |
  | Group | 3-digit | 238 | `262` = Computers and peripheral equipment |
  | Class | 4-digit | 419 | `2620` = Manufacture of computers and peripheral equipment |

- **Licensing:** **Open.** UN publishes ISIC at <https://unstats.un.org/unsd/classifications/Econ/isic> as a free download with no redistribution restrictions.
- **Uses:** UN Comtrade, World Bank country statistics, OECD national accounts, IMF balance-of-payments. Most national statistical offices outside USMCA derive their classification from ISIC (NACE for EU; ANZSIC for Australia/NZ; JSIC for Japan; etc.).

### A.9 NACE — Statistical Classification of Economic Activities in the European Community

- **Vendor of record:** **Eurostat** (EU Commission); national variants by member-state statistical offices.
- **History:** NACE rev. 1 (1990) → rev. 1.1 (2002) → rev. 2 (2008) → **rev. 2.1 (effective 2025-01-01)**, adopted by Commission Delegated Regulation (EU) 2023/137 of 10 October 2022 (source: <https://ec.europa.eu/eurostat/web/products-eurostat-news/w/wdn-20230210-1>).
- **Structure (Rev. 2.1):**

  | Level | Code | Count |
  |---|---|---|
  | Section | letter A–V | 22 |
  | Division | 2-digit | 88 |
  | Group | 3-digit | 272 |
  | Class | 4-digit | 615 |

  First 4 digits are common across all EU member states; national variants can add more digits (e.g., German WZ adds a 5th digit). (source: <https://en.wikipedia.org/wiki/Statistical_Classification_of_Economic_Activities_in_the_European_Community>).

- **Licensing:** Open; freely downloadable from Eurostat.
- **CSRD / ESRS overlay.** The European Sustainability Reporting Standards (ESRS) under CSRD anchor sector-specific disclosure requirements on NACE Rev. 2.1. EFRAG's draft ESRS SEC 1 Sector Classification standard uses NACE classes as the sector identifier (source: <https://efrag-website.azurewebsites.net/Assets/Download?assetUrl=%2Fsites%2Fwebpublishing%2FMeeting+Documents%2F2311080942079508%2F05-04+-+Sector+Classification+SEC+1+-+NACE+2-1+-+SR+TEG+240115.pdf>).

### A.10 Niche / derived schemes

#### Fama-French 12 / 17 / 30 / 48 / 49 industries
Eugene F. Fama + Kenneth R. French, Tuck School of Business, Dartmouth. Each version partitions all 4-digit SIC codes into N industry buckets: FF5 → FF10 → FF12 → FF17 → FF30 → FF38 → FF48 → FF49. The official mapping files are zipped lists of SIC ranges with industry names (`Siccodes48.zip` at <https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_48_ind_port.html>). Free for academic + commercial use. The de facto standard for asset-pricing industry controls and replication studies — trivial to ship as a derived view from SIC.

#### BEA Industry Economic Accounts
US Bureau of Economic Analysis publishes GDP-by-industry and detailed-industry accounts on a NAICS-derived 71-industry "detailed" partition rolled up to 21 super-sectors (source: <https://www.bea.gov/data/special-topics/industry>). Public domain.

#### ESMA EMIR / MiFID issuer category
For European derivatives reporting, ESMA defines a small issuer-category enumeration (e.g., "non-financial counterparty above clearing threshold"). Not a sector classification per se but used as a sector-shaped axis in EU reg-tech. Open.

#### MSCI ACWI sector / FTSE Russell ICB indices
MSCI ACWI is a regional subset of GICS (licensed under the GICS contract). Russell 1000/2000/3000 Growth/Value and FTSE All-World use ICB sectors as the cap-weight rollup spine (ICB licence covers these).

#### Wikidata `industry` (P452) and `instance of` (P31)
Wikidata's `industry` property links a company Q-ID to an industry Q-ID; Q-IDs are flat (no hierarchical numeric code). NAICS codes are exposed as property `P3224`; SIC as `P3242`; GICS / ICB / TRBC / BICS are not directly exposed but can be inferred from cross-property joins (sources: <https://www.wikidata.org/wiki/Property:P452>, <https://www.wikidata.org/wiki/Property:P3224>, <https://www.wikidata.org/wiki/Property:P3242>). **License: CC0.** The only fully-public knowledge graph that bridges issuer-level identifiers (ISIN, LEI, CIK, tickers) to industry tags — ats-eqt's most valuable "free crosswalk" hook.

---

## Part B — Cross-walks

Every taxonomy claims to be the canonical one. They all disagree. The cross-walks below are **all many-to-many** by construction — a single GICS sub-industry typically spans 5–15 NAICS-6 codes and vice versa.

### B.1 GICS ↔ NAICS

- **Canonical academic crosswalk:** Alison Weingarden (Federal Reserve Board economist) maintains a publicly-distributed GICS↔NAICS bridge as part of her research site (source: <https://sites.google.com/site/alisonweingarden/links/industries>). The 2014 version maps **144 GICS sub-industries** to **989 NAICS-6 industries** (2012 NAICS vintage). Updated periodically; the most-cited version is the 2014 snapshot used in subsequent academic papers.
- **Commercial crosswalks:** S&P CIQ exposes both GICS and NAICS on `ciqCompany`, so within the Capital IQ universe a (GICS, NAICS) tuple is observable per issuer; but the *mapping* (i.e., "GICS sub-industry X corresponds to NAICS-6 set {Y1, Y2, ...}") is not officially published by either S&P or Census Bureau.
- **Open-source crosswalks:** mgao6767 GitHub gist (<https://gist.github.com/mgao6767/4134ce36793b9e932a219ff07d7a3c7f>) constructs 4 industry classifications (SIC, NAICS, GICS, Fama-French) and aligns them; Classification.Codes maintains a paid "crosswalk tables to 50+ industry classifications" product (<https://classification.codes/classifications/industry/gics/>).
- **Known gotchas:**
  - **REIT carve-out (2016).** Pre-2016 GICS rolled REITs into Financials. NAICS keeps REITs at `525930` (Real Estate Investment Trusts) under sector 52 (Finance and Insurance). A pre-2016 vintage GICS-Financials sub-industry maps to a *broader* NAICS set than a post-2016 GICS-Real-Estate sub-industry.
  - **Internet & Direct Marketing Retail (2023).** GICS 2023 dispersed this sub-industry across multiple new ones. NAICS does not match — Amazon's NAICS at `454110` (Electronic Shopping & Mail-Order Houses) does not bijectively map to any single GICS post-2023 code.
  - **Holding companies.** NAICS `551112` (Offices of Other Holding Companies) is overused for conglomerates; GICS forces a primary industry — the two never align cleanly for Berkshire, GE-style structures.

### B.2 GICS ↔ SIC

- **Indirect via NAICS.** Most practitioners route through NAICS: GICS → NAICS (Weingarden) → SIC (Census Bureau NAICS-2002↔SIC-1987 concordance). This double-hop loses fidelity at every step.
- **Direct mapping:** No officially-published direct crosswalk. WRDS publishes the `INDCLASS` macro (<https://wrds-www.wharton.upenn.edu/pages/wrds-research/macros/wrds-macro-indclass/>) which constructs 4-way harmonisation across SIC, NAICS, GICS, and Fama-French; the underlying GICS↔SIC bridge is via Compustat's joint `sic`+`gsubind` fields.
- **Known gotchas:** SIC is frozen at 1987; GICS evolves continually. The 2016 GICS Real Estate carve-out has no SIC echo because SIC 6798 (Real Estate Investment Trusts) was always its own bucket. The 2018 Communication Services rebrand has no SIC equivalent at all — SIC 4813 (Telephone Communications) is still the right tag for a pure telco but is now a stylistic mismatch for Communication Services-classified content companies.

### B.3 TRBC ↔ GICS

- **LSEG-internal mapping.** LSEG publishes a crosswalk between TRBC and GICS in some Datastream developer documentation for licensed customers; bits have leaked into open Github repositories (e.g., the swanest/TRBC node library at <https://github.com/swanest/TRBC>).
- **Practical mapping:** TRBC's 5-level vs GICS's 4-level structure means TRBC Activity (L5) is *finer* than GICS Sub-Industry (L4). A 1:1 TRBC-Activity → GICS-Sub-Industry mapping is roughly **6:1** on average (~898 TRBC activities into 163 GICS sub-industries). The 33 TRBC Business Sectors and 25 GICS Industry Groups are the closest-aligned levels.
- **The PermID hook.** Because TRBC is exposed via PermID, an open competitor can publish a TRBC→GICS-equivalent rough mapping without licensing GICS itself; the legal posture is that the *bucket structure* is a learned approximation, not GICS itself.

### B.4 NAICS ↔ ISIC

- **Officially published by Census Bureau:** <https://www.census.gov/eos/www/naics/concordances/concordances.html> (`NAICS to ISIC` concordance). Format is XLSX/CSV; covers NAICS 2017 ↔ ISIC Rev. 4 explicitly. NAICS 2022 ↔ ISIC Rev. 4 concordance is published in the 2022 update.
- **UN Statistics Division also publishes** an ISIC↔NAICS bridge (mirror of the same content).
- **Cardinality:** NAICS-6 → ISIC-4 is approximately many-to-one (NAICS is finer than ISIC at the leaf level), but with significant exceptions in services (ISIC 62 "Computer programming, consultancy and related activities" splits into multiple NAICS-6 codes; ISIC 47 "Retail trade" splits into many NAICS subsectors).

### B.5 SEC SIC vs official SIC

- **The SEC fork is not the 1987 BLS final.** SEC added codes (`8888 Foreign Governments`, `6770 Blank Checks`, `6199 Finance Services`) and renamed others to match Division-of-Corporation-Finance review-team routing. A research pipeline that joins SEC EDGAR's `assigned-sic` to BLS / Census SIC tables will mis-match on the SPACs, sovereigns, and certain financial-services filers.
- **Operational implication.** Maintain a separate `sic_code_dim` table keyed `(sic_authority, sic_code)` where `sic_authority IN ('SEC', 'BLS_1987', 'CENSUS_1987', 'OSHA')`. Cross-walk between authorities is a static lookup that needs to be checked into the codebase.

### B.6 GICS ↔ Bloomberg BICS

- No published official crosswalk. Practitioners reverse-engineer via the Bloomberg field `GICS_SECTOR_NAME` alongside `BICS_LEVEL_1_SECTOR_NAME` per security and infer the implicit mapping. L1 GICS sectors (11) map roughly 1:1 with L1 BICS sectors (11) by name but not by definition — e.g., GICS Communication Services and BICS Communications differ on the inclusion of media/entertainment companies.

### B.7 GICS / ICB ↔ Fama-French

- Fama-French is SIC-derived, so GICS / ICB → FF is a double-hop through SIC. The FF12 / FF17 / FF30 / FF48 / FF49 partitions are coarser than GICS at every level except sector. Practitioners typically use Fama-French as a *coarsening* of GICS / SIC rather than a translation.

### B.8 The "no-bijection" cardinality table

A rough sense of how many-to-many each pair really is, at leaf level (lower number = more 1:1, higher = more many-to-many):

| From → To | Avg. fan-out | Notes |
|---|---|---|
| GICS-SubInd (163) → NAICS-6 (1,057) | ~6.5 | NAICS is finer for niche US services |
| GICS-SubInd (163) → ICB-Subsector (173) | ~1.1 | Nearly bijective; same depth, different boundary lines |
| GICS-SubInd (163) → TRBC-Activity (~898) | ~5.5 | TRBC finer at leaf |
| GICS-SubInd (163) → BICS-L4 (~712) | ~4.4 | BICS finer at L4 |
| NAICS-6 (1,057) → ISIC-4 (419) | ~0.4 (ISIC coarser) | NAICS is finer than ISIC |
| NAICS-6 (1,057) → SIC-4 (~1,005) | ~1.0 | Equal cardinality but different boundary lines |
| SIC-4 (1,005) → FF48 (48) | ~21 | FF is intentionally coarse |

All pairs are many-to-many; the table is illustrative.

---

## Part C — Public reconstruction

Goal: reconstruct GICS-equivalent classifications from open public data sources, without licensing GICS / ICB / TRBC / BICS / RBICS.

### C.1 Public data inputs

| Source | URL | Coverage | License |
|---|---|---|---|
| US Census Bureau NAICS tables | <https://www.census.gov/naics/> | 2022, 2017, 2012, 2007, 2002, 1997 | Public domain |
| Census Bureau NAICS↔SIC bridge | <https://www.census.gov/eos/www/naics/concordances/concordances.html> | 1987 SIC ↔ 1997+ NAICS | Public domain |
| BLS SIC archive | <https://www.bls.gov/emp/documentation/crosswalks.htm> | 1987 final SIC | Public domain |
| SEC SIC list | <https://www.sec.gov/search-filings/standard-industrial-classification-sic-code-list> | SEC fork | Public domain |
| SEC EDGAR per-filer SIC | `cik-lookup-data.txt` + per-CIK headers | Every EDGAR filer | Public domain |
| Eurostat NACE tables | <https://ec.europa.eu/eurostat/web/nace> | Rev. 2, Rev. 2.1 | CC-BY-equivalent |
| UN ISIC tables | <https://unstats.un.org/unsd/classifications/Econ/isic> | Rev. 3, 3.1, 4, (5 pending) | Open |
| Kenneth French industry definitions | <https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/> | 12/17/30/48/49 partitions | Free academic |
| LSEG PermID (TRBC) | <https://permid.org/> | Per-entity TRBC code | Free with key |
| Wikidata `industry` (P452), NAICS (P3224), SIC (P3242) | <https://www.wikidata.org/> | ~10M entity-tagged items | CC0 |
| OpenCorporates classification tags | <https://opencorporates.com/> | Per-jurisdiction; uneven | Strict (share-alike free; commercial paid) |

### C.2 Strategy: "GICS-shape" from NAICS

1. **Anchor on NAICS-6** as the primary US public-equity classification (derive from EDGAR SIC → NAICS via Census bridge, or scrape direct from 10-K business descriptions).
2. **Group NAICS-6 codes into a 4-level "GICS-shape" hierarchy** using Weingarden's crosswalk (or equivalent). Publish it under an explicit "this is not GICS" disclaimer.
3. **Tag with Fama-French 48-industry** for academic credibility.
4. **Tag with TRBC code via PermID** for international/cross-border alignment with LSEG indices.
5. **Optionally publish a NACE Rev. 2.1 mirror** for EU CSRD / ESRS users.
6. **Use LLM zero-shot classification** of 10-K business descriptions for issuers where (a) SEC SIC is `9999`/`8888`, (b) NAICS bridge fails, or (c) the business description has materially changed since last filing. Calibrate against the FactSet RBICS multi-tag-with-revenue-weights model where available.

### C.3 The honest limit

You cannot ship "GICS Sector = Financials" downstream without licensing GICS. You can ship "ats-eqt Financial Sector (NAICS-derived, GICS-equivalent v1.0)" and provide a published crosswalk to GICS-sub-industry-name for license-holding consumers. This is the same posture FRED + BEA take with NAICS-derived industry aggregates.

---

## Part D — Recommended ats-eqt schema

Mirroring the bitemporal long-format pattern in `schemas/data_models_and_methodology.md`. Five tables: taxonomy, taxonomy_node, entity_classification, taxonomy_mapping, and two public-taxonomy dim tables for fast public lookup.

### D.1 Taxonomy registry

```sql
CREATE TABLE taxonomy (
  taxonomy_id        INTEGER     PRIMARY KEY,
  code               TEXT        NOT NULL UNIQUE,     -- 'GICS', 'ICB', 'TRBC', 'BICS', 'RBICS',
                                                       -- 'NAICS', 'SIC', 'SIC_SEC', 'ISIC', 'NACE',
                                                       -- 'FF48', 'WIKIDATA_INDUSTRY', 'ATS_EQT_v1'
  name               TEXT        NOT NULL,
  version            TEXT        NOT NULL,             -- 'v12-2023-03-17', '2019-V2', '2022', etc.
  vendor             TEXT        NOT NULL,             -- 'S&P+MSCI', 'LSEG', 'Bloomberg', 'FactSet',
                                                       --  'US Census', 'BLS', 'SEC', 'UN', 'Eurostat',
                                                       --  'Tuck', 'ats-eqt'
  licensed_flag      BOOLEAN     NOT NULL,             -- TRUE = closed/paid; FALSE = open/public
  redistribution_ok  BOOLEAN     NOT NULL,             -- TRUE iff ats-eqt can re-publish codes downstream
  root_node_id       BIGINT      NULL,                 -- → taxonomy_node
  effective_from     DATE        NOT NULL,
  effective_to       DATE        NULL,                 -- NULL = current
  source_url         TEXT        NULL
);
```

### D.2 Taxonomy nodes (hierarchical)

```sql
CREATE TABLE taxonomy_node (
  taxonomy_id        INTEGER     NOT NULL,             -- → taxonomy
  node_id            BIGINT      NOT NULL,             -- ats-eqt internal stable key
  parent_node_id     BIGINT      NULL,                 -- → taxonomy_node (NULL at root)
  code               TEXT        NOT NULL,             -- the vendor code: '25201020', '45101020',
                                                       --  '5020101010', '541110', '3672', 'C', 'A',
                                                       --  '20-Industrials', etc.
  level              SMALLINT    NOT NULL,             -- 1=root sector; 2=industry group; etc.
                                                       --  GICS uses 1..4, TRBC uses 1..5,
                                                       --  RBICS uses 1..6, BICS uses 1..7
  level_name         TEXT        NOT NULL,             -- 'Sector', 'Industry Group', 'Sub-Industry',
                                                       --  'Activity', 'Class', 'Division', etc.
  name_en            TEXT        NOT NULL,
  name_aliases       JSONB       NULL,                 -- {'fr': '...', 'de': '...', ...} for NACE/ISIC
  effective_from     DATE        NOT NULL,             -- when this node entered the taxonomy
  effective_to       DATE        NULL,                 -- NULL = still active; date = retired
  PRIMARY KEY (taxonomy_id, node_id),
  UNIQUE (taxonomy_id, code, effective_from)
);

CREATE INDEX ix_taxonomy_node_parent ON taxonomy_node(taxonomy_id, parent_node_id);
CREATE INDEX ix_taxonomy_node_code ON taxonomy_node(taxonomy_id, code);
```

### D.3 Entity classification (bitemporal)

```sql
CREATE TABLE entity_classification (
  entity_id          BIGINT      NOT NULL,             -- → entity
  taxonomy_id        INTEGER     NOT NULL,             -- → taxonomy
  node_id            BIGINT      NOT NULL,             -- → taxonomy_node (typically a leaf)
  primary_flag       BOOLEAN     NOT NULL,             -- TRUE = primary classification under this taxonomy
  weight             NUMERIC(7,6) NULL,                -- NULL for single-primary taxonomies;
                                                       --  NON-NULL for RBICS multi-tag (revenue share)
  source             TEXT        NOT NULL,             -- 'VENDOR_FactSet', 'VENDOR_LSEG_PermID',
                                                       --  'PUBLIC_SEC_EDGAR', 'PUBLIC_Census',
                                                       --  'DERIVED_NAICS_to_GICS', 'LLM_10K'
  source_confidence  NUMERIC(4,3) NULL,                -- 0.0..1.0; populated for derived/LLM
  valid_from         DATE        NOT NULL,             -- when the classification was true in the world
  valid_to           DATE        NOT NULL,             -- typically 9999-12-31 for current
  knowledge_from     TIMESTAMP   NOT NULL,
  knowledge_to       TIMESTAMP   NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (entity_id, taxonomy_id, node_id, valid_from, knowledge_from)
);

-- Constraint: per (entity, taxonomy, valid_from period), exactly one primary_flag=TRUE
-- enforced via partial unique index:
CREATE UNIQUE INDEX ux_entity_classification_primary
  ON entity_classification (entity_id, taxonomy_id, valid_from)
  WHERE primary_flag = TRUE AND knowledge_to = 'infinity';

CREATE INDEX ix_entity_classification_entity ON entity_classification(entity_id);
CREATE INDEX ix_entity_classification_node ON entity_classification(taxonomy_id, node_id);
```

The `weight` column is the critical design choice that makes RBICS-style multi-tag work alongside GICS/ICB-style primary classification. For single-primary taxonomies, `weight` is NULL (or 1.0 by convention). For RBICS-Revenue, each entity has N rows summing to 1.0, with one row carrying `primary_flag=TRUE` (the largest revenue share).

### D.4 Cross-walk / mapping table (bitemporal)

```sql
CREATE TABLE taxonomy_mapping (
  from_taxonomy_id    INTEGER     NOT NULL,            -- → taxonomy
  from_node_id        BIGINT      NOT NULL,            -- → taxonomy_node
  to_taxonomy_id      INTEGER     NOT NULL,            -- → taxonomy
  to_node_id          BIGINT      NOT NULL,            -- → taxonomy_node
  mapping_confidence  NUMERIC(4,3) NULL,               -- 0.0..1.0
  mapping_strength    TEXT        NULL,                -- 'EXACT', 'BEST_EFFORT', 'PARTIAL', 'AGGREGATED'
  source              TEXT        NOT NULL,            -- 'Census_NAICS_SIC', 'Weingarden_GICS_NAICS',
                                                       --  'UN_NAICS_ISIC', 'LSEG_TRBC_GICS',
                                                       --  'ats_eqt_derived'
  valid_from          DATE        NOT NULL,            -- e.g., NAICS 2022 vintage start
  valid_to            DATE        NOT NULL,
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (from_taxonomy_id, from_node_id, to_taxonomy_id, to_node_id, valid_from, knowledge_from)
);

CREATE INDEX ix_taxonomy_mapping_to ON taxonomy_mapping(to_taxonomy_id, to_node_id);
```

This table is many-to-many by design. A GICS-SubInd → NAICS-6 mapping will have 5–15 rows per source code; an NAICS-6 → ISIC-4 mapping will have 1–3 rows per source code; SEC-SIC → BLS-SIC is mostly 1:1 with a small handful of fork-specific exceptions.

### D.5 Denormalized public-taxonomy lookup tables

For fast public lookups without joining through `taxonomy_node`. All four follow the same shape — the SIC variant shown; NAICS, ISIC, NACE analogous with their level columns and vintage/revision discriminator.

```sql
CREATE TABLE sic_code_dim (
  sic_code            CHAR(4)     NOT NULL,
  authority           TEXT        NOT NULL,            -- 'SEC', 'BLS_1987', 'CENSUS_1987', 'OSHA'
  division            CHAR(1)     NOT NULL,            -- 'A'..'K'
  major_group_code    CHAR(2)     NOT NULL,
  industry_group_code CHAR(3)     NOT NULL,
  name_en             TEXT        NOT NULL,
  division_name       TEXT        NOT NULL,
  major_group_name    TEXT        NOT NULL,
  industry_group_name TEXT        NOT NULL,
  PRIMARY KEY (sic_code, authority)
);

-- naics_code_dim:  PK (naics_code VARCHAR(6), vintage CHAR(4))
--                  + sector/subsector/industry_group/naics_industry codes, level 2..6
-- isic_code_dim:   PK (isic_code VARCHAR(4), revision VARCHAR(8))
--                  + section CHAR(1) 'A'..'U', division/group codes, level 1..4
-- nace_code_dim:   PK (nace_code VARCHAR(5), revision VARCHAR(8))
--                  + section CHAR(1) 'A'..'V', division/group codes, level 1..4,
--                    name_translations JSONB for fr/de/es/etc.
```

### D.6 Materialized views

```sql
-- Per-entity primary classification across every taxonomy ats-eqt holds
CREATE MATERIALIZED VIEW entity_primary_classifications AS
SELECT ec.entity_id, t.code AS taxonomy_code, tn.code AS node_code,
       tn.name_en AS node_name, tn.level, ec.valid_from, ec.valid_to
FROM entity_classification ec
JOIN taxonomy t       ON ec.taxonomy_id = t.taxonomy_id
JOIN taxonomy_node tn ON ec.taxonomy_id = tn.taxonomy_id AND ec.node_id = tn.node_id
WHERE ec.primary_flag = TRUE AND ec.knowledge_to = 'infinity';

-- Revenue-weighted multi-tag view (for RBICS-style consumers)
CREATE MATERIALIZED VIEW entity_revenue_weighted_classifications AS
SELECT ec.entity_id, ec.taxonomy_id, ec.node_id, tn.code, tn.name_en,
       ec.weight, ec.valid_from
FROM entity_classification ec
JOIN taxonomy_node tn ON ec.taxonomy_id = tn.taxonomy_id AND ec.node_id = tn.node_id
WHERE ec.weight IS NOT NULL AND ec.knowledge_to = 'infinity';
```

### D.7 Ingestion pipeline outline

1. **Bootstrap dim tables** from Census / Eurostat / UN open downloads (NAICS, SIC, ISIC, NACE). Static; refresh on vintage release.
2. **Bootstrap `taxonomy` + `taxonomy_node`** for SIC, NAICS, ISIC, NACE, Fama-French as the open-source spine. Add ats-eqt's derived "GICS-equivalent" taxonomy.
3. **Pull per-issuer SIC** from EDGAR `cik-lookup-data` and per-CIK headers; populate `entity_classification` with `taxonomy_id = SIC_SEC`, `primary_flag=TRUE`, `source='PUBLIC_SEC_EDGAR'`.
4. **Cross-walk to NAICS** via Census Bureau bridge; populate `entity_classification` with `taxonomy_id = NAICS_2022`.
5. **Pull TRBC via PermID** for non-US issuers (where free PermID lookup succeeds).
6. **Derive Fama-French** mechanically from SIC.
7. **Derive ats-eqt-v1 (GICS-equivalent)** from NAICS via Weingarden crosswalk + manual overrides.
8. **For licensed vendors (GICS, ICB, BICS, RBICS):** populate `entity_classification` only inside ats-eqt's internal database, with `source='VENDOR_*'` and `redistribution_ok=FALSE` enforced at query-time via row-level security. Public API never exposes vendor-licensed codes.
9. **Refresh schedule:** monthly for NAICS/SIC drift from new filings; quarterly for Eurostat NACE updates; annual for ISIC; on-event for GICS/ICB/TRBC vendor releases.

---

## Part E — Licensing matrix

The single most operationally important table in this document. Determines what can ship in a public ats-eqt distribution vs what stays behind an authenticated, license-checked boundary.

| Taxonomy | Internal use OK? | Re-publish codes? | Re-publish *names*? | Derive a similar-shape taxonomy? | Pricing signal |
|---|---|---|---|---|---|
| **GICS** | Only under GICS Direct subscription | NO | NO | YES (must not call it GICS) | Per-client size; mid-five to mid-six figures USD/yr `[unverified]` |
| **ICB** | Only under FTSE Russell ICB subscription | NO | NO | YES (must not call it ICB) | Per-product licensing `[unverified]` |
| **TRBC** | YES via free PermID | YES — codes are openly exposed | YES — names openly exposed via PermID | YES | Free for code lookup; Datastream time series is licensed |
| **BICS** | Only under Bloomberg subscription | NO | NO | YES (must not call it BICS) | Bundled into Terminal / BPipe / DL `[unverified]` |
| **RBICS** | Only under FactSet subscription | NO | NO | YES (must not call it RBICS) | Bundled with FactSet Fundamentals; high-five to mid-six figures USD/yr `[unverified]` |
| **NAICS** | YES | YES | YES | YES | Free; public domain |
| **SIC (BLS 1987)** | YES | YES | YES | YES | Free; public domain |
| **SIC (SEC fork)** | YES | YES | YES | YES | Free; public domain |
| **ISIC** | YES | YES | YES | YES | Free; UN open |
| **NACE** | YES | YES | YES | YES | Free; Eurostat |
| **Fama-French 48** | YES | YES | YES | YES | Free; academic |
| **BEA detailed industry** | YES | YES | YES | YES | Free; public domain |
| **Wikidata `industry`** | YES | YES | YES | YES | CC0 |

**Operational implication.** ats-eqt's public-facing distributable consists of: NAICS + SIC + ISIC + NACE + Fama-French + Wikidata + ats-eqt-derived GICS-equivalent + TRBC-via-PermID. Anything else (true GICS / ICB / BICS / RBICS) is "internal-only, vendor-licensed" and disappears behind the same query-time ACL that hides CUSIP in the 13F dataset.

---

## Part F — Strategic positioning for ats-eqt

1. **TRBC is the strategic open hook.** The PermID API exposes 5-level TRBC codes for free, including the 898-deep Activity leaf. No other vendor-grade closed taxonomy has this property. ats-eqt should make TRBC the *default* sector-attribution axis in its public API.
2. **NAICS-derived "GICS-equivalent" taxonomy is the second open hook.** Wharton's WRDS-INDCLASS macro and Weingarden's crosswalk give an academic-credentialed open mapping that ats-eqt can publish and maintain. Naming matters: "ats-eqt-Sector-v1 (open, NAICS-derived)" is legally and commercially clean; "GICS-equivalent" is sloppy.
3. **SIC remains mandatory because of EDGAR.** Every US 13F / N-PORT / 10-K pipeline reads SIC. Treat SIC as the *interop layer* with SEC data and NAICS as the *analytical layer*.
4. **Fama-French as the academic-respectability layer.** Ship FF12/FF17/FF30/FF48/FF49 alongside the SIC-derived primary classification. Zero marginal cost; significant value to research customers.
5. **RBICS-style multi-tag is the technical differentiator.** Even without licensing RBICS itself, an open competitor can replicate the multi-revenue-weight model by: (a) parsing 10-K segment data (`co_busseg` / `co_geoseg` in Compustat-shape) for primary-revenue-share, (b) augmenting with LLM-classified 10-K business descriptions, (c) calibrating against Wikidata-derived primary industry tags. This is the *one* place where an open product can match a closed vendor on capability, not just price.
6. **Bitemporal classification history.** GICS, ICB, BICS all revise their hierarchies regularly (every 5–10 years for sectors; continuously for sub-industries). A pre-2016 Berkshire that was GICS-Financials is post-2016 still GICS-Financials but with the Real Estate carve-out shifting the meaning. ats-eqt's `entity_classification` table's bitemporal columns let consumers query "what was the GICS-equivalent sector for entity X on date D in the v_2014 taxonomy vintage" — a feature that even some commercial vendors handle poorly.
7. **The "GICS revision risk" is real.** A backtest that uses *current* GICS labels for a 2008 sample period silently bakes in 18 years of taxonomy revisions. The 2016 REIT carve-out, 2018 Communication Services rebrand, and 2023 sub-industry shuffle all change which companies share a sector. ats-eqt's vintage-aware classification storage is a research-credibility feature.

---

## Part G — Open questions

1. **Exact BICS L2/L3/L4/L5/L6 counts as of 2026.** Bloomberg has not published an authoritative public count; the 49/196/712/2144 figures in the project brief are triangulated and tagged `[unverified]`. Worth pinning down from a Bloomberg DL+ catalogue snapshot if a friendly subscriber can supply one.
2. **RBICS exact code format.** FactSet's RBICS methodology guide describes the 6-level hierarchy but does not publish the digit layout. Worth confirming with a licensed customer.
3. **TRBC version drift between LSEG marketing, Wikipedia, and Equidam fact sheet.** Counts at L2-L5 differ across sources. The authoritative current count is in the LSEG TRBC Quick Reference PDF (<https://www.refinitiv.com/content/dam/marketing/en_us/documents/quick-reference-guides/trbc-business-classification-quick-guide.pdf>); the version effective as of 2026-05 needs a dated capture.
4. **ISIC Rev. 5 publication timeline.** Endorsed by UN Statistical Commission 2023; final publication and member-state adoption schedule is unclear. ats-eqt should plan for a rev. 5 ingest in 2026–2027.
5. **NAICS 2027 changes.** The 2027 revision is being scoped now (5-year cycle from 2022). The likely big change: AI-related services subdivisions. Worth monitoring the Federal Register for the 2025–2026 ECPC notices.
6. **Wikidata-as-primary-source feasibility.** If Wikidata's per-company `industry` (P452) coverage is dense enough to substitute for vendor classification for the long tail of micro-caps that vendors don't cover, the strategic value of the open-data product changes materially. Worth a coverage survey: what % of CIKs in EDGAR have a Wikidata Q-ID with a populated P452?
7. **EU CSRD / ESRS sector-classification adoption.** EFRAG's ESRS SEC 1 draft anchors on NACE Rev. 2.1; the timeline for issuer-level ESRS disclosure is still in flux. Worth tracking against ats-eqt's EU coverage roadmap.
8. **SEC SIC modernization?** There has been intermittent discussion within the SEC of migrating off SIC to NAICS for filer classification. As of 2026-05 no concrete proposal; worth watching.
9. **MSCI / S&P open-source posture.** Both vendors have made noises about an "open core" sector taxonomy as part of broader open-data initiatives. None have materialised; would change the competitive landscape if it does.
10. **CRSP-Compustat SIC divergence in newer vintages.** Older academic work (Guenther & Rosman 1994) documented ~36% disagreement. Whether the divergence has narrowed under modern automation is not publicly documented. Worth a fresh empirical check on the modern WRDS distribution.

---

## Part H — Sources

### GICS
- <https://en.wikipedia.org/wiki/Global_Industry_Classification_Standard> — Wikipedia overview
- <https://www.msci.com/our-solutions/indexes/gics> — MSCI GICS landing
- <https://www.spglobal.com/spdji/en/landing/topic/gics/> — S&P DJI GICS landing
- <https://www.msci.com/indexes/documents/methodology/1_MSCI_Global_Industry_Classification_Standard_GICS_Methodology_20240801.pdf> — MSCI GICS Methodology (Aug 2024)
- <https://www.spglobal.com/marketintelligence/en/documents/gics-direct-brochure.pdf> — GICS Direct product brochure
- <https://www.msci.com/documents/10199/5973a128-47f0-4317-b083-716a10207b50> — GICS FAQ
- <https://classification.codes/classifications/industry/gics/> — Classification.Codes GICS reference
- <https://www.westendadvisors.com/wp-content/uploads/2023/03/GICS-Sector-Revisions-Update-PRES-202303.pdf> — 2023 GICS revisions deck
- <https://fwcook.com/revisions-to-global-industry-classification-standard-gics-codes-to-be-implemented-in-march-2023/> — FW Cook on 2023 changes
- <https://www.indexologyblog.com/2023/01/27/gics-changes-are-approaching/> — S&P Indexology Blog on 2023
- <https://www.msci.com/documents/1296102/29559863/Implementation_of_2023_GICS_Changes.pdf/3b8bf06e-fe28-42f3-f6bc-73681bd3ec29> — MSCI implementation note

### ICB
- <https://en.wikipedia.org/wiki/Industry_Classification_Benchmark> — Wikipedia
- <https://www.lseg.com/en/ftse-russell/industry-classification-benchmark-icb> — LSEG ICB landing
- <https://www.lseg.com/content/dam/ftse-russell/en_us/documents/other/industry-classification-benchmark-product-overview-march-2020.pdf> — ICB overview (March 2020)
- <https://www.lseg.com/content/dam/ftse-russell/en_us/documents/other/icb-structure-and-definitions.xlsx> — ICB Structure and Definitions
- <https://www.lseg.com/content/dam/ftse-russell/en_us/documents/ground-rules/icb-ground-rules.pdf> — ICB Ground Rules (Mar 2026)
- <https://classification.codes/classifications/industry/icb> — Classification.Codes ICB reference
- <https://www.nasdaqtrader.com/TraderNews.aspx?id=fpnews2020-3> — NASDAQ migration to ICB in 2020

### TRBC
- <https://en.wikipedia.org/wiki/The_Refinitiv_Business_Classification> — Wikipedia
- <https://www.lseg.com/en/data-analytics/financial-data/indices/trbc-business-classification> — LSEG TRBC landing
- <https://www.refinitiv.com/content/dam/marketing/en_us/documents/quick-reference-guides/trbc-business-classification-quick-guide.pdf> — TRBC Quick Reference
- <https://www.equidam.com/resources/trbc-fact-sheet.pdf> — TRBC Fact Sheet
- <https://classification.codes/classifications/industry/trbc> — Classification.Codes TRBC reference
- <https://permid.org/> — LSEG PermID portal
- <https://developers.lseg.com/en/api-catalog/open-perm-id/permid-entity-search> — PermID Entity Search API
- <https://github.com/swanest/TRBC> — swanest/TRBC node library (open mapping)

### BICS
- <https://assets.bbhub.io/professional/sites/10/BICS-2024-Changes.pdf> — 2024 BICS changes
- <https://assets.bbhub.io/professional/sites/27/Bloomberg-Industry-Classification-System-BICS-Hierarchy-Change-June-2024.pdf> — June 2024 hierarchy change
- <https://www.thegoldensource.com/bloomberg-and-industry-classifications/> — GoldenSource BICS overview
- <https://data.bloomberglp.com/professional/sites/10/Classification-Data-Fact-Sheet.pdf> — Bloomberg Classification Data fact sheet
- <https://www.conseq.cz/getmedia/475278c1-bc71-4ca5-95ac-c9d5a1a7d014/Bloomberg-Global-Equity-Indices-Methodology-2312.pdf.aspx> — Bloomberg Global Equity Indices Methodology

### RBICS
- <https://insight.factset.com/resources/factset-revere-business-industry-classifications-datafeed> — FactSet RBICS at-a-glance
- <https://assets.ctfassets.net/lmz2w5z92b9u/67nHF3Io7Zg8Ka1eQSqWsi/73277f7a9bc6250c727c8625bdc55164/factset_rbics_methodology_guide.pdf> — RBICS Methodology Guide
- <https://aws.amazon.com/marketplace/pp/prodview-a7h773qqdu5mc> — FactSet RBICS on AWS Marketplace
- <https://developer.factset.com/api-catalog/factset-rbics-api> — FactSet RBICS API
- <https://doc.exabel.com/dsl/data_signals/factset_rbics.html> — Exabel RBICS docs
- <https://stoxx.com/thematic-indices/revenue-based-thematic-indices/> — STOXX revenue-thematic indices (RBICS-driven)

### NAICS / SIC / ISIC / NACE (public)
- <https://www.census.gov/naics/> — US Census NAICS landing
- <https://www.census.gov/eos/www/naics/concordances/concordances.html> — Census NAICS concordances
- <https://siccode.com/page/structure-of-naics-codes> — NAICS structure
- <https://www.bls.gov/ces/naics/> — BLS NAICS overview
- <https://en.wikipedia.org/wiki/North_American_Industry_Classification_System> — Wikipedia NAICS
- <https://en.wikipedia.org/wiki/Standard_Industrial_Classification> — Wikipedia SIC
- <https://www.sec.gov/search-filings/standard-industrial-classification-sic-code-list> — SEC SIC code list
- <https://www.osha.gov/data/sic-search> — OSHA SIC search
- <https://siccode.com/sic-code/9999/nonclassifiable-establishments> — SIC 9999 Nonclassifiable
- <https://generalliabilityinsure.com/sic-codes/8888-foreign-governments.html> — SIC 8888 Foreign Governments
- <https://www.bls.gov/emp/documentation/crosswalks.htm> — BLS classification crosswalks
- <https://unstats.un.org/unsd/classifications/Econ/isic> — UN Statistics Division ISIC
- <https://en.wikipedia.org/wiki/International_Standard_Industrial_Classification> — Wikipedia ISIC
- <https://ec.europa.eu/eurostat/web/nace> — Eurostat NACE landing
- <https://ec.europa.eu/eurostat/web/products-manuals-and-guidelines/w/ks-gq-24-007> — NACE Rev. 2.1 manual (2025 edition)
- <https://ec.europa.eu/eurostat/web/products-eurostat-news/w/wdn-20230210-1> — NACE Rev. 2.1 announcement
- <https://en.wikipedia.org/wiki/Statistical_Classification_of_Economic_Activities_in_the_European_Community> — Wikipedia NACE
- <https://classification.codes/classifications/industry/nace> — Classification.Codes NACE reference
- <https://efrag-website.azurewebsites.net/Assets/Download?assetUrl=%2Fsites%2Fwebpublishing%2FMeeting+Documents%2F2311080942079508%2F05-04+-+Sector+Classification+SEC+1+-+NACE+2-1+-+SR+TEG+240115.pdf> — EFRAG ESRS SEC 1 NACE 2.1 paper

### Fama-French / academic crosswalks / Wikidata
- <https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_48_ind_port.html> — Ken French 48-industry definitions
- <https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/changes_ind.html> — Ken French industry changes log
- <http://fmwww.bc.edu/repec/bocode/s/sicff.sthlp> — Stata sicff package
- <https://github.com/JoostImpink/fama-french-industry> — Fama-French SIC↔industry GitHub
- <https://ideas.repec.org/c/boc/bocode/s459550.html> — SIC_TO_FF Stata module
- <https://wrds-www.wharton.upenn.edu/pages/wrds-research/macros/wrds-macro-indclass/> — WRDS INDCLASS macro
- <https://sites.google.com/site/alisonweingarden/links/industries> — Weingarden GICS↔NAICS crosswalk
- <https://files.stlouisfed.org/files/htdocs/conferences/btn2014/docs/papers/stasch.pdf> — FRB vendor industry-code methodology
- <https://gist.github.com/mgao6767/4134ce36793b9e932a219ff07d7a3c7f> — mgao6767 multi-classification GitHub
- <https://arxiv.org/pdf/1706.04210> — "Open Source Fundamental Industry Classification" arXiv
- <https://www.wikidata.org/wiki/Property:P452> — Wikidata `industry` property
- <https://www.wikidata.org/wiki/Property:P3242> — Wikidata `SIC code` property
- <https://www.wikidata.org/wiki/Property:P3224> — Wikidata `NAICS code` property

### CRSP / Compustat SIC discrepancies
- <https://www.crsp.org/products/documentation/data-definitions-s> — CRSP data definitions S (HSICCD, SICCD, SICH)
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_Cross_Reference_Guide_1.0_to_2.0.pdf> — CRSP cross-reference guide
- <https://files.stlouisfed.org/files/htdocs/conferences/btn2016/docs/papers/hines.pdf> — FRB paper on financial data joins
- <https://www.semanticscholar.org/paper/Differences-between-COMPUSTAT-and-CRSP-SIC-codes-on-Guenther-Rosman/c0303ced40f51b32e930762f83d37918e969eebb> — Guenther & Rosman 1994 on SIC discrepancies
- <https://ideas.repec.org/a/eee/jaecon/v18y1994i1p115-128.html> — Guenther & Rosman J. Acc. Econ. citation

---

**Confirm:**

- File path: `c:/Users/natha/OneDrive/Desktop/C/ats/ats-eqt/research/datasets/reference_classifications.md`
- Section count: **8 top-level Parts** (0 Executive summary; 1 Why classifications matter; 2 Taxonomy stack matrix; A Per-taxonomy deep dives [10 sub-sections]; B Cross-walks [8 sub-sections]; C Public reconstruction; D Recommended ats-eqt schema [7 sub-sections]; E Licensing matrix; F Strategic positioning; G Open questions; H Sources).
- Date: 2026-05-14.
