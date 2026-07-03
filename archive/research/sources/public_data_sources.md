# ats-eqt — Public Data Sources Catalog

**Purpose:** strategic catalog of every relevant free or low-cost upstream data source for an
open-source equity-fundamentals + supply-chain data vendor competing with FactSet, S&P Global,
Refinitiv (LSEG), and Bloomberg. Each source is summarized in a uniform card so the ats-eqt
ingestion team can prioritise build order, license risk, and quality cliffs.

Last updated: 2026-05-09. Statements marked `[unverified]` are best-effort but not directly
confirmed against primary docs in this pass.

---

## 1. Equity fundamentals — filings + structured

### SEC EDGAR (full-text search, filing index, RSS, bulk archives)
- **URL / API endpoint:** `https://www.sec.gov/cgi-bin/browse-edgar`, RSS feeds, full-text search at `https://efts.sec.gov/LATEST/search-index?q=...&forms=...`, bulk archives under `https://www.sec.gov/Archives/edgar/`.
- **What it provides:** Every filing submitted to the SEC since 1993/1994 — 10-K, 10-Q, 8-K, 20-F, 40-F, 6-K, S-1, DEF 14A, 13F, 13D/G, Form 4, Form SD, Form ADV, NPORT-P, etc. Full-text search returns hit IDs across exhibits. Daily index files (`master.idx`, `form.idx`) provide every filing accessioned per day; full RSS at `https://www.sec.gov/cgi-bin/browse-edgar?action=getcurrent`.
- **Schema / format:** SGML/XML submissions; primary documents are HTML/iXBRL; exhibits are typically HTML, PDF, or XLSX. Indexes are pipe-delimited text. RSS is Atom 1.0. Full-text search returns JSON.
- **Volume:** ~25M filings cumulative; ~3,000–8,000 filings per business day; tens of TB of raw archives.
- **History depth:** 1993-present (some pre-1993 paper filings scanned but not text-extracted).
- **Refresh cadence:** Real-time during EDGAR business hours (filings appear minutes after acceptance); index files rebuilt nightly.
- **License / terms:** Public domain (US Government work, 17 USC §105). No re-use restriction. SEC requires only a descriptive User-Agent identifying app + contact email; absence returns 403.
- **Rate limits:** 10 requests/second/IP, no daily cap. Sustained abuse triggers ~10-minute IP block. Requests must include `User-Agent: AppName contact@email`. (source: https://www.sec.gov/about/developer-resources, https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data)
- **Quality issues:** Iterative amendments (10-K/A) require ordering by accession; restatement signal is implicit, not flagged. Exhibit cross-references inconsistent across periods. Full-text search occasionally lags 10–60 minutes vs. filing index. iXBRL coverage uneven for smaller registrants.
- **Strategic value for ats-eqt:** **Critical / foundational.** Without EDGAR there is no US fundamentals product. Combined with companyfacts/frames it is the spine of the entire offering.
- **Source URL:** https://www.sec.gov/edgar.shtml

### SEC Financial Statement Data Sets (DERA quarterly bulk dump)
- **URL / API endpoint:** https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets — quarterly ZIPs (e.g. `2025q4.zip`).
- **What it provides:** Pre-extracted XBRL numeric facts from 10-K/10-Q/10-K/A/10-Q/A/20-F/40-F filings, normalized into four flat files per quarter: SUB (submission metadata), NUM (numeric facts), TAG (taxonomy tag dictionary), PRE (presentation linkbase).
- **Schema / format:** Pipe-delimited TXT inside ZIP (1 row per fact in NUM). NUM columns include adsh, tag, version, ddate, qtrs, uom, segments, value, footnote. Documented in https://www.sec.gov/files/financial-statement-data-sets.pdf.
- **Volume:** ~3–6M numeric facts per quarter; ~50k–80k filings/year; total cumulative ~150M+ facts since 2009.
- **History depth:** 2009-Q1 to current (XBRL became mandatory in phases 2009–2011).
- **Refresh cadence:** Quarterly, posted ~6–10 weeks after end of quarter. Older quarters are re-issued occasionally to repair restatements.
- **License / terms:** Public domain.
- **Rate limits:** None — bulk file download.
- **Quality issues:** "As filed" — does not reconcile restatements or amendments. Same fact may appear under multiple tags. Custom (extension) tags cannot be cross-company compared without manual mapping. ~5–15% of facts use unusual axes/segments that need normalisation.
- **Strategic value for ats-eqt:** **High.** This is the closest free analogue to Compustat's flat fundamentals files. Combined with the Frames API and companyfacts JSON it is sufficient to reconstruct ~80% of a Compustat-equivalent for US issuers — but only with significant tag-mapping work.
- **Source URL:** https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets

### SEC XBRL companyfacts API (data.sec.gov)
- **URL / API endpoint:** `https://data.sec.gov/api/xbrl/companyfacts/CIK##########.json`. Bulk: `https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip` ([unverified] exact path; the zip mirror is documented as containing all companyfacts).
- **What it provides:** All XBRL facts ever reported by a single CIK across every form type, keyed by tag and reporting period. One JSON file per CIK with both us-gaap and dei taxonomies.
- **Schema / format:** JSON. Top-level: `cik`, `entityName`, `facts.{taxonomy}.{tag}.units.{unit}.[]` where each entry has `start`, `end`, `val`, `accn`, `fy`, `fp`, `form`, `filed`, `frame`.
- **Volume:** ~12,000 active CIKs with structured XBRL; JSONs range from a few KB to >50 MB per company. Bulk zip ~5–10 GB.
- **History depth:** 2009-present per company (subject to phase-in by filer size).
- **Refresh cadence:** Updated near-real-time as new filings are accepted.
- **License / terms:** Public domain.
- **Rate limits:** 10 req/s/IP. User-Agent header required.
- **Quality issues:** Some companies have many extension tags. Restatement appears as new entries with later `filed` date — consumer must dedupe by `(tag, period, form)` choosing latest. Pre-2017 tag versions sometimes missing the `frame` attribute.
- **Strategic value for ats-eqt:** **Critical.** The bulk zip is the single most efficient way to backfill 15+ years of US fundamentals. (source: https://www.sec.gov/search-filings/edgar-application-programming-interfaces, https://tldrfiling.com/blog/sec-edgar-api-guide/)
- **Source URL:** https://www.sec.gov/search-filings/edgar-application-programming-interfaces

### SEC Frames API (data.sec.gov)
- **URL / API endpoint:** `https://data.sec.gov/api/xbrl/frames/{taxonomy}/{tag}/{unit}/{period}.json` — e.g. `us-gaap/AccountsPayableCurrent/USD/CY2019Q1I.json`.
- **What it provides:** Cross-sectional snapshot — one fact per entity for the calendar period most-closely matching the requested frame. Ideal for ranking, screening, factor construction.
- **Schema / format:** JSON. Each row: `accn`, `cik`, `entityName`, `loc`, `end`, `val`.
- **Volume:** Up to ~10,000 facts per frame call (one per reporting entity).
- **History depth:** 2009-present.
- **Refresh cadence:** Continuous as filings accepted.
- **License / terms:** Public domain.
- **Rate limits:** 10 req/s/IP, User-Agent required.
- **Quality issues:** Selecting one fact per entity per period requires SEC's heuristic (first-filed match closest to requested period); revisions and amendments not exposed in Frames the same way they are in companyfacts.
- **Strategic value for ats-eqt:** **High.** Lets ats-eqt build cross-section snapshots without iterating every CIK file. Useful for factor research and percentile ranks.
- **Source URL:** https://www.sec.gov/search-filings/edgar-application-programming-interfaces

### XBRL US (academic + commercial normalization layer)
- **URL / API endpoint:** https://xbrl.us/data-rule/, REST API at https://api.xbrl.us/api/v1/. Academic/research access free; commercial tier paid.
- **What it provides:** Fact-level XBRL data normalized across taxonomy versions, with quality-rule flagging (e.g. detection of broken sign conventions, mis-tagged extensions). Also hosts ESEF (European) data via a separate program.
- **Schema / format:** JSON REST API, parameter-driven (concept, period, dimensions, entity).
- **Volume:** Mirrors EDGAR + selected ESEF — billions of facts across ~30k entities.
- **History depth:** 2009-present (US); ~2021-present for ESEF subset.
- **Refresh cadence:** Daily during US filing windows.
- **License / terms:** Free for non-commercial / academic research with registration; commercial use requires paid license. (source: https://xbrl.us/academic-repository/sec-edgar-data/)
- **Rate limits:** [unverified] documented per-tier.
- **Quality issues:** Quality-rule flags are uneven across older filings; the academic license restricts redistribution.
- **Strategic value for ats-eqt:** **Medium.** As a commercial competitor, ats-eqt cannot ship XBRL US data verbatim — but the *quality rules* are open-source and can be reimplemented over our own EDGAR ingest, which is much higher leverage than re-licensing.
- **Source URL:** https://xbrl.us/

### UK Companies House (filings + REST API)
- **URL / API endpoint:** https://api.company-information.service.gov.uk/. Developer portal: https://developer.company-information.service.gov.uk/.
- **What it provides:** Company registration data, officers, persons-with-significant-control (PSC) register, charges, accounts metadata, and filed accounts (PDF). A separate streaming API delivers a real-time event firehose.
- **Schema / format:** JSON REST. Filed accounts mostly PDF, with a growing iXBRL subset (FRS 101/102 taxonomies).
- **Volume:** ~5M active companies, ~10M historic; ~3M annual filings/year.
- **History depth:** Modern digital records 2008-present; older microfiche scans available but unstructured.
- **Refresh cadence:** Real-time for the API and streaming endpoints; bulk product (CSV) released monthly.
- **License / terms:** Open Government Licence v3.0 — commercial reuse permitted with attribution. (source: https://developer.company-information.service.gov.uk/developer-guidelines/)
- **Rate limits:** 600 requests per 5-minute window per API key. 429 on overage. Higher limits by request, not guaranteed. Streaming API has its own connection rules.
- **Quality issues:** PDF accounts dominate — XBRL coverage thin compared to SEC. Officer dates-of-birth partially redacted. PSC data quality known to be uneven (deliberate misstatement is a criminal offence but enforcement is light).
- **Strategic value for ats-eqt:** **High.** Critical for UK coverage and for any global supply-chain entity-resolution graph (UK is a major incorporation jurisdiction for multinationals).
- **Source URL:** https://developer.company-information.service.gov.uk/

### IFRS / ESEF — European Single Electronic Format (XBRL via national OAMs)
- **URL / API endpoint:** Aggregator: https://filings.xbrl.org/. Per-country OAMs vary widely (Finland: https://www.finanssivalvonta.fi/; Germany: Bundesanzeiger; France: AMF; Italy: 1info.it; Netherlands: AFM; etc.).
- **What it provides:** Annual financial reports (in XHTML+iXBRL) of EU-regulated-market issuers, with IFRS consolidated statements machine-tagged using the ESMA ESEF taxonomy (IFRS Taxonomy + ESMA extensions).
- **Schema / format:** ESEF report packages (zip with iXBRL XHTML, taxonomy linkbases). Aggregator provides xBRL-JSON conversions.
- **Volume:** >5,000 ESEF filings/year as of 2024+ across 27 EU members + Norway, Iceland, Liechtenstein.
- **History depth:** Mandatory for fiscal years starting 2020 onward (some early filings from 2020/2021).
- **Refresh cadence:** Annual per issuer; aggregator updated within days/weeks of filing.
- **License / terms:** Mixed. Filings themselves are public regulatory disclosures. Some OAMs (e.g. Bundesanzeiger) impose their own re-use restrictions on bulk extraction; filings.xbrl.org provides them under terms allowing analysis but not unrestricted resale of curated datasets — **legal review required before redistribution**. (source: https://filings.xbrl.org/docs/about)
- **Rate limits:** Vary by OAM. Some actively block scraping (esp. Bundesanzeiger).
- **Quality issues:** Per-country tagging quality varies. Anchoring of company-specific extensions to core IFRS concepts is uneven. iXBRL block tagging (notes) is shallow.
- **Strategic value for ats-eqt:** **High** for European fundamentals. The aggregator is the ingestion path of least resistance vs. 30 separate OAMs.
- **Source URL:** https://filings.xbrl.org/

### Japan EDINET (Tokyo / FSA)
- **URL / API endpoint:** EDINET API v2 — `https://api.edinet-fsa.go.jp/api/v2/...` (free key from FSA).
- **What it provides:** Annual / Semi-annual / Quarterly Securities Reports, Securities Registration Statements, Tender Offers, Shareholding Notices, Internal Control reports for all Japanese listed companies and a long tail of other SFA filers.
- **Schema / format:** XBRL packages (zip) per filing; supports JP-GAAP, IFRS, and US-GAAP taxonomies. Date-keyed `documents` endpoint returns metadata, then `?type=1` returns the XBRL ZIP.
- **Volume:** ~11,000+ filers, ~30k filings/year.
- **History depth:** 2008-present (older paper-only).
- **Refresh cadence:** Real-time on filing.
- **License / terms:** Public regulatory disclosure; FSA publishes API terms (Japanese). Generally permissive for analytical use; careful redistribution review advised.
- **Rate limits:** [unverified] published in API spec; key required.
- **Quality issues:** Documentation is Japanese-first; English labels in older taxonomy files are partial. Extension tags common.
- **Strategic value for ats-eqt:** **High** for Japan coverage — a meaningful gap in many open datasets.
- **Source URL:** https://disclosure2dl.edinet-fsa.go.jp/guide/static/disclosure/WEEK0060.html

### TSE TDnet (Tokyo Stock Exchange — Timely Disclosure Network)
- **URL / API endpoint:** https://www.release.tdnet.info/inbs/I_main_00.html — HTML index by date.
- **What it provides:** Real-time exchange-mandated disclosures (earnings flash, material change announcements). Complements EDINET (which is regulator-mandated) with exchange-mandated disclosure.
- **Schema / format:** HTML index → PDF/HTML/XBRL attachments. Earnings summaries (kessan-tanshin) are XBRL-tagged.
- **Volume:** ~100k filings/year.
- **History depth:** ~2009-present in current XBRL form.
- **Refresh cadence:** Real-time (intraday).
- **License / terms:** Public exchange disclosure, no API key. JPX has terms restricting bulk redistribution; analytical use generally accepted.
- **Rate limits:** Not formally rate-limited but scraping-pattern-sensitive.
- **Quality issues:** The kessan-tanshin XBRL is the source of "preliminary" earnings; can revise vs. EDINET annual.
- **Strategic value for ats-eqt:** **Medium-high** for Japanese earnings event capture.
- **Source URL:** https://www.release.tdnet.info/

### HKEXnews — Hong Kong Stock Exchange
- **URL / API endpoint:** https://www.hkexnews.hk/, listed-company search at https://www1.hkexnews.hk/search/titlesearch.xhtml. DION shareholding system: https://di.hkex.com.hk/di/NSSrchMethod.aspx.
- **What it provides:** Listed-company announcements, prospectuses, annual/interim/quarterly reports, disclosure of interests (DI) notices.
- **Schema / format:** Predominantly PDF; some announcements have structured CSV companions. No public official XBRL push.
- **Volume:** ~2,500 listed companies, ~100k annual filings.
- **History depth:** ~1999-present.
- **Refresh cadence:** Real-time.
- **License / terms:** Disclosures are public; HKEX terms restrict bulk republishing of website content. Analytical extraction generally tolerated.
- **Rate limits:** No formal API; web scraping is the only path. HKEX occasionally blocks aggressive scrapers.
- **Quality issues:** PDF-heavy — significant OCR/parse cost. No standardized fundamentals taxonomy; tables vary by company.
- **Strategic value for ats-eqt:** **High** for HK and dual-listed China (H-shares) coverage. Demands a parsing investment that is the moat.
- **Source URL:** https://www.hkexnews.hk/

### SSE (Shanghai), SZSE (Shenzhen) Exchange disclosures
- **URL / API endpoint:** SSE: http://www.sse.com.cn/disclosure/listedinfo/announcement/, SZSE: https://www.szse.cn/disclosure/.
- **What it provides:** Listed-company filings, annual/interim/quarterly reports, board/governance disclosures.
- **Schema / format:** PDF + some HTML; CSAR Chinese accounting standard taxonomy XBRL is filed but not always exposed publicly.
- **Volume:** SSE ~2,200, SZSE ~2,800 listed companies.
- **History depth:** ~2000-present.
- **Refresh cadence:** Real-time on disclosure.
- **License / terms:** Public disclosure; site terms restrict bulk republication. **Significant compliance/redistribution risk to non-China entities** — Chinese data export controls are an open question.
- **Rate limits:** No formal API; rate-sensitive.
- **Quality issues:** Chinese-language filings dominate. Cross-listing reconciliation with HKEX and Hong Kong-American ADRs is non-trivial.
- **Strategic value for ats-eqt:** **Medium.** Coverage is critical but legal posture is uncertain. Recommend partnering with a licensed mainland data broker rather than direct ingest.
- **Source URL:** http://www.sse.com.cn/, https://www.szse.cn/

### SEDAR+ (Canadian Securities Administrators)
- **URL / API endpoint:** https://www.sedarplus.ca/. No documented public API.
- **What it provides:** Canadian public-company filings (audited financials, MD&A, AIF, prospectuses, insider reports), exempt-market issuer offering documents, Cease Trade Orders, Disciplined List.
- **Schema / format:** PDF predominantly; metadata exposed via the search UI.
- **Volume:** ~4,000 reporting issuers, ~600k filings cumulative.
- **History depth:** SEDAR+ post-2015 fully indexed; legacy SEDAR archive available with profiles back to ~1997.
- **Refresh cadence:** Real-time.
- **License / terms:** Public regulatory disclosure. CSA terms restrict commercial republication of compiled SEDAR+ data **without permission** — a redistribution review is required.
- **Rate limits:** No formal API; scraping-rate sensitive.
- **Quality issues:** No native XBRL mandate (Canada has not adopted XBRL fundamentals filing as of mid-2025). Primary path to fundamentals is OCR of PDF financials, which is expensive and error-prone.
- **Strategic value for ats-eqt:** **Medium-high** for Canadian coverage. The lack of XBRL is a structural quality cliff vs. US/EU/Japan.
- **Source URL:** https://www.sedarplus.ca/

### ASX (Australian Securities Exchange) announcements
- **URL / API endpoint:** Public per-company announcement page at `https://www.asx.com.au/asx/v2/statistics/announcements.do?code={ticker}`. Subscription `https://api.asxonline.com/mia/1/mia-api`.
- **What it provides:** Listed-company announcements (results, dividends, governance, takeovers, etc.).
- **Schema / format:** PDF announcements + JSON metadata via undocumented public endpoint. Subscribers via MIA API get structured JSON/REST.
- **Volume:** ~2,200 listed entities, ~150k announcements/year.
- **History depth:** Modern records ~2002-present.
- **Refresh cadence:** Real-time during ASX trading hours.
- **License / terms:** Free public access to current announcements; **bulk historical access and machine-readable feed are commercial** under ASX Online Information Services. Re-publication is restricted.
- **Rate limits:** Public endpoints rate-sensitive; ASX has historically tightened against scraping.
- **Quality issues:** No mandatory XBRL. Headlines tagged ("Sensitive" Y/N), but body text is PDF.
- **Strategic value for ats-eqt:** **Medium** for Australia. May require commercial license for production use; v1 path is to ingest only the SEC 20-F equivalents from cross-listed names.
- **Source URL:** https://www.asx.com.au/connectivity-and-data/information-services/price-data/how-to-access-asx-price-data

---

## 2. Identifiers / entity reference

### GLEIF LEI (Global Legal Entity Identifier)
- **URL / API endpoint:** Golden Copy: https://www.gleif.org/en/lei-data/gleif-golden-copy/download-the-golden-copy. Concatenated files (free): https://www.gleif.org/en/lei-data/gleif-concatenated-file. REST API: https://api.gleif.org/api/v1/.
- **What it provides:** ISO 17442 20-character LEI for legal entities, plus Level-1 (who is who) and Level-2 (who owns whom — direct/ultimate parent) data.
- **Schema / format:** XML + JSON (CDF format). Daily concatenated zips; per-LEI REST resources.
- **Volume:** ~2.5M active LEIs, ~5M cumulative including lapsed.
- **History depth:** 2012-present.
- **Refresh cadence:** Three sets of Golden Copy files daily (00:00, 08:00, 16:00 UTC).
- **License / terms:** **Free of charge under the LEI Data Terms of Use.** Specific clauses: no claim of GLEIF endorsement; no claim of IP in LEIs; CHF 100,000 liquidated-damages clause for non-compliance per breach. Redistribution **is permitted** if those terms are honoured. (source: https://www.gleif.org/en/meta/lei-data-terms-of-use)
- **Rate limits:** REST API ~60 req/min unauthenticated [unverified]; bulk Golden Copy is download — no throttling.
- **Quality issues:** Level-2 parent relationships are self-reported and have ~30% null rate. ~10% of LEIs are lapsed at any time. Lei-issuer data quality varies by LOU (Local Operating Unit).
- **Strategic value for ats-eqt:** **Critical.** The LEI is the single best free entity backbone. It is also the only legally-clean way to publish a global company graph.
- **Source URL:** https://www.gleif.org/

### LSEG / Refinitiv PermID (free tier)
- **URL / API endpoint:** https://permid.org/. Entity Search REST + Record Matching REST. Open Calais (text-tagging) free tier.
- **What it provides:** Refinitiv permanent identifiers for organizations, instruments, quotes, persons. Cross-walk to LEI, RIC, ISIN, MIC, ticker.
- **Schema / format:** JSON REST.
- **Volume:** Hundreds of millions of PermIDs across all entity types.
- **History depth:** Reflects the active state of the Refinitiv graph.
- **Refresh cadence:** Continuous.
- **License / terms:** Open under PermID Terms — free for use; redistribution conditions apply. Note this is a Refinitiv asset; LSEG retains the right to change terms.
- **Rate limits:** Open Calais free tier 500 req/day; Search/Match REST tiered.
- **Quality issues:** PermIDs are stable but the underlying mapping graph is curated by Refinitiv (may carry their bias). Free tier is rate-limited; instrument-level PermIDs have less depth than the paid Refinitiv data graph.
- **Strategic value for ats-eqt:** **Medium.** Useful as a *cross-walk* (PermID ↔ LEI ↔ ticker) but cannot be the primary identifier in an open product because LSEG's terms could change. Use for resolution, not as a published key.
- **Source URL:** https://permid.org/

### OpenFIGI (Bloomberg)
- **URL / API endpoint:** https://www.openfigi.com/api. POST /v3/mapping for bulk mapping; /v3/search for free-text.
- **What it provides:** Financial Instrument Global Identifier (FIGI) — a 12-character ID for every instrument across asset classes. Mapping from ISIN/CUSIP/SEDOL/ticker/exchange-code → FIGI; reverse lookups; share-class and composite-FIGI hierarchy.
- **Schema / format:** JSON REST. Bulk mapping accepts batches.
- **Volume:** Hundreds of millions of FIGIs across equities, fixed income, FX, futures, options.
- **History depth:** FIGI is contemporaneous; no historical state machine.
- **Refresh cadence:** Continuous (Bloomberg's reference graph).
- **License / terms:** **MIT-style open license — FIGI itself is in the public domain.** No fees, no daily/weekly/monthly cap, no restriction on redistribution. (source: https://www.openfigi.com/about/faq)
- **Rate limits:** Anonymous: lower (a few requests per minute, jobs-per-batch capped). With free API key: 25,000 jobs/minute via the bulk mapping endpoint per published guidance.
- **Quality issues:** US-listed instruments are well-mapped; some emerging-market equity share-classes mapped with delays. Composite vs. exchange-level FIGI requires care.
- **Strategic value for ats-eqt:** **Critical.** This is the only free, openly-licensed instrument identifier of global breadth. Pair with LEI on the entity side and ats-eqt has the entire identifier backbone.
- **Source URL:** https://www.openfigi.com/api/documentation

### OpenCorporates
- **URL / API endpoint:** https://api.opencorporates.com/.
- **What it provides:** Aggregated company-registry data from ~140 jurisdictions, harmonised to a common schema (company number, name, status, jurisdiction, officers, addresses, filings index).
- **Schema / format:** JSON REST.
- **Volume:** ~200M+ company records.
- **History depth:** Varies by registry; OpenCorporates has been ingesting since ~2010.
- **Refresh cadence:** Continuous; per-jurisdiction cadence varies (UK Companies House feed is near-real-time; many jurisdictions are batch monthly).
- **License / terms:** **Strict.** Two API key classes: (a) free "share-alike" keys for journalists, NGOs, academics conducting public-benefit research, requiring contribution back to the open ecosystem; (b) paid "non-share-alike" keys for commercial / financial / government users. Self-serve plans 2026: Essentials £2,250/yr, Starter £6,600/yr, Basic £12,000/yr; Enterprise on request. (source: https://opencorporates.com/pricing/)
- **Rate limits:** Tier-dependent; pay-per-call metering applies.
- **Quality issues:** Coverage and freshness vary by jurisdiction (UK and US states excellent; some emerging markets months behind). Company-name matching across jurisdictions is non-trivial — they don't impose a global identifier; users must rely on `(jurisdiction_code, company_number)`.
- **Strategic value for ats-eqt:** **Medium-high but financially gated.** A commercial competitor (which ats-eqt is) cannot get a free key. Either budget £12k+/year or build per-registry ingest directly.
- **Source URL:** https://opencorporates.com/

### EDGAR CIK (Central Index Key)
- **URL / API endpoint:** https://www.sec.gov/cgi-bin/browse-edgar?action=getcompany. JSON: https://www.sec.gov/files/company_tickers.json, https://www.sec.gov/files/company_tickers_exchange.json.
- **What it provides:** Stable SEC issuer ID (10-digit CIK), with concordance to ticker and exchange.
- **Schema / format:** JSON.
- **Volume:** ~900k CIKs (issuers, funds, individuals).
- **History depth:** 1993-present (CIK stable across name changes).
- **Refresh cadence:** Updated continuously.
- **License / terms:** Public domain.
- **Rate limits:** Inherits EDGAR 10 req/s.
- **Quality issues:** Multiple CIKs can exist for one corporate group (parent + subsidiary file separately). One CIK can have multiple tickers across share classes.
- **Strategic value for ats-eqt:** **Critical** for US issuer identity.
- **Source URL:** https://www.sec.gov/files/company_tickers.json

### Wikidata / DBpedia
- **URL / API endpoint:** Wikidata Query Service: https://query.wikidata.org/. DBpedia: https://dbpedia.org/sparql. Wikipedia REST API: https://en.wikipedia.org/api/rest_v1/.
- **What it provides:** Free knowledge graph linking companies to ISINs, LEIs, CIKs, tickers, parent organisations, executives, headquarters locations, founding dates, NAICS/ISIC codes, etc. ~10M company-related items in Wikidata.
- **Schema / format:** RDF via SPARQL.
- **Volume:** ~100M Wikidata items total; ~10M with `instance of: business` or descendants.
- **History depth:** Item-creation date ranges across the project lifetime.
- **Refresh cadence:** Community-edited, continuous.
- **License / terms:** **CC0 (Wikidata) / CC-BY-SA (DBpedia / Wikipedia text).** Wikidata structured data is public-domain — ats-eqt can ingest, transform, redistribute freely. (source: https://www.wikidata.org/wiki/Wikidata:Licensing)
- **Rate limits:** Wikidata Query Service has a 60-second timeout per query and informal politeness limits; bulk dump preferable for full-graph extraction.
- **Quality issues:** User-generated — varying completeness and accuracy. Best as a *triangulation* layer over LEI/CIK/PermID.
- **Strategic value for ats-eqt:** **High** as a free, openly-licensed disambiguation graph and as a source of human-readable entity descriptions, logos, and Wikipedia sentiment context.
- **Source URL:** https://www.wikidata.org/

### Country-level company registries (direct)
- **URLs:** Examples — Germany Bundesanzeiger https://www.unternehmensregister.de/, France INPI/Sirene https://api.insee.fr/sirene, India MCA https://www.mca.gov.in/, Singapore ACRA https://www.acra.gov.sg/, Hong Kong CR https://www.icris.cr.gov.hk/, Brazil Receita Federal CNPJ public file.
- **What they provide:** Authoritative company registration, officers, status, charter; varying levels of financial-statement disclosure.
- **Schema / format:** Mix of JSON APIs, HTML scrape, downloadable CSV/JSON dumps (e.g. INSEE Sirene CSV, CNPJ monthly file).
- **License / terms:** Highly variable. INSEE Sirene is open data under Licence Ouverte 2.0 (commercial reuse OK); CNPJ data is public; many EU registries impose per-extract fees.
- **Strategic value for ats-eqt:** **Medium** — preferable to OpenCorporates for high-coverage jurisdictions where the registry itself publishes a clean dump (France, Brazil, Singapore, India). Avoids OpenCorporates licence costs.
- **Source URL:** see jurisdiction-specific links above.

---

## 3. Sector / industry classifications

### NAICS (North American Industry Classification System)
- **URL / API endpoint:** https://www.census.gov/naics/. Concordances: https://www.census.gov/eos/www/naics/concordances/concordances.html.
- **What it provides:** 6-digit hierarchical industry codes for the US/Canada/Mexico, with crosswalks to SIC, ISIC, NAICS prior vintages.
- **Schema / format:** XLSX / CSV downloads.
- **Volume:** ~1,000 6-digit codes across 20 sectors.
- **History depth:** NAICS 1997 → 2002 → 2007 → 2012 → 2017 → 2022 vintages.
- **Refresh cadence:** Every 5 years.
- **License / terms:** Public domain (US Census Bureau).
- **Rate limits:** None — static download.
- **Quality issues:** SIC↔NAICS bridge is many-to-many; not a clean 1:1.
- **Strategic value for ats-eqt:** **High** — only free, mature US/CA/MX classification with the structural depth needed for sector analytics.
- **Source URL:** https://www.census.gov/naics/

### SIC (Standard Industrial Classification)
- **URL / API endpoint:** EDGAR uses SIC: https://www.sec.gov/info/edgar/siccodes.htm.
- **What it provides:** Legacy 4-digit US classification, still the primary code attached to every EDGAR registrant (the SEC has not migrated to NAICS).
- **Schema / format:** Static page / CSV.
- **Volume:** ~1,000 4-digit codes.
- **History depth:** 1987-present (no updates since 1987 but still in use at SEC).
- **Refresh cadence:** Static.
- **License / terms:** Public domain.
- **Rate limits:** N/A.
- **Quality issues:** Not maintained; new industries (cloud, biotech sub-segments) are crammed into ill-fitting buckets.
- **Strategic value for ats-eqt:** **High** because EDGAR SIC is the *de facto* SEC-issuer industry tag. Pair with NAICS via the Census crosswalks.
- **Source URL:** https://www.sec.gov/info/edgar/siccodes.htm

### ICB (Industry Classification Benchmark) / GICS
- **What they provide:** The standard institutional classifications used by index families (FTSE Russell uses ICB; S&P/MSCI use GICS).
- **License / terms:** **Both are paid commercial.** GICS is jointly owned by S&P and MSCI. ICB is owned by FTSE Russell.
- **Strategic value for ats-eqt:** **Out of scope as a free input** — but ats-eqt should publish its own NAICS-derived sector taxonomy and provide a *crosswalk* to GICS via Wikidata cross-references where they exist.
- **Source URL:** https://www.spglobal.com/spdji/en/landing/topic/gics/, https://www.ftserussell.com/data/industry-classification-benchmark-icb

### ISIC / NACE
- **URL / API endpoint:** UN ISIC https://unstats.un.org/unsd/classifications/Econ/isic. Eurostat NACE Rev. 2 https://ec.europa.eu/eurostat/web/nace.
- **License / terms:** Public domain / freely usable.
- **Strategic value for ats-eqt:** **Medium** — needed for non-US/non-NAICS jurisdictions, especially for mapping into UN Comtrade, Eurostat, and OECD data.

---

## 4. Supply chain / trade

### US CBP AMS bill-of-lading manifest data
- **URL / API endpoint:** No first-party API. CBP releases manifest data on CD-ROM under 19 CFR 103.31 to subscribers; aggregators (ImportGenius, Panjiva, ImportYeti, Datamyne) bulk-resell. Some FOIA-extracted dumps exist (Data Liberation Project).
- **What it provides:** Per-shipment manifest detail for inbound vessels at US ports — consignee, shipper, vessel, voyage, port, container count, cargo description, weight, bill of lading, HS-prefix on cargo description. 19 CFR 103.31 is the legal authority.
- **Schema / format:** Pipe-delimited proprietary CBP format on CD; aggregators normalize to CSV/JSON.
- **Volume:** ~12M bills of lading per year for US imports.
- **History depth:** ~2006-present in the public-feed era.
- **Refresh cadence:** Daily.
- **License / terms:** **Public information** under 19 CFR 103.31, BUT shippers/consignees can file for "manifest confidentiality" (renewed every 2 years), removing themselves from the public feed. Aggregators are commercial; their compiled databases are NOT public domain even if the underlying records are. To build an open ats-eqt feed, ats-eqt must subscribe directly to CBP's CD distribution and re-publish the underlying public records.
- **Rate limits:** N/A — bulk distribution.
- **Quality issues:** Cargo descriptions are free-text, not consistently HS-coded by filers. ~5–10% of consignees use freight forwarders (FF/FF), masking ultimate consignee. Confidentiality opt-outs hide ~3,000–5,000 of the largest US importers.
- **Strategic value for ats-eqt:** **Critical and differentiating.** This is the single most valuable open dataset for supply-chain inference at the issuer level. Combined with EDGAR Exhibit 21 + Form SD, ats-eqt can build a unique supplier/customer graph that even FactSet sells as a premium add-on.
- **Source URL:** https://www.cbp.gov/trade/automated/electronic-vessel-manifest-confidentiality, https://www.ecfr.gov/current/title-19/chapter-I/part-103/subpart-C/section-103.31

### US Census USA Trade Online / International Trade API
- **URL / API endpoint:** https://api.census.gov/data/timeseries/intltrade/imports/hs, .../exports/hs (and analogous for naics, enduse, statehs, statenaics, porths). USA Trade Online UI: https://usatrade.census.gov/.
- **What it provides:** Monthly US imports and exports aggregated by HS-10 (10-digit Harmonized System) × country × district × end-use × NAICS. State and port breakouts available at HS-2/4/6.
- **Schema / format:** JSON REST.
- **Volume:** Tens of millions of rows/year at HS-10 × country × month.
- **History depth:** Annual 1992-present, monthly 2003-present in USA Trade Online; monthly 2013-present via API.
- **Refresh cadence:** Monthly, ~6 weeks lag.
- **License / terms:** Public domain.
- **Rate limits:** Census API key required (free); 500 requests/day/key default, raise on request.
- **Quality issues:** Statistical disclosure suppresses some HS-10 × country cells with low-volume reporters.
- **Strategic value for ats-eqt:** **High.** The ground truth for US trade flows. Combined with AMS gives both top-down (HS aggregates) and bottom-up (per-shipment) views.
- **Source URL:** https://www.census.gov/data/developers/data-sets/international-trade.html

### UN Comtrade (v2)
- **URL / API endpoint:** https://comtradeapi.un.org/. Developer portal: https://comtradedeveloper.un.org/.
- **What it provides:** Global trade flows (imports/exports) at HS-2/4/6 and SITC across reporting countries × partner countries × commodity × year/month.
- **Schema / format:** JSON REST.
- **Volume:** ~1B records cumulative.
- **History depth:** 1962-present.
- **Refresh cadence:** Updated as countries report (~quarterly to annual).
- **License / terms:** Free with registration (subscription key); UN data terms permit analytical reuse with attribution.
- **Rate limits:** Free tier 500 calls/day, up to 100k records/call. Premium tier (paid) has higher caps.
- **Quality issues:** Mirror data inconsistencies between reporters/partners (~10–20% bilateral discrepancy is normal). Country reporting cadence variable (some countries lag 2+ years).
- **Strategic value for ats-eqt:** **High** for global trade context, especially for inferring supply-chain dependencies in non-US-served markets where AMS isn't available.
- **Source URL:** https://comtradeplus.un.org/

### Eurostat Comext (EU import/export)
- **URL / API endpoint:** https://ec.europa.eu/eurostat/api/comext/dissemination (DS-prefixed datasets).
- **What it provides:** EU intra- and extra-EU trade in goods at CN-8 (Combined Nomenclature 8-digit) by reporter × partner × month. Also Prodcom (manufactured-goods production statistics).
- **Schema / format:** SDMX 2.1 (XML), JSON-stat, CSV.
- **Volume:** Hundreds of millions of cells annually.
- **History depth:** 1988-present.
- **Refresh cadence:** Monthly.
- **License / terms:** **Free re-use under Commission Decision 2011/833/EU**, attribution required, no warranty. Compatible with commercial redistribution.
- **Rate limits:** Public endpoints; full datasets must be filtered (no full-corpus single download).
- **Quality issues:** Intra-EU flows are based on Intrastat (importer/exporter survey) below an exemption threshold; small-shipper coverage is partial.
- **Strategic value for ats-eqt:** **High** for European trade flows.
- **Source URL:** https://ec.europa.eu/eurostat/web/international-trade-in-goods/database

### OECD Trade in Value Added (TiVA)
- **URL / API endpoint:** OECD Data Explorer with SDMX export: https://data-explorer.oecd.org/. TiVA dataset code DSD_TIVA_*.
- **What it provides:** Decomposes gross trade flows into domestic-vs-foreign value-added components — measures real upstream dependency in supply chains.
- **Schema / format:** SDMX, CSV, Excel.
- **Volume:** 76 economies × 45 industries × 1995–2020+.
- **History depth:** 1995-present.
- **Refresh cadence:** Annual revision (last release Oct 2025 per OECD page).
- **License / terms:** OECD Terms — free with attribution; redistribution permitted with proper credit.
- **Rate limits:** None for bulk download.
- **Quality issues:** Latest year typically lags 3–5 years. Estimates rely on input-output assumptions.
- **Strategic value for ats-eqt:** **Medium.** Macro-level overlay; useful as context for sector-level supply-chain risk reports rather than per-company.
- **Source URL:** https://www.oecd.org/en/topics/sub-issues/trade-in-value-added.html

### India ICEGATE / Zauba (per-shipment)
- **URL / API endpoint:** ICEGATE public enquiry: https://www.icegate.gov.in/. Zauba: https://www.zauba.com/shipment_search.
- **What it provides:** India per-shipment customs data — shipping bills (exports) and bills of entry (imports) with consignee, shipper, port, HS code, value, weight, country.
- **Schema / format:** Web UI (HTML); aggregators sell normalized CSV.
- **Volume:** ~15M shipping bills/year.
- **History depth:** ~2007-present (via aggregators).
- **Refresh cadence:** Daily.
- **License / terms:** Indian customs data is *public* under RTI rules; aggregators (Zauba, EximTradeData) commercialise the parsed feeds. Direct ICEGATE bulk export is not available — only single-record lookups.
- **Rate limits:** ICEGATE web interface is rate-sensitive; aggregator APIs are paid.
- **Quality issues:** Free-text descriptions; HS code completeness varies; aggregator-level data has its own license.
- **Strategic value for ats-eqt:** **Medium-high.** India shipment data is an asymmetric advantage if obtained at scale, especially for sector intelligence on pharma, IT services exports, gems & jewellery, agro.
- **Source URL:** https://www.icegate.gov.in/

### Other emerging-market customs (per-shipment public manifests)
- **Brazil — Comex Stat / SISCOMEX:** https://comexstat.mdic.gov.br/. **Free**, monthly, HS-8 (NCM) × state × country × value × volume since 1989. Strong public API. **Strategic value: High.**
- **Mexico — Banxico / SAT:** Aggregate trade via INEGI; per-shipment requires paid customs broker feeds. Lower public availability.
- **Argentina, Chile, Colombia, Ecuador, Peru:** Bulk per-shipment customs data is public but typically routed through paid aggregators (Penta-Transaction, ImportGenius LATAM coverage).
- **Vietnam, Pakistan, Sri Lanka, Costa Rica:** Highly variable. Vietnam customs publishes monthly aggregates only. Sri Lanka has open per-shipment data (https://www.customs.gov.lk/). Costa Rica's PROCOMER publishes summary data.
- **Strategic value:** **Medium.** A determined ats-eqt build covering Brazil + India + Sri Lanka + Costa Rica adds meaningful EM coverage at low cost.
- **Source URL:** https://comexstat.mdic.gov.br/

### AIS open feeds (NOAA Marine Cadastre, AISHub)
- **URL / API endpoint:** NOAA AccessAIS https://marinecadastre.gov/accessais/. Bulk historical (US): https://hub.marinecadastre.gov/. AISHub: https://www.aishub.net/.
- **What it provides:** Per-second to per-minute vessel-position pings (lat/lon, speed, heading, MMSI), with vessel metadata (IMO, callsign, type, cargo, dimensions). NOAA covers US territorial + EEZ waters; AISHub provides a global crowd-sourced subset.
- **Schema / format:** GeoParquet (2024+) / CSV (older) / NMEA AIVDM (raw); AISHub real-time JSON/XML.
- **Volume:** US Coast Guard NAIS feed produces ~1B position records/year. AISHub real-time push.
- **History depth:** NOAA bulk: 2009-present. AISHub: real-time only (no archive in free tier).
- **Refresh cadence:** AccessAIS daily, GeoParquet annual; AISHub live.
- **License / terms:** NOAA: public domain. AISHub: free for data contributors who share their AIS receiver feed; commercial redistribution restricted.
- **Rate limits:** NOAA download cap ~2 GB per ad-hoc order via AccessAIS (use the bulk hub for full archive). AISHub unlimited for contributors.
- **Quality issues:** Coverage gaps mid-ocean (terrestrial AIS only — satellite AIS is paid). Spoofing and "dark fleet" vessels. MMSI ↔ IMO mapping not always complete.
- **Strategic value for ats-eqt:** **High** as the open spine of a port-call / vessel-utilisation product. Combine with AMS (consignee × vessel × port × bill of lading) and ats-eqt has a unique chain: company → shipment → vessel → port-call.
- **Source URL:** https://marinecadastre.gov/ais/

### WTO TAO / IDB
- **URL / API endpoint:** WTO Tariff Analysis Online https://tao.wto.org/. Integrated Database (IDB) at https://idb.wto.org/.
- **What it provides:** Applied and bound tariff rates by country × HS line × year; non-tariff measures notifications.
- **Schema / format:** Excel / CSV downloads, query UI.
- **Volume:** Millions of country-line-year cells.
- **History depth:** ~1996-present.
- **Refresh cadence:** Annual.
- **License / terms:** WTO members' data, free for analytical use; redistribution requires attribution.
- **Strategic value for ats-eqt:** **Medium** — needed for any tariff-impact analytics layered on customs data.
- **Source URL:** https://tao.wto.org/

### WCO HS Code reference / GHS hazardous-materials reference
- **URLs:** WCO HS: https://www.wcoomd.org/en/topics/nomenclature/instrument-and-tools.aspx. GHS: https://unece.org/transport/dangerous-goods/ghs.
- **License / terms:** WCO HS code listings are commercial (WCO Bookshop); UN GHS is freely available.
- **Strategic value for ats-eqt:** **Low-medium** — needed for product-classification normalisation.

---

## 5. Corporate disclosures (non-financial)

### EU CSRD / ESRS (Corporate Sustainability Reporting Directive)
- **URL / API endpoint:** Filings will be in the ESEF report packages on national OAMs and aggregated at https://filings.xbrl.org/ once ESEF blocks for ESRS are activated.
- **What it provides:** Mandatory sustainability disclosure (climate, environment, social, governance) under the European Sustainability Reporting Standards (ESRS) for ~50,000 EU and non-EU large companies, phased in 2024–2028.
- **Schema / format:** iXBRL using the ESRS digital taxonomy.
- **Volume:** Ramping; ~12,000 firms in scope by 2025-FY filings.
- **History depth:** First filings 2025/2026.
- **Refresh cadence:** Annual.
- **License / terms:** Same as ESEF — public regulatory disclosure; OAM-specific.
- **Quality issues:** Early filings will have heavy use of narrative blocks vs. structured datapoints.
- **Strategic value for ats-eqt:** **High and growing fast.** This is the most important new data asset emerging in 2025–2028. Position to ingest from year one.
- **Source URL:** https://www.efrag.org/

### CDP (Carbon Disclosure Project)
- **URL / API endpoint:** https://data.cdp.net/. https://www.cdp.net/en/data.
- **What it provides:** Corporate climate, water, forests, plastics, supply-chain disclosure (~24,000 companies in 2024 cycle).
- **Schema / format:** Open Data Portal CSV/Socrata; new Disclosure API for corporate filers (2025+).
- **Volume:** Tens of thousands of full questionnaire responses per year.
- **History depth:** 2003-present (varying disclosure depth).
- **Refresh cadence:** Annual.
- **License / terms:** **Mixed.** Scores and aggregated public datasets are free under attribution; full questionnaire responses are restricted (CDP licensed paid product). Public access has tightened over time.
- **Rate limits:** [unverified] for the disclosure API.
- **Quality issues:** Self-reported. Comparability across years interrupted by methodology changes.
- **Strategic value for ats-eqt:** **Medium.** The free-tier (scores + A-list) is sufficient for screening; full data may require a CDP partnership.
- **Source URL:** https://www.cdp.net/en/data

### EDGAR Exhibit 21 (Subsidiaries)
- **URL / API endpoint:** Exhibit 21 is filed as part of 10-K under EDGAR; available as an exhibit document or section within the 10-K. Several open-source parsers (edgartools, etc.) extract it.
- **What it provides:** List of significant subsidiaries with jurisdiction of incorporation. Mandatory in 10-K filings (Reg S-K Item 601(b)(21)).
- **Schema / format:** Free-text inside the exhibit (not standardized — sometimes a table, sometimes narrative). NOT XBRL-tagged as of 2025.
- **Volume:** ~7,000 10-Ks/year contain Exhibit 21.
- **History depth:** Pre-1993 paper, structured EDGAR 1996+.
- **Refresh cadence:** Annual per filer.
- **License / terms:** Public domain.
- **Quality issues:** Free-text → significant NLP/parsing investment required. Inclusion is required only for "significant" subsidiaries — true global subsidiary counts are under-reported.
- **Strategic value for ats-eqt:** **Critical for supply chain.** Combining Exhibit 21 (parent → subsidiary entity graph) with AMS (subsidiary appears as consignee on bill of lading) lets ats-eqt attribute imports to *parent issuers* — a transformational capability vs. naive consignee-name matching.
- **Source URL:** https://www.sec.gov/Archives/edgar/

### Form SD / Conflict Minerals Reports (Section 1502 Dodd-Frank)
- **URL / API endpoint:** EDGAR form-type filter `Form SD`: https://www.sec.gov/cgi-bin/browse-edgar?action=getcompany&type=SD.
- **What it provides:** Annual disclosure of due diligence on tin, tungsten, tantalum, gold (3TG) sourcing from DRC region. Includes smelter lists, supplier names, country-of-origin determinations.
- **Schema / format:** Form SD (HTML/PDF) plus Conflict Minerals Report exhibit (free-text PDF mostly).
- **Volume:** ~1,000 filers/year (2022 cohort: 1,005 filings).
- **History depth:** 2014-present (first filings for FY 2013).
- **Refresh cadence:** Annual, due May 31.
- **License / terms:** Public domain.
- **Quality issues:** Free-text; smelter-list quality varies. RMI (Responsible Minerals Initiative) maintains a normalized smelter database that can join to filers.
- **Strategic value for ats-eqt:** **High** for supply-chain risk products in electronics/automotive.
- **Source URL:** https://www.sec.gov/about/forms/formsd.pdf

### Customer concentration disclosure (10-K Item 1 / Item 101)
- **URL / API endpoint:** Embedded in 10-K filings on EDGAR.
- **What it provides:** Reg S-K Item 101 requires disclosure of any customer accounting for >10% of consolidated revenues. The filer typically names the customer.
- **Schema / format:** Free-text within Part I, Item 1. Not XBRL-tagged.
- **Volume:** Several thousand US filers disclose at least one ≥10% customer.
- **License / terms:** Public domain.
- **Quality issues:** Free-text → NLP extraction. Some filers obscure customer name ("a major automotive OEM").
- **Strategic value for ats-eqt:** **Critical.** This is one of the most valuable supply-chain attributes in the entire EDGAR corpus. Pair with Exhibit 21 + AMS for an end-to-end customer-supplier graph.
- **Source URL:** https://www.sec.gov/Archives/edgar/

### GHG Protocol / Scope 3 disclosures (voluntary today, mandatory in some jurisdictions)
- **URL / API endpoint:** No central registry. Mostly disclosed via CDP, sustainability reports, or directly in 10-Ks (especially after SEC climate rule in some form).
- **Strategic value for ats-eqt:** **Medium-high once CSRD/ESRS comes into force** — Scope 3 will become structurally tagged via ESRS-E1, transforming this from an unstructured pile into a machine-readable graph.

---

## 6. Estimates / consensus alternatives

### Estimize
- **URL / API endpoint:** estimize.com — **largely defunct as of 2024**. Originally crowdsourced earnings estimates.
- **Strategic value for ats-eqt:** **Effectively zero.** Skip.

### Wall Street Horizon (corporate event calendar)
- **URL / API endpoint:** https://www.wallstreethorizon.com/. Data via API or FTP; integrations include Interactive Brokers TWS API.
- **What it provides:** Confirmed/forecasted earnings dates, dividend dates, splits, M&A, conferences, capital-markets days for 11,000+ companies, 40+ event types.
- **License / terms:** **Commercial.** Not public/free.
- **Strategic value for ats-eqt:** **Out of scope as a free input.** The cheaper open path is to derive earnings dates from EDGAR 8-K filings and exchange announcements, which captures actuals; *forecasted* dates are harder.
- **Source URL:** https://www.wallstreethorizon.com/

### Earnings call transcripts (Seeking Alpha, FMP, MotleyFool, etc.)
- **URL / API endpoint:** Seeking Alpha is paid. Financial Modeling Prep https://financialmodelingprep.com/ — paid tiered. Some companies post their own transcripts on IR sites.
- **License / terms:** All major aggregators are paid. Self-hosting transcripts requires either a vendor license or building a transcription pipeline from earnings call audio (companies often publish audio under terms-of-use that restrict bulk transcription).
- **Strategic value for ats-eqt:** **High but legally fenced.** A Whisper-based open transcription pipeline against publicly-broadcast earnings audio is legally grey but technically open; verify with counsel.

### Analyst targets via SEC research reports
- **URL / API endpoint:** Some free disclosure on EDGAR (Form 19b-4 attachments, occasionally research disclosures). FINRA broker reports are not public.
- **Strategic value for ats-eqt:** **Low.** Sell-side estimates remain the strongest moat for FactSet/Refinitiv. ats-eqt cannot replicate I/B/E/S consensus from public data.

---

## 7. Macro / supporting reference data

### FRED (Federal Reserve Economic Data)
- **URL / API endpoint:** https://fred.stlouisfed.org/docs/api/. API key required (free).
- **What it provides:** ~800,000 US and international macro time series (rates, FX, employment, inflation, sectoral indexes).
- **Schema / format:** JSON / XML REST.
- **License / terms:** Most series carry the source provider's license (Fed Board, BLS, BEA — public domain). Some commercial series (e.g. ICE, S&P) **cannot be redistributed**; FRED tags these.
- **Rate limits:** Practical limit of ~120 req/min ([unverified] published number; community-observed).
- **Strategic value for ats-eqt:** **High** as macro overlay. Mind the redistribution flags on commercial series.
- **Source URL:** https://fred.stlouisfed.org/docs/api/fred/

### World Bank Open Data
- **URL / API endpoint:** https://api.worldbank.org/v2/.
- **License:** CC-BY 4.0. Strategic value: **Medium** for country-level fundamentals overlay.

### IMF SDMX / Statistical Data Warehouse
- **URL:** https://data.imf.org/. SDMX REST.
- **License:** Free with attribution. Strategic value: **Medium.**

---

## Strategic synthesis: where the open-data competitive moat is

### Replicating Compustat / Worldscope / FactSet Fundamentals via XBRL
With **EDGAR companyfacts + DERA quarterly + ESEF (filings.xbrl.org) + EDINET + UK Companies House iXBRL**, ats-eqt can reconstruct ~75–85% of a Compustat-equivalent dataset for the four largest disclosure regimes (US, EU, Japan, UK). The hard limits are:

1. **Tag-mapping work.** XBRL extension tags are pervasive — every tag mapping to a normalized standard ("Total Revenue") needs human or LLM-assisted curation. Estimated 12 person-months of taxonomy normalisation work for a Compustat-grade output.
2. **No XBRL in Canada (SEDAR+).** Canadian fundamentals require PDF-to-fact extraction. This is a genuine quality cliff vs. the paid vendors.
3. **No XBRL on HKEX, ASX, mainland China.** Same problem; PDF parsing required.
4. **Restatements.** XBRL preserves "as-filed"; restated history requires custom logic over `(filed_date, period, tag)` to reconstruct point-in-time snapshots — a competitive table-stake that mature vendors have, ats-eqt must build.
5. **Adjustments / non-GAAP normalisation.** "Adjusted EBITDA" definitions vary by issuer; vendors curate normalised non-GAAP. ats-eqt must either (a) ship raw-only and lean into transparency as a feature, or (b) build a normalisation layer (expensive).

### Unique advantages from open data combinations
The asymmetric value of ats-eqt is **not in fundamentals** (where the vendors are entrenched) but in **the supply-chain graph that no major fundamentals vendor sells natively**:

- **EDGAR Exhibit 21 (parent→sub graph)** + **CBP AMS bills-of-lading (sub-as-consignee)** + **OpenFIGI/LEI cross-walk** = parent-issuer-attributable import flows. ImportGenius and Panjiva resell raw AMS but do not publish parent-rolled-up data; FactSet/Bloomberg do but as a high-priced add-on.
- **AIS (NOAA Marine Cadastre) + AMS + LEI** = vessel-level and port-level utilisation tied to shippers and consignees. This is a unique open-data triple.
- **Form SD + RMI smelter database + LEI** = supplier-level conflict-minerals risk graph.
- **10-K Item 1 customer concentration extraction (NLP) + Exhibit 21 + AMS** = a customer→supplier-of-record graph from primary disclosure, with provenance — a transparency feature paid vendors cannot match because their proprietary customer-supplier graphs lack auditable links.
- **Wikidata (CC0) + GLEIF (free) + OpenFIGI (MIT)** = the only fully redistributable global identifier triple. ats-eqt can publish this concordance openly; OpenCorporates (priced £12k+/yr) cannot legally be used as the public spine.

### Gaps that cannot be closed with open data (vendors' true moat)
1. **GICS / ICB.** Paid. Workaround: NAICS-based sector taxonomy + Wikidata GICS bridge where it exists.
2. **Sell-side broker estimates / consensus (I/B/E/S, S&P Capital IQ Estimates).** Effectively impossible to replicate from public data. Workaround: aggregate company-issued guidance + analyst estimates that appear on disclosed research-platform filings — much narrower coverage.
3. **Curated entity ownership graph (Refinitiv DataScope, Bloomberg PORT).** GLEIF Level-2 covers ~30% of the gap; the rest requires either OpenCorporates (paid) or per-jurisdiction registry ingest.
4. **Reference-data restatement-aware fundamentals (Compustat point-in-time, FactSet Fundamentals as-filed/as-restated).** ats-eqt can build this from XBRL but only with sustained engineering effort.
5. **Tick-by-tick exchange data.** Out of scope for ats-eqt's fundamentals + supply-chain mission.
6. **Earnings call transcripts at scale.** Aggregators are paid; in-house transcription is technically feasible but legally grey.
7. **Manifest confidentiality opt-outs.** ~3,000–5,000 large US importers self-suppress in AMS; this gap cannot be filled.

### Recommended ingestion priorities for the first 12 months

**Quarter 1 — identifier + US fundamentals spine**
- GLEIF Golden Copy (daily)
- OpenFIGI (incremental)
- EDGAR companyfacts.zip (initial bulk + incremental)
- EDGAR DERA quarterly (initial backfill)
- EDGAR submission index + RSS (incremental filing capture)
- Wikidata company subset (initial bulk + weekly delta)

**Quarter 2 — supply chain core**
- CBP AMS bills-of-lading (daily CD-ROM subscription)
- US Census International Trade API (monthly)
- EDGAR Exhibit 21 NLP pipeline
- 10-K Item 1 customer-concentration extractor

**Quarter 3 — international fundamentals expansion**
- ESEF via filings.xbrl.org (initial bulk + delta)
- EDINET XBRL (initial bulk + delta)
- UK Companies House (streaming API + monthly bulk)
- INSEE Sirene + Brazil CNPJ + Singapore ACRA (per-registry direct ingest, avoids OpenCorporates fees)

**Quarter 4 — supply chain depth + ESG**
- AIS NOAA Marine Cadastre annual GeoParquet (initial bulk) + AISHub real-time (if possible)
- UN Comtrade v2 (monthly)
- Eurostat Comext (monthly)
- Form SD / Conflict Minerals corpus
- CDP free-tier scores
- Begin ESRS/CSRD ingestion pipeline (ESEF taxonomy already in place)

**Strategic posture for license risk:** ats-eqt should publish *only* under a clean stack (public-domain US gov + GLEIF + OpenFIGI + Wikidata CC0 + Eurostat reuse decision + INSEE/CNPJ open licences) at v1, and treat OpenCorporates / XBRL US / AISHub / aggregator AMS feeds as *internal enrichment* paths that may not be redistributed without per-source vetting.

---

## Sources

- [SEC.gov — Financial Statement Data Sets](https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets)
- [SEC.gov — Financial Statement Data Sets PDF](https://www.sec.gov/files/financial-statement-data-sets.pdf)
- [SEC.gov — EDGAR Application Programming Interfaces](https://www.sec.gov/search-filings/edgar-application-programming-interfaces)
- [SEC.gov — Accessing EDGAR Data](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data)
- [SEC.gov — Developer Resources](https://www.sec.gov/about/developer-resources)
- [SEC.gov — New rate control limits to EDGAR](https://www.sec.gov/filergroup/announcements-old/new-rate-control-limits)
- [SEC.gov — EDGAR Full-Text Search](https://www.sec.gov/edgar/search/)
- [SEC.gov — Form SD PDF](https://www.sec.gov/about/forms/formsd.pdf)
- [SEC.gov — Form SD updated](https://www.sec.gov/files/formsd.pdf)
- [SEC.gov — EDGAR SIC Codes](https://www.sec.gov/info/edgar/siccodes.htm)
- [SEC.gov — company_tickers.json](https://www.sec.gov/files/company_tickers.json)
- [tldrfiling — SEC EDGAR API Guide](https://tldrfiling.com/blog/sec-edgar-api-guide/)
- [tldrfiling — EDGAR full-text search API guide](https://tldrfiling.com/blog/sec-edgar-full-text-search-api)
- [XBRL US — SEC EDGAR Data](https://xbrl.us/academic-repository/sec-edgar-data/)
- [XBRL US — ESEF Data](https://xbrl.us/academic-repository/esma-esef-data/)
- [filings.xbrl.org — about](https://filings.xbrl.org/docs/about)
- [XBRL International — Well over 3,000 ESEF filings at filings.xbrl.org](https://www.xbrl.org/news/well-over-3000-esef-filings-at-filings-xbrl-org-where-are-they-coming-from-and-how-can-we-improve-access/)
- [XBRL International — launches filings.xbrl.org for ESEF](https://www.xbrl.org/news/xbrl-international-launches-filings-xbrl-org-for-esef-filings/)
- [GLEIF — Download the Golden Copy](https://www.gleif.org/en/lei-data/gleif-golden-copy/download-the-golden-copy)
- [GLEIF — Concatenated Files](https://www.gleif.org/en/lei-data/gleif-concatenated-file)
- [GLEIF — LEI Data Terms of Use](https://www.gleif.org/en/meta/lei-data-terms-of-use)
- [GLEIF — Golden Copy Specification (v2.2)](https://www.gleif.org/lei-data/gleif-golden-copy/2022-02-23_gleif-golden-copy-and-delta-files_v2.2-final.pdf)
- [GLEIF — API](https://www.gleif.org/en/lei-data/gleif-api)
- [OpenFIGI — Documentation](https://www.openfigi.com/api/documentation)
- [OpenFIGI — FAQ](https://www.openfigi.com/about/faq)
- [OpenFIGI — Overview](https://www.openfigi.com/api/overview)
- [Bloomberg — OpenFIGI launch press release](https://www.openfigi.com/insights/all/2016/1/20/open-figi-com-and-open-figi-api-press-release)
- [OpenCorporates — API documentation](https://api.opencorporates.com/documentation/API-Reference)
- [OpenCorporates — Pricing](https://opencorporates.com/pricing/)
- [OpenCorporates — Terms of Use](https://opencorporates.com/terms-of-use-2/)
- [OpenCorporates — Enterprise API ToS](https://opencorporates.com/legal-information/enterprise-api-terms-of-service/)
- [Zephira — OpenCorporates pricing 2026](https://zephira.ai/opencorporates-pricing-explained-2026-plans-api-limits-licensing-and-what-it-means-in-production/)
- [LSEG — PermID](https://permid.org/)
- [LSEG — PermID APIs User Guide](https://developers.lseg.com/content/dam/devportal/api-families/open-permid/permid-entity-search/documentation/permid-apis-user-guide-apr-2020.pdf)
- [Wikidata — SPARQL examples](https://www.wikidata.org/wiki/Wikidata:SPARQL_query_service/queries/examples)
- [Wikidata — Query Service](https://query.wikidata.org/)
- [bobdc — Normalizing company names with SPARQL and Wikidata](https://www.bobdc.com/blog/wikidatanormalizing/)
- [UK Companies House — Developer Guidelines](https://developer.company-information.service.gov.uk/developer-guidelines/)
- [UK Companies House — Rate limiting](https://developer-specs.company-information.service.gov.uk/guides/rateLimiting)
- [UK Companies House — Get started](https://developer.company-information.service.gov.uk/get-started)
- [EDINET — Operation guides](https://disclosure2dl.edinet-fsa.go.jp/guide/static/disclosure/WEEK0060.html)
- [Axiora — EDINET for Developers](https://axiora.dev/en/blog/edinet-for-developers)
- [HKEXnews](https://www.hkexnews.hk/index.htm)
- [HKEX — Disclosure of Interests](https://www2.hkexnews.hk/Shareholding-Disclosures/Disclosure-of-Interests?sc_lang=en)
- [HKEX — DION search](https://di.hkex.com.hk/di/NSSrchMethod.aspx)
- [SEDAR+ Landing Page](https://www.sedarplus.ca/home/)
- [CSA — SEDAR+ Overview](https://www.securities-administrators.ca/national-systems/about-sedar/sedar-overview/)
- [OSC — SEDAR+](https://www.osc.ca/en/industry/sedarplus)
- [ASX — How to access ASX Price data](https://www.asx.com.au/connectivity-and-data/information-services/price-data/how-to-access-asx-price-data)
- [ASX Online — Information Services User Guide](https://asxonline.com/content/asxonline/public/documents/asxonline-information-services-user-guide.html)
- [US Census — International Trade datasets](https://www.census.gov/data/developers/data-sets/international-trade.html)
- [US Census — Guide to International Trade Datasets PDF](https://www.census.gov/foreign-trade/reference/guides/Guide_to_International_Trade_Datasets.pdf)
- [US Census — NAICS](https://www.census.gov/naics/)
- [US Census — Industry/Occupation Code Lists & Crosswalks](https://www.census.gov/topics/employment/industry-occupation/guidance/code-lists.html)
- [BLS — Classifications and Crosswalks](https://www.bls.gov/emp/documentation/crosswalks.htm)
- [UN Comtrade — main portal](https://comtrade.un.org/)
- [UN Comtrade — Plus](https://comtradeplus.un.org/ListOfReferences)
- [UN Comtrade — Developer Portal](https://comtradedeveloper.un.org/)
- [Eurostat — Comext API getting started](https://ec.europa.eu/eurostat/web/user-guides/data-browser/api-data-access/api-getting-started/comext-database)
- [Eurostat — International trade in goods database](https://ec.europa.eu/eurostat/web/international-trade-in-goods/database)
- [OECD — Trade in Value Added](https://www.oecd.org/en/topics/sub-issues/trade-in-value-added.html)
- [OECD — Data Explorer TiVA](https://data-explorer.oecd.org/vis?df%5Bds%5D=dsDisseminateFinalDMZ&df%5Bid%5D=DSD_TIVA_MAINLV@DF_MAINLV&df%5Bag%5D=OECD.STI.PIE)
- [OECD — Guide to TiVA PDF](https://stats.oecd.org/wbos/fileview2.aspx?IDFile=afa5c684-c31d-49dd-87db-6fd674f29a43)
- [eCFR — 19 CFR 103.31 vessel manifests](https://www.ecfr.gov/current/title-19/chapter-I/part-103/subpart-C/section-103.31)
- [CBP — Electronic Vessel Manifest Confidentiality](https://www.cbp.gov/trade/automated/electronic-vessel-manifest-confidentiality)
- [CBP — Confidential treatment FAQ](https://www.help.cbp.gov/s/article/Article-1108)
- [Data Liberation Project — CBP Bills of Lading](https://www.data-liberation-project.org/requests/cbp-bills-of-lading/)
- [NOAA Digital Coast — AccessAIS](https://www.coast.noaa.gov/digitalcoast/tools/ais.html)
- [Marine Cadastre — AccessAIS](https://marinecadastre.gov/accessais/)
- [Marine Cadastre — Vessel Traffic hub](https://hub.marinecadastre.gov/pages/vesseltraffic)
- [Marine Cadastre — AIS](https://marinecadastre.gov/ais/)
- [AISHub](https://www.aishub.net/)
- [ICEGATE — Indian Customs](https://www.icegate.gov.in/)
- [Zauba — Shipment Search](https://www.zauba.com/shipment_search)
- [Brazil Comex Stat — Data Basis](https://data-basis.org/dataset/74827951-3f2c-4f9f-b3d0-56e3aa7aeb39)
- [Brazil — New Comex Stat platform announcement](https://www.gov.br/secom/en/latest-news/2024/05/new-platform-expedites-access-to-brazil-foreign-trade-data)
- [CDP — Open Data Portal](https://data.cdp.net/)
- [CDP — Use CDP Data](https://www.cdp.net/en/data)
- [CDP — 2025 Disclosure API launch](https://www.cdp.net/en/insights/cdp-launches-2025-disclosure-api)
- [Wall Street Horizon — Earnings Calendar & Event Data](https://www.wallstreethorizon.com/)
- [Wall Street Horizon — Interactive Brokers integration](https://www.wallstreethorizon.com/ibkr-wsh)
- [GAO — Conflict Minerals 2022 report PDF](https://www.gao.gov/assets/gao-23-106295.pdf)
- [Finanssivalvonta — ESEF Finland](https://www.finanssivalvonta.fi/en/financial-market-participants/capital-markets/issuers-and-investors/esef-xbrl/)
- [FRED API documentation](https://fred.stlouisfed.org/docs/api/fred/)
- [FRED — Terms of use](https://fred.stlouisfed.org/docs/api/terms_of_use.html)
- [edgartools — XBRL filings (community parser)](https://www.edgartools.io/filing-xbrl/)
