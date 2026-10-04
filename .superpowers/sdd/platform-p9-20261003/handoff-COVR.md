# Handoff: lane COVR at the owner stop (2026-10-04)

The owner STOP arrived after the lane's work was finished. Nothing was in flight and nothing is unfinished. The full
account is `task-COVR-report.md` (same dir), with evidence in `cov-evidence/covr/`.

- **Worktree / branch:** `C:/atx-wt/pool-24`, `feat/p9-cov-20261003`.
- **Head before this handoff commit:** `16325e23`.
- **Root merged:** `d7f762cf` (code head `30b9c9d2`), merge commit `9a438f5e` (`--no-ff`, no conflict).
- **Status:** the attribution is complete, the report and evidence are committed, and `git status` is clean. No build
  or test of mine is running.

## Established (measured, not guessed)

- **Cause, class (i) (C runtime).** `std::exp2(-1.0 / H)`:
  - Debug resolves it to `ucrtbased.dll` 10.0.22000.832 and Release to `ucrtbase.dll` 10.0.26100.7623.
  - The bits differ at H = 84 and H = 504. Debug is 1 ULP off; Release is correctly rounded.
  - H = 42 and H = 252 give equal bits in both builds.
- **First differing value:** `LaggedEwma::decay_` at `atx-impl/src/strategy_risk_model.cpp:490`, for factor_fast
  (HL 84, built at `:694`). Also at `:696` (HL 504) and `:697` (HL 84), plus the Kish decay at `:853` (HL 504).
  - The input `-1.0/84` (`bf88618618618618`) is bit-identical in both builds.
  - In the container, the first difference is block 2, DIAG `eigen_effective`. F differs from session 63 and the
    specific variance from session 252.
  - These container numbers equal root's M2g COV-7 numbers exactly.
- **Counterfactual.** I rebound only the `exp2` import to the other CRT, in both directions. The Debug and Release
  digests then match:
  - every per-session stage digest, constant and probe, for both recipes;
  - every container block and every other output file.
  So `exp2` is the only cause on this fixture. There is no class (ii), (iii) or (iv) cause.
- **Determinism within one build.** Release = Release and Debug = Debug: whole output files byte-identical over two
  runs and a third run at CPU affinity mask 1.
  - The estimator, S2b and the writer start no thread.
  - The only pool is the readers' hash verification, which writes no bytes.
- **No fix was started.** Ruling COV-12 (b) / OD5-2 forbids a kernel change for a class (i) cause. No pinned,
  golden or expected value was edited.
- **No M2c-RED-2 skip was needed.** No test pins CRT-specific container bytes.
- **Code added:** test-only, a DISABLED_ diagnostic `atx-impl/tests/strategy_cov_stage_diag.cpp` plus one CMake
  source-list line.

## Builds and tests

Builds:
- `p9-covr-a`: Debug, exit 1 (include-order slip, fixed in `d0647e8c`).
- `p9-covr-b`: Debug, exit 0.
- `p9-covr-c`: Release, exit 0.
- **Next free letter: `d`** (`p9-covr-d`).

Test counts:

| suite | Debug | Release |
|---|---|---|
| `RiskCovContainer.*` | 21 pass | 21 pass |
| impl `Cov*` (6 suites) | 29 pass | 29 pass |
| impl regress filter | 106 pass | 106 pass |
| engine `Risk*` minus the 31-min oracle | 431 pass + 1 nightly skip | 430 pass + 2 skip |

- The extra Release skip is the pre-existing NDEBUG skip `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read`.
- 0 failed in either build.
- Not run: the 31-minute oracle `RiskQpAugment.MatchesDenseOracleAcrossBattery`, whole suites outside these filters,
  and anything on real data.

## Disk

All my build trees are deleted by name: `build-equity` objects, `build-equity-rel`, `deps/equity-rel`,
`p9-covr-out`. About 5.9 GB was freed. Receipts and logs are kept (`build-equity/mega-p9-covr-*`, copies in
`cov-evidence/covr/`). The attribution is complete, so no tree is needed. A fresh agent who wants to re-run the
diagnostic must rebuild (the next tag is `p9-covr-d`).

## Next steps (for root or the PM, not for a lane)

1. Review `task-COVR-report.md`, then merge (test-only). At merge, rebuild `atx-impl-strategy-target-tests` in Debug and
   in Release and reproduce the counts above. Optionally re-run the diagnostic with and without `ATX_COVR_SWAP=exp2`.
2. Under COV-12 (b) cause (i):
   - Record the `ucrtbase.dll` version and SHA-256.
   - Show Release = Release on TRAIN for both recipes: two runs plus one at affinity 1, SHA comparison only, with
     `cov-evidence/covr/tools/covr_cmp.py det`.
   - Then run D-COV in Release (COV-12 (c)).
3. P10: add `exp2` to the OD5-2 deterministic-libm item. The `v8-risk-lo3-v10` (Debug-built) re-pin question goes
   there.
