# Task 2 Report: Lifecycle `CloseAtHorizon` + missing-name policy for `DeclarativeStrategy`

## Status: DONE

## What I implemented

### 1. `LifecycleSpec::Holding::CloseAtHorizon` (value `2`)
Added as a third `Holding` enumerator in `atx-vol/include/atx/vol/strategy.hpp`, per the
brief's exact spec (doc comment included verbatim). `lifecycle_decide` (strategy.cpp) now
groups `HoldToExpiry` and `CloseAtHorizon` under the same open-tick rule (`EveryStep` /
`step_index % entry_every_n == 0`), never `clear`. `DeclarativeStrategy::on_step` gained a
close pass that runs **before** the lifecycle-decide/entry logic when
`holding == CloseAtHorizon`: it erases every lot from `book.lots` whose
`(lot.expiry_ts_ns - base.ts_ns()) < roll_at_T * kNsPerYear` (computed as an `int64`
subtraction then cast to `double`, exactly as specified — not two independent-cast
subtractions, which would lose precision on raw epoch-ns timestamps). The engine's
before/after `book.lots` diff (`execute` in `backtest.cpp`) books these as roll-closes at
current marks; the engine's own expiry-settlement pass runs earlier in its loop (before
`on_step`), so a lot closed here can never also settle.

### 2. Missing-name policy for `DeclarativeStrategy`
- `StrategySpec` gained `MissingNameSpec missing{}` (default `{Error, min_names=2}`).
- New `ResolveDrop{symbol, detail}` + `resolve_spec_with_policy(snap, spec, dropped=nullptr)`
  declared next to `resolve_spec` in strategy.hpp.
- Refactored `resolve_spec`'s body into a shared anonymous-namespace `resolve_spec_impl`
  (strategy.cpp) taking an explicit `MissingNameSpec` parameter (not read from `spec.missing`
  directly, so no `StrategySpec` copy is needed). `resolve_spec` now calls
  `resolve_spec_impl(snap, spec, MissingNameSpec{Error, 2}, nullptr)` — it **ignores**
  `spec.missing` entirely, so it stays bit-identical regardless of what a caller happens to
  set there. `resolve_spec_with_policy` calls the same impl with `spec.missing`.
- Per-leg expand+size logic was further factored into `expand_and_size_leg` (unchanged
  logic/messages, just extracted so both entry points share one body).
- `resolve_spec_impl` semantics under `DropRenormalize`:
  - A leg whose `expand_and_size_leg` fails is dropped (`*dropped` gets
    `{ls.symbol, error.message()}`) **unless** `constraint.kind != None && ls.group ==
    group_b` (the scaled hedge group) — that case returns `Err(Unavailable, ...)` immediately
    (fatal — a missing hedge leg makes the whole entry unbuildable).
  - After the leg loop, if `drop_policy && group_a_survivors < missing.min_names` (where
    `group_a_survivors` counts successful legs with `group == group_a`, or **all** successful
    legs when `constraint.kind == None`), returns `Err(Unavailable, ...)`.
  - The existing cross-leg constraint block (FlatVega/VegaNeutralBasket scaling) is untouched
    — it already only sees `sized`, which now only contains survivors, so the scale ratio
    renormalizes automatically.
  - Under `Error` policy, the leg loop's `min_names`/hedge-protection branches are dead code
    (first failure returns immediately with the *original* error, same code+message as the
    pre-refactor inline form).

### 3. `DeclarativeStrategy::open_cohort` + `dropped_on_last_entry()`
- `open_cohort` now calls `resolve_spec_with_policy(base, spec_, &last_dropped_)`. On error,
  if `spec_.missing.policy == DropRenormalize && error.code() == Unavailable`, it returns
  `Ok()` (no-trade: book left untouched) — mirroring `DispersionStrategy`'s documented
  no-trade contract in `dispersion_strategy.cpp`. Any other error propagates.
- New accessor `std::span<const ResolveDrop> dropped_on_last_entry() const noexcept` returns
  `last_dropped_`, a new `DeclarativeStrategy` member. It's cleared+repopulated inside
  `resolve_spec_with_policy` (which clears `*dropped` unconditionally at the top) every time
  `open_cohort` runs — i.e. every entry attempt, whether it ultimately succeeds, no-trades, or
  hard-fails.

## Tests added (`atx-vol/tests/strategy_test.cpp`)

Added helpers `Corpus`/`make_corpus(n_dates, tag)` (a ready-to-clock daily "SPY"-only
manifest, generalizing the `OverlappingClips` day-loop pattern) and
`snapshot_of(items, tag)` (single-date `write_archive` + `MarketSnapshot::load`), reusing the
file's existing `make_surface`/`write_archive`/`make_manifest`/`fresh_dir`. Adapted the
brief's pseudocode to the file's real signatures (e.g. `make_surface(uid, S, fwd, now_ts,
vol_bump)`, not the brief's placeholder arg order).

1. **`Strategy.CloseAtHorizonOverlappingCohorts`** — 10-day corpus, 6-day tenor strangle,
   `roll_at_T = 2.5/365.25`, `EveryStep`. Verifies the ramp-then-plateau `n_open_lots` series
   `{2,4,6,8,8,8,8,8,8,8}` and `pnl_settlement[i] == 0.0` for all `i` (closes are roll-closes,
   never engine settlement).
2. **`Strategy.MissingNameDropRenormalize`** — exercises `resolve_spec_with_policy` directly:
   one droppable non-hedge name (`FAKE`, dropped + recorded), the `min_names` floor
   (`Unavailable`), the hedge-leg-never-droppable guard (`Unavailable`), and the `Error`
   policy hard fail (`NotFound`, unchanged).
3. **`Strategy.CloseAtHorizonNoTradeOnMissingEntry`** — 4-day corpus with only `SPY`
   archived; the hedge leg (`MISSING_INDEX`) is absent from every date under
   `DropRenormalize`. Asserts the run completes (`Ok`) with `n_open_lots[i] == 0` throughout,
   and that `dropped_on_last_entry()` stays empty (the hedge-fatal path never reaches the drop
   bookkeeping — it's a distinct outcome from a "drop").
4. **`Strategy.DeclarativeDroppedOnLastEntryAccessor`** (added beyond the brief's three,
   to close a self-review gap — none of the brief's three given tests actually call
   `dropped_on_last_entry()` on a non-empty result) — drives `DeclarativeStrategy::on_step`
   directly (not via `run_backtest`) with a droppable non-hedge name and asserts the accessor
   reflects it after a successful entry, and is empty before the first entry.

## TDD Evidence

**RED**: `git stash push --keep-index -- atx-vol/include/atx/vol/strategy.hpp
atx-vol/src/strategy.cpp` (kept the new tests staged, reverted only the implementation to
base commit `be6a7f5`), then `.\scripts\atx-build.ps1 build atx-vol-tests`:

```
strategy_test.cpp(467,52): error: no member named 'CloseAtHorizon' in 'atx::vol::LifecycleSpec::Holding'; did you mean 'RollAtHorizon'?
strategy_test.cpp(519,8): error: no member named 'missing' in 'atx::vol::StrategySpec'
strategy_test.cpp(521,15): error: use of undeclared identifier 'ResolveDrop'
strategy_test.cpp(538,18): error: use of undeclared identifier 'resolve_spec_with_policy'
strategy_test.cpp(598,21): error: no member named 'dropped_on_last_entry' in 'atx::vol::DeclarativeStrategy'
... (15 errors total)
```
Failed for exactly the expected reason (every new symbol from the brief's interface, nothing
else). `git stash pop` restored the implementation.

**GREEN**: `.\scripts\atx-build.ps1 build atx-vol-tests` — clean build, 0 warnings (repo
builds `/WX`). `.\scripts\atx-build.ps1 -Ctest -R "Strategy|Backtest|Dispersion"`:

```
100% tests passed, 0 tests failed out of 67
```
Includes all 5 pre-existing `Strategy.*` tests (`StrikeFromDelta`, `Structures`, `FlatVega`,
`OverlappingClips`, `DispersionParity`) unmodified and green, plus the 4 new `Strategy.*`
tests, plus every `Backtest.*`/`BacktestExec.*`/`BacktestReal.*`/`Dispersion.*`/
`ListedDispersion*.*`/`SpyStrangleBacktest.*`/`SurfaceDbBacktest.*` test in the label filter.

Also ran `.\scripts\atx-build.ps1 build atx-vol` (the full library target, all consuming
TUs) — clean, 0 warnings/errors, confirming no other `atx-vol` source file broke against the
widened `Holding` enum or the new `StrategySpec::missing` field (grepped for `switch` on
`.holding` and positional `StrategySpec{...}`/`LifecycleSpec{...}` aggregate-inits elsewhere
in the tree — none found, so the additive changes are safe by construction).

## Files changed

- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\include\atx\vol\strategy.hpp`
  (+59/-2): `Holding::CloseAtHorizon`, `StrategySpec::missing`, `ResolveDrop`,
  `resolve_spec_with_policy` declaration, `DeclarativeStrategy::dropped_on_last_entry()` +
  `last_dropped_` member, `#include <span>`.
- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\src\strategy.cpp`
  (+165/-69): `lifecycle_decide` CloseAtHorizon branch; `resolve_spec`/`resolve_spec_impl`/
  `resolve_spec_with_policy`/`expand_and_size_leg` refactor; `DeclarativeStrategy::open_cohort`
  no-trade contract; `DeclarativeStrategy::on_step` close pass.
- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\tests\strategy_test.cpp`
  (+234): `Corpus`/`make_corpus`/`snapshot_of` helpers + 4 new `TEST(Strategy, ...)` cases.

## Self-review

- **Completeness against the brief's contract**: `CloseAtHorizon = 2` done (value preserved,
  existing values untouched). `StrategySpec::missing` done. `resolve_spec_with_policy`
  semantics: group_b-never-droppable done (tested, both in isolation and via the
  CloseAtHorizon no-trade path), min_names floor done (tested at the boundary — 1 passes, 2
  fails with the same survivor set), close-pass-before-entry ordering done (verified by the
  exact `n_open_lots` ramp/plateau sequence, which only works if the close erase happens
  before the day's open), no-trade contract done (tested end-to-end through `run_backtest`,
  not just the free function), `dropped_on_last_entry` accessor done (tested both
  empty-before-first-entry and the positive populated case, which the brief's own three tests
  didn't cover — I added a fourth test for this).
- **Bit-identical existing behavior**: `resolve_spec` forces `MissingNameSpec{Error, 2}`
  through the shared impl regardless of `spec.missing`, so a spec that happens to set
  `.missing` and gets passed to the *old* `resolve_spec` entry point still hard-fails exactly
  as before — the field is genuinely inert unless the caller opts into
  `resolve_spec_with_policy`. All 5 pre-existing `Strategy.*` tests pass unmodified.
- **YAGNI**: Did not restrict droppable-error-codes to `NotFound`/`Unavailable` the way
  `dispersion.cpp`'s `DroppedName` machinery does (which needs the narrower set to populate a
  `DropReason` enum) — the brief's contract for `ResolveDrop`/`resolve_spec_with_policy` has
  no reason-code field and literally says "a leg whose expansion or sizing fails is DROPPED",
  so I implemented that literally rather than importing an unrequested distinction.
- **Numerical detail worth flagging**: the close-pass residual test computes
  `lot.expiry_ts_ns - base_ts` as an `int64` difference *before* casting to `double`
  (matching the brief's literal formula), which is more precise than `lifecycle_decide`'s
  pre-existing `RollAtHorizon` branch (which casts each raw epoch-ns timestamp to `double`
  independently before subtracting — losing sub-microsecond precision on raw ~1.7e18 values,
  though immaterial at day-scale comparisons). I did not "fix" the pre-existing
  `RollAtHorizon` arithmetic since the brief scopes changes to `CloseAtHorizon` and existing
  behavior must stay byte-identical.

## Concerns

None blocking. One minor note for whoever builds Task 3's spec-builder: `resolve_spec_impl`'s
`group_a_survivors` count is at **LegSpec granularity** (one count per authored leg/name), not
per expanded option — a dropped Strangle leg costs one survivor slot, not two, which matches
the `MissingNameDropRenormalize` test's `min_names` semantics (basket *names*, not option
count) but is worth knowing when composing specs with mixed `Single`/`Strangle` legs in the
same constraint group.
