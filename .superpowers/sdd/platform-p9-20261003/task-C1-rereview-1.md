# Lane C1 re-review 1 (scoped)

## Verdict
APPROVE

## Reviewed SHA
10c35df390ccd3d1b2a0c83388521fe962904fc0 (FIX_BASE f6cd387d). It adds a275088b (code and gtest, C1-SPO) and
10c35df3 (report fixes for M1 and M2). Worktree C:/atx-wt/pool-15. HEAD matched, and the tree was clean before and
after the review.

## Evidence
1. `cd C:/atx-wt/pool-15 && PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q
   -p no:cacheprovider atx-impl/tools/test_nav_summ.py atx-impl/tools/test_nav_summ_v8.py` printed
   `29 passed, 1 skipped in 2.91s` (exit_code=0). Afterwards `git status --short` showed 0 lines.
2. I extracted the C string literals with Python (exit_code=0) and compared them:
   - the flag-absent `capacity_spo_v3_declaration` at 10c35df3 equals d7c1c520's constant byte for byte (888
     chars);
   - the gtest's pinned `base_sentence` equals that base constant;
   - the new `capacity_spo_v3_adv_hold_declaration` equals f6cd387d's reworded text.

   I also diffed the `declarations()` body against base. The only changes are the `adv_hold` parameter and the
   `capacity_spo_v3_rule(adv_hold)` selector. Every other declaration constant is unchanged.
3. The fix-round code diff adds 98 lines, with 0 over 100 columns and 0 tabs.
4. `git diff --stat f6cd387d 10c35df3` touches only these files: strategy_nav_v7.{cpp,hpp},
   strategy_spo_v3_test.cpp and the report.

## Findings from review 1
- M1 (report:111, identity run stated as Release) | **addressed**. Report §4 now has two runs:
  - **Run (a)** compares the new Debug build (`-Preset equity-dev`) against the old X-5 output. CMakePresets
    confirms that equity-dev is `build-equity` and inherits dev, which matches the receipt's
    `build-equity\bin` exe and source_sha d7c1c520.
  - **Run (b)** compares Release (`equity-rel`) against the stage-2 Debug output. Expected: byte-identical after
    dropping `summary.producer`. Any other difference falls under the plan's rule (NAV stays Debug, G-P3's NAV
    half unmet).

  The report also says that `ReplayCostSqrt.*` covers only the sqrt path. It names `guarded_move`'s `std::log`
  (which runs in every MARK, S1 included), the log/log10/pow(10) in participation p95, and the cagr `pow`.
- M2 (report:134, wholesale "may move" list) | **addressed**. The list is now a two-stage identity.
  - **Stage 1** (dd925b7f, sqrt only, against the old X-5) names every file that may differ: the CSVs of labels
    [1]-[4] and of the five capacity books, `capacity_curve.csv` and `v7_transfer_coefficient.csv`.
  - It names the JSON paths that may differ. In summary.json: `scenarios[1..4]`,
    `warm_start.score_begin_gross_leverage[<S2 labels>]` (verified keyed by label, score_begin_gross) and
    `v7.books[<S2 labels>]`. In capacity/summary.json: `scenarios[0..4]`, the warm-start map and `v7.books`. In
    v7_extras.json: `capacity[0..4]` and the two `files` SHAs.
  - The S1 CSVs, both recipes and `locate_in_aim` (taken from S1 and the shared construction) must be identical.
  - **Stage 2** (a275088b against the stage-1 output): every CSV, both recipes, capacity/summary.json,
    capacity_curve.csv and v7_transfer_coefficient.csv must be identical. summary.json may differ only at
    `v7.extras`, `v7.files` and `producer`; v7_extras.json only at `files["capacity/summary.json"]`.
  - I checked this list against the code (S1 arithmetic is per book, construction does not depend on the
    scenario without the cap, fill_liquidity stores window values only, and the old capacity summary read the
    accumulated main records). It is complete, and nothing is excused wholesale.
- Minors from review 1 (validate_nav_config store and params; book_groups wall time; aim_leverage `required`):
  **open, deferred** by the coordinator. They are listed in the report's Fix round 1 section, and none blocks.
  The minor on report:147 (exe identity and argv) is **closed** by PM ruling C1-PROD and is not re-raised.
- C1-SPO (strategy_nav_v7.cpp:208, PM-raised) | **addressed**.
  - Without `--adv-hold-q` the sentence is base's bytes (evidence 2).
  - **Recipe:** chosen by the recipe's own `adv_hold_rule` key (strategy_nav_v7.cpp:965). The target replay
    writes that key iff `adv_hold_on` is true, that is `adv_hold_q > 0` (strategy_target_replay.hpp:145).
  - **Decide path:** it recomputes the recipe through nav_recipe and extend_recipe, so it reads the same key
    without needing a replay.
  - **Summary, capacity summary, v7_extras.json (void path included) and holdings manifest:** they read
    `State::adv_hold`, which `v7::configure` sets to `adv_hold_q > 0`. configure runs for every config in
    replay_books on the calling thread, before any of these is written. Holdings are written in
    publish_holdings, after the replay.
  - All of these key off the same predicate on the same target. Grid variants share `adv_hold_q` (same_shared).
  - **The gtest pins the contract.** `SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes` pins the base
    literal in the recipe and the holdings manifest, the adv-hold sentence plus the recipe's
    "every capacity book" rule with the flag, and the return to base after configure with q = 0. Summary and
    extras are not asserted directly; they use the same `capacity_spo_v3_rule(s.adv_hold)` selector and state
    that the holdings check exercises.

## New findings
None.

## Checked
- [x] `/W4 /permissive- /WX` scan of the new lines found no definite error:
  - `capacity_spo_v3_rule` is a `[[nodiscard]] constexpr` anonymous-namespace function, used twice.
  - `declarations` has 4 call sites, all updated to the new signature.
  - `configure` writes through the non-const `active_state`.
  - Test helpers `Json`, `nav_config`, `clean_model` and `sp::v3_params` exist in strategy_spo_v3_test.cpp.
  - Nothing is unused, and no line exceeds 100 columns.
- [x] Nothing else moved:
  - Non-spo runs: declarations are unchanged, because the selector is reached only when a `capacity_engine`
    exists.
  - adv-hold runs keep f6cd387d's sentence.
  - No other file changed.
- [x] Blindness: this review opened no output and no receipt; it read only the code, the report and CMakePresets.
