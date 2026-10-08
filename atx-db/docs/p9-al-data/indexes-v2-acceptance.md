# Indexes v2 real-data acceptance

First immutable flat `indexes_v2/` publication on 2026-10-06. Primary coverage is 2020 through each measured source horizon; earlier 2018–2019 reconstitutions retain band/buffer state. Current 2026 is partial. Production activation and native-input rebuilding are outside this delivery.

R7.1(a) uses dated FINRA evidence only: no directory ETF/type decides eligibility. R7.2(d) removes backfill CIK grouping and operating influence, re-links from valid name evidence when available, and retains other lines as unlinked. It does not exclude a line because of later backfill survival.

Manifest SHA-256: `2c9be8b974d802d907414f9e325dfa6fc5f9ab2bd5c731c65672f6d834a70220`. Rules: `russell-proxy-v2, sp500-proxy-v2`. Clock: `index-vintage-mark-v2`. Producer git head: `80cff00aeae0b326db5a206a6014d256ce5687bd`. Source module hashes remain in the immutable manifest. Registry publication metadata is updated separately after this build.

New PIT input comes from reviewed `a0_check seed` and identity/backfill/names/combine against held immutable `session8-phased-r4`, under new root `data/research/identity_rehearsal/ix-pit-links-20261004/`. Original identity/link-table/combined files and r4 are preserved; this is not the future Task4 rehearsal.

## Measured PIT input and coverage

```json
{
  "bytes": 629377,
  "path": "C:\\atx\\atx-db\\data\\research\\identity_rehearsal\\ix-pit-links-20261004\\identity\\links_pit.parquet",
  "sha256": "376d2ce5a040e112d0cad15150bd90d9070198a0ca1ba87db942893213c6cb21",
  "tiers": [
    {
      "first_session": "2017-10-02",
      "first_stamp": "2012-08-07 22:00:00",
      "last_session": "2026-09-18",
      "last_stamp": "2026-09-14 22:00:00",
      "n_rows": 2614,
      "tier": "high"
    },
    {
      "first_session": "2017-10-02",
      "first_stamp": "2012-05-02 22:00:00",
      "last_session": "2026-09-18",
      "last_stamp": "2026-09-10 22:00:00",
      "n_rows": 3427,
      "tier": "medium"
    },
    {
      "first_session": "2018-01-11",
      "first_stamp": "2018-01-10 22:00:00",
      "last_session": "2026-09-18",
      "last_stamp": "2026-09-10 22:00:00",
      "n_rows": 123047,
      "tier": "name"
    }
  ]
}
```

```json
{
  "exchange_calendar_through": "2027-12-31",
  "last_reconstitution": {
    "R1000P": "2026-06-26",
    "R2000P": "2026-06-26",
    "R3000P": "2026-06-26",
    "SP500P": "2026-09-18"
  },
  "membership_through": "2026-09-21",
  "note": "constituents rows of the last reconstitution of each index run to the next scheduled one; exits and membership after the vendor data end are not yet known",
  "panel_first": "2018-01-02",
  "panel_last": "2026-09-18",
  "rank_dates": {
    "first": "2018-02-28",
    "last": "2026-08-31"
  },
  "vendor_data_end": "2026-09-18"
}
```

## Build assertions and count gates

Rank-vintage rows equal counts at all 62 reconstitutions. Directory-decided selections are zero at all 44 rank dates; backfill grouping/operating counts are 0/0 at every rank date. Exchange calendar equality passes. Exit rows are measured per index below. Every published part hash/size, receipt row count and available-year check passes. Published v1 and r4 hashes remain identical.

```json
{
  "exit_rows": {
    "R1000P": 1376,
    "R2000P": 1240,
    "R3000P": 2616,
    "SP500P": 862
  },
  "exchange_calendar": {
    "match": true,
    "overlap": [
      "2012-03-26",
      "2026-09-18"
    ],
    "rule": "nyse-sessions-v1",
    "sessions": 3642
  },
  "gate_status": {
    "companies": {
      "2018-06-22": 1110,
      "2019-06-28": 1073,
      "2020-06-26": 866,
      "2021-06-25": 1018,
      "2022-06-24": 921,
      "2023-06-23": 672,
      "2024-06-28": 503,
      "2025-06-27": 358,
      "2026-06-26": 203
    },
    "criterion": "R2000 proxy companies in [1900, 2100] at every reconstitution",
    "lines": {
      "2018-06-22": 1115,
      "2019-06-28": 1088,
      "2020-06-26": 886,
      "2021-06-25": 1042,
      "2022-06-24": 937,
      "2023-06-23": 685,
      "2024-06-28": 513,
      "2025-06-27": 367,
      "2026-06-26": 211
    },
    "official_overlap": "GAP: FTSE Russell / S&P DJI constituent lists are licensed (not measured)",
    "outside": {
      "2018-06-22": 1110,
      "2019-06-28": 1073,
      "2020-06-26": 866,
      "2021-06-25": 1018,
      "2022-06-24": 921,
      "2023-06-23": 672,
      "2024-06-28": 503,
      "2025-06-27": 358,
      "2026-06-26": 203
    },
    "pass": false
  },
  "focus_2020_plus": {
    "counts_on_recon": {
      "R1000P 2020-06-26": 984,
      "R1000P 2021-06-25": 1010,
      "R1000P 2022-06-24": 1008,
      "R1000P 2023-06-23": 873,
      "R1000P 2024-06-28": 779,
      "R1000P 2025-06-27": 695,
      "R1000P 2026-06-26": 591,
      "R2000P 2020-06-26": 886,
      "R2000P 2021-06-25": 1042,
      "R2000P 2022-06-24": 937,
      "R2000P 2023-06-23": 685,
      "R2000P 2024-06-28": 513,
      "R2000P 2025-06-27": 367,
      "R2000P 2026-06-26": 211,
      "R3000P 2020-06-26": 1870,
      "R3000P 2021-06-25": 2052,
      "R3000P 2022-06-24": 1945,
      "R3000P 2023-06-23": 1558,
      "R3000P 2024-06-28": 1292,
      "R3000P 2025-06-27": 1062,
      "R3000P 2026-06-26": 802,
      "SP500P 2020-03-20": 522,
      "SP500P 2020-06-19": 524,
      "SP500P 2020-09-18": 522,
      "SP500P 2020-12-18": 526,
      "SP500P 2021-03-19": 524,
      "SP500P 2021-06-18": 529,
      "SP500P 2021-09-17": 521,
      "SP500P 2021-12-17": 525,
      "SP500P 2022-03-18": 518,
      "SP500P 2022-06-17": 517,
      "SP500P 2022-09-16": 514,
      "SP500P 2022-12-16": 498,
      "SP500P 2023-03-17": 484,
      "SP500P 2023-06-16": 468,
      "SP500P 2023-09-15": 476,
      "SP500P 2023-12-15": 443,
      "SP500P 2024-03-15": 446,
      "SP500P 2024-06-21": 430,
      "SP500P 2024-09-20": 426,
      "SP500P 2024-12-20": 409,
      "SP500P 2025-03-21": 405,
      "SP500P 2025-06-20": 394,
      "SP500P 2025-09-19": 382,
      "SP500P 2025-12-19": 373,
      "SP500P 2026-03-20": 347,
      "SP500P 2026-06-18": 330,
      "SP500P 2026-09-18": 298
    },
    "from": "2020-01-01",
    "gate": {
      "companies": {
        "2020-06-26": 866,
        "2021-06-25": 1018,
        "2022-06-24": 921,
        "2023-06-23": 672,
        "2024-06-28": 503,
        "2025-06-27": 358,
        "2026-06-26": 203
      },
      "criterion": "R2000 proxy companies in [1900, 2100] at every reconstitution",
      "lines": {
        "2020-06-26": 886,
        "2021-06-25": 1042,
        "2022-06-24": 937,
        "2023-06-23": 685,
        "2024-06-28": 513,
        "2025-06-27": 367,
        "2026-06-26": 211
      },
      "official_overlap": "GAP: FTSE Russell / S&P DJI constituent lists are licensed (not measured)",
      "outside": {
        "2020-06-26": 866,
        "2021-06-25": 1018,
        "2022-06-24": 921,
        "2023-06-23": 672,
        "2024-06-28": 503,
        "2025-06-27": 358,
        "2026-06-26": 203
      },
      "pass": false
    },
    "through": "2026-09-18"
  },
  "preserved": {
    "C:\\atx\\atx-db\\data\\alpha_panel\\v1\\indexes": {
      "files": 12,
      "pass": true
    },
    "C:\\atx\\atx-db\\data\\research\\identity_rehearsal\\session8-phased-r4": {
      "files": 49,
      "pass": true
    }
  }
}
```

R2000 company band remains **[1900, 2100]**. FAIL counts are diagnostic published results and do not become PASS because the writer completed. Licensed official-list overlap remains unmeasured.

## Every reconstitution: measured counts and v1 difference

The diff is descriptive, not an equality gate. Rank rows/counts are the A.5 receipt keys.

| index | rank | recon | rank rows | counts | v1 lines | line diff | v1 companies | v2 companies | company diff | R2000 gate |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| SP500P | 2018-02-28 | 2018-03-16 | 501 | 501 | 501 | 0 | 500 | 500 | 0 | — |
| R1000P | 2018-05-11 | 2018-06-22 | 1013 | 1013 | 1023 | -10 | 1000 | 1000 | 0 | — |
| R2000P | 2018-05-11 | 2018-06-22 | 1115 | 1115 | 2019 | -904 | 2000 | 1110 | -890 | FAIL |
| R3000P | 2018-05-11 | 2018-06-22 | 2128 | 2128 | 3042 | -914 | 3000 | 2110 | -890 | — |
| SP500P | 2018-05-31 | 2018-06-15 | 505 | 505 | 508 | -3 | 500 | 500 | 0 | — |
| SP500P | 2018-08-31 | 2018-09-21 | 520 | 520 | 532 | -12 | 500 | 500 | 0 | — |
| SP500P | 2018-11-30 | 2018-12-21 | 519 | 519 | 533 | -14 | 500 | 500 | 0 | — |
| SP500P | 2019-02-28 | 2019-03-15 | 517 | 517 | 536 | -19 | 500 | 500 | 0 | — |
| R1000P | 2019-05-10 | 2019-06-28 | 1031 | 1031 | 1041 | -10 | 988 | 983 | -5 | — |
| R2000P | 2019-05-10 | 2019-06-28 | 1088 | 1088 | 2080 | -992 | 2012 | 1073 | -939 | FAIL |
| R3000P | 2019-05-10 | 2019-06-28 | 2119 | 2119 | 3121 | -1002 | 3000 | 2056 | -944 | — |
| SP500P | 2019-05-31 | 2019-06-21 | 522 | 522 | 533 | -11 | 500 | 500 | 0 | — |
| SP500P | 2019-08-30 | 2019-09-20 | 516 | 516 | 531 | -15 | 500 | 500 | 0 | — |
| SP500P | 2019-11-29 | 2019-12-20 | 519 | 519 | 533 | -14 | 500 | 500 | 0 | — |
| SP500P | 2020-02-28 | 2020-03-20 | 522 | 522 | 533 | -11 | 500 | 500 | 0 | — |
| R1000P | 2020-05-08 | 2020-06-26 | 984 | 984 | 1048 | -64 | 998 | 937 | -61 | — |
| R2000P | 2020-05-08 | 2020-06-26 | 886 | 886 | 2056 | -1170 | 2002 | 866 | -1136 | FAIL |
| R3000P | 2020-05-08 | 2020-06-26 | 1870 | 1870 | 3104 | -1234 | 3000 | 1803 | -1197 | — |
| SP500P | 2020-05-29 | 2020-06-19 | 524 | 524 | 531 | -7 | 500 | 500 | 0 | — |
| SP500P | 2020-08-31 | 2020-09-18 | 522 | 522 | 532 | -10 | 500 | 500 | 0 | — |
| SP500P | 2020-11-30 | 2020-12-18 | 526 | 526 | 531 | -5 | 500 | 500 | 0 | — |
| SP500P | 2021-02-26 | 2021-03-19 | 524 | 524 | 529 | -5 | 500 | 500 | 0 | — |
| R1000P | 2021-05-07 | 2021-06-25 | 1010 | 1010 | 1061 | -51 | 1011 | 958 | -53 | — |
| R2000P | 2021-05-07 | 2021-06-25 | 1042 | 1042 | 2054 | -1012 | 1989 | 1018 | -971 | FAIL |
| R3000P | 2021-05-07 | 2021-06-25 | 2052 | 2052 | 3115 | -1063 | 3000 | 1976 | -1024 | — |
| SP500P | 2021-05-28 | 2021-06-18 | 529 | 529 | 530 | -1 | 500 | 500 | 0 | — |
| SP500P | 2021-08-31 | 2021-09-17 | 521 | 521 | 528 | -7 | 500 | 500 | 0 | — |
| SP500P | 2021-11-30 | 2021-12-17 | 525 | 525 | 532 | -7 | 500 | 500 | 0 | — |
| SP500P | 2022-02-28 | 2022-03-18 | 518 | 518 | 529 | -11 | 500 | 500 | 0 | — |
| R1000P | 2022-05-06 | 2022-06-24 | 1008 | 1008 | 1049 | -41 | 1005 | 974 | -31 | — |
| R2000P | 2022-05-06 | 2022-06-24 | 937 | 937 | 2048 | -1111 | 1995 | 921 | -1074 | FAIL |
| R3000P | 2022-05-06 | 2022-06-24 | 1945 | 1945 | 3097 | -1152 | 3000 | 1895 | -1105 | — |
| SP500P | 2022-05-31 | 2022-06-17 | 517 | 517 | 529 | -12 | 500 | 500 | 0 | — |
| SP500P | 2022-08-31 | 2022-09-16 | 514 | 514 | 531 | -17 | 500 | 500 | 0 | — |
| SP500P | 2022-11-30 | 2022-12-16 | 498 | 498 | 530 | -32 | 500 | 485 | -15 | — |
| SP500P | 2023-02-28 | 2023-03-17 | 484 | 484 | 526 | -42 | 500 | 471 | -29 | — |
| R1000P | 2023-04-28 | 2023-06-23 | 873 | 873 | 1023 | -150 | 984 | 844 | -140 | — |
| R2000P | 2023-04-28 | 2023-06-23 | 685 | 685 | 2066 | -1381 | 2016 | 672 | -1344 | FAIL |
| R3000P | 2023-04-28 | 2023-06-23 | 1558 | 1558 | 3089 | -1531 | 3000 | 1516 | -1484 | — |
| SP500P | 2023-05-31 | 2023-06-16 | 468 | 468 | 527 | -59 | 500 | 454 | -46 | — |
| SP500P | 2023-08-31 | 2023-09-15 | 476 | 476 | 526 | -50 | 500 | 461 | -39 | — |
| SP500P | 2023-11-30 | 2023-12-15 | 443 | 443 | 528 | -85 | 500 | 428 | -72 | — |
| SP500P | 2024-02-29 | 2024-03-15 | 446 | 446 | 526 | -80 | 500 | 431 | -69 | — |
| R1000P | 2024-04-30 | 2024-06-28 | 779 | 779 | 1029 | -250 | 984 | 751 | -233 | — |
| R2000P | 2024-04-30 | 2024-06-28 | 513 | 513 | 2065 | -1552 | 2016 | 503 | -1513 | FAIL |
| R3000P | 2024-04-30 | 2024-06-28 | 1292 | 1292 | 3094 | -1802 | 3000 | 1254 | -1746 | — |
| SP500P | 2024-05-31 | 2024-06-21 | 430 | 430 | 526 | -96 | 500 | 416 | -84 | — |
| SP500P | 2024-08-30 | 2024-09-20 | 426 | 426 | 527 | -101 | 500 | 410 | -90 | — |
| SP500P | 2024-11-29 | 2024-12-20 | 409 | 409 | 527 | -118 | 500 | 395 | -105 | — |
| SP500P | 2025-02-28 | 2025-03-21 | 405 | 405 | 527 | -122 | 500 | 391 | -109 | — |
| R1000P | 2025-04-30 | 2025-06-27 | 695 | 695 | 1036 | -341 | 993 | 672 | -321 | — |
| R2000P | 2025-04-30 | 2025-06-27 | 367 | 367 | 2056 | -1689 | 2007 | 358 | -1649 | FAIL |
| R3000P | 2025-04-30 | 2025-06-27 | 1062 | 1062 | 3092 | -2030 | 3000 | 1030 | -1970 | — |
| SP500P | 2025-05-30 | 2025-06-20 | 394 | 394 | 527 | -133 | 500 | 381 | -119 | — |
| SP500P | 2025-08-29 | 2025-09-19 | 382 | 382 | 527 | -145 | 500 | 371 | -129 | — |
| SP500P | 2025-11-28 | 2025-12-19 | 373 | 373 | 526 | -153 | 500 | 363 | -137 | — |
| SP500P | 2026-02-27 | 2026-03-20 | 347 | 347 | 528 | -181 | 500 | 336 | -164 | — |
| R1000P | 2026-04-30 | 2026-06-26 | 591 | 591 | 1036 | -445 | 996 | 573 | -423 | — |
| R2000P | 2026-04-30 | 2026-06-26 | 211 | 211 | 2046 | -1835 | 2004 | 203 | -1801 | FAIL |
| R3000P | 2026-04-30 | 2026-06-26 | 802 | 802 | 3082 | -2280 | 3000 | 776 | -2224 | — |
| SP500P | 2026-05-29 | 2026-06-18 | 330 | 330 | 525 | -195 | 500 | 318 | -182 | — |
| SP500P | 2026-08-31 | 2026-09-18 | 298 | 298 | None | None | None | 290 | None | — |


## Every rank: measurement and prohibited influence receipts

| rank | v2 eligible ME≥30m | v1 R2000 directory lines | v1 R2000 backfill lines | directory decided | backfill keys | backfill operating exclusion |
| --- | --- | --- | --- | --- | --- | --- |
| 2018-02-28 | 1492 | — | — | 0 | 0 | 0 |
| 2018-05-11 | 2110 | 947 | 151 | 0 | 0 | 0 |
| 2018-05-31 | 2111 | — | — | 0 | 0 | 0 |
| 2018-08-31 | 2218 | — | — | 0 | 0 | 0 |
| 2018-11-30 | 2163 | — | — | 0 | 0 | 0 |
| 2019-02-28 | 2112 | — | — | 0 | 0 | 0 |
| 2019-05-10 | 2056 | 1013 | 173 | 0 | 0 | 0 |
| 2019-05-31 | 2037 | — | — | 0 | 0 | 0 |
| 2019-08-30 | 1992 | — | — | 0 | 0 | 0 |
| 2019-11-29 | 1919 | — | — | 0 | 0 | 0 |
| 2020-02-28 | 1902 | — | — | 0 | 0 | 0 |
| 2020-05-08 | 1803 | 1058 | 169 | 0 | 0 | 0 |
| 2020-05-29 | 1842 | — | — | 0 | 0 | 0 |
| 2020-08-31 | 1880 | — | — | 0 | 0 | 0 |
| 2020-11-30 | 1934 | — | — | 0 | 0 | 0 |
| 2021-02-26 | 2016 | — | — | 0 | 0 | 0 |
| 2021-05-07 | 1976 | 1128 | 191 | 0 | 0 | 0 |
| 2021-05-28 | 2000 | — | — | 0 | 0 | 0 |
| 2021-08-31 | 2094 | — | — | 0 | 0 | 0 |
| 2021-11-30 | 2092 | — | — | 0 | 0 | 0 |
| 2022-02-28 | 1976 | — | — | 0 | 0 | 0 |
| 2022-05-06 | 1895 | 1226 | 277 | 0 | 0 | 0 |
| 2022-05-31 | 1879 | — | — | 0 | 0 | 0 |
| 2022-08-31 | 1813 | — | — | 0 | 0 | 0 |
| 2022-11-30 | 1673 | — | — | 0 | 0 | 0 |
| 2023-02-28 | 1621 | — | — | 0 | 0 | 0 |
| 2023-04-28 | 1516 | 1309 | 277 | 0 | 0 | 0 |
| 2023-05-31 | 1509 | — | — | 0 | 0 | 0 |
| 2023-08-31 | 1430 | — | — | 0 | 0 | 0 |
| 2023-11-30 | 1326 | — | — | 0 | 0 | 0 |
| 2024-02-29 | 1292 | — | — | 0 | 0 | 0 |
| 2024-04-30 | 1254 | 1406 | 273 | 0 | 0 | 0 |
| 2024-05-31 | 1262 | — | — | 0 | 0 | 0 |
| 2024-08-30 | 1206 | — | — | 0 | 0 | 0 |
| 2024-11-29 | 1164 | — | — | 0 | 0 | 0 |
| 2025-02-28 | 1098 | — | — | 0 | 0 | 0 |
| 2025-04-30 | 1030 | 1489 | 300 | 0 | 0 | 0 |
| 2025-05-30 | 1036 | — | — | 0 | 0 | 0 |
| 2025-08-29 | 998 | — | — | 0 | 0 | 0 |
| 2025-11-28 | 925 | — | — | 0 | 0 | 0 |
| 2026-02-27 | 836 | — | — | 0 | 0 | 0 |
| 2026-04-30 | 776 | 1629 | 310 | 0 | 0 | 0 |
| 2026-05-29 | 755 | — | — | 0 | 0 | 0 |
| 2026-08-31 | — | — | — | 0 | 0 | 0 |


Additional measured population and source-lineage diagnostics at every rank (including name re-link/unlinked and selected SEC-share lineage counts):

```json
{
  "2018-02-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 493,
    "backfill_lines": 505,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 12,
    "directory_decided": 0,
    "eligible_lines": 1615,
    "eligible_not_v1": 10,
    "listed_lines": 7621,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 501,
    "v1_eligible_dropped_by_a": 1769,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 3374
  },
  "2018-05-11": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 620,
    "backfill_lines": 637,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 17,
    "directory_decided": 0,
    "eligible_lines": 2338,
    "eligible_not_v1": 24,
    "listed_lines": 7593,
    "selected_backfill_lines": 38,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 31,
    "selected_directory_etf_ignored": 3,
    "selected_lines": 2128,
    "v1_eligible_dropped_by_a": 2048,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4362
  },
  "2018-05-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 641,
    "backfill_lines": 654,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 13,
    "directory_decided": 0,
    "eligible_lines": 2334,
    "eligible_not_v1": 24,
    "listed_lines": 7581,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 505,
    "v1_eligible_dropped_by_a": 2057,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4367
  },
  "2018-08-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 676,
    "backfill_lines": 682,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 6,
    "directory_decided": 0,
    "eligible_lines": 2531,
    "eligible_not_v1": 25,
    "listed_lines": 8118,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 520,
    "v1_eligible_dropped_by_a": 2077,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4583
  },
  "2018-11-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 699,
    "backfill_lines": 706,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 7,
    "directory_decided": 0,
    "eligible_lines": 2496,
    "eligible_not_v1": 24,
    "listed_lines": 8327,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 519,
    "v1_eligible_dropped_by_a": 2090,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4562
  },
  "2019-02-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 692,
    "backfill_lines": 712,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 20,
    "directory_decided": 0,
    "eligible_lines": 2456,
    "eligible_not_v1": 23,
    "listed_lines": 8500,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 517,
    "v1_eligible_dropped_by_a": 2100,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4533
  },
  "2019-05-10": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 678,
    "backfill_lines": 706,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 28,
    "directory_decided": 0,
    "eligible_lines": 2368,
    "eligible_not_v1": 25,
    "listed_lines": 8286,
    "selected_backfill_lines": 38,
    "selected_backfill_relinked": 1,
    "selected_backfill_shrout_from_sec": 33,
    "selected_directory_etf_ignored": 3,
    "selected_lines": 2119,
    "v1_eligible_dropped_by_a": 2119,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4462
  },
  "2019-05-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 693,
    "backfill_lines": 712,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 19,
    "directory_decided": 0,
    "eligible_lines": 2341,
    "eligible_not_v1": 27,
    "listed_lines": 8343,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 522,
    "v1_eligible_dropped_by_a": 2121,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4435
  },
  "2019-08-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 1,
    "backfill_left_unlinked": 718,
    "backfill_lines": 743,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 25,
    "directory_decided": 0,
    "eligible_lines": 2297,
    "eligible_not_v1": 27,
    "listed_lines": 8349,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 516,
    "v1_eligible_dropped_by_a": 2147,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4417
  },
  "2019-11-29": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 1,
    "backfill_left_unlinked": 716,
    "backfill_lines": 730,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 14,
    "directory_decided": 0,
    "eligible_lines": 2221,
    "eligible_not_v1": 27,
    "listed_lines": 8284,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 519,
    "v1_eligible_dropped_by_a": 2154,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4348
  },
  "2020-02-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 728,
    "backfill_lines": 763,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 35,
    "directory_decided": 0,
    "eligible_lines": 2252,
    "eligible_not_v1": 23,
    "listed_lines": 8603,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 522,
    "v1_eligible_dropped_by_a": 2200,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4429
  },
  "2020-05-08": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 751,
    "backfill_lines": 760,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 9,
    "directory_decided": 0,
    "eligible_lines": 2162,
    "eligible_not_v1": 26,
    "listed_lines": 8404,
    "selected_backfill_lines": 31,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 21,
    "selected_directory_etf_ignored": 3,
    "selected_lines": 1870,
    "v1_eligible_dropped_by_a": 2176,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4312
  },
  "2020-05-29": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 744,
    "backfill_lines": 761,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 17,
    "directory_decided": 0,
    "eligible_lines": 2177,
    "eligible_not_v1": 26,
    "listed_lines": 8448,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 524,
    "v1_eligible_dropped_by_a": 2205,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4356
  },
  "2020-08-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 775,
    "backfill_lines": 786,
    "backfill_name_link_without_facts": 2,
    "backfill_relinked_by_name": 11,
    "directory_decided": 0,
    "eligible_lines": 2192,
    "eligible_not_v1": 27,
    "listed_lines": 8724,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 522,
    "v1_eligible_dropped_by_a": 2264,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4429
  },
  "2020-11-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 835,
    "backfill_lines": 845,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 10,
    "directory_decided": 0,
    "eligible_lines": 2238,
    "eligible_not_v1": 29,
    "listed_lines": 9142,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 526,
    "v1_eligible_dropped_by_a": 2336,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4545
  },
  "2021-02-26": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 852,
    "backfill_lines": 878,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 26,
    "directory_decided": 0,
    "eligible_lines": 2278,
    "eligible_not_v1": 29,
    "listed_lines": 9736,
    "selected_backfill_lines": 1,
    "selected_backfill_relinked": 1,
    "selected_backfill_shrout_from_sec": 1,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 524,
    "v1_eligible_dropped_by_a": 2413,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4662
  },
  "2021-05-07": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 882,
    "backfill_lines": 898,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 16,
    "directory_decided": 0,
    "eligible_lines": 2237,
    "eligible_not_v1": 33,
    "listed_lines": 10215,
    "selected_backfill_lines": 55,
    "selected_backfill_relinked": 4,
    "selected_backfill_shrout_from_sec": 42,
    "selected_directory_etf_ignored": 3,
    "selected_lines": 2052,
    "v1_eligible_dropped_by_a": 2441,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4645
  },
  "2021-05-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 1,
    "backfill_left_unlinked": 945,
    "backfill_lines": 959,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 14,
    "directory_decided": 0,
    "eligible_lines": 2260,
    "eligible_not_v1": 35,
    "listed_lines": 10281,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 529,
    "v1_eligible_dropped_by_a": 2490,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4715
  },
  "2021-08-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1025,
    "backfill_lines": 1043,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 18,
    "directory_decided": 0,
    "eligible_lines": 2351,
    "eligible_not_v1": 36,
    "listed_lines": 10656,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 521,
    "v1_eligible_dropped_by_a": 2584,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4899
  },
  "2021-11-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1082,
    "backfill_lines": 1098,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 16,
    "directory_decided": 0,
    "eligible_lines": 2409,
    "eligible_not_v1": 36,
    "listed_lines": 11130,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 525,
    "v1_eligible_dropped_by_a": 2675,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 5048
  },
  "2022-02-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1111,
    "backfill_lines": 1144,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 33,
    "directory_decided": 0,
    "eligible_lines": 2314,
    "eligible_not_v1": 36,
    "listed_lines": 11298,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 518,
    "v1_eligible_dropped_by_a": 2707,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4985
  },
  "2022-05-06": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1137,
    "backfill_lines": 1160,
    "backfill_name_link_without_facts": 2,
    "backfill_relinked_by_name": 23,
    "directory_decided": 0,
    "eligible_lines": 2247,
    "eligible_not_v1": 43,
    "listed_lines": 11180,
    "selected_backfill_lines": 75,
    "selected_backfill_relinked": 2,
    "selected_backfill_shrout_from_sec": 57,
    "selected_directory_etf_ignored": 3,
    "selected_lines": 1945,
    "v1_eligible_dropped_by_a": 2690,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4894
  },
  "2022-05-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1099,
    "backfill_lines": 1140,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 41,
    "directory_decided": 0,
    "eligible_lines": 2229,
    "eligible_not_v1": 40,
    "listed_lines": 11244,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 517,
    "v1_eligible_dropped_by_a": 2690,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4879
  },
  "2022-08-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1100,
    "backfill_lines": 1126,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 26,
    "directory_decided": 0,
    "eligible_lines": 2164,
    "eligible_not_v1": 41,
    "listed_lines": 10798,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 514,
    "v1_eligible_dropped_by_a": 2723,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4846
  },
  "2022-11-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1117,
    "backfill_lines": 1149,
    "backfill_name_link_without_facts": 3,
    "backfill_relinked_by_name": 32,
    "directory_decided": 0,
    "eligible_lines": 2032,
    "eligible_not_v1": 39,
    "listed_lines": 10782,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 498,
    "v1_eligible_dropped_by_a": 2732,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4725
  },
  "2023-02-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1127,
    "backfill_lines": 1151,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 24,
    "directory_decided": 0,
    "eligible_lines": 2026,
    "eligible_not_v1": 40,
    "listed_lines": 10682,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 484,
    "v1_eligible_dropped_by_a": 2777,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4763
  },
  "2023-04-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1134,
    "backfill_lines": 1159,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 25,
    "directory_decided": 0,
    "eligible_lines": 1895,
    "eligible_not_v1": 39,
    "listed_lines": 10486,
    "selected_backfill_lines": 53,
    "selected_backfill_relinked": 1,
    "selected_backfill_shrout_from_sec": 35,
    "selected_directory_etf_ignored": 4,
    "selected_lines": 1558,
    "v1_eligible_dropped_by_a": 2743,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4599
  },
  "2023-05-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1107,
    "backfill_lines": 1133,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 26,
    "directory_decided": 0,
    "eligible_lines": 1891,
    "eligible_not_v1": 40,
    "listed_lines": 10420,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 468,
    "v1_eligible_dropped_by_a": 2755,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4606
  },
  "2023-08-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1128,
    "backfill_lines": 1152,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 24,
    "directory_decided": 0,
    "eligible_lines": 1784,
    "eligible_not_v1": 40,
    "listed_lines": 10396,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 476,
    "v1_eligible_dropped_by_a": 2813,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4557
  },
  "2023-11-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1147,
    "backfill_lines": 1161,
    "backfill_name_link_without_facts": 2,
    "backfill_relinked_by_name": 14,
    "directory_decided": 0,
    "eligible_lines": 1680,
    "eligible_not_v1": 43,
    "listed_lines": 10324,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 443,
    "v1_eligible_dropped_by_a": 2808,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4445
  },
  "2024-02-29": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1165,
    "backfill_lines": 1184,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 19,
    "directory_decided": 0,
    "eligible_lines": 1634,
    "eligible_not_v1": 44,
    "listed_lines": 10314,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 446,
    "v1_eligible_dropped_by_a": 2866,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4456
  },
  "2024-04-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 2,
    "backfill_left_unlinked": 1184,
    "backfill_lines": 1198,
    "backfill_name_link_without_facts": 2,
    "backfill_relinked_by_name": 14,
    "directory_decided": 0,
    "eligible_lines": 1601,
    "eligible_not_v1": 46,
    "listed_lines": 10331,
    "selected_backfill_lines": 57,
    "selected_backfill_relinked": 1,
    "selected_backfill_shrout_from_sec": 40,
    "selected_directory_etf_ignored": 4,
    "selected_lines": 1292,
    "v1_eligible_dropped_by_a": 2871,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4426
  },
  "2024-05-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1204,
    "backfill_lines": 1214,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 10,
    "directory_decided": 0,
    "eligible_lines": 1609,
    "eligible_not_v1": 46,
    "listed_lines": 10382,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 430,
    "v1_eligible_dropped_by_a": 2883,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4446
  },
  "2024-08-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1227,
    "backfill_lines": 1237,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 10,
    "directory_decided": 0,
    "eligible_lines": 1533,
    "eligible_not_v1": 49,
    "listed_lines": 10400,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 426,
    "v1_eligible_dropped_by_a": 2920,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4404
  },
  "2024-11-29": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 1,
    "backfill_left_unlinked": 1263,
    "backfill_lines": 1272,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 9,
    "directory_decided": 0,
    "eligible_lines": 1458,
    "eligible_not_v1": 53,
    "listed_lines": 10507,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 409,
    "v1_eligible_dropped_by_a": 2969,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4374
  },
  "2025-02-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 2,
    "backfill_left_unlinked": 1290,
    "backfill_lines": 1317,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 27,
    "directory_decided": 0,
    "eligible_lines": 1384,
    "eligible_not_v1": 60,
    "listed_lines": 10801,
    "selected_backfill_lines": 1,
    "selected_backfill_relinked": 1,
    "selected_backfill_shrout_from_sec": 1,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 405,
    "v1_eligible_dropped_by_a": 2983,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4307
  },
  "2025-04-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1319,
    "backfill_lines": 1353,
    "backfill_name_link_without_facts": 3,
    "backfill_relinked_by_name": 34,
    "directory_decided": 0,
    "eligible_lines": 1301,
    "eligible_not_v1": 62,
    "listed_lines": 10887,
    "selected_backfill_lines": 70,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 47,
    "selected_directory_etf_ignored": 4,
    "selected_lines": 1062,
    "v1_eligible_dropped_by_a": 2982,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4221
  },
  "2025-05-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1345,
    "backfill_lines": 1361,
    "backfill_name_link_without_facts": 3,
    "backfill_relinked_by_name": 16,
    "directory_decided": 0,
    "eligible_lines": 1292,
    "eligible_not_v1": 65,
    "listed_lines": 10967,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 394,
    "v1_eligible_dropped_by_a": 3000,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4227
  },
  "2025-08-29": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1423,
    "backfill_lines": 1455,
    "backfill_name_link_without_facts": 2,
    "backfill_relinked_by_name": 32,
    "directory_decided": 0,
    "eligible_lines": 1235,
    "eligible_not_v1": 74,
    "listed_lines": 11163,
    "selected_backfill_lines": 1,
    "selected_backfill_relinked": 1,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 382,
    "v1_eligible_dropped_by_a": 3121,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4282
  },
  "2025-11-28": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1476,
    "backfill_lines": 1531,
    "backfill_name_link_without_facts": 3,
    "backfill_relinked_by_name": 55,
    "directory_decided": 0,
    "eligible_lines": 1137,
    "eligible_not_v1": 72,
    "listed_lines": 11584,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 373,
    "v1_eligible_dropped_by_a": 3128,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4193
  },
  "2026-02-27": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 0,
    "backfill_left_unlinked": 1521,
    "backfill_lines": 1589,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 68,
    "directory_decided": 0,
    "eligible_lines": 1026,
    "eligible_not_v1": 71,
    "listed_lines": 11771,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 347,
    "v1_eligible_dropped_by_a": 3121,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4076
  },
  "2026-04-30": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 2,
    "backfill_left_unlinked": 1588,
    "backfill_lines": 1671,
    "backfill_name_link_without_facts": 1,
    "backfill_relinked_by_name": 83,
    "directory_decided": 0,
    "eligible_lines": 947,
    "eligible_not_v1": 75,
    "listed_lines": 11869,
    "selected_backfill_lines": 71,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 42,
    "selected_directory_etf_ignored": 3,
    "selected_lines": 802,
    "v1_eligible_dropped_by_a": 3168,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4040
  },
  "2026-05-29": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 1,
    "backfill_left_unlinked": 1640,
    "backfill_lines": 1736,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 96,
    "directory_decided": 0,
    "eligible_lines": 920,
    "eligible_not_v1": 77,
    "listed_lines": 12094,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 330,
    "v1_eligible_dropped_by_a": 3197,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 4040
  },
  "2026-08-31": {
    "backfill_ambiguous_link": 0,
    "backfill_cik": {
      "excluded_only_is_operating": 0,
      "selected_company_keys": 0
    },
    "backfill_cik_would_fail_operating": 2,
    "backfill_left_unlinked": 1864,
    "backfill_lines": 1988,
    "backfill_name_link_without_facts": 0,
    "backfill_relinked_by_name": 124,
    "directory_decided": 0,
    "eligible_lines": 836,
    "eligible_not_v1": 94,
    "listed_lines": 12465,
    "selected_backfill_lines": 0,
    "selected_backfill_relinked": 0,
    "selected_backfill_shrout_from_sec": 0,
    "selected_directory_etf_ignored": 0,
    "selected_lines": 298,
    "v1_eligible_dropped_by_a": 3234,
    "v1_eligible_dropped_other": 0,
    "v1_eligible_lines": 3976
  }
}
```

## Every immutable part

| part | rows | first stamp | last stamp | wrong year | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| constituents/available_year=2018/constituents.parquet | 6301 | 2018-02-28 22:00:00 | 2018-11-30 22:00:00 | 0 | 63b6ae3d24aff3dfca8e55a4448a206b8693d192c45e234ef869e30a690a1d84 |
| constituents/available_year=2019/constituents.parquet | 6312 | 2019-02-28 22:00:00 | 2019-11-29 22:00:00 | 0 | a6c488a1ee82915ec1e2013783cd03ef3eb0bad4f80b3a71958173fb3fce46d1 |
| constituents/available_year=2020/constituents.parquet | 5834 | 2020-02-28 22:00:00 | 2020-11-30 22:00:00 | 0 | 263fae5979d81072a780040b8653227b858664dca81704f3115f97b2ac91bb65 |
| constituents/available_year=2021/constituents.parquet | 6203 | 2021-02-26 22:00:00 | 2021-11-30 22:00:00 | 0 | c04aad3e1472ad402a7d8cec478495916ac099f203d2bfcb04ddb272304bc7cb |
| constituents/available_year=2022/constituents.parquet | 5937 | 2022-02-28 22:00:00 | 2022-11-30 22:00:00 | 0 | b887307855f48641ed94c9310d15336633ad7dfba783517beb7c05de263ba9a3 |
| constituents/available_year=2023/constituents.parquet | 4987 | 2023-02-28 22:00:00 | 2023-11-30 22:00:00 | 0 | 032388310757bda2b4cf75a3a327fe1801a9e4a74e37209d24c502cac92c2467 |
| constituents/available_year=2024/constituents.parquet | 4295 | 2024-02-29 22:00:00 | 2024-11-29 22:00:00 | 0 | 3eff827da8491278ebae9a465b9587cc2f49a571ef000f243efd15289467988c |
| constituents/available_year=2025/constituents.parquet | 3678 | 2025-02-28 22:00:00 | 2025-11-28 22:00:00 | 0 | 2e4fa812e08c0c06301a8ab84bd6b4548efe25245ebdcd2e17c1a38ed86d12a3 |
| constituents/available_year=2026/constituents.parquet | 2579 | 2026-02-27 22:00:00 | 2026-08-31 22:00:00 | 0 | 841bc593bc025f551df7e0e73df54cff75266da78d677367fd9dc2a96cfbed39 |
| exits/available_year=2018/exits.parquet | 404 | 2018-03-09 22:00:00 | 2018-12-31 22:00:00 | 0 | ae7a6d0c6659c83e70459bdc155c5d29b2bb4bee5e5367b8bf17568cfb436f6c |
| exits/available_year=2019/exits.parquet | 731 | 2019-01-02 22:00:00 | 2019-12-31 22:00:00 | 0 | 23e1c00c7f1a5a268bf6dda13348e57044d65a1677ba99143b990ab4822eda54 |
| exits/available_year=2020/exits.parquet | 560 | 2020-01-02 22:00:00 | 2020-12-31 22:00:00 | 0 | 3f387769e24f2ed240f4be6129f8402cae4ff6ab587c89817937b79149654b73 |
| exits/available_year=2021/exits.parquet | 811 | 2021-01-05 22:00:00 | 2021-12-31 22:00:00 | 0 | 5a178130c825a9a6bb542e4dd7e539543e841b33847c59b0c5c344569fffe19f |
| exits/available_year=2022/exits.parquet | 738 | 2022-01-03 22:00:00 | 2022-12-30 22:00:00 | 0 | 41add4057e61c670009187599551b0cbee93a35fa4646606616937e94b348de5 |
| exits/available_year=2023/exits.parquet | 783 | 2023-01-03 22:00:00 | 2023-12-28 22:00:00 | 0 | a5f5195c67e96ba4229ca449a56082b3d22ae777d47ac9455fbecb781139a2ac |
| exits/available_year=2024/exits.parquet | 640 | 2024-01-02 22:00:00 | 2024-12-31 22:00:00 | 0 | 21aaa0aa84665008147e58853d13b7e7678790e6d24854c505a8b11805f0bf36 |
| exits/available_year=2025/exits.parquet | 763 | 2025-01-02 22:00:00 | 2025-12-31 22:00:00 | 0 | 1ce5a2d46d407547b981dfc25c0abc9c53a7edd92f43d2dfa02612e989a06cd8 |
| exits/available_year=2026/exits.parquet | 664 | 2026-01-02 22:00:00 | 2026-09-18 22:00:00 | 0 | 959be24e01a4849d02426ee4f6974ad91020afd6ffb393f41c730e75ee0fd54b |
| membership/available_year=2018/membership.parquet | 303496 | 2018-03-16 22:00:00 | 2018-12-31 22:00:00 | 0 | 6c61eafc15e612886ac4d8b7748b2056bdd9ee7f00a0ef3489088dfba4f6bc72 |
| membership/available_year=2019/membership.parquet | 495464 | 2019-01-02 22:00:00 | 2019-12-31 22:00:00 | 0 | db33797a2e156548dc240c8e07d3fd21905b02c1926bf781ec4825270a5e297c |
| membership/available_year=2020/membership.parquet | 463467 | 2020-01-02 22:00:00 | 2020-12-31 22:00:00 | 0 | 87ecae3e23fb722c5765c4e5405388a37a6b2f7e638bc4c94b204113c69a104f |
| membership/available_year=2021/membership.parquet | 456359 | 2021-01-04 22:00:00 | 2021-12-31 22:00:00 | 0 | b2276814b0e9b1c6cd6a4a641bf28af2846cc1260ff8278294f51e066fed15d7 |
| membership/available_year=2022/membership.parquet | 456040 | 2022-01-03 22:00:00 | 2022-12-30 22:00:00 | 0 | 88830aed44f92da210d9a3c9d3b5a7a986b94042b10d29bca107810735150d05 |
| membership/available_year=2023/membership.parquet | 391478 | 2023-01-03 22:00:00 | 2023-12-29 22:00:00 | 0 | 51c81ee835e7335dab200c5debc31ffd77232d5c4058945633e8e3366d5d0539 |
| membership/available_year=2024/membership.parquet | 315300 | 2024-01-02 22:00:00 | 2024-12-31 22:00:00 | 0 | f2062a0ec0458d25f59bd8a6b0e3ad122f21faad2d32bae166308b3b8d1e939c |
| membership/available_year=2025/membership.parquet | 254102 | 2025-01-02 22:00:00 | 2025-12-31 22:00:00 | 0 | b6db242a2a444c231f924b6e19a3881a4c95ea4da95f6eff7a63d884e6c0cfb7 |
| membership/available_year=2026/membership.parquet | 137789 | 2026-01-02 22:00:00 | 2026-09-18 22:00:00 | 0 | afdc42d967b3bd806cf2e93a80c96df91363e37e9bedc84ea467f943c8967017 |
| recon_summary/available_year=2018/recon_summary.parquet | 7 | 2018-02-28 22:00:00 | 2018-11-30 22:00:00 | 0 | 651d0e837e8289f4102cb41c683d1233afa80c6cacea38bc038d8b5a0e2c9998 |
| recon_summary/available_year=2019/recon_summary.parquet | 7 | 2019-02-28 22:00:00 | 2019-11-29 22:00:00 | 0 | 8ad6b4c1633f21bf0e23c40ff0f45c32a3c0123e8f2b3cb22a91aa94d02ba3bd |
| recon_summary/available_year=2020/recon_summary.parquet | 7 | 2020-02-28 22:00:00 | 2020-11-30 22:00:00 | 0 | ae89dc3f5769f27fa0e81e1931d885f39885c81e17075f5f9ac567a6c9bb547d |
| recon_summary/available_year=2021/recon_summary.parquet | 7 | 2021-02-26 22:00:00 | 2021-11-30 22:00:00 | 0 | 2645e95f41cef6466c5c504ab823e1fda67d67b38f2e1ef86d6e950e2614b1ec |
| recon_summary/available_year=2022/recon_summary.parquet | 7 | 2022-02-28 22:00:00 | 2022-11-30 22:00:00 | 0 | 4402437a1d4035987fafb721c9379221bc310933488a5c6a263d1f49ae47abc3 |
| recon_summary/available_year=2023/recon_summary.parquet | 7 | 2023-02-28 22:00:00 | 2023-11-30 22:00:00 | 0 | 9966936a655b6ac0481a25fee839bb1c8950a7a39b8d7137a7da5b94a6095245 |
| recon_summary/available_year=2024/recon_summary.parquet | 7 | 2024-02-29 22:00:00 | 2024-11-29 22:00:00 | 0 | 2fe6a194c52950a8e613bb1c0ce86000378c82b9d2091265971d5713a309ffff |
| recon_summary/available_year=2025/recon_summary.parquet | 7 | 2025-02-28 22:00:00 | 2025-11-28 22:00:00 | 0 | 371149b351379a454ed5de4e78e04644bb25f778925584ffc7684332b3b518d7 |
| recon_summary/available_year=2026/recon_summary.parquet | 6 | 2026-02-27 22:00:00 | 2026-08-31 22:00:00 | 0 | d2f5cd5d009b25084bb145c9f6eb818588a01d3ff7950988c994b4d0a5b1b7a8 |


## Real as-of replays

For one Russell and one S&P reconstitution, cuts lie strictly between rank and effective start. Re-ran rank selection with the same pinned PIT input; kept only ranks public before the cut, capped line last sessions at the latest known session and hid future delistings. Re-derived all products. Complete rows stamped before each cut compare using count plus sum/XOR of SHA-256 row digests, including every daily membership row, with bounded memory.

```json
[
  {
    "cut": "2021-06-10 00:00:00+00:00",
    "data_end": "2021-06-09",
    "index": "R2000P",
    "products": {
      "constituents": {
        "pass": true,
        "published": {
          "rows": 23604,
          "sum_sha256": "a46120c4dbd3436964dbfd60e241ba9f7fdcf2f3885effa2423fbfc80185601d",
          "xor_sha256": "666e59e123fbb2461454b1ec96998a6bb66bd6bca2453d34f5c5d6688fa79b53"
        },
        "replay": {
          "rows": 23604,
          "sum_sha256": "a46120c4dbd3436964dbfd60e241ba9f7fdcf2f3885effa2423fbfc80185601d",
          "xor_sha256": "666e59e123fbb2461454b1ec96998a6bb66bd6bca2453d34f5c5d6688fa79b53"
        }
      },
      "exits": {
        "pass": true,
        "published": {
          "rows": 2050,
          "sum_sha256": "b62525b6e781ef13df772f169bc0a55b9111df8436bb9744b3c5f3c13cc3d908",
          "xor_sha256": "caf42a7b3f0e0a43217147407f997b8425fb85f78e57890e225dcaaedf1d11c8"
        },
        "replay": {
          "rows": 2050,
          "sum_sha256": "b62525b6e781ef13df772f169bc0a55b9111df8436bb9744b3c5f3c13cc3d908",
          "xor_sha256": "caf42a7b3f0e0a43217147407f997b8425fb85f78e57890e225dcaaedf1d11c8"
        }
      },
      "membership": {
        "pass": true,
        "published": {
          "rows": 1446489,
          "sum_sha256": "57b0a03c41e542e3a8c1b8f18c58c691262e2860d347a3ade0170a525e322529",
          "xor_sha256": "2497d017fb43d5539e681eb89f788db3683856d7dbc3489a92d208d77bb5f1cf"
        },
        "replay": {
          "rows": 1446489,
          "sum_sha256": "57b0a03c41e542e3a8c1b8f18c58c691262e2860d347a3ade0170a525e322529",
          "xor_sha256": "2497d017fb43d5539e681eb89f788db3683856d7dbc3489a92d208d77bb5f1cf"
        }
      },
      "recon_summary": {
        "pass": true,
        "published": {
          "rows": 26,
          "sum_sha256": "c0c10e2d4adf84a400ace335be75f9c121166cd873880cbf9b7c8700c86b63bd",
          "xor_sha256": "754ef3b4188cbc2946ced0a2cbfe33979c6a5125184e8bc3c3345832d0621bbd"
        },
        "replay": {
          "rows": 26,
          "sum_sha256": "c0c10e2d4adf84a400ace335be75f9c121166cd873880cbf9b7c8700c86b63bd",
          "xor_sha256": "754ef3b4188cbc2946ced0a2cbfe33979c6a5125184e8bc3c3345832d0621bbd"
        }
      }
    },
    "rank_date": "2021-05-07",
    "recon_date": "2021-06-25"
  },
  {
    "cut": "2021-03-10 00:00:00+00:00",
    "data_end": "2021-03-09",
    "index": "SP500P",
    "products": {
      "constituents": {
        "pass": true,
        "published": {
          "rows": 18971,
          "sum_sha256": "948f60e00384876718d25f7e5fc33c03e8a7e4b47cb380870aa27ab9ed1f8da8",
          "xor_sha256": "3a4648996af28a000d25a020f82557947498ff0cbde43e5c4b1e66922da73956"
        },
        "replay": {
          "rows": 18971,
          "sum_sha256": "948f60e00384876718d25f7e5fc33c03e8a7e4b47cb380870aa27ab9ed1f8da8",
          "xor_sha256": "3a4648996af28a000d25a020f82557947498ff0cbde43e5c4b1e66922da73956"
        }
      },
      "exits": {
        "pass": true,
        "published": {
          "rows": 1812,
          "sum_sha256": "e03cca555b16a6421f67668e6f61ae80cf6f2930eb2946ae030f5ad0e6e6d571",
          "xor_sha256": "ac4f39476492642494b76b2297416c18d24a7cc8f6c22bf91d8783f3b460fe11"
        },
        "replay": {
          "rows": 1812,
          "sum_sha256": "e03cca555b16a6421f67668e6f61ae80cf6f2930eb2946ae030f5ad0e6e6d571",
          "xor_sha256": "ac4f39476492642494b76b2297416c18d24a7cc8f6c22bf91d8783f3b460fe11"
        }
      },
      "membership": {
        "pass": true,
        "published": {
          "rows": 1340084,
          "sum_sha256": "692371192f808cf82ac3193840d0f7acd0b71b0d16168936a1457fbbeb805248",
          "xor_sha256": "40d481d1266ee5aef664135edc7ae63a0b4b54b465abff7134e0e81addb9c480"
        },
        "replay": {
          "rows": 1340084,
          "sum_sha256": "692371192f808cf82ac3193840d0f7acd0b71b0d16168936a1457fbbeb805248",
          "xor_sha256": "40d481d1266ee5aef664135edc7ae63a0b4b54b465abff7134e0e81addb9c480"
        }
      },
      "recon_summary": {
        "pass": true,
        "published": {
          "rows": 22,
          "sum_sha256": "f486c87c7585c2af4aedc1ef7a2223ee78b03e79f49d0e46dd9b4495448da811",
          "xor_sha256": "162679674d3b1da40c0030608172d7a0648270874865d2a164b11aa6574b9165"
        },
        "replay": {
          "rows": 22,
          "sum_sha256": "f486c87c7585c2af4aedc1ef7a2223ee78b03e79f49d0e46dd9b4495448da811",
          "xor_sha256": "162679674d3b1da40c0030608172d7a0648270874865d2a164b11aa6574b9165"
        }
      }
    },
    "rank_date": "2021-02-26",
    "recon_date": "2021-03-19"
  }
]
```

## Resources and commands

| job | status | wall seconds | peak GiB | FIFO wait seconds | cap hit |
| --- | --- | --- | --- | --- | --- |
| preflight | completed | 1.0 | 0.0416 | 13.1 | False |
| seed | completed | 8.0 | 0.0423 | 79.2 | False |
| identity | completed | 2.0 | 0.1099 | 121.2 | False |
| backfill | completed | 136.3 | 0.6658 | 67.1 | False |
| names | completed | 91.6 | 0.7136 | 220.1 | False |
| combine | stopped_low_commit | 11.2 | 0.0604 | 909.5 | False |
| combine-r2 | stopped_low_commit | 104.9 | 0.633 | 0.1 | False |
| combine-r3 | failed | 92.6 | 0.6526 | 0.1 | False |
| combine-r4 | completed | 220.6 | 0.6849 | 1037.7 | False |
| measure | stopped_low_commit | 63.8 | 0.3547 | 1061.4 | False |
| measure-r2 | completed | 124.0 | 0.3983 | 145.4 | False |
| build | completed | 164.1 | 0.4845 | 0.1 | False |
| acceptance | failed | 119.3 | 0.4747 | 0.0 | False |
| acceptance-r2 | stopped_low_commit | 58.0 | 0.3998 | 0.0 | False |
| acceptance-r3 | completed | 276.2 | 0.4767 | 207.4 | False |


All real lake reads ran with the existing shared `run_memory_guarded.py --heavy --job-gb 1.0 --wait-minutes 90 --disk-path C:/ --min-free-disk-gb 25`, normal FIFO, no nesting or threshold changes. DuckDB used at most 600 MB and two threads. Each exact child command, timing and memory receipt is preserved under `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/task7-<job>-guard.json`; stage commands are in `task7-stage-controller.py`; measure/build and acceptance probes in `task7-build.py` and `task7-acceptance.py`.

## Limits

Held r4 is the unchanged identity source, with its original seal and evidence limitations; the Task4 reconstruction fix is not substituted. Frozen panel/shares still carry upstream SEC share lineage, including possible backfill-derived counts; removing rank company/operating influence does not independently re-admit those inputs. FINRA common_unverified classification and shrinking eligible population explain the reported count failures. No licensed official overlap, float, committee/sector discretion or full IPO/spin-off additions. Future sessions after the measured vendor end have scheduled constituent bounds, not newly observed market facts. The immutable manifest status complete denotes writer completion, not a passing R2000 band or production activation.

## Own scratch cleanup outcome

A separate guarded read-only audit completed in 9.1 seconds (peak 0.0414 GiB, FIFO wait 184.5 seconds, cap_hit=false). It rechecked the seeded-file hashes, resolved every target inside the newly created Task7 root or its SDD directory, and rejected symlinks/junctions. Audit and exact resolved paths: `task7-cleanup-validation.json`, receipt `task7-cleanup-validation-guard.json`.

The single native PowerShell `Remove-Item -LiteralPath` attempt for these exact targets was rejected before process creation with **blocked by policy**. No cleanup occurred. Per the binding refusal rule, all eight targets are retained, with no retry or alternate deletion method. No prior D3-refused Task3/Task6 target was attempted.

| Retained own target | Validated files | Validated bytes |
| --- | --- | --- |
| `data/research/identity_rehearsal/ix-pit-links-20261004/prices` | 9 | 1191272848 |
| `data/research/identity_rehearsal/ix-pit-links-20261004/short_interest` | 1 | 30481536 |
| `data/research/identity_rehearsal/ix-pit-links-20261004/calendar.parquet` | 1 | 19323 |
| `data/research/identity_rehearsal/ix-pit-links-20261004/_tmp` | 27 | 1332354831 |
| SDD `task7-measure-spill` | 0 | 0 |
| SDD `task7-build-spill` | 1 | 26226688 |
| SDD `task7-acceptance-spill` | 0 | 0 |
| SDD `task7-ix8-synthetic` | 0 | 0 |

The new root's `identity/` PIT file, original link/combined outputs and `_a0/` manifests remain intentionally retained, as do all SDD scripts, failed/successful receipts, immutable-input hash pins and final evidence. Published indexes_v2 and original v1/r4 bytes remain unchanged as proven by the successful acceptance run. This is a scratch-cleanup policy limitation, with no data acceptance gate waiver or production activation.
