# Triage: Two Pre-existing Debug Test Failures (Sprint I, step 6)

Triage agent report, 2026-07-16. Reproduced in `wt-s-surface` Debug tree (branch base `main@7fca341`); both verified unfixed by newer main (`7fca341..51df565` touches only de-Am shared-lane numerics).

## Shared facts

- Neither failure is introduced by any sub-sprint branch (K/A/S touch disjoint TUs; both fail at base).
- Neither is fixed by Sprint R's R-02 as prescribed: the R-02 fix is **selector-gated** (`selection_.has_value()`), and both failing boards are **direct-routed** (`selection_` empty), so `selector_served_admission_policy` is never consulted for them.

## Failure 1 — `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`

```
surface_v2_qualification_test.cpp(569): Value of: decision.used_fallback  Actual: false  Expected: true
surface_v2_qualification_test.cpp(571): Expected: (decision.curve.kind) != (VolCurveKind::ConvexDense), actual: <00> vs <00>
```

- **Root cause:** the adversarial fixture `make_extension_crossed_chain()` (test lines ~516–545) is no longer adversarial. Its served ConvexDense risk slices were meant to produce a calendar crossing inside the Balanced ±0.5 validation band and force the fallback ladder; at HEAD the primary ConvexDense *passes* `validate_risk_surface` + admission, so the fitter admits it directly and never walks the ladder (`pricer_fitter.cpp` ~1304–1348). The ladder itself is healthy (it correctly routes Svi→eSSVI for vxx-close in Failure 2).
- **Introduced-by:** production-side, in `(c83c9fe, 7fca341]`. Leading (unproven) suspect: `b118439` (2026-07-12, hardened independent admission oracle — also changed `dense_slice.cpp` QP-feasibility box-clamp on ConvexDense node starts). The calendar check itself was strengthened, so if `b118439` is the cause it is via the dense-slice shape change, not a weakened oracle. Other in-window de-Am numerics (e.g. `cf615f4`) could also flip this fragile fixture.
- **Secondary possibility (unresolved read-only):** genuine far-wing calendar blind spot — the hardened oracle reconstructs `w` via Black, "in-bounds by construction", and cannot see a served ConvexDense call price the fit clamped (oracle finding I-2 note at `pricer_fitter.cpp` ~92–107).
- **Proposed fix (v2 owner):** instrument the served ConvexDense calendar band for this fixture at HEAD. (a) If genuinely calendar-safe now → re-tune the fixture (steeper 3m put wing / flatter 4m, or pull last quoted strike inward) so the crossing lands inside ±0.5 again. (b) If it still crosses but the oracle misses it → validation blind spot: extend `validate_risk_surface` calendar sampling into the served extrapolation region.
- **R-02 overlap:** none (calendar validation, not quote coverage; direct-routed).

## Failure 2 — `OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard`

```
[breadth corpus] vxx-close vol-product/close-2m curve=essvi fit=2382ms pxCLN=100.00% (9/9) legs=1378
opra_breadth_corpus_test.cpp(67): Expected: (score.n_clean) > (10u), actual: 9 vs 10
```

- **Root cause:** vxx-close (VolProduct @0.95 conf → direct route → primary Svi fails risk validation → ladder admits **eSSVI** fallback) serves a legitimately sparse risk surface with 9 clean strikes; the generic breadth floor `n_clean > 10` (added in `0eb62b7`, calibrated on dense boards) rejects it even though price accuracy is 9/9 in-band. Default Mark-serving admission has `min_quote_coverage=0` and no served-breadth floor on direct/fallback routes.
- **Exposed by** (not introduced by): the v2 correctness merge (`da718f7` + `cf615f4` + carry-budget fixes) which made vxx-close scoreable at all; the fixture's own comment says vxx-close "was never empirically validated" pre-`da718f7`.
- **Proposed fix (breadth owner), two options:**
  - (a) Test-expectation, lowest risk: per-fixture `min_risk_clean` in `tests/support/breadth_fit_fixture.hpp` (vxx-close ≤ 9; dense boards keep > 10), gate read in `opra_breadth_corpus_test.cpp:67`.
  - (b) Production, if intent is reject-narrow: add a served-breadth/coverage floor to v2/risk `admission_attempt` (`pricer_fitter.cpp` ~1284) applied on **all** routes — a superset of R-02. Flips vxx-close to Rejected → test `continue`s → passes. Must re-run the 14-board corpus (`admitted >= 9` still required); may reject other marginal vol-product boards.
- **R-02 overlap:** conceptually related ("narrow served surface silently published") but R-02 verbatim does NOT fix it (selector-gated vs direct-routed). Coordinate any option-(b) change with the Sprint R engineer — same code region.

## Bottom line

Both pre-existing at `main@7fca341`, both unfixed on current main, both on the v2/eSSVI production path, neither fixed by R-02 as written. Failure 1 = stale/fragile fixture (or a calendar blind spot — needs one instrumentation run to disambiguate). Failure 2 = breadth-floor calibration vs a legitimately sparse vol-product board (or a missing all-routes coverage floor, R-02-superset).
