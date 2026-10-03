# Lane A2 review

## Verdict
APPROVE

0 blocker, 0 major, 8 minor. The C++ reads clean under clang-cl 18 `/W4 /permissive- /WX`, except one line over the
100-column house limit, which `/WX` does not catch. All five deliverables meet the brief and K-P9-1/2/3 as ruled by
P5/P6/P13:
- the kinds;
- the registry reader;
- the producer identity;
- the publish-last manifest;
- reuse with the seal refusal.

Flag-absent identity holds for the Python shim and is pinned by a test. The lane's merge-dependency claim is accurate
but incomplete: see m3 and m4.

## Reviewed SHA
`2b6f8e6f5ca86c75592bb7b4e2c137a00765d90c` (base `d7c1c520`). Pool `C:/atx-wt/pool-13`, branch
`feat/p9-a2-20261003`. The tree was clean at HEAD before and after the review.

## Evidence
Both commands were run in pool-13 with `PYTHONDONTWRITEBYTECODE=1` and `-p no:cacheprovider`. `git status --porcelain`
was empty afterwards.

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rs atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py
exit_code=0
.........ss                                                              [100%]
SKIPPED [1] ...test_research_fields_engine_path.py:261: field_registry.json (K-P9-1) lands with lane A1; lane A2's listed edit flips its three rows
SKIPPED [1] ...test_research_fields_engine_path.py:270: set ATX_RESEARCH_FIELDS_EXE (root)
9 passed, 2 skipped in 4.52s

"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py
exit_code=0
..                                                                       [100%]
2 passed in 0.66s
```

Spot checks, all read-only; the scratch scripts live outside every tree:
- **A1's registry draft against A2's reader.** A1 has not committed yet. Its draft is the untracked
  `C:/atx-wt/pool-12/atx-engine/tools/field_registry.json`. Validated against A2's C++ reader rules
  (`field_registry.cpp`: 12 keys, enums, hex64 or null, strict date or null, string arrays, distinct names), it gives
  92 rows and 0 violations. The top-level `dtype_rule` key is ignored by design.
- **The three ported rows in that draft:**
  - `si_shares` and `si_dtc` carry `{"group": "finra"}`; `vol_126` carries `{"group": "price_volume"}`. These equal the
    C++ `FieldSpec.group` (`finra_asof_field.cpp:28`, `volume_mean_field.cpp:16`).
  - Their `formula_sha256` values are `0d8deda...`, `051996b...` and `cb44b19...`. Each equals Python
    `formula_id(name, spec_definition(name, lag))` at lag 1 and at `LAG_SESSIONS`.
  - So after root's flip, `plan_registered` accepts all three rows, and nothing in the rows causes a "spec text
    drift" refusal.
- **A1's `expected/manifest.normalized.json` draft.** The diff is exactly the key rename
  (`rows_available_on_or_after_2025_dropped` becomes `rows_sealed_dropped`, values 0 and 1); the payloads are untouched.
  So `ResearchFieldsFixture.FinraFieldsAreByteIdentical` and `ResearchFieldsCli.ReceiptEqualsThePythonManifest` go green
  after A1 merges, as claimed.
- **A1's draft has the tests the report cites:**
  - `CommittedRegistry.test_lane_a2_flip_keeps_the_registry_tests_green` (the three-row flip);
  - `Entry.test_engine_rows_need_the_executable` (falls back to Python with a log when no exe is given; the exe path
    is used when one is);
  - an `engine_produced(e)` reuse branch (P6).
- **Python `run()` applies extras last** (`prepare_research_fields.py:2578`). The price module spreads `**extra` after
  its own producer (`research_fields_price.py:778-780`). So the K-P9-3 block does reach the FINRA and vol_126 entries.
- **Compile-hazard precedents:** the same-target gtests already `EXPECT_EQ` on `std::optional`
  (`research_fields_finra_asof_test.cpp:223`); `<Windows.h>` is included in other engine TUs
  (`eval/lockbox_audit.cpp` and others); and `filesystem::remove(p, ec)` with a discarded result exists in
  `panel_store.cpp:295`.
- **The C++ `formula_sha256` equals the Python value:**
  - `vol_126` is asserted directly (`research_fields_cli_test.cpp:147`);
  - `si_shares` and `si_dtc` are asserted by the shim's formula guard in the real-exe pytest.

## Findings
path:line | severity | problem | required fix

- m1. `atx-engine/include/atx/engine/research/fields/research_fields_cli.hpp:20` | minor | The line is 115 columns,
  over the house 100-column limit. The report (`task-A2-report.md:175`) says the `awk 'length > 100'` check "prints
  nothing", so the evidence contradicts the claim. | Re-wrap the comment line and re-run the awk check over every
  changed `*.hpp`/`*.cpp`.
- m2. `atx-engine/src/research/fields/producer.cpp:30-38` | minor | `GetModuleFileNameW(nullptr, ...)` hashes the
  `.exe`.
  - In an `ATX_SHARED_LIBS=ON` build (`dev-shared`; `ATX_LIB_TYPE` SHARED at root `CMakeLists.txt:171`), the builder
    code and the baked `build_identity.cpp` live in `atx-engine-research-fields.dll`.
  - A DLL-only rebuild after an arithmetic fix that leaves the spec text alone keeps `exe_sha256` (and possibly
    `git_sha`) unchanged. The reuse of K-P9-3 would then hit and carry a stale payload under a manifest that names
    this executable.
  - The hazard is latent: root's presets `equity-dev` and `equity-rel` inherit `ATX_SHARED_LIBS OFF` from `_base`.
    But the soundness of the reuse key silently depends on static linking. | Hash the module that contains the
  code, through `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | ..._UNCHANGED_REFCOUNT,
  &current_producer)` and then `GetModuleFileNameW(hmod)`. Or define a compile flag when the library is SHARED and
  return the `unknown` identity, which never reuses. State the rule in `producer.hpp`.
- m3. `.superpowers/sdd/platform-p9-20261003/task-A2-report.md:201` | minor | The merge note "Two tests are expected
  to fail on A2 alone" is incomplete. A third check fails on A2 alone:
  `test_research_fields_engine_path.py::test_real_executable_identity`, root's real-exe pytest from "How root
  verifies".
  - The real exe writes `rows_sealed_dropped`.
  - The base Python builder still writes the old key, so `without_producers(mg) == without_producers(mw)` fails.
  - It passes only after A1's Python rename merges.
  - Merge order A1 then A2 (P5) makes this moot at integration, but the list root reads is short by one. | Add the
  real-exe pytest to the A1-first dependency list.
- m4. `.superpowers/sdd/platform-p9-20261003/task-A2-report.md:263` | minor | The cross-lane flip is complete in
  content: `kind` and `builder` on three rows, verified against A1's draft above. The procedure, however, omits a
  constraint. A1's `CommittedRegistry.test_valid_and_in_v15_order` pins the committed file to
  `field_registry.dump()`'s byte form (`fr.dump(doc) == DEFAULT_PATH.read_bytes()`). A hand edit that reformats the
  JSON turns A1's suite red. | State that the flip is in-place value edits only (`"python"` becomes `"engine"`, the
  module name becomes the row name), or a load, edit and `field_registry.dump` round trip. Root runs A1's
  `test_field_registry.py` plus the un-skipped
  `test_repository_registry_routes_the_ported_rows_to_the_engine` in the merge commit.
- m5. `.superpowers/sdd/platform-p9-20261003/task-A2-report.md:257` | minor | The alternative TRAIN check says that
  against v15 "the manifest may differ only in the producer blocks of those three entries". In fact the merged tree
  also differs from v15 in the renamed `source_checks` key and in A1's documented version and reuse keys. Root would
  see diffs that this text does not predict. | Restate the check: 84 payload SHA-256 equal to v15; the manifest
  differs only in the producer blocks plus A1's documented key list (one re-pin ruling).
- m6. `atx-engine/tests/research/research_fields_manifest_test.cpp:96` | minor | The test asserts that
  `.manifest.json.pending` remains after a refused publish. That pins a failure side effect (the same leak that
  Python `publish()` has) instead of the contract, which is that the published bytes stay untouched and are never
  replaced. | Drop the line, or replace it with a contract assertion.
- m7. `atx-engine/tests/research/research_fields_manifest_test.cpp:140-225` | minor | "Receipt first, manifest last"
  is not exercised. No case makes the receipt write fail, for example with a pre-existing receipt path, and then
  asserts that `output_dir/manifest.json` is absent. The test name claims the ordering, but only the final state is
  checked. | Add the case: pre-create the receipt file, expect exit 1 and no `manifest.json`.
- m8. `atx-engine/include/atx/engine/research/fields/manifest.hpp:3-6` | minor | The header says the manifest uses
  "the layout ... the fields consumers read".
  - The IC runner, signal cache and NAV readers need only schema, status, role, fields and files, and those hold.
  - But `atx-impl/src/strategy_live.cpp:424-425` refuses a fields manifest without a top-level `code_sha256`, which a
    registry manifest by design does not carry (FD-1).
  - Nothing consumes registry manifests in wave 1, so this is a forward-contract note for E2 and the live path.
  | Name the exception in the header, and add a ledger or K-P9-3 reader note so that live is not pointed at a
  registry manifest unchanged.

Scope note, not counted: `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` lies outside
the literal files in scope (P5 corrects the scope only to `tests/research/research_fields_*`).
- It is the natural test of `engine.py`, which P5 gives to A2.
- No lane owns the file, and A1's draft does not touch it.
- The report lists it as "P5 test path" (`:79`). It belongs under "Deviations from brief" (`:279`).

## Contract and identity checks (what holds)
- **K-P9-2.** `struct BuilderKind {std::string_view id; ParseFn parse; BuildFn build;}` matches exactly. The table is a
  constexpr `std::array` of function pointers, valid C++20, with ids derived in a constexpr lambda. `BuildContext`
  carries a `SharedSources` slot, so the FINRA schedule is read once per run. The name dispatch
  (`research_fields_cli.cpp:28`, `:124-133`) is gone.
- **K-P9-1 reader.** The reader follows the K-P9-1 rules:
  - it requires all 12 keys;
  - it ignores row keys outside the 12 and top-level keys other than `schema` and `fields`;
  - it refuses repeated names;
  - `plan_registered` refuses: a python row, an unknown kind, a non-f64 dtype, a foreign or mismatched option, and
    a `formula_sha256` that drifts from the kind;
  - registration order becomes manifest order through `stable_sort`.
- **K-P9-3 / P6.**
  - Built entries carry `{kind: "engine", exe_sha256, git_sha, build_type, receipt_sha256}`.
  - A producer without `kind` (or no producer, as on Python FINRA entries) is never an engine producer, so reuse
    misses.
  - Reuse hits only on an equal, known identity.
  - A reused entry keeps the original producer and gains `reused_from`.
- **Seal (P13).** A present-and-different `seal.exclusive_end` refuses the prior manifest. An absent seal is recorded
  as `"absent"` and does not refuse.
- **Publish-last.**
  - The manifest's presence is pre-checked.
  - The receipt is written with `write_exclusive` before the manifest.
  - The manifest goes pending, fsynced, then hard-linked, so an existing manifest is never replaced (no TOCTOU gap).
  - A failure at any step leaves no manifest. A corrupt prior payload stops the build with no manifest, and this is
    tested.
- **Flag-absent identity.**
  - Shim: `main()` without `--engine-fields` calls `builder.main(rest)` unchanged. No registry or exe is read at
    import or on that path. `test_flag_absent_is_the_plain_builder` pins this and passes.
  - Exe v1 path: only the documented substitutions change it (the 3 engine identity keys and the mandated key rename).
    Build order, entry blocks (`merge_blocks` is equivalent to the old direct sets), messages and exit codes are
    unchanged.
  - No pinned receipt exists, and nothing outside the shim and its test calls the engine path.
- **Determinism.** Every object in the manifest and receipt is a sorted-key JSON object; the plan, reuse and entry
  orders are fixed; nothing iterates an unordered container and nothing depends on time.
- **Blindness.** Every test date is in 2020. The "2025-01-01" string is a synthetic seal value used in the refusal
  test. The lane opened no real data.

## Checked
- [x] `.agents/cpp/agent.md` §10 checklist applied to the diff. Results:
  - no UB found (`role.days().front()` is guarded by `RoleAxes::load`, which requires positive dates; every plan's
    `kind` and `spec` are set before use);
  - no narrowing;
  - error paths handled;
  - string_view and span lifetimes bounded by their owners;
  - switches exhaustive and loops bounded;
  - one 100-column miss (m1).
- [x] Diff stays inside the brief's files in scope: `atx-engine/tests/CMakeLists.txt` is a P2 append that touches no
  CTest line; see the scope note on the fixture pytest.
- [x] Evidence in the report matches its claims: both pytests were re-run with exit 0. Two report claims are not
  borne out: the 100-column claim (m1), and the merge list, which is short by one (m3).
