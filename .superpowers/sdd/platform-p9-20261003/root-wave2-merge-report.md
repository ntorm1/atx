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

### A3 (`a217fd7e` then flip `50cb1571`, pool-12): MERGED (post-flip re-checks NOT run: owner stop)

- **Ledger:** `Task A3: complete` (review APPROVE `56474bbf`, A3-RED confirmed; fix round A3-FIX1; re-review APPROVE
  `a8d2fbd0`). Root items: merge `a217fd7e` first (binding build), S-R1 four comments as a slip, TRAIN identity,
  read `rows_sealed_value_decoded`, N-1 (v15 list must include `ceq_iss_5y`), flip `50cb1571` only after identity.
- **Docs:** `task-A3-report.md`, `task-A3-review.md`, `handoff-A3.md`, `a3-build/*` receipts and
  `a3-red-evidence.*` arrive with the `a217fd7e` merge; `task-A3-rereview-1.md` (lane commit `a8d2fbd0`) copied.
- **Merge 1:** `git merge --no-ff a217fd7e` -> **`ff1637ac`** (merge base `1239a5ff`; 55 files). Two list-tail
  conflicts, both kept (HEAD first, one blank line): `atx-engine/CMakeLists.txt` (SQL2's catalog block, then A3's
  `target_sources(atx-engine-research-fields ...)` four sources) and `atx-engine/tests/CMakeLists.txt` (SQL2's
  catalog-tests block, then A3's three test files). Registry at this merge = pre-flip blob `793a3081` (owner suffix
  only on the six rows; 3 engine rows).
- **Slip S-R1:** **`08b0695d`** comments only: `research_fields_vendor_panel_test.cpp:4` (tradingDate statistics),
  `:767` (every value-derived statistic; `rows_in_file` / `rows_keys_decoded` count the file by design),
  `vendor_panel.hpp:19` (only sealed and off-calendar drops counted), `:22` (decoded with the chunk, released
  unread). Lines <= 100 columns.
- **Build p9-1v** (binding run; source `08b0695d`, dirty 2): first launch refused by the memory gate (free 2,613,
  commit 2,260 < 2,500; a lane's clang-cl was running; no receipt written, tag reused); waited for the gate; then
  `atx-engine-research-fields, atx-engine-research-fields-tests, atx-research-fields` -> **exit 0, 79.9 s, 20 TUs,
  3 links; 0 warning / 0 error lines.** Exes: `atx-research-fields` **`b2c30d47...5543`**,
  `atx-engine-research-fields-tests` `48b3b0b0...5281`.
- **gtests (Debug, p9-1v):** whole target **66 passed / 66** (13 suites); M1a-RED anchors
  `ResearchFieldsWriter.QuantilesPartitionLikeNumpy:ResearchFieldsVolumeMean.*` **7 passed** (both former M1a-RED
  gtests OK: the pair is resolved); A3 filter (VendorPanel / VendorFields / VendorFixture / NyseCalendar / FactorBreak)
  **19 passed**.
- **pytest, pre-flip tree, real exe** (`ATX_RESEARCH_FIELDS_EXE` = p9-1v, each file its own session, `-rs`):
  `test_vendor_engine_path.py` 8p; `test_research_fields_engine_path.py` 11p; `test_vendor_panel_fixture.py` 3p;
  `test_research_fields_fixture.py` 2p; `test_field_registry.py` 35p; `test_no_new_python_builder.py` 4p; all
  exit 0, no skips (= the lane's pre-flip counts). Known-red check: `scripts/tests/test_research_mine.py::
  test_fields_are_the_rule_applied_to_the_registry` still fails (E2's M1a-RED; unchanged by A3).
- **TRAIN identity** (run before the flip merged): registry entry `prepare_research_fields.py --registry <scratch
  copy of the flip blob 5d2954ec> --fields ret_overnight,ret_intraday,ceq_iss_5y,open_adj,high_adj,low_adj --role
  build-equity/train-2020-2023-lo3 --role-sha256 e1c67101... --tickerhistory/--price-source TickerHistory3.parquet
  --max-rss-mib 2048 --max-seconds 580`, no `--reuse`; run A without, run B with `--engine-exe
  build-equity/bin/atx-research-fields.exe` (p9-1v); both through `run_bounded_research.py --seconds 600
  --max-rss-mib 2048 --min-free-mib 512` with binds (builder, registry modules, engine shim, price / ohlc modules, flip
  registry, role manifest; + exe for B). Launcher `a3-train-run.ps1`, comparer `a3_compare.py` (session scratch).
  - **Field list (deviation, for the PM):** v15's 84-name list **includes `ceq_iss_5y` (N-1 holds)**. It was not
    used as-is: v15's argv reuses (`--reuse v15a`), and any reuse prior carries the six Python entries
    (`prepare_research_fields.py:2864` skips only engine-produced priors), so the engine would never run; a
    no-reuse 84-field build is far beyond the host budget. The six were computed in v10 (price three, with
    `ceq_iss_5y`) and v14 (bars, alone). Same history shown empirically: the run's `source_checks.ohlc` equals
    v15's (10/10 paths) and `source_checks.price` equals v15's on all 18 shared paths (v15 adds only the 9
    `/market/*` paths of its market fields).
  - Run A `p9-a3-train-py`: completed, exit 0, **44.7 s, peak tree RSS 542 MiB**, min free 2,892; receipt
    `3fce2e4a...31e6`; manifest `a1c0acf9...12e2`; stderr = the expected "computed by the Python builder" note.
  - Run B `p9-a3-train-engine`: completed, exit 0, **168.2 s, peak tree RSS 950 MiB**, min free 2,622; receipt
    `66bec77d...e73b`; manifest `566daad4...ed3a`; two engine calls (price three, bars three), receipts complete.

  | field | v15 pin | py (entry = files = disk) | engine (entry = files = disk) | py = engine = v15 |
  |---|---|---|---|---|
  | ret_overnight | `1a41d3853685c8de` | same | same | **yes** |
  | ret_intraday | `f6cf5eb3e386fbf6` | same | same | **yes** |
  | ceq_iss_5y | `88301a7bed081cb8` | same | same | **yes** |
  | open_adj | `13e4ba544690018d` | same | same | **yes** |
  | high_adj | `e7d4100c7e5fb787` | same | same | **yes** |
  | low_adj | `e9a3a7b29a7d6f65` | same | same | **yes** |

  Manifest py vs engine: **54 JSON paths differ, all under `fields[0..5].producer`** (Python `module` /
  `code_*` vs engine `kind` / `exe_sha256` / `git_sha` / `build_type` / `receipt_sha256`); **0 outside**.
  Engine producer: `kind engine`, `exe_sha256 b2c30d47...` = p9-1v, `build_type Debug`, `git_sha
  08b0695d...-dirty`. `seal.exclusive_end` 2024-01-01 both. **Identity holds.**
  - **Seal counts (S-1; engine receipts `engine_fields/*.receipt.json`, every field identical):** 262 row groups,
    `row_groups_pruned_sealed 0`, `rows_in_row_groups_pruned_sealed 0`, `row_groups_values_decoded 262`,
    `rows_in_file 32,323,644`, `rows_sealed_dropped 7,592,840`, **`rows_sealed_value_decoded 7,592,840`**. On the
    real file no row group is prunable (each spans the seal), so every sealed row's value chunk is decoded and released
    unread (the A3-FIX1 behaviour); the row-group push-down saves nothing on TickerHistory3. For the PM (A4).
- **Merge 2 (flip):** `git merge --no-ff 50cb1571` -> **`1c90011a`** (2 files: `field_registry.json` -> blob
  `5d2954ec`, nine engine rows; the pin test in `test_vendor_engine_path.py`). No conflict.
- **NOT done (owner stop arrived right after the flip merge):** post-flip registry `check` (`ok 92` expected) and
  the field-registry pytest (`test_field_registry.py` 35p, `test_vendor_engine_path.py` 9p,
  `test_research_fields_engine_path.py` 11p with the real exe); pool-12 DISK-2 deletion. Exact commands in
  `handoff-root-m2a.md`.
- **Trial ledger:** 0 trials (133 lines, `27e40f9f`).

### M2a status at owner stop

Head `1c90011a` + this docs commit. Next free build tag **p9-1w**. Root tree change: `build-equity` reconfigured
with test group `store` added. Known-red now: `test_fields_are_the_rule_applied_to_the_registry` (E2) and M1c-RED x2
Release (C2); the two M1a-RED gtests are green. Resume notes: `handoff-root-m2a.md`.
