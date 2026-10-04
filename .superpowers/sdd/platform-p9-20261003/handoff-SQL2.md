# Handoff: lane SQL2 (owner stop)

- Pool: `C:/atx-wt/pool-23` (lease: branch `feat/p9-sql2-20261003`, run id `p9-sql2-20261003`, heartbeat `p9-sql2-hb`)
- Branch: `feat/p9-sql2-20261003`
- Base: `1239a5ff` (lease) -> first commit `51ce19f2` = `git merge --no-ff 4d198fef` (SQL1; ruling SQL2-BASE). SQL1 fix round 1
  (`1a5b051d`) was merged at `afea225d`.
- Head: the commit that adds this file. The WIP code commit is `73fceb8d`.
- Commits: 51ce19f2 (merge SQL1), 1993f606 (descriptors + fixture), 1263b7c4 (library, CLI, classes.json, engine CMake
  block), 80e27e04 (fixture tree + legacy records), afea225d (merge SQL1 fix round 1), da25e5cc (StoreOpen API
  adaptation), 56469cec (gtests + tests CMake block), 73fceb8d (wip: Python checker + 3 pytests).

## Brief tasks (brief-SQL2.md)
1. Descriptors (`rows_records.hpp`, `ops_records.hpp`, `tables_records.hpp`, `records_ops.cpp`,
   `schema/catalog_records.json`): **done**.
2. Walker, seal guard, classifier (`catalog.cpp`, `seal_guard.*`, `classify.*`, `classes.json`): **done**.
3. Ingesters (`ingest_{run,stage,cycle,wave,spec,fields,build,ledger}.cpp`, `pins.*`, `json_text.cpp`): **done**.
4. CLI `atx-research-store` (`store_cli.*`, `query.*`, `cache_index.cpp`, `store_main.cpp`): **done**. It follows SQL1
   fix round 1: `init`/`catalog` use CreateIfMissing; readers use Existing; `cache init` calls `create_cache`.
5. Render-identity checker `atx-engine/tools/research_store_identity.py`: **done**. It has the CLI
   `--catalog DB --root R [--classes ..] [--registry F]` and `check_one(db, path, cls, *, root, registry)` for SQL3.
   - Class registry guard (in `test_research_store_classes.py`): **written, not run**.
6. Tests: **gtests done**; **pytests partial**; **report not started**.
   - gtests: 21/21 pass.
   - `test_research_store_identity.py`: 16 passed, 6 subtests, 4.6 s. This includes `FixtureChain`: C++ catalog of the
     fixture tree, then the checker, giving 12 checked and 0 mismatches.
   - `test_research_store_classes.py` and `test_research_store_blind.py`: written, **never run**.
   - SQL1's fixture pytests (`test_research_store_fixtures.py`, `test_research_store.py`) have not been re-run with the
     third fixture present.

### Where I stopped and the next steps
1. Run with explicit paths, under `PYTHONHASHSEED=0` and again under `=1`:
   ```
   "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider \
     atx-engine/tools/test_research_store_classes.py \
     atx-engine/tools/test_research_store_blind.py \
     atx-engine/tools/test_research_store_identity.py \
     atx-engine/tools/test_research_store_fixtures.py \
     atx-engine/tools/test_research_store.py
   ```
   Then fix anything red.
   - Classes guard: a scratch scan run before writing the test found 119 literals and 0 unregistered. The test should
     pass. `test_registry_shape` assumes every class has keys id, label, globs, schemas, format, ingest, render,
     writer, pinned_by, stage.
   - Blind test: watch for false positives of the token list on view SQL identifiers. Short tokens such as
     `se`, `ci`, `ir`, `var` and `score` could match an SQL word. If one does, narrow the list. Never rename a column
     to dodge it.
2. Update the `description` string in `classes.json` to mention the per-class `registers` key: literals a class
   registers that are not document schemas (wave-reader). This changes no behaviour.
3. Write `.superpowers/sdd/platform-p9-20261003/task-SQL2-report.md` (`git add -f`) per the dispatch contract. Its content
   is listed below.
4. Hand back to the PM.

## Content for the report
- **Files.**
  - Owned: everything under `atx-engine/{include/atx/engine,src}/research/store/catalog/`,
    `atx-engine/schemas/research_store/classes.json`, `atx-engine/tests/research/research_catalog_*`,
    `atx-engine/tests/fixtures/research_store/{schema/catalog_records.json,make_store_tree.py,tree/**,legacy_records/**}`,
    `atx-engine/tools/research_store_identity.py` and `atx-engine/tools/test_research_store_{identity,classes,blind}.py`.
  - Cross-lane: one block appended at the tail of `atx-engine/CMakeLists.txt` and one at the tail of
    `atx-engine/tests/CMakeLists.txt`. The tail of the tests file had a merge conflict with SQL1's block; I kept both
    blocks.
  - `git diff --stat <base>..HEAD -- atx-impl scripts` should be empty (flag-absent identity); confirm it.
- **Deviations / decisions.**
  - Private helpers: `catalog_detail.hpp`, `json_text.cpp`, `cache_index.cpp` (not in the brief's file list).
  - `cycle_binding.spec_sha256` is nullable: `cycle_resume` writes null for ref steps.
  - `classes.json` adds `seed_globs`, `registers` and `domains`.
  - Rows are not pruned without `--rebuild`.
  - `spec_doc.git_tracked` is set by a heuristic.
  - Build receipts are seeded through `seed_globs`.
  - In the `pin_status` view, a field-source pin with no artifact is `unresolved`.
  - The identity checker compares the render with `artifact.sha256` and, given `--root`, with the bytes on disk; the
    mismatch text shows the first differing byte offset. Reasons carry digests, offsets and key names only, never
    values (blind rule).
- **B2 seam (root must check when B2 and SQL2 are both merged).**
  - `LedgerHeadFn` and `default_ledger_head()` (`ingest.hpp`, body in `ingest_ledger.cpp`) return an empty function, so
    `ledger_state.head_sha256` and `head_rule` stay NULL.
  - Root binds B2's chain-head function inside `default_ledger_head()` only, and checks that:
    - its signature fits `LedgerHeadFn`;
    - it takes the lines without line ends, in file order;
    - SQL2 never re-implements the chain rule (G-P5).
  - `ResearchCatalog.LedgerHeadSeamReceivesLinesInFileOrder` covers the seam contract.
- **Root verifies.**
  - Build targets: `atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store` plus SQL1's three.
  - gtest: `atx-engine-research-catalog-tests --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*`.
  - pytest: `atx-engine/tools` under two seeds.
  - Fixture chain: `atx-research-store init` + `catalog --root atx-engine/tests/fixtures/research_store/tree --classes
    atx-engine/schemas/research_store/classes.json`, then `research_store_identity.py --catalog DB --root <tree>`.
    Result: 12 checked, 0 mismatches.
  - **Fixture catalog digest** (committed tree, no payloads; reproduced with relative and absolute `--root`):
    `d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69` (29 seen, 25 verified, 0 declared, 4 skipped).
    The gtests create the two 16 MiB payloads, so their counts differ: 27 artifacts (25 verified, 2 declared).
- **Concerns.**
  1. Volume of the build-equity name-only listing on the real tree (`skipped_path` rows).
  2. The CMake build tree under `build-equity/` is listed as outside-roots.
  3. **Merge trap:** other wave-2 lanes that add new `atx.<name>/v<n>` literals under `scripts/` or
     `atx-engine/tools/` will turn `test_every_schema_literal_is_registered` red after the merge. Root adds the schema
     to its class in `classes.json` (or a dated `legacy_allow` row). That is the guard working, not a regression.
  4. SQL1-MON: the CLI help, and stderr on `cache init`, warn that `book_monitor.py --fit-work` does not read
     DB-only records.

## Builds
- All builds went through `scripts/research-build.ps1`, preset equity-dev, one at a time, after the memory gate.
- `p9-sql2-a` (library): exit 0, no warnings.
- `p9-sql2-b`: failed in a test TU ("'Run' is a private member of 'testing::Test'"). Fixed by renaming the helper to
  `CliRun`.
- `p9-sql2-c` (library, tests, exe): exit 0, 18.2 s.
- The executable is at `C:/atx-wt/pool-23/build-equity/bin/atx-research-store.exe`.
- No real data was opened.

## Traps for the next agent
- Write files with the Write/Edit tools only: the shell hook mangles backslashes in heredocs.
- Run pytest only with explicit file paths and `-p no:cacheprovider`.
- SQL1's files are forbidden, as are `scripts/`, `atx-impl/`, `vcpkg.json` and `CMakePresets.json`.
- `test_research_store_identity.py` imports writers from `scripts/` and `atx-impl/tools` through `sys.path.append`. A
  scan found no module-name collisions between the three tool dirs.
- `FixtureChain` skips when the executable is absent. Override its location with `ATX_RESEARCH_STORE_EXE`.
- `classes.json` order matters: the first match wins and `other` must stay last. A json class that lists schemas also
  needs a schema match.

## Open questions for the PM
- None blocking.
- Should root fold the wave-2 lanes' new schema literals into `classes.json` at merge time (my assumption), or should
  each lane own its own rows?
