# Task T36 review: per-name trading rate `rate per-name-v1` (base 98277c45, head 0fd7f435, pool-3)

### Spec Compliance

- ✅ **Spec compliant.** Every brief step and every binding controller ruling is met in the diff:
  - Config enum and fields: `strategy_nav_replay.hpp:930-955`, with the brief's exact defaults 10, .01, .15 and .2.
  - Free function `per_name_rate_v1` with the brief's exact signature: `hpp:941-943`, defined at `strategy_nav_replay.cpp:1563-1573`.
  - `liquidity_row` reused through a window/row split: `cpp:360-401`.
  - The rate span is built in `plan_decision` (`cpp:749-752`) through `per_name_rates` (`cpp:722-729`). Members use the formula. No-liquidity or fallback names and nonmembers get rate_min.
  - CLI: `--rate`, `--rate-rra`, `--rate-min`, `--rate-max` (`cpp:1985-2021`).
  - Recipe: `rate = "per-name-v1"` plus its params (`cpp:1138-1144`).
  - Summary: `construction.v5.rate_stats` (`cpp:1146-1153`, `:1820-1823`).
  - The brief's three fixtures are present with the ruled field names, plus two more: `strategy_nav_replay_test.cpp:1811`, `:1837`, `:1886`, `:1927`, `:1970`.
- ✅ **Ruling: rate_stats fields.** `NavRateStats` (`hpp:992-995`) and `rate_stats_json` (`cpp:1146-1153`) hold exactly {n, mean, min, max, p05, p50, p95, at_min_count, at_max_count, share_at_min, share_at_max}. The summary emits all 11. The fixture asserts the sorted key set (`test:2043-2053`).
- ✅ **Ruling: T30 minor, span validation in every build.** `check_rates` (`strategy_target_replay.cpp:239-249`) is the first statement of `update_weights` (`:255`). It returns InvalidArgument in debug and release when:
  - the span is non-empty under a rule other than v5;
  - the size is not `instruments`;
  - any entry is outside [0, 1], or is NaN or inf.
  - The T30 release fallback `per_name = size == instruments` is gone (`:205`).
  - `aim_partial_weights` has one caller, `:257`, which runs after the check (grep).
  - Fixture: `TargetReplayV5.PerNameRateSpanIsChecked` (`strategy_target_replay_test.cpp:857`). It covers short and long spans, the baseline rule, NaN, −.01, 1.01, inf and a NaN nonmember, and checks that weights stay untouched.
- ✅ **Ruling: fixed-rate bytes equal T30 (verified by reading):**
  - The recipe gains keys only under PerNameV1 (`cpp:1138`).
  - The summary v5 object is the unchanged `aim_partial_json` value under Fixed (`cpp:1817-1826`).
  - The CSV `applied_fraction` under Fixed is still `rebalance ? trade_fraction : 0`. The override runs only when `per_name` is set (`target.cpp:205`, `:234`).
  - Execution under Fixed calls `liquidity_row(c, t, i)`, which is `liquidity_row(c, window_liquidity(c, t, i))` (`cpp:399-401`). The values are unchanged: `isnan(sigma)` holds exactly when the old predicate `pairs < min_vol_pairs || !isfinite(m2) || m2 < 0` holds, because a non-fallback window has pairs ≥ min_vol_pairs ≥ 2 (validated) and m2 is finite and ≥ 0. So sqrt is never NaN (`cpp:385-388`, `:395-396`).
  - Admission: `budget.add(0, rate_name_bytes)` is a no-op (`Budget::add`, `cpp:89-92`). `nav_reserve_bytes(..., false)` is unchanged (`cpp:1410-1418`).
  - `validate_nav_config` adds only a Fixed-at-defaults check on fields that T30 did not have.
- ✅ **Ruling: liquidity read once.** `fill_liquidity` (`cpp:419-432`) runs once per session, shared by every book, before the book loop (`cpp:841-843`). Nothing recomputes liquidity per name per day.
- ⚠️ **Cannot verify from the diff:**
  1. **Nothing was compiled or run.** Root: build `atx-impl-strategy-target-tests,atx-equity-strategy-targets`. Run `NavV5.*:TargetReplayV5.*`, then the regression `StrategyTargetReplay.*:StrategyNavReplay.*`.
  2. **The fixed v5 `recipe.json` pin `d53f0c09…` (`test:1999`) comes from the Python emulation, not from the T30 binary.** I did not run `t36-sha/v5_fixed_recipe_sha.py`. It does add the same 5 v5 keys that `construction_recipe_json` emits (`target.cpp:660-664`), which is 37 keys in total, consistent with T30's `v5_recipe_keys`.
  3. **Fixed-rate summary and CSV bytes are not pinned against T30.** The fixture only proves that the three T36 entry points agree with each other. Root should run one fixed v5 cell with v5-0 (T30) and v5-1 (T36) and `cmp` recipe, summary and daily CSV, then run the ledger D2 check (af058239 / 3f846525) with v5-1, as T37 plans.
  4. **The report cites a dispatch ruling "R-b" for NAV_d = pre-trade NAV.** I could not find it in `progress.md` or in the interim handoff. The controller should confirm it (see Minor 2).
  5. **`EXPECT_GT(capped, 0U)` in `FixedEqualsTradeFraction` (`test:1910`) depends on the data.** My estimate says S2 caps bind: $62.5k orders against roughly $6k at 1% of the diluted ADV. Only the root run confirms it.

### Named-risk checks (one focused check each)

- **(a) ADV units, dollars vs shares.** Raw dollars, used the same way everywhere:
  - `window_liquidity` sums `raw_close[b] * volume[b]` over present rows and divides by w (`cpp:368-389`). Volume is shares, per the `NavReplayInput` contract and the liquidity recipe text.
  - The only production call site, `per_name_rates` (`cpp:726-727`), passes `(rra, lambda, b.nav_pre, cache.sigma[i], cache.adv[i], min, max)`. That matches the parameter order `(…, daily_vol, adv_dollars, …)`.
  - The test oracle `expected_rate_c` passes `dollars / 63.0` (`test:1799-1800`).
  - I recomputed the brief values: 0.023717 ($50m), 0.10607 ($1bn), 0.00335 → .01 ($1m), and 2.5 → .15. The $200m value is 0.04743.
- **(b) Rate span size in every build type.** `check_rates` is described above. The error goes through the file's `co::Status` idiom (`ATX_TRY_VOID`), not an assert. Under PerNameV1 the span is `cache.rate`, sized `instruments` by construction (`cpp:408-414`).
- **(c) Fixed bytes equal T30.** See the ruling bullet above. I also checked that T31's `studies/v5_train.sh:140` passes no rate flags for `RATE=fixed`, so fixed cells take the default path. Its per-name flags (`:141`) pass T36's CLI checks.
- **(d) No NaN or inf in a rate:**
  - `per_name_rate_v1` (`cpp:1563-1573`) returns rate_min for any non-finite or ≤ 0 value of rra, lambda, nav, sigma or ADV. That covers ADV 0 (zero volume), ADV 0 with NaN sigma (absent window or fallback), sigma 0 (flat window), and NaN `nav_pre`.
  - An inf/inf quotient gives NaN, which fails `theta >= rate_min` and returns rate_min. +inf is clipped to rate_max.
  - Zero prices cannot reach it: present cells are validated finite and > 0, and absent rows are skipped.
  - Validation guarantees 0 < rate_min ≤ rate_max ≤ 1 and refuses NaN (`cpp:274-282`, `within` at `cpp:94`).
  - `check_rates` is a second guard. `RateStatistics::add` asserts the range (`cpp:134`).
  - Fixtures cover 0, −1, NaN and inf in each of the 5 inputs (`test:1822-1831`).
- **(d′) The cache covers every name execution prices.** This is the named risk behind the debug-only assert at `cpp:602`. `fill_liquidity` runs before MARK, so it relies on MARK only cancelling orders.
  - I grepped every `active[i] =` write: `:487` (=0, write-off), `:601` (=0), `:617` (execute residual), `:717` (=0, clamp), and `:758`/`:760` (plan).
  - MARK never activates an order, so the pre-MARK union is a superset of what execution prices. The invariant holds today; see Minor 1 for its fragility.
- **(e) Compile desk-check.** No blocker found:
  - `ATX_TRY_VOID` returns `tl::unexpected<Error>` (`atx-core/include/atx/core/error.hpp:163-168`). It is valid in `Status` functions and in `Result<TargetReplayResult> replay_targets` (`target.cpp:389`).
  - Every `update_weights` call site consumes the new `[[nodiscard]] Status`: `target.cpp:389`, `:1008`; `nav.cpp:751`; the target tests at `:669`, `:670`, `:701`, `:870`, `:884`, `:904`, `:909`. There are no other callers in the worktree (grep).
  - `plan_decision` has no bare `return;` left (`cpp:739-776`), and it has one caller, `:873`. `execute_orders` has one caller, `:855`. `nav_reserve_bytes` has one caller, `:1896`.
  - `liquidity_row` overloads have arities 2 and 3, so there is no ambiguity.
  - `aim_partial_json` returns `Json` (`cpp:1790`), so `v5["rate"] = …` compiles.
  - `NavRun::base` exists (`cpp:1781`).
  - The file-scope `nan` and `inf` (`cpp:38-39`) precede `RateStatistics`. `finite_or_null` (`cpp:984`) precedes `rate_stats_json`.
  - `per_name_rate_v1` sits at namespace scope beside the public `fixed_nav_scenarios`/`nav_scenario_matrix`, so the anonymous-namespace `per_name_rates` finds it through the header.
  - Test helpers exist with the used signatures: `Panel` (`test:54-82`, `nav() const`, `price`, `absent`, `by_name`, `raw`), `randomize_rows` `:146`, `config` `:95`, `flat` `:83`, `aim_partial_nav` `:1544`, `sorted_keys` `:1561`, `expect_same_result` `:345`, `publication_panel` `:266` (9×3 with name 0 absent at row 4, so 3·7−1 = 20 samples, as the test states), and target `aim_partial(theta, dust, leverage = 1)` `:606`.
  - The target test already includes `<algorithm>` and `<limits>`.
  - An awk pass over all 6 C++ files finds no new line over 100 columns. The long lines that exist are all pre-existing, in `target.cpp` and the target test.

(Method: I read the diff in one pass, in three chunks because of its length. I read head files in pool-3 at 0fd7f435, whose tree is clean, only for the focused checks named above.)

### Strengths

- **The window/row split proves the execution refactor value-identical rather than just asserting it.**
  - The `isnan(sigma)` ⇔ fallback argument is correct and written down in a comment (`cpp:383-384`).
  - `FixedEqualsTradeFraction` runs all 3 fixed scenarios in lockstep at a NAV where S2 caps bind. It compares every day, event and deployment field bit for bit, which pins the shared-cache execution path to the per-book path.
- **The T30 release-fallback minor is fixed at the right layer.** It is an error return in `update_weights` itself, so any future caller is protected too, not just the NAV. The fixture checks that a refused call leaves weights and turnover untouched.
- **`per_name_rate_v1` is total.** It never returns NaN, and its guard (`!(theta >= rate_min)`) also absorbs the inf/inf corner. Fixtures cover each unusable input across the 5 parameters, with bit equality against the literal expression and the 1/sqrt(NAV) scaling.
- **The fixed path is inert by construction:**
  - The cache is empty (`on()` is false) and no rate span is passed.
  - No recipe or summary keys are added.
  - Admission numbers are unchanged.
  - The CLI refuses `--rate` without v5, and `--rate-*` without per-name-v1, with a usage error before any output directory exists.
- **`RateStatistics` keeps memory flat** (32 KiB per book) and is exact where the T38 gate reads it: n, mean, min, max, counts and clip-mass quantiles are exact.
- **The no-liquidity fixture meets review focus 3 exactly.** It covers zero volume over the whole window and absence over the whole window, with an independent two-pass oracle for the liquid name. It checks `applied_fraction == stats.mean` bit for bit (same summation order) and the planned-gross decomposition.

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

None.

#### Minor (Nice to Have)

1. **The cache-coverage invariant is enforced only by a debug assert.** Location: `strategy_nav_replay.cpp:602` together with `fill_liquidity` at `:419-432`.
   - **What's wrong:** execution reads `cache.adv[i]` / `cache.sigma[i]` whenever the cache is on. The cache is filled before MARK, on the argument that MARK only cancels orders. That holds today (see (d′)).
   - **Why it matters:** a future edit that makes MARK or a stale/terminal exit activate an order would give a release build NaN ADV. Execution would then silently block that fill ("unusable ADV fills nothing"), which is the silent corruption house rule §4 forbids ("fail safe in release"). T32 is scheduled to edit this file's MARK/terminal logic.
   - **Fix:** `cache.on() && !std::isnan(cache.adv[i]) ? cached : liquidity_row(c, t, i)`. The two branches give the same values, so this costs nothing and removes the coupling. Keep the assert.
2. **NAV_d is the pre-trade NAV, while this file's own "decision-NAV dollars" is `nav_post`.** Locations: `cpp:726` (`b.nav_pre`); recipe text `cpp:1064`; `hpp:926-928`.
   - The file sizes orders as `planned * b.nav_post` and computes current weights as `held / b.nav_post` (`cpp:745`, `:758`). The brief's `NAV_decision` most naturally maps to that value.
   - The numeric effect is negligible: the two differ only by session-d trade costs, and the rate scales as NAV^−1/2.
   - The choice is disclosed in the recipe and header, and the report attributes it to a ruling R-b that I could not locate (⚠️ 4).
   - Either confirm the ruling, or switch to `b.nav_post` and update the text.
3. **`per_name_rate_declaration` re-states T30's `aim_partial` text.** Locations: `cpp:1058-1069` and `strategy_target_replay.cpp:664-…`.
   - The move/dust/nonmember sentence is re-typed, then the rate clause is appended. If T30's text is later corrected, the per-name recipe silently drifts.
   - Fix: build it from the shared text plus the rate clause. Alternatively, leave it, since both are frozen recipe strings.
4. **`NavRateOptions` duplicates `NavReplayConfig`'s five rate fields.** Locations: `hpp:953-963`; the copy is field by field at `cpp:1892-1893`.
   - A shared `NavRate` sub-struct used by both would remove the parallel lists and the copy. A sixth rate parameter could otherwise be added to one list and missed in the copy.
5. **Coverage gaps (tests only):**
   - The `--rate-lambda` CLI flag is never exercised.
   - `PerNameRate_RecipeSummaryAndCli`'s per-name statistics are degenerate. On the 9-session fixture every sample is the fallback's rate_min. So interior quantile and mean serialization through `rate_stats_json` is only covered at the struct level, by `NoLiquidity`.
   - A single extra `--rate-lambda .4` CLI run on the `SyntheticRole` panel would close both gaps.

### Assessment

**Task quality:** Approved

**Reasoning:** The diff meets the brief and all four binding rulings:
- The rate formula uses raw-dollar ADV consistently and cannot produce NaN.
- Wrong-size spans are refused with an error in every build type.
- The fixed-rate path is unchanged by construction.
- Liquidity is formed once per session and shared across books.

The desk-check finds no compile blocker. What remains is root-side confirmation: the build and test run, the emulated recipe pin, and T30-vs-T36 fixed-cell byte equality with the v5-0/v5-1 binaries. The Minor items are robustness and DRY polish.
