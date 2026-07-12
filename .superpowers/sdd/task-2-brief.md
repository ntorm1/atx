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

