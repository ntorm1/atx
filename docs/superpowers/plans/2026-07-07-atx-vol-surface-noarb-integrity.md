# atx-vol Served-Surface No-Arb Integrity — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the served ConvexDense/SVI `CurveSurface` provably static-arbitrage-free (butterfly **and** calendar), with Lee-controlled wings and an optional bid/ask-band objective.

**Architecture:** Add a calendar checker for `CurveSurface`; augment the dense per-slice active-set QP from homogeneous `Gx≥0` to `Gx≥h`; inject per-node calendar-floor rows computed from the previous slice's total variance; drive the slices sequentially in ascending T so calendar no-arb holds by construction (slack ⇒ bit-identical where no arb). Add a linear-total-variance Lee wing tail on query, and an exact interval (band) loss via slack variables behind a flag.

**Tech Stack:** C++20, Eigen (`atx::core::linalg` `MatX`/`VecX`, `solve`/`solve_spd`), GoogleTest (`gtest_discover_tests`), CMake. Error vocabulary: `atx::core::Result`/`Status`, `Err`/`Ok`, `ATX_TRY`.

## Global Constraints

- Language: C++20; warnings-as-errors (`atx_warnings`, `/WX` under clang-cl). No new external deps.
- Namespace: `atx::vol`. Follow existing file/idiom conventions exactly.
- **Zero fit-quality regression where no arbitrage exists:** enforcement must be *slack* on clean boards → the QP returns bit-identical node prices. The `h=0` augmented QP must equal the pre-change solver bit-for-bit.
- Default objective stays `CalibLossKind::Mid` (interval loss is opt-in behind a flag) so production fits are unchanged until deliberately flipped.
- Git: branch `feat/atx-vol-carry-deam` (commit directly OK). **Explicit-path staging only — never `git add -A`.** Stage only the files each task touches. Data dirs (`data/spy_ytd/**`) stay gitignored/untracked. End commit messages with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Build/test commands (run from `C:\atx`, Git Bash):
  - Configure (once): `cmake --preset <existing-preset>` — reuse the repo's configured build dir (check `build/` or `CMakePresets.json`).
  - Build tests: `cmake --build build --target atx-vol-tests -j`
  - Run one test: `./build/atx-vol/tests/atx-vol-tests --gtest_filter='<Suite>.<Case>'` (path per the build tree; adjust to actual output location).
  - Run all atx-vol tests: `ctest --test-dir build -R atx-vol --output-on-failure` (or run the `atx-vol-tests` binary directly).

---

## File structure (what changes)

- `include/atx/vol/arb.hpp` — declare `arb_check_calendar(const CurveSurface&, ...)` overload; include `vol_curve.hpp`.
- `src/arb.cpp` — implement the `CurveSurface` calendar checker.
- `src/session.cpp:277-308` — replace hardcoded `calendar_arb_free=false` with the measured result.
- `include/atx/vol/dense_slice.hpp` — `qp` no longer exposed (internal), but extend `ConvexFitOpts` (add `loss`); add a `w_prev` callback param to `fit_convex_slice`.
- `src/dense_slice.cpp` — augment `qp_active_set` to `Gx≥h`; add calendar-floor rows + `bound_slope_below` row; add Lee wing extrapolation to `ConvexSliceFit::call_price`/`iv`; add the interval-loss slack QP branch.
- `include/atx/vol/vol_curve.hpp` / `src/vol_curve.cpp` — thread the optional `w_prev` callback through `fit_slice_curve`.
- `src/curve_fit.cpp:99-171` — sequential driver: pass the previous fitted slice's `w(·)` as the next slice's floor.
- `tests/dense_slice_test.cpp`, `tests/arb_test.cpp`, `tests/spy_real_test.cpp` (or a new `tests/curve_noarb_test.cpp`) — the gate. Register any new test file in `tests/CMakeLists.txt`.

---

### Task 1: Calendar checker on `CurveSurface`

**Files:**
- Modify: `include/atx/vol/arb.hpp` (add include + one declaration)
- Modify: `src/arb.cpp` (add implementation)
- Test: `tests/arb_test.cpp` (add cases)

**Interfaces:**
- Consumes: `CurveSurface` (`include/atx/vol/vol_curve.hpp`) — `n_slices()`, `slices()` → `std::span<const std::unique_ptr<IVolCurve>>`, each `IVolCurve::w(double k_log)`, `T()`. `ArbViolation` (`arb.hpp:98`, fields `k_log,T1,T2,slack,kind`; `Kind::Calendar`).
- Produces: `Result<std::vector<ArbViolation>> arb_check_calendar(const CurveSurface& s, double k_min, double k_max, std::uint32_t n_grid)`.

- [ ] **Step 1: Write the failing test**

Add to `tests/arb_test.cpp` (it already includes GoogleTest + `atx/vol/arb.hpp`; add `#include "atx/vol/vol_curve.hpp"` and `#include "atx/vol/dense_slice.hpp"` at the top if absent). Build two dense slices whose total variance crosses.

```cpp
// Helper: a trivial constant-vol convex slice at (T,F) — flat smile sigma.
static atx::vol::ConvexSliceFit flat_slice(double T, double F, double df, double sigma) {
  using namespace atx::vol;
  ConvexSliceFit s;
  s.T = T; s.F = F; s.df = df;
  // 5 strikes around F; European call prices at flat sigma → convex/arb-free.
  for (int i = -2; i <= 2; ++i) {
    const double K = F * std::exp(0.05 * i);
    s.u.push_back(K);
    s.C.push_back(black76_price(F, K, T, sigma, df, Side::Call));
  }
  return s;
}

TEST(ArbCheckCalendarCurveSurface, FlagsCrossing) {
  using namespace atx::vol;
  CurveSurface surf;
  // T1=0.25 with HIGH vol, T2=0.50 with LOW vol → w(k,T2) < w(k,T1): calendar arb.
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.25, 100.0, 1.0, 0.40)));
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.50, 100.0, 1.0, 0.20)));
  auto v = arb_check_calendar(surf, -0.2, 0.2, 21);
  ASSERT_TRUE(v.has_value());
  EXPECT_FALSE(v->empty());
  EXPECT_EQ(v->front().kind, ArbViolation::Kind::Calendar);
}

TEST(ArbCheckCalendarCurveSurface, CleanStackNoViolation) {
  using namespace atx::vol;
  CurveSurface surf;
  // Monotone total variance: same sigma → w = sigma^2 * T is increasing in T.
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.25, 100.0, 1.0, 0.25)));
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.50, 100.0, 1.0, 0.25)));
  auto v = arb_check_calendar(surf, -0.2, 0.2, 21);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(v->empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-vol-tests -j`
Expected: FAIL — compile error, no `arb_check_calendar(const CurveSurface&, ...)` overload.

- [ ] **Step 3: Add the declaration**

In `include/atx/vol/arb.hpp`: add `#include "atx/vol/vol_curve.hpp"` near the other includes, and after the `VolSurface` calendar declaration (~line 115) add:

```cpp
// Calendar check for a polymorphic CurveSurface (ConvexDense/SVI served path).
// Sample n_grid equispaced log-moneyness points in [k_min,k_max]; record a
// Calendar violation wherever total variance DECREASES across a consecutive
// (shorter-T, longer-T) slice pair, w_prev(k) - w_curr(k) > kCalendarTol.
// Empty result => calendar-arb-free. No-op (empty) for < 2 slices or n_grid==0.
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_calendar(const CurveSurface &s, double k_min, double k_max,
                   std::uint32_t n_grid);
```

- [ ] **Step 4: Implement it**

In `src/arb.cpp` (add `#include "atx/vol/vol_curve.hpp"` if absent):

```cpp
Result<std::vector<ArbViolation>>
arb_check_calendar(const CurveSurface &s, double k_min, double k_max,
                   std::uint32_t n_grid) {
  std::vector<ArbViolation> out;
  const auto slices = s.slices();
  if (slices.size() < 2 || n_grid == 0 || !(k_max > k_min)) {
    return Ok(std::move(out));
  }
  constexpr double kCalendarTol = 1.0e-7;  // total-variance units
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  for (std::size_t i = 1; i < slices.size(); ++i) {
    const IVolCurve &prev = *slices[i - 1];
    const IVolCurve &curr = *slices[i];
    for (std::uint32_t g = 0; g <= n_grid; ++g) {
      const double k = k_min + dk * static_cast<double>(g);
      const double wp = prev.w(k);
      const double wc = curr.w(k);
      if (!std::isfinite(wp) || !std::isfinite(wc)) {
        continue;  // wing coverage gap on one side — nothing to compare
      }
      const double slack = wp - wc;
      if (slack > kCalendarTol) {
        out.push_back(ArbViolation{k, prev.T(), curr.T(), slack,
                                   ArbViolation::Kind::Calendar});
      }
    }
  }
  return Ok(std::move(out));
}
```

Ensure `<cmath>` is included for `std::isfinite`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build --target atx-vol-tests -j && ./build/.../atx-vol-tests --gtest_filter='ArbCheckCalendarCurveSurface.*'`
Expected: PASS (both cases).

- [ ] **Step 6: Commit**

```bash
git add include/atx/vol/arb.hpp src/arb.cpp tests/arb_test.cpp
git commit -m "atx-vol: calendar-arb checker for the CurveSurface (dense/SVI served path)

The only calendar validator accepted a VolSurface (eSSVI); the served dense
CurveSurface was never checkable. Add an arb_check_calendar overload sampling
w(k) across consecutive slices. Unit-tested on a hand-planted crossing + a
monotone stack.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Honest calendar reporting in `session.cpp`

**Files:**
- Modify: `src/session.cpp:277-308`
- Test: `tests/spy_real_test.cpp` (assert the field is computed, not hardcoded)

**Interfaces:**
- Consumes: `arb_check_calendar(const CurveSurface&, ...)` (Task 1); `crep.surface` (the fitted `CurveSurface`), `crep.context` (per-slice `T`, `forward`).
- Produces: `SessionDiagnostics.calendar_arb_free` / `n_calendar_viol_pre` populated from the measured result on the dense/SVI path.

- [ ] **Step 1: Write the failing test**

In `tests/spy_real_test.cpp` (which already fits the real SPY board through the session — find the existing dense/ConvexDense session fixture and add an assertion). If the suite builds a `VolaSession` on the SPY board with a ConvexDense config, add:

```cpp
// The dense/SVI served surface must now REPORT a measured calendar status
// (previously hardcoded false). Baseline: record the pre-enforcement count.
TEST(SpyRealCalendarReporting, DenseSurfaceReportsMeasuredCalendar) {
  // ... reuse the existing SPY session build in this file to get `session` ...
  const SessionDiagnostics& d = session.diagnostics();  // use the real accessor
  // Pre-enforcement this may be >0; the assertion is only that it is COMPUTED:
  // n_calendar_viol_pre is 0 IFF calendar_arb_free is true (they agree).
  EXPECT_EQ(d.calendar_arb_free, d.n_calendar_viol_pre == 0u);
}
```

(Adjust to the file's real session accessor names — mirror an existing test in `spy_real_test.cpp`.)

- [ ] **Step 2: Run test to verify it fails**

Run: build + `--gtest_filter='SpyRealCalendarReporting.*'`
Expected: FAIL — with the hardcoded `calendar_arb_free=false` and `n_calendar_viol_pre=0`, the identity `false == (0==0)` is `false == true` → fails.

- [ ] **Step 3: Replace the hardcoded stamp**

In `src/session.cpp`, replace lines 277-281:

```cpp
    // Calendar no-arb across slices is not (yet) checked for the dense/SVI
    // override; report it unverified rather than asserting an unproven property.
    // (Each convex slice is butterfly-arb-free by construction of the QP.)
    cdiag.calendar_arb_free = false;
    cdiag.n_calendar_viol_pre = 0;
```

with a real measurement over the fitted surface's own k-range:

```cpp
    // Calendar no-arb across slices, measured on the served CurveSurface. Each
    // convex slice is butterfly-arb-free by construction; this is the missing
    // half. k-range spans a wide moneyness band around the money.
    {
      constexpr double kBand = 0.60;   // log-moneyness half-width to sample
      constexpr std::uint32_t kGrid = 64;
      const auto cal = arb_check_calendar(crep.surface, -kBand, kBand, kGrid);
      const std::size_t n_viol = cal ? cal->size() : 0;
      cdiag.calendar_arb_free = (n_viol == 0);
      cdiag.n_calendar_viol_pre = static_cast<std::uint32_t>(n_viol);
    }
```

Add `#include "atx/vol/arb.hpp"` to `src/session.cpp` if not already present.

- [ ] **Step 4: Run the test + full suite**

Run: build + `--gtest_filter='SpyRealCalendarReporting.*'` → PASS. Then run the full `atx-vol-tests` to confirm no regression from the new include/measurement.

- [ ] **Step 5: Baseline measurement (record, not a test)**

Rebuild the SPY corpus example and note the reported `calendar_arb_free` / count in the run output (this quantifies the hole before enforcement). Record the number in the Task 5 commit message. (No code change.)

- [ ] **Step 6: Commit**

```bash
git add src/session.cpp tests/spy_real_test.cpp
git commit -m "atx-vol: report measured calendar-arb status on the dense/SVI surface

session.cpp hardcoded calendar_arb_free=false because the CurveSurface was not
checkable; now measure it via arb_check_calendar over the served surface's
moneyness band. Honest reporting, independent of enforcement.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Augment the dense QP to `Gx ≥ h`

**Files:**
- Modify: `src/dense_slice.cpp:40-130` (`qp_active_set` signature + feasibility/ratio test), `:351` (call site), `:305-335` (constraint assembly to pass an `h`)
- Modify: `include/atx/vol/dense_slice.hpp` (`ConvexFitOpts` — keep `bound_slope_below`, now honored)
- Test: `tests/dense_slice_test.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: internal `qp_active_set(const MatX& H, const VecX& q, const MatX& G, const VecX& h, VecX x, int max_iter)` — solves `min ½xᵀHx+qᵀx s.t. Gx ≥ h`. `h=0` ≡ prior behavior. `fit_convex_slice` now honors `opts.bound_slope_below` (adds `∂C/∂K ≥ −df` rows).

- [ ] **Step 1: Write the failing test (h=0 equivalence + a non-homogeneous bound)**

In `tests/dense_slice_test.cpp` add a test that the slope-below bound is now respected (it was silently ignored before). Fit a slice with `bound_slope_below=true` and assert the node-price differences satisfy `C[j+1]-C[j] ≥ -df*(u[j+1]-u[j]) - tol`:

```cpp
TEST(ConvexSliceFit, SlopeBelowBoundHonored) {
  using namespace atx::vol;
  // Build a simple in-the-money-heavy obs set where the unconstrained slope could
  // dip below -df; enable the bound and assert it holds at every node pair.
  std::vector<FitObs> obs = make_synthetic_slice_obs(/*F=*/100.0, /*T=*/0.5,
                                                     /*df=*/0.98, /*sigma=*/0.2);
  ConvexFitOpts opts; opts.bound_slope_below = true;
  auto fit = fit_convex_slice(obs, 100.0, 0.5, 0.98, opts);
  ASSERT_TRUE(fit.has_value());
  const double df = 0.98;
  for (std::size_t j = 0; j + 1 < fit->u.size(); ++j) {
    const double slope = (fit->C[j + 1] - fit->C[j]) / (fit->u[j + 1] - fit->u[j]);
    EXPECT_GE(slope, -df - 1e-7);
  }
}
```

Also add an explicit equivalence guard (bit-identical `h=0` path) by fitting the SAME obs with default opts before and after — since the change is in-place, rely on the existing `dense_slice_test` cases continuing to pass as the equivalence guard (they exercise `h=0`).

(`make_synthetic_slice_obs` — if no such helper exists in the file, write a small local one that builds `FitObs{K,mid,spread,vega,side}` from Black-76 call prices at a flat sigma across ~11 strikes.)

- [ ] **Step 2: Run test to verify it fails**

Run: build + `--gtest_filter='ConvexSliceFit.SlopeBelowBoundHonored'`
Expected: FAIL — `bound_slope_below` currently deferred (`dense_slice.cpp:328-330`), so the bound is not enforced.

- [ ] **Step 3: Augment `qp_active_set`**

Change the signature and the two RHS-dependent spots. In `src/dense_slice.cpp`:

Signature (line 40):
```cpp
[[nodiscard]] atx::core::Result<VecX> qp_active_set(const MatX& H, const VecX& q,
                                                    const MatX& G, const VecX& h,
                                                    VecX x, int max_iter) {
```
The KKT step block is unchanged (working-set equality keeps `G_W p = 0`). Only the feasibility test (line 86-104 region is the optimality check — unchanged) and the ratio test change. Replace the ratio-test body (lines ~109-120) with:
```cpp
    for (Eigen::Index i = 0; i < nc; ++i) {
      if (in_w[static_cast<std::size_t>(i)]) {
        continue;
      }
      const double gip = G.row(i).dot(p);
      if (gip < -1.0e-14) {
        const double gix = G.row(i).dot(x) - h(i);   // residual to the RHS
        const double ai = -gix / gip;                 // >= 0 since gix >= 0
        if (ai < alpha) {
          alpha = ai;
          block = i;
        }
      }
    }
```
No other change: the working-set solve already targets `G_W p = 0`, and multipliers/optimality are RHS-independent.

- [ ] **Step 4: Build the `h` vector at the call site + add the slope-below rows**

In `fit_convex_slice`, where constraints are assembled (`:305-335`), accumulate a parallel `h` value per row (0 for the homogeneous positivity/monotone/convex rows). After the convexity rows, if `opts.bound_slope_below`, add rows encoding `C_{j+1}-C_j ≥ -df·(u_{j+1}-u_j)` i.e. `g_j - g_{j+1} ≤ df·Δ` ⇔ `(g_{j+1} - g_j) ≥ -df·Δ`:

```cpp
  std::vector<VecX> rows;
  std::vector<double> hrows;   // parallel RHS, one per row
  // ... for each existing positivity/monotone/convexity row: rows.push_back(r); hrows.push_back(0.0);

  if (opts.bound_slope_below) {
    for (Eigen::Index i = 0; i + 1 < N; ++i) {
      VecX rrow = VecX::Zero(N);
      rrow(i + 1) = 1.0;
      rrow(i) = -1.0;                       // (g_{i+1} - g_i) >= -df*(u_{i+1}-u_i)
      rows.push_back(rrow);
      hrows.push_back(-df * (un(i + 1) - un(i)));
    }
  }
```
Then build `VecX h(nc)` from `hrows` alongside `MatX G(nc, N)`, and update the call:
```cpp
  VecX h(nc);
  for (Eigen::Index i = 0; i < nc; ++i) { h(i) = hrows[static_cast<std::size_t>(i)]; }
  ATX_TRY(VecX gn, qp_active_set(H, q, G, h, x0, opts.max_iter));
```
The existing `x0` (strictly-decreasing convex quadratic, positive) satisfies the slope-below rows for reasonable `df` (a decreasing quadratic has bounded slope); if a degenerate obs set makes `x0` infeasible, the active-set self-corrects via the KKT-drop fallback already present. Update the `dense_slice.cpp:328-330` NOTE comment to say the slope-below bound is now implemented via the augmented form.

- [ ] **Step 5: Run the test + full dense suite**

Run: build + `--gtest_filter='ConvexSliceFit.*'`
Expected: PASS — new bound honored; all pre-existing `ConvexSliceFit`/dense cases still pass (h=0 equivalence).

- [ ] **Step 6: Commit**

```bash
git add src/dense_slice.cpp include/atx/vol/dense_slice.hpp tests/dense_slice_test.cpp
git commit -m "atx-vol: augment dense QP to Gx>=h; implement slope-below bound

qp_active_set was homogeneous (Gx>=0); the ratio test now subtracts a per-row
RHS h. h=0 recovers the prior solver bit-for-bit (existing dense tests are the
equivalence guard). The non-homogeneous form also lets fit_convex_slice honor
the previously-deferred bound_slope_below constraint (dC/dK >= -df).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Per-node calendar floor in `fit_convex_slice`

**Files:**
- Modify: `include/atx/vol/dense_slice.hpp` (add a `w_prev` callback param to `fit_convex_slice`)
- Modify: `src/dense_slice.cpp` (compute per-node floor rows from `w_prev`)
- Test: `tests/dense_slice_test.cpp`

**Interfaces:**
- Consumes: augmented `qp_active_set` (Task 3); `black76_price` (`black76.hpp`).
- Produces: `fit_convex_slice(obs, F, T, df, opts, const std::function<double(double)>& w_prev = {})` — when `w_prev` is set, adds rows `g_j ≥ C_floor_j` where `C_floor_j = black76_price(F, u_j, T, sqrt(w_prev(k_j)/T), df, Call)`, `k_j=ln(u_j/F)`, only for nodes where `w_prev(k_j)` is finite and > 0.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(ConvexSliceFit, CalendarFloorLiftsLowVarianceSlice) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  // Obs imply LOW vol (0.15); floor demands total variance of a 0.25-vol prev
  // slice at the SAME T. Floor must lift w above the unconstrained fit.
  std::vector<FitObs> obs = make_synthetic_slice_obs(F, T, df, 0.15);
  auto w_prev = [&](double k) {
    const double sig = 0.25;
    return sig * sig * T;   // flat prev total variance
  };
  auto free_fit = fit_convex_slice(obs, F, T, df, {});
  auto floored  = fit_convex_slice(obs, F, T, df, {}, w_prev);
  ASSERT_TRUE(free_fit && floored);
  // At the money, floored total variance >= prev (minus tol), and >= free fit.
  const double w_floor = 0.25 * 0.25 * T;
  const double s_atm = floored->iv(0.0);
  EXPECT_GE(s_atm * s_atm * T, w_floor - 1e-6);
  EXPECT_GE(floored->iv(0.0), free_fit->iv(0.0) - 1e-9);
}

TEST(ConvexSliceFit, CalendarFloorSlackIsBitIdentical) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  std::vector<FitObs> obs = make_synthetic_slice_obs(F, T, df, 0.30);
  auto w_prev = [&](double) { return 0.10 * 0.10 * T; };  // prev far BELOW → slack
  auto free_fit = fit_convex_slice(obs, F, T, df, {});
  auto floored  = fit_convex_slice(obs, F, T, df, {}, w_prev);
  ASSERT_TRUE(free_fit && floored);
  ASSERT_EQ(free_fit->C.size(), floored->C.size());
  for (std::size_t j = 0; j < free_fit->C.size(); ++j) {
    EXPECT_NEAR(free_fit->C[j], floored->C[j], 1e-12);  // slack ⇒ identical
  }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: build + `--gtest_filter='ConvexSliceFit.CalendarFloor*'`
Expected: FAIL — `fit_convex_slice` has no `w_prev` parameter (compile error).

- [ ] **Step 3: Add the parameter + floor rows**

In `include/atx/vol/dense_slice.hpp`, add `#include <functional>` and change the declaration:
```cpp
[[nodiscard]] Result<ConvexSliceFit> fit_convex_slice(
    std::span<const FitObs> obs, double F, double T, double df,
    const ConvexFitOpts& opts = {},
    const std::function<double(double)>& w_prev = {});
```

In `src/dense_slice.cpp`, add `#include <functional>` and `#include "atx/vol/black76.hpp"` (already present). After the node grid `un` is built and BEFORE assembling `G/h`, compute the floor:
```cpp
  std::vector<double> cfloor(static_cast<std::size_t>(N), 0.0);
  std::vector<char> has_floor(static_cast<std::size_t>(N), 0);
  if (w_prev) {
    for (Eigen::Index j = 0; j < N; ++j) {
      const double k = std::log(un(j) / F);
      const double wp = w_prev(k);
      if (std::isfinite(wp) && wp > 0.0) {
        const double sig = std::sqrt(wp / T);
        const double c = black76_price(F, un(j), T, sig, df, Side::Call);
        if (std::isfinite(c) && c > 0.0) {
          cfloor[static_cast<std::size_t>(j)] = c;
          has_floor[static_cast<std::size_t>(j)] = 1;
        }
      }
    }
  }
```
Add the calendar rows during constraint assembly (diagonal `g_j ≥ cfloor_j`):
```cpp
  for (Eigen::Index j = 0; j < N; ++j) {
    if (has_floor[static_cast<std::size_t>(j)]) {
      VecX rrow = VecX::Zero(N);
      rrow(j) = 1.0;
      rows.push_back(rrow);
      hrows.push_back(cfloor[static_cast<std::size_t>(j)]);
    }
  }
```
Make the strictly-feasible start respect the floor. Replace the `x0(j)` assignment (`:346-349`) with:
```cpp
  for (Eigen::Index j = 0; j < N; ++j) {
    const double t = (span > 0.0) ? (un(N - 1) - un(j)) / span : 0.0;
    double v = floor + cmax * t * t;
    if (has_floor[static_cast<std::size_t>(j)]) {
      // max with the (convex, decreasing, positive) floor curve + strict margin.
      v = std::max(v, cfloor[static_cast<std::size_t>(j)] * (1.0 + 1.0e-6) + 1.0e-9);
    }
    x0(j) = v;
  }
```
(Pointwise max of two convex-decreasing-positive node sequences stays convex/decreasing/positive; the margin makes the calendar rows strictly satisfied. If a rare degenerate start trips the KKT solve, the existing linear-dependence drop fallback recovers.)

- [ ] **Step 4: Run the tests**

Run: build + `--gtest_filter='ConvexSliceFit.CalendarFloor*'` → PASS (lift case lifts; slack case bit-identical). Then run all `ConvexSliceFit.*`.

- [ ] **Step 5: Commit**

```bash
git add include/atx/vol/dense_slice.hpp src/dense_slice.cpp tests/dense_slice_test.cpp
git commit -m "atx-vol: per-node calendar floor in the dense convex-slice fit

fit_convex_slice takes an optional w_prev(k) callback; where set it adds
g_j >= black76_call(F,u_j,T,sqrt(w_prev(k_j)/T),df) rows so the slice's total
variance cannot dip below the previous expiry's at the nodes. Slack (no
calendar arb) ⇒ bit-identical node prices; a real crossing is lifted minimally.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Sequential calendar-enforcing driver in `fit_curve_surface`

**Files:**
- Modify: `include/atx/vol/vol_curve.hpp` / `src/vol_curve.cpp` (thread `w_prev` through `fit_slice_curve`)
- Modify: `src/curve_fit.cpp:99-171` (pass the previous fitted slice's `w(·)`)
- Test: `tests/spy_real_test.cpp` (or new `tests/curve_noarb_test.cpp`)

**Interfaces:**
- Consumes: `fit_convex_slice(..., w_prev)` (Task 4); `arb_check_calendar(CurveSurface)` (Task 1); the last-pushed slice via `out.surface.slices().back()->w(k)`.
- Produces: a `fit_curve_surface` result whose `CurveSurface` is calendar-arb-free by construction for `ConvexDense`.

- [ ] **Step 1: Write the failing test (by-construction gate)**

New `tests/curve_noarb_test.cpp` — fit the real SPY board (reuse the loader/fixture used by `spy_real_test.cpp`; copy its board-construction helper) through `fit_curve_surface` with a ConvexDense config, then assert zero calendar violations:

```cpp
TEST(CurveSurfaceNoArb, SpyDenseIsCalendarArbFree) {
  using namespace atx::vol;
  Underlying under = load_spy_real_board();          // mirror spy_real_test fixture
  SurfaceParityInputs in = spy_real_inputs();        // S, r, deam, calib, band_k
  CurveConfig cfg;  // default = ConvexDense, node_cap 40
  auto rep = fit_curve_surface(under, in, cfg);
  ASSERT_TRUE(rep.has_value());
  auto viol = arb_check_calendar(rep->surface, -0.6, 0.6, 64);
  ASSERT_TRUE(viol.has_value());
  EXPECT_TRUE(viol->empty()) << "calendar violations: " << viol->size();
}
```

- [ ] **Step 2: Run test to verify it fails (or record baseline)**

Run: build + `--gtest_filter='CurveSurfaceNoArb.*'`
Expected: FAIL if the independent fit has any calendar crossing (records the baseline count). If it already passes (no crossings on this board), keep the test as the regression guard and note the baseline was 0 — enforcement still needed for other boards/dates.

- [ ] **Step 3: Thread `w_prev` through `fit_slice_curve`**

In `include/atx/vol/vol_curve.hpp`, add `#include <functional>` and extend the dispatch:
```cpp
[[nodiscard]] Result<std::unique_ptr<IVolCurve>> fit_slice_curve(
    const CurveConfig& cfg, std::span<const FitObs> obs_eu, double F, double T,
    double df, const std::function<double(double)>& w_prev = {});
```
In `src/vol_curve.cpp`, forward it in the `ConvexDense` branch:
```cpp
    case VolCurveKind::ConvexDense: {
      ATX_TRY(ConvexSliceFit fit,
              fit_convex_slice(obs_eu, F, T, df, cfg.convex, w_prev));
      ...
    }
```
(Essvi/Svi branches ignore `w_prev` — their calendar handling stays as-is; document that in a comment.)

- [ ] **Step 4: Sequential driver**

In `src/curve_fit.cpp`, add `#include <functional>`. In the fit loop, capture the previously-pushed slice and pass its `w` as the floor. Replace the `fit_slice_curve` call (`:129`) with:
```cpp
    // Calendar floor: the previous fitted slice's total variance (ascending T).
    std::function<double(double)> w_prev;
    if (!out.surface.empty()) {
      const IVolCurve* prev = out.surface.slices().back().get();
      w_prev = [prev](double k) { return prev->w(k); };
    }
    auto slice_res = fit_slice_curve(cfg, obs->obs, F, T, df, w_prev);
```
Because slices are pushed in ascending T (the loop order + `push` at `:163`), `slices().back()` is always the immediately-shorter expiry. The floor is slack where no arb exists (bit-identical), and lifts otherwise.

- [ ] **Step 5: Run the gate + clean-board regression + full suite**

Run: build + `--gtest_filter='CurveSurfaceNoArb.*'` → PASS (0 calendar violations). Then run `spy_real_test`/`spy_bidask_regression_test` — the in-band metric must not degrade (enforcement is slack where the board had no crossing). Then the full `atx-vol-tests`.

- [ ] **Step 6: Register the new test + commit**

Add `curve_noarb_test.cpp` to `tests/CMakeLists.txt` (in the `add_executable(atx-vol-tests ...)` list).

```bash
git add tests/CMakeLists.txt tests/curve_noarb_test.cpp include/atx/vol/vol_curve.hpp src/vol_curve.cpp src/curve_fit.cpp
git commit -m "atx-vol: enforce calendar no-arb by construction on the dense surface

fit_curve_surface fits slices ascending-T and feeds each fitted slice's w(k)
as the next slice's calendar floor, so the served CurveSurface is
calendar-arb-free by construction (like butterfly). Slack where no arb ⇒
bit-identical; SPY dense board now reports 0 calendar violations. Baseline
pre-enforcement count on the corpus: <N> (record from Task 2 run).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Lee wing extrapolation

**Files:**
- Modify: `include/atx/vol/dense_slice.hpp` (doc), `src/dense_slice.cpp` (`ConvexSliceFit::call_price` / `iv`)
- Test: `tests/dense_slice_test.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `ConvexSliceFit::iv(k)` returns a finite, monotone-in-|k| total variance beyond the node range, with butterfly-clean tails.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(ConvexSliceFit, LeeWingsFiniteAndMonotone) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  auto fit = fit_convex_slice(make_synthetic_slice_obs(F, T, df, 0.20), F, T, df, {});
  ASSERT_TRUE(fit.has_value());
  const double k_edge = std::log(fit->u.back() / F);
  // Far beyond the last node the IV must be finite (not a flat-price collapse)...
  const double s_far = fit->iv(k_edge + 0.8);
  EXPECT_TRUE(std::isfinite(s_far) && s_far > 0.0);
  // ...and total variance must be non-decreasing outward.
  const double w1 = fit->iv(k_edge + 0.4); const double w1t = w1 * w1 * T;
  const double w2 = fit->iv(k_edge + 0.8); const double w2t = w2 * w2 * T;
  EXPECT_GE(w2t, w1t - 1e-9);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: build + `--gtest_filter='ConvexSliceFit.LeeWings*'`
Expected: FAIL — beyond the last node `call_price` is flat-clamped, so total variance flattens (or IV inversion fails), violating monotone-outward and possibly returning NaN.

- [ ] **Step 3: Implement the wing tail**

Replace the flat clamp in `ConvexSliceFit::call_price` / add a wing branch in `iv`. The cleanest place is `iv(k_log)` (used by the curve): compute total variance directly with a linear-in-k tail beyond the node range, matching the edge slope, capped at the Lee no-arb slope.

Add a private helper and edit `iv`:
```cpp
// Total variance at log-moneyness k, with linear-in-k Lee tails beyond nodes.
double ConvexSliceFit::w_of_k(double k_log) const noexcept {
  if (u.empty() || !(F > 0.0) || !(T > 0.0)) return kNaN;
  const double k_lo = std::log(u.front() / F);
  const double k_hi = std::log(u.back()  / F);
  if (k_log >= k_lo && k_log <= k_hi) {
    const double c = call_price(F * std::exp(k_log));
    if (!std::isfinite(c) || !(c > 0.0)) return kNaN;
    const auto s = implied_vol(c, F, F * std::exp(k_log), T, df, Side::Call);
    return s.has_value() ? (*s) * (*s) * T : kNaN;
  }
  // Edge slope of w in k from the two outermost usable log-moneyness points.
  auto w_at = [&](double k) -> double {
    const double c = call_price(F * std::exp(k));
    const auto s = implied_vol(c, F, F * std::exp(k), T, df, Side::Call);
    return s.has_value() ? (*s) * (*s) * T : kNaN;
  };
  const double kLeeCap = 2.0;   // Roper/Lee large-strike slope bound on w in k
  if (k_log > k_hi) {
    const double k1 = k_hi, k0 = std::log(u[u.size() - 2] / F);
    const double w1 = w_at(k1), w0 = w_at(k0);
    if (!std::isfinite(w1) || !std::isfinite(w0)) return w1;
    double slope = (w1 - w0) / (k1 - k0);
    slope = std::clamp(slope, 0.0, kLeeCap);
    return w1 + slope * (k_log - k1);
  }
  // k_log < k_lo
  const double k1 = k_lo, k0 = std::log(u[1] / F);
  const double w1 = w_at(k1), w0 = w_at(k0);
  if (!std::isfinite(w1) || !std::isfinite(w0)) return w1;
  double slope = (w0 - w1) / (k0 - k1);            // dw/dk near the left edge
  slope = std::clamp(slope, -kLeeCap, 0.0);
  return w1 + slope * (k_log - k1);
}

double ConvexSliceFit::iv(double k_log) const noexcept {
  const double w = w_of_k(k_log);
  if (!std::isfinite(w) || !(w > 0.0) || !(T > 0.0)) return kNaN;
  return std::sqrt(w / T);
}
```
Declare `double w_of_k(double) const noexcept;` in `dense_slice.hpp` (`ConvexSliceFit`). Include `<algorithm>` for `std::clamp` (already included). Leave `call_price` as-is for the in-range convex interpolation (parity scoring uses quoted strikes, unaffected).

- [ ] **Step 4: Wing butterfly-clean check**

Add to the same test: sample the tail region and assert no butterfly violation via the density sign using finite differences of `w_of_k` (or reuse `arb_check_butterfly` if a `ConvexDenseCurve` can be wrapped in a `CurveSurface` and sampled past the nodes):
```cpp
  // Central-difference density proxy g(k) = (1 - k w'/(2w))^2 - (w'/2)^2(1/4+1/w) + w''/2
  auto wk = [&](double k){ double s=fit->iv(k); return s*s*T; };
  const double k = k_edge + 0.5, hh = 1e-3;
  const double w = wk(k), wp = (wk(k+hh)-wk(k-hh))/(2*hh),
               wpp = (wk(k+hh)-2*w+wk(k-hh))/(hh*hh);
  const double g = std::pow(1 - k*wp/(2*w), 2) - (wp*wp/4.0)*(0.25 + 1.0/w) + wpp/2.0;
  EXPECT_GE(g, -1e-6);
```

- [ ] **Step 5: Run tests + full suite**

Run: build + `--gtest_filter='ConvexSliceFit.LeeWings*'` → PASS. Then full `ConvexSliceFit.*` + `spy_real_test` (in-range parity unchanged) + full suite.

- [ ] **Step 6: Commit**

```bash
git add include/atx/vol/dense_slice.hpp src/dense_slice.cpp tests/dense_slice_test.cpp
git commit -m "atx-vol: Lee wing extrapolation on the dense slice

Beyond the node range the convex fit flat-clamped the call price, so far-wing
total variance flattened and IV degenerated. Extrapolate total variance
linearly in log-moneyness with the edge slope, capped at the Roper/Lee
large-strike no-arb slope. Finite, monotone-outward, butterfly-clean tails;
the in-range convex interpolation and parity scoring are unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: Band-aware (interval) objective behind a flag

**Files:**
- Modify: `include/atx/vol/dense_slice.hpp` (`ConvexFitOpts` — add `CalibLossKind loss{CalibLossKind::Mid}`)
- Modify: `src/dense_slice.cpp` (interval-loss slack QP branch)
- Test: `tests/dense_slice_test.cpp`

**Interfaces:**
- Consumes: `CalibLossKind` (`calib.hpp`), augmented QP, `FitObs.spread`.
- Produces: `fit_convex_slice` with `opts.loss == CalibLossKind::Interval` fits the price into `[mid-spread/2, mid+spread/2]` (call-folded), residual zero inside the band. Default `Mid` unchanged.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(ConvexSliceFit, IntervalLossPutsPriceInsideBand) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  // Wide bands → many mid-only fits sit outside band; interval should pull inside.
  std::vector<FitObs> obs = make_synthetic_slice_obs_wideband(F, T, df, 0.20);
  ConvexFitOpts opts; opts.loss = CalibLossKind::Interval;
  auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value());
  for (const auto& o : obs) {
    const double call = (o.side == Side::Call) ? o.mid : o.mid + df*(F - o.K);
    const double c = fit->call_price(o.K);
    EXPECT_GE(c, call - o.spread/2 - 1e-6);
    EXPECT_LE(c, call + o.spread/2 + 1e-6);
  }
}

TEST(ConvexSliceFit, IntervalDegenerateBandEqualsMid) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  auto obs = make_synthetic_slice_obs(F, T, df, 0.20);
  for (auto& o : obs) o.spread = 0.0;             // zero-width band == mid target
  ConvexFitOpts mid; auto a = fit_convex_slice(obs, F, T, df, mid);
  ConvexFitOpts iv; iv.loss = CalibLossKind::Interval;
  auto b = fit_convex_slice(obs, F, T, df, iv);
  ASSERT_TRUE(a && b);
  for (std::size_t j = 0; j < a->C.size(); ++j) EXPECT_NEAR(a->C[j], b->C[j], 1e-7);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: build + `--gtest_filter='ConvexSliceFit.Interval*'`
Expected: FAIL — `ConvexFitOpts` has no `loss` field (compile error).

- [ ] **Step 3: Implement the interval branch**

Add to `ConvexFitOpts` (`dense_slice.hpp`): `#include "atx/vol/calib.hpp"` already present → `CalibLossKind loss{CalibLossKind::Mid};`

In `src/dense_slice.cpp`, when `opts.loss == CalibLossKind::Interval`, replace the `min Σ w_i (Bg - co)²` data term with a slack formulation. Extend the variable vector to `z = [g (N); s⁺ (M); s⁻ (M)]` (size `N+2M`):
- Objective `H`: roughness third-difference block on the `g` sub-block (as today); diagonal `2 w_i` on each slack; `q = 0`.
- Constraints (all `Gz ≥ h`):
  - homogeneous cone + calendar floor rows on the `g` sub-block (pad slack columns with 0);
  - band-upper: `−(Bg)_i + s⁺_i ≥ −c_ask_i` (i.e. `(Bg)_i − c_ask_i ≤ s⁺_i`), RHS `−c_ask_i`;
  - band-lower: `(Bg)_i + s⁻_i ≥ c_bid_i`, RHS `c_bid_i`;
  - `s⁺_i ≥ 0`, `s⁻_i ≥ 0`, RHS 0.
- `c_ask_i = co_i + spread_i/2`, `c_bid_i = max(0, co_i − spread_i/2)` (call-folded; `co`,`spread` already per merged obs).
- Feasible start: `g` = the Mid-path `x0`; `s⁺_i = max(0,(Bx0)_i−c_ask_i)+ε`, `s⁻_i = max(0,c_bid_i−(Bx0)_i)+ε`.

Keep the `Mid` branch exactly as today (guard the whole slack machinery behind `if (opts.loss == CalibLossKind::Interval)`), so the default path is byte-identical. Extract the shared node-grid/`B`/cone assembly so both branches reuse it.

- [ ] **Step 4: Run tests + full suite**

Run: build + `--gtest_filter='ConvexSliceFit.Interval*'` → PASS. Full `ConvexSliceFit.*` (Mid path unchanged) + full suite.

- [ ] **Step 5: In-band measurement (record, not a test)**

Add a temporary example/log run (or extend `spy_bidask_regression`'s harness locally) to compute `mean_frac_within_bidask` on the SPY corpus with `loss=Interval` vs `Mid`. Record both numbers in the commit message. Do NOT flip the default in this task — leave `Mid` default; a follow-up flips it only if the number improves.

- [ ] **Step 6: Commit**

```bash
git add include/atx/vol/dense_slice.hpp src/dense_slice.cpp tests/dense_slice_test.cpp
git commit -m "atx-vol: exact interval (band) loss for the dense fit (flag, default Mid)

Optional CalibLossKind::Interval objective via slack variables: residual zero
inside [bid,ask], quadratic outside — the true interval loss, still a convex QP.
Default stays Mid (production fits unchanged). SPY corpus mean-in-band: Mid
<x>%, Interval <y>% (follow-up flips the default iff Interval wins OOS).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: End-to-end verification (corpus rebuild + backtest sanity + full suite)

**Files:** none (verification), except possibly a one-line diagnostic print.

- [ ] **Step 1: Full test suite**

Run the entire `atx-vol-tests` binary (all ~676+ cases incl. the new no-arb tests).
Expected: 100% pass.

- [ ] **Step 2: Rebuild the SPY YTD corpus**

Run the `spy_ytd_corpus` example against the existing OPRA hive (`data/spy_ytd/opra` → `data/spy_ytd/archives`). Expected: `n_ok` unchanged (123/123), build time not materially regressed vs the 140s baseline (sequential per-board calendar fit adds a small cost; confirm). Note the wall-clock.

- [ ] **Step 3: Backtest sanity**

Run `spy_strangle_backtest --manifest data/spy_ytd/archives/manifest.tsv`. Expected: `total_return` within a small tolerance of the pre-change −167,599.96 (enforcement is slack on the 6M strangle region, so it should be ≈ identical; any change is explained by a genuine calendar lift on a fitted board). Record the number.

- [ ] **Step 4: Confirm calendar-arb-free end to end**

Confirm the corpus/session now reports `calendar_arb_free=true` on the SPY boards (from Task 2/5 wiring).

- [ ] **Step 5: Final commit (if any diagnostic left) + summary**

If a diagnostic print was added, revert or commit it deliberately. Summarize the run numbers (suite pass count, corpus build time, backtest total, calendar status) in the handoff.

---

## Self-review

**Spec coverage:**
- ① calendar checker → Task 1; honest session reporting → Task 2. ✓
- ②a augmented QP + slope-below → Task 3; ②b calendar floor → Task 4; ②c sequential driver + by-construction gate → Task 5. ✓
- ③ Lee wings → Task 6. ✓
- ④ interval objective (flag, default Mid) + in-band measurement → Task 7. ✓
- Test gate (calendar-free, butterfly-clean, no-regression, wing, interval, full suite, end-to-end) → distributed across Tasks 1,3,4,5,6,7,8. ✓

**Placeholder scan:** the `<N>`/`<x>`/`<y>` tokens in commit messages are runtime-measured numbers recorded at execution, not code placeholders. `make_synthetic_slice_obs[_wideband]` / `load_spy_real_board` / `spy_real_inputs` are named helpers — the plan says to reuse the existing `spy_real_test.cpp` fixture or write a small local Black-76 obs builder; each is one small, well-specified function. No code step ships a TODO.

**Type consistency:** `arb_check_calendar(const CurveSurface&, double, double, std::uint32_t)` used identically in Tasks 1/2/5. `fit_convex_slice(..., const ConvexFitOpts&, const std::function<double(double)>&)` consistent Tasks 4/5/6/7. `qp_active_set(H,q,G,h,x,max_iter)` consistent Task 3 onward. `ConvexSliceFit::w_of_k` new in Task 6, used only there. `ConvexFitOpts.loss` new in Task 7. `w_prev` callback signature `double(double)` (log-moneyness → total variance) consistent Tasks 4/5.
