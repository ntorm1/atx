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
    validation run. Alternatively, rerun TRAIN with this binary; with a warm cache that is cheap.
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
