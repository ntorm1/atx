### Task 5: Skew-adjusted delta / VegaSlope (`adjusted_greeks`)

**Files:**
- Create: `atx-vol/include/atx/vol/adjusted_greeks.hpp`, `atx-vol/src/adjusted_greeks.cpp`
- Test: `atx-vol/tests/adjusted_greeks_test.cpp`
- Modify: both CMakeLists.

**Model (SpiderRock LiveVolSurfaces / ClientVolatilitySurfaces, verbatim):**
- `Adjusted Δ = Δ + VegaSlope × Vega`, `VegaSlope = dσ/dS` from the smile sliding with the underlying.
- Sticky control ω = refUPrcWeight: ω=0 sticky-delta (curve slides), ω=1 sticky-strike (curve pinned) ⇒ `VegaSlope = (1−ω) · dσ/dS|slide`.
- Under sticky-delta with k = ln(K/F), F ∝ S: `dσ/dS|slide = −(∂σ/∂k)/S`.

**Interfaces (Produces):**
```cpp
namespace atx::vol {
struct StickyParams { double ref_uprc_weight{0.0}; };  // ω in [0,1]; 0 = sticky-delta

// ∂σ/∂k at k_log from the curve's analytic w-slope: σ=sqrt(w/T),
// dσ/dk = w'(k)/(2·σ·T), w'(k) by central FD h=1e-4 on IVolCurve::w.
[[nodiscard]] double curve_skew_slope(const IVolCurve& c, double k_log) noexcept;

// (1−ω)·(−skew_slope/S). S <= 0 => NaN.
[[nodiscard]] double vega_slope_per_spot(const IVolCurve& c, double k_log, double S,
                                         const StickyParams& sp = {}) noexcept;

// delta ← delta + vega_slope·vega (other fields unchanged).
[[nodiscard]] Greeks skew_adjusted(const Greeks& g, double vega_slope) noexcept;
}
```

**Test cases:**
```cpp
TEST(AdjustedGreeks, FlatSmileLeavesDeltaUnchanged) { /* LinearVarianceCurve flat -> slope 0 */ }
TEST(AdjustedGreeks, SviSlopeMatchesAnalytic) {
  // SviCurve: dw/dk analytic = b*(rho + (k-m)/sqrt((k-m)^2+sig^2)); compare FD path, tol 1e-6.
}
TEST(AdjustedGreeks, StickyStrikeOmegaOneIsRaw) { /* ω=1 -> vega_slope 0 */ }
TEST(AdjustedGreeks, PutSkewLowersCallAdjustedDelta) {
  // negative skew slope (put wing higher): call adjusted delta < raw delta.
}
```

**Steps:** tests → fail → implement → pass → gate → commit `feat(atx-vol): SpiderRock-style skew-adjusted delta (VegaSlope, sticky-delta/strike control)`.

---

