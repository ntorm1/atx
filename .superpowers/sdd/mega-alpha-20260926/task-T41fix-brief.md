### Task T41-fix: final fix wave after the whole-branch review (one dispatch, all findings)

**Pool:** pool-3. First step: in C:/atx-wt/pool-3 confirm `git status` is clean, then
`git checkout -b feat/mega-alpha-v5-t41fix-20260927 faf5943f` (faf5943f = pool-2 HEAD with the T41 review committed).
**Model:** Opus 5.5 · **Source of findings:** `task-T41-review.md` (same directory) — read sections I1, M1, M2, M5, M7, and
T31 review Minor 3 (`task-T31-review.md`) which M2 cites.

**Findings to fix (controller ruling in progress.md section "T41 whole-branch review"):**
1. **I1 (Important, C++)** `atx-impl/src/strategy_nav_replay.cpp` ~:602 (`execute_orders`): keep the debug assert and add the
   release fallback so a NaN cached ADV recomputes the row instead of silently blocking the fill (review gives the code shape:
   `cache.on() && !std::isnan(cache.adv[i]) ? liquidity_row(c, WindowLiquidity{cache.adv[i], cache.sigma[i]}) : liquidity_row(c, t, i)`
   — verify the real signatures). Output bytes must not change. Add a focused GoogleTest only if it can exercise the fallback
   without production seams; otherwise say why not.
2. **M1 (nav_summ.py)** emit `mean_gross_leverage_all_rows` and `mean_net_leverage_all_rows` (mean over every row of the
   primary daily CSV's `gross_leverage` / `net_leverage` columns — the ruled D1/R6' definition) next to the existing return-row
   means, in both the text output and `--json`; label which one the R6' mechanics gate uses (all rows).
3. **M2 (nav_summ.py)** cost per unit GMV turnover: exclude the deployment row from the numerator exactly as the denominator
   excludes it (T31 Minor 3); keep the old value under a clearly renamed key only if needed for continuity, otherwise replace.
   Signed mean net is covered by M1.
4. **M5 (nav_summ.py)** warn on stderr (do NOT refuse) when the number of directories with a defined SR differs from `--dsr-n`,
   and when two listed directories have identical daily net series.
5. **M7 (nav_summ.py)** `--json` output records argv, sha256 of nav_summ.py itself, and `git rev-parse HEAD` (best effort; null
   if git is unavailable).

**Constraints:** do not edit `fit_composition_weights.py`, `generate_fund_ic_v*.py`, library/recipe JSON (SHA-pinned), or
v5_train.sh / v51_train.sh. Every existing nav_summ JSON field and text value other than the M2 cost metric must stay
byte-identical for the same inputs (the root diffs them against `build-equity/mega-nav-v5-t40-summ-n13.json`). Extend
`studies/test_nav_summ.py` for M1/M2/M5/M7 with synthetic fixtures; run it plus
`atx-impl/tools/test_fit_composition_weights.py` (pure Python, no build, no real data). Never build; never run real data.

**Root after the fix:** cherry-pick; build tag v5-2 (atx-equity-strategy-targets, atx-impl-strategy-target-tests); filter
`TargetReplayV5.*:NavV5*:*BitIdentical*` + full exe; one D2 check; pytest; re-run nav_summ over the 13 T40 cells.
