# Task D2 report: composition fit in C++, `--engine-fit` (wave 2, slot 5)

| | |
|---|---|
| Outcome | DONE_WITH_CONCERNS |
| Pool / branch | `C:/atx-wt/pool-20`, `feat/p9-d2-20261003` |
| Base | `1239a5ff` (W2-EARLY) |
| Commits | `d61f73c2` WIP (prior session, unsplit), `a17319fb` handoff, `56661dc8` C++ fixes + params_schema + pair-cache re-pin, `5c844702` `--engine-fit` writes C++ values (D2-1) + module fingerprints (D2-2), `4b463071` 13-theme notes + G-P5 rows to D3 (D2-3), then this report |
| Contracts | K-P9-4a (reads B1's shipped factor bytes), K-P9-6 (`params_schema` per D1-PSCHEMA), P8, P19, CM-5, rulings D2-1, D2-2, D2-3, W2-BUILD |

## The WIP commit was not split

The handoff asked to split `d61f73c2` into three themed commits. No other ref contains it (`git for-each-ref
--contains`; pool-19 / AL-COMB is still on `1239a5ff`), but the permission system refused the history rewrite
(`git reset` on the lane branch). Per the dispatch ("a follow-up commit is fine"), every later change is a separate
themed commit on top. Root merges the WIP as one commit. If root wants it split, it splits it at merge time. The WIP's
file themes are: (a) the engine library, the fit verb, the gtests and CMake; (b) the rule-table carry and P8; (c) the
Python CM-5 change and `--engine-fit`.

## What each required item became

| Item | State | Where |
|---|---|---|
| C++ built and tested (W2-BUILD) | done | 4 research-build tags in pool-20 (below). The WIP C++ had exactly one compile error: `strategy_ic_rules_test.cpp` did not include the thin headers it reads constants from. |
| Red pytest (old header) | fixed | `test_composition_theme_tsmom.py` reads `atx-engine/include/atx/engine/research/composition/theme_tsmom.hpp`. No other pytest reads a moved header (grep). |
| `params_schema` (D1-PSCHEMA; E2 depends on it) | done | Each row is a closed object `{"rule": {"type": "string", "const": <id>}, <registered constants as typed const>}`, none required. E2's fixture `{name, params: {}}` validates. E2's `checked_params` fills in `rule` because the schema pins it. `composition_rules_list.json` re-pinned (only params_schema changed). gtests `TableCoversEveryRule`, `ParamsSchemaConstantsAreTheRegisteredOnes`, `ListRulesJsonPinned` pass. |
| D2-1: `--engine-fit` writes the C++ values | done | See "`--engine-fit`" below. |
| D2-2: CM-5 also for the composition modules | done | `module_sha256` (whole-file SHA) becomes `producer_fingerprint` in `provenance.{std,std_identity,ic_shrink,theme_erc,theme_tsmom,resid}`. It is `composition_rules.module_fingerprint`: the code_fingerprint AST closure of the block's writing function, plus, for each other composition module the closure reads (`code_fingerprint.cross_module_reads`), that module's fingerprint of the names read. It moves with code or constants and not with comments (test `ModuleFingerprints`). |
| D2-3: deletion needs C++ for ew-theme-aim-v2 and resid diagnostics | moved to D3 | Neither has a C++ path in D2. No deletion commit was prepared. The G-P5 rows D2 was to retire now name lane D3 (cross-lane edit, lane column only). |
| P8 (group_rerank helper) | done (WIP) | Bits unchanged, so the pair-cache source pin was re-pinned without a version bump (S1-PIN / S2-1 precedent): `strategy_marginal_pair_cache.cpp`, `MarginalIc.PairCacheSourcesPinned` passes. |
| P19 (`atx.dsl-composition-weights/v3`) | done (WIP) | `weights_schema.hpp` reserves v1/v2/v3; the runner refuses v3 (D3's). |
| CM-5 (fitter SHA out of output bytes) | done (WIP) | As in the handoff: `producer_fingerprint` replaces `script_sha256` in admission.json `inputs` and weights `provenance`; the summary prints `script_sha256`. |
| Carried D1 minors | all four done | two_speed refusal test, `--list-rules` only in argv[1..2], over-cap `--plan-only` (all WIP, now built and passing); the 13-theme pins carry a comment saying the first registered theme updates both tests in one commit. |

## `--engine-fit` (ruling D2-1)

`fit_prior` builds its composition and document in a local `compose(engine)`:

- Flag absent: one Python pass. The code path and the bytes are as before, apart from the ruled fingerprint keys.
- Flag present:
  1. The Python pass runs first.
  2. `composition_engine_fit.verify` runs the factor, admission and fit checks:
     - factor series: absolute 1e-12;
     - `admission.csv`: decisions byte for byte, floats under P12;
     - the fit: decisions exact, floats within 1e-12 x max(1, |x|).
  3. `kernels` maps `composition_fit.json` to the shapes the composition modules' new `engine` arguments take. These
     are `ew_theme_std`, `ic_shrink`, `theme_erc` and `schedule`:
     - ICs, shrunk ICs, shares, theme means, equal-share themes;
     - uncapped and capped weights, cap passes;
     - ERC shares, the covariance, contributions, dispersion;
     - tsmom parent masses, trailing sums, block masses and themes off.
  4. `compose(kernels)` runs again, so every derived number comes from the C++ values: theme tables, the blend
     diagnostic, weighted turnover, aim provenance and the tsmom block.
  5. `check_written` requires the written weights to equal the C++ final weights bit for bit.
  6. The agreement record goes into the summary and `provenance.engine_fit`.
- The admission table stays the Python screen's. B1's comparator holds it to P12.
- A gap larger than P12 refuses before anything is published.
- Supported (corrected in fix round 1, review R-2): the single-window prior path with ew-theme-std-v1 / -aim-v1 /
  ic-shrink-v1 / -aim-v1, each with or without `--theme-erc`, `--theme-tsmom`, `--theme-resid` (with or without
  `--theme-resid-parent`, either side fitted with or without the flag; ruling D2-R2) and `--two-speed`. Refused before
  any compute: ew-theme-v1, ew-theme-aim-v1, ew-theme-aim-v2 and ew-theme-v6 (no C++ fit), the train orientation,
  era pools, and the option flags without `--engine-fit`. Before fix round 1, `--theme-resid-parent` under the flag
  could not run (every child refused).
- Real executables on the fitter's synthetic world (aim_world; the factor directory is the fitter's own series):

  | Case | factor diff | admission diff | fit diff | Bytes vs flag absent |
  |---|---|---|---|---|
  | std+erc+tsmom | 0 | 2.26e-15 | 2.2e-16 | differ, 0 differences under P12 |
  | ic-shrink+tsmom | 0 | 2.26e-15 | 2.1e-16 | differ, 0 differences under P12 |
  | std-aim+resid | 0 | 2.26e-15 | 0 | equal |

  The bytes column compares the flag-present weights file, minus `provenance.engine_fit`, with the flag-absent file.

## Tests

- New `atx-impl/tools/test_composition_engine_fit.py`. Its stand-ins read what the executables read: the factor
  directory written from the fitter's state, admission.json as the candidates file, and the spec.
  - `EngineFitEndToEnd`: the stand-ins equal the Python bit for bit, so admission files are identical and the weights
    file is the flag-absent one plus `provenance.engine_fit` (3 cases). Each of these is refused and nothing is
    published:
    - a weight nudged by 1e-9;
    - a changed screen status;
    - a reversed resid order;
    - a nudged factor;
    - a moved flat day.
  - `EngineFitEndToEnd.test_a_cpp_value_within_p12_is_the_value_written`: a 1e-14 relative change of the C++ weights
    passes, and those weights are what is written, theme table included.
  - `CheckArgs` (refusals; the fitter refuses through FitError), `Comparisons` (`compare_values`,
    `compare_admission_csv`), `ModuleFingerprints` (D2-2).
  - `RealExecutables`: runs when `ATX_RESEARCH_ADMISSION_EXE` and `ATX_RESEARCH_COMPOSITION_EXE` are set; run here,
    1 passed.
- Changed: `test_composition_theme_tsmom.py` (header path; its schedule spy forwards `engine`).
- pytest, explicit paths, all exit 0:
  - The D2 set, with the real executables, 263 passed and 1 skipped (34 subtests): `test_fit_composition_weights{,_store,_pool}.py`, `test_composition_{engine_fit,fit_fixture,ic_shrink,resid,rules,theme_erc,theme_tsmom,two_speed}.py`, `test_fit_composition_rule_plugins.py`, `test_factor_series_admission.py` and `scripts/tests/test_no_python_mirror.py`.
  - The one skip is B1's exe test. It was then run with `ATX_EQUITY_TARGETS_EXE` set: `test_factor_series_admission.py` 8 passed.
- gtests (pool-20 build-equity, Debug):
  - `atx-engine-research-composition-tests`: 10/10.
  - `atx-impl-strategy-ic-tests`: 195/195. This includes CompositionRules.*, IcAdmission.*, IcShrinkV1.*, ThemeErcV1.*, ThemeTsmom.*, ThemeResid.*, MarginalIc.* and CombineMarginalRankIc.*.
  - `atx-impl-strategy-target-tests`: 325/325 (TwoSpeed.*).
  - `atx-engine-combine-tests`: 243/243 (GroupRerank.*).

## Builds (W2-BUILD; `scripts/research-build.ps1`, equity-dev, receipts `build-equity/mega-p9-d2-*-receipt.json`)

- `configure -Preset equity-dev` (pool-20 had no build-equity): exit 0, 427 s.
- The four tags:

  | Tag | Exit | Seconds | Targets |
  |---|---|---|---|
  | p9-d2-a | 0 | 178 | atx-engine-research-composition-tests, atx-research-composition |
  | p9-d2-b | 1 | 928 | the test-include compile error |
  | p9-d2-c | 0 | 54 | atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-engine-combine-tests |
  | p9-d2-d | 0 | 148 | the above, plus atx-impl-strategy-target-tests, atx-equity-strategy-targets, atx-research-admission |

- The C++ at HEAD is the C++ of p9-d2-d. Only Python changed after it.
- Not built by the lane: `atx-impl-tests`, the `*_test.cpp` glob of every atx-impl test. Every D2-touched test TU
  compiled in the focused targets above.

## How root verifies

- Targets:
  - `atx-engine-research-composition-tests`, `atx-research-composition`
  - `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`, `atx-impl-strategy-target-tests`
  - `atx-engine-combine-tests`, `atx-research-admission`, `atx-impl-tests`
- Anchored filters:
  - `ResearchComposition.*`, `CompositionRules.*`, `IcAdmission.*`, `IcShrinkV1.*`, `ThemeErcV1.*`, `ThemeTsmom.*`,
    `ThemeResid.*`
  - `MarginalIc.PairCacheSourcesPinned`, `StrategyIcRunner.*`, `TwoSpeed.*`, `CombineMarginalRankIc.*`,
    `GroupRerank.*`
- Identity, flag absent: re-run X-5's fit argv. Every byte must equal the X-5 reference apart from the ruled
  CM-5 / D2-2 keys:

  | File | Key now | Key in the reference |
  |---|---|---|
  | admission.json | `inputs.producer_fingerprint` | `inputs.script_sha256` |
  | weights | `provenance.producer_fingerprint` | `provenance.script_sha256` |
  | weights (v6 only) | `provenance.v6.inputs.producer_fingerprint` | `provenance.v6.inputs.script_sha256` |
  | weights | `provenance.<std,std_identity,ic_shrink,theme_erc,theme_tsmom,resid>.producer_fingerprint` | `module_sha256` |

  This is a one-time re-pin (DEC-20). After it, a fitter or module edit outside the producer closures moves no byte,
  so the PM7-30 substitution list becomes empty.
- Identity, flag present: the same argv plus `--engine-fit --engine-factors <B1 factors dir for the TRAIN role and
  library> --admission-exe build-equity/bin/atx-research-admission.exe --composition-exe
  build-equity/bin/atx-research-composition.exe`. Payload paths in signals.json are absolute (M1b). Expect:
  - exit 0 and summary `engine_fit.status == "agree"`;
  - admission.json and admission.csv byte-identical to the flag-absent run;
  - the weights file equal to the flag-absent file plus `provenance.engine_fit`, with every number within P12:
    `composition_engine_fit.compare_values(flag_absent_doc, doc_without_engine_fit) == []`.

  The two weights files are not byte-identical: ruling D2-1 writes the C++ values.

## Decisions

- **The C++ values are injected through explicit `engine=` arguments, not by rewriting the finished document.** The
  arguments are on the four Python rule functions, and their default `None` keeps the Python path byte for byte.
  Python then derives every report field from the C++ numbers, so the document stays internally consistent. These are
  the modules the D3 deletion retires; D3 replaces them with a document builder over `composition_fit.json`.
- **`provenance.engine_fit` is written into the weights file as well as the summary.** The document says which code
  produced its numbers. The key is present only with the flag.
- **`params_schema` also pins the row's own `rule` key.** It matches E2's `ic_table()` shape and the K-P9-6 convention
  that blocks name their rule. A manifest may restate the rule key and the constants, but not change them.
- **The module fingerprint includes cross-module reads.** ic-shrink and theme-erc read `composition_rules.member_cap`,
  `CAP_TOLERANCE` and `tier_weights`, and a whole-file SHA of their own module would not have moved with those.
- **The pair-cache source pin was re-pinned without a version bump.** P8 has the same sort and the same rank
  expression, so the pairs keep their bits (S2-1 precedent).

## Cross-lane edits

- `scripts/tests/test_no_python_mirror.py` (T1 guard): the lane column of six rows changes from D2 to D3 (ruling
  D2-3). No pair is added or removed.
- `atx-impl/src/strategy_marginal_pair_cache.cpp` (S2 area): the pinned digest and a comment.
- The WIP's cross-lane edits are as listed in the handoff:
  - CMake tails in `atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt` and `atx-impl/CMakeLists.txt`;
  - `combine/group_shrink.hpp`, `combine/marginal_rank_ic.cpp`;
  - D1's `strategy_ic_rules.*`, `strategy_ic_runner.cpp` and their tests.

## Merge requests for root

1. **E2 (slot 1, merges before D2).** `scripts/tests/test_wave_kinds.py:247-251` and `:311-312` assert that D1's
   interim block schemas refuse `{}`. Under D2's schemas the composition fixture is accepted, so after the D2 merge
   those asserts must flip, in E2's file:
   - `rule_flags([{"name": "ew-theme-std-v1"}], {"ic": ic_table(False)})` gives `[]`;
   - `checked_params("two-speed-v1", {}, row) == {"rule": "two-speed-v1"}` still holds;
   - the `new-combination-rule.json` wave passes the spec stage.
2. **AL-COMB.** It rebases on this head. Each new rule row needs a params_schema with `{"rule": {"type": "string",
   "const": <id>}}` plus its constants; the `ParamsSchemaConstantsAreTheRegisteredOnes` table gains a row; and
   `composition_rules_list.json` is re-pinned.
3. **SQL2-CLS literals for `classes.json`:** `atx.composition-fit-spec/v1`, `atx.composition-fit/v1`,
   `atx.composition-fit-fixture/v1` (test fixture), and the reserved `atx.dsl-composition-weights/v3`.

## Concerns / open risks

- **The 100-column limit is not held in the engine composition files.** 96 added C++ lines exceed 100 columns.
  - About 45 of them were relocated verbatim from atx-impl, whose idiom is about 120 columns.
  - The rest are in new files: `composition_cli.cpp` 23, `fit.cpp` 17, headers about 12, `ew_theme_std.cpp` 3,
    `research_composition_fit_test.cpp` 6.
  - All compile clean under `/W4 /WX`. I did not reformat them by hand (clang-format is not run by rule). This is a
    fix-round candidate.
- **horizon_stats.py's G-P5 row now names D3.** The handoff notes the module stays, because it is in the
  factor-record key. If it is meant to outlive P9, the PM may prefer P10.
- **A pre-D2 `--theme-resid-parent` is refused** by `parent_check`, because the module blocks renamed their identity
  key. This is fail-closed: refit the parent with the D2 fitter.
- **ic-shrink under `--engine-fit` writes C++ ICs.** The ICs are the C++ admission train_mean, while admission.json
  keeps the Python screen's. They are within P12 (2.3e-15 here). `provenance.engine_fit.written` states it.

## Carried minors

All four D1 minors are fixed (above).

## Ledger candidates

- P9 D2 `--engine-fit` on aim_world with the real executables: the C++ fit is within 1 ulp (2.2e-16 relative) of the
  Python for erc, tsmom and ic-shrink, and bit-equal for ew-theme-std-aim plus resid. The factor series are exact.
  The admission floats are within 2.3e-15.
- Configuring equity-dev in a reused pool without build-equity took 427 s; the first composition-target build took
  178 s on a warm ccache.

## Disk (DISK / DISK-2)

- Done after the report commit. The lane deleted the `build-equity` entries its configure and builds created, by
  name: `atx-core`, `atx-engine`, `atx-impl`, `atx-tsdb`, `bin`, `lib`, `tests`, `CMakeFiles` and the generated
  top-level files.
- Kept:
  - the `mega-p9-d2-{a,b,c,d}-{receipt.json,build.log,cache-stats.log}` files and `vcpkg-manifest-install.log`;
  - the two directories that predate this session, `audits/` and `v8-interim3-pitch-render-run2/`.
- Evidence logs (gtest and pytest output, build stdout) were moved to `evidence-D2/` beside this report. They are
  untracked and git-ignored.
- The lane's scratchpad files and the pool's `__pycache__` directories were deleted. Root rebuilds pool-20 from
  configure if it builds there.

## Fix round 1 (review `61a808c6`: R-1, R-2, S-1..S-8; rulings D2-R2, D2-S4, D2-5..D2-8)

| | |
|---|---|
| Outcome | DONE for the fix round; the lane stays DONE_WITH_CONCERNS only for the P10 notes below |
| Commits | `9c09b694` WIP fix round (owner stop), `57df8298` handoff, then this commit (report, one comment reflow, one whitespace fix) |
| Build | `p9-d2-e` (below); no rebuild in the resumed session |
| Not split | `9c09b694` stays one commit (no history rewrite, as D2-4); the themes are listed per item below |

### Required

| # | State | What changed | Evidence |
|---|---|---|---|
| R-1 | fixed | `strategy_marginal_pair_cache.cpp`: `pair_sources` gains `atx-engine/include/atx/engine/combine/group_rerank.hpp` (3 paths). Re-pinned by the recipe with no version bump (bits unchanged; S2-1 precedent): `df589a889927852b86815920e6ed59a37ea0b44d026d9ffb97ae73f2aa37c7bc`, equal to the reviewer's independent recompute. `MarginalIc.PairCacheSourcesPinned` asserts 3 sources and that the last is `group_rerank.hpp`; the test comment no longer claims "no include closure". | `evidence-D2/fr1-gtest-pair-cache-pin.log` (1/1), `fr1-gtest-strategy-ic.log` (195/195) |
| R-2 | fixed (ruling D2-R2: a flag-aware check, not an up-front refusal) | `composition_resid.parent_check(..., engine_fit=False) -> bool`. Engine mode applies when the re-fit runs `--engine-fit` or the parent carries `provenance.engine_fit`. In that mode `provenance.engine_fit` and the ic-shrink annotations `ic_source` / `ic_admission_max_abs_diff` (D2-S4) are set aside on both sides, and the rest is compared by `composition_engine_fit.compare_values(parent, child)`: decisions, keys, lengths and strings exactly; floats to 1e-12 x max(1, abs(parent)) (ruling D2-8: the parent is the reference). The admission tables stay exact, as without the flag. The refusal names `--engine-fit on {the parent / this re-fit}` plus the count and paths of the differing values. Engine mode also writes `provenance.resid.parent_check_engine_fit` (the comparison the file passed). With the flag absent on both sides, the path, the message and the `PARENT_CHECK` text are unchanged (byte-exact check as before). | `test_composition_engine_fit.py::EngineFitResidParent`, 5 tests: engine parent + engine child; engine parent + plain child; plain parent + engine child (the C++-valued second pass passes and its values are written); plain + plain byte-exact as before; a fitter change (ic-shrink intensity 0.25) refused in the Python pass before any exe runs or any output is written |

### Suggested

| # | State | What changed / reason |
|---|---|---|
| S-1 | fixed | `compare_admission_csv`: every row must have the header's length (no `zip` truncation). A non-finite cell must match the other side's text exactly. An unparsable float cell counts as a difference, and an empty file as a shape difference. Not tested: `Comparisons.test_compare_admission_csv` was not changed in this round and covers none of these branches (re-review S-b; the behaviour was confirmed by the re-reviewer's direct probe). The test is carried to D3 as a required item (ruling D2-9). [Corrected by root at merge, M2d.] |
| S-2 | fixed | `_read` / `_json` turn a missing or non-JSON executable output into the refusal. `run_exe` turns `TimeoutExpired` / `OSError` into the refusal. `kernels` wraps `_kernels` and turns Key/Type/Value/Index/AttributeError into the refusal. The screen manifest must be an object and its `counts` a dict. |
| S-3 | fixed | `provenance.engine_fit` (and the summary) gain `admission_exe_sha256`, `composition_exe_sha256` and `producer_fingerprint` = `module_fingerprint(composition_engine_fit, ("verify", "kernels", "check_written"))`. Test `test_provenance_names_the_producing_code`. |
| S-4 | fixed (ruling D2-S4) | Under the flag, `provenance.ic_shrink` gets `ic_source = IC_SOURCE_ENGINE` (the ICs are the C++ fit's; admission.json keeps the Python train_mean) and `ic_admission_max_abs_diff`. `kernels` refuses a C++ IC further than P12 from the admission train_mean. `admission.json` is unchanged. Test `test_ic_shrink_states_its_ics_are_the_cpp_values` (a 1e-9 shift is refused). |
| S-5 | fixed | `parent_check` refuses a parent whose provenance sub-block carries `module_sha256` (a pre-D2 parent): "fitted before P9 lane D2: its provenance blocks [...] name their code by module_sha256, which CM-5 / ruling D2-2 replaced by producer_fingerprint ...; refit the parent cell with this fitter, then re-run this cell against the new parent file". The new `"pre-d2"` case in `test_composition_resid.py::...test_a_re_fit_that_is_not_the_parent_cells_file_is_refused` builds a real pre-D2 shape (`std.module_sha256`, `script_sha256` in the weights and the admission, a recomputed `admission_sha256`). It passes with no fixture change. Texts: the `parent_check` docstring and the `--theme-resid-parent` help now name `producer_fingerprint`. `PARENT_CHECK` is not changed: it is the v8 rule's registered text and an output string (`provenance.resid.parent_check`), so changing it would move flag-absent R-11 bytes. The code comment beside `FITTER_IDENTITY_KEYS` states the equivalence. |
| S-6 | partly fixed; the rest is **not in P9 (ruling D2-7, P10 note)** | Done: the `attach*` / `apply` writers joined `PRODUCERS` (ic_shrink, theme_erc, theme_tsmom: `attach`; resid: `attach`, `apply`; rules: `attach_std`), and `composition_engine_fit` joined `LOCAL_MODULES`. These additions move the `producer_fingerprint` values; still only the four ruled CM-5 / D2-2 keys differ from the X-5 reference. Not done: the fitter-wide cross-module fingerprint (`OUTPUT_PRODUCERS` hashes imports, not cross-module reads; finding FD-2). It changes the store identity shape under pinned records, and the per-module `producer_fingerprint` (D2-2) covers the stale-module risk. **P10 note:** a cross-module edit (for example to `era_pool`, `composition_two_speed` or `composition_engine_fit`) that changes a fit without touching a fingerprinted closure would reuse a stale record; none is known. Not a regression: the old whole-file `script_sha256` had the same gap. |
| S-7 | done | (a) gtest logs kept: `evidence-D2/fr1-gtest-{research-composition,strategy-ic,strategy-target,combine,pair-cache-pin}.log`. (b) `RealExecutables` ran here with both exe env vars set: it passed (inside the 264 below; no skip). The root step is added below. |
| S-8 | fixed (report only) | `fit_max_pure_rel_diff_by_block` (abs(a-b)/abs(a) over finite non-zero floats, per block: parent, theme_erc, theme_tsmom, weights) is in the summary and in `provenance.engine_fit`. The tolerance is still the ruled P12. |

### Ordered items

- D2-5: the horizon_stats row of `scripts/tests/test_no_python_mirror.py` names `P10`, with a comment giving the reason (the module is in the factor-record key; a named G-P5 exception). The guard now prints "34 mirrored rule function(s) in 13 row(s) left for P9 lanes" (was 37 in 14).
- D2-6: no line over 100 columns remains in the new engine files (`composition_cli.{hpp,cpp}`, `ew_theme_std.{hpp,cpp}`, `fit.{hpp,cpp}`, `weights_schema.hpp`, `research_composition_fit_test.cpp`) or in D2's new lines of `strategy_ic_rules*.cpp`, `strategy_ic_runner.cpp` and `strategy_ic_theme_tsmom.hpp`. One slip was found and fixed in the resumed session: a 112-column comment at `strategy_ic_theme_tsmom.hpp:5`, reflowed after build p9-d2-e (comment only; no code token changed). The remaining long added lines are all in the moved files (`ic_shrink`, `theme_erc`, `theme_resid`, `theme_tsmom` .hpp / .cpp; 34 lines), left as D2-6 rules.
- D2-4: the WIP commits stay unsplit.
- Slip: `SUPPORTED =(` became `SUPPORTED = (` in `composition_engine_fit.py` (whitespace only; the AST is unchanged, so no fingerprint moves).

### Build and tests (fix round 1)

- Build `p9-d2-e` (`scripts/research-build.ps1`, equity-dev): exit 0, 783 s, 363 TUs, 18 links, Jobs 2.
  - Receipt `build-equity/mega-p9-d2-e-receipt.json`. Source: `61a808c6` plus 14 dirty entries, which are exactly commit `9c09b694`. Every C++ file of `9c09b694` was last written before the build started.
  - Targets: `atx-engine-research-composition-tests`, `atx-research-composition`, `atx-research-admission`, `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`, `atx-equity-strategy-targets`, `atx-impl-strategy-target-tests`, `atx-engine-combine-tests`.
  - Exe SHA-256 prefixes: composition `a074ca53`, admission `bf7e7142`, targets `dcca3362`.
- gtests, on the real exes of p9-d2-e:
  - research-composition 10/10;
  - strategy-ic 195/195, including `MarginalIc.PairCacheSourcesPinned`;
  - strategy-target 325/325;
  - combine 243/243 (the CHECK lines in its log are death tests).
- pytest: explicit paths, `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`. `ATX_RESEARCH_ADMISSION_EXE`, `ATX_RESEARCH_COMPOSITION_EXE` and `ATX_EQUITY_TARGETS_EXE` were set to the p9-d2-e exes.
  - `atx-impl/tools`: **264 passed, 0 skipped, 39 subtests passed** (184 s; `evidence-D2/fr1-pytest-tools.log`).
    - Files: `test_fit_composition_weights{,_store,_pool}.py`, `test_composition_{engine_fit,fit_fixture,ic_shrink,resid,rules,theme_erc,theme_tsmom,two_speed}.py`, `test_fit_composition_rule_plugins.py`, `test_factor_series_admission.py`.
    - Per file, in that order: 97, 5, 40, 20, 4, 17, 23, 13, 15, 11, 3, 8, 8.
  - `scripts/tests/test_no_python_mirror.py`, in a separate invocation: **7 passed** (`evidence-D2/fr1-pytest-no-python-mirror.log`).
  - No red. No expected value was edited.
- `ruff --select F --line-length 120` and an AST parse are clean on the changed Python.

### Root merge items (restated, with fix-round changes)

1. **E2 flip** (E2 merges first). `scripts/tests/test_wave_kinds.py:247-251` and `:311-312` assert that D1's interim schemas refuse `{}`. The line numbers are at E2's `5ca50ef5`; re-locate them at E2's final head. Under D2's closed `params_schema` the composition fixture is accepted, so these asserts flip in E2's file:
   - `rule_flags([{"name": "ew-theme-std-v1"}], {"ic": ic_table(False)}) == []`;
   - `checked_params("two-speed-v1", {}, row) == {"rule": "two-speed-v1"}` still holds;
   - `new-combination-rule.json` passes the spec stage.
2. **Refit check for pre-D2 `--theme-resid-parent` files.** A parent weights file fitted before D2 (a provenance block with `module_sha256`) is now refused with the S-5 message.
   - Root checks whether any P9 R-11 cell's parent (the `scripts/specs/v8/r11.json` derivation) points at such a file.
   - If one does, refit the parent cell's argv with the merged fitter (0 trials, no statistic read). The child's `--theme-resid-parent-sha256` pin is then the new file's SHA-256.
   - R-11 cells under `--engine-fit` can run after the merge (R-2 fixed). The review's "do not run until R-2 is fixed" lifts once root's merge verification passes.
3. Build the targets in "How root verifies" plus `atx-impl-tests` (never built by the lane), and run the anchored filters. `MarginalIc.PairCacheSourcesPinned` must pass with 3 sources.
4. **S-7:** run the following and read 20 passed, no skip (`RealExecutables` included):
   `ATX_RESEARCH_ADMISSION_EXE=<bin>/atx-research-admission.exe ATX_RESEARCH_COMPOSITION_EXE=<bin>/atx-research-composition.exe python -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_engine_fit.py`
5. X-5 identity:
   - Flag absent: only the four ruled CM-5 / D2-2 keys differ (table above).
   - Flag present: as above. `provenance.engine_fit` now also carries the exe SHA-256s, `producer_fingerprint` and `fit_max_pure_rel_diff_by_block`.
   - With an ic-shrink composition, `provenance.ic_shrink` also carries `ic_source` / `ic_admission_max_abs_diff`.
   - With a resid parent in engine mode, `provenance.resid.parent_check_engine_fit` is added.
6. AL-COMB rebases on this head (P17 / ALCOMB-5) with closed `params_schema` rows. The SQL2-CLS literals are as in "Merge requests" item 3.

### Open notes (P10)

- S-6: the fitter-wide cross-module fingerprint (ruling D2-7).
- horizon_stats.py retirement (ruling D2-5; a named G-P5 exception).

### Disk (fix round 1)

Deleted by name right after this commit, in `C:/atx-wt/pool-20/build-equity` (about 2.9 GB):

- directories `atx-core`, `atx-engine`, `atx-impl`, `atx-tsdb`, `bin`, `lib`, `tests`, `CMakeFiles`;
- files `CMakeCache.txt`, `build.ninja`, `.ninja_deps`, `.ninja_log`, `cmake_install.cmake`, `CTestTestfile.cmake`, `compile_commands.json`, `CPackConfig.cmake`, `CPackSourceConfig.cmake`, `spdlog.pc`.

Kept: `mega-p9-d2-{a..e}-*`, `vcpkg-manifest-install.log`, `audits/`, `v8-interim3-pitch-render-run2/`. Also deleted: the pool's `__pycache__` directories and this session's two pytest basetemp directories. Root rebuilds pool-20 from configure if it builds there.
