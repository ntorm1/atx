# Lane W0-E0b review (fresh adversarial reviewer)

## Verdict
APPROVE

## Reviewed SHA
d6b6df2f0ec0c57a621b92d4bea5e17ca0d1c4dc (lane `feat/w0-e0b`; `feat/w0-integration` =
458d0bef is an ancestor, `git merge-base --is-ancestor` exit 0)

## Evidence
All commands were run by the reviewer in `C:\atx-wt\pool-6` (free RAM 5.10 GB before the build;
`CMAKE_BUILD_PARALLEL_LEVEL=2`; `ATX_TEST_GROUPS:STRING=eval`, so no reconfigure was needed).

1. `scripts\atx-build.ps1 build -Preset equity-dev atx-engine-eval-tests`: exit 0
   (`[9/10] Linking CXX executable bin\atx-engine-eval-tests.exe`). Clean under /W4 /WX.
2. `scripts\atx-build.ps1 check -Preset equity-dev atx-impl\src\stage_equity_mine.cpp`: exit 0
   (a consumer of the changed `TrialSummary` DSR overload; the object is up to date against the
   lane headers).
3. Anchored suites (`-Ctest -Preset equity-dev -R ...`):
   ```
   ^EvalTrialClusters exit=0    100% tests passed, 0 tests failed out of 13
   ^EvalRegistryWindows exit=0  100% tests passed, 0 tests failed out of 9
   ^EvalLockboxEmbargo exit=0   100% tests passed, 0 tests failed out of 4
   ^EvalTrialRegistry exit=0    100% tests passed, 0 tests failed out of 13
   ```
4. The whole owning executable, `build-equity\bin\atx-engine-eval-tests.exe --gtest_brief=1`:
   exit 0.
   ```
   [w0e0b] G-block: G=4 n=59 rho_within=0.60 -> N_clusters=4 (n_eff=9.78, mean silhouette=0.369)
   [w0e0b] G-block: G=10 n=119 rho_within=0.50 -> N_clusters=10 (n_eff=33.44, mean silhouette=0.259)
   [w0e0b] G-block: G=20 n=199 rho_within=0.50 -> N_clusters=20 (n_eff=61.98, mean silhouette=0.262)
   [w0e0b] equicorrelated null rho=0.5 N=2000 (n_eff=3.77), 12000 experiments: FPR MonteCarloMaxV2=0.0545, summary NEffCrossVarV1 (pre-W0)=0.5248, summary RawNCrossVarV2=0.0123
   [w0e0b] tight 20-block: SR*_cluster=0.12950 SR*_mc=0.13244 ratio=0.978; V1 SR*=0.12742
   [==========] 221 tests from 38 test suites ran. (54378 ms total)
   [  PASSED  ] 221 tests.
   ```
   These numbers match the lane report exactly. The `CHECK failed` lines come from pre-existing
   death tests.

## Findings
path:line | severity | problem | required fix
---|---|---|---
atx-engine/include/atx/engine/eval/deflated_sharpe.hpp:257 | minor | Acceptance 1 (FPR 5% ± 1% on the ρ=0.5, N=2000 null) is proven only for the opt-in `AccountingDsrRule::MonteCarloMaxV2`, whose dsr is the null CDF of the maximum. The default `ClusterMcFloorV2` is PSR-based, so it is conservative on this null by construction (the summary V2 rule measured 1.2%), and its FPR was not measured. The integration note sends W1-I1 to the default overload. So the calibrated 5% property is not the one that downstream DSR will have unless callers opt in. The report discloses this (Deviation 2), and it is not a correctness bug. | In the integration notes, state which rule a selection gate at α=5% must use (`MonteCarloMaxV2`), or have the owner confirm the reading of acceptance 1.
atx-engine/include/atx/engine/eval/trial_registry.hpp:199 | minor | `keep_sketches` defaults to true, and `infos` (about 80 B per trial) is kept unconditionally (`trial_registry.cpp:607`). The pre-change "memory O(d²) regardless of trial count" contract is now O(n·d). The existing 10^6-trial bench (`eval_multiple_testing_bench.cpp`, d=64) would hold about 0.6 GB. | Record the memory change in the ledger or integration notes, or set `keep_sketches=false` for 10^6-trial callers.
atx-engine/include/atx/engine/eval/trial_clusters.hpp:70 | minor | The ONC base stage caps k at `kOncDefaultMaxK = 64`. A registry with more than 64 genuine families can only reach a larger N through the depth-2 refinement. Under `ClusterV2` alone, that under-counts N and under-deflates. The default MC floor mitigates this, and the tests only reach G=20. | Document the cap next to `ClusterV2`, or raise `max_k` in accounting for large n. There is no action for the default rule.

No blocker or major findings.

## Checked
- [x] .agents/cpp/agent.md §10 applied to the diff:
  - No UB found. Buffer indices are bounded: `start+k <= window_end < pnl_len` in `make_sketch`, and the decode path re-validates windows.
  - Overflow is guarded: `horizon + delay`, `n*n` in `onc_cluster`, `n*d` in MC.
  - No narrowing without a cast.
  - Every loop is bounded: k-means `max_iter`, recursion `max_depth`, I/O loops.
  - `LogFile` and `LogLock` are move-only RAII; a moved-from lock does not unlock.
  - Error paths return `Result`. A failed append truncates to `synced_len`. The anchor is verified before any tail repair, and the file is left untouched on mismatch.
  - The `lockbox_embargo_len` switch is exhaustive, with an unknown-rule `Err`.
  - The build is clean under /W4 /WX.
- [x] Diff stays inside the brief's files-in-scope:
  - 4 owned headers + `trial_registry.cpp` (hosting the `trial_clusters` implementation, as the brief allows).
  - The E-01 pin test.
  - 3 new `eval_w0e0b_*_test.cpp` files.
  - The lane report.
  - No CMake edits and no new `src/*.cpp`.
- [x] Acceptance items against real output and test code:
  - FPR 0.0545 (in the band, deterministic seeds). The experiment design is sound: the null is estimated on an independent calibration sample, and for Gaussian data the sample correlation is independent of the sample means. Calibration 0 is asserted bit-identical to the `TrialRegistry::mc_max_null` path.
  - G-block N = G exactly for G = 4/10/20, with every block in one cluster; for G=4 the ±10% band means N must be exact.
  - `RegistryFedDsrDiscountsCorrelationOnce` would fail on the old code: the old default SR* is below 0.5·E_mc[max], against the asserted ±20% of E_mc[max].
- [x] Defect IDs:
  - E-01 is closed. The summary default is `RawNCrossVarV2`, V1 is kept, and the cluster-N overload is added.
  - E-16 is closed on the registry side: V2 log with window, fidelity, tags, IS/OOS and a variable length. V1 logs are read and appended, and the header version check matches the old `kVersion=1`. The recording site is deferred to I0b, as the brief directs.
  - E-17 is closed at the API: `LabelHorizonV2` is the default of `LockboxEmbargo`, with `CpcvFractionV1` reproducing the old width. The call sites are deferred to their owning lanes.
  - L-08 is closed on the registry side (tested). The learn side is deferred to L0/L4, per the findings row.
- [x] No test weakened. The only edit to a pre-existing test file is the E-01 pin (22+/5−), which is tied to E-01. No DISABLED_ or GTEST_SKIP was added.
- [x] Changed numeric defaults sit behind versioned enums (`SummaryDsrRule`, `EmbargoRule`, `TrialLogFormat`). No golden digest changed. The full-window `n_eff` / `registry_hash` computation is unchanged from the base (the `c = 1/(C−1)` path is identical).
- [x] The owning executable passes whole (221/221), and the evidence in the report matches the reviewer's rerun.
