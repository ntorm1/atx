# Lane W0-L0: Learn leakage

Host tag: **RAM:light** · Batch: **W0a** · Pool: **`C:\atx-wt\pool-3`** ·
Branch: **`feat/w0-l0`** (run id `aes-w0-l0`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-L0 · Learn leakage
- **Closes:** L-01, L-02, L-03, L-07, L-08 (IcLoss).
- **Owns:** `learn/tcn_alpha.cpp`, `learn/nn/trainer.{hpp,cpp}`, `learn/nn/loss*`, `learn/latent.cpp`, `learn/linear_alpha.cpp` and `learn/gbt.cpp` (fold-augmentation sites only), `learn/feature_matrix.hpp` (label-horizon metadata).
- **Build:**
  - Inner purged validation block carved from the train dates.
  - Label-maturity metadata, with the filter `r + H ≤ t − embargo`.
  - Fold-local augmentation fitting.
  - The deployed ensemble selects on inner validation.
  - `IcLoss` uses date-grouped batches.
  - Trial count = configurations, not folds × horizons.
- **Suites:** `LearnLabelMutationInvariance_*` (it fails today), `LearnLabelMaturity_*`, `LearnFoldLocalAug_*`, `LearnIcLossPerDate_*`.
- **Accept:**
  - Mutating test-fold labels leaves OOF predictions byte-identical.
  - A held-out-label perturbation leaves fold training artifacts unchanged.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| L-01 | B | `learn/tcn_alpha.cpp:355`; `nn/trainer.cpp:163-170` | The CPCV **test fold** is the validation set, and the checkpoint is chosen on test loss, so OOF IC/DSR/PBO are biased toward passing. | W0-L0 |
| L-02 | B | `learn/latent.cpp:43-60` | `select_interactions` uses labels maturing after t (it checks embargo < H). `FeatureMatrix` carries no label horizon. | W0-L0 |
| L-03 | H | `linear_alpha.cpp:155,197`; `gbt.cpp:478,512` | Full-window augmentation (PCA/interactions) is copied into every fold. | W0-L0 |
| L-07 | M | `tcn_alpha.cpp:452` | The deployed checkpoint is chosen on training loss. | W0-L0 |
| L-08 | L | `linear_alpha.cpp:229`, `gbt.cpp:550`, `tcn_alpha.cpp:374`; `IcLoss` | The horizon blend uses pooled Pearson. `trial_count++` runs per fold. `IcLoss` is computed on shuffled mixed-date batches. The autoencoder is not GKX (it is mislabelled). | W0-L0 / W3-L4 |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - `atx-engine/include/atx/engine/learn/tcn_alpha.hpp`, `atx-engine/src/learn/tcn_alpha.cpp`
  - `atx-engine/include/atx/engine/learn/nn/trainer.hpp`, `atx-engine/src/learn/nn/trainer.cpp`
  - `atx-engine/include/atx/engine/learn/nn/loss.hpp`, `atx-engine/src/learn/nn/loss.cpp`
  - `atx-engine/include/atx/engine/learn/latent.hpp`, `atx-engine/src/learn/latent.cpp`
  - `atx-engine/src/learn/linear_alpha.cpp` and `atx-engine/src/learn/gbt.cpp` — ONLY the fold-augmentation sites (L-03) and the trial-count sites (L-08); headers only if a declaration must change
  - `atx-engine/include/atx/engine/learn/feature_matrix.hpp` — label-horizon metadata only (W1-L1 owns the rest)
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `learn` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-learn-tests`.
- Suites: `LearnLabelMutationInvariance_*` (write it first as a probe — it fails on the base; that is expected, not TDD ceremony), `LearnLabelMaturity_*`, `LearnFoldLocalAug_*`, `LearnIcLossPerDate_*`; must stay green: whole learn target.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-l0-report.md`.

## Lane notes (orchestrator)

- L-08 is split: you own the learn-side trial counting (configurations, not folds × horizons) and IcLoss per-date batches; E0b owns registry-side trial accounting. The mislabelled autoencoder (L-08 tail) is W3-L4 — mark DEFERRED.
- Pool-3 was an alpha pool; reconfigure `-Groups "learn"` once.

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
