# SPX Wilmott Figure 1 reproduction

This directory contains an independent atx-vol reproduction of Figure 1 from
`VolaDynamics_WilmottProfile_Jan2020.pdf`. It uses the exact Databento OPRA
CBBO snapshot at 2019-08-26 15:30 ET and the first standard monthly SPX expiry,
2019-09-20 (European exercise, AM settlement).

## Artifacts

- `figure-01-atx-vol-convex-dense.png`: deterministic 1318 x 1139 recreation.
- `figure-01-atx-vol.csv`: fitted curves, market error bars, and diagnostics for
  every evaluated atx-vol family.
- `figure-01-atx-vol-convex-dense.metrics.json`: quote and digitized-vendor
  comparisons, input hashes, and fitted metadata.
- `databento-provenance.json`: source URLs, schemas, row counts, costs, and
  content hashes. It contains no API credential.

The paid raw snapshot remains outside the repository at
`C:\atx-data\spx-wilmott-2019-08-26\fit_slice\SPX_2019-08-26T1930Z_2019-09-20.parquet`.

## Reproduce

From `C:\atx-wt\spx-wilmott` after building the example target:

```powershell
.\build\bin\spx_wilmott_repro.exe `
  C:\atx-data\spx-wilmott-2019-08-26\fit_slice\SPX_2019-08-26T1930Z_2019-09-20.parquet `
  C:\atx\archive\research\vol\reproduction\figure-01-atx-vol.csv

python atx-vol\python\render_spx_wilmott_repro.py `
  C:\atx\archive\research\vol\reproduction\figure-01-atx-vol.csv `
  C:\atx\archive\research\vol\reproduction\figure-01-atx-vol-convex-dense.png `
  --metrics C:\atx\archive\research\vol\reproduction\figure-01-atx-vol-convex-dense.metrics.json `
  --vendor atx-vol\tests\data\spx_wilmott_2019\figure1_fit.tsv
```

ConvexDense is recommended because it is price-convex and fits 233 of 237
visual quote bands (98.31%). Its visual IV RMSE is 0.003082; the strict
two-sided-quote IV RMSE is 0.001409. Both the observed and full Figure domains
have zero butterfly violations, and the native call-price cone has zero price
bound, monotonicity, or convexity violations.

The proprietary Vola Dynamics `C13pm` family was not copied or relabeled. The
comparison is against a digitized raster oracle and therefore includes raster
calibration error, especially in the far wings.
