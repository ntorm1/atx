# B3 and CPI acceptance, 2026-10-05

2020-to-latest delivery retains earlier history and original-window tables. C5/hash/partition checks PASS for both stages. Upstream fundamentals balance, rdq and sum4 remain FAIL. Fiscal cohort completeness is unverified; latest availability year is partial.

Manifest pins: funda `7516ec57e58a533699152ba3a8e204d82b6706b2969e7641af7e4f166a4ec97d`; CPI `06a63d16a43a9c821f213bf819b7a0971bc71b796536305ede9fa9dadb26d6e6`. Upstream `9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325`.

Fundamentals max available_at is 2026-09-19 22:00 UTC. CPI latest reference month is 2026-08-01, latest vintage 2026-09-11 (12:30 UTC). CPI uses the brief's ALFRED vintage-date alternative at 08:30 New York. A separate BLS schedule was not obtained: hash acceptance does not independently prove historical intraday release times, especially the four archival revisions. October 2025 is absent in ALFRED and blank in both FRED/BLS cross-checks; 2025 and 2026 coverage are partial. All 224 requested vintages landed; 28/30 HTTP attempts used, no new finalization requests.

Published manifests retain checkpoint code identities. Current acceptance-only enhancements and defensive manifest path validation postdate the builds; no output was replaced or changed. Exact guarded commands and actual printed outputs follow. Receipt paths are relative to C:/atx; command working directory C:/atx.

## Funda build (preserved)

```text
$ C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --heavy --job-gb 1.0 --wait-minutes 90 --disk-path C:/ --min-free-disk-gb 25 --quiet --receipt .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\guard-b3-build.json --stdout .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\b3-build.stdout --stderr .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\b3-build.stderr -- C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.compustat_std_parts build
{
  "stage": "C:\\atx\\atx-db\\data\\alpha_panel\\v1\\compustat_std_parts",
  "manifest_sha256": "7516ec57e58a533699152ba3a8e204d82b6706b2969e7641af7e4f166a4ec97d",
  "source_rows": 235563,
  "exported_rows": 235563,
  "excluded_null_available_at": 0,
  "per_part": {
    "2010": 976,
    "2011": 3477,
    "2012": 15187,
    "2013": 18705,
    "2014": 18221,
    "2015": 17347,
    "2016": 17789,
    "2017": 15449,
    "2018": 16120,
    "2019": 16454,
    "2020": 13843,
    "2021": 14738,
    "2022": 15414,
    "2023": 14691,
    "2024": 13631,
    "2025": 13229,
    "2026": 10292
  },
  "timings_s": {
    "verify_upstream": 0.072,
    "write_parts": 2.154
  }
}

```

## Funda acceptance

```text
$ C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --heavy --job-gb 0.6 --wait-minutes 90 --disk-path C:/ --min-free-disk-gb 25 --quiet --receipt .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\guard-b3-accept-sol.json --stdout .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\b3-accept-sol.stdout --stderr .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\b3-accept-sol.stderr -- C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.compustat_std_parts accept
$ python -m atx_db.alpha_panel.compustat_std_parts accept --stage compustat_std_parts
  upstream manifest pins: {'compustat_std/manifest.json': '9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325'}
  upstream gate_status (inherited, not repaired by export): {'balance': 'FAIL', 'coverage': 'PASS', 'cutoff': 'PASS', 'fsds': 'PASS', 'handcheck': 'PASS', 'rdq': 'FAIL', 'sum4': 'FAIL'}
  export counts: {'excluded_null_available_at': 0, 'exported_rows': 235563, 'per_part': {'2010': 976, '2011': 3477, '2012': 15187, '2013': 18705, '2014': 18221, '2015': 17347, '2016': 17789, '2017': 15449, '2018': 16120, '2019': 16454, '2020': 13843, '2021': 14738, '2022': 15414, '2023': 14691, '2024': 13631, '2025': 13229, '2026': 10292}, 'source_rows': 235563}

(i) C5 manifest self-check (engine PinnedStage contract; every listed file re-hashed)
  manifest C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/manifest.json bytes=6181 sha256=7516ec57e58a533699152ba3a8e204d82b6706b2969e7641af7e4f166a4ec97d
  contract errors=0 []
  files listed=17 verified=17 bad=0 [] unlisted=0 []

(iv) part year check: count(*) FILTER (WHERE available_at IS NULL OR available_at < 'Y-01-01' OR available_at >= 'Y+1-01-01') per parts/year=Y
  parts/year=2010/funda.parquet rows=976 min=2010-01-21 22:06:00 max=2010-12-29 19:12:00 outside_year=0
  parts/year=2011/funda.parquet rows=3477 min=2011-01-14 15:46:00 max=2011-12-31 01:15:00 outside_year=0
  parts/year=2012/funda.parquet rows=15187 min=2012-01-03 21:23:00 max=2012-12-31 22:20:00 outside_year=0
  parts/year=2013/funda.parquet rows=18705 min=2013-01-02 15:45:00 max=2013-12-31 21:43:00 outside_year=0
  parts/year=2014/funda.parquet rows=18221 min=2014-01-02 17:11:00 max=2014-12-31 21:55:00 outside_year=0
  parts/year=2015/funda.parquet rows=17347 min=2015-01-02 19:12:00 max=2015-12-31 22:24:00 outside_year=0
  parts/year=2016/funda.parquet rows=17789 min=2016-01-04 17:41:00 max=2016-12-30 22:42:00 outside_year=0
  parts/year=2017/funda.parquet rows=15449 min=2017-01-04 14:10:00 max=2017-12-29 22:33:00 outside_year=0
  parts/year=2018/funda.parquet rows=16120 min=2018-01-02 15:16:00 max=2018-12-31 22:31:00 outside_year=0
  parts/year=2019/funda.parquet rows=16454 min=2019-01-03 02:51:00 max=2019-12-31 21:05:00 outside_year=0
  parts/year=2020/funda.parquet rows=13843 min=2020-01-02 18:10:00 max=2020-12-31 22:59:00 outside_year=0
  parts/year=2021/funda.parquet rows=14738 min=2021-01-01 00:01:00 max=2021-12-30 22:00:00 outside_year=0
  parts/year=2022/funda.parquet rows=15414 min=2022-01-03 11:29:00 max=2022-12-30 21:35:00 outside_year=0
  parts/year=2023/funda.parquet rows=14691 min=2023-01-03 11:04:00 max=2023-12-29 22:53:00 outside_year=0
  parts/year=2024/funda.parquet rows=13631 min=2024-01-02 13:00:00 max=2024-12-31 21:53:00 outside_year=0
  parts/year=2025/funda.parquet rows=13229 min=2025-01-02 22:11:00 max=2025-12-31 22:25:00 outside_year=0
  parts/year=2026/funda.parquet rows=10292 min=2026-01-07 12:23:00 max=2026-09-19 22:00:00 outside_year=0
  rows_outside_year=0 rows_total=235563
  (i)+(iv) PASS

data end (max available_at) 2026-09-19 22:00:00; availability year 2026 is partial (rows through 2026-09-19 22:00:00). Primary window 2020-2026 (owner steering 2026-10-05); the request's original windows follow each primary table as supporting evidence.
fiscal-year status: partial = a fiscal period of the year (FYE June Y .. early June Y+1) is still open at the data end;completeness_unverified = rows as observed at the data end, no source evidence closes the year (late filings and restatement vintages can still add rows)

observed fiscal years 2020+ (first-reported rows, all templates)
  sql: SELECT fyear, min(datadate), max(datadate), count(*), count(DISTINCT cik), max(available_at) FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2010/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2011/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2012/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2013/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2014/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2015/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2016/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2017/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2018/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2019/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2020/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2021/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2022/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2023/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2024/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2025/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2026/funda.parquet']) WHERE first_reported AND fyear >= 2020 GROUP BY 1 ORDER BY 1
  fyear status min_datadate max_datadate rows issuers max_available_at
  2020 completeness_unverified 2020-06-24 2021-05-31 6738 6727 2024-12-19 18:57:00
  2021 completeness_unverified 2021-06-24 2022-05-31 7595 7586 2025-07-30 15:18:00
  2022 completeness_unverified 2022-06-24 2023-06-03 7352 7346 2026-04-03 16:53:00
  2023 completeness_unverified 2023-06-24 2024-06-01 6925 6914 2026-08-07 22:00:00
  2024 completeness_unverified 2024-06-26 2025-05-31 6729 6714 2026-07-21 22:00:00
  2025 completeness_unverified 2025-06-25 2026-05-31 6425 6416 2026-09-19 22:00:00
  2026 partial 2026-06-24 2026-08-01 143 143 2026-09-19 22:00:00

(ii) xsga coverage, primary window 2020-2026: first-reported rows, fin_template in ('other', 'utility'), every vintage stamp
  sql: SELECT fyear, count(*), count(xsga), count(*) FILTER (WHERE available_at < TIMESTAMP '2024-01-01'), count(xsga) FILTER (WHERE available_at < TIMESTAMP '2024-01-01') FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2010/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2011/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2012/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2013/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2014/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2015/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2016/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2017/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2018/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2019/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2020/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2021/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2022/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2023/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2024/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2025/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2026/funda.parquet']) WHERE first_reported AND fin_template IN ('other', 'utility') AND fyear BETWEEN 2020 AND 2026 GROUP BY 1 ORDER BY 1
  fyear status rows with_xsga share
  2020 completeness_unverified 5590 4799 0.8585
  2021 completeness_unverified 6450 5415 0.8395
  2022 completeness_unverified 6201 5389 0.8691
  2023 completeness_unverified 5783 5072 0.8771
  2024 completeness_unverified 5583 4906 0.8787
  2025 completeness_unverified 5272 4595 0.8716
  2026 partial 122 114 0.9344
  total 35001 30290 0.8654

(ii) xsga coverage, original window 2009-2022 (supporting): first-reported rows, fin_template in ('other', 'utility')
  sql: SELECT fyear, count(*), count(xsga), count(*) FILTER (WHERE available_at < TIMESTAMP '2024-01-01'), count(xsga) FILTER (WHERE available_at < TIMESTAMP '2024-01-01') FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2010/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2011/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2012/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2013/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2014/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2015/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2016/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2017/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2018/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2019/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2020/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2021/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2022/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2023/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2024/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2025/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2026/funda.parquet']) WHERE first_reported AND fin_template IN ('other', 'utility') AND fyear BETWEEN 2009 AND 2022 GROUP BY 1 ORDER BY 1
  fyear rows with_xsga share | stamped<2024: rows with_xsga share
  2009 375 308 0.8213 | 375 308 0.8213
  2010 1180 974 0.8254 | 1180 974 0.8254
  2011 6041 5240 0.8674 | 6041 5240 0.8674
  2012 6272 5503 0.8774 | 6272 5503 0.8774
  2013 6123 5394 0.8809 | 6123 5394 0.8809
  2014 5982 5307 0.8872 | 5982 5307 0.8872
  2015 5652 5021 0.8884 | 5652 5021 0.8884
  2016 5302 4739 0.8938 | 5302 4739 0.8938
  2017 5509 4864 0.8829 | 5509 4864 0.8829
  2018 5462 4828 0.8839 | 5462 4828 0.8839
  2019 5350 4748 0.8875 | 5349 4748 0.8876
  2020 5590 4799 0.8585 | 5588 4799 0.8588
  2021 6450 5415 0.8395 | 6445 5412 0.8397
  2022 6201 5389 0.8691 | 6170 5362 0.8690
  total 71489 62529 0.8747 | 71450 62499 0.8747

(iii) SG&A convention (report, not a gate)
  sql: SELECT count(*), count(*) FILTER (WHERE xsga IS NOT NULL AND xrd IS NOT NULL), count(*) FILTER (WHERE xsga IS NOT NULL AND xrd > 0), count(*) FILTER (WHERE first_reported), count(*) FILTER (WHERE first_reported AND xsga IS NOT NULL AND xrd IS NOT NULL), count(*) FILTER (WHERE first_reported AND xsga IS NOT NULL AND xrd > 0) FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2010/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2011/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2012/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2013/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2014/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2015/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2016/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2017/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2018/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2019/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2020/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2021/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2022/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2023/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2024/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2025/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2026/funda.parquet'])
  all rows=235563 xsga&xrd non-NULL=186208 share=0.7905; xsga non-NULL & xrd>0=77268 share=0.3280
  first-reported rows=106729 xsga&xrd non-NULL=83413 share=0.7815; xsga non-NULL & xrd>0=36232 share=0.3395
  sample sql: WITH fs AS (SELECT DISTINCT adsh FROM read_parquet('C:/atx/atx-db/data/staging/fsds-v2/sub/*.parquet')), r AS (SELECT cik, accession, datadate, fyear, available_at, xsga, xrd, available_at >= TIMESTAMP '2020-01-01' AS target, fs.adsh IS NOT NULL AS in_fsds FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2010/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2011/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2012/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2013/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2014/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2015/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2016/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2017/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2018/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2019/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2020/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2021/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2022/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2023/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2024/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2025/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2026/funda.parquet']) LEFT JOIN fs ON fs.adsh = accession WHERE first_reported AND xrd > 0 AND xsga IS NOT NULL), pool AS (SELECT * FROM r QUALIFY row_number() OVER (PARTITION BY cik ORDER BY target DESC, in_fsds DESC, available_at DESC, accession DESC) = 1) SELECT cik, accession, datadate, fyear, available_at, xsga, xrd, target, in_fsds, (SELECT count(*) FROM pool) AS pool_issuers, (SELECT count(*) FROM pool WHERE target) AS pool_target FROM pool ORDER BY target DESC, md5(CAST(cik AS VARCHAR)), cik LIMIT 50
  FSDS C:/atx/atx-db/data/staging/fsds-v2 pre (stmt IS, inpth false, the IS report with most SG&A/R&D/total tags) + num (qtrs 4, no segments/coreg, the filing's dominant unit, latest ddate)
  pool issuers=5597 with a filing stamped >= 2020-01-01=3890 sampled=50 sampled target-period filings=50 classes={'excludes_rd': 30, 'unresolved:face_identity_not_found': 8, 'unresolved:no_total_line_after_sga_and_rd': 7, 'unresolved:rd_not_on_face': 4, 'unresolved:sga_not_on_face': 1}
  cik accession fyear available_at in_fsds class basis face_lines uom xsga face_sga xsga_equals_face_sga xrd
  1534133 0001193125-26-088147 2025 2026-03-03 21:06:00 True excludes_rd face_identity_total_equals_sga_plus_rd 14 USD 7887000.0 7887000.0 True 15234000.0
  1763950 0001493152-26-013612 2025 2026-03-30 21:36:00 True excludes_rd face_identity_total_equals_sga_plus_rd 11 USD 6464371.0 6464371.0 True 11514123.0
  1430300 0001477932-24-003836 2023 2024-06-24 20:31:00 True excludes_rd face_identity_total_equals_sga_plus_rd 18 USD 109930.0 109930.0 True 151000.0
  2033383 0001493152-26-013285 2025 2026-03-27 21:24:00 True excludes_rd face_identity_total_equals_sga_plus_rd 26 USD 9541000.0 9541000.0 True 720000.0
  830656 0001493152-24-023201 2023 2024-06-07 21:28:00 True excludes_rd face_identity_total_equals_sga_plus_rd 20 USD 8935766.0 8935766.0 True 1367154.0
  1045986 0001213900-25-043461 2024 2025-05-14 21:45:00 True unresolved no_total_line_after_sga_and_rd 22 USD 335151000.0 335151000.0 True 84380000.0
  21510 0000021510-21-000060 2021 2021-11-29 22:32:00 True excludes_rd face_identity_total_equals_sga_plus_rd 21 USD 303863000.0 303863000.0 True 124266000.0
  1634432 0001558370-21-002288 2020 2021-03-03 13:02:00 True excludes_rd face_identity_total_equals_sga_plus_rd 26 USD 40867000.0 40867000.0 True 25795000.0
  1485003 0001628280-25-015673 2024 2025-03-31 20:33:00 True excludes_rd face_identity_total_equals_sga_plus_rd 16 USD 22138000.0 22138000.0 True 59673000.0
  1836981 0001836981-26-000018 2025 2026-03-02 21:34:00 True excludes_rd face_identity_anchor_minus_sga_and_rd_equals_operating_income 26 USD 95132000.0 95132000.0 True 16752000.0
  808450 0000808450-20-000105 2020 2020-12-17 11:39:00 True unresolved face_identity_not_found 22 USD 1021000000.0 1021000000.0 True 321000000.0
  1627281 0001193125-26-116428 2025 2026-03-19 23:53:00 True unresolved no_total_line_after_sga_and_rd 24 USD 19790000.0 19790000.0 True 93517000.0
  1408443 0001104659-26-032415 2025 2026-03-20 11:08:00 True excludes_rd face_identity_anchor_minus_sga_and_rd_equals_operating_income 12 USD 17274000.0 17274000.0 True 18108000.0
  1317833 0001493152-23-012706 2022 2023-04-18 10:04:00 True excludes_rd face_identity_total_equals_sga_plus_rd 10 USD 274379.0 274379.0 True 98450.0
  1817713 0001193125-26-076918 2025 2026-02-26 21:32:00 True excludes_rd face_identity_total_equals_sga_plus_rd 14 USD 41771000.0 41771000.0 True 125896000.0
  1454938 0001454938-26-000015 2025 2026-03-16 20:44:00 True excludes_rd face_identity_total_equals_sga_plus_rd 24 USD 401485000.0 401485000.0 True 43554000.0
  1689657 0001689657-23-000009 2022 2023-02-23 21:38:00 True excludes_rd face_identity_total_equals_sga_plus_rd 18 USD 95115000.0 95115000.0 True 53207000.0
  1927719 0001104659-26-034941 2025 2026-03-26 13:14:00 True unresolved no_total_line_after_sga_and_rd 25 USD 26897000.0 11694000.0 False 11942000.0
  1828811 0001828811-25-000009 2024 2025-03-31 21:24:00 True excludes_rd face_identity_anchor_minus_sga_and_rd_equals_operating_income 17 USD 75300000.0 75300000.0 True 1600000.0
  1893645 0001213900-26-034198 2025 2026-03-25 21:23:00 True unresolved face_identity_not_found 9 USD 4116000.0 4116000.0 True 2133000.0
  1991592 0001213900-26-024760 2025 2026-03-06 22:01:00 True excludes_rd face_identity_total_equals_sga_plus_rd 23 USD 8043901.0 8043901.0 True 2067322.0
  1178711 0001628280-23-008187 2022 2023-03-16 12:21:00 True excludes_rd face_identity_total_equals_sga_plus_rd 19 USD 145598000.0 145598000.0 True 37823000.0
  1133869 0001104659-26-029580 2025 2026-03-17 21:01:00 True excludes_rd face_identity_total_equals_sga_plus_rd 20 USD 23687535.0 23687535.0 True 84454595.0
  889331 0001628280-26-009585 2025 2026-02-19 18:50:00 True excludes_rd face_identity_total_equals_sga_plus_rd 19 USD 381773000.0 381773000.0 True 106899000.0
  1561743 0001561743-21-000024 2020 2021-03-16 20:07:00 True excludes_rd face_identity_total_equals_sga_plus_rd 13 USD 21979000.0 21979000.0 True 31281000.0
  1713923 0001104659-26-048221 2025 2026-04-24 13:12:00 True unresolved face_identity_not_found 25 CNY 32154239.66987777 231081000.0 False 10092335.60204532
  1473579 0001213900-23-028171 2022 2023-04-07 14:13:00 True excludes_rd face_identity_total_equals_sga_plus_rd 22 USD 5293379.0 5293379.0 True 3159239.0
  1814140 0000950170-23-003749 2022 2023-02-22 21:09:00 True excludes_rd face_identity_total_equals_sga_plus_rd 13 USD 9651000.0 9651000.0 True 23035000.0
  1000184 0001104659-26-020058 2025 2026-02-26 12:01:00 True unresolved sga_not_on_face 37 EUR 11885094259.200024 None None 7499413072.8000145
  1551306 0001551306-26-000020 2025 2026-02-27 14:19:00 True unresolved rd_not_on_face 15 USD 219207000.0 219207000.0 True 9500000.0
  1637890 0001193125-23-078009 2022 2023-03-23 21:03:00 True unresolved no_total_line_after_sga_and_rd 17 EUR 11109493.872000007 10546000.0 False 19939360.896000013
  67347 0001104659-26-066795 2025 2026-05-27 18:17:00 True unresolved rd_not_on_face 20 USD 360100000.0 360100000.0 True 29300000.0
  1403752 0001403752-22-000005 2021 2022-02-24 21:09:00 True excludes_rd face_identity_total_equals_sga_plus_rd 16 USD 42561000.0 42561000.0 True 199752000.0
  704532 0001193125-26-066937 2025 2026-02-24 21:04:00 True unresolved face_identity_not_found 19 USD 177032000.0 177032000.0 True 131978000.0
  1370946 0001370946-26-000067 2025 2026-02-25 11:37:00 True unresolved face_identity_not_found 30 USD 1014000000.0 1014000000.0 True 150000000.0
  1561627 0001564590-21-012530 2020 2021-03-11 21:20:00 True unresolved face_identity_not_found 19 USD 20953000.0 20953000.0 True 8845000.0
  1078207 0001078207-24-000092 2023 2024-08-05 23:32:00 True excludes_rd face_identity_total_equals_sga_plus_rd 30 USD 71297000.0 71297000.0 True 16528000.0
  1500412 0001193125-26-094004 2025 2026-03-05 21:18:00 True excludes_rd face_identity_total_equals_sga_plus_rd 21 USD 33152000.0 33152000.0 True 38486000.0
  1462223 0001683168-26-002951 2025 2026-04-15 12:31:00 True excludes_rd face_identity_total_equals_sga_plus_rd 30 USD 7854000.0 7854000.0 True 246000.0
  1759509 0001628280-26-006960 2025 2026-02-10 22:50:00 True excludes_rd face_identity_total_equals_sga_plus_rd 18 USD 1877231000.0 1877231000.0 True 451419000.0
  1822462 0001628280-26-016814 2025 2026-03-11 20:12:00 True unresolved face_identity_not_found 19 USD 27550000.0 27550000.0 True 85466000.0
  1851657 0001562762-24-000071 2023 2024-03-27 20:19:00 True excludes_rd face_identity_total_equals_sga_plus_rd 17 USD 22386000.0 22386000.0 True 35899000.0
  1766600 0001193125-26-102524 2025 2026-03-11 21:50:00 True unresolved no_total_line_after_sga_and_rd 29 CAD 140840346.23701477 182162000.0 False 350083.767403052
  1611647 0001437749-21-003615 2020 2021-02-22 21:11:00 True unresolved rd_not_on_face 18 USD 134908000.0 134908000.0 True 800000.0
  1781405 0001731122-23-000547 2022 2023-03-31 21:05:00 True excludes_rd face_identity_total_equals_sga_plus_rd 16 USD 2670523.0 2670523.0 True 2085815.0
  1613170 0001104659-21-043345 2020 2021-03-29 21:02:00 True unresolved no_total_line_after_sga_and_rd 16 USD 33053000.0 8503000.0 False 6002000.0
  1579241 0001579241-26-000007 2025 2026-02-17 11:49:00 True unresolved rd_not_on_face 29 USD 978800000.0 978800000.0 True 132000000.0
  1104657 0001104657-26-000011 2025 2026-02-12 21:55:00 True unresolved face_identity_not_found 20 USD 143057000.0 143057000.0 True 25941000.0
  1300699 0000950170-23-008734 2022 2023-03-20 18:05:00 True excludes_rd face_identity_total_equals_sga_plus_rd 29 USD 44880000.0 44880000.0 True 51758000.0
  1981462 0001213900-24-112300 2024 2024-12-26 12:20:00 True unresolved no_total_line_after_sga_and_rd 31 CAD 15232050.766732834 20722468.0 False 5474711.442415426

history: issuers with a first-reported row per fiscal year, primary window 2020-2026 (all templates, every vintage stamp)
  sql: SELECT fyear, count(DISTINCT cik), count(DISTINCT cik) FILTER (WHERE available_at < TIMESTAMP '2024-01-01') FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2010/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2011/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2012/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2013/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2014/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2015/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2016/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2017/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2018/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2019/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2020/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2021/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2022/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2023/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2024/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2025/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2026/funda.parquet']) WHERE first_reported AND fyear BETWEEN 2020 AND 2026 GROUP BY 1 ORDER BY 1
  fyear status issuers
  2020 completeness_unverified 6727
  2021 completeness_unverified 7586
  2022 completeness_unverified 7346
  2023 completeness_unverified 6914
  2024 completeness_unverified 6714
  2025 completeness_unverified 6416
  2026 partial 143

history: issuers with a first-reported row per fiscal year, original window 2009-2023 (supporting; all templates)
  sql: SELECT fyear, count(DISTINCT cik), count(DISTINCT cik) FILTER (WHERE available_at < TIMESTAMP '2024-01-01') FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2010/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2011/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2012/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2013/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2014/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2015/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2016/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2017/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2018/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2019/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2020/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2021/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2022/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2023/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2024/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2025/funda.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/compustat_std_parts/parts/year=2026/funda.parquet']) WHERE first_reported AND fyear BETWEEN 2009 AND 2023 GROUP BY 1 ORDER BY 1
  fyear issuers stamped<2024
  2009 458 458
  2010 1456 1456
  2011 7378 7378
  2012 7577 7577
  2013 7384 7384
  2014 7254 7254
  2015 6873 6873
  2016 6476 6476
  2017 6692 6692
  2018 6637 6637
  2019 6485 6484
  2020 6727 6725
  2021 7586 7581
  2022 7346 7312
  2023 6914 658

```

## CPI build

```text
$ C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --heavy --job-gb 0.5 --wait-minutes 90 --disk-path C:/ --min-free-disk-gb 25 --quiet --receipt .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\guard-cpi-build-sol.json --stdout .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\cpi-build-sol.stdout --stderr .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\cpi-build-sol.stderr -- C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.cpi_u_nsa build
{
  "stage": "C:\\atx\\atx-db\\data\\alpha_panel\\v1\\cpi_u_nsa",
  "manifest_sha256": "06a63d16a43a9c821f213bf819b7a0971bc71b796536305ede9fa9dadb26d6e6",
  "rows": 227,
  "first_release": 223,
  "revision": 4,
  "unchanged_republications": 24749,
  "per_part": {
    "2008": 11,
    "2009": 12,
    "2010": 12,
    "2011": 12,
    "2012": 12,
    "2013": 12,
    "2014": 12,
    "2015": 12,
    "2016": 16,
    "2017": 12,
    "2018": 12,
    "2019": 12,
    "2020": 12,
    "2021": 12,
    "2022": 12,
    "2023": 12,
    "2024": 12,
    "2025": 11,
    "2026": 9
  },
  "coverage": {
    "obs_start": "2008-01-01",
    "first_ref_month": "2008-01-01",
    "latest_ref_month": "2026-08-01",
    "latest_vintage": "2026-09-11",
    "alfred_page_latest_observation": "2026-08-01",
    "vintages_listed": 931,
    "vintages_used": 224,
    "missing_ref_months": [
      "2025-10-01"
    ],
    "primary_window_ref_months": 79,
    "observed_max_days_month_end_to_next_release": [
      79,
      "2025-09-01"
    ]
  },
  "crosscheck": {
    "fred_latest": {
      "compared": 223,
      "first": "2008-01-01",
      "last": "2026-08-01",
      "mismatches": [],
      "only_in_source": [],
      "blank_in_source": [
        "2025-10-01"
      ],
      "missing_in_source": []
    },
    "bls_api_v1": {
      "compared": 31,
      "first": "2024-01-01",
      "last": "2026-08-01",
      "mismatches": [],
      "only_in_source": [],
      "blank_in_source": [
        "2025-10-01"
      ],
      "missing_in_source": []
    }
  }
}

```

## CPI acceptance

```text
$ C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --heavy --job-gb 0.5 --wait-minutes 90 --disk-path C:/ --min-free-disk-gb 25 --quiet --receipt .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\guard-cpi-accept-sol.json --stdout .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\cpi-accept-sol.stdout --stderr .superpowers\sdd\2026-10-04-p9-al-data-atx-db-request\task-8-receipts\cpi-accept-sol.stderr -- C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.cpi_u_nsa accept
$ python -m atx_db.alpha_panel.cpi_u_nsa accept --stage cpi_u_nsa

(i) C5 manifest self-check (engine PinnedStage contract; every listed file re-hashed)
  manifest C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/manifest.json bytes=35381 sha256=06a63d16a43a9c821f213bf819b7a0971bc71b796536305ede9fa9dadb26d6e6
  contract errors=0 []
  files listed=19 verified=19 bad=0 [] unlisted=0 []

(iv) part year check (available_at inside the part's UTC year)
  parts/year=2008/cpi_u_nsa.parquet rows=11 min=2008-02-20 13:30:00 max=2008-12-16 13:30:00 outside_year=0
  parts/year=2009/cpi_u_nsa.parquet rows=12 min=2009-01-16 13:30:00 max=2009-12-16 13:30:00 outside_year=0
  parts/year=2010/cpi_u_nsa.parquet rows=12 min=2010-01-15 13:30:00 max=2010-12-15 13:30:00 outside_year=0
  parts/year=2011/cpi_u_nsa.parquet rows=12 min=2011-01-14 13:30:00 max=2011-12-16 13:30:00 outside_year=0
  parts/year=2012/cpi_u_nsa.parquet rows=12 min=2012-01-19 13:30:00 max=2012-12-14 13:30:00 outside_year=0
  parts/year=2013/cpi_u_nsa.parquet rows=12 min=2013-01-16 13:30:00 max=2013-12-17 13:30:00 outside_year=0
  parts/year=2014/cpi_u_nsa.parquet rows=12 min=2014-01-16 13:30:00 max=2014-12-17 13:30:00 outside_year=0
  parts/year=2015/cpi_u_nsa.parquet rows=12 min=2015-01-16 13:30:00 max=2015-12-15 13:30:00 outside_year=0
  parts/year=2016/cpi_u_nsa.parquet rows=16 min=2016-01-20 13:30:00 max=2016-12-15 13:30:00 outside_year=0
  parts/year=2017/cpi_u_nsa.parquet rows=12 min=2017-01-18 13:30:00 max=2017-12-13 13:30:00 outside_year=0
  parts/year=2018/cpi_u_nsa.parquet rows=12 min=2018-01-12 13:30:00 max=2018-12-12 13:30:00 outside_year=0
  parts/year=2019/cpi_u_nsa.parquet rows=12 min=2019-01-11 13:30:00 max=2019-12-11 13:30:00 outside_year=0
  parts/year=2020/cpi_u_nsa.parquet rows=12 min=2020-01-14 13:30:00 max=2020-12-10 13:30:00 outside_year=0
  parts/year=2021/cpi_u_nsa.parquet rows=12 min=2021-01-13 13:30:00 max=2021-12-10 13:30:00 outside_year=0
  parts/year=2022/cpi_u_nsa.parquet rows=12 min=2022-01-12 13:30:00 max=2022-12-13 13:30:00 outside_year=0
  parts/year=2023/cpi_u_nsa.parquet rows=12 min=2023-01-12 13:30:00 max=2023-12-12 13:30:00 outside_year=0
  parts/year=2024/cpi_u_nsa.parquet rows=12 min=2024-01-11 13:30:00 max=2024-12-11 13:30:00 outside_year=0
  parts/year=2025/cpi_u_nsa.parquet rows=11 min=2025-01-15 13:30:00 max=2025-12-18 13:30:00 outside_year=0
  parts/year=2026/cpi_u_nsa.parquet rows=9 min=2026-01-13 13:30:00 max=2026-09-11 12:30:00 outside_year=0
  rows_outside_year=0  (i)+(iv) PASS

coverage: ref months 2008-01-01 .. 2026-08-01 (latest vintage 2026-09-11; ALFRED page latest observation 2026-08-01); 224 of 931 listed vintages used (from 2008-01-01); the year of the latest vintage is partial
  missing ref months (no vintage holds them): ['2025-10-01']
  counts {'first_release': 223, 'per_part': {'2008': 11, '2009': 12, '2010': 12, '2011': 12, '2012': 12, '2013': 12, '2014': 12, '2015': 12, '2016': 16, '2017': 12, '2018': 12, '2019': 12, '2020': 12, '2021': 12, '2022': 12, '2023': 12, '2024': 12, '2025': 11, '2026': 9}, 'revision': 4, 'rows': 227, 'unchanged_republications': 24749}
  excluded [{'reason': 'month already present in the first selected vintage: first release not observable', 'ref_months': []}]
  vanished []
  observed max days from month end to the next month's first release: [79, '2025-09-01'] (staleness_days 100)
  network: 28 of 30 requests (ledger C:\atx\atx-db\data\raw\cpi_u_nsa\receipts.jsonl sha256 35c17b70b6af52a5355b1b9a1868c36296ce9c8b9def86e14a8d9de479352ec2)
  crosscheck bls_api_v1: {"blank_in_source": ["2025-10-01"], "compared": 31, "first": "2024-01-01", "last": "2026-08-01", "mismatches": [], "missing_in_source": [], "only_in_source": []}
  crosscheck fred_latest: {"blank_in_source": ["2025-10-01"], "compared": 223, "first": "2008-01-01", "last": "2026-08-01", "mismatches": [], "missing_in_source": [], "only_in_source": []}

primary window 2020+ by reference year
  sql: SELECT year(ref_month), count(*) FILTER (WHERE vintage_kind = 'first_release'), count(*) FILTER (WHERE vintage_kind = 'revision'), min(available_at), max(available_at), string_agg(DISTINCT clock_basis, ',') FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2008/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2009/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2010/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2011/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2012/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2013/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2014/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2015/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2016/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2017/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2018/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2019/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2020/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2021/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2022/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2023/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2024/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2025/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2026/cpi_u_nsa.parquet']) WHERE ref_month >= DATE '2020-01-01' GROUP BY 1 ORDER BY 1
  ref_year first_release revision min_available_at max_available_at clock_basis
  2020 12 0 2020-02-13 13:30:00 2021-01-13 13:30:00 alfred_vintage_date_0830_et
  2021 12 0 2021-02-10 13:30:00 2022-01-12 13:30:00 alfred_vintage_date_0830_et
  2022 12 0 2022-02-10 13:30:00 2023-01-12 13:30:00 alfred_vintage_date_0830_et
  2023 12 0 2023-02-14 13:30:00 2024-01-11 13:30:00 alfred_vintage_date_0830_et
  2024 12 0 2024-02-13 13:30:00 2025-01-15 13:30:00 alfred_vintage_date_0830_et
  2025 11 0 2025-02-12 13:30:00 2026-01-13 13:30:00 alfred_vintage_date_0830_et
  2026 8 0 2026-02-13 13:30:00 2026-09-11 12:30:00 alfred_vintage_date_0830_et

rows 2020-01-01+ (ref_month vintage_date available_at_utc value kind)
  sql: SELECT ref_month, vintage_date, available_at, value, vintage_kind FROM read_parquet(['C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2008/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2009/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2010/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2011/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2012/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2013/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2014/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2015/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2016/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2017/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2018/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2019/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2020/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2021/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2022/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2023/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2024/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2025/cpi_u_nsa.parquet', 'C:/atx/atx-db/data/alpha_panel/v1/cpi_u_nsa/parts/year=2026/cpi_u_nsa.parquet']) WHERE ref_month >= DATE '2020-01-01' ORDER BY ref_month, available_at
  2020-01-01 2020-02-13 2020-02-13 13:30:00 257.971 first_release
  2020-02-01 2020-03-11 2020-03-11 12:30:00 258.678 first_release
  2020-03-01 2020-04-10 2020-04-10 12:30:00 258.115 first_release
  2020-04-01 2020-05-12 2020-05-12 12:30:00 256.389 first_release
  2020-05-01 2020-06-10 2020-06-10 12:30:00 256.394 first_release
  2020-06-01 2020-07-14 2020-07-14 12:30:00 257.797 first_release
  2020-07-01 2020-08-12 2020-08-12 12:30:00 259.101 first_release
  2020-08-01 2020-09-11 2020-09-11 12:30:00 259.918 first_release
  2020-09-01 2020-10-13 2020-10-13 12:30:00 260.28 first_release
  2020-10-01 2020-11-12 2020-11-12 13:30:00 260.388 first_release
  2020-11-01 2020-12-10 2020-12-10 13:30:00 260.229 first_release
  2020-12-01 2021-01-13 2021-01-13 13:30:00 260.474 first_release
  2021-01-01 2021-02-10 2021-02-10 13:30:00 261.582 first_release
  2021-02-01 2021-03-10 2021-03-10 13:30:00 263.014 first_release
  2021-03-01 2021-04-13 2021-04-13 12:30:00 264.877 first_release
  2021-04-01 2021-05-12 2021-05-12 12:30:00 267.054 first_release
  2021-05-01 2021-06-10 2021-06-10 12:30:00 269.195 first_release
  2021-06-01 2021-07-13 2021-07-13 12:30:00 271.696 first_release
  2021-07-01 2021-08-11 2021-08-11 12:30:00 273.003 first_release
  2021-08-01 2021-09-14 2021-09-14 12:30:00 273.567 first_release
  2021-09-01 2021-10-13 2021-10-13 12:30:00 274.31 first_release
  2021-10-01 2021-11-10 2021-11-10 13:30:00 276.589 first_release
  2021-11-01 2021-12-10 2021-12-10 13:30:00 277.948 first_release
  2021-12-01 2022-01-12 2022-01-12 13:30:00 278.802 first_release
  2022-01-01 2022-02-10 2022-02-10 13:30:00 281.148 first_release
  2022-02-01 2022-03-10 2022-03-10 13:30:00 283.716 first_release
  2022-03-01 2022-04-12 2022-04-12 12:30:00 287.504 first_release
  2022-04-01 2022-05-11 2022-05-11 12:30:00 289.109 first_release
  2022-05-01 2022-06-10 2022-06-10 12:30:00 292.296 first_release
  2022-06-01 2022-07-13 2022-07-13 12:30:00 296.311 first_release
  2022-07-01 2022-08-10 2022-08-10 12:30:00 296.276 first_release
  2022-08-01 2022-09-13 2022-09-13 12:30:00 296.171 first_release
  2022-09-01 2022-10-13 2022-10-13 12:30:00 296.808 first_release
  2022-10-01 2022-11-10 2022-11-10 13:30:00 298.012 first_release
  2022-11-01 2022-12-13 2022-12-13 13:30:00 297.711 first_release
  2022-12-01 2023-01-12 2023-01-12 13:30:00 296.797 first_release
  2023-01-01 2023-02-14 2023-02-14 13:30:00 299.17 first_release
  2023-02-01 2023-03-14 2023-03-14 12:30:00 300.84 first_release
  2023-03-01 2023-04-12 2023-04-12 12:30:00 301.836 first_release
  2023-04-01 2023-05-10 2023-05-10 12:30:00 303.363 first_release
  2023-05-01 2023-06-13 2023-06-13 12:30:00 304.127 first_release
  2023-06-01 2023-07-12 2023-07-12 12:30:00 305.109 first_release
  2023-07-01 2023-08-10 2023-08-10 12:30:00 305.691 first_release
  2023-08-01 2023-09-13 2023-09-13 12:30:00 307.026 first_release
  2023-09-01 2023-10-12 2023-10-12 12:30:00 307.789 first_release
  2023-10-01 2023-11-14 2023-11-14 13:30:00 307.671 first_release
  2023-11-01 2023-12-12 2023-12-12 13:30:00 307.051 first_release
  2023-12-01 2024-01-11 2024-01-11 13:30:00 306.746 first_release
  2024-01-01 2024-02-13 2024-02-13 13:30:00 308.417 first_release
  2024-02-01 2024-03-12 2024-03-12 12:30:00 310.326 first_release
  2024-03-01 2024-04-10 2024-04-10 12:30:00 312.332 first_release
  2024-04-01 2024-05-15 2024-05-15 12:30:00 313.548 first_release
  2024-05-01 2024-06-12 2024-06-12 12:30:00 314.069 first_release
  2024-06-01 2024-07-11 2024-07-11 12:30:00 314.175 first_release
  2024-07-01 2024-08-14 2024-08-14 12:30:00 314.54 first_release
  2024-08-01 2024-09-11 2024-09-11 12:30:00 314.796 first_release
  2024-09-01 2024-10-10 2024-10-10 12:30:00 315.301 first_release
  2024-10-01 2024-11-13 2024-11-13 13:30:00 315.664 first_release
  2024-11-01 2024-12-11 2024-12-11 13:30:00 315.493 first_release
  2024-12-01 2025-01-15 2025-01-15 13:30:00 315.605 first_release
  2025-01-01 2025-02-12 2025-02-12 13:30:00 317.671 first_release
  2025-02-01 2025-03-12 2025-03-12 12:30:00 319.082 first_release
  2025-03-01 2025-04-10 2025-04-10 12:30:00 319.799 first_release
  2025-04-01 2025-05-13 2025-05-13 12:30:00 320.795 first_release
  2025-05-01 2025-06-11 2025-06-11 12:30:00 321.465 first_release
  2025-06-01 2025-07-15 2025-07-15 12:30:00 322.561 first_release
  2025-07-01 2025-08-12 2025-08-12 12:30:00 323.048 first_release
  2025-08-01 2025-09-11 2025-09-11 12:30:00 323.976 first_release
  2025-09-01 2025-10-24 2025-10-24 12:30:00 324.8 first_release
  2025-11-01 2025-12-18 2025-12-18 13:30:00 324.122 first_release
  2025-12-01 2026-01-13 2026-01-13 13:30:00 324.054 first_release
  2026-01-01 2026-02-13 2026-02-13 13:30:00 325.252 first_release
  2026-02-01 2026-03-11 2026-03-11 12:30:00 326.785 first_release
  2026-03-01 2026-04-10 2026-04-10 12:30:00 330.213 first_release
  2026-04-01 2026-05-12 2026-05-12 12:30:00 333.02 first_release
  2026-05-01 2026-06-10 2026-06-10 12:30:00 335.123 first_release
  2026-06-01 2026-07-14 2026-07-14 12:30:00 333.952 first_release
  2026-07-01 2026-08-12 2026-08-12 12:30:00 333.918 first_release
  2026-08-01 2026-09-11 2026-09-11 12:30:00 334.98 first_release

```
