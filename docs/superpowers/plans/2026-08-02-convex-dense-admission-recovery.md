# Convex-Dense Admission Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recover the ~10% of SPY surface-db cells (181/1890) that die at the terminal ConvexDense rung on marginal Calendar/Butterfly admission rejections, by re-running the dense fit's repair loop pinned to the admission oracle's exact grid and tolerance — producer-side only, hot path bit-identical, oracle untouched.

**Architecture:** Three layers. (1) `fit_slice_curve`'s ConvexDense calendar-repair loop gains an optional `ConvexRepairSpec` override (grid, tolerance, extra exact-QP-node k's); `nullopt` keeps today's fixed lattice code path byte-for-byte. (2) A pure helper module (`detail/convex_recovery`) translates the oracle's `RiskSurfaceValidationConfig` + a rejection's `ValidationDigest` into that spec. (3) `PricerFitter::fit` gains a bounded strict-recovery loop that fires ONLY after the existing fallback ladder is exhausted with a pure-geometry rejection — today's terminal give-up point — so no admitted fit ever pays for it.

**Root cause being fixed (diagnosed 2026-08-02, zero refuting cases in 181 cells):** the repair loop accepts calendar crossings ≤ 1e-7 on a 65-pt lattice over [-0.60, 0.60] (spacing 0.01875), while the admission oracle rejects > 1e-8 on its own uniform grid (e.g. Balanced: 65 pts over [-0.50, 0.50]); the grids are phase-misaligned and the dominant violation k = -0.50 is not on the repair lattice. Butterfly cells are FP-scale convexity kinks at the intrinsic price-slope-bound seam between QP nodes. All slack magnitudes sub-vol-tick.

**Tech Stack:** C++20, CMake presets (`dev` → `c:/atx/build`, `rel` → `c:/atx/build-rel`), GoogleTest target `atx-vol-tests`, gtest exe at `build/bin/atx-vol-tests.exe`.

## Global Constraints

- **Oracle untouched:** never change any `RiskSurfaceValidationConfig` tolerance, grid count, or band; never change `risk_validation_config()` (pricer_fitter.cpp:1491). The only permitted edit in `risk_surface_validation.{hpp,cpp}` is hoisting the `sample_k` formula into the header VERBATIM (Task 2). Repo philosophy (research/2026-07-surface-db-production-run.md): tuning admission thresholds to manufacture clean counts is forbidden.
- **Hot path bit-identical:** `CurveConfig::convex_repair` defaults to `std::nullopt`; when nullopt, the legacy repair loop must execute the exact same floating-point expressions as today (keep `kRiskCalendarMin + dk * gi` arithmetic verbatim). Existing test `CalendarFloorSlackIsBitIdentical` (dense_slice_test.cpp) must stay green.
- **Recovery is rejection-gated:** the strict-recovery loop in `PricerFitter::fit` runs only when `!admission.publish_candidate` AND the digest's failures are a subset of {Butterfly, Calendar, CarryGap} with at least one of {Butterfly, Calendar} set. Admitted fits never enter it.
- **Dependency direction:** `vol_curve.hpp` / `vol_curve.cpp` must NOT include `atx/vol/detail/risk_surface_validation.hpp` (the spec is plain data). `detail/convex_recovery.hpp` may include both.
- **Style:** 100-column line limit; comments state constraints, not narration; match surrounding comment density/idiom.
- **Build/test commands (Windows, run from `c:/atx`):**
  - Build tests: `cmake --build --preset dev --target atx-vol-tests`
  - Run one test: `build/bin/atx-vol-tests.exe --gtest_filter=VolCurve.ConvexRepairSpec*`
  - Full vol suite: `build/bin/atx-vol-tests.exe --gtest_brief=1`
- **Production data is read-only:** `C:/atx-data/opra-hive` and `C:/atx-data/surface-db/spy-*` must never be written. Task 4 uses a fresh throwaway DB root.
- Commit after each task with a conventional message; never commit on main (work happens in the plan's worktree branch).

---

### Task 1: `ConvexRepairSpec` — strict repair grid/tolerance/nodes in `fit_slice_curve`

**Files:**
- Modify: `atx-vol/include/atx/vol/vol_curve.hpp` (struct + `CurveConfig` field, near line 410)
- Modify: `atx-vol/src/vol_curve.cpp` (ConvexDense branch, lines 367-431)
- Test: `atx-vol/tests/vol_curve_test.cpp`

**Interfaces:**
- Consumes: existing `fit_slice_curve(const CurveConfig&, std::span<const FitObs>, double F, double T, double df, const std::function<double(double)>& w_prev, std::span<const double> calendar_floor_knots, std::pair<double,double> prev_data_k_range)` — later args have defaults; tests may call with 5 or 6 args.
- Produces: `struct ConvexRepairSpec { double k_min; double k_max; std::uint32_t grid_points; double tolerance; std::vector<double> extra_node_ks; }` and `CurveConfig::convex_repair` (type `std::optional<ConvexRepairSpec>`, default nullopt). Task 2's `make_strict_repair_spec` returns this struct; Task 3 assigns it onto `SessionInputs::curve.convex_repair`.

- [ ] **Step 1: Write the failing tests**

Add to `atx-vol/tests/vol_curve_test.cpp` (reuse the file's existing `make_smile_obs(T, F, df, n)` helper):

```cpp
// The 2026-08 SPY backfill failure mode in miniature: the previous slice's
// total variance crosses the current fit only inside a narrow bump centered
// on the oracle band edge k = -0.50 — a k the legacy repair lattice
// (-0.60 + i * 0.01875) never samples. The bump peak (9e-8) also sits below
// the legacy 1e-7 acceptance, so the legacy loop must serve an oracle-fatal
// crossing; a ConvexRepairSpec pinned to the oracle's Balanced calendar grid
// (65 pts over [-0.50, 0.50], which contains -0.50 exactly) with a 1e-9
// tolerance must repair it.
TEST(VolCurve, ConvexRepairSpecRepairsOffLatticeCalendarCrossing) {
  constexpr double T = 0.10;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;

  const auto base = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  const IVolCurve& base_curve = **base;

  // 1e-9 below baseline everywhere (floor inactive at nodes) except the bump:
  // +9e-8 at k=-0.50, gone by the nearest legacy lattice points at
  // -0.51875 / -0.4875 (exp(-(0.01875/3e-3)^2) ~ 1e-17).
  const auto w_prev = [&base_curve](double k) {
    const double z = (k + 0.50) / 3.0e-3;
    return base_curve.w(k) - 1.0e-9 + 9.0e-8 * std::exp(-z * z);
  };

  const auto legacy = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  const double legacy_crossing = w_prev(-0.50) - (*legacy)->w(-0.50);
  EXPECT_GT(legacy_crossing, 1.0e-8)
      << "fixture must reproduce the failure mode: legacy repair serves a "
         "crossing the admission oracle rejects";

  cfg.convex_repair = ConvexRepairSpec{};
  cfg.convex_repair->k_min = -0.50;
  cfg.convex_repair->k_max = 0.50;
  cfg.convex_repair->grid_points = 65;
  cfg.convex_repair->tolerance = 1.0e-9;
  const auto strict = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(strict.has_value()) << strict.error().to_string();
  const double strict_crossing = w_prev(-0.50) - (*strict)->w(-0.50);
  EXPECT_LE(strict_crossing, 1.0e-8)
      << "strict repair must close the crossing at the oracle grid k";
}

// extra_node_ks must become exact QP floor nodes even at a k on NEITHER the
// legacy lattice NOR the spec grid (here -0.517, outside the spec band):
// this is the mechanism the pricer recovery rung uses to promote the
// oracle's reported violation k's.
TEST(VolCurve, ConvexRepairSpecExtraNodeKsBecomeExactFloorNodes) {
  constexpr double T = 0.10;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;
  const auto base = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  const IVolCurve& base_curve = **base;

  const auto w_prev = [&base_curve](double k) {
    const double z = (k + 0.517) / 2.5e-3;
    return base_curve.w(k) - 1.0e-9 + 9.0e-8 * std::exp(-z * z);
  };

  ConvexRepairSpec spec;
  spec.k_min = -0.50;
  spec.k_max = 0.50;
  spec.grid_points = 65;
  spec.tolerance = 1.0e-9;

  cfg.convex_repair = spec; // grid alone cannot see k=-0.517 (outside band)
  const auto miss = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(miss.has_value()) << miss.error().to_string();
  EXPECT_GT(w_prev(-0.517) - (*miss)->w(-0.517), 1.0e-8);

  spec.extra_node_ks = {-0.517};
  cfg.convex_repair = spec;
  const auto hit = fit_slice_curve(cfg, obs, F, T, df, w_prev);
  ASSERT_TRUE(hit.has_value()) << hit.error().to_string();
  EXPECT_LE(w_prev(-0.517) - (*hit)->w(-0.517), 1.0e-8)
      << "promoted node must carry the w_prev floor exactly";
}

TEST(VolCurve, ConvexRepairSpecInvalidSpecRejected) {
  constexpr double T = 0.10;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  const std::vector<FitObs> obs = make_smile_obs(T, F, df, 15);

  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;
  cfg.convex_repair = ConvexRepairSpec{};
  cfg.convex_repair->grid_points = 1; // inclusive grid needs >= 2 points
  const auto one_point = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_FALSE(one_point.has_value());
  EXPECT_EQ(one_point.error().code, atx::core::ErrorCode::InvalidArgument);

  cfg.convex_repair = ConvexRepairSpec{};
  cfg.convex_repair->k_min = 0.5;
  cfg.convex_repair->k_max = -0.5; // inverted band
  const auto inverted = fit_slice_curve(cfg, obs, F, T, df);
  ASSERT_FALSE(inverted.has_value());
  EXPECT_EQ(inverted.error().code, atx::core::ErrorCode::InvalidArgument);
}
```

Fixture tuning note: the bump amplitudes/widths above are chosen so the legacy path provably misses (nearest lattice point ≥ 6 sigma away) while staying below the legacy 1e-7 acceptance. `make_smile_obs` is deterministic, so once the RED assertions hold locally they hold forever. If `legacy_crossing > 1e-8` does not hold on first run (an ATM-clustered QP node landing inside the bump), narrow the bump (`3.0e-3 → 2.0e-3`) rather than raising the amplitude past 1e-7.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build --preset dev --target atx-vol-tests` then `build/bin/atx-vol-tests.exe --gtest_filter=VolCurve.ConvexRepairSpec*`
Expected: FAIL to compile (`ConvexRepairSpec` undeclared) — that is the RED state for a new-type task.

- [ ] **Step 3: Add `ConvexRepairSpec` + `CurveConfig::convex_repair` to `vol_curve.hpp`**

Immediately before `struct CurveConfig` (~line 410); ensure `<optional>` and `<vector>` are included at the top of the header:

```cpp
// Producer-side override of the ConvexDense shared-k calendar-repair contract.
// Default (`CurveConfig::convex_repair == nullopt`) keeps the historical fixed
// lattice ([-0.60, 0.60], 64 intervals, 1e-7 acceptance) untouched, expression
// for expression. A recovery caller (pricer_fitter's strict rung) pins the
// repair loop to the admission oracle's exact inclusive sampling grid and a
// tolerance strictly inside the oracle's, so a repaired fit can no longer pass
// repair yet fail admission at a k the repair never sampled (the 2026-08 SPY
// backfill failure mode: 181/1890 cells, all sub-vol-tick slack).
// `extra_node_ks` are promoted to exact QP nodes before the first pass — the
// same mechanism the repair loop uses for its own residual crossings — so the
// oracle's reported violation k's can be pinned directly. Plain data on
// purpose: vol_curve must not depend on detail/risk_surface_validation.hpp.
struct ConvexRepairSpec {
  double k_min{-0.60};
  double k_max{0.60};
  // Inclusive point count; k_i = k_min + (i / (grid_points - 1)) * (k_max -
  // k_min), the oracle's sample_k formula verbatim (fraction first — the
  // arithmetic ORDER is part of the contract, both sides must evaluate the
  // same doubles).
  std::uint32_t grid_points{65};
  // Max accepted w_prev(k) - w_curr(k) at a grid k before promotion/refit.
  double tolerance{1.0e-7};
  // Extra exact QP node locations seeded into required_k for every slice.
  std::vector<double> extra_node_ks{};
};
```

Inside `CurveConfig`, after the `spline` member:

```cpp
  // Strict calendar-repair override for the ConvexDense kind; nullopt (the
  // default, and the only value the hot path ever sees) = legacy behavior.
  std::optional<ConvexRepairSpec> convex_repair{};
```

- [ ] **Step 4: Implement the spec path in the ConvexDense branch of `fit_slice_curve`**

In `atx-vol/src/vol_curve.cpp` (branch starts line 367). After the two `constexpr` declarations, insert spec validation and required_k seeding; replace the violation-scan loop with a shared lambda iterated over either grid. The legacy arithmetic (`kRiskCalendarMin + dk * static_cast<double>(gi)` with `dk` precomputed) must remain character-identical:

```cpp
    constexpr double kCalendarTol = 1.0e-7;
    constexpr int kMaxCalendarRefits = 4;

    const ConvexRepairSpec* repair =
        cfg.convex_repair.has_value() ? &*cfg.convex_repair : nullptr;
    if (repair != nullptr &&
        !(std::isfinite(repair->k_min) && std::isfinite(repair->k_max) &&
          repair->k_max > repair->k_min && repair->grid_points >= 2u &&
          std::isfinite(repair->tolerance) && repair->tolerance >= 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "fit_slice_curve: invalid ConvexRepairSpec");
    }
    const double calendar_tol = repair != nullptr ? repair->tolerance : kCalendarTol;

    std::vector<double> required_k(calendar_floor_knots.begin(),
                                   calendar_floor_knots.end());
    if (repair != nullptr && !repair->extra_node_ks.empty()) {
      for (const double k : repair->extra_node_ks) {
        if (std::isfinite(k)) {
          required_k.push_back(k);
        }
      }
      std::sort(required_k.begin(), required_k.end());
      required_k.erase(std::unique(required_k.begin(), required_k.end()),
                       required_k.end());
    }
```

Then inside the pass loop, the scan becomes:

```cpp
      std::vector<double> violations;
      violations.reserve(8);
      const auto scan_k = [&](double k) {
        const double wp = w_prev(k);
        const double wc = fit.iv(k);
        const double w_curr = (std::isfinite(wc) && wc > 0.0) ? wc * wc * T : kNaN;
        if (std::isfinite(wp) && std::isfinite(w_curr) &&
            wp - w_curr > calendar_tol) {
          violations.push_back(k);
        }
      };
      if (repair == nullptr) {
        const double dk = (kRiskCalendarMax - kRiskCalendarMin) /
                          static_cast<double>(kRiskCalendarIntervals);
        for (std::uint32_t gi = 0; gi <= kRiskCalendarIntervals; ++gi) {
          scan_k(kRiskCalendarMin + dk * static_cast<double>(gi));
        }
      } else {
        // The oracle's inclusive sample formula verbatim (fraction first):
        // repair must check the exact doubles admission will evaluate.
        for (std::uint32_t gi = 0; gi < repair->grid_points; ++gi) {
          const double fraction = static_cast<double>(gi) /
                                  static_cast<double>(repair->grid_points - 1u);
          scan_k(repair->k_min + fraction * (repair->k_max - repair->k_min));
        }
      }
```

Everything downstream (empty-violations return, `kMaxCalendarRefits` exhaustion error, promotion + stall detection) is unchanged. Update the header doc-comment on `fit_slice_curve` (vol_curve.hpp:445-449) with one sentence: the shared-k lattice and acceptance are overridable per-fit via `CurveConfig::convex_repair`.

- [ ] **Step 5: Run the new tests to verify they pass**

Run: `cmake --build --preset dev --target atx-vol-tests` then `build/bin/atx-vol-tests.exe --gtest_filter=VolCurve.ConvexRepairSpec*`
Expected: 3/3 PASS. If `ConvexRepairSpecRepairsOffLatticeCalendarCrossing`'s RED-side assertion (`legacy_crossing > 1e-8`) fails, tune the fixture per the note in Step 1.

- [ ] **Step 6: Run the full vol suite (hot-path regression gate)**

Run: `build/bin/atx-vol-tests.exe --gtest_brief=1`
Expected: all tests pass, including `DenseSlice.CalendarFloorSlackIsBitIdentical` — the nullopt path must be arithmetically untouched.

- [ ] **Step 7: Commit**

```bash
git add atx-vol/include/atx/vol/vol_curve.hpp atx-vol/src/vol_curve.cpp atx-vol/tests/vol_curve_test.cpp
git commit -m "feat(vol): ConvexRepairSpec — oracle-grid calendar repair override for ConvexDense"
```

---

### Task 2: `detail/convex_recovery` — pure oracle→spec bridge helpers

**Files:**
- Modify: `atx-vol/include/atx/vol/detail/risk_surface_validation.hpp` (hoist sample formula)
- Modify: `atx-vol/src/risk_surface_validation.cpp` (delegate `sample_k` to the hoisted function)
- Create: `atx-vol/include/atx/vol/detail/convex_recovery.hpp`
- Create: `atx-vol/src/convex_recovery.cpp`
- Create: `atx-vol/tests/convex_recovery_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `src/convex_recovery.cpp` next to the existing `src/risk_surface_validation.cpp` entry in the atx-vol library source list)
- Modify: `atx-vol/tests/CMakeLists.txt` (add `convex_recovery_test.cpp` to the `atx-vol-tests` source list at line 1)

**Interfaces:**
- Consumes: `ConvexRepairSpec` (Task 1), `RiskSurfaceValidationConfig` + `ValidationDigest` + `ValidationFailure` (existing: `detail/risk_surface_validation.hpp`, `surface_policy.hpp`).
- Produces (namespace `atx::vol::detail`):
  - `double validation_grid_k(const RiskSurfaceValidationConfig&, std::uint32_t point, std::uint32_t n_points) noexcept` (in risk_surface_validation.hpp)
  - `bool should_attempt_strict_recovery(ValidationFailure failures) noexcept`
  - `ConvexRepairSpec make_strict_repair_spec(const RiskSurfaceValidationConfig&)`
  - `std::vector<double> strict_promotion_ks(const ValidationDigest&, const RiskSurfaceValidationConfig&)`
  Task 3 calls all four.

- [ ] **Step 1: Write the failing tests**

Create `atx-vol/tests/convex_recovery_test.cpp`:

```cpp
#include "atx/vol/detail/convex_recovery.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

using detail::make_strict_repair_spec;
using detail::should_attempt_strict_recovery;
using detail::strict_promotion_ks;
using detail::validation_grid_k;

ValidationFailure mask(std::uint32_t bits) {
  return static_cast<ValidationFailure>(bits);
}

// The five masks observed across all 181 backfill rejections must qualify;
// anything carrying a non-geometric bit (beyond CarryGap) must not.
TEST(ConvexRecovery, ShouldAttemptMatchesObservedRejectionMasks) {
  EXPECT_TRUE(should_attempt_strict_recovery(mask(2080)));  // CarryGap|Calendar
  EXPECT_TRUE(should_attempt_strict_recovery(mask(2064)));  // CarryGap|Butterfly
  EXPECT_TRUE(should_attempt_strict_recovery(mask(2096)));  // CarryGap|Cal|Bfly
  EXPECT_TRUE(should_attempt_strict_recovery(mask(32)));    // Calendar
  EXPECT_TRUE(should_attempt_strict_recovery(mask(16)));    // Butterfly

  EXPECT_FALSE(should_attempt_strict_recovery(mask(0)));
  EXPECT_FALSE(should_attempt_strict_recovery(mask(2048))); // CarryGap alone
  EXPECT_FALSE(should_attempt_strict_recovery(mask(1024))); // InsufficientData
  EXPECT_FALSE(should_attempt_strict_recovery(mask(2080 | 1)));  // +InvalidDomain
  EXPECT_FALSE(should_attempt_strict_recovery(mask(16 | 64)));   // +Wing
  EXPECT_FALSE(should_attempt_strict_recovery(mask(32 | 256)));  // +TimedOut
}

TEST(ConvexRecovery, StrictSpecPinsOracleCalendarGridAndTightensTolerance) {
  RiskSurfaceValidationConfig config; // defaults: +/-0.50, 129 pts, 1e-8
  const ConvexRepairSpec spec = make_strict_repair_spec(config);
  EXPECT_EQ(spec.k_min, config.k_min);
  EXPECT_EQ(spec.k_max, config.k_max);
  EXPECT_EQ(spec.grid_points, config.calendar_grid_points);
  EXPECT_DOUBLE_EQ(spec.tolerance,
                   0.1 * config.calendar_total_variance_tolerance);
  EXPECT_TRUE(spec.extra_node_ks.empty());
}

TEST(ConvexRecovery, PromotionTakesCalendarKVerbatim) {
  RiskSurfaceValidationConfig config;
  ValidationDigest digest;
  digest.n_calendar_violations = 3;
  digest.first_calendar_k = -0.50;
  const std::vector<double> ks = strict_promotion_ks(digest, config);
  ASSERT_EQ(ks.size(), 1u);
  EXPECT_EQ(ks[0], -0.50);
}

TEST(ConvexRecovery, PromotionStraddlesButterflyKWithStrikeGridNeighbors) {
  RiskSurfaceValidationConfig config; // strike grid: 129 pts over [-0.5, 0.5]
  ValidationDigest digest;
  digest.n_butterfly_violations = 1;
  // Between grid points 63 and 64: neighbors 62..65 plus the k itself.
  digest.first_butterfly_k = validation_grid_k(config, 63, 129) + 1.0e-4;
  const std::vector<double> ks = strict_promotion_ks(digest, config);
  ASSERT_EQ(ks.size(), 5u);
  EXPECT_EQ(ks[0], validation_grid_k(config, 62, 129));
  EXPECT_EQ(ks[1], validation_grid_k(config, 63, 129));
  EXPECT_EQ(ks[2], digest.first_butterfly_k);
  EXPECT_EQ(ks[3], validation_grid_k(config, 64, 129));
  EXPECT_EQ(ks[4], validation_grid_k(config, 65, 129));
}

TEST(ConvexRecovery, PromotionClampsAtBandEdgeAndDropsNonFinite) {
  RiskSurfaceValidationConfig config;
  ValidationDigest digest;
  digest.n_butterfly_violations = 1;
  digest.first_butterfly_k = config.k_min; // index 0: neighbors -1 dropped
  std::vector<double> ks = strict_promotion_ks(digest, config);
  ASSERT_GE(ks.size(), 2u);
  EXPECT_EQ(ks.front(), config.k_min);
  for (std::size_t i = 1; i < ks.size(); ++i) {
    EXPECT_GT(ks[i], ks[i - 1]); // sorted, deduplicated
  }

  ValidationDigest empty; // counts zero / firsts NaN => nothing to promote
  EXPECT_TRUE(strict_promotion_ks(empty, config).empty());
}

} // namespace
} // namespace atx::vol
```

Check `ValidationDigest`'s default member values in `surface_policy.hpp` first: if `first_calendar_k`/`first_butterfly_k` default to 0.0 rather than NaN, the guards in `strict_promotion_ks` rely on the violation COUNTS, which the last test already exercises.

- [ ] **Step 2: Register the test file and run to verify failure**

Add `convex_recovery_test.cpp` to `atx-vol/tests/CMakeLists.txt`'s `add_executable(atx-vol-tests ...)` list. Run `cmake --build --preset dev --target atx-vol-tests`.
Expected: FAIL to compile (`convex_recovery.hpp` not found).

- [ ] **Step 3: Hoist the oracle sample formula into `risk_surface_validation.hpp`**

In the header, after `RiskSurfaceValidationConfig` (line 69), inside a `namespace detail {}` block:

```cpp
namespace detail {
// The oracle's inclusive uniform sample formula, exported so the producer-side
// strict-recovery path can repair on EXACTLY the doubles the oracle evaluates.
// Fraction first — reordering the arithmetic can move a sample by an ulp, and
// both sides of the producer/oracle contract must agree bit-for-bit.
[[nodiscard]] inline double validation_grid_k(const RiskSurfaceValidationConfig& config,
                                              std::uint32_t point,
                                              std::uint32_t n_points) noexcept {
  const double fraction =
      static_cast<double>(point) / static_cast<double>(n_points - 1u);
  return config.k_min + fraction * (config.k_max - config.k_min);
}
} // namespace detail
```

In `risk_surface_validation.cpp`, replace the body of the file-local `sample_k` (line ~38-42) with a call to `detail::validation_grid_k(config, point, n_points)` — do not change any call site. Run the existing oracle tests: `build/bin/atx-vol-tests.exe --gtest_filter=*RiskSurface*:*Validation*` — all green (formula identical).

- [ ] **Step 4: Create `convex_recovery.hpp` / `.cpp`**

`atx-vol/include/atx/vol/detail/convex_recovery.hpp`:

```cpp
#pragma once

// Strict convex-dense recovery: the producer-side bridge between the admission
// oracle's sampling contract (RiskSurfaceValidationConfig) and the ConvexDense
// calendar-repair override (ConvexRepairSpec). Root cause (2026-08 SPY
// backfill, 181/1890 cells): the repair loop's fixed lattice and 1e-7
// acceptance are strictly looser than the oracle's grid and 1e-8 tolerance, so
// a fit could pass repair yet die at admission by a sub-vol-tick margin at a k
// repair never sampled. These helpers are pure so that contract is
// unit-testable without a fitter.

#include <vector>

#include "atx/vol/detail/risk_surface_validation.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol::detail {

// Recovery applies only when the candidate died on geometry the strict refit
// can actually cure: at least one of Butterfly/Calendar set and nothing else
// outside {Butterfly, Calendar, CarryGap}. CarryGap alone publishes Degraded
// without help; InvalidDomain / InsufficientData / TimedOut / Wing and the
// rest need different medicine, and a refit under those masks would only burn
// the rejection budget.
[[nodiscard]] bool should_attempt_strict_recovery(ValidationFailure failures) noexcept;

// The oracle's calendar band/grid verbatim, acceptance pinned to 0.1x the
// oracle's tolerance so post-repair QP roundoff cannot straddle the gate.
[[nodiscard]] ConvexRepairSpec
make_strict_repair_spec(const RiskSurfaceValidationConfig& config);

// Exact-QP-node promotions for one recovery round, from the digest's reported
// first violations: the calendar k verbatim (repair's own grid scan handles
// the rest of the calendar band); the butterfly k plus its two straddling
// uniform strike-grid neighbors on each side — the finite-difference stencil
// the oracle measured the kink on, which is what forces the QP to be convex
// across the intrinsic-bound seam. Sorted, deduplicated, non-finite dropped.
[[nodiscard]] std::vector<double>
strict_promotion_ks(const ValidationDigest& digest,
                    const RiskSurfaceValidationConfig& config);

} // namespace atx::vol::detail
```

`atx-vol/src/convex_recovery.cpp`:

```cpp
#include "atx/vol/detail/convex_recovery.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace atx::vol::detail {

bool should_attempt_strict_recovery(ValidationFailure failures) noexcept {
  const auto bits = static_cast<std::uint32_t>(failures);
  constexpr auto geometric = static_cast<std::uint32_t>(ValidationFailure::Butterfly) |
                             static_cast<std::uint32_t>(ValidationFailure::Calendar);
  constexpr auto recoverable =
      geometric | static_cast<std::uint32_t>(ValidationFailure::CarryGap);
  return (bits & geometric) != 0u && (bits & ~recoverable) == 0u;
}

ConvexRepairSpec make_strict_repair_spec(const RiskSurfaceValidationConfig& config) {
  ConvexRepairSpec spec;
  spec.k_min = config.k_min;
  spec.k_max = config.k_max;
  spec.grid_points = config.calendar_grid_points;
  spec.tolerance = 0.1 * config.calendar_total_variance_tolerance;
  return spec;
}

std::vector<double> strict_promotion_ks(const ValidationDigest& digest,
                                        const RiskSurfaceValidationConfig& config) {
  std::vector<double> ks;
  if (digest.n_calendar_violations > 0u && std::isfinite(digest.first_calendar_k)) {
    ks.push_back(digest.first_calendar_k);
  }
  if (digest.n_butterfly_violations > 0u && std::isfinite(digest.first_butterfly_k)) {
    const double k = digest.first_butterfly_k;
    ks.push_back(k);
    const std::uint32_t n = config.strike_grid_points;
    const double span = config.k_max - config.k_min;
    if (n >= 2u && span > 0.0 && k >= config.k_min && k <= config.k_max) {
      const double step = span / static_cast<double>(n - 1u);
      const auto idx = static_cast<std::int64_t>(std::floor((k - config.k_min) / step));
      for (std::int64_t i = idx - 1; i <= idx + 2; ++i) {
        if (i >= 0 && i < static_cast<std::int64_t>(n)) {
          ks.push_back(validation_grid_k(config, static_cast<std::uint32_t>(i), n));
        }
      }
    }
  }
  std::sort(ks.begin(), ks.end());
  ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
  return ks;
}

} // namespace atx::vol::detail
```

Add `src/convex_recovery.cpp` to the atx-vol library source list in `atx-vol/CMakeLists.txt` (grep for `risk_surface_validation.cpp` and add the new file beside it).

- [ ] **Step 5: Run the new tests to verify they pass**

Run: `cmake --build --preset dev --target atx-vol-tests` then `build/bin/atx-vol-tests.exe --gtest_filter=ConvexRecovery.*`
Expected: 5/5 PASS. If enum-value assumptions (2080 etc.) fail, re-check `ValidationFailure` bit values in `surface_policy.hpp:66-89` — the masks come from production logs and are load-bearing.

- [ ] **Step 6: Run the full vol suite**

Run: `build/bin/atx-vol-tests.exe --gtest_brief=1`
Expected: all green (the sample_k delegation must be behavior-identical).

- [ ] **Step 7: Commit**

```bash
git add atx-vol/include/atx/vol/detail/convex_recovery.hpp atx-vol/src/convex_recovery.cpp \
        atx-vol/tests/convex_recovery_test.cpp atx-vol/include/atx/vol/detail/risk_surface_validation.hpp \
        atx-vol/src/risk_surface_validation.cpp atx-vol/CMakeLists.txt atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): convex_recovery helpers — oracle-contract bridge for strict dense refit"
```

---

### Task 3: Strict-recovery rung in `PricerFitter::fit` + counters

**Files:**
- Modify: `atx-vol/src/pricer_fitter.cpp` (insert recovery block between the fallback-ladder block ending at line 1428 and `risk_health_ = admission.health;` at line 1429; add `#include "atx/vol/detail/convex_recovery.hpp"`)
- Modify: `atx-vol/include/atx/vol/detail/counters.hpp` (two new `Counter` enum entries + matching name-table entries — pattern-match how `ConvexDenseWingAnchorBuilds` is registered)
- Test: `atx-vol/tests/pricer_fitter_test.cpp`

**Interfaces:**
- Consumes: `detail::should_attempt_strict_recovery`, `detail::make_strict_repair_spec`, `detail::strict_promotion_ks` (Task 2); `CurveConfig::convex_repair` (Task 1); existing in-scope locals at the insertion point: `in`, `sess`, `digest`, `admission`, `report`, `prior`, `validation_config`, `quality_mode`, `chain`, `under`, `validate_candidate`, `admission_attempt`, `decision_`, `selection_`, `timings_`, and helpers `failed_attempt_report`, `decide_risk_surface_admission`, `VolaSession::build`, `elapsed_ms`, `Clock`.
- Produces: counters `RiskStrictRecoveryRounds`, `RiskStrictRecoveryAdmitted`; recovery behavior observable in `SurfaceBuildReport::attempts` (one extra attempt entry per round) and, on success, `FitDecision::used_fallback == true` with `decision_->curve.convex_repair` set. Task 4 measures this end to end.

- [ ] **Step 1: Add the two counters**

In `atx-vol/include/atx/vol/detail/counters.hpp`, add `RiskStrictRecoveryRounds` and `RiskStrictRecoveryAdmitted` to the `Counter` enum and its name table, following the file's existing registration pattern exactly (every enum entry has a matching printable name; `kCount` must stay consistent). Build check: `cmake --build --preset dev --target atx-vol-tests` compiles.

- [ ] **Step 2: Write the regression-guard test**

Add to `atx-vol/tests/pricer_fitter_test.cpp`, reusing the file's existing successful-fit fixture (the one behind `FitStoresSurfaceAndGatesValueChain` — same chain/config helpers):

```cpp
// A clean, admitted fit must never enter the strict-recovery rung: the block
// is rejection-gated, so the hot path pays nothing for it. Counter-observable
// only in ATX_VOL_COUNTERS builds; the publication asserts run everywhere.
TEST(PricerFitter, AdmittedFitNeverEntersStrictRecovery) {
  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  // ... construct the standard successful fixture exactly as
  // FitStoresSurfaceAndGatesValueChain does, run fit(), assert Ok ...
  if constexpr (counters::counters_enabled()) {
    const auto snap = counters::snapshot();
    EXPECT_EQ(snap.values[static_cast<unsigned>(
                  counters::Counter::RiskStrictRecoveryRounds)], 0u);
  }
}
```

(The implementer copies the fixture-construction lines from the existing test rather than referencing them — tests must stand alone. Adapt the snapshot-index expression to the actual `Snapshot` API in counters.hpp.)

- [ ] **Step 3: Run to verify the test fails to compile** (counters exist but the test file edit is new — expected outcome here is simply a clean compile and PASS, since the recovery block isn't needed for this guard; treat this step as fixture verification.)

Run: `build/bin/atx-vol-tests.exe --gtest_filter=PricerFitter.AdmittedFitNeverEntersStrictRecovery`
Expected: PASS (guard is meaningful once the block lands in Step 4 — it must STAY green).

- [ ] **Step 4: Insert the strict-recovery block**

In `pricer_fitter.cpp`, immediately after the fallback-ladder block's closing brace (line 1428) and before `risk_health_ = admission.health;`:

```cpp
  // Strict convex-dense recovery — the rung after the last rung. Reached only
  // when every ladder model was rejected and the digest names pure geometry
  // (Butterfly/Calendar, optionally CarryGap). Root cause (2026-08 SPY
  // backfill): the dense repair loop's fixed lattice + 1e-7 acceptance are
  // strictly looser than this oracle's grid + 1e-8, so marginal sub-vol-tick
  // crossings pass repair and die here. The refit pins repair to the oracle's
  // exact calendar grid at 0.1x its tolerance and promotes the digest's
  // reported violation k's to exact QP nodes; each round's new firsts feed
  // the next round. Admitted fits never reach this block, so the hot path is
  // unchanged.
  if (!admission.publish_candidate && detail::should_attempt_strict_recovery(digest.failures)) {
    constexpr int kMaxStrictRecoveryRounds = 3;
    ConvexRepairSpec spec = detail::make_strict_repair_spec(validation_config);
    ValidationDigest round_digest = digest;
    for (int round = 0; round < kMaxStrictRecoveryRounds; ++round) {
      const std::vector<double> promoted =
          detail::strict_promotion_ks(round_digest, validation_config);
      const std::size_t before = spec.extra_node_ks.size();
      spec.extra_node_ks.insert(spec.extra_node_ks.end(), promoted.begin(), promoted.end());
      std::sort(spec.extra_node_ks.begin(), spec.extra_node_ks.end());
      spec.extra_node_ks.erase(
          std::unique(spec.extra_node_ks.begin(), spec.extra_node_ks.end()),
          spec.extra_node_ks.end());
      if (round > 0 && spec.extra_node_ks.size() == before) {
        break; // no new violation k's — an identical refit cannot converge
      }
      SessionInputs strict_inputs = in;
      strict_inputs.curve.kind = VolCurveKind::ConvexDense;
      strict_inputs.curve.convex.node_cap = strict_inputs.calib.max_obs_per_slice;
      strict_inputs.curve.convex_repair = spec;
      ATX_VOL_COUNT(RiskStrictRecoveryRounds);
      const auto strict_start = Clock::now();
      Result<VolaSession> strict = VolaSession::build(chain.underlying(), strict_inputs);
      timings_.risk_build_ms += elapsed_ms(strict_start);
      if (!strict.has_value()) {
        report.attempts.push_back(
            failed_attempt_report(under, strict_inputs.curve, strict.error()));
        break; // the strict QP itself failed — nothing further to promote
      }
      ValidationDigest strict_digest = validate_candidate(*strict);
      SurfaceBuildAttemptReport strict_attempt = admission_attempt(*strict, strict_digest);
      AdmissionDecision strict_admission = decide_risk_surface_admission(
          strict_digest, quality_mode, candidate_generation_, prior, cfg_.fallback);
      report.attempts.push_back(std::move(strict_attempt));
      if (strict_admission.publish_candidate) {
        ATX_VOL_COUNT(RiskStrictRecoveryAdmitted);
        // Same provenance contract as the ladder adoption above: the first
        // rejected primary of this fit stays authoritative.
        const CurveConfig rejected_primary = in.curve;
        in = std::move(strict_inputs);
        sess = std::move(*strict);
        digest = strict_digest;
        admission = strict_admission;
        if (decision_.has_value()) {
          if (!decision_->used_fallback) {
            decision_->primary_curve = rejected_primary;
          }
          decision_->used_fallback = true;
          decision_->curve = in.curve;
        }
        if (selection_.has_value()) {
          selection_->chosen = in.curve;
        }
        break;
      }
      if (!detail::should_attempt_strict_recovery(strict_digest.failures)) {
        break; // the strict refit surfaced a non-geometric failure — stop
      }
      round_digest = strict_digest;
    }
  }
```

Add `#include "atx/vol/detail/convex_recovery.hpp"` with the file's other detail includes (near line 24).

- [ ] **Step 5: Build and run the full vol suite**

Run: `cmake --build --preset dev --target atx-vol-tests` then `build/bin/atx-vol-tests.exe --gtest_brief=1`
Expected: all green, including the Step 2 guard and all existing pricer_fitter/risk-validation tests (rejection-path tests like `RejectedCandidateKeepsLastAdmittedGeneration` exercise digests whose masks — e.g. containing NonFinite or Wing — must NOT trigger recovery; if any such test now publishes where it previously rejected, the `should_attempt_strict_recovery` gate is too loose — stop and re-check, do not weaken the test).

- [ ] **Step 6: Commit**

```bash
git add atx-vol/src/pricer_fitter.cpp atx-vol/include/atx/vol/detail/counters.hpp atx-vol/tests/pricer_fitter_test.cpp
git commit -m "feat(vol): strict convex-dense recovery rung after admission rejection"
```

---

### Task 4: Real-data replay — measure recovery on the 181 failed backfill cells

Validation-only task: no repo source edits. Produces a report file; the acceptance gate is measured recovery on real production rejections.

**Files:**
- Create: `docs/superpowers/plans/2026-08-02-convex-dense-admission-recovery-replay.md` (the results report)
- Scratch outputs (NOT committed, NOT in production roots): throwaway DB roots + logs under `C:/atx-data/surface-db/recovery-replay/` and `C:/atx-data/logs/recovery-replay/`

**Interfaces:**
- Consumes: Tasks 1-3 merged into the working branch; production read-only inputs `C:/atx-data/opra-hive` (1890 SPY sessions), prior build logs `C:\atx-data\logs\spy-backfill\` and `C:\atx-data\logs\spy-backfill-2019\` (`build_*.csv` failure sections + `orchestrator.log` with full command lines).
- Produces: recovery-rate numbers per mask/year in the report file.

- [ ] **Step 1: Build release binaries with the fix**

Run: `cmake --build --preset rel --target atx-vol-surface-db-build atx-vol-surface-db`
Expected: `build-rel/bin/atx-vol-surface-db-build.exe` and `build-rel/bin/atx-vol-surface-db.exe` build clean.

- [ ] **Step 2: Extract the failed-cell list from the prior run's logs**

Write and run a small Python script (temp location fine) that replicates the known-good parsing: for each `build_*.csv` in the two log dirs, rows between the `date` header row and the `regression_date` row are per-cell failures; dedupe on `(date, symbol)`; keep `date`, `symbol`, and the `mask=NNNN` value from the detail column. Write `C:/atx-data/logs/recovery-replay/failed_cells.csv`.
Expected: exactly 181 unique cells, mask histogram {2080: 117, 2064: 40, 32: 16, 16: 6, 2096: 2}.

- [ ] **Step 3: Replay every failed date against the new binary**

For each failed date, re-run the build using the EXACT prior command line for the chunk containing that date, copied from `orchestrator.log` (this preserves `--snap-et`/`--snapshot-suffix` DST handling, rates file, and fit flags), with only these substitutions:
- `--build-exe` (or the exe path itself) → `c:/atx/build-rel/bin/atx-vol-surface-db-build.exe`
- DB root → a fresh root under `C:/atx-data/surface-db/recovery-replay/` (one per year, mirroring the per-year layout)
- date range → the single failed date (or minimal chunks batching failed dates that share a snapshot minute)
- log dir → `C:/atx-data/logs/recovery-replay/`
Never pass a production `--db` path. The hive is read-only input.
Expected: builds exit 0; new `build_*.csv` files appear in the replay log dir.

- [ ] **Step 4: Score recovery**

Parse the replay `build_*.csv` files with the Step 2 parser: a previously-failed cell counts RECOVERED if it no longer appears in the failure section (and the date's cell count in the replay DB root, via `atx-vol-surface-db.exe` verify/stats on that root, shows the SPY surface present). Produce per-mask and per-year recovery counts.
Acceptance gate: ≥ 60% of the 181 cells recovered overall, and ≥ 75% of the calendar-driven masks (2080, 32, 2096). Expect butterfly masks (2064, 16) to lag — the intrinsic-seam promotion is best-effort by design. Any cell that now fails with a NEW mask (bits outside {16,32,2048,2096,2064,2080}) is a regression: investigate before proceeding.

- [ ] **Step 5: Control replay — no regression on previously-good cells**

Pick 5 previously-successful dates spanning 2019/2022/2026 (any dates absent from `failed_cells.csv`). Replay them the same way into the throwaway root.
Expected: all 5 fit and admit exactly as before (present in replay DB, absent from failure sections). Since recovery is rejection-gated, these cells never enter the new code path.

- [ ] **Step 6: Write the report and commit**

Write `docs/superpowers/plans/2026-08-02-convex-dense-admission-recovery-replay.md`: mask/year recovery table, the residual-failure list with masks, control-set outcome, and wall-clock cost of the replay. Commit:

```bash
git add docs/superpowers/plans/2026-08-02-convex-dense-admission-recovery-replay.md
git commit -m "docs(vol): convex-dense strict-recovery replay results on 2019-2026 SPY backfill rejections"
```

Leave the throwaway DB roots on disk for inspection; note their paths in the report.
