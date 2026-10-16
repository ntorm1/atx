# Bounded representation of unresolvable exercise crossings

Implemented after the user resumed, on base
`4fa9afddcf84afb4cfc78d4d0fac26a0ff72b744` in branch
`feat/vol-pwl-crossing-20261016`. Audit prerequisite `bd6e7b5f` was cherry-picked
locally as `407ed2e3` only to run its diagnostic. Integrate the subsequent fix
commit, not that duplicate prerequisite. Temporary audit CMake wiring is restored
before committing. No benchmark data or vendor labels were used.

## Reproduced cause

Actual default65 martingale model: spot100, calendar/variance expiry.25,
rate=yield=0, volatility.2, no cash, strike domain[40,180]. Instrumented output:

```text
left K=175, right K=175.00000000000003
continuation: 18.749999999999993, 18.749999999999979
intrinsic:    18.75,              18.749999999999972
gaps:        -7.105427357601002e-15, +7.105427357601002e-15
crossing fraction=.5, rounded crossing=175
```

The two strikes are adjacent doubles. There is no distinct representable strike
at the crossing. The old code refused the entire PWL surface despite scalar and
batch pricing succeeding. Temporary instrumentation was removed after diagnosis.

## Change and error accounting

All represented knots and representable interior crossings remain. Only when
the computed crossing rounds to an endpoint, the output uses the chord joining
the maxima at both endpoints. This is explicit bounded representation error,
not an arbitrary knot-compression tolerance.

For affine continuation/payoff segments with opposing endpoint gaps of absolute
magnitudes a,b, that chord's maximum excess above their pointwise maximum is
`a*b/(a+b) <= min(a,b)`. The implementation uses the conservative minimum bound,
rounded outward with `nextafter`. Segments have disjoint interiors, so the new
uniform error is their maximum, not the sum of every segment's error.

Each immutable function carries `crossing_resolution_error_bound()`:

- Payoff starts at zero.
- A nonnegative weighted sum propagates `sum(weight_i * bound_i)`.
- Exercise maximum is 1-Lipschitz in continuation. Its result bound is the
  inherited bound plus the largest newly introduced chord error.
- Bound multiplication/addition round outward; zero contributions remain exact
  zero. Nonfinite accumulation refuses.
- Every result must fit the original fixed allowance
  `64*epsilon*max(1,max_node_value)`. Repeated operations cannot reset their
  inherited error budget. No caller knob changes this allowance.

The global convex-hull admission check is unchanged and independently applies
to the complete output function. Resource caps are unchanged. This bound covers
only the additional unresolved-crossing approximation; it does **not** certify
all ordinary floating-point arithmetic or finite-state diffusion error.

The surface diagnostic `max_output_crossing_resolution_error` exposes the largest
bound among returned sides/expiries. Individual functions expose their own bounds.
Public exact-representation wording now describes this allowance explicitly.
The model remains a finite Bermudan process, with its existing boundary, clock,
and cash conventions; this change makes no continuous-American accuracy claim.

## Validation and measured impact

29/29 focused Release tests passed via:

```powershell
powershell -NoProfile -File C:/atx-wt/pool-33/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 build atx-vol-american-markov-tests
ctest --test-dir C:/atx-wt/pool-33/build-vol-rel-avx2 `
  -R '^atx-vol-american-markov$' --output-on-failure
```

Implemented first, then added tests for both option sides at powers-of-two scales
2^-400,1,2^400;256 repeated weighted/exercise compositions with fresh crossings;
amplification that must refuse rather than reset the fixed allowance; and the
actual default65 rollback checked at1121 strike probes and every output knot.
Existing malformed-input, knot/cursor/cap, shape and calendar checks remain green.

Both sources and both tests passed the PCH-off check (both focused/monolith test
objects were compiled). The repeated-composition test was strengthened afterward,
then its PCH-off objects and the Release test suite passed again:

```powershell
powershell -NoProfile -File C:/atx-wt/pool-33/scripts/atx-build.ps1 `
  -Preset vol-hygiene -Jobs 2 check atx-vol/src/pricing/pwl_strike.cpp `
  atx-vol/src/pricing/american_markov_pwl.cpp atx-vol/tests/pwl_strike_test.cpp `
  atx-vol/tests/american_markov_pwl_test.cpp
```

The actual 88-combination audit panel was rerun, retaining default limits. All
scalar/batch price rows exactly match the prior panel.84 models still compile
and4 still refuse their model cap. PWL successes increase36->39; refusals48->45.
The successful additions are martingale/default65, martingale/domain2 and
martingale/log65. The other five former martingale crossing failures progress
until unchanged node caps refuse. No crossing-resolution refusal remains in
this panel. Default-grid PWL successes therefore increase6/11->7/11.

Default65 martingale:693 output nodes, maximum sampled PWL/batch difference
`5.684341886080802e-14`. Maximum successful PWL/batch difference over the entire
panel remains `1.9895196601282805e-13`. These differences include ordinary
arithmetic order effects; do not interpret the crossing bound as their sole bound.
The higher-resolution/node-cap limitations and earlier diffusion errors remain.

Authoritative final panel:
`C:/atx-cache/quote-benchmark/markov-crossing-fixed-20261016-full-final.json`
SHA256 `153ca5cc99ea5a284da90138e28924ea8982f3538d05923de9a03c580da95799`.
It contains all default-grid cases as well as the full refinement matrix.

Audit executable SHA256 at that run:
`981cb60336de0c15584dadbb1a86c6879873e9ed64c0fc5702b2aaf1d6e4c2a0`.
Command was `atx-vol-american-markov-audit.exe --full`, with the shared Release
vcpkg DLL directory on PATH, redirecting stdout as UTF8 JSON to the path above.
The run is numerical evidence only: timings were not quiet or pinned, and there
is no speed claim. Last post-run edits are comments/line wraps and test-only
strengthening; the algorithm used for the final panel is the delivered one.
