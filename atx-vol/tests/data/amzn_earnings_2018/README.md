# AMZN around-earnings OPRA fixture (2018-04-26 15:45 ET)

Real OPRA cbbo-1m (NBBO) snapshot of the full AMZN option chain **15 minutes before
AMZN's after-close Q1-2018 earnings print** — the exact snapshot VolaDynamics uses in
its "AMZN around earnings" example. Used by `tests/amzn_earnings_test.cpp`
(suite `AmznEarnings`) and `examples/amzn_earnings_report.cpp`.

- **Snapshot:** 2018-04-26 19:45:00Z (15:45 ET / EDT). Spot ≈ $1519 (pre-2022 split).
- **Content:** 5610 rows, 17 expiries (2018-04-27 @1 DTE → 2020-01-17 LEAP), strikes 365–2355.
- **File (NOT committed):** `amzn_opra_cbbo1m_2018-04-26T1945Z.parquet` (114 KB).

## Why the parquet is not in git
Databento OPRA data is licensed and not redistributable; the repo commits no vendor
option-chain data (README policy). The test **skips cleanly when the parquet is absent**
(same pattern as the SPY/XOM real-data tests). Regenerate it locally to run the test /
report.

## Regenerate (Databento — small paid pull, preflight ~ $0.01)
Requires `DATABENTO_API_KEY` in `C:/atx/.env`. From the repo root:

```bash
printf 'AMZN\n' > /tmp/amzn.txt
# Free preflight first:
python atx-vol/tools/pull_opra_universe_snapshot.py \
    --symbols-file /tmp/amzn.txt --date 2018-04-26 --snap-utc 19:45 \
    --out C:/atx-data/amzn-earn-2018 --cap 10 --dry-run
# Drop --dry-run to spend (est ~$0.0084), then copy into place:
cp C:/atx-data/amzn-earn-2018/AMZN/2018-04-26.parquet \
   atx-vol/tests/data/amzn_earnings_2018/amzn_opra_cbbo1m_2018-04-26T1945Z.parquet
```

`OPRA.PILLAR` / `cbbo-1m` covers 2013→present (Databento extended history May 2025);
`--snap-utc 19:45` = 15:45 EDT, the 1-minute window `[19:45:00Z, 19:46:00Z)`.

## Render the report
```bash
cmake --preset dev -DATX_BUILD_EXAMPLES=ON
cmake --build build --target amzn_earnings_report
build/bin/amzn_earnings_report.exe \
    --opra atx-vol/tests/data/amzn_earnings_2018/amzn_opra_cbbo1m_2018-04-26T1945Z.parquet \
    --snapshot 2018-04-26T19:45:00Z --r 0.019 --out atx-vol/python/report_out_real
python atx-vol/python/amzn_earnings_report.py \
    --in atx-vol/python/report_out_real --out atx-vol/python/report_out_real/figures
```
