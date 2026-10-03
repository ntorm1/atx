# P9 lane briefs (paste one section, plus "Rules for every P9 lane", into an Opus 5.5 implementer's dispatch)

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md` (cited as "plan §n"). Finding ids (F-n, P9-Rn, NV-n, OR-n, FD-n, CM-n,
DS-n) and contracts (K-P9-n) are defined in plan §0 and §2.3. Literature ids (lit §n, [n], F1..F18) refer to
`docs/plans/2026-10-02-p9-literature-review.md`. Root fills `<frozen-sha>` and the pool at dispatch.

## Rules for every P9 lane

1. Read first: `.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding: never build C++, never run real data,
   never dispatch subagents, never push, never touch `atx-db/`, files only with the Write / Edit tools because the
   shell hook breaks heredocs), then plan §0.6 and §2.2-§2.3, then the review files your brief names. For C++:
   `.agents/cpp/agent.md` first; write code that compiles first time under clang-cl 18 `/W4 /permissive- /WX`
   (no unused variables, sign conversions or shadowing; 100-column limit; copy the owning file's idiom).
2. Work only in your leased pool on your branch (`feat/p9-<id>-20261003`, base `<frozen-sha>`). Lease:
   `powershell scripts\lease-worktree.ps1 -Branch feat/p9-<id>-20261003 -Base <frozen-sha> -Agent p9-<id>
   -RunId p9-<id>-20261003 -HeartbeatId p9-<id>-hb -MaxPool 20` (root may have leased it for you; check `-Status`).
3. Blind. Do not open any return, IC, Sharpe, turnover or NAV output of 2020-2023 (`build-equity/` NAV, cards,
   marginal, admission and diagnostics files are closed; manifests, receipts and field lists are open). Nothing dated
   2024-01-01 or later is opened by you or by code you run. The numbers in status 7 and the ledger are public.
4. Identity discipline: every change is behind a flag or provably value-preserving; flag absent = byte-identical; say
   exactly how root verifies it (targets, gtest filters, argv, the expected byte-identical files, any substitution
   list). Never edit an expected hash. A Python copy of a C++ rule is deleted only in a later slice, after root's
   identity run (plan §0.6).
5. PM8-12: numerical and research logic goes in atx-engine C++ (generic) or atx-impl C++ (strategy-specific) with
   gtests; Python is orchestration, specs, receipts and reports. No new versioned copy of any script; no new
   `research_fields_*.py` builder module (plan DEC-5).
6. Stay inside "Files in scope". Touching a file another lane owns is a lane failure unless the brief names it as a
   cross-lane edit; list every such edit in the report. CMake: append one block at the end of the owning list.
7. Implement first, then the tests named in the brief (they are the acceptance contract) plus what pins behaviour.
   Run pytest yourself on synthetic data: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   <files>`. C++ tests are written, not run; name the anchored gtest filters root will run.
8. Commit per task: `git add <your files>`, conventional message, trailer
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
9. Report: `.superpowers/sdd/platform-p9-20261003/task-<ID>-report.md`, committed with `git add -f`, in the
   `.agents/harness/TEMPLATES.md` "Lane report" shape: outcome, branch / SHA, files changed, evidence (each pytest
   command with exit code 0 and output tail), how root verifies (build targets, gtest filters, identity runs with
   argv), deviations, cross-lane edits, open risks, 0-3 ledger candidates.
10. Final reply to the PM: at most 15 lines (status DONE / DONE_WITH_CONCERNS / BLOCKED, commit SHAs per task, one test
    line, concerns). An adversarial reviewer reads your exact SHA before merge; fix rounds get a new review.

---

## Lane SQL1: the SQLite store core (wrapper fixes, compile-time table descriptors, record-store cache)

**Pool / branch:** pool-23 (ruling SQL-7; `-MaxPool 23`), `feat/p9-sql1-20261003`, run id `p9-sql1-20261003`,
heartbeat `p9-sql1-hb`. **Wave:** 2; base = the post-wave-1 P9-B0 head. **Effort:** L. **Serves:** infrastructure.
**Merge slot:** wave 2, after the plan's eight lanes (and T2 / COV as the PM orders); directly before SQL2.

**Goal.** One SQLite foundation for research artifacts, in C++ and at compile time (ruling SQL-5): the existing
`atx::core::db` wrapper with its defects fixed in place and a connection policy added (ruling SQL-4); one `constexpr`
table descriptor per record type from which templates produce the DDL, bind / read code, the record digest and the
schema JSON; the engine store library (create, migrate, catalog digest); one generic Python accessor driven by the
printed schema; and the first cache moved off JSON (the fitter / report-card record store), selected by file presence
because its consumers' bytes do not depend on it (ruling SQL-6).

**Read:** `sql-design.md` §0, §3 (all; §3.2 is your defect list, §3.6 the descriptor contract), §4 row C1, §6;
`progress.md` rulings SQL-1..SQL-9; `.agents/cpp/agent.md` (§6 headers, §8 PCH); `atx-core/include/atx/core/db/
{sqlite,blob}.hpp`, `atx-core/src/db/sqlite.cpp`, `atx-core/tests/db_sqlite_test.cpp`, `atx-core/CMakeLists.txt:1-27,
69-120`; `atx-core/include/atx/core/error.hpp:29-47` (add no `ErrorCode`: the values are ABI);
`atx-engine/include/atx/engine/store/{db,schema}.hpp` (the alpha-lifecycle store: do not extend it or copy its
re-stamp versioning); B1's CMake block at the end of `atx-engine/CMakeLists.txt` (idiom);
`atx-engine/tools/record_store.py` and its callers, read-only: `atx-impl/tools/fit_composition_weights.py:1279-1350`,
`atx-impl/tools/alpha_report_card.py:735-755`, `atx-impl/tools/test_fit_composition_weights.py:20-25, 245-260`.

**Contracts.** Writes K-P9-13 (sql-design §3.3-§3.7: the descriptor API, the exact DDL grammar, the schema JSON
`atx.store-schema/v1`, the pragmas, `application_id` 0x41545843 catalog / 0x4154584B cache, `user_version` 1, the
record digest `atx.record-digest/v1`, the catalog digest `atx.catalog-digest/v1`). Reads K-P9-3 (producer shape; P6).

**Files in scope (owned in wave 2).**
- atx-core, existing (ruling SQL-4): `include/atx/core/db/sqlite.hpp`, `src/db/sqlite.cpp`,
  `tests/db_sqlite_test.cpp`. New: `include/atx/core/db/connection.hpp`, `src/db/connection.cpp`,
  `tests/db_connection_test.cpp`; one source line in `atx-core/CMakeLists.txt` (the `add_library(atx-core ...)` list)
  and one line in `atx-core/tests/CMakeLists.txt` (no lane owns either file).
- New engine public headers `atx-engine/include/atx/engine/research/store/`: `store.hpp` (`DbKind`, `TableSchema`,
  `GroupOps`, open / create / migrate, `schema_json`, `catalog_digest`), `digest.hpp` (`DigestStream`),
  `rows_core.hpp`, `rows_cache.hpp` (plain row structs), `ops_core.hpp`, `ops_cache.hpp` (non-template operation
  declarations).
- New engine private sources `atx-engine/src/research/store/`: `detail/table.hpp` (descriptor types, `col`, `table`,
  `consteval` checks), `detail/table_ops.hpp` (the operation templates), `tables_core.hpp`, `tables_cache.hpp` (the
  descriptors), `core_ops.cpp`, `cache_ops.cpp` (the only instantiating TUs), `store.cpp`, `digest.cpp`.
- CMake: one block appended to `atx-engine/CMakeLists.txt` (`atx-engine-research-store`, alias
  `atx::engine-research-store`, `PUBLIC atx::core`, `PRIVATE atx_warnings nlohmann_json::nlohmann_json`, private include
  dir `${CMAKE_CURRENT_SOURCE_DIR}/src` so the detail headers are reached as `research/store/detail/table.hpp`,
  `EXCLUDE_FROM_ALL`, no PCH entry); one block appended to `atx-engine/tests/CMakeLists.txt`
  (`atx-engine-research-store-tests`, sources in `tests/research/`, the same private include dir, fixture path define).
- Python: `atx-engine/tools/research_store.py` (new, the generic accessor); `atx-engine/tools/record_store.py`,
  `atx-engine/tools/test_record_store.py` (existing; ruling SQL-4).
- Tests and fixtures: `atx-engine/tests/research/research_store_{table,schema,digest,open}_test.cpp`;
  `atx-engine/tests/fixtures/research_store/schema/{catalog_core,cache}.json` (group schema fixtures, written by hand in
  the exact printed form), `make_store.py` (test-only: builds a store from schema fixtures with Python's SQLite),
  `digest_oracle.py`, `golden_rows.json`, `golden_digests.json` (test-only digest oracle and its vectors),
  `make_py_fixture.py`, `py_created.sqlite`, `.gitattributes` (`*.sqlite binary`, `*.json text eol=lf`);
  `atx-engine/tools/test_research_store.py`, `atx-engine/tools/test_research_store_fixtures.py`,
  `atx-engine/tools/test_record_store_sqlite.py`.

**Forbidden:** `vcpkg.json`, `CMakePresets.json`, any dependency line (ruling SQL-4: SQLite 3.53.2 is vendored as
`atx_sqlite3`, PRIVATE in atx-core; a second copy in one process corrupts, https://sqlite.org/howtocorrupt.html §2.3);
any generated source file, code-generation step or Python generator (ruling SQL-5); `atx-engine/include/atx/engine/
store/**`; `pch.hpp`; the fitter and the card (read-only); every `scripts/` file; every atx-impl file.

**Cross-lane edits:** none. D2's `test_fit_composition_weights.py` imports `record_store.canonical` and
`record_store.content_sha256`: both stay byte-identical, as do `SCHEMA`, the file layout and the `get` / `put`
contract when no index file is present.

**Tasks, in order.**
1. **Descriptor machinery, core and cache descriptors, schema fixtures (commit alone first; this commit is SQL2's
   base, ruling SQL-7; report its SHA to the PM at once).**
   - `detail/table.hpp`: `enum class Sql : u8 {Int, Bool, Real, U64, Text, Sha256, RelPath, Json, Blob}`; flags
     `kKey`, `kIndexed`, `kVolatile` (a `u8` bit set); `struct ColOpts {u8 flags; i32 since; std::span<const
     std::string_view> allowed;}`; `template <class Row, class M, Sql T> struct Column {std::string_view name; M Row::*
     member; ColOpts opts;}`; `consteval` factory `col<Sql T>(name, &Row::m, flags = 0, allowed = {}, since = 1)`
     that `static_assert`s (with a message) the member type against `T` (sql-design §3.6 table: `Int` `atx::i64`,
     `Bool` `bool`, `Real` `atx::f64`, `U64` `atx::u64`, `Text` / `Sha256` / `RelPath` / `Json` `std::string`, `Blob`
     `std::vector<std::byte>`; `std::optional<X>` allowed for each and meaning nullable); `struct TableOpts {i32
     version; i32 since; bool append_only; bool volatile_table;}`; `template <class Row, class... Cols> struct Table
     {std::string_view name; TableOpts opts; std::tuple<Cols...> cols;}`; `consteval` factory `table<Row>(name, opts,
     cols...)` checking: 1-32 columns, at least one `kKey`, unique names, every column with `since > opts.since` is
     optional, a `kKey` column is not optional. Constrain with concepts, not SFINAE (`.agents/cpp/agent.md` §6).
   - `detail/table_ops.hpp` (function templates; folds over `std::index_sequence`, no recursion; DDL built at run time
     with `std::string`, nothing `constexpr` about strings): `ddl(t) -> std::vector<std::string>` (exact grammar of
     sql-design §3.6, type CHECKs, `allowed` CHECK, indexes `ix_<t>_<c>`, append-only triggers
     `<t>_no_update` / `<t>_no_delete` with `RAISE(ABORT, '<t> is append-only')`), `ddl_since(t, v)` (the migration
     step: the table if `opts.since == v`, else `ALTER TABLE <t> ADD COLUMN ...` for columns with `since == v`);
     `insert_sql`, `upsert_sql` (`ON CONFLICT(<key>) DO UPDATE SET` every non-key column), `select_all_sql` (`ORDER BY`
     the key columns); `bind(Statement&, t, const Row&) -> Status` (1-based, in column order; `U64` via
     `std::bit_cast<atx::i64>`; `Bool` as 0 / 1; `nullopt` -> `bind_null`); `read(const Statement&, t) -> Result<Row>`
     through the checked readers of task 2 (a NULL in a non-optional member or a wrong storage class is
     `Err(InvalidArgument)` naming table and column); `encode(DigestStream&, t, const Row&)` (non-volatile columns
     only); `table_schema(t) -> TableSchema` (a plain struct, no JSON type in any header); `group_ops<Tables...>(name,
     db, version)` building a `GroupOps` (function pointers to non-template thunks defined in the instantiating TU).
   - Public `store.hpp` types: `enum class DbKind : u8 {Catalog, Cache}`; `struct ColumnSchema {std::string name, type;
     bool nullable, indexed, is_volatile; i32 since; std::vector<std::string> allowed;}`; `struct TableSchema
     {std::string name; i32 version, since; bool append_only, is_volatile; std::vector<std::string> key;
     std::vector<ColumnSchema> columns; std::vector<std::string> ddl;}`; `struct GroupOps {std::string_view name;
     DbKind db; i32 version; std::vector<TableSchema> (*tables)(); std::vector<std::string> (*steps)(i32 to_version);
     Status (*digest)(core::db::Database&, DigestStream&); std::vector<ViewSchema> (*views)();}`.
   - `rows_core.hpp` + `tables_core.hpp`: group `catalog_core` (`store_info`, `catalog_run`, `artifact` with
     `sha_source` allowed {`verified`, `declared`}, `artifact_seen` append-only, `skipped_path`, `producer`), exactly
     sql-design §3.7; `rows_cache.hpp` + `tables_cache.hpp`: group `cache` (`record`, `ic_signal`, `ic_result`,
     `pair_key`, `pair_stat`). `ops_core.hpp` / `ops_cache.hpp`: per table `insert(Database&, const XRow&) -> Status`,
     `upsert(...)`, `read_x(const Statement&) -> Result<XRow>`, `select_all_x(Database&) -> Result<std::vector<XRow>>`,
     `digest(const XRow&) -> std::string`; `core_group() -> const GroupOps&`, `cache_group() -> const GroupOps&`.
   - `digest.hpp`: `class DigestStream` (`begin_row(table, version)`, `add_null`, `add_int`, `add_bool`, `add_real`,
     `add_u64`, `add_text`, `add_blob`, `finish() -> std::string` lowercase hex), encoding exactly sql-design §3.5.
   - Schema fixtures `schema/catalog_core.json`, `schema/cache.json`: each is the group object `{name, db, version,
     tables: [...], views: [...]}` of `atx.store-schema/v1` (sql-design §3.6; keys in that order, two-space indent, LF,
     final newline), written by hand from the grammar. `make_store.py`: `build(path, db_kind, groups)` sets the policy
     pragmas, runs every fixture `ddl` statement in one transaction, stamps `application_id` / `user_version`, writes
     `store_info` rows `db_kind` and `schema_json` (the DB document of sql-design §3.6 assembled from the groups with
     `json.dumps(doc, indent=2) + "\n"`).
2. **atx-core wrapper fixes in place (ruling SQL-4) and the policy open.** Fix each defect; keep every existing
   signature and behaviour the existing tests pin:
   - W1 `prepare` ignores the SQL tail [`atx-core/src/db/sqlite.cpp:325-333`]: pass `pzTail`; a tail with anything but
     whitespace or `--` / `/* */` comments -> `Err(ParseError)` (the multi-statement path is `exec`).
   - W2 `prepare` casts `sql.size()` to `int` unchecked [`sqlite.cpp:328`]: `Err(OutOfRange)` above `INT_MAX`, as
     `bind` does [`sqlite.cpp:133-135`].
   - W3 `map_code` maps every `SQLITE_CONSTRAINT` to `AlreadyExists` and sees only the primary code [`sqlite.cpp:30-38,
     68-75`]: map extended codes (`SQLITE_CONSTRAINT_UNIQUE` / `_PRIMARYKEY` -> `AlreadyExists`; `_CHECK`, `_NOTNULL`,
     `_FOREIGNKEY`, `_TRIGGER` -> `InvalidArgument`; plain `SQLITE_CONSTRAINT` -> `AlreadyExists` as today; every
     other code maps by its primary part `rc & 0xff`, so `SQLITE_BUSY_SNAPSHOT` etc. keep today's mapping); `open`
     enables `sqlite3_extended_result_codes(db, 1)`.
   - W4 column readers return 0 / empty on NULL or a wrong storage class, unchecked [`atx-core/include/atx/core/db/
     sqlite.hpp:135-143`; `sqlite.cpp:212-260`]: keep them; add non-template `Statement::checked_int(i32) ->
     Result<i64>`, `checked_double -> Result<f64>`, `checked_text -> Result<std::string>` (copy),
     `checked_blob -> Result<std::vector<std::byte>>`: NULL or a storage class other than the requested one (an
     INTEGER is accepted for a double) -> `Err(InvalidArgument)`; a column index out of range -> `Err(OutOfRange)`.
   - W5 `BlobStream::read` / `write` narrow `i64 offset` and `size_t` length to `int` unchecked [`sqlite.cpp:506-518`]:
     refuse a negative offset, `offset + len > size()` or a value above `INT_MAX` with `Err(OutOfRange)` first.
   - W6 `Transaction` destructor and move-assign call `exec("ROLLBACK")`, which builds a `std::string`, inside
     `noexcept` [`sqlite.cpp:434-440, 451`; `exec` at `:319-322`]: call `sqlite3_exec(handle, "ROLLBACK", nullptr,
     nullptr, nullptr)` on the literal (no allocation).
   - W7 `Database::open` sets no busy timeout and no defensive config [`sqlite.cpp:279-289`]: unchanged for existing
     callers; the policy open below does it.
   - W8 `prepare_cached` discards the `reset` / `clear_bindings` status [`sqlite.cpp:338-342`]: intentional (the code
     reports the previous step's error); add a comment saying so; no behaviour change.
   `connection.hpp/.cpp`: `struct StorePolicy {u32 application_id; i32 user_version; enum class Sync : u8 {Normal,
   Full} sync; i32 busy_timeout_ms; i32 page_size;}`; `[[nodiscard]] Result<OpenedStore> open_with_policy(std::
   string_view path, OpenMode, const StorePolicy&)` where `OpenedStore {Database db; bool created; i32 user_version;}`:
   refuse a path starting `\\` or `//` (UNC) and a drive letter whose `GetDriveTypeW` is `DRIVE_REMOTE`
   (`Err(InvalidArgument)`); `SQLITE_DBCONFIG_DEFENSIVE` = 1 and `trusted_schema=OFF` through `handle()`; busy timeout;
   on an empty file (`application_id` 0, `user_version` 0, no table) set `page_size` first, then `journal_mode=WAL`
   (the returned mode must be `wal`, else `Err(IoError)`), then `synchronous`, `foreign_keys=ON`; on an existing file a
   different `application_id` -> `Err(InvalidArgument)`, `user_version` above the policy's -> `Err(NotImplemented)`
   ("this build cannot read schema N"). Also `checkpoint_truncate(Database&)`, `quick_check(Database&) ->
   Result<std::string>` (first error row, "ok"), `with_immediate(Database&, fn)` (`BEGIN IMMEDIATE`, run `fn`, commit;
   on `SQLITE_BUSY` after the busy handler, retry at most 3 times 50 ms apart). Contract docs on every public function
   (`.agents/cpp/agent.md` §9).
3. **Engine store library.** `core_ops.cpp` and `cache_ops.cpp`: include the detail headers and the descriptors, define
   every function of `ops_core.hpp` / `ops_cache.hpp` and the `GroupOps` thunks, nothing else. `store.cpp`:
   `open_store(path, DbKind, std::span<const GroupOps* const> groups) -> Result<core::db::Database>` (policy from
   sql-design §3.3 by kind; create = every group's `steps(1..version)` in one `BEGIN IMMEDIATE` with both pragmas and
   `store_info`; migrate = the missing steps in one transaction, then rewrite `store_info('schema_json')`);
   `open_cache(dir)` = `open_store(dir / "index.sqlite", Cache, {&cache_group()})`; `schema_json(DbKind, groups) ->
   std::string` (the `atx.store-schema/v1` document with nlohmann `ordered_json`, `dump(2)` + `"\n"`, LF;
   `group_schema_json(const GroupOps&) -> std::string` the group object alone, same settings); `catalog_digest(
   Database&, groups) -> Result<std::string>` (`atx.catalog-digest/v1`: every non-volatile table in group then table
   order, rows by key). `digest.cpp`: `DigestStream` over `atx::core::Sha256`.
4. **Python: generic accessor and record-store backend.** `research_store.py` (stdlib only): `Store.open(path)` sets
   `foreign_keys=ON`, `trusted_schema=OFF`, `busy_timeout=30000`, `isolation_level=None` with explicit `BEGIN
   IMMEDIATE`; reads `application_id`, `user_version` and `store_info('schema_json')`; refuses a missing schema row,
   a `user_version` different from the schema's, an `application_id` different from the schema's; never runs DDL, never
   migrates, computes no digest. Generic per table from the schema: `insert(table, row: dict)`, `upsert`,
   `get(table, key: dict)`, `select(table, where: dict | None)` (always `ORDER BY` the key), `transaction()`; Python
   values checked against the column type (`int` for Int / U64 with U64 stored as its signed bit pattern, `bool` for
   Bool, `float` for Real, `str` for text kinds, `bytes` for Blob). No table name or column list is written in the
   module. `record_store.RecordStore(root)`: SQLite mode iff `root/index.sqlite` exists (partition `root = ""`) or
   `root.parent/index.sqlite` exists (partition `root = root.name`), checked once in `__init__` (every cache index has
   this one name and the cache schema, sql-design §3.3); otherwise exactly
   today's code path. In SQLite mode it prints one line to stderr per process per index file,
   `record_store: index sqlite <posix path>` (the selection evidence until SQL3's receipt `store` block, ruling
   SQL-6); `get` reads the row, requires `key` text == the compact canonical key and re-hashes `content_sha256` (rule
   unchanged, `record_store.py:32-34`); a miss reads the legacy JSON file with today's checks and imports it; `put`
   stores the body compact in insertion order (bodies over 1 MiB to `objects/<sha[0:2]>/<content_sha256>.json`, tmp +
   fsync + rename), one `BEGIN IMMEDIATE` per put; a same-key row with another `content_sha256` returns False; one
   connection per thread; every failure returns None / False as today. The index file itself is created only by C++
   (`atx-research-store cache init`, SQL2), never by Python.
5. Tests (below), report.

**Tests (written after implementing).**
- gtests `atx-core-tests`: the 23 existing `Db*` tests unchanged; `DbSqlite.PrepareRefusesTrailingStatement`,
  `.PrepareAllowsTrailingComment`, `.UniqueViolationIsAlreadyExists`, `.CheckNotNullFkViolationsAreInvalidArgument`,
  `.CheckedReadersRejectNullAndWrongClass`, `.CheckedDoubleAcceptsInteger`, `.BlobStreamRangeChecked`,
  `.TransactionDestructorRollsBack`; `DbConnection.OpenWithPolicyStampsAppIdAndVersion`, `.RefusesForeignApplicationId`,
  `.RefusesNewerUserVersion`, `.RefusesUncPath`, `.NewFilePageSizeThenWal`, `.ImmediateWritersSerialiseAcrossThreads`
  (two threads, two connections, 500 inserts each, none lost), `.QuickCheckOk`.
- gtests `atx-engine-research-store-tests`: `ResearchStoreTable.ToyDdlExactText` (a toy table with every `Sql` type,
  an optional, an index, `allowed`, append-only: the DDL strings equal literals in the test),
  `.BindReadRoundTripEveryType`, `.NullOptionalRoundTrip`, `.U64BitPatternRoundTrip` (`2^64-1`),
  `.ReadRejectsNullInRequiredColumn`, `.AppendOnlyRefusesUpdateAndDelete`, `.UpsertReplacesNonKeyColumns`,
  `.SelectAllOrderedByKeyUnderReverseUnorderedSelects`; `ResearchStoreSchema.GroupJsonEqualsFixture` (core and cache:
  `group_schema_json` == the fixture file bytes), `.DdlAppliesOnVendoredSqlite`, `.DbDocumentHasEveryGroupInOrder`;
  `ResearchStoreDigest.EncodingLiteral` (one row's pre-hash bytes equal a literal), `.GoldenVectors` (C++ digest of each
  `golden_rows.json` row == `golden_digests.json`), `.VolatileColumnsExcluded`, `.RealsByBitPattern` (+0.0 vs -0.0, a
  NaN payload); `ResearchStoreOpen.CreateThenReopen`, `.MigratesToyV1ToV2` (an added optional column),
  `.RefusesForeignApplicationId`, `.RefusesNewerVersion`, `.StoreInfoHoldsSchemaJson` (== `schema_json(...)`),
  `.ReadsPythonCreatedFixture` (`py_created.sqlite`), `.CatalogDigestIndependentOfInsertOrder`.
- pytests: `test_research_store_fixtures.py` (every fixture `ddl` statement applies on Python's SQLite 3.43.1; CHECKs
  refuse a bad sha256, relpath, bool and `allowed` value; `make_py_fixture.py` reproduces the logical content of
  `py_created.sqlite`; `digest_oracle.py` reproduces `golden_digests.json`), `test_research_store.py` (on a store made
  by `make_store.py`: open checks and refusals, generic insert / upsert / get / select, ordering under
  `reverse_unordered_selects`, type refusals, `foreign_keys` on, WAL), `test_record_store_sqlite.py` (no index file =
  today's files and bytes and no stderr line; round trip; key mismatch and content tamper are misses; read-through
  import; a 2 MiB body goes to `objects/`; two processes put concurrently with no loss; body key order preserved;
  write failure returns False; the stderr line printed once); the existing `test_record_store.py` and
  `atx-impl/tools/test_fit_composition_weights.py` pass unchanged.

**Root verifies.** Build (equity-dev): `powershell -File scripts\research-build.ps1 -Tag p9-2<letter> -Targets
"atx-core-tests,atx-engine-research-store,atx-engine-research-store-tests"`; gtests `atx-core-tests
--gtest_filter=Db*` and `atx-engine-research-store-tests --gtest_filter=ResearchStore*`; the existing users of the
wrapper whole: `atx-engine-store-tests`, `atx-engine-library-tests`. pytest `atx-engine/tools` and `atx-impl/tools`
(two hash seeds), 0 failed. Compile-time bound (ruling SQL-5): root times `powershell scripts\atx-build.ps1 check
atx-engine\src\research\store\core_ops.cpp` and `cache_ops.cpp` and logs the seconds in the merge record; above 60 s
for one TU is a PM decision (split the TU per table family), not a merge block.

**Flag-absent identity (root procedure).** (1) No research executable links `atx-engine-research-store`; the only
research-path change is `record_store.py`, whose no-index path is unchanged: `git diff --stat <base>..<sha> --
atx-impl scripts` is empty. (2) With no `index.sqlite` anywhere, the fitter and card tests pass unchanged and no
stderr line appears. (3) The opt-in identity runs (X-5 fit and card with no index, cold index, warm index) need
`atx-research-store cache init` and run at SQL2's merge (brief-SQL2 "Root verifies").

**Out of scope:** the catalog group, ingesters and the `atx-research-store` executable (SQL2); IC caches (SQL4);
writer hooks and the receipt `store` block (SQL3); a C++ record-cache API (P10); the alpha-lifecycle store.
