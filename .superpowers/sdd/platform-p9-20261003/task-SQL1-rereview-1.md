# Lane SQL1 re-review 1 (fix round 1, ruling SQL1-FIX1)

## Verdict

APPROVE

- The three required fixes are real. Each has a test that fails at `4d198fef`.
- The extra minors (4-8, 10, 11 part, 12) are in. One of them, the finding 7 change, adds a narrow regression:
  N1, minor. It needs a one-line fix. I recommend landing that fix before SQL1 reaches main, either folded in by
  root or carried by SQL2. The PM rules.
- Flag-absent identity holds. Every caller is updated. No changed C++ line exceeds 100 columns, and I found no new
  `/W4 /WX` hazard.
- New findings: 0 blocker, 0 major, 1 minor (N1), 5 nits.

## Reviewed

- Diff: `4d198fef..1a5b051d` (`feat/p9-sql1-20261003`, `C:/atx-wt/pool-21`). One commit, which carries the
  Co-Authored-By trailer. It also commits the round-0 review file.
- Code: 12 files, +365/-67.
- Scope is limited to (a)-(e) of the dispatch. No C++ was built or run; the lane's build and gtest claims (0
  warnings, 27/27 store tests, 44 `Db*`) are root's to confirm.

## Evidence

All runs were in `C:/atx-wt/pool-21` with `PYTHONDONTWRITEBYTECODE=1`. `git status --porcelain` was empty before and
after.

```
python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store.py \
  atx-engine/tools/test_research_store_fixtures.py atx-engine/tools/test_record_store_sqlite.py \
  atx-engine/tools/test_record_store.py
31 passed, 33 subtests passed in 2.65s            exit_code=0
PYTHONHASHSEED=1 python -m pytest -v -p no:cacheprovider atx-engine/tools/test_research_store.py
11 passed, 15 subtests passed (incl. test_a_failed_commit_rolls_back)   exit_code=0
PYTHONHASHSEED=1 python -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_weights.py \
  atx-impl/tools/test_alpha_report_card_store.py
101 passed in 92.25s                              exit_code=0
```

The Python tests fail without the fix. I checked this with a scratch script on an in-memory database:
- Python's `sqlite3` defaults `synchronous` to 2 (FULL), so the cache assertion `(1,)` would fail.
- A deferred foreign key that fails at `COMMIT` leaves `in_transaction == True`, so the `assertFalse` would fail.

Static checks:
- I measured the added C++ lines per file: the longest is 100 columns (`research_store_test_support.hpp`). Added
  Python lines are at most 118, inside the files' existing 120-column style.
- `build-equity/build.ninja` compiles `connection.cpp` with `/EHsc ... /W4 /permissive- /WX` and no PCH, so the
  try/catch is legal there.
- `<sqlite3.h>` is included only by `atx-core/src/db/sqlite.cpp:20` and `connection.cpp:18` (vendored sources
  excluded).

## (a) Required fixes

1. **Schema drift at an equal version: real and tested.**
   - `check_no_drift` (`store.cpp:206-229`) compares the stored `store_info('schema_json')` byte for byte with
     `schema_json(kind, groups)`.
   - It runs on the plain open (`:309-311`) and on the concurrent-creator path under the write lock (`:260-262`).
   - Documented at `store.hpp:121-133`.
   - Test: `ResearchStoreOpen.RefusesSchemaDriftAtSameVersion` (`open_test.cpp:291-316`). It covers a descriptor
     edited without a bump, and a group added at a version the store already has.
   - At `4d198fef` both opens succeed, so `ASSERT_FALSE` fails.
   - After the refusal, the store reopens with its own groups and its columns unchanged.
2. **Exactly one `store_info` group and no repeated table name: real, tested, and checked before any file is
   touched.**
   - `store_version` (`store.cpp:39-83`) rejects a repeated table name across groups, then a list with no
     `store_info`. Together these mean exactly one.
   - It is the first call in `open_store` (`:294`), before `require_existing` and `open_with_policy`.
   - Test: `RequiresExactlyOneStoreInfoGroup` (`:320-351`). It refuses three lists (none, twice, a clashing table
     name), then checks `EXPECT_FALSE(exists(path))`.
   - At `4d198fef` the no-`store_info` list opens with create, sets WAL and so writes the file, and only then fails
     on the upsert. The existence check therefore fails.
   - The same test pins `catalog_digest` over two groups.
3. **`open_cache` never creates: real and tested.**
   - `StoreOpen` is a required argument.
   - `Existing` runs `require_existing` (`store.cpp:95-106`): a missing or 0-byte file is NotFound, and the file is
     never opened.
   - It then opens with `ReadWrite`, with no create flag, and guards `opened.created` (`:300-308`).
   - `open_cache` passes `Existing` (`:319-323`). `create_cache` (`:325-329`) is the only `CreateIfMissing` caller
     outside tests.
   - Test: `OpenCacheNeverCreatesAnIndex` (`:355-379`). A missing index is NotFound and no file appears. A 0-byte
     index stays at 0 bytes. `open_store(Existing)` creates no catalog.
   - At `4d198fef`, `open_cache` created the index, so the test fails there.
   - Edge cases: N2 and N3.

## (b) Extra minors: no regression except N1

- **4, `Column`:**
  - `Column` now carries `requires StorableAs<M, T>` (`table.hpp:126-141`).
  - `col()` returns `auto`, so its readable `static_assert` still fires first.
  - `table_test.cpp:29-36` pins the change: `!IntColumnFormable<i32>` would not compile without the constraint.
- **5, `col()` refusals:**
  - `allowed` is refused unless the kind is Text, and so is a non-ASCII `allowed` value (`(unsigned char)c > 0x7F`)
    and `kKey | kVolatile`. All three are `consteval`.
  - The old rule name is gone, with no remaining references.
  - These are negative compile cases, so no in-tree test is possible. The lane reports a reverted probe.
  - Every existing descriptor still compiles, per the lane's build. I did not rebuild.
- **6, path conversion:**
  - The helper is now inside `#if defined(_WIN32)`, so no function is unused off Windows.
  - Invalid UTF-8: MSVC's `path{u8string}` throws `std::system_error`, which the code catches and returns as
    `Err(InvalidArgument)` (`connection.cpp:42-62`).
  - `store.cpp:85-93` `path_from_utf8` does the same.
  - The `weakly_canonical(..., ec)` overload throws only `bad_alloc`.
- **7, link resolution:**
  - Links are now resolved by `weakly_canonical`.
  - On a DOS-name result, MSVC already turns `\\?\UNC\` into `\\` and drops `\\?\` before a drive (`<filesystem>`
    3088-3095). The extra prefix handling is harmless.
  - **Regression for a relative path whose first element does not exist (N1).**
- **8, `research_store.py`:**
  - A failed `COMMIT` now rolls back when `in_transaction` is still set (`:233-238`).
  - UNC paths are refused, both as given and after `resolve()` (`:162-163`). A literal UNC path short-circuits
    before `resolve()`, so the test never touches the network.
  - `synchronous` is FULL for a catalog and NORMAL for a cache (`:191`). It is set after validation and inside the
    close-on-error `try`.
  - Tests: `test_research_store.py:60-64, 172-184, 200`.
- **10 and 11 (part):** both comments are corrected. The table test now asserts the prefix (`rfind(expected, 0) ==
  0`) and the code. The volatile test asserts the exact `catalog_run` digest.
- **12:** report `:300-309` adds root's atx-impl gtests and the hygiene note.

## (c) Flag-absent identity: holds

- `record_store.py` is not touched. It selects SQLite mode by `(root|parent)/index.sqlite`.`is_file()`
  (`record_store.py:99-102`).
- Python's `Store.open` still requires `is_file()` and opens `mode=rw`.
- C++ readers open with `Existing` and never create a file.
- No executable links the store library, and `atx-impl/` and `scripts/` are not touched.
- The no-index consumers (fitter and card store) pass 101/101, and `NoIndexTest` passes.

## (d) Callers: all updated

- In pool-21, `open_store`, `open_cache` and `create_cache` are called only from `store.cpp` and
  `research_store_open_test.cpp`. Every call passes `kCreate` or `kExisting`. No other C++ calls `open_store`.
- Downstream, for information: SQL2 (pool-23) has already merged `1a5b051d` (`afea225d`) and followed the API in
  `da25e5cc`. `catalog.cpp:57` passes `how`, and `store_cli.cpp:452` `cache init` calls `create_cache`.

## (e) `/W4 /WX` and 100 columns: clean

- No added C++ line exceeds 100 columns.
- New comparisons:
  - `(flags & (kKey | kVolatile)) == ...` compares two promoted ints.
  - `static_cast<unsigned char>(c) > 0x7FU` compares a non-negative promoted value with an unsigned constant, which
    clang does not flag.
- `std::u8string{it, it}` cannot pick the initializer_list constructor, because the iterator does not convert to
  `char8_t`.
- No unused functions or variables. The includes added (`<exception>`, `<system_error>`, `<fstream>`, `<span>`)
  match their uses.

## Original findings

| # | Status |
|---|--------|
| 1 schema drift | addressed (required); tested |
| 2 `store_info` precondition | addressed (required); tested; report SQL2 note corrected (`:389-390`, `:443-444`) |
| 3 `open_cache` creates | addressed (required); tested; see N2, N3 |
| 4 `Column` bypass | addressed; pinned by `static_assert` |
| 5 `col()` combinations | addressed; compile-time only (probe reported, not committed) |
| 6 bad path throws / unused helper | addressed |
| 7 links not resolved | addressed for links; **N1** regression on relative paths |
| 8 Python transaction / UNC / synchronous | addressed; tested |
| 9 `record_store` damaged row never heals | open (parked to SQL4/P10 by SQL1-FIX1) |
| 10 stale "only TU" comments | addressed |
| 11 test gaps | part addressed (message prefix, exact digest, two-group `catalog_digest`); open: `with_immediate` BUSY retry, `GetDriveTypeW` branch, concurrent creator, `since > 1` migration (parked) |
| 12 root verification list | addressed in report; root still runs the atx-impl gtests and `equity-hygiene` check |

## New findings

`path:line | severity | problem | required fix`

1. **N1:** `atx-core/src/db/connection.cpp:42-58` | minor (regression vs `4d198fef`) | A relative path whose first
   element does not exist escapes the network-path check.
   - Example: a bare `catalog.sqlite` under `CreateIfMissing`, i.e. the `init` verbs.
   - Cause: MSVC `weakly_canonical` starts from `root_path()`, which is empty for a relative path, and stops
     canonicalising at the first missing element (`<filesystem>` 4146-4173). The result stays relative,
     `root_name()` is empty, and neither the UNC test nor `GetDriveTypeW` runs.
   - The base used `absolute()` and refused this path when the cwd was on a mapped network drive or a UNC share. It
     is now accepted, which puts WAL on a network filesystem.
   - Fix: make the path absolute first, then canonicalise:
     `const auto abs = std::filesystem::absolute(path{u8}, ec); if (ec) -> IoError; weakly_canonical(abs, ec)`.
   - Testing the remote branch still needs a network drive (finding 11, rest). A cheap guard: a relative path in a
     temp cwd still opens.
2. **N2:** `atx-engine/src/research/store/store.cpp:300-308` | nit | `Existing` does not leave every non-store file
   untouched.
   - A non-empty file with no schema, such as a header-only rollback-mode SQLite file, takes `open_with_policy`'s
     "created" branch. That branch sets `page_size` and switches the file to WAL, writing its header, before
     `open_store` returns NotFound.
   - Index presence is unchanged, because the file already existed, so identity holds. But "file left untouched"
     holds only for a missing or 0-byte file.
   - Fix (optional, SQL4/P10): `open_with_policy` returns NotFound for an empty store unless the mode is
     `ReadWriteCreate`, as it already does for `ReadOnly`. Alternatively, document the case in `store.hpp:118-120`.
3. **N3:** `store.cpp:296-298` | nit | Under `Existing`, `require_existing` stats the path before
   `open_with_policy`'s UNC refusal.
   - A UNC path therefore touches the network first.
   - A missing UNC file returns NotFound instead of InvalidArgument (UNC refused).
   - Fix: refuse `\\` and `//` before the existence check, or document the order.
4. **N4:** `store.cpp:210-215`; `connection.cpp:59` | nit | Some errors are relabelled.
   - `check_no_drift` reports every `store_info` read error as InvalidArgument, including Busy after the busy timeout
     and IoError.
   - `resolved_root` / `path_from_utf8` catch `std::exception`, so they would label `bad_alloc` "not valid UTF-8".
   - Fix: propagate `rows.error()` unchanged, and catch `std::system_error` only.
5. **N5:** `atx-engine/include/atx/engine/research/store/store.hpp:113-133` | nit | The same-version check makes the
   group list's completeness and order part of store identity.
   - `schema_json` serialises groups in the order given. A reader that passes a subset or a reordered list of a
     store's groups at the same version is now refused as drift.
   - SQL2 is consistent: it always passes `catalog_groups()`.
   - Fix: say "the store's full group list, in creation order" in the `@param groups` text.
6. **N6:** `store.cpp:68-71`; `research_store_open_test.cpp:313-315` | nit | A message and an assertion are loose.
   - The repeated-table message always cites `store_info`, even for another clash such as `plain`.
   - The second drift case asserts only the code. The list is shown valid at `:336-337`, so the reason is in fact
     drift. Asserting "schema drift" would pin it.
   - Fix: word the message per case, and add the message assertion.
