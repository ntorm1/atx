# Lane D3W-PB review (P9 RESUME 9 first review, resumed in RESUME 10; REV-FRESH, D3WPB-SEQ)

## Verdict: APPROVE

**Spec: PASS. Quality: PASS. Required open: 0 (R-1 closed at `90bc1962`). Suggested: 7 (S-a .. S-g).**

- This is the review a first reviewer left PARTIAL at STOP 9 (commit `a0c7427c`). A fresh reviewer resumed it in R10
  (REV-FRESH) after the lane's fix round 1 (D3WPB-R1, D3WPB-SEQ). The first reviewer's findings, spec table, quality
  read and D3WPB-Q2 answer are kept below. Everything I changed or added is marked "(R10)".
- R-1 is closed. The fix commit `90bc1962` sets the four `retired_by` labels to `"P10"` and cites D3WPB-Q1 in the two
  comments. It removes and weakens nothing. My own run of the mirror guard: 7 passed, 21 / 8 (see "My runs (R10)").
- The C++ teeth owed by the PARTIAL are done: ONE Debug build, `p9-d3wpb-c`, of the mutated tree (TEETH-RESTORE).
  Exactly the two expected gtests turned red, and only the expected pytest subtest turned red. The restore was
  verified before any test ran.
- New in R10: S-g, root's. At the fix commit, root's current head conflicts in `scripts/tests/test_no_python_mirror.py`
  (one hunk, comment lines only). It is a keep-both resolution, given in "What root must check at merge", item 1.
- Stretch (ruling D3W-PB): this lane merges only if it is APPROVE at the gate, else P10 whole. Under EXE-FREEZE (S-f)
  it merges before the gate's P9-B0 re-pin, or not in P9.

## Reviewed SHAs

- HEAD `362793c6` (records). Under it, the fix `90bc1962` (D3WPB-R1), then the PARTIAL review `a0c7427c`. The lane's
  code head is `460f5ba2` (the D2-3 deletion); the pre-deletion head is `c044f5f6`.
- BASE `f0726acb`, D3W's APPROVE head, as the dispatch names it (REV-BASE / REV-FRESH: the SHA the PARTIAL names).
  - REV-BASE's `git merge-base 460f5ba2 feat/platform-p9-20261003` is `fd084e78`.
  - The range `fd084e78..f0726acb` is D3W's, already reviewed (`task-D3W-review.md`).
- Scope: `git log --no-merges 460f5ba2 ^f0726acb` is 8 commits, all linear, with no merge in the range:
  `dcddb01d`, `6da02a0d`, `02d3e7a3`, `54828cdd`, `bd4e160e`, `703ac03a`, `c044f5f6`, `460f5ba2`.
  - (R10) Then `a0c7427c` (the review file only), `90bc1962` (one test file), `362793c6` (the report and the handoff).
- Code diff `git diff f0726acb 362793c6`, excluding `.superpowers`: 18 code / test files, +686 / -305.
- Root is not merged into the lane, by design (B2-EVAL-KEEP; D3WPB-FIX1 said "no merge of root").

## Fix round 1 (R10): R-1 verified closed

| check | result |
|---|---|
| `git show --stat 90bc1962` | One file, `scripts/tests/test_no_python_mirror.py`: +8 / -8. Its parent is `a0c7427c`; history is not rewritten. |
| The four labels | `:60` `composition_rules.py`, `:64` `composition_theme_erc.py`, `:66` `composition_ic_shrink.py`, `:69` `fit_composition_weights.py` (`newey_west_t`, `screen_v4`): each is now `"P10"`. No `"D3"` row remains (scan of the file). |
| The two comments | `:56-57` cite D2-3 and D3WPB-Q1 (the D2-5 precedent for `horizon_stats.py`; "the flag-absent fit still runs the Python rules"). `:61-62` cite D3WPB-Q1 (the P10 fitter that builds every document from `composition_fit.json`). |
| Nothing weakened | No row, symbol, check, `P9_LANES` / `LANES` / `FROZEN_PAIRS` entry or test changed. The diff is the four label strings and the two comment pairs. The file's longest line is 119 columns, inside its 120. |
| Own invocation, lane tree | `scripts/tests/test_no_python_mirror.py`: **7 passed**; it prints "G-P5: 21 mirrored rule function(s) in 8 row(s) left for P9 lanes". Before the fix: 32 / 12. This equals the lane's report line and `90bc1962`'s message. |
| The freeze the fix is for | On a scratch copy of the dry-merged tree (root `b182dfdd` + `362793c6`, the S-g resolution applied, `P9_FREEZE = True` set in the copy only), `test_rows_retired_by_a_p9_lane_are_gone_at_the_freeze` lists exactly one row: `scripts/wave_rules.py ['sign_pm7_35'] (E2)` (lane SIGN's). No composition row is listed. Without the fix, the four composition rows would also have been listed. |
| Records | `task-D3W-PB-report.md` "Fix round 1 (D3WPB-R1)" (lines 206-211) and the handoff head line match the commit. |

## Spec verdict (brief-D3W-PB, brief-D3 item 6, D3W-S, D3WPB-Q1 / Q2, rulings)

| item | verdict | basis |
|---|---|---|
| C++ `ew-theme-aim-v2` fit mode | DONE | `ew_theme_std.hpp:33-36` (rule id); `fit.cpp:184-189` `take_gain_parent`, `:275-287` (classification, modifier refusal), `:335-345` (gain refusal, `theme_gain_weights` on the gains, `theme_weight` in member order), `:415-418` (walk-forward refusal) |
| `composition_fit.json` keys | DONE | `composition_cli.cpp:105-110`: `parent.member_gains` and `parent.theme_weights`, written only when the composition is aim-v2. No new flag and no new `atx.*/vN` literal |
| gtest: closed form on a planted model | DONE; teeth mine (R10) | `research_composition_fit_test.cpp` `AimV2.ClosedFormOnAPlantedModel`. The first reviewer re-derived the expected values by hand (below). (R10) My mutation of `fit.cpp:340` turns it red. The clean green is the lane's log only (EXE-CUR) |
| gtests: refusals with teeth | DONE | `AimV2.RefusalsBeforeAnyFit`: 4 modifiers, 4 bad gains, missing gain, walk-forward. `AimV2.CliWritesGainsAndThemeWeightsOnlyForIt`: `--walk-forward` exit 1, nothing published. The lane's teeth T1-T3 are in `pb-teeth-cpp.log`. (R10) My mutation, a different one (the normalisation, not a refusal), also turns `CliWrites...` red |
| flag absent = bytes unchanged (C++) | DONE by reading; R10 teeth consistent | Every other composition takes the old `else if (!fit.tiers.empty())` / `else` branches unchanged. `take_gain_parent` is the same four moves. `CliWrites...` checks that a std-aim document has neither key. (R10) With aim-v2's normalisation mutated, every other test of both filters stayed green, 19 + 18 |
| flag absent = bytes unchanged (fitter) | DONE, first reviewer's run | Fingerprints, then identity 21 / 24 with 3 differing at `provenance.engine_fit.producer_fingerprint` (section "Evidence") |
| no edit inside `fit` / `fit_prior` or their closure | DONE, first reviewer's run | `fit_composition_weights.py` is not in the diff. `OUTPUT_PRODUCERS (fit, fit_prior)` is `d28a30895ad1...` at all three SHAs. `90bc1962` touches no fitter file |
| fitter post-step under `--engine-fit` | DONE | `composition_engine_fit.py:92-95` (`AIM_V2`, `SUPPORTED`, `GAIN_PARENTS`), `:439` -> `_expected_aim_v2` `:480-497` (check 3), `_kernels` `:634`. `check_written` `:683-686` holds the written weights to the C++ bit for bit |
| P12 comparison vs Python on the fixtures | DONE; teeth mine (R10) | Check 3: `P12_TOLERANCE = 1e-12` x max(1, \|x\|) (`:101`) over every key, `member_gains` / `theme_weights` included; then `check_written` bit for bit. `RealExecutables` subtest `aim-v2` (`test_composition_engine_fit.py:793`). (R10) Run with my mutated exes, it is the one red: check 3 refused the fit ("the C++ fit differs from the fitter's (16)"). The clean 180 / 0 is the lane's `pb-pytest-fitter-real.log` (EXE-CUR) |
| D2-3 deletion separate and LAST code commit | DONE | `460f5ba2` is the single child of `c044f5f6` and touches 3 files: `composition_resid.py` (the 7 reference functions, the numpy import, the docstring), `test_composition_resid.py` (the functions as `ref.*`), `test_no_python_mirror.py` (the row). The first reviewer's AST compare: all 7 moved functions are byte-identical modulo the `_` rename. No non-test caller remains. Fingerprints and identity are unchanged across the deletion. After it: a review commit, the one-file label fix and a records commit (no code) |
| D3WPB-Q1 (the remaining rules stay on G-P5 with `retired_by: P10`) | **DONE (R10): R-1 closed at `90bc1962`** | See "Fix round 1 (R10)" above |
| D3WPB-Q2 | answered | See "D3WPB-Q2" |
| S-1 placeholders / trees | DONE | `dcddb01d`. `build-equity-rel/` is gone. `build-equity/` held only tracked files and the lane's receipts at the first reviewer's start |
| S-2 (REQUIRED): one definition of mom-volman-v1's constants | DONE | See (a) below |
| S-4 | DONE | See (b) below |
| S-5 | DONE | See (c) below |
| S-8 (REQUIRED): Python copy of the lifted-candidate refusal | DONE | See (d) below |
| S-9 | DONE | `strategy_ic_mom_volman_test.cpp:352-372` `ScratchDir` (see S-b) |
| S-10 | DONE (lane's teeth T4 log only) | `fit.cpp:444`: `cut+wf.lag<=j` -> `<=j+1U` turned `WalkForward.NoRowUsesFutureDecisions` and `.ExpandingMeanClosedForm` red (`pb-teeth-cpp.log`) |
| S-3 / S-7 (ABS), S-6 (PRE) | not this lane's | - |
| D3W-STALE | noted, not a finding | `scripts/specs/p9/rules/mom-volman-v1.json:50` is untouched (nothing under `scripts/specs` is in the diff) |
| Tests: `scripts/tests` seeds 0 / 1, stage chain, whole targets, Release | deferred to the gate (TEST-1) | - |
| Report: TRAIN identity procedure, "For PRE" | DONE | `task-D3W-PB-report.md`. The first reviewer reproduced the procedure exactly |

**(a) S-2.**
- New `atx-engine/include/atx/engine/research/composition/mom_volman.hpp:13-19` holds the id, theme, lookback, lag,
  step, `min_decisions = lookback + lag` and max.
- `fit.hpp` includes it.
- `strategy_ic_mom_volman.hpp:35-43` includes it and re-exports by `using`. The old impl definitions are gone (grep).
- The remaining textual forms are bound by tests:
  - `volman_params_schema` (`strategy_ic_rules.cpp:451-455`) is tied to the Python constants through the
    `composition_rules_list.json` fixture (`test_block_is_the_rule_tables_row`).
  - The Python constants are tied to the engine header by `test_constants_match_the_runner_header`, which now parses
    `mom_volman.hpp`.

**(b) S-4.**
- `strategy_ic_rules.cpp:697` `walk_forward_shape`, `:719` `walk_forward_rows`, `:761` `lift_walk_forward_candidates`,
  `:796` `composition_walk_forward`.
- Each is under 60 lines.
- Compared with `f0726acb`, the refusal order is the same: block, shape, redistribution / residualise / tsmom, rows,
  lift. Every message is the same string after literal concatenation.

**(c) S-5.**
- `write_mom_volman_sleeves` moved to `strategy_ic_mom_volman.cpp:203`.
- Its body is identical modulo whitespace (compared against `f0726acb:strategy_ic_rules.cpp`).
- Six includes left the rule-table TU.

**(d) S-8.**
- `weights_v3` (`composition_engine_fit.py:725-749`) keeps the theme mapping only. The sign and theme refusal is gone.
- The C++ runner's `lift_walk_forward_candidates` is now its only home, tested by
  `WeightsV3.RefusalsPrecedeAnyPayloadOrOutput`. (R10) That test is in my IC filter run (18 / 18 on the build whose
  only mutation is in `fit.cpp`).
- No other Python copy appeared. `_expected_aim_v2` calls the existing `composition_rules` functions (D2's check-3
  pattern) and defines no G-P5 symbol.
- The mirror guard passes (first reviewer's run; mine in R10).
- "The fitter reads the runner's refusal" holds in this sense: the IC stage refuses such a file before any payload.
  The fitter itself publishes it unchecked. See S-e.

**Closed form, independently re-derived (first reviewer).**
- Inputs: gains a {1, 3}, b {2, 2, 4}; T = 2.
- Uncapped weights: 1/8, 3/8 / 1/8, 1/8, 1/4. The cap is 1/4.
- Pass 1 caps c1 (excess 1/8). Pro rata on b's base 1/2 this gives +1/32, +1/32, +1/16, so c4 = 5/16.
- Pass 2 caps c4 (excess 1/16). The only receiver is c0, so c0 = 3/16.
- Final weights: 3/16, 1/4, 5/32, 5/32, 1/4 (sum 1). Theme masses 7/16 and 9/16.
- These equal the test's constants. The test pins the contract with dyadic values, not the implementation; only its last
  check (equality with `theme_gain_weights` called directly) is an implementation cross-check.
- (R10) Under the equal split, a member's uncapped weight is 1/4 in theme a and 1/6 in theme b. So
  `parent_uncapped`, `parent_weights`, `parent_passes` and `theme_weight` all move. The test went red as this predicts.
  `ReadsNoFactorValue` stayed green: both of its fits share the mutation. `RefusalsBeforeAnyFit` stayed green: the
  refusals sit before line 340.

## D3WPB-Q2: the rules P9-W's parent names; TRAIN-fitted gains (first reviewer; unchanged in R10)

**Answer: no possible P9-W parent carries a TRAIN-fitted aim gain, so P9-W is unaffected by Q2. The prereg states it
in round 2b-ii.**

- **Templates.** `scripts/specs/p9/templates/` holds only the P9-H (`theme-cov-hedge.json`) and P9-M
  (`mom-volman.json`) cell templates. There is no P9-W template; P9-W runs as kind `walk-forward`.
- **The walk-forward kind fixture** names the parent `tiny.json` (tiny world, ew-theme-v1, Python fit). D3's fixture
  (pool-20 `699440bb`) says preflight refuses it there, so it is a test parent, not P9-W's.
- **The registered parent** is P9-F0h (prereg A.4, B.6; PRE-39).
  - With no accepted cell, it is the re-based Y-F0, `scripts/specs/v8/lib-v8ysb-gm.json`. Its fit flags:
    `--orientation prior --screen v4-prior-v1 --composition ew-theme-std-v1 --theme-erc theme-erc-v1`.
  - ew-theme-std-v1's scores are prior tier scores (after the v8 re-grades); there is no gain.
  - theme-erc-v1's covariance is refit on every row's window (`fit_walk_forward` runs `fit_composition` on the masked
    TRAIN view).
- **The cells before P9-W** can add theme-cov-hedge-v1 (P9-H), mom-volman-v1 (P9-M) and info-clock-rate-v1 (P9-C, a NAV
  rule).
  - P9-MV reads the risk model, so it is never P9-F0h.
  - None of these is an aim rule. The aim-gain rules are ew-theme-std-aim-v1, ic-shrink-aim-v1 and ew-theme-aim-v2, and
    the last is refused under `--walk-forward`.
- **Whole-TRAIN constants that do stay fixed across P9-W's rows** (not gains; for PRE's disclosure, not findings of this
  lane):
  1. The admitted member set (the `v4-prior-v1` screen runs once on the whole TRAIN window).
  2. mom-volman-v1's schedule, if P9-M is accepted. This is already the "Limit" line of B.6 (ALCOMB-4).
- **Wording note.** The aim-v2 refusal message gives "every row would be the full fit" as the reason
  (`fit.cpp:415-418`). That is also true of a plain ew-theme-std-v1 parent, which is accepted. The reason that separates
  aim-v2 is Q2's, the whole-TRAIN gains, which the message also names. No change is needed.

## Quality verdict (owner priority: modular / reusable / performant C++; the Python side shrinks)

**PASS.**

- **One fit entry point per mode.**
  - aim-v2 is one branch of the one `fit_composition`, sharing `theme_gain_weights` and `take_gain_parent` with the
    std rules.
  - No window or residual logic was added.
  - The walk-forward refusal sits in the one `fit_walk_forward`.
  - The CLI adds one branch.
  - (R10, my read of the hunks) Adding aim-v2 to `aim` also brings it under the existing mom-volman-on-aim refusal.
    That refusal is shadowed by the earlier aim-v2 modifier refusal, so the order is deterministic and the message is
    the more specific one.
- **Bounded allocation.** `theme_weight` holds at most 32 entries and `gains` at most 256. The hot path is untouched.
- **`/W4 /WX`.**
  - The lane's Debug build `p9-d3wpb-a` (source `703ac03a`, every C++ hunk) exited 0.
  - (R10) My teeth build `p9-d3wpb-c` compiled 341 TUs of HEAD's four targets with one mutated line in `fit.cpp`. It
    exited 0 under the same flags, and its build log has no warning line.
  - The deletion and the fix are Python only.
- **100 columns.** One added C++ line is over: S-a. (R10 scan of every added `.cpp` / `.hpp` / CMake line in
  `f0726acb..362793c6`: that one line, 101 columns, and no other.)
- **Python shrinks.**
  - Production Python is net about -100 lines: `composition_resid.py` -128 (+4 / -132), `composition_engine_fit.py`
    +30 (+51 / -21).
  - The tests grow, by the moved oracle.
  - No new rule copy.
  - G-P5 on the lane tree (counting rows retired by a P9 lane) drops from 34 functions in 13 rows to 32 in 12 with the
    deletion, and to 21 in 8 with the fix.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `scripts/tests/test_no_python_mirror.py:60`, `:64`, `:66`, `:69` | Required (R-1): **CLOSED at `90bc1962` (R10)** | Ruling D3WPB-Q1 says the remaining composition rules "stay on the G-P5 list with `retired_by: P10`". The four rows the deletion left named retiring lane `"D3"`, a P9 lane, so root's `P9_FREEZE = True` would have failed on them. | Done: one commit sets the four labels to `"P10"` and cites D3WPB-Q1 in the comments at `:56-57` and `:61-62`. Mirror guard in its own invocation: 7 passed, 21 / 8. On the dry-merged freeze copy, no composition row is listed. |
| `atx-engine/include/atx/engine/research/composition/mom_volman.hpp:3` | Suggested (S-a) | A comment line of 101 columns (house limit 100). | Reflow at the next touch. |
| `atx-impl/tests/strategy_ic_mom_volman_test.cpp:347-372` | Suggested (S-b) | The bespoke `ScratchDir` repeats what `atx-test-scratch` (`tests/support/process_scratch.cpp`) already does for every atx-impl test target (`atx-impl/tests/CMakeLists.txt:34`): it repoints TMP / TEMP per process before `main`. So S-9's collision could not happen on this target. The lane's own comment says so. It is harmless, but it is 25 lines of duplicated mechanism against the reuse priority. | Keep it, or drop it and rely on `atx-test-scratch` with a one-line comment. |
| `atx-impl/tools/test_fit_walk_forward.py` (no case) | Suggested (S-c) | No fitter-level test runs `--engine-fit --walk-forward` with ew-theme-aim-v2 and checks that the exe's refusal reaches the fitter's exit with nothing published. The pieces are covered separately: C++ `CliWrites...` (exit 1, no dir) and `test_run_exe_refusals` (the fitter on a failing exe). | A `RealExecutables` case at the next touch. |
| `composition_engine_fit.py:683-686` with `:480-497` | Suggested (S-d) | For aim-v2, ruling D2-1 ("write the C++ values; agreement under P12") becomes "the Python weights must equal the C++ bit for bit, else refuse". The reason: the fitter's rule takes no engine argument inside the fingerprint closure. This is stricter than D2-1. A one-ulp gap inside P12 on real data would refuse the `--engine-fit` fit. The risk is low: both sides use `theme_gain_weights` with the same expression order, and the bits are equal on the fixture. The lane documents it in the docstring. | None in P9. P10's document builder over `composition_fit.json` removes it. |
| `composition_engine_fit.py:725-749` (S-8) | Suggested (S-e) | That the C++ fit can never produce a row the runner refuses is argued in a docstring: members are +1, and every theme of a fit carries mass. No test pins it. If that invariant ever broke, the fitter would publish a v3 file that is refused only at the IC stage (fail-late) rather than at fit time as before. | A `WalkForward.*` gtest at the next C++ touch: for every row, each member with a row weight > 0 has a theme with full-fit mass > 0. |
| `atx-impl/src/strategy_ic_rules.cpp` (S-2 / S-4 / S-5 hunks) | Suggested (S-f), root's | The IC exe's source changes, though its behaviour does not (every message and byte is kept). `atx-research-composition` changes too. Under EXE-FREEZE, any relink after the wave-3 P9-B0 re-pin forces P9-B0 to be redone. | Root merges D3W-PB before the gate's P9-B0 re-pin, or not in P9 (see "What root must check"). |
| `scripts/tests/test_no_python_mirror.py:56-57` (the fix's comment hunk) vs root | Suggested (S-g), root's (R10) | Root's head `b182dfdd` carries B2's merge, which deletes the six B2 STATS rows and adds `# P9 B2 retired the statistics-of-record rows ...` directly above the old D2-3 comment. The fix rewrote that comment, so `git merge-tree --write-tree feat/platform-p9-20261003 362793c6` reports `CONFLICT (content)` in this file. It is the only conflict and comment-only; `460f5ba2` alone merges cleanly. The lane was told not to merge root (D3WPB-FIX1), so this is a root merge step, not a lane defect. | Keep-both at root's merge of `90bc1962`: root's B2 comment line, then the fix's two D3WPB-Q1 comment lines; drop the old two-line D2-3 comment; keep root's deletion of the B2 rows. Verified on a scratch copy: 7 passed, G-P5 1 function in 1 row (E2's `sign_pm7_35`). The resolved file differs from root's only in the lane's hunks (resid row gone, the comments, four `"P10"` labels). |

Count note (R10): the PARTIAL's header said "7 Suggested", but its table held six (S-a .. S-f). With S-g there are
seven.

## Evidence: own runs vs the lane's logs

All runs were in pool-23, or on `git archive` copies under the reviewer's scratch. Interpreter
`C:/Program Files/Python312/python.exe`, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, own `--basetemp` with its
parent created first. `git status --porcelain` was empty after each run.

### My runs (R10, fresh reviewer, HEAD `362793c6`)

- **R-1, own invocation.** `scripts/tests/test_no_python_mirror.py` on the pool-23 tree: **7 passed**, "G-P5: 21
  mirrored rule function(s) in 8 row(s) left for P9 lanes".
- **Dry merges.**
  - `git merge-tree --write-tree feat/platform-p9-20261003 362793c6` (root `b182dfdd`): one conflict, S-g.
  - The same with `460f5ba2`: clean (tree `14ebca2c`).
  - On archives (with the C++ sources the guard checks), with S-g resolved keep-both:
    - lane `362793c6`: 7 passed, 21 / 8;
    - merged tree: 7 passed, 1 / 1.
  - `P9_FREEZE = True` set in the scratch copies only: the lane copy lists the six B2 rows and the `wave_rules.py` rows
    (B2's and C3's are gone in root; E2's is not), and no composition row. The merged copy lists only `scripts/wave_rules.py ['sign_pm7_35'] (E2)`.
- **Host (HOST-4, then HOST-5).**
  - My wait tool polled every 60 s from 19:29. At 19:43 it adopted ruling HOST-5: builds of a directory outside
    `C:\atx-wt\` (the foreign `cmake --build C:\atx\build-vol`) are ignored, and only pool builds count.
  - It gave COV-MV's live waiter its turn (`p9-covmv-i`) and then waited out B2's `p9-b2-r` and one more pool build.
  - It reached two quiet polls at 20:21 (about 52 minutes; free RAM 4,000 MiB).
  - Then: configure `equity-dev` (exit 0), and research-build `-DryRun` "Admitted".
- **Teeth build `p9-d3wpb-c` (TEETH-RESTORE).** Tool:
  `.superpowers/sdd/platform-p9-20261003/d3wpb-review-logs/tools/rev-teeth-cpp.py`. Log: `rev-teeth-cpp.log` beside
  it. That directory is git-ignored and not committed, as the dispatch says.
  - **Before the mutation:** the tree was clean and `fit.cpp` hash-object = HEAD blob `1c3461a2`. The bytes were kept,
    raw SHA-256 `35d05dd5...c3a1`.
  - **Mutation:** exactly one occurrence at `fit.cpp:340`, `theme_gain_weights(gains, ...)` ->
    `theme_gain_weights(std::vector<f64>(gains.size(),1.0), ...)`. This replaces aim-v2's normalisation by the gains
    with the equal split.
  - **Build:** `scripts\research-build.ps1 -Tag p9-d3wpb-c -Targets
    "atx-engine-research-composition-tests,atx-research-composition,atx-research-admission,atx-impl-strategy-ic-tests"`.
    Exit 0. Receipt `build-equity/mega-p9-d3wpb-c-receipt.json`: Source `362793c6`, DirtyEntries 1, Jobs 3,
    416 s, 341 TUs, 16 links.
  - **Restore, in `finally`, BEFORE any test:** raw SHA-256 `35d05dd5...` EQUAL the kept bytes;
    `git status --porcelain` EMPTY; hash-object `1c3461a2` EQUAL the HEAD blob.
  - **Exes:** the SHA-256 of all four equal the receipt's: composition-tests `cbf41665...`, `atx-research-composition`
    `cbdf25a9...`, `atx-research-admission` `2a844dd9...`, ic-tests `1e9212c4...`. They are MUTATED: never evidence of
    the head (EXE-CUR).
  - **`AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*`** on the mutated composition-tests: exit 1; **21 ran,
    19 passed, 2 failed**: `AimV2.ClosedFormOnAPlantedModel` and `AimV2.CliWritesGainsAndThemeWeightsOnlyForIt`.
    Exactly the expected reds.
  - **`WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*`** on the mutated ic-tests:
    exit 0; **18 / 18**, as expected (no aim-v2 path).
  - **The five fitter pytest files** (`test_composition_engine_fit`, `test_composition_mom_volman`,
    `test_composition_resid`, `test_fit_composition_weights`, `test_fit_walk_forward`), with
    `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` = the mutated exes: exit 1; **1 failed, 180 passed,
    87 subtests passed**.
    - The one red is `SUBFAILED[aim-v2] test_composition_engine_fit.py::RealExecutables::test_real_executables_agree_and_are_written`.
    - It fails at `composition_engine_fit.check_fit` (check 3, `:570`), which refuses the `--engine-fit` fit ("the C++
      fit differs from the fitter's (16)").
    - This is exactly the expected result, as Python-side teeth evidence only.
    - The lane's clean real-exe count stays the evidence: 180 passed, 88 subtests (`pb-pytest-fitter-real.log`). In my
      run, 87 + the 1 red = 88.
    - The two walk-forward real-exe tests stayed green.
    - The 177 tests that need no exe ran on HEAD's Python and passed.
  - **End:** `git status --porcelain` EMPTY; `fit.cpp` = HEAD blob.
- **Object tree.** Its deletion was REFUSED by the permission classifier (see "Permission refusals"). Under PERM-1 it is
  not retried. `build-equity/` in pool-23 still holds the configure / build tree of `p9-d3wpb-c` (about 2.4 GB). Its
  `bin/` holds MUTATED exes, which are never evidence (EXE-CUR). Nobody may point `ATX_RESEARCH_*_EXE` at pool-23's bin.

### First reviewer's runs (R9, kept)

- **Fingerprints.** `d3w-logs/tools/d3w-fingerprints.py`, cwd each archive's `atx-impl/tools`, at `f0726acb`,
  `c044f5f6` and `460f5ba2`:
  - Only `composition_engine_fit` moves, `a6dd0da59c13` -> `b0052dc58356`, at the pre-deletion head.
  - The other 11 entries are equal at all three SHAs, `OUTPUT_PRODUCERS (fit, fit_prior)` `d28a30895ad1` and
    `composition_resid` `9c48f390ac6d` included.
  - The deletion moves nothing.
  - These equal the lane's `pb-fingerprints-pre-deletion.json` and `pb-fingerprints-deletion.json` exactly.
  - (R10) `90bc1962` touches no fingerprinted module.
- **Identity.** `pb-ident.py` at each of the three archives with the SAME work dir, then `cmp.py`:
  - `f0726acb` vs `c044f5f6`: 21 of 24 files byte-identical. The 3 `engine-*` `composition_weights.json` differ only at
    `$.provenance.engine_fit.producer_fingerprint`.
  - `c044f5f6` vs `460f5ba2`: 24 of 24 identical.
  - This reproduces `pb-identity-pre-deletion.log` and `pb-identity-deletion.log`.
- **The five fitter pytest files, without exes** (`RealExecutables` skipped): 177 passed, 3 skipped, 84 subtests, exit 0.
- **`test_no_python_mirror.py`, own invocation:** lane tree `460f5ba2` 7 passed, 32 / 12. Dry-merged root `73b4acfd` +
  `460f5ba2`: 7 passed, 14 / 6.
- **Dry merges.** `git merge-tree --write-tree` of root `73b4acfd` with `f0726acb`, `c044f5f6` and `460f5ba2`: all
  clean. `460f5ba2` with ABS `6a3f5043`, SIGN `9c360cde`, D3 `699440bb` and C3b `76b9f256`: all clean.

### From the lane's logs only

- Clean gtests: `AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*` 21 / 21 and
  `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` 18 / 18 (`pb-gt-a-*.log`, build
  `p9-d3wpb-a`, source `703ac03a`).
  - EXE-CUR: `git diff 703ac03a 362793c6` touches no C++ / CMake file (R10 check). It touches only
    `composition_resid.py`, `test_composition_resid.py`, `test_no_python_mirror.py` and records.
- Teeth T1-T4 (`pb-teeth-cpp.log`, build `p9-d3wpb-b`, MUTATED, restored, blob = HEAD).
- The five fitter pytest files with clean real exes: 180 / 0, 88 subtests (`pb-pytest-fitter-real.log`).

**Blind.** I read synthetic fixtures, specs (fit flags only) and logs. I opened no 2020-2023 output and nothing dated 2024
or later. No statistic is quoted here. The teeth failure message printed synthetic fixture weights; they are not quoted.

## What root must check at merge

1. **Order (MERGE-SHA).**
   1. Merge D3W `f0726acb` first, with its own review's checks: B2-EVAL-KEEP on `eval_report.cpp` (B2's PRE-15 hunks and
      D3W's `--vol` hunk are keep-both). Then run the eval gtests and the PRE-15 gtest.
   2. Then the pre-deletion head `c044f5f6`, with the report's TRAIN identity procedure:
      - fingerprints (step 1);
      - steps 2-3: `pb-ident.py` on BASE and on the merged tree with the SAME work dir, then `cmp.py`.
      - Expect 21 / 24, with the 3 `engine-*` weights files differing only at
        `provenance.engine_fit.producer_fingerprint`, and `OUTPUT_PRODUCERS` `d28a3089...`.
   3. Then the deletion `460f5ba2`. Re-run `test_composition_resid.py` and `test_no_python_mirror.py` (own invocation).
      Fingerprints and identity must be unchanged.
   4. Then the fix `90bc1962` (or the records head `362793c6`, which adds only lane records). This merge CONFLICTS in
      `scripts/tests/test_no_python_mirror.py` (S-g). Resolve it keep-both:
      - keep root's line `# P9 B2 retired the statistics-of-record rows (backtest_integrity.py, nav_summ.py, dsr_total.py): atx-research-eval.`;
      - follow it with the fix's two lines `# P9 ruling D2-3 moved the composition_*.py deletion to lane D3. Ruling D3WPB-Q1 keeps these rows through P9,`
        and `# retired in P10 (the D2-5 precedent for horizon_stats.py): the flag-absent fit still runs the Python rules.`;
      - drop the old two-line D2-3 comment, and keep root's deletion of the B2 rows.
      - Then run `test_no_python_mirror.py` in its own invocation. Expect 7 passed; on root `b182dfdd` the G-P5 count
        is 1 function in 1 row (E2's `sign_pm7_35`); it moves as SIGN lands.
2. **Targeted checks (ROOT-1a).**
   - Debug build of `atx-engine-research-composition-tests`, `atx-impl-strategy-ic-tests`, `atx-research-composition`
     and `atx-research-admission`.
   - Filters `AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*` (21) and
     `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` (18).
   - The five fitter pytest files with `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` set to ROOT's
     fresh exes, never pool-23's mutated bin. Expect 180 passed, 88 subtests, 0 failed.
3. **SQL2-CLS.** No new `atx.<name>/v<n>` literal. The added lines use the existing `atx.composition-fit-spec/v1` and
   `atx.dsl-composition-weights/v3`.
4. **Mirror guard at the freeze.** With R-1 closed, no composition row blocks `P9_FREEZE = True`. Only rows of lanes
   that have not retired theirs remain (on `b182dfdd` + this lane: E2's `sign_pm7_35`, SIGN's).
5. **EXE-FREEZE (S-f).** The IC exe and `atx-research-composition` relink with this lane. Merge before the gate's P9-B0
   re-pin, or not in P9.
6. **Stretch (D3W-PB).** Merges only if APPROVE at the gate, else P10 whole.
7. **Disk.** pool-23's `build-equity/` still holds the `p9-d3wpb-c` object tree, about 2.4 GB of MUTATED binaries. Its
   deletion by name was refused (below). It is regenerable and never evidence. The owner or root decides; it is never
   used as an exe source.

## Permission refusals

- R9 (first reviewer): none.
- R10: the deletion of the reviewer's own `p9-d3wpb-c` object tree by literal names in pool-23 was refused by the auto
  mode classifier ("Irreversible Local Destruction"). Not retried and not routed around (PERM-1). The exact command:
  `cd C:/atx-wt/pool-23/build-equity && rm -rf .ninja_deps .ninja_log CMakeCache.txt CMakeFiles CPackConfig.cmake CPackSourceConfig.cmake CTestTestfile.cmake atx-core atx-engine atx-impl atx-tsdb bin build.ninja cmake_install.cmake compile_commands.json lib spdlog.pc tests vcpkg-manifest-install.log && rtk proxy ls -a && du -sh . && cd .. && rtk proxy git status --porcelain && echo CLEAN`
  - Nothing was deleted (listing checked afterwards).
  - Kept intact: the tracked `audits/` and `v8-interim3-pitch-render-run2/`, and the `mega-p9-d3wpb-{a,b,c}-*`
    receipts and logs.
