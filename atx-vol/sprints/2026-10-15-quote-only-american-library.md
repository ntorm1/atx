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

## Follow-up: breaking changes and mandatory arbitrage admission

The user explicitly permits breaking changes and removal of outdated code.
Numerical checks now establish independent accuracy and worker determinism;
historical output hashes are not a reason to retain unnecessary arithmetic.
The user also requires every valid fit to enforce no-arbitrage requirements.
Earlier benchmark runs are development baselines, not accepted production fits.

Integrated after the initial checkpoint:

* Per-strike carry (`87029a0e`, native `9a10c8c2`) and paired/raw-stratum
  evaluation (`e4c90a0e`). Carry preservation changes dev20 price MAE from
  $0.282430 to $0.282881 on identical rows; this is input correctness, not a
  reference-error win. All 12 focused slice and 40 Python checks passed then.
* Quote-quality configuration (`913d741e`) excludes bidless/locked point
  targets, records rejection reasons, and caps inverse-uncertainty weights.
  Contracts remain in the evaluation cohort. Wide-spread gates are tunable;
  final-price arbitrage gates cannot be disabled.
* Dense Newton IV acceleration (`ac2eecde`, `10a830da`): 807 finite cases out
  of 840, with 33 expected numerical-domain refusals. Against independent
  80-step bisection, max IV difference 4.55e-12, max Black price difference
  4.47e-9, max scaled analytic Greek difference 2.13e-9. Twenty-four American
  comparisons give max price difference 2.85e-14 and scaled Greek difference
  4.56e-8. All 66 targeted tests and PCH-off changed-TU compilation passed in
  the implementing lane. Release binaries for original production bisection
  and direct Newton are preserved outside the repository; speed measurement
  still requires a coordinated quiet window.
* `AmericanPriceSlice` (`370e74fa`) owns final call/put price knots, rejects
  intrinsic/upper, vertical-spread, convexity and conservative cross-side
  violations, and offers bounded, allocation-free piecewise-linear serving.
  Fixed roundoff tolerance only. Ten independent focused tests passed.
* Generic curve gate repairs (`524bf6fd`) close LinearVariance jump, spline
  clamp, residual hinge, and warm-refit bypasses. The unsafe ConvexDense
  variance-wing fallback and cache were removed. Ninety targeted tests passed.

Development-200 cohort: `C:/atx-cache/quote-benchmark/20261014-dev200`, prepared
using `--max-underliers 200 --buckets 1030 --split development --csv`.
72,348 selected / 63,942 raw-eligible rows. The carry-preserving baseline serves
62,412 rows (97.61%), price MAE $0.277666 vs same-population midpoint $0.350695.
This baseline predates mandatory final-American/calendar admission.

The strict quality defaults alone reduce coverage to 39.53%. On the exact
25,165-row price intersection, MAE improves $0.283341 -> $0.266394, while the
25,276-row volatility intersection worsens 432.93 -> 524.98 bp. This is not an
overall improvement; hard filters need replacement with useful uncertainty or
bid/ask-interval information while retaining mandatory arbitrage admission.

The first final-price integration run (`essvi-release-arb-v1`) additionally
refuses four strike-shape failures and 116 calendar-inconsistent chains. It
serves 10,300 rows from 83 chains; 935 chains lacked enough quality point
observations. All 1,244 available expiry pairs were checked on their full
overlapping piecewise-linear domains. Reduced coverage is an explicit failure
cost, never credited as an accuracy improvement. Wider quote policies are
being measured with the same arbitrage gates.

Independent math review reproduced a flat-volatility carry-spike arbitrage:
S=100, r=0, T=1, sigma=.2, q(100)=0 and q(100.1)=-.005 gives accepted old-model
calls 7.965567 and 8.191733. The higher strike costs more. A regression now
requires final-price refusal. Generic European-curve certificates do not
survive arbitrary strike-dependent carry or American exercise.

Static price constraints are necessary; they do not establish a general joint
American optimal-stopping model with arbitrary cash/carry. The next model
layer is a common positive finite-state transition system across every strike
and expiry, with separate funding/variance clocks, exact dividend event times,
and shared exercise opportunities. A sub-agent is implementing scalar rollback
first; exact convex piecewise-linear strike recursion follows. This constructs
a consistent finite Bermudan model and requires separate convergence evidence
for continuous-American accuracy. No heldout validation labels have been read.
