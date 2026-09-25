# Lane W0-L0 review

## Verdict

BLOCK. There is one major finding: the new `nn::train(..., RowGroups)` API aborts on an input its
own contract documents as valid. There are no blockers. Everything else checks out: both plan
acceptance items are met and proven, every cited ID is closed or properly deferred, file
ownership is within scope, no test was weakened, and the owning executable passes whole.

## Reviewed SHA

`1a24188454aebca609fa75748590033c748803aa` (lane head `feat/w0-l0`; code head `3ec645a8`).
Integration branch `feat/w0-integration` = base `458d0bef` (the merge-base equals the base, so no
pre-merge was needed).

## Evidence

The reviewer ran every command below from `C:\atx-wt\pool-3`, with `CMAKE_BUILD_PARALLEL_LEVEL=2`
and 5.17 GB of free RAM.

```
powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-learn-tests
[9/10] Linking CXX executable bin\atx-engine-learn-tests.exe
exit=0            (build-equity ATX_TEST_GROUPS=learn; learn sources already up to date = built /W4 /WX)

build-equity\bin\atx-engine-learn-tests.exe --gtest_brief=1
[W0-L0 tcn V2] fold=0..3 n_test=12 artifact_f64=1330 max|dpred|=0 identical=1
[W0-L0 tcn V1] fold=1 n_test=12 max|dpred|=1.45626 identical=0
[W0-L0 gru V1] fold=1 max|dpred|=1.25447
[W0-L0 linear V2] fold=0..3 n_test=48 artifact_f64=17 aug_flipped=1 identical=1
[W0-L0 linear V1] fold=1 n_test=48 max|dpred|=0.0562598
[W0-L0 gbt V1] fold=1 max|dpred|=0.119193
[==========] 187 tests from 31 test suites ran. (100822 ms total)
[  PASSED  ] 187 tests.
exit=0

scripts\atx-build.ps1 -Ctest -Preset equity-dev -R <suite>
^LearnLabelMutationInvariance_ -> 100% tests passed, 0 tests failed out of 9   exit=0
^LearnLabelMaturity_           -> 100% tests passed, 0 tests failed out of 7   exit=0
^LearnFoldLocalAug_            -> 100% tests passed, 0 tests failed out of 8   exit=0
^LearnIcLossPerDate_           -> 100% tests passed, 0 tests failed out of 14  exit=0
^Latent\.                      -> 100% tests passed, 0 tests failed out of 5   exit=0
```

The measured numbers match the report's claims line for line.

## Findings

path:line | severity | problem | required fix
---|---|---|---
`atx-engine/src/learn/nn/trainer.cpp:121` (with `:232-236`; contract at `include/.../nn/trainer.hpp:109`) | major | `RowGroups{train = {}, val = g}` aborts. The header says an empty span means "no groups" for that design, so this input is valid, and `train()` accepts it (only the lengths are validated). Here is what happens. `eval_loss` sets the loss's groups to `groups.val` for the epoch-0 baseline. `run_epoch` then calls `set_row_groups` only `if (!groups.empty())`, and `groups.train` is empty. So every training minibatch reaches `IcLoss::grad`/`value` with the stale validation labels (length n_val). `ATX_CHECK(groups_.size() == B)` (`loss.cpp:232,255`) then calls `std::abort()` in every build. If a batch happens to have n_val rows, it silently groups the batch by the validation labels instead. The owning suite only passes `{{}, short_g}` with a length mismatch, which returns an error before this path runs, so no test covers it. | In `run_epoch`, set the batch's groups on every batch: the batch labels when training is grouped, `set_row_groups({})` otherwise. Alternatively, reject `train empty, val non-empty` with `InvalidArgument` and document that. Add a `LearnIcLossPerDate_Trainer` test that trains `IcLoss` with `RowGroups{{}, gv}` and batch size != n_val and expects no abort, plus per-date validation grouping.
`atx-engine/src/learn/nn/trainer.cpp:232` | minor | `RowGroups{train = g, val = {}}` with a non-empty `x_val` trains on whole-date batches, but the checkpoint pass scores the validation design with a pooled, mixed-date IC (the groups are cleared). That is the L-08 pattern, moved to checkpoint selection. It is contract-consistent but a silent trap. | Reject `train grouped, val ungrouped` when `x_val` is non-empty, or document it explicitly in the `RowGroups` contract.
`atx-engine/src/learn/latent.cpp:210` (with `latent.hpp:114`) | minor | `interactions_fixed` defaults to `false`, so any hand-built pair list is treated as a top-m clique. For example, `{(0,1),(2,3)}` gives m = 4, and each fold refits 6 pairs while the deployed model uses 2. The OOF series then evaluates a different model structure from the deployed one. This is documented in the header, and no production caller builds a non-empty augmentation (pipeline and ensemble pass `empty_aug`). | Assert or document that `interactions_fixed = false` requires a complete clique; for example, check `pairs.size() == C(m,2)` and treat anything else as fixed. Or record the selection m explicitly on `LatentAugmentation`.

## Acceptance items (re-run and test code read)

- **A1: mutating test-fold labels leaves OOF predictions byte-identical. MET.**
  - Tests: `LearnLabelMutationInvariance_Seq.{Tcn,Gru}TestFoldLabelsDoNotReachFoldModel` and `_Tabular.{Linear,Gbt}TestFoldLabelsDoNotReachFoldModel`.
  - Every fold is covered (4 of 4). Each test compares both the per-fold `test_pred` and the trace `oof_pred` with `memcmp`.
  - The mutation is large (`-4y+1.5`, or a label driven by features 0 and 1 scaled by 50). In the tabular tests the caller's own augmentation is refit on the mutated labels (`aug_flipped=1`).
  - Not vacuous: each test is paired with a V1 control on the same fixture and mutation (`TestFoldV1` / `FullWindowV1`), and the control asserts that the output moves (measured deltas 1.456, 1.254, 0.056, 0.119). The V1 paths reproduce the base code; the reviewer diffed them against the base and they are equivalent.
- **A2: a held-out-label perturbation leaves fold training artifacts unchanged. MET.**
  - The same four tests compare the fold artifacts byte for byte:
    - TCN and GRU: member states, 1330 f64.
    - Linear: fold standardization, fold augmentation and coefficients, 17 f64.
    - GBT: every node of the fold forest.
  - `LearnFoldLocalAug_Fit.HeldOutFeaturesAndLabelsCannotChangeFoldAug` also perturbs the held-out features, labels and a NaN label, and shows that a perturbation of a train row does move the fold augmentation.

## Defect IDs (code read at the cited sites)

- **L-01: CLOSED.**
  - `tcn_alpha.cpp` fold loop, `SeqValidationRule::InnerPurgedV2` (the default): `inner_purged_split` takes the last ceil(0.2n) of the fold's train ordinals, purged against them with `eval::detail::purged_embargoed_train`.
  - Validation is a subset of the outer-purged train set, so it never overlaps the test labels.
  - A fold with no validation rows is skipped (`nval == 0`), so it never falls back to training loss.
  - `LearnLabelMutationInvariance_Seq.InnerValidationIsPurgedTrainBlock` checks that validation dates are never test dates and that every validation/fit date pair is at least H apart.
- **L-02: CLOSED.**
  - `label_matured` is `r <= (t-e) - H` with guarded subtractions. `select_interactions` uses `MaturedV2` by default and fails closed on an unannotated matrix.
  - No production caller of `select_interactions` exists (the reviewer grepped for it), so the closure has no runtime regression.
  - Populating `build_features` is correctly routed to W1-L1.
- **L-03: CLOSED.**
  - The `linear_alpha.cpp` and `gbt.cpp` fold loops call `fold_augmentation(..., FoldLocalV2)` on `f.train_rows` only.
  - `fold_selection_label` uses Y[0] only when its span nests inside the horizon-h purge.
- **L-07: CLOSED.** Under `SeqDeployRule::InnerValV2`, the deploy checkpoints on the latest-date block, purged at the largest horizon with non-zero blend weight. `DeployCheckpointUsesInnerValidation` shows that the checkpoint follows the block (it equals a zero-epoch fit), while V1 keeps trained states.
- **L-08: trial count CLOSED.** `protocol_trial_count` is used at all three fitter sites.
- **L-08: horizon blend CLOSED.** It uses `oof_mean_date_ic`. The test rebuilds both statistics from the trace, and the date-level-horizon fixture shows V1 above 0.5 and V2 below 0.25.
- **L-08: IcLoss CLOSED.** The per-date reduction is correct: the gradient `-(1/G)(tc/root - rho*pc/spp)` matches finite differences to 1e-7. The trainer's whole-date batches are proven by a spy loss. No production code uses `IcLoss` yet; that is infrastructure, not a gap. The RowGroups abort is a separate finding (the major above).
- **L-08: autoencoder not GKX. DEFERRED to W3-L4,** per the brief's lane note. That is justified.

## Checked

- [x] `.agents/cpp/agent.md` §10 checklist applied to the diff:
  - No UB was found in the new code. The float-to-usize casts are guarded (`inner_val_frac` in (0, 0.5], `cpcv.embargo` in [0, 1] and NaN are rejected before `embargo_len_of`), and the maturity subtractions are guarded.
  - Every new `switch` is exhaustive over its enum, and each has a documented, conservative fallback.
  - Lifetimes: `RowGroups` holds spans that the caller keeps alive for the duration of `train()`. `GroupsGuard` clears the loss's groups on every exit path.
  - `[[nodiscard]]` is on the new functions.
  - `/W4 /WX`: the learn target and the engine library compiled clean.
  - Exception: the RowGroups contract break (the major finding).
- [x] The diff stays inside the brief's files-in-scope.
  - The `gbt.hpp` and `linear_alpha.hpp` declaration changes are justified: they carry the RULES §2 versioned enum and the trace overload, and they are declared as deviations.
  - Beyond that, the lane touched only new `learn_w0l0_*` tests, `latent_test.cpp` (an L-02 fixture annotation) and its own sdd report.
- [x] No test was weakened. `latent_test.cpp` only adds `label_horizons = {1}` and pins `FiniteLabelV1` in `Interactions_TailNaNLabel_StillRanks`, so the NaN filter still carries weight. The expectations are unchanged and tied to L-02. No `DISABLED_` or `GTEST_SKIP` was added.
- [x] Every changed numeric default has a V1 value:
  - `FoldAugRule::FullWindowV1`
  - `TrialCountRule::PerFoldFitV1`
  - `HorizonBlendIc::PooledPearsonV1`
  - `SeqValidationRule::TestFoldV1`
  - `SeqDeployRule::TrainLossV1`
  - `IcReduction::PooledV1`
  - `LabelMaturityRule::FiniteLabelV1`

  The reviewer read the V1 paths against the base: they are equivalent, including the no-groups trainer path. No golden digest is pinned in the learn target, so a re-baseline table of "none" is correct.
- [x] The evidence in the report matches its claims: the reviewer re-ran the whole executable and the five anchored suites, and the numbers are identical.

## Diagnostics

None. Every command the reviewer ran exited 0.

## Re-review 1

Reviewer: fresh fix-only re-reviewer. Reviewed SHA: `c962996a8cca2f98697dbce9ac1d2e8fac3863e9`. Fix range: `1a241884..c962996a` (commit `c962996a`, "fix pass 1").

**Verdict: APPROVE.** All three findings are fixed. The fix introduces no regression, and no test was weakened.

### Findings

1. **FIXED (major): a stale validation-groups abort with `RowGroups{train = {}, val = gv}`.**
   - `run_epoch` (`trainer.cpp:123-131`) now calls `loss.set_row_groups({})` before every ungrouped batch. The labels that `eval_loss` sets for validation (length n_val) can therefore never reach `IcLoss::grad` or `IcLoss::value` during training.
   - The `RowGroups` contract table in `trainer.hpp` documents this combination.
   - New tests:
     - `LearnIcLossPerDate_Trainer.UngroupedTrainWithGroupedValDoesNotAbort` runs a real `IcLoss` with batch 8, n_val 6 and n_train 20, so the batches are 8, 8 and 4. Before the fix this reached `ATX_CHECK` and aborted. A spy then checks, at `bs` = 6 (the silent mis-grouping case) and at `bs` = 8, that every training batch sees empty groups and all 10 validation passes see `gv`.
     - `UngroupedTrainWithGroupedValSelectsOnPerDateLoss` confirms that the kept state's per-date validation loss equals the recorded minimum and differs from its pooled loss by more than 1e-3.
   - For callers without groups the extra `set_row_groups({})` does not change the numbers: `NoGroupsIsByteIdenticalToLegacyTrainer` still passes byte for byte.
2. **FIXED (minor): selecting on a pooled validation IC with `RowGroups{train = g, val = {}}`.**
   - `train()` (`trainer.cpp:225-230`) now returns `InvalidArgument` when `groups.train` is non-empty, `groups.val` is empty and `x_val` has rows. The case with an empty `x_val` is still allowed; its checkpoint scores train grouped by `g`.
   - Both the header error list and the contract table document this.
   - `RejectsGroupedTrainWithUngroupedVal` covers both branches.
   - No source caller passes `RowGroups`. Only tests do, so nothing that already runs is newly rejected.
   - `RejectsMismatchedGroupLengths` is unchanged and still rejects both cases.
3. **FIXED (minor): a hand-built non-clique pair list was refit as a top-m selection.**
   - `fit_fold_augmentation` (`latent.cpp:236-244`) now refits only when `!interactions_fixed && detail::is_selected_clique(...)`. Any other list is reused verbatim.
   - `is_selected_clique` matches exactly what `select_interactions_on_rows` emits, which the reviewer read at `latent.cpp:94-111`: an ascending sorted feature set with every `(F[i], F[j])`, i < j, in nested-loop order. Every real selection output therefore still refits, and the fold-local (FoldLocalV2) leakage fix is preserved.
   - Empty lists behave as before, and so does the selection-returns-nothing case for m <= 1.
   - Tests:
     - `OnlyCompleteCanonicalCliqueIsASelection` checks the negative cases (partial, out of order, a > b, self pair, duplicates) and real `select_interactions` outputs for m = 2..5.
     - `NonCliquePairsAreReusedVerbatimNotRefit` is non-vacuous: the same four features as a full clique do refit, to a different top-4.
     - `LearnFoldLocalAug_Linear.NonCliquePairsReachEveryFoldUnchanged` checks that the fold artifacts carry the deployed structure in all 4 folds.

### Regression and test-integrity check

- The fix diff to the test files contains additions only: 6 new tests and one `#include <algorithm>`. No expectation changed, and nothing was deleted, disabled or skipped.
- `latent.hpp` and `trainer.hpp` changed only in comments and a new `detail::is_selected_clique` declaration. The signatures are unchanged.
- Callers of the changed APIs are `tcn_alpha.cpp`, `autoencoder_alpha.cpp` and `linear_alpha`. All of them are exercised by `atx-engine-learn-tests`.

### Commands run

- `Set-Location C:\atx-wt\pool-3; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-learn-tests`
  - Free RAM was 5.6 GB. The build exited 0 and linked `bin\atx-engine-learn-tests.exe`.
  - The executable's timestamp is later than the fixed sources.
- `build-equity\bin\atx-engine-learn-tests.exe --gtest_filter='LearnIcLossPerDate_*:LearnFoldLocalAug_*:Latent.*' --gtest_brief=1`
  - Result: 33 tests from 8 suites, PASSED.
- `build-equity\bin\atx-engine-learn-tests.exe --gtest_brief=1`
  - Result: **193 tests from 31 suites, PASSED**, exit=0. The previous count was 187; the difference is the 6 new tests.

No new findings.
