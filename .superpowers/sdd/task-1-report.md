# Task 1 Report: `Clock::from_surface_db` — SurfaceDb-backed backtest clock

## What I implemented

- `atx-vol/include/atx/vol/backtest.hpp`:
  - Forward-declared `class SurfaceDb;` in the `atx::vol` namespace (next to the existing `class IStrategy;` forward declaration), so the header need not include `surface_db.hpp`.
  - Added `[[nodiscard]] static Result<Clock> from_surface_db(const SurfaceDb &db);` to `class Clock`, next to `from_manifest`, with a doc comment matching the brief's spec verbatim (ordering, ref.date/archive_path semantics, InvalidArgument-on-empty, MarketSnapshot/SnapshotCache compatibility).
- `atx-vol/src/backtest.cpp`:
  - Added `#include "atx/vol/surface_db.hpp"` (the one place the header's type is fleshed out) and `#include <filesystem>` (needed to build `<root>/partitions/<KEY>.atxvsa`, not previously included in this TU).
  - Implemented `Clock::from_surface_db` immediately after `Clock::from_manifest`: reads `db.partitions()`, rejects empty with `Err(ErrorCode::InvalidArgument, ...)`, sorts by ascending `key`, and builds one `SnapshotRef{p.key, path}` per partition using `db.root()` / `kSurfaceDbPartitionDir` / `kSurfaceDbPartitionExt`. Uses the same private-ctor/member-access mechanics as `from_manifest` (default-construct `Clock`, push into `refs_`, `Ok(std::move(clock))`) — no new public mutators added.
  - One deliberate micro-adjustment from the brief's literal implementation snippet: `p.key + kSurfaceDbPartitionExt` does not compile as written (`std::string + std::string_view` has no `operator+` overload — the `string_view`→`string` conversion is explicit), so I wrote `p.key + std::string(kSurfaceDbPartitionExt)`. Behaviorally identical output path.
- `atx-vol/tests/surface_db_backtest_test.cpp` (new): the 3 tests from the brief, copied essentially verbatim, with:
  - A `make_surface(double S, std::int64_t now_ts, double vol_bump, std::uint32_t uid)` helper matching the exact call-site signature used by the brief's test bodies (`make_surface(500.0, day_ts[d], 0.0, /*uid=*/1)` etc.). This is the `strategy_test.cpp` / `spy_strangle_backtest_test.cpp` `make_surface` pattern (7 eSSVI slices, `T∈{0.05,...,1.00}`, `PricingContext{S, r=0.043, now_ts, AndersenLake, al_fast_opts(), uid}`) reshaped to the brief's 4-arg call convention — flat forward `F = S` (no separate `fwd` parameter, since none of the three tests need spot/forward divergence).
  - `test_root` copied verbatim from `surface_db_test.cpp:150-153`.
- `atx-vol/tests/CMakeLists.txt`: added `surface_db_backtest_test.cpp` to the `add_executable(atx-vol-tests ...)` source list, immediately after `spy_strangle_backtest_test.cpp` and before the closing paren.

## TDD Evidence

### RED

Command:
```
& .\scripts\atx-build.ps1 build atx-vol-tests
```

Failing output (test file written, CMakeLists.txt updated, but `backtest.hpp`/`backtest.cpp` NOT yet touched):
```
C:\atx\...\atx-vol\tests\surface_db_backtest_test.cpp(108,23): error: no member named 'from_surface_db' in 'atx::vol::Clock'
  108 |   auto clock = Clock::from_surface_db(*db);
C:\atx\...\atx-vol\tests\surface_db_backtest_test.cpp(128,23): error: no member named 'from_surface_db' in 'atx::vol::Clock'
  128 |   auto clock = Clock::from_surface_db(*db);
C:\atx\...\atx-vol\tests\surface_db_backtest_test.cpp(149,23): error: no member named 'from_surface_db' in 'atx::vol::Clock'
  149 |   auto clock = Clock::from_surface_db(*db);
3 errors generated.
```

Why expected: this is exactly the missing-symbol compile failure the brief predicts (Step 2) — the ONLY errors are the three `Clock::from_surface_db` call sites, confirming the rest of the test file (the `make_surface` fixture, `SurfaceArchiveItem` aggregate-init, `SurfaceDb::create`/`write_partition`, `StrategySpec`/`LegSpec`/`DeclarativeStrategy` usage) already compiles cleanly against the current headers.

### GREEN

Build:
```
& .\scripts\atx-build.ps1 build atx-vol-tests
```
→ clean build, no compiler errors or warnings from the changed files (warnings-as-errors `/WX` in effect).

Test run:
```
& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDbBacktest|SurfaceDb|SurfaceArchive|Backtest"
```
```
100% tests passed, 0 tests failed out of 58

Label Time Summary:
atx_vol    =  14.22 sec*proc (58 tests)
Total Test time (real) =  14.89 sec
```
Includes, among the 58:
```
56/58 Test #919: SurfaceDbBacktest.ClockFromDb_OrderedRefsAndPathsLoad ..... Passed  0.16 sec
57/58 Test #920: SurfaceDbBacktest.ClockFromDb_EmptyDbRejected ............. Passed  0.13 sec
58/58 Test #921: SurfaceDbBacktest.DbDrivesRunBacktestEndToEnd ............. Passed  0.28 sec
```

Note: the brief's stated baseline ("99/99 targeted tests green") doesn't match the 58 tests this exact `-R` regex selects (55 pre-existing + 3 new). I ran the command exactly as specified in the brief; the regex-matched set is internally consistent (100% pass, no regressions vs. the pre-existing 55). The "99" figure in the operator's baseline note likely refers to a different scope/point in time and isn't something I could reconcile without guessing, so I did not chase it further — it doesn't indicate any missing coverage or regression in what I touched.

## Files changed

- `atx-vol/include/atx/vol/backtest.hpp` (+10/-0)
- `atx-vol/src/backtest.cpp` (+19/-0)
- `atx-vol/tests/CMakeLists.txt` (+1/-0)
- `atx-vol/tests/surface_db_backtest_test.cpp` (new, 185 lines)

Commit: `5b53d1c feat(atx-vol): Clock::from_surface_db - SurfaceDb-backed backtest clock`

## Self-review

- **Completeness**: every brief requirement covered — forward declaration, exact factory signature, sort-by-key ordering, `InvalidArgument` on empty db, path construction matching `<root>/partitions/<KEY>.atxvsa`, all 3 tests from the brief present and passing, CMakeLists.txt registration in the specified location.
- **Quality**: names/comments mirror existing `from_manifest` style; doc comment on the header declaration follows the brief's own wording (kept consistent with how `from_manifest`'s neighboring comments read). Error message prefixed with `Clock::from_surface_db:` for consistency with `from_manifest`'s `Clock::from_manifest:` prefix (the brief's inline snippet omitted the prefix — I added it to match the sibling function's convention; error code/behavior unaffected).
- **Discipline (YAGNI)**: no new public mutators, no extra helper methods, no scope creep beyond the one factory function and its test file. `surface_db.hpp` is included only in the `.cpp`, never the `.hpp`, per the brief.
- **Testing**: all 3 tests exercise real behavior — out-of-order partition writes sorted correctly, cross-symbol resolvability (including case-insensitive `"aapl"` lookup) through the loaded `MarketSnapshot`, empty-db rejection with the correct `ErrorCode`, and a full `run_backtest` acceptance run through `DeclarativeStrategy` producing 6 non-degenerate rows with 2 open lots each. Output is pristine (no stray printf noise); temp dirs are cleaned up at the end of each test via `std::filesystem::remove_all`.
- **No regressions**: all 55 pre-existing tests in the targeted regex (`SurfaceArchive.*`, `SurfaceDb*.*`, `Backtest.*`, `BacktestExec.*`, `BacktestReal.*`, `SpyStrangleBacktest.*`, `ListedDispersionReconciliation.ExactModelPnlClosesToCanonicalBacktest`) still pass.
- **Scope**: staged and committed only the 4 files this task touches (explicit paths, not `-A`); verified via `git status` before commit that an unrelated pre-existing unstaged modification to `.superpowers/sdd/task-1-brief.md` (made by some other process, not me) was left untouched and out of the commit.

## Concerns

- Minor: the brief's literal implementation snippet (`p.key + kSurfaceDbPartitionExt`) does not compile as-is against `std::string`/`std::string_view` overload resolution; I used `p.key + std::string(kSurfaceDbPartitionExt)` instead, which produces an identical path string. Flagging in case downstream tasks copy that exact snippet from the brief rather than from the committed source.
- The "99/99" baseline figure in the task instructions doesn't match what the specified `-Ctest -R` regex actually selects (58 total, both before my 3 additions at 55 and after at 58) — noted above, not something I could resolve without more context, and it does not indicate any regression (100% pass rate either way).
