# ats-eqt — Insider & Non-13F Ownership Datasets

**Status:** Research, v0.1 (wave 2 of the ownership stack)
**Audience:** ats-eqt engineering team (ingestion, storage, query); ats-core team designing event-driven primitives over EDGAR
**Scope:** Section 16 insider filings (Forms 3 / 4 / 5), Schedule 13D / 13G blockholder filings, Form N-PORT fund-level holdings, Form 144 restricted-stock sales, Form N-PX proxy-voting records, tender-offer adjacencies (Schedule TO / 14D-9), STOCK Act congressional disclosures, plus the vendor stack (FactSet, S&P CIQ, Bloomberg, LSEG/Refinitiv, WhaleWisdom, Quiver, OpenInsider) that consolidates the above.
**Last updated:** 2026-05-14
**Companion file:** `13f_holdings.md` (manager-level 5%+ institutional holdings; see Part A.4 of that file for the parallel `informationTable` XSD).

---

## 0. Executive summary

13F (manager-level) is one face of the public-ownership graph. The *adjacent* faces — insiders, 5%+ blockholders, fund-level holdings, restricted-stock sellers, proxy-vote records, and congressional traders — together make up the rest of the legally compelled disclosure surface for US equity ownership. This file documents that surface at field/schema level so ats-eqt can stand up a unified `insider × blockholder × fund-holding` model that joins cleanly to the 13F manager-level model.

Five headline findings drive the recommended approach:

1. **Form 4 XML is the cleanest open-data substrate in the SEC corpus.** Mandatory 2-business-day filing since SOX 2003, fully structured XML, ~250k filings per year. Schema is `ownershipDocument` with `reportingOwner`, `nonDerivativeTable`, `derivativeTable`, and `footnotes`. The 28-letter `transactionCode` enumeration is the load-bearing field for any signal model (source: <https://www.sec.gov/edgar/searchedgar/ownershipformcodes.html>).
2. **Schedule 13D/G is finally machine-readable as of 2024-12-18.** Before that date Schedules were filed as HTML/ASCII free-form; the October 2023 modernization rule (effective 2024-02-05) and the structured-data mandate that followed (compliance 2024-12-18) make 13D/G filings parseable XML for the first time. Backfill before 2024-12-18 still requires HTML/ASCII parsing (source: <https://www.sec.gov/newsroom/press-releases/2023-219>; <https://www.olshanlaw.com/newsroom/alerts/client-alert-important-reminder-schedules-13d-and-13g-must-be-filed-using-structured-machine-readable-xml-based-language-beginning-december-18-2024>).
3. **The 13D filing deadline collapsed from 10 calendar days to 5 business days** on 2024-02-05; 13D amendments now due within 2 business days (down from "promptly"); 13G timelines tightened (compliance 2024-09-30). This compresses the "stealth accumulation window" used by activists and is directly observable in the data.
4. **The August 2024 Form N-PORT monthly-public-disclosure rule was delayed to 2027–2028** by the April 2025 extension order. Until 2027-11-17 (≥$1B fund groups) / 2028-05-18 (smaller funds), N-PORT remains quarterly-public with a 60-day lag; the SEC published a *new* proposal in February 2026 to scale back parts of the 2024 amendments. This is a moving target — wave-3 should re-check (source: <https://www.sec.gov/newsroom/press-releases/2025-64>; <https://www.federalregister.gov/documents/2025/04/22/2025-06861>).
5. **10b5-1 plan adoption is now disclosed on the Form 4 cover.** Since 2023-04-01, Forms 4 and 5 include a `rule10b5-1Indicator` checkbox plus the plan-adoption date — finally distinguishing pre-arranged trades from discretionary insider sales without footnote heuristics. The 90/120-day cooling-off rule (officers/directors) and 30-day rule (other §16 persons) define the legal earliest-trade window and let ats-eqt audit plan compliance directly (source: <https://www.sec.gov/newsroom/press-releases/2022-222>).

Phase-0 build plan: ingest Form 4 first (highest signal density, cleanest schema, oldest mandatory-XML history), then 13D/G XML from 2024-12-18 forward (with separate HTML-parser pipeline for backfile), then N-PORT (quarterly-public until 2027), then Form 144, then N-PX. Stand up STOCK Act congressional data as a labelled-experiment dataset.

---

## Part A — Vendor stack matrix

Comparative coverage across the consolidating vendors. "Y/N" = product offered; "—" = not part of product line; bracketed cells `[unverified]` flag claims not cross-confirmed.

| Vendor                | Form 3/4/5 insider | 13D/G  | N-PORT (fund holdings) | Form 144 | Form NPX | Beneficial-owner roll-up | Insider scoring | Pricing tier (annual)                |
|-----------------------|--------------------|--------|------------------------|----------|----------|--------------------------|-----------------|--------------------------------------|
| FactSet Ownership     | Y (US back to 1986)| Y      | Y (fund-level)         | Y `[unverified]` | Y `[unverified]` | Y (issuer-level + decision-maker) | Y (proprietary "transaction quality") | $30k–$200k+ `[unverified]`          |
| S&P CIQ Pro Ownership | Y (337k+ insiders) | Y (12k+ activism campaigns) | Y (51k+ funds) | Y `[unverified]` | Y `[unverified]` | Y (`ciqOwnership` + `ciqInsider`) | Y                | $25k–$150k+ `[unverified]`           |
| Bloomberg OWN/HDS/PHDC| Y                  | Y      | Y                       | Y        | Y        | Y (FIGI-keyed)             | Limited         | Terminal $32k/seat; Enterprise custom |
| Refinitiv/LSEG (Eikon + Workspace) | Y (1986–) | Y | Y (Lipper fund-level) | Y | Y `[unverified]` | Y (PermID-keyed)         | Limited         | $30k–$250k+ `[unverified]`           |
| WhaleWisdom Premium   | Y (Form 4 + 13D/G ~2006–) | Y (15+ yrs) | —                      | —        | —        | Limited (filer-level)     | "WhaleScore"    | $300–$500/yr Std/Pro; Ent. custom    |
| Quiver Quantitative   | Y                  | —      | —                      | —        | —        | —                          | Limited         | $30/$75/mo retail; Commercial custom |
| OpenInsider           | Y (Form 4 only, free) | —    | —                      | —        | —        | —                          | "Cluster Buys"  | Free (display ads)                   |
| InsiderInsights / 2iQ | Y                  | —      | —                      | —        | —        | —                          | Y (proprietary) | $50–$500/mo retail                   |
| Verafin               | Y (Forms 3/4/5 used as AML signal) | — | — | — | — | — | AML/CTR-focused | Bank enterprise, $50k+ `[unverified]` |
| Senate Stock Watcher / capitoltrades.com | — | — | — | — | — | — | Congressional only | Free / freemium |

Sources: <https://www.factset.com/marketplace/catalog/product/factset-ownership>; <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro>; <https://whalewisdom.com/info/subscription_info>; <https://api.quiverquant.com/docs/>; <http://openinsider.com/>; <https://www.lseg.com/en/data-analytics/financial-data/company-data/company-ownership-information-profiles>; <https://www.bloomberg.com/professional/dataset/united-states-ownership-filings/>; <https://senatestockwatcher.com/>.

---

## Part B — SEC Form 3 / 4 / 5 (Section 16 insider transactions)

### B.1 Filing trigger and population

- Imposed by Section 16(a) of the Securities Exchange Act of 1934. Filing persons ("§16 reporting persons"):
  - Every **officer** of an issuer of a registered class of equity (Section 16 "officer" is a term of art — CEO, CFO, principal accounting officer, plus any VP-in-charge-of-a-principal-business-unit or any other "policy-making" officer; not every legal-title VP).
  - Every **director** of such an issuer.
  - Every **direct or indirect beneficial owner of more than 10%** of any registered class of equity.
- **Form 3** — Initial Statement of Beneficial Ownership. Due within **10 calendar days** of becoming a §16 person (or by the issuer's IPO effective date for officers/directors at IPO).
- **Form 4** — Statement of Changes in Beneficial Ownership. Due within **2 business days** of the triggering transaction (this is the SOX-era deadline; pre-2003 was the 10th of the month following the transaction).
- **Form 5** — Annual Statement of Beneficial Ownership. Due **45 calendar days** after the issuer's fiscal year end. Captures transactions that were exempt from Form 4 filing during the year (small acquisitions, certain gifts, etc.) plus any Form 4 transactions that should have been but were not reported.
- Approximately 250,000 Form 4 filings per calendar year across ~50,000 unique reporting persons `[unverified — order of magnitude from EDGAR full-index counts]`.

### B.2 The Ownership XML technical specification

The authoritative schema for Forms 3/4/5 is the **EDGAR Ownership XML Technical Specification**, currently published as Version 5.1 (with a Version 5.4 draft) (source: <https://www.sec.gov/info/edgar/ownershipxmltechspec.htm>; <https://www.sec.gov/page/edgar-ownership-xml-tech-spec>). The root element is `<ownershipDocument>` with the following structural children:

```
ownershipDocument
├── schemaVersion             (e.g. X0508 for v5.8 of the form)
├── documentType              ('3' | '3/A' | '4' | '4/A' | '5' | '5/A')
├── periodOfReport            (YYYY-MM-DD; the transaction date for Form 4)
├── notSubjectToSection16     (boolean override)
├── issuer
│   ├── issuerCik             (10-digit CIK, zero-padded)
│   ├── issuerName
│   └── issuerTradingSymbol
├── reportingOwner            (repeating, 1..N)
│   ├── reportingOwnerId
│   │   ├── rptOwnerCik
│   │   ├── rptOwnerCcc       (not in public dissemination)
│   │   └── rptOwnerName
│   ├── reportingOwnerAddress (street1/2, city, state, zipCode, stateDescription)
│   └── reportingOwnerRelationship
│       ├── isDirector        (boolean: 1 = director)
│       ├── isOfficer         (boolean: 1 = officer)
│       ├── isTenPercentOwner (boolean: 1 = 10%+ beneficial owner)
│       ├── isOther           (boolean: 1 = "other" reason — e.g. designated by court)
│       └── officerTitle      (free text — see B.6 normalization problem)
│       └── otherText         (free text when isOther = 1)
├── aff10b5One                (Form 3 only — affirmation of pre-existing 10b5-1 plan)
├── nonDerivativeTable        (Table I; repeating nonDerivativeTransaction + nonDerivativeHolding rows)
├── derivativeTable           (Table II; repeating derivativeTransaction + derivativeHolding rows)
├── footnotes                 (repeating <footnote id="F1">...</footnote>)
├── remarks                   (free text remarks)
├── ownerSignature            (repeating signatureName + signatureDate)
```

### B.3 Non-derivative transaction row

Element: `<nonDerivativeTransaction>` (under `<nonDerivativeTable>`). Fields:

| Element                              | Type / cardinality       | Semantics                                                                                                  |
|---|---|---|
| `securityTitle/value`                | string                   | Title of the equity (e.g. "Common Stock", "Class A Common", "ADS").                                        |
| `transactionDate/value`              | date (YYYY-MM-DD)        | Calendar date of the transaction.                                                                          |
| `deemedExecutionDate/value`          | date, optional           | Used for §16 timing fictions (e.g. deemed exercise on a record date).                                       |
| `transactionCoding/transactionFormType` | enum                   | '4' or '5' indicating which form the transaction is reported on.                                            |
| `transactionCoding/transactionCode`  | enum (single letter)     | The 28-letter enumeration; see Part B.5.                                                                   |
| `transactionCoding/equitySwapInvolved` | boolean                | 1 if the transaction is an equity-swap leg (K-coded).                                                       |
| `transactionTimeliness/value`        | enum 'E' (early) or null | Reports a `V` (voluntary early reporting) timing flag.                                                     |
| `transactionAmounts/transactionShares/value` | int64            | Number of shares in the transaction.                                                                       |
| `transactionAmounts/transactionPricePerShare/value` | decimal   | Reported price per share. May be 0 for grants/gifts; may have an exemptionFlag for fractional adjustments. |
| `transactionAmounts/transactionAcquiredDisposedCode/value` | enum 'A' or 'D' | A = acquired, D = disposed. Required.                                                              |
| `postTransactionAmounts/sharesOwnedFollowingTransaction/value` | int64 | Running total of beneficially owned shares after this transaction.                                         |
| `ownershipNature/directOrIndirectOwnership/value` | enum 'D' or 'I' | D = direct (held in own name), I = indirect (through trust, family member, LLC, etc.).                     |
| `ownershipNature/natureOfOwnership/value` | string, optional, indirect-only | Free text describing the indirect arrangement (e.g. "By Family Trust", "By 401(k)").              |
| `*/footnoteId/@id`                   | reference, repeating     | Each leaf field can carry one or more `<footnoteId id="F1"/>` references resolving to a `<footnote>`.       |

`<nonDerivativeHolding>` uses the same skeleton but without the transaction-specific elements — it captures a current beneficial-ownership position not tied to a transaction (used on Form 3 and to disclose new indirect-holding categories).

### B.4 Derivative transaction row

Element: `<derivativeTransaction>` (under `<derivativeTable>`). Adds, on top of the non-derivative skeleton:

| Element                                          | Semantics                                                                                  |
|---|---|
| `conversionOrExercisePrice/value`                | Strike / conversion price of the derivative.                                                |
| `exerciseDate/value`                             | First date the derivative is exercisable.                                                   |
| `expirationDate/value`                           | Final expiration date.                                                                      |
| `underlyingSecurity/underlyingSecurityTitle/value` | Title of the underlying equity (must match the issuer's class).                            |
| `underlyingSecurity/underlyingSecurityShares/value` | Number of underlying shares represented by the derivative.                                 |
| `transactionAmounts/transactionShares/value`     | Number of derivatives (typically option contracts representing 100 shares — but reported as the underlying-share count). |

### B.5 The Form 4 / Form 5 transactionCode enumeration

The single most important enumeration in the entire insider corpus. Source: SEC Ownership Form Codes page (<https://www.sec.gov/edgar/searchedgar/ownershipformcodes.html>), the form instructions (Tables I and II), and the EDGAR Ownership XML Technical Specification §3.6.9 `TransCodeList`.

```
General Transaction Codes
  P  Open-market or private purchase of non-derivative or derivative security
  S  Open-market or private sale of non-derivative or derivative security
  V  Transaction voluntarily reported earlier than required

Rule 16b-3 Transaction Codes (issuer-grant family)
  A  Grant, award, or other acquisition pursuant to Rule 16b-3(d)
  D  Disposition to the issuer of issuer equity securities pursuant to Rule 16b-3(e)
  F  Payment of exercise price or tax liability by delivering or withholding securities
     incident to the receipt, exercise or vesting of a security issued under Rule 16b-3
  I  Discretionary transaction in accordance with Rule 16b-3(f) resulting in
     acquisition or disposition of issuer securities
  M  Exercise or conversion of derivative security exempted pursuant to Rule 16b-3

Derivative Securities Codes
  C  Conversion of derivative security
  E  Expiration of short derivative position
  H  Expiration (or cancellation) of long derivative position with value received
  O  Exercise of an out-of-the-money derivative security
  X  Exercise of an in-the-money or at-the-money derivative security

Other §16(b) Exempt Transaction and Small-Acquisition Codes
  G  Bona fide gift
  L  Small acquisition under Rule 16a-6
  W  Acquisition or disposition by will or laws of descent and distribution
  Z  Deposit into, or withdrawal from, voting trust

Other Transaction Codes
  J  Other acquisition or disposition (filer must describe nature in remarks/footnote)
  K  Transaction in equity swap or similar instrument
  U  Disposition pursuant to a tender of shares in a change-of-control transaction
```

Twenty-eight codes total when both Table I and Table II usages are merged. Operational notes:

- `P` is the highest-signal code by a wide margin — pure open-market buys with the insider's own capital. OpenInsider's "Latest Insider Purchases $25k" stream and most academic literature filter on `P` exclusively (source: <http://openinsider.com/latest-insider-purchases-25k>).
- `S` is *not* a clean sale signal because `S` is dominated by 10b5-1 plan trades — pre-arranged sales the insider committed to months earlier. Use the `rule10b5-1Indicator` (Part B.8) to split discretionary `S` from plan `S`.
- `F` and `M` together describe the cashless-exercise pattern: option exercised (`M`), shares withheld for tax (`F`), residual shares to insider. These should be netted to reveal the *new* economic exposure, not counted as a "sale".
- `K` (equity swap) is the load-bearing code for hedging via collars / variable-prepaid forwards / equity total-return swaps. Insiders rarely use it because of disclosure stigma; when they do, treat it as a strong "I've reduced my downside" signal.
- `J` ("Other") is a catch-all whose interpretation requires reading the `<footnotes>` text. Pre-build a footnote NER pipeline for `J` rows.
- `U` (tender) clusters around take-private events — useful for M&A event-study labelling.
- `V` is *not* a transaction type; it is a timeliness flag. ats-eqt should store it as a separate boolean (`reported_early`) and never count `V` as its own transaction.

### B.6 The "officer title" problem

`reportingOwnerRelationship/officerTitle` is **free text**. Real-world examples in 2024 filings: `"Chief Executive Officer"`, `"CEO"`, `"CEO and President"`, `"Pres. & CEO"`, `"Chief Executive Officer (Principal Executive Officer)"`, `"CEO, President & Chairman"`, `"see Remarks"`. The same person can appear in multiple filings with different free-text spellings.

Vendor normalization (FactSet, S&P CIQ, Bloomberg) maps these to a controlled vocabulary — typically `{CEO, CFO, COO, CIO, CTO, GC, Director, Chair, Other-Officer, Other}` plus a binary `is_named_executive_officer` flag joined from the issuer's proxy statement. ats-eqt should plan for the same: a `role_norm` column populated by an LLM classifier + rule layer over `officerTitle`.

### B.7 Footnotes — the most under-parsed field

`<footnotes>` is a repeating block of `<footnote id="F1">free text</footnote>`. *Any* leaf element in the transaction row can reference one or more footnotes via `<footnoteId id="F1"/>`. The footnote text is the only place where:

- 10b5-1 plan details (plan adoption date if not on the cover, plan modification history, plan termination)
- Family-relationship details on indirect ownership ("By spouse", "By 401(k) for benefit of minor children")
- Estate / trust details ("By Smith Family Trust dated 2014-03-12, of which the Reporting Person is grantor and trustee")
- Vesting / RSU schedules ("This grant vests in four equal annual installments beginning 2025-01-15")
- Fractional / rounding adjustments
- Donation receipts and gift-recipient identities (for `G` rows)

…appear. ats-eqt must store footnotes both as raw text and as parsed entities. For the 10b5-1 case specifically the post-2022 `rule10b5-1Indicator` checkbox covers the adoption-flag, but the modification/termination history still lives only in footnotes.

### B.8 The 10b5-1 plan disclosure layer (post-2022 amendments)

The SEC's December 14, 2022 final rule (Release Nos. 33-11138, 34-96492) modernized Rule 10b5-1 with effect on §16 filings from 2023-04-01 (source: <https://www.sec.gov/newsroom/press-releases/2022-222>; <https://www.sec.gov/files/33-11138-fact-sheet.pdf>).

Material changes:

1. **Form 4/5 cover-page checkbox.** A new checkbox indicates whether the reported transaction was executed under a Rule 10b5-1 plan intended to satisfy the affirmative defense. In XML the field is `transactionCoding/rule10b5-1Indicator` (boolean) and a paired `transactionCoding/plan10b5_1AdoptionDate/value` (date) `[unverified — exact XML element names from v5.4 draft; confirm against ownership-form-codes.html]`.
2. **Cooling-off period.** For officers and directors, no trades may occur until the later of (a) 90 days after plan adoption/modification or (b) two business days after the Form 10-Q or 10-K for the fiscal quarter of adoption/modification is filed — capped at 120 days. For other §16 persons, the period is 30 days. ats-eqt should compute a derived `cooling_off_days_compliant` boolean to flag suspect plans.
3. **One-plan-at-a-time restriction.** Reporting persons may not have multiple overlapping 10b5-1 plans for open-market trades (with narrow exceptions for sell-to-cover and certain conditional plans).
4. **Annual quantity cap.** Reporting persons may use the affirmative defense for only one single-trade plan in any 12-month period.
5. **Good-faith certification.** Plan adopters must certify they are not aware of MNPI and are acting in good faith.
6. **Issuer-side annual disclosure.** Item 408 of Regulation S-K requires the issuer to disclose any 10b5-1 plan adoptions/terminations by §16 persons in the immediately preceding fiscal quarter, in 10-Q and 10-K. This gives an alternative cross-reference (issuer-side) for plan adoption events.

For ats-eqt the operational implication is a dedicated `tradingplan_10b5_1` table (Part I), with bi-directional links: insider-side from the Form 4 checkbox and issuer-side from Item 408 disclosures parsed out of 10-Q/10-K text.

### B.9 Acquisition channels

Three parallel access methods to Form 4 XML:

1. **EDGAR full-text search** (`https://efts.sec.gov/LATEST/search-index?q=&forms=4&dateRange=custom&startdt=...&enddt=...`) — JSON results; rate-limited ~10 req/s; declared `User-Agent` required.
2. **EDGAR daily index** (`https://www.sec.gov/Archives/edgar/full-index/{YYYY}/QTR{n}/form.idx` and `master.idx`) — line-oriented `form-type|company|cik|date|filename`. Used for backfill.
3. **EDGAR Public Dissemination Service (PDS)** — paid real-time push (~$50k+/yr) used by latency-sensitive consumers (HFT signal shops). The free RSS feeds lag by 5–10 seconds; PDS is sub-second.

Each Form 4 filing's primary document is typically at `https://www.sec.gov/Archives/edgar/data/{cik}/{accession-no-dashes}/{primary-doc}.xml` (often literally named `wf-form4_*.xml` or similar — there is no naming convention guarantee; resolve via the filing index `index.json`).

### B.10 Tools and reference implementations

- **edgartools** (Python, by Dwight Gunning) — actively maintained, parses Form 4 XML to typed Python objects; <https://github.com/dgunning/edgartools>
- **python-edgar-tools / sec-edgar** — multiple variants on PyPI with overlapping APIs
- **SEC EDGAR Public Dissemination Service** — official sub-second push (paid)
- **secdatabase.com** — third-party normalized DB with Form 4 dictionary at <https://www.secdatabase.com/Articles/tabid/42/ArticleID/10/Form-4-Transaction-Code-Definitions.aspx>
- **form345.com** — focused on Forms 3/4/5 parsing as a service (blog at <https://blog.form345.com/form-4-transaction-codes-decoded>)
- **mccgr/edgar** (academic) — pre-existing Form 3/4/5 ETL with schema versioning notes (<https://github.com/mccgr/edgar/issues/42> documents the X0101/null `schemaVersion` quirk for pre-2007 filings)

---

## Part C — Schedule 13D / 13G (5%+ blockholders)

### C.1 Filing trigger

Section 13(d) of the Exchange Act requires any person (individual or group) who, after acquiring beneficial ownership of more than 5% of any class of registered equity, to file a Schedule 13D with the SEC. **Schedule 13G** is the short-form alternative available to:

- "Qualified Institutional Investors" (QIIs) — broker-dealers (BD), banks (BK), insurance companies (IC), investment companies registered under the ICA-40 (IV), investment advisers registered under the IAA (IA), employee benefit plans (EP), parent holding companies / control persons of QIIs (HC), savings associations (SA), church plans (CP), etc. — *provided* the acquisition was in the ordinary course of business and without a change-of-control purpose.
- "Passive Investors" who own less than 20% and certify they do not intend to influence control.
- "Exempt Investors" who acquired the position before the issuer became §13(d)-subject.

### C.2 Filing deadlines (post 2024-02-05 modernization)

The October 2023 SEC modernization rule (Release Nos. 33-11253, 34-98704; effective 2024-02-05) compressed the deadlines materially (source: <https://www.sec.gov/newsroom/press-releases/2023-219>; <https://www.sec.gov/files/33-11253-fact-sheet.pdf>):

| Filing                  | Pre-2024 deadline                        | Post 2024-02-05 deadline                                  |
|---|---|---|
| Initial 13D             | 10 calendar days from crossing 5%        | **5 business days** from crossing 5%                       |
| 13D amendment           | "Promptly"                               | **2 business days** of material change                     |
| Initial 13G (QII)       | 45 days after calendar-year end          | 45 days after **calendar-quarter end** (effective 2024-09-30) |
| Initial 13G (Passive)   | 10 days from crossing 5%                 | **5 business days** from crossing 5%                       |
| 13G amendments          | 45 days after calendar-year end (any change) | 45 days after **calendar-quarter end** if any material change; **5 business days** if QII crosses 10%; **5 business days** if Passive crosses 10% (then 5 business days for further 5% changes) |
| EDGAR daily cut-off     | 5:30 p.m. ET                             | **10:00 p.m. ET** (effective 2024-02-05; gives an extra ~4.5 hours)|

The compressed timelines combined with the 2024-12-18 structured-data mandate produce a discontinuity in the data: faster-filed, more-parseable 13D filings starting in 2024 should not be naively compared to historical 13D filings for purposes of "how quickly did the activist file?" because the legal deadline changed.

### C.3 Cover-page schema (Items 1–7)

Both schedules share a numbered cover-page form with the following items (source: 17 CFR 240.13d-101 and 13d-102 at <https://www.law.cornell.edu/cfr/text/17/240.13d-101>):

```
1. Names of Reporting Persons
2. Check the Appropriate Box if a Member of a Group (a)/(b)
3. SEC use only
4. Source of Funds (SC=subject company, BK=bank loan, AF=affiliate funds,
   WC=working capital, PF=personal funds, OO=other) — 13D only
5. Check Box if Disclosure of Legal Proceedings Is Required Pursuant to Items 2(d) or 2(e)
6. Citizenship or Place of Organization
7. Sole Voting Power
8. Shared Voting Power
9. Sole Dispositive Power
10. Shared Dispositive Power
11. Aggregate Amount Beneficially Owned by Each Reporting Person
12. Check Box if the Aggregate Amount in Row (11) Excludes Certain Shares
13. Percent of Class Represented by Amount in Row (11)
14. Type of Reporting Person  (enum: BD, BK, IC, IV, IA, EP, HC, SA, CP, CO, PN, IN, OO,
                              DR (registered investment adviser), FI (federal insurance),
                              MM (insurance holding company))
```

Body items differ by form. Schedule 13D additionally requires:

```
Item 1. Security and Issuer (name, address, class, CUSIP)
Item 2. Identity and Background (each reporting person — name, address, principal business,
        citizenship, criminal/civil judgments per 2(d)/2(e))
Item 3. Source and Amount of Funds
Item 4. Purpose of Transaction  — the load-bearing free-text disclosure for activists;
        must enumerate any plans for extraordinary corporate action (M&A, tender offer,
        capitalization change, dividend change, sale, change in board/management, etc.)
Item 5. Interest in Securities of the Issuer (shares, percentage, transactions in past 60 days)
Item 6. Contracts, Arrangements, Understandings or Relationships with Respect to Securities
        of the Issuer — must disclose ALL derivative interests (post-2024 expansion)
Item 7. Material to Be Filed as Exhibits (joint-filer agreements, written 10b5-1-style plans,
        loan agreements, etc.)
```

Schedule 13G is a stripped-down version with Items 1–10 only and no "purpose of transaction" requirement — that is precisely the legal trade-off: 13G filers waive the right to influence control in exchange for a shorter disclosure burden.

### C.4 The Type of Reporting Person enumeration

The single-letter `Type of Reporting Person` code (Item 14) drives most downstream cross-sectional analysis. The standard enumeration (per 17 CFR 240.13d-101 and 13d-102):

```
BD  Broker-Dealer (Section 3(a)(6) registrant)
BK  Bank (Section 3(a)(6))
IC  Insurance Company (Section 3(a)(19))
IV  Investment Company (registered under ICA 1940)
IA  Investment Adviser (registered under IAA 1940 or state)
EP  Employee Benefit Plan, Pension Fund, or Endowment
HC  Parent Holding Company / Control Person
SA  Savings Association (Section 3(b) of the BHCA)
CP  Church Plan (Section 414(e) of the Code)
CO  Corporation
PN  Partnership
IN  Individual
OO  Other
```

`DR`, `FI`, `MM` appear in some older filings but are non-canonical / superseded by `IA`, `IC`, and `HC` respectively `[unverified — confirm cleanly against current rule text]`.

### C.5 Group filings under §13(d)(3)

When two or more persons "act as a partnership, limited partnership, syndicate, or other group for the purpose of acquiring, holding, or disposing of securities," they form a "group" under §13(d)(3) and must file jointly. Practical examples:

- Wolf-pack activism (multiple hedge funds coordinating before an activist campaign)
- Family offices voting together
- 13D filers backed by sub-advised funds

A joint filing has multiple `<reportingPerson>` blocks within the same Schedule. ats-eqt should preserve each reporting person as a distinct fact row keyed on `(filing_id, reporting_person_id)`.

### C.6 The structured-data mandate (2024-12-18)

Before 2024-12-18 Schedules 13D/G were filed as HTML or plain ASCII free-form. The new XML-based filing format (effective 2024-12-18 per the October 2023 modernization rule's structured-data section) introduces a schema covering:

- All cover-page numeric fields (now strictly numeric — no "Less than 1%" string)
- Identification checkboxes
- A structured textual-narrative element for Items 3, 4, 6, 7

Footnotes attached to numeric fields are *not* permitted in the XML schema — they must move into the narrative items. This is a meaningful break in continuity for any backtest that relied on the footnote convention "approximately X%". (Source: <https://www.olshanlaw.com/newsroom/alerts/client-alert-important-reminder-schedules-13d-and-13g-must-be-filed-using-structured-machine-readable-xml-based-language-beginning-december-18-2024>.)

Operational implication: ats-eqt needs two parsing pipelines for 13D/G:
- **Pre 2024-12-18:** HTML/text parser with regex extraction of numbered items; quality unstable.
- **Post 2024-12-18:** XML parser against the new EDGAR Filer Manual Volume II Chapter 9 specification (`<sch13DDocument>` / `<sch13GDocument>` root) `[unverified — exact root element naming; confirm from EDGAR Filer Manual]`.

The earliest of the 13D filing-deadline compression (2024-02-05) and the structured-data mandate (2024-12-18) bracket a transition year in which the filing population may not be directly comparable to either side.

### C.7 13D vs 13G in practice

| Dimension                         | Schedule 13D                            | Schedule 13G                                                |
|---|---|---|
| Filer eligibility                 | Anyone (default)                        | Qualified Institutional Investor, Passive, or Exempt        |
| Purpose disclosure                | Required (Item 4)                       | Not required                                                |
| Initial filing window             | 5 business days                         | 5 business days (Passive) / 45 days after Q-end (QII)       |
| Amendment cadence                 | 2 business days on material change      | Quarterly (if material) + 5-business-day triggers           |
| Typical use case                  | Activist, M&A acquirer, take-private    | Passive index funds, mutual funds, pensions                  |
| Average filer profile             | Hedge fund, family office, individual   | BlackRock, Vanguard, State Street, FMR                       |
| Annual filings per issuer (S&P 500) | ~0–3                                  | ~5–15                                                       |

### C.8 Schedule 13D/G amendments — 13D/A and 13G/A

Amendments have form-type suffix `/A` and a sequence number embedded in the filing. The new XML schema preserves a stable `originalAccessionNumber` link to the schedule being amended, simplifying chain reconstruction. Pre-XML the amendment chain had to be reconstructed by matching reporting-person CIK + issuer CUSIP + form sequence — a notorious source of bugs in DIY pipelines.

---

## Part D — Form N-PORT (mutual-fund / ETF fund-level holdings)

### D.1 Filing trigger and cadence

Registered open-end management investment companies (mutual funds), exchange-traded funds (ETFs), registered closed-end funds, and unit investment trusts (UITs) — but **not** money-market funds (which file Form N-MFP) — must file Form N-PORT. Money-market funds and small-business investment companies are excluded.

**Filing cadence:** monthly. The N-PORT-P for months 1 and 2 of a fiscal quarter is filed within **60 days** of month-end and was historically **non-public** until the third month of the quarter, when the corresponding N-PORT-P became public. Money-market funds and SBICs file Form N-MFP instead.

**Current public availability (as of 2026-05-14):**
- Months 1 and 2 of each fiscal quarter: filed but **non-public** (per FOIA-exempt §17(f)).
- Month 3 of each fiscal quarter: **public** 60 days after month-end.
- The August 2024 amendment to require monthly-public availability of N-PORT was scheduled to take effect 2025-11-17 but has been **delayed to 2027-11-17** for fund groups with ≥$1B in net assets and **2028-05-18** for smaller fund groups (April 2025 extension order; source: <https://www.sec.gov/newsroom/press-releases/2025-64>; <https://www.federalregister.gov/documents/2025/04/22/2025-06861>).
- In February 2026 the SEC published a proposal to scale back parts of the 2024 amendments — wave-3 should re-check the final status (source: <https://www.sec.gov/newsroom/press-releases/2026-19-sec-proposes-amendments-reduce-burdens-reporting-fund-portfolio-holdings>).

### D.2 Filer hierarchy

- **Investment company (registrant)** — the legal entity (e.g. "Vanguard Index Funds"), keyed by CIK.
- **Series** — a sub-fund under the registrant (e.g. "Vanguard Total Stock Market Index Fund"), keyed by Series ID (`S000NNNNNN`).
- **Class** — a share class of a series (e.g. "Investor Shares", "Admiral Shares", "Institutional Shares"), keyed by Class ID (`C000NNNNNN`).

N-PORT is filed at the **series level**. Class-level differences are limited to fees and minimum-investment requirements; portfolio holdings are series-shared. ats-eqt's `fund` dimension must encode the three-level hierarchy.

### D.3 The investmentOrSecurity schema (Part C)

The portfolio-holdings section of Form N-PORT (Part C: "Schedule of Portfolio Investments") is a repeating `<invstOrSec>` block. Source: the SEC's Form N-PORT specification PDF (<https://www.sec.gov/files/formn-port.pdf>) and the XML technical spec (Version 1.7 draft; <https://www.sec.gov/info/edgar/specifications/form-n-port-xml-tech-specs.htm>).

Field set:

| Element                              | Type            | Semantics                                                                                |
|---|---|---|
| `name`                               | string          | Name of issuer (e.g. "Apple Inc.").                                                       |
| `lei`                                | string (LEI)    | LEI of issuer if available. NULL if no LEI assigned.                                      |
| `title`                              | string          | Title of issue (e.g. "Common Stock", "5.25% Senior Notes due 2031").                     |
| `cusip`                              | string (9)      | CUSIP-9 of the security; required if no other ID.                                          |
| `identifiers/isin` or `identifiers/ticker` or `identifiers/other/@otherDesc` | string | ISIN or ticker (if CUSIP not available). |
| `balance`                            | decimal         | Balance (number of shares or principal amount; see `units`).                              |
| `units`                              | enum            | `NS` (number of shares), `PA` (principal amount), `NC` (number of contracts), `OU` (other units). |
| `descOthUnits`                       | string          | Free-text description if `units = OU`.                                                    |
| `curCd` / `curGenMeas`               | string (ISO-4217) / decimal | Currency of measurement and conversion measure if non-USD.                       |
| `valUSD`                             | decimal         | Value in USD as of the report period end.                                                  |
| `pctVal`                             | decimal         | Percentage of net assets attributable to this position.                                    |
| `payoffProfile`                      | enum            | `Long` / `Short` / `N/A`.                                                                  |
| `assetCat`                           | enum (see below)| Asset category — equity-common, equity-preferred, debt, derivative-*, ABS-*, repo, etc.   |
| `issuerCat`                          | enum (see below)| Issuer category — corporate, US Treasury, US gov agency, USGSE, municipal, non-US sovereign, private fund, registered fund, other. |
| `invCountry`                         | string (ISO-3166-1 alpha-2) | Investment country.                                                            |
| `isRestrictedSec`                    | boolean         | Y/N restricted security flag.                                                              |
| `fairValLevel`                       | enum 1/2/3      | ASC 820 fair-value hierarchy level.                                                        |
| `debtSec/*`                          | structured block | Required when `assetCat` = debt — maturityDt, couponKind (Fixed/Float/Zero/Variable/None), annualizedRt, isDefault, areIntrstPmtsInArrs, isPaidKind, etc. |
| `debtSec/dbtSecRefInstruments`       | structured block | Reference-instrument metadata for convertibles. |
| `repurchaseAgrmt/*`                  | structured block | Required when `assetCat` = repurchase agreement — clearedCentCparty, isTriParty, repurchaseRt, maturityDt, collaterals. |
| `derivativeInfo/*`                   | structured block | Required when `assetCat` contains "derivative-*". Sub-shapes per derivative class:        |
|   `derivativeInfo/futureContract`    |                 | counterparty, payOffProfile, expDate, notionalAmt, unrealizedAppr. |
|   `derivativeInfo/fwdFoonContract`   |                 | forward FX contract — counterparty, ccyAmtCurrencyTrades, settlementDt, unrealizedAppr. |
|   `derivativeInfo/swap`              |                 | swap — counterparty, payOffProfile, descRefInstrument, swapFlag (CDS/IRS/TRS/etc), termination, etc. |
|   `derivativeInfo/option`            |                 | option — counterparty, putOrCall, writtenOrPur, descRefInstrument, sharesCnt, expDt, exercisePrice, unrealizedAppr. |
|   `derivativeInfo/swaption`          |                 | swaption — counterparty, putOrCall, writtenOrPur, expDt, swapDesc. |
|   `derivativeInfo/otherDerivative`   |                 | catch-all. |
| `securityLending/loanByFundCondition` | structured | Whether the position is on loan; cash collateral; non-cash collateral. |
| `lendingInfo/isCashCollateral`       | boolean         |                                                                                            |
| `lendingInfo/isNonCashCollateral`    | boolean         |                                                                                            |
| `lendingInfo/isLoanByFund`           | boolean         |                                                                                            |

### D.4 Enumeration values (assetCat / issuerCat)

`assetCat` values (representative, drawn from filed N-PORT and the SEC form):

```
STIV   Short-term investment vehicle (money-market fund / cash management)
RA     Repurchase agreement
EC     Equity-common
EP     Equity-preferred
DBT    Debt
DCO    Derivative-commodity
DCR    Derivative-credit
DE     Derivative-equity
DFE    Derivative-foreign exchange
DIR    Derivative-interest rate
DOT    Derivatives-other
SN     Structured note
LON    Loan
ABS-MBS ABS-mortgage backed security
ABS-APCP ABS-asset backed commercial paper
ABS-CBDO ABS-collateralized bond/debt obligation
ABS-O  ABS-other
COMM   Commodity
RE     Real estate
OTH    Other
```

`issuerCat` values:

```
CORP   Corporate
UST    US Treasury
USGA   US Government Agency
USGSE  US Government Sponsored Entity
MUN    Municipal
NUSS   Non-US Sovereign
PF     Private fund
RF     Registered fund
OTH    Other
```

`[unverified — exact code strings; confirm against XML schema]`.

### D.5 N-PORT-P versus N-PORT-NT

- **N-PORT-P** ("public") — the form itself. Filed every month. First two months of a fiscal quarter remain non-public until the third-month filing.
- **N-PORT-NT** — notice / amendment / late-filing notice. Lower-volume signal but useful for fund-restructuring audit trails.

### D.6 Data access

- **SEC EDGAR N-PORT data sets:** quarterly bulk extracts of all publicly disseminated N-PORT-P filings at <https://www.sec.gov/data-research/sec-markets-data/form-n-port-data-sets>. CSV format aligned with the XML schema.
- **EDGAR Archives:** raw XML at `https://www.sec.gov/Archives/edgar/data/{cik}/{accession}/primary_doc.xml`.
- **WRDS:** linked via `crsp.fund_*` for CRSP Survivor-Bias-Free Mutual Fund Database joins.
- **Morningstar Open Mutual Fund holdings:** subscription-only premium dataset that incorporates N-PORT and direct-from-fund-company holdings; deeper history and faster than N-PORT.

### D.7 N-PORT vs 13F (cross-form link)

The high-value cross-form join, as the 13F file (Part G.6) noted: a manager's **13F manager-level positions** (long, ≥$200M aggregate value) reconciled against the **sum of their N-PORT fund-level positions** (the same manager's registered funds, all positions including short and non-13(f)). Discrepancies surface SMA positions, non-13(f) positions, and reporting-timing offsets. ats-eqt schema must support this join via `fund.adviser_cik → filer_13f.primary_cik` (Part I).

---

## Part E — Form 144 (Restricted-stock and affiliate sales)

### E.1 Filing trigger

Rule 144 under the Securities Act provides a safe harbor for the resale of *restricted securities* (acquired in a private placement / unregistered sale) and *control securities* (held by an affiliate of the issuer). An affiliate proposing to sell either restricted or control securities must file **Form 144** with the SEC if, within any three-month rolling period, the sale exceeds:

- **5,000 shares**, or
- **$50,000 in aggregate sale price**

Form 144 is filed *before* the sale, and the sale must occur within the **next 3 months** or the filer must file an amendment / re-file.

### E.2 Electronic filing mandate

Until 2022 Form 144 was filed in paper or HTML; the SEC's June 2022 amendments (released as 33-11070; effective phased — Form 144 filings for reporting-issuer securities required to be filed electronically since 2023-04-13) made EDGAR XML mandatory for all Form 144 filings related to reporting-company securities (source: <https://www.federalregister.gov/documents/2022/06/10/2022-12253/updating-edgar-filing-requirements-and-form-144-filings>; <https://www.sec.gov/submit-filings/filer-support-resources/how-do-i-guides/file-form-144-electronically>).

### E.3 Field-level schema

The Form 144 PDF (<https://www.sec.gov/files/form144.pdf>) and its XML technical specification define:

```
Part I — Information about the seller
  - Name and address of the affiliate
  - SSN/EIN (not in public dissemination)
  - Name of the issuer
  - Issuer SEC file number / CIK
  - Issuer address
  - Issuer telephone

Part II — Information about the proposed sale
  - Title of the class of securities to be sold
  - Number of shares to be sold ("aggregateNbrOfShares")
  - Approximate date of sale
  - Name of each broker through which sale is to be made
  - Approximate date of acquisition of the securities to be sold
  - Nature of acquisition (purchase, gift, stock split, etc.)
  - If acquired by purchase, name of the person from whom acquired and amount paid

Part III — Securities sold during past 3 months
  - Name and address of each seller affiliated with the seller
  - Title of securities sold
  - Date of sale
  - Number of shares sold
  - Gross proceeds

Signature block — Date, signature, name
```

Key XML elements `[unverified — confirm against EDGAR Form 144 XML technical specification]`:

```
form144Document
├── issuerInfo (issuerName, issuerCik, issuerAddress)
├── securitiesInformation
│   ├── titleOfClass
│   ├── aggregateNbrOfShares
│   ├── aggregateMarketValue
│   └── approxDateOfSale
├── securitiesToBeSoldInfo (repeating; broker, date, shares)
├── securitiesSoldPast3MonthsInfo (repeating; seller, title, date, shares, grossProceeds)
└── remarks
```

### E.4 Why Form 144 matters separately from Form 4

- Form 144 is a *forward-looking* declaration of intent to sell — filed before the trade.
- Form 4 is a *backward-looking* report of the executed trade — filed within 2 business days after.
- The pair can be reconciled: `(Form 144 intent, Form 4 actual)` reveals (a) how much of the proposed sale actually completed and (b) the realized vs declared price. Discrepancies are a useful audit signal.
- Founders and 10%+ holders often appear on Form 144 even when they have no Section 16 obligation — Form 144 is the only systematic disclosure of large affiliate sales for non-§16 affiliates.

---

## Part F — Form N-PX (Mutual-fund proxy voting record)

### F.1 Filing trigger and modernization

All registered management investment companies, plus (since 2024) institutional investment managers required to file Form 13F that exercised voting authority over executive-compensation matters, must file Form N-PX annually disclosing how they voted each proxy ballot during the year. The post-2022 amendments (effective 2024-08-31, covering votes from 2023-07-01 to 2024-06-30 onward) introduced:

- **Mandatory XML structured data** (custom XML schema specific to Form N-PX).
- **Standardized vote-categorization fields** — environmental/climate, human rights, human capital/workforce, DEI, "other social", governance, executive compensation, capital management, M&A, audit, etc.
- **Disclosure of securities lending impact** on voting (whether lent shares were recalled to vote).
- **Sub-categorization fields** to make vote records cross-section-able.

(Source: <https://www.sec.gov/newsroom/press-releases/2022-198>; <https://www.toppanmerrill.com/blog/form-n-px-to-require-mutual-funds/>; <https://www.sec.gov/investment/enhanced-reporting-proxy-votes>.)

### F.2 Schema highlights

```
nPXFilingDocument
├── filerInformation
│   ├── registrantInformation (CIK, series, class)
│   └── managerInformation (for §13F filers reporting say-on-pay)
├── voteTable (repeating per matter voted)
│   ├── issuerName
│   ├── issuerCusip
│   ├── meetingDate
│   ├── matterDescription
│   ├── voteCategory  (environmental, social, governance, M&A, compensation, etc.)
│   ├── voteSubcategory (climate, human rights, DEI, etc.)
│   ├── shareholderProponent (boolean — was matter raised by a shareholder)
│   ├── recommendation (management's recommendation)
│   ├── voteCast (For / Against / Abstain / Withhold)
│   ├── sharesVoted
│   ├── sharesLoaned (and recalled-to-vote indicator)
│   └── disclosureInfo (any custom narrative)
└── exhibits
```

`[unverified — element-naming details; the XML schema is published in the EDGAR Filer Manual]`.

### F.3 Why N-PX matters for ats-eqt

- ESG-aligned investing alpha: funds that vote with shareholder ESG proposals may attract ESG-mandated AUM. The N-PX corpus is the only structured source for fund-level voting behavior.
- Activist campaign attribution: tying 13D filings to subsequent N-PX votes by passive holders measures the activist's persuasion success.
- "Proxy-voting consensus" cross-fund — how concentrated is the vote on any given climate proposal?

---

## Part G — Tender offers (Schedule TO / SC TO-T / 14D-9)

These are the M&A-event-adjacent ownership filings:

| Form        | Filer                                        | Purpose                                                                                  |
|---|---|---|
| Schedule TO (TO-I) | Issuer making a self-tender                | Issuer offers to repurchase its own securities.                                          |
| Schedule TO-T  | Third-party bidder making a tender offer | Third party (e.g. PE firm) offers to purchase >5% of a target's shares.                  |
| Schedule 14D-9 | Target company                            | Target's response — recommend, oppose, or remain neutral on the tender offer.            |
| Schedule 14D-9F | Foreign target                          | Foreign target's response.                                                               |
| SC 13E-3    | Going-private transaction filer              | When an issuer or affiliate goes private.                                                 |

Filed in HTML/PDF; XBRL-tagged at limited granularity. Tender-offer rules require filing within 10 business days of commencement; target response within 10 business days of bidder's commencement.

Cross-references 13D: any tender offer by a 5%+ holder also triggers a Schedule 13D amendment under Item 4 (Purpose of Transaction). The combined `(13D, Schedule TO, 14D-9)` triad is the canonical M&A-event-study label set.

---

## Part H — Vendor-side deep dives

### H.1 FactSet Ownership

- **Insider coverage:** US Forms 3/4/5 normalized back to 1986; non-US declarable-stake regimes covered separately (UK Companies Act, EU TR Major-Holdings, Japan EDINET).
- **Schema:** `factset_own_*` family on WRDS (Snowflake / DataFeed elsewhere):
  - `own_inst_*` — institutional holdings (13F + global declarable-stake)
  - `own_stk_*` — stakeholder snapshot
  - `own_insider_*` — insider transactions normalized from Form 4
  - `own_fund_*` — fund-level holdings (rolled up from 13F + N-PORT + non-US)
  - `[unverified — exact schema-naming in WRDS]`
- **"Decision-maker" attribution:** FactSet's differentiator — rolls 13F filer to the *true decision-making entity*, collapsing sub-advisor relationships and fund-of-fund structures. Same logic applied on the insider side normalizes officer-title free-text to a controlled vocabulary.
- **Three date fields per ownership position:** `report_date`, `filing_date`, `transfer_date` (when FactSet processed). Bitemporal by construction.
- **Coverage of activism:** FactSet acquired SharkRepellent for activism / proxy-fight data — proxy contest events, activist demands, settlement terms.
- **Source URLs:** <https://www.factset.com/marketplace/catalog/product/factset-ownership>; <https://insight.factset.com/resources/at-a-glance-factset-ownership-standard-datafeed>; <https://developer.factset.com/api-catalog/factset-ownership-api>.

### H.2 S&P Capital IQ Pro Ownership

- **Coverage scale:** 49k+ public companies, 35k+ institutions, 51k+ funds, **337k+ insiders**, 12k+ activism campaigns (source: <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro>).
- **Schema:** the `ciqOwnership` table family (extension of the long-format `ciqFin*` pattern documented in `vendors/sp_global.md` Section 3.1):
  - `ciqOwnership` — central fact table; rows per (holder × security × date).
  - `ciqInsider` — insider master, keyed by `personId`.
  - `ciqInsiderTransaction` — Form 4 / Form 144 transactions per insider per security.
  - `ciqInstitution` — institutional-holder master.
  - `ciqFundHolding` — N-PORT-style fund-level holdings.
  - `[unverified — exact table naming; some research guides cite `ciqOwnershipDetail`]`
- **Activism integration:** every 13D-filed activist position is tagged to a campaign (`ciqActivismCampaign`) with demands, outcomes, settlement terms. The single biggest CIQ differentiator vs FactSet on the ownership axis.
- **Insider scoring:** S&P's proprietary "transaction quality" score (cluster buys, opportunistic timing, post-earnings purchase windows).
- **Delivery:** Capital IQ Pro web, Excel plug-in, Xpressfeed bulk, Snowflake share, S&P Marketplace.

### H.3 Bloomberg HDS / PHDC / OWN

- **Terminal functions:** `HDS <GO>` (Top Holders Summary), `OWN <GO>` (Ownership), `PHDC <GO>` (Portfolio Holdings Detail), `INSD <GO>` (Insider Transactions), `INSI <GO>` (Insider Trading Activity), `13F <GO>` (specifically 13F filings), `IRR <GO>` (Insider Relationship Reports). `FLNG <GO>` for filings.
- **Field reference (representative — Bloomberg field reference is paywalled, the names below come from third-party documentation):**
  - `EQY_HOLDINGS_LATEST` — latest holdings snapshot for an entity.
  - `EQY_HOLDERS_ANALYSIS` — analytical holder-side rollup.
  - `EQY_INSIDERS_TRANSACTIONS_*` — Form 4 transactions.
  - `EQY_FUND_*` family for N-PORT-derived fund holdings.
  - `[unverified — Bloomberg field reference is gated behind Terminal]`.
- **Differentiator:** FIGI-native (no CUSIP-licensing exposure on the consumer side); tightly integrated with Bloomberg PORT for portfolio impact analysis; OPRA-equivalent options integration for derivative-table rows.
- **Delivery:** Terminal, BPipe, Bloomberg Data License (DL) Enterprise feed, DL+ Snowflake Native App.
- **Sources:** <https://data.bloomberglp.com/professional/sites/10/Security-Ownership-fact-sheet.pdf>; <https://www.bloomberg.com/professional/dataset/united-states-ownership-filings/>.

### H.4 LSEG (Refinitiv) Stock Ownership + Lipper + eMAXX

- **Stock Ownership** (LSEG main): institutional holdings + insider transactions, US insider history from **1986**, 13F from 1978 — the longest commercially distributed run.
- **Lipper Mutual Fund Holdings:** below-13F-threshold mutual fund holdings, including APAC/EMEA funds where 13F doesn't reach; ~70k+ mutual/hedge funds.
- **eMAXX (fixed-income institutional):** bond holdings by institutional investor; the bond-side cousin of 13F. Monthly cadence.
- **Symbology:** PermID-keyed entity master (open via permid.org); RIC-keyed instruments (proprietary).
- **Delivery:** Eikon / LSEG Workspace, DataScope Select, LSEG Data Platform API, WRDS share.
- **Source:** <https://www.lseg.com/en/data-analytics/financial-data/company-data/company-ownership-information-profiles>.

### H.5 WhaleWisdom

- **Coverage:** 13F filers (full history 2001-Q1+), Schedule 13D/G filings going back to **2006** (15+ years), Form 4 insider transactions.
- **API:** `holdings`, `holders`, `filer`, `stock` commands; `include_13d=1` parameter to augment holdings with Schedule 13D/G data; rate-limited 20 req/min; Premium subscribers get unlimited API + nightly FTP files (source: <https://whalewisdom.com/help/api>; <https://whalewisdom.com/info/subscription_info>).
- **Premium-tier pricing:** $300/yr Standard, $500/yr Pro, Enterprise custom.
- **Differentiator:** "WhaleScore" performance ranking; activist tracking page (<https://whalewisdom.com/schedule13d>); cluster analytics across 13F+13D+Form 4.

### H.6 Quiver Quantitative

- **Endpoints:** Congressional Trades (Senate + House periodic transaction reports, January 2016+), Insider Trading (Form 4), Lobbying, Patents, Government Contracts, WSB sentiment.
- **Congressional Trading coverage:** ~1,800 US Equities, cumulative-return calculation per politician.
- **Pricing:** Hobbyist $30/mo or $300/yr (Tier 1); Trader $75/mo or $750/yr (Tier 1+2); Commercial custom. (source: <https://api.quiverquant.com/docs/>; <https://www.quiverquant.com/congresstrading/>).
- **Schema:** REST/JSON with API-key auth; Python SDK `quiverquant` on PyPI.

### H.7 OpenInsider (free)

- **Coverage:** Form 4 only; live-stream from EDGAR.
- **Methodology:** "Cluster Buys" methodology — companies where multiple insiders bought stock in a short window. Industry-and-insider-count tagged on cluster pages. Pure `P` (open-market purchase) rows filtered separately from `S`/`F`/`M`/etc.
- **URL:** <http://openinsider.com/>; sub-pages: `/insider-purchases`, `/insider-sales`, `/latest-cluster-buys`, `/latest-insider-purchases-25k`.
- **License:** Free, display-ads monetized; no formal API documented but page contents are scrapeable.

### H.8 InsiderInsights / Form4Oracle / 2iQ Research / Verafin

- **InsiderInsights** — retail newsletter; "Insider Buy Recommendations" scored on cluster + sentiment. ~$50–$500/mo.
- **Form4Oracle** — Form 4 normalization + alerts. ~$50–$300/mo.
- **2iQ Research** — institutional insider analytics; broader than US, covers global insider data (EU, Asia where disclosed). Subscriber-only; price not published.
- **Verafin** (acquired by Nasdaq 2021) — Forms 3/4/5 as AML/anti-fraud signal for bank compliance, not investment analytics. Sold to ~2,400 banks. Bank enterprise pricing.

### H.9 Congressional and executive-branch holdings

- **STOCK Act 2012** (Pub.L. 112-105, signed 2012-04-04) requires members of Congress, congressional officers/employees, and certain executive-branch employees to disclose financial transactions:
  - **Periodic Transaction Reports (PTRs)** — within 30 days of receiving notification of a transaction >$1,000; in no case >45 days after the transaction.
  - **Annual Financial Disclosure Reports** (FDRs) — annually, the long-form personal financial disclosure with assets, income, liabilities.
- **House clerk disclosures:** <https://disclosures-clerk.house.gov/FinancialDisclosure> — PTRs and FDRs as PDFs.
- **Senate Select Committee on Ethics:** <https://www.ethics.senate.gov/public/index.cfm/financialdisclosure> — same for senators.
- **OGE Form 278** — executive-branch annual disclosure (cabinet, EOP staff, agency heads); paper / PDF.
- **Third-party normalizers:**
  - **Senate Stock Watcher / senatestockwatcher.com** — JSON-formatted normalization of senate PTRs.
  - **House Stock Watcher / housestockwatcher.com** — same for house PTRs.
  - **capitoltrades.com** — premium / freemium consolidated view.
  - **Quiver Quantitative** — paid API access to both senate and house data (Part H.6).
- **Cross-form linkage value:** congressional-trade signals are noisy and small-volume but high-publicity. STOCK Act compliance is patchy (45-day deadline routinely missed); ats-eqt should record `disclosure_lag = filing_date - transaction_date` per row as a compliance audit field.

---

## Part I — Recommended ats-eqt schema

This extends the bitemporal long-format pattern from `13f_holdings.md` Part H and `schemas/data_models_and_methodology.md`. All tables include `(valid_from, valid_to, knowledge_from, knowledge_to)` for bitemporality; the SQL below omits them for compactness.

### I.1 Insider person dimension

```sql
-- The natural person filing as a §16 insider (or non-§16 affiliate via Form 144)
CREATE TABLE insider (
  insider_id         BIGINT      PRIMARY KEY,           -- ats-eqt internal stable key
  reporting_owner_cik INTEGER    NULL,                  -- EDGAR rptOwnerCik (NULL if no CIK)
  full_name          TEXT        NOT NULL,
  full_name_norm     TEXT        NOT NULL,              -- normalized (strip suffix, fold case)
  citizenship        TEXT        NULL,                  -- 2-letter ISO from 13D Item 6
  date_of_birth      DATE        NULL,                  -- inferred from earliest filing (proxy)
  resolution_source  TEXT        NOT NULL,              -- 'EDGAR_CIK' | 'NAME_MATCH' | 'MANUAL'
  notes              TEXT
);

CREATE INDEX ix_insider_cik ON insider(reporting_owner_cik);
CREATE INDEX ix_insider_name_norm ON insider(full_name_norm);
```

### I.2 Insider × entity × role (bitemporal relationship)

```sql
-- One row per (insider, issuer, role) period
CREATE TABLE insider_relationship (
  insider_id          BIGINT      NOT NULL,             -- → insider
  entity_id           BIGINT      NOT NULL,             -- → entity (the issuer)
  is_director         BOOLEAN     NOT NULL DEFAULT FALSE,
  is_officer          BOOLEAN     NOT NULL DEFAULT FALSE,
  is_ten_percent_owner BOOLEAN    NOT NULL DEFAULT FALSE,
  is_other            BOOLEAN     NOT NULL DEFAULT FALSE,
  officer_title_raw   TEXT        NULL,                 -- free text from Form 4
  officer_title_norm  TEXT        NULL,                 -- normalized: CEO/CFO/COO/Director/etc.
  other_text          TEXT        NULL,                 -- when is_other = 1
  valid_from          DATE        NOT NULL,
  valid_to            DATE        NOT NULL DEFAULT '9999-12-31',
  PRIMARY KEY (insider_id, entity_id, valid_from)
);
```

### I.3 Insider transaction fact

```sql
CREATE TABLE insider_transaction (
  filing_id            BIGINT       NOT NULL,           -- → filing_form4 (B.10 below)
  insider_id           BIGINT       NOT NULL,           -- → insider
  entity_id            BIGINT       NOT NULL,           -- → entity (issuer)
  security_id          BIGINT       NOT NULL,           -- → security (ats-eqt internal)
  transaction_date     DATE         NOT NULL,
  deemed_execution_date DATE        NULL,
  is_derivative        BOOLEAN      NOT NULL,
  transaction_code     CHAR(1)      NOT NULL,           -- P/S/A/M/F/I/D/J/K/X/V/G/L/W/Z/C/E/H/O/U
  acquired_disposed    CHAR(1)      NOT NULL,           -- 'A' or 'D'
  transaction_shares   NUMERIC(20,4) NOT NULL,
  transaction_price    NUMERIC(20,4) NULL,
  shares_owned_following NUMERIC(20,4) NOT NULL,
  direct_indirect      CHAR(1)      NOT NULL,           -- 'D' or 'I'
  nature_of_ownership  TEXT         NULL,
  rule_10b5_1_indicator BOOLEAN     NULL,               -- post-2023-04-01 only
  plan_10b5_1_id       BIGINT       NULL,               -- → tradingplan_10b5_1
  equity_swap_involved BOOLEAN      NOT NULL DEFAULT FALSE,
  underlying_security_id BIGINT     NULL,               -- → security (for derivative rows)
  underlying_shares    NUMERIC(20,4) NULL,
  conversion_or_exercise_price NUMERIC(20,4) NULL,
  exercise_date        DATE         NULL,
  expiration_date      DATE         NULL,
  footnote_ids         TEXT[]       NULL,               -- references into filing_form4.footnotes
  reported_early       BOOLEAN      NOT NULL DEFAULT FALSE,  -- 'V' timeliness flag
  PRIMARY KEY (filing_id, security_id, transaction_date, transaction_code, is_derivative)
);

CREATE INDEX ix_insider_tx_insider_date ON insider_transaction(insider_id, transaction_date);
CREATE INDEX ix_insider_tx_entity_date ON insider_transaction(entity_id, transaction_date);
CREATE INDEX ix_insider_tx_code ON insider_transaction(transaction_code);
```

### I.4 Form 4 filing metadata

```sql
CREATE TABLE filing_form4 (
  filing_id            BIGINT       PRIMARY KEY,
  accession_number     TEXT         NOT NULL UNIQUE,
  form_type            TEXT         NOT NULL,           -- '3', '3/A', '4', '4/A', '5', '5/A'
  schema_version       TEXT         NOT NULL,           -- 'X0508' etc.
  issuer_cik           INTEGER      NOT NULL,
  period_of_report     DATE         NOT NULL,
  filing_date          DATE         NOT NULL,
  receive_date         TIMESTAMP    NOT NULL,
  remarks              TEXT         NULL,
  footnotes            JSONB        NULL,               -- { "F1": "text", "F2": "text", ... }
  source_url           TEXT         NOT NULL
);

CREATE INDEX ix_filing_form4_issuer ON filing_form4(issuer_cik, period_of_report);
```

### I.5 Schedule 13D/G snapshot

```sql
CREATE TABLE blockholder_filing (
  filing_id            BIGINT       PRIMARY KEY,
  accession_number     TEXT         NOT NULL UNIQUE,
  schedule_type        CHAR(3)      NOT NULL,           -- '13D', '13G', '13D/A', '13G/A'
  amendment_seq        INTEGER      NULL,               -- amendment sequence number
  amends_filing_id     BIGINT       NULL,               -- → blockholder_filing
  is_group_filing      BOOLEAN      NOT NULL DEFAULT FALSE,
  issuer_entity_id     BIGINT       NOT NULL,           -- → entity
  cusip                CHAR(9)      NOT NULL,           -- not exported via public API
  event_date           DATE         NOT NULL,           -- date of event triggering filing
  filing_date          DATE         NOT NULL,
  filing_lag_business_days INTEGER  NOT NULL,           -- derived: business days from event to filing
  is_xml_filing        BOOLEAN      NOT NULL,           -- TRUE if post 2024-12-18 structured
  purpose_text         TEXT         NULL,               -- Item 4 of 13D (free text)
  source_url           TEXT         NOT NULL
);

CREATE TABLE blockholder_reporting_person (
  filing_id            BIGINT       NOT NULL,           -- → blockholder_filing
  reporting_person_seq INTEGER      NOT NULL,           -- 1..N within filing
  insider_id           BIGINT       NULL,               -- if individual (IN); → insider
  entity_id            BIGINT       NULL,               -- if entity-typed; → entity
  reporting_person_name TEXT        NOT NULL,
  type_of_reporting_person CHAR(2)  NOT NULL,           -- BD/BK/IC/IV/IA/EP/HC/SA/CP/CO/PN/IN/OO
  citizenship_or_place_of_org TEXT  NULL,
  source_of_funds      CHAR(2)      NULL,               -- SC/BK/AF/WC/PF/OO (13D only)
  sole_voting_power    NUMERIC(20,4) NOT NULL DEFAULT 0,
  shared_voting_power  NUMERIC(20,4) NOT NULL DEFAULT 0,
  sole_dispositive_power NUMERIC(20,4) NOT NULL DEFAULT 0,
  shared_dispositive_power NUMERIC(20,4) NOT NULL DEFAULT 0,
  aggregate_beneficially_owned NUMERIC(20,4) NOT NULL,
  percent_of_class     NUMERIC(8,4) NOT NULL,
  excludes_certain_shares BOOLEAN   NOT NULL DEFAULT FALSE,
  legal_proceedings_flag BOOLEAN    NOT NULL DEFAULT FALSE,
  PRIMARY KEY (filing_id, reporting_person_seq)
);

CREATE INDEX ix_blockholder_issuer_date ON blockholder_filing(issuer_entity_id, filing_date);
CREATE INDEX ix_blockholder_person_filing ON blockholder_reporting_person(insider_id, filing_id);
```

### I.6 N-PORT fund holdings

```sql
CREATE TABLE fund (
  fund_id              BIGINT       PRIMARY KEY,        -- ats-eqt internal
  registrant_cik       INTEGER      NOT NULL,           -- the legal entity (810 series)
  series_id            TEXT         NOT NULL,           -- 'S000NNNNNN'
  fund_name            TEXT         NOT NULL,
  fund_family_entity_id BIGINT      NOT NULL,           -- → entity (Vanguard, BlackRock, etc.)
  adviser_cik          INTEGER      NULL,               -- joins to filer_13f for cross-form link
  inception_date       DATE         NULL,
  termination_date     DATE         NULL
);

CREATE TABLE fund_class (
  class_id             BIGINT       PRIMARY KEY,
  fund_id              BIGINT       NOT NULL,           -- → fund
  class_external_id    TEXT         NOT NULL,           -- 'C000NNNNNN'
  class_name           TEXT         NOT NULL,
  ticker               TEXT         NULL
);

CREATE TABLE filing_nport (
  filing_id            BIGINT       PRIMARY KEY,
  accession_number     TEXT         NOT NULL UNIQUE,
  fund_id              BIGINT       NOT NULL,           -- → fund
  form_type            TEXT         NOT NULL,           -- 'NPORT-P', 'NPORT-NT', 'NPORT-P/A'
  period_of_report     DATE         NOT NULL,           -- month end
  is_public            BOOLEAN      NOT NULL,           -- depends on month-in-quarter + post-2027 status
  filing_date          DATE         NOT NULL,
  source_url           TEXT         NOT NULL
);

CREATE TABLE fund_holding (
  filing_id            BIGINT       NOT NULL,           -- → filing_nport
  fund_id              BIGINT       NOT NULL,           -- denormalized for query speed
  period_of_report     DATE         NOT NULL,
  security_id          BIGINT       NULL,               -- → security; NULL for derivatives without underlying
  issuer_name_raw      TEXT         NOT NULL,           -- name from XML
  issuer_lei           CHAR(20)     NULL,
  title_of_issue       TEXT         NOT NULL,
  units                CHAR(2)      NOT NULL,           -- 'NS'/'PA'/'NC'/'OU'
  balance              NUMERIC(20,4) NOT NULL,
  value_usd            NUMERIC(20,2) NOT NULL,
  pct_of_net_assets    NUMERIC(8,4) NOT NULL,
  payoff_profile       CHAR(5)      NOT NULL,           -- 'Long'/'Short'/'N/A'
  asset_cat            TEXT         NOT NULL,           -- EC/EP/DBT/DCO/DCR/DE/DFE/DIR/...
  issuer_cat           CHAR(5)      NOT NULL,           -- CORP/UST/USGA/USGSE/MUN/NUSS/PF/RF/OTH
  investment_country   CHAR(2)      NULL,               -- ISO-3166-1
  is_restricted_security BOOLEAN    NOT NULL DEFAULT FALSE,
  fair_value_level     SMALLINT     NULL,               -- 1/2/3
  derivative_json      JSONB        NULL,               -- full derivativeInfo subtree
  debt_security_json   JSONB        NULL,               -- full debtSec subtree
  repo_json            JSONB        NULL,               -- full repurchaseAgrmt subtree
  is_on_loan           BOOLEAN      NOT NULL DEFAULT FALSE,
  PRIMARY KEY (filing_id, security_id, asset_cat, payoff_profile)
);

CREATE INDEX ix_fund_holding_fund_period ON fund_holding(fund_id, period_of_report);
CREATE INDEX ix_fund_holding_security_period ON fund_holding(security_id, period_of_report);
```

### I.7 Form 144 intent-to-sell

```sql
CREATE TABLE form144_intent (
  filing_id            BIGINT       PRIMARY KEY,
  accession_number     TEXT         NOT NULL UNIQUE,
  seller_name          TEXT         NOT NULL,
  insider_id           BIGINT       NULL,               -- → insider (when matched)
  issuer_entity_id     BIGINT       NOT NULL,           -- → entity
  security_id          BIGINT       NOT NULL,           -- → security
  filing_date          DATE         NOT NULL,
  approx_sale_date     DATE         NOT NULL,
  shares_proposed      NUMERIC(20,4) NOT NULL,
  aggregate_market_value NUMERIC(20,2) NOT NULL,
  brokers              TEXT[]       NOT NULL,
  acquisition_date     DATE         NULL,
  acquisition_nature   TEXT         NULL,               -- 'PURCHASE'/'GIFT'/'STOCK_SPLIT'/...
  past_3mo_sales_json  JSONB        NULL,               -- Part III rows
  source_url           TEXT         NOT NULL
);

-- Reconciliation: link Form 144 intent to subsequent Form 4 actual
CREATE TABLE form144_to_form4_link (
  form144_filing_id    BIGINT       NOT NULL,           -- → form144_intent
  insider_transaction_filing_id BIGINT NOT NULL,       -- → filing_form4
  match_confidence     NUMERIC(4,3) NOT NULL,           -- 0..1
  PRIMARY KEY (form144_filing_id, insider_transaction_filing_id)
);
```

### I.8 10b5-1 trading plan

```sql
CREATE TABLE tradingplan_10b5_1 (
  plan_id              BIGINT       PRIMARY KEY,
  insider_id           BIGINT       NOT NULL,           -- → insider
  entity_id            BIGINT       NOT NULL,           -- → entity (issuer)
  adoption_date        DATE         NOT NULL,           -- from Form 4 cover or footnote
  termination_date     DATE         NULL,
  modification_dates   DATE[]       NULL,               -- chain of modifications
  is_single_trade_plan BOOLEAN      NOT NULL,
  cooling_off_compliant BOOLEAN     NOT NULL,           -- derived: first trade-date - adoption-date >= 90d for officer/director
  source_filing_ids    BIGINT[]     NOT NULL,           -- supporting Form 4 filings
  issuer_item_408_disclosed BOOLEAN NOT NULL DEFAULT FALSE  -- cross-check from 10-Q/10-K Item 408
);
```

### I.9 N-PX proxy voting

```sql
CREATE TABLE proxy_vote (
  filing_id            BIGINT       NOT NULL,           -- → filing_nport (or separate filing_npx)
  fund_id              BIGINT       NOT NULL,           -- → fund (for fund-side filings)
  manager_filer_id     BIGINT       NULL,               -- → filer_13f (for §13F manager say-on-pay)
  issuer_entity_id     BIGINT       NOT NULL,           -- → entity
  meeting_date         DATE         NOT NULL,
  matter_description   TEXT         NOT NULL,
  vote_category        TEXT         NOT NULL,           -- ENV/SOC/GOV/COMP/MA/AUDIT/CAPITAL/OTHER
  vote_subcategory     TEXT         NULL,               -- climate/human_rights/DEI/...
  is_shareholder_proposed BOOLEAN   NOT NULL,
  management_recommendation TEXT    NULL,
  vote_cast            TEXT         NOT NULL,           -- For/Against/Abstain/Withhold
  shares_voted         NUMERIC(20,4) NOT NULL,
  shares_on_loan_at_record_date NUMERIC(20,4) NULL,
  loan_recalled_to_vote BOOLEAN     NULL,
  PRIMARY KEY (filing_id, issuer_entity_id, meeting_date, matter_description)
);
```

### I.10 Congressional / STOCK Act disclosure

```sql
CREATE TABLE congressional_disclosure (
  disclosure_id        BIGINT       PRIMARY KEY,
  chamber              CHAR(1)      NOT NULL,           -- 'H' or 'S'
  member_id            BIGINT       NOT NULL,           -- internal
  member_name          TEXT         NOT NULL,
  filing_type          TEXT         NOT NULL,           -- 'PTR' or 'FDR'
  filing_date          DATE         NOT NULL,
  transaction_date     DATE         NULL,               -- for PTRs
  disclosure_lag_days  INTEGER      NULL,               -- filing_date - transaction_date
  asset_name           TEXT         NULL,
  asset_security_id    BIGINT       NULL,               -- → security
  transaction_type     TEXT         NULL,               -- Purchase/Sale (Full)/Sale (Partial)/Exchange/...
  amount_range_low     NUMERIC(20,2) NULL,              -- discretized: $1k-$15k, $15k-$50k, etc.
  amount_range_high    NUMERIC(20,2) NULL,
  source_url           TEXT         NOT NULL,
  source_normalizer    TEXT         NOT NULL            -- 'senatestockwatcher' | 'housestockwatcher' | 'direct'
);
```

### I.11 Convenience views

```sql
-- Insider's most recent disclosed beneficial-ownership snapshot per security
CREATE MATERIALIZED VIEW insider_position_latest AS
SELECT DISTINCT ON (insider_id, security_id)
  insider_id, security_id, transaction_date,
  shares_owned_following AS shares_held,
  direct_indirect
FROM insider_transaction
ORDER BY insider_id, security_id, transaction_date DESC;

-- Issuer-level "% closely held" approximation
CREATE MATERIALIZED VIEW issuer_insider_ownership AS
SELECT entity_id, period_of_report,
       SUM(CASE WHEN direct_indirect='D' THEN shares_owned_following ELSE 0 END) AS direct_shares,
       SUM(CASE WHEN direct_indirect='I' THEN shares_owned_following ELSE 0 END) AS indirect_shares,
       COUNT(DISTINCT insider_id) AS n_insiders
FROM insider_transaction
GROUP BY entity_id, period_of_report;

-- Activist watchlist: all open (non-superseded) 13D filings with purpose-of-transaction
CREATE VIEW activist_watch AS
SELECT bf.issuer_entity_id, bf.event_date, bf.filing_date, bf.purpose_text,
       brp.reporting_person_name, brp.aggregate_beneficially_owned, brp.percent_of_class
FROM blockholder_filing bf
JOIN blockholder_reporting_person brp ON brp.filing_id = bf.filing_id
WHERE bf.schedule_type IN ('13D', '13D/A')
  AND bf.knowledge_to = 'infinity';
```

### I.12 Ingestion pipeline outline

1. **Discover.** Poll EDGAR `data.sec.gov/submissions/CIK*.json` and daily-index files for `form_type IN ('3','4','5','SCHEDULE 13D','SCHEDULE 13G','NPORT-P','144','N-PX', plus '/A' variants)`.
2. **Fetch.** GET primary doc (XML for Forms 3/4/5, N-PORT, 144, N-PX; XML for 13D/G post 2024-12-18, HTML/ASCII for pre).
3. **Parse.**
   - Forms 3/4/5: XSD-validate against Ownership XML Tech Spec v5.x; extract `ownershipDocument` tree.
   - 13D/G post 2024-12-18: XSD-validate against EDGAR Filer Manual Vol II Ch 9.
   - 13D/G pre 2024-12-18: HTML parser + regex extraction; quality flag.
   - N-PORT: parse `<edgarSubmission>` → `<formData>` → `<invstOrSecs>` → repeated `<invstOrSec>`.
   - 144: parse XML; if filing is HTML (pre-mandate), fall through to OCR-quality path.
   - N-PX: parse custom XML schema.
4. **Resolve.**
   - Insider: `rptOwnerCik → insider_id` via CIK index; if no CIK, normalize name + match against existing `insider` rows; queue for manual review otherwise.
   - Issuer: `issuerCik → entity_id`; CUSIP → `security_id` via security_alias (CUSIP non-redistributable, see 13F Part B.3).
   - Fund: `(registrantCik, seriesId) → fund_id`; auto-create if new.
5. **Normalize.**
   - Officer title raw → role_norm via LLM classifier + rule table.
   - Footnote text → entity extraction (10b5-1 plan adoption dates, family-trust names, vesting schedules).
   - 13D Item 4 purpose text → topic classifier (M&A, board seat, dividend, share repurchase, etc.).
6. **Insert.** Bitemporal upsert with `knowledge_from = now()`.
7. **Cross-link.**
   - Form 144 → subsequent Form 4 reconciliation (sliding 90-day window on insider + security).
   - 13D → subsequent N-PX votes by passive holders (engagement-success analytics).
   - Manager-level 13F → fund-level N-PORT (Part D.7).
8. **Publish.** Refresh materialized views; emit Kafka events.

---

## Part J — Cross-cutting open questions / wave-3 gaps

1. **N-PORT monthly-public timing.** The August 2024 rule was delayed to 2027-11-17 / 2028-05-18, and the February 2026 SEC proposal would scale back parts of the 2024 amendments. Wave-3 must monitor the final effective date and whether the monthly-public requirement survives.
2. **13D/G XML schema element naming.** The official root-element names (`<sch13DDocument>` vs `<schedule13DDocument>` etc.) and the precise textual-narrative element names need to be pulled from EDGAR Filer Manual Volume II Chapter 9 — not yet read at the required granularity.
3. **Form 144 XML schema completeness.** Pre-2023 paper/HTML filings cover the majority of historical Form 144 data; post-2023 XML coverage of restricted-stock sales by non-reporting-company affiliates is partial. Are private-company affiliate sales captured anywhere systematic?
4. **Insider attribution edge cases.** "Indirect" ownership via family trusts, LLCs, family offices, foundations is the noisiest part of the corpus. Vendor normalizers do this; ats-eqt's plan is unclear. Wave-3 should propose a footnote-parsing roadmap.
5. **Section 16 "officer" determination.** Whether a given VP is a §16 officer is an issuer-level policy determination, not a public datum. Vendors infer it from proxy-statement appearance + Form 4 history. Open question: should ats-eqt expose `is_section_16_officer` as a *derived* boolean with confidence, or as a label-only feature?
6. **Group filings and §13(d)(3) "wolf packs."** Detecting un-disclosed coordinated activism requires inference; the only structured signal is "Group" checkbox in 13D Item 2(a)/(b). Are there ML approaches worth scoping?
7. **10b5-1 plan multi-quarter reconstruction.** The Form 4 cover-page indicator started 2023-04-01. Pre-2023 plan adoptions were footnote-only; vendors have backfilled plan histories. ats-eqt's pre-2023 10b5-1 coverage is best-effort only.
8. **Form NPX vote-topic taxonomy.** The 2024 amendments introduced categorization fields but in practice issuers tag inconsistently. Build a controlled topic vocabulary?
9. **Schedule TO / 14D-9 XBRL.** Tender-offer filings remain narrative-heavy HTML/PDF. Out of Phase-0 scope for structured ingestion.
10. **Congressional disclosure completeness.** 30/45-day deadlines are routinely missed; some PTRs are filed years late. ats-eqt should plan a compliance-audit table that flags delinquent filers — useful as a feature, not just a data-quality flag.
11. **WhaleWisdom 13D/G coverage start date (~2006) vs FactSet's deeper history.** Is there a free archival source for pre-2006 13D/G filings, or is FactSet/Refinitiv the only option?
12. **CUSIP-licensing exposure on 13D/G.** The same CUSIP-redistribution rule from the 13F file applies. 13D filings are also CUSIP-keyed; ats-eqt should resolve to FIGI at ingest (see `13f_holdings.md` Part B).
13. **EDGAR Next account-management mandate (Sep 2024).** All filers — including §16 reporting persons — must comply with EDGAR Next from a defined date. Does this affect bulk-download or only the filing side?
14. **eMAXX fixed-income ownership.** Bond-side institutional ownership is the underexplored cousin of 13F; LSEG's eMAXX is the canonical source. Out of Phase-0 scope.

---

## Part K — Sources

### SEC primary — Forms 3/4/5
- <https://www.sec.gov/info/edgar/ownershipxmltechspec.htm> — EDGAR Ownership XML Technical Specification v5.1
- <https://www.sec.gov/page/edgar-ownership-xml-tech-spec> — Ownership XML Technical Specification v5.4 draft
- <https://www.sec.gov/info/edgar/ownershipxmltechspec-v4_d.htm> — Earlier draft v4
- <https://www.sec.gov/edgar/searchedgar/ownershipformcodes.html> — Official Ownership Form Codes list (transactionCode enumeration)
- <https://www.sec.gov/files/form4data,0.pdf> — Form 4 (current paper form)
- <https://en.wikipedia.org/wiki/Form_4> — Form 4 overview
- <https://www.secdatabase.com/Articles/tabid/42/ArticleID/10/Form-4-Transaction-Code-Definitions.aspx> — Third-party transactionCode reference

### SEC primary — Schedule 13D/G
- <https://www.sec.gov/newsroom/press-releases/2023-219> — SEC press release on Schedule 13D/G modernization (Oct 2023)
- <https://www.sec.gov/files/rules/final/2023/33-11253.pdf> — Final rule release 33-11253
- <https://www.sec.gov/files/33-11253-fact-sheet.pdf> — Modernization of Beneficial Ownership Reporting fact sheet
- <https://www.law.cornell.edu/cfr/text/17/240.13d-101> — 17 CFR 240.13d-101 (Schedule 13D content)
- <https://www.law.cornell.edu/cfr/text/17/240.13d-102> — 17 CFR 240.13d-102 (Schedule 13G content)
- <https://www.govinfo.gov/content/pkg/CFR-2012-title17-vol3/pdf/CFR-2012-title17-vol3-sec240-13d-102.pdf> — Schedule 13G GovInfo PDF
- <https://www.sec.gov/interps/telephone/cftelinterps_reg13d-13g.pdf> — Regulation 13D/G C&DIs
- <https://www.olshanlaw.com/newsroom/alerts/client-alert-important-reminder-schedules-13d-and-13g-must-be-filed-using-structured-machine-readable-xml-based-language-beginning-december-18-2024> — XML mandate Dec 18 2024
- <https://www.toppanmerrill.com/blog/schedule-13d-and-schedule-13g-key-challenges-and-considerations-for-the-xml-mandate/> — XML mandate technical considerations
- <https://www.skadden.com/insights/publications/2024/02/reminders-amended-beneficial-ownership-rules-effective> — Skadden alert on 2024-02-05 effective date
- <https://www.sidley.com/en/insights/newsupdates/2023/10/sec-shortens-filing-deadlines-for-schedules-13d-g> — Sidley summary of 2023 amendments
- <https://www.thompsoncoburn.com/insights/final-rules-issued-amending-sec-schedules-13d-and-13g-beneficial-ownership-reporting-requirements/> — Thompson Coburn analysis
- <https://corpgov.law.harvard.edu/2023/10/24/sec-adopts-updates-to-schedule-13d-and-13g-reporting/> — Harvard CGF analysis
- <https://www.knowntrends.com/2024/09/reminder-new-schedule-13g-filing-deadlines/> — Q3 2024 13G filing-deadline reminder
- <https://www.troutman.com/insights/sec-adopts-final-rules-amending-and-modernizing-beneficial-ownership-reporting-requirements/> — Troutman analysis
- <https://kpmg.com/us/en/articles/2023/sec-beneficial-ownership-reporting-amendments-reg-alert.html> — KPMG reg-alert
- <https://www.thecorporatecounsel.net/nonmember/promo/01_25/materials.pdf> — "ABCs of Schedule 13D/G" January 2024
- <https://www.mintz.com/sites/default/files/viewpoints/orig/14/2015/02/Memo_-Summary-of-Schedule-13D-and-13G-Filing-Obligations-DOC1.pdf> — Mintz summary memo

### SEC primary — N-PORT
- <https://www.sec.gov/info/edgar/specifications/form-n-port-xml-tech-specs.htm> — EDGAR Form N-PORT XML Technical Specification v1.7
- <https://www.sec.gov/info/edgar/specifications/form-n-port-xml-tech-specs-1.htm> — Form N-PORT XML Technical Spec v1
- <https://www.sec.gov/files/formn-port.pdf> — Form N-PORT (PDF form)
- <https://www.sec.gov/data-research/sec-markets-data/form-n-port-data-sets> — Form N-PORT bulk data sets
- <https://www.sec.gov/newsroom/press-releases/2025-64> — SEC press release: extension of N-PORT effective and compliance dates (Apr 2025)
- <https://www.federalregister.gov/documents/2025/04/22/2025-06861/form-n-port-and-form-n-cen-reporting-guidance-on-open-end-fund-liquidity-risk-management-programs> — Federal Register on delay
- <https://www.sec.gov/files/rules/final/2025/ic-35538.pdf> — IC-35538 Final Rule on N-PORT delay
- <https://www.sec.gov/rules-regulations/2025/04/s7-26-22> — S7-26-22 rulemaking page
- <https://www.federalregister.gov/documents/2024/09/11/2024-19819/form-n-port-and-form-n-cen-reporting-guidance-on-open-end-fund-liquidity-risk-management-programs> — Original 2024 N-PORT amendments Federal Register
- <https://www.sec.gov/newsroom/press-releases/2026-19-sec-proposes-amendments-reduce-burdens-reporting-fund-portfolio-holdings> — February 2026 proposal to scale back 2024 amendments
- <https://www.klgates.com/Fast-Track-to-Fine-Tuned-How-the-SECs-New-Form-N-PORT-Proposed-Amendments-Refine-the-Rules-for-Fund-Reporting-3-2-2026> — K&L Gates analysis of 2026 proposal
- <https://www.sidley.com/en/insights/newsupdates/2026/03/us-sec-proposes-to-scale-back-2024-form-n-port-amendments> — Sidley analysis of 2026 proposal
- <https://www.morganlewis.com/pubs/2026/02/sec-staff-publishes-additional-names-rule-faqs-sec-proposes-n-port-amendments> — Morgan Lewis on 2026 proposal
- <https://sec-api.io/datasets/form-nport> — sec-api.io N-PORT dataset reference
- <https://sec-api.io/docs/n-port-data-api> — sec-api.io N-PORT API docs (field reference)
- <https://www.compsciresources.com/blog/form-n-port-understanding-new-regulations-and-solutions-for-investment-companies> — N-PORT field guide
- <https://www.workiva.com/blog/your-guide-sec-updates-n-port-n-cen-forms> — Workiva guide to N-PORT/N-CEN updates

### SEC primary — Form 144
- <https://www.sec.gov/files/form144.pdf> — Form 144 (PDF)
- <https://www.sec.gov/submit-filings/filer-support-resources/how-do-i-guides/file-form-144-electronically> — File Form 144 Electronically
- <https://www.federalregister.gov/documents/2022/06/10/2022-12253/updating-edgar-filing-requirements-and-form-144-filings> — Form 144 electronic-filing rule (2022)
- <https://www.federalregister.gov/documents/2021/01/19/2020-28790/rule-144-holding-period-and-form-144-filings> — 2021 Rule 144 holding-period amendments
- <https://www.sec.gov/resources-small-businesses/small-business-compliance-guides/updating-edgar-filing-requirements-form-144-filings> — Small business compliance guide
- <https://www.toppanmerrill.com/blog/clarifying-form-144-filing-requirements/> — Toppan Merrill clarification
- <https://www.sec.gov/reports/rule-144-selling-restricted-control-securities> — Rule 144 overview
- <https://en.wikipedia.org/wiki/Form_144> — Form 144 overview

### SEC primary — Form N-PX
- <https://www.sec.gov/newsroom/press-releases/2022-198> — SEC press release on N-PX modernization (Nov 2022)
- <https://www.sec.gov/investment/enhanced-reporting-proxy-votes> — Enhanced Reporting of Proxy Votes guidance
- <https://www.federalregister.gov/documents/2022/12/22/2022-24292/enhanced-reporting-of-proxy-votes-by-registered-management-investment-companies-reporting-of> — Federal Register N-PX final rule
- <https://www.toppanmerrill.com/blog/form-n-px-to-require-mutual-funds/> — Toppan Merrill on N-PX XML mandate
- <https://corpgov.law.harvard.edu/2024/02/23/additional-proxy-vote-disclosure-is-coming-for-the-2024-proxy-season/> — Harvard CGF 2024 proxy season
- <https://www.goodwinlaw.com/en/insights/publications/2024/05/alerts-technology-new-proxy-voting-reporting-requirements> — Goodwin N-PX alert
- <https://corpgov.law.harvard.edu/2022/12/24/enhanced-proxy-voting-disclosure-requirements-for-investment-funds/> — Harvard CGF Dec 2022
- <https://www.dfinsolutions.com/knowledge-hub/thought-leadership/knowledge-resources/sec-form-n-px> — DFin N-PX overview
- <https://www.sec.gov/search-filings/mutual-funds-search/search-mutual-fund-proxy-voting-records> — Search Mutual Fund Proxy Voting Records

### SEC primary — Rule 10b5-1
- <https://www.sec.gov/newsroom/press-releases/2022-222> — SEC press release on 10b5-1 amendments (Dec 14 2022)
- <https://www.sec.gov/files/33-11138-fact-sheet.pdf> — 10b5-1 Insider Trading Arrangements fact sheet
- <https://www.skadden.com/insights/publications/2022/12/sec-amends-rules-for-rule-10b51-trading-plans-and-adds-new-disclosure-requirement> — Skadden on amendments
- <https://www.cooley.com/news/insight/2022/2022-12-21-filling-the-gaps-sec-adopts-final-rules-on-10b5-1-trading-plans-and-related-disclosures> — Cooley analysis
- <https://www.morganlewis.com/pubs/2022/12/sec-adopts-significant-changes-to-rule-10b5-1-affecting-trading-by-insiders> — Morgan Lewis analysis
- <https://www.bclplaw.com/en-US/events-insights-news/sec-adopts-big-changes-to-rule-10b5-1-plan-requirements-reaffirms-warning-about-insider-gifting.html> — BCLP analysis
- <https://www.bassberrysecuritieslawexchange.com/sec-amendments-to-rule-10b5-1-trading-plans/> — Bass Berry FAQs
- <https://www.lw.com/en/insights/2023/01/Amended-Rule-10b5-1-and-New-Insider-Trading-Disclosure-Frequently-Asked-Questions> — Latham & Watkins FAQs
- <https://www.troutman.com/insights/sec-adopts-final-rule-amendments-for-rule-10b5-1-trading-plans-and-creates-new-disclosure-requirements/> — Troutman analysis
- <https://www.morganstanley.com/atwork/employees/learning-center/articles/10B5-1-changes-to-know> — Morgan Stanley plain-language summary

### SEC primary — EDGAR access
- <https://www.sec.gov/search-filings/edgar-application-programming-interfaces> — EDGAR APIs
- <https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data> — Accessing EDGAR data
- <https://www.sec.gov/submit-filings/technical-specifications> — All EDGAR technical specifications
- <https://www.sec.gov/submit-filings/edgar-news-announcements/edgar-release-23-4> — EDGAR Release 23.4
- <https://www.sec.gov/submit-filings/edgar-news-announcements/edgar-release-24-4> — EDGAR Release 24.4
- <https://www.sec.gov/files/edgar/filermanual/archive/efmvol2-v72.pdf> — EDGAR Filer Manual Volume II Dec 2024

### Vendor — FactSet, S&P, Bloomberg, LSEG ownership
- <https://www.factset.com/marketplace/catalog/product/factset-ownership> — FactSet Ownership product
- <https://insight.factset.com/resources/at-a-glance-factset-ownership-standard-datafeed> — FactSet Ownership at-a-glance
- <https://www.factset.com/marketplace/catalog/product/factset-ownership-api> — FactSet Ownership API
- <https://developer.factset.com/api-catalog/factset-ownership-api> — FactSet Ownership API developer docs
- <https://developer.factset.com/api-catalog/factset-ownership-report-builder-api> — FactSet Ownership Report Builder API
- <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro> — S&P Capital IQ Pro
- <https://www.bloomberg.com/professional/dataset/united-states-ownership-filings/> — Bloomberg US Ownership Filings dataset
- <https://data.bloomberglp.com/professional/sites/10/Security-Ownership-fact-sheet.pdf> — Bloomberg Security Ownership fact sheet
- <https://libguides.nypl.org/c.php?g=1084166&p=8024705> — NYPL Bloomberg Holders guide (HDS function)
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/company-ownership-information-profiles> — LSEG Ownership
- <https://developers.lseg.com/en/api-catalog/refinitiv-data-platform/ownership-API> — LSEG Ownership API
- <https://wrds-www.wharton.upenn.edu/documents/1414/WRDS_Ownership_Data.pdf> — WRDS Ownership Data overview (June 2020)
- <https://www.library.hbs.edu/services/help-center/insider-data-from-thomson-reuters-ownership-data> — HBS Baker on Thomson Reuters Insider data
- <https://wrds-www.wharton.upenn.edu/pages/analytics/sec-analytics-suite-wrds/wrds-insiders/> — WRDS Insiders analytics
- <https://guides.library.ualberta.ca/az/thomson-reuters-insiders-data-wrds> — University of Alberta guide
- <https://kenanflaglerresearchtools.web.unc.edu/research-resource/insider-filings-data/> — UNC Kenan-Flagler Insider Filings data guide
- <https://guides.library.ualberta.ca/finance/ownership> — Alberta Ownership & Insider Trading guide

### Vendor — Mid-tier and free
- <https://whalewisdom.com/info/subscription_info> — WhaleWisdom subscription pricing
- <https://whalewisdom.com/help/api> — WhaleWisdom API docs
- <https://whalewisdom.com/schedule13d> — WhaleWisdom 13D/G activist tracker
- <https://whalewisdom.com/shell/api_help> — WhaleWisdom API auth docs
- <https://whalewisdom.com/info/features> — WhaleWisdom features
- <https://whalewisdom.com/info/excel_add_in> — WhaleWisdom Excel Add-in
- <https://www.dakota.com/resources/blog/the-top-13f-databases-for-2023> — Dakota's "Top 13F Databases for 2023"
- <https://www.dakota.com/resources/blog/whalewisdom-opportunity-hunter-sec-api-which-is-right-for-you> — Dakota comparison
- <https://api.quiverquant.com/docs/> — Quiver Quantitative API
- <https://www.quiverquant.com/> — Quiver Quantitative home
- <https://www.quiverquant.com/congresstrading/> — Quiver Congress Trading
- <https://github.com/Quiver-Quantitative/python-api> — Quiver Python SDK
- <http://openinsider.com/> — OpenInsider home
- <http://openinsider.com/insider-purchases> — OpenInsider purchases
- <http://openinsider.com/latest-cluster-buys> — OpenInsider cluster buys
- <http://openinsider.com/latest-insider-purchases-25k> — OpenInsider $25k+ purchases
- <https://www.secdatabase.com/Articles/tabid/42/ArticleID/10/Form-4-Transaction-Code-Definitions.aspx> — secdatabase.com transaction code definitions
- <https://blog.form345.com/form-4-transaction-codes-decoded> — form345 blog: transaction codes decoded
- <https://www.insidermole.com/article/what-do-the-transaction-codes-mean> — InsiderMole transaction code reference
- <https://www.2iqresearch.com/blog/what-is-sec-form-4-and-how-do-you-read-form-4-filings-2022-03-11> — 2iQ Research Form 4 primer
- <https://stocktrot.com/learn/form4/transaction-codes> — StockTrot transaction code reference

### STOCK Act and congressional disclosure
- <https://www.congress.gov/bill/112th-congress/senate-bill/2038> — STOCK Act of 2012 (S.2038)
- <https://en.wikipedia.org/wiki/STOCK_Act> — STOCK Act overview
- <https://disclosures-clerk.house.gov/FinancialDisclosure> — House Clerk Financial Disclosure portal
- <https://www.ethics.senate.gov/public/index.cfm/financialdisclosure> — Senate Ethics Financial Disclosure portal
- <https://www.congress.gov/crs_external_products/R/HTML/R47818.html> — CRS Laws Governing Financial Disclosure
- <https://www.congress.gov/crs_external_products/TE/HTML/TE10073.html> — CRS Stock Trading in Congress
- <https://www.govinfo.gov/content/pkg/CRPT-112srpt244/html/CRPT-112srpt244.htm> — Senate Report 112-244 (STOCK Act)
- <https://senatestockwatcher.com/> — Senate Stock Watcher (normalized data)
- <https://housestockwatcher.com/> — House Stock Watcher (normalized data)
- <https://www.capitoltrades.com/> — Capitol Trades aggregator

### Filing/parsing reference and tools
- <https://github.com/dgunning/edgartools> — edgartools Python library
- <https://github.com/mccgr/edgar> — Academic Form 3/4/5 ETL
- <https://github.com/mccgr/edgar/issues/42> — schemaVersion X0101/null quirk
- <https://www.sec.gov/Archives/edgar/data/1067983/000095017024114125/xslF345X05/ownership.xml> — Example Form 4 XML (Berkshire Hathaway, Buffett)
- <https://elsaifym.github.io/EDGAR-Parsing/> — Open EDGAR parsing reference

---

**Confirm:**

- File path: `c:/Users/natha/OneDrive/Desktop/C/ats/ats-eqt/research/datasets/insider_ownership.md`
- Section count: **11 top-level parts** (0 Executive summary; A Vendor stack matrix; B Forms 3/4/5; C Schedule 13D/G; D Form N-PORT; E Form 144; F Form N-PX; G Tender offers; H Vendor deep dives; I Recommended ats-eqt schema; J Wave-3 open questions; K Sources), with 50+ sub-sections and 11 SQL tables / views.
