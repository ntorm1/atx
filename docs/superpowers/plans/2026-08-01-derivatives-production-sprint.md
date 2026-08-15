# derivatives.hpp Production Sprint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete atx-vol's vol-derivatives module to production quality — capped variance/vol swaps, mid-life vol swaps, full greeks, strip error estimate, dated fixing tracker — and wire it into portfolio pricing and the backtest engine.

**Architecture:** The existing model-free variance strip (`derivatives.cpp`, shared `strip_grid.hpp` conventions) stays the fair-strike engine. A new lognormal realized-variance distribution engine (the reserved `DerivEngine::RvDistributionProxy`) supplies everything nonlinear in realized variance: mid-life vol swaps, capped var swaps (closed-form displaced Black), capped vol swaps (Gauss–Hermite). Greeks come from finite-difference bumps through the same pricing path using sticky-strike respot/vol-shift wrapper views. Portfolio wiring is a new additive `DerivBook` module (does NOT touch the bit-identity-pinned `Portfolio`/`PreparedPortfolio` option pipeline). Backtest wiring is an additive swap-lot lane in `PortfolioState` with engine-owned accrual state, its own P&L lane, and checkpoint support.

**Tech Stack:** C++20, clang-cl 18, CMake presets + `scripts\atx-build.ps1`, GoogleTest, `atx::core::Result<T>/Status` error model.

## Global Constraints

- Toolchain gates: `/W4 /permissive- /WX`; clang-format enforced; clang-tidy DISABLED (do not run).
- TDD mandatory: failing test first, then implementation. New tests go in `atx-vol/tests/`, added to `atx-vol/tests/CMakeLists.txt` explicit list (append at end with a workstream comment).
- Error model: `Result<T>`/`Status` via `types.hpp`; no exceptions for control flow; every non-void return `[[nodiscard]]`.
- Uncomputed values are **NaN, never 0.0** (portfolio convention). `integration_error_est` keeps NaN = not-estimated sentinel.
- Units: decimal variance internally (0.04 ⇔ 20 vol); annualization default 252; `T` year-fraction; `k = ln(K/F)`; `w = σ²T`.
- Thread-safety contract of `derivatives.hpp` pricing entries (stateless pure functions) must be preserved. No allocation in per-call hot paths beyond what already exists.
- Backward compatibility: zero-swap books and existing `deriv_price` behavior on uncapped, unaged/fully-aged contracts must be numerically UNCHANGED (existing tests pin this).
- Naming: `PascalCase` types, `snake_case` functions, `kPascalCase` constants, `enum class : std::uint8_t`, trailing-underscore members, `namespace atx::vol`.
- Build/iterate: `powershell scripts\atx-build.ps1 check atx-vol\src\derivatives.cpp` while shaping; `powershell scripts\atx-build.ps1 build atx-vol-tests`; `powershell scripts\atx-build.ps1 -Ctest -R <Suite>`. Full-suite + `hygiene` preset only at gate time.
- Work in a leased pool worktree: `powershell scripts\lease-worktree.ps1 -Branch feat/deriv-prod-sprint-<run-slug> -Base <frozen-sha> -Agent deriv-prod -RunId <run-id> -HeartbeatId <run-unique-heartbeat> -MaxPool 20`. Pulse around long commands and release with the same run ID. Commit per task, message style `feat(vol): ...` / `fix(vol): ...`, trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Reference math (shared by Tasks 2–6)

Blended total annualized variance of a contract at valuation: `V = a + b·W` where
`a = w_done·rv_done_dec`, `b = w_future = 1 − w_done`, `w_done = n_done/n_total`, and `W` is the
(annualized) future-leg variance, modeled lognormal with mean `m = K_var_future` (from the strip)
and log-stdev `s = ξ·√τ` (`ξ` = vol-of-vol, `τ = maturity_t`).

- Lognormal call: `E[(W−K)+] = m·Φ(d1) − K·Φ(d2)`, `d1 = (ln(m/K) + s²/2)/s`, `d2 = d1 − s`.
- `E[√W] = √m · exp(−s²/8)` (exact lognormal).
- Carr–Lee consistency calibration at inception: `ξ` solves `√K_var·exp(−s²/8) = K_vol_CL`
  ⇒ `s² = −8·ln(K_vol_CL/√K_var)`, `ξ = s/√τ`; guard `K_vol_CL ≥ √K_var` ⇒ `ξ = 0`.
- Capped var swap: `E[min(V,C)] = V̄ − b·E[(W − K_c)+]` with `K_c = (C−a)/b`, `V̄ = a + b·m`.
  If `C ≤ a` (accrued already exceeds cap): `E[min(V,C)] = C` — pinned.
- Capped vol swap payoff `min(√V, c) = √min(V, c²)` (√ monotone) — Gauss–Hermite.
- Gauss–Hermite (physicists'): `E[g(W)] = (1/√π)·Σᵢ wᵢ·g(m·exp(s√2·xᵢ − s²/2))`.

---

### Task 1: Strip Richardson error estimate

**Files:**
- Modify: `atx-vol/src/derivatives.cpp` (strip loop, ~line 371-431)
- Test: `atx-vol/tests/derivatives_test.cpp`

**Interfaces:**
- Produces: `DerivQuote::integration_error_est` finite (≥ 0) whenever the strip runs with a grid where `(n_nodes % 4) == 1`; stays NaN otherwise. No signature changes.

- [ ] **Step 1: Write the failing test** (append to `derivatives_test.cpp`)

```cpp
// Richardson half-grid estimate: |I_h - I_2h|/15. Finite and small on a smooth
// flat-vol integrand at every tier whose node count is 4m+1; and the estimate
// must bound the actual flat-vol truth error at Standard.
TEST(VarStrip, IntegrationErrorEstimate_FiniteAndBoundsFlatVolError) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;  // 257 = 4*64+1 nodes
  const auto q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_TRUE(q.has_value());
  ASSERT_TRUE(std::isfinite(q->integration_error_est)) << "estimate not populated";
  EXPECT_GE(q->integration_error_est, 0.0);
  // Estimate is in K_var units and should be tiny on a flat surface.
  EXPECT_LT(q->integration_error_est, 1.0e-6);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `powershell scripts\atx-build.ps1 build atx-vol-tests` then `powershell scripts\atx-build.ps1 -Ctest -R VarStrip.IntegrationErrorEstimate`
Expected: FAIL — `integration_error_est` is NaN today.

- [ ] **Step 3: Implement.** In the strip loop of `var_swap_fair_strike` (derivatives.cpp:378-397), accumulate a second Simpson sum over the half grid (every other node), then Richardson:

```cpp
  const bool halvable = (n % 4u) == 1u;  // half grid (n+1)/2 is odd again
  const std::size_t n_half = (n + 1) / 2;
  double integral_half = 0.0;
  // inside the existing node loop, after `integral += ...`:
  //   if (halvable && (i % 2u) == 0u) {
  //     integral_half += simpson_w(i / 2, n_half) * integrand;
  //   }
  // after the loop:
  integral *= (dx / 3.0);
  double err_est = kNaN;
  if (halvable) {
    integral_half *= (2.0 * dx / 3.0);
    err_est = std::fabs((2.0 / T) * (integral - integral_half)) / 15.0;
  }
  // ... out.integration_error_est = err_est;
```

All four tier node counts (97, 257, 769, 2049) are 4m+1. The adaptive node-count rescale (FIX-E M-7 block, ~line 364) must round to 4m+1: after `strip::odd_nodes(...)`, add `if ((grid.n_nodes % 4u) != 1u) grid.n_nodes += 2;`. A caller-pinned `strip_nodes` is NOT rounded beyond the existing odd-forcing — if the result is not 4m+1 the estimate simply stays NaN (a pinned grid is a request).

- [ ] **Step 4: Run tests to verify pass** — same commands; also `-Ctest -R VarStrip` (no regressions: the adaptive-wing tests pin K_var values, node-count rounding must not move them beyond their stated tolerances).

- [ ] **Step 5: Commit** `feat(vol): estimate strip quadrature error via Richardson half-grid`

---

### Task 2: Gauss–Hermite quadrature + lognormal RV distribution helper

**Files:**
- Create: `atx-vol/include/atx/vol/detail/rv_lognormal.hpp`
- Create: `atx-vol/src/detail/rv_lognormal.cpp` (add to `atx-vol/CMakeLists.txt` library list next to `src/derivatives.cpp`)
- Test: `atx-vol/tests/deriv_distribution_test.cpp` (new; append to `atx-vol/tests/CMakeLists.txt` with comment `# deriv production sprint`)

**Interfaces:**
- Produces (namespace `atx::vol::detail`):

```cpp
// Fixed-order Gauss–Hermite rule (physicists' weight exp(-x^2)). Nodes are
// computed once at first use (Newton on the Hermite recurrence) and cached in a
// function-local static array — no per-call allocation.
inline constexpr std::size_t kGhOrder = 21;
struct GhRule { std::array<double, kGhOrder> x; std::array<double, kGhOrder> w; };
[[nodiscard]] const GhRule& gh_rule() noexcept;

// E[g(W)] for W lognormal with mean m and log-stdev s, by Gauss–Hermite.
// g is any callable double -> double. s == 0 collapses to g(m) exactly.
template <class G>
[[nodiscard]] double lognormal_expect(double m, double s, G&& g) noexcept;

// Exact lognormal moments/identities used by the engine and as test oracles.
[[nodiscard]] double lognormal_sqrt_moment(double m, double s) noexcept;   // sqrt(m)*exp(-s^2/8)
[[nodiscard]] double lognormal_call(double m, double s, double k) noexcept; // E[(W-k)+]
[[nodiscard]] double norm_cdf(double x) noexcept;                           // 0.5*erfc(-x/sqrt2)
```

- `lognormal_call` handles `k <= 0` → `m − k`; `s <= 0` → `max(m − k, 0)`.
- `lognormal_expect` evaluates `g(m·exp(s·√2·xᵢ − s²/2))`, weights `wᵢ/√π`.

- [ ] **Step 1: Write the failing tests**

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include "atx/vol/detail/rv_lognormal.hpp"

namespace {
using atx::vol::detail::gh_rule;
using atx::vol::detail::lognormal_call;
using atx::vol::detail::lognormal_expect;
using atx::vol::detail::lognormal_sqrt_moment;
using atx::vol::detail::norm_cdf;

TEST(GhRule, WeightsIntegrateGaussianMoments) {
  const auto& r = gh_rule();
  double s0 = 0.0, s2 = 0.0;
  for (std::size_t i = 0; i < r.x.size(); ++i) {
    s0 += r.w[i];
    s2 += r.w[i] * r.x[i] * r.x[i];
  }
  const double rt_pi = std::sqrt(std::acos(-1.0));
  EXPECT_NEAR(s0, rt_pi, 1e-12);            // ∫e^{-x²} = √π
  EXPECT_NEAR(s2, 0.5 * rt_pi, 1e-12);      // ∫x²e^{-x²} = √π/2
}

TEST(RvLognormal, ExpectRecoversMeanAndSqrtMoment) {
  const double m = 0.04, s = 0.45;
  const double mean = lognormal_expect(m, s, [](double w) { return w; });
  EXPECT_NEAR(mean, m, 1e-9 * m);           // E[W] = m by construction
  const double sq = lognormal_expect(m, s, [](double w) { return std::sqrt(w); });
  EXPECT_NEAR(sq, lognormal_sqrt_moment(m, s), 1e-10);
}

TEST(RvLognormal, CallMatchesQuadratureAndParity) {
  const double m = 0.04, s = 0.60, k = 0.05;
  const double via_gh =
      lognormal_expect(m, s, [k](double w) { return w > k ? w - k : 0.0; });
  EXPECT_NEAR(lognormal_call(m, s, k), via_gh, 5e-7 * m);
  // parity: E[min(W,k)] = m - E[(W-k)+]
  const double capped =
      lognormal_expect(m, s, [k](double w) { return w < k ? w : k; });
  EXPECT_NEAR(capped, m - lognormal_call(m, s, k), 5e-7 * m);
  // degenerate edges
  EXPECT_NEAR(lognormal_call(m, 0.0, k), 0.0, 0.0);          // m < k, s=0
  EXPECT_NEAR(lognormal_call(m, 0.0, 0.03), 0.01, 1e-15);    // intrinsic
  EXPECT_NEAR(lognormal_call(m, s, 0.0), m, 1e-15);          // k<=0 -> m-k
}

TEST(RvLognormal, NormCdfKnownValues) {
  EXPECT_NEAR(norm_cdf(0.0), 0.5, 1e-15);
  EXPECT_NEAR(norm_cdf(1.959963984540054), 0.975, 1e-9);
  EXPECT_NEAR(norm_cdf(-1.959963984540054), 0.025, 1e-9);
}
}  // namespace
```

- [ ] **Step 2: Add `deriv_distribution_test.cpp` to `tests/CMakeLists.txt`, build, verify FAIL** (header does not exist).

- [ ] **Step 3: Implement.** Newton root-finding for GH nodes on Hermite recurrence `H_{n+1} = 2xH_n − 2nH_{n−1}`, weights `w = √π·2^{n−1}·n!/(n²·H_{n−1}(x)²)`; symmetric — solve upper half, mirror. Function-local `static const GhRule` (magic static, thread-safe). `lognormal_expect` in the header (template), the rest in the .cpp. 60-line budget per function; all `noexcept`, no allocation after the static init.

- [ ] **Step 4: Run** `-Ctest -R "GhRule|RvLognormal"` → PASS. Also run `powershell scripts\atx-build.ps1 check atx-vol\src\detail\rv_lognormal.cpp`.

- [ ] **Step 5: Commit** `feat(vol): Gauss-Hermite + lognormal RV distribution kernel`

---

### Task 3: Vol-of-vol config + Carr–Lee-consistent auto-calibration

**Files:**
- Modify: `atx-vol/include/atx/vol/derivatives.hpp` (DerivConfig, DerivQuote, DerivFlags)
- Modify: `atx-vol/src/derivatives.cpp`
- Test: `atx-vol/tests/deriv_distribution_test.cpp`

**Interfaces:**
- `DerivConfig` gains `double vol_of_vol = 0.0;` — annualized lognormal vol of the future realized-variance leg. `0` = auto-calibrate from the surface's own Carr–Lee convexity; `> 0` = use as-is; `< 0` = InvalidArgument.
- `DerivQuote` gains `double vol_of_vol_used = 0.0;` (NaN when no distribution model ran).
- `DerivFlags` gains `VolOfVolCalibrated = 1u << 9` (set when auto-calibration produced ξ).
- Produces (internal, `derivatives.cpp` anon namespace, used by Tasks 4–6):

```cpp
// Resolve the vol-of-vol for a contract: explicit cfg wins; otherwise calibrate
// s.t. the lognormal E[sqrt(W)] reproduces the Carr-Lee K_vol on this surface
// at this tenor: s^2 = -8 ln(k_vol_cl / sqrt(k_var)), xi = s / sqrt(T).
// Returns xi and whether it was calibrated (for the flag). k_vol_cl >= sqrt(k_var)
// (no convexity, or degenerate inputs) yields xi = 0.
struct VolOfVol { double xi; bool calibrated; };
template <class SurfaceT>
[[nodiscard]] Result<VolOfVol> resolve_vol_of_vol(const SurfaceT& surface,
                                                  const CurveSet& curves, double T,
                                                  double k_var_future,
                                                  const DerivConfig& cfg);
```

- [ ] **Step 1: Failing tests** (these drive through the public API of Task 4's capped var swap for the flag; the pure math is tested here via inception vol swap once Task 6 lands — for THIS task test the config validation + the closed-form identity using existing entries):

```cpp
TEST(VolOfVol, NegativeConfigRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = -0.5;
  const auto q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

// On a FLAT surface Carr-Lee K_vol < sqrt(K_var) purely from the ATMF
// straddle's own lognormal convexity, so auto-calibration must return xi > 0,
// and the calibrated lognormal must reproduce Carr-Lee exactly by construction:
// sqrt(K_var) * exp(-s^2/8) == K_vol_CL.
TEST(VolOfVol, AutoCalibrationReproducesCarrLee) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  const double T = 0.25;
  const auto kv = var_swap_fair_strike(surf, cs, T, cfg);
  const auto kl = vol_swap_fair_strike(surf, cs, T, cfg);
  ASSERT_TRUE(kv.has_value());
  ASSERT_TRUE(kl.has_value());
  ASSERT_LT(kl->fair_strike_dec, std::sqrt(kv->fair_strike_dec));  // convexity exists
  const double s2 = -8.0 * std::log(kl->fair_strike_dec / std::sqrt(kv->fair_strike_dec));
  ASSERT_GT(s2, 0.0);
  const double recon = std::sqrt(kv->fair_strike_dec) * std::exp(-s2 / 8.0);
  EXPECT_NEAR(recon, kl->fair_strike_dec, 1e-14);
}
```

- [ ] **Step 2: Build + run, verify FAIL** (`vol_of_vol` member does not exist → compile failure is the failing state for test 1; test 2 passes already — it pins the closed form the implementation will use, keep it as the oracle).

- [ ] **Step 3: Implement.** Add the fields/flag; validate `vol_of_vol >= 0` in `var_swap_fair_strike`, `vol_swap_fair_strike`, and `deriv_price` alongside `reserved_fields_clean`. Implement `resolve_vol_of_vol`: explicit → `{cfg.vol_of_vol, false}`; else compute `k_vol_cl` via the existing Carr–Lee block (factor the ATMF computation out of `vol_swap_fair_strike` into a shared helper so the two never drift), apply the closed form, clamp `ratio > 1 → xi = 0`, `xi = sqrt(s2)/sqrt(T)`.

- [ ] **Step 4: Run** `-Ctest -R "VolOfVol|VarStrip|Marquee|AgedDispatch|ReservedValidation"` → PASS, no regressions.

- [ ] **Step 5: Commit** `feat(vol): vol-of-vol knob with Carr-Lee-consistent auto-calibration`

---

### Task 4: Capped variance swap

**Files:**
- Modify: `atx-vol/include/atx/vol/derivatives.hpp` (flags + quote field + doc header)
- Modify: `atx-vol/src/derivatives.cpp` (new `price_capped_var_swap`, dispatch)
- Test: `atx-vol/tests/deriv_distribution_test.cpp`

**Interfaces:**
- `DerivFlags` gains `CapApplied = 1u << 10` (cap option value subtracted), `CapPinned = 1u << 11` (accrued ≥ cap; PV pinned at `df·N·(C − K)`).
- `DerivQuote` gains `double cap_option_value_dec = 0.0;` — `b·E[(W − K_c)+]`, the expectation haircut (0 for uncapped kinds, NaN never).
- `DerivContract::cap_dec` activates for `CappedVarSwap` (annualized decimal VARIANCE cap, e.g. `(2.5·0.20)² = 0.25`). Validation in `deriv_price`: capped kinds require `cap_dec > 0` else InvalidArgument; `VarSwap`/`VolSwap` with `cap_dec != 0` now returns **InvalidArgument** (was NotImplemented — update `ReservedValidation.DerivPrice_NonzeroCapDecOnVarSwap_ReturnsNotImplemented` to expect InvalidArgument and rename it `..._ReturnsInvalidArgument`).
- Engine: `Auto` and `RvDistributionProxy` both route `CappedVarSwap` to the distribution model; `StripLogContract`/`VolCarrLee` on a capped kind → InvalidArgument ("engine cannot price capped kinds").
- Semantics: `E[min(V,C)]` per the Reference-math section; PV `= df·N·(E[min(V,C)] − K)`; `fair_strike_dec = E[min(V,C)]` (strike pricing the contract to zero); flags `ModelProxy|CapApplied` (+`Aged`/`FullyAged` as applicable, +`VolOfVolCalibrated` when auto).

- [ ] **Step 1: Failing tests**

```cpp
// Far-OTM cap: capped == uncapped to quadrature noise; CapApplied still stamped.
TEST(CappedVarSwap, FarOtmCapMatchesUncapped) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::VarSwap; c.maturity_t = 0.25; c.notional = 1e6;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto plain = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(plain.has_value());
  c.kind = DerivKind::CappedVarSwap;
  c.cap_dec = 25.0;  // absurdly high variance cap
  const auto capped = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(capped.has_value());
  EXPECT_NEAR(capped->fair_strike_dec, plain->fair_strike_dec,
              1e-9 * plain->fair_strike_dec);
  EXPECT_TRUE(has_flag(capped->flags, DerivFlags::CapApplied));
  EXPECT_TRUE(has_flag(capped->flags, DerivFlags::ModelProxy));
}

// Parity: capped expectation + cap option value == uncapped expectation.
TEST(CappedVarSwap, CapParityHolds) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;
  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap; c.maturity_t = 0.25; c.notional = 1e6;
  c.cap_dec = 0.05;  // near-the-money cap vs K_var ~ 0.04
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  DerivContract u = c; u.kind = DerivKind::VarSwap; u.cap_dec = 0.0;
  const auto uq = deriv_price(surf, cs, u, cfg);
  ASSERT_TRUE(uq.has_value());
  EXPECT_GT(q->cap_option_value_dec, 0.0);
  EXPECT_NEAR(q->fair_strike_dec + q->cap_option_value_dec,
              uq->fair_strike_dec, 1e-12);
  EXPECT_LT(q->fair_strike_dec, uq->fair_strike_dec);  // cap lowers fair strike
}

// Accrued already above the cap: PV pinned at df*N*(C-K), CapPinned stamped,
// and the surface is never needed (mid-life, but the future leg is irrelevant).
TEST(CappedVarSwap, AccruedAboveCapPinsPv) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap; c.maturity_t = 0.10; c.notional = 1e6;
  c.strike_dec = 0.04; c.cap_dec = 0.09;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.09 * 3.001;  // w_done*rv_done = (1/3)*0.27 > 0.09 = C
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  const double df = cs.yield.disc(0.10);
  EXPECT_NEAR(q->pv, df * 1e6 * (0.09 - 0.04), 1e-6);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::CapPinned));
  EXPECT_NEAR(q->fair_strike_dec, 0.09, 1e-15);
}

TEST(CappedVarSwap, ZeroCapRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap; c.maturity_t = 0.25; c.notional = 1.0;
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Build + run `-Ctest -R CappedVarSwap`, verify FAIL** (NotImplemented today).

- [ ] **Step 3: Implement `price_capped_var_swap`** mirroring `price_var_swap` structure: strip for `K_var_future` (skip when fully aged or pinned), blend weights, pin check `a = w_done·rv_done ≥ C` first (pin path needs no strip — order the checks so a pinned contract never errors on `T <= 0` at expiry), `resolve_vol_of_vol`, displaced closed form via `detail::lognormal_call`. Update `deriv_price` dispatch + `cap_dec` validation matrix. Fully-aged: `E[min(V,C)] = min(rv_done, C)` (deterministic — no model, no ModelProxy flag). Update the two stale `ReservedValidation` expectations.

- [ ] **Step 4: Run** `-Ctest -R "CappedVarSwap|ReservedValidation|AgedDispatch"` → PASS.

- [ ] **Step 5: Commit** `feat(vol): capped variance swap via lognormal RV distribution engine`

---

### Task 5: Capped volatility swap

**Files:**
- Modify: `atx-vol/src/derivatives.cpp` (`price_capped_vol_swap`, dispatch)
- Test: `atx-vol/tests/deriv_distribution_test.cpp`

**Interfaces:**
- `DerivContract::cap_dec` for `CappedVolSwap` is a decimal VOL cap (e.g. `2.5·0.20 = 0.50`). Same validation matrix as Task 4.
- `E[min(√V, c)] = E[√min(V, c²)]` by GH over `W`; `V = a + b·W`. Fully aged → `min(√rv_done, c)` deterministic. Pin: `a ≥ c²` → `CapPinned`, PV `= df·N·(c − K)`.
- `fair_strike_dec` = the capped expectation (vol units); `convexity_adjustment_dec = √(E[V]) − fair_strike_dec` (diagnostic, uncapped-var-root minus capped-vol strike); flags `ModelProxy|CapApplied` (+aging, +calibration).

- [ ] **Step 1: Failing tests**

```cpp
// Jensen ordering at inception with explicit vol-of-vol:
//   E[min(sqrt V, c)] <= E[sqrt V] <= sqrt(E[V])   (cap haircut, then concavity)
TEST(CappedVolSwap, JensenAndCapOrdering) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.cap_dec = 0.22;  // near-the-money vol cap vs sigma = 0.20
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  const auto kv = var_swap_fair_strike(surf, cs, 0.25, cfg);
  ASSERT_TRUE(kv.has_value());
  EXPECT_LT(q->fair_strike_dec, std::sqrt(kv->fair_strike_dec));
  EXPECT_GT(q->fair_strike_dec, 0.10);  // sane magnitude
  // Removing the cap (huge c) must recover the pure E[sqrt V] which exceeds
  // the capped strike.
  DerivContract un = c; un.cap_dec = 10.0;
  const auto uq = deriv_price(surf, cs, un, cfg);
  ASSERT_TRUE(uq.has_value());
  EXPECT_GT(uq->fair_strike_dec, q->fair_strike_dec);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::CapApplied));
}

// GH consistency: with a far-OTM cap and zero accrual the capped vol swap must
// equal the exact lognormal sqrt moment.
TEST(CappedVolSwap, FarOtmCapMatchesLognormalSqrtMoment) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.60;
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.cap_dec = 10.0;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  const auto kv = var_swap_fair_strike(surf, cs, 0.25, cfg);
  ASSERT_TRUE(kv.has_value());
  const double s = 0.60 * std::sqrt(0.25);
  const double truth =
      atx::vol::detail::lognormal_sqrt_moment(kv->fair_strike_dec, s);
  EXPECT_NEAR(q->fair_strike_dec, truth, 1e-8);
}

// Fully aged: payoff-exact min(sqrt(rv), c).
TEST(CappedVolSwap, FullyAgedPaysMinSqrtRvCap) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.20; c.cap_dec = 0.25;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.09;  // sqrt = 0.30 > cap 0.25
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 1e5 * (0.25 - 0.20), 1e-9);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::FullyAged));
}
```

- [ ] **Step 2: Build + run `-Ctest -R CappedVolSwap`, verify FAIL.**

- [ ] **Step 3: Implement `price_capped_vol_swap`:** pin check (`a ≥ c²`), fully-aged shortcut, else strip → `m`, `resolve_vol_of_vol` → `s = ξ√τ`, `detail::lognormal_expect(m, s, [&](double w){ const double v = a + b*w; return std::sqrt(std::min(v, c2)); })`.

- [ ] **Step 4: Run** `-Ctest -R "CappedVolSwap|CappedVarSwap"` → PASS.

- [ ] **Step 5: Commit** `feat(vol): capped volatility swap via Gauss-Hermite on the RV distribution`

---

### Task 6: Mid-life vol swap dispatch

**Files:**
- Modify: `atx-vol/src/derivatives.cpp` (`price_vol_swap` mid-life branch)
- Test: `atx-vol/tests/deriv_distribution_test.cpp`

**Interfaces:**
- `price_vol_swap` intermediate `n_done` no longer returns NotImplemented: `E[√V] = E[√(a + bW)]` by GH (`detail::lognormal_expect`), `m` from the strip at residual `maturity_t`, ξ via `resolve_vol_of_vol`. Flags: `Aged|ModelProxy` (+`VolOfVolCalibrated`). `accrued_component_dec = w_done·rv_done`, `future_component_dec = w_future·m` (variance-space diagnostics, same convention as var swap); `convexity_adjustment_dec = √(a+b·m) − fair_strike_dec`.
- Engine matrix: `Auto` mid-life → distribution proxy; explicit `VolCarrLee` on a mid-life contract → InvalidArgument (CL cannot blend accrual); explicit `RvDistributionProxy` at inception → distribution engine end to end (calibrated ξ makes it match CL to 1e-12 — pin that consistency in a test).

- [ ] **Step 1: Failing tests**

```cpp
// Mid-life continuity: as n_done -> 0 the mid-life price approaches the
// inception Carr-Lee price (auto-calibrated xi makes them agree by construction).
TEST(MidLifeVolSwap, ContinuousWithInceptionAtZeroAccrual) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 0u;
  const auto q0 = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q0.has_value());
  // One observation, realized exactly at the implied level: the blend barely moves.
  c.rv_spec.n_obs_done = 1u;
  c.rv_spec.rv_done_dec = q0->uncapped_var_dec;
  const auto q1 = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q1.has_value()) << q1.error().to_string();
  EXPECT_NEAR(q1->fair_strike_dec, q0->fair_strike_dec,
              2e-3 * q0->fair_strike_dec);
  EXPECT_TRUE(has_flag(q1->flags, DerivFlags::Aged));
  EXPECT_TRUE(has_flag(q1->flags, DerivFlags::ModelProxy));
}

// Mid-life monotonicity: higher accrued realized => higher vol-swap mark.
TEST(MidLifeVolSwap, MonotoneInAccruedRealized) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.10; c.notional = 1e5;
  c.strike_dec = 0.20;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 42u;
  c.rv_spec.rv_done_dec = 0.02;
  const auto lo = deriv_price(surf, cs, c, cfg);
  c.rv_spec.rv_done_dec = 0.09;
  const auto hi = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(lo.has_value());
  ASSERT_TRUE(hi.has_value());
  EXPECT_GT(hi->fair_strike_dec, lo->fair_strike_dec);
  EXPECT_GT(hi->pv, lo->pv);
  // Deterministic-floor sanity: strike can never fall below sqrt(a) and never
  // exceed sqrt(a + b*m) (Jensen).
  EXPECT_GE(hi->fair_strike_dec, std::sqrt((42.0 / 63.0) * 0.09) - 1e-12);
}

// At expiry the mid-life path must hand over to the fully-aged branch exactly.
TEST(MidLifeVolSwap, HandsOverToFullyAgedAtExpiry) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;  // sqrt = 0.21
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 1e5 * (0.21 - 0.18), 1e-9);
}
```

- [ ] **Step 2: Build + run `-Ctest -R MidLifeVolSwap`, verify FAIL** (first test errors NotImplemented at `n_done == 1`).

- [ ] **Step 3: Implement** the mid-life branch in `price_vol_swap`; keep the existing unaged/fully-aged paths byte-identical (`Marquee` + `VarStrip` suites pin them).

- [ ] **Step 4: Run** `-Ctest -R "MidLifeVolSwap|Marquee|CappedVolSwap"` → PASS.

- [ ] **Step 5: Commit** `feat(vol): mid-life vol swap marks via the RV distribution engine`

---

### Task 7: Greeks

**Files:**
- Modify: `atx-vol/include/atx/vol/derivatives.hpp` (DerivGreeks, DerivGreekBumps, `deriv_greeks` declarations + extern templates + PricedSurface overload)
- Modify: `atx-vol/src/derivatives.cpp`
- Test: `atx-vol/tests/deriv_greeks_test.cpp` (new; append to `tests/CMakeLists.txt`)

**Interfaces:**

```cpp
// Spot-based sensitivity block, same conventions as the option pipeline's
// AmericanGreeks (portfolio_pricer.hpp:58-60): delta = dPV/dS, gamma = d2PV/dS2,
// vega = dPV/dsigma (parallel surface shift, per 1.00 vol), volga = d2PV/dsigma2,
// vanna = d2PV/dSdsigma, theta = dPV/dt (calendar, PV units per year, holding
// realized accrual fixed), rho = dPV/dr, charm = d2PV/dSdt. NaN = not computed.
struct DerivGreeks {
  double pv = 0.0;
  double delta = 0.0, gamma = 0.0, vega = 0.0, volga = 0.0, vanna = 0.0;
  double theta = 0.0, rho = 0.0, charm = 0.0;
  DerivQuote quote{};  // the center (unbumped) quote
};

struct DerivGreekBumps {
  double spot_rel = 1.0e-4;        // relative S bump (central)
  double vol_abs = 1.0e-4;         // absolute parallel sigma bump (central)
  double rate_abs = 1.0e-4;        // absolute zero-rate bump (one-sided)
  double time_years = 1.0 / 365.25;  // theta roll (one-sided, T decreasing)
  bool second_order = true;        // volga/vanna/charm (adds 6 evals)
};

template <class SurfaceT>
[[nodiscard]] Result<DerivGreeks>
deriv_greeks(const SurfaceT& surface, const CurveSet& curves,
             const DerivContract& contract, const DerivConfig& cfg = DerivConfig{},
             const DerivGreekBumps& bumps = DerivGreekBumps{});

[[nodiscard]] Result<DerivGreeks> deriv_greeks(const PricedSurface& surface,
                                               const DerivContract& contract,
                                               const DerivConfig& cfg = DerivConfig{},
                                               const DerivGreekBumps& bumps = DerivGreekBumps{});
```

Bump mechanics (all through `deriv_price` so every product/age/cap regime prices its greeks through the exact same path it prices its mark):
- **Spot** (sticky-strike): scale `CurveSet::spot` and every `ForwardPoint::F` by `(1±h)`; wrap the surface in `RespotView{base, k_shift = ln(1±h)}` exposing `iv(k,T) = base.iv(k + k_shift, T)` so the vol is re-read at the ORIGINAL absolute strike.
- **Vol**: `VolShiftView{base, ±dv}` exposing `iv(k,T) = base.iv(k,T) + dv`; curves unchanged.
- **Theta**: reprice with `contract.maturity_t − dt` (clamped at 0), same curves/surface, realized spec untouched; `theta = (pv_dn − pv_0)/dt`.
- **Rho**: rebuild the `CurveSet` yield with all zero rates `+dr` (sample `curves.yield.zero(T)` at the forward pillars' Ts plus the contract T; one-sided).
- Auto-calibrated ξ: resolve ONCE from the center surface and pin it into the bumped evaluations (pass an internal cfg with `vol_of_vol = xi_center`) — otherwise vega double-counts the calibration's own drift. Document this in the header.
- Fully-aged contracts: delta/gamma/vega/volga/vanna/charm = 0 exactly, theta 0, rho = −T·pv analytic (skip bumping entirely).

- [ ] **Step 1: Failing tests**

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include "atx/vol/curve.hpp"
#include "atx/vol/derivatives.hpp"
#include "atx/vol/surface.hpp"
// reuse the local flat-surface helpers pattern from derivatives_test.cpp
// (copy make_flat_surface / make_flat_curves into this file's anon namespace).

namespace {
// ... make_flat_surface / make_flat_curves exactly as in derivatives_test.cpp ...

// Var swap on a FLAT surface: analytic truths.
//   vega  = dK_var/dsigma * w_future * df * N = 2*sigma * 1 * df * N
//   delta = 0 (no skew, sticky-strike)   gamma ~ 0
//   rho: PV(K != fair) discounts, d(df)/dr = -T*df
TEST(DerivGreeks, VarSwapFlatSurfaceAnalyticTruths) {
  const double sigma = 0.20, T = 0.25, N = 1e6;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap; c.maturity_t = T; c.notional = N;
  c.strike_dec = 0.02;  // off-fair so rho has something to discount
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  const double df = cs.yield.disc(T);
  EXPECT_NEAR(g->vega, 2.0 * sigma * df * N, 2e-2 * 2.0 * sigma * df * N);
  EXPECT_NEAR(g->delta * 100.0 / (N), 0.0, 1e-3);   // per-spot units, flat => ~0
  EXPECT_NEAR(g->rho, -T * g->pv, 5e-3 * std::fabs(-T * g->pv) + 1e-6);
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->theta));
  // theta of an off-fair var swap on a flat surface: future K_var is
  // T-independent, so d/dt only hits the discount: theta ~ r*pv = 0 here (r=0).
  EXPECT_NEAR(g->theta, 0.0, 1e-2 * std::fabs(g->pv) + 1.0);
}

// Skewed surface: delta must be nonzero and negative for a long var swap under
// sticky-strike with a negative skew (down-moves ride up the smile).
TEST(DerivGreeks, VarSwapSkewGivesNonzeroDelta) {
  EssviSurface surf(2);
  // rho < 0 skew, phi > 0 curvature
  const EssviSlice s0{0.04 * 0.01, 1.5, -0.6, 0.01};
  const EssviSlice s1{0.04 * 1.00, 1.5, -0.6, 1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap; c.maturity_t = 0.25; c.notional = 1e6;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value());
  EXPECT_LT(g->delta, 0.0);
  EXPECT_GT(std::fabs(g->delta) * 100.0, 1.0);  // economically visible
}

// FD self-consistency: greeks must reproduce a direct large-bump repricing.
TEST(DerivGreeks, VegaMatchesDirectReprice) {
  const double sigma = 0.20, T = 0.25;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const EssviSurface surf_up = make_flat_surface(sigma + 0.01, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap; c.maturity_t = T; c.notional = 1e6;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  const auto p0 = deriv_price(surf, cs, c, deriv_default_config());
  const auto p1 = deriv_price(surf_up, cs, c, deriv_default_config());
  ASSERT_TRUE(g.has_value());
  ASSERT_TRUE(p0.has_value());
  ASSERT_TRUE(p1.has_value());
  const double fd = (p1->pv - p0->pv) / 0.01;
  EXPECT_NEAR(g->vega, fd, 2e-2 * std::fabs(fd));
}

// Fully aged: pure discounting, all market greeks exactly zero.
TEST(DerivGreeks, FullyAgedHasOnlyRho) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(g->delta, 0.0);
  EXPECT_EQ(g->gamma, 0.0);
  EXPECT_EQ(g->vega, 0.0);
  EXPECT_EQ(g->volga, 0.0);
  EXPECT_EQ(g->vanna, 0.0);
  EXPECT_EQ(g->theta, 0.0);
}

// Every product kind produces finite greeks mid-life (the full matrix).
TEST(DerivGreeks, AllKindsMidLifeFinite) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  for (const DerivKind kind : {DerivKind::VarSwap, DerivKind::VolSwap,
                               DerivKind::CappedVarSwap, DerivKind::CappedVolSwap}) {
    DerivContract c{};
    c.kind = kind; c.maturity_t = 0.10; c.notional = 1e5; c.strike_dec = 0.03;
    c.cap_dec = (kind == DerivKind::CappedVarSwap)   ? 0.25
                : (kind == DerivKind::CappedVolSwap) ? 0.50
                                                     : 0.0;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 21u;
    c.rv_spec.rv_done_dec = 0.05;
    const auto g = deriv_greeks(surf, cs, c);
    ASSERT_TRUE(g.has_value()) << static_cast<int>(kind);
    for (const double v : {g->pv, g->delta, g->gamma, g->vega, g->volga,
                           g->vanna, g->theta, g->rho, g->charm}) {
      EXPECT_TRUE(std::isfinite(v)) << static_cast<int>(kind);
    }
  }
}

// PricedSurface-native overload works end to end.
TEST(DerivGreeks, PricedSurfaceOverload) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(9, 100.0, 100.0, 0.30);
  DerivContract c{};
  c.kind = DerivKind::VarSwap; c.maturity_t = 0.35; c.notional = 1e6;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 88u;
  const auto g = atx::vol::deriv_greeks(ps, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_NEAR(g->vega, 2.0 * 0.30 * g->quote.pv == 0.0 ? 2.0 * 0.30 * 1e6 : 2.0 * 0.30 * 1e6,
              0.05 * 2.0 * 0.30 * 1e6);  // df ~ 1 on the flat fixture
}
}  // namespace
```

(That last EXPECT is deliberately loose — flat fixture df ≈ 1; tighten to the fixture's actual df at implementation time using `kFixtureRate` from `analytics_fixture.hpp`.)

- [ ] **Step 2: Add test file to `tests/CMakeLists.txt`, build, verify FAIL** (no `deriv_greeks`).

- [ ] **Step 3: Implement.** Anon-namespace wrapper views (`RespotView`, `VolShiftView` — 6 lines each, satisfy the `iv(k,T)` concept). Bumped `CurveSet` builders (respot-scale, rate-shift). A small evaluation table: center, S±, σ±, (S±,σ±) if second_order, T−dt, S± at T−dt if second_order, r+. Central differences for delta/gamma/vega/volga, cross for vanna `((pv_pp − pv_pm − pv_mp + pv_mm)/(4 h_s S h_v))`, `charm = (delta(T−dt) − delta(T))/(−dt)`. All `deriv_price` failures propagate (ATX_TRY). Add extern/explicit template instantiations for `EssviSurface`/`SviSurface` and the `PricedSurface` overload via the existing `carry_from` + `PricedSurfaceStripView` + view composition. Fully-aged fast path first.

- [ ] **Step 4: Run** `-Ctest -R DerivGreeks` → PASS. Then `check` the TU and `-Ctest -R "VarStrip|Marquee|AgedDispatch"` for regressions.

- [ ] **Step 5: Commit** `feat(vol): finite-difference greeks for all vol-derivative kinds`

---

### Task 8: Dated fixings on RealizedTracker

**Files:**
- Modify: `atx-vol/include/atx/vol/derivatives.hpp` (RealizedTracker)
- Modify: `atx-vol/src/derivatives.cpp`
- Test: `atx-vol/tests/derivatives_test.cpp`

**Interfaces:**

```cpp
// Timestamped observe for daily-fixing drivers (the backtest). Same accrual
// arithmetic as observe(); additionally enforces STRICTLY ASCENDING fixing
// timestamps so a re-delivered snapshot cannot double-count a fixing:
// ts_ns <= last_fixing_ts_ns() returns AlreadyExists and mutates nothing.
[[nodiscard]] Status observe_dated(std::int64_t ts_ns, double spot);
[[nodiscard]] std::int64_t last_fixing_ts_ns() const noexcept;  // INT64_MIN before first
```

(Check `atx/core/error.hpp` for the exact duplicate-y code — if `AlreadyExists` does not exist, use `InvalidArgument` with message `"fixing timestamp not ascending"` and assert the message in the test via `error().to_string()`.)

- [ ] **Step 1: Failing tests**

```cpp
TEST(RealizedTracker, ObserveDated_RefusesReplayAndBackdate) {
  auto built = RealizedTracker::create(252.0, 10u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  EXPECT_TRUE(t.observe_dated(1000, 100.0).has_value());
  EXPECT_TRUE(t.observe_dated(2000, 101.0).has_value());
  const auto after_two = t.snapshot();
  EXPECT_EQ(after_two.n_obs_done, 1u);
  // exact replay: refused, state untouched
  EXPECT_FALSE(t.observe_dated(2000, 101.0).has_value());
  // backdate: refused
  EXPECT_FALSE(t.observe_dated(1500, 99.0).has_value());
  const auto still = t.snapshot();
  EXPECT_EQ(still.n_obs_done, after_two.n_obs_done);
  EXPECT_EQ(t.last_fixing_ts_ns(), 2000);
  // forward continues fine
  EXPECT_TRUE(t.observe_dated(3000, 102.0).has_value());
  EXPECT_EQ(t.snapshot().n_obs_done, 2u);
}

TEST(RealizedTracker, ObserveDated_MatchesUndatedArithmetic) {
  auto a = RealizedTracker::create(252.0, 10u);
  auto b = RealizedTracker::create(252.0, 10u);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const double spots[] = {100.0, 101.0, 99.0, 102.0};
  ASSERT_TRUE(a->observe_batch(spots).has_value());
  std::int64_t ts = 1;
  for (const double s : spots) {
    ASSERT_TRUE(b->observe_dated(ts++, s).has_value());
  }
  EXPECT_EQ(a->snapshot().n_obs_done, b->snapshot().n_obs_done);
  EXPECT_DOUBLE_EQ(a->snapshot().sum_sq_log_returns_done,
                   b->snapshot().sum_sq_log_returns_done);
}
```

- [ ] **Step 2: Build + run `-Ctest -R RealizedTracker`, verify FAIL.**

- [ ] **Step 3: Implement:** private `std::int64_t last_fixing_ts_ns_ = std::numeric_limits<std::int64_t>::min();`; `observe_dated` validates ordering FIRST (before the spot-positivity check mutates nothing anyway), then delegates to `observe(spot)` and stamps the ts only on success.

- [ ] **Step 4: Run** `-Ctest -R RealizedTracker` → PASS.

- [ ] **Step 5: Commit** `feat(vol): dated idempotent fixings on RealizedTracker`

---

### Task 9: DerivBook — portfolio-layer pricing of swap books

**Files:**
- Create: `atx-vol/include/atx/vol/deriv_book.hpp`
- Create: `atx-vol/src/deriv_book.cpp` (add to `atx-vol/CMakeLists.txt` next to `src/derivatives.cpp`)
- Test: `atx-vol/tests/deriv_book_test.cpp` (new; append to `tests/CMakeLists.txt`)

**Interfaces:**

```cpp
// A book of vol-derivative positions priced against a SurfaceSet — the additive
// companion to PortfolioPricer for non-option legs. Deliberately does NOT touch
// Portfolio/PreparedPortfolio: the option pipeline's bit-identity and SIMD
// grouping contracts stay untouched; an option book and a deriv book are priced
// separately and their PriceTotals combined.
struct DerivPosition {
  std::uint64_t id = 0;      // caller key, echoed into the frame
  std::uint32_t uid = 0;     // underlier, resolved via SurfaceSet::find
  DerivContract contract{};
  double qty = 1.0;          // position scale on top of contract.notional
};

struct DerivPriceRow {
  std::uint64_t id = 0;
  std::uint32_t uid = 0;
  double pv = kPortNaN;           // qty-scaled
  double fair_strike_dec = kPortNaN;
  DerivGreeks greeks{};           // qty-scaled, NaN block on failure
  PriceStatus status = PriceStatus::Ok;
};

struct DerivPriceFrame {
  std::vector<DerivPriceRow> rows;   // input order
  PriceTotals totals;                // Ok rows only, serial fixed-order sum
  [[nodiscard]] std::size_t n_ok() const noexcept;
};

// Price every position against its uid's surface. A missing surface or a
// pricing failure marks that ROW (ModelUnavailable / NumericError), never the
// call. greeks=false prices marks only (greeks stay NaN, totals greek fields NaN).
[[nodiscard]] Result<DerivPriceFrame>
price_deriv_book(const SurfaceSet& surfaces, std::span<const DerivPosition> book,
                 const DerivConfig& cfg = DerivConfig{}, bool greeks = true,
                 const DerivGreekBumps& bumps = DerivGreekBumps{});

// Sum two totals blocks (NaN-propagating per field; n_ok adds).
[[nodiscard]] PriceTotals combine_totals(const PriceTotals& a, const PriceTotals& b) noexcept;
```

Implementation notes: `SurfaceSet`/`SurfaceRef`/`PriceTotals`/`PriceStatus` come from `portfolio_pricer.hpp`. A `SurfaceRef` exposes `iv(K,T)` (absolute strike), `forward_at`, `rate_at`, `pricing()` — build a single-tenor `CurveSet` per position (`spot = pricing().S`, one forward pillar `(T, forward_at(T))`, flat yield at `rate_at(T)`), wrap in the absolute-strike→log-moneyness adapter (same shape as `PricedSurfaceStripView` but over `SurfaceRef`), and call the templated `deriv_price`/`deriv_greeks`. Map greeks into `PriceTotals` fields (`pv, delta, gamma, vega, abs_vega = |vega|`, `theta, rho, vanna, volga, charm`; `dP_dq` NaN). qty-scale everything once, at the row.

- [ ] **Step 1: Failing tests**

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include "atx/vol/deriv_book.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "support/analytics_fixture.hpp"

namespace {
using atx::vol::DerivContract;
using atx::vol::DerivKind;
using atx::vol::DerivPosition;
using atx::vol::price_deriv_book;
using atx::vol::PriceStatus;
using atx::vol::SurfaceSet;

TEST(DerivBook, PricesVarAndVolSwapAgainstSurfaceSet) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface* arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivPosition p0{};
  p0.id = 11; p0.uid = 7; p0.qty = 2.0;
  p0.contract.kind = DerivKind::VarSwap;
  p0.contract.maturity_t = 0.35;
  p0.contract.notional = 1e6;
  p0.contract.rv_spec.annualization = 252.0;
  p0.contract.rv_spec.n_obs_total = 88u;
  DerivPosition p1 = p0;
  p1.id = 12; p1.qty = -1.0;
  p1.contract.kind = DerivKind::VolSwap;

  const DerivPosition book[] = {p0, p1};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 2u);
  EXPECT_EQ(f->rows[0].id, 11u);
  EXPECT_EQ(f->rows[0].status, PriceStatus::Ok);
  EXPECT_EQ(f->rows[1].status, PriceStatus::Ok);
  EXPECT_NEAR(f->rows[0].fair_strike_dec, 0.09, 5e-4);   // sigma^2
  EXPECT_NEAR(f->rows[1].fair_strike_dec, 0.30, 5e-3);   // ~sigma
  // struck at 0 => pv = df*qty*N*K; sign follows qty
  EXPECT_GT(f->rows[0].pv, 0.0);
  EXPECT_LT(f->rows[1].pv, 0.0);
  // totals = serial sum of Ok rows
  EXPECT_NEAR(f->totals.pv, f->rows[0].pv + f->rows[1].pv, 1e-9);
  EXPECT_EQ(f->n_ok(), 2u);
  // qty scaling: row0 vega is 2x the single-contract vega, roughly 2*2*sigma*N*df
  EXPECT_GT(f->rows[0].greeks.vega, 0.0);
}

TEST(DerivBook, MissingSurfaceMarksRowNotCall) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface* arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition good{};
  good.id = 1; good.uid = 7;
  good.contract.kind = DerivKind::VarSwap;
  good.contract.maturity_t = 0.35; good.contract.notional = 1e6;
  good.contract.rv_spec.annualization = 252.0;
  good.contract.rv_spec.n_obs_total = 88u;
  DerivPosition orphan = good;
  orphan.id = 2; orphan.uid = 999;  // no such surface
  const DerivPosition book[] = {good, orphan};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->rows[0].status, PriceStatus::Ok);
  EXPECT_EQ(f->rows[1].status, PriceStatus::ModelUnavailable);
  EXPECT_TRUE(std::isnan(f->rows[1].pv));
  EXPECT_EQ(f->n_ok(), 1u);
  EXPECT_NEAR(f->totals.pv, f->rows[0].pv, 1e-9);  // orphan excluded, not zeroed
}

TEST(DerivBook, MarksOnlySkipsGreeks) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface* arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition p{};
  p.id = 1; p.uid = 7;
  p.contract.kind = DerivKind::VarSwap;
  p.contract.maturity_t = 0.35; p.contract.notional = 1e6;
  p.contract.rv_spec.annualization = 252.0;
  p.contract.rv_spec.n_obs_total = 88u;
  const DerivPosition book[] = {p};
  const auto f = price_deriv_book(*ss, book, {}, /*greeks=*/false);
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(std::isfinite(f->rows[0].pv));
  EXPECT_TRUE(std::isnan(f->rows[0].greeks.vega));
  EXPECT_TRUE(std::isnan(f->totals.vega));
  EXPECT_TRUE(std::isfinite(f->totals.pv));
}
}  // namespace
```

- [ ] **Step 2: Add to `tests/CMakeLists.txt`, build, verify FAIL** (header missing).

- [ ] **Step 3: Implement** `deriv_book.{hpp,cpp}` per the interface notes. Serial loop over positions (books are small; no threading v1 — document). Reuse `PriceTotals` accumulation semantics: NaN greek fields when `greeks=false` or any Ok row failed its greek block; totals sum Ok rows in input order.

- [ ] **Step 4: Run** `-Ctest -R DerivBook` → PASS.

- [ ] **Step 5: Commit** `feat(vol): DerivBook prices swap books against a SurfaceSet with combined totals`

---

### Task 10: Backtest swap lane — state, accrual, MTM, settlement

**Files:**
- Modify: `atx-vol/include/atx/vol/backtest.hpp` (`SwapLot`, `PortfolioState`, `BacktestCheckpoint`, result columns)
- Modify: `atx-vol/src/backtest.cpp` (loop insertion, validate-transition, checkpoint io)
- Test: `atx-vol/tests/backtest_swap_test.cpp` (new; append to `tests/CMakeLists.txt`)

**Interfaces:**

```cpp
// An OTC vol-derivative lot. Immutable once emitted by a strategy (validated by
// the same transition check that pins option lots). Accrual state lives in the
// ENGINE (SwapAccrualState), never on the lot.
struct SwapLot {
  std::uint64_t id = 0;
  std::uint32_t uid = 0;
  DerivKind kind = DerivKind::VarSwap;
  double strike_dec = 0.0;
  double cap_dec = 0.0;              // 0 unless capped kind
  double notional = 0.0;
  double qty = 1.0;
  std::int64_t start_ts_ns = 0;      // first fixing seed timestamp
  std::int64_t expiry_ts_ns = 0;     // exact-match settlement, option convention
  std::uint32_t n_obs_total = 0;
  double annualization = 252.0;
};

struct PortfolioState {
  std::vector<Lot> lots;
  std::vector<SwapLot> swap_lots;    // NEW — additive, default empty
};

// Engine-owned per-swap-lot running state (checkpointable POD).
struct SwapAccrual {
  std::uint64_t lot_id = 0;
  RealizedVarianceSpec rv{};
  double prev_spot = 0.0;
  std::int64_t prev_ts_ns = 0;
  bool have_prev = false;
  double prev_pv = 0.0;              // yesterday's qty-scaled mark (0 pre-first-mark)
};
// BacktestCheckpoint gains: std::vector<SwapAccrual> swap_accruals;
```

Engine loop changes (all inside `run_backtest_strategy_impl`, insertion window backtest.cpp:2441-2519 per the wiring map — where `base`, `shifted`, and `dt` are all live):
1. **Fixing + MTM pass** for each `book.swap_lots` entry: find `shifted.find(lot.uid)`; absent surface on an unexpired lot → fail closed (same policy as `n_unpriced_shares`: error, not silent skip). Feed `S_shifted` into the lot's `SwapAccrual` (RealizedTracker arithmetic via `observe_dated`-equivalent on the POD: refuse `ts <= prev_ts_ns`; seed when `!have_prev`). Then mark: build `DerivContract` from the lot + current `rv` snapshot, `maturity_t = residual_T(lot.expiry_ts_ns, shifted.ts_ns())`, price via the `PricedSurface`-path `deriv_price` against the shifted surface; `swap_pnl += (pv_now − accrual.prev_pv)`; `accrual.prev_pv = pv_now`.
2. **Settlement**: `lot.expiry_ts_ns <= shifted.ts_ns()` (exact-match required like options): terminal payoff from the accrual (`rv_done` capped per kind, `sqrt` for vol kinds) minus strike, × `qty·notional`; `cash += payoff`; `swap_pnl += payoff − accrual.prev_pv`; erase lot + accrual. A swap lot whose expiry passes WITHOUT an exact observation → `Err(NotFound)`, same as options.
3. **NAV**: `step_total = pnl_total + settlement + shares_pnl + swap_pnl + financing − cost`. New `BacktestResult` columns: `swap_pv` (sum of live marks), `swap_pnl` (per step). Zero-swap books: both columns exactly 0.0 and NAV bit-identical to today (pin with a test asserting the engine skips the pass when `swap_lots.empty()`).
4. **Transition validation**: surviving `SwapLot`s bit-compared on every field (extend `validate_strategy_transition`); new ids must be `>= next_lot_id` watermark like options; a strategy may open swap lots in `on_step` by appending to `book.swap_lots`.
5. **Checkpoint**: `swap_accruals` serialized alongside existing fields; resume reproduces marks exactly.
6. **Entry economics v1**: swaps open at zero cost (fair-or-not strike is the strategy's choice; entry PV difference flows into the first mark's `swap_pnl`). No frictions on swaps v1 — document in the header.

- [ ] **Step 1: Failing tests.** Build the smallest synthetic multi-day fixture already used by backtest tests (copy the snapshot-construction pattern from `backtest_exec_test.cpp` — flat surface per day with a chosen spot path; if that file's helper is reusable, include it via `tests/support/`):

```cpp
// (sketch of the three core behaviors; adapt fixture plumbing to the local
// helpers in backtest_exec_test.cpp at implementation time)

// 1. Accrual correctness: a var-swap lot held over 3 steps accrues exactly
//    sum(ln(S_i/S_{i-1})^2) and the terminal settlement pays
//    qty*notional*(A/n_total*sum - K) into cash on the expiry snapshot.
TEST(BacktestSwap, VarSwapAccruesAndSettlesExactly) { ... }

// 2. Zero-swap byte-identity: an option-only strategy run twice — engine
//    before/after this change is not directly testable in one binary, so pin
//    the observable: swap columns are exactly 0.0 and NAV equals the
//    hand-recomputed option-only sum for the same book.
TEST(BacktestSwap, OptionOnlyBookHasZeroSwapColumns) { ... }

// 3. Checkpoint resume: run 5 steps with a swap lot; checkpoint at step 3;
//    resume; final NAV and swap_pnl series bit-identical to the uninterrupted run.
TEST(BacktestSwap, CheckpointResumeReproducesSwapMarks) { ... }

// 4. Replayed snapshot date does not double-fix (idempotence guard).
TEST(BacktestSwap, DuplicateTimestampRefusedNotDoubleCounted) { ... }

// 5. Missing surface for a live swap lot fails closed.
TEST(BacktestSwap, MissingSurfaceForSwapLotErrors) { ... }
```

Write these five tests IN FULL against the chosen fixture before implementing — the `...` above is fixture plumbing to be copied from `backtest_exec_test.cpp`, not behavior left undefined. The asserted numbers are hand-computed from the fixture's spot path exactly as `RealizedTracker.ObserveBatch_HandComputedThreeReturns` does.

- [ ] **Step 2: Build + run `-Ctest -R BacktestSwap`, verify FAIL.**

- [ ] **Step 3: Implement** per the engine-loop changes above. Keep the swap pass in its own ~60-line function (`step_swap_lots(...)`) called from the loop, not inlined into the 200-line step body.

- [ ] **Step 4: Run** `-Ctest -R "BacktestSwap"` then the full backtest suites `-Ctest -R "Backtest"` (regression gate: existing suites must not move).

- [ ] **Step 5: Commit** `feat(vol): variance/vol swap lane in the backtest engine`

---

### Task 11: Docs, umbrella, gates

**Files:**
- Modify: `atx-vol/include/atx/vol/vol.hpp` (add `deriv_book.hpp` include)
- Modify: `atx-vol/README.md` (Ported-modules row for `derivatives.hpp` — update capabilities sentence; add `deriv_book.hpp` row)
- Modify: `atx-vol/CHANGELOG.md` (sprint entry)
- Modify: `atx-vol/include/atx/vol/derivatives.hpp` (file-header banner: remove the "Reserved for follow-on work" list, document the distribution engine, greeks, dated tracker)

- [ ] **Step 1: Update the header banner** — the "What this port ships" list must state: capped var/vol swaps, mid-life vol swaps, distribution engine + vol-of-vol calibration, greeks, dated fixings, Richardson error estimate. Remove stale "reserved" claims that now ship (McQe / affine stay reserved).
- [ ] **Step 2: README + CHANGELOG rows.**
- [ ] **Step 3: Umbrella gate:** `vol_umbrella_test.cpp` compiles with the new include.
- [ ] **Step 4: Hygiene gate:** `cmake --preset hygiene` + build both new TUs (`rv_lognormal.cpp`, `deriv_book.cpp`) — includes must be per-TU clean.
- [ ] **Step 5: Full suite:** `powershell scripts\atx-build.ps1 build atx-vol-tests` then `powershell scripts\atx-build.ps1 -Ctest -L atx_vol` (fast+slow). All green.
- [ ] **Step 6: Perf sanity:** time `-Ctest -R "DerivGreeks|CappedV|MidLife|DerivBook"` — the new suites must land in the fast label (<10 s total); if the greek matrix pushes a suite past 10 s, append it to `ATX_VOL_SLOW_FILTER` instead of thinning tests.
- [ ] **Step 7: Commit** `docs(vol): document the vol-derivatives production sprint surface`

---

## Self-Review Notes

- **Spec coverage:** price/PV/greeks for all four kinds — Tasks 4/5/6/7; fair-strike computation — existing strip + Task 3 calibration; position definitions — Task 9 (`DerivPosition`) + Task 10 (`SwapLot`); daily fixing accrual — Tasks 8/10; daily PV+greeks through time — Tasks 7/9/10; portfolio + pricer wiring — Task 9; vol-surface wiring — already shipped (E6) + Task 9's SurfaceRef adapter; backtest wiring — Task 10; correctness gates — per-task oracles; speed — closed forms everywhere possible, GH only where needed, Task 11 step 6; feature breadth — the full kind × age × cap matrix (Task 7 `AllKindsMidLifeFinite`).
- **Deliberate scope cuts (documented, not silent):** `McQe`/`RvDistributionAffine` engines stay reserved; swap entry frictions v1 = none; `DerivBook` single-threaded; CBOE variance-future marking stays reserved; discrete-monitoring `FullMc` stays reserved.
- **Type consistency check:** `DerivGreeks` (Task 7) is consumed by `DerivPriceRow` (Task 9); `RealizedVarianceSpec` reused by `SwapAccrual` (Task 10); `detail::lognormal_*` (Task 2) consumed by Tasks 4/5/6; flag values 9/10/11 do not collide with existing 0-8.
