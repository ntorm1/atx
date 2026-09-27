# Task T41-fix report: final fix wave after the whole-branch review

**Implementer:** Claude Opus 5.5, 2026-09-27. Worktree `C:/atx-wt/pool-3`, branch `feat/mega-alpha-v5-t41fix-20260927`
from `faf5943f`. I did not build anything, run on real data or spawn subagents. The only commands were pure-Python
pytest on synthetic fixtures, plus the read-only desk checks on existing `build-equity/` CSVs described under M1 and M2.

**Commits:**
- `927343ac` fix(nav): release fallback when the per-name liquidity cache misses (T41 I1)
- `9a9bb5d3` fix(studies): nav_summ gate numbers tool-produced (T41 M1 M2 M5 M7)
- this report (separate commit)

**Untouched, per the brief:** `fit_composition_weights.py`, `generate_fund_ic_v*.py`, the library and recipe JSON,
`v5_train.sh`, `v51_train.sh`.

---

## I1 (Important, C++): release fallback in `execute_orders`

**What changed:** `atx-impl/src/strategy_nav_replay.cpp:602-608`.
- The debug assert stays (`:605`).
- The row now reads the cache only when it is on **and** the entry is formed (`:606`):
  `cache.on() && !std::isnan(cache.adv[i]) ? liquidity_row(c, WindowLiquidity{cache.adv[i], cache.sigma[i]}) : liquidity_row(c, t, i)`.
- A comment gives the reason (house rule `agent.md:84`).

**Signatures checked:**
- `liquidity_row(const Ctx&, const WindowLiquidity&)` is at `:391` and `liquidity_row(const Ctx&, usize, usize)` at `:399`.
- `WindowLiquidity{adv, sigma}` is at `:360`.
- `LiquidityCache::on()` is at `:411`.
- `<cmath>` and `assert` were already used on this line.
- `a && b ? x : y` needs no parentheses: `&&` binds tighter than `?:`. The same form already compiles under `/WX` at
  `:909` and `strategy_runner.cpp:281`, and clang's `-Wparentheses` conditional-precedence check does not fire on `&&`.
- There is only one call site, and the signature did not change.

**Why output bytes cannot change:**
- **Cache off (fixed rate):** unchanged path.
- **Cache on and formed:** the same cached values as before.
- **Cache on and NaN:** this branch is new. It is unreachable while the invariant holds, and
  `validate_nav_input` (`:300-307`) makes every present row finite. So a formed ADV is never NaN, and NaN means
  "not formed" exactly.
- On the fallback branch, `liquidity_row(c, t, i)` is `liquidity_row(c, window_liquidity(c, t, i))`. That is the same
  arithmetic `fill_liquidity` caches at the same `t` (`:429-430`), so any fallback is bit-identical to a formed entry.

**GoogleTest: not added.**
- `execute_orders`, `LiquidityCache` and `fill_liquidity` are in the anonymous namespace (`:31`–`:1532`). The public
  replay always forms the cache before EXECUTE (`:842-843` before `:855`), so the fallback cannot be reached without a
  production seam.
- The test gate is the Debug `dev` preset, where the kept assert fires before the fallback runs.
- The byte-identity of the cached path stays pinned by the existing `NavV5*` fixtures (e.g.
  `PerNameRate_FixedEqualsTradeFraction`) and the `*BitIdentical*` suites the root runs.

**Not changed:** `per_name_rates` (`:722-730`) also reads the cache. Its members are covered structurally: validation
refuses `member` on absent rows, and `fill_liquidity` forms every present decision member. It does not depend on MARK
and was outside I1's scope.

## M1: all-rows gross and net leverage (the ruled D1/R6' definition)

**What changed:** `studies/nav_summ.py`.
- `construction_stats` (`:154`) adds these fields (`:173-176`):
  - `mean_gross_leverage_all_rows` and `mean_net_leverage_all_rows`: the mean of `gross_leverage` / `net_leverage`
    over every row of the analysed (primary or `--scenario`) daily CSV.
  - `csv_rows`
  - `leverage_gate_basis`, the constant `LEVERAGE_GATE_BASIS` (`:63`). It says the R6' mechanics gate reads the
    all-rows keys, and that `mean_gross_leverage` / `mean_net_leverage` are the return-row means kept for continuity.
- The text output gets a new line after the construction line (`:468`):
  `   leverage over all N CSV rows [R6' mechanics gate]: gross_lev_all_rows X net_lev_all_rows ±Y (construction gross_lev/net_lev: previous close over return rows)`.
- The docstring section is "leverage gate".

**Desk check (read-only, on existing output):** on `mega-nav-v5-ew-t.05-d.1-fixed-L1.279` S2, the all-rows means are
1.0019819 gross and +0.0148069 net over 756 rows. These match the review's 1.00198 and +.0148.

**Tests:**
- `test_all_rows_leverage_in_text_and_json`
- The by-hand assertions added to `test_construction_stats_and_netting_ratio_by_hand`

## M2: cost per unit GMV turnover on one basis

**What changed:** `construction_stats` now forms the numerator over the same rows as the denominator (`:161-166`):
`sum over the tau_t sessions s (executed, pre-trade gross > 0, deployment excluded) of trade_cost_dollars_s / pretrade_nav_s`,
divided by `sum tau_t`.
- A new helper, `turnover_rows` (`:140`), returns that mask. `turnover_gmv` still returns the same array.
- The key name `cost_per_gmv_turnover` is kept and its value replaced: no old copy is needed for continuity, since T40
  never cited it. The docstring states the new definition.

**Decision, recorded:** "drop the deployment row from Σ `trade_cost_return`" would have been a no-op. MARK books the
fills of session s into row s+1 (`strategy_nav_replay.cpp:556-559`, `:582`), and the deployment row's own
`trade_cost_return` is 0 (on real data: row 1, session 400, has 0, and row 2 carries the $48,154 deployment cost). So
the numerator reads the cost of the τ sessions themselves.

**Desk check (read-only):** `trade_cost_dollars[s] / pretrade_nav[s] == trade_cost_return[s+1]` holds **bit-for-bit
on all 60,320 executed rows** of every `build-equity/mega-nav-v5*` daily CSV (all scenarios). So this is exactly
"Σ `trade_cost_return` over the τ sessions' booking rows". `pretrade_nav` is also present in the v3/v4 CSVs.

**Unchanged:** the units are still cost per unit NAV against turnover per unit GMV, as T38 specified (T31 Minor 3's
second remark). The brief did not ask me to change them, and `cost_bps_traded` stays alongside as the pure per-dollar
figure.

**Tests:**
- `test_cost_per_gmv_turnover_excludes_the_deployment_cost_like_tau`. It uses C++-shaped rows (the last row not
  executed, and a zero-gross executed row), and pins four things:
  - the new value, computed longhand
  - equality with the lagged `trade_cost_return` form
  - that the pre-fix value minus the new value equals exactly the deployment cost plus the zero-gross cost
  - that `cost_bps_traded` is unchanged
- The existing by-hand test's cost assertion is updated to the new definition.

## M5: listing warnings on stderr (never refusals)

**What changed:**
- `listing_warnings` (`:401`) warns when the number of listed dirs with a defined per-session SR is not `--dsr-n`.
- It also warns for each pair of listed dirs whose daily net series (session_ns → net over the return rows) are
  identical. `same_series` (`:392`) requires the same sessions in the same order, and treats NaN as equal to NaN.
- Each warning prints as `nav_summ: WARNING ...` on stderr (`:525`) and is also recorded in the JSON (M7). Stdout,
  the exit code and every number are unchanged.
- `analyse` now returns `(row, nets)` (`:433`) so main can compare the series. Its only caller is main.

**Tests:**
- `test_listing_warnings_go_to_stderr_and_never_refuse`. It covers four cases:
  - a byte-identical check cell (one warning, all three DSRs still computed, warning in the JSON)
  - the default N = 10 with 2 dirs
  - a constant series not counted as defined
  - a clean listing, which gives empty stderr
- `test_same_series`

**For the root:** the T40 run was 13 cells with `--dsr-n 13`, all distinct as far as the review knows. So a re-run
should print no warning. `v5_train.sh` and `v51_train.sh` pipe `2>&1`, so any warning would show in their logs.

## M7: provenance in `--json`

**What changed:** every JSON row carries `nav_summ_run` (`:532`), built by `run_provenance` (`:426`). It holds:
- `argv`: the argument list actually parsed
- `script`: the resolved path of `nav_summ.py`
- `script_sha256`
- `git_head`: from `git_head` (`:415`), which runs `git rev-parse HEAD` in the script's directory. It gives null on
  OSError, SubprocessError, a non-zero exit, or output that is not a 40- or 64-hex SHA.
- `warnings`

**Decision, recorded:** I put the block on every row rather than wrapping the output as `{run, results}`. The top level
stays the per-dir list, so existing consumers, and the root's field-by-field diff against
`mega-nav-v5-t40-summ-n13.json`, index rows exactly as before. The cost is 13 identical copies of the block.

**Tests:** `test_json_records_argv_script_sha_and_git_head` checks:
- the real run: argv, script path, SHA-256 of the file, and git_head null or a SHA
- git_head with git missing, a non-repository, non-SHA output and a valid SHA, all via monkeypatch
- that main still exits 0 with a null head

## Byte stability of every other field (the root's diff)

**New test:** `test_existing_fields_and_text_unchanged_against_the_pre_t41_blob`. It loads the pre-fix `nav_summ.py`
from git blob `5c414e38…` (the file at `faf5943f`). It runs both versions on the same synthetic 3-cell world, with
`--weights`, `--reference`, `--dsr-n 3` and consistent lagged costs, and checks:
- **JSON:** the new keys are exactly {`mean_gross_leverage_all_rows`, `mean_net_leverage_all_rows`, `csv_rows`,
  `leverage_gate_basis`, `nav_summ_run`}. Every old key other than `cost_per_gmv_turnover` serializes identically, and
  the new cost is lower because the 2e5 $ deployment is out.
- **Text:** stdout is line-for-line identical once the three new "leverage over all" lines are removed and the
  cost/GMV-tau token is masked.

**Line lengths:** every line over 120 columns in either file was already there before this change.

**Expected diff when the root re-runs on the 13 T40 cells:**
- 5 added keys per row
- the changed `cost_per_gmv_turnover`
- one added text line per dir
- the cost/GMV-tau value on the construction line, which may round the same at 5 decimals

## Verification

The two suites below ran together after the final commit: **87 passed in 15.53 s** (18 + 69).

Command: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider <file>`, per file:
- `studies/test_nav_summ.py` (sprint dir): **18 passed**. That is the 12 pre-existing tests (one assertion updated
  under M2, M1 assertions added) plus 6 new ones.
- `atx-impl/tools/test_fit_composition_weights.py`: **69 passed**. This file is unchanged and was run as regression.

## Root after cherry-pick

**Registration:** there are no new source or test files for CMake. The C++ change is inside
`atx-impl/src/strategy_nav_replay.cpp`, which is in `atx-impl-core` and linked by both targets below.

**Steps:**
1. Build tag `v5-2`: `atx-equity-strategy-targets,atx-impl-strategy-target-tests`.
2. Run `atx-impl-strategy-target-tests --gtest_filter=TargetReplayV5.*:NavV5*:*BitIdentical*`, then the full exe.
3. Repeat one D2 check.
4. Run the pytest command above on both files.
5. Re-run nav_summ over the 13 T40 cells and diff the output against `mega-nav-v5-t40-summ-n13.json`, as described
   above.

## Could not do

- **No build:** I could not compile the C++ change and desk-checked it instead.
- **No GoogleTest for the I1 fallback:** the reason is given under I1.
- **No real-data run of nav_summ:** the root owns the 13-cell re-run.
- **Pre-existing, not fixed:** the mypy note that `expected_max_sr` receives `float | None` (`nav_summ.py`, in
  `dsr_rows`) was already there. Fixing it was out of scope and would not change any output.
