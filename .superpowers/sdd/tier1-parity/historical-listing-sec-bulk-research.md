# SEC bulk filing evidence for historical listings

Research date: 2026-09-20. Scope: SEC Financial Statement and Notes (FSN) archives and closely related official SEC bulk filing products. Primary-source documentation, published example outputs, and directory listings only; no archives downloaded, implementation executed, database changed, or contacts made.

**Decision: preserve FSN as a viable bulk evidence source; size any adapter only after the core warehouse measurements establish the needed scope.** There is verified evidence that FSN TXT data includes actual symbol/exchange/class-title facts. Complete coverage back to 2012, continuous listing intervals, and identification of SpiderRock vendor share classes remain unproved. A future current-month pilot could enrich observed filings without resolving the full survivorship gap.

| Question | Assessment |
|---|---|
| Does an official bulk archive carry actual nonnumeric listing facts? | **Yes.** SEC's saved archive-reading example contains all three target facts. |
| Are archives available covering 2012 onward? | **Yes.** Archive coverage does not imply field coverage. |
| Can this certify all US-equity symbols, exchanges, and instrument types since 2012? | **No on present evidence.** Earlier field coverage is unknown; tagging changed substantially in 2019–2021. |
| Can contexts distinguish multiple classes/exchanges within a filing? | **Yes in the schema and tagging rules.** Extraction must preserve those distinctions and reject unresolved combinations. |
| Does one filing establish vendor-ID identity or continuous listing dates? | **No.** These require separate evidence. |

## Verified archive contents

The SEC-maintained [narrative-facts notebook](https://raw.githubusercontent.com/sec-gov/python-for-dera-financial-datasets/main/examples/5_Find_Narrative_Text_Facts.ipynb) reads `sub.tsv` and `txt.tsv`, and its saved `textFacts` output includes these three rows:

| Shared accession | Tag | Value |
|---|---|---|
| `0002004032-24-000003` | `Security12bTitle` | `Common Stock, par value $0.01 per share` |
| same | `SecurityExchangeName` | `NYSE` |
| same | `TradingSymbol` | `MOS` |

All three show `version=dei/2023`, `ddate=20240229`, `context=c-1`, `dimh=0x00000000`, and `dimn=0`. This is actual saved data output, rather than a list of possible taxonomy tags. The notebook labels its example February 2024, while its current input path says `2024q1_notes`; therefore it verifies inclusion, not the current ZIP's complete row count or exact monthly provenance. Its saved values also show `iprx=0` and `datp=8.0`: preserve raw fields and verify contemporary semantics before relying on old dictionary constraints. [SEC example source](https://raw.githubusercontent.com/sec-gov/python-for-dera-financial-datasets/main/examples/5_Find_Narrative_Text_Facts.ipynb).

No downloaded archive was independently inspected in this research. No pre-2019 target-fact sample or coverage percentage was established.

## Fields and joins

Relevant documented fields are below. `adsh` links facts to submissions; `dimh` links dimensional metadata. [FSN dictionary, sections 2–5.5](https://www.sec.gov/files/aqfsn_1.pdf).

| Table | Exact useful fields | Meaning |
|---|---|---|
| SUB | `adsh`, `cik`, `name`, `form`, `filed`, `accepted`, `period`, `fy`, `fp`, `prevrpt`, `instance`, `nciks`, `aciks` | Accession, registrant, filing identity/timing, reporting period, amendment indicator, source filename, co-registrants. |
| TXT | `adsh`, `tag`, `version`, `ddate`, `qtrs`, `iprx`, `lang`, `dcml`, `durp`, `datp`, `dimh`, `dimn`, `coreg`, `escaped`, `srclen`, `txtlen`, `footnote`, `footlen`, `context`, `value` | Nonnumeric facts, source context, dimensions, text, and processing metadata. |
| DIM | `dimh`, `segments`, `segt` | Flattened axis/member pairs and truncation flag. |
| TAG | `tag`, `version`, `custom`, `datatype`, `tlabel`, `doc` | Taxonomy definition information. |

TXT dates/durations are rounded; `context` retains the source context reference. DIM removes namespaces and shortens axis/member names. **TAG also contains unused standard tags**, so finding `TradingSymbol` there does not establish a reported fact. `instance` often resembles a ticker but is a filename, not an authoritative mapping. `prevrpt` reflects amendment status at the archive cutoff, potentially later than the original filing. The documented scope starts 2009-04-15; 2009 Q1 is a header-only placeholder. Forms 485BPOS, 497, and specified SDR variants are excluded. [FSN dictionary](https://www.sec.gov/files/aqfsn_1.pdf).

## Coverage change around 2019

The SEC's [2019 DEI release notes](https://www.sec.gov/xbrl/site/doc/releasenotes-2019-dei-draft.pdf) identify `Security12bTitle`, `Security12gTitle`, `SecurityExchangeName`, and `NoTradingSymbolFlag` among new cover-page elements. They describe `TradingSymbol` as an existing element. The document is explicitly labeled DRAFT; it corroborates the taxonomy transition, not historical archive population. Do not infer that TradingSymbol began in 2019, or that its earlier existence means US-equity filings consistently used it.

The official [cover-page tagging phase-in](https://www.sec.gov/resources-small-businesses/small-business-compliance-guides/fast-act-modernization-simplification-regulation-s-k) applies to reports for fiscal periods ending on or after:

| Filer category | Date |
|---|---|
| Large accelerated filers using US GAAP | 2019-06-15 |
| Accelerated filers using US GAAP | 2020-06-15 |
| Other affected filers | 2021-06-15 |

For Form 10-Q filers, other forms enter the requirement after the first applicable 10-Q. The SEC also explains that voluntary early Inline XBRL use did not itself trigger mandatory cover-page tagging. [SEC Interactive Data interpretations, questions 101.01–101.02](https://www.sec.gov/rules-regulations/staff-guidance/corporation-finance-interpretations-cfis/interactive-data).

**Inference:** expect materially different evidence density before and after that rollout. Treat 2012–2018 ticker-only, custom-tag, or narrative evidence as unmeasured possibilities. Do not backfill exchange/type from a later filing, present-day directory, or an old taxonomy definition. Exact earliest occurrence and completeness require a bounded archive scan.

## Classes, exchanges, and identity limits

The [August 2026 SEC XBRL Guide, section 3.2.4](https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide-2026-08-14.pdf) distinguishes single-class contexts from `StatementClassOfStockAxis` and `ClassesOfShareCapitalAxis` contexts. `EntityListingsExchangeAxis` distinguishes multiple exchange listings. Class titles and exchanges are paired within the same context. Registered securities include more than common equity. ADRs can require dimensional treatment even for a single registered security. Exchange values are EDGAR acronyms such as NYSE, not a promised MIC field; non-national exchanges are outside that tag's stated scope. The guide does not require a class-title fact in every instance, and its validation conditions therefore do not guarantee a complete tuple.

Recommended conservative extraction:

1. Group candidate facts by **accession plus source context**, retaining taxonomy version, dimensions, period fields, language, and co-registrant information. Never cross-join all symbols, titles, and exchanges within one accession.
2. Emit a tuple only when values are consistent within that group. Keep duplicate conflicts, absent fields, truncated dimensions, and unresolved entities in an explicit unknown state. Equivalent-but-differently-named contexts need separate validation; do not merge them merely because dates match.
3. Preserve title, exchange code, symbol spelling, and class members as raw evidence. Any common/preferred/ADR/unit/warrant/debt categorization is a versioned interpretation, not the source's universal instrument-type code.
4. Associate with a vendor instrument only after checking time overlap and class identity. Symbol reuse, punctuation normalization, corporate reorganizations, and multiple securities per CIK defeat an unrestricted ticker join. A matching CIK identifies the registrant, not a unique share class.

These are implementation recommendations. Neither absence of a later filing nor the first/last observed tuple proves a listing start, delisting date, or uninterrupted interval. Class-member names/context IDs are local filing constructs, not permanent instrument identifiers.

## Point-in-time handling

Keep reporting period, accepted timestamp, filed date, archive retrieval time, and any assigned usable-at timestamp separate. A financial period end cannot serve as the date the market knew the filing. The SEC states that some submissions after 5:30 p.m. ET are disseminated the next business day; `accepted` alone is therefore not universal proof of immediate public availability. Its filing indexes also incorporate later corrections, while some older daily indexes retain removed filings. [SEC access and dissemination documentation](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data).

SUB `accepted` is documented as `yyyy-mm-dd hh:mm:ss`, with no offset/timezone field; `filed` is a date. The dictionary mentions an EST filing cutoff, but does not explicitly specify the stored timestamp's timezone or DST treatment. That encoding is **unknown from the inspected dictionary**; preserve the raw value pending validation. [FSN dictionary, section 5.1](https://www.sec.gov/files/aqfsn_1.pdf).

Recommended policy: use a documented conservative filing-availability rule, retain original accessions and amendments independently, and avoid retrospectively removing the original because a later archive marks it amended. Record the rule version and uncertainty. Do not extend a newly observed classification backward into price history. A carry-forward classification, if used, must be explicitly identified as an assumption and cannot certify continuous listing status.

The SEC refreshed earlier FSN archives and corrected 2010–2013 processing in 2024. Thus today's downloadable archive is a reconstruction of filed disclosures, not necessarily the bytes available from this bulk product at a past date. [FSN release notes on the download page](https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets).

## Bulk footprint and access

The [official FSN download page](https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets) currently lists January 2009–August 2026. Updates became monthly in November 2020; older months are consolidated into quarters. The currently listed 2012-onward set contains **54 quarterly archives through 2025 Q2 plus 14 monthly archives from July 2025 through August 2026**. Summing the page's displayed sizes gives **24,698.78 MB**, approximately 24.70 GB using the publisher's MB labels; expanded size is unknown.

| Archive | Published compressed size |
|---|---:|
| 2012 Q1 / Q2 / Q3 / Q4 | 424.02 / 301.13 / 440.09 / 487.80 MB |
| 2021 Q1 | 810.44 MB |
| 2026 January | 41.06 MB |
| 2026 July | 102.91 MB |
| 2026 August, latest listed | 298.24 MB |

The latest month is a reasonable pilot candidate; it is not an all-company snapshot. Select its actual download link from the official page because paths and consolidation can change. With roughly 123 GB free reported by the root task, do not presume that every archive plus expanded TSVs and new warehouse tables fits. Read ZIP central-directory sizes first; stream only SUB/TXT/DIM and retain compact evidence, using an explicit disk budget. These are proposed controls, not measured extraction results.

Public EDGAR access is free. SEC asks for an identifying User-Agent and limits requests to 10/second; bulk retrieval should stay below that shared limit. [SEC fair-access documentation](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data). The user's contact restriction remains binding: never send their email; the only permitted dummy contact here is `atx-research@example.com`. No contact or account is needed for this research.

## Other official SEC bulk routes considered

**Submissions/companyfacts ZIPs:** SEC republishes these nightly, and the submissions product supplies filing history. Its company-level exchange/ticker metadata is not documented as a dated per-security history. The XBRL API aggregates standard-taxonomy, entity-wide facts, which is insufficient assurance for dimensioned class-level nonnumeric coverage. Therefore neither ZIP is established here as a replacement for FSN TXT evidence or the missing historical security master. [SEC API and bulk documentation](https://www.sec.gov/search-filings/edgar-application-programming-interfaces).

**Feed/Oldloads:** SEC documents daily raw filing archives, including submission headers, so there is an official bulk route to older filing text without per-company fetching. [SEC archive description](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data). The [2012 Feed directory](https://www.sec.gov/Archives/edgar/Feed/2012/QTR1/) actually lists daily TAR/GZIPs: January 3 is 141,245 KB and February 29 is 990,078 KB. The [2012 Oldloads directory](https://www.sec.gov/Archives/edgar/Oldloads/2012/QTR1/) lists analogous daily GZIPs. These are much larger than a listing table and require extracting evidence from filings, with unmeasured historical completeness and parsing accuracy. The two inspected listings even differ for March 16, 2012, reinforcing the need for manifest checks. No raw daily archives were downloaded, and this task does not propose an unbounded text-ingestion project.

## Actionable next step

After core warehouse measurements establish the need, the future adapter should operate as **filing-observation enrichment**:

1. Inspect the selected ZIP's declared compressed/expanded sizes, then stream TXT for the requested standard nonnumeric tags. Keep the raw `adsh`, `context`, `dimh`, `coreg`, tag/version, value, and date/processing fields.
2. Stream only the needed DIM and SUB members, retaining just DIM keys and accessions referenced by the selected TXT rows. SUB must supply registrant CIK, co-registrants, form, `accepted`, `filed`, and reporting period; keep availability uncertainty explicit.
3. Join on accession/context, preserving dimensions and entity scope. No ticker-only cross-products, guessed class identity, or classification/universe flips caused by missing fields. Retain contradictions as evidence requiring resolution.
4. Measure actual bytes, tuple coverage, multiple classes, conflicts, ambiguous vendor matches, and timing discrepancies before broader ingestion. A month of filings cannot be labeled a complete current universe.

This brief authorizes no additional dispatch or download in this research task.

A separate bounded historical audit would compare actual TXT facts in one 2012 quarter, a pre-rollout quarter, rollout-era quarters, and a post-2021 quarter. Presence of tags in TAG is not a pass condition. Until those measurements and independent identity/listing evidence exist, keep the full 2012-onward PIT/survivorship certification blocked. No commercial-source survey or per-filing scraping is needed to reach that conclusion.
