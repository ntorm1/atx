# Lane D3W-PB review (P9 RESUME 9; first review; fresh adversarial reviewer)

## PARTIAL at the owner stop (STOP 9)

**Status: PARTIAL. Provisional verdict: CHANGES_REQUIRED (1 Required, 7 Suggested).**

- Done: the spec check item by item, the quality read, my own Python runs, and the D3WPB-Q2 answer.
- Not done: the TEST-1 C++ teeth build and my own gtest runs.
  - I configured `equity-dev` in pool-23 (exit 0).
  - The build waited because another agent was compiling on the host: pool-31 (COV-MV) `atx-build.ps1 check`, HOST-3 priority.
  - The owner stop came before the host was free. **No build ran and no mutation was ever applied.**
  - The tree is clean: `fit.cpp` hash-object `1c3461a2` = the HEAD blob. My configure tree is deleted by name.
- R-1 below does not depend on the pending teeth: the verdict cannot become APPROVE until R-1 is closed.
- R-1 is a one-round fix: 4 labels and a comment in one test file. It is **not** P10 for D3W-PB.
- The exact next step is at the end of this file.

## Reviewed SHAs

- HEAD `460f5ba2` (the deletion commit, the last of the branch); pre-deletion head `c044f5f6`.
- BASE `f0726acb`, D3W's APPROVE head, as the dispatch names it.
  - REV-BASE's `git merge-base 460f5ba2 feat/platform-p9-20261003` is `fd084e78`.
  - The range `fd084e78..f0726acb` is D3W's, already reviewed (`task-D3W-review.md`).
- Scope: `git log --no-merges 460f5ba2 ^f0726acb` is 8 commits, all linear, with no merge in the range:
  `dcddb01d`, `6da02a0d`, `02d3e7a3`, `54828cdd`, `bd4e160e`, `703ac03a`, `c044f5f6`, `460f5ba2`.
- Code diff `git diff f0726acb 460f5ba2`: 18 code / test files, +680 / -299.
- Root is not merged into the lane, by design (B2-EVAL-KEEP).

## Spec verdict (brief-D3W-PB, brief-D3 item 6, D3W-S, D3WPB-Q1 / Q2, rulings)

| item | verdict | basis |
|---|---|---|
| C++ `ew-theme-aim-v2` fit mode | DONE | `ew_theme_std.hpp:33-36` (rule id); `fit.cpp:184-189` `take_gain_parent`, `:275-287` (classification, modifier refusal), `:335-345` (gain refusal, `theme_gain_weights` on the gains, `theme_weight` in member order), `:415-418` (walk-forward refusal) |
| `composition_fit.json` keys | DONE | `composition_cli.cpp:105-110`: `parent.member_gains`, `parent.theme_weights`, only when the composition is aim-v2. No new flag and no new `atx.*/vN` literal |
| gtest: closed form on a planted model | DONE (lane's log; my run pending) | `research_composition_fit_test.cpp` `AimV2.ClosedFormOnAPlantedModel`. I re-derived the expected values by hand, independently (below) |
| gtests: refusals with teeth | DONE (lane's teeth T1-T3, `pb-teeth-cpp.log`); my own teeth pending | `AimV2.RefusalsBeforeAnyFit`: 4 modifiers, 4 bad gains, missing gain, walk-forward. `AimV2.CliWritesGainsAndThemeWeightsOnlyForIt`: `--walk-forward` exit 1, nothing published |
| flag absent = bytes unchanged (C++) | DONE by reading | Every other composition takes the old `else if (!fit.tiers.empty())` / `else` branches unchanged. `take_gain_parent` is the same four moves. `CliWrites...` checks that a std-aim document has neither key |
| flag absent = bytes unchanged (fitter) | DONE, my run | See "My runs": fingerprints, then identity 21 / 24 with 3 at `provenance.engine_fit.producer_fingerprint` |
| no edit inside `fit` / `fit_prior` or their closure | DONE, my run | `fit_composition_weights.py` is not in the diff. `OUTPUT_PRODUCERS (fit, fit_prior)` is `d28a30895ad1...` at all three SHAs |
| fitter post-step under `--engine-fit` | DONE | `composition_engine_fit.py:92-95` (`AIM_V2`, `SUPPORTED`, `GAIN_PARENTS`), `:439` -> `_expected_aim_v2` `:480-497` (check 3), `_kernels` `:634`. `check_written` `:683-686` holds the written weights to the C++ bit for bit |
| P12 comparison vs Python on the fixtures | DONE (lane's real-exe log 180 / 0; my real-exe run pending) | Check 3: `P12_TOLERANCE = 1e-12` x max(1, \|x\|) (`:101`) over every key, `member_gains` / `theme_weights` included; then `check_written` bit for bit. `RealExecutables` subtest `aim-v2` (`test_composition_engine_fit.py:793`); stand-in teeth `test_aim_v2_carries_the_cpp_bits_or_refuses` `:347` |
| D2-3 deletion separate and LAST | DONE | `460f5ba2` is the single child of `c044f5f6` and touches 3 files: `composition_resid.py` (the 7 reference functions, the numpy import, the docstring), `test_composition_resid.py` (the functions as `ref.*`), `test_no_python_mirror.py` (the row). My AST compare: all 7 moved functions are byte-identical modulo the `_` rename. No non-test caller remains (grep of every `.py`). Fingerprints and identity are unchanged across the deletion (my run) |
| D3WPB-Q1 (the remaining rules stay on G-P5 with `retired_by: P10`) | **NOT DONE: R-1** | `test_no_python_mirror.py:60`, `:64`, `:66`, `:69` still say `"D3"` |
| D3WPB-Q2 | answered | See "D3WPB-Q2" |
| S-1 placeholders / trees | DONE | `dcddb01d`. `build-equity-rel/` is gone; `build-equity/` held only tracked files and the lane's receipts at my start |
| S-2 (REQUIRED): one definition of mom-volman-v1's constants | DONE | See (a) below |
| S-4 | DONE | See (b) below |
| S-5 | DONE | See (c) below |
| S-8 (REQUIRED): Python copy of the lifted-candidate refusal | DONE | See (d) below |
| S-9 | DONE | `strategy_ic_mom_volman_test.cpp:352-372` `ScratchDir` (see S-b) |
| S-10 | DONE (lane's teeth T4 log only) | `fit.cpp:444` `cut+wf.lag<=j` -> `<=j+1U` turned `WalkForward.NoRowUsesFutureDecisions` and `.ExpandingMeanClosedForm` red (`pb-teeth-cpp.log`) |
| S-3 / S-7 (ABS), S-6 (PRE) | not this lane's | - |
| D3W-STALE | noted, not a finding | `scripts/specs/p9/rules/mom-volman-v1.json:50` is untouched (nothing under `scripts/specs` is in the diff) |
| Tests: `scripts/tests` seeds 0 / 1, stage chain, whole targets, Release | deferred to the gate (TEST-1) | - |
| Report: TRAIN identity procedure, "For PRE" | DONE | `task-D3W-PB-report.md`. I reproduced the procedure exactly |

**(a) S-2.**
- New `atx-engine/include/atx/engine/research/composition/mom_volman.hpp:13-19` holds the id, theme, lookback, lag, step, `min_decisions = lookback + lag` and max.
- `fit.hpp` includes it.
- `strategy_ic_mom_volman.hpp:35-43` includes it and re-exports by `using`. The old impl definitions are gone (grep).
- The remaining textual forms are bound by tests:
  - `volman_params_schema` (`strategy_ic_rules.cpp:451-455`) is tied to the Python constants through the `composition_rules_list.json` fixture (`test_block_is_the_rule_tables_row`).
  - The Python constants are tied to the engine header by `test_constants_match_the_runner_header`, which now parses `mom_volman.hpp`.

**(b) S-4.**
- `strategy_ic_rules.cpp:697` `walk_forward_shape`, `:719` `walk_forward_rows`, `:761` `lift_walk_forward_candidates`, `:796` `composition_walk_forward`.
- Each is under 60 lines.
- I compared them with `f0726acb`. The refusal order is the same: block, shape, redistribution / residualise / tsmom, rows, lift. Every message is the same string after literal concatenation.

**(c) S-5.**
- `write_mom_volman_sleeves` moved to `strategy_ic_mom_volman.cpp:203`.
- Its body is identical modulo whitespace (my compare against `f0726acb:strategy_ic_rules.cpp`).
- Six includes left the rule-table TU.

**(d) S-8.**
- `weights_v3` (`composition_engine_fit.py:725-749`) keeps the theme mapping only. The sign and theme refusal is gone.
- The C++ runner's `lift_walk_forward_candidates` is now its only home, tested by `WeightsV3.RefusalsPrecedeAnyPayloadOrOutput`.
- No other Python copy appeared. `_expected_aim_v2` calls the existing `composition_rules` functions (D2's check-3 pattern) and defines no G-P5 symbol.
- The mirror guard passes (my run).
- "The fitter reads the runner's refusal" holds in this sense: the IC stage refuses such a file before any payload. The fitter itself publishes it unchecked. See S-e.

**Closed form, independently re-derived.**
- Inputs: gains a {1, 3}, b {2, 2, 4}; T = 2.
- Uncapped weights: 1/8, 3/8 / 1/8, 1/8, 1/4. The cap is 1/4.
- Pass 1 caps c1 (excess 1/8). Pro rata on b's base 1/2 this gives +1/32, +1/32, +1/16, so c4 = 5/16.
- Pass 2 caps c4 (excess 1/16). The only receiver is c0, so c0 = 3/16.
- Final weights: 3/16, 1/4, 5/32, 5/32, 1/4 (sum 1). Theme masses 7/16 and 9/16.
- These equal the test's constants. The test pins the contract with dyadic values, not the implementation; only its last check (equality with `theme_gain_weights` called directly) is an implementation cross-check.

## D3WPB-Q2: the rules P9-W's parent names; TRAIN-fitted gains

**Answer: no possible P9-W parent carries a TRAIN-fitted aim gain, so P9-W is unaffected by Q2. The prereg states it in round 2b-ii.**

- **Templates.** `scripts/specs/p9/templates/` holds only the P9-H (`theme-cov-hedge.json`) and P9-M (`mom-volman.json`) cell templates. There is no P9-W template; P9-W runs as kind `walk-forward`.
- **The walk-forward kind fixture** names the parent `tiny.json` (tiny world, ew-theme-v1, Python fit). D3's fixture (pool-20 `699440bb`) says preflight refuses it there, so it is a test parent, not P9-W's.
- **The registered parent** is P9-F0h (prereg A.4, B.6; PRE-39).
  - With no accepted cell, it is the re-based Y-F0, `scripts/specs/v8/lib-v8ysb-gm.json`. Its fit flags: `--orientation prior --screen v4-prior-v1 --composition ew-theme-std-v1 --theme-erc theme-erc-v1`.
  - ew-theme-std-v1's scores are prior tier scores (after the v8 re-grades); there is no gain.
  - theme-erc-v1's covariance is refit on every row's window (`fit_walk_forward` runs `fit_composition` on the masked TRAIN view).
- **The cells before P9-W** can add theme-cov-hedge-v1 (P9-H), mom-volman-v1 (P9-M) and info-clock-rate-v1 (P9-C, a NAV rule).
  - P9-MV reads the risk model, so it is never P9-F0h.
  - None of these is an aim rule. The aim-gain rules are ew-theme-std-aim-v1, ic-shrink-aim-v1 and ew-theme-aim-v2, and the last is refused under `--walk-forward`.
- **Whole-TRAIN constants that do stay fixed across P9-W's rows** (not gains; for PRE's disclosure, not findings of this lane):
  1. The admitted member set (the `v4-prior-v1` screen runs once on the whole TRAIN window).
  2. mom-volman-v1's schedule, if P9-M is accepted. This is already the "Limit" line of B.6 (ALCOMB-4).
- **Wording note.** The aim-v2 refusal message gives "every row would be the full fit" as the reason (`fit.cpp:415-418`). That is also true of a plain ew-theme-std-v1 parent, which is accepted. The reason that separates aim-v2 is Q2's, the whole-TRAIN gains, which the message also names. No change is needed.

## Quality verdict (owner priority: modular / reusable / performant C++; the Python side shrinks)

**Sound.**

- **One fit entry point per mode.**
  - aim-v2 is one branch of the one `fit_composition`, sharing `theme_gain_weights` and `take_gain_parent` with the std rules.
  - No window or residual logic was added.
  - The walk-forward refusal sits in the one `fit_walk_forward`.
  - The CLI adds one branch.
- **Bounded allocation.** `theme_weight` holds at most 32 entries and `gains` at most 256. The hot path is untouched.
- **`/W4 /WX`.** The lane's Debug build `p9-d3wpb-a` (source `703ac03a`, every C++ hunk) exited 0; my build is pending. The deletion commit is Python only.
- **100 columns.** One added line is over: S-a.
- **Python shrinks.**
  - Production Python is net about -100 lines: `composition_resid.py` -128 (+4 / -132), `composition_engine_fit.py` +30
    (+51 / -21).
  - The tests grow, by the moved oracle.
  - No new rule copy.
  - G-P5 drops from 34 to 32 functions and from 13 to 12 rows (my mirror-guard run).

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `scripts/tests/test_no_python_mirror.py:60`, `:64`, `:66`, `:69` | **Required (R-1)** | Ruling D3WPB-Q1 says the remaining composition rules "stay on the G-P5 list with `retired_by: P10`". The four rows the deletion leaves still name retiring lane `"D3"`: `composition_rules.py`, `composition_theme_erc.py`, `composition_ic_shrink.py`, and the fitter's `newey_west_t` / `screen_v4`. `D3` is in `P9_LANES`. So when root sets `P9_FREEZE = True`, `test_rows_retired_by_a_p9_lane_are_gone_at_the_freeze` fails on all four rows; nothing in P9 retires them. The ruling's parenthesis "(32 functions in 12 rows)" is the count with the `"D3"` labels. With `P10` the lane-tree count is 21 / 8, and on the dry-merged root tree 3 / 2 (14 / 6 today). | One new commit after `460f5ba2` (history is not rewritten): set the four `retired_by` fields to `"P10"` and update the comments at `:56-57` and `:61-62` to cite D3WPB-Q1 (the D2-5 precedent for `horizon_stats.py`). Re-run `test_no_python_mirror.py` in its own invocation. Alternatively the PM may rule that root makes this edit at the freeze commit; then it becomes a root merge check, not a lane finding. |
| `atx-engine/include/atx/engine/research/composition/mom_volman.hpp:3` | Suggested (S-a) | A comment line of 101 columns (house limit 100). | Reflow at the next touch. |
| `atx-impl/tests/strategy_ic_mom_volman_test.cpp:347-372` | Suggested (S-b) | The bespoke `ScratchDir` repeats what `atx-test-scratch` (`tests/support/process_scratch.cpp`) already does for every atx-impl test target (`atx-impl/tests/CMakeLists.txt:34`): it repoints TMP / TEMP per process before `main`. So S-9's collision could not happen on this target. The lane's own comment says so. It is harmless, but it is 25 lines of duplicated mechanism against the reuse priority. | Keep it, or drop it and rely on `atx-test-scratch` with a one-line comment. |
| `atx-impl/tools/test_fit_walk_forward.py` (no case) | Suggested (S-c) | No fitter-level test runs `--engine-fit --walk-forward` with ew-theme-aim-v2 and checks that the exe's refusal reaches the fitter's exit with nothing published. The pieces are covered separately: C++ `CliWrites...` (exit 1, no dir) and `test_run_exe_refusals` (the fitter on a failing exe). | A `RealExecutables` case at the next touch. |
| `composition_engine_fit.py:683-686` with `:480-497` | Suggested (S-d) | For aim-v2, ruling D2-1 ("write the C++ values; agreement under P12") becomes "the Python weights must equal the C++ bit for bit, else refuse". The reason: the fitter's rule takes no engine argument inside the fingerprint closure. This is stricter than D2-1. A one-ulp gap inside P12 on real data would refuse the `--engine-fit` fit. The risk is low: both sides use `theme_gain_weights` with the same expression order, and the bits are equal on the fixture. The lane documents it in the docstring. | None in P9. P10's document builder over `composition_fit.json` removes it. |
| `composition_engine_fit.py:725-749` (S-8) | Suggested (S-e) | That the C++ fit can never produce a row the runner refuses is argued in a docstring: members are +1, and every theme of a fit carries mass. No test pins it. If that invariant ever broke, the fitter would publish a v3 file that is refused only at the IC stage (fail-late) rather than at fit time as before. | A `WalkForward.*` gtest at the next C++ touch: for every row, each member with a row weight > 0 has a theme with full-fit mass > 0. |
| `atx-impl/src/strategy_ic_rules.cpp` (S-2 / S-4 / S-5 hunks) | Suggested (S-f), root's | The IC exe's source changes, though its behaviour does not (every message and byte is kept). `atx-research-composition` changes too. Under EXE-FREEZE, any relink after the wave-3 P9-B0 re-pin forces P9-B0 to be redone. | Root merges D3W-PB before the gate's P9-B0 or not in P9 (see "What root must check"). |

## Evidence: my runs vs the lane's logs

All my runs are in pool-23 or on `git archive` copies under my scratch. Interpreter `C:/Program Files/Python312/python.exe`, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, own `--basetemp` with its parent created first. `git status --porcelain` was empty after each run.

**My runs.**
- **Fingerprints.** `d3w-logs/tools/d3w-fingerprints.py`, cwd each archive's `atx-impl/tools`, at `f0726acb`, `c044f5f6` and `460f5ba2`:
  - Only `composition_engine_fit` moves, `a6dd0da59c13` -> `b0052dc58356`, at the pre-deletion head.
  - The other 11 entries are equal at all three SHAs, `OUTPUT_PRODUCERS (fit, fit_prior)` `d28a30895ad1` and `composition_resid` `9c48f390ac6d` included.
  - The deletion moves nothing.
  - Mine equal the lane's `pb-fingerprints-pre-deletion.json` and `pb-fingerprints-deletion.json` exactly.
- **Identity.** `pb-ident.py` at each of the three archives with the SAME work dir, then `cmp.py`:
  - `f0726acb` vs `c044f5f6`: 21 of 24 files byte-identical. The 3 `engine-*` `composition_weights.json` differ only at `$.provenance.engine_fit.producer_fingerprint`.
  - `c044f5f6` vs `460f5ba2`: 24 of 24 identical.
  - This reproduces `pb-identity-pre-deletion.log` and `pb-identity-deletion.log`.
- **The five fitter pytest files, without exes** (`RealExecutables` skipped): `test_composition_engine_fit`, `test_composition_mom_volman`, `test_composition_resid`, `test_fit_composition_weights`, `test_fit_walk_forward`. Result: **177 passed, 3 skipped, 84 subtests**, exit 0.
- **`scripts/tests/test_no_python_mirror.py`, own invocation:**
  - Lane tree: **7 passed**, "G-P5: 32 mirrored rule function(s) in 12 row(s)".
  - Dry-merged root tree, root `73b4acfd` + `460f5ba2` via `git merge-tree --write-tree`, tree `77cc8434`, archived: **7 passed**, 14 / 6.
- **Dry merges.** `git merge-tree --write-tree` of root `73b4acfd` with `f0726acb`, `c044f5f6` and `460f5ba2`: all clean. `460f5ba2` with ABS `6a3f5043` (APPROVE), SIGN `9c360cde`, D3 `699440bb` and C3b `76b9f256`: all clean.
- **Configure.** `atx-build.ps1 configure -Preset equity-dev` in pool-23: exit 0. The tree was deleted by name at the stop: 7.3 MB, no object file.

**From the lane's logs only (my C++ runs did not happen):**
- gtests `AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*` 21 / 21 and `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` 18 / 18 (`pb-gt-a-*.log`, build `p9-d3wpb-a`, source `703ac03a`).
- Teeth T1-T4 (`pb-teeth-cpp.log`, build `p9-d3wpb-b`, MUTATED, restored, blob = HEAD).
- The five fitter pytest files with real exes: 180 / 0 (`pb-pytest-fitter-real.log`).
- Under EXE-CUR, `p9-d3wpb-a` is current for HEAD's C++. `git diff 703ac03a 460f5ba2` touches no C++ / CMake file.

**Not run:**
- My C++ teeth build (tag `p9-d3wpb-c`, never started).
- My gtest filters.
- The fitter pytest with my exes.

**Blind.** I read synthetic fixtures, specs (fit flags only) and logs. I opened no 2020-2023 output and nothing dated 2024 or later. No statistic is quoted here.

## What root must check at merge

1. **Order.**
   - Merge D3W `f0726acb` first, with its own review's checks: B2-EVAL-KEEP on `eval_report.cpp` (B2's PRE-15 hunks and D3W's `--vol` hunk are keep-both; the dry merge was clean). Then run the eval gtests and the PRE-15 gtest.
   - Then the pre-deletion head `c044f5f6`. Run the report's TRAIN identity procedure: fingerprints step 1, then steps 2-3 (`pb-ident.py` on BASE and the merged tree with the SAME work dir, then `cmp.py`). Expect 21 / 24, with the 3 `engine-*` weights files differing only at `provenance.engine_fit.producer_fingerprint`, and `OUTPUT_PRODUCERS` `d28a3089...`.
   - Then the deletion `460f5ba2`. Re-run `test_composition_resid.py` and `test_no_python_mirror.py` (own invocation). Fingerprints and identity must be unchanged.
   - Then R-1's fix commit, or root's own `retired_by: P10` edit if the PM rules so.
2. **Targeted checks (ROOT-1a).**
   - Debug build of `atx-engine-research-composition-tests`, `atx-impl-strategy-ic-tests`, `atx-research-composition` and `atx-research-admission`.
   - Filters `AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*` and `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*`.
   - The five fitter pytest files with `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` set. Expect 180 / 0.
3. **SQL2-CLS.** No new `atx.<name>/v<n>` literal. The added lines use the existing `atx.composition-fit-spec/v1` and `atx.dsl-composition-weights/v3`.
4. **Mirror guard on the merged tree.** 7 passed (my dry-merged run: 14 functions in 6 rows before R-1; 3 in 2 after). `P9_FREEZE` fails at the freeze unless R-1 is done.
5. **EXE-FREEZE (S-f).** The IC exe and `atx-research-composition` relink with this lane. Merge before the gate's P9-B0 re-pin, or not in P9.
6. **Stretch (D3W-PB).** Merges only if APPROVE at the gate, else P10 whole.

## Next step (for the resuming reviewer)

1. Wait until no other compile runs on the host (HOST-3: this review ranks lowest).
2. In pool-23, run `powershell scripts\atx-build.ps1 configure -Preset equity-dev`.
3. Run one teeth tool in TEETH-RESTORE form. My tool was `rev_teeth_cpp.py` in this session's scratch, which a resumer cannot reach, so recreate it:
   - Keep `atx-engine/src/research/composition/fit.cpp`'s bytes and assert `hash-object` = HEAD blob `1c3461a2`.
   - Replace exactly one occurrence, at `:340`, of `    ATX_TRY(auto parent,theme_gain_weights(gains,fit.member_theme,themes));` with `    ATX_TRY(auto parent,theme_gain_weights(std::vector<f64>(gains.size(),1.0),fit.member_theme,themes));`. This is the aim-v2 closed form's normalisation by the gains replaced by the equal split.
   - Run `scripts\research-build.ps1 -Tag p9-d3wpb-c -Targets "atx-engine-research-composition-tests,atx-research-composition,atx-research-admission,atx-impl-strategy-ic-tests"`.
   - Restore the bytes in a `finally` BEFORE any test runs, then assert the raw SHA-256 equals the kept bytes, `git status --porcelain` is empty, and `hash-object` = HEAD blob.
   - Then run the two named filters on the built binaries.
4. Expected:
   - Red: `AimV2.ClosedFormOnAPlantedModel` and `AimV2.CliWritesGainsAndThemeWeightsOnlyForIt`.
   - Green: the other composition filter tests and `WeightsV3.*:CompositionWeightRows.*`.
   - The five fitter pytest files with those (mutated) exes: only the `RealExecutables` subtest `aim-v2` red. The fixture world's aim-v2 has distinct gains within each theme and a cap pass, so check 3 refuses. This is Python-side teeth evidence only (EXE-CUR); the clean real-exe count stays the lane's 180 / 0.
5. Delete the object tree by name. Write the final verdict (APPROVE only if R-1 is closed or ruled to root).

## Permission refusals

None.
