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

## Iteration 1 (development only)

Integrated quote-slice commits `06ae923f` / `3ce0eddd`, adapter `4c897b3f`,
adapter review fixes `c046c6b1`. Eight focused QuoteSlice checks pass; 34 Python
adapter/native boundary checks pass. Dense solver candidate `f5f259e4` passed
66 scoped checks including the unchanged portfolio fingerprint, and PCH-off
compilation in its lane. Independent source review approved its guarded bracket;
Release speed comparison is pending. Its synthetic grid has 630 cases, of which
623 have finite IVs (all bit-identical to original production bisection).

Data: `C:/atx-cache/quote-benchmark/20261014-dev`, prepared with
`--split development --max-underliers 20 --buckets 1030 --csv`.
10,948 selected rows, 7,308 eligible, 3,508 European, 126 invalid option quotes,
six invalid carry rows. Source projection/normalization takes 33.16 s on this host,
including source hashing; this is diagnostic IO timing.

First eSSVI baseline (`essvi-debug-v1.csv.run.json` and matching score JSON):
7,172 predictions / 7,308 eligible; 7,075 price comparisons; price MAE $0.67723,
median $0.11103; vol MAE 1,582.07 bp, median 247.99 bp. Same-population midpoint
price MAE $0.33868. These results are not acceptable as a production parity claim.
SVI covers only 3,848 rows; spline only 552. Their fit refusal reasons are under
investigation, and smaller-population MAEs are not compared as wins.

Findings, using labels for evaluation only:

* Four adjusted roots account for 59.92% of absolute price error on just 3.90%
  of scored rows. The schema lacks adjusted deliverables. Native import now
  refuses OCC numeric-suffix 1-9 roots explicitly and counts their coverage loss.
  This is corrected contract scope, not a numerical pricing improvement.
  Standard-root baseline alone has $0.28243 MAE vs $0.33309 midpoint over 6,799
  price rows. [OCC adjusted symbols memo](https://infomemo.theocc.com/infomemos?number=26853).
* 41/125 standard eligible chains have strike-varying `sdiv`, maximum range
  0.005, median nine distinct values. All 3,508 matched call-put pairs agree
  exactly at each strike. Native median aggregation loses a carry curve.
  A reusable optional per-contract yield and owned carry interpolation are in
  development, using supplied raw fields only.
  [SpiderRock surface conventions](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/).
* Two longest IEF expiries have vendor prices inside raw quotes only 16.38% and
  10.64% of the time. This is reference disagreement evidence, never an admission
  filter. Another IEF expiry has good reference containment but poor model
  containment and deserves carry/dividend investigation.
* Independent review reproduced native input destruction through a hardlink
  output alias. Root fixed file identity checks and added preservation validation.
  Duplicate contracts/mixed identity or expiry clocks within a declared chain
  now return explicit failure rows, rather than being silently averaged.

Next: finish per-strike carry; compare on matched standard-contract rows; expand
to 200 development underliers; add paired comparison and raw-input strata;
compare dense solver speed against the original production solver on a quiet
Release host; evaluate frozen settings on unseen underliers. No validation
cohort has been read yet.
