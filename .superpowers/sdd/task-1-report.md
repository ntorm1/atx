# Task 1 Report: Volatility-time clock (`vol_time`)

## Summary

Implemented the SpiderRock-style hybrid volatility-time clock as a pure,
additive module. No existing file's runtime behavior changed.

## Files changed

- `atx-vol/include/atx/vol/vol_time.hpp` (new) — `VolTimeParams`,
  `VolTimeCalendar`, `trading_hours_between`, `vol_time_years`.
- `atx-vol/src/vol_time.cpp` (new) — implementation: anonymous-namespace
  Hinnant `days_from_civil`/`civil_from_days`, US 2007+ DST rule (resolved at
  calendar-day granularity), ET session-window -> UTC conversion, the NYSE
  2024-2028 holiday table, and the two public pure functions.
- `atx-vol/tests/vol_time_test.cpp` (new) — 15 tests (gtest).
- `atx-vol/CMakeLists.txt` — registered `src/vol_time.cpp` in the library
  source list.
- `atx-vol/tests/CMakeLists.txt` — registered `vol_time_test.cpp` in the test
  source list.

## Design notes

- Followed the brief's implementation note literally: the Hinnant civil-date
  math and DST rule are implemented locally in `vol_time.cpp`'s anonymous
  namespace, independent of `atx-core::time` (which already has an equivalent
  `Calendar`/`Date` implementation) — this keeps the new module self-contained
  per the brief, at the cost of ~30 lines of duplicated (but well-understood)
  Hinnant arithmetic. The *test file* does reuse
  `atx::core::time::timestamp_from_utc` for its `ns_utc(...)` UTC-fixture
  helper, since that's just test scaffolding, not part of vol_time's
  production logic, and reusing already-validated code there avoids a third
  hand-rolled copy of the same algorithm.
- `trading_hours_between` loops over ET calendar-day indices padded by one day
  on either side of `[start_ns, end_ns)` (covers the <=5h ET/UTC offset
  unambiguously; out-of-range days contribute exactly 0 once intersected), with
  a `kMaxLoopDays` (~20y) defensive bound per JPL Rule 2 / bounded-loop
  convention used elsewhere in atx-vol (e.g. `kIvMaxIter`).
- `vol_time_years`/`trading_hours_between` both return `0.0` for
  `end <= start`, per the brief.

## TDD evidence

**RED** — registered `vol_time_test.cpp` in `tests/CMakeLists.txt` only (the
header/impl already existed from drafting but the library source list did not
yet include `vol_time.cpp`), then built:

```
> powershell -File scripts/atx-build.ps1 build atx-vol-tests
...
FAILED: bin/atx-vol-tests.exe ...
lld-link: error: undefined symbol: public: static class atx::vol::VolTimeCalendar const & __cdecl atx::vol::VolTimeCalendar::us_default(void)
>>> referenced by \atx-vol\tests\vol_time_test.cpp:44
...
lld-link: error: undefined symbol: double __cdecl atx::vol::vol_time_years(...)
lld-link: error: undefined symbol: double __cdecl atx::vol::trading_hours_between(...)
lld-link: error: undefined symbol: public: bool __cdecl atx::vol::VolTimeCalendar::is_holiday(int) const
lld-link: error: undefined symbol: public: __cdecl atx::vol::VolTimeCalendar::VolTimeCalendar(class std::vector<int,...>)
ninja: build stopped: subcommand failed.
```

**GREEN** — registered `src/vol_time.cpp` in `atx-vol/CMakeLists.txt`, rebuilt,
ran the focused suite:

```
> powershell -File scripts/atx-build.ps1 -Ctest -R VolTime
...
100% tests passed, 0 tests failed out of 12
```

(Grew to 15 tests after the self-review pass added 3 more cases — see below;
final focused run is 15/15 passed.)

## Full-gate result

`ctest --test-dir build -L atx_vol -j16 --timeout 900` (via the VsDevCmd
wrapper), run twice (before and after the self-review fix):

- Before fix: **99% tests passed, 3 tests failed out of 1001** (12 new VolTime
  tests included, all passed).
- After fix + 3 additional tests: **99% tests passed, 3 tests failed out of
  1004** (15 new VolTime tests included, all passed).

Both runs' 3 failures are exactly the pre-existing quarantined set, unchanged:

```
MultinamePipeline.HeldLotWithoutSurfaceIsCountedNotHidden (Failed)
MultinamePipeline.DefaultPolicyFullBasketBitIdentical (Failed)
MultinamePipeline.DefaultPolicyStillBitIdentical (Failed)
```

No new regressions.

## Self-review findings

1. **Precision bug (fixed)**: `et_local_to_utc_ns`'s first draft computed
   `z_et * kNsPerDay` in `double` (product ~1.7e18 ns for present-day dates),
   which exceeds double's exact-integer range (2^53 ~ 9e15) — a genuine
   precision loss, though at a magnitude (~100s of ns) far below the 1e-12
   test tolerances actually exercised, so it did not manifest as a test
   failure. Fixed by keeping `et_local_to_utc_ns` and the
   session-open/session-close intersection entirely in `int64_t`; only the
   fractional-hour-of-day term touches a `double`, rounded via `llround`. This
   also better matches the brief's explicit "pure integer math" instruction
   for the civil-date conversion layer. Rebuilt and re-ran both the focused
   suite and the full gate after the fix — unchanged pass/fail counts.

2. **DST transition weeks**: added `DstSpringForwardWeekSessionsAreExact` and
   `DstFallBackWeekSessionsAreExact`, each checking the Friday-before /
   Monday-after a real 2026 DST transition both resolve to exactly one full
   7.5h session. Confirmed correct: since both transition instants fall on the
   intervening Sunday (a non-trading day), resolving the DST offset at
   calendar-day granularity is exact for every session boundary the module
   computes — verified this holds for both the spring-forward (2026-03-08) and
   fall-back (2026-11-01) transitions.

3. **Half-open interval semantics**: `trading_hours_between` documents
   `[start_ns, end_ns)`; the per-day session intersection uses strict `hi >
   lo`, so a zero-width overlap (e.g. `end_ns` landing exactly on a session
   boundary) contributes 0, consistent with continuous-measure semantics
   regardless of open/closed convention (a single instant has zero measure).
   No change needed.

4. **Expiry before/at now**: `vol_time_years` already returned 0 per the
   brief; added `VolTimeYearsIsZeroWhenExpiryNotAfterNow` to lock in both the
   `expiry == now` and `expiry < now` cases explicitly (previously only
   `trading_hours_between`'s equivalent guard was directly tested).

No other gaps found: the NYSE holiday table was double-checked field-by-field
against the brief's exact table (including the two irregular cases: 2027
Independence Day observed Monday 07-05, and 2028's unobserved New Year's Day,
which is deliberately absent from the table and covered by
`CalendarUnobservedNewYear2028IsNotAHoliday`).

## Concerns

None outstanding. The module is additive only — `projection.hpp`'s
`TimeMode`/`TimeModel` were not touched (that wiring is explicitly deferred to
Task 4 per the sprint plan).

## Commits

- `a284fb3` feat(atx-vol): SpiderRock-style hybrid volatility-time clock (vol_time)
- `80dc2aa` fix(atx-vol): vol_time session-boundary math in pure int64, add DST-week + degenerate-interval tests
