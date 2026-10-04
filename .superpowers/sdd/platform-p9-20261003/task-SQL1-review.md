# Lane SQL1 review

## Verdict

APPROVE

- Spec compliance: PASS. Every item of brief-SQL1 tasks 1-5 is present, including every named gtest and pytest.
  Rulings SQL-2, SQL-4, SQL-5, SQL-6, SQL-10, SQL-11, SQL1-SCOPE and SQL1-MON are respected. The listed deviations
  are sound, and none contradicts a ruling in substance (see "Deviation assessment").
- Quality: PASS with minors. Blockers 0, majors 0, minors 12. No UB was found. Ownership and lifetimes are clean.
  Every wrapper error path finalizes its statement or rolls its transaction back through RAII.

## Reviewed SHA

`4d198fefa25fc052de2108dd3b729e418acc7a02` (`feat/p9-sql1-20261003`, `C:/atx-wt/pool-21`), base `20443022`.
Commits:
- `a5070246`: task 2 (wrapper);
- `03fc676d`: task 1 (descriptors, SQL2's base);
- `ffadcacd`: task 3 (store library + gtests);
- `811e6283`: task 4 (Python);
- `4d198fef`: report.

Each commit holds only its task's files and has the Co-Authored-By trailer.

## Evidence

All commands were run by me in `C:/atx-wt/pool-21` with `PYTHONDONTWRITEBYTECODE=1`. `git status --porcelain` was
empty before and after each run. No C++ was built (forbidden to the reviewer).

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider \
  C:/atx-wt/pool-21/atx-engine/tools/test_research_store_fixtures.py \
  C:/atx-wt/pool-21/atx-engine/tools/test_research_store.py \
  C:/atx-wt/pool-21/atx-engine/tools/test_record_store_sqlite.py \
  C:/atx-wt/pool-21/atx-engine/tools/test_record_store.py
..............................          [100%]
30 passed, 33 subtests passed in 2.60s
exit_code=0

PYTHONHASHSEED=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider \
  C:/atx-wt/pool-21/atx-impl/tools/test_fit_composition_weights.py \
  C:/atx-wt/pool-21/atx-impl/tools/test_alpha_report_card_store.py
101 passed in 114.11s (0:01:54)
exit_code=0
```

Static checks, run on the diff:
- `git diff --stat 20443022 4d198fef -- atx-core/third-party vcpkg.json CMakePresets.json CMakeLists.txt atx-impl
  scripts atx-engine/include/atx/engine/store atx-engine/pch.hpp atx-engine/src/pch.hpp` is empty.
- `error.hpp` is untouched, so no new `ErrorCode` enumerator.
- No C++ line in the diff is longer than 100 columns. The long CMake lines in the engine files pre-date the lane;
  the lane's own blocks are 100 columns or less.
- The no-index `_file_get` / `_file_put` in `record_store.py` are textually identical to the base `get` / `put`.
- The lane's gtest and build claims (44 `Db*`, 24 `ResearchStore*`, store 40/40, library 66/66, 0 warnings) were not
  re-run by me. They are root's to confirm.

## Findings

`path:line | severity | problem | required fix`

1. `atx-engine/src/research/store/store.cpp:230-232` (also `:164-198`) | minor | Schema drift goes undetected when the
   versions are equal.
   - The schema version is global: `user_version` = max(group versions), and `steps(v)` uses that same numbering.
     An existing store whose `user_version` equals V opens with no check that it holds the caller's tables.
   - Three cases slip through:
     - a group added at a version the store already has;
     - a group bumped to a version another group already reached (its steps never run);
     - a descriptor edited in place without a version bump.
   - Each fails later and loudly ("no such table/column"), not silently. But `store_info('schema_json')` stays stale,
     and Python then works from the stale schema.
   - **Fix:** when `found == version`, compare `store_info('schema_json')` with `schema_json(kind, groups)` and
     return `Err(InvalidArgument)` on a mismatch ("schema drift: rebuild or migrate"). Document in `store.hpp` that
     versions are store-global: a group's new version must exceed every version the store has reached.

2. `atx-engine/include/atx/engine/research/store/store.hpp:103-116`; `store.cpp:36-62, 179-181` | minor | Hidden
   `store_info` precondition.
   - `open_store` needs exactly one of its groups to fold in `kStoreInfoTable`. With none, create fails ("no such
     table: store_info"). With two, CREATE fails.
   - This is undocumented. `store_version` also does not check table names for uniqueness across groups.
   - The report's SQL2 guidance ("include `tables_common.hpp`") invites SQL2 to fold `store_info` into
     `catalog_records`.
   - **Fix:** document the rule in `store.hpp`. Check it in `store_version`: table names unique across groups,
     `store_info` present exactly once. Reword the report's SQL2 note.

3. `atx-engine/include/atx/engine/research/store/store.hpp:118-119` | minor | `open_cache` creates a missing index.
   - It opens `ReadWriteCreate`. sql-design §3.9 says `cache init` is the only creator of a cache index.
   - Because `record_store.py` selects by file presence, a C++ cache user (SQL4) that calls `open_cache` on a record
     root or its parent would silently switch the Python consumers of that root to SQLite mode.
   - **Fix:** state in the contract that `open_cache` creates the file. Either add a non-creating variant
     (`ReadWrite`) for consumers, or have SQL4 open only after `cache init`. Record the choice for SQL2/SQL4.

4. `atx-engine/src/research/store/detail/table.hpp:117-137, 143-166` | minor | The type check can be bypassed, and no
   test pins it.
   - The C++/SQL type-match guarantee is enforced only inside `col()` (`static_assert(StorableAs<M, T>)`). `Column` is
     a plain aggregate, so a `Column<Row, i32, Sql::Int>{...}` built by hand reaches `table()` unchecked. It then
     compiles through `read()`'s `i64` -> `i32` assignment.
   - No test pins the negative cases.
   - **Fix:** put `requires StorableAs<M, T>` on `Column` itself. Add `static_assert(!StorableAs<i32, Sql::Int>)`,
     `!StorableAs<float, Sql::Real>`, `!StorableAs<std::string, Sql::Blob>` and
     `!StorableAs<std::optional<i64>, Sql::Bool>` in `research_store_table_test.cpp`.

5. `atx-engine/src/research/store/detail/table.hpp:143-166` | minor | Descriptor combinations that `col()` should
   refuse.
   - It accepts `allowed` on a non-text kind. That emits `CHECK(x IN ('a', ...))` on an INTEGER / REAL / BLOB
     column, which refuses every insert at run time.
   - It accepts `kKey | kVolatile`. A volatile key leaves the key out of the row digest.
   - Nothing keeps `allowed` values ASCII. `schema_json` dumps raw UTF-8 while the test builder's `json.dumps`
     escapes, so the byte identity of "ASCII content" (`make_store.py:9`) is unguarded.
   - **Fix:** in `col()`, refuse `allowed` unless `T` is `Text`, refuse `kKey` with `kVolatile`, and refuse any
     non-ASCII byte in `allowed`.

6. `atx-core/src/db/connection.cpp:37-44, 54` | minor | A bad path can throw, and a helper is unused off Windows.
   - `utf8_path` builds `std::filesystem::path{std::u8string}`. On MSVC STL that throws `std::system_error` for
     invalid UTF-8, so an exception escapes `open_with_policy`, whose contract returns `Result`.
   - `utf8_path` is defined for every platform but used only under `_WIN32`, so a non-Windows `-Werror` build fails
     on `-Wunused-function`.
   - **Fix:** convert with an error-code path (or validate the UTF-8 first) and return `Err(InvalidArgument)`. Move
     the helper inside `#if defined(_WIN32)`.

7. `atx-core/src/db/connection.cpp:47-74` | minor | The network-path refusal does not resolve links.
   - It tests `std::filesystem::absolute(path)`, not the resolved path that sql-design §3.3 names. A junction or
     symlink to a share passes.
   - The Win32 namespace form `\\?\C:\...` is refused as UNC. That is per the brief, but note it.
   - **Fix:** use `weakly_canonical(p, ec)` before the `root_name` and `GetDriveTypeW` tests.

8. `atx-engine/tools/research_store.py:218-229` (also `:158-191`) | minor | `transaction()` and the Python open
   policy.
   - `transaction()` does not roll back when `COMMIT` raises. The connection stays `in_transaction`, and
     `record_store` caches that connection per thread for the process life. Every later `put` on that thread then
     returns False ("a transaction is already open").
   - `Store.open` does not apply sql-design §3.3's UNC / network-path refusal. It also leaves `synchronous` at the
     default, FULL, where the cache policy is NORMAL.
   - **Fix:** wrap `COMMIT`, and `ROLLBACK` if `in_transaction` remains. Optionally refuse `\\` / `//` paths, as the
     C++ policy does.

9. `atx-engine/tools/record_store.py:226-243, 245-267` | minor | A damaged record never heals.
   - `_write_row` publishes a large body before checking for an existing row. A same-key row with other content
     therefore leaves an orphan object.
   - When the existing row's `content_sha256` matches but its body is unreadable, `put` returns True and leaves the
     row as it is. This covers a tampered inline body, a deleted object, and a same-size damaged object, which
     `_publish` keeps because its sizes match. Every later `get` misses: a permanent recompute.
   - **Fix:** check the existing row first. When the content matches but `_row_body` fails, replace the row and
     re-publish the object, unconditionally.

10. `atx-core/src/db/sqlite.cpp:3-5`; `atx-core/include/atx/core/db/sqlite.hpp:58-59` | minor | Two comments still
    say `sqlite.cpp` is the ONLY TU that includes `<sqlite3.h>`. `connection.cpp` now includes it too. | **Fix:**
    update both comments.

11. Test gaps:
    - `atx-engine/tests/research/research_store_table_test.cpp:145-152` | minor | It pins wrapper message text
      ("column 1 has storage class 3"). The contract is only the `table.column:` prefix plus the code.
    - `research_store_digest_test.cpp:146` | minor | `size() == 64` is near-tautological.
    - Behaviours that have no test:
      - `with_immediate`'s retry after BUSY;
      - the `GetDriveTypeW` branch;
      - the concurrent-creator path of `open_store` (re-read under the write lock);
      - migrating a table added at a later version (`opts.since > 1`);
      - `catalog_digest` over two groups.
    - **Fix:** loosen the message assertion to the prefix and the code. Assert the exact catalog line in the
      volatile test. Add the missing tests (the retry can use a second connection holding `BEGIN IMMEDIATE` with a
      short busy timeout).

12. `.superpowers/sdd/platform-p9-20261003/task-SQL1-report.md:287-311` | minor | Root's verification list is too
    narrow.
    - W1 and W3 change behaviour on every connection: extended codes, error text, and the refusal of a trailing
      statement.
    - The pipeline stages reach the wrapper through `atx/engine/store/*`: `stage_discover.cpp` and the other stage
      files. Their gtests `atx-impl/tests/{store_discover,provenance,provenance_digest}_test.cpp` are not in root's
      list.
    - I found no multi-statement `prepare` and no `AlreadyExists` branch on a non-unique constraint among the users.
      The lane did not run the hygiene (PCH-off) check on the atx-core TUs.
    - **Fix:** root also builds and runs those atx-impl gtests. It runs `atx-build.ps1 check -Preset equity-hygiene`
      on `atx-core/src/db/connection.cpp`, `sqlite.cpp` and `db_connection_test.cpp`.

## Deviation assessment (report "Decisions")

- **`store_info` in the cache group, as a volatile table: sound.**
  - Ruling SQL-10 requires every store to carry `store_info('schema_json')`, and §3.4 says every store describes
    itself.
  - Volatile keeps the exe SHA (`created_by`) and the schema text out of the catalog digest, so reproducibility does
    not depend on the executable.
  - It amends K-P9-13 §3.7 (`store_info` is now in two groups and marked volatile). The PM should record that
    amendment. See finding 2 for the precondition it creates.
- **Table-level CHECK on `record` (`TableOpts.check`): sound.**
  - §3.7 demands a table CHECK that the §3.6 grammar could not express.
  - The extension `, CHECK(<expr>)` after `PRIMARY KEY(...)` is additive, and the fixture, gtest and pytest all
    cover it.
- **STRICT DATATYPE -> InvalidArgument: sound.**
  - It is a refused value, the same class as CHECK and NOT NULL. Every other extended code still maps by its primary
    part, and no enumerator is added.
- **NaN and -0.0 refused for REAL (C++ and Python): sound.**
  - `sqlite3_bind_double` turns NaN into NULL, and an integral REAL is stored as INTEGER, so -0.0 would read back as
    +0.0.
  - Refusing beats a silent change of the stored value. The digest still encodes the bits, as §3.5 says.
- **`open_with_policy` does not stamp: sound.**
  - It matches §3.4 (stamp in the creating `BEGIN IMMEDIATE`) better than the brief's test name does. A crash cannot
    leave a stamped, empty store.
  - The contract is written in `connection.hpp:14-18`. `DbConnection.OpenWithPolicyStampsAppIdAndVersion` stamps
    through `stamp_identity`; the name is misleading, but the test is correct.
- **`schema/toy.json`: sound.** It is test-only, it gives Python bool and blob coverage, and it is checked byte for
  byte against C++.
- **`tables_common.hpp`, `research_store_test_support.hpp`: in scope (the brief's directories). Process items:**
  - The lane ran in pool-21, not SQL-7's pool-23. That breaks the letter of SQL-7 but not its intent (a fresh pool).
    No code effect.
  - Task 2 was committed before task 1. SQL2's base `03fc676d` therefore carries the wrapper fixes. No conflict with
    SQL2-BASE.
  - The lane built C++ despite lane rule 1, citing a PM dispatch I have not seen. No code effect.
- **Fixtures transcribed by an uncommitted scratch script: acceptable.**
  - No generator is committed, so SQL-5 holds.
  - The fixtures are checked against two renderers: C++ `group_schema_json` (gtest) and Python `json.dumps`
    (pytest).

## Flag-absent identity and vendored SQLite

**Identity holds.**
- No executable links `atx-engine-research-store`: it is `EXCLUDE_FROM_ALL` with no consumer.
- `atx-impl/` and `scripts/` are untouched.
- No pinned JSON artifact changed. No Python schema generator is committed: `make_store.py`, `make_py_fixture.py`
  and `digest_oracle.py` are test-only and consume the fixtures.
- With no `index.sqlite`, `record_store` runs the base bodies verbatim. `NoIndexTest` pins the bytes, the file set
  and the stderr silence, and the fitter and card store pytests pass unchanged (above).
- The atx-core wrapper changes reach every executable, but only on its db paths. No research executable calls the
  wrapper; the pipeline stages do (finding 12).

**Vendored SQLite (unchanged by the lane).**
- `atx_sqlite3` is a static target. `/w` is PRIVATE to that target only.
- Defines: `SQLITE_THREADSAFE=2`, `DQS=0`, `DEFAULT_FOREIGN_KEYS=1`, FTS5.
- It is linked PRIVATE into atx-core alone. The engine store links only `atx::core`, so there is one SQLite copy per
  process.
- 3.53.2 and the `sqlite3.c` SHA-256 are recorded in `PROVENANCE.md`.
- No dependency line was added.

## Checked

- [x] `.agents/cpp/agent.md` §10 checklist applied to the diff.
  - No UB, no narrowing.
  - `noexcept` rollback path (W6); RAII finalize and rollback on every error path.
  - Bounded loops: `tail_is_blank` consumes at least one byte per iteration; `select_all` / `digest_table` end at
    Done.
  - Exhaustive switches; contracts documented.
  - Hygiene-preset include check not run by the lane (finding 12).
- [x] Wrapper specifics:
  - statement / connection lifetime and moves;
  - the `prepare` tail check and `INT_MAX` guard;
  - bind indices 1..N in declared order;
  - `SQLITE_TRANSIENT` for text and blob;
  - UTF-8 text; i64 / u64 bit-pattern / f64 round trips;
  - busy timeout + `with_immediate` retry; `backup_to` busy test on the primary code;
  - W3 mapping (extended -> primary fallback, no new `ErrorCode`).
- [x] Descriptors and schema:
  - DDL is deterministic: runtime strings, fold order = declared order, STRICT + WITHOUT ROWID on every table;
  - committed fixtures equal the C++ output (gtest) and Python's re-render (pytest);
  - type mismatch is a compile error through `col()` (finding 4 for the bypass);
  - `store_info` and the identity stamps are written inside the creating / migrating `BEGIN IMMEDIATE`.
- [x] Catalog digest:
  - domain line, then groups and tables in the order given, rows `ORDER BY` key (BINARY collation);
  - volatile tables and columns excluded; NULL `~`; reals as IEEE bits; decimal ints via `to_chars`;
  - one read snapshot; no platform-dependent formatting.
- [x] Diff stays inside the brief's files in scope (plus the listed private header, test-support header and toy
  fixture). CMake additions:
  - atx-core: one source line, and one line in the tests list;
  - atx-engine: one library block before `if(ATX_BUILD_TESTS)`, following B1's research-fields idiom;
  - atx-engine tests: one block at the file end.
- [x] Evidence in the report matches its claims where cheap: the pytest counts reproduced (30 / 33 subtests), the
  `atx-impl` / `scripts` diff is empty, and the no-index bodies are identical. The C++ build and gtest claims are
  root's to confirm.
