# Whole-wave review, waves 2 + 3: area store / catalog (GATE-1 step 8; RESUME 11)

- **Range:** BASE `3c9eeb01` (wave-1 gate head) .. HEAD `d51d011e` (root after M6), branch
  `feat/platform-p9-20261003`, `C:/atx-wt/pool-2`. SQL4 (pool-32) is not in the range. If it merges at M6b, an
  addendum follows.
- **Lanes:** SQL1 (wrapper, store library, Python accessor and record-store backend; wave 2; its N1 fix was root's,
  `8d4cc52c`), SQL2 (catalog, classes, guard, identity checker; wave 2), SQL3 (hook, `store` block, W3-4 seal
  narrowing, item 5, SQL3-WQ; wave 3), and the root's SQL2-CLS commits.
- **Reviewer:** fresh, read-only in pool-2. Code was read by SHA only. Nothing was built and nothing was committed.
- **Verdict:** **CHANGES_REQUIRED**. There is 1 Required finding (M-1). Root can fix it as a slip before any cell: one
  `classes.json` commit plus a one-line edit of the guard test. There are 9 Suggested findings and 0 Incorrect
  findings.
- **Judged:**
  1. The hook. It never changes a writer's exit code or a JSON byte, it ingests exactly once per write, and with the
     catalog off it starts no subprocess. **Holds.**
  2. Digest reproducibility, `verify --pins` and the SQL2-CLS classes. Reproducibility **holds**, and `verify --pins`
     **holds** with two semantic notes. Guard coverage: **does not hold** (M-1).
  3. sqlite usage. **Holds.**
  4. House style. **Holds**, with duplicated plumbing and Python-copy notes.
  5. No expected hash, golden or pinned value was edited. **Holds.**

## Files reviewed (73; +14,645 / -27 lines)

These files are `git diff --name-only 3c9eeb01..d51d011e` filtered to the area. The 44 files under
`atx-engine/tests/fixtures/research_store/` (+4,090 lines: goldens, schema fixtures, the synthetic tree, builders)
are **claimed by the fields area** (precedence 1) and are only cited here. For them I checked one thing: each golden
was created once and never edited (see (5)).

| group | files (added / removed lines) |
|---|---|
| atx-core db (SQL1 + N1) | `include/atx/core/db/connection.hpp` +115, `include/atx/core/db/sqlite.hpp` +38/-7, `src/db/connection.cpp` +248, `src/db/sqlite.cpp` +161/-16, `tests/db_connection_test.cpp` +256, `tests/db_sqlite_test.cpp` +197 |
| store library (SQL1) | `include/.../research/store/{digest,ops_cache,ops_core,rows_cache,rows_core,store}.hpp` +75 +57 +77 +69 +83 +164; `src/research/store/{cache_ops,core_ops,digest,store}.cpp` +54 +55 +154 +367; `src/research/store/{tables_cache,tables_common,tables_core}.hpp` +70 +20 +79; `src/research/store/detail/{table,table_ops}.hpp` +236 +632 |
| catalog (SQL2, SQL3 W3-4) | `include/.../store/catalog/{catalog,classify,ingest,ops_records,pins,query,rows_records,seal_guard,store_cli}.hpp` +112 +81 +87 +66 +43 +69 +265 +76 +70; `src/research/store/catalog/{cache_index,catalog,classify,ingest_build,ingest_cycle,ingest_fields,ingest_ledger,ingest_run,ingest_spec,ingest_stage,ingest_wave,json_text,pins,query,records_ops,seal_guard,store_cli,store_main}.cpp` +283 +806 +275 +70 +86 +181 +146 +142 +171 +48 +128 +512 +306 +305 +76 +228 +595 +29; `catalog_detail.hpp` +171, `tables_records.hpp` +280 |
| registry | `atx-engine/schemas/research_store/classes.json` +101 (14 commits: SQL2 + root SQL2-CLS) |
| gtests | `atx-engine/tests/research/research_{catalog_cli,catalog_seal,catalog}_test.cpp` +260 +190 +464, `research_catalog_test_support.hpp` +163, `research_store_{digest,open,schema,table}_test.cpp` +170 +379 +89 +273, `research_store_test_support.hpp` +212; `atx-impl/tests/strategy_cov_store_test.cpp` +427 (lane COV's risk-container store: no sqlite; read for style and goldens only) |
| Python | `atx-engine/tools/{record_store,research_store,research_store_identity}.py` +189/-1 +268 +384; `atx-engine/tools/test_{record_store_sqlite,research_store,research_store_blind,research_store_classes,research_store_fixtures,research_store_identity}.py` +217 +204 +68 +215 +156 +521; `atx-impl/tools/test_fit_composition_weights_store.py` +11/-3 (D2's CM-5 test update); `scripts/research_store_hook.py` +211; `scripts/tests/test_research_store_hook.py` +559 |

The hook's call sites belong to the Python area (`run_bounded_research.py:491-494, 562`, `stage_chain.py:128-135,
282, 286`, `research_wave.py:41`, `wave_queue.py:146, 350`, `cycle_resume.py:193`, `cycle_verdict.py:183, 196`,
`wave_stage_record.py:348`). I traced them for judgment (1); I did not review those files as a whole.

## Required

| id | file:line | failing input | fix |
|---|---|---|---|
| **M-1** (missing) | `atx-engine/tools/test_research_store_classes.py:26` (`PY_DIRS = ("scripts", "atx-engine/tools", "atx-impl/tools")`), against `sql-design.md:458` (§3.9: "every `atx.<name>/v<n>` literal written by Python or C++ under `scripts/`, `atx-engine/`, `atx-impl/`") and plan §0.4 G-P10 (e) (`docs/plans/2026-10-03-p9-sprint-plan.md:114`: "every `atx.*/vN` record schema written by Python or C++ is registered") | See "M-1 detail" below. | See "M-1 detail" below. |

**M-1 detail.**

- **Failing input.** `atx-impl/strategies/generate_from_spec.py:39` defines
  `RECEIPT_SCHEMA = "atx.library-spec-receipt/v1"`, and `:174` writes it as `<out>/receipt.json`. A live Python
  writer under `atx-impl/` therefore stamps a record schema that is in no class, no `domains` row and no
  `legacy_allow` row of `classes.json`, and the guard stays green.
- **Evidence.** I scanned the whole tree at `d51d011e`: 184 distinct literals. Outside tests, fixtures and docs, this
  is the only unregistered literal that a writer stamps.
- **Consequences.**
  - The catalog classes such a receipt as `other`: run-receipt's glob matches, but its schema list does not. The
    file is therefore artifact-only.
  - Its pins (`spec.sha256`, `generator.sha256_lf`, `registry_pins`) are never extracted. G-P10 (a) ("every pin of
    every ... receipt") silently misses them.
  - G-P10 (e) is PARTIAL in the merged tree, not "landed".
- **Fix (root slip, one SQL2-CLS commit).**
  1. Add `"atx-impl/strategies"` to `PY_DIRS`, so the guard covers what §3.9 says it covers.
  2. Register `atx.library-spec-receipt/v1`. For example, add an artifact-only class `library-spec-receipt`: glob
     `**/receipt.json`, after `run-start`, schemas `[atx.library-spec-receipt/v1]`. `shared_literal_problems` is
     unaffected because the literal is not shared. Alternatively, add a dated `legacy_allow` row with a reason.
  3. Re-run the guard and the store suite at seeds 0 and 1, and check that the fixture-chain digest stays
     `d32f7655...4e69`. No fixture file has this schema, so no classification moves.
  4. If root does not take the slip, it prints G-P10 (e) as PARTIAL and names the literal.
- **JSON inputs, for the record.** Three more unregistered literals occur only in git-tracked JSON *inputs* under
  `atx-impl/strategies/`: `atx.curated-corporate-action-evidence/v1` (7 files), `atx.selected-stock-transition-basis/v1`
  (2), `atx.dsl-strategy-experiment/v1` (1). No tree writer stamps them, so they fall outside (e)'s "written by"
  wording. The catalog classes them as `other`. P10, or rows in the same commit if root prefers.

## Suggested

| id | file:line | problem | fix / when |
|---|---|---|---|
| S-1 | `scripts/research_store_hook.py:158-159`; `scripts/tests/test_research_store_hook.py` | The hook's catch-all (`except Exception: warn(...)`) is the only guard of "never raises" against an *unexpected* exception, such as a locked or corrupt catalog in `rs.Store.open` or a failing `identity()` import. No test reaches it. My mutation M1 (catch-all re-raises) left all 13 hook tests green (SURVIVED, below). This matters most in `cycle_verdict.keep_copy` (`:172-186`), where a raising hook inside the `try ... except FileExistsError` would write an extra verdict copy. | Add one case that drives the real `check` against the empty catalog file `catalog_on` already plants (`StoreError` raised). Assert one warning, the writer's return value and bytes unchanged, and exit 0. P10, or root's next touch. The hook is OFF in every P9 run of record (SQL3-2). |
| S-2 | `atx-engine/src/research/store/catalog/catalog.cpp:174-186` (also `:511-530`, `:677-703`); `cache_index.cpp:56-58` | The comment says "symbolic links and junctions are left out". The check is `is_symlink()`. MSVC's STL reports an NTFS junction as `file_type::junction`, so `is_symlink()` is false for it, and `is_directory()` follows it. A junction inside the root would then be walked, and its target's files hashed or ingested under a root-relative name that hides the target's real (possibly dated) path. `expand_recursive` and `list_build_equity` keep no visited set, so a junction loop runs until path-length errors. No gtest plants a junction. This is unverified on this toolchain, since I could not build. | Skip every reparse point: test `symlink_status(ec).type()` for anything that is not `regular` or `directory`. Add a gtest with a junction (`mklink /J`). P10. Today's exposure is nil: a names-only check found 0 links under pool-2's `build-equity/` and `build-equity-rel/`, and `dir /AL /S` reports none under `build-equity/`. Root re-checks before step 7 (below). |
| S-3 | `catalog.cpp:576-579`, `tables_records.hpp:266-278` | `verify --pins` semantics. (i) A pin whose target is a seal-named path is never opened, so no artifact row exists and the pin reads **`missing`**, the same state as a file absent from disk or unreadable. `--strict` then refuses with "stale or missing". (ii) For a big file, `stale` can mean only that two manifests declare different SHA-256 values: the smallest holder's declaration becomes the artifact row, and nothing was hashed. The output line prints `catalogued <sha>` without saying it was declared. | Add a `sealed` state (join `skipped_path` `seal-name`), and print `sha_source` on problem lines. P10. For the gate print, see "What root must do" 3. |
| S-4 | `catalog.cpp:562-568` (`roots_json`); `store_cli.cpp:187-204` | The catalog digest depends on classification, which comes from `classes.json`. The `catalog_run` row records specs, ledgers and includes, but not which registry or its SHA-256. The fallback `ATX_RESEARCH_STORE_CLASSES` is the build tree's absolute source path (`atx-engine/CMakeLists.txt:429-430`), so it reads that working tree, not a commit. | Record the registry path and SHA-256 in `catalog_run` (the row is volatile, so the digest is unaffected). P10. Until then, see "What root must do" 3. |
| S-5 | `atx-engine/tools/research_store_identity.py:50, 135-141` (seal regex and `path_key`, the same as `seal_guard.cpp` `has_sealed_year` / `path_key`), `:74-92, 152-160` (`TYPED`, `_where`: copies of `ingest_*.cpp` key-to-column maps and family keys) | These are Python copies of C++ rules, added in the range with no `test_no_python_mirror.py` row. The precedent is the ruled `require_research_seal` row, `test_no_python_mirror.py:71-74`. They are not G-P5 research rules, and drift fails loud (a mismatch, or a refusal to open). | A ruled allowlist row, or P10: C++ `schema --json` prints the doc-key map and the seal rule is asked of C++. |
| S-6 | `scripts/research_store_hook.py:63-68`; `catalog.cpp:213-225`; `store.cpp:86-93, 166-174` | The code has duplicated plumbing. (1) `sha256_file` is a byte-for-byte copy of `scripts/research_common.py:34-39`, which is E2's "one copy". (2) `utc_now()` is hand-rolled beside `atx/core/datetime.hpp:312` `to_iso8601`. (3) There are three UTF-8 path helpers: `store.cpp` `path_from_utf8` / `utf8`, `seal_guard.cpp` `fs_path` / `utf8_path`, and the inline conversion in `connection.cpp:47-48`. | One helper each, in atx-core for the path and time helpers. P10, on the centralization list. Run ids are volatile, so the digest is unaffected. |
| S-7 | `atx-core/src/db/connection.cpp:44-57` (root's SQL1-N1 fix `8d4cc52c`); `atx-core/tests/db_connection_test.cpp:161-175` | The N1 fix is correct by reading: `absolute()`, then `weakly_canonical`, then `root_name`, then `GetDriveTypeW`, so a relative name on a mapped drive is refused. The added test can only show that a relative name still opens locally. The host has no `DRIVE_REMOTE` drive, so a mutation that drops `absolute()` stays green. | Add a drive-type probe seam and a gtest: relative path on a "remote" drive refused. P10. The fix itself is reviewed and holds. This is the "wave-2 whole-wave review covers it" item of SQL1-N1. |
| S-8 | `atx-engine/src/research/store/catalog/store_main.cpp:17-28` | `main(int, char**)` takes ANSI argv and has no top-level catch. An exception, for example `std::filesystem::path` built from non-UTF-8 argv bytes under MSVC, aborts. MSVC `abort()` exits with code 3, which is the CLI's documented refusal code (the hook would print "ingest exit 3"). | Use `wmain` (or a UTF-8 manifest) plus a catch-all that returns 4. P10. Every path on this host is ASCII. |
| S-9 (nit) | `atx-engine/tools/test_research_store_identity.py:52` | 122 columns. It is the only over-length line among the area's added lines. All 60 C++ files are within 100 columns, with no tabs. | When the file is next touched. |

**Carried, not re-raised** (already ruled P10 or root's):

- SQL3 review 1: S2 / S3 (`seal_guard.cpp` F-2 seam, bound exhaustion), S4 (stale `catalog.hpp:27-28`, still
  stale at HEAD), S7 (no raw-bytes check with the catalog on), S8 (`indexes()` globs under seal-named children).
- SQL3 review 2: S-1, S-2 (the stage record's side files are not hooked); ruling SQL3-S2.
- SQL1's parked minors 9 / 11.

## Judgment detail

**(1) The hook. Holds, from my reading of every call site plus the tests.**

- **Exit code and bytes.** Every call is the statement right after its file is closed:
  - `write_text` / `with ... open("x")`;
  - the runner's `start.json` at `:493-494`, and `receipt.json` inside `finally` after its `with` closes, at
    `:559-562`;
  - the chain's `write`, then `written` (`stage_chain.py:282, 286`).
- `record` never writes to the file. It runs `ingest` (C++ reads the file through `ifstream`, and
  `subprocess.run(..., timeout=60)` finishes before the call returns) and then `check`, which opens the catalog
  through a context manager and reads the file with `read_bytes()`.
- Every hook path catches `Exception`: `record`, `selection` and `catalog_of`. On top of that, the stage chain's
  `written` catches `Exception` a second time. Untested: S-1.
- Expressions evaluated outside the hook's `try` (`cycle.res.root`, `w.path(path)`, `Path(d or QUEUE_DIR) / p.name`,
  `a.output`) are values each writer has already used. This matches review 2's table.
- The runner's return value (`:564`) depends only on `outcome`.
- The `store` key is present only when `selection` is non-None. It is last in `start.json`
  (`dict(receipt, store=store)`) and last in `receipt.json` (set after `logs`). The GOLDEN pre-change bytes test
  (`test_runner_without_catalog_or_index_writes_todays_bytes`) proves absence means today's keys.
- **One ingest per write.** There is one call per write at each of the 10 call sites. `write_verdict` ingests the
  verdict and `keep_copy` ingests each copy, so nothing is ingested twice. `test_one_ingest_per_write_in_a_fake_wave`
  pins the exact (path, class) list. In review 2, teeth R1 (a doubled call) and R2 (a moved call) are caught.
- **Catalog off means no subprocess.** `record` returns at `catalog is None`, before `ingest` or `identity()`.
  `test_hook_off_spawns_and_imports_nothing` poisons `subprocess` in a fresh interpreter (passed in my run).
- **SQL3-2.** The hook's on-switch is only the default `build-equity/research-store/catalog.sqlite` plus the exe.
  That path is absent in pool-2 before and after my runs. The runner records a block only for a catalog or an
  `index.sqlite` reachable from argv directories. A names-only search found 0 `*.sqlite` files under pool-2's
  `build-equity/` and `build-equity-rel/` (seal-named directories pruned).

**(2) Digest, pins and classes.**

- **Digest reproducibility.**
  - The design's claim is "a run over the same tree from scratch, in any walk order, reproduces it"
    (`catalog.hpp:22-26`).
  - Proofs on the fixture: the gtests `ResearchCatalog.DigestReproducibleFromScratch` (a re-run into the same store,
    and a fresh store at `segment_files = 1`) and `ResearchCatalog.DigestIndependentOfIngestOrder` (`reverse_walk`,
    segment 3). Both passed in root's Debug `p9-2r` and Release `p9-2s` runs, 28 / 28
    (`m1-logs/dbg-gt-catalog.log`, `rel-gt-catalog.log`; the log is the source, I did not run them).
  - Volatile tables `catalog_run`, `artifact_seen` and `skipped_path` stay outside the digest
    (`tables_core.hpp:24-62`).
  - The digest is read in one read transaction (`store.cpp:357-364`).
  - My own evidence: two fresh catalogs of the `d51d011e` fixture tree, at a root path root never used (my scratch
    archive), with pool-2's store exe `d8a500d8`. Both give `catalog_digest d32f7655...4e69`, the recorded value,
    with `files_seen 28 verified 25 declared 0 skipped 4`.
  - The real tree (G-P10 (c)) is the gate's step 7.
- **`verify --pins`** (`store_cli.cpp:346-391`; view `tables_records.hpp:271-278`).
  - States in precedence order: `unresolved` (no target, a digest pin, or an uncatalogued field-source), `missing`,
    `stale`, `declared`, `ok`.
  - Counts are printed by state and by kind. `--strict` exits 3 only for stale or missing pins, and the counts are
    printed first.
  - Holder pins are rewritten on every ingest (`pins.cpp:289-301`, delete then insert).
  - My fixture run: exit 0; `ok 14`, `declared 0`, `stale 0`, `missing 5` (four `.f64` payloads the fixture leaves
    out, plus one deliberately absent input), `unresolved 10`.
  - Semantic notes: S-3.
- **SQL2-CLS.**
  - Across the range, no literal row was removed from `classes.json`, and no existing class's globs, ingest, render,
    format, stage or seed globs changed; the class order is kept.
  - Six classes were added: blocked-registration, diagnostic-spec, eval-output, ledger-head, od3-drop-list,
    rule-registration.
  - The guard is green over its six directories (5 passed).
  - The guard's scope is narrower than its spec: M-1.

**(3) sqlite. Holds.**

- **Single writer.** Every write goes through `BEGIN IMMEDIATE` (`connection.hpp:79-113` `with_immediate`: 30 s busy
  handler, then 3 x 50 ms retries, rollback per attempt). Create and migrate re-read both pragmas under the write lock
  (`store.cpp:231-267`), so two openers never create the schema twice. Python `transaction()` is also
  `BEGIN IMMEDIATE`, with autocommit otherwise (`research_store.py:167, 222-238`).
- **Lock order.** No code path holds transactions on two stores at once. The CLI opens one store per verb. The hook's
  Python read starts only after the C++ ingest has exited. `RecordStore` keeps one connection per thread per index
  file and opens one transaction per put. sql-design §3.3 and `connection.hpp` document this. A per-process
  "one store transaction at a time" sentence would make it explicit (folded into S-6's P10 pass).
- **Forks.** None. Windows spawn; SQLite handles are not inheritable. `RecordStore`'s thread-local connections close
  with `close_indexes()` or at thread end. The hook opens and closes per check.
- **WAL and journal.** WAL is set on a new file after `page_size` and must report "wal". `synchronous` is FULL for
  the catalog and NORMAL for caches. Defensive mode is on and trusted_schema off. `wal_checkpoint(TRUNCATE)` runs at
  the end of a catalog run and before `--rebuild` deletes the files (`connection.cpp:126-227`,
  `store_cli.cpp:206-228`).
- **Paths.** UNC paths and network drives are refused after `absolute()` (N1). The Python accessor refuses UNC only;
  a network drive is C++'s refusal (minor, P10). `root_relative` refuses `..` and other roots (`seal_guard.cpp:183-206`).
  The hook resolves both forms. The SQL3-WQ precedent holds at every call site: review 2's table, which I
  re-checked.
- **W3-4.** The root seal check scans only below the outermost `.git` (`seal_guard.cpp:61-79, 162-176`). On a file
  system error it scans the whole path.

**(4) House style.**

- `/W4 /WX`, from the receipts and logs: p9-1t, p9-1u, p9-2r and p9-2s all have 0 warning lines
  (`m1-logs/builds.txt`, `handoff-root-m2a.md:13-28`).
- All 60 area C++ files are within 100 columns.
- Templates sit only in private `src/` headers, and there is no PCH.
- The `[[nodiscard]]` / `Result` discipline holds. Discarded steps carry a comment (`(void)step; // a DELETE returns
  no rows`).
- Loops carry stated bounds, with S-2's junction caveat.
- The store library links only `atx::core`, and the catalog library links only the store library
  (`atx-engine/CMakeLists.txt:338-438`).
- One generic Python accessor serves every Python reader.
- No new G-P5-class rule mirror. S-5 lists the infrastructure copies.

**(5) No golden or pin edited. Holds.**

- `golden_digests.json`, `golden_rows.json`, `py_created.sqlite` and the four `schema/*.json` fixtures were each
  touched by one commit only (their creation).
- Three test edits in the range changed expected values, each a ruled fix:
  - `research_catalog_test.cpp` `files_seen 31 -> 30` (SQL2-FIX1 S5, `bedd090a`, a counting bug fixed);
  - `research_store_digest_test.cpp`, where SQL1-FIX1 (`1a5b051d`) tightened `size() == 64` to the exact vector;
  - the hook and identity test counts after item 5 added the wave-result ingest (`b55c0d11`; the runner GOLDEN is
    unchanged).
- The fixture-chain digest `d32f7655...4e69` reproduces at HEAD (my runs above).

## Tests run (mine; seed 0; nothing written under pool-2)

**How the tests were run.**

- Tree: a `git archive d51d011e` of `scripts`, `atx-engine/{tools,schemas,tests/fixtures/research_store}`,
  `atx-impl/{tools,strategies}` and `.gitattributes`, extracted into my scratch at `.../scratchpad/wave-store/tree`.
  This puts every store under my scratch: no `build-equity/research-store/` anywhere in pool-2 (SQL3-2).
- Command: `C:/Program Files/Python312/python.exe -m pytest -p no:cacheprovider --basetemp <scratch>/bt/<name>`.
- Environment: `PYTHONDONTWRITEBYTECODE=1`, `PYTHONHASHSEED=0`, `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`,
  `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, `ATX_RESEARCH_STORE_EXE=<that bin>/atx-research-store.exe`,
  and `TMP` / `TEMP` set to my scratch.

**Exe SHA-256, equal before and after every run:**

| exe | SHA-256 | build |
|---|---|---|
| `atx-research-store.exe` | `d8a500d84dc2a25130c69b226d8f5deec47b8dabb6256522c83c00764d3d9d65` | `mega-p9-2r-receipt.json`, source `fd084e78` |
| `atx-engine-research-catalog-tests.exe` | `f1134aaa...78ed` | p9-2r; not run |
| `atx-research-eval.exe` | `47910191...893b` | hashed only |
| `atx-equity-strategy-targets.exe` | `7e101b56...f0dc` | hashed only |
| `atx-equity-strategy-ic.exe` | `ef3974cf...9b38` | hashed only |

The store exe is current by source: `git diff fd084e78..d51d011e` touches no atx-core file and no store or catalog
source, and the `atx-engine/CMakeLists.txt` changes add other targets' sources only.

**Results:**

| run | result |
|---|---|
| store suite: `atx-engine/tools/test_research_store{,_blind,_classes,_fixtures,_identity}.py` | **43 passed**, 136 subtests passed, 0 skipped (`FixtureChain` ran the real exe: 12 checked, 0 mismatches) |
| `scripts/tests/test_research_store_hook.py` | **13 passed** (`test_real_store_exe_ingests_every_class` PASSED) |
| `atx-engine/tools/test_record_store_sqlite.py` | **9 passed** |
| `atx-impl/tools/test_fit_composition_weights_store.py` | **5 passed** |
| store exe on the HEAD fixture tree, two fresh catalogs | both exit 0, `d32f7655...4e69` twice; `verify --pins` exit 0 (counts above) |
| names-only checks in pool-2 | `build-equity/research-store/` absent (before and after); 0 `*.sqlite` and 0 links under `build-equity/` and `build-equity-rel/` (`find`, seal-named directories pruned); `dir /AL /S build-equity` reports none |
| whole-tree literal scan at `d51d011e` (`git grep`) | 184 distinct literals; M-1 |

Total: **70 pytest passed, 0 failed**. ENV-1: 0 hits.

The C++ claims come only from the lanes' and root's logs and receipts: `m1-logs/dbg-gt-catalog.log`,
`rel-gt-catalog.log`, `builds.txt`, `sql2cls-fixture-chain.log`, `handoff-root-m2a.md:9-28`, and the SQL3 reviews in
pool-13.

## Mutation (mine; TEETH-RESTORE, TEETH-CRLF form; on the scratch archive, never in a pool)

- **Tool.** `.../scratchpad/wave-store/teeth_hook.py`.
  - Precondition: `git -C pool-2 hash-object --path scripts/research_store_hook.py <archive file>` equals
    `git rev-parse d51d011e:scripts/research_store_hook.py` (`ffc532a7...`).
  - The tool keeps the bytes in memory and as a copy, and restores them in `finally`.
- **M1.** `record`'s catch-all `warn(cls, rel, ...)` is replaced by `raise`.
- **Result.** The hook file ran **13 passed: SURVIVED** (S-1).
- **Restore.** The raw SHA-256 `4daa7f42...c31c` equals the kept copy, and the hash-object equals HEAD's blob.
- **Pool-2.** Porcelain unchanged. It shows only the PM's `progress.md` / `r11-dispatch.md` and the five PNGs, as
  before.

## What root must do before any cell

1. **M-1 slip** (one commit, `classes.json` plus `test_research_store_classes.py:26`).
   - Then run the guard and the store suite at seeds 0 and 1.
   - The fixture chain must stay `d32f7655...4e69`.
   - Otherwise, print G-P10 (e) as PARTIAL and name `atx.library-spec-receipt/v1`.
2. **Step 7 catalog (M2A-4, SQL3-2).**
   - Use a non-default `--catalog` path, and state it in the gate report.
   - Run it as a bounded run (RUN-1 / RUN-2). A timed-out run is partial, because its committed segments stay.
     Print it as partial and never reuse its file.
   - Before the run, list reparse points under the root, names only (`dir /AL /S`). Exclude `.git`, and do not
     descend into seal-named directories. Any junction must be resolved before the run (S-2).
3. **Step 7 readings.**
   - The second catalog must be a **fresh file** (a new path, or `--rebuild`), not a re-run into the first: a re-run
     upserts and keeps the rows of vanished files.
   - Pass `--classes <root>/atx-engine/schemas/research_store/classes.json` and record its SHA-256 in the report
     (S-4).
   - In `verify --pins`, read `missing` as "no artifact row": absent, seal-named (never opened) or unreadable. Before
     calling a pin missing, check `query stale-pins` targets against `skipped_path` `seal-name` rows (S-3 (i)).
   - Read `stale` on a `declared` artifact as two manifests disagreeing (S-3 (ii)).
   - The render checker is blind (digests and offsets only): `research_store_identity.py --catalog DB --root <root>`.
4. **X-5 opt-in cache identity (G-P10 (d)).**
   - Create `index.sqlite` only in a **scratch copy** of the X-5 fit work dir, never in a work dir that any reference
     or cell names. A record store selects the backend by file presence, `d/index.sqlite` or `d/../index.sqlite`
     (`record_store.py` `RecordStore.__init__`). SQL-11 keeps every cell off the index, and
     `book_monitor --fit-work` refuses an indexed dir.
   - After the identity, delete the index by literal path.
   - Then, before the first cell, re-run the names-only checks: no `index.sqlite` under `build-equity*/`;
     `build-equity/research-store/` absent; 0 receipts carrying `atx.run-store/v1`.
5. **Step 3 identity with the hook off.** Use root's F-1 filter for console bytes (`m1-logs/tools/fakewave_f1.py`):
   git's "warning: in the working copy of" lines (SQL3 review 1 S1, review 2 S-4).
6. **Print G-P10** from the table below, not from the lane report (SQL3-S2 S-3).
7. **Exes.** Step 1 rebuilds the store exe. The exe step 7 uses is named by its receipt. p9-2r `d8a500d8` is current
   by source at `d51d011e`, but the gate's own build supersedes it (EXE-CUR).

## G-P10 (a)-(e) from the merged tree at `d51d011e` (printed, gates nothing, SQL-8)

| item | status | evidence |
|---|---|---|
| (a) every pin indexed; `verify --pins` counts by state | **PARTIAL** | Landed: the pin extractors for every holder class (`pins.cpp`; `pins.hpp:6-24`), the five-state view, and `verify --pins [--spec] [--strict]` (`store_cli.cpp:346-391`). Gtest `ResearchCatalog.PinsResolvedAndStaleDetected` (`m1-logs/dbg-gt-catalog.log`); my fixture run printed ok 14 / declared 0 / stale 0 / missing 5 / unresolved 10. Not landed: the real-tree counts (gate step 7); the library-spec receipts' pins (M-1); the seal-named `missing` conflation (S-3). |
| (b) render identity of receipt, start, stage receipt, binding, verdict, wave result, candidate | **PARTIAL** | Landed: the checker for all 8 rendered classes (`research_store_identity.py`; `rendered_classes` asserted in `test_research_store_identity.py:495-498`); the fixture chain at 12 checked / 0 mismatches (my `FixtureChain` run; `m1-logs/sql2cls-fixture-chain.log`); the write-time check in the hook for all seven writer classes (item 5 merged at `d51d011e`; hook file 13 / 13 with the real exe). Not landed: the real-tree mismatch count (gate step 7). The hook is OFF for every P9 run of record (SQL3-2). |
| (c) two from-scratch catalog digests | **PARTIAL** | Landed on the fixture: `DigestReproducibleFromScratch` and `DigestIndependentOfIngestOrder` (28 / 28 Debug and Release, `m1-logs/{dbg,rel}-gt-catalog.log`); root's rel = abs `d32f7655...4e69`; my two fresh catalogs at HEAD give `d32f7655...4e69`. Not landed: the real-tree pair (gate step 7, "What root must do" 3). |
| (d) caches | **PARTIAL** | The record-store SQLite backend landed (SQL1: `record_store.py`; `test_record_store_sqlite.py` 9 passed). The SQL-6 recording landed (the `store` block, last key, absent = today's bytes; hook tests). "Three writers hooked, the stage record's side files not" (SQL3-S2 S-2: `wave_stage_record.py:349-364`). X-5 fit / card identity with no index, a cold index and a warm index: root's at the gate (M2A-4; not run; "What root must do" 4). IC `--candidate-cache-index sqlite`: SQL4, not in the range, so **P10** unless M6b merges it. SQL-11 holds. |
| (e) every `atx.*/vN` record schema written by Python or C++ registered | **PARTIAL** (landed after the M-1 slip) | `atx.run-store/v1` is registered on the run-receipt and run-start rows (`fd084e78`). The guard is green over its six directories (5 passed, my run). Unregistered: `atx.library-spec-receipt/v1`, written by `atx-impl/strategies/generate_from_spec.py:39, 174`, outside the guard's `PY_DIRS` (M-1). |

## Open questions

1. M-1: should root take the slip (guard scope plus a class row) or print (e) as PARTIAL? I recommend the slip: it
   is two lines, Python and JSON only, and builds nothing.
2. Should the three JSON-input literals under `atx-impl/strategies/` get class rows now, or in P10? Today they class
   as `other`.
