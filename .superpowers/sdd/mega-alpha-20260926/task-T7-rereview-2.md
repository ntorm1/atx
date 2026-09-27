# Task T7 re-review, fix round 2 (fb037679..1c7827f0)

Line numbers below are from `C:/atx-wt/pool-2` HEAD 55319cbd. The runner, header and test files are byte-identical to 1c7827f0 (`git diff --stat 1c7827f0 HEAD -- atx-impl atx-engine atx-core` is empty). I did not build or run any test; build and test evidence is root's (33.1 s, 60/60).

### Finding Verdicts

- **I1: all referenced extras stayed resident for the whole candidate loop, so v3 would have needed 1,656,728,202 B at 4 workers, over the 1536 MiB cap.** ADDRESSED.
  - **Schedule.** `field_plan` (`strategy_ic_runner.cpp:454-491`, built once at `:552`) works as follows:
    - `needs[k]` is candidate k's extras mask, and `capacity` is the largest `popcount(needs[k])`.
    - At step k, a resident field with no reader at index k or later is dropped (`:473`).
    - A needed field that is not resident is loaded. At capacity, the loader first evicts the resident, non-needed field whose next use is farthest away (`:476-486`).
    - A victim always exists. If `popcount(resident) >= capacity`, then fewer than `popcount(needs[k]) <= capacity` needed fields are resident, because field g is needed and not resident. So at least one resident field is not needed.
    - Therefore `needs[k] ⊆ planned[k]` and `|planned[k]| <= capacity` hold for every k.
    - I hand-traced the fixture library (extras iv=0, mkt=1, si=2; needs 0,0,{1,2},{0},{},{2},{1}). The result is capacity 2 and loads mkt, si, iv (evicting mkt: next use 6 > 5), then mkt again, which is 4 loads. This matches `test:1406` and the order asserted at `test:1413-1426`.
  - **Runtime never evaluates with a needed field absent.**
    - `enter(k)` (`:1116-1125`) drops `resident_ & ~planned[k]`.
    - On a miss, `panel_for(k)` (`:1126-1157`) loads `needs[k] & ~resident_` before building or returning the panel.
    - So when the VM is constructed or reused at `:1251ff`, `needs[k] ⊆ resident_`.
    - As a second, loud guard, the VM resolves every program field by name on each evaluate (`vm.hpp:1128-1135`, `panel_.field_id` returns an error, not NaN).
  - **Runtime never uses a stale column.** Every change to `resident_` is preceded by `vm.reset(); panel_.reset();`: in `enter` at `:1119` (before `release`), in `panel_for` at `:1130` (before any `load_pinned_f64` resizes a column), and in `drop_all` at `:1159`.
    - `panel_` is only rebuilt over the current `resident_` (`:1146-1153`).
    - `columns_` is a fixed-size outer vector (`:1097`), so a load never moves the storage of other resident columns.
    - The only surviving pairings are "vm over `panel_`, where `panel_` covers the current resident set" and "vm over `role.panel` with `resident_ == 0`".
    - Declaration order in `score_role` is `role` (`:1306`), then `fields` (`:1312`), `pool` (`:1326`) and `vm` (`:1328`). Early returns therefore destroy vm before fields, and fields before role.
  - **Admission matches actual residency.**
    - Admission charges `cells × (8·capacity + 1)` (`:582`).
    - At runtime, `resident_ ⊆ planned[k]` after `enter`, and the loads are a subset of `needs[k] ⊆ planned[k]`. So at most `capacity` columns are held, plus one owned 1 B/cell presence mask; the old `panel_` is reset before the new one is built.
    - `create_borrowed` moves the mask in without copying it (`panel.cpp:71`).
    - Loads happen after `vm.reset()`, so the VM arena is not live during I/O.
    - `verify_fields` streams through a 64 KiB stack buffer (`sha256.cpp:166`) before the role opens.
    - Runtime loads never exceed planned loads: each planned residency interval can hold at most one runtime load of that field.
  - **Arithmetic.** 48 × 6,499,185 = 311,960,880, and 1,656,728,202 − 311,960,880 = 1,344,767,322 B (1282.5 MiB), which is under the 1,610,612,736 B cap. This is still an estimate; see ⚠️1.
  - **Byte identity.** The following hold:
    - base-only bytes in a fields run equal a plain run's (`test:905-911`, `test:1444-1446`);
    - uncached, cold and warm runs agree on every exact output, and the warm run loads 0 fields (`test:1449-1466`).
- **I2: `dsl_vm_semantics_version` was bumped by hand with no automated check.** ADDRESSED.
  - The pin is `dsl_vm_sources[29]` plus a SHA (`:70-101`), exposed through `ic_cache_vm_identity()` (`:1497`, `hpp:44-50`). The test is at `test:1569-1596`.
  - **Coverage.** I listed every non-standard `#include` of all 29 files. Every `atx/engine/...` include is itself listed, so the engine closure is complete. The runner's DSL compile path (`al::parse_expr`, `al::analyze`, `al::compile`, `:533-534`) is inside that closure (`bytecode.hpp:221`).
    - The 4 alpha TUs that are not listed (`oracle`, `segment_panel`, `streams`, `unparse`) include only their own unlisted headers and define nothing declared by a listed header.
    - `factory/ic_research.hpp` is correctly excluded, because it scores the signal after the cached raw bytes are produced.
    - `det_pool` is header-only (there is no `src/parallel/det_pool.cpp`).
  - **CRLF stability, verified.** I recomputed the digest with an independent Python script. It gives `51bc0b2e…d67e`, equal to the pin, both from the CRLF working tree (11,511 CRLF pairs) and from the LF git blobs at 1c7827f0, e885687b and fb037679.
    - The normalization strips only a CR that is followed by LF, so it is invariant under autocrlf (`core.autocrlf=true` here).
    - `ATX_IMPL_TESTS_DIR` is `${CMAKE_CURRENT_SOURCE_DIR}` with no trailing slash, so `parent_path()²` is the repo root.
  - **Legacy exemption.** `git diff --stat 6d85ac2a 1c7827f0` over alpha, parallel, `strategy_data` and atx-core shows only the known 5-line `vm.hpp` comment, so the keyless-sidecar exemption still holds.
  - Residual gaps are listed under New Breakage N1 and N2 (Minor).
- **Root report: `--workers 1` failed with `metadata missing/over1MiB`.** ADDRESSED.
  - The diagnosis is consistent. At w4 and w2 the train admission refused first (`progress.md:171-172`), so `bind_fields` and `pinned_json(DIR/manifest.json)` were first reached at w1.
  - A non-directory `--train-fields` now refuses by name (`:662-664`); the test is at `test:1515-1526`.
  - `metadata_text` now reports the path, and separates "missing or unreadable" (`:189`) from "empty or over 1 MiB (N B)" (`:191`). Any other cause will name its file.
  - I cannot see root's exact command line. If the rerun still fails, the new message is self-diagnosing.
- **M1: a residual path for picking weights on validation.** NOT ADDRESSED: it was deferred to a root ruling, which is correct (report `:432-433`). A recommended ruling is below.
- **M2: presence masking of extras was untested.** ADDRESSED.
  - `ExtraFieldsAreMaskedByRolePresence` (`test:1469-1488`) punches holes where `present=0`, asserts that the field file is finite there, and checks that the raw cached signal is NaN at exactly those cells and equals the file everywhere else.
- **M3: base candidate bytes in a fields run were uncompared.** ADDRESSED (`test:905-911` against a plain cached run; `base_mid` at `test:1444-1446`).
- **M4: stale header cache-layout comment.** ADDRESSED (`hpp:19-23`).
- **M5: field definitions were not checked across roles.** ADDRESSED as specified, with one caveat (N3).
  - `field_definition` (`:623-635`) and `same_field_definitions` (`:637-644`, called at `:1528`) implement the check; the test is at `test:1490-1513`.
  - The compared keys are role-invariant in the producer, so the real TRAIN and validation manifests will not falsely refuse:
    - `units`, `clock`, `staleness`, `source_columns`, `point_in_time`, `non_pit_aspects` and `definition` come from the static `FIELDS[name]` spec (`prepare_research_fields.py:1158-1167`).
    - `plausibility` `min`/`max`/`inclusive`/`rule` are constants (`:795-797`, `:818`, `:866-869`) merged at `:1170`.
    - Role-specific coverage and counts are excluded.
- **M6: public `vm.hpp` touched (comment only).** NOT ADDRESSED, and correctly left: root imported it knowingly (`progress.md:100-101`), and changing it now would move the new pin. No action is needed.
- **M7: the report overstated the lookup.** ADDRESSED. The fix-round-2 report records the correction (report `:434-436`); the code was already correct.
- **M8: T1 minors carried forward.** Partly addressed; NOT ADDRESSED in full.
  - T1 M2 (fail-fast sidecar preflight) is done: `cache_plan` runs before the role payload (`:1300-1304`), and the test is at `test:1528-1567`.
  - Half of T1 M6 is done (`cache_preflight` path-is-file, device-name id, corrupt last sidecar, truncated payload).
  - Not done: T1 M3 (hex helper, which belongs in atx-core), T1 M5 (declined; the remaining uses are ranked blends), and T1 M6's unit-level composition refusals (T1-owned file).
  - Each has a stated reason and none blocks.

**M1 in one sentence.** In validation-only mode, when the frozen TRAIN run was unweighted, `composition_weights` (`:795-798`) accepts any weights file whose self-declared top-level `train_manifest_sha256` equals `--train-sha256`. `frozen_train` (`:304-313`) pins weights exactly only when the TRAIN source was itself weighted.
- Nothing ties such a file to the frozen TRAIN run. So several fits can each be run on validation, and the best one kept; that is auditable only through the recipe's weights SHA.
- **Recommended root ruling: adopt the tightening, as a small non-blocking T7 follow-up that must land before the v3 validation run.**
  - In validation-only mode with pinned weights and an unweighted frozen source, require `weights.provenance.orientations_sha256 == --orientations-sha256`. When `--train-fields-sha256` is given, also require `provenance.fields_manifest_sha256 == --train-fields-sha256`.
  - The T11 fitter already emits both keys (`fit_composition_weights.py:1117-1119`), so no current artifact breaks.
  - Pair this with a process rule: ledger the final weights SHA and the construction before the single validation run. Code can bind the weights to the frozen TRAIN run, but it cannot prove the fit never looked at validation.
  - A stricter alternative also works and is cheap with a warm cache: refuse unweighted-source-plus-weights, which forces a weighted TRAIN rerun whose recipe pins the weights.

### New Breakage in the Fix Diff

- **N1 (Minor). The tripwire only fires when someone runs a target that is excluded from the default build and has no label.**
  - `atx-impl-strategy-ic-tests` is `EXCLUDE_FROM_ALL` with no ctest label (`atx-impl/tests/CMakeLists.txt:95-100`).
  - A lane that edits a listed engine file and gates only on atx-engine tests (for example, the T5 `vec_avg` member-mask question) will not trip it. A rebuilt `atx-equity-strategy-ic` would then serve pre-change cache entries under an unchanged identity until someone runs this test.
  - Fix, either one:
    - **Process:** add `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` to the gate of any lane that touches the 29 paths, and ledger that rule.
    - **Code:** hash the 29 files at configure time (`file(SHA256)` plus `CMAKE_CONFIGURE_DEPENDS`) into a compile definition, and have the runner refuse `--candidate-cache` when it differs from `dsl_vm_sources_sha256`.
- **N2 (Minor). The closure check has blind spots** (`test:1582-1591`).
  1. It scans only the `#include "atx/engine/` spelling, not `<atx/engine/...>` or `# include`.
  2. It never checks the runner TU's own engine includes (`strategy_ic_runner.cpp:31-35`).
  3. It cannot detect a newly added `.cpp` that implements a listed header. If code moves out of a header, the hash trips, but nothing forces the new TU onto the list.
  4. atx-core headers in the closure (`types`, `error`, `macro`, `hash`, `container/hash_map`, `sha256`) and xsimd are not hashed.
  - Today's risk is low:
    - hash-consing compares structurally (`dag.hpp:83`), so a hash change cannot merge distinct nodes;
    - the kernels use only exact xsimd ops (`abs`, `select`, `isfinite`, batch arithmetic; `ts_ops.hpp:551-585`).
  - A cheap hardening covers items 1-3: assert that every `atx-engine/src/alpha/*.cpp` is either listed or on an explicit exclusion list, and include the runner TU's own `atx/engine` includes in the closure scan.
- **N3 (Minor). The M5 check is skipped in the validation-only run itself unless `--train-fields` is also passed.**
  - `same_field_definitions` returns Ok when either role has no pin (`:638`).
  - In validation-only mode `--train-fields` is optional: the frozen recipe's pin is carried over by `expect_frozen_fields`, `:270-279`.
  - Fix: root should always pass `--train-fields`/`--train-fields-sha256` in the v3 validation-only run. When passed, it is verified against the frozen recipe pin (`:276-277`), and the definition check then fires.
  - A code alternative: in validation-only mode, require `--train-fields` whenever the library declares extras.

Nothing else is new: no Critical or Important breakage.
- The default path is unchanged: with no extras, `planned` and `needs` are all-zero vectors of length n, `enter` returns immediately, and `panel_for` returns `&role.panel`. Test (d) asserts that no residency lines appear.
- `field_plan` bounds are enforced: n ≤ 256 (`:519-520`) and f ≤ 64 (`:456`), so O(n²f) is trivial and the shifts stay in range.
- No tool parses the removed `IC fields-loaded` line, and none reads `loaded_bytes`. The fitter reads only `research_fields.manifest_sha256` (`fit_composition_weights.py:303-305`).

### Out-of-Scope Observations

- ⚠️1. The v3 `required_bytes` figure is still arithmetic. Root should run native `--plan-only` for v3 on TRAIN role v2 with fields-v2 (or v3 after T6 fix round 3) at `--workers 4`, and confirm it is 1,344,767,322 B (`resident_capacity=2`) before scheduling the run.
- ⚠️2. T6 fix round 3 changes the `shares_out` rule and definition. Both the TRAIN and validation fields manifests must be regenerated with the same producer blob, otherwise the new M5 check (correctly) refuses the validation run.

### Verdict

**Fix round:** All Important findings are addressed (I1, I2, and the root `--workers 1` report), with no new Critical or Important breakage.
- Open non-blocking items:
  - M1, which needs a root ruling (recommended above: tie the weights' `provenance.orientations_sha256` to `--orientations-sha256`, and ledger the weights SHA before the single validation run);
  - M6, accepted;
  - M8's remaining T1 minors;
  - new Minors N1 (tripwire cadence), N2 (closure blind spots) and N3 (pass `--train-fields` in the validation-only run).
