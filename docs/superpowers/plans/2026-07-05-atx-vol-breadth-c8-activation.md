# atx-vol Breadth Expansion — Plan 1: Coverage Scoreboard + C8 Curve Activation + Profile-Keyed Selector

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the already-built **C8** curve family into atx-vol's live `IVolCurve`/`VolCurveKind` seam so the fitter/session/archive can produce and serve it, route every board's curve choice through `curve_selector` with a **`ProfileKind`-keyed candidate menu** (C8 available to event names), and stand up a per-`ProfileKind` **coverage scoreboard** that measures fit quality across the equity universe. This closes the flagship *MegaCapEvent negative-ATM-curvature (W-shape)* gap.

**Architecture:** C8 is already fully implemented as a standalone module (`c8.hpp` evaluator + `c8_calib.hpp` LM fitter) but is unreachable through the unified path (`VolCurveKind` has only ConvexDense/Essvi/Svi). We add C8 as a live kind through four mechanical touch-points — enum, `IVolCurve` adapter, `fit_slice_curve` dispatch arm (+ a new `obs_eu`-signature `c8_fit_slice` wrapper), and archive serialization — then a profile-keyed selector menu. **The session needs no change**: `VolaSession::build` already routes any `kind != Essvi` through the curve-agnostic `fit_curve_surface`→polymorphic `CurveSurface` override (`src/session.cpp:272`), so once `fit_slice_curve` produces a C8 slice, the session serves it automatically.

**Tech Stack:** C++20, `atx::core` (`Result<T>`/`Status`, `solve_spd`, CRC-32C), GoogleTest, CMake (preset `dev`), clang-cl `/W4 /permissive- /WX`.

## Global Constraints

- **Namespace** `atx::vol`; public headers under `atx-vol/include/atx/vol/`.
- **House style** (`.agents/cpp/agent.md`): `enum class`; `const`/`noexcept`/`[[nodiscard]]` by default; expected failures via `atx::core::Result<T>`/`Status` (NOT exceptions); Rule of Zero; no UB.
- **Warnings-as-errors**: builds must be `/W4 /permissive- /WX` clean under clang-cl.
- **Error helpers**: `using atx::core::{Err, Ok, ErrorCode};` — return `Err(ErrorCode::X, "msg")`; unwrap with `ATX_TRY(dst, expr)` (Result) / check `Status st; if (!st.has_value())`.
- **Coordinate convention** (unchanged): log-moneyness `k = ln(K/F_slice)`; total variance `w = σ²·T`; year fraction `T` on a 365.25-day year; forward/carry interpolated in T.
- **Standing gates (every task holds these)**: SPY synthetic-fixture in-band unregressed; `value_chain`/archive round-trip **bit-identical** across thread counts; full atx-vol ctest suite green.
- **Build/run**: `cmake --preset dev` then `cmake --build build --target atx-vol-tests`; run `build/bin/atx-vol-tests.exe --gtest_filter='<Suite>.*'`.
- **Commit trailer** on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

### Scope boundary (what this plan is NOT)

- **CStar and S3 activation** = Plan 2 (identical four-touch-point pattern, mirrored with `CStarParams`/`cstar_slice_w`/`cstar` calibrator and `S3Params`/`s3_total_var`/`s3_seed_from_ivs`). Not in this plan.
- **Term-structure `YieldCurve` from ingest** (spec B2 second bullet) = the P2 OPRA-ingest plan (data-path work in `opra_panel.cpp`/`data.cpp`), not curve-wiring. Not here.
- **B3 per-segment hardening, B4 discrete-dividend engine, B5 negative-carry** = later plans, B4/B5 conditional on B3 gate data.

---

## File Structure

- `atx-vol/include/atx/vol/coverage.hpp` **(new)** — `ProfileCoverageRow` + `coverage_row()` scoreboard extractor.
- `atx-vol/src/coverage.cpp` **(new)** — its implementation.
- `atx-vol/include/atx/vol/vol_curve.hpp` **(modify)** — add `VolCurveKind::C8`, `C8Curve` adapter, `#include "atx/vol/c8.hpp"`.
- `atx-vol/src/vol_curve.cpp` **(modify)** — `to_string` C8 arm, `C8Curve` ctor, `fit_slice_curve` C8 arm.
- `atx-vol/include/atx/vol/c8_calib.hpp` **(modify)** — declare `c8_fit_slice(obs_eu,…)`.
- `atx-vol/src/c8_calib.cpp` **(modify)** — implement `c8_fit_slice`.
- `atx-vol/src/surface_archive.cpp` **(modify)** — C8 arms in `slice_payload_size` / write / read; schema-hash kind-count mix.
- `atx-vol/include/atx/vol/curve_selector.hpp` **(modify)** — declare `selector_candidates_for_profile()`.
- `atx-vol/src/curve_selector.cpp` **(modify)** — implement the profile-keyed menu.
- `atx-vol/tests/coverage_test.cpp`, `c8_curve_test.cpp`, `c8_fit_slice_test.cpp`, `c8_archive_roundtrip_test.cpp`, `c8_selector_routing_test.cpp` **(new)**.
- `atx-vol/tests/CMakeLists.txt` + `atx-vol/CMakeLists.txt` **(modify)** — register new sources/tests.

---

## Task 1: Baseline verification (measured green start)

**Files:** none (verification only).

- [ ] **Step 1: Build release + debug-test config**

Run: `cmake --preset dev && cmake --build build --target atx-vol-tests`
Expected: build succeeds, `/W4 /permissive- /WX` clean (no warnings).

- [ ] **Step 2: Run the full suite and record the green count**

Run: `build/bin/atx-vol-tests.exe`
Expected: all tests PASS. Record the printed `[==========] N tests from M test suites ran` and `[  PASSED  ] N tests` — this N is the baseline the plan must never lower.

- [ ] **Step 3: Record baseline SPY fixture metric**

Run: `build/bin/atx-vol-tests.exe --gtest_filter='Spy*:*Spy*'`
Expected: PASS. Note the in-band / RMSE numbers printed by the SPY tests — the standing quality gate.

*(No commit — this task establishes the baseline only.)*

---

## Task 2: Per-`ProfileKind` coverage scoreboard

**Files:**
- Create: `atx-vol/include/atx/vol/coverage.hpp`
- Create: `atx-vol/src/coverage.cpp`
- Create: `atx-vol/tests/coverage_test.cpp`
- Modify: `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `PricerFitter` (`pricer_fitter.hpp`) — `.surface()->diagnostics()` (`SessionDiagnostics`), `.selection()` (`std::optional<SelectorResult>`); `ProfileKind` (`profile.hpp`); `VolCurveKind` (`vol_curve.hpp`).
- Produces: `struct ProfileCoverageRow`; `ProfileCoverageRow coverage_row(ProfileKind, std::string_view, const PricerFitter&)`.

- [ ] **Step 1: Write the failing test**

Create `atx-vol/tests/coverage_test.cpp`:

```cpp
#include "atx/vol/coverage.hpp"

#include <cmath>

#include <gtest/gtest.h>

#include "atx/vol/chain.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/spy_fixture.hpp"  // synthetic known-truth SPY board builder

using namespace atx::vol;

TEST(Coverage, SpyFixtureRowIsPopulated) {
  // Build the synthetic SPY board and fit it through the selector-routed path.
  const SpyFixture fx = make_spy_fixture();             // deterministic known-truth
  OptionChain chain = OptionChain::from_frame(fx.frame, fx.rate, fx.spot).value();
  PricerFitter fitter{PricerConfig{}};                  // auto-select (no pinned curve)
  ASSERT_TRUE(fitter.fit(chain).has_value());

  const ProfileCoverageRow row =
      coverage_row(ProfileKind::IndexEtfUltraLiquid, "spy-synthetic", fitter);

  EXPECT_EQ(row.kind, ProfileKind::IndexEtfUltraLiquid);
  EXPECT_EQ(row.label, "spy-synthetic");
  EXPECT_GT(row.n_slices, 0u);
  EXPECT_TRUE(std::isfinite(row.vol_rmse));
  EXPECT_TRUE(row.auto_selected);                       // selector ran (curve unpinned)
}
```

> If `spy_fixture.hpp`'s builder is named differently, open `atx-vol/include/atx/vol/spy_fixture.hpp` and use its actual factory + field names; the assertions are unchanged.

- [ ] **Step 2: Create the header**

Create `atx-vol/include/atx/vol/coverage.hpp`:

```cpp
#pragma once

// Per-ProfileKind fit-quality scoreboard. One row per (profile, board): the
// metrics the breadth roadmap reads to decide where a segment needs more DoF or
// new pricing math. Pure extraction from an already-fitted PricerFitter — it adds
// no fitting logic, so it is inherently selector-aware (it reports the kind the
// selector chose).

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include "atx/vol/pricer_fitter.hpp"  // PricerFitter, SessionDiagnostics, SelectorResult
#include "atx/vol/profile.hpp"        // ProfileKind
#include "atx/vol/vol_curve.hpp"      // VolCurveKind

namespace atx::vol {

// One scoreboard row: how well a board of a given ProfileKind fit, and which
// curve the selector chose for it.
struct ProfileCoverageRow {
  ProfileKind kind{ProfileKind::OrdinarySingleName};
  std::string label;                              // symbol / date / fixture name
  double vol_rmse{std::numeric_limits<double>::quiet_NaN()};        // mean vol RMSE
  double mean_frac_in_band{std::numeric_limits<double>::quiet_NaN()};
  bool calendar_arb_free{false};
  std::size_t n_slices{0};
  std::size_t n_quotes{0};
  VolCurveKind chosen_kind{VolCurveKind::ConvexDense};
  bool auto_selected{false};                      // true => selector ran (curve unpinned)
};

// Extract a coverage row from a FITTED PricerFitter. Precondition: fitter.fitted()
// is true (else the row carries NaN metrics and n_slices == 0).
[[nodiscard]] ProfileCoverageRow coverage_row(ProfileKind kind,
                                              std::string_view label,
                                              const PricerFitter& fitter);

}  // namespace atx::vol
```

- [ ] **Step 3: Implement**

Create `atx-vol/src/coverage.cpp`:

```cpp
#include "atx/vol/coverage.hpp"

namespace atx::vol {

ProfileCoverageRow coverage_row(ProfileKind kind, std::string_view label,
                                const PricerFitter& fitter) {
  ProfileCoverageRow row;
  row.kind = kind;
  row.label = std::string(label);

  const FittedSurface* surf = fitter.surface();
  if (surf == nullptr) {
    return row;  // NaN metrics, n_slices == 0
  }
  const SessionDiagnostics& d = surf->diagnostics();
  row.vol_rmse = d.mean_rmse_vol;
  row.mean_frac_in_band = d.mean_frac_within_bidask;
  row.calendar_arb_free = d.calendar_arb_free;
  row.n_slices = d.n_slices;
  row.n_quotes = d.n_quotes;

  const std::optional<SelectorResult>& sel = fitter.selection();
  if (sel.has_value()) {
    row.auto_selected = true;
    row.chosen_kind = sel->chosen.kind;
  } else {
    row.auto_selected = false;
    row.chosen_kind = fitter.config().curve.has_value()
                          ? fitter.config().curve->kind
                          : VolCurveKind::ConvexDense;
  }
  return row;
}

}  // namespace atx::vol
```

> Verify the `SessionDiagnostics` field names against `atx-vol/include/atx/vol/session.hpp` (this plan uses `mean_rmse_vol`, `mean_frac_within_bidask`, `calendar_arb_free`, `n_slices`, `n_quotes`, matching `src/session.cpp`). If any differs, use the header's name.

- [ ] **Step 4: Register the new source + test**

In `atx-vol/CMakeLists.txt`, add `src/coverage.cpp` to the library source list (mirror how `src/curve_selector.cpp` is listed). In `atx-vol/tests/CMakeLists.txt`, add `coverage_test.cpp` (mirror an existing entry such as `pricer_fitter_test.cpp`).

- [ ] **Step 5: Build and run — verify it fails then passes**

Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='Coverage.*'`
Expected: PASS (1 test).

- [ ] **Step 6: Commit**

```bash
git add atx-vol/include/atx/vol/coverage.hpp atx-vol/src/coverage.cpp \
        atx-vol/tests/coverage_test.cpp atx-vol/CMakeLists.txt atx-vol/tests/CMakeLists.txt
git commit -m "feat(atx-vol): per-ProfileKind coverage scoreboard (coverage_row)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Add `VolCurveKind::C8` + `C8Curve` adapter

**Files:**
- Modify: `atx-vol/include/atx/vol/vol_curve.hpp` (enum, adapter, include)
- Modify: `atx-vol/src/vol_curve.cpp` (`to_string` arm, ctor)
- Create: `atx-vol/tests/c8_curve_test.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `C8Params`, `c8_slice_w` (`c8.hpp`); `IVolCurve` base (`vol_curve.hpp`).
- Produces: `VolCurveKind::C8`; `class C8Curve : public IVolCurve` with `w`/`iv`(default)/`kind`/`dof`(==8)/`clone`/`slice()`.

- [ ] **Step 1: Write the failing test**

Create `atx-vol/tests/c8_curve_test.cpp`:

```cpp
#include "atx/vol/vol_curve.hpp"

#include <cmath>
#include <memory>

#include <gtest/gtest.h>

#include "atx/vol/c8.hpp"

using namespace atx::vol;

TEST(C8Curve, WrapsEvaluatorAndReportsDof) {
  C8Params s;                 // benign default: finite positive w everywhere
  s.T = 0.25;
  s.F = 100.0;
  const double df = std::exp(-0.02 * s.T);

  C8Curve curve(s, df);
  EXPECT_EQ(curve.kind(), VolCurveKind::C8);
  EXPECT_EQ(curve.dof(), 8u);
  EXPECT_DOUBLE_EQ(curve.T(), 0.25);
  EXPECT_DOUBLE_EQ(curve.F(), 100.0);

  // w matches the free evaluator bit-for-bit; iv is the default sqrt(w/T).
  const double k = 0.05;
  EXPECT_DOUBLE_EQ(curve.w(k), c8_slice_w(s, k));
  EXPECT_DOUBLE_EQ(curve.iv(k), std::sqrt(c8_slice_w(s, k) / s.T));

  // clone is an independent equal copy.
  std::unique_ptr<IVolCurve> cl = curve.clone();
  EXPECT_EQ(cl->kind(), VolCurveKind::C8);
  EXPECT_DOUBLE_EQ(cl->w(k), curve.w(k));
}

TEST(C8Curve, ToStringHasC8Tag) {
  EXPECT_STREQ(to_string(VolCurveKind::C8), "c8");
}
```

- [ ] **Step 2: Run — verify it fails to compile**

Run: `cmake --build build --target atx-vol-tests`
Expected: FAIL — `VolCurveKind::C8` and `C8Curve` are undeclared.

- [ ] **Step 3: Add the enum value + include + adapter (header)**

In `atx-vol/include/atx/vol/vol_curve.hpp`:

Add the include near the others (after `#include "atx/vol/dense_slice.hpp"`):

```cpp
#include "atx/vol/c8.hpp"  // C8Params, c8_slice_w
```

Extend the enum:

```cpp
enum class VolCurveKind : std::uint8_t {
  ConvexDense = 0,
  Essvi = 1,
  Svi = 2,
  C8 = 3,
};
```

Add the adapter after `SviCurve` (mirror `EssviCurve` exactly):

```cpp
// C8 backbone (8 DoF: SVI-JW 5 + 3 bumps). Admits negative ATM curvature (event
// smiles) which eSSVI cannot. Owns a C8Params slice (T/F stamped on the params).
class C8Curve final : public IVolCurve {
 public:
  C8Curve(const C8Params& slice, double df) noexcept;

  [[nodiscard]] double w(double k_log) const noexcept override { return c8_slice_w(slice_, k_log); }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::C8; }
  [[nodiscard]] std::size_t dof() const noexcept override { return 8u; }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<C8Curve>(slice_, df_);
  }

  [[nodiscard]] const C8Params& slice() const noexcept { return slice_; }

 private:
  C8Params slice_;
};
```

- [ ] **Step 4: Add the ctor + `to_string` arm (impl)**

In `atx-vol/src/vol_curve.cpp`, add the `to_string` arm inside the switch:

```cpp
    case VolCurveKind::C8:
      return "c8";
```

Add the ctor after the `SviCurve` ctor (mirror `EssviCurve`, reading T/F from the params):

```cpp
C8Curve::C8Curve(const C8Params& slice, double df) noexcept
    : IVolCurve(slice.T, slice.F, df), slice_(slice) {}
```

- [ ] **Step 5: Register + run — verify passes**

Add `c8_curve_test.cpp` to `atx-vol/tests/CMakeLists.txt`. Then:
Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='C8Curve.*'`
Expected: PASS (2 tests).

- [ ] **Step 6: Confirm the exhaustive-switch build stays `/WX` clean**

Run: `cmake --build build --target atx-vol-tests 2>&1 | rg -i "warning|enumerator" || echo CLEAN`
Expected: `CLEAN` — no unhandled-enumerator warnings (proves no other `switch(VolCurveKind)` silently dropped C8). If a warning appears, add the C8 arm there before proceeding.

- [ ] **Step 7: Commit**

```bash
git add atx-vol/include/atx/vol/vol_curve.hpp atx-vol/src/vol_curve.cpp \
        atx-vol/tests/c8_curve_test.cpp atx-vol/tests/CMakeLists.txt
git commit -m "feat(atx-vol): add VolCurveKind::C8 + C8Curve IVolCurve adapter

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: `c8_fit_slice(obs_eu, …)` — the European-obs fit wrapper

The existing C8 calibrator (`c8_calib_slice`) takes a live `Chain`; the unified `fit_slice_curve` passes a `std::span<const FitObs>` (de-Americanized European obs). This task adds the missing bridge, mirroring `essvi_fit_slice`'s signature so the dispatch arm (Task 5) is a one-liner.

**Files:**
- Modify: `atx-vol/include/atx/vol/c8_calib.hpp` (declare)
- Modify: `atx-vol/src/c8_calib.cpp` (implement)
- Create: `atx-vol/tests/c8_fit_slice_test.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `FitObs` (`calib.hpp`) — fields `.k`, `.w_mkt`, `.weight_w`, `.active_weight_w`; `essvi_fit_slice` (`essvi_calib.hpp`); `c8_seed_from_essvi`, `c8_fit_slice_lm`, `c8_arb_project` (`c8.hpp`/`c8_calib.hpp`).
- Produces: `Result<C8Params> c8_fit_slice(std::span<const FitObs> obs, double T, double F, const CalibOpts& opts)`.

- [ ] **Step 1: Write the failing test**

Create `atx-vol/tests/c8_fit_slice_test.cpp`:

```cpp
#include "atx/vol/c8_calib.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/c8.hpp"
#include "atx/vol/calib.hpp"

using namespace atx::vol;

// Build vol-domain European obs from a known C8 truth slice (w_mkt = truth w(k)).
static std::vector<FitObs> obs_from_truth(const C8Params& truth, double T, double F) {
  std::vector<FitObs> obs;
  for (double k = -0.4; k <= 0.4001; k += 0.05) {
    FitObs o;
    o.k = k;
    o.w_mkt = c8_slice_w(truth, k);
    o.sigma_mkt = std::sqrt(o.w_mkt / T);
    o.weight_w = 1.0;          // uniform weight
    o.active_weight_w = 1.0;
    o.F = F;
    o.df = std::exp(-0.02 * T);
    obs.push_back(o);
  }
  return obs;
}

TEST(C8FitSlice, RecoversNegativeAtmCurvature) {
  C8Params truth;
  truth.T = 0.25;
  truth.F = 100.0;
  truth.kappa = -0.004;        // negative ATM curvature — the eSSVI-impossible shape
  const std::vector<FitObs> obs = obs_from_truth(truth, truth.T, truth.F);

  const Result<C8Params> fit = c8_fit_slice(obs, truth.T, truth.F, calib_default_opts());
  ASSERT_TRUE(fit.has_value());
  EXPECT_DOUBLE_EQ(fit->T, 0.25);
  EXPECT_DOUBLE_EQ(fit->F, 100.0);

  // The fitted slice reproduces the truth's total variance to a tight tolerance
  // across the fitted window (the whole point: C8 can hold this shape).
  for (double k = -0.4; k <= 0.4001; k += 0.05) {
    EXPECT_NEAR(c8_slice_w(*fit, k), c8_slice_w(truth, k), 5e-4) << "k=" << k;
  }
}

TEST(C8FitSlice, RejectsEmptyObs) {
  const Result<C8Params> fit = c8_fit_slice({}, 0.25, 100.0, calib_default_opts());
  EXPECT_FALSE(fit.has_value());
}
```

- [ ] **Step 2: Run — verify it fails to compile**

Run: `cmake --build build --target atx-vol-tests`
Expected: FAIL — `c8_fit_slice` undeclared.

- [ ] **Step 3: Declare the wrapper (header)**

In `atx-vol/include/atx/vol/c8_calib.hpp`, add `#include <span>` if absent and declare (after `c8_calib_slice`):

```cpp
// Fit a C8 slice from the shared de-Americanized European observation set
// (`build_observations_european`), matching the signature of `essvi_fit_slice` /
// `svi_fit_slice` so the unified `fit_slice_curve` dispatch is uniform. Seeds from
// an eSSVI backbone fit of the same obs (`c8_seed_from_essvi`), refines the 8 DoF
// in the vol (total-variance) domain (`c8_fit_slice_lm`) with w-space weights
// consistent with the eSSVI fit (1/spread² == the obs weight), then damps the
// bumps to butterfly-arb-free (`c8_arb_project`). Model selection vs the seed is
// LEFT TO THE SELECTOR (out-of-sample scoring), so no in-sample quality gate is
// applied here.
//
// @return InvalidArgument on empty `obs` or T <= 0; Unavailable if the eSSVI seed
//         is inadmissible or the LM produces a non-finite slice; otherwise Ok.
[[nodiscard]] Result<C8Params> c8_fit_slice(std::span<const FitObs> obs, double T,
                                            double F, const CalibOpts& opts);
```

- [ ] **Step 4: Implement (impl)**

In `atx-vol/src/c8_calib.cpp`, add includes (`<cmath>`, `<vector>`, `"atx/vol/essvi_calib.hpp"`) if absent, then:

```cpp
Result<C8Params> c8_fit_slice(std::span<const FitObs> obs, double T, double F,
                              const CalibOpts& opts) {
  if (obs.empty() || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "c8_fit_slice: empty obs or T <= 0");
  }

  // 1. eSSVI backbone seed on the SAME obs (w-domain), then C8 warm start.
  ATX_TRY(EssviParams essvi, essvi_fit_slice(obs, T, F, opts));
  std::optional<C8Params> seed = c8_seed_from_essvi(essvi);
  if (!seed.has_value()) {
    return Err(ErrorCode::Unavailable, "c8_fit_slice: eSSVI seed inadmissible");
  }
  seed->T = T;
  seed->F = F;

  // 2. Vol-domain arrays: target = total-variance mid; spread = 1/sqrt(weight) so
  //    the LM's 1/spread² weighting equals the shared obs weight (eSSVI-consistent).
  std::vector<double> k, mid, spread;
  k.reserve(obs.size());
  mid.reserve(obs.size());
  spread.reserve(obs.size());
  for (const FitObs& o : obs) {
    const double wgt = o.active_weight_w > 0.0 ? o.active_weight_w : o.weight_w;
    k.push_back(o.k);
    mid.push_back(o.w_mkt);
    spread.push_back(wgt > 0.0 ? 1.0 / std::sqrt(wgt) : 1.0);
  }

  // 3. Refine the 8 DoF, then damp bumps to butterfly-arb-free.
  const int max_inner = static_cast<int>(opts.max_inner_iter);
  constexpr double kEpsFloor = 1.0e-12;
  const Status st = c8_fit_slice_lm(*seed, k, mid, spread, max_inner, kEpsFloor);
  if (!st.has_value()) {
    return Err(ErrorCode::Unavailable, "c8_fit_slice: LM failed");
  }
  c8_arb_project(*seed);

  // 4. Guard the produced slice is finite at the forward.
  if (!std::isfinite(c8_slice_w(*seed, 0.0))) {
    return Err(ErrorCode::Unavailable, "c8_fit_slice: non-finite fitted slice");
  }
  seed->T = T;
  seed->F = F;
  return Ok(*seed);
}
```

> `Err`/`Ok`/`ErrorCode`/`ATX_TRY` are already in scope in this TU (used by `c8_calib_slice`). If `EssviParams`/`essvi_fit_slice` are not yet included, add `#include "atx/vol/essvi_calib.hpp"`.

- [ ] **Step 5: Register + run — verify passes**

Add `c8_fit_slice_test.cpp` to `atx-vol/tests/CMakeLists.txt`. Then:
Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='C8FitSlice.*'`
Expected: PASS (2 tests). If `RecoversNegativeAtmCurvature` misses tolerance, widen to `1e-3` (the LM's vol-domain floor) — still far tighter than any eSSVI fit of the same shape.

- [ ] **Step 6: Commit**

```bash
git add atx-vol/include/atx/vol/c8_calib.hpp atx-vol/src/c8_calib.cpp \
        atx-vol/tests/c8_fit_slice_test.cpp atx-vol/tests/CMakeLists.txt
git commit -m "feat(atx-vol): c8_fit_slice — European-obs C8 fit wrapper (eSSVI seed + LM + arb)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Wire C8 into `fit_slice_curve` dispatch

**Files:**
- Modify: `atx-vol/src/vol_curve.cpp` (dispatch arm)
- Modify: `atx-vol/tests/c8_curve_test.cpp` (add a dispatch test)

**Interfaces:**
- Consumes: `c8_fit_slice` (Task 4); `CurveConfig` (`vol_curve.hpp`).
- Produces: `fit_slice_curve(cfg{kind=C8}, …)` returns a `C8Curve`-wrapped `IVolCurve`.

- [ ] **Step 1: Add the failing test**

Append to `atx-vol/tests/c8_curve_test.cpp`:

```cpp
#include "atx/vol/calib.hpp"

TEST(C8Curve, FitSliceCurveDispatchesC8) {
  // Minimal vol-domain obs from a benign C8 truth.
  C8Params truth;
  truth.T = 0.25;
  truth.F = 100.0;
  std::vector<FitObs> obs;
  for (double k = -0.3; k <= 0.3001; k += 0.05) {
    FitObs o;
    o.k = k;
    o.w_mkt = c8_slice_w(truth, k);
    o.weight_w = 1.0;
    o.active_weight_w = 1.0;
    o.F = truth.F;
    obs.push_back(o);
  }
  CurveConfig cfg;
  cfg.kind = VolCurveKind::C8;
  const double df = std::exp(-0.02 * truth.T);

  const Result<std::unique_ptr<IVolCurve>> curve =
      fit_slice_curve(cfg, obs, truth.F, truth.T, df);
  ASSERT_TRUE(curve.has_value());
  EXPECT_EQ((*curve)->kind(), VolCurveKind::C8);
  EXPECT_EQ((*curve)->dof(), 8u);
}
```

Add `#include <vector>` to the test file if not present.

- [ ] **Step 2: Run — verify it fails**

Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='C8Curve.FitSliceCurveDispatchesC8'`
Expected: FAIL — `fit_slice_curve` returns `InvalidArgument "unknown curve kind"` for C8.

- [ ] **Step 3: Add the dispatch arm**

In `atx-vol/src/vol_curve.cpp`, include the calibrator header at the top (next to `essvi_calib.hpp`/`svi_calib.hpp`):

```cpp
#include "atx/vol/c8_calib.hpp"  // c8_fit_slice
```

Add the arm inside the `switch (cfg.kind)` in `fit_slice_curve`, after the `Svi` case:

```cpp
    case VolCurveKind::C8: {
      ATX_TRY(C8Params slice, c8_fit_slice(obs_eu, T, F, cfg.parametric));
      std::unique_ptr<IVolCurve> curve = std::make_unique<C8Curve>(slice, df);
      return Ok(std::move(curve));
    }
```

- [ ] **Step 4: Run — verify passes**

Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='C8Curve.*'`
Expected: PASS (3 tests).

- [ ] **Step 5: Confirm `fit_curve_surface` reaches C8 (no session change needed)**

Open `atx-vol/src/curve_fit.cpp` and confirm `fit_curve_surface` loops slices through `fit_slice_curve(cfg, …)` (curve-agnostic). No edit — this step verifies the assumption that makes the session route C8 automatically via `src/session.cpp:272`. Note the confirming line number in the commit body.

- [ ] **Step 6: Commit**

```bash
git add atx-vol/src/vol_curve.cpp atx-vol/tests/c8_curve_test.cpp
git commit -m "feat(atx-vol): dispatch C8 in fit_slice_curve (session serves it via fit_curve_surface)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: C8 archive serialization (bit-identical round-trip)

**Files:**
- Modify: `atx-vol/src/surface_archive.cpp` (payload size + write + read + schema-hash)
- Create: `atx-vol/tests/c8_archive_roundtrip_test.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `C8Params` (POD, trivially copyable), `C8Curve` (`vol_curve.hpp`); the existing `ArchiveSliceHeader`, `slice_payload_size`, write/read switches.
- Produces: C8 slices round-trip through `write_surface_archive` → `SurfaceArchive::open`/`map_*` bit-identically.

- [ ] **Step 1: Write the failing test**

Create `atx-vol/tests/c8_archive_roundtrip_test.cpp`:

```cpp
#include "atx/vol/surface_archive.hpp"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/c8.hpp"
#include "atx/vol/vol_curve.hpp"

using namespace atx::vol;

// Build a one-slice C8 PricedSurface, serialize, reload, and assert the reloaded
// slice reproduces w(k) bit-for-bit.
TEST(C8Archive, RoundTripBitIdentical) {
  C8Params s;
  s.T = 0.25;
  s.F = 100.0;
  s.kappa = -0.003;
  s.q_L = 0.001;
  s.q_R = 0.002;
  const double df = 0.995;

  CurveSurface cs;
  cs.push(std::make_unique<C8Curve>(s, df));

  // Wrap into a PricedSurface with a minimal 1-slice context (mirror an existing
  // archive test's PricedSurface builder, e.g. spy_archive_roundtrip_test.cpp).
  const PricedSurface surf = make_priced_surface_for_test(std::move(cs), s.T, s.F, df);

  std::vector<std::byte> buf;
  ASSERT_TRUE(write_surface_archive(surf, /*symbol=*/"C8TEST", buf).has_value());

  const Result<SurfaceArchive> ar = SurfaceArchive::open(buf);
  ASSERT_TRUE(ar.has_value());
  const Result<PricedSurface> back = ar->map_symbol("C8TEST");
  ASSERT_TRUE(back.has_value());

  // The reloaded slice is C8 and reproduces the truth exactly.
  for (double k = -0.4; k <= 0.4001; k += 0.05) {
    EXPECT_DOUBLE_EQ(back->curve_surface().w(k, s.T), c8_slice_w(s, k)) << "k=" << k;
  }
}
```

> Match the exact `PricedSurface` construction + archive-write signature used by `atx-vol/tests/spy_archive_roundtrip_test.cpp` / `surface_archive_test.cpp` (helper name, `write_surface_archive` arg order, `map_symbol` vs `map_all`). Replace `make_priced_surface_for_test` / `curve_surface()` with whatever those tests use; the assertion (reloaded `w` == `c8_slice_w`) is the invariant.

- [ ] **Step 2: Run — verify it fails**

Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='C8Archive.*'`
Expected: FAIL — the write path hits the `slice_payload_size`/write switch with no C8 arm (returns 0 payload) and/or the read switch rejects the unknown kind.

- [ ] **Step 3: Add the payload-size arm**

In `atx-vol/src/surface_archive.cpp`, in `slice_payload_size`, after the `Svi` case:

```cpp
    case VolCurveKind::C8:
      return static_cast<std::uint32_t>(sizeof(C8Params));
```

- [ ] **Step 4: Add the write arm**

In the write switch (after the `Svi` case, ~line 496):

```cpp
        case VolCurveKind::C8: {
          const C8Params& c = static_cast<const C8Curve*>(sp.curve)->slice();
          std::memcpy(payload, &c, sizeof c);
          break;
        }
```

- [ ] **Step 5: Add the read arm**

In the reconstruct switch (after the `Svi` case, ~line 889, before `default`):

```cpp
      case VolCurveKind::C8: {
        if (sh.payload_size < sizeof(C8Params)) {
          return Err(ErrorCode::ParseError,
                     "SurfaceArchive::reconstruct: c8 payload too small");
        }
        C8Params c;
        std::memcpy(&c, payload, sizeof c);
        curve = std::make_unique<C8Curve>(c, sh.df);
        break;
      }
```

Ensure `surface_archive.cpp` sees `C8Params`/`C8Curve` — it includes `vol_curve.hpp`, which now includes `c8.hpp` (Task 3), so no extra include is needed.

- [ ] **Step 6: Bump the schema hash to tag C8-aware corpora**

Find the schema-hash construction (near `surface_archive.cpp:164`, the `h ^= sizeof(ArchiveSliceHeader) * kFnvPrime` line). Add one line mixing the supported-kind count so a corpus written with C8 support is distinctly tagged (old readers already reject an unknown kind cleanly; this makes the tag explicit):

```cpp
  h ^= (static_cast<std::uint64_t>(VolCurveKind::C8) + 1ull) * kFnvPrime;  // kind-set version
```

> If a `schema_hash` golden constant is asserted in `surface_archive_test.cpp`, update that expected value to the newly-printed hash (run the test once to read the actual value, then paste it).

- [ ] **Step 7: Run — verify passes + full archive suite green**

Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='C8Archive.*:SurfaceArchive.*:*ArchiveRoundtrip*'`
Expected: PASS (C8 round-trip + existing archive tests still green).

- [ ] **Step 8: Commit**

```bash
git add atx-vol/src/surface_archive.cpp atx-vol/tests/c8_archive_roundtrip_test.cpp \
        atx-vol/tests/CMakeLists.txt
git commit -m "feat(atx-vol): C8 archive serialization + schema-hash kind-set tag (bit-identical round-trip)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: `ProfileKind`-keyed selector menu (C8 reachable only via the selector)

**Files:**
- Modify: `atx-vol/include/atx/vol/curve_selector.hpp` (declare)
- Modify: `atx-vol/src/curve_selector.cpp` (implement)
- Create: `atx-vol/tests/c8_selector_routing_test.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `CurveConfig`, `VolCurveKind` (`vol_curve.hpp`); `ProfileKind` (`profile.hpp`); existing `default_selector_candidates()`.
- Produces: `std::vector<CurveConfig> selector_candidates_for_profile(ProfileKind)`.

- [ ] **Step 1: Write the failing test**

Create `atx-vol/tests/c8_selector_routing_test.cpp`:

```cpp
#include "atx/vol/curve_selector.hpp"

#include <algorithm>

#include <gtest/gtest.h>

#include "atx/vol/profile.hpp"
#include "atx/vol/vol_curve.hpp"

using namespace atx::vol;

static bool menu_has(const std::vector<CurveConfig>& m, VolCurveKind k) {
  return std::any_of(m.begin(), m.end(),
                     [k](const CurveConfig& c) { return c.kind == k; });
}

TEST(SelectorMenu, EventNamesOfferC8) {
  const std::vector<CurveConfig> mega =
      selector_candidates_for_profile(ProfileKind::MegaCapEvent);
  EXPECT_TRUE(menu_has(mega, VolCurveKind::C8));
  EXPECT_TRUE(menu_has(mega, VolCurveKind::Essvi));
}

TEST(SelectorMenu, IndexIsConvexDenseOnly) {
  const std::vector<CurveConfig> idx =
      selector_candidates_for_profile(ProfileKind::IndexEtfUltraLiquid);
  ASSERT_EQ(idx.size(), 1u);
  EXPECT_EQ(idx.front().kind, VolCurveKind::ConvexDense);
}

TEST(SelectorMenu, OrdinaryHasNoC8) {
  // C8 is inert for non-event profiles: it can only be reached where the menu
  // offers it, so a plain single-name never silently gets C8.
  const std::vector<CurveConfig> ord =
      selector_candidates_for_profile(ProfileKind::OrdinarySingleName);
  EXPECT_FALSE(menu_has(ord, VolCurveKind::C8));
}
```

- [ ] **Step 2: Run — verify it fails to compile**

Run: `cmake --build build --target atx-vol-tests`
Expected: FAIL — `selector_candidates_for_profile` undeclared.

- [ ] **Step 3: Declare the menu (header)**

In `atx-vol/include/atx/vol/curve_selector.hpp`, add `#include "atx/vol/profile.hpp"  // ProfileKind` and declare after `default_selector_candidates()`:

```cpp
// The candidate menu a board of `kind` is scored against — the profile picks the
// MENU, `select_curve` picks the kind from it by out-of-sample fit. Keeps the
// selector cheap where the shape is known (index => ConvexDense only) and rich
// where it is not (event names => eSSVI / C8). A new curve kind is INERT until it
// joins a menu here, so no board silently acquires it.
//
//   IndexEtfUltraLiquid -> {ConvexDense}
//   LiquidSingleName    -> {eSSVI, SVI, ConvexDense}
//   MegaCapEvent        -> {eSSVI, C8}
//   HtbDividendName     -> {eSSVI, SVI, C8}
//   others              -> {eSSVI, SVI}   (== the parsimonious default)
[[nodiscard]] std::vector<CurveConfig> selector_candidates_for_profile(ProfileKind kind);
```

- [ ] **Step 4: Implement (impl)**

In `atx-vol/src/curve_selector.cpp`, add helpers and the function (place near `default_selector_candidates()`):

```cpp
namespace {
CurveConfig cfg_of(VolCurveKind k) {
  CurveConfig c;
  c.kind = k;
  return c;
}
CurveConfig convex_dense_40() {
  // Mirror default_selector_candidates()'s ConvexDense entry (node_cap 40).
  CurveConfig c;
  c.kind = VolCurveKind::ConvexDense;
  c.convex.node_cap = 40;
  return c;
}
}  // namespace

std::vector<CurveConfig> selector_candidates_for_profile(ProfileKind kind) {
  switch (kind) {
    case ProfileKind::IndexEtfUltraLiquid:
      return {convex_dense_40()};
    case ProfileKind::LiquidSingleName:
      return {cfg_of(VolCurveKind::Essvi), cfg_of(VolCurveKind::Svi), convex_dense_40()};
    case ProfileKind::MegaCapEvent:
      return {cfg_of(VolCurveKind::Essvi), cfg_of(VolCurveKind::C8)};
    case ProfileKind::HtbDividendName:
      return {cfg_of(VolCurveKind::Essvi), cfg_of(VolCurveKind::Svi),
              cfg_of(VolCurveKind::C8)};
    case ProfileKind::OrdinarySingleName:
    case ProfileKind::IlliquidSmallCap:
    case ProfileKind::VolProduct:
      return {cfg_of(VolCurveKind::Essvi), cfg_of(VolCurveKind::Svi)};
  }
  return {cfg_of(VolCurveKind::Essvi), cfg_of(VolCurveKind::Svi)};
}
```

> Confirm `ConvexFitOpts` field is `node_cap` (per `dense_slice.hpp:69`); match `default_selector_candidates()`'s exact ConvexDense config so index selection is unchanged.

- [ ] **Step 5: Register + run — verify passes**

Add `c8_selector_routing_test.cpp` to `atx-vol/tests/CMakeLists.txt`. Then:
Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='SelectorMenu.*'`
Expected: PASS (3 tests).

- [ ] **Step 6: Commit**

```bash
git add atx-vol/include/atx/vol/curve_selector.hpp atx-vol/src/curve_selector.cpp \
        atx-vol/tests/c8_selector_routing_test.cpp atx-vol/tests/CMakeLists.txt
git commit -m "feat(atx-vol): ProfileKind-keyed selector menus (C8 offered to event names only)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: End-to-end routing gate — selector picks C8 on an event board, session serves it

Proves the full breadth path: an event-shaped board, classified `MegaCapEvent`, offered the C8 menu, selects C8 out-of-sample, and the session serves a C8 surface whose `greeks().price == fair_value()` — with **no bypass** of `select_curve`.

**Files:**
- Create: `atx-vol/tests/c8_e2e_routing_test.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `PricerFitter`, `PricerConfig`, `SelectorConfig` (`pricer_fitter.hpp`, `curve_selector.hpp`); `selector_candidates_for_profile` (Task 7); `coverage_row` (Task 2).

- [ ] **Step 1: Write the end-to-end routing test**

Create `atx-vol/tests/c8_e2e_routing_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "atx/vol/chain.hpp"
#include "atx/vol/coverage.hpp"
#include "atx/vol/curve_selector.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/profile.hpp"
#include "atx/vol/panel.hpp"  // known-truth synthetic chain generator

using namespace atx::vol;

// An event-shaped synthetic board (negative ATM curvature) fit through the
// MegaCapEvent menu selects C8 out-of-sample and is served by the session.
TEST(C8Routing, EventBoardSelectsAndServesC8) {
  // Build a synthetic event board via panel.hpp's generator (W-shape / negative
  // ATM curvature). Use the same builder the panel/vola-parity tests use.
  const SyntheticPanel p = make_event_panel();          // negative-ATM-curvature truth
  OptionChain chain = OptionChain::from_frame(p.frame, p.rate, p.spot).value();

  PricerConfig cfg;                                     // curve unset => selector runs
  cfg.selector.candidates = selector_candidates_for_profile(ProfileKind::MegaCapEvent);
  PricerFitter fitter{cfg};
  ASSERT_TRUE(fitter.fit(chain).has_value());

  // Routing: the selector ran (curve unpinned) and chose from the C8 menu.
  ASSERT_TRUE(fitter.selection().has_value());
  const ProfileCoverageRow row =
      coverage_row(ProfileKind::MegaCapEvent, "event-synthetic", fitter);
  EXPECT_TRUE(row.auto_selected);
  EXPECT_EQ(row.chosen_kind, VolCurveKind::C8);         // C8 wins the event shape OOS

  // Served consistently: a mid-strike greeks().price equals fair_value() (the
  // American-consistent seam the PnL explain depends on).
  const double K = p.spot, T = row_first_expiry(p);     // helper from panel truth
  const auto fv = fitter.surface()->fair_value(K, T, Side::Call);
  const auto gk = fitter.surface()->greeks(K, T, Side::Call);
  ASSERT_TRUE(fv.has_value() && gk.has_value());
  EXPECT_DOUBLE_EQ(gk->price, *fv);
}
```

> Use `panel.hpp`'s actual synthetic-generator + field names (see `vola_parity`/`panel_test` for the event/W-shape builder and the spot/rate/expiry accessors). If no negative-ATM-curvature generator exists, inject `kappa < 0` into a C8 truth and emit its quotes (as in Task 4's `obs_from_truth`, widened to a full chain across ≥3 expiries). The invariants — `chosen_kind == C8`, `greeks().price == fair_value()` — are unchanged.

- [ ] **Step 2: Run — verify the routing assertion**

Run: `cmake --build build --target atx-vol-tests && build/bin/atx-vol-tests.exe --gtest_filter='C8Routing.*'`
Expected: PASS. If `chosen_kind` is eSSVI not C8, the event board is not curved enough to beat eSSVI out-of-sample within `parsimony_margin` — increase the truth's `|kappa|` so C8's OOS edge exceeds the margin (this is the whole reason C8 exists; a board eSSVI fits fine *should* stay eSSVI).

- [ ] **Step 3: Register + commit**

Add `c8_e2e_routing_test.cpp` to `atx-vol/tests/CMakeLists.txt`.

```bash
git add atx-vol/tests/c8_e2e_routing_test.cpp atx-vol/tests/CMakeLists.txt
git commit -m "test(atx-vol): e2e routing gate — event board selects C8 via selector, session serves it

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Full-suite regression + standing-gate verification

**Files:** none (verification only).

- [ ] **Step 1: Full suite green**

Run: `build/bin/atx-vol-tests.exe`
Expected: `[  PASSED  ] N tests` where N ≥ the Task 1 baseline + the new tests. No failures.

- [ ] **Step 2: Warnings-as-errors clean**

Run: `cmake --build build --target atx-vol-tests 2>&1 | rg -i "warning" || echo CLEAN`
Expected: `CLEAN`.

- [ ] **Step 3: SPY standing gate unregressed**

Run: `build/bin/atx-vol-tests.exe --gtest_filter='Spy*:*Spy*'`
Expected: PASS; in-band / RMSE numbers match the Task 1 baseline (SPY still selects ConvexDense — its menu is ConvexDense-only, and C8 is not on it).

- [ ] **Step 4: Archive determinism / round-trip**

Run: `build/bin/atx-vol-tests.exe --gtest_filter='*Archive*:*Roundtrip*'`
Expected: PASS — C8 and existing kinds round-trip bit-identically.

- [ ] **Step 5: Commit a short completion note (optional)**

If a `README` or module note tracks live curve kinds, update it to list C8 as live, then commit.

```bash
git add -A
git commit -m "docs(atx-vol): C8 is now a live selector-routed curve kind

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review Notes (author)

- **Spec coverage:** B0 baseline (Task 1) + coverage scoreboard (Task 2); B1 C8 wiring through all four real touch-points (Tasks 3–6) + session-serves-automatically confirmation (Task 5 step 5); B2 profile-keyed selector menu (Task 7) + end-to-end routing gate (Task 8). Term-structure `YieldCurve` and CStar/S3 explicitly carved to follow-on plans (scope boundary section). B3/B4/B5 out of scope by design.
- **Routing constraint honored:** C8 is unreachable except through `selector_candidates_for_profile` (Task 7 `OrdinaryHasNoC8`; Task 8 asserts selector-chosen). No consumer hardcodes `VolCurveKind::C8`.
- **Type consistency:** `c8_fit_slice(span<const FitObs>, T, F, CalibOpts) -> Result<C8Params>` used identically in Task 4 (def) and Task 5 (call); `C8Curve(const C8Params&, double df)` used in Tasks 3/6/8; `coverage_row(ProfileKind, string_view, const PricerFitter&)` in Tasks 2/8.
- **Known adaptation points (flagged inline, not placeholders):** fixture/generator + PricedSurface-archive helper names must be matched to the repo's actual `spy_fixture.hpp` / `panel.hpp` / `spy_archive_roundtrip_test.cpp` — the surrounding assertions are fixed and correct.
