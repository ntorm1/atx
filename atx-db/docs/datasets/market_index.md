# market_index (`market/index_returns.parquet`)

Rebuilt 2026-10-03 in Task 2x with NULL exchange repair from dated FINRA listing classes through one shared mapping (SC -> XNAS, BZX -> BATS). Frozen panel is never rebuilt. Rule `crsp-index-v1`, schema `atx.alpha-panel.market-index/v1`; manifest `market/market_index_manifest.json`; validation `validation/factors_market.json`.

R5 strict holdout: source rows are filtered to session_date <= --end <= 2022-12-31 before daily/monthly aggregation. No index-return products after 2022 are published. Future factor-return products remain unavailable until separately authorized; code, index membership and universe coverage still span the latest session. The initial inherited-builder run ignored --end for product rows; that artifact was superseded by this corrected build. No held-out comparison/evaluation was performed.

Inputs: frozen panel, shares_daily, security_master/finra_names, reference, prices and prices_history (validation history only). Exchange fallback preserves FINRA dissemination_date < d and age <=45 days; unsupported/stale classes remain unknown. T1-b's panel/link/directory qualifications remain inherited.

Command: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.market_index build --end 2022-12-31`.

## Product coverage

| freq | universe | weighting | rows | first | last |
|---|---|---|---|---|---|
| D | all_common | EW | 1251 | 2018-01-12 | 2022-12-30 |
| D | all_common | VW | 1251 | 2018-01-12 | 2022-12-30 |
| D | member_common | EW | 1195 | 2018-04-05 | 2022-12-30 |
| D | member_common | VW | 1195 | 2018-04-05 | 2022-12-30 |
| M | all_common | EW | 59 | 2018-02-28 | 2022-12-31 |
| M | all_common | VW | 59 | 2018-02-28 | 2022-12-31 |
| M | member_common | EW | 56 | 2018-05-31 | 2022-12-31 |
| M | member_common | VW | 56 | 2018-05-31 | 2022-12-31 |

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


## Measured

| gate | value | threshold | status |
|---|---|---|---|
| Mkt-RF monthly | 0.9997303917927841 | 0.99 | PASS |
| SMB monthly | 0.9861019625328911 | 0.9 | PASS |
| Market 2013-2022 | 0.9983966080273335 | 0.99 | PASS |


## Eligible source rows by year (through holdout)

| year | all_common cells | member cells | known ME | member known ME |
|---|---|---|---|---|
| 2018 | 1134423 | 371727 | 991127 | 364089 |
| 2019 | 1162228 | 472793 | 1108971 | 471545 |
| 2020 | 1157848 | 479452 | 1100334 | 476896 |
| 2021 | 1266153 | 505636 | 1162326 | 501824 |
| 2022 | 1321612 | 483861 | 1244076 | 482370 |


## Limits

No exact delisting returns; pre-2018 validation history uses first-panel-session eligibility and vendor shares with survivorship; French is a reposted CRSP202608 vintage. Mapping fixes known SC/BZX omissions but cannot create missing FINRA evidence. NYSE breakpoints retain issuer-primary XNYS logic.

## Input provenance

| input | SHA-256 |
|---|---|
| market/market_shares_manifest.json | 41af29e0fa817b0dff57028e027537f850825c18d11c6ed7a449ef5e6a8b0990 |
| panel | c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4 |
| prices | aea43c13d1bb59d1088bdaa7d7ff953f6ede54c96e346cac2582f764a7ec56e8 |
| prices_history | 5204677e495dc007f2e8fa53ebb3d3c0e22d33df57af3555a5ca69c2f4117177 |
| reference | 2f09dcbc1b737add7567d97ab1621d64b40b410fa767edacc75126797ddda2e7 |
| security_master | 3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a |
