# Task V6-C2 rebase report: C2 onto C1 (pool-2 HEAD), review I1-I3

**Implementer:** Opus 5.5, pool-11, branch `feat/mega-alpha-v6-c2-20260927`. Nothing was built or run
(per brief). The only numerics run were a numpy replica of the new test fixtures (premises below).

## Final commits (pool-11)

| SHA | Content |
|---|---|
| `12283c34` | C2 (was `720a0066`), rebased; conflicts resolved, message unchanged |
| `1da82261` | review I1, I2, I3 (second commit, as briefed) |

Base is **`3b72c36e`**, the pool-2 HEAD at hand-off, not `70af55d1`.
- I first rebased onto `70af55d1`, which was HEAD when I started.
- While I worked, pool-2 gained the C1 fix round (`144071a3`, `64e5fe98`) and the W commits.
- I rebased both commits again onto `3b72c36e`. That rebase was conflict-free: C1-fix's
  `nonmember_exit_clause` sits next to C2's `parse_neutralize` edit at the end of `detail`, and the
  two auto-merged.
- `git -C C:/atx-wt/pool-2 cherry-pick 12283c34 1da82261` onto `3b72c36e` is therefore a
  fast-forward-equivalent.

## 1. What conflicted and how it was resolved

These are the five textual conflicts from the review §4. They appeared against `70af55d1`; the move
to `3b72c36e` added none.

1. **`strategy_nav_replay.cpp` `run_nav_replay` reserve block** (now `:2054-2059`).
   - Took C2's `role_geometry(cfg)` + `nav_workspace_reserve_bytes(base, books, tiered, names,
     sessions)`.
   - C1's `base.order_basis / locate_in_aim / liquidity_cache` assignments above the hunk were kept
     (they auto-merged).
   - C1's `nav_reserve_bytes` is gone (C2 deleted it; it had no other caller).
   - The semantic fix (I2) is in commit 2.
2. **`strategy_target_replay.hpp` `ConstructionDay`** (`:103-108`): kept C2's
   `neutralize_groups / _unknown_group_names / _fallback_names`, then C1's `locate_zeroed`. No
   positional aggregate init of `ConstructionDay` exists anywhere (grep), so the field order is
   free.
3. **`strategy_target_replay_detail.hpp` `form_desired` doc** (`:66-81`): concatenated C2's
   industry sentence and C1's `no_short` paragraph. Commit 2 extends the latter for I3.
4. **`tests/strategy_nav_replay_test.cpp` tail**: C1's NavV6 block, then C2's block (starting
   `:2618`). Both were kept; the shared closing brace was restored between them. Duplicate-helper
   scan: none (`write_artifact` has two overloads, and both predate this work).
5. **`tests/strategy_target_replay_test.cpp` tail**: C1's three ExitRate tests, then C2's block
   (`:1038`). Both were kept; no collisions.

I also checked the regions that auto-merged:
- `validate_nav_input`: C2's industry check next to C1's `liquidity_cached` budget line.
- The `--neutralize` help and parse next to C1's new flags. I read the help string once and it is
  coherent.
- `form_desired`: C1 zeroing, then `if (!neutralizing)`, then C2's industry dispatch.
- The `strategy_nav_replay.hpp` declarations.

## 2. Review findings applied (commit `1da82261`)

**I2: reserve and the liquidity cache.** Fixed in `strategy_nav_replay.cpp:1715-1723`.
- `nav_workspace_reserve_bytes` now charges `rate_name_bytes` per name iff
  `liquidity_cached(base)` (`:246`: `rate == PerNameV1 || liquidity_cache`). This is the predicate
  `validate_nav_input`'s budget and the NAV's `LiquidityCache` use.
- The doc was updated at `strategy_nav_replay.hpp:396-403`.
- Test: `NavV6.WorkspaceReserveChargesTheLiquidityCacheAtAFixedRate` (`strategy_nav_replay_test.cpp:2688`)
  checks four things:
  - cached > fixed;
  - the difference is a multiple of names;
  - cached == per-name-v1;
  - per-name + cache == per-name.

**I1: base-compilable golden.** In `strategy_price_exposures_test.cpp`:
- Golden (a), `PriceRiskV1BytesArePinnedOnTheExistingFixture` (`:458`), no longer reads
  `stats.groups` or `stats.fallback_names`.
- Those checks moved to `StrategyPriceNeutralizeV6.PriceRiskV1LeavesTheWithinGroupsFieldsZero`
  (`:492`). That test is stronger: a within-groups call first fills `stats`, then `neutralize_target`
  on the same object leaves groups, unknown and fallback at 0.

To run (a) at base you still need `#include "atx/core/sha256.hpp"` in the base test file (base
lacks it). Any of `04e9d5bc`, `70af55d1` or `3b72c36e` works as the base: C1 never touched
`strategy_price_exposures.*`.

**I3: locate-in-aim with price-risk-ind-v1/-v2.** This is the ruled "re-zero after demean" fix.
- `neutralize_target_within_groups` and `neutralize_price_risk_within_groups` gain a trailing
  optional `std::span<const u8> hold_zero = {}` (`strategy_price_exposures.hpp:174,183`).
  - A span of another length gives InvalidArgument, target unmodified (`.cpp:550,564`).
  - The doc gained step 4b (`.hpp:152-154`) and the post-hold algebra (`.hpp:162-166`).
- In `fit_residual` (`strategy_price_exposures.cpp:424-428`): after `demean_within_groups` and
  before `factor`/OLS, every used row with `hold[i] != 0 && target[i] == 0` gets `e[r] = 0`.
  - `neutralize_target` and `neutralize_price_risk` (v1) pass an empty hold (`:531,538`).
- `form_desired` passes `no_short` as the hold, **on the industry dispatch only**
  (`strategy_target_replay.cpp:379-382`). Comments were updated at `:356-358`, `detail.hpp:77-80`
  and `strategy_nav_replay.hpp:154-156`.
- The target replay always passes an empty `no_short`, so nothing changes there.

Semantics to record:
- **The held set is slightly wider than the brief's wording.** It is `no_short && aim == 0` at
  entry: the shorts locate-in-aim zeroed, plus any special-tier member whose tied-rank aim was
  already exactly ±0.
  - That second case is measure-zero, and the same mechanism would push it to −(group mean).
  - Why: this avoids a new per-decision mask buffer and keeps C1's `no_short` const contract.
  - If you want exactly "the zeroed set", say so. It needs a u8 scratch mask filled in
    `form_desired`.
- **Group neutrality becomes approximate when holds exist.** After the hold the result is
  orthogonal to the intercept and the demeaned z only.
  - Before the rescale, slot G sums to h_G·m_G − n_G·c, with c = Σ_G h_G·m_G / used rows (the
    intercept).
  - A held name ends at −(c + its demeaned-z fit), not at exactly 0. This is the same class of
    effect as C1 review M1 for v1: the fit term remains.
  - The swap-fin post-block is still the safety net. The unblocked financing books can still short
    a held name by that fit term.

I3 tests:
- `StrategyPriceNeutralizeV6.HeldZeroAimsAreResetAfterTheGroupDemeaning`
  (`strategy_price_exposures_test.cpp:697`).
  - Setup: three zero-aim names of group 7 carry the group's mean exposures, so their demeaned z
    rows vanish.
  - Exact identities, to 1e-12:
    - unheld = −m·scale;
    - held = −(3m/60)·scale;
    - intercept coefficient = 3m/60;
    - group-7 sum = (3m − 30c)·scale;
    - gross = entry gross.
  - A hold byte on nonzero aims gives bit-identical output, and a short hold span is refused with
    the target unmodified.
  - Premise check: the numpy replica gives m = 0.00553 (test asserts > 1e-3); the identities hold to
    about 5e-18.
- `TargetReplayV6.LocateInAimHoldsZeroedShortsThroughTheIndustryDemeaning`
  (`strategy_target_replay_test.cpp:1176`).
  - Setup: Role(70, 12, 21), d = 60, group 4 (names 0-5) special tier, zeroing names 3, 4, 5.
  - `form_desired` under ind-v1 equals zero-then-primitive-with-hold bit for bit, the outcome is
    Applied and `locate_zeroed` is 3.
  - The held names end less short than unheld: replica −0.031 vs −0.124. Replica amplification is
    1.45, under the cap of 5.
  - Under v1, the same mask equals the v1 primitive on the zeroed target (C1's path, unchanged).

## 3. Default path: output bytes unchanged

The default path is price-risk-v1 or none, with no new flags. Traced:

- **None:** `form_desired` returns before any neutralization, and the industry code is unreachable.
- **price-risk-v1, construction:** the call chain is `neutralize_price_risk` → `price_risk(.., {}, {}, ..)`
  → `neutralize(group={}, hold={})`. From there:
  - `grouped = false`, so `reserve_group_scratch`, `assign_slots` and `demean_within_groups` are
    skipped.
  - The new hold loop `for (r = 0; r < n && !hold.empty(); ++r)` fails its condition at r = 0 and
    writes nothing.
  - The only C2 reorder, `e[r] = target[rows[r]]` before `factor(normal_matrix(z))`, is
    order-neutral: `factor` reads z only, and nothing aliases.
  - The v1 arithmetic and its order are therefore identical to base.
  - With locate-in-aim on under v1 (the C1 cells), `no_short` still goes only to C1's zeroing, never
    into v1, so those outputs are unchanged too.
- **Strings:**
  - The v1 recipe and summary literals are unchanged.
  - The industry keys (`industry`, `construction.neutralize_industry`, `fields_used.grp_ff12`) are
    emitted only for the industry ids.
  - No CSV column was added.
  - `--help` text changed, but that is not a run output.
- **Reserve (I2 + C2 C4):** the default is a fixed rate with no cache, so the cache term is 0,
  exactly as in C1 and in the original C2.
  - The reserve is now charged at actual geometry, which is ≤ the old max-geometry reserve, so
    every previously admitted run is still admitted.
  - No output publishes the reserve (the reviewer grepped for this).
  - `sizeof(ConstructionDay)` grows by 24 B. That moves only the `validate_nav_input` and
    target-replay budget arithmetic (by about 24 B × sessions × books) and never an output byte.
- **Loader:** `load_fields` loads exactly 2 fields unless an industry id is set. `role_geometry`
  plus the post-load geometry equality always hold for a valid pinned role. The only ordering
  change: a malformed role manifest now fails before the budget check.

The review's gates 2 and 3 are still the proof: the base golden, and the end-to-end SHA A/B of the
v5 REF cell against the base binary.

## 4. For the root: targets and filters

The brief says tag v6-1; the ledger (22:40) assigns NAV C2 to tag **v6-2**. Either way:

```powershell
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_price_exposures.cpp
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_target_replay.cpp
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_nav_replay.cpp
powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests
build\bin\atx-impl-strategy-target-tests.exe --gtest_filter=StrategyPriceNeutralizeV6.*:TargetReplayV6.*:NavV6.*
build\bin\atx-impl-strategy-target-tests.exe            # full exe
```

- New tests in this round: the four named in §2.
- The full exe includes the C1-fix tests. `3b72c36e` carries the C1 test-UB fix, so
  `NavV6.RecipeSummaryKeysAndCliRefusals` should pass now.
- NAV binary for the A/B gate: `atx-equity-strategy-targets` (equity-dev, `build-equity`).

## 5. Concerns

- **Uncompiled.** Nothing was compiled; the first compile is the root's.
  - Watch the new trailing default parameters (`hold_zero = {}`).
  - Watch the `neutralize(.., {}, {}, ..)` braced span arguments.
- **I3 held-set semantics.** The held set is a slight superset of "the names locate-in-aim zeroed":
  it adds special-tier members whose aim is exactly 0. Tell me if you want the exact set instead.
- **I3 trade-off.** Held names still carry −(intercept + demeaned-z fit), and group sums are no
  longer exactly 0 when holds exist. Both are documented in the header, and the prior ruling already
  keeps C3 × C5 as separate cells.
- **Review minors not addressed:** M1-M9 stay open, including M1 (fallback pool under 5 names) and
  M2 (no applied ind-v2 decision test), as before any FF49/SIC2 use.
