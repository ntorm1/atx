# Task V6-C1 report: order basis, exit rate, locate-in-aim, post-ramp L, fixed-rate liquidity cache

Implementer: Claude Opus 5.5, 2026-09-27. Worktree C:/atx-wt/pool-10, branch `feat/mega-alpha-v6-c1-20260927`, base
`04e9d5bc` (the root's override of the brief's e0dfb8c7).

**Status: DONE_WITH_CONCERNS.** The code is complete and committed. The C++ was **not compiled or run**, as the brief
requires: the 13 new GoogleTest cases were desk-checked by hand only. The Python side was run: `test_nav_summ.py`
passes 20/20 on synthetic fixtures, with no skips. `v6_train.sh` passes `bash -n` and seven DRY runs. No build, no real
data, no subagents. Nothing outside pool-10 was touched except this report.

| commit | content |
|---|---|
| `5b162cbe` | feat(nav): C1-C3 + F8. `strategy_nav_replay.{cpp,hpp}`, `strategy_target_replay.{cpp,hpp}`, `strategy_target_replay_detail.hpp`, and both test files (+941 / -55) |
| `5e1c7f6d` | feat(studies): `nav_summ.py` post-ramp keys (C4), `test_nav_summ.py`, and the new `v6_train.sh` (+286 / -4) |

All options are off by default. Without a new flag, no recipe key, summary key, CSV column or code path changes; the
evidence is in §2.

## 1. What changed (file:line at 5e1c7f6d)

### C1 `--order-basis target|delta` (NAV only)
- `strategy_nav_replay.hpp:126-137`: `enum class NavOrderBasis { Target, Delta }`. The contract comment there is the
  reference definition.
- `hpp:150`: `NavReplayConfig::order_basis`. `hpp:163-168`: the new `NavExecutionOptions`. `hpp:413-424`: a new
  six-argument `run_nav_replay` overload. The five-argument overload forwards `NavExecutionOptions{}` to it
  (`cpp:1988`).
- NAV state (`cpp:180-196`): `NameState` gains `anchor` (f64) and `delta` (u8), allocated only under delta, plus a
  `delta_order()` helper. They fit inside the existing 192 B/name/book reserve (79 B used), so the budget formula is
  unchanged (comment at `cpp:45`).
- **Semantics.** A working order stores the target `T = planned × NAVpost` as it does today. A delta order also stores
  `anchor` = the holding at its decision plus every dollar filled on it since.
  - Placement (`anchor_order`, `cpp:783-787`, called at `cpp:814-819`): a name whose plan changes to a nonzero weight
    gets a delta order, with `anchor = held_d`.
  - Request (`execute_orders`, `cpp:629-630`): `order - anchor`, which equals `(planned - current) × NAVpost` minus
    the dollars filled so far. The one-day drift between decision and fill therefore rides, and the next DECIDE
    re-plans from the drifted holding at θ.
  - Complete fill (`cpp:649-656`): `held = T + (held - anchor)`, i.e. the target plus the drift since the decision.
    Without drift, `held - anchor` is exactly +0, so the fill lands on `T` bit for bit.
  - Partial fill: `held += filled` and `anchor += filled`.
- **Residual carry, as the brief asked me to decide.** A capped residual keeps its remaining decision-dollar delta and
  is never adjusted for drift. It ends when it is filled, when a decision that changes the name's plan replaces it
  (every rebalance with a changed plan does), or when it is cancelled (a rebalance with an unchanged plan, a flat
  nonmember, or a write-off). On non-rebalance decisions a kept delta order stays a delta order.
- **Exceptions that stay target orders:**
  - A zero plan. Every exit, forced or dusted, must end flat. A zero-dollar delta would leave a drift residual.
  - Under `block_special_shorts`, every order on a special-tier name. Without this, a delta sale of a special long
    whose price falls before the fill can sell through zero and **open a short on a special name**. The test
    `DeltaSpecialTierOrdersStayTargetUnderTheLocateRule` shows that case.
  - A kept delta order on a name that later turns special. `clamp_kept_order` (`cpp:757-761`) first converts it to the
    target order it currently amounts to, `T + held - anchor`, and then applies the existing clamp.
- Recipe keys `order_basis: "delta"` and `order_basis_rule` (`cpp:1139`, `cpp:1236-1243`); summary top-level key
  `order_basis: "delta"` (`cpp:1969`). None of these are written under target.
- CLI (`cpp:2119`): `--order-basis target|delta`. Any other value is a usage error (exit 2).

### C2 `--exit-rate r` (target replay; used by both the `targets` and `nav` verbs)
- `strategy_target_replay.hpp:49-55`: `TargetReplayConfig::exit_rate{1.0}`.
- Validation (`cpp:106-115`): `r` must be in (0, 1]; NaN is refused. `r < 1` also requires aim-partial-v5 and
  `dust_multiple > 0`, because without a dust band the snap never fires and an exit never ends.
- Presence (`update_weights`, `cpp:280-284`): `r < 1` requires presence data, i.e. `--role`. The check sits there, not
  in `validate_input`, because `admit_saved` validates the blend before its prices load.
- Rule (`aim_partial_weights`, `cpp:201-248`): at every decision, a nonmember **present** at d moves to
  `current × (1 - r)`, and is set to 0 once `|next| <= dust_multiple / N_d` (the same band as live names; +inf when
  N_d = 0). A nonmember absent at d takes the existing `next = 0` path, so stale carry and write-off are unchanged.
  With r = 1 the new branch is never entered.
  - These moves count as forced turnover.
  - The snapped exit is a zero plan, so under delta it is a target order. The decaying steps are delta orders.
- Recipe keys `exit_rate` and `exit_rule` (`cpp:672`, `cpp:715-718`; they appear in both the target and NAV recipes);
  summary key `construction.v5.exit_rate` (`cpp:738`). The `targets` recipe's `forced_exits` becomes
  `decay-at-exit-rate;snap-inside-dust-band;absent-immediate` (`cpp:820`).
- CLI `--exit-rate` on both verbs (`target cpp:999`; `nav` `cpp:2118`).

### C3 `--locate-in-aim` (NAV)
- Borrow tiers at d are classified before the desired target is formed. `run_books` (`nav cpp:931-934`) copies the
  special tier of decision d into a shared `no_short` mask. The mask exists only when locate-in-aim is on, and it is
  covered by the existing `fixed_workspace_bytes` slack (comment at `cpp:67-69`).
- The only neutralization-adjacent edit is in the caller. `detail::form_desired` gains an optional `no_short` span
  (declaration `detail.hpp:69-79`, implementation `target cpp:333-349`): after the tied-rank target and **before**
  `neutralize_price_risk`, any member whose mask bit is set and whose desired weight is negative is set to 0 and counted
  in `ConstructionDay::locate_zeroed` (`hpp:82-84`). The regression's intercept and beta columns then re-balance net and
  beta. `strategy_price_exposures.cpp` is not touched.
- No explicit re-scaling: the neutralization rescales to the zeroed target's gross, the same order of magnitude as the
  post-block path.
- The post-rule `block_special_plan` stays in place as the safety net.
- Refusals:
  - Without borrow fields (`replay_nav_scenarios`, `cpp:1705-1708`).
  - Under `--neutralize none` (`validate_nav_config`, `cpp:303-309`). The check is `!= None`, so it also admits V6-C2's
    future `price-risk-ind-*` ids.
- The zeroed aim is part of the shared construction, so it applies to **every book**, including S2 × flat-300-v0, which
  has no locate rule.
- Recipe keys `locate_in_aim: true` and `locate_in_aim_rule` (`cpp:1152`, `cpp:1240-1242`); summary key
  `locate_in_aim.zeroed_special_short_aims` (`cpp:1970-1974`). CLI: a valueless switch (`cpp:2058-2062`).

### C4 post-ramp leverage (`studies/nav_summ.py`)
- `RAMP_ROWS = 63`. `construction_stats` adds `mean_gross_leverage_post_ramp`, `mean_net_leverage_post_ramp` and
  `post_ramp_rows`: the means over the CSV rows after the first 63, null when there are none. The output gets one new
  line per directory: `   leverage post-ramp over the N CSV rows after the first 63 [v6 C4 L calibration; gate stays
  all rows]: ...`.
- The gate is unchanged: it still reads `mean_*_all_rows`.

### F8 `--liquidity-cache` (NAV, fixed rate)
- `liquidity_cached()` (`cpp:246-249`) turns the existing shared `LiquidityCache` on when the rate is per-name-v1 **or**
  the flag is set. At a fixed rate only the execution windows are formed: `rate_decision = per_name && decision`
  (`cpp:906-908`), so decision-member windows are not computed.
- `plan_decision` now gates per-name rates on `rate == PerNameV1` (`cpp:808`) instead of `cache.on()`, so a cached
  fixed-rate book never picks up per-name rates.
- Admission charges the existing `rate_name_bytes` (24 B/name) when the cache is on (`cpp:343` and the reserve call at
  `cpp:2016`). The rate vector itself is allocated only for per-name-v1.
- **Bit-identity argument (code reading).**
  - `fill_liquidity` stores exactly `window_liquidity(c, t, i)`, the function `execute_orders` would call for the same
    (t, i).
  - The inputs are the same: `x`, `volume`, and the window and pair minimum come from the base config shared by every
    book.
  - An f64 stored in a vector reads back exactly.
  - Coverage: the cache is filled before any book's MARK at t. MARK only ever cancels orders (a write-off), so every
    order priced at t has a formed entry. In release builds, a missing entry would recompute the window with the same
    arithmetic.
  - Each book still applies its own half-spread and fallback sigma.
  - The per-name path already executes from this cache, and `NavV5.PerNameRate_FixedEqualsTradeFraction` pins that
    equivalence.
- **Why it is still opt-in.** The brief's header says "all default-off", and turning it on changes the admission
  reserve (by +24 B/name) even though no output changes.
- It writes **no recipe or summary key**. Every file is byte-identical with the cache on or off, and a key would break
  that. Its only trace is the console line `nav replay: shared execution liquidity cache on` in the runner's
  `stdout.log`.
- `v6_train.sh` defaults `LCACHE=1`.

### `studies/v6_train.sh` (new; `v5_train.sh` and `v51_train.sh` untouched)
- It is `v51_train.sh`'s `nav` phase on the **same v5.1 inputs**, read through v51's pin files: `$WT-$COMBINED.final`,
  `.combined.sha256` and `$W.weights.sha256`. The `u`, `fit` and `w` phases point to `v51_train.sh`.
- Knobs:
  - `ORDER_BASIS=target|delta`
  - `EXIT_RATE=1|<decimal>` (strict pattern)
  - `LOCATE_AIM=0|1`
  - `LCACHE=0|1` (default 1)
  - plus `THETA`, `DUST`, `RATE` and `LEV` as in v51
- Output directory: `build-equity/mega-nav-v6-<ew|aim>-t<θ>-d<dust>-<rate>-ob<basis>-x<exit>[-loc][-L<lev>]`.
  `LCACHE` is not in the name, because it is bit-identical.
- Grid guard: the six pre-registered cells, θ .05 / d .1 / fixed / L 1 / no locate × {target, delta} × {1, .05, .1},
  run freely. Every other cell needs `EXTRA_CELL=1`.
- **Exits non-zero** when the bounded run is not `0 completed` (T34c m3), or when `summary.json` is missing.
- Bounds are v51's: 180 s, 1536 MiB, `--max-bytes 1073741824`.
- `obtarget-x1` has the parent's own flags. The script compares every published file with
  `REF=build-equity/mega-nav-v51-$COMBINED-t.05-d.1-fixed` and **exits 3 on any difference**; `REF_CHECK=0` turns the
  check off. This is a real-data byte check of the default path and of the cache.
- DRY runs were exercised on: a grid cell (delta, x.05), the reference cell, locate without `EXTRA_CELL` (exit 2),
  locate + L1.279 + LCACHE=0 with `EXTRA_CELL` (directory `...-obtarget-x1-loc-L1.279`), `EXIT_RATE=0` (exit 2), the
  `fit` phase (exit 2), and a validation-named `REF` (exit 2).

## 2. How unchanged bytes are shown

1. **By construction.**
   - Every new branch is gated by a non-default value: `delta_order()` is false when the `delta` vector is empty;
     `decaying_exit()` requires `exit_rate != 1`; `no_short` is empty; `liquidity_cached()` is false.
   - Under target, `execute_orders` evaluates the same expressions as before: `order - held`, and `held = order` on a
     complete fill.
   - The recipe and summary keys are added only when non-default. `nlohmann::json` sorts keys, so absent keys leave
     the dump unchanged.
   - `aim_partial_summary` builds the same object before an optional key.
   - The per-name-v1 path is unchanged: same cache calls, same `fill_liquidity(execution, decision)` arguments.
2. **Tests pinned to pre-change bytes** (`NavV6.OrderBasisTargetAndExitRateOneAreBitIdentical`):
   - `--order-basis target --exit-rate 1` publishes every file byte for byte as the plain run, under both baseline-v1
     and the v5 flags. It also asserts the plain recipes still hash to the pre-change pins `73cb45f1` (baseline) and
     `d53f0c09` (v5 fixed).
   - `NavExecutionOptions{}` through the six-argument overload publishes the plain run byte for byte.
   - All existing `StrategyNavReplay.*`, `NavV5.*` and `TargetReplayV5.*` hand-computed and SHA-pinned tests are
     unchanged, and must pass as before.
3. **Delta equals target without drift** (`NavV6.DeltaWithoutDriftIsTargetBitForBit`). Constant prices, capped S2/S3
   residuals, non-rebalance kept orders, forced and absent exits, S3 write-offs, the five-book financing matrix with
   special-name blocks, baseline at cadence 1 and 3, v5 at θ 1, and θ .05 with the exit rate: every compared field is
   bit-identical. This pins every delta code path to the target arithmetic.
4. **F8** (`NavV6.LiquidityCacheAtFixedRateIsBitIdentical`): lockstep books under baseline, v5, and v5 + exit rate, each
   under both bases, are bit-identical with the cache on and off. The pinned CLI run with fields (12 files) is byte
   for byte.
5. **nav_summ** (`test_every_pre_v6_field_and_line_is_byte_identical`): against blob `73fbb749` (`nav_summ.py` at the
   base), every JSON field, `cost_per_gmv_turnover` included, and every text line is byte-identical. The only additions
   are the three new keys and one line per directory. The existing pre-T41 test was extended for the same additions.
6. **Real data** (root): the `obtarget-x1` byte check in `v6_train.sh`.

## 3. Tests (desk-checked; to be run by the root)

`atx-impl/tests/strategy_nav_replay_test.cpp`, new `NavV6.*`:
1. `OrderBasisTargetAndExitRateOneAreBitIdentical` — (a); see §2.2.
2. `LiquidityCacheAtFixedRateIsBitIdentical` — F8.
3. `DeltaWithoutDriftIsTargetBitForBit` — (b), the no-drift identity.
4. `DeltaThetaOnePlansTheAimAndRidesOneDayOfDrift` — (b), hand-computed at NAV 1000. Planned weights are the aim under
   both bases. After the drift day the target book holds -450/450 (traded 260) and the delta book -510/450 (traded
   200). Both hold -420/420 after a drift-free day.
5. `DeltaCappedResidualKeepsItsDecisionDollarDelta` — residual carry. S2 fills under target are 2000, 2000, 380, ending
   on 5000; under delta 2000, 2000, 1000, ending on 5620.
6. `DeltaSpecialTierOrdersStayTargetUnderTheLocateRule` — the locate guard. The unguarded delta book opens a short of
   0.6 × 1/3 − 1/6 of NAV on a special name; the guarded book ends long on its target.
7. `ExitRateDecaysPresentNonmemberInTheNavBook` — (c). Exit rate 1 equals the default book. At .05 the planned forced
   turnover follows the geometric decay to 1e-12 and snaps at decision 54; an absent name exits at once.
8. `LocateInAimLeavesLessNetThanThePostBlock` — (d). One special, lowest-ranked name with mid-range exposures (name 6).
   Mean |net| under locate-in-aim is below 0.5× the post-block value (the post-block book is above 0.05), and fewer
   dollars are blocked. Exactly one aim is zeroed per decision. The run is refused without fields or without
   neutralization.
9. `RecipeSummaryKeysAndCliRefusals` — every new recipe and summary key is present only when set. Six value refusals
   exit 1 and three usage errors exit 2, all before any output.
10. `TargetsVerbCarriesTheExitRate` — the `targets` verb's keys, `--exit-rate 1` equal to the plain run byte for byte,
    and exit 1 without `--role`.

`atx-impl/tests/strategy_target_replay_test.cpp`, new `TargetReplayV6.*`:
- `ExitRateDecaysPresentNonmemberAndSnapsInsideTheDustBand` — exact bits of `current × (1 − .05)` and of the forced
  turnover. The snap is at decision 59.
- `ExitRateOneIsTheImmediateExitAndDecaySpreadsTheSameTotal`.
- `ExitRateRefusals`.

`studies/test_nav_summ.py`: +2 tests; the pre-T41 test was adapted. **Ran 20/20 passed.**

**Root command lines** (from the integration tree with both lanes merged):
```powershell
powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests atx-equity-strategy-targets
<build>\...\atx-impl-strategy-target-tests.exe --gtest_filter=TargetReplayV5.*:NavV5*:*BitIdentical*:*V6*
<build>\...\atx-impl-strategy-target-tests.exe            # full exe: shared seams (form_desired, update_weights) changed
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .superpowers/sdd/mega-alpha-20260926/studies/test_nav_summ.py
```
Expected: the filter count is the previous 18 plus 13 new tests; the full exe is the previous total plus 13.
TRAIN cells, after the build:
```bash
COMBINED=ew bash .superpowers/sdd/mega-alpha-20260926/studies/v6_train.sh nav                    # obtarget-x1: byte check vs v5.1 parent
COMBINED=ew ORDER_BASIS=delta bash .superpowers/sdd/mega-alpha-20260926/studies/v6_train.sh nav
COMBINED=ew EXIT_RATE=.05 bash ...v6_train.sh nav ; COMBINED=ew EXIT_RATE=.1 bash ...v6_train.sh nav
COMBINED=ew ORDER_BASIS=delta EXIT_RATE=.05 bash ...v6_train.sh nav ; ... EXIT_RATE=.1 ...
```
Run `DRY=1 ...` first to print the exact command lines.

## 4. Open risks and concerns

1. **Not compiled.** Everything was desk-checked for `/W4 /WX` (clang-cl): line length is at most 100, there are no
   narrowing conversions, and the overloads resolve uniquely. A compile error is still possible. The expectations most
   exposed to a hand-arithmetic slip:
   - the snap decisions 59 (TR) and 54 (NAV); margins are 2–4% on each side;
   - the locate-in-aim thresholds (> .05, < .5×);
   - `member_tiers {4,0,1}` and the ±1 $ tolerance in the special-tier test;
   - `zeroed_special_short_aims == 6`.
   If one fails, the formula it pins is stated in its comment.
2. **Merge with V6-C2 (pool-11).** Both lanes add a trailing defaulted parameter to `form_desired` and touch
   `validate_nav_config` and the reserve call. The conflicts are textual.
   - I did not edit `nav_reserve_bytes`. I only pass `liquidity_cached(base)` into its `per_name_rate` parameter, whose
     name is now misleading. C2 rewrites that function anyway (reserve at geometry).
   - `locate_in_aim` checks `neutralize != None`, so C2's new ids pass.
3. **Pre-registration interaction.** With `EXIT_RATE < 1`, the "dust re-tune {0, .2}" cell at dust 0 is **refused**,
   because there is no snap band. A C2-on winner can only be re-tuned at dust > 0; the root should rule on this before
   the re-tune.
4. **Locate-in-aim changes every book's construction**, including the S2 × flat-300-v0 book, which has no locate rule.
   This is intended (the target is shared), but it should be disclosed.
5. **Delta raises gross dispersion** (drift rides) and C2 holds ex-members longer. Both can raise steady-state gross
   against the fixed L and the ≤ 1.05 cap (review C2). Re-derive L on the new post-ramp keys (C4).
   - C2 also increases exposure to delisting-prone ex-members; watch S3.
   - The special-name exception under delta means drift is still traded back on those names.
6. **`v6_train.sh` REF check.** If the v5.1 parent was produced by a binary whose default path differs for another
   reason, the `obtarget-x1` cell exits 3. That is intentional: investigate, then use `REF_CHECK=0` only with a ledger
   note. The script also assumes v51's pin files exist in pool-2, and it pins the v5.1 library `9e5ea08c`.
7. **Kept orders under delta at cadence 1** occur only on neutralize-skipped decisions, so the `clamp_kept_order`
   conversion is exercised only in the no-drift identity test, not in a drift test.
8. The liquidity cache leaves no trace in the output files (only in the console / `stdout.log`). This is required for
   byte identity.

## Fix round 1 (review task-V6C1-review.md: FIX REQUIRED I1, plus M3 and M5)

Commits on `feat/mega-alpha-v6-c1-20260927`, on top of `5e1c7f6d`:
- `2bcfe646` fixes I1 and M3 (C++ source and tests).
- `335c955e` fixes M5 (`v6_train.sh`).

Nothing was built and no real data was used.

- **I1, the dangling temporary** (`atx-impl/tests/strategy_nav_replay_test.cpp`, the `exit_summary` binding at about
  line 2527).
  - The fix binds `read_json(...exit/summary.json)` to a named local before the range-for.
  - I checked both replay test files for the same pattern: every range-for with a call in its range, and every
    `const auto& x = <call>...` binding.
  - Every other case iterates or binds a named object, or binds a temporary directly (`{...}` lists,
    `directory_iterator(dir)`), which the language keeps alive for the loop. No other site is affected.
- **M3, key naming and stale text.**
  - The recipe key `exit_rule` is now `exit_rate_rule` (target `cpp:724`; the constant is now
    `exit_rate_rule_declaration`). The summary key stays `construction.v5.exit_rate`; it has no `_rule` companion.
  - The aim_partial text's nonmember clause now comes from `detail::nonmember_exit_clause(cfg)` (`detail.hpp:127`,
    target `cpp:681`). The clause is "nonmembers exit to 0" at exit_rate 1 and "nonmembers follow exit_rate_rule" below
    1. It is used by the target replay's fixed `aim_partial` text and by the NAV per-name text
    (`per_name_rate_head` + clause + `per_name_rate_tail`, nav `cpp:1127-1135` and `cpp:1237-1238`).
  - Comments in `strategy_target_replay.hpp` were updated to match.
  - **Default path.** No key is emitted or changed on the default path. `exit_rate`, `exit_rate_rule` and the changed
    clause appear only with `--exit-rate` < 1.
  - **Default bytes.** At exit_rate 1 both aim_partial strings are the pre-fix bytes. I checked this mechanically:
    evaluating the C literal concatenations from HEAD and from the working tree gives identical strings for the target
    text (374 chars) and the per-name text (903 chars).
  - **Tests.**
    - The recipe pins `d53f0c09` and `73cb45f1` still apply unchanged.
    - New assertions: the rename (`exit_rate_rule` present, `exit_rule` absent) and the default text "nonmembers exit
      to 0" in the v5 recipe and in the plain `targets` recipe.
    - The decaying clause is asserted in the fixed NAV recipe, in a new per-name + `--exit-rate .05` NAV run (`exitpn`,
      which also shares the same `exit_rate_rule` text), and in the `targets` verb.
- **M5 (`v6_train.sh`).**
  - `set -uo pipefail`.
  - `REF_CHECK` must be 0 or 1; anything else exits 2.
  - With `REF_CHECK=1` (default) and REF absent, the script **exits 3 before anything runs**; a DRY run reports this.
    The old "skipped" branch now also exits 3, though it is unreachable.
  - A failed `nav_summ` exits 1, read from `PIPESTATUS[0]` so that grep's no-match status cannot be mistaken for a
    failure. With `REF_CHECK=0` and REF absent, nav_summ runs unpaired, as before.
  - Verified: `bash -n`. DRY runs of the reference cell (REF-absent note, rc 0), delta x.05 (rc 0), `REF_CHECK=2` (rc
    2), `REF_CHECK=0` (no note, rc 0) and `EXIT_RATE=0` (rc 2). A stub harness ran the non-DRY gate and tail, extracted
    verbatim from the script:
    - REF absent with `REF_CHECK=1`: rc 3.
    - REF absent with `REF_CHECK=0`: rc 0, unpaired.
    - REF present: rc 0, `--reference` passed.
    - nav_summ exiting 4: rc 1.
- M1, M2, M4, M6 and M7 were not addressed, as instructed. They remain open, disclosure-only items.

**Root re-run (after merging `335c955e`):**
```powershell
powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests atx-equity-strategy-targets
<build>\...\atx-impl-strategy-target-tests.exe --gtest_filter=TargetReplayV5.*:NavV5*:*BitIdentical*:*V6*
```
Expected: all pass, including `NavV6.RecipeSummaryKeysAndCliRefusals`, with the same test count as before.
`NavV6.OrderBasisTargetAndExitRateOneAreBitIdentical` and `NavV5.*` re-prove the default recipe bytes. A re-run of the
real-data default cell is not needed for M3, because the default text is byte-identical, but the obtarget-x1 cell's
REF check covers it anyway.
