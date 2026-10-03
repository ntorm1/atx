# Task 3 report: Compustat-shaped PIT `compustat_std/` (fundq + funda + rdq)

Status: DONE_WITH_CONCERNS (5 gates PASS, 3 FAIL with measured numbers and diagnosed causes; the stage publishes
with `gate_status` in its manifest, GC9). Spec: gap analysis section 2 item 2. Lane B, first task.

## 1. What was built

* Module `atx-db/src/atx_db/alpha_panel/compustat_std.py` (rule `cstd-pit-v1`, schema
  `atx.alpha-panel.compustat-std/v1`): `build` (fundamentals stage `--fund-stage`, default `fundamentals_v10`, and
  `--earn-stage`, default `earnings_calendar`; `--stage`, default `compustat_std`), `validate` (coverage, rdq, sum4,
  balance, fsds), `cutoff` (rebuild at cutoffs), `handcheck` (SEC 10-Q inline XBRL), `gate-status`;
  `macros_sql(stage_dir)` returns the `fundq_asof(ts)` / `funda_asof(ts)` macros. Memory: 8 CIK buckets
  (`cik % 8`), file-backed DuckDB at 450 MB (`ATX_CSTD_DUCKDB_MEM`), guard cap 0.8 GiB.
* Stage `data/alpha_panel/v1/compustat_std/` (build 2026-10-03, code SHA-256 in the manifest equals the
  committed module):

| file | rows | bytes | keys |
| --- | --- | --- | --- |
| `fundq.parquet` | 421,230 (17,010 CIKs) | 54,090,742 | (cik, datadate, fyearq, fqtr, accession), one row per vintage (filing) |
| `funda.parquet` | 112,802 (15,876 CIKs) | 34,408,214 | (cik, datadate, fyear, accession) = fundq rows with fqtr 4 |
| `validation.json`, `manifest.json` | | | gates; `gate_status` {'balance': 'FAIL', 'coverage': 'PASS', 'cutoff': 'PASS', 'fsds': 'PASS', 'handcheck': 'PASS', 'rdq': 'FAIL', 'sum4': 'FAIL'} |

  datadate 2004-12-31 .. 2026-08-31; available_at 2010-01-05 19:00:00 .. 2026-09-19 22:00:00 UTC;
  406,166 first-reported rows; 5,384 FX-converted rows; 10 rows
  without fqtr. fundq: 32 Compustat items (saleq ... intanq, ibq, epspxq, epsfxq, cshprq) + 6 YTD items (oancfy,
  capxy, dvy, prstkcy, sstky, xrdy) + descriptors (fyr, rdq/rdq_src, curcd, fx_converted, first_reported,
  is_restated, ...). funda: 32 annual items + the 81 catalog items under Compustat names.
* Datadate window [1990-01-01, build date + 365 d = 2027-10-03]: excluded rows events 0
  (of 421,230), quarterly_history 108 (period ends before 1990), prepared Company
  Facts EPS/IB/DO/share facts 0. The inventory's outliers live in the raw Company Facts extract
  (`fundamentals_v10/_work/cf`): 1,058 facts dated after 2027-10-02 (max 6016-06-30) and 663 before 1990 over all
  concepts; the v10 events already drop them (`period_end > filed`), so no fundq row is affected.
* Docs `atx-db/docs/datasets/compustat_std.md` (columns with type / unit / definition / source / clock /
  staleness, keys, PIT rule, fiscal calendar, measured coverage, gates, known limits) and the registration stub
  `atx-db/docs/datasets/compustat_std.registry.json` (stage, path, keys, schema id, as-of macro SQL) for task 17.
  Task 4 reads `fundq` / `funda` and the macros exactly as documented there.

## 2. Gates (GC9)

Linked member issuers = CIKs linked (`identity/link_table_v3`, valid on the session) to a `member_equity` cell
(`_tmp/panel_member`) in the year. Task 2's `universe/` stage has no manifest yet (in progress), so the
univ_all_common / univ_r3000p bases (GC3) are not measured.

| criterion | result | measured | command |
| --- | --- | --- | --- |
| saleq, ibq, atq, ceqq, cheq, cshoq >= 95% per year 2020-2025 | PASS | minima: saleq 0.964 (2025), ibq 0.996, atq 0.996, ceqq 0.994, cheq 0.988, cshoq 0.997 | `cd C:/atx/atx-db && PYTHONPATH=src OPENBLAS_NUM_THREADS=1 .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.compustat_std validate` |
| oancfy, capxy >= 92% per year | PASS | minima: oancfy 0.977 (2021), capxy 0.932 (2021) | same |
| rdq >= 95% of 10-Q periods per year | FAIL | 8-K-sourced (PIT) 0.850-0.901; rdq = matched 8-K date in any acceptance order 0.901-0.946; period has a primary 8-K any time 0.922-0.958; populated 1.000 | same |
| four quarters sum to the annual within 0.5% on >= 98% (saleq, niq) | FAIL | saleq 0.944-0.968, niq 0.953-0.977 (quarters as known at the 10-K: saleq 0.962-0.982, niq 0.970-0.986) | same |
| atq = ltq + teqq within 0.5% on >= 97% | FAIL | 0.915-0.935 (with the temporary-equity term 0.961-0.975) | same |
| FSDS >= 98% within 0.5% | PASS | 6,030 of 6,039 mapped = 0.9985 (6,084 cells, mapped 0.9926) | same |
| 30 issuer-quarters vs the 10-Q, 0 mismatches | PASS | 30 issuer-quarters, 240 item checks: {'match': 228, 'match_derived': 10, 'zero_rule_no_revenue_line': 2} | `cd C:/atx/atx-db && PYTHONPATH=src OPENBLAS_NUM_THREADS=1 .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.compustat_std handcheck --n 30` (cap 0.6) |
| cutoff rebuild 0 diffs | PASS | 10,622 rows, 867,948 cells, 0 differences, 0 row-set differences (90 CIKs incl. 19 FX-converted, 5 cutoffs 2017-2025) | `cd C:/atx/atx-db && PYTHONPATH=src OPENBLAS_NUM_THREADS=1 .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.compustat_std cutoff` |
| datadate outliers excluded and counted (brief) | reported | events 0, quarterly_history 108, facts 0 (raw extract 1,058 + 663) | `cd C:/atx/atx-db && PYTHONPATH=src OPENBLAS_NUM_THREADS=1 .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.compustat_std build ...` (manifest) |

Guard receipts (`.superpowers/sdd/tier1-v4/logs/t3-final-*.receipt.json`): build rc 0, 67 s, peak 0.69 GiB; validate
rc 0, 37 s, 0.57 GiB; cutoff rc 0, 15 s, 0.51 GiB; handcheck rc 0, 22 s, 0.12 GiB. No cap hit.

### Coverage per year (first-reported fundq rows, 10-Q/10-K-type forms, linked member issuers)

| year | issuer_quarters | saleq | ibq | atq | ceqq | cheq | cshoq | oancfy | capxy | niq | epspxq | epsfxq | cshprq | dvy | xrdy | teqq | dlttq |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2020 | 10,077 | 0.965 | 0.997 | 0.998 | 0.995 | 0.993 | 0.997 | 0.980 | 0.944 | 0.998 | 0.951 | 0.944 | 0.943 | 0.947 | 0.924 | 0.995 | 0.998 |
| 2021 | 10,929 | 0.967 | 0.996 | 0.996 | 0.994 | 0.993 | 0.997 | 0.977 | 0.932 | 0.997 | 0.936 | 0.931 | 0.936 | 0.918 | 0.924 | 0.994 | 0.997 |
| 2022 | 10,068 | 0.971 | 0.997 | 0.997 | 0.994 | 0.992 | 0.998 | 0.991 | 0.956 | 0.998 | 0.953 | 0.942 | 0.951 | 0.958 | 0.936 | 0.994 | 0.997 |
| 2023 | 9,399 | 0.972 | 0.999 | 0.997 | 0.995 | 0.992 | 0.997 | 0.992 | 0.953 | 0.999 | 0.957 | 0.947 | 0.952 | 0.971 | 0.935 | 0.995 | 0.997 |
| 2024 | 9,526 | 0.968 | 0.996 | 0.996 | 0.994 | 0.989 | 0.998 | 0.987 | 0.947 | 0.996 | 0.956 | 0.946 | 0.951 | 0.961 | 0.935 | 0.994 | 0.997 |
| 2025 | 9,353 | 0.964 | 0.996 | 0.996 | 0.994 | 0.988 | 0.998 | 0.983 | 0.942 | 0.997 | 0.951 | 0.941 | 0.946 | 0.962 | 0.934 | 0.994 | 0.996 |

### rdq

| year | periods | rdq_populated | rdq_from_8k_pit | info_rdq_equals_matched_8k_date_any_order | info_period_has_primary_8k_any_time | info_issuer_files_no_202_near_period |
| --- | --- | --- | --- | --- | --- | --- |
| 2020 | 7,599 | 1.000 | 0.861 | 0.911 | 0.929 | 0.050 |
| 2021 | 8,214 | 1.000 | 0.850 | 0.901 | 0.922 | 0.056 |
| 2022 | 7,569 | 1.000 | 0.894 | 0.942 | 0.954 | 0.030 |
| 2023 | 7,075 | 1.000 | 0.901 | 0.946 | 0.958 | 0.028 |
| 2024 | 7,170 | 1.000 | 0.889 | 0.941 | 0.952 | 0.030 |
| 2025 | 7,070 | 1.000 | 0.877 | 0.927 | 0.937 | 0.044 |

Cause of the FAIL: 51.5% of these 10-Qs carry an 8-K 2.02 with the same ET date; when the 10-Q is accepted first
(minutes earlier) the PIT rule may not use the later 8-K, so the row's `rdq_src` is `filing` (same date). 2.8-5.6%
of periods belong to issuers that file no 8-K 2.02 near the period, and FPI 6-K releases are outside
`earnings_calendar` (calendar v2, gap item 7). Even ignoring PIT order, 2020/2021/2025 stay below 95%.

### Four quarters vs annual

| year | saleq FYs | saleq within 0.5% | saleq (quarters as known at the 10-K) | niq FYs | niq within 0.5% | niq (as known) | ibq within 0.5% |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2020 | 2,268 | 0.962 | 0.982 | 2,373 | 0.965 | 0.984 | 0.932 |
| 2021 | 2,452 | 0.944 | 0.962 | 2,566 | 0.953 | 0.970 | 0.919 |
| 2022 | 2,355 | 0.965 | 0.976 | 2,432 | 0.966 | 0.981 | 0.943 |
| 2023 | 2,200 | 0.968 | 0.979 | 2,274 | 0.976 | 0.986 | 0.952 |
| 2024 | 2,214 | 0.961 | 0.976 | 2,290 | 0.977 | 0.985 | 0.945 |
| 2025 | 2,126 | 0.968 | 0.979 | 2,221 | 0.973 | 0.985 | 0.946 |

Cause of the FAIL (pooled 2020-2025 diagnosis, saleq: 13,615 fiscal years, 531 outside 0.5%): 198 are fixed by the
quarters as the fundamentals stage knew them at the 10-K (quarters restated after first report keep their
first-reported fundq row); the rest are fundamentals-stage quarter derivations that disagree with the fiscal-year fact,
e.g. CIK 1398805 FY2022: the filer tagged a Q4 3-month Revenues fact of 21,995,000 = the FY value (FY - 9M = 7,896,000);
CIK 36146 FY2024 (bank revenue = interest + noninterest income): the Q1 noninterest-income comparative was restated
in the next year's 10-Q; 8 rows have a zero-filled annual sale against non-zero quarters (CIK 30625).

### Balance identity

| year | issuer-quarters | atq = ltq + teqq (gate) | with temporary equity (info) | ltq or teqq missing |
| --- | --- | --- | --- | --- |
| 2020 | 10,374 | 0.924 | 0.961 | 38 |
| 2021 | 11,266 | 0.915 | 0.963 | 37 |
| 2022 | 10,385 | 0.929 | 0.971 | 41 |
| 2023 | 9,697 | 0.935 | 0.972 | 30 |
| 2024 | 9,827 | 0.934 | 0.975 | 28 |
| 2025 | 9,613 | 0.929 | 0.974 | 21 |

Cause of the FAIL: v10 `ltq` = Liabilities (US GAAP temporary/mezzanine equity is neither liability nor equity);
with the catalog `mibt` term the identity holds on 96.1-97.5%; the rest is temporary equity the v10 `mibt` chain
misses (e.g. CIK 1096343, redeemable NCI not captured) -- gap item 4 / task 5 (mezzanine term in A = L + E).

### FSDS benchmark

FSDS: cells 6084, strata 234, quota 26, mapped 6039 (0.9926), matched 6030 (0.9985 of mapped)

| column | cells | mapped | match of mapped |
| --- | --- | --- | --- |
| `actq` | 156 | 1.000 | 1.0000 |
| `apq` | 156 | 1.000 | 1.0000 |
| `atq` | 156 | 1.000 | 1.0000 |
| `capx` | 156 | 0.994 | 1.0000 |
| `capxy` | 156 | 0.968 | 1.0000 |
| `cheq` | 156 | 1.000 | 1.0000 |
| `cogsq` | 156 | 1.000 | 1.0000 |
| `cshpri` | 156 | 1.000 | 1.0000 |
| `cshprq` | 156 | 1.000 | 1.0000 |
| `dlttq` | 156 | 1.000 | 1.0000 |
| `dvy` | 156 | 0.968 | 1.0000 |
| `epsfi` | 156 | 1.000 | 1.0000 |
| `epsfxq` | 156 | 1.000 | 1.0000 |
| `epspi` | 156 | 0.994 | 0.9935 |
| `epspx` | 156 | 0.981 | 1.0000 |
| `epspxq` | 156 | 1.000 | 1.0000 |
| `gdwlq` | 156 | 1.000 | 1.0000 |
| `ib` | 156 | 1.000 | 1.0000 |
| `ibq` | 156 | 1.000 | 1.0000 |
| `intanq` | 156 | 1.000 | 0.9679 |
| `invtq` | 156 | 1.000 | 1.0000 |
| `lctq` | 156 | 1.000 | 1.0000 |
| `ltq` | 156 | 1.000 | 1.0000 |
| `ni` | 156 | 1.000 | 1.0000 |
| `niq` | 156 | 1.000 | 1.0000 |
| `oancf` | 156 | 1.000 | 1.0000 |
| `oancfy` | 156 | 0.981 | 1.0000 |
| `oiadpq` | 156 | 0.994 | 1.0000 |
| `ppentq` | 156 | 1.000 | 1.0000 |
| `prstkcy` | 156 | 0.885 | 1.0000 |
| `rectq` | 156 | 1.000 | 1.0000 |
| `sale` | 156 | 1.000 | 1.0000 |
| `saleq` | 156 | 1.000 | 1.0000 |
| `seqq` | 156 | 1.000 | 1.0000 |
| `teqq` | 156 | 1.000 | 1.0000 |
| `txtq` | 156 | 1.000 | 1.0000 |
| `xintq` | 156 | 1.000 | 1.0000 |
| `xrdy` | 156 | 0.949 | 0.9797 |
| `xsgaq` | 156 | 1.000 | 1.0000 |

Mismatches (9): intanq (5: IntangibleAssetsNetIncludingGoodwill differs from our gdwl + intan sum), xrdy (3: YTD
R&D sums differ from the FSDS YTD fact), epspi (1: -0.001555 vs -0.0016, FSDS rounds to 4 decimals).

### Hand check vs the 10-Q, row by row

Seeded (20261002) first-reported 10-Q rows of linked member issuers, datadate 2021-2025, USD filers. Each filing's
primary document was fetched from EDGAR through `atx_db.sec_http` (`<acc>-index-headers.html` + primary document,
<= 5 req/s, approved user agent) and its inline XBRL parsed (non-dimensional contexts; nested `ix:nonFraction`
handled). Filing value = the item's documented concept chain on the filing; `match_derived` = the documented fallback
rule evaluated on the filing's facts (bank revenue = interest and dividend income + noninterest income; ltq = assets -
equity incl. NCI; seqq = equity incl. NCI - minority interest); `zero_rule_no_revenue_line` = our saleq 0 under the
zero rule and the filing tags no revenue line. Tolerance 0.5% (EPS: 0.005 absolute). One pick (CIK 1781983, 2021) had
no inline XBRL and was replaced by the next seeded pick. Multi-class issuers: `cshoq` follows the documented rule on
non-dimensional facts (dei cover, else balance-sheet shares, else weighted-average basic shares); the class-summed
dei cover count is shown as information (CIK 1816431: our 136,456,848 = weighted-average basic shares; class-summed
cover count 137,416,283 at 2021-11-09, -0.7%).

| # | cik | accession | datadate | fqtr | item | fundq | 10-Q (inline XBRL) | concept | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | saleq | 66,263,000 | 66,263,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | niq | 10,630,000 | 10,630,000 | NetIncomeLoss | match |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | atq | 480,879,000 | 480,879,000 | Assets | match |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | ltq | 710,849,000 | 710,849,000 | Liabilities | match |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | seqq | -229,970,000 | -229,970,000 | StockholdersEquity | match |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | oancfy | 6,703,000 | 6,703,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | epspxq | 0.04 | 0.04 | EarningsPerShareBasic | match |
| 1 | 899460 | 0000950170-24-055661 | 2024-03-31 | 1 | cshoq | 272,315,846 | 272,315,846 | EntityCommonStockSharesOutstanding | match |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | saleq | 3,351,000,000 | 3,351,000,000 | Revenues | match |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | niq | 639,000,000 | 639,000,000 | NetIncomeLoss | match |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | atq | 50,783,000,000 | 50,783,000,000 | Assets | match |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | ltq | 42,375,000,000 | 42,375,000,000 | derived:Assets-EquityInclNCI | match_derived |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | seqq | 3,865,000,000 | 3,865,000,000 | StockholdersEquity | match |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | oancfy | 2,818,000,000 | 2,818,000,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | epspxq | 0.94 | 0.94 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 2 | 874761 | 0000874761-25-000084 | 2025-09-30 | 3 | cshoq | 712,120,944 | 712,120,944 | EntityCommonStockSharesOutstanding | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | saleq | 345,311,000 | 345,311,000 | Revenues | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | niq | -3,791,000 | -3,791,000 | NetIncomeLoss | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | atq | 2,146,804,000 | 2,146,804,000 | Assets | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | ltq | 672,883,000 | 672,883,000 | Liabilities | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | seqq | 1,473,921,000 | 1,473,921,000 | StockholdersEquity | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | oancfy | 61,304,000 | 61,304,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | epspxq | -0.01 | -0.01 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 3 | 1845257 | 0000950170-25-105096 | 2025-06-30 | 2 | cshoq | 388,936,394 | 388,936,394 | EntityCommonStockSharesOutstanding | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | saleq | 750,334,000 | 750,334,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | niq | -217,710,000 | -217,710,000 | NetIncomeLoss | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | atq | 5,346,218,000 | 5,346,218,000 | Assets | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | ltq | 2,536,074,000 | 2,536,074,000 | Liabilities | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | seqq | 2,712,562,000 | 2,712,562,000 | StockholdersEquity | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | oancfy | 346,036,000 | 346,036,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | epspxq | -2.39 | -2.39 | EarningsPerShareBasic | match |
| 4 | 1520697 | 0000950170-23-058814 | 2023-09-30 | 3 | cshoq | 92,216,476 | 92,216,476 | EntityCommonStockSharesOutstanding | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | saleq | 320,637,000 | 320,637,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | niq | -85,195,000 | -85,195,000 | NetIncomeLoss | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | atq | 2,893,581,000 | 2,893,581,000 | Assets | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | ltq | 2,631,263,000 | 2,631,263,000 | Liabilities | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | seqq | 309,930,000 | 309,930,000 | StockholdersEquity | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | oancfy | -25,597,000 | -25,597,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | epspxq | -1.14 | -1.14 | EarningsPerShareBasic | match |
| 5 | 1592057 | 0001592057-23-000054 | 2023-09-30 | 3 | cshoq | 74,496,537 | 74,496,537 | EntityCommonStockSharesOutstanding | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | saleq | 196,936,000 | 196,936,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | niq | -48,360,000 | -48,360,000 | NetIncomeLoss | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | atq | 883,157,000 | 883,157,000 | Assets | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | ltq | 658,601,000 | 658,601,000 | Liabilities | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | seqq | 224,556,000 | 224,556,000 | StockholdersEquity | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | oancfy | 46,599,000 | 46,599,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | epspxq | -0.2 | -0.2 | EarningsPerShareBasic | match |
| 6 | 1477720 | 0001477720-25-000200 | 2025-07-31 | 2 | cshoq | 236,218,000 | 236,218,000 | rule:WeightedAverageNumberOfSharesOutstandingBasic (cover class sum 236,246,925, rel diff -0.00012) | match_derived |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | saleq | 433,201,000 | 433,201,000 | derived:InterestAndDividendIncomeOperating+NoninterestIncome | match_derived |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | niq | 84,306,000 | 84,306,000 | NetIncomeLoss | match |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | atq | 32,788,494,000 | 32,788,494,000 | Assets | match |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | ltq | 27,474,045,000 | 27,474,045,000 | Liabilities | match |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | seqq | 5,314,449,000 | 5,314,449,000 | StockholdersEquity | match |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | oancfy | 119,982,000 | 119,982,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | epspxq | 0.59 | 0.59 | EarningsPerShareBasic | match |
| 7 | 729986 | 0001193125-25-116615 | 2025-03-31 | 1 | cshoq | 142,059,584 | 142,059,584 | EntityCommonStockSharesOutstanding | match |
| - | 1781983 | 0001558370-21-006263 | | | | | | no inline XBRL facts | skipped (replaced by the next seeded pick) |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | saleq | 270,526,000 | 270,526,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | niq | 34,425,000 | 34,425,000 | NetIncomeLoss | match |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | atq | 2,710,647,000 | 2,710,647,000 | Assets | match |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | ltq | 1,816,151,000 | 1,816,151,000 | Liabilities | match |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | seqq | 894,496,000 | 894,496,000 | StockholdersEquity | match |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | oancfy | 208,353,000 | 208,353,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | epspxq | 0.22 | 0.22 | EarningsPerShareBasic | match |
| 8 | 1389050 | 0001389050-24-000035 | 2024-06-30 | 2 | cshoq | 168,939,131 | 168,939,131 | EntityCommonStockSharesOutstanding | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | saleq | 657,599,000 | 657,599,000 | Revenues | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | niq | 2,329,000 | 2,329,000 | NetIncomeLoss | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | atq | 2,184,688,000 | 2,184,688,000 | Assets | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | ltq | 2,555,911,000 | 2,555,911,000 | Liabilities | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | seqq | -450,816,000 | -450,816,000 | StockholdersEquity | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | oancfy | 36,567,000 | 36,567,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | epspxq | 0.09 | 0.09 | EarningsPerShareBasic | match |
| 9 | 1262976 | 0001262976-21-000065 | 2021-09-30 | 1 | cshoq | 26,094,869 | 26,094,869 | EntityCommonStockSharesOutstanding | match |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | saleq | 6,610,000,000 | 6,610,000,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | niq | 1,493,000,000 | 1,493,000,000 | ProfitLoss | match |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | atq | 75,524,000,000 | 75,524,000,000 | Assets | match |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | ltq | 51,558,000,000 | 51,558,000,000 | Liabilities | match |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | seqq | 23,939,000,000 | 23,939,000,000 | derived:EquityInclNCI-MinorityInterest | match_derived |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | oancfy | 6,682,000,000 | 6,682,000,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | epspxq | 3.46 | 3.46 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 10 | 1730168 | 0001730168-21-000116 | 2021-05-02 | 2 | cshoq | 410,260,560 | 410,260,560 | EntityCommonStockSharesOutstanding | match |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | saleq | 0 |  |  | zero_rule_no_revenue_line |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | niq | -5,969,000 | -5,969,000 | NetIncomeLoss | match |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | atq | 23,789,000 | 23,789,000 | Assets | match |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | ltq | 6,849,000 | 6,849,000 | Liabilities | match |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | seqq | 16,940,000 | 16,940,000 | StockholdersEquity | match |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | oancfy | -14,096,000 | -14,096,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | epspxq | -0.21 | -0.21 | EarningsPerShareBasicAndDiluted | match |
| 11 | 1205922 | 0001564590-21-044453 | 2021-06-30 | 2 | cshoq | 30,801,110 | 30,801,110 | EntityCommonStockSharesOutstanding | match |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | saleq | 393,000,000 | 393,000,000 | Revenues | match |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | niq | 61,400,000 | 61,400,000 | NetIncomeLoss | match |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | atq | 2,973,500,000 | 2,973,500,000 | Assets | match |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | ltq | 2,634,700,000 | 2,634,700,000 | Liabilities | match |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | seqq | 338,800,000 | 338,800,000 | derived:EquityInclNCI-MinorityInterest | match_derived |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | oancfy | 391,000,000 | 391,000,000 | NetCashProvidedByUsedInOperatingActivitiesContinuingOperations | match |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | epspxq | 1.59 | 1.59 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 12 | 20212 | 0000020212-21-000076 | 2021-09-30 | 3 | cshoq | 38,249,128 | 38,249,128 | EntityCommonStockSharesOutstanding | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | saleq | 504,000 | 504,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | niq | -116,591,000 | -116,591,000 | NetIncomeLoss | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | atq | 2,439,017,000 | 2,439,017,000 | Assets | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | ltq | 355,081,000 | 355,081,000 | Liabilities | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | seqq | 2,083,936,000 | 2,083,936,000 | StockholdersEquity | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | oancfy | 109,752,000 | 109,752,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | epspxq | -1.43 | -1.43 | EarningsPerShareBasic | match |
| 13 | 1674416 | 0000950170-24-055719 | 2024-03-31 | 1 | cshoq | 84,918,093 | 84,918,093 | EntityCommonStockSharesOutstanding | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | saleq | 5,868,800,000 | 5,868,800,000 | Revenues | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | niq | 155,100,000 | 155,100,000 | NetIncomeLoss | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | atq | 17,817,200,000 | 17,817,200,000 | Assets | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | ltq | 11,048,200,000 | 11,048,200,000 | Liabilities | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | seqq | 6,646,900,000 | 6,646,900,000 | StockholdersEquity | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | oancfy | -142,000,000 | -142,000,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | epspxq | 3.26 | 3.26 | EarningsPerShareBasic | match |
| 14 | 1037976 | 0001037976-24-000044 | 2024-09-30 | 3 | cshoq | 47,444,666 | 47,444,666 | EntityCommonStockSharesOutstanding | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | saleq | 2,024,000,000 | 2,024,000,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | niq | 156,000,000 | 156,000,000 | NetIncomeLoss | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | atq | 8,506,000,000 | 8,506,000,000 | Assets | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | ltq | 4,621,000,000 | 4,621,000,000 | Liabilities | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | seqq | 3,727,000,000 | 3,727,000,000 | StockholdersEquity | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | oancfy | -24,000,000 | -24,000,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | epspxq | 0.71 | 0.71 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 15 | 1307954 | 0001437749-21-018023 | 2021-06-30 | 2 | cshoq | 221,929,899 | 221,929,899 | EntityCommonStockSharesOutstanding | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | saleq | 2,051,000,000 | 2,051,000,000 | Revenues | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | niq | 251,000,000 | 251,000,000 | NetIncomeLoss | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | atq | 161,631,000,000 | 161,631,000,000 | Assets | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | ltq | 155,760,000,000 | 155,760,000,000 | Liabilities | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | seqq | 4,153,000,000 | 4,153,000,000 | StockholdersEquity | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | oancfy | 231,000,000 | 231,000,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | epspxq | 2.29 | 2.29 | EarningsPerShareBasic | match |
| 16 | 1535929 | 0001535929-24-000050 | 2024-03-31 | 1 | cshoq | 100,701,204 | 100,701,204 | EntityCommonStockSharesOutstanding | match |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | saleq | 0 |  |  | zero_rule_no_revenue_line |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | niq | -18,091,000 | -18,091,000 | NetIncomeLoss | match |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | atq | 509,526,000 | 509,526,000 | Assets | match |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | ltq | 15,611,000 | 15,611,000 | Liabilities | match |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | seqq | 493,915,000 | 493,915,000 | StockholdersEquity | match |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | oancfy | -49,117,000 | -49,117,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | epspxq | -0.13 | -0.13 | EarningsPerShareBasic | match |
| 17 | 1816431 | 0001140361-21-037702 | 2021-09-30 | 3 | cshoq | 136,456,848 | 136,456,848 | rule:WeightedAverageNumberOfSharesOutstandingBasic (cover class sum 137,416,283, rel diff -0.00698) | match_derived |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | saleq | 649,573,000 | 649,573,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | niq | 21,219,000 | 21,219,000 | NetIncomeLoss | match |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | atq | 1,050,833,000 | 1,050,833,000 | Assets | match |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | ltq | 579,204,000 | 579,204,000 | Liabilities | match |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | seqq | 471,625,000 | 471,625,000 | StockholdersEquity | match |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | oancfy | 88,562,000 | 88,562,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | epspxq | 1.26 | 1.26 | EarningsPerShareBasic | match |
| 18 | 700923 | 0000700923-21-000030 | 2021-06-30 | 2 | cshoq | 16,867,470 | 16,867,470 | EntityCommonStockSharesOutstanding | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | saleq | 650,405,000 | 650,405,000 | Revenues | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | niq | 57,413,000 | 57,413,000 | NetIncomeLoss | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | atq | 1,454,258,000 | 1,454,258,000 | Assets | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | ltq | 377,802,000 | 377,802,000 | Liabilities | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | seqq | 1,076,456,000 | 1,076,456,000 | StockholdersEquity | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | oancfy | 294,890,000 | 294,890,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | epspxq | 0.87 | 0.87 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 19 | 1808834 | 0001808834-21-000113 | 2021-09-30 | 3 | cshoq | 65,391,285 | 65,391,285 | EntityCommonStockSharesOutstanding | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | saleq | 4,360,196,000 | 4,360,196,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | niq | 121,278,000 | 121,278,000 | NetIncomeLoss | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | atq | 9,927,816,000 | 9,927,816,000 | Assets | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | ltq | 6,836,593,000 | 6,836,593,000 | Liabilities | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | seqq | 3,077,576,000 | 3,077,576,000 | StockholdersEquity | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | oancfy | 399,598,000 | 399,598,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | epspxq | 0.51 | 0.51 | EarningsPerShareBasic | match |
| 20 | 802481 | 0000802481-23-000081 | 2023-09-24 | 3 | cshoq | 236,789,929 | 236,789,929 | EntityCommonStockSharesOutstanding | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | saleq | 489,578,000 | 489,578,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | niq | 134,503,000 | 134,503,000 | NetIncomeLoss | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | atq | 5,700,881,000 | 5,700,881,000 | Assets | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | ltq | 3,320,706,000 | 3,320,706,000 | Liabilities | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | seqq | 2,380,175,000 | 2,380,175,000 | StockholdersEquity | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | oancfy | 386,364,000 | 386,364,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | epspxq | 0.49 | 0.49 | EarningsPerShareBasic | match |
| 21 | 23194 | 0000023194-23-000020 | 2023-03-31 | 1 | cshoq | 277,510,165 | 277,510,165 | EntityCommonStockSharesOutstanding | match |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | saleq | 195,598,000 | 195,598,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | niq | 34,484,000 | 34,484,000 | NetIncomeLoss | match |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | atq | 963,012,000 | 963,012,000 | Assets | match |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | ltq | 201,840,000 | 201,840,000 | derived:Assets-EquityInclNCI | match_derived |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | seqq | 761,172,000 | 761,172,000 | StockholdersEquity | match |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | oancfy | 4,572,000 | 4,572,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | epspxq | 0.51 | 0.51 | EarningsPerShareBasic | match |
| 22 | 1046568 | 0000950170-23-017893 | 2023-03-31 | 1 | cshoq | 67,477,996 | 67,477,996 | EntityCommonStockSharesOutstanding | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | saleq | 120,571,000 | 120,571,000 | Revenues | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | niq | -88,669,000 | -88,669,000 | NetIncomeLoss | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | atq | 3,394,339,000 | 3,394,339,000 | Assets | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | ltq | 2,509,097,000 | 2,509,097,000 | Liabilities | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | seqq | 879,775,000 | 879,775,000 | StockholdersEquity | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | oancfy | 40,704,000 | 40,704,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | epspxq | -0.51 | -0.51 | EarningsPerShareBasic | match |
| 23 | 790816 | 0000790816-25-000034 | 2025-06-30 | 2 | cshoq | 173,699,039 | 173,699,039 | EntityCommonStockSharesOutstanding | match |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | saleq | 144,748,000 | 144,748,000 | derived:InterestAndDividendIncomeOperating+NoninterestIncome | match_derived |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | niq | 36,241,000 | 36,241,000 | NetIncomeLoss | match |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | atq | 9,307,376,000 | 9,307,376,000 | Assets | match |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | ltq | 8,266,232,000 | 8,266,232,000 | Liabilities | match |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | seqq | 1,041,144,000 | 1,041,144,000 | StockholdersEquity | match |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | oancfy | 113,934,000 | 113,934,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | epspxq | 1.23 | 1.23 | EarningsPerShareBasic | match |
| 24 | 835324 | 0001437749-25-033206 | 2025-09-30 | 3 | cshoq | 29,473,458 | 29,473,458 | EntityCommonStockSharesOutstanding | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | saleq | 6,582,000 | 6,582,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | niq | -33,155,000 | -33,155,000 | NetIncomeLoss | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | atq | 210,293,000 | 210,293,000 | Assets | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | ltq | 79,482,000 | 79,482,000 | Liabilities | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | seqq | 130,811,000 | 130,811,000 | StockholdersEquity | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | oancfy | -44,217,000 | -44,217,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | epspxq | -0.34 | -0.34 | EarningsPerShareBasic | match |
| 25 | 1437402 | 0001628280-21-009180 | 2021-03-31 | 1 | cshoq | 98,709,188 | 98,709,188 | EntityCommonStockSharesOutstanding | match |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | saleq | 170,464,000 | 170,464,000 | RevenueFromContractWithCustomerIncludingAssessedTax | match |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | niq | 18,197,000 | 18,197,000 | NetIncomeLoss | match |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | atq | 1,037,882,000 | 1,037,882,000 | Assets | match |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | ltq | 379,861,000 | 379,861,000 | derived:Assets-EquityInclNCI | match_derived |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | seqq | 658,021,000 | 658,021,000 | StockholdersEquity | match |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | oancfy | 17,573,000 | 17,573,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | epspxq | 1.54 | 1.54 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 26 | 310354 | 0001437749-24-032878 | 2024-09-30 | 1 | cshoq | 12,081,695 | 12,081,695 | EntityCommonStockSharesOutstanding | match |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | saleq | 47,742,000 | 47,742,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | niq | 3,576,000 | 3,576,000 | NetIncomeLoss | match |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | atq | 220,766,000 | 220,766,000 | Assets | match |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | ltq | 114,172,000 | 114,172,000 | derived:Assets-EquityInclNCI | match_derived |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | seqq | 106,594,000 | 106,594,000 | StockholdersEquity | match |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | oancfy | 3,117,000 | 3,117,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | epspxq | 0.13 | 0.13 | EarningsPerShareBasic | match |
| 27 | 78749 | 0000950170-22-020405 | 2022-09-30 | 2 | cshoq | 25,064,471 | 25,064,471 | EntityCommonStockSharesOutstanding | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | saleq | 293,116,000 | 293,116,000 | Revenues | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | niq | 249,268,000 | 249,268,000 | NetIncomeLoss | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | atq | 6,599,607,000 | 6,599,607,000 | Assets | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | ltq | 2,026,588,000 | 2,026,588,000 | Liabilities | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | seqq | 4,573,019,000 | 4,573,019,000 | StockholdersEquity | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | oancfy | 361,679,000 | 361,679,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | epspxq | 0.81 | 0.81 | EarningsPerShareBasic | match |
| 28 | 876437 | 0000876437-22-000035 | 2022-06-30 | 2 | cshoq | 303,439,959 | 303,439,959 | EntityCommonStockSharesOutstanding | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | saleq | 1,717,200,000 | 1,717,200,000 | Revenues | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | niq | 2,100,000 | 2,100,000 | NetIncomeLoss | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | atq | 9,287,100,000 | 9,287,100,000 | Assets | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | ltq | 5,942,700,000 | 5,942,700,000 | Liabilities | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | seqq | 3,319,400,000 | 3,319,400,000 | StockholdersEquity | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | oancfy | -426,300,000 | -426,300,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | epspxq | 0 | 0 | IncomeLossFromContinuingOperationsPerBasicShare | match |
| 29 | 1681459 | 0001681459-22-000053 | 2022-06-30 | 2 | cshoq | 452,211,536 | 452,211,536 | EntityCommonStockSharesOutstanding | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | saleq | 49,013,000 | 49,013,000 | RevenueFromContractWithCustomerExcludingAssessedTax | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | niq | -12,221,000 | -12,221,000 | NetIncomeLoss | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | atq | 266,512,000 | 266,512,000 | Assets | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | ltq | 55,445,000 | 55,445,000 | Liabilities | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | seqq | 211,067,000 | 211,067,000 | StockholdersEquity | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | oancfy | -17,406,000 | -17,406,000 | NetCashProvidedByUsedInOperatingActivities | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | epspxq | -0.17 | -0.17 | EarningsPerShareBasicAndDiluted | match |
| 30 | 1626450 | 0001564590-21-042140 | 2021-06-30 | 2 | cshoq | 71,110,070 | 71,110,070 | EntityCommonStockSharesOutstanding | match |


## 3. Tests

`cd C:/atx/atx-db && PYTHONPATH=src OPENBLAS_NUM_THREADS=1 .venv/Scripts/python.exe -m pytest -n0
tests/test_alpha_panel_compustat_std.py` -> `12 passed in 12.29s` (offline fixtures). Rules covered: fiscal calendar
(fyr from FY rows / label fallback, fqtr by dates over a wrong label, Compustat fyear for January and 52/53-week
years); datadate window exclusion and count; YTD sums (PIT history vintages, missing quarter, zero rule, fqtr 4 TTM);
FX-converted YTD at per-quarter rates and other-currency history ignored; ibq paths (ib 3-month, niq when no DO,
niq - doq, FY - 9M, DO own-period rule, YTD-only DO -> NULL); EPS continuing-first and total only when doq = 0;
cshprq 4 FY - 3 x 9M; facts PIT (an amendment's restated fact invisible to the original row); rdq (8-K before the
filing, 8-K after the filing -> first filing ET date, amendment keeps rdq); first_reported; catalog renames (INTAN,
EPSPX/EPSPI); funda = fqtr 4 rows; the as-of macro; cutoff rebuild equality; coverage / rdq / sum4 / balance / FSDS
gates; inline XBRL parsing (scale, sign, nested tags, dimensional contexts, class-sum info, derived fallbacks).

## 4. Files changed, sources, disk

* New: `atx-db/src/atx_db/alpha_panel/compustat_std.py`, `atx-db/tests/test_alpha_panel_compustat_std.py`,
  `atx-db/docs/datasets/compustat_std.md`, `atx-db/docs/datasets/compustat_std.registry.json`, this report. No
  shared registry, foreign-dirty or other-lane file touched.
* Sources landed: none (no new source). SEC requests for the hand check and its diagnosis: 31 filings x 2 requests +
  3 diagnostic documents, through `sec_http` (<= 5 req/s); nothing stored besides `validation.json`.
* Disk: stage 85 MB (`fundq` 54.1 MB, `funda` 34.4 MB, `validation.json` 77 KB); scratch
  `_tmp/compustat_std/facts.parquet` 33 MB (kept for the cutoff check; bucket parts and scratch DuckDB files
  removed). C: free 70 GB.

## 5. Deviations, open issues, self-review

Deviations (all documented in `docs/datasets/compustat_std.md`):
* Vintage design: one fundq row per filing of the issuer's then-current datadate (the fundamentals event). A later
  filing that restates an older quarter does not add a row to that older datadate; restated quarter values stay in
  `quarterly_history` (`fund_history_asof`). This is the main contributor to the sum4 FAIL cause (1).
* `funda` keys (cik, datadate, fyear, accession): fqtr is always 4 and is dropped (Compustat funda has no fqtr).
* `datadate` is the actual period end (Compustat month-rounds 52/53-week ends); fyr / fqtr use the effective month.
* fqtr follows the dates (fyr from FY rows received) over the DEI label: 1,922 of 308,729 Q1-Q3-labelled rows differ.
* Catalog columns renamed to their Compustat meaning: catalog `intano` (= gdwl + intan) -> `intan`, events `intan`
  -> `intano`; catalog `epspx_ttm`/`epsfx_ttm` (total EPS) -> `epspi`/`epsfi`, catalog `epspi_ttm`/`epsfi_ttm`
  (continuing operations) -> `epspx`/`epsfx`. `mibt` keeps the catalog name but holds temporary equity.
* `dvy` = common cash dividends paid (v10 dvc chain); funda carries `dvc` and the catalog `dv` (total).
* `rdq` is PIT: only 8-K 2.02 announcements accepted by the row's `available_at`; otherwise the first vintage's
  filing date with `rdq_src = filing`. The rdq gate is measured on the 8-K-sourced share (strict); the lenient
  readings are reported beside it.
* sum4 gates saleq and niq (the spec names no items); ibq is reported (0.919-0.952).
* `ibq` / `epspxq` / `epsfxq` / `cshprq` read the fundamentals stage's Company Facts extract (`<stage>/_work/cf`);
  without it they fall back to niq - 0 where the catalog DO TTM is 0 and are otherwise NULL (manifest `facts`).

Open issues / handoffs:
* Task 5 (v11): prefer FY - 9M over a Q4 3-month fact that disagrees with it; extend the temporary-equity chain
  (balance gate); fix the catalog labels `intano` (holds INTAN) and `epspx/epspi` (swapped vs Compustat); then rebuild
  this stage with `--fund-stage fundamentals_v11` and re-run validate / cutoff / handcheck.
* rdq coverage needs `earnings_calendar_v2` (FPI 6-K) and 8-K coverage of issuers without item 2.02.
* Optional extension: restated-quarter vintage rows for older datadates from `quarterly_history`.

Self-review findings fixed before the final build: FSDS per-share cells used uom `USD/shares` (FSDS stores `USD`), so
EPS was never sampled; the inline XBRL parser dropped nested `ix:nonFraction` facts; the balance info metric took the
temporary-equity term from funda (fqtr 4 only) instead of the fundamentals catalog; the DO zero rule was a 460-day
look-back that nulled ibq for issuers with a tiny DO fact 15 months earlier (now the filing's own period). Remaining:
`epsfxq_src` is not published (epsfxq follows the epspxq rule with diluted concepts).
