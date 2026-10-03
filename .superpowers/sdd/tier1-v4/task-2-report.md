# tier1-v4 Task 2 report: universe stage `universe/`

Status: DONE_WITH_CONCERNS (one binding gate FAILs: French NYSE count, 14 of 60 months; reported, not weakened).
Branch `feat/tier1-v3-warehouse`. Every data job ran under the memory guard from `C:/atx/atx-db` with
`PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`; `G(x)` = `.venv/Scripts/python.exe
../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb x --wait-minutes 240 -- .venv/Scripts/python.exe -m`.

## 1. What was built

| item | value |
|---|---|
| module | `atx-db/src/atx_db/alpha_panel/universe.py` (rule `universe-all-common-v1`, schema `atx.alpha-panel.universe/v1`) |
| output | `data/alpha_panel/v1/universe/year=YYYY/universe.parquet`, one row per panel row, keys `(session_date, security_id)` |
| rows / bytes | 21,868,842 rows, 198,121,511 bytes (2018 2,006,722 ... 2025 2,783,952; 2026 2,151,262) |
| span | 2018-01-02 .. 2026-09-18 (the panel's) |
| manifest | `universe/manifest.json` (`gate_status`, code SHA = committed module, 9 input manifest SHA-256s, all current) |
| validation | `validation/universe.json` (gates, per-year counts, 30-field coverage), `validation/universe_hand_check_sample.json` |
| command | `G(0.7) atx_db.alpha_panel.universe build`: final run 441.8 s, guard peak 0.655 GiB (DuckDB 380 MB); resumable per year |
| docs | `atx-db/docs/datasets/universe.md`, `universe.registry.json` (stub), `universe_hand_check.json` (100 verdicts) |

Columns: `cik, shrcd_like, exchcd_like, exchcd_basis, is_adr, is_reit, is_spac, is_cef_etf, is_foreign_issuer,
filer_regime, me, univ_all_common, univ_r3000p, univ_r1000p, univ_r2000p, univ_sp500p, available_at` (= d 22:00 UTC).
Issuer facts are taken as of `lake_cutoff(d)` (strict `<`); codes through the single path
`share_exchange_history_as_of(lake_cutoff(d)) WHERE d BETWEEN valid_from AND valid_to` on Task 1's fixed stage
(`share-exchange-runs-v2`, rebuilt 02:07Z; my builds started after it), so a new code shows from its first
session: codes are on 100.00% of all_common cells.

Rule `universe-all-common-v1` = (1) the market_index all_common rule, imported (`market_index.common_filter`,
`is_operating`, not `is_index`, exchange in `market_index.LISTED`) on a repaired listing exchange; (2) raw close >= $1,
`shares_daily.shrout` > 0; (3) CRSP 10/11 refinements: vendor ticker not preferred / warrant / unit / right
(`security_master.SYMBOL_SUFFIX`, imported), filer regime `domestic`, not foreign issuer (FPI or incorporated
abroad), SPAC (SIC 6770), CEF / ETF / BDC (814- file number) or REIT (SIC 6798). Full text in the module docstring /
manifest `rule_text` and `docs/datasets/universe.md`.

## 2. Gates (GC9; all measured by `G(0.7) atx_db.alpha_panel.universe build` -> `validation/universe.json`)

| criterion | result | measured |
|---|---|---|
| all_common names/day (yearly mean) in [3,200, 4,800] each year 2020-2025 (ruling T2-a) | PASS | 2020 3,312; 2021 3,681; 2022 3,731; 2023 3,561; 2024 3,497; 2025 3,428 (daily min 3,061 on 2020-06-30, max 3,870) |
| NYSE all_common lines within +-5% of French ME-breakpoint NYSE count, each month <= 2022 | FAIL | 46/60 months inside. Outside: 2018-01..04 -9.7..-13.1% (shares gap), 2020-06 -6.7% (vendor gap), 2021-11, 2021-12, 2022-01..06, 2022-08 +5.1..+6.6% (companies +3.9..+5.3%). Other months within +-4.9% |
| R2000 proxy companies in [1,900, 2,100] at each reconstitution (first effective session) | PASS | 1,962 .. 1,994 over 9 recons (2018-06-25 .. 2026-06-29) |
| codes on >= 99% of all_common cells | PASS | 100.0000% each year 2020-2025 (shrcd_like and exchcd_like) |
| flags >= 95% precise on a 100-line hand check | PASS | 97/100: all_common 40/40, is_reit 12/12, is_spac 12/12, is_cef_etf 12/12, is_foreign_issuer 12/12, is_adr 9/12 |
| per-year coverage of 30 core fields on univ_r3000p and univ_all_common republished | DONE | `docs/datasets/universe.md` (also member_equity for reference) |

Summary: 4 PASS, 1 FAIL (+ coverage table delivered). Manifest `gate_status.pass` = false (French).

Per-year daily all_common count with and without the $1 filter (mean, min-max):

| year | with $1 | without $1 | market_index rule as published (no $1 / shares) | market_index rule + $1 + shares, repaired exchange |
|---|---|---|---|---|
| 2020 | 3,312 (3,061-3,492) | 3,485 (3,220-3,574) | 3,852 | 4,150 |
| 2021 | 3,681 (3,494-3,870) | 3,739 (3,557-3,994) | 4,102 | 4,539 |
| 2022 | 3,731 (3,561-3,867) | 4,021 (3,980-4,052) | 4,250 | 4,613 |
| 2023 | 3,561 (3,443-3,701) | 3,942 (3,867-4,015) | 3,978 | 4,361 |
| 2024 | 3,497 (3,444-3,542) | 3,814 (3,748-3,884) | 3,729 | 4,189 |
| 2025 | 3,428 (3,314-3,515) | 3,674 (3,583-3,753) | 3,532 | 4,048 |

The third column reproduces Task 1's receipt (2025 3,532).

## 3. Tests

- `.venv/Scripts/python.exe -m pytest -n 0 tests/test_universe_stage.py` -> 18 passed in 5.15 s (fixture lake, offline).
  Rules covered: all_common membership on a fixture day; $1 and shares screens; preferred ticker, FPI regime, REIT
  SIC, ETF, ADR exclusions; exchange repair (SC -> XNAS, exchcd 3, basis `finra_class_repair`) and its 45-day
  staleness; codes as-of (new run visible from its first session, a late-stamped run never visible, no fallback to
  an older run); cover-page incorporation PIT with snapshot fallback; SPAC SIC history PIT; BDC from 814- filings with
  N-54C withdrawal and a Form 4 not counted; link_table_v3 fallback PIT and no fan-out from overlapping links; index
  membership as-of; per-session receipt counts; pure maps / regex; `FINRA_STALE_DAYS == panel.SI_STALE_DAYS`; band,
  French (float boundary), R2000 and hand-check gates; end-to-end `build()` publishing the manifest with
  `gate_status` and resuming from per-year receipts.
- `pytest -n 0 tests/test_universe_stage.py tests/test_alpha_panel_market.py tests/test_alpha_panel_indexes.py
  tests/test_alpha_panel_identity_v3.py tests/test_lake_registry.py` -> 61 passed, 1 failed: the pre-existing
  `test_live_lake_every_manifest...` (20 unregistered manifests of several lanes; `universe/manifest.json` joins them
  by design, GC5 stub).

## 4. Files changed

- New: `atx-db/src/atx_db/alpha_panel/universe.py`, `atx-db/tests/test_universe_stage.py`,
  `atx-db/docs/datasets/universe.md`, `atx-db/docs/datasets/universe.registry.json`,
  `atx-db/docs/datasets/universe_hand_check.json`, this report.
- Not changed: `panel.py` (foreign), `market_index.py`, `security_master.py`, `identity_v3.py` (imported only).

Commits: b2d3b470 (module + tests), 8791f4cd (docs + stub + hand check), this report.

## 5. Sources, disk

No new source (all inputs in the lake; French breakpoints from `data/raw/benchmarks/parsed/`). One read of
`https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_me_breakpoints.html` to confirm the count
basis ("all NYSE stocks with CRSP share code 10 or 11 ... exclude closed end funds and REITs": stocks, so the gate
compares lines; companies reported too). Disk: `universe/` 189 MB, `_tmp/mkt/universe` 0.9 MB receipts; C: 70 GB free.

## 6. Deviations, open issues, self-review

1. **Rule refined beyond the market_index rule (deviation, needed by the binding gate).** With the imported rule
   alone, NYSE lines exceed the French count by +30% to +48% every month 2018-05 .. 2022-12 (preferreds with `.PR`
   vendor tickers typed `common_unverified`, warrants, unlinked ADRs / foreign ordinaries, CEFs, BDCs). The rule
   imports `common_filter` + `LISTED` and adds CRSP 10/11 screens on top; nothing is copied. Task 6 / 8 / 16 consume
   the refined flag; market_index's own all_common return series still uses its rule (VW returns barely affected).
2. **Upstream defect found (concern for the controller):** `security_master.EXCHANGE_BY_CLASS` lacks FINRA market
   classes `SC` (Nasdaq Capital Market) and `BZX`. Consequences outside my files: panel `exchange` NULL for those
   lines, `share_exchange_history.exchcd` NULL, market_index all_common and `indexes` eligibility drop every Nasdaq
   Capital Market stock (so the R2000 proxy never holds one). The universe repairs the exchange locally from the
   same FINRA row (492-706 all_common lines a day); the source fix is `security_master.py` + rebuilds of panel
   (foreign), codes, market_index, indexes.
3. **French FAIL causes:** (a) `market/shares_daily` covers ~3,700-3,900 lines a session in Jan-Apr 2018 vs
   6,700-7,700 from May 2018; (b) vendor short day 2020-06-30 (panel 7,934 rows vs 8,635); (c) 2021-11..2022-08
   residual +5.1..+6.6% with no identifiable class left (≈3 residual SPACs with a non-6770 SIC, 2-7 LP/LLC names on
   NYSE; LP-name screen tested and rejected as immaterial); it tracks the 2021 IPO / de-SPAC wave. Not closable
   without CRSP share codes.
4. `share_exchange_history` stamp: the original `valid_from - 2 h` stamp would have left the transition session
   without a code under the as-of path; Task 1's fix landed before my builds, and the published universe shows
   codes on 100% of all_common cells. A run stamped at or after `lake_cutoff(d)` gives NULL, never an older run
   (fixture test).
5. Qualification carried into the doc: codes and panel flags inherit the panel's same-session visibility of a few
   inputs (FINRA names `dissemination_date < d`, panel link `<= d 22:00`, non-PIT Nasdaq directory fallback).
6. `is_adr` (panel flags) 9/12 precise: `is_adr_likely` marks directly listed foreign ordinaries and a FINRA
   "ADR"-typed warrant; no effect on all_common (foreign issuers are excluded either way).
7. identity_v3 `shrcd_like` quirks inherited: preferreds typed `common_unverified` carry 11/18; funds/BDCs carry 73
   (14 never occurs). all_common does not depend on `shrcd_like`.
8. Process: no `LAKE_STAGES` literal (three inputs have no registered stage). While the module was in progress
   (uncommitted, about 02:20-02:55Z) a draft literal with a non-literal value made `stagelake.registry.load()` raise
   for anyone importing it from the shared tree; removed before any commit (registry loads 35 stages now).
9. Self-review: diff touches only the five new files; no data or scratch committed; every as-of join filters
   `available_at < lake_cutoff(d)`; no return statistic computed (GC2 untouched: value/count comparisons only);
   test file runs in ~5 s; the manifest binds the committed module SHA and current input manifests.
