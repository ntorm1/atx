# Handoff D2 (owner stop): composition rules on D1's rule table

| | |
|---|---|
| Pool | `C:/atx-wt/pool-20` (D1's pool, reused) |
| Branch | `feat/p9-d2-20261003` |
| Base | `1239a5ff` (W2-EARLY, D1 merge) |
| Head | `d61f73c2` `wip(D2): ...` (one WIP commit; this file is committed after it) |
| Contract | `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/wave2-lane-dispatch.md` (LANE = D2), brief `briefs/brief-D2.md`, carry `wave2-carry.md` (D2 section) |

Status: about 70% done. **No C++ has been compiled.** Lanes do not build C++, so every C++ file below is unbuilt and
untested. The WIP commit is one commit. It must be split into themed commits before merge, or root accepts it as one.

## Items: done / partial / not started

| Item | State | Where |
|---|---|---|
| Library `atx-engine-research-composition`: rules relocated, atx-impl files thin includes | done (unbuilt) | `atx-engine/{include,src}/…/research/composition/{ic_shrink,theme_erc,theme_tsmom,theme_resid,two_speed(hdr only),ew_theme_std,weights_schema,fit,composition_cli}.*`, `composition_main.cpp`; thin `atx-impl/src/strategy_ic_{shrink,theme_erc,theme_tsmom,theme_resid}.{hpp,cpp}`, `strategy_two_speed.hpp` |
| `atx-research-composition fit` (ic-shrink, ERC covariance, theme-resid order, tsmom sleeves, ew-theme-std tiers + member cap; CM 5/5b) | done (unbuilt) | `fit.cpp`, `composition_cli.cpp`. Summation order is pinned in `fit.hpp`: numpy pairwise (block 128, 8 accumulators) for sums and means; a sequential dot for the covariance (BLAS order cannot be reproduced). The cap iterates themes in ascending name order, which is Python's `sorted(excess)`. |
| gtests: ERC 2x2 closed form (F-15), equality with the Python fixture | written (never run) | `atx-engine/tests/research/research_composition_fit_test.cpp` (suite `ResearchComposition`, 10 TESTs); fixture `atx-engine/tests/fixtures/research_composition/fit_v1.json` from `atx-impl/tools/test_composition_fit_fixture.py` (pytest 4p; regen `ATX_D2_REGEN=1`) |
| D1-PSCHEMA: params_schema describes K-P9-9 manifest `params`; fixture re-pinned | done (unbuilt) | `strategy_ic_rules.cpp`: a closed object of the rule's registered constants (`const`, all optional, `additionalProperties:false`). `composition_rules_list.json` re-pinned: only params_schema changed. Generator script: `scratchpad/repin_list_rules.py` (not in repo). |
| P8 group_rerank helper | done (unbuilt) | `atx-engine/src/combine/marginal_rank_ic.cpp` `centred_tied_ranks` now calls `for_each_centered_rank`. Same sort and rank expression, so the bits are the same. |
| P19 `atx.dsl-composition-weights/v3` | done | `weights_schema.hpp` reserves v1/v2/v3. v3 is D3's, and the runner still refuses it. |
| CM-5: fitter SHA out of output bytes | done, pytests green | `fit_composition_weights.py`: `OUTPUT_PRODUCERS=("fit","fit_prior")`; `inputs`/`provenance`/`provenance.v6.inputs` carry `producer_fingerprint` (was `script_sha256`); `script_sha256` is printed only in `main`'s stdout summary. `composition_resid.parent_check` pops both keys (`FITTER_IDENTITY_KEYS`). Composition modules' `module_sha256` (whole-file) is **not** changed (decision below). |
| `--engine-fit` | code written, **no tests, never run** | new `atx-impl/tools/composition_engine_fit.py` (check_args, factor/admission/fit checks, `compare_values`, `expected_fit`, `verify`); hooked in `fit()` (check_args), `parse_args` (add_argument), `fit_prior` just before `files[OUTPUT_WEIGHTS]` (summary `engine_fit`; output bytes unchanged) |
| Carried D1 minors | two_speed refusal test: done. `--list-rules` anywhere in argv: done. Over-cap `--plan-only` reloadable plan: done. 13-themes pytests: **not started** | `strategy_ic_rules_test.cpp` (`CompositionRules.TwoSpeedRefusesAThemeOutsideTheTable`, `ParamsSchemaConstantsAreTheRegisteredOnes`, ListRulesJsonPinned cases); `strategy_ic_runner.cpp` + `strategy_ic_runner_test.cpp` (`IcAdmission.PlanPrintsRequiredBytesOverCap`) |
| `composition_*.py` deletion, separate prepared commit | **not started** | see "Next steps" |
| Report `task-D2-report.md` | **not started** | |

## Where I stopped, and the next steps in order

1. **`--engine-fit` tests** (I was about to write them): new `atx-impl/tools/test_composition_engine_fit.py`.
   - World: `tfw.aim_world()` with `extra["flip"].theme="value"` and `extra["medium_half"].tier="C+"`, `tfw.Fixture`, `cr.REGISTRY_PATH` patched to a missing file (the same world as `test_composition_resid.py:461`).
   - Factor directory: monkeypatch `composition_engine_fit.check_factor_series` so that it first writes the directory from the `FitterState` (factor.f64 decisions x candidates `<f8`, tau.f64, factor_h21.f64 NaN, and a manifest with schema `atx.factor-series/v1`, status complete, role_manifest_sha256, decisions, decision_sessions_ns, candidates[{id}], files.{name}.sha256), then calls the real check. `check_args` needs a placeholder `manifest.json` and existing exe paths.
   - Fake `run_exe` for the screen: Python `screen_v4` via `test_factor_series_admission.fitter_screen`/`table_rows` on the directory. For the fit: recompute with the Python composition modules from spec + directory, then `expected_fit`.
   - Cases: std + erc + tsmom, ic-shrink-v1 + tsmom, ew-theme-std-aim-v1 + resid.
   - Asserts:
     - The bytes are identical with and without the flag, and the summary shows `engine_fit.status == "agree"`.
     - Each of these is refused and nothing is published: a nudged weight (1e-9), a changed status, a nudged factor, a moved flat day.
     - `check_args` refusals: options without the flag, ew-theme-v1, train orientation, pooled.
     - `compare_values` / `compare_admission_csv` unit cases.
     - A real-exe test, skipped unless `ATX_RESEARCH_ADMISSION_EXE` and `ATX_RESEARCH_COMPOSITION_EXE` are set.
   - TRAP: v4_world's `fast_a` tier is `"A\u2212"` (Unicode minus). The C++ `parse_candidates` may not treat it as a grade ("tiers mix grades and integers"). Check this in the real-exe test.
2. Fix `atx-impl/tools/test_composition_theme_tsmom.py`: its HEADER path must point to `atx-engine/include/atx/engine/research/composition/theme_tsmom.hpp`. The thin atx-impl header no longer holds the constants, so this pytest **fails today**. Also grep the other pytests that read moved headers.
3. 13-themes minor: give `test_fit_composition_rule_plugins.py:157` and `test_composition_resid.py:268` a docstring line saying they pin today's registry until E2 (or retire them).
4. Run pytests with explicit paths: `test_fit_composition_weights{,_store,_pool}.py`, `test_composition_*.py`, `test_fit_composition_rule_plugins.py`, `test_composition_fit_fixture.py`, `test_factor_series_admission.py`, `scripts/tests/test_no_python_mirror.py`. Kill leftover python processes.
5. Re-read all new C++ for `/W4 /WX` hazards and the 100-column limit (not done for the latest files).
6. Split the WIP into themed commits: (a) C++ library + fit verb + gtests + CMake, (b) rule-table carry + P8, (c) Python CM-5 + `--engine-fit` + fixture pytest. Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
7. Prepared deletion, on a separate branch, not the lane head:
   - Retire the D2 ALLOWLIST rows of `scripts/tests/test_no_python_mirror.py`. Delete those functions and route the fitter through the C++ fit values (engine required for these compositions).
   - Not covered by the C++ fit today: `ew_theme_aim_v2`, `composition_resid.centred_tied_ranks`/`tie_block_means`, and `horizon_stats` (that module stays: it is in the factor-record key).
   - Say in the report what remains.
8. Write `task-D2-report.md` (contract format) and send the final message.

## Interfaces AL-COMB must code against (pool 19, rebases on D2)

- New composition kernels go in `atx/engine/research/composition/` (namespace `atx::engine::research::composition`). Group primitives go in `atx/engine/combine/`.
- atx-impl `strategy_ic_*.hpp` are thin includes: `#include` the engine header, then `using` declarations in `atx::impl::strategy`. The JSON parse functions (`ic_detail` `composition_schedule` / `composition_residualise`) stay in atx-impl `strategy_ic_*.cpp`.
- Rule rows are D1's table in `strategy_ic_rules.cpp`. A row's `params_schema` = `{"type":"object","additionalProperties":false,"properties":{<const per registered constant>}}`, with `no_params_schema` for rules without constants. Adding a row re-pins `composition_rules_list.json` (`--list-rules --json`).
- `combine::cap_across_groups(..., tolerance, std::vector<GroupCapPass>* trace = nullptr)`: optional trace; the arithmetic is unchanged.
- `IcShrinkFit` / `ThemeErcFit` gained `uncapped` + `passes`. The signatures take a trailing `bool trace=false`.
- Library target `atx-engine-research-composition` (alias `atx::engine-research-composition`), linked PUBLIC into `atx-impl-core`.

## Files touched

- Owned (new):
  - `atx-engine/include/atx/engine/research/composition/*`
  - `atx-engine/src/research/composition/*`
  - `atx-engine/tests/research/research_composition_fit_test.cpp`
  - `atx-engine/tests/fixtures/research_composition/fit_v1.json`
  - `atx-impl/tools/composition_engine_fit.py`
  - `atx-impl/tools/test_composition_fit_fixture.py`
- Owned (edited):
  - the thin `atx-impl/src/strategy_ic_{shrink,theme_erc,theme_tsmom,theme_resid}.*` and `strategy_two_speed.hpp`
  - `fit_composition_weights.py`
  - `composition_resid.py` (parent_check only)
  - `test_fit_composition_weights{,_store}.py`
  - `test_composition_resid.py`
- Cross-lane:
  - `atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt`, `atx-impl/CMakeLists.txt` (appended blocks)
  - `combine/group_shrink.hpp` (`GroupCapPass` trace)
  - `combine/marginal_rank_ic.cpp` (P8)
  - D1's `strategy_ic_rules.{cpp,hpp}`, `strategy_ic_runner.cpp`, `strategy_ic_rules_test.cpp`, `strategy_ic_runner_test.cpp`, `composition_rules_list.json`

## Tests and their last result

- pytest, after CM-5:
  - `test_fit_composition_weights.py`: 97 passed.
  - `test_fit_composition_weights_store.py` + `test_composition_resid.py`: 28 passed.
  - `test_composition_fit_fixture.py`: 4 passed (before CM-5; it does not depend on CM-5).
  - `--engine-fit` hook is in place after those runs; only `py_compile` has run since.
- New CM-5 tests:
  - `CacheInvalidation.test_cm5_script_sha_moves_no_output_byte`
  - the store suite's comment edit now asserts all output bytes are identical
  - `without_fitter_identity` normaliser in the git-history byte tests
  - the resid parent-check case with a pre-CM-5 parent
- gtests written, never built:
  - `ResearchComposition.*` (10)
  - `CompositionRules.ParamsSchemaConstantsAreTheRegisteredOnes`
  - `CompositionRules.TwoSpeedRefusesAThemeOutsideTheTable`
  - extended `ListRulesJsonPinned` and `IcAdmission.PlanPrintsRequiredBytesOverCap`
- Root must build:
  - targets: `atx-engine-research-composition`, `atx-research-composition`, `atx-engine-research-composition-tests`, `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`, `atx-impl-strategy-target-tests`, `atx-impl-tests`
  - filters: `ResearchComposition.*`, `CompositionRules.*`, `IcAdmission.*`, `IcShrinkV1.*`, `ThemeErcV1.*`, `ThemeTsmom.*`, `ThemeResid.*`, `TwoSpeed.*`, `StrategyIcRunner.*`, and the combine marginal-rank tests

## Decisions and why

- **`--engine-fit` writes the Python bytes and cross-checks.** It does not write the C++ values. B1's factor series differ from Python's by about 1e-16, BLAS dot order cannot be reproduced, and B1's admission meets P12 tolerance, not bytes. So C++ values cannot be byte-identical to X-5. Root's "X-5 fit byte-identical with the flag" holds by construction, and agreement goes in the stdout summary. Flipping to C++ values is the deletion slice: a ruled re-pin.
- **`--engine-fit` takes a precomputed `--engine-factors DIR`.** The fitter does not run the `factors` verb itself. Root already produces this directory for B1's comparator. Payload paths in signals.json must be absolute (an M1b slip).
- **CM-5 key is renamed to `producer_fingerprint`** rather than putting a fingerprint under `script_sha256`. The name must not lie. This moves every fit's bytes once (a ruled re-pin per DEC-20), and the PM7-30 list becomes empty afterwards.
- **Composition modules' `module_sha256` is left as a whole-file SHA.** The brief names only the fitter. These modules are what the deletion slice retires. Note: editing any `composition_*.py` moves the bytes of fits that embed it. `composition_resid.py` changed here, which affects `--theme-resid` fits only, and those move with CM-5 anyway.
- **params_schema = the rule's registered constants as `const`.** A manifest may restate them but not change them, and rules without constants get `{}`.

## Open questions for the PM

1. Is "flag writes the Python bytes; C++ agreement in the summary" acceptable for "fit byte-identical with `--engine-fit`"? Or do you want the C++ values written now (not byte-identical: P12 only)?
2. Should CM-5 also convert the composition modules' `module_sha256` to fingerprints, or leave them to the deletion slice?
3. Deletion scope: does the C++ fit need to cover `ew-theme-aim-v2` and the resid diagnostics before `composition_*.py` can go?

## Traps

- Writing files with Bash heredocs breaks under the hook (use Write/Edit). Python `read_text`+`write_bytes` drops CRLF; git autocrlf hides it.
- rtk compresses Bash output, so use `rtk proxy` for exact output. Use the Grep tool, not `grep \|`.
- `strategy_ic_shrink.cpp` / `strategy_ic_theme_erc.cpp` are stub TUs on purpose: the source lists were not edited.
- `ATX_TRY(decl, expr)` is not wrapped in do/while.
- Engine Debug builds need `/O2 /Ob2 /clang:-finline` on the composition target for the theme-resid hot loop. It is already in CMake.
- The fitter's `producer_fingerprint` is lru-cached by funcs tuple. Tests patch the module attribute, not the cache.
