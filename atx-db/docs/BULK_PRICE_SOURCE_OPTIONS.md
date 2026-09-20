# Bulk price source investigation — 2026-09-20

The user supplied an updated SpiderRock file while this investigation was in
progress. The completed download is `TickerHistory3.parquet`, not a ZIP. It
has been copied with a 1 MiB buffer into a separate staging directory; the old
archive and the Downloads original remain intact. Footer inspection reports
32,323,644 rows in 262 row groups, dated 2012-03-26 through 2026-09-18.

The staged file is 3,617,973,507 bytes with SHA-256
`0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae`.
The copy observed an unchanged source size and modification time. Full source
quality auditing and warehouse publication are still pending. Footer dates do
not prove valid rows, historical listing classification, adjustment accuracy or
historical delivery vintages.

## Verified source options

| Source | Documented bulk route | Access and relevance |
| --- | --- | --- |
| SpiderRock | Date-partitioned Parquet on S3; TickerHistory3 full history | Matches the supplied source family. AWS credentials or an authorized cross-account role are provisioned during onboarding. [Delivery documentation](https://docs.spiderrockconnect.com/docs/HistoricalData/parquet_files/) |
| Massive | Daily whole-US-market compressed files; alternatively one grouped OHLC request per date | File history starts in 2003. Flat files are unadjusted; grouped API adjustment is split-only, not dividend total return. [Daily files](https://massive.com/docs/flat-files/stocks/day-aggregates), [file semantics](https://massive.com/docs/flat-files/stocks/overview), [grouped API](https://massive.com/docs/rest/stocks/aggregates/daily-market-summary) |
| EODHD | Whole-exchange daily prices, splits or dividends in one request per dataset/date | Bulk access is excluded from its free plan and consumes plan quota. It is a possible supplemental feed, not an anonymous download. [Bulk API](https://eodhd.com/financial-apis/bulk-api-eod-splits-dividends) |
| Nasdaq Data Link | Whole-table ZIP export through `qopts.export=true` | Useful delivery mechanism for an entitled dataset; the Sharadar product page did not render usable current terms/schema in this inspection. [Table export documentation](https://docs.data.nasdaq.com/v1.0/docs/in-depth-usage-1) |
| Norgate | Local data updater and supported export/integration tools | Platinum/Diamond packages include delisted securities and historical index constituents. Those are candidates for survivorship work, not proof of this warehouse's required listing intervals. [Package coverage](https://norgatedata.com/stockmarketpackages.php), [content definitions](https://norgatedata.com/data-content-tables.php) |

Massive's free individual tier documents two years of API history and excludes
flat files. Its individual plans are for personal/nonprofessional use; business
access must be evaluated under its business offering. No account or license was
created or purchased. [Plan comparison](https://massive.com/stocks?auth=signup),
[business access](https://massive.com/business).

Massive also documents dated ticker queries, security type, exchange, CIK/FIGI
and delisting fields. This is a concrete candidate for the missing historical
reference input if access becomes available. The API is paginated, with a
maximum of 1,000 records per page. Historical availability and completeness
would still need validation before using it as PIT listing evidence.
[Reference API](https://massive.com/docs/rest/stocks/tickers/all-tickers).

Stooq's bulk page returned a browser-verification requirement during inspection.
No current whole-market download, delisted coverage or suitable adjustment
contract was verified from its own documentation, so it is not an approved
replacement source. [Bulk page](https://stooq.pl/db/h/).

## Local access and next action

The bounded configuration check found no matching provider key names in the
current environment or repository `.env`, and no AWS config/credentials files.
No secret values were printed, credentials sent, accounts created, or contact
messages sent. This is a check of these configured locations, not a claim about
every application or account on the machine.

Use the newly supplied Parquet through the bounded native source reader,
re-audit its full contents, and publish its actual counts. Keep the existing
raw/adjusted/split-only distinctions and unknown historical listing status.
Do not splice differently normalized vendors' adjusted prices by ticker.
The new file addresses the observed price-date gap; it does not independently
close the historical identity and common-equity membership gates.

Local operator receipts: `.superpowers/sdd/tier1-parity/updated-price-footer.json`,
`updated-price-staging.json` and `updated-price-staging-memory.json`.
