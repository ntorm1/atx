# Lane SQL4 report (IC cache indexes on SQLite behind explicit flags; the trial-ledger index head)

## Outcome

DONE_WITH_CONCERNS. Brief tasks 1-7 are coded, compiled clean (`/W4 /WX`) and tested. Results:

- IC named filter: 79 / 79 on the merged head.
- `ResearchCatalog.*`: 17 / 17.
- The three source tripwires pass. Their test sources are unchanged.
- Teeth: one build, 13 mutations applied at once, 9 named tests red, restore verified.

The concerns are listed under "Open risks": M8 is unattributed in the one teeth build, the SQL4-Q2 deviation, and the
SQL4-Q1 P10 item.

## Branch / SHA

- Branch: `feat/p9-sql4-20261008` in `C:/atx-wt/pool-32`.
- Code head: `c5df575d`. This is the second root merge. The last lane code commit is `44fce688`.
- Records head: the commit that adds this report.
- All work is committed. The tree is clean: `git status --porcelain` is empty.
- Root merged: `475f5d8f`, at `c5df575d`. I re-checked `git -C C:/atx-wt/pool-2 log --format=%h -1` before writing
  this report and it still printed `475f5d8f`.
- Review BASE = `git merge-base HEAD feat/platform-p9-20261003` = `475f5d8f`.

| sha | slice |
|---|---|
| `82756940` | merge root `e3b6c111` (RESUME 11 step 1). |
| `96b2b1e1` | build: the index TU in `atx-impl-strategy` and the store-library link. Tasks 1-4 (the STOP 10 WIP) compile with no source change. |
| `22e41583` | task 1 remainder (`--pair-cache-index`) and task 5 (pair cache on SQLite). |
| `237fc8a6` | task 6: the ledger index head from B2's `chain_head`, and `verify --ledger`. |
| `4aade941` | task 7: the 15 brief gtests and the CMake tails; four WIP lines reflowed to 100 columns. |
| `44fce688` | test fix: two range-for loops over a member of a temporary summary (a dangling reference in C++20). |
| `c5df575d` | merge root `475f5d8f`. Four CMake tail conflicts, resolved region by region: root's block kept verbatim, the lane's block appended after it. |
| `c4fe6aba`, `3734e7a6` | records: builds b, c, teeth tool, teeth d. |

## Frozen base / lease

- Lease: `pool-32`, from `lease-worktree.ps1 -Status`, read only:
  `LEASED run_id=p9-sql4-20261008 | agent=p9-sql4 | owner_kind=heartbeat | owner=alive | keeper_pid=11816 |
  branch=feat/p9-sql4-20261008 | acquired=2026-10-08T23:18:37.7467716Z`.
- The lease is untouched (LEASE-DEAD rule) and was not released.
- The lane started RESUME 11 at `73a871fb`, with the STOP 10 WIP at `a1955a71`.

## Acquisition receipt

The lease was acquired in R8 by the PM's tooling. No acquire or release ran in this lane. The `-Status` line above is
the only lease evidence.

## Files changed (vs BASE `475f5d8f`)

**atx-impl**

- New:
  - `src/strategy_ic_cache_index.{hpp,cpp}`: one index object per cache root.
    - One connection under a leaf mutex. The lock order is documented at `hpp:16`.
    - `PRAGMA cache_size` is 8 MiB.
    - Failure policy (`hpp:26`): a transient error on a read is a miss plus a progress line; on a write, a skipped
      store plus a progress line.
    - Read-only mode for `--plan-only`.
  - `tests/strategy_ic_cache_index_test.cpp`.
- Changed:
  - `src/strategy_ic_signal_cache.cpp` and `src/strategy_ic_result_cache.cpp`: row first, then the legacy file,
    verified as today and imported.
  - `src/strategy_ic_detail.hpp`: a `CacheIndex*` parameter on five cache functions.
  - `src/strategy_ic_runner.{hpp,cpp}`: config field, flag parse, refusals, one index per run, the `"index"` key.
  - `src/strategy_marginal_pair_cache.{hpp,cpp}`: the indexed open, import and store.
  - `src/strategy_marginal_ic.{hpp,cpp}`: config field, flag parse, refusals, the sqlite `pair_cache` block.
  - `CMakeLists.txt` and `tests/CMakeLists.txt`: tail blocks.

**atx-engine**

- `include/atx/engine/research/store/ops_cache.hpp` and `src/research/store/cache_ops.cpp`: keyed reads and
  `INSERT ... ON CONFLICT DO NOTHING` on the cache group. No schema change and no group version bump.
- `src/research/store/catalog/ingest_ledger.cpp`: `default_ledger_head()` bound to `ledger::chain_head`, and the new
  `verify_ledger_heads`.
- `include/atx/engine/research/store/catalog/ingest.hpp`: its declaration and three report structs.
- `src/research/store/catalog/store_cli.cpp`: `verify --ledger L` (repeatable, with or without `--pins`; exit 3 on
  failure).
- `include/.../catalog/store_cli.hpp`: usage comment.
- `CMakeLists.txt` and `tests/CMakeLists.txt`: tail blocks.
- New: `tests/research/research_catalog_ledger_test.cpp`.
- `tests/research/research_catalog_test.cpp:368`: `EXPECT_FALSE` becomes `EXPECT_TRUE`, because the seam is now
  bound.

**Records** (sprint dir, `git add -f`):

- `sql4-logs/`: configure logs, `r11-check-1..17`, builds a-d, gtest logs, receipts.
- `sql4-logs/tools/sql4_teeth.py` and `sql4-logs/tools/subst.py`: lane tools.
- `handoff-SQL4.md` and this report.

No product Python file and no pinned source list or SHA changed. No golden, expected value, recipe byte,
`vcpkg.json` or `CMakePresets.json` changed.

## Evidence

All builds are Debug, `equity-dev`, run through `scripts/research-build.ps1`. TEST-1 allows one build per slice; the
receipts are in `sql4-logs/receipt-p9-sql4-*.json`.

- Configure: `powershell scripts\atx-build.ps1 configure -Preset equity-dev` gave exit 0 four times. The last run
  (`r11-configure-equity-dev-4.log`) printed `atx-impl engine_git_sha: c5df575d70449e9d9eba48d4496e014e56346f27`.
- Single-TU checks: `powershell scripts\atx-build.ps1 -Preset equity-dev check <file>` succeeded with no warning for
  every new or changed TU. Logs: `r11-check-*.log`. Diagnostics:
  - check-10 printed "is its target configured?" because CMake had not regenerated yet. Check-14 then compiled the
    same file.
  - check-12 had one compile error: `std::string == json` had no operator. I fixed it with `.get<std::string>()`, and
    checks 13 and 15 compiled.
  - The first check of the test TU pulled 211 order-only dependency objects, including the shared
    `C:\atx-cache\deps\spdlog-build` objects (SPDLOG-RELINK hazard: other pools linking spdlog may relink).

| tag | targets | source | exit | wall | exe SHA-256 (8) |
|---|---|---|---|---|---|
| p9-sql4-a | equity-strategy-ic, impl-strategy-ic-tests | `4aade941` | 0 | 119.4 s | ic `cd8a7a02`, tests `06a6ef44` |
| p9-sql4-b | + research-store, engine-research-catalog-tests (the one build of these) | `44fce688` | 0 | 20.6 s | store `9e52cec1`, catalog-tests `cc0ed106`, ic `d9c10959`, tests `0d40d688` |
| p9-sql4-c | equity-strategy-ic, impl-strategy-ic-tests | `c5df575d` (merged) | 0 | 17.9 s | ic `ac7edadf`, tests `92f55688` |
| p9-sql4-d | teeth (TEETH binaries, never evidence) | `c4fe6aba` + 7 mutated files | 0 | 19.4 s | ic `fb99fc38`, tests `5c0559b3`, catalog-tests `96b17484` |

Named filters:

- `atx-impl-strategy-ic-tests.exe --gtest_filter=IcCacheIndex.*:IcResultIndex.*:MarginalPairIndex.*:StrategyIcRunner.*:MarginalIc.* --gtest_brief=1`:
  - a: 77 / 79. The two reds were the test's dangling-temporary bug, fixed in `44fce688`.
  - b: exit 0, `[==========] 79 tests from 5 test suites ran.` / `[  PASSED  ] 79 tests.`
    (`r11-gtest-p9-sql4-b-ic.log`).
  - c (merged head): exit 0, `[  PASSED  ] 79 tests.` (`r11-gtest-p9-sql4-c-ic.log`).
- `atx-engine-research-catalog-tests.exe --gtest_filter=ResearchCatalog.* --gtest_brief=1` (b):
  - exit 0, `[==========] 17 tests from 1 test suite ran.` / `[  PASSED  ] 17 tests.`
  - It passed again on the merged tree (`r11-gtest-p9-sql4-b-catalog-merged.log`). The second merge changed no source
    in the catalog closure; `classes.json`, which is read at run time, did change.
- Tripwires: `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`, `.IcSourcesPinnedToSemanticsVersion` and
  `MarginalIc.PairCacheSourcesPinned` are inside the filter and pass. `git diff --stat 82756940..HEAD --
  atx-impl/tests/strategy_ic_runner_test.cpp atx-impl/tests/strategy_marginal_ic_test.cpp` is empty.

Teeth:

- Command: `python sql4-logs/tools/sql4_teeth.py`. A `--check` run went first: all 13 anchors matched once and the
  tree was clean.
- The tool keeps the 7 files' bytes, applies 13 mutations, builds tag d, runs both filters, writes the bytes back, and
  asserts TEETH-CRLF:
  - porcelain is empty;
  - `git hash-object` equals the HEAD blob;
  - the raw SHA-256 equals the kept copy's.
- Log: `teeth-p9-sql4-d.log`. Its last line: "restored: git status --porcelain empty; every mutated file's
  hash-object == HEAD's blob and its raw SHA-256 == the kept copy's".

| mutation (disabled) | red test (failing line) |
|---|---|
| M1 sqlite without `DIR/index.sqlite` (IC dispatch) | `IcCacheIndex.FlagWithoutIndexRefused` (:359) |
| M2 sqlite with `--cache-report` (run_ic) | same test (:374-375) |
| M3 index flag without `--candidate-cache` | same test (:356-357) |
| M4 row key parts differ | `IcCacheIndex.KeyMismatchRefuses` (tamper 0, :498-501) |
| M5 row payload differs | same test (tamper 1, :498-501) |
| M6 IC-result self-hash | `IcResultIndex.TamperedRecordIsMiss` (:670) |
| M7 present pair with other bits | `MarginalPairIndex.ConflictingBitsRefused` (:778) |
| M8 key row of another key text | NOT ATTRIBUTED: M7's `ASSERT` ends the test before the key-text check (:784-787) |
| M9 sqlite without `DIR/index.sqlite` (marginal dispatch) | `MarginalPairIndex.FlagAbsentBlockUnchanged` (:904) |
| M10 recorded head outside every prefix | `ResearchCatalog.RecordedHeadsArePrefixHeads` (:184, :187) |
| M11 `ledger_state.lines` != rows | same test (:142) |
| S1 seam unbound | `ResearchCatalog.LedgerHeadFromResearchLedger` (:96-108), `.LedgerHeadSeamReceivesLinesInFileOrder` (:368) |
| S2 index opened by file presence | `IcCacheIndex.FlagAbsentKeepsFileLayoutAndSummary` (:318, :320) |

Every other named test stayed green (logs `teeth-p9-sql4-d-*.log`).

Lane flag-absent identity, on the synthetic fakes (the runner tests' D=480 x N=8 role, two candidates), in the gtests
above:

- `IcCacheIndex.FlagAbsentKeepsFileLayoutAndSummary`: no flag, with and without `index.sqlite`, cold and warm. Every
  exact output file is identical by SHA-256, the summary is identical outside the timings, and the cache listing
  (names and SHA-256) is identical. The index file keeps its bytes and no `-wal` or `-shm` file appears, so it was
  never opened.
- `IcCacheIndex.SummaryRecordsIndex`: with the flag, cold and warm, every exact output file is byte-identical to the
  uncached run. The summary is identical outside `candidate_cache` and the timings; `index: sqlite` appears, and only
  with the flag.
- `IcCacheIndex.RowIsTheCommitMarker`: the indexed cache dir holds 4 `.f64` files and 0 `.json` files.
- `MarginalPairIndex.FlagAbsentBlockUnchanged`: `marginal_ic.json` with `index.sqlite` present and no flag equals the
  shard run outside `stage_seconds`, and the index keeps its bytes. With the flag, cold and warm runs equal the shard
  run outside `inputs.pair_cache`, `stage_seconds` and `working_bytes`; the cold `working_bytes` equals the shard
  run's. Only `index.sqlite*` is written.

I did not run the Python `tiny_world` canary (`scripts/tests/test_cycle_e2e.py` live) in the lane, because pytest
suites are the gate's. Its goldens (u, fit, w, NAV digests) give the tiny-world flag-absent identity on the exe when
root runs it.

## How root verifies

Build and gtests are the brief's "Root verifies":

- Debug, then Release for the IC exe, with `-Targets
  "atx-equity-strategy-ic,atx-impl-strategy-ic-tests,atx-research-store,atx-engine-research-catalog-tests"`.
- The two filters above.
- pytest `atx-engine/tools` and `scripts/tests`. A reasoned claim, not tested in the lane:
  `test_research_cycle.py`'s `verb_cli_from_cpp` regex reads `key == "--x") cfg.member`. The new
  `--pair-cache-index` parse is written `key == "--pair-cache-index") {`, and its `run_marginal_ic` check sits after
  the "bounded config (needs" marker. So the pinned `VERB_OPTIONS` should not move.

Identity procedure. It needs 0 trials. Compare by SHA-256 and JSON paths only, and open no NAV stdout.

1. **X-5 u / w, no flag** (Debug and Release):
   - Use X-5's argv exactly as in R0-3, changing only `--output`. The w form is
     `m2e-logs/receipt-p9-m2e-alcomb-x5-w-post.json` `command`. The u form is the same with `--no-composition` in
     place of `--composition-weights F --composition-weights-sha256 S`.
   - Run once with the cache dir as is.
   - Run again after copying the cache dir and running `atx-research-store cache init <copy>` on it (DIR must exist),
     pointing `--candidate-cache` at the copy.
   - Expected for w, 10 / 12 files byte-identical to the pre-merge build: `orientations.json`, `recipe.json`,
     `train_combined.f64`, `train_combined.json`, `train_combined_finite.u8`, `train_combined_ids.u64`,
     `train_combined_member.u8`, `train_combined_sessions.i64`, `train_daily_ic.csv`, `train_planned_targets.csv`.
   - `summary.json` and `train_candidates.jsonl` may differ only on R0-3's allowed timing paths.
   - u: the same rule over its file set.
   - The copy's `index.sqlite` must keep its SHA-256, with no `-wal` / `-shm`.
2. **Flag sqlite, fresh dir:** `mkdir D; atx-research-store cache init D`, then X-5 u and w twice each with
   `--candidate-cache D --candidate-cache-index sqlite`.
   - Cold run: `candidate_cache.hits` is 0. Warm run: hits = candidate count, and `ic_results.hits` = candidate count.
   - Outputs are byte-identical to (1), except `summary.json` under `/roles/*/candidate_cache` (directory, `index`,
     hits, misses, `vm_evaluations`, entries paths, `ic_results` counters), R0-3's timing paths,
     `/roles/*/stage_seconds/cache_write`, `/roles/*/hash_seconds` and `/roles/*/verify_bytes`, plus the same timing
     paths in `train_candidates.jsonl`.
   - `find D -name '*.json'` must be empty: no sidecar or IC-result file.
   - Log the warm u wall time before and after the merge.
   - Delete D after the run (ruling SQL-11).
3. **Marginal (Y-S pool-only):** use the Y-S pool-only marginal argv.
   - Run (a): `--pair-cache P1` on an empty P1, cold then warm (shards).
   - Run (b): `mkdir P2; atx-research-store cache init P2`, then `--pair-cache P2 --pair-cache-index sqlite`, cold then
     warm.
   - Compare cold with cold and warm with warm: `marginal_ic.json` must be byte-identical outside `/inputs/pair_cache`
     and `/stage_seconds`. `working_bytes` and the `candidates` block must be equal.
   - In (b): cold has `rows_read` 0 and `rows_written` = `computed`; warm has `computed` 0 and `hits` = the pair count.
   - P2 must hold only `index.sqlite*`.
   - Optional read-through check: a copy of P1 after `cache init` with the flag shows `legacy_shards_read` > 0 and
     `legacy_rows_imported` = `cached_pairs`, with the same rows.
4. **Ledger:** re-catalogue first (`verify` reads the catalogued `trial_line` rows, not the file). Then
   `atx-research-store verify --catalog <root's real catalog> --ledger build-equity/trials.jsonl` must exit 0. The
   output must print `index ok`, and `prefix <k>` for each recorded head, the last verdict's `k` being the line count
   at its time. The trial ledger is only read.

## Design decisions (for the reviewer)

- **Flag absent:** every indexed branch sits behind a non-null `CacheIndex*` or `std::optional<CacheIndex>`. No index
  object is constructed without the flag. A null index runs today's code path. There is no selection by file
  presence (SQL-6); teeth S2 shows the test catches one.
- **Pair cache read-through:**
  - With no key row, the open reads today's shards (same code, moved into `read_shards`) and imports them in chunks of
    4096, with the key row in the last chunk. A partial import re-imports on the next open; it shows as
    `legacy_import_pending`.
  - Once the key row exists, shards are not read again. A pair that exists only in a shard written later by a
    files-mode run is a miss and is recomputed. Its bits are equal, and the store's conflict check confirms it.
  - Value checks and messages are shared with the shard path: `checked_pair`, and `place_pair` with its 2^18 bound.
  - The admission arithmetic is untouched.
- **The sqlite `pair_cache` block** has `{directory, index, key_sha256, rows_read, legacy_shards_read,
  legacy_rows_imported, cached_pairs, hits, computed, rows_written}`, plus `store_skipped` / `legacy_import_pending`
  only when true. The files block is byte-identical to today's.
- **`verify --ledger`:**
  - Prefix heads are genesis plus `ledger::prefix_heads` over the catalogued lines. The chain rule is never
    re-implemented.
  - A recorded head is matched against the ledger it names, or against any named ledger when it names none. A head
    naming another ledger counts in `other`.
  - A `ledger_state` row with a NULL head (a catalogue made before the seam was bound) has its line count checked, not
    its head. Recorded heads are always checked.
  - `\` in a path is normalised to `/`.

## Deviations from brief

1. **SQL4-Q2, accepted provisionally:**
   - The config field is `IcRunnerConfig::candidate_cache_index` in `strategy_ic_runner.hpp`. `IcRunnerConfig` lives
     there, not in `strategy_ic_detail.hpp`.
   - `CacheIndex*` is threaded through `cache_store`, `cache_resolve`, `cache_plan`, `ic_cache_lookup` and
     `ic_cache_store` (`strategy_ic_detail.hpp`). Every call site is in `strategy_ic_runner.cpp`.
   - `run_ic` opens the one index and passes it down. This goes beyond "the flag parse and the key only".
   - A null index is today's path byte for byte, as `FlagAbsentKeepsFileLayoutAndSummary` shows.
2. **SQL2 files outside the named list:**
   - `store_cli.cpp` (the `verify --ledger` sub-option the task names) and `store_cli.hpp` (usage comment).
   - `catalog/ingest.hpp` (the declaration and report structs).
   - `research_catalog_test.cpp:368` (the seam is now bound).
3. **CMake:**
   - `atx-impl/tests/CMakeLists.txt` gets three lines, not one: the test source, plus the store link on both
     `atx-impl-strategy-ic-tests` and the globbed `atx-impl-tests`.
   - `atx-engine/tests/CMakeLists.txt` gets source and link lines.
   - All are tail appends.
4. **The marginal sqlite block** carries keys beyond `rows_read`, listed above; they appear inside the flagged block
   only.
5. **SQL1's** `ops_cache.hpp` / `cache_ops.cpp` gain keyed ops. They are "if needed" files, and there is no schema or
   version change.

## Cross-lane edits

The SQL2 and SQL1 files in deviations 2 and 5. There are none in other lanes' files.

## Open risks

- **SQL4-Q1 (P10):** the marginal verb reads candidate signals only from sidecar files
  (`strategy_marginal_ic.cpp:338-400`, `accept_sidecar`). A candidate cache filled under
  `--candidate-cache-index sqlite` has no sidecars, so it cannot feed the marginal verb. This needs a P10 item: rows
  in the marginal reader, or a follow-up flag. No P9 cell uses the flags (SQL-11).
- **M8 is unattributed** in the one teeth build. The key-text refusal (`strategy_marginal_pair_cache.cpp`
  `open_indexed`) is covered by `ConflictingBitsRefused` (:784-787) in normal runs. A reviewer's own mutation of M8
  alone would close this.
- `book_monitor.py --fit-work` does not read SQLite-only records (SQL1-MON). `cache init` prints the warning; SQL4
  does not change it.
- **Host:** about 4.4 GB of RAM was free during the builds, which ran with 4 jobs.

## Disk

- I deleted `pool-32/build-equity/{CMakeFiles,atx-core,atx-engine,atx-impl,atx-tsdb,bin,lib,tests}` by name: 2,962,965,000
  bytes freed. C: had 65.5 GB free afterwards.
- Kept:
  - in `build-equity/`: `audits/`, `v8-interim3-pitch-render-run2/` (tracked), the `mega-p9-sql4-{a,b,c,d}-*`
    receipts and logs, and the CMake cache and configure files;
  - all logs in `sql4-logs/`.
- `build/` (the lease's failed `dev` configure) was left alone.

## Permission refusals

None.

## Ledger candidates

- A gtest range-for over `read_json(...).at("roles")` dangles in C++20, because the temporary json dies before the
  loop body. The symptom is `json type_error 304`. Name the json first.
- A single-TU `check` of a new test TU can compile about 211 order-only dependency objects, including the shared
  `C:\atx-cache\deps\spdlog-build` objects (SPDLOG-RELINK).
- In a teeth build with several mutations at once, an `ASSERT_*` that one mutation trips masks a later mutation in the
  same test. Put each mutation's first check in its own test, or use `EXPECT`, to attribute every mutation in one
  build.
