# ats-eqt — Research Database

**Project:** ats-eqt — open-source-data competitor to FactSet, S&P Global Market Intelligence, Refinitiv (LSEG), and Bloomberg for **equity fundamentals** and **supply-chain** datasets, built in Python on top of the in-house **ats-core** C database.

**Research wave 1:** 2026-05-09. Six parallel research agents producing one file each. Vendor-of-record table + initial dataset & schema set.
**Research wave 2:** 2026-05-14. Eight parallel research agents producing field-level dataset deep-dives + a cross-vendor field map. Drove the database from ~5,400 lines to ~21,500 lines (~+350 unique URLs). Focused on **what fields to build and where each comes from**.
**Research wave 3:** 2026-05-17. Classical NLP / entity-resolution methodology for extracting fundamentals, relationships, and supply-chain facts from filings, releases, news, and shipment records without relying on expensive LLM inference.

This index is the entry point.

---

## How to read this database

```
research/
├── INDEX.md                                ← you are here
├── competitive_landscape.md                ← cross-vendor synthesis + strategic angles
├── vendors/                                ← incumbent vendor deep-dives
│   ├── factset.md
│   ├── sp_global.md
│   ├── refinitiv_bloomberg.md
│   ├── supply_chain_specialists.md
│   └── whale_wisdom.md                     ← 2026-05-16: mid-market 13F direct-replacement target
├── sources/                                ← upstream public/open data sources
│   └── public_data_sources.md
├── schemas/                                ← design reference for ats-eqt internals
│   ├── data_models_and_methodology.md
│   └── cross_vendor_field_map.md           ← wave-2: canonical concept → vendor field
├── datasets/                               ← per-dataset deep-dives (vendors + schema + public reconstruction for one dataset)
│   ├── 13f_holdings.md
│   ├── edgar_loader.md
│   ├── fundamentals_us_equities.md         ← wave-2: foundational fundamentals dataset
│   ├── estimates.md                        ← wave-2: I/B/E/S + FactSet + BEst + CIQ
│   ├── corporate_actions.md                ← wave-2: CRSP + DTCC + Form 8937
│   ├── pricing_market_data.md              ← wave-2: CRSP DSF + TAQ + retail APIs
│   ├── esg_sustainability.md               ← wave-2: MSCI/Sustainalytics/CSRD/SEC
│   ├── insider_ownership.md                ← wave-2: Form 4 + 13D/G + N-PORT
│   └── reference_classifications.md        ← wave-2: GICS/ICB/TRBC/BICS/NAICS/SIC
└── methodology/                            ← extraction and build methodology
    └── nlp_fact_extraction_supply_chain.md  ← wave-3: non-LLM filings/news/BOL extraction
```

If you only have time to read three files, read **competitive_landscape.md**, **schemas/data_models_and_methodology.md**, and **sources/public_data_sources.md** — those three carry the strategic and design payload.

For field-level engineering reference, the wave-2 **datasets/fundamentals_us_equities.md** (foundational) plus **schemas/cross_vendor_field_map.md** (the lookup table) are the daily-driver references.

---

## Vendor profiles

### [vendors/factset.md](vendors/factset.md) — FactSet
- **Stack:** FactSet Fundamentals (FF_BASIC / ADVANCED / DER / INFOTECH / REIT), Estimates, RBICS sector hierarchy, **Revere** supply-chain graph, Truvalue ESG, Concordance API, FSYM symbology, **CGS / CUSIP** (acquired 2022 for $1.925B).
- **Scale:** FY2025 revenue $2.32B, ASV $2.41B.
- **Headline finding:** Revere supply-chain graph is small (~25k entities, ~144–270k edges) — within scope for a filings-NER + customs open-data competitor. PIT methodology (`estimate → preliminary → originally reported → restated` lifecycle with knowledge dates) is well-documented and the principal asset to clone. Concordance API uses TF-IDF over character trigrams — directly reproducible.

### [vendors/sp_global.md](vendors/sp_global.md) — S&P Global Market Intelligence
- **Stack:** **Compustat** XPF (CO_FNDQ, CO_FNDA, CO_AFND, CO_IDESIND), **Capital IQ Pro** with EAV `ciqFinInstanceItem` long-format model, **Panjiva** supply chain (US CBP AMS + 14+ countries), GICS (joint with MSCI), GVKEY + CIQ identifiers.
- **Scale:** Compustat history to 1950, snapshot PIT to 1987, Panjiva ~10–15M US shipments/yr.
- **Headline finding:** Compustat's true moat is the **standardized item dictionary + Snapshot PIT** (not the raw data), enforcing `pdate ≤ rdq ≤ fdate ≤ ldate` semantics. Capital IQ uses long-format EAV (replicable). Panjiva's underlying US data is **FOIA-public** — moat is entity resolution and HS-code text imputation.

### [vendors/refinitiv_bloomberg.md](vendors/refinitiv_bloomberg.md) — LSEG (Refinitiv) + Bloomberg
- **Refinitiv stack:** Worldscope fundamentals (numeric WS items + `WS.*` mnemonics), Datastream PIT/vintages, **I/B/E/S** detail+summary estimates, LSEG Workspace, **PermID** (open) + RIC (closed), **TRBC** sector taxonomy.
- **Bloomberg stack:** Terminal fundamentals + BEst, **BICS** sector, **SPLC** supply chain, BPipe / Data License / DL+ (Snowflake), **FIGI** (open), Bloomberg Global ID.
- **Headline finding:** PermID + OpenFIGI + TRBC are the three open hooks an open-source competitor can lean on; **BICS and SPLC monetization are the clearest moats Bloomberg keeps closed**. Bloomberg Terminal ≈$28k/yr/seat sets the price umbrella.

### [vendors/whale_wisdom.md](vendors/whale_wisdom.md) — WhaleWisdom (mid-market 13F)
- **Stack:** Privately-held SaaS aggregator of SEC EDGAR — 13F-HR / 13F-HR/A, Schedule 13D/G (since 2006), Form 3/4/5, Form ADV, Form D, N-SAR. Single web product + Excel Add-in + signed HMAC-SHA1 REST API at `/shell/command` (12+ commands, 20 req/min). Internal opaque `filer_id` / `stock_id` keyspace.
- **Scale:** Founded 2008. Self-described as fully automated ingestion with no human content review. History to 2001-Q1 for 13F; to 2006 for 13D/G.
- **Pricing:** Free / Standard $300/yr / Pro $500/yr / Enterprise custom (unlimited API + nightly FTP).
- **Headline finding:** The single clearest *direct-replacement* target for ats-eqt's 13F line. The moat is **API + UI + analytics (WhaleScore, Backtester) + entity-resolved ID keyspace**, not data acquisition. A 6-engineer-month sprint produces feature parity; 12 months yields a strict superset by adding **bitemporal PIT**, **N-PORT integration**, and **FIGI-only redistributable distribution** — three wedges WhaleWisdom structurally cannot match. Companion crawler: [scripts/whalewisdom_crawl.py](../scripts/whalewisdom_crawl.py).

### [vendors/supply_chain_specialists.md](vendors/supply_chain_specialists.md) — Supply-chain pure-plays
- **Vendors:** Resilinc (contributory NDA network), Interos (operational resilience), Everstream Analytics (predictive risk), Sayari Graph (CBP partnership, beneficial ownership), Kpler (commodities/AIS), ImportYeti / ImportGenius / Datamyne / Descartes (BoL aggregators), Z2Data (electronics components), Exiger / Supplier.io (third-party risk).
- **Headline findings:**
  - The three "incumbent" databases (FactSet Revere / Bloomberg SPLC / Panjiva) **only share ~43% of suppliers** (Culot 2023) — no one has the "real" graph.
  - **HS code is NOT in the public US AMS feed** — only "Description of Goods" free-text. All Panjiva-style HS classifications are NLP-inferred → structural opportunity for better imputation.
  - Resilinc is fundamentally different from Panjiva — a *contributory data network* (suppliers self-attest under MNDA), not customs aggregation.
  - **GLEIF + OpenCorporates + libpostal + CBP FOIA + Eurostat Comext + UN Comtrade gets ~80% of Panjiva-grade coverage**; the remaining 20% is non-US shipment-level licensing + entity-resolution quality.

---

## Per-dataset deep-dives

### [datasets/13f_holdings.md](datasets/13f_holdings.md) — 13F institutional holdings
- **Scope:** SEC Form 13F-HR / 13F-HR/A / 13F-NT primary source (XML INFORMATION TABLE) + commercial vendor stack (FactSet Ownership, S&P CIQ Pro Ownership, Bloomberg HDS/PHDC, Refinitiv eMAXX/Lipper) + mid-tier (WhaleWisdom, 13F.info, Symmetric, Holdings Channel) + API plays (Fintel, Quiver Quantitative) + bulk/academic (Thomson Reuters s34 via WRDS).
- **Recommended ats-eqt schema:** three new bitemporal tables — `filer_13f`, `filing_13f`, `holding_13f` — fitting the canonical long-format EAV pattern. CUSIP held in `security_alias` with `redistributable=FALSE`; FIGI on the public API.
- **Headline findings:**
  - **2023-01-03 unit cutover** — `<value>` switched from $thousands to actual dollars at SEC compliance date. Filings with `periodOfReport ≤ 2022-12-31` need a ×1000 multiplier; later filings do not. Silent ingestion bug if missed.
  - **CUSIP licensing is the load-bearing legal question.** SEC explicitly permits FIGI as the redistributable identifier since June 2022 amendments. ats-eqt should adopt FIGI publicly and keep CUSIP only in a non-redistributable internal alias.
  - **Form SHO / 13F-2 short-position data is not happening soon** — December 2025 SEC exemption order extended compliance to 2028-01-02; first filings due 2028-02-14. Don't build before late 2027.
  - **N-PORT integration is the real enterprise differentiator** — manager-level 13F vs fund-level N-PORT (now monthly post-Aug 2024 amendments) reveals SMA positions, non-13(f) holdings, and timing offsets. This is FactSet/S&P's structural value-add.
  - **WhaleWisdom benchmark:** $300/yr Standard, $500/yr Pro, custom Enterprise (with API + nightly FTP). API rate-limited to 20 req/min. ats-eqt's 13F sits naturally between WhaleWisdom (free–$500) and FactSet/S&P (enterprise $50k+).

### [datasets/edgar_loader.md](datasets/edgar_loader.md) — EDGAR ingestion pipeline
- **Scope:** SEC EDGAR retrieval mechanics (full-text search, full-index, RSS, REST/submissions, `companyfacts.json`, Daily Index) + rate-limit conformance + parsing tools (Arelle, EdgarTools, edgar-crawler) + the staging-area pipeline shape ats-eqt should run.
- **Headline finding:** EDGAR rate-limit is 10 req/s per IP with mandatory `User-Agent` containing a real contact email; non-conformance results in IP blocks. The bulk Daily Index + Archives is the right primary path; the REST API is supplementary for `companyfacts.json` lookups.

### [datasets/fundamentals_us_equities.md](datasets/fundamentals_us_equities.md) — US equity fundamentals (foundational)
- **Scope:** Compustat NA (XPF: `funda`/`fundq`/`co_idesind`/`secm`/`segfnd`/`idxcst_his`/Snapshot pit_*), FactSet Fundamentals (FF_BASIC/ADVANCED/DER/INFOTECH/REIT/BANK/INS/PIT), Worldscope (numeric `WSxxxxx` + `WS.*`), Bloomberg FA/BQL (`IS_*`/`BS_*`/`CF_*`/`FA_*`/`BEST_*`/`COFI`), S&P Capital IQ (`ciqFinInstanceItem` EAV with full dimension model), Sharadar SF1, SimFin, WRDS academic distribution. **147 us-gaap concepts mapped** to vendor fields with industry overlays (banks/insurance/REIT/Nareit FFO).
- **Recommended ats-eqt schema:** long-format EAV (`fact` keyed by `entity × period × item × bitemporal_dates`), `xbrl_concept_map` for us-gaap → canonical, `entity.industry_template` field gating ingestion routing for bank/insurance/REIT extension taxonomies.
- **Headline findings:**
  - **~85% of headline Compustat annual mnemonics are mechanically reconstructable from us-gaap XBRL** (>95% for top 20 line items); remaining 15% needs footnote NLP + industry-specific taxonomies + pre-2009 backfill.
  - **The 8-K Item 2.02 → 10-Q latency window (avg ~14 days) is the most under-modelled PIT detail outside Compustat** — every fact row needs the four-date model (data_date, report_date, filing_date, knowledge_from).
  - **Sharadar SF1 at $540/yr is the closest existing low-cost Compustat clone** with both ARQ (as-reported) and MRQ (restated) vintages — the open-data competitive baseline ats-eqt must beat.
  - **CIQ's `ciqFinInstanceItem` EAV long-format is the right schema shape**, not Compustat's wide table — scales to 10K+ items and gives natural bitemporal versioning.
  - **Banks/insurance/REITs each require a distinct industry-template overlay** that is not part of core us-gaap.

### [datasets/estimates.md](datasets/estimates.md) — Analyst estimates
- **Scope:** I/B/E/S (LSEG; Detail + Summary + Recommendations + Guidance), FactSet Estimates (FE_BASIC/FE_ADVANCED), Bloomberg BEst, S&P Capital IQ Estimates, Visible Alpha (now under CIQ), plus defunct/alternative (Estimize, TipRanks, Zacks) + public sources (8-K Item 2.02/7.01, Wall Street Horizon, AlphaSense/Tegus transcripts).
- **Recommended ats-eqt schema:** `est_detail` (estimator-level facts) + `est_summary` (consensus aggregates) + `est_actual` + `est_guidance` (8-K extracted) + `est_recommendation` + `broker` + `measure_dim` + `period_dim`.
- **Headline findings:**
  - **All vendor schemas converge to the same long-format triple** `(broker_id, analyst_id, measure, period, ts, value)` — ats-eqt's `est_fact` table can hold all five vendors with only dimension-table translation.
  - **IBES broker-ID reshuffling** (2018 vintage: 13.8% broker IDs / 30.7% analyst IDs reassigned) is the single most citation-breaking gotcha in academic replication; vintage-versioned alias table is the only defensible design.
  - **No us-gaap XBRL element exists for forward-looking guidance** — must come from prose extraction of 8-K exhibits and transcript NER.
  - **Bloomberg inverts the IBES recommendation scale** (Bloomberg 5=Buy, IBES 1=Buy) — the most common silent cross-vendor reconciliation error.
  - **Visible Alpha integration with CIQ Pro in 2025** subsumes VA's separate IDs into CIQ symbology; line-item granularity (avg 156 items/company) is its remaining differentiator.

### [datasets/corporate_actions.md](datasets/corporate_actions.md) — Splits, dividends, M&A, spinoffs
- **Scope:** CRSP (`dsedist`/`dsedelist`/`dsenames`/`ccmxpf_lnkhist` — fully-public DISTCD + DLSTCD enumerations), FactSet (FSYM_CA lineage + Adjustments Feed), Bloomberg (DVD/EQY_SPLIT/MA), Compustat/CCM, LSEG Datastream + Workspace, **DTCC Corporate Actions ISO 20022 with CAEV codes**, NYSE/Nasdaq/ICE/EDI WCA direct feeds, OpenFIGI / Yahoo / Databento.
- **Recommended ats-eqt schema:** 10 DDL tables (`corp_action`, `corp_action_type_dim`, `adjustment_factor`, `delisting`, `delist_code_dim`, `name_history`, `ticker_history`, `spinoff_basis_allocation`, `offering`, `trading_halt`).
- **Headline findings:**
  - **CRSP is the only fully-public field-level methodology** — ats-eqt should mirror DISTCD/DLSTCD enumerations literally.
  - **DTCC CA 20022 is the issuer-of-record substrate** (~1.3M securities, 16 file slices/day).
  - **Form 8937 / IRC §358 is the only public path to spinoff cost-basis allocation**, discoverable via EDGAR 8-K Item 8.01 wrappers.
  - **Yahoo's adjusted-close is documented broken for combined split+dividend periods** — ats-eqt must compute its own.
  - **FIGI survives ticker change but not share-class change**; CUSIP-9 changes on issue-suffix change. Bitemporal `name_history`/`ticker_history` mirror `dsenames`.

### [datasets/pricing_market_data.md](datasets/pricing_market_data.md) — OHLCV + adjustments + total return + short interest
- **Scope:** CRSP DSF/MSF/DSI (1925/1962-present), Compustat CCM (`co_secd`, `sec_dprc`, `sec_dtrt`, `ccmxpf_linktable`), FactSet (`FF_SEC_PRICES`), Bloomberg (Terminal/BPipe/DL+), LSEG/Refinitiv (Datastream + Tick History), NYSE Daily TAQ, retail APIs (Polygon $29–499/mo, Tiingo $10–50/mo, Alpaca, IEX Cloud legacy), Yahoo/yfinance, FINRA bi-monthly + daily short volume, S3 Partners, IHS Markit/S&P Securities Finance, OPRA-options (ORATS, IVolatility, CBOE LiveVol, Databento).
- **Recommended ats-eqt schema:** 6 DDL tables (`bar_daily`, `bar_intraday`, `quote_eod`, `shares_outstanding_history`, `short_interest`, `adjustment_factor_history`).
- **Headline findings:**
  - **CRSP DSF remains the academic standard** primarily for its **delisting-return treatment** (`dlret` + Shumway-Warther); no other vendor handles delistings PIT-correctly.
  - **"Adjusted close" has three different semantics across vendors** — CRSP/Compustat ship separate price + total-return factors; Yahoo/Tiingo conflate into a single column; PIT-incorrect by 5–30% on dividend-heavy names.
  - **Retail APIs (Polygon/Tiingo, post-2023) are good enough for forward daily bars** but deep pre-2003 history still requires WRDS/CRSP.
  - **Intraday is the cost cliff** — daily is commoditised; full TAQ-equivalent tick is still 6-figure or per-TB billing.
  - **Manager-level short positions remain unobservable until Form SHO 13f-2** first filings 2028-02-14; FINRA bi-monthly security-level is the only free path until then.

### [datasets/esg_sustainability.md](datasets/esg_sustainability.md) — ESG scores + regulatory disclosures
- **Scope:** MSCI ESG Ratings, S&P Global CSA/DJSI, Sustainalytics (Morningstar), Bloomberg ESG, Refinitiv/LSEG ESG, ISS ESG, FactSet Truvalue, CDP, GRESB; plus **17 regulatory frameworks** (SEC 10-K Item 1/1A, Exhibit 21, Form SD, SEC Climate Rule with full death-spiral timeline, EU CSRD/ESRS with 12 standards and 1,144+269 datapoints, EU SFDR Article 8/9 + 18 PAI, UK SDR, Japan TCFD/SSBJ, California SB 253/SB 261, CSDDD, DEF 14A, GHG Protocol, GRI, SASB/ISSB, TCFD).
- **Recommended ats-eqt schema:** `esg_metric_dim`, `esg_metric`, `esg_score`, `esg_controversy`, `ghg_emission` + convenience materialised views.
- **Headline findings:**
  - **Vendor ESG ratings are functionally uncorrelated** (Berg/Kölbel/Rigobon: pairwise 0.38–0.71 vs ~0.99 for credit). 56% measurement, 38% scope, 6% weight — measurement is irreducible. **No "true" ESG signal exists.**
  - **SEC Climate Disclosure Rule is dead at federal level** (5th Cir stay Mar 2024 → SEC voluntary stay Apr 2024 → SEC ended defense Mar/Apr 2025).
  - **CSRD/ESRS has crystallised as the "GAAP for ESG":** 12 standards, 1,144 mandatory + 269 voluntary datapoints, iXBRL-tagged.
  - **California SB 253 first Scope 1+2 deadline 2026-08-10** is now the operative US climate disclosure anchor.
  - **ats-eqt's wedge: raw disclosed metrics + bitemporal evidence trail**, NOT another opinionated score; vendor scores join in as `redistributable=FALSE` overlays.

### [datasets/insider_ownership.md](datasets/insider_ownership.md) — Form 4 + 13D/G + N-PORT + Form 144
- **Scope:** SEC Form 3/4/5 (Section 16 insiders), Schedule 13D/13G (5%+ blockholders post-2024-rule), Form N-PORT (fund-level monthly holdings), Form 144 (restricted-stock sales), Form N-PX (proxy voting), Schedule TO/14D-9 (tender offers), STOCK Act PTRs (congressional); vendors: FactSet Ownership, S&P CIQ Pro Ownership (`ciqOwnership*`), Bloomberg HDS/PHDC/OWN, LSEG Stock Ownership + Lipper + eMAXX, WhaleWisdom Premium, Quiver Quantitative, OpenInsider, InsiderInsights/2iQ/Form4Oracle/Verafin, Senate/House Stock Watcher, capitoltrades.com.
- **Recommended ats-eqt schema:** 11+ DDL tables (`insider`, `insider_relationship`, `insider_transaction`, `filing_form4`, `blockholder_filing`, `blockholder_reporting_person`, `fund`, `fund_class`, `filing_nport`, `fund_holding`, `form144_intent`, `form144_to_form4_link`, `tradingplan_10b5_1`, `proxy_vote`, `congressional_disclosure`).
- **Headline findings:**
  - **Full 28-letter Form 4 `transactionCode` enumeration documented** — P highest signal; S contaminated by 10b5-1; F/M netting pattern; J requires footnote NER.
  - **Schedule 13D filing deadline collapsed to 5 business days from 10 calendar days** (effective 2024-02-05); 13D/G structured-data XML mandate effective 2024-12-18 creates two parsing pipelines (pre/post).
  - **N-PORT monthly-public-availability rule (Aug 2024) was delayed to 2027-11-17 / 2028-05-18** by April 2025 extension order; February 2026 SEC proposal may scale it back further — moving target.
  - **Rule 10b5-1 amendments (Dec 2022)** added Form 4 cover-page `rule10b5-1Indicator` checkbox + plan-adoption date from 2023-04-01; 90/120-day officer cooling-off period.

### [datasets/reference_classifications.md](datasets/reference_classifications.md) — GICS / ICB / TRBC / BICS / NAICS / SIC / RBICS
- **Scope:** All 10 major taxonomies — GICS (S&P+MSCI, closed), ICB (LSEG, closed), TRBC (LSEG, **open via PermID**), BICS (Bloomberg, closed), RBICS (FactSet, multi-tag revenue-weighted), NAICS (Census, open), SIC (BLS-frozen 1987 + SEC's modified fork), ISIC (UN, open), NACE (EU + CSRD overlay), Fama-French 12/17/30/48/49 (academic, open), BEA, MSCI ACWI, FTSE-ICB, ESMA MiFID, Wikidata P452/P3224/P3242.
- **Recommended ats-eqt schema:** `taxonomy`, `taxonomy_node`, `entity_classification` (with revenue-weight column for multi-tag), `taxonomy_mapping` (many-to-many crosswalks), `sic_code_dim`/`naics_code_dim`.
- **Headline findings:**
  - **TRBC-via-PermID is the only deep, index-grade, fully-open taxonomy** — the strategic open hook.
  - **All four "real" institutional taxonomies (GICS/ICB/TRBC/BICS) are closed at the redistribution boundary** — must mirror the CUSIP carve-out pattern.
  - **SEC has never migrated off 1987-frozen SIC**, and its fork (8888 Foreign Governments, 9995/9999 Nonclassifiable, 6770 SPAC Blank Checks) differs materially from BLS canonical — every EDGAR pipeline must carry a `(sic_code, authority)`-keyed dim table.
  - **RBICS's revenue-weighted multi-tag model is the one technical capability** an open product can match by combining 10-K segments + LLM 10-K classification + Wikidata.
  - **All taxonomy cross-walks are many-to-many** (Weingarden GICS↔NAICS maps 144 GICS sub-industries to 989 NAICS-6 codes; CRSP↔Compustat SIC disagree on ~36% of jointly covered firms).

---

## Public / open data sources

### [sources/public_data_sources.md](sources/public_data_sources.md) — 44+ uniform source cards
Categories:
1. **Equity fundamentals (filings + structured):** SEC EDGAR, DERA Financial Statement Data Sets, `companyfacts.json` API, Frames API, XBRL US, UK Companies House, ESEF / IFRS, Japan EDINET / TSE TDnet, HKEXnews, SSE/SZSE, SEDAR+, ASX.
2. **Identifiers / entity reference:** GLEIF LEI (free), PermID (free tier), OpenFIGI (MIT), OpenCorporates (priced commercial), EDGAR CIK, Wikidata/DBpedia (CC0), national company registries.
3. **Sector / industry:** NAICS, SIC, ICB/GICS (commercial), ISIC/NACE.
4. **Supply chain / trade:** US CBP AMS, US Census USA Trade Online, UN Comtrade, Eurostat Comext, OECD TiVA, India ICEGATE/Zauba, Brazil + LatAm customs, AIS feeds, WTO TAO, WCO HS/GHS.
5. **Corporate disclosures (non-financial):** EU CSRD/ESRS, CDP, Exhibit 21, Form SD conflict minerals, customer concentration (10-K Item 1), GHG/Scope 3.
6. **Estimates alternatives:** Estimize (defunct), Wall Street Horizon, transcripts, analyst targets.
7. **Macro:** FRED, World Bank, IMF.

**Key strategic finding:** the genuine open-data moat is **NOT fundamentals** (where SEC XBRL gets 75–85% of Compustat coverage with effort) but the **supply-chain graph** built from EDGAR Exhibit 21 + 10-K Item 1 customer concentration + CBP AMS + AIS + GLEIF/OpenFIGI — a combination no major vendor sells natively at the parent-attributable layer. The only **fully-redistributable identifier triple** under clean licenses is **GLEIF (free) + OpenFIGI (MIT) + Wikidata (CC0)**; OpenCorporates is £12k+/yr commercial and cannot serve as the public spine.

---

## Internal design reference

### [schemas/data_models_and_methodology.md](schemas/data_models_and_methodology.md) — Design reference v0.1 (~8,300 words)
Sections:
- **0.** Executive summary — five non-negotiable design choices
- **A.** Fundamentals schemas (long vs wide, PIT vs as-reported vs restated, bitemporal, period definitions, restatement audit)
- **B.** Estimates schemas (I/B/E/S detail vs summary, broker IDs, MEASURE codes, FPI, dates)
- **C.** Supply-chain graph schemas (entity/edge taxonomy, multi-tier expansion, confidence, bitemporal graph, entity resolution)
- **D.** Identifier / symbology / concordance (FIGI, LEI, PermID, FSYM, GVKEY, CIK, alias DDL, corporate actions, survivorship)
- **E.** Data quality / collection methodology (XBRL pipeline, Arelle, NLP, QA, late-arriving, coverage targeting)
- **F.** Storage / serving (Snowflake share, Parquet, kdb+, Postgres, Neo4j, **ats-core mapping**)
- **G.** Recommended ats-eqt internal schema — consolidated DDL: `entity / security / listing / id_alias / fund_item / period / fact / est_* / sc_* / corp_action / filing` plus mermaid ER + bitemporal lifecycle diagrams
- **H.** Implementation phasing — sequenced 18-month build

This is the file the engineering team should treat as canon when designing ats-eqt's storage layer, ingestion pipeline, and serving APIs.

### [schemas/cross_vendor_field_map.md](schemas/cross_vendor_field_map.md) — Canonical concept → vendor field (wave-2 synthesis)
938-line consolidated lookup table synthesizing wave-1 + wave-2 docs. ~480 data rows across 8 master tables (Fundamentals, Estimates, Corporate Actions, Pricing, Identifiers, Industry Classifications, ESG, Ownership) plus an 85-row canonical `ats-eqt` item dictionary (the "if you only build one table" reference keyed off `item_id`).
- **Headline findings:**
  - **No single vendor covers all eight datasets** — best-of-breed mapping is required.
  - **Sign-convention inversions are the silent integration bug** (Bloomberg `BEST_ANALYST_RATING` inverts IBES `ireccd`; Sustainalytics ESG Risk Rating inverts MSCI ESG Rating).
  - **Period-indicator encoding (FPI/FPERIOD/estimatePeriodType) is non-aligned across IBES/FactSet/Bloomberg/CIQ** — equality joins silently misalign.
  - **CUSIP licensing forces FIGI as the only safe public spine.**
  - **Unit-discontinuities silently break ~30% of cross-vendor SUMs** (13F `<value>` switched from $thousands to $actual on 2023-01-03; Compustat $millions; Sharadar/SimFin $actual). Explicit unit-multiplier layer is mandatory.

### [methodology/nlp_fact_extraction_supply_chain.md](methodology/nlp_fact_extraction_supply_chain.md) — Classical NLP fact extraction (wave-3)
- **Scope:** Non-LLM extraction methodology and implementation blueprint for EDGAR filings, XBRL-adjacent prose, 8-Ks, press releases, financial news, and bill-of-lading/customs records.
- **Headline findings:**
  - Use **XBRL first** for standard numeric fundamentals; use NLP for relationships, customer concentration, product/input exposure, operational facts, events, and evidence-linked relationship edges.
  - The vendor pattern is reproducible: public/regulatory documents + shipment records + entity resolution + rule extraction + weak supervision + analyst QA + point-in-time evidence trails.
  - Start with **major customers and customer concentration** because the language is formulaic, source-backed, and high-value.
  - Entity resolution is the core moat; FactSet-style TF-IDF character trigrams, aliases, attributes, and review queues are enough for a strong v1.
  - Keep disclosed relationships separate from inferred shipment edges; they answer different questions and have different error modes.
  - The implementation extension now includes fact contracts, pseudocode, CRF/NER feature templates, relation classifiers, weak-supervision labeling functions, confidence scoring, QA loops, and publishable table/view shapes.

---

## Cross-cutting findings

These are the recurring threads worth flagging at the top of any planning conversation:

1. **Compustat's PIT semantics are the key technical asset to replicate.** Every fundamentals fact must be stored with `data_date`, `report_date (RDQ)`, `first_available_date (FDATE)`, and `last_modified_date (LDATE)` so that both as-of-knowledge and as-of-economic-time queries are exact. Bitemporal modeling (valid time + transaction time) is the right pattern.
2. **Long-format EAV (Capital IQ style) beats wide-format (Compustat XPF style) for the modern build.** Higher-cardinality items, easier extension, and a natural fit for columnar storage.
3. **The supply-chain graph is the strategic wedge, not fundamentals.** Vendors only overlap ~43% on supplier identity — no one has the truth. Combining Exhibit 21 + Item 1 + CBP AMS + AIS + GLEIF gives unique parent-attributable lineage that vendors don't sell.
4. **CUSIP changed hands in 2022.** S&P sold CUSIP/CGS to FactSet for $1.925B. ats-eqt must avoid CUSIP at the spine and lean on **CIK + LEI + FIGI + ISIN** instead.
5. **GICS is jointly licensed by S&P + MSCI.** Either license, or use NAICS / FTSE-ICB / a homegrown taxonomy. TRBC (Refinitiv) is open via PermID and is a more attractive starting point than NAICS for finance use cases.
6. **HS codes in US AMS data are NLP-inferred, not declared.** Industry-wide false precision. Better imputation (modern LLM/NER) is a concrete advantage ats-eqt could ship in v1.
7. **Bloomberg Terminal pricing (~$28k/yr/seat) sets the umbrella.** ats-eqt does not need to undercut by 10× to win; sub-$5k/seat with API-first delivery and clean licenses is highly disruptive in mid-market and academia.

See [competitive_landscape.md](competitive_landscape.md) for the full vendor matrix and the ats-eqt strategic positioning statement that follows from these findings.

---

## Sources

Each file ships its own source list. Combined:
- **Wave 1 (2026-05-09):** ~350 unique URLs.
- **Wave 2 (2026-05-14):** **+892 additional unique URL citations** across the 8 new files (fundamentals 112, estimates 115, corporate-actions 122, pricing 116, ESG 110, insider-ownership 128, classifications 74, cross-vendor map 63 + 64 carried `[unverified]` provenance markers).
- **Wave 3 (2026-05-17):** NLP / supply-chain extraction methodology covering SEC APIs/XBRL, FactSet Revere, Bloomberg SPLC, Panjiva, ImportYeti, Datamyne, CBP vessel manifests, LSEG/RavenPack news analytics, Loughran-McDonald, GLEIF/OpenCorporates/OpenFIGI, and weak-supervision tooling.

Combined database: vendor docs, academic guides (WRDS, Stanford GSB, Tilburg, Tidy Finance, Berg/Kölbel/Rigobon, Shumway-Warther), open-source projects (XBRL US, EdgarTools, OpenBB, SimFin, Sharadar, Arelle, edgar-crawler), bitemporal-modeling references, supply-chain academic literature, identifier-system specs, SEC rule dockets (Climate Disclosure, Schedule 13 modernization, N-PORT amendments, Form SHO), EU regulatory texts (CSRD/ESRS, SFDR, CSDDD), California climate disclosure statutes.

Items flagged `[unverified]` could not be cross-confirmed; the cross-vendor field map carries provenance through so the same cell stays flagged in synthesis. Wave-3 should retire as many `[unverified]` flags as possible.

---

## Open questions for wave 3

Wave-2 retired most of wave-1's "PDFs that WebFetch couldn't parse" by sourcing the field schemas through alternate vendor-developer-portal / WRDS / academic-replication routes. New open questions surfaced in wave 2:

**Vendor / pricing**
- Vendr / G2 per-seat pricing confirmation for Capital IQ Pro, FactSet Workstation, LSEG Workspace, Bloomberg Terminal (~$28k/seat is well-cited but not confirmed) — still loose.
- Latency SLAs for Snowflake / Marketplace share products (S&P, FactSet, Bloomberg) — relevant for ats-eqt's serving guarantees.

**SEC rule moving targets** (verify before any reader path is wired)
- **N-PORT monthly public availability** rule (Aug 2024) was delayed to 2027-11-17 / 2028-05-18 by April 2025 extension; February 2026 proposal may scale it back further.
- **Form SHO / 13f-2 short-position** rule — compliance extended to 2028-01-02, first filings 2028-02-14.
- **SEC Climate Disclosure** rule status — federally dead post-Apr 2025; California SB 253 first Scope 1+2 deadline 2026-08-10 is now the operative US anchor.
- **Schedule 13D/G structured-data XML mandate** effective 2024-12-18 — verify post-effective-date filings actually use XML (vs pre-2024 HTML).

**Internal-schema specifics**
- **Confidence-score scales** for FactSet Revere and Bloomberg SPLC — methodology pages remain thin.
- **BICS / RBICS exact leaf counts** and code formats (vendor docs gated).
- **TRBC version drift** — LSEG bumps quietly; need vintage-tracking strategy.
- **ESRS Omnibus simplification** status (proposed Feb 2025; may collapse mandatory datapoints).
- **CDP 2025 API schema** — pending public release.
- **CSDDD member-state implementation acts** — timeline per country.

**Cross-vendor reconciliation projects**
- **Vendor close-price reconciliation regression** — measure systematic disagreement of Bloomberg vs LSEG vs CRSP closes on the same security/day.
- **PIT integrity of free APIs** (Polygon, Tiingo, Alpaca) — measure look-ahead bias in adjusted closes.
- **CRSP↔Compustat SIC divergence regression** for 1990–2025.
- **Panjiva exact field count** + the carry-forward S&P 10-K Mobility revenue split.

**Non-US fundamentals**
- **EDINET / HKEX / SSE/SZSE / SEDAR+ / ASX** rate limits + XBRL adoption maturity — country-by-country.
- **ESEF (EU mandatory iXBRL)** retail-issuer coverage rollout.

**Identifiers / mapping**
- **GLEIF coverage of private issuers** — LEI is excellent for funds and banks; mid-market private coverage thin.
- **PermID Quote vs Instrument level** mapping for ADRs — pending.
