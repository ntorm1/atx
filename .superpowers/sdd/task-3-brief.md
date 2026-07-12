### Task 3: Vol-multiple cubic-spline curve (`SplineVol`) + fitter

**Files:**
- Create: `atx-vol/include/atx/vol/spline_curve.hpp`, `atx-vol/src/spline_curve.cpp`
- Modify: `atx-vol/include/atx/vol/vol_curve.hpp` (enum + adapter decl or include), `atx-vol/src/curve.cpp`/wherever `to_string(VolCurveKind)` + `fit_slice_curve` dispatch live (locate: `atx-vol/src/curve_fit.cpp` / `vol_curve` impl — grep `to_string(VolCurveKind`).
- Test: `atx-vol/tests/spline_curve_test.cpp`
- Modify: both CMakeLists.

**Model (SpiderRock LiveVolSurfaces):** curve = cubic spline over standardized moneyness with volatility *multiples* `m = σ_K/σ_ATM` on a fixed grid; moneyness `z = ln(K/F)/(σ_ATM·√T)` (LogStd); wings flat beyond the outermost knot; serve `σ(k) = σ_ATM · m(z)`.

**Interfaces (Produces):**
```cpp
// spline_curve.hpp
namespace atx::vol {

inline constexpr std::array<double, 29> kSrMoneynessGrid = {
    -25, -14, -11, -8.5, -6.5, -5, -3.75, -2.75, -2, -1.5, -1, -0.75, -0.5,
    -0.25, 0, 0.25, 0.5, 0.75, 1, 1.5, 2, 2.75, 3.75, 5, 6.5, 8.5, 11, 14, 25};

struct SplineVolParams {
  double atm_vol{0.0};                       // σ_ATM > 0
  std::vector<double> z;                     // knot grid, strictly increasing
  std::vector<double> mult;                  // ‖z‖ vol multiples, > 0
  double z_lo_valid{0.0}, z_hi_valid{0.0};   // observed-moneyness range; flat outside
};

struct SplineFitOpts {
  std::span<const double> grid{kSrMoneynessGrid};  // knot z-grid
  double lambda{1e-3};       // 2nd-difference roughness penalty on multiples
  double mult_floor{0.05};   // post-solve clamp
  std::size_t min_obs{6};    // below this: InvalidArgument
};

class SplineVolCurve final : public IVolCurve {
 public:
  SplineVolCurve(SplineVolParams p, double T, double F, double df);
  [[nodiscard]] double w(double k_log) const noexcept override;   // (atm·m(z))²·T
  [[nodiscard]] VolCurveKind kind() const noexcept override;      // SplineVol
  [[nodiscard]] std::size_t dof() const noexcept override;        // active knots
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override;
  [[nodiscard]] const SplineVolParams& params() const noexcept;
};

// Penalized WLS fit of knot multiples from de-Americanized European obs.
[[nodiscard]] Result<std::unique_ptr<IVolCurve>>
fit_spline_vol_slice(std::span<const FitObs> obs_eu, double F, double T, double df,
                     const SplineFitOpts& opts = {});
}
// vol_curve.hpp: enum gains `SplineVol = 5`; CurveConfig gains `SplineFitOpts spline{};`
// fit_slice_curve dispatch gains a SplineVol case (no w_prev support v1 — document).
```

**Fitting algorithm:**
1. σ_ATM seed: vega-weight-weighted mean of obs IVs with |k| ≤ 0.5·σ_guess·√T (fallback: global vega-weighted mean; σ_guess = global mean IV). Then one refinement pass: σ_ATM = spline-interpolated fit at z=0 after solve, re-standardize once (two-pass total, deterministic).
2. Standardize each obs: `z_i = k_i/(σ_ATM√T)`, target `y_i = iv_i/σ_ATM`, weight `wt_i = FitObs.weight_w`.
3. Restrict to active knots: knots inside `[min z_i − 1, max z_i + 1]` (never fewer than 4); outer knots excluded from DoF and pinned by the natural-spline flat extension.
4. Cardinal natural-cubic-spline basis: for each active knot j solve the tridiagonal natural-spline system for the unit vector e_j once (O(K²) total, K ≤ 29); basis matrix `B[i][j] = basis_j(z_i)`.
5. Solve `(BᵀWB + λ·DᵀD)·m = BᵀWy` where D = second-difference matrix over knots, via `atx::core::linalg::solve_spd` (same helper the C8 LM uses). Clamp `m` to `[mult_floor, ∞)`.
6. Diagnostics: post-fit Roper `g(k) ≥ 0` scan on a 128-pt k-grid within the valid range; count violations (do NOT project v1 — record count; callers can reject). Store in fit report the same way existing fitters expose diag (return curve; violations logged via counter or accessible via params — expose `n_butterfly_viol` on `SplineVolParams`).
7. Eval: binary-search knot interval, cubic Hermite/natural-spline eval; `z` clamped to `[z.front(), z.back()]` (flat wings); w NaN if T/F/df invalid.

**Test cases (write first):**
```cpp
TEST(SplineVol, FlatSmileRoundTrip) {
  // obs from flat 20% smile, 15 strikes: fit → every mult ≈ 1, atm ≈ 0.20, iv(k)=0.20
}
TEST(SplineVol, RecoversSviSmile) {
  // Generate obs from a raw-SVI slice (a=.02,b=.4,rho=-.3,m=0,sigma=.4,T=.25,F=100);
  // 25 strikes, tight uniform weights. RMSE(iv) < 2e-3 inside observed range.
}
TEST(SplineVol, WingsAreFlat) { /* iv at z=40 == iv at z clamp boundary */ }
TEST(SplineVol, DofCountsActiveKnots) { /* narrow board -> dof < 29 */ }
TEST(SplineVol, CloneIsDeepAndIdentical) { /* clone then compare w() on grid */ }
TEST(SplineVol, DispatchThroughFitSliceCurve) {
  // CurveConfig{kind=SplineVol} through fit_slice_curve returns kind()==SplineVol
  // and serves through CurveSurface (push + w/iv query).
}
TEST(SplineVol, RejectsDegenerateInputs) { /* <min_obs, F<=0, T<=0 */ }
TEST(SplineVol, ButterflyViolationCounterOnConvexData) { /* clean synthetic -> 0 */ }
```

**Steps:** tests → fail → implement (spline_curve.* first, then enum/dispatch wiring) → pass → full gate (existing golden tests must be untouched: SplineVol is NOT added to `default_selector_candidates()` v1) → commit `feat(atx-vol): SplineVol vol-multiple cubic-spline curve family (SpiderRock SRCubic-style)`.

**Acceptance:** new tests pass; gate green; `default_selector_candidates()` unchanged.

---

