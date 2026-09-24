# Lane W0-E0B: Trial accounting

Host tag: **RAM:light** · Batch: **W0a** · Pool: **`C:\atx-wt\pool-6`** ·
Branch: **`feat/w0-e0b`** (run id `aes-w0-e0b`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-E0b · Trial accounting
- **Closes:** E-01, E-16, E-17, plus L-08 (trial counting).
- **Owns:** new `eval/trial_clusters.{hpp,cpp}`, `eval/deflated_sharpe.hpp`, `eval/trial_registry.{hpp,cpp}`, `eval/lockbox.hpp`.
- **Build:**
  - ONC-style clustering on the registry PnL Gram. DSR uses N = number of clusters and V = variance of the cluster-representative Sharpes. A Monte-Carlo E[max] under the estimated correlation is the cross-check.
  - Registry records per trial:
    - data window [start, end]
    - fidelity level
    - family and theme tags
    - variable `pnl_len`
    - the OOS/IS flag (the registry doc said OOS PnL, but equity-mine records train net PnL; findings E-16)
  - Lockbox embargo = maximum label horizon + delay.
  - Tamper-evident chain head exported outside the log, plus a multi-writer lock (the L4 gaps).
- **Suites:** `EvalTrialClusters_*`, `EvalRegistryWindows_*`, `EvalLockboxEmbargo_*`.
- **Accept:**
  - An equicorrelated null (ρ=0.5, N=2000) gives a Monte-Carlo false-positive rate of 5% ± 1%.
  - A G-block model gives N ≈ G ± 10%.
  - `RegistryFedDsrIsLessOverDeflatedOnCorrelatedTrials` is replaced by a test that asserts the correct behavior.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| E-01 | H | `eval/deflated_sharpe.hpp:183-193`; `trial_registry.cpp:398-409` | The registry DSR double-discounts correlation: N_eff = N²/Σρ² is paired with a cross-trial variance V that has already removed the common component. For L9 this understates SR* about 2.7×. A test locks this in. | W0-E0b |
| E-16 | M | `stage_equity_mine.cpp:742`; registry doc | The registry records train PnL, and a fixed `pnl_len` blocks trials over different windows. | W0-E0b |
| E-17 | L | `eval/lockbox.hpp` | The embargo is ⌈0.01·T⌉, not tied to the label horizon. | W0-E0b |
| L-08 | L | `linear_alpha.cpp:229`, `gbt.cpp:550`, `tcn_alpha.cpp:374`; `IcLoss` | The horizon blend uses pooled Pearson. `trial_count++` runs per fold. `IcLoss` is computed on shuffled mixed-date batches. The autoencoder is not GKX (it is mislabelled). | W0-L0 / W3-L4 |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - new `atx-engine/include/atx/engine/eval/trial_clusters.hpp` (header-only, or implemented inside `atx-engine/src/eval/trial_registry.cpp`; no new .cpp)
  - `atx-engine/include/atx/engine/eval/deflated_sharpe.hpp`
  - `atx-engine/include/atx/engine/eval/trial_registry.hpp`, `atx-engine/src/eval/trial_registry.cpp`
  - `atx-engine/include/atx/engine/eval/lockbox.hpp`
  - the existing test `RegistryFedDsrIsLessOverDeflatedOnCorrelatedTrials` (replace it with a test asserting the correct behaviour — this is the cited E-01 pin, not a weakening)
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `eval` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-eval-tests`.
- Suites: `EvalTrialClusters_*`, `EvalRegistryWindows_*`, `EvalLockboxEmbargo_*`; must stay green: whole eval target.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-e0b-report.md`.

## Lane notes (orchestrator)

- E-16 recording side (`stage_equity_mine.cpp:742`) is I0b (W0b): expose the registry API (window [start,end], fidelity, family/theme tags, variable pnl_len, OOS/IS flag) and write the integration note.
- L-08 here = registry-side trial accounting API (count configurations); the learn-side count sites are L0's.
- Registry format change: keep reading old logs (versioned record format) — cp14–cp22/L9/L10 sidecars are imported in W1-I1.

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
