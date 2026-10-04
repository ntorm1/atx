# P9 root wave-2 merge report

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Recipe: `root-wave2-merge-brief.md` (per merge = the
wave-1 recipe). One section per step group, appended in order.

## M2a: SQL1, SQL2, A3 (2026-10-03)

Agent: fresh root for step group M2a. Integration head at dispatch: `632060d1` (wave-1 gate met, known-reds only).
Tree clean except `progress.md` (PM's, never committed here) and the owner's untracked png. Python
`"C:/Program Files/Python312/python.exe"`; every pytest with `PYTHONDONTWRITEBYTECODE=1 -q -p no:cacheprovider`,
explicit paths only (PY-HYG), `--basetemp` in the session scratchpad (deleted after each run). Gtest and pytest logs
`gt-*.log` / `pyt-*.log` in the session scratchpad.

### SQL1 (`1a5b051d`, pool-21): MERGED

- **Ledger:** `Task SQL1: complete` (review APPROVE at `4d198fef`, re-review APPROVE at `1a5b051d`); ruling SQL1-N1
  (root applies the N1 one-line fix at merge, existing network-path test extended); root items from the ledger: the
  atx-impl gtests `store_discover` / `provenance` / `provenance_digest` and the equity-hygiene include check on
  `connection.cpp` / `sqlite.cpp`.
- **Docs:** `task-SQL1-report.md` and `task-SQL1-review.md` are committed on the lane branch and arrive with the
  merge; `task-SQL1-rereview-1.md` was uncommitted in pool-21 and is copied here (docs commit).
- **Merge:** `git merge --no-ff 1a5b051d` -> **`d31c5367`** (merge base `20443022`; 47 files, +7,538 / -24). One
  list-tail conflict, `atx-engine/tests/CMakeLists.txt`: B1's `atx-engine-research-admission-tests` block (HEAD) and
  SQL1's `atx-engine-research-store-tests` block; both kept, HEAD's first, one blank separator line; the diff against
  HEAD is exactly SQL1's block. `atx-engine/CMakeLists.txt` auto-merged. `git diff --stat 20443022 1a5b051d -- atx-impl
  scripts` is empty (flag-absent: no research executable links the store library).
- **SQL1-N1 fix:** **`8d4cc52c`** `fix(db): make a store path absolute before the network checks (SQL1-N1)`.
  `atx-core/src/db/connection.cpp` `resolved_root`: `std::filesystem::absolute(path, ec)` first (an `ec` is
  `Err(IoError)`), then `weakly_canonical(abs, ec)`; the comment states why (a relative path whose first element does
  not exist stays relative under `weakly_canonical`, its empty root name skipped the UNC and `GetDriveTypeW` checks).
  Test: `DbConnection.RefusesUncPath` extended (the re-review's "cheap guard"): from a local temp cwd, a bare relative
  file name under `ReadWriteCreate` opens, is `created`, and lands under the cwd (cwd restored before the assertions).
  The remote branch itself still needs a network drive to test (finding 11 rest, parked). Added lines <= 100 columns;
  the `connection.cpp` hunk is clang-format clean, the test hunk takes clang-format's layout.
- **Build p9-1s: FAILED, no compile** (`ninja: error: unknown target 'atx-engine-store-tests'`, 27.9 s, 0 TUs). Cause:
  pool-2's equity-dev tree was configured in wave 1 with `ATX_TEST_GROUPS=alpha;factory;learn;data;eval;combine;risk;
  book;library` (no `store`), while pool-21 used `all`. Fix: reconfigured through the wrapper with the same list plus
  `store` (`atx-build.ps1 configure -Preset equity-dev -Groups "...;library;store"`, dry run first: `cmake --preset
  equity-dev -DATX_TEST_GROUPS=...`; no other cache variable changes). Adding a group only adds the
  `atx-engine-store-tests` target.
- **Build p9-1t** (equity-dev, source `8d4cc52c`, dirty 2 = `progress.md` + png): targets SQL1's
  `atx-core-tests, atx-engine-research-store, atx-engine-research-store-tests` + the existing wrapper users
  `atx-engine-store-tests, atx-engine-library-tests` + `atx-impl-tests` (ledger root item) -> **exit 0, 331.9 s,
  82 TUs, 10 links, 4 jobs, free 3,689 MiB; 0 warning lines, 0 error lines** in the build log. Executables:
  `atx-core-tests` `297cafd7...`, `atx-engine-research-store-tests` `6d7d6791...`, `atx-engine-store-tests`
  `3cb297b8...`, `atx-engine-library-tests` `b7258ba9...`, `atx-impl-tests` `1ee5bf6d...`.
  Store TU compile times (`.ninja_log`, -j4, under load; the lane's uncached single-job figures were ~9 / ~5 s net):
  `core_ops.cpp` 9.5 s, `cache_ops.cpp` 7.9 s, `store.cpp` 8.3 s, `digest.cpp` 3.6 s (below the SQL-5 60 s bound).
- **gtests (Debug, p9-1t):**

  | binary | filter | result | exit | wall |
  |---|---|---|---|---|
  | `atx-core-tests` | `Db*` | **44 passed** (23 existing + 15 new `DbSqlite` / `DbConnection` + 6 `Dbn*`) | 0 | 7.3 s |
  | `atx-engine-research-store-tests` | `ResearchStore*` | **27 passed** (24 + 3 fix round 1) | 0 | 0.6 s |
  | `atx-engine-store-tests` | whole | **40 passed** | 0 | 0.3 s |
  | `atx-engine-library-tests` | whole | **66 passed**, 2 DISABLED (pre-existing) | 0 | 6.8 s |
  | `atx-impl-tests` | `AtxImplStoreDiscover.*:AtxImplProvenance.*:AtxImplProvenanceDigest.*` | **23 passed** | 0 | 27.3 s |

- **Hygiene (PCH-off) check:** no `equity-hygiene` tree exists in pool-2 (a cold configure + deps fetch); the check
  is satisfied in equity-dev itself: `build-equity/build.ninja` FLAGS for `atx-core` `sqlite.cpp` / `connection.cpp`,
  `atx-core-tests` `db_sqlite_test.cpp` / `db_connection_test.cpp`, the four `research/store/*.cpp` and the four
  `research_store_*_test.cpp` carry no `cmake_pch` / `/Yu` / `/FI` (control: `atx-engine` `factory.cpp` does). These
  TUs therefore compile from their own includes; p9-1t compiled them.
- **pytest:**

  | command | result | exit | wall |
  |---|---|---|---|
  | `PYTHONHASHSEED=0 -m pytest atx-engine/tools/test_research_store_fixtures.py test_research_store.py test_record_store_sqlite.py test_record_store.py` | 31 passed, 33 subtests | 0 | 3 s |
  | same, `PYTHONHASHSEED=1` | 31 passed, 33 subtests | 0 | 3 s |
  | `PYTHONHASHSEED=0 -m pytest atx-impl/tools/test_fit_composition_weights.py test_alpha_report_card.py test_alpha_report_card_store.py test_fit_composition_weights_store.py test_fit_composition_weights_pool.py` (record-store consumers, no index) | 166 passed (lane: 116 + 50) | 0 | 120 s |

- **Slips fixed:** none beyond the ruled N1 fix. The p9-1s failure was a root tree configuration gap, not lane code.
- **Not run here (out of M2a scope, listed for the PM):** the full `atx-engine/tools` / `atx-impl/tools` suites (wave-2
  gate after the last merge), the compile-time `check` with ccache disabled, SQL1's opt-in X-5 identity (moves to
  SQL2's real-tree items).
- **DISK-2(b):** pool-21's `build-equity` object tree deleted (`atx-core/`, `atx-engine/`, `atx-impl/`, `atx-tsdb/`,
  `bin/`, `CMakeFiles/`, `lib/`, `tests/`, `build.ninja`, `.ninja_*`, `CMakeCache.txt`, `compile_commands.json`,
  `*.cmake`, `spdlog.pc`; ~1.75 GB). Kept: `mega-p9-sql1-{a,b,c,d}-*` receipts and logs, `vcpkg-manifest-install.log`,
  the git-tracked `audits/` and `v8-interim3-pitch-render-run2/`. pool-21 `git status` clean.
- **Trial ledger:** 0 trials.

### SQL2 (`cfe2de87`, pool-23): MERGED

- **Ledger:** `Task SQL2: complete` (review APPROVE at `e835b434`; fix round SQL2-FIX1; re-review APPROVE at
  `cfe2de87`, rereview commit `33367944`). Root at merge: rebuild from the committed head (the lane's p9-sql2-d
  receipt was built from the uncommitted fix tree), 27 gtests + 43 pytests + fixture digest `d32f7655`; then the
  SQL2-CLS guard on the merged tree. Ruling SQL2-S7 (root-path seal check narrowing) and N3 are SQL3 carry items.
- **Docs:** `task-SQL2-report.md`, `task-SQL2-review.md` and `handoff-SQL2.md` are committed on the lane branch at
  `cfe2de87` and arrive with the merge; `task-SQL2-rereview-1.md` (lane commit `33367944`, after the merge SHA)
  is copied here (docs commit).
- **Merge:** `git merge --no-ff cfe2de87` -> **`1300fd6a`** (merge bases `1a5b051d` and `1239a5ff`; 77 files,
  +11,376 / -0 against HEAD). One conflict, `atx-engine/tests/CMakeLists.txt`: HEAD's side empty after SQL1's
  block, SQL2's `atx-engine-research-catalog-tests` block taken. `atx-engine/CMakeLists.txt` auto-merged (SQL2's
  `atx-engine-research-catalog` library + `atx-research-store` exe block at the tail, both `EXCLUDE_FROM_ALL`, no
  PCH). Every file of the merge is SQL2-owned (catalog headers / sources / tests / fixtures, `classes.json`,
  `research_store_identity.py` + three test files, three docs) except the two CMake tails. `git diff --stat
  1239a5ff cfe2de87 -- atx-impl scripts` is empty (flag-absent).
- **Build p9-1u** (equity-dev, source `1300fd6a`, dirty 2 = `progress.md` + png): SQL2's
  `atx-engine-research-catalog, atx-engine-research-catalog-tests, atx-research-store` + SQL1's three -> **exit 0,
  91.4 s, 21 TUs, 3 links, 4 jobs, free 3,438 MiB; 0 warning lines, 0 error lines.** Executables
  `atx-research-store` **`3b955ba1...9757`**, `atx-engine-research-catalog-tests` `8923c962...63e3`; SQL1's
  `atx-core-tests` / `atx-engine-research-store-tests` unchanged (same SHA as p9-1t, not rebuilt). Catalog TU
  compile times (`.ninja_log`, -j4): `records_ops.cpp` **23.9 s** (the only TU that instantiates the descriptor
  templates; review R2), `json_text.cpp` 13.4 s, `catalog.cpp` 12.6 s, the others 6.0-11.1 s.
- **Hygiene (PCH-off):** the 21 catalog / catalog-test TUs carry no PCH flag in `build-equity/build.ninja` (no PCH
  entry in the CMake block), so their own includes are what p9-1u compiled.
- **gtests (Debug, p9-1u):** `atx-engine-research-catalog-tests --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*`
  **27 passed** (exit 0, 10.0 s; whole binary also 27); SQL1's again: `ResearchStore*` 27 passed, `Db*` 44 passed.
  The gtest temp root `%TEMP%/atx_research_catalog_tests` is absent afterwards.
- **SQL2-CLS (classes guard):** on the merged head `1300fd6a` the guard
  `test_every_schema_literal_is_registered` failed on exactly one literal: `atx.nav-rules/v1`
  (`atx-impl/src/strategy_nav_replay.{hpp,cpp}`, C1's `--list-rules` contract; 1 failed / 4 passed, 90 subtests).
  Commit **`1892a183`**: the literal joins the `nav-output` class (its writer's class; precedent
  `atx.composition-rules/v1` in `fit-output`). Nothing else missing.
- **pytest** (on `1892a183`, after `atx-research-store` was built; `-rs`: no skips):

  | command | result | exit |
  |---|---|---|
  | `PYTHONHASHSEED=0 -m pytest atx-engine/tools/test_research_store_{classes,blind,identity,fixtures}.py test_research_store.py` | **43 passed**, 129 subtests | 0 |
  | same, `PYTHONHASHSEED=1` | **43 passed**, 129 subtests | 0 |
  | `-k FixtureChain test_research_store_identity.py` | 1 passed (ran, not skipped) | 0 |

- **Fixture chain** (`atx-research-store` p9-1u, scratch catalog, run twice: relative and absolute `--root`):
  `init` 0; `catalog` 0, `files_seen 28 verified 25 declared 0 skipped 4`, `catalog_digest
  d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69`; `digest` 0, same value; `verify --pins` 0;
  `research_store_identity.py` **total checked 12 mismatches 0**, exit 0. Both spellings identical. **Digest = the
  expected `d32f7655...4e69`** (unchanged by the SQL2-CLS row, as predicted: the registry is not hashed and no fixture
  carries `atx.nav-rules/v1`). Recorded here as the canary golden (review R3).
- **Slips fixed:** none.
- **Not run (outside M2a; for the PM):** the real-tree bounded catalog run (review R4: `--root .`, `skipped_path`
  counts by reason, duplicate id / tag check) and SQL1's opt-in X-5 identity run (`cache init`, cold and warm, then
  delete the index); the full `atx-engine/tools` suite (wave-2 gate).
- **DISK-2:** pool-23's object tree was already deleted by the lane (only receipts, logs and the git-tracked
  `audits/` / `v8-interim3-pitch-render-run2/` remain); nothing deleted by me.
- **Trial ledger:** 0 trials (133 lines, `27e40f9f`).
