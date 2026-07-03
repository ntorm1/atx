# Supply-Chain Intelligence Pure Plays — Competitive & Data-Source Landscape

> Research database entry for **ats-eqt**, an open-source-data competitor for equity
> fundamentals + supply-chain intelligence. This document profiles dedicated supply-chain
> intelligence vendors (excluding general financial-data terminals), maps their
> underlying public data inputs, and synthesizes the implicit "industry standard"
> graph schema that ats-eqt would need to match or replace.
>
> Date: 2026-05-09 | Author: research agent | Status: working draft
> Citations are inline `(source: URL)`. Unverified claims are marked `[unverified]`.

---

## 1. Vendor-by-Vendor Product Summaries

### 1.1 Resilinc

**What they sell.** Resilinc is a multi-tier supplier-risk-management (SRM) and
supply-chain-resiliency platform. Core products are *EventWatchAI* (24x7 disruption
monitoring across 40+ event types), *Multi-Tier Mapping* (tier-1 → tier-N visibility
down to part-site), and an "Agentic AI" orchestration layer rebranded in 2026 around
their Databricks-built Agent Factory partnership (source:
https://resilinc.ai/, https://markets.financialcontent.com/stocks/article/gnwcq-2026-3-5-resilinc-becomes-validated-databricks-built-on-partner-with-agent-factory-for-supply-chain-risk-and-compliance).

**Customer profile.** Procurement, supply-chain risk, and resilience teams at
Fortune 500 manufacturers — heavy concentration in pharma, semiconductors, aerospace,
and automotive. Resilinc was named a Leader in the 2026 Gartner Magic Quadrant for
Supplier Risk Management Solutions (source:
https://www.globenewswire.com/news-release/2026/05/06/3289253/0/en/Resilinc-Named-a-Leader-in-2026-Gartner-Magic-Quadrant-for-Supplier-Risk-Management-Solutions.html).
Recent FedRAMP authorization extends the buyer profile into US government / DoD
prime contractors (source:
https://www.globenewswire.com/news-release/2026/04/01/3266667/0/en/Resilinc-Achieves-FedRAMP-Authorization-Strengthening-Secure-AI-Driven-Supply-Chain-Risk-Management-for-Government-and-Regulated-Industries.html).

**Underlying data sources.**
- **Direct supplier disclosure** is the primary differentiator: Resilinc maintains
  a "validated dataset directly sourced from suppliers down to the part-site level"
  with 800K+ suppliers mapped, suppliers self-attesting site/part data into the
  platform under MNDAs. This is fundamentally a *contributory data network* — closer
  to a Dun & Bradstreet-style trade exchange than a customs-feed aggregator.
- **News/web monitoring** at scale: 100M+ sources, 40+ event types, 100+ countries
  and languages, NLP-filtered (source: https://resilinc.ai/).
- **Geophysical / hazard data** (USGS-style earthquake feeds, weather, hurricane
  tracks) [unverified — typical for the segment but not explicitly listed].

**Schema / data model.** A part-site-supplier hierarchy with cross-links to events.
Public docs describe entities at the **supplier**, **site**, **part**, **product**,
and **category** levels with risk scores attached at each level. Edges are primarily
*supplies → part → site* and *site → location* (geo). Multi-tier traversal is the
flagship feature — explicit Tier-1, Tier-2, Tier-N traversal with confidence
attached to each hop (source: https://www.resilinc.com/solutions/).

**Coverage scale.** 800K+ mapped suppliers; thousands of data fields per supplier;
40+ event types across 100+ countries.

**Differentiator vs FactSet Revere / Bloomberg SPLC / S&P Panjiva.** Resilinc is
*not* a filings or BoL aggregator — it does not derive its supplier graph from
10-K Item 1 disclosures or US Customs manifests. The graph is *contributed*, which
yields deeper sub-tier coverage (you see Apple's tier-3 chemical supplier; Panjiva
does not) but narrower public-equity coverage (you cannot use it to estimate revenue
exposure of a publicly traded firm to its supplier base).

---

### 1.2 Interos (interos.ai)

**What they sell.** Interos sells an "operational resilience" graph keyed on
business-to-business relationships. Flagship products are the *Interos Knowledge
Graph* and the *iQ* platform (launched April 2026), which matches a customer's ERP
identifiers to the graph and quantifies risk exposure in dollar terms (source:
https://www.businesswire.com/news/home/20260428661670/en/interos.ai-Launches-iQ-to-Elevate-Supply-Chain-Risks-to-the-C-Level).

**Customer profile.** Procurement + third-party-risk-management (TPRM) teams,
plus financial-services operational-resilience functions (post DORA / OCC
third-party guidance). Strong public sector + DoD presence — Interos has won
multiple DoD prime contracts for TPRM / supply-chain illumination.

**Underlying data sources.** Interos publicly claims its knowledge graph covers
**400 million+ companies and billions of relationships** (source:
https://www.interos.ai/). Public materials describe "thousands of proprietary
data points" and seven canonical risk signal classes: **ESG, Cyber, Financial,
Restrictions, Geopolitical, Catastrophic, Operational** (source:
https://www.interos.ai/our-software). The mix is believed to include:
- Government corporate registries [unverified, but consistent with the entity count]
- Sanctions / watchlist / restricted-party lists
- Cyber-posture probes and BitSight-style external attack surface signals
- News / NLP for event extraction
- Filings + SEC + court records for financial signals
- Some BoL aggregation [unverified]

**Schema / data model.** A property graph keyed on company entities with
multi-typed risk-signal edges. The iQ product specifically pivots on the *ERP
identifier → graph entity* match — implying a vendor-master-data resolution
layer sits between customer data and the graph.

**Coverage scale.** 400M+ companies, "billions of relationships" — by far the
largest claimed entity count among supply-chain pure plays. (For comparison:
Sayari claims 450M+; Panjiva ~10M companies via shipments.)

**Differentiator.** Breadth + the cyber-risk overlay. Interos is the most
horizontal of the SRM vendors — closer to a TPRM-and-resilience suite than a
deep-tier procurement tool. Weakness vs Resilinc is sub-tier depth; weakness
vs Sayari is beneficial-ownership traceability.

---

### 1.3 Everstream Analytics

**What they sell.** Predictive supply-chain risk + global monitoring platform.
Core surfaces: *Risk Assessment*, *Global Monitoring & Alerting*, *Risk-Optimized
Planning*, and an annual flagship *Annual Supply Chain Risk Report* used as a
demand-gen anchor (source: https://www.everstream.ai/).

**Customer profile.** Logistics + procurement teams at large shippers. Strong in
CPG, automotive, and retail — i.e., physical-goods-heavy industries where the
optimization unit is the *lane* (origin–destination–mode) rather than the
*supplier-part*.

**Underlying data sources.** Everstream is the most data-volume-loud vendor in
the segment, advertising **8M sources processed daily, 128 trillion data points
including weather, 1M news/media articles per hour** (source:
https://www.everstream.ai/platform/global-monitoring/). Concrete inputs:
- **Customer ERP/TMS integration** for supplier, material, and shipment data
- **Public + proprietary historical event datasets** for weather, natural
  disasters, geopolitics, transport disruptions, customs delays, cargo theft
- **Real-time media + on-ground analyst network**
- **"Exclusive" channels**: paywalled content, logistics-provider partnerships
  (DHL is publicly named), private chat groups (WeChat is publicly named)
  (source: https://www.everstream.ai/articles/rate-supply-chain-risk-with-scoring/)

**Schema / data model.** A network graph with **100M+ trading relationships and
12M+ suppliers**, attribute-mapped via entity resolution. Risk is decomposed
into **30 sub-categories** under economic / environmental / sociopolitical /
ethical / operational / compliance umbrellas. Predictive scores are layered on
top of the static graph.

**Coverage scale.** 12M suppliers, 100M trading relationships.

**Differentiator.** The "predictive" framing — Everstream leans on
forward-looking event probability rather than historical mapping. The DHL and
WeChat partnerships are the moat: very few competitors get usable signal out of
private logistics-provider operational data or Chinese closed messaging.

---

### 1.4 Sayari Graph

**What they sell.** Sayari Graph is an investigative knowledge graph for
**beneficial ownership, corporate hierarchy, and trade flows**, with a
*Supply Chain Mapping* premium add-on (source:
https://sayari.com/platform/graph/, https://sayari.com/global-trade/).

**Customer profile.** This is a **compliance / national-security / sanctions**
buyer profile, not procurement. Customers include US CBP itself (Sayari was
awarded a $7.8M Enterprise Trade Analytics contract in 2024–2025 to support
CBP's UFLPA enforcement — source:
https://sayari.com/resources/sayari-awarded-7-8m-enterprise-trade-analytics-contract-to-support-u-s-customs-border-protection/),
plus banks (KYC/KYB, beneficial-ownership screening), exporters under
export-controls regimes (BIS Entity List screening), and multinationals managing
forced-labor / UFLPA risk.

**Underlying data sources.** Public materials are unusually concrete:
- **Government corporate registries** in 250+ jurisdictions, with all 50 US
  states now searchable (source:
  https://sayari.com/resources/official-company-data-from-all-50-us-states-now-searchable-in-sayari-graph/)
- **Customs agencies** (their phrasing is "customs agencies and regulatory
  bodies across 250+ jurisdictions")
- **645+ global corporate and trade data sources** consolidated into entity
  profiles
- **2B+ ownership and control records**
- **450M+ entities, 700+ sources**

Sayari's stated discipline is that "every finding traces to an original
government record" — i.e., the graph is provenance-typed, with every edge
attached to a citable source document. This is closer to OpenCorporates' / OFAC
discipline than to Bloomberg SPLC's filings-and-estimates model.

**Schema / data model.** Property graph: **Entity** (company / person /
vessel / address) → **Ownership / Control / Officer** edges → **Trade**
edges (shipment-level). Beneficial-ownership traversal is the headline:
they advertise resolving beneficial owners through 20+ shell companies across
15 jurisdictions.

**Coverage scale.** 450M entities, 2B ownership/control records, 250+
jurisdictions. Trade data is layered on top via the Supply Chain Mapping add-on.

**Differentiator.** Provenance + jurisdictional depth + customs-agency
*partnership* (not just customs-data ingestion). Where Panjiva is a BoL
aggregator and Bloomberg SPLC is a filings synthesizer, Sayari is a
beneficial-ownership-first platform that *added* trade data — meaning the
graph's primary key is *who owns what*, with shipments as evidence.

---

### 1.5 Kpler (incl. Genscape, ClipperData, MarineTraffic)

**What they sell.** Real-time global commodity / cargo / shipping intelligence.
Kpler now spans crude, products, LNG, LPG, dry bulk, agriculture, and metals,
with the *Cargo Analytics*, *Maritime*, and *Power & Energy* surfaces as the
main product lines (source: https://www.kpler.com/).

**Customer profile.** Commodity-trading desks (oil majors, trading houses,
hedge funds), shipping owners/operators, and refineries. Increasingly used by
macro and equity desks for nowcasting commodity flows, and by regulators for
sanctions evasion detection.

**Underlying data sources.** Kpler is the cleanest example of a
*satellite-AIS-plus-proprietary-overlay* model.
- **Satellite + terrestrial AIS** (the core feed)
- **Port and terminal intelligence** (in-house analyst network)
- **Customs and shipping data**
- **Pipeline flow data** (inherited from Genscape)
- **ClipperData's** waterborne-cargo proprietary feed (acquired Sep 2021,
  source:
  https://www.kpler.com/blog/press-release-kpler-acquires-clipperdata,
  https://www.bloomberg.com/news/articles/2021-09-08/commodity-data-firm-kpler-buys-clipperdata-in-u-s-expansion)
- **MarineTraffic** AIS data services (Kpler acquired MarineTraffic; source:
  https://www.kpler.com/product/maritime/data-services)
- **Genscape**: physical-energy infrastructure monitoring (power plants, oil
  storage tanks, pipelines) — Kpler integrated Genscape after Wood Mackenzie
  divested it [unverified date specifics, but the product line is now Kpler-branded]

**Schema / data model.** Vessel-centric: **Vessel** (MMSI, IMO) → **Voyage**
(load port → discharge port) → **Cargo** (commodity class, volume, cargo
chemistry where measurable) → **Owner / Operator / Charterer** (corporate
entity). Kpler's value-add is the *cargo inference* layer — going from "tanker
left port X" to "tanker is carrying Y barrels of WTI heading to refinery Z."

**Coverage scale.** Global; effectively all AIS-equipped vessels (~70K+ active
commercial vessels). Annual recurring revenue quadrupled to ~$40M after the
ClipperData deal and reportedly targets ~$100M ARR — making Kpler the
financially largest pure-play in this list.

**Differentiator.** The *only* vendor on this list that prices the underlying
physical commodity flow rather than the corporate B2B relationship. Closer
substitute is Vortexa (oil cargoes specifically) than Resilinc / Interos /
Everstream.

---

### 1.6 ImportYeti / ImportGenius / Datamyne (Descartes)

These are best treated as a tier of **commercial bill-of-lading aggregators**
that all source from the same upstream CBP feed; the differences are coverage
breadth, UX, and price.

**ImportYeti.**
- *What they sell:* freemium BoL search UI; popular with e-commerce sellers
  hunting suppliers and with OSINT investigators
  (Bellingcat lists it in their toolkit — source:
  https://bellingcat.gitbook.io/toolkit/more/all-tools/importyeti).
- *Source:* "All bills of lading data from January 2015 through a Freedom of
  Information Act request to US Customs" (source:
  https://www.softwareadvice.com/bi/importyeti-profile/).
- *Coverage:* US ocean import bills of lading only. No exports, no air, no road,
  no other countries.
- *Pricing:* free tier with paid pro plans; the cheapest of the three.

**ImportGenius.**
- *What they sell:* paid trade-data platform with API access.
- *Source:* US ocean import BoLs **plus** scraped/licensed data from 12 Latin
  American countries, Russia, Turkey, India, Sri Lanka, Ukraine, Vietnam, and
  more (source: https://www.importgenius.com/pricing).
- *Coverage:* broader than ImportYeti; positioned as a Panjiva alternative for
  procurement teams and trade journalists.

**Datamyne (Descartes).**
- *What they sell:* enterprise BoL database, owned by Descartes Systems Group
  (NASDAQ: DSGX) — embedded in Descartes' broader Global Logistics Network.
- *Source:* the "U.S. shipment data is gathered from the Automated Manifest
  System (AMS), customs declarations and import-export customs statistics"
  (source: https://www.datamyne.com/our-product/bill-of-lading-database/).
  Descartes has the broadest non-US coverage of the three thanks to Mercosur
  customs licensing relationships.
- *Coverage:* US imports + a long tail of LatAm, plus Asian customs feeds
  (source: https://www.datamyne.com/faq/).

**Customer profile (all three).** Trade compliance, e-commerce sourcing,
sales/marketing prospecting (find competitors' suppliers), journalism,
academic research, and the long tail of investment researchers who can't
afford Panjiva.

**Differentiator vs S&P Panjiva.** Panjiva, ImportGenius, ImportYeti, and
Datamyne all ingest the same upstream CBP feed for US imports. The product
differentiation is therefore (a) breadth of non-US country licensing,
(b) entity-resolution / company-rollup quality, (c) UX and analytic surfaces,
and (d) integration into a broader stack (Datamyne → Descartes; Panjiva →
S&P Capital IQ).

---

### 1.7 Z2Data

**What they sell.** Component-grade, electronics-focused supply-chain risk
platform. Core products are *Part Risk Manager*, *BOM Management*, *Supply
Chain Watch*, and an Altium Designer integration that makes Z2Data the de facto
risk overlay for hardware engineers (source:
https://resources.altium.com/p/z2data-integration-advantages,
https://www.z2data.com/).

**Customer profile.** Electronics OEMs, contract manufacturers (EMS), and
component distributors. The buyer is typically a *component engineer* or
*sourcing engineer* — i.e., an individual contributor with a BOM in hand —
rather than a C-suite SCRM officer.

**Underlying data sources.**
- **Manufacturer-published part data** (datasheets, PCNs, EOL notices).
  Z2Data claims to "predict EOL dates before manufacturers issue formal
  notices" — implying NLP on PCN feeds and statistical lifecycle modeling.
- **Distributor inventory data** (Digi-Key relationship is publicly disclosed —
  source:
  https://www.prnewswire.com/news-releases/digi-key-electronics-and-z2data-announce-free-bom-management-licenses-for-companies-combating-covid-19-301034200.html).
- **Regulatory / compliance feeds**: REACH, RoHS, UFLPA (Uyghur Forced Labor
  Prevention Act), CMRT (conflict minerals), Prop65.
- **Geopolitical and supplier-health overlays** (financial signals on suppliers).

**Schema / data model.** **Part** (manufacturer P/N) → **Manufacturer** →
**Manufacturing Site** (200K+ sites) → **Country / Geopolitical Risk**, with
**Alternative Parts** edges between part nodes. The BOM is itself a graph
construct: BOM → Part → Risk score, rolling up to a 0–100 BOM-level score.

**Coverage scale.** 1B+ components, 1M+ suppliers (or 150K depending on the
page — Z2Data's marketing inconsistently quotes both numbers; see
https://www.z2data.com/landing/lpa/bom and https://www.z2data.com/), 200K /
30K manufacturing sites.

**Differentiator.** Z2Data is a niche hardware-electronics specialist. Where
Panjiva sees the *shipment* and Resilinc sees the *part-site*, Z2Data sees
the *electronic component* (with EOL forecasts and drop-in alternatives). For
an equity researcher modeling a semi-equipment supplier or an EMS, Z2Data is
the only vendor on this list with the granularity to answer "which Apple BOM
parts go end-of-life next quarter?".

---

### 1.8 Exiger (and the TealBook acquisition)

**Note on TealBook.** TealBook was acquired by **Supplier.io** (not by Exiger)
in April 2026, where it became part of Supplier.io's *Atlas* vendor-master-data
solution (source:
https://www.businesswire.com/news/home/20260402407775/en/Supplier.io-Acquires-TealBook-Connecting-Supplier-Intelligence-and-Enterprise-Data-Management).
TealBook is therefore now a vendor-MDM cleansing layer (225M global supplier
profiles, legal-entity resolution, corporate hierarchy) rather than an
independent supplier-intelligence pure play. The original brief grouped Exiger
and TealBook together — they are now adjacent rather than identical.

**Exiger — what they sell.** A unified supplier / product / part / component /
risk graph. Core products are *1Exiger* (the unified platform), *Insight 3PM*
(third-party risk management), and *DDIQ* (due diligence) (source:
https://www.exiger.com/innovations-technology/, https://www.exiger.com/).

**Customer profile.** A blend of (a) regulated financial-services TPRM,
(b) defense / aerospace / federal supply chain (DoD CMMC, NDAA Section 889),
and (c) Fortune 500 procurement. Exiger's federal book is large.

**Underlying data sources.** Exiger publicly markets "the world's largest
engineering knowledge graph of **10B supply-chain records, 400M part attributes,
and 6.5M mapped relationships in 200+ countries** that links parts, materials,
specs, and suppliers" (source:
https://www.exiger.com/innovations-technology/). The mix is believed to include:
- Component datasheets + manufacturer disclosures (parts layer)
- Government registries + sanctions lists (entity layer)
- Customs / BoL aggregation (relationship evidence layer) [unverified mix]
- Federal databases (SAM.gov, FPDS, NDAA-restricted lists, OFAC, BIS Entity List)
- Adverse-media + NLP

**Schema / data model.** The "seven dimensions of risk" framework wraps the
graph; entities span **supplier, product, part, component, material, spec, and
risk-event**. Multi-tier traversal is supported, but Exiger's unique angle is
the *part-level* graph — closer to Z2Data than to Resilinc on resolution, but
broader than Z2Data across non-electronic components.

**Coverage scale.** 10B supply-chain records, 400M parts, 200+ countries.

**Differentiator.** Federal-grade compliance posture (cleared analyst staff,
government-cloud deployments) plus the part-level engineering graph.

---

### 1.9 (Sidebar) Where the Old Standards Sit

For ats-eqt's competitive framing, the *non-pure-play* incumbents are also
worth a paragraph:

- **FactSet Revere Supply Chain Relationships.** ~25K public companies,
  ~144K relationships, sourced from filings + press releases + corporate
  disclosures. Bidirectional ("direct" + "reverse"). No dollar-volume estimates.
- **Bloomberg SPLC.** ~23K public + ~96K private companies, ~900K relationships,
  filings + transcripts + analyst-estimated revenue exposure.
- **S&P Panjiva.** 8M+ companies, 1B+ shipment records, sourced primarily
  from BoL feeds across ~20 countries (US + LatAm + Asia subset). The
  *scale-of-edges* leader, but the relationships are inferred-from-shipments
  rather than disclosed.

The three databases have only ~43% supplier overlap — a striking statistic
that academic researchers (Culot 2023) use to argue that no single database is
"complete" and that they capture different *facets* of the supply chain
(disclosed vs inferred) rather than the same chain measured differently
(source: https://onlinelibrary.wiley.com/doi/full/10.1111/jscm.12294).

---

## 2. Underlying Public Data Sources — The Primary Inputs

This is the load-bearing section for ats-eqt's open-data strategy. The thesis:
**most of the supply-chain "data moat" is actually public data + entity
resolution + UI**. If that holds, an open-data competitor can recreate
80% of Panjiva's coverage at <5% of the cost.

### 2.1 US Customs / CBP AMS (Automated Manifest System)

**Schema of the publicly disclosable bill-of-lading record.** Per 19 CFR § 4.7a
and CBP's published FOIA-disclosable list, AMS manifest data made available to
the public includes (source:
https://www.law.cornell.edu/cfr/text/19/4.7a,
https://www.ecfr.gov/current/title-19/chapter-I/part-4/subject-group-ECFR9356f0b8b5be866/section-4.7a):

| Field | Notes |
|---|---|
| Bill of Lading Number | Unique key |
| Carrier Code | SCAC |
| Vessel Name | Free-text |
| Vessel Country | Flag state |
| Voyage Number | Carrier-issued |
| District / Port of Unlading | US port (Schedule D code) |
| Estimated Arrival Date | |
| Foreign Port of Lading | Schedule K code |
| Shipper Name | Foreign shipper — *suppressible on confidentiality request* |
| Shipper Address | Same |
| Consignee Name | US importer |
| Consignee Address | Same |
| Notify Party Name & Address | Often the broker |
| Description of Goods | Free-text — most BoL aggregators NLP this for HS-code inference |
| Manifest Quantity / Units | |
| Weight / Weight Unit | |
| Piece Count | |
| Container Number | ISO 6346 |
| Seal Number | |

Note that **HS code is not in the public AMS feed** — the public field is
"Description of Goods" (free text). Aggregators infer HS via NLP. This is the
single largest reason Panjiva-style trade data has a known false-precision
problem.

**Publication mechanism.** The data is **FOIA-disclosable** and made available
via paid bulk feeds purchased from CBP and from a small number of authorized
resellers. ImportYeti explicitly describes the path: a FOIA request to CBP,
with "a significant associated fee" (source:
https://www.softwareadvice.com/bi/importyeti-profile/). The *Data Liberation
Project* has documented the FOIA-feed mechanics and pricing publicly (source:
https://www.data-liberation-project.org/requests/cbp-bills-of-lading/).

**License.** The raw data is effectively **public domain** (federal
government work, FOIA-disclosed). Resellers add value through ETL,
entity-resolution, and indexing — but do **not** own the underlying records.
This is the core economic vulnerability of Panjiva / ImportGenius / Datamyne /
ImportYeti, and the structural opportunity for ats-eqt.

**Aggregator landscape.** Panjiva, ImportGenius, ImportYeti, and Datamyne all
source the same CBP feed for US ocean imports (source:
https://mywifequitherjob.com/importyeti/). They differentiate via (a)
non-US country licensing, (b) entity rollup quality, (c) UX, and (d) bundling.

**Volume.** ~10–15M ocean BoL records per year flowing into the US
[unverified-precise number, but the order of magnitude is widely cited].
The Federal Reserve's working paper on BoL data is the cleanest academic
primary source on volumes and structure (source:
https://www.federalreserve.gov/econres/feds/files/2021066pap.pdf).

**Known coverage gaps.**
- **US imports only**, ocean only — no air, no road, no rail.
- **Exports are not in this feed.** Vessel-cargo *export* manifests are
  becoming electronically reportable under the Electronic Export Manifest
  rule (Federal Register, 2026 — source:
  https://www.federalregister.gov/documents/2026/02/10/2026-02662/electronic-export-manifest-for-vessel-cargo)
  but historical US export data must currently be triangulated via foreign
  *import* manifests.
- **Confidentiality requests** allow foreign shippers (and indirectly some US
  importers) to suppress their identity from the public feed. Estimates of
  the suppression rate vary; Apple, for example, is famously suppressed.
- **No HS code, no contract value** in the public feed.

### 2.2 Other-Country Customs Feeds

The "open customs" landscape is highly heterogeneous. A working taxonomy:

**Public / scrapeable:**
- **India — DGCI&S + DGFT + Zauba.** Shipping bills, bills of entry, and
  invoices filed with Indian Customs are public; Zauba.com makes shipment-level
  records freely searchable (source:
  https://www.zauba.com/shipment_search). Aggregators pull from Indian
  Customs, DGFT (Directorate General of Foreign Trade), and port records
  (source: https://eximtradedata.com/india-import-export-data).
- **Brazil — SISCOMEX.** Brazilian importers operate via SISCOMEX (Foreign
  Trade Integrated System), with a Single Window (SW) Program centralizing
  electronic submission (source:
  https://www.trade.gov/country-commercial-guides/brazil-customs-regulations).
  Shipment-level data is licensable from the central system.
- **Mexico, Colombia, Peru, Ecuador, Paraguay, Uruguay, Argentina, Chile,
  Costa Rica.** A significant portion of LatAm publishes shipment-level
  customs data (this is why ImportGenius / Datamyne have strong LatAm coverage).
- **Vietnam, Pakistan, Sri Lanka.** All publicly available in licensed form
  (source: https://www.importgenius.com/pricing).
- **Turkey, Ukraine.** Publicly available; ImportGenius lists both.

**Historically available, currently sensitive:**
- **Russia.** Russian customs data was historically one of the deepest open
  feeds; sanctions and an internal classification decision after Feb 2022
  reduced public availability, but private aggregators continue to publish
  shipment-level data sourced from Russian customs and BoL filings (source:
  https://www.importglobals.com/russia-Import-data,
  https://russia-importdata.com/). Provenance is murkier and quality varies.
- **China.** Aggregate trade data is publicly published by China Customs;
  shipment-level data is not officially open but is widely available in the
  grey market. Bilateral Russia-China trade volumes continue to be published
  by both sides (source:
  https://kinacentrum.se/en/publications/china-russia-trade-in-early-2025-fueling-moscows-war-despite-headwinds/).

**Limited / aggregate only:**
- **South Korea.** KCS publishes aggregate but not shipment-level data publicly.
- **Taiwan.** Aggregate via the Bureau of Foreign Trade; shipment-level limited.
- **Japan, Singapore, Hong Kong.** Aggregate only — these are the cleanest
  examples of advanced economies that *do not* publish BoL-level data.
- **EU member states.** Comext is aggregate (HS6 / CN8); no shipment-level open
  data.
- **UK.** HMRC publishes aggregate only.

For an open-data competitor, the practical "global-coverage" target is
roughly **US + ~15 LatAm/Asia countries** with shipment-level records, plus
**Eurostat / Comtrade / national stats offices** for HS-level aggregates
elsewhere.

### 2.3 Maritime AIS

**Sources.**
- **Spire Maritime** — satellite + terrestrial AIS, GraphQL API, historical
  archive (source:
  https://spire.com/maritime/solutions/standard-ais/,
  https://documentation.spire.com/blog/).
- **MarineTraffic** — terrestrial-heavy + satellite, REST API, now part of
  Kpler (source: https://servicedocs.marinetraffic.com/tag/AIS-API/).
- **exactEarth** — satellite-AIS, acquired by Spire in 2021.
- **AISHub** — community-contributed terrestrial AIS, free with reciprocity
  agreements (source: https://www.aishub.net/api).
- **Windward** — analytics layer on top of AIS, focused on
  sanctions-evasion and dark-fleet detection.

**Schema (canonical AIS message fields).**

| Field | Description |
|---|---|
| MMSI | 9-digit Maritime Mobile Service Identity (primary key per voyage) |
| IMO | 7-digit International Maritime Organization number (lifetime ID) |
| Name | Vessel name |
| Callsign | Radio call sign |
| Ship Type | AIS type code (cargo, tanker, container, fishing, etc.) |
| Latitude / Longitude | Position |
| SOG | Speed over Ground |
| COG | Course over Ground |
| Heading | True heading |
| NAVSTAT | Navigational status code |
| Draught | Self-reported draft |
| Destination | Free-text — frequently misreported |
| ETA | Self-reported |
| Dimensions (A, B, C, D) | Distance from antenna to bow / stern / port / starboard |
| Timestamp | UTC |

(Source: https://documentation.spire.com/blog/,
https://servicedocs.marinetraffic.com/tag/AIS-API/.)

**How AIS connects to BoL data.** The join is **vessel name + voyage**:
- **AIS** carries `MMSI`, `IMO`, `Name`, and a sequence of (timestamp,
  lat/lon, port) observations.
- **BoL** carries `Vessel Name`, `Voyage Number`, `Foreign Port of Lading`,
  `Port of Unlading`, `Estimated Arrival Date`.
- Join: match (Vessel Name fuzzy ↔ AIS Name) AND (Foreign Port → US Port leg
  with arrival date within tolerance). The result is a *cargo manifest tied
  to a specific physical voyage*, which is exactly what Kpler does for crude
  and what Panjiva / ImportGenius do for containers.

The IMO number is the *only* lifetime-stable vessel identifier (MMSI changes
with flag), so any production-grade integration uses IMO as the primary
vessel key.

### 2.4 EU + Multilateral Statistics

- **Eurostat Comext** — full HS-level intra- and extra-EU trade. Data
  classified per the **Combined Nomenclature (CN)**: HS2 / HS4 / HS6 / CN8
  (~9,500 8-digit codes, annually revised). Free, programmatically
  accessible via Easy Comext and the `restatapi` R package (source:
  https://ec.europa.eu/eurostat/web/international-trade-in-goods/database,
  https://github.com/eurostat/restatapi/discussions/7).
- **UN Comtrade** — global HS / SITC trade data for 130+ countries; free
  + paid tiers; the standard cross-country reference (source:
  https://comtrade.un.org/).
- **US Census USA Trade Online** — monthly + cumulative US export and
  import data at HS-10 (source:
  https://www.census.gov/foreign-trade/Press-Release/current_press_release/ft900.pdf).
- **OECD trade flows** — bilateral and value-added / TiVA decomposition,
  most useful for input-output / Tier-N inference (not direct supplier
  identification).

These feeds are all **HS-code aggregate** — they tell you "country A
exported $X of HS code Y to country B in month M" but not "company P
shipped to company Q." For an equity-grade supplier graph, aggregate
trade is a *prior* on shipment-level data, not a substitute.

---

## 3. Common Data-Model Patterns Across Vendors

Synthesizing across Resilinc / Interos / Everstream / Sayari / Kpler /
Z2Data / Exiger / Panjiva / Bloomberg SPLC / FactSet Revere, an
"industry-standard" supply-chain graph schema is reasonably stable.

### 3.1 Core Entity Types

| Entity | Typical attributes |
|---|---|
| **Corporation / Legal Entity** | name, LEI, DUNS, country, registry ID, ultimate parent, status |
| **Site / Plant / Facility** | lat/lon, address, certifications, capacity, owner |
| **Part / Component / SKU** | manufacturer P/N, HS code, lifecycle state, BOM parents |
| **Material / Commodity** | classification, certification (RoHS, REACH, conflict-mineral) |
| **Vessel** | IMO, MMSI, type, capacity, flag, owner, operator |
| **Port / Airport / Terminal** | UN/LOCODE, country, geo |
| **Voyage / Shipment / BoL** | BL number, container, dates, carrier |
| **Lane** | (origin port, destination port, mode) — used by Everstream, Kpler |
| **Person** | for beneficial-ownership graphs (Sayari) |
| **Sanctions / Watchlist Entry** | OFAC SDN ID, BIS Entity List, etc. |
| **Risk Event** | type, geo, severity, time window, NLP source |

### 3.2 Core Edge Types

| Edge | Direction | Typical attributes |
|---|---|---|
| **owns / controls** | parent → child entity | %, type (direct/beneficial), as-of date |
| **operates** | entity → site | start / end, role |
| **employs / officer-of** | person → entity | role, start / end |
| **supplier-of** | A → B | start / end, confidence, dollar-volume estimate, source |
| **customer-of** | inverse | same |
| **manufactures** | site → part | volume, certifications |
| **uses / consumes** | product → part (BOM) | qty per assembly |
| **substitute-for** | part → part | compatibility level |
| **ships-to** | shipment → consignee | event-typed |
| **shipped-on** | shipment → vessel | container, voyage |
| **calls-at** | vessel → port | timestamp, draft, cargo state |
| **subject-to-restriction** | entity → sanctions list | as-of date, source |
| **affected-by** | entity / site → risk event | severity, last-seen |

### 3.3 Edge Attributes Universally Present

- `start_date`, `end_date` (or `last_seen`) — temporal validity
- `confidence` — 0..1 or low/medium/high
- `source_id` / `source_doc` / `provenance` — Sayari makes this mandatory
- `frequency` — for shipping edges, # shipments per month
- `dollar_volume` — Bloomberg SPLC estimates this; FactSet / Panjiva
  generally do not at the relationship level
- `tier` — explicit T1 / T2 / T3 label

### 3.4 Multi-Tier Traversal

Multi-tier visibility is the headline product feature for Resilinc, Interos,
Exiger, and Z2Data. Implementation patterns:
1. **Direct disclosure** (Resilinc): T1 supplier names their T2 suppliers
   under MNDA. Highest fidelity, narrowest coverage.
2. **Filings + BoL bridging** (Bloomberg / Panjiva / Sayari): T1 from filings,
   T2 inferred from T1's customs imports.
3. **Co-shipping inference** (Panjiva-style): if vessel V loads at port P
   on date D from shipper S, and another consignee C on the same vessel
   buys components matching firm F's BOM, infer S → F via C.
4. **Input-output prior** (OECD TiVA): given country/sector trade flows,
   probabilistically distribute the trade across the candidate firms.

### 3.5 Confidence / Strength Scoring

Common scoring inputs:
- **Recency**: when was the relationship last observed?
- **Frequency**: how many shipments / disclosures support it?
- **Source quality**: filing > customs > news > inferred
- **Convergence**: do multiple sources confirm?

Sayari and Bloomberg SPLC both expose these signals directly to the user;
Resilinc and Interos hide them behind a single risk score.

### 3.6 Entity Resolution

This is where vendor moats actually live. The canonical "Apple Inc" vs
"APPLE INC." vs "Apple Computer" problem is solved through a stack of:

1. **Normalization** — case, punctuation, suffix stripping ("Inc", "GmbH",
   "Pte Ltd"), address parsing (libpostal).
2. **Identifier crosswalks** — LEI (GLEIF, ~2.9M entities, free under
   open license — source: https://www.gleif.org/en/lei-data/gleif-api),
   DUNS (D&B, proprietary), tax IDs (EIN, VAT, GST), stock tickers.
3. **Deterministic matching** — exact identifier, exact (name + country +
   address) tuple.
4. **Probabilistic matching** — fuzzy name + address + phone + jurisdiction
   blocking, Fellegi-Sunter-style scoring.
5. **Hierarchy resolution** — direct parent → ultimate parent (GLEIF Level 2
   data publishes this for LEI'd entities).
6. **Manual curation** — every serious vendor has analyst review for
   high-stakes edges.

OpenCorporates publicly walks through this stack (source:
https://blog.opencorporates.com/2025/06/17/entity-resolution-for-data-aggregators/).
For ats-eqt, **GLEIF + OpenCorporates + libpostal + deterministic matching
on LEI** gets ~80% of the way to production-quality resolution for free.

---

## 4. Sourcing Strategies — How Vendors Actually Build the Graph

A categorical playbook of how an edge gets into a supply-chain graph:

| Strategy | Vendors that lean on it | Strengths | Weaknesses |
|---|---|---|---|
| **Filings (10-K Item 1, ASC 280 10% customer disclosure)** | FactSet Revere, Bloomberg SPLC | Auditable, long history, public companies | Only ≥10% customers, only public filers, late |
| **Bill-of-lading aggregation** | Panjiva, ImportGenius, ImportYeti, Datamyne, partly Sayari | Shipment-level, granular, real-time | US-imports-only-public; HS inferred; suppression; ocean only |
| **Disclosed proxy: corporate sustainability reports** | Resilinc + everyone (CDP, GRI) | Voluntary upstream disclosure (T2/T3) | Sparse, unverified, marketing-skewed |
| **News / NLP extraction** | Everstream, Interos, Resilinc | Real-time event detection | Noisy, mention ≠ relationship |
| **Trade-show / supplier directories** | All vendors as enrichment | Cheap, broad | Stale, marketing-biased |
| **Direct corporate disclosure (paid contributors)** | Resilinc primarily; Interos, Exiger secondarily | Sub-tier visibility | Coverage = whoever paid; selection bias |
| **Government registries** | Sayari, Interos | Authoritative entity identity | Doesn't directly capture supply relationships |
| **Customs partnerships** | Sayari (CBP), Kpler (gov) | Authoritative trade flows | Restricted licensing |
| **Inferred via co-shipping / co-logistics patterns** | Panjiva, Everstream | Fills gaps in disclosed graph | Probabilistic, false positives |
| **Satellite / AIS / overhead imagery** | Kpler, Spire, Windward | Physical-world ground truth | Maps to vessels not corps; cargo inference is hard |
| **ERP / TMS direct integration with customers** | Everstream, Interos iQ | Fully accurate for that customer's chain | Single-tenant; not transferable |
| **Distributor inventory + part datasheets** | Z2Data, Exiger | Component-grade granularity | Electronics-specific |

The key strategic observation for ats-eqt: **no single source covers the
graph**. The vendor moats are (a) the *combination* of sources, (b) the
*entity-resolution* that stitches them together, and (c) the *temporal
freshness* layer. An open-data competitor needs all three.

---

## 5. Sources

### Vendor primary
- [Resilinc — Agentic Supply Chain Resiliency](https://resilinc.ai/)
- [Resilinc — Solutions](https://www.resilinc.com/solutions/)
- [Resilinc — 2026 Gartner Magic Quadrant Leader](https://www.globenewswire.com/news-release/2026/05/06/3289253/0/en/Resilinc-Named-a-Leader-in-2026-Gartner-Magic-Quadrant-for-Supplier-Risk-Management-Solutions.html)
- [Resilinc — Databricks Built-on Partner](https://markets.financialcontent.com/stocks/article/gnwcq-2026-3-5-resilinc-becomes-validated-databricks-built-on-partner-with-agent-factory-for-supply-chain-risk-and-compliance)
- [Resilinc — FedRAMP Authorization](https://www.globenewswire.com/news-release/2026/04/01/3266667/0/en/Resilinc-Achieves-FedRAMP-Authorization-Strengthening-Secure-AI-Driven-Supply-Chain-Risk-Management-for-Government-and-Regulated-Industries.html)
- [Interos — interos.ai homepage](https://www.interos.ai/)
- [Interos — Software / Knowledge Graph](https://www.interos.ai/our-software)
- [Interos — iQ launch (Apr 2026)](https://www.businesswire.com/news/home/20260428661670/en/interos.ai-Launches-iQ-to-Elevate-Supply-Chain-Risks-to-the-C-Level)
- [Everstream Analytics homepage](https://www.everstream.ai/)
- [Everstream — Risk Scoring methodology](https://www.everstream.ai/articles/rate-supply-chain-risk-with-scoring/)
- [Everstream — Global Monitoring](https://www.everstream.ai/platform/global-monitoring/)
- [Sayari Graph — Platform overview](https://sayari.com/platform/graph/)
- [Sayari — CBP $7.8M contract](https://sayari.com/resources/sayari-awarded-7-8m-enterprise-trade-analytics-contract-to-support-u-s-customs-border-protection/)
- [Sayari — All 50 US states searchable](https://sayari.com/resources/official-company-data-from-all-50-us-states-now-searchable-in-sayari-graph/)
- [Sayari — Global Trade](https://sayari.com/global-trade/)
- [Kpler homepage](https://www.kpler.com/)
- [Kpler — ClipperData acquisition press release](https://www.kpler.com/blog/press-release-kpler-acquires-clipperdata)
- [Bloomberg — Kpler buys ClipperData](https://www.bloomberg.com/news/articles/2021-09-08/commodity-data-firm-kpler-buys-clipperdata-in-u-s-expansion)
- [Kpler — MarineTraffic Data Services](https://www.kpler.com/product/maritime/data-services)
- [Z2Data homepage](https://www.z2data.com/)
- [Z2Data — Altium integration](https://resources.altium.com/p/z2data-integration-advantages)
- [Z2Data — Part Risk Manager (1B+ components)](https://www.z2data.com/part-risk-manager/overview)
- [Exiger — Technology Platform](https://www.exiger.com/innovations-technology/)
- [Exiger homepage](https://www.exiger.com/)
- [TealBook — Supplier.io acquisition (Apr 2026)](https://www.businesswire.com/news/home/20260402407775/en/Supplier.io-Acquires-TealBook-Connecting-Supplier-Intelligence-and-Enterprise-Data-Management)
- [ImportYeti — Bellingcat OSINT toolkit](https://bellingcat.gitbook.io/toolkit/more/all-tools/importyeti)
- [ImportYeti — Software Advice profile](https://www.softwareadvice.com/bi/importyeti-profile/)
- [ImportGenius pricing / coverage](https://www.importgenius.com/pricing)
- [Datamyne — Bill of Lading Database](https://www.datamyne.com/our-product/bill-of-lading-database/)
- [Datamyne FAQ — sources](https://www.datamyne.com/faq/)
- [Panjiva — S&P Global product page](https://www.spglobal.com/market-intelligence/en/solutions/products/panjiva-supply-chain-intelligence)

### Trade data — public sources
- [19 CFR § 4.7a — AMS publicly disclosable elements](https://www.law.cornell.edu/cfr/text/19/4.7a)
- [eCFR — 19 CFR 4.7a current version](https://www.ecfr.gov/current/title-19/chapter-I/part-4/subject-group-ECFR9356f0b8b5be866/section-4.7a)
- [CBP Public Data Portal](https://www.cbp.gov/newsroom/stats/cbp-public-data-portal)
- [Federal Reserve — Bill of Lading Data in International Trade Research](https://www.federalreserve.gov/econres/feds/files/2021066pap.pdf)
- [Data Liberation Project — CBP Bills of Lading](https://www.data-liberation-project.org/requests/cbp-bills-of-lading/)
- [Federal Register — Electronic Export Manifest for Vessel Cargo (2026)](https://www.federalregister.gov/documents/2026/02/10/2026-02662/electronic-export-manifest-for-vessel-cargo)
- [Zauba — India shipment search](https://www.zauba.com/shipment_search)
- [eximtradedata — India customs data](https://eximtradedata.com/india-import-export-data)
- [trade.gov — Brazil Customs Regulations / SISCOMEX](https://www.trade.gov/country-commercial-guides/brazil-customs-regulations)
- [Eurostat — International Trade in Goods database (Comext)](https://ec.europa.eu/eurostat/web/international-trade-in-goods/database)
- [Eurostat restatapi — monthly trade by HS code](https://github.com/eurostat/restatapi/discussions/7)
- [UN Comtrade](https://comtrade.un.org/)
- [US Census — USA Trade Online (FT900)](https://www.census.gov/foreign-trade/Press-Release/current_press_release/ft900.pdf)
- [Spire — Standard AIS](https://spire.com/maritime/solutions/standard-ais/)
- [Spire — Maritime documentation](https://documentation.spire.com/blog/)
- [MarineTraffic — AIS API documentation](https://servicedocs.marinetraffic.com/tag/AIS-API/)
- [AISHub — community AIS API](https://www.aishub.net/api)

### Methodology, comparisons, supporting
- [Culot et al. 2023 — Using supply chain databases in academic research](https://onlinelibrary.wiley.com/doi/full/10.1111/jscm.12294)
- [Wharton Lippincott — Untangling the Supply Chains Part 1](https://lippincottlibrary.wordpress.com/2021/12/10/untangling-the-supply-chains-part-1/)
- [GLEIF — LEI Data API](https://www.gleif.org/en/lei-data/gleif-api)
- [GLEIF — LEI Mapping](https://www.gleif.org/en/lei-data/lei-mapping)
- [OpenCorporates — Entity resolution for data aggregators](https://blog.opencorporates.com/2025/06/17/entity-resolution-for-data-aggregators/)
- [Wikipedia — Legal Entity Identifier](https://en.wikipedia.org/wiki/Legal_Entity_Identifier)
- [Ubisecure — Comparing organisation identifiers (DID, GLN, DUNS, BIC, TIN, LEI)](https://www.ubisecure.com/legal-entity-identifier-lei/comparing-organisation-identifiers/)
- [Neo4j — Pharmaceutical supply chain demo](https://neo4j.com/developer/demos/supply_chain-demo/)
- [Neo4j — AI-Driven Supply Chain Insights with Knowledge Graphs](https://neo4j.com/developer/demos/supply_chain-ai/)
- [Swedish National China Centre — China-Russia trade in early 2025](https://kinacentrum.se/en/publications/china-russia-trade-in-early-2025-fueling-moscows-war-despite-headwinds/)
- [FAS — Tracking Proliferation through Trade Data](https://fas.org/wp-content/uploads/media/Tracking-Proliferation-through-Trade-Data.pdf)

---

*End of document. Next steps for ats-eqt strategy: see
`research/strategy/open-data-supply-chain-build-vs-buy.md` (to be drafted) and
`research/strategy/entity-resolution-stack.md` (to be drafted).*
