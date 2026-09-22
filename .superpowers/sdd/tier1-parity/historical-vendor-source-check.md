# SpiderRock historical reference source check

Research date: 2026-09-20. Scope: official public documentation and the existing local readiness reports. No data files were opened, archives downloaded, credentials used, contacts made, runtime/tests/database work performed, or production source changed. This report is the only authored file. Current documentation identified itself as version 8.6.8.3; an older version is explicitly identified below.

**Recommendation: request the local path to an already-held US `TickerDefinitionHist` historical export, if one exists, then inspect its actual coverage before designing an adapter. It is a credible source for dated vendor-instrument membership evidence, but does not currently close AR5 or the historical issuer-to-security link.** No purchase, account setup, vendor contact, or API query is proposed.

The local baseline remains 34,251 published price identities spanning 2012-03-26 through 2026-09-18, one directory snapshot dated 2026-09-20, and no measured historical listings with usable exchange/MIC evidence. Those are controller measurements, not new measurements from this research. The controller's refreshed Downloads filename inventory found only the old ticker-history ZIP and updated `TickerHistory3.parquet`, with no matching companion reference file. See [activation status](../../../atx-db/docs/TIER1_ACTIVATION_STATUS.md), [AR5](activation-readiness-audit.md), and the distinct [SEC bulk research](historical-listing-sec-bulk-research.md).

## Exact candidate and documented limits

The [TickerDefinitionHist dictionary](https://docs.spiderrockconnect.com/docs/HistoricalData/Data%20Dictionaries/TickerDefinitionHist/) documents dated reference observations, US history starting **2010-01-04**, and US delivery at **22:00 CT on T+0**. These are publisher statements, not verified archive contents.

| Purpose | Exact fields |
|---|---|
| Observation/identity | `tradingDate`, `securityID`, `ticker_at`, `ticker_ts`, `ticker_tk` |
| Venue | `primaryExch`, `mic`, `micSeg`, `Exchange` |
| Classification | `symbolType`, `issueClass`, `name`, `country` |
| Other identifiers | `isin`, `cik`, `altID` |

The same dictionary describes v7 backfill: `Mic` becomes `mic`; `EQS` becomes `Equity`; `DR` becomes `ADR`; `PRF`/`PFS` become `PreferenceShare`/`PreferredSec`; ETF remains ETF. Blank, ETC, UNT and CVR become `None`. `primaryExch` is null because its old value was a composite-source token, USCOMP. `cik`, `issueClass`, `altID`, `micSeg`, `symbol` and `Exchange`, among others, have no v7 source. Old timestamps and Bloomberg identifiers are dropped. Backfill excludes OTC and the `ForeignIssue` category. The schema has no listing-start/end, delisting-status, or publication-time field. FIGI appears in descriptive prose but has no listed schema column. [Dictionary and migration notes](https://docs.spiderrockconnect.com/docs/HistoricalData/Data%20Dictionaries/TickerDefinitionHist/).

**Classification assessment:** the [SymbolType enum](https://docs.spiderrockconnect.com/docs/MessageSchemas/Schema/Enums/SymbolType/) separately names `Equity`, `ADR`, `ETF`, `PreferredSec` and `PreferenceShare`, alongside other instrument categories. It does **not** expressly define `Equity` as common stock only or supply a `CommonStock` value. Thus it supports distinguishing those vendor categories, but a claim of common-equity eligibility needs a documented interpretation and actual category/conflict checks. `ticker_at=EQT` is not sufficient by itself. US listing and issuer domicile are different questions; foreign incorporation must not automatically exclude a US-listed common share or be mistaken for an ADR.

## Joining the existing vendor IDs

[TickerHistory3](https://docs.spiderrockconnect.com/docs/HistoricalData/Data%20Dictionaries/TickerHistory3/) identifies its `securityID` as a SpiderRock security ID, pairs it with `tradingDate` and `ticker_tk`, and defines `todayTicker` as the symbol at the latest trading date. Its schema is documented as unchanged from v7. This supports investigating a same-provider, same-date ID association. It does not make `todayTicker` a historical identity key.

The official [TickerAnalytics schema, version 8.6.3.1](https://docs.spiderrockconnect.com/docs/8.6.3.1/MessageSchemas/Schema/Topics/market-statistics/TickerAnalytics/) describes `securityID` continuity across name changes and corporate actions as **best effort**. No permanent, never-reused, globally unique share-class guarantee or formal cross-product ID contract was found in the inspected documentation. This older related schema is a caution about identity semantics, not a substitute for the companion's actual data.

Recommended validation, once a real file is available:

1. Associate records initially by the **existing native vendor ID and matching historical date**, retaining the full source ticker key and provider/version. Preserve the existing warehouse identities; do not create a new ticker-based merge.
2. Measure zero/missing IDs, multiplicity per ID/date, competing symbols/classes/ISINs, and overlap with price IDs by year. Multiple venue rows may describe one instrument; multiple incompatible instruments must remain unresolved. Matching an ID is evidence to check, not permission to ignore conflicts.
3. Preserve raw venue/type values and classify only observations with usable, consistent evidence. A populated MIC must identify an eligible listing venue under the project's exchange policy; a composite token or trade execution venue cannot substitute. Missing evidence remains unknown.
4. Treat first/last observations and missing days as observations, not inferred IPO/delisting dates. Do not stretch intervals across unexplained gaps, copy present-day classifications backward, or merge renamed/reorganized share classes by symbol similarity.

These are proposed acceptance requirements, not performed checks. Even a clean vendor-ID match would establish a vendor-instrument association only. Issuer fundamentals still require a dated, class-safe link to the SEC identity spine; neither a CIK match alone nor a matching company name establishes that link. The historical null issuer fields identified above prevent this product alone from supplying a general historical CIK bridge.

## Bulk availability, retention and point-in-time use

[SpiderRock's S3 documentation](https://docs.spiderrockconnect.com/docs/HistoricalData/parquet_files/) says historical datasets are delivered as Parquet with Hive-style `date_p` partitions; US partition dates follow Central Time and timestamp values use UTC. Access uses onboarding credentials or a provided IAM role. It does not publish a public anonymous download or an exact `TickerDefinitionHist` object basename/all-history filename. Therefore **`TickerDefinitionHist` is the verified product name; `TickerDefinitionHist.parquet` is not a verified promised single-file deliverable**. Use the actual export or partition manifest supplied through the user's existing route rather than inventing an S3 key.

[Snowflake access](https://docs.spiderrockconnect.com/docs/HistoricalData/snowflake/) is documented through client reader accounts for subscribed datasets. The inspected page does not establish this user's entitlement, an exact table name for the candidate, or free access. No account/query is necessary to ask for an existing local export.

[The release process](https://docs.spiderrockconnect.com/docs/HistoricalData/release_process/) exposes canonical `v8/` files and corrected `v8_latest/` files. A correction stable for 30 days is promoted on a weekly cycle and replaces the canonical file. This is a correction lifecycle, **not** a 30-day historical-retention limit. No guarantee of indefinite retention, old file-version recovery, complete daily partitions, or original publication vintages was found. Capture delivery/source version, retrieval time, manifest and hashes if a file is later supplied; a newly downloaded corrected history must not be described as the original bytes available in 2012.

The [historical-products overview](https://docs.spiderrockconnect.com/docs/HistoricalData/) advertises point-in-time records and inclusion of delisted stocks in historical stock datasets. This is helpful product intent, but does not measure reference-table completeness for the project's instruments. Its bundle summary also lists equity reference history from January 2014, while the specific candidate dictionary states the earlier date above. Treat the exact export's date inventory and entitlement as authoritative for what is actually available here.

Availability implication: the documented reference delivery occurs after the project's same-day 22:00 UTC decision cutoff. A historical observation date must therefore remain separate from its usable-at time. Do not equate the two 22:00 clocks or backdate receipt to satisfy the gate. Current delivery schedules do not prove old delivery times; any retrospective convention must remain explicitly modeled and cannot certify original-vintage availability. Carry `available_at = max(inputs)` through decisions.

## Alternatives considered and smallest useful request

`StockCloseMarkHist` supplies historical price marks and vendor IDs, but its [dictionary](https://docs.spiderrockconnect.com/docs/HistoricalData/Data%20Dictionaries/StockCloseMarkHist/) does not supply the required listing/type classification. Its prior-ticker fields are reserved. `ReturnFactorsHist` supplies dated vendor IDs and return adjustments; its [dictionary](https://docs.spiderrockconnect.com/docs/HistoricalData/Data%20Dictionaries/ReturnFactorsHist/) likewise supplies no listing/type history. Neither should be requested merely to close AR5. A live/current `TickerDefinition` snapshot also cannot establish older observations.

Suggested request for the controller to use only if the source is not already available locally:

> Provide the local path to an existing US TickerDefinitionHist historical Parquet export from the same vendor delivery as your TickerHistory3 data, ideally covering 2012-03-26 through 2026-09-18, with its original dated partitions or manifest. Please retain its original columns and any delivery/version notes. If only a smaller existing sample is available, its path is enough to assess the source before requesting more.

This is a file-location request, not a request for credentials, a subscription, a purchase or vendor correspondence. If the user has no export, record the unmet source prerequisite and continue independent warehouse work.

**AR5 disposition:** still open. This research identifies a plausible direct vendor companion, more specific than filing-level enrichment, but establishes no populated eligible cohort. Dated venue/type observations could support membership for measured, unambiguous matched instruments after source/availability checks. Whole-period membership, excluded or missing instruments, historical SEC linkage, terminal-event identity and continuous listing status remain unproved. No adapter or claimed coverage increase should precede the actual source inspection.
