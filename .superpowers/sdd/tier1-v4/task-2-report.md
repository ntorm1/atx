# tier1-v4 Task 2 report: universe stage `universe/`

Current fix-round status: DONE_WITH_CONCERNS; 4 PASS / 1 FAIL, French 10/60 months outside unchanged +/-5%. The original report and Fix round 1 below are historical; current measurements are in Fix round 2. I4 remains open; no waiver or carry-forward ruling is asserted.

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

## Fix round 1 (2026-10-03): Task 2 plus Task 2x dependency

Status: DONE_WITH_CONCERNS. Implementer: Codex, controller-selected gpt-6.1-sol under R1; historical GC6 trailer retained as required, not a claim of an Anthropic runtime. I1, I2, I3 and I5 fixed; I4 remains FAIL with measured residuals requiring controller ruling/remediation. No PIT/accuracy threshold amended. No foreign panel code or stage changed/rebuilt. No shared registry or progress.md edits.

Code commits: `d306e7768f7d72293aa600718a396814c98caba9` (Task 2x), `f0ab20f50a5d9fa4e4e8d0a68b23beeba90f51f4` (Task 2 classifiers/sample binding). Documentation/report commit is listed in the final controller response; use these specific commits, never BASE..HEAD.

- I1: incorporation uses dated cover facts strictly before cutoff; undated profile snapshot removed, earlier state unknown. Later-foreign-snapshot fixture protects history.
- I2: SIC 6770 supplements SPAC only with zero/missing panel sale_ttm, matching established panel behavior. Positive-revenue retained-SIC de-SPAC fixture admitted.
- I3: authoritative SC/BZX mapping and dated NULL-panel exchange fallback in codes, market and index eligibility. Company caps/ranks/bands/buffers/intervals fully recomputed; universe rebuilt after dependency. Task 2x details in task-2x-report.md.
- I5: latest qualifying 814- status event before each cutoff replaces global min-election/max-withdrawal. Withdrawal/re-election/future-withdrawal fixture proves no appended-future revision; withdrawals win a tied event timestamp.
- Also closes validation incompleteness risk: expected French months must exist, missing R2000 year counts fail; precision requires unique keys, matching seed/rule/sample hash. 17 newly selected sample keys manually rechecked against primary SEC descriptions/closing chronology, 83 unchanged identity/stratum claims retain prior judgments. The same-classifier zero-suspect result below is internal consistency, not independent accuracy validation.

### Gates

| criterion | PASS/FAIL | measured | command |
|---|---|---|---|
| daily_band | PASS | {"2020": 3340.3, "2021": 3689.3, "2022": 3732.8, "2023": 3562.4, "2024": 3499.0, "2025": 3430.5} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| French +/-5% | FAIL | 33/60 inside; missing=[] | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| R2000 effective companies | PASS | {"2018-06-25": 1987, "2019-07-01": 1993, "2020-06-29": 1980, "2021-06-28": 1972, "2022-06-27": 1980, "2023-06-26": 1990, "2024-07-01": 1992, "2025-06-30": 1988, "2026-06-29": 1972} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| codes >=99% | PASS | {"2020": 1.0, "2021": 1.0, "2022": 1.0, "2023": 1.0, "2024": 1.0, "2025": 1.0} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| hand precision >=95% | PASS | 97/100; per stratum {'is_adr': {'correct': 9, 'lines': 12}, 'is_cef_etf': {'correct': 12, 'lines': 12}, 'is_foreign_issuer': {'correct': 12, 'lines': 12}, 'is_reit': {'correct': 12, 'lines': 12}, 'is_spac': {'correct': 12, 'lines': 12}, 'univ_all_common': {'correct': 40, 'lines': 40}} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |


Rule `universe-all-common-v2`; sample SHA-256 `1bddcafc12563e986a6ec5124df0e4d3e2fd97f16b78e2399c6f8aa29388a37e`. ADR remains 9/12 despite pooled 97/100; no recall claim. Actual beta/idiosyncratic-volatility coverage remains Task 6; the 30-field table includes clearly labeled input sufficiency. Annual counts and all three coverage universes still cover 2020-2025.

### Built artifacts

| year | rows | bytes |
|---|---|---|
| 2018 | 2006722 | 15813869 |
| 2019 | 2110686 | 19088007 |
| 2020 | 2192161 | 19783137 |
| 2021 | 2616995 | 22863117 |
| 2022 | 2765910 | 25731721 |
| 2023 | 2615921 | 24346647 |
| 2024 | 2625233 | 24100337 |
| 2025 | 2783952 | 25162589 |
| 2026 | 2151262 | 19681143 |


### Month-by-month French reconciliation

Benchmark count unit and +/-5% line-count gate are retained. Company counts are diagnostic only. Exclusive classes partition observed NYSE panel lines in t2-french-reconciliation.json; source coverage counters overlap separately. Zero admitted non-common names under the existing FINRA classifier is not external validation.

| month | French | admitted lines | companies | residual lines | otherwise eligible missing shares | admitted unknown incorporation |
|---|---|---|---|---|---|---|
| 2018-01 | 1228 | 1128 | 1122 | -100 | 278 | 1128 |
| 2018-02 | 1226 | 1169 | 1166 | -57 | 233 | 1169 |
| 2018-03 | 1225 | 1172 | 1169 | -53 | 230 | 1172 |
| 2018-04 | 1224 | 1165 | 1165 | -59 | 219 | 1165 |
| 2018-05 | 1224 | 1354 | 1339 | 130 | 16 | 1354 |
| 2018-06 | 1225 | 1354 | 1339 | 129 | 17 | 1354 |
| 2018-07 | 1226 | 1346 | 1330 | 120 | 20 | 1346 |
| 2018-08 | 1225 | 1353 | 1340 | 128 | 15 | 1353 |
| 2018-09 | 1226 | 1357 | 1341 | 131 | 15 | 1357 |
| 2018-10 | 1225 | 1344 | 1328 | 119 | 15 | 1344 |
| 2018-11 | 1221 | 1350 | 1332 | 129 | 14 | 1350 |
| 2018-12 | 1215 | 1332 | 1318 | 117 | 16 | 1332 |
| 2019-01 | 1211 | 1333 | 1317 | 122 | 14 | 1333 |
| 2019-02 | 1206 | 1328 | 1311 | 122 | 14 | 1328 |
| 2019-03 | 1204 | 1329 | 1312 | 125 | 11 | 1329 |
| 2019-04 | 1204 | 1326 | 1310 | 122 | 12 | 1326 |
| 2019-05 | 1209 | 1320 | 1306 | 111 | 16 | 1320 |
| 2019-06 | 1213 | 1328 | 1313 | 115 | 17 | 1324 |
| 2019-07 | 1213 | 1305 | 1289 | 92 | 17 | 1065 |
| 2019-08 | 1214 | 1258 | 1243 | 44 | 17 | 403 |
| 2019-09 | 1210 | 1247 | 1236 | 37 | 17 | 319 |
| 2019-10 | 1206 | 1241 | 1229 | 35 | 13 | 261 |
| 2019-11 | 1205 | 1234 | 1222 | 29 | 12 | 188 |
| 2019-12 | 1194 | 1227 | 1214 | 33 | 13 | 168 |
| 2020-01 | 1192 | 1220 | 1207 | 28 | 11 | 161 |
| 2020-02 | 1190 | 1214 | 1198 | 24 | 12 | 146 |
| 2020-03 | 1182 | 1184 | 1169 | 2 | 12 | 126 |
| 2020-04 | 1182 | 1186 | 1174 | 4 | 12 | 120 |
| 2020-05 | 1177 | 1188 | 1175 | 11 | 10 | 103 |
| 2020-06 | 1173 | 1100 | 1091 | -73 | 10 | 90 |
| 2020-07 | 1167 | 1184 | 1172 | 17 | 12 | 86 |
| 2020-08 | 1168 | 1187 | 1175 | 19 | 14 | 21 |
| 2020-09 | 1171 | 1184 | 1172 | 13 | 15 | 15 |
| 2020-10 | 1169 | 1173 | 1162 | 4 | 13 | 15 |
| 2020-11 | 1168 | 1199 | 1188 | 31 | 12 | 16 |
| 2020-12 | 1170 | 1208 | 1195 | 38 | 14 | 18 |
| 2021-01 | 1167 | 1203 | 1191 | 36 | 12 | 18 |
| 2021-02 | 1170 | 1212 | 1199 | 42 | 11 | 17 |
| 2021-03 | 1182 | 1216 | 1202 | 34 | 18 | 14 |
| 2021-04 | 1180 | 1214 | 1203 | 34 | 17 | 14 |
| 2021-05 | 1184 | 1224 | 1212 | 40 | 24 | 13 |
| 2021-06 | 1195 | 1229 | 1212 | 34 | 24 | 13 |
| 2021-07 | 1207 | 1232 | 1214 | 25 | 15 | 13 |
| 2021-08 | 1211 | 1254 | 1236 | 43 | 26 | 3 |
| 2021-09 | 1213 | 1266 | 1249 | 53 | 26 | 1 |
| 2021-10 | 1222 | 1265 | 1249 | 43 | 29 | 0 |
| 2021-11 | 1229 | 1292 | 1277 | 63 | 20 | 0 |
| 2021-12 | 1230 | 1297 | 1281 | 67 | 23 | 0 |
| 2022-01 | 1230 | 1301 | 1286 | 71 | 18 | 0 |
| 2022-02 | 1229 | 1302 | 1285 | 73 | 18 | 0 |
| 2022-03 | 1228 | 1310 | 1294 | 82 | 16 | 2 |
| 2022-04 | 1228 | 1309 | 1291 | 81 | 17 | 2 |
| 2022-05 | 1228 | 1309 | 1292 | 81 | 12 | 0 |
| 2022-06 | 1241 | 1305 | 1290 | 64 | 12 | 0 |
| 2022-07 | 1239 | 1301 | 1285 | 62 | 11 | 0 |
| 2022-08 | 1236 | 1305 | 1288 | 69 | 9 | 0 |
| 2022-09 | 1239 | 1298 | 1282 | 59 | 11 | 0 |
| 2022-10 | 1229 | 1287 | 1270 | 58 | 12 | 0 |
| 2022-11 | 1224 | 1273 | 1258 | 49 | 8 | 0 |
| 2022-12 | 1219 | 1266 | 1250 | 47 | 8 | 0 |


Exact failed months/deviations: `{"2018-01": -0.0814, "2018-05": 0.1062, "2018-06": 0.1053, "2018-07": 0.0979, "2018-08": 0.1045, "2018-09": 0.1069, "2018-10": 0.0971, "2018-11": 0.1057, "2018-12": 0.0963, "2019-01": 0.1007, "2019-02": 0.1012, "2019-03": 0.1038, "2019-04": 0.1013, "2019-05": 0.0918, "2019-06": 0.0948, "2019-07": 0.0758, "2020-06": -0.0622, "2021-11": 0.0513, "2021-12": 0.0545, "2022-01": 0.0577, "2022-02": 0.0594, "2022-03": 0.0668, "2022-04": 0.066, "2022-05": 0.066, "2022-06": 0.0516, "2022-07": 0.05, "2022-08": 0.0558}`.

Measured unresolved causes and next work: early negative residuals coexist with dated shares/source-coverage holes among lines otherwise satisfying the rule. Those holes are measured here, but the French aggregate contains no identities, so not every missing candidate is established as a missing benchmark member. Repairing authorized market_shares SEC/vendor coverage is a feasible upstream step; it must use original clocks, not future share backfill. Removal of the newly introduced non-PIT incorporation fallback also exposes early positive residuals in 2018-05..2019-07, when dated incorporation covers are missing. Those cannot be closed by restoring the snapshot; an authorized dated historical incorporation source is a feasible remedy. Later positive residuals persist after internal fixes. Multi-class lines explain only line-minus-company counts; admitted incorporation-unknown counts quantify whether missing dated incorporation could explain excess. No named class reconciliation establishes the remaining counterpart; do not assert licensed CRSP is necessary or that broad counts validate accuracy. Feasible next remediation is an independent dated audit of the residual candidate cohort (US domicile, security/share type, blank-check chronology, filer regimes, vendor/link validity), plus a documented benchmark-definition comparison and issuer-level reconciliation using free source evidence. Aggregate benchmark alone cannot assign a specific falsely admitted line. Controller carry-forward ruling remains pending; manifest FAIL preserved.

### Tests and self-review

`.venv/Scripts/python.exe -m pytest tests/test_universe_stage.py tests/test_exchange_repair.py tests/test_alpha_panel_market.py tests/test_alpha_panel_indexes.py tests/test_alpha_panel_identity_v3.py -q -n0`: 61 passed, offline, under 10 seconds combined. R5 regression injects an extreme 2023 return, proves it never enters source u before aggregation, checks no output period >=2023, and rejects later --end. Covered exchange SC/BZX, unsupported/stale/same-date evidence, codes, market/rank eligibility, cutoff/link/index transitions, retained SIC, snapshots, BDC future append, missing months and stale sample binding. Explicit-path diffs checked; owned code/tests clean after commits. No full suites or held-out evaluation.

### Commands / memory evidence

All real builds and SQL/fetch/build diagnostics use the guard, one at a time in this lane. Fixture tests need no guard. All source updates are local source contracts; no new raw source landing.

| job | exact command |
|---|---|
| before audit | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_audit.py before |
| codes | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.identity_v3 codes |
| market R5 final | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.market_index build --end 2022-12-31 |
| indexes | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.indexes build |
| universe final | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| French reconciliation | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_reconcile.py |
| verification | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_verify.py |
| hand evidence binding | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.4 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_handcheck.py |


| guard receipt | status | cap GiB | peak GiB | wall s | exit |
|---|---|---|---|---|---|
| 20261003T160034-3528 | completed | 0.8 | 0.3918 | 14.0 | 0 |
| 20261003T160212-17664 | completed | 0.8 | 0.4613 | 27.0 | 0 |
| 20261003T160357-6664 | completed | 0.9 | 0.5447 | 26.0 | 0 |
| 20261003T160448-16944 | completed | 0.9 | 0.3929 | 45.0 | 0 |
| 20261003T160607-11508 | completed | 0.9 | 0.5385 | 30.0 | 0 |
| 20261003T160722-15108 | completed | 0.9 | 0.6555 | 265.2 | 0 |
| 20261003T161245-14416 | completed | 0.9 | 0.5236 | 192.2 | 0 |
| 20261003T161717-13484 | completed | 0.4 | 0.037 | 1.0 | 0 |
| 20261003T162105-14416 | completed | 0.9 | 0.6603 | 263.2 | 0 |
| 20261003T162800-12600 | completed | 0.9 | 0.6045 | 93.1 | 0 |
| 20261003T162956-15928 | completed | 0.8 | 0.4347 | 44.0 | 0 |


Raw/stage provenance: original Task 1 commits ee353557, 0c35357e, ff4d829a, 5b773a2f, 3afb9687; original Task 2 b2d3b4705272a01c61d8924283f1431610d9604a, 8791f4cdaf6613f11e157b5f79887a5919a0da3f, bf0ad7faa480aff09cac7540b070cfe6fd52c937. Frozen panel provenance SHA `c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4` unchanged. Baseline full manifests/input hashes in t2-before-audit.json; corrected hashes/code fingerprints in t2-after-verification.json and dataset docs. No return value/evaluation >=2023 inspected. Initial inherited market run published later aggregate-return product rows despite --end; R5 corrected/superseded it, and future factor-return products are now unavailable without authorization.

Files: owned alpha_panel universe/security_master/identity_v3/market_index/indexes modules; universe/exchange regressions; universe/indexes/market_index/share_exchange_history docs and registry stubs; hand judgments; reports and reproducible diagnostics/measurement artifacts in this scratch directory. No new raw data; SEC/FINRA/Nasdaq web pages were primary reference checks, not lake source landings. Existing free FINRA semi-monthly names, SEC filings/covers, vendor price/shares and French monthly benchmark receipts/terms/cadence remain those of original Task 1/2 reports.

### Final source-coverage and dated-header assessment

June 2020 residual is -73 lines. A diagnostic cohort of 1,170 NYSE common lines present at both May29 and July31 month ends has 90 missing June30 panel rows; all 90 also lack June30 price-stage and original vendor bars. This is a measured vendor-source coverage hole. Neighboring future observations are diagnostic only and do not backfill features; aggregate French counts cannot prove which candidates belong to the benchmark. Evidence: t2-vendor-coverage.json and its producer.

Missing-incorporation admitted NYSE month-end cohorts contain 1,420 distinct CIKs in 2018 and 1,359 in 2019, union 1,454. All 1,454 have a current CIK file in existing submissions.zip; that archive can supply accession discovery, but its current incorporation profile is not historical evidence. Existing sec_text primary-document metadata covers 2,588 pre2020 filings for 1,277 cohort CIKs, earliest 2018-01-19. Selected parsed sections contain the literal STATE OF INCORPORATION in 15 pre2020 rows for nine CIKs. These are candidates, not verified original submission-header facts: completeness, state value, issuer identity and filing acceptance/publication clocks still require inspection. No complete-header cache or usable pre2018 header coverage has been established by this bounded assessment. No acquisition launched.

A primary EDGAR submission header containing STATE OF INCORPORATION, the issuer CIK and original acceptance timestamp is a feasible dated source after validating publication/revision semantics. One suitable pre-first-eligibility accession per cohort issuer implies a minimum 1,454 candidate header requests before cache reuse; state changes require more, so this is a lower bound rather than a fetch plan. Inspect the nine existing text candidates first; use cached archive accession discovery to locate earlier filings and bounded missing-header requests. Never assign a later filing state to earlier sessions. SEC header backfill should be separately authorized/reviewed and preserve strict cutoff and unresolved unknowns.

For 2021-22 excess, missing incorporation is near zero and the same FINRA classifier's zero suspects proves only internal consistency. Independent candidates are dated original SEC cover/security descriptions and prospectuses for NYSE listed classes, contemporaneous 814-N/814-NW and fund status, completed-SPAC versus retained SIC6770 chronology, foreign domicile/FPI filing history, duplicate share classes, and dated exchange/link validity. French uses its documented count unit; company counts remain a diagnostic alternative. The aggregate benchmark has no constituent identities, so these hypotheses are not established false inclusions. Preserve French FAIL pending controller ruling and independently measured remediation.

Guard commands (all from atx-db with PYTHONPATH=src): `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/SCRIPT`. Vendor coverage: CAP=0.8, SCRIPT=t2_vendor_coverage.py, receipt 20261003T163345-14836, peak 0.0738 GiB, 2 seconds. Header cohort/schema/hash: CAP=0.9, SCRIPT=t2_header_assessment.py, receipt 20261003T163832-2172, peak 0.8454 GiB, 13 seconds. Header text-cache: CAP=0.6, SCRIPT=t2_header_cache_check.py, receipt 20261003T163926-16608, peak 0.2476 GiB, 14 seconds. All completed with exit 0 and no cap hit.

All four published manifest LF-normalized code fingerprints match current modules (individual comparisons in t2-header-assessment.json). Owned directory disk bytes: universe 196,586,801; indexes 3,187,407; market 1,761,260,397 (includes inherited non-return market files). This is directory footprint, not new raw bytes. No foreign output modified.

## Fix round 2 — independently dated source repair, 2026-10-03

Implemented by Codex gpt-6.1-sol. Review-T2-r1 had closed I1/I2/I3/I5 and approved Task 2x; only I4 remained. This round repairs incorporation/class sources and remeasures without changing any accuracy or PIT threshold. Final result is **DONE_WITH_CONCERNS: 4 PASS / 1 FAIL**; I4 remains open because French is still FAIL in 10/60 months, maximum absolute deviation 13.36%. No controller waiver is inferred. Task 2x market/index products and the frozen foreign panel were not edited or rebuilt this round. Approved aggregate-return products remain bounded through 2022; membership, flags and 30-field annual value/presence coverage still span 2020–2025 and output extends through the frozen panel's 2026 sessions.

### Implementation and dated source contract

Commits `02f6d78b1243d0fd31eda9e9a78b07152eadf6bf` (new target-bound original header/class modules, universe integration and fixtures) and `ee104b2e1621ec34ba13717836f0e2067f8bbcee` (verified source clocks, missing-state class identity and dated termination provenance) contain the functional repair. The final docs/evidence commit includes only an accurate universe rule docstring, hand judgments, standalone registry stubs/docs and reproducible evidence; its SHA is returned to the controller with completion. Rule is `universe-all-common-v3`. Standalone modules are issuer_history and issuer_classes; no foreign changes were adopted or edited; source modules call the existing SEC HTTP and archived-accession parser APIs.

The nine existing cached text candidates were inspected first (t2r2-cached-inspection.json): their header literals were prose, not complete original issuer-bound headers. Archived submissions.zip supplies accession discovery only, never snapshot incorporation. The incorporation plan chooses the last available corporate filing before first eligibility plus later dated observations. Original headers require exact accession, target CIK/company block and corporate form; state literals from reporting owners, agents, other companies and addresses do not bind. Ambiguous/mismatched blocks, noncorporate filings, conflicting states and unknown codes stay unknown. Incorporation codes use the EDGAR header namespace. Availability is max(actual acceptance, filing/change date end-of-day Eastern), converted to UTC; ambiguous/invalid clocks reject. No later redomicile is assigned backward. Fixtures cover these risks, strict cutoff/future appends, conditional status assertions and positive/negative events.

Missing incorporation and class-source identity are separate: a verified target corporate CIK/form/accession and reliable clock can date an independently target-bound primary tax assertion even when state remains unknown. Class assertions bind named issuer or unique target dei CIK/company definition; nonasserting, subsidiary, conditional, proposed, contradictory and unreadable status remains unknown, never false. Current termination is representable. `report_period_end` is DATE provenance and never sets availability. Seritage's acquired November quarterly says current REIT requirements ended, while describing an earlier tax effective date; false is first available **2022-11-10 04:59:59 UTC**, never January/March backdating. Earlier disclosure coverage is still missing. An explicit false overrides stale SIC/panel REIT exclusion only after the strict cutoff.

The complete financial candidate cohort comprises 285 admitted NYSE financial issuers and its complete 41-real-estate subset across all 60 2018–2022 benchmark month ends. Dated SIC selects inspection candidates; it never assigns tax status. Financial annual 2021/2022 and real-estate pre-eligibility/2018–2020 observations apply one rule to the cohort, without ticker/count-fit patches. Every positive-source issuer receives the same latest-2022-periodic follow-up; the closed positive cohort contains 11 CIKs. Independent risk-stratified audit and changed hand evidence are separate from French constituent attribution.

### Source acquisition, completeness and provenance

All acquisition used the existing sec_http user agent and shared limiter, plus persisted <=1 HTTP attempt/s and <=3,000 combined round attempts, counting retries/redirects before dispatch. Counter and receipts were never reset. Acquisition stopped at **2,998 attempts**, two remain unused; minimum recorded spacing is **1.0000403 seconds**. Initial header tranche acquired all 1,454 target pre-eligibility candidates. All 704 planned class observations have successful header/primary receipts; 40 annual boundaries unavailable in accession discovery remain inventoried. These facts do not imply exhaustive source history or class precision.

Issuer_history publishes **1,346 rows / 1,332 CIKs**, available 2017-10-04 through 2022-11-19 UTC; 123 initial headers lack usable target state and remain unknown, and 6,616 later plan observations remain unfetched. Both are complete explicit inventories in the source manifest and integrity artifact. Later class acquisitions contribute only their actual dated header facts. Issuer_classes publishes **653 incorporation rows / 286 CIKs**, plus **49 status rows / 11 CIKs** (48 positive, one false). Of 704 assessments, **655 class unknowns** remain (650 nonasserting disclosures, five issuer-binding failures). Unknown is not a non-REIT assertion. The status source clock horizon is **2017-02-22 through 2022-11-15 UTC**. Full 2018–latest output carries observed statuses with fallback; this does **not certify uninterrupted status or coverage of 2023–2026 termination/conversion**. Later source refresh, unknowns and intermediate conversions remain concrete obligations. Newly acquired 2024 hand sources are audit-only and never extend model event history.

Shared class/header landing: **2,159 successful header URLs and 704 primary URLs**, 4,420,767,098 decompressed source bytes, 285,017,073 disk bytes. Separate changed-hand landing: 77 successful primary URLs, 215,281,186 source bytes, 14,421,602 disk bytes. Shared ledger preserves **58 historical response_too_large receipts** from the initial 10 MB limit; the authorized 64 MB streaming/compressed retry succeeded for those URLs. No latest failed URL remains, but parse/binding/status unknowns remain. No DOM was materialized during fetch. C: free space was checked above 40 GiB before tranches capable of landing over 1 GiB; last pre-hand check recorded 66,453,626,880 bytes (61.89 GiB). No synthetic price/share data, licensed comparator or outside price acquisition was introduced.

### Final publication, verification and hand evidence

Final full universe build: guard receipt **20261003T190725-18928**, **399.4 seconds**, **0.6623 GiB peak**, exit 0, no cap hit. Current hand sample binds rule/seed/hash **652e3d884a29df032187d4082778dd272356284d3ab2a310b6f02016b08906e0**. All 64 affected common/REIT/foreign claims were manually rechecked against original pre-sample cover, identity and tax/status disclosures; 36 unchanged unaffected ADR/fund/SPAC claims retain earlier independent judgments. Pooled **97/100 PASS**; ADR **9/12** remains visible, every other stratum passes. REIT annual plus latest periodic context checks conversions; foreign annual older than 365 days gets latest periodic context (FRES's original 6-K confirms foreign-private-issuer reporting; its US office is not domicile). No classifier change inherits a stale hand PASS. Evidence: universe_hand_check.json, t2r2-hand-source-evidence.json and receipt-backed producers.

Guarded **t2r2_verify.py**, receipt **20261003T191420-13552**, **7.0 seconds / 0.1548 GiB**, exit 0, verifies **every one of nine published universe file SHAs and sizes**, exact file set, no partial files, unique keys, current code/input fingerprints, source output SHAs/sizes/keys/clocks, source plan/raw-ledger/network contracts, frozen panel identity, persistent rate/cap and zero aggregate-return output at/after 2023. This is file-level integrity, not merely a manifest hash. Both demonstrated independent-audit REIT errors are now excluded at their sampled dates. Stable snapshot was released under R9 before report finalization; no further source/code/output rebuild follows that release.

| Published manifest | SHA-256 |
|---|---|
| universe | 90e9ca70cec64838a96b5f7ffc0de88a28e1faf529b48b480c56f4b9d85d0c0e |
| issuer_history | 46c83410ee7ed899b2aac239335c97d9983a27346df990cc0b4719d98d6f8939 |
| issuer_classes | 35c906ba83c05bebc3fc9c4bd403a05393068f8379cfb08f95be5094c3d1958d |
| frozen panel, unchanged | c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4 |

Complete integrity evidence: **t2r2-after-verification.json**. Every year 2020–2025 has all 30 core fields measured under univ_all_common, univ_r3000p and panel member_equity; denominator cells and per-field coverage are in validation/universe.json and regenerated universe.md. Aggregate returns were not evaluated after 2022. Relevant offline suite: **77 passed in 8.78s**, using tests/test_issuer_history.py, test_issuer_classes.py, test_universe_stage.py, test_exchange_repair.py, test_alpha_panel_market.py, test_alpha_panel_indexes.py and test_alpha_panel_identity_v3.py with `-o addopts='' -q -n0`. R5's narrow before-aggregation return boundary regressions remain covered; Task 2x was not reopened.

### French residuals and independently measured causes

French improves from round 1's 27 outside months to 10, while early deficits become more pronounced after correct foreign-source exclusions. All 60 original monthly comparisons and exclusive class/source counters are preserved in t2r2-french-reconciliation.json and universe.md. The documented benchmark line-count unit is unchanged; company counts are diagnostic only. Same-classifier FINRA zero suspects is internal consistency, not independent class accuracy. Aggregate French data does not reveal benchmark constituent identities.

| Failed month | Our lines | French | Residual | Otherwise-eligible missing positive shares | Admitted unknown incorporation |
|---|---:|---:|---:|---:|---:|
| 2018-01 | 1064 | 1228 | -164 | 277 | 91 |
| 2018-02 | 1100 | 1226 | -126 | 233 | 95 |
| 2018-03 | 1102 | 1225 | -123 | 230 | 91 |
| 2018-04 | 1095 | 1224 | -129 | 215 | 92 |
| 2020-06 | 1086 | 1173 | -87 | 10 | 9 |
| 2022-01 | 1292 | 1230 | +62 | 18 | 0 |
| 2022-02 | 1293 | 1229 | +64 | 18 | 0 |
| 2022-03 | 1301 | 1228 | +73 | 16 | 0 |
| 2022-04 | 1301 | 1228 | +73 | 17 | 0 |
| 2022-05 | 1301 | 1228 | +73 | 12 | 0 |

Early 2018 Jan–Apr has 277/233/230/215 otherwise-eligible observed NYSE lines without positive shares, dropping to 16 in May, against deficits 164/126/123/129. This measures a substantial shares-source coverage contributor, not a vendor-price diagnosis or proof that all those lines belong to French. Incorporation unknowns remain 91/95/91/92 in admitted early-month lines, but their unknown state does not establish a residual direction. The correct new dated classifications cannot be undone to fit a count. Feasible next repair is primary dated historical shares/market-shares initialization in the owning source lane, with actual availability and independent evidence; this lane has not edited that foreign producer.

June 2020 recheck: 1155 NYSE common lines appear at both neighboring May29 and July31 month ends; **89 lack June30 panel, price-stage and original vendor bars** (zero independent bars found). Remaining deficit is **-87**. This is measured vendor-source continuity evidence, not exact French constituent overlap and not permission to synthesize prices. Existing Databento metadata starts in 2024 and actual landed dates in 2026; no existing authorized independent June 2020 price replacement was established. Vendor-derived prices_history has the same source, so is not independent. Future licensed/authorized independent price coverage may repair source completeness, but the true unmatched benchmark set remains unknown.

Jan–May2022 excess remains **+62/+64/+73/+73/+73**, with **zero admitted unknown incorporation**. A fixed deterministic November 2021/March 2022 risk-stratified draw has 33 stratum draws/28 unique line-dates; original primary covers/business/status disclosures demonstrate **two initial wrong admissions (HPP/AHH REITs)** and 26 cases without a demonstrated wrong admission. The full 285/41 cohort rule repairs independently evidenced statuses beyond those tickers; it was not fit to French count deficits. Its 650 nonasserting/five unbound observations are unknown, not negative class certification. Converted SPACs, dual common classes, subsidiaries/advisers and foreign/link cases were checked against original pre-date issuer disclosures; later historical descriptions were not backdated. This audit is neither a population precision estimate nor French overlap attribution. Other classes/statuses, missing SIC candidates, intermediary conversions and licensed line-level constituent coverage remain unresolved hypotheses.

Concrete remaining source work: dated shares initialization for the early cohort; an authorized independent June 2020 price source; complete issuer-bound class/status follow-up for unknown/missing candidates and later 2023–2026 observations; and independent dated class/link/listing or licensed constituent evidence for2022 excess. Older corporate header alternatives for 123 missing-state initial candidates can be investigated under a separately allocated budget, but none was launched after the required hand review. Identity share-code products were not refreshed from new tax assertions; code presence PASS is not semantic class certification. These limitations preserve **French FAIL and I4 open** until real source repair or a concrete controller ruling.

### Commands and execution history

All real jobs run one at a time in this lane under <=1 GiB native-job guard, with `PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`, from C:/atx/atx-db. General wrapper: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 -- .venv/Scripts/python.exe COMMAND`. Source prepare/fetch/load commands and exact argv/caps/receipts are in t2r2-guard-receipts.json; docs show standalone commands and rate/cadence/clock/partial coverage. Main final load: issuer_history receipt 183853-13588,4s/0.1048GiB; issuer_classes183858-17868,160.1s/0.1709GiB. French reconciliation190124-12156,79.1s/0.5937GiB; changed-hand acquisition185945-8524,85.1s/0.1069GiB, followed by successful receipt reuse and one required older-annual supplement at190435-21316 (22s/0.1029GiB). Vendor recheck191510-17584,1s/0.0743GiB; final closed-cohort status original recheck191529-20180,2s/0.0853GiB. No return values at/after 2023 were inspected.

Failed/stopped/superseded execution is retained, not concealed. Initial nonstreaming zip inspection164947-20392 hit 0.6034 GiB against 0.6 GiB and failed; streaming rerun passed. Slow inherited ZIP-directory preparation165404-18944 was stopped after 90.1s (0.2156GiB), then streaming indexed iteration completed. Three independent-audit preparation attempts170037-3476/170142-19792/170204-18016 failed querying inherited sec_text sections (invalid data); producer stopped depending on those foreign sections and original-primary audit succeeded, without foreign edits. Initial 10 MB class fetch174230-9008 was deliberately stopped after652.5s (0.0753GiB) to resume authorized 64 MB streaming. Final full-source class reload supersedes intermediate loaders, including the loader active while clock-only parser work was completed; no intermediate code fingerprint is promoted as final evidence. Intermediate full universe build181525-5248 and source-incomplete build183605-19348 (stopped 125.1s/0.5076GiB) were superseded; partial-year writes were disclosed and consumers held until R9 file integrity. Source-closed build185341-20224 (353.3s/0.6561GiB) still had stale hand judgments and inherited inaccurate rule docstring; final 399.4s build replaces it. Audit-detail190650-6416 wrote evidence but failed printing Unicode to the Windows console (1s/0.0757GiB); print-only producer correction reran191532-19004 successfully (1s/0.0751GiB). Counter, successful raw receipts and old oversized receipts were never deleted or repeated silently.

Owned paths only: new issuer source modules/tests (committed), universe integration/docstring/tests and hand JSON, issuer/universe docs/standalone stubs, this report and t2r2 evidence/producers. No progress.md, shared registry, foreign panel, market/index return product or foreign code edited. Reports retain historical round1 provenance, including the inherited --end return-computation violation and its R5 correction in Task2x. Final publication stays quiescent for downstream coverage refresh.
# Fix round 3 operational checkpoint — publication pending

Actual implementer: GPT-6.1 Sol. This remains fix/review round 3 of 5. Source corrections are committed as `28ec2918` (R2-1 retained acquisition vintages and R2-2 conditional assertions) and `386378e5` (completed elections with future maintenance clauses). Offline coverage includes plan→load→universe transition preservation, genuine positives/terminations, pending elections and pending terminations; no count fitting or gate relaxation.

Canonical source replay stopped twice under the unchanged 0.75 GiB native guard: `20261003T203238-11108`, exit 137 after 384.0 seconds, peak 0.2054 GiB; and `20261003T204159-12244`, exit 137 after 407.3 seconds, peak 0.1692 GiB. Both were low host commit headroom stops, not process cap violations. Receipts are preserved in `t2r3-guard-receipts.json`. History published manifest `0a2cf97e9d00ee0c152742b9674b125e8c40b2ca757dac1a913ec3cb652d3f14`; the class phase and post-second-stop files are not certified. Old universe files retain the round-2 bytes, but their source snapshot is stale: R9 and descendants stay on hold. No additional retry or real-data cache run is authorized before independent scoped review and controller slot allocation.

The optional task-scratch assertion cache binds full plan-row identity, issuer name, header/primary receipt status/error/hash/size, exact retained compressed-or-raw object hashes and sizes, LF-normalized parser/replay/clock/common/source-store code, Python, rule and maximum read limit. Headers and original clocks are reconstructed afresh. Cache records have identity and typed assertion checksums; stale/corrupt entries recompute from verified originals. Loader publication requires one assessment per unique planned key, unchanged plan/ledger/code and revalidated completed source identities. Interrupted parsing publishes no source outputs; a fresh loader resumes only remaining expensive assertions. Ordinary uncached semantics are retained and synthetic cached/uncached Parquet and assessment bytes are compared.

Offline tests cover interruption/fresh-loader resume, changed plan/code/name/receipt/header/primary, invalid JSON/checksum/type/identity, corrupt retained bytes, source/code mutation before publication, concurrent plan/ledger changes, duplicate keys and missing-header unknown accounting. The targeted 76-test source/universe suite passed in 14.46 seconds. The broader suite of replay/history/classes/universe/market/market-returns/index/identity tests passed 114 tests in 14.32 seconds (`pytest -n 0`); all fixtures are offline and no real-data replay was launched.

The 704-original intermediate replay (`t2r3-intermediate-parser-revalidation.json`, 343.7 seconds) found two valid completed elections lost by an overbroad future-maintenance filter; `386378e5` fixes those using original-text regression fixtures. It is intermediate evidence, not final corrected status certification. Final canonical replay must expressly supersede it with exact original hashes and code identity; stale earlier zero-change evidence must not be reused. SEC remains closed at 2,998 attempts; Nareit remains 11 attempts, with no HTTP in this operational correction. Six retained Nareit PDFs were SHA-verified and cover-rendered under guard: Jan–June 2022 editions describe December 2021–May 2022 observation dates; publication clocks remain unknown. Index names are candidate/audit evidence, not automatic issuer tax status or exhaustive French constituents. French FAIL/I4 remain open; no return values or aggregate return products at/after 2023 were inspected.
# Fix round 4 of 5 — scoped engineering checkpoint

Actual implementer GPT-6.1 Sol; addresses both Important findings in `review-T2-r3-prebuild-findings.md`. No real-data/HTTP job was run. Canonical sources, universe/hand measurements and all final gates remain pending; the stale source snapshot/R9 descendant hold and French FAIL/I4 remain unchanged.

R3-1: guarded replay captures an immutable complete plan-row/receipt/retained-object identity before the first header read, clock/name reconstruction or primary read. Receipt identity includes retrieval metadata when present. A `finally` check verifies the captured identity after every row's consumption, including missing/unbound headers and primary-read errors. Before publication, all captured planned assessments are verified again, independently of the set of successful assertion checkpoints; thus prior Unknown/failure sources cannot change unnoticed while later rows run. Regression fixtures reproduce replacement immediately after a verified header read, between clock reconstruction and assertion, and replacement of earlier Unknown/primary-failure source bytes during later-row processing. Each refuses publication of either Parquet or the final manifest. Existing interruption/resume and cached/uncached byte-equivalence tests remain.

R3-2: the initial-election trailing prospective qualifier is bounded by the actual sentence instead of an arbitrary 180-character window. A completed election followed by a separate future-termination sentence stays True. The loader/universe fixture proves the current positive, no premature pending-termination event/transition, and a later completed termination applied only after its own verified acceptance/availability clock. Pending initial elections and conditional positives/terminations remain Unknown; no historical effective date is backdated.

Targeted source/universe regressions passed 82 tests in 28.94 seconds before the expanded completed-termination transition assertion. Final broader replay/history/classes/universe/market/market-returns/index/identity regression suite passed 120 tests in 41.72 seconds (`pytest -n 0`), including the expanded termination transition and retrieval metadata binding. This checkpoint requires independent scoped review before any replay data job; review count is 4/5, not reset.
