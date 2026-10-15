# Quote-only American fitting and benchmark

User objective: improve the reusable American equity options library and its
speed/accuracy against SpiderRock fair price and volatility using only contract,
quote, carry, dividend and clock inputs. Reference values are evaluation labels.
The user explicitly requested sub-agent development, implementation before tests,
and sustained autonomous iteration. Existing code is preserved and extended.

## Working state

Base: `e696e208ef622aeb8a9ffbcc4bd3e4ea16a74548`.
The graph MCP and repository's older broker workflow are not exposed in this
session. Native Codex agents use isolated leased worktrees and separate review.
No legacy oracle holdout is accessed. The October file gets its own deterministic
underlier split, with aggregate validation scored only after configuration freezes.

* Integration/build and native runner: pool-33, `feat/vol-benchmark-20261015`,
  lease `vol-20261015-root`.
* Dense inversion optimization: pool-34, `feat/vol-dense-iv-20261015`,
  lease `vol-20261015-dense`.
* Streaming adapter/scorer: pool-35, `feat/vol-quote-data-20261015`,
  lease `vol-20261015-data`.
* Reusable quote slice fitter: pool-36, `feat/vol-quote-slice-20261015`,
  lease `vol-20261015-slice`.

Source (external licensed data, never committed):
`Downloads/OptionIntradayHist_30min_2026-10-14.parquet`, 34,011,291 rows,
280 row groups, 1,916,931,890 bytes. Exercise census reported by adapter lane:
31,889,698 American, 2,121,593 European. One trading day cannot establish
multi-day robustness or live trading readiness.

## Design

1. Project an explicit input allowlist; write normalized quotes and evaluation
   labels separately. Exclude `srPrc`, `srVol`, vendor bid/ask IVs, Greeks,
   `atmVol`, and `synSpot` from the model input and selection logic.
2. Fit each expiry with supplied variance/funding clocks and carry, via American
   IV extraction, a cross-strike curve, and American repricing of that curve.
   The library entry point has no dependency on Parquet or SpiderRock schemas.
3. Distinguish continuous carry, explicit dated cash dividends, and aggregate
   cash-PV approximation. Aggregate PV does not identify exercise dates.
4. Score a left join from the input cohort; report failures and coverage beside
   price/volatility errors, spread-normalized errors, and containment. Record
   source/input/label/binary hashes and configuration.
5. Measure speed only after numerical validation, on a quiet Release AVX2 host.
   Cold calibration, cached queries, and IO must have separate timing scopes.

## Initial evidence

* `vol-dev` baseline `atx-vol` builds successfully through `atx-build.ps1`.
* Lease bootstrap's default `dev` configure fails because optional deleted
  modules are absent. Lease survives; configure `vol-dev` in the leased tree.
* Old oracle Mode A consumes `srVol`; it is excluded from this task's benchmark.
  Old Mode B reprices an inverted midpoint and does not measure smile fitting.
* New `vol-rel-avx2` and `vol-hygiene` presets extend the restored module graph
  with isolated dependency directories.

Validation results and benchmark measurements will be recorded after execution.
No world-leading performance or production-readiness claim has been established.
