# indexes (`indexes_v2/`, point in time; legacy `indexes/` v1 below)

Stage `indexes_v2` (P9 request A.1-A.5, Path B): rules `russell-proxy-v2` / `sp500-proxy-v2`, the same proxies
as v1 rebuilt so that every row holds only facts public at its own `available_at`. It is written beside the
published v1 stage, which is never touched (readers re-pin to `indexes_v2/`). Module
`atx_db.alpha_panel.indexes` (rules, R7 rank snapshot, writer), kernel `private_index_pit` (R1-R5 and the
receipt), `exchange_calendar` (NYSE sessions from rules). Registry: the module's `LAKE_STAGES` declares
`indexes_v2` and keeps the legacy `indexes` declaration (no command: v1 is never rebuilt); stub
`indexes.registry.json`. The v2 declaration is `planned: true` until Task 7's first publication.

Commands (every real-data run under `run_memory_guarded.py --heavy --job-gb <= 1.0`):

- `python -m atx_db.alpha_panel.indexes build [--pit-links identity/links_pit.parquet]` writes `indexes_v2/`
  (refuses when its manifest exists). Not yet run on real data (P9 Task 7).
- `python -m atx_db.alpha_panel.indexes measure [--from-year 2018] [--pit-links P]` reads the published v1
  stage and the panel and writes nothing: per rank date, the v1-selected lines whose type came from the
  directory snapshot, carry `directory_etf` or a backfill link, and the v2-eligible company count.

## Eligibility (R7) — options chosen: R7.1 (a), R7.2 (d)

- **R7.1 (a), dated type only.** The panel `security_type` counts only where `security_type_basis =
  'finra_name'`; a line typed by the 2026-09-18 directory snapshot (or the vendor) is not eligible. `is_etf` =
  type ETF / fund / ETN without `directory_etf`; no `directory_*` column decides selection. `is_operating`,
  `is_adr_likely`, `is_fpi`, `is_spac`, `is_royalty_trust` are recomputed in the stage with `panel.py`'s
  formulas from these inputs.
- **R7.2 (d), strict and name links only.** Company key and `is_operating` use a CIK only from `link_tier`
  strict / name. A backfill-linked line is kept but loses the backfill CIK and every issuer fact it brought
  (`filer_regime`, `sec_last_periodic_date`, `sic`, `sale_ttm`, `ni_*`). With `--pit-links` (link days computed
  without backfill withholding, `links_combined` schema, tier strict / high / medium / name with `available_at` <= the rank
  mark) it is name-linked: issuer facts from its own row when the CIKs agree, else from a strict / name row of
  that CIK on the date, else it stays unlinked (counted). Without `--pit-links` every backfill line is its own
  company with `is_operating` by the unlinked rule, and the manifest says `pit_links: null`.
- Then as v1: common / common_unverified / REIT, not ADR(-likely) / FPI / LP / royalty trust, operating, not an
  index line, listed (dated FINRA exchange repair), raw close >= USD 1, `shrout` known. Russell: ME >= USD 30M,
  top 3,000, R1000 / R2000 at rank 1,000 with the 2.5% band. S&P-like: 500 by ME, buffer 600, entrants with
  positive `ni_ttm` and `ni_q` (unknown when share classes disagree) and `adv_63` x 252 >= 0.75 ME (every line's
  `adv_63` known).

## PIT rule (R1-R5)

- R1 `constituents`: every member selected on the rank-date snapshot, `available_at` = rank date 22:00 UTC; no
  column depends on `last_session`, `delisting_date` or any later session.
- R2 `effective_from` = first NYSE session after the reconstitution date; `effective_to` = the next scheduled
  reconstitution (`scheduled_reconstitutions`; a holiday Friday moves to the session before, e.g. S&P 2026-06-18),
  also for the last one built (Russell 2026-06-26 -> 2026-12-11; S&P 2026-09-18 -> 2026-12-18). The calendar
  is generated from rules (`holidays` NYSE calendar) and checked equal to the vendor `calendar.parquet` on its
  whole extent at every build (3,642 sessions, 2012-03-26 .. 2026-09-18 on 2026-10-05); a mismatch aborts.
- R3 `exits`: a member whose line ceased before the scheduled end (`delisting_date` set, or `last_session` before
  the vendor's last session), stamped 22:00 UTC of the first session after its last session; `exit_basis`
  `explicit_delisting_date` / `ceased_before_observed_vendor_end`.
- R4 `recon_summary`: stamped at the rank mark; prior members minus exits stamped before it.
- R5 `membership` of session s: `available_at` = max(rank mark, 22:00 UTC of the session before s), from vintages
  stamped by then; it runs to the first session after the vendor's last session.
- As of a mark T: rows with `available_at < T`; a constituents row is closed early by its exit row once that is
  visible. A reconstitution is built whenever its rank date has panel data: 2018-02-28 .. 2026-08-31 today (2018-
  2019 give the band / buffer their prior state; acceptance foregrounds 2020 through the latest data).

## Outputs (R8)

`indexes_v2/<product>/available_year=YYYY/<product>.parquet` (UTC year of `available_at`, written as naive UTC
TIMESTAMP; ZSTD, 262,144-row groups), every part in the manifest `files` with bytes and SHA-256. Manifest keys:
`schema` `atx.alpha-panel.indexes/v2`, `status: complete`, `rule`, `rule_text`, `clock_rule`
`index-vintage-mark-v2`, `staleness_days` 371, `files`, `code`, `input_manifests_sha256`, `pit_links`,
`gate_status`, `receipt`; written last and atomically.

| product | columns |
|---|---|
| constituents | `index_id, security_id, cik, effective_from, effective_to, weight, company_rank, rank_date, recon_date, basis, available_at` |
| exits | `index_id, security_id, recon_date, last_session, available_at, exit_basis` |
| membership | `session_date, security_id, cik, r1000, r2000, r3000, sp500, available_at` |
| recon_summary | `index_id, rank_date, recon_date, effective_from, lines, companies, additions, deletions, turnover_oneway, turnover_cap, p1000_cum_share, eligible_companies, available_at` |

`cik` is the company key CIK (strict / name, or the pit name link); never a backfill CIK.

## Receipt (A.5, measured)

`counts_on_recon` and `rank_vintage_rows_on_recon` per `"<index_id> <recon_date>"` (must be equal, else the
build aborts before writing); `exit_rows` per index; `directory_decided_on_rank` (selected lines whose type basis
is not `finra_name`; must be 0) and `backfill_cik_on_rank` (`selected_company_keys`: selected lines keyed by a
backfill CIK; `excluded_only_is_operating`: backfill lines failing only `is_operating` through a backfill CIK;
both must be 0) per rank date, measured on the selected set and the rank-date population;
`eligibility_on_rank` diagnostics (v1-eligible lines dropped by (a) / otherwise, backfill lines re-linked by name
or left unlinked, `backfill_cik_would_fail_operating` (what R7.2 (e) would report), selected lines whose
`directory_etf` was ignored or whose `shrout` came from an SEC count); `coverage`; `focus_2020_plus`; the
calendar check; the inherited R2000 gate [1,900, 2,100] in `gate_status`.

## Measured before the first build (2026-10-05, guarded, nothing written)

`measure` on the published v1 stage: of the v1 R2000P members, 1,058 / 1,128 / 1,226 / 1,309 / 1,406 / 1,489 /
1,629 lines (rank dates 2020-05-08 .. 2026-04-30) had a type from the directory snapshot and 169 / 191 / 277 /
277 / 273 / 300 / 310 a backfill link. Every one of those lines has a dated FINRA type of `common_unverified`
(panel `finra_type`) that the directory named `common` / `REIT`. Eligible companies with ME >= USD 30M under
(a) + (d): 1,803 / 1,976 / 1,895 / 1,516 / 1,254 / 1,031 / 777, so the R2000 proxy cannot reach 1,900. A dry
selection of the build (no output) gave R2000P companies 866 / 1,018 / 921 / 671 / 502 / 356 / 201 at the
2020-2026 reconstitutions: **gate FAIL**, band unchanged (ruling IX-4). Details: P9 Task 6 report.

## Limits

Official-list overlap unmeasured (licensed). No float, committee or sector discretion, IPO or spin-off
additions. R7.1 (a) leaves the dated FINRA types only, which cannot tell a `common_unverified` common stock from
an ETP; `shrout` of a backfill-linked line may still come from an SEC count of the backfill CIK
(`market_shares`, reported per rank date, not changed here).

## Legacy v1 publication (`indexes/`, unchanged)

Rebuilt 2026-10-03 in Task 2x, recomputing eligible company caps, ranks, Russell bands, S&P buffers and membership intervals from the frozen panel with dated fresh FINRA exchange repair. SC -> XNAS, BZX -> BATS; no additive membership patch and no panel rebuild. Rule `russell-proxy-v1` / `sp500-proxy-v1`; manifest `indexes/manifest.json`; registry stub `indexes.registry.json`.

Command: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.indexes build`.

FINRA fallback replaces only NULL panel exchange when dissemination_date < rank session and age <=45 days. Unknown/stale classes remain unknown. T1-b's panel/link/directory qualification persists. Rank-date availability remains 22:00 UTC and membership starts the next session after reconstitution. These are value/membership products and span 2018-2026 despite R5's truncation of aggregate-return products.

### Rules (summary; full text in the module docstring and manifest `rule_text`)

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

### PIT rule

`available_at` = rank date 22:00 UTC (every input is known by then); membership is effective from the first
session after the reconstitution date, so `available_at` precedes `effective_from` by >= 4 weeks. As-of:
`indexes_membership_as_of(lake_cutoff(d))` rows with `session_date = d`, or interval join on `constituents`
(`effective_from <= d <= coalesce(effective_to, d)`) with `available_at < lake_cutoff(d)`.

### Columns

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


### R2000 reconstitutions

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

### Outputs

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


### Limits

Official-list overlap remains an unmeasured licensed-source GAP. No float adjustment, committee/sector discretion, complete IPO or spin-off additions; CIK company identity inherits panel links. R3000 includes REITs and some classes deliberately excluded by broad all_common. SC representation and row-key/alignment checks are recorded in Task 2x report.

### Input provenance

| input | SHA-256 |
|---|---|
| market/market_liquidity_manifest.json | fc73dbfeb820743598127bfab814363339c2648a778e743b1ea1d74ceaafc51d |
| market/market_shares_manifest.json | 41af29e0fa817b0dff57028e027537f850825c18d11c6ed7a449ef5e6a8b0990 |
| panel | c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4 |
| security_master | 3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a |
