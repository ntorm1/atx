# Lane W0-O1 report: integrate lane 6, equity-bench preset, red test

## Outcome

- **Outcome:** DONE (one acceptance item is DEFERRED-to-gate by the brief's own instruction; see the table).
- **Branch:** `feat/w0-o1-l6`. **Final SHA:** the commit that adds this report (the head of the branch; the code commit is `c339ead5`).
- **Base:** `458d0bef480a624e258070c9d45174a9984466bf` (W0 base; `git merge --no-ff feat/w0-integration` said "Already up to date").
- **Pool:** `C:\atx-wt\pool-7` (run id `aes-w0-o1`).

## Commits on the lane branch

| SHA | What |
|---|---|
| `1de5814f` | `w0-o1: merge lane 6 (feat/qps-l6-optim@1cf59cb7)`. Clean `--no-ff` merge, no conflicts. 22 files, +5608/-33. |
| `c339ead5` | `w0-o1: equity-bench preset, Nightly gate, stod smoke fix`. |
| (this commit) | `w0-o1: lane report`. |

## Files changed (beyond the lane-6 merge)

| File | Change |
|---|---|
| `CMakePresets.json` | New configure preset `equity-bench` and a matching build preset. The working-tree file keeps CRLF (223 CRLF, 0 bare LF, checked after the edit); the committed blob is LF-only (`git ls-files --eol`: `i/lf w/crlf`; blob 15184 bytes vs 15407 bytes on disk = 223 CRs stripped). |
| `atx-engine/tests/risk_qp_augment_test.cpp` | The Nightly gate now takes `ATX_NIGHTLY` (repo-wide) or `ATX_RISK_NIGHTLY` (lane 6). The decision is a pure `constexpr` function of the two raw values. The Nightly battery is moved into `nightly_battery()` with no change to its cases. New suites `RiskNightlyGate_*`. |
| `atx-impl/tests/stage_run_synthetic_smoke_test.cpp` | The stod fix. The report kvs are now audited instead of being fed blindly to `std::stod` (see root cause below). |
| `atx-impl/tests/w0o1_report_kv.hpp` (new, header-only, test-only) | A strict, non-throwing kv audit: `parse_numeric_kv` (`std::from_chars`, the whole string must parse), the report's identity key set, and `audit_report_kvs`. |
| `atx-impl/tests/w0o1_stage_run_smoke_malformed_test.cpp` (new) | The regression suites `ImplStageRunSmokeMalformed_*`, which feed the malformed inputs in directly. |
| `atx-engine/bench/optimizer_production_bench.cpp` (lane-6 file) | Comment only: the build recipe now points at `equity-bench`, and a SCOPE note says every mode solves a problem with no costs and no turnover (R-14). |

No `CMakeLists.txt` edits. No new `src/` files. No product (`src/`) code changed.

## Root cause: `StageRunSyntheticSmoke` "invalid stod argument"

- **Reproduced** on the unfixed tree (after the lane-6 merge):
  `build-equity\bin\atx-impl-tests.exe --gtest_filter=StageRunSyntheticSmoke.*` printed
  `unknown file: error: C++ exception with description "invalid stod argument" thrown in the test body.`
  followed by `[  FAILED  ] StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard (1216 ms)`. The other 2 tests passed. Exit code 1.
- **Cause:** `run_report` (`atx-impl/src/stage_report.cpp`, `run_report_impl`, `sr.kvs = {...}`) emits two identity kvs first: `research_artifact_id` and `books_artifact_id`. Their value is a 64-hex artifact id, or the word `"unknown"` when the panel is unidentified. The smoke fixture sets `allow_unidentified_panels = true`, so both values are `"unknown"`. The test (written 2026-07-04 in `3fc8fa21`) ran `std::stod(v)` on **every** report kv. The identity kvs came later (they are first visible in `092314d4`, "equity alpha platform through checkpoint 22"). `std::stod("unknown")` throws `std::invalid_argument`, and the exception aborted the test body before any finiteness check could run.
- **Confirmation:** the fixed test asserts that the audit finds no failures, that exactly the 2 identity kvs are present and both are `"unknown"`, that every other kv (≥ 12) parses completely and is finite, and that numeric + identity = all kvs. It passes. So the identity kvs were the only non-numeric values.
- **Fix site:** the test only, `atx-impl/tests/stage_run_synthetic_smoke_test.cpp`. The identity kvs are an intended part of the report contract, so the stage is correct and the test's "every kv is a number" assumption was wrong. The fix does not weaken the test. Every numeric kv is still checked for finiteness, the check is now also strict (`"1.5x"` or `"nan"` would fail where `stod` accepted the first and only `isfinite` caught the second), the identity kvs are checked as well-formed ids, and a malformed value becomes a named test failure instead of an exception. The two other `std::stod` calls in the test (`book_turnover_per_day`, `total_pnl_borrow`) also go through the strict parser now.
- **Regression test:** `ImplStageRunSmokeMalformed_*` (6 tests). It pins the mechanism (`std::stod("unknown")` and `std::stod(<hex id>)` throw `std::invalid_argument`) and checks that the audit handles `"unknown"`, `""`, `"1.5x"`, `" 1.0"`, `"1,5"`, `"--1"`, a hex id, `"0x1p3"`, `"nan"`, `"inf"`, a misspelt identity and an uppercase non-64 id without throwing and with one named failure each.

## Acceptance table

| # | Plan accept item | Test(s) / evidence | Measured result | Status |
|---|---|---|---|---|
| 1 | `atx-engine-risk-tests` green, Nightly skipped | whole exe `build-equity\bin\atx-engine-risk-tests.exe --gtest_brief=1`; anchored ctest `^(RiskNightlyGate_\|RiskQpAugment)` | 451 tests from 59 suites: **450 passed, 1 skipped** (`RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`), 0 failed, exit 0, 123.4 s. ctest: 13/13 passed ("100% tests passed"), `447 - RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery (Skipped)`, exit 0. | MET |
| 1a | Move `RiskQpAugment.MatchesDenseOracleAcrossBattery` to Nightly | `RiskNightlyGate_Decision.*` (2), `RiskNightlyGate_Env.*` (2), `RiskNightlyGate_Registry.NightlyAndFastBatteriesAreRegistered`, `RiskNightlyGate_Battery.ContainsEveryPreSplitCase` | All 6 pass. All 11 pre-split cases (copied verbatim from base `458d0bef`) are in the Nightly battery with the same seed and iteration budget. With neither switch set, `nightly_enabled()` is false. `"1"` on either switch turns it on; `"0"`, `""` or unset leaves it off. The fast battery (`RiskQpAugmentFast`, 20 cases, M ≤ 30) runs by default: passed, 44.7 s Debug on a loaded host. Switch-on run (reviewer, `ATX_NIGHTLY=1`): Nightly battery PASS, 1364 s Debug; see Integration note 6 and "Fix pass 1". | MET |
| 2 | `atx-impl-tests` 521/521 | whole exe `build-equity\bin\atx-impl-tests.exe --gtest_brief=1`, run from the ctest working dir `build-equity\atx-impl\tests` | 533 tests from 101 suites: **527 passed, 6 skipped, 0 failed**, exit 0, 309 s. Of the 533 tests, 527 come from the base suite (the plan's "521" is an older count) and 6 are new. Every test that runs passes, including `StageRunSyntheticSmoke` 3/3. The 6 skips are pre-existing opt-in gates (5 need real data, `ATX_ALPHA101_PANEL` / `ATX_L10_FUNDZOO_OUT`; 1 is `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies`, "not run from the repository root"). See the caveat below. | MET |
| 2a | Fix `StageRunSyntheticSmoke` + regression test with the malformed input | anchored ctest `^(StageRunSyntheticSmoke\|ImplStageRunSmokeMalformed_)` | 9/9 passed ("100% tests passed, 0 tests failed out of 9"), exit 0. | MET |
| 3 | Quiet-host baselines for the L1 kernels, L2 WQ101 battery, L3 search, and L6 modes 4/6/7 | `equity-bench` configures (exit 0) and builds `atx-engine-bench` + `atx-shm-worker` (exit 0, 22.2 min at `-j2`). One smoke run per category (below). | Every bench binary runs. The numbers are **not** baselines: the host was busy with other lanes. | **DEFERRED-to-gate (orchestrator)**, as the brief instructs |
| 4 | Ledger line: the lane-6 58 ms figure excludes costs and turnover (R-14) | Wording under "Ledger candidates". The bench header now carries the same scope note. | Only the orchestrator appends to the ledger. | MET (wording delivered; append is the orchestrator's) |

**Caveat on item 2 (`ExistingCp14Ledger_StillVerifies`).** When the whole impl exe is run from the **repository root** (`C:\atx-wt\pool-7`), this test does not skip. It fails with `ParseError: trial ledger: CR in line 0 (LF endings only)` (first run: 533 ran, 527 passed, 5 skipped, 1 failed, exit 1). The cause is the environment, not this lane. `core.autocrlf=true` checks out `atx-engine/reviews/trial-ledger.jsonl` with CRLF (`git ls-files --eol`: `i/lf w/crlf`, no attribute), and the ledger verifier rightly accepts LF only. Under ctest, or from the build dir, the test skips. This is the same CRLF/LF hazard as the Lane-0 failure. The fix is outside this lane's files (see Integration notes).

## Defect table

| ID | Disposition |
|---|---|
| R-14 | **Code: DEFERRED to W2-R3** (its planned owner: per-name κ, the costed prox, and routing costs to the factor-space solver; `solve_with_costs` at `cost_terms.cpp:291-299` still goes through `solve_augmented_form`, and `factor_admm_eligible` at `qp_factor_admm.hpp:111-114` excludes cones). **O1's part: CLOSED.** The scope is recorded in the `optimizer_production_bench.cpp` header: `make_book` builds no `TradeCostTerms`, no `TurnoverBudget` and an empty `w_prev` in every mode. The ledger wording is below for the orchestrator to append. |

## Evidence (verbatim tails)

Build (Debug, `equity-dev`, `CMAKE_BUILD_PARALLEL_LEVEL=2`, free RAM 5.73 GB and then 3.54 GB before the builds):
```
build -Preset equity-dev atx-impl-tests atx-shm-worker            -> [182/185] Linking CXX executable bin\atx-impl-tests.exe ; exit 0
build -Preset equity-dev atx-impl-tests atx-shm-worker atx-engine-risk-tests
  [93/99] ... atx-impl-tests.dir\w0o1_stage_run_smoke_malformed_test.cpp.obj
  [95/99] Linking CXX executable bin\atx-engine-risk-tests.exe
  [96/99] Linking CXX executable bin\atx-impl-tests.exe           ; exit=0
```
Reproduction (before the fix): see "Root cause" (exit 1).

Anchored ctest (after the fix):
```
-Ctest -Preset equity-dev -R '^(StageRunSyntheticSmoke|ImplStageRunSmokeMalformed_)'
100% tests passed, 0 tests failed out of 9      Total Test time (real) = 5.31 sec     exit=0

-Ctest -Preset equity-dev -R '^(RiskNightlyGate_|RiskQpAugment)'
 6/13 Test #446: RiskQpAugmentFast.MatchesDenseOracleAcrossBattery ...............   Passed   44.72 sec
 7/13 Test #447: RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery .......***Skipped   0.04 sec
100% tests passed, 0 tests failed out of 13     Total Test time (real) = 45.33 sec
The following tests did not run:
	447 - RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery (Skipped)
exit=0
```
Whole executables:
```
atx-engine-risk-tests.exe --gtest_brief=1   (risk exit=0 secs=123.6)
Nightly: set ATX_NIGHTLY=1 (or ATX_RISK_NIGHTLY=1) to run the large dense-oracle battery
[==========] 451 tests from 59 test suites ran. (123406 ms total)
[  PASSED  ] 450 tests.
[  SKIPPED ] 1 test.

atx-impl-tests.exe --gtest_brief=1  (cwd build-equity\atx-impl\tests; impl exit=0 secs=309.4)
[==========] 533 tests from 101 test suites ran. (309075 ms total)
[  PASSED  ] 527 tests.
[  SKIPPED ] 6 tests.
```
equity-bench:
```
configure -Preset equity-bench   -> Configuring done (71.6s) / Generating done (0.9s) /
                                    Build files have been written to: C:/atx-wt/pool-7/build-equity-bench ; exit=0
CMakeCache: ATX_BUILD_BENCH=ON, ATX_EQUITY_ONLY=ON, ATX_TEST_GROUPS=all, CMAKE_BUILD_TYPE=Release,
            FETCHCONTENT_BASE_DIR=C:/atx-wt/pool-7/deps/equity-bench
build -Preset equity-bench atx-engine-bench atx-shm-worker
  [232/235] Linking CXX executable bin\atx-shm-worker.exe
  [234/235] Linking CXX executable bin\atx-engine-bench.exe      ; exit=0 mins=22.2
```
Bench smoke runs (one run each, busy host, **not baselines**; all exit 0):
```
--benchmark_filter=^BM_KernelCsRank$        BM_KernelCsRank       47.1 ms  46.9 ms  1  ns_per_cell=30.5176ns
--benchmark_filter=^Wq101_OpFamily/0$       Wq101_OpFamily/0      80.2 ms  93.8 ms  1  cells_per_s=13.44M/s
--benchmark_filter=^BM_SearchScalar/16$     BM_SearchScalar/16    6.03 ms  0.000 ms 10 trials=35
--benchmark_filter=^BM_OptimizerProduction/M:1000/mode:(4|6|7)/
  BM_OptimizerProduction/M:1000/mode:4/iterations:3/real_time   700 ms  630 ms 3 admm_iters=250 polished=0
  BM_OptimizerProduction/M:1000/mode:6/iterations:3/real_time  22.5 ms 20.8 ms 3 admm_iters=100 polished=1
      max_abs_diff_vs_augmented=5.25001u max_abs_diff_vs_tight=0 obj_fs_minus_tight=0
  BM_OptimizerProduction/M:1000/mode:7/iterations:3/real_time  40.4 ms 41.7 ms 3 admm_iters=250 polished=1
```

## Golden-digest old->new table

None. No product code changed. All changes are to tests, comments and the preset file.

## Deviations from the brief

1. **The Nightly split already existed.** Lane 6 (`8c6695a9`) had already split `RiskQpAugment.MatchesDenseOracleAcrossBattery` into `RiskQpAugmentFast.MatchesDenseOracleAcrossBattery` (default) and `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery` (gated on `ATX_RISK_NIGHTLY`). O1 kept that split. It added the repo-wide `ATX_NIGHTLY` switch the brief suggests, made the decision a pure `constexpr` function, and proved with `RiskNightlyGate_Battery.ContainsEveryPreSplitCase` that the original battery body survives unchanged. The pre-split test name no longer exists. Its cases live in the Nightly test.
2. **The stod fix is in the test, not in `config.cpp`/`stage_run.cpp`.** The brief guessed those files. Root-cause analysis showed that the stage output is correct and the test's assumption was wrong, so no W0b-owned source file is touched.
3. **Per-worktree deps.** The `equity-bench` preset sets `FETCHCONTENT_BASE_DIR=${sourceDir}/deps/equity-bench` (gitignored). A Release tree sharing the machine-wide `C:/atx-cache/deps` `spdlog-build` with the Debug lane trees would race on `_ITERATOR_DEBUG_LEVEL`. The cost is a one-time dependency clone per worktree (about 70 s configure here).
4. **Impl test count.** This base has 527 impl tests, not 521. With O1's 6 new tests, 533 run.

## Integration notes

1. **CRLF hazard for `.jsonl` ledgers (owner/orchestrator).** On a `core.autocrlf=true` checkout, `atx-engine/reviews/trial-ledger.jsonl` lands with CRLF, and `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` fails whenever `atx-impl-tests.exe` runs from the repository root. The suggested fix, outside O1's files: add `*.jsonl text eol=lf` (or at least `atx-engine/reviews/*.jsonl text eol=lf`) to `.gitattributes`, then renormalize. `CMakePresets.json` has no attribute either: it is `i/lf w/crlf`, which is harmless for CMake but explains the Lane-0 CRLF/LF mix. Anyone editing it with a tool that writes bare LF should keep the file's CRLF working-tree endings, as O1 did.
2. **The bench exe runs static-init "side benches" on every launch**, whatever the filter (AlphaBatch CSE lever, AlphaWidened CSE lever, Executor speedup/knee). Their hard-coded labels say "(Debug build)" even in this Release tree. There is no Google Benchmark DEBUG warning, and `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`. At the gate, the orchestrator should expect that startup cost and ignore the stale label.
3. **Gate bench recipe (for the quiet-host baselines):**
   `atx-build.ps1 configure -Preset equity-bench`, then
   `atx-build.ps1 build -Preset equity-bench atx-engine-bench atx-shm-worker`, then
   `build-equity-bench\bin\atx-engine-bench.exe --benchmark_filter=<...> --benchmark_out=<f>.json --benchmark_out_format=json`.
   Filters used for the smoke runs: `^BM_Kernel` (L1), `^Wq101_` (L2), `^BM_Search` (L3), `^BM_OptimizerProduction/M:(1000|3000|5000)/mode:(4|6|7)/` (L6). Compare with `scripts\bench-gate.ps1`.
4. **The fast augment battery takes 44.7 s in Debug on a loaded host.** Lane 6's comment says it stays under a 30 s budget; that only holds on a quiet host.
5. **R0 may start.** The lane-6 merge brings new `risk/` headers but does not touch `risk/factor_model.*` or `risk/exposures.hpp`.
6. **W0 gate note: Nightly battery cost.** `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery` was run with the switch on by the reviewer at `edc7724a` (`ATX_NIGHTLY=1 atx-engine-risk-tests.exe --gtest_filter=RiskQpAugmentNightly.*`): **PASS, 1364360 ms (about 23 min), Debug `equity-dev`**. Budget about 23 min per Nightly run in Debug; it must never be part of the default or fast gate.

## Ledger candidates (for the orchestrator to append; ≤ 3)

1. `2026-09-24 R-14: the lane-6 factor-space solve figure (58 ms; 57.7 ms warm, M=3000 K=64, eps 2e-7, BM_OptimizerProduction mode 6, commit d72d96fe) EXCLUDES trade costs and turnover: the bench book has no TradeCostTerms and no TurnoverBudget, and solve_with_costs never reaches the factor-space path. The costed factor-space solve is W2-R3.`
2. `2026-09-24 StageRunSyntheticSmoke "invalid stod argument" root cause: run_report's identity kvs (research_artifact_id/books_artifact_id = "unknown" on unidentified panels) were fed to std::stod by the test; fixed test-side with a strict non-throwing kv audit (W0-O1).`
3. `2026-09-24 core.autocrlf=true checks out atx-engine/reviews/trial-ledger.jsonl as CRLF; ExistingCp14Ledger_StillVerifies then fails when atx-impl-tests runs from the repo root (the verifier is LF-only). Needs a .gitattributes eol=lf rule.`

## Fix pass 1

Review: `.superpowers/sdd/w0/lane-o1-review.md` (verdict APPROVE, reviewed `edc7724a`). No blocker or major findings; both minors were in scope and cheap, so both are fixed.

| Finding | What changed | Evidence |
|---|---|---|
| `risk_qp_augment_test.cpp:668` (minor): `RiskNightlyGate_Env.DefaultEnvironmentSkipsNightly` clears both switches in `SetUp()`, so it proves "unset means off", not the ambient default | Renamed to `RiskNightlyGate_Env.UnsetSwitchesSkipNightly`, body unchanged (no assertion removed). A comment above it says what it proves and cites the whole-exe / ctest `Skipped` line for `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery` as the default-environment proof. No other reference to the old name exists outside build trees. | ctest `Test #450: RiskNightlyGate_Env.UnsetSwitchesSkipNightly ... Passed`; the default-environment skip is the whole-exe line `risk_qp_augment_test.cpp(528): Skipped` / `[  SKIPPED ] 1 test.` below, run with both switches removed from the process environment. |
| `lane-o1-report.md:47` (minor): acceptance 1a claimed "not weakened" without an `ATX_NIGHTLY=1` run | The reviewer's run is carried into the gate notes: Integration note 6 records `ATX_NIGHTLY=1 ... --gtest_filter=RiskQpAugmentNightly.*` -> PASS, 1364360 ms (about 23 min) Debug `equity-dev`, at `edc7724a`. Acceptance 1a's evidence is now case-list equality (`ContainsEveryPreSplitCase`) plus that passing switch-on run. The Nightly body and `kDiffTol` are untouched by this pass, so the run still applies. | Review "Evidence": `[       OK ] RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery (1364360 ms)`, `[  PASSED  ] 1 test.`, exit 0. Not re-run in this pass (23 min of Debug CPU on a shared host, no change to the tested code). |

Re-verification after the fix (Debug `equity-dev`, `CMAKE_BUILD_PARALLEL_LEVEL=2`, 5.2 GB free):
```
build -Preset equity-dev atx-engine-risk-tests
  [9/11] Building CXX object ...atx-engine-risk-tests.dir\risk_qp_augment_test.cpp.obj
  [10/11] Linking CXX executable bin\atx-engine-risk-tests.exe      exit=0
build -Preset equity-dev atx-impl-tests atx-engine-risk-tests
  [2/3] Linking CXX executable bin\atx-impl-tests.exe               exit=0

-Ctest -Preset equity-dev -R '^(StageRunSyntheticSmoke|ImplStageRunSmokeMalformed_|RiskNightlyGate_|RiskQpAugment)'
  10/22 Test #450: RiskNightlyGate_Env.UnsetSwitchesSkipNightly ...   Passed    0.03 sec
  100% tests passed, 0 tests failed out of 22     Total Test time (real) = 46.63 sec
  The following tests did not run:
	447 - RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery (Skipped)
  exit=0

atx-engine-risk-tests.exe --gtest_brief=1  (cwd build-equity\atx-engine\tests; ATX_NIGHTLY / ATX_RISK_NIGHTLY unset)
  ..\atx-engine\tests\risk_qp_augment_test.cpp(528): Skipped
  [==========] 451 tests from 59 test suites ran. (81147 ms total)
  [  PASSED  ] 450 tests.
  [  SKIPPED ] 1 test.                        risk exit=0 secs=81.2
atx-impl-tests.exe --gtest_brief=1  (cwd build-equity\atx-impl\tests)
  [==========] 533 tests from 101 test suites ran. (204711 ms total)
  [  PASSED  ] 527 tests.
  [  SKIPPED ] 6 tests.                       impl exit=0 secs=204.9
```
Files touched in this pass: `atx-engine/tests/risk_qp_augment_test.cpp` (rename + comment; CRLF working tree kept, 0 bare LF) and this report. No product code; no test weakened, skipped or deleted.