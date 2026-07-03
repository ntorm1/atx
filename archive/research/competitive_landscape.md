# ats-eqt — Competitive Landscape & Strategic Synthesis

**Date:** 2026-05-09 (research wave 1)
**Audience:** ats-eqt founders, engineering, GTM
**Inputs:** [vendors/](vendors/), [sources/](sources/), [schemas/](schemas/)

This is the synthesis layer. It does three things:
1. Lays out a **vendor capability matrix** so we can see who has what.
2. Identifies the **moats** of incumbents and where they are thin enough to attack.
3. Articulates an **ats-eqt strategic positioning statement** and the resulting product wedge.

---

## 1. Vendor capability matrix

Capabilities are scored on a coarse 0–3 scale (3 = best-in-class, 0 = absent). Numbers are directional — see linked files for citations and `[unverified]` flags.

### 1.1 Equity fundamentals

| Capability                              | Compustat (S&P) | FactSet Fundamentals | Worldscope (LSEG) | Bloomberg | SEC XBRL (open) |
|------------------------------------------|:---------------:|:--------------------:|:-----------------:|:---------:|:---------------:|
| History depth (years)                    | 3 (1950)        | 3 (1980s)            | 3 (1980s)         | 2 (1990s) | 1 (2009 US)     |
| Field count (standardised items)         | 3               | 3                    | 3                 | 3         | 1 (raw tags)    |
| Point-in-time / first-reported snapshot  | 3 (since 1987)  | 3                    | 2                 | 2         | 1 (DIY)         |
| Restatement audit trail                  | 3               | 3                    | 2                 | 2         | 1               |
| Global coverage (non-US)                 | 2               | 3                    | 3                 | 3         | 1               |
| Estimates / consensus                    | 2 (CIQ)         | 3                    | 3 (I/B/E/S)       | 3 (BEst)  | 0               |
| Open / redistributable                   | 0               | 0                    | 0                 | 0         | 3               |
| Per-seat price (~)                       | high            | high                 | high              | very high | $0              |

Where ats-eqt should land in v1: **fundamentals coverage equivalent to mid-tier Compustat**, point-in-time correct, US-first via XBRL with non-US ramp through ESEF + EDINET + HKEX. Lower per-seat cost, **redistributable to subscribers** (within source license terms).

### 1.2 Supply chain graph

| Capability                              | FactSet Revere | Bloomberg SPLC | S&P Panjiva | Resilinc | Sayari | Open data combo |
|------------------------------------------|:--------------:|:--------------:|:-----------:|:--------:|:------:|:---------------:|
| Entity coverage                          | 2              | 2              | 3           | 2        | 3      | 2 (with effort) |
| Edge coverage                            | 2              | 2              | 3 (BoL)     | 2 (NDA)  | 2      | 2               |
| Multi-tier (T2/T3) traversal             | 2              | 1              | 3 (BoL)     | 3        | 2      | 2               |
| Customs / shipment level                 | 0              | 0              | 3           | 0        | 1      | 2 (CBP AMS)     |
| Filings-derived (10-K Item 1, Ex 21)     | 3              | 3              | 1           | 0        | 2      | 3 (XBRL+NER)    |
| AIS / vessel level                       | 0              | 1              | 1           | 0        | 0      | 2 (AIS public)  |
| Beneficial-ownership lineage             | 1              | 1              | 0           | 0        | 3      | 2 (GLEIF+OC)    |
| Confidence / strength scoring            | 2              | 2              | 1           | 2        | 2      | 2 (build)       |
| Open / redistributable                   | 0              | 0              | 0           | 0        | 0      | 3               |

The standout finding: **the three incumbent graphs (Revere, SPLC, Panjiva) only overlap ~43% on suppliers** (Culot 2023). No vendor has the truth. An open-data combo can credibly produce a *fourth* graph that exceeds any one incumbent for parent-attributable lineage when built well.

### 1.3 Identifier / symbology

| Identifier        | Owner       | Open? | License                 | Strategic role for ats-eqt |
|-------------------|-------------|:-----:|-------------------------|----------------------------|
| **CIK**           | SEC         | yes   | public domain           | Primary US filer ID        |
| **LEI**           | GLEIF       | yes   | free with terms         | Primary global entity ID   |
| **FIGI**          | OMG / BBG   | yes   | MIT-licensed dataset    | Primary security ID        |
| **ISIN**          | local NNAs  | partial | use rights vary        | Secondary security ID      |
| PermID            | LSEG        | yes (free tier) | terms-of-use      | Secondary entity ID        |
| Wikidata QID      | Wikimedia   | yes   | CC0                     | Disambiguation hub         |
| OpenCorporates    | OC Ltd      | partial | £12k+/yr commercial    | Reference only — cannot redistribute |
| CUSIP             | FactSet/CGS | no    | licensed                | Avoid at spine             |
| SEDOL             | LSEG        | no    | licensed                | Avoid at spine             |
| RIC               | LSEG        | no    | licensed                | Avoid at spine             |
| Bloomberg Ticker  | BBG         | no    | licensed                | Avoid at spine             |

**Recommended spine:** `(LEI | CIK) → (FIGI | ISIN) → listing` with all proprietary IDs handled as aliases under licensing constraints, or omitted.

### 1.4 Sector classification

| Taxonomy | Owner | Open? | Use for ats-eqt |
|----------|-------|:-----:|-----------------|
| **NAICS** | US Census | yes (public domain) | Default |
| **TRBC**  | LSEG (open via PermID) | yes (free) | Recommended secondary — finance-aware |
| GICS      | S&P + MSCI | no | License if customer demands; not at spine |
| ICB       | FTSE Russell | no | Same |
| BICS      | Bloomberg | no | Same |
| RBICS L1–L6 | FactSet | no | Same |
| SIC       | US Census | yes | Already in EDGAR — keep for compatibility |

### 1.5 Delivery technology

| Vendor | Snowflake share | Parquet/S3 | API REST | Bulk file | Terminal | Cloud (AWS/Azure/GCP) |
|--------|:---------------:|:----------:|:--------:|:---------:|:--------:|:---------------------:|
| FactSet | yes | yes (Open:FactSet) | yes | yes (DataFeed) | Workstation | yes |
| S&P    | yes (Marketplace) | yes (Databricks) | yes | yes (Xpressfeed) | Capital IQ Pro | yes |
| LSEG   | yes (DataScope) | partial | yes (RDP) | yes (Tick History) | Workspace | yes |
| Bloomberg | yes (DL+) | partial | yes (BQL) | yes (DL) | Terminal | partial |
| **ats-eqt target** | yes | **yes (primary)** | yes | yes | no | yes |

ats-eqt should be **API+parquet first**, with Snowflake share as an enterprise add-on once we cross the customer-onboarding threshold. Terminal / GUI is a distraction in v1.

---

## 2. Where the moats are thin

Moats ranked by how attackable they are with open data + good engineering.

### 2.1 Most attackable
1. **US-centric fundamentals.** SEC XBRL gets to 75–85% of Compustat coverage with disciplined normalization. The remaining 15–25% is non-trivial (ratios, derived items, reclass mapping) but tractable. Bitemporal capture from filing inception is straightforward — XBRL filings are timestamped.
2. **Supply-chain graphs derived from public filings + BoL.** Exhibit 21 (subsidiary lineage), 10-K Item 1 (customer concentration), Form SD (conflict minerals), and CBP AMS (US-import shipments) combine into a graph that matches or beats single-vendor coverage on parent-attributable suppliers. The work is entity resolution, not data acquisition.
3. **HS code imputation on AMS data.** All vendors infer HS from free-text Description-of-Goods using older NLP. Modern LLM-based extraction with calibrated uncertainty is a step-change improvement.
4. **Concordance / corporate-action ledger.** FactSet's Concordance API uses TF-IDF on character trigrams — directly reproducible, well-understood. Building a clean alias table that survives M&A, spinoffs, ticker changes, and share-class splits is engineering work, not data work.

### 2.2 Moderately attackable
5. **Estimates.** I/B/E/S, BEst, FactSet Estimates require *broker contributions*, which is a network effect. ats-eqt cannot replicate the broker network in v1. **Workaround:** scrape sell-side reports, harvest analyst targets from filings, and offer a "consensus-from-public" feed that's clearly secondary in quality but adequate for many use cases.
6. **ESG / sustainability.** EU CSRD/ESRS will publish structured XBRL data starting 2026-2028. CDP free tier exists. FactSet Truvalue and similar are NLP over public filings/news — replicable.
7. **Non-US fundamentals.** ESEF (EU), EDINET (Japan), HKEX, SEDAR+ all expose machine-readable filings. Entity-resolution and taxonomy-mapping work, but the data is there.

### 2.3 Hard to attack (true moats)
8. **GICS sector classification.** Locked behind S&P + MSCI license. Customer demand will drive licensing eventually — but not in v1.
9. **CUSIP at spine.** FactSet owns CGS post-2022. Cannot use without license.
10. **Real-time tick data.** Out of ats-eqt's scope. Irrelevant.
11. **Resilinc-style contributory NDA networks.** Paid disclosure under MNDA is a network effect; the participants license to Resilinc specifically. ats-eqt cannot replicate cheaply, but also doesn't compete here directly.
12. **Capital IQ private-company financials.** Paid contributors + analyst capture. Not feasible from open data.

---

## 3. ats-eqt strategic positioning

### 3.1 The thesis in one sentence
> ats-eqt is the open-data fundamentals + supply-chain dataset that ships **point-in-time-correct fundamentals** and a **parent-attributable supply-chain graph** at API-first delivery, priced for mid-market and quants who can't justify a Bloomberg seat — leveraging ats-core's columnar storage as the engine.

### 3.2 The product wedge
- **v0 (months 0–3):** US-only fundamentals (S&P 500 + Russell 2000) sourced from SEC XBRL, with full PIT capture. Concordance + corp-actions ledger. Identifier spine = CIK + LEI + FIGI. Delivery = Python client + parquet bulk. **Goal: replace a $50–500k Compustat academic license for 80% of use cases.**
- **v1 (months 3–9):** Add Exhibit 21 + Item 1 + Form SD + CBP AMS to produce the **first open parent-attributable supply-chain graph** with confidence scoring. Hand-graded gold-standard supplier validation set used for benchmarking. **Goal: have data that demonstrably beats Panjiva on parent-attribution and matches Revere on coverage of S&P 500.**
- **v2 (months 9–18):** Non-US fundamentals via ESEF + EDINET + HKEX. AIS-derived shipping flows. Synthetic estimates feed. Snowflake share for enterprise. **Goal: be the no-brainer choice for any team building US/EU multi-asset research that doesn't already have a Bloomberg / FactSet enterprise contract.**

### 3.3 Differentiators we will lead with
1. **Bitemporal correctness as a first-class API.** Every query takes `as_of_knowledge` and `as_of_economic` parameters. No competitor delivers this cleanly at API level.
2. **Source-attributable everything.** Every fact links back to the filing (CIK + accession #), the customs record (CBP BL #), the news article (URL), or the structured XBRL fact ID. Auditability beats opaqueness.
3. **Open identifier spine.** No CUSIP, no SEDOL, no RIC at the core — only CIK, LEI, FIGI, ISIN. Customers can redistribute derived datasets.
4. **Graph + facts in the same query layer.** ats-core's columnar engine handles fundamentals; an integrated graph store (built on top, not separate) handles supply-chain edges. One join layer.
5. **Cost model.** Sub-$10k/seat for the standard tier; bulk parquet cheaper still. No terminal, no per-CPU enterprise licensing.

### 3.4 What we explicitly will NOT do in the first 18 months
- No real-time / intraday data.
- No private-company financials.
- No paid-contributor estimates network.
- No GICS / RBICS / BICS licensing — NAICS + TRBC + (later) homegrown.
- No terminal product.
- No tick / quote data.
- No alt-data (satellite, credit-card panels, web scrapes outside structured sources).

### 3.5 Distribution & GTM hypotheses (for separate validation)
1. Quant funds and academic finance shops are over-served on price by Compustat. There is room.
2. Mid-market hedge funds and corporate-strategy teams will pay for an *audited* supply-chain graph more than for incremental fundamentals.
3. Procurement / risk teams (separate buyer) will pay for the supply-chain product even when fundamentals aren't a fit. Two segments, one dataset.
4. Open-source library + paid hosted SaaS is the right model — like Hugging Face for finance reference data. ats-eqt should likely have a permissive open-core for the schema + parsers, with paid hosted DBs and Snowflake shares.

---

## 4. Risks and unknowns

- **Licensing of redistributed XBRL.** SEC EDGAR is public domain, but third-party-tagged variants (XBRL US calculations) may carry restrictions. Need a legal review before commercializing derived datasets.
- **Customs data redistribution.** US CBP AMS is FOIA-public, but downstream aggregators (Panjiva, ImportYeti) layer their own ToS. ats-eqt must source directly from CBP, not from aggregators, to be safe.
- **GLEIF terms-of-use** allow free use but with attribution; need to confirm commercial-distribution clauses.
- **OpenCorporates is NOT redistributable** at open-data-grade pricing. We should design around it — use as *internal reference only*, not as a published spine.
- **Resilinc / Sayari moats are real.** ats-eqt won't compete head-on with paid contributory networks.
- **Coverage gap for late-arriving / amended filings** is a known XBRL pain point — needs robust late-arrival handling in v0.
- **Schema drift in the US GAAP taxonomy** (annual updates) requires a normalization layer with explicit versioning.
- **CUSIP/CGS pricing leverage.** If ats-eqt grows, FactSet's leverage over CUSIP becomes a friction point at customer integrations. Plan an explicit "CUSIP-free" mode.

---

## 5. Wave-2 research priorities (open questions)

- Pricing benchmarks (Vendr/G2) for Capital IQ Pro, FactSet Workstation, LSEG Workspace per-seat.
- Snowflake share latency SLAs from S&P, FactSet, Bloomberg.
- Confidence-score scales for Revere and SPLC (vendor pages thin).
- ESEF country-by-country adoption maturity (only some EU countries publish via national OAMs cleanly).
- EDINET / HKEX rate limits.
- Legal: GLEIF commercial-redistribution clause review; SEC XBRL derived-dataset licensing.
- Quantify the gold-standard supplier validation set we'd need to build in v1 and the cost of producing it.

See [INDEX.md](INDEX.md) for the full research entry-point and links into each detailed file.
