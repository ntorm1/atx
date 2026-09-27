# W1 foundations and IC V3: bounded correctness qualification

2026-09-26. Production source is `7e9f6f4a`, built at clean checkpoint
`6a401f56`. The final test-only fixture correction is `e2aaea7e`.
The original DAG's performance, scale and causality-harness gates remain open.
No real market data or long benchmark was run. No alpha is qualified as tradeable.

## Results

| Focused executable | Result | Native test time |
|---|---|---:|
| W1 foundation, ten owning translation units | 83/83 passed, 35 suites | 19.904 s |
| IC engine | 63/64 initially passed; fixture correction then passed 1/1 | 17.362 s + 0.011 s |
| IC pipeline/config/mine/discovery | 42/42 passed, nine suites | 35.752 s |

All **189 distinct checks** now pass, with no skips or unresolved failures.
The initially failing assertion counted two missing paired instruments; the
screen correctly also excludes a missing decision-date price. The corrected
fixture uses three distinct missing names at every horizon and compares each
rank correlation against an independent all-pairs oracle. Two different signal
panels reuse the same worker scratch. No production behavior changed in this fix;
the other 63 passing IC checks were reused, rather than rerun without cause.
Historical failed-run XML is retained alongside the successful focused rerun.
These are focused checks, not the complete owning subsystem suites.

A1 checks cover independent delay/delta arithmetic, overlap safety, tiled SIMD
sum/mean, pair overflow recovery, exponential decay, future perturbation, and
explicit legacy arithmetic. Existing oracle/conformance checks passed. The
streaming corpus exercised all 101 canonical formulas at 24 and 128 names in
both AuditExact and ResearchFast, with exact batch/streaming agreement and no
unsupported formulas in those fixtures. Release per-cell and scaling bars are
unmeasured; remaining scalar and exceptional O(window) paths are disclosed in
the A1 implementation report.

E1 checks cover immutable chunk integrity, quantization, original-value exact
statistics, explicit f32 mode, bounded geometry, cached GK/EWMA fit parity and
walk-forward label maturity/embargo behavior. All six cube and six consumer
checks passed, as did the included existing combiner and walk-forward checks.
The large cube RSS experiment remains unrun.

## IC implementation and statistical limits

V3 imports are kernel `b9e55206`, mine `d7b11bc5`, and discovery/defaults
`8ccf0982`. The application recipe is `equivalence-v3`, absolute IC floor .002,
confidence multiplier 3.5, and horizons 5/21/63/126. Every Pearson and tied-rank
bound must exclude the configured effect in both signs before rejection.
Undefined, sparse and underpowered observations pass. The quarter-mean check is
an additional heuristic, not a calibrated guarantee for intermittent effects.
Explicit V2 and disabled recipes remain available and have separate identities.

V3 estimate-level boundary/power cases and the small production-screen algebraic
fixture passed. The latter rejects a deliberately precise null and retains
planted signed .002/.005 effects; it is not noisy-null power or population recall
evidence. The two historical V2 noisy cohorts again rejected 0/18 nulls.
**Practical V3 pruning, general recall and production throughput remain
unqualified.** The implementation can skip full fitness/backtests and retain
durable trial accounting, but no real-market efficiency claim follows yet.

`7e9f6f4a` makes the screen date-major and reuses exact signal ranks across
horizons when their ordered paired instrument IDs match. Missing-data subsets
still re-rank exactly. This removes repeated sorting on the common path without
changing the decision rule, adding only one bounded O(names) ID vector per
worker. Independent source review and the existing/added scalar-oracle checks
passed. No controlled speedup is claimed from Debug test timings.

## Build and artifacts

The first combined build at `8ccf0982` compiled 32 production translation units
then stopped on the cube's missing JSON include (324.500 s). Source-local JSON
include properties fixed the cube and its test without changing the engine's
global include environment or rebuilding its PCH. The resumed build at clean
`6a401f56` passed in 375.350 s: 42 C++ objects and five links, including the rank
reuse change. The test-only correction rebuilt one object and one link in
8.531 s. Every build used the existing equity-dev compiler cache/PCH, isolated
dependencies and Jobs1. No worker or dependency rebuild occurred.

During the measured latter part of the build, ccache's preprocessing-failure
and compilation-failure counters did not increase. The much larger historical
global uncacheable count is not attributed to this build. This is incremental
build evidence, not a controlled compiler speed comparison.

Receipts, binary SHA256 hashes, logs and XML are in `pool-2/build-equity`:

- `w1-v3-build*`, `w1-v3-resume-build*`, `ic-screen-rank-fixture-build*`
- `w1-foundation-tests*`
- `ic-screen-v3-engine-tests*`, `ic-screen-rank-fixture-tests*`
- `ic-screen-v3-impl-tests*`

Impl's baked source identity remains clean `6a401f56`; the later change affected
only the IC test fixture. No production-code rebuild was needed for that fix.
The original DAG, not this bounded packet, controls full wave completion.
