# Plan amendment proposal: the SQL workstream (owner directive 2; rulings SQL-1..SQL-9)

Proposals only; the PM rules each block into `docs/plans/2026-10-03-p9-sprint-plan.md`. Design and evidence:
`sql-design.md` (revision 2); paste-ready briefs: `briefs/brief-SQL1.md` .. `brief-SQL4.md`. Every block below is the
exact text to insert, with its anchor. Revision 2 applies rulings SQL-4..SQL-9 (`progress.md` lines 203-208); the
former OQ-SQL list is closed by them.

## A. Rulings applied, and the three questions still open

| ruling | applied as |
|---|---|
| SQL-4 | no dependency line anywhere; SQL1 owns and fixes `atx-core/{include/atx/core,src}/db/sqlite.*` in place (defects W1-W8, sql-design §3.2, each with file:line in brief-SQL1) and owns `atx-engine/tools/record_store.py` |
| SQL-5 | compile-time C++ table descriptors and templates (DDL, bind, read, record digest, schema JSON); no generated file, no code-gen step, no Python generator; Python has one generic accessor driven by the `atx.store-schema/v1` document that `atx-research-store schema --json` prints (also stored in each store's `store_info`); compile cost bounded by three instantiating TUs plus one test TU (sql-design §3.6, §6.2) |
| SQL-6 | record store: file presence (consumer bytes identical), recorded in the receipt's `store` block (SQL3); IC signal / IC-result caches and the pair cache: explicit flags `--candidate-cache-index sqlite`, `--pair-cache-index sqlite` (their outputs record cache state) (sql-design §3.10) |
| SQL-7 | SQL2 base = SQL1's task-1 commit; `artifact.sha_source` in {`verified`, `declared`} and `pin_status` reports `declared` apart from `ok`; real-tree ingest is a root-only bounded run; B2's chain-head function; pools SQL1 = 23, SQL2 = 24 |
| SQL-8 | G-P10 is printed at the freeze with its evidence; it gates no phase exit and not the freeze (blocks B, L) |
| SQL-9 | the trial ledger stays hash-chained JSONL as the authority through P9; SQLite holds an index; permanence is the owner's P10 decision (block N) |

Open for the PM (proposed ruling texts):

| id | proposed ruling text |
|---|---|
| PQ-1 | Ruling: in wave 2 the record-store index exists and prints its selection to stderr (pinned through the receipt's `logs`), but only root's identity runs use it; real research cells use it only after SQL3's receipt `store` block merges -- SQL-6 requires the choice in the receipt and the runner is E2's in wave 2 -- cost if wrong: the record store's speed-up starts one wave later. |
| PQ-2 | Ruling: SQL4 is wave 3b, dispatched after D3 and C3 merge, and owns `strategy_ic_runner.cpp` / `strategy_ic_detail.hpp` (flag and summary key only), `strategy_marginal_ic.{cpp,hpp}` (flag and `pair_cache` key only) and the one link line in `atx-impl/CMakeLists.txt` -- the flags SQL-6 requires live in D3's files -- cost if wrong: SQL4 slips to P10, which G-P10 being print-only allows. |
| PQ-3 | Ruling: the spec keys that pass the two index flags into argv, and whether they are `REUSE_NEUTRAL` for resume, go to SQL3 in a file the PM names that D3 does not own in wave 3; failing that, root passes the flags by direct argv in identity runs until P10 -- `MARGINAL_SPEC_FLAGS` admits only integer flags today (`research_cycle.py@e1:248`) -- cost if wrong: the flags are root-only in P9. |

## B. §0.4 (new row after G-P9; print-only, ruling SQL-8)

| gate | measure | how it is checked |
|---|---|---|
| G-P10 artifact store (printed at the freeze with its evidence; gates nothing, ruling SQL-8) | (a) the catalog of the integration head indexes every pin of every v8 / P9 spec, template, wave manifest, receipt and fields manifest: `atx-research-store verify --pins` prints counts by kind and state (`ok`, `declared`, `stale`, `missing`, `unresolved`); (b) the render-identity checker re-renders every receipt, start receipt, stage receipt, binding, verdict, wave result and candidate from its rows: mismatches of N printed; (c) two catalogs built from scratch on one tree: both catalog digests printed; (d) caches: X-5 fit / card byte-identical with no index, cold and warm index; X-5 u / w byte-identical with and without `--candidate-cache-index sqlite` (summary block and cache / timing-only files ruled before the run); (e) every `atx.*/vN` record schema written by Python or C++ is registered in `atx-engine/schemas/research_store/classes.json` | root's store step at each P9-B0 (block K); SQL1-SQL4 tests; guard test `test_research_store_classes.py`; the freeze report prints (a)-(e) with what landed and what slipped to P10 |

## C. §0.5 (new row after DEC-20)

| id | decision | code evidence | literature / ledger evidence | where |
|---|---|---|---|---|
| DEC-21 | Artifacts: JSON stays the authority for every pinned or chained class through the freeze; a SQLite catalog indexes them (path, SHA-256 verified or declared, typed keys, pins) and proves byte regeneration; unpinned caches move to SQLite (record store by recorded file presence, IC caches by explicit flag); tables are compile-time C++ descriptors; human-authored inputs stay text in git through P9 (P10: the owner's call) | SQLite vendored and wrapped (`atx-core/CMakeLists.txt:1-27`, `atx/core/db/sqlite.hpp`); receipts / bindings written in text mode (CRLF) vs stage receipts / verdicts / ledger LF (`run_bounded_research.py@e1:434, 497`; `stage_chain.py@e1:273`); record store content-keyed and absent from fitter / card outputs (`record_store.py:1-13`; `fit_composition_weights.py:1293-1325`); IC summary and marginal output record cache state (`strategy_ic_runner.cpp:583-596`; `strategy_marginal_ic.cpp@s1:1001-1048`) | rulings SQL-1..SQL-9; sqlite.org wal / howtocorrupt / intern-v-extern-blob / stricttables | SQL1-SQL4 |

## D. §2.1 (four rows appended to the lane catalogue)

| lane | group | goal (one line) | serves | wave | pool | effort | needs |
|---|---|---|---|---|---|---|---|
| SQL1 | INFRA-SQL | SQLite store core: `atx/core/db` wrapper fixed in place + policy open; compile-time C++ table descriptors (DDL, bind / read, digest, schema JSON); `research/store` library (create, migrate, catalog digest); generic Python accessor; record-store cache on SQLite | I | 2 | 23 | L | wave-1 merged (post-P9-B0 head) |
| SQL2 | INFRA-SQL | artifact catalog + `atx-research-store` (seal-safe walk from pins, verified / declared SHA-256, ingest, pins, query, `schema --json`, `cache init`), render-identity checker, class registry guard | I, Sig | 2 | 24 | L | SQL1 task 1 (K-P9-13) |
| SQL3 | INFRA-SQL | dual-write: Python record writers ingest what they write into the catalog with a per-write render check; the receipt `store` block recording the catalog hook and cache-index selection | I | 3 | 17 | M | SQL2, E2 merged |
| SQL4 | INFRA-SQL | IC signal / IC-result / pair cache indexes on SQLite behind explicit flags (payload files stay); ledger index head through B2's `research/ledger` | I | 3b | 18 | L | SQL3, S2, B2, C3, D3 merged |

## E. §2.2 (rows appended to the owned-files table; the "wave 2-3" row gains the SQL entries)

| lane | owns (create / modify) |
|---|---|
| SQL1 | `atx-core/{include/atx/core,src}/db/sqlite.*`, `atx-core/tests/db_sqlite_test.cpp` (ruling SQL-4); new `atx-core/{include/atx/core,src}/db/connection.*`, `atx-core/tests/db_connection_test.cpp`; one source line in `atx-core/CMakeLists.txt` and one line in `atx-core/tests/CMakeLists.txt`; new `atx-engine/include/atx/engine/research/store/{store,digest,rows_core,rows_cache,ops_core,ops_cache}.hpp`, `atx-engine/src/research/store/{detail/table.hpp,detail/table_ops.hpp,tables_core.hpp,tables_cache.hpp,core_ops.cpp,cache_ops.cpp,store.cpp,digest.cpp}`; `atx-engine/tools/research_store.py` (new), `atx-engine/tools/record_store.py`, `test_record_store.py` (ruling SQL-4); new tests `atx-engine/tests/research/research_store_*`, fixtures `atx-engine/tests/fixtures/research_store/{schema/catalog_core.json,schema/cache.json,make_store.py,digest_oracle.py,golden_*.json,make_py_fixture.py,py_created.sqlite}`, pytests `atx-engine/tools/test_research_store*.py`, `test_record_store_sqlite.py`; one appended block each in `atx-engine/CMakeLists.txt` and `atx-engine/tests/CMakeLists.txt` |
| SQL2 | new `atx-engine/include/atx/engine/research/store/catalog/**`, `atx-engine/src/research/store/catalog/**` (incl. `tables_records.hpp`, `records_ops.cpp`), `atx-engine/schemas/research_store/classes.json`, `atx-engine/tools/research_store_identity.py`, tests `atx-engine/tests/research/research_catalog_*`, `atx-engine/tools/test_research_store_{identity,classes,blind}.py`, fixtures `atx-engine/tests/fixtures/research_store/{schema/catalog_records.json,make_store_tree.py,tree/**}`; one appended block each in `atx-engine/CMakeLists.txt` and `atx-engine/tests/CMakeLists.txt` |
| SQL3 (wave 3) | new `scripts/research_store_hook.py` + test; `scripts/run_bounded_research.py` (the `store` block), `atx-engine/tools/stage_chain.py`, `scripts/wave_context.py`, `scripts/cycle_resume.py`, `scripts/cycle_verdict.py`, `scripts/wave_stage_record.py`, `scripts/wave_queue.py` (or the `scripts/cycle/` file E2 moved a writer into, named by the PM) and their tests; under PQ-3 the file the PM names for the flag spec keys |
| SQL4 (wave 3b) | `atx-impl/src/strategy_ic_signal_cache.cpp`, `strategy_ic_result_cache.cpp`, `strategy_marginal_pair_cache.{cpp,hpp}`, `strategy_marginal_ic.{cpp,hpp}` (flag + `pair_cache` key), `strategy_ic_runner.cpp` (flag + `candidate_cache` key), `strategy_ic_detail.hpp` (one field), new `atx-impl/src/strategy_ic_cache_index.*` + test (one line in `atx-impl/tests/CMakeLists.txt`), one link line in `atx-impl/CMakeLists.txt`; SQL2's `ingest_ledger.cpp` and catalog CMake block; SQL1's cache-group files if a v2 is needed |

Text added under the table: "SQL lanes never touch `vcpkg.json`, `CMakePresets.json` or any dependency line (SQL-4),
`pch.hpp`, `atx-engine/include/atx/engine/store/**`, `wave_manifest.py`, `scripts/cycle/**` beyond a file the PM names
(E2 / D3), `backtest_integrity.py`, `research_ledger.py` (B2) or the fitter; no SQL lane adds a generated file or a
code-generation step (SQL-5)."

## F. §2.3 (new contract row)

| id | writer | readers | content |
|---|---|---|---|
| K-P9-13 artifact store schema | SQL1 (descriptor API and DDL grammar, policy, digests, schema JSON, groups `catalog_core` and `cache`), SQL2 (group `catalog_records`, the `atx-research-store` CLI, `classes.json`), SQL3 (receipt key `store`), SQL4 (two index flags and their output keys) | SQL2, SQL3, SQL4, root, PRE (pins report), D3 (cache tables, optional) | one `constexpr` descriptor per record type (`store::table<Row>(name, opts, store::col<Sql::T>(name, &Row::m, flags)...)`; nullability = `std::optional`; flags key / indexed / volatile; `since` per table and column) from which templates produce the DDL (exact grammar, sql-design §3.6), bind / read, the record digest and the schema; instantiated only in `core_ops.cpp`, `cache_ops.cpp`, `records_ops.cpp`; consumers see plain row structs and non-template operations; schema document `atx.store-schema/v1` printed by `atx-research-store schema --json --db catalog\|cache` and stored in `store_info('schema_json')`, read by one generic Python accessor (`research_store.py`); catalog `build-equity/research-store/catalog.sqlite` (`application_id` 0x41545843, `user_version` 1), every cache index `<dir>/index.sqlite` (0x4154584B, 1), created and migrated only by C++; WAL, `BEGIN IMMEDIATE`, busy 30 s, `synchronous` FULL (catalog) / NORMAL (caches), `page_size` 8192 before WAL, STRICT + WITHOUT ROWID tables with natural keys, every read ordered by key; `artifact.sha_source` `verified` (hashed by the catalog) or `declared` (stated by a manifest, file not opened); record digest `atx.record-digest/v1` (typed TLV, reals as IEEE bit hex, volatile columns excluded) and catalog digest `atx.catalog-digest/v1`; values <= 1 MiB inline, payloads always files by SHA-256; typed columns hold no return / IC / Sharpe / turnover / NAV statistic; receipt `store` block `atx.run-store/v1` (absent when no catalog and no index); flags `--candidate-cache-index`, `--pair-cache-index` `{files,sqlite}` (default `files`); CLI `atx-research-store {init, catalog, ingest, verify, query, digest, dump, cache init, cache prune, schema, quick-check}`, exit 0 / 2 / 3 / 4 |

## G. §2.4 (new lane block after INFRA-T)

**INFRA-SQL: the artifact store (owner directive 2; rulings SQL-1..SQL-9, DEC-21)**

- **SQL1 store core (wave 2, pool-23).** Inputs: sql-design §0, §3; the vendored SQLite and the `atx/core/db` wrapper.
  Deliverables: wrapper defects fixed in place and a policy open in atx-core; compile-time table descriptors and their
  templates; the `research/store` library (create, migrate, record and catalog digests, schema JSON); the generic
  Python accessor; the record store (`record_store.py`) on SQLite when `index.sqlite` exists in its root or parent,
  with read-through of legacy JSON and a stderr selection line. Task 1 (descriptor machinery, core and cache
  descriptors, schema fixtures) is committed first as SQL2's base. Root: builds `atx-core-tests`,
  `atx-engine-research-store`, `atx-engine-research-store-tests`; logs the compile seconds of the instantiating TUs.
  Trials 0. Merge slot: wave 2, after the eight planned lanes.
- **SQL2 catalog (wave 2, pool-24).** Inputs: SQL1's task-1 commit; sql-design §1, §3.7, §3.9. Deliverables:
  `atx-research-store` (walk from pins, seal guard, verified / declared SHA-256, ingest of receipts, stage receipts,
  bindings, verdicts, wave results, specs, candidates, ledger lines, fields manifests and build receipts; pins and
  their status; query, dump, digest; `schema --json`; `cache init` / `prune`); the Python render-identity checker;
  `classes.json` and its guard test. Root: builds the catalog targets; fixture chain 0 mismatches; real tree as a
  root-only bounded run: pins report, render mismatches, a reproducible catalog digest; SQL1's X-5 fit and card
  identity with no index, cold and warm index. Trials 0. Merge slot: directly after SQL1.
- **SQL3 writer dual-write (wave 3, pool 17).** Inputs: SQL2 merged; E2's split. Deliverables: one hook call after
  each Python write of a receipt, start receipt, stage receipt, binding, verdict, wave result and candidate; the
  bounded-run receipt's `store` block (catalog hook and cache-index selection, absent when neither exists). Root: E1's
  tiny-world fake-wave identity without a catalog (byte-identical) and with one (the `store` block and the digests over
  it as the only substitutions); 0 render mismatches. Trials 0. Merge slot 6 of wave 3, never under an in-flight wave.
- **SQL4 cache indexes (wave 3b, pool 18).** Inputs: D3 and C3 merged; S2's cache identity tokens; B2's ledger
  library. Deliverables: IC signal, IC-result and pair caches indexed in SQLite only under `--candidate-cache-index
  sqlite` / `--pair-cache-index sqlite` (payload files unchanged, read-through; the output records the index); the
  ledger index head from `research/ledger`. Root: X-5 u / w with no flag byte-identical (index file present or not);
  with the flag cold and warm byte-identical apart from the summary block and the ruled cache / timing-only files;
  marginal rows identical; `verify --ledger` exit 0. Trials 0. Merge: 3b, alone, before the freeze's P9-B0; may slip to
  P10.

## H. §3.1 graph (lines added to the mermaid block)

```
  subgraph W2[wave 2]
    SQ1[SQL1 store core]
    SQ2[SQL2 catalog]
  end
  M1 --> SQ1
  SQ1 -. K-P9-13 task 1 .-> SQ2
  SQ1 --> M2
  SQ2 --> M2
  subgraph W3[wave 3]
    SQ3[SQL3 dual-write]
  end
  M2 --> SQ3
  SQ3 --> M3
  subgraph W3B[wave 3b]
    SQ4[SQL4 cache indexes]
  end
  M3 --> SQ4
  C3 --> SQ4
  D3 --> SQ4
```

## I. §3.2 (rows added; the merge-slot cells amended)

| node | hard dependencies | soft (contract-first, may code in parallel) | merge slot |
|---|---|---|---|
| SQL1 | wave-1 merges + P9-B0 | - | wave 2, after AL-SIG (and T2 / COV as the PM orders) |
| SQL2 | SQL1 task 1 committed (ruling SQL-7) | SQL1 (K-P9-13 API) | wave 2, directly after SQL1 (rebased on it) |
| SQL3 | SQL2, E2 merged | - | wave 3, slot 6 |
| SQL4 | SQL3, S2, B2, C3, D3 merged | B2 (chain-head function, ruling SQL-7) | wave 3b, alone (PQ-2) |

Amended cells: wave-2 slot list "E2 1, A3 2, S2 3, B2 4, D2 5, C2 6, AL-COMB 7, AL-SIG 8" becomes "E2 1, A3 2, S2 3,
B2 4, D2 5, C2 6, AL-COMB 7, AL-SIG 8, SQL1 9, SQL2 10" (T2 / COV slots as ruled); wave-3 list "C3 1, D3 2, AL-CLOCK 3,
AL-DATA 4, A4 5" becomes "C3 1, D3 2, AL-CLOCK 3, AL-DATA 4, A4 5, SQL3 6"; a wave-3b list "SQL4" is added (AL-CLOCK's
3b placement as ruled is unchanged).

## J. §3.3 (cells amended)

| wave | lanes (pool) | starts | ends |
|---|---|---|---|
| 2 | ... AL-SIG (13), T2 (21), **SQL1 (23), SQL2 (24; from SQL1's task-1 commit)** | unchanged | unchanged; SQL1 / SQL2 move no output byte, so they add nothing to P9-B0 |
| 3 | ... A4 (12, stretch); AL-CLOCK (19) as 3b after C3; **SQL3 (17); SQL4 (18) as 3b after C3 and D3** | unchanged | unchanged; SQL4's identity runs join the wave-3 identity set when it lands before the freeze |

Critical path: unchanged. No SQL lane is on root's serial chain except its merge builds (SQL1, SQL2: one build each of
small new targets plus the root-only real-tree catalog run; SQL4: the IC exe, Debug and Release). SQL4 slipping to P10
changes no gate (ruling SQL-8).

## K. §4.2 (row added to the per-wave checks)

| check | content |
|---|---|
| store | from wave 2 on: root runs `atx-research-store catalog` on the integration head as a root-only bounded run, logs the catalog digest, `verify --pins` counts by state (`verified` / `declared` apart) and the render-identity mismatch count; a second from-scratch catalog reproduces the digest. Evidence for the G-P10 print; never a merge block by itself |

## L. §4.3 and §4.4 (text amended)

- Phase 2 and phase 3 exit gates: unchanged (G-P10 is not added to either; ruling SQL-8).
- §4.4 item 3 stays "Platform: G-P1..G-P9 all ticked"; a new item: "G-P10 printed with its evidence ((a)-(e) of §0.4,
  each marked landed / partial / P10); it does not gate the freeze (ruling SQL-8)."

## M. §7.1 (risk rows added)

| risk | likelihood | effect | control |
|---|---|---|---|
| Two writers on one SQLite file on Windows (parallel phases, two IC exes on one cache) | medium | a busy error drops a cache entry or delays a hook | WAL, `BEGIN IMMEDIATE`, 30 s busy timeout, short transactions; caches treat failure as a miss; hooks only warn |
| The catalog opens a sealed file | low | a seal breach | roots from pins only, year-token backstop, payload SHA-256s recorded as `declared`, `field-source` pins never followed (SQL-7); fixture test with a decoy dir |
| A store change moves a pinned byte | low | voided lineage evidence | SQL-2: JSON stays authority; record store by recorded presence, IC caches by explicit flag (SQL-6); every SQL lane's identity is byte-for-byte with and without the store |
| Descriptor templates fail root's first build or compile slowly (SQL-5) | medium | a fix round; slower store TUs | three instantiating TUs plus one test TU; consumers see plain rows only; nothing in the PCH; root logs compile seconds, > 60 s per TU is a PM decision |
| Token cost of four more lanes | high | budget | SQL4 (then SQL3) moves to P10 first; G-P10 is print-only (SQL-8) |

## N. Deferred to P10 (new list under Appendix B or §6)

- Stage 3 (JSON retired) for every pinned or chained class (receipts, start receipts, stage receipts, bindings,
  verdicts, wave results, candidates), after one full wave of dual-write with 0 mismatches, every reader reading
  through the store, and one ruled re-pin of pins as record digests; `VACUUM INTO` snapshots and an
  `integrity_check` gate for authority classes.
- The owner's decision whether the hash-chained JSONL stays the trial ledger's authority beyond P9 or the SQLite index
  takes a larger role (ruling SQL-9); likewise whether human-authored inputs stay text in git.
- Dual-write by the C++ writers (NAV, IC, fitter, fields manifests, factors verb) and by `nav_summ` / readers.
- The bounded runner, stage chain and receipt writer in C++ over the store; the scoreboard, pin checks and
  `research_gc` reading the store.
- Content-addressed payload storage for fields reuse (hard links instead of copies).
- A C++ record-cache API with Python-compatible canonical JSON (when a C++ fit needs it); RFC 8785 canonical JSON for
  JSON columns with two writer languages.
- Porting the alpha-lifecycle store (`atx/engine/store/`) onto the descriptors.
- SQL4 if it slips; the spec keys of the index flags if PQ-3 finds no owner.
- Owners for the modules no P9 lane owns (`sql-design.md` §2): roles and mine drivers, the card, monitor and
  diagnostics readers, `holdout_gate`, `horizon_stats` deletion, role and data builders, audits, library generators.
