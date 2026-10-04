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

## Lane SQL4: IC cache indexes on SQLite behind explicit flags; the trial-ledger index head

**Pool / branch:** 18 (S2's warm tree, released after wave 2), `feat/p9-sql4-20261003`, run id `p9-sql4-20261003`,
heartbeat `p9-sql4-hb`. **Wave:** 3b (PQ-2): dispatched after D3 and C3 merge; base = that head (the post-wave-3
P9-B0 head when wave 3 is complete). **Effort:** L. **Serves:** infrastructure (IC warm-pass speed, cache hygiene).
**Merge slot:** 3b, alone, before the freeze's P9-B0. **Needs merged:** SQL1, SQL2, SQL3, S2 (identity tokens in the
caches), B2 (`research/ledger`), C3 (the atx-impl target split), D3 (owner of `strategy_ic_runner.cpp` /
`strategy_ic_detail.hpp` in wave 3). May slip to P10 without blocking the freeze (G-P10 is printed, ruling SQL-8).

**Goal.** The three IC caches keep their payload files and may replace their JSON index files with rows of the cache
tables (sql-design §3.7 group `cache`) **only when an explicit flag asks for it** (ruling SQL-6: the u / w summary and
`marginal_ic.json` write their cache state, so file presence is not allowed): `--candidate-cache-index sqlite` on the
IC exe, `--pair-cache-index sqlite` on the marginal verb. Flag absent = today's code path and bytes, whatever files
exist. With the flag, the output records the choice (`"index": "sqlite"` in the summary's `candidate_cache` block / the
`pair_cache` block) and the argv in the receipt records the flag. Read-through imports legacy entries. Second, the
catalog's ledger index gets its chain head from B2's C++ ledger library, never from a re-implementation (ruling SQL-7);
the JSONL ledger stays the authority (ruling SQL-9).

**Read:** `sql-design.md` §3.3, §3.5, §3.6, §3.7 (group `cache`), §3.10, §3.11, §6; `progress.md` rulings
SQL-1..SQL-9; `.agents/cpp/agent.md`; `atx-impl/src/strategy_ic_signal_cache.cpp` (all; the layout comment `:96-125`,
publish `:398-474`), `strategy_ic_result_cache.cpp` (all; `:56-75`), `strategy_marginal_pair_cache.{hpp,cpp}` (all),
`strategy_ic_runner.cpp` (the `candidate_cache` summary block `:583-596`, the `--candidate-cache` parse `:861`, which
thread calls `cache_load` / `cache_store`, admission slack), `strategy_marginal_ic.cpp` (the `pair_cache` block
`:1001-1048`, the `--pair-cache` parse `:1129`); D3's and C3's reports (their changes to those files); S2's report
(cache identity tokens, audit-exact cache); B2's `research/ledger` header (the chain-head function); SQL1's
`open_cache`, `ops_cache.hpp`, `rows_cache.hpp`; SQL2's `ingest_ledger.cpp`.

**Contracts.** Reads K-P9-13 (cache tables, policy, `cache init`, ruling SQL-6). Writes two argv flags and one optional
output key each (K-P9-13 addendum). Reads B2's ledger API. If a cache table needs a column, SQL4 owns SQL1's
`rows_cache.hpp` / `tables_cache.hpp` / `cache_ops.cpp` / `schema/cache.json` in wave 3b: add an optional column with
`since = 2`, bump the cache group version to 2 (an additive step from the descriptors), keep v1 files readable.

**Files in scope (owned in wave 3b).**
- Existing: `atx-impl/src/strategy_ic_signal_cache.cpp`, `atx-impl/src/strategy_ic_result_cache.cpp`,
  `atx-impl/src/strategy_marginal_pair_cache.{hpp,cpp}`, `atx-impl/src/strategy_marginal_ic.{hpp,cpp}` (the flag and
  the `pair_cache` key only), `atx-impl/src/strategy_ic_runner.cpp` (the flag parse and the `candidate_cache` key only),
  `atx-impl/src/strategy_ic_detail.hpp` (one config field).
- New: `atx-impl/src/strategy_ic_cache_index.{hpp,cpp}` (one index object per cache root: SQL1's `open_cache`, one
  connection guarded by a `std::mutex`, prepared statements through SQL1's `ops_cache.hpp`; documented lock order;
  never held across hashing, VM work or payload I/O), `atx-impl/tests/strategy_ic_cache_index_test.cpp` (+ one line
  appended to `atx-impl/tests/CMakeLists.txt`).
- SQL2's catalog files: `atx-engine/src/research/store/catalog/ingest_ledger.cpp`, the catalog CMake block (link B2's
  ledger library), `atx-engine/tests/research/research_catalog_ledger_test.cpp` (new).
- If needed: SQL1's cache-group files named under Contracts.
- `atx-impl/CMakeLists.txt`: one `target_link_libraries(<the target that compiles strategy_ic_signal_cache.cpp after
  C3's split> PRIVATE atx::engine-research-store)` line (C3 has merged; no cross-lane edit).

**Forbidden:** `strategy_ic_composition.*`, the marginal verb's arithmetic, every IC computation, the pinned source
lists and their SHA-256 (`dsl_vm_sources`, `ic_result_sources`, the pair-cache identity: an index is not a semantics
change and needs no bump), every recipe byte, any output byte when the flags are absent, any selection by file
presence, `vcpkg.json`, `CMakePresets.json`, every Python file (the spec keys for the flags belong to the file the PM
names under PQ-3).

**Cross-lane edits:** none (wave 3b has no other lane).

**Tasks, in order.**
1. **Flags.** IC exe: `--candidate-cache-index {files,sqlite}` (default `files` = today); refused without
   `--candidate-cache`; `sqlite` refused (exit 2, usage) when `<cache>/index.sqlite` is absent (created only by
   `atx-research-store cache init DIR`). The summary's `candidate_cache` block gains `"index": "sqlite"` only when the
   flag says `sqlite`; with `files` or no flag the block is byte-identical. Marginal verb: `--pair-cache-index
   {files,sqlite}` the same way; with `sqlite` the `pair_cache` block gains `"index": "sqlite"` and reports `rows_read`
   in place of `shards_read`.
2. **Index object** (`strategy_ic_cache_index`): `PRAGMA cache_size` fixed at 8 MiB and counted inside the runner's
   fixed admission slack (`strategy_ic_result_cache.cpp:73-75` states 32 MiB); a `SQLITE_BUSY` after the timeout or any
   I/O error on a read is a miss with a progress line; a write failure is a skipped store with a progress line,
   exactly where today's file cache would skip.
3. **Signal cache** (flag `sqlite`): a v2 entry = payload `.f64` published no-replace first (today's code), then one
   `ic_signal` row (the commit marker; the row holds today's sidecar document verbatim in `sidecar` and its key parts
   in columns); lookup by key SHA-256, then today's payload checks; a legacy sidecar (v1 or v2 file) on a miss is
   verified exactly as today and imported; no sidecar file is written in this mode; a row whose key parts differ from
   the request refuses as a sidecar mismatch does today.
4. **IC-result cache** (flag `sqlite`): the self-hashed record text of today's file (`ic<v>_<key16>/<id>.json`) stored
   unchanged in `ic_result.record`, keyed by the entry dir and id; read verifies the same self-hash; read-through
   import.
5. **Pair cache** (flag `sqlite`): shards become `pair_stat` rows under `pair_key`; open = every row of the key ordered
   by `(payload_lo, payload_hi)`; store = one `BEGIN IMMEDIATE` inserting the computed pairs, `ON CONFLICT DO NOTHING`
   then a check that a conflicting row carries the same bits (else Err, as two shards with different stats are today);
   the 2^18 pair bound and the admission bytes unchanged; legacy shards read through.
6. **Ledger index head:** `ingest_ledger.cpp` calls B2's chain-head function over the ledger's line texts and fills
   `ledger_state.head_sha256` / `head_rule`; `verify --ledger` (new sub-option) refuses (exit 3) when a head recorded in
   an ingested verdict or wave result is not the head of some prefix of the ledger (a tail edit). The index never
   decides N; the JSONL file stays the authority.
7. Tests (below), report.

**Tests (written after implementing).** `IcCacheIndex.FlagAbsentKeepsFileLayoutAndSummary` (an `index.sqlite`
present, no flag -> the same sidecar and payload files and summary bytes as before), `.FlagWithoutIndexRefused`,
`.RowIsTheCommitMarker` (payload without row = miss), `.ReadThroughImportsLegacySidecar`, `.KeyMismatchRefuses`,
`.TwoConnectionsInterleave`, `.SummaryRecordsIndex`; `IcResultIndex.RecordTextRoundTrip`, `.TamperedRecordIsMiss`;
`MarginalPairIndex.SameStatsAsShards`, `.ConflictingBitsRefused`, `.ReadsLegacyShards`, `.FlagAbsentBlockUnchanged`;
`ResearchCatalog.LedgerHeadFromResearchLedger`, `.RecordedHeadsArePrefixHeads`. The existing `StrategyIcRunner.*` and
`MarginalIc.*` suites, including the source tripwires `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`,
`StrategyIcRunner.IcSourcesPinnedToSemanticsVersion` and `MarginalIc.PairCacheSourcesPinned`, pass unchanged.

**Root verifies.** Build (equity-dev, then equity-rel for the IC exe): `-Targets "atx-equity-strategy-ic,
atx-impl-strategy-ic-tests,atx-research-store,atx-engine-research-catalog-tests"`; gtests
`atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*:MarginalIc.*:IcCacheIndex.*:IcResultIndex.*:
MarginalPairIndex.*` and `atx-engine-research-catalog-tests --gtest_filter=ResearchCatalog.*`; pytest
`atx-engine/tools`.

**Flag-absent identity (root procedure), 0 trials.** (1) No flag, with and without an `index.sqlite` in the cache
dir: X-5's u and w passes on the new build (Debug and Release) byte-identical to the pre-merge build's outputs (no
substitution list). (2) Flag `sqlite` on a fresh dir after `atx-research-store cache init <dir>`: X-5's u and w twice
(cold: every candidate a miss, rows written; warm: every candidate a hit from rows) -> outputs byte-identical to (1)
except the summary's `candidate_cache` block and the cache / timing-only files, listed before the run from R0-3's
precedent (w 10 / 12 + 2 timing / cache-only, `progress.md:57`); no sidecar JSON written in the fresh dir. (3)
Marginal: the Y-S pool-only marginal with `--pair-cache` and `--pair-cache-index sqlite` on an indexed dir, cold then
warm -> marginal rows byte-identical to the shard run, the `pair_cache` block aside. (4) `verify --ledger` on the real
catalog exits 0 and the head equals the one the last verdict records. Wall time of the warm u pass before / after
logged.

**SQLite dependency (ruling SQL-4):** none (vendored `atx_sqlite3` through `atx::core`); the one link line above is
the only CMake change outside SQL-owned blocks.

**Out of scope:** the fields reuse store (A-lanes; content-addressed reuse is P10), the record store (SQL1), payload
formats, any change to which entries hit or miss for a given key, the spec keys and resume status of the two flags
(PQ-3).
