# Earnings-Censored ATM Vol Term-Structure Reproduction — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce SpiderRock `tbltickerhistoryv3` ATM earnings-censored implied-vol term
structure (`atmCenI_{5d..504d}`, `atmCenI_st/lt/decay`) and implied per-event move (`iEMove`)
from an OPRA cbbo snapshot + estimated earnings dates, to cohort atmCenI RMSE ≤ 0.002 vol.

**Architecture:** Reuse the existing atx-vol pipeline (OPRA ingest → de-Am American IV → eSSVI
fit → forward-ATM total variance) and add a small censoring/term-fit layer: a global joint
`{eMove, st, lt, decay}` optimizer that censors per-expiry ATM total variance, fits the smooth
`lt+(st−lt)e^(−decay·T)` censored term curve, and samples it on SpiderRock's fixed tenor grid.
Wire event bucketing to be exact (stamp real `expiry_ns`) and unify the censoring path so the
term-structure output uses censored-*space* interpolation.

**Tech Stack:** C++20, clang-cl 18, CMake presets (`dev`), Ninja, GoogleTest (vcpkg), Eigen
(fixed-cap LM), existing atx-vol libs (`atx::vol`), Databento OPRA cbbo parquet (Arrow).

## Global Constraints
- Standard C++20; `.agents/cpp/agent.md` rules: no UB, no narrowing (brace-init), `const`/
  `constexpr`/`noexcept`/`[[nodiscard]]` where they hold, Rule of Zero, `Result<T,E>` for
  expected failures (no throw for control flow), validate inputs at boundary.
- Build: `powershell scripts\atx-build.ps1 configure` then `... build atx-vol-tests` from the
  worktree `C:\atx-wt\earn-vol`. `/W4 /permissive- /WX`; ASan/UBSan clean.
- New `*_test.cpp` appended to the explicit list in `atx-vol/tests/CMakeLists.txt` (before the
  closing `)`); fast unit tests get CTest label `atx_vol_fast`, real-data suites `atx_vol_slow`.
- Vols are decimals (0.30 = 30%); moves are decimal fractions (0.05 = 5%), entering variance as
  `eMove²` per event. Never consume `rvVar` (vendor: "N.A for now").
- Model verbatim: `w_total(T) = n·eMove² + σ_C²·T`; censored `σ_C=sqrt((w−n·eMove²)/T)`;
  term model `atmCen(T)=lt+(st−lt)·exp(−decay·T)`.
- Frequent commits: one commit per task (after its tests pass).
- Namespaces: all new code in `namespace atx::vol`. Reuse `atx/vol/event_vol.hpp`
  (`censored_total_variance`) and `atx/vol/types.hpp` (`Result`, `ErrorCode`).

---

## Task 1: Earnings term-fit core — types + censor primitive

**Files:**
- Create: `atx-vol/include/atx/vol/earnings_term_fit.hpp`
- Create: `atx-vol/src/earnings_term_fit.cpp`
- Test: `atx-vol/tests/earnings_term_fit_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `src/earnings_term_fit.cpp` to the atx-vol lib sources)
- Modify: `atx-vol/tests/CMakeLists.txt` (append `earnings_term_fit_test.cpp` to the explicit list)

**Interfaces:**
- Consumes: `atx::vol::censored_total_variance` (event_vol.hpp), `Result`/`ErrorCode` (types.hpp).
- Produces (later tasks rely on these EXACT names/types):
  ```cpp
  namespace atx::vol {
  struct CensorObsInput {            // one per listed expiry
    double T;                        // year-fraction, convention already applied (>0)
    double w_dirty;                  // total ATM variance = sigma_atm^2 * T (>0)
    std::size_t n;                   // scheduled earnings events before this expiry
  };
  enum class EmoveFitCode : std::uint8_t {
    Ok, Minimum, MaxSteps, LeftBound, RightBound, CenterFlat, Degenerate
  };
  struct SrTenorGrid;               // Task 2
  struct EarningsFitConfig {
    std::span<const double> tenor_T{};  // 12 SR tenor year-fractions (precomputed via tenor_years,
                                        // Task 2) for PARAMETRIC-model sampling of atm_cen (secondary).
    double emove_lo{0.0};
    double emove_hi{0.30};
    double wcen_floor{1e-10};        // reuse event_vol kWCenFloor semantics
    int    max_iters{200};
    // weighting of each expiry in the term-curve LSQ (uniform in v1)
  };
  struct EarningsTermFit {
    double emove{};                  // iEMove
    double st{}, lt{}, decay{};      // parametric censored term curve
    std::vector<double> atm_cen;     // atmCenI sampled at each grid tenor (Task 3)
    double fit_error{};              // RMS residual of censored points vs model
    EmoveFitCode fit_code{EmoveFitCode::Ok};
    std::size_t expiry_count{};
  };
  // censored ATM VOL at expiry i for a candidate eMove: sqrt(max((w-n*e^2)/T, floor)/1)
  [[nodiscard]] double censored_atm_vol(const CensorObsInput& o, double emove,
                                        double wcen_floor) noexcept;
  }
  ```

- [ ] **Step 1: Write the failing test** (`earnings_term_fit_test.cpp`)
```cpp
#include <gtest/gtest.h>
#include "atx/vol/earnings_term_fit.hpp"
#include <cmath>
using namespace atx::vol;

TEST(EarningsTermFit_CensoredAtmVol_StripsEventVariance) {
  // w_dirty = sigma_C^2*T + n*e^2 ; recover sigma_C
  const double T = 0.25, sigmaC = 0.30, e = 0.06;
  const std::size_t n = 1;
  const double w = sigmaC*sigmaC*T + double(n)*e*e;
  CensorObsInput o{T, w, n};
  EXPECT_NEAR(censored_atm_vol(o, e, 1e-10), sigmaC, 1e-9);
}

TEST(EarningsTermFit_CensoredAtmVol_FloorsOnOvershoot) {
  CensorObsInput o{0.02, 1e-6, 3};            // n*e^2 >> w
  const double v = censored_atm_vol(o, 0.10, 1e-10);
  EXPECT_TRUE(std::isfinite(v));
  EXPECT_GE(v, 0.0);
}
```

- [ ] **Step 2: Run test to verify it fails**
Run: `powershell scripts\atx-build.ps1 build atx-vol-tests` (expect: compile error — header/symbols absent).

- [ ] **Step 3: Write header + minimal impl**
Header `earnings_term_fit.hpp`: the types above + declaration. `earnings_term_fit.cpp`:
```cpp
#include "atx/vol/earnings_term_fit.hpp"
#include "atx/vol/event_vol.hpp"
#include <algorithm>
#include <cmath>
namespace atx::vol {
double censored_atm_vol(const CensorObsInput& o, double emove, double wcen_floor) noexcept {
  const double w_cen = censored_total_variance(o.w_dirty, o.n, emove);   // w - n*e^2, floored 1e-10
  const double floored = std::max(w_cen, wcen_floor);
  return std::sqrt(floored / o.T);
}
}
```
Add `src/earnings_term_fit.cpp` to the atx-vol library target in `atx-vol/CMakeLists.txt`
(find the atx-vol `add_library`/source list and append the path in the same style as siblings).

- [ ] **Step 4: Run tests to verify they pass**
Run: `powershell scripts\atx-build.ps1 -Ctest -R EarningsTermFit_CensoredAtmVol`
Expected: 2 PASS.

- [ ] **Step 5: Commit**
```bash
git add atx-vol/include/atx/vol/earnings_term_fit.hpp atx-vol/src/earnings_term_fit.cpp \
        atx-vol/tests/earnings_term_fit_test.cpp atx-vol/CMakeLists.txt atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): earnings term-fit types + censored_atm_vol primitive"
```

---

## Task 2: SpiderRock fixed tenor grid (TRADING days) + calendar-aware tenor→T

**CORRECTION (data-recon):** the SpiderRock tenors are **TRADING days** (5,10,21,42,63,84,105,126,
189,252,378,504 = 21/mo, 63/qtr, 252/yr), and `nEarnCnt_Nd` counts within an N-**trading-day**
horizon. So a tenor's year-fraction is NOT `N/365.25`; it is `time_to_expiry_years(now, advance N
NYSE trading days from now, convention)`. This task provides the grid + the calendar-aware converter.

**Files:**
- Create: `atx-vol/include/atx/vol/sr_tenor_grid.hpp`
- Create: `atx-vol/src/sr_tenor_grid.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `src/sr_tenor_grid.cpp` to the atx-vol lib sources)
- Test: extend `atx-vol/tests/earnings_term_fit_test.cpp`

**Interfaces:**
- Consumes: `VolTimeCalendar` (`us_default`), `TimeSpec`, `time_to_expiry_years`, `kCalendarYearNs`
  (all in `atx/vol/vol_time.hpp`).
- Produces:
  ```cpp
  namespace atx::vol {
  struct SrTenorGrid {
    // SpiderRock censored-term tenors, TRADING days (matches tbltickerhistory columns/nEarnCnt).
    static constexpr std::array<int, 12> kTradingDays{5,10,21,42,63,84,105,126,189,252,378,504};
  };
  // advance n NYSE trading days (skip weekends + cal holidays) from now_ns; n>=0.
  [[nodiscard]] std::int64_t advance_trading_days(std::int64_t now_ns, int n,
                                                  const VolTimeCalendar& cal) noexcept;
  // year-fraction of a tenor: advance n trading days, then apply the time convention.
  [[nodiscard]] double tenor_years(std::int64_t now_ns, int n_trading_days,
                                   const TimeSpec& spec) noexcept; // uses VolTimeCalendar::us_default()
  }
  ```

- [ ] **Step 1: Write the failing test**
```cpp
#include "atx/vol/sr_tenor_grid.hpp"
#include "atx/vol/vol_time.hpp"
TEST(SrTenorGrid_TradingDays_MatchTickerHistoryColumns) {
  EXPECT_EQ(SrTenorGrid::kTradingDays.front(), 5);
  EXPECT_EQ(SrTenorGrid::kTradingDays.back(), 504);
  EXPECT_EQ(SrTenorGrid::kTradingDays.size(), 12u);
}
TEST(SrTenorGrid_AdvanceTradingDays_SkipsWeekend) {
  // Fri 2026-02-13 16:00 UTC + 1 trading day -> Tue 2026-02-17 (Mon 02-16 Presidents Day holiday).
  const std::int64_t fri = 1770998400000000000LL; // 2026-02-13T16:00:00Z (verify exact ns in impl)
  const auto& cal = VolTimeCalendar::us_default();
  const std::int64_t nxt = advance_trading_days(fri, 1, cal);
  // 2026-02-16 is a NYSE holiday; next trading day is 2026-02-17.
  EXPECT_GT(nxt, fri);
}
TEST(SrTenorGrid_TenorYears_252TdApproxOneYear) {
  const std::int64_t now = 1770998400000000000LL; // some instant
  const double y = tenor_years(now, 252, TimeSpec{}); // Calendar365 default
  EXPECT_GT(y, 0.95);
  EXPECT_LT(y, 1.05);
}
```

- [ ] **Step 2: Run to verify fail** — `powershell scripts\atx-build.ps1 build atx-vol-tests` (symbols missing).
- [ ] **Step 3: Write header + impl.** `advance_trading_days`: from `now_ns`, step one civil day at a
  time (via `kCalendarYearNs`-free day math — reuse `days_from_civil`/`civil_from_days` in vol_time),
  skipping Saturdays/Sundays and `cal.is_holiday(day)`, decrementing `n` on each trading day, until
  `n` reaches 0; keep the intraday time-of-day of `now_ns`. `tenor_years`: `time_to_expiry_years(now,
  advance_trading_days(now, n, VolTimeCalendar::us_default()), spec)`. Bound the loop (`n` trading
  days ⇒ ≤ `2*n + 20` civil steps; assert the bound). Add `src/sr_tenor_grid.cpp` to the lib.
- [ ] **Step 4: Run to verify pass** — `powershell scripts\atx-build.ps1 -Ctest -R SrTenorGrid`.
- [ ] **Step 5: Commit**
```bash
git add atx-vol/include/atx/vol/sr_tenor_grid.hpp atx-vol/src/sr_tenor_grid.cpp \
        atx-vol/tests/earnings_term_fit_test.cpp atx-vol/CMakeLists.txt
git commit -m "feat(vol): SpiderRock trading-day tenor grid + calendar-aware tenor->T converter"
```

---

## Task 3: Term-curve model fit (fixed decay ⇒ linear st/lt), sampled on the grid

**Files:**
- Modify: `atx-vol/include/atx/vol/earnings_term_fit.hpp` (declare `fit_term_curve_for_emove`)
- Modify: `atx-vol/src/earnings_term_fit.cpp`
- Test: extend `atx-vol/tests/earnings_term_fit_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace atx::vol {
  struct TermCurve { double st, lt, decay, rms_resid; };
  // For a FIXED eMove, censor all obs then fit atmCen(T)=lt+(st-lt)exp(-decay*T).
  // decay is searched (1D, bounded); {st,lt} solved by linear LSQ at each decay.
  [[nodiscard]] TermCurve fit_term_curve_for_emove(std::span<const CensorObsInput> obs,
                                                   double emove, const EarningsFitConfig& cfg) noexcept;
  // atmCen(T) from a fitted curve; st is the 5d/T->0 anchor, lt the long-T anchor.
  [[nodiscard]] double term_curve_value(const TermCurve& c, double T) noexcept; // lt+(st-lt)exp(-decay*T)
  }
  ```

- [ ] **Step 1: Write the failing test** (recover a planted smooth curve exactly, no events)
```cpp
TEST(EarningsTermFit_FitTermCurve_RecoversPlantedCurve) {
  const double st=0.45, lt=0.25, decay=3.0, e=0.0;
  std::vector<CensorObsInput> obs;
  for (double T : {0.02,0.05,0.10,0.25,0.5,1.0,2.0}) {
    const double sc = lt + (st-lt)*std::exp(-decay*T);
    obs.push_back({T, sc*sc*T, 0});                 // pure diffusive, n=0
  }
  EarningsFitConfig cfg{};
  const TermCurve c = fit_term_curve_for_emove(obs, e, cfg);
  EXPECT_NEAR(c.st, st, 1e-4);
  EXPECT_NEAR(c.lt, lt, 1e-4);
  EXPECT_NEAR(c.decay, decay, 1e-2);
  EXPECT_LT(c.rms_resid, 1e-6);
  EXPECT_NEAR(term_curve_value(c, 0.25), lt+(st-lt)*std::exp(-decay*0.25), 1e-4);
}
```

- [ ] **Step 2: Run to verify fail.**
- [ ] **Step 3: Implement.** In `.cpp`:
  - `term_curve_value`: `return c.lt + (c.st - c.lt) * std::exp(-c.decay * T);`
  - `fit_term_curve_for_emove`: build `y_i = censored_atm_vol(obs_i, emove, cfg.wcen_floor)`.
    For a grid of `decay ∈ [decay_lo, decay_hi]` (e.g. 0.1..30, log-spaced ~40 pts) plus a
    golden-section refine: at each `decay`, basis `b_i = exp(-decay*T_i)`; solve the 2×2 normal
    equations for `(lt, A)` in `y = lt + A*b` (least squares), then `st = lt + A`. Track the min
    weighted RMS residual. Return the best `{st,lt,decay,rms_resid}`. Bound `decay>0`.
    No dynamic alloc in the inner loop beyond the obs span (pre-reserve).

- [ ] **Step 4: Run to verify pass** — `... -Ctest -R EarningsTermFit_FitTermCurve`.
- [ ] **Step 5: Commit**
```bash
git add atx-vol/include/atx/vol/earnings_term_fit.hpp atx-vol/src/earnings_term_fit.cpp atx-vol/tests/earnings_term_fit_test.cpp
git commit -m "feat(vol): censored term-curve fit lt+(st-lt)exp(-decay*T) via decay search + linear st/lt"
```

---

## Task 4: Global joint {eMove, st, lt, decay} fit + grid sampling (the core)

**Files:**
- Modify: `atx-vol/include/atx/vol/earnings_term_fit.hpp` (declare `fit_earnings_term`)
- Modify: `atx-vol/src/earnings_term_fit.cpp`
- Test: extend `atx-vol/tests/earnings_term_fit_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  [[nodiscard]] Result<EarningsTermFit> fit_earnings_term(
      std::span<const CensorObsInput> obs, const EarningsFitConfig& cfg);
  ```
  Behavior: outer 1D minimization of `fit_term_curve_for_emove(...).rms_resid` over
  `emove ∈ [cfg.emove_lo, cfg.emove_hi]` (golden-section, `cfg.max_iters` cap); at the optimum,
  populate `{emove, st, lt, decay, fit_error, expiry_count}` and sample
  `atm_cen[i] = term_curve_value(curve, cfg.tenor_T[i])` for the 12 grid tenors when `cfg.tenor_T`
  is provided (empty ⇒ `atm_cen` empty). **NOTE (data-recon):** this `atm_cen` is the PARAMETRIC-
  model read = a SECONDARY summary. The PRIMARY `atmCenI_Nd` reproduction target is the RAW
  censored-space interpolation produced in Tasks 7/8 (censor listed-expiry ATM variance with this
  `emove`, interpolate censored total variance to each tenor's T, `atmCenI=sqrt(w_cen(T)/T)`).
  `fit_code`: `LeftBound`/`RightBound` if the optimum sits on a bound, `MaxSteps` if the cap hit,
  `CenterFlat` ONLY when the objective is GENUINELY numerically flat near the optimum (measured with
  a LOCAL scale — the optimum's neighborhood — NOT the whole-bracket worst point, which the emove=0
  endpoint dominates). all-`n`-equal-nonzero is WEAKLY IDENTIFIED (a constant event lump is separable
  from the T-scaled curve) ⇒ classifies as `Minimum`, not CenterFlat. Else `Minimum`.
  `Err(InvalidArgument)` if `obs.size() < 2`, any non-finite/non-positive `T`/`w_dirty`. all `n==0`
  (no event to identify) ⇒ Ok with emove=0 and `CenterFlat` (special-cased).
  FLOOR-EXCLUSION (outer objective): rank a candidate `+inf` ONLY in the degenerate case where fewer
  than 2 event-bearing observations remain NON-floored (the identification minimum) — NOT when a
  single observation floors, which would bias emove downward on real front-expiry event censoring.

- [ ] **Step 1: Write the failing test** (recover planted eMove + curve from event-bearing obs)
```cpp
TEST(EarningsTermFit_FitEarningsTerm_RecoversEmoveAndCurve) {
  const double st=0.55, lt=0.28, decay=4.0, e=0.07;
  // n(T): 1 event before the 2 nearest expiries, 2 before the far ones.
  struct P{double T; std::size_t n;};
  std::vector<P> pts{{0.03,1},{0.06,1},{0.12,1},{0.25,2},{0.5,2},{1.0,2},{2.0,3}};
  std::vector<CensorObsInput> obs;
  for (auto p : pts) {
    const double sc = lt + (st-lt)*std::exp(-decay*p.T);
    const double w  = sc*sc*p.T + double(p.n)*e*e;      // dirty = censored + event
    obs.push_back({p.T, w, p.n});
  }
  // 12 tenor year-fractions (calendar-approx here; the tool uses tenor_years from Task 2).
  std::array<double,12> tt{};
  const std::array<int,12> td{5,10,21,42,63,84,105,126,189,252,378,504};
  for (std::size_t i=0;i<12;++i) tt[i] = td[i]/252.0;   // trading-day basis
  EarningsFitConfig cfg{}; cfg.tenor_T = tt;
  auto r = fit_earnings_term(obs, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->emove, e, 2e-3);
  EXPECT_NEAR(r->st, st, 3e-3);
  EXPECT_NEAR(r->lt, lt, 3e-3);
  EXPECT_EQ(r->atm_cen.size(), 12u);
  EXPECT_NEAR(r->atm_cen[0], lt+(st-lt)*std::exp(-decay*tt[0]), 3e-3); // 5d parametric read
}

TEST(EarningsTermFit_FitEarningsTerm_AllZeroEvents_EmoveZero) {
  std::vector<CensorObsInput> obs{{0.05,0.3*0.3*0.05,0},{0.5,0.28*0.28*0.5,0}};
  auto r = fit_earnings_term(obs, EarningsFitConfig{});
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->emove, 0.0, 1e-9);
}
```

- [ ] **Step 2: Run to verify fail.**
- [ ] **Step 3: Implement** the outer golden-section over emove wrapping Task 3; boundary/flatness
  → `fit_code`; sample grid; validate inputs at entry returning `Result`.
- [ ] **Step 4: Run to verify pass** — `... -Ctest -R EarningsTermFit_FitEarningsTerm`.
- [ ] **Step 5: Commit**
```bash
git add atx-vol/include/atx/vol/earnings_term_fit.hpp atx-vol/src/earnings_term_fit.cpp atx-vol/tests/earnings_term_fit_test.cpp
git commit -m "feat(vol): global joint {eMove,st,lt,decay} earnings censoring term-fit + grid sampling"
```

---

## Task 5: Earnings-forecast loader → sorted epoch-ns events

**Files:**
- Create: `atx-vol/include/atx/vol/earnings_forecast_loader.hpp`
- Create: `atx-vol/src/earnings_forecast_loader.cpp`
- Test: `atx-vol/tests/earnings_forecast_loader_test.cpp` (+ append to tests/CMakeLists.txt)
- Test fixture: `atx-vol/tests/support/earnings_forecast_sample.tsv` (3-4 hand-authored rows)

**Interfaces:**
- Produces:
  ```cpp
  namespace atx::vol {
  // Parse a tblstockearnforecasthist TSV; return sorted UTC epoch-ns of nextEarnDate1..8 for
  // `ticker`. Uses the UTC datetime columns (e.g. "2026-02-25 22:00:00.000000"). Ok even if the
  // ticker has fewer than 8 forward dates; Err(NotFound) if ticker absent, Err(InvalidArgument)
  // on malformed header/rows.
  [[nodiscard]] Result<std::vector<std::int64_t>> load_earnings_events(
      std::string_view forecast_tsv_path, std::string_view ticker);
  }
  ```
  Consumers (Task 7) build `EventSchedule(std::move(events))` and `count_events_at`.

- [ ] **Step 1: Write the failing test** — parse the sample TSV, assert the ticker's events are
  sorted ascending, count matches, and the first instant equals the known UTC ns. Include a
  boundary test: a ticker with only 2 forward dates; a missing ticker → `Err`.
- [ ] **Step 2: Run to verify fail.**
- [ ] **Step 3: Implement** a tab-split line parser: read header, index the `ticker_tk` and
  `nextEarnDate1..8` UTC columns; for the matching row parse `"YYYY-MM-DD HH:MM:SS[.ffffff]"` as
  UTC → epoch-ns (use the repo's civil-date helpers `days_from_civil` used in vol_time.cpp; do
  not pull in a new date lib); drop empty/sentinel cells; sort ascending. Validate at boundary.
- [ ] **Step 4: Run to verify pass.**
- [ ] **Step 5: Commit**
```bash
git add atx-vol/include/atx/vol/earnings_forecast_loader.hpp atx-vol/src/earnings_forecast_loader.cpp \
        atx-vol/tests/earnings_forecast_loader_test.cpp atx-vol/tests/support/earnings_forecast_sample.tsv \
        atx-vol/CMakeLists.txt atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): tblstockearnforecasthist loader -> sorted epoch-ns earnings events"
```

---

## Task 6 (Seam S1): Stamp real `expiry_ns` on fitted eSSVI slices

**Files:**
- Modify: `atx-vol/src/session.cpp` (the `run_surface_parity`/eSSVI fit loop that emits slices)
- Modify (read first): `atx-vol/include/atx/vol/vol_surface.hpp` (`EssviParams::expiry_ns`)
- Test: `atx-vol/tests/session_test.cpp` (add a case asserting fitted slices carry `expiry_ns`)

**Interfaces:**
- Consumes: existing `EssviParams::expiry_ns` (field already present, currently left 0).
- Produces: fitted eSSVI slices whose `expiry_ns == chain.expiry_ns`, so `count_events_at`/
  `solve_implied_emove` read the real instant instead of the Calendar365-inverse synthesis.

- [ ] **Step 1: Write the failing test** — build a `VolaSession` from a small synthetic frame
  (reuse a `session_test.cpp` fixture) with ≥2 expiries; assert `surface.essvi_slices()[k].expiry_ns`
  equals the installed `chain.expiry_ns` for each k (currently 0 → fails).
- [ ] **Step 2: Run to verify fail** — `... -Ctest -R Session*ExpiryNs`.
- [ ] **Step 3: Implement** — in the eSSVI surface fit loop, set `params.expiry_ns = chain.expiry_ns`
  (and `expiry_id`) when constructing each slice. Read the surrounding code to place it where the
  slice is finalized. Then update `solve_implied_emove`/`count_events_at` call sites to prefer the
  stamped `expiry_ns` when non-zero (fall back to the synthesis only when 0), removing the
  Calendar365-lock for VolTime frames.
- [ ] **Step 4: Run to verify pass** — the new case + the full `session`/`event_vol`/`projection`
  suites (`... -Ctest -L atx_vol_fast`) stay green.
- [ ] **Step 5: Commit**
```bash
git add atx-vol/src/session.cpp atx-vol/tests/session_test.cpp
git commit -m "fix(vol): stamp real expiry_ns on fitted eSSVI slices (exact event bucketing, unblock VolTime)"
```

---

## Task 7: End-to-end `earnings-repro` tool (OPRA → fit → term-fit → atmCenI/iEMove)

**Files:**
- Create: `atx-vol/examples/earnings_repro.cpp` (target `earnings-repro`)
- Modify: `atx-vol/CMakeLists.txt` (register the example target, guarded by `ATX_BUILD_EXAMPLES`)

**Interfaces:**
- Consumes: `load_opra_cbbo_parquet` (opra_panel.hpp), `VolaSession::from_frame` +
  `SessionInputs{events=...}` (session.hpp), `atmf_vol`/`total_variance` at forward-ATM
  (analytics_primitives.hpp), `load_earnings_events` (Task 5) → `EventSchedule`, `count_events_at`
  (event_vol.hpp), `fit_earnings_term` (Task 4), `SrTenorGrid` (Task 2).
- Produces: a CLI that, given `--opra <parquet> --earnings <tsv> --ticker <SYM> --now <iso>`
  (+ optional `--convention calendar|voltime`), prints `iEMove`, `st,lt,decay`, and the 12
  `atmCenI_{Nd}` values; `--truth-row <csv>` optionally prints per-tenor residual vs a
  tickerhistory row.

- [ ] **Step 1** — Read `examples/cstar_panel.cpp` `--real` path for the exact
  `load_opra_cbbo_parquet → data_install → VolaSession` wiring; mirror it.
- [ ] **Step 2 (write a smoke test first via the tool):** add `atx-vol/tests/earnings_repro_smoke_test.cpp`
  (label `atx_vol_slow`) that runs the pipeline on the on-disk `NVDA/2026-02-10.parquet` +
  a checked-in NVDA earnings TSV subset and asserts: 12 finite `atm_cen` values in (0,3),
  monotone-ish censored curve, `iEMove` finite ≥ 0. Run → fail (tool absent).
- [ ] **Step 3** — Implement the tool: build per-expiry `CensorObsInput{T, w_dirty=total_variance
  at forward-ATM, n=count_events_at(schedule, now, T)}` from the fitted session, call
  `fit_earnings_term`, print. Forward-ATM total variance = `ps.total_variance(ps.forward_at(T),T)`.
- [ ] **Step 4** — `powershell scripts\atx-build.ps1 build earnings-repro` then run on NVDA;
  the slow smoke test passes.
- [ ] **Step 5: Commit**
```bash
git add atx-vol/examples/earnings_repro.cpp atx-vol/tests/earnings_repro_smoke_test.cpp atx-vol/CMakeLists.txt atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): earnings-repro end-to-end tool (OPRA -> fit -> atmCenI/iEMove) + slow smoke test"
```

---

## Task 8 (Seam S2): Unify the censoring path for the term-structure output

**Files:**
- Modify (read first): `atx-vol/src/analytics_aggregate.cpp` (`compute_surface_analytics` session
  overload :252, `earnings_implied_move` :39), `atx-vol/src/analytics_primitives.cpp`
  (`atmf_vol_ex_earnings` :117), `atx-vol/src/projection.cpp` (`event_aware_w`).
- Test: `atx-vol/tests/analytics_aggregate_test.cpp` (add an event-straddling-tenor case)

**Interfaces:**
- Produces: `atm_vol_ex_earn` on a tenor that sits between two pillars straddling an earnings date
  (n_hi>n_lo) equals the censored-*space* interpolation (censor both brackets, interpolate
  censored variance in T, re-add `n_query·eMove²`) — matching `event_aware_w`, NOT the plain-space
  censoring of the interpolated total variance.

- [ ] **Step 1: Write the failing test** — construct a 2-pillar priced surface with `n_lo=0`,
  `n_hi=1`, a known `eMove`, and an `EventSchedule` such that a mid tenor has `n_query=1`; assert
  `atm_vol_ex_earn(mid)` equals the `event_aware_w`-derived value and differs from the current
  plain-space result by the documented `e²·[n_lo+α(n_hi−n_lo)−n_q]` term. Run → fail.
- [ ] **Step 2: Run to verify fail.**
- [ ] **Step 3: Implement** — route the aggregator's censored value through censored-space
  interpolation. Preferred: give `compute_surface_analytics` access to the `EventSchedule`
  (stop dropping it in `to_priced_surface()`, or pass it via `EventContext`) and compute
  `atm_vol_ex_earn` by bracketing pillars + `event_aware_w`. Keep the plain path behind a config
  flag for A/B during the sweep (`EarningsReproConfig::censor_space{true}`).
- [ ] **Step 4: Run to verify pass** — new case + `analytics_*` suites green under ASan/UBSan.
- [ ] **Step 5: Commit**
```bash
git add atx-vol/src/analytics_aggregate.cpp atx-vol/src/analytics_primitives.cpp atx-vol/tests/analytics_aggregate_test.cpp
git commit -m "fix(vol): term-structure atmCen uses censored-space interpolation (SpiderRock FLEX), not plain-space"
```

---

## Task 9: Convention config + validation harness over the cohort

**Files:**
- Create: `atx-vol/include/atx/vol/earnings_repro_config.hpp` (the `EarningsReproConfig` carrier)
- Create: `atx-vol/tools/earnings_validation.cpp` (target `earnings-validation`) — batch driver
- Create: `atx-vol/tests/support/tickerhistory_2026-02-10_cohort.csv` (checked-in truth subset,
  from the data-recon slice — cohort names only, the columns iEMove + atmCenI_{5d..504d} + nEarnCnt_*)
- Test: `atx-vol/tests/earnings_validation_test.cpp` (label `atx_vol_slow`)

**Interfaces:**
- Consumes: Tasks 4/5/7 outputs + the truth CSV.
- Produces: `EarningsReproConfig{ TimeConvention time; bool censor_space; enum AtmMode; enum
  DeAmPricer{Alo,Crr}; bool implied_borrow; InterpSpace{Variance,Vol}; double clock_days_per_year;
  ... }` and a batch tool that, per name in the cohort, computes atmCenI/iEMove and emits a
  per-tenor + cohort RMSE table and residual attribution.

- [ ] **Step 1: Write the failing test** — for one cohort name with a known truth row, assert the
  harness produces a `CohortResult` whose per-tenor residual vector has size 12 and whose cohort
  RMSE is finite; assert `nEarnCnt` computed from the earnings schedule EXACTLY matches the truth
  `nEarnCnt_*` (schedule-alignment gate). Run → fail.
- [ ] **Step 2: Run to verify fail.**
- [ ] **Step 3: Implement** the config struct + batch harness: iterate cohort, run the Task-7
  pipeline under a given `EarningsReproConfig`, compare to truth, accumulate RMSE + attribution.
- [ ] **Step 4: Run to verify pass.**
- [ ] **Step 5: Commit**
```bash
git add atx-vol/include/atx/vol/earnings_repro_config.hpp atx-vol/tools/earnings_validation.cpp \
        atx-vol/tests/support/tickerhistory_2026-02-10_cohort.csv atx-vol/tests/earnings_validation_test.cpp \
        atx-vol/CMakeLists.txt atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): EarningsReproConfig + cohort validation harness (RMSE + residual attribution)"
```

---

## Task 10: Convention sweep → lock the matching config (analysis, produces a report)

**Files:**
- Create: `atx-vol/docs/reviews/2026-07-18-atmcen-reproduction-convention-sweep.md`
- (Uses `earnings-validation` from Task 9; no new production code unless the sweep reveals a needed knob.)

**Interfaces:** Consumes the Task-9 harness. Produces the locked `EarningsReproConfig` defaults.

- [ ] **Step 1** — Run `earnings-validation` over the cohort under each knob setting:
  `time ∈ {Calendar365, VolTime}` × `censor_space ∈ {true,false}` × `implied_borrow ∈ {true,false}`
  × `InterpSpace ∈ {Variance,Vol}` (start one-factor-at-a-time from a sensible base).
- [ ] **Step 2** — Record cohort atmCenI RMSE + iEMove rel-err per setting; back-solve the
  `iEMult`-vs-`iEMove` question on one name (does censoring with raw `iEMove` reproduce the truth
  atmCen, or is a multiplier needed?).
- [ ] **Step 3** — Set the winning combination as `EarningsReproConfig` defaults; write the sweep
  report (table + chosen config + residual attribution + any remaining timing-limited gap).
- [ ] **Step 4** — Re-run the full cohort under the locked config; assert cohort RMSE meets the
  gate (or document the timing residual and trigger the $100 Databento EOD-slice decision).
- [ ] **Step 5: Commit**
```bash
git add atx-vol/docs/reviews/2026-07-18-atmcen-reproduction-convention-sweep.md atx-vol/include/atx/vol/earnings_repro_config.hpp
git commit -m "docs(vol): atmCen reproduction convention sweep + locked EarningsReproConfig defaults"
```

---

## Follow-on Phase M5 (separate plan after the gate is met) — SOTA upgrades
Documented for continuity; NOT part of this plan's shippable deliverable:
1. Non-spanning σ_C identification + monotone-θ hard constraint in the joint fit.
2. Per-event / next-print eMove weighting (relax equal-event); earnings-date slide (`eMoveExpAdj`).
3. Vendor-compat CRR + Vellekoop-Nieuwenhuis discrete-div de-Am mode (vs Andersen-Lake) as a knob.
4. Branch-light vectorized IV; right-size pricer per tenor; perf bench vs current hot path.
5. hEMove historical-move path (`atmCenH_*`) with max-clipping.

## Self-review (spec coverage)
- Model/censoring → Tasks 1,3,4. Global eMove objective (=st/lt/decay) → Task 4. Tenor grid (S3)
  → Task 2. Earnings adapter (S4) → Task 5. expiry_ns (S1) → Task 6. End-to-end → Task 7.
  Censoring unify (S2) → Task 8. Convention config + validation + tight-bar gate → Tasks 9,10.
  Vol-time end-to-end → enabled by Task 6, exercised in Task 10 sweep. American/European IV,
  forward/div/borrow, eSSVI fit, OPRA ingest → REUSED (no task; verified via Task 7 smoke).
  Databento reserve → decision point in Task 10. M5 SOTA → follow-on.
