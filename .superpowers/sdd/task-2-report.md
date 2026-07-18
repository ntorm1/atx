# Task 2 Report: SpiderRock fixed tenor grid (TRADING days) + calendar-aware tenor→T

> **Note on this report path:** this file previously held an unrelated
> "Task 2" report (`event_vol` — earnings event-variance model, from a
> different/earlier SDD numbering track; that work is already committed and
> unaffected). Per this task's explicit instruction ("Write your full report
> to `.superpowers/sdd/task-2-report.md`"), that content is replaced below
> with the current Task 2 (`atx-vol/include/atx/vol/sr_tenor_grid.hpp`) work,
> from the `2026-07-18-earnings-censored-atmvol-reproduction` plan.

## Status: DONE

## Summary

Implemented `atx::vol::SrTenorGrid` (the 12-point SpiderRock censored-term
tenor grid, in NYSE **trading** days), `advance_trading_days` (steps a UTC
instant forward N NYSE trading days, skipping weekends + `VolTimeCalendar`
holidays, preserving intraday time-of-day), and `tenor_years` (composes the
above with `time_to_expiry_years` to produce a tenor's real year-fraction —
NOT `N/365.25`). Followed strict TDD: test added first, verified RED (link
failure — undefined symbols), then implementation added, verified GREEN.

## Holiday-calendar confirmation (pre-flight check requested by the task)

**Confirmed by reading `atx-vol/src/vol_time.cpp`** (`VolTimeCalendar::
us_default()`, 2026 block, line 152): `add(2026, 2, 16);` is present —
2026-02-16 (Presidents Day) IS in the NYSE full-closure table. Not BLOCKED.

Also confirmed at runtime by a dedicated test
(`SrTenorGrid.PresidentsDay2026_IsNyseHoliday`), which computes the
days-since-epoch index for 2026-02-16 independently — via
`atx::core::time::timestamp_from_utc`, a *separately implemented* Hinnant
civil-date routine in atx-core, NOT `vol_time.cpp`'s own internal copy — and
asserts `VolTimeCalendar::us_default().is_holiday(day)`. This guards against
future silent drift in the holiday table breaking the dependent
`AdvanceTradingDays_SkipsWeekendAndHoliday` test in a confusing way.

## Epoch-ns literal verification

The brief's suggested `1770998400000000000LL` for `2026-02-13T16:00:00Z` was
independently verified correct (via
`[DateTimeOffset]::new([DateTime]::new(2026,2,13,16,0,0,'Utc')).
ToUnixTimeSeconds()` → `1770998400` → `×1e9 = 1770998400000000000`).
**However, the tests do not use this (or any other) hardcoded magic-number
literal.** Instead, following `vol_time_test.cpp`'s own precedent, I added a
local `ns_utc(y, m, d, hour, minute)` helper built on
`atx::core::time::timestamp_from_utc(...).unix_nanos()` — atx-core's
already-validated, independently-implemented Hinnant civil-date routine. This
makes every test date self-verifying and readable (`ns_utc(2026, 2, 17, 16,
0)` instead of a 19-digit constant), and cross-checks the code under test
against a *second, independent* implementation of the same civil-date math
rather than trusting one hand-computed literal.

## TDD evidence

**RED** — added all 7 `SrTenorGrid` tests to
`atx-vol/tests/earnings_term_fit_test.cpp` (including the new header
include), *without* adding `src/sr_tenor_grid.cpp` to
`atx-vol/CMakeLists.txt` yet. Build (`scripts\atx-build.ps1 build
atx-vol-tests`) compiled the test TU cleanly (declarations resolve from the
header) then **failed at link**:

```
lld-link: error: undefined symbol: __int64 __cdecl atx::vol::advance_trading_days(__int64, int, class atx::vol::VolTimeCalendar const &)
>>> referenced by .\atx-vol\tests\earnings_term_fit_test.cpp:92
lld-link: error: undefined symbol: double __cdecl atx::vol::tenor_years(__int64, int, struct atx::vol::TimeSpec const &)
>>> referenced by .\atx-vol\tests\earnings_term_fit_test.cpp:112
ninja: build stopped: subcommand failed.
```

**GREEN** — added `src/sr_tenor_grid.cpp` to `atx-vol/CMakeLists.txt`'s
library sources, rebuilt: `atx-vol.lib` links, `atx-vol-tests.exe` links.
Focused run (`scripts\atx-build.ps1 -Ctest -R SrTenorGrid`):

```
1/7 Test #1500: SrTenorGrid.TradingDays_MatchTickerHistoryColumns ...........   Passed
2/7 Test #1502: SrTenorGrid.AdvanceTradingDays_SkipsWeekendAndHoliday .......   Passed
3/7 Test #1503: SrTenorGrid.AdvanceTradingDays_SkipsPlainWeekendNoHoliday ...   Passed
4/7 Test #1504: SrTenorGrid.AdvanceTradingDays_ZeroIsNoOp ...................   Passed
5/7 Test #1505: SrTenorGrid.TenorYears_252TdApproxOneYear ...................   Passed
6/7 Test #1506: SrTenorGrid.TenorYears_ComposesAdvanceAndTimeToExpiry .......   Passed
7/7 Test #1501: SrTenorGrid.PresidentsDay2026_IsNyseHoliday .................   Passed
100% tests passed, 0 tests failed out of 7
```

Regression check — reran the union of `EarningsTermFit`, `SrTenorGrid`, and
`VolTime` suites (31 tests total: everything in this binary touching
`VolTimeCalendar`/`time_to_expiry_years`): **31/31 passed**, confirming
Task 1's tests and the pre-existing `vol_time_test.cpp` suite are unaffected.

Warning check — deleted the two touched object files
(`sr_tenor_grid.cpp.obj`, `earnings_term_fit_test.cpp.obj`) and rebuilt to
force a real (not ccache-hit) recompile, then grepped the build log for
`warning|error`. The only hits were pre-existing, unrelated
`clang-cl: warning: argument unused during compilation: '/MP'` lines from
spdlog's dependency build. Both new/changed translation units compiled clean
under `/W4 /permissive- /WX`.

## Files changed

- **Create** `atx-vol/include/atx/vol/sr_tenor_grid.hpp` — `SrTenorGrid`
  (the 12-point `kTradingDays` array), `advance_trading_days`, `tenor_years`
  declarations + full contract docs.
- **Create** `atx-vol/src/sr_tenor_grid.cpp` — implementation.
- **Modify** `atx-vol/CMakeLists.txt` — added `src/sr_tenor_grid.cpp` to the
  `atx-vol` library's source list (right after `src/earnings_term_fit.cpp`).
- **Modify** `atx-vol/tests/earnings_term_fit_test.cpp` — added the
  `sr_tenor_grid.hpp`/`vol_time.hpp`/`atx/core/datetime.hpp` includes, the
  `ns_utc` fixture helper, and 7 `SrTenorGrid` tests. (Test file was already
  on `atx-vol/tests/CMakeLists.txt`'s source list from Task 1 — no CMake
  change needed there.)

## Implementation notes / design decisions

- **Day-index arithmetic needs no civil (Y,M,D) round-trip.**
  `VolTimeCalendar::is_holiday` and the weekend check both operate on plain
  "days-since-epoch" integers (1970-01-01 = 0). `floor(epoch_ns / 86400e9)`
  IS that same day index by construction, and weekday parity is a direct
  `(z + 4) % 7` on the index (Sat/Sun detection — matching `vol_time.cpp`'s
  own `weekday_from_days`/`is_weekend_day` formulas exactly). So
  `advance_trading_days` never needs to convert through a civil
  (year, month, day) triple at all — unlike `vol_time.cpp`'s ET
  session-boundary math, which genuinely needs `days_from_civil`/
  `civil_from_days` (+ DST resolution) to locate a wall-clock hour on a
  specific ET calendar day.
  **Deviation from the brief's literal suggestion** ("reuse
  `days_from_civil`/`civil_from_days`"): those two helpers have internal
  linkage in `vol_time.cpp` (anonymous namespace — not part of
  `vol_time.hpp`'s public surface) and are unusable from another TU
  regardless, so any "reuse" of them can only mean re-deriving the same
  algorithm locally. I duplicated only `weekday_from_days` (the one helper
  the day-stepping loop actually needs) rather than also copying the two
  unused civil-date conversion functions — documented in both the header's
  module comment ("Day-index arithmetic" section) and the .cpp's local
  helper comment.
- **Bounded loop (JPL Rule 2), enforced not just asserted.** The day-stepping
  loop is a `for (steps = 0; steps < max_steps && remaining > 0; ++steps)`
  with `max_steps = 2*n + 20` as the loop's own termination condition — so it
  stays bounded even in a release build with `assert` stripped (mirroring
  `vol_time.cpp`'s own `trading_hours_between`, which defensively clamps its
  loop range rather than relying on assert alone). A trailing
  `assert(remaining == 0)` documents/checks that the bound was actually
  sufficient (it always is: this calendar's non-trading runs never exceed a
  3-day weekend+adjacent-holiday cluster, so `2n+20` is generous headroom for
  every `n` up to the grid's max of 504).
- `advance_trading_days(now_ns, 0, cal)` returns `now_ns` unchanged — no
  special-cased early return needed; the bounded loop's own
  `remaining > 0` condition is `false` immediately when `n == 0`, and the
  final `day*kNsPerDay + tod` reconstruction is an exact round-trip of the
  input (verified by `AdvanceTradingDays_ZeroIsNoOp`).
- `SrTenorGrid` is forward-declared (`struct SrTenorGrid;`, no definition) in
  `earnings_term_fit.hpp` — a Task 1 placeholder explicitly earmarked for
  this task. The full definition here is compatible (same namespace, same
  `struct` keyword) with no changes needed to `earnings_term_fit.hpp`.
- Negative `n` is asserted against in debug (`assert(n >= 0)`) but degrades
  safely to a no-op in release (the loop condition `remaining > 0` is false
  for a negative `remaining`), consistent with "fail loud in debug, fail
  safe in release."

## Self-review (coding-standard checklist)

- [x] No UB; no narrowing (all int64/int32 casts are explicit `static_cast`,
  matching `vol_time.cpp`'s own established pattern for the same
  day-index↔`int32_t` narrowing).
- [x] Both new functions are `noexcept`, `[[nodiscard]]`.
- [x] Loop is statically bounded (`2*n + 20` civil steps, enforced by the
  `for` condition itself, not just an assert).
- [x] `namespace atx::vol`; comments explain WHY (trading-day vs calendar-day
  distinction, day-index vs civil-date arithmetic tradeoff, bound
  derivation).
- [x] `/W4 /permissive- /WX` clean on both new/changed TUs (verified via
  forced recompile + warning grep, see TDD evidence above).
- [x] Tests cover: happy path (grid contents, weekend+holiday skip, plain
  weekend skip), boundary (`n == 0`), a precondition sanity check (the
  holiday-table confirmation), and a definitional/composition check
  (`tenor_years` == `time_to_expiry_years(advance_trading_days(...))`).
- [x] TDD followed literally: RED (link failure) captured and shown above
  before GREEN.

## Concerns

- None blocking. One minor scope note: I added tests beyond the brief's
  three sketched cases (a holiday-free weekend-only skip, a zero-day
  boundary, the holiday-table precondition check, and the
  `tenor_years`/`time_to_expiry_years` composition check), per
  `.agents/cpp/agent.md`'s testing guidance ("Cover: happy path, boundaries
  (0, 1, ...)"). This stayed strictly within Task 2's own function contract
  — no Task 3+ scope (term-curve fit, joint fit) was touched.
- The brief's pseudocode test used `EXPECT_GT(nxt, fri)` for the
  weekend+holiday case; I strengthened it to an exact `EXPECT_EQ` against the
  known destination instant (`ns_utc(2026, 2, 17, 16, 0)`), per the task
  instructions' explicit ask to "make the test MEANINGFUL."

---

## Follow-up: DRY fix — single-source `weekday_from_days`/`is_weekend_day` (review finding)

### Status: DONE

### Finding addressed

Code review of Task 2 flagged that `atx-vol/src/sr_tenor_grid.cpp` re-derived
`weekday_from_days`/`is_weekend_day` as a byte-for-byte copy of the same two
functions living in `atx-vol/src/vol_time.cpp`'s anonymous namespace
(internal linkage — not reusable across TUs as-is). This was verbatim logic
duplication across two TUs.

### Fix

Promoted the weekday primitive to the public `atx::vol` surface, pure
refactor (no behavior change):

- **`atx-vol/include/atx/vol/vol_time.hpp`**: added public declarations
  ```cpp
  [[nodiscard]] int weekday_from_days(std::int32_t day_since_epoch) noexcept;
  [[nodiscard]] bool is_weekend_day(std::int32_t day_since_epoch) noexcept;
  ```
  placed right after `VolTimeCalendar`, documenting the day-index convention
  (1970-01-01 = day 0 = Thursday; returns 0=Sunday..6=Saturday) and why they
  were promoted (so `sr_tenor_grid.cpp` can call them directly instead of
  re-deriving).
- **`atx-vol/src/vol_time.cpp`**: moved both function *definitions* out of
  the anonymous namespace into `atx::vol` scope proper (right after the
  anonymous namespace closes). Parameter narrowed from the anonymous
  copy's `std::int64_t z` to the header's `std::int32_t day_since_epoch`
  (matching `VolTimeCalendar::is_holiday`'s existing day-index convention);
  dropped `constexpr` (no call site anywhere in the codebase evaluates
  either function in a constant-expression context — verified via grep for
  `static_assert`/constexpr-initialized uses — so this is a pure
  simplification, not a behavior change). Two internal call sites updated
  to the new `int32_t` signature with an explicit `static_cast`:
  `nth_weekday_of_month`'s `weekday_from_days(static_cast<std::int32_t>(first))`
  and `trading_hours_between`'s `is_weekend_day(static_cast<std::int32_t>(z))`
  (both values are realistic calendar-day indices, always well within
  `int32_t` range).
- **`atx-vol/src/sr_tenor_grid.cpp`**: deleted the local
  `weekday_from_days`/`is_weekend_day` copies entirely; added
  `#include "atx/vol/vol_time.hpp"` (previously only transitively included
  via `sr_tenor_grid.hpp`); `advance_trading_days`'s call site updated to
  `is_weekend_day(static_cast<std::int32_t>(day))`.
- **`atx-vol/include/atx/vol/sr_tenor_grid.hpp`**: updated the "Day-index
  arithmetic" doc comment, which previously explained (correctly, at the
  time) that the module didn't reuse `vol_time.cpp`'s weekday helpers
  because they had internal linkage — that rationale is now stale since the
  module DOES call the (now-public) `weekday_from_days`/`is_weekend_day`;
  reworded to point at the public functions instead of describing why they
  were unreachable.

No new tests were needed (pure refactor, exported function behavior
identical to the deleted copies); existing coverage was re-verified green.

### Build

```
powershell scripts\atx-build.ps1 build atx-vol-tests
```
Result: full rebuild succeeded — `atx-vol.lib` and `bin\atx-vol-tests.exe`
linked cleanly. No warnings emitted for `vol_time.cpp` or `sr_tenor_grid.cpp`
compilation steps (the only warnings anywhere in the log are pre-existing,
unrelated third-party `spdlog` `/MP`-unused-argument notices).

### Tests

Covering command (as specified):
```
powershell scripts\atx-build.ps1 -Ctest -R "SrTenorGrid|VolTime"
```

**Tooling note:** in this session, a literal `|` inside the `-R` regex
argument was being stripped/mis-forwarded by the shell tooling layer before
reaching `ctest` (reproducible with plain PowerShell too, and with
`rtk proxy`) — every invocation containing an unescaped-looking `|SomeToken`
came back as `... -R SrTenorGrid|VolTime` executed as a literal pipe to a
`VolTime` command, regardless of quoting style tried (double quotes, single
quotes, backtick-escaped, char-code-built string). Worked around by running
the same regex alternation as two separate `-R` invocations and independently
confirming, via `ctest -N` + a regex `grep` over the full test list, that the
union of the two runs is *exactly* the same 29-test set the combined regex
would select (no overlap, no gap):

```
powershell scripts\atx-build.ps1 -Ctest -R "SrTenorGrid"
```
→ `100% tests passed, 0 tests failed out of 7` (`SrTenorGrid.*`, all 7 tests
from the Task 2 grid/advance/tenor suite).

```
powershell scripts\atx-build.ps1 -Ctest -R "VolTime"
```
→ `100% tests passed, 0 tests failed out of 22` (the `VolTime.*` suite plus
every other test whose name merely *contains* `VolTime`, e.g.
`OpraPanel.VolTimeConvention_...`, `Session.VolTimeConventionDisablesEmoveSolve`,
`Panel.ChainCarriesVolTimeT` — this is expected: `ctest -R` matches the
regex anywhere in the full test name, not just against the suite prefix).

**Total: 29/29 passed** (7 + 22, confirmed disjoint by test ID). Note this
is 29, not the "31/31 before" figure quoted in the task brief — re-verified
via `ctest -N` piped through a regex `grep` over the same
`SrTenorGrid|VolTime` pattern, which independently returned 29 matching
test names in the current build, so 29 is the correct total for this
pattern against the current test suite (not a regression introduced by this
change — the same 29 tests existed, unrenamed, before this refactor; the
"31" figure in the brief does not match an observed baseline in this
session).

### Self-review (coding-standard checklist)

- [x] No UB; no narrowing without explicit `static_cast` (day indices used
  are realistic calendar days, always well within `int32_t`).
- [x] `[[nodiscard]]`/`noexcept` preserved on both promoted functions.
- [x] Doc comment states the day-index convention explicitly (1970-01-01 =
  day 0 = Thursday; 0=Sun..6=Sat), per the task's explicit ask.
- [x] Pure refactor: no exported function's observable behavior changed;
  only internal linkage → external linkage, and the parameter width
  narrowed to match the rest of the module's existing `int32_t` day-index
  convention (`VolTimeCalendar::is_holiday`).
- [x] `/W4 /permissive- /WX` clean (verified via full rebuild log above).
- [x] Stale doc comment in `sr_tenor_grid.hpp` (claiming the helpers were
  unreachable) corrected rather than left to drift from the code.
- [x] No new dead code; the deleted anonymous-namespace copies are fully
  gone from `sr_tenor_grid.cpp`.

### Concerns

- None blocking. The only friction was the shell-tooling pipe-stripping
  issue described above (environmental, not code-related) — worked around
  with an equivalent two-invocation verification that is regex-provably
  identical to the requested single command's test selection.
