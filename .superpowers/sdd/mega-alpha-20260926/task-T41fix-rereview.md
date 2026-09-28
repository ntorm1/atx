# Task T41-fix scoped re-review

**Scope:** fix base `faf5943f` → head `76c879f1` (pool-3 `feat/mega-alpha-v5-t41fix-20260927`), from `review-T41fix.diff`. Re-reviewer: Claude Opus 5.5, 2026-09-27.

**Method:** read-only desk check. I made no edits, builds or pytest runs, and ran nothing on real data. I did read CSV header names, the T40 JSON, and the C++ source at head in pool-3.

## Finding Verdicts

### I1 — cache coverage enforced only by a debug assert: ADDRESSED

**Where:** `atx-impl/src/strategy_nav_replay.cpp:605-608`.
- The assert is kept at `:605`.
- The row reads the cache only when `cache.on() && !std::isnan(cache.adv[i])`. Otherwise it calls `liquidity_row(c, t, i)`.

**It compiles by desk check:**
- The signatures are real:
  - `liquidity_row(const Ctx&, const WindowLiquidity&)` at `:391`
  - `liquidity_row(const Ctx&, usize, usize)` at `:399`
  - aggregate `WindowLiquidity{adv, sigma}` at `:360`
  - `LiquidityCache::on()` at `:411`
- `<cmath>` and `<cassert>` are included at `:5` and `:7`.
- Both ternary arms are `Liquidity` prvalues.
- `&&` inside a `?:` condition does not trigger clang's conditional-precedence `-Wparentheses` (that check covers arithmetic and bitwise operators only).
- Short-circuiting means an empty `adv` is never indexed when the cache is off.

**Output bytes cannot change:**
- When the cache is off, the path is unchanged.
- When the cache is on, a formed `adv` is never NaN:
  - `validate_nav_input` (`:300-307`) makes every present `raw_close` and `volume` finite.
  - `validate_nav_config` (`:265-267`) enforces `liquidity_window >= 2`.
  - So `window_liquidity`'s `dollars / w` (`:386`) is finite, and the old cached branch is taken exactly as before.
- The fallback does the same arithmetic as `fill_liquidity(ctxs.front(), ...)` (`:842-843`, `:429-430`). Every `Ctx` copies one `base` config, and scenarios differ only in `.scenario` (`:1600-1603`). So `liquidity_window`, `min_vol_pairs`, `x` and `volume` are shared across books.
- A GoogleTest was deliberately not added: the target is in an anonymous namespace, and the Debug assert fires before the fallback runs. The brief allowed this.

### M1 — the ruled all-rows gross has no committed producer: ADDRESSED

**Where:** `studies/nav_summ.py:173-176`.
- It emits `mean_gross_leverage_all_rows` and a signed `mean_net_leverage_all_rows` over every row of the analysed daily CSV, plus `csv_rows`.
- `leverage_gate_basis` (`:63`) labels all-rows as the R6' gate statistic.
- The text line at `:468` carries the label `[R6' mechanics gate]`.
- The existing return-row keys are untouched.

**Tests:** `test_nav_summ.py:155` (by-hand assertions) and `:365`.

### M2 — cost per GMV-turnover mixes bases (T31 Minor 3): ADDRESSED

**Where:** `nav_summ.py:140-151, 164-166, 182`.
- The numerator is now Σ `trade_cost_dollars_s / pretrade_nav_s` over the same `keep` mask that selects τ: executed sessions with pretrade gross > 0 and the deployment excluded.

**The implementer's booking claim checks out in C++:**
- `mark_session` sets `trade_cost_return_t = pending_cost_{t-1} / nav_pre_{t-1}` (`strategy_nav_replay.cpp:574, 582`).
- `pretrade_nav_t` is written after MARK t (`:855`).
- `execute_orders` sets `pending_cost = cost` (`:631`).
- So each numerator term is session s's own cost in NAV-return units: the same session basis as the τ denominator, and the same units T38 used (cost per NAV ÷ turnover per GMV).
- Dropping only row `dep` from Σ `trade_cost_return` would have been a no-op, as the report says.

**Other checks:**
- The writer uses `setprecision(17)`.
- `pretrade_nav` is present in the v3 CSV headers I checked (`mega-nav-v3-VAL`, `-netcost-b0`), so the new column read does not break older dirs.
- The key is kept and its value replaced, as the brief allows.

**Test:** `test_nav_summ.py:334`.

### M5 — V[SR_n] depends silently on the listing: ADDRESSED (warn, not refuse, per the brief's ruling)

**Where:**
- `listing_warnings` (`nav_summ.py:401-412`) counts defined SRs with the same predicate that `dsr_rows` uses (`sr_daily is not None`, `:333`).
- It flags any pair of listed dirs with identical daily net series via `same_series` (`:392-398`).
- Warnings print to stderr (`:524-525`) and are also recorded in the JSON.
- The exit code and every number are unchanged.

**Tests:** `:386` and `:395`.

**Expected effect on the T40 re-run:** read-only check of `build-equity/mega-nav-v5-t40-summ-n13.json`: 13 rows, 13 distinct `sr_daily` values, and `n` = 13. So the root's re-run should print no warning.

### M7 — thin provenance on the gate output: ADDRESSED

**Where:** `nav_summ.py:415-429, 509, 530-532`.
- Every JSON row carries `nav_summ_run` = {`argv`, `script`, `script_sha256`, `git_head`, `warnings`}.
- `git_head` is best effort. It returns null on OSError, SubprocessError (which includes TimeoutExpired), a non-zero exit, or output that is not a SHA.

**Test:** `:423`.

## New Breakage in the Fix Diff

**Critical/Important:** none.

**Check (c) — no other field changes:**
- Old JSON keys are unchanged. `construction_stats` only adds keys, and the JSON is `sort_keys=True`.
- `turnover_gmv` returns the same array.
- `analyse`'s new tuple return has exactly one caller (`:519`). I grepped all `studies/*.py` and `*.sh`, and the scripts only invoke `nav_summ` as a CLI.
- `argv` normalization is equivalent to the old `parse_args(None)`.
- The only stdout change is the added M1 line.
- The 18 tests reported as passing (12 old + 6 new) include `test_existing_fields_and_text_unchanged_against_the_pre_t41_blob` (`:450`). That test diffs every old key and line against the `faf5943f` blob, so it ran rather than skipped.

**Minor (deferred):**
- **Where:** `nav_summ.py:405-407`.
- **What:** the count warning fires on every documented single-dir run (Lo-variance mode, default N = 10). `v5_train.sh:157` and `v51_train.sh:308,310` run `nav_summ` on one cell per run with `2>&1`, so each per-cell log now gains a warning line. In that mode the text "V[SR_n] … comes from the listed dirs" is inaccurate: the variance is Lo's single-cell sampling variance.
- **Impact:** cosmetic noise only. Numbers and exit code are unaffected, and it follows the brief's literal rule.
- **Fix:** suppress the warning, or reword it, when there is fewer than 2 defined SRs.

## Out-of-Scope Observations

- **Where:** `atx-impl/src/strategy_nav_replay.cpp:725-732` (`per_name_rates`). This code is untouched by the fix diff.
- **What:** it also reads `cache.adv`/`cache.sigma` with no release fallback. A NaN entry would silently map to `rate_min` through `per_name_rate_v1`.
- **Why it holds today:** `fill_liquidity`'s `decision && member` arm (`:424`) forms every member, and validation refuses absent members (`:303`).
- **Status:** non-blocking; ledger it for the whole-branch review or T32.

## Verdict

**Fix round:** all findings are addressed (I1, M1, M2, M5, M7) and the fix introduces no new Critical or Important breakage. There are 2 deferred minors: 1 new Minor from the fix diff and 1 out-of-scope observation.
