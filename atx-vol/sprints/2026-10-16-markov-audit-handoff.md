# Markov pricing audit handoff, 2026-10-16

The user requested a stop during this lane. Development stopped. This note and
the diagnostic source preserve completed work; no PWL fix was implemented.

## Lane and integration

- Branch: `feat/vol-pricing-audit-20261016`.
- Frozen base: `1bde9704a0b46efac7af6ca9fdc2b9b2c8635106`.
- Worktree: `C:/atx-wt/pool-36`.
- RunId and heartbeat: `vol-20261016-audit`.
- Only new diagnostic source and this note are committed in the lane.
- Temporary CMake registration was restored before commit. The root must add
  the target after integration; the source is not automatically built.
- Release resets the physical worktree. Read the durable branch rather than
  assuming pool-36 retains its feature source.

Suggested target, under the existing `if(ATX_BUILD_TOOLS)` block:

```cmake
add_executable(atx-vol-american-markov-audit tools/american_markov_audit_main.cpp)
target_link_libraries(atx-vol-american-markov-audit PRIVATE
    atx::vol atx::core atx_warnings nlohmann_json::nlohmann_json)
```

## Read-only review conclusions

Reviewed batch pricer `ae50b8f0`, PWL primitive `21ca9177`, shared PWL surface
`9e0e7451`, and baseline Markov compiler `d7c7e109` via `git show` before leasing.
No graph tools were available; fallback discovery used `rg` and Git.

- No concrete batch algebra, aliasing, workspace-capacity or bounds bug found.
  The result buffer is written only after successful rollback. Input/output
  overlap is supported because every strike read precedes the final copy.
- PWL weighted merge cursors remain valid because all term domains must have
  identical endpoints: all cursors finish together at the common upper bound.
- PWL surface live/generated/input counters have bounded subtraction invariants;
  no concrete underflow or released-node accounting defect found.
- The shared positive operator and Snell recursion justify a common finite
  Bermudan model. They do not establish convergence to continuous American
  prices, nor accurate unbounded diffusion tails. The actual bounded process,
  exercise dates, limited-liability cash payments, and carry conventions matter.
- Copies/construction can allocate. The PWL surface catches construction-time
  `bad_alloc` inside its body, but copying its model-by-value argument occurs
  before entry. The primitive itself does not promise allocation-free creation.
- Existing PWL tests primarily use short five-state examples. The practical
  default grid required a separate diagnostic panel; that panel exposed the
  reproducible numerical refusal below.

## Concrete numerical-usability defect

Default Markov configuration, spot 100, one interval
`{calendar_end=.25, variance_end=.25, rate=0, continuous_yield=0,
volatility=.2, cash_dividend_at_end=0}`, and PWL strike domain `[40,180]`:

```text
OutOfRange: American Markov PWL: call expiry 0:
PWL strike: exercise crossing below strike resolution
```

The scalar and batch pricers both succeed. The same refusal occurred on all
eight tested uniform/logarithmic/domain variants of this martingale case.
This is a fail-closed numerical-usability defect, not evidence that the output
violates arbitrage constraints: no PWL surface is returned.

Likely mechanism, not yet instrumented: roundoff produces opposite-signed tiny
continuation-minus-intrinsic gaps in an economically dominated exercise region;
the computed crossing rounds onto an endpoint. The exact cause should be
confirmed with node/gap diagnostics before choosing a fix.

Do not silently skip every unrepresentable crossing or loosen the convexity
gate. A possible future approach is a rigorous fixed roundoff error bound for
the endpoint chord when the crossing has no distinct representable strike.
That needs an explicit contract and proof of the error allowance; it was not
implemented or authorized beyond the stop.

## Diagnostic source and runs

New file: `tools/american_markov_audit_main.cpp`.

CLI:

```text
atx-vol-american-markov-audit [--full] [--case NAME] [--grid NAME]
```

Default runs 11 synthetic fixtures at default65. `--full` runs 88 combinations:
11 fixtures x 8 grids. Optional filters select named fixtures/grids; a nonexistent
selection rejects. No data file, vendor label, or market-dependent tuning is used.

Fixtures: martingale, funded, separate_clocks, short_low_vol, high_vol,
negative_rate, negative_rate_and_yield, dated_cash, near_cash, clock_pause,
deterministic_cash. Grids: default65, uniform129, uniform257, domain2, domain8,
log65, log129, log257. Uniform domain variants preserve the coarse spacing;
their CFL time subdivisions can still differ. All model and PWL resource caps
remain the public defaults and refusals are reported rather than hidden.

The JSON includes model inputs/caps, transition/row counts, boundary exposure,
scalar/batch disagreement, independent reference discrepancies, PWL status and
node counters when successful, and all sampled prices. There are 33 strikes
uniformly spanning `[.4*S,1.8*S]`, both sides and both exercise styles at every
expiry. **This grid does not include exactly ATM**; add ATM and immediate ATM
neighbors when continuing. An attempted unrun ATM addition was reverted at the
stop so the preserved source matches the executed numerical behavior.

Independent references:

- Direct lognormal European formula for cash-free prefixes, integrating the
  separate calendar-rate/yield clocks and variance clock. Does not call the
  library's Black pricer.
- Continuous deterministic optimal stopping when all prefix volatilities are
  zero: examine interval endpoints, the possible interior stationary point of
  discounted intrinsic, and both sides of every limited-liability cash event.
- Stochastic cash-dividend American values have no independent continuous-time
  oracle in this tool. Scalar/batch/PWL agreement tests the common discrete
  recursion, not stochastic American convergence.

Elapsed times are diagnostic only. Runs were not pinned or quiet, scalar/batch
order is fixed, query timing includes JSON access, and no speed claim is valid.

Commands actually executed with temporary target wiring:

```powershell
powershell -NoProfile -File C:/atx-wt/pool-36/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 configure
powershell -NoProfile -File C:/atx-wt/pool-36/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 build atx-vol-american-markov-audit

$env:PATH = 'C:/atx-cache/vcpkg_installed/x64-windows/bin;' + $env:PATH
& C:/atx-wt/pool-36/build-vol-rel-avx2/bin/atx-vol-american-markov-audit.exe |
  Set-Content -Encoding UTF8 C:/atx-cache/quote-benchmark/markov-audit-20261016-default.json
& C:/atx-wt/pool-36/build-vol-rel-avx2/bin/atx-vol-american-markov-audit.exe --full |
  Set-Content -Encoding UTF8 C:/atx-cache/quote-benchmark/markov-audit-20261016-full.json

powershell -NoProfile -File C:/atx-wt/pool-36/scripts/atx-build.ps1 `
  -Preset vol-hygiene -Jobs 2 configure
```

Release configure/build and both executions exited 0. Hygiene configure exited
0, but the source's PCH-off object check was **not run** before the stop.
Formatting-only line wraps followed the successful Release build; no semantic
change remains after that build. `git diff --check` was clean before commit.
No unit test or core-library change was made in this lane.

All owned process handles are terminal: lease launcher38201 exited1 after its
expected missing-optional-directory dev configure (lease remained held);
Release configure25065 exited0; build77482 exited0; full run43513 exited0;
hygiene configure11190 exited0. Default run completed synchronously exit0.
There are no running builds or diagnostics to preserve.

## Measured evidence

Default 11 fixtures: all 11 Markov compilers and scalar/batch pricing succeed;
6 PWL surfaces succeed and 5 refuse. Refusals are the martingale crossing plus
node-cap failures in high_vol, negative_rate, negative_rate_and_yield, and the
last dated_cash expiry.

Full 88 combinations: 84 Markov models compile, 4 explicitly refuse on expanded
transition cap. Among 84 compiled models, 36 PWL surfaces succeed and 48 refuse.
No partial surface or silently relaxed cap is reported as success.

- Maximum scalar/batch disagreement across full panel: `2.1316282072803006e-13`.
- Maximum successful PWL/batch discrepancy: `1.9895196601282805e-13`.
- Default deterministic cash vs continuous deterministic reference discrepancy:
  `1.4210854715202004e-13`.
- Default funded European maximum absolute error: `.19954653065124672`.
- Separate-clock European maximum absolute errors: default65 `.1698354797809487`,
  uniform129 `.03249547702450739`, uniform257 `.006656255806111844`.
- Martingale European maximum absolute errors: default65 `.07829207889303214`,
  uniform129 `.04362021369420299`, uniform257 `.010785942496881695`;
  log257 `.0050793293616178126`.
- High-vol `.8`, 1-year, domain2 has maximum European error `8.048122049631104`.
  Domain4 default65 error is `.09164861996276841`. Upper-boundary exposure is
  material and finer resolution alone cannot establish tail accuracy.

These results expose approximation cost/coverage, not a calibrated market fit.

SHA256:

| Artifact | SHA256 |
| --- | --- |
| pool-36 Release executable | `3d62bbcc674e480b5aaaf3a52487672a5e3ea7c9f65dd26bdbbe1eef835fd4a3` |
| `markov-audit-20261016-default.json` | `9be912b6e5c6d643c308d6a40242a9b48422ef09d72d799a85e539065ae0bba4` |
| `markov-audit-20261016-full.json` | `99005ea4eaba9ac42e27ce40cacb37bba91920dc86e9da38f5095a40ad6c5a50` |

The JSON files are external under `C:/atx-cache/quote-benchmark/`; preserve them
and use new output paths after changing algorithms or fixtures.

## Recommended continuation, only after user resumes

1. Integrate this new diagnostic target, run the missing PCH-off source check.
2. Instrument and repair the martingale near-endpoint crossing without relaxing
   no-arbitrage admission; validate the exact failing example after the fix.
3. Evaluate representation cost before raising caps. Default finite-model scalar
   pricing works across the panel while exact PWL retention often explodes.
4. Add ATM/near-ATM probes, independent stochastic American benchmarks, and
   separate state, clock, and domain refinements. A single coarse/fine match is
   not an accuracy certificate.
5. Extend cash reference evidence with a single-cash European conditional
   lognormal quadrature or terminal-cash analytic formula, then an independent
   monotone American obstacle solver. Do not use the existing quadratic cash
   extrapolator as proof of a positive shared model.
6. For performance work, split compile, scalar, batch and hot PWL queries;
   prebuild query arrays outside timing, test odd batch tails and capacity
   reuse, pin quiet Release runs, and report latency distribution plus errors.

Research checked during this review: QuantLib's current
[American test source](https://raw.githubusercontent.com/lballabio/QuantLib/master/test-suite/americanoption.cpp)
contains negative-rate/no-exercise cases and QD fixed-point precision comparisons;
its [dividend tests](https://raw.githubusercontent.com/lballabio/QuantLib/master/test-suite/dividendoption.cpp)
provide useful independent fixture ideas. Those are starting points for an oracle
panel, not evidence that our solver passes it. The
[Cox/Hoeggerl paper](https://arxiv.org/pdf/1301.5467) has specialized American price
consistency/model-existence results whose assumptions must be retained; they are
not a replacement for our cash/carry process specification.
