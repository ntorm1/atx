# indexes (`indexes/`)

Rebuilt 2026-10-03 in Task 2x, recomputing eligible company caps, ranks, Russell bands, S&P buffers and membership intervals from the frozen panel with dated fresh FINRA exchange repair. SC -> XNAS, BZX -> BATS; no additive membership patch and no panel rebuild. Rule `russell-proxy-v1` / `sp500-proxy-v1`; manifest `indexes/manifest.json`; registry stub `indexes.registry.json`.

Command: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.indexes build`.

FINRA fallback replaces only NULL panel exchange when dissemination_date < rank session and age <=45 days. Unknown/stale classes remain unknown. T1-b's panel/link/directory qualification persists. Rank-date availability remains 22:00 UTC and membership starts the next session after reconstitution. These are value/membership products and span 2018-2026 despite R5's truncation of aggregate-return products.

## Rules (summary; full text in the module docstring and manifest `rule_text`)

- Eligible line on the rank date: `security_type` common / common_unverified / REIT, not ADR or FPI (US-domicile
  proxy), not LP or royalty trust, `is_operating`, listed on NYSE / NYSE American / Nasdaq / NYSE Arca / Cboe BZX,
  raw close >= USD 1, `shares_daily.shrout` known. Company = CIK (an unlinked line is its own company); company ME =
  sum of eligible lines' shrout x raw close. Every eligible line of a selected company is a member.
- Russell proxy: rank day in May (2018-2022) / last business day of April (2023+), effective after the fourth
  Friday of June; companies with ME >= USD 30M ranked; R3000 = top 3,000; R1000 / R2000 split at rank 1,000 with
  the +-2.5% cumulative-percentile band (none in 2018). A line leaves on its last vendor session; IPO / spin-off
  additions are not modelled.
- S&P-like proxy: quarterly (rank last session of Feb / May / Aug / Nov, effective after the third Friday of
  Mar / Jun / Sep / Dec); entrants need `ni_ttm` > 0 and `ni_q` > 0 and annual dollar volume >= 0.75 x company ME
  (`market_liquidity.adv_63` x 252); members stay while ranked <= 600; filled to 500 companies by ME.

## PIT rule

`available_at` = rank date 22:00 UTC (every input is known by then); membership is effective from the first
session after the reconstitution date, so `available_at` precedes `effective_from` by >= 4 weeks. As-of:
`indexes_membership_as_of(lake_cutoff(d))` rows with `session_date = d`, or interval join on `constituents`
(`effective_from <= d <= coalesce(effective_to, d)`) with `available_at < lake_cutoff(d)`.

## Columns

| file | name | type | unit | definition | clock | staleness |
|---|---|---|---|---|---|---|
| constituents | index_id | VARCHAR | - | proxy index | - | - |
| constituents | security_id, cik | BIGINT | id | member line and its company (CIK may be NULL) | - | - |
| constituents | effective_from / effective_to | DATE | date | first / last session (inclusive; NULL while open) | - | annual / quarterly |
| constituents | weight | DOUBLE | share | line ME share of the index on the rank date | rank 22:00 | - |
| constituents | company_rank | BIGINT | rank | company ME rank on the rank date | rank 22:00 | - |
| constituents | rank_date, recon_date | DATE | date | rank day and reconstitution day | - | - |
| constituents | basis | VARCHAR | - | `rank`, `band_stay` (Russell), `entry`, `buffer_stay` (S&P) | - | - |
| membership | session_date, security_id, cik | DATE / BIGINT | - | line-session | - | daily |
| membership | r1000, r2000, r3000, sp500 | BOOLEAN | flag | member of the proxy that session | rank 22:00 | daily |
| both | available_at | TIMESTAMP | UTC | rank date 22:00 | - | - |
| recon_summary | lines, companies, additions, deletions | BIGINT | count | per index and reconstitution | - | - |
| recon_summary | turnover_oneway, turnover_cap | DOUBLE | share | additions / lines; added ME share | - | - |
| recon_summary | p1000_cum_share, eligible_companies | DOUBLE / BIGINT | - | Russell breakpoint; companies eligible | - | - |


## R2000 reconstitutions

| rank | effective | lines | companies |
|---|---|---|---|
| 2018-05-11 | 2018-06-25 | 2019 | 2000 |
| 2019-05-10 | 2019-07-01 | 2080 | 2012 |
| 2020-05-08 | 2020-06-29 | 2056 | 2002 |
| 2021-05-07 | 2021-06-28 | 2054 | 1989 |
| 2022-05-06 | 2022-06-27 | 2048 | 1995 |
| 2023-04-28 | 2023-06-26 | 2066 | 2016 |
| 2024-04-30 | 2024-07-01 | 2065 | 2016 |
| 2025-04-30 | 2025-06-30 | 2056 | 2007 |
| 2026-04-30 | 2026-06-29 | 2046 | 2004 |


Inherited R2000 count gate [1,900,2,100]: {"companies": {"2018-06-22": 2000, "2019-06-28": 2012, "2020-06-26": 2002, "2021-06-25": 1989, "2022-06-24": 1995, "2023-06-23": 2016, "2024-06-28": 2016, "2025-06-27": 2007, "2026-06-26": 2004}, "criterion": "R2000 proxy companies in [1900, 2100] at every reconstitution", "lines": {"2018-06-22": 2019, "2019-06-28": 2080, "2020-06-26": 2056, "2021-06-25": 2054, "2022-06-24": 2048, "2023-06-23": 2066, "2024-06-28": 2065, "2025-06-27": 2056, "2026-06-26": 2046}, "official_overlap": "GAP: FTSE Russell / S&P DJI constituent lists are licensed (not measured)", "outside": {}, "pass": true}

## Outputs

| file | rows | bytes |
|---|---|---|
| indexes\constituents.parquet | 73175 | 1026539 |
| indexes\membership\year=2018\membership.parquet | 425591 | 155605 |
| indexes\membership\year=2019\membership.parquet | 751091 | 251546 |
| indexes\membership\year=2020\membership.parquet | 762144 | 251117 |
| indexes\membership\year=2021\membership.parquet | 757470 | 260781 |
| indexes\membership\year=2022\membership.parquet | 750282 | 251361 |
| indexes\membership\year=2023\membership.parquet | 748128 | 249599 |
| indexes\membership\year=2024\membership.parquet | 752665 | 246684 |
| indexes\membership\year=2025\membership.parquet | 747145 | 250103 |
| indexes\membership\year=2026\membership.parquet | 527028 | 197331 |
| indexes\recon_summary.parquet | 61 | 6971 |


## Limits

Official-list overlap remains an unmeasured licensed-source GAP. No float adjustment, committee/sector discretion, complete IPO or spin-off additions; CIK company identity inherits panel links. R3000 includes REITs and some classes deliberately excluded by broad all_common. SC representation and row-key/alignment checks are recorded in Task 2x report.

## Input provenance

| input | SHA-256 |
|---|---|
| market/market_liquidity_manifest.json | fc73dbfeb820743598127bfab814363339c2648a778e743b1ea1d74ceaafc51d |
| market/market_shares_manifest.json | 41af29e0fa817b0dff57028e027537f850825c18d11c6ed7a449ef5e6a8b0990 |
| panel | c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4 |
| security_master | 3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a |
