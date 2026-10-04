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

### A3 (`a217fd7e` then flip `50cb1571`, pool-12): MERGED (post-flip re-checks green in M2b)

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
- **Post-flip re-checks (run in M2b, 2026-10-04, head `56060f84` = `c7ef4e3d` + HYG-1; logs `m2b-logs/a3-*`):**
  - Registry check from `atx-engine/tools`, fresh interpreter, repository window (no conftest bind):
    `d = fr.load(); fr.bind(vars(b), d); fr.check(vars(b), d); assert fr.dump(fr.regenerate(vars(b), d)) == file`
    -> **`ok 92`**, round-trip holds, **9 engine rows** (3 wave-1 + 6 A3). File sha256 `ffbce95d...67fb` (200,436 B),
    blob `5d2954ec` = the flip blob. Note: the handoff's literal command passed `fr.python_modules(d)` (module
    *names*) to `bind_modules`, which takes module objects; `fr.bind(ns, doc)` is the wave-1 recipe and imports them.
  - pytest, real exe (`ATX_RESEARCH_FIELDS_EXE` = p9-1v `b2c30d47...5543`), each file its own session, `-q -rs -p
    no:cacheprovider`, `PYTHONDONTWRITEBYTECODE=1`: `atx-engine/tools/test_field_registry.py` **35 passed**;
    `atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py` **9 passed**;
    `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` **11 passed**; no skips.
    (Slip, mine: the first launch of the last two errored at fixture setup because the `--basetemp` parent did not
    exist (`WinError 3` in `pathlib.mkdir`, 0 tests run); created the parent and re-ran; counts above are the re-run.)
- **DISK-2 (pool-12):** nothing to delete: pool-12's `build-equity` object tree was already gone (2.6 MB left = the
  keep-list exactly: `mega-p9-a3-{a..e}-*` receipts / build / cache-stats logs, `a3-fix1-configure.log`, git-tracked
  `audits/`, `v8-interim3-pitch-render-run2/`). pool-12's `build/` (580 KB lease-configure stub) not touched.
  The TRAIN identity outputs `build-equity/p9-a3-train-*` in pool-2 are kept (owner question pending, RESUME 4).
- **Trial ledger:** 0 trials (133 lines, `27e40f9f`).

### M2a status at owner stop

Head `1c90011a` + this docs commit. Next free build tag **p9-1w**. Root tree change: `build-equity` reconfigured
with test group `store` added. Known-red now: `test_fields_are_the_rule_applied_to_the_registry` (E2) and M1c-RED x2
Release (C2); the two M1a-RED gtests are green. Resume notes: `handoff-root-m2a.md`.

## M2b: HYG-1, A3 post-flip checks, AL-SIG (2026-10-04)

Agent: fresh root for step group M2b (dispatch R4-1). Head at dispatch `c7ef4e3d`. Python
`"C:/Program Files/Python312/python.exe"`; every pytest `-B -q -rs -p no:cacheprovider`, `PYTHONDONTWRITEBYTECODE=1`,
explicit paths, own `--basetemp` in the session scratchpad. Evidence logs: `m2b-logs/` (this dir), including a copy of
the p9-1w receipt.

### HYG-1 (Ruling HYG-1)

- Unstaged the 609 staged `wq-explore/.mypy_cache` paths (`git restore --staged`; never committed) and deleted both
  cache dirs by name (`wq-explore/.mypy_cache`, `tools/.mypy_cache`).
- Finding: `tools/.mypy_cache` was not only on disk but **committed** (129 files, added by the PM session-2 handoff
  commit `749f9ac2`). Deleting it left 129 tracked deletions, so the deletion is recorded in its own commit
  **`56060f84`** `chore(sdd): drop the committed tools/.mypy_cache from the sprint dir (HYG-1)`. No `.mypy_cache`
  path is tracked after it.
- Index clean afterwards except what I staged myself; the four untracked PNGs under `docs/plans/` left alone.

### A3 post-flip checks

Result written into the A3 section above (M2a): registry `ok 92` + round-trip (9 engine rows, blob `5d2954ec`);
real-exe pytest `test_field_registry.py` 35p, `test_vendor_engine_path.py` 9p, `test_research_fields_engine_path.py`
11p. DISK-2 pool-12: object tree already gone, keep-list intact, 0 B freed. `build-equity/p9-a3-train-*` in pool-2
kept (owner question pending).

### AL-SIG (`ebb081ab`, pool-13): MERGED

- **Ledger:** `Task AL-SIG: complete` at `ebb081ab` (review CHANGES_REQUIRED `1c649035`; fix round 1; re-review
  APPROVE `3f9980d0`; fix round 2 = N1-N4 + ALSIG-7 / -8 with no third re-review, Ruling ALSIG-9: root re-proves).
  Rulings applied: ALSIG-FLIP (merge after A3: done, A3 incl. flip is in), ALSIG-2 / -4 / -5 / -6 / -7 / -8 (lane
  side; K1 `--plan-only` withdrawn, not run), W1-MIN M-2 (seam check below), SQL2-CLS.
- **Docs:** `task-AL-SIG-report.md`, `task-AL-SIG-review.md`, `task-AL-SIG-rereview-1.md`, `handoff-AL-SIG.md` are
  committed on the lane branch and arrived with the merge (blobs = `ebb081ab`'s; nothing to copy).
- **Merge:** `git merge --no-ff ebb081ab` -> **`b1406e14`** (merge base `1239a5ff`; 66 files, +6,095 / -26 against
  the pre-merge head). Seven conflicts, all on the A2 shared tail that A3 and AL-SIG both extended; every one
  resolved by keeping both sides, A3 (HEAD) first:
  - `atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt`: both `target_sources` blocks (A3's vendor panel,
    then AL-SIG's stage readers / kinds; one blank line between).
  - `build_spec.hpp`: `price_source`, then `thirteenf` / `indexes` (`StageRequest` auto-merged).
  - `registry.hpp`: the kind-list doc comment merged into one sentence (vendor kinds, then AL-SIG's); both includes
    (`sources/stage.hpp`, `sources/vendor_panel.hpp`); `SharedSources` = `finra_schedule`, `vendor_panel`,
    `thirteenf`, `indexes`.
  - `registry.cpp`: both include sets; kind table **9 + 2 = 11** (si_shares, si_dtc, vol_126, the six vendor kinds,
    gia_13f, russell_recon). `engine_field_names()` returns `builder_kind_ids()`, so the v1 name list follows.
  - `research_fields_cli.cpp`: `is_known_key` accepts `price_source` and `thirteenf` / `indexes`; AL-SIG's
    `sha256_pin` / `pinned_dir` refactor of `parse_reuse` kept; all four `check_stage_pins` call sites present.
  - `research_fields_registry_test.cpp`: `KindLookup` expects the 11 ids in table order; A3's `spec_of` helper now
    also sets both stage requests (it already set `price_source`), so every kind plans.
  - `field_registry.json` auto-merged. Round-trip check from `atx-engine/tools` (fresh interpreter, repository
    window): **`ok 94`**, `dump(regenerate(file)) == file`, 11 engine rows (A2's 3 + A3's 6 + gia_13f +
    russell_recon), `russell_recon` `point_in_time` false with 1 `non_pit_aspects` entry, `gia_13f` true with none;
    sha256 `d9e0d802...7758` (207,464 B).
- **Build p9-1w** (equity-dev; source `b1406e14`, dirty 7 = three sprint-dir docs + four untracked PNGs, no code):
  `atx-engine-research-fields-tests, atx-research-fields` (exactly the lane report's targets) -> **exit 0, 124.0 s,
  24 TUs, 3 links, 3 jobs, free 3,362 MiB; 0 warning / 0 error lines** (log UTF-16, scanned whole). Exes:
  `atx-research-fields` **`84390f36...417e`**, `atx-engine-research-fields-tests` `183bde0f...d0b4`.
- **gtests (Debug, p9-1w):** `--gtest_filter=ResearchFieldsAlSig*:ResearchFieldsRegistry*` **33 passed** (= the lane's
  33); the whole target **92 / 92 passed, 17 suites** (A3's 66 + AL-SIG's 26; Fixture, Cli, Manifest included); the
  former M1a-RED pair `ResearchFieldsWriter.QuantilesPartitionLikeNumpy` and `ResearchFieldsVolumeMean.SumOrderIsNumpys`
  **both OK** (the lane's 70/72 target red is gone with A3 in).
- **pytest** (each line its own session):

  | command | result | exit |
  |---|---|---|
  | `atx-engine/tools/test_field_registry.py test_no_new_python_builder.py` | **39 passed** | 0 |
  | `scripts/tests/test_p9_al_sig_registrations.py scripts/tests/test_wave_queue.py` | **16 passed** | 0 |
  | `ATX_RESEARCH_FIELDS_EXE`=p9-1w, `fixtures/research_fields/test_research_fields_engine_path.py` | **11 passed**, no skip | 0 |
  | same exe, `fixtures/research_fields/test_vendor_engine_path.py` (A3's registry pin; shared registry file) | **9 passed** | 0 |
  | `scripts/research_cycle.py candidates validate --dir scripts/specs/p9/candidates` | `candidates: 1 valid` | 0 |

- **ALSIG-9, admission refusal read** (`atx-impl/src/strategy_ic_admission.cpp`; AL-SIG does not touch it; last
  changed by D1 `b1764d22`):
  - `:297-315` `non_pit_aspects(row, where)`: an entry without a boolean `point_in_time` and an array
    `non_pit_aspects`, with an empty or non-string aspect, or with `point_in_time == !aspects.empty()`, refuses the
    whole fields manifest (`lacks point_in_time/non_pit_aspects flags`, `malformed`, `contradicts`). It runs for every
    manifest row (`:389`) before the row joins `available`.
  - `:398-406`: for every field a library declares (`lib.declared_extra`; referenced fields are a subset), a non-empty
    `non_pit_aspects` returns `InvalidArgument` "library field '<name>' is not point-in-time in the pinned <role>
    fields manifest (non_pit_aspects: ...); refusing look-ahead". No flag or override bypasses it.
  - Chain to `russell_recon`: the engine manifest copies the spec's flag and aspect into the entry and lists the field
    under `non_point_in_time_fields` (pinned by `ResearchFieldsAlSigFixture.BuildMatchesTheOracle`, passed above), so
    a library declaring `russell_recon` is refused at admission. Queue side: `rank(russell_recon)` is `dropped`
    (final), pinned by `test_non_point_in_time_field_feeds_no_trial` (passed above).
  - Supplementary: the existing `atx-impl-strategy-ic-tests.exe` (built p9-1p from `58fab573`; admission source
    unchanged since, only a test scratch path moved in M1d-RED-2) passes
    `StrategyIcRunner.NonPointInTimeFieldsAndUnflaggedManifestsAreRefused` (1/1), which asserts both refusal texts. Not
    rebuilt (not a lane target).
- **SQL2-CLS** (commit **`6673f8d1`**, `classes.json` only):
  - The guard `test_every_schema_literal_is_registered` on `b1406e14` failed on exactly one literal, and it was
    **not** the one the ledger names: `atx.alpha-panel.indexes/v1` (`russell_proxy.hpp`, the atx-db Russell-proxy
    stage schema the reader checks). The guard scans only Python / C++ code, so `atx.p9-blocked-registration/v1`
    (JSON specs and a test file only) was never flagged.
  - Added both: `atx.alpha-panel.indexes/v1` as a dated `legacy_allow` row, same form, reason and expiry (2027-03-31)
    as its already-listed sibling `atx.alpha-panel.thirteenf/v1` (read, never written by the engine);
    `atx.p9-blocked-registration/v1` as its own class row `blocked-registration` (label I4, after `wave-candidate`;
    glob `**/blocked/*.json`, `spec_doc` kind `candidate`, render null, writer human, pinned by git). Why a row of its
    own: `wave-candidate`'s glob `**/candidates/*.json` never matches `blocked/`; its typed `candidate` ingest would
    put blocked registrations into the wave queue's table (whose statuses cannot be `blocked_on_data`); its render rule
    would claim byte identity for hand-written files. **For the PM:** this is a class choice, not only a literal; if
    the PM wants another home it is a one-line move (no fixture carries the literal).
  - After: guard green; store suite `test_research_store_{classes,blind,identity,fixtures}.py test_research_store.py`
    **43 passed, 131 subtests** under `PYTHONHASHSEED` 0 and 1 (M2a: 129; +1 subtest each for the new class row and
    legacy row); `-k FixtureChain` 1 passed (ran).
  - **Fixture chain** (`atx-research-store` p9-1u `3b955ba1...`, catalog sources unchanged since; scratch catalogs;
    relative and absolute `--root`): `init` 0; `catalog` 0, `files_seen 28 verified 25 declared 0 skipped 4`,
    **`catalog_digest d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69`** (= expected); `digest` 0,
    same value; `verify --pins` 0; `research_store_identity.py` **total checked 12 mismatches 0**, exit 0. Both
    spellings identical.
- **W1-MIN M-2 (theme-table seam):** holds at this merge. AL-SIG touches no `atx-impl` file and not `registry.json`;
  the registry's `themes` equal the 13-name built-in prefix (`strategy_ic_rules.hpp:43`) in order; its three
  registrations use existing themes (`russell_recon` ownership_flow, `hf_crowd` ownership_flow, `lazy_prices`
  filing_events). The two pytests that pin the 13 themes (`atx-impl/tools/test_composition_resid.py`,
  `test_fit_composition_rule_plugins.py`) **31 passed, 17 subtests**. The seam stays open for AL-COMB.
- **ALSIG-3 (carried, not a merge item):** `prepare_research_fields_engine.ENGINE_FIELDS` does not route the two
  engine-only rows; `russell_recon` / `gia_13f` build only through `atx-research-fields build --registry` with a v2
  spec carrying `indexes` / `thirteenf`.
- **DISK-2 (pool-13):** nothing to delete: the lane had already deleted its object tree; `build-equity` holds only
  `mega-p9-alsig-{a..d}-*` receipts / logs, `audits/`, `v8-interim3-pitch-render-run2/` (2.6 MB). `build/` (580 KB
  lease stub) not touched.
- **Slips:** none in lane code. Mine: the first A3 post-flip pytest launch had a missing `--basetemp` parent
  (fixture-setup error, 0 tests run; re-run green).
- **Trial ledger:** 0 trials (133 lines, `27e40f9f`), checked before the merge and at the end.

### M2b status

Head `6673f8d1` + the docs commit that adds this section. Next free build tag **p9-1x**. Known-red now: pytest
`scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2) and Release
`BookNormalScore.*` x2 (C2); no new red. Resume notes: `handoff-root-m2b.md`.

## M2c: OD-A3-6F deletion, S2 merge (2026-10-04)

Agent: fresh root for step group M2c (dispatch after R4-1). Head at dispatch `4497784b`. Python
`"C:/Program Files/Python312/python.exe"`; every pytest `-B -q -rs -p no:cacheprovider`, `PYTHONDONTWRITEBYTECODE=1`,
explicit paths, own `--basetemp` in the session scratchpad. Evidence: `m2c-logs/` (this dir): gtest logs, receipts,
exe SHA-256 lists (`exe-sha256-{debug,release}.txt`, so every log names the binary it ran), comparison outputs and the
four scratch scripts used (`m2c-logs/tools/`: `digests.py`, `pathlen.py`, `cmp.py`, `rerun.py`).

**Status: BLOCKED on one item** (a Release red outside the known-red set, below). Every other item in the step group
is done. Nothing is half-merged and no process of mine is running.

### OD-A3-6F (owner decision): A3 TRAIN identity outputs deleted

- Checked first: `git grep p9-a3-train` over the tree finds only sprint docs (`handoff-root-m2a.md`,
  `handoff-root-m2b.md`, this report). `rg` over every `*.json/yaml/toml/jsonl/py/ps1` outside `build-equity*` finds
  nothing. Inside `build-equity/`, only the five dirs' own start, receipt and spec files name them. No spec, pin, test
  or fixture reads them.
- Deleted by name in `build-equity/`: `p9-a3-train-py` (381 MiB, 7 files), `p9-a3-train-py-run` (4 files),
  `p9-a3-train-engine` (381 MiB, 11 files), `p9-a3-train-engine-run` (4 files), `p9-a3-train-identity` (196 KiB,
  1 file). About 762 MiB freed.

### S2 (`2ed2fc18`, pool-18): MERGED; re-proved except one Release red (BLOCKED)

- **Ledger:** `Task S2: complete` at `2ed2fc18`. Review APPROVE `98c85cc9`; fix round 1 `110fd735` / `80d81b26`;
  re-review APPROVE `8314fb59`. Rulings applied: S2-1 (digests), S2-2 (capabilities), S2-3, S2-4 / W1-I1 (path), S2-5
  (long lines stay), S2-6 (stale imputed entries), SQL2-CLS, DISK-2, M1d-NOTE-3.
- **Docs:** `task-S2-report.md`, `task-S2-review.md`, `handoff-S2.md` and `s2-evidence/` arrived with the merge. Their
  blobs are the lane's; the working copies differ from pool-18's only in CRLF. `task-S2-rereview-1.md` was committed on
  the lane after `2ed2fc18` (at `8314fb59`); it is copied in here and added with `-f`.
- **Merge:** `git merge --no-ff 2ed2fc18` -> **`f6444417`** (merge base `1239a5ff`; 31 files). **No conflict.** The
  shared tails the review expected (`strategy_ic_rules.cpp`, `strategy_ic_runner.cpp`, `composition_rules_list.json`,
  `strategy_ic_admission.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_runner_test.cpp`) auto-merged. D2 and AL-COMB
  are not merged yet, so their overlap is still to come.
- **Digests (S2-1, step 3):** recomputed from git blobs with the tripwire recipe (`strategy_ic_runner_test.cpp:2230-2256`:
  per listed path `path\n<len>\n<text>`, CRLF -> LF, SHA-256). Output in `m2c-logs/source-digests.txt`:

  | tree | `dsl_vm_sources` | `ic_result_sources` |
  |---|---|---|
  | base `1239a5ff` | 34 files, `fcf8021e` = pin | 11, `2d758bff` = pin |
  | lane `2ed2fc18` | 35, `5bcd55c7` = pin | 11, `dd3c6011` = pin |
  | root before merge `4497784b` | 34, `fcf8021e` = pin | 11, `2d758bff` = pin |
  | merged `f6444417` (HEAD and working tree) | 35, **`5bcd55c7`** = pin | 11, **`dd3c6011`** = pin |

  No other merged lane touched a digested source: for both lists, the merged tree's digest equals the lane's. Both
  tripwire gtests are green in Debug and Release.
- **S2-2 (`exe_capabilities`):** append-only.
  - `strategy_ic_rules.cpp` goes from 5 to 8 entries, still sorted. The five existing ones (`eval-mode-audit-exact`,
    `list-rules`, `marginal`, `no-composition`, `theme-registry`) are unchanged.
  - Added: `ic-hac-rule`, `label-terminal`, `min-coverage`. Array size 8 = entry count.
  - The K-P9-6 fixture `composition_rules_list.json` gains exactly the same three lines. No other file pins the list
    (`git grep`).
- **Builds** (all through `scripts/research-build.ps1`). The targets are exactly the lane report's: `atx-engine-alpha-tests,
  atx-engine-factory-tests, atx-impl-strategy-ic-tests, atx-impl-strategy-mine-tests, atx-equity-strategy-ic,
  atx-equity-strategy-targets`.

  | tag | preset | source / dirty | result | warnings | canary |
  |---|---|---|---|---|---|
  | **p9-1x** | equity-dev | `f6444417` / 5 (docs, untracked PNGs) | exit 0, 1,142.9 s, 203 TUs, 8 links | **0** (log scanned whole; the one `NativeCommandError` line is CMake's `GLOB mismatch!` re-glob notice on stderr for the new `alpha_op_traits_test.cpp`) | check: digests equal, but **failed the 15 s first-run wall limit** (23.2 s; host at 100 % CPU from other sessions). Re-check alone: **15 passed in 12.29 s** |
  | **p9-1y** | equity-rel | `f6444417` / 5 | exit 0, 1,048.4 s, 216 TUs, 9 links | **0** (same re-glob notice only) | check: **15 passed in 10.93 s** (5 Release digests) |
  | **p9-1z** | equity-rel | `f6444417` / 5 | `atx-shm-worker` only (test prerequisite; slip below), exit 0, 17.0 s, 1 TU | 0 | - |

- **gtests.** Logs are `m2c-logs/{debug,release}-*.log`. The anchored S2 / fix-round filter is
  `IcScreen.*:IcLabels.*:IcLabelTerminal.*:StrategyIcRunner.AuditExactEvalModeIsRecordedAndCachedUnderItsOwnIdentity:StrategyIcRunner.*SourcesPinnedToSemanticsVersion`.
  It covers the re-review's Suggested 1 plus every test the fix round `110fd735..2ed2fc18` added or extended.

  | binary | Debug (p9-1x) | Release (p9-1y) | lane |
  |---|---|---|---|
  | ic-tests, S2 filter | **28 / 28** (incl. new `IcLabels.TerminalImputedPairsPassBothReturnGuards`, `IcLabelTerminal.AbsurdTerminalReturnIsGuardedOnTheLabelRole`; extended `LabelRoleRefusals`, `FrozenTrainResumeBindsTheLabelRolePins`, `MissingExitIcIsCachedUnderItsOwnScope`, `AuditExact...OwnIdentity`; both source pins) | **28 / 28** | - |
  | `atx-impl-strategy-ic-tests` whole | **204 / 204** | **204 / 204** | 204 |
  | `atx-impl-strategy-mine-tests` whole | **44 / 44** (golden `0x889874a3b9b29c55` at 1 and 4 workers: `SignalFitnessDefaults.*` 2 / 2) | **44 / 44** (same 2 / 2) | 44 |
  | `atx-engine-alpha-tests` whole | **782 / 782** (OpSig 3, AlphaVmSlotReuse 6, AlphaVmBandParity 4, AlphaVmTruncation 4) | 781 run: 779 passed; 1 skipped (`AlphaVm_ZeroAlloc.WarmDispatchLoop...`, "requires the MSVC CRT debug heap", by design); **1 FAILED: `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`**. (782 vs 781: `AlphaSlotPoolDeathTest.OverAcquire_Aborts` is Debug-only.) | 782 (Debug) |
  | `atx-engine-factory-tests` whole | **396 / 396** (M1d 392 + IcScreen 2 + IcLabels 2) | first run 389 passed / 7 failed; after p9-1z **396 / 396** | - |

- **Release red (STOP item): `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`**
  (`release-atx-engine-alpha-tests-whole.log:803-822`).
  - Release prints `vm_audit_exact=0xeeae40c4a6d7d93b vm_research_fast=0x24c97b5bfeab6518 oracle=0xeeae40c4a6d7d93b`.
  - The pins (`alpha_lit_golden_test.cpp:54-56`) are `0x971c3da60ca89aa3 / 0xe9e7128fd5359900 / 0x971c3da60ca89aa3`.
    Debug reproduces them (green).
  - All three asserts fail in Release, **including the oracle's**.

  My attribution is by code reading, not by a pre-S2 Release build: the red is pre-existing, Release-only, and not
  caused by S2.
  1. The pins were captured on the Debug build `mega-v7-w2` (W2 sprint, `41894ea4`, 2026-09-28;
     `platform-20260928/progress.md:279`). `atx-engine-alpha-tests` was never built in Release before this step: this
     Release tree had built only the IC / targets exes and the ic, target, w1-cost and w1-foundation test targets
     (p9-1j, p9-1n, p9-1o, p9-1q). So no earlier Release run of this test
     exists.
  2. The oracle (`evaluate_reference`) misses its pin too.
     - S2 changes the oracle in one place only: the grouped-op predicate (`oracle.cpp:321`, hand list ->
       `detail::needs_group_operand`).
     - `OpSig.PredicatesAgree` pins that predicate equal to the old hand list for every opcode, and it is green in
       Release.
     - The oracle's other includes (`bytecode.hpp`, `panel.hpp`, `registry.hpp`, where S2 changed a comment) carry no
       numeric change from S2.
     - So the Release oracle digest does not depend on S2, and the Release VM equals it (`vm_audit_exact == oracle`).
       The failure is a Release-vs-Debug floating-point difference against a golden pinned on Debug (the M1c-RED class).
  3. On real data, S2 moves no Release bit: 67 / 67 candidate signals are byte-identical between the pre-S2 and post-S2
     Release exes (cold-u pair below).

  Not fixed; no expected value edited. Options for the PM:
  - (a) Rule it a known red (Release-only, Debug-pinned golden) and pin Release digests beside the Debug ones.
  - (b) Make the battery contraction-free (C2-RED precedent).
  - (c) Attribute it empirically first: build `atx-engine-alpha-tests` in Release at `4497784b` (needs a detached
    checkout in pool-2 or a pool lease) and run the filter.

  Any other Release-only exposure stays unknown until the wave-2 Release suite runs.
- **Slip (environment, fixed and logged).**
  - Seven Release factory tests failed: `CascadeTrialCount.SeqEqualsParallelAtRealN`,
    `FactoryOos.{WalkForwardSeqParallel, PriceScaleGate_SeqEqualsParallel, DsrSubwindows_SeqEqualsParallel,
    CascadeGate_SeqEqualsParallel, AdmitLadder_SharedHelper_SeqEqualsParallel}` and
    `RobustnessBatteryFullWire.SerialParallelAgreeWithFullBattery`.
  - Each failed with `CHECK failed: Factory::gather_mine_scores: mine shard reported a fault` / SEH `0xc000001d`.
  - Cause: the documented missing-`atx-shm-worker` case (`atx-engine/plans/p6/TRACKER.md:61-63`). The Debug bin has
    the worker; the Release bin did not.
  - Fix: p9-1z built the worker. The seven now pass (`release-factory-seqpar-after-shm-worker.log`), and the whole
    factory binary passes 396 / 396 (`release-atx-engine-factory-tests-whole-2.log`). No code change.
- **pytest:**
  - `atx-impl/tools/test_fit_composition_rule_plugins.py`: **8 passed** (lane 8).
  - `scripts/tests/test_cycle_e2e.py` (canary, `ATX_CANARY_REQUIRED=1`): Debug **15 passed**, Release **15 passed**
    (lane 15, Debug only).
  - Store suite (next item): 43 passed under each of two seeds.
- **SQL2-CLS: nothing to add.**
  - The merge adds no `atx.<name>/v<n>` literal. Its diff names only `atx.composition-rules/v1`,
    `atx.dsl-candidate-signal/v2` and `atx.research-role-fields/v1`, all already registered. The guard
    `test_every_schema_literal_is_registered` is green.
  - Store suite `atx-engine/tools/test_research_store_{classes,blind,identity,fixtures}.py test_research_store.py`:
    **43 passed, 131 subtests** under `PYTHONHASHSEED` 0 and 1 (FixtureChain ran).
  - Fixture chain (`atx-research-store` p9-1u `3b955ba1`, rel and abs `--root`, `m2c-logs/fixture-chain.log`): init,
    catalog, digest and verify all exit 0; `files_seen 28 verified 25 declared 0 skipped 4`;
    **`catalog_digest d32f7655...4e69`** (= expected); `research_store_identity.py` total checked 12, mismatches 0.
  - No `classes.json` commit.
- **X-5 u / w flag-absent identity against the P9-B0 references** (step 4 i).
  - Exe: Debug p9-1x ic `73a6b4fd`. Argv: the reference receipt's, with only `--output` changed (`rerun.py` asserts the
    substitution). Bounded runner, reference limits, admission wait <= 3,600 s, 0 trials.
  - Compared by SHA-256 and JSON leaf path only (`cmp.py` prints no value). Volatile: timing leaves (`wall_seconds`,
    `hash_seconds`, `stage_seconds/*`) and the M1d-NOTE-3 cache strings.

  | run | vs | wall / peak | payload files | outside the volatile list |
  |---|---|---|---|---|
  | u `p9-m2c-yf0-u` | `p9-b0-yf0-u` | 13.3 s / 530 MiB | `orientations.json` `d0d16f30`, `recipe.json` `b9b4029c`, `train_daily_ic.csv` `c8714175` **same** | `summary.json`: **`/roles/0/ic_cache_bytes` only** (277,276,744 -> 277,276,800); `train_candidates.jsonl`: none |
  | w `p9-m2c-yf0-w` | `p9-b0-yf0-w` | 85.8 s / 1,436 MiB | all 10 payloads **same** (`train_combined.json` `e13fbc4d`, `.f64` `70d863f9`, planned targets `cb744a74`, ...) | `summary.json`: **`/roles/0/ic_cache_bytes` only** (+56); `train_candidates.jsonl`: none |

  - Cache: both runs warm on `mega-candidate-cache-v8-lo3`: 67 / 67 signal hits and 67 / 67 IC-result hits. Scope
    `ic1_b479dd191f59cbff` and identity `dslvm1_clang18.1` are unchanged, so the existing entries hit and the IC cache
    key did not move.
  - **`ic_cache_bytes` (+56 B in both build types; for the PM).**
    - It is a memory-accounting leaf, not a payload, key, recipe or decision: `labels.bytes()` =
      `sizeof(ic_screen_detail::Cache)` + label arrays (`ic_screen.cpp:366`).
    - It grows by exactly the S2-1 fields: `IcScreenConfig` `min_coverage` (8) + `hac_rule` (8 padded),
      `Cache::terminal` (8 padded), and `ResearchIcCoverage::terminal_imputed_pairs` x 4 (32). Total 56.
    - The lane's "summary minus timings" claim missed this leaf. I treat it as explained, not as an identity miss.
    - The wave-2 P9-B0 list must carry `/roles/*/ic_cache_bytes`, or the re-based references must be taken after S2.
- **Cold u before / after S2 (step 4 i, E-24), and Release identity of the banded VM on real data.**
  - Argv: Y-F0 u (library v8ysb, fields v15, `--workers 4`, `--no-composition`), with `--candidate-cache` pointing at
    a fresh empty dir. Release; cap 600 s / 3,072 MiB.
  - Before: the pre-S2 Release exe still on disk (`ea370cec`, p9-1n at `506ea123`), run before p9-1y replaced it.
    After: p9-1y `bf0ad802`. The two runs were about 40 minutes apart on the same busy host.

  | run | exe | wall | peak | admission wait |
  |---|---|---|---|---|
  | `p9-m2c-cold-u-old` | `ea370cec` (pre-S2) | **163.7 s** | 1,617 MiB | 0 s |
  | `p9-m2c-cold-u-new` | `bf0ad802` (S2) | **93.7 s** (-42.8 %) | 1,616 MiB | 155.6 s (a compiler elsewhere) |

  - New vs old outputs (`cmp-cold-u-new-vs-old.txt`):
    - `orientations.json`, `recipe.json` and `train_daily_ic.csv` are identical, and equal the P9-B0 Debug reference
      (`d0d16f30` / `b9b4029c` / `c8714175`).
    - `summary.json` differs only at timing leaves, the cache-dir path strings (two DIR names) and
      `/roles/0/ic_cache_bytes` (+56, as above).
    - `train_candidates.jsonl` differs at timing leaves only.
  - Cache entries (`cmp-cold-caches-new-vs-old.txt`): 201 / 201 files.
    - **67 / 67 signal payloads byte-identical.**
    - Sidecars differ only at `/engine_git_sha`.
    - IC-result entries differ only at `/recorded/engine_git_sha`, which is outside the hashed record.
  - Peak memory is unchanged at 4 workers, so the per-worker as-of scratch (review Suggested 5) does not show at the P9
    worker count.
  - Both cold caches (2 x 4.47 GB) were deleted by name afterwards.
- **S2-4 / W1-I1 path margin (step 4 ii; measurement only, `s2-4-path-margin.txt`).**
  - Release + audit-exact cache root: `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0_audit` (45 chars). The Release identity
    was read from the cold run's summary.
  - The deepest file is the IC-result entry `ROOT\<role sha64>\fp_<16>\ic1_<16>\<id>.<dsl16>.json`
    (`strategy_ic_runner.cpp:497,539`). The `.<16 hex>.partial` name (25 chars) is never longer than the final name for ids
    of 3+ chars.
  - Measured from pool-2:
    - Real entries on disk, rebased onto the audit root: `mega-candidate-cache-v8-lo3` is **251** chars
      (`value_composite_v49`, a field candidate), so **1 char over 250** and 8 under 259.
      `mega-candidate-cache-v8-lo1` is 247 (3 to spare).
    - Current parent `lib-v8ysb-gm.json` (and `lib-v8ys*`): deepest **250** (`earn_surprise_comp`, field candidate),
      margin **0**. `lib-v8x4*`: 251.
    - v6 / v7-era specs: up to 255. Their caches are absent, so unseen ids are counted as field candidates.
  - Rule: deepest = len(cache dir) + 175 + len(id) for a field candidate (+155 for a base one). With the lo3 root
    (57 chars), a field candidate's id must be <= 18 chars to stay <= 250. A pool with a two-digit number adds 1.
  - Effect of E2's plan-time refusal (W1-I1, > 250): it would refuse an audit-exact Release cell over lo3 from pool-2
    for `value_composite_v49`, and pass the current parent at exactly 250.
  - The 24-char AL / PRE id budget in W1-I1 does not fit the lo3 root. Field candidates with 24-char ids need a cache
    dir of <= 51 chars.
- **S2-6:** no `imputed-v1` IC-result entry exists under the spec caches.
  - lo3 holds one IC scope dir name (`ic1_b479dd191f59cbff`, in 93 dirs). It equals the flag-absent scope the P9-B0
    runs record.
  - lo1 holds one (`ic1_710caa717cb3193d`).
  - No file in either cache names `label_terminal`, and neither has a `dslvm*` or `_audit` root. Nothing deleted.
- **P13 (review "root must check" 3):**
  - The live X-5 / Y fields manifests `train-2020-2023-lo3-fields-v13` and `-v15` carry `seal.exclusive_end`
    `2024-01-01`, which binds. The runs above passed admission.
  - FYI: 10 v6 / v7-era manifests (`recent-fast-*-fields-*`) carry `2025-01-01`, and the IC runner now refuses them.
    Only superseded specs name them: `v61*.json`, `v70*.json`, `v71.json`, `v7u-lo3.json` (and
    `fund_industry_ic_v71.recipe.json`).
- **Not run:** the `hygiene` preset the original review lists. `research-build.ps1` builds only equity-dev and
  equity-rel, and raw cmake is forbidden.
- **DISK-2 (pool-18):** nothing to delete. `build-equity/` holds only `mega-p9-s2-*` receipts and logs (3.0 MB total
  with `audits/` and `v8-interim3-pitch-render-run2/`). The object tree was already gone. The `build/` lease stub is not
  touched.
- **Blind rule: one slip of mine.** While checking the old cold u's progress, I printed the tail of its `stdout.log`.
  It showed one candidate's screen-reason label (a classification string, no number). No return / IC / Sharpe /
  turnover / NAV statistic was read. Every later check read receipts, SHAs and JSON paths only.
- **Trial ledger:** 0 trials. `build-equity/trials.jsonl` is 133 lines, `27e40f9f`, at the start and at the end.

### M2c status

Code head `f6444417` (the S2 merge); the docs commit that adds this section sits on top. Next free build tag **p9-2a**
(p9-1x, p9-1y and p9-1z used). Known-red now:
- pytest `test_fields_are_the_rule_applied_to_the_registry` (E2);
- Release `BookNormalScore.*` x2 (C2);
- **Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (new, awaiting a ruling).**

Resume notes: `handoff-root-m2c.md`.

## M2d: D2 merge, COV merge (2026-10-04)

Agent: fresh root for step group M2d (dispatch after M2c-RED-1). Head at dispatch `f96f9399` (code head `f6444417`).
Python `"C:/Program Files/Python312/python.exe"`; every pytest `-q -rs -p no:cacheprovider`,
`PYTHONDONTWRITEBYTECODE=1`, explicit paths, own `--basetemp` in the session scratchpad; `scripts/tests` and the tools
suites in separate invocations. Evidence: `m2d-logs/` (this dir): gtest / pytest logs, receipts, identity reports and
the scratch scripts used (`m2d-logs/tools/`: `digests.py` (M2c's plus the pair-cache list),
`d2_flag_absent_fixture.py`, `d2_flag_absent_identity.py`, `rerun.py` (M2c's plus `--append`)).

**Housekeeping (before the merges):** docs commit `64069a3c` adds the PM's untracked files (`wave3-carry.md`,
`briefs/brief-COV-MV.md`, `resume4-dispatch.md`, `main-merge-verify.md`, `al-data/{atx-db-request,builder-designs,
task-AL-DATA-report}.md`; `git add -f`, no cache path). `disk-audit.md` (also untracked, not named) and the four
`docs/plans/*.png` were left alone.

### D2 (`49af0a52`, pool-20; re-review APPROVE `c55063ff`): MERGED and verified

- **Ledger:** `Task D2: complete` (re-review 1 APPROVE, 0 Required, 5 Suggested); rulings D2-1..D2-9, D2-R2, D2-S4
  read. D2-9: no second round; root fixes S-a and the report sentence as slips; the S-1 test, the S-2 refusal in
  `check_factor_series` and strict types in engine-mode `parent_check` are D3's (not done here).
- **Merge:** `61378337` = `git merge --no-ff 49af0a52`. Conflicts: the CMake tails of `atx-engine/CMakeLists.txt`
  (SQL2 catalog library + A3 + AL-SIG field sources vs D2's composition library and exe) and
  `atx-engine/tests/CMakeLists.txt` (SQL1 / SQL2 test targets + A3 + AL-SIG test sources vs D2's composition test
  target): both sides kept, HEAD's blocks first. Auto-merged: `strategy_ic_rules.cpp`, `strategy_ic_runner.cpp`,
  `composition_rules_list.json`, `strategy_ic_rules_test.cpp`, `strategy_ic_runner_test.cpp`. D2 adds no
  capability: `exe_capabilities` stays S2's 8 entries, sorted, size = count. The lane's report and first review came
  with the merge; `task-D2-rereview-1.md` and `evidence-D2/` (15 logs) copied in from pool-20 (`git add -f`).
- **Slips (ruling D2-9):**
  - `e129a7fa`: the three D2-added lines over 100 columns (re-review S-a: `ic_shrink.cpp:21` 105,
    `theme_erc.hpp:44` 102, `theme_erc.cpp:25` 110) wrapped. Checked whitespace-only: the files' non-whitespace bytes
    equal the merge's (the header gains one `//` for its continued trailing comment). The other long lines in those
    files are verbatim moves from atx-impl (D2-6), confirmed line by line against `1239a5ff`.
  - `task-D2-report.md:250` (S-1 row): "Tested in `Comparisons`" replaced by a marked correction (no S-1 test
    exists; the re-reviewer's probe confirmed the behaviour; the test is D3's required carry). Committed with the docs.
- **SQL2-CLS:** `c0446b19`. The guard failed on the merged head on two literals: `atx.composition-fit-spec/v1`
  (`composition_cli.{hpp,cpp}`, `composition_engine_fit.py`) and `atx.composition-fit/v1` (`composition_cli.hpp`,
  `composition_engine_fit.py`). Both join `fit-output` (precedent `atx.composition-rules/v1`,
  `atx.research-admission/v1`); its writer text names `research/composition (D2)`. `atx.dsl-composition-weights/v3`
  was already registered; `atx.composition-fit-fixture/v1` lives only in a test file (not scanned). After: guard green;
  store suite `test_research_store_{classes,blind,identity,fixtures}.py test_research_store.py` **43 passed, 131
  subtests** under `PYTHONHASHSEED` 0 and 1, no skip. **Fixture chain** (`atx-research-store` p9-1u `3b955ba1...`,
  catalog sources unchanged; rel and abs `--root`; `m2d-logs/d2-sql2cls-fixture-chain.log`): `files_seen 28 verified 25
  declared 0 skipped 4`, **`catalog_digest d32f7655...4e69`** (= expected), `digest` same, `verify --pins` 0,
  `research_store_identity.py` total checked 12, mismatches 0.
- **Build p9-2a** (equity-dev, `-Canary`; source `c0446b19`, 6 dirty entries = `progress.md`, the D2 report edit and
  the four PNGs, no code): `atx-engine-research-composition-tests, atx-research-composition, atx-research-admission,
  atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-equity-strategy-targets, atx-impl-strategy-target-tests,
  atx-engine-combine-tests, atx-impl-tests` -> **exit 0, 430.9 s, 135 TUs, 13 links, Jobs 4, 0 warnings**. It waited
  (bounded, about 6 min) for two lane builds on the host (pool-19 AL-COMB, pool-18 S2R) to finish before starting.
  Exe SHA-256 prefixes: composition `b0175bd1`, admission `411af33a`, ic `ba1232ed`, targets `2604e862`, ic-tests
  `8a9045eb`, target-tests `f9d7d161`, combine-tests `58e3214b`, composition-tests `be73af8f`, impl-tests `b2e00d49`
  (receipt `m2d-logs/mega-p9-2a-receipt.json`). **Canary (Debug, tiny world): 15 passed in 8.3 s** (goldens unchanged).
- **gtests** (Debug, p9-2a; logs `m2d-logs/d2-gt-*.log`):

  | target | filter | result |
  |---|---|---|
  | atx-engine-research-composition-tests | `ResearchComposition.*` | **10 passed** |
  | atx-impl-strategy-ic-tests | `MarginalIc.PairCacheSourcesPinned` | **1 passed** (3 sources) |
  | atx-impl-strategy-ic-tests | whole (CompositionRules, IcAdmission, IcShrinkV1, ThemeErcV1, ThemeTsmom, ThemeResid, MarginalIc, CombineMarginalRankIc, StrategyIcRunner, S2's) | **206 passed** (lane 195 at its base; S2 204 at M2c) |
  | atx-impl-strategy-target-tests | whole (TwoSpeed and the rest) | **338 passed** (lane 325 at its base) |
  | atx-engine-combine-tests | whole (GroupRerank and the rest) | **243 passed** |
  | atx-impl-tests | whole (never built by the lane) | **1135 passed, 5 skipped** (real-data / opt-in gated: `Alpha101Orats.RankBySharpeRiskParity`, `.RankByOptimizerSharpe`, `AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`, `FundamentalZoo.RealDataIcReport`, `SingleAlphaCapacity.SweepAndVerify`), 0 failed, 717 s |

- **pytest** (real exes of p9-2a: `ATX_RESEARCH_ADMISSION_EXE`, `ATX_RESEARCH_COMPOSITION_EXE`,
  `ATX_EQUITY_TARGETS_EXE` set): `atx-impl/tools`, the lane's 13 files: **264 passed, 39 subtests, 0 skipped** (177 s;
  includes S-7: `test_composition_engine_fit.py` 20 with `RealExecutables` run, no skip);
  `scripts/tests/test_no_python_mirror.py` separately: **7 passed**.
- **Root item (i), pair-cache pin:** recomputed from the merged tree (HEAD blobs and the work tree) with the tripwire
  recipe: `pair_sources` 3 files `df589a88...` = pin (`m2d-logs/d2-source-digests.txt`). The pin was `88f97f24`
  (2 files) at both `1239a5ff` and `f6444417`, so no other lane touched it. `dsl_vm` `5bcd55c7` (35) and `ic_result`
  `dd3c6011` (11) unchanged on the merged tree (= M2c).
- **Root item (ii), refit check:** **no P9 parent spec uses a pre-D2 `--theme-resid-parent` file.** `git grep` over
  the tracked tree: the only spec naming `--theme-resid` is the v8 template `scripts/specs/v8/r11.json` (parent
  derived at lock time, `nominal_parent r1-comp-v8.json`); no locked spec pins a resid parent. The P9 lineage parent
  Y-F0 (`lib-v8ysb-gm.json`, also `lib-v8ysb.json`) fits `ew-theme-std-v1 --theme-erc theme-erc-v1`, no resid;
  X-10 / Y-1 are NAV templates. Note: the P9-B0 fit reference `p9-b0-yf0-fit/composition_weights.json` is pre-D2 (key
  names read: `provenance.std.module_sha256`, `provenance.theme_erc.module_sha256`), so an R-11-type P9 cell on Y-F0
  would be refused by `parent_check` (S-5) until its parent file is post-D2. The wave-2 P9-B0 re-fit with the merged
  fitter plus the W1-I2 (b) re-pin produces such a file; nothing to refit now (0 trials if ever needed).
- **Root item (iii), flag-absent identity (synthetic, 0 trials, no statistic read):** the fitter run flag-absent with
  the pre-D2 tools (`git archive f6444417` of `atx-impl/tools`, `atx-engine/tools`, `research_window.json`) and with
  the merged head's, on the same inputs; every output compared by SHA-256 and JSON path.
  - Tiny world (`m2d-logs/d2-flag-absent-tinyworld.json`; the canary's own fit argv `ew-theme-v1` on a u pass of the
    tiny world; with three themes the standardised / shrink rules refuse the tiny world by the member cap):
    `admission.csv` identical; `admission.json` differs only by `$.inputs.script_sha256` ->
    `$.inputs.producer_fingerprint`; `composition_weights.json` only by `$.provenance.script_sha256` ->
    `$.provenance.producer_fingerprint` and the value of `$.provenance.admission_sha256`.
  - Fitter fixture (`m2d-logs/d2-flag-absent-fixture.json`; `aim_world` as `test_composition_engine_fit.World`, two
    themes, registry absent), 9 cases, all exit 0 on both sides: std-erc (the Y-F0 form), std-erc-tsmom, shrink,
    shrink-erc, shrink-tsmom, std, std-aim-resid, an R-11 child of `std` (`--theme-resid-parent` + pin), ew-theme.
    In every case `admission.csv` is identical and the differences are exactly: admission.json
    `inputs.script_sha256` -> `inputs.producer_fingerprint`; weights `provenance.script_sha256` ->
    `provenance.producer_fingerprint`; `provenance.<block>.module_sha256` -> `provenance.<block>.producer_fingerprint`
    for each block the case writes (std, ic_shrink, theme_erc, theme_tsmom, resid); the value of
    `provenance.admission_sha256` (admission.json's bytes move with its CM-5 key); and, in the R-11 child, the value
    of `provenance.resid.parent_weights_sha256` (its parent file moved). No other path; no file added or removed.
  - So flag-absent outputs differ from the pre-D2 build only in the CM-5 / D2-2 keys and the two SHA values that hash
    them. Decision bytes unchanged (the canary digests strip `inputs` / provenance: canary 15 passed).
  - **Draft wave-2 P9-B0 substitution list, D2 part** (to be ruled before the P9-B0 run, not now). Fit outputs of
    the Y-F0 re-fit (`ew-theme-std-v1 --theme-erc theme-erc-v1`):
    1. `admission.json`: `$.inputs.script_sha256` (removed), `$.inputs.producer_fingerprint` (added);
    2. `composition_weights.json`: `$.provenance.script_sha256` -> `$.provenance.producer_fingerprint`;
       `$.provenance.std.module_sha256` -> `$.provenance.std.producer_fingerprint`;
       `$.provenance.theme_erc.module_sha256` -> `$.provenance.theme_erc.producer_fingerprint`;
       `$.provenance.admission_sha256` (value; consequence of 1);
    3. for other lineage forms the same rename on `provenance.{std_identity,ic_shrink,theme_tsmom,resid}`, on
       `provenance.v6.inputs.script_sha256` (v6 only), and the value of `provenance.resid.parent_weights_sha256` in
       resid children;
    4. downstream only if a pass reads the re-fit file. P9-B0 runs each reference argv with only `--output` changed,
       so its w reads the reference weights and nothing cascades. If the W1-I2 (b) re-pin later points w at a post-D2
       weights file, its SHA moves into w `recipe.json` / `summary.json` / `train_combined.json`
       `composition_weights_sha256` (and `summary.json` `composition_weights.sha256`) and NAV `summary.json`
       `composition_weights_sha256` / `source_bindings.composition_weights_sha256` (paths found in the P9-B0 w / NAV
       references by key, no value read).
- **Root item (iv):** superseded (ledger "Root item (replaces D2's ...)"); nothing done. E2 is not merged, so the
  positive live-table check runs at E2's merge.
- **Not done here:** the D2 report's X-5 real-data flag-absent / flag-present runs (the brief named the canary /
  fixture for (iii); the X-5 re-fit belongs to the wave-2 P9-B0, with the list above).
- **DISK-2 (pool-20):** nothing to delete; the lane had already deleted its object tree (2.7 MB left: receipts, logs,
  `audits/`, `v8-interim3-pitch-render-run2/`).
- **Trial ledger:** 0 trials (133 lines, `27e40f9f`).

### COV (`c8ff4f1c`, pool-24, base `ad406718`; re-review 2 APPROVE `75428364`)

#### Expected substitution list for the COV flag-absent identity (written before the run)

Real data, 0 trials, compared by file SHA-256 and JSON path only (no statistic read). Reference = the R-8 store of
record `build-equity/v8-risk-lo3-v10` (receipt `v8-risk-lo3-v10-run`, exe `8967952c...`, engine git `807af678`).
Both runs are the reference receipt's argv with only `--output` changed (`m2d-logs/tools/rerun.py`), through
`run_bounded_research.py`, Debug, `--seconds 600 --max-rss-mib 8192` (brief: <= 600 s, <= 8,192 MiB), exe =
p9-2b `atx-equity-strategy-risk`. S9 checked first: role `train-2020-2023-lo3` (`e1c67101...`)
`universe.delisting.returns_applied == false` (`m2d-logs/cov-s9-role-delisting.txt`).

1. `p9-m2d-cov-risk-legacy` (no new flag): every `manifest.files` entry (bias.csv, bias_summary.json,
   diagnostics.csv, exposures_last.csv, factor_covariance.f64, factor_returns.csv, factor_structural.u8,
   industry_slot.u8, specific_variance.f64, style_exposures.f32) byte-identical to the reference (SHA-256 of the
   file and the manifest entry); `manifest.json` equal to the reference's at every JSON path except
   `$.producer.executable_sha256` and `$.producer.engine_git_sha`.
2. `p9-m2d-cov-risk-container` (the same argv plus `--emit-container`): every file of 1 byte-identical to 1;
   `manifest.json` equal to 1's except the added `$.container` (and everything under it) and the added
   `$.files["model.atxcov"]`; `atx-equity-strategy-risk cov-diff --legacy build-equity/p9-m2d-cov-risk-container`
   exits 0 (plan section 4.2).

Any other difference is a stop.

#### COV: MERGED and verified

- **Ledger:** `Task COV: complete` (re-review 2 APPROVE, 0 Required, 3 Suggested); rulings COV-1..COV-11, COV-R1
  read. COV-11: S10 / S11 are COV-MV's required carry (not done here); S12 is a root slip.
- **Merge:** `694b4b95` = `git merge --no-ff c8ff4f1c`, **no conflict**. The two named cross-lane appends merged with
  both sides kept: `atx-engine/CMakeLists.txt` (`src/risk/cov_container.cpp` beside `specific_risk.cpp`) and the
  `atx-impl-strategy-target-tests` source tail in `atx-impl/tests/CMakeLists.txt` (C2 is not merged yet, so its tail
  lines meet COV's at C2's merge). `cov-design.md` / `cov-plan-amendment.md` already equal on both sides. The report,
  review, re-review 1, handoff and `cov-evidence/` came with the merge; `task-COV-rereview-2.md` copied in from pool-24.
- **Slip S12 (COV-11):** `task-COV-report.md:494` named the old encoder refusal "factor covariance is not K x K";
  corrected to "exposures are not rows x dense in the file's dtype" (`check_block` checks the exposures first,
  `cov_container.cpp:110` before `:112`), with a marked root note. Committed with the docs.
- **SQL2-CLS:** no new literal (the lane's report: the container format name `atx.cov-container/1.0` is not an
  `atx.<name>/v<n>` schema). Guard on the merged head: 5 passed, 92 subtests; no commit. Fixture chain re-run anyway
  (`m2d-logs/cov-fixture-chain.log`, rel and abs): `files_seen 28 verified 25 declared 0 skipped 4`,
  `catalog_digest d32f7655...4e69`, `verify --pins` 0, identity 12 / 0.
- **Build p9-2b** (equity-dev; source `4c9dfd9d` = the COV merge plus the D2 docs commit, 5 dirty entries, docs
  only): `atx-engine, atx-engine-risk-tests, atx-impl-strategy-target-tests, atx-equity-strategy-risk,
  atx-equity-strategy-targets` -> **exit 0, 269.4 s, 82 TUs, 6 links, Jobs 4, 0 warnings**. Exe SHA-256 prefixes:
  risk `efb21fad`, targets `3fa29f2b`, engine-risk-tests `3d52efaa`, target-tests `dff75bd9` (receipt
  `m2d-logs/mega-p9-2b-receipt.json`).
- **gtests** (Debug, p9-2b; logs `m2d-logs/cov-gt-*.log`):

  | target | filter | result |
  |---|---|---|
  | atx-engine-risk-tests | `RiskCovContainer.*` | **21 passed** |
  | atx-engine-risk-tests | `Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` | **431 passed, 1 skipped** (`RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`, nightly), 135 s |
  | atx-impl-strategy-target-tests | `CovTruncation.*:CovEigen.*:CovRecipe.*:CovStore.*:CovBias.*:CovDiff.*` | **29 passed**, 188 s |
  | atx-impl-strategy-target-tests | unchanged `RiskWls.*:RiskEwma.*:RiskBias.*:RiskModel.*:RiskVerb.*:RiskRobust.*:RiskTarget.*:VolTarget.*:NavBookRule.*:Spo*:NavV7Hook.*` | **104 passed** |

- **S9 (COV-9):** role `build-equity/train-2020-2023-lo3/manifest.json` (`e1c67101...`):
  `universe.delisting.returns_applied = false`, and 0 of its 990 events carry `returns_applied: true`
  (`m2d-logs/cov-s9-role-delisting.txt`). Checked before both COV runs below.
- **Flag-absent identity (real data, 0 trials; the list above was committed first, `70104732`):**
  - Limits amendment (no output byte depends on it): the first launch with `--max-rss-mib 8192` waited about 10 min
    in the runner's admission, which needs free memory >= peak + floor (8,704 MiB, never free on this host); I
    stopped my two processes (no exe had launched) and deleted that run dir by name. Both runs then used
    `--seconds 600 --max-rss-mib 2048` (within the brief's <= 8,192), argv unchanged.
  - 1 `p9-m2d-cov-risk-legacy`: completed, exit 0, **54.3 s, peak 612 MiB** (reference 31.2 s on the v8 exe). All
    10 payload files **byte-identical** to `v8-risk-lo3-v10` (and the manifest's `files` entries self-consistent);
    `manifest.json` differs **only** at `$.producer.engine_git_sha` (`807af678` -> `4c9dfd9d-dirty`) and
    `$.producer.executable_sha256` (`8967952c` -> `efb21fad`), as listed (`m2d-logs/cov-identity-legacy-cmp.txt`).
  - 2 `p9-m2d-cov-risk-container` (`--emit-container`): completed, exit 0, **53.6 s, peak 625 MiB**. The 10 legacy
    files byte-identical to 1; one file added, `model.atxcov`; `manifest.json` = 1's plus `$.container`
    (`bytes, file, file_sha256, format, root_sha256`) and `$.files["model.atxcov"]` (`bytes, sha256`), no other path
    (`m2d-logs/cov-identity-container-cmp.txt`). **`atx-equity-strategy-risk cov-diff --legacy
    build-equity/p9-m2d-cov-risk-container` exit 0**: "legacy equal: 1405 sessions factor_covariance.f64
    specific_variance.f64 style_exposures.f32 industry_slot.u8" (`m2d-logs/cov-diff-legacy.log`).
  - So the default recipe on the merged build reproduces the store of record bit for bit, and the container carries
    the same bytes as the legacy files for every session.
- **Bench (root step 6):** built and run once, no gate (`m2d-logs/cov-bench.log`, `cov-bench-list.txt`). The Release tree `build-equity-rel` was reconfigured with `ATX_BUILD_BENCH=ON` through `atx-build.ps1 configure -Preset equity-rel -Bench` (FetchContent fetched google-benchmark v1.9.0 into `deps/equity-rel`, 12 MB), **p9-2c** equity-rel `atx-engine-bench` -> exit 0, 162.9 s, 57 TUs, 0 warnings, exe `8b34199d` (`m2d-logs/mega-p9-2c-receipt.json`); then reconfigured back to `ATX_BUILD_BENCH=OFF` (cache otherwise unchanged: `ATX_TEST_GROUPS all`, Release). The `equity-rel` tree stands in for the report's `rel` preset (same build type; `rel` has no tree in pool-2). `--benchmark_filter=CovContainer --benchmark_repetitions=5`, 32 benchmarks, 204 s, SHA-256 backend `x86-sha-ni`, warm cache only. Medians against cov-design 4.12 (N = names, f32 / f64):

  | benchmark | N 3000 f32 | N 5627 f32 | N 3000 f64 | N 5627 f64 | 4.12 target | met |
  |---|---|---|---|---|---|---|
  | Write (excl. estimation) | 509 ms, 691 MiB/s | 1,059 ms, 561 MiB/s | 1,248 ms, 600 MiB/s | 2,037 ms, 609 MiB/s | >= 300 MB/s | yes |
  | OpenLazy | 211 us | 225 us | 217 us | 211 us | <= 2 ms | yes |
  | OpenFull, default workers | 115 ms | 206 ms | 187 ms | 338 ms (2.0 GiB/s) | <= 0.5 s for 402 MiB (SHA-NI) | yes |
  | OpenFull, 1 worker | 338 ms | 566 ms | 554 ms | 960 ms (single-thread rate ~ 0.7 GiB/s) | printed | - |
  | FirstTouch (lazy verify + view) | 404 us | 612 us | 568 us | 1,038 us | <= 1 ms | yes, except N 5627 f64 (1.04 ms, 4 % over) |
  | View (verified block) | 0.079 us | 0.076 us | 0.082 us | 0.071 us | <= 5 us | yes |
  | LegacyStoreOpen (emulation) | 310 ms | 558 ms | 455 ms | 688 ms | ratio only | open-full is faster than legacy open in every shape (the design's only blocking case does not occur) |
  | LegacyStoreRead (emulation) | 424 us | 636 us | 494 us | 799 us | ratio only | - |

- **Not now (ruled):** D-COV and the COV-7 Debug = Release container identity (wave-2 gate steps). Also left for the
  wave-2 gate: report step 4 (Y-1's NAV read through the container store vs its P9-B0 reference; the container dir
  `p9-m2d-cov-risk-container` is kept for it, 968 MB) and step 5 (`atx-cov-v1` on TRAIN, Release, after COV-7).
- **Engine owner note (report root item 5):** `atx-engine/src/book/risk_target.cpp:35` computes `k * k` in usize
  (pre-existing); unreachable through a container now that K <= 8,193.
- **DISK-2 (pool-24):** nothing to delete; the lane had already deleted its object tree (2.9 MB left: receipts, logs,
  `audits/`, `v8-interim3-pitch-render-run2/`). Mine: `build-equity/p9-m2d-cov-risk-legacy` (481 MB, a byte copy of
  the store of record's payloads) deleted by name after the comparison; its run receipt kept.
- **Blind-rule disclosures (FYI, no ruling asked):** (a) my first S9 read printed the role manifest's
  `universe.delisting` block; the preview showed three 2018 delisting events' metadata (cause codes, imputed
  constants), no 2020-2023 statistic; the output was discarded and the check re-run printing only the flag. (b) One
  print of the reference manifest's `run` block showed the risk model's `mean_r2` (a factor-regression fit
  diagnostic, not a return / IC / Sharpe / turnover / NAV statistic). Every comparison above is SHA / JSON path only.
- **Trial ledger:** 0 trials (133 lines, `27e40f9f`).

### M2d status

Both merges done and verified; nothing half-merged; no process of mine running. Code head `694b4b95` (the COV merge);
the docs commits sit on top. Build tags used: p9-2a, p9-2b (equity-dev), p9-2c (equity-rel bench). Next free build tag
**p9-2d**. No new red. Known-red now (unchanged):
- pytest `test_fields_are_the_rule_applied_to_the_registry` (E2);
- Release `BookNormalScore.*` x2 (C2);
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED; lane S2R).

Trial ledger at the end: `build-equity/trials.jsonl` 133 lines, sha256 `27e40f9f...` (0 trials). Resume notes:
`handoff-root-m2d.md`.

## M2e: C2 merge, AL-COMB merge (2026-10-04)

Agent: root for step group M2e, resumed after the owner stop (`handoff-root-m2e.md`; dispatch R5-1). Evidence:
`m2e-logs/` (this dir), scratch scripts in `m2e-logs/tools/`.

### C2 (`4d77d3db`, pool-15, base `ad406718`; re-review 2 APPROVE `1ea664f2`)

#### Expected substitution list for the C2 X-5 gm-pair identity (written before the run)

Real data, 0 trials, no return / NAV / Sharpe / turnover statistic read: files compared by SHA-256 and JSON path; the
only values read are all-rows S2 gross leverages (mechanics, compared for bit equality, not printed) and the
calibration record. Tool `m2e-logs/tools/c2_gm_pair.py`; output `m2e-logs/c2-gm-pair-cmp.txt`.

- **Reference:** `build-equity/p9-c1-s2-x5-nav` = X-5 (`x-theme-erc-gm.json`, the cell of record at L 1.1720), the
  Debug X-5 NAV reference since C1 (DEC-20 re-pin; exe `bf4b0ec2`, receipt `p9-c1-s2-x5-nav-run`, argv sha
  `85a2ed98`; 27 files).
- **G_parent:** `scripts/wave_readers.py mechanics` (the reader the match stage runs as `mech-parent`) of X-5's parent
  X-3 NAV `build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b` (summary.json `e342db4f`,
  the value locked in both X-5 specs), `mean_gross_leverage_all_rows`, kept as `repr` in `m2e-logs/c2-gm-gparent.json`.
- **Runs** (each = the reference receipt's argv with only the named tokens substituted; exe = p9-2d
  `atx-equity-strategy-targets` `90cb34a7`, Debug, source `a25453be`; `run_bounded_research.py --seconds 600
  --max-rss-mib 2048` per RUN-1):
  - P1 `p9-m2e-c2-x5-cal`: `--aim-leverage 1.1414` (the Python match's calibration run at the parent's L);
  - P2 `p9-m2e-c2-x5-gm`: argv unchanged but `--output` (the Python match's matched run at L' = 1.1720);
  - E `p9-m2e-c2-x5-engine`: `--aim-leverage 1.1414` plus `--calibrate-gross <repr(G_parent)> --calibrate-tolerance
    0.005` appended (exactly what `wave_stage_cell.calibrated_doc` adds to the cell's nav flags).

Expected (anything else is a stop):

1. **Flag-absent (C2 report identity 1):** P2 vs the reference: the same 27 files; the 26 other than `summary.json`
   byte-identical; `summary.json` differs only at `/producer/engine_git_sha` (the configure-time commit,
   `1239a5ff...-dirty` -> `a25453be...-dirty`).
2. **Python rule on P1 / P2** (`wave_rules`, the code the deletion in (ii) removes): `gross_matches("pm6-6", G_parent,
   G_cal)` false (one correction); `matched_leverage("1.1414", G_parent, G_cal)` = `"1.1720"` (the registered -gm L);
   `gross_matches("pm6-6", G_parent, G_P2)` true. (G_cal, G_P2 = the mechanics reader's all-rows gross of P1, P2.)
3. **Engine vs Python (C2 report identity 2):** E vs P2: the same 27 files; the 26 other than `summary.json`
   byte-identical (recipe.json is the plain run's at L'); `summary.json` equal at every JSON path except the added
   `/calibration` (and the leaves under it). `/producer` equal (same exe, same tree).
4. **Calibration record of E:** keys exactly `target, tolerance, leverage_requested, gross_requested, corrected,
   leverage, gross, matched, construction_reused, rule`; `target` = G_parent (bit); `tolerance` 0.005;
   `leverage_requested` 1.1414; `gross_requested` = G_cal (bit); `corrected` true; `leverage` = float(Python L')
   within 1e-12 relative; `gross` = G_P2 (bit) = the mechanics reader on E itself (bit); `matched` true.
   `construction_reused` is read and reported, not compared (X-5 sets none of the exclusions; false would mean pass 2
   formed the construction again: same bytes, smaller wall gain).
5. **Printed, not compared:** wall and peak RSS of P1 + P2 (two loads, two runs) against E (one load); E's peak
   against the 2,048 MiB cap (S9: the tape is reserved up front).

Not run, by design: the real-data engine *wave* (report item 2, second half). A wave runs the judge stage (book
reader, trial ledger), which the 0-trial and blind rules forbid here; the wave layer is covered on the synthetic
fixture by `test_engine_gross_match_is_the_python_match_in_one_nav` (`test_research_wave.py` 20 passed on the merged
head, `m2e-logs/c2-pyt-research-wave.log`).

#### C2: MERGED and verified (root item (ii) left to C3, reasons below)

Python `"C:/Program Files/Python312/python.exe"`; every pytest `-q -rs -p no:cacheprovider`,
`PYTHONDONTWRITEBYTECODE=1`, explicit paths, own `--basetemp` in the session scratchpad; `scripts/tests` and the
`atx-engine/tools` suite in separate invocations. gtests through `m2e-logs/tools/gt.sh` (vcpkg DLL dir of the build
type first on PATH).

- **Housekeeping (before the merge):** docs commit `20bc5f5f` adds, unedited, `status-5.md`, `next-goal-prompt.md`,
  `progress.md` (LEDGER-1), the two modified `al-data/*.md` and `docs/plans/2026-10-04-p9-engine-impl-status.md`
  (`git add -f`, no cache path). The four `docs/plans/*.png` and `disk-audit.md` left alone. Trial ledger at the
  start: 133 lines, `27e40f9f`.
- **Ledger:** `Task C2: complete` (re-review 2 APPROVE at `4d77d3db`, 0 Required, 1 Suggested = S14); rulings P15,
  C2-1..C2-10, C2-R2, C2-R3, C2-4b, C2-RED, M1c-RED read. C2-10: S14 is C3's required carry (not done here).
- **Merge:** `a25453be` = `git merge --no-ff 4d77d3db`. One conflict, the `atx-impl/CMakeLists.txt` tail (D2's
  `target_link_libraries(atx-impl-core PUBLIC atx-engine-research-composition)` vs C2's
  `target_sources(atx-impl-core PRIVATE src/strategy_nav_calibrate.cpp)`): both kept, HEAD's block first. Auto-merged:
  the `atx-impl/tests/CMakeLists.txt` tail (COV's lines and C2's `strategy_nav_calibrate_test.cpp` both present), and
  the two E2-file edits `scripts/wave_result.py` (round 1) and `scripts/wave_stage_record.py` (R4's two lines, C2-8;
  E2 is not in root, so they wait for E2's merge to be reconciled). The report, review, re-review 1, handoff and the
  `c2-fix*` / `c2-*` evidence came with the merge (checked equal to pool-15's files modulo CRLF);
  `task-C2-rereview-2.md` (lane commit `1ea664f2`, after the merged SHA) copied in from the lane blob (`git add -f`).
  pool-15's `c2-fix1/` holds only a `.mypy_cache` and was not copied.
- **Build p9-2d** (equity-dev; source `a25453be`, 4 dirty entries = the four PNGs, no code; the tree reconfigured
  itself on the CMake change, ConfiguredProvenance `a25453be...-dirty`): `atx-impl-strategy-target-tests,
  atx-equity-strategy-targets, atx-engine-eval-tests, atx-engine-book-tests, atx-impl-tests` -> **exit 0, 331.8 s,
  114 TUs, 8 links, Jobs 4, 0 warnings** (the log's one stderr line is CMake's "GLOB mismatch" reconfigure notice).
  Exe SHA-256 prefixes: target-tests `ecb229ca`, targets `90cb34a7`, eval-tests `65f78372`, book-tests `ff3ddfce`,
  impl-tests `eee53e02` (`m2e-logs/mega-p9-2d-receipt.json`).
- **Build p9-2e** (equity-rel; same source and targets; no stale-PCH refusal: `deps/equity-rel` was not fetched again)
  -> **exit 0, 775.4 s, 253 TUs, 10 links, Jobs 4, 0 warnings**. Exe prefixes: target-tests `38bf32f6`, targets
  `cb2df40d`, eval-tests `e4edccb1`, book-tests `3032649c`, impl-tests `b38952e9`
  (`m2e-logs/mega-p9-2e-receipt.json`).
- **gtests** (logs `m2e-logs/c2-gt-{dbg,rel}-*.log`):

  | binary | filter | Debug (p9-2d) | Release (p9-2e) |
  |---|---|---|---|
  | atx-impl-strategy-target-tests | `CalibrateGross.*:NavBookRule.*:BookNormalScore.*` | **19 passed** | **19 passed** |
  | atx-impl-strategy-target-tests | whole | **377 passed** (= M2d's 338 + COV's 29 + C2's 10) | **377 passed** |
  | atx-impl-tests | `CalibrateGross.*` (the glob's copy; never built by the lane) | **7 passed** | **7 passed** |
  | atx-engine-eval-tests | `*StatsExt*:*Deflated*:*MinTrl*` | **10 passed** | **10 passed** |
  | atx-engine-book-tests | `BookNormalScore.*:*DecayMonitor*` | **13 passed** | **13 passed** |

  **M1c-RED cleared:** `BookNormalScore.TiesShareTheMeanRankAndMirrorsAreOpposite` and `.FixtureTellsWrongRulesApart`
  pass in Release in both binaries that hold them (target-tests and book-tests), with
  `NormPpfOfALiteralIsItsRunTimeValue`; no expected value edited (C2-RED). From this head, a Release run of a
  `--rank-shape norm-score-v1` book is evidence again (M1c-RED Ruling's restriction lifted).
- **pytest:** `scripts/tests/test_research_wave.py` **20 passed** (38 s); from `scripts/tests` the eight wave suites
  (`test_no_python_mirror test_wave_driver test_wave_hardening test_wave_queue test_wave_review test_wave_review_minor
  test_wave_scoreboard test_wave_speed`) **77 passed** (194 s; G-P5 line "34 mirrored rule function(s) in 13 row(s)",
  = D2's count). Logs `m2e-logs/c2-pyt-*.log`.
- **(i) X-5 gm-pair identity (real data, 0 trials; list committed first, `4d0583cf`): HOLDS** on every item
  (`m2e-logs/c2-gm-pair-cmp.txt`; receipts `m2e-logs/receipt-p9-m2e-c2-x5-{cal,gm,engine}.json`). All three runs
  exe `90cb34a7`, source `4d0583cf` (= the merge plus the list commit), "clean in the code pathspec":

  | run | what | outcome | wall | peak RSS |
  |---|---|---|---|---|
  | P1 `p9-m2e-c2-x5-cal` | Python step 1, L 1.1414 | completed, exit 0 | 45.0 s | 591 MiB |
  | P2 `p9-m2e-c2-x5-gm` | Python step 2, L 1.1720 | completed, exit 0 | 47.4 s | 591 MiB |
  | E `p9-m2e-c2-x5-engine` | `nav --calibrate-gross`, L 1.1414 requested | completed, exit 0 | 63.7 s | 639 MiB (cap 2,048) |

  1. P2 vs `p9-c1-s2-x5-nav`: 27 / 27 files, 26 byte-identical; `summary.json` differs only at
     `/producer/engine_git_sha`. Flag-absent identity of the merged NAV exe on X-5 holds.
  2. Python rule: the L 1.1414 calibration is outside .005 (corrected); `matched_leverage` gives **`1.1720`** = the
     registered `x-theme-erc-gm.json` L; the -gm run is within .005 (matched).
  3. E vs P2: 26 files byte-identical (recipe.json included: the plain run's at L'); `summary.json` differs only by
     the added `/calibration` (10 leaves).
  4. Calibration record: `target` = G_parent (bit), `tolerance` 0.005, `leverage_requested` 1.1414,
     `gross_requested` = the Python reader's G of P1 (bit), `corrected` true, `leverage` = float("1.1720") (relative
     difference 0.0), `gross` = the Python reader's G of P2 and of E (bit), `matched` true.
     **`construction_reused` true** (S6 / S9 / R5 real-data cover: X-5 carries `--warm-start-sessions 60` and
     `--max-bytes 1 GiB`; the console says "construction reused").
  5. Wall: two processes 92.3 s (P1 + P2) vs one process 63.7 s; E's peak +48 MiB over a plain run (the tape,
     reserved up front), far inside the cap.

  G_parent travels in E's argv (its receipt), exactly as the engine match stage's run recipe carries it (the X-5
  spec's description already states it to 10 digits); no other gross value was printed (E's console lines carry them
  and were read only through a filter that masks the numbers). No return / NAV / Sharpe / turnover statistic was read.
- **(ii) deletion of `wave_rules.py:110-119`: NOT done here; left to lane C3** (its carry already lists it as
  "required (root, if still present)", `wave3-carry.md:221`). The two functions (`matched_leverage`,
  `gross_matches`; lines 113-122 on the merged head) are not a self-contained block: they are the whole Python `pm6-6`
  match mode. Callers on the merged head: `wave_stage_cell.match` (`:241`, `:245`, `:262`, the Python branch, which is
  also the path of `gross_match: none`); `scripts/tests/wave_fixture.py:343` (`FakeCycle.calibrate`); the fixture's
  default manifest is `"gross_match": "pm6-6"` (`wave_fixture.py:180`), so the wave suites run the Python mode by
  default; `test_research_wave.py:77-84` asserts the two functions directly and `:236-288` compares the engine mode
  with the Python mode; the G-P5 row `test_no_python_mirror.py:75-76` goes with them. Deleting the lines alone turns
  those suites red; retiring the mode means removing `pm6-6` from `GROSS_MATCH`, a new `none` path, a new default
  manifest and rewritten wave tests: a designed change (Ruling C2-10: "root does not hand-write designed checks")
  inside files E2 is rewriting now (pool-17 fix round 3: `wave_stage_cell.py`, `wave_manifest.py`, the wave tests).
  The identity it waits for now holds (above), so C3 can do it with C2-9 and S14. **C2-9's plan-time refusal likewise
  goes to C3** (it depends on the deletion). No wave pytest re-run was needed (no commit).
- **(iii) S13:** at E2's merge (as briefed).
- **(iv) no P9 reference under the `rel` preset: holds** (`m2e-logs/c2-rel-preset-scan.txt`,
  `m2e-logs/tools/rel_preset_scan.py`). pool-2's 105 build receipts: 74 equity-dev, 10 equity-rel, 21 without a
  Preset field (pre-P9 `mega-build.ps1` era: v5 / v6 / t-series / sha-o2, which built `build-equity` only); none names
  `rel` or `rel-avx2`. 643 bounded-run receipts: every executable is under `build-equity\bin`, `build-equity-rel\bin`,
  the two M1b reference copies under `build-equity\`, or Python; none under `build-rel` / `build-rel-avx2`. Neither
  tree exists in pool-2, and no `build-rel*` exists in any `C:/atx-wt/pool-*` or `C:/atx`.
- **(v) SQL2-CLS: no commit** (C2 adds no `atx.<name>/v<n>` literal, as its report says). Guard
  `atx-engine/tools/test_research_store_classes.py` on the merged head **5 passed, 92 subtests**. Fixture chain
  (`atx-research-store` p9-1u `3b955ba1`, rel and abs `--root`, `m2e-logs/c2-fixture-chain.log`,
  `m2e-logs/tools/fixture_chain.sh`): `files_seen 28 verified 25 declared 0 skipped 4`, **`catalog_digest
  d32f7655...4e69`**, `digest` same, `verify --pins` 0, identity total checked 12, mismatches 0.
- **Not run (by design):** the real-data engine wave (see the list). C2-4 was the lane's (C2-4b). The ledger
  candidates 1-4 of the C2 report are the PM's to append.
- **DISK-2 (pool-15):** nothing to delete: the lane had deleted its object trees (`build-equity/` 3.2 MB = receipts,
  logs, `audits/`, `v8-interim3-pitch-render-run2/`; `deps/` empty; no `build-equity-rel/`). Mine: P1 and P2 outputs
  (`build-equity/p9-m2e-c2-x5-{cal,gm}`, 11 MB each; P2 is a byte copy of the reference but `producer`) are deleted by
  name after this commit, receipts kept; E (`p9-m2e-c2-x5-engine`, 11 MB, the first engine-calibrated X-5 NAV) is kept.
- **Trial ledger:** 0 trials (133 lines, `27e40f9f`).

### AL-COMB (`efb7ab2e`, pool-19, base `1239a5ff` + D2 `49af0a52` merged in; re-review 1 APPROVE `098e6ccd`)

#### Expected substitution list for the AL-COMB flag-absent X-5 w identity (written before either run)

The re-review's item 5 (the lane could not run it: X-5's role inputs are not in pool-19). Real data, 0 trials;
compared by file SHA-256 and JSON leaf path only (`m2c-logs/tools/cmp.py`), nothing printed. Both runs = X-5's w argv
(`build-equity/mega-v8xw-train-theme-erc-run1/receipt.json`: library `fund_industry_ic_v8x3b.json` `32f8d69f`, role
lo3 `e1c67101`, fields v13, X-5's `composition_weights.json` `8310da2c`, candidate cache `mega-candidate-cache-v8-lo3`,
`--save-combined`) with only `--output` changed, through `run_bounded_research.py`, Debug, `--seconds 600
--max-rss-mib 2048` (RUN-1; the w pass peaked at 1,372-1,436 MiB before):

- `p9-m2e-alcomb-x5-w-pre`: the IC exe in `build-equity/bin` now = p9-2a `ba1232ed` (root before C2 and AL-COMB);
- `p9-m2e-alcomb-x5-w-post`: the p9-2f IC exe (C2 + AL-COMB merged).

Expected: the same file set; every file byte-identical except `summary.json` and `train_candidates.jsonl`, which may
differ only at timing leaves (`.../stage_seconds/*`, `.../wall_seconds`, `/roles/*/hash_seconds`), the M2c precedent's
classes. No `composition_hedge_trace` / `composition_schedule_trace` key (neither block is present). Anything else is a
stop, to be attributed to C2 (`stats_ext.hpp`; run-time norm_ppf values claimed unchanged) or AL-COMB.

#### Expected substitution list for the AL-COMB flag-absent fitter identity (written before the run)

Re-review item 5, second half, on the fitter's synthetic fixture (M2d's `m2d-logs/tools/d2_flag_absent_fixture.py`:
`aim_world`, two themes, registry absent; 9 cases std-erc, std-erc-tsmom, shrink, shrink-erc, shrink-tsmom, std,
std-aim-resid, the R-11 child of std, ew-theme), neither `--theme-hedge` nor `--mom-volman`, 0 trials. Old = the
tools of `a25453be` (root after C2, before AL-COMB; `git archive` of `atx-impl/tools`, `atx-engine/tools`,
`research_window.json`); new = the merged head's. Expected, per case: the same exit code and file set;
`admission.csv` identical; `admission.json` differs only in the value of `$.inputs.producer_fingerprint`;
`composition_weights.json` only in the values of `$.provenance.producer_fingerprint` and
`$.provenance.admission_sha256`, plus, in the R-11 child only, `$.provenance.resid.parent_weights_sha256` (its parent
file moved). Every `$.provenance.<block>.producer_fingerprint` unchanged. Anything else is a stop.
