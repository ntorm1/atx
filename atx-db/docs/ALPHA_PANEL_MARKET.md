# Alpha panel: reference data, market data, index proxies, classification (tier1-v3 lane MKT)

Plan `docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md` tasks S1.5, S3.2 (internal), S3.4, S3.6, S3.7,
S7.1 and the S3 exit checks. Code: `src/atx_db/alpha_panel/{reference,shares_daily,liquidity,market_index,indexes,
classification,returns_validation,market_common}.py`. Build root `data/alpha_panel/v1`. Every stage publishes its
manifest last (schema, code SHA-256, per-file SHA-256, bound input manifests) and registers itself with the lake
registry through a module-level `LAKE_STAGES` literal. Measured numbers: section 8.

## 1. Reference stage (`reference/`, S1.5)

Sources (raw as served under `data/raw/fred/` and `data/raw/french/`, append-only `receipts.jsonl`):

| source | what | url | cadence |
|---|---|---|---|
| FRED graph CSV (no key) | H.15 `DGS1MO DGS3MO DGS6MO DGS1 DGS2 DGS3 DGS5 DGS7 DGS10 DGS20 DGS30 DTB3 DFF`, `VIXCLS`, the 23 H.10 daily series listed on FRED release 17 | `https://fred.stlouisfed.org/graph/fredgraph.csv?id=<SERIES>&cosd=2008-01-01` | daily / weekly (H.10) |
| Federal Reserve Board | H.10 release dates (JSON, 1996+) | `https://www.federalreserve.gov/releases/h10/releaseDates.json` | weekly |
| Ken French data library | FF5 (2x3) and FF3 daily + monthly, momentum daily + monthly, `Siccodes{5,10,12,17,30,38,48,49}` | `https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/` | monthly re-post |

Outputs:

| file | columns |
|---|---|
| `fx_daily.parquet` (lane FUND contract, ruling D4) | exactly `obs_date DATE, currency VARCHAR (ISO-4217), usd_per_ccy DOUBLE, series_id VARCHAR, available_at TIMESTAMP` |
| `rates_daily.parquet` | `obs_date, series_id, description, tenor_months, value_pct, release, available_at` |
| `vix_daily.parquet` | `obs_date, vix_close, series_id, available_at` |
| `series_catalog.parquet` | per series: first / last observation, business days expected and missing since 2010 |
| `french/{ff5,ff3,mom}_{daily,monthly}.parquet` | factors in decimal units, `vintage` (`CRSP YYYYMM`), `available_at`, `vintage_risk` |
| `french/siccodes.parquet` | `scheme, industry_no, industry_short, industry_name, sic_lo, sic_hi, range_desc, available_at` |

H.10 quotes in currency per USD are inverted (`usd_per_ccy`). FRED keeps one code for the bolivar across two
redenominations: `DEXVZUS` is VEF, VES from 2018-08-20 and VED from 2021-10-04.

Publication clocks (`available_at`, naive UTC; 16:30 America/New_York = 15 minutes after the stated posting time):

| rule | release | clock |
|---|---|---|
| `h10-weekly-monday-v1` | H.10: "On Mondays at 4:15 p.m. the Federal Reserve Board releases daily bilateral exchange rates ... for the previous business week. If Monday falls on a Federal Holiday, the data will be released on the following business day." | first Board release date on or after the Monday after the observation's week (mid-week revision releases skipped); 2008 observations (weekly release suspended May 2006 - January 2009) take the first release after the suspension, 2009-01-05 |
| `h15-next-business-day-v1` | H.15: "posted daily Monday through Friday at 4:15pm ... not posted on holidays"; each posting carries the previous business day (the 2026-09-29 posting ends at 2026-09-28) | next federal business day after the observation (weekend / holiday `DFF` values: after the next business day) |
| `vix-cboe-close-v1` | Cboe VIX close at 16:15 ET | observation day 16:30 ET |
| `french-fetch-v1` | whole files re-posted with each CRSP update, no history | fetch time; `vintage_risk` true |

Federal business days are weekdays that are not US federal holidays, observed dates included (`holidays.US`); this
closes the Board on more days than it closes, so clocks can only be later than publication.

## 2. Shares outstanding (`market/shares_daily/`, S3.4)

`market/shares_daily/year=YYYY/shares_daily.parquet`, one row per vendor line-session from 2018-01-02, rule
`shrout-pit-v1` (the CRSP `shrout` analog, point in time):

* `shares_sec`: the fundamentals stage `shrs_q` of the line's CIK (dei cover count, else balance-sheet, weighted
  average or class sums: `shares_sec_source`) of the latest filing known at 22:00 UTC of the previous session, as
  of its filing date, stale after 400 days.
* `shares_vendor_pit`: the vendor share series is the dei cover count applied from the cover date or period end,
  i.e. before the filing (measured: vendor change to matching filing lag p50 6-7 days, p95 49-56, p99 115-124 on
  2019 / 2021 / 2023). A vendor change is visible at the clock of the SEC filing whose count it equals (to the
  vendor's thousand-share rounding), never before the change date; unmatched changes 120 days later.
* both are split-adjusted to the session with the vendor split / reverse-split ratios after their as-of date.
* `shares_sec_line_level`: the SEC count measures the line itself: a dei cover count (Company Facts drops the
  dimensional per-class cover counts of multi-class issuers; its `cso` / `waso` / `cls_*` fallbacks are issuer
  totals or period averages), the CIK has exactly one linked equity line, and the line is not an ADR (the cover
  counts ordinary shares; TSM 5:1, BABA 8:1, PBR 2:1 in 2022).
* `shrout` = `shares_sec` when line-level, not older than the vendor observation and within a factor 1.5 of it,
  else `shares_vendor_pit` (per class: the only per-line source for multi-class issuers and ADRs). `shrout_source`,
  `shrout_asof`, `obs_available_at`, `split_adj`, `shrout_conflict` document the choice; `available_at` =
  max(observation clock, 22:00 UTC of the latest split ex-date applied).
* Company market equity (Russell-style total ME) sums `shrout x close` over the issuer's lines; an unlisted class
  (ZM class B, DLB class B) is not in any vendor line and is missing from it.
* `shares_vendor_current` (same-session vendor value) is kept for comparison only: it is not point in time.

## 3. Liquidity and cost inputs (`market/liquidity/`, S3.7)

`market/liquidity/year=YYYY/liquidity.parquet`, rule `liquidity-v1`, one row per line and calendar session between
its first and last bar (a listed session without a bar is a row): Corwin-Schultz (`spread_cs_21`, overnight-adjusted,
two-day estimates floored at 0) and Abdi-Ranaldo (`spread_ar_21`) spreads over 21 sessions, Amihud illiquidity
(`amihud_21/63/252`, |ret| per $1M), `turnover` = volume / `shrout` and its 21/63-session means, `zero_vol_21/63`,
dollar-volume ADV (`adv_21/63`, untraded sessions as 0), `halt_proxy` (listed, no trade) and `halt_days_21`.
Values use bars through the session (`available_at` = its 22:00 UTC mark). There is no halt feed in atx-db;
`halt_proxy` is the proxy.

## 4. Market index returns (`market/index_returns.parquet`) and the S3 exit checks

Rule `crsp-index-v1`: VW (lagged ME weights) and EW daily and monthly returns over `member_common` (member_equity
common stocks: no ADR, FPI, REIT, LP, royalty trust) and `all_common` (the same stock filters on every operating
listed line). Monthly: a line compounds its daily returns, weights are ME at the previous month's last session.
Delisting returns are not added (imputed in the delisting stage).

Exit checks (`validation/factors_market.json`, ruling D6: windows end 2022-12-31): monthly and daily correlation of
VW - rf (DTB3) with French Mkt-RF; SMB and HML from our own 2x3 size x BE/ME sorts (rule `ff-2x3-v1`: NYSE
breakpoints from `exchange` XNYS, BE = fundamentals `be` of the fiscal year ending in t-1 known by the end of
June t, ME of December t-1, portfolios held July t to June t+1) against French FF3 SMB / HML.

## 5. Index proxies (`indexes/`, S3.6)

Rules `russell-proxy-v1` (R1000P, R2000P, R3000P) and `sp500-proxy-v1` (SP500P); details in the module docstring.
Russell: rank days 2018-05-11, 2019-05-10, 2020-05-08, 2021-05-07, 2022-05-06, 2023-04-28, 2024-04-30,
2025-04-30, 2026-04-30; effective after the fourth Friday of June; total company ME >= $30M, price >= $1, top 3,000,
+-2.5% cumulative-percentile band at rank 1,000 (no band at 3,000). From 2026 FTSE Russell reconstitutes
semi-annually (second rank day the last business day of October, effective after the second Friday of December,
2026-12-11); that falls after the last vendor session and is not in the stage. S&P-like: quarterly, entry needs
positive GAAP TTM and quarterly earnings and annual dollar volume >= 0.75 x ME; members stay while ranked <= 600.

Gap: official constituent lists (FTSE Russell, S&P DJI) are licensed and their public pages terms-gated; nothing is
scraped and the proxy's overlap with the official lists is not measured. The LIC lane's index adapter
(`atx_db/licensed/indexes.py`) takes a licensed file with the same columns as `constituents.parquet`.

## 6. Classification (`classification/`, S7.1)

Rule `classification-v1`: FF 5/10/12/17/30/38/48/49 from the dated SIC with French's definition files (unlisted
SICs go to the residual industry; for 48/49, whose `Other` has explicit ranges, to `Other` with `ff48_listed` /
`ff49_listed` false); NAICS 2002 from the Census 1987 SIC -> 2002 NAICS concordance (a curated primary for nine
frequent SEC codes whose first concordance piece is a minor activity or that are not 1987 SICs: 6770 blank checks
-> 525990, 7372 -> 511210, 7389 -> 561990, 7370 -> 518210, 1000 -> 212299, 4911 / 4931 -> 221122, 5812 -> 722110,
6799 -> 523910; else the whole industry, else the "(except ...)" piece, else the first piece; SEC group codes by the
hierarchical mode of the member SICs' primaries), chained to NAICS 2022 through the 2002 -> 2007 -> 2012 -> 2017 ->
2022 concordances; `naics2002_prefix` = the longest common prefix of every candidate code (the digits that hold
whichever piece applies); `naics_approx` on every row. Dated by the
SIC event clock (one row per CIK and SIC run). Sources under `data/raw/census/` (the `.xls` files converted to CSV
at landing with `xlrd`; the build reads the CSV).

## 7. Return identity (`validation/returns.json`, S3.2 internal)

Rule `returns-identity-v1`: 500 line-months 2018-02 .. 2022-12, stratified by year x ME tercile x event month
(20% event months), deterministic seed. Checks: CRSP-convention identity with the corporate-action split ratio and
cash amount, the adjusted-close chain, and monthly compounding. The independent second source is not done: free
daily price APIs (Yahoo, Stooq, Nasdaq, Alpha Vantage) forbid bulk automated retrieval or redistribution, or need a
key under terms not approved here; SEC holds no prices.

## 8. Measured (2026-09-30, from the stage manifests and validation files)

**Reference.** `fx_daily` 107,863 rows, 23 H.10 series / 25 ISO codes (VEF, VES, VED), 2008-01-02 .. 2026-09-25;
`rates_daily` 63,102 rows; `vix_daily` 4,747 rows. Completeness 2010-01-01 .. last observation against federal
business days (4,194 H.10 / 4,195 H.15 and VIX expected):

| family | missing days | explained |
|---|---|---|
| H.10 (all 23) | 9 (INR 10, VEF 10) | 2014-12-26, 2018-12-24, 2020-12-24 (executive-order closures), 2017-01-20 and 2021-01-20 (Inauguration Day), 2018-12-05 (national day of mourning); 2019-01-14, 2019-02-20, 2020-11-27 blank in the Board series; INR 2010-01-26 and VEF 2011-05-13 single-series blanks |
| H.15 DGS*, DTB3 | 13 | Good Fridays (SIFMA close) 2011-2025, 2012-10-30 (Hurricane Sandy), 2018-12-05 |
| DFF | 0 | 7-day series |
| VIXCLS | 20 | Good Fridays (NYSE closed), 2012-10-29/30, 2018-12-05 |

H.10 clock basis: 102,228 rows from the Board release list, 5,635 (2008) from the first release after the
suspension. French files: CRSP 202608 vintage.

**Shares (`market_shares`).** 21.9M line-sessions 2018-2026 (173 MB). Share of panel `member_equity` cells
(2019-2026, `validation/shares_coverage.json`): `shrout` 99.64% (per year 99.29-99.86%); source SEC dei 76%,
vendor 24%; conflict fallbacks 1.7-2.9%. Agreement within 5% on line-level cells where both exist: SEC PIT vs
vendor PIT 96.34%; SEC PIT vs same-session vendor (not PIT, weeks ahead) 94.99%; any SEC source (issuer totals
included) vs vendor 85.7% (dual-class totals and ADR ordinary shares, not like-for-like).

**Classification.** 19,571 SIC runs of 16,623 CIKs from 418,138 SIC events; 8,467 of 8,467 linked issuers with a
SIC (of 8,644 linked) carry every FF scheme and NAICS 2002 / 2022 (100%); FF49 unlisted-SIC share 0.21%. NAICS
basis over the 435 SICs: whole 122, except-piece 85, first-piece 53, 3-digit group 119, 2-digit group 44,
curated 9, none 3 (6189 / 8880 / 9995, no linked issuer).

**Not yet run on the lake** (code and fixture tests committed): `market_liquidity`, `market_index` with
`validation/factors_market.json`, `indexes`, `validation/returns.json`. Resume: section 9.

## 9. Build commands

Every DuckDB step runs under the memory guard; the shares and liquidity builds resume per work unit (64 units; a
guard stop keeps the finished units while the input manifests and code are unchanged).

```bash
cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
G="../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.5 --wait-minutes 600 --"
.venv/Scripts/python.exe -m atx_db.alpha_panel.reference fetch    # network landing (<4 MB)
.venv/Scripts/python.exe -m atx_db.alpha_panel.reference build    # pure pyarrow, peak 0.155 GiB
.venv/Scripts/python.exe $G .venv/Scripts/python.exe -m atx_db.alpha_panel.classification all
.venv/Scripts/python.exe $G .venv/Scripts/python.exe -m atx_db.alpha_panel.shares_daily all    # build + measure
.venv/Scripts/python.exe $G .venv/Scripts/python.exe -m atx_db.alpha_panel.liquidity all       # build + measure
.venv/Scripts/python.exe $G .venv/Scripts/python.exe -m atx_db.alpha_panel.market_index build
.venv/Scripts/python.exe $G .venv/Scripts/python.exe -m atx_db.alpha_panel.indexes build
.venv/Scripts/python.exe $G .venv/Scripts/python.exe -m atx_db.alpha_panel.returns_validation
```

The `.xls` Census files need `xlrd` once at `classification fetch` (not in the project environment; it was loaded
from a scratch directory for the landing, and the converted CSVs with their SHA-256 are in the receipt).
