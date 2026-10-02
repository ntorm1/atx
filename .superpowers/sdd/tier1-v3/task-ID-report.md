# Lane ID report (tier1-v3 S2.2-S2.6: security master and symbology)

## Outcome

PARTIAL (owner stop, 2026-09-30 ~01:20Z). S2.2 done and passing. S2.3 built (pre-OpenFIGI) with ISIN check 100 %
and 13F coverage passing 28 of 30 quarters 2019-2026. S2.5 link table v3 built, 0 ambiguous, 2018 passes; 2019-2025
PIT sits at 93.8-94.9 % against 95 % because the cover-page evidence (NOTES `COVER READY`) has not been published
yet. S2.4 / S2.6 outputs (figi, line_figi, lei, hierarchy) NOT RUN: the OpenFIGI landing was still in flight and
the LEI build depends on it. Branch `feat/tier1-v3-warehouse`.

## Commits

| SHA | content |
|---|---|
| 9ec47c44 | S2.2 `listing_events.py` + `test_alpha_panel_security_listing.py` |
| 0c809b5f | S2.3 `cusip_history.py` + `test_alpha_panel_security_cusip_history.py` |
| fe3c2a65 | S2.4/S2.6 `figi_lei.py` + `test_alpha_panel_security_identifiers.py` |
| 9ab3b686 | S2.5 `identity_v3.py` + `test_alpha_panel_identity_v3.py` |
| (last commit) | S2.3 per-row option-title fix in the equity-only 13F measure, `codes()` fixture test, doc, this report |

## Done criteria

| task | criterion | result | measured | how |
|---|---|---|---|---|
| S2.2 | listing date known (not censored) >= 95 % of lines first seen after 2012 | PASS | 18,008 / 18,660 = 96.51 % (member lines 98.54 %, linked lines 99.85 %) | `listing_events build` receipt `line_listing.done_measure` in `security_master/listing_events_manifest.json` |
| S2.3 | >= 99.5 % of 13F SH value per quarter maps to a line | PASS 2019-2026 except 2 quarters; FAIL 2013-2015 | history_all: 2019Q1-2026Q2 28/30 quarters >= 99.5 % (min 99.556 %); fails 2021Q2 98.97 % (one filer's bogus `00507V1xx` codes), 2022Q3 99.46 %; 2016-2018 8/12; 2013-2015 97.7-98.3 % (0/12). Pre-OpenFIGI numbers (figi.parquet not built) | `cusip_history build` -> `measure()`; `security_master/cusip_history_manifest.json` receipt `thirteenf_coverage` |
| S2.3 | ISIN check digits valid 100 % (tests with known ISINs) | PASS | 38,402 / 38,402 derived ISINs valid; tests US0378331005, US0846707026, CA82509L1076, GB0002634946, ... | manifest `isin_check_valid`; `tests/test_alpha_panel_security_identifiers.py` |
| S2.4 | FIGI >= 98 % of member lines | NOT RUN | OpenFIGI `cusip` pass at 2,900 / 3,082 requests (0 errors) when stopped; figi-build / figi-lines not run | - |
| S2.4 | LEI >= 90 % of linked CIKs (or measured rate + why) | NOT RUN | GLEIF LEI2 / RR / ISIN-LEI landed and parsed; `lei-build` waits on cusip_history with OpenFIGI | - |
| S2.5 | PIT (strict+dated+name) >= 95 % of member_equity cells every year 2019-2026 | FAIL (pending cover evidence) | 2019 94.56, 2020 94.32, 2021 93.81, 2022 94.53, 2023 94.61, 2024 94.86, 2025 94.71, 2026 95.91 % | `identity_v3 table` -> `identity/audit_v3.json`, `link_table_v3_manifest.json` receipt `audit.per_year.*.share.pit_strict_dated_name` |
| S2.5 | >= 90 % in 2018 | PASS | 94.32 % | same |
| S2.5 | 0 ambiguous line-days | PASS | 0 | receipt `ambiguous_line_days` |
| S2.5 | dated-tier precision (not a brief criterion) | - | agrees with strict CIK on 99.97-100 % of overlapping line-days per year, with the snapshot backfill CIK on 99.50-99.98 % | receipt `dated_agreement` |
| S2.5 | shrcd/exchcd dated histories | NOT RUN | `codes()` written and fixture-tested; the guarded job was queued (see "Processes still alive") | - |
| S2.6 | hierarchy from GLEIF Level 2 | NOT RUN | code in `figi_lei._hierarchy`, runs inside `lei-build` | - |

Gap analysis for S2.5 (guarded, 2019-2025 member_equity cells, 4,054,230): missed = 146,240 backfill + 77,613
unlinked cells on 580 lines. Top misses are foreign private issuers (Philips, ASML, ICICI, BP, FEMSA, CGI, JD.com,
NICE ...): they file no Form 3/4/5, so only cover-page evidence (20-F / 40-F / 10-K dei:TradingSymbol) can date
them; 703 backfill cells have weight-1 evidence only. Bank OZK (bank without an SEC registrant) cannot link.

## Built

| path (under `data/alpha_panel/v1/`) | rows | bytes | sha256 |
|---|---|---|---|
| `security_master/listing_events.parquet` | 503,164 | 7,119,068 | 5e2f3c23a3d716556cf16293dae8b85b0bdcd9f1b5b35f4c7c74eba439cb8965 |
| `security_master/line_listing.parquet` | 25,760 | 616,662 | 54c6b480c7b262ac60263f89a68d01c9785a4d3b897b853274da5307de5fe629 |
| `security_master/name_history.parquet` | 129,366 | 1,795,297 | 41e75d8684420def136088d30e2894d2d4a8c2984812758786b5cb836ab57831 |
| `security_master/filer_addresses.parquet` | 98,490 | 3,960,981 | 2111b1cffcd4158033f1c374e4338aa96fbb7b5ea1ddc4d6774c8364ed450d62 |
| `security_master/listing_events_manifest.json` | - | - | - |
| `security_master/cusip_history.parquet` (pre-OpenFIGI) | 58,849 (45,523 CUSIPs, 25,047 lines) | 1,656,028 | 9f921f14c7cac13d2c92fc77764f9551124f3c764304720ab5dedde5cb8112eb |
| `security_master/cusip_history_manifest.json` | - | - | - |
| `identity/link_table_v3.parquet` | 184,694 (12,163 lines, 9,279 CIKs) | 1,379,818 | eeccd7a5e08a8a29f81f8bcfa450f62fd3bd894952533d76f8fc7edda92c6cf1 |
| `identity/link_table_v3_manifest.json`, `identity/audit_v3.json` | - | - | - |
| `export/identity-bridge-v3-strict/links.parquet` | 16,494 | 145,430 | e1d5fa4443e1a7f477239ed53b20128594239c2c34f594d9919d574d8c764e3e |
| `export/identity-bridge-v3-pit/links.parquet` | 173,266 | 1,098,501 | 82a5442308f8c5ac6c7b46e5cad87d3a4c628fae4011a24da4e58b71c8b592ee |
| `export/identity-bridge-v3-all/links.parquet` | 184,694 | 1,205,988 | fc4c73d7f58ca9eb61fb44fe8c2f6acc619b93e74d133948900d43ddf0ec8d8e |

Link table v3 by tier (rows / lines / line-sessions): strict 16,494 / 5,915 / 7.93 M; dated 23,012 / 4,245 /
2.03 M; name 133,760 / 5,187 / 2.03 M; backfill 11,428 / 2,209 / 0.57 M. linktype/linkprim: LC/P 30,977, LC/J
2,107, LC/N 2,364, LU/P 70,997, LU/J 24,931, LU/N 53,318. Dated tier: 9,490,108 line-days 2018-2026, 56,181
with more than one candidate CIK. No existing file in `identity/` or `security_master/` was overwritten.

Work files: `_tmp/identity_v3/` (evidence 2,148,814 Form 3/4/5 rows, name_days_full 9,714,517, dated_parts/, 13F
per-quarter CUSIP values, FTD runs 31,709).

## Sources

| source | url | bytes landed | span | cadence | terms | rate limit |
|---|---|---|---|---|---|---|
| GLEIF LEI2 golden copy (2026-09-29 16:00) | goldencopy.gleif.org/storage/golden-copy-files/2026/09/29/1282430/...lei2-golden-copy.csv.zip | 506,095,931 (zip deleted after parse; parsed 340,673,986; 3,446,215 records) | snapshot | 3x daily | CC0 1.0 | none documented; 1 sequential download |
| GLEIF RR (Level 2) golden copy | .../1282475/...rr-golden-copy.csv.zip | 24,410,280 (deleted; parsed 14,670,661; 489,389 records) | snapshot | 3x daily | CC0 1.0 | same |
| GLEIF ISIN-LEI mapping | mapping.gleif.org/api/v2/isin-lei/latest (Accept: application/vnd.api+json) | 32,241,923 (deleted; parsed 39,245,804; 9,159,139 rows) | snapshot | daily | CC0 1.0 (GLEIF/ANNA) | same |
| OpenFIGI mapping API v3 | api.openfigi.com/v3/mapping | ~12 MB responses + receipts (2,966 requests at stop, 0 errors) | snapshot | on demand | OpenFIGI terms of use (free, unauthenticated) | 25 requests/min, 10 jobs/request (unauthenticated); client spaces 2.6 s and honours ratelimit headers / 429 |
| SEC submissions.zip (read in place) + sec_filings stage | existing cache | 0 new | 1994-2026 | - | SEC public data | not fetched |

Receipts: `data/raw/gleif/receipts.jsonl`, `data/raw/openfigi/receipts.jsonl` (per request: url, bytes, sha256,
http_status, fetched_at). Landings resumable (OpenFIGI job keys ledger; GLEIF by sha256 / status).

## Disk used

`data/raw/gleif` 377 MB, `data/raw/openfigi` 12 MB, `_tmp/identity_v3` 127 MB, stage outputs ~17 MB. Peak transient
+506 MB (LEI2 zip, deleted after parse). C: free > 50 GB throughout.

## Memory guard / C-1

- Every DuckDB job ran guarded (0.3-0.45 GiB caps). Guard peaks: dated 0.32 GiB, table 0.37 GiB (412.8 s), cusip
  build 0.33 GiB, gleif parse 0.17 GiB, gap analysis 0.14 GiB. Many attempts were stopped by host low commit
  (stop_reason low_commit); rerun through a requeue-on-137 wrapper; `dated()` made resumable per year.
- Unguarded per C-1: OpenFIGI fetch (`figi-run-early`, network + pyarrow plan; peak printed at its end in
  `_logs/id_figi_early.log`), GLEIF fetch (peak WS 0.045 GiB, commit 0.027 GiB).
- C-1 overshoots (before the switch to guarded analysis): three exploratory pyarrow reads ran unguarded with peaks
  commit 1.34 GiB / WS 0.85 GiB, commit 0.60 GiB, commit 0.54 GiB / WS 0.09 GiB; all later analysis ran guarded.

## Processes still alive at hand-back (I could not stop them: process kill denied by the permission classifier)

| PID | what | next action if left alone |
|---|---|---|
| 16544 (parents 18652, 10676) | OpenFIGI `figi-run-early` (cusip pass, network only, unguarded per C-1) | finishes ~180 remaining requests (~8 min), exits |
| 7456 | `scratchpad/figi_next.sh` | after 16544 ends: starts OpenFIGI `cusip` + `isin_fragment` fetch passes (network, unguarded, ~1 h) |
| 19252 | `scratchpad/figi_chain.sh` | after figi_next: guarded `figi-plan-followups`, unguarded fetch `cusip_any` + `ticker`, then guarded `figi-build`, `cusip_history build`, `lei-build`, `figi-lines` (0.4 GiB each) |
| 18780 / 22352, guard 3372 -> 22904 | `cusip_history collect` (in flight, per-quarter parts resumable) | finishes; requeues itself up to 6 times on a low-commit stop |
| 24328 / 24944, guard 27652 (waiting in FIFO) | `identity_v3 codes` | runs when admitted (0.4 GiB), writes `security_master/share_exchange_history.parquet` + manifest |

To honour the owner stop fully, kill 7456, 19252, 18780, 22352, 24328, 24944 and guard 27652 (all mine; the scratchpad
`own/retry_guarded.sh` loops belong to lane OWN). Everything is resumable.

## Resume instructions

Prefix every DuckDB step with
`cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1 && .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.4 --wait-minutes 600 --`.

1. OpenFIGI (network, unguarded per C-1; each pass resumes from the response ledger):
   `python -m atx_db.alpha_panel.figi_lei figi-fetch --pass cusip`, then `--pass isin_fragment`;
   guarded `python -m atx_db.alpha_panel.figi_lei figi-plan-followups`; unguarded `figi-fetch --pass cusip_any`,
   `--pass ticker`; guarded `figi-build`.
2. Guarded `python -m atx_db.alpha_panel.cusip_history collect` (only if `_logs/id_cusip_collect5.log` lacks
   `### final rc=0`), then guarded `python -m atx_db.alpha_panel.cusip_history build` (build + 13F measure + manifest).
3. Guarded `python -m atx_db.alpha_panel.figi_lei lei-build` (lei.parquet, hierarchy.parquet, lei_manifest.json)
   and `figi-lines` (line_figi.parquet, figi_manifest.json with member-line coverage).
4. When `COVER READY` appears in `.superpowers/sdd/tier1-v3/signals.md`: guarded
   `python -m atx_db.alpha_panel.identity_v3 evidence`, `dated`, `table` (`--job-gb 0.45`), then rerun `lei-build`
   (its coverage is measured against the link table in place).
5. Guarded `python -m atx_db.alpha_panel.identity_v3 codes` (if the queued run did not happen).
6. `.venv/Scripts/python.exe -m pytest -n 0 -q tests/test_alpha_panel_identity_v3.py tests/test_alpha_panel_security_identifiers.py tests/test_alpha_panel_security_cusip_history.py tests/test_alpha_panel_security_listing.py`
   (last green run: all four files after the final code change of each).

## Deviations and open issues

- S2.5 PIT < 95 % in 2019-2025 until cover-page evidence lands; rule and co-registrant handling are fixture-tested
  against the NOTES schema (`symbol_norm`, `coreg`, `entity_cik`, `is_equity_like`).
- S2.3: 13F 2013-2015 at ~98 % and 2021Q2 / 2022Q3 below 99.5 % before OpenFIGI; OpenFIGI `cusip` / `isin_fragment`
  answers are expected to close part of it (snapshot, `vintage_risk = snapshot_non_pit`). The equity-only
  diagnostic in the current cusip_history manifest is wrong (a CUSIP was dropped whole when any filer titled a row
  PUT/CALL); fixed per row in code, needs the collect + build rerun. Form 4 / 13D-G CUSIPs not used: no stage
  carries a parsed subject CUSIP.
- Listing events cannot key lines (no ticker on 8-A / 25): used for listing dates and as LC corroboration only.
- LEI2 was parsed by the earlier csv-module parser; `csv_zip_to_parquet` now streams with Arrow's CSV reader
  (quoted newlines, BOM), fixture-tested only.
- `names()` reads `security_master/finra_names.parquet` (FINRA raw CSVs OOM'd under 0.4 GiB); share class now also
  from that file.

## Ledger candidates

- DuckDB `list(DISTINCT source)` in a 2 M-group aggregate OOMs at 210 MB; a TINYINT bit mask + `bit_or` fits.
- 13F `bool_or(title IN ('PUT','CALL'))` per CUSIP flags most large-cap CUSIPs (one mis-typed filer row suffices):
  measure option titles per row.
- Foreign private issuers file no Form 3/4/5: Form 4 `issuerTradingSymbol` alone tops out near 94-95 % PIT of
  member_equity cells; the rest needs cover-page (20-F / 40-F) symbols.
