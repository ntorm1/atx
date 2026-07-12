# atx-vol SpiderRock-Guided Analytics Sprint — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the highest-value gaps between atx-vol and a SpiderRock-class production vol platform: a volatility-time clock, an earnings event-variance model, a vol-multiple cubic-spline curve family, shape-blend (FLEX-style) time interpolation, skew-adjusted hedge deltas, SpiderRock-style band-violation fit stats, and the outstanding calendar-floor correctness item (R3).

**Architecture:** Every task is an additive module or a guarded extension of an existing seam (`IVolCurve`/`fit_slice_curve` for new curves, `TimeModel`/`projection` for time, `fit_metrics` for stats). Defaults are bit-identical to current behavior; all new behavior is opt-in. Worktree: `C:/atx-wt/atx-vol-spiderrock`, branch `feat/atx-vol-spiderrock`, base = local `main` (01d88b8).

**Tech stack:** C++20, `atx::core::Result`/`Status` (no exceptions), GTest, CMake preset `dev` (build dir `build/`), clang-cl + Ninja. House rules: no fast-math, no virtual on arithmetic hot path, `[[nodiscard]]`, `noexcept` on pure evaluators, aggregate value types.

## Global Constraints

- Build: `cmake --preset dev` then `cmake --build build -j16`. Test gate: `ctest --test-dir build -L atx_vol -j16 --output-on-failure --timeout 900` (expect ~869 pass; SPY-parquet tests GTEST_SKIP when `data/` fixture absent).
- **Never** `git add -A` — stage explicit paths only.
- Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- New headers go in `atx-vol/include/atx/vol/`, sources in `atx-vol/src/`, tests in `atx-vol/tests/` and must be registered in `atx-vol/CMakeLists.txt` (library sources) and `atx-vol/tests/CMakeLists.txt` (test list — follow the existing per-test pattern).
- Default-off / bit-identical: existing tests must pass unchanged (except where a task explicitly rebaselines a named test).
- Follow existing header documentation style: file-comment explaining what/why/thread-safety, `@param/@return` on entry points.
- All formulas below sourced from SpiderRock Connect 8.6.6.3 Analytics docs (VolTimeCalc, LiveVolSurfaces, FLEXVolInterpolation, ClientVolatilitySurfaces, OptionPricing, InterestRateTerm) — digest in §"SpiderRock reference" at the bottom.

---

## Task order & parallelism

Sequential execution (each task = dispatch → review → commit):
T1 (vol time) → T2 (event variance) → T3 (spline curve) → T4 (shape-blend interp, touches projection after T1) → T5 (adjusted delta) → T6 (calendar floor R3) → T7 (band stats + session guard roll-up).
T1/T2/T3/T5/T7 are file-disjoint; T4 must follow T1 (both edit `projection.*`); T6 is independent.

---

### Task 1: Volatility-time clock (`vol_time`)

**Files:**
- Create: `atx-vol/include/atx/vol/vol_time.hpp`
- Create: `atx-vol/src/vol_time.cpp`
- Test: `atx-vol/tests/vol_time_test.cpp`
- Modify: `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt` (register source + test)

**Why:** atx-vol computes T as calendar year-fraction (365.25d) everywhere; `projection.hpp`'s `TimeModel` reserves overnight/weekend weights but only `TimeMode::Clock` is implemented. SpiderRock prices everything in hybrid volatility time.

**Model (SpiderRock VolTimeCalc, verbatim):**
- Annual trading hours = 1890 (252 trading days × 7.5h — the RTH session *plus the hour after close*); annual non-trading hours = 6870 (8760 − 1890).
- α = fraction of variance attributed to trading time, default 0.7.
- `T_vol = TradingHoursRemaining × α/1890 + NonTradingHoursRemaining × (1−α)/6870`.
- Sanity identity: α weighting makes one full RTH session at α-weight = 7.5·α/1890 years; at α=1 a full trading day is exactly 1/252.

**Interfaces (Produces):**
```cpp
namespace atx::vol {

struct VolTimeParams {
  double alpha{0.7};                    // variance fraction in trading hours, in [0,1]
  double trading_hours_per_year{1890.0};
  double nontrading_hours_per_year{6870.0};
  double session_open_hour_et{9.5};     // 09:30 ET
  double session_span_hours{7.5};       // 09:30–17:00 ET (RTH + 1h post-close)
};

// Immutable named-holiday calendar (dates as days-since-epoch, UTC civil).
// us_default() carries the NYSE full-closure table 2024–2028 inclusive.
class VolTimeCalendar {
 public:
  explicit VolTimeCalendar(std::vector<std::int32_t> holiday_days);
  [[nodiscard]] bool is_holiday(std::int32_t day_since_epoch) const noexcept;
  [[nodiscard]] static const VolTimeCalendar& us_default();
 private:
  std::vector<std::int32_t> days_;  // sorted
};

// Trading hours (fractional) in [start_ns, end_ns) under the ET session window,
// skipping weekends + calendar holidays. 0 if end <= start.
[[nodiscard]] double trading_hours_between(std::int64_t start_ns, std::int64_t end_ns,
                                           const VolTimeParams& p,
                                           const VolTimeCalendar& cal) noexcept;

// SpiderRock master formula. Total wall hours = (end-start)/3600e9;
// nontrading = total - trading. Returns 0 for end <= start.
[[nodiscard]] double vol_time_years(std::int64_t now_ns, std::int64_t expiry_ns,
                                    const VolTimeParams& p,
                                    const VolTimeCalendar& cal) noexcept;
}
```

**Implementation notes:**
- Epoch-ns → ET: implement `civil_from_days` / `days_from_civil` (Howard Hinnant algorithms, pure integer math) in an anonymous namespace; US DST rule: EDT (UTC−4) from second Sunday of March 02:00 to first Sunday of November 02:00, else EST (UTC−5). Unit-test the converter through the public API (e.g. a timestamp at 13:30 ET winter = 18:30 UTC).
- `trading_hours_between`: iterate whole days between the two ET civil dates (bounded: ≤ ~1200 days for 3y options — fine; guard `end−start > 5y` by clamping day loop, still exact via per-day accumulation), for each non-weekend non-holiday day intersect `[open, open+span]` with `[start, end]`.
- No global state; both functions pure.

**Steps:**
- [ ] **T1.1** Write `vol_time_test.cpp` with the cases below; register in `tests/CMakeLists.txt`; build; confirm compile failure (header missing).
```cpp
// Anchor: Wed 2026-07-08 (regular trading day, EDT, UTC-4).
// 13:30 ET == 17:30 UTC. kDay = 86'400e9 ns.
TEST(VolTime, FullTradingDayAtAlphaOne) {
  VolTimeParams p; p.alpha = 1.0;
  // Wed 2026-07-08 00:00 ET -> Thu 2026-07-09 00:00 ET covers one full session.
  const auto t0 = ns_utc(2026, 7, 8, 4, 0);   // 00:00 EDT
  const auto t1 = t0 + kDayNs;
  EXPECT_NEAR(vol_time_years(t0, t1, p, VolTimeCalendar::us_default()),
              7.5 / 1890.0, 1e-12);           // == 1/252
}
TEST(VolTime, WeekendIsPureNonTrading) {
  VolTimeParams p;  // alpha 0.7
  const auto sat0 = ns_utc(2026, 7, 11, 4, 0);   // Sat 00:00 EDT
  const auto mon0 = sat0 + 2 * kDayNs;
  EXPECT_NEAR(vol_time_years(sat0, mon0, p, VolTimeCalendar::us_default()),
              48.0 * 0.3 / 6870.0, 1e-12);
}
TEST(VolTime, JulyFourthHolidayHasNoTradingHours) {
  // 2026-07-03 (Fri) is the NYSE observed Independence Day closure.
  VolTimeParams p; p.alpha = 1.0;
  const auto t0 = ns_utc(2026, 7, 3, 4, 0);
  EXPECT_NEAR(vol_time_years(t0, t0 + kDayNs, p, VolTimeCalendar::us_default()), 0.0, 1e-15);
}
TEST(VolTime, OneYearIsApproximatelyOne) {
  VolTimeParams p;
  const auto t0 = ns_utc(2026, 1, 2, 5, 0);
  const auto t1 = ns_utc(2027, 1, 2, 5, 0);
  EXPECT_NEAR(vol_time_years(t0, t1, p, VolTimeCalendar::us_default()), 1.0, 0.02);
}
TEST(VolTime, MonotoneNonIncreasingAsNowAdvances) { /* step now_ns by 1h over 2 weeks, assert T_vol non-increasing, continuous within 2*step budget */ }
TEST(VolTime, IntradayDecayFasterThanOvernight) {
  // 1 trading hour at alpha .7 outweighs 1 overnight hour: a/1890*.7 > .3/6870.
  VolTimeParams p;
  const auto mid_session = ns_utc(2026, 7, 8, 16, 0);  // 12:00 ET
  const auto d_trading = vol_time_years(mid_session, mid_session + 3600e9, p, cal);
  const auto overnight = ns_utc(2026, 7, 9, 2, 0);     // 22:00 ET Wed
  const auto d_night = vol_time_years(overnight, overnight + 3600e9, p, cal);
  EXPECT_GT(d_trading, d_night);
  EXPECT_NEAR(d_trading, 0.7 / 1890.0, 1e-12);
  EXPECT_NEAR(d_night, 0.3 / 6870.0, 1e-12);
}
```
- [ ] **T1.2** Implement `vol_time.hpp` + `vol_time.cpp` (civil-date math, DST rule, NYSE holidays 2024–2028). Register in `CMakeLists.txt`. Exact full-closure table (NYSE published; 2025-01-09 = National Day of Mourning):

```
2024: 01-01, 01-15, 02-19, 03-29, 05-27, 06-19, 07-04, 09-02, 11-28, 12-25
2025: 01-01, 01-09, 01-20, 02-17, 04-18, 05-26, 06-19, 07-04, 09-01, 11-27, 12-25
2026: 01-01, 01-19, 02-16, 04-03, 05-25, 06-19, 07-03, 09-07, 11-26, 12-25
2027: 01-01, 01-18, 02-15, 03-26, 05-31, 06-18, 07-05, 09-06, 11-25, 12-24
2028: 01-17, 02-21, 04-14, 05-29, 06-19, 07-04, 09-04, 11-23, 12-25
```
(2027 Independence observed Mon 07-05 since 07-04 is a Sunday; 2027 Christmas observed Fri 12-24; 2028 New Year's unobserved — falls Saturday.)
- [ ] **T1.3** Build + run `ctest --test-dir build -R VolTime`; all pass.
- [ ] **T1.4** Run full fast gate; confirm no regressions.
- [ ] **T1.5** Commit: `feat(atx-vol): SpiderRock-style hybrid volatility-time clock (vol_time)`.

**Acceptance:** all new tests pass; full gate green; no existing file's behavior changed.

---

### Task 2: Earnings event-variance model (`event_vol`)

**Files:**
- Create: `atx-vol/include/atx/vol/event_vol.hpp`, `atx-vol/src/event_vol.cpp`
- Test: `atx-vol/tests/event_vol_test.cpp`
- Modify: both CMakeLists.

**Model (SpiderRock LiveVolSurfaces / FLEXVolInterpolation, verbatim):**
- Decomposition: `w_total(T) ≡ σ_T²·T = n·eMove² + σ_C²·T` (n = earnings events before expiry, eMove = per-event instantaneous move vol, σ_C = censored diffusive vol).
- FLEX recombination: `atmVol = sqrt(atmCen² + n·eMove²/T)`.
- Censored interpolation across expiries happens in *censored variance*, linear in T; event variance re-added for the query expiry.

**Interfaces (Produces):**
```cpp
namespace atx::vol {

// Sorted schedule of event timestamps (epoch ns). Pure value.
class EventSchedule {
 public:
  explicit EventSchedule(std::vector<std::int64_t> event_ts_ns);  // sorts
  // events in (now_ns, expiry_ns]
  [[nodiscard]] std::size_t count_between(std::int64_t now_ns, std::int64_t expiry_ns) const noexcept;
  [[nodiscard]] std::span<const std::int64_t> events() const noexcept;
};

// w_censored = w_total − n·emove², floored at kWCenFloor = 1e-10. NaN in => NaN out.
[[nodiscard]] double censored_total_variance(double w_total, std::size_t n_events,
                                             double emove) noexcept;

// sqrt(atm_cen² + n·emove²/T); T <= 0 => NaN.
[[nodiscard]] double event_recombined_vol(double atm_cen, double T, std::size_t n_events,
                                          double emove) noexcept;

// Implied per-event move from two expiries with different event counts, assuming a
// common censored instantaneous variance: solves
//   (w1 − n1 e²)/T1 = (w2 − n2 e²)/T2  =>  e² = (w1·T2 − w2·T1)/(n1·T2 − n2·T1).
// InvalidArgument if T1,T2 <= 0, T1 == T2, n1·T2 == n2·T1 (no identification), or
// inputs non-finite; FailedPrecondition if the solved e² < 0 (reports e=0 case
// separately: e² in [−eps,0] clamps to 0).
[[nodiscard]] Result<double> implied_emove(double w1, double T1, std::size_t n1,
                                           double w2, double T2, std::size_t n2);

// Event-aware total variance at T_query: censor both bracketing slices, interpolate
// the censored variance linearly in T, re-add n_query·emove². Falls back to plain
// linear-in-w when emove <= 0 or (n_lo == n_hi == n_query == 0).
[[nodiscard]] double event_aware_w(double w_lo, double T_lo, std::size_t n_lo,
                                   double w_hi, double T_hi, std::size_t n_hi,
                                   double T_query, std::size_t n_query,
                                   double emove) noexcept;
}
```

**Test cases (write first):**
```cpp
TEST(EventVol, RoundTripKnownEmove) {
  // flat censored vol 20%, emove 5%: w(T,n) = 0.04*T + n*0.0025
  const double w1 = 0.04 * 0.10 + 1 * 0.0025, w2 = 0.04 * 0.25 + 2 * 0.0025;
  auto e = implied_emove(w1, 0.10, 1, w2, 0.25, 2);
  ASSERT_TRUE(e.ok());
  EXPECT_NEAR(*e, 0.05, 1e-12);
}
TEST(EventVol, NoIdentificationWhenProportional) {
  // n1/T1 == n2/T2 -> denominator 0
  EXPECT_FALSE(implied_emove(0.01, 0.1, 1, 0.02, 0.2, 2).ok());
}
TEST(EventVol, EventAwareInterpExactAtSlices) {
  // at T_query == T_lo with n_query == n_lo, returns w_lo exactly
}
TEST(EventVol, EventAwareInterpJumpAcrossEvent) {
  // censored vol flat 20%, emove 5%. Slices at T=0.1 (n=0), T=0.3 (n=1).
  // Query T=0.19 (n=0) vs T=0.21 (n=1): censored parts nearly equal, w jumps by ~emove².
  const double e2 = 0.0025;
  const double w_lo = 0.04 * 0.1, w_hi = 0.04 * 0.3 + e2;
  const double w_a = event_aware_w(w_lo, 0.1, 0, w_hi, 0.3, 1, 0.19, 0, 0.05);
  const double w_b = event_aware_w(w_lo, 0.1, 0, w_hi, 0.3, 1, 0.21, 1, 0.05);
  EXPECT_NEAR(w_a, 0.04 * 0.19, 1e-12);
  EXPECT_NEAR(w_b, 0.04 * 0.21 + e2, 1e-12);
}
TEST(EventVol, ZeroEventsIsPlainLinearW) { /* emove=0 path == linear interp */ }
TEST(EventVol, RecombinedVolMatchesSpiderRockFormula) { /* direct formula check */ }
TEST(EventVol, CensoredFlooredNonNegative) { /* n·e² > w_total floors at 1e-10 */ }
```
Plus `EventSchedule::count_between` boundary semantics: event exactly at expiry counts, exactly at now does not.

**Steps:** test-first as T1 (write tests → fail → implement → pass → full gate → commit `feat(atx-vol): earnings event-variance model (censored vol, implied eMove, event-aware interpolation)`).

**Acceptance:** new tests pass; gate green; pure additive module.

---

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
TEST(AdjustedGreeks, NegativeSkewSlopeRaisesCallAdjustedDelta) {
  // CORRECTED (original prose had the sign inverted): under sticky-delta,
  // negative skew slope (typical put skew) => vega_slope = -(dσ/dk)/S > 0
  // => call adjusted delta > raw delta. Companion case: locally positive
  // slope past the smile minimum lowers adjusted delta.
}
```

**Steps:** tests → fail → implement → pass → gate → commit `feat(atx-vol): SpiderRock-style skew-adjusted delta (VegaSlope, sticky-delta/strike control)`.

---

### Task 6: Calendar floor grid alignment — drive between-node residual to 0 (backlog R3)

**Files:**
- Modify: `atx-vol/src/curve_fit.cpp` (sequential enforcing driver), possibly `atx-vol/src/dense_slice.cpp` (floor-row grid), `atx-vol/tests/curve_noarb_test.cpp` (tighten assertion).
- Read first: `docs/superpowers/plans/2026-07-07-atx-vol-noarb-followups.md` §R3, `curve_noarb_test.cpp` comments, `dense_slice.hpp` floor plumbing.

**Problem:** A5 enforces the calendar floor at fit *nodes*; `arb_check_calendar` scans a 64-pt k-grid; 2 crossings survive between nodes (SPY put wing ~0.4y).

**Fix (per backlog):** enforce the floor on a grid aligned to (or a superset of) the check grid: add floor rows at the union of the previous slice's node grid and the current slice's node grid (floor rows are constraint ROWS — cheap per finding #1; never new variables).

**Note:** the SPY fixture lives in `data/` (gitignored, absent in this worktree). Before starting, create a junction so SPY tests run: `cmd /c mklink /J C:\atx-wt\atx-vol-spiderrock\data C:\atx\data` (verify `data/spy_ytd` exists there; if absent the test GTEST_SKIPs and this task must be validated in the main checkout later — record that).

**Steps:**
- [ ] Junction data dir; run `ctest --test-dir build -R CurveSurfaceNoArb` to reproduce residual == 2.
- [ ] Implement union-grid floor rows in the sequential driver.
- [ ] Residual → 0; tighten test to `EXPECT_TRUE(viol->empty())`.
- [ ] Measure in-band px_clean shift (backlog says strict floor already cost 4.8pp; this should be marginal). If px_clean drops below the `kPxCleanFloor = 94` gate, STOP and report — do not rebaseline without recording the tradeoff in the commit message.
- [ ] Full gate; commit `fix(atx-vol): enforce calendar floor on union node grid — between-node residual 0 (R3)`.

**Acceptance:** `CurveSurfaceNoArb.SpyDenseIsCalendarArbFree` residual 0 with tightened assertion; gate green; px_clean shift recorded.

---

### Task 7: Band-violation fit stats + session guard roll-up

**Files:**
- Modify: `atx-vol/include/atx/vol/fit_metrics.hpp`, `atx-vol/src/fit_metrics.cpp`
- Modify: `atx-vol/src/session.cpp` (line ~284 guard)
- Test: extend `atx-vol/tests/fit_metrics_test.cpp` (+ a session test only if a seam exists)

**Model (SpiderRock LiveVolSurfaces fit-quality fields):** `cBidMiss/cAskMiss/pBidMiss/pAskMiss` (surface crossing counts), `fitMaxPrcErr` (largest bid-ask violation in premium; zero for ~90% of SpiderRock fits), error-location fields.

**Interfaces (Produces):**
```cpp
// fit_metrics.hpp
struct BandViolationStats {
  std::size_t n{};            // quotes scored
  std::size_t n_bid_miss{};   // model price < bid  (surface crosses bid)
  std::size_t n_ask_miss{};   // model price > ask
  double max_prc_err{};       // max(bid−p, p−ask, 0) over quotes (premium units)
  std::size_t max_err_idx{};  // index of the worst quote (size_t(-1) when n==0)
  double avg_signed_err{};    // mean(p − mid)
};
// Spans paired element-wise; crossed/zero quotes (ask < bid) are skipped (not scored).
// InvalidArgument on length mismatch; empty spans => Ok with n==0.
[[nodiscard]] atx::core::Result<BandViolationStats>
band_violation_stats(std::span<const double> model_price,
                     std::span<const double> bid_price,
                     std::span<const double> ask_price) noexcept;
```
**Session guard:** `session.cpp:284` `n_viol = cal ? cal->size() : 0` treats a FAILED calendar check as arb-free; match the conservative sibling at `session.cpp:639` (failed check ⇒ report as not-verified, not as zero violations). Follow the sibling's exact pattern.

**Test cases:** in-band quotes → zeros; one bid cross + one ask cross counted with correct max/idx; crossed quote skipped; mismatch rejected; empty ok.

**Steps:** tests → fail → implement → pass → gate → commit `feat(atx-vol): SpiderRock-style band-violation fit stats; fix session failed-calendar-check reporting`.

---

## Deferred (recorded, not in this sprint)

- SplineVol in `default_selector_candidates()` + archive (ATXVSA additive payload) — after the family proves out OOS.
- Event-aware `VolaSession`/`CurveSurface` query integration (T2 provides the math; wiring the schedule through `SessionInputs` is a follow-up).
- sdiv EMA / strike-dependent cpAdj spline; uPrcRatio futures calibration.
- Vol-time as default T convention (needs corpus rebaseline).
- R2 Lee wings + wing-aware calendar (Sprint E scope), R4 test-perf pass.
- **R3 (this plan's Task 6) — investigated and reclassified 2026-07-11:** the 2 surviving calendar crossings sit at the flat-clamped wing boundary of the shorter slice (k≈−0.469/−0.431, T≈0.4y), not between interior nodes; 6 union/checker-grid floor-row variants all cascade into worse violations (up to 34) because the wing region needs R2's wing-aware enforcement first. Fold R3 into Sprint E with R2. Evidence: `.superpowers/sdd/task-6-report.md`.
- Per-expiry rate-curve carry integration (market_env.hpp documented enhancement).

## SpiderRock reference (digest)

- **VolTimeCalc:** T = TH×α/1890 + NTH×(1−α)/6870; α≈0.7; 1890 = 252×7.5h (RTH+1h post-close); 6870 = 8760−1890.
- **LiveVolSurfaces:** cubic spline on vol multiples σ_K/σ_ATM over fixed 29-pt moneyness grid [−25…25]; LogStd moneyness ln(K/F)/(σ_ATM√T); wings decay to flat outside [minX,maxX]; two-tier fit (slow shape ~45s, ATM recalib 2–10×/s); fit into bid-ask channel, not mids; earnings model σ_T²·T = n·eMove² + σ_C²·T; fit-quality fields incl. fitMaxPrcErr (0 for ~90% of fits); adjusted Δ = Δ + VegaSlope·Vega.
- **FLEXVolInterpolation:** wwHi=(T−T_lo)/(T_hi−T_lo); atmCen interpolated in variance; atmVol = sqrt(atmCen² + n·eMove²/T); sVol = atmVol·(wwLo·m_lo + wwHi·m_hi).
- **ClientVolatilitySurfaces:** moneyness taxonomy (Strike/SimpleMoney/RTMoney/VolRTMoney/LogStdMoney/T-variants); dynamicVol = theoVol + tvSlope·(uPrc−refUPrc); ω=refUPrcWeight sticky control; ICurve blend.
- **OptionPricing:** generalized BSM; rate/sdiv/carry triple; sdiv calibrated to minimize call/put surface mismatch; HTB: sdiv = overnight − HTB rate; binomial-splice for discrete divs (atx-vol: Andersen-Lake + escrowed-dividend forward instead); greeks conventions; expiration-day American→European switch.
- **InterestRateTerm:** OIS/SOFR/SPX-box curves; monotone cubic spline on log discount factors over fixed day grid; Act/365 interpreted in vol time. (atx-vol `YieldCurve` already matches the interpolation scheme.)
