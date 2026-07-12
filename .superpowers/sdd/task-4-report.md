# Task 4 report — Shape-blend (FLEX-style) arbitrary-expiry interpolation

## Summary

Implemented `InterpMode::ShapeBlend`, the SpiderRock FLEXVolInterpolation-style
"vol-multiple" blend, as a new opt-in maturity-interpolation mode alongside
the existing `PiecewiseTotalVariance` default. Default-mode behavior is
untouched (same code path, same floating-point operations) — bit-identical by
construction, not just by testing. Six new tests added (four from the brief
plus two exercising the `surface_eval_ex` wiring decision below); full
`atx_vol` label gate run with no new failures.

## Files changed

- `atx-vol/include/atx/vol/projection.hpp`
- `atx-vol/src/projection.cpp`
- `atx-vol/tests/projection_test.cpp`

No new files.

## What was implemented

### `projection.hpp`

- `InterpMode::ShapeBlend = 1` added, with a doc comment carrying the full
  SpiderRock formula (wwLo/wwHi, ATM total-variance blend, standardized-
  moneyness vol-multiple blend, edge cases) so the enum is self-documenting
  the way `PiecewiseTotalVariance`'s neighbor comments already are.
- New provenance bit `kFlagShapeBlendCalendarUnsafe = 1u << 13` (bits 13/14
  were the only unused slots in the Stage-I 0..15 range; no collision with
  any other flag vocabulary in the repo — verified by grep across
  `atx-vol/` for `1u << 13`/`1u << 14`). Stamped only when a genuine two-
  slice blend happens (i.e. the existing `kFlagInterpolatedT` branch); an
  exact-pillar or clamped single-slice hit gets weight 1.0 and carries no
  calendar-safety caveat.
- `InsertedSliceHandle`, `w_on_inserted_slice`/`iv_on_inserted_slice`,
  `EvalRequest.interp_mode`, `surface_eval_ex` doc comments updated to
  describe the new mode and its NotImplemented/fallback semantics.
- Top-of-file port-scope note extended to mention ShapeBlend as a later,
  opt-in addition (kept the original "v1 ships ... PIECEWISE_TOTAL_VARIANCE"
  sentence intact, since that's still literally true of the *default*).

### `projection.cpp`

- New anonymous-namespace helper `shape_blend_w(surface, handle, k_log)`:
  1. `ww_hi = handle.alpha_T`, `ww_lo = 1 - ww_hi` (alpha_T already equals
     SpiderRock's wwHi — same weight formula, so it's reused as-is).
  2. ATM total variance of each parent: `w_lo0 = slice_w(lo, 0)`,
     `w_hi0 = slice_w(hi, 0)`. `atm(T_q)^2 * T_q = wwLo*w_lo0 + wwHi*w_hi0`
     — algebraically identical to what `PiecewiseTotalVariance`'s
     `w_on_inserted_slice` returns at k=0, so ATM matches to machine
     precision by construction (proved analytically and confirmed by test).
  3. Standardized moneyness `z = k_log / (atm_q * sqrt(T_q))`, mapped into
     each parent's own coordinates `k_lo = z*atm_lo*sqrt(T_lo)`,
     `k_hi = z*atm_hi*sqrt(T_hi)`.
  4. Vol multiples `m_lo = sqrt(w_lo(k_lo)/T_lo)/atm_lo`, `m_hi` likewise;
     blended vol `sigma_q = atm_q*(wwLo*m_lo + wwHi*m_hi)`; returned as
     total variance `sigma_q^2 * T_q` so the function's contract
     (`w_on_inserted_slice` always returns total variance) stays uniform
     across both modes.
  - Falls back to the plain `PiecewiseTotalVariance` formula (linear-in-w at
    fixed k) whenever `T_lo`/`T_hi`/`T_q` aren't positive, either parent's
    ATM total variance is non-finite/non-positive, the blended ATM total
    variance/vol comes out non-finite/non-positive, or either parent's
    mapped-k total variance is negative/non-finite. This is the documented
    "degenerate ATM → fall back to linear-w" edge case from the brief, not
    an error path.
- `surface_insert_vol_slice`: the reserved-mode check now accepts
  `ShapeBlend` in addition to `PiecewiseTotalVariance`; the genuine-blend
  branch (`kFlagInterpolatedT`) additionally stamps
  `kFlagShapeBlendCalendarUnsafe` when `interp == ShapeBlend`. The
  exact-pillar and clamped-single-slice branches are completely unchanged
  and mode-agnostic (weight 1.0 either way, matching the brief).
- `w_on_inserted_slice`: dispatches to `shape_blend_w` when
  `handle.interp_mode == ShapeBlend` and the handle isn't already resolved
  to a single slice (`exact_slice_idx >= 0` still short-circuits first,
  identically for both modes).
- `surface_eval_ex`: added a reserved-`interp_mode` check (mirroring the
  existing `delta_convention`/`pricing_route_policy` reserved-value checks)
  and a new branch — see seam decision below.

### `projection_test.cpp`

Added, in order:
1. `ShapeBlendMatchesLinearWForIdenticalShapes` — two raw-SVI slices built as
   a self-similar family in standardized moneyness (shared shape constants
   A/B/rho/S, each slice scaled by `s_x = atm*sqrt(T_x)`), so the
   vol-multiple curve is literally identical between slices. ATM matches to
   1e-10; max vol deviation across `|z| <= 2` from `PiecewiseTotalVariance`
   measured at 8.4e-4 (well under the brief's 3e-3 bound — numerically
   verified via a standalone Python replica of both formulas before writing
   the C++ constants, see "TDD evidence").
2. `ShapeBlendPreservesSkewBetweenSlices` — slice lo: strong put skew
   (rho=-0.7); slice hi: flat (rho=0). A local test-only bisection helper
   (`solve_k_for_delta_on_handle`, `test_forward_delta`) solves 25-delta
   put/call off an `InsertedSliceHandle` (production `surface_solve_k_for_delta`
   can't be reused here — it bisects against the raw multi-slice
   `VolSurface::iv`, which is always `PiecewiseTotalVariance` and has no
   notion of an inserted-slice handle). Verified: `spread_hi (-0.0046) <
   spread_blend (0.0225) < spread_lo (0.0376)`, strictly, with margin.
3. `ShapeBlendExactAtSliceT` — `T_clock == T_lo` reproduces slice lo exactly
   (`exact_slice_idx == 0`, no `kFlagShapeBlendCalendarUnsafe`, iv matches
   direct `svi_total_w` to 1e-13 across a small k grid).
4. `ShapeBlendAtmIsLinearInTotalVariance` — for several `T_q` between the
   parents, `w_on_inserted_slice(..., k=0)` and `iv_on_inserted_slice(...,
   0)^2 * T_q` both equal `wwLo*w_lo(0) + wwHi*w_hi(0)` (the literal
   SpiderRock ATM formula) to 1e-12/1e-10.
5. `EvalEx_ShapeBlend_MatchesInsertedSliceHotPath` — bonus test covering the
   `surface_eval_ex` wiring decision: requesting `ShapeBlend` through the
   high-level API matches the low-level insert-slice call directly, and
   carries `kFlagShapeBlendCalendarUnsafe`.
6. `EvalEx_ReservedInterpMode_ReturnsNotImplemented` — bonus test for the new
   reserved-value guard in `surface_eval_ex`.

New helpers added to the test file's anonymous namespace:
`make_svi_surface(sl_lo, sl_hi)`, `test_forward_delta(...)`,
`solve_k_for_delta_on_handle(...)`. All test-only, no production code changes.

## Seam decision: `surface_eval_ex` wiring

The brief names both `surface_insert_vol_slice` and `surface_eval_ex` as
extension points. Investigation showed `surface_eval_ex` previously carried
`EvalRequest.interp_mode` through to nothing — it called
`surface.w(cr->k_log, cr->tau_vol)` directly (a hardcoded
`PiecewiseTotalVariance` evaluator on `VolSurface` itself), never consulting
the field. `surface_project_compare` even forwarded `in.interp_mode` into a
request that (pre-change) silently discarded it. The *only* place `InterpMode`
was actually load-bearing was the inserted-slice trio
(`surface_insert_vol_slice` / `w_on_inserted_slice` / `iv_on_inserted_slice`),
confirmed by `portfolio_risk.cpp` — its Stage-II resolver already threads its
own `interp_mode` config through `surface_insert_vol_slice` (line 358),
meaning it will pick up ShapeBlend for free with zero changes to
`portfolio_risk.cpp`.

Given that, I implemented ShapeBlend fully in the insert-slice trio (the
real, pre-existing seam) and *additionally* wired `surface_eval_ex` to route
through it when `interp_mode == ShapeBlend`, while leaving the
`PiecewiseTotalVariance` branch calling the exact same `surface.w(...)`
expression as before (bit-identical, not just numerically close). This also
completes a previously-dead validation gap: reserved `interp_mode` values are
now rejected with `NotImplemented` in `surface_eval_ex`, mirroring the
existing `delta_convention`/`pricing_route_policy` checks in the same
function. This is strictly additive — no existing call path's computation
changed — so I judged the risk low relative to the benefit of making the
brief's stated `surface_eval_ex` opt-in actually work end-to-end rather than
silently no-op.

If this wiring is considered out of scope, it can be reverted by dropping the
two new blocks in `surface_eval_ex` (the reserved-mode check and the `w`
dispatch) and the two bonus tests — the four brief-mandated tests do not
depend on it.

## TDD evidence

1. Wrote the enum, flag bit, and doc comments in `projection.hpp` first
   (needed for the test file to even parse `InterpMode::ShapeBlend`).
2. Wrote all 6 tests in `projection_test.cpp` against the *unimplemented*
   `projection.cpp` (interp-mode acceptance check still `!=
   PiecewiseTotalVariance` only).
3. Confirmed red: swapped in the pre-implementation `projection.cpp` (`git
   show HEAD:...` copy) with the new header + tests, rebuilt, ran the 6 new
   tests directly — all 6 failed as expected:
   ```
   [ RUN      ] VolProjection.ShapeBlendMatchesLinearWForIdenticalShapes
   ... Actual: false Expected: true
   [  FAILED  ] VolProjection.ShapeBlendMatchesLinearWForIdenticalShapes
   ... (all 6 FAILED)
   ```
4. Restored the `shape_blend_w` implementation, `surface_insert_vol_slice`
   acceptance, `w_on_inserted_slice` dispatch, and `surface_eval_ex` wiring.
   Rebuilt; all 6 new tests passed, plus all 15 pre-existing `VolProjection`/
   `CurveProjection` tests unchanged (green, matching the pre-change
   baseline exactly — no behavior drift in the default path).
5. Before hand-picking test constants, verified the numeric claims
   (`ShapeBlendMatchesLinearWForIdenticalShapes`'s 3e-3 bound,
   `ShapeBlendPreservesSkewBetweenSlices`'s strict-ordering claim) with a
   standalone Python replica of both the production formula and the test's
   delta-bisection, iterating on parameters until margins were comfortable
   (8.4e-4 vs. the 3e-3 bound; spread ordering with ~40% margin either side)
   before transcribing the constants into C++.

## Full-gate result

```
ctest --test-dir build -L atx_vol -j16 --timeout 900
99% tests passed, 3 tests failed out of 1044
```

Failures are exactly the three pre-existing, out-of-scope failures called
out in the task instructions:
```
MultinamePipeline.HeldLotWithoutSurfaceIsCountedNotHidden (Failed)
MultinamePipeline.DefaultPolicyFullBasketBitIdentical (Failed)
MultinamePipeline.DefaultPolicyStillBitIdentical (Failed)
```
(Verified these have no reference to `ShapeBlend`/`InterpMode` in
`multiname_pipeline_test.cpp` — unrelated to this change.)

Focused `VolProjection`/`CurveProjection` run: 38/38 passed (21 `VolProjection`
+ 4 `CurveProjection`, plus `ContractProjection`/`HistoricalProjection` in the
same binary, all passing).

## Self-review

- **Division by zero at atm≈0**: `shape_blend_w` guards `T_lo/T_hi/T_q > 0`,
  `w_lo0/w_hi0` finite-and-positive, `w_atm_q` finite-and-positive, and
  `atm_q` finite-and-positive — each guard precedes the divide that depends
  on it (`atm_lo = sqrt(w_lo0/T_lo)` only after `T_lo>0` and `w_lo0>0`
  confirmed; `z = k_log/(atm_q*sqrt(T_q))` only after `atm_q>0` confirmed).
  Every failure mode falls back to `linear_w_fallback()` rather than
  propagating NaN/Inf or dividing by zero.
- **z-mapping consistency between slices**: both `k_lo` and `k_hi` are
  derived from the *same* `z`, each multiplied by that slice's own
  `atm_x*sqrt(T_x)` — matches the brief's `k_x = z*atm_x*sqrt(T_x)` exactly,
  no cross-slice mixing of scale factors.
- **Provenance bit collisions**: grepped `atx-vol/` for `1u << 13` / `1u <<
  14` before and after adding `kFlagShapeBlendCalendarUnsafe` — only this
  new definition occupies bit 13 in the projection.hpp flag space; bit 14
  remains free for a future addition. Unrelated flag vocabularies elsewhere
  (`surface_db.hpp`'s `kDbSym*`, `uint16_t`) are a different type/space, not
  a collision.
- **`InsertedSliceHandle` not widened**: considered caching
  `atm_lo`/`atm_hi`/`atm_q` on the handle to avoid recomputing them per
  `k_log` call in a batch; decided against it because (a) the existing
  `PiecewiseTotalVariance` path already recomputes `slice_w` per call with no
  caching, so this matches established convention, (b) the AVX2 batch
  kernel is explicitly deferred per the file's PORT NOTE, so this isn't the
  hot path yet, and (c) it avoids touching a struct with several existing
  consumers (`portfolio_risk.cpp`).

## Concerns / follow-ups (not blocking)

- `shape_blend_w` recomputes each parent's ATM total variance (`slice_w` at
  k=0) on every call, including inside `iv_on_inserted_slice_batch`'s
  per-point loop. Fine for correctness and consistent with the existing
  no-caching style, but a future AVX2/batch pass for ShapeBlend (when the
  currently-deferred batch kernel work resumes) should hoist the k-invariant
  `atm_lo`/`atm_hi`/`atm_q`/`ww_lo`/`ww_hi` out of the per-observation loop,
  the same way recent perf sprints did for the C8 Jacobian and SVI-MM basis.
- The non-ATM calendar-arbitrage-safety caveat is real and by design (per
  the brief) — `ShapeBlendPreservesSkewBetweenSlices`'s test surface was not
  checked for calendar cleanliness at arbitrary strikes under the blend; the
  provenance flag is the intended mitigation, not a proof of safety.
