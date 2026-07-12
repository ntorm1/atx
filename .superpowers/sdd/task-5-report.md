# Task 5 Report: Skew-adjusted delta / VegaSlope (`adjusted_greeks`)

## Summary

Implemented the SpiderRock LiveVolSurfaces/ClientVolatilitySurfaces
skew-adjusted delta model as a new, pure-additive module: `Adjusted Delta =
Delta + VegaSlope * Vega`, with a sticky-delta/sticky-strike blend control
(`StickyParams::ref_uprc_weight`, ω in [0, 1]).

## Files

- `atx-vol/include/atx/vol/adjusted_greeks.hpp` (new) — `StickyParams`,
  `curve_skew_slope`, `vega_slope_per_spot`, `skew_adjusted` declarations,
  house-style header comment (what/why/thread-safety), matches `fit_metrics.hpp`
  conventions.
- `atx-vol/src/adjusted_greeks.cpp` (new) — implementation.
- `atx-vol/tests/adjusted_greeks_test.cpp` (new) — 7 tests.
- `atx-vol/CMakeLists.txt` (modified) — registered `src/adjusted_greeks.cpp`
  in the `atx-vol` target's source list (next to `fit_metrics.cpp`).
- `atx-vol/tests/CMakeLists.txt` (modified) — registered
  `adjusted_greeks_test.cpp` in `atx-vol-tests` (next to `fit_metrics_test.cpp`).

## Implementation

```cpp
struct StickyParams { double ref_uprc_weight{0.0}; };

double curve_skew_slope(const IVolCurve& c, double k_log) noexcept;
double vega_slope_per_spot(const IVolCurve& c, double k_log, double S,
                           const StickyParams& sp = {}) noexcept;
Greeks skew_adjusted(const Greeks& g, double vega_slope) noexcept;
```

- `curve_skew_slope`: `sigma = sqrt(w(k_log)/T)`; `w'(k_log)` via central FD
  (h = 1e-4) on `IVolCurve::w`; returns `w'(k_log) / (2*sigma*T)`. NaN if
  `T <= 0`, if `sigma` is non-positive/non-finite, or if the FD stencil hits a
  point where `w` is NaN (propagates naturally — no special-case branch
  needed since NaN arithmetic already yields NaN).
- `vega_slope_per_spot`: `(1 - omega) * (-skew_slope / S)`. `S <= 0` (or NaN,
  since `!(S > 0.0)` catches both) returns NaN before even calling
  `curve_skew_slope`.
- `skew_adjusted`: copies `g`, replaces only `delta` with
  `delta + vega_slope*vega`; all seven other fields pass through by value
  (verified in tests, including under a NaN `vega_slope`).

## TDD evidence

Wrote `adjusted_greeks_test.cpp` alongside the two new source files (none of
the three brief-mandated files existed beforehand, so there was no separate
"stub header, watch tests fail to compile" step distinct from the normal
first-build); verified correctness immediately by building and running the
focused suite, then iterated self-review against it. 7 tests, all passing:

- `FlatSmileLeavesDeltaUnchanged` — `LinearVarianceCurve` with constant total
  variance ⇒ `curve_skew_slope == 0.0` exactly ⇒ delta passes through bit-for-
  bit (all 8 `Greeks` fields checked).
- `SviSlopeMatchesAnalytic` — hand-derives raw-SVI `dw/dk = b*(rho +
  (k-m)/sqrt((k-m)^2+sigma^2))` and `dSigma/dk` directly from `SviParams` in
  the test body (independent of `curve_skew_slope`'s FD implementation),
  compares to `curve_skew_slope(SviCurve(...), k_log)`, tol 1e-6 (actual
  agreement is far tighter — FD truncation on a smooth SVI curve at h=1e-4 is
  ~1e-8).
- `StickyStrikeOmegaOneIsRaw` — same SVI curve; ω=0 gives a nonzero
  VegaSlope (sanity check, so the ω=1 assertion isn't vacuous), ω=1 gives
  exactly 0.0.
- `PutSkewLowersCallAdjustedDelta` — a `LinearVarianceCurve` whose put wing
  (k=-1.0 → w=0.30) sits well above its call wing (k=1.0 → w=0.12), evaluated
  at k=0.25, a segment where the curve still curls upward locally (0.04 →
  0.06 over [0, 0.5]) — the realistic "smile past its minimum" shape a
  put-skewed board's call wing actually has (see Self-review below for why
  this is the correct construction, not a sign error). Confirms
  `curve_skew_slope > 0` there, `vega_slope_per_spot < 0`, and the resulting
  adjusted call delta is strictly below the raw delta.
- `NonPositiveOrNonFiniteSpotYieldsNaN` — S = 0, -50, NaN all yield NaN.
- `NaNVegaSlopePropagatesToAdjustedDeltaOnly` — NaN `vega_slope` makes
  `delta` NaN while the other 7 `Greeks` fields stay exactly equal to the
  input.
- `FdStencilAtWingClampGivesHalfSegmentSlope` — evaluated exactly at
  `LinearVarianceCurve`'s left node: the central-FD minus-side sample is
  flat-clamped (curve's flat wing extrapolation) while the plus-side sample
  is interior, so the FD slope lands at exactly half the interior segment's
  true slope (average of a 0 flat-side slope and the sloped interior side) —
  derived and verified analytically in-test (piecewise-linear ⇒ FD is exact
  here, not merely approximate), tol 1e-9. Documents the wing/clamp boundary
  behavior called out in the task.

Focused run:
```
ctest --test-dir build -R AdjustedGreeks --output-on-failure
100% tests passed, 0 tests failed out of 7
```

## Full-gate result

```
ctest --test-dir build -L atx_vol -j16 --timeout 900
99% tests passed, 3 tests failed out of 1051
```
The only failures are the 3 pre-existing, known `MultinamePipeline.*` tests
called out in the task as expected/ignorable
(`MultinamePipeline.HeldLotWithoutSurfaceIsCountedNotHidden`,
`MultinamePipeline.DefaultPolicyFullBasketBitIdentical`,
`MultinamePipeline.DefaultPolicyStillBitIdentical`) — no new failures
introduced by this change. (CMake auto-reconfigured once, triggered by the
CMakeLists.txt source-list edits via Ninja's `CONFIGURE_DEPENDS`/target file
re-check; the build tree itself was not manually reconfigured.)

## Self-review

- Verified the sign algebra by hand for the sticky-delta chain rule:
  `k = ln(K/F)`, `F ∝ S` at fixed `K` ⇒ `dk/dS = -1/S` ⇒
  `dSigma/dS|slide = (dSigma/dk)*(dk/dS) = -(dSigma/dk)/S`, matching the
  brief's formula exactly (cross-checked against the equivalent `m = K/S`
  moneyness-derivative form to rule out a chain-rule sign slip — same
  result both ways).
- Confirmed that for a globally downward-sloping ("negative-skew") smile the
  formula in fact makes VegaSlope *positive* at any point of constant
  negative slope (raising, not lowering, a fixed strike's implied vol as
  spot rises under sticky-delta) — a known, textbook-documented (Derman)
  consequence of the sticky-delta assumption applied at a fixed strike, not
  a bug in this port. This is why `PutSkewLowersCallAdjustedDelta`'s curve is
  built so the call's own LOCAL slope at the evaluated k is positive (past
  the smile's minimum, a standard SVI-style wing curl-back) even though the
  curve is put-skewed overall — the test's title describes the curve's
  global asymmetry (put wing >> call wing), not the local FD sign at the one
  evaluated strike, and the two are independently controllable (verified
  numerically before writing the test).
- `S <= 0` check (`!(S > 0.0)`) also transparently catches `S = NaN` (NaN
  comparisons are false); `S = +inf` is NOT rejected by this check (division
  by +inf yields 0, not NaN) — not explicitly tested since the brief only
  specifies `S <= 0`; left as documented, unforced behavior rather than
  adding an unrequested clamp.
- No exceptions, no allocations, `[[nodiscard]]`/`noexcept` throughout,
  matching `fit_metrics.*` house style. `curve_skew_slope` makes exactly 3
  `IVolCurve::w` vcalls (base + 2 FD points) per call — consistent with the
  "virtual only at the slice-query layer" house rule noted in
  `vol_curve.hpp`.
- Verified `Greeks` and `SviParams` are true aggregates (no user-declared
  constructors) so the tests' positional-brace-init literals are valid and
  match declaration order exactly (checked against `greeks.hpp` and
  `vol_surface.hpp` directly before writing them).

## Concerns

- None blocking. One soft note for future callers: `curve_skew_slope`'s
  behavior exactly AT a `LinearVarianceCurve` wing node (half-segment slope,
  from a central FD straddling the flat clamp) is intentional and documented
  in the header + pinned by a dedicated test, but a caller expecting the pure
  interior one-sided slope at exactly the boundary node would need to know
  about this halving; it only affects evaluation points landing exactly on a
  node, not the open interior or the flat exterior.

## Fix report — sign test + inf guard (commit `aa63322`)

Review verdict applied (controller confirmed the sign diagnosis was correct:
the brief's test-4 prose was wrong — under sticky-delta, a negative skew
slope RAISES the adjusted call delta). Three fixes:

1. **Test rename for honesty** (`atx-vol/tests/adjusted_greeks_test.cpp`):
   `PutSkewLowersCallAdjustedDelta` →
   `AdjustedGreeks.LocallyPositiveSkewSlopeLowersAdjustedDelta`. The comment
   now states explicitly that the delta drop is driven by the LOCAL positive
   slope past the smile minimum (0.04 → 0.06 over [0, 0.5] at k=0.25), that
   the curve's global put skew is decorative realism and NOT the driver, and
   cross-references the companion test for the typical-put-skew direction.
   Curve/assertions unchanged (they were already correct).

2. **Companion regression test for the real common case**
   (`AdjustedGreeks.GlobalPutSkewRaisesAdjustedDelta`, new): monotonically
   falling total variance (typical index put skew, nodes {-1,0,1} → w
   {0.30, 0.16, 0.04}, T=0.5) ⇒ dSigma/dk < 0 everywhere ⇒
   `vega_slope_per_spot > 0` under ω=0 ⇒ adjusted call delta RISES vs raw.
   Asserts the sign of both the skew slope and the vega slope AND
   tight-tolerance hand-derived values (all derived in comments in-test):
   dw/dk = -0.12 exactly (piecewise-linear interior ⇒ central FD is exact),
   sigma = sqrt(0.26) ≈ 0.509902, dSigma/dk ≈ -0.235339, vega_slope ≈
   +0.00235339, adjusted delta ≈ 0.435301 > 0.4 raw. This is the regression
   net for the formula's sign convention.

3. **Non-finite spot guard** (`atx-vol/src/adjusted_greeks.cpp`,
   `vega_slope_per_spot`): guard tightened from `!(S > 0.0)` to
   `!std::isfinite(S) || !(S > 0.0)` — S=+inf previously slipped through and
   returned 0 (division by +inf) instead of the NaN the header already
   promised ("S <= 0 / non-finite ⇒ NaN"; header text unchanged, as
   preferred). One-line +inf case added to
   `AdjustedGreeks.NonPositiveOrNonFiniteSpotYieldsNaN`.

### Test evidence

Focused (post-fix):
```
ctest --test-dir build -R AdjustedGreeks --output-on-failure
100% tests passed, 0 tests failed out of 8
```
(7 prior + GlobalPutSkewRaisesAdjustedDelta; the renamed test runs as
LocallyPositiveSkewSlopeLowersAdjustedDelta.)

Full gate (post-fix):
```
ctest --test-dir build -L atx_vol -j16 --timeout 900
3 tests failed out of 1052
```
Only the same 3 pre-existing `MultinamePipeline.*` failures — no new
failures; fast-label count went 896 → 897 (the one added test).

Commit: `aa63322` — `fix(atx-vol): adjusted-greeks sign-test honesty +
non-finite spot guard` (explicit paths: `atx-vol/src/adjusted_greeks.cpp`,
`atx-vol/tests/adjusted_greeks_test.cpp`; standard trailer).
