# Task T36 report: per-name trading rate `rate per-name-v1` for aim-partial-v5 (C++)

Status: **DONE_WITH_CONCERNS**. The concern is that nothing was compiled or run, as the lane
contract requires. Details are under "Concerns" below.

- Worktree: `C:/atx-wt/pool-3`
- Branch: `feat/mega-alpha-v5-construction-20260927`
- Base: 98277c45 (T30 landed and reviewed)

## Commits

- `7c89bbd2` feat(nav): per-name trading rate per-name-v1 for aim-partial-v5 (T36). This holds
  the code and fixtures.
- A second commit adds this report and the SHA derivation script under `t36-sha/`, force-added.
  Its subject is the same, with "report" appended.

## What was built

**Formula.** `per_name_rate_v1(rra, lambda, nav, daily_vol, adv_dollars, rate_min, rate_max)` is a
free function declared in `strategy_nav_replay.hpp` and defined in `strategy_nav_replay.cpp`. It is
`noexcept` and `[[nodiscard]]`, and computes

    theta = clip( sqrt( rra * daily_vol^2 * adv_dollars / (lambda * nav) ), rate_min, rate_max )

- The expression is evaluated left to right: `rra * daily_vol * daily_vol * adv_dollars / (lambda * nav)`.
- If any of rra, lambda, nav, daily_vol or adv_dollars is not finite and > 0, it returns rate_min.
- A NaN quotient (inf/inf on overflow) also returns rate_min. `!(theta >= rate_min)` catches it.
- +inf clips to rate_max.
- It never returns NaN.
- The precondition 0 < rate_min <= rate_max is checked by config validation.

**Config** (`strategy_nav_replay.hpp`, as the brief specifies):

- `enum class NavRateRule : u8 { Fixed = 0, PerNameV1 = 1 }`.
- Declared defaults: `nav_rate_rra` 10, `nav_rate_lambda` 0.2, `nav_rate_min` 0.01, `nav_rate_max` 0.15.
- `NavReplayConfig` gains `rate{Fixed}, rate_rra, rate_min, rate_max, rate_lambda`, appended at the end.

**Validation** (`validate_nav_config`, InvalidArgument):

- PerNameV1 requires `target.rule == AimPartialV5`.
- rate_rra and rate_lambda must be in (0, 1e6].
- 0 < rate_min <= rate_max <= 1 must hold. NaN is refused through `within()`.
- Fixed requires every rate_* at its default, so a fixed-rate recipe cannot silently carry rate
  parameters. This mirrors T30's aim_leverage/dust rule.
- An out-of-enum rate value is refused.

**NAV path** (`strategy_nav_replay.cpp`):

- `liquidity_row` is split into two parts:
  - `window_liquidity(c, t, i)`: the unchanged O(w) loop, returning ADV and sigma. sigma is NaN
    exactly when the window has too few pairs.
  - `liquidity_row(c, WindowLiquidity)`: applies the scenario's half spread and fallback sigma.
  - The old `liquidity_row(c, t, i)` composes the two. The values are identical: the same
    operations, and `isnan(sigma)` is true if and only if the old fallback predicate is true
    (argued in a code comment).
- `LiquidityCache` exists only under per-name-v1 and is empty otherwise. It holds `adv`, `sigma` and
  `rate` vectors of `instruments` doubles, allocated once in `run_books` and reused every session.
- `fill_liquidity` runs once per session before the book loop, and only under PerNameV1. It forms
  the window for every name present at t that is either:
  - a decision member at t, or
  - a holder of a working order in some book at an execution session t.

  MARK only ever cancels orders, so this is a superset of what execution prices. Every other name is
  set to NaN.
- `execute_orders` reads the cache when it is on, and otherwise uses the old per-book
  `liquidity_row`. A debug `assert` checks coverage.
- `per_name_rates(c, b, d, cache)` runs per book at each decision:
  - Each member gets `per_name_rate_v1(rate_rra, rate_lambda, b.nav_pre, sigma_i, adv_i, rate_min, rate_max)`.
  - Nonmembers get rate_min. The rule never moves them, but the span must be all-valid.
  - One sample per member goes into the book's `RateStatistics`.
- `plan_decision` now returns `co::Status`. It passes `cache.rate` as the `per_name_rate` span under
  PerNameV1 and an empty span otherwise. The call site uses `ATX_TRY_VOID`.
- `RateStatistics` is a fixed-size streaming accumulator, one per `Book`:
  - It keeps the exact n, sum, min, max and clip masses (== rate_min, == rate_max).
  - Interior values go into 4096 equal bins over [rate_min, rate_max].
  - Quantiles use rank ceil(q n):
    - A rank inside the lo or hi mass returns rate_min or rate_max exactly.
    - Otherwise the result is the bin's upper edge, capped at the observed max. The error is less
      than (max − min)/4096, about 3.4e-5.
  - Results: `NavReplayResult::construction.rate_stats` (new `NavConstructionStats` / `NavRateStats`).
    It is set only under PerNameV1. A fixed rate leaves every field zero, and n == 0 gives NaN
    statistics.

**Target replay** (`strategy_target_replay.cpp`, detail header):

- `update_weights` returns `[[nodiscard]] co::Status`.
- A new `check_rates` refuses any non-empty span, with InvalidArgument and before any weight moves,
  when the rule is not aim-partial-v5, or the size is not `in.instruments`, or any entry is not in
  [0, 1]. NaN, inf and nonmember entries are all checked.
- The T30 release-build fallback (`per_name = size == instruments`) and the non-v5 assert are removed.
- `aim_partial_weights` keeps a documentation assert.
- Under a non-empty span on a rebalance decision, `applied_fraction` = the members' mean rate, with
  dusted members included. Otherwise it is unchanged (trade_fraction or 0).
- `replay_targets` and the detail wrapper propagate the status.
- `aim_rate()` stays `"fixed"`: the target replay has no NAV or liquidity. Its seam comment now says
  the NAV overrides the spelling.

**Recipe and summary** (NAV):

- **Recipe, per-name only:** `rate = "per-name-v1"`, plus `rate_rra`, `rate_lambda`, `rate_min` and
  `rate_max`. `aim_partial` is replaced by a per-name description (`per_name_rate_declaration`). All
  other keys are the fixed-rate v5 recipe's.
- **Summary, per-name only:** `construction.v5.rate = "per-name-v1"`, plus
  `construction.v5.rate_stats = {n, mean, min, max, p05, p50, p95, at_min_count, at_max_count,
  share_at_min, share_at_max}`. Non-finite values are written as null.
- **Fixed:** nothing is added. The summary code path builds the same JSON object and assigns it.

**CLI** (`dispatch_nav_replay`):

- `--rate fixed|per-name-v1` (an unknown spelling exits 2).
- `--rate-rra`, `--rate-lambda`, `--rate-min`, `--rate-max`.
- Usage errors (exit 2): `--rate` present without `--rule aim-partial-v5`, and any `--rate-*`
  parameter without `--rate per-name-v1`.
- Invalid values (exit 1, from validation): for example rate_min > rate_max, or rra 0.
- Neither kind creates an output directory.
- `--help` lists the flags.
- The values reach the run through a new overload,
  `run_nav_replay(cfg, limits, fields, const NavRateOptions& rate, progress)`. `NavRateOptions` has
  the same field names as `NavReplayConfig`'s rate fields, and the run copies them into `base`.
- The four-argument overload now forwards with `NavRateOptions{}`.
- The target-replay CLI is unchanged, and rejects `--rate` as an unknown flag (exit 2).

**Admission.**

- `validate_nav_input` charges 3×8 bytes per name, only under PerNameV1 (`rate_name_bytes`).
- `nav_reserve_bytes` gains a `per_name_rate` flag and charges the same, for max_names.
- Under Fixed both charges are exactly as before.
- The per-book histogram (32 KiB) sits inside the existing 1 MiB `fixed_workspace_bytes`.

## Controller rulings: how each was applied

- **R-a:**
  - `NavRateStats` fields are exactly {n, mean, min, max, p05, p50, p95, at_min_count, at_max_count,
    share_at_min, share_at_max}, and the summary emits all of them. This happens only when v5 and
    per-name are both set: per-name implies v5 by validation.
  - The fixtures use these names.
  - There is one sample per member per decision, over every decision row, including a non-rebalance
    one: the rate is formed on each decision.
- **R-b:**
  - These names get rate_min, are counted in at_min_count, and never produce NaN:
    - ADV <= 0 (zero volume, or absent for the whole window);
    - sigma <= 0;
    - sigma NaN, which is the fallback, meaning fewer than `min_vol_pairs` usable return pairs.
  - NAV_decision = `b.nav_pre`, the book's pre-trade NAV at the decision session (`day.pretrade_nav`).
  - at_min_count counts every sample equal to rate_min. That includes liquid names clipped at the
    floor, not only no-liquidity names. Recorded in the header doc.
- **R-c:**
  - Under PerNameV1 the span is always `cache.rate`, which is non-empty and sized `instruments` by
    construction.
  - `update_weights` refuses any wrong span with an error return, the file's `co::Status` idiom.
    That makes the refusal work in release builds too.
  - Under Fixed the span is empty and every T30 path is unchanged:
    - `check_rates` passes empty spans;
    - the fixed `applied_fraction` is set exactly as before;
    - execution takes the old per-book `liquidity_row`, which is value-identical;
    - there are no recipe or summary additions;
    - the admission numbers are unchanged.
  - Fixtures: `PerNameRate_FixedEqualsTradeFraction`, plus the Fixed v5 recipe SHA pin and the byte
    identity checks in `PerNameRate_RecipeSummaryAndCli` (see below).
- **R-d:** implemented as described above. The defaults are RRA 10, λ 0.2, min .01, max .15. The
  recipe keys are as ruled. `--rate-lambda` is an extra flag; the config field and recipe key exist
  either way.
- **R-e:** see the next section.

## R-e: runtime and memory (what the existing code does)

**The existing code keeps no rolling structure.** `liquidity_row` recomputes the full O(w) window,
with w = 63 and two `std::log` per pair (`guarded_move`), for every (book, name with a working order,
execution session). Under v5, nearly every member has a working order on every session. A fixed-rate
run therefore costs about books × members × w per session. With `--fields` that is 5 books.

**T36, under per-name-v1 only,** forms the window once per session for the union of decision members
and working-order names, and shares it with all books:

- Execution reads it instead of recomputing, and each book still applies its own half spread and
  fallback sigma.
- The decision rates read the same session-d window [d−w, d), which is exactly the window execution
  at session d uses.
- So a per-name run computes about 1× members × w per session. That is fewer windows than a
  fixed-rate run (1/books), not more.
- The estimate for 754 TRAIN sessions × ~3,000 names × 63 is about 1.4e8 inner iterations, or a few
  seconds. The fixed-rate path is untouched, and its speed is unchanged.

**Memory is flat.** There are 3 vectors of `instruments` doubles (adv, sigma, rate), allocated once:
about 72 KiB at 3,000 names, and 480 KiB at the 20,000 cap. Each book adds one 32 KiB histogram.
Nothing is stored per sample or per decision.

The controller asked for "a single vector". There are three because execution reuses the shared
adv/sigma, which is the runtime win, and the per-book rate span needs its own buffer. All three are
O(names) and reused.

## Other decisions (small ambiguities; decided and recorded here)

1. **Where the config lives:** `NavReplayConfig`, as the brief specifies. The rate needs NAV and
   liquidity, and only the NAV replay has them. The CLI carries it in `NavRateOptions`, a new
   `run_nav_replay` overload, because `TargetReplayRunConfig` has only the target config.
2. **`theta` / `trade_fraction` under per-name.** They are still recorded, and still validated in
   (0, 1], but unused. The per-name `aim_partial` text says "trade_fraction (theta) unused". I did
   not null them, to keep downstream numeric parsing stable.
3. **`applied_fraction` under per-name.** It is the mean member rate on rebalance decisions (T30 left
   this open). It is a CSV value only; the CSV columns are unchanged.
4. **Liquidity window at the decision:** [d−w, d), per the brief's `liquidity_row(i, d)`. It does not
   include row d. It is causal and conservative.
5. **Quantiles** are histogram-based, as above, to keep memory flat. The pooled TRAIN sample is
   about 2.3M per book, and storing it would cost about 18 MB per book.

## Fixtures (postimplementation; not built, not run)

`atx-impl/tests/strategy_nav_replay_test.cpp`. Test-local helpers:

- `v5_config(theta, dust, nav)`.
- `SyntheticRole{Panel p; view()}`, built on the file's `Panel` builder, with two constructors:
  - `three_names_one_zero_volume_one_absent_window()`: 66 sessions, one decision at d0 = 63, w = 63.
    - A alternates 100/102 on zero volume.
    - B is absent on rows [0, 63).
    - C alternates 100/102 on 1.25e6 shares, giving θ_C ≈ 0.05016.
  - `default_role()`: T30's randomized 40×8 role, begin 5.
- `expected_rate_c`: C's window computed two-pass, independently of the replay.

The tests:

- `NavV5.PerNameRate_Formula`:
  - The brief's four checks (0.0237, 0.106, clip at 0.01, clip at 0.15) plus the $200m value 0.0474.
  - Bit equality with the literal expression.
  - 4×NAV halves θ.
  - min == max returns that value.
  - Every unusable input (0, −1, NaN, inf in each of vol, adv, nav, rra, lambda) returns rate_min.
  - inf/inf returns rate_min, and +inf returns rate_max.
- `NavV5.PerNameRate_NoLiquidity_UsesMin` (review focus 3):
  - n 3, min == rate_min, at_min_count 2 (A, B), at_max 0, share_at_min 2/3.
  - max ≈ θ_C (1e-12), and the mean.
  - p05 = p50 = rate_min exactly, and p95 within one bin of max.
  - Every day's gross_leverage and applied_fraction is finite.
  - The decision's planned gross = 0.5·rate_min + 0.5·θ_C: A moves at rate_min, and B is dusted
    (banded 1).
  - applied_fraction == stats.mean.
  - At NAV 4e9, max ≈ θ_C/2 and at_min_count stays 2.
- `NavV5.PerNameRate_FixedEqualsTradeFraction`:
  - Setup: fixed θ .05, and per-name with min = max = .05. All 3 fixed scenarios run in lockstep, at
    NAV 1e7 with w 63.
  - Every day's net_return is DOUBLE_EQ.
  - The full `expect_same_result`: every compared day field, construction, events, deployment and
    participation. applied_fraction is first checked within 1e-15 and then patched.
  - S2 has capped fills (> 0). This pins the shared cache in execution to the per-book arithmetic
    under caps, fallbacks and forced exits.
  - Stats: n = Σ decision members, at_min = at_max = n, min = max = p05 = p50 = p95 = .05, and shares 1.
  - The fixed rate has n 0.
- `NavV5.PerNameRate_ConfigRefusals`:
  - Each of these is refused with InvalidArgument:
    - PerNameV1 under baseline;
    - rate_min 0, rate_min .2 > max, rate_max 1.5;
    - NaN max, rra 0, NaN rra, lambda −.2, lambda 2e6;
    - enum value 7;
    - Fixed with rra 5, and Fixed with max .2.
  - Controls: a good per-name config, min = max = 1, and Fixed at the defaults.
- `NavV5.PerNameRate_RecipeSummaryAndCli`:
  - **Fixed v5, `aim_partial_nav(.5, .1, 1.5)` on `publication_panel`:**
    - `recipe.json` SHA-256 = `d53f0c09018244862e8974579e8a87184310778786b08545041c605f725fc71a`,
      hand-derived from the T30 writers (below).
    - recipe.json and summary.json bytes are identical across the 2-argument overload,
      `NavRateOptions{}` and CLI `--rate fixed`.
    - The recipe has rate `fixed`, and the summary v5 has no `rate_stats`.
  - **Per-name:**
    - The recipe keys are the fixed keys plus the 4 rate keys, with these values; `rate` is
      `per-name-v1` and `aim_partial` differs; every other key is equal to the fixed recipe's.
    - summary `recipe_sha256` = sha(recipe.dump()).
    - The daily headers are unchanged.
    - `rate_stats` has exactly the 11 keys.
    - n = 20 (7 decisions × 3 − 1 absent), all at rate_min: min, max and quantiles are .01, and
      share_at_min is 1.
    - The CLI per-name run (`--rate per-name-v1 --rate-rra 10 --rate-min .01 --rate-max .15`) gives
      bytes identical to the API run.
  - **Refusals:**
    - `--rate per-name-v1` and `--rate fixed` without v5 → 2.
    - `--rate-rra` without `--rate` → 2.
    - `--rate fixed --rate-min` → 2.
    - `--rate per-name` → 2.
    - min > max → 1.
    - rra 0 → 1.
    - None of these creates an output directory.

`atx-impl/tests/strategy_target_replay_test.cpp`:

- `TargetReplayV5.PerNameRateSpanIsChecked` (R-c):
  - A good span moves each member bit-exactly by θ_i·aim. The nonmember stays 0, and applied_fraction
    is bit-equal to the mean member rate.
  - These are refused with InvalidArgument, leaving weights and turnover untouched:
    - short and long spans;
    - a span under baseline;
    - NaN, −.01, 1.01 or inf entries;
    - a NaN nonmember entry.
  - Controls: an empty span uses the fixed θ (applied_fraction .05), and the bounds 0 and 1 are
    admitted.
- The three T30 `detail::update_weights` call sites are wrapped in `ASSERT_TRUE`, because the
  function is now `[[nodiscard]]` Status.

## Fixed v5 recipe pin: how it was derived

`.superpowers/sdd/mega-alpha-20260926/t36-sha/v5_fixed_recipe_sha.py` reuses T30's validated
emulation by importing `t30-sha/fixture_sha.py`. That emulation:

- is byte-exact on 25 committed C++ outputs;
- reproduces T30's pins `17349e65…`, `29559952…`, `73cb45f1…` and `ef16be17…`, which I re-ran and
  confirmed.

The script adds T30's v5 keys:

- rule `aim-partial-v5`, cadence 1, trade_fraction/theta .5, dust .1, L 1.5;
- `rate` and the `aim_partial` text, both regex-extracted from
  `git show 98277c45:atx-impl/src/strategy_target_replay.cpp`.

It prints `v5_fixed_nav_recipe_file_sha256 d53f0c09…` over 37 keys.

Run it with:

    python v5_fixed_recipe_sha.py ../t30-sha <root> <t30_target.cpp>

**If the pin fails at the root**, confirm with the T30 binary (98277c45) on the same fixture before
changing the constant. Never re-pin from post-change output. A mismatch that the T30 binary shares
means the emulation is wrong. A mismatch only in the T36 binary is a regression.

The summary bytes cannot be emulated, because they depend on the results. Only the three T36 entry
points are asserted byte-identical to each other. The authoritative T30 ↔ T36 summary check is the
root re-running the fixed v5 cell with both binaries, or T37/T38's re-run.

## Desk-check list

**Includes:**

- `strategy_nav_replay.cpp` gained `<cassert>`. That TU compiles with SKIP_PRECOMPILE_HEADERS, and
  everything else it uses was already included: `std::any_of` (`<algorithm>`), `std::unique_ptr`
  (`<memory>`), `std::ceil`/`isnan` (`<cmath>`).
- `strategy_target_replay.cpp` already had `<algorithm>` for `std::all_of`, and `<cassert>`.
- The header adds only `atx::` types and `std::` names it already had.
- The NAV test gained `<iterator>` for `std::istreambuf_iterator`.

**Signatures and call sites** (grep over the whole worktree):

- `update_weights` (now `co::Status`) is called from:
  - (1) `replay_targets`, via `ATX_TRY_VOID`;
  - (2) the detail wrapper, which returns it;
  - (3) NAV `plan_decision`, via `ATX_TRY_VOID`;
  - (4) the three T30 test sites, now `ASSERT_TRUE`;
  - (5) the new target fixture.
- `plan_decision` (now `co::Status`, with the added cache parameter) has one call in `run_books`,
  wrapped in `ATX_TRY_VOID`.
- `execute_orders` (added cache parameter) has one call.
- `liquidity_row(c, t, i)` has one call, in `execute_orders`. It is overloaded with
  `liquidity_row(c, WindowLiquidity)`, and the different arities mean no ambiguity.
- `nav_reserve_bytes` has one call.
- `run_nav_replay`: the 4-argument overload forwards, and the 5-argument overload is new. The tool
  `equity_strategy_targets.cpp` uses only `dispatch_nav_replay`, whose signature is unchanged.
- The new header function `per_name_rate_v1` is defined outside the anonymous namespace, before
  `nav_scenario_matrix`. The anonymous `per_name_rates` calls it through the header declaration.

**Constness and ownership:**

- New helpers take `const&` (Ctx, WindowLiquidity, books vector) or spans of const.
- The cache is passed by non-const reference only where it is filled or its rate is written.
- `RateStatistics::stats`/`quantile` are const and `[[nodiscard]]`.

**Warnings:**

- All added lines are ≤ 100 columns and ASCII-only (checked by script).
- There are no unused locals or functions.
- The constructor parameter was renamed so it does not reuse the member function name `on`.
- A shadowing test local was renamed (`panel`).
- The float `==`/`!=` in validation matches the T30 style, and `-Wfloat-equal` is not enabled.
- The only exhaustive-switch concern is `NavRateRule`, which is handled by ternaries and validation.
  There is no switch.

**Other checks:**

- **UB:** `static_cast<usize>(position)` only runs for lo < rate < hi. Position is then in (0, 4096)
  and clamped, and a NaN rate is impossible (asserted).
- **Lifetimes:** there is no range-for over a temporary's member. One such case was caught and
  replaced by a named `fixed_summary`.

## Root: build targets, test filter, CMake

- **Build:** `atx-impl-strategy-target-tests`, `atx-equity-strategy-targets`. There is no separate
  NAV test target: both test files are in `atx-impl-strategy-target-tests`.
- **Filter:** `NavV5.*:TargetReplayV5.*`.
- **Regression:** `StrategyTargetReplay.*:StrategyNavReplay.*`, in the same binary. These are the
  pre-existing byte-stability and bit-parity suites, and they exercise the refactored
  `liquidity_row` and the Status-returning `update_weights`.
- **CMake:** no change. There are no new translation units or test files.

## Concerns

1. Nothing was compiled or run. Compile errors, if any, will first show up in the root build.
2. The Fixed v5 recipe SHA pin (`d53f0c09…`) is hand-derived through the T30 emulation. Handle a
   mismatch as described in the pin section above. Summary byte identity against T30 is not pinned,
   because it cannot be emulated.
3. NAV_decision uses `nav_pre`, the pre-trade NAV of session d, as ruled. `nav_post`, the
   decision-dollar NAV the weights use, differs only by session-d costs. The recipe text states
   which one is used.
4. The quantiles are histogram approximations: exact at the clip masses, and otherwise the upper
   edge of the bin, within 3.4e-5. That is ample for the T38 gate p50 ∈ [0.02, 0.06]. The mean,
   min, max, counts and shares are exact.
5. Only per-name-v1 runs use the shared liquidity cache. Fixed-rate v5 runs keep the old per-book
   window cost, which is unchanged. That cost is books × members × 63 per session, and was the
   larger runtime risk before T36. Enabling the cache for Fixed runs would be bit-identical (the
   FixedEqualsTradeFraction fixture pins it) and a small follow-up if T37 shows Fixed v5 cells near
   the 60 s limit.
