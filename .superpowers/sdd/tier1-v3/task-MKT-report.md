# task-MKT report (tier1-v3 lane MKT)

Tasks: S1.5, S3.4, S3.6, S3.7, S7.1, CRSP-style market returns, S3 exit factor checks, S3.2 (internal).
Stopped at the owner stop (2026-09-30 ~01:33Z) after the step in flight (the shares stage) finished; no MKT job is
left in the guard queue, and every loop and monitor I started is stopped.

## 1. What was built

| module (`atx-db/src/atx_db/alpha_panel/`) | stage / output | state |
|---|---|---|
| `reference.py` | `reference/` fx_daily, rates_daily, vix_daily, series_catalog, french/{ff5,ff3,mom}_{daily,monthly}, french/siccodes (1.6 MB) | published; manifest sha 2f09dcbc1b737add7567d97ab1621d64b40b410fa767edacc75126797ddda2e7 |
| `classification.py` | `classification/` issuer_industry (19,571 rows), sic_map (435 SICs) (0.8 MB) | published; manifest sha c86e761d8c60be50f7796bcacf6da494352390fe84c157470fc4115c51eb5b35 |
| `shares_daily.py` | `market/shares_daily/year=YYYY/` 21,868,842 rows 2018-2026 (173 MB), `validation/shares_coverage.json` | published; `market/market_shares_manifest.json` sha 41af29e0fa817b0dff57028e027537f850825c18d11c6ed7a449ef5e6a8b0990 |
| `liquidity.py` | `market/liquidity/year=YYYY/` | code + tests only; 2 of 64 work units built when stopped (resumable) |
| `market_index.py` | `market/index_returns.parquet`, `validation/factors_market.json` | code + tests only, NOT RUN |
| `indexes.py` | `indexes/` constituents, membership, recon_summary | code + tests only, NOT RUN |
| `returns_validation.py` | `validation/returns.json` | code + tests only, NOT RUN |
| `market_common.py` | shared helpers (price projection, per-part manifests, resumable work units, member coverage) | committed |

Each stage self-registers with the lake registry through a module-level `LAKE_STAGES` literal (reference,
classification, market_shares, market_liquidity, market_index, indexes); `stagelake.contract.validate` passes.
Doc: `atx-db/docs/ALPHA_PANEL_MARKET.md`. Tests: `atx-db/tests/test_alpha_panel_{reference,shares,liquidity,market,
market_returns,indexes,classification}.py`, 41 tests, all offline, < 2 s each.

FX: `FX READY .../reference/fx_daily.parquet c49b057a3048406ba65146191d605bd63e6dc95f8600fed8b9ac1b2cc47b034c` in
signals.md (supersedes the first line 459580e7...: DEXVZUS is VEF, VES from 2018-08-20, VED from 2021-10-04).
The reference build ran unguarded per ruling C-1 (pure pyarrow): peak commit 0.155 GiB, working set 0.183 GiB.

## 2. Done criteria

| task | criterion | result | measured | command |
|---|---|---|---|---|
| S1.5 | every series complete 2010+ (gaps vs business days) | PASS | H.10 23 series: 9 missing of 4,194 federal business days (INR, VEF 10), 6 are federal closures, 3 Board blanks (2019-01-14, 2019-02-20, 2020-11-27); H.15 13 of 4,195 (Good Fridays, Sandy, 2018-12-05); DFF 0; VIX 20 (Good Fridays, Sandy, 2018-12-05) | `reference build` -> `reference/series_catalog.parquet` |
| S1.5 | each clock documented, never before publication | PASS | H.10 = Board release list (102,228 rows) or first release after the 2006-2009 suspension (5,635 rows, 2008), 16:30 ET; H.15 next federal business day 16:30 ET (checked: 2026-09-29 posting ends 09-28, FRED Last-Modified 20:16Z); VIX close day 16:30 ET; French = fetch time + vintage_risk | module docstring, ALPHA_PANEL_MARKET.md s.1 |
| S1.5 | fx_daily exact contract | PASS | columns exactly obs_date DATE, currency VARCHAR, usd_per_ccy DOUBLE, series_id VARCHAR, available_at TIMESTAMP; 107,863 rows | `DESCRIBE` |
| S3.7 | >= 99% of member_equity cells finite 2019-2026 | NOT RUN | stage not built (owner stop) | `liquidity all` |
| S3.4 | >= 99% of member_equity cells | PASS | 99.64% (per year 99.29-99.86%) | `shares_daily measure` -> `validation/shares_coverage.json` |
| S3.4 | two sources within 5% on >= 95% where both exist | PASS (PIT basis) | line-level cells (dei cover count, one equity line, not ADR): SEC PIT vs vendor PIT 96.34%; SEC PIT vs same-session vendor (not PIT) 94.99%; all SEC sources incl. issuer totals 85.7% (dual-class totals, ADR ordinary shares) | same |
| market returns | VW / EW daily + monthly | NOT RUN | code done (`crsp-index-v1`) | `market_index build` |
| S3 exit | monthly rho(VW - DTB3, French Mkt-RF) >= 0.99, <= 2022-12 | NOT RUN | | same, `validation/factors_market.json` |
| S3 exit | own 2x3 SMB vs French rho >= 0.90; HML recorded | NOT RUN | | same |
| S3.6 | membership daily 2019-2026; R2000 in [1900, 2100] per recon; turnover | NOT RUN | code done (`russell-proxy-v1`, `sp500-proxy-v1`) | `indexes build` |
| S3.6 | official list overlap | GAP (documented) | FTSE Russell / S&P lists are licensed, pages terms-gated; not scraped | |
| S7.1 | 100% of linked issuers with a SIC | PASS | 8,467 / 8,467 (of 8,644 linked CIKs) in every FF scheme and NAICS 2002 / 2022; FF49 unlisted 0.21% | `classification build` -> manifest receipt.coverage |
| S3.2 | total-return identity, 500 stratified line-months | NOT RUN | code done (`returns-identity-v1`) | `returns_validation` |
| S3.2 | independent second source | GAP (documented) | free price APIs forbid bulk automated use or need unapproved keys; SEC has no prices | |

## 3. Sources landed

| source | url | bytes | history | cadence | terms | rate |
|---|---|---|---|---|---|---|
| FRED graph CSV (38 series) | https://fred.stlouisfed.org/graph/fredgraph.csv?id=<SERIES>&cosd=2008-01-01 | ~3.3 MB (`data/raw/fred/`) | 2008-01-02 .. 2026-09-28 | daily; H.10 weekly | FRED public data, no key, attribution to the Board / source | 1 req/s |
| FRB H.10 release dates JSON | https://www.federalreserve.gov/releases/h10/releaseDates.json | 72,879 | 1996-07 .. 2026-09 | weekly | public domain (US government) | 1 req/s |
| FRED H.10 release table page | https://fred.stlouisfed.org/release/tables?rid=17&eid=23340 | 86,577 | snapshot | - | public | 1 req/s |
| Ken French library (6 factor zips, 8 Siccodes zips) | https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/ | ~2.9 MB extracted (`data/raw/french/`; zips deleted, sha in receipt) | 1926/1963 .. 2026-08 (CRSP 202608) | monthly re-post | free with citation | 1 req/s |
| Census SIC/NAICS concordances (5 files) | https://www.census.gov/naics/concordances/ | 857,015 (+3 converted CSVs; 1.4 MB `data/raw/census/`) | 1987 SIC -> 2002 -> 2022 NAICS | static | public domain | 1 req/s |

User agent: `atx-db/0.1 public-data-loader` (`shortflow_common.PUBLIC_UA`). Disk: raw 7.6 MB; lake reference 1.6 MB,
classification 0.8 MB, market 173 MB; `_tmp/mkt` 756 MB scratch (price projection + unit files; deletable, rebuilt
on demand). C: 54 GB free.

## 4. Deviations and open issues

1. Owner stop: liquidity, index returns + factor checks, index proxies and the return identity are coded and
   fixture-tested but not run. Resume (each under the guard, in order):
   `shares_daily` is done; then `liquidity all` (resumes at unit 3 of 64 if its inputs are unchanged),
   `market_index build`, `indexes build`, `returns_validation` - commands in ALPHA_PANEL_MARKET.md section 9, or run
   the scratch chain `mkt_chain.sh` (shares is skipped only if its inputs are unchanged; otherwise it rebuilds).
   Expect ~20 s per work unit; the guard stopped the chain 5 times on host low commit (stop_reason low_commit, my
   job peak 0.35-0.43 GiB of a 0.5 cap), hence the per-unit resume.
2. Shares: the fundamentals stage was republished during the build; the published shares bind the fundamentals
   manifest 9f9b2f85.... A later fundamentals publish makes `market_shares` STALE in `lake verify`: rebuild.
3. Shares agreement: the same-session vendor comparison is 94.99% (just under 95%); the gate is met on the
   point-in-time basis (96.34%). Multi-class issuers get per-line counts from the vendor only (Company Facts drops
   the per-class dei counts); an unlisted class (ZM, DLB class B) is absent from company ME.
4. Market cap and turnover use `shrout`, not the panel's `shares_out` (vendor lagged 90 days): the panel is outside
   this lane; switching it is a controller decision.
5. Classification: NAICS is approximate by construction; nine curated primaries (7372 -> 511210, 7389 -> 561990,
   6770 -> 525990, ...) and hierarchical group modes are documented; `naics2002_prefix` holds the certain digits.
   The Census `.xls` needed `xlrd` (not in the project environment) once at landing, loaded from a scratch dir.
6. Index proxies: IPO / spin-off additions between reconstitutions are not modelled; the semi-annual December 2026
   Russell reconstitution falls after the last vendor session; S&P proxy has no committee, float or sector rules.
7. Paths outside my ownership: none edited.

## 5. Commits

- 9096b54f reference stage (S1.5)
- ac06f988 classification stage (S7.1)
- 758a4781 market_shares stage + market_common + ALPHA_PANEL_MARKET.md (S3.4)
- 7fa535c8 liquidity, market_index, indexes, returns_validation code + tests (S3.7 / S3 / S3.6 / S3.2, not run)
- this report (next commit)
