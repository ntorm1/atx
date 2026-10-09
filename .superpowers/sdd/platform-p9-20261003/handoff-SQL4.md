# Handoff: lane SQL4 (IC cache indexes on SQLite; ledger index head) -- stopped at OWNER STOP 10

Pool `C:/atx-wt/pool-32`, branch `feat/p9-sql4-20261008`. State at the stop (2026-10-08): head `a1955a71` (WIP),
tree clean (`git status --porcelain` empty). Root merged: `b182dfdd`. The owner's STOP 10 ("stop here") arrived while
task 5 was being coded. No build, configure or test was started in this lane; nothing was compiled.

## Commits of this lane

| sha | what |
|---|---|
| `a347eea8` | `git merge f0726acb` (D3W APPROVE head). Clean merge. Default merge message: it has NO `Co-Authored-By` trailer (my slip; history is not rewritten). |
| `12d457e6` | `git merge 6a3f5043` (ABS APPROVE head). Clean. |
| `569a1c6f` | `git merge feat/platform-p9-20261003` = root `b182dfdd` (C3b merged in root by then). Clean, no conflict. |
| `a1955a71` | `WIP(sql4): ...` the code below, UNCOMPILED, plus the lane tool `sql4-logs/tools/subst.py`. |

## Brief tasks: done / not done

| task | state | where (file:line at `a1955a71`) |
|---|---|---|
| 1 flags, IC exe half | CODED, uncompiled | `atx-impl/src/strategy_ic_runner.hpp:113-119` (config field `candidate_cache_index`, default Files; IcRunnerConfig lives in this header, not in `strategy_ic_detail.hpp` as the brief says: deviation to list); `strategy_ic_runner.cpp:1142-1146` (parse `--candidate-cache-index files\|sqlite`), `:1187-1194` (exit 2: given without `--candidate-cache`; `sqlite` without `DIR/index.sqlite`), `:797-805` (run_ic refusals: sqlite without the cache dir; sqlite with `--cache-report`), `:822-831` (one index object per run; `--plan-only` opens it read-only, no progress lines), `:737-739` (`"index": "sqlite"` in the summary's `candidate_cache` block only when indexed), help text `:1053`, `:1097-1101`. |
| 1 flags, marginal half | NOT DONE | `--pair-cache-index` on `strategy_marginal_ic.{hpp,cpp}`; `pair_cache` block `"index": "sqlite"`, `rows_read` in place of `shards_read`. |
| 2 index object | CODED, uncompiled | NEW `atx-impl/src/strategy_ic_cache_index.hpp` (contract, lock order, failure policy, memory) and `.cpp` (open `:101`, put_pairs `:204`). One connection under one mutex (leaf lock), `PRAGMA cache_size=-8192` (8 MiB, counted in the 32 MiB fixed slack: comment in `strategy_ic_result_cache.cpp:86-90`), transient (Unavailable / IoError) read = miss + progress line, transient write = Skipped + progress line, read-only mode for `--plan-only`. Engine side: keyed reads and `INSERT ... ON CONFLICT DO NOTHING` in `atx-engine/include/atx/engine/research/store/ops_cache.hpp:58-91` and `atx-engine/src/research/store/cache_ops.cpp:63-224` (cache group's instantiating TU; schema and fixture `cache.json` unchanged, no version bump). |
| 3 signal cache | CODED, uncompiled | `atx-impl/src/strategy_ic_signal_cache.cpp`: `read_sidecar` / `v1_probe` / `cache_lookup` gained a null-default text out-param (files path unchanged); `row_hit`, `indexed_lookup` `:440` (row first, else legacy files verified as today and imported), `commit_signal_row` `:463`, `cache_store` `:583` (index: payload no-replace first, then the row; no sidecar file), `cache_resolve` / `cache_plan` take `CacheIndex*`; `cache_report` passes null (refused with sqlite). |
| 4 IC-result cache | CODED, uncompiled | `atx-impl/src/strategy_ic_result_cache.cpp`: today's lookup split into `parse_ic_entry` / `match_ic_entry` / `file_lookup` (same checks, same messages); `store_ic_row` `:344`; `ic_cache_lookup` `:373` (a row failing its self-hash = miss + progress line, then the file, then import replacing the bad row); `ic_cache_store` `:398` (index: row only, no directory created). |
| 5 pair cache | NOT DONE | Design ready (below). `CacheIndex::put_pairs(key_sha256, rows, key*)` and `pair_stats_page` already exist for it. |
| 6 ledger head | NOT DONE | Design ready (below). |
| 7 tests, CMake tails | NOT DONE | Nothing written. |
| report | NOT DONE | `task-SQL4-report.md` not written. |

Signature changes in `atx-impl/src/strategy_ic_detail.hpp:374-385, 400-408` (a `CacheIndex*` parameter on
`cache_store`, `cache_resolve`, `cache_plan`, `ic_cache_lookup`, `ic_cache_store`): beyond the brief's "one config
field"; every call site is in `strategy_ic_runner.cpp` (updated).

## What compiles / what is tested

Nothing compiled, built or tested. `a1955a71` does NOT link as is: `strategy_ic_cache_index.cpp` is not yet a source
of `atx-impl-strategy` and that target does not yet link `atx::engine-research-store`. The first act of the next agent
is the two CMake lines below, then `powershell scripts\atx-build.ps1 configure -Preset equity-dev` and single-TU
`check` of: `atx-engine/src/research/store/cache_ops.cpp`, `atx-impl/src/strategy_ic_cache_index.cpp`,
`strategy_ic_signal_cache.cpp`, `strategy_ic_result_cache.cpp`, `strategy_ic_runner.cpp`. Known things to verify at
the first check: `-Wshadow` on `hit` in `indexed_lookup`; the `TableDescriptor` / `detail::` names used in
`cache_ops.cpp`; `ATX_TRY` used with an assignment target (`ATX_TRY(out.existing, ...)`, `ATX_TRY(hit.payload_sha,
...)`).

## Next steps, in order

1. CMake tail lines (append at the end of each file): `atx-impl/CMakeLists.txt`:
   `target_sources(atx-impl-strategy PRIVATE src/strategy_ic_cache_index.cpp)` and
   `target_link_libraries(atx-impl-strategy PRIVATE atx::engine-research-store)`; `atx-impl/tests/CMakeLists.txt`:
   `target_sources(atx-impl-strategy-ic-tests PRIVATE strategy_ic_cache_index_test.cpp)` plus
   `target_link_libraries(atx-impl-strategy-ic-tests PRIVATE atx::engine-research-store)` (and the same link for the
   globbed `atx-impl-tests`); `atx-engine/CMakeLists.txt`: `target_link_libraries(atx-engine-research-catalog PRIVATE
   atx::engine-research-ledger)`; `atx-engine/tests/CMakeLists.txt`: `target_sources(atx-engine-research-catalog-tests
   PRIVATE research/research_catalog_ledger_test.cpp)` + link `atx::engine-research-ledger`.
2. Configure equity-dev, single-TU checks of the five TUs above; fix slips.
3. Task 5 (pair cache), design: `strategy_marginal_pair_cache.hpp` forward-declares `CacheIndex`;
   `MarginalPairCache` gains `rows_read`; `open_marginal_pair_cache(root, scope, CacheIndex* index)`: with an index,
   a `pair_key` row present (same version = `kPairwiseRowCorrelationVersion` and key text, else refuse) means read
   every `pair_stat` row of the key page by page (`pair_stats_page`, 4096 rows) through the same value checks as a
   shard row (factor `checked_pair` out of `parse_pair`, and the "one pair, two statistics" / 2^18 bound placement out
   of `read_shard`; messages unchanged); an unreadable page = empty cache (a miss); no key row = today's shard read,
   then import in chunks of 4096 with the key row in the LAST chunk (a partial import re-imports next time).
   `store_marginal_pair_rows(cache, computed, index)` = one `put_pairs(key_sha, rows, &key_row)` (one BEGIN
   IMMEDIATE); returns rows written and whether the store was skipped. Verb: `MarginalIcConfig::pair_cache_index`
   (CacheIndexMode), `--pair-cache-index` parse + exit-2 refusals as the IC exe, run_marginal_ic bounded-config check,
   `PairCacheUse` holds the `std::optional<CacheIndex>`, `seed_pairs` gets `progress`; sqlite block = `{directory,
   index: "sqlite", key_sha256, rows_read, legacy_shards_read, cached_pairs, hits, computed, rows_written}` (+
   `store_skipped: true` only when skipped); files block byte-identical. Admission arithmetic untouched (Forbidden).
4. Task 6: `ingest_ledger.cpp` `default_ledger_head()` returns a lambda over `ledger::chain_head`
   (`LedgerHead{head.head_sha256, head.rule}`); new `verify_ledger_heads(Database&, span<const std::string>
   ledger_paths)` there (declared in `catalog/ingest.hpp`): prefix heads from `trial_line` rows by `ledger::prefix_heads`
   plus genesis, ledger_state lines / head / rule consistency, every non-null `cycle_verdict.ledger_head` and
   `wave_result.ledger_head` must be a prefix head of a named ledger; `store_cli.cpp` `cmd_verify`: `--ledger L`
   (already a value option of the parser; repeatable), allowed with or without `--pins`, exit 3 on any recorded head
   that is not a prefix head or an inconsistent index; usage line updated. One existing assertion must change:
   `atx-engine/tests/research/research_catalog_test.cpp:368` `EXPECT_FALSE(default_ledger_head())` ("B2 not bound at
   this base") becomes `EXPECT_TRUE` (the seam is bound now). The synthetic tree's recorded head
   `8b1e9fe0...f008` is the head of its 5-line ledger (checked with the chain rule), so `verify --ledger
   build-equity/trials.jsonl` must pass on it; a tree copy whose last ledger line is edited before cataloguing must
   exit 3.
5. Task 7 tests (brief names), then ONE Debug build per slice (`p9-sql4-a` ...; HOST-4 seventh priority, HOST-5: only
   builds under `C:\atx-wt\pool-*` count, the foreign `C:\atx\build-vol` build does not), named filters, the three
   tripwires (`StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`, `.IcSourcesPinnedToSemanticsVersion`,
   `MarginalIc.PairCacheSourcesPinned`: no pinned source file was edited), one teeth build with a TEETH-RESTORE tool,
   tiny-world flag-absent identity, report, `git merge feat/platform-p9-20261003`, hand back.

## Notes for the report / review

- Known limitation to report: the marginal verb reads candidate signals through sidecar FILES
  (`strategy_marginal_ic.cpp:338-400`, out of SQL4's scope), so a candidate cache used with `--candidate-cache-index
  sqlite` (no sidecar files) cannot feed the marginal verb: P10 item (or a follow-up flag).
- No new `atx.<name>/v<n>` literal; no Python file; no pinned source list or SHA touched; no recipe byte; flag absent
  = no index opened anywhere (every indexed branch is behind a non-null `CacheIndex*`).
- Lane tool: `sql4-logs/tools/subst.py` (exact count-checked substitutions keeping CRLF; the Bash heredoc layer
  mangles backslashes).
- Disk: no object tree was built (nothing to delete). The `build/` directory in the pool is the lease's failed `dev`
  configure (known; not this lane's; left alone). C: 51 GB free at the stop.
- Permission refusals: none.
