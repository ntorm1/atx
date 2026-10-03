# indexes (`indexes/`)

Rule-based Russell 1000 / 2000 / 3000 and S&P-500-like proxies, point in time (tier1-v3 S3.6, rules
`russell-proxy-v1`, `sp500-proxy-v1`, module `atx_db.alpha_panel.indexes`). Built 2026-10-03 in tier1-v4 Task 1.
Manifest `indexes/manifest.json` (schema `atx.alpha-panel.indexes/v1`, `gate_status` included).

## Outputs

| file | keys | rows | bytes |
|---|---|---|---|
| `indexes/constituents.parquet` | `(index_id, security_id, recon_date)` | 73,177 | 1,016,128 |
| `indexes/membership/year=YYYY/membership.parquet` | `(session_date, security_id)` (unique) | 6,218,671 | 2,085,448 |
| `indexes/recon_summary.parquet` | `(index_id, recon_date)` | 61 (3 x 9 Russell, 34 S&P) | 6,964 |

`index_id`: `R1000P`, `R2000P`, `R3000P`, `SP500P`. Membership spans 2018-03-19 (S&P proxy) / 2018-06-25 (Russell
proxies) .. 2026-09-18; every session from 2019 carries all four flags.

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

## Measured

| gate | measured | result |
|---|---|---|
| R2000 proxy companies in [1900, 2100] at every reconstitution | 1,989 .. 2,016 (2018 2,000; 2019 2,014; 2020 2,003; 2021 1,989; 2022 1,992; 2023 2,014; 2024 2,016; 2025 2,008; 2026 1,996) | PASS |
| official list overlap >= 95% (plan S3.6) | not measurable: FTSE Russell / S&P DJI lists are licensed, pages terms-gated | GAP |

Mean member lines per session: R3000P 2,964 (2019) .. 2,986 (2025); R1000P 1,002-1,029; R2000P 1,959-1,986;
SP500P 525-530 (500 companies; share classes counted as lines). R2000P one-way turnover 12-22% per year
(cap-weighted 5.5-16.8%); SP500P 1-4.5% per quarter after the first.

## Known limits

- Proxies only: no float adjustment, no committee, sector or IPO rules; the December 2026 semi-annual Russell
  reconstitution falls after the last vendor session.
- SP500P counts every eligible line of a member company (about 26-36 extra lines vs 500 companies).
- Company identity is the CIK from the panel link; an unlinked line is ranked as its own company.
