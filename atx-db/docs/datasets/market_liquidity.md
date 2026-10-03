# market_liquidity (`market/liquidity/`)

Cost-model inputs per line-session (tier1-v3 S3.7, rule `liquidity-v1`, module `atx_db.alpha_panel.liquidity`).
Built 2026-10-03 in tier1-v4 Task 1. Manifest `market/market_liquidity_manifest.json` (schema
`atx.alpha-panel.market-liquidity/v1`, `gate_status` included); coverage receipt `validation/liquidity_coverage.json`.

- Path: `data/alpha_panel/v1/market/liquidity/year=YYYY/liquidity.parquet` (one file per year).
- Keys: `(security_id, session_date)`, unique (22,927,098 rows, 0 duplicates). `security_id` = TickerHistory3 line.
- Span: 2018-01-02 .. 2026-09-18; 1,579,964,789 bytes. Rows per year: 2018 2,139,183; 2019 2,198,681; 2020
  2,266,378; 2021 2,699,451; 2022 2,996,980; 2023 2,790,668; 2024 2,742,304; 2025 2,860,249; 2026 2,233,204.
- Grain: every calendar session between a line's first and last vendor bar (a listed session without a bar is a
  row with `has_bar = false`). Delisted lines included up to their last bar.
- Inputs: `prices/` (raw OHLC, volume, dollar volume, vendor total return, price factor) and
  `market/shares_daily/` (`shrout`); input manifest SHA-256s in the manifest.

## PIT rule

A value at session d uses bars through d's close: `available_at` = d 22:00 UTC. Lake convention: visible at
decision session d only if `available_at < lake_cutoff(d)` (22:00 UTC of the previous session), i.e.
`market_liquidity_as_of(lake_cutoff(d))`. Daily, no restatement (rebuilt with prices; a prices republish changes
history only where the vendor bars change).

## Columns

| name | type | unit | definition | source | clock | staleness |
|---|---|---|---|---|---|---|
| security_id | BIGINT | id | vendor line | prices | - | - |
| session_date | DATE | date | calendar session | calendar | - | - |
| has_bar | BOOLEAN | flag | vendor bar exists this session | prices | d 22:00 | daily |
| halt_proxy | BOOLEAN | flag | listed (inside first..last bar) and no trade (no bar or volume 0); no halt feed exists | prices | d 22:00 | daily |
| halt_days_21 | INTEGER | sessions | `halt_proxy` sessions in the last 21 | prices | d 22:00 | daily |
| spread_cs_21 | DOUBLE | fraction of price | Corwin-Schultz (2012) high-low spread: mean of two-day estimates over pairs (t-1, t), t in last 21 sessions, negatives set to 0, overnight-gap adjustment (CS II.D), t-1 on t's price basis; NULL if < 10 pairs | prices | d 22:00 | daily |
| n_pairs_cs_21 | INTEGER | count | pairs used by `spread_cs_21` | prices | d 22:00 | daily |
| spread_ar_21 | DOUBLE | fraction of price | Abdi-Ranaldo (2017) CHL estimator sqrt(max(mean 4(c_t-eta_t)(c_t-eta_t+1), 0)); NULL if < 10 pairs | prices | d 22:00 | daily |
| amihud_21 / amihud_63 / amihud_252 | DOUBLE | abs return per USD 1M | Amihud (2002) mean of abs(ret) / dollar volume x 1e6 over traded sessions with an unguarded return; needs >= 10 / 30 / 60 obs | prices | d 22:00 | daily |
| n_amihud_252 | INTEGER | count | observations in `amihud_252` | prices | d 22:00 | daily |
| turnover | DOUBLE | shares / shares out | volume / `shrout` of the session | prices, shares_daily | d 22:00 | daily |
| turnover_21 / turnover_63 | DOUBLE | per session | mean turnover over the window (untraded = 0); needs `shrout` on >= half the window | prices, shares_daily | d 22:00 | daily |
| zero_vol_21 / zero_vol_63 | DOUBLE | share | share of window sessions without a trade | prices | d 22:00 | daily |
| adv_21 / adv_63 | DOUBLE | USD | mean raw close x volume over the window (untraded = 0) | prices | d 22:00 | daily |
| shrout_used | DOUBLE | shares | `market/shares_daily.shrout` of the session | shares_daily | d 22:00 | daily |
| available_at | TIMESTAMP | UTC | session_date 22:00 | - | - | - |

Windows count calendar sessions (`ROWS`), so a halted or untraded session is inside the window.

## Measured coverage (finite share of panel `member_equity` cells; gate S3.7 >= 99%)

| year | cells | spreads, amihud, adv, zero_vol, halt | turnover | turnover_21 | turnover_63 |
|---|---|---|---|---|---|
| 2019 | 576,186 | 1.0000 | 0.9974 | 0.9965 | 0.9945 |
| 2020 | 580,695 | 1.0000 | 0.9952 | 0.9943 | 0.9920 |
| 2021 | 611,044 | 1.0000 | 0.9929 | 0.9906 | 0.9851 |
| 2022 | 584,009 | 1.0000 | 0.9971 | 0.9964 | 0.9949 |
| 2023 | 575,461 | 1.0000 | 0.9986 | 0.9982 | 0.9975 |
| 2024 | 573,063 | 1.0000 | 0.9982 | 0.9977 | 0.9966 |
| 2025 | 553,772 | 1.0000 | 0.9961 | 0.9955 | 0.9942 |
| 2026 | 389,602 | 1.0000 | 0.9960 | 0.9951 | 0.9930 |
| 2019-2026 | 4,443,832 | 1.0000 | 0.9964 | 0.9955 | 0.9934 |

Member-cell medians (2021): `spread_cs_21` 0.80% (p10 0.40%, p90 1.75%), `spread_ar_21` 0.26% (p10 0, p90 1.75%),
`amihud_21` 4.4e-4, `turnover_21` 0.78% per session, `adv_63` USD 43.2M (p10 11.9M, p90 309M).

## Known limits

- Spreads are daily OHLC estimators, not TAQ quoted / effective spreads; Abdi-Ranaldo is floored at 0 (p10 = 0).
- `halt_proxy` is a no-trade proxy; there is no exchange halt feed in atx-db.
- Turnover inherits `shares_daily` coverage (99.64% of member cells); turnover_63 in 2021 is 98.5% (gate is the
  2019-2026 pool).
- Early 2018 rows are warm-up: windows shorter than their minimum observation counts are NULL.
- 1.58 GB on disk: read a year partition or project columns.
