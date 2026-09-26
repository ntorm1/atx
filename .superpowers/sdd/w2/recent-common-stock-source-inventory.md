# Recent common-stock/listing evidence inventory

Bounded source/metadata inspection, 2026-09-26. No warehouse connection, price or
return statistics, filing/archive payload scan, external request, paid API,
download, build or production change. Graph tools were unavailable; targeted
source search was used. Source snapshots: pool3 `7a69b22a` and newer warehouse
branch `feat/tier1-parity` at `a5c8386026942729c0733db0260744bd66aaeeb4`, read through
Git objects without merging. Their implementations differ materially.

**No inspected local evidence establishes thousands of verified common-stock
lines for the 2020–2024 strategy cohort.** Keep the current role's
`common_stock_verified=false`. The strongest direct vendor companion is identified
but not found in the bounded local filename inventory. Existing SEC evidence can
enrich a dated subset; coverage and vendor security-line identity remain separate
requirements.

| Route | Exact useful fields / implementation | Local availability and qualification |
|---|---|---|
| SpiderRock `TickerDefinitionHist` | `tradingDate`, `securityID`, `ticker_at/ticker_ts/ticker_tk`; `symbolType`, `issueClass`, `name`, `country`; `primaryExch`, `mic`, `micSeg`, `Exchange`; `isin`, `cik`, `altID`. | Retained source research identifies this product; no companion found in the top-level Downloads/cache filename check. Same-provider ID/date association is preferable to a current-ticker join but still needs conflict checks. `Equity` is not documented there as a universal common-stock-only code. Original vintage/publication and missing historic fields remain qualifications. |
| Nasdaq retained directories | `symbol_directory.py`: `Symbol` or `ACT Symbol`, `Security Name`, `ETF`, `Test Issue`, `Exchange`/`Market Category`, CQS/Nasdaq symbols; file creation trailer. Adds/deletes source additionally has `NASDAQ Action`, `Effective Date`, `Primary Listing Market`. | `C:/atx/atx-db/data/cache/nasdaqlisted.txt` (349,031 bytes) and `otherlisted.txt` (541,678 bytes) exist. Header/trailer-only inspection finds **both generated 2026-09-18 21:31**. These snapshots cannot qualify 2020–2024 membership. No historical snapshot archive was established by this pass. ETF=false alone does not imply common stock. |
| Retained SEC filing-class evidence | Newer `historical_identity_sources.py`: 8-A12B/8-A12G, CERT variants, 25/25-NSE and periodic cover-page class/title/symbol/venue evidence. `historical_identity.py` records namespaced native key, fact kind, security/class/CIK, validity interval, source locator/hash/revision, `source_published_at`, `observed_at`, `available_at`, evidence and availability status. | Existing cache `C:/atx/atx-db/data/cache/C9-sec-identity-documents/`. Only its small fetch ledger was inspected: 381 records, 357 HTTP200, 22 HTTP503, 2 HTTP404. These are fetch outcomes, **not** unique companies, successful classifications or cohort coverage. Newer source explicitly labels vendor historical-symbol line links reconstructed; document-level verified facts do not automatically qualify that link. |
| SEC Financial Statement and Notes (FSN) text facts | `TXT`: `adsh`, `tag`, `value`, `context`, `dimh`, `coreg`; target tags `TradingSymbol`, `Security12bTitle`/`Security12gTitle`, `SecurityExchangeName`. Join `SUB` accession/CIK/accepted/filed and `DIM` within the same source context/class. | Two retained feasibility reports establish this source route, not local population coverage. Cover tagging rollout in 2019–2021 makes recent years promising. No FSN notes payload was read or acquired. Compact FSDS `sub.txt` SIC/issuer rows alone cannot supply security type. A filing tuple is not a continuous listing interval or vendor-ID proof. |
| OpenFIGI | `openfigi_signals.py`: `/v3/mapping`; `figi`, `compositeFIGI`, `shareClassFIGI`, `securityType`, `securityType2`, `exchCode`, `marketSector`, `ticker`, `name`; offline `identifiers_figi.py` also accepts injected mapping exports. | Source support exists; no retained mapping export was established. Mapping response type can help current classification but is not dated historical validity by itself. Existing signal selection permits ADR/REIT and is not this common-stock-only rule. No API call or credential inspection occurred. |
| SEC current issuer snapshots / ownership | `company_tickers.json` contains issuer CIK/ticker/title; submissions entity `tickers`, `exchanges`, `entityType`, `sic` are not dated security histories. Newer `ownership_identity.py` uses 13F `title_of_class`/CUSIP/common-share predicates. | Current ticker file (799,073 bytes), submissions archive (1,564,656,199 bytes) and its index exist; metadata only checked. 13F can corroborate a held class after filing availability, but holdings select a subset and its current-name bridges remain expressly reconstructed. Neither route provides a complete common-stock cohort. |

## Existing consumers and integration boundary

Do not copy the older pool3 warehouse classifier: its unmatched nonempty name
defaults to `common`. The newer branch's `universe_us_listed.py:157–217` instead
requires positive common/ordinary-share name evidence and emits
`common_unverified` otherwise. Its strict and reconstructed variants also preserve
distinct knowledge clocks. However, its eligible policy still includes ADR, REIT
and LP; it is not the strategy's strict common-stock-only policy. Its `<=` cutoff
and late-known interval conventions also require explicit translation into the
engine's strict-before-decision rule, not wholesale reuse of row eligibility.

The engine already has a useful strict type contract at
`atx-impl/src/stage_equity_universe.cpp:608` (`bind_types`) and
`data/point_in_time_universe.hpp`: schema `atx-instrument-types-v1`, source hashes,
security IDs, independently qualified type/publication/vintage, economic validity,
and endpoints known under the source clock. Unknown/conflicting types remain
excluded; an unknown-clock row cannot suppress earlier qualified evidence. That
stage and schema explicitly seal before 2020. The new recent research adapter has
no type-input seam. Neither consumer can silently be declared recent-stock
qualified merely by attaching today's directory.

## Smallest useful next step

1. Reuse the existing C9 evidence contract and already retained document inventory
   to identify **dated class/type candidates**, preserving its evidence and clock
   qualifications. A bounded evidence-only export can measure overlap with the
   role's fixed IDs without reading prices or using returns. No such export or
   overlap measurement was performed here.
2. Prefer an already-held `TickerDefinitionHist` export if one becomes available:
   validate actual dated type/venue fields, ID/class conflicts, original delivery
   metadata and historical completeness before assigning coverage. The retained
   research says reference delivery is 22:00 Central, which must not be mistaken
   for the strategy's 23:00 UTC decision clock or blindly applied to old vintages.
3. If filing enrichment is needed, preserve accession+context+class in FSN or
   original covers; never join title/symbol/exchange by CIK alone or backfill a
   later observation. Explicitly report unresolved line identity, missing type,
   unsupported vintage and listing gaps against the unchanged cohort denominator.
4. Only after evidence exists, add a versioned recent type projection/consumer
   with strict clocks and a named common-stock policy. Keep reconstructed research
   eligibility distinct from verified historical eligibility. There is no evidence
   here that this can immediately qualify all 3,000 names.

## Retained provenance

- `C:/atx/.superpowers/sdd/tier1-parity/historical-vendor-source-check.md`, SHA256
  `4164eb8e52a21cdafdb24dba1b7089e91ac19e9dc3c340a74ddf277c4ac560cc`:
  prior official SpiderRock product/schema research and limitations.
- `C:/atx/.superpowers/sdd/tier1-parity/historical-listing-source-check.md`, SHA256
  `1e48af7e3a0e03af5a043f6c1e329b72c07314dd2655daafa5fa8af9a4c9a6a1`:
  prior Nasdaq snapshot/Daily List and SEC API assessment.
- Newer branch reports `.superpowers/sdd/tier1-parity/historical-listing-bulk-feasibility.md`
  and `historical-listing-sec-bulk-research.md`: exact FSN fields, official source
  links, context/coverage/vintage limits. Their web findings were read as retained
  research, not freshly reverified in this inventory.
- Captured C9 fetch ledger (131,417 bytes) SHA256
  `92688dae5f5f6de2af6b42b0036c433984599a2ea6a07c3b59e0d7d407f977d0`.
  This is a capture of a potentially appendable ledger, not a permanent claim that
  future bytes will match. Document payloads were not opened.
- Filename-only scope: top-level `C:/Users/natha/Downloads` and
  `C:/atx/atx-db/data/cache`, plus the named C9 directory. The Downloads matches
  were only the old ticker-history ZIP and current `TickerHistory3.parquet`.
  This is not an exhaustive disk search or a warehouse coverage audit.
