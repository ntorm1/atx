# Lane W0-O1 review

## Verdict

APPROVE

## Reviewed SHA

`edc7724a4ea6cd2560dcf43489c59a97fe33474c` (lane head, `feat/w0-o1-l6`; base `458d0bef480a624e258070c9d45174a9984466bf` = `merge-base feat/w0-integration HEAD`).

## Evidence

All commands were run by the reviewer in `C:\atx-wt\pool-7` at the reviewed SHA.

Merge integrity:
```
git merge-tree --write-tree 458d0bef 1cf59cb7      -> 30c9ea6cb6341e0e37aca6bfc9c56c3353aa85d2
git rev-parse 1de5814f^{tree}                       -> 30c9ea6cb6341e0e37aca6bfc9c56c3353aa85d2
git rev-parse 1de5814f^2 feat/qps-l6-optim          -> 1cf59cb7... (both)
```
The lane-6 merge commit is exactly the clean merge, with no hand edits.

Build (equity-dev, `CMAKE_BUILD_PARALLEL_LEVEL=2`, 4.14 GB free):
```
atx-build.ps1 build -Preset equity-dev atx-engine-risk-tests atx-impl-tests atx-shm-worker
[10/12] Linking CXX executable bin\atx-engine-risk-tests.exe
[11/12] Linking CXX executable bin\atx-impl-tests.exe      exit=0
```
Anchored ctest:
```
atx-build.ps1 -Ctest -Preset equity-dev -R '^(StageRunSyntheticSmoke|ImplStageRunSmokeMalformed_|RiskNightlyGate_|RiskQpAugment)'
100% tests passed, 0 tests failed out of 22
The following tests did not run:
	447 - RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery (Skipped)
exit=0
```
Whole owning executables:
```
atx-engine-risk-tests.exe --gtest_brief=1   (cwd build-equity\atx-engine\tests)
[==========] 451 tests from 59 test suites ran. (160880 ms total)
[  PASSED  ] 450 tests.
[  SKIPPED ] 1 test.            exit=0
atx-impl-tests.exe --gtest_brief=1          (cwd build-equity\atx-impl\tests)
[==========] 533 tests from 101 test suites ran. (304002 ms total)
[  PASSED  ] 527 tests.
[  SKIPPED ] 6 tests.           exit=0
```
The 6 impl skips are in untouched files (alpha101_orats x2, discover, fundamental_zoo, single_alpha_capacity, trial_ledger). They are the base's opt-in gates: base 521 run+pass plus O1's 6 new tests = 527.

Nightly battery run with the switch ON. The lane never did this; the reviewer ran it to prove the moved battery still passes:
```
ATX_NIGHTLY=1 atx-engine-risk-tests.exe --gtest_filter=RiskQpAugmentNightly.*
[       OK ] RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery (1364360 ms)
[  PASSED  ] 1 test.            exit=0 secs=1365
```
equity-bench smoke (the existing Release tree, built 20:09 after code commit c339ead5; CMakeCache ATX_BUILD_BENCH=ON, ATX_TEST_GROUPS=all, Release, per-worktree FETCHCONTENT_BASE_DIR):
```
build-equity-bench\bin\atx-engine-bench.exe --benchmark_filter=^BM_OptimizerProduction/M:1000/mode:6/
BM_OptimizerProduction/M:1000/mode:6/iterations:3/real_time  15.8 ms  15.6 ms  3 admm_iters=100 ... polished=1   exit=0
```
Diagnostics (pre-existing and environmental, not a lane defect): run from the repository root, `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` fails with `CR in line 0 (LF endings only)`. `git ls-files --eol atx-engine/reviews/trial-ledger.jsonl` gives `i/lf w/crlf attr/`. The file was last changed in 092314d4 and this lane does not touch it. The report discloses this as a caveat and an integration note.

## Findings

path:line | severity | problem | required fix
---|---|---|---
atx-engine/tests/risk_qp_augment_test.cpp:668 | minor | `RiskNightlyGate_Env.DefaultEnvironmentSkipsNightly` unsets both switches in `SetUp()` before it asserts. It proves "unset means off", not the real default environment. The default-run skip is proven only by the whole-exe / ctest "Skipped" line, which the reviewer confirmed. | Optional: rename it (e.g. `UnsetSwitchesSkipNightly`), or cite the whole-exe skip as the default-environment proof. No code change is needed for acceptance.
.superpowers/sdd/w0/lane-o1-report.md:47 | minor | Acceptance 1a says the Nightly battery is "not weakened", but the lane never ran it with the switch on, so the evidence shows only case-list equality. Reviewer run: PASS, 1364 s Debug (above). | Carry the reviewer's ATX_NIGHTLY=1 run (PASS, about 23 min Debug) into the gate notes, so the Nightly cost is known.

No blocker or major findings.

## Checked

- [x] `.agents/cpp/agent.md` §10 checklist applied to the diff:
  - `nightly_value_on`: `v[1]` is read only when `v[0]=='0'`, so it is at most the NUL. No out-of-bounds read.
  - `read_env`: `_dupenv_s` buffer is freed on the success path; the error path returns nullopt with a null buffer.
  - `w0o1_report_kv.hpp`: `from_chars` with a full-consumption check; noexcept; no narrowing; `identity_value_valid` mirrors `report_hash_valid` (stage_report.cpp:223-227).
  - Lane-6 product changes are additive and opt-in: `QpConfig::factor_space=false`, `MultiHorizonConfig::true_mpc=false`, and `cost_terms.{hpp,cpp}` replace an empty scaffold. The historical `solve_with_cert(p)` / `solve_augmented_form(aug,p)` stay on the `sched==nullptr && ws==nullptr` path, and every byte pin in the risk exe passes.
  - `solve_factor_admm` validates warm-start lengths (x0==M, y0==ny) and the finiteness of `rho_warm`, and every loop is bounded by `cfg.iters`.
  - `/W4 /WX`: all owning targets built clean.
- [x] Acceptance items against real output:
  - (1) Risk exe green with Nightly skipped: 450 pass, 1 skip.
  - (1a) The pre-split battery is in `nightly_battery()`: `ContainsEveryPreSplitCase` compares against the 11 base cases (verified identical to the `458d0bef` source). `kDiffTol` is still 1e-8 (line 413). The Nightly run passes with the switch ON.
  - (2) Impl exe: 527 pass + 6 pre-existing skips.
  - (2a) Smoke fix: the root cause is confirmed at stage_report.cpp:920-921 (identity kvs "unknown"). The fix is test-side and stricter: a full-parse `from_chars`, an identity-count check, and numeric+identity == all kvs. It passes; the old code threw.
  - (3) Bench baselines: DEFERRED-to-gate per the brief; the preset configures and builds and the bench runs (reviewer smoke above).
  - (4) The R-14 ledger wording is supplied in the report's ledger candidates.
- [x] Cited defect R-14: `cost_terms.cpp:290-299` `solve_with_costs` always calls `solve_augmented_form` (never the factor path). `qp_factor_admm.hpp:111-114` `factor_admm_eligible` excludes cones. `optimizer_production_bench.cpp` `make_book` (72-106) builds no TradeCostTerms, no TurnoverBudget and an empty w_prev. The code fix is justifiably DEFERRED to W2-R3 (the register's owning lane). The O1 part (scope note + ledger wording) is CLOSED.
- [x] Diff stays inside the brief's scope: the 22 lane-6 files (identical to `merge-base..1cf59cb7`), `CMakePresets.json` (the `equity-bench` preset inherits `equity-rel`, BENCH=ON, groups=all, own binaryDir; blob LF, working tree CRLF, `i/lf w/crlf`), `risk_qp_augment_test.cpp` (Nightly gate), `stage_run_synthetic_smoke_test.cpp` (fix site), the new `w0o1_*` test files, and the lane report. No CMakeLists or src edits.
- [x] No test weakened: the only pre-existing tests modified are the augment battery (split into Fast + Nightly superset, tolerance unchanged, body intact, Nightly passes) and the smoke test (stricter parsing; identity kvs checked as ids). No DISABLED_; the only new GTEST_SKIP is the brief-mandated Nightly gate.
- [x] Numeric behaviour: no default changed; new behaviour is behind opt-in flags. No golden re-baseline ("none" is correct).
- [x] Evidence in the report matches the claims (every whole-exe / ctest count reproduced by the reviewer).
