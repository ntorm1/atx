# Task 2 Report: Earnings event-variance model (`event_vol`)

## Status: DONE

Commit: `ec43778` — `feat(atx-vol): earnings event-variance model (censored vol, implied eMove, event-aware interpolation)`

## What was implemented

New, pure-additive module implementing the SpiderRock LiveVolSurfaces /
FLEXVolInterpolation earnings event-variance model:

- **`EventSchedule`** — immutable, sorted (not de-duplicated) set of
  earnings-announcement instants (epoch ns). `count_between(now_ns,
  expiry_ns)` counts events in `(now_ns, expiry_ns]` via two
  `std::upper_bound` scans (event exactly at `now` excluded, exactly at
  `expiry` included; returns 0 if `expiry_ns < now_ns`).
- **`censored_total_variance(w_total, n_events, emove)`** —
  `w_total − n·emove²`, floored at `kWCenFloor = 1e-10`. NaN propagates
  naturally: the flooring comparison is false for NaN, so the NaN falls
  through unmodified — no explicit `isnan` branch needed.
- **`event_recombined_vol(atm_cen, T, n_events, emove)`** — FLEX
  recombination `sqrt(atm_cen² + n·emove²/T)`; `T <= 0` or non-finite `T`
  returns NaN via `!(T > 0.0)` (catches both cases in one comparison).
- **`implied_emove(w1, T1, n1, w2, T2, n2)`** — solves
  `e² = (w1·T2 − w2·T1)/(n1·T2 − n2·T1)` from the shared-censored-variance
  assumption. Returns `Result<double>` (`atx::core::Result`, `tl::expected`).
- **`event_aware_w(...)`** — censors both bracketing slices, linearly
  interpolates the censored variance in T, re-adds `n_query·emove²`; falls
  back to plain linear-in-w when `emove <= 0` or all three `n`'s are 0.

Files:
- `atx-vol/include/atx/vol/event_vol.hpp` (new)
- `atx-vol/src/event_vol.cpp` (new)
- `atx-vol/tests/event_vol_test.cpp` (new, 23 tests)
- `atx-vol/CMakeLists.txt` (+1 line: `src/event_vol.cpp`)
- `atx-vol/tests/CMakeLists.txt` (+1 line: `event_vol_test.cpp`)

## Ambiguity resolved without blocking (not NEEDS_CONTEXT)

The brief (and the sprint plan doc it was drawn from) specifies:

> `InvalidArgument` if T1,T2 <= 0, T1 == T2, n1·T2 == n2·T1 ...; `FailedPrecondition` if the solved e² < 0

`atx::core::ErrorCode` has **no `FailedPrecondition` enumerator**
(`Unknown, InvalidArgument, OutOfRange, NotFound, AlreadyExists,
PermissionDenied, Unavailable, Internal, NotImplemented, IoError,
ParseError`). I searched for precedent rather than asking: `american_iv.cpp`
(`american_implied_vol`) draws exactly this "bad input" vs. "solved value
outside its valid domain" distinction using `InvalidArgument` for the
former and `ErrorCode::OutOfRange` for the latter (e.g. "price above
max-vol price"). None of the brief's given test cases assert the specific
error code for the negative-e² case (only `.ok()`/`.has_value()` is
checked), so this was a safe, precedent-following judgment call, documented
as a PORT NOTE in the header. `implied_emove` reports negative-e² (beyond
the clamp window) as `ErrorCode::OutOfRange`.

The brief also didn't name the `eps` in "e² in [−eps,0] clamps to 0"; I
defined `kEmoveSqClampEps = 1e-9` (documented rationale in the header:
~9 orders of magnitude below a typical e², absorbs FP cancellation noise
without masking a real inconsistency) and exercised both sides of the
window with dedicated tests (exact boundary construction, see below).

## TDD evidence

**RED** (test file + header written first, `src/event_vol.cpp` left as an
empty stub): `cmake --build build --target atx-vol-tests` failed at the
link step with undefined-symbol errors for every new symbol, e.g.:

```
lld-link: error: undefined symbol: public: __cdecl atx::vol::EventSchedule::EventSchedule(class std::vector<__int64,...>)
lld-link: error: undefined symbol: double __cdecl atx::vol::censored_total_variance(double, unsigned __int64, double)
lld-link: error: undefined symbol: class tl::expected<double, class atx::core::Error> __cdecl atx::vol::implied_emove(...)
lld-link: error: undefined symbol: double __cdecl atx::vol::event_aware_w(...)
ninja: build stopped: subcommand failed.
```

**GREEN** (after implementing `event_vol.cpp`): build succeeded; `ctest
--test-dir build -R EventVol` — 23/23 passed, e.g.:

```
Test #625: EventVol.RoundTripKnownEmove ............... Passed
Test #626: EventVol.NoIdentificationWhenProportional ... Passed
Test #630: EventVol.ImpliedEmove_NegativeESquaredBeyondEps_ReturnsOutOfRange ... Passed
Test #632: EventVol.ImpliedEmove_ESquaredWithinEpsWindow_ClampsToZero ......... Passed
Test #635: EventVol.EventAwareInterpJumpAcrossEvent .... Passed
100% tests passed, 0 tests failed out of 23
```

All 6 brief-sketched test cases were fleshed out with hand-derived
expected values in comments (verified in this report's derivation, e.g.
`RoundTripKnownEmove`: denom = 1·0.25−2·0.10 = 0.05, numer =
0.0065·0.25−0.015·0.10 = 0.000125, e² = 0.0025, e = 0.05 ✓), plus 17
additional tests covering: schedule sort/boundary/empty/reversed-interval,
NaN propagation for censored/recombined, non-positive-T errors, T1==T2,
non-finite inputs, the exact-zero and epsilon-window e² clamp cases
(constructed algebraically to land at precisely `e² = -kEmoveSqClampEps/2`),
interp-exact-at-both-slices, and the all-n-zero-with-positive-emove
fallback variant.

## Full-gate result

`ctest --test-dir build -L atx_vol -j16 --timeout 900` (via
`scripts/atx-build.ps1 -Ctest`):

```
99% tests passed, 3 tests failed out of 1027
```

The 3 failures are exactly the pre-quarantined `MultinamePipeline.*`
bit-identity tests (`HeldLotWithoutSurfaceIsCountedNotHidden`,
`DefaultPolicyFullBasketBitIdentical`, `DefaultPolicyStillBitIdentical`) —
**no new failures**. All 23 `EventVol.*` tests present and clean in the
full run (grep over the full log matched 46 `EventVol` lines — 23 `Start`
+ 23 `Passed`; no `Fail` matches).

## Self-review findings (documented in the header, not code defects)

1. **e² clamp window** — `[-kEmoveSqClampEps, 0)` clamps to `0.0`;
   more negative reports `OutOfRange`. Absolute (not relative) epsilon —
   a defensible simplicity tradeoff matching the brief and existing
   codebase constants (e.g. `kIvTol`), called out as a limitation for
   extreme-scale inputs (very large w).
2. **NaN propagation** — `censored_total_variance`/`event_recombined_vol`
   rely on IEEE-754 "any comparison with NaN is false" so domain-floor
   checks fall through to NaN rather than substituting a floor. Verified
   by dedicated tests (`CensoredNaNInNaNOut`, `RecombinedVolNonPositiveTIsNaN`).
3. **`event_aware_w` NaN-emove edge case** — the fallback guard uses
   `emove <= 0.0` (not `!(emove > 0.0)`), so a NaN `emove` does *not*
   trigger the plain-linear-w fallback when real events are present;
   it flows through the censored path and propagates to NaN, consistent
   with the module's NaN-in/NaN-out convention. (It still gets the
   fallback if `n_lo==n_hi==n_query==0`, since the event math is inert
   regardless of `emove`'s value in that case.)
4. **`n_query` vs. `n_lo`/`n_hi`** — no cross-consistency check;
   `event_aware_w` has no `Result` return (matches the brief's `noexcept`
   signature) and trusts the caller. Documented as `EventSchedule`'s
   responsibility, not this function's.
5. **`T_query` outside `[T_lo, T_hi]`** — extrapolates the linear formula
   rather than clamping or rejecting (undocumented in the brief; chose
   the least-surprising behavior consistent with a "linear interpolation"
   formula with no stated clamp).
6. **`T_lo == T_hi`** — undefined/not special-cased; produces NaN (0/0) or
   ±inf (x/0), which then propagates. Documented; callers must pass
   distinct bracketing expiries.
7. **`EventSchedule` sort vs. de-dup** — sorts but does not de-duplicate
   (unlike the Task-1 `VolTimeCalendar`, which de-dupes); a repeated
   timestamp is treated as a caller data-quality issue since
   `count_between`'s `upper_bound` scan still counts correctly either way.

No code changes resulted from self-review — all findings were pre-empted
by design choices already documented in the header's PORT NOTE / self-review
sections before the review pass, and re-verified against the passing test
suite.

## Concerns

None blocking. The only noteworthy item is the FailedPrecondition→OutOfRange
mapping and the unspecified epsilon value above — both resolved with clear
codebase precedent and documented rationale rather than guessed silently.

## Note on this report path

This file previously held an unrelated report (a "Task 2" for
`surface_db.hpp`/`SymbolFitConfig`/manifest write-parse, from a different
SDD track). Per this task's explicit instruction to overwrite
`task-2-report.md`, that stale content has been replaced above with this
`event_vol` report. Flagging in case the surface_db work still needs a
home for its own report elsewhere.

---

## Fix report — PORT NOTE citation

**Commit:** `40bcebf` — `docs(atx-vol): correct event_vol PORT NOTE error-code precedent citation`

**What changed:**
The PORT NOTE in `atx-vol/include/atx/vol/event_vol.hpp` (lines 55–68) incorrectly cited
`american_iv.cpp`'s `american_implied_vol` as the precedent for both `InvalidArgument`
(non-finite inputs) and `OutOfRange` (solved value outside valid domain). In reality:
- `american_iv.cpp:96–98` maps non-finite to `OutOfRange` (wrong for InvalidArgument precedent)
- `andersen_lake` in `american.cpp:1262–1263` maps non-finite to `InvalidArgument` (correct precedent)
- `american_iv.cpp:108–113` uses `OutOfRange` for out-of-band price checks (correct analogy for negative e²)

**Rewritten PORT NOTE:** Now correctly points non-finite→`InvalidArgument` to `andersen_lake` 
(american.cpp 1262–1263) and justifies negative-e²→`OutOfRange` by analogy to american_iv.cpp's 
out-of-band price check (108–113). Comment-only change; no behavior modification.

**Test command run:**
```
cmake --build build -j16 --target atx-vol-tests
ctest --test-dir build -R EventVol --output-on-failure
```

**Result:**
- Build: successful (1 file recompiled: `event_vol_test.cpp`)
- Tests: `100% tests passed, 0 tests failed out of 23` (all EventVol suite tests green)
