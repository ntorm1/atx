# ats-eqt — 13F Institutional Holdings Dataset

**Status:** Research, v0.1
**Audience:** ats-eqt engineering team (ingestion, storage, query); ats-core team designing request-shape primitives
**Scope:** the 13F institutional holdings dataset — primary source (SEC EDGAR), commercial vendor landscape (FactSet, S&P, Bloomberg, LSEG; mid-tier; API; bulk/academic), known data-quality traps, the CUSIP-licensing problem, and a recommended bitemporal long-format schema for ats-eqt that fits the pattern in `schemas/data_models_and_methodology.md`.
**Last updated:** 2026-05-09

---

## 0. Executive summary

Form 13F is the cleanest open-data competitive opportunity in ats-eqt's Phase 0 stack. Unlike fundamentals, where the moat is normalisation across 750+ items and 30 years of restatement history, 13F's *raw substrate is fully public, structured, and free*: a single XML schema, ~5,300 quarterly filers, 45-day cadence, 17,500+ securities, fully ingestable in under a day. The incumbent moat is therefore not the data itself but four value-adds:

1. **Entity resolution** — CIK is stable; the *manager* is not. Acquisitions, sub-advisor relationships, fund-of-fund structures, and renames produce ~2–4% noisy filer linkage every quarter (source: <https://wrds-www.wharton.upenn.edu/pages/support/research-wrds/research-guides/research-note-regarding-thomson-reuters-ownership-data-issues/>).
2. **Identifier crosswalk** — 13F is keyed by CUSIP, which is owned (since March 2022) by FactSet/CGS and is the subject of an active antitrust class action. Redistributing a CUSIP-keyed dataset is legally fraught. The accepted workaround is FIGI, which the SEC explicitly permitted as an alternative 13F identifier in the June 2022 amendments (source: <https://www.openfigi.com/about/regulations>).
3. **Cross-form linkage** — manager-level 13F joined to fund-level N-PORT (monthly disclosure as of August 2024 amendments) gives a complete picture of asset-manager positioning that neither form provides alone.
4. **Backfile cleanup** — the legacy WRDS Thomson Reuters s34 file (CDA/Spectrum) has well-documented errors pre-2013, including missing BlackRock holdings (source: <https://wrds-www.wharton.upenn.edu/pages/support/research-wrds/research-guides/research-note-regarding-thomson-reuters-ownership-data-issues/>).

WhaleWisdom is the closest mid-market benchmark: $300/yr Standard, $500/yr Pro, custom Enterprise (source: <https://whalewisdom.com/info/subscription_info>). The 12-month build to feature parity is dominated by entity resolution and crowding analytics, not by ingestion.

---

## Part A — The primary source: SEC EDGAR

### A.1 Form types

Four EDGAR submission types within the 13F family:

| Form | Purpose | Notes |
|---|---|---|
| `13F-HR` | Initial holdings report | The primary public artefact; contains the structured `informationTable` XML. |
| `13F-HR/A` | Amendment | Two amendment-types: "restatement" (replaces prior filing in full) and "additional information" (supplements). Filers must indicate which. |
| `13F-NT` | Notice of no holdings | Filed when all reportable holdings are reported on someone else's 13F (e.g., subadvisor whose holdings appear on the master manager's filing). |
| `13F-CTR` / `13F-CTR/A` | Confidential treatment request | Mandatory electronic filing as of 2023-02-28 (source: <https://www.sidley.com/en/insights/newsupdates/2022/07/sec-adopts-rules-requiring-electronic-filing-for-form-13f-confidential-treatment-requests>). Hides specified holdings until the SEC grants/denies/expires the request. Granted requests typically last up to 1 year. |

A pending `13F-2` / Form SHO short-position regime exists in rule but with compliance now extended to **2028-01-02** after the December 2025 exemption order (source: <https://www.sec.gov/newsroom/press-releases/2025-37>). First filings will not be due until 2028-02-14. ats-eqt should not plan ingestion for short-position data until that horizon firms up.

### A.2 Filer universe and threshold

- **Threshold:** investment discretion over ≥ $100 million in Section 13(f) securities on the last trading day of any month in the calendar year (source: <https://www.investor.gov/introduction-investing/investing-basics/glossary/form-13f-reports-filed-institutional-investment>).
- **Cadence:** quarterly, due 45 calendar days after quarter end (i.e., Feb 14 / May 15 / Aug 14 / Nov 14, sliding to the next business day).
- **Population:** historically ~5,000 filers; FactSet reported "5,200+ 13F filers" collected for 2017 (source: <https://www.wiso.uni-hamburg.de/bibliothek/recherche/datenbanken/unternehmensdaten/factset-ownership.pdf>).
- **History:** 13F has been required since 1979 under Section 13(f)(1) of the Securities Exchange Act, but only structured XML filings since 2013-Q2 are reliably parseable. Earlier filings are paper/text and were aggregated by CDA Spectrum (now Thomson Reuters s34, on WRDS).

### A.3 Section 13(f) securities list

The SEC publishes an **Official List of Section 13(f) Securities** quarterly, listing the ~17,500 securities that qualify (source: <https://www.sec.gov/rules-regulations/staff-guidance/official-list-section-13f-securities>; FY2026Q1 list at <https://www.sec.gov/files/investment/13flist2026q1.pdf>). Includes:

- US exchange-traded common stock (NYSE, NASDAQ, AMEX)
- Closed-end fund shares
- ETF shares
- Certain equity options and warrants
- Convertible debt that converts into a 13(f) security

Notable exclusions: foreign securities not US-listed (no ADR), private equity, most fixed income, derivatives that don't reference a 13(f) security. ats-eqt completeness claims about institutional ownership must be qualified: *"institutional ownership of US-listed Section-13(f)-eligible long positions only"*.

The list itself has a CUSIP-licensing wrinkle — the SEC publishes only PDF, not a clean machine-readable format, because the ABA asserts copyright over the CUSIPs in the list (source: <https://www.federalreserve.gov/apps/proposals/comments/FR-0000-0136-01-C15>). Practitioners parse the PDF or use third-party machine-readable copies.

### A.4 The structured INFORMATION TABLE schema

The authoritative spec is the **EDGAR Form 13F XML Technical Specification** (current draft v1.6) (source: <https://www.sec.gov/edgar/filer-information/specifications/form13fxmltechspec-draft>). The XSD defines two top-level documents:

- **Cover Page** (`coverPage`) — manager identification, period, filing-type code, signature, list of `otherIncludedManagers`, `summaryPage` totals.
- **Information Table** (`informationTable`) — repeated `infoTable` blocks, one per (security, manager-attribution) row.

Field-by-field schema for `<infoTable>`:

| Field | Type / constraint | Semantics |
|---|---|---|
| `nameOfIssuer` | string, ≤ 200 chars | Issuer name as it appears on the Official List, or abbreviated. **Not a stable join key** — same issuer appears with slightly different strings across filers. |
| `titleOfClass` | string, ≤ 150 chars | E.g., "COM", "CL A", "ADR", "PUT", "CALL", "NOTE". Free text — use only for display. |
| `cusip` | string, exactly 9 chars | CUSIP-9 of the security. **Primary security identifier.** See Part B for licensing. |
| `figi` | string, exactly 12 chars, **optional** | FIGI-12 alternative identifier permitted since 2022 amendments. Optional, sparsely populated in practice. |
| `value` | int64, ≤ 16 digits | Market value. **Pre-2023-Q1: rounded to nearest $1,000. From 2023-Q1 onward: rounded to nearest $1.** Cutover at 2023-01-03 amendments compliance date — backfill code must branch on filing date (source: <https://www.toppanmerrill.com/blog/sec-updates-edgar-on-jan-3-2023-for-form-13f-changes/>). |
| `shrsOrPrnAmt/sshPrnamt` | int64 | Quantity. Either a share count (when type = SH) or a principal amount in dollars (when type = PRN). |
| `shrsOrPrnAmt/sshPrnamtType` | enum: `SH`, `PRN` | SH = shares; PRN = principal amount (used for convertible debt). |
| `putCall` | enum, optional: empty, `Put`, `Call` | When set, the row is a derivative position, not a stock position. |
| `investmentDiscretion` | enum: `SOLE`, `DFND` (defined), `OTR` (other) | Whose discretion controls the position. |
| `votingAuthority/Sole` | int64 | Shares with sole voting authority. |
| `votingAuthority/Shared` | int64 | Shares with shared voting authority. |
| `votingAuthority/None` | int64 | Shares with no voting authority. |
| `otherManager` | string, optional, repeating | Comma-separated list of indices into the cover-page `otherManagers2` list, identifying co-filers. |

Source for the field list: SEC EDGAR Form 13F XML Technical Specification v1.6 (source: <https://www.sec.gov/edgar/filer-information/specifications/form13fxmltechspec-draft>) and v1.2 (source: <https://irdirect.net/scripts/xslt/13F/EDGAR%20Form%2013%20F%20XML%20Technical%20Specification.pdf>). XBRL has *not* been adopted for Form 13F; the format is plain XML constrained by XSD `[unverified — confirm there is no XBRL transition planned]`.

### A.5 EDGAR APIs

The free SEC dissemination endpoints:

- **Submissions index by CIK:** `https://data.sec.gov/submissions/CIK{cik:0>10}.json` — every filing the entity has made, with form type, accession number, filing date, primary document filename. Real-time updates (sub-second processing delay) (source: <https://www.sec.gov/search-filings/edgar-application-programming-interfaces>).
- **Full-text search:** `https://efts.sec.gov/LATEST/search-index?q=...&forms=13F-HR` — JSON, ~10 req/s rate limit (source: same).
- **RSS feed:** `https://www.sec.gov/cgi-bin/browse-edgar?action=getcompany&type=13F-HR&output=atom` — filterable by company, CIK, form type.
- **Bulk index:** `https://www.sec.gov/Archives/edgar/full-index/{year}/QTR{n}/form.idx` — quarterly cumulative index of all submissions; ats-eqt's preferred backfill source.
- **Per-filing artefacts:** `https://www.sec.gov/Archives/edgar/data/{cik}/{accession_no_dashes}/{primary_doc.xml}` — the actual XML; for 13F-HR the structured table is typically `infotable.xml` or `Form13FInfoTable.xml`.

Rate limit: 10 requests/second per user, declarative `User-Agent` header required (e.g., `User-Agent: ats-eqt research@example.com`).

---

## Part B — The CUSIP-licensing problem

This is the single largest legal-operational risk for an open-data 13F product. Status as of 2026-05:

### B.1 Background

CUSIP Global Services (CGS) was acquired by **FactSet from S&P Global in March 2022 for approximately $1.925B**, transferring operational control of the standard but not its underlying copyright claim (source: <https://www.waterstechnology.com/regulation/7936086/class-action-lawsuit-takes-aim-at-cusip-sp-factset-aba>). The American Bankers Association (ABA) holds the asserted copyright; CGS operates the database under licence.

A class action filed 2022-03-04 in SDNY by **Dinosaur Financial Group and Swiss Life Investment Management** alleges CGS, S&P, ABA, and FactSet conspired to leverage control of CUSIP issuance into unlawful control of *downstream use*, in violation of Sherman §§1–2, Clayton §4, and the Copyright Act. The plaintiffs' core legal argument: a 9-character alphanumeric identifier is a **fact, not copyrightable expression** under Feist v. Rural (source: <https://www.napa-net.org/news-info/daily-news/class-action-suit-challenges-big-cusip-licensing-fees>). The case remains active `[unverified — exact docket status as of May 2026]`.

### B.2 Practical exposure for ats-eqt

The position taken by CGS/FactSet in licensing letters (as documented at <https://www.waterstechnology.com/data-management/7951850/as-legal-letters-fly-cusip-licensing-debate-rolls-on>): commercial redistribution of a CUSIP-keyed dataset, even when the CUSIPs were sourced from a public SEC filing, requires a CGS subscriber licence ($25–$500k+ annually depending on use case `[unverified — pricing not publicly disclosed]`).

Three observed industry stances:

1. **Pay the licence (FactSet, S&P, Bloomberg, LSEG, WRDS).** Required for end-user terminals.
2. **Strip CUSIPs at publication boundary (most retail-facing 13F sites).** WhaleWisdom and 13F.info both expose CUSIPs in the UI for browsing, citing the SEC public-record carve-out, but neither offers a downloadable CUSIP-keyed bulk file without auth `[unverified — sample-checked the public WhaleWisdom UI; no bulk endpoint observed]`. Fintel's free 13F endpoints return tickers/issuer names primarily; CUSIP appears in some response shapes (source: <https://developers.fintel.io/reference/ownership-1>).
3. **Cross-walk to FIGI at ingest, expose only FIGI downstream.** The SEC's June 2022 amendments explicitly permit FIGI as an alternative 13F identifier (source: <https://www.openfigi.com/about/regulations>), and FIGI is published under the **MIT licence at zero cost** with no redistribution restrictions (source: <https://www.openfigi.com/>).

### B.3 ats-eqt recommendation

**Adopt option 3.** At ingest:

1. Parse `cusip` from `<infoTable>`.
2. Resolve `cusip → figi` via the OpenFIGI API (`POST https://api.openfigi.com/v3/mapping`, free, 25 req / 6s anonymous, 1000 req / 6s authenticated).
3. Resolve `cusip → ats-eqt internal security_id` via the security-master alias table (which itself stores CUSIP for *internal* match but never exposes it on the API boundary).
4. Persist CUSIP in a **non-redistributable internal column** (`alias_cusip`, behind a query-time ACL).
5. The public-facing `holding_13f` view exposes `figi`, `ticker`, `entity_id`, never `cusip`.

This mirrors how OpenFIGI itself was constructed (source: <https://www.bloomberg.com/company/press/waterstechnology-feature-not-a-bug-bloomberg-makes-the-case-for-the-figi/>), and is the only stance that lets ats-eqt distribute bulk parquet without CGS exposure. Document the policy explicitly in the data licence.

---

## Part C — Premium-tier vendors

### FactSet Ownership (formerly LionShares)
- **Product name:** FactSet Ownership / Ownership Standard DataFeed.
- **Coverage:** Global. US 13F (~5,200 filers/yr in 2017), plus ~80,000+ funds, plus non-US declarable-stake regimes (UK Companies Act, EU TR-Major-Holdings, Japan EDINET).
- **History depth:** Equity ownership back to 1999 (US 13F further); fixed-income from 2013-09 (source: <https://www.factset.com/marketplace/catalog/product/factset-ownership>).
- **Refresh cadence:** Daily updates as filings are processed; intraday for high-priority filers.
- **Schema highlights:** Three date fields per position — `report_date`, `filing_date`, `transfer_date` (when FactSet processed it) — explicitly bitemporal. Entity-centric symbology links sub-advisors and parent companies (source: <https://insight.factset.com/resources/at-a-glance-factset-ownership-standard-datafeed>).
- **Differentiator vs raw EDGAR:** Manager-entity rollups across CIKs/aliases; fund-level (non-13F) holdings; non-US holdings; "true decision maker" attribution (collapses subadvisors into the discretion-holding entity).
- **Delivery:** DataFeed (CSV/parquet bulk), Workstation, Snowflake share (source: <https://go.factset.com/hubfs/Website/Resources%20Section/Brochures/factset-data-solutions-via-snowflake-brochure.pdf>).
- **Pricing signal:** $30k–$200k+/yr depending on scope `[unverified — not publicly listed]`.
- **License:** Subscriber-only; redistribution prohibited.
- **Source URLs:** <https://www.factset.com/marketplace/catalog/product/factset-ownership>, <https://insight.factset.com/resources/at-a-glance-factset-ownership-standard-datafeed>, <https://github.com/lucieluyiliu/Factset_Holdings_SAS>.

### S&P Capital IQ Pro Ownership
- **Product name:** S&P Capital IQ Pro Ownership module (extends ciqOwnership tables).
- **Coverage:** 49,000+ public companies, 35,000+ institutions, 51,000+ funds, 337,000+ insiders, 12,000+ activism campaigns (source: <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro>).
- **History depth:** Late-1990s for US 13F; ownership tables documented back to 1996 in Capital IQ schema `[unverified — exact start date depends on filer]`.
- **Refresh cadence:** Daily.
- **Schema highlights:** Long-format fact tables `ciqInstitutionHolding`, joined to `ciqInstitution`, `ciqSecurity` (CIQ's internal security key, not CUSIP). Activism integration is differentiating.
- **Differentiator:** Activism campaigns linked to the holder; insider trading (Form 4) joined into the same ownership graph; fixed-income institutional holdings (announced 2024, expanding scope).
- **Delivery:** Capital IQ Pro web, Excel plug-in, Xpressfeed bulk, Snowflake share, S&P Marketplace.
- **Pricing signal:** $25k–$150k+/yr `[unverified]`.
- **License:** Subscriber-only.
- **Source URLs:** <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro>, <https://guides.newman.baruch.cuny.edu/c.php?g=188192&p=1243485>.

### Bloomberg HDS / PHDC / OWN
- **Product name:** Holdings via Terminal functions `HDS` (Holders Search), `PHDC` (Portfolio Holdings), `OWN`, `FLNG` (filings); Enterprise Data via the United States Ownership Filings dataset.
- **Coverage:** Global; uses Bloomberg's PORT and historical filings spine.
- **History depth:** Bloomberg Terminal coverage of 13F effectively as far back as EDGAR; bulk data feed from ~1999 `[unverified]`.
- **Refresh cadence:** Real-time during filing windows; intraday updates.
- **Schema highlights:** Keyed by FIGI (Bloomberg's home identifier). Manager identifier is the `BB Manager ID` (proprietary).
- **Differentiator:** FIGI-native (no CUSIP-licensing overhead at the consumer); tightly integrated with Bloomberg PORT scenario analysis; OPRA options-equivalent integration for `putCall` rows.
- **Delivery:** Terminal, BPipe, Bloomberg Enterprise Data feed, B-PORT.
- **Pricing signal:** Terminal $24k/yr/seat; Enterprise Data feeds $100k–$1M+ `[unverified for Ownership product specifically]`.
- **License:** Subscriber.
- **Source URLs:** <https://data.bloomberglp.com/professional/sites/10/Security-Ownership-fact-sheet.pdf>, <https://www.bloomberg.com/professional/dataset/united-states-ownership-filings/>, <https://www.bloomberg.com/professional/blog/webinar/gain-insights-from-institutional-and-13f-filings/>.

### Refinitiv (LSEG) eMAXX / Lipper Holdings
- **Product name:** LSEG Stock Ownership (formerly Refinitiv Ownership), eMAXX (fixed-income institutional), Lipper Mutual Fund Holdings.
- **Coverage:** $50–$71T in equity holdings across 70 markets; 26,000+ institutional investors; 70,000+ mutual/hedge funds (source: <https://www.lseg.com/en/data-analytics/financial-data/company-data/company-ownership-information-profiles>).
- **History depth:** Global from 1997; **US 13F from 1978**, US insider from 1986 (source: same; via WRDS at <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-stock-ownership/>). The 1978-current US history is the longest commercially distributed.
- **Refresh cadence:** Daily; eMAXX monthly for FI.
- **Schema highlights:** Long-format institutional holdings; Lipper joins below-13F-threshold fund holdings (especially APAC/EMEA where 13F doesn't reach); PermID-keyed.
- **Differentiator:** Longest history; non-US ownership-disclosure regimes harmonised; below-threshold mutual fund holdings via Lipper.
- **Delivery:** Eikon/Workspace, DataScope Select, LSEG Data Platform API, WRDS share.
- **Pricing signal:** $30k–$250k+/yr `[unverified]`.
- **License:** Subscriber.
- **Source URLs:** <https://www.lseg.com/en/data-analytics/financial-data/company-data/company-ownership-information-profiles>, <https://www.lseg.com/content/dam/data-analytics/en_us/documents/brochures/data-for-quant-research.pdf>, <https://developers.lseg.com/en/api-catalog/refinitiv-data-platform/ownership-API>.

---

## Part D — Mid-tier and retail vendors

### WhaleWisdom (the canonical mid-market benchmark)
- **Coverage:** All 13F filers, 13D/13G activists, Form 4 insiders.
- **History depth:** 2001-Q1 onward.
- **Refresh cadence:** Within hours of EDGAR dissemination.
- **Pricing tiers:** Standard $90/qtr or $300/yr (10 funds at a time); Pro $150/qtr or $500/yr (50 funds, combined-portfolio reports); Enterprise (custom, includes API + nightly FTP) (source: <https://whalewisdom.com/info/subscription_info>).
- **API:** Subscriber-only for full history; non-subscribers get last 8 quarters minus current; rate limit 20 req/min (source: <https://whalewisdom.com/help/api>).
- **Differentiator:** "WhaleScore" performance ranking, alerts, backtesting.
- **License:** Personal/internal use; redistribution prohibited.
- **Source URLs:** <https://whalewisdom.com/>, <https://whalewisdom.com/help/api>.

### 13F.info
- **Coverage:** All 13F filers, all securities. Free, no auth.
- **History depth:** Effectively 13F-XML era (2013+) `[unverified — earlier filings appear but parsing quality varies]`.
- **Differentiator:** Comparison across periods, manager holding history per security, list of all managers holding a given CUSIP per quarter. Open-source on GitHub `[unverified — repo not located]`.
- **Monetisation:** Light advertising; no paid tier observed.
- **License:** Not formally specified; implicit public-data status.
- **Source URLs:** <https://13f.info/>.

### Symmetric.io
- **Coverage:** Hedge-fund-focused 13F analytics; "consensus score" measuring crowding across managers.
- **Differentiator:** Fund-of-funds analysis, manager performance attribution, "Top 20" hedge fund rankings.
- **Pricing:** Tiered, contact-sales `[unverified — public site does not list prices]`.
- **S&P relationship:** Despite the project brief's hint, **no public record of an S&P acquisition was found.** Symmetric.io still operates independently as of 2026-05 `[unverified — possibly confused with another vendor]`. Treat the "now part of S&P" framing as unconfirmed.
- **Source URLs:** <https://www.symmetric.io/>, <https://www.symmetric.io/index/features>.

### Holdings Channel
- **Coverage:** Free 13F lookup by fund, ticker, date.
- **Differentiator:** Simplest UX in the segment; no auth, no paywall.
- **Monetisation:** Display advertising; affiliate links to BarChart/Nasdaq.
- **Source URLs:** <https://www.holdingschannel.com/>.

### Insider Monkey & HedgeFollow
- **Coverage:** Both bundle 13F + 13D/G + Form 4. HedgeFollow advertises tracking 10,000+ filers and updates insider data every ~5 minutes.
- **Monetisation:** Insider Monkey is editorial-content-driven (newsletter $150–$500/yr); HedgeFollow has free tier + paid Pro `[unverified]`.
- **Differentiator:** Editorial commentary on smart-money moves; HedgeFollow has portfolio tracking and screeners.
- **Source URLs:** <https://www.insidermonkey.com/>, <https://hedgefollow.com/>.

---

## Part E — API / developer-friendly vendors

### Fintel.io
- **Endpoints:** Security Ownership returns 13F + N-PORT holders for a security. 13F endpoints free; N-PORT premium-only.
- **Pricing:** Free tier; Premium $35/mo or $300/yr `[unverified for current pricing]`.
- **Schema:** REST/JSON; key fields `cik`, `name`, `cusip`, `value`, `shares`, `report_period`.
- **License:** API ToS prohibits redistribution.
- **Source URL:** <https://developers.fintel.io/reference/ownership-1>.

### Quiver Quantitative
- **Endpoints:** Congressional trades primary; 13F secondary.
- **Pricing:** Hobbyist $30/mo or $300/yr (Tier 1); Trader $75/mo or $750/yr (Tier 1+2); Commercial custom (source: <https://api.quiverquant.com/docs/>).
- **Schema:** REST/JSON; API-key auth.
- **Differentiator:** Alternative-data adjacencies (Congress, Reddit/WSB, lobbying).
- **Source URL:** <https://api.quiverquant.com/docs/>.

### sec-api.io
- **Endpoints:** Form 13F holdings dataset, full-text search, real-time stream.
- **Pricing:** $49–$999/mo tiers `[unverified for current pricing]`.
- **Differentiator:** Pure pass-through of EDGAR with normalised JSON; real-time WebSocket streaming.
- **Source URL:** <https://sec-api.io/datasets/form-13f-holdings>.

### Financial Modeling Prep, Finnhub, Financial Datasets, Fincoded
- All offer 13F endpoints in the $30–$200/mo range; differ mainly on history depth and rate limits.
- Financial Datasets: by-investor and by-security endpoints (source: <https://docs.financialdatasets.ai/api-reference/endpoint/institutional-ownership/investor>).
- Finnhub: institutional portfolio endpoint (source: <https://finnhub.io/docs/api/institutional-portfolio-13f>).
- Fincoded: 20 years of history claimed (source: <https://fincoded.com/datasets/institutional>).

### stockanalysis.com
- Free institutional ownership pages keyed by ticker; no documented public API as of 2026-05 `[unverified]`.

### Tegus (transcript-adjacent)
- Out of scope for 13F directly; relevant only as a downstream join for "smart-money" analytics products. Acquired by AlphaSense 2024.

---

## Part F — Bulk / academic sources

### WRDS Thomson Reuters Institutional Holdings (s34)
- **Coverage:** US 13F-filed institutional holdings, 1980-Q1 onward; quarterly.
- **Predecessor:** CDA Spectrum, then Thomson Financial, then Thomson Reuters, now Refinitiv/LSEG.
- **Known issues:** Researchers documented missing BlackRock holdings in pre-2018 vintages; even the 2018 fix had S&P 500 discrepancies (source: <https://wrds-www.wharton.upenn.edu/pages/support/research-wrds/research-guides/research-note-regarding-thomson-reuters-ownership-data-issues/>). The broader academic critique: Ben-David, Franzoni, Moussawi & Sedunov ("The Granular Nature of Large Institutional Investors") and Fich, Harford & Tran have all separately flagged manager-identifier instability in pre-2013 vintages.
- **Recommendation for ats-eqt:** Use s34 *only* as a sanity check for pre-2013 backfile. Do not treat as ground truth.

### WRDS Thomson Reuters Mutual Fund Holdings (s12)
- **Coverage:** US-registered mutual fund holdings, separate from 13F (which is manager-level).
- **Now superseded by N-PORT for post-2019 data.**

### Backfile cleanup vendors
- Several academic projects and small consultancies offer "cleaned 13F" backfiles, including the **Common Ownership Data** project by Michael Sinkinson (source: <https://sites.google.com/view/msinkinson/research/common-ownership-data>). Useful as cross-checks; not redistributable.

---

## Part G — Cross-cutting analysis

### G.1 The pre-2023 / post-2023 dollar-value discontinuity

The most consequential silent-ingestion bug: **`<value>` switched units on 2023-01-03**.

| Period | `<value>` units | Example: 100 shares of AAPL @ $150 |
|---|---|---|
| 2013-Q2 → 2022-Q4 | thousands of dollars | reported as `15` (i.e., $15,000) |
| 2023-Q1 → present | actual dollars | reported as `15000` |

Source: SEC Final Rule Release 34-95148, adopted 2022-06-23, effective 2023-01-03 (source: <https://www.sec.gov/files/rules/final/2022/34-95148.pdf>; summary at <https://www.toppanmerrill.com/blog/sec-updates-edgar-on-jan-3-2023-for-form-13f-changes/>).

**Ingestion code must branch on the `coverPage/reportCalendarOrQuarter` end date.** Filings with `periodOfReport ≤ 2022-12-31` apply a ×1000 multiplier; filings on or after 2023-01-01 do not. Amendments to pre-2023 periods filed after 2023-01-03 retain the old units (per SEC Q&A — they are restating the old period in the old format) `[unverified — confirm from FAQ updates]`.

### G.2 Confidential treatment workflow

Confidential treatment requests (`13F-CTR`) hide specified rows. Workflow (source: <https://www.sec.gov/rules-regulations/staff-guidance/division-investment-management-frequently-asked-questions/frequently-asked-questions-about-form-13f>):

1. Manager files `13F-HR` with confidential rows omitted, plus a parallel `13F-CTR` listing them.
2. SEC reviews; typical grant period is up to 1 year.
3. Within 6 business days of grant expiration *or* denial, manager must file `13F-HR/A` **adding back** the previously-confidential rows.

The famous case: Berkshire Hathaway's 2004 confidential-treatment denial (source: <https://www.sec.gov/rules-regulations/2004/08/berkshire-hathaway-inc-order-denying-requests-confidential-treatment>), and more recently routine grants for new-position accumulation (Burlington Northern, Apple, Chevron stakes were all initially confidential).

**ats-eqt implementation:** the bitemporal model handles this naturally. The `13F-HR/A` revealing the previously-confidential row will arrive with a later `knowledge_from` timestamp; queries asof the original filing date correctly show the row missing, queries asof a date after the amendment correctly include it.

### G.3 Manager-identifier instability

CIK is stable for an entity, but managers change identity in ways that break naive joins:

- **Acquisition / merger.** AcquiringCo's CIK absorbs TargetCo's positions; TargetCo's CIK stops filing.
- **Re-organisation.** Same legal entity, new CIK after restructuring.
- **Sub-advisor flip.** Same portfolio reported under different filers in successive quarters as the relationship moves.
- **Name-change without CIK-change.** Janus → Janus Henderson, MFS Investment Management aliases.

FactSet's "true decision maker" attribution and S&P's `ciqInstitution` table both encode an entity-resolution layer above CIK. ats-eqt should plan for an explicit `filer_13f_entity` table that survives CIK changes (Section H below).

### G.4 Amendments — restatement vs additional information

Form 13F-HR/A has two flavours, distinguished by an `amendmentType` field on the cover page:

- `RESTATEMENT` — the new filing **fully replaces** the prior `13F-HR`. Old rows are no longer authoritative.
- `NEW HOLDINGS` (additional information) — the new filing **adds rows** to those in the prior `13F-HR`.

**ats-eqt implementation:** on `13F-HR/A` ingest, parse the `amendmentType`. If RESTATEMENT, mark all rows of the original filing's `holding_13f` as superseded (`knowledge_to = now`); if NEW HOLDINGS, append. This is a common trap in DIY implementations.

### G.5 Short positions: not yet

Per the Dec 2025 exemption order, the first Form SHO filings under Rule 13f-2 are now due **2028-02-14** (source: <https://www.sec.gov/newsroom/press-releases/2025-37>; <https://www.morganlewis.com/pubs/2025/12/short-sale-reporting-on-form-sho-compliance-date-further-extended-to-2028>). Until then, 13F's long-only nature is a *fundamental* limitation — short interest at the manager level is not publicly observable. ats-eqt should plan a `holding_13f_short` table footprint but not stand it up before late 2027.

### G.6 N-PORT integration

Form N-PORT (registered investment companies) was amended in August 2024 to require **monthly** rather than quarterly public disclosure (source: <https://www.sec.gov/newsroom/speeches-statements/uyeda-statement-form-n-port-amendments-082824>). N-PORT covers the entire fund portfolio (cash, shorts, foreign, derivatives) at the *fund* level, while 13F is *manager*-level long-only.

The high-value join: an asset manager's **13F manager-level positions** reconciled against the **sum of their N-PORT fund-level positions**. Discrepancies expose:

- Separately-managed account (SMA) positions (in 13F, not in N-PORT).
- Non-13(f)-eligible positions (in N-PORT, not in 13F).
- Reporting timing offsets.

This is the strongest analytical differentiator vs raw EDGAR and the foundation of FactSet/S&P's value-add. ats-eqt's Phase 0 schema should anticipate the N-PORT join even before that data is ingested.

---

## Part H — Recommended ats-eqt schema

This fits the bitemporal long-format pattern in `schemas/data_models_and_methodology.md` (entity_id-keyed, bitemporal columns `valid_from/valid_to/knowledge_from/knowledge_to`, vendor IDs as time-bounded aliases).

### H.1 Filer entity (extension of `entity`)

```sql
-- The 13F-filing legal entity, distinct from issuers in `entity`
CREATE TABLE filer_13f (
  filer_id            BIGINT      PRIMARY KEY,        -- ats-eqt internal stable key
  entity_id           BIGINT      NOT NULL,           -- → entity (the underlying corporate entity)
  primary_cik         INTEGER     NOT NULL,           -- current CIK; may change under reorg
  filer_name          TEXT        NOT NULL,
  filer_type          TEXT        NOT NULL,           -- 'BANK', 'INSURANCE', 'IA', 'BD', 'PARENT', …
  parent_filer_id     BIGINT      NULL,               -- for sub-advisor / wholly-owned manager rollup
  first_seen          DATE        NOT NULL,           -- earliest 13F filing observed
  last_seen           DATE        NULL,               -- latest 13F filing; NULL = active
  knowledge_from      TIMESTAMP   NOT NULL,
  knowledge_to        TIMESTAMP   NOT NULL DEFAULT 'infinity'
);

-- Time-bounded CIK aliases for filers that change CIK
CREATE TABLE filer_13f_cik_alias (
  filer_id            BIGINT      NOT NULL,
  cik                 INTEGER     NOT NULL,
  valid_from          DATE        NOT NULL,
  valid_to            DATE        NOT NULL,
  PRIMARY KEY (filer_id, cik, valid_from)
);
```

### H.2 Filing metadata

```sql
CREATE TABLE filing_13f (
  filing_id           BIGINT      PRIMARY KEY,
  filer_id            BIGINT      NOT NULL,           -- → filer_13f
  accession_number    TEXT        NOT NULL UNIQUE,    -- EDGAR 18-digit accession (with dashes)
  form_type           TEXT        NOT NULL,           -- '13F-HR', '13F-HR/A', '13F-NT', '13F-CTR'
  amendment_type      TEXT        NULL,               -- 'RESTATEMENT' | 'NEW HOLDINGS' | NULL for non-amendment
  amends_filing_id    BIGINT      NULL,               -- → filing_13f for the amended filing
  period_of_report    DATE        NOT NULL,           -- quarter-end (YYYY-03-31, etc.)
  filing_date         DATE        NOT NULL,           -- when the filing was accepted by EDGAR
  receive_date        TIMESTAMP   NOT NULL,           -- exact dissemination timestamp
  table_value_total   NUMERIC(20,2) NULL,             -- normalized to dollars (post-2023 native, pre-2023 ×1000)
  table_entry_total   INTEGER     NULL,
  is_confidential     BOOLEAN     NOT NULL DEFAULT FALSE,  -- TRUE if 13F-CTR was granted
  units_multiplier    INTEGER     NOT NULL,           -- 1 (post-2023) or 1000 (pre-2023); precomputed for speed
  source_url          TEXT        NOT NULL
);

CREATE INDEX ix_filing_13f_filer_period ON filing_13f(filer_id, period_of_report);
```

### H.3 Holding fact table (the long-format core)

```sql
CREATE TABLE holding_13f (
  filing_id            BIGINT       NOT NULL,         -- → filing_13f
  filer_id             BIGINT       NOT NULL,         -- → filer_13f (denormalized for fast filer scans)
  period_of_report     DATE         NOT NULL,         -- denormalized from filing_13f
  security_id          BIGINT       NOT NULL,         -- → security (ats-eqt internal)
  -- The CUSIP is held in the security_alias table, NOT here, to control redistribution.
  shares_or_principal  BIGINT       NOT NULL,         -- sshPrnamt
  amount_type          CHAR(3)      NOT NULL,         -- 'SH' or 'PRN'
  put_call             VARCHAR(4)   NULL,             -- NULL | 'Put' | 'Call'
  market_value         NUMERIC(20,2) NOT NULL,        -- ALREADY NORMALIZED to dollars at ingest
  investment_discretion CHAR(4)     NOT NULL,         -- 'SOLE' | 'DFND' | 'OTR'
  voting_sole          BIGINT       NOT NULL DEFAULT 0,
  voting_shared        BIGINT       NOT NULL DEFAULT 0,
  voting_none          BIGINT       NOT NULL DEFAULT 0,
  other_managers       INT[]        NULL,             -- list of manager indices from cover page
  -- Bitemporal columns
  valid_from           DATE         NOT NULL,         -- = period_of_report
  valid_to             DATE         NOT NULL,         -- next period_of_report by same filer (or 9999-12-31)
  knowledge_from       TIMESTAMP    NOT NULL,         -- when ats-eqt ingested
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (filing_id, security_id, put_call, knowledge_from)
);

CREATE INDEX ix_holding_13f_filer_period ON holding_13f(filer_id, period_of_report);
CREATE INDEX ix_holding_13f_security_period ON holding_13f(security_id, period_of_report);
```

### H.4 Issuer-side linkage

The `security_id` joins to ats-eqt's existing `security` table. Since 13F uses CUSIP as input but ats-eqt's redistributable surface uses FIGI:

```sql
-- Existing security_alias (sketch):
-- security_id, alias_type ('CUSIP'|'FIGI'|'ISIN'|'TICKER'|'PERMID'), alias_value, valid_from, valid_to,
-- redistributable BOOLEAN
--
-- For 13F ingestion, the CUSIP from <infoTable> joins to security_alias WHERE alias_type='CUSIP'.
-- Public exports SELECT only WHERE redistributable=TRUE (excludes CUSIP).
```

### H.5 Materialized convenience views

```sql
-- Filer's latest filed positions (for "current portfolio" queries, the dominant retail use case)
CREATE MATERIALIZED VIEW current_holdings_13f AS
SELECT h.* FROM holding_13f h
JOIN (SELECT filer_id, MAX(period_of_report) AS max_period
      FROM filing_13f WHERE form_type IN ('13F-HR', '13F-HR/A')
      GROUP BY filer_id) m
  ON h.filer_id = m.filer_id AND h.period_of_report = m.max_period
WHERE h.knowledge_to = 'infinity';

-- Per-security ownership rollup
CREATE MATERIALIZED VIEW institutional_ownership AS
SELECT security_id, period_of_report,
       COUNT(DISTINCT filer_id) AS n_filers,
       SUM(CASE WHEN amount_type='SH' AND put_call IS NULL THEN shares_or_principal ELSE 0 END) AS shares_held,
       SUM(market_value) FILTER (WHERE put_call IS NULL) AS total_value
FROM holding_13f WHERE knowledge_to = 'infinity'
GROUP BY security_id, period_of_report;
```

### H.6 Ingestion pipeline outline

1. **Discover.** Poll `data.sec.gov/submissions/CIK*.json` and the daily-index files for `form_type IN ('13F-HR','13F-HR/A','13F-NT','13F-CTR')`.
2. **Fetch.** GET the primary XML at `Archives/edgar/data/{cik}/{accession}/...`.
3. **Parse.** XSD-validate against the v1.6 spec; extract cover page + information table.
4. **Resolve.**
   - Filer: `cik → filer_id` via `filer_13f_cik_alias`. Create new `filer_13f` row if no match; link to `entity` via name match + reconciliation queue.
   - Security: `cusip → security_id` via `security_alias`. Fall back to OpenFIGI mapping if no internal match; queue for manual review if both fail.
5. **Normalize value.** Multiply by 1000 if `period_of_report ≤ 2022-12-31`.
6. **Insert.** Bitemporal upsert. For `13F-HR/A` with `amendmentType='RESTATEMENT'`, set `knowledge_to = now()` on prior filing's holdings before inserting new rows.
7. **Reconcile.** Cross-check `summaryPage` totals against sum of `value` column; flag discrepancies > 1%.
8. **Publish.** Refresh materialized views; emit Kafka events for downstream subscribers.

---

## Part I — Strategic positioning for ats-eqt

### I.1 Where the open-data moat is thin

13F is *the* most competed-on dataset in financial open data. Substrate is free; ingestion is < 1 person-week; the SEC does the schema design. Competition is purely on:

- **Entity resolution** quality (manager hierarchy, sub-advisor rollup, M&A continuity).
- **Cross-form linkage** (13F ↔ N-PORT ↔ Form 4 ↔ 13D/G ↔ Form ADV).
- **History depth and cleanliness** (pre-2013 backfile; CTR-revealed positions integrated).
- **UX** (portfolio comparison, alerting, screening — WhaleWisdom's strength).
- **Latency** (minutes after EDGAR vs hours).

### I.2 First-customer profile

Three plausible ats-eqt 13F customer cohorts, in order of fit:

1. **Quant-academic researchers.** Currently using WRDS s34; pain is the known data-quality issues and the CUSIP-licensing constraint when publishing replication packages. ats-eqt FIGI-keyed bulk parquet, redistributable, fits perfectly. Price point: institutional-site licence in the $5k–$25k/yr range.
2. **Smart-money-tracking retail platforms.** API consumers building "follow-the-fund" newsletters and Discord bots. Currently using Fintel/Quiver/sec-api.io. Price point: $50–$300/mo. WhaleWisdom is the volume benchmark.
3. **Hedge funds doing crowding/factor analysis.** Want manager-entity rollup and 13F+N-PORT join. Currently buying FactSet Ownership at $50k–$200k. Realistic ats-eqt capture: 10–20% discount, undifferentiated on coverage but cheaper and developer-friendlier.

### I.3 12-month build to feature parity with WhaleWisdom

| Month | Milestone |
|---|---|
| M0 | EDGAR poller; XML parser; raw-table loader. ~5,300 filers/qtr ingesting. |
| M1 | CUSIP↔FIGI↔internal security resolution; bitemporal holding_13f populated. |
| M2 | Backfill 2013-Q2 → present from EDGAR archives. Pre-2023 unit-cutover handled. |
| M3 | Filer entity-resolution layer (CIK aliasing, parent/subadvisor rollup) — the hard part. |
| M4 | Public read API (REST + GraphQL); FIGI-keyed; first beta customers. |
| M5 | Alerts (filing arrived, position breached threshold, fund returned to security). |
| M6 | Portfolio comparison UI; consensus/crowding score. |
| M7 | N-PORT ingestion and cross-link to 13F. |
| M8 | Pre-2013 backfile from cleaned WRDS s34 (quality-flagged tier). |
| M9 | 13D/G + Form 4 cross-link. |
| M10 | Activism-campaign linkage (compete with S&P CIQ Pro on this axis). |
| M11 | Form ADV linkage (manager AUM + strategy). |
| M12 | Bulk parquet / Snowflake share for academic and hedge-fund customers. |

The M3 entity-resolution layer is the moat. Everything else is mechanical.

---

## Part J — Sources

### SEC primary
- <https://www.sec.gov/rules-regulations/staff-guidance/division-investment-management-frequently-asked-questions/frequently-asked-questions-about-form-13f> — Form 13F FAQs
- <https://www.sec.gov/edgar/filer-information/specifications/form13fxmltechspec-draft> — XML Technical Specification draft v1.6
- <https://irdirect.net/scripts/xslt/13F/EDGAR%20Form%2013%20F%20XML%20Technical%20Specification.pdf> — XML Technical Specification v1.2
- <https://www.sec.gov/files/rules/final/2022/34-95148.pdf> — Final rule release adopting 2022 amendments
- <https://www.sec.gov/rules-regulations/staff-guidance/official-list-section-13f-securities> — Official List of Section 13(f) Securities
- <https://www.sec.gov/files/investment/13flist2026q1.pdf> — FY2026Q1 Section 13(f) list
- <https://www.sec.gov/rules-regulations/2004/08/berkshire-hathaway-inc-order-denying-requests-confidential-treatment> — Berkshire Hathaway CTR denial
- <https://www.sec.gov/investment/divisionsinvestmentguidance13fpt2htm> — Section 13(f) Confidential Treatment Requests guidance
- <https://www.sec.gov/search-filings/edgar-application-programming-interfaces> — EDGAR APIs
- <https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data> — Accessing EDGAR data
- <https://www.sec.gov/data-research/sec-markets-data/form-n-port-data-sets> — Form N-PORT data sets
- <https://www.sec.gov/files/formn-port.pdf> — Form N-PORT specification
- <https://www.sec.gov/newsroom/speeches-statements/uyeda-statement-form-n-port-amendments-082824> — N-PORT August 2024 amendments
- <https://www.sec.gov/newsroom/press-releases/2025-37> — Form SHO Rule 13f-2 December 2025 exemption
- <https://www.sec.gov/files/rules/exorders/2025/34-104303.pdf> — Form SHO compliance extension order
- <https://www.investor.gov/introduction-investing/investing-basics/glossary/form-13f-reports-filed-institutional-investment> — Investor.gov 13F glossary

### Regulatory analysis
- <https://corpgov.law.harvard.edu/2022/08/06/amendments-to-form-13f/> — Harvard Corp Gov Forum on 2022 amendments
- <https://www.toppanmerrill.com/blog/sec-updates-edgar-on-jan-3-2023-for-form-13f-changes/> — Jan 3 2023 cutover details
- <https://www.sidley.com/en/insights/newsupdates/2022/07/sec-adopts-rules-requiring-electronic-filing-for-form-13f-confidential-treatment-requests> — Sidley on 13F-CTR electronic filing
- <https://www.morganlewis.com/pubs/2025/12/short-sale-reporting-on-form-sho-compliance-date-further-extended-to-2028> — Form SHO 2028 extension
- <https://www.federalregister.gov/documents/2022/06/30/2022-13936/electronic-submission-of-applications-for-orders-under-the-advisers-act-and-the-investment-company> — Federal Register 2022 amendments
- <https://www.acaglobal.com/industry-insights/sec-rule-13f-2-are-you-ready-january-2-2025-filing-deadline/> — Rule 13f-2 readiness
- <https://natlawreview.com/article/changes-coming-2023-form-13f-content-and-confidential-treatment-requests> — 2023 changes summary

### CUSIP licensing
- <https://www.waterstechnology.com/regulation/7936086/class-action-lawsuit-takes-aim-at-cusip-sp-factset-aba> — Class action coverage
- <https://www.napa-net.org/news-info/daily-news/class-action-suit-challenges-big-cusip-licensing-fees> — CUSIP licensing class action
- <https://reason.org/commentary/class-action-lawsuits-against-cusip-could-improve-government-transparency/> — Reason Foundation analysis
- <https://finopsinfo.com/operations/whats-a-cusip-worth-over-us1b-in-class-action-win/> — FinOps coverage
- <https://www.waterstechnology.com/data-management/7951850/as-legal-letters-fly-cusip-licensing-debate-rolls-on> — Licensing letter campaign

### FIGI / OpenFIGI
- <https://www.openfigi.com/> — OpenFIGI portal
- <https://www.openfigi.com/about/regulations> — FIGI regulatory recognition (incl. SEC 13F)
- <https://www.bloomberg.com/company/press/waterstechnology-feature-not-a-bug-bloomberg-makes-the-case-for-the-figi/> — Bloomberg on FIGI vs CUSIP
- <https://en.wikipedia.org/wiki/Financial_Instrument_Global_Identifier> — FIGI overview

### Premium vendors
- <https://www.factset.com/marketplace/catalog/product/factset-ownership> — FactSet Ownership product page
- <https://insight.factset.com/resources/at-a-glance-factset-ownership-standard-datafeed> — FactSet Ownership DataFeed brief
- <https://www.wiso.uni-hamburg.de/bibliothek/recherche/datenbanken/unternehmensdaten/factset-ownership.pdf> — FactSet Ownership documentation
- <https://github.com/lucieluyiliu/Factset_Holdings_SAS> — Academic FactSet Holdings SAS code
- <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro> — S&P Capital IQ Pro
- <https://data.bloomberglp.com/professional/sites/10/Security-Ownership-fact-sheet.pdf> — Bloomberg Security Ownership fact sheet
- <https://www.bloomberg.com/professional/dataset/united-states-ownership-filings/> — Bloomberg US Ownership Filings dataset
- <https://www.bloomberg.com/professional/blog/webinar/gain-insights-from-institutional-and-13f-filings/> — Bloomberg 13F webinar
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/company-ownership-information-profiles> — LSEG Ownership
- <https://www.lseg.com/content/dam/data-analytics/en_us/documents/brochures/data-for-quant-research.pdf> — LSEG quant data brochure
- <https://developers.lseg.com/en/api-catalog/refinitiv-data-platform/ownership-API> — LSEG Ownership API

### Mid-tier and API vendors
- <https://whalewisdom.com/info/subscription_info> — WhaleWisdom pricing
- <https://whalewisdom.com/help/api> — WhaleWisdom API docs
- <https://whalewisdom.com/info/faq> — WhaleWisdom FAQ
- <https://www.dakota.com/resources/blog/whalewisdom-opportunity-hunter-sec-api-which-is-right-for-you> — Comparison
- <https://13f.info/> — 13F.info
- <https://www.symmetric.io/> — Symmetric.io
- <https://hedgefollow.com/> — HedgeFollow
- <https://www.insidermonkey.com/> — Insider Monkey
- <https://www.holdingschannel.com/> — Holdings Channel
- <https://developers.fintel.io/> — Fintel API
- <https://developers.fintel.io/reference/ownership-1> — Fintel Security Ownership endpoint
- <https://api.quiverquant.com/docs/> — Quiver Quantitative API
- <https://sec-api.io/datasets/form-13f-holdings> — sec-api.io 13F dataset
- <https://docs.financialdatasets.ai/api-reference/endpoint/institutional-ownership/investor> — Financial Datasets API
- <https://finnhub.io/docs/api/institutional-portfolio-13f> — Finnhub 13F endpoint
- <https://fincoded.com/datasets/institutional> — Fincoded institutional dataset

### Academic / WRDS
- <https://wrds-www.wharton.upenn.edu/pages/grid-items/thomson-reuters-stock-ownership/> — WRDS LSEG Stock Ownership
- <https://wrds-www.wharton.upenn.edu/pages/support/research-wrds/research-guides/research-note-regarding-thomson-reuters-ownership-data-issues/> — Thomson Reuters s34 issues note
- <https://wrds-www.wharton.upenn.edu/documents/1414/WRDS_Ownership_Data.pdf> — WRDS Ownership data overview
- <https://sites.google.com/view/msinkinson/research/common-ownership-data> — Sinkinson common-ownership data project
- <https://elsaifym.github.io/EDGAR-Parsing/> — Open EDGAR 13F parsing reference
- <https://gist.github.com/mgao6767/41877b83763846008680acb3d355e9ff> — Institutional ownership ratios calculation gist

---

**Confirm:**

- File path: `c:/Users/natha/OneDrive/Desktop/C/ats/ats-eqt/research/datasets/13f_holdings.md`
- Section count: **11 top-level parts** (0 Executive summary; A Primary source; B CUSIP licensing; C Premium vendors; D Mid-tier vendors; E API vendors; F Bulk/academic; G Cross-cutting analysis; H Recommended schema; I Strategic positioning; J Sources), with 30+ sub-sections.
