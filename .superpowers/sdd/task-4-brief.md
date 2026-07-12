### Task 4: Shape-blend (FLEX-style) arbitrary-expiry interpolation

**Files:**
- Modify: `atx-vol/include/atx/vol/projection.hpp` + `atx-vol/src/projection.cpp` (new `InterpMode::ShapeBlend`), or free functions if the insert-slice path resists extension — implementer reads `projection.hpp` first and follows its provenance-flag conventions.
- Test: extend `atx-vol/tests/projection_test.cpp`.

**Model (SpiderRock FLEXVolInterpolation, verbatim):**
- Weights: `wwHi = (T_q − T_lo)/(T_hi − T_lo)`, `wwLo = 1 − wwHi`.
- ATM: `atm(T_q) = sqrt[(wwLo·T_lo·atm_lo² + wwHi·T_hi·atm_hi²)/T_q]` (linear in ATM total variance).
- Strike vol: `σ(k, T_q) = atm(T_q) · (wwLo·m_lo(z) + wwHi·m_hi(z))` where `m_x(z) = σ_x(k_x(z))/atm_x` evaluated at the SAME standardized moneyness `z = k/(atm(T_q)·√T_q)` mapped into each slice's own coordinates `k_x = z·atm_x·√T_x`.
- Single bracketing slice ⇒ weight 1.0 on it.

**Contract:** current `InterpMode::PiecewiseTotalVariance` stays default and bit-identical. `ShapeBlend` is an opt-in mode of `surface_insert_vol_slice`/`surface_eval_ex`. ATM total variance under ShapeBlend must still be monotone when the input slices are calendar-clean (it is linear-in-w at ATM by construction); non-ATM calendar safety is NOT guaranteed — document, and set a provenance flag bit (follow the reserved-bit pattern in projection.hpp).

**Test cases:**
```cpp
TEST(Projection, ShapeBlendMatchesLinearWForIdenticalShapes) {
  // two SVI slices, same shape scaled in T: ShapeBlend == PiecewiseTotalVariance within 1e-10 at ATM,
  // and within 3e-3 vol across |z|<=2 (shape identical => multiples identical).
}
TEST(Projection, ShapeBlendPreservesSkewBetweenSlices) {
  // slice lo: strong put skew; slice hi: flat. Blended 25d-put-vs-call spread must lie
  // strictly between the two slices' spreads (linear-w does not guarantee this in z-space).
}
TEST(Projection, ShapeBlendExactAtSliceT) { /* T_q == T_lo reproduces slice lo exactly */ }
TEST(Projection, ShapeBlendAtmIsLinearInTotalVariance) { /* atm² · T_q linear check */ }
```

**Steps:** read projection.hpp/cpp; tests → fail → implement → pass → gate → commit `feat(atx-vol): ShapeBlend (FLEX-style vol-multiple) time interpolation mode`.

**Acceptance:** default mode bit-identical (existing projection tests untouched); new tests pass.

---

