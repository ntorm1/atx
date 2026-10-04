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

---

## Fix round 1 (PM ruling SQL2-FIX1; review `task-SQL2-review.md` at `1c993ab5`: APPROVE, six Suggested findings)

### Outcome
DONE. All six findings are fixed, each with its own test, in one commit: `bedd090a`. The head is the commit that
appends this section. **The fixture-chain catalog digest did not move.** It is still
`d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69`. Only the printed `files_seen` changed, from 29 to
28 (S5). `catalog_run` is a volatile table, so it sits outside the digest.

### What each finding became
| # | fix | where | test |
|---|---|---|---|
| S1 | A schema literal that two or more classes share is allowed only when every glob of those classes ends in a literal file name (no `*`, `?` or `[` in its last segment), and no file name, lower-cased, belongs to two of the classes. Then no path can match two such classes, so the first-match rule can never make one dead. The old check compared glob strings only. | `test_research_store_classes.py` (`shared_literal_problems`); the `classes.json` description | `ClassRegistryGuard.test_shared_literal_needs_disjoint_literal_file_names`: the review's probes `build-equity/**`, `**/*receipt.json`, `**/*` and `**/*.json` all fail, as do `**`, `**/receipt.json`, `build-equity/x/START.json` (case folded), `**/rec?ipt.json`, `**/[rs]tart.json`, a mixed literal + catch-all, and empty globs. A third literal name (`**/finish.json`) passes. `test_registry_shape` asserts no problems on the real registry. |
| S2 | `sealed_root(root)` is `has_sealed_year` over the root's absolute, lexically normal path. `run_catalog` and `ingest_one` return `Err(PermissionDenied)` for a sealed root. The `catalog` and `ingest` verbs check it first, before `--rebuild` and before the class registry under the root is read, and exit 3. A root below a sealed directory is refused as well. | `seal_guard.{hpp,cpp}`, `catalog.cpp`, `store_cli.cpp` | `SealGuard.SealedRootNeverOpened` (library: root and root/scripts refused, 0 artifact and 0 catalog_run rows). `StoreCli.SealedRootRefusedBeforeAnythingIsRead`: both trees hold a broken `classes.json` where the CLI looks for one. The clean tree exits 4 (the registry was read), the sealed tree exits 3, so the seal check runs first. No catalog file is created. |
| S3 | `import_legacy_records` refuses a sealed DIR (`Err(PermissionDenied)`). A partition or kind directory whose name holds a year token is skipped without being listed or opened, and counted in the new `CacheImportReport::sealed`. A file whose stem is not 64 lower-case hex is rejected before it is opened. That leaves the DIR, partition and kind names as the only free path text below DIR, and all three are checked. The `cache init DIR --import` verb refuses a sealed DIR before it creates the index and exits 3; its output line gains `sealed N`. `cache init` without `--import` reads nothing and is unchanged. | `cache_index.cpp`, `store_cli.{hpp,cpp}` | `SealGuard.CacheImportNeverOpensSealedPaths`: two sealed dirs hold invalid decoys and count as `sealed 2`, not as rejected; `notes.json` is rejected; 2 records are imported; a sealed DIR is refused with 0 rows. The CLI half of `StoreCli.SealedRootRefusedBeforeAnythingIsRead` checks exit 3, no `index.sqlite`, and the exact `imported 0 present 0 rejected 0 sealed 0` line. |
| S4 | `detail::claim_key` runs inside each write closure, before the `candidate` (`id`) or `build_receipt` (`tag`) upsert. If another path already holds the key, the write returns `Err(PermissionDenied)` naming both files in sorted order. The result is a refusal in every walk order and in `ingest`, never last-writer-wins. Re-ingesting the holder itself stays idempotent. The `catalog.hpp` "any walk order" claim now states this rule. This is a hard refusal (exit 3), the same class as the append-only ledger refusal. Build tags got the same rule because the review named them with S4, and `research-build.ps1` puts the tag in the receipt's file name. A duplicate can therefore only arise across `build-equity/` and `build-equity-rel/`. | `catalog.cpp`, `catalog_detail.hpp`, `ingest_spec.cpp`, `ingest_build.cpp`, `catalog.hpp` | `ResearchCatalog.DuplicateKeyAcrossFilesRefusedInEveryOrder` covers forward and reverse walks (both files named), single-file ingest of the second claimant (refused, stored row untouched), re-ingest of the holder (ok), and the same tag in `build-equity/` and `build-equity-rel/`. `StoreCli.DuplicateCandidateIdIsARefusal` covers exit 3 with both paths on stderr. |
| S5 | `files_seen` = verified + declared + skipped - `unparsed` skips. It now counts distinct paths, and a seal-named directory counts as one listed path. The meaning is documented on `CatalogReport`. | `catalog.cpp`, `catalog.hpp` | `ResearchCatalog.IngestsSyntheticTree` expects `files_seen` 30, was 31. This is a deliberate semantic change ordered by S5, not a value fitted to make the test pass. The 30 is 25 + 2 + 4 - 1, where the one unparsed file is `build-equity/fx-nav/extra_nan.json`. The committed tree prints 28, was 29. |
| S6 | `catalog --rebuild` maps a failure to open the old catalog with `open_failure`, exactly as every other verb maps open failures. A foreign store (cache index, foreign application_id: InvalidArgument) exits 3 and was 4 before. The file is left in place. | `store_cli.cpp` | `StoreCli.RebuildOpensTheOldCatalogLikeEveryVerb`: for a cache index and a plain text file, the `--rebuild` exit equals the `digest` exit, and both files remain. The cache index exits 3. A text file is NOTADB, which maps to IoError and exits 4 in every verb, `digest` included; that mapping is SQL1's and was not changed. |

### Evidence (pool-23, after `bedd090a`)
1. **Build** `powershell -NoProfile -File scripts\research-build.ps1 -Tag p9-sql2-d -Targets
   "atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store"`.
   - The dry run first showed `Admitted : True`, with FreeMiB 2683 and CommitMiB 3330.
   - Result: exit=0, receipt `ExitCode 0`, `WallSeconds 94.66`, `CompiledTUs 26`.
   - Warnings: the log has 7 `warning` lines. All 7 are clang-cl's `argument unused during compilation: '/MP'` on the
     spdlog dependency TUs; 0 come from lane code.
2. **gtests:** `build-equity/bin/atx-engine-research-catalog-tests.exe --gtest_filter='ResearchCatalog*:SealGuard*:StoreCli*'`
   ```
   [==========] Running 27 tests from 3 test suites.
   [==========] 27 tests from 3 test suites ran. (13880 ms total)
   [  PASSED  ] 27 tests.
   exit=0
   ```
   That is 21 tests before plus 6 new: `ResearchCatalog.DuplicateKeyAcrossFilesRefusedInEveryOrder`,
   `SealGuard.SealedRootNeverOpened`, `SealGuard.CacheImportNeverOpensSealedPaths`,
   `StoreCli.SealedRootRefusedBeforeAnythingIsRead`, `StoreCli.RebuildOpensTheOldCatalogLikeEveryVerb` and
   `StoreCli.DuplicateCandidateIdIsARefusal`.
3. **pytest:** `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider -rs`, over the same five files as before (classes,
   blind, identity, SQL1 fixtures, SQL1 store).
   ```
   == PYTHONHASHSEED=0
   43 passed, 129 subtests passed in 7.26s
   exit=0
   == PYTHONHASHSEED=1
   43 passed, 129 subtests passed in 6.71s
   exit=0
   ```
   - There were no skips, so `FixtureChain` ran against the new exe.
   - +1 test (S1). The subtest count went 119 -> 129: the old shared-literal subtest became 11 probe subtests.
   - The full 45-file `atx-engine/tools` suite was not re-run. Its only Python change is this test file, plus the
     description string in `classes.json`.
4. **Fixture chain** with the new exe, run twice from scratch, once with a relative and once with an absolute
   `--root`:
   ```
   files_seen 28 verified 25 declared 0 skipped 4
   catalog_digest d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69
   catalog exit=0
   total checked 12 mismatches 0
   identity exit=0          (both runs identical)
   ```
5. **Flag-absent identity:** unchanged. No file under `atx-impl/` or `scripts/` was touched, both targets stay
   `EXCLUDE_FROM_ALL`, and no CMake list changed in this round.

### Notes for root
- **Exit-code changes** (all are refusals that used to be errors or silent behaviour):
  - `catalog --rebuild` on a foreign store: 4 -> 3.
  - A duplicate candidate id or build tag: was last-writer-wins, now exit 3.
  - A sealed `--root`: was opened, now exit 3.
  - A sealed `cache init --import` DIR: was read, now exit 3.
  - The import report line gains ` sealed N`; no caller parses it (checked).
- **R1 (classes guard at the merge slots):** with S1, a class root adds in a SQL2-CLS commit must not reuse an
  existing schema literal unless the file names are literal and disjoint. The pytest message names the conflict.
- **Real-tree bounded run (R4):** a duplicate candidate id or build tag in the real tree now stops the run with exit
  3, naming both files, instead of picking a row silently.
  - Neither was checked on the real tree, because lanes do not open it.
  - A duplicate build tag needs one receipt in each of `build-equity/` and `build-equity-rel/`.
  - If the run stops this way, the remedy is to investigate the two files and then `catalog --rebuild`. A renamed
    candidate file in an existing catalog also refuses until `--rebuild`, the same as an edited ledger line.
- **Path premise of the root seal check:** it reads the whole absolute path. A checkout or temp directory whose own
  path holds a standalone 2024-2099 token would therefore refuse every catalog (fail closed). Today's paths
  (`C:/atx`, `C:/atx-wt/pool-N`, `%TEMP%`) hold none.
- **Rebuild needed:** root builds the three targets anew. Pool-23's object tree is deleted (DISK-2), so this pool has
  no binaries now.

### Files changed in fix round 1 (all owned; no cross-lane edit)
- `atx-engine/include/atx/engine/research/store/catalog/{catalog,seal_guard,store_cli}.hpp`
- `atx-engine/src/research/store/catalog/{catalog.cpp,catalog_detail.hpp,cache_index.cpp,ingest_build.cpp,ingest_spec.cpp,seal_guard.cpp,store_cli.cpp}`
- `atx-engine/tests/research/research_catalog_{test,seal_test,cli_test}.cpp`
- `atx-engine/tools/test_research_store_classes.py`
- `atx-engine/schemas/research_store/classes.json` (description string only)

### Hygiene (fix round 1; DISK, DISK-2)
- Deleted:
  - `atx-db/src/atx_db/__pycache__/` (ignored; from the earlier full-suite run), as instructed;
  - one `atx-engine/tools/__pycache__` from this round's chain run;
  - the scratch chain DBs.
- Deleted the untracked CMake / Ninja object tree of `build-equity/` (about 405 MB): `atx-core/`, `atx-engine/`,
  `atx-impl/`, `atx-tsdb/`, `bin/`, `CMakeFiles/`, `lib/`, `tests/`, `build.ninja`, `.ninja_*`, `CMakeCache.txt`,
  `compile_commands.json`, `*.cmake` and `spdlog.pc`.
- Kept:
  - the build receipts and logs `build-equity/mega-p9-sql2-{a,b,c,d}-*` and `vcpkg-manifest-install.log`;
  - the 79 git-tracked files under `build-equity/` (`audits/**` and `v8-interim3-pitch-render-run2/{receipt.json,
    stdout.log}`), which are part of the checkout and not object files. `git status` stays clean.
- No `build-equity-rel/` exists in this pool.
- The gtest temp root is absent. No pool-23 process is left running.
- No data dated 2024 or later, and no real research output, was opened.
