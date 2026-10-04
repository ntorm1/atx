# Lane SQL1 report

## Outcome

DONE_WITH_CONCERNS. All of the brief's tasks 1-5 are implemented, built through the wrapper with 0 warnings, and
tested. 24/24 new engine gtests pass. All 23 existing `Db*` gtests pass unchanged, plus 15 new `Db*` gtests. The
existing wrapper users pass: `atx-engine-store-tests` 40/40 and `atx-engine-library-tests` 66/66. 426
`atx-engine/tools` pytests pass, and the fitter and card pytests pass unchanged. The concerns are listed below:
`book_monitor --fit-work` cannot see DB-only records, and the brief's file plan needed a few deviations.

## Branch / SHA

`feat/p9-sql1-20261003` in `C:/atx-wt/pool-21`, base `20443022757a14d66622794c6de2103991306973`.

| commit | task | content |
|---|---|---|
| `a5070246` | 2 | atx-core wrapper defects W1-W8 fixed in place; `connection.{hpp,cpp}` policy open; `DbSqlite.*` and `DbConnection.*` gtests |
| `03fc676d` | 1 | **SQL2's base (ruling SQL-7)**: descriptor machinery, the catalog_core and cache descriptors, public headers, schema fixtures, `make_store.py` |
| `ffadcacd` | 3 | store library (`core_ops.cpp`, `cache_ops.cpp`, `digest.cpp`, `store.cpp`), CMake blocks, 24 gtests, digest oracle, `py_created.sqlite` |
| `811e6283` | 4 | `research_store.py`, `record_store.py` SQLite mode, 3 pytest files, the toy group fixture |
| (this commit) | 5 | this report |

Task 2 is committed before task 1 because the task-1 headers use task 2's API (`Statement::checked_*` in
`table_ops.hpp`, `StorePolicy` in `store.hpp`). The task-1 commit holds only task-1 files, and its headers compile
against its parent. The task-1 SHA was sent to the PM (`main`) as soon as it existed.

## Frozen base / lease

base_sha=20443022757a14d66622794c6de2103991306973; worktree=C:\atx-wt\pool-21; lease_name=pool-21;
lease_run_id=p9-sql1-20261003; heartbeat_id=p9-sql1-20261003-hb; keeper_pid=12656;
acquired=2026-10-03T18:40:36.9465380Z. The lease is NOT released (as instructed).

Acquisition: `powershell scripts\lease-worktree.ps1 -Branch feat/p9-sql1-20261003 -Base 20443022 -Agent p9-sql1
-RunId p9-sql1-20261003 -HeartbeatId p9-sql1-20261003-hb -MaxPool 24`. The script created pool-21, switched the
branch, initialised the submodule and published the lease. It then exited 1 at its optional cold-tree
`configure -Preset dev` step, as every wave-1 lease did (`atx-vol/` is absent at this base; see pools.md). The lease
holds: `-Status` shows `pool-21 ... LEASED run_id=p9-sql1-20261003 | agent=p9-sql1 | owner_kind=heartbeat |
owner=alive | keeper_pid=12656`. I configured `equity-dev` separately with `scripts\atx-build.ps1 configure -Preset
equity-dev` (exit 0).

The pool is pool-21, not pool-23. The script takes the lowest free number and cannot target one, so the brief's
pool-23 was not reachable.

## Files changed

atx-core (ruling SQL-4):
- `atx-core/include/atx/core/db/sqlite.hpp`: checked readers declared; contracts for prepare, open and result codes.
- `atx-core/src/db/sqlite.cpp`: W1-W6 and W8 fixes.
- `atx-core/include/atx/core/db/connection.hpp`, `atx-core/src/db/connection.cpp` (new).
- `atx-core/tests/db_sqlite_test.cpp` (8 tests appended; the 23 existing tests untouched),
  `atx-core/tests/db_connection_test.cpp` (new).
- `atx-core/CMakeLists.txt` (+1 source line), `atx-core/tests/CMakeLists.txt` (+1 test line).

atx-engine, public headers (`include/atx/engine/research/store/`): `store.hpp`, `digest.hpp`, `rows_core.hpp`,
`rows_cache.hpp`, `ops_core.hpp`, `ops_cache.hpp`.

atx-engine, private sources (`src/research/store/`): `detail/table.hpp`, `detail/table_ops.hpp`, `tables_common.hpp`
(new file, see Decisions), `tables_core.hpp`, `tables_cache.hpp`, `core_ops.cpp`, `cache_ops.cpp`, `store.cpp`,
`digest.cpp`.

CMake:
- One block in `atx-engine/CMakeLists.txt`, placed after the research-fields block and before
  `if(ATX_BUILD_TESTS)` (the tail of the target definitions).
- One block appended at the end of `atx-engine/tests/CMakeLists.txt`.

Tests (`atx-engine/tests/research/`): `research_store_{table,schema,digest,open}_test.cpp`,
`research_store_test_support.hpp` (the toy descriptors).

Fixtures (`atx-engine/tests/fixtures/research_store/`): `schema/{catalog_core,cache,toy}.json`, `make_store.py`,
`digest_oracle.py`, `golden_rows.json`, `golden_digests.json`, `make_py_fixture.py`, `py_created.sqlite` (88 KiB,
binary), `.gitattributes`.

Python (`atx-engine/tools/`): `research_store.py` (new), `record_store.py`, `test_research_store.py`,
`test_research_store_fixtures.py`, `test_record_store_sqlite.py`. `test_record_store.py` is unchanged.

No file under `atx-impl/` or `scripts/`, no `vcpkg.json`, `CMakePresets.json`, `pch.hpp` or
`atx-engine/include/atx/engine/store/**`, and no `ErrorCode` enumerator.
`git diff --stat 20443022..HEAD -- atx-impl scripts` is empty.

## API surface for SQL2 / SQL3 / SQL4

**atx-core (`atx/core/db/`)**
- `Statement::checked_int/_double/_text/_blob(i32) const -> Result<T>`:
  - `Err(InvalidArgument)` for NULL or a wrong storage class (`checked_double` accepts an INTEGER);
  - `Err(OutOfRange)` when no row is current or the column is outside it.
- `connection.hpp`:
  - `StorePolicy {application_id, user_version, sync, busy_timeout_ms, page_size}`;
  - `open_with_policy(path, OpenMode, policy) -> Result<OpenedStore{db, created, user_version}>`;
  - `stamp_identity`, `read_application_id`, `read_user_version`, `checkpoint_truncate`, `quick_check`;
  - `with_immediate(db, fn)`: template; retries Unavailable 3 times, 50 ms apart.
- `open_with_policy` does not stamp a new file; the caller stamps inside its create transaction (see Decisions).

**Engine, consumers (public headers only)**
- `store.hpp`:
  - `DbKind {Catalog, Cache}`; `ColumnSchema`, `TableSchema`, `ViewSchema`;
  - `GroupOps {name, db, version, tables(), steps(v), digest(db, stream), views()}`;
  - `store_policy(kind, v)`, `open_store(path, kind, span<const GroupOps* const>, StoreOpen how)`,
    `open_cache(dir)` (never creates), `create_cache(dir)` (fix round 1: see "Fix round 1" below);
  - `schema_json(kind, groups)`, `group_schema_json(group)`, `catalog_digest(db, groups)`;
  - constants: `kCatalogApplicationId` 0x41545843, `kCacheApplicationId` 0x4154584B, `kStorePageSize` 8192,
    `kStoreBusyTimeoutMs` 30000, `kCacheIndexName` "index.sqlite", `kStoreSchemaId`, `kDbKindKey`, `kSchemaJsonKey`.
- `digest.hpp`: `DigestStream(Kind::Record|Catalog)` with `begin_row`, `add_null/int/bool/real/u64/text/blob`,
  `add_entry(table, row_digest)` (catalog lines) and `finish() -> std::string` (64 lower-case hex, call once).
- `ops_core.hpp` / `ops_cache.hpp`, per table X:
  - `insert(db, XRow)`, `upsert(db, XRow)`, `read_X(stmt)`, `select_all_X(db)`, `digest(XRow) -> std::string`;
  - group accessors `core_group()`, `cache_group()`.
  - store_info's ops live in `ops_core.hpp`.
  - `read_X` requires a statement whose result columns are the table's columns in row-member order.

**Engine, groups (SQL2 `records_ops.cpp`, SQL4 v2)**
- Include `research/store/detail/table.hpp` and `detail/table_ops.hpp` only. They are reachable through the
  library's PRIVATE include dir `atx-engine/src`; a new instantiating TU added to the `atx-engine-research-store`
  source list (or to a library with the same PRIVATE include) sees them.
- **Do not fold `kStoreInfoTable` (`tables_common.hpp`) into `catalog_records`** (corrected in fix round 1).
  catalog_core already carries `store_info`. `open_store` refuses a store whose groups carry it zero times or twice,
  or repeat any table name across groups.
- A descriptor is `inline constexpr auto kX = table<XRow>("x", TableOpts{.version, .since, .append_only,
  .volatile_table, .check}, col<Sql::...>("c", &XRow::c, flags, allowed, since)...)`.
- The group is `constexpr GroupOps kG = group_ops<kA, kB, ...>("name", DbKind::Catalog, 1, &views_fn)`.
- `views_fn` returns `std::vector<ViewSchema>`; each `sql` is a full `CREATE VIEW` statement. `open_store` creates
  views after the tables and drops and re-creates them on a migration.
- Templates available to that TU: `ddl`, `ddl_since`, `insert_sql`, `upsert_sql`, `select_all_sql`, `bind`, `read`,
  `encode`, `row_digest`, `table_schema`, `execute_row`, `select_all`, `digest_table`.
- A child-row record digest (a run and its argv rows) is built by calling `encode()` for each row on one
  `DigestStream`, in key order.

**Python**
- `research_store.Store.open(path)`: `.tables`, `.schema`, `.schema_text`, `insert`, `upsert`, `get`,
  `select(where)`, `transaction()`, `connection`, `close()`.
- `StoreError` subclasses `ValueError`.
- `record_store.RecordStore(root)` keeps its `get` / `put` / `path` contract; `record_store.close_indexes()` and
  `INDEX_NAME` / `INLINE_BODY_MAX` are new.

## Build targets and tags (equity-dev, `scripts\research-build.ps1`)

| tag | targets | exit | wall | TUs |
|---|---|---|---|---|
| p9-sql1-a | atx-core-tests, atx-engine-research-store, atx-engine-research-store-tests | 0 | 102.2 s | 111 (cold pool, incl. deps) |
| p9-sql1-b | same (after the toy group was added) | 0 | 32.2 s | 8 |
| p9-sql1-c | atx-engine-store-tests, atx-engine-library-tests (the existing wrapper users) | 0 | 378.7 s | 174 |

`grep -i warning build-equity/mega-p9-sql1-a-build.log` shows no warning outside third-party code. The build runs
with `/W4 /permissive- /WX`, so any warning would have failed it.

## Measured compile times (ruling SQL-5 bound)

Measured with ccache disabled (`CCACHE_DISABLE=1`), the object deleted first, `atx-build.ps1 check -Jobs 1`. Wall
time includes the wrapper's vcvars start-up; a no-op check measured 6.1 s.

| TU | wall | net of wrapper |
|---|---|---|
| core_ops.cpp | 15.0 s | ~8.9 s |
| cache_ops.cpp | 11.4 s | ~5.3 s |
| store.cpp | 10.9 s | ~4.8 s |
| digest.cpp | 9.6 s | ~3.5 s |

All are far below the 60 s PM-decision threshold.

## Tests

**gtests `atx-core-tests --gtest_filter=Db*`: 44 passed**
- The 23 existing tests, unchanged: DbDatabase 8, DbExec 2, DbStatement 8, DbTransaction 2, DbBlob 3.
- DbSqlite (8, new): PrepareRefusesTrailingStatement, PrepareAllowsTrailingComment, UniqueViolationIsAlreadyExists,
  CheckNotNullFkViolationsAreInvalidArgument, CheckedReadersRejectNullAndWrongClass, CheckedDoubleAcceptsInteger,
  BlobStreamRangeChecked, TransactionDestructorRollsBack.
- DbConnection (7, new): OpenWithPolicyStampsAppIdAndVersion, RefusesForeignApplicationId, RefusesNewerUserVersion,
  RefusesUncPath, NewFilePageSizeThenWal, ImmediateWritersSerialiseAcrossThreads (2 threads × 500, none lost),
  QuickCheckOk.
- The `Db*` filter also matches 6 existing `Dbn*` decoder tests.

**gtests `atx-engine-research-store-tests --gtest_filter=ResearchStore*`: 24 passed**
- ResearchStoreTable (10): ToyDdlExactText, BindReadRoundTripEveryType, NullOptionalRoundTrip,
  U64BitPatternRoundTrip, ReadRejectsNullInRequiredColumn, AppendOnlyRefusesUpdateAndDelete,
  UpsertReplacesNonKeyColumns, SelectAllOrderedByKeyUnderReverseUnorderedSelects, plus two extra:
  BindRefusesNanAndNegativeZeroReal, TypeChecksRefuseBadValues.
- ResearchStoreSchema (3): GroupJsonEqualsFixture (core, cache and toy fixtures, byte for byte),
  DdlAppliesOnVendoredSqlite, DbDocumentHasEveryGroupInOrder.
- ResearchStoreDigest (4): EncodingLiteral, GoldenVectors (7 oracle vectors, plus the toy and artifact rows through
  the descriptors), VolatileColumnsExcluded, RealsByBitPattern.
- ResearchStoreOpen (7): CreateThenReopen, MigratesToyV1ToV2, RefusesForeignApplicationId, RefusesNewerVersion,
  StoreInfoHoldsSchemaJson, ReadsPythonCreatedFixture, CatalogDigestIndependentOfInsertOrder.

**pytest: 30 passed, 33 subtests**
- `test_research_store_fixtures.py` (8):
  - DDL applies on Python's SQLite 3.43.1, including column, NOT NULL, key, STRICT and WITHOUT ROWID checks;
  - fixture key order and LF bytes;
  - CHECK refusals for sha256, relpath, bool, json, allowed values, STRICT types, append-only and the record
    body / body_file rule;
  - `make_py_fixture` reproduces `py_created.sqlite`;
  - `digest_oracle` reproduces `golden_digests.json`, plus an encoding literal.
- `test_research_store.py` (10):
  - open pragmas and refusals; no DDL and no file creation;
  - every-type round trip; upsert, get, select with where;
  - ordering under `reverse_unordered_selects`;
  - type refusals; integrity errors; transaction rollback; the catalog group.
- `test_record_store_sqlite.py` (9):
  - no index: today's files and bytes and no stderr line;
  - root and parent partitions; key mismatch and tamper are misses; read-through import;
  - a 2 MiB body goes to `objects/`;
  - write failures return False;
  - stderr line printed once; two processes × 60 puts, no loss.
- `test_record_store.py` (3): unchanged.

**Full suites run by me**
- `atx-engine/tools` with PYTHONHASHSEED=0: 426 passed, 39 subtests, 419.7 s.
- The four new or changed modules again with PYTHONHASHSEED=1: 30 passed.
- `atx-impl/tools/test_fit_composition_weights.py` + `test_alpha_report_card.py`: 116 passed in 121 s.
- `atx-impl/tools/test_alpha_report_card_store.py` + `test_fit_composition_weights_store.py` +
  `test_fit_composition_weights_pool.py`: 50 passed in 62 s.

## Evidence (exit code 0 for each)

```
research-build.ps1 -Tag p9-sql1-a -Targets "atx-core-tests,atx-engine-research-store,atx-engine-research-store-tests"
  "ExitCode": 0, "WallSeconds": 102.2326265, "CompiledTUs": 111
research-build.ps1 -Tag p9-sql1-c -Targets "atx-engine-store-tests,atx-engine-library-tests"
  "ExitCode": 0, "WallSeconds": 378.725694, "CompiledTUs": 174
build-equity/bin/atx-engine-store-tests.exe --gtest_brief=1
  [==========] 40 tests from 15 test suites ran. (302 ms total)   [  PASSED  ] 40 tests.
build-equity/bin/atx-engine-library-tests.exe --gtest_brief=1
  [==========] 66 tests from 11 test suites ran. (7692 ms total)  [  PASSED  ] 66 tests.  (2 DISABLED, pre-existing)
build-equity/bin/atx-engine-research-store-tests.exe --gtest_brief=1
  [==========] 24 tests from 4 test suites ran. (341 ms total)   [  PASSED  ] 24 tests.
build-equity/bin/atx-core-tests.exe --gtest_filter='Db*'
  [  PASSED  ] 44 tests.
python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store_fixtures.py
    atx-engine/tools/test_research_store.py atx-engine/tools/test_record_store_sqlite.py
    atx-engine/tools/test_record_store.py
  30 passed, 33 subtests passed in 3.11s
PYTHONHASHSEED=0 python -m pytest -q -p no:cacheprovider atx-engine/tools
  426 passed, 39 subtests passed in 419.71s (0:06:59)
python -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_weights.py
    atx-impl/tools/test_alpha_report_card.py
  116 passed in 121.12s (0:02:01)
python -m pytest -q -p no:cacheprovider atx-impl/tools/test_alpha_report_card_store.py
    atx-impl/tools/test_fit_composition_weights_store.py atx-impl/tools/test_fit_composition_weights_pool.py
  50 passed in 61.93s (0:01:01)
```

`record_store.py`'s no-index `_file_get` / `_file_put` match the base's `get` / `put` bodies byte for byte (diffed
after the rename: identical).

## Decisions (the brief was ambiguous or contradicted the design)

1. **The cache group carries `store_info` too, and `store_info` is a volatile table.**
   - Section 3.7 lists `store_info` only under catalog_core. But `open_store` writes `store_info('schema_json')` for
     both kinds, and Python reads it from every store (SQL-10). A cache index without the table could not describe
     itself.
   - One shared descriptor lives in the new private header `tables_common.hpp`; its per-table ops are defined once,
     in `core_ops.cpp`.
   - It is volatile because a store's self description (schema text, SQL2's `created_by` exe SHA) is not catalog
     content and would make the catalog digest depend on the executable.
2. **`TableOpts` gains `std::string_view check`** (an optional table CHECK, printed as `, CHECK(<expr>)` after the
   PRIMARY KEY). Section 3.7 requires "exactly one of body / body_file (checked by the writer and a table CHECK)",
   and the grammar had no table CHECK. `record` uses `(body IS NULL) <> (body_file IS NULL)`.
3. **`open_with_policy` does not stamp `application_id` / `user_version` on a new file.**
   - It reports `created = true`. `open_store` stamps both in the same `BEGIN IMMEDIATE` that creates the tables
     (section 3.4), via the new `stamp_identity`.
   - A crash can therefore never leave a stamped, empty store.
   - `open_store` re-reads both pragmas under the write lock, so two processes creating the same file do not both
     run the DDL.
   - `DbConnection.OpenWithPolicyStampsAppIdAndVersion` tests open → stamp → reopen.
4. **W3 adds `SQLITE_CONSTRAINT_DATATYPE` → InvalidArgument** next to the brief's CHECK / NOT NULL / FOREIGN KEY /
   TRIGGER. It is a STRICT column given a wrong storage class, a refused value like the others. Every other code
   maps by its primary part, as the brief says. `backup_to` compares busy / locked on the primary part, because the
   connections now return extended codes.
5. **REAL NaN and -0.0 are refused at bind** (`Err(InvalidArgument)` naming table.column), and Python does the same.
   - SQLite stores NaN as NULL and writes an integral REAL as an integer, so -0.0 reads back as +0.0. I checked this
     against the vendored `sqlite3VdbeIntegerAffinity`.
   - A silent change would break the "digest of what was stored" identity.
6. **A column's effective `since` is `max(column since, table since)`.** A later column (since > table since) must
   be optional (consteval check). `ddl()` prints the current-version table with every column inline. `ddl_since(t, v)`
   is the migration step. A store created fresh at v2 and one migrated from v1 have the same columns (tested).
7. **DB `user_version` = the largest group version**; `steps(v)` is empty for a group that has nothing at v.
8. **Fixtures were transcribed independently of the C++.** A throwaway script in my scratchpad (not committed)
   wrote them from the section 3.6 grammar. The gtest then compares the C++ output to them byte for byte, so the
   comparison checks two independent renderings.
   - `schema/toy.json` was added: the real groups have no bool or blob column, and the Python type tests need them.
9. **`record_store` details.**
   - One connection per thread *per index file* is shared across `RecordStore` instances (the fitter builds many).
   - A DB miss or failed check falls back to the legacy file. A valid legacy record is imported (best effort) and
     returned.
   - An index that cannot be opened or is not a cache index counts as a miss (get) or False (put).
   - A non-finite or unserialisable body still raises, as today.
10. **CMake placement.** The library block sits before `if(ATX_BUILD_TESTS)` in `atx-engine/CMakeLists.txt` (the
    research-fields idiom). The test block is at the file end and registers its own `gtest_discover_tests` under
    label `atx_research`; T1's deferred function skips a registered target.
11. **SQL1-EARLY reads "atx-core only".** I read it as "depends on atx-core only, touches no atx-impl / research
    file". The brief's file scope puts the store library in atx-engine `research/store`, and that is where it is.

## How root verifies after merge

1. Build: `powershell -File scripts\research-build.ps1 -Tag p9-2<x> -Targets
   "atx-core-tests,atx-engine-research-store,atx-engine-research-store-tests"`. Expect exit 0.
2. Gtests:
   - `build-equity\bin\atx-core-tests.exe --gtest_filter=Db*`: 44 pass, including the 23 existing tests unchanged.
   - `build-equity\bin\atx-engine-research-store-tests.exe --gtest_filter=ResearchStore*`: 27 pass (24 + 3 from fix
     round 1).
   - Or `ctest -L atx_research` for the store tests.
3. Existing wrapper users: build and run `atx-engine-store-tests` and `atx-engine-library-tests` whole (my tag
   p9-sql1-c: `atx-engine-store-tests` 40/40 passed, `atx-engine-library-tests` 66/66 passed with 2 pre-existing
   DISABLED). Review finding 12: W1 and W3 also reach the pipeline stages through `atx/engine/store/*`, so root also
   builds and runs the atx-impl gtests `store_discover_test`, `provenance_test` and `provenance_digest_test`. I did
   not run these.
   - The hygiene (PCH-off) check is already satisfied for the lane's TUs. In `equity-dev`, atx-core,
     `atx-core-tests`, `atx-engine-research-store` and its tests compile without the PCH:
     `build-equity/build.ninja` shows no `cmake_pch` force-include on `connection.cpp`, `sqlite.cpp`,
     `db_connection_test.cpp`, `core_ops.cpp` or the store tests.
   - `atx-build.ps1 check -Preset equity-hygiene` on them remains available to root.
4. Pytest, two hash seeds:
   - `python -m pytest -q -p no:cacheprovider atx-engine/tools` (expect 426 passed);
   - `python -m pytest -q -p no:cacheprovider atx-impl/tools`.
5. Compile-time bound: `powershell scripts\atx-build.ps1 check -Preset equity-dev
   atx-engine\src\research\store\core_ops.cpp` and the same for `cache_ops.cpp` (mine: ~9 s / ~5 s net, uncached).
6. Flag-absent identity:
   - (1) No research executable links `atx-engine-research-store`; `git diff --stat 20443022..<head> -- atx-impl
     scripts` is empty.
   - (2) Without an `index.sqlite`, `record_store` runs the base's `get` / `put` bodies verbatim. Its no-index pytest
     pins today's bytes and no stderr. The fitter and card pytests pass unchanged.
   - (3) The opt-in X-5 identity runs (no, cold and warm index) need `atx-research-store cache init` and belong to
     SQL2's merge.
7. Merge conflicts: the expected ones are CMake tails (`atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt`,
   `atx-core/CMakeLists.txt` and `atx-core/tests/CMakeLists.txt` source lists). Keep both sides.

## Concerns / open risks

1. **`book_monitor.py --fit-work` cannot see DB-only factor records.**
   - `fit_composition_weights.stored_factor_series` (used by `book_monitor.py` `fit_records`) finds factor records by
     globbing `<work>/<sha16>-<window>/factor/*.json`.
   - With an `index.sqlite` in the fit work dir, new factor records are written to the DB only. That monitor would
     then miss them, and its residual output would change.
   - So SQL-6's premise ("consumer bytes identical either way") holds for the fitter and the card. It does not hold
     for `book_monitor --fit-work` on an indexed work dir.
   - Both files are outside SQL1's scope (atx-impl, read-only). Needed before any real cell opts in (SQL3 / P10):
     either `stored_factor_series` enumerates through `RecordStore` (a small `iter(kind)` API), or the monitor is
     never pointed at an indexed work dir.
   - SQL-11 already keeps P9 cells off the index, so no P9 evidence is affected today.
2. Orphaned `record` rows: `research_gc` deletes store directories, so rows of a deleted root stay in
   `<work>/index.sqlite` until `atx-research-store cache prune` (SQL2). Harmless (a miss is a recompute) but they
   grow the index.
3. Pool-21 instead of pool-23, and task 2 committed before task 1 (both explained above). SQL2's base is
   `03fc676d`; its parent `a5070246` carries the wrapper fixes SQL2 needs.
4. `py_created.sqlite` is 88 KiB (11 schema objects × 8 KiB pages). It is marked binary and only changes together
   with `make_py_fixture.py` (pytest-checked).
5. Python's `sqlite3` on the host is 3.43.1. The DDL uses nothing newer (STRICT 3.37, json 3.38, `pragma_table_list`
   3.37 in tests), and pytest checks every fixture statement on it.

## Deviations from brief

- `store_info` was added to the cache group and made volatile (Decision 1).
- `TableOpts.check` was added (Decision 2).
- The new private header `tables_common.hpp` and the toy fixture `schema/toy.json` were added (Decisions 1 and 8).
- DATATYPE maps to InvalidArgument (Decision 4); NaN and -0.0 REAL are refused (Decision 5).
- Commit order is task 2 before task 1, and the lane ran in pool-21.
- C++ was built and run here: the PM's dispatch asked for wrapper builds with 0 warnings, which overrides the brief's
  "written, not run".

Cross-lane edits: none.

## Ledger candidates

- SQLite stores a REAL -0.0 as integer 0 and NaN as NULL (`sqlite3VdbeIntegerAffinity`), so a typed REAL column
  cannot round-trip either value. The research store refuses both at bind, in C++ and in Python.
- The research-store descriptor TUs compile in ~9 s (core_ops) and ~5 s (cache_ops) uncached under clang-cl 18 Debug.
  This is the measured cost of ruling SQL-5's compile-time generator.
- A `consteval` factory can reject a descriptor with a readable rule name by calling a deliberately non-constexpr
  `descriptor_error_<rule>()` on the failing branch; clang 18 prints the function name in the diagnostic.

## Fix round 1 (ruling SQL1-FIX1; review `task-SQL1-review.md` at 4d198fef: APPROVE, 12 minors)

### Required fixes

1. **Schema drift at an equal version (finding 1).**
   - `open_store` now compares `store_info('schema_json')` with `schema_json(kind, groups)` whenever the stored
     `user_version` equals this build's version. That covers the plain open, and the concurrent-creator path that
     re-reads the pragmas under the write lock.
   - A mismatch is `Err(InvalidArgument)` "schema drift: ... rebuild the derived store or migrate with a new
     version".
   - `store.hpp` now documents that versions are store-global: a group's new version must exceed every version the
     store has reached.
   - Test `ResearchStoreOpen.RefusesSchemaDriftAtSameVersion` covers two cases:
     - a descriptor edited in place without a bump;
     - a group added at a version the store already has.
     In both, the store is left untouched and still opens with its own groups.
2. **The `store_info` precondition (finding 2).**
   - `open_store` validates the group list before touching any file. Table names must be distinct across groups,
     and exactly one group must carry `store_info`. Breaking either is `Err(InvalidArgument)` with a message naming
     the rule.
   - Documented on `open_store` in `store.hpp`. The report's SQL2 note is corrected: SQL2 must NOT include
     `tables_common.hpp` or fold `kStoreInfoTable` into `catalog_records`.
   - Test `ResearchStoreOpen.RequiresExactlyOneStoreInfoGroup` refuses three group lists, with no file created:
     no `store_info` at all, `store_info` twice, and a table name repeated.
   - The same test opens a valid two-group catalog and checks `catalog_digest` over both groups against the exact
     line sequence (closes that part of finding 11).
3. **Creation is explicit (finding 3).**
   - `open_store` takes a required `StoreOpen how`. `Existing` never creates a store: a missing or 0-byte file is
     `Err(NotFound)` and is left untouched. `CreateIfMissing` creates one.
   - `open_cache(dir)` is now `Existing`. The new `create_cache(dir)` is the only way to create a cache index (for
     SQL2's `cache init`).
   - Test `ResearchStoreOpen.OpenCacheNeverCreatesAnIndex` checks three things:
     - `open_cache` on an empty dir creates nothing;
     - on a 0-byte `index.sqlite` it leaves the file at 0 bytes;
     - `open_store(Existing)` creates no catalog.
     It then runs `create_cache` followed by `open_cache`.

### Minors fixed (each small)

- **4:** `Column` itself is now `requires StorableAs<M, T>`. `col()` deduces its return type so the readable
  static_assert message still fires first. `research_store_table_test.cpp` gains `static_assert`s: `Column` is not
  formable for `i32` / `bool` as Int, and `StorableAs` is false for the four named mismatches.
- **5:** `col()` now rejects three combinations at compile time: `allowed` on a non-Text kind, `kKey | kVolatile`, and
  a non-ASCII `allowed` byte. A temporary probe (reverted) showed each fails with
  `descriptor_error_allowed_on_non_text_column`, `descriptor_error_bad_flags` and
  `descriptor_error_allowed_value_has_quote_or_non_ascii`.
- **6 + 7:** `connection.cpp`'s path resolution is now Windows-only (no unused helper elsewhere). Invalid UTF-8 is
  `Err(InvalidArgument)` instead of a throw. The path is resolved with `weakly_canonical`, so links are followed. A
  `\?\` final-path prefix is stripped, and `\?\UNC\` counts as a share.
- **8:** `research_store.Store`:
  - `transaction()` now rolls back when `COMMIT` itself fails, so a cached connection is never left in a
    transaction. Pinned by `test_a_failed_commit_rolls_back`, which uses a deferred foreign key that fails at
    COMMIT.
  - `open` refuses UNC paths and sets `synchronous` to FULL for a catalog and NORMAL for a cache, pinned in the
    open tests.
- **10:** the two comments that called `sqlite.cpp` the only `<sqlite3.h>` TU now name `connection.cpp` too.
- **11 (part):**
  - The table test asserts only the `table.column: ` prefix and the code.
  - The volatile test asserts the exact `catalog_run` record digest instead of its length.
  - `catalog_digest` over two groups is now tested (fix 2).
- **12:** root's verification list now adds the atx-impl wrapper-user gtests, plus the PCH-off note (above).

### Minors left (larger than the round's ten-line bound)

- **9:** `record_store` heals a damaged same-content row, and stops publishing a large body before the row check.
- **11 (rest):** tests for `with_immediate`'s BUSY retry, the `GetDriveTypeW` branch, the concurrent-creator race and
  migrating a table added at a later version.

### API changes SQL2 must follow

- `open_store(path, kind, groups, StoreOpen how)`: the fourth argument is required.
  - `atx-research-store init` and `catalog` create with `StoreOpen::CreateIfMissing`.
  - Readers (`verify`, `query`, `digest`, `dump`, `quick-check`) use `StoreOpen::Existing`.
- `cache init DIR` calls `create_cache(DIR)`. A cache consumer (SQL4) calls `open_cache(DIR)`, which never creates.
- `catalog_records` must not fold in `kStoreInfoTable` (catalog_core carries it), and must not repeat a table name of
  catalog_core.
- A group change ships with a version past every version the store has reached. Otherwise open refuses with
  "schema drift".
- `col()`: `allowed` lists only on `Sql::Text` columns, ASCII values only; no volatile key column.

### Evidence (fix round 1)

```
research-build.ps1 -Tag p9-sql1-d -Targets "atx-core-tests,atx-engine-research-store,atx-engine-research-store-tests"
  "ExitCode": 0, "WallSeconds": 65.5714994, "CompiledTUs": 11   (admitted on the first try after waiting for the
  memory gate; 0 warnings)
build-equity/bin/atx-engine-research-store-tests.exe --gtest_brief=1
  [==========] 27 tests from 4 test suites ran. (753 ms total)   [  PASSED  ] 27 tests.
build-equity/bin/atx-core-tests.exe --gtest_filter=Db* --gtest_brief=1
  [==========] 44 tests from 10 test suites ran. (7874 ms total)  [  PASSED  ] 44 tests.
python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store_fixtures.py
    atx-engine/tools/test_research_store.py atx-engine/tools/test_record_store_sqlite.py
    atx-engine/tools/test_record_store.py
  31 passed, 33 subtests passed in 5.68s        (PYTHONHASHSEED=1: 31 passed, 33 subtests, 5.50s)
```

The p9-sql1-c result (`atx-engine-store-tests` 40/40, `atx-engine-library-tests` 66/66) predates this round. This
round changes `sqlite.cpp` / `sqlite.hpp` only in comments, and `connection.cpp` (the policy open, which neither
suite calls). So their wrapper behaviour is unchanged, and they were not rebuilt.
