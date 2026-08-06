# SPY Surface Fit Integrity — Starvation + Ratchet Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the two confirmed surface-fit defects that produce backtest PnL spike artifacts (COVID 2020-03-18/19, Liberation Day 2025-04-10/11) by adding a k-coverage slice-admission gate, wiring the dormant quote-fidelity publish floor, containing the calendar-floor ratchet to data-supported k, adding a `band-audit` detector, and validating end-to-end on the known-bad dates.

**Architecture:** All changes are TIGHTENINGS on the ConvexDense risk-fit pipeline (`build_observations_european -> PreparedSlice -> fit_curve_surface -> fit_slice_curve -> fit_convex_slice -> admission -> populate`). New refusals route into the EXISTING slice-refusal lane (`SlicePrepOutcome`/`ExpiryFitOutcome` -> tenor truncation -> `mark_domain` policies) or the EXISTING board-refusal lane (`FitAdmissionPolicy` -> `FailedCell` + safe-mode record retention). Nothing mutates what a passing fit computes: calm-day fits stay byte-identical unless a slice trips a NEW gate.

**Tech Stack:** C++20 (clang-cl + Ninja, build dir `build-rel` at the worktree root), Eigen active-set QP, gtest (`atx-vol-tests`), OPRA hive v2 parquet loader (Arrow), SurfaceDb `.atxvsa` partitions.

**Worktree:** `C:\atx\.claude\worktrees\strangle-backtest` (branch `worktree-strangle-backtest`). All relative paths below are relative to `C:\atx\.claude\worktrees\strangle-backtest\atx-vol` unless prefixed. The build dir is `C:\atx\.claude\worktrees\strangle-backtest\build-rel` (already configured: Ninja + clang-cl, tool exes land in `build-rel\bin\`).

**Evidence dossiers (ground truth, cited throughout):**
- `C:\Users\natha\AppData\Local\Temp\claude\c--atx\12e0463a-eda0-409d-9fe5-f610a091f48b\scratchpad\inv_starvation.md` (Mode A)
- `C:\Users\natha\AppData\Local\Temp\claude\c--atx\12e0463a-eda0-409d-9fe5-f610a091f48b\scratchpad\inv_wing.md` (Mode B)

## Global Constraints

- Never loosen oracle admission tolerances or QP certification tolerances (kQpCertificateTol/kQpActiveTol/kQpStartTol).
- Production data read-only: never write C:/atx-data/opra-hive or C:/atx-data/surface-db/spy-*; r2 clones C:/atx-data/surface-db-r2/spy-<year> are writable.
- No full-suite test runs; small targeted test groups only (e.g. --gtest_filter on the touched module's tests).
- Calm-day fits must remain byte-identical unless a slice trips a NEW gate; every new refusal must be observable (counter/taxonomy value), never silent.
- Build via PowerShell 'cmd /c' + vcvars64 (Bash silently no-ops vcvars builds); Ninja + clang-cl; build dir build-rel.
- TDD: each behavior change lands with a failing-first test (production-captured boards where possible; the dossiers name exact dates/expiries/counts to reproduce).

**Standard build/test commands (used by every task; run from PowerShell):**

```powershell
# Build the test binary (and any tool targets a task names):
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake --build C:\atx\.claude\worktrees\strangle-backtest\build-rel --target atx-vol-tests'

# Run a targeted test group (the exe lands beside the other tool exes; if it is
# not in bin\, locate it once with:  Get-ChildItem -Recurse -Filter atx-vol-tests.exe C:\atx\.claude\worktrees\strangle-backtest\build-rel):
C:\atx\.claude\worktrees\strangle-backtest\build-rel\bin\atx-vol-tests.exe --gtest_filter='<Filter>'
```

## Failure-mode summary (why these five tasks)

- **Mode A (starvation, 2020-03-18):** the absolute spread filters (`SpreadToMid` cap 0.60, calib.cpp:133-140; `SpreadVol` cap 0.05, calib.cpp:167-174) evacuate the long-dated belly on stress days (T=1.7496: 159 of 160 drops). The count-only floor `kMinPreparedFitRows = 5` (include/atx/vol/detail/prepared_fitting.hpp:73, checked src/curve_fit.cpp:311-312) passes the 9 survivors, which straddle ATM around a 0.516-wide k-hole; `ConvexSliceFit::call_price` interpolates LINEARLY in K across the hole (src/dense_slice.cpp:340-346), and the strictly-convex chord serves ATM 8-15 vol points high. **Task 1** refuses such slices; **Task 4/5** detect and validate.
- **Mode B (seed + ratchet, 2025-04-10):** the freshly-listed 2025-04-24 expiry admits 11 one-sided far-OTM puts (k in [-0.124, -0.069]); the fit extrapolates ATM/call-side data-free (ATM 105%, iv(+0.15)=177.5%). The strict per-node calendar floor (src/dense_slice.cpp:599-621) plus violation-k node promotion (src/vol_curve.cpp:436-497, src/pricer_fitter.cpp:1478-1496) then pins w(+0.15)=0.1208 on ALL 10 later slices out to T=2.686. **Task 1** kills the seed (one-sided => no straddle); **Task 3** contains the ratchet as defense-in-depth; **Task 2** is the publish backstop (the 4/10 worst long slices scored ~5% of their own admitted rows in-band; the existing gate `min_worst_frac_within_bidask` defaults to 0.0 = dormant, include/atx/vol/fit_policy.hpp:128, enforced src/fit_policy.cpp:144-149, evidence computed src/curve_fit.cpp:918).
- **Division of labor:** Task 2 does NOT catch Mode A (a Mode-A slice reprices its own 9 admitted survivors mostly in-band; it is wrong about the strikes it REFUSED) — that is exactly why Task 1 gates on coverage of the admitted set, not on fit quality.

## File Structure (what changes where)

| File | Change |
|---|---|
| `include/atx/vol/detail/prepared_fitting.hpp` | Task 1: `SliceKCoverage`, `slice_k_coverage()`, coverage constants |
| `src/prepared_fitting.cpp` | Task 1: `slice_k_coverage()` impl |
| `src/curve_fit.cpp` | Task 1: `SlicePrepOutcome::Uncovered` + gate + tally/switch; Task 3: `n_slice_calendar_unsupported` count |
| `include/atx/vol/curve_fit.hpp` | Task 1: `CurveSurfaceReport::n_slices_uncovered`; Task 3: `n_slice_calendar_unsupported` |
| `include/atx/vol/surface_parity.hpp` | Task 1: `ExpiryFitOutcome::PrepUncovered` (appended) |
| `src/surface_db_build.cpp` | Task 1: `rich_drop_reason_name` case |
| `tools/include/atx/vol/tools/surface_db_populate.hpp` + `src/surface_db_populate.cpp` | Task 2: `populate_admission_policy()` + wire into `pricer_config_for_symbol` |
| `include/atx/vol/dense_slice.hpp` + `src/dense_slice.cpp` | Task 3: `ConvexFitContext::floor_support_k`, floor-row bound |
| `include/atx/vol/vol_curve.hpp` + `src/vol_curve.cpp` | Task 3: `kCalendarFloorUnsupportedMsg`, bounded scan/refusal in the ConvexDense arm |
| `tools/include/atx/vol/tools/surface_db_admin.hpp` + `src/surface_db_admin.cpp` | Task 4: `band_audit` library |
| `tools/surface_db_main.cpp` | Task 4: `band-audit` subcommand |
| `tests/prepared_fitting_test.cpp`, `tests/curve_fit_coverage_test.cpp` (new), `tests/surface_db_populate_policy_test.cpp` (new), `tests/dense_slice_test.cpp`, `tests/surface_db_admin_test.cpp`, `tests/CMakeLists.txt` | tests |

---

### Task 1: k-coverage slice admission (`PrepUncovered`) — kills both seeds

**Files:**
- Modify: `include/atx/vol/detail/prepared_fitting.hpp` (after `kMinPreparedFitRows` at :73)
- Modify: `src/prepared_fitting.cpp` (new function, namespace `atx::vol`)
- Modify: `src/curve_fit.cpp` (`SlicePrepOutcome` enum :82-89; `prepare_fit_slice_into_slot` :281-365; starve tally :615-622; outcome switch :647-661)
- Modify: `include/atx/vol/curve_fit.hpp` (`CurveSurfaceReport`, after `n_slices_starved` at :94)
- Modify: `include/atx/vol/surface_parity.hpp` (`ExpiryFitOutcome` :273-282 — APPEND at end)
- Modify: `src/surface_db_build.cpp` (`rich_drop_reason_name` :446-464)
- Test: `tests/prepared_fitting_test.cpp` (unit), `tests/curve_fit_coverage_test.cpp` (NEW, integration), `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `atx::vol::SliceKCoverage { bool straddles_atm; double max_central_gap; bool admissible() const noexcept; }` and `[[nodiscard]] SliceKCoverage slice_k_coverage(std::span<const FitObs> rows);` (declared in `atx/vol/detail/prepared_fitting.hpp`, namespace `atx::vol`), constants `kCoverageAtmEps = 0.01`, `kCoverageCentralBand = 0.30`, `kCoverageMaxCentralGap = 0.40`.
- Produces: enum value `ExpiryFitOutcome::PrepUncovered` and counter `CurveSurfaceReport::n_slices_uncovered` (Task 5 reads the populate report's `slice_drop.*` rows, which spell it `PrepUncovered`).
- Consumes: `PreparedSlice::fit_observations() -> std::span<const FitObs>` (`FitObs::k` = ln(K/F), populated at src/calib.cpp:188).

**Threshold calibration (dossier receipts):**
- Refused, Mode A (inv_starvation.md §3): 2020-03-18 T=1.7496 hole (-0.155, +0.361) width **0.516**; T=1.8454 hole (-0.441, +0.070) width **0.511**; T=2.7462 hole (-0.498, +0.041) width **0.539**. All exceed the 0.40 cap; all three holes overlap [-0.30, +0.30].
- Refused, Mode B (inv_wing.md, executive summary): 2025-04-10 exp 2025-04-24, 11 admitted rows all in k [-0.124, -0.069] — no row with k >= +0.01, so the straddle test fails. Same shape for 4/09's seed (strikes 475-520, all below F).
- Passing, healthy neighbours: 2020-03-19 re-admits 60 rows on the same 2021-12-17 expiry (inv_starvation.md §2: "Next day spreads tightened and the same filters admitted 60 rows"); 2025-04-11 has every slice at n_used=60 and no thin expiry (inv_wing.md: "4/11 ... Every slice n_used=60; long end healthy"). A 60-row RDP-thinned belly (cap_observations_for_deam, src/calib.cpp:222-306, spends knots on curvature) has central gaps an order of magnitude under 0.40; deep-wing sparsity is exempt because only holes OVERLAPPING (-0.30, +0.30) are measured. Task 5 verifies byte-identity on those two dates and recalibrates per Step 12's rule if any healthy-day slice trips.
- Scope: the gate applies ONLY when the fitted family is `VolCurveKind::ConvexDense` (the `kind` parameter of `prepare_fit_slice_into_slot`, currently discarded via `(void)kind;` at src/curve_fit.cpp:284). eSSVI/SVI/C8/LinearVariance populations remain byte-identical.
- No legacy rescue for coverage: the LegacyEssviCompatibility rescue (src/curve_fit.cpp:319-351) exists for COUNT starvation; the coverage check is applied to whatever rows the (possibly rescued) `PreparedSlice` finally carries.

- [ ] **Step 1: Write the failing unit tests** — append to `tests/prepared_fitting_test.cpp`:

```cpp
// ── Task 1: k-coverage slice admission (stress-day starvation guard) ─────────
// Calibrated on the 2026-08 investigation numbers; see the constants' doc in
// atx/vol/detail/prepared_fitting.hpp.

namespace {
[[nodiscard]] std::vector<atx::vol::FitObs> rows_at(std::initializer_list<double> ks) {
  std::vector<atx::vol::FitObs> rows;
  for (const double k : ks) {
    atx::vol::FitObs o{};
    o.k = k;
    rows.push_back(o);
  }
  return rows;
}
} // namespace

TEST(SliceKCoverage, RefusesTheCovidBellyHole) {
  // The nine 2020-03-18 T=1.7496 survivors: straddle ATM, but with a
  // 0.516-wide hole from k=-0.155 to k=+0.361 crossing the forward.
  const auto rows = rows_at({-2.052, -0.442, -0.317, -0.260, -0.155,
                             +0.361, +0.526, +0.563, +0.587});
  const auto cov = atx::vol::slice_k_coverage(rows);
  EXPECT_TRUE(cov.straddles_atm);
  EXPECT_NEAR(cov.max_central_gap, 0.516, 1.0e-9);
  EXPECT_FALSE(cov.admissible());
}

TEST(SliceKCoverage, RefusesTheOneSidedSeed) {
  // The eleven 2025-04-10 exp-2025-04-24 survivors: all puts, k in
  // [-0.124, -0.069] — nothing right of ATM, so straddle fails whatever the
  // gaps look like.
  std::vector<atx::vol::FitObs> rows;
  for (int i = 0; i < 11; ++i) {
    atx::vol::FitObs o{};
    o.k = -0.124 + 0.0055 * static_cast<double>(i);
    rows.push_back(o);
  }
  const auto cov = atx::vol::slice_k_coverage(rows);
  EXPECT_FALSE(cov.straddles_atm);
  EXPECT_FALSE(cov.admissible());
}

TEST(SliceKCoverage, AdmitsAHealthyDenseSlice) {
  // A healthy thinned belly: k from -0.50 to +0.45 in 0.05 steps — every
  // central gap is 0.05, an order of magnitude under the 0.40 cap.
  std::vector<atx::vol::FitObs> rows;
  for (double k = -0.50; k <= 0.451; k += 0.05) {
    atx::vol::FitObs o{};
    o.k = k;
    rows.push_back(o);
  }
  const auto cov = atx::vol::slice_k_coverage(rows);
  EXPECT_TRUE(cov.straddles_atm);
  EXPECT_LT(cov.max_central_gap, 0.06);
  EXPECT_TRUE(cov.admissible());
}

TEST(SliceKCoverage, WingGapsOutsideTheCentralBandDoNotRefuse) {
  // Sparse deep wings are normal (the RDP cap thins linear regions): a
  // 1.0-wide hole entirely outside [-0.30, +0.30] must not trip the gate.
  const auto rows =
      rows_at({-1.50, -0.50, -0.25, -0.10, 0.0, 0.10, 0.25, 0.50, 1.50});
  const auto cov = atx::vol::slice_k_coverage(rows);
  EXPECT_TRUE(cov.straddles_atm);
  EXPECT_NEAR(cov.max_central_gap, 0.25, 1.0e-12);
  EXPECT_TRUE(cov.admissible());
}

TEST(SliceKCoverage, EmptyOrNonFiniteRowsAreInadmissible) {
  EXPECT_FALSE(atx::vol::slice_k_coverage({}).admissible());
  atx::vol::FitObs nan_row{};
  nan_row.k = std::numeric_limits<double>::quiet_NaN();
  const std::vector<atx::vol::FitObs> rows{nan_row};
  EXPECT_FALSE(atx::vol::slice_k_coverage(rows).admissible());
}
```

- [ ] **Step 2: Run the unit tests to verify they fail**

Run: build `atx-vol-tests` with the standard command.
Expected: COMPILE failure — `slice_k_coverage` is not declared. That is this step's red (the API does not exist yet).

- [ ] **Step 3: Declare the predicate** — in `include/atx/vol/detail/prepared_fitting.hpp`, directly below `inline constexpr std::size_t kMinPreparedFitRows = 5u;` (:73):

```cpp
// ── k-coverage slice admission (stress-day starvation guard) ─────────────────
// Count alone (kMinPreparedFitRows) admits two defective populations the
// ConvexDense price-space fit then extrapolates:
//   * 2020-03-18 SPY T=1.7496: 9 survivors straddling ATM around a 0.516-wide
//     log-moneyness hole (k in (-0.155, +0.361)) — the convex chord across the
//     hole served ATM 8-15 vol points high;
//   * 2025-04-10 SPY T=0.0383: 11 survivors ALL below the forward
//     (k in [-0.124, -0.069]) — ATM and the whole call wing were served
//     data-free (ATM 105%, iv(+0.15) 177.5%) and then calendar-ratcheted.
// The criterion: fit rows must STRADDLE the forward (>= 1 row at
// k <= -kCoverageAtmEps AND >= 1 row at k >= +kCoverageAtmEps) and leave no
// adjacent-row hole wider than kCoverageMaxCentralGap where the hole overlaps
// (-kCoverageCentralBand, +kCoverageCentralBand). Measured bad-slice holes:
// 0.511 / 0.516 / 0.539 (all refused with margin at 0.40); healthy 60-row
// slices carry central gaps ~0.05. Deep-wing sparsity is deliberately exempt.
inline constexpr double kCoverageAtmEps = 0.01;
inline constexpr double kCoverageCentralBand = 0.30;
inline constexpr double kCoverageMaxCentralGap = 0.40;

struct SliceKCoverage {
  bool straddles_atm{false};
  double max_central_gap{0.0};
  [[nodiscard]] bool admissible() const noexcept {
    return straddles_atm && max_central_gap <= kCoverageMaxCentralGap;
  }
};

// Pure predicate over a prepared slice's fit rows (FitObs::k = ln(K/F)).
// Non-finite k rows are ignored; an empty/all-non-finite set is inadmissible.
[[nodiscard]] SliceKCoverage slice_k_coverage(std::span<const FitObs> rows);
```

(`std::span` is already visible in this header via the `PreparedSlice` API; add `#include <span>` if the build disagrees.)

- [ ] **Step 4: Implement it** — in `src/prepared_fitting.cpp` (namespace `atx::vol`, beside the other free functions):

```cpp
SliceKCoverage slice_k_coverage(std::span<const FitObs> rows) {
  SliceKCoverage out{};
  std::vector<double> ks;
  ks.reserve(rows.size());
  for (const FitObs &o : rows) {
    if (std::isfinite(o.k)) {
      ks.push_back(o.k);
    }
  }
  if (ks.empty()) {
    return out; // inadmissible: nothing straddles, nothing covered
  }
  std::sort(ks.begin(), ks.end());
  bool has_left = false;
  bool has_right = false;
  for (const double k : ks) {
    has_left = has_left || (k <= -kCoverageAtmEps);
    has_right = has_right || (k >= kCoverageAtmEps);
  }
  out.straddles_atm = has_left && has_right;
  for (std::size_t i = 0; i + 1 < ks.size(); ++i) {
    const double lo = ks[i];
    const double hi = ks[i + 1];
    // The hole (lo, hi) is measured only when it overlaps the central band.
    if (hi > -kCoverageCentralBand && lo < kCoverageCentralBand) {
      out.max_central_gap = std::max(out.max_central_gap, hi - lo);
    }
  }
  return out;
}
```

- [ ] **Step 5: Run the unit tests to verify they pass**

Run: build, then `atx-vol-tests.exe --gtest_filter='SliceKCoverage.*'`
Expected: 5 PASS.

- [ ] **Step 6: Write the failing integration test** — NEW file `tests/curve_fit_coverage_test.cpp`, modelled on `tests/curve_fit_carry_fallback_test.cpp` (repeat the helpers locally; test TUs do not share helpers):

```cpp
// Task 1 — board-level k-coverage refusal (PrepUncovered).
//
// A ConvexDense slice whose admitted rows are entirely one-sided (the
// 2025-04-24 freshly-listed-daily shape) must be REFUSED into the existing
// slice-drop lane (ExpiryFitOutcome::PrepUncovered -> tenor truncation), not
// fitted and extrapolated. The healthy sibling expiry still fits.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp"       // american_price, AmericanMethod
#include "atx/vol/curve_fit.hpp"      // fit_curve_surface, CurveSurfaceReport
#include "atx/vol/dividend.hpp"       // hybrid_forward, HybridDivParams
#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs, ExpiryFitOutcome
#include "atx/vol/types.hpp"          // Side
#include "atx/vol/universe.hpp"       // Underlying, Chain, chain_index
#include "atx/vol/vol_curve.hpp"      // CurveConfig

namespace {

using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::DividendEvent;
using atx::vol::ExpiryFitOutcome;
using atx::vol::fit_curve_surface;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::Side;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;

[[nodiscard]] double true_sigma(double k) noexcept { return 0.20 + 0.15 * k * k; }
[[nodiscard]] double borrow_of_T(double T) noexcept { return 0.02 + 0.04 * T; }

void size_chain(Chain &c) {
  const std::size_t n = c.strikes.size();
  c.bids.assign(2 * n, 0.0);
  c.asks.assign(2 * n, 0.0);
  c.bid_sizes.assign(2 * n, 0);
  c.ask_sizes.assign(2 * n, 0);
  c.mids.assign(2 * n, 0.0);
  c.ivs.assign(2 * n, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(2 * n, 0);
  c.flags.assign(2 * n, 0);
}

// Accurate Andersen-Lake quote for one (strike, side) leg, 1% half-spreads
// (the curve_fit_carry_fallback_test recipe). void so ASSERT is legal.
void fill_leg(Chain &c, double F, double q_eff, double T, std::size_t i, Side side) {
  const double K = c.strikes[i];
  const double sigma = true_sigma(std::log(K / F));
  const auto px = american_price(kSpot, K, T, sigma, kRate, q_eff, side,
                                 AmericanMethod::AndersenLake, std::nullopt);
  ASSERT_TRUE(px.has_value()) << "american_price failed K=" << K << " T=" << T;
  const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
  c.mids[idx] = *px;
  c.bids[idx] = *px * 0.99;
  c.asks[idx] = *px * 1.01;
  c.bid_sizes[idx] = 1;
  c.ask_sizes[idx] = 1;
}
```
```cpp
// Two-sided healthy expiry: every strike carries BOTH legs (co-terminal carry
// pairs everywhere, so the borrow solve is confident and the OTM strip is
// dense on both sides of F).
[[nodiscard]] Chain make_two_sided_chain(double T) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * kYearNs);
  const std::vector<DividendEvent> no_divs;
  const double borrow = borrow_of_T(T);
  const double F = hybrid_forward(kSpot, kRate, borrow, T, no_divs, c.expiry_ns,
                                  /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;
  for (int i = 0; i < 15; ++i) {
    const double k = -0.20 + 0.40 * static_cast<double>(i) / 14.0;
    c.strikes.push_back(F * std::exp(k));
  }
  size_chain(c);
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    fill_leg(c, F, q_eff, T, i, Side::Call);
    fill_leg(c, F, q_eff, T, i, Side::Put);
  }
  return c;
}

// Freshly-listed-daily shape (the 2025-04-24 seed): quotes exist ONLY below
// the forward, k in [-0.124, -0.069]. Both legs are quoted (so the borrow
// solve has co-terminal pairs); the prefer-OTM heuristic (calib.cpp:119-124)
// admits only the PUT legs to the fit strip, so every admitted row sits left
// of ATM.
[[nodiscard]] Chain make_one_sided_put_chain(double T) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * kYearNs);
  const std::vector<DividendEvent> no_divs;
  const double borrow = borrow_of_T(T);
  const double F = hybrid_forward(kSpot, kRate, borrow, T, no_divs, c.expiry_ns,
                                  /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;
  for (int i = 0; i < 11; ++i) {
    const double k = -0.124 + 0.0055 * static_cast<double>(i);
    c.strikes.push_back(F * std::exp(k));
  }
  size_chain(c);
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    fill_leg(c, F, q_eff, T, i, Side::Put);
    fill_leg(c, F, q_eff, T, i, Side::Call);
  }
  return c;
}

[[nodiscard]] SurfaceParityInputs coverage_inputs() {
  SurfaceParityInputs in{};
  in.S = kSpot;
  in.r = kRate;
  in.deam.imply_borrow = true;
  in.deam.require_carry_confidence = false;
  in.fit_workers = 1; // deterministic
  return in;
}

} // namespace

TEST(CurveFitCoverage, OneSidedThinExpiryIsRefusedAsPrepUncovered) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_one_sided_put_chain(0.04));
  under.chains.push_back(make_two_sided_chain(0.50));

  const auto rep = fit_curve_surface(under, coverage_inputs(), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_EQ(rep->expiry_reports.size(), 2u);
  EXPECT_EQ(rep->expiry_reports[0].outcome, ExpiryFitOutcome::PrepUncovered);
  EXPECT_EQ(rep->expiry_reports[1].outcome, ExpiryFitOutcome::Fitted);
  EXPECT_EQ(rep->n_slices, 1u);
  EXPECT_EQ(rep->n_slices_uncovered, 1u);
}

TEST(CurveFitCoverage, WellCoveredBoardIsUntouched) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_two_sided_chain(0.25));
  under.chains.push_back(make_two_sided_chain(0.50));

  const auto rep = fit_curve_surface(under, coverage_inputs(), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  EXPECT_EQ(rep->n_slices, 2u);
  EXPECT_EQ(rep->n_slices_uncovered, 0u);
  for (const auto &er : rep->expiry_reports) {
    EXPECT_EQ(er.outcome, ExpiryFitOutcome::Fitted);
  }
}
```

Register it: add `curve_fit_coverage_test.cpp` to the `add_executable(atx-vol-tests ...)` source list in `tests/CMakeLists.txt` (beside `curve_fit_carry_fallback_test.cpp`). `CurveConfig{}` defaults to `VolCurveKind::ConvexDense` (include/atx/vol/vol_curve.hpp:447), so the gate is in scope.

- [ ] **Step 7: Run the integration test to verify it fails**

Run: build, then `--gtest_filter='CurveFitCoverage.*'`
Expected: COMPILE failure first (`PrepUncovered` / `n_slices_uncovered` do not exist). After Step 8's taxonomy lands but before Step 9's gate, `OneSidedThinExpiryIsRefusedAsPrepUncovered` FAILS with `outcome == Fitted` and `n_slices == 2` — the defect, demonstrated: the one-sided slice fits and extrapolates.

- [ ] **Step 8: Add the taxonomy**

1. `include/atx/vol/surface_parity.hpp` (:273-282) — append at the END of the enum (numeric stability for every existing value):

```cpp
enum class ExpiryFitOutcome : std::uint8_t {
  Fitted = 0,          // the primary curve fit succeeded
  FittedFallbackCurve, // recovered via the per-slice LinearVariance fallback
  FittedLegacyPrep,    // fit succeeded on a Legacy-prep-rescued (thin) slice
  CarryFailed,         // carry / forward resolution failed — no slice
  PrepStarved,         // below the usable-row floor (thin) — no slice
  PrepFailed,          // HARD preparation error (defect) — `error` is set
  FitFailed,           // slice fit failed — `error` is set
  Skipped,             // degenerate maturity (T<=0) — never attempted
  PrepUncovered,       // admitted rows fail k-coverage (ATM straddle / central
                       // gap) — refused before fitting, no slice (Task 1)
};
```

2. `src/curve_fit.cpp` (:82-89) — add to the TU-local enum:

```cpp
enum class SlicePrepOutcome : std::uint8_t {
  Skipped,              // degenerate maturity (T<=0) — never attempted
  Prepared,             // the primary policy produced a fittable slice
  PreparedLegacyRescue, // recovered via the opt-in Legacy-prep rescue
  Starved,              // below the usable-row floor even after any rescue (thin)
  Uncovered,            // admitted rows fail the k-coverage criterion (Task 1)
  CarryFailed,          // carry / forward resolution failed
  Failed,               // HARD preparation error (defect) — error retained
};
```

3. `include/atx/vol/curve_fit.hpp` — after `n_slices_starved` (:94):

```cpp
  // Task 1 (k-coverage): expiries refused because their admitted fit rows do
  // not straddle ATM or leave a central k-hole wider than the cap
  // (ExpiryFitOutcome::PrepUncovered). Surfaced, never silent.
  std::size_t n_slices_uncovered{0};
```

4. `src/surface_db_build.cpp` `rich_drop_reason_name` (:446-464) — add before the `Fitted` group:

```cpp
  case ExpiryFitOutcome::PrepUncovered:
    return "PrepUncovered";
```

5. Sweep for other exhaustive switches: Grep `case ExpiryFitOutcome::` across `src/ tools/ tests/`; the clang-cl `-Wswitch` build is the backstop — fix any newly-incomplete switch by adding a `PrepUncovered` case with the same semantics as `PrepStarved` (a truthful drop).

- [ ] **Step 9: Wire the gate** — `src/curve_fit.cpp`:

1. In `prepare_fit_slice_into_slot` (:281-365): delete `(void)kind;` (:284) and insert immediately BEFORE `slot.prepared.emplace(std::move(*prepared));` (:356):

```cpp
  // Task 1 (k-coverage): count alone (kMinPreparedFitRows) waves through
  // stress-day husks whose belly the absolute spread filters evacuated
  // (2020-03-18) and one-sided freshly-listed expiries (2025-04-10); the
  // ConvexDense chord / power tails then serve the missing region
  // extrapolated. Refuse such slices into the same truthful-drop lane as
  // Starved. ConvexDense only: every other family's population and admission
  // are byte-identical.
  if (kind == VolCurveKind::ConvexDense &&
      !slice_k_coverage(prepared->fit_observations()).admissible()) {
    slot.prep_outcome = SlicePrepOutcome::Uncovered;
    return;
  }
```

2. Tally loop (:615-622) — beside the Starved count:

```cpp
    if (pre.prep_outcome == SlicePrepOutcome::Uncovered) {
      ++out.n_slices_uncovered; // Task 1: coverage-refused — surfaced, not hidden
    }
```

3. Outcome switch (:647-661) — new case:

```cpp
      case SlicePrepOutcome::Uncovered:
        rep.outcome = ExpiryFitOutcome::PrepUncovered;
        break;
```

(The `!pre.usable` guard already routes an `Uncovered` slot into that switch, because `prepare_fit_slice_into_slot` returns before setting `slot.usable = true` — the same mechanism `Starved` uses.)

- [ ] **Step 10: Run the tests to verify they pass**

Run: build, then `--gtest_filter='SliceKCoverage.*:CurveFitCoverage.*'`
Expected: all PASS.

- [ ] **Step 11: Targeted regression group (touched modules only — NO full suite)**

Run: `--gtest_filter='PreparedFitting.*:CurveFitCarryFallback.*:ConvexSliceFit.*'`
Expected: PASS (the gate refuses nothing on two-sided fixtures; `CurveFitCarryFallback` boards are two-sided by construction).

- [ ] **Step 12: Record the recalibration rule** (protects Task 5): if Task 5 finds a healthy-day slice refused, the ONLY permitted adjustments are narrowing `kCoverageCentralBand` (e.g. 0.30 -> 0.25) or raising `kCoverageMaxCentralGap` toward — but never past — **0.50** (the smallest measured bad-slice hole is 0.511). The straddle requirement (`kCoverageAtmEps`) is not negotiable; it is what kills Mode B. Document any adjustment beside the constants.

- [ ] **Step 13: Commit**

```bash
git add atx-vol/include/atx/vol/detail/prepared_fitting.hpp atx-vol/src/prepared_fitting.cpp \
        atx-vol/src/curve_fit.cpp atx-vol/include/atx/vol/curve_fit.hpp \
        atx-vol/include/atx/vol/surface_parity.hpp atx-vol/src/surface_db_build.cpp \
        atx-vol/tests/prepared_fitting_test.cpp atx-vol/tests/curve_fit_coverage_test.cpp \
        atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): refuse ConvexDense slices without ATM k-coverage (PrepUncovered)"
```

---

### Task 2: Wire the dormant in-band publish gate for surface-db populate

**Files:**
- Modify: `tools/include/atx/vol/tools/surface_db_populate.hpp` (new public policy helper; add `#include "atx/vol/fit_policy.hpp"`)
- Modify: `src/surface_db_populate.cpp` (`pricer_config_for_symbol` :52-67 + the helper's implementation)
- Test: `tests/surface_db_populate_policy_test.cpp` (NEW), `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `inline constexpr double kPopulateMinWorstFracInBand = 0.35;` and `[[nodiscard]] FitAdmissionPolicy populate_admission_policy() noexcept;` (namespace `atx::vol`, declared in `atx/vol/tools/surface_db_populate.hpp`).
- Consumes (all pre-existing, ZERO changes to them): `FitAdmissionPolicy::min_worst_frac_within_bidask` (include/atx/vol/fit_policy.hpp:128), the `evaluate_surface_admission` QualityBelowFloor check (src/fit_policy.cpp:144-149), the risk-arm dual gate that folds `cfg_.admission` into the digest (src/pricer_fitter.cpp:1357-1390: `admission_attempt` -> `completed_attempt_report(..., risk_admission_policy)` where `risk_admission_policy = detail::selector_served_admission_policy(cfg_.admission, cfg_.selector)` at :1374-1375), and the evidence value `worst_frac_within_bidask` computed at src/curve_fit.cpp:918 from per-expiry `ParityReport.frac_fv_within_bidask`.

**How the gate becomes live (verified from code — this is the whole fix):**
`populate_universe_streaming` builds each board's `PricerConfig` via `pricer_config_for_symbol` (src/surface_db_populate.cpp:52-67), which today never touches `PricerConfig::admission`, so the default `FitAdmissionPolicy{}` (Mark consumer, floor **0.0** = disabled) reaches `PricerFitter::fit` and QualityBelowFloor can never fire. Setting the floor there makes the EXISTING risk-arm dual gate enforce it: a below-floor candidate fails `evaluate_surface_admission`; `admission_attempt` folds that into `digest.failures |= ValidationFailure::InvalidDomain` (pricer_fitter.cpp:1379-1381); every fallback-ladder rung faces the same floor; the final refusal surfaces as `Err` from `PricerFitter::fit` -> `fit_board` returns `FitSlot{status=Failed, error_code, error_message}` (src/corpus_board_fit.cpp:296-306) -> populate records `n_failed` + `FailedCell{date, symbol, code, detail}` and, in safe (non-destructive) mode, re-appends the previously stored record for that cell (src/surface_db_populate.cpp:888-929). No crash anywhere on the path; the refusal lane is the pre-existing one.

The `admission=nullptr` argument at src/surface_db_populate.cpp:591 is the CORPUS quarantine layer (`CorpusAdmissionPolicy*` — quality-metric collection for `build_corpus` archives; src/corpus_board_fit.hpp:88-91: "pass nullptr to skip quality collection entirely (populate_surface_db's use)"). It is NOT the publish gate and stays `nullptr`; the publish gate lives on `PricerConfig::admission`, which is exactly what this task wires. Add a one-line comment at :591 saying so, so the next reader does not chase the same decoy.

**Floor value = 0.35, justification (dossier receipts):**
- Blocks the defect: on 2025-04-10 "the worst long slices were ~5% in-band; any floor in [0.3, 0.9] blocks the publish" (inv_wing.md Q4). 0.35 clears the required 0.3 with margin.
- Passes calm days: the gate measures each slice against the fit's OWN admitted+scored rows, NOT the full listed chain — src/curve_fit.cpp:804-812 ("The prepared score rows are the same keyed population as the fit rows") feeding `worst = min(worst, parity.frac_fv_within_bidask)` at :886-887. The chronic calm-day long-end ~55-80% figure was measured against the FULL listed chain (a strictly harder population), so it LOWER-bounds the gated metric; the served SPY regression board scores ~94.65% board-wide with calendar enforcement (tests/spy_bidask_regression_test.cpp:36-46). 0.35 sits far below both.
- Conservative by construction: the lowest floor that blocks the demonstrated defect with margin. Task 5 re-checks every rebuilt window for spurious calm-day refusals (acceptance: zero `FailedCell`s on the healthy control dates). If one appears, the floor may move DOWN toward 0.30 — never up.
- TIGHTENING ONLY: no other `FitAdmissionPolicy` field moves; no oracle/QP tolerance is touched. Accepted side effect: a floored Mark policy makes `fit_admission_consumes_parity` true (fit_policy.hpp:138-147), which forces `score_parity = true` on the mark arm too (pricer_fitter.cpp:826-832); the risk arm already forces it (pricer_fitter.cpp:1088). A symbol manifest that explicitly pinned `score_parity=false` now fails closed with `DiagnosticsUnavailable` (fit_policy.cpp:130-136) — loud, not silent.
- Division of labor: this floor does NOT catch Mode A (a starved slice reprices its own 9 survivors mostly in-band — it is wrong about the strikes it REFUSED). Task 1 owns Mode A; this is the Mode-B/general backstop.

- [ ] **Step 1: Write the failing test** — NEW file `tests/surface_db_populate_policy_test.cpp`:

```cpp
// Task 2 — the populate publish gate: the dormant
// FitAdmissionPolicy::min_worst_frac_within_bidask floor (fit_policy.hpp:128,
// default 0.0 = disabled; enforced at fit_policy.cpp:144-149) is wired to a
// conservative 0.35 for every surface-db populate fit.

#include <gtest/gtest.h>

#include "atx/vol/fit_policy.hpp"
#include "atx/vol/tools/surface_db_populate.hpp"

namespace {

using atx::vol::evaluate_surface_admission;
using atx::vol::fit_admission_consumes_parity;
using atx::vol::FitAdmissionPolicy;
using atx::vol::has_admission_failure;
using atx::vol::kPopulateMinWorstFracInBand;
using atx::vol::ParityDiagnosticState;
using atx::vol::populate_admission_policy;
using atx::vol::SurfaceAdmissionEvidence;
using atx::vol::SurfaceAdmissionReason;

// Evidence for a structurally healthy fitted board; only the in-band figure
// varies per test.
[[nodiscard]] SurfaceAdmissionEvidence healthy_evidence() {
  SurfaceAdmissionEvidence e{};
  e.attempted_expiries = 10u;
  e.fitted_expiries = 10u;
  e.attempted_quotes = 600u;
  e.fitted_quotes = 500u;
  e.front_expiry_fitted = true;
  e.finite_diagnostics = true;
  e.calendar_arb_free = true;
  e.finite_iv_domain = true;
  e.european_price_bounds = true;
  e.strike_monotone = true;
  e.strike_convex = true;
  e.calendar_total_variance = true;
  e.forward_variance_nonnegative = true;
  e.parity_state = ParityDiagnosticState::Valid;
  return e;
}

} // namespace

TEST(SurfaceDbPopulatePolicy, FloorIsWiredAndEverythingElseIsDefault) {
  const FitAdmissionPolicy policy = populate_admission_policy();
  EXPECT_DOUBLE_EQ(policy.min_worst_frac_within_bidask, kPopulateMinWorstFracInBand);
  EXPECT_DOUBLE_EQ(kPopulateMinWorstFracInBand, 0.35);
  // A floored Mark policy consumes the re-Americanized parity diagnostics, so
  // score_parity is forced on and an unscored board fails closed.
  EXPECT_TRUE(fit_admission_consumes_parity(policy));
  // TIGHTENING ONLY: every other admission field keeps its WP12 default.
  const FitAdmissionPolicy dflt{};
  EXPECT_EQ(policy.enabled, dflt.enabled);
  EXPECT_EQ(policy.consumer, dflt.consumer);
  EXPECT_EQ(policy.min_fitted_expiries, dflt.min_fitted_expiries);
  EXPECT_DOUBLE_EQ(policy.min_expiry_coverage, dflt.min_expiry_coverage);
  EXPECT_DOUBLE_EQ(policy.min_quote_coverage, dflt.min_quote_coverage);
  EXPECT_EQ(policy.require_front_expiry, dflt.require_front_expiry);
  EXPECT_EQ(policy.max_consecutive_expiry_gaps, dflt.max_consecutive_expiry_gaps);
  EXPECT_EQ(policy.require_calendar_arb_free, dflt.require_calendar_arb_free);
}

TEST(SurfaceDbPopulatePolicy, FloorRefusesTheAprilTenthSignatureAndAdmitsCalm) {
  SurfaceAdmissionEvidence evidence = healthy_evidence();

  evidence.worst_frac_within_bidask = 0.05; // 2025-04-10 worst long slice
  const auto refused = evaluate_surface_admission(evidence, populate_admission_policy());
  EXPECT_FALSE(refused.admitted);
  EXPECT_TRUE(has_admission_failure(refused, SurfaceAdmissionReason::QualityBelowFloor));

  evidence.worst_frac_within_bidask = 0.55; // calm-day long-end LOWER bound
  const auto admitted = evaluate_surface_admission(evidence, populate_admission_policy());
  EXPECT_TRUE(admitted.admitted);
}
```

Register `surface_db_populate_policy_test.cpp` in `tests/CMakeLists.txt`. (`ParityDiagnosticState` reaches this TU through `fit_policy.hpp`'s own includes — it is already used unqualified in `SurfaceAdmissionEvidence` at fit_policy.hpp:192.)

- [ ] **Step 2: Run to verify it fails**

Run: build `atx-vol-tests`.
Expected: COMPILE failure — `populate_admission_policy` / `kPopulateMinWorstFracInBand` are not declared.

- [ ] **Step 3: Declare + implement the policy**

1. `tools/include/atx/vol/tools/surface_db_populate.hpp` — add `#include "atx/vol/fit_policy.hpp"` to the include block and, near the top of the public API (namespace `atx::vol`):

```cpp
// ── Task 2: the populate publish floor ───────────────────────────────────────
// The quote-fidelity publication gate for every populate fit. The evidence
// (worst per-expiry frac_fv_within_bidask, scored against the fit's OWN
// admitted rows — curve_fit.cpp:918) and the check (fit_policy.cpp:144-149,
// QualityBelowFloor) both pre-exist; this floor is what arms them. 0.35:
// blocks the demonstrated 2025-04-10 publish (worst long slices ~5% in-band;
// any floor >= 0.3 refuses it) while sitting far under calm-day boards
// (>= ~55% against the strictly harder full-chain population; ~94.65%
// board-wide on the served SPY regression board). A refused board lands in
// the pre-existing FailedCell lane; safe mode retains the previously stored
// record. TIGHTENING ONLY — no oracle/QP tolerance moves.
inline constexpr double kPopulateMinWorstFracInBand = 0.35;

// WP12 Mark-serving defaults plus the floor above. Every other field is the
// default on purpose: the strict Risk shape gates already run separately via
// RiskAdmission::Required (the no-arb oracle), and structural coverage is the
// mark_domain/tenor-truncation policies' job downstream.
[[nodiscard]] FitAdmissionPolicy populate_admission_policy() noexcept;
```

2. `src/surface_db_populate.cpp` — implementation (namespace `atx::vol`, OUTSIDE the anonymous namespace, e.g. right above `populate_universe_streaming`):

```cpp
FitAdmissionPolicy populate_admission_policy() noexcept {
  FitAdmissionPolicy policy; // WP12 Mark-serving defaults (fit_policy.hpp:119)
  policy.min_worst_frac_within_bidask = kPopulateMinWorstFracInBand;
  return policy;
}
```

3. Wire it in `pricer_config_for_symbol` (src/surface_db_populate.cpp:52-67) — add one line beside the other `out.*` assignments:

```cpp
  // Task 2: arm the quote-fidelity publish floor (see populate_admission_policy).
  out.admission = populate_admission_policy();
```

4. Decoy note at the `fit_board` call (src/surface_db_populate.cpp:591), extend the existing comment:

```cpp
      // NOTE (Task 2): this nullptr is the CORPUS quarantine layer, not the
      // publish gate. The quote-fidelity publication floor rides on
      // pc.admission (populate_admission_policy via pricer_config_for_symbol).
```

- [ ] **Step 4: Run to verify it passes**

Run: build, then `--gtest_filter='SurfaceDbPopulatePolicy.*'`
Expected: 2 PASS.

- [ ] **Step 5: Targeted regression group**

Run: `--gtest_filter='FitPolicy*.*:SurfaceDbPopulatePolicy.*'` (plus, if a populate-lane suite exists under another name, the `SurfaceDbBuild.*` group: `--gtest_filter='SurfaceDbBuild*.*'`).
Expected: PASS. Any pre-existing test that fits a synthetic board through the populate lane and asserts publication will fail ONLY if its fixture fits worse than 35% in-band against its own rows — that is a fixture telling us it publishes garbage; inspect before touching either side (do not weaken the floor to green a test without reading the fixture).

- [ ] **Step 6: Commit**

```bash
git add atx-vol/tools/include/atx/vol/tools/surface_db_populate.hpp \
        atx-vol/src/surface_db_populate.cpp \
        atx-vol/tests/surface_db_populate_policy_test.cpp atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): arm populate quote-fidelity publish floor (worst in-band >= 0.35)"
```

---

### Task 3: Contain the calendar-floor ratchet to the previous slice's data-supported k-range

**Files:**
- Modify: `include/atx/vol/dense_slice.hpp` (`ConvexFitContext` :121-133 + new constant)
- Modify: `src/dense_slice.cpp` (calendar-floor row loop :599-621)
- Modify: `include/atx/vol/vol_curve.hpp` (shared refusal-message constant, near `ConvexRepairSpec` :423)
- Modify: `src/vol_curve.cpp` (ConvexDense arm of `fit_slice_curve` :400-501)
- Modify: `src/curve_fit.cpp` (count the new refusal in the fit-failure branch :770-799) and `include/atx/vol/curve_fit.hpp` (counter field)
- Test: `tests/dense_slice_test.cpp`

**Interfaces:**
- Produces: `ConvexFitContext::floor_support_k` (`std::pair<double,double>`, default `{-inf, +inf}`), `inline constexpr double kCalendarFloorSupportMargin = 0.10;` (dense_slice.hpp), `inline constexpr std::string_view kCalendarFloorUnsupportedMsg = "fit_slice_curve: calendar floor breach beyond previous slice's data-supported k-range";` (vol_curve.hpp), counter `CurveSurfaceReport::n_slice_calendar_unsupported`.
- Consumes: `fit_slice_curve`'s existing `prev_data_k_range` parameter (include/atx/vol/vol_curve.hpp:505-511) — ALREADY threaded from `fit_curve_surface` as `last_committed_obs_k` (src/curve_fit.cpp:631-636, :686-703, :870-884) and today consumed only by the parametric/SplineVol arms; the ConvexDense arm ignores it. That is the defect surface.

**Design (and why it cannot weaken published no-arb):**
- The ratchet (inv_wing.md): the per-node floor rows in `fit_convex_slice` (src/dense_slice.cpp:599-621) and the violation-k promotion loop (src/vol_curve.cpp:436-497; strict recovery promotion src/pricer_fitter.cpp:1478-1496) apply `w_prev` at EVERY node/lattice point in the ±0.60 band, including k where the previous slice had zero admitted data — so the 2025-04-24 seed's data-free `w(+0.15)=0.1208` became a hard floor on all 10 later slices (measured identical out to T=2.686).
- Containment: floor rows and violation promotion apply ONLY for k inside `[prev_lo - 0.10, prev_hi + 0.10]` where `(prev_lo, prev_hi)` is the previous COMMITTED slice's admitted-observation k-range. This is the principle the codebase already states for parametric projections at src/curve_fit.cpp:694-698 ("A crossing outside both slices' quoted ranges is extrapolation-vs-extrapolation and must not move the level") — extended to the ConvexDense QP.
- Refuse rather than propagate: the shared-k scan STILL samples the full legacy lattice ([-0.60, 0.60], 64 intervals — the exact grid the risk oracle checks; src/vol_curve.cpp:460-465) at the existing tolerance. A violation INSIDE the support band promotes + refits exactly as today. A violation OUTSIDE it returns `Err(ErrorCode::Unavailable, kCalendarFloorUnsupportedMsg)` — a SOFT code, so `fit_curve_surface` drops the slice into the existing `FitFailed` lane (src/curve_fit.cpp:781-799, `prep_error_is_expected(Unavailable)` keeps the board alive) and the surface truncates there. The published surface therefore NEVER contains a pair that crosses anywhere on the oracle lattice: what is published still passes the untouched risk oracle (`kQpCertificateTol`/`kQpActiveTol`/`kQpStartTol` and `RiskSurfaceValidationConfig` untouched). We refuse the region instead of ratcheting it — exactly the sanctioned trade.
- Why calm days stay byte-identical: healthy SPY slices' admitted ranges are WIDE — even the starved 2020-03-18 T=1.7496 slice spans k [-2.052, +0.587] (inv_starvation.md §3), and 60-row calm slices span wider — so `[prev_lo-0.10, prev_hi+0.10]` covers the entire ±0.60 enforcement band: every floor row survives, the scan classifies nothing as unsupported, and the QP is IDENTICAL (same rows, same order). The only boards that can change are ones with a narrow-range slice (freshly-listed dailies) — which Task 1 already mostly refuses; Task 3 is defense-in-depth for a narrow-but-straddling slice that passes Task 1.
- Residual accepted (documented, not hidden): when a narrow slice IS published (passed Task 1) and its extrapolated wing exceeds a later slice's natural w outside the band, the LATER slices now refuse and the surface truncates at the poisoned point — the poison stays contained to its own slice instead of being stamped onto ten. The truncation is loud (tenor-audit / band-audit / `n_slice_calendar_unsupported`). The eviction alternative (pop the earlier slice and refit) was considered and rejected for this pass: it requires mutating `CurveSurface`/context/report bookkeeping mid-walk for a case Task 1 makes rare. Revisit only with band-audit evidence.
- Out of scope (documented): the per-slice LinearVariance fallback's union-grid floor (src/curve_fit.cpp:731-758) has the same unrestricted-`w_prev` shape but only runs under `CalibOpts::per_slice_linear_fallback`, which the populate path does not enable. Leave it; note it in the Task 5 report if band-audit ever implicates it.

- [ ] **Step 1: Write the failing tests** — append to `tests/dense_slice_test.cpp` (uses the file's existing `mk_obs` helper at :39-52 and its `using` block):

```cpp
// ── Task 3: calendar-floor ratchet containment ───────────────────────────────
// A narrow front slice's DATA-FREE wing must not pin later slices (the
// 2025-04-10 ratchet: seed w(+0.15)=0.1208 stamped onto all 10 later slices).
// With containment, the pair REFUSES (soft Unavailable) instead of ratcheting;
// the board fitter turns that into a loud truncation.
TEST(ConvexSliceFit, DataFreeFrontWingRefusesInsteadOfRatcheting) {
  using namespace atx::vol;
  constexpr double F = 100.0, df = 0.99;
  const double T0 = 0.04, T1 = 0.50;
  std::vector<FitObs> front; // freshly-listed-daily: data only in |k| <= 0.09
  for (double k = -0.09; k <= 0.091; k += 0.02) {
    front.push_back(mk_obs(F, T0, df, F * std::exp(k), 0.60));
  }
  std::vector<FitObs> back; // calm dense slice across the full band
  for (double k = -0.60; k <= 0.601; k += 0.05) {
    back.push_back(mk_obs(F, T1, df, F * std::exp(k), 0.20));
  }
  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;

  const auto front_curve = fit_slice_curve(cfg, front, F, T0, df);
  ASSERT_TRUE(front_curve.has_value()) << front_curve.error().to_string();
  const IVolCurve *prev = front_curve->get();
  // Fixture self-check: the front's EXTRAPOLATED wing really exceeds the back
  // slice's natural total variance out there (else there is nothing to
  // contain). If this ever fails, raise the front vol (0.60 -> 0.80).
  ASSERT_GT(prev->w(0.5), 0.20 * 0.20 * T1);

  const std::function<double(double)> w_prev = [prev](double k) { return prev->w(k); };
  const auto contained =
      fit_slice_curve(cfg, back, F, T1, df, w_prev,
                      /*calendar_floor_knots=*/{},
                      /*prev_data_k_range=*/{-0.09, 0.09});
  ASSERT_FALSE(contained.has_value())
      << "pair fitted: the data-free wing was ratcheted, not contained";
  EXPECT_EQ(contained.error().code(), ErrorCode::Unavailable);
  EXPECT_EQ(contained.error().message(), std::string(kCalendarFloorUnsupportedMsg));
}

// Containment must be INVISIBLE when the previous slice's data spans the
// checked lattice: identical constraint rows => bit-identical served values.
TEST(ConvexSliceFit, WideSupportRangeIsByteIdenticalToUnbounded) {
  using namespace atx::vol;
  constexpr double F = 100.0, df = 0.99;
  const double T0 = 0.25, T1 = 0.50;
  std::vector<FitObs> front;
  std::vector<FitObs> back;
  for (double k = -0.70; k <= 0.701; k += 0.05) {
    front.push_back(mk_obs(F, T0, df, F * std::exp(k), 0.24));
    back.push_back(mk_obs(F, T1, df, F * std::exp(k), 0.24));
  }
  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;
  const auto front_curve = fit_slice_curve(cfg, front, F, T0, df);
  ASSERT_TRUE(front_curve.has_value()) << front_curve.error().to_string();
  const IVolCurve *prev = front_curve->get();
  const std::function<double(double)> w_prev = [prev](double k) { return prev->w(k); };

  const auto unbounded = fit_slice_curve(cfg, back, F, T1, df, w_prev);
  const auto ranged = fit_slice_curve(cfg, back, F, T1, df, w_prev,
                                      /*calendar_floor_knots=*/{},
                                      /*prev_data_k_range=*/{-0.70, 0.70});
  ASSERT_TRUE(unbounded.has_value()) << unbounded.error().to_string();
  ASSERT_TRUE(ranged.has_value()) << ranged.error().to_string();
  for (double k = -0.60; k <= 0.601; k += 0.03) {
    EXPECT_EQ((*unbounded)->iv(k), (*ranged)->iv(k)) << "diverged at k=" << k; // EXACT
  }
}
```

(`ErrorCode` / `std::function` are already in this TU's vocabulary — `fit_slice_curve` floor tests at :641-673 use both patterns. Add `using atx::vol::kCalendarFloorUnsupportedMsg;` alongside if the unqualified name is not found.)

- [ ] **Step 2: Run to verify they fail**

Run: build, then `--gtest_filter='ConvexSliceFit.DataFreeFrontWingRefusesInsteadOfRatcheting:ConvexSliceFit.WideSupportRangeIsByteIdenticalToUnbounded'`
Expected: COMPILE failure (`kCalendarFloorUnsupportedMsg` undeclared). After Step 3.1 declares the constant only, `DataFreeFrontWingRefuses...` FAILS at `ASSERT_FALSE(contained.has_value())` — today the pair FITS with the back slice lifted onto the front's data-free wing (the ratchet, demonstrated). `WideSupportRange...` should PASS even pre-change (the parameter is currently ignored by this arm) — it is the pinned regression for after.

- [ ] **Step 3: Implement the containment**

1. `include/atx/vol/vol_curve.hpp` — near `ConvexRepairSpec` (:423), add (plus `#include <string_view>` if absent):

```cpp
// Task 3: the ConvexDense refusal for a calendar-floor breach OUTSIDE the
// previous slice's data-supported k-range (extrapolation-vs-extrapolation —
// curve_fit.cpp's tradeable-overlap principle). A SHARED constant so
// fit_curve_surface can count these refusals (n_slice_calendar_unsupported)
// without string drift. Soft (Unavailable): the board fitter drops the slice
// and truncates, like any other thin-slice refusal — the earlier slice's
// data-free wing is refused, never propagated.
inline constexpr std::string_view kCalendarFloorUnsupportedMsg =
    "fit_slice_curve: calendar floor breach beyond previous slice's "
    "data-supported k-range";
```

2. `include/atx/vol/dense_slice.hpp` — add beside `ConvexFitContext` (:121-133), plus `#include <limits>` / `#include <utility>` if absent:

```cpp
// Task 3: margin (log-moneyness) added to the previous slice's observed
// k-range when bounding calendar-floor authority. Wide enough that ordinary
// edge extrapolation between two dense slices stays floored; narrow enough
// that a 0.05-wide freshly-listed strip cannot pin the ±0.60 band.
inline constexpr double kCalendarFloorSupportMargin = 0.10;
```

and the new `ConvexFitContext` field:

```cpp
  // Task 3 (ratchet containment): the k-interval over which a supplied w_prev
  // carries floor authority — the previous slice's data range widened by
  // kCalendarFloorSupportMargin (set by fit_slice_curve). Nodes outside it
  // get NO calendar-floor row: an earlier slice's data-free extrapolated wing
  // must not pin this slice. Default (infinite) keeps every caller
  // byte-identical.
  std::pair<double, double> floor_support_k{
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::infinity()};
```

3. `src/dense_slice.cpp` — in the calendar-floor row loop (:599-621), gate each node:

```cpp
  if (w_prev) {
    for (Eigen::Index j = 0; j < N; ++j) {
      const double k = std::log(un(j) / F);
      if (k < context.floor_support_k.first || k > context.floor_support_k.second) {
        continue; // Task 3: no floor authority beyond the prev slice's data
      }
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

4. `src/vol_curve.cpp` — ConvexDense arm (:400-501). After the `repair` validation block (:411-420), derive the support band from the ALREADY-PASSED `prev_data_k_range` parameter:

```cpp
    // Task 3: floor/promotion authority is bounded by the previous slice's
    // data-supported range (+margin). Infinite when the caller supplied no
    // range — every existing caller is byte-identical.
    const double floor_lo = prev_data_k_range.first - kCalendarFloorSupportMargin;
    const double floor_hi = prev_data_k_range.second + kCalendarFloorSupportMargin;
```

In the pass loop, populate the context (:437-439):

```cpp
      ConvexFitContext context;
      context.required_k = std::span<const double>{required_k};
      context.noise_aware_regularization = true;
      context.floor_support_k = {floor_lo, floor_hi};
```

Classify scan hits (the `scan_k` lambda, :451-459) and refuse unsupported ones after the scan:

```cpp
      std::vector<double> violations;
      violations.reserve(8);
      bool unsupported_violation = false;
      const auto scan_k = [&](double k) {
        const double wp = w_prev(k);
        const double wc = fit.iv(k);
        const double w_curr = (std::isfinite(wc) && wc > 0.0) ? wc * wc * T : kNaN;
        if (std::isfinite(wp) && std::isfinite(w_curr) &&
            wp - w_curr > calendar_tol) {
          if (k < floor_lo || k > floor_hi) {
            unsupported_violation = true; // extrapolation-vs-extrapolation
          } else {
            violations.push_back(k);
          }
        }
      };
      /* ...the two sampling branches (:460-478) are UNCHANGED: same grids,
         same inclusive formulas, same order... */
      if (unsupported_violation) {
        // Refuse, never ratchet: promoting an out-of-support k would stamp the
        // previous slice's data-free wing onto this slice (the 2025-04-10
        // w(+0.15)=0.1208 plateau). Publishing without the floor would break
        // calendar order on the oracle lattice. Soft-drop => truncation.
        return Err(ErrorCode::Unavailable, std::string(kCalendarFloorUnsupportedMsg));
      }
      if (violations.empty()) {
        std::unique_ptr<IVolCurve> curve =
            std::make_unique<ConvexDenseCurve>(std::move(fit));
        return Ok(std::move(curve));
      }
      /* ...promotion + stall handling (:484-497) UNCHANGED... */
```

5. `include/atx/vol/curve_fit.hpp` — after `n_slices_uncovered` (Task 1's field):

```cpp
  // Task 3: slices refused because the previous slice's calendar floor bound
  // them only where that slice had no admitted data (the seed-ratchet shape;
  // kCalendarFloorUnsupportedMsg). Truncation follows; surfaced, never silent.
  std::size_t n_slice_calendar_unsupported{0};
```

6. `src/curve_fit.cpp` — in the fit-failure branch, immediately before the `ExpiryFitReport rep{}` for `FitFailed` (:786-794):

```cpp
        if (fit_code == ErrorCode::Unavailable &&
            slice_res.error().message() == kCalendarFloorUnsupportedMsg) {
          ++out.n_slice_calendar_unsupported; // Task 3: refused, not ratcheted
        }
```

- [ ] **Step 4: Run the new tests to verify they pass**

Run: `--gtest_filter='ConvexSliceFit.DataFreeFrontWingRefusesInsteadOfRatcheting:ConvexSliceFit.WideSupportRangeIsByteIdenticalToUnbounded'`
Expected: 2 PASS.

- [ ] **Step 5: Targeted regression group (the calendar-floor surface)**

Run: `--gtest_filter='ConvexSliceFit.*:VolCurve*.*:SurfaceV2Qualification*.*:SpyBidAskRegression.*'`
Expected: PASS. `SharedKCalendarRefitPreservesSliceConvexity` (dense_slice_test.cpp:641-673) and surface_v2_qualification_test.cpp:534's whole-slice-lift case both use WIDE two-sided fixtures, so their floors are entirely in-support and byte-identical. `SpyBidAskRegression` GTEST_SKIPs without the parquet fixture; if the fixture is present it must hold its 94.0 floor unchanged.

- [ ] **Step 6: Commit**

```bash
git add atx-vol/include/atx/vol/dense_slice.hpp atx-vol/src/dense_slice.cpp \
        atx-vol/include/atx/vol/vol_curve.hpp atx-vol/src/vol_curve.cpp \
        atx-vol/include/atx/vol/curve_fit.hpp atx-vol/src/curve_fit.cpp \
        atx-vol/tests/dense_slice_test.cpp
git commit -m "feat(vol): bound ConvexDense calendar floor to prev slice's data-supported k-range"
```

---

### Task 4: `band-audit` — full-chain quote-fidelity inspector (surface-db admin)

**Files:**
- Modify: `tools/include/atx/vol/tools/surface_db_admin.hpp` (new section after `tenor_audit` :829-830)
- Modify: `src/surface_db_admin.cpp` (implementation; new includes: `atx/vol/opra_hive.hpp`, `atx/vol/opra_batch.hpp` for `corpus_board_from_opra`, `atx/vol/corpus.hpp` for `CorpusBoard`, `atx/vol/data.hpp`/`universe.hpp` for `OptionChain::from_frame`/`Chain`/`chain_index`, `atx/vol/fit_metrics.hpp` for `band_violation_stats` — both `opra_hive.cpp` and `surface_db_admin.cpp` already live in the same library target, CMakeLists.txt:76/:96, so no new link edges)
- Modify: `tools/surface_db_main.cpp` (usage text :14-58 and :183-215 region; flag parse loop :884-...; dispatch :1089-1090 region)
- Test: `tests/surface_db_admin_test.cpp`

**Interfaces (pattern: `tenor_audit`, surface_db_admin.hpp:703-830 / surface_db_admin.cpp:491-615 / surface_db_main.cpp:816-853):**

```cpp
// ── band_audit — full-chain quote-fidelity inspector ─────────────────────────
// AFTER-THE-FACT counterpart of the fit's own parity diagnostics, measured
// against the FULL LISTED CHAIN instead of the fit's admitted rows: for each
// (date, symbol) cell, load the stored surface AND the OPRA hive chain at the
// build snapshot, reprice every two-sided listed quote through
// PricedSurface::fair_value, and report per-expiry band statistics. This is
// the detector for the 2020-03-18 / 2025-04-10 defect class (model above the
// ask of the whole listed belly/wing) and the before/after instrument for the
// fix validation. AN AUDIT, NOT A GATE (same stance as tenor_audit): every
// row prints; the exit-code opt-in lives on the CLI.

struct BandAuditRow {
  std::string date{};
  std::string symbol{};
  double T{0.0};                           // expiry year-fraction at the snapshot
  std::size_t n{0};                        // scored quotes (two-sided, finite model)
  double frac_in_band{0.0};                // bid <= model <= ask
  double frac_above_ask{0.0};              // model > ask (the spike signature)
  double avg_signed_err_half_spreads{0.0}; // mean (model - mid) / half-spread
  double max_abs_err_half_spreads{0.0};    // max |model - mid| / half-spread
  bool flagged{false};                     // n > 0 && frac_in_band < floor
};

struct BandAuditSpec {
  std::vector<std::string> symbols{};      // empty = every manifest symbol
  std::string hive_root{};                 // REQUIRED: OPRA hive v2 root
  std::string date_lo{};                   // empty = first partition key
  std::string date_hi{};                   // empty = last partition key
  std::string snapshot_suffix{"T19:55:00Z"};
  double r{0.0};                           // flat fallback rate for the chain env
  double min_frac_in_band{0.30};           // per-expiry flag floor
  std::size_t max_skip_notes{kSurfaceDbVerifyMaxFailures};
};

struct BandAuditReport {
  // Date-major (the hive loads one file per date), symbol-major within a
  // date, ascending T within a cell.
  std::vector<BandAuditRow> rows{};
  std::size_t n_flagged{0};
  std::vector<std::string> skip_notes{};   // capped like TenorAuditReport
  std::size_t n_skip_notes_elided{0};
};

// Pure per-expiry scorer (unit-tested): a quote is scored when bid > 0,
// ask > bid and model_price is finite; counts via band_violation_stats
// (fit_metrics.hpp:192-195, which skips the same rows), half-spread stats
// over the same scored subset. date/symbol/T are left default for the caller.
[[nodiscard]] BandAuditRow score_expiry_band(std::span<const double> model_price,
                                             std::span<const double> bid,
                                             std::span<const double> ask,
                                             double min_frac_in_band);

// Err: InvalidArgument for an empty hive_root; NotFound when spec.symbols
// names an unconfigured symbol (the tenor_audit stance — checked BEFORE any
// hive IO so a typo fails loud without a hive present). A cell that fails to
// load (surface or chain) is a skip note, never fatal.
[[nodiscard]] Result<BandAuditReport> band_audit(const SurfaceDb &db,
                                                 const BandAuditSpec &spec);
```

**CLI:** `band-audit --db R --hive H [--symbol SYM] [--from D] [--to D] [--r X] [--min-frac X] [--fail-on-flagged]`. TSV: `date  symbol  T  n  frac_in_band  frac_above_ask  avg_signed_hs  max_abs_hs  flag` (flag = `BELOWFLOOR` or empty). Exit 0 normally; 3 iff `--fail-on-flagged` and `n_flagged > 0` (the tenor-audit precedent for reusing 3, surface_db_main.cpp:107-114); 1 runtime failure; 2 usage (missing `--hive` included).

- [ ] **Step 1: Write the failing unit tests** — append to `tests/surface_db_admin_test.cpp` (after the TenorAudit section, reusing its fixture idiom at :1737-1789):

```cpp
// ── band_audit — per-expiry scorer + spec validation ─────────────────────────

TEST(BandAudit, ScoreExpiryBandCountsAndHalfSpreadStats) {
  // Bands [9,11] [19,21] [29,31] [39,41] (mids 10/20/30/40, half-spread 1).
  // Model: 10 (in-band), 22 (+2 half-spreads, above ask), 28.5 (-1.5, below
  // bid), 40.5 (+0.5, in-band).
  const double model[] = {10.0, 22.0, 28.5, 40.5};
  const double bid[] = {9.0, 19.0, 29.0, 39.0};
  const double ask[] = {11.0, 21.0, 31.0, 41.0};
  const auto row = atx::vol::score_expiry_band(model, bid, ask, 0.30);
  EXPECT_EQ(row.n, 4u);
  EXPECT_DOUBLE_EQ(row.frac_in_band, 0.5);
  EXPECT_DOUBLE_EQ(row.frac_above_ask, 0.25);
  EXPECT_DOUBLE_EQ(row.avg_signed_err_half_spreads, (0.0 + 2.0 - 1.5 + 0.5) / 4.0);
  EXPECT_DOUBLE_EQ(row.max_abs_err_half_spreads, 2.0);
  EXPECT_FALSE(row.flagged); // 0.5 >= 0.30
}

TEST(BandAudit, ScoreExpiryBandSkipsUnscorableQuotesAndFlagsBelowFloor) {
  // Zero-bid, crossed, and NaN-model quotes are skipped; the one scored quote
  // sits above the ask, so the row flags at floor 0.30 — the 2020-03-18
  // signature in miniature.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double model[] = {5.0, 5.0, nan, 33.0};
  const double bid[] = {0.0, 6.0, 9.0, 29.0};
  const double ask[] = {2.0, 5.5, 11.0, 31.0};
  const auto row = atx::vol::score_expiry_band(model, bid, ask, 0.30);
  EXPECT_EQ(row.n, 1u);
  EXPECT_DOUBLE_EQ(row.frac_in_band, 0.0);
  EXPECT_DOUBLE_EQ(row.frac_above_ask, 1.0);
  EXPECT_TRUE(row.flagged);
}

TEST(BandAudit, EmptyExpiryIsAZeroRowNeverFlagged) {
  const auto row = atx::vol::score_expiry_band({}, {}, {}, 0.30);
  EXPECT_EQ(row.n, 0u);
  EXPECT_FALSE(row.flagged);
}

TEST(BandAudit, RejectsAnUnconfiguredSymbolBeforeAnyHiveIo) {
  // The tenor_audit stance (surface_db_admin.cpp:536-556): a typo'd --symbol
  // must fail loud, not audit zero rows. The bogus hive_root proves no hive
  // IO happens before the check.
  const fs::path root = fresh_tenor_dir("band_audit_unconfigured");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  ASSERT_TRUE(db->upsert_symbol("AAA", symbol_config_from_preset(FitPreset::Populate)).has_value());
  atx::vol::BandAuditSpec spec;
  spec.symbols = {"AAAX"};
  spec.hive_root = (root / "no-such-hive").string();
  const auto rep = atx::vol::band_audit(*db, spec);
  ASSERT_FALSE(rep.has_value());
  EXPECT_EQ(rep.error().code(), ErrorCode::NotFound);
  fs::remove_all(root);
}

TEST(BandAudit, EmptyHiveRootIsInvalidArgument) {
  const fs::path root = fresh_tenor_dir("band_audit_no_hive");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  const auto rep = atx::vol::band_audit(*db, atx::vol::BandAuditSpec{});
  ASSERT_FALSE(rep.has_value());
  EXPECT_EQ(rep.error().code(), ErrorCode::InvalidArgument);
  fs::remove_all(root);
}
```

(The end-to-end reprice path — hive parquet -> chain -> `fair_value` — is deliberately NOT synthesized in-tree: writing a parquet hive fixture would dwarf the feature. It is validated against the real hive + r2 db in Task 5 Step 2, where 2020-03-18's known numbers pin it: iv(k=+0.25)=40.9 published vs nearest quoted call 22.8 must show up as `frac_above_ask ~ 1` on the T=1.75 row.)

- [ ] **Step 2: Run to verify they fail**

Run: build `atx-vol-tests`.
Expected: COMPILE failure — `BandAuditSpec` / `score_expiry_band` / `band_audit` undeclared.

- [ ] **Step 3: Add the header declarations** — paste the Interfaces block above into `tools/include/atx/vol/tools/surface_db_admin.hpp` after the `tenor_audit` declaration (:829-830). Add `#include <span>` if not present.

- [ ] **Step 4: Implement the library** — `src/surface_db_admin.cpp`, after `tenor_audit` (:615):

```cpp
// ── band_audit ───────────────────────────────────────────────────────────────

BandAuditRow score_expiry_band(std::span<const double> model_price,
                               std::span<const double> bid,
                               std::span<const double> ask,
                               double min_frac_in_band) {
  BandAuditRow row;
  const std::size_t n_in =
      std::min({model_price.size(), bid.size(), ask.size()});
  std::vector<double> m, b, a;
  m.reserve(n_in);
  b.reserve(n_in);
  a.reserve(n_in);
  double sum_signed = 0.0;
  double max_abs = 0.0;
  for (std::size_t i = 0; i < n_in; ++i) {
    const double p = model_price[i];
    if (!(bid[i] > 0.0) || !(ask[i] > bid[i]) || !std::isfinite(p)) {
      continue; // one-sided / crossed / failed model — unscorable
    }
    const double mid = 0.5 * (bid[i] + ask[i]);
    const double half = 0.5 * (ask[i] - bid[i]); // > 0 by the gate above
    const double err_hs = (p - mid) / half;
    sum_signed += err_hs;
    max_abs = std::max(max_abs, std::fabs(err_hs));
    m.push_back(p);
    b.push_back(bid[i]);
    a.push_back(ask[i]);
  }
  row.n = m.size();
  if (row.n == 0) {
    return row; // never flagged: nothing was measurable
  }
  // Counts via the shared scorer (its skip set is empty after the filter
  // above, so `stats.n == row.n` by construction).
  const auto stats = band_violation_stats(m, b, a);
  if (stats.has_value() && stats->n > 0) {
    const double n_d = static_cast<double>(stats->n);
    row.frac_in_band =
        static_cast<double>(stats->n - stats->n_bid_miss - stats->n_ask_miss) / n_d;
    row.frac_above_ask = static_cast<double>(stats->n_ask_miss) / n_d;
  }
  row.avg_signed_err_half_spreads = sum_signed / static_cast<double>(row.n);
  row.max_abs_err_half_spreads = max_abs;
  row.flagged = row.frac_in_band < min_frac_in_band;
  return row;
}

Result<BandAuditReport> band_audit(const SurfaceDb &db, const BandAuditSpec &spec) {
  if (spec.hive_root.empty()) {
    return Err(ErrorCode::InvalidArgument, "band_audit: hive_root is required");
  }
  // Symbol resolution — the tenor_audit stance, verbatim (loud NotFound on an
  // unconfigured explicit name, BEFORE any hive IO).
  std::vector<std::string> symbols;
  if (!spec.symbols.empty()) {
    const std::vector<std::string> configured = db.symbols();
    symbols.reserve(spec.symbols.size());
    for (const std::string &s : spec.symbols) {
      const std::string canon = canonical_ascii(s);
      if (std::find(configured.begin(), configured.end(), canon) == configured.end()) {
        return Err(ErrorCode::NotFound,
                   "band_audit: symbol '" + canon + "' is not configured in this database");
      }
      symbols.push_back(canon);
    }
  } else {
    symbols = db.symbols();
  }

  std::vector<std::string> dates;
  for (const DbPartitionInfo &p : db.partitions()) { // sorted ascending
    if (!spec.date_lo.empty() && p.key < spec.date_lo) continue;
    if (!spec.date_hi.empty() && p.key > spec.date_hi) continue;
    dates.push_back(p.key);
  }

  BandAuditReport out;
  const auto note = [&](std::string text) {
    if (out.skip_notes.size() < spec.max_skip_notes) {
      out.skip_notes.push_back(std::move(text));
    } else {
      ++out.n_skip_notes_elided;
    }
  };

  for (const std::string &date : dates) {
    OpraHiveSpec hs;
    hs.root_dir = spec.hive_root;
    hs.date_lo = date;
    hs.date_hi = date;
    hs.symbols = symbols;
    hs.snapshot_suffix = spec.snapshot_suffix;
    hs.r = spec.r;
    Result<OpraBatchResult> loaded = load_opra_hive(hs);
    if (!loaded) {
      note(date + ": hive load failed: " + loaded.error().to_string());
      continue;
    }
    for (OpraBatchEntry &entry : loaded->entries) {
      if (!entry.panel.has_value()) {
        note(date + " " + entry.symbol + ": " + entry.panel.error().to_string());
        continue;
      }
      const Result<PricedSurface> surf = db.load_surface(date, entry.symbol);
      if (!surf) {
        note(date + " " + entry.symbol + ": surface: " + surf.error().to_string());
        continue;
      }
      CorpusBoard board =
          corpus_board_from_opra(date, entry.symbol, std::move(*entry.panel));
      auto chain = OptionChain::from_frame(board.frame, board.env);
      if (!chain) {
        note(date + " " + entry.symbol + ": chain: " + chain.error().to_string());
        continue;
      }
      for (const Chain &c : chain->underlying().chains) {
        std::vector<double> model, cb, ca;
        const std::size_t n_str = c.strikes.size();
        model.reserve(2 * n_str);
        cb.reserve(2 * n_str);
        ca.reserve(2 * n_str);
        for (std::size_t i = 0; i < n_str; ++i) {
          for (const Side side : {Side::Call, Side::Put}) {
            const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
            const double qb = c.bids[idx];
            const double qa = c.asks[idx];
            if (!(qb > 0.0) || !(qa > qb)) {
              continue; // unscorable quote — keep spans aligned by skipping here
            }
            const Result<double> fv = surf->fair_value(c.strikes[i], c.T, side);
            model.push_back(fv.has_value() ? *fv
                                           : std::numeric_limits<double>::quiet_NaN());
            cb.push_back(qb);
            ca.push_back(qa);
          }
        }
        BandAuditRow row = score_expiry_band(model, cb, ca, spec.min_frac_in_band);
        row.date = date;
        row.symbol = entry.symbol;
        row.T = c.T;
        if (row.flagged) {
          ++out.n_flagged;
        }
        out.rows.push_back(std::move(row));
      }
    }
  }
  return Ok(std::move(out));
}
```

- [ ] **Step 5: Run the unit tests to verify they pass**

Run: `--gtest_filter='BandAudit.*'`
Expected: 5 PASS.

- [ ] **Step 6: Wire the CLI** — `tools/surface_db_main.cpp`, mirroring `run_tenor_audit` (:816-853) and the parse loop (:884 onward):

1. New locals in `main` beside `fail_on_truncated` (:882): `std::string hive_root; double band_r = 0.0; double band_min_frac = 0.30; bool fail_on_flagged = false;`
2. New flag branches in the parse loop (same `nv()`/`missing_value` discipline as `--db` at :899-900; numeric flags reuse the strict-parse pattern of `--strike` at :905-914 but accept 0 for `--r`, so parse with the same helper used by `--probe-tenor`'s non-negative parse if one exists, else `parse_positive_double` for `--min-frac` and a finite-parse for `--r`): `--hive`, `--r`, `--min-frac`, `--fail-on-flagged` (boolean, no value).
3. New runner:

```cpp
int run_band_audit(const SurfaceDb &db, const std::string &symbol, const std::string &hive_root,
                   const std::string &from, const std::string &to, double r, double min_frac,
                   bool fail_on_flagged) {
  BandAuditSpec spec;
  if (!symbol.empty()) {
    spec.symbols.push_back(symbol);
  }
  spec.hive_root = hive_root;
  spec.date_lo = from;
  spec.date_hi = to;
  spec.r = r;
  spec.min_frac_in_band = min_frac;
  const Result<BandAuditReport> rep = band_audit(db, spec);
  if (!rep) {
    std::fprintf(stderr, "atx-vol-surface-db: band_audit: %s\n", rep.error().to_string().c_str());
    return 1;
  }
  for (const std::string &note : rep->skip_notes) {
    std::fprintf(stderr, "atx-vol-surface-db: band-audit: skipped %s\n", note.c_str());
  }
  if (rep->n_skip_notes_elided > 0) {
    std::fprintf(stderr, "atx-vol-surface-db: band-audit: %zu additional skip note(s) elided.\n",
                 rep->n_skip_notes_elided);
  }
  std::printf("date\tsymbol\tT\tn\tfrac_in_band\tfrac_above_ask\tavg_signed_hs\tmax_abs_hs\tflag\n");
  for (const BandAuditRow &row : rep->rows) {
    std::printf("%s\t%s\t%.6f\t%zu\t%.4f\t%.4f\t%.3f\t%.3f\t%s\n", row.date.c_str(),
                row.symbol.c_str(), row.T, row.n, row.frac_in_band, row.frac_above_ask,
                row.avg_signed_err_half_spreads, row.max_abs_err_half_spreads,
                row.flagged ? "BELOWFLOOR" : "");
  }
  if (fail_on_flagged && rep->n_flagged > 0) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: band-audit: %zu row(s) BELOWFLOOR; failing per "
                 "--fail-on-flagged.\n",
                 rep->n_flagged);
    return 3; // the tenor-audit precedent: the one deliberate reuse of 3
  }
  return 0;
}
```

4. Dispatch beside `tenor-audit` (:1089-1090): `band-audit` requires `--db` AND `--hive` (missing `--hive` = usage error, exit 2, before the db opens); reuses `verify_spec.key_lo/key_hi` for `--from`/`--to` or the new locals — keep whichever the parse loop already fills, and pass `symbol` through. Register `band-audit` in the `--help` text (:44-58, :206-215 region) and in the subcommand-recognition list (:1009 region).

- [ ] **Step 7: Build the CLI and smoke it (read-only against production, allowed)**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake --build C:\atx\.claude\worktrees\strangle-backtest\build-rel --target atx-vol-surface-db atx-vol-tests'
# READ-ONLY smoke on the production 2020 db (never written) + production hive:
C:\atx\.claude\worktrees\strangle-backtest\build-rel\bin\atx-vol-surface-db.exe band-audit --db C:/atx-data/surface-db/spy-2020 --hive C:/atx-data/opra-hive --symbol SPY --from 2020-03-18 --to 2020-03-18 --r 0.003
```

Expected: TSV rows for every 2020-03-18 SPY expiry; the T~1.75 / T~1.85 / T~2.75 rows show `frac_above_ask` near 1.0 and `BELOWFLOOR` (the dossier's belly: iv(k=+0.25)=40.9 published vs nearest quoted call at 22.8). Short-dated rows score high. If the production db root differs in name, list `C:/atx-data/surface-db/` first — but NEVER write there.

- [ ] **Step 8: Commit**

```bash
git add atx-vol/tools/include/atx/vol/tools/surface_db_admin.hpp atx-vol/src/surface_db_admin.cpp \
        atx-vol/tools/surface_db_main.cpp atx-vol/tests/surface_db_admin_test.cpp
git commit -m "feat(vol): add surface-db band-audit subcommand (full-chain in-band inspector)"
```

---

### Task 5: Validation loop — rebuild known-bad windows into r2, band-audit before/after, re-run the LEAPS strangle backtest

**Files:** none in-repo (operational task; artifacts land in the session scratchpad `C:\Users\natha\AppData\Local\Temp\claude\c--atx\12e0463a-eda0-409d-9fe5-f610a091f48b\scratchpad\validation\`). One optional repo artifact at Step 8.

**Interfaces:**
- Consumes: `atx-vol-surface-db-build.exe` (`--db --hive --from --to --symbols --index --preset --r`, tools/surface_db_build_main.cpp:14-31), `atx-vol-surface-db.exe band-audit` (Task 4), `atx-vol-spy-leaps-strangle.exe` (`--out --db-prefix --from --to --mark-domain extrapolate|carry`, examples/spy_leaps_strangle_backtest.cpp:56-77), populate report `slice_drop.*` rows spelling `PrepUncovered` (Task 1) and `FailedCell` rows (Task 2).
- Sanctioned r2 mutation: delete `C:/atx-data/surface-db-r2/spy-<year>/partitions/<date>.atxvsa` then rebuild (the documented recovery flow, include/atx/vol/surface_db.hpp:364: "DELETE the affected partition files (`<db-root>/partitions/<KEY>.atxvsa`)"). NEVER touch `C:/atx-data/surface-db/spy-*` or `C:/atx-data/opra-hive`.

**Windows and rates** (rates from `data/rates/us_3m_monthly.csv` — one row per month; 2020-03 is 0.003 at :33; read the file for each other month, do not guess):

| Window | db | dates | why |
|---|---|---|---|
| COVID | spy-2020 | 2020-03-16..2020-03-20 | Mode A spike pair 03-18/19 + healthy-neighbour byte-check |
| Liberation Day | spy-2025 | 2025-04-08..2025-04-11 | Mode B seed/ratchet pair 04-10/11 + 04-09 intermediate |
| Jul-24 | spy-2024 | 2024-07-15..2024-07-16 | known-bad pair |
| Aug-24 | spy-2024 | 2024-08-05..2024-08-06 | known-bad pair (VIX spike Monday) |
| Starvation census | spy-2019 / spy-2022 | 2019-10-11; 2022-10-04; 2022-12-19 | severe starvation-census dates |

- [ ] **Step 1: Build all binaries once**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake --build C:\atx\.claude\worktrees\strangle-backtest\build-rel --target atx-vol-surface-db-build atx-vol-surface-db atx-vol-spy-leaps-strangle'
```

- [ ] **Step 2: BEFORE snapshot — band-audit the pre-fix r2 partitions + archive the bytes**

```powershell
$scratch = 'C:\Users\natha\AppData\Local\Temp\claude\c--atx\12e0463a-eda0-409d-9fe5-f610a091f48b\scratchpad\validation'
New-Item -ItemType Directory -Force $scratch | Out-Null
$exe = 'C:\atx\.claude\worktrees\strangle-backtest\build-rel\bin\atx-vol-surface-db.exe'
# Byte archive of every partition this task will delete (rollback + byte-compare):
New-Item -ItemType Directory -Force "$scratch\before-partitions" | Out-Null
$cells = @(
  @{db='spy-2020'; dates=@('2020-03-16','2020-03-17','2020-03-18','2020-03-19','2020-03-20'); r=0.003},
  @{db='spy-2025'; dates=@('2025-04-08','2025-04-09','2025-04-10','2025-04-11'); r=$null},
  @{db='spy-2024'; dates=@('2024-07-15','2024-07-16'); r=$null},
  @{db='spy-2024'; dates=@('2024-08-05','2024-08-06'); r=$null},
  @{db='spy-2019'; dates=@('2019-10-11'); r=$null},
  @{db='spy-2022'; dates=@('2022-10-04','2022-12-19'); r=$null}
)
foreach ($c in $cells) {
  foreach ($d in $c.dates) {
    Copy-Item "C:/atx-data/surface-db-r2/$($c.db)/partitions/$d.atxvsa" "$scratch\before-partitions\$($c.db)_$d.atxvsa" -ErrorAction SilentlyContinue
    & $exe band-audit --db "C:/atx-data/surface-db-r2/$($c.db)" --hive C:/atx-data/opra-hive --symbol SPY --from $d --to $d --r 0.0 | Out-File -Encoding utf8 "$scratch\band_before_$($c.db)_$d.tsv"
  }
}
```

(`--r` for the AUDIT only shapes the chain env, not the stored surface's carry — 0.0 is fine for the before/after DELTA; use the month rate when absolute short-dated rows matter.) Sanity-pin the detector here: `band_before_spy-2020_2020-03-18.tsv` must show the T~1.75/1.85/2.75 rows BELOWFLOOR with `frac_above_ask` near 1.0, and `band_before_spy-2025_2025-04-10.tsv` must show high `frac_above_ask` on the long rows (the w-plateau serving 26.2% where 17.4% is listed). If not, STOP: the detector or the dossier reading is wrong — do not proceed to rebuilds.

- [ ] **Step 3: Rebuild each window into r2** (delete partition, then build; per-month `--r` from `data/rates/us_3m_monthly.csv`)

```powershell
$build = 'C:\atx\.claude\worktrees\strangle-backtest\build-rel\bin\atx-vol-surface-db-build.exe'
# COVID window, explicit (repeat the pattern for every window with its rate):
foreach ($d in '2020-03-16','2020-03-17','2020-03-18','2020-03-19','2020-03-20') {
  Remove-Item "C:/atx-data/surface-db-r2/spy-2020/partitions/$d.atxvsa" -Force -Confirm:$false -ErrorAction SilentlyContinue
}
& $build --db C:/atx-data/surface-db-r2/spy-2020 --hive C:/atx-data/opra-hive `
         --from 2020-03-16 --to 2020-03-20 --symbols SPY --index SPY --preset populate --r 0.003
```

Expected build-report reading (stdout + report CSV): 2020-03-18 shows `slice_drop.*` rows with outcome `PrepUncovered` for the starved long expiries (Task 1's refusals — count them); 2020-03-19 and 2020-03-20 show NO PrepUncovered and NO FailedCell. For 2025-04-08..11: 04-10 (and possibly 04-09) shows the 2025-04-24/22 expiry `PrepUncovered`; IF the residual board still fails the Task 2 floor, 04-10 lands as a `FailedCell` (exit code may be nonzero per the CLI's failure predicates — that is the honest refusal lane, record it, not an error in this plan). 04-11 must fit fully.

- [ ] **Step 4: Healthy-day byte-identity check (the calm-day contract)**

```powershell
foreach ($pair in @(@('spy-2020','2020-03-19'), @('spy-2020','2020-03-20'), @('spy-2025','2025-04-11'))) {
  $dbname, $d = $pair
  $old = "$scratch\before-partitions\${dbname}_$d.atxvsa"
  $new = "C:/atx-data/surface-db-r2/$dbname/partitions/$d.atxvsa"
  $ho = (Get-FileHash $old -Algorithm SHA256).Hash
  $hn = (Get-FileHash $new -Algorithm SHA256).Hash
  "{0} {1}: {2}" -f $dbname, $d, $(if ($ho -eq $hn) {'BYTE-IDENTICAL'} else {'DIFFERS'})
}
```

Expected: BYTE-IDENTICAL for all three (dates fit independently; no gate should trip on them). NOTE: byte-identity additionally requires the rebuild `--r` and binary provenance to match what produced the BEFORE partition — the starvation dossier's own 2020-03-18 rebuild was byte-equivalent under this exact flow. If a healthy date DIFFERS: diff its build-report `slice_drop.*` rows; if any `PrepUncovered`/`FailedCell` appears on a healthy date, recalibrate per Task 1 Step 12 / Task 2 (floor down toward 0.30), rebuild, re-check. Loop until healthy days are clean; the known-bad refusals must survive recalibration (re-check Step 3's expectations).

- [ ] **Step 5: AFTER band-audit + compare**

Re-run Step 2's band-audit loop into `$scratch\band_after_*.tsv`. Acceptance, per file (compare with the paired `band_before_*`):
- 2020-03-18: the poisoned long rows are either ABSENT (expiry refused -> tenor truncation) or in-band; NO row's `frac_in_band` decreases vs before; total BELOWFLOOR row count strictly drops.
- 2025-04-10: the T~0.038 seed row absent (refused) or two-sided-sane; the long-end rows' `frac_above_ask` collapses vs before (the 0.1208 plateau is gone) UNLESS the whole cell became a FailedCell (then band-audit reports it as a skip note "surface: ..." — record that as the honest outcome).
- 2025-04-11, 2020-03-19/20: rows numerically IDENTICAL to before (byte-identical partitions must audit identically).
- Census dates (2019-10-11, 2022-10-04, 2022-12-19): BELOWFLOOR count must not increase; expect truncation instead of high `frac_above_ask` rows.

- [ ] **Step 6: Re-run the SPY LEAPS strangle backtest (both mark domains)**

```powershell
$bt = 'C:\atx\.claude\worktrees\strangle-backtest\build-rel\bin\atx-vol-spy-leaps-strangle.exe'
& $bt --out "$scratch\bt_carry"       --db-prefix C:/atx-data/surface-db-r2/spy --mark-domain carry
& $bt --out "$scratch\bt_extrap"     --db-prefix C:/atx-data/surface-db-r2/spy --mark-domain extrapolate
```

(These two runs ARE the deliverable validation — the pre-fix tearsheets for comparison are whatever the session already has; if none exist, run the same two commands BEFORE Step 3 into `$scratch\bt_carry_before` / `$scratch\bt_extrap_before`. Add that to Step 2 if starting fresh.)

- [ ] **Step 7: Verify the vega-reversal pairs shrink to listed-market magnitude**

The defect signature (dossiers): held-strike marks implied ~±15 vol pt day-pair reversal on 2020-03-18/19 (listed move: +2.65 / -2.1 pt) and ~±4.5 pt on 2025-04-10/11 (listed: +1.2 / -0.2 pt). Read the backtest TSV (`write_backtest_tsv` output in each `--out` dir: one header row, `date`, then pnl/greek columns — tools/include/atx/vol/tools/tearsheet.hpp:185-201) and compare the pair rows:

```bash
python - <<'PY'
import csv, glob, os
scratch = r'C:\Users\natha\AppData\Local\Temp\claude\c--atx\12e0463a-eda0-409d-9fe5-f610a091f48b\scratchpad\validation'
pairs = [('2020-03-18','2020-03-19'), ('2025-04-10','2025-04-11')]
for run in ('bt_carry','bt_extrap','bt_carry_before','bt_extrap_before'):
    for tsv in glob.glob(os.path.join(scratch, run, '*.tsv')):
        with open(tsv, newline='') as f:
            rows = [r for r in csv.reader(f, delimiter='\t') if r and not r[0].startswith('#')]
        hdr = rows[0]
        if 'date' not in hdr: continue
        di = hdr.index('date')
        vcols = [i for i, h in enumerate(hdr) if 'vega' in h.lower() or h.lower() == 'pnl_total']
        byd = {r[di]: r for r in rows[1:]}
        for a, b in pairs:
            if a in byd and b in byd:
                for i in vcols:
                    print(run, os.path.basename(tsv), hdr[i], a, byd[a][i], b, byd[b][i])
PY
```

Acceptance:
- The 2020-03-18/19 pair's equal-and-opposite spike (pnl_vega / pnl_total reversal) shrinks to listed-market magnitude: post-fix pair magnitudes <= 1/3 of the pre-fix pair AND consistent with a <= ~5 vol pt mark move (listed was 2.65 pt; 5 allows carry/model slack). Under `--mark-domain carry` a refused/truncated 03-18 long end means the held position marks CARRIED — the pair spike should nearly vanish.
- The 2025-04-10/11 pair shrinks likewise (target: consistent with <= ~2 vol pt vs the listed 1.2 pt; previously ~±4.5 pt).
- No NEW day-pair reversal anywhere in the run exceeding the pre-fix run's third-largest pair (i.e. the fix must not mint fresh artifacts). Scan: sort |pnl_total| day-pair sums, compare top-10 lists before/after.

- [ ] **Step 8: Write the validation summary + commit**

Summarize per-window: PrepUncovered counts, FailedCells, byte-identity verdicts, band-audit before/after BELOWFLOOR counts, backtest pair magnitudes (before/after) into `docs/superpowers/plans/2026-08-05-strangle-fit-validation-notes.md` (repo, atx-vol worktree) — this is the one repo artifact of Task 5.

```bash
git add atx-vol/docs/superpowers/plans/2026-08-05-strangle-fit-validation-notes.md
git commit -m "docs(vol): starvation/ratchet fix validation — known-bad windows, band-audit, backtest pairs"
```

---

## Execution notes for every task

- Failing-first discipline: where a step's red is a COMPILE failure (new API), that is acceptable red — note it in the step and proceed; where red is a behavioral FAIL (Task 1 Step 7, Task 3 Step 2), the failure output is the demonstration of the defect: paste it into the commit body.
- Never run the full suite. Every test invocation in this plan is `--gtest_filter`-scoped.
- If any pre-existing targeted test fails after a change, read the fixture before touching thresholds; the Global Constraints forbid loosening oracle/QP tolerances (`kQpCertificateTol`/`kQpActiveTol`/`kQpStartTol`) and any existing gate.
- Windows paths in commands are for PowerShell; git commands run fine in either shell from `C:\atx\.claude\worktrees\strangle-backtest`.

## Self-review (performed at planning time)

- Spec coverage: Task 1 = k-coverage criterion (straddle + central-gap, refusal lane PrepUncovered, healthy-day calibration cited); Task 2 = dormant gate wired (0.35 floor justified, admission=nullptr decoy documented, refusal->FailedCell lane verified); Task 3 = ratchet containment (curve_fit.cpp:694-698 principle, oracle untouched, refuse-over-propagate); Task 4 = band-audit subcommand (tenor-audit pattern, band_violation_stats reuse, per-expiry n/frac/half-spread stats, BELOWFLOOR flags, exit-3 opt-in); Task 5 = rebuild/audit/backtest with the exact dossier magnitudes as acceptance. Global constraints copied verbatim into the header.
- Known open risks, flagged for the executor: (a) Task 1 gap-cap 0.40 vs the un-measured 2020-03-18 T=1.0021/T=1.5004 holes — if they pass Task 1, Task 2's floor and Task 3's containment are the backstops, and Task 5 Step 5's band-audit acceptance ("no row's frac_in_band decreases") is the net. (b) Task 3's refusal drops the LATER slice of an unsupported pair; the earlier-slice eviction alternative is documented in Task 3's design and deferred with rationale (Task 1 makes the case rare; revisit only on band-audit evidence). (c) Task 2's floor also arms the MARK arm's parity scoring (`fit_admission_consumes_parity` -> `score_parity=true`), a build-time cost, and fails closed on a manifest that pinned `score_parity=false` — loud, acceptable. (d) Task 3 Step 1's fixture carries a self-check ASSERT (`prev->w(0.5) > back natural w`) so a non-cooperating geometry fails loudly with an in-test adjustment instruction (raise front vol 0.60 -> 0.80).
- Placeholder scan: no TBD/TODO/"handle edge cases"/"similar to Task N" anywhere; every code step carries the actual code; the two spots that depend on unverified runtime facts (test-exe location; TSV column spellings) each carry an explicit discovery command instead of a guess.
- Type consistency: `slice_k_coverage(std::span<const FitObs>)` matches `PreparedSlice::fit_observations()`'s span; Task 3 consumes `fit_slice_curve`'s EXISTING `prev_data_k_range` parameter (vol_curve.hpp:505-511) rather than adding one; `populate_admission_policy()` returns `FitAdmissionPolicy` by value into `PricerConfig::admission` (pricer_fitter.hpp:175); `score_expiry_band`'s spans match `band_violation_stats`' vocabulary (fit_metrics.hpp:192-195); `SlicePrepOutcome::Uncovered` (TU-local) maps 1:1 to the public `ExpiryFitOutcome::PrepUncovered`, spelled `"PrepUncovered"` by `rich_drop_reason_name`.
