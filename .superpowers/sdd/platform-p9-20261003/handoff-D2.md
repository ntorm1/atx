# Handoff D2 (owner stop, fix round 1): composition fit in C++ (`--engine-fit`)

| | |
|---|---|
| Pool | `C:/atx-wt/pool-20` |
| Branch | `feat/p9-d2-20261003` |
| Head | `9c09b694` `wip(d2): fix round 1 ...` (this file is committed after it) |
| Review | `task-D2-review.md` (commit `61a808c6`): CHANGES_REQUIRED, 2 Required + 8 Suggested |
| Rulings | `progress.md` D2-R2 (resid parent under `--engine-fit`), D2-S4 (ic-shrink IC provenance), D2-5, D2-6 |

Status: about 85% done. All fix-round code is written and committed in one WIP commit. The C++ side is built and its
gtests pass. On the Python side only `test_composition_engine_fit.py` has been run. The rest of the pytest set has not
been run, and neither has the new S-5 test. The "Fix round 1" report section has not been written.

## Items

| Item | State | Where |
|---|---|---|
| R-1 pair_sources pin | Done, built, tested | `atx-impl/src/strategy_marginal_pair_cache.cpp` adds `combine/group_rerank.hpp` as the third source. The pin is now `df589a889927852b86815920e6ed59a37ea0b44d026d9ffb97ae73f2aa37c7bc`, with no version bump (P8). `strategy_marginal_ic_test.cpp` `PairCacheSourcesPinned` asserts 3 sources and the last path. It passes. |
| R-2 `parent_check` (D2-R2) | Done. Engine-fit pytests pass (5 new tests in `EngineFitResidParent`). `test_composition_resid.py` not re-run | `composition_resid.parent_check(..., engine_fit=False) -> bool`. Engine mode applies when the flag is set or the parent has `provenance.engine_fit`. In that mode it pops `engine_fit` and the ic_shrink `ic_source` / `ic_admission_max_abs_diff`, then runs `composition_engine_fit.compare_values` under P12. The admission comparison stays exact. Engine mode also writes `provenance.resid.parent_check_engine_fit`. The flag-absent path, its message and the `PARENT_CHECK` text are unchanged. The refusal names `--engine-fit on {the parent and/or this re-fit}` and the count and paths of the differing values. All 4 combinations are tested, plus a fitter change (`shrunk_shares` intensity 0.25) that is refused before any exe runs or any output is written. |
| S-4 (D2-S4) | Done, tested (engine-fit pytest) | `composition_ic_shrink.py`: in engine mode the provenance gets `ic_source = IC_SOURCE_ENGINE` and `ic_admission_max_abs_diff`. `composition_engine_fit.kernels` computes max abs(C++ IC − admission train mean) and refuses anything above P12. `admission.json` is unchanged. |
| S-5 pre-D2 parent message | Code done. Test written, **not run** | `composition_resid.parent_check` refuses any parent whose provenance sub-block carries `module_sha256`. The message gives the cause (CM-5 / D2-2 producer_fingerprint) and the remedy (refit the parent with this fitter, then re-run). The test is the new `"pre-d2"` case in `test_composition_resid.py::...test_a_re_fit_that_is_not_the_parent_cells_file_is_refused`. It builds the parent from `parent` with `std.module_sha256`, `script_sha256` and a recomputed `admission_sha256`. Watch for the assumptions that the parent's rule block is named `std` and that admission `inputs` has `producer_fingerprint`. Fix the fixture if either is wrong. |
| D2-6 line length | Done, built | Engine composition files (`composition_cli.{hpp,cpp}`, `ew_theme_std.{hpp,cpp}`, `fit.{hpp,cpp}`, `weights_schema.hpp`, `research_composition_fit_test.cpp`), plus the new `strategy_ic_rules*.cpp`, `strategy_ic_runner.cpp:815` and `strategy_ic_theme_tsmom.hpp` lines. No lines over 100 columns remain in new files. Moved files (ic_shrink, theme_erc/resid/tsmom) are left alone per D2-6. |
| D2-5 | Done | `scripts/tests/test_no_python_mirror.py`: the horizon_stats row lane is now `P10`. Not re-run. |
| S-1 admission CSV | Done, tested | `compare_admission_csv`: a row must match the header length, non-finite values must match as text, and a parse error counts as a difference. |
| S-2 guarded reads | Done, tested | `_read` / `_json` helpers. `run_exe` catches TimeoutExpired / OSError. `kernels` wraps `_kernels` and turns Key/Type/Value/Index/AttributeError into a refusal. |
| S-3 provenance | Done, tested | The verify summary gains `admission_exe_sha256`, `composition_exe_sha256` and `producer_fingerprint`. |
| S-6 | Partial (attach writers only) | The `PRODUCERS` of ic_shrink, theme_erc, theme_tsmom and resid now include `attach`. rules adds `attach_std`, and `composition_engine_fit` was added to `LOCAL_MODULES`. **Not done, by choice:** a fitter-wide cross-module fingerprint. It would change the CM-5 / store identity shape, and the review says it is not a regression. List this in the report. |
| S-7 | Root step | The gtest evidence logs are saved (below). Add RealExecutables to the report's "How root verifies" section. |
| S-8 | Done, tested | `fit_max_pure_rel_diff_by_block` covers parent, theme_erc, theme_tsmom and weights. |
| Report support list (R-2) | Code docstring done. Report **not updated** | The `composition_engine_fit` docstring now says: resid supported with or without `--theme-resid-parent`; ew-theme-v1, aim-v1, aim-v2 and v6 refused. Copy that into `task-D2-report.md`. |

## Builds and tests run

- **Build p9-d2-e** via `scripts/research-build.ps1`: exit 0, 783 s, 363 TUs, Jobs 2. Receipt and logs are `build-equity/mega-p9-d2-e-{receipt.json,build.log,cache-stats.log}`; the wrapper log is `evidence-D2/d2-build-e.log`.
- **gtests**, real exes, run on the WIP code. Logs: `evidence-D2/fr1-gtest-*.log`.
  - research-composition: 10/10
  - strategy-ic: 195/195, including `PairCacheSourcesPinned`
  - strategy-target: 325/325
  - combine: 243/243 (the CHECK lines in its log are death tests)
- **pytest** `atx-impl/tools/test_composition_engine_fit.py`: 19 passed, 1 skipped, 18 subtests. This run used no real-exe env vars, so the skip is probably the real-exe case.
- py_compile and `ruff --select F --line-length 120` are clean on all changed Python.
- `atx-impl-tests` was not built; that is root's job.

## Exact next steps

1. In `C:\atx-wt\pool-20\atx-impl\tools`, set the env vars:
   - `ATX_RESEARCH_ADMISSION_EXE=C:\atx-wt\pool-20\build-equity\bin\atx-research-admission.exe`
   - `ATX_RESEARCH_COMPOSITION_EXE=...\atx-research-composition.exe`
   - `ATX_EQUITY_TARGETS_EXE=...\atx-equity-strategy-targets.exe`

   Then run pytest with explicit paths only (PY-HYG):
   - `test_fit_composition_weights.py`, `test_fit_composition_weights_store.py`, `test_fit_composition_weights_pool.py`
   - `test_composition_engine_fit.py`, `test_composition_fit_fixture.py`, `test_composition_ic_shrink.py`, `test_composition_resid.py`, `test_composition_rules.py`, `test_composition_theme_erc.py`, `test_composition_theme_tsmom.py`, `test_composition_two_speed.py`
   - `test_fit_composition_rule_plugins.py`, `test_factor_series_admission.py`
   - `C:\atx-wt\pool-20\scripts\tests\test_no_python_mirror.py`

   Save the logs to `evidence-D2/fr1-pytest-*.log` and fix any failures. The likely failure points are the S-5 fixture (step 2) and any test pinning `PRODUCERS` or fingerprint values, because the `attach` additions move those fingerprints.
2. If the S-5 case fails, fix the fixture to match the real parent's block names, not the message.
3. Optionally split `9c09b694` into themed commits: C++ R-1 + D2-6; Python R-2/S-*; D2-5. A follow-up commit is fine; do not rewrite history.
4. Append a "Fix round 1" section to `task-D2-report.md` covering:
   - each item from the table above
   - the S-6 fitter-wide fingerprint and S-7, which are not done, with reasons
   - the corrected support list
   - the p9-d2-e receipt
   - the gtest and pytest counts

   Commit it with `git add -f` and the Opus 5.5 trailer.
5. DISK: after the report commit, delete by name from `C:/atx-wt/pool-20/build-equity` (2.8 GB):
   - `atx-core`, `atx-engine`, `atx-impl`, `atx-tsdb`, `bin`, `lib`, `tests`, `CMakeFiles`
   - `CMakeCache.txt`, `build.ninja`, `.ninja_deps`, `.ninja_log`
   - `cmake_install.cmake`, `CTestTestfile.cmake`, `compile_commands.json`
   - `CPackConfig.cmake`, `CPackSourceConfig.cmake`, `spdlog.pc`

   Keep `mega-p9-d2-*`, `vcpkg-manifest-install.log`, `audits/` and `v8-interim3-pitch-render-run2/`. Also delete the pool-20 `__pycache__` dirs. The tree is kept for now only so step 1 can use the real exes without a 13-minute rebuild. If disk is critical, delete it now and rebuild with `research-build.ps1 -Tag p9-d2-f` before step 1.

## Open questions

- S-6: is the fitter-wide cross-module fingerprint wanted in P9? It changes the store identity shape, which is why it is deferred here.
- R-2 / D2-R2: in engine mode, the parent comparison uses the parent as the P12 reference (`max(1, abs(parent value))`). Confirm this matches the ruling's intent.
