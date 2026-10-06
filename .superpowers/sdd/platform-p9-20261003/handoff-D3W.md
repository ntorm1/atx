# handoff-D3W: walk-forward fit, v3 weights runner, fitter post-step (OWNER STOP)

Lane D3W, worktree `C:/atx-wt/pool-23`, branch `feat/p9-d3w-20261005`, created at `899b5811`.
Stopped at the owner's order. Head `2209821e` (WIP). Root merged: `a15ad887` (merge commit `8363656e`).
The end-of-lane re-merge of root has NOT been done.

## Commits

| SHA | Item |
|---|---|
| 8363656e | 1. merge root head a15ad887 |
| 200cbf74 | 2. C++ walk-forward fit + gtests |
| 994210f4 | 3. IC runner reads atx.dsl-composition-weights/v3 + gtests |
| 2209821e | 4. WIP: fitter flags + check_walk_forward only |

## Build

- Tag `p9-d3w-a` (Debug, preset `equity-dev`), source 994210f4, ExitCode 0, 341 TUs, 830 s.
  Receipt: `build-equity/mega-p9-d3w-a-receipt.json`. Log: scratchpad `d3w-build-p9-d3w-a.log`.
- Executables and their SHA-256:
  - atx-engine-research-composition-tests: `f2ea6ce5eab05bc57516f339fc873fef991b29c41ea545754992fcafce80735e`
  - atx-research-composition: `021138d73f070281fb37d0871279f68172b7e708d947ff1ca39228191dec1a24`
  - atx-impl-strategy-ic-tests: `8cc97584700957d7fb3bb3a9872a514ff4e2165a7cc1bd2340e1d56d29151a1f`
  - atx-equity-strategy-ic: `24513276026397dc8f4858a9525510485826ea4e924f43dc662bc2a2a6d22972`
- Items 2 and 3 compile clean under clang-cl /W4 /WX. No Release build was made.
- The build tree `build-equity/` is kept so the gtests can run without a rebuild. Delete it by name when the lane closes.

## Tests actually run

NONE. The build finished as the stop arrived, and no gtest or pytest suite was started.
The only check made was an `ast.parse` syntax check of `composition_engine_fit.py`.

## Per item

1. **Merge root: DONE.** 8363656e merged a15ad887.

2. **C++ walk-forward fit: DONE, compiled, gtests NOT RUN.**
   - `atx-engine/include/atx/engine/research/composition/fit.hpp:206` has `fit_walk_forward`, `WalkForwardSpec` and `WalkForwardRow`. Its constants are lag 2, step 21, min 21 and max_rows 4096.
   - `atx-engine/src/research/composition/fit.cpp:339` holds the implementation and its refusals.
   - `composition_cli.cpp:186` has `walk_forward_json`, and `:319` handles `fit --walk-forward expanding --lag N --step N [--min-decisions N]`. With the flag absent, the bytes are unchanged.
   - Tests in `atx-engine/tests/research/research_composition_fit_test.cpp`:
     - `:582` `WalkForward.ExpandingMeanClosedForm`
     - `:620` `WalkForward.NoRowUsesFutureDecisions`
     - `:659` `WalkForward.RefusesBeforeAnyRow`
     - `:693` `WalkForward.CliWritesTheRowsAndRefuses`
   - Pending:
     - Run the tests.
     - Teeth by mutation for each refusal.
     - fit.cpp must strip `mom_volman` from the row spec once ALCOMB-7 adds it.

3. **IC runner v3: DONE, compiled, gtests NOT RUN.**
   - `strategy_ic_composition.cpp:533` adds `IcComposition::set_weight_rows`. A date before the first row is flat, and a zero-weight date is skipped. `IcComposition::add` keeps a per-date `weight_at(d)` hook, which the per-member skip can extend.
   - `strategy_ic_rules.cpp:616` adds `walk_forward_stages`, which merges the row boundaries with the volman boundaries.
   - `strategy_ic_rules.cpp:689` adds `composition_walk_forward`, which parses the block and holds its refusals.
   - `strategy_ic_admission.cpp:715` accepts v3. Without v3, a `walk_forward` block is refused. v3 also adds `weights_summary.walk_forward` and `method_recipe.composition_walk_forward`.
   - `strategy_ic_runner.cpp`: the manifest gets `composition_walk_forward`.
   - `strategy_ic_mom_volman.cpp:147` is the one appended line, a cross-lane edit: `pinned.volman_theme = momentum;`.
   - Tests in `atx-impl/tests/strategy_ic_runner_test.cpp`:
     - `:4903` `WeightsV3.SingleRowEqualsV1Bytes`
     - `:4933` `WeightsV3.RowsSwitchAtTheirSessionAndTheBookIsFlatBefore`
     - `:4994` `WeightsV3.RefusalsPrecedeAnyPayloadOrOutput`
   - `strategy_ic_composition_test.cpp:768` has `CompositionWeightRows.RowInForcePerDateAndFlatBeforeTheFirst`.
   - The old unknown-schema runner case now uses v4.
   - Pending:
     - Run the tests.
     - C++ teeth.
     - Check that mom-volman, theme-tsmom and theme-cov-hedge are byte-identical with the flag absent: run the whole `atx-impl-strategy-ic-tests`.

4. **Fitter post-step: WIP (2209821e).**
   - Written in `atx-impl/tools/composition_engine_fit.py`:
     - the docstring with the design (lines 38-52);
     - the constants (lines 86-97);
     - the flags in `add_argument`;
     - `walk_forward`, `walk_forward_options` and `check_walk_forward`, called first in `check_args`. With the flag present, this refuses until `args._walk_forward_stage` is armed, so no unfinished path publishes.
   - Unused imports `shutil` and `Callable` are left for `run_walk_forward`.
   - NOT written:
     - `check_fit` adding `--walk-forward expanding --lag --step --min-decisions` to the exe argv (around line 470);
     - `verify` storing the C++ rows on `args._walk_forward_fit` (around line 572);
     - `run_walk_forward(args, fit, hooks, log)` with a `WalkForwardHooks` NamedTuple of error, canonical, publish, limit, weights_file and exit_ok. Its steps:
       - refuse an existing output or a stale `<output>.wf-stage`;
       - fit into the stage;
       - on EXIT_NO_WEIGHTS, publish the stage as it is;
       - on EXIT_OK, run `weights_v3`, check the size limit, publish, then rmtree the stage;
       - fix the summary;
     - `weights_v3`, which writes schema v3 with a top-level `walk_forward {rule, window, lag, step, min_decisions, rows:[{from_session, weights:{every library id}}]}`. It also adds `theme_standardise.themes` entries for row-weighted members whose top-level weight is 0 (only if their theme is a top-level weighted theme and they have a sign) and sets `provenance.walk_forward`;
     - the `fit_composition_weights.py` `main()` route (build the hooks inside main and add no module-level names);
     - the pytest, a new `atx-impl/tools/test_fit_walk_forward.py` reusing `test_composition_engine_fit` StandIns. It covers: v3 output, flag absent unchanged, refusals with teeth, main routing, and no stage left behind;
     - the fingerprint check.
   - Fingerprint baseline: `.superpowers/sdd/platform-p9-20261003/d3w-logs/fingerprints-before.json` (gitignored dir, kept locally). The fitter's OUTPUT_PRODUCERS must stay `d28a30895ad1393e512d3a20384af025063675e9a2617611ee9cae41e17304a2`. Re-run with scratchpad `d3w-fingerprints.py` from `atx-impl/tools`.

5. **ALCOMB-7: NOT STARTED.** Plan:
   - Add `FitSpec.mom_volman` and the `CompositionFit` fields `mom_volman`, `volman_theme` and `sleeve` (the tsmom step-1 formula on the final weights, 0 outside TRAIN).
   - Add a JSON `mom_volman {theme, sleeve}`. The spec key appears only when on.
   - `composition_engine_fit` compares the C++ sleeve with `composition_theme_tsmom.sleeves`. This is a documented deviation: no rewrite in main(); instead, verify requires the C++ sleeve to equal the Python sleeve recomputed on the C++ weights.
   - Drop the engine-fit refusal and `ENGINE_FIT_REFUSED` in `composition_mom_volman.check_args`.

6. **ALCOMB-6: NOT STARTED.** Plan:
   - The IC exe writes, per scored role, `<role>_mom_volman_sleeve_raw.csv` and `<role>_mom_volman_sleeve_scaled.csv` (header `session_ns,net_return`), only when non-empty.
   - `summary.json` is unchanged.
   - Add a gtest.

7. **D2-9: NOT STARTED.**
   - The S-1 test.
   - The S-2 refusal in `check_factor_series` (`composition_engine_fit.py:225`).
   - Strict int-vs-float equality in `compare_values` (`composition_engine_fit.py:417-448`: line 446's branch and the int/float mix at 424-435 pass an int against an equal float).
   - Tests in `atx-impl/tools/test_fit_composition_weights.py:750`.

8. **D2R-3 (S1, S2): NOT STARTED.** Assert the full `unrecognized arguments: <token>` in both loops of `scripts/tests/test_research_spec.py:938`. This is a cross-lane edit.

## New literals, flags and keys (so far)

- Literals: `walk-forward-v1` and `expanding`. `atx.dsl-composition-weights/v3` is already in classes.json, and no new `atx.<name>/v<n>`.
- Flags:
  - `atx-research-composition fit --walk-forward expanding --lag N --step N [--min-decisions N]`.
  - Fitter: `--walk-forward expanding --walk-forward-lag N --walk-forward-step N [--walk-forward-min-decisions N]`.
- Keys:
  - `composition_fit.json` `walk_forward {rule, window, lag, step, min_decisions, rows:[{decision_index, from_session, fit_decisions, last_fit_index, last_fit_session, weights}]}`.
  - The weights file v3 `walk_forward {rule, window, lag, step, min_decisions, rows:[{from_session, weights}]}`.
  - IC `weights_summary.walk_forward {rule, rows, first_session, last_session}`.
  - `method_recipe.composition_walk_forward` and the manifest `composition_walk_forward`.

## Cross-lane edits

- `atx-impl/src/strategy_ic_mom_volman.cpp:147`: one line.
- `scripts/tests/test_research_spec.py`: not yet made.

## Flag-absent identity procedure (for root, once tests run)

1. Build `atx-impl-strategy-ic-tests` and `atx-engine-research-composition-tests`.
2. Run both in full. Every pre-existing case, including mom-volman, theme-tsmom and theme-cov-hedge, must pass unchanged. The v1/v2 byte tests prove the runner's output is identical.
3. Run the fitter's pytest files and confirm that OUTPUT_PRODUCERS is still `d28a3089...`.

## Next step for a fresh agent

1. Run the anchored gtests on the existing p9-d3w-a binaries (`--gtest_filter=WalkForward.*:ResearchComposition*`, then `WeightsV3.*:CompositionWeightRows.*`, then each whole exe). Fix anything red with tag `p9-d3w-b`.
2. Finish item 4 in `composition_engine_fit.py`, from `check_fit` at line 464 onward, then `main()` in `fit_composition_weights.py`, the pytest and the fingerprint check.
3. Do items 5 through 8 in order.
4. Release build, re-merge root, write the report, delete `build-equity` by name.

## Open questions

- `atx-research-eval` `net_moments` prints no vol (PRE-38), so ALCOMB-6's sleeve vol needs the eval verb, which is not D3W's file.

## Permission refusals

None.
