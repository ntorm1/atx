# Lane W0-O1: Integrate lane 6, equity-bench preset, red test

Host tag: **RAM:light** · Batch: **W0a** · Pool: **`C:\atx-wt\pool-7`** ·
Branch: **`feat/w0-o1-l6`** (run id `aes-w0-o1`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-O1 · Integrate lane 6, bench preset, red test (orchestrator + 1 lane)
- **Build:**
  - Merge `feat/qps-l6-optim@1cf59cb7`.
  - Add the `equity-bench` preset (inherits `equity-rel`, `ATX_BUILD_BENCH=ON`, groups=all; check CRLF/LF, the root cause of the Lane-0 failure).
  - Fix `StageRunSyntheticSmoke` ("invalid stod argument"; use systematic-debugging; add a regression test with the malformed input).
  - Move `RiskQpAugment.MatchesDenseOracleAcrossBattery` to Nightly.
- **Accept:**
  - `atx-engine-risk-tests` green, with Nightly skipped.
  - `atx-impl-tests` 521/521.
  - Quiet-host baselines recorded for the L1 kernels, the L2 WQ101 battery, L3 search, and L6 modes 4/6/7.
  - Ledger line recording that the lane-6 58 ms figure **excludes costs and turnover** (R-14).

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| R-14 | H | lane-6 `cost_terms.cpp:291-299`; `qp_factor_admm.hpp:111-114` | Costs never reach the factor-space solver (cones are ineligible and κ is scalar). The 58 ms figure is for a problem with no costs and no turnover. | W2-R3 |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - every file changed by `feat/qps-l6-optim` (merge it: `git -C C:\atx-wt\pool-7 merge --no-ff feat/qps-l6-optim`)
  - `CMakePresets.json` (add `equity-bench`: inherits `equity-rel`, `ATX_BUILD_BENCH=ON`, `ATX_TEST_GROUPS=all`, own binaryDir `build-equity-bench`; keep the file's existing line endings — the CRLF/LF mismatch killed the qps Lane-0 attempt)
  - the Nightly gate for `RiskQpAugment.MatchesDenseOracleAcrossBattery` (`atx-engine/tests/risk_qp_augment_test.cpp`; mechanism is your choice, e.g. `GTEST_SKIP()` unless env `ATX_NIGHTLY=1` — the test body must stay intact, not weakened; CMake edits allowed for this lane only)
  - the minimal root-cause fix site for `StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` ("invalid stod argument") — wherever it is (likely `atx-impl/src/config.cpp` or `stage_run.cpp`, owned by W0b lanes that start after you merge; keep the diff minimal and name the file in the report) + a regression test with the malformed input
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `risk` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-risk-tests`, `atx-impl-tests (+ atx-shm-worker)`.
- Suites: the whole `atx-engine-risk-tests` (Nightly skipped) and the whole `atx-impl-tests`; new suites prefixed `ImplStageRunSmokeMalformed_*` / `RiskNightlyGate_*`.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-o1-report.md`.

## Lane notes (orchestrator)

- Use superpowers:systematic-debugging for the stod failure: reproduce, find root cause, then fix.
- Quiet-host bench baselines (L1 kernels, L2 WQ101 battery, L3 search, L6 modes 4/6/7) CANNOT be measured during W0a (6 lanes running). Your job: make `equity-bench` configure and build `atx-engine-bench` (or the lane-6 `optimizer_production_bench`) and prove each bench binary runs once with a tiny filter (smoke only, numbers not recorded as baselines). The orchestrator records quiet-host baselines at the W0 gate. Mark that acceptance item 'DEFERRED-to-gate (orchestrator)' in the report.
- Ledger line (R-14) is appended by the orchestrator only: put the exact proposed wording in 'Ledger candidates' — the lane-6 58 ms figure excludes costs and turnover.
- You merge FIRST into integration; R0 (risk) starts only after you merge. Do not edit `risk/factor_model.*` or `risk/exposures.hpp` (R0 owns them) beyond what the lane-6 merge itself brings.

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
