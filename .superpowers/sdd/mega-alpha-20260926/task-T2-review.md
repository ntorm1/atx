# Task T2 review: $1bn NAV replay of the pinned saved blend, including fix round 1

Reviewed: `review-T2.diff` (c4ba9c80 code, 97e6b392 CMake, f6df5fe8 fixture fix). Line numbers refer to the current source in `C:/atx-wt/pool-2`. `strategy_nav_replay.cpp` is cited as `nav.cpp`, `strategy_nav_replay_test.cpp` as `test`, and `strategy_target_replay.cpp` as `str.cpp`. Read in two passes: production code, then tests. No hunk was cut off.

## Spec Compliance

- ✅ **Spec compliant.** Every item in the brief and the design has a matching hunk:
  - **Scenarios.** S1, S2 (primary) and S3 are fixed in code and match the ruling: `nav.cpp:817-831`, asserted at `test:665-694`. Borrow is 300 bps, K=5 for the primary and K=1 with −0.55/+0.30 for the adverse case. The adverse constants match `book::assumed_missing_price_return` (asserted at `test:666-672`).
  - **Pure core.** `replay_nav` (`nav.cpp:833`) and `summarize_nav` (`:852`) do no I/O. All I/O is in `run_nav_replay` (`:922`) and `dispatch_nav_replay` (`:988`).
  - **Extension point.** `form_desired_target` (`nav.cpp:389-393`) is the only NAV-path call of `detail::desired_target`. It is called only from `plan_decision` (`:403`), before the partial/budget planning. The recipe records `desired_target_postprocess: none`. No neutralization was implemented, as required.
  - **Rules.** Only baseline-v1 and monthly-budget-v2 exist; no new levers (CLI `nav.cpp:1030-1034`).
  - **Summary turnover fields.** Each scenario gets `turnover_definition` plus `mean_monthly_one_way_turnover` and `..._ex_deployment_month` (`nav.cpp:638-664`).
  - **Target replay untouched.**
    - `str.cpp` changes are limited to the append-only `detail::` wrappers (`str.cpp:579-623`), the optional volume load (`:364`, `:394-401`), the 36 B/cell budget that applies only when volume is requested (`:331`), and the sanctioned root addendum `admitted_signal_semantics` (`:291-302`).
    - `replay_targets` arithmetic is unchanged, and `strategy_target_replay_test.cpp` is not in the diff.
  - **CLI.** The `nav` verb is wired (`tools/equity_strategy_targets.cpp`). `--role` is required, and `--one-way-bps`/`--annual-borrow-bps` exit with code 2 (`test:656-660`).
  - **CMake.** The source is added to `atx-impl-core` and to both Debug `/O2` lists, and the test TU is added to `atx-impl-strategy-target-tests` (97e6b392).
  - **Output sequence.** Every scenario is computed before the output directory exists. The directory is exclusive; `recipe.json`, then the CSVs, then `summary.json` are written in that order (`nav.cpp:941-981`).
- **Global constraints**
  - **Accounting identities hold and are hard-checked.**
    - `r = gross − cost − borrow` is checked at `nav.cpp:341-345`. It is exact in real arithmetic, because NAVpre_t = NAVpost_{t−1} + P + W − B (`:334`) and NAVpost_{t−1} = NAVpre_{t−1} − TC_{t−1} (`:381`).
    - `cash + Σh = NAV` is checked every session at 1e-9 relative (`:439-442`).
    - A write-off moves cash by h(1+η) and zeroes h, so ΔNAV = hη = W (`:283-284`).
  - **No look-ahead.**
    - `run_book` runs MARK(t) → EXECUTE(t) → DECIDE(t) (`nav.cpp:469-477`).
    - A decision at d uses signal and membership from row d and holdings marked at close d. Its fill happens at d+1.
    - Liquidity reads only rows [t−w−1, t) (`:210-232`).
    - Absence is detected only from `present[t]`, and the write-off trigger counts only past absences (`:277-280`).
    - Fixtures 4 and 5 prove bit-identical prefixes after rewriting the future (`test:411-484`).
  - **Forced exits are charged and counted.** A zero-dollar order is placed for every changed name (`nav.cpp:409-412`). It fills through the cost model, and |fill| goes into `traded` (`:372`). Fix round 1 now asserts that a held forced exit is filled and counted (`test:314-328`), and fixture 3 covers a blocked absent exit that fills at the reprint (`test:392-397`).
  - **Stale and write-off events.** Their P&L is charged to NAV (`W`, haircut) and they are counted (events CSV plus the gap 1/2-4/5+, written-off, reappeared and unresolved buckets with dollar amounts). Excluding write-offs from turnover matches handoff §2a (`tau = Σ|fill$| / pre-trade GMV`; a write-off is not a fill).
  - **Return statistics.** Sharpe uses sample SD × √252 (`nav.cpp:494-496`). HAC is `mean_inference(series, BartlettV1, 5, true, true)` (`:899-900`); I checked this against `hac.hpp:171`. Returns count as excess returns under the declared 0%-cash/no-rebate convention (limitations text).
  - **Accepted deviations.** The 756 rows carry `return_observation` (`:468`), p95 is a histogram upper bound, and ex-deployment flags were added. All three are implemented as described.
- ⚠️ **Cannot verify from this diff (cross-task, for the controller)**
  - **T4: pre-trade GMV is not exposed.**
    - T2 exposes the tau numerator: `traded_dollars` (Σ|fill|, forced exits included). It also exposes `deployment_index` for the exclusion.
    - It does not expose the denominator. The `long_dollars`/`short_dollars` values in `NavReplayDay` are post-trade (`close_day`, `nav.cpp:424-437`).
    - T4 must capture Σ|held| in `run_book` between `mark_session` and `execute_orders` (`nav.cpp:469-472`). Write-offs are already zeroed at that point.
    - Until T4 lands, handoff item 2's "daily turnover mean/p95 from the daily CSV" can only be NAV-denominated (`one_way_turnover`), or an approximation that uses the previous row's post-trade GMV and ignores drift.
  - **T10: financing.** `swap-fin-v1` is absent (flat 300 bps short borrow, 0% cash, no long financing). This is T10's scope; T2 matches its brief ("Borrow stays 300 bps").
  - **Real-data behaviour is unverified:**
    - whether the identity tolerances hold at 5.6k names × 754 sessions;
    - how close the events come to the 262,144 cap;
    - runtime: `liquidity_row` is O(w) per active order per session, and at cadence 1 that is roughly 5.6k × 63 × 754 × 3 scenarios.

    The first TRAIN run (handoff item 2) is the check.

### Named-risk checks run (one each)
- **Does `plan_decision` diverge from `replay_targets` on month reset, the pre-decision `spent` value, the `spent += turnover` accumulation, or `budget_excess`?** Compared with `str.cpp:196-209`: identical (`nav.cpp:400-422`).
- **Is the complete-fill equality `filled == requested` (`nav.cpp:367`) inexact?** `replay_cost.cpp:109` returns `trade_dollars` verbatim when uncapped, and `FlatBpsCost` does the same (`:63`). The equality is exact.
- **Is the linear/impact split wrong?** `replay_cost.cpp:93` computes the linear rate as (half_spread + commission)·1e-4. `Ctx.linear_rate` (`nav.cpp:843`) uses the same rate.
- **Is the borrow day count truncated when session keys are not at midnight?** `validate_input` enforces UTC-midnight keys (`str.cpp:91-92`), so the integer division at `nav.cpp:323` is exact.
- **Does the recipe-hash convention differ from the target replay's?** No: `str.cpp:505` uses the same `sha256_hex(method.dump())`.
- **Must write-offs count in tau?** Handoff §2a (`parent-handoff-2.md:81-82,111-112`) defines tau as Σ|fill$| / pre-trade GMV with forced exits counted. A write-off is not a fill, so T2 is consistent.
- **100-column limit?** An awk scan of the four new files found 0 lines over 100.

## Strengths
- **The target rules are reused, not copied.** The `detail::` seam forwards to the file-local functions via qualified `::atx::impl::strategy::` calls (`str.cpp:611-622`). The NAV plan is then bit-identical to `replay_targets` under zero drift and cost, and fixture 1 proves it field by field (`test:292-307`).
- **Both identities are enforced at runtime.** They are hard refusals rather than log lines, and the maximum errors are published (`accounting_checks`). This is the right posture for a Sharpe-gated deliverable.
- **Causality is proven, not asserted.** The future-rewrite fixtures compare bit patterns (`expect_same_day`, `test:~100-114`) under both rules and both stale policies, including events before the cut.
- **The fix round was diagnosed correctly.** The failing assertion was on the unchanged target replay: under v2, name 1's d0 target was exactly 0 (middle tie group) and the budget was fully spent at d0. The fixture now uses a name that is held under both rules. It adds executed-side assertions (fill count, `one_way_turnover == planned_forced` bit-equal, held_names −1), which were the missing evidence that forced exits reach turnover.
- **Diagnostics are thorough and every numeric decision is explicit and recorded:** unrationed full-request cost, participation histogram, linear/impact split, a missing-price histogram with dollars, guard sensitivity, and a planned-vs-actual reconciliation with a monthly reconciliation error.

## Issues

#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
1. **No fixture covers the primary scenario's K=5 write-off.**
   - Why it matters: `carry_absent` (`nav.cpp:277-290`) is exercised only with K=1 (S3). Under S2 (`test:446-458`) the fixture has a 3-session gap and a 4-session unresolved run; it never reaches 5.
   - The boundary is correct by inspection (the write-off fires on the 5th consecutive absent mark), but S2 is the headline scenario.
   - Fix: add a 5-absence and a 4-absence name under `scenarios[1]`. Assert:
     - the write-off happens on the 5th absent mark;
     - `writeoff_dollars == 0`;
     - cash is credited h at the stale mark;
     - the 4-gap resolves as `gap_run_2_4`.
2. **The guard branch is never exercised.**
   - Why it matters: `realize` (`nav.cpp:308-314`) emits `Guarded` events and accumulates `guard_sensitivity`. Every fixture sets raw == adjusted, and the random walk moves about ±2%, so no fixture triggers the guard. The guard is reporting-only (it does not change NAV), but it feeds a published summary field.
   - Fix: add a split-like interval where raw and adjusted diverge by more than 0.10 log. Assert the event, `guarded_intervals`, and `guard_sensitivity = h(r_raw − r_adj)`.
3. **The budget-refusal test never reaches the loader's volume budget.**
   - Why it matters: the refusal in fixture 8 uses `max_working_bytes = 1` (`test:652`). That hits only the NAV reserve check in `run_nav_replay` (`nav.cpp:934-935`), not the loader's with-volume 36 B/cell admission (`str.cpp:331`). So the 28 → 36 change is unpinned.
   - Fix: use a budget of reserve + an admission that passes at 28 B/cell but fails at 36.
4. **Under v2, a blocked stale exit is charged to the monthly budget on every decision.**
   - What happens: `update_weights` counts the |current| of every held absent nonmember as `forced` each day (`str.cpp:131,136`), and `plan_decision` adds it to `b.spent` each day (`nav.cpp:408`). While the name stays absent, discretionary v2 trading is starved.
   - The implementer disclosed the double count in planned turnover but not its effect on the budget.
   - Why Minor: v2 is retired and will not ship (handoff item 2).
   - Fix: note it in `limitations`, or exclude exits already pending on absent names from `forced`.
5. **The retired 30%/month target is still published as pass/fail flags.**
   - Where: `meets_turnover_target_*`, `months_le_0.30`, and `monthly_turnover_target` in the recipe (`nav.cpp:513-540`, `:621`). The header calls it an "Owner target" (`strategy_nav_replay.hpp`, `nav_monthly_turnover_target`), and `limitations` does not label these fields as legacy.
   - This conflicts with the lane-contract ruling that the target is "never a flag/constraint". T4 requirement 6 already relabels them as legacy, reporting-only; ensure that lands before any run is reported.
6. **`run_nav_replay` maps every exception to InvalidArgument.**
   - Where: the catch-all at `nav.cpp:983-984` also catches `std::bad_alloc` from JSON/CSV construction and `filesystem_error`.
   - Why it matters: `replay_nav` and `summarize_nav` map allocation failure to OutOfRange, so the error code a caller sees depends on where the allocation failed.
   - Fix: add a `bad_alloc` → OutOfRange arm before the catch-all.
7. **`summarize_nav` is long, and its exposure means mix bases.**
   - Length: about 68 lines (`nav.cpp:852-920`), over the house guideline of about 60.
   - Mixed bases: `mean_stale_names` uses the current row, while `mean_gross_leverage` and `mean_held_names` use the previous row (`:885-887`, "exposure that earned this row's return").
   - Fix: use one basis for all three, and extract the month and year accumulation into a helper.
8. **Fixture 5 skips S1.** It loops `s = 1..2` (`test:469`). S1 differs only in its cost model and is uncapped, but including it costs nothing and pins the flat path's causality.

## Assessment

**Task quality:** Approved

**Reasoning:** The NAV engine implements the approved design: causal MARK → EXECUTE → DECIDE timing, exact and hard-checked cash/NAV and return identities, charged and counted forced exits, stale/write-off handling as ruled, and a single extension point. The fix round correctly identified a fixture defect and added executed-side evidence. What remains is test coverage of the primary K=5 write-off, the guard, and the volume budget, plus cross-task hand-offs: pre-trade GMV for T4 and `swap-fin-v1` for T10.
