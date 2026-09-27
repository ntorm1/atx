# T14 report: validation-only binding of weights and field definitions

## T14

Branch `feat/mega-alpha-ic-validation-bind-20260927` in pool-4, one commit `f38e79ad` on root `55319cbd`. I did
not build or run anything here (lane rule).
- **Root build:** `atx-impl-strategy-ic-tests`, and `atx-equity-strategy-ic` for the binary.
- **Filter:** `--gtest_filter=StrategyIcRunner.*` (ctest `-R StrategyIcRunner` after building `atx-impl-tests`).
- **CMake:** no change.

Files: `atx-impl/src/strategy_ic_runner.{cpp,hpp}` and `atx-impl/tests/strategy_ic_runner_test.cpp`.

### M1: validation-only weights must name the frozen TRAIN orientations

A new function, `frozen_weights_binding`, runs after `frozen_train`, before any payload and also under
`--plan-only`. It applies whenever `--orientations` and `--composition-weights` are both given. It checks the
weights file's `provenance` object, which the T11 fitter already emits (`fit_composition_weights.py:1104-1120`).

- **`provenance.orientations_sha256` must be present and a valid hash.** Otherwise the run refuses with
  "validation-only composition weights need provenance.orientations_sha256 naming the frozen TRAIN orientations
  they were fit on".
- **Unweighted frozen source.** The value must equal `--orientations-sha256`, the artifact file SHA the runner
  binds. This is the same file SHA the fitter records.
  - Refusal: "composition weights provenance.orientations_sha256 must equal the frozen TRAIN
    --orientations-sha256".
- **Weighted frozen source** (the frozen TRAIN recipe itself pins `composition_weights_sha256`).
  - `frozen_train` already requires these exact weights bytes, so the provenance is already part of the frozen
    TRAIN recipe's identity.
  - Equality with `--orientations-sha256` is impossible by construction. The provenance names the *unweighted*
    TRAIN orientations the fit consumed. The weighted run's own `orientations.json` embeds its own recipe hash,
    which includes the weights pin, so its SHA always differs.
  - So here the value is required to be present and well formed, and the binding is recorded as
    `frozen-TRAIN-recipe-pins-these-weights`.
  - If root wants the strict reading ("every validation weights file equals `--orientations-sha256`"), the
    consequence is that weighted frozen sources can no longer be validated. That is a one-line change.
- **Fields (review recommendation).** `provenance.fields_manifest_sha256` must equal the frozen TRAIN recipe's
  train fields pin. When TRAIN pinned none, the key must be absent or null, which is what the fitter emits.
  - Refusal: "composition weights provenance.fields_manifest_sha256 must equal the frozen TRAIN fields manifest
    pin".
- **`train_manifest_sha256 == --train-sha256`** is still checked as before, in `composition_weights`.
- **Recorded in the validation `summary.json`** (and in the plan-only output) as `composition_weights`:
  - fields: `sha256`, `train_manifest_sha256`, `signs` (`TRAIN-orientation-signs` or `pinned-candidate-signs`),
    `provenance_orientations_sha256`, `provenance_fields_manifest_sha256` and `binding`;
  - the existing `composition_weights_sha256` and `composition_signs` keys remain;
  - a non-validation-only weighted run records `binding: "train-manifest-sha256;TRAIN-scored-in-this-run"`.
- **Recipe bytes are unchanged.**
- **Process gap.** Code cannot stop several fits being made from the same TRAIN orientations; each would pass.
  The weights SHA must still be ledgered before the single validation run.

### N3: the field-definition check without `--train-fields`

- **Recorded at TRAIN time.** A scored TRAIN run whose library declares extras and whose TRAIN fields manifest is
  pinned now writes `orientations.json["research_fields"] = {manifest_sha256: <train pin>, definitions: {name:
  definition}}`.
  - The definitions use the same compared keys as M5.
  - Nothing is added otherwise, so no-field outputs are unchanged.
  - The T11 fitter's `load_orientations` ignores unknown keys, and so does the runner's own frozen check.
- **Checked at validation time.** A new function, `frozen_field_definitions`, runs in validation-only mode after
  `frozen_train`.
  - If the frozen artifact carries the record, its `manifest_sha256` must equal the frozen recipe's train pin.
    Otherwise: "frozen TRAIN artifact research field record differs from its recipe pin".
  - Every declared extra's recorded definition must equal the validation manifest's. Otherwise: "research field
    '<name>' definition differs between the frozen TRAIN artifact and the validation fields manifest".
  - The M5 check against a live `--train-fields` manifest still runs first when that option is passed.
- **Artifacts written before T14 have no record.** They then require `--train-fields`/`--train-fields-sha256`,
  which is verified against the frozen pin and compared by M5. Otherwise: "frozen TRAIN artifact records no
  research field definitions; pass --train-fields/--train-fields-sha256 …".
  - **Action for root:** a v3 TRAIN artifact produced by the pre-T14 binary needs `--train-fields` in the
    validation run.
  - Rerunning TRAIN with this binary is the alternative, and cheap with a warm cache, but it changes the
    artifact. It adds the record to the hashed `orientations.json`, so its SHA changes. Weights fit against the
    old artifact then fail M1 and must be **refit** from the new TRAIN-only run's summary (corrected in fix
    round 1).
- **Recorded as** `summary.research_field_definitions_checked_against`, set to `frozen-TRAIN-artifact` or
  `train-fields-manifest`.

### N1: no change

The tripwire `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` lives in `strategy_ic_runner_test.cpp`.
- That file is built and run by `atx-impl-strategy-ic-tests`, which is where root's IC 60/60 figure comes from.
- It is also ctest-registered without a label, through `atx-impl-tests` (the `*_test.cpp` glob plus
  `gtest_discover_tests`), so `-R VmSourcesPinned` selects it.
- The residual risk is a lane that edits the 29 engine paths and gates only on atx-engine. That needs a process
  rule: add `--gtest_filter=StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` to such lanes' gate, and ledger
  it.

### Fixtures

- **`ValidationWeightsMustNameTheFrozenTrainOrientations`** (M1). Each refusal below is checked in plan-only and in
  real mode, with no output and an empty log:
  - no provenance, a non-object provenance, and a malformed SHA;
  - another fit's orientations SHA;
  - a fields SHA when TRAIN pinned none.

  The accepted file's plan and summary record equal the exact expected JSON, and pinned signs are recorded as
  such.
- **`ValidationOnlyChecksFieldDefinitionsAgainstFrozenTrain`** (N3, plus M1 with fields). It uses a TRAIN-only
  source that records its definitions. It checks:
  - validation-only without `--train-fields` passes, recording `frozen-TRAIN-artifact`;
  - a changed validation `units` is refused, and M5 refuses first when `--train-fields` is passed;
  - a pre-T14 artifact (record removed) is refused without `--train-fields` and passes with it
    (`train-fields-manifest`);
  - a record naming another manifest is refused;
  - weights provenance must name TRAIN's fields pin.
- **`ValidationOnlyResumeComposesCandidateCacheAndTrainBoundWeights`** (updated). Its weights now carry
  fitter-style provenance naming the unweighted source's orientations. It asserts the two binding records:
  unweighted source means provenance equality; weighted source means the recipe pins the weights.

## T14 fix round 1

Branch `feat/mega-alpha-ic-validation-bind-fix1-20260927` in pool-4, one commit `43cc33f3` on root `5e36ed6b`. I
did not build or run anything here (lane rule).
- **Build:** `atx-impl-strategy-ic-tests` and `atx-equity-strategy-ic`.
- **Filter:** `--gtest_filter=StrategyIcRunner.*`.
- **CMake:** no change.

### I1: strict M1 (root ruling)

- **The weighted-source exemption is gone.**
  - `frozen_weights_binding` now requires `provenance.orientations_sha256 == --orientations-sha256` for every
    validation-only run with pinned weights.
  - The fields pin check is unchanged.
- **A weighted frozen TRAIN artifact is refused in `frozen_train`, with or without weights.** This is before any
  payload, and also under `--plan-only`. Message: "frozen TRAIN artifact is a weighted run; validate from the
  unweighted TRAIN run whose orientations the weights were fit on (the weights' provenance.orientations_sha256)".
  - This replaces the old "validation must pin exactly the same --composition-weights" path, which now led nowhere.
  - `FrozenTrain::weighted` and the `pinned_signs` parameter of `frozen_train` were removed.
  - The `frozen-TRAIN-recipe-pins-these-weights` binding no longer exists.
- **Shipped flow:** unweighted TRAIN run (orientations O) → T11 fitter → weights W (provenance names O) →
  validation-only run with O + W.
  - The fixture now asserts that O + W reproduces the weighted TRAIN run's validation bytes exactly
    (`validation_combined.f64`, the finite mask, planned targets and daily IC), so no capability is lost.
- **The laundering bypass is fixtured.** An all-zero-provenance fit is scored in a weighted TRAIN-only run, then
  validated against that weighted artifact. It is refused in both plan-only and real mode.
  - The all-zero `other_fit` case against the unweighted source still refuses.

### Minors

- **M3 (duplication).** One `weights_summary(cfg, pinned, binding)` helper now builds the `composition_weights`
  record for both validation-only and joint runs.
  - It includes `provenance_*`, null when absent.
  - It derives the signs label from `pinned.signs` only.
- **M1 (coverage).**
  - The joint-run record `train-manifest-sha256;TRAIN-scored-in-this-run` is asserted on `weighted_source`.
  - The weighted-source `same`, `without` and `other` legs are now refusals.
  - The `frozen_field_definitions` refusals for non-object `definitions` and for a missing declared name are
    fixtured.
- **M2 (report).** The "rerun TRAIN" advice above now says the weights must be refit.
  - Required order: final TRAIN-only run with a binary at or after T14 → fit W from that run's summary → ledger
    W's SHA → the single validation-only run with that run's orientations + W.
- **⚠️1 (outside scope, not changed).** A joint TRAIN+validation run with `--composition-weights` still applies
  `train_manifest_sha256`-bound weights to validation without a provenance check. Its summary records
  `binding: "train-manifest-sha256;TRAIN-scored-in-this-run"`.
  - The deliverable's validation run must be validation-only.
  - A NAV consumer can require `run_mode == "validation-only-frozen-TRAIN"` and
    `composition_weights.binding == "provenance-orientations-equal-frozen-TRAIN-orientations-artifact"`.
  - Refusing weights in joint runs would be a separate ruling.
