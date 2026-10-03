# Lane SQL2 report (P9, wave 2): artifact catalog, `atx-research-store`, render-identity checker

## Outcome
DONE_WITH_CONCERNS. All six brief tasks are implemented and tested: 21/21 gtests, 42 lane + SQL1 pytests under two
seeds, and the full `atx-engine/tools` suite at 450 passed. The fixture chain gives 12 checked, 0 mismatches. The
concerns are merge-time items for root (see "Open risks"). None of them is a defect in the lane.

## Branch / SHA
`feat/p9-sql2-20261003` in `C:/atx-wt/pool-23`. Head = the commit that adds this report. The last code/test commit is
`6954b605`. All work is committed and the tree is left clean.

## Base / lease
- Lease base is `1239a5ff` (W2-EARLY). The first lane commit `51ce19f2` is `git merge --no-ff 4d198fef` (SQL1 head,
  ruling SQL2-BASE). SQL1 fix round 1 (`1a5b051d`) is merged at `afea225d`.
- Lease: run id `p9-sql2-20261003`, heartbeat `p9-sql2-hb`, pool-23. The brief named pool-24; the lane ran in the
  pool its lease gave it.
- This report covers a resume (ruling RESUME). The handoff file is `handoff-SQL2.md` in this directory.

## Commits (lane-own, in order)
| SHA | content |
|---|---|
| `1993f606` | task 1: `catalog_records` descriptors, `pin_status` view, group schema fixture |
| `1263b7c4` | tasks 2-4: walker, seal guard, classifier, ingesters, queries, the `atx-research-store` CLI, `classes.json`, engine CMake block |
| `80e27e04` | fixture tree (`make_store_tree.py` with the real writer calls) and legacy record fixtures |
| `da25e5cc` | follows SQL1 fix round 1: explicit StoreOpen; `cache init` creates through `create_cache` |
| `56469cec` | gtests (ResearchCatalog / SealGuard / StoreCli) and the tests CMake block |
| `73fceb8d` | task 5: render-identity checker and the identity / classes / blind pytests |
| `47349322` | handoff at the owner stop |
| `6954b605` | resume: the registry-shape test allows one schema in two classes with disjoint globs; `classes.json` description documents this and the `registers` key |

Merges: `51ce19f2` (SQL1 `4d198fef`) and `afea225d` (SQL1 fix `1a5b051d`).

## Files changed
**Owned (new):**
- `atx-engine/include/atx/engine/research/store/catalog/`: `rows_records.hpp`, `ops_records.hpp`, `catalog.hpp`,
  `classify.hpp`, `seal_guard.hpp`, `pins.hpp`, `ingest.hpp`, `query.hpp`, `store_cli.hpp`.
- `atx-engine/src/research/store/catalog/`: `tables_records.hpp`, `records_ops.cpp`, `catalog.cpp`, `classify.cpp`,
  `seal_guard.cpp`, `pins.cpp`, `ingest_{run,stage,cycle,wave,spec,ledger,fields,build}.cpp`, `query.cpp`,
  `store_cli.cpp`, `store_main.cpp`. Private helpers not named in the brief: `catalog_detail.hpp`, `json_text.cpp`,
  `cache_index.cpp`.
- `atx-engine/schemas/research_store/classes.json`.
- `atx-engine/tests/research/`: `research_catalog_test.cpp`, `research_catalog_seal_test.cpp`,
  `research_catalog_cli_test.cpp`, `research_catalog_test_support.hpp`.
- `atx-engine/tests/fixtures/research_store/`: `schema/catalog_records.json`, `make_store_tree.py`, `tree/**`
  (29 files, including `tree/.gitattributes` `* -text`) and `legacy_records/**` (3 files).
- `atx-engine/tools/`: `research_store_identity.py` and `test_research_store_{identity,classes,blind}.py`.

**Cross-lane (CMake tails, P2):**
- One block at the tail of `atx-engine/CMakeLists.txt`: library `atx-engine-research-catalog` and executable
  `atx-research-store`, both `EXCLUDE_FROM_ALL`, with no PCH entry.
- One block at the tail of `atx-engine/tests/CMakeLists.txt` (`atx-engine-research-catalog-tests`). The tail conflicted
  with SQL1's block at the SQL1 merge; both blocks were kept.

**Not touched:** `scripts/**`, every atx-impl file, `vcpkg.json`, `CMakePresets.json`, SQL1's files,
`research_ledger.py`, `backtest_integrity.py`, `research_spec.py`, `atx-db/`.

## Contracts implemented
- **K-P9-13, the `catalog_records` part (sql-design §3.7):**
  - Descriptors use keys, `kIndexed` / `kVolatile`, `std::optional` nullability and `allowed` sets.
  - `trial_line` is `append_only`.
  - The `pin_status` view has the states `ok` / `declared` / `stale` / `missing` / `unresolved`.
  - `records_group()` is group version 1 with db `Catalog`. `records_ops.cpp` is the only TU that includes SQL1's
    detail headers.
- **CLI and exit codes (§3.9):** `init`, `catalog`, `ingest`, `verify --pins`, `query {artifacts,pins,stale-pins,runs,
  timings,producers} [--json]`, `digest`, `dump --table`, `cache init [--import]`, `cache prune`,
  `schema --json --db catalog|cache` and `quick-check`. Exit codes: 0 ok / 2 usage / 3 refusal / 4 error.
- **Inputs read:** K-P9-3 / P6 producer shapes (`ingest_fields.cpp`), K-P9-10 receipt keys (`ingest_run.cpp`),
  K-P9-9 wave manifest (`ingest_spec.cpp`, `spec_kind` `wave-manifest`) and K-P9-1 registry (`field-registry` class).
- **SQL-7:**
  - SHA-256 values are `verified` (hashed by the catalog) or `declared` (stated by a manifest, file never opened).
  - Files over 16 MiB and year-token paths 2024-2099 are never opened. `field-source` pins are not followed.
  - `--verify-payloads DIR` is the only upgrade from `declared` to `verified`.
- **SQL-9 / G-P5:** ledger lines are stored with their bytes and line SHA-256. `ledger_state.head_sha256` and
  `head_rule` stay NULL. Neither the chain rule nor `spec_digest` is re-implemented, and template digest pins stay
  `unresolved`.

## Required carry items
The SQL2 section of `wave2-carry.md` lists rulings only and no carried minors. Each ruling is covered:
- **SQL-7:** see "Contracts implemented".
- **SQL-9:** the seam is described under "B2 seam" below.
- **SQL-11:** `cache init` is the only verb that creates an index. The CLI help and the `cache init` stderr carry the
  SQL1-MON warning and say that no P9 cell uses an index.
- **SQL-4 / SQL-5:** no dependency line was added and nothing is generated. The schema JSON comes from the
  `constexpr` descriptors.

## Schema literals (ruling SQL2-CLS)
- The only new literal in SQL2's non-test code is `atx.research-store-classes/v1`, the registry's own document id. It
  is registered under `domains` in `classes.json`.
- Every other literal SQL2's C++ / Python names belongs to an existing writer and is already in its class row:
  - `atx.bounded-research-run/v1`, `atx.stage-receipt/v1`, `atx.cycle-nav-binding/v1`, `atx.cycle-verdict/v1`;
  - `atx.wave-result/v1`, `atx.wave-candidate/v1`, `atx.trial-ledger/v1`, `atx.research-role-fields/v1`;
  - `atx.record-store/v1` and `atx.record-digest/v1`.
- Per SQL2-CLS, root adds the literals of other wave-2 lanes to `classes.json` at merge. SQL2 added none for them.
- This settles the handoff's open question.

## Tests written
- **gtests** (`atx-engine-research-catalog-tests`, 21):
  - `ResearchCatalog`: `.RecordsSchemaJsonEqualsFixture`, `.IngestsSyntheticTree`, `.DigestReproducibleFromScratch`,
    `.DigestIndependentOfIngestOrder`, `.PinsResolvedAndStaleDetected`, `.DeclaredPayloadNeverOpenedAndMarkedDeclared`,
    `.VerifyPayloadsUpgradesToVerified`, `.UnparsedJsonCataloguedAsArtifact`, `.CrlfAndLfDetected`,
    `.IngestIsIdempotent`, `.TrialLinesAppendOnly`, `.LedgerHeadSeamReceivesLinesInFileOrder`,
    `.ClassifierUsesGlobsAndSchemas`, `.CacheImportMatchesRecordStore`.
  - `SealGuard`: `.HasSealedYearMatchesTheBackstopRegex`, `.YearTokenPathNeverOpened`, `.OutsideRootsListedNotOpened`,
    `.FieldSourcePinsNotFollowed`.
  - `StoreCli`: `.ExitCodes`, `.QueryRowsOrderedByKey`, `.SchemaJsonEqualsStoreInfo`.
- **pytests:**
  - `test_research_store_identity.py` (16 tests, 6 subtests): each render rule is checked against bytes from the real
    writer calls, in CRLF and LF; a one-byte edit is reported. `FixtureChain` runs the C++ catalog of the fixture tree
    and then the checker.
  - `test_research_store_classes.py` (4 tests, 80 subtests): the guard, a planted unregistered literal that fails,
    `legacy_allow` expiry, and the registry shape.
  - `test_research_store_blind.py` (3 tests): no typed column, table or view identifier matches the statistic list.
    The token list needed no narrowing; there were no false positives on view SQL.

## Evidence
The working directory is `C:/atx-wt/pool-23` for every command. Python is `"C:/Program Files/Python312/python.exe"`.

1. Lane and SQL1 pytests, two seeds:
   ```
   foreach ($s in '0','1') { $env:PYTHONHASHSEED=$s; python -m pytest -q -p no:cacheprovider \
     atx-engine/tools/test_research_store_classes.py atx-engine/tools/test_research_store_blind.py \
     atx-engine/tools/test_research_store_identity.py atx-engine/tools/test_research_store_fixtures.py \
     atx-engine/tools/test_research_store.py }
   ```
   ```
   == PYTHONHASHSEED=0
   42 passed, 119 subtests passed in 3.91s
   exit=0
   == PYTHONHASHSEED=1
   42 passed, 119 subtests passed in 3.76s
   exit=0
   ```
   Per file (seed 0): classes 4 passed / 80 subtests; blind 3 passed; identity 16 passed / 6 subtests; SQL1 fixtures 8
   passed / 18 subtests; SQL1 store 11 passed / 15 subtests. `pytest -v -k FixtureChain` shows
   `FixtureChain::test_cpp_catalog_renders_identically PASSED`, so it ran and was not skipped.
2. Full suite, seed 0:
   `python -m pytest -q -p no:cacheprovider --basetemp <scratch> atx-engine/tools`
   gave `450 passed, 125 subtests passed in 688.43s (0:11:28)`, exit=0.
3. gtests: `build-equity/bin/atx-engine-research-catalog-tests.exe --gtest_filter='ResearchCatalog*:SealGuard*:StoreCli*'`
   ```
   [==========] Running 21 tests from 3 test suites.
   [==========] 21 tests from 3 test suites ran. (15133 ms total)
   [  PASSED  ] 21 tests.
   exit=0
   ```
   The binary is build `p9-sql2-c` (via `scripts/research-build.ps1`). No source under `research/store/**` or
   `atx-core/src/db` is newer than it, and `git diff --stat 56469cec HEAD -- '*.cpp' '*.hpp' '*CMakeLists.txt'` is empty.
4. Fixture chain, in a scratch dir (since deleted):
   ```
   atx-research-store init --catalog <db>                                   -> exit 0
   atx-research-store catalog --catalog <db> --root atx-engine/tests/fixtures/research_store/tree \
       --classes atx-engine/schemas/research_store/classes.json             -> exit 0
     files_seen 29 verified 25 declared 0 skipped 4
     catalog_digest d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69
     checkpoint ok
   atx-research-store digest --catalog <db>                                 -> exit 0
     d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69
   atx-research-store verify --catalog <db> --pins                          -> exit 0 (tail)
     pin receipt-binding ok 1 / unresolved 1; receipt-log ok 2; rule-template ok 1; spec-digest unresolved 3;
     stage-receipt ok 2; wave-manifest ok 1; missing: the 4 field-payload pins of the two absent 16 MiB payloads
     (not committed; the gtests create them) and the planted reference_daily -> daily_missing.csv
   python atx-engine/tools/research_store_identity.py --catalog <db> --root atx-engine/tests/fixtures/research_store/tree
     class cycle-verdict checked 2 mismatches 0
     class wave-result checked 1 mismatches 0
     class wave-candidate checked 2 mismatches 0
     class trial-ledger checked 1 mismatches 0
     total checked 12 mismatches 0                                          -> exit 0
   ```
   This reproduces the digest from the handoff, which was also reproduced earlier with both a relative and an
   absolute `--root`.
5. Flag-absent identity: `git diff --stat 1239a5ff..HEAD -- atx-impl scripts` prints nothing.
6. Builds, all through `scripts/research-build.ps1` (preset equity-dev, one at a time after the memory gate):
   - `p9-sql2-a` (library): exit 0, no warnings.
   - `p9-sql2-b`: failed in a test TU (see Diagnostics).
   - `p9-sql2-c` (library, tests, exe): exit 0, 18.2 s.

### Diagnostics (failed attempts; they support no claim)
- **Build `p9-sql2-b`:** the error was `'Run' is a private member of 'testing::Test'`. The helper was renamed to
  `CliRun`, and `p9-sql2-c` is green.
- **First resume pytest run:** `ClassRegistryGuard.test_registry_shape` failed because
  `atx.bounded-research-run/v1` is in both classes `run-receipt` and `run-start`.
  - Cause: the test assumed one class per literal. The registry is right. `run_bounded_research.py:423-434,497`
    stamps that schema on both `start.json` and `receipt.json`, and the two classes are separated by glob.
  - Fix (`6954b605`): a literal may sit in more than one class only if every such class has file-specific globs (no
    `**` / `**/*.json`) and no glob repeats between them. That is exactly the case in which first-match could not make
    the later class dead. No expected value was edited.

## How root verifies
- **Build** (equity-dev, `scripts/research-build.ps1`):
  `-Targets "atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store"` plus SQL1's three
  targets. Log the compile seconds of `records_ops.cpp` (`scripts\atx-build.ps1 check
  atx-engine/src/research/store/catalog/records_ops.cpp`).
- **gtest:** `atx-engine-research-catalog-tests --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*` (21 tests), plus
  SQL1's filters again.
- **pytest:** `atx-engine/tools` under `PYTHONHASHSEED=0` and `=1`, with explicit paths.
- **Fixture chain:** run the `init` / `catalog` / `digest` / `research_store_identity.py` commands of Evidence item 4.
  Expect 0 mismatches of 12. Record the fixture catalog digest
  `d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69` (29 seen, 25 verified, 0 declared, 4 skipped) once
  as the canary golden. Every later build must reproduce it.
  - The digest does not depend on the `classes.json` bytes: the registry is read for classification only and is not
    hashed.
  - The gtests create the two 16 MiB payloads in their temp copies, so their counts differ: 27 artifacts, 25 verified,
    2 declared.
- **Flag-absent identity:** `git diff --stat <integration base>..<sha> -- atx-impl scripts` is empty. No research
  executable links the new library, because both targets are `EXCLUDE_FROM_ALL`. Research outputs are unchanged by
  construction, and no substitution list is needed.
- **Real tree:** the root-only bounded run and SQL1's X-5 opt-in identity run (`cache init`, cold and warm), exactly as
  the brief says (SQL-7 / SQL-11). Delete the index afterwards.

## B2 seam (check when B2 and SQL2 are both merged)
- `LedgerHeadFn` and `default_ledger_head()` are declared in `ingest.hpp`, with the body in `ingest_ledger.cpp`.
  `default_ledger_head()` returns an empty function, so `ledger_state.head_sha256` and `head_rule` stay NULL.
- SQL4 / root binds B2's chain-head function inside `default_ledger_head()` only. Check that:
  - its signature fits `LedgerHeadFn`;
  - it receives the lines without line ends, in file order;
  - SQL2 never re-implements the chain rule (G-P5).
- `ResearchCatalog.LedgerHeadSeamReceivesLinesInFileOrder` pins the seam contract.

## Deviations from brief / decisions
1. Private helpers outside the brief's file list: `catalog_detail.hpp`, `json_text.cpp` and `cache_index.cpp`, all in
   the owned directory.
2. `cycle_binding.spec_sha256` is nullable, because `cycle_resume` writes null for ref steps.
3. `classes.json` adds three keys:
   - `seed_globs`: walk seeds such as the registry, the window and the build receipts.
   - `registers`: literals that are not document schemas (`wave-reader`).
   - `domains`: digest and document domains.
4. A shared schema across glob-disjoint classes is allowed (run-start / run-receipt), and the registry shape test
   enforces that rule.
5. Rows are pruned only by `--rebuild`, which removes the catalog file and catalogs from scratch. A plain re-catalog
   upserts and keeps rows whose file has gone.
6. `spec_doc.git_tracked` is set by a heuristic (`ingest_spec.cpp:108`): it is true unless the root-relative path
   starts with `build`, because `build-*/` is git-ignored. The catalog never runs git.
7. Build receipts reach the walk through `seed_globs`.
8. In the `pin_status` view, a field-source pin with no artifact row is `unresolved`, not `missing`. The catalog does
   not follow those pins.
9. The identity checker compares the render with `artifact.sha256` and, given `--root`, with the bytes on disk. A
   mismatch reports the first differing byte offset. Reasons carry digests, offsets and key names only, never values
   (blind rule).
10. The lane ran in pool-23 (lease), not pool-24 (brief).

## Open risks / concerns
1. **Merge trap (expected; the guard working as designed).** Slots 1-10 merge before SQL2. Any of them that adds an
   `atx.<name>/v<n>` literal under `scripts/`, `atx-engine/tools/`, `atx-impl/tools/` or the scanned C++ dirs turns
   `test_research_store_classes.py::test_every_schema_literal_is_registered` red at SQL2's merge. Root adds each one to
   its class (or as a dated `legacy_allow` row) in one commit per slot (SQL2-CLS). The failure message names each
   literal and its files. On this base the guard is green: 119 literals, 0 unregistered.
2. **SQL1 drift.** The branch carries SQL1 `4d198fef` and fix `1a5b051d` as merges. If SQL1 gets another fix round
   before slot 10, rebase or merge SQL2 onto SQL1's merged head (P17 pattern). The places that touch SQL1's API are
   `records_ops.cpp`, `store_cli.cpp` (StoreOpen modes) and `cache_index.cpp`.
3. **Real-tree volume.** The build-equity name-only listing (`skipped_path` rows) may be large, and the CMake build
   tree under `build-equity/` is listed as outside-roots. Root's bounded run should log the row counts. Neither has
   been measured, because lanes do not open the real tree.
4. **SQL1-MON.** `book_monitor.py --fit-work` does not read records that exist only in the DB. The CLI warns about
   this, and the index must stay off every research cell (SQL-11).

## Hygiene (DISK, PY-HYG)
- Pytest ran with explicit paths only.
- No python, pytest or atx child process is left running (checked).
- Deleted:
  - the scratch fixture-chain DB dir and the pytest `--basetemp` dir;
  - 5 `__pycache__` dirs in this pool.
- The gtest temp root `%TEMP%/atx_research_catalog_tests` is absent after the run.
- Kept as evidence: the build-tag receipts and logs `build-equity/mega-p9-sql2-{a,b,c}-*` (they are tiny).
- No data dated 2024 or later, and no real research output, was opened.

## Ledger candidates
1. P9 SQL2 fixture catalog digest `d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69` (committed tree, 29
   seen / 25 verified / 0 declared / 4 skipped). It is the canary golden for every later `atx-research-store` build.
2. `run_bounded_research.py` stamps `atx.bounded-research-run/v1` on both `start.json` and `receipt.json`. In
   `classes.json` a schema literal is unique per class except across glob-disjoint classes.
