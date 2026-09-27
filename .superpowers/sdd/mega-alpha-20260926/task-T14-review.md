# Task T14 review: IC runner binds validation-only weights and field definitions to frozen TRAIN

Scope: root commit 22eae712 (pool-4 f38e79ad), from `review-T14.diff`. Line numbers are from `git show 22eae712:<path>`. I did not build or run anything. Build and test evidence is root's: 43.1 s, `atx-impl-strategy-ic-tests` 62/62.

### Spec Compliance

- ❌ **One deviation from the M1 root ruling, on the weighted-frozen-source path (I1).**
  - In that path, `provenance.orientations_sha256` is only format-checked: `strategy_ic_runner.cpp:835` exempts it with `!frozen.weighted &&`.
  - So a weights file fit on **different** TRAIN orientations still passes, as does one naming any 64-hex value.
- ✅ **M1, unweighted frozen source.**
  - `frozen_weights_binding` (`:827-848`) refuses in each of these cases:
    - provenance is absent or not an object;
    - `orientations_sha256` is missing or malformed;
    - `orientations_sha256` differs from `--orientations-sha256` (`:835-837`).
  - The existing `train_manifest_sha256 == --train-sha256` check is unchanged (`:806-809`).
  - The binding runs before any payload and under `--plan-only` (`:1612-1616`, ahead of `:1623`).
  - The weights SHA and signs usage are recorded as `composition_weights{sha256, signs, provenance_*, binding}` in both the plan and `summary.json` (`:1636`, `:1678`).
- ✅ **N3.**
  - A TRAIN run with declared extras and a pinned TRAIN fields manifest now writes `orientations.json.research_fields{manifest_sha256, definitions}` (`:1709-1713`).
  - `definitions` is the M5 `field_definition` subset (`:632-643`, stored at `:716`), so role-specific coverage and counts are not compared.
  - `frozen_field_definitions` (`:854-876`) runs in validation-only mode without `--train-fields`. It refuses in three cases:
    - the record's `manifest_sha256` is not the frozen recipe's TRAIN pin;
    - a declared extra's definition differs from the validation manifest's;
    - the artifact is a pre-T14 one with no record and no `--train-fields`.
- ✅ **N1 (optional): no code change, and the rationale is correct.**
  - I checked `atx-impl/tests/CMakeLists.txt:40-54`. `atx-impl-tests` globs `*_test.cpp`, which includes `strategy_ic_runner_test.cpp`, and is built by default with unlabeled `gtest_discover_tests`. So `ctest -R VmSourcesPinned` selects the tripwire.
- ✅ **Extras.** Both are in scope:
  - the `fields_manifest_sha256` binding, which the T7 re-review-2 ruling recommended (it is stricter, because it binds to the frozen pin even without `--train-fields`);
  - a `composition_weights` record for non-validation-only weighted runs (`:1618-1622`).
- ✅ **Global constraint "nothing in validation mode refits or reorients".** T14 adds only checks and records. Signs still come from the frozen artifact or the pinned file.
- ⚠️ **Cannot verify from this diff:** the joint TRAIN+validation path (see ⚠️1 under Minor).

### Strengths

- **Correct ordering.** Every new refusal happens after `frozen_train` and before `roles.erase`, the plan branch and the output directory. The new fixtures check every refusal in both plan-only and real mode:
  - the M1 fixture asserts no output and an empty log (`test:1617-1624`);
  - the N3 fixture asserts no output (`test:1681-1688`).
- **The N3 record is sound.**
  - It is inside the externally pinned `orientations.json` bytes, so it is tamper-evident against `--orientations-sha256`.
  - It is cross-checked against the recipe's TRAIN pin, which the `lying` fixture pins (`test:1716-1719`).
  - Because it reuses the M5 `field_definition` subset, the real TRAIN and validation manifests will not refuse falsely.
- **Clean legacy handling.** A pre-T14 artifact needs `--train-fields`. `expect_frozen_fields` (`:276-277`) verifies that manifest against the frozen pin, and M5 (`:1603`) then compares definitions. The report tells root about this.
- **No existing consumer breaks.**
  - The fitter's `load_orientations` (`fit_composition_weights.py:180-196`) reads only named keys.
  - The fitter's `research_fields` read is from the runner summary's TRAIN role (`:303`), not from `orientations.json`.
  - `frozen_train` reads only named artifact keys.
- **The impossibility claim is right.** In the weighted path, equality with `--orientations-sha256` would need a SHA-256 fixed point: the weighted artifact hashes its recipe, the recipe pins the weights SHA, and the weights contain the provenance.
- **Disclosure.** The report documents the deviation and states the one-line strict alternative.

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

**I1. The weighted-frozen-source exemption makes the M1 refusal bypassable, and gains nothing in return** (`strategy_ic_runner.cpp:835`, `:846`; comment `:820-824`).

- **Answer to the controller's question.** The deviation is not equivalent to the ruling. Yes, a weight fit from different TRAIN orientations passes on this path.
  - `frozen_weights_binding` checks only that `orientations_sha256` is present and is a valid hash.
  - The TRAIN run that produced the weighted artifact never checks the provenance either: non-validation-only runs record only `train-manifest-sha256;TRAIN-scored-in-this-run` (`:1618-1622`).
  - Concretely, the fixture's own `other_fit` provenance (`std::string(64,'0')`, `test:1631`) is refused against an unweighted source. The same file is accepted after one TRAIN-only weighted run with it, followed by a validation-only run against that weighted artifact.
  - So the "refuse otherwise" clause of the ruling can be sidestepped with one extra TRAIN run.
- **What the weighted path does still bind** (equal to, or tighter than, the unweighted path):
  - The frozen recipe pins exactly one weights file (`:311-317`), so the "several fits, keep the best on validation" risk is no worse than in the unweighted path, where every fit made from O passes.
  - Library, TRAIN manifest and TRAIN fields pin are still enforced (`:806-809`, `:838-842`).
  - The fitter reads TRAIN only: it requires a TRAIN-only runner summary (`fit_composition_weights.py:283-287`).
  - So no validation leakage is opened. The global constraint "weights/signs/fields frozen on TRAIN" holds.
- **What the weighted path loses.** It loses the link from provenance to the frozen orientations, which is exactly what the ruling mandates. That link catches accidental mismatches. One example: weights fit on a TRAIN artifact from another runner/VM identity, or on a pre-T14 artifact, validated after root reruns TRAIN (see M2).
  - The comment's premise is also unenforced. It says the provenance names "the UNWEIGHTED TRAIN orientations the fit consumed". The fitter accepts any TRAIN-only run's orientations, weighted ones included.
- **The exemption buys nothing.**
  - The existing fixture proves that validation-only with the unweighted artifact O plus weights F produces byte-identical validation payloads to the weighted artifact W plus F: `validation_combined.f64`, the finite mask, planned targets and daily IC are all equal to `source` for `cold`, `warm` and `same` (`test:777-781`).
  - Weights and signs come from F, and the method recipe is canonical (`frozen_train` requires `recipe == method_recipe(...)`, `:326-332`).
  - So the strict reading removes no capability. The single validation run uses O+F, and a weighted TRAIN run can remain a TRAIN diagnostic.
- **Fix (recommended): take the strict reading.**
  - In validation-only mode, refuse a weighted frozen source that is paired with weights, using a specific message such as "a weighted frozen TRAIN artifact cannot bind its weights' provenance; validate against the unweighted TRAIN artifact named by provenance.orientations_sha256". In practice this means dropping `!frozen.weighted &&` and adding an explicit message for the weighted case.
  - Convert the `same` leg and the `without`/`other` refusals of `ValidationOnlyResumeComposesCandidateCacheAndTrainBoundWeights` (`test:774-812`), which currently resume from `weighted_source`, into a refusal plus the O+F equivalents.
- **Alternative, if weighted sources must stay validatable.**
  - Add a pinned `--weights-source-orientations PATH` plus its SHA256.
  - Require that SHA to equal `provenance.orientations_sha256`, the same schema, library and TRAIN manifest, and candidate rows (id, dsl_sha, sign) equal to the frozen weighted artifact's.
  - This restores equivalence, but costs a new option and roughly 25 lines.

#### Minor (Nice to Have)

**M1. Gaps in test coverage around the new branches.**
- The weighted path has no refusal case: absent provenance, or a wrong `fields_manifest_sha256` against a weighted source.
  - If root keeps the exemption, there is also no fixture documenting that foreign provenance is accepted there.
- The non-validation-only record `binding: "train-manifest-sha256;TRAIN-scored-in-this-run"` (`:1618-1622`) is never asserted, even though `weighted_source` produces it (`test:760`).
- The `frozen_field_definitions` refusals for a record with non-object `definitions`, or with a declared name missing from `definitions` (`:866-873`), are unfixtured. The `lying` case covers only the `manifest_sha256` arm.

**M2. The report's "or rerun TRAIN with this binary; cheap with a warm cache" is incomplete.**
- Rerunning TRAIN with a binary at or after 22eae712 adds the `research_fields` record to the hashed `orientations.json` (`:1709-1713`), so the artifact SHA changes.
- Weights already fit against a pre-T14 artifact then fail M1 on the unweighted path. They must be refit from the new TRAIN-only run's summary (the fitter binds `summary.orientations_artifact_sha256`, `fit_composition_weights.py:281-283`).
- Under I1 as it stands, a weighted route would silently accept the stale provenance.
- Required order: final TRAIN-only run with a binary at or after 22eae712, then fit, then ledger the weights SHA, then the single validation-only run.
  - progress.md already notes the first step. The refit dependency should be ledgered too.

**M3. Small duplication in the summary record.**
- The `composition_weights` record is built twice with a hand-copied key layout: `frozen_weights_binding` (`:843-847`) and the joint-run branch (`:1619-1621`).
- The signs label is derived from two different expressions (`pinned.signs.empty()` and `pinned_signs`).
- The legacy `composition_weights_sha256` and `composition_signs` keys are now redundant with it. They were kept, which is fine for compatibility.
- A small `weights_record(cfg, pinned, binding)` helper would keep the two in sync.

**⚠️1 (outside T14 scope; for root).** The ruling covers validation-only runs only.
- A joint TRAIN+validation run with `--composition-weights` still applies any `train_manifest_sha256`-bound weights to the validation role, with no provenance check. It writes `validation_combined.*`, as `weighted_source` does at `test:760`.
- The deliverable's single validation run must therefore be a validation-only run.
- Recommend that the NAV validation consumer accept only a combined artifact whose summary has:
  - `run_mode == "validation-only-frozen-TRAIN"`, and
  - `composition_weights.binding == "provenance-orientations-equal-frozen-TRAIN-orientations-artifact"`.
- Alternatively, refuse `--composition-weights` in runs that score both TRAIN and validation.

### Assessment

**Task quality:** Needs fixes

**Reasoning:**
- The unweighted-path M1 binding and the N3 frozen-definition check are correct, ordered before any payload and well fixtured.
- However, the weighted-source exemption reduces the ruling's orientations equality to a format check that any foreign fit passes after one TRAIN run.
- The strict one-line reading removes no capability: O+F already gives byte-identical validation output (`test:777-781`), so it should be adopted.
