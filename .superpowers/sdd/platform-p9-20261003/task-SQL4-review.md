# Review: lane SQL4 (IC cache indexes on SQLite; the trial-ledger index head), first review

- Reviewer: fresh (REV-FRESH), RESUME 11, 2026-10-15. Pool `C:/atx-wt/pool-32`, branch `feat/p9-sql4-20261008`.
- BASE `475f5d8f` (root's head the lane merged last; `git merge-base` with root). HEAD `15f51e62` (records) over
  code `c5df575d`. `git diff c5df575d 15f51e62 -- . ':!.superpowers'` is empty: the head's code is `c5df575d`.
- Range reviewed: `475f5d8f..15f51e62` only. Lane code commits `a1955a71`, `96b2b1e1`, `22e41583`, `237fc8a6`,
  `4aade941`, `44fce688`; root merges `82756940`, `c5df575d`.
- Read: `resume9/10/11-addendum.md` (and 4 / 6 / 7 / 8 for the owner rules), `r7-review-dispatch.md`,
  `briefs/review-template.md`, `r11-dispatch.md` SQL4-FINISH and the scoped-review bullet, `r10-dispatch.md` SQL4,
  `.agents/cpp/agent.md`, `briefs/brief-SQL4.md`, `task-SQL4-report.md`, `handoff-SQL4.md`, `sql4-logs/`; ledger
  rulings SQL-1 .. SQL-12, SQL4-GO, SQL4-Q1, SQL4-Q2, SQL4-XLANE, SQL4-M8, SQL4-CUTOFF, EXE-FREEZE, EXE-CUR,
  TEST-1, TEETH-RESTORE, TEETH-CRLF, HOST-4, HOST-5, M1d-NOTE-3, the "SQL4 lane hand-back" line.

## Verdict

**APPROVE** at `15f51e62` (code `c5df575d`). 0 Required, 9 Suggested.

- **Spec:** brief tasks 1-7 are done. Every named test exists, in the right suite and target. The deviations are
  stated and acceptable: SQL4-Q2, the SQL2 / SQL1 files, the CMake line counts and the extra marginal block keys.
  SQL4-Q1 is stated as a P10 item in the report and the handoff.
- **Quality:**
  - Flag absent is today's code path byte for byte at every call site (trace below).
  - SQLite discipline holds: a leaf lock, one connection per index object, WAL through `open_cache`, versioned
    schema, '/'-relative paths, refusal or miss and never a silent wrong read.
  - The three tripwires and every pinned list are untouched. No Python file changed.
  - The M8 tooth is now attributed by my own build.

## Merges in the range

| merge | check | result |
|---|---|---|
| `82756940` (root `e3b6c111` into `73a871fb`) | `git merge-tree --write-tree 73a871fb e3b6c111` | `7089a374` = the merge's tree: plain |
| `c5df575d` (root `475f5d8f` into `44fce688`) | recompute: conflicts in exactly the 4 CMake tails | see below |

For `c5df575d`, the recompute conflicts only in `atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt`,
`atx-impl/CMakeLists.txt` and `atx-impl/tests/CMakeLists.txt`. `git diff <recomputed tree> <merge tree>` touches
only those 4 files. In each, the conflict markers are removed, root's block (A4 / SIGN) is kept verbatim, and the
lane's SQL4 block is appended after it. Nothing else changed: a clean region-by-region resolution.

## Spec compliance (brief "Lane SQL4", tasks in order)

| item | state | evidence (file:line at HEAD) |
|---|---|---|
| 1 IC `--candidate-cache-index {files,sqlite}` | done | parse `strategy_ic_runner.cpp:1142-1146`; usage refusals (exit 2) `:1187-1194`; API refusals `:797-805`; `"index": "sqlite"` only when indexed `:737-739`; plan-only `strategy_ic_signal_cache.cpp:654` |
| 1 marginal `--pair-cache-index` | done | `strategy_marginal_ic.cpp:1164-1168`, `:1180-1189`, `:937-939`; sqlite block `:1024-1035` with `rows_read` in place of `shards_read`; files block unchanged `:1036-1044` |
| 2 index object | done | `strategy_ic_cache_index.{hpp,cpp}`: `cache_size` -8192 KiB = 8 MiB (`.cpp:104-105`); busy / I/O on a read = miss plus a progress line (`.cpp:70-75`), on a write = skipped store (`.cpp:79-88`); other errors refuse |
| 3 signal cache | done | payload published no-replace first, then the row (`strategy_ic_signal_cache.cpp:592,605-609`); row key parts checked, refusing as a sidecar mismatch (`row_hit :406`, `:413-416`); read-through import (`:441-460`); no sidecar written |
| 4 IC-result cache | done | record text unchanged in `ic_result.record` under (entry_dir, id) (`strategy_ic_result_cache.cpp:343-366`); self-hash on read (`intact_row :335`); read-through import (`:373-394`) |
| 5 pair cache | done | rows ordered by `(payload_lo, payload_hi)`, keyset pages (`strategy_marginal_pair_cache.cpp:172-199`); one BEGIN IMMEDIATE, ON CONFLICT DO NOTHING, then a bits check (`strategy_ic_cache_index.cpp:213-244`); 2^18 bound through `place_pair`; legacy shards read through (`:201-232`) |
| 6 ledger head; `verify --ledger` | done | `default_ledger_head` bound to B2's `chain_head` (`ingest_ledger.cpp:34-39`); prefix heads only from B2's `prefix_heads` (`:255-296`, `:266`); exit 3 (`store_cli.cpp:389-440`); the JSONL file stays the authority |
| 7 tests | done | the 13 IC / pair tests in `strategy_ic_cache_index_test.cpp` (`atx-impl-strategy-ic-tests`), the 2 ledger tests in `research_catalog_ledger_test.cpp` (`atx-engine-research-catalog-tests`) |
| Forbidden list | held | untouched: `strategy_ic_composition.*`, the marginal arithmetic, every IC computation, `dsl_vm_sources` / `ic_result_sources` / pair sources and their SHA-256 (no `-`/`+` line names them), recipes, `vcpkg.json`, `CMakePresets.json`, `atx-db/`; no product Python |
| Out of scope | held | the fields reuse store, the record store and the payload formats are untouched. Hit / miss is unchanged for a key except in the documented pair-cache mixed-use case (S-7). Spec keys / resume status: none |
| SQL-6 / SQL-11 | held | `cache_index_present` only refuses. `git grep` of both flags at `15f51e62`: only `atx-impl/src`, the new test, the CMake comment and `docs/plans`; no spec, prereg, cell argv or Python |
| SQL4-Q1 | stated P10 | report "Open risks", handoff "Stated for the review" |

## Judged items

### (1) Flag absent: every output byte unchanged

I traced every place a `CacheIndex*` or the config field is consulted:

- `run_ic` (`strategy_ic_runner.cpp:822-831`) builds an index only when `candidate_cache_index == Sqlite`.
  Otherwise `cache_index` is null. No file is opened, and nothing is constructed or printed.
- `score_role` and `candidate_signal` only forward the pointer. The one new output key is behind `if (index)`
  (`:739`).
- `cache_store`: the `if (index)` branch comes after the payload store and before the sidecar publish. The null
  path is the old code unchanged.
- `cache_resolve`: a ternary, and the null arm is the old `cache_lookup`.
  - `cache_lookup`'s refactor is behaviour-preserving: `manifest_agrees` is the same lambda body, and `text`
    defaults to null.
  - `read_sidecar` renamed a local and added a no-op `if (text)`.
- `cache_plan`: the extra key is behind `if (index)`. `cache_report` always passes `nullptr`.
- `ic_cache_lookup`: `if (!index) return file_lookup(...)`.
  - `file_lookup` performs the old checks in the old order (parse, malformed, digest, integrity, then match), with
    the same messages.
  - `ic_cache_store`'s null path creates the directory and publishes as before. `std::move(digest)` became a copy,
    with the same bytes.
- `open_marginal_pair_cache`: the null arm is `read_shards`, the old body moved verbatim. `parse_pair`,
  `checked_pair` and `place_pair` keep the old checks in the old order, with the same messages.
- `seed_pairs` opens nothing unless the mode is Sqlite. The store block's `else` is the old block verbatim.
- No JSON key order changes: nlohmann objects are sorted maps, and the null path adds no key. No timing-dependent
  content is added, and no new file is written.
- The only flag-absent byte changes are the two `--help` texts and the usage message of `verify` given neither
  `--pins` nor `--ledger` (still exit 2).

Lane tiny-world fakes, re-derived:

- `IcCacheIndex.FlagAbsentKeepsFileLayoutAndSummary` compares the whole summary (timings removed), every exact
  output file, and the cache listing by name and SHA-256, with and without `index.sqlite`, cold and warm. It also
  checks that the index bytes are unchanged and that no `-wal` / `-shm` file appears.
- `IcCacheIndex.SummaryRecordsIndex` and `MarginalPairIndex.FlagAbsentBlockUnchanged` cover the flagged runs:
  identical outside the listed blocks.
- Teeth S2 (open by presence) turns the first test red.
- All three tests pass in my own build (below), and at `-c` in the lane's log.

### (2) Tripwires, pins, Python

- `git diff --stat 475f5d8f 15f51e62 -- atx-impl/tests/strategy_ic_runner_test.cpp
  atx-impl/tests/strategy_marginal_ic_test.cpp` is empty.
- The three tripwires (`StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`, `.IcSourcesPinnedToSemanticsVersion`,
  `MarginalIc.PairCacheSourcesPinned`) pass in my build. That build also mutated
  `strategy_marginal_pair_cache.cpp`, which confirms it is outside the pair sources list.
- The only `.py` files in the range are the lane's tools under the sprint dir.

### (3) SQLite

- **Lock order:** documented at `strategy_ic_cache_index.hpp:16-24`.
  - The lock is a leaf mutex, held only across statements: `with_connection` returns before the result is used,
    and `write` runs one `with_immediate` under the lock.
  - Progress lines are written with the lock released. No callback runs under it.
- **Single writer:** one connection per index object, and one index object per run, owned by `run_ic`
  (`std::optional`) or by `PairCacheUse`. Writers in other processes are serialised by BEGIN IMMEDIATE, the 30 s
  busy timeout and the bounded retry. `TwoConnectionsInterleave` covers two connections.
- **No connection across forks:** the IC and marginal code spawns no process. The connection lives inside the
  run's scope.
- **Journal mode:** WAL, synchronous NORMAL, 8192 pages, `trusted_schema` OFF, all through SQL1's `open_cache`
  (`store.hpp:13-17`). There is a TRUNCATE checkpoint in the destructor. The index header does not restate WAL
  (S-8).
- **Paths:** rows hold '/'-paths relative to DIR. `CacheIndex::relative` refuses anything outside DIR, and both
  sides derive from `cfg.candidate_cache_directory`, so a relative DIR works. The pair rows hold no paths.
- **Schema:** the cache group is versioned by `user_version` with a `schema_json` drift check (SQL1). The lane adds
  no column and no version step.
- **Missing or corrupt index:**
  - Missing with the flag: exit 2 (usage), or Err(NotFound) from the API.
  - Foreign, drifted or newer: `open_cache` refuses (exit 1).
  - A corrupt page or busy at read: `IoError` / `Unavailable` (`atx-core/src/db/sqlite.cpp:61-69`). That is a
    miss with a progress line, then the legacy files or a recompute.
  - A row that decodes is always checked:
    - signal row: key parts, schema, the sidecar text's payload SHA-256, and the payload path. The payload is
      hashed again on load;
    - IC-result row: self-hash and scope key;
    - pair row: the shard value checks.
  - So a silent wrong read is never possible.

### (4) SQL4-Q2, on the owner's bar: accepted

- The config field belongs in `strategy_ic_runner.hpp`, because `IcRunnerConfig` is defined there; the brief named
  the wrong file.
- One index object is passed explicitly through the five functions. There is no global, and null is today's path.
- The design is modular:
  - a PIMPL index TU;
  - keyed reads and inserts added to SQL1's descriptor-generated `ops_cache` (no hand-written SQL in atx-impl);
  - shared validators extracted instead of copied: `checked_pair`, `place_pair`, `read_shards`, `parse_ic_entry` /
    `match_ic_entry`, `manifest_agrees`.
- Small leftovers are Suggested: the header weight (S-4) and two twin bodies (S-5).

### (5) SQL4-XLANE: the cross-lane hunks, each minimal and necessary

| file | hunk | necessary? | minimal? |
|---|---|---|---|
| `catalog/ingest.hpp` | header comment: the seam is now bound; `verify --ledger` contract | yes: the old text says "returns an empty function" | yes |
| `catalog/ingest.hpp` | `#include "atx/core/types.hpp"` | yes (`i64` in the new structs) | yes |
| `catalog/ingest.hpp` | `LedgerIndexCheck`, `RecordedLedgerHead`, `LedgerVerifyReport`, `verify_ledger_heads` | yes: task 6's function, defined in the brief-owned `ingest_ledger.cpp`; this is its header | yes (declarations only) |
| `catalog/store_cli.hpp` | usage comment and exit-3 cases | yes (documents the new sub-option) | yes |
| `catalog/store_cli.cpp` | usage line | yes | yes |
| `catalog/store_cli.cpp` | `cmd_verify` split into `verify_pins` (old body verbatim, `*db` to `db`) + `verify_ledgers` + a new `cmd_verify` | yes: task 6 names `verify --ledger` | yes. `verify --pins` output and exit codes are unchanged. Neither flag still gives exit 2, with a new message (`StoreCli.ExitCodes:83` still holds by reading) |
| `research_catalog_test.cpp:368` | `EXPECT_FALSE` to `EXPECT_TRUE` on `default_ledger_head()` | yes: binding the seam (task 6) falsifies the old "B2 not bound at this base" assertion | yes (one line) |
| SQL1 `ops_cache.hpp` / `cache_ops.cpp` | keyed selects, `insert_or_ignore`, keyset page, all through the table descriptors | yes (tasks 3-5) | yes; no schema or version change |

The green evidence is partial:

- `ResearchCatalog.*` passed 17 / 17 on `-b`, from the lane's log. That exe is source `44fce688`; the second merge
  changed no C++ in the catalog closure, only `classes.json` and other targets' CMake blocks (S-9).
- `StoreCli.*` and `SealGuard*` were never run with the `store_cli.cpp` change: the lane's filter is
  `ResearchCatalog.*` only.
- I find them green by reading (`verify_pins` is the old body), and `LedgerHeadFromResearchLedger` runs the CLI
  `catalog` over the fixture tree with exit 0.
- The Python store suite (43, seeds 0 / 1) is root's. Its only case that reaches this C++ is `FixtureChain`, which
  is skipped without the exe. Its first two argv equal that gtest's CLI run, and its checker ignores
  `ledger_state`'s head.

The edits are accepted on the condition that root's M6b runs SQL2's full filter and the store suite with the fresh
`atx-research-store` (see "What root must check").

### (6) House style

- `/W4 /WX`: my build compiled 343 TUs and linked 15 with exit 0 and no warnings. So did the lane's builds.
- 100 columns: an awk scan of every added line in the diff found 0 lines over 100 (37 longer context lines are
  pre-existing).
- Tests are in the right group: the IC tests are in the focused `atx-impl-strategy-ic-tests` (and the
  `atx-impl-tests` glob), and the ledger tests are in `atx-engine-research-catalog-tests` (`tests/research` is not a
  test group, as SQL2's block notes). Suite names do not clash.
- Memory is bounded:
  - pair pages of 4096 rows, under at most 65 pages and the 2^18 bound;
  - import chunks of 4096;
  - one signal or IC-result row at a time;
  - `select_rows` with a `max_rows` guard;
  - ledger lines bounded by the ledger.

### (7) Teeth

- From the lane's logs (`teeth-p9-sql4-d*.log`):
  - The IC exe has 6 reds: FlagAbsentKeepsFileLayoutAndSummary :318 / :320, FlagWithoutIndexRefused :356-359 /
    :374-375, KeyMismatchRefuses (both tampers) :498-501, TamperedRecordIsMiss :670, ConflictingBitsRefused :778,
    FlagAbsentBlockUnchanged :904. The other 73 pass.
  - The catalog tests have 3 reds: LedgerHeadFromResearchLedger :96-108, RecordedHeadsArePrefixHeads :142 / :184 /
    :187, LedgerHeadSeamReceivesLinesInFileOrder :368. The other 14 pass.
  - That is the claimed 9 named tests, and each mutation in the table maps to its line.
- The tool's restore follows the TEETH-CRLF form and its log ends with that line:
  - it compares `git hash-object` against `git rev-parse HEAD:<path>`;
  - it compares the raw SHA-256 against the kept copy;
  - it checks that porcelain is empty.
- **SQL4-M8: attributed by my own run.**
  - No build of ours was running or queued: the process list was clean at 13:47 and again just before the start.
  - Root GATE-0 is "no build", and M6b follows this verdict.
  - My one build, tag `p9-sql4-e`, applied the M8 mutation ALONE (`open_indexed`'s key-row check disabled, the
    lane's own replacement text). Results:
    - 79 ran, 78 passed, 1 failed: `MarginalPairIndex.ConflictingBitsRefused` at `:786` (`ASSERT_FALSE(foreign)`),
      exactly the key-text check.
    - Restore verified: porcelain empty; `git hash-object` = HEAD blob `ce6edbb9`; raw SHA-256 = the kept copy
      (`1d36b7c0...`).
  - The test split in P10 that SQL4-M8 names is no longer needed.

### (8) Root's identity procedure: executable, with the clarifications below

- The X-5 w argv exists (`m2e-logs/receipt-p9-m2e-alcomb-x5-w-post.json` `command`), and the 10 expected-identical
  files match R0-3's precedent.
- Three points need tightening before the run. They are listed in S-2, and the corrected rules are in "What root
  must check":
  1. Step 1's second run uses a COPY of the cache dir. That changes the path-valued leaves of
     `/roles/*/candidate_cache`, which are not on the stated allowed list.
  2. Step 2's allowed list does not name the per-candidate hit / miss strings (M1d-NOTE-3) or `stage_seconds/vm`.
  3. "u: the same rule" and "the Y-S pool-only marginal argv" name no file list or receipt.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `atx-engine/src/research/store/catalog/ingest_ledger.cpp:34-39`, `:137-141` | Suggested (S-1) | The bound seam fills `ledger_state.head_sha256` / `head_rule` on every `catalog` / `ingest` of a trial ledger (task 6, by design, no flag). So root's recorded fixture-chain catalog digest (`d32f7655...4e69`, `handoff-SQL2.md:90`, checked at earlier merges) changes after this merge. The report does not say so. | Root (see below): show that only those two columns of the one `ledger_state` row differ, then record the new digest with this attribution. It is a derived index, not a pin, and not a regression. |
| `task-SQL4-report.md` "How root verifies" 1-3 | Suggested (S-2) | Step 1(b)'s copied cache dir changes `/roles/*/candidate_cache/{directory,fields_directory,entries/*/*}` (outside the stated list). Step 2 omits the per-candidate `signal_cache` / `ic_result_cache` / `ic_missing_exit_v1_result_cache` strings and `stage_seconds/vm` (a cold vs warm comparison). "u: the same rule" lists no files. The Y-S marginal argv names no receipt. | The corrected partition rule and allowed paths are in "What root must check" (no lane round needed). |
| `atx-engine/tests/research/research_catalog_cli_test.cpp:76-101` (not run) | Suggested (S-3) | `StoreCli.*` / `SealGuard*` were not run after the `store_cli.cpp` change; the lane's filter was `ResearchCatalog.*` only. Green by reading only. | Root runs SQL2's filter `ResearchCatalog*:SealGuard*:StoreCli*` at M6b. |
| `atx-impl/src/strategy_ic_runner.hpp:8`, `strategy_marginal_ic.hpp:8` | Suggested (S-4) | The two config headers include the whole index header (`rows_cache.hpp`, `<filesystem>`, `<memory>`, `<span>`) for the `CacheIndexMode` enum only. About 20 TUs now see the store row types (agent.md section 6, minimal includes). | P10: move `CacheIndexMode` and `parse_cache_index_mode` into a small header; `strategy_ic_detail.hpp` forward-declares `CacheIndex`. |
| `atx-impl/src/strategy_ic_cache_index.cpp:132-183` | Suggested (S-5) | `put_signal` and `put_ic_result` are one body written twice; the two dispatchers repeat the same two refusals. | P10: one template over the row type and its select. |
| `atx-engine/src/research/store/catalog/ingest_ledger.cpp:276-290` | Suggested (S-6) | A recorded head naming a ledger not given (or a `\`-path: recorded paths are not normalised, only `--ledger` is) is only counted as `other-ledger` and exits 0. | Root's step 4 requires `other-ledger 0`. P10: normalise recorded paths, and refuse unmatched heads under `--strict`. |
| `atx-impl/src/strategy_marginal_pair_cache.hpp:37-39` | Suggested (S-7) | Once a key row exists, shards a later files-mode run writes into the same DIR are never read. Those pairs are recomputed (same bits, conflict-checked), so only the `pair_cache` counters differ. This is documented, and only in mixed use with the flag. | P10: re-scan shards when the key directory has more shards than imported, or refuse a files-mode run on an indexed DIR. |
| `atx-impl/src/strategy_ic_cache_index.hpp:12-37` | Suggested (S-8) | The lock and failure policies are stated, but the journal mode is not. WAL / synchronous NORMAL come from `open_cache` (`store.hpp:13-17`). | One line naming WAL + NORMAL + the TRUNCATE checkpoint. |
| `sql4-logs/r11-gtest-p9-sql4-b-catalog-merged.log` | Suggested (S-9) | `ResearchCatalog.*` on the merged head ran the `-b` exe (source `44fce688`). EXE-CUR holds in substance: the merge changed no C++ of the catalog closure, only other targets' CMake blocks and `classes.json`, which is read at run time. It is not a build of the head. | Root's M6b build runs it on the merge head. |

## What I verified myself vs from the lane's logs

My own runs:

- Both merge recomputes and the conflict-resolution diff.
- The full diff, read hunk by hunk.
- The 100-column scan.
- `git grep`: no Python, no pinned list, no forbidden file, no new `atx.*` literal in production code, and no flag
  outside the source and tests.
- A configure (`powershell scripts\atx-build.ps1 configure -Preset equity-dev`, exit 0, 12 s; it printed
  `engine_git_sha 15f51e62`).
- Build `p9-sql4-e` (`scripts/research-build.ps1`, targets `atx-equity-strategy-ic,atx-impl-strategy-ic-tests`):
  - Debug, source `15f51e62` plus the one M8 mutation (DirtyEntries 1);
  - exit 0, 343 TUs, 135.5 s, 4 jobs, free 4485 MiB;
  - IC exe `6a6dccc0...`, tests exe `68d99650...`. These are teeth binaries, never evidence of record, and they are
    deleted.
- The named filter `IcCacheIndex.*:IcResultIndex.*:MarginalPairIndex.*:StrategyIcRunner.*:MarginalIc.*`: 79 ran,
  78 passed, 1 failed (the M8 tooth).
  - So 78 of the 79 pass by my own run on the head's code.
  - The 79th (`ConflictingBitsRefused`) differs only by the mutated line, and it is green at `-c` in the lane's log.
- The restore assertions.
- The object tree deleted by name: `build-equity/{atx-core,atx-engine,atx-impl,atx-tsdb,bin,lib,tests}`,
  2,670,176,258 bytes. I kept the receipts, logs, `CMakeFiles/` and `CMakeCache.txt`.
- The teeth tool was a scratch script (TEETH-RESTORE + TEETH-CRLF form). Its log lines are quoted above.

From the lane's logs only:

- 79 / 79 at `-c` (`c5df575d`).
- `ResearchCatalog.*` 17 / 17 at `-b` and on the merged tree.
- The 13-mutation teeth at `-d`.
- The check logs.

No pytest was run: the lane has no pytest file, and TEST-1 says reviewers run the lane's files only.

## What root must check at merge (M6b, LAST, before the gate's P9-B0 re-pin; SQL-12, SQL4-CUTOFF)

1. **Merge.** Merge `feat/p9-sql4-20261008` at this review's commit: records only, above `15f51e62`, code
   `c5df575d`.
   - Root has moved past BASE to `22f99018` plus GATE-0's commits. Expect conflicts only in the four CMake tails:
     keep root's blocks verbatim and append SQL4's.
   - Check that the merge tree equals the `merge-tree` recompute outside those regions.
2. **Build.** One Debug build (next free tag) of
   `atx-equity-strategy-ic,atx-impl-strategy-ic-tests,atx-research-store,atx-engine-research-catalog-tests`.
   Release of the IC exe is the gate's (TEST-1). EXE-FREEZE: this relinks the IC exe, so it must land before the
   P9-B0 re-pin.
3. **gtests.**
   - `atx-impl-strategy-ic-tests --gtest_filter=IcCacheIndex.*:IcResultIndex.*:MarginalPairIndex.*:StrategyIcRunner.*:MarginalIc.*`:
     79 / 79, which includes the three tripwires.
   - `atx-engine-research-catalog-tests --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*`: 0 failures (S-3).
     `ResearchCatalog.*` alone is 17 / 17.
4. **Tripwire sources.** `git diff <root pre-merge>..<merge> -- atx-impl/tests/strategy_ic_runner_test.cpp
   atx-impl/tests/strategy_marginal_ic_test.cpp` must show no SQL4 hunk. No pinned list or SHA moves.
5. **Python** (merge-affected, ROOT-1a):
   - the store suite (`atx-engine/tools` store tests, seeds 0 / 1) with `ATX_RESEARCH_STORE_EXE` set to the fresh
     exe, so that `FixtureChain` RUNS: it must print `total checked 12 mismatches 0`;
   - `scripts/tests/test_research_cycle.py::test_marginal_argv_is_the_verbs_full_cli`: the report's regex claim,
     which I checked by reading;
   - the SQL3 hook file (`ingest` now fills the head).
6. **Fixture catalog digest (S-1).** Run `init` + `catalog --root atx-engine/tests/fixtures/research_store/tree
   --classes atx-engine/schemas/research_store/classes.json`, then `digest`. Expect a value different from
   `d32f7655...4e69`.
   - Prove the cause: `dump --table` of every table equals the pre-merge exe's dump, except `ledger_state`.
   - In `ledger_state`, only `head_sha256` / `head_rule` go from NULL to B2's chain head and `kChainRule`.
   - Record the new digest with this attribution. Edit no expected value.
7. **Flag-absent identity on the fakes.** These are the gtests in item 3: `FlagAbsentKeepsFileLayoutAndSummary`,
   `SummaryRecordsIndex`, `MarginalPairIndex.FlagAbsentBlockUnchanged`. The Python `tiny_world` canary is the
   gate's.
8. **Real X-5 u / w identity** (0 trials; compare by SHA-256 and JSON paths only; open no output values).
   - **Argv.** w = the `command` of `m2e-logs/receipt-p9-m2e-alcomb-x5-w-post.json`, with only `--output` changed
     (and `--candidate-cache` / `--candidate-cache-index` where stated). u = the same argv with `--no-composition`
     in place of `--composition-weights F --composition-weights-sha256 S`.
   - **Rule, for u and w.** Every file in the output directory except `summary.json` and `train_candidates.jsonl`
     must be byte-identical. For w that is the report's 10 files. For u it is whatever the pre-merge build writes:
     list it from the pre-merge run before the post-merge run. The two JSON files may differ only on the allowed
     paths of the step.
   - **1a.** No flag, pre-merge exe vs post-merge exe, same cache dir. Allowed: R0-3's timing paths (any key ending
     in `seconds`, at any depth).
   - **1b.** No flag, with an `index.sqlite` present. Do EITHER of the following:
     - Run `atx-research-store cache init <the real cache dir>`, run, check that `index.sqlite` keeps its SHA-256
       and that no `-wal` / `-shm` appears, then delete `index.sqlite` by name. Allowed paths as in 1a.
     - Copy the cache dir (about 7.3 GB) and `cache init` the copy. Allowed paths are 1a's plus the path-valued
       leaves of `/roles/*/candidate_cache` (`directory`, `fields_directory`, `entries/*/*`).
   - **2.** Flag `sqlite` on a fresh `D` (`mkdir D; atx-research-store cache init D`), cold then warm, each against
     1a. Allowed:
     - 1a's paths;
     - the whole `/roles/*/candidate_cache` block;
     - `/roles/*/hash_seconds` and `/roles/*/verify_bytes`;
     - per candidate (`/roles/*/candidates/*/` in `summary.json`, and the top level of each `train_candidates.jsonl`
       line): `signal_cache`, `ic_result_cache`, `ic_missing_exit_v1_result_cache`, and all of `stage_seconds`
       (M1d-NOTE-3).
     - Cold: `candidate_cache.hits` is 0. Warm: hits = the candidate count, and the `ic_results` hits = the
       candidate count.
     - `find D -name '*.json'` must be empty. Log the warm u wall time before / after.
     - The cold pass is a full VM pass that writes a cache of the real one's size: budget it against the 16:45
       cutoff.
9. **Marginal shard identity.** Use the Y-S pool-only marginal argv of record (the one root's ledgered-rows identity
   used), changing only `--output`, `--pair-cache` and `--pair-cache-index`.
   - (a) shards on an empty P1, cold then warm. (b) `cache init` P2, `--pair-cache-index sqlite`, cold then warm.
   - `marginal_ic.json` must be byte-identical cold to cold and warm to warm, outside `/inputs/pair_cache` and
     `/stage_seconds`, with `working_bytes` and `candidates` equal.
   - (b) cold: `rows_read` 0 and `rows_written` = `computed`. (b) warm: `computed` 0 and `hits` = the pair count.
     P2 must hold only `index.sqlite*`.
10. **Ledger.** `verify --catalog <a fresh real-tree catalog> --ledger build-equity/trials.jsonl`:
    - exit 0, `index ok`;
    - one `prefix <k>` per recorded head;
    - `other-ledger 0` and `not-a-prefix-head 0` (S-6).
    The trial ledger is only read. Re-catalogue first.
11. **SQL2-CLS:** no new `atx.*` literal in SQL4's production code, so no class row is due. **SQL-11:** no
    `--candidate-cache-index` / `--pair-cache-index` on any cell argv, spec or prereg of the gate head (`git grep`).
12. **Clean-up.** Delete every index, copy, `D`, `P1` and `P2` after the runs (SQL-11). The real cache dir must end
    as it began.

## Permission refusals

- None.
- One harness block: a foreground `sleep 45` chained with a log read was refused as a wait pattern. I did not retry
  it: I waited on the background-task notice instead.
