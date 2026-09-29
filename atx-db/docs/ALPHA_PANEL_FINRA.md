# Alpha panel: FINRA stages S (short interest) and V (daily short volume)

These are the stage S and stage V producers of the alpha panel (`docs/ALPHA_PANEL.md`). Code:

* `src/atx_db/alpha_panel/short_interest.py`
* `src/atx_db/alpha_panel/short_volume.py`
* `src/atx_db/alpha_panel/finra_fetch.py`: polite downloader, the vendor ticker map, and the securityID-0 repair.

Build root: `atx-db/data/alpha_panel/v1` (override with `ATX_ALPHA_PANEL_ROOT`).

## Progress

| Milestone | State (2026-09-27) |
| --- | --- |
| CNMS download 2018-08-01 .. 2026-09-18 | done: 2,123 weekdays tried, 2,044 files (every session), 79 HTTP 403 (all exchange holidays), 222 MB gz |
| SI fetch of new settlements | done: API partitions list ends 2026-09-15. CDN `shrt20260915.csv` returned 403, so the file was taken from the public Query API (22,595 rows) |
| Stage S build + validation | done: 2,122,264 rows. Exact match vs the earlier as-of CSVs is 99.961 % (si_shares) and 99.966 % (si_dtc) |
| Stage V build | done: 19,583,883 rows, 2018-08-01 .. 2026-09-18, mapped 94-95 % of rows and 95-99.8 % of volume |
| Shared securityID-0 repair | agreed with the controller; identical to stage P for 2018+ (95,423 of 95,423 repaired rows equal) |

## Commands

Run every command under the memory guard. Rows marked "network" make sequential public-FINRA requests
spaced at least 0.6 s apart, using User-Agent `atx-research/0.1 (nathan.tormaschy2@gmail.com)`.

```
set G=C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py --wait-minutes 30 --quiet
set PYTHONPATH=C:\atx\atx-db\src
%G% --job-gb 0.3 -- C:\atx\atx-db\.venv\Scripts\python.exe -m atx_db.alpha_panel.short_volume download    & rem network, resumable
%G% --job-gb 0.3 -- C:\atx\atx-db\.venv\Scripts\python.exe -m atx_db.alpha_panel.short_interest fetch     & rem network, resumable
%G% --job-gb 0.8 -- C:\atx\atx-db\.venv\Scripts\python.exe -m atx_db.alpha_panel.short_interest build
%G% --job-gb 0.8 -- C:\atx\atx-db\.venv\Scripts\python.exe -m atx_db.alpha_panel.short_interest validate
%G% --job-gb 0.8 -- C:\atx\atx-db\.venv\Scripts\python.exe -m atx_db.alpha_panel.short_volume build
```

DuckDB runs with `memory_limit` 420MB (`ATX_FINRA_DUCKDB_MEMORY`), two threads, and spill under
`_tmp/finra_*`. Measured guard peaks: SI build 0.57-0.60 GiB, SI validate 0.43 GiB, SV build
0.61-0.63 GiB, downloads 0.04 GiB. Wall times: SI build 69-80 s, validate about 150 s, SV build
136-184 s, CNMS download 1,275 s.

Every step is resumable:

* Downloads skip manifest dates already landed.
* Parsed raw files are cached by sha256.
* Vendor extracts are cached by TickerHistory3 identity, rule and dates.
* One guard stop (exit 137, host low-commit) was re-run with no loss.

## Landings (new, gzip at rest)

`atx-db/data/raw/finra_short_volume/`:

* `CNMSshvolYYYYMMDD.txt.gz` files.
* `manifest.csv` with columns `date,file,bytes,sha256_of_raw_bytes,rows,url,http_status,downloaded_at`.

`atx-db/data/raw/finra_short_interest/`:

* `si_api_20260915.csv.gz`, the Query API CSV exactly as served: header once, pages sorted by `symbolCode,marketClassCode`.
* A snapshot of the partitions API.
* `manifest.csv` with the same columns.

About the manifests:

* Both are append-only: the last row per date wins, and 403 attempts are kept.
* `sha256_of_raw_bytes` is the sha256 of the uncompressed bytes.
* gzip uses `mtime=0`, so the stored bytes are reproducible.

The existing landing `C:\atx\data\finra_short_interest` is read-only input; the sha256 of every file is
verified against its manifest on each build. The existing `C:\atx\data\finra_short_volume` holds
only 2012-2016 per-facility files and is not used.

Query API vs CDN check: settlement 2026-08-31 was fetched through the API and compared with the CDN file.
The API returned 22,569 rows against 22,569 in the CDN file, with the same keys and 0 differences in
current, previous, ADV, DTC, revision and split.

## Shared rules

### Symbol canonical form

FINRA short interest writes class shares without punctuation (`BRKB`, `BFB`, `GEFB`, `BFHPRA`).
CNMS writes a slash (`BRK/B`, `AIG/WS`). ORATS `ticker_tk` writes a dot (`BRK.B`, `BFH.PRA`, `AAC.U`).

The canonical form strips `.`, `/`, `-` and whitespace. It is case-sensitive: CNMS and ORATS mark
preferreds, rights and when-issued lines with lower-case letters (`TpC`, `SRVr`, `ABRpA`).
Upper-casing made `TpC` (AT&T preferred C) collide with `TPC` (Tutor Perini) and dropped Tutor
Perini's short volume. The first V build had 12,510 collision rows; after the fix there are 56.

### securityID 0 repair (`sid0-bracketed-ticker-v2`)

TickerHistory3 files 40-80 real lines a day under `securityID = 0`. MSFT is one of them on most
sessions from 2025-11-24 to 2026-08. The repair, agreed with the controller and identical to stage P,
reassigns a sid-0 row `(d, ticker_tk)` to id `S` only when all four conditions hold:

1. It is the only sid-0 row with that ticker on `d`.
2. The nearest non-zero row with that exact ticker strictly before `d`, and the nearest one strictly after, both have id `S`. A neighbour date that carries two ids fails.
3. Each bracket row is within 400 calendar days of `d`.
4. `S` has no row of its own on `d`.

Results on 223,542 sid-0 (date, ticker) pairs since 2017-10-01:

| basis | pairs |
| --- | --- |
| bracketed | 97,912 |
| ticker_never_nonzero | 68,326 |
| next_only | 38,804 |
| prev_only | 10,494 |
| bracket_too_far | 6,639 |
| bracket_disagrees | 1,357 |
| id_has_own_row | 10 |

The later bracket row is dated after `d`. This makes it an identity repair of the vendor file, not a
point-in-time decision.

Checked against `prices/` (`sid0_repaired`): the 95,423 repaired prices rows equal my 2018+
bracketed set. My extra 2,489 rows are all in 2017, which is before the prices window.

## Stage S: `short_interest/si.parquet`

One row per `(security_id, settlement_date)`, sorted by settlement then id.

| column | meaning |
| --- | --- |
| `security_id` BIGINT | ORATS securityID |
| `symbol` | FINRA `symbolCode` |
| `settlement_date`, `dissemination_date` DATE | settlement; official FINRA publication date (the exchange receipt date up to the 2020-12-31 cycle) |
| `si_shares`, `si_prev` DOUBLE | `currentShortPositionQuantity`, `previousShortPositionQuantity` |
| `si_dtc` DOUBLE | `daysToCoverQuantity`; NULL when `adv_finra = 0` (FINRA then writes 999.99; 4,113 mapped rows) |
| `adv_finra` DOUBLE | `averageDailyVolumeQuantity` |
| `revision_flag`, `split_flag`, `market_class` | `R`/NULL, `S`/NULL, `marketClassCode` |
| provenance | `dissemination_source` (`official` for all rows), `map_basis` (`exact` 1,992,333 / `canonical` 118,487 / `sid0_repaired` 11,444), `vendor_date`, `vendor_date_fallback`, `source_file` |

`short_interest/mapping_audit.parquet` keeps every FINRA row after duplicate resolution, with its
outcome (`mapped`, `unmapped`, `ambiguous`, `collision_*`). `validation.json` holds the full comparison.

### Rule `si-ticker-asof-settlement-v3`

Mapping:

* The map date is the last session (vendor date with at least 1,000 rows) on or before the settlement.
* A FINRA symbol maps to the single ORATS id whose ticker has the same canonical form that day. Two or more ids is ambiguous and the row stays unmapped; this never occurs.
* Fallback: TickerHistory3 omits thinly traded lines on some sessions. When no row carries the form on the map date, the latest earlier session within 7 calendar days of the settlement is used (`vendor_date_fallback`). This adds 55,230 SI rows (2.6 %), mostly warrants, units, preferreds and small ETFs; the gap to the settlement is 1 day for 46 % of them and at most 7 days for all. The fallback reads only dates on or before the settlement.

Duplicates:

* Duplicate `(settlement, symbol)` rows resolve by exchange class before OTC, then the earliest landing, then the last line. There are 0 such rows.
* When two symbols map to one id, the exchange-class pool decides: 16 were resolved by a single exact match and 28 rows were dropped.

Dissemination and visibility:

* All 210 settlements are in the official schedule. The fallback of settlement + 7 NYSE business days is implemented but was never used.
* The contract makes a row visible on sessions strictly after `dissemination_date`.

Class shares:

* FINRA is `BRKB`, ORATS is `BRK.B`, and they match through the canonical form.
* Preferreds are written differently by the two sources (FINRA `BFHPRA` vs ORATS `BFH.PRA` or `ABRpA`). The dotted ORATS form matches; the `p` form does not. Preferreds are outside the equity universe.

### Coverage and mapping rate

Settlements run from 2017-12-29 to 2026-09-15 (210). The last dissemination is 2026-09-24. Within
the vendor window, the last usable row is settlement 2026-08-31, disseminated 2026-09-10, which is
visible from 2026-09-11.

| settlement year | FINRA rows | exchange rows | mapped | mapped / exchange rows | output ids | si_dtc NULL | si_dtc = 1.00 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2017 | 15,495 | 8,326 | 8,185 | 0.9829 | 8,185 | 1 | 2,869 |
| 2018 | 375,548 | 201,707 | 195,971 | 0.9715 | 9,311 | 69 | 67,028 |
| 2019 | 385,795 | 204,809 | 202,121 | 0.9868 | 9,442 | 111 | 70,332 |
| 2020 | 414,562 | 211,352 | 208,952 | 0.9886 | 10,184 | 81 | 83,955 |
| 2021 | 478,429 | 252,681 | 250,884 | 0.9929 | 12,570 | 33 | 105,344 |
| 2022 | 496,296 | 281,175 | 272,529 | 0.9692 | 12,887 | 1,217 | 108,604 |
| 2023 | 470,207 | 267,486 | 255,163 | 0.9538 | 12,417 | 1,849 | 96,078 |
| 2024 | 468,759 | 261,150 | 251,524 | 0.9630 | 12,112 | 127 | 104,012 |
| 2025 | 493,428 | 274,422 | 268,861 | 0.9796 | 12,989 | 198 | 117,508 |
| 2026 | 373,233 | 212,792 | 208,074 | 0.9777 | 13,802 | 46 | 90,092 |

Output total: 2,122,264 rows and 22,528 ids. The "mapped" column counts OTC rows too, but only
about 210 of them map. FINRA floors days-to-cover at 1.00, so about 40 % of rows equal 1.00.

| market class | FINRA rows | mapped | share | share for settlements >= 2020-09-15 |
| --- | --- | --- | --- | --- |
| NYSE | 659,364 | 645,727 | 0.9793 | 0.9821 |
| NNM (Nasdaq GS/GM) | 618,294 | 603,207 | 0.9756 | 0.9700 |
| SC (Nasdaq CM) | 318,322 | 296,283 | 0.9308 | 0.9228 |
| ARCA | 390,486 | 389,482 | 0.9974 | 0.9994 |
| BZX | 123,919 | 123,481 | 0.9965 | 0.9975 |
| AMEX | 65,491 | 63,850 | 0.9749 | 0.9729 |
| IEX | 24 | 24 | 1.0000 | - |
| OTC | 1,795,672 | 210 | 0.0001 | 0.0001 |
| OTCBB | 180 | 0 | 0 | - |

Unmapped exchange symbols with the largest ADV are sub-dollar Nasdaq CM names at dates when ORATS
carries no row for them: BETS, HCTI, GWAV, DMN, GNLN, LGMK, INLF, ELAB.

Coverage of the scorecard-like universe: take the top 3,000 names by dollar volume with close above
$5 on each session from 2020-09-28. Of these, 99.0-99.7 % per year have an SI row visible strictly
after dissemination whose settlement is at most 45 days old:

| year | cells | covered |
| --- | --- | --- |
| 2020 | 201,000 | 99.0 % |
| 2021 | 756,000 | 99.2 % |
| 2022 | 753,000 | 99.7 % |
| 2023 | 750,000 | 99.7 % |
| 2024 | 756,000 | 99.6 % |
| 2025 | 750,000 | 99.5 % |
| 2026 | 537,000 | 99.6 % |

### Validation against the earlier as-of build

The earlier build is `C:\atx\data\finra_short_interest\asof`, whose `available_at` is the dissemination date.

| | old rows | exact match | rate | value mismatch | missing in new |
| --- | --- | --- | --- | --- | --- |
| si_shares | 1,975,506 | 1,974,734 | 0.999609 | 0 | 772 |
| si_dtc | 1,972,621 | 1,971,959 | 0.999664 | 0 | 662 |

No value differs: every old row found in the new build matches exactly. All 772 missing rows
(662 for dtc) are explained:

* 771 rows: TickerHistory3 has no row for the old id within the 7 days before the settlement, neither under its own id nor as a repairable sid-0 row. The earlier producer read an older vendor vintage (the filtered `accepted.zip` up to 2019, then `tbltickerhistory3_10y.zip`) that had those rows. The lines are thin ETFs/ETNs, warrants and units (for example RODI, PMOM, DBUK, MFLA, WTGUU, RVRB). These ids have no prices rows in the gap either.
* 1 row (CHEK): the old id's vendor ticker has no FINRA row that settlement.

All 772 missing rows, by the id's last vendor row before the settlement:

| gap | rows |
| --- | --- |
| 8-30 days | 558 |
| 31-120 days | 117 |
| more than 120 days | 32 |
| none (the id first appears after the settlement) | 65 |

All 772 by dissemination year: 2018: 249, 2019: 8, 2020: 9, 2021: 2, 2022: 66, 2023: 75, 2024: 62,
2025: 68, 2026: 233.

The new build has 71,682 rows in the old window that the old build lacked:

* The sid-0 repair, including MSFT from 2025-12 to 2026-06.
* Full TickerHistory3 coverage in 2018-2019, where the old build used the accepted subset.
* The 7-day vendor fallback.
* 229 OTC or IEX-class rows, a class set the old build excluded.

Rows and ids per dissemination year:

| year | old rows / ids | new rows / ids |
| --- | --- | --- |
| 2018 | 182,154 / 9,260 | 195,847 / 9,306 |
| 2019 | 194,955 / 9,386 | 201,864 / 9,438 |
| 2020 | 202,907 / 10,081 | 208,251 / 10,103 |
| 2021 | 243,000 / 12,431 | 248,829 / 12,446 |
| 2022 | 258,035 / 12,843 | 272,990 / 12,878 |
| 2023 | 244,410 / 12,319 | 255,661 / 12,423 |
| 2024 | 245,706 / 11,973 | 251,190 / 12,057 |
| 2025 | 263,137 / 12,879 | 267,808 / 12,944 |
| 2026 | 141,202 / 13,032 (to 2026-06-25) | 219,824 / 13,845 (to 2026-09-24) |

## Stage V: `short_volume/year=YYYY/short_volume.parquet`

One row per `(trade_date, symbol)` in the CNMS file.

| column | meaning |
| --- | --- |
| `security_id` BIGINT | NULL when unmapped |
| `symbol` | CNMS symbol |
| `trade_date` DATE | trade date |
| `short_volume`, `short_exempt_volume`, `total_volume` DOUBLE | shares; fractional since FINRA began reporting fractional shares |
| `market` | facility codes |
| `map_basis` | `exact`, `canonical`, `ambiguous`, `unmapped`, `collision` |
| `sid_repaired` | true when mapped through a repaired sid-0 row |

Rule `sv-ticker-on-trade-date-v2`:

* Mapping uses the vendor ticker on the trade date itself: a case-sensitive exact match first, then the canonical form. Either must name exactly one id.
* When two symbols map to one id, only the single exact match keeps it.
* The trailer line (record count) is ignored.
* There are 0 duplicate symbol keys, 0 bad lines and 0 date mismatches, and the row counts equal the manifest.
* Trade date `T` is visible from session `T+1`.

| year | sessions | files | missing | 403 weekdays | rows | mapped rows | share rows | share volume | ids | sid0-repaired | collision |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2018 (from 08-01) | 105 | 105 | 0 | 4 | 814,888 | 765,263 | 0.9391 | 0.9789 | 8,417 | 1,896 | 0 |
| 2019 | 252 | 252 | 0 | 9 | 1,973,575 | 1,858,199 | 0.9415 | 0.9899 | 9,368 | 3,491 | 0 |
| 2020 | 253 | 253 | 0 | 9 | 2,111,875 | 1,984,838 | 0.9398 | 0.9955 | 10,187 | 2,773 | 0 |
| 2021 | 252 | 252 | 0 | 9 | 2,464,645 | 2,345,819 | 0.9518 | 0.9983 | 12,088 | 3,150 | 0 |
| 2022 | 251 | 251 | 0 | 9 | 2,516,888 | 2,387,188 | 0.9485 | 0.9920 | 12,490 | 4,930 | 0 |
| 2023 | 250 | 250 | 0 | 10 | 2,441,520 | 2,311,638 | 0.9468 | 0.9597 | 11,998 | 4,701 | 2 |
| 2024 | 252 | 252 | 0 | 10 | 2,503,350 | 2,381,018 | 0.9511 | 0.9613 | 11,726 | 3,447 | 0 |
| 2025 | 250 | 250 | 0 | 11 | 2,669,269 | 2,546,805 | 0.9541 | 0.9494 | 12,665 | 680 | 0 |
| 2026 (to 09-18) | 179 | 179 | 0 | 8 | 2,087,873 | 1,989,424 | 0.9528 | 0.9740 | 13,509 | 146 | 54 |

Total: 19,583,883 rows.

Calendar agreement:

* Every session in the calendar has a file.
* All 79 absent (403) weekdays are exchange closures, including 2018-12-05 and 2025-01-09.
* No file exists on a non-session day.

Unmapped share and residual collisions:

* The unmapped share of volume in 2023-2025 comes from sub-dollar names on days without an ORATS row, such as HCTI (16.2 bn shares in 53 days of 2025), DMN, GNLN and NCNA.
* The 54 collisions in 2026 are ticker-change days on which two CNMS symbols reach one id (PTN/PTNT, CALI/CALY, BZAI/BZAIW, BRKH/BRKHW).

Universe coverage: among the top 3,000 names by dollar volume from 2020-09-28, 99.6 % of
(session, id) cells have a CNMS row. The median CNMS share of consolidated volume rises from 36 %
in 2018 to 47 % in 2025.

## Limitations and open issues

* **Republication vintage (S):** consolidated files for settlements before June 2021 are a later FINRA re-publication. At the time, FINRA published only OTC; listed names came from the exchanges on the exchange receipt date. Positions may have been revised since, and `si_prev`/`revision_flag` reflect the revised next cycle. The coverage window (2020-09-28 on) is affected up to the 2021-05-28 settlement. Treat 2020-09 .. 2021-05 SI as a revised vintage.
* **CDN lag (S):** the CDN file for 2026-09-15 was still 403 on 2026-09-27 while the partitions API listed it. The API copy is used, and the API and CDN copies of 2026-08-31 are identical. A later `fetch --retry-absent` can land the CDN bytes. The build prefers the existing landing, then the new landing.
* **Vendor identity (S, V):** TickerHistory3 has no id for 68,326 sid-0 (date, ticker) pairs whose ticker never carries a non-zero id, plus about 57,000 one-sided or disagreeing pairs. Those lines cannot be keyed in any stage. Thin lines are also missing on some sessions; S covers these with the 7-day fallback, while V does not by rule.
* **CNMS scope (V):** off-exchange (TRF/ADF) volume only, not consolidated tape volume. FINRA keeps no revision history, so the files are the vintage as of download (2026-09-27, sha256 in the manifest).
* **OTC SI:** kept in `mapping_audit`, but essentially unmapped because ORATS covers listed optionable names.
* **Point-in-time:** the sid-0 repair looks at a later vendor row to identify the line (identity only, shared with stage P). The SI fallback and all values use only data dated on or before the settlement or trade date.
