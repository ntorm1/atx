# Lane A2 report

## Outcome

DONE_WITH_CONCERNS. The deliverables are done:
- the C++ builder kinds (K-P9-2, kind half) and the reader for the K-P9-1 field registry;
- producer identity (K-P9-3), and `atx-research-fields build --registry R --spec S --receipt OUT`, which writes a
  manifest last and decides reuse;
- a seal refusal on prior manifests;
- the engine shim, now a caller of the registry's `kind: engine` rows;
- the gtests and pytests the brief names.

The concerns:
1. The C++ was written but never compiled or run, by rule.
2. Two existing gtests only go green after A1's regenerated `expected/` merges first (P5 merge order).
3. The flip of the three registry rows is a listed edit for root to apply at merge, because
   `field_registry.json` does not exist at my base.

## Branch / SHA

`feat/p9-a2-20261003`. All work is committed and the leased tree is left clean.

| commit | content |
|---|---|
| `8ab9ec60` | C++ (library, executable, CMake, gtests) |
| `106db131` | engine shim and its pytest |
| (this report's commit) | report only |

## Frozen base / lease

- base_sha=d7c1c520c3caa162ef453349b256669ce8de3c80; worktree=C:\atx-wt\pool-13; lease_name=pool-13
- lease_run_id=p9-a2-20261003; heartbeat_id=p9-a2-hb; keeper_pid=19424
- keeper_process_started_utc=2026-10-03T10:08:03.7431522Z

## Acquisition receipt

Root leased the pool; the lane did not lease or release it. The `.atx-lease` record is version 3:
- run_id=p9-a2-20261003, agent=p9-a2, branch=feat/p9-a2-20261003
- base_sha=d7c1c520c3caa162ef453349b256669ce8de3c80
- acquired_utc=2026-10-03T10:08:04.7750132Z
- owner_kind=heartbeat, heartbeat_id=p9-a2-hb, keeper_pid=19424, keeper_ready_utc=2026-10-03T10:08:04.4725206Z

## Files changed

New files (all `atx-engine/`):
- `include/atx/engine/research/fields/build_spec.hpp`: `BuildSpec` and `ReuseRequest`, moved out of the CLI header
  so that the kinds can read them.
- `include/.../fields/field_registry.hpp`, `src/research/fields/field_registry.cpp`: the K-P9-1 reader.
- `include/.../fields/registry.hpp`, `src/.../registry.cpp`: K-P9-2 `BuilderKind {id, parse, build}`, `FieldPlan`,
  `BuildContext` (with a `SharedSources` slot for A3's vendor panel), `KindOutput`, `builder_kinds()`,
  `find_builder_kind()` and `plan_fields()`.
- `include/.../fields/producer.hpp`, `src/.../producer.cpp`, `src/.../build_identity.cpp.in` (generated): K-P9-3.
- `include/.../fields/reuse.hpp`, `src/.../reuse.cpp`: the reuse decision and the seal refusal.
- `include/.../fields/manifest.hpp`, `src/.../manifest.cpp`: the manifest that is written last.
- `tests/research/research_fields_registry_test.cpp` (`ResearchFieldsRegistry.*`, 7 tests).
- `tests/research/research_fields_manifest_test.cpp` (`ResearchFieldsManifest.*`, 8 tests).
- `tests/research/research_fields_registry_support.hpp`.

Modified files (all `atx-engine/`):
- `src/.../research_fields_cli.cpp`, `include/.../research_fields_cli.hpp`:
  - the name dispatch (old :28 and :124-133) is replaced by kind lookup;
  - the `--registry` verb and the v2 spec;
  - the receipt carries the engine identity;
  - the sealed-rows key is renamed.
- `src/.../file_io.cpp`, `include/.../file_io.hpp`: `digest_file`, `copy_exclusive` and `publish_exclusive`
  (pending file, fsync, hard link, the same steps as the Python `publish()`).
- `include/.../finra_asof_field.hpp`: comment on the renamed key.
- `CMakeLists.txt`, fields block:
  - the 5 new TUs;
  - nlohmann_json moves to PUBLIC, because the headers speak JSON;
  - the configure-time git SHA and build type go into the generated source, following atx-impl's
    `engine_git_sha` rule.
- `tests/research/research_fields_cli_test.cpp`: the new `build_fields(spec, sha, producer)` signature, plus
  engine-identity asserts.
- `tests/research/research_fields_fixture_test.cpp`: the key literal becomes `rows_sealed_dropped` (2 places; P5).
- `tools/prepare_research_fields_engine.py` (P5: A2 owns it): it now calls the registry's kind-engine rows and
  stamps the K-P9-3 producer. The dead test reference at :13 now names the real test.
- `tests/fixtures/research_fields/test_research_fields_engine_path.py` (P5 test path).

## Interfaces as coded

**K-P9-2 kinds.**
- `struct BuilderKind {std::string_view id; ParseFn parse; BuildFn build;}`
- `ParseFn = Result<FieldPlan>(*)(string_view field, const json &options, const BuildSpec &)`
- `BuildFn = Result<KindOutput>(*)(const FieldPlan &, BuildContext &)`
- Table order: `si_shares`, `si_dtc`, `vol_126`. `engine_field_names()` returns these ids, so its order is
  unchanged.
- Each kind builds only its own field. The only option it accepts is `{"group": <spec group>}` (A1's rows carry
  `{"group": "finra"}` and `{"group": "price_volume"}`). `FieldPlan.inputs` lists the files the build reads, in
  the order the entry records its sources.

**K-P9-1 reader.**
- The top level is `{"schema": "atx.field-registry/v1", "fields": [...]}`. Other top-level keys are ignored.
- Every row needs all 12 keys. Unknown row keys are ignored. Names must be distinct.
- `formula_sha256` is null or 64 lower-case hex digits. `first_session` is null or a strict date.
- `field_registry_document()` round-trips.

**Registry planning.**
- Each field must be an engine row whose `builder` is a known kind id, with dtype f64.
- `formula_sha256`, when declared, must equal the kind's fingerprint.
- Plans come back in registration order.

**CLI.** `atx-research-fields build --spec S --receipt R [--registry REG]`.
- Without `--registry` the spec is v1, with today's keys and today's checks. The receipt changes only in two ways:
  the `engine` block gains `exe_sha256`, `git_sha` and `build_type`, and the sealed-rows key is
  `rows_sealed_dropped`.
- With `--registry` the spec is `atx.research-fields-spec/v2`: the v1 keys plus an optional
  `"reuse": {"dir", "manifest_sha256"?}`.
- A registry build runs in this order:
  1. refuse an existing `output_dir/manifest.json`;
  2. reuse;
  3. build the rest;
  4. write the receipt exclusively, now carrying `registry {path, sha256}` and `reused [names]`;
  5. publish `output_dir/manifest.json` last.
- Exit codes: 0 done, 2 for usage, registry or spec errors, 1 for build errors.

**K-P9-3 producer.** Every entry the run built carries
`producer: {kind: "engine", exe_sha256, git_sha, build_type, receipt_sha256}`. A producer without `kind` is the
legacy Python shape (P6) and is never treated as an engine producer.

**Manifest.**
- It is `atx.research-role-fields/v1` in the consumer layout: role binding (manifest, sessions, ids and member
  SHAs), seal, entries, files pins and source_checks.
- It also carries `engine`, `receipt_sha256`, `spec_sha256`, `registry`, `research_window`, and `reuse` when reuse
  was asked for.
- Encoding: sorted keys, indent 2, ASCII, final newline. Nothing claims it equals a Python manifest. What it claims
  is the payload and its coverage (FD-1).

**Reuse rules.** The prior manifest itself is refused when any of these hold:
- it is not a complete v1 manifest;
- the pin (when given) does not match;
- it is bound to another role;
- its `seal.exclusive_end` is present and not 2024-01-01. An absent seal is recorded as `"absent"` and does not
  refuse (P13).

Each field is then decided by the first rule it fails, and the reason is recorded:
1. it has an entry and a pin;
2. the layout, shape and pin match;
3. the producer is an engine producer whose {exe_sha256, git_sha, build_type} equals this executable's, and this
   executable's identity is known (not "unknown");
4. the formula fingerprint matches;
5. the sources match re-hashed inputs, by (bytes, sha256) in order.

A reused payload is copied and verified against its pin; a mismatch refuses the build as a corrupt prior directory.
A reused entry is the prior entry unchanged (its producer stays the original producer) plus
`reused_from {dir, manifest_sha256, payload_sha256, mode: "copy"}`.

**Engine shim.**
- `--engine-fields` accepts only `kind: "engine"` rows of `field_registry.json` (module global `REGISTRY`) that the
  shim can route (`ROUTES`: si_shares, si_dtc, vol_126).
- `engine_path(exe, names)` keeps its signature, because A1's entry calls it. It refuses names it cannot route.
- The receipt's `engine.exe_sha256` must equal the SHA-256 of the executable that was run.
- The producer block reaches each entry through the builder's own extras. The FINRA extras are spread by `run()`.
  For vol_126, the price module spreads `**extra` after its own producer, so the engine producer wins.

## Evidence

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py
exit_code=0
.........ss                                                              [100%]
9 passed, 2 skipped in 4.89s
(-rs: SKIPPED test_research_fields_engine_path.py:260 field_registry.json (K-P9-1) lands with lane A1; lane A2's
listed edit flips its three rows | SKIPPED :269 set ATX_RESEARCH_FIELDS_EXE (root))

"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py
exit_code=0
..                                                                       [100%]
2 passed in 0.63s
```

I wrote the C++ tests but did not run them. I checked every changed or new C++ file for the 100-column limit:
`awk 'length > 100'` over `atx-engine/{include/.../fields,src/research/fields,tests/research}` prints nothing.

## How root verifies

**Build** (equity-dev, target-scoped):

```
powershell -File scripts\research-build.ps1 -Tag p9-1a2 -Targets "atx-engine-research-fields,atx-engine-research-fields-tests,atx-research-fields"
```

The `generated/research_fields_build_identity.cpp` file is created at configure time. The CMakeLists edit forces a
reconfigure on the first build.

**Gtests** (A1 merged first, so that `expected/manifest.normalized.json` carries `rows_sealed_dropped`):

```
build-equity\bin\atx-engine-research-fields-tests.exe --gtest_filter=ResearchFields*
```

Anchored subsets:
- `--gtest_filter=ResearchFieldsRegistry.*`
- `--gtest_filter=ResearchFieldsManifest.*`
- `--gtest_filter=ResearchFieldsFixture.*:ResearchFieldsCli.*`

Expect: 7 Registry tests, 8 Manifest tests, the 3 Fixture tests and the 3 Cli tests green.

Two tests are expected to fail on A2 alone, before A1 merges, because of the key in `expected/`:
- `ResearchFieldsFixture.FinraFieldsAreByteIdentical`
- `ResearchFieldsCli.ReceiptEqualsThePythonManifest`

**Real-exe pytest:**

```
set ATX_RESEARCH_FIELDS_EXE=<root>\build-equity\bin\atx-research-fields.exe
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py
```

`test_real_executable_identity` must pass: payloads and coverage are equal, and each engine entry's
`producer.exe_sha256` equals the executable's SHA-256 and the research-build receipt's `Executables` entry.

**Flag-absent identity.** The procedure:
- (a) No accepted output was produced through `atx-research-fields`. The target is `EXCLUDE_FROM_ALL`, and
  `research_cycle.py` passes no engine flag. No receipt or engine-path manifest is pinned anywhere. Without
  `--engine-fields`, the engine shim calls `builder.main(rest)` and reads neither the registry nor an executable.
  `test_flag_absent_is_the_plain_builder` pins this: it points `REGISTRY` at a missing file and checks that the
  bytes equal the plain builder's.
- (b) The argv is the v15 fields builder argv, unchanged. Byte-identical files: every file of a Python-builder fields
  directory. Lane A2 did not touch `prepare_research_fields.py`; A1 owns it and its re-pin.
- (c) The C++ fixture identity on 8 x 300 is unchanged: the payload, coverage and formula assertions are untouched.
  Only the source_checks key literal changes.

**Substitution list.** These bytes change only with a flag present:
- The `atx-research-fields` receipt:
  - `engine` gains `exe_sha256`, `git_sha` and `build_type`;
  - `source_checks.rows_available_on_or_after_2025_dropped` becomes `rows_sealed_dropped`;
  - registry builds also get `registry` and `reused`.
- Engine-path manifests (`prepare_research_fields_engine.py --engine-fields ...`): each engine entry's `producer`
  becomes the K-P9-3 block. FD-1 intends this change.
- The new registry-build `manifest.json` is new output, with no prior bytes.

**TRAIN identity of the three engine fields.** Root does this after the merge and after the flip below. Write a v2
spec:

```json
{"schema": "atx.research-fields-spec/v2",
 "role": {"dir": "<v15 TRAIN role dir>", "manifest_sha256": "<its pin>"},
 "output_dir": "<new empty dir>",
 "fields": ["si_shares", "si_dtc", "vol_126"],
 "finra": "<v15 finra dir>"}
```

Then run it through `run_bounded_research.py`:

```
build-equity\bin\atx-research-fields.exe build --registry atx-engine\tools\field_registry.json --spec <spec.json> --receipt <dir>\a2-train.receipt.json
```

Expect: each `<new dir>/manifest.json` `fields[i].sha256` (and `files[<name>.f64].sha256`) equals v15's manifest
entry `sha256` for that field.

The alternative is the engine path inside the Python entry: the v15 argv plus
`--engine-fields si_shares,si_dtc,vol_126 --engine-exe <exe>`. Its 84 payload SHAs must equal v15's, and the
manifest may differ only in the producer blocks of those three entries.

## Cross-lane edits

1. `atx-engine/tests/CMakeLists.txt`: one block appended at the end (P2), a `target_sources` of the two new TUs
   into `atx-engine-research-fields-tests`. It touches no CTest lines.
2. Listed edit, for root to apply at merge (P5): in `atx-engine/tools/field_registry.json` (A1's file), change only
   the three rows `si_shares`, `si_dtc` and `vol_126`:
   - `"kind": "python"` becomes `"kind": "engine"`;
   - `"builder": "<module>"` becomes `"builder": "<row name>"`;
   - options, owner, spec_text and formula_sha256 stay as they are.

   The edit cannot be committed here because the file does not exist at d7c1c520.
   `test_repository_registry_routes_the_ported_rows_to_the_engine` skips while the file is absent and enforces the
   flip once it is present. A1 confirmed that its entry never imports an engine row's `builder`: it resolves the
   module from `owner`, or the builder binds it itself. A1 also confirmed that with no executable the build falls
   back to Python, byte-identical (`test_field_registry.py Entry.test_engine_rows_need_the_executable`). So the
   default path is unchanged by the flip.

## Deviations from brief

- The brief's test path `atx-engine/tests/research_fields/**` does not exist. The tests are under
  `tests/research/research_fields_*` (P5, preflight-scan).
- The brief cites the CMake block as :266-282. The real fields block is 248-282, sources 252-265.
- "Existing fixture identity unchanged": payload, coverage and formula identity are unchanged. The source_checks key
  literal is renamed as P5 rules, which makes this lane depend on A1's `expected/` regeneration.
- The flip of the three registry rows is delivered as a listed edit, as above, not as a commit.
- The v1 receipt gains identity keys even without `--registry`. This is needed so that the engine path can stamp
  K-P9-3 (git_sha and build_type exist only inside the executable). No receipt is pinned.
- nlohmann_json is now PUBLIC on `atx-engine-research-fields`, because the registry, kind, manifest and reuse
  headers speak JSON. The library is not exported or installed anywhere.

## Open risks

- The C++ was not compiled. Watch `/W4 /WX` first: the constexpr kind table of function pointers, the
  `<Windows.h>` include in `producer.cpp` (NOMINMAX and WIN32_LEAN_AND_MEAN are set), and gtest printing of
  `std::optional`.
- `git_sha` is the HEAD at configure time (the atx-impl rule) and goes stale until the next reconfigure. Reuse is
  keyed on `exe_sha256`, which embeds it. Any rebuild of the executable, even a non-deterministic one, makes reuse
  miss and recompute. That is the safe direction.
- Reuse re-hashes every input file of a candidate field, including the role's `volume.f64`, which is tens of MB on
  TRAIN. This costs seconds.
- The seal rule (refuse a present-and-different seal) exists twice in this wave: in A1's Python `load_prior` and in
  C++ `reuse.cpp`. That needs a G-P5 allowlist row, retired when the Python field builders retire (A3/A4 slices).
- The role builder (`prepare_recent_research.py:863`) still uses the old key name. That is out of scope (preflight
  table 5 #12).

## Ledger candidates

- The K-P9-3 producer block reaches Python-built manifests through the builder's extras. The FINRA extras are spread
  by `run()`. The price module spreads `**extra` after its own producer, so the engine producer wins for vol_126.
  The plain builder needs no edit.
- A `.cmd` stand-in executable's SHA-256 is what the engine shim checks against the receipt's `exe_sha256`. A
  stand-in therefore has to hash its own `.cmd` path, which `STANDIN.format(exe=...)` passes in.
