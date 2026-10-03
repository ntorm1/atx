# market_index (`market/index_returns.parquet`)

CRSP-style value- and equal-weight market returns (tier1-v3 S3 market returns, rule `crsp-index-v1`, module
`atx_db.alpha_panel.market_index`). Built 2026-10-03 in tier1-v4 Task 1. Manifest
`market/market_index_manifest.json` (schema `atx.alpha-panel.market-index/v1`, `gate_status` included); factor
checks `validation/factors_market.json`.

- Path: `data/alpha_panel/v1/market/index_returns.parquet` (9,022 rows, 137,142 bytes).
- Keys: `(freq, period_end, universe, weighting)`.
- Span: daily `all_common` 2018-01-12 .. 2026-09-18 (2,182 sessions), daily `member_common` 2018-04-05 ..
  2026-09-18 (2,126); monthly `all_common` 2018-02 .. 2026-08 (103 months), `member_common` 2018-05 .. 2026-08
  (100). The running month is left out until its last session is within 4 days of month end.
- Inputs: `panel/` (security flags, exchange, raw close, `ret`, `member_equity`), `market/shares_daily/`
  (`shrout`), `reference/` (French factors, DTB3), `prices/`, `prices_history/` (validation history only).

## Universes

- `all_common`: every `is_operating`, non-index panel line listed on NYSE, NYSE American, Nasdaq, NYSE Arca or
  Cboe BZX with `security_type` common / common_unverified, not ADR (`is_adr`, `is_adr_likely`), not FPI, REIT,
  LP or royalty trust (CRSP share code 10/11-like). Mean 3,594 stocks per session (daily VW), 3,605 per month.
  Line-sessions per year: 2018 971,233; 2019 985,222; 2020 974,439; 2021 1,033,777; 2022 1,066,648; 2023 994,425;
  2024 939,717; 2025 882,981; 2026 595,519 (ME known on 86% in 2018, 94-96% from 2019).
- `member_common`: the same filters on panel `member_equity` lines (top-3,000 liquidity screen). Mean 1,831 per
  session.

## PIT rule

ME = `shares_daily.shrout` x raw close. Daily return of session d: lines in the universe at the previous session
with ME > 0 there and an unguarded `ret` at d, weighted by that lagged ME (VW) or equally (EW). Monthly: a line's
month compounds its daily returns (a guarded or missing day drops the line-month), weights = ME at the prior
month's last session. Delisting returns are not added. `available_at` = 22:00 UTC of the period's last session
(as-of: `available_at < lake_cutoff(d)`).

## Columns

| name | type | unit | definition | source | clock | staleness |
|---|---|---|---|---|---|---|
| freq | VARCHAR | - | `D` daily, `M` monthly | - | - | - |
| period_end | DATE | date | session (D) or calendar month end (M) | calendar | - | - |
| universe | VARCHAR | - | `all_common` or `member_common` | panel | - | - |
| weighting | VARCHAR | - | `VW` (lagged ME) or `EW` | - | - | - |
| ret | DOUBLE | simple return | index return of the period | panel, shares_daily | period end 22:00 | daily / monthly |
| n_stocks | BIGINT | count | lines in the average | panel | period end 22:00 | daily / monthly |
| me_lag_total | DOUBLE | USD | sum of lagged ME of those lines | shares_daily, panel | period end 22:00 | daily / monthly |
| available_at | TIMESTAMP | UTC | last session of the period, 22:00 | - | - | - |

## Validation (ruling D6 / GC2: windows end 2022-12-31)

| check | measured | gate | result |
|---|---|---|---|
| monthly rho(member_common VW - DTB3, French Mkt-RF), 2018-05..2022-12 (56 months) | 0.99968 | >= 0.99 | PASS |
| same, all_common, 2018-02..2022-12 (59 months) | 0.99958 (daily 0.99965, 1,250 days) | - | recorded |
| monthly rho(own 2x3 SMB, French SMB), 2018-07..2022-12 (54 months) | 0.98019 | >= 0.90 | PASS |
| monthly rho(own 2x3 HML, French HML) | 0.98502 | recorded | - |
| monthly rho(all_common VW, French Mkt-RF + RF), 2013-01..2022-12 (120 months) | 0.99835 | >= 0.99 (GC2) | PASS |
| of which history 2013-01..2018-01 (61 months, rule `crsp-index-history-v1`) | 0.99431 | - | recorded |
| of which stage 2018-02..2022-12 (59 months) | 0.99956 | - | recorded |

Mean monthly difference vs French Mkt over 2013-2022: -7.6 bp (stage-only Mkt-RF all_common: -2.8 bp). 2x3
portfolios hold on average SL 422, SM 691, SH 606, BL 431, BM 302, BH 131 firms.

The 2013-2018 history (validation only, not published as index rows): lines in `all_common` on the panel's first
session (2018-01-02), their flags standing in for 2012-2017; ME = vendor `shares_vendor` x raw close at the prior
month's last session; returns from `prices_history` (2012-2017) and `prices/` before 2018-02. Mean 3,362 stocks.

## Known limits

- No delisting returns in the index (the delisting stage's are imputed).
- History segment has survivorship (lines that ended before 2018 are absent) and uses vendor shares.
- `exchange` is NULL on part of the panel (lines then drop out of `all_common`); NYSE breakpoints use `XNYS` on
  the issuer's primary line.
- French files are a re-posted vintage (CRSP 202608, `vintage_risk`).
