# Task D2 re-review 1: fix round 1 (R-1, R-2, S-1..S-8, D2-5..D2-8)

| | |
|---|---|
| Lane / pool | D2, `C:/atx-wt/pool-20`, `feat/p9-d2-20261003` |
| Reviewed | `9fad6907..49af0a52` (WIP `9c09b694`, handoff `57df8298`, report `49af0a52`); lane base `1239a5ff` |
| Prior review | `61a808c6` (CHANGES_REQUIRED: R-1, R-2, S-1..S-8) |
| Read | resume4-dispatch, wave2-review-dispatch, review-template, task-D2-review, task-D2-report "Fix round 1", `evidence-D2/fr1-*`, build receipt `mega-p9-d2-e-receipt.json`, ledger lines for D2 (D2-1..D2-8, D2-R2, D2-S4), the full round diff |
| Built | nothing. Every question was settled by reading, by synthetic Python probes and by recomputing the pin. |
| Real data | none opened. No trial, no ledger append. Probes ran on the synthetic `aim_world` only, in my scratchpad, deleted after the run. |

**Verdict: APPROVE** (0 Required, 5 Suggested).

- Spec: compliant. R-1 and R-2 are fixed as ruled (D2-R2, D2-8). D2-S4, D2-5 and D2-7 are met. D2-6 is met except for 3 lines (S-a).
- Quality: high. The flag-absent outputs were diffed against `9fad6907`: they move only in the ruled `producer_fingerprint` values. The exe evidence holds.
- The Suggested items are a report claim of a test that was never written (S-1), an S-2 residual, a type-level widening in the engine-mode parent check, 3 long lines, and a stale message.

## (a) R-1: pair-cache source pin

**Fixed.**

- `atx-impl/src/strategy_marginal_pair_cache.cpp:38-43`: `pair_sources` now lists 3 paths, the third being `atx-engine/include/atx/engine/combine/group_rerank.hpp`.
- I recomputed the digest from git blobs at `49af0a52` with the recipe (path, normalised length, CRLF to LF text):
  - 3 sources give `df589a88...37c7bc`, equal to the pin. The `9c09b694` blobs and the working tree give the same value.
  - Dropping `group_rerank.hpp` gives `7b9a6dd8...`, the old pin. So the bits are unchanged and no version bump is needed (S2-1 precedent).
- No other header carries pair bits:
  - `group_rerank.hpp` includes only std headers and `core/{error,types}.hpp`.
  - `pair_value` / `centred_tied_ranks` call nothing from `orthogonalize.hpp` or `hac.hpp`.
  - The caller `strategy_marginal_ic.cpp` was not touched by the lane.
  - `marginal_pair_cache_identity()` is read only by the test, so no output byte moves.
- The test has teeth. `atx-impl/tests/strategy_marginal_ic_test.cpp:704-724` fails in each of these cases:
  - a source is dropped: `ASSERT_EQ(size, 3U)` fails and the digest no longer matches;
  - the sources are reordered or replaced: `EXPECT_EQ(sources.back(), "…group_rerank.hpp")` fails;
  - `group_rerank.hpp` is edited without a re-pin: the digest fails.
- Evidence: `fr1-gtest-pair-cache-pin.log` (1/1) and `fr1-gtest-strategy-ic.log` (195/195).

## (b) R-2: `--engine-fit` with `--theme-resid --theme-resid-parent`

**Fixed, and the acceptance is no wider than ruling D2-R2 allows,** apart from one type-level detail (S-c).

How the code works:

- `composition_resid.parent_check` (`:194-258`) runs in engine mode iff the re-fit runs `--engine-fit` or the parent carries `provenance.engine_fit`.
- It runs in both fitter passes: `fit_composition_weights.py:2609` inside `compose()`, which is called at `:2615` and again at `:2640`. So the pass that is written, the C++-valued second pass, is the one checked.
- In engine mode the code:
  - sets aside `provenance.engine_fit` and the two D2-S4 annotations;
  - keeps the admission tables exact (`:236-238`, unchanged);
  - compares the rest with `composition_engine_fit.compare_values(mother, child)`. The parent is `want`, so the P12 reference is the parent value, as D2-8 rules.

Synthetic probe: an engine-fitted ic-shrink parent, edited and then re-pinned, with a re-fit child run with and without `--engine-fit`.

| parent edit | engine child | plain child | as ruled? |
|---|---|---|---|
| `provenance.ic_shrink.producer_fingerprint` changed (a rule-module identity) | REFUSED, path named | REFUSED | yes |
| one weight x (1+1e-11), beyond P12 | REFUSED `$.weights.flip`, "--engine-fit on the parent and this re-fit (ruling D2-R2)" | REFUSED | yes |
| one weight x (1+5e-13), inside P12 | accepted | accepted | yes (D2-R2 / D2-8) |
| `provenance.engine_fit` record edited (exe SHA) | accepted | accepted | yes: the excluded key |
| top-level `provenance.producer_fingerprint` changed | accepted | accepted | yes: the v8 PARENT_CHECK allows it with the flag absent too, so not new |
| an extra provenance key | REFUSED (keys) | REFUSED | yes |
| `provenance.window.turnover_transitions` 5 → 5.0 | accepted | accepted | **wider than "integers exactly"** (S-c) |
| `ic_admission_max_abs_diff` set to 0.5 | accepted | accepted | set aside wholesale. Unreachable from the fitter, which refuses above P12 (S-c) |

Further checks:

- **Pre-D2 parent.** The stale check (`:220-224`) runs before the mode is chosen, so a pre-D2 parent is refused in both modes. The message names the cause (CM-5 / D2-2) and the remedy (refit). The test builds a real pre-D2 shape (`test_composition_resid.py:667-681`).
- **`module_sha256`.** No post-D2 writer emits this key (grep of `atx-impl/tools`), so no valid parent is refused by the stale check. Every rule that can be a resid parent carries a module block before D2: `STANDARDISE_RULES` = ew-theme-std, ic-shrink, ic-shrink-aim.
- **Decisions byte-equal.** In engine mode, strings, bools, keys, list lengths and orders are exact. Members with weight > 0 are listed by name in `theme_standardise.themes`, so a weight flipping between 0 and non-zero is refused. Tsmom masses or `themes_off` that flip are refused. The one gap is that an int compared with an equal float passes (S-c).
- **Refusal before any exe.** A fitter change is refused in the Python pass with `calls == []` (`test_a_fitter_change_is_still_refused_naming_the_cause`). The probe agrees: every refusal had `calls=[]`.
- **Flag absent on both sides.** The path, the message and the `PARENT_CHECK` text are unchanged. `parent_check_engine_fit` is written only in engine mode, as the tests and the flag-absent diff below show.

## (c) The eight Suggested items

| # | claim | state |
|---|---|---|
| S-1 | `compare_admission_csv`: row length against the header, non-finite cells exact, an unparsable cell is a difference, an empty file is a shape difference | **Code fixed, test claim false.** My direct probe:<br>- NaN vs finite: refused;<br>- NaN vs NaN with the same text: passes;<br>- 5 cells under a 4-column header: refused;<br>- `abc`: refused;<br>- empty: refused;<br>- NaN on line 2 after a finite line 1: refused.<br>The report says "Tested in `Comparisons`" (`task-D2-report.md:250`), but `Comparisons.test_compare_admission_csv` (`test_composition_engine_fit.py:601-609`) was not changed in the round (no hunk) and covers none of these branches. See S-b. |
| S-2 | `_read` / `_json` / `run_exe` / `kernels` turn failures into the refusal | **Fixed for what it names.** My probe: `TimeoutExpired`, `OSError` (missing exe), a missing file, invalid JSON and a malformed document in `kernels` each raise the `error` class. No test was added and none was claimed. Residual: `check_factor_series` (`composition_engine_fit.py:147-148,161-162`) still reads the factor directory unguarded. That directory is also an executable's output (the `factors` verb). See S-d. |
| S-3 | `admission_exe_sha256`, `composition_exe_sha256`, `producer_fingerprint` of `("verify","kernels","check_written")` | **Fixed.** `composition_engine_fit.py:484-487`, `:497-500`. All three names exist. `composition_engine_fit` joined `LOCAL_MODULES`. Test `test_provenance_names_the_producing_code`. |
| S-4 / D2-S4 | under the flag the weights file states that the ic-shrink ICs are the C++ values, records max \|C++ − admission train_mean\| ≤ P12, and leaves `admission.json` unchanged | **Fixed as ruled.** `ic_shrink()` writes the engine ICs into `members[].ic` and `member_ic` (`composition_ic_shrink.py:161-164`). It sets `ic_source = IC_SOURCE_ENGINE` and `ic_admission_max_abs_diff` (`:207-209`) only when `engine is not None`. `kernels` refuses a value above P12 (`composition_engine_fit.py:440-444`). The test asserts that `admission.json` is byte-equal to the flag-absent one. The registered `RULE_TEXT` still reads "ic_k = the admission row's train_mean"; it is the rule's definition, `ic_source` qualifies it, and the gap is at most 1e-12. Acceptable. |
| S-5 | pre-D2 parent refused with cause and remedy; real pre-D2 shape tested; texts | **Fixed.** `PARENT_CHECK` was not changed, and the reason holds: it is an output string (`provenance.resid.parent_check`), so changing it would add a fifth X-5 key. See the PM note. |
| S-6 | `attach*` / `apply` joined `PRODUCERS`; fitter-wide cross-module fingerprint out of P9 | **Fixed in part. The rest is ruled out (D2-7).** Every listed name exists: `attach_std`, three `attach`, and resid `attach` / `apply`. The resid fingerprint now moves with `P12_TOLERANCE` (test `:654-659`). |
| S-7 | gtest logs kept; `RealExecutables` ran | **Done.** `fr1-gtest-*.log` passes 10 / 195 / 325 / 243 / 1. `fr1-pytest-tools.log` reads "264 passed, 39 subtests", with no skip printed. Collect-only confirms 20 tests in `test_composition_engine_fit.py`, which matches the root step. |
| S-8 | `fit_max_pure_rel_diff_by_block` (report only) | **Fixed.** `pure_relative` (`:371-381`) is deterministic: `max` over a set is order-free. It is written to the summary and to `provenance.engine_fit` only. |

Ordered items:

- **D2-5:** done. The `horizon_stats.py` row names `P10` and gives its reason. The guard prints "34 in 13".
- **D2-6:** not fully met (S-a).
- **D2-7:** honoured.
- **D2-8:** `compare_values(mother, child)`, as checked in (b).

## (d) Flag-absent identity

Method: I ran the flag-absent fitter on the synthetic `aim_world` with the `9fad6907` tools tree (`git archive`) and with the `49af0a52` tree, then diffed every output file. Eight runs: std, shrink, shrink-erc, std-erc-tsmom, shrink-tsmom, std-aim-resid, and resid children of a shrink parent and of a std parent.

Results:

- `admission.json` and `admission.csv` are identical in all 8 runs.
- `composition_weights.json` differs only in `provenance.{std,ic_shrink,theme_erc,theme_tsmom,resid}.producer_fingerprint`. These are the ruled CM-5 / D2-2 keys; they move because S-6 added the `attach*` / `apply` writers.
- In the two resid children, `provenance.resid.parent_weights_sha256` also differs, as a direct consequence of the parent's fingerprint values.
- No key was added or removed.
- The top-level `provenance.producer_fingerprint` is unchanged.
- `ic_source` and `parent_check` are unchanged, and `parent_check_engine_fit` is absent.

On the C++ side, the round changed only:

- formatting;
- the `member_row_shape` refactor (`composition_cli.cpp:191-197`, used at `:218`), checked to be the De Morgan equivalent of the old condition with the same short-circuit order;
- `pair_sources`, an identity read only by its test.

So the IC runner and composition exe outputs are unchanged. The X-5 key set against the base is unchanged (the four ruled keys). Root's X-5 must take the fingerprint values from the merged head, not from any earlier D2 head.

## (e) Exe freshness

| fact | value |
|---|---|
| `p9-d2-e` receipt | source `61a808c6` + 14 dirty entries. The receipt was written at 22:29:03.5 (2026-10-03) after 782.8 s of wall time, so the build started at about **22:16:00** |
| `git log %cI` | `9c09b694` at 22:43:48 and `49af0a52` at 08:01:14 (10-04). **Both commit dates are after the exe.** |
| working-tree mtimes of the 13 code-bearing C++ files of the round | 22:01:41 to 22:15:10, all **before** the build start. The 14 C++ files of `9c09b694` match the 14 dirty entries. Every Python file of the round was written after 22:16:00. |
| the only C++ write after the build | `atx-impl/src/strategy_ic_theme_tsmom.hpp`, at 07:53:37 (10-04). It is a comment-only reflow: `9c09b694..49af0a52` touches comment lines 5-7 and no token. |
| source pins over round-touched C++ files | none. `ic_result_sources` and `dsl_vm_sources` list none of them, and grep finds no other pin. |

Conclusion:

- The literal commit-date test fails, but commit date is not edit time. The mtimes show the exe compiled the code content of `9c09b694`. The only file that differs is `strategy_ic_theme_tsmom.hpp`, and only in comments.
- So **no test rests on a stale exe semantically.**
- Strictly, `atx-impl-strategy-ic-tests` (195) and `atx-equity-strategy-ic` were built from a tree 3 comment lines away from `49af0a52`.
- The gtests ran at 22:29-22:33, before `9c09b694` was committed. The pytest run with the real exes was at 07:40, before the whitespace-only `SUPPORTED = (` fix at 07:56, which leaves the AST unchanged.
- The receipt hashes no dirty content, so the mtime argument is the strongest evidence available. Root rebuilds at merge (below).

## (f) Look-ahead, expected values, mirrors, seal

- **Look-ahead:** no fit window or decision range was touched. `fit.cpp` and `composition_cli.cpp` hunks are formatting and the equivalent refactor. `train_mask` is unchanged apart from a reflow.
- **Expected values:** none edited to make a test pass.
  - `pair_sources_sha256` is the required re-pin, recomputed above.
  - The `test_no_python_mirror` lane cell is the D2-5 ruling.
  - `without_engine_fit` asserts the D2-S4 annotations exactly before it strips them.
- **Python mirrors:** none added. The comparators and `pure_relative` are not rules. The G-P5 guard passes at 34 / 13.
- **Seal:** nothing dated 2024-01-01 or later was opened. All fixtures are synthetic.

## Required

None.

## Suggested

| # | where | problem | fix |
|---|---|---|---|
| S-a | `atx-engine/src/research/composition/ic_shrink.cpp:21` (105 cols), `atx-engine/include/atx/engine/research/composition/theme_erc.hpp:44` (102), `atx-engine/src/research/composition/theme_erc.cpp:25` (110) | D2-6 asks for lines over 100 columns in new engine files to be fixed, leaving only lines moved unchanged. These 3 lines sit in moved files but are D2's own: the added `bool trace` parameter and the traced `uncapped` member. None is verbatim at base, so reflowing them hides no move. The other 31 long lines in the moved files are verbatim at base. The report (`:262`) exempts the moved files whole. | Reflow the 3 lines (whitespace only, no token change). Root may do it at merge as a logged slip. |
| S-b | `task-D2-report.md:250`; `atx-impl/tools/test_composition_engine_fit.py:601-609` | The report says S-1 is "Tested in `Comparisons`", but no S-1 test exists. The NaN, row-length, unparsable and empty branches are unexercised. The behaviour is right (my probe above). No S-2 test exists either; none was claimed. | Add to `test_compare_admission_csv`: NaN vs finite refused; NaN vs NaN with equal text passes; an extra cell refused; a non-numeric float cell refused; an empty reference refused. Add one `run_exe` test for `TimeoutExpired` / `OSError` (patched). Correct the report line. |
| S-c | `atx-impl/tools/composition_engine_fit.py:344-355`, used by `composition_resid.py:252`; text `composition_resid.py:95-99` | In engine mode an int and an equal float pass: the probe accepted `turnover_transitions` 5 vs 5.0. `PARENT_CHECK_ENGINE_FIT` says "integers … exactly", and the flag-absent path refuses this. Both files are Python canonical JSON, so a type flip always means a code change; no decision value can move. Also, the D2-S4 annotations are set aside wholesale, so a parent's `ic_admission_max_abs_diff` is never checked. It is unreachable from the fitter, which refuses above P12. | Give `compare_values` a strict-types option for `parent_check` (an int against a float is a difference), keeping `check_fit` unchanged. Optionally require a present parent annotation to be `IC_SOURCE_ENGINE` and at most P12. |
| S-d | `composition_engine_fit.py:147-148,161-162` (`check_factor_series`) | An S-2 residual. A missing `factor.f64` / `tau.f64`, a non-JSON `manifest.json`, or a non-dict candidate row in the `--engine-factors` directory raises a raw `OSError`, `JSONDecodeError` or `AttributeError` instead of the refusal. The run fails closed and nothing is published. | Route the three reads through `_read` / `_json` and require each `candidates` entry to be a dict. |
| S-e | `composition_resid.py:237-238` | The admission refusal still says "beyond inputs.script_sha256". After CM-5 the key set aside is `inputs.producer_fingerprint` (or `script_sha256`). The message is not an output byte, so it can change freely. | Reword to "beyond the fitter identity (inputs.producer_fingerprint / script_sha256)". |

## PM note (no ruling needed for merge)

`PARENT_CHECK` (`composition_resid.py:83-85`) is written into every R-11 weights file. It still says the file "equals the parent … apart from provenance.script_sha256". Post-D2 files carry `producer_fingerprint` there, so the text is not literally true. The lane kept it to hold flag-absent X-5 identity (reason valid), and the code comment beside `FITTER_IDENTITY_KEYS` documents the equivalence. If the D2-S4 principle ("provenance text must be literally true") is to cover this text as well, the change is a fifth ruled X-5 key: a P10 candidate.

## Evidence (this re-review)

```
cd C:/atx-wt/pool-20; PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider \
  --basetemp=<scratch>/bt1 atx-impl/tools/test_composition_engine_fit.py atx-impl/tools/test_composition_resid.py \
  atx-impl/tools/test_composition_fit_fixture.py atx-impl/tools/test_composition_rules.py
59 passed, 1 skipped, 36 subtests passed in 73.97s        exit=0   (skip: RealExecutables, no exes)

... -m pytest -q -p no:cacheprovider --basetemp=<scratch>/bt3 atx-impl/tools/test_fit_composition_weights.py \
  atx-impl/tools/test_fit_composition_weights_store.py atx-impl/tools/test_fit_composition_weights_pool.py \
  atx-impl/tools/test_composition_ic_shrink.py atx-impl/tools/test_composition_theme_erc.py \
  atx-impl/tools/test_composition_theme_tsmom.py atx-impl/tools/test_composition_two_speed.py \
  atx-impl/tools/test_fit_composition_rule_plugins.py atx-impl/tools/test_factor_series_admission.py
203 passed, 1 skipped in 224.49s                           exit=0

... -m pytest -q -p no:cacheprovider --basetemp=<scratch>/bt2 scripts/tests/test_no_python_mirror.py   (separate)
G-P5: 34 mirrored rule function(s) in 13 row(s) left for P9 lanes
7 passed in 4.86s                                          exit=0
```

Together these runs cover all 264 lane tests at `49af0a52` (262 pass, 2 skip without exes). The other evidence:

- the pin recompute;
- the D2-R2 boundary probe (table in (b));
- the S-1 / S-2 direct probe;
- the old-vs-new flag-absent diff in (d).

All of these ran from scratchpad scripts on synthetic data and were deleted after the run.

## What root must check at merge

1. **Rebuild** (the lane's tree is deleted): `atx-engine-research-composition-tests`, `atx-research-composition`, `atx-research-admission`, `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`, `atx-equity-strategy-targets`, `atx-impl-strategy-target-tests`, `atx-engine-combine-tests`, plus `atx-impl-tests`, which the lane never built.
   - Re-run ResearchComposition (10), strategy-ic (195), strategy-target (325) and combine (243).
   - `MarginalIc.PairCacheSourcesPinned` must pass with 3 sources.
   - Strictly, the strategy-ic suite last ran on an exe 3 comment lines away from `49af0a52` (`strategy_ic_theme_tsmom.hpp`; section (e)).
2. Run the S-7 step: both exe env vars set, `test_composition_engine_fit.py` gives 20 passed, no skip.
3. **X-5:**
   - Flag absent, against the base: only the four ruled CM-5 / D2-2 keys differ, with the fingerprint values of the merged head. Section (d) shows the values move between D2 heads.
   - Flag present: `admission.*` is byte-equal. The weights equal the flag-absent weights under P12, plus `provenance.engine_fit`, plus with ic-shrink the D2-S4 annotations, plus in engine-mode resid `provenance.resid.parent_check_engine_fit`.
4. Run the R-11 refit check from the report (item 2): any P9 R-11 parent fitted before D2 is refused and must be refit (0 trials). R-11 under `--engine-fit` is unblocked once item 1 passes.
5. S-a: reflow the 3 long lines (whitespace only), or leave them with a logged PM note.
6. Carry the earlier merge items unchanged:
   - the E2 `test_wave_kinds.py` flip;
   - the SQL2-CLS literals;
   - the AL-COMB rebase on this head (P17 / ALCOMB-5).
