# Lane W0-L0 report — Learn leakage

## Outcome

**DONE.** All five cited defects are closed in the learn layer (L-08's autoencoder
clause is deferred to W3-L4 as the brief directs). Both plan acceptance items are met
and backed by named tests with measured numbers. The whole `atx-engine-learn-tests`
executable is green: 187 of 187 tests.

## Branch / SHA

- Branch: `feat/w0-l0`. Code head: `3ec645a8a3ef416d32051972f338fd8465d2e2c6`. The
  report commit sits on top of it (its SHA is returned in the structured result).
- Base: `458d0bef480a624e258070c9d45174a9984466bf`. `git merge --no-ff
  feat/w0-integration` at the start printed "Already up to date."
- Pool: `C:\atx-wt\pool-3`, lease run id `aes-w0-l0` (held by the orchestrator).
- Build tree: `build-equity\`, reconfigured once with `-Groups "learn"` (it held
  `alpha;parallel`).

## Files changed

Owned sources and headers:

- `atx-engine/include/atx/engine/learn/feature_matrix.hpp`: label-horizon metadata
  (`label_horizons`, `has_label_horizons()`) and the `label_matured` filter.
- `atx-engine/include/atx/engine/learn/latent.hpp`, `atx-engine/src/learn/latent.cpp`:
  `LabelMaturityRule`, a maturity-aware `select_interactions`, the row-explicit fitters,
  `fit_fold_augmentation` / `fold_augmentation`, the protocol vocabulary
  (`FoldAugRule`, `TrialCountRule`, `HorizonBlendIc`, `LearnProtocol`), the
  `LearnFitTrace` audit record and the `mean_date_ic` helpers.
- `atx-engine/include/atx/engine/learn/tcn_alpha.hpp`, `atx-engine/src/learn/tcn_alpha.cpp`:
  the inner purged validation split for the folds and for the deployed ensemble
  (`SeqValidationRule`, `SeqDeployRule`, `SeqFitProtocol`, `detail::inner_purged_split`),
  the protocol-driven trial count and blend, and the trace overloads.
- `atx-engine/include/atx/engine/learn/nn/loss.hpp`, `atx-engine/src/learn/nn/loss.cpp`:
  `IcReduction`, `IcLoss` per-date reduction, and `Loss::set_row_groups`.
- `atx-engine/include/atx/engine/learn/nn/trainer.hpp`, `atx-engine/src/learn/nn/trainer.cpp`:
  `RowGroups` and date-grouped minibatches, plus the documented checkpoint contract.
- `atx-engine/src/learn/linear_alpha.cpp`, `atx-engine/src/learn/gbt.cpp`: changes only
  at the fold-augmentation sites, the trial-count sites and the blend-IC sites, plus
  trace recording.
- `atx-engine/include/atx/engine/learn/linear_alpha.hpp`, `atx-engine/include/atx/engine/learn/gbt.hpp`:
  the declarations that had to change. Each config gained `LearnProtocol protocol{}`,
  which carries the versioned enums, and each fitter gained a `trace` overload.

Tests:

- New: `atx-engine/tests/learn/learn_w0l0_label_mutation_test.cpp`,
  `learn_w0l0_label_maturity_test.cpp`, `learn_w0l0_fold_local_aug_test.cpp`,
  `learn_w0l0_ic_loss_per_date_test.cpp`, `learn_w0l0_l08_protocol_test.cpp`.
- Changed: `atx-engine/tests/learn/latent_test.cpp` (fixture annotation, L-02; see
  Deviations).

## Acceptance table

| Plan acceptance item | Test(s) | Measured result | Status |
|---|---|---|---|
| Mutating test-fold labels leaves OOF predictions byte-identical | `LearnLabelMutationInvariance_Seq.TcnTestFoldLabelsDoNotReachFoldModel`, `.GruTestFoldLabelsDoNotReachFoldModel`, `LearnLabelMutationInvariance_Tabular.LinearTestFoldLabelsDoNotReachFoldModel`, `.GbtTestFoldLabelsDoNotReachFoldModel` | TCN, 4 of 4 folds: `n_test=12`, `max|dpred|=0`, and the OOF prediction bytes are identical for every test sample. Linear, 4 of 4 folds: `n_test=48`, identical. GRU and GBT: identical on all 4 folds. Legacy controls on the same mutation: TCN V1 `max|dpred|=1.45626`, GRU V1 `1.25447`, linear V1 `0.0562598`, GBT V1 `0.119193`. | MET |
| A held-out-label perturbation leaves fold training artifacts unchanged | Same four tests (artifact bytes), plus `LearnFoldLocalAug_Fit.HeldOutFeaturesAndLabelsCannotChangeFoldAug` | TCN fold artifacts (2 horizons x 2 members of member states = 1330 f64) are byte-identical on 4 of 4 folds. Linear fold artifacts (standardization + augmentation + coefficients = 17 f64) are byte-identical on 4 of 4 folds, even though the caller's own re-selection flipped (`aug_flipped=1`) in every fold. GBT forests are identical on all folds. Under V1 the artifacts differ (`EXPECT_FALSE(artifacts_identical)` passes). | MET |

Supporting coverage of the Build items:

| Build item | Test(s) | Result |
|---|---|---|
| Inner purged validation block carved from the train dates | `LearnLabelMutationInvariance_Seq.InnerValidationIsPurgedTrainBlock` | Every validation sample is on a non-test date. Every inner-train and validation date pair is at least H apart. |
| Label-maturity metadata, filter `r + H <= t - embargo` | `LearnLabelMaturity_Filter.*` (3 tests), `LearnLabelMaturity_Select.*` (4 tests) | Boundary is inclusive; there is no underflow or overflow at `usize`/`u16` extremes. Under V2 the selection is `{2,3}` (matured rows); legacy V1 gives `{0,1}` (it leaks). Perturbing all 40 unrealized labels leaves the selection unchanged. The embargo shifts the cutoff. An unannotated matrix selects nothing. |
| Fold-local augmentation fitting | `LearnFoldLocalAug_Fit.*` (5), `LearnFoldLocalAug_Linear.*` (2), `LearnFoldLocalAug_Gbt.*` (1) | A held-out perturbation leaves the fold augmentation unchanged, while a train-row perturbation moves it. Passing pairs `{0,1}` or `{3,4}` gives identical fold artifacts and `oos_score_series` under V2, and different ones under V1. V1 reproduces the verbatim copy. |
| Deployed ensemble selects on inner validation | `LearnLabelMutationInvariance_Deploy.DeployCheckpointUsesInnerValidation`, `.RejectsOutOfContractValFraction` | The validation block holds the latest dates. With inverted labels in the block, the deployed states equal a zero-epoch fit byte-for-byte (the checkpoint was chosen on the block). Legacy `TrainLossV1` keeps trained states. A bad `inner_val_frac` or `cpcv.embargo` returns an error. |
| `IcLoss` uses date-grouped batches | `LearnIcLossPerDate_Loss.*` (4), `LearnIcLossPerDate_Trainer.*` (4) | On the market-timing fixture the per-date loss is 2.0 while the pooled loss is below 0.25. The gradient matches finite differences to 1e-7. Every trainer minibatch holds whole dates. The legacy trainer path is byte-identical without groups. Grouped IC leaves the date-constant weight within 1e-9 of its initial value, while pooled IC moves it by more than 1e-2. |
| Trial count = configurations, not folds x horizons | `LearnIcLossPerDate_TrialCount.*` (3) | Linear and GBT: 1 under V2, 10 (5 folds x 2 horizons) under V1. GRU: 1 under V2, 3 under V1. Zero fits give 0 trials. |
| (L-08) Horizon blend without pooled Pearson | `LearnIcLossPerDate_HorizonBlend.*` (3) | Weights equal the rule statistic rebuilt from the trace (both rules). On the date-level horizon, V1 weight is above 0.5 and V2 weight is below 0.25. |

## Defect table

| ID | Status | How / where |
|---|---|---|
| L-01 | CLOSED | `tcn_alpha.cpp` `fit_seq_alpha`: under `SeqValidationRule::InnerPurgedV2` (the default), each fold checkpoints on `detail::inner_purged_split`. That block is the latest `inner_val_frac` (0.2) of the fold's train dates. The split is purged against the block with `eval::detail::purged_embargoed_train` and uses the outer CPCV embargo. The test fold is only predicted. A degenerate split skips the fold, so the trainer never silently falls back to training loss. `nn/trainer.hpp` documents the checkpoint contract, and the trainer checkpoint code at `trainer.cpp:163-170` is unchanged apart from being routed through `eval_loss`. `TestFoldV1` reproduces the old selection. |
| L-02 | CLOSED | `FeatureMatrix::label_horizons` plus `label_matured(r, H, t, e)` (`r + H <= t - e`, overflow-safe). `select_interactions` defaults to `LabelMaturityRule::MaturedV2` and returns nothing for an unannotated matrix. `FiniteLabelV1` is the legacy behaviour. Populating the field in `build_features` is an integration note for W1-L1 (see below). |
| L-03 | CLOSED | `linear_alpha.cpp` and `gbt.cpp` fold loops call `fold_augmentation(..., cfg.protocol.fold_aug)`. Under `FoldAugRule::FoldLocalV2` (the default), `fit_fold_augmentation` refits PCA (same k) and re-selects interactions (same top-m) on the fold's train rows. The label channel is `fold_selection_label`, which uses Y[0] only when its span nests inside the fold's purged span. Explicit `interactions_fixed` pairs are reused. The deployed model keeps the caller's augmentation. `FullWindowV1` is legacy. |
| L-07 | CLOSED | `fit_seq_alpha` deploy section: under `SeqDeployRule::InnerValV2` (the default), the deployed ensemble trains on the window's inner-train dates and checkpoints on its latest-date block, purged at the longest horizon that carries blend weight. A degenerate split leaves the model undeployed (`predict_nn` returns 0) rather than selecting on training loss. `TrainLossV1` is legacy. |
| L-08 (trial count) | CLOSED | `TrialCountRule::PerConfigurationV2` gives one fit call one trial, via `detail::protocol_trial_count`. It is used in `linear_alpha.cpp`, `gbt.cpp` and `tcn_alpha.cpp`; `PerFoldFitV1` is legacy. Registry-side accounting belongs to W0-E0b. |
| L-08 (IcLoss) | CLOSED | `IcLoss` with `IcReduction::PerGroupMeanV2` (the default) computes `1 - mean_g rho_g` over the batch's date groups. `nn::train(..., RowGroups)` builds minibatches of whole dates and passes the labels to the loss. `PooledV1` and "no groups" reproduce the old value byte-for-byte. |
| L-08 (horizon blend, pooled Pearson) | CLOSED | `HorizonBlendIc::MeanDateIcV2` (the default) computes the blend IC as the mean per-date IC of the fold-averaged OOF prediction at horizon h (`detail::oof_mean_date_ic`) in all three fitters. `PooledPearsonV1` is legacy. |
| L-08 (autoencoder is not GKX) | DEFERRED to W3-L4 | Per the brief's lane note. `autoencoder_alpha.cpp` was not touched. |

## Evidence

All commands were run from `C:\atx-wt\pool-3` with `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'`
(free RAM before each build was between 2.8 and 5.3 GB).

Reconfigure (the groups differed):

```
powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev -Groups "learn"
-- Build files have been written to: C:/atx-wt/pool-3/build-equity
exit=0
```

Single-TU checks (each printed no errors or warnings):

```
powershell -NoProfile -File scripts\atx-build.ps1 check -Preset equity-dev atx-engine\src\learn\latent.cpp        -> exit=0
... check ... atx-engine\src\learn\linear_alpha.cpp  -> exit=0
... check ... atx-engine\src\learn\gbt.cpp           -> exit=0
... check ... atx-engine\src\learn\tcn_alpha.cpp     -> exit=0
... check ... atx-engine\src\learn\nn\loss.cpp       -> exit=0
... check ... atx-engine\src\learn\nn\trainer.cpp    -> exit=0
```

Owning target build (final):

```
powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-learn-tests
[1/3] Building CXX object atx-engine\tests\CMakeFiles\atx-engine-learn-tests.dir\learn\learn_w0l0_label_mutation_test.cpp.obj
[2/3] Linking CXX executable bin\atx-engine-learn-tests.exe
build exit=0
```

Anchored suites (final code):

```
powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^LearnLabelMutationInvariance_'
100% tests passed, 0 tests failed out of 9          exit=0
... -R '^LearnLabelMaturity_'
100% tests passed, 0 tests failed out of 7          exit=0
... -R '^LearnFoldLocalAug_'
100% tests passed, 0 tests failed out of 8          exit=0
... -R '^LearnIcLossPerDate_'
100% tests passed, 0 tests failed out of 14         exit=0
... -R '^Latent\.'
100% tests passed, 0 tests failed out of 5          exit=0
```

Whole owning executable (final code):

```
build-equity\bin\atx-engine-learn-tests.exe --gtest_brief=1
[==========] 187 tests from 31 test suites ran. (114699 ms total)
[  PASSED  ] 187 tests.
exit=0
```

(The base had 149 tests; this lane adds 38.)

Measured lines (`atx-engine-learn-tests.exe --gtest_filter="LearnLabelMutationInvariance_*"`, exit=0, 9 passed):

```
[W0-L0 tcn V2] fold=0 n_test=12 artifact_f64=1330 max|dpred|=0 identical=1
[W0-L0 tcn V2] fold=1 n_test=12 artifact_f64=1330 max|dpred|=0 identical=1
[W0-L0 tcn V2] fold=2 n_test=12 artifact_f64=1330 max|dpred|=0 identical=1
[W0-L0 tcn V2] fold=3 n_test=12 artifact_f64=1330 max|dpred|=0 identical=1
[W0-L0 tcn V1] fold=1 n_test=12 max|dpred|=1.45626 identical=0
[W0-L0 gru V1] fold=1 max|dpred|=1.25447
[W0-L0 linear V2] fold=0 n_test=48 artifact_f64=17 aug_flipped=1 identical=1
[W0-L0 linear V2] fold=1 n_test=48 artifact_f64=17 aug_flipped=1 identical=1
[W0-L0 linear V2] fold=2 n_test=48 artifact_f64=17 aug_flipped=1 identical=1
[W0-L0 linear V2] fold=3 n_test=48 artifact_f64=17 aug_flipped=1 identical=1
[W0-L0 linear V1] fold=1 n_test=48 max|dpred|=0.0562598
[W0-L0 gbt V1] fold=1 max|dpred|=0.119193
[  PASSED  ] 9 tests.
```

"It fails today": the brief expected `LearnLabelMutationInvariance_*` to fail on the
base. Each invariance test is paired with the legacy rule (`TestFoldV1`,
`FullWindowV1`, `TrainLossV1`), which reproduces the base behaviour, and asserts that
the artifact moves. The V1 lines above are that failure, measured.

Downstream check (not an owning target; built once to check integration):
`atx-impl-tests` consumes `fit_stack` → `fit_linear`/`fit_gbt` with the new defaults.

```
powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests     -> exit=0
build-equity\bin\atx-impl-tests.exe --gtest_filter="*Combine*:*Stack*:*Regime*:*Learn*" --gtest_brief=1
[==========] 53 tests from 12 test suites ran. (21639 ms total)
[  PASSED  ] 53 tests.
exit=0
```

Diagnostics (failed attempts; they support no claim):

- The first background build (started before the edits, to warm the cache) failed at
  link with an undefined `select_interactions` symbol. I had changed its signature
  while that build was running. Later builds were clean.
- The first full run after the implementation had 146 of 149 passing. The three
  failures were `Latent.Interactions_*`: their hand-built matrices carry no label
  horizon, so `MaturedV2` refused them. Fixed by annotating the fixture (Deviation 4).
- One of my new tests called `MseLoss::value` with mismatched shapes (a test bug that
  tripped the release `ATX_CHECK`), and a scripted edit split a string literal across a
  CRLF. Both were fixed in the test file before the final runs.

## Golden-digest old→new table

None. The learn target pins no golden digest (`learn_integration_test` compares two
runs of `full_pipeline_digest` for equality, not against a constant). The unpinned
`full_pipeline_digest` value and learned-model `trial_count` / `blend_w` values
change by design under the new defaults (L-08). Each legacy value can be reproduced by
setting the V1 enum on `cfg.protocol`.

## Deviations from brief

1. **Declarations in `linear_alpha.hpp` / `gbt.hpp`.** Each config gained
   `LearnProtocol protocol{}`, because RULES §2 requires a versioned enum for the changed
   numeric defaults. Each fitter gained a `LearnFitTrace*` overload, because
   acceptance needs per-fold OOS predictions and artifacts to be observable. The
   existing 3-argument signatures are unchanged.
2. **Shared vocabulary in `latent.hpp`.** The protocol enums, `LearnProtocol` and
   `LearnFitTrace` live there because it is the one owned header that the linear, GBT
   and sequence fitters all include. I did not create a new header.
3. **L-08 horizon blend.** The plan's Build list names only the trial count and IcLoss.
   The findings row also cites the pooled-Pearson blend at owned sites, so I fixed it
   behind `HorizonBlendIc` (V1 is legacy).
4. **Existing test changed (L-02).** `latent_test.cpp` `make_fm` now annotates its
   one-date label (`label_horizons = {1}`). The expectations of the three interaction
   tests are unchanged. `Interactions_TailNaNLabel_StillRanks` now passes
   `FiniteLabelV1` explicitly so that its non-finite-label filter stays load-bearing:
   under `MaturedV2` maturity alone already removes the tail.
5. **Deploy trade-off (L-07).** The deployed sequence ensemble no longer trains on the
   latest `inner_val_frac` of dates (plus the purge gap). Those dates are its checkpoint
   block, the same protocol the CPCV folds evaluate. A degenerate inner split skips the
   fold or leaves the model undeployed. It never falls back to test-loss or train-loss
   selection.
6. **New error path.** `fit_tcn/gru/attn` now reject `cpcv.embargo` outside [0, 1]
   (NaN included), because the embargo length is converted to `usize`.
7. **Extra build.** Beyond the owning target, I built `atx-impl-tests` once (RAM was
   5.3 GB free) to confirm that the stack/combine consumers stay green.

## Integration notes

- **W1-L1 (`feature_matrix.cpp`, not owned):** `build_features` should set
  `fm.label_horizons = spec.horizons`. `learn::meta_features_from_pool` in
  `ensemble.cpp` should set it from its `horizons` argument. Until then,
  `select_interactions` returns no pairs on those matrices under the default
  `MaturedV2`. No production code calls it today; only tests and the Python
  `LatentAugmentation` binding touch it.
- **W0-E0b:** a learned model's `trial_count` is now 1 per `fit_*` call (one
  configuration). Registry-side accounting must count configurations and sweeps.
  `ensemble.cpp` `fit_regime_nonlinear` sums per-regime counts, and `nn_gate` sums
  candidate counts; both now sum configurations.
- **W3-L4:** the autoencoder is still mislabelled as GKX (L-08 tail). It also calls
  `nn::train(X, X, X, X)`, so it selects its checkpoint on training reconstruction
  loss. That is the same pattern as L-07 and belongs with the autoencoder rework.
- **G0:** learned-model DSRs will rise relative to base runs because N is now 1 per
  configuration instead of folds x horizons. Horizon blends change (per-date IC), and
  the sequence OOF numbers change (inner validation). These are intended truth deltas.
- **Python bindings** (not owned): `LinearAlphaCfg.protocol`,
  `LatentAugmentation.interactions_fixed` and `FeatureMatrix.label_horizons` are not
  exposed. Python callers get the V2 defaults.
- Pre-existing, not changed: `LinearAlphaCfg::use_ridge_baseline` and `master_seed`
  have no default initializers.

## Ledger candidates

1. Pearson IC is scale-invariant. A per-date `IcLoss` has zero gradient on a
   date-constant feature and on the scale of a lone varying feature, so an IcLoss
   training test needs at least two within-date features.
2. The W0-L0 learn defaults can be switched back to the legacy numbers through
   `cfg.protocol` V1 enums: `FoldAugRule::FullWindowV1`, `TrialCountRule::PerFoldFitV1`,
   `HorizonBlendIc::PooledPearsonV1`, `SeqValidationRule::TestFoldV1`,
   `SeqDeployRule::TrainLossV1`, `IcReduction::PooledV1` and
   `LabelMaturityRule::FiniteLabelV1`.
3. `select_interactions` selects nothing on a `FeatureMatrix` without
   `label_horizons`, and `build_features` does not set that field yet (W1-L1).

## Fix pass 1

Addresses `.superpowers/sdd/w0/lane-l0-review.md` (reviewed SHA `1a241884`). Fixer base:
`9c1d05f2`. All three findings fixed; no existing test changed, weakened or skipped.

| Finding | Severity | Change | Evidence |
|---|---|---|---|
| `nn::train(..., RowGroups{{}, gv})` aborts on stale validation labels | major | `trainer.cpp` `run_epoch`: every training batch now sets the loss's groups explicitly — the batch's labels when `groups.train` is non-empty, `set_row_groups({})` otherwise. Leftover epoch-0/checkpoint `groups.val` labels can no longer reach a training batch. The `RowGroups` contract in `trainer.hpp` now lists all four combinations. | New `LearnIcLossPerDate_Trainer.UngroupedTrainWithGroupedValDoesNotAbort` runs a real `IcLoss` with batch 8 != n_val 6, then runs a spy with batch 6 (== n_val, the silent mis-grouping case) and batch 8. Every training batch sees no groups, and all 2 x (1 + 4) validation passes see `gv`. New `...UngroupedTrainWithGroupedValSelectsOnPerDateLoss` runs a real `IcLoss` with batch 5 != n_val 12. The kept state's per-date validation loss equals the minimum recorded checkpoint loss, and its pooled loss differs by more than 1e-3, so selection was grouped by date. **Mutation check:** with the `else` branch disabled, both tests fail with `[loss.cpp:255] CHECK failed: groups_.size() == static_cast<std::size_t>(B)` (SEH 0xc000001d). They pass with the fix restored. |
| `RowGroups{g, {}}` with a non-empty `x_val` selects on a pooled mixed-date IC | minor | `trainer.cpp` `train()`: this combination is rejected with `InvalidArgument` and a message ("groups.train set but groups.val empty with a validation design"). An empty `x_val` stays allowed, because the checkpoint then scores train with `g`. Documented in `trainer.hpp`. No production caller passes `RowGroups`, so only tests are affected. | New `LearnIcLossPerDate_Trainer.RejectsGroupedTrainWithUngroupedVal` checks two cases: (a) a 5-row `x_val` returns `InvalidArgument`; (b) a 0-row `x_val` succeeds, and every checkpoint `value()` sees `g`. `RejectsMismatchedGroupLengths` is unchanged and still fails on its length check first. |
| Hand-built non-clique pair list treated as a top-m recipe | minor | `latent.cpp`: new `detail::is_selected_clique(pairs)` is true only for the exact canonical output of `select_interactions_on_rows`: every C(m,2) pair `(F[i], F[j])` with i < j, in ascending order, m >= 2. `fit_fold_augmentation` refits only when `!interactions_fixed && is_selected_clique`. Any other list is reused verbatim in every fold. Documented in `latent.hpp` on `LatentAugmentation` and `fit_fold_augmentation`. Empty lists behave as before, and so does every existing caller: all existing fixtures and `select_interactions` outputs are canonical cliques. | New `LearnFoldLocalAug_Fit.OnlyCompleteCanonicalCliqueIsASelection` covers positive cases, `{(0,1),(2,3)}`, a missing pair, the wrong order, a > b, a self pair, duplicates, and real `select_interactions` output for m = 2..5. New `LearnFoldLocalAug_Fit.NonCliquePairsAreReusedVerbatimNotRefit` checks that `{(0,1),(2,3)}` is kept and that the complete 4-clique IS re-selected and differs (non-vacuous). New `LearnFoldLocalAug_Linear.NonCliquePairsReachEveryFoldUnchanged` checks that all 4 `fit_linear` fold artifacts carry the deployed pairs byte for byte. |

Evidence (from `C:\atx-wt\pool-3`, `CMAKE_BUILD_PARALLEL_LEVEL=2`, >= 4.5 GB free RAM):

```
scripts\atx-build.ps1 build -Preset equity-dev atx-engine-learn-tests
[11/12] Linking CXX executable bin\atx-engine-learn-tests.exe          build exit=0 (/W4 /WX clean)

scripts\atx-build.ps1 -Ctest -Preset equity-dev -R <suite>
^LearnIcLossPerDate_           -> 100% tests passed, 0 tests failed out of 17   exit=0   (was 14)
^LearnFoldLocalAug_            -> 100% tests passed, 0 tests failed out of 11   exit=0   (was 8)
^LearnLabelMutationInvariance_ -> 100% tests passed, 0 tests failed out of 9    exit=0
^LearnLabelMaturity_           -> 100% tests passed, 0 tests failed out of 7    exit=0
^Latent\.                      -> 100% tests passed, 0 tests failed out of 5    exit=0

build-equity\bin\atx-engine-learn-tests.exe --gtest_brief=1
[==========] 193 tests from 31 test suites ran. (72733 ms total)
[  PASSED  ] 193 tests.                                                   exit=0   (was 187)
```

The acceptance measurements are unchanged. The mutation-invariance lines are identical to the review: tcn V2 `identical=1`, tcn V1 `max|dpred|=1.45626`, gru V1 `1.25447`, linear V2 `identical=1`, linear V1 `0.0562598`, gbt V1 `0.119193`. Golden-digest table: none.
