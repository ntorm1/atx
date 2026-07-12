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

