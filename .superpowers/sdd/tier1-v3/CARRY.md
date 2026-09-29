# tier1-v3 CARRY (plan `docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md`)

Durable decisions and facts that every lane carries. One line per item; newest last.

## Owner directive (2026-09-29 goal)

- Directive: implement the v3 plan to support downstream alpha generation for a US equity long/short strategy;
  focus on the last 6 years of data (score coverage 2020-09 → today; lookback history kept only where a
  characteristic needs it); reclaim as much disk as possible; deletion of anything unnecessary in `atx-db` approved.

## Rulings (S0.5)

- Ruling D1: the Parquet stage lake (`atx-db/data/alpha_panel/v1`) is the system of record. `warehouse.duckdb` is
  retired now rather than after S4 (owner disk directive): its 49 non-empty catalog/seed/rule tables are archived to
  `atx-db/data/archive/warehouse-v2/*.parquet` (4.2 MB, manifest with row counts + SHA-256); its five raw reload tables
  (fundamental_points, sec_company_facts, sec_submissions, equity_daily_bars, custom_features_daily) are reproducible
  from retained sources (`data/cache/companyfacts.zip`, `data/cache/submissions.zip`, TickerHistory3 sha256
  0ed96b26…abbae). The serving layer is `data/catalog.duckdb` (S1.3): views only, rebuildable.
- Ruling D2: alpha_panel fundamentals v9 is the canonical fundamentals engine. The v2 standardization rules and the
  derived DSL survive as code/seeds and as the archived catalog tables; the F.1 probe state (13.7 GB) was deleted.
- Ruling D3: buy nothing now. Build license adapters with mock loaders (S8.1) and free substitutes (guidance, OHLC
  spreads, rule-based index proxies, public short-side borrow proxy). Revisit before S8.3.
- Ruling D4: non-USD filers are converted with FRED H.10: period-end rate for balances, period-average rate for flows;
  the FX clock is the H.10 publication date; `fx_converted` flag set; the reporting-currency value is kept.
- Ruling D5: US-listed only (ADRs and FPIs included).
- Ruling D6: no return-based statistic on 2023+ data. Factor-replication windows end 2022-12-31; value comparisons are
  allowed in any year.
- Ruling D7: storage by deletion (owner-approved) rather than a new drive. New landings are limited to what a
  characteristic in the 2020+ score window needs: period-partitioned sources land from 2019 (one year of lookback),
  except fundamentals-like sources that need 5-year lags (from 2015). Zips are deleted after parse; receipts kept in
  `data/archive/receipts/`.
- Ruling D8: scheduling via Windows Task Scheduler remains user gate U10; S10 ships the job definitions and a
  runnable supervisor, and does not register tasks without the owner.

## Facts

- Fact 2026-09-29: two DuckDB processes sharing one `temp_directory` crash (0xC0000005 after 28 s in panel assemble
  2018-2021 while 2022-2026 ran). `alpha_panel.common.connect` now spills to `_tmp/spill-<pid>`.
- Fact 2026-09-29: disk reclaim batch 1 freed 49 GB on C: (29 → 78 GB free): `.pytest_cache` 9.8 GB (stale DuckDB
  schema templates), `warehouse.duckdb` 12.9 GB, `research/work` 15.2 GB, `research/features` 2.7 GB, duplicate
  TickerHistory3 in `staging/broad-bars` 3.6 GB (sha-identical to the Downloads copy the lake reads), FSDS zips
  5.7 GB (parsed copy in `staging/fsds-v2`), identity rehearsal/oracle scratch 2.2 GB, stale panel scratch 1.2 GB.
- Fact: lake source dependencies that must stay: `data/cache/companyfacts.zip`, `data/cache/submissions.zip`,
  `data/staging/companyfacts/ee099c7394a357f1`, `data/staging/fsds-v2/{num,pre,sub,tag}`,
  `data/research/identity_rehearsal/session8-phased-r4`, `C:/Users/natha/Downloads/TickerHistory3.parquet`,
  `C:/atx/data/finra_*`, `data/raw/*` landings.
